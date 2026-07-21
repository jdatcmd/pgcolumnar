#!/usr/bin/env bash
#
# pgColumnar multiple-projections DDL/catalog coverage (gap 26, phase 1).
#
# Phase 1 adds the columnar.projection catalog and the add_projection /
# drop_projection DDL; no data is written to a projection's storage yet, so this
# suite checks catalog shape and DDL semantics only: base projection recorded
# lazily, column/sort-key resolution and validation, name uniqueness, storage-id
# allocation, and drop. Later phases add differential read/write coverage.
#
# Usage:  test/projections.sh [PG_CONFIG]
#
# Written fresh for pgColumnar; it does not reuse any upstream test file.

set -uo pipefail

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

# Run a statement expected to FAIL; PASS the check when it errors out.
expect_fail() {
	local name="$1" sql="$2"
	PGC_CHECKS=$((PGC_CHECKS + 1))
	if psql_run "$sql" >/dev/null 2>&1; then
		echo "FAIL  $name: statement unexpectedly succeeded"
		PGC_FAIL=1
	else
		echo "PASS  $name"
	fi
}

proj_q() { q "SELECT $1 FROM columnar.projection WHERE storage_id = $SID $2;"; }

echo "-- setup"
psql_run "CREATE TABLE p (a int, b text, c int) USING columnar;"
psql_run "INSERT INTO p SELECT g, 'r'||g, g*2 FROM generate_series(1,100) g;"
check "table populated" "$(q 'SELECT count(*) FROM p;')" "100"
SID="$(q "SELECT columnar.get_storage_id('p');")"
check "storage id resolves" "$([ -n "$SID" ] && echo ok)" "ok"

# Base projection is recorded lazily, so before any add there are no rows.
check "no projection rows before first add" "$(proj_q 'count(*)' '')" "0"

echo "-- add_projection records base lazily and the new projection"
psql_run "SELECT columnar.add_projection('p', 'p1', ARRAY['a','c'], ARRAY['c']);"
check "two rows after first add (base + p1)" "$(proj_q 'count(*)' '')" "2"
check "base id 0 columns are all attrs" "$(proj_q 'columns' 'AND projection_id = 0')" "{1,2,3}"
check "base id 0 sort_key empty"         "$(proj_q 'sort_key' 'AND projection_id = 0')" "{}"
check "base id 0 name"                    "$(proj_q 'name' 'AND projection_id = 0')" "base"
check "base proj_storage_id == base"      "$(proj_q 'proj_storage_id = storage_id' 'AND projection_id = 0')" "t"
check "p1 id is 1"                         "$(proj_q 'projection_id' \"AND name = 'p1'\")" "1"
check "p1 columns"                         "$(proj_q 'columns' \"AND name = 'p1'\")" "{1,3}"
check "p1 sort_key"                        "$(proj_q 'sort_key' \"AND name = 'p1'\")" "{3}"
check "p1 has its own storage id"          "$(proj_q 'proj_storage_id <> storage_id' \"AND name = 'p1'\")" "t"

echo "-- a second projection with no sort key"
psql_run "SELECT columnar.add_projection('p', 'p2', ARRAY['b']);"
check "p2 id is 2"        "$(proj_q 'projection_id' \"AND name = 'p2'\")" "2"
check "p2 columns"        "$(proj_q 'columns' \"AND name = 'p2'\")" "{2}"
check "p2 sort_key empty" "$(proj_q 'sort_key' \"AND name = 'p2'\")" "{}"
check "distinct storage ids" \
	"$(q "SELECT count(DISTINCT proj_storage_id) FROM columnar.projection WHERE storage_id = $SID;")" "3"

echo "-- validation errors"
expect_fail "duplicate name rejected"       "SELECT columnar.add_projection('p', 'p1', ARRAY['a']);"
expect_fail "unknown column rejected"       "SELECT columnar.add_projection('p', 'px', ARRAY['zzz']);"
expect_fail "empty columns rejected"        "SELECT columnar.add_projection('p', 'pe', ARRAY[]::text[]);"
expect_fail "duplicate column rejected"     "SELECT columnar.add_projection('p', 'pd', ARRAY['a','a']);"
expect_fail "sort key not in columns"       "SELECT columnar.add_projection('p', 'ps', ARRAY['a'], ARRAY['b']);"
expect_fail "add on heap table rejected"    "CREATE TABLE h (x int); SELECT columnar.add_projection('h', 'ph', ARRAY['x']);"
expect_fail "drop base rejected"            "SELECT columnar.drop_projection('p', 'base');"
expect_fail "drop unknown rejected"         "SELECT columnar.drop_projection('p', 'nope');"

echo "-- drop_projection"
psql_run "SELECT columnar.drop_projection('p', 'p1');"
check "p1 gone after drop"  "$(proj_q 'count(*)' \"AND name = 'p1'\")" "0"
check "base + p2 remain"    "$(proj_q 'count(*)' '')" "2"
check "table still readable after DDL" "$(q 'SELECT count(*) FROM p;')" "100"

pgc_summary
