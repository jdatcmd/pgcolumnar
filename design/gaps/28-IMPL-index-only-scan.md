# Gap 28 (impl): full index-only scan via a columnar visibility map

Direction (1) of gap 28. Direction (2), covering `count(*)` from metadata, already
ships. This adds true index-only scans: for an all-visible range the executor
answers from the index tuple and skips the columnar fetch (and its whole
chunk-group decode).

Status: SPEC. Implementation is MVCC-critical and must be developed against the
differential/concurrency suites on a stable cluster; it is a multi-phase change,
not a single drop. On-disk impact: additive (a visibility-map fork + an
`all_visible` summary), 2.0/2.1 tables without it fall back to the current fetch.

## Why a real VM fork

PostgreSQL's index-only scan (`nodeIndexonlyscan.c`) is hard-wired to the table's
visibility-map fork: for each index tuple it calls `VM_ALL_VISIBLE(rel, block,
&vmbuf)` and only calls `table_index_fetch_tuple` when the block is *not*
all-visible. A table AM cannot substitute a different all-visible source into that
node. So to get real IOS (with the fetch skipped) pgColumnar must maintain an
actual `VISIBILITYMAP_FORKNUM` on its relation, keyed by the same block numbers
its TIDs use. This is the "literal heap-style VM" the exploratory note set aside;
it is in fact the only way the core IOS path will skip the fetch.

## TID / block / chunk-group mapping

`ColumnarRowNumberToItemPointer` packs a row number into a synthetic TID:

    block  = rowNumber / MaxHeapTuplesPerPage      (~291 rows per block)
    offset = rowNumber % MaxHeapTuplesPerPage + 1

Row numbers are global and contiguous per relation, so VM block `b` covers rows
`[b*K, (b+1)*K)` with `K = MaxHeapTuplesPerPage`. A chunk group (default 10000
rows) spans ~34 whole VM blocks plus up to two boundary blocks shared with the
adjacent group. The VM fork therefore has `ceil(nextRowNumber / K)` bits.

## All-visible rule (MVCC)

A columnar row is visible to a snapshot iff its **stripe** catalog row is visible
to that snapshot AND its **row_mask** delete bit is unset (both are ordinary
catalog rows read through `ColumnarCatalogSnapshot`). A VM block may be marked
all-visible only when *every* row it covers is visible to *every* snapshot, i.e.:

1. Each stripe covering the block is committed and older than the all-visible
   horizon `GetOldestNonRemovableTransactionId(rel)` (PG14+) /
   `GetOldestXmin` (PG13) — no snapshot can fail to see the insert.
2. `deleted_rows == 0` for every chunk group the block touches, in the committed
   catalog — no row in the block is dead for any snapshot, and no delete is
   in flight (an in-flight delete leaves an uncommitted `row_mask` row, so the
   group is not yet all-committed-clean; we require the delete count observed
   under a fresh catalog snapshot to be zero and the stripe/mask rows to be all
   committed past the horizon).

A boundary block shared by two groups is all-visible only if both groups satisfy
the rule. The determination is deliberately conservative: when in doubt, leave the
bit clear (a false "not all-visible" only costs a fetch; a false "all-visible"
returns a row a snapshot must not see — a correctness failure). It is never set
from within the inserting/deleting transaction.

## Setting and clearing bits

- **Set** (all-visible): only `columnar.vacuum` sets bits. After it computes, per
  chunk group, the committed `deleted_rows` and confirms the covering stripes are
  older than the horizon, it sets the VM bits for the fully-covered blocks (and
  boundary blocks whose both groups qualify) with `visibilitymap_set`, WAL-logged
  like heap VACUUM. Newly written groups are never all-visible until a later
  vacuum, exactly as for heap.
- **Clear**: any write that can change a block's visibility clears its bits before
  commit, via `visibilitymap_clear`:
  - INSERT / `multi_insert` / bulk load: clear the block(s) the new row numbers
    fall in (the tail blocks of the relation).
  - DELETE / UPDATE (`ColumnarMarkRowDeleted`): clear the block(s) covering the
    deleted row numbers.
  - `columnar.vacuum` compaction that rewrites stripes: clear all bits for the
    rewritten range, then re-set for groups that qualify after the rewrite.
  Clearing must be WAL-logged and must happen in the same transaction as the data
  change so a crash cannot leave a set bit over changed data.

## Planner / executor wiring

- Remove `columnar_forbid_index_only_scan` (it currently clears the IOS path in
  `columnar_build_simple_rel` / `columnar_get_relation_info`) — but gate the
  removal behind a GUC `columnar.enable_index_only_scan` (default off until the
  MVCC suite is green) so the change cannot affect correctness before it is
  proven.
- With IOS enabled, the planner considers an index-only path when the index can
  return all referenced columns; at runtime `VM_ALL_VISIBLE` reads our VM fork
  and the fetch is skipped for set blocks. Not-all-visible blocks fall through to
  `columnar_index_fetch_tuple`, which already applies snapshot + row mask, so the
  result is correct regardless of VM state — the VM only affects *whether* the
  fetch happens, never correctness, provided the set-rule is conservative.

## Crash safety / recovery

VM set/clear are WAL-logged (`visibilitymap.c` already emits
`XLOG_HEAP2_VISIBLE` / clear via the buffer). On a table AM the same routines
apply to our fork. A torn or lost VM bit is safe in one direction only (a lost
*set* just costs a fetch); a spuriously *set* bit would be a correctness bug, so
the clear-before-write ordering and WAL are mandatory. Recovery replays the VM
records with the data records.

## Testing (must pass before enabling the GUC / merging)

Differential vs heap, plus MVCC-specific scenarios, all on the assert matrix:

1. Covering query and `count(*)` served by IOS returns identical rows to heap and
   to the same query with `columnar.enable_index_only_scan = off`.
2. Snapshot isolation: open a repeatable-read snapshot, delete rows in another
   session, vacuum — the old snapshot's IOS must still see the deleted rows (the
   block must not be treated all-visible for that snapshot). This is the core
   correctness test.
3. Concurrent insert/delete during an IOS; after-vacuum all-visible; after a
   subsequent delete the bit is cleared and the fetch resumes.
4. Recovery: crash after vacuum sets bits / after a delete clears them; replay and
   verify no set bit covers a changed row.
5. `EXPLAIN` shows Index Only Scan and "Heap Fetches: 0" for an all-visible range.

## Phasing

1. VM fork read/write plumbing on the columnar relation + block/group mapping
   helpers (no planner change; GUC off). Compile + unit-level checks.
2. Clear-on-write in insert/delete/vacuum-rewrite paths, WAL-logged.
3. Set-in-vacuum for qualifying groups, WAL-logged.
4. Planner enable behind the GUC; executor uses core IOS path.
5. Full MVCC differential + concurrency + recovery suite; flip the GUC default on
   only when green across PG13-19.
