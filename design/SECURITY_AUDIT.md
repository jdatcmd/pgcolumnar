# pgColumnar C memory-safety audit

Audit of all `src/*.c` against the CWE classes in the request: buffer overflow
(CWE-787/121), out-of-bounds read (CWE-125), use-after-free (CWE-416), double
free (CWE-415), null-pointer dereference (CWE-476), integer overflow/underflow
(CWE-190/191), format string (CWE-134). Method: per-file read-through plus manual
verification of every high-severity finding against the source.

## Verdict

- **No format-string, no use-after-free, no double-free anywhere.** All
  `elog`/`ereport` use literal format strings; buffer/StringInfo/context lifetimes
  are correct.
- **The normal path (self-produced data) is memory-safe** — the full PG13-19
  matrix exercises it.
- **The gap is robustness against *corrupt on-disk input*.** Several value-stream
  decoders and the reader trust catalog-supplied lengths, counts, offsets, and
  dictionary codes without validation, so a corrupt `columnar.*` catalog row or a
  corrupt/crafted value stream (including a malicious format-2.0 file) can cause
  out-of-bounds reads/writes, integer underflow, or division by zero. This is in
  scope because pgColumnar reads format-2.0 files and ships a corruption-hardening
  suite. It is defense-in-depth, not a flaw in normal operation.

Clean files: `columnar_compression.c` (exemplary length validation, safe LZ4/zstd
APIs), `columnar_write_state.c`, `columnar_customscan.c`, `columnar_tableam.c`,
`columnar_vacuum.c`, `columnar_unique.c`, `columnar_cache.c`, `columnar_row_mask.c`.

## Remediation status

FIXED. The decode/reader/bloom/metadata findings are fixed on
`feat/decode-hardening` (central `ColumnarDecodeChunk` validation, length-aware
`bitunpack`/`BitReader`, `decode_dict` code/length bounds, reader attr/group and
stream-offset guards, `chunk_row_count` guard, bloom `nbits` guard, metadata
`VARSIZE` guards, `fb_grow` 64-bit growth). The read-stream `Assert` finding is
fixed on `feat/read-stream-aio`. `test/corruption.sh` mutates the exact fields
and asserts a clean error or safe fallback with the backend surviving; the full
PostgreSQL 13-19 matrix (all suites incl. corruption) is green.

## Findings (corrupt-input robustness)

Severity is under the corrupt/malicious on-disk-input threat model.

### columnar_encoding.c — HIGH
- `bitunpack` and `br_get` (BitReader) take no input-length bound; every
  bit-packed decoder can read past the encoded buffer (CWE-125). Root cause.
- `ColumnarDecodeChunk` does no cross-validation: it never checks `encLen`
  against the per-encoding header size, `rawLen == n * att->attlen` for the
  fixed-width encodings, or width/code-width bounds (CWE-20 root cause).
- `decode_for` / `decode_delta` / `decode_dod`: read the element width from the
  on-disk header and feed it to `store_uint(raw + i*w, ...)`; with unvalidated
  `n`/`rawLen`/`w` this is an OOB read of the header (missing `encLen` check) and
  an OOB write to `raw` (CWE-125/787). `store_uint`/`load_uint` also over-run
  their 8-byte scratch when `w > 8`.
- `decode_dict`: **the dictionary code is never bounded against `nDistinct`**, the
  per-entry varlena length is unbounded, and the running output position is never
  checked against `rawLen` — `memcpy(raw + pos, dptr[code], dlen[code])` is an
  arbitrary-pointer read and an OOB write (CWE-125/787). Verified at
  columnar_encoding.c:805-811.
- `decode_gorilla`: unbounded `br_get` reads; a corrupt bitstream can make the
  trailing-zero count negative (shift UB) or the output length exceed `rawLen`.
- `decode_rle`: run/value reads can over-read the input by up to `w-1` bytes at
  the buffer end; output overflows if `rawLen < n * attlen`.

### columnar_reader.c — HIGH / MEDIUM
- `chunkMap[m->attrNum - 1][m->chunkGroupNum]` and the fetch-by-rownumber
  `chunkForGroup[m->attrNum - 1]`: `attrNum`/`chunkGroupNum` are signed and only
  upper-bounded; a corrupt `attr_num = 0` writes `chunkMap[-1]` (OOB write,
  CWE-787). Two sites.
- Stream offsets/lengths (`existsStreamOffset`, `valueStreamOffset`,
  `valueStreamLength`) are used to index the `palloc(stripe->dataLength)` stripe
  buffer with no check that they lie within `dataLength` (CWE-125).
- Division by zero when a corrupt `stripe.chunk_row_count = 0` drives the
  fetch-by-tid path (CWE-369, DoS).

### columnar_bloom.c — HIGH
- `ColumnarBloomProbe` reads `nbits` from the on-disk bloom bytea and indexes
  `bits[pos>>3]` without checking `bloomLen >= 5 + nbits/8`; a corrupt header with
  a large `nbits` and short buffer reads far out of bounds (CWE-125). It also
  yields wrong results (may drop a matching group). Verified.

### columnar_metadata.c — MEDIUM
- `VARSIZE(x) - VARHDRSZ` for the bloom/min/max/mask byteas underflows to a
  near-4GB `uint32` if a corrupt varlena header reports `VARSIZE < VARHDRSZ`,
  feeding a huge `palloc`/`memcpy` (CWE-191). Guard `VARSIZE >= VARHDRSZ`.
- `heap_getattr` `isnull` flags are ignored on NOT-NULL numeric columns; a NULL
  yields 0 and feeds the div-by-zero above. Add defensive checks.

### columnar_storage.c — HIGH (in the unmerged read-stream branch, not main)
- The new read-stream path guards `read_stream_next_buffer` returning
  `InvalidBuffer` only with `Assert` (compiled out in production); a corrupt
  offset that miscomputes the block range yields zero buffers, then
  `LockBuffer(InvalidBuffer)` crashes. Fix on `feat/read-stream-aio`: replace the
  Assert with a runtime `elog(ERROR)`, and validate the offset/length range.

### Not memory-safety, noted
- `columnar_vector.c`: `sum(int2/int4)` accumulates into an unchecked `int64`,
  matching core PostgreSQL's `-fwrapv` behavior; `sum(int8)` routes to numeric.
  Intentional, not a distinct vuln.
- `columnar_customscan.c`: a cross-type-but-same-family scan key (e.g. int4 col vs
  int8 const) is passed with `sk_subtype != column type`; if the reader's
  min/max compare does not dispatch on `sk_subtype`, a matching chunk group could
  be skipped before the executor re-check (wrong results, not memory). Confirm the
  reader path; separate from this audit.
- `fb_grow` (columnar_arrow.c) `newcap = cap*2` can overflow uint32, but is
  unreachable (the FlatBuffers builder holds only 16-column-bounded KB metadata).
  Add a guard for defense in depth.

## Remediation plan

1. Central validation gate in `ColumnarDecodeChunk`: reject `encLen`/`rawLen`/`n`/
   width combinations that violate the encoder's invariants (`rawLen == n*attlen`
   for fixed-width; `attlen ∈ {1,2,4,8}`; width/code-width ≤ 64) with
   `ERRCODE_DATA_CORRUPTED`. This alone bounds most fixed-width output writes.
2. Make `bitunpack`/`br_get` length-aware; reject reads past the input.
3. `decode_dict`: bound `code < nDistinct`, validate each dict-entry length and the
   running `pos` against `rawLen`, and keep the header walk within `encLen`.
4. Reader: validate `attrNum ∈ [1,natts]` and `chunkGroupNum ∈ [0,count)`; validate
   stream offsets/lengths within `dataLength`; reject `chunkRowCount <= 0`.
5. Bloom: validate `bloomLen >= 5 + nbits/8` (treat a malformed filter as "no
   filter").
6. Metadata: guard `VARSIZE >= VARHDRSZ`; honor `isnull`.
7. Read-stream branch: runtime `elog` instead of Assert; validate the range.
8. Extend `test/hardening.sh`/`test/fuzz.sh` to mutate these specific catalog
   fields (encoding type, raw/decompressed length, value count, attr/group
   numbers, dict codes, bloom `nbits`, stream offsets) and assert a clean error
   rather than a crash, on the assert-enabled matrix.
