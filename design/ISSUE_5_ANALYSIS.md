# Issue #5 design and feasibility analysis: concurrent unique-key inserts

Status: analysis only. No production code or test is changed by this document.
Scope: the correctness gap where two concurrent transactions inserting the same
unique key can both miss the conflict. All file:line references are to the
pgColumnar tree as of this writing.

## A. Current behaviour and the exact race window

### A.1 How an insert into a columnar table with a unique index actually runs

pgColumnar does **not** insert index entries itself. The table AM's insert
callbacks only buffer the row and publish its synthetic item pointer; the
executor performs index maintenance afterward, exactly as it does for heap.

1. `columnar_tuple_insert` (`src/columnar_tableam.c:212-231`) calls
   `ColumnarWriteRow` and then sets `slot->tts_tid` from the returned row number
   via `ColumnarRowNumberToItemPointer`. `columnar_multi_insert`
   (`src/columnar_tableam.c:233-251`) does the same per slot.
2. The **row number (and therefore the TID) is reserved eagerly**, when the
   first row of a stripe is buffered: `ColumnarWriteRow` calls
   `ColumnarReserveRowNumbers` under `!haveReservation`
   (`src/columnar_write_state.c:256-262`), which advances the metapage
   `reservedStripeId`/`reservedRowNumber` high-water marks under the metapage
   buffer lock (`src/columnar_storage.c:128-158`). A whole `stripe_row_limit`
   run is reserved up front, so every buffered row has a final, stable TID at
   insert time (`src/columnar_write_state.c:264`). This is the Phase 4 eager
   reservation.
3. Back in the executor, `ExecInsert` calls `ExecInsertIndexTuples` after the
   AM's `tuple_insert` returns (upstream `nodeModifyTable.c`; confirmed in the
   PG source tree at `nodeModifyTable.c:1199/1240/2328`). That drives the btree
   `index_insert`, which runs `_bt_check_unique` and writes `(key, TID)` into
   the btree. **The btree index page goes through shared buffers and WAL
   immediately** -- it is a normal index AM, not deferred like the columnar heap.

So for a unique btree index, index entries **are already inserted eagerly, per
row, during the statement**, carrying the eagerly reserved synthetic TID. What
is deferred is only the columnar *row data*.

### A.2 When row data becomes visible to other backends

Buffered rows are held per (relation, subtransaction) in backend-private memory
(`ColumnarWriteState`, `src/columnar_write_state.c:53-87`) and are flushed to
storage + catalog by `columnar_flush_stripe`
(`src/columnar_write_state.c:432-624`). Flush happens:

- when a stripe fills (`src/columnar_write_state.c:333-334`),
- at statement executor-end (`columnar_executor_end`,
  `src/columnar_tableam.c:902-912`),
- at `finish_bulk_insert` for COPY/CTAS (`src/columnar_tableam.c:253-263`),
- at scan start / index build for the same relation
  (`src/columnar_tableam.c:110-111`, `660-661`),
- and at transaction pre-commit (`columnar_xact_callback` ->
  `ColumnarFlushAllPendingWrites`, `src/columnar_tableam.c:844-865`).

The stripe/chunk/chunk_group catalog rows written by the flush are ordinary heap
tuples, so they obey MVCC: another backend sees them only after the inserting
transaction commits (or, under a dirty snapshot, as in-progress). **Before the
flush, a freshly inserted columnar row exists only in the inserting backend's
private memory and is invisible to every other backend.**

### A.3 How the btree uniqueness check resolves a columnar TID

`_bt_check_unique` (PG source `nbtinsert.c:408+`) finds an existing index entry
with the same key and calls `table_index_fetch_tuple_check(heapRel, &htid,
&SnapshotDirty, &all_dead)` (`nbtinsert.c:560`). That wraps the AM's
`index_fetch_tuple` (`tableam.c:table_index_fetch_tuple_check`). Two outcomes
matter:

- If the fetch returns **true**, btree reads `SnapshotDirty.xmin`/`xmax`
  (`nbtinsert.c:586-587`). If a valid XID is present, it waits on that
  transaction (`xwait`) and rechecks; otherwise it treats the row as a committed
  live duplicate and raises the unique violation.
- If the fetch returns **false**, btree treats the entry as dead and keeps
  scanning -- **no conflict is reported.**

pgColumnar's `columnar_index_fetch_tuple` (`src/columnar_tableam.c:429-455`)
resolves a TID by row number:

```
if (!ColumnarReadRowByNumber(rel, snapshot, rowNumber, ...) &&   // flushed stripes
    !ColumnarBufferedRowByNumber(rel, rowNumber, ...))           // THIS backend's buffer
    return false;
```

- `ColumnarReadRowByNumber` (`src/columnar_reader.c:903-...`) reads the stripe
  list under `ColumnarCatalogSnapshot(snapshot)`. Crucially,
  `ColumnarCatalogSnapshot` returns a non-MVCC snapshot unchanged
  (`src/columnar_metadata.c`, "if (base == NULL || !IsMVCCSnapshot(base)) return
  base"), so `SnapshotDirty` is passed straight through to the catalog scan.
- `ColumnarBufferedRowByNumber` (`src/columnar_write_state.c:353-423`) walks
  `ColumnarWriteStates`, which is a **backend-private** list. It can only see the
  *calling* backend's own buffered rows.

The function never sets `SnapshotDirty.xmin`/`xmax` itself; the only way an
inserter XID reaches btree is incidentally, when the catalog scan inside
`ColumnarReadRowByNumber` runs `HeapTupleSatisfiesDirty` on a flushed-but-
uncommitted stripe row and stamps the passed-through `SnapshotDirty` with that
row's xmin.

### A.4 The exact race window

Two backends insert the same key `k` into a columnar table with a unique btree
index:

```
T1: INSERT k  -> tuple_insert buffers row, TID=R1 (eager reservation)
             -> ExecInsertIndexTuples -> btree _bt_check_unique (finds nothing)
                                      -> writes (k,R1) into shared btree page
   ... T1's inserting statement is still in flight; R1 is NOT flushed ...

T2: INSERT k  -> tuple_insert buffers row, TID=R2
             -> ExecInsertIndexTuples -> btree _bt_check_unique finds (k,R1)
                -> table_index_fetch_tuple_check(columnar, R1, SnapshotDirty)
                   -> columnar_index_fetch_tuple(R1):
                      ColumnarReadRowByNumber(R1): no committed/flushed stripe
                          covers R1 (T1 has not flushed)         -> false
                      ColumnarBufferedRowByNumber(R1): scans T2's own buffers;
                          R1 lives in T1's private memory         -> false
                   -> returns FALSE
                -> btree treats (k,R1) as dead, reports NO conflict
             -> writes (k,R2). No error.

Both commit. Two live rows with key k. Uniqueness violated.
```

The window is exactly: **from the moment T1's index entry is written until T1's
inserting row is flushed to storage.** It closes at T1's statement executor-end
flush (`src/columnar_tableam.c:910`). Same-statement duplicates are caught,
because `ColumnarBufferedRowByNumber` sees the first row in the *same* backend's
buffer (`src/columnar_tableam.c:415-427`, `src/columnar_write_state.c:341-352`).
Inserts separated in time are caught, because by then T1 has flushed and
committed and `ColumnarReadRowByNumber` finds R1.

### A.5 Root cause

The btree dirty-snapshot uniqueness protocol assumes the heap AM can, for any
index TID, (a) see an in-progress conflicting tuple written by *another* backend
and (b) return that backend's inserting XID so btree can wait on it. pgColumnar
can do neither for an unflushed row: it has **no cross-backend visibility of
buffered rows** and **no per-row XID**. Its transactional visibility is at
stripe-catalog granularity and only materialises at flush. Aborted inserts are
handled correctly for free (an aborted transaction's stripe catalog rows are
MVCC-invisible, so `ColumnarReadRowByNumber` returns false and the dangling
btree entry is inert -- the same reasoning already documented for
`columnar_index_delete_tuples`, `src/columnar_tableam.c:502-536`).

## B. Candidate fixes

### (i) Rely on eager index insertion + btree dirty-snapshot check

**Observation that reframes this option:** eager per-row index insertion with the
reserved TID is *already implemented* (section A.1). The btree already runs its
dirty-snapshot check. The gap is not the insertion -- it is that
`columnar_index_fetch_tuple` cannot resolve a not-yet-flushed, cross-backend row
into a "wait on XID" answer. To make option (i) actually correct, the conflicting
row would have to be (a) visible to other backends and (b) carry a resolvable
inserting XID *before* its index entry is exposed. Two ways to get there, both
bad:

- **Flush every row (or every key) to shared storage before its index entry is
  inserted.** Then `ColumnarReadRowByNumber` under `SnapshotDirty` finds the
  flushed-but-uncommitted stripe row, and the catalog `HeapTupleSatisfiesDirty`
  stamps `SnapshotDirty.xmin` with the inserter XID, so btree waits correctly.
  - Correctness: sound in principle.
  - Insert performance: catastrophic. It defeats the entire columnar batching
    model -- one stripe (or a stripe per key) per row, a metapage extension-lock
    round trip and multiple catalog inserts per row.
  - Format compatibility: technically still format 2.0, but it produces
    pathological 1-row stripes and massive catalog bloat; not viable in practice.
  - Abort/rollback: unchanged (aborted stripes are MVCC-invisible).
  - Verdict: infeasible.

- **Maintain a shared per-row inserting-XID map** (shared memory or a side heap
  table keyed by row number) that `columnar_index_fetch_tuple` consults and uses
  to stamp `SnapshotDirty.xmin`. This is re-implementing heap tuple visibility on
  the side.
  - Correctness: sound, but large and subtle (crash recovery of the map, cleanup
    on abort, memory bounding, savepoints).
  - Cross-version: heavy `smgr`/shared-memory surface; high PG13-19 maintenance
    cost.
  - Verdict: disproportionate; effectively rebuilds what columnar deliberately
    does not have.

The hard sub-questions from the brief, answered:
- *Cleanup of entries for aborted/never-flushed rows:* already correct -- fetch
  returns false, entry is inert, reclaimed by REINDEX.
- *Does the heap-fetch-for-visibility work when the conflicting row is in another
  backend's private buffer?* No. That is the entire bug.
- *Interaction with row mask and `index_fetch_tuple`:* the row mask is applied
  correctly on fetch (`src/columnar_reader.c:957-972`); it is orthogonal to this
  bug, which is about rows that are not yet in any stripe at all.

### (ii) Transaction-scoped key-value lock (analogous to issue #4)

Mechanism: when inserting a row into a table that has a unique index, compute a
lock key from the unique key value(s) and take a **transaction-scoped advisory
lock** (`SET_LOCKTAG_ADVISORY` + `LockAcquire(..., /*sessionLock=*/false)`),
exactly the primitive used by the issue #4 fix
(`src/columnar_metadata.c:586-597`). Take it in `columnar_tuple_insert` /
`columnar_multi_insert`, before the row is buffered, for each applicable unique
index. Two concurrent inserts of the same key then serialize; the loser waits
until the winner's transaction ends.

Why the second inserter reliably sees the conflict: the lock is held until
transaction end. When T2 finally acquires it, T1 has either

- **committed** -- T1's executor-end flush already ran inside T1's transaction,
  so T1's stripe catalog rows are committed and visible. T2's normal btree
  `_bt_check_unique` now finds `(k,R1)`, `ColumnarReadRowByNumber` finds the
  committed row, `SnapshotDirty.xmin` is invalid (committed) -> definite
  conflict -> unique violation raised. Correct.
- **aborted** -- T1's stripe rows are MVCC-invisible, fetch returns false, no
  conflict -> T2 proceeds. Correct.

So the key lock converts the racy in-flight window into a clean serialized
check, and it reuses the existing, already-tested issue #4 lock primitive. **No
on-disk format change** (advisory locks only), and `SET_LOCKTAG_ADVISORY` /
`RelationGetIndexList` / opclass lookups are stable across **PG13-19**.

Hard parts, each with an honest assessment:

1. **Identifying the unique key and computing a collision-safe key.** In the
   insert callback we have the `Relation` and the slot values. We can walk
   `RelationGetIndexList`, open each index, and keep the ones that are unique (or
   exclusion). For each we know its key attnums / expression tree and its
   `indnullsnotdistinct` flag (`pg_index.indnullsnotdistinct`, present PG15+; on
   PG13/14 NULLs are always distinct). The **critical correctness constraint**:
   two key values that are *equal under the index's operator class* MUST hash to
   the same lock key, or they get different locks and the race survives. Raw
   datum bytes are unsafe -- e.g. `numeric` `1.0` vs `1.00`, `citext`,
   `float8 -0.0` vs `0.0`, non-default collations. The hash must therefore go
   through the opclass equality/hash support function (the type's hash opclass
   for the index's operator family), not memory bytes. For key types with no
   compatible hash support, fall back to a coarser lock (see below). This is
   strictly harder than issue #4, whose key is exact integers.

2. **Lock-table exhaustion (the biggest practical problem).** A transaction-
   scoped lock is held until commit. A bulk insert of N distinct keys would try
   to hold N advisory locks simultaneously and blow through
   `max_locks_per_transaction` ("out of shared memory"). Issue #4 never hits this
   because a transaction touches few chunk groups. The fix is to **hash keys into
   a bounded bucket space** (e.g. `key mod B`, B fixed) so a transaction holds at
   most B advisory locks. Equal keys still map to the same bucket, so correctness
   is preserved; the cost is false serialization between unrelated keys that
   share a bucket. Bucketing is mandatory for (ii) to be usable.

3. **False sharing / performance.** Even bucketed, every row acquires (and holds)
   one advisory lock per unique index -- a `LockAcquire` on a partitioned shared
   lock table per row. For a table under heavy concurrent same-table insert load,
   bucket collisions serialize independent inserters, reducing throughput. For
   single-writer bulk load the cost is a per-row fast-path lock acquire, on top
   of the btree descent already paid. Non-trivial but bounded.

4. **Multi-column, partial, expression indexes; multiple unique indexes.**
   - Multi-column: hash all key columns together (in order, with the per-column
     opclass hash).
   - Partial: only lock when the row satisfies the index predicate (evaluate
     `ii_Predicate`), matching where the btree would actually enforce uniqueness.
   - Expression: evaluate the index expression via an `ExprContext` to get the
     key value before hashing -- extra per-row executor work.
   - Multiple unique indexes: one lock per applicable unique index. Multiplies
     cost 2-4 and the bucket budget.

5. **NULL handling.** With default (NULLS DISTINCT) semantics, a key containing a
   NULL never conflicts -- skip locking for such rows. With `NULLS NOT DISTINCT`
   (PG15+), NULLs do conflict -- must lock. Honor `indnullsnotdistinct`.

6. **Deadlocks.** Rows arrive row-by-row through the executor, so we cannot
   globally order key-lock acquisition the way issue #4 orders chunk locks
   (`rowmask_chunk_cmp`, `src/columnar_row_mask.c:70-83`). Two transactions each
   inserting keys `{a,b}` in opposite order can form an AB-BA cycle. The deadlock
   detector resolves it by aborting one transaction; the surviving behaviour is
   correct but a genuine same-key conflict can surface as a deadlock abort rather
   than a unique-violation error. Acceptable, but a behavioural wart to document.

7. **Same-statement duplicates still work.** Advisory locks are re-entrant within
   a backend, so re-locking the same bucket succeeds; within-statement duplicates
   continue to be caught by the existing `ColumnarBufferedRowByNumber` path. No
   regression.

Verdict: **feasible and format-safe, but with real complexity** in opclass-safe
hashing, lock-table bounding, and per-row cost. It does not make columnar behave
like heap; it *serializes same-key (or same-bucket) inserters* so the ordinary
btree check works after commit.

### (iii) Hybrids and alternatives

- **Coarse per-unique-index lock (the simplest correct increment).** Take a
  single transaction-scoped advisory lock keyed by `(relation OID, index OID)`
  -- not by the key value -- whenever a row is inserted into a table that has a
  unique index. This serializes *all* concurrent inserts to that table's unique
  index, which is obviously correct (only one inserter is ever in its window at a
  time) and trivially bounded (one lock per unique index per transaction, so no
  lock-table blowup, no opclass hashing, no NULL/expression subtleties). The cost
  is that concurrent inserts to a unique-constrained columnar table no longer run
  in parallel. For an append-mostly analytics store this is often acceptable, and
  it is a strictly smaller and lower-risk change than (ii). It is effectively
  (ii) with a single bucket.

- **Speculative-insertion / `ON CONFLICT` style.** pgColumnar stubs the
  speculative callbacks (`src/columnar_tableam.c:538-552`). Full speculative
  support is strictly harder than (i) and does not by itself close the cross-
  backend visibility gap. Not recommended.

- **Defer uniqueness to flush with a recheck.** At `columnar_flush_stripe`, after
  writing the stripe, probe the unique index for each just-written key and raise
  on a committed duplicate. This still cannot see another *uncommitted* backend's
  buffered rows, so two transactions that both flush at pre-commit can still race
  the recheck window; it only narrows the window, and it complicates pre-commit
  error handling. Weaker than (ii). Not recommended.

## C. Recommendation

**Recommend: keep this as a documented limitation now, and treat option (ii) --
in its bounded, opclass-safe form, with option (iii)'s coarse per-index lock as
the first increment -- as the sanctioned fix to implement only if the project
commits to concurrent cross-backend uniqueness as a hard guarantee.**

Reasoning:

- Option (i), fully correct, is infeasible without either destroying the columnar
  batching model (flush-per-row) or rebuilding heap-style per-row visibility on
  the side. Both are out of proportion to a narrow, timing-dependent gap.
- Option (ii) is genuinely feasible and format-compatible, but it is not a small
  or low-risk change: opclass-safe key hashing, lock-table bounding via
  bucketing, partial/expression/NULLS-NOT-DISTINCT handling, multiple indexes,
  deadlock-vs-unique-error behaviour, and a measurable per-row cost. It does not
  restore heap semantics; it serializes same-bucket inserters.
- The current behaviour is already correct for the common cases (single session;
  inserts separated in time; same-statement duplicates) and fails only for
  genuinely concurrent, overlapping same-key inserts -- an unusual pattern for an
  append-oriented columnar store, and one that a heap staging table or an
  application-level guard also addresses.

If the owner decides the guarantee is required, implement in two steps: ship the
coarse per-index lock (iii) first (small, obviously correct, low risk), then, only
if its serialization proves too costly in practice, refine to the bucketed
per-key form (ii). Document the limitation in either case until a fix lands.

## D. Implementation and test plan (only if a fix is chosen)

### D.1 Files that would change

- `src/columnar_tableam.c` -- in `columnar_tuple_insert` and
  `columnar_multi_insert`, before `ColumnarWriteRow`, call a new
  `ColumnarLockUniqueKeys(rel, slot)` helper. `columnar_tuple_update` inserts a
  new row too (`src/columnar_tableam.c:578-598`) and would need the same call for
  the new tuple.
- `src/columnar_metadata.c` (or a new `src/columnar_unique.c`) -- the helper:
  resolve unique indexes via `RelationGetIndexList`, evaluate predicate/
  expressions, compute the opclass-safe bucketed key, and take the advisory lock
  with the existing `SET_LOCKTAG_ADVISORY` pattern
  (`src/columnar_metadata.c:586-597`). Reuse the murmur finalizer already present
  (`src/columnar_metadata.c:~555-566`).
- `src/columnar.h` -- declare the helper and any GUC (e.g. a bucket count, or a
  `columnar.enable_unique_insert_lock` switch defaulting on, to make the cost
  opt-outable).
- `test/` -- a new deterministic two-session test (below). No on-disk format
  change, so no catalog/metapage edits and no `columnar--1.0.sql` change.

### D.2 Deterministic two-session race test (mirrors test/concurrency.sh)

Reuse the existing FIFO-driven session harness in `test/concurrency.sh`
(`start_session`, `send`, `send_wait`, and the `pg_stat_activity` lock-wait
barrier). The barrier that made the issue #4 test deterministic works here too:
poll `pg_stat_activity` until session 2 is actually blocked on a lock, rather
than sleeping.

Setup: `CREATE TABLE u (k int) USING columnar; CREATE UNIQUE INDEX ON u (k);`

Race case (must fail before the fix, pass after):
```
s1: BEGIN;
s1: INSERT INTO u VALUES (1);      -- executor-end flush + key lock held, uncommitted
s2: BEGIN;
s2: INSERT INTO u VALUES (1);      -- must BLOCK on the key lock (barrier: poll
                                   --   pg_stat_activity for the lock wait)
s1: COMMIT;                        -- releases the lock; s2 unblocks
s2: <the blocked INSERT now errors with a unique_violation>   -- assert error
s2: ROLLBACK;
assert: SELECT count(*) FROM u WHERE k = 1;  -- exactly 1
```
Without the fix, s2 does not block, both insert, both commit, and the count is 2
-- the test fails, proving it catches the bug.

Concurrency-preserved case (must not regress): two sessions inserting *different*
keys (with enough buckets, different buckets) must not block each other -- assert
that s2's insert of key 2 completes while s1 holds key 1 uncommitted. (With the
coarse per-index lock of option (iii), this case is expected to serialize; the
test should assert serialization instead, which is the honest behaviour of that
variant.)

Additional cases worth covering: multi-column unique index; partial unique index
(rows outside the predicate must not serialize); a type whose equal values differ
in byte representation (e.g. `numeric` `1.0` vs `1.00`, or `citext`) to prove the
opclass-safe hash actually serializes them; `NULLS NOT DISTINCT` on PG15+;
same-statement duplicate (`INSERT ... SELECT` with two equal rows) to prove the
existing in-buffer detection still fires; and an aborted first inserter (s1
ROLLBACK) after which s2's insert of the same key must succeed.

### D.3 Verification matrix

Build assert-enabled and run the new test plus the full suite on PG13-19 via
`test/run_all_versions.sh`, matching the project's end-of-phase gate.
