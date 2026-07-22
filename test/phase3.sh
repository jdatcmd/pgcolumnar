#!/usr/bin/env bash
#
# pgColumnar phase 3 test: update and delete via the row mask, snapshot
# visibility and same-transaction read-your-writes, transaction and savepoint
# rollback, and temporary columnar tables. Also re-runs the phase 1 and phase 2
# regression checks.
#
# Builds and installs the extension, spins up a throwaway PostgreSQL cluster as
# the postgres OS user, and exercises the phase 3 feature set. Written fresh for
# pgColumnar; it does not reuse any upstream test file or expected-output file.
#
# Usage:
#   test/phase3.sh [PG_CONFIG]
#
# PG_CONFIG defaults to /usr/local/pg17/bin/pg_config. Run as a user that may
# "runuser -u postgres" (e.g. root) when the current user is not postgres.

set -euo pipefail

PG_CONFIG="${1:-/usr/local/pg17/bin/pg_config}"
BINDIR="$("$PG_CONFIG" --bindir)"
PORT="${PGC_PORT:-54323}"
SRCDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

WORKDIR="$(mktemp -d /tmp/pgcolumnar-phase3.XXXXXX)"
PGDATA="$WORKDIR/data"
LOGFILE="$WORKDIR/server.log"

echo "== pgColumnar phase 3 test =="
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
run_pg "createdb -p $PORT p3"

# -qAt: quiet (no command tags), unaligned, tuples-only, so a value is exactly
# the query output. The SQL is fed on psql's stdin rather than through -c: a
# multi-statement -c string prints only the last command's result before
# PostgreSQL 14, whereas reading from stdin prints every statement's result on
# all supported majors. Statements joined with ';' still run in one session
# (and, with explicit BEGIN/COMMIT, one transaction), which is what the
# read-your-writes and savepoint checks below rely on.
PSQL="psql -p $PORT -d p3 -qAt -v ON_ERROR_STOP=1"
q() { run_pg "$PSQL" <<<"$1"; }
qq() { run_pg "$PSQL" <<<"$1" | tr '\n' ',' ; }

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
# Phase 1/2 regression: count and filter still work.
# ---------------------------------------------------------------------------
echo "-- phase 1/2 regression"
q "CREATE TABLE reg (a int, b text) USING pgcolumnar;" >/dev/null
q "INSERT INTO reg SELECT g, g::text FROM generate_series(1,50000) g;" >/dev/null
check "reg count"        "$(q 'SELECT count(*) FROM reg;')"              "50000"
check "reg filter a<100" "$(q 'SELECT count(*) FROM reg WHERE a < 100;')" "99"
q "DROP TABLE reg;" >/dev/null

# ---------------------------------------------------------------------------
# Same-transaction read-your-writes: rows inserted earlier in a transaction are
# visible to a later scan in the same transaction (the phase 1 gap, now closed).
# ---------------------------------------------------------------------------
echo "-- read-your-writes"
check "ryw count" \
	"$(q "BEGIN; CREATE TABLE ryw (a int) USING pgcolumnar; INSERT INTO ryw SELECT g FROM generate_series(1,1000) g; SELECT count(*) FROM ryw; COMMIT;")" \
	"1000"
check "ryw sum" \
	"$(q 'BEGIN; INSERT INTO ryw VALUES (100000); SELECT sum(a) FROM ryw; COMMIT;')" \
	"600500"
check "ryw persisted" "$(q 'SELECT count(*) FROM ryw;')" "1001"
q "DROP TABLE ryw;" >/dev/null

# ---------------------------------------------------------------------------
# DELETE marks rows in the row mask; scans return exactly the survivors.
# 25000 rows span three chunk groups (10000/10000/5000), so the deleted range
# below straddles a chunk-group boundary.
# ---------------------------------------------------------------------------
echo "-- delete"
q "CREATE TABLE d (id int, v text) USING pgcolumnar;" >/dev/null
q "INSERT INTO d SELECT g, 'r'||g FROM generate_series(1,25000) g;" >/dev/null
q "DELETE FROM d WHERE id BETWEEN 9995 AND 10005;" >/dev/null
check "delete count"       "$(q 'SELECT count(*) FROM d;')"                       "24989"
check "delete gone"        "$(q 'SELECT count(*) FROM d WHERE id BETWEEN 9995 AND 10005;')" "0"
check "delete neighbors"   "$(qq 'SELECT id FROM d WHERE id IN (9994,10006) ORDER BY id;')" "9994,10006,"
check "delete boundaries"  "$(q 'SELECT min(id)||chr(47)||max(id) FROM d;')"      "1/25000"
# a second, whole-chunk-group delete
q "DELETE FROM d WHERE id > 20000;" >/dev/null
check "delete tail count"  "$(q 'SELECT count(*) FROM d;')"                       "19989"
check "delete tail max"    "$(q 'SELECT max(id) FROM d;')"                        "20000"
# row_mask catalog reflects the deletions
check "row_mask rows>0"    "$(q 'SELECT (count(*) > 0)::int FROM pgcolumnar.row_mask;')" "1"
check "row_mask deleted"   "$(q 'SELECT sum(deleted_rows) FROM pgcolumnar.row_mask;')"   "5011"
q "DROP TABLE d;" >/dev/null

# ---------------------------------------------------------------------------
# UPDATE is delete-plus-insert: the new value shows, the old row disappears,
# and the row count is unchanged. Updating the key column works too.
# ---------------------------------------------------------------------------
echo "-- update"
q "CREATE TABLE u (id int, v text) USING pgcolumnar;" >/dev/null
q "INSERT INTO u SELECT g, 'r'||g FROM generate_series(1,5000) g;" >/dev/null
q "UPDATE u SET v='changed' WHERE id = 2500;" >/dev/null
check "update value"     "$(q 'SELECT v FROM u WHERE id = 2500;')"   "changed"
check "update one row"   "$(q "SELECT count(*) FROM u WHERE v = 'changed';")" "1"
check "update count"     "$(q 'SELECT count(*) FROM u;')"            "5000"
q "UPDATE u SET id = id + 100000 WHERE id <= 10;" >/dev/null
check "update key gone"  "$(q 'SELECT count(*) FROM u WHERE id <= 10;')"      "0"
check "update key new"   "$(q 'SELECT count(*) FROM u WHERE id > 100000;')"   "10"
check "update key total" "$(q 'SELECT count(*) FROM u;')"            "5000"
# update every row (bulk), verify no old survivors and the suffix is present
q "UPDATE u SET v = v || 'ZZ';" >/dev/null
check "bulk update rows"  "$(q 'SELECT count(*) FROM u;')"                  "5000"
check "bulk update sfx"   "$(q "SELECT count(*) FROM u WHERE right(v,2) = 'ZZ';")" "5000"
q "DROP TABLE u;" >/dev/null

# ---------------------------------------------------------------------------
# ROLLBACK of an inserting and deleting transaction leaves the table unchanged.
# ---------------------------------------------------------------------------
echo "-- rollback"
q "CREATE TABLE rb (a int) USING pgcolumnar;" >/dev/null
q "INSERT INTO rb SELECT g FROM generate_series(1,100) g;" >/dev/null
check "rb before" "$(q 'SELECT count(*) FROM rb;')" "100"
q "BEGIN; INSERT INTO rb SELECT g FROM generate_series(101,200) g; DELETE FROM rb WHERE a <= 50; ROLLBACK;" >/dev/null
check "rb after count" "$(q 'SELECT count(*) FROM rb;')"                "100"
check "rb after range" "$(q 'SELECT min(a)||chr(47)||max(a) FROM rb;')" "1/100"
q "DROP TABLE rb;" >/dev/null

# ---------------------------------------------------------------------------
# SAVEPOINT then ROLLBACK TO discards only the subtransaction's changes.
# Rows written before the savepoint (and deletes made before it) survive; work
# done after the savepoint is undone.
# ---------------------------------------------------------------------------
echo "-- savepoint"
q "CREATE TABLE sp (a int) USING pgcolumnar;" >/dev/null
q "INSERT INTO sp SELECT g FROM generate_series(1,10) g;" >/dev/null
SPRES="$(q "BEGIN;
	INSERT INTO sp VALUES (11);
	DELETE FROM sp WHERE a = 1;
	SAVEPOINT s1;
	INSERT INTO sp VALUES (12);
	DELETE FROM sp WHERE a = 2;
	ROLLBACK TO s1;
	SELECT string_agg(a::text, ',' ORDER BY a) FROM sp;
	COMMIT;")"
# before savepoint: added 11, removed 1 -> {2..11}. after-savepoint work undone.
check "savepoint in-txn"  "$SPRES" "2,3,4,5,6,7,8,9,10,11"
check "savepoint final"   "$(qq 'SELECT a FROM sp ORDER BY a;')" "2,3,4,5,6,7,8,9,10,11,"
q "DROP TABLE sp;" >/dev/null

# ---------------------------------------------------------------------------
# Temporary columnar table: create, insert, scan, delete, update all work.
# ---------------------------------------------------------------------------
echo "-- temporary table"
TEMPRES="$(q "CREATE TEMP TABLE t_tmp (a int, b text) USING pgcolumnar;
	INSERT INTO t_tmp SELECT g, g::text FROM generate_series(1,2000) g;
	DELETE FROM t_tmp WHERE a % 5 = 0;
	UPDATE t_tmp SET b = 'u' WHERE a = 3;
	SELECT count(*) FROM t_tmp;")"
check "temp survivors" "$TEMPRES" "1600"
check "temp deleted"   "$(q 'CREATE TEMP TABLE t_tmp2 (a int) USING pgcolumnar; INSERT INTO t_tmp2 SELECT g FROM generate_series(1,2000) g; DELETE FROM t_tmp2 WHERE a % 5 = 0; SELECT count(*) FROM t_tmp2 WHERE a % 5 = 0;')" "0"

# ---------------------------------------------------------------------------
# Deletes survive across transactions, and drop cleans up row_mask rows.
# ---------------------------------------------------------------------------
echo "-- durability and cleanup"
q "CREATE TABLE dur (a int) USING pgcolumnar;" >/dev/null
q "INSERT INTO dur SELECT g FROM generate_series(1,1000) g;" >/dev/null
q "DELETE FROM dur WHERE a % 2 = 0;" >/dev/null
check "durable survivors" "$(q 'SELECT count(*) FROM dur;')"                 "500"
check "durable odd only"  "$(q 'SELECT count(*) FROM dur WHERE a % 2 = 0;')" "0"
q "DROP TABLE dur;" >/dev/null
check "row_mask cleaned"  "$(q 'SELECT count(*) FROM pgcolumnar.row_mask;')" "0"
check "row group cleaned" "$(q 'SELECT count(*) FROM pgcolumnar.row_group;')" "0"

echo
if [ "$fail" = "0" ]; then
	echo "PHASE 3 TEST PASSED"
else
	echo "PHASE 3 TEST FAILED"
	echo "---- server log tail ----"
	run_pg "tail -40 '$LOGFILE'" || true
fi
exit $fail
