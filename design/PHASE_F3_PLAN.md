# Phase F3 plan: online incremental compaction and reclustering (lazy path)

Status: active, on `phase-f/delete-vectors` (after F2). F3 is the REQUIRED lazy
counterpart to F2's eager reorg: incremental, per-row-group, MVCC-safe compaction
and reclustering that holds only ShareUpdateExclusiveLock (concurrent reads AND
writes), never a table-wide AccessExclusiveLock. Every maintenance op must offer
this non-blocking path (see the lazy-option requirement). Written before code,
from a full survey of the MVCC / visibility model.

## The MVCC model F3 builds on (survey result)

pgColumnar has no columnar-specific visibility machinery. Visibility is ORDINARY
heap MVCC applied to the metadata catalog tuples; data pages are append-only,
unversioned raw bytes at monotonic file offsets. Concretely, a snapshot S sees
row r iff:
1. the `row_group` catalog tuple covering r is MVCC-visible to S, AND
2. no `row_mask` tuple visible to S has r's bit set, AND
3. r's validity bit is set (NULL handling, orthogonal to MVCC).

Consequences that make an online swap possible WITHOUT AccessExclusiveLock:
- Row numbers come from a monotone metapage counter; compaction can reserve a
  fresh disjoint range for rewritten rows, coexisting with concurrent inserters.
- Data offsets are append-only and never reused, so writing a rewritten group to
  fresh offsets does not disturb old-snapshot readers still reading old offsets.
- The reader loads the row-group list once at scan begin under the scan snapshot,
  then reads raw bytes; old snapshots keep reading the old group.
- Therefore a group can be replaced by: in ONE transaction, insert the new
  `row_group` / `column_chunk` / `zone_map` / `bloom` rows AND delete the old
  `row_group` tuple. Heap MVCC makes the swap atomic exactly as an INSERT becomes
  visible: old snapshots keep the old group, new snapshots see the new one.

## Hazards (from the survey) and how F3 handles them

- H1 concurrent DELETE racing a rewrite. A delete resolves a row's OLD number
  under its snapshot and buffers it; if compaction rewrites that row to a NEW
  number and commits, the delete lands on the now-gone old group and the row is
  resurrected for post-swap snapshots. ShareUpdateExclusiveLock does not block
  deletes. This is the crux and is why F3 is decomposed with the safe case first.
- H2 concurrent INSERT: no hazard; disjoint row-number ranges.
- H3 old pages pinned by old snapshots: reclamation of an old group's offsets must
  be DEFERRED until oldestXmin passes the swap, mirroring
  ColumnarComputeAllVisibleGroups. Append-only offsets make deferral trivial.
- H4 index bloat: after a rewrite both old- and new-number index entries exist;
  the old ones are filtered as not-live (their group tuple is gone) via
  ColumnarReadRowByNumber / columnar_index_delete_tuples.
- H5 curcid self-visibility: the rewrite must read the source under the pre-swap
  snapshot, not a self-including catalog snapshot.

## Decomposition (each PG17-gated PR)

### F3a. Online retirement of fully-dead row groups [start here]

The safe, race-free first slice. Retire (drop the catalog rows for, and defer
page reclaim of) any row group ALL of whose rows are deleted as-of `oldestXmin` --
i.e. every live snapshot agrees the group is dead. No rewrite, no row-number
remap, no index change (the rows are already dead to everyone), so H1/H4 do not
arise, and gating on oldestXmin closes H3. This directly retires delete vectors
and reclaims space for the common delete-heavy pattern (drop old partitions,
bulk deletes), all under ShareUpdateExclusiveLock, concurrent with reads/writes.
Invocable as `pgcolumnar.compact(table)` and hookable from
`columnar_relation_vacuum` (autovacuum), which today only sets all-visible bits.

Design: extend the oldestXmin all-visible computation to also report groups whose
visible row_mask marks every row deleted with all deleting xids < oldestXmin.
For each such group, in one transaction delete its `row_group` + `column_chunk` +
`zone_map` + `bloom` + `row_mask` catalog rows; record the freed offset range for
deferred physical reclaim. Never touch a group any snapshot still sees as live.

### F3b. Online rewrite of partially-deleted groups

Rewrite a group past a deleted-fraction threshold: read its live rows under a
captured snapshot, write them to fresh offsets as a new group with fresh row
numbers, insert the new catalog rows and delete the old `row_group` tuple in one
transaction, and insert index entries for the new row numbers. Handle H1 by
serializing with deleters on the per-chunk-group advisory lock AND detecting
conflict at commit: hold the group's advisory lock across capture..commit so no
delete commits to the group meanwhile; a delete that resolved an old row number
before the lock and blocks on it must, on unblock, find the old group gone and
re-resolve (or its transaction conflicts and retries). The exact conflict
protocol (redirect the buffered delete to the new number vs. abort-and-retry the
losing transaction) is the key design decision and is settled in F3b, not F3a.

### F3c. Incremental reclustering

Maintain F2's Z-order online: pick groups whose keys interleave (out-of-order
neighbors), merge-and-resplit them in Z-order via the F3b rewrite path, bounded
per invocation. Reuses F2's Morton-key code and F3b's online-swap machinery.

## Validation

- The differential oracle and the concurrency suites (`concurrent_diff`,
  `unique_conc`) stay green while compaction runs concurrently with DML -- add a
  concurrent-compaction stressor that runs `pgcolumnar.compact` on many
  connections against ongoing insert/update/delete and diffs against the heap.
- F3a: assert a fully-deleted group is retired (row group count drops, space
  reclaimed) only after its deleting txns pass oldestXmin, and that a concurrent
  old snapshot still sees the rows until then.
- Lock assertion: compaction runs under ShareUpdateExclusiveLock and does NOT
  block a concurrent reader or writer (verify via a second session).
- PG17 gate during the work; full 15-19 matrix at the end.
