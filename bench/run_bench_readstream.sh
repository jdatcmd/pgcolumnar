#!/usr/bin/env bash
#
# pgColumnar read-stream / asynchronous-I/O benchmark (gap 29).
#
# Measures cold-cache scan latency across the PostgreSQL 18 io_method settings
# (sync, worker, io_uring) with the columnar read stream on and off. Because a
# columnar scan reads a whole stripe's blocks as one contiguous range, the read
# stream (PostgreSQL 17+) lets the AIO subsystem (PostgreSQL 18) prefetch them.
#
# Cold reads are forced without /proc/sys/vm/drop_caches: the cluster is
# restarted before each timed run (clearing shared_buffers) and the relation's
# files are dropped from the OS page cache with posix_fadvise(DONTNEED) via
# bench/evict. Run against a NON-assert server built --with-liburing for the
# io_uring method (e.g. /usr/local/pg18_uring).
#
# Usage:   bench/run_bench_readstream.sh [PG_CONFIG]
# Env:     BENCH_ROWS (default 60000000), BENCH_PORT (default 55490)
#
# Written fresh for pgColumnar.

set -uo pipefail

PG_CONFIG="${1:-/usr/local/pg18_uring/bin/pg_config}"
BINDIR="$("$PG_CONFIG" --bindir)"
PORT="${BENCH_PORT:-55490}"
ROWS="${BENCH_ROWS:-60000000}"
SRCDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORKDIR="$(mktemp -d /tmp/pgcolumnar-rsbench.XXXXXX)"
PGDATA="$WORKDIR/data"
LOG="$WORKDIR/server.log"

echo "== pgColumnar read-stream / AIO benchmark =="
echo "PG_CONFIG=$PG_CONFIG  ($("$PG_CONFIG" --version))"
echo "rows=$ROWS  workdir=$WORKDIR"

echo "-- build + install"
make -C "$SRCDIR" PG_CONFIG="$PG_CONFIG" clean >/dev/null 2>&1 || true
make -C "$SRCDIR" PG_CONFIG="$PG_CONFIG" >/dev/null
make -C "$SRCDIR" install PG_CONFIG="$PG_CONFIG" >/dev/null
cc -O2 -o "$WORKDIR/evict" "$SRCDIR/bench/evict.c"

if [ "$(id -u)" = "0" ]; then RUNPG=(runuser -u postgres --); chown -R postgres "$WORKDIR"; else RUNPG=(env); fi
run_pg() { "${RUNPG[@]}" env PATH="$BINDIR:$PATH" bash -lc "$1"; }
PSQL="psql -p $PORT -d bench -v ON_ERROR_STOP=1 -qAtX"

cleanup() { run_pg "pg_ctl -D '$PGDATA' stop -m immediate -w" >/dev/null 2>&1 || true; rm -rf "$WORKDIR"; }
trap cleanup EXIT

echo "-- initdb"
run_pg "initdb -D '$PGDATA' -A trust" >/dev/null 2>&1
{
	echo "port=$PORT"
	echo "shared_preload_libraries='columnar'"
	echo "shared_buffers=128MB"
	echo "max_parallel_workers_per_gather=0"
	echo "effective_io_concurrency=32"
	echo "maintenance_io_concurrency=32"
	echo "max_wal_size=8GB"
	echo "checkpoint_timeout=60min"
} | run_pg "cat >> '$PGDATA/postgresql.conf'"

start_pg() { run_pg "pg_ctl -D '$PGDATA' -l '$LOG' start -w" >/dev/null 2>&1; }
stop_pg()  { run_pg "pg_ctl -D '$PGDATA' stop -m fast -w" >/dev/null 2>&1; }

start_pg
run_pg "createdb -p $PORT bench"

# Eight random bigint columns: random data defeats the lightweight encodings and
# the block codec, so the on-disk streams are large and a cold scan is I/O-bound
# (the regime where prefetch/AIO can help). A linear/monotonic column would
# delta-encode to almost nothing and make the scan CPU/decode-bound instead.
echo "-- load $ROWS rows x 8 random bigint columns (columnar, compression=none)"
run_pg "$PSQL -c \"CREATE EXTENSION columnar;\""
run_pg "$PSQL -c \"SET columnar.compression='none';
  CREATE TABLE t (c0 bigint,c1 bigint,c2 bigint,c3 bigint,
                  c4 bigint,c5 bigint,c6 bigint,c7 bigint) USING columnar;\""
run_pg "$PSQL -c \"INSERT INTO t SELECT
    (random()*9e18)::bigint,(random()*9e18)::bigint,(random()*9e18)::bigint,(random()*9e18)::bigint,
    (random()*9e18)::bigint,(random()*9e18)::bigint,(random()*9e18)::bigint,(random()*9e18)::bigint
  FROM generate_series(1,$ROWS) g;\""
run_pg "$PSQL -c \"CHECKPOINT;\""

RELPATH="$(run_pg "$PSQL -tAc \"SELECT pg_relation_filepath('t');\"")"
ABSREL="$PGDATA/$RELPATH"
TABLESIZE="$(run_pg "$PSQL -tAc \"SELECT pg_size_pretty(pg_table_size('t'));\"")"
echo "   table size: $TABLESIZE   file: $RELPATH   shared_buffers: 128MB"

# Scan all eight column streams so the cold read is as large as possible.
QUERY="SELECT sum(c0)+sum(c1)+sum(c2)+sum(c3)+sum(c4)+sum(c5)+sum(c6)+sum(c7) FROM t"
cold_median() {
	local rs="$1" reps=3 t
	local times=()
	for _ in $(seq 1 $reps); do
		stop_pg; start_pg >/dev/null
		run_pg "$WORKDIR/evict '$ABSREL'" >/dev/null 2>&1
		t="$(run_pg "psql -p $PORT -d bench -qAtX -c \"SET columnar.enable_read_stream=$rs;\" -c \"\\timing on\" -c \"$QUERY\" 2>&1 | sed -n 's/^Time: \\([0-9.]*\\) ms.*/\\1/p' | tail -1")"
		times+=("${t:-0}")
	done
	printf '%s\n' "${times[@]}" | sort -n | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]}'
}

echo
echo "=== COLD-SCAN LATENCY: io_method x read_stream (median of 3, ms) ==="
printf '%-10s | %-12s | %-12s | %s\n' io_method rs_on_ms rs_off_ms rs_speedup
printf -- '-----------+--------------+--------------+-----------\n'
declare -A ON
for iom in sync worker io_uring; do
	run_pg "$PSQL -c \"ALTER SYSTEM SET io_method='$iom';\"" >/dev/null 2>&1
	stop_pg; start_pg >/dev/null 2>&1
	got="$(run_pg "$PSQL -tAc \"SHOW io_method;\"" 2>/dev/null)"
	if [ "$got" != "$iom" ]; then
		printf '%-10s | %s\n' "$iom" "unavailable (server reports io_method=$got); skipped"
		continue
	fi
	on_ms="$(cold_median on)"
	off_ms="$(cold_median off)"
	ON[$iom]="$on_ms"
	sp="$(awk -v a="$off_ms" -v b="$on_ms" 'BEGIN{ if(b>0) printf "%.2f", a/b; else print "n/a"}')"
	printf '%-10s | %-12s | %-12s | %sx\n' "$iom" "$on_ms" "$off_ms" "$sp"
done

echo
echo "=== io_method speedup vs sync (read_stream on) ==="
base="${ON[sync]:-}"
for iom in sync worker io_uring; do
	v="${ON[$iom]:-}"
	[ -z "$v" ] && continue
	sp="$(awk -v a="$base" -v b="$v" 'BEGIN{ if(a!="" && b>0) printf "%.2f", a/b; else print "n/a"}')"
	printf '%-10s  %s ms  (%sx vs sync)\n' "$iom" "$v" "$sp"
done

echo
echo "== benchmark complete =="
