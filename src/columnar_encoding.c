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

/*
 * Decoding runs on bytes read back from disk. Normal pgColumnar data is
 * self-consistent, but a corrupt catalog row, bit rot, or a crafted format-2.0
 * file can present lengths, counts, widths, and dictionary codes that do not
 * match the buffer. Every decoder validates its inputs and raises this rather
 * than reading or writing out of bounds.
 */
#define DECODE_CORRUPT(msg) \
	ereport(ERROR, \
			(errcode(ERRCODE_DATA_CORRUPTED), \
			 errmsg("columnar: corrupt encoded chunk (%s)", (msg))))

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

/* inverse of bitpack: unpack n values of `width` bits each into out[]. inLen is
 * the number of bytes available at `in`; reads past it are rejected. */
static void
bitunpack(const unsigned char *in, uint32 inLen, uint32 n, int width,
		  uint64 *out)
{
	uint64		bitpos = 0;
	uint32		i;

	if (width < 0 || width > 64)
		DECODE_CORRUPT("bit width out of range");

	if (width == 0)
	{
		for (i = 0; i < n; i++)
			out[i] = 0;
		return;
	}

	/* bytes needed to hold n values of `width` bits, LSB-first */
	if (((uint64) n * (uint32) width + 7) / 8 > inLen)
		DECODE_CORRUPT("bit-packed body exceeds encoded length");

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

/* is this a fixed-width integer-family value we can FOR/DELTA/DOD encode?
 * Floats are excluded: their bit patterns are not integers, and they have their
 * own encoding (gorilla). Applying integer delta to float bits happens to help
 * same-exponent runs but is not the right tool. */
static inline bool
is_packable_int(Form_pg_attribute att)
{
	return att->attbyval &&
		att->atttypid != FLOAT4OID && att->atttypid != FLOAT8OID &&
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

	while (produced < n)
	{
		uint32		run;

		if (p + sizeof(uint32) > end)
			DECODE_CORRUPT("RLE run header past encoded length");
		memcpy(&run, p, sizeof(uint32));
		p += sizeof(uint32);
		if (p + w > end)
			DECODE_CORRUPT("RLE run value past encoded length");
		while (run-- > 0 && produced < n)
		{
			memcpy(raw + (uint64) produced * w, p, w);	/* produced < n; n*w == rawLen */
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
	int			w;
	int			width;
	uint64		minv;
	uint64	   *vals;
	uint32		i;

	if (encLen < 10)			/* [u8 w][u8 width][u64 min] */
		DECODE_CORRUPT("FOR header truncated");
	w = (unsigned char) enc[0];
	width = (unsigned char) enc[1];
	if (w != 1 && w != 2 && w != 4 && w != 8)
		DECODE_CORRUPT("FOR element width invalid");
	if ((uint64) n * (uint32) w != rawLen)
		DECODE_CORRUPT("FOR raw length mismatch");

	memcpy(&minv, enc + 2, sizeof(uint64));
	vals = palloc(sizeof(uint64) * (n > 0 ? n : 1));
	bitunpack((const unsigned char *) enc + 10, encLen - 10, n, width, vals);
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
	int			w;
	int			width;
	uint64		base;
	uint64	   *deltas;
	uint64		cur;
	uint32		i;

	if (encLen < 10)			/* [u8 w][u8 width][u64 base] */
		DECODE_CORRUPT("DELTA header truncated");
	w = (unsigned char) enc[0];
	width = (unsigned char) enc[1];
	if (w != 1 && w != 2 && w != 4 && w != 8)
		DECODE_CORRUPT("DELTA element width invalid");
	if ((uint64) n * (uint32) w != rawLen)
		DECODE_CORRUPT("DELTA raw length mismatch");

	memcpy(&base, enc + 2, sizeof(uint64));
	deltas = palloc(sizeof(uint64) * (n > 1 ? n - 1 : 1));
	if (n > 1)
		bitunpack((const unsigned char *) enc + 10, encLen - 10, n - 1, width,
				  deltas);

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
	uint64		nbits;			/* total readable bits (bytes * 8) */
	uint64		bitpos;
} BitReader;

static void
br_init(BitReader *br, const unsigned char *data, uint32 nbytes)
{
	br->data = data;
	br->nbits = (uint64) nbytes * 8;
	br->bitpos = 0;
}

static uint64
br_get(BitReader *br, int bits)
{
	uint64		v = 0;
	int			i;

	if (bits < 0 || bits > 64)
		DECODE_CORRUPT("bit field width out of range");
	if (br->bitpos + (uint64) bits > br->nbits)
		DECODE_CORRUPT("bit reader ran past encoded length");

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

	br_init(&br, (const unsigned char *) enc, encLen);
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
			uint64		meaningful;

			if (lz < 0 || mlen < 1 || mlen > total || tz < 0)
				DECODE_CORRUPT("GORILLA XOR field out of range");
			meaningful = br_get(&br, mlen);
			v = prev ^ (meaningful << tz);
		}
		store_uint(raw + (uint64) i * w, v, w);	/* i < n; n*w == rawLen */
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
	int			w;
	int			width;
	uint64		base;
	int64		firstDelta;
	uint64	   *dods;
	int64		delta;
	uint64		cur;
	uint32		i;

	if (encLen < 18)			/* [u8 w][u8 width][u64 base][i64 firstDelta] */
		DECODE_CORRUPT("DOD header truncated");
	w = (unsigned char) enc[0];
	width = (unsigned char) enc[1];
	if (w != 1 && w != 2 && w != 4 && w != 8)
		DECODE_CORRUPT("DOD element width invalid");
	if ((uint64) n * (uint32) w != rawLen)
		DECODE_CORRUPT("DOD raw length mismatch");

	memcpy(&base, enc + 2, sizeof(uint64));
	memcpy(&firstDelta, enc + 10, sizeof(int64));
	dods = palloc(sizeof(uint64) * (n > 2 ? n - 2 : 1));
	if (n > 2)
		bitunpack((const unsigned char *) enc + 18, encLen - 18, n - 2, width,
				  dods);

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
 * DICT: dictionary encoding for low-cardinality columns, fixed-width or varlena.
 * This is the cardinality axis of per-chunk selection (I5): distinct values are
 * stored once and each position becomes a small bit-packed code.
 *   [uint32 nDistinct][dictionary][uint8 codeWidth][bit-packed codes]
 * dictionary is nDistinct fixed-width values, or for varlena each entry is
 * [uint32 len][len bytes]. Only attempted up to a bounded distinct count.
 * ------------------------------------------------------------------------- */

#define DICT_MAX_DISTINCT 1024

static bool
encode_dict(const char *raw, uint32 rawLen, Form_pg_attribute att, uint32 n,
			char **out, uint32 *outLen)
{
	int			w = att->attlen;
	uint32	   *codes = palloc(sizeof(uint32) * n);
	uint32		distOff[DICT_MAX_DISTINCT];
	uint32		distLen[DICT_MAX_DISTINCT];
	int			nd = 0;
	uint64	   *codes64;
	int			codeWidth;
	StringInfoData buf;
	uint32		pos = 0;
	uint32		i;
	int			j;

	for (i = 0; i < n; i++)
	{
		const char *vp = raw + pos;
		uint32		vlen = (w > 0) ? (uint32) w : (uint32) VARSIZE_ANY(vp);
		int			code = -1;

		for (j = 0; j < nd; j++)
			if (distLen[j] == vlen &&
				memcmp(raw + distOff[j], vp, vlen) == 0)
			{
				code = j;
				break;
			}

		if (code < 0)
		{
			if (nd >= DICT_MAX_DISTINCT)
			{
				pfree(codes);
				return false;	/* too many distinct values for a dictionary */
			}
			distOff[nd] = pos;
			distLen[nd] = vlen;
			code = nd;
			nd++;
		}
		codes[i] = (uint32) code;
		pos += vlen;
	}

	codeWidth = bits_needed(nd > 0 ? (uint64) (nd - 1) : 0);

	initStringInfo(&buf);
	{
		uint32		ndVal = (uint32) nd;

		appendBinaryStringInfo(&buf, (char *) &ndVal, sizeof(uint32));
	}
	for (j = 0; j < nd; j++)
	{
		if (w > 0)
			appendBinaryStringInfo(&buf, raw + distOff[j], w);
		else
		{
			appendBinaryStringInfo(&buf, (char *) &distLen[j], sizeof(uint32));
			appendBinaryStringInfo(&buf, raw + distOff[j], distLen[j]);
		}
	}
	appendStringInfoChar(&buf, (char) codeWidth);

	codes64 = palloc(sizeof(uint64) * n);
	for (i = 0; i < n; i++)
		codes64[i] = codes[i];
	bitpack(codes64, n, codeWidth, &buf);
	pfree(codes64);
	pfree(codes);

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
decode_dict(const char *enc, uint32 encLen, Form_pg_attribute att, uint32 n,
			uint32 rawLen, MemoryContext cx)
{
	int			w = att->attlen;
	char	   *raw = MemoryContextAlloc(cx, rawLen > 0 ? rawLen : 1);
	const char *p = enc;
	const char *end = enc + encLen;
	uint32		nd;
	const char **dptr;
	uint32	   *dlen;
	int			codeWidth;
	uint64	   *codes;
	uint32		pos = 0;
	uint32		i;
	uint32		j;

	if (encLen < sizeof(uint32))
		DECODE_CORRUPT("DICT header truncated");
	memcpy(&nd, p, sizeof(uint32));
	p += sizeof(uint32);

	dptr = palloc(sizeof(char *) * (nd > 0 ? nd : 1));
	dlen = palloc(sizeof(uint32) * (nd > 0 ? nd : 1));
	for (j = 0; j < nd; j++)
	{
		if (w > 0)
		{
			if (p + w > end)
				DECODE_CORRUPT("DICT fixed entry past encoded length");
			dptr[j] = p;
			dlen[j] = (uint32) w;
			p += w;
		}
		else
		{
			if (p + sizeof(uint32) > end)
				DECODE_CORRUPT("DICT varlen header past encoded length");
			memcpy(&dlen[j], p, sizeof(uint32));
			p += sizeof(uint32);
			if (dlen[j] > (uint32) (end - p))
				DECODE_CORRUPT("DICT varlen entry past encoded length");
			dptr[j] = p;
			p += dlen[j];
		}
	}

	if (p + 1 > end)
		DECODE_CORRUPT("DICT code width past encoded length");
	codeWidth = (unsigned char) *p;
	p++;

	codes = palloc(sizeof(uint64) * (n > 0 ? n : 1));
	bitunpack((const unsigned char *) p, (uint32) (end - p), n, codeWidth, codes);

	for (i = 0; i < n; i++)
	{
		uint32		code = (uint32) codes[i];

		if (code >= nd)
			DECODE_CORRUPT("DICT code out of range");
		if (dlen[code] > rawLen - pos)	/* pos <= rawLen invariant */
			DECODE_CORRUPT("DICT output exceeds raw length");
		memcpy(raw + pos, dptr[code], dlen[code]);
		pos += dlen[code];
	}
	pfree(codes);
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
		case COLUMNAR_ENCODING_DICT:
			return "dict";
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
 *		Selection is adaptive per chunk: each applicable encoding is measured and
 *		the smallest pre-compression result wins (each encoder bails cheaply when
 *		it cannot help). The candidate set spans the encoding axes -- runs (rle),
 *		range (for), sequences (delta, dod), cardinality (dict), and floats
 *		(gorilla) -- so the choice adapts to the column's data (I5).
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

	/* fixed-width encodings (rle/for/delta/dod/gorilla); dict handled below */
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

	/*
	 * Dictionary (the cardinality axis of selection) applies to any fixed-width
	 * or varlena column, and is the only lightweight encoding available for
	 * varlena (text/bytea/jsonb) low-cardinality columns. It bails cheaply when
	 * the distinct count is too high to help.
	 */
	if (w > 0 || w == -1)
	{
		if (encode_dict(raw, rawLen, att, n, &buf, &len) && len < bestLen)
		{
			if (bestBuf)
				pfree(bestBuf);
			bestCode = COLUMNAR_ENCODING_DICT;
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

	if (valueCount > PG_UINT32_MAX)
		DECODE_CORRUPT("value count too large");

	/*
	 * Cross-check the catalog-supplied lengths against the encoder's invariants
	 * before dispatching, so a corrupt chunk cannot drive a decoder past its
	 * buffers. The fixed-width encodings always produce exactly n * attlen raw
	 * bytes; NONE stores the raw bytes verbatim. DICT validates internally
	 * (its raw length is the sum of variable-length dictionary entries).
	 */
	switch (encodingType)
	{
		case COLUMNAR_ENCODING_RLE:
		case COLUMNAR_ENCODING_FOR:
		case COLUMNAR_ENCODING_DELTA:
		case COLUMNAR_ENCODING_GORILLA:
		case COLUMNAR_ENCODING_DOD:
			if (att->attlen != 1 && att->attlen != 2 &&
				att->attlen != 4 && att->attlen != 8)
				DECODE_CORRUPT("fixed-width encoding on a non-fixed-width column");
			if ((uint64) n * (uint32) att->attlen != rawLen)
				DECODE_CORRUPT("raw length does not match value count");
			break;
		case COLUMNAR_ENCODING_NONE:
			if (encLen != rawLen)
				DECODE_CORRUPT("uncompressed length mismatch");
			break;
		default:
			break;
	}

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
		case COLUMNAR_ENCODING_DICT:
			return decode_dict(enc, encLen, att, n, rawLen, cx);
		default:
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("columnar: unknown value encoding type %d",
							encodingType)));
			return NULL;			/* keep the compiler happy */
	}
}
