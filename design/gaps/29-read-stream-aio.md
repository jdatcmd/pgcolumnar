# Gap 29: read stream / asynchronous I/O in the scan

Status: PLANNED. Tier: performance. Format change: none. Effort: medium.

Adopt the PostgreSQL read stream API (PostgreSQL 17+) for the columnar block
reads so the PostgreSQL 18 asynchronous I/O subsystem prefetches chunk blocks
during a scan. Falls back to the current synchronous path on PostgreSQL 13-16.
No on-disk or SQL-surface change; results are unchanged. See
[../PG18_19_OPPORTUNITIES.md](../PG18_19_OPPORTUNITIES.md) item 1.

## Current state

`ColumnarReadLogicalData` (src/columnar_storage.c) reads a contiguous logical
byte range by walking its blocks, each with a synchronous `ReadBuffer` +
share-lock + memcpy + unlock. Both callers (src/columnar_reader.c) pass a whole
stripe's `fileOffset` and `dataLength`, so each call is a large, contiguous,
ascending block range that is fully known up front. There is no prefetch: a cold
scan waits on each block in turn.

## API (verified on the installed majors)

`storage/read_stream.h` is present in PostgreSQL 17, 18, and 19 and absent in 16
and earlier. The functions used are identical across 17-19:

- `ReadStream *read_stream_begin_relation(int flags, BufferAccessStrategy strategy,
  Relation rel, ForkNumber forknum, ReadStreamBlockNumberCB callback,
  void *callback_private_data, size_t per_buffer_data_size)`
- `Buffer read_stream_next_buffer(ReadStream *stream, void **per_buffer_data)`
- `void read_stream_end(ReadStream *stream)`
- `typedef BlockNumber (*ReadStreamBlockNumberCB)(ReadStream *stream,
  void *callback_private_data, void *per_buffer_data)`

PostgreSQL 18 adds `block_range_read_stream_cb` and `READ_STREAM_USE_BATCHING`;
we do not depend on them, so one code path covers 17-19. Gate is
`PG_VERSION_NUM >= 170000`.

## Design

Rewrite `ColumnarReadLogicalData` with two paths, selected at compile time and by
a GUC:

- PostgreSQL 17+ and `columnar.enable_read_stream = on` (default): open a read
  stream over the block range `[logicalOffset / COLUMNAR_BYTES_PER_PAGE ..
  (logicalOffset + length - 1) / COLUMNAR_BYTES_PER_PAGE]` with a trivial
  contiguous-range callback (return next block, then `InvalidBlockNumber`).
  `read_stream_next_buffer` returns the pinned buffers in that order; for each,
  share-lock, memcpy the page slice (`SizeOfPageHeaderData + pageOffset`, same
  math as today), unlock and release. `read_stream_end` at the end. Flags:
  `READ_STREAM_SEQUENTIAL`; strategy `NULL` (keep current buffer-cache behavior;
  a `BAS_BULKREAD` strategy is a possible follow-up); `per_buffer_data_size` 0.
- Otherwise: the current synchronous `ReadBuffer` loop, unchanged.

Callback private state: `{ BlockNumber next; BlockNumber last; }`. `length == 0`
returns immediately without opening a stream. Buffers come back in request order,
so the existing `L`/`pageOffset`/`n` walk drives the copy unchanged; assert each
returned buffer's block number matches the expected block.

Only `ColumnarReadLogicalData` changes. Single-block metapage reads keep plain
`ReadBuffer` (no benefit from streaming).

## Compatibility

New shim region in `src/columnar_compat.h` (or guarded include in
columnar_storage.c): `#if PG_VERSION_NUM >= 170000 #include "storage/read_stream.h"`.
The callback and its state struct are guarded the same way. GUC
`columnar.enable_read_stream` (bool, default on) registered in
columnar_tableam.c; on PostgreSQL 16 and earlier the GUC still exists but has no
effect (the streaming path is compiled out).

## Testing

- The whole existing suite exercises this path (every columnar read goes through
  `ColumnarReadLogicalData`), so the differential/recovery/fuzz suites and the
  full matrix are the correctness gate: streaming must return byte-identical data.
- Add `test/read_stream.sh`: load a columnar table and a heap mirror, then diff a
  range of query shapes with `columnar.enable_read_stream` on and off (both must
  equal the heap oracle), including a multi-stripe table so the streamed range
  spans many blocks. Add it to `test/run_all_versions.sh`.
- Benchmark (follow-up): cold-scan latency with the stream on vs off on
  PostgreSQL 18 (`io_method = worker`/`io_uring` vs `sync`); requires dropping
  caches, so it is measured separately from the standard bench.

## Effort / risk

Medium. Risk is confined to the read path and is caught by the differential
oracle. The API shape is stable across 17-19; the GUC gives an instant fallback
if a regression appears.
