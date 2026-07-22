# Phase D4 plan: cascade encoding and adaptive selection

Status: plan, on the `re-origination` branch (a `phase-d/d4-cascade` branch off
`re-origination` once approved). D4 makes native-format (PGCN v1) column chunks
compressed. D2b wrote raw `[validity bitmap][values]` chunks with the encoding
descriptor hardcoded to a single zero byte and `block_codec` 0; D3 read them back
value by value. D4 fills in the encoding: each column chunk is encoded per
1024-value vector with the lightweight primitives, an optional block codec runs
over the chunk bytes, and the chosen scheme is recorded in the
`pgcolumnar.column_chunk` encoding descriptor so the reader reconstructs the exact
raw bytes D3 expects. The differential oracle stays the correctness proof: a
native table still returns the same rows as its heap mirror, now from compressed
storage.

## What already exists (the reuse base)

The lightweight-encoding toolkit and the block-codec layer are already in the tree,
clean-room and matrix-tested on the 1.0-dev (2.2) stripe path. D4 does not write a
new codec library; it wires the existing one into the native flush and load
functions.

- `src/columnar_encoding.c`: the primitives NONE, RLE, FOR, DELTA, GORILLA, DOD,
  and DICT, with bit-packing and zigzag helpers. FOR and DELTA already fold
  bit-packing in (each primitive is itself a short internal cascade). The selector
  `ColumnarEncodeChunk(raw, rawLen, att, valueCount, &out, &outLen)` returns the
  winning encoding type and its encoded bytes; `ColumnarDecodeChunk(enc, encLen,
  encodingType, att, valueCount, rawLen, cx)` is the exact inverse. A run iterator
  (`ColumnarBlockReaderInit` / `ColumnarBlockNextRun`) already exists for later
  run-stream aggregation.
- `src/columnar_compression.c`: `ColumnarCompressValueStream` /
  `ColumnarDecompressValueStream` for pglz, lz4, and zstd, each storing raw when the
  codec does not shrink and reporting the codec actually used.
- 2.2 wiring precedent to mirror: encode then compress at
  `columnar_write_state.c:593-618`; decompress then decode at
  `columnar_reader.c:1630-1638`.
- Catalog: `pgcolumnar.column_chunk` already has `encoding_descriptor bytea` and
  `block_codec smallint` (`pgcolumnar--1.0.sql:203-214`), with the C struct,
  Anum accessors, and insert/read plumbing in place
  (`NativeColumnChunkMetadata`, `ColumnarInsertColumnChunkRow`,
  `ColumnarReadColumnChunkList`). D4 needs no SQL or catalog schema change.
- A codec property harness exists at `test/pbt/test_encoding.c` (`test/pbt/run.sh`).

## The write and read seams

- Write: `columnar_flush_row_group` (`columnar_write_state.c:744`). The chunk is
  laid out column-major as `[validity bitmap][values]`; the descriptor is set to a
  single zero byte at lines 842-857. The flush currently concatenates every
  1024-value chunk-group value stream into one flat column chunk, so the per-vector
  boundary survives only in the in-memory `chunkGroups` list iterated at 786-807.
- Read: `columnar_native_load_group` (`columnar_reader.c:745`) reads the row group
  whole, points `nativeValidity[c]` at the chunk base and `nativeValueCursor[c]` at
  `base + validityBytes`, and ignores the descriptor. Per-row reconstruction in
  `columnar_native_next_row` (line 799) advances that cursor with
  `ColumnarDecodeValue`.

## Design

The correctness-first seam keeps the `[validity bitmap][encoded values]` chunk
layout and confines all D4 work to the flush and the group-load step.

- Write: for each column chunk, encode each 1024-value vector with
  `ColumnarEncodeChunk`, concatenate the encoded vectors, optionally run a block
  codec over the concatenation with `ColumnarCompressValueStream`, and write
  `[validity][encoded (maybe block-compressed) values]`. Record the recipe in the
  encoding descriptor and set `block_codec` to the codec actually used.
- Read: in `columnar_native_load_group`, when the descriptor is not the
  uncompressed baseline, reverse the block codec with
  `ColumnarDecompressValueStream`, then decode each vector with
  `ColumnarDecodeChunk` back into the exact raw value stream D2b produced,
  materialize it in the group context, and point `nativeValueCursor[c]` at it.
  `columnar_native_next_row` is unchanged: it still walks the raw stream with
  `ColumnarDecodeValue`. Because the decode reconstructs identical bytes, the
  round-trip is byte-exact.

### Encoding descriptor layout

The `encoding_descriptor` bytea becomes a small self-describing header, versioned
by a leading tag so a later minor can extend it:

- descriptor version tag and the chunk's vector length,
- vector count, then per vector: encoding type (the `int` from
  `ColumnarEncodeChunk`), encoded byte length, raw byte length, and value count
  (1024 for all but the last vector).

Persisting per-vector encoded and raw lengths restores the vector boundary the
native writer currently drops at flush, which is also what D5 needs to attach a
zone map to each vector. The uncompressed baseline written by D2b (a single zero
byte) is read as "one implicit vector, encoding NONE, no block codec" so existing
native tables written before D4 still read without a rewrite.

### Selector scope

Decision (owner, 2026-07-21): Option A. D4 reuses the existing per-vector selector
and block codec; the true multi-level cascade and sample-based selection are
deferred to D4b (or folded into Phase E, whose ALP and FSST are new cascade
primitives anyway).

Section 5.1 of the native spec calls for a cascade to depth three chosen by
sampling about one percent of a chunk's vectors. There were two ways to land that,
differing in the size and novelty of this PR:

- Option A, reuse first: use the existing per-vector selector
  (`ColumnarEncodeChunk`) exhaustively over each 1024-value vector. Each primitive
  is already an internal short cascade (FOR plus bit-packing, delta plus zigzag
  plus bit-packing), so this delivers adaptive, per-vector, descriptor-recorded
  encoding plus the block codec using only proven code. Exhaustive measurement over
  1024 values is cheap, so sampling is not needed for cost. Arbitrary multi-level
  chaining across primitives and sample-then-apply-chunk-wide selection become a
  D4b refinement.
- Option B, full spec now: implement true three-level chaining across the
  primitives and a sample-based selector that measures about one percent of a
  chunk's vectors and applies one cascade chunk-wide, per BtrBlocks. Higher ratio
  potential, but a larger and more novel surface in a single matrix-gated PR.

Option A was chosen: it is the matrix-gated foundation that gets real compression
onto native tables with the least risk, and it matches the minimal-per-PR
discipline of D1 through D3 and the PHASE_D_PLAN risk note that a wrong selector
choice must only ever be a size or speed regression, never a correctness bug.
Option B is tracked as D4b.

## Scope for D4 (kept minimal, symmetric with D2b and D3)

In scope:

- Native column chunks encoded per 1024-value vector with the existing primitives,
  chosen adaptively; an optional block codec over the chunk; the descriptor
  persisted; the reader reconstructing exact bytes.
- Property and round-trip validation and the full matrix.

Deferred, and why it is correctness-neutral:

- ALP and FSST float and string schemes are Phase E; D4 uses the current primitive
  set (GORILLA remains the float primitive until ALP lands).
- Zone maps, vector and chunk skipping, and zone-map-only aggregates are D5; D4
  persists the per-vector boundaries D5 builds on but computes no aggregates.
- Compressed execution (handing the executor still-encoded vectors) is deferred.
  After D3 native tables use only the base access-method scan, so in D4 the
  executor consumes fully reconstructed values. Operating on encoded vectors needs
  a native vector path re-enabled and is a later optimization; it cannot change
  results because the reader always reconstructs exact values.

## Validation

- Extend `test/pbt/test_encoding.c` (or add a native cascade case) to cover the
  descriptor round-trip: encode then decode reproduces the input across the
  supported types and null patterns, including empty and single-value vectors and
  the last short vector.
- Add `test/native_encoding.sh` (or extend `native_roundtrip.sh`): an encoded
  native table matches its heap mirror by the order-independent set hash across
  several row groups with nulls, and `pgcolumnar.column_chunk` shows non-baseline
  descriptors and reduced `page_length` on compressible data (a low-cardinality
  column, a monotonic column, a constant column), with a row-count sanity assertion
  so a down cluster cannot make the check pass falsely. Register it in
  `test/run_all_versions.sh`.
- Full PostgreSQL 13 through 19 matrix; the differential oracle green on
  native-format tables.

## Risks

- The selector choice is a size and speed tradeoff, never correctness: the reader
  reconstructs exactly what the descriptor records, proven by the property tests
  and the differential oracle.
- Two format lines coexist through D5; native stays opt-in until D6, so D4 is
  exercised only by tables that select the native format and by the new native
  cases.
- Reconstructing a column chunk to a full raw buffer at group-load raises native
  scan memory to about one row group's decoded size. This is bounded by
  `row_group_limit` and is the same order as the 2.2 path's per-stripe decode; it
  is revisited if and when compressed execution removes the full materialization.
