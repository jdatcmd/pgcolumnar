# Phase F: physical page reclaim (free-list with MVCC-gated reuse)

Status: active, on `phase-f/physical-reclaim`. Finishes the lazy maintenance
story: F3a/F3b/F3c and delete-vacuum retire a group's CATALOG rows but leave its
data blocks in the relation file, because offsets come from a monotonic highwater
(`reservedOffset`) that never decreases. Across compaction cycles the file bloats
even as logical data stays flat. This adds a free-list so freed offset ranges are
reused, making online compaction space-neutral and bounding file growth.

## Storage model (survey)

- A stripe reserves `[alignedOffset, alignedOffset+dataLength)` from the metapage
  `reservedOffset` highwater (`ColumnarReserveOffset`), page-aligned, and writes
  it with `ColumnarWriteLogicalData`, which today ALWAYS extends via `P_NEW` and
  asserts the extended block equals the expected block -- so it can only write at
  the highwater, never in place.
- Retiring a group deletes its `row_group`/`column_chunk`/`zone_map`/`bloom`/
  `row_mask` rows; the `row_group` row carried `file_offset` and `byte_length`.
- Visibility is heap MVCC on the catalog; a snapshot older than a group's
  retirement still sees the group and reads its `file_offset` blocks.

## The MVCC hazard (why reuse must be gated)

Reusing a freed block while ANY snapshot can still read the old group there is
silent corruption. A group retired at xid R is still read by snapshots older than
R. So a freed range is safe to reuse only once `oldestXmin > R` -- then no
snapshot sees the pre-retirement state, so none reads the old blocks. Reuse is
gated on that horizon exactly like the all-visible and F3a computations.

## Design

- New catalog `pgcolumnar.free_space(storage_id int8, file_offset int8,
  byte_length int8, freed_xid int8)`. One row per freed offset range.
- On retirement, capture the group's `file_offset`/`byte_length` before deleting
  its `row_group` row and insert a `free_space` row with the current xid as
  `freed_xid`. Same transaction, so an aborted compaction rolls back the free
  record too (catalog MVCC).
- `ColumnarReserveOffset`: before extending the highwater, look for a `free_space`
  range with `byte_length >= dataLength` AND `freed_xid` preceding the relation's
  `oldestXmin` (safe to reuse). If found, allocate from it (delete the row, or
  shrink it and re-insert the remainder, keeping page alignment), and return that
  offset. Otherwise extend as today. This makes rewrites reuse freed space.
- `ColumnarWriteLogicalData`: when the target block already exists
  (`blockno < RelationGetNumberOfBlocks`), read and overwrite it in place instead
  of `P_NEW`-extending; only extend for blocks past the current end. This is what
  lets a reused (below-highwater) offset be written.
- End truncation (opportunistic): when the free tail reaches `reservedOffset`
  (the highest ranges are all free and reusable), lower `reservedOffset` and
  `smgrtruncate` the file, returning disk to the OS. Runs in the vacuum/compact
  path under the lock the caller holds.

## Correctness

- Reuse only of ranges with `freed_xid < oldestXmin`: no live snapshot reads the
  old blocks, so overwriting them cannot corrupt any reader.
- Page alignment preserved: reservations stay page-aligned, so reused ranges are
  whole-page and never straddle a live stripe.
- Aborted compaction rolls back both the retirement and the free record.
- The differential oracle stays byte-exact through delete+compact+reuse cycles.

## Validation

- `native_reclaim.sh`: repeated delete + compact_rewrite / recluster cycles keep
  the relation file size bounded (reused, not grown), while results match a heap
  mirror; a fresh insert after reclaim reuses freed offsets (file does not grow).
- MVCC-gated reuse (isolation spec `reclaim_gated`): an old REPEATABLE READ
  snapshot pins `oldestXmin`, so a retired group's space is NOT reused while it is
  open (the old reader still reads correct data); after it commits and the horizon
  advances, the space becomes reusable.
- The differential oracle, concurrency suites, and existing isolation specs stay
  green. PG17 gate during the work; full 15-19 matrix at the end.
