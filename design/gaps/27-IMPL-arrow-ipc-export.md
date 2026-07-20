# Gap 27 — first slice implementation plan: Arrow IPC stream export

Implements the export half of [27-arrow-parquet-interop.md](27-arrow-parquet-interop.md),
first slice: write a columnar table to an **Arrow IPC stream** file. Self-contained
writer (no libarrow link, no build dependency); pyarrow is used only as the test
oracle.

## Environment facts (verified)

- No libarrow/libparquet/arrow-glib at build time (checked with pkg-config).
- DuckDB CLI 1.1.3 present but its arrow support (`scan_arrow_ipc`/`to_arrow_ipc`,
  even with `INSTALL arrow`) is in-process buffer-based, **cannot read an IPC
  file** — no `read_arrow`, no `COPY TO (FORMAT arrow)`.
- **pyarrow 23.0.1 installed via apt** (`python3-pyarrow`) reads the IPC stream
  format incl. nulls — this is the test oracle.

## SQL surface

```
columnar.export_arrow(rel regclass, path text)  RETURNS bigint  -- rows written
```
- Writes an Arrow IPC **stream** (`0xFFFFFFFF`-framed messages: Schema,
  RecordBatch*, EOS). One RecordBatch per ARROW_BATCH_ROWS (16384) rows.
- Server-side file write (like other columnar.* admin funcs); requires the same
  privileges as writing server files. Errors if the path can't be opened.
- Errors on any column whose type is not in the supported set (below), naming the
  column and type — no silent lossy coercion.

## Type mapping (first slice)

| PG type | Arrow type | body buffers |
| --- | --- | --- |
| int2 | Int(16, signed)   | validity, values (2 bytes) |
| int4 | Int(32, signed)   | validity, values (4 bytes) |
| int8 | Int(64, signed)   | validity, values (8 bytes) |
| float4 | FloatingPoint(SINGLE) | validity, values (4) |
| float8 | FloatingPoint(DOUBLE) | validity, values (8) |
| bool | Bool              | validity, values (1 bit/row) |
| text, varchar | Utf8     | validity, offsets(int32, n+1), data |
| bytea | Binary           | validity, offsets(int32, n+1), data |

Everything else -> ereport(ERROR) (unsupported column type). Later slices:
numeric, date/time/timestamp (unit mapping), arrays, jsonb-as-string, large/utf8
64-bit offsets, dictionary encoding, big-endian.

## Files / build

- New `src/columnar_arrow.c` (+ decls where needed). Add
  `src/columnar_arrow.o` to Makefile OBJS.
- Contains: (a) a minimal little-endian **FlatBuffers builder** (tables+vtables,
  scalars, uoffsets, strings, vectors of offsets and of inline structs, unions);
  (b) Arrow **Schema** and **RecordBatch** message builders per Message.fbs /
  Schema.fbs (MetadataVersion V5, Endianness Little); (c) row reader loop via
  `ColumnarReadNextRow` accumulating per-column buffers, flushing a RecordBatch
  every ARROW_BATCH_ROWS; (d) encapsulated-message framing with 8-byte alignment
  and the EOS marker.

## Encapsulated message framing

Each message: `0xFFFFFFFF` (continuation) + int32 LE metadata_length (flatbuffer
size incl. padding to 8) + flatbuffer + body (padded to 8). EOS = `0xFFFFFFFF` +
`0x00000000`. Buffer offsets in RecordBatch are relative to body start; each
buffer padded to a multiple of 8. Validity bitmap always emitted (LSB-first, bit
set = valid); null_count reported per FieldNode.

## Testing (pyarrow oracle)

New `test/arrow_export.sh`, gated on pyarrow being importable (skip-with-note
otherwise, like BENCH_DUCKDB), added to `test/run_all_versions.sh` SUITES:
1. Type-matrix table (all supported types) with NULLs, empty strings, boundary
   ints (INT*_MIN/MAX), NaN/Inf floats, non-ASCII/embedded-quote text, empty and
   non-empty bytea. Export, read back with a small Python/pyarrow script, and
   compare every value to the same rows read from PostgreSQL (heap oracle) --
   order preserved (export is row-order), so compare row-by-row.
2. Multi-batch: > ARROW_BATCH_ROWS rows to exercise multiple RecordBatches.
3. Empty table: schema-only stream, 0 rows, still readable.
4. Errors: non-columnar relation; unsupported column type (e.g. json).
5. Schema fidelity: pyarrow schema field names/types/nullability match.

## Out of scope (later slices)

Import; Parquet; C Data Interface streaming; unsupported types above; compression
in the IPC body; big-endian hosts; COPY integration.
