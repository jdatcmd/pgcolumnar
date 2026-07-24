# Phase G Parquet follow-ons -- session handoff

Written 2026-07-24, updated the same day after the streaming work merged.
Captures the state of the Phase G external-Parquet read work so the next session
can resume without rediscovery.

## Where main is

`main` tip is `d364c62`, the merge of #117. The external-Parquet read surface, its
follow-ons, the docs audit, the multi-file hardening, and streaming reads are all
merged. There are no open PRs, no open issues, and no unmerged local branches.
Merged across this work:

- **#106** FLBA read: uuid (16-byte fixed binary), numeric via DECIMAL (fixed or
  variable big-endian bytes, precision <= 38), fixed bytea. INT32/INT64-backed
  DECIMAL is deferred (schema does not advertise it, bind fails cleanly).
- **#107** codecs: GZIP (zlib), ZSTD, LZ4_RAW added to uncompressed and Snappy.
  One `pq_decompress` dispatch; file-declared sizes bounded to `MaxAllocSize`.
- **#108** `test/devloop.sh`: atomic sync + clean rebuild (`test/rebuild.sh`) +
  run. Use this, not a manual `cp` + `PGC_SKIP_BUILD=1`, or you test a stale `.so`.
- **#109** column projection pushdown: needed columns computed at plan time from
  the base rel's reltarget + quals (not the exec target list, which may be
  physical), passed via `fdw_private`; `count(*)` decodes zero columns. EXPLAIN
  shows Columns Read/Total.
- **#110** `test/rebuild.sh` fails on any compiler warning (the matrix gate
  rejects warnings; the inner loop must too). `PGC_ALLOW_WARNINGS=1` overrides.
- **#111** multi-file: a directory or glob reads its `*.parquet` files as one
  relation, sorted, deterministic. Per-file predicate pushdown, projection once,
  a "Files" EXPLAIN counter. All four read surfaces loop over `pq_resolve_paths`.
- **#113** `pq_resolve_paths` requires a regular file (`pq_is_regular_file`,
  `stat` + `S_ISREG`) in both the directory and glob branches, so a subdirectory
  named `foo.parquet` or a glob catching a directory is skipped rather than
  reaching the parser. A glob whose matches are all filtered out errors. Also adds
  `native_parquet_multifile` to the matrix (see the coverage lesson below).
- **#116** path classification: a `stat` failure (dangling symlink, unsearchable
  parent) is kept so the open reports it, rather than skipped silently; only
  directories and other known non-files are skipped, which also keeps a FIFO away
  from `AllocateFile`, where `open(2)` blocks and `SA_RESTART` defeats cancel.
  Closes #114.
- **#117** streaming reads: a `PqSource` holds only the footer and pages are read
  as the decoder reaches them, so peak raw memory is one page. A file of 1GB or
  more previously could not be read at all (the whole-file `palloc` exceeded
  `MaxAllocSize`); that ceiling is gone, a row group skipped by pushdown now
  costs no I/O, and `parquet_schema` reads no page bytes. Five page guards, four
  proven by removal with `test/mutate_guard.py`.
- **#112** docs audit: documents the whole read surface across `features.md`,
  `sql-reference.md`, `user-guide.md`, `limitations.md`, `ARCHITECTURE.md`, and
  `testing.md`, and corrects claims that had gone stale. The review found five
  real gaps, all fixed before merge: the `numeric` round trip needs
  `numeric(p,s)` with `p <= 38` (an unconstrained `numeric` is exported as text
  and cannot be imported back); `installation.md` needed zlib, the codec-fallback
  claim scoped to the native format, the little-endian requirement extended to the
  read surface, and its version range corrected to 15 through 19; the column
  definition list must cover every leaf column (a subset is an error, not a
  projection); the row-group skipping preconditions were undocumented; and the
  four in-database `COMMENT` strings still described a single Parquet file.

Gate on both: PostgreSQL 18.4 and 19beta2, all 61 suites, no failures.

## Open

Nothing. #114 is closed by #116, and #117 landed streaming. The next substantive
work is a roadmap choice, not a Parquet loose end; see `design/ROADMAP.md`, whose
remaining Parquet items are Hive-style partition pruning, a recursive directory
walk, and INT32/INT64-backed DECIMAL reads.

One thing is deliberately left unproven and is recorded so it is not rediscovered
as a gap: the v2 level-length guard in `decode_leaf_entries` has no test that
isolates it. Removing it does not change behaviour (the level decode fails on the
garbage it reads, and a negative `compressed_size - levLen` becomes a huge
`size_t` so `valbuf + vallen` wraps and bounds checks fail closed), and an ASAN
build does not see the over-read either, because `palloc` sub-allocates from a
larger block. A PostgreSQL built with `USE_VALGRIND` would show it. That is the
way to close it if anyone wants to.

## Gating cadence (owner's rule)

- Per PR: build + suites on **PG18 and PG19 only** (fast; catches the version
  breaks that bite -- PG18 API renames, PG19 beta). See [[parquet-followon-gating]].
- Full **15 through 19** matrix once per feature completion, not per PR.
- Always run gates on an **idle** container; concurrent builds make results
  unreadable (cost a wrong PG17 failure in an earlier session). Verify the tree
  before launching a gate (a matrix once started on the wrong branch).
- A docs-only PR skips the gate. A PR that touches `pgcolumnar--1.0.sql` does not,
  even when the change is only a `COMMENT` string; #112 was regated for that.

## Dev environment

Container `pgcolumnar-dev` (clone of `plexcellent`), PG 13-19 under
`/usr/local/pgNN` (pg18 is `pg18_nc`, pg19 is `pg19`; the matrix default set uses
`/usr/local/pgsql` for pg18). RO repo mount at `/root/pgcolumnar_host`. Build in a
copy, never the mount. Standard loop:
`PGC_BUILD=/root/b bash /root/pgcolumnar_host/test/devloop.sh <pg_config> <suite>...`

The `incus` CLI on the host works and captures output; the Incus MCP `ExecInstance`
returns an operation without the command's stdout, so prefer
`incus exec pgcolumnar-dev -- bash -lc '...'`.

Check `gh auth status` immediately before every push, merge, or comment meant to
come from the author. The active account sits on `ChronicallyJD` (the review
identity) at session start and reverts on its own; that produces a 403 on push, a
permissions error on `gh pr merge`, and a comment silently posted under the
reviewer account.

## Deferred follow-ons (documented, not built)

Within Parquet: Hive-style partition pruning (`col=value` dirs as virtual
columns), recursive directory walk, INT32/INT64-backed DECIMAL reads, per-file schema-uniformity validation
in `parquet_schema` (it describes the first file only), json/jsonb import.
Beyond Parquet: ORC, and open table formats (Iceberg, Delta, Hudi). All noted in
`design/ROADMAP.md` future directions and `docs/limitations.md`.

Row-group skipping is also narrower than a reader might assume, and is now
documented in `docs/limitations.md`: `Var op Const` only (no Param, which is what
lets the skip set be computed once per scan and reused across rescans), INT32,
INT64, FLOAT, and DOUBLE physical types only, an exact constant-type match, and
both statistics present and not inverted. Widening any of those is a real feature,
not a bug fix.

## Patterns worth remembering

- **A passing crafted-file test proves nothing by itself.** Three of the streaming
  guard tests passed for the wrong reason: one file was never built (the read
  failed with "could not open file", which is still an error), one was rejected by
  a different guard, and one was aimed at a case the loop already handled. Delete
  the specific guard, rebuild, and require that exactly its check fails.
  `test/mutate_guard.py` does this and refuses to run if the text it patches has
  moved, because it once silently tested an unmutated build.
- **A green full matrix is not coverage.** The all-null Parquet regression in #117
  passed 15 through 19 on every suite, because nothing in the matrix read an
  all-null Parquet column. When a guard rejects a shape, ask which real writer
  produces that shape and test it: pyarrow writes an all-null column with a
  dictionary page carrying zero entries.

- **Crafted-file guards.** ChronicallyJD caught three real crafted-file bugs
  across this work (FLBA DECIMAL scale stack overflow, codec unvalidated
  `uncompressed_size`, and earlier the NaN and inverted-stats skips), all the same
  class: a file-declared value used without a range check. Every new "consume a
  header/footer field" path needs the guard up front.
- **Verify the negative case.** A regression test must be confirmed to FAIL on the
  pre-fix code (a heredoc stub silently no-op'd twice and nearly gave false
  confidence). For #113 this was done by reverse-applying the source diff onto the
  branch, rebuilding on PG18, and watching both new tests fail with
  `could not size ".../sub.parquet": Invalid argument`.
- **A new suite is not covered until it is in the matrix.** #111 shipped
  `test/native_parquet_multifile.sh` without adding it to the `SUITES` list in
  `test/run_all_versions.sh`, so the directory and glob read path was exercised
  only by hand through devloop, while the matrix reported green on a set that did
  not include it. Adding a suite file is half the change; check `SUITES`.
