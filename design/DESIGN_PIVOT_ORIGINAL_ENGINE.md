# Design pivot: from format-compatible reimplementation to an original engine

Status: proposal, uncommitted. Written 2026-07-21. This document records a
strategic decision and its consequences. It changes no code. It exists so the
decision and its plan are durable while the provenance question is settled.

## 1. The decision

Two constraints were assumed at the project's start and are now lifted by the
project owner:

1. On-disk storage-format compatibility with the Citus and Hydra columnar
   extension is **not** required.
2. SQL API compatibility with Citus and Hydra (the `columnar` schema,
   `USING columnar`, the `columnar.*` function names, the GUC names) is **not**
   required.

Those two constraints were the entire reason the project was built as a
clean-room reimplementation of the Citus/Hydra columnar format and interface.
With both removed, the project is free to become an original columnar storage
engine for PostgreSQL: its own name, its own on-disk format designed from public
research and the open Arrow, Parquet, and ORC specifications, and its own SQL
surface. This document describes that pivot.

## 2. Why the pivot also resolves provenance, and its limits

The current provenance model (see PROVENANCE.md and REWRITE_PLAN.md) is a
two-role clean room: a specification role read the Citus/Hydra AGPL source and
extracted functional facts into design/FORMAT_AND_INTERFACE_SPEC.md, and an
implementation role wrote all code from that specification without reading the
source. Clean-room reimplementation of a file format and an API is a recognized
method, and interfaces and formats are largely not copyrightable, but the result
still deliberately mirrors the upstream format and interface. That mirroring is
what a reader recognizes as "taken from Hydra."

Two honest points:

- A rename and a new API alone would **not** fully clear the concern. The current
  on-disk format and catalog (the stripe / chunk / chunk_group layout, the
  `columnar` catalog tables, the format 2.x version line) are the Hydra-derived
  design even though the code implementing them was written independently.
- What genuinely re-originates the project is designing a **new** on-disk format
  and catalog from public research and the open columnar-format specifications,
  rather than from the upstream design. That is a larger effort than a rename, and
  it is the same effort as the modernization the roadmap already wants. The reset
  and the improvement are one project.

This document is an engineering plan, not legal advice. The owner has decided a
legal review is not required (2026-07-21). The plan below is written to be safe
regardless of that decision: it originates the new format and catalog from public
sources rather than from the upstream design.

## 3. What carries over and what must be re-originated

The engine is more than its format. Most of the machinery is PostgreSQL
table-access-method plumbing and executor integration that was written from the
public PostgreSQL API, not from the upstream format, and it carries over with
naming changes only.

### Carries over (re-namable, not re-originated)

- Table-access-method handler and slot/scan callbacks (`columnar_tableam.c`).
- MVCC and row-visibility model, delete marking, read-your-writes
  (`columnar_row_mask.c`), and the visibility-map fork for index-only scans
  (`columnar_visibilitymap.c`).
- Index support: stable row numbers, synthetic item pointers, btree/hash build
  and fetch, unique-insert serialization (`columnar_unique.c`).
- Vectorized scan, filter, aggregate, late materialization, and the decompressed
  chunk cache (`columnar_vector.c`, `columnar_reader.c`, `columnar_cache.c`).
- Custom scan path, qualifier pushdown, planner integration
  (`columnar_customscan.c`).
- The codec implementations themselves (RLE, FOR, delta, delta-of-delta, Gorilla,
  dictionary, and the block codecs) as algorithms (`columnar_encoding.c`,
  `columnar_compression.c`), which are textbook or paper-derived, not upstream
  expression.
- Arrow and Parquet import and export (`columnar_arrow.c`, `columnar_parquet.c`,
  `columnar_parquet_reader.c`), which were written from the open Arrow and Parquet
  specifications, not from upstream.

### Must be re-originated (designed fresh from public sources)

- The on-disk block and stripe layout, the metapage, and the logical-to-physical
  mapping (`columnar_storage.c`, `columnar_write_state.c`), designed from the
  research in section 5 and the open columnar-format specifications.
- The metadata catalog: table names, column names, and versioning. Today these are
  `columnar.stripe`, `columnar.chunk`, `columnar.chunk_group`, `columnar.options`,
  `columnar.row_mask`, `columnar.projection`. The new catalog is designed for the
  new format and named in the new namespace.
- The SQL surface: extension name, schema, access-method name, GUC prefix, and
  function names.
- The specification document. design/FORMAT_AND_INTERFACE_SPEC.md was extracted
  from upstream and is retired. A new specification is written from the research
  and the open specifications, and the clean-room log is restarted against it.

## 4. Identity and interface re-origination

- **Name.** The project name is **pgColumnar** (unchanged; owner decision,
  2026-07-21). What changes is the SQL namespace, which today is the
  Hydra-mirrored `columnar`.
- **Namespace.** The SQL schema, access method, extension, and GUC prefix move to
  **`pgcolumnar`**: `CREATE EXTENSION pgcolumnar`, schema `pgcolumnar`,
  `USING pgcolumnar`, functions `pgcolumnar.*`, GUCs `pgcolumnar.*`. This removes
  the surface that mirrors the upstream (`columnar`, `USING columnar`,
  `columnar.*`).
- **SQL surface.** Prefer standard PostgreSQL SQL where it exists rather than a
  bespoke function: `ALTER TABLE ... SET ACCESS METHOD`, native `VACUUM` and
  autovacuum for maintenance, `MERGE` for merge-on-read, and reloptions for
  per-table storage settings. Reserve `pgcolumnar.*` functions only for what has
  no standard form (export/import, statistics, explicit reclustering).
- **GUCs.** Prefix `pgcolumnar.`, with names chosen for the engine rather than
  copied from upstream.
- **Terminology.** Use open-standard columnar vocabulary (row group, column chunk,
  page, zone map) from the Parquet/ORC/Arrow specifications rather than the
  upstream catalog vocabulary.

## 5. A native format designed from public research

This is where the project becomes better than a compatible reimplementation. The
format is designed from peer-reviewed research and the open columnar-format
specifications. Citations are in design/ROADMAP.md "Future directions" and are
repeated here in brief.

### Layout

- Fixed-width **vector** unit (for example 1024 values), the granularity of
  decode and of vectorized execution, following FastLanes (PVLDB vol.18, 2025).
  Encodings are data-parallel over the vector so decode is SIMD-friendly and the
  executor can receive partially decoded, still-compressed vectors and operate on
  them directly (compressed execution).
- **Cascade encoding**: the output of one lightweight scheme is encoded by
  another (for example FOR then bit-packing, or dictionary then RLE), following
  BtrBlocks (SIGMOD 2023). A bounded cascade depth keeps decode fast.
- **Adaptive, sample-based scheme selection**: choose each block's encoding by
  sampling a small fraction of its values, following BtrBlocks. This replaces the
  current "try each and keep the smallest" with a cheaper, near-optimal chooser
  and is the single highest value-to-effort item, because the primitive encodings
  already exist.

### Codecs

- **ALP** for floating point and decimals (SIGMOD 2024), replacing or augmenting
  Gorilla, decoding faster than Gorilla and Zstd.
- **FSST** for short strings, keeping random access while compressing.
- Retain the existing integer encodings (FOR, delta, delta-of-delta, RLE,
  dictionary) as cascade primitives.
- **Block compression opt-in, not default.** On fast local storage, general
  block compression (pglz/lz4/zstd) can cost more CPU than it saves in I/O (Zeng
  et al., VLDB 2024). Default to lightweight encodings plus aggressive dictionary
  use; keep block compression as a per-table, per-tier option, and default it on
  for remote or object storage where the tradeoff reverses.

### Metadata and skipping

- **SMA zone maps**: per-block minimum, maximum, sum, count, and null count
  (Moerkotte, VLDB 1998), stored in the format so aggregates can be answered from
  metadata and low-selectivity scans prune well. This generalizes the current
  per-chunk min/max.
- Retain per-block bloom filters for equality skipping.
- **Space-filling-curve clustering** (Z-order, then Hilbert) as the evolution of
  one-time sorted storage, with incremental background reclustering, following
  Delta Lake Liquid Clustering.

### Mutation

- **Native delete vectors and merge-on-read**: mark deleted and updated rows in a
  side structure, reconcile at read, and compact in the background. This replaces
  the row_mask design with a first-class deletion vector and underpins an efficient
  `MERGE` (Delta Lake 3.1).

### Interoperability as the compatibility story

The migration and interchange path is the open standards, not binary
compatibility with one extension: Arrow IPC and Parquet import and export (already
implemented), extended over time to reading external Parquet, Iceberg, and Delta
tables with predicate and projection pushdown. This is broader and more durable
than reading Citus/Hydra files, and it does not reintroduce the provenance tie.

## 6. Phased migration of the existing engine

The pivot is executed in phases, each matrix-gated, so the working engine keeps
running while the format and identity are re-originated. No phase reads upstream
source; each is written from the new specification and public PostgreSQL API.

- **Phase A. Provenance reset (docs only).** Rewrite PROVENANCE.md and
  REWRITE_PLAN.md to state the project is originated from public research and the
  public PostgreSQL API. Retire FORMAT_AND_INTERFACE_SPEC.md as upstream-derived.
  Begin the new specification document. No code.
- **Phase B. New specification.** Write the new format and catalog specification
  from section 5 and the open columnar-format specifications. Design the catalog
  and the SQL surface fresh. Review it against the clean-room rules before any
  implementation.
- **Phase C. Rename and re-namespace.** Rename the extension, schema, access
  method, GUC prefix, and functions to the new identity, keeping the current
  format for now. This is mechanical and fully matrix-gated, and it produces a
  running engine under the new name while the new format is built.
- **Phase D. New format core.** Implement the new vector-based, cascade-encoded
  format with adaptive selection, SMA zone maps, and opt-in block compression, as
  a new format version, behind the existing reader/writer interfaces. Keep the
  differential oracle green throughout.
- **Phase E. New codecs.** Add ALP and FSST as cascade primitives; make the
  adaptive selector choose among the full set.
- **Phase F. Mutation and clustering.** Replace the row_mask with native delete
  vectors and merge-on-read; add space-filling-curve clustering with background
  reclustering.
- **Phase G. Interop extension.** Extend Arrow/Parquet interop toward reading
  external Parquet and open-table-format files with pushdown.
- Later phases (own projects): morsel-driven parallelism, adaptive JIT. These are
  independent of the format reset and can proceed once the core is stable.

The large execution bets (morsel parallelism, JIT) and the interop and PG-17/19
integration items remain as in the roadmap's "Future directions" and are not
gated on the format reset.

## 7. What is given up, and the risks

- **No drop-in read of existing Citus/Hydra columnar tables.** Migration is by
  `COPY` or Arrow/Parquet import, both already supported. The owner has stated
  compatibility is not required, so this is acceptable.
- **Cost.** The format, catalog, specification, and naming are a generation of
  work. The estimate in section 3 is that the majority of the engine (TAM, MVCC,
  indexing, IOS, vectorized executor, interop) carries over with renaming, and the
  format/catalog/spec/identity layers are what change.
- **Two format lines during transition.** Phases C and D imply a period where the
  new engine still writes the old format under the new name, then gains the new
  format as a new version. The reader must handle both during transition, then the
  old format is dropped. This is a normal format-migration burden.
- **Provenance during transition.** Until Phase A lands and the upstream-derived
  spec is retired, the repository still contains the derived specification. The
  owner has chosen to hold public repository changes until provenance is settled;
  this plan is consistent with that hold.

## 8. Open questions for the owner

- The new project name and namespace.
- Priority order among the phases, and how much of the large execution work
  (morsel parallelism, JIT) to commit to.

Decided (2026-07-21, owner): legal review is not required; project name stays
**pgColumnar**; SQL namespace becomes **`pgcolumnar`**; keep the existing repo
with `main` on the 1.0-dev line (preserved by the `v1.0-dev` tag) and run the
pivot on a long-lived integration branch **`re-origination`**, fed by per-phase
matrix-gated PRs, merged to `main` once at the end.

## 9. Immediate status

- design/ROADMAP.md carries the research-informed "Future directions" section
  (uncommitted).
- This document is uncommitted.
- No code has changed. No repository commit or push will be made on this pivot
  until the owner clears provenance and chooses the name and repository strategy.
