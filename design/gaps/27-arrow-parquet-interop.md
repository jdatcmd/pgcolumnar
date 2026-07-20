# Gap 27: Arrow / Parquet interop

Status: exploratory. Tier: capability. Format change: additive. Effort: very large.

## Motivation

Columnar data in pgColumnar is naturally close to Apache Arrow (in-memory) and
Parquet (on-disk) shapes. Exporting to those formats gives zero-friction
interchange with the analytics ecosystem (DuckDB, pandas/pyarrow, Spark) and lets
users move large results out of Postgres cheaply. Import is a secondary,
lower-priority direction.

## Current state

None. Data is only accessible via SQL through the table access method.

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
