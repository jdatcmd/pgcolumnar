# Issue #452: late materialization

Decode cost scales with rows scanned rather than rows emitted. This is the plan,
written before any code, per the house rule.

Status: **plan only, nothing implemented.** Read `CONTEXT.md` first for the
vocabulary; note especially that a stripe is a row group and that EXPLAIN's
"Chunk Groups" counters count ROW groups.

## What is actually there today, verified in the source

Established by reading, not assumed. Line numbers are `main` at `ed28c32`.

- **A chunk is one (row group, column).** `NativeColumnChunkMetadata`
  (`columnar.h:254`) carries `groupNumber` and `columnIndex` and **no chunk-group
  index**. There is no smaller addressable unit on disk than a column's whole
  row group.
- **Columns are independently addressable.** The group loader builds per-chunk
  byte ranges and skips unwanted columns before reading
  (`columnar_reader.c:1271`), so column projection already avoids I/O.
- **A chunk's payload is per-1024-value vector streams concatenated, then
  block-compressed as a whole** (`pgcolumnar_native_decode_chunk`, `:647`).
  Decode reverses the codec once, then decodes each vector separately. So
  **vectors are individually decodable after decompression**, and the FSST
  shared symbol table is a chunk-level trailing region that is available to every
  vector once the descriptor is parsed.
- **Per-vector skipping already exists** (Phase D5b, `:1115`) and already avoids
  *decode*: `nativeSkipVec[v]` marks a vector ruled out, and `nativeVecRawLen`
  steps the value cursor past it, so its rows are "neither decoded nor emitted"
  (`:137-144`).
- **But it is driven only by per-vector zone maps.** `build_skipvec` rules a
  vector out when a predicate's min/max proves no row can match. Nothing
  evaluates the predicate against decoded values.
- **Reader predicates are btree-only.** `SkipPredicate` (`columnar_reader.c`)
  holds a strategy, a constant and a comparison function. `URL LIKE '%google%'`
  is never a `SkipPredicate`, so for q24 `numPredicates` is **0** and no
  reader-level mechanism can help it at all.

## Three separable costs, and only one needs a format change

The issue treats decode as one cost. It is three, with different fixes:

| cost | granularity available today | needs format change |
| --- | --- | --- |
| 1. block-codec decompression | whole chunk (row group x column) | **yes** |
| 2. per-vector decode | 1024 rows, already skippable | no |
| 3. per-row materialization (deform, palloc+memcpy per varlena) | per row | no |

The scoping comment on the issue costed only a fix for **cost 1** (Route A,
+169% storage; Route B, +29-58%) and correctly called that an owner decision.
**Costs 2 and 3 are the larger share for wide text and need no format change**,
so the owner decision is not on the critical path and should not block this.

## Phase 1a: qual-column-first materialization (the heap analogue)

This is the one that fixes q24 and it does not live in the reader.

Heap's advantage, stated exactly in the issue: `slot_deform_heap_tuple` stops at
the qual's attribute for rejected rows and pays the full 105-attribute deform
only for survivors. The analogue here:

1. Split the projected columns into **qual columns** (those referenced by the
   node's qual) and **payload columns** (the rest of the targetlist).
2. Materialize qual columns only, per row.
3. Run `ExecQual`.
4. Materialize payload columns **only for rows that survive**.

Properties worth stating because they decide the design:

- **It works for any qual, including ones that cannot be pushed down.** The
  custom scan holds the qual as an `ExprState` regardless of whether the reader
  would admit it as a `SkipPredicate`. This is why it, and not the reader route,
  is what reaches q24.
- It is orthogonal to #426. #426 makes text predicates *prunable*; this makes
  them *cheap to evaluate*. Either helps; together they compound.
- For fixed-width by-value columns, positional access into the decoded raw
  buffer is `row * attlen`. For varlena the cursor must still be walked to
  advance, but walking a length prefix is not the cost — the cost is the
  `MemoryContextAlloc` plus memcpy per value, and that is what gets skipped.

### Corrected after reading the producer: no batching, and no random access

The paragraph below was written before tracing the code and is **wrong in a way
that made 1a look harder than it is.** Recorded rather than deleted, because the
correction is the useful part.

Each column carries its **own** cursor — `nativeValueCursor[natts]`
(`columnar_reader.c:135`), advanced per column inside the per-row loop. The order
in which columns are visited *within a row* is therefore free. So 1a needs
neither a batch restructure nor random access:

1. Visit the **qual columns** for this row, decoding their values.
2. `ExecQual`.
3. On false: **advance the payload cursors without decoding** and move to the
   next row. For a by-value column that is `+= attlen`; for a varlena it is
   reading a length prefix and adding it — no `MemoryContextAlloc`, no memcpy,
   which is the cost being removed.
4. On true: decode the payload columns normally and return the row.

`rs->rowInGroup` is shared and must not advance until both phases are done, which
is the only sequencing constraint.

This is exactly heap's `slot_deform_heap_tuple` behaviour: stop at the qual's
attribute for rejected rows, pay the full deform only for survivors.

**Where the qual is evaluated today, and what has to change.**
`PgColumnarExecCustomScan` delegates to core's `ExecScan`
(`columnar_customscan.c:1763`), which calls `PgColumnarScanNext` for a **fully
materialized** tuple and only then applies the qual. So 1a moves qual evaluation
inside `PgColumnarScanNext`, which loops until a row passes.

**Consequence that needs a guard:** core's `ExecScan` will then evaluate the same
qual a second time on the returned tuple. Harmless for an immutable qual, wrong
for a volatile one, which would be evaluated twice per surviving row. So 1a is
enabled only when `contain_volatile_functions()` is false for the qual, and that
guard needs its own test.

**Superseded, kept for the record:** the reason this looked harder — the reader
currently exposes a per-row sequential producer that fills `values[]`/`nulls[]`
for all wanted columns in one pass (`:1640-1690`). Phase 1a needs that split into
two passes over the same decoded group. The cursors are per column
(`nativeValueCursor[]`), so a second pass is feasible, but a varlena payload
column cannot be re-walked from an arbitrary row without either retaining
per-row offsets or walking from the vector boundary. Retaining per-vector start
offsets already exists (`nativeVecStart`); per-row offsets within a vector do
not. **Walking from the vector start (at most 1023 length-prefix steps, no
copies) is the cheap answer and should be measured before anything more clever.**

## Phase 1b splits in two, and only one half needs batching

Established by reading `pgcolumnar_native_load_group`, not assumed:

```
:1598   pgcolumnar_native_decode_chunk(...)      per wanted column
:1615   pgcolumnar_native_build_skipvec(...)     from zone maps
```

**Decode runs before the skip vector exists.** So today even a vector the zone
maps have ruled out is fully decoded; `nativeSkipVec` only makes the row producer
step its cursors past it (`:1686`). "Vectors Skipped" means "not turned into
Datums", never "not decoded". That is a stronger statement than #452's text,
which says only that decompression is not skipped.

### 1b-i: let decode honour the skip vector it already builds

Self-contained, and a prerequisite for 1b-ii. Move `build_skipvec` above the
decode loop (it reads zone maps and needs no decoded data) and give
`pgcolumnar_native_decode_chunk` the mask, so ruled-out vectors are never
decoded. **No batching, no restructure, no new observable** — `Columnar Vectors
Skipped` already reports the count and the suites already assert on it.

Worth being explicit that **this does nothing for ClickBench q24**, whose qual is
a leading-wildcard LIKE and therefore yields `numPredicates == 0` and a NULL skip
vector. It pays on queries with a usable predicate, which is most analytic
filters and none of the six #445 losses.

#### 1b-i is larger than it looks: the vectorized fold reads the raw buffer

Found before writing the code, and it is the reason 1b-i was not attempted in the
same session as 1a.

Skipping a vector's decode leaves a **hole** in the chunk's raw buffer. The row
producer never reads it, because it steps its cursors past skipped vectors. But
the batch-fold path does:

- `PgColumnarReadFoldColumn` (`columnar_reader.c:1953`) hands out the raw buffer
  pointer and the per-vector lengths, with no skip information.
- `columnar_vector.c:3161` fetches `skipVec` into a local and **never indexes
  it** — `skipVec[` does not appear in that file.
- The fold stays correct anyway because it **evaluates the scan keys itself**,
  per value (`columnar_vector.c:3236-3242`). That is exact filtering, so a
  ruled-out vector's rows are excluded by evaluation rather than by the skip.

So today the fold reads vectors the zone maps ruled out, and gets the right
answer because it re-checks every value. **Leave a hole and it re-checks
uninitialized memory**, which can only produce a wrong aggregate — silently, and
only on data whose zone maps rule something out.

**1b-i therefore requires the fold to honour the skip as well**, and that is a
change to the vectorized aggregate path, not just the loader. It is still worth
doing, and it is no longer a small self-contained slice. The eligibility rule
(`pgcolumnar_batch_shape_eligible` requires every qual to be convertible to a
scan key) means the fold runs precisely when predicates exist — which is exactly
when vectors get skipped, so this is the common case, not an edge.

A suite was written for 1b-i and is red against `main` for the right reasons: 30
of 32 vectors skipped, no `Columnar Vectors Decoded` counter. It is not in the
tree, because a registered red suite or an unregistered stray `.sh` are both
worse than a specification. Its shape, to rebuild:

- one row group of 32 vectors, monotonic `id` so zone maps are tight
- narrow predicate (`id BETWEEN 2000 AND 2050`) against wide (`BETWEEN 1 AND n`)
- premises: narrow skips vectors, wide skips none, the counter exists and moves
- checks: decoded(narrow) < decoded(wide), and the difference equals the skipped
  count exactly — "fewer" alone would pass on decoding one vector less

### 1b-ii: exact selection, which does need batching

To skip decode for vectors holding no *surviving* row, the qual must be evaluated
before the payload columns are decoded — and the qual can only be evaluated on
decoded qual columns. So the producer has to work a vector at a time: decode the
qual columns, evaluate 1024 rows, decode the payload for that vector only if any
survived, emit.

This is the batching 1a turned out not to need, and it is the larger piece. It is
also the only one of the two that reaches q24, where the measured budget is the
~3200 ms 1a leaves behind.

**Order matters:** 1b-i first, because it is small, independently valuable, and
its mask plumbing is what 1b-ii extends from zone-map-derived to
evaluation-derived.

## Phase 1b (original sketch): exact selection feedback into the existing skip vector

Cheap, additive, and it fixes the issue's own headline test case.

Where a predicate *is* admitted as a `SkipPredicate`, evaluate it against the
decoded qual column and extend `nativeSkipVec` from "zone maps could not rule
this vector out" to "no row in this vector actually matched". Payload columns
then skip decode for those vectors through the machinery that already exists.

This is what makes the issue's stated acceptance check pass:

- `clienttimezone = 3600` matches zero rows of 11,110,833 but lies inside every
  min/max, so today every vector is admitted and all 105 columns are decoded.
  With exact feedback, zero vectors survive and the payload columns are never
  decoded.
- It does nothing for q24, whose qual is not a `SkipPredicate`. That is Phase 1a.

## Phase 2: per-vector compression blocks (deferred, owner decision)

Only needed to avoid cost 1. Route B (+29-58% storage) keeps the codec and makes
vectors independently decompressible. **Not required for Phases 1a/1b**, and it
should be re-costed after them, because if 1a and 1b remove most of the decode
and materialization cost, the remaining decompression may not be worth 29-58% of
the storage headline. Do not decide this until 1a is measured.

## Acceptance checks

From the issue, restated as assertions a suite can make. Each needs a removal
proof, and per the house rule the proof must fail for the stated reason.

1. **The complements must diverge.** `clienttimezone = 3600` (pushed down, zero
   matches) and `clienttimezone <> 3600` (not pushed down, all rows) currently
   read 1351 blocks each. After 1b the first must read materially fewer.
2. **`SELECT *` with a zero-matching pushed-down qual must approach the
   `count(*)` figure**, not 134x it (181,103 blocks against 1351).
3. **A qual that cannot be pushed down must still get 1a's benefit**, which is
   the q24 case and the one no reader-level check can see. Assert on rows
   materialized rather than blocks read, since 1a does not change I/O.

Check 3 is the one most likely to be written vacuously: blocks read will NOT
move for 1a, so a check written against `BUFFERS` will pass unchanged with the
feature removed. It must assert something 1a actually changes.

## Risks

- **A second pass over a decoded group must not change results.** The existing
  per-vector skip already proves rows in skipped vectors are never emitted;
  splitting materialization must preserve that, including for `nulls[]` on
  non-projected columns, which are deliberately set to an explicit NULL rather
  than a missing-value default (`:1652`).
- **`EXEC_FLAG_SKIP_TRIGGERS` / qual side effects.** A qual containing a volatile
  function must not be evaluated a different number of times than today.
- Vectorized aggregate paths (`columnar_vector.c`) build their own selection and
  must not double-apply a selection built here.
