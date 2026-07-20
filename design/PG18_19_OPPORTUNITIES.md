# PostgreSQL 18 and 19 opportunities for pgColumnar

Features in PostgreSQL 17 through 19 that are not in older releases and that
pgColumnar can use. The support matrix is PostgreSQL 13-19, so anything adopted
must be version-gated with `#if PG_VERSION_NUM` and fall back to the current path
on older majors, in the style of `src/columnar_compat.h`.

Sources: PostgreSQL 18 release notes (postgresql.org/docs/18/release-18.html);
PostgreSQL 19 beta 1/2 announcements; read_stream.h and read_stream.c
(doxygen.postgresql.org); pgsql-hackers "Trying out read streams in pgvector (an
extension)" and "Allow ReadStream to be consumed as raw block numbers".

> Status (2026-07): item 1 (read stream / AIO) is shipped
> (`columnar.enable_read_stream`, gap 29). Items 2 and 3 are covered by
> `test/generated_columns.sh` and `test/temporal.sh`. Item 5 (REPACK) has been
> investigated; see the note under that section.

## 1. Read Stream API + asynchronous I/O — highest value

- **Availability.** The read stream API (`storage/read_stream.h`,
  `read_stream_begin_relation`, `read_stream_next_buffer`) landed in PostgreSQL 17
  and drives sequential scans and ANALYZE. PostgreSQL 18 added the asynchronous
  I/O subsystem (`io_method` = sync | worker | io_uring, `io_combine_limit`,
  `io_max_combine_limit`, `pg_aios`), and the read stream is the interface that
  feeds it. Reported up to 3x on reads from storage.
- **Current state in pgColumnar.** The reader fetches each chunk's value and
  exists stream blocks with individual `ReadBuffer` calls, synchronously, one
  chunk group at a time (`src/columnar_reader.c`). There is no prefetch, so a
  cold scan waits on each block in turn.
- **Opportunity.** Drive the block reads for a stripe/chunk-group scan through a
  read stream. The block numbers a columnar scan needs are known ahead of time
  from the stripe/chunk catalog, which is exactly the case the read stream (and
  `read_stream_next_block` for callers that compute their own block numbers) is
  built for. On PostgreSQL 18 this gets AIO prefetch for free; on 17 it gets
  posix_fadvise-based prefetch; on 13-16 it falls back to the current path.
- **Effort / risk.** Medium. The API shape has moved between 17, 18, and 19, so
  the adoption must be behind a compat shim and validated on each major. Risk is
  confined to the read path and covered by the differential/recovery suites.
- **Why first.** It is the largest cold-scan performance lever available and maps
  directly onto how pgColumnar already plans its block reads.

## 2. Virtual generated columns (PostgreSQL 18, now the default)

- Generated columns can be virtual and are virtual by default; their values are
  computed at read time rather than stored.
- **Relevance.** A columnar table with a virtual generated column must return the
  computed value on read and must not store a chunk for it. Confirm the table AM
  and custom scan handle read-time generation correctly, and add differential
  coverage (columnar vs heap) for stored and virtual generated columns on
  PostgreSQL 18+. Likely handled at the executor level, but it is unverified.
- **Effort.** Small (a correctness check plus a test), version-gated to 18+.
- **Verified (`test/generated_columns.sh`).** Stored and virtual generated
  columns both read correctly on a columnar table across the matrix; the executor
  recomputes the virtual value on read, so values match the heap oracle and the
  generation expression. One finding: pgColumnar currently *materializes an
  all-null chunk* for a virtual generated column at insert rather than skipping
  its storage. The read-time value overrides it, so this is a storage
  inefficiency, not a correctness problem. Skipping the write for
  `attgenerated = 'v'` columns (and returning NULL for them from the reader) is a
  worthwhile future write-path optimization; it needs matching reader/vacuum
  changes and its own coverage, so it is not bundled here.

## 3. Temporal constraints (PostgreSQL 18 `WITHOUT OVERLAPS`, PostgreSQL 19 `FOR PORTION OF`)

- PostgreSQL 18 allows non-overlapping PRIMARY KEY/UNIQUE (`WITHOUT OVERLAPS`) and
  temporal foreign keys (`PERIOD`); PostgreSQL 19 adds `FOR PORTION OF` updates.
- **Relevance.** These run through the index and constraint machinery pgColumnar
  already integrates with. Verify enforcement on a columnar table and add
  coverage; do not assume it works. Effort small, version-gated.

## 4. btree skip scan (PostgreSQL 18)

- The btree AM can skip leading index columns. Indexes built on columnar tables
  benefit automatically; no pgColumnar code change. Worth a benchmark line and a
  test that a multicolumn index on a columnar table plans a skip scan on 18+.

## 5. REPACK, concurrent (PostgreSQL 19) — investigate

- PostgreSQL 19 adds `REPACK` for concurrent table repacking, a lower-lock
  alternative to `VACUUM FULL`/`CLUSTER`.
- **Relevance.** pgColumnar's `columnar.vacuum`/`vacuum_sorted` rewrite takes
  `AccessExclusiveLock`. If `REPACK` dispatches through the table AM (or offers an
  extension hook), pgColumnar could offer concurrent compaction. Whether it is
  table-AM-extensible is unknown; the first task is to read the REPACK code/tableam
  wiring in the 19 tree and decide feasibility. Do not promise it until confirmed.
- **Investigated (PostgreSQL 19 headers).** REPACK is not a new table-AM callback.
  `commands/repack.h` shows it reuses the CLUSTER machinery (`cluster_rel`,
  `make_new_heap`, `finish_heap_swap`), which dispatches a table rewrite through
  the existing `relation_copy_for_cluster` table-AM callback. pgColumnar already
  implements that callback (`columnar_relation_copy_for_cluster`), so the
  non-concurrent `REPACK` (its default, under AccessExclusiveLock) runs through
  the same path as `CLUSTER`/`VACUUM FULL` and should work; this is worth a
  direct test. The concurrent variant (`CLUOPT_CONCURRENT`,
  ShareUpdateExclusiveLock) captures concurrent changes with logical-decoding
  workers (`commands/repack_internal.h`) rather than through a table-AM entry
  point, so it depends on logical decoding of the relation's changes and is not
  something the AM opts into. Concurrent REPACK on a columnar table is therefore
  unverified and likely needs additional work; treat it as future work.

## 6. Optimizer statistics injection (PostgreSQL 18)

- `pg_restore_relation_stats()`, `pg_restore_attribute_stats()`,
  `pg_clear_relation_stats()`, `pg_clear_attribute_stats()` let code set per-
  relation and per-column stats. pgColumnar could seed planner statistics that
  reflect columnar reality (per-column distinct/min/max already in the catalog),
  improving plan choice. Optional, additive, version-gated to 18+.

## Noted, out of scope for pgColumnar

SQL/PGQ property graphs, `ON CONFLICT DO SELECT`, `GROUP BY ALL`, `IGNORE NULLS`,
native JSON `COPY TO`, LZ4 as the default TOAST codec, `pg_plan_advice`. These are
server or SQL-surface features that do not change what a columnar table AM does.
`COPY TO ... (FORMAT json)` (PostgreSQL 19) is unrelated to the Arrow/Parquet
export work in gap 27.

## Suggested order

1. Read stream / AIO adoption in the scan (item 1) — flagship performance work.
2. Correctness coverage for virtual generated columns and temporal constraints
   (items 2, 3) — small, closes PG18/19 gaps.
3. Investigate REPACK feasibility (item 5).
4. Statistics injection and a skip-scan benchmark line (items 6, 4) as smaller
   follow-ups.
