# pgColumnar

**Release: 1.0-dev** (pre-release; on-disk format 2.2). The `VERSION` file is the
source of truth for the release marker; a `-prod` build will be cut once the
remaining gap work lands and the full matrix stays green.

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
on), `enable_column_cache` (default off), `column_cache_size` (megabytes),
`enable_index_only_scan` (default on), and `enable_projection_scan` (default on).

## Features

- Column-oriented storage in the relation's main fork, so the buffer manager,
  WAL, and page checksums apply. The on-disk format is version 2.2, specified in
  [design/FORMAT_AND_INTERFACE_SPEC.md](design/FORMAT_AND_INTERFACE_SPEC.md);
  2.0 and 2.1 files still read.
- Lightweight, type-aware value encodings applied per chunk before compression:
  run-length (RLE), frame-of-reference with bit-packing (FOR), delta, delta-of-
  delta, Gorilla XOR for floats, and a dictionary for low-cardinality columns
  (including text). Each chunk picks whichever encoding shrinks it most, then the
  block codec runs on the encoded stream. 2.0 files still read.
- Per-chunk block compression with four codecs: `none`, `pglz`, `lz4`, and
  `zstd` with a compression level. Each column chunk is compressed independently,
  and a chunk that does not shrink is stored uncompressed.
- Column projection: a scan decodes only the columns the query references.
- Chunk-group skipping: a per-chunk min/max skip list lets a filtered scan skip
  chunk groups that cannot match a pushed-down `column op const` qualifier, and a
  per-chunk bloom filter additionally skips groups on an equality probe whose
  value is provably absent (for hashable, non-collatable columns such as ids and
  uuids). The executor always re-applies the full qualifier, so skipping never
  changes results.
- Indexes: `CREATE INDEX` builds btree and hash indexes over a columnar table.
  Every row is assigned a stable row number and synthetic item pointer at insert
  time, so ordinary index scans fetch rows by item pointer.
- Index-only scans: a columnar visibility-map fork records which chunk groups are
  all-visible. Lazy `VACUUM` sets the bits for groups whose inserting transaction
  precedes the oldest snapshot horizon and that have no deletes; any write clears
  them (WAL-logged). A covering index query then answers from the index tuple for
  all-visible groups and falls back to the snapshot-checked row fetch otherwise,
  so an index-only answer never returns a row not visible to the snapshot. On by
  default (`enable_index_only_scan`).
- Multiple projections (C-Store): `columnar.add_projection(table, name, columns,
  sort_key)` declares an extra physical copy of a column subset, stored in its own
  sort order and sharing the table's row identity. Every insert fans out to each
  projection; a projection is stored sorted so its per-chunk min/max ranges are
  tight. The planner scans a projection instead of the base when it covers the
  query's columns and its leading sort column is restricted (EXPLAIN shows
  `Columnar Projection: <name>`), pruning chunk groups by the projection's
  min/max; deletes and MVCC visibility come from the base, and `columnar.vacuum`
  keeps projections aligned. `columnar.drop_projection(table, name)` frees one.
  On by default (`enable_projection_scan`); format 2.2.
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
  vector from simple qualifiers using typed, branch-free comparison loops for
  integer, float, and date/time types (with an operator-function fallback), and a
  vectorized aggregate computes `count`, `sum`, `avg`, `min`, and `max`. With no
  predicates it folds each column run-at-a-time over the value stream (so a value
  repeated N times costs one operation, not N); otherwise it evaluates per row.
  A scan with a filter uses late materialization: it decodes the predicate
  columns first and decodes the remaining output columns only for chunk groups
  that have surviving rows. Vectorization is on by default and never changes a
  result; it only changes how the result is computed. See Limitations for the
  exact aggregate and type coverage.
- Compaction: `columnar.vacuum(table)` rewrites a table's live rows into full
  stripes, combining small stripes and physically reclaiming deleted-row space,
  and rebuilds indexes. `columnar.vacuum_full(schema)` does the same across a
  schema.
- Sorted storage: `columnar.vacuum_sorted(table, col [, col ...])` rewrites a
  table stored sorted on the given columns (ascending, nulls last). A sorted key
  gives per-chunk-group min/max ranges that are tight and non-overlapping, so
  range predicates and ordered scans skip more chunk groups; the sort key also
  compresses better under RLE and delta encodings. It is a one-time reorder, like
  `CLUSTER`: rows inserted afterward append in insert order until the next call.
  Results are unchanged.
- `ALTER TABLE ... ADD COLUMN` on a populated table without a rewrite: a stripe
  written before the column existed carries no chunk for it, and the reader
  produces the column's missing value (NULL, or the constant default the column
  was added with), matching heap's fast-default behavior.
- Heap to columnar conversion: `columnar.alter_table_set_access_method(table,
  method)` rewrites a table through the target access method.
- Export to Arrow and Parquet: `columnar.export_arrow(table, path)` writes an
  Arrow IPC stream file and `columnar.export_parquet(table, path)` writes a
  Parquet file, both without a libarrow or libparquet dependency. Scalar column
  types (int2/int4/int8, float4/float8, bool, text/varchar, bytea, date, time,
  timestamp[tz], uuid, numeric, json) plus **1-D arrays** (Arrow List / Parquet
  LIST) and **composite types** (Arrow Struct / Parquet group) are supported, with
  nulls (including null elements and null fields). Both require superuser and
  return the number of rows written.
- Import from Arrow and Parquet: `columnar.import_arrow(table, path)` and
  `columnar.import_parquet(table, path)` insert a file's rows into an existing
  target table (its column types define what is expected). The Parquet reader is
  self-contained — Thrift metadata, Snappy decompression, PLAIN and dictionary
  encodings, and both data-page v1 and v2 (what pyarrow writes by default) — with
  no libparquet dependency. Both readers reconstruct **1-D arrays** and
  **composite types** in addition to scalars: Arrow from its List/Struct buffers,
  Parquet from the Dremel repetition/definition levels (LIST → array, group →
  composite), including null arrays/elements/fields and empty arrays. Superuser
  only.

## Benchmarks

`bench/run_bench.sh` builds and installs the extension into a throwaway cluster,
loads one identical dataset into a heap table and into columnar tables, and
reports on-disk size and query latency. It also shows the effect of vectorized
execution and of the compression codec. Run it against a non-assert build:

    bench/run_bench.sh /path/to/pg_config

The numbers below are from PostgreSQL 17.10 (non-assert), 3,000,000 rows, an
8-column table, median of 3 timed runs after a warm-up, with the format 2.1
encoding layer active. They are one run on one machine and are meant to show the
shape of the tradeoff, not a precise score.

On-disk size (total relation size, including indexes):

    heap                354 MB
    columnar (zstd)      88 MB
    columnar (none)      40 MB

Table-only size (excluding indexes):

    heap                289 MB
    columnar (none)      40 MB     (lightweight encodings, no block codec)
    columnar (zstd)      24 MB     (encodings + zstd)

The `columnar (none)` line has no block compression, so its 40 MB versus heap's
289 MB is the lightweight encoding layer alone (about 7x); zstd on top brings the
table to 24 MB (about 12x smaller than the heap table).

Query latency, heap vs columnar (zstd), median milliseconds:

    query                                    heap_ms  columnar_ms  heap/col
    count(*) full table                        84.8        0.02   4241.50
    sum/avg over one int column               120.0       69.7       1.72
    filtered agg, min/max-skippable range      88.7       44.8       1.98
    point lookup by indexed id                  0.01       3.44       0.00
    projection: 3 of 8 cols, 1% filter         89.9       26.8       3.35

`count(*)` over the whole table is answered from catalog metadata, so it does not
scan.

Vectorization on vs off (columnar zstd, median milliseconds):

    query                    on_ms   off_ms   speedup
    sum/avg over int          76.4   197.9      2.59
    filtered agg (range)      41.6    44.4      1.07

Compression none vs zstd (columnar table-only): 40 MB vs 24 MB (1.7x smaller),
with scan latency essentially unchanged (the encoded stream is already small).

Sorted projection (`columnar.vacuum_sorted`), narrow range scan on a key that is
not correlated with insert order, median milliseconds:

    before vacuum_sorted    72.9
    after  vacuum_sorted    10.4

Storing the table sorted on the range key lets min/max skipping prune the chunk
groups outside the range (about 7x here).

Export throughput (3,000,000 rows, 5 exportable columns):

    format    ms     file_size   M rows/s
    arrow     403.9  93 MB        7.4
    parquet   399.7  93 MB        7.5

Reading of the results. Columnar wins on the analytic shapes: `count(*)` from
metadata, aggregates (about 1.7x here), filtered aggregates the min/max and bloom
skip pruning can remove, and wide-table projections (about 3.4x), plus a large
size reduction that comes mostly from the encoding layer even before zstd.
Vectorization gives a further speedup on aggregates, and storing a table sorted
on its range key improves skipping further. Heap still wins on a single-row index
fetch. Columnar remains the wrong choice for write-heavy OLTP and the right choice
for scan- and aggregate-heavy analytics over wide, append-mostly tables.
`bench/run_bench.sh` can add a DuckDB comparison with `BENCH_DUCKDB=1`; it is off
by default and was not part of this run.

## Limitations

- Point lookups and narrow OLTP access are slow relative to heap. A single-row
  fetch by item pointer (an index scan) must read and decode the chunk group that
  contains the row. Per-chunk bloom filters do speed up an equality *scan* on a
  hashable, non-collatable column by skipping chunk groups that cannot contain
  the value, but they do not help an index fetch by item pointer. Columnar is
  intended for scans and aggregates, not point access.
- Standard `VACUUM FULL` and `CLUSTER` are not supported on a columnar table
  (the copy-for-cluster callback raises an error). Use `columnar.vacuum` or
  `columnar.vacuum_full` for compaction instead.
- `columnar.vacuum` always rewrites the whole relation into full stripes. It
  accepts a `stripe_count` argument for interface compatibility but ignores it
  and performs the full rewrite, which is the strongest form of the compaction
  contract. Because it renumbers rows, it rebuilds the table's indexes.
- Index-only scans use a columnar visibility-map fork populated by lazy `VACUUM`;
  a group is only reported all-visible once its inserting transaction precedes the
  oldest snapshot horizon and it has no deletes, and any later write clears the
  bit, so a not-all-visible block always falls back to the snapshot-checked fetch.
  Autovacuum (or an explicit `VACUUM`) is what makes recently loaded data
  eligible; before that the scan simply fetches. Turn off with
  `columnar.enable_index_only_scan = off`.
- Multiple projections are additional sorted copies, so they add write and storage
  cost proportional to the number of projections and are rebuilt by
  `columnar.vacuum`. The planner uses a projection only when it covers every
  referenced column (no system columns / whole-row) and its leading sort column is
  restricted; other queries scan the base. A projection added to a populated table
  is back-filled under `ShareLock` (blocks concurrent writes for the build, like
  non-concurrent `CREATE INDEX`). Turn off projection scans with
  `columnar.enable_projection_scan = off`.
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
    test/differential.sh /path/to/pg_config # heap-vs-columnar oracle: types, boundaries, encodings, exec
    test/recovery.sh /path/to/pg_config  # SIGKILL crash recovery and atomicity
    test/fuzz.sh   /path/to/pg_config    # seeded randomized differential
    test/hardening.sh /path/to/pg_config # format 2.0 compatibility and corrupted-input robustness
    test/concurrent_diff.sh /path/to/pg_config # concurrent DML vs a heap oracle
    test/parallel.sh /path/to/pg_config  # parallel scan plan and results vs a heap oracle
    test/sorted_projection.sh /path/to/pg_config # columnar.vacuum_sorted results and skipping
    test/arrow_export.sh /path/to/pg_config # Arrow IPC export read back with pyarrow
    test/parquet_export.sh /path/to/pg_config # Parquet export read back with pyarrow and DuckDB

`test/differential.sh`, `recovery`, `fuzz`, `hardening`, and `concurrent_diff`
share `test/lib.sh`, a heap-vs-columnar differential oracle: a query is run
against a heap mirror and the columnar table and compared as an order-independent
result-set hash, so heap is the correctness oracle. `test/pbt/run.sh` is a
separate, PostgreSQL-independent C property test of the value-stream codecs
(round-trip over randomized and boundary inputs); run it with `test/pbt/run.sh
[seed] [iterations]`.

To build and run every suite across a set of PostgreSQL majors in one pass, each
in its own fresh build directory, pass their `pg_config` paths to the matrix
helper. With no arguments it uses a default list of PostgreSQL 13 through 19:

    test/run_all_versions.sh /usr/local/pg13/bin/pg_config ... /usr/local/pg19/bin/pg_config

All suites pass on PostgreSQL 13 through 19 (PostgreSQL 19 validated against
19beta2; revalidation against the final PostgreSQL 19 release is pending that
release).
