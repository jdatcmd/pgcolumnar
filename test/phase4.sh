#!/usr/bin/env bash
#
# pgColumnar phase 4 test: indexes and constraints. Covers btree and hash index
# build and scan over a columnar table, index fetch by item pointer respecting
# the row mask, unique/primary-key constraint enforcement (across statements and
# within a single statement), NOT NULL and CHECK constraints, heap<->columnar
# conversion round-trips, and confirmation that ordinary index scans work (with
# the index-only-scan fallback verified when the feature is turned off; the
# feature itself is covered by index_only.sh).
#
# Builds and installs the extension, spins up a throwaway PostgreSQL cluster as
# the postgres OS user, and exercises the phase 4 feature set. Written fresh for
# pgColumnar; it does not reuse any upstream test file or expected-output file.
#
# Usage:
#   test/phase4.sh [PG_CONFIG]
#
# PG_CONFIG defaults to /usr/local/pg17/bin/pg_config. Run as a user that may
# "runuser -u postgres" (e.g. root) when the current user is not postgres.

set -euo pipefail

PG_CONFIG="${1:-/usr/local/pg17/bin/pg_config}"
BINDIR="$("$PG_CONFIG" --bindir)"
PORT="${PGC_PORT:-54324}"
SRCDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

WORKDIR="$(mktemp -d /tmp/pgcolumnar-phase4.XXXXXX)"
PGDATA="$WORKDIR/data"
LOGFILE="$WORKDIR/server.log"

echo "== pgColumnar phase 4 test =="
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
run_pg "createdb -p $PORT p4"

# -qAt: quiet, unaligned, tuples-only, so a value is exactly the query output.
PSQL="psql -p $PORT -d p4 -qAt -v ON_ERROR_STOP=1"
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

# assert a query succeeds and its plan (with seqscan disabled) contains/omits text
assert_plan() {
	# $1 name, $2 query, $3 must-contain, $4 must-NOT-contain
	local name="$1" sql="$2" want="$3" notwant="$4"
	local plan
	plan="$(run_pg "$PSQL -c \"SET enable_seqscan=off; EXPLAIN (COSTS OFF) $sql\"")"
	if echo "$plan" | grep -q "$want" && ! echo "$plan" | grep -q "$notwant"; then
		echo "PASS  $name: $(echo "$plan" | grep -E 'Scan' | head -1 | sed 's/^ *//')"
	else
		echo "FAIL  $name: plan was:"
		echo "$plan" | sed 's/^/        /'
		fail=1
	fi
}

# assert a statement is rejected (returns an error)
expect_fail() {
	local name="$1" sql="$2"
	if run_pg "$PSQL -c \"$sql\"" >/dev/null 2>&1; then
		echo "FAIL  $name: expected error, got success"
		fail=1
	else
		echo "PASS  $name: rejected"
	fi
}

q "CREATE EXTENSION pgcolumnar;" >/dev/null

# ---------------------------------------------------------------------------
# Btree index: build over a columnar table, then point and range index scans.
# ---------------------------------------------------------------------------
echo "-- btree index build and scan"
q "CREATE TABLE bt (a int, b text) USING pgcolumnar;" >/dev/null
q "INSERT INTO bt SELECT g, 'r'||g FROM generate_series(1,20000) g;" >/dev/null
q "CREATE INDEX bt_a_idx ON bt (a);" >/dev/null
assert_plan "btree plan is index scan" "SELECT b FROM bt WHERE a = 12345;" \
	"Index Scan" "Index Only Scan"
check "btree point"       "$(q 'SET enable_seqscan=off; SELECT b FROM bt WHERE a = 12345;')" "r12345"
check "btree range count"  "$(q 'SET enable_seqscan=off; SELECT count(*) FROM bt WHERE a BETWEEN 100 AND 199;')" "100"
check "btree range first"  "$(q 'SET enable_seqscan=off; SELECT b FROM bt WHERE a BETWEEN 100 AND 199 ORDER BY a LIMIT 1;')" "r100"
check "btree miss"         "$(q 'SET enable_seqscan=off; SELECT count(*) FROM bt WHERE a = 999999;')" "0"

# ---------------------------------------------------------------------------
# Hash index: equality lookups.
# ---------------------------------------------------------------------------
echo "-- hash index build and equality lookup"
q "CREATE TABLE ht (a int, b text) USING pgcolumnar;" >/dev/null
q "INSERT INTO ht SELECT g, 'h'||g FROM generate_series(1,20000) g;" >/dev/null
q "CREATE INDEX ht_a_hash ON ht USING hash (a);" >/dev/null
assert_plan "hash plan is index scan" "SELECT b FROM ht WHERE a = 5000;" \
	"Index Scan" "Index Only Scan"
check "hash point"    "$(q 'SET enable_seqscan=off; SELECT b FROM ht WHERE a = 5000;')" "h5000"
check "hash point2"   "$(q 'SET enable_seqscan=off; SELECT b FROM ht WHERE a = 17777;')" "h17777"
check "hash miss"     "$(q 'SET enable_seqscan=off; SELECT count(*) FROM ht WHERE a = 999999;')" "0"

# ---------------------------------------------------------------------------
# Unique / primary key: reject duplicates across statements and within one.
# ---------------------------------------------------------------------------
echo "-- unique / primary key enforcement"
q "CREATE TABLE uq (id int PRIMARY KEY, v text) USING pgcolumnar;" >/dev/null
q "INSERT INTO uq VALUES (1,'a'),(2,'b'),(3,'c');" >/dev/null
check "uq initial"          "$(q 'SELECT count(*) FROM uq;')" "3"
expect_fail "uq cross-stmt dup"   "INSERT INTO uq VALUES (2, 'dup');"
expect_fail "uq within-stmt dup"  "INSERT INTO uq VALUES (10,'x'),(10,'y');"
check "uq unchanged"        "$(q 'SELECT count(*) FROM uq;')" "3"
# a non-conflicting insert still works, and is found by the index afterwards
q "INSERT INTO uq VALUES (4,'d');" >/dev/null
check "uq new row"          "$(q 'SET enable_seqscan=off; SELECT v FROM uq WHERE id = 4;')" "d"
check "uq final count"      "$(q 'SELECT count(*) FROM uq;')" "4"
# a standalone UNIQUE index (not a PK) behaves the same
q "CREATE TABLE uq2 (id int, v text) USING pgcolumnar;" >/dev/null
q "CREATE UNIQUE INDEX uq2_id ON uq2 (id);" >/dev/null
q "INSERT INTO uq2 SELECT g, 'u'||g FROM generate_series(1,5000) g;" >/dev/null
expect_fail "uq2 dup"       "INSERT INTO uq2 VALUES (2500, 'dup');"
check "uq2 count"           "$(q 'SELECT count(*) FROM uq2;')" "5000"

# ---------------------------------------------------------------------------
# NOT NULL and CHECK constraints (driven by the executor through the AM).
# ---------------------------------------------------------------------------
echo "-- not null and check constraints"
q "CREATE TABLE ck (a int NOT NULL, b int CHECK (b > 0)) USING pgcolumnar;" >/dev/null
q "INSERT INTO ck VALUES (1, 10), (2, 20);" >/dev/null
expect_fail "not null reject"  "INSERT INTO ck VALUES (NULL, 5);"
expect_fail "check reject"     "INSERT INTO ck VALUES (3, -1);"
check "ck unchanged"        "$(q 'SELECT count(*) FROM ck;')" "2"

# ---------------------------------------------------------------------------
# Index scan does not return rows marked deleted in the row mask.
# ---------------------------------------------------------------------------
echo "-- index scan skips row-mask-deleted rows"
q "CREATE TABLE di (a int, b text) USING pgcolumnar;" >/dev/null
q "INSERT INTO di SELECT g, 'd'||g FROM generate_series(1,20000) g;" >/dev/null
q "CREATE INDEX di_a_idx ON di (a);" >/dev/null
q "DELETE FROM di WHERE a BETWEEN 5000 AND 5100;" >/dev/null
check "deleted point"      "$(q 'SET enable_seqscan=off; SELECT count(*) FROM di WHERE a = 5050;')" "0"
check "deleted range"      "$(q 'SET enable_seqscan=off; SELECT count(*) FROM di WHERE a BETWEEN 5000 AND 5100;')" "0"
check "survivors around"   "$(q 'SET enable_seqscan=off; SELECT count(*) FROM di WHERE a BETWEEN 4900 AND 5200;')" "200"
check "neighbor present"   "$(q 'SET enable_seqscan=off; SELECT b FROM di WHERE a = 4999;')" "d4999"
# update then index-fetch the new value; the old key must be gone via the index
q "UPDATE di SET a = a + 1000000 WHERE a = 8000;" >/dev/null
check "updated old gone"   "$(q 'SET enable_seqscan=off; SELECT count(*) FROM di WHERE a = 8000;')" "0"
check "updated new found"  "$(q 'SET enable_seqscan=off; SELECT b FROM di WHERE a = 1008000;')" "d8000"

# ---------------------------------------------------------------------------
# Heap <-> columnar conversion round-trips row counts and values.
# ---------------------------------------------------------------------------
echo "-- heap to columnar and back"
q "CREATE TABLE conv (a int, b text);" >/dev/null
q "INSERT INTO conv SELECT g, 'v'||g FROM generate_series(1,10000) g;" >/dev/null
check "conv starts heap"   "$(q "SELECT amname FROM pg_am m JOIN pg_class c ON c.relam=m.oid WHERE c.relname='conv';")" "heap"
q "SELECT pgcolumnar.alter_table_set_access_method('conv', 'pgcolumnar');" >/dev/null
check "conv now columnar"  "$(q "SELECT amname FROM pg_am m JOIN pg_class c ON c.relam=m.oid WHERE c.relname='conv';")" "pgcolumnar"
check "conv col count"     "$(q 'SELECT count(*) FROM conv;')" "10000"
check "conv col value"     "$(q 'SELECT b FROM conv WHERE a = 7777;')" "v7777"
check "conv col sum"       "$(q 'SELECT sum(a) FROM conv;')" "50005000"
q "SELECT pgcolumnar.alter_table_set_access_method('conv','heap');" >/dev/null
check "conv back heap"     "$(q "SELECT amname FROM pg_am m JOIN pg_class c ON c.relam=m.oid WHERE c.relname='conv';")" "heap"
check "conv heap count"    "$(q 'SELECT count(*) FROM conv;')" "10000"
check "conv heap value"    "$(q 'SELECT b FROM conv WHERE a = 7777;')" "v7777"
check "conv heap sum"      "$(q 'SELECT sum(a) FROM conv;')" "50005000"

# ---------------------------------------------------------------------------
# Index-only scans are a real feature now (gap 28, pgcolumnar.enable_index_only_scan,
# on by default) and are exercised comprehensively in index_only.sh. Here we only
# confirm the fallback path: with the GUC off the planner builds a plain Index
# Scan for a covering query, and that scan returns the correct value.
# ---------------------------------------------------------------------------
echo "-- index-only scan can be turned off (plain index scan fallback)"
q "CREATE TABLE ios (a int, b int) USING pgcolumnar;" >/dev/null
q "INSERT INTO ios SELECT g, g*2 FROM generate_series(1,20000) g;" >/dev/null
q "CREATE INDEX ios_a_idx ON ios (a);" >/dev/null
# With index-only scans disabled, a covering query falls back to an Index Scan.
iosoff_plan="$(run_pg "$PSQL -c \"SET pgcolumnar.enable_index_only_scan=off; SET enable_seqscan=off; EXPLAIN (COSTS OFF) SELECT a FROM ios WHERE a = 100;\"")"
if echo "$iosoff_plan" | grep -q "Index Scan" && ! echo "$iosoff_plan" | grep -q "Index Only Scan"; then
	echo "PASS  IOS off: plain index scan"
else
	echo "FAIL  IOS off: plain index scan: plan was:"; echo "$iosoff_plan" | sed 's/^/        /'; fail=1
fi
check "covering value"    "$(q 'SET enable_seqscan=off; SELECT a FROM ios WHERE a = 100;')" "100"
# a full-table scan is still available when index/bitmap scans are disabled.
# As of phase 5 the columnar full scan is the custom scan (ColumnarScan); with
# the custom scan disabled it is a plain sequential scan. Either is acceptable.
assert_plan_seq() {
	local plan
	plan="$(run_pg "$PSQL -c \"SET enable_indexscan=off; SET enable_bitmapscan=off; EXPLAIN (COSTS OFF) SELECT * FROM ios WHERE a = 100;\"")"
	if echo "$plan" | grep -qE "Seq Scan|Custom Scan \(ColumnarScan\)"; then
		echo "PASS  full-table scan available"
	else
		echo "FAIL  full-table scan available: $plan"; fail=1
	fi
}
assert_plan_seq

echo
if [ "$fail" = "0" ]; then
	echo "PHASE 4 TEST PASSED"
else
	echo "PHASE 4 TEST FAILED"
	echo "---- server log tail ----"
	run_pg "tail -60 '$LOGFILE'" || true
fi
exit $fail
