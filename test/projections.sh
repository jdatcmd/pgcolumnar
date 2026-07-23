#!/usr/bin/env bash
#
# pgColumnar multiple-projections DDL/catalog coverage (gap 26, phase 1).
#
# Phase 1 adds the pgcolumnar.projection catalog and the add_projection /
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

proj_q() { q "SELECT $1 FROM pgcolumnar.projection WHERE storage_id = $SID $2;"; }

echo "-- setup"
psql_run "CREATE TABLE p (a int, b text, c int) USING pgcolumnar;"
psql_run "INSERT INTO p SELECT g, 'r'||g, g*2 FROM generate_series(1,100) g;"
check "table populated" "$(q 'SELECT count(*) FROM p;')" "100"
SID="$(q "SELECT pgcolumnar.get_storage_id('p');")"
check "storage id resolves" "$([ -n "$SID" ] && echo ok)" "ok"

# Base projection is recorded lazily, so before any add there are no rows.
check "no projection rows before first add" "$(proj_q 'count(*)' '')" "0"

echo "-- add_projection records base lazily and the new projection"
psql_run "SELECT pgcolumnar.add_projection('p', 'p1', ARRAY['a','c'], ARRAY['c']);"
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
psql_run "SELECT pgcolumnar.add_projection('p', 'p2', ARRAY['b']);"
check "p2 id is 2"        "$(proj_q 'projection_id' "AND name = 'p2'")" "2"
check "p2 columns"        "$(proj_q 'columns' "AND name = 'p2'")" "{2}"
check "p2 sort_key empty" "$(proj_q 'sort_key' "AND name = 'p2'")" "{}"
check "distinct storage ids" \
	"$(q "SELECT count(DISTINCT proj_storage_id) FROM pgcolumnar.projection WHERE storage_id = $SID;")" "3"

echo "-- validation errors"
expect_fail "duplicate name rejected"       "SELECT pgcolumnar.add_projection('p', 'p1', ARRAY['a']);"
expect_fail "unknown column rejected"       "SELECT pgcolumnar.add_projection('p', 'px', ARRAY['zzz']);"
expect_fail "empty columns rejected"        "SELECT pgcolumnar.add_projection('p', 'pe', ARRAY[]::text[]);"
expect_fail "duplicate column rejected"     "SELECT pgcolumnar.add_projection('p', 'pd', ARRAY['a','a']);"
expect_fail "sort key not in columns"       "SELECT pgcolumnar.add_projection('p', 'ps', ARRAY['a'], ARRAY['b']);"
expect_fail "add on heap table rejected"    "CREATE TABLE h (x int); SELECT pgcolumnar.add_projection('h', 'ph', ARRAY['x']);"
expect_fail "drop base rejected"            "SELECT pgcolumnar.drop_projection('p', 'base');"
expect_fail "drop unknown rejected"         "SELECT pgcolumnar.drop_projection('p', 'nope');"

echo "-- drop_projection"
psql_run "SELECT pgcolumnar.drop_projection('p', 'p1');"
check "p1 gone after drop"  "$(proj_q 'count(*)' "AND name = 'p1'")" "0"
check "base + p2 remain"    "$(proj_q 'count(*)' '')" "2"
check "table still readable after DDL" "$(q 'SELECT count(*) FROM p;')" "100"

# add_projection back-fills from existing rows (gap 26 phase 6): p2 was added
# after p already had 100 rows, so its storage is populated with those rows.
check "back-fill: p2 populated from existing rows" \
	"$(q "SELECT count(*) FROM pgcolumnar.read_projection('p', 'p2');")" "100"
check "back-fill: p2 matches base (b column)" \
	"$(pgc_set_hash "SELECT pgcolumnar.read_projection('p','p2')")" \
	"$(pgc_set_hash "SELECT b FROM p")"

# ---------------------------------------------------------------------------
# Phase 2: write fan-out. Projections declared before load are populated by
# INSERT; read_projection reproduces the base's live rows for the projection's
# columns (deletes come from the base delete_vector). Values are rendered by their
# output functions and joined by '|' on both sides for an exact multiset match.
# ---------------------------------------------------------------------------
echo "-- phase 2: write fan-out populates projections"
psql_run "CREATE TABLE fo (a int, b text, c int) USING pgcolumnar;"
psql_run "SELECT pgcolumnar.add_projection('fo', 'fp', ARRAY['a','c'], ARRAY['c']);"
psql_run "SELECT pgcolumnar.add_projection('fo', 'fq', ARRAY['b']);"
psql_run "INSERT INTO fo SELECT g, 'r'||g, (g*7)%100 FROM generate_series(1,5000) g;"

check "fp fan-out matches base (a,c)" \
	"$(pgc_set_hash "SELECT pgcolumnar.read_projection('fo','fp')")" \
	"$(pgc_set_hash "SELECT a::text || '|' || c::text FROM fo")"
check "fq fan-out matches base (b)" \
	"$(pgc_set_hash "SELECT pgcolumnar.read_projection('fo','fq')")" \
	"$(pgc_set_hash "SELECT b FROM fo")"
check "fp row count matches base" \
	"$(q "SELECT count(*) FROM pgcolumnar.read_projection('fo','fp');")" \
	"$(q 'SELECT count(*) FROM fo;')"

# Phase 4a: projection chunks carry per-chunk min/max skip metadata, so a sorted
# projection gives tight ranges the planner can use (gap 26). Native projections
# carry the same skip metadata in the zone map (D6e).
fp_sid="(SELECT proj_storage_id FROM pgcolumnar.projection WHERE storage_id = pgcolumnar.get_storage_id('fo') AND name='fp')"
fp_minmax="$(q "SELECT count(*) FROM pgcolumnar.zone_map WHERE storage_id = $fp_sid AND minimum IS NOT NULL;")"
check "fp chunks carry min/max skip metadata" \
	"$([ "$fp_minmax" -ge 1 ] && echo yes || echo no)" "yes"

echo "-- phase 2: deletes reflected via the base delete_vector"
psql_run "DELETE FROM fo WHERE a BETWEEN 1000 AND 2000;"
check "fp reflects deletes (a,c)" \
	"$(pgc_set_hash "SELECT pgcolumnar.read_projection('fo','fp')")" \
	"$(pgc_set_hash "SELECT a::text || '|' || c::text FROM fo")"
check "fp count after delete matches base" \
	"$(q "SELECT count(*) FROM pgcolumnar.read_projection('fo','fp');")" \
	"$(q 'SELECT count(*) FROM fo;')"

echo "-- phase 2: multi-stripe fan-out (exceed the stripe row limit)"
psql_run "SELECT pgcolumnar.alter_columnar_table_set('fo', stripe_row_limit => 2000);"
psql_run "CREATE TABLE fo2 (a int, c int) USING pgcolumnar;"
psql_run "SELECT pgcolumnar.alter_columnar_table_set('fo2', stripe_row_limit => 2000);"
psql_run "SELECT pgcolumnar.add_projection('fo2', 'fp2', ARRAY['a','c'], ARRAY['c']);"
psql_run "INSERT INTO fo2 SELECT g, (g*13)%1000 FROM generate_series(1,7000) g;"
check "fp2 multi-stripe fan-out matches base" \
	"$(pgc_set_hash "SELECT pgcolumnar.read_projection('fo2','fp2')")" \
	"$(pgc_set_hash "SELECT a::text || '|' || c::text FROM fo2")"
fp2_sid="(SELECT proj_storage_id FROM pgcolumnar.projection WHERE storage_id = pgcolumnar.get_storage_id('fo2') AND name='fp2')"
fp2_stripes="$(q "SELECT count(*) FROM pgcolumnar.row_group WHERE storage_id = $fp2_sid;")"
check "fp2 spans multiple projection row groups" \
	"$([ "$fp2_stripes" -ge 2 ] && echo yes || echo no)" "yes"

echo "-- phase 2: reading the base projection by name is rejected"
expect_fail "read_projection base rejected" "SELECT pgcolumnar.read_projection('fo', 'base');"

# ---------------------------------------------------------------------------
# Phase 3: row-number reconstruction. A projection on a subset of columns is
# read, and the columns it does not store are fetched from the base by the
# projection's stored row number. The reconstructed full rows must match the
# base exactly (this also proves the projection<->base row-number linkage).
# ---------------------------------------------------------------------------
echo "-- phase 3: reconstruct non-covered columns from the base by row number"
psql_run "CREATE TABLE rc (a int, b text, c int) USING pgcolumnar;"
psql_run "SELECT pgcolumnar.add_projection('rc', 'rp', ARRAY['a','c'], ARRAY['c']);"
psql_run "INSERT INTO rc SELECT g, 'r'||g, (g*7)%100 FROM generate_series(1,5000) g;"

# rp stores only (a,c); reconstruct must pull b from the base by row number.
check "reconstruct full row matches base" \
	"$(pgc_set_hash "SELECT pgcolumnar.reconstruct_via_projection('rc','rp')")" \
	"$(pgc_set_hash "SELECT a::text || '|' || b || '|' || c::text FROM rc")"

echo "-- phase 3: reconstruction reflects deletes and NULLs"
psql_run "INSERT INTO rc VALUES (99991, NULL, NULL), (99992, 'x', NULL);"
psql_run "DELETE FROM rc WHERE a BETWEEN 2000 AND 3000;"
check "reconstruct matches base after delete + NULLs" \
	"$(pgc_set_hash "SELECT pgcolumnar.reconstruct_via_projection('rc','rp')")" \
	"$(pgc_set_hash "SELECT coalesce(a::text,'\\N') || '|' || coalesce(b,'\\N') || '|' || coalesce(c::text,'\\N') FROM rc")"
check "reconstruct row count matches base" \
	"$(q "SELECT count(*) FROM pgcolumnar.reconstruct_via_projection('rc','rp');")" \
	"$(q 'SELECT count(*) FROM rc;')"

# ---------------------------------------------------------------------------
# Phase 4b: the planner scans a covering projection when its sort key is
# restricted, and the executor returns correct rows from the projection storage
# (deletes filtered by the base delete_vector). pgcolumnar.enable_projection_scan gates
# it; a query referencing a non-covered column falls back to the base.
# ---------------------------------------------------------------------------
echo "-- phase 4b: planner selects a covering projection for a sort-key predicate"
psql_run "CREATE TABLE ps (a int, b text, c int) USING pgcolumnar;"
psql_run "SELECT pgcolumnar.add_projection('ps', 'pc', ARRAY['a','c'], ARRAY['c']);"
psql_run "INSERT INTO ps SELECT g, 'r'||g, (g*7)%1000 FROM generate_series(1,20000) g;"
psql_run "CREATE TABLE ps_h (a int, b text, c int) USING heap;"
psql_run "INSERT INTO ps_h SELECT g, 'r'||g, (g*7)%1000 FROM generate_series(1,20000) g;"

check "projection chosen for covering + sort-key query" \
	"$(q "EXPLAIN (COSTS OFF) SELECT a, c FROM ps WHERE c BETWEEN 100 AND 200;" | grep -c 'Columnar Projection: pc')" "1"
check "projection-scan results match heap oracle" \
	"$(pgc_set_hash "SELECT a, c FROM ps WHERE c BETWEEN 100 AND 200")" \
	"$(pgc_set_hash "SELECT a, c FROM ps_h WHERE c BETWEEN 100 AND 200")"
check "aggregate over projection scan matches oracle" \
	"$(q "SELECT count(*), sum(a) FROM ps WHERE c BETWEEN 100 AND 200;")" \
	"$(q "SELECT count(*), sum(a) FROM ps_h WHERE c BETWEEN 100 AND 200;")"

check "GUC off: no projection scan" \
	"$(q "SET pgcolumnar.enable_projection_scan=off; EXPLAIN (COSTS OFF) SELECT a, c FROM ps WHERE c BETWEEN 100 AND 200;" | grep -c 'Columnar Projection')" "0"
check "non-covering query (references b) uses the base" \
	"$(q "EXPLAIN (COSTS OFF) SELECT a, b, c FROM ps WHERE c BETWEEN 100 AND 200;" | grep -c 'Columnar Projection')" "0"

echo "-- phase 4b: projection scan reflects deletes (base delete_vector)"
psql_run "DELETE FROM ps   WHERE a BETWEEN 5000 AND 6000;"
psql_run "DELETE FROM ps_h WHERE a BETWEEN 5000 AND 6000;"
check "projection scan matches oracle after delete" \
	"$(pgc_set_hash "SELECT a, c FROM ps WHERE c BETWEEN 100 AND 200")" \
	"$(pgc_set_hash "SELECT a, c FROM ps_h WHERE c BETWEEN 100 AND 200")"
check "full-range projection scan matches oracle" \
	"$(pgc_set_hash "SELECT a, c FROM ps")" \
	"$(pgc_set_hash "SELECT a, c FROM ps_h")"

# ---------------------------------------------------------------------------
# Phase 5: pgcolumnar.vacuum compacts the base into fresh storage with new row
# numbers, so it must rebuild every projection aligned to the compacted base.
# After vacuum the projection must still exist, still be chosen by the planner,
# and still match the heap oracle.
# ---------------------------------------------------------------------------
echo "-- phase 5: pgcolumnar.vacuum rebuilds projections aligned to the compacted base"
psql_run "CREATE TABLE pv (a int, b text, c int) USING pgcolumnar;"
psql_run "SELECT pgcolumnar.add_projection('pv', 'pvp', ARRAY['a','c'], ARRAY['c']);"
psql_run "INSERT INTO pv SELECT g, 'r'||g, (g*7)%1000 FROM generate_series(1,20000) g;"
psql_run "DELETE FROM pv WHERE a BETWEEN 5000 AND 8000;"
psql_run "CREATE TABLE pv_h (a int, b text, c int) USING heap;"
psql_run "INSERT INTO pv_h SELECT g, 'r'||g, (g*7)%1000 FROM generate_series(1,20000) g WHERE g NOT BETWEEN 5000 AND 8000;"

psql_run "SELECT pgcolumnar.vacuum('pv');"
check "projection survives vacuum" \
	"$(q "SELECT count(*) FROM pgcolumnar.projection WHERE storage_id = pgcolumnar.get_storage_id('pv') AND name='pvp';")" "1"
check "read_projection matches base after vacuum" \
	"$(pgc_set_hash "SELECT pgcolumnar.read_projection('pv','pvp')")" \
	"$(pgc_set_hash "SELECT a::text || '|' || c::text FROM pv_h")"
check "planner still uses projection after vacuum" \
	"$(q "EXPLAIN (COSTS OFF) SELECT a, c FROM pv WHERE c BETWEEN 100 AND 200;" | grep -c 'Columnar Projection: pvp')" "1"
check "projection-scan matches oracle after vacuum" \
	"$(pgc_set_hash "SELECT a, c FROM pv WHERE c BETWEEN 100 AND 200")" \
	"$(pgc_set_hash "SELECT a, c FROM pv_h WHERE c BETWEEN 100 AND 200")"
check "reconstruct (a,b,c) matches base after vacuum" \
	"$(pgc_set_hash "SELECT pgcolumnar.reconstruct_via_projection('pv','pvp')")" \
	"$(pgc_set_hash "SELECT a::text||'|'||b||'|'||c::text FROM pv_h")"

echo "-- phase 5: a second vacuum stays correct (fresh row numbers again)"
psql_run "DELETE FROM pv   WHERE a BETWEEN 100 AND 200;"
psql_run "DELETE FROM pv_h WHERE a BETWEEN 100 AND 200;"
psql_run "SELECT pgcolumnar.vacuum('pv');"
check "projection matches base after second vacuum" \
	"$(pgc_set_hash "SELECT pgcolumnar.read_projection('pv','pvp')")" \
	"$(pgc_set_hash "SELECT a::text || '|' || c::text FROM pv_h")"

# ---------------------------------------------------------------------------
# Phase 6: MVCC. An old REPEATABLE READ snapshot must not see rows committed by
# another session after it, even through a projection scan -- the projection
# stripe list and the base liveness check both use the query snapshot.
# ---------------------------------------------------------------------------
echo "-- phase 6: old snapshot never sees post-snapshot rows via a projection scan"
psql_run "CREATE TABLE pm (a int, c int) USING pgcolumnar;"
psql_run "SELECT pgcolumnar.add_projection('pm', 'pmp', ARRAY['a','c'], ARRAY['c']);"
psql_run "INSERT INTO pm SELECT g, g FROM generate_series(1,10000) g;"   # batch 1

A_IN="$PGC_WORKDIR/pmA.in"; A_OUT="$PGC_WORKDIR/pmA.out"
mkfifo "$A_IN"; : > "$A_OUT"
env PATH="$PGC_BINDIR:$PATH" psql -h 127.0.0.1 -p "$PGC_PORT" -U postgres \
	-d "$PGC_DB" -At -f "$A_IN" >> "$A_OUT" 2>&1 &
A_PID=$!
exec 9> "$A_IN"
pa_send() { printf '%s\n' "$*" >&9; }
pa_wait() { local t="$1" i; for i in $(seq 1 200); do grep -q "$t" "$A_OUT" && return 0; sleep 0.1; done; return 1; }

# A opens a snapshot that sees batch 1, using a projection scan.
pa_send "SET pgcolumnar.enable_projection_scan = on;"
pa_send "BEGIN ISOLATION LEVEL REPEATABLE READ;"
pa_send "SELECT 'PMA_' || count(a) FROM pm WHERE c BETWEEN 1 AND 20000;"
if pa_wait "PMA_"; then
	snap="$(grep -o 'PMA_[0-9]*' "$A_OUT" | tail -1 | sed 's/PMA_//')"
	check "session A baseline via projection sees batch 1" "$snap" "10000"

	psql_run "INSERT INTO pm SELECT g, g FROM generate_series(10001,20000) g;"  # batch 2 commits

	pa_send "SELECT 'PMB_' || count(a) FROM pm WHERE c BETWEEN 1 AND 20000;"
	if pa_wait "PMB_"; then
		see="$(grep -o 'PMB_[0-9]*' "$A_OUT" | tail -1 | sed 's/PMB_//')"
		check "old snapshot projection scan does not see post-snapshot rows" "$see" "10000"
	else
		check "session A responded post-commit" "timeout" "ok"
	fi
	pa_send "COMMIT;"
else
	check "session A opened snapshot" "timeout" "ok"
fi
pa_send "SELECT 'PMF_' || count(a) FROM pm WHERE c BETWEEN 1 AND 20000;"
pa_wait "PMF_" || true
final="$(grep -o 'PMF_[0-9]*' "$A_OUT" | tail -1 | sed 's/PMF_//')"
check "new snapshot projection scan sees both batches" "$final" "20000"
exec 9>&-
wait "$A_PID" 2>/dev/null || true

pgc_summary
