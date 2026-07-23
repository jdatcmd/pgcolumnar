# Phase F3b plan: fully-online rewrite of partially-deleted groups

Status: active, on `phase-f/f3b-online-rewrite`. F3b reclaims space from row
groups that are only PARTIALLY deleted (F3a retires only fully-dead groups) by
rewriting their live rows into a fresh group, ONLINE under
ShareUpdateExclusiveLock: concurrent reads AND writes, like regular VACUUM. This
is the hardest piece of the mutation story; correctness rests on the protocol
below, verified against the MVCC model (see design/PHASE_F3_PLAN.md).

## Mechanism (per group, bounded batch per call)

For a group G whose deleted fraction exceeds a threshold:
1. Acquire the per-chunk-group advisory ExclusiveLock for (storageId, G) -- the
   SAME lock deleters take in ColumnarUpsertRowMask -- held to transaction end.
2. Take a fresh snapshot AFTER the lock, so it reflects every delete committed
   before the lock was granted.
3. Read G's live rows (skip row_mask-deleted rows under that snapshot),
   materializing their values.
4. Write them as a new group G' through the write state: reserve fresh row
   numbers, write column chunks to fresh append-only offsets, insert G's
   replacement `row_group` / `column_chunk` / `zone_map` / `bloom` catalog rows.
5. Insert index entries for each moved row's NEW row number into every index on
   the relation (online index maintenance; the offline path instead reindexes).
6. Delete G's catalog rows (row_group, column_chunk, zone_map, bloom, row_mask).
7. Commit -- releasing the advisory lock. Data pages of G are not reclaimed here
   (deferred; append-only offsets).

Because data pages are append-only and the swap is a set of catalog inserts plus
the old row_group delete in one transaction, heap MVCC makes it atomic: snapshots
older than the commit keep G (and read its pages); newer snapshots see G'. No
relfilenode swap, no AccessExclusiveLock.

## The concurrent-DELETE conflict protocol (the crux)

Hazard: a DELETE resolves a row's OLD number under its snapshot and buffers the
mark (flushed at pre-commit). If F3b moves that row to a NEW number in G' and
commits first, the delete would mark the now-gone G and the row would resurrect
for post-swap snapshots. The deleter's snapshot hides F3b's retirement, so the
deleter cannot detect the conflict from its own snapshot.

Resolution -- advisory-lock serialization + deleter abort-on-conflict:
- Both F3b and DELETE-flush take the per-chunk-group advisory lock, so they
  serialize per group.
- In ColumnarUpsertRowMask, AFTER acquiring the lock, check under the LATEST
  snapshot (not the deleter's stale snapshot) whether G's `row_group` catalog row
  still exists. If it is gone (F3b retired G), raise
  ERRCODE_T_R_SERIALIZATION_FAILURE ("row group compacted concurrently; retry").
  The deleter aborts and retries; on retry it re-resolves the row at G' and marks
  it correctly.

Case analysis (all correct):
- Deleter commits before F3b takes the lock: F3b's post-lock snapshot sees the
  delete, excludes the row from G'.
- Deleter holds the lock, F3b waits: F3b proceeds after the deleter commits,
  post-lock snapshot sees the delete, excludes the row.
- F3b commits before the deleter flushes: deleter takes the lock, checks latest,
  finds G gone, aborts and retries.
- Deleter buffered but not flushed while F3b runs: same as above -- caught at
  flush by the latest-state check.

Cost: a DELETE/UPDATE that races a concurrent rewrite of exactly its group gets a
serialization failure and retries. Rare, and standard for online reorg.

## Index correctness during the old/new overlap window

After commit both old-number (G) and new-number (G') index entries exist for the
moved rows; the btree returns both and table-fetch filtering picks the right one
per snapshot:
- New snapshot: old-number fetch hits retired G (its row_group tuple invisible)
  -> ColumnarReadRowByNumber returns not-live -> filtered; new-number fetch hits
  G' -> live -> returned. Each row returned once.
- Old snapshot: sees G, old-number fetch returns the row; new-number fetch hits
  G' whose row_group tuple (xmin = F3b) is invisible to the old snapshot ->
  not-live -> filtered. Each row returned once.
- Unique index: two entries for a key during the window, but the old-number one
  is not-live, so uniqueness holds (H4, via liveness filtering).

Old-number entries age out via btree vacuum / columnar_index_delete_tuples, which
already report deletability by actual liveness.

## Deferred / follow-on

- Physical page reclaim of rewritten groups (append-only offsets; eager vacuum
  reclaims the file).
- F3c incremental reclustering reuses this online-swap path with the F2 Morton
  key to re-sort out-of-order neighbors.

## Validation

- `test/native_compact.sh` (extend) or a new `native_rewrite.sh`: a
  partially-deleted group is rewritten (row group replaced, live rows preserved,
  deleted rows gone), heap-mirror parity, `pg_locks` shows
  ShareUpdateExclusiveLock and no AccessExclusiveLock, and index scans still
  return each live row exactly once after the rewrite.
- Concurrency stressor: run `pgcolumnar.compact_rewrite` on many connections
  against ongoing INSERT/UPDATE/DELETE and diff the surviving rows against a heap
  mirror; assert that DELETE conflicts surface as serialization failures (retried)
  and never as lost or resurrected rows.
- The differential oracle, `concurrent_diff`, and `unique_conc` stay green.
- PG17 gate during the work; full 15-19 matrix at the end.
