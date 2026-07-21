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

### Critical subtlety: synthetic TIDs vs `visibilitymap_set`

Columnar TIDs are synthetic (`block = rowNumber / MaxHeapTuplesPerPage`) and, per
`columnar.h`, are "never used to locate bytes in the data file" — there is no
heap-format page at those block numbers in the main fork. That breaks the *write*
side of the stock VM API: `visibilitymap_set` takes the heap buffer of `heapBlk`,
sets `PD_ALL_VISIBLE` on that page, and (asserts) the page is already all-visible.
There is no such page to pass. The *read* side is fine — `visibilitymap_get_status`
/ `VM_ALL_VISIBLE` only read the VM fork bit for a block number and return "not
all-visible" (0) for any block beyond the fork's current extent, so IOS stays
correct (it just fetches) regardless.

Therefore the VM bits must be written by a **custom routine that writes the VM
fork page directly** (pin/extend the VM fork, set the bit in the VM page, WAL-log
it) *without* touching a main-fork data page or `PD_ALL_VISIBLE`. This is a small
reimplementation of `visibilitymap_set`'s fork-write half, minus the heap-page
coupling. IOS reads continue to use the stock `visibilitymap_get_status`
unchanged. This is the load-bearing design decision for the whole feature and must
be prototyped and proven (a written bit is read back by `VM_ALL_VISIBLE`; a torn
write is safe) before the rest is built. It is also why this is a research-grade
subsystem, not a mechanical change.

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

**Setting is a *lazy* vacuum, not the AccessExclusiveLock rewrite.** Marking a
group all-visible only reads committed state and writes the VM fork -- it never
rewrites data -- so it belongs in the table-AM `relation_vacuum` callback
(`columnar_relation_vacuum`), which plain `VACUUM`/autovacuum invoke under
ShareUpdateExclusiveLock, concurrent with readers and writers. The heavy
AccessExclusiveLock stays only on the space-reclaiming compaction rewrite
(`columnar.vacuum`, the VACUUM-FULL analog). This keeps index-only-scan
maintenance concurrency-friendly and autovacuum-driven.

- **Set** (all-visible), in `columnar_relation_vacuum` under SUEL: compute the
  horizon (`GetOldestNonRemovableTransactionId(rel)` on 14+, `GetOldestXmin` on
  13); for each stripe whose insert xid is committed and precedes the horizon,
  and each of its chunk groups with zero deletes, mark the group's row range
  all-visible; merge adjacent all-visible ranges and set the VM bits for blocks
  fully inside a merged range (a boundary block is set only if both groups
  qualify). WAL-logged.
- **Clear**: any write clears the affected block's bit in its own transaction
  (phase 2, WAL-logged), so a committed delete/insert always leaves the block
  not-all-visible.
  - INSERT / `multi_insert`: clear the block a new row falls in.
  - DELETE / UPDATE (`ColumnarMarkRowDeleted`): clear the block of the deleted row.
  - Compaction rewrite creates a new relfilenode with a fresh (empty) VM fork,
    so no explicit clear is needed there.

**Concurrency (lazy set vs concurrent writer).** Because the set runs under SUEL,
a writer can modify a group between the vacuum's visibility check and its set.
Correctness rests on three things together: (1) the horizon -- only groups whose
inserts precede the oldest snapshot are eligible, so no in-flight *insert* is
ever marked; (2) clear-on-write -- any delete/insert clears the block's bit in
its own transaction; (3) a post-set recheck -- after setting a group's bits the
vacuum re-reads the group's delete state, including in-progress deletes via a
dirty snapshot, and clears the bits if the group was (or is being) modified. A
set can therefore never survive a concurrent delete: either clear-on-write or the
recheck removes it. This is the load-bearing concurrency protocol and MUST be
proven by the phase-5 concurrency/MVCC differential suite before the planner GUC
is enabled.

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

1. **DONE (merged).** VM fork read/write plumbing + block mapping;
   `columnar.vm_selftest` proves a written bit is read back by
   `visibilitymap_get_status`. Runtime round-trip passes; compiles PG13-19.
2. **DONE (merged).** Clear-on-write in insert/`multi_insert`/delete, WAL-logged.
   No-op until phase 3 sets bits; establishes the invariant.
3. **DONE (merged, #26/#27/#28 + #29).** Lazy set-in-vacuum:
   `columnar_relation_vacuum` (SUEL, autovacuum path) sets all-visible bits per
   the horizon + zero-delete rule, with the post-set recheck for concurrent
   modifiers. Single-session tested (insert → commit → vacuum → bits set; delete
   → vacuum → bits cleared; fresh insert not yet all-visible). Kept the
   AccessExclusiveLock only on the compaction rewrite. #29 fixed a PG18/19
   backend crash: the all-visible stripe scan used an unregistered
   `GetLatestSnapshot()`, tripping a PG18 MVCC-snapshot assertion under the
   snapshotless vacuum path — now `RegisterSnapshot`/`UnregisterSnapshot`.
   Known limitation (perf, not correctness): a synthetic block that straddles a
   chunk-group boundary — or the leading block that nominally covers the unused
   row-0 slot — is conservatively left not-all-visible, so rows in those boundary
   blocks fall back to the (correct) fetch instead of being fetch-free. Interior
   blocks of an all-visible group skip the fetch. A future optimization could
   mark the leading block when its only real rows are all-visible.
4. **DONE (merged, #30).** Planner enable behind
   `columnar.enable_index_only_scan` (PGC_USERSET): when on,
   `columnar_forbid_index_only_scan` is a no-op so the core IOS path is used,
   served from the VM fork. Tested: with the GUC on the planner builds an Index
   Only Scan, all-visible interior ranges report Heap Fetches: 0, results match
   the heap oracle, and a delete forces the snapshot-checked fetch fallback
   (still correct); with the GUC off no IOS is built.
5. **DONE.** MVCC + concurrency + recovery, then default flipped on.
   - MVCC concurrency (merged #30): a persistent REPEATABLE READ session proves
     an old snapshot never sees post-snapshot rows through an index-only scan,
     even after a concurrent VACUUM — the all-visible horizon accounts for the
     open snapshot, so the new block stays not-all-visible and the fetch fallback
     applies the snapshot.
   - Crash recovery (`recovery.sh` scenario 4): a VM bit flushed by a checkpoint
     and then cleared by a delete stays cleared after a SIGKILL crash — WAL
     replay of the clear (log_newpage_buffer) wins over the on-disk set bit, so
     no all-visible bit survives over a deleted row; a clean block keeps its bit;
     contents match the heap oracle after recovery.
   - **GUC default flipped to on** once the above were green across PG13-19: the
     protocol is proven and the not-all-visible fetch fallback is always
     snapshot-correct, so the feature is safe on by default (`set off` forces a
     plain index scan).
