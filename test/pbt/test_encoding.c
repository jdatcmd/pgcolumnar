/*
 * Property test for the value-stream codecs (src/columnar_encoding.c), run
 * standalone against the test/pbt PostgreSQL shim.
 *
 * The governing property is round-trip: for any raw value stream,
 * ColumnarDecodeChunk(ColumnarEncodeChunk(raw)) reproduces the exact bytes. It
 * is exercised over randomized data shaped to hit each encoding (constant,
 * alternating, monotonic, clustered, low-cardinality, runs, random) across all
 * fixed widths, floats (gorilla), and varlena (dict), plus explicit boundary
 * cases (empty, single value, integer extremes). Applies hegel's methodology
 * (round-trip, boundary values, no-crash); the hegel C client is not installed
 * here, so this is a self-contained randomized driver with a reproducible seed.
 *
 * Usage: test_encoding [seed] [iterations]
 */
#include "columnar.h"
#include "catalog/pg_type.h"

#include <math.h>

static unsigned long rng_state;
static void
rng_seed(unsigned long s)
{
	rng_state = s ? s : 0x9e3779b97f4a7c15UL;
}
static uint64
rng_next(void)
{
	/* xorshift64* */
	rng_state ^= rng_state >> 12;
	rng_state ^= rng_state << 25;
	rng_state ^= rng_state >> 27;
	return rng_state * 0x2545F4914F6CDD1DUL;
}
static uint64
rng_below(uint64 n)
{
	return n ? rng_next() % n : 0;
}

static int failures = 0;
static long checks = 0;

/* store the low w bytes of v (matches the codec's load/store_uint) */
static void
put_uint(char *p, uint64 v, int w)
{
	memcpy(p, &v, w);
}

/* one fixed-width round-trip check */
static void
check_fixed(int w, Oid typid, uint32 n, const char *raw)
{
	FormData_pg_attribute att;
	uint32		rawLen = n * (uint32) w;
	char	   *enc;
	uint32		encLen;
	int			code;
	char	   *dec;

	att.attlen = (int16) w;
	att.attbyval = true;
	att.atttypid = typid;

	code = ColumnarEncodeChunk(raw, rawLen, &att, n, NULL, 0, &enc, &encLen);
	dec = ColumnarDecodeChunk(enc, encLen, code, &att, n, rawLen, NULL, 0, NULL);
	checks++;

	if (rawLen > 0 && memcmp(dec, raw, rawLen) != 0)
	{
		failures++;
		fprintf(stderr,
				"FAIL fixed w=%d typid=%u n=%u code=%s(%d) encLen=%u\n",
				w, typid, n, ColumnarEncodingName(code), code, encLen);
	}
}

/* fill n values of width w following a pattern, then round-trip */
static void
gen_fixed(int w, Oid typid, uint32 n, int pattern)
{
	char	   *raw = (char *) malloc(n ? (size_t) n * w : 1);
	uint64		mask = (w == 8) ? ~0UL : (((uint64) 1 << (w * 8)) - 1);
	uint64		base = rng_next() & mask;
	uint64		lo = 1 + rng_below(6);	/* low-cardinality modulus */
	uint64		step = rng_below(8);
	uint32		i;
	uint64		cur = base;

	for (i = 0; i < n; i++)
	{
		uint64		v;

		switch (pattern)
		{
			case 0:				/* constant */
				v = base;
				break;
			case 1:				/* alternating two values */
				v = (i & 1) ? base : (base ^ 0x5a5a5a5aUL);
				break;
			case 2:				/* monotonic small random step */
				cur += rng_below(4);
				v = cur;
				break;
			case 3:				/* monotonic constant step (favors dod) */
				v = base + (uint64) i *step;
				break;
			case 4:				/* low cardinality (favors dict/rle/for) */
				v = base + (rng_next() % lo);
				break;
			case 5:				/* clustered (favors for) */
				v = base + rng_below(16);
				break;
			case 6:				/* runs of a repeated value */
				if (rng_below(8) == 0)
					cur = rng_next();
				v = cur;
				break;
			default:			/* fully random (favors none) */
				v = rng_next();
				break;
		}
		put_uint(raw + (size_t) i * w, v & mask, w);
	}
	check_fixed(w, typid, n, raw);
	free(raw);
}

/* float round-trip (gorilla eligible), width 4 or 8 */
static void
gen_float(int w, uint32 n, int pattern)
{
	char	   *raw = (char *) malloc(n ? (size_t) n * w : 1);
	uint32		i;
	double		cur = (double) (int64) rng_next() / 1000.0;

	for (i = 0; i < n; i++)
	{
		double		d;

		switch (pattern)
		{
			case 0:
				d = 3.14159;
				break;			/* constant */
			case 1:
				cur += ((double) (rng_below(200)) - 100.0) / 100.0;
				d = cur;
				break;			/* random walk */
			case 2:
				d = (double) i * 1.5;
				break;			/* linear */
			case 4:
				d = (double) (int64) (rng_next() % 2000000) / 100.0;
				break;			/* two-decimal values (favors ALP) */
			case 5:
				/* decimals with occasional special values (ALP exceptions) */
				switch (rng_below(8))
				{
					case 0:
						d = NAN;
						break;
					case 1:
						d = INFINITY;
						break;
					case 2:
						d = -INFINITY;
						break;
					case 3:
						d = -0.0;
						break;
					default:
						d = (double) (int64) (rng_next() % 1000000) / 1000.0;
						break;
				}
				break;
			default:
				d = (double) (int64) rng_next();
				break;			/* random */
		}
		if (w == 4)
		{
			float		f = (float) d;

			memcpy(raw + (size_t) i * 4, &f, 4);
		}
		else
			memcpy(raw + (size_t) i * 8, &d, 8);
	}
	check_fixed(w, (w == 4) ? FLOAT4OID : FLOAT8OID, n, raw);
	free(raw);
}

/*
 * varlena round-trip. shape 0: low cardinality (favors dict). shape 1: random
 * (favors none). shape 2: high-cardinality strings assembled from a small pool
 * of shared words (favors FSST -- distinct values, common substrings).
 */
static const char *const fsst_words[] = {
	"http://", "https://", "www.", ".com/", ".org", "user_", "_id=",
	"SELECT ", " FROM ", " WHERE ", "postgres", "columnar", "2026-07-",
	"error: ", "value", "/path/to/", "@example.com", "0123456789"
};

static void
gen_varlena(uint32 n, int shape)
{
	StringInfoData s;
	uint32		i;
	FormData_pg_attribute att;
	char	   *enc;
	uint32		encLen;
	int			code;
	char	   *dec;

	initStringInfo(&s);
	for (i = 0; i < n; i++)
	{
		char		body[64];
		int			blen = 0;
		uint32		total;
		int			j;

		if (shape == 2)
		{
			/* concatenate 1..4 shared words, then a per-row disambiguator */
			int			nw = 1 + (int) rng_below(4);
			int			k;

			int			nwords = (int) (sizeof(fsst_words) / sizeof(fsst_words[0]));

			for (k = 0; k < nw && blen < 48; k++)
			{
				const char *w = fsst_words[rng_below(nwords)];
				int			wl = (int) strlen(w);
				int			t;

				for (t = 0; t < wl && blen < 48; t++)
					body[blen++] = w[t];
			}
			/* a couple of varying bytes so most rows are distinct */
			body[blen++] = (char) ('0' + (i % 10));
			body[blen++] = (char) ('a' + (rng_next() % 26));
		}
		else
		{
			uint32		pick = (shape == 0) ? (uint32) rng_below(6)
				: (uint32) rng_next();

			blen = 1 + (int) (pick % 20);
			for (j = 0; j < blen; j++)
				body[j] = (char) ('a' + (pick + j) % 26);
		}

		total = 4 + (uint32) blen;
		appendBinaryStringInfo(&s, (char *) &total, 4);
		appendBinaryStringInfo(&s, body, blen);
	}

	att.attlen = -1;
	att.attbyval = false;
	att.atttypid = 25;			/* text */

	/*
	 * E3b: FSST now encodes against a chunk-shared table built once. Build it from
	 * this chunk's raw stream and pass it through; a NULL table (no useful symbols)
	 * simply means FSST is not a candidate, which the round trip still covers.
	 */
	{
		char	   *tbl = NULL;
		uint32		tblLen = 0;
		bool		haveTbl = ColumnarFsstBuildChunkTable(s.data, (uint32) s.len,
														  &att, &tbl, &tblLen);

		code = ColumnarEncodeChunk(s.data, (uint32) s.len, &att, n,
								   haveTbl ? tbl : NULL, haveTbl ? tblLen : 0,
								   &enc, &encLen);
		dec = ColumnarDecodeChunk(enc, encLen, code, &att, n, (uint32) s.len,
								  haveTbl ? tbl : NULL, haveTbl ? tblLen : 0, NULL);
		checks++;
		if (s.len > 0 && memcmp(dec, s.data, s.len) != 0)
		{
			failures++;
			fprintf(stderr, "FAIL varlena n=%u code=%s rawLen=%d\n",
					n, ColumnarEncodingName(code), s.len);
		}
		if (haveTbl)
			free(tbl);
	}
	free(s.data);
}

/* explicit boundary cases that randomized runs might under-sample */
static void
boundary_cases(void)
{
	int			widths[] = {1, 2, 4, 8};
	Oid			typ[] = {18, 21, 23, 20};
	int			wi;

	for (wi = 0; wi < 4; wi++)
	{
		int			w = widths[wi];
		uint64		mask = (w == 8) ? ~0UL : (((uint64) 1 << (w * 8)) - 1);
		uint32		n;

		/* empty, single, and a couple of tiny counts */
		for (n = 0; n <= 3; n++)
		{
			char	   *raw = (char *) malloc(n ? (size_t) n * w : 1);
			uint32		i;

			for (i = 0; i < n; i++)
				put_uint(raw + (size_t) i * w, (uint64) i & mask, w);
			check_fixed(w, typ[wi], n, raw);
			free(raw);
		}

		/* integer extremes: all-min, all-max, alternating min/max, 0/-1 mix */
		{
			uint32		cnt = 300;
			char	   *raw = (char *) malloc((size_t) cnt * w);
			uint64		ex[] = {0, mask, mask >> 1, (mask >> 1) + 1};
			int			e;

			for (e = 0; e < 4; e++)
			{
				uint32		i;

				for (i = 0; i < cnt; i++)
					put_uint(raw + (size_t) i * w, ex[e] & mask, w);
				check_fixed(w, typ[wi], cnt, raw);
			}
			/* alternating extremes stresses delta/dod zigzag on huge deltas */
			{
				uint32		i;

				for (i = 0; i < cnt; i++)
					put_uint(raw + (size_t) i * w, (i & 1 ? mask : 0), w);
				check_fixed(w, typ[wi], cnt, raw);
			}
			free(raw);
		}
	}
}

int
main(int argc, char **argv)
{
	unsigned long seed = (argc > 1) ? strtoul(argv[1], NULL, 0) : 1;
	long		iters = (argc > 2) ? strtol(argv[2], NULL, 0) : 200000;
	long		it;

	rng_seed(seed);
	printf("codec property test: seed=%lu iters=%ld\n", seed, iters);

	boundary_cases();

	for (it = 0; it < iters; it++)
	{
		int			kind = (int) rng_below(3);
		uint32		n;

		/* mostly small, sometimes large; include 0/1/2 boundaries */
		if (rng_below(10) == 0)
			n = (uint32) rng_below(3);
		else
			n = (uint32) rng_below(3000);

		if (kind == 0)
		{
			int			ws[] = {1, 2, 4, 8};
			Oid			ts[] = {18, 21, 23, 20};
			int			wi = (int) rng_below(4);

			gen_fixed(ws[wi], ts[wi], n, (int) rng_below(8));
		}
		else if (kind == 1)
			gen_float((rng_below(2) ? 8 : 4), n, (int) rng_below(6));
		else
			gen_varlena(n, (int) rng_below(3));
	}

	printf("checks=%ld failures=%d\n", checks, failures);
	if (failures == 0)
		printf("CODEC PBT PASSED\n");
	else
		printf("CODEC PBT FAILED\n");
	return failures == 0 ? 0 : 1;
}
