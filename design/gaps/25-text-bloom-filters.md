# Gap 25: bloom filters for collatable/text columns

Status: planned. Tier: performance. Format change: none (reuses the 2.1 bloom column).

## Motivation

I7 built per-chunk bloom filters only for hashable, *non-collatable* types
(int/bigint/uuid/date/timestamp/bytea) to stay collation-safe. Text and varchar
keys -- a common filter target -- get no bloom skipping. Extending bloom to text
under deterministic collations covers that case without risking the
collation-mismatch bug the audit already fixed for min/max skipping.

## Current state

- `columnar_write_state.c`: builds a bloom only when
  `att->attcollation == InvalidOid` (non-collatable), hashing with `InvalidOid`.
- `columnar_reader.c`: `columnar_build_predicates` enables the bloom probe for an
  equality predicate only when `att->attcollation == InvalidOid`.
- `columnar_bloom.c`: build/probe are hash-agnostic; they take a precomputed
  32-bit hash.

## Design

Enable bloom for collatable types when the collation is *deterministic*, hashing
with that collation on both build and probe, and only probing when the query's
equality collation matches the column's:

1. Build: for a collatable column whose collation is deterministic
   (`get_collation_isdeterministic(att->attcollation)`, true for C/POSIX and most
   ICU collations), resolve the type's hash-with-collation proc
   (`TYPECACHE_HASH_PROC_FINFO`) and hash each value with `att->attcollation`.
   Nondeterministic collations are skipped (equal-but-not-byte-identical values
   would hash inconsistently) -- keep them unbloomed.
2. Probe: enable only when (a) the column collation is deterministic and (b) the
   scan key's collation equals the column collation. The scan key carries an
   input collation; compare it to `att->attcollation`. On mismatch, do not probe
   (exactly the guard the audit applied to min/max skipping). This makes a wrong
   skip impossible.
3. Reuse the existing `bloom_filter` catalog column and `columnar_bloom.c`
   verbatim; only the eligibility checks and the hash collation change.

## On-disk / API impact

None -- the 2.1 `bloom_filter` column already exists; this widens which columns
populate it. Older tables simply have no bloom for text (NULL), still correct.

## Testing

- Differential: low-cardinality and high-cardinality text/varchar columns under
  the default and `C` collations; equality for present and absent values on and
  off (`columnar.enable_bloom_filter`), versus the heap oracle; plus the
  audit-style case: `col = const COLLATE "different"` must not wrongly skip
  (results still correct because the probe is disabled on collation mismatch).
- An EXPLAIN-ANALYZE assertion (as in the I7 test) that bloom removes groups
  min/max cannot for a deterministic-collation text column.

## Effort / risk

Small-medium. Risk is collation correctness; the deterministic-only +
matching-collation-only guards, plus the audit-style differential case, contain
it (a bloom false negative would show as missing rows against the oracle).

## Source

Standard bloom-filter data skipping; the collation handling mirrors the existing
min/max collation guard (see test/audit.sh and PROVENANCE).
