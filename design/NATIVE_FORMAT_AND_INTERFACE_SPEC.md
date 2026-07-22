# pgColumnar native format and interface specification

Status: draft, in progress on the `re-origination` branch. This is the build
source for the re-originated pgColumnar engine, replacing the 1.0-dev compatibility
specification in [FORMAT_AND_INTERFACE_SPEC.md](FORMAT_AND_INTERFACE_SPEC.md). It
is written from public research (the papers in section 15) and the open Apache
Arrow, Parquet, and ORC specifications, not from any other implementation's
source. See [PROVENANCE.md](../PROVENANCE.md), [TODO_PIVOT.md](TODO_PIVOT.md), and
[DESIGN_PIVOT_ORIGINAL_ENGINE.md](DESIGN_PIVOT_ORIGINAL_ENGINE.md).

This document specifies the on-disk storage format, the metadata catalog, and the
SQL interface. It records design decisions and contracts, not implementation
expression. Little-endian hosts are assumed for the layout below; a big-endian
port byte-swaps at the page boundary.

## 1. Design principles

These follow from the research and set the shape of the format.

1. The unit of decode and execution is a fixed-size **vector** of values, so
   encodings are data-parallel and the executor can process vectors directly,
   including in encoded form (FastLanes; Abadi et al. on operating on compressed
   data).
2. Compression is a **cascade** of lightweight encodings chosen per block by
   sampling, not a single fixed scheme plus a general block codec (BtrBlocks).
3. General block compression (lz4, zstd) is **opt-in per storage tier**, not the
   default, because on fast local storage its CPU cost can exceed the I/O saving
   (Zeng et al.). Dictionary encoding is applied aggressively, including on float
   columns.
4. Data skipping is served by **Small Materialized Aggregates** (min, max, sum,
   count, null count per block) and per-block bloom filters (Moerkotte; Databricks
   data skipping).
5. Mutation uses **delete vectors and merge-on-read**, not in-place rewrite (Delta
   Lake deletion vectors).
6. The format is a **new line**, identified by its own magic and version, with no
   compatibility requirement to the 1.0-dev format. Interchange is through Arrow
   and Parquet.

## 2. Terminology

Open-standard columnar vocabulary is used throughout.

- **Storage id**: a 64-bit identifier for a relation's physical columnar storage,
  linking the relation file to its catalog rows.
- **Row group**: a horizontal partition of a relation holding up to a configured
  number of rows across all columns. A relation is a sequence of row groups. Row
  groups are the write unit and the parallel-scan unit.
- **Column chunk**: the data of one column within one row group.
- **Vector**: a fixed run of values within a column chunk, the unit of decode,
  data skipping, and vectorized execution. The vector length is a format constant,
  1024 values.
- **Page**: the on-disk container of one column chunk's encoded vectors plus its
  local headers. A page is a contiguous byte range within the relation file.
- **Zone map**: the Small Materialized Aggregate for a vector or a column chunk.
- **Row number**: a 1-based logical position of a row within a relation, stable
  for the life of the row, mapped to a synthetic item pointer for the executor and
  indexes.

## 3. Physical storage layout

Columnar data lives in the relation's main fork, so the buffer manager, WAL, and
page checksums apply, as in the 1.0-dev line.

- **Block 0 is the metapage.** It carries the format magic `PGCN` (pgColumnar
  native), the format major version (initially 1), the vector length (1024), the
  storage id, and the reservation marks (next row group number, next row number,
  next logical byte offset). The major version is the only compatibility gate: a
  reader refuses a major it does not implement.
- **Logical data area.** Row-group pages are appended to a logical byte space that
  is mapped to physical blocks and in-page offsets, so a page may span block
  boundaries. The mapping is the same logical-to-physical scheme the engine
  already uses; it is not tied to any column layout.
- **Alignment.** Vectors within a page are aligned to their value width so a
  reader can address a vector without a byte-unaligned load.

## 4. Row groups, column chunks, and vectors

- A row group holds up to `pgcolumnar.row_group_limit` rows (default to be set in
  section 12; the working target is 122880 rows, a multiple of the 1024 vector
  length, replacing the 1.0-dev 150000 stripe limit which was not vector-aligned).
- Within a row group, each column is one column chunk. A column chunk is a
  sequence of vectors of 1024 values, except the last, which holds the remainder.
- Each vector is independently addressable and carries its own zone map, so a
  scan can skip a vector without decoding it, and a filtered scan decodes only the
  vectors of the output columns that have surviving rows (late materialization at
  vector granularity).
- Null values are recorded per column chunk as a validity bitmap, one bit per row,
  laid out so a vector's validity slice is a contiguous run.

## 5. Encodings

### 5.1 Cascade and adaptive selection

A column chunk is encoded by a **cascade**: the output of one lightweight scheme
is the input to the next, to a bounded depth (three levels). For each column chunk
the encoder samples a small fraction of its vectors (about one percent, following
BtrBlocks) and greedily chooses the cascade that minimizes encoded size on the
sample, then applies it to the whole chunk. The chosen cascade is recorded in the
catalog as an encoding descriptor (section 11) so the reader reconstructs it
exactly.

### 5.2 Primitive schemes

The primitives, all data-parallel over a vector:

- **Frame of reference (FOR)** plus **bit-packing**: store values as offsets from
  the vector minimum in the fewest bits.
- **Delta** and **delta-of-delta**: for monotonic or near-monotonic sequences,
  followed by FOR and bit-packing.
- **Run-length (RLE)**: for long runs, as a value stream and a run-length stream,
  each further encodable.
- **Dictionary**: a per-column-chunk dictionary plus bit-packed codes, applied
  aggressively including on float columns with low distinct-value ratios.
- **Constant** and **sparse**: a single value, or a frequent value plus a patch
  list of exceptions.

### 5.3 Type-specific schemes

- **ALP** for `float4`, `float8`, and `numeric`: the adaptive decimal scheme
  encodes values that originated as decimals as integers, and the real-double
  scheme (ALP_rd) vector-compresses the front bits of genuinely real values. ALP
  replaces the 1.0-dev Gorilla codec as the default float scheme; Gorilla may be
  retained as a cascade primitive.
- **FSST** for `text`, `varchar`, and short `bytea`: a symbol-table encoding that
  compresses short strings while keeping per-value random access, feeding a
  dictionary or RLE stage.

### 5.4 Compressed execution

Because encodings are data-parallel over the 1024-value vector and the reader can
stop the cascade partway (partial bottom-up decode), the executor receives
vectors that are still encoded where profitable and runs directly on them: a
predicate over a dictionary code stream, an aggregate over an RLE run stream (a
value repeated N times costs one operation), and FOR or delta arithmetic without
materializing full values.

## 6. Block compression

General block compression (lz4, zstd, and pglz) is available as an optional final
stage over a page's encoded bytes, controlled per table and per storage tier
(section 12). It is off by default for local storage and on by default for tables
marked remote or cold, reflecting the storage-dependent tradeoff. A page that does
not shrink is stored without the block stage. The block codec is recorded per
page.

## 7. Data skipping

### 7.1 Zone maps (Small Materialized Aggregates)

Each vector and each column chunk carries a zone map: minimum, maximum, sum,
count, and null count, for types that have a default btree ordering and, for sum,
a defined addition. The scan uses zone maps to:

- prune a vector or a whole column chunk whose min and max cannot satisfy a
  pushed-down `column op const` predicate;
- answer an ungrouped aggregate (`count`, `sum`, `min`, `max`) from the zone maps
  without decoding, when there is no residual filter (a generalization of the
  1.0-dev metadata `count(*)`).

Pruning respects collation: a predicate whose collation differs from the column's
own collation is applied as a filter but does not drive skipping, so results never
depend on pushdown.

### 7.2 Bloom filters

Each column chunk may carry a bloom filter for equality skipping on hashable,
non-collatable types, sized from the chunk's distinct-value estimate. A scan skips
a chunk whose filter proves the probe value absent.

## 8. Mutation: delete vectors and merge-on-read

- Deletes and the old side of updates are recorded in a **delete vector**: a
  per-row-group bitmap, one bit per row number in the group, set when the row is
  deleted. Data pages are not rewritten.
- A scan reads a row group's delete vector and skips set rows while keeping vector
  cursors aligned. Visibility for MVCC and for the transaction's own changes
  follows the same snapshot model as the 1.0-dev line.
- `UPDATE` is delete plus insert; the new row gets a fresh row number in the
  current row group.
- `MERGE` (PostgreSQL 15 and later) is served through the delete-vector path.
- Background compaction rewrites row groups to physically drop deleted rows and to
  recluster (section 9), reclaiming space; it is the point at which delete vectors
  are retired.

## 9. Ordering and clustering

- A row group may be stored sorted on one or more columns, which makes those
  columns' zone maps tight and non-overlapping so range predicates and ordered
  scans skip more (as in the 1.0-dev sorted storage).
- Multi-dimensional clustering orders rows by a **space-filling curve** (Z-order,
  and preferably Hilbert) over several columns, so skipping improves across those
  columns together rather than only the leading one. Clustering is produced and
  maintained by background compaction, incrementally, not only by a one-time
  reorder.

## 10. Projections

A projection is a named subset of a table's columns stored a second time in its
own sort or clustering order, sharing the table's row identity by carrying the
base row number. The planner scans a projection instead of the base when it covers
the query and its ordering serves the predicate. Deletes and visibility derive
from the base delete vector, so only inserts fan out to projections. This carries
the 1.0-dev projection design forward under the new catalog and namespace.

## 11. Metadata catalog

All catalog objects are in the `pgcolumnar` schema. Names describe the native
format and do not mirror any other implementation. Column lists below are the
logical contract; exact types follow the implementation.

- **`pgcolumnar.storage`**: one row per columnar relation. storage_id, relation
  oid, format version, vector length, row_group_limit, options snapshot.
- **`pgcolumnar.row_group`**: one row per row group. storage_id, group_number,
  file_offset, row_count, byte_length, sort_key, clustering descriptor.
- **`pgcolumnar.column_chunk`**: one row per column chunk. storage_id,
  group_number, column_index, value_count, encoding_descriptor (the cascade),
  block_codec, page_offset, page_length.
- **`pgcolumnar.zone_map`**: Small Materialized Aggregates. storage_id,
  group_number, column_index, vector_index (or null for the whole chunk),
  minimum, maximum, sum, value_count, null_count.
- **`pgcolumnar.bloom`**: storage_id, group_number, column_index, filter bytes.
- **`pgcolumnar.delete_vector`**: storage_id, group_number, bitmap bytes,
  deleted_count.
- **`pgcolumnar.projection`**: storage_id, name, column list, sort or clustering
  key, projection storage_id.
- **`pgcolumnar.options`**: per-table storage options (section 12).

Zone maps and encoding descriptors are stored decoded in the catalog so the
planner reads them cheaply; the on-disk pages carry only what the reader needs to
reconstruct values.

## 12. SQL interface

### 12.1 Namespace

The extension, schema, access method, and GUC prefix are all `pgcolumnar`.

- `CREATE EXTENSION pgcolumnar;`
- `CREATE TABLE ... USING pgcolumnar;`
- functions in schema `pgcolumnar`, GUCs with prefix `pgcolumnar.`.

### 12.2 Prefer standard SQL

Standard PostgreSQL SQL is used wherever it exists, and a `pgcolumnar.*` function
is reserved only for what has no standard form.

- Conversion: `ALTER TABLE ... SET ACCESS METHOD pgcolumnar` (PostgreSQL 15 and
  later); a helper covers 13 and 14.
- Maintenance: native `VACUUM` and autovacuum drive visibility-map maintenance and
  trigger background compaction; there is no bespoke vacuum function for the common
  path. An explicit `pgcolumnar.compact(table)` and
  `pgcolumnar.cluster(table, columns)` are provided for on-demand compaction and
  reclustering.
- Mutation: `INSERT`, `UPDATE`, `DELETE`, `COPY`, and `MERGE` are native.
- Per-table options: table reloptions where possible, with
  `pgcolumnar.set_options(table, ...)` for options that do not map to reloptions.
- Statistics: `pgcolumnar.stats(table)` reports per-row-group counts, deleted
  counts, sizes, and encodings.
- Interchange: `pgcolumnar.export_arrow`, `pgcolumnar.export_parquet`,
  `pgcolumnar.import_arrow`, `pgcolumnar.import_parquet`.

### 12.3 GUCs

The settings mirror the 1.0-dev behavior under the new prefix, plus the format's
new controls. Non-exhaustive: `pgcolumnar.row_group_limit`,
`pgcolumnar.vector_length` (fixed at 1024, read-only), `pgcolumnar.block_codec`
(none, lz4, zstd, pglz; default none for local storage),
`pgcolumnar.block_codec_level`, `pgcolumnar.enable_bloom_filter`,
`pgcolumnar.enable_zone_map_aggregate`, `pgcolumnar.enable_vectorization`,
`pgcolumnar.enable_compressed_execution`, `pgcolumnar.enable_index_only_scan`,
`pgcolumnar.enable_projection_scan`, `pgcolumnar.enable_read_stream`. Defaults and
ranges are finalized during implementation and documented in the user-facing
configuration reference.

## 13. Format identity and versioning

- Metapage magic: `PGCN`. Major version starts at 1.
- The major version is the compatibility gate. A reader reads any minor within a
  major it implements; a new minor is additive (a new optional catalog column, a
  new encoding scheme a reader may not produce but must decode if the descriptor
  requires it, subject to the reader knowing the scheme).
- The native format has no requirement to read the 1.0-dev format (magic and
  version differ). A one-time migration path is `COPY` or Arrow/Parquet import
  from a 1.0-dev table into a native table.

## 14. Interoperability

Arrow IPC and Parquet import and export are first-class and self-contained (no
libarrow or libparquet dependency), covering scalar types, one-dimensional arrays,
and composite types with nulls, as in the 1.0-dev line. The roadmap extends this
to reading external Parquet and open-table-format files with predicate and
projection pushdown. Arrow and Parquet, not binary compatibility with any
extension, are the interchange and migration path.

## 15. References

The design is grounded in these public sources (full context in
[ROADMAP.md](ROADMAP.md) "Future directions"):

- Vectorized layout and cascade with partial decode: FastLanes, PVLDB vol.18,
  2025.
- Sample-based adaptive cascade selection: BtrBlocks, SIGMOD 2023.
- Float and decimal encoding: ALP, SIGMOD 2024.
- Block-compression tradeoff and dictionary effectiveness: Zeng et al., VLDB 2024.
- Small Materialized Aggregates: Moerkotte, VLDB 1998; Databricks data skipping.
- Delete vectors and merge-on-read, space-filling clustering: Delta Lake.
- Operating on compressed data; column-store design space: Abadi et al., column
  stores survey and tutorial.

## 16. Relationship to the 1.0-dev line

The 1.0-dev format (2.2), specified in
[FORMAT_AND_INTERFACE_SPEC.md](FORMAT_AND_INTERFACE_SPEC.md), remains on `main`
and is preserved by the `v1.0-dev` tag. The native format is a separate line and
is not required to read it. The implementation reuses the parts of the engine that
are independent of the format (table access method plumbing, MVCC and row
visibility, index and index-only-scan support, the vectorized executor, and the
Arrow and Parquet codecs), as recorded in DESIGN_PIVOT_ORIGINAL_ENGINE.md section
3, and re-originates the on-disk layout, catalog, and SQL surface per this
document.
