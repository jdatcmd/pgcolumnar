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
