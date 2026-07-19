# pgColumnar

A column-oriented storage extension for PostgreSQL, implemented as a table
access method. pgColumnar is an independent implementation. It is not derived
from the source of any other columnar project. It is built from a functional
specification of the on-disk format and SQL interface, recorded in
[design/FORMAT_AND_INTERFACE_SPEC.md](design/FORMAT_AND_INTERFACE_SPEC.md).

Licensed under the MIT License. See [LICENSE](LICENSE).

Status: early construction. See [design/REWRITE_PLAN.md](design/REWRITE_PLAN.md)
for the delivery phases and task list, and [PROVENANCE.md](PROVENANCE.md) for how
the implementation stays independent.

## Building and testing

pgColumnar builds with PGXS against PostgreSQL 13, 14, 15, 16, 17, 18, or 19
from one source tree; version differences are handled by `src/columnar_compat.h`
and a few `PG_VERSION_NUM` guards, and the Makefile picks the right C standard
per major automatically:

    make PG_CONFIG=/path/to/pg_config
    make install PG_CONFIG=/path/to/pg_config

One conversion helper degrades on the oldest two majors: PostgreSQL 13 and 14
have no `ALTER TABLE ... SET ACCESS METHOD`, so on those versions
`columnar.alter_table_set_access_method` rewrites the table into a fresh one and
swaps names (preserving columns, defaults, constraints, and indexes, but not the
original relation's OID or dependent objects). PostgreSQL 15 and later use the
in-place `ALTER TABLE ... SET ACCESS METHOD`.

The `lz4` and `zstd` codecs are linked automatically when the system
development libraries (`liblz4`, `libzstd`) are found with `pkg-config`. When a
library is absent the codec is compiled out and a request for it falls back to a
codec that is present; `pglz` (built into PostgreSQL) and `none` are always
available.

Then, in a database:

    CREATE EXTENSION columnar;
    CREATE TABLE t (a int, b text) USING columnar;

The end-to-end tests build, install, spin up a throwaway cluster, and check
behavior. Phase 1 covers create/insert/scan/drop; phase 2 covers compression,
projection, min/max skip lists, and filtering; phase 3 covers delete, update,
read-your-writes, transaction and savepoint rollback, and temporary tables;
phase 4 covers btree and hash indexes, unique/primary-key and NOT NULL/CHECK
constraints, and heap<->columnar conversion; phase 5 covers the custom scan,
qual and projection pushdown, per-table options, and vacuum; phase 6 covers the
vectorized scan and aggregate fast paths and the decompressed-chunk cache:

    test/smoke.sh  /path/to/pg_config
    test/phase2.sh /path/to/pg_config
    test/phase3.sh /path/to/pg_config
    test/phase4.sh /path/to/pg_config
    test/phase5.sh /path/to/pg_config
    test/phase6.sh /path/to/pg_config

To build and run every suite across a set of PostgreSQL majors in one pass, each
in its own fresh build directory, pass their `pg_config` paths to the matrix
helper (with no arguments it uses a default 13-through-19 list):

    test/run_all_versions.sh /usr/local/pg13/bin/pg_config ... /usr/local/pg19/bin/pg_config

For full functionality (drop-time metadata cleanup, which runs from an object
access hook) add the library to `shared_preload_libraries = 'columnar'`, as is
standard for a table access method extension.

Phase 1 implements a format-2.0-compatible metapage and storage layer, the
stripe/chunk/chunk_group catalog, uncompressed bulk insert batched into chunk
groups and stripes, and a sequential scan.

Phase 2 adds value-stream compression with four codecs (`none`, `pglz`, `lz4`,
`zstd` with a compression level), independent per chunk, with a fall back to
uncompressed storage when compression does not shrink the data. The exists
(null) stream is never compressed. It computes and stores a per-chunk min/max
skip list for orderable types in `columnar.chunk`, adds a reader that
decompresses per column and projects only referenced columns, and adds the
chunk-group skipping machinery that uses the min/max skip list to skip groups
that cannot match pushed-down scan qualifiers. The compression, compression
level, chunk-group row limit, stripe row limit, and qual-pushdown GUCs from the
specification are honored.

Phase 3 adds update and delete without rewriting stripes. Deletes and the old
side of updates are marked in the `columnar.row_mask` table (spec 7.5); an
update inserts the new row with a fresh row number. Scans apply the row mask and
skip deleted rows. Metadata is read with a command-id-advanced snapshot so a
transaction sees its own inserts and deletes made earlier in the same
transaction (read-your-writes) while other transactions stay isolated. Pending
writes and delete marks are flushed at each statement end and at pre-commit, and
are discarded on transaction rollback and on savepoint (subtransaction) rollback,
with correct attribution so work done before a savepoint survives ROLLBACK TO.
Temporary columnar tables are supported.

Phase 4 adds indexes and constraints. Every inserted row is assigned its stable
row number (and synthetic item pointer) at insert time, so `CREATE INDEX` builds
btree and hash indexes over a columnar table and ordinary index scans fetch rows
by item pointer, applying the row mask so deleted rows are never returned. Unique
and primary-key constraints reject duplicates on insert (across statements, and
within a single statement via the unflushed write buffer) and at index build
time; NOT NULL and CHECK constraints are enforced through the normal insert path.
A table converts between heap and columnar storage with
`columnar.alter_table_set_access_method(table, method)`, which rewrites through
the target access method and round-trips row counts and values. Index-only scans
are never chosen for columnar tables (there is no visibility map), while ordinary
index scans and sequential scans are.

Phase 5 adds planner integration and vacuum. A custom scan path replaces the
sequential scan for columnar tables: it reads only the columns the query
references (projection pushdown) and translates simple `column op const`
restriction clauses into scan keys, so the reader's per-chunk min/max skip
lists remove chunk groups that cannot match a filter. Because the executor
always re-applies the full restriction clauses as a filter, chunk-group
skipping is a pure optimization that never changes results: a filtered query
returns the same rows whether or not `columnar.enable_qual_pushdown` is set.
The custom scan is controlled by `columnar.enable_custom_scan`, and EXPLAIN
reports the projected column count and (under ANALYZE) how many chunk groups
were read versus removed by filter. Parallel sequential scans are disabled for
columnar tables so plans are stable. Per-table options in `columnar.options`
(spec 7.4) override the instance-wide compression, compression level,
chunk-group row limit, and stripe row limit for a table's later writes, set and
cleared with `columnar.alter_columnar_table_set` and
`columnar.alter_columnar_table_reset`. `columnar.vacuum(table)` compacts a table
by rewriting its live rows into full stripes, which combines many small stripes
into few and physically reclaims the space of rows marked deleted in the row
mask; indexes are rebuilt so their row addresses stay valid.
`columnar.vacuum_full(schema)` does the same across a schema, and
`columnar.stats(table)` reports the per-stripe layout.

Phase 6 adds vectorized execution. A vectorized batch reader hands back one
decoded chunk group at a time as flat per-column value and null arrays, so a
group is processed in tight typed loops rather than one tuple at a time, with
the same min/max chunk-group skipping, row mask, and column projection as the
scalar reader. A plan's simple `column op const` restriction clauses become
predicates evaluated column-at-a-time to build a selection vector; the custom
scan uses this to drop rows before forming a tuple while the executor still
re-applies the full qual, so a vectorized scan returns exactly the rows the
scalar scan returns. For a plain `SELECT agg(col) FROM t [WHERE ...]` with no
GROUP BY or HAVING, a vectorized aggregate computes `count`, `sum`, `avg`,
`min`, and `max` directly over the decoded arrays, reproducing PostgreSQL's own
result semantics exactly (integer sum as `int8`, integer average as `numeric`,
min and max by the column type's default ordering). Vectorization is controlled
by `columnar.enable_vectorization` (on by default); toggling it never changes a
result, only how the result is computed. An optional decompressed-chunk cache,
off by default behind `columnar.enable_column_cache` and sized by
`columnar.column_cache_size` megabytes, keeps decompressed value streams so
repeated reads of the same chunk group skip decompression; it never changes
results and is flushed automatically when a table is truncated, vacuumed, or
otherwise invalidated.

The vectorized aggregate is chosen only when every aggregate, column type, and
filter clause is one it fully supports: `count` (including `count(*)` and
`count(col)`), `sum` and `avg` over `smallint` and `integer` columns, and `min`
and `max` over any column type with a default ordering, with `WHERE` clauses
that are conjunctions of simple `column op const` comparisons. Anything else
falls back to the ordinary scalar aggregate plan and stays correct: `sum` and
`avg` over `bigint`, `numeric`, or floating-point columns, ordered-set and
string aggregates, `DISTINCT`-qualified aggregates, `GROUP BY`, `HAVING`, and
non-constant or non-simple filters. UPDATE and DELETE, and any query referencing
a whole-row or system column, use the scalar per-row scan.

Known limitations at this phase: row numbers are reserved a whole stripe at a
time, so a stripe flushed with fewer than `stripe_row_limit` rows leaves a gap in
the row-number space (harmless; row numbers need only be unique and stable). A
bulk UPDATE re-fetches each old row by item pointer to fill unchanged columns (as
the executor requires), which is O(rows x stripe) and not yet optimized. Unique
enforcement between two concurrent transactions inserting the same key can miss a
conflict only in the narrow window where one transaction's inserting statement is
still mid-flight (its rows not yet flushed and invisible to the other backend);
once a statement ends its rows are flushed and the conflict is caught. Stale
index entries left by deletes and updates are filtered on fetch and reclaimed by
REINDEX rather than removed opportunistically. `CREATE INDEX CONCURRENTLY` (the
concurrent validate path) and partial-block-range index builds are not supported.
`columnar.vacuum` accepts a `stripe_count` argument for interface compatibility
but always rewrites the whole relation into full stripes, which is the strongest
form of the compaction contract; because it renumbers rows it rebuilds the
table's indexes. The vectorized aggregate covers the single-relation,
ungrouped `SELECT agg(col) FROM t [WHERE ...]` shape for the aggregate and type
matrix above; aggregates, types, joins, and grouping outside that matrix run on
the correct scalar plan rather than the vectorized path.

`ALTER TABLE ... ADD COLUMN` on a populated table is supported without a
rewrite: a stripe written before the column existed carries no chunk for it, and
the reader then produces the column's missing value (NULL, or the constant
default the column was added with), matching heap's fast-default behavior.
Chunk-group skipping from a pushed-down filter is only applied when the
comparison's collation matches the column's own collation (the collation the
stored per-chunk min/max were ordered under); a differently collated comparison
is still applied as a filter but does not drive skipping, so results never
depend on whether the filter was pushed down.
