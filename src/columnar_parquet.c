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
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/array.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"
#include "utils/typcache.h"
#include "utils/uuid.h"

PG_FUNCTION_INFO_V1(columnar_export_parquet);

#define PARQUET_ROWGROUP_ROWS 65536

/* PostgreSQL epoch (2000-01-01) to Unix epoch (1970-01-01) offsets */
#define PG_TO_UNIX_DAYS		((int64) (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE))
#define PG_TO_UNIX_USECS	(PG_TO_UNIX_DAYS * USECS_PER_DAY)

/* Parquet physical types */
#define PQ_BOOLEAN		0
#define PQ_INT32		1
#define PQ_INT64		2
#define PQ_FLOAT		4
#define PQ_DOUBLE		5
#define PQ_BYTE_ARRAY	6
#define PQ_FIXED_LEN_BYTE_ARRAY 7
/* ConvertedType */
#define PQ_CT_UTF8		0
#define PQ_CT_DECIMAL	5
#define PQ_CT_DATE		6
#define PQ_CT_TIME_MICROS 8
#define PQ_CT_TIMESTAMP_MICROS 10
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
	P_BINARY,
	P_DATE,						/* date -> INT32 (days from Unix epoch) */
	P_TIME,						/* time -> INT64 TIME_MICROS */
	P_TIMESTAMP,				/* timestamp/tz -> INT64 TIMESTAMP_MICROS */
	P_UUID,						/* uuid -> FIXED_LEN_BYTE_ARRAY(16) */
	P_DECIMAL					/* numeric(p,s) -> FIXED_LEN_BYTE_ARRAY(16) */
}			ParquetKind;

/* Parse a numeric value (via its text form) into a 128-bit unscaled integer at
 * the given scale. Returns false for NaN/Infinity, which a decimal cannot hold. */
static bool
numeric_to_int128(Datum numd, int scale, __int128 *out)
{
	char	   *s = DatumGetCString(DirectFunctionCall1(numeric_out, numd));
	char	   *p = s;
	bool		neg = false;
	bool		seenDot = false;
	int			fracDigits = 0;
	__int128	acc = 0;

	if (*p == '-')
	{
		neg = true;
		p++;
	}
	else if (*p == '+')
		p++;

	for (; *p; p++)
	{
		if (*p == '.')
		{
			seenDot = true;
			continue;
		}
		if (*p < '0' || *p > '9')	/* NaN, Infinity: not representable */
		{
			pfree(s);
			return false;
		}
		acc = acc * 10 + (*p - '0');
		if (seenDot)
			fracDigits++;
	}
	while (fracDigits < scale)
	{
		acc *= 10;
		fracDigits++;
	}
	while (fracDigits > scale)
	{
		acc /= 10;
		fracDigits--;
	}
	*out = neg ? -acc : acc;
	pfree(s);
	return true;
}

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

/*
 * A leaf column is one Parquet column chunk: a scalar column, an array's element,
 * or one field of a composite. Nesting is expressed through repetition and
 * definition levels (the Dremel model). For a plain scalar, max_rep is 0 and
 * max_def is 1, so the encoding is byte-identical to the flat writer.
 */
typedef struct PqLeaf
{
	const char *path[3];		/* schema path from the top column to the leaf */
	int			pathlen;
	ParquetKind kind;
	int			ptype;			/* Parquet physical type */
	int			convType;		/* ConvertedType, or -1 */
	int			typeLength;		/* FIXED_LEN_BYTE_ARRAY length, else 0 */
	int			precision;
	int			scale;
	bool		convertText;
	FmgrInfo	outFinfo;
	int			max_def;
	int			max_rep;
	StringInfoData defs;		/* one byte per level entry (definition level) */
	StringInfoData reps;		/* one byte per level entry (repetition level) */
	StringInfoData values;		/* PLAIN values, non-null only */
	StringInfoData boolbits;	/* 1 byte per non-null bool value */
	int64		nEntries;		/* level entries in the current row group */
}			PqLeaf;

/* how a top-level column shreds into leaves */
typedef enum
{
	TOP_SCALAR,
	TOP_LIST,					/* 1-D array -> LIST of one scalar element */
	TOP_STRUCT					/* composite -> group of scalar fields */
}			TopKind;

typedef struct TopColumn
{
	char	   *name;
	TopKind		tkind;
	int			firstLeaf;		/* index of the first leaf in leaves[] */
	int			nleaves;		/* 1 for scalar/list, #fields for struct */
	/* list element decode */
	Oid			elemtype;
	int16		elemlen;
	bool		elembyval;
	char		elemalign;
	/* struct */
	TupleDesc	structDesc;
	int		   *fieldLeaf;		/* [structDesc->natts] leaf index, -1 if dropped */
}			TopColumn;

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
pqleaf_reset(PqLeaf *c)
{
	resetStringInfo(&c->defs);
	resetStringInfo(&c->reps);
	resetStringInfo(&c->values);
	resetStringInfo(&c->boolbits);
	c->nEntries = 0;
}

static ParquetKind
parquet_kind_for_type(Oid typid, int32 typmod, int *ptype, int *convType,
					  int *typeLength, int *precision, int *scale)
{
	*convType = -1;
	*typeLength = 0;
	*precision = 0;
	*scale = 0;
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
		case JSONOID:
		case JSONBOID:
			*ptype = PQ_BYTE_ARRAY;
			*convType = PQ_CT_UTF8;
			return P_UTF8;
		case BYTEAOID:
			*ptype = PQ_BYTE_ARRAY;
			return P_BINARY;
		case DATEOID:
			*ptype = PQ_INT32;
			*convType = PQ_CT_DATE;
			return P_DATE;
		case TIMEOID:
			*ptype = PQ_INT64;
			*convType = PQ_CT_TIME_MICROS;
			return P_TIME;
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			*ptype = PQ_INT64;
			*convType = PQ_CT_TIMESTAMP_MICROS;
			return P_TIMESTAMP;
		case UUIDOID:
			*ptype = PQ_FIXED_LEN_BYTE_ARRAY;
			*typeLength = 16;
			return P_UUID;
		case NUMERICOID:
			/* numeric(p,s) with p<=38 and 0<=s<=p -> DECIMAL in a 16-byte
			 * FIXED_LEN_BYTE_ARRAY; otherwise fall back to text. */
			if (typmod >= (int32) VARHDRSZ)
			{
				int32		tmp = typmod - VARHDRSZ;
				int			p = (tmp >> 16) & 0xffff;
				int			s = tmp & 0xffff;

				if (p >= 1 && p <= 38 && s >= 0 && s <= p)
				{
					*ptype = PQ_FIXED_LEN_BYTE_ARRAY;
					*typeLength = 16;
					*convType = PQ_CT_DECIMAL;
					*precision = p;
					*scale = s;
					return P_DECIMAL;
				}
			}
			*ptype = PQ_BYTE_ARRAY;
			*convType = PQ_CT_UTF8;
			return P_UTF8;			/* text fallback */
		default:
			*ptype = -1;
			return P_INT32;
	}
}

/* whether a value has a Parquet representation (else it is folded to null) */
static bool
leaf_value_representable(PqLeaf *c, Datum d, __int128 *dec)
{
	switch (c->kind)
	{
		case P_DATE:
			return !DATE_NOT_FINITE(DatumGetDateADT(d));
		case P_TIMESTAMP:
			return !TIMESTAMP_NOT_FINITE(DatumGetTimestamp(d));
		case P_DECIMAL:
			return numeric_to_int128(d, c->scale, dec);
		default:
			return true;
	}
}

/* append one PLAIN value to a leaf's value buffer */
static void
write_leaf_value(PqLeaf *c, Datum d, __int128 dec)
{
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
		case P_DATE:
			{
				int32		v = (int32) (DatumGetDateADT(d) + PG_TO_UNIX_DAYS);

				appendBinaryStringInfo(&c->values, (char *) &v, 4);
				break;
			}
		case P_TIME:
			{
				int64		v = (int64) DatumGetTimeADT(d);

				appendBinaryStringInfo(&c->values, (char *) &v, 8);
				break;
			}
		case P_TIMESTAMP:
			{
				int64		v = (int64) DatumGetTimestamp(d) + PG_TO_UNIX_USECS;

				appendBinaryStringInfo(&c->values, (char *) &v, 8);
				break;
			}
		case P_UUID:
			appendBinaryStringInfo(&c->values,
								   (char *) DatumGetUUIDP(d)->data, UUID_LEN);
			break;
		case P_DECIMAL:
			{
				/* big-endian two's complement, 16 bytes */
				char		be[16];
				char	   *le = (char *) &dec;
				int			j;

				for (j = 0; j < 16; j++)
					be[j] = le[15 - j];
				appendBinaryStringInfo(&c->values, be, 16);
				break;
			}
		case P_BOOL:
			appendStringInfoChar(&c->boolbits, DatumGetBool(d) ? 1 : 0);
			break;
		case P_UTF8:
		case P_BINARY:
			if (c->convertText)
			{
				/* numeric/jsonb fallback: canonical text via output function */
				char	   *str = OutputFunctionCall(&c->outFinfo, d);
				int32		len = (int32) strlen(str);

				appendBinaryStringInfo(&c->values, (char *) &len, 4);
				if (len > 0)
					appendBinaryStringInfo(&c->values, str, len);
				pfree(str);
			}
			else
			{
				struct varlena *v = PG_DETOAST_DATUM_PACKED(d);
				int32		len = VARSIZE_ANY_EXHDR(v);

				appendBinaryStringInfo(&c->values, (char *) &len, 4);
				if (len > 0)
					appendBinaryStringInfo(&c->values, VARDATA_ANY(v), len);
			}
			break;
	}
}

/*
 * Append one Dremel entry to a leaf: its definition and repetition levels, and
 * the PLAIN value when present. A present value with no Parquet representation
 * is folded to the "container present, value absent" level (max_def - 1).
 */
static void
leaf_entry(PqLeaf *c, int def, int rep, bool hasValue, Datum d)
{
	__int128	dec = 0;

	if (hasValue && !leaf_value_representable(c, d, &dec))
	{
		hasValue = false;
		def = c->max_def - 1;
	}
	appendStringInfoChar(&c->defs, (char) def);
	if (c->max_rep > 0)
		appendStringInfoChar(&c->reps, (char) rep);
	c->nEntries++;
	if (hasValue)
		write_leaf_value(c, d, dec);
}

/* shred one top-level column value for a row into its leaf/leaves */
static void
shred_top(TopColumn *tc, PqLeaf *leaves, Datum d, bool isnull)
{
	switch (tc->tkind)
	{
		case TOP_SCALAR:
			leaf_entry(&leaves[tc->firstLeaf], isnull ? 0 : 1, 0, !isnull, d);
			break;
		case TOP_LIST:
			{
				PqLeaf	   *leaf = &leaves[tc->firstLeaf];

				if (isnull)
					leaf_entry(leaf, 0, 0, false, (Datum) 0);
				else
				{
					ArrayType  *arr = DatumGetArrayTypeP(d);
					Datum	   *elems;
					bool	   *enulls;
					int			n;
					int			k;

					if (ARR_NDIM(arr) > 1)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("columnar.export_parquet does not support multi-dimensional arrays")));
					deconstruct_array(arr, tc->elemtype, tc->elemlen,
									  tc->elembyval, tc->elemalign, &elems, &enulls, &n);
					if (n == 0)
						leaf_entry(leaf, 1, 0, false, (Datum) 0);	/* empty list */
					else
						for (k = 0; k < n; k++)
							leaf_entry(leaf, enulls[k] ? 2 : 3, k == 0 ? 0 : 1,
									   !enulls[k], elems[k]);
				}
				break;
			}
		case TOP_STRUCT:
			{
				Datum	   *fv = NULL;
				bool	   *fn = NULL;
				int			a;

				if (!isnull)
				{
					HeapTupleHeader th = DatumGetHeapTupleHeader(d);
					HeapTupleData tmp;

					fv = palloc(sizeof(Datum) * tc->structDesc->natts);
					fn = palloc(sizeof(bool) * tc->structDesc->natts);
					tmp.t_len = HeapTupleHeaderGetDatumLength(th);
					ItemPointerSetInvalid(&tmp.t_self);
					tmp.t_tableOid = InvalidOid;
					tmp.t_data = th;
					heap_deform_tuple(&tmp, tc->structDesc, fv, fn);
				}
				for (a = 0; a < tc->structDesc->natts; a++)
				{
					int			li = tc->fieldLeaf[a];

					if (li < 0)
						continue;	/* dropped field */
					if (isnull)
						leaf_entry(&leaves[li], 0, 0, false, (Datum) 0);
					else if (fn[a])
						leaf_entry(&leaves[li], 1, 0, false, (Datum) 0);
					else
						leaf_entry(&leaves[li], 2, 0, true, fv[a]);
				}
				break;
			}
	}
}

static int
bits_for(int m)
{
	int			b = 0;

	while ((1 << b) <= m)
		b++;
	return b;
}

/*
 * Build an RLE/bit-packed hybrid level section (one bit-packed run at the given
 * bit width), prefixed with its int32 LE byte length (DataPage v1 convention).
 * With bit width 1 this is byte-identical to the flat writer's def levels.
 */
static void
build_rle_levels(StringInfo out, const uint8 *levels, int64 n, int bit_width)
{
	StringInfoData h;
	int64		ngroups = (n + 7) / 8;
	int64		nbytes = ngroups * bit_width;
	uint8	   *packed = palloc0(Max(nbytes, 1));
	int64		i;
	int32		len;

	initStringInfo(&h);
	tc_varint(&h, (uint64) ((ngroups << 1) | 1));	/* one bit-packed run */
	for (i = 0; i < n; i++)
	{
		uint32		v = levels[i];
		int			b;

		for (b = 0; b < bit_width; b++)
			if (v & (1u << b))
			{
				int64		abs = i * bit_width + b;

				packed[abs >> 3] |= (1 << (abs & 7));
			}
	}
	appendBinaryStringInfo(&h, (char *) packed, nbytes);
	len = h.len;
	appendBinaryStringInfo(out, (char *) &len, 4);
	appendBinaryStringInfo(out, h.data, h.len);
	pfree(packed);
	pfree(h.data);
}

/* assemble the PLAIN values buffer for a leaf (from its accumulators) */
static void
build_values(StringInfo out, PqLeaf *c)
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

/* build a data page body for a leaf: [rep levels][def levels][values] */
static void
build_leaf_body(StringInfo body, PqLeaf *c)
{
	if (c->max_rep > 0)
		build_rle_levels(body, (const uint8 *) c->reps.data, c->nEntries,
						 bits_for(c->max_rep));
	build_rle_levels(body, (const uint8 *) c->defs.data, c->nEntries,
					 bits_for(c->max_def));
	build_values(body, c);
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

/* one leaf SchemaElement (a primitive) */
static void
write_schema_leaf(StringInfo b, const char *name, PqLeaf *leaf, int repetition)
{
	int16		last = 0;

	tc_i32_field(b, &last, 1, leaf->ptype);	/* type */
	if (leaf->ptype == PQ_FIXED_LEN_BYTE_ARRAY)
		tc_i32_field(b, &last, 2, leaf->typeLength);
	tc_i32_field(b, &last, 3, repetition);
	tc_string_field(b, &last, 4, name, (int) strlen(name));
	if (leaf->convType >= 0)
		tc_i32_field(b, &last, 6, leaf->convType);
	if (leaf->convType == PQ_CT_DECIMAL)
	{
		tc_i32_field(b, &last, 7, leaf->scale);
		tc_i32_field(b, &last, 8, leaf->precision);
	}
	tc_stop(b);
}

/* one group SchemaElement (no physical type; has num_children) */
static void
write_schema_group(StringInfo b, const char *name, int repetition,
				   int num_children, int convType)
{
	int16		last = 0;

	tc_i32_field(b, &last, 3, repetition);
	tc_string_field(b, &last, 4, name, (int) strlen(name));
	tc_i32_field(b, &last, 5, num_children);
	if (convType >= 0)
		tc_i32_field(b, &last, 6, convType);
	tc_stop(b);
}

/* number of SchemaElements a top column contributes (excluding the root) */
static int
schema_count_for_top(TopColumn *tc)
{
	switch (tc->tkind)
	{
		case TOP_LIST:
			return 3;			/* group(LIST), group(list), element */
		case TOP_STRUCT:
			return 1 + tc->nleaves;
		default:
			return 1;
	}
}

/* emit a top column's schema subtree (pre-order) */
static void
write_top_schema(StringInfo b, TopColumn *tc, PqLeaf *leaves)
{
	switch (tc->tkind)
	{
		case TOP_SCALAR:
			write_schema_leaf(b, tc->name, &leaves[tc->firstLeaf], 1);
			break;
		case TOP_LIST:
			write_schema_group(b, tc->name, 1, 1, 3 /* LIST */);
			write_schema_group(b, "list", 2 /* REPEATED */, 1, -1);
			write_schema_leaf(b, "element", &leaves[tc->firstLeaf], 1);
			break;
		case TOP_STRUCT:
			{
				int			a;

				write_schema_group(b, tc->name, 1, tc->nleaves, -1);
				for (a = 0; a < tc->structDesc->natts; a++)
				{
					int			li = tc->fieldLeaf[a];

					if (li < 0)
						continue;
					write_schema_leaf(b,
									  NameStr(TupleDescAttr(tc->structDesc, a)->attname),
									  &leaves[li], 1);
				}
				break;
			}
	}
}

/* one column chunk for a leaf (path_in_schema is the full leaf path) */
static void
write_column_chunk(StringInfo b, PqLeaf *c, PqColMeta *m)
{
	int16		last = 0;
	int16		mlast = 0;
	int			p;

	tc_i64_field(b, &last, 2, m->dataPageOffset);	/* file_offset */
	tc_field(b, &last, 3, TC_STRUCT);				/* meta_data */
	tc_i32_field(b, &mlast, 1, c->ptype);			/* type */
	tc_field(b, &mlast, 2, TC_LIST);				/* encodings [PLAIN, RLE] */
	tc_list_header(b, 2, TC_I32);
	tc_zigzag32(b, PQ_ENC_PLAIN);
	tc_zigzag32(b, PQ_ENC_RLE);
	tc_field(b, &mlast, 3, TC_LIST);				/* path_in_schema */
	tc_list_header(b, c->pathlen, TC_BINARY);
	for (p = 0; p < c->pathlen; p++)
	{
		tc_varint(b, (uint64) strlen(c->path[p]));
		appendBinaryStringInfo(b, c->path[p], strlen(c->path[p]));
	}
	tc_i32_field(b, &mlast, 4, 0);					/* codec = UNCOMPRESSED */
	tc_i64_field(b, &mlast, 5, m->numValues);		/* num_values */
	tc_i64_field(b, &mlast, 6, m->totalSize);		/* total_uncompressed_size */
	tc_i64_field(b, &mlast, 7, m->totalSize);		/* total_compressed_size */
	tc_i64_field(b, &mlast, 9, m->dataPageOffset);	/* data_page_offset */
	tc_stop(b);										/* end ColumnMetaData */
	tc_stop(b);										/* end ColumnChunk */
}

static void
write_row_group(StringInfo b, PqLeaf *leaves, int nleaves, PqRowGroup *rg)
{
	int16		last = 0;
	int			i;

	tc_field(b, &last, 1, TC_LIST);					/* columns */
	tc_list_header(b, nleaves, TC_STRUCT);
	for (i = 0; i < nleaves; i++)
		write_column_chunk(b, &leaves[i], &rg->cols[i]);
	tc_i64_field(b, &last, 2, rg->totalByteSize);
	tc_i64_field(b, &last, 3, rg->numRows);
	tc_stop(b);
}

/* initialize a scalar leaf for a given type; *ok=false if unsupported */
static void
build_leaf_scalar(PqLeaf *leaf, Oid typid, int32 typmod,
				  int max_def, int max_rep, bool *ok)
{
	int			ptype,
				convType,
				typeLength,
				precision,
				scale;
	ParquetKind kind = parquet_kind_for_type(typid, typmod, &ptype, &convType,
											 &typeLength, &precision, &scale);

	if (ptype < 0)
	{
		*ok = false;
		return;
	}
	leaf->kind = kind;
	leaf->ptype = ptype;
	leaf->convType = convType;
	leaf->typeLength = typeLength;
	leaf->precision = precision;
	leaf->scale = scale;
	leaf->max_def = max_def;
	leaf->max_rep = max_rep;
	leaf->convertText = (kind == P_UTF8 &&
						 (typid == NUMERICOID || typid == JSONBOID));
	if (leaf->convertText)
	{
		Oid			outfunc;
		bool		isvarlena;

		getTypeOutputInfo(typid, &outfunc, &isvarlena);
		fmgr_info(outfunc, &leaf->outFinfo);
	}
	initStringInfo(&leaf->defs);
	initStringInfo(&leaf->reps);
	initStringInfo(&leaf->values);
	initStringInfo(&leaf->boolbits);
}

/* number of leaves a top column of this type contributes */
static int
count_leaves_for(Oid typid, int32 typmod)
{
	if (OidIsValid(get_element_type(typid)))
		return 1;
	if (get_typtype(typid) == TYPTYPE_COMPOSITE)
	{
		TupleDesc	td = lookup_rowtype_tupdesc(typid, typmod);
		int			a,
					live = 0;

		for (a = 0; a < td->natts; a++)
			if (!TupleDescAttr(td, a)->attisdropped)
				live++;
		ReleaseTupleDesc(td);
		return live;
	}
	return 1;
}

/* build one top column and its leaves; *ok=false if any leaf type is unsupported */
static void
build_top_column(TopColumn *tc, const char *name, Oid typid, int32 typmod,
				 PqLeaf *leaves, int *nleaves, bool *ok)
{
	Oid			elemtype = get_element_type(typid);

	tc->name = pstrdup(name);
	tc->structDesc = NULL;
	tc->fieldLeaf = NULL;

	if (OidIsValid(elemtype))
	{
		PqLeaf	   *leaf;

		tc->tkind = TOP_LIST;
		tc->firstLeaf = *nleaves;
		tc->nleaves = 1;
		tc->elemtype = elemtype;
		get_typlenbyvalalign(elemtype, &tc->elemlen, &tc->elembyval, &tc->elemalign);
		leaf = &leaves[(*nleaves)++];
		build_leaf_scalar(leaf, elemtype, -1, 3, 1, ok);
		leaf->path[0] = tc->name;
		leaf->path[1] = "list";
		leaf->path[2] = "element";
		leaf->pathlen = 3;
		return;
	}
	if (get_typtype(typid) == TYPTYPE_COMPOSITE)
	{
		TupleDesc	td = lookup_rowtype_tupdesc(typid, typmod);
		int			a;
		int			live = 0;

		tc->tkind = TOP_STRUCT;
		tc->firstLeaf = *nleaves;
		tc->structDesc = CreateTupleDescCopy(td);
		tc->fieldLeaf = palloc(sizeof(int) * td->natts);
		for (a = 0; a < td->natts; a++)
		{
			Form_pg_attribute fa = TupleDescAttr(td, a);
			PqLeaf	   *leaf;

			if (fa->attisdropped)
			{
				tc->fieldLeaf[a] = -1;
				continue;
			}
			leaf = &leaves[*nleaves];
			build_leaf_scalar(leaf, fa->atttypid, fa->atttypmod, 2, 0, ok);
			leaf->path[0] = tc->name;
			leaf->path[1] = pstrdup(NameStr(fa->attname));
			leaf->pathlen = 2;
			tc->fieldLeaf[a] = (*nleaves)++;
			live++;
		}
		tc->nleaves = live;
		ReleaseTupleDesc(td);
		return;
	}

	tc->tkind = TOP_SCALAR;
	tc->firstLeaf = *nleaves;
	tc->nleaves = 1;
	{
		PqLeaf	   *leaf = &leaves[(*nleaves)++];

		build_leaf_scalar(leaf, typid, typmod, 1, 0, ok);
		leaf->path[0] = tc->name;
		leaf->pathlen = 1;
	}
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
	int			ntop;
	TopColumn  *tops;
	PqLeaf	   *leaves;
	int			nleaves = 0;
	int			totalLeaves = 0;
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
	ntop = tupdesc->natts;

	/* reject dropped columns and count the leaf columns (arrays and composites
	 * expand to more than one leaf) */
	for (i = 0; i < ntop; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		if (att->attisdropped)
		{
			table_close(rel, AccessShareLock);
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("columnar.export_parquet does not support dropped columns")));
		}
		totalLeaves += count_leaves_for(att->atttypid, att->atttypmod);
	}

	tops = palloc0(sizeof(TopColumn) * ntop);
	leaves = palloc0(sizeof(PqLeaf) * Max(totalLeaves, 1));
	for (i = 0; i < ntop; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);
		bool		ok = true;

		build_top_column(&tops[i], NameStr(att->attname), att->atttypid,
						 att->atttypmod, leaves, &nleaves, &ok);
		if (!ok)
		{
			table_close(rel, AccessShareLock);
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("column \"%s\" has type %s, which columnar.export_parquet does not support",
							NameStr(att->attname),
							format_type_be(att->atttypid))));
		}
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

	values = palloc(sizeof(Datum) * ntop);
	nulls = palloc(sizeof(bool) * ntop);

	snapshot = ActiveSnapshotSet() ? GetActiveSnapshot() : GetTransactionSnapshot();
	readState = ColumnarBeginRead(rel, snapshot, NULL, NULL, 0, NULL);

	for (;;)
	{
		bool		got = ColumnarReadNextRow(readState, values, nulls, &rowNumber);

		if (got)
		{
			CHECK_FOR_INTERRUPTS();
			for (i = 0; i < ntop; i++)
				shred_top(&tops[i], leaves, values[i], nulls[i]);
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
			rg->cols = MemoryContextAlloc(rgCtx, sizeof(PqColMeta) * Max(nleaves, 1));
			rg->numRows = groupRows;
			rg->totalByteSize = 0;

			for (i = 0; i < nleaves; i++)
			{
				StringInfoData body;
				StringInfoData ph;
				int64		pageStart = offset;

				initStringInfo(&body);
				build_leaf_body(&body, &leaves[i]);

				initStringInfo(&ph);
				write_page_header(&ph, leaves[i].nEntries, (int32) body.len);

				fwrite(ph.data, 1, ph.len, f);
				fwrite(body.data, 1, body.len, f);
				offset += ph.len + body.len;

				rg->cols[i].dataPageOffset = pageStart;
				rg->cols[i].totalSize = ph.len + body.len;
				rg->cols[i].numValues = leaves[i].nEntries;
				rg->totalByteSize += ph.len + body.len;

				pfree(body.data);
				pfree(ph.data);
				pqleaf_reset(&leaves[i]);
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
		int			nschema = 1;

		for (i = 0; i < ntop; i++)
			nschema += schema_count_for_top(&tops[i]);

		initStringInfo(&fmd);
		tc_i32_field(&fmd, &last, 1, 1);	/* version */
		/* schema list (2): root + the (possibly nested) elements per column */
		tc_field(&fmd, &last, 2, TC_LIST);
		tc_list_header(&fmd, nschema, TC_STRUCT);
		write_schema_element_root(&fmd, ntop);
		for (i = 0; i < ntop; i++)
			write_top_schema(&fmd, &tops[i], leaves);
		tc_i64_field(&fmd, &last, 3, total);	/* num_rows */
		/* row_groups list (4) */
		tc_field(&fmd, &last, 4, TC_LIST);
		tc_list_header(&fmd, nrgs, TC_STRUCT);
		for (i = 0; i < nrgs; i++)
			write_row_group(&fmd, leaves, nleaves, &rgs[i]);
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
