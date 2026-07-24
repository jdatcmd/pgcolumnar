# SQL reference

Every function is in the `pgcolumnar` schema. Types are shown as in the function
signature. For server settings, see [Configuration reference](configuration.md).

## Table management

### pgcolumnar.alter_table_set_access_method(t text, method text)

Converts a table to another access method, for example from the default heap to
`pgcolumnar` or back.

On PostgreSQL 15 and later this runs `ALTER TABLE ... SET ACCESS METHOD`, which
rewrites the table in place and preserves its identity and dependents. On
PostgreSQL 13 and 14, which have no such command, it builds a sibling table with
`LIKE ... INCLUDING ALL`, copies every row through the target method, and swaps
names. On those two majors the conversion does not preserve the original table's
OID or objects that depend on it, such as views and foreign keys.

```sql
SELECT pgcolumnar.alter_table_set_access_method('events', 'pgcolumnar');
```

### pgcolumnar.alter_columnar_table_set(...) and pgcolumnar.alter_columnar_table_reset(...)

Set or reset per-table storage options (row group and vector row limits,
compression codec and level). See
[Configuration reference](configuration.md#per-table-storage-options).

### pgcolumnar.get_storage_id(rel regclass) returns bigint

Returns the internal storage identifier of a columnar table. Used to join the
`pgcolumnar` catalog tables. Most users read [`pgcolumnar.stats`](#pgcolumnarstatsrel-regclass)
instead.

## Maintenance

### pgcolumnar.vacuum(tablename regclass, stripe_count int DEFAULT 0)

Compacts a columnar table by combining small row groups and reclaiming space held
by rows that were deleted or updated. Use it after bulk deletes or updates, or
after many small load transactions have produced many small row groups.

`stripe_count` bounds how many row groups are combined in one call; `0` places no
bound.

```sql
SELECT pgcolumnar.vacuum('events');
```

### pgcolumnar.vacuum_sorted(tablename regclass, VARIADIC sort_columns name[])

Compacts a columnar table and stores its rows sorted ascending on the given
columns. Sorted storage makes per-chunk minimum and maximum values tight and
non-overlapping on the sort columns, so range filters on those columns skip more
chunk groups. Use it for a column whose values are scattered in insertion order
but are often queried by range.

```sql
SELECT pgcolumnar.vacuum_sorted('events', 'customer_id');
```

### pgcolumnar.vacuum_full(schema name DEFAULT 'public', sleep_time real DEFAULT 0.0, stripe_count int DEFAULT 0)

Runs `pgcolumnar.vacuum` on every columnar table in a schema. `sleep_time` is a
pause in seconds between tables. `stripe_count` is passed through to each call.

```sql
SELECT pgcolumnar.vacuum_full('public');
```

### pgcolumnar.stats(rel regclass)

Returns one row per row group, with these columns:

| Column | Type | Meaning |
| --- | --- | --- |
| `stripeid` | bigint | Row group number within the table. |
| `fileoffset` | bigint | Byte offset of the row group in the relation file. |
| `rowcount` | bigint | Rows written into the row group. |
| `deletedrows` | bigint | Rows in the row group marked deleted. |
| `chunkcount` | integer | Vectors in the row group. |
| `datalength` | bigint | On-disk length of the row group in bytes. |

```sql
-- total live rows, deleted rows, and size
SELECT sum(rowcount) AS rows,
       sum(deletedrows) AS deleted,
       pg_size_pretty(sum(datalength)) AS size
FROM pgcolumnar.stats('events');
```

## Projections

A projection is a named subset of a table's columns stored a second time,
optionally sorted on a key. When a projection covers a query and serves it better
than the base table, the planner scans the projection instead. See
[Administration](administration.md#projections).

### pgcolumnar.add_projection(rel regclass, name text, columns text[], sort_key text[] DEFAULT '{}')

Declares a projection on `rel` named `name`, storing `columns`, sorted on
`sort_key`. Existing rows are back-filled when the projection is added.

```sql
SELECT pgcolumnar.add_projection(
    'events', 'events_by_customer',
    columns  => ARRAY['customer_id', 'amount', 'ts'],
    sort_key => ARRAY['customer_id']);
```

### pgcolumnar.drop_projection(rel regclass, name text)

Drops a projection and frees its storage.

```sql
SELECT pgcolumnar.drop_projection('events', 'events_by_customer');
```

### pgcolumnar.read_projection(rel regclass, name text) and pgcolumnar.reconstruct_via_projection(rel regclass, name text)

Return a projection's stored rows as text, for verification. They are for
inspection and testing, not for query use.

## Import and export

These functions read and write Arrow IPC stream files and Parquet files. They
require superuser, because they read and write files on the server host. They run
on little-endian hosts only. They support scalar column types, one-dimensional
arrays, and composite types, with nulls at every level. Multi-dimensional arrays
and unsupported types are rejected. See
[Limitations and compatibility](limitations.md).

### pgcolumnar.export_arrow(rel regclass, path text) returns bigint

Writes the live rows of `rel` to an Arrow IPC stream file at `path`. Returns the
number of rows written.

### pgcolumnar.export_parquet(rel regclass, path text) returns bigint

Writes the live rows of `rel` to a Parquet file at `path`. Returns the number of
rows written.

### pgcolumnar.import_arrow(rel regclass, path text) returns bigint

Inserts the rows of an Arrow IPC stream file at `path` into the existing table
`rel`. The table's column types define what is expected. Returns the number of
rows inserted.

### pgcolumnar.import_parquet(rel regclass, path text) returns bigint

Inserts the rows of a Parquet file at `path` into the existing table `rel`. The
reader handles uncompressed, Snappy, GZIP, ZSTD, and LZ4_RAW pages, PLAIN and
dictionary encodings, and data-page versions 1 and 2. `path` may name a single
file, a directory (every `*.parquet` file inside it is imported), or a glob
pattern. Returns the number of rows inserted.

```sql
-- round-trip a table through Parquet
SELECT pgcolumnar.export_parquet('events', '/tmp/events.parquet');   -- returns row count
CREATE TABLE events_copy (LIKE events) USING pgcolumnar;
SELECT pgcolumnar.import_parquet('events_copy', '/tmp/events.parquet');

-- import an entire directory of Parquet files
SELECT pgcolumnar.import_parquet('events_copy', '/data/events/');
```

## Reading external Parquet

These read a server-side Parquet file in place, without importing it. They
require superuser and run on little-endian hosts. In every case `path` may be a
single file, a directory (all `*.parquet` files inside it, read as one relation
in sorted order), or a glob pattern.

### pgcolumnar.read_parquet(path text) returns setof record

Returns the rows of a Parquet file. The caller supplies a column definition list
that names the output columns and their types; the reader binds it against the
file's leaf columns by position, with the same type-compatibility rules as
import.

The list must cover every leaf column in the file. Declaring a subset is an
error, not a projection: the read stops with a message reporting the file's leaf
count and the count the target expands to. The same rule applies to a foreign
table's column definitions. Projection pushdown decides which of the declared
columns are decoded, which is separate from how many must be declared. Use
`parquet_schema` to generate the full list.

```sql
SELECT * FROM pgcolumnar.read_parquet('/data/events.parquet')
  AS t(id int, ts timestamp, amount numeric(12,2));

-- read a whole directory
SELECT count(*) FROM pgcolumnar.read_parquet('/data/events/')
  AS t(id int, ts timestamp, amount numeric(12,2));
```

### pgcolumnar.parquet_schema(path text) returns table(column_name text, data_type text, nullable bool)

Reports the leaf columns of a Parquet file and the PostgreSQL type each maps to,
without reading the data. Useful for writing the column definition list for
`read_parquet` or a foreign table. For a directory or glob it describes the first
file.

```sql
SELECT * FROM pgcolumnar.parquet_schema('/data/events.parquet');
```

### The pgcolumnar_parquet foreign-data wrapper

Exposes a Parquet file, directory, or glob as a foreign table. The scan pushes
work down: row groups whose min/max statistics exclude the query's predicate are
skipped, and only referenced columns are decoded. Skipping requires a
`column op constant` clause over an integer or floating-point column with a
constant of the same type; [limitations.md](limitations.md) lists the conditions.
A scan that skips nothing still returns correct rows.

```sql
CREATE SERVER pq FOREIGN DATA WRAPPER pgcolumnar_parquet;
CREATE FOREIGN TABLE events (id int, ts timestamp, amount numeric(12,2))
  SERVER pq OPTIONS (path '/data/events/');

SELECT sum(amount) FROM events WHERE ts >= '2026-01-01';

-- EXPLAIN ANALYZE reports Row Groups, Row Groups Skipped, Columns Read,
-- Columns Total, and Files.
EXPLAIN (ANALYZE, COSTS OFF) SELECT id FROM events WHERE ts >= '2026-01-01';
```

## Visibility map inspection

These report the state of the columnar visibility-map fork that serves
index-only scans. They are for diagnostics.

### pgcolumnar.vm_is_visible(rel regclass, blk int)

Returns whether the given block (chunk group) is marked all-visible.

### pgcolumnar.vm_selftest(rel regclass, blk int)

Runs a set and clear self-test against the visibility map for one block.
