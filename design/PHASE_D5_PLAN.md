# Phase D5 plan: Small Materialized Aggregate zone maps, and retiring the 1.0-dev format

Status: plan, on the `re-origination` branch (a `phase-d/d5-zone-maps` branch off
`re-origination` once approved). D5 gives the native (PGCN v1) format data
skipping: per-vector and per-chunk Small Materialized Aggregates (minimum,
maximum, sum, value count, null count) stored in `pgcolumnar.zone_map`, wired into
scan skipping and into zone-map-only aggregates, with optional per-chunk bloom
filters for equality skipping. It closes the last read-side gap between the native
format and the 1.0-dev line for scan performance.

This document also plans, at the end, the retirement of the legacy 1.0-dev (2.2)
on-disk format, which the re-origination exists to reach. That removal is a late,
prerequisite-gated capstone, not part of D5; it is planned here because it is the
destination the sub-phases are walking toward.

## What already exists (the base)

- `pgcolumnar.zone_map` catalog (`pgcolumnar--1.0.sql:216-228`): storage_id,
  group_number, column_index, vector_index (-1 for the whole column chunk),
  minimum bytea, maximum bytea, sum numeric, value_count, null_count, with a
  unique index on (storage_id, group_number, column_index, vector_index). It is
  unused scaffolding: no C struct, Anum, insert, or read helper exists, nothing
  writes it, and `ColumnarDeleteMetadata` (`columnar_metadata.c:945-952`) does not
  clean it. D5 fills all of that in.
- Min/max accumulation already runs for native inserts: `ColumnarWriteRow`
  (`columnar_write_state.c:362-403`) maintains per-column min/max with the column's
  btree compare proc and collation on every insert, native included. D5 adds sum
  and counts and moves the granularity to per vector and per chunk.
- The 2.2 mechanisms to mirror: skip-list build and evaluation
  (`columnar_reader.c` `columnar_build_predicates` :329-399 and
  `columnar_group_can_match` :408-500), scan-key construction shared with the
  aggregate (`columnar_customscan.c` `ColumnarBuildScanKeys` :357-380), the
  metadata `count(*)` path (`columnar_vector.c` `columnar_try_metadata_count`
  :961-1002), and the bloom module (`columnar_bloom.c`
  `ColumnarBloomBuild`/`ColumnarBloomProbe`, format-agnostic, currently wired only
  to the 2.2 writer/reader).
- Native currently uses only the base access-method scan: the vectorized custom
  scan returns early for native (`columnar_customscan.c:538-546`) and the
  vectorized aggregate returns early for native (`columnar_vector.c:645-651`), so
  native scans run through `ColumnarReadNextRow` with no skip or metadata-aggregate
  logic today.

## Design

The native write and read seams from D2b through D4 are the attach points, and the
correctness contract is unchanged: zone maps only ever cause a scan to skip data
that provably cannot match, or to answer an aggregate the catalog already holds
exactly, so results are identical with or without them. The differential oracle
stays the proof.

### Compute and store (write)

In `columnar_flush_row_group` (`columnar_write_state.c:744`), during the existing
per-column, per-vector loop that D4 added, compute for each 1024-value vector:
minimum and maximum (btree compare proc and collation, as `ColumnarWriteRow`
already does), value count and null count (from the validity bitmap and the
present count), and sum as a `numeric` accumulator for summable types (int2, int4,
int8, float4, float8, numeric). Fold the per-vector results into the whole-chunk
zone map (vector_index -1). Insert the rows through a new
`ColumnarInsertZoneMapRow` helper (with a matching struct and Anum accessors, and
a `ColumnarReadZoneMapList`), mirroring the column_chunk plumbing. Extend
`ColumnarDeleteMetadata` to delete zone_map rows with the rest of a relation's
catalog.

Zone maps are stored only for types with a default btree ordering (min and max)
and, for sum, a defined addition; other columns get value_count and null_count
only. Minimum and maximum are serialized with `ColumnarEncodeValue`, the same codec
the 2.2 skip list and the value stream use.

### Skip (read)

Extend the native read path to prune using zone maps:

- Row-group skip: in `columnar_read_start` / `columnar_native_load_group`
  (`columnar_reader.c:722`, `:845`), before loading a row group's bytes, evaluate
  the pushed-down predicates against the whole-chunk zone map and skip the group
  when its min and max prove no row can match, reusing `columnar_build_predicates`
  and the strategy logic of `columnar_group_can_match`. The base scan already
  receives the scan keys; no custom scan is required.
- Vector skip: within a loaded group, use per-vector zone maps to skip the 1024-row
  vectors of a column chunk that cannot match, decoding only surviving vectors
  (late materialization at vector granularity, which the D4 per-vector descriptor
  boundaries make addressable).

Skipping respects collation exactly as the 2.2 path does: a predicate whose
collation differs from the column's own collation is applied as a filter and does
not drive skipping, so results never depend on pushdown.

### Zone-map-only aggregates (read)

Generalize the 1.0-dev metadata `count(*)` to native. For an ungrouped
`count`, `sum`, `min`, or `max` with no residual filter, answer from the
whole-chunk zone maps without decoding, mirroring `columnar_try_metadata_count`
but reading `pgcolumnar.zone_map` and accounting for null counts. This requires a
narrow native aggregate entry point, since the vectorized aggregate currently skips
native; it is added for the zone-map-answerable cases only, falling back to the
base scan otherwise.

### Bloom filters (read, optional)

Add the `pgcolumnar.bloom` catalog table from spec section 11 (storage_id,
group_number, column_index, filter bytes) and wire the existing `columnar_bloom.c`
into the native writer (build a per-column-chunk filter for hashable,
deterministic-collation types) and the native reader (probe it in the equality
case of the skip check). This carries the 2.2 bloom behavior to native.

## Scope for D5, and a likely split

D5 is larger than D1 through D4. It is cleanest as two matrix-gated PRs, mirroring
the D2a/D2b split:

- D5a: compute and store zone maps at native flush (per vector and per chunk),
  add the catalog struct/Anum/insert/read helpers, and clean them up on delete. No
  read behavior change; validated by reading back `pgcolumnar.zone_map` and by the
  differential oracle staying green. Low risk.
- D5b: use the zone maps: native row-group and vector skipping, zone-map-only
  ungrouped aggregates, and the optional per-chunk bloom. Validated by result
  parity with skipping on and off, and by skip-counter assertions.

Deferred and correctness-neutral: multi-column and space-filling clustering (Phase
F), compressed execution over encoded vectors (still deferred from D4), and the
D4b cascade refinements.

## Retiring the 1.0-dev (2.2) on-disk format

Goal: remove the legacy 2.2 on-disk format so the engine carries one native format
line, which is the purpose of the re-origination. This is a dedicated late phase,
added to the roadmap as Phase H, not part of D5. It is planned here so the
intervening sub-phases aim at it.

### What is removed

- 2.2 writer: `columnar_flush_stripe` and the 2.2 min/max and bloom encode block
  (`columnar_write_state.c` ~520-728), and the `!isNative` branch of
  `ColumnarWriteRow`.
- 2.2 reader: the stripe/chunk sequential decode path in `columnar_reader.c` gated
  by `!isNative`, `ColumnarReadRowByNumber`'s stripe path, and the 2.2 skip
  predicates once native skipping (D5) subsumes them.
- 2.2 catalog access and tables: `ColumnarInsertStripeRow`,
  `ColumnarInsertChunkGroupRow`, `ColumnarInsertChunkRow`,
  `ColumnarReadStripeList` and the chunk/chunk-group readers
  (`columnar_metadata.c`), and the tables `pgcolumnar.stripe`,
  `pgcolumnar.chunk`, `pgcolumnar.chunk_group` (inline skip lists live in `chunk`).
- The per-table `format_version` option and `ColumnarTableFormatVersion` collapse
  to a single format; `columnar_tableam.c` stays format-agnostic and simply always
  drives the native reader and writer.

Shared code is kept: the logical storage layer (`columnar_storage.c`), the value
codec (`columnar_encoding.c`, `columnar_compression.c`), `ColumnarEncodeValue` /
`ColumnarDecodeValue`, the options catalog, and the table-AM entry layer.

### Hard prerequisites (why it cannot happen at D5)

Removal is blocked until the native format has parity for everything the default
path serves today, because the default for `CREATE TABLE ... USING pgcolumnar` is
still the 2.2 format (`ColumnarTableFormatVersion` returns 2.2 when the option is
unset, `columnar_metadata.c:1284-1292`) and the whole differential and feature
suite runs on that default. Native gaps that must close first:

1. Delete and update visibility. The row mask is keyed by 2.2 stripe and chunk ids
   (`columnar_row_mask.c`), so native rows cannot be marked deleted. Phase F
   (native delete vectors and merge-on-read) closes this.
2. Index scan and index-only scan. `ColumnarReadRowByNumber`
   (`columnar_reader.c:1659-1677`) walks only the 2.2 stripe list; a native
   by-row-number fetch over row groups is required.
3. Projections. The projection writer reuses the 2.2 stripe writer and the
   projection scan lives in the custom scan, which skips native.
4. Zone-map skipping and zone-map-only aggregates. Delivered by D5.
5. Native default. Phase D6 flips the default to native.
6. Arrow and Parquet import and export verified on native (no native-specific
   coverage exists today).

Compressed storage already has native parity (D4). Compressed execution over
encoded vectors remains a performance follow-up, not a removal blocker, because the
base scan reconstructs exact values.

### Sequencing and migration

Ordered: D5 (this phase) closes skipping. Phase D6 makes native the default and
takes the full suite green on native. Phase F gives native delete and update
parity and native index and projection parity land alongside. Only then, Phase H:

1. Provide and verify a migration from 2.2 to native while the 2.2 reader still
   exists: `COPY` or Arrow/Parquet export from a 2.2 table into a native table, and
   `ALTER TABLE ... SET ACCESS METHOD` rewrite where it applies. This is the
   migration path the spec already names (section 13); the `v1.0-dev` tag preserves
   the old line permanently regardless.
2. Remove the 2.2 writer, so no new 2.2 data is produced.
3. Remove the 2.2 reader and catalog, and drop the format selector.

### Decision (owner, 2026-07-21): clean cut, no compatibility

The project keeps no Hydra or Citus style page format and no storage-format
compatibility. Phase H is a required clean cut: the 2.2 writer and reader are
removed together, not kept as a transitional read-only reader. Migration off 2.2
is by COPY or Arrow/Parquet export/import while the 2.2 reader still exists at the
start of H, and the `v1.0-dev` tag permanently preserves the old line for anyone
who needs it. No on-disk compatibility shim is carried forward.

## Validation

- A native zone-map test (`test/native_zonemap.sh`, registered in the matrix):
  populate a native table whose columns have known ranges, read back
  `pgcolumnar.zone_map` and assert per-vector and per-chunk min/max/sum/counts;
  then assert result parity for range and equality queries with skipping enabled
  versus a heap mirror, and assert skip counters advance (a selective predicate
  skips groups and vectors) with a row-count sanity check so a down cluster cannot
  pass falsely.
- Zone-map-only aggregate parity: ungrouped count/sum/min/max on native equal the
  heap oracle, and are answered without decoding (assert via the read stats).
- The differential oracle green on native tables with skipping on.
- Full PostgreSQL 13-19 matrix (restored from the temporary 17-19 iteration set
  before the final pass).

## Risks

- Skipping is the first native read-side optimization that changes what the scan
  touches. It is correctness-neutral by construction (skip only provably
  non-matching data; collation-gated), validated by parity with skipping on and
  off and by the differential oracle.
- Sum accumulation must not overflow: use a `numeric` accumulator, and omit sum
  when the type has no defined addition, matching the catalog's nullable sum.
- Retiring 2.2 is high-impact and is deliberately deferred behind explicit parity
  prerequisites and a verified migration path, so it is a mechanical removal once
  native is the sole, full-featured format, never a correctness risk to live data.
