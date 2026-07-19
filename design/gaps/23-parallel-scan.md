# Gap 23: parallel scan

Status: IMPLEMENTED (feat/gap23-parallel-scan). Tier: performance (largest user-visible win). Format change: none.

## Motivation

The custom scan currently removes the parallel paths for plan stability, so a
large columnar scan or aggregate runs on a single core. Analytic workloads are
exactly where parallelism pays off, and the columnar layout (independent stripes
and chunk groups) parallelizes cleanly. This is likely the biggest remaining
real-world speedup.

## Current state

- `columnar_customscan.c`: `set_rel_pathlist_hook` replaces the seqscan path with
  a non-parallel custom scan and drops parallel paths.
- `columnar_reader.c`: `ColumnarBeginRead` already accepts a
  `ParallelTableScanDesc` parameter (currently always NULL) -- the plumbing point
  exists but is unused.

## Design

Make the custom scan parallel-aware:

1. Planner: expose a partial custom path (`CUSTOMPATH_SUPPORT_*`,
   `parallel_workers`), so the planner can put a Gather above a parallel columnar
   scan for large relations.
2. Executor: implement the CustomScan parallel callbacks --
   `EstimateDSMCustomScan`, `InitializeDSMCustomScan`, `InitializeWorkerCustomScan`
   (and `ReInitializeDSM`). Put a shared atomic "next chunk-group index" (or
   next-stripe) counter in the DSM segment.
3. Work distribution: each worker atomically claims the next chunk group (or
   stripe) across the whole relation and scans it with its own read state.
   Chunk-group granularity balances better than stripe granularity for skewed
   stripe sizes; stripe granularity is simpler and has less contention -- start
   with stripe-level claiming, measure, refine to chunk-group if needed.
4. The vectorized aggregate: a partial-aggregate parallel plan (workers produce
   partial aggregates, Gather + Finalize combines). Simplest first step: only
   parallelize the base scan (Gather above it, aggregation stays above Gather),
   then add partial aggregation as a follow-up.

Row-mask, min/max skipping, bloom, projection, and encoding all already work per
read state, so a worker's read state behaves like the serial one over its subset.

## On-disk / API impact

None on disk. New parallel CustomScan callbacks and a DSM layout (a shared
counter plus the immutable scan parameters). No new format fields.

## Testing

- Differential: run every scan/aggregate query with
  `max_parallel_workers_per_gather` = 0 and > 0 and assert identical results
  against the heap oracle (extend lib.sh to toggle it).
- A dedicated test forcing a parallel plan (small `parallel_setup_cost`,
  `parallel_tuple_cost`, `min_parallel_table_scan_size`) and asserting via
  EXPLAIN that a Gather over the parallel columnar scan is chosen and returns
  correct rows.
- The full matrix (parallel worker APIs differ across majors -- watch the
  CustomScan parallel callback signatures in columnar_compat.h).

## Effort / risk

Large. Risk: cross-version CustomScan parallel API differences; correct DSM
lifecycle; ensuring read-your-writes (pending writes are per-backend, so a
parallel scan must flush and see only committed/flushed data consistently, as the
serial scan does via ColumnarFlushWriteStateForRelation before scan start --
workers must not each re-flush). Design note: flush in the leader before workers
launch.

## Source

MonetDB/X100 (vectorized, cache-efficient execution) motivates CPU parallelism;
PostgreSQL parallel-query and custom-scan parallel APIs.
