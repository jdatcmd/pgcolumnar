# pgColumnar roadmap

Status of the forward-looking work items and what remains. Every item preserves
the clean-room discipline in [../PROVENANCE.md](../PROVENANCE.md) and must land
with differential coverage against the heap oracle and pass the PostgreSQL 13-19
matrix. Gap specifications are in [gaps/](gaps/).

## Done

| Item | Where |
| --- | --- |
| Format 2.1 encodings (RLE, FOR, delta, delta-of-delta, Gorilla, dictionary) | I1-I8 |
| Vectorized scan, filter, and aggregate; late materialization | I3-I6 |
| Bloom filters, including collatable/text columns | gap 25 |
| Parallel scan | gap 23 |
| Covering `count(*)` from metadata | gap 28 (slice) |
| Sorted single-projection (`columnar.vacuum_sorted`) | gap 26 piece 1 |
| Arrow IPC export (`columnar.export_arrow`) | gap 27 |
| Parquet export (`columnar.export_parquet`) | gap 27 |

## Remaining

Ordered by value-to-effort.

1. Arrow/Parquet export type coverage. The writers currently cover int2/int4/
   int8, float4/float8, bool, text/varchar, and bytea. Add numeric, date/time/
   timestamp (with unit mapping), uuid, and arrays; document the mapping for
   types with no clean equivalent. Additive to the existing writers; verified
   with pyarrow and the DuckDB CLI. Spec: [gaps/27-arrow-parquet-interop.md](gaps/27-arrow-parquet-interop.md).

2. Full index-only scan (gap 28 direction 1). Maintain a per-chunk-group
   all-visible summary derived from the row mask and answer the table AM's
   index-only path from the index tuple for all-visible groups. Large; the risk
   is MVCC correctness (an index-only answer must never return a row not visible
   to the snapshot). Spec: [gaps/28-index-only-visibility-map.md](gaps/28-index-only-visibility-map.md).

3. Arrow/Parquet import. Read an Arrow IPC or Parquet file into a columnar table
   (`COPY`-style or a function). Secondary to export. Spec:
   [gaps/27-arrow-parquet-interop.md](gaps/27-arrow-parquet-interop.md).

4. Multiple projections (gap 26 piece 2). C-Store projections: N physical copies
   of a column subset, each in its own sort order, sharing the row-identity
   space. Requires a projections catalog, write fan-out, planner selection of the
   projection per query, row reconstruction for subset projections, and vacuum
   coordination. On-disk format change (2.2), additive for reads of 2.0/2.1.
   Largest item; a multi-PR project. Spec:
   [gaps/26-projections-pax.md](gaps/26-projections-pax.md).

## Test-harness follow-up

The suites are bash driving a differential oracle (heap mirror vs columnar,
compared by order-independent result hash) plus C property tests for the codecs.
One structural improvement is worthwhile when the harness is next touched: assert
EXPLAIN output from `FORMAT JSON` fields rather than text grep, and make a failed
step report which assertion failed instead of aborting under `set -e`. A wider
move to a Python/pyunit differential harness is possible later but is not
required; it should be specced before starting.
