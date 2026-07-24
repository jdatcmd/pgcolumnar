# Adaptive cascade encoding selection

Plan written 2026-07-24, before any code. Covers the roadmap's "adaptive,
sample-based cascade encoding selection" item, sourced to BtrBlocks (SIGMOD 2023,
https://dl.acm.org/doi/10.1145/3589263). The roadmap calls it high value, low to
medium effort. This plan disagrees with the second half of that estimate and says
why, then structures the work so the cheap half can land without the expensive
half.

## What exists

`ColumnarEncodeChunk` (`src/columnar_encoding.c`) takes one chunk's raw value
stream and picks an encoding by **running every applicable encoder in full** and
keeping the smallest output. The candidate set already spans the axes: RLE,
frame-of-reference, delta, delta-of-delta, dictionary, Gorilla, ALP, and FSST.
Each encoder bails cheaply when it cannot help.

So pgColumnar already has adaptive per-chunk selection. What it does not have is
either of BtrBlocks' two actual ideas:

1. **Sampling.** Choose the encoding from a small sample instead of encoding the
   whole chunk with every candidate.
2. **Cascading.** Encode the *output* of one scheme with another, recursively:
   dictionary codes are integers and compress further with FOR plus bitpacking;
   RLE run lengths likewise.

These are independent. Cascading is where the size win is. Sampling is a
write-throughput optimization whose value depends entirely on what selection
currently costs, which nobody here has measured.

## Step 1: measure, and let the measurement decide

Do not write a sampling selector before knowing what it saves. The measurement:

- Instrument `ColumnarEncodeChunk` (a temporary build, not committed) to
  accumulate time spent per candidate encoder and total time in selection.
- Load the 6M-row benchmark (`bench/run_bench.sh`, per the standing rule that the
  full-scale bench is what catches scale bugs) and record: selection time as a
  fraction of total write time, and the same per column type, since a wide varlena
  column and a packable int column will differ.
- Report the number honestly, including if it is small. If selection is a few
  percent of write time, sampling is not worth its complexity and this step ends
  the sampling half of the work. That is a legitimate outcome, and cheaper to
  reach now than after building it.

Only if selection is a material fraction does the sampling selector get built:
estimate each candidate's size from a bounded sample (proposal: a strided sample
of about 1024 values, strided rather than prefix so a sorted or clustered column
is not misjudged), keep the top one or two candidates, and encode only those in
full. Correctness is unaffected either way, since the chosen encoding is recorded
per chunk; a worse choice costs size, never correctness. That asymmetry is what
makes sampling safe to tune later.

## Step 2: cascading, which is the actual win

Cascading changes the on-disk format, and that is the part the roadmap's "low to
medium effort" understates.

Today an encoding is one byte, one scheme. A cascade needs the chunk header to
express a *chain*: for example dictionary, then FOR, then bitpack. Options:

- **New codes for fixed useful chains.** For example one code meaning
  "dictionary codes, FOR-encoded". Simple, no format grammar change, but it does
  not compose and each new pairing needs another code.
- **A chain descriptor.** A small count plus a list of scheme bytes, so any chain
  is expressible and the decoder unwinds in reverse. More format work, and it is
  the shape that actually matches the paper.

Proposal: the chain descriptor, bounded to depth 3, with the depth bound enforced
on read as a corrupt-file guard rather than trusted. A file-declared chain length
is exactly the class of field that has produced three crafted-file bugs in the
Parquet reader; it gets a range check before it drives a loop, and a test that
fails without that check.

Format and compatibility, to be settled before code:

- PGCN v1 is the current format line and the reader must reject a chain it cannot
  decode with a clean error, not a wrong value.
- A new build reading old files is unaffected: no chains exist in them.
- An **old build reading new files is not possible**. That is a real
  compatibility break for anyone who has written data with an intermediate build,
  and it needs an explicit decision: either bump the format version so the failure
  is clear, or gate cascading behind a per-table option that defaults off until a
  version bump. Recommend the version bump, because a silent "your old binary
  cannot read this table" is worse than a loud one.
- Candidate chains worth measuring first: dictionary then FOR, dictionary then
  bitpack, RLE run-lengths then bitpack, delta then RLE. Each is a claim about
  data shape and each should be justified by a measured size win on the benchmark
  data before it ships, not by the paper's numbers on their data.

## What this does not include

Morsel-driven parallelism, data-centric JIT, and FastLanes-style vectorized
expression encoding are separate roadmap items. FastLanes in particular is a
format generation, not an increment on this one.

## Gating

Per PR: PostgreSQL 18 and 19. The differential, fuzz, and recovery suites are the
regression net that matters here, because this changes the on-disk value stream:
they compare against a heap oracle and will catch a cascade that decodes to the
wrong values. A full 15 through 19 matrix before the format change merges, and the
6M-row benchmark re-run with before and after numbers in the PR, since the whole
justification is size and speed.
