#!/usr/bin/env bash
#
# pgColumnar phase 1 smoke test.
#
# Builds and installs the extension, spins up a throwaway PostgreSQL cluster
# as the postgres OS user, exercises create/insert/scan/drop on a columnar
# table, and checks the results. Written fresh for pgColumnar; it does not
# reuse any upstream test file or expected-output file.
#
# Usage:
#   test/smoke.sh [PG_CONFIG]
#
# PG_CONFIG defaults to /usr/local/pg17/bin/pg_config. Run as a user that may
# "runuser -u postgres" (e.g. root) when the current user is not postgres.

set -euo pipefail

PG_CONFIG="${1:-/usr/local/pg17/bin/pg_config}"
BINDIR="$("$PG_CONFIG" --bindir)"
PORT="${PGC_PORT:-54321}"
SRCDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# A scratch area the postgres user can read and write.
WORKDIR="$(mktemp -d /tmp/pgcolumnar-smoke.XXXXXX)"
PGDATA="$WORKDIR/data"
LOGFILE="$WORKDIR/server.log"

echo "== pgColumnar smoke test =="
echo "PG_CONFIG=$PG_CONFIG"
echo "workdir=$WORKDIR"

# ---- build and install -----------------------------------------------------
echo "-- building"
make -C "$SRCDIR" PG_CONFIG="$PG_CONFIG" >/dev/null
echo "-- installing"
make -C "$SRCDIR" install PG_CONFIG="$PG_CONFIG" >/dev/null

# ---- decide how to run cluster commands ------------------------------------
# pg_regress and initdb cannot run as root; use the postgres user if we are
# root, otherwise run directly.
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
echo "-- initdb"
run_pg "initdb -D '$PGDATA' -A trust" >/dev/null 2>&1
run_pg "echo \"port=$PORT\" >> '$PGDATA/postgresql.conf'"
# Preload the library so the drop-time metadata cleanup hook is installed in
# every backend (the canonical deployment for a table-AM extension).
run_pg "echo \"shared_preload_libraries='pgcolumnar'\" >> '$PGDATA/postgresql.conf'"
# This suite exercises the 2.2-line mechanics (chunk / stripe catalogs); pin
# the instance default to 2.2 so the D6f native default does not change what it
# writes. The 2.2 line is retired in the later Phase H.
run_pg "echo \"pgcolumnar.default_format_version=0\" >> '$PGDATA/postgresql.conf'"
echo "-- start"
run_pg "pg_ctl -D '$PGDATA' -l '$LOGFILE' start -w" >/dev/null
run_pg "createdb -p $PORT smoke"

PSQL="psql -p $PORT -d smoke -At -v ON_ERROR_STOP=1"

# ---- exercise the access method --------------------------------------------
echo "-- running smoke SQL"
run_pg "$PSQL -c \"CREATE EXTENSION pgcolumnar;\"" >/dev/null
run_pg "$PSQL -c \"CREATE TABLE t (a int, b text) USING pgcolumnar;\"" >/dev/null
run_pg "$PSQL -c \"INSERT INTO t SELECT g, g::text FROM generate_series(1, 100000) g;\"" >/dev/null

TOTAL="$(run_pg "$PSQL -c \"SELECT count(*) FROM t;\"")"
FILTERED="$(run_pg "$PSQL -c \"SELECT count(*) FROM t WHERE a < 50;\"")"
FIRST3="$(run_pg "$PSQL -c \"SELECT a || '|' || b FROM t ORDER BY a LIMIT 3;\"" | tr '\n' ',')"
NULLCOUNT="$(run_pg "$PSQL -c \"SELECT count(*) FROM t WHERE b IS NULL;\"")"

# a table with a null value round-trips
run_pg "$PSQL -c \"CREATE TABLE n (a int, b text) USING pgcolumnar;\"" >/dev/null
run_pg "$PSQL -c \"INSERT INTO n VALUES (1,'x'),(2,NULL),(3,'z');\"" >/dev/null
NROWS="$(run_pg "$PSQL -c \"SELECT count(*) FROM n;\"")"
NNULL="$(run_pg "$PSQL -c \"SELECT count(*) FROM n WHERE b IS NULL;\"")"
NVAL="$(run_pg "$PSQL -c \"SELECT b FROM n WHERE a = 3;\"")"

run_pg "$PSQL -c \"DROP TABLE t;\"" >/dev/null
run_pg "$PSQL -c \"DROP TABLE n;\"" >/dev/null

# metadata rows are cleaned up on drop
ORPHANS="$(run_pg "$PSQL -c \"SELECT count(*) FROM pgcolumnar.stripe;\"")"

# ---- check -----------------------------------------------------------------
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

check "count(*)"            "$TOTAL"    "100000"
check "count where a<50"   "$FILTERED" "49"
check "order by a limit 3" "$FIRST3"   "1|1,2|2,3|3,"
check "no nulls in t.b"    "$NULLCOUNT" "0"
check "null table rows"    "$NROWS"    "3"
check "null table nulls"   "$NNULL"    "1"
check "null table value"   "$NVAL"     "z"
check "orphan stripes"     "$ORPHANS"  "0"

# Phase D1: the native format catalog tables exist (empty until the native
# writer, Phase D2). Confirms CREATE EXTENSION created the additive catalog.
NATIVE_TABLES="$(run_pg "$PSQL -c \"SELECT count(*) FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname='pgcolumnar' AND c.relkind='r' AND c.relname IN ('storage','row_group','column_chunk','zone_map');\"")"
check "native catalog tables" "$NATIVE_TABLES" "4"

echo
if [ "$fail" = "0" ]; then
	echo "SMOKE TEST PASSED"
else
	echo "SMOKE TEST FAILED"
	echo "---- server log tail ----"
	run_pg "tail -30 '$LOGFILE'" || true
fi
exit $fail
