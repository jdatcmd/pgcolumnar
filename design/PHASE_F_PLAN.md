# Phase F plan: mutation and clustering

Status: active, branched off `main` (post E3b). Phase F delivers the native
format spec's mutation and clustering design: first-class delete vectors,
space-filling-curve clustering, and incremental background compaction. Per the
plan-before-code discipline this decomposes the work before touching code, from
a full survey of the current (interim) machinery.

## What already works (interim), from the code survey

Much of the "delete vectors and merge-on-read" design is already functioning
through interim mechanisms flagged in the code as Phase F's to replace:

- **Delete/merge-on-read works.** `pgcolumnar.row_mask` (a catalog table) is a
  per-row-group bitmap, one bit per row in the group. `columnar_tuple_delete`
  marks a bit; `columnar_tuple_update` is delete-plus-insert; the reader ORs the
  masks per row group and skips set rows while keeping vector cursors aligned.
  Buffering, subtransaction discard/promote, deadlock-safe lock ordering, and
  read-your-writes are all handled (`src/columnar_row_mask.c`, `columnar_reader.c`).
  This IS a delete vector; it is just named `row_mask` and carries vestigial
  columns (`stripe_id`, `chunk_id` always 0, `start_row_number`, `end_row_number`).
- **MERGE already works** through the generic update/insert tableam path and is
  tested (`test/native_dml.sh`). Phase F re-bases it on the native delete vector
  but adds no new SQL capability there.
- **Foreground compaction works** but coarsely: `pgcolumnar.vacuum` rewrites the
  whole relation, reading only live rows (so deletes are physically dropped) into
  fresh stripes. `pgcolumnar.vacuum_sorted` adds a one-shot single-key physical
  sort. Neither is incremental, background, or multi-dimensional.

What genuinely does NOT exist yet: a spec-named `delete_vector` catalog keyed by
group number; space-filling-curve (Z-order/Hilbert) clustering; incremental /
background / per-row-group compaction; `INSERT ... ON CONFLICT` (speculative
insert is unsupported).

## Decomposition

Each sub-phase is its own PG17-gated PR into `main` (PG17-only during the work,
full 15-19 matrix at the end per the standing directive).

Ordering note: implementation leads with the genuinely-new functionality (F2
clustering, then F3 compaction, optionally F4 ON CONFLICT), because those are the
real functional gaps. F1 is behavior-preserving spec-alignment cleanup of a path
that already works, so it is valuable but lower priority and is scheduled after
the new features (or whenever a clean-catalog break is convenient); F3 wants the
`delete_vector` naming, so F1 lands before or with F3.

### F1. Native delete vector (catalog alignment) [rename DONE; column cleanup deferred]

Status (2026-07-23): the rename half landed. `pgcolumnar.row_mask` is now
`pgcolumnar.delete_vector` (table, sequence, indexes), the C module
`columnar_row_mask.c` is `columnar_delete_vector.c`, and every `row_mask` /
`RowMask` / `rowmask` identifier is renamed to the delete_vector form. This was
kept deliberately small and behavior-preserving: the vestigial columns
(`id`, `stripe_id`, `chunk_id`, `start_row_number`, `end_row_number`) and the
row-number-range keying are UNCHANGED, and the spec column names (`group_number`,
`bitmap`, `deleted_count`) are not yet applied. Structural half (2026-07-23, done in this PR): the catalog is now the spec's
four columns `(storage_id, group_number, bitmap, deleted_count)` keyed uniquely by
`(storage_id, group_number)`. Dropped the surrogate `id` (and its
`delete_vector_seq` sequence), `chunk_id`, `start_row_number`, and
`end_row_number`, plus the two extra unique indexes. This is safe because no
consumer used those columns: the bitmap is interpreted group-relative from the
row_group's `first_row_number` (e.g. `rowInGrp = rowNumber - rg->firstRowNumber`
in the reader), never from a stored `start_row_number`, and there is exactly one
delete_vector row per group (chunk id always 0). The in-memory delete buffer is
unchanged; only the catalog DTO and I/O changed.


Replace the interim `row_mask` with the spec's `pgcolumnar.delete_vector` (spec
section 11: `storage_id, group_number, bitmap, deleted_count`). Key it by
`group_number` directly instead of by row-number ranges, and drop the vestigial
`stripe_id` / `chunk_id` / `start_row_number` / `end_row_number` columns. The
mechanism (mark, buffer, merge-on-read, subxact discard/promote, lock ordering)
is preserved behaviorally; this is spec alignment plus cleanup, not a rewrite.
Rename the C module `columnar_row_mask.c` accordingly and the metadata accessors.
Low risk: existing DML/oracle suites (`native_dml`, `differential`, `unique_conc`,
`concurrent_diff`) already exercise the mechanism and must stay green. Value:
the catalog matches the published spec and sheds cruft, and group-keyed access is
cleaner for F2/F3.

### F2. Space-filling-curve clustering, eager path [start here]

Implement multi-dimensional clustering ordering rows by a Z-order (Morton)
interleaving over several columns, exposed as `pgcolumnar.cluster(table,
columns)`, superseding the single-key `vacuum_sorted`. Compute a per-row Morton
key from the chosen columns (normalize each to an unsigned ordinal, bit-interleave),
`tuplesort` by it, and rewrite. Hilbert ordering is a follow-on refinement over
the same plumbing. Value: dramatically tighter zone maps and skipping for
multi-column range/point workloads.

LOCK SCOPE (important). This F2 entry point is the EAGER / offline reorg: it
rewrites the whole relation and swaps the relfilenode
(RelationSetNewRelfilenumber), which inherently needs AccessExclusiveLock, exactly
like PostgreSQL's own `CLUSTER` / `VACUUM FULL`. That blocks all reads and writes
for the duration and is NOT acceptable as the routine maintenance path. It is
useful only as an initial bulk reorg. The routine path is the LAZY one in F3.
This constraint applies equally to the existing `vacuum` and `vacuum_sorted`.

### F3. Incremental compaction and reclustering, LAZY path (required)

The lazy counterpart to F2's eager reorg, and the primary production maintenance
mechanism. Per-row-group, incremental, MVCC-safe rewrite that holds only
ShareUpdateExclusiveLock (concurrent reads AND writes allowed), like PostgreSQL's
regular VACUUM -- never a table-wide AccessExclusiveLock. It retires delete
vectors (drops fully-deleted row groups, rewrites groups past a deleted-fraction
threshold), and MAINTAINS clustering order incrementally rather than only via the
one-shot eager reorg. Invocable per-group and hookable from autovacuum. This is
also the MVCC-safe row-group-rewrite machinery the asynchronous-compaction
wishlist (ROADMAP.md) depends on. REQUIRED, not optional: every maintenance op
must offer a non-AccessExclusive lazy path (see the lazy-option requirement).
Value: bounded compaction cost, space reclaimed and rows reclustered online, and
the foundation for deferred compression.

### F4 (optional). INSERT ... ON CONFLICT and first-class MERGE

Speculative insert is currently unsupported, so `INSERT ... ON CONFLICT` fails.
Implement speculative insert/complete over the delete-vector path and confirm
`MERGE` remains robust. Value: closes a real DML gap. Scoped optional; ordered
last because MERGE (the spec's named case) already works.

## Validation (each sub-phase)

- The differential oracle stays green on native tables (`differential.sh`,
  `fuzz.sh`, `native_dml.sh`), plus the concurrency suites (`concurrent_diff`,
  `unique_conc`) since mutation runs on many connections.
- New coverage per sub-phase: F1 asserts the `delete_vector` catalog shape and
  delete/update/MERGE round-trips; F2 asserts clustering tightens zone maps and
  round-trips; F3 asserts incremental compaction retires delete vectors and
  reclaims space while preserving results.
- `corruption.sh` / `hardening.sh` confirm a corrupt delete vector or clustering
  descriptor raises a clean error, not a crash.
- PG17 gate during the work; full 15-19 matrix at the end.
