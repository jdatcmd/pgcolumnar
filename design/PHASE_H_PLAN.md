# Phase H plan: retire the 1.0-dev (2.2) on-disk format, clean cut

Status: DONE. Phase H is the capstone of the re-origination: remove the legacy
2.2 writer, reader, catalog, and the per-table format selector, leaving one
native format line (PGCN v1). Owner decision (2026-07-21): a clean cut, no Hydra
or Citus style page format and no on-disk compatibility shim. The `v1.0-dev` git
tag preserves the old line permanently.

Completed on `phase-h/retire-2.2`: H1 (native-only tests) merged in PR #68 with
the plan and the PostgreSQL 13/14 matrix drop; H2 (remove the 2.2 code, catalog,
selector, and the 2.2-only vectorized scan path; ~2955 lines) and H3 (retire the
two now-dead vectorized-scan GUCs and the full docs pass) follow on the branch.
Two bugs surfaced during H2 and were fixed: a stale COMMENT ON FUNCTION arity on
alter_columnar_table_reset that aborted CREATE EXTENSION, and an unconditional
pre-commit flush that reused a stale row-group id (duplicate row_group_pkey), now
guarded by an early return in columnar_flush_row_group. columnar_relation_estimate_size
gained a native row-group path. Gate: the full PostgreSQL 15-19 matrix passed
during H1; H2 and H3 each passed the PostgreSQL 17-19 iteration set.

## Prerequisites: satisfied

The retirement plan in [PHASE_D5_PLAN.md](PHASE_D5_PLAN.md) gated removal on native
parity for everything the default path serves. All of it is now in
`re-origination`:

1. Delete and update visibility: delivered in D6b via the interim row mask keyed
   by row group (the plan expected Phase F, but D6b closed it earlier; Phase F
   later replaces the row mask with delete vectors, which is not a removal
   blocker). The `row_mask` catalog is shared and stays.
2. Index scan and index-only scan: native `ColumnarReadRowByNumber` (D6a) and the
   native visibility-map path (D6c).
3. Projections: native projection storage and scan (D6d).
4. Zone-map skipping and zone-map-only aggregates: D5.
5. Native default: D6f flipped `pgcolumnar.default_format_version` to 1.
6. Arrow and Parquet verified on native: the D6 native matrix runs the
   arrow/parquet suites on native tables.

No released 2.2 data exists (pre-release, `1.0-dev`), so no migration step is
needed beyond the `v1.0-dev` tag; the spec's COPY / Arrow / Parquet paths remain
for anyone on the tag.

## What is removed, converted, kept

Full line-level inventory produced by survey; summary here. The load-bearing
discriminator is `ColumnarStorageIsNative` (and `ColumnarTableFormatVersion`),
threaded through writer, reader, row mask, metadata, custom scan, and vector
paths. Removing 2.2 collapses each of these to its native arm.

REMOVE (2.2-only):
- Writer: the 2.2 body of `columnar_flush_stripe` (stripe buffer build, 2.2
  min/max and bloom encode, `ColumnarInsertStripeRow` / `ColumnarInsertChunkGroupRow`
  / `ColumnarInsertChunkRow`); the `ColumnarWriteState.isNative` field and the
  write-state format anchoring.
- Reader: `columnar_group_can_match`, `columnar_setup_group`,
  `columnar_position_group`, `columnar_load_stripe`; the 2.2 per-row producer in
  `ColumnarReadNextRow`; the 2.2 body of `ColumnarReadRowByNumber`; the 2.2 arm of
  `ColumnarComputeAllVisibleGroups` and `ColumnarBuildLivenessCache`; the
  `readState->isNative` flag and 2.2 skip predicates.
- The vectorized scan and aggregate path, which is 2.2-only (native always takes
  the scalar path; the custom scan gates vectorization off for native):
  `ColumnarReadNextVector`, `ColumnarReadNextRawGroup`, `columnar_decode_group_columns`,
  `columnar_decode_group_to_vector`, `columnar_advance_group`,
  `columnar_next_stripe_index`, `ColumnarDecodeCurrentGroupVector`, the
  vectorization gate in `columnar_customscan.c`, and the scan-and-fold path in
  `columnar_vector.c` (`ColumnarVecSelect`, vectorized aggregate over decoded
  groups). The native zone-map aggregate path in `columnar_vector.c` is KEPT. A
  native vectorized path is a later performance phase, not part of H.
- Metadata: `ColumnarInsertStripeRow` / `ColumnarInsertChunkGroupRow` /
  `ColumnarInsertChunkRow`, `ColumnarReadStripeList`, `ColumnarReadChunkGroupList`,
  `ColumnarReadChunkList`, `ColumnarStorageHasStripes`, `ColumnarTableFormatVersion`,
  the `stripe`/`chunk`/`chunk_group` deletes in `ColumnarDeleteMetadata`, and the
  `stripe`/`chunk`/`chunk_group` Anum blocks.
- `columnar_tableam.c`: the `columnar_default_format_version` GUC and variable.
- `columnar_row_mask.c`: `rowmask_find_stripe` and the 2.2 fall-through in the
  delete-mark path.
- `columnar.h`: `StripeMetadata`, `ChunkGroupMetadata`, `ChunkMetadata`, the
  `columnar_default_format_version` extern, and the removed-function externs.
- SQL (`pgcolumnar--1.0.sql`): the `stripe`, `chunk`, `chunk_group` tables and
  indexes; the `format_version` option column and its handling in
  `alter_columnar_table_set` / `alter_columnar_table_reset`; the 2.2 `UNION ALL`
  arm of `pgcolumnar.stats`.

CONVERT (collapse to the native arm, or fix a native gap):
- `ColumnarStorageIsNative` usages and `ColumnarTableFormatVersion` callers
  (`columnar_customscan.c`, `columnar_vector.c`, writer, reader, row mask)
  collapse to the unconditional native path.
- `columnar_relation_estimate_size` (`columnar_tableam.c`): today it reads only
  `ColumnarReadStripeList`, so a native table estimates zero rows. Give it a
  native `row_group` path. This is a native correctness fix folded into H.
- `ColumnarRowIsLive`: 2.2-only with no native arm; verify it is dead (the
  projection scan uses the liveness cache) and REMOVE, else convert to row groups.
- `pgcolumnar.stats`: keep only the native (row group) branch.

KEEP (shared or native): logical storage (`columnar_storage.c`), the value codec
(`columnar_encoding.c`, `columnar_compression.c`), `ColumnarEncodeValue` /
`ColumnarDecodeValue`, `ColumnarWriteRow`, the native flush and native decode
helpers, the native catalogs (`storage`, `row_group`, `column_chunk`, `zone_map`,
`bloom`), the shared `row_mask` and `options` catalogs, `SkipPredicate` and
`columnar_build_predicates`, the liveness cache, and the table-AM entry layer.

## Decomposition (matrix-gated sub-PRs into re-origination)

Iterate gating on PostgreSQL 17-19 for speed; run the full 13-19 matrix before
the final merge.

- H1. Native-only test surface (test/ only, no src change). Make every suite
  exercise only native while the 2.2 code still compiles, so the code removal in
  H2 is validated by an all-native green suite:
  - `lib.sh`: drop `PGC_NATIVE` / `native_mode`, the `default_format_version` conf
    lines, and make `stripe_count` / `chunk_group_count` native-only.
  - Standalone suites (`smoke`, `phase2`-`phase6`, `audit`, `concurrency`,
    `unique_conc`): remove the `default_format_version=0` pins; convert 2.2-catalog
    introspection to the native catalogs (`phase2` compression/min-max/encoding,
    `phase5` compression option effects, `phase3`/`phase6`/`smoke` catalog checks)
    and remove the `format_version` option round-trip test in `phase5`.
  - Shared suites: remove the `if ! native_mode` 2.2 blocks (`differential`
    encoding/bloom/count introspection) and unwrap the `if native_mode` native
    arms (`projections`, `generated_columns`, `sorted_projection`, `corruption`,
    `hardening`).
  - `hardening.sh`: remove Parts 1-2 (2.0 compat and 2.2 corrupt-input), keep the
    native tamper block. `corruption.sh`: remove the 2.2 fall-through, keep native.
  - `native_writer.sh`: remove the legacy sub-test and the pin (native checks
    stay), or retire the suite since native coverage lives in the `native_*`
    suites.
  - Gate PG17-19.
- H2. Remove the 2.2 code, catalog, and format selector (src + SQL), and fix the
  native `estimate_size` gap. One coherent collapse-to-native change (the
  discriminator is threaded too widely to split without a broken intermediate).
  Gate PG17-19, then full 13-19.
- H3. Documentation pass: drop the `format_version` option and
  `pgcolumnar.default_format_version` from the configuration reference; remove the
  "earlier line still read / retired later" language from README, CHANGELOG,
  features, sql-reference, ARCHITECTURE, testing; keep the `v1.0-dev` tag as the
  historical pointer; qualify the vectorized-execution description (native uses the
  scalar scan path with zone-map aggregates). No em-dashes, no extra adjectives.

## Validation

- H1 green PG17-19 (native-only suite).
- H2 green PG17-19, then the full PostgreSQL 13-19 matrix, assert-enabled, all
  suites, no warnings.
- The differential oracle stays green on native throughout.
- After H2 there is no `PGC_NATIVE` mode: one format, one path.

## Open scope decision (flag to owner)

Removing the vectorized scan and aggregate path is a capability removal, not just
dead-code cleanup: it is 2.2-only today but is a documented feature. The
retirement plan classes compressed and vectorized execution as a performance
follow-up, not a removal blocker, so H removes the 2.2-coupled machinery and a
native vectorized path is a later phase. Confirm before executing H2.
