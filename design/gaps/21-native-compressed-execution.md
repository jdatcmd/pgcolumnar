# Gap 21: native compressed execution on packed bytes

Status: planned. Tier: performance. Format change: none.

## Motivation

I3 gave "compressed execution" in the sense that the vectorized aggregate folds
each column one run at a time (`ColumnarBlockReader` coalesces adjacent equal
values over the decoded raw stream). That captures the RLE win but still fully
decodes FOR/DELTA/DoD/dict chunks to the raw value stream first. The literature's
headline result (Abadi, SIGMOD 2006: 3-10x) comes from operating on the *packed*
representation without materializing every value -- e.g. `SUM` over a
frame-of-reference block as `base*n + sum(offsets)`, and predicate evaluation on
dictionary codes rather than decoded values.

## Current state

- `columnar_encoding.c`: `ColumnarDecodeChunk` always reconstructs the full raw
  stream; `ColumnarBlockReader` iterates that stream.
- `columnar_vector.c`: `columnar_apply_run` sums `value * runLength`; it never
  sees the packed form.

## Design

Add an operator-facing block API alongside the run iterator, one implementation
per encoding, exposing what an operator needs without full decode:

- `ColumnarBlockSum(block) -> int128/numeric` for FOR/DELTA/DoD/RLE:
  - FOR: `min * n + sum(unpacked offsets)`; offsets summed from the bit-packed
    body (still unpacks offsets, but no per-value Datum, and no `base` re-add per
    value).
  - RLE: `sum(value_i * run_i)` (already the run path).
  - DELTA/DoD: reconstruct running value with integer adds only (no Datum).
- `ColumnarBlockMinMax(block)` from FOR header (`min`) and a single pass for max,
  or dict dictionary min/max.
- Dictionary predicate pushdown: evaluate `col = const` by finding the const's
  code once, then testing codes (int compares) instead of decoded values; for
  range predicates, precompute the set of matching codes.
- A block "all one value" fast path (FOR width 0, RLE single run, dict single
  code) that answers count/sum/min/max in O(1).

Keep the existing decode path as the correctness fallback and for any
encoding/operator pair without a native kernel. Selection: the aggregate asks the
block for a native kernel; if absent, it falls back to the run/per-row path.

## On-disk / API impact

None on disk. New internal functions in `columnar_encoding.c` and dispatch in
`columnar_vector.c`. No new GUC required (reuse
`columnar.enable_compressed_execution`).

## Testing

Extend the differential suite so every aggregate over every encoding is compared
to the heap oracle with compressed execution on and off (already partly there);
add integer-overflow boundary data (sum near int64 limits routes to numeric, must
match heap). The codec PBT (test/pbt) gains a "sum over decoded == native block
sum" property per encoding.

## Effort / risk

Medium. Risk is arithmetic correctness (overflow, signedness, numeric promotion)
matching PostgreSQL exactly; the differential oracle plus PBT contain it.

## Source

Abadi, Madden, Ferreira, "Integrating Compression and Execution in
Column-Oriented Database Systems," SIGMOD 2006; Abadi PhD thesis (MIT 2008).
