/*-------------------------------------------------------------------------
 *
 * columnar_parquet_reader.c
 *		Parquet file import: columnar.import_parquet(rel regclass, path text).
 *
 * A self-contained Parquet reader with no libparquet/libarrow dependency. It
 * parses the Thrift compact-protocol file metadata, decompresses Snappy (and
 * handles uncompressed) data pages, and decodes PLAIN and dictionary
 * (RLE_DICTIONARY / PLAIN_DICTIONARY) encodings from both DATA_PAGE (v1) and
 * DATA_PAGE_V2 pages -- the combination pyarrow writes by default. Rows are
 * inserted into an existing target table (its tuple descriptor defines the
 * expected columns and types), mirroring columnar.import_arrow.
 *
 * Independent MIT implementation built from the Apache Parquet format and Thrift
 * compact-protocol specifications, the Snappy format description, and the public
 * PostgreSQL API only. See PROVENANCE.md. Little-endian hosts only.
 *
 *-------------------------------------------------------------------------
 */
#include "columnar.h"

#include <math.h>
#include <sys/stat.h>

#include "fmgr.h"
#include "funcapi.h"
#include "access/htup_details.h"
#include "common/int.h"
#include "access/relation.h"
#include "access/reloptions.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#if PG_VERSION_NUM >= 180000
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#else
#include "commands/explain.h"
#endif
#include "executor/tuptable.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "storage/fd.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"
#include "utils/typcache.h"

PG_FUNCTION_INFO_V1(columnar_import_parquet);
PG_FUNCTION_INFO_V1(columnar_parquet_schema);
PG_FUNCTION_INFO_V1(columnar_read_parquet);
PG_FUNCTION_INFO_V1(pgcolumnar_parquet_fdw_handler);
PG_FUNCTION_INFO_V1(pgcolumnar_parquet_fdw_validator);

#define PG_TO_UNIX_DAYS		((int64) (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE))
#define PG_TO_UNIX_USECS	(PG_TO_UNIX_DAYS * USECS_PER_DAY)

/* Parquet physical types (parquet.thrift Type) */
#define PQ_BOOLEAN		0
#define PQ_INT32		1
#define PQ_INT64		2
#define PQ_FLOAT		4
#define PQ_DOUBLE		5
#define PQ_BYTE_ARRAY	6
#define PQ_FIXED_LEN_BYTE_ARRAY 7

/* ConvertedType (parquet.thrift ConvertedType); -1 means none */
#define PQ_CT_UTF8				0
#define PQ_CT_ENUM				4
#define PQ_CT_DECIMAL			5
#define PQ_CT_DATE				6
#define PQ_CT_TIME_MILLIS		7
#define PQ_CT_TIME_MICROS		8
#define PQ_CT_TIMESTAMP_MILLIS	9
#define PQ_CT_TIMESTAMP_MICROS	10
#define PQ_CT_INT_8				15
#define PQ_CT_INT_16			16
#define PQ_CT_INT_32			17
#define PQ_CT_INT_64			18
#define PQ_CT_JSON				19

/* Encodings (parquet.thrift Encoding) */
#define PQE_PLAIN		0
#define PQE_PLAIN_DICTIONARY 2
#define PQE_RLE			3
#define PQE_RLE_DICTIONARY 8

/* Compression codecs */
#define PQC_UNCOMPRESSED 0
#define PQC_SNAPPY		1

/* PageType */
#define PQ_PAGE_DATA	0
#define PQ_PAGE_DICTIONARY 2
#define PQ_PAGE_DATA_V2	3

/* Thrift compact field types */
#define TC_STOP			0
#define TC_BOOL_TRUE	1
#define TC_BOOL_FALSE	2
#define TC_BYTE			3
#define TC_I16			4
#define TC_I32			5
#define TC_I64			6
#define TC_DOUBLE		7
#define TC_BINARY		8
#define TC_LIST			9
#define TC_SET			10
#define TC_STRUCT		12

/* -------------------------------------------------------------------------
 * Snappy raw decompression (format spec: preamble varint uncompressed length,
 * then a stream of literal and copy elements). Clean-room from the format.
 * ------------------------------------------------------------------------- */
static bool
snappy_raw_uncompress(const uint8 *in, size_t inlen, StringInfo out)
{
	size_t		pos = 0;
	uint32		ulen = 0;
	int			shift = 0;

	/* preamble: uncompressed length as a varint */
	while (pos < inlen)
	{
		uint8		b = in[pos++];

		ulen |= (uint32) (b & 0x7f) << shift;
		if ((b & 0x80) == 0)
			break;
		shift += 7;
		if (shift > 32)
			return false;
	}

	enlargeStringInfo(out, ulen);
	while (pos < inlen)
	{
		uint8		tag = in[pos++];
		int			type = tag & 0x03;

		if (type == 0)			/* literal */
		{
			uint32		len = (tag >> 2) + 1;

			if (len > 60)
			{
				int			nb = (tag >> 2) - 59;	/* 1..4 bytes of length */
				uint32		l = 0;
				int			i;

				if (pos + nb > inlen)
					return false;
				for (i = 0; i < nb; i++)
					l |= (uint32) in[pos++] << (8 * i);
				len = l + 1;
			}
			if (pos + len > inlen)
				return false;
			appendBinaryStringInfo(out, (const char *) in + pos, len);
			pos += len;
		}
		else					/* copy */
		{
			uint32		len;
			uint32		offset;
			int			i;

			if (type == 1)
			{
				len = ((tag >> 2) & 0x07) + 4;
				if (pos >= inlen)
					return false;
				offset = ((uint32) (tag >> 5) << 8) | in[pos++];
			}
			else if (type == 2)
			{
				len = (tag >> 2) + 1;
				if (pos + 2 > inlen)
					return false;
				offset = (uint32) in[pos] | ((uint32) in[pos + 1] << 8);
				pos += 2;
			}
			else				/* type == 3 */
			{
				len = (tag >> 2) + 1;
				if (pos + 4 > inlen)
					return false;
				offset = (uint32) in[pos] | ((uint32) in[pos + 1] << 8) |
					((uint32) in[pos + 2] << 16) | ((uint32) in[pos + 3] << 24);
				pos += 4;
			}
			if (offset == 0 || offset > (uint32) out->len)
				return false;
			/* copies may overlap, so copy byte by byte from the output so far */
			for (i = 0; i < (int) len; i++)
			{
				char		c = out->data[out->len - offset];

				appendBinaryStringInfo(out, &c, 1);
			}
		}
	}
	return (uint32) out->len == ulen;
}

/* -------------------------------------------------------------------------
 * Thrift compact-protocol reader over an in-memory buffer.
 * ------------------------------------------------------------------------- */
typedef struct TCReader
{
	const uint8 *buf;
	size_t		len;
	size_t		pos;
	bool		error;
} TCReader;

static uint64
tcr_varint(TCReader *r)
{
	uint64		v = 0;
	int			shift = 0;

	while (r->pos < r->len)
	{
		uint8		b = r->buf[r->pos++];

		v |= (uint64) (b & 0x7f) << shift;
		if ((b & 0x80) == 0)
			return v;
		shift += 7;
		if (shift > 63)
			break;
	}
	r->error = true;
	return v;
}

static int64
tcr_zigzag(TCReader *r)
{
	uint64		u = tcr_varint(r);

	return (int64) (u >> 1) ^ -(int64) (u & 1);
}

/* read a binary/string field: returns pointer into the buffer and its length */
static const uint8 *
tcr_bytes(TCReader *r, uint32 *outlen)
{
	uint64		n = tcr_varint(r);
	const uint8 *p;

	if (r->error || r->pos + n > r->len)
	{
		r->error = true;
		*outlen = 0;
		return NULL;
	}
	p = r->buf + r->pos;
	r->pos += n;
	*outlen = (uint32) n;
	return p;
}

/*
 * Read a struct field header. Returns the compact field type in *ftype (TC_STOP
 * at end of struct) and the absolute field id in *fid. lastId is updated for the
 * short-form delta encoding.
 */
static void
tcr_field(TCReader *r, int *ftype, int *fid, int *lastId)
{
	uint8		b;

	if (r->pos >= r->len)
	{
		r->error = true;
		*ftype = TC_STOP;
		return;
	}
	b = r->buf[r->pos++];
	if (b == 0)
	{
		*ftype = TC_STOP;
		return;
	}
	*ftype = b & 0x0f;
	if ((b >> 4) != 0)
		*fid = *lastId + (b >> 4);	/* short-form delta */
	else
		*fid = (int) tcr_zigzag(r);	/* long form */
	*lastId = *fid;
}

/* skip a value of the given compact type (for fields we do not consume) */
static void
tcr_skip(TCReader *r, int ftype)
{
	switch (ftype)
	{
		case TC_BOOL_TRUE:
		case TC_BOOL_FALSE:
			break;
		case TC_BYTE:
			r->pos += 1;
			break;
		case TC_I16:
		case TC_I32:
		case TC_I64:
			(void) tcr_zigzag(r);
			break;
		case TC_DOUBLE:
			r->pos += 8;
			break;
		case TC_BINARY:
			{
				uint32		n;

				(void) tcr_bytes(r, &n);
				break;
			}
		case TC_LIST:
		case TC_SET:
			{
				uint8		sizeType;
				uint32		size;
				int			et;
				uint32		i;

				if (r->pos >= r->len)
				{
					r->error = true;
					return;
				}
				sizeType = r->buf[r->pos++];
				size = (sizeType >> 4) & 0x0f;
				et = sizeType & 0x0f;
				if (size == 0x0f)
					size = (uint32) tcr_varint(r);
				for (i = 0; i < size && !r->error; i++)
					tcr_skip(r, et);
				break;
			}
		case TC_STRUCT:
			{
				int			lastId = 0;

				for (;;)
				{
					int			ft,
								fid;

					tcr_field(r, &ft, &fid, &lastId);
					if (ft == TC_STOP || r->error)
						break;
					tcr_skip(r, ft);
				}
				break;
			}
		default:
			break;
	}
}

/* list header: returns element count and element compact type */
static uint32
tcr_list_header(TCReader *r, int *etype)
{
	uint8		b;
	uint32		size;

	if (r->pos >= r->len)
	{
		r->error = true;
		*etype = 0;
		return 0;
	}
	b = r->buf[r->pos++];
	size = (b >> 4) & 0x0f;
	*etype = b & 0x0f;
	if (size == 0x0f)
		size = (uint32) tcr_varint(r);
	return size;
}

/* -------------------------------------------------------------------------
 * Parsed metadata (flat schema only).
 * ------------------------------------------------------------------------- */
typedef struct PqSchemaCol
{
	int			phys_type;		/* PQ_* physical type (-1 for a group) */
	int			repetition;		/* 0 required, 1 optional, 2 repeated */
	int			converted_type;	/* -1 if none */
	int			type_length;	/* FIXED_LEN_BYTE_ARRAY length */
	int			scale;			/* DECIMAL scale (0 if none) */
	int			precision;		/* DECIMAL precision (0 if none) */
	int			num_children;	/* >0 for a group */
	char	   *name;
} PqSchemaCol;

/* a leaf column (primitive) with its computed Dremel level bounds */
typedef struct PqLeafInfo
{
	PqSchemaCol *sc;
	int			max_def;
	int			max_rep;
} PqLeafInfo;

typedef struct PqChunk
{
	int			codec;
	int64		num_values;
	int64		data_page_offset;
	int64		dict_page_offset;	/* 0 if none */
	int64		total_compressed_size;
	/* Statistics (optional). min/max point into the metadata buffer, so they are
	 * valid as long as the file buffer lives (through a scan). */
	bool		has_min;
	bool		has_max;
	const uint8 *stat_min;
	const uint8 *stat_max;
	uint32		stat_min_len;
	uint32		stat_max_len;
} PqChunk;

typedef struct PqRowGroup
{
	int64		num_rows;
	PqChunk    *chunks;			/* [nchunks]; consumers require nchunks == ncols */
	int			nchunks;		/* column chunks this row group actually carries */
} PqRowGroup;

typedef struct PqFile
{
	int			nelems;			/* all schema elements, pre-order (root at [0]) */
	PqSchemaCol *elems;
	int			ncols;			/* leaf columns (= column chunks per row group) */
	PqLeafInfo *leaves;			/* [ncols], in pre-order = column-chunk order */
	int			ntop;			/* top-level columns (root's children) */
	int			nrowgroups;
	PqRowGroup *rgs;
} PqFile;

/*
 * Parse a Statistics struct into *ch. Prefers the current min_value (field 6) /
 * max_value (field 5) over the deprecated min (2) / max (1). Values are kept as
 * raw pointers into the metadata buffer. Other fields, including null_count, are
 * skipped: nothing reads them yet, and an all-NULL skip would additionally need
 * the operator's strictness checked.
 */
static void
parse_statistics(TCReader *r, PqChunk *ch)
{
	int			lastId = 0;

	for (;;)
	{
		int			ft,
					fid;

		tcr_field(r, &ft, &fid, &lastId);
		if (ft == TC_STOP || r->error)
			break;
		switch (fid)
		{
			case 1:				/* max (deprecated): fallback only */
				{
					uint32		n;
					const uint8 *p = tcr_bytes(r, &n);

					if (p && !ch->has_max)
					{
						ch->stat_max = p;
						ch->stat_max_len = n;
						ch->has_max = true;
					}
					break;
				}
			case 2:				/* min (deprecated): fallback only */
				{
					uint32		n;
					const uint8 *p = tcr_bytes(r, &n);

					if (p && !ch->has_min)
					{
						ch->stat_min = p;
						ch->stat_min_len = n;
						ch->has_min = true;
					}
					break;
				}
			case 5:				/* max_value (preferred) */
				{
					uint32		n;
					const uint8 *p = tcr_bytes(r, &n);

					if (p)
					{
						ch->stat_max = p;
						ch->stat_max_len = n;
						ch->has_max = true;
					}
					break;
				}
			case 6:				/* min_value (preferred) */
				{
					uint32		n;
					const uint8 *p = tcr_bytes(r, &n);

					if (p)
					{
						ch->stat_min = p;
						ch->stat_min_len = n;
						ch->has_min = true;
					}
					break;
				}
			default:
				tcr_skip(r, ft);
				break;
		}
	}
}

/* parse a ColumnMetaData struct into *ch */
static void
parse_column_meta(TCReader *r, PqChunk *ch)
{
	int			lastId = 0;

	ch->codec = PQC_UNCOMPRESSED;
	ch->num_values = 0;
	ch->data_page_offset = 0;
	ch->dict_page_offset = 0;
	ch->total_compressed_size = 0;
	ch->has_min = false;
	ch->has_max = false;
	ch->stat_min = NULL;
	ch->stat_max = NULL;
	ch->stat_min_len = 0;
	ch->stat_max_len = 0;

	for (;;)
	{
		int			ft,
					fid;

		tcr_field(r, &ft, &fid, &lastId);
		if (ft == TC_STOP || r->error)
			break;
		switch (fid)
		{
			case 4:				/* codec */
				ch->codec = (int) tcr_zigzag(r);
				break;
			case 5:				/* num_values */
				ch->num_values = tcr_zigzag(r);
				break;
			case 7:				/* total_compressed_size */
				ch->total_compressed_size = tcr_zigzag(r);
				break;
			case 9:				/* data_page_offset */
				ch->data_page_offset = tcr_zigzag(r);
				break;
			case 11:			/* dictionary_page_offset */
				ch->dict_page_offset = tcr_zigzag(r);
				break;
			case 12:			/* statistics */
				if (ft == TC_STRUCT)
					parse_statistics(r, ch);
				else
					tcr_skip(r, ft);
				break;
			default:
				tcr_skip(r, ft);
				break;
		}
	}
}

/* parse a ColumnChunk struct (field 3 is the ColumnMetaData) */
static void
parse_column_chunk(TCReader *r, PqChunk *ch)
{
	int			lastId = 0;

	for (;;)
	{
		int			ft,
					fid;

		tcr_field(r, &ft, &fid, &lastId);
		if (ft == TC_STOP || r->error)
			break;
		if (fid == 3 && ft == TC_STRUCT)
			parse_column_meta(r, ch);
		else
			tcr_skip(r, ft);
	}
}

/* parse a SchemaElement into *sc; returns num_children (0 for a leaf) */
static int
parse_schema_element(TCReader *r, PqSchemaCol *sc)
{
	int			lastId = 0;
	int			num_children = 0;

	sc->phys_type = -1;
	sc->repetition = 0;
	sc->converted_type = -1;
	sc->type_length = 0;
	sc->scale = 0;
	sc->precision = 0;
	sc->num_children = 0;
	sc->name = NULL;

	for (;;)
	{
		int			ft,
					fid;

		tcr_field(r, &ft, &fid, &lastId);
		if (ft == TC_STOP || r->error)
			break;
		switch (fid)
		{
			case 1:				/* type */
				sc->phys_type = (int) tcr_zigzag(r);
				break;
			case 2:				/* type_length */
				sc->type_length = (int) tcr_zigzag(r);
				break;
			case 3:				/* repetition_type */
				sc->repetition = (int) tcr_zigzag(r);
				break;
			case 4:				/* name */
				{
					uint32		n;
					const uint8 *p = tcr_bytes(r, &n);

					if (p)
						sc->name = pnstrdup((const char *) p, n);
					break;
				}
			case 5:				/* num_children */
				num_children = (int) tcr_zigzag(r);
				sc->num_children = num_children;
				break;
			case 6:				/* converted_type */
				sc->converted_type = (int) tcr_zigzag(r);
				break;
			case 7:				/* scale (DECIMAL) */
				sc->scale = (int) tcr_zigzag(r);
				break;
			case 8:				/* precision (DECIMAL) */
				sc->precision = (int) tcr_zigzag(r);
				break;
			default:
				tcr_skip(r, ft);
				break;
		}
	}
	return num_children;
}

/*
 * Recursively walk one schema element at *cursor (pre-order), accumulating the
 * definition/repetition level bounds. A primitive (num_children == 0) becomes a
 * leaf; a group recurses into its children. Repetition contributes: OPTIONAL
 * +1 def, REPEATED +1 def and +1 rep (Dremel).
 */
static bool
walk_schema(PqFile *pf, int *cursor, int def, int rep,
			PqLeafInfo *leaves, int *nleaf)
{
	PqSchemaCol *e;
	int			d,
				rp;

	if (*cursor >= pf->nelems)
		return false;
	e = &pf->elems[(*cursor)++];
	d = def + (e->repetition == 1 ? 1 : 0) + (e->repetition == 2 ? 1 : 0);
	rp = rep + (e->repetition == 2 ? 1 : 0);

	if (e->num_children == 0)
	{
		leaves[*nleaf].sc = e;
		leaves[*nleaf].max_def = d;
		leaves[*nleaf].max_rep = rp;
		(*nleaf)++;
	}
	else
	{
		int			c;

		for (c = 0; c < e->num_children; c++)
			if (!walk_schema(pf, cursor, d, rp, leaves, nleaf))
				return false;
	}
	return true;
}

/* parse the whole FileMetaData; returns false on error or unsupported shape */
static bool
parse_file_metadata(const uint8 *buf, size_t len, PqFile *pf)
{
	TCReader	r = {buf, len, 0, false};
	int			lastId = 0;

	pf->ncols = 0;
	pf->nelems = 0;
	pf->elems = NULL;
	pf->leaves = NULL;
	pf->ntop = 0;
	pf->nrowgroups = 0;
	pf->rgs = NULL;

	for (;;)
	{
		int			ft,
					fid;

		tcr_field(&r, &ft, &fid, &lastId);
		if (ft == TC_STOP || r.error)
			break;

		if (fid == 2 && ft == TC_LIST)	/* schema: list<SchemaElement> */
		{
			int			etype;
			uint32		n = tcr_list_header(&r, &etype);
			uint32		i;
			PqSchemaCol *tmp = palloc0(sizeof(PqSchemaCol) * Max(n, 1));

			for (i = 0; i < n && !r.error; i++)
				parse_schema_element(&r, &tmp[i]);
			pf->nelems = (int) n;
			pf->elems = tmp;
		}
		else if (fid == 4 && ft == TC_LIST)		/* row_groups */
		{
			int			etype;
			uint32		n = tcr_list_header(&r, &etype);
			uint32		i;

			pf->nrowgroups = n;
			pf->rgs = palloc0(sizeof(PqRowGroup) * Max(n, 1));
			for (i = 0; i < n && !r.error; i++)
			{
				PqRowGroup *rg = &pf->rgs[i];
				int			rgLast = 0;

				rg->chunks = NULL;
				rg->nchunks = 0;
				for (;;)
				{
					int			rft,
								rfid;

					tcr_field(&r, &rft, &rfid, &rgLast);
					if (rft == TC_STOP || r.error)
						break;
					if (rfid == 1 && rft == TC_LIST)	/* columns */
					{
						int			cet;
						uint32		cn = tcr_list_header(&r, &cet);
						uint32		ci;

						rg->chunks = palloc0(sizeof(PqChunk) * Max(cn, 1));
						rg->nchunks = (int) cn;
						for (ci = 0; ci < cn && !r.error; ci++)
							parse_column_chunk(&r, &rg->chunks[ci]);
					}
					else if (rfid == 3)		/* num_rows */
						rg->num_rows = tcr_zigzag(&r);
					else
						tcr_skip(&r, rft);
				}
			}
		}
		else
			tcr_skip(&r, ft);
	}

	if (r.error || pf->nelems < 1)
		return false;

	/* derive leaf columns + their level bounds from the schema tree */
	{
		int			cursor = 1;	/* skip the root element */
		int			nleaf = 0;
		int			c;
		PqLeafInfo *leaves = palloc0(sizeof(PqLeafInfo) * Max(pf->nelems, 1));

		pf->ntop = pf->elems[0].num_children;
		for (c = 0; c < pf->ntop; c++)
			if (!walk_schema(pf, &cursor, 0, 0, leaves, &nleaf))
				return false;
		pf->ncols = nleaf;
		pf->leaves = leaves;
	}
	if (pf->ncols <= 0)
		return false;

	return true;
}

/*
 * Reject a file whose row groups do not carry one column chunk per schema leaf.
 *
 * Every value consumer indexes rgs[].chunks[] by the schema's leaf count, not by
 * whatever the row group happened to carry: pq_read_rows() walks i < ncols and
 * pqfdw_compute_skip() takes chunks[top->firstLeaf]. A row group with a short
 * column list is therefore an out-of-bounds read, and one with no `columns` field
 * at all leaves chunks NULL. A row group that disagrees with the schema is
 * malformed and there is no safe reading of it.
 *
 * This is deliberately separate from parsing, and from the schema itself, so that
 * parquet_schema() still describes such a file. Reporting the schema is exactly
 * what one wants when diagnosing a suspect file, and it touches no chunk.
 */
static void
pq_check_row_groups(const PqFile *pf, const char *path)
{
	int			rg;

	for (rg = 0; rg < pf->nrowgroups; rg++)
		if (pf->rgs[rg].chunks == NULL || pf->rgs[rg].nchunks != pf->ncols)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("Parquet file \"%s\" has a malformed row group", path),
					 errdetail("Row group %d carries %d column chunks, but the schema has %d leaf columns.",
							   rg, pf->rgs[rg].nchunks, pf->ncols)));
}

/* -------------------------------------------------------------------------
 * RLE / bit-packing hybrid decoder (Parquet). Decodes `count` values of
 * bit_width bits into out[]. Used for definition levels and dictionary indices.
 * ------------------------------------------------------------------------- */
static bool
rle_bitpack_decode(const uint8 *buf, size_t len, int bit_width,
				   int count, uint32 *out)
{
	size_t		pos = 0;
	int			produced = 0;

	if (bit_width == 0)
	{
		int			i;

		for (i = 0; i < count; i++)
			out[i] = 0;
		return true;
	}

	while (produced < count)
	{
		uint64		header = 0;
		int			shift = 0;

		while (pos < len)
		{
			uint8		b = buf[pos++];

			header |= (uint64) (b & 0x7f) << shift;
			if ((b & 0x80) == 0)
				break;
			shift += 7;
			if (shift > 63)
				return false;
		}

		if (header & 1)			/* bit-packed run */
		{
			int			ngroups = (int) (header >> 1);
			int			nvals = ngroups * 8;
			int			bytes = ngroups * bit_width;
			int			v;

			if (pos + bytes > len)
				return false;
			for (v = 0; v < nvals && produced < count; v++)
			{
				uint32		val = 0;
				int			bit;

				for (bit = 0; bit < bit_width; bit++)
				{
					int			abs = v * bit_width + bit;

					if (buf[pos + (abs >> 3)] & (1 << (abs & 7)))
						val |= (1u << bit);
				}
				out[produced++] = val;
			}
			pos += bytes;
		}
		else					/* RLE run */
		{
			int			runlen = (int) (header >> 1);
			int			nbytes = (bit_width + 7) / 8;
			uint32		val = 0;
			int			i;

			if (pos + nbytes > len)
				return false;
			for (i = 0; i < nbytes; i++)
				val |= (uint32) buf[pos + i] << (8 * i);
			pos += nbytes;
			for (i = 0; i < runlen && produced < count; i++)
				out[produced++] = val;
		}
	}
	return true;
}

static int
bits_for(int maxval)
{
	int			b = 0;

	while ((1 << b) <= maxval)
		b++;
	return b;
}

/* -------------------------------------------------------------------------
 * Per-column import plan derived from the target tuple descriptor.
 * ------------------------------------------------------------------------- */
typedef struct PqColPlan
{
	Oid			typid;
	int16		typlen;
	bool		typbyval;
	int			expect_phys;	/* required Parquet physical type */
} PqColPlan;

/*
 * A physical value that does not fit the bound PostgreSQL type. On the data path
 * (strict) this is an error: the file is well-formed, the value simply cannot be
 * represented as the column's declared type, and silently substituting a wrapped
 * one would be a wrong answer. On the statistics path (not strict) it only means
 * the bounds cannot be trusted, so the caller declines to skip and reads the group.
 */
static Datum
pq_value_out_of_range(bool strict, bool *ok, int sqlerrcode, const char *typname,
					  int64 value)
{
	if (strict)
		ereport(ERROR,
				(errcode(sqlerrcode),
				 errmsg("value out of range for type %s", typname),
				 errdetail("The Parquet file stores " INT64_FORMAT
						   ", which cannot be represented as %s.",
						   value, typname)));
	*ok = false;
	return (Datum) 0;
}

/*
 * Convert one PLAIN physical value at *p (advancing *p) to a target Datum.
 *
 * Several PostgreSQL types bind to a wider Parquet physical type, because Parquet
 * has no narrower one: int2 and date ride on INT32, time and the timestamps on
 * INT64. Those conversions are range-checked rather than cast, so an out-of-range
 * value raises instead of wrapping. See pq_value_out_of_range for what strict
 * selects.
 */
static Datum
plain_value_to_datum(const PqColPlan *plan, const uint8 **p, const uint8 *end,
					 bool strict, bool *ok)
{
	const uint8 *cur = *p;

	*ok = true;
	switch (plan->expect_phys)
	{
		case PQ_INT32:
			{
				int32		v;

				if (cur + 4 > end)
				{
					*ok = false;
					return (Datum) 0;
				}
				memcpy(&v, cur, 4);
				*p = cur + 4;
				if (plan->typid == INT2OID)
				{
					if (v < PG_INT16_MIN || v > PG_INT16_MAX)
						return pq_value_out_of_range(strict, ok,
													 ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
													 "smallint", (int64) v);
					return Int16GetDatum((int16) v);
				}
				if (plan->typid == DATEOID)
				{
					int32		d;

					if (pg_sub_s32_overflow(v, PG_TO_UNIX_DAYS, &d) ||
						!IS_VALID_DATE((DateADT) d))
						return pq_value_out_of_range(strict, ok,
													 ERRCODE_DATETIME_FIELD_OVERFLOW,
													 "date", (int64) v);
					return DateADTGetDatum((DateADT) d);
				}
				return Int32GetDatum(v);
			}
		case PQ_INT64:
			{
				int64		v;

				if (cur + 8 > end)
				{
					*ok = false;
					return (Datum) 0;
				}
				memcpy(&v, cur, 8);
				*p = cur + 8;
				if (plan->typid == TIMESTAMPOID || plan->typid == TIMESTAMPTZOID)
				{
					int64		t;

					if (pg_sub_s64_overflow(v, PG_TO_UNIX_USECS, &t) ||
						!IS_VALID_TIMESTAMP((Timestamp) t))
						return pq_value_out_of_range(strict, ok,
													 ERRCODE_DATETIME_FIELD_OVERFLOW,
													 plan->typid == TIMESTAMPOID
													 ? "timestamp" : "timestamp with time zone",
													 v);
					return TimestampGetDatum((Timestamp) t);
				}
				if (plan->typid == TIMEOID)
				{
					/* TimeADT is microseconds since midnight; nothing else is a time */
					if (v < INT64CONST(0) || v > USECS_PER_DAY)
						return pq_value_out_of_range(strict, ok,
													 ERRCODE_DATETIME_FIELD_OVERFLOW,
													 "time", v);
					return TimeADTGetDatum((TimeADT) v);
				}
				return Int64GetDatum(v);
			}
		case PQ_FLOAT:
			{
				float4		v;

				if (cur + 4 > end)
				{
					*ok = false;
					return (Datum) 0;
				}
				memcpy(&v, cur, 4);
				*p = cur + 4;
				return Float4GetDatum(v);
			}
		case PQ_DOUBLE:
			{
				float8		v;

				if (cur + 8 > end)
				{
					*ok = false;
					return (Datum) 0;
				}
				memcpy(&v, cur, 8);
				*p = cur + 8;
				return Float8GetDatum(v);
			}
		case PQ_BOOLEAN:
			/* PLAIN booleans are bit-packed; handled by the caller, not here */
			*ok = false;
			return (Datum) 0;
		case PQ_BYTE_ARRAY:
			{
				uint32		blen;
				text	   *t;

				if (cur + 4 > end)
				{
					*ok = false;
					return (Datum) 0;
				}
				memcpy(&blen, cur, 4);
				cur += 4;
				if (cur + blen > end)
				{
					*ok = false;
					return (Datum) 0;
				}
				if (plan->typid == BYTEAOID)
				{
					bytea	   *b = (bytea *) palloc(blen + VARHDRSZ);

					SET_VARSIZE(b, blen + VARHDRSZ);
					memcpy(VARDATA(b), cur, blen);
					*p = cur + blen;
					return PointerGetDatum(b);
				}
				t = cstring_to_text_with_len((const char *) cur, blen);
				*p = cur + blen;
				return PointerGetDatum(t);
			}
		default:
			*ok = false;
			return (Datum) 0;
	}
}

/* parsed Parquet PageHeader (only the fields we use) */
typedef struct PqPageHeader
{
	int			type;
	int			uncompressed_size;
	int			compressed_size;
	int			num_values;		/* data page: values (rows) in the page */
	int			encoding;		/* data page value encoding */
	/* v2 only */
	int			def_levels_len;
	int			rep_levels_len;
	bool		is_compressed;	/* v2; default true */
	bool		is_v2;
} PqPageHeader;

static void
parse_data_page_header(TCReader *r, PqPageHeader *h, bool v2)
{
	int			lastId = 0;

	h->is_v2 = v2;
	h->is_compressed = true;
	for (;;)
	{
		int			ft,
					fid;

		tcr_field(r, &ft, &fid, &lastId);
		if (ft == TC_STOP || r->error)
			break;
		if (!v2)
		{
			switch (fid)
			{
				case 1:
					h->num_values = (int) tcr_zigzag(r);
					break;
				case 2:
					h->encoding = (int) tcr_zigzag(r);
					break;
				default:
					tcr_skip(r, ft);
					break;
			}
		}
		else
		{
			switch (fid)
			{
				case 1:
					h->num_values = (int) tcr_zigzag(r);
					break;
				case 4:
					h->encoding = (int) tcr_zigzag(r);
					break;
				case 5:
					h->def_levels_len = (int) tcr_zigzag(r);
					break;
				case 6:
					h->rep_levels_len = (int) tcr_zigzag(r);
					break;
				case 7:
					h->is_compressed = (ft == TC_BOOL_TRUE);
					break;
				default:
					tcr_skip(r, ft);
					break;
			}
		}
	}
}

/* parse a PageHeader; on return r->pos is just past the header (page data next) */
static void
parse_page_header(TCReader *r, PqPageHeader *h)
{
	int			lastId = 0;

	memset(h, 0, sizeof(*h));
	h->is_compressed = true;
	for (;;)
	{
		int			ft,
					fid;

		tcr_field(r, &ft, &fid, &lastId);
		if (ft == TC_STOP || r->error)
			break;
		switch (fid)
		{
			case 1:
				h->type = (int) tcr_zigzag(r);
				break;
			case 2:
				h->uncompressed_size = (int) tcr_zigzag(r);
				break;
			case 3:
				h->compressed_size = (int) tcr_zigzag(r);
				break;
			case 5:				/* DataPageHeader (v1) */
				parse_data_page_header(r, h, false);
				break;
			case 7:				/* DictionaryPageHeader */
				{
					int			dl = 0;

					for (;;)
					{
						int			dft,
									dfid;

						tcr_field(r, &dft, &dfid, &dl);
						if (dft == TC_STOP || r->error)
							break;
						if (dfid == 1)
							h->num_values = (int) tcr_zigzag(r);
						else
							tcr_skip(r, dft);
					}
					break;
				}
			case 8:				/* DataPageHeaderV2 */
				parse_data_page_header(r, h, true);
				break;
			default:
				tcr_skip(r, ft);
				break;
		}
	}
}

/* decode `n` PLAIN booleans (bit-packed) into Datums */
static void
decode_plain_bools(const uint8 *buf, int n, Datum *out)
{
	int			i;

	for (i = 0; i < n; i++)
		out[i] = BoolGetDatum((buf[i >> 3] >> (i & 7)) & 1);
}

/*
 * Decode a whole column chunk into its Dremel entry sequence: defs[nEntries],
 * reps[nEntries], and vals[nPresent] (present entries, def == max_def, densely
 * packed in order). The caller pre-allocates the arrays to ch->num_values. A
 * flat scalar (max_rep 0, max_def <= 1) yields one entry per row; nested leaves
 * carry rep/def levels the assembler groups into arrays/composites.
 */
static bool
decode_leaf_entries(const uint8 *filebuf, size_t filelen, PqChunk *ch,
					PqColPlan *plan, int max_def, int max_rep,
					uint32 *defs, uint32 *reps, Datum *vals,
					int64 *nEntriesOut, int64 *nPresentOut)
{
	size_t		pos = (size_t) (ch->dict_page_offset ? ch->dict_page_offset
										: ch->data_page_offset);
	int64		nEntries = 0;
	int64		nPresent = 0;
	Datum	   *dict = NULL;
	int			dictCount = 0;

	while (nEntries < ch->num_values && pos < filelen)
	{
		TCReader	hr = {filebuf + pos, filelen - pos, 0, false};
		PqPageHeader h;
		const uint8 *praw;
		size_t		hdrlen;
		StringInfoData dec;

		parse_page_header(&hr, &h);
		if (hr.error)
			return false;
		hdrlen = hr.pos;
		praw = filebuf + pos + hdrlen;
		if (pos + hdrlen + h.compressed_size > filelen)
			return false;

		initStringInfo(&dec);

		if (h.type == PQ_PAGE_DICTIONARY)
		{
			const uint8 *db;
			size_t		dblen;
			const uint8 *p;
			const uint8 *end;
			int			i;

			if (ch->codec == PQC_SNAPPY)
			{
				if (!snappy_raw_uncompress(praw, h.compressed_size, &dec))
					return false;
				db = (const uint8 *) dec.data;
				dblen = dec.len;
			}
			else
			{
				db = praw;
				dblen = h.compressed_size;
			}
			dictCount = h.num_values;
			dict = palloc(sizeof(Datum) * Max(dictCount, 1));
			p = db;
			end = db + dblen;
			if (plan->expect_phys == PQ_BOOLEAN)
				decode_plain_bools(db, dictCount, dict);
			else
				for (i = 0; i < dictCount; i++)
				{
					bool		ok;

					dict[i] = plain_value_to_datum(plan, &p, end, true, &ok);
					if (!ok)
						return false;
				}
			pos += hdrlen + h.compressed_size;
			continue;
		}

		/* a data page (v1 or v2) */
		{
			int			npage = h.num_values;
			uint32	   *pdefs = NULL;
			uint32	   *preps = NULL;
			const uint8 *valbuf;
			size_t		vallen;
			int			i;
			int			nnn;
			Datum	   *pv;

			if (h.is_v2)
			{
				const uint8 *levels = praw;
				int			levLen = h.def_levels_len + h.rep_levels_len;
				const uint8 *vraw = praw + levLen;
				int			vrawlen = h.compressed_size - levLen;

				if (max_rep > 0)
				{
					preps = palloc(sizeof(uint32) * npage);
					if (!rle_bitpack_decode(levels, h.rep_levels_len,
											bits_for(max_rep), npage, preps))
						return false;
				}
				if (max_def > 0)
				{
					pdefs = palloc(sizeof(uint32) * npage);
					if (!rle_bitpack_decode(levels + h.rep_levels_len,
											h.def_levels_len, bits_for(max_def),
											npage, pdefs))
						return false;
				}
				if (ch->codec == PQC_SNAPPY && h.is_compressed)
				{
					if (!snappy_raw_uncompress(vraw, vrawlen, &dec))
						return false;
					valbuf = (const uint8 *) dec.data;
					vallen = dec.len;
				}
				else
				{
					valbuf = vraw;
					vallen = vrawlen;
				}
			}
			else
			{
				const uint8 *pb;
				size_t		pblen;
				size_t		off = 0;

				if (ch->codec == PQC_SNAPPY)
				{
					if (!snappy_raw_uncompress(praw, h.compressed_size, &dec))
						return false;
					pb = (const uint8 *) dec.data;
					pblen = dec.len;
				}
				else
				{
					pb = praw;
					pblen = h.compressed_size;
				}
				/* v1: repetition levels first, then definition levels, both
				 * prefixed with a 4-byte length */
				if (max_rep > 0)
				{
					uint32		llen;

					if (off + 4 > pblen)
						return false;
					memcpy(&llen, pb + off, 4);
					off += 4;
					if (off + llen > pblen)
						return false;
					preps = palloc(sizeof(uint32) * npage);
					if (!rle_bitpack_decode(pb + off, llen, bits_for(max_rep),
											npage, preps))
						return false;
					off += llen;
				}
				if (max_def > 0)
				{
					uint32		llen;

					if (off + 4 > pblen)
						return false;
					memcpy(&llen, pb + off, 4);
					off += 4;
					if (off + llen > pblen)
						return false;
					pdefs = palloc(sizeof(uint32) * npage);
					if (!rle_bitpack_decode(pb + off, llen, bits_for(max_def),
											npage, pdefs))
						return false;
					off += llen;
				}
				valbuf = pb + off;
				vallen = pblen - off;
			}

			if (max_def > 0)
			{
				nnn = 0;
				for (i = 0; i < npage; i++)
					if (pdefs[i] == (uint32) max_def)
						nnn++;
			}
			else
				nnn = npage;

			pv = palloc(sizeof(Datum) * Max(nnn, 1));
			if (h.encoding == PQE_RLE_DICTIONARY || h.encoding == PQE_PLAIN_DICTIONARY)
			{
				uint32	   *idx;
				int			bw;

				if (dict == NULL || vallen < 1)
					return false;
				bw = valbuf[0];
				idx = palloc(sizeof(uint32) * Max(nnn, 1));
				if (!rle_bitpack_decode(valbuf + 1, vallen - 1, bw, nnn, idx))
					return false;
				for (i = 0; i < nnn; i++)
				{
					if ((int) idx[i] >= dictCount)
						return false;
					pv[i] = dict[idx[i]];
				}
			}
			else if (h.encoding == PQE_PLAIN)
			{
				if (plan->expect_phys == PQ_BOOLEAN)
					decode_plain_bools(valbuf, nnn, pv);
				else
				{
					const uint8 *p = valbuf;
					const uint8 *end = valbuf + vallen;

					for (i = 0; i < nnn; i++)
					{
						bool		ok;

						pv[i] = plain_value_to_datum(plan, &p, end, true, &ok);
						if (!ok)
							return false;
					}
				}
			}
			else if (h.encoding == PQE_RLE && plan->expect_phys == PQ_BOOLEAN)
			{
				uint32		rlen;
				uint32	   *bits;

				if (vallen < 4)
					return false;
				memcpy(&rlen, valbuf, 4);
				if ((size_t) rlen + 4 > vallen)
					return false;
				bits = palloc(sizeof(uint32) * Max(nnn, 1));
				if (!rle_bitpack_decode(valbuf + 4, rlen, 1, nnn, bits))
					return false;
				for (i = 0; i < nnn; i++)
					pv[i] = BoolGetDatum(bits[i] != 0);
			}
			else
				return false;	/* unsupported value encoding */

			/* append this page's entries (levels) and present values */
			for (i = 0; i < npage; i++)
			{
				defs[nEntries] = (max_def > 0) ? pdefs[i] : 0;
				reps[nEntries] = (max_rep > 0) ? preps[i] : 0;
				nEntries++;
			}
			for (i = 0; i < nnn; i++)
				vals[nPresent++] = pv[i];
			pos += hdrlen + h.compressed_size;
		}
	}
	*nEntriesOut = nEntries;
	*nPresentOut = nPresent;
	return nEntries == ch->num_values;
}

/*
 * Map a target PostgreSQL scalar type to the Parquet physical type it must have
 * been written as. Returns -1 for an unsupported type.
 */
static int
pq_want_phys(Oid typid)
{
	switch (typid)
	{
		case INT2OID:
		case INT4OID:
		case DATEOID:
			return PQ_INT32;
		case INT8OID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case TIMEOID:
			return PQ_INT64;
		case FLOAT4OID:
			return PQ_FLOAT;
		case FLOAT8OID:
			return PQ_DOUBLE;
		case BOOLOID:
			return PQ_BOOLEAN;
		case TEXTOID:
		case VARCHAROID:
		case BYTEAOID:
			return PQ_BYTE_ARRAY;
		default:
			return -1;
	}
}

/*
 * Infer the PostgreSQL column type a Parquet leaf should map to. This is the
 * inverse of the exporter's parquet_kind_for_type (columnar_parquet.c): it reads
 * the physical type plus the ConvertedType annotation, so a round-tripped file
 * reports the source column types. It is tolerant of files other writers produce
 * (both the millis and micros time/timestamp variants, INT_8/INT_32 widths) so
 * the schema view is useful for foreign Parquet too. Returns true and sets
 * the out-params typid and typmod on success; returns false for a leaf whose
 * type cannot be mapped to a supported scalar (the caller reports it unknown).
 */
static bool
pq_leaf_to_pgtype(const PqSchemaCol *sc, Oid *typid, int32 *typmod)
{
	int			ct = sc->converted_type;

	*typmod = -1;

	switch (sc->phys_type)
	{
		case PQ_BOOLEAN:
			*typid = BOOLOID;
			return true;
		case PQ_INT32:
			if (ct == PQ_CT_DATE)
				*typid = DATEOID;
			else if (ct == PQ_CT_INT_8 || ct == PQ_CT_INT_16)
				*typid = INT2OID;
			else
				*typid = INT4OID;	/* INT_32 or unannotated */
			return true;
		case PQ_INT64:
			if (ct == PQ_CT_TIMESTAMP_MICROS || ct == PQ_CT_TIMESTAMP_MILLIS)
				*typid = TIMESTAMPOID;
			else if (ct == PQ_CT_TIME_MICROS || ct == PQ_CT_TIME_MILLIS)
				*typid = TIMEOID;
			else
				*typid = INT8OID;	/* INT_64 or unannotated */
			return true;
		case PQ_FLOAT:
			*typid = FLOAT4OID;
			return true;
		case PQ_DOUBLE:
			*typid = FLOAT8OID;
			return true;
		case PQ_BYTE_ARRAY:
			/* UTF8/ENUM/JSON annotate string data; anything else is raw bytes */
			if (ct == PQ_CT_UTF8 || ct == PQ_CT_ENUM || ct == PQ_CT_JSON)
				*typid = TEXTOID;
			else
				*typid = BYTEAOID;
			return true;
		case PQ_FIXED_LEN_BYTE_ARRAY:
			if (ct == PQ_CT_DECIMAL && sc->precision >= 1 && sc->precision <= 38 &&
				sc->scale >= 0 && sc->scale <= sc->precision)
			{
				*typid = NUMERICOID;
				*typmod = (int32) (((sc->precision << 16) | (sc->scale & 0xffff)) +
								   VARHDRSZ);
				return true;
			}
			if (ct < 0 && sc->type_length == 16)
			{
				/* the exporter writes uuid as an unannotated 16-byte FLBA */
				*typid = UUIDOID;
				return true;
			}
			*typid = BYTEAOID;
			return true;
		default:
			return false;
	}
}

/* -------------------------------------------------------------------------
 * Nested import assembly. A target column is one of three shapes, mirroring the
 * nested Parquet exporter: a scalar leaf, a 1-D array (LIST of one element leaf),
 * or a composite (group of scalar field leaves). Each leaf is decoded into its
 * full Dremel entry sequence (defs/reps/dense values), then rows are assembled
 * by walking the entries and grouping repeated runs (rep > 0) into arrays.
 * ------------------------------------------------------------------------- */
typedef enum
{
	IMP_SCALAR,
	IMP_LIST,
	IMP_STRUCT
}			ImpKind;

/* one primitive leaf column: decoding plan + decoded entry stream + cursors */
typedef struct ImpLeaf
{
	PqColPlan	plan;
	int			max_def;
	int			max_rep;
	/* decoded per row group */
	uint32	   *defs;
	uint32	   *reps;
	Datum	   *vals;
	int64		nEntries;
	int64		nPresent;
	int64		ei;				/* entry cursor */
	int64		vi;				/* present-value cursor */
}			ImpLeaf;

typedef struct ImpTop
{
	ImpKind		kind;
	int			attno;			/* target attribute index (0-based) */
	int			firstLeaf;		/* index into leaves[] */
	int			nleaves;
	/* IMP_LIST */
	Oid			elemtype;
	int16		elemlen;
	bool		elembyval;
	char		elemalign;
	/* IMP_STRUCT */
	TupleDesc	structDesc;
	int		   *fieldLeaf;		/* [structDesc->natts] leaf index or -1 */
}			ImpTop;

/*
 * Build the target tree from the tuple descriptor and bind each leaf to a
 * Parquet leaf column (validating physical type and Dremel level bounds).
 * Returns the tops array; sets the leaves array and top count via out-params.
 *
 * Relation-free: on a binding error it just ereports, and any relation the caller
 * holds is released by transaction abort. This lets a caller with no relation (the
 * read_parquet function, the FDW) bind a descriptor against a file exactly as
 * import binds a target table's descriptor.
 */
static ImpTop *
build_imp_targets(TupleDesc tupdesc, PqFile *pf,
				  ImpLeaf **pleaves, int *ntops)
{
	int			natts = tupdesc->natts;
	ImpTop	   *tops = palloc0(sizeof(ImpTop) * Max(natts, 1));
	ImpLeaf    *leaves = palloc0(sizeof(ImpLeaf) * Max(pf->ncols, 1));
	int			nt = 0;
	int			lf = 0;
	int			i;

#define IMP_FAIL(...) \
	ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH), errmsg(__VA_ARGS__)))

	for (i = 0; i < natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);
		Oid			typid;
		Oid			elemtype;
		ImpTop	   *t;

		if (att->attisdropped)
			continue;
		typid = att->atttypid;
		t = &tops[nt];
		t->attno = i;
		t->firstLeaf = lf;

		elemtype = get_element_type(typid);
		if (OidIsValid(elemtype))
		{
			/* 1-D array -> LIST(element) */
			int			want = pq_want_phys(elemtype);
			ImpLeaf    *l = &leaves[lf];

			if (want < 0)
				IMP_FAIL("array column \"%s\" has element type %s, which columnar.import_parquet does not support",
						 NameStr(att->attname), format_type_be(elemtype));
			if (lf >= pf->ncols)
				IMP_FAIL("Parquet file has fewer columns than the target table");
			if (pf->leaves[lf].max_rep < 1)
				IMP_FAIL("target column \"%s\" is an array but the Parquet column is not repeated",
						 NameStr(att->attname));
			if (pf->leaves[lf].sc->phys_type != want)
				IMP_FAIL("Parquet column for array \"%s\" has incompatible physical type",
						 NameStr(att->attname));
			t->kind = IMP_LIST;
			t->nleaves = 1;
			t->elemtype = elemtype;
			get_typlenbyvalalign(elemtype, &t->elemlen, &t->elembyval, &t->elemalign);
			l->plan.typid = elemtype;
			get_typlenbyval(elemtype, &l->plan.typlen, &l->plan.typbyval);
			l->plan.expect_phys = want;
			l->max_def = pf->leaves[lf].max_def;
			l->max_rep = pf->leaves[lf].max_rep;
			lf++;
		}
		else if (get_typtype(typid) == TYPTYPE_COMPOSITE)
		{
			/* composite -> group of scalar field leaves */
			TupleDesc	td = lookup_rowtype_tupdesc(typid, att->atttypmod);
			int			a;

			t->kind = IMP_STRUCT;
			t->structDesc = CreateTupleDescCopy(td);
			t->fieldLeaf = palloc(sizeof(int) * td->natts);
			for (a = 0; a < td->natts; a++)
			{
				Form_pg_attribute fa = TupleDescAttr(td, a);
				int			want;
				ImpLeaf    *l;

				if (fa->attisdropped)
				{
					t->fieldLeaf[a] = -1;
					continue;
				}
				want = pq_want_phys(fa->atttypid);
				if (want < 0)
				{
					ReleaseTupleDesc(td);
					IMP_FAIL("composite column \"%s\" field \"%s\" has type %s, which columnar.import_parquet does not support",
							 NameStr(att->attname), NameStr(fa->attname),
							 format_type_be(fa->atttypid));
				}
				if (lf >= pf->ncols)
				{
					ReleaseTupleDesc(td);
					IMP_FAIL("Parquet file has fewer columns than the target table");
				}
				if (pf->leaves[lf].max_rep != 0)
				{
					ReleaseTupleDesc(td);
					IMP_FAIL("Parquet column for composite field \"%s\" is unexpectedly repeated",
							 NameStr(fa->attname));
				}
				if (pf->leaves[lf].sc->phys_type != want)
				{
					ReleaseTupleDesc(td);
					IMP_FAIL("Parquet column for composite field \"%s\" has incompatible physical type",
							 NameStr(fa->attname));
				}
				l = &leaves[lf];
				l->plan.typid = fa->atttypid;
				get_typlenbyval(fa->atttypid, &l->plan.typlen, &l->plan.typbyval);
				l->plan.expect_phys = want;
				l->max_def = pf->leaves[lf].max_def;
				l->max_rep = pf->leaves[lf].max_rep;
				t->fieldLeaf[a] = lf;
				lf++;
			}
			t->nleaves = lf - t->firstLeaf;
			ReleaseTupleDesc(td);
		}
		else
		{
			/* plain scalar */
			int			want = pq_want_phys(typid);
			ImpLeaf    *l = &leaves[lf];

			if (want < 0)
				IMP_FAIL("column \"%s\" has type %s, which columnar.import_parquet does not support",
						 NameStr(att->attname), format_type_be(typid));
			if (lf >= pf->ncols)
				IMP_FAIL("Parquet file has fewer columns than the target table");
			if (pf->leaves[lf].max_rep != 0)
				IMP_FAIL("Parquet column for scalar \"%s\" is unexpectedly repeated",
						 NameStr(att->attname));
			if (pf->leaves[lf].sc->phys_type != want)
				IMP_FAIL("Parquet column %d physical type is not compatible with target column \"%s\" (%s)",
						 lf, NameStr(att->attname), format_type_be(typid));
			t->kind = IMP_SCALAR;
			t->nleaves = 1;
			l->plan.typid = typid;
			get_typlenbyval(typid, &l->plan.typlen, &l->plan.typbyval);
			l->plan.expect_phys = want;
			l->max_def = pf->leaves[lf].max_def;
			l->max_rep = pf->leaves[lf].max_rep;
			lf++;
		}
		nt++;
	}

	if (lf != pf->ncols)
		IMP_FAIL("Parquet file has %d leaf columns, target table expands to %d",
				 pf->ncols, lf);
#undef IMP_FAIL

	*pleaves = leaves;
	*ntops = nt;
	return tops;
}

/*
 * Read an entire server-side Parquet file into a palloc'd buffer and parse its
 * footer metadata into *pf. On success the file bytes are returned in *bufOut
 * (length *lenOut), allocated in the caller's memory context. This never returns
 * on failure: any open/size/read/format/metadata error is reported with ereport,
 * so the caller does not need to check a return value or clean the buffer up.
 * The caller is responsible for any privilege check before calling.
 */
static void
pq_slurp_and_parse(const char *path, uint8 **bufOut, long *lenOut, PqFile *pf)
{
	FILE	   *f;
	long		filelen;
	uint8	   *filebuf;
	uint32		metalen;

	f = AllocateFile(path, PG_BINARY_R);
	if (f == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\" for reading: %m", path)));
	if (fseek(f, 0, SEEK_END) != 0 || (filelen = ftell(f)) < 0)
	{
		FreeFile(f);
		ereport(ERROR, (errcode_for_file_access(), errmsg("could not size \"%s\": %m", path)));
	}
	if (filelen < 12)
	{
		FreeFile(f);
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("\"%s\" is not a Parquet file", path)));
	}
	filebuf = (uint8 *) palloc(filelen);
	if (fseek(f, 0, SEEK_SET) != 0 ||
		fread(filebuf, 1, filelen, f) != (size_t) filelen)
	{
		FreeFile(f);
		ereport(ERROR, (errcode_for_file_access(), errmsg("could not read \"%s\": %m", path)));
	}
	FreeFile(f);

	if (memcmp(filebuf, "PAR1", 4) != 0 ||
		memcmp(filebuf + filelen - 4, "PAR1", 4) != 0)
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
						errmsg("\"%s\" is not a Parquet file (bad magic)", path)));
	memcpy(&metalen, filebuf + filelen - 8, 4);
	if ((long) metalen + 8 > filelen)
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
						errmsg("\"%s\" has a corrupt Parquet footer", path)));

	if (!parse_file_metadata(filebuf + filelen - 8 - metalen, metalen, pf))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("could not parse \"%s\" (unsupported or corrupt Parquet metadata)", path)));

	*bufOut = filebuf;
	*lenOut = filelen;
}

/*
 * Row sink for the shared reader: called once per assembled row with the row in
 * slot (already ExecStoreVirtualTuple'd). The sink must copy the row out, because
 * the caller resets the per-row memory immediately after; both table_tuple_insert
 * and tuplestore_puttupleslot do.
 */
typedef void (*PqRowSink) (TupleTableSlot *slot, void *arg);

typedef struct PqInsertSinkArg
{
	Relation	rel;
	CommandId	cid;
}			PqInsertSinkArg;

static void
pq_insert_sink(TupleTableSlot *slot, void *arg)
{
	PqInsertSinkArg *a = (PqInsertSinkArg *) arg;

	table_tuple_insert(a->rel, slot, a->cid, 0, NULL);
}

static void
pq_tuplestore_sink(TupleTableSlot *slot, void *arg)
{
	tuplestore_puttupleslot((Tuplestorestate *) arg, slot);
}

/*
 * The shared row-producing core (Phase G scan core). For each row group it decodes
 * every leaf column into its Dremel entry stream, then assembles each row into slot
 * per the bound target tree (scalar, 1-D array from a LIST, composite from a group)
 * and hands it to sink. Returns the number of rows produced.
 *
 * groupCtx bounds each group's decoded streams; rowCtx bounds each row's transient
 * arrays/composites and is reset after the sink copies the row, so a large file
 * never accumulates O(rows) memory. Both import (insert sink) and read_parquet
 * (tuplestore sink) run through here, so their row semantics are identical.
 */
static int64
pq_read_rows(PqFile *pf, const uint8 *filebuf, long filelen,
			 ImpTop *tops, int ntops, ImpLeaf *leaves,
			 TupleTableSlot *slot, PqRowSink sink, void *sinkarg,
			 const bool *skipGroup)
{
	int			natts = slot->tts_tupleDescriptor->natts;
	MemoryContext groupCtx;
	MemoryContext rowCtx;
	int64		total = 0;
	int			i;
	int			rg;

	groupCtx = AllocSetContextCreate(CurrentMemoryContext,
									 "columnar parquet read group",
									 ALLOCSET_DEFAULT_SIZES);
	rowCtx = AllocSetContextCreate(CurrentMemoryContext,
								   "columnar parquet read row",
								   ALLOCSET_DEFAULT_SIZES);

	for (rg = 0; rg < pf->nrowgroups; rg++)
	{
		PqRowGroup *g = &pf->rgs[rg];
		int64		n = g->num_rows;
		int64		r;
		int			t;
		MemoryContext oldCtx;

		/* row-group skipping (predicate pushdown): the group cannot match, so
		 * decode nothing and emit nothing. The executor still rechecks quals on
		 * the rows we do emit, so a missed skip only costs work, never rows. */
		if (skipGroup != NULL && skipGroup[rg])
			continue;

		/* decode every leaf column of this row group into its entry stream */
		MemoryContextReset(groupCtx);
		oldCtx = MemoryContextSwitchTo(groupCtx);
		for (i = 0; i < pf->ncols; i++)
		{
			ImpLeaf    *l = &leaves[i];
			PqChunk    *ch = &g->chunks[i];
			int64		cap = Max(ch->num_values, 1);

			l->defs = palloc(sizeof(uint32) * cap);
			l->reps = palloc(sizeof(uint32) * cap);
			l->vals = palloc(sizeof(Datum) * cap);
			l->ei = 0;
			l->vi = 0;
			if (!decode_leaf_entries(filebuf, filelen, ch, &l->plan,
									 l->max_def, l->max_rep,
									 l->defs, l->reps, l->vals,
									 &l->nEntries, &l->nPresent))
			{
				MemoryContextSwitchTo(oldCtx);
				ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
								errmsg("could not decode Parquet column %d in row group %d",
									   i, rg)));
			}
		}
		MemoryContextSwitchTo(oldCtx);

		for (r = 0; r < n; r++)
		{
			MemoryContext rowOld;

			ExecClearTuple(slot);
			for (i = 0; i < natts; i++)
				slot->tts_isnull[i] = true;

			rowOld = MemoryContextSwitchTo(rowCtx);
			for (t = 0; t < ntops; t++)
			{
				ImpTop	   *tp = &tops[t];

				if (tp->kind == IMP_SCALAR)
				{
					ImpLeaf    *l = &leaves[tp->firstLeaf];
					bool		present = (l->defs[l->ei] == (uint32) l->max_def);

					slot->tts_values[tp->attno] = present ? l->vals[l->vi++] : (Datum) 0;
					slot->tts_isnull[tp->attno] = !present;
					l->ei++;
				}
				else if (tp->kind == IMP_LIST)
				{
					ImpLeaf    *l = &leaves[tp->firstLeaf];
					uint32		def0 = l->defs[l->ei];

					if (def0 == 0)
					{
						/* NULL array */
						slot->tts_isnull[tp->attno] = true;
						l->ei++;
					}
					else if (def0 == 1)
					{
						/* empty array */
						ArrayType  *arr = construct_empty_array(tp->elemtype);

						slot->tts_values[tp->attno] = PointerGetDatum(arr);
						slot->tts_isnull[tp->attno] = false;
						l->ei++;
					}
					else
					{
						/* one or more elements (rep marks continuation) */
						Datum	   *elems = palloc(sizeof(Datum) * Max(l->nEntries - l->ei, 1));
						bool	   *enulls = palloc(sizeof(bool) * Max(l->nEntries - l->ei, 1));
						int			k = 0;
						int			dims[1];
						int			lbs[1] = {1};
						ArrayType  *arr;

						do
						{
							uint32		d = l->defs[l->ei];

							if (d == (uint32) l->max_def)
							{
								elems[k] = l->vals[l->vi++];
								enulls[k] = false;
							}
							else
							{
								elems[k] = (Datum) 0;
								enulls[k] = true;
							}
							k++;
							l->ei++;
						} while (l->ei < l->nEntries && l->reps[l->ei] != 0);

						dims[0] = k;
						arr = construct_md_array(elems, enulls, 1, dims, lbs,
												 tp->elemtype, tp->elemlen,
												 tp->elembyval, tp->elemalign);
						slot->tts_values[tp->attno] = PointerGetDatum(arr);
						slot->tts_isnull[tp->attno] = false;
					}
				}
				else			/* IMP_STRUCT */
				{
					TupleDesc	sd = tp->structDesc;
					Datum	   *fv = palloc(sizeof(Datum) * sd->natts);
					bool	   *fn = palloc(sizeof(bool) * sd->natts);
					bool		structNull = false;
					int			a;

					for (a = 0; a < sd->natts; a++)
					{
						int			li = tp->fieldLeaf[a];
						ImpLeaf    *l;
						uint32		d;

						if (li < 0)
						{
							fv[a] = (Datum) 0;
							fn[a] = true;
							continue;
						}
						l = &leaves[li];
						d = l->defs[l->ei];
						if (d == 0)
							structNull = true;	/* every field agrees */
						if (d == (uint32) l->max_def)
						{
							fv[a] = l->vals[l->vi++];
							fn[a] = false;
						}
						else
						{
							fv[a] = (Datum) 0;
							fn[a] = true;
						}
						l->ei++;
					}

					if (structNull)
						slot->tts_isnull[tp->attno] = true;
					else
					{
						HeapTuple	htup = heap_form_tuple(sd, fv, fn);

						slot->tts_values[tp->attno] = HeapTupleGetDatum(htup);
						slot->tts_isnull[tp->attno] = false;
					}
				}
			}

			ExecStoreVirtualTuple(slot);
			sink(slot, sinkarg);
			MemoryContextSwitchTo(rowOld);
			MemoryContextReset(rowCtx);
			total++;
			CHECK_FOR_INTERRUPTS();
		}
	}

	MemoryContextDelete(groupCtx);
	MemoryContextDelete(rowCtx);
	return total;
}

/*
 * columnar.import_parquet(rel regclass, path text) -> bigint
 */
Datum
columnar_import_parquet(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	char	   *path = text_to_cstring(PG_GETARG_TEXT_PP(1));
	Relation	rel;
	TupleDesc	tupdesc;
	long		filelen;
	uint8	   *filebuf;
	PqFile		pf;
	ImpTop	   *tops;
	ImpLeaf    *leaves;
	int			ntops;
	TupleTableSlot *slot;
	PqInsertSinkArg sinkarg;
	int64		total;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("columnar.import_parquet requires superuser (reads a server-side file)")));

	/* read + parse the file before taking the table lock (no lock held on I/O) */
	pq_slurp_and_parse(path, &filebuf, &filelen, &pf);
	pq_check_row_groups(&pf, path);

	rel = table_open(relid, RowExclusiveLock);
	tupdesc = RelationGetDescr(rel);

	/* build the target tree and bind each Parquet leaf column to it */
	tops = build_imp_targets(tupdesc, &pf, &leaves, &ntops);

	slot = table_slot_create(rel, NULL);

	sinkarg.rel = rel;
	sinkarg.cid = GetCurrentCommandId(true);
	total = pq_read_rows(&pf, filebuf, filelen, tops, ntops, leaves,
						 slot, pq_insert_sink, &sinkarg, NULL);

	ExecDropSingleTupleTableSlot(slot);
	table_close(rel, RowExclusiveLock);

	PG_RETURN_INT64(total);
}

/*
 * columnar.read_parquet(path text) returns setof record
 *
 * Stream a server-side Parquet file's rows in place, without importing. The caller
 * supplies a column definition list:
 *
 *   SELECT * FROM pgcolumnar.read_parquet('/data/f.parquet') AS t(id int, name text);
 *
 * The declared descriptor is bound against the file's leaf columns by position,
 * exactly as import binds a target table's descriptor (same type-compatibility
 * rules, same "declared column count must equal the file's" contract), and rows are
 * produced through the shared scan core. Superuser only (reads a server-side file);
 * materialize-mode SRF.
 */
Datum
columnar_read_parquet(PG_FUNCTION_ARGS)
{
	char	   *path = text_to_cstring(PG_GETARG_TEXT_PP(0));
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	retdesc;
	long		filelen;
	uint8	   *filebuf;
	PqFile		pf;
	ImpTop	   *tops;
	ImpLeaf    *leaves;
	int			ntops;
	Tuplestorestate *tupstore;
	TupleTableSlot *slot;
	MemoryContext oldContext;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("pgcolumnar.read_parquet requires superuser (reads a server-side file)")));

	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo) ||
		!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));

	/* the caller's column definition list defines the output columns and types */
	if (get_call_result_type(fcinfo, NULL, &retdesc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("pgcolumnar.read_parquet requires a column definition list"),
				 errhint("Call it as SELECT * FROM pgcolumnar.read_parquet(path) AS t(col1 type1, ...).")));

	pq_slurp_and_parse(path, &filebuf, &filelen, &pf);
	pq_check_row_groups(&pf, path);

	/* bind the declared descriptor against the file (validates types + count) */
	tops = build_imp_targets(retdesc, &pf, &leaves, &ntops);

	oldContext = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);
	retdesc = CreateTupleDescCopy(retdesc);
	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = retdesc;
	MemoryContextSwitchTo(oldContext);

	slot = MakeSingleTupleTableSlot(retdesc, &TTSOpsVirtual);
	(void) pq_read_rows(&pf, filebuf, filelen, tops, ntops, leaves,
						slot, pq_tuplestore_sink, tupstore, NULL);
	ExecDropSingleTupleTableSlot(slot);

	PG_RETURN_NULL();
}

/*
 * columnar.parquet_schema(path text)
 *     -> table(column_name text, data_type text, nullable bool)
 *
 * Read a server-side Parquet file's footer and report its leaf columns with the
 * PostgreSQL type each maps to (see pq_leaf_to_pgtype). This is the schema half
 * of the external-Parquet scan core: it shares the file open/parse and type
 * inference the data path will use, and lets a caller inspect a file (and the
 * round-trip test confirm exported types) without importing it. A leaf whose
 * physical type has no supported mapping is reported with data_type NULL. Nested
 * files are reported as their flattened leaf columns.
 */
Datum
columnar_parquet_schema(PG_FUNCTION_ARGS)
{
	char	   *path = text_to_cstring(PG_GETARG_TEXT_PP(0));
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	long		filelen;
	uint8	   *filebuf;
	PqFile		pf;
	TupleDesc	retdesc;
	Tuplestorestate *tupstore;
	MemoryContext oldContext;
	int			i;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("columnar.parquet_schema requires superuser (reads a server-side file)")));

	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo) ||
		!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));

	pq_slurp_and_parse(path, &filebuf, &filelen, &pf);

	retdesc = CreateTemplateTupleDesc(3);
	TupleDescInitEntry(retdesc, 1, "column_name", TEXTOID, -1, 0);
	TupleDescInitEntry(retdesc, 2, "data_type", TEXTOID, -1, 0);
	TupleDescInitEntry(retdesc, 3, "nullable", BOOLOID, -1, 0);

	oldContext = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);
	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = retdesc;
	MemoryContextSwitchTo(oldContext);

	for (i = 0; i < pf.ncols; i++)
	{
		PqSchemaCol *sc = pf.leaves[i].sc;
		Oid			typid;
		int32		typmod;
		Datum		values[3];
		bool		nulls[3] = {false, false, false};

		values[0] = CStringGetTextDatum(sc->name != NULL ? sc->name : "");
		if (pq_leaf_to_pgtype(sc, &typid, &typmod))
			values[1] = CStringGetTextDatum(
				format_type_extended(typid, typmod, FORMAT_TYPE_TYPEMOD_GIVEN));
		else
			nulls[1] = true;
		/* a column is nullable iff it is OPTIONAL somewhere above the leaf */
		values[2] = BoolGetDatum(pf.leaves[i].max_def > 0);

		tuplestore_putvalues(tupstore, retdesc, values, nulls);
	}

	PG_RETURN_NULL();
}

/* -------------------------------------------------------------------------
 * Parquet foreign-data wrapper (Phase G). A foreign table over a single Parquet
 * file: its column definitions are bound against the file's leaves exactly as
 * read_parquet's column list is, and rows are produced through the same scan
 * core. Read-only, superuser (reads a server-side file). The single required
 * table option is "path". Projection and predicate pushdown are a follow-on; this
 * surface returns full rows and lets the executor project and filter.
 *
 *   CREATE SERVER pq FOREIGN DATA WRAPPER pgcolumnar_parquet;
 *   CREATE FOREIGN TABLE ft (id int, name text) SERVER pq
 *       OPTIONS (path '/data/f.parquet');
 * ------------------------------------------------------------------------- */

/* per-scan state: the whole file materialized into a tuplestore, drained by
 * IterateForeignScan. Eager (reads the file up front); streaming is a follow-on.
 * readslot is a minimal-tuple slot the tuplestore drains into; each row is then
 * copied into the scan slot, whose ops are not guaranteed to be minimal-tuple. */
typedef struct PqFdwScanState
{
	Tuplestorestate *tupstore;
	TupleTableSlot *readslot;
	int			groupsTotal;	/* row groups in the file */
	int			groupsSkipped;	/* skipped by predicate pushdown */
}			PqFdwScanState;

/* the "path" option of a foreign table, or NULL if unset */
static char *
pqfdw_get_path(Oid foreigntableid)
{
	ForeignTable *ft = GetForeignTable(foreigntableid);
	ListCell   *lc;

	foreach(lc, ft->options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "path") == 0)
			return defGetString(def);
	}
	return NULL;
}

static void
pqfdwGetForeignRelSize(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid)
{
	char	   *path = pqfdw_get_path(foreigntableid);
	struct stat st;
	double		rows = 1000.0;

	/*
	 * Estimate row count from the file size without reading it: a generic
	 * bytes-per-row divisor. Only a planning ballpark; the executor reads the
	 * real rows. The stat() is gated on superuser -- the same bar the scan
	 * enforces -- so a non-privileged planner cannot use the EXPLAIN estimate to
	 * probe whether a server-side path exists or how big it is.
	 */
	if (superuser() && path != NULL && stat(path, &st) == 0 && st.st_size > 0)
		rows = Max(1.0, (double) st.st_size / 64.0);
	baserel->rows = rows;
}

static void
pqfdwGetForeignPaths(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid)
{
	Cost		startup_cost = 0;
	Cost		total_cost = baserel->rows;

	add_path(baserel, (Path *)
			 COLUMNAR_CREATE_FOREIGNSCAN_PATH(root, baserel,
											  NULL, /* default pathtarget */
											  baserel->rows,
											  startup_cost, total_cost,
											  NIL,	/* no pathkeys */
											  NULL, /* no outer rel */
											  NULL, /* no extra plan */
											  NIL));
}

static ForeignScan *
pqfdwGetForeignPlan(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid,
					ForeignPath *best_path, List *tlist, List *scan_clauses,
					Plan *outer_plan)
{
	/*
	 * Clause evaluation stays with the executor; pushdown here is row-group
	 * skipping only (see pqfdw_compute_skip), so every clause is still recheckable
	 * and must remain in the plan's qual.
	 */
	scan_clauses = extract_actual_clauses(scan_clauses, false);
	return make_foreignscan(tlist, scan_clauses, baserel->relid,
							NIL, NIL, NIL, NIL, outer_plan);
}

/* the top (target column) bound to attribute attno0, or NULL */
static ImpTop *
pqfdw_top_for_attno(ImpTop *tops, int ntops, int attno0)
{
	int			t;

	for (t = 0; t < ntops; t++)
		if (tops[t].attno == attno0)
			return &tops[t];
	return NULL;
}

/*
 * Decide whether a single row group can be skipped for the qual `col op const`
 * (with the Var on varLeft). Conservative: returns true only when the group's
 * min/max prove no row can match. Anything uncertain -- missing stats, an
 * unsupported physical type, a mismatched constant type, a non-btree operator --
 * returns false (do not skip). Restricted to fixed-width ordered physical types,
 * whose statistics decode unambiguously.
 */
static bool
pqfdw_clause_excludes_group(ImpLeaf *leaf, PqChunk *ch, int strategy,
							bool varLeft, Const *con)
{
	TypeCacheEntry *tce;
	const uint8 *p;
	bool		ok;
	Datum		minD;
	Datum		maxD;
	int			phys = leaf->plan.expect_phys;

	if (!ch->has_min || !ch->has_max)
		return false;
	/* only fixed-width ordered stats decode without ambiguity */
	if (phys != PQ_INT32 && phys != PQ_INT64 &&
		phys != PQ_FLOAT && phys != PQ_DOUBLE)
		return false;
	if (con->consttype != leaf->plan.typid || con->constisnull)
		return false;

	tce = lookup_type_cache(leaf->plan.typid, TYPECACHE_CMP_PROC_FINFO);
	if (!OidIsValid(tce->cmp_proc_finfo.fn_oid))
		return false;

	p = ch->stat_min;
	minD = plain_value_to_datum(&leaf->plan, &p, p + ch->stat_min_len, false, &ok);
	if (!ok)
		return false;
	p = ch->stat_max;
	maxD = plain_value_to_datum(&leaf->plan, &p, p + ch->stat_max_len, false, &ok);
	if (!ok)
		return false;

#define PQ_CMP(a, b) \
	DatumGetInt32(FunctionCall2Coll(&tce->cmp_proc_finfo, InvalidOid, (a), (b)))

	/*
	 * Every test below assumes min <= max, and the decoded statistics may not
	 * satisfy that. plain_value_to_datum() narrows a Parquet INT32 to int16 when
	 * the column is bound to a PG int2, so a group holding 30000 and 40000
	 * decodes to min=30000, max=-25536. Parquet's UINT_32/UINT_64 logical types
	 * sort unsigned while we decode signed, so any group straddling the sign
	 * boundary inverts the same way, as can a corrupt file. Refuse to skip on an
	 * interval we cannot trust: the data path applies the identical narrowing, so
	 * those rows are real matches and dropping them would be an unsound skip that
	 * the executor's recheck can never recover.
	 */
	if (PQ_CMP(minD, maxD) > 0)
		return false;

	/* normalize `const op var` to `var op' const` by flipping the strategy */
	if (!varLeft)
	{
		switch (strategy)
		{
			case BTLessStrategyNumber:
				strategy = BTGreaterStrategyNumber;
				break;
			case BTLessEqualStrategyNumber:
				strategy = BTGreaterEqualStrategyNumber;
				break;
			case BTGreaterStrategyNumber:
				strategy = BTLessStrategyNumber;
				break;
			case BTGreaterEqualStrategyNumber:
				strategy = BTLessEqualStrategyNumber;
				break;
				/* BTEqual is symmetric */
		}
	}

	/*
	 * Float and double statistics do not describe NaN. parquet.thrift's
	 * TypeDefinedOrder compatibility rules exclude NaN from min/max, and require
	 * that a NaN-valued min or max be ignored outright. PostgreSQL instead orders
	 * NaN above every other value and treats NaN = NaN as true, so a group that
	 * holds a NaN advertises finite bounds while its NaN row still satisfies
	 * col > c, col >= c and col = 'NaN'. Skipping on those would drop rows that
	 * never reach the executor's recheck, so refuse them. col < c and col <= c
	 * stay sound, because a NaN row cannot satisfy those either.
	 *
	 * The +/-0 clauses in the same spec block need no handling: PostgreSQL
	 * compares -0 and +0 as equal, which is exactly what those rules require.
	 */
	if (phys == PQ_FLOAT || phys == PQ_DOUBLE)
	{
		float8		lo = (phys == PQ_FLOAT)
			? (float8) DatumGetFloat4(minD) : DatumGetFloat8(minD);
		float8		hi = (phys == PQ_FLOAT)
			? (float8) DatumGetFloat4(maxD) : DatumGetFloat8(maxD);
		float8		c = (phys == PQ_FLOAT)
			? (float8) DatumGetFloat4(con->constvalue)
			: DatumGetFloat8(con->constvalue);

		if (isnan(lo) || isnan(hi))
			return false;
		if (strategy == BTGreaterStrategyNumber ||
			strategy == BTGreaterEqualStrategyNumber)
			return false;
		if (strategy == BTEqualStrategyNumber && isnan(c))
			return false;
	}

	switch (strategy)
	{
		case BTLessStrategyNumber:	/* col < const: skip if min >= const */
			return PQ_CMP(minD, con->constvalue) >= 0;
		case BTLessEqualStrategyNumber: /* col <= const: skip if min > const */
			return PQ_CMP(minD, con->constvalue) > 0;
		case BTEqualStrategyNumber: /* col = const: skip if const outside [min,max] */
			return PQ_CMP(con->constvalue, minD) < 0 ||
				PQ_CMP(con->constvalue, maxD) > 0;
		case BTGreaterEqualStrategyNumber:	/* col >= const: skip if max < const */
			return PQ_CMP(maxD, con->constvalue) < 0;
		case BTGreaterStrategyNumber:	/* col > const: skip if max <= const */
			return PQ_CMP(maxD, con->constvalue) <= 0;
		default:
			return false;
	}
#undef PQ_CMP
}

/*
 * Compute the per-row-group skip mask from the scan's restriction clauses and the
 * file's statistics. skipGroup[rg] becomes true when some pushable `col op const`
 * clause proves the group empty. Returns the number of groups marked skippable.
 *
 * Only Const operands are pushable, so the mask cannot change between rescans;
 * that is what lets BeginForeignScan compute it once and ReScanForeignScan merely
 * rewind. Adding Param support would invalidate that and require recomputing the
 * mask (and re-reading the file) on each rescan.
 *
 * leaves[] and rgs[rg].chunks[] are both indexed by top->firstLeaf. That is valid
 * because build_imp_targets() binds leaves strictly positionally against
 * pf->leaves[] and errors unless it consumes exactly pf->ncols of them, which is
 * the same identity pq_read_rows() already relies on.
 */
static int
pqfdw_compute_skip(ForeignScanState *node, PqFile *pf,
				   ImpTop *tops, int ntops, ImpLeaf *leaves, bool *skipGroup)
{
	ForeignScan *fs = (ForeignScan *) node->ss.ps.plan;
	List	   *quals = fs->scan.plan.qual;
	int			skipped = 0;
	int			rg;

	for (rg = 0; rg < pf->nrowgroups; rg++)
	{
		ListCell   *lc;

		skipGroup[rg] = false;
		foreach(lc, quals)
		{
			OpExpr	   *op = (OpExpr *) lfirst(lc);
			Node	   *larg;
			Node	   *rarg;
			Var		   *var;
			Const	   *con;
			bool		varLeft;
			ImpTop	   *top;
			List	   *interp;
			ListCell   *ic;
			int			strategy = 0;

			if (!IsA(op, OpExpr) || list_length(op->args) != 2)
				continue;
			larg = (Node *) linitial(op->args);
			rarg = (Node *) lsecond(op->args);
			if (IsA(larg, Var) && IsA(rarg, Const))
			{
				var = (Var *) larg;
				con = (Const *) rarg;
				varLeft = true;
			}
			else if (IsA(larg, Const) && IsA(rarg, Var))
			{
				var = (Var *) rarg;
				con = (Const *) larg;
				varLeft = false;
			}
			else
				continue;
			if (var->varattno < 1)
				continue;

			top = pqfdw_top_for_attno(tops, ntops, var->varattno - 1);
			if (top == NULL || top->kind != IMP_SCALAR)
				continue;

			/*
			 * First btree comparison interpretation gives the strategy. Any
			 * btree family will do: a given operator means the same comparison
			 * in every family that lists it.
			 */
			interp = ColumnarGetOpInterpretation(op->opno);
			foreach(ic, interp)
			{
				ColumnarOpInterpretation *o = (ColumnarOpInterpretation *) lfirst(ic);
				int			s = ColumnarOpInterpStrategy(o);

				if (s >= BTLessStrategyNumber && s <= BTGreaterStrategyNumber)
				{
					strategy = s;
					break;
				}
			}
			if (strategy == 0)
				continue;

			if (pqfdw_clause_excludes_group(&leaves[top->firstLeaf],
											&pf->rgs[rg].chunks[top->firstLeaf],
											strategy, varLeft, con))
			{
				skipGroup[rg] = true;
				break;
			}
		}
		if (skipGroup[rg])
			skipped++;
	}
	return skipped;
}

static void
pqfdwBeginForeignScan(ForeignScanState *node, int eflags)
{
	Relation	rel = node->ss.ss_currentRelation;
	TupleDesc	tupdesc = RelationGetDescr(rel);
	char	   *path;
	long		filelen;
	uint8	   *filebuf;
	PqFile		pf;
	ImpTop	   *tops;
	ImpLeaf    *leaves;
	int			ntops;
	TupleTableSlot *slot;
	PqFdwScanState *st;
	bool	   *skipGroup;

	/* nothing to do for a plan-only (EXPLAIN without ANALYZE) invocation */
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("pgcolumnar_parquet foreign tables require superuser (read a server-side file)")));

	path = pqfdw_get_path(RelationGetRelid(rel));
	if (path == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_OPTION_NAME_NOT_FOUND),
				 errmsg("foreign table \"%s\" has no \"path\" option",
						RelationGetRelationName(rel))));

	pq_slurp_and_parse(path, &filebuf, &filelen, &pf);
	pq_check_row_groups(&pf, path);
	tops = build_imp_targets(tupdesc, &pf, &leaves, &ntops);

	st = (PqFdwScanState *) palloc0(sizeof(PqFdwScanState));
	/* randomAccess so ReScan can rewind the materialized rows */
	st->tupstore = tuplestore_begin_heap(true, false, work_mem);
	st->readslot = MakeSingleTupleTableSlot(tupdesc, &TTSOpsMinimalTuple);
	st->groupsTotal = pf.nrowgroups;

	/* predicate pushdown: mark row groups the quals prove empty */
	skipGroup = (bool *) palloc0(sizeof(bool) * Max(pf.nrowgroups, 1));
	st->groupsSkipped = pqfdw_compute_skip(node, &pf, tops, ntops, leaves, skipGroup);

	slot = MakeSingleTupleTableSlot(tupdesc, &TTSOpsVirtual);
	(void) pq_read_rows(&pf, filebuf, filelen, tops, ntops, leaves,
						slot, pq_tuplestore_sink, st->tupstore, skipGroup);
	ExecDropSingleTupleTableSlot(slot);

	node->fdw_state = st;
}

static TupleTableSlot *
pqfdwIterateForeignScan(ForeignScanState *node)
{
	PqFdwScanState *st = (PqFdwScanState *) node->fdw_state;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
	MemoryContext oldcxt;
	bool		got;

	if (st == NULL)
		return ExecClearTuple(slot);

	/*
	 * Drain one row into our minimal-tuple readslot, then copy it into the scan
	 * slot (a heap-tuple slot: table_slot_callbacks() hands foreign tables
	 * TTSOpsHeapTuple, which cannot receive a minimal tuple directly).
	 *
	 * The fetch must run in a context that outlives the row. ForeignNext() calls
	 * us in ecxt_per_tuple_memory, and ExecScan resets that before every fetch --
	 * including the no-qual fast path. Whenever tuplestore_gettupleslot hands the
	 * slot a palloc'd tuple it also hands over ownership (TTS_FLAG_SHOULDFREE),
	 * and the slot frees it on its next store. Allocated in the per-tuple context
	 * that pointer is already reclaimed by then, so the next store frees wiped
	 * memory and corrupts the allocator. That happens for a spilled tuplestore
	 * even with copy=false (readtup palloc's and sets should_free), so pin the
	 * context rather than the copy flag.
	 */
	oldcxt = MemoryContextSwitchTo(node->ss.ps.state->es_query_cxt);
	got = tuplestore_gettupleslot(st->tupstore, true, false, st->readslot);
	MemoryContextSwitchTo(oldcxt);

	if (!got)
		return ExecClearTuple(slot);
	return ExecCopySlot(slot, st->readslot);
}

static void
pqfdwReScanForeignScan(ForeignScanState *node)
{
	PqFdwScanState *st = (PqFdwScanState *) node->fdw_state;

	if (st != NULL && st->tupstore != NULL)
		tuplestore_rescan(st->tupstore);
}

static void
pqfdwEndForeignScan(ForeignScanState *node)
{
	PqFdwScanState *st = (PqFdwScanState *) node->fdw_state;

	if (st == NULL)
		return;

	/* drop the slot first: a non-copied fetch leaves it pointing into the
	 * tuplestore's own memory, which tuplestore_end() releases */
	if (st->readslot != NULL)
	{
		ExecDropSingleTupleTableSlot(st->readslot);
		st->readslot = NULL;
	}
	if (st->tupstore != NULL)
	{
		tuplestore_end(st->tupstore);
		st->tupstore = NULL;
	}
}

static void
pqfdwExplainForeignScan(ForeignScanState *node, ExplainState *es)
{
	PqFdwScanState *st = (PqFdwScanState *) node->fdw_state;

	/* only populated once the scan has begun (EXPLAIN ANALYZE) */
	if (st == NULL)
		return;
	ExplainPropertyInteger("Row Groups", NULL, st->groupsTotal, es);
	ExplainPropertyInteger("Row Groups Skipped", NULL, st->groupsSkipped, es);
}

Datum
pgcolumnar_parquet_fdw_handler(PG_FUNCTION_ARGS)
{
	FdwRoutine *r = makeNode(FdwRoutine);

	r->GetForeignRelSize = pqfdwGetForeignRelSize;
	r->GetForeignPaths = pqfdwGetForeignPaths;
	r->GetForeignPlan = pqfdwGetForeignPlan;
	r->BeginForeignScan = pqfdwBeginForeignScan;
	r->IterateForeignScan = pqfdwIterateForeignScan;
	r->ReScanForeignScan = pqfdwReScanForeignScan;
	r->ExplainForeignScan = pqfdwExplainForeignScan;
	r->EndForeignScan = pqfdwEndForeignScan;

	PG_RETURN_POINTER(r);
}

/*
 * Option validator: the only accepted option is "path" on a foreign table. Server,
 * wrapper, and user-mapping objects take no options.
 */
Datum
pgcolumnar_parquet_fdw_validator(PG_FUNCTION_ARGS)
{
	List	   *options = untransformRelOptions(PG_GETARG_DATUM(0));
	Oid			catalog = PG_GETARG_OID(1);
	ListCell   *lc;

	foreach(lc, options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (catalog == ForeignTableRelationId && strcmp(def->defname, "path") == 0)
		{
			if (defGetString(def)[0] == '\0')
				ereport(ERROR,
						(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
						 errmsg("\"path\" option cannot be empty")));
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
					 errmsg("invalid option \"%s\"", def->defname),
					 errhint("The pgcolumnar_parquet wrapper accepts only the foreign-table option \"path\".")));
	}

	PG_RETURN_VOID();
}
