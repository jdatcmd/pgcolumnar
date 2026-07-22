# pgColumnar documentation

pgColumnar is a column-oriented storage extension for PostgreSQL, implemented as
a table access method. A table created `USING pgcolumnar` stores its data by
column, with per-column compression, chunk-group skipping, and a vectorized scan
and aggregate path. It targets analytic workloads: large scans, aggregates, and
column projections over append-mostly data.

pgColumnar builds from one source tree on PostgreSQL 13 through 19. It is
licensed under the MIT License.

## Where to start

| If you want to | Read |
| --- | --- |
| See what pgColumnar provides | [Features](features.md) |
| Install the extension and load it into a server | [Installation](installation.md) |
| Create columnar tables, load data, and query them | [User guide](user-guide.md) |
| Operate columnar tables in production | [Administration](administration.md) |
| Look up a setting and its default | [Configuration reference](configuration.md) |
| Look up a `pgcolumnar.*` function | [SQL reference](sql-reference.md) |
| Check type coverage and known constraints | [Limitations and compatibility](limitations.md) |
| See size and latency numbers | [Benchmarks](benchmarks.md) |
| Run the test suite | [Testing](testing.md) |

## When to use columnar storage

A columnar table stores each column separately and compresses it. Reads that
touch a subset of columns and scan many rows benefit, because only the requested
columns are read and decompressed, and per-chunk minimum and maximum values let
the scan skip groups of rows that cannot match a filter.

Use pgColumnar for:

- Fact tables and event logs that are appended to and read with aggregates or
  wide scans.
- Queries that select a few columns from a table with many columns.
- Data that compresses well and is queried more often than it is updated.

Row storage (the default heap) remains the better choice for high-rate single
row updates and deletes, and for point lookups that return whole rows.
pgColumnar supports updates, deletes, and indexes, but its storage layout is
built for append-mostly data. See [Limitations and compatibility](limitations.md).

## How it fits together

A columnar table is an ordinary PostgreSQL relation. It works with transactions,
WAL, replication, indexes, `COPY`, and `pg_dump`. The extension adds:

- A table access method named `pgcolumnar`. New tables are written in the native
  on-disk format, PGCN v1.
- A set of catalog tables and functions in the `pgcolumnar` schema.
- Planner and executor paths for columnar scans, aggregates, index-only scans,
  and projections, controlled by settings under the `pgcolumnar.` prefix.

## Design and internals

The documents above are for users and administrators. The design and format
specifications are separate:

- [../design/FORMAT_AND_INTERFACE_SPEC.md](../design/FORMAT_AND_INTERFACE_SPEC.md):
  on-disk format and SQL interface specification.
- [ARCHITECTURE.md](ARCHITECTURE.md): source layout and how the pieces connect.
- [../design/ROADMAP.md](../design/ROADMAP.md): completed work and remaining items.
- [../PROVENANCE.md](../PROVENANCE.md): clean-room implementation method.
