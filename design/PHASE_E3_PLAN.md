# Phase E3 plan: selector cost and cascade refinement

Status: active investigation, branched off `main` after E2 (FSST) merges. E3 is
the refinement the Phase E plan tracked as "executed only if warranted" -- it is
evidence-gated, not a scheduled deliverable. This document is written before any
E3 code, per the plan-before-code discipline: it fixes the gate measurement and
the design options that measurement chooses between.

## The question E3 answers

The per-vector cascade selector (`ColumnarEncodeChunk`, called once per chunk
group from `columnar_write_state.c`) measures every applicable encoding on the
full 1024-value vector and keeps the smallest. This is per-vector optimal for
compression ratio, and cheap for the lightweight integer/float encoders. FSST
(E2) changed the cost profile: building a symbol table per vector is by far the
costliest encoder in the set. E2 already added two guards -- FSST is skipped when
a cheaper encoding already compressed the vector below three quarters of raw, and
its candidate-counting hash is sized to the sample -- but when FSST *is* the right
encoding (high-cardinality varlena with shared substrings), the table is still
rebuilt from scratch for every vector in the column chunk.

E3 asks: **is that per-vector rebuild a measurable ingestion cost, and if so,
what is the smallest change that removes it without losing ratio?**

## Gate measurement (run first, before any E3 code)

The existing 6M-row benchmark does not exercise FSST at all: its text columns are
low cardinality (`c1` 1000 distinct, `c2` 20 distinct, `c3` constant), so dict
wins and the cost guard skips FSST. The gate therefore needs a targeted load.

Measurement: `bench/run_bench_fsst.sh` (new, non-assert build), loading N million
rows of a **high-cardinality shared-substring** text column (URL / log-line
shaped -- the case FSST is for) into a native columnar table, timing the INSERT
(ingestion is where the per-vector build cost lands). Compare two builds of the
same branch:

- FSST enabled (as shipped).
- FSST disabled (one-line local `return false` at the top of `encode_fsst`),
  rebuilt -- the column then falls to `none`/`dict`, isolating FSST's cost.

The delta is FSST's ingestion cost. Decision rule:

- Delta is small (say < ~15% of total columnar ingestion time on that column, and
  ingestion still comfortably faster than the value it delivers): E3 is **not
  warranted**. Record the number here and stop; the per-vector selector stays.
- Delta is large: implement the targeted fix below (E3b), then re-run the gate to
  confirm the cost is removed and ratio is unchanged.

Also record the compression ratio in both runs, so any E3 change can be shown to
be ratio-neutral (its whole justification).

## Gate result (2026-07-22): E3b warranted

Ran `bench/run_bench_fsst.sh` at 3M rows of high-cardinality shared-substring URL
text, `compression=none`, on `pg17_nc`, A/B against an FSST-stubbed build (both
under concurrent matrix load, so absolute times are inflated but the relative
delta is what matters and is robust):

| build     | columnar INSERT | columnar size | chunks using FSST |
| --------- | --------------- | ------------- | ----------------- |
| FSST on   | 11792 ms        | 106.2 MB      | 20 / 20           |
| FSST off  | 2615 ms         | 324.6 MB      | 0 / 20            |
| heap base | ~1550 ms        | 439.3 MB      | --                |

- **Cost:** FSST adds 9177 ms, 78% of columnar ingestion time and a 4.5x
  slowdown, on an FSST-heavy column. Decisively past the gate threshold.
- **Ratio:** FSST buys 3.06x (324.6 -> 106.2 MB), even with no block codec -- a
  win worth keeping. Round-trip MATCH in both runs.
- **Amortization headroom:** ~20 column chunks over 3M rows is ~150k rows/chunk,
  ~146 vectors/chunk. The symbol table is rebuilt once per vector today; building
  it once per chunk is ~146x fewer builds, which should return ingestion to near
  the FSST-off baseline while keeping the 3x ratio.

Decision: implement **E3b** (chunk-shared FSST symbol table). E3a is not pursued
now; it does not remove the per-vector build, which is where the measured cost is.

## Post-implementation measurement (2026-07-22): correction

Re-running the gate under clean, matched conditions (no concurrent matrix load)
against E2, E3b, and an FSST-off build corrected the cost attribution the first
gate got wrong. The first gate ran under matrix contention, which inflated the
absolute times and, worse, led to attributing FSST's cost to the table build. The
clean numbers (3M rows, high-cardinality URL text, compression=none, pg17_nc):

| build (clean)        | columnar INSERT | size     |
| -------------------- | --------------- | -------- |
| FSST off (baseline)  | 2777 ms         | 324.6 MB |
| E3b, 256 KB sample   | 10329 ms        | 105.8 MB |
| E3b, 32 KB sample    | 9550 ms         | 111.2 MB |
| E2 (per-vector table)| 11087 ms        | 106.2 MB |

FSST's cost decomposes as roughly **82% per-vector greedy-match compression**
(6773 ms, which E3b does NOT change) and only **18% table build** (1537 ms, which
E3b amortizes). So E3b's ingestion win is real but modest, not the ~4x the first
(confounded) gate implied. With the small 32 KB sample E3b was 14% faster but 4.7%
larger (a less representative shared table gives looser codes). Training the shared
table on a larger 256 KB sample -- affordable because the build is now amortized
~146x per chunk -- makes E3b **strictly better than E2 on both axes**: ~7% faster
ingestion and a slightly smaller result (105.8 vs 106.2 MB), so E3b ships with the
256 KB sample. E3b also scales better than E2 as vectors-per-chunk grows (E2
rebuilds per vector; E3b builds once per chunk).

The real lever for FSST ingestion cost is the per-vector greedy-match compression,
not the build. That is future work (faster matcher, or the asynchronous
compaction lever below), and is why E3b is a refinement rather than a fix for the
whole cost.

## Design options the measurement chooses between

### E3a. Sample-based selection (general, BtrBlocks-style)

Sample ~1% of a column chunk's vectors, run the full candidate measurement on the
sample, pick the winning encoding, and apply it to every vector in the chunk --
skipping the exhaustive measurement of the losing candidates per vector. This is
a general selector speedup (it saves measuring encoders that were never going to
win) and keeps per-vector encoding descriptors.

For FSST specifically it is only a partial fix: even once FSST is chosen for the
chunk, each vector still *builds its own table*, which is the dominant cost. E3a
helps the mixed-column case (stop measuring dict/rle/for on every vector of a
column FSST always wins) but does not remove the per-vector table build.

### E3b. Chunk-shared FSST symbol table (targeted at the measured cost)

Build one FSST symbol table per column chunk from a sample of the chunk's values,
store it once at the chunk level in the encoding descriptor, and encode every
vector's code stream against that shared table. The table build -- the dominant
cost -- is then paid once per chunk instead of once per vector, while each vector
keeps its own code stream (so per-vector skipping and random access are
unchanged). Ratio is essentially unchanged or slightly better: one table trained
on the whole chunk is at least as representative as many tables trained on 1024
values each, and the per-vector table bytes are no longer repeated.

Cost: a format touch. The per-column-chunk descriptor gains an optional
chunk-level FSST table region that FSST vectors in that chunk reference, instead
of each FSST vector embedding its own table inline. The decoder reads the shared
table once per chunk. This is the invasive part and the reason E3b is gated on
the measurement actually showing a cost worth a format change.

As-built format (from reading the descriptor code). The encoding descriptor is
`[version:1][reserved:1][vectorCount:uint32]` then `vectorCount` entries of
`[encType:1][valueCount:uint32][rawLen:uint32][encLen:uint32]`, with the encoded
value streams concatenated in a separate region (optionally block-compressed).
E3b:
- Bump `COLUMNAR_NATIVE_ENCDESC_VERSION` 1 -> 2 and **append** the shared table as
  a trailing region `[sharedTableLen:uint32][table bytes]` after the per-vector
  entries -- deliberately at the end so every existing per-vector `encType` offset
  is unchanged and current descriptor assertions keep reading the right bytes.
  `sharedTableLen` is 0 for non-varlena chunks and for varlena chunks where FSST
  was not built.
- The table serialization is the existing FSST symbol table `[uint8 nSym][ nSym x
  (uint8 len, len bytes) ]`; an FSST vector's encoded bytes become just the bare
  code stream (no inline table). Inline-table FSST (E2's on-disk form) is replaced
  wholesale; the version bump makes any stray v1 descriptor fail cleanly rather
  than misdecode. No persistent v1 data exists (pre-release, tests regenerate).
- The table lives in `encoding_descriptor` (uncompressed, ~1-2 KB/chunk); the code
  streams still get the block codec. Build once per chunk from a bounded (~64 KB)
  sample gathered across the chunk's vectors.
- Touch points: `ColumnarEncodeChunk`/`ColumnarDecodeChunk` gain shared-table
  params; a new `ColumnarFsstBuildChunkTable` builds+serializes once per chunk;
  the writer builds the table before its per-vector loop and appends it to the
  descriptor; `columnar_native_decode_chunk` reads the trailing table and passes
  it to each FSST vector decode. Only one decode call site exists. `test/pbt` and
  its shim update to build+pass a shared table for the varlena round-trip.

### Recommendation

If the gate shows a real cost, **E3b is the fix** -- it removes the dominant
per-vector build, which E3a does not. E3a is a separate, orthogonal selector
speedup that can be considered on its own merits later; it is not a substitute
for E3b on FSST-heavy ingestion. If the gate shows no meaningful cost, neither is
warranted and the per-vector selector stays as is.

## Tracked alternative: asynchronous / deferred compression

A larger lever for the same ingestion-cost problem: commit the insert in a fast,
lightly-encoded (or uncompressed) write-optimized form, return to the transaction
immediately, and have a background worker rewrite the row groups with the full
cascade afterward. This is the classic write-optimized-store to read-optimized-
store split (Vertica WOS/ROS, SQL Server columnstore delta rowgroups) as prior
art. It hides all encoder latency from the foreground, not just FSST's.

It is not pursued in E3 because it is a substantial subsystem with real costs:
write amplification (data written twice plus WAL), transient ~3x space and slower
interim reads until compaction, and MVCC-safe atomic row-group rewrite -- the same
machinery Phase F (mutation, clustering, MERGE) needs. E3b removes the measured
FSST cost cheaply and synchronously and likely makes async compaction unnecessary
for FSST specifically. If, after E3b, foreground encoding cost is still a problem
(other encoders, larger scale), the async delta-store is the right next step and
belongs with the Phase F compaction work. Tracked here; gated on a post-E3b
measurement, exactly as E3b was gated on the E2 measurement.

## Validation (if E3b is implemented)

- The codec property test (`test/pbt`) stays byte-exact: E3b changes where the
  FSST table lives, not the round-trip contract; a chunk-shared-table variant is
  added to the harness.
- `native_encoding.sh` still asserts FSST is chosen, round-trips text and bytea,
  and shrinks the column; add an assertion that a chunk carries one shared table,
  not one per vector.
- The differential oracle stays green on native tables.
- The gate benchmark re-run shows ingestion cost removed and ratio unchanged.
- Full PostgreSQL 15-19 matrix, assert-enabled, no warnings, before merge.
