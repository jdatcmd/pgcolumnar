# Provenance

pgColumnar is built with a clean-room method so that it is free of any copyright
tie to other columnar projects and can be released under the MIT License.

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
- Build only from design/FORMAT_AND_INTERFACE_SPEC.md, design/REWRITE_PLAN.md,
  and the public PostgreSQL API.
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
