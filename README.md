# pgColumnar

pgColumnar is a column-oriented storage extension for PostgreSQL, implemented as
a table access method. A table created `USING columnar` stores its data by
column, with per-column compression, chunk-group skipping, and a vectorized scan
and aggregate path. It is aimed at analytic workloads: large scans, aggregates,
and column projections over append-mostly data.

pgColumnar is an independent implementation. It is not derived from the source
of any other columnar project. It is built from a functional specification of
the on-disk format and SQL interface, recorded in
[design/FORMAT_AND_INTERFACE_SPEC.md](design/FORMAT_AND_INTERFACE_SPEC.md). The
implementation stays independent by the clean-room method described in
[PROVENANCE.md](PROVENANCE.md).

Licensed under the MIT License. See [LICENSE](LICENSE).

## Supported PostgreSQL versions

pgColumnar builds from one source tree on PostgreSQL 13, 14, 15, 16, 17, 18, and
19. Version differences are handled in `src/columnar_compat.h` and a small
number of `PG_VERSION_NUM` guards, and the Makefile selects the C standard per
major automatically. Every test suite passes on all seven majors (see Testing).

## Building and installing

pgColumnar builds with PGXS. Point it at the `pg_config` of the target server:

    make PG_CONFIG=/path/to/pg_config
    make install PG_CONFIG=/path/to/pg_config

### Dependencies

The `lz4` and `zstd` codecs are linked when the system development libraries
(`liblz4`, `libzstd`) are found with `pkg-config`. When a library is absent the
codec is compiled out and a request for it falls back to a codec that is present.
The `pglz` codec (built into PostgreSQL) and `none` are always available. No
other external dependency is required.

### Loading the extension

For full functionality, add the library to `shared_preload_libraries` so the
drop-time metadata cleanup hook is installed in every backend, as is standard
for a table access method extension:

    shared_preload_libraries = 'columnar'

Then, in a database:

    CREATE EXTENSION columnar;

## Quickstart

    CREATE TABLE events (id bigint, ts timestamptz, kind int, payload text)
      USING columnar;

    INSERT INTO events
      SELECT g, now(), g % 8, 'p' || g
      FROM generate_series(1, 1000000) g;

    SELECT count(*), avg(kind) FROM events WHERE kind = 3;

Per-table options override the instance defaults for a table's later writes:

    -- set the codec and chunk-group size for future writes to this table
    SELECT columnar.alter_columnar_table_set('events',
             compression => 'zstd', compression_level => 6,
             chunk_group_row_limit => 20000);

    -- clear an override back to the instance default
    SELECT columnar.alter_columnar_table_reset('events', compression => true);

    -- compact: rewrite live rows into full stripes, reclaim deleted space
    SELECT columnar.vacuum('events');

    -- per-stripe layout
    SELECT * FROM columnar.stats('events');

Instance-wide behavior is controlled by GUCs, all in the `columnar.` namespace:
`compression` (default `zstd`), `compression_level` (default 3),
`stripe_row_limit` (default 150000), `chunk_group_row_limit` (default 10000),
`enable_qual_pushdown`, `enable_custom_scan`, `enable_vectorization` (default
on), `enable_column_cache` (default off), and `column_cache_size` (megabytes).

## Features

- Column-oriented storage in the relation's main fork, so the buffer manager,
  WAL, and page checksums apply. The on-disk format is version 2.0, specified in
  [design/FORMAT_AND_INTERFACE_SPEC.md](design/FORMAT_AND_INTERFACE_SPEC.md).
- Per-chunk compression with four codecs: `none`, `pglz`, `lz4`, and `zstd`
  with a compression level. Each column chunk is compressed independently, and a
  chunk that does not shrink is stored uncompressed.
- Column projection: a scan decodes only the columns the query references.
- Chunk-group skipping: a per-chunk min/max skip list lets a filtered scan skip
  chunk groups that cannot match a pushed-down `column op const` qualifier. The
  executor always re-applies the full qualifier, so skipping never changes
  results.
- Indexes: `CREATE INDEX` builds btree and hash indexes over a columnar table.
  Every row is assigned a stable row number and synthetic item pointer at insert
  time, so ordinary index scans fetch rows by item pointer. Index-only scans are
  never chosen (there is no visibility map).
- Constraints: unique and primary-key constraints are enforced on insert and at
  index build time; NOT NULL and CHECK constraints are enforced through the
  insert path.
- Transactions and MVCC: reads see the transaction's own inserts and deletes
  (read-your-writes) while staying isolated from other transactions. Deletes and
  the old side of updates are marked in a row mask without rewriting stripes.
  Pending work is discarded on transaction and savepoint rollback with correct
  attribution across `ROLLBACK TO`.
- Vectorized execution: a batch reader hands back one decoded chunk group at a
  time as flat per-column arrays. A column-at-a-time filter builds a selection
  vector from simple qualifiers, and a vectorized aggregate computes `count`,
  `sum`, `avg`, `min`, and `max` directly over the decoded arrays. Vectorization
  is on by default and never changes a result; it only changes how the result is
  computed. See Limitations for the exact aggregate and type coverage.
- Compaction: `columnar.vacuum(table)` rewrites a table's live rows into full
  stripes, combining small stripes and physically reclaiming deleted-row space,
  and rebuilds indexes. `columnar.vacuum_full(schema)` does the same across a
  schema.
- `ALTER TABLE ... ADD COLUMN` on a populated table without a rewrite: a stripe
  written before the column existed carries no chunk for it, and the reader
  produces the column's missing value (NULL, or the constant default the column
  was added with), matching heap's fast-default behavior.
- Heap to columnar conversion: `columnar.alter_table_set_access_method(table,
  method)` rewrites a table through the target access method.

## Benchmarks

`bench/run_bench.sh` builds and installs the extension into a throwaway cluster,
loads one identical dataset into a heap table and into columnar tables, and
reports on-disk size and query latency. It also shows the effect of vectorized
execution and of the compression codec. Run it against a non-assert build:

    bench/run_bench.sh /path/to/pg_config

The numbers below are from PostgreSQL 17.10 (non-assert), 6,000,000 rows, an
8-column table, median of 5 timed runs after a warm-up. They are one run on one
machine and are meant to show the shape of the tradeoff, not a precise score.

On-disk size (total relation size, including indexes):

    heap                707 MB
    columnar (none)     462 MB
    columnar (zstd)     378 MB

Table-only size (excluding indexes), where zstd compresses the columnar data:

    heap                579 MB
    columnar (none)     462 MB
    columnar (zstd)      96 MB

Query latency, heap vs columnar (zstd), median milliseconds:

    query                                    heap_ms  columnar_ms  heap/col
    count(*) full table                       212.5        92.0      2.31
    sum/avg over one int column               277.8        62.8      4.43
    filtered agg, min/max-skippable range     223.5        20.5     10.92
    point lookup by indexed id                  0.01      373.1      0.00
    projection: 3 of 8 cols, 1% filter        217.2        22.5      9.64

Vectorization on vs off (columnar zstd, median milliseconds):

    query                    on_ms   off_ms   speedup
    sum/avg over int          60.5    303.8      5.02
    filtered agg (range)      17.9     23.1      1.29

Compression none vs zstd (columnar):

    total size          462 MB  vs   96 MB     (4.8x smaller with zstd)
    sum/avg scan ms      97.6   vs   64.4       (zstd faster: less IO to decode)
    count(*) ms          99.3   vs   91.4

Reading of the results. Columnar wins clearly on the analytic shapes: full scans
and aggregates (2x to 4x here), filtered aggregates that the min/max skip lists
can prune (about 11x here), and column projections from a wide table (about 10x
here), plus a large size reduction with zstd. Vectorization gives a further
large speedup on aggregates that stay on the vectorized path. Heap wins decisively
on the single-row point lookup: an indexed lookup on heap is about 0.01 ms, while
the columnar index fetch must locate and decode the chunk group that holds the
row, which was about 373 ms here. Columnar is the wrong choice for point-lookup
and narrow OLTP access patterns and for write-heavy workloads; it is the right
choice for scan and aggregate heavy analytic queries over wide, append-mostly
tables. DuckDB is present in some environments and `bench/run_bench.sh` can add a
DuckDB comparison with `BENCH_DUCKDB=1`; it is off by default and was not part of
this run.

## Limitations

- Point lookups and narrow OLTP access are slow relative to heap. A single-row
  fetch by item pointer must read and decode the chunk group that contains the
  row. Columnar is intended for scans and aggregates, not point access.
- Standard `VACUUM FULL` and `CLUSTER` are not supported on a columnar table
  (the copy-for-cluster callback raises an error). Use `columnar.vacuum` or
  `columnar.vacuum_full` for compaction instead.
- `columnar.vacuum` always rewrites the whole relation into full stripes. It
  accepts a `stripe_count` argument for interface compatibility but ignores it
  and performs the full rewrite, which is the strongest form of the compaction
  contract. Because it renumbers rows, it rebuilds the table's indexes.
- Index-only scans are never chosen for a columnar table, because there is no
  visibility map. Ordinary index scans and sequential (custom) scans are used.
- Concurrent deletes or updates to rows in the same chunk group serialize on
  that chunk group's row-mask entry. Each delete mark is applied as an upsert of
  the chunk group's shared mask, guarded by a transaction-scoped chunk-group
  lock: a second writer to the same chunk group waits for the first to commit,
  then re-reads the committed mask and merges its bits in, so both sets of delete
  marks survive. Deletes and updates to different chunk groups do not contend and
  proceed concurrently. The cost is per-chunk-group serialization of the mask
  write for the brief window it is held.
- Two concurrent transactions inserting the same unique key are serialized so
  the conflict is always caught (issue #5). Before a freshly inserted row is
  handed to the executor's uniqueness check, the table access method takes a
  transaction-scoped advisory lock keyed by the row's unique key value(s); a
  second inserter of an equal key waits for the first to commit (and therefore
  flush its row), at which point the ordinary btree check sees the committed
  duplicate and raises `unique_violation`. Equal keys always map to the same
  lock: each key column is hashed with its type's default hash function, which is
  consistent with the index's equality (so `numeric` `1.0` and `1.00`, `citext`
  case differences, and collation-equal text serialize correctly). Keys are
  hashed into a bounded number of buckets per index (`columnar.unique_lock_buckets`,
  default 128) so a bulk load holds at most that many locks per unique index;
  unrelated keys that fall in the same bucket are over-serialized, never
  under-serialized. Unique, immediate, valid indexes are covered, including
  multi-column, partial (locked only when the row satisfies the predicate), and
  expression indexes; `NULLS DISTINCT` keys containing a NULL are not locked (they
  cannot conflict) while `NULLS NOT DISTINCT` (PostgreSQL 15+) keys are. An index
  whose operator class cannot be shown to match its key type's default equality,
  or whose key type has no hash support, falls back to a single coarse per-index
  lock (correct, but serializes all inserts to that index). The serialization can
  be turned off with `columnar.enable_unique_insert_lock = off`, which restores
  the prior racy behavior. Because per-key locks are taken in row arrival order, a
  genuine same-key conflict between two transactions can occasionally surface as a
  deadlock abort rather than a `unique_violation`; both outcomes reject the
  duplicate.
- Stale index entries left by deletes and updates are filtered on fetch and
  reclaimed by `REINDEX`, not removed opportunistically.
- `CREATE INDEX CONCURRENTLY` (the concurrent validate path) and partial
  block-range index builds are not supported.
- The vectorized aggregate path covers the single-relation, ungrouped
  `SELECT agg(col) FROM t [WHERE ...]` shape only, and only when every aggregate,
  column type, and filter clause is supported: `count` (including `count(*)` and
  `count(col)`), `sum` and `avg` over `smallint` and `integer` columns, and
  `min` and `max` over any type with a default ordering, with `WHERE` clauses
  that are conjunctions of simple `column op const` comparisons. Anything else
  (`sum`/`avg` over `bigint`, `numeric`, or floating point; ordered-set and
  string aggregates; `DISTINCT`-qualified aggregates; `GROUP BY`; `HAVING`;
  non-simple filters; joins; whole-row or system column references) falls back to
  the scalar plan and stays correct.
- Chunk-group skipping from a pushed-down filter is applied only when the
  comparison's collation matches the column's own collation (the collation the
  stored min/max were ordered under). A differently collated comparison is still
  applied as a filter but does not drive skipping, so results never depend on
  whether the filter was pushed down.
- Row numbers are reserved a whole stripe at a time, so a stripe flushed with
  fewer than `stripe_row_limit` rows leaves a gap in the row-number space. This
  is harmless: row numbers need only be unique and stable.
- A bulk `UPDATE` re-fetches each old row by item pointer to fill unchanged
  columns, which is O(rows x stripe) and is not yet optimized.
- On PostgreSQL 13 and 14 there is no `ALTER TABLE ... SET ACCESS METHOD`, so
  `columnar.alter_table_set_access_method` falls back to building a new table,
  copying, and swapping names. This preserves columns, defaults, constraints, and
  indexes, but not the original relation's OID or its dependent objects.
  PostgreSQL 15 and later use the in-place `ALTER TABLE ... SET ACCESS METHOD`.

## Architecture

[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) is a module-by-module map of the
source (storage, metadata, compression, reader, writer, row mask, table access
method, custom scan, vector, cache, vacuum, and the version compatibility
header) for developers who want to navigate or extend the code.

## Testing

The test suite builds and installs the extension, starts a throwaway cluster,
exercises the access method, and checks results. Each script takes a `pg_config`
and is self-contained:

    test/smoke.sh  /path/to/pg_config   # create, insert, scan, drop
    test/phase2.sh /path/to/pg_config   # compression, projection, min/max skip, filter
    test/phase3.sh /path/to/pg_config   # delete, update, MVCC, savepoints, temp tables
    test/phase4.sh /path/to/pg_config   # btree/hash indexes, constraints, conversion
    test/phase5.sh /path/to/pg_config   # custom scan, pushdown, options, vacuum
    test/phase6.sh /path/to/pg_config   # vectorized scan and aggregates, column cache
    test/audit.sh  /path/to/pg_config   # regression tests for audited defects
    test/concurrency.sh /path/to/pg_config  # concurrent same-chunk-group deletes (issue #4)
    test/unique_conc.sh /path/to/pg_config  # concurrent same-unique-key inserts (issue #5)

To build and run every suite across a set of PostgreSQL majors in one pass, each
in its own fresh build directory, pass their `pg_config` paths to the matrix
helper. With no arguments it uses a default list of PostgreSQL 13 through 19:

    test/run_all_versions.sh /usr/local/pg13/bin/pg_config ... /usr/local/pg19/bin/pg_config

All suites pass on PostgreSQL 13 through 19.
