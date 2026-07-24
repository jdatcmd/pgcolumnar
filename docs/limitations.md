# Limitations and compatibility

## PostgreSQL versions

pgColumnar builds from one source tree on PostgreSQL 15, 16, 17, 18, and
19. Every test suite runs on all seven majors.

Two behaviors depend on the major:

- `ALTER TABLE ... SET ACCESS METHOD` exists on PostgreSQL 15 and later. On 13 and
  14, `pgcolumnar.alter_table_set_access_method` builds a new table, copies rows,
  and swaps names. This preserves columns, defaults, constraints, and indexes, but
  not the original relation's OID or its dependent objects such as views and
  foreign keys.
- The read stream prefetch path (`pgcolumnar.enable_read_stream`) is effective on
  PostgreSQL 17 and later. On earlier majors the setting has no effect.

## Host architecture

The Arrow and Parquet import and export functions run on little-endian hosts
only. The rest of the extension runs on any architecture PostgreSQL supports.

## Workload and access patterns

- Columnar storage is built for append-mostly data. Updates and deletes are
  supported, but they mark rows rather than rewriting data, and the space is
  reclaimed only by `pgcolumnar.vacuum`.
- Point lookups are slow relative to heap. A single-row fetch by item pointer must
  read and decode the row group that contains the row. Bloom filters speed up an
  equality scan by skipping row groups, but do not help an index fetch by item
  pointer.
- A bulk `UPDATE` re-fetches each old row by item pointer to fill unchanged
  columns, which is proportional to rows times row group size and is not yet
  optimized.

## Vacuum and compaction

- `VACUUM FULL` and `CLUSTER` are not supported on a columnar table; the
  copy-for-cluster path raises an error. Use `pgcolumnar.vacuum` or
  `pgcolumnar.vacuum_full` instead.
- `pgcolumnar.vacuum` always rewrites the whole relation into full row groups. It
  accepts a `stripe_count` argument for interface compatibility but performs the
  full rewrite. Because it renumbers rows, it rebuilds the table's indexes.
- Row numbers are reserved a whole row group at a time, so a row group flushed
  with fewer than `stripe_row_limit` rows leaves a gap in the row-number space. Row
  numbers need only be unique and stable, so the gap is harmless.

## Index-only scans

An index-only scan uses the columnar visibility-map fork, which lazy `VACUUM`
populates. A row group is reported all-visible only once its inserting
transaction precedes the oldest snapshot horizon and it has no deletes, and any
later write clears the bit. Recently loaded data is served by a snapshot-checked
fetch until autovacuum or an explicit `VACUUM` marks it. Turn the feature off with
`pgcolumnar.enable_index_only_scan = off`.

## Projections

Projections are additional sorted copies, so they add write and storage cost
proportional to the number of projections, and they are rebuilt by
`pgcolumnar.vacuum`. The planner uses a projection only when it covers every
referenced column (no system columns or whole-row references) and its leading sort
column is restricted; other queries scan the base. A projection added to a
populated table is back-filled under `ShareLock`, which blocks concurrent writes
for the build, like non-concurrent `CREATE INDEX`. Turn projection scans off with
`pgcolumnar.enable_projection_scan = off`.

## Concurrency

- Concurrent deletes or updates to rows in the same row group serialize on that
  row group's row-mask entry: a second writer waits for the first to commit,
  re-reads the committed mask, and merges its bits, so both sets of delete marks
  survive. Writes to different row groups proceed concurrently.
- Concurrent inserts of the same unique key are serialized so the conflict is
  always caught. Before a freshly inserted row reaches the uniqueness check, the
  access method takes a transaction-scoped advisory lock keyed by the row's unique
  key. Equal keys hash to the same lock, consistent with the index's equality, so
  `numeric` `1.0` and `1.00`, `citext` case differences, and collation-equal text
  serialize correctly. Keys hash into a bounded number of buckets per index
  (`pgcolumnar.unique_lock_buckets`, default 128); unrelated keys sharing a bucket
  are over-serialized, never under-serialized. Unique, immediate, valid indexes
  are covered, including multi-column, partial, and expression indexes. An index
  whose operator class cannot be matched to its key type's default equality, or
  whose key type has no hash support, falls back to a single per-index lock. A
  genuine same-key conflict can surface as a deadlock abort rather than a
  `unique_violation`; both reject the duplicate. Turn the serialization off with
  `pgcolumnar.enable_unique_insert_lock = off`.

## Indexes

- Stale index entries left by deletes and updates are filtered on fetch and
  reclaimed by `REINDEX`, not removed opportunistically.
- `CREATE INDEX CONCURRENTLY` (the concurrent validate path) and partial
  block-range index builds are not supported.

## Vectorized aggregate coverage

The vectorized aggregate path covers the single-relation, ungrouped
`SELECT agg(col) FROM t [WHERE ...]` shape only, and only when every aggregate,
column type, and filter clause is supported: `count` (including `count(*)` and
`count(col)`), `sum` and `avg` over `smallint` and `integer` columns, and `min`
and `max` over any type with a default ordering, with `WHERE` clauses that are
conjunctions of simple `column op const` comparisons. Anything else (`sum` or
`avg` over `bigint`, `numeric`, or floating point; ordered-set and string
aggregates; `DISTINCT`-qualified aggregates; `GROUP BY`; `HAVING`; non-simple
filters; joins; whole-row or system column references) falls back to the scalar
plan and stays correct.

## Skipping and collation

Chunk-group skipping from a pushed-down filter is applied only when the
comparison's collation matches the column's own collation, the collation the
stored minimum and maximum were ordered under. A differently collated comparison
is still applied as a filter but does not drive skipping, so results never depend
on whether the filter was pushed down.

## Replication and backup

- Physical replication and physical backups (`pg_basebackup`, snapshots) include
  columnar tables, which are WAL-logged relations.
- `pg_dump` and `pg_restore` handle columnar tables. The target server must have
  the extension installed and preloaded.
- Logical decoding reads heap-tuple WAL records. Changes to columnar tables are
  not emitted through logical decoding, so logical replication does not carry
  them. Use physical replication for columnar tables.

## Import and export type coverage

The import and export functions require superuser and run on little-endian hosts.
They support one-dimensional arrays and composite types built from the scalar
types below, with nulls at every level. Multi-dimensional arrays and types not
listed are rejected.

| Type | Arrow export | Parquet export | Arrow import | Parquet import |
| --- | --- | --- | --- | --- |
| `int2`, `int4`, `int8` | yes | yes | yes | yes |
| `float4`, `float8` | yes | yes | yes | yes |
| `bool` | yes | yes | yes | yes |
| `text`, `varchar` | yes | yes | yes | yes |
| `bytea` | yes | yes | yes | yes |
| `date`, `time`, `timestamp`, `timestamptz` | yes | yes | yes | yes |
| `uuid` | yes | yes | yes | yes |
| `numeric` | yes | yes | yes | yes |
| `json`, `jsonb` | yes | yes | yes | no |
| one-dimensional array of the above | yes | yes | yes | yes |
| composite of the above | yes | yes | yes | yes |

`uuid` is imported from a 16-byte fixed-length binary column, and `numeric` from
a DECIMAL column stored as fixed or variable big-endian bytes with precision up
to 38. A DECIMAL backed by an INT32 or INT64 physical column is not yet read.
`json` and `jsonb` can be exported to Parquet and read back with other tools, but
pgColumnar does not currently import them; they are supported end to end through
Arrow.

## Compression codecs

For the native table format, `lz4` and `zstd` are available only when the
extension was built with the corresponding system libraries. When a codec is not
built in, a request for it falls back to a codec that is present. `pglz` and
`none` are always available.

When reading external Parquet files, the reader decodes uncompressed, Snappy,
GZIP, ZSTD, and LZ4_RAW pages. GZIP requires a build with zlib, and ZSTD and
LZ4_RAW require the same libraries as the native codecs; a page whose codec was
not built in fails with a clean decode error. LZO, BROTLI, and the deprecated
Hadoop-framed LZ4 (codec 5, as distinct from LZ4_RAW) are not read.

## Reading external Parquet

The read-in-place surface (`read_parquet`, `parquet_schema`, and the
`pgcolumnar_parquet` foreign-data wrapper) has these limits:

- Reads are superuser only and run on little-endian hosts, as import and export
  do, since they read a server-side path.
- A `path` that is a directory reads the `*.parquet` files directly inside it;
  there is no recursive walk and no Hive-style partition pruning (directory names
  of the form `col=value` are not exposed as columns).
- `parquet_schema` describes the first file of a directory or glob, assuming the
  set is uniform. The read paths still bind every file against the declared
  columns, so a mismatched file raises rather than returning wrong rows.
- A `TIMESTAMP` column with nanosecond precision is advised as `bigint`, which is
  exact; declaring it `timestamp` reads it with the sub-microsecond digits
  truncated.
- Each file is read fully into memory before its rows are produced; there is no
  streaming.
