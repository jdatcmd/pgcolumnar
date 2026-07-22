# pgColumnar pivot: TODO

Status: durable, uncommitted (kept in the working tree, not in git history) while
the provenance pivot is in progress. Authoritative task list for the pivot from a
Citus/Hydra-compatible reimplementation to an original engine. Full rationale and
design are in [DESIGN_PIVOT_ORIGINAL_ENGINE.md](DESIGN_PIVOT_ORIGINAL_ENGINE.md);
research-backed feature directions are in [ROADMAP.md](ROADMAP.md) "Future
directions". Both of those are also uncommitted.

## Decisions made (owner, 2026-07-21)

- Storage-format compatibility with Citus/Hydra is not required.
- SQL API compatibility with Citus/Hydra is not required.
- Legal review is not required.
- Project name stays **pgColumnar**. SQL namespace becomes **`pgcolumnar`**
  (schema, extension, access method, GUC prefix, functions), replacing the
  Hydra-mirrored `columnar`.

## Repository strategy (decided 2026-07-21, owner)

- Keep the existing repo `github.com/jdatcmd/pgcolumnar`. Nothing about the
  1.0-dev work is disowned; it was a valid practice round.
- The `v1.0-dev` tag permanently preserves the practice-round snapshot, so no
  history needs to be frozen to keep it. `main` is kept on the 1.0-dev line only
  as an installable stable line during the transition, not for preservation.
- **`re-origination` is a long-lived integration branch** off `main`. Each phase
  below lands as its own short-lived, matrix-gated PR **into `re-origination`**
  (same per-PR discipline as the 1.0-dev work), not as one accumulating branch.
- A single merge `re-origination` -> `main` happens at the end, once the pivot is
  stable and the owner approves. `re-origination` is meant to become `main`, not
  to fork away permanently.
- Commits and pushes to `re-origination` and its phase branches are fine. Do NOT
  merge to `main` until the end.

**Done (2026-07-22): the cutover happened.** The core pivot (Phases A-D, H) is
complete, so `re-origination` was merged into `main` with owner approval. `main`
now carries the native engine; the legacy 1.0-dev line is preserved only by the
`v1.0-dev` tag. Going forward, the remaining feature phases (E2, F, G) branch off
`main` directly and land as matrix-gated PRs into `main`; the `re-origination`
integration branch is retired.

## Phases (each matrix-gated; none reads upstream source)

- [x] **Phase A. Provenance reset (docs only).** Done (PR #51 into
  `re-origination`). PROVENANCE.md reframed with a re-origination section and log
  entry; REWRITE_PLAN.md and FORMAT_AND_INTERFACE_SPEC.md marked as the 1.0-dev
  record and superseded as the build source; build source repointed at the new
  spec (Phase B).
- [x] **Phase B. New format and catalog specification.** Done: written to
  design/NATIVE_FORMAT_AND_INTERFACE_SPEC.md from the research (FastLanes vectors,
  BtrBlocks cascade + adaptive selection, ALP, FSST, SMA zone maps, delete
  vectors, opt-in block compression, space-filling clustering) and the open
  Arrow/Parquet/ORC specs, under the `pgcolumnar` namespace with a new format
  identity (magic `PGCN`, major version 1). It is the build source for the
  re-originated engine. Refine during implementation (defaults, exact catalog
  types) as Phases C onward proceed.
- [ ] **Phase C. Rename and re-namespace to `pgcolumnar`.** Extension, schema,
  access method, GUC prefix, functions. Mechanical, fully matrix-gated; keeps the
  current format for now and yields a running engine under the new namespace.
- [x] **Phase D. New format core.** Vector-based, cascade-encoded format with
  adaptive selection, SMA zone maps, as a new format line behind the existing
  reader/writer interfaces. Differential oracle green throughout. Decomposed in
  [PHASE_D_PLAN.md](PHASE_D_PLAN.md) into matrix-gated sub-phase PRs into
  `re-origination`. Done: all of D1 through D6 merged; native (PGCN v1) is the
  default on-disk format, proven across PostgreSQL 13-19 in both the native
  default and the legacy (`PGC_NATIVE=0`) modes.
  - [x] D1. Format identity (PGCN v1), new `pgcolumnar.*` catalog (storage,
    row_group, column_chunk, zone_map), and per-table format selection; default
    stays 1.0-dev, no behavior change. Done (commit 4a8b013, PR #55).
  - [x] D2. Native writer (row groups, column chunks, 1024-value vectors,
    validity) with a baseline encoding. Done: D2a per-table format selection
    (e74dcfd, PR #56), D2b baseline native writer (44d8b89, PR #57).
  - [x] D3. Native reader (sequential scan); differential oracle runs on native
    tables. Done (commit 0086c52): native catalog read helpers,
    row_group.first_row_number, native reader branch in columnar_reader.c
    (per-group [validity][values] decode via ColumnarDecodeValue), native tables
    use only the base scan (vectorized custom scan / metadata aggregate skip
    native), test/native_roundtrip.sh registered in the matrix. Full PG 13-19
    matrix green (all 32 suites incl. native_writer + native_roundtrip; no
    warnings). Sequential scan only; index fetch and delete/update visibility on
    native tables are later sub-phases (native_roundtrip is insert-only).
  - [x] D4. Cascade encoding + adaptive selection; encoding descriptor. Done
    (commit e76966c): the existing lightweight-encoding toolkit
    (columnar_encoding.c) and block-codec layer (columnar_compression.c) are wired
    into the native flush (columnar_flush_row_group) and load
    (columnar_native_decode_chunk) seams. Each 1024-value vector is encoded
    adaptively and the chunk optionally block-compressed; the choice is recorded
    in the per-vector column_chunk.encoding_descriptor (no schema change) and the
    reader reconstructs the exact raw bytes. test/native_encoding.sh registered in
    the matrix. Full PG 13-19 matrix green (all 33 suites; no warnings). Scope per
    the D4 plan (Option A): reuse the per-vector selector + block codec.
    Deferred: multi-level cascade chaining and sample-based selection (D4b);
    compressed execution (needs a native vector path; correctness-neutral).
  - [x] D5. SMA zone maps (min/max/sum/count/null per vector and chunk); skipping
    and zone-map-only aggregates; per-chunk bloom. Done, planned in
    [PHASE_D5_PLAN.md](PHASE_D5_PLAN.md). D5a (d10a435): compute and store per-vector
    and per-chunk zone maps in pgcolumnar.zone_map, no read change. D5b: native
    tables take the custom scan's scalar path so pushed-down predicates drive
    zone-map row-group skipping (f7433a4); zone-map-only ungrouped aggregates
    count/sum/avg/min/max with the int2/int4 writer sum (85b6f29); per-chunk bloom
    filters for equality skipping in a new pgcolumnar.bloom (82e5e9b); and
    per-vector (1024-row) skipping within a group, decoding neither skipped
    vectors nor their rows (0f800a8). Full PG 13-19 matrix green (all 37 suites
    incl. native_zonemap/skip/agg/bloom/vecskip; no warnings). Design note: the
    plan assumed the base scan receives scan keys, but a seqscan pushes none, so
    the custom scan (scalar path) is the qual-pushdown mechanism for native.
  - [x] D6. Default new tables to native; full suites green 13-19; Arrow/Parquet
    over native; user docs updated. Planned in
    [PHASE_D6_PLAN.md](PHASE_D6_PLAN.md). Done, decomposed with the flip last:
    D6a unify the switch + native ReadRowByNumber, unlocking index/unique
    (e2dbcd0, PR #61); D6b native delete/update via the interim row mask by row
    group (98ff493, PR #62; delete vectors replace it in Phase F); D6c native
    index-only scan via the visibility map (eb60fbf, PR #63); D6d native
    projections (7e049ce, PR #64); D6e native-mode suites green and
    format-dependent tests made mode-aware (25d5800, PR #65), which also fixed
    three real bugs surfaced by native-default running (projection-scan liveness
    cache 2.2-only, concurrent native storage-row race on storage_pkey, native
    parallel scan not claiming row groups from the shared counter) and a
    PostgreSQL 18 snapshot-registration assertion; D6f flip the default to native
    plus a full user-docs pass and a native-aware pgcolumnar.stats (033be24,
    PR #66). A PGC_NATIVE harness mode validated native before the flip and now
    selects the legacy line after it. Gate: full PG 13-19 matrix, assert-enabled,
    ALL VERSIONS PASSED with native as the default and again with PGC_NATIVE=0.
- [ ] **Phase E. New codecs.** Add ALP (floats/decimals) and FSST (strings) as
  cascade primitives; adaptive selector chooses among the full set.
- [ ] **Phase F. Mutation and clustering.** Replace the row_mask with native
  delete vectors and merge-on-read; add space-filling-curve clustering with
  background reclustering; wire up `MERGE`.
- [ ] **Phase G. Interop extension.** Extend Arrow/Parquet interop toward reading
  external Parquet and open-table-format (Iceberg/Delta) files with predicate and
  projection pushdown.
- [x] **Phase H. Retire the 1.0-dev (2.2) on-disk format (REQUIRED, clean cut).**
  Done, planned in [PHASE_H_PLAN.md](PHASE_H_PLAN.md). Removed the legacy 2.2
  writer, reader, and catalog (`stripe`, `chunk`, `chunk_group`, inline skip
  lists), the per-table format selector, and the 2.2-only vectorized scan path,
  leaving one native (PGCN v1) format line. A clean cut, no Hydra or Citus style
  page format and no storage-format compatibility (owner, 2026-07-21); the
  `v1.0-dev` tag permanently preserves the old line. The prerequisites were met
  by D5/D6 (the retirement plan expected delete/update parity from Phase F, but
  D6b's interim row mask closed it earlier; Phase F later swaps the row mask for
  delete vectors). Decomposed: H1 native-only tests plus the PostgreSQL 13/14
  matrix drop (PR #68); H2 remove the 2.2 code, catalog, and selector, ~2955
  lines, and give columnar_relation_estimate_size a native path (PR #69); H3
  retire the two now-dead vectorized-scan GUCs and the full docs pass (PR #69).
  Two bugs surfaced and were fixed during H2: a stale COMMENT ON FUNCTION arity
  on alter_columnar_table_reset that aborted CREATE EXTENSION, and an
  unconditional pre-commit flush that reused a stale row-group id (duplicate
  row_group_pkey). Gate: the full PostgreSQL 15-19 matrix, assert-enabled,
  ALL VERSIONS PASSED, native-only, no warnings.

## Independent later work (not gated on the format reset)

- [ ] Morsel-driven parallelism.
- [ ] Data-centric JIT with adaptive interpret-then-compile.
- [ ] Join acceleration (runtime bloom filters / sideways information passing;
  hash-join and hash-GROUP-BY pushdown); needs its own investigation first.
- [ ] PostgreSQL 17-19 integration points (read stream/AIO, MERGE, pg_ivm,
  logical decoding, optimizer-statistics injection, TOAST); spec before starting.

## Carry-over reminder (from the design doc)

Most of the engine carries over with renaming only (TAM handler, MVCC/row
visibility, IOS visibility-map fork, indexing, unique-insert serialization,
vectorized executor, custom scan, codec algorithms, Arrow/Parquet interop). What
is re-originated: on-disk layout + metapage, the metadata catalog, the SQL
surface, and the specification document.
