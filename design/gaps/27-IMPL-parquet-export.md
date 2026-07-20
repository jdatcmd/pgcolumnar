# Gap 27 — second slice implementation plan: Parquet export

Adds `columnar.export_parquet(rel regclass, path text) -> bigint`, a
self-contained Parquet file writer (no libparquet dependency). Verified with the
DuckDB CLI (`read_parquet`, built in) and pyarrow, both against a heap oracle.

## File layout

```
"PAR1"
<data page per column, per row group>
<FileMetaData (Thrift compact protocol)>
<footer length: int32 LE>
"PAR1"
```

One row group per PARQUET_ROWGROUP_ROWS (65536) rows. Within a row group, one
column chunk per column, each a single DataPage v1:
`[def levels: int32 LE length + RLE/bit-packed hybrid][values: PLAIN]`,
UNCOMPRESSED.

All columns are declared OPTIONAL (max definition level 1); a def level of 1
means present, 0 means null. PLAIN values are written for non-null rows only.

## Type mapping (first slice, matches export_arrow)

| PG type | Parquet physical | converted/logical |
| --- | --- | --- |
| int2 | INT32 | INT_16 |
| int4 | INT32 | (none) |
| int8 | INT64 | (none) |
| float4 | FLOAT | (none) |
| float8 | DOUBLE | (none) |
| bool | BOOLEAN | (none) |
| text, varchar | BYTE_ARRAY | UTF8 |
| bytea | BYTE_ARRAY | (none) |

Other types rejected with a column/type error. Little-endian hosts only.
Superuser only (server-side file write).

## Encodings

- PLAIN: INT32/INT64/FLOAT/DOUBLE little-endian fixed width; BOOLEAN bit-packed
  LSB-first; BYTE_ARRAY as int32 LE length + bytes. Non-null values only.
- Definition levels: RLE/bit-packed hybrid, bit width 1, written as one
  bit-packed run (num_groups = ceil(nrows/8)); section prefixed with its int32 LE
  byte length (DataPage v1 convention).

## Thrift compact protocol

Minimal writer: field header (delta id + compact type nibble; long form when
delta > 15 or 0), zigzag varint for i16/i32/i64, binary/string (varint len +
bytes), list header (size<<4 | elem type, long form when size >= 15), bool-as-
field-type, struct stop 0x00. Structures emitted: FileMetaData, SchemaElement,
RowGroup, ColumnChunk, ColumnMetaData, PageHeader, DataPageHeader.

## Files / build

New `src/columnar_parquet.c` (+ `src/columnar_parquet.o` in Makefile) and the SQL
function in `columnar--1.0.sql`. Row read loop mirrors export_arrow
(ColumnarReadNextRow, physical order, one row group flushed per
PARQUET_ROWGROUP_ROWS).

## Testing

`test/parquet_export.sh` (added to the matrix, gated on the DuckDB CLI):
export a mixed-type table mirroring a heap table (NULLs, int boundaries, NaN/Inf,
empty/unicode text, bytea; > one row group), then for every column compare
`read_parquet` output to the heap oracle (order-independent set hash per the
existing differential harness). If pyarrow is present, also assert the Parquet
schema types. Empty table and error cases (non-columnar, unsupported type).
Full matrix PG13-19.

## Out of scope

Compression (UNCOMPRESSED only), dictionary/RLE value encodings, statistics,
row-group size tuning, nested types, import, additional PG types.
