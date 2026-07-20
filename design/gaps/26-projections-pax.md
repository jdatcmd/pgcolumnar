# Gap 26: projections / PAX layout

Status: exploratory. Tier: capability. Format change: major (2.2). Effort: very large.

## Motivation

C-Store's central idea is *projections*: keep the same columns in multiple
physical copies, each sorted on a different attribute. A query uses the copy
whose sort order best serves it, which makes range scans, point lookups, and
min/max skipping dramatically more effective (a sorted column has tight,
non-overlapping per-chunk ranges, so skipping prunes almost everything). PAX /
hybrid layouts are a smaller-scope relative that improves cache behavior. This is
the single largest structural lever left, and correspondingly the largest effort.

## Current state

One physical layout per table, in insert order. min/max skipping only helps
columns correlated with insert order; bloom (I7) helps equality on any column but
not ranges. There is no notion of an alternate sort order.

## Design (sketch -- this is a project, not a task)

Two independently shippable pieces:

1. Sorted single-projection (smaller): allow a table (or a `columnar.vacuum`
   variant) to be stored sorted on a chosen key. Implementation: a sorting
   rewrite in the vacuum path (already a full-relation rewrite) that orders rows
   by the key before re-striping. No planner change needed; min/max skipping and
   the encodings (RLE/delta on the now-sorted key) immediately improve. This
   delivers most of the range/skip benefit with far less machinery than full
   projections. Recommended first step.
2. Multiple projections (full C-Store): a table has N projections, each a
   column subset in its own sort order, materialized as separate physical
   storages sharing the row identity space. Requires: catalog for projections
   (columns, sort key, storage id per projection), write fan-out (every insert
   writes all projections), a planner that picks the projection per query and
   reconstructs full rows via row-number join when a projection is a subset, and
   vacuum/index coordination. This is a major subsystem.

PAX/hybrid layout is an alternative to (2) focused on cache locality within a
page rather than multiple sort orders; lower value here since data already lives
in per-column chunk streams.

## On-disk / API impact

Format 2.2: a projections catalog and per-projection storage ids; the metapage
or a new catalog records the projection set. Additive for reads of 2.0/2.1
(single implicit projection). New SQL surface to declare projections.

## Testing

Differential oracle for every query shape against a heap, for tables with 1..N
projections, verifying identical results regardless of which projection the
planner picks; plus write-fan-out correctness (all projections agree),
vacuum/rebuild, and crash recovery per projection.

## Effort / risk

Very large (multiple subsystems: catalog, writer fan-out, planner selection, row
reconstruction, vacuum). Risk: planner integration and keeping N copies
consistent under concurrency and recovery. Recommendation: ship piece (1) sorted
single-projection first as a contained, high-value increment; treat piece (2) as
a separate multi-PR project.

## Source

Stonebraker et al., "C-Store," VLDB 2005 (projections); Ailamaki et al., "Weaving
Relations for Cache Performance" (PAX), VLDB 2001.
