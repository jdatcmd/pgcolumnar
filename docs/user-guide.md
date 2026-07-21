# User guide

This guide covers creating columnar tables, loading data, and querying them. It
assumes the extension is installed and loaded (see [Installation](installation.md)).

## Create a columnar table

Add `USING pgcolumnar` to `CREATE TABLE`:

```sql
CREATE TABLE events (
    id          bigint,
    customer_id int,
    amount      numeric,
    kind        text,
    ts          timestamptz
) USING pgcolumnar;
```

The table behaves like any PostgreSQL table for SQL purposes. It supports
transactions, constraints, indexes, `COPY`, and `pg_dump`.

## Convert an existing table

On PostgreSQL 15 and later:

```sql
ALTER TABLE events SET ACCESS METHOD columnar;
```

On any supported major, including 13 and 14, the extension provides a helper:

```sql
SELECT pgcolumnar.alter_table_set_access_method('events', 'pgcolumnar');
```

On PostgreSQL 13 and 14 the helper rebuilds the table and does not preserve its
OID or dependent objects such as views and foreign keys. See the
[SQL reference](sql-reference.md#pgcolumnaralter_table_set_access_methodt-text-method-text).

To convert back to the default heap, use `heap` as the method.

## Load data

Columnar storage is built for append-mostly data. Rows are buffered and written
in stripes of up to `pgcolumnar.stripe_row_limit` rows. Each transaction writes its
own stripes, so a load's shape affects the result:

- Prefer `COPY` or multi-row `INSERT` over many single-row `INSERT` statements. A
  `COPY` of N rows writes about N divided by `stripe_row_limit` stripes.
- Many small transactions produce many small stripes. If a table was loaded that
  way, run [`pgcolumnar.vacuum`](sql-reference.md#pgcolumnarvacuumtablename-regclass-stripe_count-int-default-0)
  to combine stripes.

```sql
COPY events FROM '/data/events.csv' WITH (FORMAT csv, HEADER);

INSERT INTO events
SELECT g, g % 1000, (random() * 100)::numeric(10,2), 'sale',
       now() - (g || ' seconds')::interval
FROM generate_series(1, 1000000) g;
```

## Query

Queries need no special syntax. The planner adds columnar scan and aggregate
paths for columnar tables. Reads that touch a subset of columns and scan many
rows benefit most.

```sql
-- reads only amount and ts, skips chunk groups outside the time range
SELECT date_trunc('day', ts) AS day, sum(amount)
FROM events
WHERE ts >= '2026-01-01' AND ts < '2026-02-01'
GROUP BY 1
ORDER BY 1;
```

### How the scan is chosen

Read the plan with `EXPLAIN`:

```sql
EXPLAIN (ANALYZE, VERBOSE) SELECT sum(amount) FROM events WHERE customer_id = 42;
```

A columnar scan shows as a custom scan node. The scan uses the following, each
controlled by a setting in the [Configuration reference](configuration.md):

- Chunk-group skipping: per-chunk minimum and maximum values drop groups of rows
  that cannot satisfy a filter.
- Bloom filters: per-chunk filters drop groups for equality filters.
- Vectorized execution and late materialization: filters run first, then only the
  output columns of matching rows are decoded.
- `count(*)` answered from catalog metadata when there is no filter.

### Point lookups and indexes

Create indexes on columnar tables as usual:

```sql
CREATE INDEX ON events (id);
SELECT * FROM events WHERE id = 12345;
```

An index supports point lookups and range scans. When a query's columns are all
in the index and the table's rows are marked all-visible, pgColumnar can answer
from the index alone with an index-only scan, served by its visibility-map fork.
Visibility bits are set by `VACUUM`. See
[Administration](administration.md#index-only-scans).

## Arrays and composite types

Columnar tables store one-dimensional arrays and composite types. These are also
covered by the Arrow and Parquet import and export functions.

```sql
CREATE TYPE addr AS (city text, zip text);

CREATE TABLE people (
    id    int,
    tags  text[],
    home  addr
) USING pgcolumnar;

INSERT INTO people VALUES (1, ARRAY['a','b'], ROW('Portland','97201')::addr);
```

## Next steps

- Operate tables in production: [Administration](administration.md).
- Improve range scans on a scattered key or serve a query from a column subset:
  [projections](administration.md#projections) and
  [`pgcolumnar.vacuum_sorted`](sql-reference.md#pgcolumnarvacuum_sortedtablename-regclass-variadic-sort_columns-namel).
- Move data in and out of the Arrow and Parquet ecosystem:
  [import and export](sql-reference.md#import-and-export).
