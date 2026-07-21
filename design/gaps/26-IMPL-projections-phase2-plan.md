# Multiple projections — phase 2 implementation plan (grounded)

Phase 2 of `26-IMPL-multiple-projections.md`: write fan-out with buffer-and-sort
at flush, row-number sharing, and delete handling. After this phase every
additional projection is populated and verifiably equal to the base by row
number; the planner still does not use projections (phase 4).

## Grounded storage model (from reading columnar_storage.c / columnar_write_state.c)

- **One physical file per table, shared by all projections.** Data bytes live in
  the table relation's main fork. `ColumnarReserveOffset(rel, len)` hands out a
  page-aligned byte range from the metapage's `reservedOffset`; `stripe.file_offset`
  is an offset into that one shared file. So a projection writes its stripes into
  the *same* file as the base, using the shared metapage offset counter — offsets
  stay globally unique across projections for free.
- **Catalog rows are keyed by storage id.** columnar.stripe/chunk/chunk_group
  rows carry `storage_id`; a projection's rows use its `proj_storage_id`, so they
  are naturally separated from the base's while sharing the file.
- **Row numbers are the base's.** `ColumnarReserveRowNumbers` advances the
  metapage's `reservedRowNumber`; only the base does this. A projection reuses the
  base row number assigned to each row (shared identity space) and must NOT
  advance the row-number counter. It still needs its own stripe id + file offset.

## Key simplification vs the spec draft: derive deletes/visibility from base

The spec draft proposed a per-projection row_mask keyed by base row number. But a
projection is sorted by its sort key, so a projection chunk holds an arbitrary,
**non-contiguous** set of base row numbers — which does not fit the row_mask
model (one contiguous [start,end] range per chunk). Instead:

- **A projection stores the base row number of each physical row** (an int8
  "row-number stream", written alongside the column value streams — conceptually
  a leading system column of the projection's chunks).
- **Visibility and deletes come from the base**, not a projection row_mask. When a
  projection is read (phase 3 / planner), each physical row's base row number is
  decoded and checked against the *base* row_mask (deleted?) and base stripe
  visibility (MVCC via the base catalog snapshot). Deleted/invisible rows are
  skipped.
- Therefore **only INSERT fans out.** DELETE/UPDATE touch only the base row_mask
  exactly as today; "a single delete marks the row dead everywhere" holds because
  every projection consults the base row_mask. This removes per-projection delete
  fan-out and its concurrency/recovery risk — the hardest part of the spec draft.
  (Deviation from the spec's "row mask in every projection", chosen for
  correctness+simplicity; recorded here and to be reflected in the spec.)

A projection stripe's own columnar.stripe row commits with the inserting
transaction, so the projection copy appears/disappears atomically with the base
insert (crash leaves all-or-none, same as base). Per-row deletes after the fact
are the base row_mask's job.

## Write fan-out

- **Per-projection write buffers.** Extend the write-state registry to key by
  (relid, subid, projection_id). projection_id 0 is the base (today's path,
  unchanged). Additional projections get their own ColumnarWriteState-like buffer
  with storageId = proj_storage_id, but writing bytes/offsets through the *table*
  relation and NOT reserving row numbers.
- **Buffer-and-sort at flush.** The base encodes incrementally; a sorted
  projection cannot. So a projection buffer accumulates raw projected tuples
  (Datums + base row number) up to stripe_row_limit, then at flush: sort by the
  projection sort key, encode into chunk groups (value streams + row-number
  stream + min/max), reserve a stripe id and file offset from the shared
  metapage, write, and insert catalog rows keyed by proj_storage_id. Memory is
  bounded by stripe_row_limit rows.
- **Insert path.** In columnar_tuple_insert / multi_insert: write the base row as
  today, obtaining its base row number; then for each additional projection,
  project the tuple to (its columns, base row number) and append to that
  projection's buffer. Fan-out reads the projection list once per statement
  (cache on the write state).
- **Flush coordination.** ColumnarFlushWriteStateForRelation and the pre-commit
  flush must flush the base and every projection buffer for the relation.
  Subtransaction abort drops all of the relation's projection buffers too (they
  are keyed by subid).

## Row-number stream encoding

Reuse the existing chunk stream machinery. Add an int8 value stream per chunk
that stores the base row number of each row in physical order. On read, decode it
first to get the row-number vector, then the requested column streams. Store it
under a reserved attnum (e.g. 0) in columnar.chunk rows for the projection's
storage, or as a distinguished stream. Confirm the chunk catalog can represent
attnum 0 (it is `attr_num integer`; base columns are 1..natts, so 0 is free).

## Vacuum / DDL interactions (phase 2 scope)

- `columnar.add_projection` on a non-empty table: phase 1 records the catalog
  row; phase 2 must back-fill the projection from existing base rows (scan base,
  project, buffer-sort, flush) so the projection is complete. Do this under the
  add_projection SUEL. (Alternatively defer back-fill to phase 5 vacuum and mark
  the projection "unpopulated"; decide during implementation — back-fill at add
  is simpler to reason about and test.)
- `columnar.drop_projection`: free the projection's stripes — delete its
  columnar.stripe/chunk/chunk_group rows (keyed by proj_storage_id). The shared
  file's bytes become dead space reclaimed by a later full rewrite; acceptable.
- `TRUNCATE` / table drop: ColumnarDeleteMetadata must also remove projection
  storage rows for all proj_storage_ids of the table.

## Testing (extend test/projections.sh + differential)

- Fan-out agreement: after inserting N rows into a table with projections,
  each projection's storage has N live rows (sum of stripe row_count for
  proj_storage_id), and a low-level reader that scans a projection storage and
  joins its row-number stream back to the base reproduces the base row for every
  column the projection stores. (Add a debug function
  `columnar.read_projection(rel, name)` returning setof record, used by the test
  and reused by phase 3.)
- Sort order: within a projection stripe, rows are ordered by the sort key
  (min/max per chunk is monotonic across chunks within a stripe).
- Delete/update: after deleting/updating base rows, reading a projection (via the
  debug reader, which consults the base row_mask) reflects the deletes — matches
  the base's live set.
- Back-fill: add_projection on a populated table yields a projection whose live
  set equals the base.
- Recovery: crash after inserts (with projections) replays base + projection
  stripes together; live sets agree after recovery.
- Full PG13-19 matrix as the gate.

## Sequencing

Land after phase 1 merges. This is the largest phase; if it grows too big for one
PR, split as 2a (write buffers + fan-out + row-number stream + debug reader +
fan-out agreement tests) and 2b (add_projection back-fill, drop free, truncate,
recovery). Keep the planner untouched until phase 4.
