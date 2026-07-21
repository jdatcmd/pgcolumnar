# pgColumnar clean-room rewrite plan

> Scope note (2026-07-21). This plan describes the **1.0-dev** line: a clean-room
> reimplementation built for compatibility with the Citus/Hydra columnar on-disk
> format and SQL interface. That goal has since been dropped. For the
> re-origination of the format, catalog, and SQL surface from public research, see
> [DESIGN_PIVOT_ORIGINAL_ENGINE.md](DESIGN_PIVOT_ORIGINAL_ENGINE.md), which
> supersedes this document for all work on the `re-origination` branch. This file
> is kept as the record of the 1.0-dev line.

## Goal

Produce a new columnar storage extension for PostgreSQL that is independent of
the Citus and Hydra columnar code, so that pgColumnar can be released under the
MIT license with no copyleft obligation and no derivative work tie to the
upstream AGPL projects. The new implementation must read and write the existing
on-disk format (version 2.0) and expose the same SQL surface, so that current
columnar tables keep working, but its code, tests, and build system are written
fresh.

## 1. Why this is not just a port

Copyright covers the expression of a program and its translations and
adaptations. Translating the existing C to Rust, or C to Rust and back to C,
produces a derivative work in every case, so neither removes the AGPL tie. The
only path to fresh intellectual property is an independent reimplementation
written from a functional specification, not from the source.

What is safe to keep and what must be original:

- Safe to preserve, because formats and interfaces carry weak copyright
  protection and preserving them is the point of interoperability: the on-disk
  layout, the `columnar` metadata catalog schema, and the SQL surface, all
  captured in FORMAT_AND_INTERFACE_SPEC.md.
- Must be written fresh: every line of implementation, the build system, and the
  test suite, since the existing regression tests and their expected output
  files are themselves AGPL licensed.

## 2. Clean-room method

The defensible process separates two roles.

- Specification role. A person or context that has read the AGPL source extracts
  only functional and interoperability facts into
  FORMAT_AND_INTERFACE_SPEC.md. That document deliberately contains no source and
  no implementation expression. This role is already complete for the format and
  interface.
- Implementation role. A person or context that has not read the AGPL source
  writes the new code using only the specification and the public PostgreSQL
  documentation and headers. The implementer must not open the upstream source.

Practical notes and honest caveats:

- The context that produced the specification has read the upstream source in
  depth and is therefore not a clean implementer. To keep the strongest
  position, the implementation should be carried out by a separate developer or
  a fresh agent context that works only from the specification and the
  PostgreSQL public API. If the same contaminated context also implements, the
  legal position rests on the fact that formats and interfaces are not
  protectable and the expression is independently authored, which is a weaker
  but common posture. Choose the rigor level deliberately.
- Keep a provenance log: who wrote what, from which inputs, and when. This is the
  record that supports the clean-room claim if it is ever questioned.
- Correctness during development may be checked against the old extension by
  running both and comparing behavior, since running a program is not copying
  it. Do not copy its test files or expected output into the new project.
- Copyright is not patents. A clean-room reimplementation does not clear
  patents. Before release, check whether Citus or Hydra hold patents on any
  technique used here. The core methods, columnar stripes, chunk skip lists, and
  general block compression, are long standing prior art from cstore_fdw and
  earlier columnar systems, so exposure is likely low, but confirm.
- Have counsel review the plan and the license choice before public release.

## 3. Language decision

Implement in C. Rationale, in short: the risky surface is the table access
method, buffer manager, smgr, WAL, catalog, and a vectorized executor over raw
datums, which is unavoidable unsafe foreign function interface work in Rust, so
Rust's safety benefit is partial exactly where columnar bugs occur; C aligns
with the existing autoconf build, the assert enabled test matrix, and the
toolchain that already builds across PostgreSQL 13 through 19; and PGRX tracks a
moving window of PostgreSQL majors and would likely not yet support PostgreSQL
19 Beta 2, which would regress support this project already has. The full
comparison is recorded in the project discussion that led to this plan.

## 4. Architecture of the new extension

Modules, from the storage layer up:

1. Storage. Metapage read and write, logical to physical mapping, block and
   page reservation, WAL. Reads and writes the format in sections 2 and 3 of the
   specification.
2. Metadata. Access to the `columnar` catalog tables and sequences. Resolves a
   relation to its storage id, including the temporary relation fallback.
3. Compression. Encode and decode value streams with none, pglz, lz4, and zstd.
4. Reader. Given a relation and a projection and filters, seek and decode the
   needed streams, apply chunk group skipping, and produce tuples.
5. Writer. Batch rows into chunk groups and stripes, compress, reserve space,
   write streams, and record metadata. Flush at transaction pre commit.
6. Row mask. Track deletes and updates without rewriting stripes.
7. Table access method glue. Implement the handler callbacks for scan, insert,
   update, delete, vacuum, index build, and size estimation, with the version
   compatibility shims for PostgreSQL 13 through 19.
8. Planner integration. The custom scan path for projection and filter
   pushdown, and the choices that disable parallel and index only scans.
9. Vectorized execution. The aggregate and filter fast paths. This is a later
   phase and can ship after a correct scalar path.
10. Index and vacuum support. Btree and hash index build and scan, and the
    vacuum functions that compact stripes.

## 5. Delivery phases

Each phase ends with the extension building on PostgreSQL 13 through 19 under an
assert enabled build and passing the new test suite for that phase.

- Phase 0, foundation. Repository scaffolding, MIT license, autoconf build, CI
  across the seven versions, and the fresh test harness. Provenance log started.
- Phase 1, minimal read and write. Metapage, storage layer, metadata for stripe
  and chunk_group and chunk, table access method create, single row and bulk
  insert, sequential scan, drop. Format compatible with 2.0.
- Phase 2, compression and projection. The four compression types, column
  projection, chunk skip lists, and chunk group filtering from pushed down
  quals.
- Phase 3, update and delete. Row mask, delete and update marking, snapshot
  visibility, transaction and savepoint semantics, rollback of pending writes.
- Phase 4, indexes and constraints. Btree and hash index build and scan,
  constraint enforcement on insert, and the conversion function between heap and
  columnar.
- Phase 5, planner and vacuum. Custom scan path, qual pushdown, per table
  options, and the vacuum and stats functions.
- Phase 6, vectorized execution. Vectorized aggregates and filters, and the
  optional decompressed chunk cache.
- Phase 7, release. Multi version validation, documentation, patent check,
  counsel review, and the MIT release.

## 6. Complete task list

### Phase 0, foundation

- [ ] Create the new repository or module with an MIT LICENSE file
- [ ] Write the autoconf configure that accepts PostgreSQL 13 through 19 and
      selects the C standard per version, gnu99 through 18 and gnu23 on 19
- [ ] Set up the compatibility headers for version specific macros
- [ ] Stand up continuous integration that builds assert enabled PostgreSQL 13
      through 19 and runs the test suite
- [ ] Create a fresh regression test harness that does not reuse upstream test
      files or expected output
- [ ] Start the provenance log and record the specification and implementation
      roles
- [ ] Assign the implementation role to a context or developer that has not read
      the upstream source

### Phase 1, minimal read and write

- [ ] Implement the metapage read and write with the fields in specification
      section 3, including the WAL full page image and immediate sync on create
- [ ] Implement the logical to physical mapping and page reservation
- [ ] Create the `columnar` schema, the stripe, chunk, and chunk_group tables,
      the storageid_seq sequence, and their indexes, matching section 7
- [ ] Implement storage id assignment from the sequence and the metapage link
- [ ] Implement the table access method handler skeleton and register the access
      method
- [ ] Implement create table, relation set new filenode, and drop
- [ ] Implement the writer for a single uncompressed column type end to end
- [ ] Implement bulk insert batching into chunk groups and stripes with the
      stripe row limit
- [ ] Implement the row number to item pointer mapping in section 6
- [ ] Implement a sequential scan that reads all columns of all stripes
- [ ] Write phase 1 tests: create, insert, count, scan, drop, and metapage
      format checks
- [ ] Verify a table written by phase 1 is readable by the existing extension and
      the reverse, as a format compatibility check during development only

### Phase 2, compression and projection

- [ ] Implement encode and decode for none and pglz
- [ ] Implement encode and decode for lz4
- [ ] Implement encode and decode for zstd with a compression level
- [ ] Store the compression type and level per chunk and fall back to
      uncompressed when compression does not help
- [ ] Implement the exists stream for nulls
- [ ] Implement column projection so only referenced columns are read
- [ ] Compute and store per chunk minimum and maximum values for orderable types
- [ ] Implement chunk group skipping from minimum and maximum values
- [ ] Write phase 2 tests for each compression type, projection, and skipping

### Phase 3, update and delete

- [ ] Create the row_mask table and row_mask_seq and their indexes per section 7
- [ ] Implement delete by marking rows in the row mask
- [ ] Implement update as delete plus insert with correct row numbers
- [ ] Apply the row mask during scans so deleted rows are not returned
- [ ] Implement snapshot visibility for stripes using first row number and the
      metapage reserved values
- [ ] Implement flush of pending writes at pre commit with an active snapshot
- [ ] Implement discard of pending writes and metadata on rollback and savepoint
      rollback
- [ ] Support temporary columnar tables, including the relation resolution
      fallback for temporary relations
- [ ] Write phase 3 tests for delete, update, rollback, savepoints, and temporary
      tables

### Phase 4, indexes and constraints

- [ ] Implement index build range scan for btree and hash
- [ ] Implement index fetch of a row by item pointer
- [ ] Enforce constraints on insert and multi insert
- [ ] Implement the heap to columnar and columnar to heap conversion function
- [ ] Ensure index only scans are not chosen, since there is no visibility map
- [ ] Write phase 4 tests for btree, hash, unique constraints, and conversion

### Phase 5, planner and vacuum

- [ ] Implement the custom scan path and node for columnar scans
- [ ] Implement qual pushdown into the scan for chunk skipping
- [ ] Implement per table options in the options table and the set and reset
      functions
- [ ] Implement the vacuum function that combines recent stripes and reclaims
      space from deleted rows
- [ ] Implement the stats function and vacuum_full
- [ ] Disable parallel sequential scans and pin planner behavior so plans are
      stable
- [ ] Write phase 5 tests for pushdown, options, vacuum, and stats

### Phase 6, vectorized execution

- [ ] Implement the vectorized scan and filter fast path
- [ ] Implement vectorized aggregates for count, sum, avg, min, and max
- [ ] Implement the optional decompressed chunk cache behind
      columnar.enable_column_cache
- [ ] Write phase 6 tests and a performance check against the scalar path

### Phase 7, release

- [ ] Validate the full suite passes on PostgreSQL 13 through 19 under assert
      enabled builds
- [ ] Run the benchmark suite and record results in the documentation
- [ ] Complete the patent review for the techniques used
- [ ] Have counsel review the clean-room record and the MIT license choice
- [ ] Write user and developer documentation under the pgColumnar name
- [ ] Tag and release under MIT

## 7. Risks and how they are handled

- Clean-room contamination. Handled by separating the specification and
  implementation roles and by keeping a provenance log. The weakest point is
  reusing a contaminated context for implementation; prefer a fresh one.
- Format drift. Handled by validating against the specification and by a
  development time round trip check with the existing extension.
- Test coverage gaps. Handled by writing fresh tests per phase and by using the
  documented behavioral contracts in specification section 9 as the checklist,
  not by copying upstream tests.
- Patent exposure. Handled by the phase 7 patent review before release.
- Version support regressions. Handled by keeping the assert enabled matrix from
  PostgreSQL 13 through 19 green at the end of every phase.
