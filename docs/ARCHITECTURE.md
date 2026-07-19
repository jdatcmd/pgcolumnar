# pgColumnar architecture

This document is a module-by-module map of the pgColumnar source so a new
developer can navigate the code. It is derived from the source in `src/` and
from `design/FORMAT_AND_INTERFACE_SPEC.md` (format version 2.0). Read the
specification first for the on-disk layout and catalog schema; this document
describes how the code is organized against it.

## On-disk model in one paragraph

A columnar relation stores its data in its own main fork using standard
PostgreSQL pages, so the buffer manager, WAL, and page checksums apply. Block 0
is a metapage, block 1 is reserved, and block 2 onward is a logical byte area.
Rows are grouped into stripes (a run of up to `stripe_row_limit` rows). A stripe
is divided into chunk groups (up to `chunk_group_row_limit` rows each). Within a
chunk group, one column's data is a chunk, holding a value stream and an exists
(null bitmap) stream. A separate set of ordinary heap tables in the `columnar`
schema is the metadata catalog: it records each stripe, chunk group, and chunk,
the per-chunk compression and min/max, and the delete/update row mask. A storage
id links a relation's physical file to its catalog rows.

## Module map

### columnar_tableam.c
The table access method handler and extension glue. It fills a `TableAmRoutine`
with the callbacks PostgreSQL calls for a `USING columnar` table: create
storage, bulk and single insert, sequential scan open/next/close, delete and
update (through the row mask), fetch a row by item pointer, size estimation, and
truncate. It also holds `_PG_init`, which registers every `columnar.*` GUC (the
compression codec and level, stripe and chunk-group row limits, and the qual
pushdown, custom scan, vectorization, and column cache toggles), the pre-commit
hook that flushes pending writes, and the object-access hook that removes a
table's metadata rows when the table is dropped.

### columnar_compat.h
Major-version compatibility shims. pgColumnar keeps one source tree that builds
on PostgreSQL 13 through 19. PostgreSQL changed several of its own API and
callback contracts across those majors (for example the RelFileNode to
RelFileLocator rename in 16, the table-AM signature changes in 19, and the
planner hook that suppresses index-only scans moving in 19). Each shim here only
selects the correct core API for the running major; none of them change
pgColumnar's behavior.

### columnar_storage.c
The physical storage layer: the metapage, the logical-to-physical byte mapping,
and the append-only reservation model. A logical offset maps to a block number
and in-page offset by `BYTES_PER_PAGE = BLCKSZ - SizeOfPageHeaderData`. The
metapage holds three high-water marks (next stripe id, next row number, next
logical byte offset) advanced under the relation extension lock, so concurrent
writers serialize their reservations there and then write their data
independently. This module reads and writes the raw logical buffers through the
buffer manager.

### columnar_metadata.c
Access to the `columnar` catalog tables (stripe, chunk_group, chunk, options,
row_mask) and the storage-id sequence. Metadata are ordinary heap tables keyed
by storage id, read and written with direct catalog access (heap opens, index
scans, and inserts) rather than SPI, so metadata access does not depend on SPI
reentrancy from inside a scan or write. Catalog reads use a snapshot whose
command id is advanced so a transaction sees its own just-written metadata
(read-your-writes) while staying isolated from other transactions.

### columnar_write_state.c
The writer. It batches incoming rows into per-column chunk buffers, closes a
chunk group when it reaches `chunk_group_row_limit`, and flushes a stripe (its
data pages plus its catalog rows) when the stripe fills or at transaction
pre-commit. Row numbers and the stripe id are reserved up front when a stripe
starts buffering, so every row has its stable row number and synthetic item
pointer at insert time (needed for indexes and unique checks). Pending writes
are held per relation and per subtransaction for the life of the transaction,
promoted to the parent on subtransaction commit and discarded on rollback. Per
chunk it computes and stores the min/max skip values for orderable types.

### columnar_compression.c
The value-stream codecs: `none`, `pglz` (built into PostgreSQL), `lz4`, and
`zstd` with a compression level. Each chunk's value stream is compressed
independently. If a codec does not shrink the data, or was not compiled into
this binary (lz4 and zstd are linked only when the system libraries are found),
the raw bytes are stored as `none` instead, and a request for an absent codec
falls back to one that is present. The exists (null bitmap) stream is never
compressed.

### columnar_reader.c
The reader and the shared value-stream decode path. It walks a relation's
stripes and chunk groups, decompresses each projected column's chunk into a
per-chunk-group context, applies the stripe's row mask so deleted rows are
skipped, and reconstructs rows. It has three entry shapes: the scalar
sequential scan (one tuple at a time), a fetch-by-item-pointer path (used by
index scans and by UPDATE to refill unchanged columns), and the vectorized
batch reader (`ColumnarReadNextVector`) that returns one decoded chunk group as
flat per-column value and null arrays. All three honor column projection,
min/max chunk-group skipping, and the row mask, and all three yield a column's
missing value (from `getmissingattr`) for a stripe written before an
`ADD COLUMN`.

### columnar_row_mask.c
Delete and update marking. A delete, and the old side of an update, sets a bit
in the `columnar.row_mask` entry for the row's chunk group rather than
rewriting the stripe; an update inserts the new row with a fresh row number.
Marks accumulate in a per-subtransaction in-memory buffer and are applied to
the catalog in one upsert per chunk group at flush time. The reader loads a
stripe's mask and skips masked rows.

### columnar_customscan.c
Planner and executor integration. A `set_rel_pathlist_hook` replaces a columnar
base relation's sequential-scan path with a custom scan path (and removes the
parallel paths, so plans are stable). The custom scan computes the referenced
column set from the target list and restriction clauses and pushes it into the
reader (projection pushdown), and translates simple `column op const` clauses
into scan keys so the reader's min/max skip lists remove chunk groups that
cannot match (qual pushdown). The executor always re-applies the full
restriction clauses as a filter, so chunk-group skipping never changes results.
This module also drives the vectorized scan mode and, through a
`create_upper_paths_hook`, adds the vectorized aggregate path for a supported
`SELECT agg(col) FROM t [WHERE ...]`. EXPLAIN reporting (projected columns, and
under ANALYZE the chunk groups read versus removed) lives here.

### columnar_vector.c
Vectorized execution kernels. A shared column-at-a-time filter turns a plan's
simple strict `column op const` clauses into predicates evaluated over a decoded
chunk group's arrays to build a selection vector (a null value or a failed
comparison excludes the row, matching SQL WHERE semantics). The vectorized
aggregates compute `count`, `sum`, `avg`, `min`, and `max` directly over the
decoded arrays, reproducing PostgreSQL's own result types exactly (integer sum
as int8, integer average as numeric, min/max by the column type's default
ordering). These paths are chosen only when every aggregate, column type, and
clause is fully supported; anything else falls back to the scalar plan.

### columnar_cache.c
The optional decompressed-chunk cache, off by default behind
`columnar.enable_column_cache` and bounded by `columnar.column_cache_size`
megabytes. It is a backend-local, LRU-bounded cache of decompressed value
streams keyed by storage id and absolute logical offset. It returns a fresh copy
to the caller so eviction is always safe, and it is flushed on any relcache
invalidation so a truncate offset reuse or a vacuum storage swap can never serve
a stale buffer. It only avoids repeated decompression; it never changes results.

### columnar_vacuum.c
Compaction, statistics, and storage-id lookup. `columnar.vacuum` materializes a
relation's live rows (the reader skips row-mask-deleted rows), swaps the
relation to a fresh relfilenode, removes the old metadata, writes the live rows
back into full stripes, and rebuilds the indexes so their item pointers address
the renumbered rows. This combines small stripes and physically reclaims
deleted-row space. `columnar.stats` reports the per-stripe layout, and a
storage-id lookup resolves a relation to its storage id.

### columnar_unique.c
Concurrent unique-key insert serialization (issue #5). Before a freshly inserted
row is handed back to the executor's index maintenance, the table-AM insert
paths call `ColumnarLockUniqueKeys`, which takes a transaction-scoped advisory
lock (the same `SET_LOCKTAG_ADVISORY` primitive as the `columnar_row_mask`
chunk-group lock) keyed by the row's unique key value(s). Because a columnar
row's data is invisible to other backends until its stripe flushes at statement
end, the btree dirty-snapshot uniqueness check can miss a conflict against a row
still buffered in another backend; holding a per-key lock to commit forces a
second inserter of an equal key to wait until the first commits and flushes, so
the ordinary btree check then catches the duplicate. Equal keys map to one lock
by hashing each key column with its type's default hash function (consistent
with the index equality: `numeric` scale, `citext` case, collation-aware text),
combining columns, and mapping into a bounded number of buckets per index
(`columnar.unique_lock_buckets`) to bound the lock budget. Unique, immediate,
valid indexes are handled, including multi-column, partial (predicate evaluated
per row), and expression indexes; an index whose operator class is not provably
the type default, or whose key type has no hash proc, falls back to one coarse
per-index lock. A relcache-invalidated backend-local cache holds the per-relation
unique-index metadata so the per-row cost is a hash plus a lock acquire.

## Data flow summaries

Insert: executor calls the table-AM insert or multi-insert callback ->
`columnar_unique` takes the per-key advisory lock for each applicable unique
index (issue #5) -> `columnar_write_state` buffers the row into per-column chunk
buffers, assigning its reserved row number and item pointer -> a full chunk group
is closed, a full stripe is compressed per column by `columnar_compression`,
written to pages by `columnar_storage`, and recorded by `columnar_metadata` ->
remaining buffers flush at statement end and pre-commit.

Scan: the planner installs the custom scan (`columnar_customscan`) with a column
projection and scan keys -> the reader (`columnar_reader`) walks stripes and
chunk groups, uses the min/max in `columnar_metadata` to skip groups, decodes
projected chunks through `columnar_compression` (optionally via
`columnar_cache`), applies the row mask (`columnar_row_mask`), and returns rows
or, in vectorized mode, decoded arrays that `columnar_vector` filters and
aggregates -> the executor re-applies the full qual as a filter.

Delete or update: the custom scan supplies each row's item pointer ->
`columnar_row_mask` marks the old row; an update also inserts the new row through
the writer.
