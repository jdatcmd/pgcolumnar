# Gap 26 — first slice implementation plan: sorted single-projection

Implements piece (1) of [26-projections-pax.md](26-projections-pax.md): store a
columnar table physically sorted on a chosen key, via a sorting rewrite in the
existing vacuum path. **No on-disk format change, no planner change, no new
catalog.** This is a one-shot physical reorder (like `CLUSTER`): it is not
auto-maintained; rows inserted after the sort append in insert order until the
next sort.

## Why this is contained

`columnar_compact_relation()` (src/columnar_vacuum.c) already does a full-relation
rewrite: read every live row into a tuplestore → swap to a fresh relfilenode →
write rows back → reindex. Sorting is a drop-in swap of the tuplestore for a
`tuplesort` keyed on the chosen columns. Once rows are stored sorted, min/max
chunk-group skipping and the RLE/DELTA encodings on the sort key improve for free
— no reader/planner changes.

## SQL surface

```
columnar.vacuum_sorted(tablename regclass, VARIADIC sort_columns name[])
```
- Sorts ascending, NULLS LAST (PG default for ASC) on the given columns, in order.
- Errors if: relation is not columnar; no sort columns given; a name is not a
  column of the table; a column's type has no default btree ordering (no `<`).
- Otherwise identical semantics/guarantees to `columnar.vacuum` (transactional
  relfilenode swap, deleted rows reclaimed, indexes rebuilt).
- DESC / NULLS FIRST deferred to a later slice (keep the first cut minimal).

## C changes

- Refactor `columnar_compact_relation(Relation rel)` →
  `columnar_compact_relation(Relation rel, int nsortkeys, AttrNumber *sortAtts)`.
  - `nsortkeys == 0`: current tuplestore path (plain vacuum) — unchanged behavior.
  - `nsortkeys > 0`: use `tuplesort_begin_heap` with per-key `<` operator
    (`lookup_type_cache(atttypid, TYPECACHE_LT_OPR)`, error if `lt_opr` invalid),
    the attribute collation, nullsFirst=false; `tuplesort_puttupleslot` each row;
    `tuplesort_performsort`; `tuplesort_gettupleslot` to write back in order.
- New `PG_FUNCTION_INFO_V1(columnar_vacuum_sorted)` entry point: resolve column
  names → attnums (reject system/dropped/absent), then call the refactored
  compactor with the sort keys. `columnar_vacuum` calls it with `nsortkeys=0`.
- Add tuplesort include (`utils/tuplesort.h`) and typcache include.

## Version portability (PG13–19)

`tuplesort_begin_heap`'s final argument changed at PG16: `bool randomAccess`
(≤15) → `int sortopt` (≥16, use `TUPLESORT_NONE`). Add a compat macro in
`columnar_compat.h`:
```
#if PG_VERSION_NUM >= 160000
#define COLUMNAR_TUPLESORT_NONACCESS TUPLESORT_NONE
#else
#define COLUMNAR_TUPLESORT_NONACCESS false
#endif
```
`tuplesort_puttupleslot` / `performsort` / `gettupleslot` / `end` are stable
across 13–19.

## SQL install

Add the function to `columnar--1.0.sql` next to `columnar.vacuum`, with a COMMENT.
No format/catalog change, so no new upgrade script and no metapage/version bump.

## Testing (differential oracle — results must be identical to a heap)

New `test/sorted_projection.sh`, added to `test/run_all_versions.sh` SUITES:
1. **Correctness / invariance**: build a columnar table + heap mirror; run a
   battery of query shapes (range, equality, ORDER BY, aggregates, `*`) and
   compare order-independent result hashes **before and after**
   `vacuum_sorted` — sorting must never change results.
2. **Physical effect**: after sorting on a scattered column, a narrow range
   predicate on the sort key skips strictly more chunk groups than before
   (assert `Chunk Groups Removed by Filter` increases) — proves the reorder took
   effect. Use `sum()`/filtered `count(*)` so the scan actually runs (the
   covering-count path answers unfiltered `count(*)` from metadata; see phase5).
3. **Reclaim + index**: delete a fraction, `vacuum_sorted`, verify live count,
   sum, a point value, and an index lookup are all correct (row renumbering +
   reindex).
4. **Errors**: non-columnar relation; unknown column; no columns; a
   non-btree-sortable column type.
5. Idempotence: running `vacuum_sorted` twice yields identical results.

Run the fixed full matrix (PG13–19) green before committing/PR.

## Out of scope (later slices)

Persisted sort key / auto-maintenance, DESC/NULLS FIRST, multiple projections
(piece 2, format 2.2), planner projection selection.
