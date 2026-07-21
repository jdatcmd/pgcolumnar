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
check "p1 id is 1"                         "$(proj_q 'projection_id' "AND name = 'p1'")" "1"
check "p1 columns"                         "$(proj_q 'columns' "AND name = 'p1'")" "{1,3}"
check "p1 sort_key"                        "$(proj_q 'sort_key' "AND name = 'p1'")" "{3}"
check "p1 has its own storage id"          "$(proj_q 'proj_storage_id <> storage_id' "AND name = 'p1'")" "t"

echo "-- a second projection with no sort key"
psql_run "SELECT columnar.add_projection('p', 'p2', ARRAY['b']);"
check "p2 id is 2"        "$(proj_q 'projection_id' "AND name = 'p2'")" "2"
check "p2 columns"        "$(proj_q 'columns' "AND name = 'p2'")" "{2}"
check "p2 sort_key empty" "$(proj_q 'sort_key' "AND name = 'p2'")" "{}"
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
check "p1 gone after drop"  "$(proj_q 'count(*)' "AND name = 'p1'")" "0"
check "base + p2 remain"    "$(proj_q 'count(*)' '')" "2"
check "table still readable after DDL" "$(q 'SELECT count(*) FROM p;')" "100"

# Phase 2a does not back-fill: p2 was added after p already had 100 rows, so its
# storage is empty until a future statement inserts (or a back-fill phase).
check "no back-fill: p2 empty on populated table" \
	"$(q "SELECT count(*) FROM columnar.read_projection('p', 'p2');")" "0"

# ---------------------------------------------------------------------------
# Phase 2: write fan-out. Projections declared before load are populated by
# INSERT; read_projection reproduces the base's live rows for the projection's
# columns (deletes come from the base row_mask). Values are rendered by their
# output functions and joined by '|' on both sides for an exact multiset match.
# ---------------------------------------------------------------------------
echo "-- phase 2: write fan-out populates projections"
psql_run "CREATE TABLE fo (a int, b text, c int) USING columnar;"
psql_run "SELECT columnar.add_projection('fo', 'fp', ARRAY['a','c'], ARRAY['c']);"
psql_run "SELECT columnar.add_projection('fo', 'fq', ARRAY['b']);"
psql_run "INSERT INTO fo SELECT g, 'r'||g, (g*7)%100 FROM generate_series(1,5000) g;"

check "fp fan-out matches base (a,c)" \
	"$(pgc_set_hash "SELECT columnar.read_projection('fo','fp')")" \
	"$(pgc_set_hash "SELECT a::text || '|' || c::text FROM fo")"
check "fq fan-out matches base (b)" \
	"$(pgc_set_hash "SELECT columnar.read_projection('fo','fq')")" \
	"$(pgc_set_hash "SELECT b FROM fo")"
check "fp row count matches base" \
	"$(q "SELECT count(*) FROM columnar.read_projection('fo','fp');")" \
	"$(q 'SELECT count(*) FROM fo;')"

echo "-- phase 2: deletes reflected via the base row_mask"
psql_run "DELETE FROM fo WHERE a BETWEEN 1000 AND 2000;"
check "fp reflects deletes (a,c)" \
	"$(pgc_set_hash "SELECT columnar.read_projection('fo','fp')")" \
	"$(pgc_set_hash "SELECT a::text || '|' || c::text FROM fo")"
check "fp count after delete matches base" \
	"$(q "SELECT count(*) FROM columnar.read_projection('fo','fp');")" \
	"$(q 'SELECT count(*) FROM fo;')"

echo "-- phase 2: multi-stripe fan-out (exceed the stripe row limit)"
psql_run "SELECT columnar.alter_columnar_table_set('fo', stripe_row_limit => 2000);"
psql_run "CREATE TABLE fo2 (a int, c int) USING columnar;"
psql_run "SELECT columnar.alter_columnar_table_set('fo2', stripe_row_limit => 2000);"
psql_run "SELECT columnar.add_projection('fo2', 'fp2', ARRAY['a','c'], ARRAY['c']);"
psql_run "INSERT INTO fo2 SELECT g, (g*13)%1000 FROM generate_series(1,7000) g;"
check "fp2 multi-stripe fan-out matches base" \
	"$(pgc_set_hash "SELECT columnar.read_projection('fo2','fp2')")" \
	"$(pgc_set_hash "SELECT a::text || '|' || c::text FROM fo2")"
check "fp2 spans multiple projection stripes" \
	"$([ "$(q "SELECT count(*) FROM columnar.stripe WHERE storage_id = (SELECT proj_storage_id FROM columnar.projection WHERE storage_id = columnar.get_storage_id('fo2') AND name='fp2');")" -ge 2 ] && echo yes || echo no)" "yes"

echo "-- phase 2: reading the base projection by name is rejected"
expect_fail "read_projection base rejected" "SELECT columnar.read_projection('fo', 'base');"

pgc_summary
