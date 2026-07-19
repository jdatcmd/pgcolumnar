/*-------------------------------------------------------------------------
 *
 * columnar_encoding.c
 *		Lightweight, type-aware value-stream encodings (I1) and the
 *		compression-block abstraction they implement (I2).
 *
 * The encoding layer sits between the raw serialized value stream that the
 * writer builds (a packed sequence of ColumnarEncodeValue outputs, spec 4) and
 * the general-purpose block codec (columnar_compression.c, spec 5). An encoding
 * is a reversible transform of the raw value-stream BYTES: encode(raw) -> a
 * smaller encoded buffer, and decode(encoded) -> the byte-identical raw stream.
 * Because decode reconstructs the exact raw stream, every downstream consumer
 * (per-value ColumnarDecodeValue, the vectorized group decoder, the min/max
 * skip list) is unchanged; only the flush and load paths gain one step.
 *
 * The techniques come from the public column-store literature (see
 * design/IMPROVEMENT_PLAN.md): run-length encoding and frame-of-reference with
 * bit-packing [C-Store; Abadi SIGMOD 2006], and delta+bit-packing
 * [Abadi SIGMOD 2006; Zukowski et al. Super-Scalar Compression]. This file is
 * an independent implementation written from those descriptions and the public
 * PostgreSQL API only (clean-room; see PROVENANCE.md).
 *
 * On-disk: columnar.chunk records value_encoding_type per chunk, and
 * value_raw_length (the decoded raw stream length). A NULL/absent encoding type
 * (old format 2.0 chunks) is treated as NONE, so 2.0 files still read.
 *
 *-------------------------------------------------------------------------
 */
#include "columnar.h"

#include "catalog/pg_type.h"
#include "utils/memutils.h"

/* -------------------------------------------------------------------------
 * fixed-width value load/store and bit-packing helpers
 * ------------------------------------------------------------------------- */

/*
 * Load/store a fixed-width value as a uint64. We copy the low W bytes so the
 * transform is exactly reversible on any architecture; the integer value is
 * only used to cluster/delta and never leaves this file, so its interpretation
 * (little-endian on x86) does not affect correctness of the round trip.
 */
static inline uint64
load_uint(const char *p, int w)
{
	uint64		v = 0;

	memcpy(&v, p, w);
	return v;
}

static inline void
store_uint(char *p, uint64 v, int w)
{
	memcpy(p, &v, w);
}

/* number of bits needed to represent maxval; 0 when maxval == 0 */
static int
bits_needed(uint64 maxval)
{
	int			n = 0;

	while (maxval > 0)
	{
		n++;
		maxval >>= 1;
	}
	return n;
}

/* pack n values of `width` bits each (LSB-first) and append to out */
static void
bitpack(const uint64 *vals, uint32 n, int width, StringInfo out)
{
	uint64		totalbits;
	uint32		nbytes;
	unsigned char *buf;
	uint64		bitpos = 0;
	uint32		i;

	if (width == 0 || n == 0)
		return;

	totalbits = (uint64) n * (uint64) width;
	nbytes = (uint32) ((totalbits + 7) / 8);
	buf = palloc0(nbytes);

	for (i = 0; i < n; i++)
	{
		uint64		v = vals[i];
		int			b;

		for (b = 0; b < width; b++)
		{
			if ((v >> b) & 1)
				buf[(bitpos + b) >> 3] |= (unsigned char) (1u << ((bitpos + b) & 7));
		}
		bitpos += width;
	}

	appendBinaryStringInfo(out, (char *) buf, nbytes);
	pfree(buf);
}

/* inverse of bitpack: unpack n values of `width` bits each into out[] */
static void
bitunpack(const unsigned char *in, uint32 n, int width, uint64 *out)
{
	uint64		bitpos = 0;
	uint32		i;

	if (width == 0)
	{
		for (i = 0; i < n; i++)
			out[i] = 0;
		return;
	}

	for (i = 0; i < n; i++)
	{
		uint64		v = 0;
		int			b;

		for (b = 0; b < width; b++)
		{
			if ((in[(bitpos + b) >> 3] >> ((bitpos + b) & 7)) & 1)
				v |= (uint64) 1 << b;
		}
		out[i] = v;
		bitpos += width;
	}
}

/* zigzag map signed<->unsigned so small-magnitude deltas pack tightly */
static inline uint64
zigzag(int64 x)
{
	return ((uint64) x << 1) ^ (uint64) (x >> 63);
}

static inline int64
unzigzag(uint64 z)
{
	return (int64) (z >> 1) ^ -(int64) (z & 1);
}

/* is this a fixed-width integer-family value we can FOR/DELTA/DOD encode? */
static inline bool
is_packable_int(Form_pg_attribute att)
{
	return att->attbyval &&
		(att->attlen == 1 || att->attlen == 2 ||
		 att->attlen == 4 || att->attlen == 8);
}

/* is this a float we can Gorilla-encode? */
static inline bool
is_gorilla_float(Form_pg_attribute att)
{
	return att->attbyval &&
		(att->atttypid == FLOAT4OID || att->atttypid == FLOAT8OID);
}

/* -------------------------------------------------------------------------
 * RLE: runs of an equal fixed-width value, [uint32 count][W bytes] per run
 * ------------------------------------------------------------------------- */

static bool
encode_rle(const char *raw, uint32 rawLen, int w, uint32 n,
		   char **out, uint32 *outLen)
{
	StringInfoData buf;
	uint32		i = 0;

	initStringInfo(&buf);
	while (i < n)
	{
		const char *val = raw + (uint64) i * w;
		uint32		run = 1;

		while (i + run < n &&
			   memcmp(raw + (uint64) (i + run) * w, val, w) == 0)
			run++;

		appendBinaryStringInfo(&buf, (char *) &run, sizeof(uint32));
		appendBinaryStringInfo(&buf, val, w);
		i += run;

		/* give up early if we are not beating the raw size */
		if ((uint32) buf.len >= rawLen)
		{
			pfree(buf.data);
			return false;
		}
	}

	*out = buf.data;
	*outLen = buf.len;
	return true;
}

static char *
decode_rle(const char *enc, uint32 encLen, int w, uint32 n, uint32 rawLen,
		   MemoryContext cx)
{
	char	   *raw = MemoryContextAlloc(cx, rawLen > 0 ? rawLen : 1);
	const char *p = enc;
	const char *end = enc + encLen;
	uint32		produced = 0;

	while (p < end && produced < n)
	{
		uint32		run;

		memcpy(&run, p, sizeof(uint32));
		p += sizeof(uint32);
		while (run-- > 0 && produced < n)
		{
			memcpy(raw + (uint64) produced * w, p, w);
			produced++;
		}
		p += w;
	}
	return raw;
}

/* -------------------------------------------------------------------------
 * FOR: frame of reference + bit-packing.
 * header: [uint8 w][uint8 width][uint64 min], body: bit-packed (value - min)
 * ------------------------------------------------------------------------- */

static bool
encode_for(const char *raw, uint32 rawLen, int w, uint32 n,
		   char **out, uint32 *outLen)
{
	uint64	   *vals = palloc(sizeof(uint64) * n);
	uint64		minv;
	uint64		maxoff = 0;
	int			width;
	StringInfoData buf;
	uint32		i;

	for (i = 0; i < n; i++)
		vals[i] = load_uint(raw + (uint64) i * w, w);

	minv = vals[0];
	for (i = 1; i < n; i++)
		if (vals[i] < minv)
			minv = vals[i];
	for (i = 0; i < n; i++)
	{
		vals[i] -= minv;
		if (vals[i] > maxoff)
			maxoff = vals[i];
	}
	width = bits_needed(maxoff);

	initStringInfo(&buf);
	appendStringInfoChar(&buf, (char) w);
	appendStringInfoChar(&buf, (char) width);
	appendBinaryStringInfo(&buf, (char *) &minv, sizeof(uint64));
	bitpack(vals, n, width, &buf);
	pfree(vals);

	if ((uint32) buf.len >= rawLen)
	{
		pfree(buf.data);
		return false;
	}
	*out = buf.data;
	*outLen = buf.len;
	return true;
}

static char *
decode_for(const char *enc, uint32 encLen, uint32 n, uint32 rawLen,
		   MemoryContext cx)
{
	char	   *raw = MemoryContextAlloc(cx, rawLen > 0 ? rawLen : 1);
	int			w = (unsigned char) enc[0];
	int			width = (unsigned char) enc[1];
	uint64		minv;
	uint64	   *vals;
	uint32		i;

	memcpy(&minv, enc + 2, sizeof(uint64));
	vals = palloc(sizeof(uint64) * (n > 0 ? n : 1));
	bitunpack((const unsigned char *) enc + 10, n, width, vals);
	for (i = 0; i < n; i++)
		store_uint(raw + (uint64) i * w, vals[i] + minv, w);
	pfree(vals);
	return raw;
}

/* -------------------------------------------------------------------------
 * DELTA: store first value, then zigzag(value[i]-value[i-1]) bit-packed.
 * header: [uint8 w][uint8 width][uint64 base], body: bit-packed zigzag deltas
 * ------------------------------------------------------------------------- */

static bool
encode_delta(const char *raw, uint32 rawLen, int w, uint32 n,
			 char **out, uint32 *outLen)
{
	uint64	   *vals = palloc(sizeof(uint64) * n);
	uint64	   *deltas;
	uint64		base;
	uint64		maxz = 0;
	int			width;
	StringInfoData buf;
	uint32		i;

	for (i = 0; i < n; i++)
		vals[i] = load_uint(raw + (uint64) i * w, w);

	base = vals[0];
	deltas = palloc(sizeof(uint64) * (n > 1 ? n - 1 : 1));
	for (i = 1; i < n; i++)
	{
		int64		d = (int64) vals[i] - (int64) vals[i - 1];
		uint64		z = zigzag(d);

		deltas[i - 1] = z;
		if (z > maxz)
			maxz = z;
	}
	width = bits_needed(maxz);

	initStringInfo(&buf);
	appendStringInfoChar(&buf, (char) w);
	appendStringInfoChar(&buf, (char) width);
	appendBinaryStringInfo(&buf, (char *) &base, sizeof(uint64));
	if (n > 1)
		bitpack(deltas, n - 1, width, &buf);
	pfree(vals);
	pfree(deltas);

	if ((uint32) buf.len >= rawLen)
	{
		pfree(buf.data);
		return false;
	}
	*out = buf.data;
	*outLen = buf.len;
	return true;
}

static char *
decode_delta(const char *enc, uint32 encLen, uint32 n, uint32 rawLen,
			 MemoryContext cx)
{
	char	   *raw = MemoryContextAlloc(cx, rawLen > 0 ? rawLen : 1);
	int			w = (unsigned char) enc[0];
	int			width = (unsigned char) enc[1];
	uint64		base;
	uint64	   *deltas;
	uint64		cur;
	uint32		i;

	memcpy(&base, enc + 2, sizeof(uint64));
	deltas = palloc(sizeof(uint64) * (n > 1 ? n - 1 : 1));
	if (n > 1)
		bitunpack((const unsigned char *) enc + 10, n - 1, width, deltas);

	cur = base;
	if (n > 0)
		store_uint(raw, cur, w);
	for (i = 1; i < n; i++)
	{
		cur = (uint64) ((int64) cur + unzigzag(deltas[i - 1]));
		store_uint(raw + (uint64) i * w, cur, w);
	}
	pfree(deltas);
	return raw;
}

/* -------------------------------------------------------------------------
 * streaming bit writer/reader (LSB-first) for variable-width fields
 * ------------------------------------------------------------------------- */

typedef struct BitWriter
{
	StringInfoData buf;
	uint8		cur;
	int			nbits;			/* bits currently held in cur (0..7) */
} BitWriter;

static void
bw_init(BitWriter *bw)
{
	initStringInfo(&bw->buf);
	bw->cur = 0;
	bw->nbits = 0;
}

/* append the low `bits` bits of v, least-significant first */
static void
bw_put(BitWriter *bw, uint64 v, int bits)
{
	int			i;

	for (i = 0; i < bits; i++)
	{
		if ((v >> i) & 1)
			bw->cur |= (uint8) (1u << bw->nbits);
		bw->nbits++;
		if (bw->nbits == 8)
		{
			appendStringInfoChar(&bw->buf, (char) bw->cur);
			bw->cur = 0;
			bw->nbits = 0;
		}
	}
}

static void
bw_flush(BitWriter *bw)
{
	if (bw->nbits > 0)
	{
		appendStringInfoChar(&bw->buf, (char) bw->cur);
		bw->cur = 0;
		bw->nbits = 0;
	}
}

typedef struct BitReader
{
	const unsigned char *data;
	uint64		bitpos;
} BitReader;

static void
br_init(BitReader *br, const unsigned char *data)
{
	br->data = data;
	br->bitpos = 0;
}

static uint64
br_get(BitReader *br, int bits)
{
	uint64		v = 0;
	int			i;

	for (i = 0; i < bits; i++)
	{
		uint64		p = br->bitpos + i;

		if ((br->data[p >> 3] >> (p & 7)) & 1)
			v |= (uint64) 1 << i;
	}
	br->bitpos += bits;
	return v;
}

/* leading/trailing zero counts of a non-zero value within a `total`-bit window */
static inline int
clz_in(uint64 x, int total)
{
	int			n = 0;

	while (n < total && ((x >> (total - 1 - n)) & 1) == 0)
		n++;
	return n;
}

static inline int
ctz_in(uint64 x)
{
	int			n = 0;

	while (((x >> n) & 1) == 0)
		n++;
	return n;
}

/* -------------------------------------------------------------------------
 * GORILLA: XOR-against-previous for float4/float8 (Facebook Gorilla, VLDB 2015),
 * simplified without the previous-window reuse. Per value after the first:
 *   XOR==0            -> control bit 0
 *   else              -> control bit 1, [lz][mlen-1][mlen meaningful bits]
 * where lz/len fields are 5 bits for 32-bit values, 6 bits for 64-bit.
 * ------------------------------------------------------------------------- */

static bool
encode_gorilla(const char *raw, uint32 rawLen, int w, uint32 n,
			   char **out, uint32 *outLen)
{
	int			total = w * 8;
	int			field = (w == 8) ? 6 : 5;
	BitWriter	bw;
	uint64		prev;
	uint32		i;

	bw_init(&bw);
	prev = load_uint(raw, w);
	bw_put(&bw, prev, total);	/* first value verbatim */

	for (i = 1; i < n; i++)
	{
		uint64		v = load_uint(raw + (uint64) i * w, w);
		uint64		x = v ^ prev;

		if (x == 0)
			bw_put(&bw, 0, 1);
		else
		{
			int			lz = clz_in(x, total);
			int			tz = ctz_in(x);
			int			mlen = total - lz - tz;

			bw_put(&bw, 1, 1);
			bw_put(&bw, (uint64) lz, field);
			bw_put(&bw, (uint64) (mlen - 1), field);
			bw_put(&bw, x >> tz, mlen);
		}
		prev = v;
	}
	bw_flush(&bw);

	if ((uint32) bw.buf.len >= rawLen)
	{
		pfree(bw.buf.data);
		return false;
	}
	*out = bw.buf.data;
	*outLen = bw.buf.len;
	return true;
}

static char *
decode_gorilla(const char *enc, uint32 encLen, int w, uint32 n, uint32 rawLen,
			   MemoryContext cx)
{
	char	   *raw = MemoryContextAlloc(cx, rawLen > 0 ? rawLen : 1);
	int			total = w * 8;
	int			field = (w == 8) ? 6 : 5;
	BitReader	br;
	uint64		prev;
	uint32		i;

	br_init(&br, (const unsigned char *) enc);
	prev = br_get(&br, total);
	if (n > 0)
		store_uint(raw, prev, w);

	for (i = 1; i < n; i++)
	{
		uint64		v;

		if (br_get(&br, 1) == 0)
			v = prev;
		else
		{
			int			lz = (int) br_get(&br, field);
			int			mlen = (int) br_get(&br, field) + 1;
			int			tz = total - lz - mlen;
			uint64		meaningful = br_get(&br, mlen);

			v = prev ^ (meaningful << tz);
		}
		store_uint(raw + (uint64) i * w, v, w);
		prev = v;
	}
	return raw;
}

/* -------------------------------------------------------------------------
 * DOD: delta-of-delta + zigzag + bit-packing, for fixed-width int-family types.
 * Regular sequences (e.g. fixed-interval timestamps) have zero second
 * differences and pack to almost nothing.
 * header: [uint8 w][uint8 width][uint64 base][int64 firstDelta]
 * body: bit-packed zigzag(delta[i]-delta[i-1]) for i >= 2
 * ------------------------------------------------------------------------- */

static bool
encode_dod(const char *raw, uint32 rawLen, int w, uint32 n,
		   char **out, uint32 *outLen)
{
	uint64	   *vals = palloc(sizeof(uint64) * n);
	uint64	   *dods;
	uint64		base;
	int64		firstDelta;
	uint64		maxz = 0;
	int			width;
	StringInfoData buf;
	uint32		i;

	for (i = 0; i < n; i++)
		vals[i] = load_uint(raw + (uint64) i * w, w);

	base = vals[0];
	firstDelta = (n > 1) ? (int64) vals[1] - (int64) vals[0] : 0;
	dods = palloc(sizeof(uint64) * (n > 2 ? n - 2 : 1));
	for (i = 2; i < n; i++)
	{
		int64		d = (int64) vals[i] - (int64) vals[i - 1];
		int64		dd = d - ((int64) vals[i - 1] - (int64) vals[i - 2]);
		uint64		z = zigzag(dd);

		dods[i - 2] = z;
		if (z > maxz)
			maxz = z;
	}
	width = bits_needed(maxz);

	initStringInfo(&buf);
	appendStringInfoChar(&buf, (char) w);
	appendStringInfoChar(&buf, (char) width);
	appendBinaryStringInfo(&buf, (char *) &base, sizeof(uint64));
	appendBinaryStringInfo(&buf, (char *) &firstDelta, sizeof(int64));
	if (n > 2)
		bitpack(dods, n - 2, width, &buf);
	pfree(vals);
	pfree(dods);

	if ((uint32) buf.len >= rawLen)
	{
		pfree(buf.data);
		return false;
	}
	*out = buf.data;
	*outLen = buf.len;
	return true;
}

static char *
decode_dod(const char *enc, uint32 encLen, uint32 n, uint32 rawLen,
		   MemoryContext cx)
{
	char	   *raw = MemoryContextAlloc(cx, rawLen > 0 ? rawLen : 1);
	int			w = (unsigned char) enc[0];
	int			width = (unsigned char) enc[1];
	uint64		base;
	int64		firstDelta;
	uint64	   *dods;
	int64		delta;
	uint64		cur;
	uint32		i;

	memcpy(&base, enc + 2, sizeof(uint64));
	memcpy(&firstDelta, enc + 10, sizeof(int64));
	dods = palloc(sizeof(uint64) * (n > 2 ? n - 2 : 1));
	if (n > 2)
		bitunpack((const unsigned char *) enc + 18, n - 2, width, dods);

	cur = base;
	if (n > 0)
		store_uint(raw, cur, w);
	delta = firstDelta;
	for (i = 1; i < n; i++)
	{
		if (i >= 2)
			delta += unzigzag(dods[i - 2]);
		cur = (uint64) ((int64) cur + delta);
		store_uint(raw + (uint64) i * w, cur, w);
	}
	return raw;
}

/* -------------------------------------------------------------------------
 * public entry points
 * ------------------------------------------------------------------------- */

/* -------------------------------------------------------------------------
 * compression-block run iterator (I2)
 * ------------------------------------------------------------------------- */

void
ColumnarBlockReaderInit(ColumnarBlockReader *br, const char *raw,
						uint64 valueCount, int width)
{
	br->raw = raw;
	br->valueCount = valueCount;
	br->width = width;
	br->pos = 0;
}

bool
ColumnarBlockNextRun(ColumnarBlockReader *br, const char **valBytes,
					 uint64 *runLen)
{
	const char *v;
	uint64		run = 1;
	int			w = br->width;

	if (br->pos >= br->valueCount)
		return false;

	v = br->raw + br->pos * (uint64) w;
	while (br->pos + run < br->valueCount &&
		   memcmp(br->raw + (br->pos + run) * (uint64) w, v, w) == 0)
		run++;

	*valBytes = v;
	*runLen = run;
	br->pos += run;
	return true;
}

const char *
ColumnarEncodingName(int encodingType)
{
	switch (encodingType)
	{
		case COLUMNAR_ENCODING_NONE:
			return "none";
		case COLUMNAR_ENCODING_RLE:
			return "rle";
		case COLUMNAR_ENCODING_FOR:
			return "for";
		case COLUMNAR_ENCODING_DELTA:
			return "delta";
		case COLUMNAR_ENCODING_GORILLA:
			return "gorilla";
		case COLUMNAR_ENCODING_DOD:
			return "dod";
		default:
			return "unknown";
	}
}

/*
 * ColumnarEncodeChunk
 *		Choose and apply the best lightweight encoding for one chunk's raw value
 *		stream. Returns the encoding code used and sets out and outLen to the
 *		encoded buffer. When no encoding beats the raw size, returns NONE and
 *		sets out to the raw pointer itself (no copy); callers must treat it as
 *		read-only and must not free it. Encoded buffers for the other cases are
 *		palloc'd in the current memory context.
 *
 *		Selection here is deliberately simple (smallest pre-compression encoded
 *		size); adaptive, sampled selection is a later step (I5).
 */
int
ColumnarEncodeChunk(const char *raw, uint32 rawLen, Form_pg_attribute att,
					uint64 valueCount, char **out, uint32 *outLen)
{
	int			w = att->attlen;
	uint32		n = (uint32) valueCount;
	int			bestCode = COLUMNAR_ENCODING_NONE;
	char	   *bestBuf = NULL;
	uint32		bestLen = rawLen;
	char	   *buf;
	uint32		len;

	/* nothing to do for an empty (all-null) chunk */
	if (rawLen == 0 || n == 0)
	{
		*out = (char *) raw;
		*outLen = rawLen;
		return COLUMNAR_ENCODING_NONE;
	}

	/* lightweight encodings currently target fixed-width columns */
	if (w > 0)
	{
		if (encode_rle(raw, rawLen, w, n, &buf, &len) && len < bestLen)
		{
			bestCode = COLUMNAR_ENCODING_RLE;
			bestBuf = buf;
			bestLen = len;
		}

		if (is_packable_int(att))
		{
			if (encode_for(raw, rawLen, w, n, &buf, &len) && len < bestLen)
			{
				if (bestBuf)
					pfree(bestBuf);
				bestCode = COLUMNAR_ENCODING_FOR;
				bestBuf = buf;
				bestLen = len;
			}
			if (encode_delta(raw, rawLen, w, n, &buf, &len) && len < bestLen)
			{
				if (bestBuf)
					pfree(bestBuf);
				bestCode = COLUMNAR_ENCODING_DELTA;
				bestBuf = buf;
				bestLen = len;
			}
			if (encode_dod(raw, rawLen, w, n, &buf, &len) && len < bestLen)
			{
				if (bestBuf)
					pfree(bestBuf);
				bestCode = COLUMNAR_ENCODING_DOD;
				bestBuf = buf;
				bestLen = len;
			}
		}

		if (is_gorilla_float(att) &&
			encode_gorilla(raw, rawLen, w, n, &buf, &len) && len < bestLen)
		{
			if (bestBuf)
				pfree(bestBuf);
			bestCode = COLUMNAR_ENCODING_GORILLA;
			bestBuf = buf;
			bestLen = len;
		}
	}

	if (bestCode == COLUMNAR_ENCODING_NONE)
	{
		*out = (char *) raw;
		*outLen = rawLen;
		return COLUMNAR_ENCODING_NONE;
	}

	*out = bestBuf;
	*outLen = bestLen;
	return bestCode;
}

/*
 * ColumnarDecodeChunk
 *		Reverse an encoding, reconstructing the byte-identical raw value stream
 *		in cx. For NONE, returns the input pointer unchanged (no copy).
 */
char *
ColumnarDecodeChunk(const char *enc, uint32 encLen, int encodingType,
					Form_pg_attribute att, uint64 valueCount, uint32 rawLen,
					MemoryContext cx)
{
	uint32		n = (uint32) valueCount;

	switch (encodingType)
	{
		case COLUMNAR_ENCODING_NONE:
			return (char *) enc;
		case COLUMNAR_ENCODING_RLE:
			return decode_rle(enc, encLen, att->attlen, n, rawLen, cx);
		case COLUMNAR_ENCODING_FOR:
			return decode_for(enc, encLen, n, rawLen, cx);
		case COLUMNAR_ENCODING_DELTA:
			return decode_delta(enc, encLen, n, rawLen, cx);
		case COLUMNAR_ENCODING_GORILLA:
			return decode_gorilla(enc, encLen, att->attlen, n, rawLen, cx);
		case COLUMNAR_ENCODING_DOD:
			return decode_dod(enc, encLen, n, rawLen, cx);
		default:
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("columnar: unknown value encoding type %d",
							encodingType)));
			return NULL;			/* keep the compiler happy */
	}
}
