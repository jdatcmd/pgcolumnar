#!/usr/bin/env bash
#
# pgColumnar online-rewrite concurrency stress (Phase F3b). While several backends
# run INSERT/DELETE/UPDATE over disjoint id ranges, a compactor loop concurrently
# runs pgcolumnar.compact_rewrite (online rewrite of partially-deleted groups) and
# pgcolumnar.compact (retire fully-dead groups). A DELETE/UPDATE that races a
# rewrite of its group is aborted with a serialization failure by the conflict
# protocol; the worker retries. Because the id ranges are disjoint the final
# logical state is deterministic, so t_col must end equal to the heap oracle --
# proving the online rewrite never loses or resurrects a row under concurrent
# writes. This is the decisive correctness test for F3b's ShareUpdateExclusiveLock
# path.
#
# Usage:  test/native_rewrite_conc.sh [PG_CONFIG]
# Written fresh for pgColumnar.

set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

make_pair "id int, cat int, v bigint, txt text"
q "SELECT pgcolumnar.alter_columnar_table_set('t_col', chunk_group_row_limit => 500, stripe_row_limit => 1000);" >/dev/null

WORKERS=6
PER=3000

# Run one autocommit statement, retrying on the serialization failures the rewrite
# conflict protocol raises (each failed attempt rolls back fully, so a retry
# reapplies exactly once). Returns non-zero only if it never succeeds.
runsql() {
	local sql="$1" errf="$2" t=0
	until env PATH="$PGC_BINDIR:$PATH" psql -h 127.0.0.1 -p "$PGC_PORT" -U postgres \
			-d "$PGC_DB" -v ON_ERROR_STOP=1 -q -c "$sql" >/dev/null 2>>"$errf"; do
		t=$((t + 1))
		[ "$t" -ge 200 ] && return 1
	done
	return 0
}

# Compactor loop: hammer the online rewrite + fully-dead retirement concurrently.
STOP="$PGC_WORKDIR/stop"
rm -f "$STOP"
(
	while [ ! -f "$STOP" ]; do
		env PATH="$PGC_BINDIR:$PATH" psql -h 127.0.0.1 -p "$PGC_PORT" -U postgres \
			-d "$PGC_DB" -q -c "SELECT pgcolumnar.compact_rewrite('t_col', 0.0);" \
			>/dev/null 2>>"$PGC_WORKDIR/compactor.err" || true
		env PATH="$PGC_BINDIR:$PATH" psql -h 127.0.0.1 -p "$PGC_PORT" -U postgres \
			-d "$PGC_DB" -q -c "SELECT pgcolumnar.compact('t_col');" \
			>/dev/null 2>>"$PGC_WORKDIR/compactor.err" || true
	done
) &
comp_pid=$!

echo "-- launching $WORKERS workers against a concurrent compactor"
pids=()
for w in $(seq 0 $((WORKERS - 1))); do
	lo=$((w * PER + 1))
	hi=$(((w + 1) * PER))
	errf="$PGC_WORKDIR/w.$w.err"
	(
		# inserts never conflict with the rewrite (only delete/update do)
		runsql "INSERT INTO t_heap SELECT g, g%5, g*3, md5(g::text) FROM generate_series($lo,$hi) g;" "$errf" || exit 1
		runsql "INSERT INTO t_col  SELECT g, g%5, g*3, md5(g::text) FROM generate_series($lo,$hi) g;" "$errf" || exit 1
		runsql "DELETE FROM t_heap WHERE id BETWEEN $lo AND $hi AND id % 7 = 0;" "$errf" || exit 1
		runsql "DELETE FROM t_col  WHERE id BETWEEN $lo AND $hi AND id % 7 = 0;" "$errf" || exit 1
		runsql "UPDATE t_heap SET v = v + 1 WHERE id BETWEEN $lo AND $hi AND id % 5 = 0;" "$errf" || exit 1
		runsql "UPDATE t_col  SET v = v + 1 WHERE id BETWEEN $lo AND $hi AND id % 5 = 0;" "$errf" || exit 1
	) &
	pids+=($!)
done

fail=0
for pid in "${pids[@]}"; do
	wait "$pid" || fail=1
done
touch "$STOP"
wait "$comp_pid" 2>/dev/null || true
check "all workers succeeded (retried past serialization failures)" "$fail" "0"

# A final settling pass, then the columnar table must equal the heap oracle.
q "SELECT pgcolumnar.compact_rewrite('t_col', 0.0);" >/dev/null
q "SELECT pgcolumnar.compact('t_col');" >/dev/null

diff_query "conc-rewrite whole-row" "SELECT * FROM %T"
diff_query "conc-rewrite count/sum" "SELECT count(*), sum(v), count(distinct cat) FROM %T"
diff_query "conc-rewrite range"     "SELECT count(*) FROM %T WHERE id BETWEEN 4000 AND 12000"
check "row count sane" "$([ "$(q 'SELECT count(*) FROM t_col;')" -gt 0 ] && echo ok)" "ok"

pgc_summary
