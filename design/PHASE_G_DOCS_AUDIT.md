# Phase G documentation audit

Reconciles the user and engineering docs against the external-Parquet read
surface, which landed across #98-#101 and the follow-ons #106/#107/#109/#111 and
was never documented. Some existing claims are now false. Style per project
convention: professional, no em-dashes, no unnecessary adjectives.

## What is now true but undocumented or mis-documented

Read surfaces, all superuser, server-side paths:
- `pgcolumnar.read_parquet(path) AS t(...)` -- SRF, reads rows in place.
- `pgcolumnar.parquet_schema(path)` -- reports leaf columns and the PostgreSQL
  type each maps to.
- The `pgcolumnar_parquet` foreign-data wrapper -- `CREATE SERVER` +
  `CREATE FOREIGN TABLE ... OPTIONS (path ...)`.
- Predicate pushdown (row-group skipping via min/max stats) and column projection
  pushdown, both visible in `EXPLAIN ANALYZE` (Row Groups, Row Groups Skipped,
  Columns Read/Total).
- A `path` that is a directory reads every `*.parquet` in it; a glob pattern
  expands; sorted, deterministic.
- Codecs on read: uncompressed, Snappy, GZIP, ZSTD, LZ4_RAW.
- FLBA types on read: uuid and numeric(p,s) via DECIMAL, plus fixed bytea.

## Corrections (currently wrong)

- `docs/limitations.md` type matrix: `uuid` and `numeric` Parquet import are
  marked "no" -- both are "yes" now (#106). `json`/`jsonb` import stays "no".
- `docs/limitations.md` prose "does not currently import those three types from
  Parquet" -- now only json.
- `docs/features.md` "decompresses Snappy" -- now uncompressed, Snappy, GZIP,
  ZSTD, LZ4_RAW.
- `docs/sql-reference.md` import_parquet "handles Snappy compression" -- same
  codec list update.

## Additions

- `docs/sql-reference.md`: new sections for `read_parquet`, `parquet_schema`, and
  the FDW; note directory/glob paths on all read paths; note pushdown in EXPLAIN.
- `docs/features.md`: an "external Parquet" bullet group -- read in place via SRF
  or FDW, directory/glob, predicate and projection pushdown, codec list.
- `docs/user-guide.md`: a worked example of read_parquet and a foreign table over
  a directory, with an EXPLAIN showing skipping.
- `docs/ARCHITECTURE.md`: the shared scan core, the two surfaces, and where
  pushdown sits.
- `CHANGELOG.md`: entries for the read surface and the follow-ons.
- `design/ROADMAP.md`: mark the Parquet read follow-ons done.

## Still-true limitations to state

- No json/jsonb import from Parquet.
- INT32/INT64-backed DECIMAL not read (only FLBA/BYTE_ARRAY DECIMAL); the schema
  does not advertise numeric for them.
- TIMESTAMP_NANOS advises `bigint` (lossless); declaring `timestamp` truncates.
- No Hive-style partition pruning, no recursive directory walk, no streaming
  (each file is read fully into memory), single schema per directory assumed by
  `parquet_schema`.
- LZO, BROTLI, and the deprecated Hadoop-framed LZ4 (codec 5) are not read.
- Reads are superuser-only and little-endian only, as export/import already are.

## Order of work
limitations (fix false claims first) -> features -> sql-reference -> user-guide
-> ARCHITECTURE -> CHANGELOG -> ROADMAP.
