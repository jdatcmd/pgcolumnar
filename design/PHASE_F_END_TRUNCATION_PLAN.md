# Phase F reclaim: physical end-truncation (design, for review)

Status: IMPLEMENTED (2026-07-23), PG17-gated, pending owner review before merge
(first truncation WAL in the extension, corruption-critical). `pgcolumnar.truncate(regclass)`
under the brief conditional AccessExclusiveLock, gated by the
`pgcolumnar.enable_end_truncation` GUC. Main-fork-only `xl_smgr_truncate`
(`ColumnarTruncateMainFork`), VM fork untouched. Built on PR A's gap-tolerant
writes for abort/crash self-heal.

Notes from implementation:
- The oldest-xmin horizon guard is doubly enforced: online `compact` will not even
  retire a group while an old snapshot could see it (its deletes are not yet before
  the horizon), so free space in the tail only exists once retirement was safe; and
  `truncate` re-checks the same horizon on the free ranges it would remove. The
  `truncate_vs_reader` isolation spec pins this: with the delete committed before a
  reader's snapshot, `compact` retires the group but `truncate` is a no-op
  (reclaimed = f) while the reader still resolves it and reads correctly, then
  reclaims (t) once the reader commits.
- Concurrency is pinned by three isolation specs: `truncate_vs_reader` (horizon
  guard), `truncate_vs_writer` (conditional AEL yields to an uncommitted writer,
  no wait), `truncate_vs_truncate` (two truncations serialize on
  ShareUpdateExclusiveLock; the second finds nothing left).
- Abort safety (revised after review): truncate refuses to run inside a
  transaction block, is ordered so the physical shrink precedes the (non-rolled-
  back) highwater lowering, and purges stale free ranges at the start of each run.
  native_truncate.sh asserts the transaction-block refusal, that a refused attempt
  leaves the table unchanged, and that a normal truncate + reinsert + compaction
  keeps the no-overlap validator green (no double-allocation). See "WAL, crash, and
  abort" below for the persistence-class reasoning and the documented residual.

This introduces the first partial-fork `smgrtruncate` and truncation WAL in the
extension and is corruption-critical. It builds directly on the free-list, reuse,
and split/coalesce work already merged. The rest of this document is the design.

## Goal

Return trailing freed blocks of a columnar relation's main fork to the operating
system, so a table that grew and then had large deletions can shrink on disk.
Today the file never shrinks: `reservedOffset` (the metapage highwater) only
advances, and reuse writes below it, so `pg_relation_size` stays at the historical
peak even after compaction reclaims the space logically.

## What makes this hard (from the machinery map)

1. `reservedOffset` is a logical highwater, not the physical EOF. Physical length
   is `smgrnblocks(MAIN_FORKNUM)`. Reuse writes below the highwater, so file
   length and highwater are decoupled. The truncation point must be derived from
   live data, not from `reservedOffset`.
2. Readers do not bounds-check a block number against EOF
   (`ColumnarReadLogicalData`). Any snapshot that references a row group whose
   bytes lie past a shrunken EOF will fault. So truncation is safe only if no
   snapshot can reference any group in the truncated region.
3. The relation file is shared by the base storage id and every projection
   storage id (multiple `row_group` sets, one file). The safe point must account
   for all of them.
4. The visibility-map fork is indexed by row-number-derived synthetic blocks,
   independent of main-fork data blocks. Truncation must scope strictly to
   `MAIN_FORKNUM` and leave the VM fork untouched.
5. `smgrtruncate` is not transactional. A truncate followed by transaction abort
   (or a crash mid-operation) leaves a short file with a stale-high
   `reservedOffset` and un-deleted `free_space` rows, which today would trip the
   extend assert in `ColumnarWriteLogicalData`.

## Safe truncation point

Under the truncation lock (below), with `oldestXmin = ColumnarOldestXmin(rel)`:

1. `liveEnd` = max over EVERY storage id in this relation (base + projections) of
   `row_group.file_offset + COLUMNAR_PAGE_ROUND_UP(row_group.byte_length)` for the
   live (latest-snapshot-visible) row groups. If there are no live groups,
   `liveEnd = COLUMNAR_FIRST_LOGICAL_OFFSET`. `liveEnd` is page-aligned because
   footprints are page-aligned.
2. `truncBlock = liveEnd / COLUMNAR_BYTES_PER_PAGE`, clamped to at least the 2
   reserved blocks.
3. Guard: every `free_space` row with `file_offset >= liveEnd` (the trailing
   region being removed) MUST have `freed_xid < oldestXmin`. If any is more
   recent, a snapshot older than that freeing could still see the retired group
   and read its bytes in the truncated region, so we do NOT truncate this pass
   (the space is reclaimed later once the horizon advances). This is the same
   safety gate the reuse allocator already applies, and it is required even
   though we hold the truncation lock, because it protects FUTURE scans that hold
   an old snapshot, not just concurrent ones.
4. If `truncBlock >= smgrnblocks(MAIN_FORKNUM)` there is nothing to do.

## Locking

Take a brief, CONDITIONAL `AccessExclusiveLock` for the physical step only,
exactly as PostgreSQL's own lazy-VACUUM truncation does:

- The bulk reclaim (retiring groups, building the free list) already runs lazily
  under `ShareUpdateExclusiveLock`. End-truncation is a separate, opt-in step.
- The physical truncate must drop buffers for the removed blocks and must exclude
  concurrent appends (`P_NEW` extension under the extension lock) and in-place
  reuse writes; `DropRelationBuffers` + `smgrtruncate` require that no other
  backend holds or dirties a buffer in the truncated range. `AccessExclusiveLock`
  is the standard, proven way to guarantee that, and it matches
  `RelationTruncate`'s contract.
- Acquire it with `ConditionalLockRelation` (or a short timeout). If the lock is
  not immediately available, skip truncation and return 0 -- best effort, never
  block a busy table. This keeps the operation friendly on live systems: it does
  the physical shrink only when it can do so without waiting, and yields
  otherwise. (Justification recorded per the minimize-lock-strength discipline: a
  weaker lock cannot make `DropRelationBuffers`/`smgrtruncate` safe against
  concurrent extension and buffer pins.)

The lock is released at transaction end as usual; the operation is short (compute
point, WAL, truncate, metapage update, catalog deletes).

## WAL, crash, and abort

The subtlety here is a **persistence-class mismatch**. Lowering the metapage
`reservedOffset` is a full-page-image WAL (`log_newpage_buffer`); like every
buffer-page mutation it is NOT undone on transaction abort, and the FPI replays
unconditionally in recovery. The `free_space` row deletes, by contrast, are
transactional heap deletes that DO roll back. If both happened and then the
transaction aborted, the result would be the **inverted** state: highwater lowered
(persisted) but the trailing free ranges restored, over a shortened file. From
there a plain INSERT reserves from the lowered highwater and a later compaction's
`ColumnarAllocateFreeSpace` best-fits a restored range covering the same bytes and
writes a second group over the first -- silent corruption. (Found in review; the
earlier version of this document wrongly claimed the highwater lowering rolls
back.)

Three things together make the operation safe:

1. **No transaction block.** `columnar_truncate` errors via `IsTransactionBlock()`
   if called inside `BEGIN ... COMMIT`, exactly as VACUUM's truncation does. A
   user `ROLLBACK` -- the easy way to reach the inverted state -- is therefore
   impossible, and the AccessExclusiveLock stays genuinely brief (released at the
   implicit commit, not held to a user transaction's end).
2. **Truncate before lowering the highwater.** Order under the lock: purge stale
   free ranges (below); compute `liveEnd`/`truncBlock` and the horizon guard;
   delete the trailing `free_space` rows; `CommandCounterIncrement`; physically
   truncate (`ColumnarTruncateMainFork`); THEN lower `reservedOffset`. A crash in
   the large window -- after the physical truncate, before the highwater is
   lowered -- leaves "highwater still high + free_space restored + file short",
   which the gap-tolerant write path (below) self-heals. Only the residual window
   between lowering the highwater and commit could leave the inverted state, and a
   crash there is caught by #3.
3. **Purge stale free ranges first.** At the start of every truncation, under the
   lock, delete any `free_space` row at or above the current `reservedOffset`. In
   a consistent state no such row exists (freed ranges are always below the
   highwater), so any that are present are the residual of a crash in the window
   of #2 and are dropped before they can be reused.

`ColumnarTruncateMainFork` mirrors `RelationTruncate`'s crash-safety envelope:
`RelationPreTruncate` for `wal_level=minimal` pending-sync, `MyProc->delayChkptFlags |= DELAY_CHKPT_COMPLETE`
across the record + physical op, a critical section, and `XLogFlush` of the
main-fork-only `xl_smgr_truncate` (flag `SMGR_TRUNCATE_HEAP`, NOT `_VM`/`_FSM`)
before `smgrtruncate` (`smgrtruncate2` on PG<=17, `smgrtruncate` on PG18+; it drops
the removed blocks' buffers itself). The offset-keyed column cache is invalidated
via `CacheInvalidateRelcacheByRelid`.

Gap-tolerant writes (PR #91) are the self-heal net: a stale `free_space` row in the
truncated region, if reused, writes at its offset and re-extends the file; a
high `reservedOffset` reserves past EOF and re-extends. No reader is ever affected
because readers only touch LIVE groups, none of which are in the truncated region.

Residual and the long-term fix. The window in #2 (highwater lowered, crash before
commit) is closed on the next truncation by #3, but a crash there followed by an
insert and a compaction, before any next truncation, could still reuse a buried
stale range. It is extremely narrow (a crash inside the sub-millisecond gap
between the metapage FPI and commit, on an opt-in operation), and the feature ships
`enable_end_truncation` OFF by default.

UPDATE (2026-07-23): this residual is now closed by reconciliation, on the MVCC
`free_space` catalog, rather than by moving the free list off the catalog.
`ColumnarReconcileFreeList(rel)` runs at the start of every reuse op
(`compact_rewrite`, `recluster`) and deletes any `free_space` row overlapping a
live row-group footprint, so a range left stale by a crash in the window above is
dropped before it can be reused. An earlier attempt (PR #94) moved the free list
into a non-transactional metapage page to make truncate's effects one persistence
class; it was closed because it made truncate atomic at the cost of the common
retirement path (which MVCC makes atomic for free), needing the same
reconciliation anyway, plus a fixed page capacity and a format change.
Reconciliation is the universal fix and keeps the MVCC catalog's retirement
atomicity, concurrency, and unbounded free list. Pinned by
`native_reclaim_reconcile.sh`.

Replication: the `xl_smgr_truncate` record replays on standbys through the
standard `smgr_redo`, truncating the same fork to the same block. The metapage FPI
and heap deletes replicate as usual.

## Toggle

Gate the whole operation behind an opt-in surface:
`SELECT pgcolumnar.truncate(regclass)` (a new maintenance function, sibling to
`compact`), plus a GUC `pgcolumnar.enable_end_truncation` (default off initially,
flipped to on once proven) so it can be disabled globally. Best-effort and
conditional, so it never blocks.

## Test plan (thorough, concurrency-focused)

Deterministic isolationtester specs (pg_isolation_regress, as with the existing
`isolation` suite):

1. truncate vs a reader holding an OLD snapshot: session A opens a repeatable-read
   snapshot before deletes; session B deletes + compacts + truncates; A must still
   read its rows correctly (the guard forbids truncating space A can see) and B's
   truncate either shrinks safely or is a no-op. Assert no error and correct rows.
2. truncate vs a concurrent appending writer: B truncates while A inserts; the
   conditional AEL makes truncate yield or serialize; assert no corruption, file
   valid, differential parity.
3. truncate vs concurrent compact/recluster on the same table: assert
   serialization (both take conflicting locks), parity preserved.
4. truncate vs truncate: two truncations serialize; second is a no-op.

Non-isolation suites:

5. A functional suite (`native_truncate.sh`): load, delete a trailing block of
   groups, compact, truncate; assert `pg_relation_size` strictly drops, parity vs
   a heap mirror holds, and the table is fully usable afterward (more inserts,
   reads, index scans). Include a projections case (shared file) and a case where
   the trailing region is NOT past the horizon (an open old snapshot) so truncate
   is correctly a no-op.
6. A crash/abort test: begin, truncate, ROLLBACK; then insert and read -- exercises
   the gap-tolerant write path and proves self-healing. If feasible, an immediate
   `pg_ctl stop -m immediate` restart-recovery variant to exercise WAL replay of
   the truncate.
7. The assert-only no-overlap validator runs after truncation too.

All gated on PG17 during iteration, then the full 15-19 matrix. The
gap-tolerant-write change lands first as its own matrix-gated PR.

## Sequencing / what needs sign-off

1. PR A: gap-tolerant `ColumnarWriteLogicalData` (independently correct; prerequisite).
2. PR B: `pgcolumnar.truncate()` + GUC + the isolation and functional tests.

Given this is the first truncation WAL in the extension and is corruption-critical,
the approach above (brief conditional AEL, MVCC horizon guard, main-fork-only
`xl_smgr_truncate`, gap-tolerant writes for abort/crash self-healing) should be
read before implementation.

## Follow-ups (tracked, not in this PR)

- **Ownership check across maintenance functions.** `truncate`, `compact`,
  `compact_rewrite`, `recluster`, and the `vacuum` family have no owner check, so
  any user can invoke them (truncate takes an AccessExclusiveLock, the strongest).
  Add an `object_ownercheck`/`pg_class_ownercheck` gate to all of them in one
  follow-up PR, with the PG15-vs-16 name compat in `columnar_compat.h`.
- **Abort atomicity: DONE via reconciliation** (not by moving the free list off
  the catalog). `ColumnarReconcileFreeList` drops free_space rows overlapping live
  groups at the start of every reuse, closing the residual window while keeping the
  MVCC catalog. See the UPDATE note in "WAL, crash, and abort" and PR #94's closure
  for why the metapage-page approach was rejected.
