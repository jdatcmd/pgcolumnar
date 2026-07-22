# Phase D6 plan: make native the default and finalize

Status: plan, on the `re-origination` branch (phase branches off `re-origination`
once approved). D6 is the finalize sub-phase of Phase D: default new
`CREATE TABLE ... USING pgcolumnar` to the native (PGCN v1) format, take the full
suites green on native across PostgreSQL 13-19, validate Arrow and Parquet over
native, and update the user documentation. It is the last sub-phase before the
native format is the engine's normal path.

D6 is larger than D1 through D5, because flipping the default routes every existing
suite onto native, and native does not yet support several things the default path
leans on. It is decomposed into matrix-gated sub-PRs D6a through D6f, with the
default flip sequenced last, after native has parity.

## What native already supports (D1-D5)

Sequential scan, cascade encoding and block compression, zone-map row-group and
per-vector skipping, zone-map-only aggregates, per-chunk bloom filters, and
`INSERT` / `COPY`. Arrow and Parquet import and export already work on native,
because they go through the format-agnostic scan and insert layer
(`columnar_arrow.c`, `columnar_parquet.c`, `columnar_parquet_reader.c` have no
2.2-specific catalog reads).

## The gaps that block the default flip

Each is confirmed in the code. The two "silent wrong results" gaps are the
dangerous ones: they do not error, they return zero rows, so only a differential
run on native catches them.

1. DELETE / UPDATE / MERGE: hard error. The row mask is keyed by 2.2 stripe and
   chunk ids, which native never populates. `ColumnarMarkRowDeleted`
   (`columnar_row_mask.c`) resolves the row via `rowmask_find_stripe`, which reads
   the empty native stripe list and raises "cannot delete row: no stripe covers
   it". `columnar_tuple_delete` / `columnar_tuple_update` (`columnar_tableam.c`)
   both hit this.
2. Index scan and index-only scan: silent 0 rows. `ColumnarReadRowByNumber`
   (`columnar_reader.c`) has only a 2.2 stripe path and returns false for a native
   row, so `columnar_index_fetch_tuple` finds nothing. The visibility map is
   computed from the stripe catalog only (`ColumnarComputeAllVisibleGroups`,
   `columnar_metadata.c`), so index-only scans never set VM bits either.
3. Unique and primary-key enforcement: silently not enforced. The uniqueness
   decision rides on the same `ColumnarReadRowByNumber` (via `_bt_check_unique`),
   which returns false on native, so a conflicting row looks absent and the
   duplicate commits. `columnar_unique.c` itself is format-agnostic.
4. Projections (covering, gap 26): silent 0 rows. The projection writer is hard
   wired to 2.2 (`columnar_build_write_state` never sets `isNative`), but the
   projection reader takes `isNative` from the base table's format, so on a native
   base it reads native row-groups from a storage that holds only 2.2 stripes.
5. The default selector is split. `ColumnarTableFormatVersion`
   (`columnar_metadata.c`) returns 2.2 by default and only the reader consults it;
   the writer decides native from the raw option directly
   (`columnar_write_state.c`). Flipping only the reader default would make the
   reader expect native while the writer still emits 2.2, breaking even plain
   scans. Both sites must flip together.
6. Format-dependent tests: `hardening.sh` and `corruption.sh` inject corruption by
   `UPDATE pgcolumnar.chunk ...`, which does not exist for native (native uses
   `column_chunk`); `phase5.sh` asserts the default is 2.2. These need native
   equivalents or updates.

## Design and decomposition

The high-leverage observation: index scan, index-only scan, and unique
enforcement all collapse onto one function, `ColumnarReadRowByNumber`. Giving it a
native row-group path unlocks three gaps at once. Native delete uses the interim
row-mask adapted to row groups, per the Phase D design constraint (the first-class
delete vector replaces it in Phase F); this keeps D6 to a mechanical re-keying
rather than the novel merge-on-read work.

The default flip is the last step. Until then native stays opt-in, and each parity
fix is validated by running the existing suites against native tables through a
harness native mode (a `PGC_NATIVE=1` switch that makes `make_pair` and the suite
tables select `format_version => 1`), so parity is proven before the global
default changes and the silent-wrong-results gaps cannot hide.

### D6a. Unify the format switch, and native fetch-by-row-number

- Make the writer consult the same default as the reader: both
  `columnar_build_write_state` / `ColumnarGetWriteState` and the reader derive
  native from `ColumnarTableFormatVersion`, and the projection reader derives
  `isNative` from the storage being read, not the base relid.
- Add a native row-group branch to `ColumnarReadRowByNumber`: map a row number to
  its row group (via `first_row_number` / `row_count` in `pgcolumnar.row_group`),
  load the group, and reconstruct the row, reusing the D3/D4 decode. This unlocks
  index scan, bitmap scan, and unique / primary-key enforcement on native.
- No default change yet. Validated by the native-mode index and unique suites.

### D6b. Native delete and update (interim row mask by row group)

- Add a native-aware row mask keyed by `row_group` (group number and row-in-group)
  instead of stripe and chunk. `ColumnarMarkRowDeleted` resolves a native row to
  its row group; the native scan and `ReadRowByNumber` apply the mask.
- `columnar_tuple_delete` / `columnar_tuple_update` / `MERGE` then work on native.
  This is the interim marking; Phase F replaces it with the delete vector and
  merge-on-read.

### D6c. Native index-only scan (visibility map)

- Compute all-visible groups from the native `row_group` catalog
  (`ColumnarComputeAllVisibleGroups` native path) and set the VM bits, so
  index-only scans skip the heap fetch on native.

### D6d. Native projections

- Write projection storage in the native format (the projection writer selects
  native like the base), and read it via its own storage format. Covering
  projection scans and back-fill then work on native.

### D6e. Native-mode green, and format-dependent tests

- Run the full suite in native mode (`PGC_NATIVE=1`) across 13-19 and close any
  residual gaps. Give `hardening.sh` and `corruption.sh` native-catalog
  equivalents (poke `pgcolumnar.column_chunk` / `row_group`), and update the
  `phase5.sh` default assertion.

### D6f. Flip the default, finalize, and document

- Flip `ColumnarTableFormatVersion` and the writer default to native together, so
  a bare `USING pgcolumnar` is native. Keep the 2.2 reader so existing 2.2 tables
  still read (retirement is the later Phase H).
- Full differential, fuzz, hardening, recovery, and concurrency suites green
  13-19 with native as the default.
- Confirm Arrow and Parquet import and export over native default tables.
- Update the user-facing documentation in full. Every file under `docs/`
  (index, installation, administration, configuration, features, limitations,
  sql-reference, user-guide, benchmarks, testing, ARCHITECTURE), plus README.md
  and CHANGELOG.md, describes the native (PGCN v1) format as the format and the
  `pgcolumnar` namespace. Remove any Hydra or Citus framing and any description of
  the 1.0-dev (2.2) format as current; the project keeps no such compatibility
  (a pointer to the `v1.0-dev` tag is the only mention the 2.2 line needs). Style:
  professional, no em-dashes, no extra adjectives (matches the existing user-docs
  style). This is a full pass, not a diff over the old text.

## Sequencing decision (for the owner)

Two choices to record when D6 is scheduled:

- Interim row mask vs. delete vectors now. This plan follows the Phase D design
  constraint: native delete/update uses the interim row mask in D6b, and Phase F
  replaces it with the first-class delete vector and merge-on-read. The
  alternative is to pull Phase F's delete-vector work into D6b and skip the
  interim. Recommendation: keep the interim (lower risk, smaller D6, defers the
  novel merge-on-read to its own phase).
- Whether D6a-e land as separate PRs into `re-origination` with native opt-in
  (default stays 2.2 until D6f), or whether the default flip rides in the same PR
  as the last parity fix. Recommendation: separate PRs, flip last, so any
  regression is isolated to the parity fix that caused it and the default is never
  flipped while a gap remains.

## Validation

- A harness native mode (`PGC_NATIVE=1`) so `make_pair` and the suites create
  native tables; run the differential oracle and the feature suites on native for
  each of D6a-e, proving parity before the flip.
- Full PostgreSQL 13-19 matrix, first in native mode (D6e), then with the default
  flipped (D6f).
- The differential oracle stays the correctness proof throughout: native results
  equal the heap oracle for scans, deletes, updates, index scans, unique
  enforcement, and projections.

## Risks

- Largest behavior change of Phase D: after D6f every new columnar table is
  native, so the whole suite exercises native. Mitigated by proving parity in
  native mode before the flip and by flipping last.
- Two gaps return wrong results rather than erroring (index and projection scans
  yield 0 rows on native today). The native-mode differential run is the guard;
  without it a flip could pass smoke tests yet be silently wrong.
- The interim row mask is throwaway work replaced in Phase F. Accepted, per the
  Phase D design constraint, to keep the novel delete-vector and merge-on-read
  design in its own phase.
