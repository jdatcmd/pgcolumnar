# Phase F reclaim: free-space splitting and coalescing (design, for review)

Status: IMPLEMENTED (2026-07-23), behind the `pgcolumnar.reclaim_coalesce` GUC
(default on; off reverts to whole-range reuse). Two refinements were made during
implementation and are worth recording:

- **Store the page-rounded footprint, not the exact byte length.** `record_free_space`
  now rounds a freed range up to a whole page (`COLUMNAR_PAGE_ROUND_UP`), so free
  ranges tile the file page-aligned in both offset and length. This is what makes
  a split remnant and a coalesced union stay page-aligned, and it also reclaims a
  group's trailing padding.
- **Coalesce only ranges freed by the SAME transaction.** Merging a fresh free
  with an older, already-reusable neighbor forces the union to the newer freed_xid
  (required for safety), which delays reuse of space that was reusable a moment
  ago -- a measured regression (the file grew every cycle in native_reclaim_cycles
  until this was restricted). Same-transaction coalescing still merges the many
  groups one maintenance operation retires together, which is the fragmentation
  case that matters, without poisoning older reusable ranges.

Result on the fragmentation test (native_reclaim_frag): retiring 18 adjacent
groups yields 1 coalesced free range with the GUC on vs 18 fragmented rows off,
and a large reclustering then reuses in place (smaller final file) instead of
extending. The assert-only no-overlap validator runs at the end of every online
maintenance op. The rest of this document is the original design.

## Current behavior

Physical reclaim keeps a `pgcolumnar.free_space` free list of
`(storage_id, file_offset, byte_length, freed_xid)` rows. A retired row group
records its whole byte range as one free_space row. An allocation
(`ColumnarAllocateFreeSpace`) takes the smallest single row whose `byte_length`
is at least the request and whose `freed_xid` precedes the oldest-xmin horizon,
consumes the entire row, and returns its offset. Allocation happens only under
ShareUpdateExclusiveLock (online compaction), which is self-serialized.

This works and reaches a steady-state file size when freed ranges are close in
size to later requests. The measured plateau in `test/native_scale.sh` (uniform
group sizes) confirms it.

Two gaps remain, both about fragmentation:

1. No splitting. A request of 60 KB served from a 100 KB free row consumes the
   whole 100 KB row. The writer only writes 60 KB at that offset; the remaining
   40 KB is never recorded again and is leaked until the group is retired and
   re-freed as a whole.
2. No coalescing. Two adjacent freed ranges stay two rows. A request larger than
   either but no larger than their sum cannot be satisfied, so the file extends
   even though contiguous free space exists.

Under a workload with variable group sizes (mixed `stripe_row_limit`, wide vs
narrow rows, encoding-dependent chunk sizes) this fragments the free list and the
file grows past the ideal working set.

## Proposed change

Allocation unit is the block (`BLCKSZ`). Every free range and every reservation
is already block-aligned via `ColumnarReserveOffset`; keep that invariant so
offsets and remnants stay aligned. Reason in whole blocks.

### Split on allocate

When the chosen free row is larger than the (block-rounded) request, re-record
the remainder instead of leaking it:

- Consume the chosen row (delete), as today.
- If `bestLen > allocLen`, insert a remnant free_space row at
  `(bestOff + allocLen, bestLen - allocLen, freed_xid)` where `allocLen` is the
  request rounded up to a block multiple, and `freed_xid` is the parent row's
  freed_xid. Inheriting the parent freed_xid is safe: the remnant was already
  freed at that transaction, so the same oldest-xmin gate still applies.

### Coalesce on free

When recording a freed range, merge it with an immediately adjacent existing free
row for the same storage before inserting:

- Left neighbor: a row with `offset + length == newOffset`.
- Right neighbor: a row with `newOffset + newLength == offset`.
- Merge into one row spanning the union. Set the merged `freed_xid` to the
  maximum (newest) of the merged rows' freed_xids, so reuse stays gated until
  every component free is behind the horizon. Using the newest xid is the
  conservative choice and cannot expose still-visible data.

### Command-counter discipline

Both operations mutate more than one free_space tuple per command. As the
allocator bug just fixed showed (PR #84), a second scan in the same command must
see the earlier mutations, so a `CommandCounterIncrement()` is required after the
deletes and before any follow-up scan or the adjacency lookups. This is the same
failure mode as `tuple already updated by self`; treat it as a hard requirement,
not an optimization.

## Correctness and safety

The hazard is overlap: a remnant or a coalesced range that overlaps live data or
another free range corrupts the file on the next reuse. Guards:

- Keep everything block-aligned; never split below a block.
- An assertion-only validator (debug builds) that walks a storage's live row
  groups plus its free_space rows and verifies they tile the file with no
  overlap and no gap below the highwater. Run it at the end of each compaction in
  assert builds, off in production.
- Coalesce only same-storage rows; the PK is `(storage_id, file_offset)`.

Alignment note: `allocLen` must round the request up to a block multiple so the
remnant offset stays block-aligned. The writer still writes only the exact
`dataLength` bytes; the tail of the last block of the allocation is padding, as
it already is on the extend path.

## Toggle

Per the maintenance-ops-need-lazy-option and the owner's turn-it-on-or-off
directive, gate both behaviors behind a GUC, for example
`pgcolumnar.reclaim_coalesce` (boolean, default on once proven). With it off the
allocator behaves exactly as today (whole-row best-fit, no remnant, no merge), so
the change is reversible at runtime and a suspected regression can be isolated
without a rebuild.

## Test plan

Extend `test/native_scale.sh` (or a sibling) with a variable-size-group phase
that fragments the free list on purpose: interleave groups written under
different `stripe_row_limit` settings, delete a mix, and run repeated compaction.

- Without split/coalesce this phase should show the file growing past the working
  set (the current gap), so it is a genuine failing-then-passing guard.
- With split/coalesce the file must plateau at close to the live working-set size.
- The no-overlap validator must hold after every round.
- Keep the existing uniform-size plateau assertion as a non-regression check.

Gate on the full PostgreSQL 15-19 matrix, and run the opt-in scale test at
`SCALE_N=2000000` at least once before merge.

## Why this is review-gated, not autonomous

It rewrites offset math in the storage allocator. A wrong remnant length or a bad
coalesce merges or overlaps ranges and corrupts data on the next reuse, which is
not caught by parity on the current generation and can survive to disk. That risk
class is the same one that deferred end-truncation. The design above is believed
correct; it should still be read before implementation.
