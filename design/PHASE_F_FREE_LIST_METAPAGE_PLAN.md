# Phase F reclaim: move the free list into the metapage (design, for review)

Status: IMPLEMENTED (2026-07-23), PG17-gated, submitted as a PR for review. This
replaces the `pgcolumnar.free_space` catalog table with a non-transactional free
list stored in the relation's own pages, so the free list and the metapage
highwater share one persistence class. It is corruption-critical (it changes the
on-disk format and the crash/abort behavior of reclaim).

Implementation notes (small deviations from the design below):
- The entry count is the block-1 page's `pd_lower`, not a `freeCount` field in the
  metapage, so block 1 is fully self-describing and each update is a single
  atomic full-page-image write (`ColumnarFreeListRead` / `ColumnarFreeListWrite`
  in `columnar_storage.c`). No metapage struct change.
- The former queryable `free_space` catalog is replaced by an introspection
  function `pgcolumnar.free_list(regclass)` returning the block-1 entries.
- `TRUNCATE TABLE` clears block 1 (it is kept by the truncate-to-two-blocks).
- Truncate's abort window is now closed inherently (all three effects are
  non-transactional page/file ops in one class); its transaction-block prohibition
  and ordering are retained as harmless belt-and-suspenders, relaxable in a
  follow-up.
- The overflow-leak fallback is exercised by the opt-in `native_free_overflow.sh`:
  260 non-adjacent retirements fill the page to exactly its 255-entry capacity,
  data stays correct, and the no-overlap validator stays green.

## Motivation

Physical end-truncation (PR #92) has a documented residual abort/crash window: it
lowers the metapage highwater (a full-page-image write that is NOT rolled back on
abort) while deleting `free_space` rows (transactional heap deletes that ARE
rolled back). The two effects are different persistence classes, so a crash
between lowering the highwater and commit can leave a lowered highwater with the
trailing free ranges restored (the "inverted" state), which a later insert plus
compaction could double-allocate. Truncate currently narrows this window
(transaction-block prohibition, reorder, start-of-run purge) but does not close
it.

Moving the free list into the metapage's non-transactional pages makes the free
list and the highwater the same persistence class, so truncate's two effects
persist or roll back together. That eliminates the window rather than narrowing
it. Secondary wins: it removes a catalog table and the per-allocation
`systable_beginscan` over it, and it simplifies the reclaim reasoning to a single
durability class.

## What the free list needs

An entry is `(storage_id, file_offset, byte_length, freed_xid)` = 32 bytes. It is:
- Recorded on group retirement (`record_free_space`), with same-transaction
  coalescing.
- Consumed on compaction reuse (`ColumnarAllocateFreeSpace`), best-fit, split on
  oversize, gated on `freed_xid < oldestXmin`.
- Scanned by the trailing-free horizon guard and the assert-only no-overlap
  validator, and purged at/above an offset by truncate.

Crucially, EVERY mutation happens under ShareUpdateExclusiveLock: retirement runs
inside compact / compact_rewrite / recluster (all SUEL), and reuse is gated on the
writer holding SUEL (`columnar_write_state.c`). SUEL self-conflicts, so free-list
mutations are globally serialized per relation. That is what lets a page-resident
list use plain buffer locks instead of MVCC. (Implementation must assert this
invariant: `record_free_space` / `ColumnarAllocateFreeSpace` are never reached
without SUEL held.)

## Storage layout

Block 1 of the main fork (`COLUMNAR_EMPTY_BLOCKNO`) is already reserved and unused;
data starts at block 2 (`COLUMNAR_FIRST_LOGICAL_OFFSET`). Use block 1 as the
free-list page: a page header plus a packed array of 32-byte entries, about 253
entries in the 8 KB page. The metapage (block 0) gains a `freeCount` field (number
of live entries in block 1). Both blocks are written under an exclusive buffer
lock with `log_newpage_buffer` (the exact pattern `ColumnarReserveOffset` and
`ColumnarSetReservedOffset` already use), so they are crash-safe and replicate
identically.

Capacity and overflow. Same-transaction coalescing (already implemented) keeps the
list compact by merging adjacent frees, so a healthy table stays far below 253
entries. If the list is full when recording a new range, the range is simply not
recorded: the space stays reclaimable-in-principle but is not tracked until a full
rewrite (`compact_rewrite` / `recluster`) rebuilds the groups. This is graceful
degradation (unreclaimed space, never corruption), and it is logged so it is
visible. This is the one real trade-off against today's unbounded catalog list;
see Alternatives.

Per relation, not per storage. Projections share the file and the metapage, so the
single block-1 list holds entries for the base and all projection storage ids,
keyed by the `storage_id` field, exactly as the catalog did.

## Truncate becomes fully non-transactional

With the free list in block 1, truncate's effects are: purge block-1 entries at or
above `liveEnd`, lower the metapage highwater, and `smgrtruncate`. All three are
non-transactional page/file operations. A crash or abort therefore leaves the same
consistent state as a commit (highwater lowered, trailing entries gone, file
short) rather than the inverted state. The residual window is closed. The
transaction-block prohibition can then be relaxed (truncate no longer has any
transactional effect to roll back inconsistently), though keeping it for
least-surprise is a reasonable call to make at implementation time.

## Concurrency and crash safety

- All mutations under SUEL (asserted), so the block-1 buffer lock plus the
  metapage buffer lock are sufficient; no MVCC, no advisory locks for the list
  itself. The existing per-chunk-group advisory lock for delete-vector upserts is
  unaffected (that guards delete vectors, not the free list).
- Reads (horizon guard, validator) take a share buffer lock on block 1.
- Every mutation is a full-page-image WAL record, so recovery and standbys are
  consistent, matching the current metapage writes.
- `freed_xid < oldestXmin` gating is unchanged; the xid is stored per entry.

## Touchpoints

Almost all of the change is in `columnar_metadata.c`, plus small edits elsewhere:
- Rewrite `insert_free_space_row`, `record_free_space` (with coalescing),
  `ColumnarAllocateFreeSpace` (best-fit + split), `ColumnarTrailingFreeSpaceSafe`,
  `ColumnarDeleteFreeSpaceAtOrAbove`, and `ColumnarCheckFreeSpaceNoOverlap` to
  operate on the block-1 array under buffer locks instead of the catalog.
- `columnar.h`: add `freeCount` to `ColumnarMetapage`; a block-1 entry struct and
  accessors; bump `COLUMNAR_NATIVE_VERSION_*` (on-disk format change).
- `columnar_storage.c`: initialize block 1 as an empty free-list page in
  `ColumnarWriteNewMetapage`; helpers to read/modify the block-1 list.
- `pgcolumnar--1.0.sql`: drop the `free_space` table, its indexes. `ColumnarDeleteMetadata`
  no longer deletes `free_space` rows (block 1 goes with the relation file).
- `columnar_metadata.c` Anum/Natts for `free_space`: removed.

No data migration path: this is a dev-version on-disk format change, so existing
columnar tables must be recreated (note the format-version bump in the commit).

## Test plan

- All existing reclaim suites must stay green unchanged: `native_reclaim`,
  `native_reclaim_cycles`, `native_reclaim_frag`, `native_gap`, `native_truncate`,
  and the `truncate_vs_*` isolation specs (the observable behavior is identical).
- New `native_free_overflow.sh`: drive the free list past its capacity with highly
  fragmented, non-coalescible retirements and assert graceful degradation (parity
  vs heap holds, the no-overlap validator stays green, the file is not corrupted;
  some space is left unreclaimed until a full rewrite, then reclaimed).
- Abort test: with the transaction-block prohibition relaxed (if we relax it),
  `BEGIN; truncate; ROLLBACK` followed by insert + compaction must keep the
  validator green and parity intact, proving the window is closed. If we keep the
  prohibition, assert it as today.
- Gate on PG17 during iteration, full 15-19 matrix before merge.

## Alternatives considered

- Overflow to a chain of reserved blocks. Reserve several pages (blocks 1..N)
  before data, or chain overflow pages, for a larger or unbounded list. Removes
  the cap but adds page-chain management and wastes pages on unfragmented tables.
  Deferred; the bounded single page plus coalescing is expected to suffice, and
  the overflow-leak fallback is safe. Revisit if the overflow test shows the cap
  bites real workloads.
- A custom relation fork. PostgreSQL's fork numbers are a fixed enum
  (MAIN/FSM/VM/INIT); an extension cannot cleanly add one, and FSM tracks
  block-granular free space, not byte-range extents with an xid. Not viable.
- Leave the catalog, fix truncate differently. No transactional mechanism can make
  a metapage FPI and a heap delete atomic across a crash; that is the whole reason
  for this change.

## Risk and why it is review-gated

It changes the on-disk format and the crash/abort semantics of the reclaim path,
which is corruption-critical. The design is believed correct (single persistence
class, all mutations under SUEL, FPI-logged, horizon gating unchanged), but the
SUEL-serialization invariant and the overflow-leak fallback should be read before
implementation. The change lands as a single PR (this plan plus the code) for
review, not auto-merged.
