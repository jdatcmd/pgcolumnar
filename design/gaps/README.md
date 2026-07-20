# pgColumnar gap specifications

Design specs for the forward-looking gaps identified after the format 2.1
encoding/execution work (I1-I8). Each is a self-contained plan to be picked up as
its own tested branch/PR. They are derived from the public column-store
literature (see ../IMPROVEMENT_PLAN.md) and the current implementation; they
preserve the clean-room discipline in ../../PROVENANCE.md.

Every gap here changes only *how* results are produced (or adds an optional
capability); none may change query results. Each lands with differential/fuzz
coverage proving equality against the heap oracle, and passes the full version
matrix on PostgreSQL 13-19.

| Spec | Gap | Status |
| --- | --- | --- |
| [25](25-text-bloom-filters.md) | Bloom filters for collatable/text columns | IMPLEMENTED (PR #12) |
| [22](22-position-list-late-materialization.md) | Intra-group late materialization (position lists) | IMPLEMENTED (PR #13) |
| [23](23-parallel-scan.md) | Parallel scan | IMPLEMENTED (PR #14) |
| [28](28-index-only-visibility-map.md) | Index-only scans via a visibility map | SLICE: covering count(*) (PR #15); full scan deferred |
| [21](21-native-compressed-execution.md) | Native compressed execution on packed bytes | SUBSUMED by I3 |
| [24](24-explicit-simd-kernels.md) | Explicit SIMD kernels | DEFERRED (auto-vectorized already) |
| [26](26-projections-pax.md) | Projections / PAX layout | PIECE 1 IMPLEMENTED (sorted single-projection: `columnar.vacuum_sorted`, see [26-IMPL](26-IMPL-sorted-projection.md)); multi-projection (piece 2) SPECED |
| [27](27-arrow-parquet-interop.md) | Arrow/Parquet interop | EXPORT IMPLEMENTED (`columnar.export_arrow`, [27-IMPL-arrow](27-IMPL-arrow-ipc-export.md); `columnar.export_parquet`, [27-IMPL-parquet](27-IMPL-parquet-export.md)); import SPECED |

Remaining work is tracked in [../ROADMAP.md](../ROADMAP.md): multi-projection
(gap 26 piece 2), Arrow/Parquet import and wider export type coverage (gap 27),
and the full index-only scan (gap 28 direction 1).
