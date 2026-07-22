#!/usr/bin/env bash
#
# pgColumnar sorted single-projection suite (gap 26, piece 1).
#
# pgcolumnar.vacuum_sorted() rewrites a columnar table physically sorted on a
# chosen key. This must never change query results (verified against a heap
# oracle, order-independent), and it must actually reorder storage so that a
# range predicate on the sort key skips more chunk groups than before. Also
# checks deleted-row reclaim + index rebuild after the sorted rewrite, argument
# validation, and idempotence.
#
# Usage:  test/sorted_projection.sh [PG_CONFIG]
#
# Written fresh for pgColumnar; it does not reuse any upstream test file.

set -uo pipefail

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

# Number of vectors a filtered aggregate removes via min/max skipping. sum()
# (not count(*)) is used so the scan actually runs -- an unfiltered count(*) is
# answered from metadata and never scans (see test/phase5.sh). The native format
# keeps one row group here and prunes at the per-vector level, reported as
# "Columnar Vectors Skipped".
groups_removed() {
	q "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) $1" \
		| grep -F "Vectors Skipped" | grep -oE '[0-9]+$' | head -1
}

expect_error() {
	local label="$1" sql="$2"
	if psql_run "$sql" >/dev/null 2>&1; then
		check "$label (expected error)" "succeeded" "error"
	else
		check "$label" "error" "error"
	fi
}

# Many small chunk groups; the sort key k is scrambled relative to insert order
# so that, before sorting, every group's [min,max] for k spans nearly the whole
# domain and range skipping cannot prune. v is unique for aggregate checks.
make_pair "id int, k int, v bigint, t text"
q "SELECT pgcolumnar.alter_columnar_table_set('t_col', chunk_group_row_limit => 1000);" >/dev/null
load_pair "SELECT g, ((g*7919)%1000), g::bigint*2, 'r'||g FROM generate_series(1,50000) g"

# ---------------------------------------------------------------------------
# Correctness is invariant under the sorted rewrite: identical to the heap
# oracle both before and after vacuum_sorted, across query shapes.
# ---------------------------------------------------------------------------
echo "-- results identical to heap before sorting"
diff_query "pre range"      "SELECT id, v, t FROM %T WHERE k BETWEEN 100 AND 120"
diff_query "pre equality"   "SELECT id FROM %T WHERE k = 500"
diff_query "pre order-by"   "SELECT k, v FROM %T ORDER BY k, v"
diff_query "pre aggregate"  "SELECT k, count(*), sum(v) FROM %T GROUP BY k"
diff_query "pre star"       "SELECT * FROM %T WHERE id % 997 = 0"

removed_before="$(groups_removed "SELECT sum(v) FROM t_col WHERE k BETWEEN 100 AND 120")"

echo "-- sort the columnar table on k"
q "SELECT pgcolumnar.vacuum_sorted('t_col', 'k');" >/dev/null

echo "-- results still identical to heap after sorting"
diff_query "post range"     "SELECT id, v, t FROM %T WHERE k BETWEEN 100 AND 120"
diff_query "post equality"  "SELECT id FROM %T WHERE k = 500"
diff_query "post order-by"  "SELECT k, v FROM %T ORDER BY k, v"
diff_query "post aggregate" "SELECT k, count(*), sum(v) FROM %T GROUP BY k"
diff_query "post star"      "SELECT * FROM %T WHERE id % 997 = 0"

# ---------------------------------------------------------------------------
# The reorder took physical effect: a narrow range on the sort key now prunes
# strictly more chunk groups than before.
# ---------------------------------------------------------------------------
echo "-- sorting improves chunk-group skipping on the sort key"
removed_after="$(groups_removed "SELECT sum(v) FROM t_col WHERE k BETWEEN 100 AND 120")"
check "pre-sort prunes little" "$removed_before" "0"
if [ "${removed_after:-0}" -gt "$removed_before" ]; then
	check "post-sort prunes more chunk groups" "more" "more"
else
	check "post-sort prunes more chunk groups" "removed_before=$removed_before removed_after=$removed_after" "more"
fi

# ---------------------------------------------------------------------------
# Multi-column sort key, then reclaim + index rebuild after a sorted rewrite.
# ---------------------------------------------------------------------------
echo "-- multi-column sort key stays correct"
q "SELECT pgcolumnar.vacuum_sorted('t_col', 'k', 'v');" >/dev/null
diff_query "multi-key range" "SELECT id, v FROM %T WHERE k BETWEEN 100 AND 120"

echo "-- reclaim and index rebuild after sorted rewrite"
q "CREATE INDEX t_col_id_idx ON t_col (id);" >/dev/null
psql_run "DELETE FROM t_heap WHERE id % 2 = 0;" >/dev/null
psql_run "DELETE FROM t_col  WHERE id % 2 = 0;" >/dev/null
q "SELECT pgcolumnar.vacuum_sorted('t_col', 'k');" >/dev/null
check "live count after delete+sort" "$(q "SELECT count(*) FROM t_col;")" "25000"
diff_query "count matches heap"  "SELECT count(*) FROM %T"
diff_query "sum matches heap"    "SELECT sum(v) FROM %T"
# SET prints its own command tag; the scalar value is the last line.
check "index point lookup after sort" \
	"$(q "SET enable_seqscan=off; SELECT t FROM t_col WHERE id = 4999;" | tail -1)" "r4999"
check "index sees deletion after sort" \
	"$(q "SET enable_seqscan=off; SELECT count(*) FROM t_col WHERE id = 5000;" | tail -1)" "0"

# ---------------------------------------------------------------------------
# Idempotence: sorting an already-sorted table changes nothing observable.
# ---------------------------------------------------------------------------
echo "-- idempotent"
h1="$(pgc_set_hash "SELECT * FROM t_col")"
q "SELECT pgcolumnar.vacuum_sorted('t_col', 'k');" >/dev/null
h2="$(pgc_set_hash "SELECT * FROM t_col")"
check "idempotent sorted rewrite" "$h2" "$h1"

# ---------------------------------------------------------------------------
# Argument validation.
# ---------------------------------------------------------------------------
echo "-- argument validation"
expect_error "reject non-columnar table"   "SELECT pgcolumnar.vacuum_sorted('t_heap', 'k');"
expect_error "reject unknown column"        "SELECT pgcolumnar.vacuum_sorted('t_col', 'nope');"
expect_error "reject no sort columns"       "SELECT pgcolumnar.vacuum_sorted('t_col');"
q "DROP TABLE IF EXISTS t_js;" >/dev/null
q "CREATE TABLE t_js (a int, j json) USING pgcolumnar;" >/dev/null
q "INSERT INTO t_js SELECT g, '{}'::json FROM generate_series(1,10) g;" >/dev/null
expect_error "reject non-sortable type"     "SELECT pgcolumnar.vacuum_sorted('t_js', 'j');"

pgc_summary
