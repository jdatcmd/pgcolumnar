# Phase E plan: ALP and FSST cascade primitives

Status: plan, on the `re-origination` branch (phase branches off `re-origination`).
Phase E adds two type-specific encoding primitives the native format specification
(section 5.3) calls for: ALP for floating-point columns and FSST for string
columns. Both are added as candidates in the existing adaptive per-vector
selector, so the selector chooses among the full primitive set and picks whichever
is smallest for each vector.

## Where this fits

The native format already encodes each 1024-value vector with an adaptive
selector (`ColumnarEncodeChunk` in `src/columnar_encoding.c`, D4). Each primitive
is an `encode_X` / `decode_X` pair. The selector measures every applicable
primitive on the vector and keeps the smallest pre-block-codec result; the reader
dispatches on the per-vector encoding code recorded in the encoding descriptor.
Adding a primitive is local:

- a new `COLUMNAR_ENCODING_*` code,
- `encode_X` / `decode_X` in `columnar_encoding.c`,
- a candidate line in `ColumnarEncodeChunk` and a dispatch case in
  `ColumnarDecodeChunk` (plus its length cross-check and `ColumnarEncodingName`).

No writer flush, encoding-descriptor, catalog, or reader-structure change is
needed: a primitive's own metadata (ALP's exponent, factor, and exceptions; the
FSST symbol table) is stored inline in the encoded buffer, self-describing, the
same pattern `decode_dict` already uses. The encoding code is one byte in the
descriptor, so codes 7 and 8 fit the existing format. The format is pre-release,
so adding codes needs no compatibility handling; the reader already errors on an
unknown code.

## Clean-room method

ALP and FSST are published algorithms with public reference implementations. Per
[PROVENANCE.md](../PROVENANCE.md) the implementation is built from the algorithm
descriptions in the peer-reviewed papers, not from any reference source code:

- ALP: Afroozeh, Kuffo, Boncz, "ALP: Adaptive Lossless floating-Point
  Compression" (SIGMOD 2023).
- FSST: Boncz, Neumann, Leis, "FSST: Fast Static Symbol Table Compression"
  (VLDB 2020).

Both papers describe the algorithm in prose and pseudocode; the code is written
from that description. No reference source is read.

## Correctness discipline

Every primitive is lossless and byte-exact: `ColumnarDecodeChunk(ColumnarEncodeChunk(raw))`
reproduces the raw value stream byte for byte. This is the governing property and
the first-line gate (`test/pbt/test_encoding.c`, a PostgreSQL-independent C
round-trip test over randomized and boundary inputs). The selector choice is a
size tradeoff, never a correctness one: a primitive returns false (declines) when
it cannot apply or cannot represent a value, and the reader reconstructs the exact
raw stream regardless of which primitive won. The differential oracle
(`native_encoding.sh`, `differential.sh`) then confirms end-to-end equality
against a heap mirror on native tables.

## Decomposition

Matrix-gated sub-PRs into `re-origination`. Iterate on PostgreSQL 17-19; run the
full 15-19 matrix before the final merge of each sub-phase.

### E1. ALP (floating-point)

`COLUMNAR_ENCODING_ALP = 7`, for `float4` and `float8`. Two sub-schemes selected
per vector, both lossless:

- ALP (decimal): many doubles are decimals in disguise (a price, a rate). For a
  chosen exponent e and factor f, `round(value * 10^e * f)` is an integer for most
  values; store those integers (themselves FOR plus bit-packed) plus a short
  exception list (position and original bit pattern) for the values that do not
  round-trip. e and f are chosen by sampling the vector for the pair that
  minimizes size while every value either round-trips exactly or becomes an
  exception. Decoding multiplies back and patches the exceptions, reproducing the
  exact IEEE-754 bits.
- ALP-RD (real double): for genuinely real values, split each value's bits into a
  left part (the high bits, low cardinality across a vector) and a right part;
  dictionary-encode the left parts and bit-pack the right. Reproduces the exact
  bits.

`encode_alp` measures both sub-schemes and keeps the smaller, recording which
sub-scheme and its parameters inline. Gorilla stays a candidate; the selector
picks ALP or Gorilla per vector by size (the spec keeps Gorilla as a primitive).
`numeric` is deferred (it is varlena and needs a scaled-integer path; `dict`
already covers low-cardinality numeric). Validation: `test/pbt` gains
decimal-shaped and real-double-shaped float generators asserting round-trip and
that ALP is chosen on decimal data; `native_encoding.sh` asserts an ALP-encoded
chunk and heap-oracle equality.

### E2. FSST (strings)

`COLUMNAR_ENCODING_FSST = 8`, for `text`, `varchar`, and short `bytea`. Build a
static symbol table of up to 255 symbols (1 to 8 bytes each) over a sample of the
vector's strings, greedily choosing the substrings that shorten the corpus most;
replace each string with a sequence of 1-byte symbol codes, using an escape code
for bytes no symbol covers. The symbol table is stored inline (the encoded buffer
is: table, then the per-value code streams with offsets), so decoding is a table
lookup per code. FSST keeps per-value random access, so the varlena raw stream is
reconstructed exactly, offsets and all. It is measured against `dict` in the
selector; the smaller wins (high-cardinality strings that `dict` declines are
where FSST pays off). Validation: `test/pbt` gains string generators (repeated
substrings, natural-language-like, high cardinality) asserting round-trip and FSST
selection on compressible high-cardinality strings; `native_encoding.sh` asserts
an FSST-encoded chunk and heap-oracle equality; the differential type matrix
already exercises text/varchar/bytea end to end.

### E3 (optional). Selector and cascade refinement

The deferred D4b work: true multi-level cascade chaining across primitives and
sample-based selection (measure ~1% of vectors, apply one cascade chunk-wide, per
BtrBlocks). This is correctness-neutral (a size and speed tradeoff) and only worth
doing if E1/E2 show the exhaustive per-vector selector is a measurable cost. The
current selector measures every candidate on the full 1024-value vector, which is
cheap and per-vector optimal, so E3 is a refinement, not a requirement. Tracked
here; executed only if warranted.

## Validation (each sub-phase)

- `test/pbt/test_encoding.c` round-trip green over the new data shapes (the
  byte-exact gate), plus the existing shapes.
- `native_encoding.sh` asserts the new primitive is chosen on data shaped for it
  and that reads match the heap oracle.
- The differential oracle stays green on native tables (`differential.sh`,
  `fuzz.sh`, `hardening.sh`).
- `corruption.sh` / `hardening.sh` confirm a corrupt ALP or FSST descriptor raises
  a clean error, not a crash or out-of-bounds read (the new decoders cross-check
  their inline metadata lengths before use, as the existing decoders do).
- Full PostgreSQL 15-19 matrix, assert-enabled, no warnings, before merge.

## Risks

- A float or string encoder that miscomputes a boundary (an exception it should
  have recorded, a symbol-table overflow) would corrupt data. Mitigated by the
  byte-exact round-trip property test with boundary generators (subnormals,
  infinities, NaN, empty strings, 8-byte symbols, non-UTF-8 bytea) and by each
  encoder declining (returning false, falling back to another primitive or raw)
  whenever it cannot represent a value exactly.
- Adding encoding codes is an on-disk format change. It is additive and
  pre-release, and the reader validates the code, so an old reader meeting a new
  code errors cleanly rather than misreading.
