# Administration

This guide is for database administrators operating columnar tables. It covers
storage layout, compression, compaction, index-only scans, projections,
monitoring, backup, and security.

## Storage layout

A columnar table is one PostgreSQL relation plus rows in the `pgcolumnar` catalog
tables. Data is organized as follows:

- A **row group** is the unit of write. Each write transaction appends one or
  more row groups of up to `pgcolumnar.stripe_row_limit` rows.
- Within a row group, each column is stored and compressed separately as a
  chunk. A chunk's values are encoded in **vectors** of up to
  `pgcolumnar.chunk_group_row_limit` rows, the unit of the encoding cascade and
  of minimum and maximum skipping.
- Each chunk records its minimum and maximum and an optional bloom filter, and a
  per-vector zone map records the finer minimum and maximum ranges.

Deletes and updates do not rewrite data. They mark rows in a row mask. Space is
reclaimed by compaction (see below).

Inspect the layout with [`pgcolumnar.stats`](sql-reference.md#pgcolumnarstatsrel-regclass).

## Compression

The default codec is `zstd` at level 3. Set the default for new data with
`pgcolumnar.compression` and `pgcolumnar.compression_level`, or per table with
[`pgcolumnar.alter_columnar_table_set`](configuration.md#per-table-storage-options).

| Codec | Notes |
| --- | --- |
| `none` | No compression. Lowest write cost, largest size. |
| `pglz` | Built in, always available. |
| `lz4` | Available when built with `liblz4`. Fast decompression. |
| `zstd` | Available when built with `libzstd`. Higher compression at a given speed than `pglz`; the level trades size against write cost. |

A codec change applies to data written after the change. To apply it to existing
data, rewrite the table with [`pgcolumnar.vacuum`](sql-reference.md#pgcolumnarvacuumtablename-regclass-stripe_count-int-default-0).

## Row-group sizing

`pgcolumnar.chunk_group_row_limit` (default 10000) sets how many rows share one
minimum and maximum in a vector. Smaller vectors skip more precisely on selective
range filters but hold less data per vector. `pgcolumnar.stripe_row_limit` (default
150000) sets the write unit. The defaults suit most workloads. Change them for a
table with `pgcolumnar.alter_columnar_table_set` when a specific access pattern
calls for it, and measure the result.

## Compaction and vacuum

There are two distinct operations, and the difference matters:

- **Standard `VACUUM`** (manual or autovacuum) runs the columnar table's vacuum,
  which sets visibility-map bits used by index-only scans and maintains
  statistics. It does not rewrite data or reclaim space from deleted rows.
- **`pgcolumnar.vacuum`** (a function) rewrites the table, combining row groups and
  reclaiming space held by deleted and updated rows.

Run `pgcolumnar.vacuum` after bulk deletes or updates, or after many small load
transactions have produced many small row groups:

```sql
SELECT pgcolumnar.vacuum('events');
```

To store rows sorted on a column so range filters on it skip more row groups,
use `pgcolumnar.vacuum_sorted`:

```sql
SELECT pgcolumnar.vacuum_sorted('events', 'customer_id');
```

To compact every columnar table in a schema, use `pgcolumnar.vacuum_full`.

Leave autovacuum on. It maintains visibility-map bits and statistics for columnar
tables. Schedule `pgcolumnar.vacuum` separately based on delete and update volume.

## Index-only scans

An index-only scan answers a query from the index without reading the table, when
the index covers the query and the rows are marked all-visible. pgColumnar serves
this through a columnar visibility-map fork:

- `VACUUM` marks a row group all-visible when its inserting transaction is old
  enough and the group has no deletes.
- Any insert, update, or delete clears the bit for the affected group.

Index-only scans are on by default (`pgcolumnar.enable_index_only_scan`). To make a
covering query use one, ensure the table has been vacuumed since its last write.
Check with `EXPLAIN (ANALYZE)`: an index-only scan reports `Heap Fetches: 0`.

## Projections

A projection stores a subset of a table's columns a second time, optionally
sorted on a key. The planner scans a projection instead of the base table when it
covers the query and serves it better, for example a range query on a key that is
scattered in the base table but is the projection's sort key.

Declare a projection:

```sql
SELECT pgcolumnar.add_projection(
    'events', 'events_by_customer',
    columns  => ARRAY['customer_id', 'amount', 'ts'],
    sort_key => ARRAY['customer_id']);
```

Existing rows are back-filled when the projection is added. New inserts write to
the base table and its projections. Projection scans are on by default
(`pgcolumnar.enable_projection_scan`). Drop a projection with
`pgcolumnar.drop_projection`.

A projection adds write cost and storage, because inserts write it too. Add one
for a query pattern that a covering, sorted column subset serves, and measure the
result. Confirm the plan uses it with `EXPLAIN`, which names the chosen projection.

## Monitoring

`pgcolumnar.stats(rel)` reports per-row-group row counts, deleted-row counts, chunk
counts, and byte sizes. Use it to see fragmentation and decide when to compact:

```sql
SELECT count(*)                         AS row_groups,
       sum(rowcount)                    AS rows,
       sum(deletedrows)                 AS deleted,
       round(100.0 * sum(deletedrows)
             / nullif(sum(rowcount), 0), 1) AS pct_deleted,
       pg_size_pretty(sum(datalength))  AS size
FROM pgcolumnar.stats('events');
```

A high deleted-row percentage or a large number of small row groups indicates that
`pgcolumnar.vacuum` would help.

## Concurrent unique inserts

When a columnar table has a unique index, `pgcolumnar.enable_unique_insert_lock`
(on by default) serializes concurrent inserts of the same key with a
transaction-scoped advisory lock, so overlapping same-key inserts conflict
correctly. `pgcolumnar.unique_lock_buckets` (default 128) bounds how many advisory
locks a transaction holds per unique index. Leave the lock on unless you have a
specific reason to change it.

## Column cache

`pgcolumnar.enable_column_cache` (off by default) caches decompressed chunk groups
so they can be reused across reads, sized by `pgcolumnar.column_cache_size` (default
200 MB). Enable it for repeated scans over the same recently read data, and size
the cache to the working set.

## Backup and restore

A columnar table is an ordinary WAL-logged relation.

- **Physical backup** (`pg_basebackup`, file-system snapshots) and **physical
  replication** include columnar tables and their WAL.
- **Logical backup** (`pg_dump`) writes the table definition, including `USING
  columnar`, and its data with `COPY`. Restore requires the `pgcolumnar` extension
  installed and present in `shared_preload_libraries` on the target server.

Install and preload the extension on any server that restores or replicates a
columnar table, because reading the table requires the access method.

## Security

The Arrow and Parquet import and export functions read and write files on the
server host and require superuser. Grant them only to roles you trust with
server-side file access, and control the paths those roles can reach. All other
`pgcolumnar.*` functions run with ordinary table privileges.
