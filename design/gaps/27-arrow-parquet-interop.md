# Gap 27: Arrow / Parquet interop

Status: **complete** — every slice shipped and matrix-gated. Tier: capability.
Format change: additive. Effort: very large.

## Motivation

Columnar data in pgColumnar is naturally close to Apache Arrow (in-memory) and
Parquet (on-disk) shapes. Exporting to those formats gives zero-friction
interchange with the analytics ecosystem (DuckDB, pandas/pyarrow, Spark) and lets
users move large results out of Postgres cheaply. Import is a secondary,
lower-priority direction.

## Current state

Complete in both directions, for both formats, self-contained (no
libarrow/libparquet build or link dependency; pyarrow/DuckDB are test oracles
only). What shipped:

- **Export** — `columnar.export_arrow` (hand-rolled Arrow IPC / FlatBuffers) and
  `columnar.export_parquet` (hand-rolled Thrift + Snappy). Scalars (int2/4/8,
  float4/8, bool, text/varchar, bytea, date/time/timestamp[tz], uuid, numeric,
  json), **1-D arrays** (Arrow List / Parquet LIST), and **composites** (Arrow
  Struct / Parquet group), with nulls at every level.
- **Import** — `columnar.import_arrow` and `columnar.import_parquet` into an
  existing target table. The Parquet reader parses Thrift metadata, decompresses
  Snappy, and decodes PLAIN and dictionary (RLE_DICTIONARY / PLAIN_DICTIONARY)
  values from data-page v1 and v2. Both readers reconstruct arrays and composites
  — Arrow from its List/Struct buffers, Parquet from the Dremel repetition and
  definition levels — including null arrays/elements/fields and empty arrays.
  Both bound transient memory with per-row scratch contexts.

Coverage: differential round-trips against a heap oracle plus pyarrow/DuckDB
cross-checks (`arrow_export`, `parquet_export`, `arrow_import`, `parquet_import`,
`arrow_nested`, `parquet_nested`, `arrow_nested_import`, `parquet_nested_import`),
green across the PostgreSQL 13-19 matrix. The remainder of this document is the
original exploratory design sketch, retained for provenance.

## Design (sketch -- a project)

Prefer export first, and prefer a pure-Postgres path that does not hard-depend on
libarrow/libparquet at build time:

1. Parquet export via a function: `columnar.export_parquet(rel regclass, path
   text)` (or `COPY ... TO ... (FORMAT parquet)` if a COPY handler is feasible on
   the target majors). Write Parquet directly: the format is well-specified
   (Thrift metadata + column chunks with encodings we already produce -- RLE,
   dictionary, delta, bit-packing map almost one-to-one onto Parquet encodings).
   A self-contained Parquet writer avoids a libparquet build dependency but is
   substantial; alternatively link libparquet/libarrow when present (pkg-config,
   like lz4/zstd) and compile the feature out otherwise.
2. Arrow export: produce Arrow IPC (Feather) record batches, or expose data
   through the Arrow C Data Interface (a stable ABI of struct definitions, no
   libarrow link needed) so external consumers can pull batches. The C Data
   Interface is the lightest-dependency option and pairs well with the existing
   decoded-vector batches (`ColumnarReadNextVector`).
3. Type mapping: define and document the pgColumnar-type to Arrow/Parquet-type
   mapping (numeric, timestamp units, text, bytea, arrays, jsonb-as-string), with
   explicit handling of types that have no clean Arrow equivalent.

Recommendation: start with Arrow C Data Interface export from the vector reader
(smallest dependency, immediate DuckDB/pyarrow consumption), then Parquet file
export.

## On-disk / API impact

No change to pgColumnar's own on-disk format. New SQL functions and an optional
build dependency (libarrow/libparquet) detected via pkg-config, compiled out when
absent, mirroring the codec dependency handling.

## Testing

Round-trip against a reference: export a table, read it back with DuckDB /
pyarrow (available in some environments; gate like BENCH_DUCKDB), and assert the
values equal a heap oracle. Type-matrix coverage for the type mapping. No change
to existing query results, so the differential suites are unaffected.

## Effort / risk

Very large (a format writer or an external library integration, plus a type
mapping). Risk: type-mapping fidelity and dependency/build portability across
majors and platforms. Recommendation: scope to Arrow C Data Interface export
first as a contained deliverable.

## Source

Apache Arrow (columnar in-memory format, C Data Interface) and Apache Parquet
specifications.
