# Multiple projections — phase 1 implementation plan (grounded)

Phase 1 of the 6-phase plan in `26-IMPL-multiple-projections.md`: catalog +
`add_projection`/`drop_projection` DDL + format 2.2 + base projection recorded
explicitly. **No write fan-out** (projections stay empty); reads unaffected.

## Grounding decisions (from reading the code)

- **Metapage version bump is safe.** `ColumnarReadMetapage` (columnar_storage.c)
  validates only `versionMajor != COLUMNAR_VERSION_MAJOR`; the minor is only
  echoed in error text. Bump `COLUMNAR_VERSION_MINOR` 1 → 2. Old 2.0/2.1 tables
  (major 2) still read. Do **not** add fields to `struct ColumnarMetapage` — old
  metapages wrote only `sizeof(old struct)` bytes; a wider struct would read
  past the written region. Projection count/info lives in the catalog, which is
  authoritative; the metapage change is version-only.
- **Storage ids** come from `ColumnarNextStorageId()` (columnar.storageid_seq),
  already used at table create (columnar_tableam.c:306). Reuse it for
  `proj_storage_id`.
- **Base projection** = projection_id 0, `columns` = all live attnums in attnum
  order, `sort_key` = `{}` (insert order), `proj_storage_id` = the table's base
  storage id (the metapage storageId). Recorded when the table is created.
- **Implicit-base fallback.** Tables created before this extension version (or
  during upgrade) have no `columnar.projection` rows. Treat "no rows for
  storage_id" as "one implicit base projection" everywhere a reader consults the
  catalog. So recording the base row is a convenience, not a correctness
  dependency — matches how a 2.0/2.1 metapage implies a single base projection.

## Catalog (columnar--1.0.sql)

```sql
CREATE TABLE columnar.projection (
    storage_id      bigint  NOT NULL,   -- table's base storage id
    projection_id   integer NOT NULL,   -- 0 = base, 1..N additional
    name            name    NOT NULL,
    proj_storage_id bigint  NOT NULL,   -- this projection's own storage id
    sort_key        smallint[] NOT NULL,-- attnums in sort order ({} = insert order)
    columns         smallint[] NOT NULL -- attnums stored (base = all)
);
CREATE UNIQUE INDEX projection_pkey       ON columnar.projection (storage_id, projection_id);
CREATE UNIQUE INDEX projection_name_idx   ON columnar.projection (storage_id, name);
CREATE UNIQUE INDEX projection_storage_idx ON columnar.projection (proj_storage_id);
```

DDL functions (C-backed):
```sql
CREATE FUNCTION columnar.add_projection(rel regclass, name text,
    columns text[], sort_key text[] DEFAULT '{}') RETURNS void ...;
CREATE FUNCTION columnar.drop_projection(rel regclass, name text) RETURNS void ...;
```

## C (new file src/columnar_projection.c)

Catalog helpers (mirror columnar_metadata.c style: `ColumnarMetadataRelation`,
heap_form_tuple, CatalogTupleInsert, systable_beginscan, ColumnarCatalogSnapshot):
- `ColumnarRecordBaseProjection(Relation rel, uint64 storageId)` — insert
  projection_id 0 with all live attnums; no-op if a row already exists (create
  path may run under CONCURRENTLY-ish retries).
- `ColumnarListProjections(uint64 storageId)` → List of a small struct; used by
  drop + later phases.
- `ColumnarInsertProjection(...)`, `ColumnarDeleteProjection(storageId, projId)`.

SQL entrypoints:
- `columnar_add_projection(rel, name, columns[], sort_key[])`:
  1. Verify `rel` uses the columnar AM (get_rel_relam == columnar am oid), else
     ereport(ERROR, "not a columnar table").
  2. Ensure the base row exists (`ColumnarRecordBaseProjection`), so pre-existing
     tables get their base recorded lazily on first add.
  3. Resolve each `columns` name → attnum via `get_attnum`; reject missing,
     dropped, system, or duplicate attnums. `columns` must be non-empty.
  4. Resolve `sort_key` names → attnums; each must be a member of `columns`.
  5. Reject duplicate projection `name` for this storage_id.
  6. `projection_id` = max(existing) + 1.
  7. `proj_storage_id` = `ColumnarNextStorageId()`.
  8. Insert the row. (No stripes/streams written in phase 1.)
- `columnar_drop_projection(rel, name)`:
  1. Verify columnar table; look up (storage_id, name).
  2. Refuse to drop projection_id 0 (the base): ereport(ERROR).
  3. Delete the catalog row. (No storage to free yet in phase 1; later phases
     free the projection's stripes.)

Register both in the `.c` with `PG_FUNCTION_INFO_V1` and add prototypes to
columnar.h. Add the new object file to the Makefile OBJS list.

## Base-projection recording at create

At the table-create path (columnar_tableam.c, where `storageId =
ColumnarNextStorageId()` is written to the metapage), call
`ColumnarRecordBaseProjection(rel, storageId)` after the metapage is
initialized, inside the same transaction. Guard with the implicit-base fallback
so it is never load-bearing.

## Tests (new test/projections.sh, added to run_all_versions SUITES)

Single cluster; no fan-out yet, so assertions are catalog-shape + DDL semantics:
- Create a columnar table; base projection row exists: projection_id 0, columns
  = all attnums, sort_key = {}.
- `add_projection('p1', ARRAY['a','c'], ARRAY['c'])` → row with projection_id 1,
  columns {1,3}, sort_key {3}, a fresh proj_storage_id distinct from base.
- Add a second projection → projection_id 2; names unique.
- Error cases: duplicate name; unknown column; sort_key column not in columns;
  add_projection on a heap table; drop of the base projection; drop of unknown
  name.
- `drop_projection('p1')` removes exactly that row; others remain.
- New table reports metapage version 2.2 via an existing introspection (or add a
  tiny `columnar.format_version(regclass)` if none exists) — optional; only if
  cheap. Otherwise assert nothing about the metapage (reads already unaffected).
- Row-count sanity guard (cluster-up) per the harness note.

## Sequencing

Build on fresh `main` AFTER IOS phase 5 merges (FM4 green). Files are disjoint
from the IOS change (new SQL catalog + new .c), so low conflict risk. One PR:
"Multiple projections phase 1: catalog + add/drop DDL + format 2.2". Gate on the
full PG13-19 matrix before merge.
