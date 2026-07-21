# Provenance

pgColumnar is built with a clean-room method so that it is free of any copyright
tie to other columnar projects and can be released under the MIT License.

## Re-origination (the 2.0 line, on the re-origination branch)

The 1.0-dev line (on `main`, preserved by the `v1.0-dev` tag) was a clean-room
reimplementation whose goal was compatibility with the Citus and Hydra columnar
on-disk format and SQL interface. That work stands and is not disowned.

Going forward, on the `re-origination` branch, that compatibility goal is
dropped: neither the on-disk format nor the SQL interface needs to match Citus or
Hydra. The project is therefore re-originating its format, catalog, and SQL
surface from public research (peer-reviewed papers) and the open Arrow, Parquet,
and ORC specifications, rather than from the upstream design. The plan is in
[design/DESIGN_PIVOT_ORIGINAL_ENGINE.md](design/DESIGN_PIVOT_ORIGINAL_ENGINE.md).

What this changes for provenance:

- The specification the implementation builds from is being replaced. The new
  format and catalog specification (in progress) is written from public sources,
  not extracted from upstream source.
  [design/FORMAT_AND_INTERFACE_SPEC.md](design/FORMAT_AND_INTERFACE_SPEC.md)
  described the 1.0-dev compatibility format and is retained as the record of
  that line, not as the source for the re-originated engine.
- The clean-room rule against reading upstream source is unchanged and still
  binds every contributor.
- Copyright is not patents; the note in the rewrite plan about checking for
  patents on techniques used still applies.

The roles, rules, and log below continue to govern all work.

## Roles

- Specification role. A context that read prior columnar source extracted only
  functional and interoperability facts into
  design/FORMAT_AND_INTERFACE_SPEC.md. That document contains no source and no
  implementation expression. It records the on-disk format, the metadata
  catalog, identifier encodings, compression codes, and the SQL surface.
- Implementation role. A separate context writes all code, build files, and
  tests using only the specification, the delivery plan, and the public
  PostgreSQL documentation and headers.

## Rules for implementers

- Do not read, copy, or reference the source of any other columnar project.
- Do not open the prior AGPL source tree. It is kept in a separate location and
  is never checked out beside this repository.
- Build only from the project's own specification and the public PostgreSQL API.
  On the 1.0-dev line that specification was
  design/FORMAT_AND_INTERFACE_SPEC.md. On the re-origination line it is the new
  format and catalog specification written from public research and the open
  Arrow/Parquet/ORC specifications (in progress; see
  design/DESIGN_PIVOT_ORIGINAL_ENGINE.md).
- Correctness may be checked by running the prior extension and comparing
  observable behavior. Running a program is not copying it. Do not copy its test
  files or expected output.

## Log

- 2026-07-18. Specification role produced design/FORMAT_AND_INTERFACE_SPEC.md and
  design/REWRITE_PLAN.md.
- 2026-07-18. Repository created, MIT License applied, specification and plan
  imported. Implementation role assigned to a fresh context working only from the
  specification.
- 2026-07-18. Phase 0 and phase 1 implemented by the implementation role from
  design/FORMAT_AND_INTERFACE_SPEC.md and the public PostgreSQL 17 headers only:
  PGXS build, extension control and SQL script, the stripe/chunk/chunk_group
  catalog and storageid_seq, the storage layer (metapage, logical/physical
  mapping, reservation), the uncompressed writer, the sequential-scan reader, and
  the table access method handler. Verified with a fresh smoke test on
  PostgreSQL 17. No other columnar source was consulted.
- 2026-07-18. Phase 2 (compression and projection) implemented by the
  implementation role from design/FORMAT_AND_INTERFACE_SPEC.md (sections 4, 5,
  7.2, 8.3, 9), design/REWRITE_PLAN.md section 6, and the public PostgreSQL 17
  headers plus the public system liblz4 and libzstd APIs only. Added a new
  compression module (columnar_compression.c) implementing the four codecs from
  spec section 5: none, PostgreSQL's builtin pglz, system liblz4, and system
  libzstd with a compression level; lz4 and zstd are detected with pkg-config in
  the Makefile and compiled out cleanly when absent, with a runtime fall back to
  a built-in codec. Each chunk's value stream is compressed independently and
  falls back to uncompressed storage when compression does not shrink it; the
  exists stream is never compressed. The writer now computes a per-chunk min/max
  skip list for orderable types (via the type cache default btree comparison
  proc) and stores it as the encoded values in columnar.chunk.minimum_value and
  maximum_value. The reader decompresses per column into a per-chunk-group
  context, decodes only projected columns, and contains the chunk-group skipping
  evaluator that uses the stored min/max against pushed-down comparison
  predicates. The columnar.compression, columnar.compression_level, and
  columnar.enable_qual_pushdown GUCs from spec 8.3 were added; the chunk-group
  and stripe row-limit GUCs are honored. Verified with a fresh phase 2 test
  (test/phase2.sh) on PostgreSQL 17 (assert-enabled), covering per-codec
  round-trips, the uncompressed fallback, nulls under compression, projection,
  min/max storage and values, and filter correctness, with the phase 1 smoke
  test kept green. Note recorded honestly: automatic qualifier pushdown into a
  plain sequential scan needs the custom scan node scheduled for phase 5, so the
  skipping evaluator is wired and correct but activates only when the executor
  supplies scan keys. No other columnar source was consulted.
- 2026-07-18. Phase 3 (update and delete) implemented by the implementation role
  from design/FORMAT_AND_INTERFACE_SPEC.md (sections 6, 7.5, 7.6, 9),
  design/REWRITE_PLAN.md section 6, and the public PostgreSQL 17 headers and
  public PostgreSQL source (for callback contracts only). Added the
  columnar.row_mask table, the row_mask_seq sequence, and their three indexes
  exactly per spec 7.5, plus their cleanup on drop. Added a new module
  (columnar_row_mask.c) that accumulates delete marks per chunk group in a
  per-subtransaction in-memory buffer and applies them to columnar.row_mask in a
  single upsert per chunk group at flush time; a set bit in the mask, stored
  LSB-first over [start_row_number, end_row_number], marks a deleted row. DELETE
  marks rows without rewriting stripes; UPDATE is delete-plus-insert with the new
  row getting a fresh row number; the reader loads a stripe's row mask and skips
  masked rows while keeping the value-stream cursors aligned; a fetch-by-tid path
  reconstructs a single row for the UPDATE executor. Snapshot visibility and
  same-transaction read-your-writes are achieved by flushing pending writes and
  delete marks to the catalog and reading the columnar metadata with a snapshot
  whose command id is advanced (ColumnarCatalogSnapshot); that affects only the
  current transaction's own tuples, so isolation from other transactions is
  preserved (verified with a concurrent uncommitted-insert probe and a
  repeatable-read probe). Transaction and savepoint semantics are honored:
  pending writes and delete marks are keyed by subtransaction and discarded on
  rollback and on subtransaction abort, and promoted to the parent on
  subtransaction commit; buffers are flushed at each statement end (an
  ExecutorEnd hook, since INSERT/UPDATE/DELETE do not call finish_bulk_insert) so
  a buffer written before a savepoint is attributed to the outer subtransaction
  and survives ROLLBACK TO. Temporary columnar tables are supported; the
  implementation always carries the Relation and never reverse-resolves a
  relation from its relfilenumber, so the temporary-relation resolution fallback
  is inherent. This closes the phase 1 read-your-writes gap. Verified with a
  fresh phase 3 test (test/phase3.sh) on PostgreSQL 17 (assert-enabled) covering
  delete across chunk-group boundaries, update of values and keys,
  read-your-writes, transaction rollback, savepoint rollback, and temporary
  tables, with the phase 1 smoke and phase 2 tests kept green. No other columnar
  source was consulted.
- 2026-07-19. Phase 4 (indexes and constraints) implemented by the
  implementation role from design/FORMAT_AND_INTERFACE_SPEC.md (sections 6, 8.2,
  9), design/REWRITE_PLAN.md section 6, the public PostgreSQL 17 headers and
  documentation, and the public PostgreSQL 17 source consulted only for
  callback/API contracts (the index_build_range_scan and index_fetch_tuple
  table-AM contracts, the btree uniqueness-check fetch path, and the
  get_relation_info planner hook and IndexOptInfo). Row numbers are now reserved
  eagerly: when a writer begins buffering a stripe it reserves the stripe id and
  a full stripe_row_limit run of row numbers up front (a new
  ColumnarReserveRowNumbers that advances only the row-number and stripe-id
  metapage marks), so every inserted row is assigned its stable row number and
  synthetic item pointer at insert time; the byte offset is reserved separately
  at flush (ColumnarReserveOffset). ColumnarWriteRow returns the assigned row
  number and the insert, multi-insert, and update callbacks publish it as the
  slot's item pointer so the executor records correct index entries and enforces
  unique constraints. index_build_range_scan reconstructs every live row (the
  reader skips row-mask-deleted rows) and drives the index build callback, so
  CREATE INDEX for btree and hash works; a partial-block range is rejected as
  unsupported. index_fetch_tuple maps an item pointer back to a row number and
  reads the row from the flushed stripes, falling back to the unflushed write
  buffer (a new ColumnarBufferedRowByNumber that reads only process-local
  memory), which lets a unique constraint reject two duplicate rows inserted by a
  single statement without taking any lock while the caller holds an index buffer
  lock; a row-mask-deleted row is reported as not found, so index scans never
  return deleted rows. index_delete_tuples is a safe no-op (stale entries are
  filtered on fetch). NOT NULL and CHECK constraints are enforced by the executor
  through the ordinary insert path. Heap<->columnar conversion is exposed as
  columnar.alter_table_set_access_method (spec 8.2), a plpgsql wrapper over
  PostgreSQL's own ALTER TABLE ... SET ACCESS METHOD, which rewrites through the
  columnar insert or scan path. A get_relation_info planner hook clears each
  candidate index's per-column can-return flags for columnar tables, so
  index-only scans are never chosen (no visibility map, spec 9) while ordinary
  index scans and sequential scans remain available; relation_estimate_size now
  counts rows from stripe metadata instead of the reservation high-water mark so
  the estimate is accurate under eager reservation. Verified with a fresh phase 4
  test (test/phase4.sh) on PostgreSQL 17 (assert-enabled) covering btree and hash
  index build and scan, unique/primary-key rejection across statements and within
  one statement and at index build time, NOT NULL and CHECK rejection, index
  scans skipping deleted and updated rows, heap->columnar->heap round-trips of
  counts and values, and EXPLAIN confirming an Index Scan (never an Index Only
  Scan) with a sequential scan still available; the phase 1 smoke, phase 2, and
  phase 3 tests were kept green. No other columnar source was consulted.
- 2026-07-19. Phase 5 (planner integration and vacuum) implemented by the
  implementation role from design/FORMAT_AND_INTERFACE_SPEC.md (sections 7.4,
  8.2, 8.3, 9), design/REWRITE_PLAN.md section 6, the public PostgreSQL 17
  headers and documentation, and the public PostgreSQL 17 source consulted only
  for callback/API contracts (the custom-scan provider contract in
  nodes/extensible.h and executor/nodeCustom.c, create_customscan_plan and
  use_physical_tlist in createplan.c, the set_rel_pathlist_hook signature,
  slot_getsysattr's ctid handling, and the reindex_relation and
  RelationSetNewRelfilenumber signatures). Added a custom scan provider
  (columnar_customscan.c) registered from _PG_init with a set_rel_pathlist_hook:
  for a columnar base relation it removes the sequential-scan path, drops the
  parallel (partial) paths, and adds a custom scan path that inherits the
  sequential scan's cost (so SET enable_seqscan=off still steers to an index
  scan). The custom scan computes the projected column set from the plan's
  target list and restriction clauses (pull_varattnos) and pushes it into the
  reader so only referenced columns are decoded, and translates simple
  "column op const" restriction clauses into scan keys via the column type's
  default btree opfamily so the reader's phase 2 min/max skip lists remove chunk
  groups that cannot match. The executor always re-applies the full restriction
  clauses as an ExecScan filter, so chunk-group skipping is a pure optimization
  that never changes results (a filtered query returns the same rows with
  columnar.enable_qual_pushdown on or off); both custom scan and pushdown are
  gated on the columnar.enable_custom_scan and columnar.enable_qual_pushdown
  GUCs from spec 8.3. EXPLAIN reports projected columns and, under ANALYZE, the
  chunk groups read versus removed by filter. UPDATE and DELETE flow through the
  custom scan because the scan stores each row's synthetic item pointer on the
  slot and slot_getsysattr returns it for the ctid row-identity var. Added the
  columnar.options catalog table and its unique index exactly per spec 7.4,
  read at write-state creation so per-table compression, compression level,
  chunk-group row limit, and stripe row limit override the instance GUC defaults
  for subsequent writes (ColumnarReadOptions), with the plpgsql
  alter_columnar_table_set and alter_columnar_table_reset functions from spec
  8.2 and cleanup of a table's options row on drop. Added columnar.vacuum
  (columnar_vacuum.c), which materializes a relation's live rows (the reader
  skips row-mask-deleted rows), swaps the relation to a fresh relfilenode
  transactionally (RelationSetNewRelfilenumber), removes the old metadata, writes
  the live rows back into full stripes, and rebuilds indexes (reindex_relation)
  so their synthetic item pointers address the renumbered rows; this combines
  small stripes and physically reclaims deleted-row space. Added
  columnar.vacuum_full (plpgsql, over a schema), columnar.stats (SQL over the
  catalog), and a get_storage_id C function that resolves a relation to its
  storage id. This activates phase 2's chunk-group skipping for ordinary
  filtered SELECTs, closing the dormant-skipping limitation. Verified with a
  fresh phase 5 test (test/phase5.sh) on PostgreSQL 17 (assert-enabled) covering
  the custom scan in EXPLAIN, chunk-group skipping on a plain filtered SELECT
  (4 of 5 groups removed) with results identical to pushdown off, column
  projection counts, options changing stored compression/level/chunk-group
  layout and resetting, vacuum reducing stripe count and reclaiming deleted
  space while returning correct rows and keeping an index correct, no parallel
  plan being chosen, and a clean fallback to a sequential scan; the phase 1
  smoke and phase 2, 3, and 4 tests were kept green (phase 4's full-scan
  availability check was updated to accept the columnar custom scan, which is
  now the full-table access path). No other columnar source was consulted.
- 2026-07-19. Phase 6 (vectorized execution) implemented by the implementation
  role from design/FORMAT_AND_INTERFACE_SPEC.md (sections 8.3 and 9),
  design/REWRITE_PLAN.md section 6, and the public PostgreSQL 17 headers and
  source consulted only for the custom-scan provider contract, the
  create_upper_paths_hook signature, the relcache invalidation callback, and the
  documented aggregate result types; no other columnar source was consulted.
  Added a vectorized batch reader (ColumnarReadNextVector in columnar_reader.c)
  that reuses the existing stripe/chunk-group state machine, min/max skipping,
  row mask, and projection to hand back one decoded chunk group at a time as flat
  per-column value and null arrays plus a per-row deleted flag, so a consumer can
  process a group in tight typed loops rather than one tuple at a time. Added a
  shared column-at-a-time filter (columnar_vector.c): a plan's simple, strict
  "column op const" restriction clauses become predicates evaluated over the
  decoded arrays to build a selection vector, where a null column value or a
  failed comparison excludes the row exactly as SQL WHERE semantics require. The
  base custom scan uses this in a vectorized mode (columnar_customscan.c) that
  decodes a group, drops rows failing a pushed-down conjunct or the row mask, and
  emits the survivors, while the executor still re-applies the full qual, so the
  vectorized scan returns exactly the rows the scalar reader returns; the mode is
  used only when every needed column is decoded (it stays on the scalar per-row
  path for whole-row and ctid references, as UPDATE and DELETE use). Added a
  vectorized aggregate: for a plain SELECT agg(col) FROM columnar_table [WHERE
  simple quals] with no grouping or HAVING, a create_upper_paths_hook adds a
  custom path on the grouping upper relation that scans the base table vectorized
  and computes count, sum, avg, min and max directly over the decoded arrays,
  reproducing PostgreSQL's own result semantics exactly (integer sum in int8 as
  int2_sum/int4_sum do, average of an integer column as numeric via
  int8_numeric and numeric_div as int8_avg does, min and max by the column type's
  default btree comparison). The aggregate custom scan reuses the base scan's
  single registered CustomScanMethods (so both show as "Custom Scan
  (ColumnarScan)") with a create-state callback that dispatches on scanrelid, and
  it is chosen only when every aggregate, every column type, and every filter
  clause is fully supported; anything else (bigint or numeric sum and average,
  float sum and average, ordered-set and string aggregates, DISTINCT-qualified
  aggregates, GROUP BY, HAVING, non-constant or non-simple quals) adds no path
  and the ordinary scalar Agg plan runs, so a vectorized result always equals the
  scalar result. Added the optional decompressed-chunk cache (columnar_cache.c)
  behind columnar.enable_column_cache (default off) and sized by
  columnar.column_cache_size megabytes (spec 8.3): a backend-local, LRU-bounded
  cache of decompressed value streams keyed by storage id and absolute logical
  offset (both immutable within a storage id except across a truncate), returning
  a fresh copy to the caller so eviction is always safe, and flushed on any
  relcache invalidation so truncate offset reuse and vacuum storage swaps can
  never serve a stale buffer; it only avoids repeated decompression and never
  changes results. Added the columnar.enable_vectorization (default on),
  columnar.enable_column_cache (default off), and columnar.column_cache_size
  (default 200 MB) GUCs from spec 8.3. Verified with a fresh phase 6 test
  (test/phase6.sh) on PostgreSQL 17 (assert-enabled): vectorized count, sum, avg,
  min and max equal the scalar results with the toggle on versus off, on a
  multi-chunk-group table, with and without a filter; a filtered vectorized scan
  returns exactly the scalar rows (compared over an md5 of the ordered output);
  NULLs are handled correctly in aggregates and filters, including empty-result
  sum/min returning NULL; the decompressed-chunk cache on versus off is
  identical, including under a tiny cache budget that forces eviction; bigint and
  numeric and float aggregates, an ordered-set aggregate, a string aggregate,
  count(DISTINCT), and GROUP BY all fall back and stay correct; the row mask is
  honored by the vectorized path after deletes; and a large aggregate ran faster
  vectorized than scalar (about 130 ms versus 280 ms for three passes over two
  million rows). The phase 1 smoke and phase 2 through 5 tests were kept green.
  No other columnar source was consulted.
- 2026-07-19. Multi-version portability (part of phase 7). Made the single
  source tree build assert-enabled and pass all six suites (smoke,
  phase2..phase6) on PostgreSQL 13, 14, 15, 16, 17, 18, and 19, where it had
  built and passed only on 17. All version differences were derived solely from
  the public PostgreSQL headers and source of each target major in the container
  (/usr/local/pgNN/include and /usr/local/src/pgNN, consulted only for
  PostgreSQL's own API and callback contracts), plus
  design/FORMAT_AND_INTERFACE_SPEC.md and design/REWRITE_PLAN.md. No other
  columnar source was consulted. Added a compatibility header
  (src/columnar_compat.h) plus minimal PG_VERSION_NUM guards in the .c files
  covering: the RelFileLocator/RelFileNode rename (PG16) and the matching
  SMgrRelation locator field; RelationCreateStorage gaining register_delete
  (PG15); the index-tuple delete callback (compute_xid_horizon_for_tuples on
  PG13 vs index_delete_tuples on PG14+); relation_set_new_filenode ->
  relation_set_new_filelocator (PG16); the tuple_update/tuple_delete update flag
  (bool vs TU_UpdateIndexes, PG16); scan_analyze_next_block gaining a ReadStream
  (PG17); const VacuumParams (PG19); palloc_aligned/PG_IO_ALIGN_SIZE (PG16);
  get_rel_relam moving into lsyscache (PG17); EmitWarningsOnPlaceholders ->
  MarkGUCPrefixReserved (PG15); the central _PG_init prototype (PG16);
  RelationSetNewRelfilenode -> RelationSetNewRelfilenumber (PG16) and the
  reindex_relation arity changes (PG14 and PG17); the CustomPath
  custom_restrictinfo field (PG17); and the PG19 table-AM rework (the options
  bitmask widening to uint32 and the new leading options argument on
  tuple_insert/tuple_insert_speculative/multi_insert/finish_bulk_insert/
  tuple_delete/tuple_update, index_fetch_begin gaining a flags argument,
  relation_copy_for_cluster gaining a Snapshot, scan_analyze_next_tuple dropping
  OldestXmin, the parallel scan descriptor's phs_relid -> phs_locator in PG18,
  PageSetChecksumInplace -> PageSetChecksum in PG19, and the ExplainProperty*
  helpers moving to commands/explain_format.h in PG18). PG19 removed
  get_relation_info_hook; the index-only-scan suppression (a columnar table has
  no visibility map) was moved to the new build_simple_rel_hook there, which
  fires at the same planning point and clears the same index canreturn flags, so
  the plan is an identical plain index scan on every major. The Makefile now
  selects the C standard by major (gnu17 for 13..18, gnu23 for 19, whose headers
  use the C23 typeof_unqual). One feature degrades on PostgreSQL 13 and 14, which
  have no ALTER TABLE ... SET ACCESS METHOD: the
  columnar.alter_table_set_access_method helper falls back to a
  build-new-table/copy/swap rewrite that preserves columns, defaults,
  constraints and indexes but not the original relation's OID or dependent
  objects; PostgreSQL 15+ still uses the in-place ALTER. The phase3 test now
  feeds SQL on psql's stdin instead of a single multi-statement -c string, since
  psql before 14 prints only the last command's result; the checks are otherwise
  unchanged and no upstream expected-output file was used. Verified with a fresh
  test/run_all_versions.sh that builds each major in an isolated directory and
  runs every suite: all seven majors pass warning-free, and the vectorized
  scan-after-delete md5 is identical across majors (byte-for-byte equal results).
- 2026-07-19. Bug audit by the implementation role, working only from the
  specification, the pgColumnar source, and the public PostgreSQL headers
  (consulted solely for API/callback contracts). Three real defects were found,
  each proved with a minimal repro, fixed, and covered by a regression test in
  test/audit.sh (added to the run_all_versions.sh suite list). (1) A sequential
  scan of a table read ERROR "columnar: missing chunk for attr N" after ALTER
  TABLE ADD COLUMN, because a stripe written before the column existed has no
  chunk for it; the reader now yields the attribute's missing value via the
  public getmissingattr (attmissingval for a constant default, else NULL),
  matching heap's fast-default semantics, across the sequential, vectorized,
  index-fetch and by-tid read paths (columnar_reader.c). (2) Chunk-group
  skipping compared a pushed-down predicate against the stored per-chunk min/max
  using the column's own collation while the query's comparison used a different
  collation (an explicit COLLATE), so a group holding matching rows could be
  wrongly skipped and a filtered count returned 0 instead of the true count;
  columnar_clause_to_scankey now declines to push a clause whose operator
  collation differs from the column's attcollation, so the executor still
  filters but skipping never changes results (columnar_customscan.c). (3)
  columnar.alter_columnar_table_set stored an out-of-range chunk_group_row_limit
  / stripe_row_limit / compression_level without validation; a zero
  chunk_group_row_limit recorded a stripe with chunk_row_count = 0 and made the
  row-number arithmetic divide by zero on delete/update/fetch. The setter now
  bounds the three integers to the same ranges as the corresponding GUCs
  (columnar--1.0.sql). Re-verified with test/run_all_versions.sh: all seven
  majors (PostgreSQL 13 through 19) build warning-free and pass every suite,
  including the new audit suite; the pre-fix tree fails the audit suite.
- 2026-07-19. Benchmarks and consolidated documentation (part of phase 7) by the
  implementation role, using only the pgColumnar source, the specification, the
  delivery plan, and the public PostgreSQL API and tools. No other columnar
  source was consulted, and no prior benchmark script or number was reused.
  Added a fresh, self-contained benchmark harness (bench/run_bench.sh) that
  builds and installs the extension into a throwaway cluster, loads one identical
  dataset into a heap table and into columnar tables (zstd and none), and reports
  on-disk size (pg_total_relation_size and pg_table_size) and the median latency
  of a representative query set (count, a sum/avg aggregate, a min/max-skippable
  filtered aggregate, an indexed point lookup, and a wide-table projection), plus
  the vectorization on/off effect and the compression none/zstd tradeoff, with an
  optional DuckDB comparison behind BENCH_DUCKDB. Timing is measured server-side
  around EXECUTE in a plpgsql helper as the median of a configurable number of
  runs after a warm-up. Ran it on PostgreSQL 17.10 non-assert and 19beta2
  non-assert at 6,000,000 rows in the container (fresh build dir
  /root/pgcolumnar_bench): columnar zstd was about 6x smaller than heap
  table-only (96 MB vs 579 MB) and 2x to 11x faster on scan, aggregate, filtered
  aggregate, and projection queries, while heap was far faster on the single-row
  point lookup (about 0.01 ms vs 373 ms), reported honestly in the README.
  Consolidated README.md into a professional document (what it is, MIT license
  and clean-room provenance, supported majors 13-19, PGXS build and the
  liblz4/libzstd dependency, a quickstart, the feature set, the summarized
  benchmark numbers, a complete limitations section, and the testing section) and
  added docs/ARCHITECTURE.md, a module-by-module map derived only from the source
  and the specification. No src/ behavior or test was changed. One reproducible
  defect was noticed and flagged for a separate pass, not fixed here: CREATE INDEX
  on a columnar table logs "WARNING: resource was not closed: relation" (a leaked
  relation reference in the index-build scan path); the index still builds and
  queries return correct results, and INSERT alone does not trigger it.
- 2026-07-19. Fixed the CREATE INDEX relation-reference leak flagged in the
  previous entry, by the implementation role working only from the pgColumnar
  source, the specification, and the public PostgreSQL table AM contract
  (heapam_index_build_range_scan and the parallel btree build caller in the
  installed PostgreSQL source, consulted only for that contract). Root cause: a
  parallel index build opens a TableScanDesc per participant through the table
  AM's scan_begin callback, which takes a relation reference; the
  index_build_range_scan callback owns that scan and must end it. Our
  columnar_index_build_range_scan ignored the passed scan entirely: it never
  ended it, so every build participant leaked one reference to the table
  (reported at commit as "resource was not closed: relation"), and it read
  through a private full-table reader instead of the participant's claimed scan,
  so each live row was indexed once per participant. The fix reads through the
  passed scan's reader (whose single-participant claim makes exactly one
  participant read the whole table) and ends the scan on completion, matching
  heapam; the serial path (no scan supplied) is unchanged. Extended
  test/audit.sh with a regression check that forces a parallel index build and
  asserts the server logfile carries no "resource was not closed" warning and
  that every row is indexed exactly once. The pre-fix tree fails this check
  (leak warning present; on an assert build the duplicate index tuples trip the
  btree sort assertion). Verified all suites on PostgreSQL 13 through 19, each
  built from a clean per-major copy.
- 2026-07-19. Fixed tracking issue #4 (concurrent deletes to the same chunk
  group can drop delete marks) by the implementation role, working only from
  design/FORMAT_AND_INTERFACE_SPEC.md (section 7.5, the row_mask schema, which is
  unchanged), design/REWRITE_PLAN.md, and the public PostgreSQL lock-manager and
  heap/catalog APIs (storage/lock.h SET_LOCKTAG_ADVISORY and LockAcquire,
  miscadmin.h MyDatabaseId, nodes/pg_list.h list_sort) as documented in the
  public PostgreSQL headers. Root cause: ColumnarUpsertRowMask applied each
  chunk group's accumulated delete bits as an unguarded read-modify-write of the
  single shared columnar.row_mask heap tuple (read the mask with SnapshotSelf,
  OR in the new bits, CatalogTupleUpdate/Insert). Two transactions deleting
  different rows in the same chunk group could both read the pre-existing
  committed mask and then collide, so one transaction's bits were lost (its
  deleted row stayed visible) or its statement aborted; the first delete of a
  chunk group could also race on the row_mask unique index. The fix keeps the
  on-disk format 2.0 unchanged and serializes the read-modify-write per chunk
  group with a transaction-scoped exclusive advisory lock keyed by a hash of
  (storage_id, stripe_id, chunk_id): a second writer to the same chunk group
  blocks until the first commits, then re-reads the committed mask (SnapshotSelf,
  now guaranteed to see the prior committed writer because the lock is held to
  transaction end) and merges its bits in. The single lock covers both the
  update path and the first-delete insert race, so no subtransaction is needed
  and the fix is safe in every flush context including pre-commit. Chunk-group
  locks are acquired in a consistent global order (chunk buffers are sorted by
  stripe/chunk/start row before flush) so two multi-group deleters cannot form an
  AB-BA deadlock. Deletes to different chunk groups use different keys and do not
  contend. Update (delete-plus-insert) shares the same path, so it is consistent
  automatically. Added test/concurrency.sh, a deterministic regression test that
  forces the interleaving (a columnar DELETE flushes its marks at statement end
  inside the open transaction; a pg_stat_activity poll on the real lock wait is
  the barrier, not a sleep) and asserts both concurrent deletes survive, covering
  the update-existing path, the first-delete insert race, and that different
  chunk groups do not serialize; the pre-fix tree fails it (the second deleter's
  bit is lost and its row stays visible) and the fixed tree passes. Wired the new
  suite into test/run_all_versions.sh. Verified all suites on PostgreSQL 13
  through 19, each built from a clean per-major copy. No other columnar source
  was consulted.
- 2026-07-19. Fixed tracking issue #5 (two concurrent transactions inserting the
  same unique key can both miss the conflict) by the implementation role, working
  only from design/FORMAT_AND_INTERFACE_SPEC.md (sections 6 and 9, unchanged), the
  design analysis design/ISSUE_5_ANALYSIS.md, design/REWRITE_PLAN.md, and the
  public PostgreSQL API as documented in the public headers: storage/lock.h
  (SET_LOCKTAG_ADVISORY, LockAcquire), catalog/index.h (BuildIndexInfo,
  FormIndexDatum), executor/executor.h (ExecPrepareQual, ExecQual,
  CreateExecutorState, per-tuple expr context), utils/typcache.h
  (lookup_type_cache, TYPECACHE_HASH_PROC_FINFO), commands/defrem.h
  (GetDefaultOpClass), utils/lsyscache.h (get_opclass_family), utils/rel.h
  relcache fields (rd_opcintype, rd_opfamily, rd_indcollation, rd_index),
  utils/inval.h (CacheRegisterRelcacheCallback), and utils/hsearch.h. Root cause:
  a columnar row's data is invisible to other backends until its stripe is
  flushed at statement end, but its btree index entry with the eagerly reserved
  synthetic TID is written immediately, so PostgreSQL's dirty-snapshot uniqueness
  check resolves that TID through columnar_index_fetch_tuple, which returns false
  for a row still in another backend's private write buffer. Two inserters of the
  same key in overlapping statement windows could both miss the conflict. The fix
  keeps the on-disk format 2.0 unchanged. It adds src/columnar_unique.c, which the
  table AM insert paths (columnar_tuple_insert, columnar_multi_insert, and the new
  row version in columnar_tuple_update) call before the executor's btree check.
  For each applicable unique index it takes a transaction-scoped exclusive
  advisory lock keyed by (index OID, bucket, discriminator 2) so a second inserter
  of an equal key waits until the first commits and flushes, at which point the
  ordinary btree check catches the committed duplicate. Equal keys map to one lock
  by hashing each key column with its type's default hash proc (consistent with
  the index equality: numeric scale, citext case, collation-aware text), combining
  columns with an FNV mix and a splitmix finalizer, and reducing to a bounded
  bucket count (columnar.unique_lock_buckets, default 128) so a bulk load holds at
  most that many locks per unique index. An index whose operator family does not
  match its key type's default btree opclass family, or whose key type has no hash
  proc, falls back to one coarse per-index lock (correct, more serialization).
  Partial indexes are locked only when the row satisfies the predicate; expression
  indexes are handled by hashing the FormIndexDatum expression value; NULLS
  DISTINCT keys containing a NULL are not locked while NULLS NOT DISTINCT (PG15+)
  keys are. A relcache-invalidated backend-local cache holds the per-relation
  unique-index metadata. The advisory-lock discriminator (2) differs from the
  issue #4 row_mask lock (1) so the two lock spaces never false-share. Added
  test/unique_conc.sh, a deterministic regression test that holds one inserter
  mid-statement (a two-row INSERT whose second row blocks on a holder-session
  advisory lock, so the first, key-carrying row is buffered and indexed but not
  yet flushed) and, using a pg_stat_activity poll on the real lock wait as the
  barrier, asserts fails-before (with columnar.enable_unique_insert_lock off the
  duplicate commits, count 2) and passes-after (the second inserter blocks then
  gets a unique_violation, count 1) for the same-key and opclass-equal numeric
  1.0/1.00 cases, plus multi-column, partial (inside and outside the predicate),
  different-keys-do-not-block, NULLS DISTINCT, NULLS NOT DISTINCT (PG15+),
  same-statement duplicate, and aborted-first-inserter cases. The citext case is
  covered only when the extension is installed and is skipped otherwise; the
  numeric case proves the same opclass-safe hashing. Renamed test/concurrency.sh's
  final banner to the project convention (CONCURRENCY TEST PASSED/FAILED) and
  wired unique_conc into test/run_all_versions.sh. Verified all suites on
  PostgreSQL 13 through 19, each built from a clean per-major copy. No other
  columnar source was consulted.

## Encoding, execution, and skipping work (I1-I8, format 2.1)

Implemented the research-driven roadmap in design/IMPROVEMENT_PLAN.md, derived
solely from the cited public column-store literature (C-Store; MonetDB/X100;
Abadi SIGMOD 2006 / ICDE 2007 / PhD thesis; Zukowski et al. Super-Scalar
Compression; Facebook Gorilla, VLDB 2015) and the public PostgreSQL API. No
other columnar source was consulted.

- I1 columnar_encoding.c: lightweight, type-aware value-stream encodings applied
  per chunk before the block codec as reversible byte transforms (RLE,
  frame-of-reference + bit-packing, delta). Format 2.1 adds the nullable chunk
  columns value_encoding_type and value_raw_length; 2.0 files read as encoding
  none.
- I2/I3: a compression-block run iterator (ColumnarBlockReader) and compressed
  aggregate execution that folds each column run-at-a-time when there are no
  predicates and no deletes, with a per-row fallback; GUC
  columnar.enable_compressed_execution.
- I4: Gorilla XOR (float4/float8, simplified without previous-window reuse) and
  delta-of-delta (integer family), via a streaming bit reader/writer.
- I5: dictionary encoding (fixed-width and varlena, bounded distinct count),
  completing adaptive per-chunk selection across runs/range/sequence/cardinality/
  float axes; integer encodings restricted to true integers so floats use gorilla.
- I6: column-at-a-time selection vector with typed branch-free comparison loops
  for integer/float/date/time types (btree strategy resolved portably from the
  type's opfamily), operator-function fallback otherwise.
- I7 columnar_bloom.c: per-chunk bloom filters for equality chunk-group skipping,
  restricted to hashable non-collatable types so the hash is collation-independent;
  nullable chunk column bloom_filter; GUC columnar.enable_bloom_filter.
- I8: late materialization in the vectorized scan (decode predicate columns,
  filter, then decode output columns only for surviving groups); reader split
  into ColumnarAdvanceGroup and ColumnarDecodeGroupColumns; GUC
  columnar.enable_late_materialization.

Testing: extended the differential oracle, recovery, and fuzz suites; all suites
pass on PostgreSQL 13 through 19, each from a clean per-major build. Every
feature is off/on-equivalent where it has a GUC and is validated against a heap
oracle, so none changes query results.

- 2026-07-21. 1.0-dev tagged (`v1.0-dev`, format 2.2) as the compatibility
  clean-room line on `main`. Owner then established that neither on-disk-format
  nor SQL-API compatibility with Citus/Hydra is required. Started the
  re-origination line on the `re-origination` branch: the format, catalog, and
  SQL surface are being redesigned from public research and the open
  Arrow/Parquet/ORC specifications, and the SQL namespace moves from `columnar`
  to `pgcolumnar`. This Phase A change is documentation only; it retires
  design/FORMAT_AND_INTERFACE_SPEC.md as the build source (kept as the 1.0-dev
  record) and points the build source at the new specification to be written in
  Phase B. No upstream source consulted. See
  design/DESIGN_PIVOT_ORIGINAL_ENGINE.md and design/TODO_PIVOT.md.
