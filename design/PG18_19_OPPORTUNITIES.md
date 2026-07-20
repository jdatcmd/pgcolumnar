# PostgreSQL 18 and 19 opportunities for pgColumnar

Features in PostgreSQL 17 through 19 that are not in older releases and that
pgColumnar can use. The support matrix is PostgreSQL 13-19, so anything adopted
must be version-gated with `#if PG_VERSION_NUM` and fall back to the current path
on older majors, in the style of `src/columnar_compat.h`.

Sources: PostgreSQL 18 release notes (postgresql.org/docs/18/release-18.html);
PostgreSQL 19 beta 1/2 announcements; read_stream.h and read_stream.c
(doxygen.postgresql.org); pgsql-hackers "Trying out read streams in pgvector (an
extension)" and "Allow ReadStream to be consumed as raw block numbers".

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
