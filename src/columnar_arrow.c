/*-------------------------------------------------------------------------
 *
 * columnar_arrow.c
 *		Arrow IPC stream export for pgColumnar (gap 27, piece 1).
 *
 *		columnar.export_arrow(rel regclass, path text) writes a columnar table
 *		to an Apache Arrow IPC *stream* file: a Schema message, one RecordBatch
 *		message per ARROW_BATCH_ROWS rows, and an end-of-stream marker. The
 *		writer is self-contained -- it hand-builds the FlatBuffers metadata and
 *		the record-batch body buffers, so there is no libarrow/libparquet build
 *		or run-time dependency. Rows are read in physical order via the scalar
 *		reader; deleted rows are skipped by the reader.
 *
 *		First slice type mapping: int2/4/8, float4/8, bool, text/varchar (Utf8),
 *		bytea (Binary). Any other column type is rejected. Little-endian hosts
 *		only (the Arrow body mirrors native scalar bytes).
 *
 * Independent MIT implementation built from the Apache Arrow columnar format
 * and IPC specifications (Schema.fbs, Message.fbs, encapsulated message format)
 * and the public PostgreSQL API only.
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
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"
#include "utils/uuid.h"

PG_FUNCTION_INFO_V1(columnar_export_arrow);
PG_FUNCTION_INFO_V1(columnar_import_arrow);

/* one RecordBatch per this many rows */
#define ARROW_BATCH_ROWS 16384

/* PostgreSQL epoch (2000-01-01) to Unix epoch (1970-01-01) offsets */
#define PG_TO_UNIX_DAYS		((int64) (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE))
#define PG_TO_UNIX_USECS	(PG_TO_UNIX_DAYS * USECS_PER_DAY)

/* Arrow Type union tags (Schema.fbs) */
#define ARROW_TYPE_Int			2
#define ARROW_TYPE_FloatingPoint 3
#define ARROW_TYPE_Binary		4
#define ARROW_TYPE_Utf8			5
#define ARROW_TYPE_Bool			6
#define ARROW_TYPE_Decimal		7
#define ARROW_TYPE_Date			8
#define ARROW_TYPE_Time			9
#define ARROW_TYPE_Timestamp	10
#define ARROW_TYPE_FixedSizeBinary 15
/* Arrow MessageHeader union tags (Message.fbs) */
#define ARROW_MSG_Schema		1
#define ARROW_MSG_DictionaryBatch 2
#define ARROW_MSG_RecordBatch	3
/* MetadataVersion V5 */
#define ARROW_METADATA_V5		4

/* Column kind -> how we lay out its Arrow buffers */
typedef enum ArrowKind
{
	A_INT16,
	A_INT32,
	A_INT64,
	A_FLOAT32,
	A_FLOAT64,
	A_BOOL,
	A_UTF8,
	A_BINARY,
	A_DATE32,					/* date -> Date32 (days from Unix epoch) */
	A_TIME64,					/* time -> Time64[us] */
	A_TIMESTAMP,				/* timestamp -> Timestamp[us], no zone */
	A_TIMESTAMPTZ,				/* timestamptz -> Timestamp[us], zone "UTC" */
	A_UUID,						/* uuid -> FixedSizeBinary(16) */
	A_DECIMAL128				/* numeric(p,s) -> Decimal128(p,s) */
}			ArrowKind;

/* Parse a numeric value (via its text form) into a 128-bit unscaled integer at
 * the given scale. Returns false for NaN/Infinity, which a decimal cannot hold.
 * The stored value already carries scale s, so padding suffices; the defensive
 * truncation branch never fires for a validly scaled numeric. */
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

/* -------------------------------------------------------------------------
 * Minimal FlatBuffers builder (little-endian). The buffer is built back to
 * front: data occupies buf[cap - tail .. cap); an object's identity is its
 * "tail" value (bytes from the end) captured right after it is written.
 * ------------------------------------------------------------------------- */
typedef struct FBB
{
	uint8	   *buf;
	uint32		cap;
	uint32		tail;
	uint32		minalign;
	/* current table under construction */
	int			nslots;
	uint32		objectEnd;
	uint32		vslot[16];
}			FBB;

static void
fb_init(FBB *b)
{
	b->cap = 256;
	b->buf = palloc(b->cap);
	b->tail = 0;
	b->minalign = 1;
	b->nslots = 0;
	b->objectEnd = 0;
}

static void
fb_grow(FBB *b, uint32 need)
{
	uint64		want;
	uint32		newcap;
	uint8	   *nb;

	if (b->cap - b->tail >= need)
		return;
	/* compute the new capacity in 64-bit to avoid a uint32 doubling overflow */
	want = (uint64) b->cap * 2;
	while (want < (uint64) b->tail + need)
		want *= 2;
	if (want > MaxAllocSize)
		elog(ERROR, "columnar: arrow metadata buffer too large");
	newcap = (uint32) want;
	nb = palloc(newcap);
	memcpy(nb + newcap - b->tail, b->buf + b->cap - b->tail, b->tail);
	pfree(b->buf);
	b->buf = nb;
	b->cap = newcap;
}

/* prepend n raw bytes (already in final order) */
static void
fb_place(FBB *b, const void *src, uint32 n)
{
	fb_grow(b, n);
	b->tail += n;
	memcpy(b->buf + b->cap - b->tail, src, n);
}

static void
fb_pad(FBB *b, uint32 n)
{
	if (n == 0)
		return;
	fb_grow(b, n);
	b->tail += n;
	memset(b->buf + b->cap - b->tail, 0, n);
}

static inline uint32
fb_offset(FBB *b)
{
	return b->tail;
}

/* align so that, after `additional` more bytes plus a `size`-aligned scalar are
 * written, the scalar lands aligned (relative to the eventually-aligned end). */
static void
fb_prep(FBB *b, uint32 size, uint32 additional)
{
	uint32		alignsize;

	if (size > b->minalign)
		b->minalign = size;
	alignsize = ((~(b->tail + additional)) + 1) & (size - 1);
	fb_pad(b, alignsize);
}

static void
fb_push_u8(FBB *b, uint8 v)
{
	fb_prep(b, 1, 0);
	fb_place(b, &v, 1);
}
static void
fb_push_i16(FBB *b, int16 v)
{
	fb_prep(b, 2, 0);
	fb_place(b, &v, 2);
}
static void
fb_push_i32(FBB *b, int32 v)
{
	fb_prep(b, 4, 0);
	fb_place(b, &v, 4);
}
static void
fb_push_i64(FBB *b, int64 v)
{
	fb_prep(b, 8, 0);
	fb_place(b, &v, 8);
}

/* prepend a uoffset that references object at `off` (bytes-from-end) */
static void
fb_push_uoffset(FBB *b, uint32 off)
{
	uint32		v;

	fb_prep(b, 4, 0);
	v = (fb_offset(b) + 4) - off;
	fb_place(b, &v, 4);
}

/* ---- vectors ---- */
static void
fb_start_vector(FBB *b, uint32 elemSize, uint32 count, uint32 align)
{
	fb_prep(b, 4, elemSize * count); /* length prefix */
	fb_prep(b, align, elemSize * count);	/* element alignment */
}

static uint32
fb_end_vector(FBB *b, uint32 count)
{
	fb_prep(b, 4, 0);
	fb_place(b, &count, 4);		/* length prefix precedes the elements */
	return fb_offset(b);
}

/* ---- tables ---- */
static void
fb_start(FBB *b, int nslots)
{
	int			i;

	Assert(nslots <= 16);
	b->nslots = nslots;
	for (i = 0; i < nslots; i++)
		b->vslot[i] = 0;
	b->objectEnd = fb_offset(b);
}

static void
fb_slot(FBB *b, int i)
{
	b->vslot[i] = fb_offset(b);
}

static void
fb_add_i16(FBB *b, int i, int16 val, int16 def)
{
	if (val == def)
		return;
	fb_push_i16(b, val);
	fb_slot(b, i);
}
static void
fb_add_i32(FBB *b, int i, int32 val, int32 def)
{
	if (val == def)
		return;
	fb_push_i32(b, val);
	fb_slot(b, i);
}
static void
fb_add_i64(FBB *b, int i, int64 val, int64 def)
{
	if (val == def)
		return;
	fb_push_i64(b, val);
	fb_slot(b, i);
}
static void
fb_add_bool(FBB *b, int i, bool val, bool def)
{
	if (val == def)
		return;
	fb_push_u8(b, val ? 1 : 0);
	fb_slot(b, i);
}
static void
fb_add_u8(FBB *b, int i, uint8 val, uint8 def)
{
	if (val == def)
		return;
	fb_push_u8(b, val);
	fb_slot(b, i);
}
static void
fb_add_offset(FBB *b, int i, uint32 off)
{
	if (off == 0)
		return;
	fb_push_uoffset(b, off);
	fb_slot(b, i);
}

static uint32
fb_end(FBB *b)
{
	uint32		objectOffset;
	uint32		vtOffset;
	int32		soff;
	int			i;
	int16		objsize;
	int16		vtsize;
	int32		zero = 0;

	/* soffset placeholder = table location */
	fb_prep(b, 4, 0);
	fb_place(b, &zero, 4);
	objectOffset = fb_offset(b);

	/* vtable: field voffsets (high slot first), then objsize, then vtsize */
	for (i = b->nslots - 1; i >= 0; i--)
	{
		int16		voff = b->vslot[i] ? (int16) (objectOffset - b->vslot[i]) : 0;

		fb_place(b, &voff, 2);
	}
	objsize = (int16) (objectOffset - b->objectEnd);
	fb_place(b, &objsize, 2);
	vtsize = (int16) ((b->nslots + 2) * 2);
	fb_place(b, &vtsize, 2);

	vtOffset = fb_offset(b);
	soff = (int32) (vtOffset - objectOffset);
	memcpy(b->buf + b->cap - objectOffset, &soff, 4);
	return objectOffset;
}

static void
fb_finish(FBB *b, uint32 root)
{
	fb_prep(b, b->minalign, 4);
	fb_push_uoffset(b, root);
}

static uint32
fb_create_string(FBB *b, const char *s)
{
	uint32		n = (uint32) strlen(s);
	uint8		zero = 0;

	fb_prep(b, 4, n + 1);
	fb_place(b, &zero, 1);		/* null terminator */
	fb_place(b, s, n);			/* characters (s[0] ends lowest) */
	{
		uint32		len = n;

		fb_place(b, &len, 4);	/* length prefix (already aligned) */
	}
	return fb_offset(b);
}

/* ---- Arrow Type table for one column; returns tag via *typetag ---- */
static uint32
fb_arrow_type(FBB *b, ArrowKind kind, int precision, int scale, uint8 *typetag)
{
	switch (kind)
	{
		case A_INT16:
		case A_INT32:
		case A_INT64:
			{
				int32		bits = (kind == A_INT16) ? 16 : (kind == A_INT32) ? 32 : 64;

				fb_start(b, 2);
				fb_add_i32(b, 0, bits, 0);	/* bitWidth */
				fb_add_bool(b, 1, true, false); /* is_signed */
				*typetag = ARROW_TYPE_Int;
				return fb_end(b);
			}
		case A_FLOAT32:
		case A_FLOAT64:
			fb_start(b, 1);
			fb_add_i16(b, 0, (kind == A_FLOAT32) ? 1 : 2, 0);	/* SINGLE/DOUBLE */
			*typetag = ARROW_TYPE_FloatingPoint;
			return fb_end(b);
		case A_BOOL:
			fb_start(b, 0);
			*typetag = ARROW_TYPE_Bool;
			return fb_end(b);
		case A_UTF8:
			fb_start(b, 0);
			*typetag = ARROW_TYPE_Utf8;
			return fb_end(b);
		case A_BINARY:
			fb_start(b, 0);
			*typetag = ARROW_TYPE_Binary;
			return fb_end(b);
		case A_DATE32:
			/* Date { unit: DateUnit = MILLISECOND (1) }; want DAY (0) */
			fb_start(b, 1);
			fb_add_i16(b, 0, 0, 1);
			*typetag = ARROW_TYPE_Date;
			return fb_end(b);
		case A_TIME64:
			/* Time { unit: TimeUnit = MILLISECOND (1); bitWidth: int = 32 } */
			fb_start(b, 2);
			fb_add_i16(b, 0, 2, 1);		/* MICROSECOND */
			fb_add_i32(b, 1, 64, 32);
			*typetag = ARROW_TYPE_Time;
			return fb_end(b);
		case A_TIMESTAMP:
		case A_TIMESTAMPTZ:
			{
				uint32		tzOff = 0;

				if (kind == A_TIMESTAMPTZ)
					tzOff = fb_create_string(b, "UTC");
				/* Timestamp { unit: TimeUnit = SECOND (0); timezone: string } */
				fb_start(b, 2);
				fb_add_i16(b, 0, 2, 0);		/* MICROSECOND */
				fb_add_offset(b, 1, tzOff);
				*typetag = ARROW_TYPE_Timestamp;
				return fb_end(b);
			}
		case A_UUID:
			/* FixedSizeBinary { byteWidth: int } */
			fb_start(b, 1);
			fb_add_i32(b, 0, 16, 0);
			*typetag = ARROW_TYPE_FixedSizeBinary;
			return fb_end(b);
		case A_DECIMAL128:
			/* Decimal { precision: int; scale: int; bitWidth: int = 128 } */
			fb_start(b, 3);
			fb_add_i32(b, 0, precision, 0);
			fb_add_i32(b, 1, scale, 0);
			*typetag = ARROW_TYPE_Decimal;
			return fb_end(b);
	}
	*typetag = 0;
	return 0;					/* unreachable */
}

/* -------------------------------------------------------------------------
 * Per-column accumulator for one RecordBatch. Values for null slots are still
 * written (zeros / no advance) as Arrow requires; validity records nullness.
 * ------------------------------------------------------------------------- */
typedef struct ArrowCol
{
	char	   *name;
	ArrowKind	kind;
	int			width;			/* fixed-width byte size, 0 otherwise */
	int			precision;		/* decimal precision (A_DECIMAL128) */
	int			scale;			/* decimal scale (A_DECIMAL128) */
	bool		convertText;	/* A_UTF8 fallback needs the type output fn */
	FmgrInfo	outFinfo;		/* output function (when convertText) */
	StringInfoData valid;		/* 1 byte per row: 1 valid, 0 null */
	int64		nullCount;
	StringInfoData fixed;		/* fixed-width raw values */
	StringInfoData boolvals;	/* 1 byte per row (A_BOOL) */
	StringInfoData vardata;		/* concatenated var-length bytes */
	StringInfoData offs;		/* int32 running offsets, n+1 entries */
	int32		running;
}			ArrowCol;

static void
arrowcol_reset(ArrowCol *c)
{
	resetStringInfo(&c->valid);
	resetStringInfo(&c->fixed);
	resetStringInfo(&c->boolvals);
	resetStringInfo(&c->vardata);
	resetStringInfo(&c->offs);
	c->nullCount = 0;
	c->running = 0;
	if (c->kind == A_UTF8 || c->kind == A_BINARY)
	{
		int32		z = 0;

		appendBinaryStringInfo(&c->offs, (char *) &z, 4);	/* offsets[0] = 0 */
	}
}

static ArrowKind
arrow_kind_for_type(Oid typid, int32 typmod, int *width, int *precision, int *scale)
{
	*precision = 0;
	*scale = 0;
	switch (typid)
	{
		case INT2OID:
			*width = 2;
			return A_INT16;
		case INT4OID:
			*width = 4;
			return A_INT32;
		case INT8OID:
			*width = 8;
			return A_INT64;
		case FLOAT4OID:
			*width = 4;
			return A_FLOAT32;
		case FLOAT8OID:
			*width = 8;
			return A_FLOAT64;
		case BOOLOID:
			*width = 0;
			return A_BOOL;
		case TEXTOID:
		case VARCHAROID:
		case JSONOID:
		case JSONBOID:
			*width = 0;
			return A_UTF8;
		case BYTEAOID:
			*width = 0;
			return A_BINARY;
		case DATEOID:
			*width = 4;
			return A_DATE32;
		case TIMEOID:
			*width = 8;
			return A_TIME64;
		case TIMESTAMPOID:
			*width = 8;
			return A_TIMESTAMP;
		case TIMESTAMPTZOID:
			*width = 8;
			return A_TIMESTAMPTZ;
		case UUIDOID:
			*width = 16;
			return A_UUID;
		case NUMERICOID:
			/* numeric(p,s) with p<=38 and 0<=s<=p -> Decimal128; otherwise
			 * (unconstrained, over-precision) fall back to text. */
			if (typmod >= (int32) VARHDRSZ)
			{
				int32		tmp = typmod - VARHDRSZ;
				int			p = (tmp >> 16) & 0xffff;
				int			s = tmp & 0xffff;

				if (p >= 1 && p <= 38 && s >= 0 && s <= p)
				{
					*width = 16;
					*precision = p;
					*scale = s;
					return A_DECIMAL128;
				}
			}
			*width = 0;
			return A_UTF8;		/* text fallback */
		default:
			*width = -1;
			return A_INT32;		/* caller checks width == -1 */
	}
}

/* append one row's value for a column */
static void
arrowcol_append(ArrowCol *c, Datum d, bool isnull)
{
	char		one = 1,
				zero = 0;
	__int128	dec = 0;

	/*
	 * A few types can carry values with no target representation (±infinity
	 * dates/timestamps, NaN/Infinity numerics). Fold those to null, computing
	 * the decimal here so a non-finite value is detected before validity is
	 * written.
	 */
	if (!isnull)
	{
		switch (c->kind)
		{
			case A_DATE32:
				if (DATE_NOT_FINITE(DatumGetDateADT(d)))
					isnull = true;
				break;
			case A_TIMESTAMP:
			case A_TIMESTAMPTZ:
				if (TIMESTAMP_NOT_FINITE(DatumGetTimestamp(d)))
					isnull = true;
				break;
			case A_DECIMAL128:
				if (!numeric_to_int128(d, c->scale, &dec))
					isnull = true;
				break;
			default:
				break;
		}
	}

	appendStringInfoChar(&c->valid, isnull ? zero : one);
	if (isnull)
		c->nullCount++;

	switch (c->kind)
	{
		case A_INT16:
			{
				int16		v = isnull ? 0 : DatumGetInt16(d);

				appendBinaryStringInfo(&c->fixed, (char *) &v, 2);
				break;
			}
		case A_INT32:
			{
				int32		v = isnull ? 0 : DatumGetInt32(d);

				appendBinaryStringInfo(&c->fixed, (char *) &v, 4);
				break;
			}
		case A_INT64:
			{
				int64		v = isnull ? 0 : DatumGetInt64(d);

				appendBinaryStringInfo(&c->fixed, (char *) &v, 8);
				break;
			}
		case A_FLOAT32:
			{
				float4		v = isnull ? 0 : DatumGetFloat4(d);

				appendBinaryStringInfo(&c->fixed, (char *) &v, 4);
				break;
			}
		case A_FLOAT64:
			{
				float8		v = isnull ? 0 : DatumGetFloat8(d);

				appendBinaryStringInfo(&c->fixed, (char *) &v, 8);
				break;
			}
		case A_DATE32:
			{
				int32		v = isnull ? 0 :
					(int32) (DatumGetDateADT(d) + PG_TO_UNIX_DAYS);

				appendBinaryStringInfo(&c->fixed, (char *) &v, 4);
				break;
			}
		case A_TIME64:
			{
				int64		v = isnull ? 0 : (int64) DatumGetTimeADT(d);

				appendBinaryStringInfo(&c->fixed, (char *) &v, 8);
				break;
			}
		case A_TIMESTAMP:
		case A_TIMESTAMPTZ:
			{
				int64		v = isnull ? 0 :
					(int64) DatumGetTimestamp(d) + PG_TO_UNIX_USECS;

				appendBinaryStringInfo(&c->fixed, (char *) &v, 8);
				break;
			}
		case A_UUID:
			{
				static const char zeros[UUID_LEN] = {0};

				if (isnull)
					appendBinaryStringInfo(&c->fixed, zeros, UUID_LEN);
				else
					appendBinaryStringInfo(&c->fixed,
										   (char *) DatumGetUUIDP(d)->data, UUID_LEN);
				break;
			}
		case A_DECIMAL128:
			{
				char		buf[16];

				if (isnull)
					memset(buf, 0, 16);
				else
					memcpy(buf, &dec, 16);	/* little-endian two's complement */
				appendBinaryStringInfo(&c->fixed, buf, 16);
				break;
			}
		case A_BOOL:
			appendStringInfoChar(&c->boolvals,
								 (!isnull && DatumGetBool(d)) ? 1 : 0);
			break;
		case A_UTF8:
		case A_BINARY:
			{
				if (!isnull)
				{
					if (c->convertText)
					{
						char	   *str = OutputFunctionCall(&c->outFinfo, d);
						int			len = (int) strlen(str);

						if (len > 0)
							appendBinaryStringInfo(&c->vardata, str, len);
						c->running += len;
						pfree(str);
					}
					else
					{
						struct varlena *v = PG_DETOAST_DATUM_PACKED(d);
						int			len = VARSIZE_ANY_EXHDR(v);

						appendBinaryStringInfo(&c->vardata, VARDATA_ANY(v), len);
						c->running += len;
					}
				}
				appendBinaryStringInfo(&c->offs, (char *) &c->running, 4);
				break;
			}
	}
}

/* append `body` with one buffer of `bytes`/`len`, 8-padded; record Buffer meta */
static void
body_add_buffer(StringInfo body, int64 *offsets, int64 *lengths, int *nbuf,
				const char *bytes, int64 len)
{
	static const char zeros[8] = {0};
	int64		pad;

	offsets[*nbuf] = body->len;
	lengths[*nbuf] = len;
	(*nbuf)++;
	if (len > 0)
		appendBinaryStringInfo(body, bytes, len);
	pad = (8 - (body->len % 8)) % 8;
	if (pad > 0)
		appendBinaryStringInfo(body, zeros, pad);
}

/* build one RecordBatch (metadata + body) and write it */
static void
write_record_batch(FILE *f, ArrowCol *cols, int ncols, int64 nrows)
{
	StringInfoData body;
	FBB			b;
	int64		nodeLen[16 * 3],
				nodeNull[16 * 3];
	int64		bufOff[16 * 3],
				bufLen[16 * 3];
	int			nnodes = 0,
				nbuf = 0;
	int			i;
	uint32		nodesVec,
				bufsVec,
				rbOff,
				msgOff;
	int32		vlen = (int32) ((nrows + 7) / 8);
	uint32		cont = 0xFFFFFFFF;
	uint32		metaLen;
	uint32		metaPad;

	initStringInfo(&body);

	for (i = 0; i < ncols; i++)
	{
		ArrowCol   *c = &cols[i];
		char	   *validbits = palloc0(vlen);
		int			r;

		for (r = 0; r < nrows; r++)
			if (c->valid.data[r])
				validbits[r >> 3] |= (1 << (r & 7));

		nodeLen[nnodes] = nrows;
		nodeNull[nnodes] = c->nullCount;
		nnodes++;

		body_add_buffer(&body, bufOff, bufLen, &nbuf, validbits, vlen);

		if (c->kind == A_BOOL)
		{
			char	   *bits = palloc0(vlen);

			for (r = 0; r < nrows; r++)
				if (c->boolvals.data[r])
					bits[r >> 3] |= (1 << (r & 7));
			body_add_buffer(&body, bufOff, bufLen, &nbuf, bits, vlen);
		}
		else if (c->kind == A_UTF8 || c->kind == A_BINARY)
		{
			body_add_buffer(&body, bufOff, bufLen, &nbuf,
							c->offs.data, c->offs.len);
			body_add_buffer(&body, bufOff, bufLen, &nbuf,
							c->vardata.data, c->vardata.len);
		}
		else
			body_add_buffer(&body, bufOff, bufLen, &nbuf,
							c->fixed.data, c->fixed.len);
	}

	/* ---- RecordBatch metadata flatbuffer ---- */
	fb_init(&b);

	/* nodes vector: [FieldNode{length,null_count}] structs, 16B/8-align */
	fb_start_vector(&b, 16, nnodes, 8);
	for (i = nnodes - 1; i >= 0; i--)
	{
		fb_prep(&b, 8, 0);
		fb_place(&b, &nodeNull[i], 8);	/* null_count (higher) */
		fb_place(&b, &nodeLen[i], 8);	/* length (lower) */
	}
	nodesVec = fb_end_vector(&b, nnodes);

	/* buffers vector: [Buffer{offset,length}] structs */
	fb_start_vector(&b, 16, nbuf, 8);
	for (i = nbuf - 1; i >= 0; i--)
	{
		fb_prep(&b, 8, 0);
		fb_place(&b, &bufLen[i], 8); /* length (higher) */
		fb_place(&b, &bufOff[i], 8); /* offset (lower) */
	}
	bufsVec = fb_end_vector(&b, nbuf);

	fb_start(&b, 4);
	fb_add_i64(&b, 0, nrows, 0);
	fb_add_offset(&b, 1, nodesVec);
	fb_add_offset(&b, 2, bufsVec);
	rbOff = fb_end(&b);

	fb_start(&b, 5);
	fb_add_i16(&b, 0, ARROW_METADATA_V5, 0);
	fb_add_u8(&b, 1, ARROW_MSG_RecordBatch, 0);
	fb_add_offset(&b, 2, rbOff);
	fb_add_i64(&b, 3, body.len, 0); /* bodyLength */
	msgOff = fb_end(&b);
	fb_finish(&b, msgOff);

	/* ---- write encapsulated message ---- */
	metaLen = b.tail;
	metaPad = (8 - (metaLen % 8)) % 8;
	{
		uint32		metaLenField = metaLen + metaPad;
		static const char zeros[8] = {0};

		fwrite(&cont, 4, 1, f);
		fwrite(&metaLenField, 4, 1, f);
		fwrite(b.buf + b.cap - b.tail, 1, metaLen, f);
		if (metaPad)
			fwrite(zeros, 1, metaPad, f);
		if (body.len > 0)
			fwrite(body.data, 1, body.len, f);
	}

	pfree(b.buf);
	pfree(body.data);
}

/*
 * columnar_export_arrow
 *		SQL: columnar.export_arrow(rel regclass, path text) -> bigint.
 *		Write a columnar table to an Arrow IPC stream file; returns the number
 *		of rows written.
 */
Datum
columnar_export_arrow(PG_FUNCTION_ARGS)
{
	Oid			relid;
	text	   *pathText;
	char	   *path;
	Relation	rel;
	TupleDesc	tupdesc;
	int			ncols;
	ArrowCol   *cols;
	Snapshot	snapshot;
	ColumnarReadState *readState;
	Datum	   *values;
	bool	   *nulls;
	uint64		rowNumber;
	int64		total = 0;
	int64		batchRows = 0;
	FILE	   *f;
	FBB			b;
	int			i;
	uint32		vec,
				schemaOff,
				msgOff;
	uint32	   *fieldOff;
	MemoryContext batchCtx,
				oldCtx;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("relation and path must not be null")));

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to export to a server-side file")));

	relid = PG_GETARG_OID(0);
	pathText = PG_GETARG_TEXT_PP(1);
	path = text_to_cstring(pathText);

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
	if (ncols > 16)
	{
		table_close(rel, AccessShareLock);
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("columnar.export_arrow supports at most 16 columns")));
	}

	cols = palloc0(sizeof(ArrowCol) * ncols);
	for (i = 0; i < ncols; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);
		int			width;
		int			precision,
					scale;
		ArrowKind	kind;

		if (att->attisdropped)
		{
			table_close(rel, AccessShareLock);
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("columnar.export_arrow does not support dropped columns")));
		}
		kind = arrow_kind_for_type(att->atttypid, att->atttypmod,
								   &width, &precision, &scale);
		if (width < 0)
		{
			table_close(rel, AccessShareLock);
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("column \"%s\" has type %s, which columnar.export_arrow does not support",
							NameStr(att->attname),
							format_type_be(att->atttypid))));
		}
		cols[i].name = pstrdup(NameStr(att->attname));
		cols[i].kind = kind;
		cols[i].width = width;
		cols[i].precision = precision;
		cols[i].scale = scale;
		cols[i].convertText = (kind == A_UTF8 &&
							   (att->atttypid == NUMERICOID ||
								att->atttypid == JSONBOID));
		if (cols[i].convertText)
		{
			Oid			outfunc;
			bool		isvarlena;

			getTypeOutputInfo(att->atttypid, &outfunc, &isvarlena);
			fmgr_info(outfunc, &cols[i].outFinfo);
		}
		initStringInfo(&cols[i].valid);
		initStringInfo(&cols[i].fixed);
		initStringInfo(&cols[i].boolvals);
		initStringInfo(&cols[i].vardata);
		initStringInfo(&cols[i].offs);
		arrowcol_reset(&cols[i]);
	}

	f = AllocateFile(path, PG_BINARY_W);
	if (f == NULL)
	{
		table_close(rel, AccessShareLock);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\" for writing: %m", path)));
	}

	/* ---- Schema message ---- */
	fieldOff = palloc(sizeof(uint32) * ncols);
	fb_init(&b);
	for (i = 0; i < ncols; i++)
	{
		uint32		nameOff = fb_create_string(&b, cols[i].name);
		uint8		typetag;
		uint32		typeOff = fb_arrow_type(&b, cols[i].kind,
											cols[i].precision, cols[i].scale,
											&typetag);

		fb_start(&b, 7);
		fb_add_offset(&b, 0, nameOff);	/* name */
		fb_add_bool(&b, 1, true, false);	/* nullable */
		fb_add_u8(&b, 2, typetag, 0);	/* type_type */
		fb_add_offset(&b, 3, typeOff);	/* type */
		fieldOff[i] = fb_end(&b);
	}
	fb_start_vector(&b, 4, ncols, 4);
	for (i = ncols - 1; i >= 0; i--)
		fb_push_uoffset(&b, fieldOff[i]);
	vec = fb_end_vector(&b, ncols);

	fb_start(&b, 4);
	/* endianness Little=0 is the default, so omit slot 0 */
	fb_add_offset(&b, 1, vec);	/* fields */
	schemaOff = fb_end(&b);

	fb_start(&b, 5);
	fb_add_i16(&b, 0, ARROW_METADATA_V5, 0);
	fb_add_u8(&b, 1, ARROW_MSG_Schema, 0);
	fb_add_offset(&b, 2, schemaOff);
	msgOff = fb_end(&b);
	fb_finish(&b, msgOff);

	{
		uint32		cont = 0xFFFFFFFF;
		uint32		metaLen = b.tail;
		uint32		metaPad = (8 - (metaLen % 8)) % 8;
		uint32		metaLenField = metaLen + metaPad;
		static const char zeros[8] = {0};

		fwrite(&cont, 4, 1, f);
		fwrite(&metaLenField, 4, 1, f);
		fwrite(b.buf + b.cap - b.tail, 1, metaLen, f);
		if (metaPad)
			fwrite(zeros, 1, metaPad, f);
	}
	pfree(b.buf);

	/* ---- RecordBatch messages ---- */
	batchCtx = AllocSetContextCreate(CurrentMemoryContext,
									 "columnar arrow batch",
									 ALLOCSET_DEFAULT_SIZES);

	values = palloc(sizeof(Datum) * ncols);
	nulls = palloc(sizeof(bool) * ncols);

	snapshot = ActiveSnapshotSet() ? GetActiveSnapshot() : GetTransactionSnapshot();
	readState = ColumnarBeginRead(rel, snapshot, NULL, NULL, 0, NULL);

	while (ColumnarReadNextRow(readState, values, nulls, &rowNumber))
	{
		CHECK_FOR_INTERRUPTS();
		for (i = 0; i < ncols; i++)
			arrowcol_append(&cols[i], values[i], nulls[i]);
		batchRows++;
		total++;

		if (batchRows == ARROW_BATCH_ROWS)
		{
			oldCtx = MemoryContextSwitchTo(batchCtx);
			write_record_batch(f, cols, ncols, batchRows);
			MemoryContextSwitchTo(oldCtx);
			MemoryContextReset(batchCtx);
			for (i = 0; i < ncols; i++)
				arrowcol_reset(&cols[i]);
			batchRows = 0;
		}
	}
	ColumnarEndRead(readState);

	if (batchRows > 0)
	{
		oldCtx = MemoryContextSwitchTo(batchCtx);
		write_record_batch(f, cols, ncols, batchRows);
		MemoryContextSwitchTo(oldCtx);
	}

	/* ---- end-of-stream marker ---- */
	{
		uint32		cont = 0xFFFFFFFF;
		uint32		zero = 0;

		fwrite(&cont, 4, 1, f);
		fwrite(&zero, 4, 1, f);
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

/* =========================================================================
 * Arrow IPC stream import: columnar.import_arrow(rel regclass, path text).
 *
 * Reads an Arrow IPC *stream* file (as columnar.export_arrow writes, and as
 * pyarrow writes for non-dictionary arrays) and inserts its rows into an
 * existing columnar table whose column types match the file's schema, using
 * the reverse of the export type mapping. Uncompressed bodies only; a
 * DictionaryBatch or a compressed RecordBatch is rejected. Because it parses an
 * external file, every metadata and body read is bounds-checked and a
 * malformed file raises ERRCODE_DATA_CORRUPTED rather than reading out of
 * bounds.
 * ========================================================================= */

#define IMPORT_CORRUPT(msg) \
	ereport(ERROR, \
			(errcode(ERRCODE_DATA_CORRUPTED), \
			 errmsg("columnar: malformed Arrow IPC file: %s", (msg))))

/* ---- bounds-checked little-endian FlatBuffers reader ---- */
static uint8
fbr_u8(const uint8 *b, uint32 len, uint32 pos)
{
	if ((uint64) pos + 1 > len)
		IMPORT_CORRUPT("truncated u8");
	return b[pos];
}
static uint16
fbr_u16(const uint8 *b, uint32 len, uint32 pos)
{
	uint16		v;

	if ((uint64) pos + 2 > len)
		IMPORT_CORRUPT("truncated u16");
	memcpy(&v, b + pos, 2);
	return v;
}
static uint32
fbr_u32(const uint8 *b, uint32 len, uint32 pos)
{
	uint32		v;

	if ((uint64) pos + 4 > len)
		IMPORT_CORRUPT("truncated u32");
	memcpy(&v, b + pos, 4);
	return v;
}
static int32
fbr_i32(const uint8 *b, uint32 len, uint32 pos)
{
	return (int32) fbr_u32(b, len, pos);
}
static int64
fbr_i64(const uint8 *b, uint32 len, uint32 pos)
{
	int64		v;

	if ((uint64) pos + 8 > len)
		IMPORT_CORRUPT("truncated i64");
	memcpy(&v, b + pos, 8);
	return v;
}

/* absolute position of field `i` of the table at `tab`, or 0 if absent */
static uint32
fb_field(const uint8 *b, uint32 len, uint32 tab, int i)
{
	int32		soff = fbr_i32(b, len, tab);
	int64		vt = (int64) tab - soff;
	uint16		vtsize;
	uint32		slot = 4 + (uint32) i * 2;
	uint16		voff;

	if (vt < 0 || (uint64) vt + 4 > len)
		IMPORT_CORRUPT("vtable out of bounds");
	vtsize = fbr_u16(b, len, (uint32) vt);
	if (slot >= vtsize)
		return 0;
	voff = fbr_u16(b, len, (uint32) vt + slot);
	if (voff == 0)
		return 0;
	return tab + voff;
}

/* follow the uoffset stored at `pos` to the object it points at */
static uint32
fb_indirect(const uint8 *b, uint32 len, uint32 pos)
{
	return pos + fbr_u32(b, len, pos);
}

/* Build a numeric input string for a 128-bit unscaled value at the given scale
 * (reverse of numeric_to_int128). */
static char *
int128_to_numeric_cstring(__int128 v, int scale)
{
	char		digs[48];
	int			n = 0;
	int			L,
				j;
	bool		neg = v < 0;
	unsigned __int128 u = neg ? -(unsigned __int128) v : (unsigned __int128) v;
	StringInfoData s;

	if (u == 0)
		digs[n++] = '0';
	while (u > 0 && n < (int) sizeof(digs))
	{
		digs[n++] = (char) ('0' + (int) (u % 10));
		u /= 10;
	}
	/* digs[] holds least-significant digit first; emit most-significant first */
	initStringInfo(&s);
	if (neg)
		appendStringInfoChar(&s, '-');
	L = n - scale;				/* number of integer digits */
	if (L <= 0)
	{
		appendStringInfoString(&s, "0.");
		for (j = 0; j < -L; j++)
			appendStringInfoChar(&s, '0');
		for (j = 0; j < n; j++)
			appendStringInfoChar(&s, digs[n - 1 - j]);
	}
	else
	{
		for (j = 0; j < L; j++)
			appendStringInfoChar(&s, digs[n - 1 - j]);
		if (scale > 0)
		{
			appendStringInfoChar(&s, '.');
			for (j = L; j < n; j++)
				appendStringInfoChar(&s, digs[n - 1 - j]);
		}
	}
	return s.data;
}

/* Per-column import plan, derived from the target table's tuple descriptor. */
typedef struct ImpCol
{
	ArrowKind	kind;
	int			width;			/* fixed-width byte size */
	int			scale;			/* decimal scale */
	int32		atttypmod;
	bool		needsInput;		/* A_UTF8: build via type input function */
	FmgrInfo	inFinfo;
	Oid			inTypioparam;
}			ImpCol;

/* Read a validity bit for row r from a bitmap buffer (empty bitmap = all valid). */
static inline bool
imp_is_null(const uint8 *body, int64 bufOff, int64 bufLen, int64 r)
{
	if (bufLen == 0)
		return false;			/* Arrow omits the bitmap when null_count == 0 */
	return ((body[bufOff + (r >> 3)] >> (r & 7)) & 1) == 0;
}

/*
 * columnar_import_arrow
 *		SQL: columnar.import_arrow(rel regclass, path text) -> bigint.
 *		Insert the rows of an Arrow IPC stream file into a columnar table;
 *		returns the number of rows inserted.
 */
Datum
columnar_import_arrow(PG_FUNCTION_ARGS)
{
	Oid			relid;
	char	   *path;
	Relation	rel;
	TupleDesc	tupdesc;
	int			ncols;
	ImpCol	   *plan;
	FILE	   *f;
	TupleTableSlot *slot;
	CommandId	cid;
	int64		total = 0;
	bool		sawSchema = false;
	int			i;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("relation and path must not be null")));
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to import from a server-side file")));

	relid = PG_GETARG_OID(0);
	path = text_to_cstring(PG_GETARG_TEXT_PP(1));

	rel = table_open(relid, RowExclusiveLock);
	if (!ColumnarIsColumnarRelation(relid))
	{
		table_close(rel, RowExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" is not a columnar table",
						RelationGetRelationName(rel))));
	}

	tupdesc = RelationGetDescr(rel);
	ncols = tupdesc->natts;

	plan = palloc0(sizeof(ImpCol) * ncols);
	for (i = 0; i < ncols; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);
		int			width,
					precision,
					scale;
		ArrowKind	kind;

		if (att->attisdropped)
		{
			table_close(rel, RowExclusiveLock);
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("columnar.import_arrow does not support dropped columns")));
		}
		kind = arrow_kind_for_type(att->atttypid, att->atttypmod,
								   &width, &precision, &scale);
		if (width < 0)
		{
			table_close(rel, RowExclusiveLock);
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("column \"%s\" has type %s, which columnar.import_arrow does not support",
							NameStr(att->attname),
							format_type_be(att->atttypid))));
		}
		plan[i].kind = kind;
		plan[i].width = width;
		plan[i].scale = scale;
		plan[i].atttypmod = att->atttypmod;
		plan[i].needsInput = (kind == A_UTF8);
		if (plan[i].needsInput)
		{
			Oid			infunc;

			getTypeInputInfo(att->atttypid, &infunc, &plan[i].inTypioparam);
			fmgr_info(infunc, &plan[i].inFinfo);
		}
	}

	f = AllocateFile(path, PG_BINARY_R);
	if (f == NULL)
	{
		table_close(rel, RowExclusiveLock);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\" for reading: %m", path)));
	}

	slot = table_slot_create(rel, NULL);
	cid = GetCurrentCommandId(true);

	for (;;)
	{
		uint32		first;
		uint32		metaLen;
		uint8	   *meta;
		int64		bodyLength;
		uint8	   *body = NULL;
		uint32		msg,
					hdr,
					pos;
		uint8		headerType;

		/* message framing: [0xFFFFFFFF] [metaLen] or [metaLen] (legacy) */
		if (fread(&first, 4, 1, f) != 1)
			break;				/* clean EOF */
		if (first == 0xFFFFFFFF)
		{
			if (fread(&metaLen, 4, 1, f) != 1)
				IMPORT_CORRUPT("truncated continuation");
		}
		else
			metaLen = first;
		if (metaLen == 0)
			break;				/* end-of-stream marker */

		meta = palloc(metaLen);
		if (fread(meta, 1, metaLen, f) != metaLen)
			IMPORT_CORRUPT("truncated metadata");

		msg = fb_indirect(meta, metaLen, 0);
		pos = fb_field(meta, metaLen, msg, 1);	/* header_type (u8) */
		headerType = pos ? fbr_u8(meta, metaLen, pos) : 0;
		pos = fb_field(meta, metaLen, msg, 2);	/* header (offset) */
		hdr = pos ? fb_indirect(meta, metaLen, pos) : 0;
		pos = fb_field(meta, metaLen, msg, 3);	/* bodyLength (i64) */
		bodyLength = pos ? fbr_i64(meta, metaLen, pos) : 0;
		if (bodyLength < 0)
			IMPORT_CORRUPT("negative body length");

		if (bodyLength > 0)
		{
			body = palloc((Size) bodyLength);
			if (fread(body, 1, bodyLength, f) != (size_t) bodyLength)
				IMPORT_CORRUPT("truncated body");
		}

		if (headerType == ARROW_MSG_Schema)
		{
			uint32		fieldsVecPos = hdr ? fb_field(meta, metaLen, hdr, 1) : 0;
			uint32		fieldsVec = fieldsVecPos ?
				fb_indirect(meta, metaLen, fieldsVecPos) : 0;
			uint32		nfields = fieldsVec ? fbr_u32(meta, metaLen, fieldsVec) : 0;

			if ((int) nfields != ncols)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("Arrow file has %u columns, target table has %d",
								nfields, ncols)));
			sawSchema = true;
		}
		else if (headerType == ARROW_MSG_RecordBatch)
		{
			int64		nrows;
			uint32		nodesVecPos,
						buffersVecPos,
						buffersVec;
			uint32		nbuffers;
			int			bufIdx = 0;
			int64		r;

			if (!sawSchema)
				IMPORT_CORRUPT("RecordBatch before Schema");
			if (!hdr)
				IMPORT_CORRUPT("missing RecordBatch header");
			if (fb_field(meta, metaLen, hdr, 3) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("columnar.import_arrow does not support compressed Arrow bodies")));

			pos = fb_field(meta, metaLen, hdr, 0);	/* length */
			nrows = pos ? fbr_i64(meta, metaLen, pos) : 0;
			nodesVecPos = fb_field(meta, metaLen, hdr, 1);
			buffersVecPos = fb_field(meta, metaLen, hdr, 2);
			buffersVec = buffersVecPos ? fb_indirect(meta, metaLen, buffersVecPos) : 0;
			nbuffers = buffersVec ? fbr_u32(meta, metaLen, buffersVec) : 0;
			(void) nodesVecPos;

			for (r = 0; r < nrows; r++)
			{
				CHECK_FOR_INTERRUPTS();
				ExecClearTuple(slot);

				bufIdx = 0;
				for (i = 0; i < ncols; i++)
				{
					ImpCol	   *c = &plan[i];
					int64		vOff,
								vLen;
					bool		isnull;

					/* validity buffer for this column */
					{
						uint32 base = buffersVec + 4 + (uint32) bufIdx * 16;

						if ((uint32) bufIdx >= nbuffers)
							IMPORT_CORRUPT("buffer index past end");
						vOff = fbr_i64(meta, metaLen, base);
						vLen = fbr_i64(meta, metaLen, base + 8);
						bufIdx++;
					}
					if (vOff < 0 || vLen < 0 || (uint64) vOff + vLen > (uint64) bodyLength)
						IMPORT_CORRUPT("validity buffer out of range");
					isnull = imp_is_null(body, vOff, vLen, r);

					if (c->kind == A_BOOL)
					{
						int64		dOff,
									dLen;
						uint32		base = buffersVec + 4 + (uint32) bufIdx * 16;

						if ((uint32) bufIdx >= nbuffers)
							IMPORT_CORRUPT("buffer index past end");
						dOff = fbr_i64(meta, metaLen, base);
						dLen = fbr_i64(meta, metaLen, base + 8);
						bufIdx++;
						if (dOff < 0 || dLen < 0 || (uint64) dOff + dLen > (uint64) bodyLength)
							IMPORT_CORRUPT("bool buffer out of range");
						if (isnull)
							slot->tts_isnull[i] = true;
						else
						{
							bool		v = ((body[dOff + (r >> 3)] >> (r & 7)) & 1) != 0;

							slot->tts_values[i] = BoolGetDatum(v);
							slot->tts_isnull[i] = false;
						}
					}
					else if (c->kind == A_UTF8 || c->kind == A_BINARY)
					{
						int64		oOff,
									oLen,
									dOff,
									dLen;
						uint32		ob = buffersVec + 4 + (uint32) bufIdx * 16;
						uint32		db;

						if ((uint32) bufIdx + 1 >= nbuffers)
							IMPORT_CORRUPT("buffer index past end");
						oOff = fbr_i64(meta, metaLen, ob);
						oLen = fbr_i64(meta, metaLen, ob + 8);
						bufIdx++;
						db = buffersVec + 4 + (uint32) bufIdx * 16;
						dOff = fbr_i64(meta, metaLen, db);
						dLen = fbr_i64(meta, metaLen, db + 8);
						bufIdx++;
						if (oOff < 0 || oLen < 0 || (uint64) oOff + oLen > (uint64) bodyLength ||
							dOff < 0 || dLen < 0 || (uint64) dOff + dLen > (uint64) bodyLength)
							IMPORT_CORRUPT("varlen buffer out of range");

						if (isnull)
							slot->tts_isnull[i] = true;
						else
						{
							int32		start,
										end,
										vlen;

							if ((uint64) oOff + (uint64) (r + 1) * 4 + 4 > (uint64) oOff + oLen)
								IMPORT_CORRUPT("offsets buffer too short");
							memcpy(&start, body + oOff + r * 4, 4);
							memcpy(&end, body + oOff + (r + 1) * 4, 4);
							vlen = end - start;
							if (start < 0 || vlen < 0 || (uint64) dOff + start + vlen > (uint64) dOff + dLen)
								IMPORT_CORRUPT("varlen value out of range");

							if (c->kind == A_BINARY)
							{
								bytea	   *out = (bytea *) palloc(vlen + VARHDRSZ);

								SET_VARSIZE(out, vlen + VARHDRSZ);
								memcpy(VARDATA(out), body + dOff + start, vlen);
								slot->tts_values[i] = PointerGetDatum(out);
							}
							else
							{
								char	   *str = palloc(vlen + 1);

								memcpy(str, body + dOff + start, vlen);
								str[vlen] = '\0';
								slot->tts_values[i] =
									InputFunctionCall(&c->inFinfo, str,
													  c->inTypioparam, c->atttypmod);
							}
							slot->tts_isnull[i] = false;
						}
					}
					else
					{
						/* fixed-width value buffer */
						int64		dOff,
									dLen;
						uint32		base = buffersVec + 4 + (uint32) bufIdx * 16;
						const uint8 *vp;

						if ((uint32) bufIdx >= nbuffers)
							IMPORT_CORRUPT("buffer index past end");
						dOff = fbr_i64(meta, metaLen, base);
						dLen = fbr_i64(meta, metaLen, base + 8);
						bufIdx++;
						if (dOff < 0 || dLen < 0 ||
							(uint64) dOff + (uint64) (r + 1) * c->width > (uint64) dOff + dLen ||
							(uint64) dOff + dLen > (uint64) bodyLength)
							IMPORT_CORRUPT("fixed buffer out of range");
						vp = body + dOff + r * c->width;

						if (isnull)
						{
							slot->tts_isnull[i] = true;
						}
						else
						{
							Datum		d = (Datum) 0;

							switch (c->kind)
							{
								case A_INT16:
									{
										int16 v;
										memcpy(&v, vp, 2); d = Int16GetDatum(v);
										break;
									}
								case A_INT32:
									{
										int32 v;
										memcpy(&v, vp, 4); d = Int32GetDatum(v);
										break;
									}
								case A_INT64:
									{
										int64 v;
										memcpy(&v, vp, 8); d = Int64GetDatum(v);
										break;
									}
								case A_FLOAT32:
									{
										float4 v;
										memcpy(&v, vp, 4); d = Float4GetDatum(v);
										break;
									}
								case A_FLOAT64:
									{
										float8 v;
										memcpy(&v, vp, 8); d = Float8GetDatum(v);
										break;
									}
								case A_DATE32:
									{
										int32 v;
										memcpy(&v, vp, 4);
										d = DateADTGetDatum((DateADT) (v - PG_TO_UNIX_DAYS));
										break;
									}
								case A_TIME64:
									{
										int64 v;
										memcpy(&v, vp, 8); d = TimeADTGetDatum(v);
										break;
									}
								case A_TIMESTAMP:
								case A_TIMESTAMPTZ:
									{
										int64 v;
										memcpy(&v, vp, 8);
										d = TimestampGetDatum((Timestamp) (v - PG_TO_UNIX_USECS));
										break;
									}
								case A_UUID:
									{
										pg_uuid_t *uu = palloc(sizeof(pg_uuid_t));
										memcpy(uu->data, vp, UUID_LEN);
										d = UUIDPGetDatum(uu);
										break;
									}
								case A_DECIMAL128:
									{
										__int128 v;
										char	*str;
										memcpy(&v, vp, 16);
										str = int128_to_numeric_cstring(v, c->scale);
										d = DirectFunctionCall3(numeric_in,
																CStringGetDatum(str),
																ObjectIdGetDatum(InvalidOid),
																Int32GetDatum(c->atttypmod));
										break;
									}
								default:
									IMPORT_CORRUPT("unexpected fixed kind");
							}
							slot->tts_values[i] = d;
							slot->tts_isnull[i] = false;
						}
					}
				}

				ExecStoreVirtualTuple(slot);
				table_tuple_insert(rel, slot, cid, 0, NULL);
				total++;
			}
		}
		else if (headerType == ARROW_MSG_DictionaryBatch)
		{
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("columnar.import_arrow does not support dictionary-encoded Arrow files")));
		}
		/* any other message type is ignored */

		if (body)
			pfree(body);
		pfree(meta);
	}

	FreeFile(f);
	ExecDropSingleTupleTableSlot(slot);
	table_close(rel, RowExclusiveLock);
	PG_RETURN_INT64(total);
}
