#!/usr/bin/env bash
#
# pgColumnar physical reclaim under fragmentation (Phase F split/coalesce).
#
# Runs an identical retire+recluster workload twice -- once with
# pgcolumnar.reclaim_coalesce on (split oversized freed ranges on reuse, coalesce
# adjacent same-transaction frees) and once off (whole-range reuse) -- and
# asserts:
#   * the live set matches a heap mirror in BOTH modes (correctness);
#   * coalescing actually merges: fully deleting a large CONTIGUOUS block of
#     groups and compacting frees many small adjacent byte ranges, and with the
#     GUC on they collapse to FEWER free-list entries than with it off (direct,
#     deterministic evidence the coalesce path runs); and
#   * coalesce is never worse than whole-range reuse for the final file size.
# The assert-build no-overlap validator (ColumnarCheckFreeSpaceNoOverlap) runs at
# the end of every compaction here, so this also exercises the tiling invariant
# on variable-size ranges.
#
# Usage:  test/native_reclaim_frag.sh [PG_CONFIG]
# Written fresh for pgColumnar.

set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

# group k (= id/1000) gets width 10..250 bytes, so byte sizes vary widely.
GEN="SELECT g AS id, repeat('ab', 5 + ((g / 1000) % 16) * 8) AS payload
  FROM generate_series(1, 30000) g"

freerows() { q "SELECT count(*) FROM pgcolumnar.free_list('n') WHERE storage_id = pgcolumnar.get_storage_id('n');"; }

run() {			# $1 = on|off ; echoes "<free_rows_after_compact>|<final_size>|<ok|MISMATCH>"
	{
		psql_run "DROP TABLE IF EXISTS n;"
		psql_run "DROP TABLE IF EXISTS h;"
		psql_run "CREATE TABLE h (id int, payload text);"
		psql_run "CREATE TABLE n (id int, payload text) USING pgcolumnar;"
		# 30 groups of 1000 rows so a deleted block frees many adjacent ranges.
		psql_run "SELECT pgcolumnar.alter_columnar_table_set('n', stripe_row_limit => 1000, chunk_group_row_limit => 1000);"
		psql_run "SET pgcolumnar.reclaim_coalesce = $1; INSERT INTO h $GEN;"
		psql_run "SET pgcolumnar.reclaim_coalesce = $1; INSERT INTO n $GEN;"
		# fully delete a large CONTIGUOUS block of groups, then compact: many small
		# adjacent byte ranges are freed in one transaction.
		psql_run "SET pgcolumnar.reclaim_coalesce = $1; DELETE FROM h WHERE id BETWEEN 6001 AND 24000;"
		psql_run "SET pgcolumnar.reclaim_coalesce = $1; DELETE FROM n WHERE id BETWEEN 6001 AND 24000;"
		psql_run "SET pgcolumnar.reclaim_coalesce = $1; SELECT pgcolumnar.compact('n');"
	} >/dev/null 2>&1

	# free-list entries right after the compaction, before anything consumes them.
	local fr size ph hh
	fr="$(freerows)"

	{
		# force large output groups and recluster: reuses the freed space.
		psql_run "SET pgcolumnar.reclaim_coalesce = $1; SELECT pgcolumnar.alter_columnar_table_set('n', stripe_row_limit => 20000, chunk_group_row_limit => 20000);"
		psql_run "SET pgcolumnar.reclaim_coalesce = $1; SELECT pgcolumnar.recluster('n', 'id');"
	} >/dev/null 2>&1

	size="$(q 'SELECT pg_relation_size('"'"'n'"'"');')"
	ph="$(pgc_set_hash 'SELECT id, payload FROM n')"
	hh="$(pgc_set_hash 'SELECT id, payload FROM h')"
	echo "${fr}|${size}|$([ "$ph" = "$hh" ] && echo ok || echo MISMATCH)"
}

on="$(run on)";  off="$(run off)"
on_fr="${on%%|*}";   on_rest="${on#*|}";   on_size="${on_rest%%|*}";   on_par="${on##*|}"
off_fr="${off%%|*}"; off_rest="${off#*|}"; off_size="${off_rest%%|*}"; off_par="${off##*|}"

check "coalesce on: parity with heap"  "$on_par"  "ok"
check "coalesce off: parity with heap" "$off_par" "ok"
echo "  (free-list entries after compact: coalesce-on=$on_fr coalesce-off=$off_fr)"
echo "  (final file size: coalesce-on=$on_size coalesce-off=$off_size)"
check "coalesce merges adjacent frees (fewer free-list entries)" \
	"$([ "$on_fr" -lt "$off_fr" ] && echo yes || echo no)" "yes"
check "coalesce never larger than whole-range reuse" \
	"$([ "$on_size" -le "$off_size" ] && echo yes || echo no)" "yes"

pgc_summary
