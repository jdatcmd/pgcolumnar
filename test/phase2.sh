#!/usr/bin/env bash
#
# pgColumnar phase 2 test: compression, projection, min/max skip lists, and
# chunk-group filtering, plus the phase 1 regression checks.
#
# Builds and installs the extension, spins up a throwaway PostgreSQL cluster as
# the postgres OS user, and exercises the phase 2 feature set. Written fresh for
# pgColumnar; it does not reuse any upstream test file or expected-output file.
#
# Usage:
#   test/phase2.sh [PG_CONFIG]
#
# PG_CONFIG defaults to /usr/local/pg17/bin/pg_config. Run as a user that may
# "runuser -u postgres" (e.g. root) when the current user is not postgres.

set -euo pipefail

PG_CONFIG="${1:-/usr/local/pg17/bin/pg_config}"
BINDIR="$("$PG_CONFIG" --bindir)"
PORT="${PGC_PORT:-54322}"
SRCDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

WORKDIR="$(mktemp -d /tmp/pgcolumnar-phase2.XXXXXX)"
PGDATA="$WORKDIR/data"
LOGFILE="$WORKDIR/server.log"

echo "== pgColumnar phase 2 test =="
echo "PG_CONFIG=$PG_CONFIG"
echo "workdir=$WORKDIR"

echo "-- building"
make -C "$SRCDIR" PG_CONFIG="$PG_CONFIG" >/dev/null
echo "-- installing"
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

echo "-- initdb"
run_pg "initdb -D '$PGDATA' -A trust" >/dev/null 2>&1
run_pg "echo \"port=$PORT\" >> '$PGDATA/postgresql.conf'"
run_pg "echo \"shared_preload_libraries='pgcolumnar'\" >> '$PGDATA/postgresql.conf'"
echo "-- start"
run_pg "pg_ctl -D '$PGDATA' -l '$LOGFILE' start -w" >/dev/null
run_pg "createdb -p $PORT p2"

PSQL="psql -p $PORT -d p2 -At -v ON_ERROR_STOP=1"
q() { run_pg "$PSQL -c \"$1\""; }
qq() { run_pg "$PSQL -c \"$1\"" | tr '\n' ',' ; }

fail=0
check() {
	local name="$1" got="$2" want="$3"
	if [ "$got" = "$want" ]; then
		echo "PASS  $name: $got"
	else
		echo "FAIL  $name: got [$got] want [$want]"
		fail=1
	fi
}

q "CREATE EXTENSION pgcolumnar;" >/dev/null

# ---------------------------------------------------------------------------
# Phase 1 regression: count, filter, order-limit, nulls, orphan cleanup.
# ---------------------------------------------------------------------------
echo "-- phase 1 regression"
q "CREATE TABLE t (a int, b text) USING pgcolumnar;" >/dev/null
q "INSERT INTO t SELECT g, g::text FROM generate_series(1,100000) g;" >/dev/null
check "count(*)"          "$(q 'SELECT count(*) FROM t;')"            "100000"
check "count where a<50"  "$(q 'SELECT count(*) FROM t WHERE a < 50;')" "49"
check "order by a limit3" "$(qq 'SELECT a || CHR(124) || b FROM t ORDER BY a LIMIT 3;')" "1|1,2|2,3|3,"
check "no nulls t.b"      "$(q 'SELECT count(*) FROM t WHERE b IS NULL;')" "0"
q "DROP TABLE t;" >/dev/null

# ---------------------------------------------------------------------------
# Compression: each codec round-trips, and the stored codec matches.
# Highly compressible payload so the codec is actually used (no fallback).
# The table is the only columnar table when we inspect pgcolumnar.chunk, so all
# chunk rows belong to it.
# ---------------------------------------------------------------------------
echo "-- compression round-trip per codec"
for codec in none pglz lz4 zstd; do
	case "$codec" in
		none) code=0 ;;
		pglz) code=1 ;;
		lz4)  code=2 ;;
		zstd) code=3 ;;
	esac

	q "CREATE TABLE c (id int, payload text) USING pgcolumnar;" >/dev/null
	# SET and INSERT in one session so the GUC is in force at write time.
	q "SET pgcolumnar.compression='$codec'; INSERT INTO c SELECT g, repeat('x',100) FROM generate_series(1,20000) g;" >/dev/null

	check "$codec count"       "$(q 'SELECT count(*) FROM c;')"                 "20000"
	check "$codec sum(id)"     "$(q 'SELECT sum(id) FROM c;')"                  "200010000"
	check "$codec payload ok"  "$(q "SELECT payload = repeat('x',100) FROM c WHERE id = 12345;")" "t"
	check "$codec distinct pl" "$(q 'SELECT count(DISTINCT payload) FROM c;')"  "1"
	# stored codec on the payload column (attr_num 2)
	check "$codec stored type" "$(q 'SELECT DISTINCT value_compression_type FROM pgcolumnar.chunk WHERE attr_num = 2;')" "$code"

	q "DROP TABLE c;" >/dev/null
done

# ---------------------------------------------------------------------------
# Fallback: data too small to compress is stored uncompressed (type 0) even
# when a codec is requested (spec 5).
# ---------------------------------------------------------------------------
echo "-- compression fallback"
q "CREATE TABLE f (id int) USING pgcolumnar;" >/dev/null
q "SET pgcolumnar.compression='zstd'; INSERT INTO f VALUES (42);" >/dev/null
check "fallback value"  "$(q 'SELECT id FROM f;')" "42"
check "fallback type=0" "$(q 'SELECT value_compression_type FROM pgcolumnar.chunk WHERE attr_num = 1;')" "0"
q "DROP TABLE f;" >/dev/null

# ---------------------------------------------------------------------------
# Nulls round-trip with compression on (exists stream is never compressed).
# ---------------------------------------------------------------------------
echo "-- nulls with compression"
q "CREATE TABLE n (a int, b text) USING pgcolumnar;" >/dev/null
q "SET pgcolumnar.compression='lz4'; INSERT INTO n SELECT g, CASE WHEN g%3=0 THEN NULL ELSE repeat('y',50) END FROM generate_series(1,15000) g;" >/dev/null
check "null rows"   "$(q 'SELECT count(*) FROM n;')"                 "15000"
check "null count"  "$(q 'SELECT count(*) FROM n WHERE b IS NULL;')" "5000"
check "null value"  "$(q "SELECT b IS NULL FROM n WHERE a = 9;")"    "t"
check "nonnull val" "$(q "SELECT b = repeat('y',50) FROM n WHERE a = 8;")" "t"
q "DROP TABLE n;" >/dev/null

# ---------------------------------------------------------------------------
# Projection: selecting individual columns returns correct values.
# ---------------------------------------------------------------------------
echo "-- projection"
q "CREATE TABLE p (a int, b text, c int) USING pgcolumnar;" >/dev/null
q "SET pgcolumnar.compression='zstd'; INSERT INTO p SELECT g, 'row'||g, g*2 FROM generate_series(1,30000) g;" >/dev/null
check "project a"    "$(q 'SELECT a FROM p WHERE a = 100;')"     "100"
check "project b"    "$(q "SELECT b FROM p WHERE a = 100;")"     "row100"
check "project c"    "$(q 'SELECT c FROM p WHERE a = 100;')"     "200"
check "project sum"  "$(q 'SELECT sum(c) FROM p;')"              "900030000"
check "project b,c"  "$(qq 'SELECT b || CHR(124) || c FROM p ORDER BY a LIMIT 2;')" "row1|2,row2|4,"
q "DROP TABLE p;" >/dev/null

# ---------------------------------------------------------------------------
# Min/max skip list: stored per chunk group for orderable columns. Sorted
# int data over 3 chunk groups (10000 each) so ranges are predictable. The
# stored bytea encodes int4 in native little-endian order.
# ---------------------------------------------------------------------------
echo "-- min/max skip list"
q "CREATE TABLE s (id int) USING pgcolumnar;" >/dev/null
q "SET pgcolumnar.compression='zstd'; INSERT INTO s SELECT g FROM generate_series(1,25000) g;" >/dev/null
check "chunk groups"  "$(q 'SELECT count(*) FROM pgcolumnar.chunk WHERE attr_num = 1;')" "3"
check "minmax present" "$(q 'SELECT count(*) FROM pgcolumnar.chunk WHERE attr_num = 1 AND minimum_value IS NOT NULL AND maximum_value IS NOT NULL;')" "3"
DEC="((get_byte(minimum_value,3)::bigint<<24)|(get_byte(minimum_value,2)<<16)|(get_byte(minimum_value,1)<<8)|get_byte(minimum_value,0))"
DECX="((get_byte(maximum_value,3)::bigint<<24)|(get_byte(maximum_value,2)<<16)|(get_byte(maximum_value,1)<<8)|get_byte(maximum_value,0))"
check "cg0 min=1"      "$(q "SELECT $DEC FROM pgcolumnar.chunk WHERE attr_num=1 AND chunk_group_num=0;")" "1"
check "cg0 max=10000"  "$(q "SELECT $DECX FROM pgcolumnar.chunk WHERE attr_num=1 AND chunk_group_num=0;")" "10000"
check "cg2 max=25000"  "$(q "SELECT $DECX FROM pgcolumnar.chunk WHERE attr_num=1 AND chunk_group_num=2;")" "25000"

# ---------------------------------------------------------------------------
# Chunk-group filtering correctness: filters over the sorted data return
# exactly the right rows (the executor also re-checks, so skipping never
# changes the result set).
# ---------------------------------------------------------------------------
echo "-- filter correctness"
check "between count"  "$(q 'SELECT count(*) FROM s WHERE id BETWEEN 15000 AND 15010;')" "11"
check "range min/max"  "$(qq 'SELECT min(id) || CHR(124) || max(id) FROM s WHERE id > 24990;')" "24991|25000,"
check "point lookup"   "$(q 'SELECT count(*) FROM s WHERE id = 20005;')" "1"
check "empty range"    "$(q 'SELECT count(*) FROM s WHERE id > 30000;')" "0"
q "DROP TABLE s;" >/dev/null

# ---------------------------------------------------------------------------
# Orphan cleanup after all drops.
# ---------------------------------------------------------------------------
check "orphan stripes" "$(q 'SELECT count(*) FROM pgcolumnar.stripe;')" "0"
check "orphan chunks"  "$(q 'SELECT count(*) FROM pgcolumnar.chunk;')"  "0"

echo
if [ "$fail" = "0" ]; then
	echo "PHASE 2 TEST PASSED"
else
	echo "PHASE 2 TEST FAILED"
	echo "---- server log tail ----"
	run_pg "tail -40 '$LOGFILE'" || true
fi
exit $fail
