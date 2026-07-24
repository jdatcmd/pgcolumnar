# Phase G Parquet follow-ons -- session handoff

Written 2026-07-24. Captures the state of the Phase G external-Parquet read work
so the next session can resume without rediscovery.

## Where main is

`main` tip is the merge of #111. The external-Parquet read surface and its
follow-ons are all merged, and the full 15 through 19 matrix passed on the
combined set (all `native_parquet_*` suites green on every major). Merged this
session:

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

## Open / in-flight

- **#112** docs audit -- OPEN, awaiting review. Documents the whole read surface
  and corrects stale claims (uuid/numeric import were wrongly listed as
  unsupported; only Snappy was listed as a read codec). Docs only, no gate needed.
- **`phase-g/multifile-isreg`** (local branch, commit `69e0e52`) -- the one loose
  end. Follow-up to #111's two non-blocking review notes: `pq_resolve_paths` now
  requires a regular file (`S_ISREG`) in both the directory and glob branches, so
  a subdirectory named `foo.parquet` or a glob catching a directory is skipped
  rather than reaching the parser. Committed, tested on PG18 via devloop (passes,
  includes two new tests), **not yet PG18+19 gated and not PR'd**. RESUME HERE:
  gate it (`run_all_versions.sh <pg18> <pg19>`), push, open the PR, merge.

## Gating cadence (owner's rule this session)

- Per PR: build + suites on **PG18 and PG19 only** (fast; catches the version
  breaks that bite -- PG18 API renames, PG19 beta). See [[parquet-followon-gating]].
- Full **15 through 19** matrix once per feature completion, not per PR.
- Always run gates on an **idle** container; concurrent builds make results
  unreadable (cost a wrong PG17 failure this session). Verify the tree before
  launching a gate (a matrix once started on the wrong branch).

## Dev environment

Container `pgcolumnar-dev` (clone of `plexcellent`), PG 13-19 under
`/usr/local/pgNN` (pg18 is `pg18_nc`, pg19 is `pg19`; the matrix default set uses
`/usr/local/pgsql` for pg18). RO repo mount at `/root/pgcolumnar_host`. Build in a
copy, never the mount. Standard loop:
`PGC_BUILD=/root/b bash /root/pgcolumnar_host/test/devloop.sh <pg_config> <suite>...`

## Deferred follow-ons (documented, not built)

Within Parquet: Hive-style partition pruning (`col=value` dirs as virtual
columns), recursive directory walk, streaming (each file is read fully into
memory), INT32/INT64-backed DECIMAL reads, per-file schema-uniformity validation
in `parquet_schema` (it describes the first file only), json/jsonb import.
Beyond Parquet: ORC, and open table formats (Iceberg, Delta, Hudi). All noted in
`design/ROADMAP.md` future directions and `docs/limitations.md`.

## Review pattern worth remembering

ChronicallyJD caught three real crafted-file bugs this session (FLBA DECIMAL
scale stack overflow, codec unvalidated `uncompressed_size`, and earlier the NaN
and inverted-stats skips), all the same class: a file-declared value used without
a range check. Every new "consume a header/footer field" path needs the guard up
front, and a regression test verified to FAIL on the pre-fix code (confirm the
negative case actually fires -- a heredoc stub silently no-op'd twice this
session and nearly gave false confidence).
