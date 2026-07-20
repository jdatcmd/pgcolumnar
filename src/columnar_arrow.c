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
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

PG_FUNCTION_INFO_V1(columnar_export_arrow);

/* one RecordBatch per this many rows */
#define ARROW_BATCH_ROWS 16384

/* Arrow Type union tags (Schema.fbs) */
#define ARROW_TYPE_Int			2
#define ARROW_TYPE_FloatingPoint 3
#define ARROW_TYPE_Binary		4
#define ARROW_TYPE_Utf8			5
#define ARROW_TYPE_Bool			6
/* Arrow MessageHeader union tags (Message.fbs) */
#define ARROW_MSG_Schema		1
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
	A_BINARY
}			ArrowKind;

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
fb_arrow_type(FBB *b, ArrowKind kind, uint8 *typetag)
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
arrow_kind_for_type(Oid typid, int *width)
{
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
			*width = 0;
			return A_UTF8;
		case BYTEAOID:
			*width = 0;
			return A_BINARY;
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
		case A_BOOL:
			appendStringInfoChar(&c->boolvals,
								 (!isnull && DatumGetBool(d)) ? 1 : 0);
			break;
		case A_UTF8:
		case A_BINARY:
			{
				if (!isnull)
				{
					struct varlena *v = PG_DETOAST_DATUM_PACKED(d);
					int			len = VARSIZE_ANY_EXHDR(v);

					appendBinaryStringInfo(&c->vardata, VARDATA_ANY(v), len);
					c->running += len;
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
		ArrowKind	kind;

		if (att->attisdropped)
		{
			table_close(rel, AccessShareLock);
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("columnar.export_arrow does not support dropped columns")));
		}
		kind = arrow_kind_for_type(att->atttypid, &width);
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
		uint32		typeOff = fb_arrow_type(&b, cols[i].kind, &typetag);

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
