# pgColumnar architecture

This document is a module-by-module map of the pgColumnar source so a new
developer can navigate the code. It is derived from the source in `src/` and
from `design/NATIVE_FORMAT_AND_INTERFACE_SPEC.md`. Read the specification first
for the on-disk layout and catalog schema; this document describes how the code
is organized against it.

Data is stored in one format, the native format PGCN v1. (An earlier 1.0-dev
format line has been removed; the `v1.0-dev` git tag preserves it.)

## On-disk model in one paragraph

A columnar relation stores its data in its own main fork using standard
PostgreSQL pages, so the buffer manager, WAL, and page checksums apply. Block 0
is a metapage, block 1 is reserved, and block 2 onward is a logical byte area.
Rows are grouped into row groups (a run of up to `stripe_row_limit` rows, the
write unit). Within a row group one column's data is a chunk, holding a validity
bitmap and a value stream encoded in fixed-size vectors. A separate set of
ordinary heap tables in the `pgcolumnar` schema is the metadata catalog:
`storage`, `row_group`, and `column_chunk` record the layout, `zone_map` records
each chunk's and each vector's minimum and maximum for skipping, `bloom` records
the per-chunk equality filters, `row_mask` records the delete and update marks,
and `options` holds per-table settings. A storage id links a relation's physical
file to its catalog rows.

## Module map

### columnar_tableam.c
The table access method handler and extension glue. It fills a `TableAmRoutine`
with the callbacks PostgreSQL calls for a `USING pgcolumnar` table: create
storage, bulk and single insert, sequential scan open/next/close, delete and
update (through the row mask), fetch a row by item pointer, size estimation, and
truncate. It also holds `_PG_init`, which registers every `pgcolumnar.*` GUC (the
compression codec and level, the row-group and vector row limits, and the qual
pushdown, custom scan, vectorized aggregate, and column cache toggles), the
pre-commit hook that flushes pending writes, and the object-access hook that
removes a table's metadata rows when the table is dropped.

### columnar_compat.h
Major-version compatibility shims. pgColumnar keeps one source tree that builds
on PostgreSQL 15 through 19. PostgreSQL changed several of its own API and
callback contracts across those majors (for example the RelFileNode to
RelFileLocator rename in 16, the table-AM signature changes in 19, and the
planner hook that edits the index list -- get_relation_info_hook through 18,
build_simple_rel_hook in 19 -- used to enable index-only scans). Each shim here
only selects the correct core API for the running major; none of them change
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
Access to the `pgcolumnar` catalog tables and the storage-id sequence: the
`storage`, `row_group`, `column_chunk`, `zone_map`, `bloom`, `row_mask`, and
`options` tables. Metadata are ordinary heap tables keyed by storage id, read and
written with direct catalog access (heap opens, index scans, and inserts) rather
than SPI, so metadata access does not depend on SPI reentrancy from inside a scan
or write. Catalog reads use a snapshot whose command id is advanced so a
transaction sees its own just-written metadata (read-your-writes) while staying
isolated from other transactions. The storage-row insert is serialized by a
storage-id advisory lock so concurrent first-writers do not race.

### columnar_write_state.c
The writer. It batches incoming rows into per-column buffers, closes a vector
when it reaches `chunk_group_row_limit`, and flushes a row group (its data pages
plus its catalog rows) when the group fills or at transaction pre-commit. The row
group number and the row-number range are reserved up front when a row group
starts buffering, so every row has its stable row number and synthetic item
pointer at insert time (needed for indexes and unique checks). Pending writes are
held per relation and per subtransaction for the life of the transaction,
promoted to the parent on subtransaction commit and discarded on rollback. At
flush it encodes each column chunk with the per-vector encoding cascade, computes
the zone maps and per-chunk bloom filters, and writes the `storage`, `row_group`,
`column_chunk`, `zone_map`, and `bloom` rows. The projection fan-out shares this
writer.

### columnar_compression.c
The value-stream codecs: `none`, `pglz` (built into PostgreSQL), `lz4`, and
`zstd` with a compression level. Each chunk's value stream is compressed
independently. If a codec does not shrink the data, or was not compiled into
this binary (lz4 and zstd are linked only when the system libraries are found),
the raw bytes are stored as `none` instead, and a request for an absent codec
falls back to one that is present. The exists (null bitmap) stream is never
compressed.

### columnar_encoding.c
Lightweight, type-aware value-stream encodings, applied per chunk before the
block codec and reversed after decompression, as reversible transforms of the
raw value-stream bytes (so all downstream decode is unchanged). Encodings: RLE,
frame-of-reference with bit-packing, delta and delta-of-delta (integer family),
Gorilla XOR (float4/float8), and a dictionary (any fixed-width or varlena type,
for low cardinality). `ColumnarEncodeChunk` measures each applicable encoding and
keeps the smallest; `ColumnarDecodeChunk` reverses it. This file also holds the
compression-block run iterator (`ColumnarBlockReader`) that exposes a chunk as
(value, run-length) pairs so an aggregate can fold a run at a time.

### columnar_bloom.c
Per-chunk bloom filters for equality chunk-group skipping. The writer hashes each
non-null value (hashable, non-collatable columns only) and builds a filter per
chunk; the reader probes it for an equality predicate the min/max range could not
rule out, skipping the group when the value is provably absent. Never a false
negative, so results are unaffected.

### columnar_reader.c
The reader and the shared value-stream decode path. It walks a relation's row
groups, decodes each projected column's per-vector encoded chunk into a per-group
context, applies the row group's row mask so deleted rows are skipped, and
reconstructs rows. It has two entry shapes: the scalar sequential scan
(`ColumnarReadNextRow`, one tuple at a time) and a fetch-by-row-number path
(`ColumnarReadRowByNumber`, used by index and bitmap scans and by UPDATE to
refill unchanged columns and by unique enforcement). Both honor column
projection, zone-map row-group and per-vector skipping, the per-chunk bloom
filter for equality predicates, and the row mask, and both yield a column's
missing value (from `getmissingattr`) for a row group written before an
`ADD COLUMN`. Under a parallel scan each worker claims distinct row groups from
the shared counter. The liveness cache (`ColumnarBuildLivenessCache`) reads the
row-group list and row masks once so a projection scan can test each row's base
row number cheaply.

### columnar_row_mask.c
Delete and update marking. A delete, and the old side of an update, sets a bit
in the `pgcolumnar.row_mask` entry for the row's row group rather than rewriting
the group; an update inserts the new row with a fresh row number. Marks
accumulate in a per-subtransaction in-memory buffer and are applied to the
catalog in one upsert per row group at flush time. The reader loads a row
group's mask and skips masked rows. This interim mask is replaced by first-class
delete vectors in a later phase.

### columnar_customscan.c
Planner and executor integration. A `set_rel_pathlist_hook` replaces a columnar
base relation's sequential-scan path with a custom scan path (and removes the
parallel paths, so plans are stable). The custom scan computes the referenced
column set from the target list and restriction clauses and pushes it into the
reader (projection pushdown), and translates simple `column op const` clauses
into scan keys so the reader's zone maps remove row groups and vectors that
cannot match (qual pushdown). The executor always re-applies the full
restriction clauses as a filter, so skipping never changes results. The scan is
the scalar per-row path (`ColumnarScanNext`). Through a `create_upper_paths_hook`
the module adds the vectorized aggregate path for a supported
`SELECT agg(col) FROM t [WHERE ...]`. EXPLAIN reporting (projected columns, and
under ANALYZE the row groups and vectors read versus skipped) lives here.

### columnar_vector.c
The vectorized aggregate path and its shared filter. A column-at-a-time filter
(`ColumnarVecSelect`) turns a plan's simple strict `column op const` clauses into
a selection vector over a decoded group; for integer, float, and date/time
columns it uses a typed, branch-free comparison loop (the btree strategy resolved
from the type's opfamily), falling back to the operator function otherwise (a
null value or a failed comparison excludes the row, matching SQL WHERE
semantics). The vectorized aggregate computes `count`, `sum`, `avg`, `min`, and
`max`, reproducing PostgreSQL's result types exactly (integer sum as int8,
integer average as numeric, min/max by the column type's default ordering). It is
answered from the zone-map metadata, falling back to a scan-and-fold when the
group has deletes. It is chosen only when every aggregate, column type, and
clause is supported; anything else falls back to the scalar plan.

### columnar_cache.c
The optional decompressed-chunk cache, off by default behind
`pgcolumnar.enable_column_cache` and bounded by `pgcolumnar.column_cache_size`
megabytes. It is a backend-local, LRU-bounded cache of decompressed value
streams keyed by storage id and absolute logical offset. It returns a fresh copy
to the caller so eviction is always safe, and it is flushed on any relcache
invalidation so a truncate offset reuse or a vacuum storage swap can never serve
a stale buffer. It only avoids repeated decompression; it never changes results.

### columnar_vacuum.c
Compaction, statistics, and storage-id lookup. `pgcolumnar.vacuum` materializes a
relation's live rows (the reader skips row-mask-deleted rows), swaps the
relation to a fresh relfilenode, removes the old metadata, writes the live rows
back into full stripes, and rebuilds the indexes so their item pointers address
the renumbered rows. This combines small stripes and physically reclaims
deleted-row space. `pgcolumnar.vacuum_sorted` runs the same rewrite but feeds the
live rows through a tuplesort keyed on the chosen columns first, so the table is
stored physically sorted (a one-time reorder; see gap 26). `pgcolumnar.stats`
reports the per-stripe layout, and a storage-id lookup resolves a relation to its
storage id.

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
(`pgcolumnar.unique_lock_buckets`) to bound the lock budget. Unique, immediate,
valid indexes are handled, including multi-column, partial (predicate evaluated
per row), and expression indexes; an index whose operator class is not provably
the type default, or whose key type has no hash proc, falls back to one coarse
per-index lock. A relcache-invalidated backend-local cache holds the per-relation
unique-index metadata so the per-row cost is a hash plus a lock acquire.

### columnar_arrow.c
Arrow IPC stream export (`pgcolumnar.export_arrow`, gap 27). A self-contained
FlatBuffers builder emits the Schema and RecordBatch messages (MetadataVersion
V5); rows are read in physical order via the scalar reader and buffered one
RecordBatch at a time (validity bitmap, then values, with utf8/binary offsets).
No libarrow dependency. Supported types are int2/int4/int8, float4/float8, bool,
text/varchar, and bytea; other types are rejected. Little-endian hosts only.

### columnar_parquet.c
Parquet export (`pgcolumnar.export_parquet`, gap 27). A self-contained Thrift
compact-protocol writer emits the file metadata (row groups, column chunks, page
headers) and PLAIN-encoded, UNCOMPRESSED data pages with RLE/bit-packed
levels. Columns shred into leaf column chunks with repetition and definition
levels (Dremel): scalars (one leaf), 1-D arrays (a 3-level LIST), and composites
(a group with one leaf per field). One row group per 65536 rows. No libparquet
dependency; same scalar type coverage as the Arrow writer.

### columnar_parquet_reader.c
Parquet import and the external-Parquet read surface (gap 27 and Phase G). A
self-contained reader: a Thrift compact-protocol decoder for the footer and page
headers, clean-room Snappy plus GZIP (zlib), ZSTD, and LZ4_RAW page
decompression, the RLE/bit-packed hybrid decoder for repetition/definition levels
and dictionary indices, PLAIN and dictionary value decoding, and both DATA_PAGE
v1 and v2 (what pyarrow writes). The schema tree is walked to derive each leaf
column's Dremel level bounds; nested columns are reconstructed by decoding each
leaf's full entry sequence (defs/reps/dense values) and grouping repeated runs
(LIST to array, group to composite), the inverse of the nested Parquet exporter.
Scalars remain byte-for-byte the flat path. No libparquet dependency.

One shared row-producing core (`pq_read_rows`, decode one row group into slots)
feeds three surfaces over the same file parse and type inference:

- `import_parquet` inserts into a target table via `table_tuple_insert`.
- `read_parquet` returns rows as a set-returning function, and `parquet_schema`
  reports the leaf columns and their inferred PostgreSQL types.
- The `pgcolumnar_parquet` foreign-data wrapper materializes the file into a
  tuplestore drained by the scan. It pushes down predicates by skipping row
  groups whose min/max statistics prove empty (only fixed-width ordered types,
  with NaN and inverted-interval guards), and projects by decoding only the
  columns the plan references (computed from the base rel's reltarget and quals).

A `path` that is a directory or glob resolves to a sorted list of files, each
read through the same core into the one sink; per-file decode buffers are freed
between files. Decode paths are hardened against crafted files: file-declared
sizes, DECIMAL scale, and per-row-group chunk counts are all range-checked so a
malformed footer yields a clean error rather than an out-of-bounds read or a
wrong value.

### columnar_visibilitymap.c
Index-only-scan support (gap 28). A columnar visibility-map fork records which
synthetic blocks (chunk groups) are all-visible. Lazy `VACUUM`
(`columnar_relation_vacuum`, ShareUpdateExclusiveLock) sets a block's bit only
when its stripe's inserting xid precedes the oldest removable-xid horizon and the
group has no deletes; every insert/delete/update clears the bit. Both set and
clear are WAL-logged (`log_newpage_buffer`). The core index-only-scan executor
reads these bits through `visibilitymap_get_status`, so an all-visible group is
answered from the index tuple and any other block falls back to the
snapshot-checked fetch. Gated by `pgcolumnar.enable_index_only_scan` (default on).

### columnar_projection.c
Multiple projections DDL and reader (gap 26). `add_projection` /
`drop_projection` manage the `pgcolumnar.projection` catalog; `add_projection`
back-fills the projection from existing rows under ShareLock. The catalog CRUD
lives in `columnar_metadata.c`; write fan-out and the sorted per-stripe encoder
live in `columnar_write_state.c` (each projection stores the base row number as a
leading column); the planner selection and executor projection scan live in
`columnar_customscan.c` (a covering projection with a restricted sort key is
scanned instead of the base, pruning chunk groups by the projection's min/max,
with deletes/visibility taken from the base). `pgcolumnar.vacuum` rebuilds
projections aligned to the compacted base.

## Data flow summaries

Insert: executor calls the table-AM insert or multi-insert callback ->
`columnar_unique` takes the per-key advisory lock for each applicable unique
index (issue #5) -> `columnar_write_state` buffers the row into per-column
buffers, assigning its reserved row number and item pointer -> a full vector is
closed, a full row group is encoded per column by `columnar_encoding` /
`columnar_compression`, written to pages by `columnar_storage`, and recorded by
`columnar_metadata` -> remaining buffers flush at statement end and pre-commit.

Scan: the planner installs the custom scan (`columnar_customscan`) with a column
projection and scan keys -> the reader (`columnar_reader`) walks row groups, uses
the zone maps in `columnar_metadata` to skip groups and vectors, decodes
projected chunks through `columnar_encoding` / `columnar_compression` (optionally
via `columnar_cache`), applies the row mask (`columnar_row_mask`), and returns
rows one at a time -> the executor re-applies the full qual as a filter. An
ungrouped aggregate is answered by `columnar_vector` from the zone-map metadata.

Delete or update: the custom scan supplies each row's item pointer ->
`columnar_row_mask` marks the old row; an update also inserts the new row through
the writer.
