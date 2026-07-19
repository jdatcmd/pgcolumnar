# Gap 24: explicit SIMD kernels

Status: planned. Tier: performance. Format change: none.

## Motivation

I6's typed predicate loops (`VEC_CMP` in `columnar_vector.c`) and the aggregate
fold loops are written to be auto-vectorizable, but they rely on the compiler.
Explicit SIMD (AVX2 on x86-64, NEON on arm64) for the hottest kernels --
comparison-into-selection-vector and sum/min/max over a decoded array -- gives a
further, more predictable per-tuple win, and is a natural pairing with the
run-at-a-time paths.

## Current state

- `columnar_vector.c`: `vecsel_i16/i32/i64/f32/f64` compare a Datum array against
  a constant into `sel[]` with a scalar loop the compiler may or may not
  vectorize; the aggregates fold scalar loops.

## Design

- Add SIMD kernels behind a narrow, well-tested interface, one per (type, op):
  `simd_cmp_i32_lt(const int32 *in, int32 c, uint8 *sel, n)` etc., plus
  `simd_sum_i32`, `simd_min/max`. Selection vector as a byte-per-row `uint8`
  (SIMD-friendly) rather than `bool`.
- Runtime dispatch: detect CPU features once (`__builtin_cpu_supports("avx2")` on
  gcc/clang x86; compile NEON unconditionally on aarch64) and pick the kernel;
  always keep a scalar kernel as the fallback and the reference.
- Values feeding the kernels must be contiguous typed arrays. The decoded vector
  already stores `Datum[]`; for by-value fixed-width types a `Datum` array is
  effectively an array of the widened type -- add a compaction step (or decode
  directly into a typed array) so the kernel sees `int32*`/`float8*` without
  Datum overhead. This is the main plumbing cost.
- Build system: compile the SIMD translation unit with target attributes
  (`__attribute__((target("avx2")))` per function, so the base build stays
  portable and PGXS flags are unchanged).

## On-disk / API impact

None. New `columnar_simd.c` (or kernels within columnar_vector.c) with a scalar
fallback. No format or GUC change required (optional `columnar.enable_simd` for
A/B testing and safety).

## Testing

- The scalar kernel is the oracle for the SIMD kernel: a C unit/property test
  (extend test/pbt) asserts `simd_kernel(x) == scalar_kernel(x)` over randomized
  arrays and lengths, including non-multiple-of-vector-width tails and NaN/Inf
  for floats.
- Differential: all filtered/aggregate queries with `columnar.enable_simd` on and
  off versus the heap oracle.
- Matrix across majors and, ideally, both x86-64 and arm64 (SIMD divergence is a
  known bug class -- see the hegel catalogue).

## Effort / risk

Medium. Risk: SIMD/scalar divergence (especially float NaN/sign, tail handling)
and per-arch behavior; the scalar-oracle PBT is the guard. Portability: keep all
SIMD optional and gated behind runtime detection.

## Source

MonetDB/X100 (super-scalar, SIMD-friendly primitives); standard SIMD kernel
practice.
