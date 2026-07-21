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

## Remaining

Ordered by value-to-effort. **Gap 27 (Arrow/Parquet interop) is fully complete**:
export and import, flat and nested, for both Arrow and Parquet, all self-contained
(no libarrow/libparquet dependency) and matrix-gated. See
[gaps/27-arrow-parquet-interop.md](gaps/27-arrow-parquet-interop.md).

1. Skip virtual generated-column storage. pgColumnar currently writes an all-null
   chunk for a virtual generated column (PostgreSQL 18+); reads are correct but
   the bytes are wasted. Skip the write for `attgenerated = 'v'` columns and have
   the reader return NULL for them. Small-to-medium write/read/vacuum change with
   its own coverage. See [PG18_19_OPPORTUNITIES.md](PG18_19_OPPORTUNITIES.md) item 2.

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
