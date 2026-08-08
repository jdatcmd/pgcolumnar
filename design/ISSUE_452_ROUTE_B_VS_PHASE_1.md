# #452: Route B against Phase 1, decided by measurement

Owner asked for the two courses to be tested against each other rather than
argued. This is the experiment design, written **before** any measurement so the
attribution is pre-committed and cannot be chosen after seeing the numbers.

Companion to `ISSUE_452_LATE_MATERIALIZATION.md`, which holds the architecture.

## The two courses

- **Phase 1** (1a + 1b): qual columns materialized first, `ExecQual`, then
  payload columns only for surviving rows; plus exact selection fed into the
  existing per-vector skip. **No storage-format change.**
- **Route B**: per-vector compression blocks, so a vector can be decompressed
  independently. Costs **+29% to +58%** on disk by the reviewer's measurement,
  which this experiment re-derives rather than trusts.

They are not exclusive. The question is which is worth doing, and whether
Route B's storage cost buys anything once Phase 1 exists.

## The question that decides it

For the shape that actually loses — the ClickBench wide-text queries, q21-q24 —
what fraction of scan time is:

| bucket | what removes it | route |
| --- | --- | --- |
| **D** block-codec decompression (zstd) | decompressing fewer vectors | **Route B only** |
| **E** per-vector decode (encoding -> raw buffer) | skipping vectors with no selected row | **1b** (already has the skip machinery) |
| **M** per-row materialization (deform, `MemoryContextAlloc` + memcpy per varlena) | not materializing rejected rows | **1a** |

**Pre-committed decision rule**, fixed now:

- If **D < 20%** of scan time, Route B cannot repay +29-58% storage on this
  workload and Phase 1 is the course. Do Phase 1, defer Route B.
- If **D > 50%**, Phase 1 leaves the dominant cost untouched and Route B is
  required regardless of storage. Cost it properly and take it to the owner.
- Between 20% and 50%, do Phase 1 first and **re-measure D afterwards**, because
  Phase 1 removes E and M from the denominator and D's share necessarily rises;
  the decision is then made on absolute milliseconds saved per GB spent, not on
  a share.

Storage is a headline number for this project, so the burden is on Route B to
justify itself, not on Phase 1 to rule it out.

## Method

**Fixture: the real thing, not a synthetic stand-in.** The whole question is
about wide text, and `url`/`title` compressibility is the variable. Load
`hits_col` from the existing 11,110,833-row stride extract on the bench
(`/srv/clickbench/hits.every9.tsv`), which is the same table the #445 run
measured. A synthetic fixture would decide this question with invented data.

**Query: q24**, the 8.49x loss and the extreme case:

    SELECT * FROM hits_col WHERE URL LIKE '%google%' ORDER BY EventTime LIMIT 10

with q21 (`SELECT count(*) ... WHERE URL LIKE '%google%'`, one column) as the
control that isolates M — q21 materializes almost nothing, so the difference
between them is close to pure M plus E.

### Method change, recorded before any measurement was taken

**`perf` cannot sample on this box, so the symbol-bucket attribution below is not
available.** Established by performing it, not by reading #505:

    perf stat -e cycles true   ->  "No supported events found. The cycles event is not supported."
    perf record -e cpu-clock   ->  writes /tmp/p.data, 21,400 bytes, "has no samples!"
    /proc/sys/kernel/perf_event_paranoid = -1   (already permissive; not a permissions fix)

Note the shape: `perf record` **succeeded**, produced a non-empty file, and
contained nothing. An instrument reporting success while measuring nothing, which
is the same failure this session has now met nine times, and which #505 predicted
for this host.

**Replaced with a differential design, which is better evidence for this
decision anyway** — it measures the cost each route would *remove*, in
milliseconds, rather than attributing symbols and inferring what a change would
save:

- **D, the decompression cost** = `T(zstd table) - T(none table)` for the same
  query on the same data. `pgcolumnar.compression` takes `none`, and it is a
  per-table option, so two tables can differ in exactly that one property.
  **Route B's ceiling is D times the fraction of vectors holding no selected
  row**, because Route B does not remove decompression, it lets unselected
  vectors go undecompressed.
- **M + E, what Phase 1 removes** = `T(SELECT *) - T(count(*))` under the same
  predicate, which is the cost of the 104 columns that only the wide query
  touches. Measured on the `none` table it excludes decompression, isolating
  decode plus materialization — exactly what 1a and 1b remove.

The decision rule above is restated in these terms: compare **D x unselected
vector fraction** against **M + E**. Both are milliseconds on the same query, so
they are directly comparable, and the storage cost is then priced per
millisecond saved.

Ratios are reported with their baselines, per the standing rule, since the two
tables have different absolute times by construction.

**Superseded plan, kept because it is why the method changed:**
`perf` symbol buckets, from `bench/run_profile.sh`'s probe
(non-assert `pg18n`, DWARF unwind, whatever event the box exposes; #505 records
that this host has no hardware PMU, so the profiler's own report of event and
unwind method must be read and recorded rather than assumed).

Buckets by symbol, decided now:

- **D**: `ZSTD_*`, and any block-codec entry point.
- **E**: `pgcolumnar_native_decode_chunk`, `PgColumnarDecodeChunk`, FSST decode
  (`fsst_*`, `decode_fsst*`), bit-unpacking (`bitunpack*`, per #501).
- **M**: `PgColumnarDecodeValue`, `MemoryContextAlloc`/`palloc`, `memcpy` under
  the per-row producer, slot fill.

Anything unattributed is reported as its own bucket rather than distributed,
because a residual quietly spread across three buckets is how a 70%/16% figure
went wrong in #472.

## Verification, before any number is quoted

Per the standing rule, the instrument is a claim:

1. **The box must be idle** — `uptime`, stray backends, per the #207/#212
   history. Recorded with the result.
2. **Non-assert build** — `pg_config --configure` must not show
   `--enable-cassert`. #504 exists because an assert build put a function that
   does not exist in production at 9.28% of a profile.
3. **The profile must not measure its own fixture.** #504's other artifact:
   `md5_calc` at 12.31% of an "ingest" profile was the generator inside the
   timed statement. The query here reads a pre-loaded table, so the load must be
   outside the sample window.
4. **Sanity bound**: bucket shares must sum to 100% with the residual named, and
   D+E+M must not exceed total scan time. A share above 100% means the buckets
   double-count a symbol.
5. **The two queries must differ in the predicted direction.** q21 (1 column)
   must show far less M than q24 (105 columns). If it does not, the attribution
   is wrong, not the theory.

## Route B's storage cost, re-derived

The +29-58% figure was measured on 122,880 rows of three columns. Before it is
used in a decision it gets re-derived at the scale that matters, on the same
`hits_col`, by re-encoding representative columns with per-vector blocking and
comparing bytes. If it cannot be re-derived without implementing Route B, that
is stated plainly and the decision uses the reviewer's number **labelled as
theirs and unverified**, rather than silently adopting it.

## Result: Phase 1, and Route B is not close

Measured 2026-08-08 on the bench (16 cores, 62 GB, load 0.02), PostgreSQL 18.4
non-assert `pg18n`, on the real 11,110,833-row ClickBench table. Both tables
carry identical data (same row count, same 1,728 matches) and differ only in the
codec, **proven from the catalog** rather than inferred from size:

    hits_col        block_codec 3 (zstd)  7,718 chunks  1288 MB
    hits_col        block_codec 0         157 chunks     119 MB
    hits_col_none   block_codec 0         7,875 chunks  3166 MB

Medians of 3 interleaved tries, milliseconds, **baselines beside the shares**:

| query | zstd | none | D (codec's whole contribution) |
| --- | ---: | ---: | ---: |
| q21 `count(*)`, 1 column | 1237.0 | 1117.7 | 119.3 ms = **9.65%** of q21 |
| q24 `SELECT *`, 105 columns | 6005.2 | 5803.2 | 202.0 ms = **3.36%** of q24 |

Vector occupancy of the predicate: **974 of 10,851 vectors** hold a matching row,
so **91.0% hold none**.

| | ms | share of q24 |
| --- | ---: | ---: |
| **Route B ceiling** (D x 91.0%) | **184** | **3.06%** |
| **Phase 1 target** (q24 - q21, codec removed) | **4686** | 80.7% |
| **Phase 1 ceiling** (target x 91.0%) | **4265** | **71.0%** |

**23.2x more opportunity in Phase 1 than in Route B.** The pre-committed rule
said D < 20% means Route B cannot repay its storage; D is 3.36%.

**The bound is stronger than the estimate, and does not depend on the estimate
being precise.** Route B keeps the codec and changes only the granularity at
which it is applied. Removing the codec **entirely** saves 202 ms on this query.
So no scheme that merely applies the codec more selectively can save more than
202 ms, and skipping 91% of vectors puts it at ~184 ms. That holds even if the
202 ms is wrong.

**Honest about the noise:** D(q24) at 202 ms is the same size as the run-to-run
spread (197.5 ms zstd, 170.9 ms none), so D is at the edge of what these three
tries resolve. Min-against-min gives 227.7 ms (3.87%), the same answer. The
conclusion does not rest on the precision of D, only on it being far below 20%,
and it is an order of magnitude below.

**Route B's storage cost was therefore not re-derived.** The decision does not
turn on whether it is +29% or +58%, because the whole prize is 3%. The
reviewer's figure stays labelled as theirs and unverified, per the plan.

Note the codec buys **2.25x** on this table (3.33 GB to 1.48 GB) for 3.36% of
this query's time. That is a good trade and an argument for keeping it, not
against it.

### Measured after 1a shipped: the 71% was 1a and 1b together, not 1a

1a is implemented and measured on the same table, same box, interleaved, medians
of 3, **with the postmaster restarted so the new `.so` was actually loaded** (the
first attempt measured the old binary and reported plausible numbers; the premise
check caught it):

| query | 1a off | 1a on | saved |
| --- | ---: | ---: | ---: |
| q24 `SELECT *`, 105 columns | 6253 ms | **5184 ms** | **1069 ms, 17.1% (1.21x)** |
| q21 `count(*)`, 1 column | 1378 ms | 1392 ms | none, as designed |

q21 is unchanged because its qual column *is* its only projected column, so the
`deferrable == 0` guard refuses the path. That the number does not move is the
guard working, not the feature failing.

**Why 1069 ms and not the 4265 ms this document predicted.** The prediction
conflated two costs that turn out to be separable in the code:

- `pgcolumnar_native_decode_chunk` decodes a **whole chunk** into a raw buffer at
  group-load time, before any row is produced.
- The per-row work 1a removes is only the copy **out of that raw buffer** into
  the row context -- the `MemoryContextAlloc` plus memcpy per value.

So **1a captures M and leaves E untouched.** The decode already happened. Getting
E requires not decoding the vectors that hold no surviving row, which is exactly
1b, and 1b now has a measured budget rather than an assumed one: roughly the
3200 ms of q24 that 1a does not reach.

This does not change the Route B decision -- D is still 202 ms and Route B still
cannot exceed it -- but it does mean **1b is worth more than 1a was**, and the
71% figure should be read as the pair, not as either one.

### Decision

**Do Phase 1a then 1b. Defer Route B and do not spend the storage.** Re-measure D
after Phase 1 lands, because removing 71% of the runtime necessarily raises D's
*share* — and that rise must not be read as Route B having become worthwhile. It
will be the same 202 ms against a smaller denominator, which is the presentation
trap this project has already met once today.

## What this experiment cannot decide

Whether Route B is worth it for a *different* workload. This measures
ClickBench wide text because that is where #452 was found and ranked. A
narrow-projection scan over compressible integers may attribute completely
differently, and the write-up must say so rather than generalising from one
workload — the failure this session already produced twice.
