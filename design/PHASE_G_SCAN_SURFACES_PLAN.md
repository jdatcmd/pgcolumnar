# Phase G scan surfaces: read_parquet(), FDW, predicate pushdown

Follows the scan core (open/parse, type inference, `parquet_schema`) landed on main.
Three sequential PRs, each independently reviewable and PG17-gated during development;
the full 15-19 matrix runs once after all three are submitted.

## Shared row engine (PR1 refactor)

The import path already decodes a Parquet file into rows and assembles nested shapes
(scalar, 1-D array from LIST, composite from group) via Dremel level walking. That
row-producing loop is the scan core; it was coupled to a target `Relation` only for
inserts. PR1 factors it into a Relation-free engine both surfaces call:

- `build_imp_targets(TupleDesc, PqFile*, ...)` loses its `Relation` parameter; on a
  binding error it just `ereport`s (transaction abort releases any caller lock), so a
  caller with no relation (the function, the FDW) can bind a tuple descriptor against a
  file the same way import binds a table's descriptor.
- `pq_read_rows(pf, filebuf, filelen, tops, ntops, leaves, slot, sink, sinkarg)` runs
  the per-row-group decode plus per-row assembly, fills `slot`, and calls `sink` per
  row. Import's sink is `table_tuple_insert`; the function's sink is
  `tuplestore_puttupleslot`. The sink copies each tuple, so the per-row context reset is
  safe for both.

## PR1: read_parquet(path text) returns setof record

A set-returning function that streams a file's rows in place. The caller supplies a
column definition list (`SELECT * FROM pgcolumnar.read_parquet('f.parquet') AS
t(id int, name text)`); the declared descriptor is bound against the file leaves by
position, exactly as import binds a target table (same type-compatibility rules, same
"file column count must match" contract). Materialize-mode SRF, superuser only (reads a
server-side file).

- Contract: position-based binding, so columns are declared in file order with
  compatible types. Name-based binding is a possible later enhancement.
- Differential oracle: `read_parquet(f) AS t(...)` equals `import_parquet` into a table
  of the same shape, then a native scan.

## PR2: FDW surface

A foreign-data wrapper so a file is a table: `CREATE FOREIGN TABLE ... SERVER ...
OPTIONS (path '...')`. It plans and executes over the same engine, so results match the
function. The scan materializes the file into a tuplestore in BeginForeignScan and
drains it in IterateForeignScan (streaming and column-chunk projection are follow-ons,
folded into PR3's pushdown work). The single required table option is `path`; an option
validator rejects anything else and BeginForeignScan errors clearly on a missing path,
non-Parquet file, or incompatible declared type. Superuser only.

## PR3: pushdown (projection + predicate / row-group skipping)

Two pushdowns land together, since both need the scan core to decode a subset of the
file:

- **Projection** — read only the referenced column chunks, from the scan's `attrs_used`.
  The row engine gains a per-top "needed" mask; unreferenced tops skip decode and stay
  NULL.
- **Predicate / row-group skipping** — the metadata parser currently skips the Parquet
  `Statistics` field, so PR3 first extends the column-chunk parser to read per-group
  min/max and null-count, then turns pushable `col op const` clauses (ordered types)
  into per-row-group skip decisions, with the executor rechecking every returned row so
  a partial or absent pushdown is always correct.

## Not in scope here

Multi-file/directory datasets, Zstd/gzip page codecs, and ORC/Iceberg/Delta remain
follow-ons per PHASE_G_EXTERNAL_PARQUET_PLAN.md.
