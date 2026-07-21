#!/usr/bin/env bash
#
# pgColumnar phase 5 test: planner integration and vacuum. Covers the columnar
# custom scan (EXPLAIN shows it for a plain SELECT), qual pushdown driving
# chunk-group skipping (a filtered scan reads fewer chunk groups than an
# unfiltered one, while results are identical whether or not pushdown is on),
# column projection pushed into the scan, per-table options (compression,
# compression level, chunk-group row limit) taking effect for later writes and
# resetting to the instance default, vacuum combining stripes and reclaiming
# space from row-mask-deleted rows while returning correct rows (including with
# an index, which is rebuilt), and plan stability (no parallel sequential scan;
# a clean fallback to a sequential scan when the custom scan is disabled).
#
# Builds and installs the extension, spins up a throwaway PostgreSQL cluster as
# the postgres OS user, and exercises the phase 5 feature set. Written fresh for
# pgColumnar; it does not reuse any upstream test file or expected-output file.
#
# Usage:
#   test/phase5.sh [PG_CONFIG]
#
# PG_CONFIG defaults to /usr/local/pg17/bin/pg_config. Run as a user that may
# "runuser -u postgres" (e.g. root) when the current user is not postgres.

set -euo pipefail

PG_CONFIG="${1:-/usr/local/pg17/bin/pg_config}"
BINDIR="$("$PG_CONFIG" --bindir)"
PORT="${PGC_PORT:-54325}"
SRCDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

WORKDIR="$(mktemp -d /tmp/pgcolumnar-phase5.XXXXXX)"
PGDATA="$WORKDIR/data"
LOGFILE="$WORKDIR/server.log"

echo "== pgColumnar phase 5 test =="
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
run_pg "createdb -p $PORT p5"

# -qAtX: quiet, unaligned, tuples-only, no psqlrc, so a value is exactly output.
PSQL="psql -p $PORT -d p5 -qAtX -v ON_ERROR_STOP=1"
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

# assert an EXPLAIN plan contains / omits text
assert_plan() {
	# $1 name, $2 setup+query, $3 must-contain, $4 must-NOT-contain (or "")
	local name="$1" sql="$2" want="$3" notwant="${4:-}"
	local plan
	plan="$(run_pg "$PSQL -c \"$sql\"")"
	if echo "$plan" | grep -q "$want" && { [ -z "$notwant" ] || ! echo "$plan" | grep -q "$notwant"; }; then
		echo "PASS  $name"
	else
		echo "FAIL  $name: plan was:"
		echo "$plan" | sed 's/^/        /'
		fail=1
	fi
}

# run an EXPLAIN and echo the integer trailing a given label line
explain_num() {
	# $1 = full EXPLAIN sql, $2 = label to grep
	run_pg "$PSQL -c \"$1\"" | grep -F "$2" | grep -oE '[0-9]+$' | head -1
}

q "CREATE EXTENSION pgcolumnar;" >/dev/null

# ---------------------------------------------------------------------------
# The custom scan is chosen for a plain SELECT on a columnar table.
# ---------------------------------------------------------------------------
echo "-- custom scan path"
q "CREATE TABLE t (a int, b text, c int) USING pgcolumnar;" >/dev/null
q "INSERT INTO t SELECT g, 'r'||g, g%7 FROM generate_series(1,50000) g;" >/dev/null
assert_plan "plain select uses custom scan" \
	"EXPLAIN (COSTS OFF) SELECT * FROM t;" "Custom Scan (ColumnarScan)" "Seq Scan"
assert_plan "filtered select uses custom scan" \
	"EXPLAIN (COSTS OFF) SELECT a FROM t WHERE a = 12345;" \
	"Custom Scan (ColumnarScan)" "Seq Scan"

# ---------------------------------------------------------------------------
# Qual pushdown: a filtered scan skips chunk groups the min/max rule out; an
# unfiltered scan reads them all. Default chunk_group_row_limit is 10000, so
# 50000 rows form 5 chunk groups; a narrow range hits exactly one.
#
# An unfiltered count(*) is answered from catalog metadata (the covering-count
# fast path, gap 28) and never scans chunk groups, so it emits no chunk-group
# counters. sum(a) is used here to exercise the actual scan-and-skip path; a
# filtered count(*) declines the metadata path and scans, so it is checked too.
# ---------------------------------------------------------------------------
echo "-- chunk-group skipping from pushed-down quals"
EA="EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF)"
# covering count(*): no filter -> answered from metadata, no chunk-group scan
cover_lines="$(run_pg "$PSQL -c \"$EA SELECT count(*) FROM t;\"" | grep -c 'Chunk Groups Total' || true)"
check "covering count(*) skips the scan" "$cover_lines" "0"
# sum(a) always scans: unfiltered it reads every group and skips none
total_all="$(explain_num "$EA SELECT sum(a) FROM t;" 'Chunk Groups Total')"
skip_all="$(explain_num "$EA SELECT sum(a) FROM t;" 'Removed by Filter')"
check "unfiltered reads all groups" "$total_all" "5"
check "unfiltered skips none" "$skip_all" "0"
# a narrow range skips all but one group; verified via sum(a) and filtered count(*)
skip_f="$(explain_num "$EA SELECT sum(a) FROM t WHERE a BETWEEN 12000 AND 12100;" 'Chunk Groups Removed by Filter')"
read_f="$(explain_num "$EA SELECT sum(a) FROM t WHERE a BETWEEN 12000 AND 12100;" 'Chunk Groups Read')"
check "filtered reads one group" "$read_f" "1"
check "filtered skips four groups" "$skip_f" "4"
cnt_read_f="$(explain_num "$EA SELECT count(*) FROM t WHERE a BETWEEN 12000 AND 12100;" 'Chunk Groups Read')"
check "filtered count(*) also scans one group" "$cnt_read_f" "1"

# ---------------------------------------------------------------------------
# Skipping must never change results: the filtered query returns the same rows
# with pushdown on and with pushdown off.
# ---------------------------------------------------------------------------
echo "-- results identical with pushdown on vs off"
on_cnt="$(q "SET pgcolumnar.enable_qual_pushdown=on;  SELECT count(*) FROM t WHERE a BETWEEN 12000 AND 12100;")"
off_cnt="$(q "SET pgcolumnar.enable_qual_pushdown=off; SELECT count(*) FROM t WHERE a BETWEEN 12000 AND 12100;")"
check "pushdown-on count" "$on_cnt" "101"
check "pushdown matches off" "$on_cnt" "$off_cnt"
on_sum="$(q "SET pgcolumnar.enable_qual_pushdown=on;  SELECT sum(a) FROM t WHERE a > 49990;")"
off_sum="$(q "SET pgcolumnar.enable_qual_pushdown=off; SELECT sum(a) FROM t WHERE a > 49990;")"
check "pushdown-on sum matches off" "$on_sum" "$off_sum"
# equality on a low-cardinality column still returns every match, not just one group
check "equality full result" "$(q "SELECT count(*) FROM t WHERE c = 3;")" "$(q "SET pgcolumnar.enable_qual_pushdown=off; SELECT count(*) FROM t WHERE c = 3;")"

# ---------------------------------------------------------------------------
# Column projection: only referenced columns are read. Reported by EXPLAIN.
# ---------------------------------------------------------------------------
echo "-- column projection pushdown"
proj_one="$(explain_num "EXPLAIN (COSTS OFF) SELECT a FROM t WHERE a = 5;" 'Projected Columns')"
tot_cols="$(explain_num "EXPLAIN (COSTS OFF) SELECT a FROM t WHERE a = 5;" 'Total Columns')"
proj_star="$(explain_num "EXPLAIN (COSTS OFF) SELECT * FROM t;" 'Projected Columns')"
check "projects one column" "$proj_one" "1"
check "table has three columns" "$tot_cols" "3"
check "select star projects all" "$proj_star" "3"
# two referenced columns -> two projected
proj_two="$(explain_num "EXPLAIN (COSTS OFF) SELECT a, c FROM t WHERE a = 5;" 'Projected Columns')"
check "projects two columns" "$proj_two" "2"

# ---------------------------------------------------------------------------
# Per-table options take effect for subsequent writes.
# ---------------------------------------------------------------------------
echo "-- per-table options"
sid() { q "SELECT pgcolumnar.get_storage_id('$1');"; }

# default compression is zstd (code 3); overriding to none stores code 0
q "CREATE TABLE o_def (a int, b text) USING pgcolumnar;" >/dev/null
q "INSERT INTO o_def SELECT g, repeat('x',200) FROM generate_series(1,20000) g;" >/dev/null
check "default uses zstd" \
	"$(q "SELECT max(value_compression_type) FROM pgcolumnar.chunk WHERE storage_id=pgcolumnar.get_storage_id('o_def');")" "3"

q "CREATE TABLE o_none (a int, b text) USING pgcolumnar;" >/dev/null
q "SELECT pgcolumnar.alter_columnar_table_set('o_none', compression => 'none');" >/dev/null
q "INSERT INTO o_none SELECT g, repeat('x',200) FROM generate_series(1,20000) g;" >/dev/null
check "option none disables compression" \
	"$(q "SELECT max(value_compression_type) FROM pgcolumnar.chunk WHERE storage_id=pgcolumnar.get_storage_id('o_none');")" "0"

# compression level flows through to stored chunks
q "CREATE TABLE o_lvl (a int, b text) USING pgcolumnar;" >/dev/null
q "SELECT pgcolumnar.alter_columnar_table_set('o_lvl', compression => 'zstd', compression_level => 9);" >/dev/null
q "INSERT INTO o_lvl SELECT g, repeat('x',200) FROM generate_series(1,20000) g;" >/dev/null
check "option level 9 stored" \
	"$(q "SELECT max(value_compression_level) FROM pgcolumnar.chunk WHERE storage_id=pgcolumnar.get_storage_id('o_lvl') AND value_compression_type=3;")" "9"

# chunk_group_row_limit option changes how many chunk groups a stripe holds
q "CREATE TABLE o_cg (a int) USING pgcolumnar;" >/dev/null
q "SELECT pgcolumnar.alter_columnar_table_set('o_cg', chunk_group_row_limit => 1000);" >/dev/null
q "INSERT INTO o_cg SELECT g FROM generate_series(1,5000) g;" >/dev/null
check "chunk group limit applied" \
	"$(q "SELECT count(*) FROM pgcolumnar.chunk_group WHERE storage_id=pgcolumnar.get_storage_id('o_cg');")" "5"

# reset returns an option to the instance default
q "SELECT pgcolumnar.alter_columnar_table_set('o_reset', chunk_group_row_limit => 1000);" 2>/dev/null || true
q "CREATE TABLE o_reset (a int) USING pgcolumnar;" >/dev/null
q "SELECT pgcolumnar.alter_columnar_table_set('o_reset', chunk_group_row_limit => 1000);" >/dev/null
q "SELECT pgcolumnar.alter_columnar_table_reset('o_reset', chunk_group_row_limit => true);" >/dev/null
q "INSERT INTO o_reset SELECT g FROM generate_series(1,5000) g;" >/dev/null
check "reset restores default limit" \
	"$(q "SELECT count(*) FROM pgcolumnar.chunk_group WHERE storage_id=pgcolumnar.get_storage_id('o_reset');")" "1"

# Phase D2a: the format_version option round-trips (native writer honors it in a
# later phase; here it is recorded and read back). An invalid value is rejected,
# and the default is the 1.0-dev format (no row / NULL).
echo "-- format_version option (native format selection)"
q "CREATE TABLE o_fmt (a int) USING pgcolumnar;" >/dev/null
check "format_version default legacy" \
	"$(q "SELECT count(*) FROM pgcolumnar.options WHERE regclass='o_fmt'::regclass AND format_version IS NOT NULL;")" "0"
q "SELECT pgcolumnar.alter_columnar_table_set('o_fmt', format_version => 1);" >/dev/null
check "format_version set to native" \
	"$(q "SELECT format_version FROM pgcolumnar.options WHERE regclass='o_fmt'::regclass;")" "1"
if q "SELECT pgcolumnar.alter_columnar_table_set('o_fmt', format_version => 2);" >/dev/null 2>&1; then
	echo "FAIL  format_version invalid rejected: expected error"; fail=1
else
	echo "PASS  format_version invalid rejected"
fi
q "SELECT pgcolumnar.alter_columnar_table_reset('o_fmt', format_version => true);" >/dev/null
check "format_version reset to legacy" \
	"$(q "SELECT format_version FROM pgcolumnar.options WHERE regclass='o_fmt'::regclass;")" ""

# ---------------------------------------------------------------------------
# Vacuum: combine small stripes and reclaim deleted rows, returning correct
# data. stripe_row_limit=1000 makes 5 stripes; after deleting half and vacuum
# (with the limit reset), the live rows compact into a single stripe.
# ---------------------------------------------------------------------------
echo "-- vacuum compaction and space reclaim"
q "CREATE TABLE v (a int, b text) USING pgcolumnar;" >/dev/null
q "SELECT pgcolumnar.alter_columnar_table_set('v', stripe_row_limit => 1000);" >/dev/null
q "INSERT INTO v SELECT g, 'v'||g FROM generate_series(1,5000) g;" >/dev/null
check "v starts with five stripes" "$(q "SELECT count(*) FROM pgcolumnar.stats('v');")" "5"
q "DELETE FROM v WHERE a % 2 = 0;" >/dev/null
check "v live rows after delete" "$(q "SELECT count(*) FROM v;")" "2500"
check "v deleted rows tracked" "$(q "SELECT sum(deletedrows) FROM pgcolumnar.stats('v');")" "2500"
q "SELECT pgcolumnar.alter_columnar_table_reset('v', stripe_row_limit => true);" >/dev/null
q "SELECT pgcolumnar.vacuum('v');" >/dev/null
check "v compacts to one stripe" "$(q "SELECT count(*) FROM pgcolumnar.stats('v');")" "1"
check "v no deleted rows after vacuum" "$(q "SELECT COALESCE(sum(deletedrows),0) FROM pgcolumnar.stats('v');")" "0"
check "v rows correct after vacuum" "$(q "SELECT count(*) FROM v;")" "2500"
check "v sum correct after vacuum" "$(q "SELECT sum(a) FROM v;")" "$(q "SELECT sum(a) FROM (SELECT generate_series(1,5000) a) s WHERE a % 2 = 1;")"
check "v value intact after vacuum" "$(q "SELECT b FROM v WHERE a = 4999;")" "v4999"

# vacuum rebuilds indexes so index scans stay correct after renumbering
echo "-- vacuum rebuilds indexes"
q "CREATE TABLE vi (a int, b text) USING pgcolumnar;" >/dev/null
q "INSERT INTO vi SELECT g, 'i'||g FROM generate_series(1,20000) g;" >/dev/null
q "CREATE INDEX vi_a_idx ON vi (a);" >/dev/null
q "DELETE FROM vi WHERE a BETWEEN 100 AND 5000;" >/dev/null
q "SELECT pgcolumnar.vacuum('vi');" >/dev/null
check "vi count after vacuum" "$(q "SELECT count(*) FROM vi;")" "15099"
check "vi index point after vacuum" "$(q "SET enable_seqscan=off; SELECT b FROM vi WHERE a = 12345;")" "i12345"
check "vi index sees deletion after vacuum" "$(q "SET enable_seqscan=off; SELECT count(*) FROM vi WHERE a BETWEEN 100 AND 5000;")" "0"
check "vi index survivor after vacuum" "$(q "SET enable_seqscan=off; SELECT count(*) FROM vi WHERE a BETWEEN 1 AND 99;")" "99"

# vacuum_full across a schema
q "SELECT pgcolumnar.vacuum_full('public');" >/dev/null
check "vacuum_full keeps data" "$(q "SELECT count(*) FROM t;")" "50000"

# ---------------------------------------------------------------------------
# Plan stability: at default settings a columnar scan is the serial custom scan
# (no parallel sequential scan, and no Gather). Parallel columnar scans are now
# supported but only chosen when the planner deems them worthwhile; they are
# covered by test/parallel.sh.
# ---------------------------------------------------------------------------
echo "-- default plan is the serial custom scan"
assert_plan "columnar default uses custom scan, not seqscan" \
	"EXPLAIN (COSTS OFF) SELECT count(*) FROM t;" \
	"Custom Scan (ColumnarScan)" "Seq Scan"
assert_plan "columnar default plan is not parallel" \
	"EXPLAIN (COSTS OFF) SELECT count(*) FROM t;" \
	"Custom Scan (ColumnarScan)" "Gather"

# ---------------------------------------------------------------------------
# Disabling the custom scan falls back cleanly to a sequential scan.
# ---------------------------------------------------------------------------
echo "-- custom scan can be disabled"
assert_plan "disable custom scan -> seq scan" \
	"SET pgcolumnar.enable_custom_scan=off; EXPLAIN (COSTS OFF) SELECT count(*) FROM t;" \
	"Seq Scan" "Custom Scan"
check "seq-scan fallback returns correct rows" \
	"$(q "SET pgcolumnar.enable_custom_scan=off; SELECT count(*) FROM t WHERE a BETWEEN 12000 AND 12100;")" "101"

echo
if [ "$fail" = "0" ]; then
	echo "PHASE 5 TEST PASSED"
else
	echo "PHASE 5 TEST FAILED"
	echo "---- server log tail ----"
	run_pg "tail -60 '$LOGFILE'" || true
fi
exit $fail
