# Phase G pushdown -- status

## Current state

PR3 is open as #101. The crash is fixed, and so is a float pushdown soundness
defect found in review. The full 15 through 19 matrix is green: 58 suites on
each of PG15/16/17/18 and all suites on PG19, zero failures, zero compiler
warnings on every major.

## What was done and merged to main

`main` tip is the merge of #100 (`748dcd0`). Phase G's first three surfaces:

- **#96** parallel test harness (+ legacy-suite `PGC_SKIP_BUILD` install-race fix).
- **#97** Phase G external-Parquet design.
- **#98** scan core: `pq_slurp_and_parse`, `pq_leaf_to_pgtype`, `parquet_schema(path)`.
- **#99** `read_parquet(path)` + the shared row engine `pq_read_rows`.
- **#100** Parquet FDW (`pgcolumnar_parquet`).

## PR3 (predicate / row-group skipping)

### Implemented
- `parse_statistics()` reads the Parquet `Statistics` field (min_value/max_value,
  with deprecated min/max fallback) into new `PqChunk` fields.
- `pq_read_rows()` gained a `const bool *skipGroup` param; skipped groups decode
  and emit nothing. import/read_parquet pass NULL.
- FDW `pqfdw_compute_skip()` turns pushable `col op const` clauses into a
  per-row-group skip mask using stats min/max. Restricted to fixed-width ordered
  physical types (INT32/INT64/FLOAT/DOUBLE), and for float/double to the
  NaN-safe directions only. Statistics are refused when the interval is inverted.
  Conservative: only skips when provably empty; the executor still rechecks, so a
  missed skip only costs work. That is not a safety net for an *unsound* skip,
  since a skipped group emits nothing to recheck.
- `pqfdwExplainForeignScan()` prints "Row Groups" / "Row Groups Skipped".
- Test `test/native_parquet_pushdown.sh` uses pyarrow to write a stats-bearing
  200k-row / 4-row-group file (our exporter does not write stats), asserts
  correctness against a heap oracle, and asserts the skip counter.

### The bug, and why the earlier diagnosis was wrong

The previous session recorded the trigger as "more than one row group decoded
AND a recheck qual present". That was a red herring. On resume, a control matrix
over four files (1 vs 4 row groups, statistics vs none) showed **every** FDW
scan corrupting memory, including a plain `SELECT count(*)` with no qual at all.
Statistics, group count, and quals were all irrelevant. `main` over the same
files was clean, so the defect was introduced by PR3.

`MemoryContextCheck` checkpoints through `pqfdwBeginForeignScan` all passed,
proving `ExecutorState` was still intact when that function returned. A gdb
backtrace on the `pfree` then named the real site:

```
tts_heap_clear -> tts_heap_store_tuple -> ExecStoreHeapTuple
  -> ExecCopySlot -> pqfdwIterateForeignScan (columnar_parquet_reader.c:2584)
```

Root cause: `ForeignNext()` calls `IterateForeignScan` in
`ecxt_per_tuple_memory`, and `ExecScan` resets that context before every fetch,
including the no-qual fast path. Whenever `tuplestore_gettupleslot()` hands the
slot a palloc'd tuple it also transfers ownership (`TTS_FLAG_SHOULDFREE`), and
the slot frees it on its next store. Allocated in the per-tuple context, that
pointer is already reclaimed by then, so the next store frees wiped memory (the
`0x7f7f7f7f...` headers) and corrupts `ExecutorState`.

The checkpoint commit's `copy=true` made this fire on every row. `copy=false`
was not a fix either: once the tuplestore spills, `readtup()` palloc's in the
same per-tuple context and sets `should_free`, which was the original spill
crash. Both symptoms are the same defect.

### The fix
Pin the context rather than the copy flag: run the tuplestore fetch in
`es_query_cxt`, where the slot's ownership stays valid until it stores the next
row. Also:
- `EndForeignScan` drops the readslot before ending the tuplestore, since a
  non-copied fetch leaves the slot pointing into tuplestore memory.
- The scan slot is `TTSOpsHeapTuple`, not virtual. `table_slot_callbacks()`
  gives foreign tables a heap slot deliberately. The old comment said virtual.
- PG18 build fix for `pqfdw_compute_skip()`: PG18 generalized index-AM
  strategies, so `get_op_btree_interpretation()` became
  `get_op_index_interpretation()` and `OpBtreeInterpretation.strategy` became
  `OpIndexInterpretation.cmptype`. Shim added to `columnar_compat.h`.

### Verification
- Full 15 through 19 matrix green: 58 suites on each of PG15/16/17/18 and all
  suites on PG19, zero failures, zero compiler warnings on any major. The matrix
  runs in the `pgcolumnar-dev` container (a clone of `plexcellent`), building from
  a copy of the read-only repo mount; logs land in `/root/matrix*.log` there, not
  in `plexcellent`. Re-run in full after each source change, including the NaN and
  inversion guards.
- A dedicated stress over `work_mem` 64kB/1MB/64MB (forcing tuplestore spill)
  plus a nested-loop rescan: 14 checks, zero memory diagnostics in the server log.
- The assert-enabled build is what surfaces this. `AllocSetCheck` reports the
  corrupted context by name, which is the fastest first signal; note it detects
  corruption at teardown, not where it was caused.

### Float pushdown was unsound for NaN (found in review of #101)

Parquet writers exclude NaN when computing min/max, so a group holding a NaN
advertises ordinary finite bounds. PostgreSQL orders NaN above every value and
treats `NaN = NaN` as true, so `col > c`, `col >= c` and `col = 'NaN'` can all
match a row the statistics do not describe. Confirmed empirically: a 4-group file
with one NaN returned 0 rows for `val > 1e300` instead of 1, with all four groups
skipped. The executor's recheck cannot recover this, because a skipped group
emits nothing to recheck.

`>` and `>=` are therefore never pushed down for float/double, `=` is refused
when the constant is NaN, and NaN-valued statistics are ignored outright.
`<` and `<=` stay sound, since a NaN row cannot satisfy those either. The +/-0
rules in the same spec block need no handling, because PostgreSQL compares -0 and
+0 as equal.

### Inverted statistics were trusted (found in review of #101)

Same shape as the NaN defect: statistics that do not mean what the comparison
assumes. Every skip test assumes `min <= max`, and the decoded interval may be
inverted without any corruption. `pq_want_phys()` maps `INT2OID` to `PQ_INT32`
and `plain_value_to_datum()` narrows with an unchecked cast, so an `int2` foreign
column over a Parquet INT32 column holding 30000 and 40000 decodes to
`min=30000, max=-25536`. `WHERE c = 30000::int2` then takes the `const > max`
branch and skips a group that genuinely contains the row, because the data path
applies the identical narrowing. Confirmed empirically: the unfiltered scan
returned `[-25536, 30000]` while the equality predicate returned 0 rows with
`Row Groups Skipped: 1`.

Parquet's `UINT_32`/`UINT_64` logical types invert the same way, since they sort
unsigned while we decode signed, as can a genuinely corrupt file. The guard
refuses to skip whenever `min > max`.

### Test-harness hardening
`pgc_set_hash()` returned an empty string when a query errored, so two failing
queries compared equal and the check passed vacuously. Four checks "passed" this
way while a server was down. It now returns a per-call unique `QUERY_ERROR.*`
marker. A genuinely empty result set still hashes to `EMPTY`, so real empty-vs-
empty comparisons are unaffected.

## Debugging notes worth keeping

- valgrind memcheck does not catch this class of bug: PostgreSQL sub-allocates
  palloc chunks inside large malloc'd blocks, so intra-block overwrites are
  invisible unless built with `-DUSE_VALGRIND`.
- `MemoryContextCheck(CurrentMemoryContext)` checkpoints bisect a corrupting
  step in one build, without gdb. It only checks the one context.
- gdb single-user with `break errfinish` gives the faulting backtrace directly:
  `runuser -u postgres -- gdb -batch -x cmds postgres`, with
  `run --single -D $PGDATA db < in.sql`. The data directory must stay 0700.

## Follow-ups deliberately not in PR3

- **Malformed-file hardening (pre-existing).** `rg->chunks` is allocated from the
  row group's own column count and left NULL when the `columns` field is absent,
  but is indexed by `pf->ncols` in `pq_read_rows()` and by `top->firstLeaf` in the
  skip path. A short or missing chunk list is an out-of-bounds read or NULL
  dereference. A `cn == pf->ncols` validation at parse time belongs with the
  `corruption` suite.
- **Float `>=` / `>` pushdown.** Disabled for NaN safety. Recovering it needs a
  per-group NaN indicator; Parquet has no widely written one, so this stays off.
- **All-NULL group skipping.** `null_count` is no longer parsed. Using it needs
  the operator's strictness checked, so it goes with the change that wants it.
- **Unchecked narrowing in `plain_value_to_datum()`.** Silently wrapping a
  Parquet INT32 40000 into an int16 -25536 is wrong on the read path regardless
  of pushdown. Either range-check and error, or refuse the bind in
  `build_imp_targets()`. The inversion guard only stops the skip path from
  trusting the result; it does not make the value correct.
- **Costing.** `pqfdwGetForeignPaths` still costs the full relation, so a highly
  selective scan is costed as if nothing were skipped.
- **Plain EXPLAIN** reports no row-group counts, since `fdw_state` is NULL under
  `EXEC_FLAG_EXPLAIN_ONLY`.

Phase G follow-ons (projection/column-chunk pushdown, streaming FDW, multi-file,
ORC/Iceberg, uuid and numeric decoder gap) are in
`design/PHASE_G_SCAN_SURFACES_PLAN.md` and
`design/PHASE_G_EXTERNAL_PARQUET_PLAN.md`.
