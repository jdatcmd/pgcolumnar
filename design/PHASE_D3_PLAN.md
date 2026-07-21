# Phase D3 plan: native reader

Status: plan, on the `re-origination` branch (`phase-d/d3-native-reader`). D3 makes
native-format (PGCN v1) tables round-trip: it reads the row groups and column
chunks the D2b writer produced and feeds them to the executor, so a native table
can be scanned. Once it works, the differential oracle runs its suites against
native tables (heap as the correctness oracle), which is the real validation the
writer has been waiting for.

## What D2b wrote (the read contract)

For a native table (storage id S):

- `pgcolumnar.storage` has one row: format_version 1, vector_length 1024,
  row_group_limit.
- `pgcolumnar.row_group` has one row per group: group_number, file_offset,
  row_count, byte_length.
- `pgcolumnar.column_chunk` has one row per (group, column): value_count (= group
  row count), encoding_descriptor (1 byte, 0 = uncompressed), block_codec (0),
  page_offset (logical byte offset), page_length.
- On disk at page_offset, each column chunk is `[validity bitmap][values]`:
  - validity: `ceil(row_count / 8)` bytes, one bit per row, LSB-first; set = present.
  - values: the present values only, serialized exactly as `ColumnarWriteRow`
    buffers them (the same bytes `ColumnarDecodeValue` reads in the 2.2 fetch
    path), concatenated in row order.

## Design

Extend the existing reader (`columnar_reader.c`) with a native mode rather than a
separate scan object, so the AM scan callbacks and the row-mask visibility path
are reused unchanged.

- Add `bool isNative` to `ColumnarReadState`, plus native cursors: the row_group
  list, the current group's per-column decoded state (a value cursor into the
  chunk's values, a pointer to the chunk's validity bitmap, and a running present
  index), the current group index, and row-in-group.
- `ColumnarBeginRead` / `ColumnarBeginReadWithStorage`: detect native via
  `ColumnarTableFormatVersion(relid) == COLUMNAR_NATIVE_VERSION_MAJOR` (or a
  storage-row lookup). When native, load the row_group list and the
  column_chunk map for S instead of the stripe list.
- `ColumnarReadNextRow`: when native, iterate row groups; for the current group,
  on first entry read each projected column's chunk bytes with
  `ColumnarReadLogicalData` into the group context and split into
  `[validity][values]`. For each row, for each column, read the validity bit; if
  set, `ColumnarDecodeValue` from that column's value cursor; else NULL. Advance
  to the next group at end.
- `ColumnarEndRead` / `ColumnarRescanRead`: reset the native cursors alongside the
  existing teardown.

### Scope for D3 (kept minimal, symmetric with the D2b baseline)

- Sequential scan only. Native tables bypass, for now: chunk-group skip predicates
  (there are no native zone maps until D5), the vectorized fast paths, projection
  pushdown (read all columns, project in the slot), and parallel scan. These are
  correctness-neutral (the executor re-applies quals and projection) and are
  optimized in later sub-phases.
- Delete visibility: native tables still record deletes through the existing row
  mask (D2b reused the 2.2 delete marking), so the reader applies the same
  `groupMask` logic keyed by row number. If wiring the row mask into the native
  group iteration proves intricate, D3 may first validate insert-only round-trips
  and defer delete visibility to a follow-up, but the target is full DML parity.

## Validation

- A new `native_roundtrip.sh`: create a native table, insert scalar data with
  nulls across several row groups, and compare `SELECT`s against a heap mirror
  with the differential oracle (order-independent set hash). This is the first
  time native data is read back.
- Extend the differential oracle (`test/lib.sh` `make_pair`) so a columnar side
  can be created native, and run a native pass of the core differential suite.
- Full PostgreSQL 13-19 matrix.

## Risks

- The reader is intricate (MVCC snapshot, row mask, projection, skip, parallel).
  The native branch deliberately bypasses the optimizations and reuses only the
  visibility essentials, keeping the change surface small and correctness-first.
- The values-stream decode must match the writer's buffering exactly; it reuses
  `ColumnarDecodeValue`, the same decoder the 2.2 fetch-by-tid path uses, so the
  encoding contract is shared and already exercised.
