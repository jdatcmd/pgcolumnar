# Phase G: read external Parquet in place (design, for review)

Status: proposed, not implemented. This is a large net-new capability, not a
correctness-critical change to existing storage, so it is written up for review
and a surface decision before code. Parquet first; ORC and the table formats
(Iceberg, Delta) are follow-on phases that build on the same scan core.

## Goal

Query Parquet files that pgColumnar did not write, in place, without importing
them into native storage. Today `pgcolumnar.import_parquet` reads a Parquet file
and copies it into a native columnar table. Phase G scans the external file
directly on every query, so there is no copy and no second source of truth.

## Architecture: one scan core, two surfaces

Both surfaces are thin wrappers over a single scan engine, so the reader, type
mapping, projection, and predicate logic are written and tested once.

Scan core (`ColumnarParquetScan`): given a file path, the set of requested
columns, and any pushable predicates, it opens the file, parses the footer, maps
the Parquet schema to a tuple descriptor, and yields tuples one row group at a
time, reading only the requested column chunks and skipping row groups whose
statistics exclude the predicates. It reuses the self-contained decode already in
`columnar_parquet_reader.c` (used by `import_parquet`); the implementation
factors out a "decode one row group into slots" primitive that both import and
this scan call.

Two surfaces sit on top:

- **FDW** (`CREATE FOREIGN TABLE t (...) SERVER parquet OPTIONS (filename '...')`).
- **Set-returning function** (`SELECT * FROM pgcolumnar.read_parquet('/path') AS
  t(col1 int, col2 text, ...)`).

## The two surfaces, with pros and cons

Both read the same files through the same core; they differ in how the planner
sees them and therefore in how much can be pushed down.

### Foreign-data wrapper

A `parquet` FDW: `CREATE SERVER`, `CREATE FOREIGN TABLE` with a `filename`
(or `path`/`directory`) option, and the scan callbacks (`GetForeignRelSize`,
`GetForeignPaths`, `GetForeignPlan`, `Begin`/`Iterate`/`EndForeignScan`).

Pros:
- **Full planner integration.** It is a relation: it participates in joins, has
  statistics and row-count estimates, shows up in `EXPLAIN`, and can be granted
  on, viewed, and referenced by name across sessions.
- **Both pushdowns are natural.** Projection comes from the scan's target list
  and `attrs_used`; predicates come from `scan_clauses`, which the core turns into
  row-group skipping (with the executor rechecking, so correctness never depends
  on the pushdown being complete).
- **Stable schema.** The column list and types are declared once in the foreign
  table and validated against the file, so queries are ordinary SQL with no
  per-query column definition.
- **Partitioning.** A foreign table per file plus native partitioning, or a
  directory option, gives partition pruning.

Cons:
- **Setup ceremony.** Requires `CREATE SERVER` and `CREATE FOREIGN TABLE` before
  the first query; heavier for a one-off look at a file.
- **Schema must be declared** (or introspected into a `CREATE FOREIGN TABLE`); a
  file whose schema drifts needs the foreign table redefined.
- **More code.** The FDW callback surface is larger and more version-sensitive
  than a function.

### Set-returning function

`pgcolumnar.read_parquet(path text)` returning `SETOF record`, with the caller
supplying the column definition (`AS t(...)`), plus a convenience
`pgcolumnar.parquet_schema(path)` that returns the file's inferred columns and
types so users can write the `AS` list (or generate a `CREATE FOREIGN TABLE`).

Pros:
- **Zero setup.** `SELECT * FROM pgcolumnar.read_parquet('/data/x.parquet') AS
  t(...)` works immediately; ideal for exploration, scripts, and one-offs.
- **Dynamic path.** The path is an argument, so it can come from a variable or
  another query; good for iterating over files.
- **Small, self-contained.** One C function over the shared core.

Cons:
- **Limited pushdown.** A set-returning function is opaque to the planner: the
  `WHERE` clause is applied by the executor *after* the function returns, so there
  is no predicate-based row-group skipping, and projection is only available if
  the function inspects its expected result columns (we will do that, so it reads
  only the needed column chunks, but predicate skipping is not available).
- **Per-query schema.** The `AS (...)` column list must be written on every call
  (mitigated by `parquet_schema()`), and a wrong list is a runtime error.
- **No relation identity.** It is not a table: no persistent name, no statistics,
  weaker `EXPLAIN`, and it re-reads the footer on every call.

### Guidance we will document

Use the **FDW** for anything queried repeatedly, joined, or performance-sensitive
(you get predicate pushdown and real planning). Use the **function** for quick,
ad-hoc, or dynamic-path reads where setup is not worth it. Because both sit on the
same core, results are identical; only planning and pushdown differ.

## Pushdown

- **Projection**: both surfaces read only the referenced column chunks (Parquet is
  columnar, so this is a direct win). The FDW gets the referenced columns from the
  plan; the function from its result tuple descriptor.
- **Predicate / row-group skipping**: the FDW turns pushable `scan_clauses`
  (`col op const` on ordered types) into checks against each row group's min/max
  and null-count statistics, skipping groups that cannot match, and the executor
  rechecks every returned row so a partial or absent pushdown is always correct.
  The function does not receive the query predicate and so does not skip on it.

## Type and encoding coverage

Reuse the import path's coverage: integer, float, boolean, decimal, date, time,
timestamp, uuid, json, and binary/text; dictionary encoding; Snappy, Zstd, and
gzip compression; and nested types (Parquet LIST to array, group to composite,
via Dremel level assembly). Anything the import reader already decodes, the scan
core decodes, because they share the primitive. Types with no PostgreSQL mapping
are surfaced as an error at plan time (FDW) or bind time (function), not silently
dropped.

## Multi-file datasets (follow-on within Parquet)

A dataset is often many Parquet files under a directory, optionally partitioned by
Hive-style path segments (`year=2024/month=01/...`). After single-file works, add
a `directory` option (FDW) and a `read_parquet_dir()` variant (function) that scan
all matching files, surface partition path segments as columns, and prune
partitions from predicates on those columns.

## Tests

- Differential: for a set of files, assert that scanning externally returns the
  same rows (order-independent hash) as `import_parquet` followed by a native
  scan. This makes the existing, trusted import path the oracle for the scan path.
- Round-trip: `export_parquet` a native table, scan it back through both surfaces,
  compare to the original.
- Pushdown correctness: a query with a selective predicate returns the correct
  rows AND provably skips row groups (assert via a scan counter or `EXPLAIN`
  output), and returns identical rows to the no-pushdown path.
- Type coverage: one file per logical type and per compression, flat and nested.
- Edge cases: empty file, single-row-group file, all-null column, a column absent
  from the file, a wrong `AS` list (function) and a schema-mismatched foreign
  table (FDW) both error cleanly.
- Both surfaces run on the full PostgreSQL 15-19 matrix; the FDW callback surface
  is the most version-sensitive part and is exercised there.

## Sequencing

1. Design + surface decision (this document).
2. Scan core: factor the "decode one row group into slots" primitive out of the
   import reader; add footer parse to tupdesc and a row-group iterator with
   projection.
3. Function surface (`read_parquet` + `parquet_schema`) over the core -- smallest,
   proves the core end to end.
4. FDW surface over the core, with projection then predicate pushdown.
5. Multi-file / directory + partition pruning.
6. ORC (a separate reader, same surfaces), then Iceberg / Delta (manifest and
   snapshot metadata over Parquet, adding time-travel and manifest-level pruning).

## Open decisions for review

- Confirm building **both** surfaces (this document assumes yes).
- File access scope: DECIDED (2026-07-23) -- local filesystem first. Object storage
  (S3 and S3-compatible such as MinIO, plus GCS/Azure) is a future todo behind the
  same path/URL option: an `s3://bucket/key` path resolves through an object-store
  reader while `/path/file.parquet` reads the local FS. The scan core is unchanged
  either way because both just hand it bytes.
- Whether the function's column list is always caller-supplied (`AS (...)`) or we
  also provide a fixed-shape `read_parquet(path, columns jsonb)` convenience.
  Recommend caller-supplied `AS`, matching every other record-returning function,
  plus `parquet_schema()` to generate it.
