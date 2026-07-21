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

#include "fmgr.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "executor/tuptable.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"

PG_FUNCTION_INFO_V1(columnar_import_parquet);

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
	int			phys_type;		/* PQ_* physical type */
	int			repetition;		/* 0 required, 1 optional */
	int			converted_type;	/* -1 if none */
	int			type_length;	/* FIXED_LEN_BYTE_ARRAY length */
	char	   *name;
} PqSchemaCol;

typedef struct PqChunk
{
	int			codec;
	int64		num_values;
	int64		data_page_offset;
	int64		dict_page_offset;	/* 0 if none */
	int64		total_compressed_size;
} PqChunk;

typedef struct PqRowGroup
{
	int64		num_rows;
	PqChunk    *chunks;			/* [ncols] */
} PqRowGroup;

typedef struct PqFile
{
	int			ncols;
	PqSchemaCol *schema;		/* [ncols] leaf columns */
	int			nrowgroups;
	PqRowGroup *rgs;
} PqFile;

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
				break;
			case 6:				/* converted_type */
				sc->converted_type = (int) tcr_zigzag(r);
				break;
			default:
				tcr_skip(r, ft);
				break;
		}
	}
	return num_children;
}

/* parse the whole FileMetaData; returns false on error or unsupported shape */
static bool
parse_file_metadata(const uint8 *buf, size_t len, PqFile *pf)
{
	TCReader	r = {buf, len, 0, false};
	int			lastId = 0;

	pf->ncols = 0;
	pf->schema = NULL;
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
			int			leaf = 0;

			for (i = 0; i < n && !r.error; i++)
			{
				PqSchemaCol sc;
				int			nc = parse_schema_element(&r, &sc);

				if (i == 0)
					continue;	/* root element */
				if (nc != 0)
					return false;	/* nested schema not supported */
				tmp[leaf++] = sc;
			}
			pf->ncols = leaf;
			pf->schema = tmp;
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

				rg->chunks = palloc0(sizeof(PqChunk) * Max(pf->ncols, 1));
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

						for (ci = 0; ci < cn && !r.error; ci++)
							parse_column_chunk(&r,
											   ci < (uint32) pf->ncols
											   ? &rg->chunks[ci] : &(PqChunk){0});
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
	return !r.error && pf->ncols > 0;
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

/* convert one PLAIN physical value at *p (advancing *p) to a target Datum */
static Datum
plain_value_to_datum(const PqColPlan *plan, const uint8 **p, const uint8 *end,
					 bool *ok)
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
					return Int16GetDatum((int16) v);
				if (plan->typid == DATEOID)
					return DateADTGetDatum((DateADT) (v - PG_TO_UNIX_DAYS));
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
					return TimestampGetDatum((Timestamp) (v - PG_TO_UNIX_USECS));
				if (plan->typid == TIMEOID)
					return TimeADTGetDatum((TimeADT) v);
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
 * Decode one column chunk (all its pages) into per-row Datums/nulls for the row
 * group. Reads pages from the in-memory file buffer.
 */
static bool
decode_column_chunk(const uint8 *filebuf, size_t filelen, PqChunk *ch,
					PqSchemaCol *sc, PqColPlan *plan, int64 nrows,
					Datum *outVals, bool *outNulls)
{
	int			max_def = (sc->repetition == 1) ? 1 : 0;
	size_t		pos = (size_t) (ch->dict_page_offset ? ch->dict_page_offset
										: ch->data_page_offset);
	int64		rowsDone = 0;
	Datum	   *dict = NULL;
	int			dictCount = 0;

	while (rowsDone < nrows && pos < filelen)
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
			{
				decode_plain_bools(db, dictCount, dict);
			}
			else
			{
				for (i = 0; i < dictCount; i++)
				{
					bool		ok;

					dict[i] = plain_value_to_datum(plan, &p, end, &ok);
					if (!ok)
						return false;
				}
			}
			pos += hdrlen + h.compressed_size;
			continue;
		}

		/* a data page (v1 or v2) */
		{
			int			npage = h.num_values;
			uint32	   *defs = NULL;
			int			nnn;
			const uint8 *valbuf;
			size_t		vallen;
			int			i;
			int			vi = 0;
			Datum	   *pv;

			if (h.is_v2)
			{
				/* levels are uncompressed at the front; values may be compressed */
				const uint8 *levels = praw;
				int			levLen = h.def_levels_len + h.rep_levels_len;
				const uint8 *vraw = praw + levLen;
				int			vrawlen = h.compressed_size - levLen;

				if (max_def > 0)
				{
					defs = palloc(sizeof(uint32) * npage);
					if (!rle_bitpack_decode(levels + h.rep_levels_len,
											h.def_levels_len, bits_for(max_def),
											npage, defs))
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
				/* v1: whole page is compressed together; levels length-prefixed */
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
				/* flat schema: no repetition levels; definition levels first */
				if (max_def > 0)
				{
					uint32		llen;

					if (off + 4 > pblen)
						return false;
					memcpy(&llen, pb + off, 4);
					off += 4;
					if (off + llen > pblen)
						return false;
					defs = palloc(sizeof(uint32) * npage);
					if (!rle_bitpack_decode(pb + off, llen, bits_for(max_def),
											npage, defs))
						return false;
					off += llen;
				}
				valbuf = pb + off;
				vallen = pblen - off;
			}

			/* number of non-null values in this page */
			if (max_def > 0)
			{
				nnn = 0;
				for (i = 0; i < npage; i++)
					if (defs[i] == (uint32) max_def)
						nnn++;
			}
			else
				nnn = npage;

			/* decode the nnn values into pv[] */
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

						pv[i] = plain_value_to_datum(plan, &p, end, &ok);
						if (!ok)
							return false;
					}
				}
			}
			else if (h.encoding == PQE_RLE && plan->expect_phys == PQ_BOOLEAN)
			{
				/* RLE-encoded booleans (pyarrow's default for bool in v2): the
				 * RLE/bit-packed hybrid at bit width 1, prefixed with a 4-byte
				 * little-endian length. */
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

			/* scatter into the row-group output by definition level */
			for (i = 0; i < npage && rowsDone < nrows; i++)
			{
				if (max_def > 0 && defs[i] != (uint32) max_def)
				{
					outNulls[rowsDone] = true;
					outVals[rowsDone] = (Datum) 0;
				}
				else
				{
					outNulls[rowsDone] = false;
					outVals[rowsDone] = pv[vi++];
				}
				rowsDone++;
			}
			pos += hdrlen + h.compressed_size;
		}
	}
	return rowsDone == nrows;
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
	int			natts;
	int			livecols;
	FILE	   *f;
	long		filelen;
	uint8	   *filebuf;
	uint32		metalen;
	PqFile		pf;
	PqColPlan  *plans;
	TupleTableSlot *slot;
	CommandId	cid = GetCurrentCommandId(true);
	int64		total = 0;
	int			i;
	int			rg;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("columnar.import_parquet requires superuser (reads a server-side file)")));

	rel = table_open(relid, RowExclusiveLock);
	tupdesc = RelationGetDescr(rel);
	natts = tupdesc->natts;

	livecols = 0;
	for (i = 0; i < natts; i++)
		if (!TupleDescAttr(tupdesc, i)->attisdropped)
			livecols++;

	/* read the whole file into memory */
	f = AllocateFile(path, PG_BINARY_R);
	if (f == NULL)
	{
		table_close(rel, RowExclusiveLock);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\" for reading: %m", path)));
	}
	if (fseek(f, 0, SEEK_END) != 0 || (filelen = ftell(f)) < 0)
	{
		FreeFile(f);
		table_close(rel, RowExclusiveLock);
		ereport(ERROR, (errcode_for_file_access(), errmsg("could not size \"%s\": %m", path)));
	}
	if (filelen < 12)
	{
		FreeFile(f);
		table_close(rel, RowExclusiveLock);
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("\"%s\" is not a Parquet file", path)));
	}
	filebuf = (uint8 *) palloc(filelen);
	if (fseek(f, 0, SEEK_SET) != 0 ||
		fread(filebuf, 1, filelen, f) != (size_t) filelen)
	{
		FreeFile(f);
		table_close(rel, RowExclusiveLock);
		ereport(ERROR, (errcode_for_file_access(), errmsg("could not read \"%s\": %m", path)));
	}
	FreeFile(f);

	if (memcmp(filebuf, "PAR1", 4) != 0 ||
		memcmp(filebuf + filelen - 4, "PAR1", 4) != 0)
	{
		table_close(rel, RowExclusiveLock);
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
						errmsg("\"%s\" is not a Parquet file (bad magic)", path)));
	}
	memcpy(&metalen, filebuf + filelen - 8, 4);
	if ((long) metalen + 8 > filelen)
	{
		table_close(rel, RowExclusiveLock);
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
						errmsg("\"%s\" has a corrupt Parquet footer", path)));
	}

	if (!parse_file_metadata(filebuf + filelen - 8 - metalen, metalen, &pf))
	{
		table_close(rel, RowExclusiveLock);
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("could not parse \"%s\" (unsupported or corrupt Parquet metadata)", path)));
	}

	if (pf.ncols != livecols)
	{
		table_close(rel, RowExclusiveLock);
		ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH),
						errmsg("Parquet file has %d columns, target table has %d",
							   pf.ncols, livecols)));
	}

	/* build per-column plans, validating physical type against the target type */
	plans = palloc0(sizeof(PqColPlan) * pf.ncols);
	{
		int			c = 0;

		for (i = 0; i < natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(tupdesc, i);
			Oid			t;
			int			want;

			if (att->attisdropped)
				continue;
			t = att->atttypid;
			plans[c].typid = t;
			get_typlenbyval(t, &plans[c].typlen, &plans[c].typbyval);

			switch (t)
			{
				case INT2OID:
				case INT4OID:
				case DATEOID:
					want = PQ_INT32;
					break;
				case INT8OID:
				case TIMESTAMPOID:
				case TIMESTAMPTZOID:
				case TIMEOID:
					want = PQ_INT64;
					break;
				case FLOAT4OID:
					want = PQ_FLOAT;
					break;
				case FLOAT8OID:
					want = PQ_DOUBLE;
					break;
				case BOOLOID:
					want = PQ_BOOLEAN;
					break;
				case TEXTOID:
				case VARCHAROID:
				case BYTEAOID:
					want = PQ_BYTE_ARRAY;
					break;
				default:
					table_close(rel, RowExclusiveLock);
					ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
									errmsg("column \"%s\" has type %s, which columnar.import_parquet does not support",
										   NameStr(att->attname), format_type_be(t))));
			}
			if (pf.schema[c].phys_type != want)
			{
				table_close(rel, RowExclusiveLock);
				ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH),
								errmsg("Parquet column %d physical type %d is not compatible with target column \"%s\" (%s)",
									   c, pf.schema[c].phys_type,
									   NameStr(att->attname), format_type_be(t))));
			}
			plans[c].expect_phys = want;
			c++;
		}
	}

	slot = table_slot_create(rel, NULL);

	for (rg = 0; rg < pf.nrowgroups; rg++)
	{
		PqRowGroup *g = &pf.rgs[rg];
		int64		n = g->num_rows;
		Datum	  **cv = palloc(sizeof(Datum *) * pf.ncols);
		bool	  **cn = palloc(sizeof(bool *) * pf.ncols);
		int64		r;

		for (i = 0; i < pf.ncols; i++)
		{
			cv[i] = palloc(sizeof(Datum) * Max(n, 1));
			cn[i] = palloc(sizeof(bool) * Max(n, 1));
			if (!decode_column_chunk(filebuf, filelen, &g->chunks[i],
									 &pf.schema[i], &plans[i], n, cv[i], cn[i]))
			{
				table_close(rel, RowExclusiveLock);
				ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
								errmsg("could not decode Parquet column %d in row group %d",
									   i, rg)));
			}
		}

		for (r = 0; r < n; r++)
		{
			int			c = 0;

			ExecClearTuple(slot);
			for (i = 0; i < natts; i++)
			{
				if (TupleDescAttr(tupdesc, i)->attisdropped)
				{
					slot->tts_isnull[i] = true;
					continue;
				}
				slot->tts_values[i] = cv[c][r];
				slot->tts_isnull[i] = cn[c][r];
				c++;
			}
			ExecStoreVirtualTuple(slot);
			table_tuple_insert(rel, slot, cid, 0, NULL);
			total++;
			CHECK_FOR_INTERRUPTS();
		}
	}

	ExecDropSingleTupleTableSlot(slot);
	table_close(rel, RowExclusiveLock);

	PG_RETURN_INT64(total);
}
