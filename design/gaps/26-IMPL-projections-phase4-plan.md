# Multiple projections — phase 4 plan (grounded): planner selection

Phase 4 makes the planner choose a projection for a scan and the executor read it,
reconstructing non-covered columns from the base. Phases 1-3 (catalog, write
fan-out, reconstruction primitive) and 4a (min/max on projection chunks) are the
foundation.

## 4a (DONE): min/max on projection chunks
`columnar_init_col_defs` is now shared by the base and projection writers, so a
sorted projection stores tight per-chunk min/max — the skip metadata the planner
relies on.

## 4b: planner + executor integration

Grounded in `columnar_customscan.c` (ColumnarSetRelPathlist, ColumnarPlanCustomPath,
ColumnarBeginCustomScan, ColumnarScanNext, ColumnarExplainCustomScan).

### Path generation (ColumnarSetRelPathlist)
For a columnar baserel with additional projections (ColumnarListProjections):
- Collect the columns the query references: `rel->reltarget` vars + vars in
  `rel->baserestrictinfo`. Call this `neededCols` (table attnums).
- For each projection P (id > 0):
  - `covers` = neededCols ⊆ P.columns.
  - `skips` = some baserestrictinfo clause is an opclause on P.sort_key[0]
    (`=`, `<`, `<=`, `>`, `>=`, BETWEEN) — a predicate the sorted min/max ranges
    prune tightly.
  - Emit a CustomPath for P when `covers` (reconstruction is optional but the
    first cut requires `covers` to avoid per-row base fetches). Cost: start from
    the seqscan cost and multiply the run cost by an estimated selectivity when
    `skips` (e.g. clamp(selectivity,0.05,1.0)); a covering+skipping projection
    then costs less than the base custom path and wins via add_path.
  - Encode the choice in `cpath->custom_private`: a list of
    {projectionId, projStorageId, sortKey, columns} (as an integer list or a
    small copyable node). ColumnarPlanCustomPath copies it into
    `cscan->custom_private`.
- Always also emit the base custom path (today's behavior) so the planner has a
  correct fallback and picks by cost.

### Executor (ColumnarBeginCustomScan / ColumnarScanNext)
- If `custom_private` names a projection, build its projTupdesc
  ([rownumber int8, cols…]) and open `ColumnarBeginReadWithStorage(rel, snap,
  projStorageId, projTupdesc, NULL, projectedProjCols, nkeys, keys)` where the
  ScanKeys are the pushed-down restriction clauses translated to the projection's
  attnums (so the reader's existing chunk-skipping uses the projection min/max).
- ColumnarScanNext returns a base-shaped tuple: covered columns from the
  projection row; non-covered needed columns fetched from the base by the stored
  row number (ColumnarReadRowByNumber, which also gives liveness/MVCC). Rows not
  live in the base are skipped. For a covering projection, still consult base
  liveness (cheap row_mask check; optimize later to avoid full base decode).
- Requal/recheck: the executor already re-applies quals above the scan, so
  chunk-level skipping is an optimization, not a correctness dependency.

### EXPLAIN
ColumnarExplainCustomScan adds `Projection: <name>` (and, when reconstructing,
`Reconstructed columns: …`).

### Parallelism
First cut: a projection path is not parallel-aware (parallel_safe=false); the base
partial path still exists for parallel plans. Parallel projection scan is a later
optimization.

### GUC
Add `columnar.enable_projection_scan` (default on once proven) to force base-only
for A/B testing and safety, mirroring enable_index_only_scan.

### Correctness gates (phase 6 does the full sweep; phase 4 proves the basics)
- Differential vs heap: same query answered from a projection vs the base returns
  identical results, for covering and (later) reconstructing projections, with and
  without a sort-key predicate, after deletes/updates.
- EXPLAIN shows the projection is chosen when it covers + skips, and the base when
  it does not.
- MVCC: an old snapshot reading via a projection sees only its snapshot (liveness
  from base row_mask + base stripe visibility).

## Risks / deferrals
- Reconstruction cost model is rough; start with covering-only selection, add
  reconstruction-aware costing after.
- Parallel projection scan deferred.
- Vacuum must keep projections aligned (phase 5) — until then, a vacuum on a table
  with projections is not coordinated; phase 5 handles rewrite/realignment.
