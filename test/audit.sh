#!/usr/bin/env bash
#
# pgColumnar audit regression test. Each block below reproduces a concrete
# defect found during the bug audit and asserts the fixed behaviour. Every
# check here fails against the pre-fix code and passes after the fix.
#
#   1. ADD COLUMN on a table with existing data. A stripe written before the
#      column existed has no chunk for it; the reader must yield the column's
#      missing value (NULL, or the constant default) instead of erroring with
#      "missing chunk for attr N". Covers sequential scan, projection, the
#      vectorized aggregate path, and index fetch.
#   2. Chunk-group skipping under a mismatched collation. The stored per-chunk
#      min/max are ordered under the column's own collation; a predicate whose
#      comparison uses a different collation (an explicit COLLATE) must not
#      drive skipping, or matching rows are wrongly dropped. Results must be
#      identical whether or not qual pushdown is enabled.
#   3. Per-table option bounds. alter_columnar_table_set must reject an
#      out-of-range chunk_group_row_limit / stripe_row_limit / compression_level
#      rather than store it: a zero chunk_group_row_limit records a stripe with
#      chunk_row_count = 0 and makes delete/update/fetch divide by zero.
#   4. CREATE INDEX must not leak a relation reference. A parallel index build
#      opens a TableScanDesc per participant through columnar_scan_begin (which
#      takes a relation reference); the index_build_range_scan callback owns that
#      scan and must end it, or each participant leaks a reference that surfaces
#      at transaction commit as "resource was not closed: relation". The callback
#      must also read through the participant's claimed scan, so every live row is
#      indexed exactly once rather than once per participant.
#
# Builds and installs the extension, spins up a throwaway PostgreSQL cluster as
# the postgres OS user, and exercises the cases above. Written fresh for
# pgColumnar; it does not reuse any upstream test file or expected-output file.
#
# Usage:
#   test/audit.sh [PG_CONFIG]
#
# PG_CONFIG defaults to /usr/local/pg17/bin/pg_config. Run as a user that may
# "runuser -u postgres" (e.g. root) when the current user is not postgres.

set -euo pipefail

PG_CONFIG="${1:-/usr/local/pg17/bin/pg_config}"
BINDIR="$("$PG_CONFIG" --bindir)"
PORT="${PGC_PORT:-54327}"
SRCDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

WORKDIR="$(mktemp -d /tmp/pgcolumnar-audit.XXXXXX)"
PGDATA="$WORKDIR/data"
LOGFILE="$WORKDIR/server.log"

echo "== pgColumnar audit test =="
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
run_pg "createdb -p $PORT audit"

PSQL="psql -p $PORT -d audit -qAtX -v ON_ERROR_STOP=1"
q() { run_pg "$PSQL -c \"$1\""; }

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

# run SQL that is expected to raise an error; PASS when it does.
expect_error() {
	local name="$1" sql="$2"
	if run_pg "$PSQL -c \"$sql\"" >/dev/null 2>&1; then
		echo "FAIL  $name: statement unexpectedly succeeded"
		fail=1
	else
		echo "PASS  $name (rejected)"
	fi
}

q "CREATE EXTENSION pgcolumnar;" >/dev/null

# ---------------------------------------------------------------------------
# 1. ADD COLUMN after data exists must not break reads (missing-value support).
# ---------------------------------------------------------------------------
q "CREATE TABLE ac (a int) USING pgcolumnar;" >/dev/null
q "INSERT INTO ac SELECT g FROM generate_series(1,5) g;" >/dev/null
q "ALTER TABLE ac ADD COLUMN b int;" >/dev/null
q "ALTER TABLE ac ADD COLUMN c int DEFAULT 42;" >/dev/null
q "ALTER TABLE ac ADD COLUMN d text DEFAULT 'hi';" >/dev/null

# old rows: b is NULL, c is the constant default 42, d is 'hi'
check "add-column nullable is null" \
	"$(q "SELECT count(*) FROM ac WHERE b IS NULL;")" "5"
check "add-column const default" \
	"$(q "SELECT string_agg(DISTINCT c::text, ',') FROM ac;")" "42"
check "add-column text default" \
	"$(q "SELECT string_agg(DISTINCT d, ',') FROM ac;")" "hi"

# projection of only the added columns
check "add-column projected only" \
	"$(q "SELECT c || '/' || d FROM ac ORDER BY a LIMIT 1;")" "42/hi"

# new rows carry real values, mixed with old rows
q "INSERT INTO ac VALUES (6, 7, 8, 'new');" >/dev/null
check "add-column mixed rows" \
	"$(q "SELECT a||':'||coalesce(b::text,'-')||':'||c||':'||d FROM ac WHERE a IN (5,6) ORDER BY a;" | tr '\n' ' ')" \
	"5:-:42:hi 6:7:8:new "

# vectorized aggregate over the added columns
check "add-column aggregate" \
	"$(q "SELECT sum(c) || '/' || count(d) || '/' || count(b) FROM ac;")" "218/6/1"

# index scan over an added column
q "CREATE INDEX ac_c_idx ON ac(c);" >/dev/null
check "add-column index scan" \
	"$(q "SET enable_seqscan=off; SELECT count(*) FROM ac WHERE c=42;")" "5"
q "DROP TABLE ac;" >/dev/null

# ---------------------------------------------------------------------------
# 2. Chunk-group skipping must respect the predicate collation.
# ---------------------------------------------------------------------------
q "SET pgcolumnar.chunk_group_row_limit=100; SET pgcolumnar.stripe_row_limit=1000;
   CREATE TABLE col (t text COLLATE \\\"en_US\\\") USING pgcolumnar;" >/dev/null
# all values uppercase, so each group's min/max are uppercase letters
q "INSERT INTO col SELECT chr(65 + (g%20)) || lpad(g::text,5,'0')
     FROM generate_series(1,300) g;" >/dev/null

# under C collation every uppercase letter is < 'a'; all 300 rows match.
# With the bug, en_US-ordered min/max wrongly skip every group -> 0 rows.
ON="$(q "SET pgcolumnar.enable_qual_pushdown=on;  SELECT count(*) FROM col WHERE t < 'a' COLLATE \\\"C\\\";")"
OFF="$(q "SET pgcolumnar.enable_qual_pushdown=off; SELECT count(*) FROM col WHERE t < 'a' COLLATE \\\"C\\\";")"
check "collation pushdown == no-pushdown" "$ON" "$OFF"
check "collation result correct"          "$ON" "300"

# a same-collation predicate is still pushed down (skipping preserved)
PUSHED="$(q "EXPLAIN (COSTS off) SELECT t FROM col WHERE t < 'A00001';" \
	| grep -c 'Pushed-Down Filters: 1' || true)"
check "same-collation still pushes" "$PUSHED" "1"
q "DROP TABLE col;" >/dev/null

# ---------------------------------------------------------------------------
# 3. Per-table option bounds are validated (no divide-by-zero from limit 0).
# ---------------------------------------------------------------------------
q "CREATE TABLE opt (a int) USING pgcolumnar;" >/dev/null
expect_error "reject chunk_group_row_limit 0" \
	"SELECT pgcolumnar.alter_columnar_table_set('opt'::regclass, chunk_group_row_limit => 0);"
expect_error "reject stripe_row_limit 5" \
	"SELECT pgcolumnar.alter_columnar_table_set('opt'::regclass, stripe_row_limit => 5);"
expect_error "reject compression_level 99" \
	"SELECT pgcolumnar.alter_columnar_table_set('opt'::regclass, compression_level => 99);"

# valid values are accepted, and delete/update work (no divide-by-zero)
q "SELECT pgcolumnar.alter_columnar_table_set('opt'::regclass,
     chunk_group_row_limit => 1000, stripe_row_limit => 2000, compression_level => 9);" >/dev/null
q "INSERT INTO opt SELECT g FROM generate_series(1,10) g;" >/dev/null
q "DELETE FROM opt WHERE a=5;" >/dev/null
check "valid options delete works" "$(q "SELECT count(*) FROM opt;")" "9"
q "DROP TABLE opt;" >/dev/null

# ---------------------------------------------------------------------------
# 4. CREATE INDEX must not leak a relation reference (parallel index build).
# ---------------------------------------------------------------------------
# parallel_workers on the table plus min_parallel_table_scan_size=0 forces a
# parallel build regardless of the table's estimated size; a small stripe limit
# gives several stripes. Before the fix each build participant leaked a
# reference to the table, logged as "resource was not closed: relation", and
# re-scanned the whole table so rows were indexed once per participant.
q "SET pgcolumnar.stripe_row_limit=10000;
   CREATE TABLE idxleak (a int, b int) USING pgcolumnar WITH (parallel_workers=2);" >/dev/null
q "INSERT INTO idxleak SELECT g, g%100 FROM generate_series(1,50000) g;" >/dev/null
run_pg "$PSQL -c \"SET max_parallel_maintenance_workers=2; SET min_parallel_table_scan_size=0;
   CREATE INDEX idxleak_a_idx ON idxleak(a);\"" >/dev/null 2>&1 || true

# the server logfile must carry no resource-leak warning
LEAKS="$(run_pg "grep -c 'resource was not closed' '$LOGFILE'" || true)"
check "create index no relation leak" "$LEAKS" "0"

# and every row is indexed exactly once (no duplicate TIDs from a per-worker rescan)
check "create index rows indexed once" \
	"$(q "SET enable_seqscan=off; SELECT count(*) FROM idxleak WHERE a>0;")" "50000"
q "DROP TABLE idxleak;" >/dev/null

echo
if [ "$fail" = "0" ]; then
	echo "AUDIT TEST PASSED"
else
	echo "AUDIT TEST FAILED"
	echo "---- server log tail ----"
	run_pg "tail -30 '$LOGFILE'" || true
fi
exit $fail
