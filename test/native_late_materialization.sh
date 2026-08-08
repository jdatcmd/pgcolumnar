#!/usr/bin/env bash
#
# pgColumnar late materialization (#452 Phase 1a): a row the qual rejects must
# not have its payload columns materialized.
#
# Today the scan builds a complete tuple -- every projected column, every
# varlena copied into the row context -- and core's ExecScan applies the qual
# afterwards, so decode cost scales with rows SCANNED rather than rows EMITTED.
# Heap does the opposite: slot_deform_heap_tuple stops at the qual's attribute
# for a rejected row and pays the full deform only for survivors. Measured on
# ClickBench q24 (#445), that difference is 8.49x.
#
# The predicate here is deliberately one that CANNOT be pushed down (a leading
# wildcard LIKE builds no scan key, and no min/max can prune a substring match),
# because pruning is a different mechanism with its own suites. The premises
# below assert that nothing was pruned, so a green run cannot be pruning wearing
# this feature's name.
#
# Usage:  test/native_late_materialization.sh [PG_CONFIG]
# Written fresh for pgColumnar.

set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

ROWS=20480
NEEDLE=zqx

# One row group so nothing is pruned wholesale, and wide payload columns so
# materialization is the cost under test. Only row 7 in every 2048 carries the
# needle, so the qual rejects the overwhelming majority.
GEN="SELECT g AS id,
            CASE WHEN g % 2048 = 7 THEN 'aaa${NEEDLE}bbb' ELSE 'aaaaaabbb' END AS k,
            repeat(md5(g::text), 4) AS p1,
            repeat(md5((g+1)::text), 4) AS p2,
            repeat(md5((g+2)::text), 4) AS p3
     FROM generate_series(1, $ROWS) g"

psql_run "CREATE TABLE h (id int, k text, p1 text, p2 text, p3 text);"
psql_run "CREATE TABLE n (id int, k text, p1 text, p2 text, p3 text) USING pgcolumnar;"
psql_run "SELECT pgcolumnar.set_options('n', stripe_row_limit => 65536, chunk_group_row_limit => 1024);"
psql_run "INSERT INTO h $GEN;"
psql_run "INSERT INTO n $GEN;"
psql_run "ANALYZE h; ANALYZE n;"

Q="SELECT * FROM n WHERE k LIKE '%${NEEDLE}%'"
QH="SELECT * FROM h WHERE k LIKE '%${NEEDLE}%'"

# One EXPLAIN ANALYZE counter for a query.
counter() {  # counter <label> <query>
	env PATH="$PGC_BINDIR:$PATH" psql -h 127.0.0.1 -p "$PGC_PORT" -U postgres \
		-d "$PGC_DB" -At -c "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) $2" 2>/dev/null \
		| grep "$1" | grep -oE '[0-9]+' | head -1
}
scalar() {  # scalar <sql>
	env PATH="$PGC_BINDIR:$PATH" psql -h 127.0.0.1 -p "$PGC_PORT" -U postgres \
		-d "$PGC_DB" -At -c "$1" 2>/dev/null
}

# ---- premises, so a green run cannot mean something else --------------------

n_rows=$(scalar "SELECT count(*) FROM n;")
check_num "premise: the fixture loaded every row" "$n_rows" "$ROWS"

n_match=$(scalar "SELECT count(*) FROM n WHERE k LIKE '%${NEEDLE}%';")
h_match=$(scalar "SELECT count(*) FROM h WHERE k LIKE '%${NEEDLE}%';")
check_num "premise: the needle is selective, not most of the table" "$n_match" "10"
check_num "premise: heap agrees on how many rows match" "$h_match" "$n_match"

# The scalar columnar custom scan reports "Columnar Projected Columns"; no other
# node reports it. Without this the counters below would be read off a node that
# is not the one under test.
proj=$(counter "Columnar Projected Columns" "$Q")
check "premise: the query runs on the columnar custom scan" \
	"$([ -n "$proj" ] && echo yes || echo no)" "yes"

# A leading-wildcard LIKE must not prune. If it ever does, this suite is
# measuring pruning and its headline check means nothing.
removed=$(counter "Columnar Chunk Groups Removed by Filter" "$Q")
vskip=$(counter "Columnar Vectors Skipped" "$Q")
check_num "premise: nothing was pruned at the group level" "${removed:-0}" "0"
check_num "premise: nothing was pruned at the vector level" "${vskip:-0}" "0"

# ---- the feature -----------------------------------------------------------

early=$(counter "Columnar Rows Filtered Before Materialization" "$Q")
check_num "a rejected row does not materialize its payload columns" \
	"${early:-0}" "$((ROWS - n_match))"

# ---- the executor's own counter must keep counting -------------------------
#
# ExecScan increments nfiltered1 for every tuple ITS qual rejects, and that is
# what EXPLAIN prints as "Rows Removed by Filter". Filtering inside the scan
# means ExecScan never sees a rejected row, so the line silently went to 0 on
# every columnar scan with a qual -- the plan still looked right and a counter
# other suites depend on had stopped counting.
#
# The five-major gate caught it (pushdown_report and analyze_stats, all five
# majors); this pins it here, next to the feature that broke it.
removed=$(counter "Rows Removed by Filter" "$Q")
check_num "the executor still reports the rows the scan filtered" \
	"${removed:-0}" "$((ROWS - n_match))"

# ---- and it may not change a single answer ---------------------------------
#
# The heap mirror is the oracle. Late materialization is a pure optimization:
# every row heap returns, columnar must return, byte for byte.
nh=$(scalar "SELECT md5(string_agg(t::text, '|' ORDER BY id)) FROM ($QH) t;")
nn=$(scalar "SELECT md5(string_agg(t::text, '|' ORDER BY id)) FROM ($Q) t;")
check_text "the rows are identical to the heap mirror" "$nn" "$nh"

# ---- the removal proof, run by the suite itself ----------------------------
#
# With the feature off the counter must be 0 AND the answers must be unchanged.
# Asserting only the first would let a broken implementation that silently
# emitted nothing pass the off-arm.
psql_run "SET pgcolumnar.enable_late_materialization = off;"
off_early=$(env PATH="$PGC_BINDIR:$PATH" psql -h 127.0.0.1 -p "$PGC_PORT" -U postgres \
	-d "$PGC_DB" -At \
	-c "SET pgcolumnar.enable_late_materialization = off;" \
	-c "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) $Q" 2>/dev/null \
	| grep "Columnar Rows Filtered Before Materialization" | grep -oE '[0-9]+' | head -1)
check_num "with the feature off nothing is filtered early" "${off_early:-0}" "0"

nn_off=$(env PATH="$PGC_BINDIR:$PATH" psql -h 127.0.0.1 -p "$PGC_PORT" -U postgres \
	-d "$PGC_DB" -At \
	-c "SET pgcolumnar.enable_late_materialization = off;" \
	-c "SELECT md5(string_agg(t::text, '|' ORDER BY id)) FROM ($Q) t;" 2>/dev/null \
	| tail -1)
check_text "and the answers are the same with it off" "$nn_off" "$nh"

# ---- a volatile qual must refuse the path entirely -------------------------
#
# ExecScan re-applies the node's qual to every row this returns, so a row that
# survives has its qual evaluated twice. That is invisible for an immutable
# expression and wrong for a volatile one, which is why Begin refuses the path.
#
# This check exists because nothing else reaches that guard: every other query in
# this suite is immutable, so deleting contain_volatile_functions() would leave
# the whole suite green. Same shape as the collation and divisor guards found
# unreachable elsewhere today.
#
# random() < 2 is always true, so the rows are the ones the LIKE selects and the
# heap mirror is still the oracle. It cannot be constant-folded away: volatile is
# precisely what stops the planner doing that, which is what makes it a fixture.
QV="SELECT * FROM n WHERE k LIKE '%${NEEDLE}%' AND random() < 2"
QVH="SELECT * FROM h WHERE k LIKE '%${NEEDLE}%' AND random() < 2"

vol_early=$(counter "Columnar Rows Filtered Before Materialization" "$QV")
check_num "a volatile qual is refused the two-pass path" "${vol_early:-0}" "0"

# And refusing it must not change the answer either.
vh=$(scalar "SELECT md5(string_agg(t::text, '|' ORDER BY id)) FROM ($QVH) t;")
vn=$(scalar "SELECT md5(string_agg(t::text, '|' ORDER BY id)) FROM ($QV) t;")
check_text "and the volatile query still matches the heap mirror" "$vn" "$vh"

# The premise that makes the check above mean something: the SAME query without
# the volatile term does take the path. Without this, "0" would be satisfied by a
# fixture that never qualified for late materialization at all.
check_num "premise: the same shape without the volatile term does use it" \
	"$(counter "Columnar Rows Filtered Before Materialization" "$Q")" \
	"$((ROWS - n_match))"

pgc_summary
