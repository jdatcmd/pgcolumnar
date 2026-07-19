# Gap 28: index-only scans via a visibility map

Status: exploratory. Tier: capability. Format change: additive. Effort: large.

## Motivation

Index-only scans are never chosen for a columnar table because there is no
visibility map, so a covering query (e.g. `SELECT indexed_col ... WHERE
indexed_col ...`, or `count(*)` served by an index) still fetches from the
columnar storage, and each fetch decodes a whole chunk group -- the documented
point-lookup weakness. A visibility mechanism would let the planner choose
index-only scans and skip the storage fetch entirely for all-visible ranges.

## Current state

- `columnar_tableam.c`: the table AM reports no visibility map;
  `amgetbitmap`/index-only paths are effectively disabled, and
  `ColumnarReadRowByNumber` decodes a chunk group per fetch.
- Deletes/updates are tracked in `columnar.row_mask`, not a per-page VM.

## Design (sketch -- a subsystem)

The columnar main fork is not organized as heap pages with a parallel VM fork, so
a literal heap-style visibility map does not apply. Two viable directions:

1. All-visible chunk groups: maintain, per chunk group, an "all-visible" flag
   (no in-progress or recently-deleted rows relative to a horizon) derived from
   the row mask and the group's transaction metadata. Teach the table AM's
   index-only path to answer from the index tuple when the group is all-visible,
   consulting the row mask only for not-all-visible groups. This reuses existing
   row-mask machinery and adds a small per-group summary rather than a new fork.
2. `count(*)` / covering fast path: even without full index-only-scan support,
   the custom scan can already answer `count(*)` from chunk-group row counts minus
   row-mask deletes (partly done in the vectorized aggregate). Extend the planner
   so a covering aggregate/scan avoids value decode using only catalog counts and
   the row mask. This captures much of the practical benefit with far less
   machinery.

Direction (2) is the pragmatic first step (large-analytic `count`/covering
queries) ; direction (1) is the fuller index-only-scan capability.

## On-disk / API impact

Additive: an optional per-chunk-group all-visible summary (a catalog column on
`columnar.chunk_group` or a derived cache), and table-AM callback wiring for the
index-only path. 2.0/2.1 tables without the summary fall back to consulting the
row mask (correct, just not skipping the fetch).

## Testing

Differential/oracle: covering and `count(*)` queries with and without the fast
path, across visibility scenarios (fresh insert, concurrent readers, after
delete/update, after vacuum), versus heap -- results must be identical and, under
MVCC, respect each snapshot exactly (a not-all-visible group must not be answered
from the index). Concurrency and recovery coverage for the all-visible summary.

## Effort / risk

Large. Risk: MVCC correctness -- an index-only answer must never return a row not
visible to the snapshot, so the all-visible determination must be conservative
and snapshot-aware. Recommendation: ship direction (2) covering-count first;
treat direction (1) as a separate project with heavy MVCC differential testing.

## Source

PostgreSQL visibility map and index-only scan design; adapted to the columnar
row-mask model.
