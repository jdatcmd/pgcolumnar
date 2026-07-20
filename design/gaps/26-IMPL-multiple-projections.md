# Gap 26 (impl): multiple projections (C-Store), format 2.2

Piece (2) of gap 26. Piece (1), sorted single-projection (`columnar.vacuum_sorted`),
ships. This adds N physical projections per table, each a column subset in its own
sort order, sharing the row-identity space — the central C-Store idea.

Status: SPEC. This is a multi-PR subsystem (catalog, write fan-out, planner
selection, row reconstruction, vacuum, recovery) with a format bump to 2.2. It
must be built and merged in phases, each with full differential coverage on a
stable cluster. Reads of 2.0/2.1 tables are unaffected (one implicit projection).

## Model

A projection is a named, ordered subset of the table's columns, stored as its own
columnar storage (its own `storage_id`, stripes, chunk groups, streams) sorted on
the projection's sort key. Every base table has an implicit "base projection"
containing all columns in insert order (today's single storage). Additional
projections are declared by the user. All projections of a table cover the same
logical rows and share the row-number identity space so a subset projection can be
completed by row-number join against a superset projection (usually the base).

Invariant: for every logical row, every projection has exactly one physical row,
and a given row number denotes the same logical row in every projection.

## Catalog (format 2.2)

New catalog `columnar.projection`:

    storage_id      bigint    -- the table's base storage id
    projection_id   int       -- 0 = base, 1..N additional
    name            name
    proj_storage_id bigint    -- this projection's own storage id
    sort_key        int2[]    -- attnums, in sort order (empty = insert order)
    columns         int2[]    -- attnums stored in this projection (base = all)

The metapage records format 2.2 and the projection count; a 2.0/2.1 metapage
implies a single base projection. Additive: old readers ignore the new catalog.

New SQL surface (shape, not final):
`columnar.add_projection(rel, name, columns text[], sort_key text[])`,
`columnar.drop_projection(rel, name)`.

## Write fan-out

Every INSERT / multi_insert / bulk load writes the base projection as today, then
writes each additional projection: project the tuple to the projection's columns
and append to that projection's write buffer. Additional projections are stored
sorted, so they cannot simply append in insert order like the base — options:

- **Buffer-and-sort at flush**: accumulate a projection's rows in the write buffer
  and sort by the projection sort key when the stripe flushes. Cheap on load,
  gives per-stripe (not global) sort order — enough for min/max skipping within a
  stripe. This is the pragmatic choice and mirrors how load-then-`vacuum_sorted`
  already works.
- **Global sort via vacuum**: `columnar.vacuum` re-sorts each projection globally,
  as `vacuum_sorted` does for the single projection today.

Row numbers are assigned once (by the base projection) and carried into each
projection's rows so identity is shared; a projection stores the row number
alongside its columns (an implicit system column in its streams) to support
reconstruction and delete propagation.

## Delete / update

`ColumnarMarkRowDeleted` marks a row number deleted; the mark must apply to *all*
projections (each has a row-mask keyed by the shared row number). Update =
delete + insert, fanned out to every projection. The row mask is keyed by the
base row number in every projection so a single delete marks the row dead
everywhere.

## Planner selection and reconstruction

- For a scan, the custom scan provider chooses the projection whose sort key and
  column set best serve the query (predicate on the sort key → tight min/max
  skipping; projection covers all referenced columns → no reconstruction).
- If the chosen projection is a subset missing some referenced columns, the
  remaining columns are fetched from the base projection by row number (a
  row-number lookup, the same `ReadRowByNumber` path index fetch uses). The
  planner weighs the reconstruction cost against the skipping benefit.
- Default when no projection helps: the base projection (today's behavior).

## Vacuum / rebuild

`columnar.vacuum` must rewrite every projection consistently, re-deriving row
numbers so all projections stay aligned, and re-sorting each per its key.
`vacuum_sorted` becomes a special case (base projection sorted). Dropping a
projection frees its storage.

## Concurrency / recovery

Every projection is written under the same transaction as the base insert, so a
crash leaves either all or none of a row's projection copies (the stripe catalog
rows commit together). Recovery replays each projection's stripes with the base.
The differential suite must confirm all projections agree after concurrent
inserts/deletes and after crash recovery.

## Testing

Differential vs heap for every query shape, on tables with 1..N projections,
asserting identical results regardless of which projection the planner picks;
write-fan-out agreement (each projection, read alone by row number, reproduces the
base row); delete/update propagation to all projections; vacuum/rebuild alignment;
per-projection crash recovery; `EXPLAIN` shows the chosen projection.

## Phasing (each a tested PR)

1. Catalog + `add_projection`/`drop_projection` DDL; format 2.2 metapage; base
   projection recorded explicitly. No write fan-out yet (projections empty).
2. Write fan-out with buffer-and-sort at flush; row-number sharing; delete
   propagation. Projections populated but not yet used by the planner.
3. Read path: read a projection by itself; row-number reconstruction from base.
4. Planner selection: choose a projection per query; cost model; `EXPLAIN`.
5. Vacuum coordination across projections; global re-sort.
6. Full differential + concurrency + recovery; enable by default.

## Effort / risk

Very large. Primary risks: keeping N copies consistent under concurrency and
recovery, and planner integration. Recommendation: land phases 1-2 first (catalog
+ fan-out, projections written and verified equal to base by row number) before
any planner change, so correctness is established before the optimizer depends on
it.
