# Features

This document lists what pgColumnar provides. For how to use each capability see
the [user guide](user-guide.md) and [administration](administration.md); for
settings see the [configuration reference](configuration.md); for constraints see
[limitations](limitations.md).

## Storage and format

- Column-oriented storage in the relation's main fork, so the buffer manager,
  WAL, and page checksums apply. Data is stored in the native format, PGCN v1,
  specified in
  [../design/NATIVE_FORMAT_AND_INTERFACE_SPEC.md](../design/NATIVE_FORMAT_AND_INTERFACE_SPEC.md).
- Rows are grouped into row groups (the write unit). Within a row group each
  column is stored and compressed as its own chunk, and a chunk's values are
  encoded in fixed-size vectors. Zone maps hold each chunk's and each vector's
  minimum and maximum for skipping.

## Encodings and compression

- Type-aware value encodings applied per chunk before compression: run-length
  (RLE), frame-of-reference with bit-packing (FOR), delta, delta-of-delta,
  Gorilla XOR for floats, and a dictionary for low-cardinality columns including
  text. Each chunk picks the encoding that shrinks it most, then the block codec
  runs on the encoded stream.
- Block compression with four codecs: `none`, `pglz`, `lz4`, and `zstd` with a
  level. Each column chunk is compressed independently, and a chunk that does not
  shrink is stored uncompressed.

## Scan and execution

- Column projection: a scan decodes only the columns the query references.
- Chunk-group skipping: a per-chunk minimum and maximum skip list lets a filtered
  scan skip chunk groups that cannot match a pushed-down `column op const`
  qualifier. A per-chunk bloom filter additionally skips groups on an equality
  probe whose value is provably absent, for hashable, non-collatable columns such
  as ids and uuids. The executor always re-applies the full qualifier, so skipping
  never changes results.
- Vectorized aggregate: an ungrouped `count`, `sum`, `avg`, `min`, or `max` over
  a supported column type is answered from the zone-map metadata, or by a
  column-at-a-time fold over the decoded values when the group has deletes,
  without the per-tuple executor path.
- `count(*)` with no filter is answered from catalog metadata without scanning.
- Parallel scan across a table's row groups.
- Read stream prefetch of block reads on PostgreSQL 17 and later
  (`pgcolumnar.enable_read_stream`).

The vectorized aggregate and skipping change how a result is computed, never the
result. See [limitations](limitations.md) for the exact aggregate and type
coverage.

## Indexes and index-only scans

- `CREATE INDEX` builds btree and hash indexes over a columnar table. Every row is
  assigned a stable row number and synthetic item pointer at insert time, so
  ordinary index scans fetch rows by item pointer.
- Index-only scans: a columnar visibility-map fork records which chunk groups are
  all-visible. Lazy `VACUUM` sets the bit for a group whose inserting transaction
  precedes the oldest snapshot horizon and that has no deletes; any write clears
  the bit, and both are WAL-logged. A covering index query answers from the index
  tuple for all-visible groups and falls back to the snapshot-checked row fetch
  otherwise, so an index-only answer never returns a row not visible to the
  snapshot. On by default (`pgcolumnar.enable_index_only_scan`).

## Projections

- Multiple projections (C-Store model): `pgcolumnar.add_projection(table, name,
  columns, sort_key)` declares an extra physical copy of a column subset, stored
  in its own sort order and sharing the table's row identity. Every insert fans
  out to each projection. A projection stored sorted has tight per-chunk minimum
  and maximum ranges.
- The planner scans a projection instead of the base table when it covers the
  query's columns and its leading sort column is restricted. `EXPLAIN` shows
  `Columnar Projection: <name>`. Deletes and MVCC visibility come from the base,
  and `pgcolumnar.vacuum` keeps projections aligned.
  `pgcolumnar.drop_projection(table, name)` frees one. On by default
  (`pgcolumnar.enable_projection_scan`).

## Transactions, MVCC, and DML

- Reads see the transaction's own inserts and deletes while staying isolated from
  other transactions. Deletes and the old side of updates are marked in a row mask
  without rewriting row groups. Pending work is discarded on transaction and
  savepoint rollback, with correct attribution across `ROLLBACK TO`.
- Unique and primary-key constraints are enforced on insert and at index build
  time. NOT NULL and CHECK constraints are enforced through the insert path.
- Concurrent inserts of the same unique key are serialized so the conflict is
  always caught, controlled by `pgcolumnar.enable_unique_insert_lock`. See
  [limitations](limitations.md) for the exact behavior.

## Schema changes

- `ALTER TABLE ... ADD COLUMN` on a populated table without a rewrite: a row group
  written before the column existed carries no chunk for it, and the reader
  produces the column's missing value (NULL, or the constant default the column
  was added with), matching heap fast-default behavior.
- `pgcolumnar.alter_table_set_access_method(table, method)` converts a table to or
  from columnar storage. See [limitations](limitations.md) for the PostgreSQL 13
  and 14 behavior.

## Maintenance

- `pgcolumnar.vacuum(table)` rewrites a table's live rows into full row groups,
  combining small row groups, reclaiming deleted-row space, and rebuilding indexes.
  `pgcolumnar.vacuum_full(schema)` does the same across a schema.
- `pgcolumnar.vacuum_sorted(table, col [, col ...])` rewrites a table stored sorted
  on the given columns, ascending with nulls last. A sorted key gives tight,
  non-overlapping per-chunk ranges, so range predicates and ordered scans skip
  more chunk groups, and the sort key compresses better under RLE and delta
  encodings. It is a one-time reorder, like `CLUSTER`: rows inserted afterward
  append in insert order until the next call.
- `pgcolumnar.stats(table)` reports per-row-group row counts, deleted-row counts,
  chunk counts, and byte sizes.

## Interoperability

- Export to Arrow and Parquet: `pgcolumnar.export_arrow(table, path)` and
  `pgcolumnar.export_parquet(table, path)`, both without a libarrow or libparquet
  dependency.
- Import from Arrow and Parquet: `pgcolumnar.import_arrow(table, path)` and
  `pgcolumnar.import_parquet(table, path)` into an existing target table. The
  Parquet reader parses Thrift metadata, decompresses uncompressed, Snappy, GZIP,
  ZSTD, and LZ4_RAW pages, and decodes PLAIN and dictionary encodings from
  data-page versions 1 and 2.
- Both directions cover scalar types, one-dimensional arrays, and composite types
  (Arrow List and Struct, Parquet LIST and group), with nulls at every level. The
  functions require superuser and run on little-endian hosts. See the
  [SQL reference](sql-reference.md#import-and-export) and the
  [type-coverage table](limitations.md#import-and-export-type-coverage).

## Reading external Parquet in place

- `pgcolumnar.read_parquet(path) AS t(...)` reads a server-side Parquet file's
  rows without importing them, and `pgcolumnar.parquet_schema(path)` reports its
  leaf columns and the PostgreSQL type each maps to.
- The `pgcolumnar_parquet` foreign-data wrapper exposes a Parquet file as a
  foreign table: `CREATE FOREIGN TABLE ... SERVER ... OPTIONS (path '...')`.
- A `path` that is a directory reads every `*.parquet` file inside it as one
  relation, and a glob pattern expands the same way, in a deterministic sorted
  order.
- The foreign-table scan pushes work down: row groups whose min/max statistics
  exclude the query's predicate are skipped, and only the columns the query
  references are decoded. `EXPLAIN ANALYZE` reports the row groups read and
  skipped, the columns read, and the number of files.
- uuid and numeric columns are read from their Parquet representations, and the
  reader handles millisecond, microsecond, and nanosecond time units.
