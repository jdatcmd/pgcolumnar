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
- The abort/self-heal path is covered by native_truncate.sh (truncate inside a
  rolled-back transaction, then a successful insert) and the no-overlap validator
  runs after truncation.

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

Order of operations inside the truncation transaction, under the lock:

1. Recompute `liveEnd`/`truncBlock` and the trailing-free guard (above) under the
   lock, so the state cannot change under us.
2. Delete the `free_space` rows with `file_offset >= liveEnd` (heap-WAL-logged).
3. Lower the metapage `reservedOffset` to `liveEnd`, using the existing
   exclusive-buffer-lock + `START_CRIT_SECTION` + `log_newpage_buffer` FPI pattern
   (same as `ColumnarReserveOffset`).
4. `CommandCounterIncrement()` so the catalog changes are self-visible.
5. Physically truncate: in a critical section, WAL-log an `xl_smgr_truncate` for
   `MAIN_FORKNUM` only (flag `SMGR_TRUNCATE_HEAP`, NOT `_VM`/`_FSM`), then
   `smgrtruncate(RelationGetSmgr(rel), forks, 1, &truncBlock)` after
   `DropRelationBuffers` for `[truncBlock, oldnblocks)`. This mirrors the internals
   of `RelationTruncate`, scoped to the main fork.
6. Invalidate the offset-keyed column cache.

Crash/abort safety. `smgrtruncate` is not rolled back on abort, so we make the
system tolerant of the resulting state (short file, stale-high `reservedOffset`,
possibly un-deleted `free_space` rows if the abort rolled back step 2) rather than
relying on atomicity we cannot get:

- Make `ColumnarWriteLogicalData` tolerate `blockno >= nblocks` with a GAP: extend
  the fork with freshly initialized empty pages from `nblocks` up to `blockno`,
  then write, instead of asserting `blockno == nblocks`. This is a small,
  standalone robustness change that makes "highwater past EOF" always safe: the
  next reservation simply re-extends across the gap. It is independently correct
  and should land first, on its own, with its own test.
- With gap-tolerant writes, every post-crash/abort state is self-healing: a stale
  `free_space` row in the truncated region, if later chosen for reuse, writes at
  its offset and re-extends the file; a stale-high `reservedOffset` reserves past
  EOF and re-extends. No reader is ever affected because readers only touch LIVE
  groups, none of which are in the truncated region (that was the truncation
  precondition).

Replication: the `xl_smgr_truncate` record replays on standbys through the
standard `smgr_redo`, truncating the same fork to the same block, so primary and
standby stay consistent. The metapage FPI and heap deletes replicate as usual.

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
