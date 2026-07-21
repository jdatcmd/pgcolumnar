# pgColumnar format and interface specification

This document specifies the on-disk storage format, the metadata catalog, and
the SQL interface of the columnar table access method. It records functional and
interoperability facts only: block layout, catalog schema, identifier encodings,
compression codes, the SQL surface, and behavioral contracts. It does not
reproduce source code or internal implementation expression.

The purpose of this document is twofold. It is the interoperability
specification that lets an independent implementation read and write the same
on-disk format and expose the same SQL surface. It is also the reference an
implementer works from without reading the existing source, which is the
condition for the reimplementation to be free of the upstream copyright. See
REWRITE_PLAN.md for how that condition is maintained.

Format version described here is 2.2. Version 2.1 adds optional per-chunk
value-stream encodings (section 5.1) and two chunk catalog columns (section 7.2).
Version 2.2 adds multiple physical projections: a `columnar.projection` catalog
(section 7.7) and additional per-table columnar storages, all additive. Each
minor is otherwise identical to its predecessor, and 2.0/2.1 files are read
unchanged by a 2.2 implementation (only the metapage major version is validated;
a table with no `columnar.projection` rows is a single implicit base projection).

## 1. Terminology

- Storage id: a 64 bit identifier assigned to a columnar relation's physical
  storage. It links the physical file to its metadata rows.
- Stripe: a contiguous run of a relation's data holding up to a configured
  number of rows across all columns. A relation is a sequence of stripes.
- Chunk group: a horizontal slice of a stripe holding up to a configured number
  of rows for every column.
- Chunk: the data of one column within one chunk group. A chunk holds a value
  stream and an exists (null bitmap) stream.
- Row number: a 1 based logical position of a row within a relation, stable for
  the life of the row. Row numbers are mapped to synthetic item pointers so that
  the executor and indexes can address rows.
- Logical offset: a byte position in the relation's logical data area. Logical
  offsets are translated to physical block and in-page offset.

## 2. Physical storage layout

Columnar data lives in the relation's main fork, using standard PostgreSQL
pages so that the buffer manager, WAL, and checksums apply. The layout is:

- Block 0: the metapage. Fixed contents, described in section 3.
- Block 1: reserved and left empty.
- Block 2 and onward: the logical data area. The first logical byte is at
  logical offset `ColumnarFirstLogicalOffset`, which equals
  `2 * (BLCKSZ - SizeOfPageHeaderData)`.

A relation always has at least two blocks.

### 2.1 Logical to physical mapping

Let `BYTES_PER_PAGE = BLCKSZ - SizeOfPageHeaderData`. A logical offset L maps to:

- block number: `L / BYTES_PER_PAGE`
- in-page byte offset: `SizeOfPageHeaderData + (L % BYTES_PER_PAGE)`

The inverse maps a physical (block, offset) back to
`BYTES_PER_PAGE * block + offset - SizeOfPageHeaderData`.

Data is written as large contiguous logical buffers and split across page
boundaries by this mapping. New reservations are aligned to the start of a new
page.

### 2.2 Reservation model

The metapage records three high water marks that are advanced under a relation
extension lock: the next unused stripe id, the next unused row number, and the
next unused logical byte offset. Writers reserve a stripe id, a run of row
numbers, and a byte range by advancing these values, then write the data at the
reserved offset and record the stripe in the catalog. Concurrent writers only
need an ordinary lock on the relation because reservation is serialized by the
extension lock.

## 3. Metapage (block 0)

The metapage stores fixed size metadata at the start of block 0, after the
standard page header. Fields, in order:

| Field | Type | Meaning |
| --- | --- | --- |
| versionMajor | uint32 | format major version, currently 2 |
| versionMinor | uint32 | format minor version, currently 0 |
| storageId | uint64 | storage id linking file to catalog rows |
| reservedStripeId | uint64 | first unused stripe id |
| reservedRowNumber | uint64 | first unused row number |
| reservedOffset | uint64 | first unused logical byte offset |
| unloggedReset | bool | reserved for unlogged support, currently unused |

On initialization the reserved stripe id is 1, the reserved row number is
`COLUMNAR_FIRST_ROW_NUMBER` (1), and the reserved offset is
`ColumnarFirstLogicalOffset`. The metapage is written with a WAL full page image
and an immediate sync, since it does not pass through shared buffers on
creation.

The storage id itself is drawn from a database wide sequence
`columnar.storageid_seq` with a minimum value of 10000000000 and no cycling, so
storage ids are unique and monotically increasing within a database.

## 4. Stripe and chunk data layout

A stripe holds, for each column, the concatenation of that column's chunks
across all chunk groups in the stripe. Within a chunk, two streams are stored:

- Value stream: the encoded, optionally compressed column values for the rows in
  the chunk that are not null.
- Exists stream: a boolean array marking which rows in the chunk are present
  (not null). It is stored uncompressed, so its compressed and decompressed
  lengths are equal.

Each stripe is a single logical byte range starting at the stripe's file offset.
The catalog records, per column and per chunk group, the byte offset and length
of the value stream and the exists stream relative to the stripe, along with the
compression type, compression level, decompressed length, and value count. The
reader uses these to seek directly to the streams it needs, reading only the
projected columns and only the chunk groups that pass filtering.

A stripe is limited to `stripe_row_limit` rows. A chunk group is limited to
`chunk_group_row_limit` rows. Both are per relation options with instance wide
defaults.

## 5. Compression

Each chunk's value stream is compressed independently. Compression types are
encoded as small integers:

| Code | Type |
| --- | --- |
| 0 | none |
| 1 | pglz |
| 2 | lz4 |
| 3 | zstd |

The compression level applies to zstd. If a chunk does not compress smaller than
its raw form, it is stored uncompressed with type 0. The exists stream is never
compressed.

### 5.1 Value-stream encoding (format 2.1)

Before block compression, a chunk's value stream may be transformed by a
lightweight, reversible encoding whose code is recorded per chunk. An encoding
maps the raw value stream bytes to a smaller encoded stream; block compression is
then applied to the encoded stream. The reader decompresses to the encoded stream
and reverses the encoding to obtain the byte-identical raw stream, so all
downstream decoding is unchanged. Encoding types are:

| Code | Type |
| --- | --- |
| 0 | none |
| 1 | rle (run-length of a fixed-width value) |
| 2 | for (frame of reference + bit-packing) |
| 3 | delta (delta + zigzag + bit-packing) |
| 4 | gorilla (XOR-against-previous for float4/float8) |
| 5 | dod (delta-of-delta + zigzag + bit-packing) |
| 6 | dict (dictionary of distinct values + bit-packed codes) |

rle applies to any fixed-width column; for, delta, and dod apply to fixed-width
by-value integer-family types (attribute length 1, 2, 4, or 8); gorilla applies
to float4 and float8; dict applies to any fixed-width or varlena column and is
the encoding for low-cardinality text and other variable-length types. Each
chunk is encoded with whichever applicable encoding yields the smallest stream,
and records type 0 when none helps.
Format 2.0 chunks carry no encoding fields and are read as type 0 (none), with
the raw length equal to the decompressed length (see 7.2).

## 6. Row identity and item pointers

Rows carry a stable 1 based row number. Row numbers are mapped to synthetic
item pointers so that scans and indexes can address them:

- block number of the item pointer: `rowNumber / VALID_ITEMPOINTER_OFFSETS`
- offset of the item pointer:
  `(rowNumber % VALID_ITEMPOINTER_OFFSETS) + FirstOffsetNumber`

`VALID_ITEMPOINTER_OFFSETS` is the number of item pointer offsets that are valid
on a page for this purpose, bounded by `MaxOffsetNumber`. The inverse recovers
the row number from an item pointer. Row number 0 is invalid. The maximum row
number is bounded by `VALID_ITEMPOINTER_OFFSETS * VALID_BLOCKNUMBERS`.

## 7. Metadata catalog

Metadata lives in the `columnar` schema as ordinary heap tables, keyed by
storage id. These tables and their indexes are part of the format. The current
schema (format 2.0) is below. Column order matters for on the wire compatibility
of tools that read the catalog by attribute number, so it is specified here.

### 7.1 columnar.stripe

| # | Column | Type |
| --- | --- | --- |
| 1 | storage_id | bigint |
| 2 | stripe_num | bigint |
| 3 | file_offset | bigint |
| 4 | data_length | bigint |
| 5 | column_count | integer |
| 6 | chunk_row_count | integer |
| 7 | row_count | bigint |
| 8 | chunk_group_count | integer |
| 9 | first_row_number | bigint |

Indexes:
- `stripe_pkey` unique on (storage_id, stripe_num)
- `stripe_first_row_number_idx` unique on (storage_id, first_row_number)

### 7.2 columnar.chunk

| # | Column | Type |
| --- | --- | --- |
| 1 | storage_id | bigint |
| 2 | stripe_num | bigint |
| 3 | attr_num | integer |
| 4 | chunk_group_num | integer |
| 5 | minimum_value | bytea |
| 6 | maximum_value | bytea |
| 7 | value_stream_offset | bigint |
| 8 | value_stream_length | bigint |
| 9 | exists_stream_offset | bigint |
| 10 | exists_stream_length | bigint |
| 11 | value_compression_type | integer |
| 12 | value_compression_level | integer |
| 13 | value_decompressed_length | bigint |
| 14 | value_count | bigint |
| 15 | value_encoding_type | integer |
| 16 | value_raw_length | bigint |
| 17 | bloom_filter | bytea |

Index:
- `chunk_pkey` unique on (storage_id, stripe_num, attr_num, chunk_group_num)

`bloom_filter` (format 2.1) is an optional per-chunk bloom filter over the
column's non-null values, used to skip a chunk group on an equality predicate
when the value is provably absent. It is present only for hashable,
non-collatable columns and chunks large enough to benefit; it is NULL otherwise
and absent in 2.0 chunks. Its layout is `[uint32 nbits][uint8 k][ceil(nbits/8)
bytes]`, with nbits a power of two and k probe positions per value derived from
the type's hash by double hashing.

`value_decompressed_length` is the length of the stream produced by
decompression, which is the encoded stream (5.1); for encoding type none it
equals the raw value-stream length. `value_encoding_type` (5.1) and
`value_raw_length` (the fully decoded raw value-stream length) are present from
format 2.1; they are absent/NULL in 2.0 chunks, where a reader assumes encoding
none and `value_raw_length` = `value_decompressed_length`.

`minimum_value` and `maximum_value` are the encoded per chunk min and max of the
column, used as a skip list for chunk group filtering. They are present only for
types that have a byte comparable or family comparable ordering.

### 7.3 columnar.chunk_group

| # | Column | Type |
| --- | --- | --- |
| 1 | storage_id | bigint |
| 2 | stripe_num | bigint |
| 3 | chunk_group_num | integer |
| 4 | row_count | bigint |
| 5 | deleted_rows | bigint |

Index:
- `chunk_group_pkey` unique on (storage_id, stripe_num, chunk_group_num)

### 7.4 columnar.options

| # | Column | Type |
| --- | --- | --- |
| 1 | regclass | regclass |
| 2 | chunk_group_row_limit | integer |
| 3 | stripe_row_limit | integer |
| 4 | compression_level | integer |
| 5 | compression | name |

Index:
- `options_pkey` unique on (regclass)

### 7.5 columnar.row_mask

Tracks deleted rows for updates and deletes without rewriting stripes.

| # | Column | Type |
| --- | --- | --- |
| 1 | id | bigint |
| 2 | storage_id | bigint |
| 3 | stripe_id | bigint |
| 4 | chunk_id | integer |
| 5 | start_row_number | bigint |
| 6 | end_row_number | bigint |
| 7 | deleted_rows | integer |
| 8 | mask | bytea |

Indexes:
- `row_mask_pkey` unique on (id, storage_id, start_row_number, end_row_number)
- `row_mask_chunk_unique` unique on (storage_id, stripe_id, chunk_id, start_row_number)
- `row_mask_stripe_unique` unique on (storage_id, stripe_id, start_row_number)

`mask` is a bit array over the rows in the range `[start_row_number,
end_row_number]`; a set bit marks a deleted row. `id` is drawn from
`columnar.row_mask_seq`.

### 7.6 Sequences

- `columnar.storageid_seq`: minimum value 10000000000, no cycle.
- `columnar.row_mask_seq`: identifiers for row mask rows.

### 7.7 columnar.projection (format 2.2)

Records the physical projections of a table (multiple projections, the C-Store
model). A projection is a named, ordered subset of the table's columns stored as
its own columnar storage (its own `proj_storage_id`, with stripe/chunk/chunk_group
rows keyed by that id, in the same relation file), sorted on `sort_key`, sharing
the base table's row-number identity space.

| Column | Type | Notes |
| --- | --- | --- |
| storage_id | bigint | the base table's storage id |
| projection_id | integer | 0 = implicit base projection, 1..N additional |
| name | name | projection name, unique per storage_id |
| proj_storage_id | bigint | this projection's own storage id (base uses the table's) |
| sort_key | smallint[] | attnums in sort order (empty = insert order) |
| columns | smallint[] | attnums stored (base = all live columns) |

Unique indexes on `(storage_id, projection_id)`, `(storage_id, name)`, and
`(proj_storage_id)`. A projection's stripes store, as a leading `int8` column, the
base row number of each row, so a projection joins back to the base; deletes and
MVCC visibility derive from the base row mask, so only INSERT fans out to
projections. A table with no rows here has a single implicit base projection
(so 2.0/2.1 tables are unaffected). Projection storage ids draw from
`columnar.storageid_seq`.

## 8. SQL interface

### 8.1 Extension and access method

- The extension installs into schema `columnar` and creates the table access
  method named `columnar` with `TYPE TABLE HANDLER columnar.columnar_handler`.
- A table is columnar when created with `USING columnar` or converted with the
  conversion function below.

### 8.2 Functions

| Function | Arguments | Purpose |
| --- | --- | --- |
| columnar_handler | internal | table access method handler |
| alter_columnar_table_set | table_name regclass, chunk_group_row_limit int, stripe_row_limit int, compression name, compression_level int | set per table options, NULL leaves a value unchanged |
| alter_columnar_table_reset | table_name regclass, chunk_group_row_limit bool, stripe_row_limit bool, compression bool, compression_level bool | reset options to instance defaults |
| alter_table_set_access_method | t text, method text | convert a table between heap and columnar |
| vacuum | tablename regclass, stripe_count int default 0 | compact by combining recent stripes (rebuilds projections and indexes) |
| vacuum_full | schema name, sleep_time real, stripe_count int | vacuum across a schema |
| add_projection | rel regclass, name text, columns text[], sort_key text[] default '{}' | declare a physical projection (format 2.2); back-fills existing rows |
| drop_projection | rel regclass, name text | drop a projection and free its storage |
| export_arrow / export_parquet | rel regclass, path text | write the table to an Arrow IPC / Parquet file (scalars, 1-D arrays, composites); returns rows written |
| import_arrow / import_parquet | rel regclass, path text | insert an Arrow / Parquet file's rows into a table; returns rows inserted (scalars, 1-D arrays, composites) |
| read_projection | rel regclass, name text | (debug) read a projection's stored columns for live rows |
| reconstruct_via_projection | rel regclass, name text | (debug) read all rows via a projection, reconstructing non-covered columns from the base |
| stats | regclass, out stripeid, fileoffset, rowcount, deletedrows, chunkcount, datalength | per stripe statistics |
| create_table_row_mask | table_name regclass | create the row mask backing for a table |
| upgrade_columnar_storage | rel regclass | upgrade a table's metapage to the current format |
| downgrade_columnar_storage | rel regclass | downgrade a table's metapage |
| columnar_ensure_am_depends_catalog | | ensure catalog dependencies exist |

### 8.3 Configuration parameters

Instance wide defaults, overridable per table where noted, and scan control
flags:

| Parameter | Default | Meaning |
| --- | --- | --- |
| columnar.compression | zstd | default compression, per table overridable |
| columnar.compression_level | 3 | default zstd level, per table overridable |
| columnar.chunk_group_row_limit | 10000 | rows per chunk group, per table overridable |
| columnar.stripe_row_limit | 150000 | rows per stripe, per table overridable |
| columnar.enable_column_cache | off | enable the decompressed chunk cache |
| columnar.column_cache_size | 200 | cache size in megabytes |
| columnar.min_parallel_processes | 8 | minimum parallel processes for columnar work |
| columnar.planner_debug_level | debug3 | log level for planner messages |
| columnar.enable_custom_scan | on | use the columnar custom scan path |
| columnar.enable_compressed_execution | on | fold aggregates over runs of the value stream |
| columnar.enable_bloom_filter | on | skip chunk groups on equality via per-chunk bloom filters |
| columnar.enable_late_materialization | on | decode output columns only after the filter selects rows |
| columnar.enable_qual_pushdown | on | push filters into the scan for chunk skipping |
| columnar.enable_index_only_scan | on | answer covering index queries from the visibility-map fork (gap 28) |
| columnar.enable_projection_scan | on | let the planner scan a covering projection instead of the base (gap 26) |
| columnar.enable_columnar_index_scan | off | allow the custom index backed scan |
| columnar.qual_pushdown_correlation_threshold | 0.4 | correlation threshold for pushdown |

## 9. Behavioral contracts

- Writes are append only. A bulk insert batches rows into chunk groups and
  stripes, flushing a stripe when it reaches `stripe_row_limit`. Pending writes
  are held per relation until transaction end and flushed at pre commit.
- Reads project only referenced columns and skip chunk groups whose min and max
  cannot match a pushed down filter.
- Update and delete mark rows in the row mask rather than rewriting stripes.
  Space is reclaimed by vacuum.
- Transactions and savepoints are honored. Pending writes are discarded on
  rollback, and metadata rows created in an aborted transaction are not visible.
- Temporary columnar tables are supported.
- Btree and hash indexes and the constraints built on them are supported.
  Index-only scans are supported through a columnar visibility-map fork
  (format-independent, in the relation's VM fork): lazy `VACUUM` marks a chunk
  group all-visible only when its inserting transaction precedes the oldest
  removable-xid horizon and it has no deletes, every write clears the bit
  (WAL-logged), and a not-all-visible block falls back to the snapshot-checked
  fetch, so an index-only answer never returns a row invisible to the snapshot.
- Multiple projections (format 2.2) are honored: every write fans out to each
  projection, `columnar.vacuum` rebuilds them aligned to the compacted base, and
  the planner may scan a covering projection whose leading sort column is
  restricted. Results are identical to a base scan.
- Unsupported: logical replication of columnar tables, unlogged columnar tables,
  and table sample scans.

## 10. Compatibility requirement

An independent implementation that follows sections 2 through 8 can create,
read, and write format 2.0 storage and can operate on tables created by the
existing extension. The metapage version fields let an implementation detect and
refuse or upgrade older formats.

Version 2.1 is on-disk compatible with 2.0 at the value-stream level: 2.0 value
streams are encoding none and read unchanged. The catalog gained three chunk
columns (7.2); a 2.1 reader treats them as NULL for any 2.0 chunk row, meaning
encoding none, raw length equal to the decompressed length, and no bloom filter.
A catalog created by a 2.0 implementation lacks those columns physically, so an
in-place upgrade of an existing 2.0 database first adds them (nullable, default
NULL) before a 2.1 binary reads it; a fresh 2.1 install creates the columns
directly. The behavior of the NULL-default path is covered by test/hardening.sh.
