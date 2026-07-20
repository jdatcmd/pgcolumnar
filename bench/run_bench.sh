#!/usr/bin/env bash
#
# pgColumnar benchmark harness.
#
# Builds and installs the extension into a throwaway PostgreSQL cluster, loads
# one identical dataset into a heap table and into columnar tables, and reports
# on-disk size and query latency for a representative query set. It also shows
# the effect of vectorized execution (columnar.enable_vectorization on/off) and
# of the compression codec (none vs zstd).
#
# Written fresh for pgColumnar. It does not reuse any prior benchmark script or
# any prior numbers. Run it against a NON-assert (non-cassert) build of
# PostgreSQL, since assertions distort timing.
#
# Usage:
#   bench/run_bench.sh [PG_CONFIG]
#
# Environment:
#   BENCH_SCALE   number of rows to load (default 6000000)
#   BENCH_PORT    cluster port (default 55432)
#   BENCH_REPS    timed repetitions per query, median reported (default 5)
#   BENCH_DUCKDB  set to 1 to add a DuckDB comparison if duckdb is on PATH
#
# Run as a user that may "runuser -u postgres" (e.g. root) when not postgres.

set -euo pipefail

PG_CONFIG="${1:-/usr/local/pg17_nc/bin/pg_config}"
BINDIR="$("$PG_CONFIG" --bindir)"
PORT="${BENCH_PORT:-55432}"
SCALE="${BENCH_SCALE:-6000000}"
REPS="${BENCH_REPS:-5}"
SRCDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

WORKDIR="$(mktemp -d /tmp/pgcolumnar-bench.XXXXXX)"
PGDATA="$WORKDIR/data"
LOGFILE="$WORKDIR/server.log"

echo "== pgColumnar benchmark =="
echo "PG_CONFIG=$PG_CONFIG  ($("$PG_CONFIG" --version))"
echo "scale=$SCALE rows  reps=$REPS  workdir=$WORKDIR"

# ---- build and install -----------------------------------------------------
echo "-- building (non-assert expected)"
# Clean first so objects are never reused across majors: the same source file
# compiles differently per PG_VERSION_NUM, and a stale object linked against a
# different major's headers fails to load (for example an undefined planner
# hook symbol). PGXS provides the clean target.
make -C "$SRCDIR" PG_CONFIG="$PG_CONFIG" clean >/dev/null 2>&1 || true
make -C "$SRCDIR" PG_CONFIG="$PG_CONFIG" >/dev/null
make -C "$SRCDIR" install PG_CONFIG="$PG_CONFIG" >/dev/null

# ---- run cluster commands as postgres when root ----------------------------
if [ "$(id -u)" = "0" ]; then
	RUNPG=(runuser -u postgres --)
	chown -R postgres "$WORKDIR"
else
	RUNPG=(env)
fi
run_pg() { "${RUNPG[@]}" env PATH="$BINDIR:$PATH" bash -lc "$1"; }

cleanup() {
	run_pg "pg_ctl -D '$PGDATA' stop -m immediate -w" >/dev/null 2>&1 || true
	rm -rf "$WORKDIR"
}
trap cleanup EXIT

# ---- start a throwaway cluster ---------------------------------------------
echo "-- initdb and start"
run_pg "initdb -D '$PGDATA' -A trust" >/dev/null 2>&1
{
	echo "port=$PORT"
	echo "shared_preload_libraries='columnar'"
	echo "shared_buffers=512MB"
	echo "work_mem=64MB"
	echo "maintenance_work_mem=256MB"
	echo "max_wal_size=8GB"
	echo "checkpoint_timeout=30min"
	echo "max_parallel_workers_per_gather=0"
} | run_pg "cat >> '$PGDATA/postgresql.conf'"
run_pg "pg_ctl -D '$PGDATA' -l '$LOGFILE' start -w" >/dev/null
run_pg "createdb -p $PORT bench"

PSQL="psql -p $PORT -d bench -v ON_ERROR_STOP=1"

# ---- load an identical dataset into heap and columnar tables ---------------
# Data is generated once into an unlogged staging table and copied into each
# target so every table holds byte-identical rows. Columns:
#   id   bigint  1..N, unique, monotonic (point/range lookups, index)
#   k    int     equal to id, monotonic (drives min/max chunk-group skipping)
#   val  int     pseudo-random 0..9999 (sum/avg over an integer column)
#   cat  int     id mod 50, low cardinality (a second aggregate column)
#   ts   timestamptz  monotonic
#   c1,c2,c3  text  filler so the table is "wide" and projection matters
echo "-- loading $SCALE rows (staging, heap, columnar zstd, columnar none)"
run_pg "$PSQL -q -c \"CREATE EXTENSION columnar;\""

run_pg "$PSQL -q -c \"
CREATE UNLOGGED TABLE stage AS
SELECT g::bigint                                   AS id,
       g::int                                      AS k,
       ((g * 2654435761) % 10000)::int             AS val,
       (g % 50)::int                               AS cat,
       TIMESTAMPTZ '2020-01-01' + (g * interval '1 second') AS ts,
       'name-'    || (g % 1000)::text              AS c1,
       'region-'  || (g % 20)::text                AS c2,
       repeat('x', 16)                             AS c3
FROM generate_series(1, $SCALE) g;\""

run_pg "$PSQL -q -c \"
CREATE TABLE h (LIKE stage);
INSERT INTO h SELECT * FROM stage;
CREATE INDEX h_id ON h (id);
ANALYZE h;\""

run_pg "$PSQL -q -c \"
SET columnar.compression = 'zstd';
CREATE TABLE cz (LIKE stage) USING columnar;
INSERT INTO cz SELECT * FROM stage;
CREATE INDEX cz_id ON cz (id);
ANALYZE cz;\""

run_pg "$PSQL -q -c \"
SET columnar.compression = 'none';
CREATE TABLE cn (LIKE stage) USING columnar;
INSERT INTO cn SELECT * FROM stage;
ANALYZE cn;\""

run_pg "$PSQL -q -c \"DROP TABLE stage;\""

# range bounds derived from scale
LO=$(( SCALE * 80 / 100 ))
HI=$(( SCALE * 82 / 100 ))
MID=$(( SCALE / 2 ))
PROJ=$(( SCALE / 100 ))

# ---- the benchmark SQL -----------------------------------------------------
# bench_time(q, n) runs q once to warm caches, then n more times, returning the
# median wall-clock milliseconds measured server-side around EXECUTE.
echo "-- running queries"
run_pg "$PSQL <<SQL
\\set QUIET on
\\pset pager off
SET max_parallel_workers_per_gather = 0;

CREATE OR REPLACE FUNCTION bench_time(q text, n int) RETURNS numeric AS \\\$\\\$
DECLARE t0 timestamptz; times numeric[] := '{}'; i int;
BEGIN
	EXECUTE q;                          -- warm-up, discarded
	FOR i IN 1..n LOOP
		t0 := clock_timestamp();
		EXECUTE q;
		times := array_append(times,
			extract(epoch FROM clock_timestamp() - t0) * 1000);
	END LOOP;
	RETURN round((SELECT percentile_cont(0.5) WITHIN GROUP (ORDER BY t)
	              FROM unnest(times) t)::numeric, 2);
END \\\$\\\$ LANGUAGE plpgsql;

-- ---------------------------------------------------------------- sizes
\\echo
\\echo === TABLE SIZES ===
SELECT format('%-22s', label) AS table,
       pg_size_pretty(bytes)  AS total_size,
       bytes
FROM (
	SELECT 'heap'               AS label, pg_total_relation_size('h')  AS bytes
	UNION ALL SELECT 'columnar (zstd)', pg_total_relation_size('cz')
	UNION ALL SELECT 'columnar (none)', pg_total_relation_size('cn')
) s ORDER BY bytes DESC;

\\echo
\\echo (table-only size, excluding indexes)
SELECT format('%-22s', label) AS table, pg_size_pretty(bytes) AS table_size
FROM (
	SELECT 'heap'               AS label, pg_table_size('h')  AS bytes
	UNION ALL SELECT 'columnar (zstd)', pg_table_size('cz')
	UNION ALL SELECT 'columnar (none)', pg_table_size('cn')
) s ORDER BY bytes DESC;

-- ---------------------------------------------------------------- latency
\\echo
\\echo === QUERY LATENCY: heap vs columnar zstd (median ms of $REPS) ===
WITH q(id, descr, sql_h, sql_c) AS (VALUES
 (1, 'count(*) full table',
     'SELECT count(*) FROM h',
     'SELECT count(*) FROM cz'),
 (2, 'sum/avg over one int column',
     'SELECT sum(val), avg(val) FROM h',
     'SELECT sum(val), avg(val) FROM cz'),
 (3, 'filtered agg, min/max-skippable range',
     'SELECT sum(val) FROM h  WHERE k BETWEEN $LO AND $HI',
     'SELECT sum(val) FROM cz WHERE k BETWEEN $LO AND $HI'),
 (4, 'point lookup by indexed id',
     'SELECT * FROM h  WHERE id = $MID',
     'SELECT * FROM cz WHERE id = $MID'),
 (5, 'projection: 3 of 8 cols, 1% filter',
     'SELECT id, val, cat FROM h  WHERE k < $PROJ',
     'SELECT id, val, cat FROM cz WHERE k < $PROJ')
)
SELECT id, query, heap_ms, columnar_ms,
       round(heap_ms / nullif(columnar_ms,0), 2) AS heap_over_col
FROM (
	SELECT id,
	       format('%-38s', descr)   AS query,
	       bench_time(sql_h, $REPS) AS heap_ms,
	       bench_time(sql_c, $REPS) AS columnar_ms
	FROM q
) t ORDER BY id;

-- ---------------------------------------------------------------- vectorize
\\echo
\\echo === VECTORIZATION on vs off (columnar zstd, median ms) ===
SELECT descr AS query, on_ms, off_ms,
       round(off_ms / nullif(on_ms,0), 2) AS speedup_vec
FROM (
 SELECT 'sum/avg over int'::text AS descr,
   (SELECT set_config('columnar.enable_vectorization','on',false)) AS s1,
   bench_time('SELECT sum(val), avg(val) FROM cz', $REPS) AS on_ms,
   (SELECT set_config('columnar.enable_vectorization','off',false)) AS s2,
   bench_time('SELECT sum(val), avg(val) FROM cz', $REPS) AS off_ms
 UNION ALL
 SELECT 'filtered agg (range)'::text,
   set_config('columnar.enable_vectorization','on',false),
   bench_time('SELECT sum(val) FROM cz WHERE k BETWEEN $LO AND $HI', $REPS),
   set_config('columnar.enable_vectorization','off',false),
   bench_time('SELECT sum(val) FROM cz WHERE k BETWEEN $LO AND $HI', $REPS)
) v;
SET columnar.enable_vectorization = on;

-- ---------------------------------------------------------------- codec
\\echo
\\echo === COMPRESSION none vs zstd (size and scan latency) ===
SELECT 'total size' AS metric,
       pg_size_pretty(pg_table_size('cn')) AS none,
       pg_size_pretty(pg_table_size('cz')) AS zstd,
       round(pg_table_size('cn')::numeric / nullif(pg_table_size('cz'),0),2)
                                            AS none_over_zstd;

SELECT 'sum/avg scan ms' AS metric,
       bench_time('SELECT sum(val), avg(val) FROM cn', $REPS)::text AS none,
       bench_time('SELECT sum(val), avg(val) FROM cz', $REPS)::text AS zstd,
       ''::text AS none_over_zstd
UNION ALL
SELECT 'count(*) ms',
       bench_time('SELECT count(*) FROM cn', $REPS)::text,
       bench_time('SELECT count(*) FROM cz', $REPS)::text,
       '';
SQL
"

# ---- sorted projection (gap 26) --------------------------------------------
# A key uncorrelated with insert order gets no chunk-group skipping until the
# table is stored sorted on it. Measure a narrow range scan before and after.
echo "-- sorted projection"
run_pg "$PSQL <<SQL
\\pset pager off
SET max_parallel_workers_per_gather = 0;
CREATE TABLE sp (id bigint, sortk int, val int) USING columnar;
INSERT INTO sp SELECT g, ((g * 2654435761) % 1000000)::int, (g % 1000)::int
FROM generate_series(1, $SCALE) g;
ANALYZE sp;

\\echo
\\echo === SORTED PROJECTION: narrow range scan on a scattered key (median ms) ===
SELECT 'before vacuum_sorted' AS state,
       bench_time('SELECT count(*), sum(val) FROM sp WHERE sortk BETWEEN 500000 AND 501000', $REPS) AS ms;
SELECT columnar.vacuum_sorted('sp', 'sortk');
ANALYZE sp;
SELECT 'after vacuum_sorted'  AS state,
       bench_time('SELECT count(*), sum(val) FROM sp WHERE sortk BETWEEN 500000 AND 501000', $REPS) AS ms;
SQL
"

# ---- export to Arrow and Parquet (gap 27) ----------------------------------
# Export a table of exportable-typed columns and report wall time, file size,
# and throughput. cz has a timestamptz column, so a projection is used.
echo "-- export"
run_pg "$PSQL <<SQL
CREATE TABLE ex (id bigint, k int, val int, cat int, c1 text) USING columnar;
INSERT INTO ex SELECT id, k, val, cat, c1 FROM cz;

CREATE OR REPLACE FUNCTION bench_export(q text) RETURNS numeric AS \\\$\\\$
DECLARE t0 timestamptz;
BEGIN
	t0 := clock_timestamp();
	EXECUTE q;
	RETURN round(extract(epoch FROM clock_timestamp() - t0) * 1000, 1);
END \\\$\\\$ LANGUAGE plpgsql;

\\echo
\\echo === EXPORT: Arrow IPC and Parquet ($SCALE rows, 5 columns) ===
SELECT format('%-8s', format) AS format, ms,
       pg_size_pretty(bytes) AS file_size,
       round(($SCALE / 1000000.0) / (ms / 1000.0), 1) AS m_rows_per_s
FROM (
	SELECT 'arrow' AS format,
	       bench_export('SELECT columnar.export_arrow(''ex'', ''$WORKDIR/ex.arrows'')') AS ms,
	       (pg_stat_file('$WORKDIR/ex.arrows')).size AS bytes
	UNION ALL
	SELECT 'parquet',
	       bench_export('SELECT columnar.export_parquet(''ex'', ''$WORKDIR/ex.parquet'')'),
	       (pg_stat_file('$WORKDIR/ex.parquet')).size
) e ORDER BY format;
SQL
"

# ---- optional DuckDB comparison --------------------------------------------
if [ "${BENCH_DUCKDB:-0}" = "1" ] && command -v duckdb >/dev/null 2>&1; then
	echo
	echo "=== DuckDB comparison (same data, in-process columnar engine) ==="
	CSV="$WORKDIR/data.csv"
	run_pg "$PSQL -q -c \"COPY (SELECT id,k,val,cat FROM h) TO '$CSV' WITH (FORMAT csv, HEADER)\"" || true
	if [ -f "$CSV" ]; then
		duckdb -c "
CREATE TABLE d AS SELECT * FROM read_csv_auto('$CSV');
.timer on
SELECT count(*) FROM d;
SELECT sum(val), avg(val) FROM d;
SELECT sum(val) FROM d WHERE k BETWEEN $LO AND $HI;
" || echo "(duckdb run skipped)"
	fi
else
	echo
	echo "(DuckDB comparison not run; set BENCH_DUCKDB=1 with duckdb on PATH to enable)"
fi

echo
echo "== benchmark complete =="
