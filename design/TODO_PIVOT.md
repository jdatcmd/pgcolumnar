# pgColumnar pivot: TODO

Status: durable, uncommitted (kept in the working tree, not in git history) while
the provenance pivot is in progress. Authoritative task list for the pivot from a
Citus/Hydra-compatible reimplementation to an original engine. Full rationale and
design are in [DESIGN_PIVOT_ORIGINAL_ENGINE.md](DESIGN_PIVOT_ORIGINAL_ENGINE.md);
research-backed feature directions are in [ROADMAP.md](ROADMAP.md) "Future
directions". Both of those are also uncommitted.

## Decisions made (owner, 2026-07-21)

- Storage-format compatibility with Citus/Hydra is not required.
- SQL API compatibility with Citus/Hydra is not required.
- Legal review is not required.
- Project name stays **pgColumnar**. SQL namespace becomes **`pgcolumnar`**
  (schema, extension, access method, GUC prefix, functions), replacing the
  Hydra-mirrored `columnar`.

## Repository strategy (decided 2026-07-21, owner)

- Keep the existing repo `github.com/jdatcmd/pgcolumnar`. Nothing about the
  1.0-dev work is disowned; it was a valid practice round.
- The `v1.0-dev` tag permanently preserves the practice-round snapshot, so no
  history needs to be frozen to keep it. `main` is kept on the 1.0-dev line only
  as an installable stable line during the transition, not for preservation.
- **`re-origination` is a long-lived integration branch** off `main`. Each phase
  below lands as its own short-lived, matrix-gated PR **into `re-origination`**
  (same per-PR discipline as the 1.0-dev work), not as one accumulating branch.
- A single merge `re-origination` -> `main` happens at the end, once the pivot is
  stable and the owner approves. `re-origination` is meant to become `main`, not
  to fork away permanently.
- Commits and pushes to `re-origination` and its phase branches are fine. Do NOT
  merge to `main` until the end.

## Phases (each matrix-gated; none reads upstream source)

- [x] **Phase A. Provenance reset (docs only).** Done (PR #51 into
  `re-origination`). PROVENANCE.md reframed with a re-origination section and log
  entry; REWRITE_PLAN.md and FORMAT_AND_INTERFACE_SPEC.md marked as the 1.0-dev
  record and superseded as the build source; build source repointed at the new
  spec (Phase B).
- [ ] **Phase B. New format and catalog specification.** Design the on-disk
  format, catalog, and SQL surface fresh from the research (FastLanes vectors,
  BtrBlocks cascade + adaptive selection, ALP, FSST, SMA zone maps, delete
  vectors, opt-in block compression, space-filling clustering) and the open
  Arrow/Parquet/ORC specs. Review against clean-room rules before coding.
- [ ] **Phase C. Rename and re-namespace to `pgcolumnar`.** Extension, schema,
  access method, GUC prefix, functions. Mechanical, fully matrix-gated; keeps the
  current format for now and yields a running engine under the new namespace.
- [ ] **Phase D. New format core.** Vector-based, cascade-encoded format with
  adaptive selection, SMA zone maps, opt-in block compression, as a new format
  version behind the existing reader/writer interfaces. Differential oracle green
  throughout.
- [ ] **Phase E. New codecs.** Add ALP (floats/decimals) and FSST (strings) as
  cascade primitives; adaptive selector chooses among the full set.
- [ ] **Phase F. Mutation and clustering.** Replace the row_mask with native
  delete vectors and merge-on-read; add space-filling-curve clustering with
  background reclustering; wire up `MERGE`.
- [ ] **Phase G. Interop extension.** Extend Arrow/Parquet interop toward reading
  external Parquet and open-table-format (Iceberg/Delta) files with predicate and
  projection pushdown.

## Independent later work (not gated on the format reset)

- [ ] Morsel-driven parallelism.
- [ ] Data-centric JIT with adaptive interpret-then-compile.
- [ ] Join acceleration (runtime bloom filters / sideways information passing;
  hash-join and hash-GROUP-BY pushdown) — needs its own investigation first.
- [ ] PostgreSQL 17-19 integration points (read stream/AIO, MERGE, pg_ivm,
  logical decoding, optimizer-statistics injection, TOAST) — spec before starting.

## Carry-over reminder (from the design doc)

Most of the engine carries over with renaming only (TAM handler, MVCC/row
visibility, IOS visibility-map fork, indexing, unique-insert serialization,
vectorized executor, custom scan, codec algorithms, Arrow/Parquet interop). What
is re-originated: on-disk layout + metapage, the metadata catalog, the SQL
surface, and the specification document.
