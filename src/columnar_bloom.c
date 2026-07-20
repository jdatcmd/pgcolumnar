/*-------------------------------------------------------------------------
 *
 * columnar_bloom.c
 *		Per-chunk bloom filters for equality chunk-group skipping (I7).
 *
 * min/max skip lists (spec 7.2) prune equality predicates only when the probed
 * value falls outside a chunk's range. For an unsorted column an equality probe
 * usually falls inside every chunk's range, so nothing is skipped and a point
 * lookup decodes every chunk group -- the documented point-lookup weakness. A
 * bloom filter per chunk lets an equality probe skip a chunk group when the
 * value is provably absent, with a small false-positive rate.
 *
 * The filter is stored as [uint32 nbits][uint8 k][ceil(nbits/8) bytes], nbits a
 * power of two so a bit index is hash & (nbits-1). k positions per value are
 * derived from one 32-bit hash by double hashing. Build and probe hash values
 * the same way (the type's hash opclass proc), so a set built over a chunk's
 * values answers membership for a probe of the same type.
 *
 * Written from the standard bloom-filter construction and the public PostgreSQL
 * hashing API only (clean-room; see PROVENANCE.md).
 *
 *-------------------------------------------------------------------------
 */
#include "columnar.h"

#include "access/htup_details.h"
#include "catalog/pg_collation.h"
#include "utils/syscache.h"

/*
 * ColumnarCollationIsDeterministic
 *		Whether a bloom filter is safe for this collation: InvalidOid (a
 *		non-collatable type) and deterministic collations qualify; a
 *		nondeterministic collation does not, since equal values need not be
 *		byte-identical and would hash inconsistently.
 */
bool
ColumnarCollationIsDeterministic(Oid collid)
{
	HeapTuple	tp;
	bool		result = true;

	if (!OidIsValid(collid))
		return true;

	tp = SearchSysCache1(COLLOID, ObjectIdGetDatum(collid));
	if (HeapTupleIsValid(tp))
	{
		result = ((Form_pg_collation) GETSTRUCT(tp))->collisdeterministic;
		ReleaseSysCache(tp);
	}
	return result;
}

/* target ~1% false positives: ~10 bits/value, 6 probes */
#define BLOOM_BITS_PER_VALUE 10
#define BLOOM_K 6
#define BLOOM_MIN_BITS 64
#define BLOOM_MAX_BITS (1u << 21)	/* 256 KB cap per chunk */

static uint32
next_pow2(uint32 x)
{
	uint32		p = BLOOM_MIN_BITS;

	while (p < x && p < BLOOM_MAX_BITS)
		p <<= 1;
	return p;
}

/* two derived hashes for double hashing; h2 forced odd for full coverage */
static inline void
bloom_hashes(uint32 h, uint32 *h1, uint32 *h2)
{
	*h1 = h;
	*h2 = ((h >> 16) | (h << 16)) | 1u;
}

/*
 * ColumnarBloomBuild
 *		Build a filter over n precomputed value hashes. Returns false (no filter)
 *		when n is too small for a filter to be worthwhile.
 */
bool
ColumnarBloomBuild(const uint32 *hashes, uint32 n, char **out, uint32 *outLen)
{
	uint32		nbits;
	uint32		nbytes;
	uint32		total;
	char	   *buf;
	unsigned char *bits;
	uint8		k = BLOOM_K;
	uint32		i;

	if (n < 64)
		return false;			/* min/max and per-group scan suffice */

	nbits = next_pow2(n * BLOOM_BITS_PER_VALUE);
	nbytes = nbits / 8;
	total = sizeof(uint32) + sizeof(uint8) + nbytes;

	buf = palloc0(total);
	memcpy(buf, &nbits, sizeof(uint32));
	buf[sizeof(uint32)] = (char) k;
	bits = (unsigned char *) buf + sizeof(uint32) + sizeof(uint8);

	for (i = 0; i < n; i++)
	{
		uint32		h1;
		uint32		h2;
		int			j;

		bloom_hashes(hashes[i], &h1, &h2);
		for (j = 0; j < k; j++)
		{
			uint32		pos = (h1 + (uint32) j * h2) & (nbits - 1);

			bits[pos >> 3] |= (unsigned char) (1u << (pos & 7));
		}
	}

	*out = buf;
	*outLen = total;
	return true;
}

/*
 * ColumnarBloomProbe
 *		Return true when the hash may be present (all k bits set), false when it
 *		is definitely absent. A malformed/empty filter conservatively returns
 *		true (never skips wrongly).
 */
bool
ColumnarBloomProbe(const char *bloom, uint32 bloomLen, uint32 hash)
{
	uint32		nbits;
	uint8		k;
	const unsigned char *bits;
	uint32		h1;
	uint32		h2;
	int			j;

	if (bloom == NULL || bloomLen < sizeof(uint32) + sizeof(uint8))
		return true;

	memcpy(&nbits, bloom, sizeof(uint32));
	k = (uint8) bloom[sizeof(uint32)];
	bits = (const unsigned char *) bloom + sizeof(uint32) + sizeof(uint8);
	if (nbits == 0 || (nbits & (nbits - 1)) != 0)
		return true;			/* not a power of two: treat as no filter */

	/* the persisted length must actually hold the bitset; a corrupt header with
	 * a large nbits over a short buffer must not be indexed out of bounds */
	if ((uint64) bloomLen < sizeof(uint32) + sizeof(uint8) + ((uint64) nbits + 7) / 8)
		return true;			/* malformed: treat as no filter */

	bloom_hashes(hash, &h1, &h2);
	for (j = 0; j < k; j++)
	{
		uint32		pos = (h1 + (uint32) j * h2) & (nbits - 1);

		if (((bits[pos >> 3] >> (pos & 7)) & 1) == 0)
			return false;		/* a required bit is unset: definitely absent */
	}
	return true;
}
