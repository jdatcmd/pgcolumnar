# pgColumnar roadmap

Status of the forward-looking work items and what remains. Every item preserves
the clean-room discipline in [../PROVENANCE.md](../PROVENANCE.md) and must land
with differential coverage against the heap oracle and pass the PostgreSQL 13-19
matrix. Gap specifications are in [gaps/](gaps/).

## Done

| Item | Where |
| --- | --- |
| Format 2.1 encodings (RLE, FOR, delta, delta-of-delta, Gorilla, dictionary) | I1-I8 |
| Vectorized scan, filter, and aggregate; late materialization | I3-I6 |
| Bloom filters, including collatable/text columns | gap 25 |
| Parallel scan | gap 23 |
| Covering `count(*)` from metadata | gap 28 (slice) |
| Sorted single-projection (`columnar.vacuum_sorted`) | gap 26 piece 1 |
| Arrow IPC export (`columnar.export_arrow`) | gap 27 |
| Parquet export (`columnar.export_parquet`) | gap 27 |
| Read stream / AIO in the scan (`columnar.enable_read_stream`) | gap 29 |
| Corrupt-input decode/reader hardening | SECURITY_AUDIT.md |
| Arrow/Parquet export type coverage (date/time/timestamp/uuid/numeric/json) | gaps/27-IMPL-export-type-coverage.md |
| Arrow IPC import (`columnar.import_arrow`) | gap 27 |
| PG18/19 coverage: generated columns, temporal constraints; REPACK investigated | PG18_19_OPPORTUNITIES.md |
| Full index-only scan (visibility-map fork, lazy vacuum, default on) | gap 28 |
| Multiple projections (C-Store): catalog, write fan-out, planner scan, vacuum, back-fill | gap 26 piece 2 |
| Arrow/Parquet nested export (arrays → List, composite → Struct/group) | gap 27 |
| Parquet import (`columnar.import_parquet`): Snappy, dictionary, data page v1/v2 | gap 27 |
| Arrow nested import (List → array, Struct → composite) | gap 27 |
| Parquet nested import (LIST → array, group → composite; Dremel level assembly) | gap 27 |
| **Gap 27 complete** — Arrow/Parquet interop: export + import, flat + nested, both formats | gap 27 |
| Read external Parquet in place: `read_parquet`, `parquet_schema`, `pgcolumnar_parquet` FDW | Phase G |
| Parquet FDW predicate pushdown (row-group skipping) and column projection pushdown | Phase G |
| Parquet read codecs: GZIP, ZSTD, LZ4_RAW (added to uncompressed and Snappy) | Phase G |
| Parquet read type coverage: uuid, numeric via DECIMAL, fixed binary, ms/us/ns time units | Phase G |
| Multi-file reads: a directory of `*.parquet` or a glob read as one relation | Phase G |
| Reader hardening against crafted files (scale/size/chunk-count guards, NaN and inverted stats) | Phase G |
| **Phase G read surface complete** — external Parquet read, pushdown, multi-file, all matrix-gated | Phase G |

## Remaining

Ordered by value-to-effort. **Gap 27 (Arrow/Parquet interop) is fully complete**:
export and import, flat and nested, for both Arrow and Parquet, all self-contained
(no libarrow/libparquet dependency) and matrix-gated. See
[gaps/27-arrow-parquet-interop.md](gaps/27-arrow-parquet-interop.md).

The concrete remaining list is complete as of 2026-07-23. The former item, skip
virtual generated-column storage, is DONE (the flush skips the chunk for
`attgenerated = 'v'` columns and the reader returns their missing value; see
`generated_columns.sh`). Phases E (ALP, FSST, chunk-shared FSST) and F (Z-order
cluster, online compaction/rewrite/recluster, physical page reclaim) also landed
on the native PGCN v1 engine, all matrix-gated on PostgreSQL 15-19. What follows
is Future directions (larger, and some deferred for review).

Deferred (documented, not yet built): end-truncation for lazy disk reclaim
(corruption-critical VM-fork/WAL hazards, see PHASE_F_RECLAIM_PLAN.md); the F1
delete-vector catalog rename (PHASE_F_PLAN.md); reclaim free-list splitting and
coalescing (design written up in PHASE_F_RECLAIM_SPLIT_COALESCE_PLAN.md, review
before implementing: it changes storage-allocator offset math and is
corruption-critical).

## Future directions

Candidate directions beyond the item above, drawn from a survey of published
columnar-engine techniques (deep-research pass, 2026-07-21). Each notes rough
effort and a primary citation. The speedup figures are self-reported by each
system's authors on their own hardware and workloads; they indicate the value of
a technique, not a guaranteed pgColumnar gain. Anything adopted still lands with
differential coverage and the PostgreSQL 13-19 matrix, and clean-room provenance
is preserved.

Already implemented on the native engine (this list predates that work; kept for
provenance, but do not treat these as open): ALP for floats and FSST for strings
(Phase E, including the chunk-shared FSST table); Z-order multi-dimensional
clustering (Phase F2/F3c, eager and online; Hilbert is still open); and richer
zone maps / SMA, i.e. per-chunk sum, value_count, and null_count are stored and
sum/count aggregates are answered from metadata when a group has no deletes
(falling back to a scan when it does, see `columnar_vector.c`). The genuinely open
directions below are the large ones: morsel-driven parallelism, data-centric JIT,
join/aggregate acceleration, delete-vector merge-on-read for MERGE, per-tier block
compression defaults, and the FastLanes on-disk format generation.

### Execution

- Adaptive, sample-based cascade encoding selection. Move from one fixed encoding
  per column to a recursive cascade (encode the output of one lightweight scheme
  with another) chosen per block by sampling a small fraction of tuples. pgColumnar
  already has the primitives (RLE, FOR, delta, dictionary); the missing piece is
  the sampling selector. High value, low-to-medium effort.
  Source: BtrBlocks, SIGMOD 2023, https://dl.acm.org/doi/10.1145/3589263 .
- Morsel-driven parallelism. Schedule small fixed-size row fragments to a worker
  pool running whole pipelines, so the degree of parallelism is elastic at runtime
  rather than fixed in the plan. The morsel unit maps onto the existing per-chunk
  parallel scan, but PostgreSQL's parallel-worker model is process-based and
  plan-fixed, so this is a large effort.
  Source: Leis et al., SIGMOD 2014, https://dl.acm.org/doi/10.1145/2588555.2610507 .
- Data-centric JIT with adaptive execution. Compile hot pipelines to machine code
  (push-based produce/consume, tuples in registers), and start each query in an
  interpreter, switching per-morsel to compiled code from runtime feedback to
  avoid the compile latency that penalizes short queries. Large effort; PostgreSQL
  already ships LLVM JIT infrastructure to build on.
  Sources: Neumann, VLDB 2011, https://www.vldb.org/pvldb/vol4/p539-neumann.pdf ;
  Kohn/Leis/Neumann, ICDE 2018, https://db.in.tum.de/~leis/papers/adaptiveexecution.pdf .
- Join and aggregate acceleration (open). Bloom or runtime join filters with
  sideways information passing, or hash-join and hash-GROUP-BY pushdown into the
  columnar scan. The research pass found no surviving primary source scoped to a
  table access method, so this needs its own investigation before a spec.

### Storage and data skipping

- Richer zone maps (Small Materialized Aggregates). Extend the per-chunk minimum
  and maximum to also carry sum, count, and null count, so aggregates can be
  answered from metadata and pruning improves on low-selectivity scans where
  indexes do not help. Low-to-medium effort, on top of the existing zone-map
  catalog. Sources: Moerkotte, VLDB 1998, https://vldb.org/conf/1998/p476.pdf ;
  Databricks data skipping, https://docs.databricks.com/aws/en/tables/data-skipping .
- Multi-dimensional clustering. Order rows by a space-filling curve (Z-order, and
  preferably Hilbert) so existing data skipping improves across several columns at
  once, with incremental background reclustering rather than one-time sorting.
  Medium effort, building on `vacuum_sorted`. Sources: Databricks (above); Delta
  Lake 3.1 Liquid Clustering, https://delta.io/blog/delta-lake-3-1/ .
- Delete vectors and merge-on-read. Mark deleted and updated rows in a side
  structure and reconcile at read time, with background compaction to reclaim,
  reducing write amplification and underpinning an efficient `MERGE`. Builds on
  the existing visibility-map fork and row mask. Medium effort.
  Source: Delta Lake 3.1, https://delta.io/blog/delta-lake-3-1/ .

### Compression and layout

- ALP for floats and decimals, and FSST for strings. ALP encodes doubles that
  originated as decimals losslessly as integers and vector-compresses genuinely
  real values, decoding faster than Gorilla and Zstd; FSST compresses short
  strings while keeping random access. Both are per-column codec upgrades. Low
  effort. Sources: ALP, SIGMOD 2024, https://duckdb.org/science/alp ; FSST is used
  by BtrBlocks and FastLanes (below).
- Reconsider default block compression. On fast local NVMe, general-purpose block
  compression (pglz, lz4, zstd) can cost more in CPU than it saves in I/O; make it
  opt-in per storage tier, and apply dictionary encoding aggressively, including on
  float columns. This finding is scoped to fast local storage and reverses for
  high-latency or remote (object-store) storage, so keep block compression the
  default there. Low effort (defaults and per-table options).
  Source: Zeng et al., VLDB 2024, https://www.vldb.org/pvldb/vol17/p148-zeng.pdf .
- FastLanes-style expression encoding. For a future on-disk format generation,
  cascade lightweight encodings over fixed 1024-value vectors with multi-column
  compression and partial bottom-up decode, so the executor receives compressed
  vectors and runs directly on them. Large effort (a new format generation) that
  targets the run-at-a-time compressed executor pgColumnar already has.
  Source: FastLanes, PVLDB vol.18, 2025, https://www.vldb.org/pvldb/vol18/p4629-afroozeh.pdf .
- Asynchronous write and background compaction (wishlist). Commit inserts in a
  fast, lightly encoded or uncompressed write-optimized form and return to the
  transaction immediately, then have a background worker rewrite the row groups
  with the full encoding cascade afterward. This hides encoder latency from the
  foreground for the heaviest encoders at scale. The classic write-optimized to
  read-optimized store split is the prior art. It must be toggleable on or off per
  table or per storage tier, since it trades foreground latency for write
  amplification, transient extra space, and slower interim reads until compaction.
  It shares the MVCC-safe row-group rewrite machinery with the mutation and
  clustering work, so it belongs with that phase. Gated on a measurement showing
  synchronous encoding cost is still a problem after the per-chunk shared FSST
  table (E3b) lands. Large effort.

### Interoperability and PostgreSQL integration

The research pass returned few surviving primary sources in this area, so these
are directions to investigate and spec, not validated recommendations:

- External Parquet read with predicate and projection pushdown is done (Phase G,
  see above). What remains here: ORC, open table formats (Apache Iceberg, Delta
  Lake, Hudi), and, within Parquet, Hive-style partition pruning, recursive
  directory walks, streaming instead of reading each file fully into memory, and
  INT32/INT64-backed DECIMAL reads.
- Arrow C Data Interface zero-copy export, and Arrow Flight SQL or ADBC access.
- New PostgreSQL 17-19 integration points: read stream and asynchronous IO (partly
  used), `MERGE`, incremental materialized views (pg_ivm), logical decoding of
  columnar changes, optimizer-statistics injection, and TOAST or large-value
  handling.

### Open questions to resolve before starting

- Which join-acceleration technique returns the most inside a table access method,
  and how does it interact with the planner and executor hooks.
- The concrete design for external file and open-table-format access (native
  reader versus foreign data wrapper) and whether it reuses the existing pruning
  metadata.
- Which PostgreSQL 17-19 APIs are the highest-leverage integration points.
- Whether the next format is a full FastLanes-style vector rewrite (larger) or an
  incremental BtrBlocks-style cascade selector on the current format 2.2 encodings
  (lower), given the run-at-a-time compressed executor.

## PostgreSQL 18/19 adoption

Features new in PostgreSQL 17-19 that pgColumnar can use, all version-gated to
preserve the 13-19 matrix. Detail and sources in
[PG18_19_OPPORTUNITIES.md](PG18_19_OPPORTUNITIES.md):

- Read stream / AIO in the scan (item 0 above) — flagship.
- Virtual generated columns (PostgreSQL 18): confirm read-time generation on a
  columnar table and add differential coverage.
- Temporal constraints (`WITHOUT OVERLAPS` in 18, `FOR PORTION OF` in 19): verify
  enforcement and add coverage.
- REPACK (PostgreSQL 19): investigate whether concurrent, lower-lock compaction is
  reachable through the table AM.
- Optimizer statistics injection (PostgreSQL 18) and a btree skip-scan benchmark
  line: smaller follow-ups.

## Test-harness follow-up

The suites are bash driving a differential oracle (heap mirror vs columnar,
compared by order-independent result hash) plus C property tests for the codecs.
One structural improvement is worthwhile when the harness is next touched: assert
EXPLAIN output from `FORMAT JSON` fields rather than text grep, and make a failed
step report which assertion failed instead of aborting under `set -e`. A wider
move to a Python/pyunit differential harness is possible later but is not
required; it should be specced before starting.
