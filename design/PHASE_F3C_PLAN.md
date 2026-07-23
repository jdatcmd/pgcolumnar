# Phase F3c plan: online reclustering (lazy path)

Status: active, on `phase-f/f3c-online-recluster`. F3c is the online, lazy
counterpart to F2's eager `cluster()`: it re-establishes Z-order clustering over
a table's live rows WITHOUT AccessExclusiveLock. It reuses F2's Morton-key code
and F3b's online-swap machinery (catalog-MVCC swap, online index maintenance, and
the concurrent-DELETE conflict protocol).

## Mechanism

`pgcolumnar.recluster(table, VARIADIC columns)`:
1. Capture the current row groups' numbers and take the per-chunk-group advisory
   lock on each (ascending group-number order, deadlock-safe) -- the same lock
   deleters take, so a delete to any of these groups serializes with the recluster
   and, if it loses, aborts with a serialization failure and retries (the F3b
   conflict protocol, unchanged).
2. Under a snapshot taken after the locks, read every live row (ColumnarBeginRead
   skips deleted rows), compute its Z-order Morton key, and sort by it (an
   augmented bytea key column carried through tuplesort, as in eager cluster).
3. Write the globally Morton-sorted rows back as fresh groups via the write state,
   inserting index entries for their new row numbers (online, no reindex).
4. Retire all the captured old groups (delete their catalog rows) in the same
   transaction, so the swap is atomic under heap MVCC: old snapshots keep the old
   groups (append-only data pages untouched), new snapshots see the reclustered
   groups. No relfilenode swap, no AccessExclusiveLock.

The result is globally Z-order-clustered, like eager `cluster()`, but online:
concurrent reads never block, and concurrent writes to the reclustered groups
retry rather than block reads.

## Scope and limits (v1)

- v1 reclusters the whole table's live rows in one transaction, so it holds one
  advisory lock per group for the duration. That bounds concurrent DELETE/UPDATE
  latency to the recluster's runtime and consumes one lock slot per group, so v1
  refuses tables above a group-count cap and directs them to eager `cluster()`.
  Streaming, per-batch-committed reclustering (bounded locks, bounded delete-block
  window) that still converges to global order is the v2 refinement -- it needs
  per-batch commits (a procedure or repeated calls) and a merge strategy across
  batches, and is deferred.
- Physical page reclaim of retired groups is deferred (append-only offsets), as in
  F3a/F3b.

## Validation

- `native_recluster.sh`: online recluster tightens multi-column zone maps (a 2D
  box skips far more groups after recluster) exactly as eager cluster does,
  results match a heap mirror, and `pg_locks` shows ShareUpdateExclusiveLock and
  no AccessExclusiveLock.
- The delete-vs-recluster race is the same code path as delete-vs-rewrite, already
  pinned deterministically by the `delete_vs_rewrite` isolation spec and stressed
  by `native_rewrite_conc.sh`; a recluster variant can be added if it diverges.
- The differential oracle and concurrency suites stay green.
- PG17 gate during the work; full 15-19 matrix at the end.
