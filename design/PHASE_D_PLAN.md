# Phase D plan: native format core

Status: plan, on the `re-origination` branch. Phase D implements the native
on-disk format core from
[NATIVE_FORMAT_AND_INTERFACE_SPEC.md](NATIVE_FORMAT_AND_INTERFACE_SPEC.md): row
groups, column chunks, 1024-value vectors, a cascade of the existing lightweight
encodings chosen by sampling, Small Materialized Aggregate zone maps, and the new
`pgcolumnar.*` catalog. It does not include ALP or FSST (Phase E) or first-class
delete vectors and clustering (Phase F); those follow.

Phase D is too large for one change, so it is decomposed into sub-phases D1
through D6. Each is a matrix-gated PR into `re-origination`, keeps the
differential oracle green, and is written from the native spec and the public
PostgreSQL API only.

## Design constraints

- The native format is a new line: metapage magic `PGCN`, major version 1. It has
  no requirement to read the 1.0-dev (2.2) format. Both format lines coexist in
  the code during Phase D; the format used by a table is selected per table.
- The parts of the engine independent of the format are reused unchanged: the
  table access method plumbing, MVCC and row visibility, index and index-only
  scan support, the vectorized executor, and the Arrow and Parquet codecs
  (DESIGN_PIVOT_ORIGINAL_ENGINE.md section 3). Phase D adds a native reader and
  writer behind the interfaces those consume.
- Deletes in Phase D continue to use the existing row-mask style marking adapted
  to row groups; the first-class delete vector replaces it in Phase F.
- The default format stays 1.0-dev until D6 flips new tables to native, so every
  intermediate sub-phase keeps the matrix green with no behavior change to
  existing tables.

## Sub-phases

### D1. Format identity, new catalog, and format selection (scaffolding)

- Add the `PGCN` magic and major version 1 as constants, and the metapage fields
  for the native format (vector length, reservation marks), gated so the reader
  refuses an unknown major.
- Add the new catalog tables from the spec section 11 to the extension SQL,
  additive and alongside the existing 2.2 catalog: `pgcolumnar.storage`,
  `pgcolumnar.row_group`, `pgcolumnar.column_chunk`, `pgcolumnar.zone_map`. (The
  `delete_vector` table lands in Phase F; `projection` and `options` are reused.)
- Add a per-table format-version selector (a reloption or an option in
  `pgcolumnar.options`), defaulting to the 1.0-dev format so nothing changes yet.
- No writer or reader behavior change. Matrix green because the default is
  unchanged and the new catalog is empty.

### D2. Native writer (baseline encoding)

- When a table selects the native format, the write path lays out row groups of
  up to `row_group_limit` rows, each column as a column chunk of 1024-value
  vectors, with a per-column-chunk validity bitmap, and records `row_group` and
  `column_chunk` catalog rows.
- Start with a single baseline encoding (uncompressed, or frame-of-reference plus
  bit-packing) to establish the layout and the catalog wiring before the cascade.
- Row-number reservation, item pointers, and the insert callbacks are reused.

### D3. Native reader (sequential scan)

- Read the native layout back: iterate row groups, then column chunks, then
  vectors, applying the validity bitmap, feeding the existing vectorized scan and
  the executor.
- A native table now round-trips. Extend the differential oracle to run its
  suites against native-format tables, so heap remains the correctness oracle for
  both format lines.

### D4. Cascade encoding and adaptive selection

- Implement the cascade over vectors using the existing primitives (frame of
  reference and bit-packing, delta, delta-of-delta, run-length, dictionary,
  constant, sparse), to a bounded depth.
- Implement the sample-based selector: sample about one percent of a column
  chunk's vectors, greedily choose the cascade that minimizes encoded size, and
  record the chosen cascade as the encoding descriptor in `column_chunk`.
- The reader reconstructs values from the descriptor, and where cheap, hands the
  executor still-encoded vectors for compressed execution (dictionary-code
  predicates, run-stream aggregates, frame-of-reference and delta arithmetic).

### D5. Small Materialized Aggregate zone maps

- Compute per-vector and per-chunk minimum, maximum, sum, count, and null count
  for ordered and summable types, and store them in `pgcolumnar.zone_map`.
- Wire zone maps into skipping (prune a vector or chunk whose min and max cannot
  satisfy a pushed-down predicate, collation permitting) and into zone-map-only
  aggregates (answer an ungrouped `count`, `sum`, `min`, `max` without decoding
  when there is no residual filter), generalizing the 1.0-dev metadata `count(*)`.
- Bloom filters carry over per column chunk for equality skipping.

### D6. Make native the default and finalize

- Default new tables to the native format. Keep the 1.0-dev reader so existing
  tables still read.
- Full differential, fuzz, hardening, recovery, and concurrency suites green
  across PostgreSQL 13 through 19, for native-format tables.
- Arrow and Parquet import and export over native tables.
- Update the user-facing documentation (features, configuration, limitations,
  benchmarks) for the native format and the `pgcolumnar` namespace.

## Sequencing and dependencies

- D1 depends on Phase C (the `pgcolumnar` namespace) being merged into
  `re-origination`, because the new catalog and options live under that
  namespace.
- D2 and D3 depend on D1. D4 depends on D3. D5 depends on D4 (it stores zone maps
  the descriptor-aware reader can trust) but the zone-map computation can be
  developed alongside D2/D3 and wired in at D5. D6 depends on all prior.
- Phase E (ALP, FSST) extends D4's cascade primitives. Phase F (delete vectors,
  clustering) replaces the interim delete marking and adds the `delete_vector`
  catalog table and background compaction. Phase G (external-file interop)
  follows.

## Risks

- Two format lines in one codebase during D1 through D5. Kept manageable by
  selecting the format per table and defaulting to 1.0-dev until D6, so the
  native path is exercised only by tables that opt in and by new differential
  cases.
- The cascade selector is the most novel piece. D4 lands it behind the native
  format only, validated by the differential oracle and the codec property tests,
  so a wrong choice is a size or speed regression on native tables, never a
  correctness bug (the reader always reconstructs exactly what the descriptor
  records).
