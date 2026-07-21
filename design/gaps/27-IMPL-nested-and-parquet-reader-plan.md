# Gap 27 remainder — grounded plan: nested types + Parquet reader

Two pieces remain in gap 27 (the rest — Arrow/Parquet scalar export, Arrow
import, export type coverage — shipped). Both are large and land as their own
matrix-gated PRs.

## A. Nested-type export: arrays and composite (Arrow List / Struct, Parquet)

Grounded in columnar_arrow.c (hand-rolled FlatBuffers writer) and
columnar_parquet.c. Today `ArrowCol` is flat: one validity buffer plus either a
fixed buffer or (offsets, vardata). Nested types make a column recursive.

Arrow:
- Make `ArrowCol` hold optional children: for `A_LIST`, one child ArrowCol (the
  element) plus the list's own int32 offsets (n+1) and validity; for `A_STRUCT`,
  N child ArrowCols and only a validity buffer (no data buffer).
- `arrowcol_append`: for a list datum, `deconstruct_array` the element type and
  append each element to the child, push the running element count as the list
  offset; for a composite datum, `heap_deform_tuple` and append each field to its
  child. Recurse (one level covers 1-D arrays and flat composites; nested-of-
  nested is a later extension).
- `write_record_batch`: emit FieldNodes and buffers depth-first (parent before
  children), matching the schema's child order. List buffers: [validity,
  offsets] then the child's buffers; Struct buffers: [validity] then each child's
  buffers.
- Schema: `fb_arrow_type` gains ARROW_TYPE_List (12) and ARROW_TYPE_Struct (13);
  the Field writer emits a `children` vector of child Fields (recursive).
- Type mapping: `arrow_kind_for_type` maps `get_element_type(typid) != InvalidOid`
  -> A_LIST(child = element kind); `typtype == 'c'` (composite) -> A_STRUCT with
  child Fields from the composite's TupleDesc.

Parquet (columnar_parquet.c): arrays/composite need repetition + definition
levels. A 1-D array of a required element is rep/def-level pattern
(def 0=null list, 1=empty/null elem, 2=value; rep 0=new record, 1=same list).
Composite maps to a Parquet group. This is the harder writer; land Arrow nested
first, then Parquet nested.

Tests: extend arrow_export.sh / parquet_export.sh with int[]/text[] and a simple
composite column; read back with pyarrow (List<...>, Struct<...>) and compare to
the heap oracle rendered the same way. Empty arrays, NULL arrays, NULL elements,
NULL fields.

## B. Parquet reader (columnar.import_parquet)

pyarrow writes SNAPPY + RLE_DICTIONARY + DATA_PAGE_V2 by default, so a useful
reader needs all three. Pieces:
1. **Footer**: read the 4-byte `PAR1` magic + footer length; parse the
   `FileMetaData` Thrift **compact protocol** (schema elements, row groups,
   column chunks, codec, encodings, num_values, dictionary/data page offsets). A
   small compact-Thrift decoder (varint zigzag, field deltas, list/struct) is
   ~150 lines — implement from the Thrift compact spec (clean-room).
2. **Snappy**: `libsnappy.so.1` is present but there is no dev symlink/header;
   implement `snappy_raw_uncompress` from the Snappy format spec (preamble
   varint length + LZ77 copies) — ~150 lines, clean-room, matches the pglz-style
   codecs already in the tree.
3. **Pages**: DATA_PAGE_V2 header (Thrift), definition/repetition level bytes
   (RLE-only in v2), then values. Encodings: PLAIN (fixed + byte-array), and
   RLE_DICTIONARY (a dictionary page of PLAIN values + data pages of
   bit-packed/RLE indices). Implement the RLE/bit-packing hybrid decoder.
4. **Map to columnar**: for each row group/column, decode to Datums by Parquet
   physical/logical type -> PG type, and write through ColumnarWriteRow, matching
   import_arrow's target-table handling.

Scope order: scalar required/optional columns first (INT32/64, FLOAT/DOUBLE,
BYTE_ARRAY=text/bytea, BOOLEAN), PLAIN then dictionary, DATA_PAGE_V2 then V1.
Nested Parquet import last (rep/def reconstruction).

Tests: write Parquet from pyarrow (default options) for the scalar warehouse
types with NULLs and multiple row groups; import and compare to the heap oracle.
Gate on the full PG13-19 matrix (guard with a pyarrow-available check like the
export suites).

## Risk / sequencing
Land in this order, each its own gated PR: (A1) Arrow nested export, (A2) Parquet
nested export, (B1) Parquet reader scalar PLAIN+Snappy+pagev2, (B2) Parquet
dictionary, (B3) Parquet nested import. Keep changes additive; never regress the
scalar Arrow/Parquet writers (their suites must stay green each step).
