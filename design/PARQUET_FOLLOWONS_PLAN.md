# Parquet read follow-ons: DECIMAL widths, recursive walk, partition pruning

Plan written 2026-07-24, before any code. These are the three remaining items on
the Parquet side of `design/ROADMAP.md` after the read surface, pushdown,
multi-file reads, and streaming landed. They are sequenced smallest first, and
the third depends on the second.

Everything here follows the rule that cost this session real time: a guard or a
behaviour is not covered until a test fails without it. See
`test/mutate_guard.py` and the notes in `PHASE_G_FOLLOWON_HANDOFF.md`.

## 1. INT32/INT64-backed DECIMAL reads

Today a `numeric` target binds only when the leaf is `converted_type == DECIMAL`
with a `FIXED_LEN_BYTE_ARRAY` or `BYTE_ARRAY` physical type
(`pq_want_phys_for`). The Parquet spec also allows DECIMAL backed by `INT32` or
`INT64`, which is what writers use for small precisions, and `pq_leaf_to_pgtype`
does not advertise those either, so `parquet_schema` describes such a column as a
plain integer and a `numeric` binding fails.

Work:

- `pq_leaf_to_pgtype`: when `converted_type == DECIMAL` and the physical type is
  `INT32` or `INT64`, advertise `numeric(p,s)` rather than `int4`/`int8`, with the
  same precision and scale bounds already applied to the byte-array forms
  (`1 <= p <= 38`, `0 <= s <= p`). Those bounds exist because the decoder trusts
  scale for zero-fill; the new path must reuse them, not re-derive them.
- `pq_want_phys_for`: accept `PQ_INT32`/`PQ_INT64` for `NUMERICOID` under the same
  conditions.
- Decode: build the numeric from the integer value and the scale. The existing
  `pq_decimal_to_numeric` takes big-endian two's-complement bytes; the integer
  forms are little-endian in the file, so either byte-swap into the existing path
  or add a small integer-to-numeric path. Prefer reusing one conversion so scale
  handling has a single implementation.
- A `numeric` column whose declared typmod disagrees with the file's scale must
  behave exactly as the byte-array path does today. Whatever that is, pin it in a
  test rather than assuming it is right.

Tests (`test/native_parquet_flba.sh` is the natural home, it already covers the
byte-array DECIMAL forms):

- pyarrow writes DECIMAL as INT32/INT64 for small precisions; confirm which
  precisions produce which physical type in this pyarrow version rather than
  assuming the spec's thresholds, then assert round-trips for one of each.
- `parquet_schema` advises `numeric(p,s)` for both.
- Precision or scale out of bounds is rejected cleanly.
- Negative case: verify the round-trip test fails on the pre-change build with the
  current "does not support" error.

Docs: `limitations.md` currently says an INT32/INT64-backed DECIMAL is not read;
that line goes, and the type table's numeric row loses its qualifier for import.

## 2. Recursive directory walk

`pq_resolve_paths` reads `*.parquet` directly inside a directory and does not
descend. Partition pruning needs the descent, so it comes first and ships on its
own.

Design decisions to make deliberately, not by default:

- **Opt-in or always?** A directory of parquet files with an unrelated
  subdirectory would change meaning if descent became unconditional. Proposal:
  descend by default, because a directory tree of parquet files is the common
  layout and #116 already skips non-file entries, but state it in the docs as a
  behaviour change and cover the "subdirectory of unrelated files" case in a test.
- **Depth bound.** A fixed maximum depth (proposal: 32) with a clean error rather
  than silent truncation, so a pathological tree cannot walk forever.
- **Symlink loops.** `AllocateDir`/`ReadDir` do not detect cycles. A symlink to an
  ancestor makes the walk infinite. Either refuse to follow symlinked directories,
  or track visited `(st_dev, st_ino)` pairs. Proposal: do not descend into
  symlinked directories, and say so; it is the cheaper rule and the one a user can
  reason about. A symlink to a *file* is still followed, as today.
- Ordering stays deterministic: sort the full resolved list, so a nested layout
  reads in a stable order across systems.
- The #116 classification rule is preserved exactly: a path we cannot `stat` is
  kept so the open reports it; directories are descended into rather than skipped;
  other non-regular entries are skipped.

Tests: a nested tree reads as one relation; depth bound raises rather than
truncating; a symlink loop terminates; a symlinked directory is not descended; a
non-parquet file deep in the tree is ignored; ordering is stable. Each with the
negative case checked.

## 3. Hive-style partition pruning

A layout like `events/dt=2026-01-01/region=eu/part-0.parquet` encodes column
values in directory names. Two features: expose them as columns, and skip whole
files when a predicate excludes a partition value.

The design question is how partition columns are known, and there are two honest
options:

- **Declared.** A foreign-table option, for example
  `OPTIONS (path '/data/events', partition_columns 'dt,region')`, with the column
  types taken from the foreign table's own declarations. Explicit, no inference,
  and it fails loudly when the tree does not match.
- **Inferred.** Parse `key=value` from the path components of the first file and
  assume uniformity, as `parquet_schema` already does for the schema.

Proposal: **declared**, for the same reason the column definition list is
required rather than inferred: a wrong guess here silently changes what rows a
query sees. Inference can be added later behind the same option.

Work, once the walk exists:

- Resolve each file to its partition values by matching path components against
  the declared columns; a file missing a declared component is an error, not a
  null, so a malformed tree is loud.
- Bind partition columns as virtual columns: they are not in the file, so the
  scan materializes them per file from the path. They must not be counted against
  the file's leaf columns, which is where the "every leaf must be declared" rule
  would otherwise collide. That interaction is the main implementation risk and
  should be settled first, in code, before the pruning half.
- Prune at plan time: a restriction clause on a partition column that excludes a
  file's value drops the whole file before it is opened. This reuses the shape of
  `pqfdw_compute_skip` but at file granularity, and it is strictly cheaper than
  row-group skipping because nothing is read.
- `EXPLAIN ANALYZE`: add a `Files Pruned` counter beside the existing `Files`.
- Scope: FDW only. `read_parquet` has no place to declare partition columns, and
  overloading its column definition list would be guessing.

Tests: values materialize correctly; a predicate on a partition column prunes
files (assert the counter, not timing); a predicate on a non-partition column
still works; a malformed tree raises; partition columns combine with row-group
skipping on file columns. Negative cases throughout.

## Sequencing and gating

1. DECIMAL widths. Independent of the other two, smallest, lands first.
2. Recursive walk. Ships alone so the behaviour change is reviewable on its own.
3. Partition pruning, on top of the walk.

Per PR: PostgreSQL 18 and 19. Full 15 through 19 matrix once at the end of the
three, per `PHASE_G_FOLLOWON_HANDOFF.md`. Every new suite goes into the `SUITES`
array in `run_all_versions.sh` in the same commit that adds it, which is the gap
#111 left and #113 had to fix.
