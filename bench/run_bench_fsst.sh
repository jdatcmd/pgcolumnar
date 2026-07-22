#!/usr/bin/env bash
#
# pgColumnar FSST ingestion micro-benchmark (Phase E3 gate).
#
# The main benchmark (run_bench.sh) never exercises FSST: its text columns are
# low cardinality, so dict wins and the FSST cost guard skips the symbol-table
# build. This harness loads the case FSST is actually for -- a high-cardinality
# column whose values are distinct but share frequent substrings (URL / log-line
# shaped) -- and times ingestion, which is where the per-vector symbol-table
# build lands. Its output is the number Phase E3 is gated on (design/PHASE_E3_PLAN.md).
#
# To isolate FSST's cost, run it twice against two builds of the same branch:
#   1. as shipped (FSST enabled)
#   2. with FSST disabled (a one-line `return false;` at the top of encode_fsst),
#      rebuilt -- the column then falls to none/dict.
# The ingestion-time delta is FSST's cost; the size delta is the ratio it buys.
#
# Written fresh for pgColumnar. Run against a NON-assert build (assertions
# distort timing). Run as a user that may "runuser -u postgres" (e.g. root).
#
# Usage:   bench/run_bench_fsst.sh [PG_CONFIG]
# Environment:
#   BENCH_SCALE  rows to load (default 3000000)
#   BENCH_PORT   cluster port (default 55462)

set -euo pipefail

PG_CONFIG="${1:-/usr/local/pg17_nc/bin/pg_config}"
BINDIR="$("$PG_CONFIG" --bindir)"
PORT="${BENCH_PORT:-55462}"
SCALE="${BENCH_SCALE:-3000000}"
SRCDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

WORKDIR="$(mktemp -d /tmp/pgcolumnar-fsstbench.XXXXXX)"
PGDATA="$WORKDIR/data"
LOGFILE="$WORKDIR/server.log"

echo "== pgColumnar FSST ingestion micro-benchmark =="
echo "PG_CONFIG=$PG_CONFIG  ($("$PG_CONFIG" --version))"
echo "scale=$SCALE rows  workdir=$WORKDIR"

echo "-- building (non-assert expected)"
make -C "$SRCDIR" PG_CONFIG="$PG_CONFIG" clean >/dev/null 2>&1 || true
make -C "$SRCDIR" PG_CONFIG="$PG_CONFIG" >/dev/null
make -C "$SRCDIR" install PG_CONFIG="$PG_CONFIG" >/dev/null

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

echo "-- initdb and start"
run_pg "initdb -D '$PGDATA' -A trust" >/dev/null 2>&1
{
	echo "port=$PORT"
	echo "shared_preload_libraries='pgcolumnar'"
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
run_pg "$PSQL -q -c \"CREATE EXTENSION pgcolumnar;\""

# High-cardinality, shared-substring text: every row distinct (g and an md5), but
# heavy shared structure (scheme, host, path, query key) -- exactly what FSST
# compresses and dict declines. compression=none so we measure the encoding, not
# a block codec on top of it.
echo "-- staging $SCALE rows of shared-substring text"
run_pg "$PSQL -q -c \"
CREATE UNLOGGED TABLE stage AS
SELECT g::bigint AS id,
       'https://www.example.com/user/' || g || '/profile?id=' ||
         md5(g::text) || '&ref=columnar&lang=en-US' AS url
FROM generate_series(1, $SCALE) g;\""

# Server-side timing via clock_timestamp(), so no dependence on psql \timing
# formatting; the elapsed milliseconds are RAISEd as a NOTICE and grepped out.
time_insert() {  # $1 = target table, prints elapsed ms
	run_pg "$PSQL -qtA -c \"
DO \\\$\\\$
DECLARE t0 timestamptz; ms numeric;
BEGIN
  t0 := clock_timestamp();
  INSERT INTO $1 SELECT * FROM stage;
  ms := extract(epoch from clock_timestamp() - t0) * 1000;
  RAISE NOTICE 'INGEST_MS %', round(ms, 1);
END \\\$\\\$;\" 2>&1" | grep -oE 'INGEST_MS [0-9.]+' | grep -oE '[0-9.]+' | tail -1
}

echo "-- timing heap ingestion (baseline)"
run_pg "$PSQL -q -c \"CREATE TABLE h (LIKE stage);\""
heap_ms=$(time_insert h)

echo "-- timing columnar (compression=none) ingestion"
run_pg "$PSQL -q \
	-c \"CREATE TABLE c (LIKE stage) USING pgcolumnar;\" \
	-c \"SELECT pgcolumnar.alter_columnar_table_set('c', compression => 'none');\""
col_ms=$(time_insert c)

# Order-independent fingerprint (no giant string_agg): count, total length, and
# xor of per-row md5 hashes catch any byte-level corruption cheaply.
echo "-- verify round-trip (heap vs columnar fingerprint)"
FP="SELECT count(*), sum(length(url))::bigint, bit_xor(('x'||substr(md5(id||'|'||url),1,16))::bit(64)::bigint)"
hh=$(run_pg "$PSQL -qtA -c \"$FP FROM h;\"")
ch=$(run_pg "$PSQL -qtA -c \"$FP FROM c;\"")

echo "-- sizes and FSST usage"
hsize=$(run_pg "$PSQL -qtA -c \"SELECT pg_table_size('h');\"")
csize=$(run_pg "$PSQL -qtA -c \"SELECT pg_table_size('c');\"")
fsst_chunks=$(run_pg "$PSQL -qtA -c \"
SELECT count(*) FROM pgcolumnar.column_chunk
 WHERE storage_id = pgcolumnar.get_storage_id('c')
   AND column_index = 1
   AND get_byte(encoding_descriptor, 6) = 8;\"")
total_chunks=$(run_pg "$PSQL -qtA -c \"
SELECT count(*) FROM pgcolumnar.column_chunk
 WHERE storage_id = pgcolumnar.get_storage_id('c')
   AND column_index = 1;\"")

echo
echo "===================== FSST INGESTION GATE ======================"
printf '  %-28s %s\n' "rows"                 "$SCALE"
printf '  %-28s %s ms\n' "heap INSERT"       "${heap_ms:-?}"
printf '  %-28s %s ms\n' "columnar INSERT"   "${col_ms:-?}"
printf '  %-28s %s\n' "round-trip"           "$([ "$hh" = "$ch" ] && echo MATCH || echo MISMATCH)"
printf '  %-28s %s bytes\n' "heap size"      "${hsize:-?}"
printf '  %-28s %s bytes\n' "columnar size"  "${csize:-?}"
printf '  %-28s %s / %s (first-vector = FSST)\n' "url chunks using FSST" "${fsst_chunks:-?}" "${total_chunks:-?}"
echo "================================================================"
echo "Run once as shipped and once with encode_fsst stubbed to return false;"
echo "the columnar-INSERT delta is FSST's ingestion cost, the size delta its ratio."
