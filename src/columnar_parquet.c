/*-------------------------------------------------------------------------
 *
 * columnar_parquet.c
 *		Parquet file export for pgColumnar (gap 27, piece 2).
 *
 *		columnar.export_parquet(rel regclass, path text) writes a columnar table
 *		to a Parquet file. The writer is self-contained -- it emits the Thrift
 *		compact-protocol metadata and PLAIN-encoded, UNCOMPRESSED data pages
 *		directly -- so there is no libparquet build or run-time dependency. Rows
 *		are read in physical order via the scalar reader; one row group is
 *		emitted per PARQUET_ROWGROUP_ROWS rows, with one DATA_PAGE per column.
 *
 *		First-slice type mapping (matches columnar.export_arrow): int2/int4 ->
 *		INT32 (int2 tagged INT_16), int8 -> INT64, float4 -> FLOAT, float8 ->
 *		DOUBLE, bool -> BOOLEAN, text/varchar -> BYTE_ARRAY (UTF8), bytea ->
 *		BYTE_ARRAY. All columns are OPTIONAL; nulls are carried in definition
 *		levels. Little-endian hosts only.
 *
 * Independent MIT implementation built from the Apache Parquet format and
 * Thrift compact-protocol specifications and the public PostgreSQL API only.
 *
 *-------------------------------------------------------------------------
 */
#include "columnar.h"

#include "fmgr.h"
#include "access/relation.h"
#include "access/table.h"
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

PG_FUNCTION_INFO_V1(columnar_export_parquet);

#define PARQUET_ROWGROUP_ROWS 65536

/* Parquet physical types */
#define PQ_BOOLEAN		0
#define PQ_INT32		1
#define PQ_INT64		2
#define PQ_FLOAT		4
#define PQ_DOUBLE		5
#define PQ_BYTE_ARRAY	6
/* ConvertedType */
#define PQ_CT_UTF8		0
#define PQ_CT_INT_16	16
/* Encoding */
#define PQ_ENC_PLAIN	0
#define PQ_ENC_RLE		3
/* Thrift compact field types */
#define TC_STOP			0
#define TC_BOOL_TRUE	1
#define TC_BOOL_FALSE	2
#define TC_I32			5
#define TC_I64			6
#define TC_BINARY		8
#define TC_LIST			9
#define TC_STRUCT		12

typedef enum ParquetKind
{
	P_INT16,
	P_INT32,
	P_INT64,
	P_FLOAT,
	P_DOUBLE,
	P_BOOL,
	P_UTF8,
	P_BINARY
}			ParquetKind;

/* ---- Thrift compact-protocol writer (into a StringInfo) ---- */

static void
tc_varint(StringInfo b, uint64 v)
{
	while (v >= 0x80)
	{
		appendStringInfoChar(b, (char) ((v & 0x7f) | 0x80));
		v >>= 7;
	}
	appendStringInfoChar(b, (char) v);
}

static void
tc_zigzag32(StringInfo b, int32 v)
{
	tc_varint(b, (uint32) ((v << 1) ^ (v >> 31)));
}

static void
tc_zigzag64(StringInfo b, int64 v)
{
	tc_varint(b, (uint64) ((v << 1) ^ (v >> 63)));
}

/* field header with delta-encoded id */
static void
tc_field(StringInfo b, int16 *lastId, int16 id, int type)
{
	int			delta = id - *lastId;

	if (delta > 0 && delta <= 15)
		appendStringInfoChar(b, (char) ((delta << 4) | type));
	else
	{
		appendStringInfoChar(b, (char) type);
		tc_zigzag32(b, id);
	}
	*lastId = id;
}

static void
tc_i32_field(StringInfo b, int16 *lastId, int16 id, int32 v)
{
	tc_field(b, lastId, id, TC_I32);
	tc_zigzag32(b, v);
}

static void
tc_i64_field(StringInfo b, int16 *lastId, int16 id, int64 v)
{
	tc_field(b, lastId, id, TC_I64);
	tc_zigzag64(b, v);
}

static void
tc_string_field(StringInfo b, int16 *lastId, int16 id, const char *s, int len)
{
	tc_field(b, lastId, id, TC_BINARY);
	tc_varint(b, (uint64) len);
	if (len > 0)
		appendBinaryStringInfo(b, s, len);
}

/* list header; caller then appends the elements */
static void
tc_list_header(StringInfo b, int size, int elemType)
{
	if (size < 15)
		appendStringInfoChar(b, (char) ((size << 4) | elemType));
	else
	{
		appendStringInfoChar(b, (char) (0xF0 | elemType));
		tc_varint(b, (uint64) size);
	}
}

static void
tc_stop(StringInfo b)
{
	appendStringInfoChar(b, (char) TC_STOP);
}

/* ---- per-column accumulator for one row group ---- */
typedef struct PqCol
{
	char	   *name;
	ParquetKind kind;
	int			ptype;			/* Parquet physical type */
	int			convType;		/* ConvertedType, or -1 */
	StringInfoData present;		/* 1 byte per row: 1 present, 0 null */
	StringInfoData values;		/* PLAIN values, non-null only */
	StringInfoData boolbits;	/* 1 byte per non-null bool value */
	int64		numValues;		/* rows in the current group (incl nulls) */
}			PqCol;

typedef struct PqColMeta
{
	int64		dataPageOffset;
	int64		totalSize;		/* page header + body */
	int64		numValues;		/* rows */
}			PqColMeta;

typedef struct PqRowGroup
{
	PqColMeta  *cols;
	int64		totalByteSize;
	int64		numRows;
}			PqRowGroup;

static void
pqcol_reset(PqCol *c)
{
	resetStringInfo(&c->present);
	resetStringInfo(&c->values);
	resetStringInfo(&c->boolbits);
	c->numValues = 0;
}

static ParquetKind
parquet_kind_for_type(Oid typid, int *ptype, int *convType)
{
	*convType = -1;
	switch (typid)
	{
		case INT2OID:
			*ptype = PQ_INT32;
			*convType = PQ_CT_INT_16;
			return P_INT16;
		case INT4OID:
			*ptype = PQ_INT32;
			return P_INT32;
		case INT8OID:
			*ptype = PQ_INT64;
			return P_INT64;
		case FLOAT4OID:
			*ptype = PQ_FLOAT;
			return P_FLOAT;
		case FLOAT8OID:
			*ptype = PQ_DOUBLE;
			return P_DOUBLE;
		case BOOLOID:
			*ptype = PQ_BOOLEAN;
			return P_BOOL;
		case TEXTOID:
		case VARCHAROID:
			*ptype = PQ_BYTE_ARRAY;
			*convType = PQ_CT_UTF8;
			return P_UTF8;
		case BYTEAOID:
			*ptype = PQ_BYTE_ARRAY;
			return P_BINARY;
		default:
			*ptype = -1;
			return P_INT32;
	}
}

static void
pqcol_append(PqCol *c, Datum d, bool isnull)
{
	c->numValues++;
	appendStringInfoChar(&c->present, isnull ? 0 : 1);
	if (isnull)
		return;

	switch (c->kind)
	{
		case P_INT16:
			{
				int32		v = (int32) DatumGetInt16(d);

				appendBinaryStringInfo(&c->values, (char *) &v, 4);
				break;
			}
		case P_INT32:
			{
				int32		v = DatumGetInt32(d);

				appendBinaryStringInfo(&c->values, (char *) &v, 4);
				break;
			}
		case P_INT64:
			{
				int64		v = DatumGetInt64(d);

				appendBinaryStringInfo(&c->values, (char *) &v, 8);
				break;
			}
		case P_FLOAT:
			{
				float4		v = DatumGetFloat4(d);

				appendBinaryStringInfo(&c->values, (char *) &v, 4);
				break;
			}
		case P_DOUBLE:
			{
				float8		v = DatumGetFloat8(d);

				appendBinaryStringInfo(&c->values, (char *) &v, 8);
				break;
			}
		case P_BOOL:
			appendStringInfoChar(&c->boolbits, DatumGetBool(d) ? 1 : 0);
			break;
		case P_UTF8:
		case P_BINARY:
			{
				struct varlena *v = PG_DETOAST_DATUM_PACKED(d);
				int32		len = VARSIZE_ANY_EXHDR(v);

				appendBinaryStringInfo(&c->values, (char *) &len, 4);
				if (len > 0)
					appendBinaryStringInfo(&c->values, VARDATA_ANY(v), len);
				break;
			}
	}
}

/* build the RLE/bit-packed hybrid definition-level section (bit width 1),
 * prefixed with its int32 LE byte length (DataPage v1 convention) */
static void
build_def_levels(StringInfo out, const char *present, int64 nrows)
{
	StringInfoData h;
	int64		ngroups = (nrows + 7) / 8;
	int64		g;
	int32		len;

	initStringInfo(&h);
	/* one bit-packed run: header = (ngroups << 1) | 1 */
	tc_varint(&h, (uint64) ((ngroups << 1) | 1));
	for (g = 0; g < ngroups; g++)
	{
		uint8		byte = 0;
		int			b;

		for (b = 0; b < 8; b++)
		{
			int64		idx = g * 8 + b;

			if (idx < nrows && present[idx])
				byte |= (1 << b);
		}
		appendStringInfoChar(&h, (char) byte);
	}
	len = h.len;
	appendBinaryStringInfo(out, (char *) &len, 4);
	appendBinaryStringInfo(out, h.data, h.len);
	pfree(h.data);
}

/* assemble the PLAIN values buffer for a column (from its accumulators) */
static void
build_values(StringInfo out, PqCol *c)
{
	if (c->kind == P_BOOL)
	{
		int64		n = c->boolbits.len;
		int64		nbytes = (n + 7) / 8;
		char	   *bits = palloc0(nbytes);
		int64		i;

		for (i = 0; i < n; i++)
			if (c->boolbits.data[i])
				bits[i >> 3] |= (1 << (i & 7));
		appendBinaryStringInfo(out, bits, nbytes);
		pfree(bits);
	}
	else
		appendBinaryStringInfo(out, c->values.data, c->values.len);
}

/* write a DATA_PAGE PageHeader (Thrift) for a page of body_size bytes */
static void
write_page_header(StringInfo out, int64 nrows, int32 body_size)
{
	int16		last = 0;
	int16		dlast = 0;

	/* PageHeader */
	tc_i32_field(out, &last, 1, 0);			/* type = DATA_PAGE */
	tc_i32_field(out, &last, 2, body_size); /* uncompressed_page_size */
	tc_i32_field(out, &last, 3, body_size); /* compressed_page_size */
	/* field 5: data_page_header (struct) */
	tc_field(out, &last, 5, TC_STRUCT);
	tc_i32_field(out, &dlast, 1, (int32) nrows); /* num_values */
	tc_i32_field(out, &dlast, 2, PQ_ENC_PLAIN);	/* encoding */
	tc_i32_field(out, &dlast, 3, PQ_ENC_RLE);	/* def level encoding */
	tc_i32_field(out, &dlast, 4, PQ_ENC_RLE);	/* rep level encoding */
	tc_stop(out);								/* end data_page_header */
	tc_stop(out);								/* end PageHeader */
}

/* ---- FileMetaData footer ---- */
static void
write_schema_element_root(StringInfo b, int ncols)
{
	int16		last = 0;

	tc_string_field(b, &last, 4, "schema", 6);	/* name */
	tc_i32_field(b, &last, 5, ncols);			/* num_children */
	tc_stop(b);
}

static void
write_schema_element_col(StringInfo b, PqCol *c)
{
	int16		last = 0;

	tc_i32_field(b, &last, 1, c->ptype);	/* type */
	tc_i32_field(b, &last, 3, 1);			/* repetition_type = OPTIONAL */
	tc_string_field(b, &last, 4, c->name, (int) strlen(c->name));
	if (c->convType >= 0)
		tc_i32_field(b, &last, 6, c->convType); /* converted_type */
	tc_stop(b);
}

static void
write_column_chunk(StringInfo b, PqCol *c, PqColMeta *m)
{
	int16		last = 0;
	int16		mlast = 0;

	/* ColumnChunk.file_offset (2) */
	tc_i64_field(b, &last, 2, m->dataPageOffset);
	/* ColumnChunk.meta_data (3) = ColumnMetaData struct */
	tc_field(b, &last, 3, TC_STRUCT);
	tc_i32_field(b, &mlast, 1, c->ptype);	/* type */
	/* encodings list [PLAIN, RLE] (2) */
	tc_field(b, &mlast, 2, TC_LIST);
	tc_list_header(b, 2, TC_I32);
	tc_zigzag32(b, PQ_ENC_PLAIN);
	tc_zigzag32(b, PQ_ENC_RLE);
	/* path_in_schema list [name] (3) */
	tc_field(b, &mlast, 3, TC_LIST);
	tc_list_header(b, 1, TC_BINARY);
	tc_varint(b, (uint64) strlen(c->name));
	appendBinaryStringInfo(b, c->name, strlen(c->name));
	tc_i32_field(b, &mlast, 4, 0);			/* codec = UNCOMPRESSED */
	tc_i64_field(b, &mlast, 5, m->numValues);	/* num_values */
	tc_i64_field(b, &mlast, 6, m->totalSize);	/* total_uncompressed_size */
	tc_i64_field(b, &mlast, 7, m->totalSize);	/* total_compressed_size */
	tc_i64_field(b, &mlast, 9, m->dataPageOffset);	/* data_page_offset */
	tc_stop(b);								/* end ColumnMetaData */
	tc_stop(b);								/* end ColumnChunk */
}

static void
write_row_group(StringInfo b, PqCol *cols, int ncols, PqRowGroup *rg)
{
	int16		last = 0;
	int			i;

	/* columns list (1) */
	tc_field(b, &last, 1, TC_LIST);
	tc_list_header(b, ncols, TC_STRUCT);
	for (i = 0; i < ncols; i++)
		write_column_chunk(b, &cols[i], &rg->cols[i]);
	tc_i64_field(b, &last, 2, rg->totalByteSize);	/* total_byte_size */
	tc_i64_field(b, &last, 3, rg->numRows);			/* num_rows */
	tc_stop(b);
}

/*
 * columnar_export_parquet
 *		SQL: columnar.export_parquet(rel regclass, path text) -> bigint.
 */
Datum
columnar_export_parquet(PG_FUNCTION_ARGS)
{
	Oid			relid;
	char	   *path;
	Relation	rel;
	TupleDesc	tupdesc;
	int			ncols;
	PqCol	   *cols;
	Snapshot	snapshot;
	ColumnarReadState *readState;
	Datum	   *values;
	bool	   *nulls;
	uint64		rowNumber;
	int64		total = 0;
	int64		groupRows = 0;
	int64		offset = 0;
	FILE	   *f;
	int			i;
	PqRowGroup *rgs = NULL;
	int			nrgs = 0;
	int			rgCap = 0;
	MemoryContext rgCtx = CurrentMemoryContext;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("relation and path must not be null")));
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to export to a server-side file")));

	relid = PG_GETARG_OID(0);
	path = text_to_cstring(PG_GETARG_TEXT_PP(1));

	rel = table_open(relid, AccessShareLock);
	if (!ColumnarIsColumnarRelation(relid))
	{
		table_close(rel, AccessShareLock);
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" is not a columnar table",
						RelationGetRelationName(rel))));
	}

	tupdesc = RelationGetDescr(rel);
	ncols = tupdesc->natts;

	cols = palloc0(sizeof(PqCol) * ncols);
	for (i = 0; i < ncols; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);
		int			ptype,
					convType;
		ParquetKind kind;

		if (att->attisdropped)
		{
			table_close(rel, AccessShareLock);
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("columnar.export_parquet does not support dropped columns")));
		}
		kind = parquet_kind_for_type(att->atttypid, &ptype, &convType);
		if (ptype < 0)
		{
			table_close(rel, AccessShareLock);
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("column \"%s\" has type %s, which columnar.export_parquet does not support",
							NameStr(att->attname),
							format_type_be(att->atttypid))));
		}
		cols[i].name = pstrdup(NameStr(att->attname));
		cols[i].kind = kind;
		cols[i].ptype = ptype;
		cols[i].convType = convType;
		initStringInfo(&cols[i].present);
		initStringInfo(&cols[i].values);
		initStringInfo(&cols[i].boolbits);
	}

	f = AllocateFile(path, PG_BINARY_W);
	if (f == NULL)
	{
		table_close(rel, AccessShareLock);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\" for writing: %m", path)));
	}

	fwrite("PAR1", 1, 4, f);		/* magic header */
	offset = 4;

	values = palloc(sizeof(Datum) * ncols);
	nulls = palloc(sizeof(bool) * ncols);

	snapshot = ActiveSnapshotSet() ? GetActiveSnapshot() : GetTransactionSnapshot();
	readState = ColumnarBeginRead(rel, snapshot, NULL, NULL, 0, NULL);

	for (;;)
	{
		bool		got = ColumnarReadNextRow(readState, values, nulls, &rowNumber);

		if (got)
		{
			CHECK_FOR_INTERRUPTS();
			for (i = 0; i < ncols; i++)
				pqcol_append(&cols[i], values[i], nulls[i]);
			groupRows++;
			total++;
		}

		/* flush a row group when full, or at end if it holds rows */
		if ((groupRows == PARQUET_ROWGROUP_ROWS) || (!got && groupRows > 0))
		{
			PqRowGroup *rg;

			if (nrgs == rgCap)
			{
				rgCap = rgCap ? rgCap * 2 : 8;
				rgs = rgs ? repalloc(rgs, sizeof(PqRowGroup) * rgCap)
					: palloc(sizeof(PqRowGroup) * rgCap);
			}
			rg = &rgs[nrgs++];
			rg->cols = MemoryContextAlloc(rgCtx, sizeof(PqColMeta) * ncols);
			rg->numRows = groupRows;
			rg->totalByteSize = 0;

			for (i = 0; i < ncols; i++)
			{
				StringInfoData body;
				StringInfoData ph;
				int64		pageStart = offset;

				initStringInfo(&body);
				build_def_levels(&body, cols[i].present.data, groupRows);
				build_values(&body, &cols[i]);

				initStringInfo(&ph);
				write_page_header(&ph, groupRows, (int32) body.len);

				fwrite(ph.data, 1, ph.len, f);
				fwrite(body.data, 1, body.len, f);
				offset += ph.len + body.len;

				rg->cols[i].dataPageOffset = pageStart;
				rg->cols[i].totalSize = ph.len + body.len;
				rg->cols[i].numValues = groupRows;
				rg->totalByteSize += ph.len + body.len;

				pfree(body.data);
				pfree(ph.data);
				pqcol_reset(&cols[i]);
			}
			groupRows = 0;
		}

		if (!got)
			break;
	}
	ColumnarEndRead(readState);

	/* ---- FileMetaData footer ---- */
	{
		StringInfoData fmd;
		int16		last = 0;
		int32		footerLen;

		initStringInfo(&fmd);
		tc_i32_field(&fmd, &last, 1, 1);	/* version */
		/* schema list (2): root + one per column */
		tc_field(&fmd, &last, 2, TC_LIST);
		tc_list_header(&fmd, ncols + 1, TC_STRUCT);
		write_schema_element_root(&fmd, ncols);
		for (i = 0; i < ncols; i++)
			write_schema_element_col(&fmd, &cols[i]);
		tc_i64_field(&fmd, &last, 3, total);	/* num_rows */
		/* row_groups list (4) */
		tc_field(&fmd, &last, 4, TC_LIST);
		tc_list_header(&fmd, nrgs, TC_STRUCT);
		for (i = 0; i < nrgs; i++)
			write_row_group(&fmd, cols, ncols, &rgs[i]);
		tc_string_field(&fmd, &last, 6, "pgColumnar", 10);	/* created_by */
		tc_stop(&fmd);

		fwrite(fmd.data, 1, fmd.len, f);
		footerLen = fmd.len;
		fwrite(&footerLen, 4, 1, f);	/* footer length, LE */
		fwrite("PAR1", 1, 4, f);		/* magic footer */
		pfree(fmd.data);
	}

	if (FreeFile(f) != 0)
	{
		table_close(rel, AccessShareLock);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m", path)));
	}

	table_close(rel, AccessShareLock);
	PG_RETURN_INT64(total);
}
