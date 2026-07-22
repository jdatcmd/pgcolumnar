#!/usr/bin/env bash
#
# pgColumnar index-only-scan coverage (gap 28 direction 1).
#
# Phase 1 (this file for now): prove the load-bearing assumption that a
# visibility-map bit written by pgColumnar's custom VM writer on a columnar
# relation -- whose TIDs are synthetic and have no heap page -- is read back by
# the backend's own visibilitymap_get_status (the exact call the index-only-scan
# executor makes). pgcolumnar.vm_selftest sets a bit for a synthetic block and
# reads it back; it must return true. Later phases add the differential MVCC
# coverage for actual index-only scans.
#
# Usage:  test/index_only.sh [PG_CONFIG]
#
# Written fresh for pgColumnar; it does not reuse any upstream test file.

set -uo pipefail

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

echo "-- VM fork write/read round-trip on a columnar relation (phase 1)"
make_pair "id int, v int"
load_pair "SELECT g, g * 2 FROM generate_series(1, 50000) g"

# sanity: the table is populated (guards against a dead cluster passing trivially)
check "rows loaded" "$(q "SELECT count(*) FROM t_col;")" "50000"

# A synthetic block covers MaxHeapTuplesPerPage (~291) row numbers, so 50000
# rows span well over 100 blocks. Set + read back the all-visible bit for a few
# blocks across that range; each must round-trip (false before, true after).
for blk in 0 3 50 171; do
	r="$(q "SELECT pgcolumnar.vm_selftest('t_col', $blk);")"
	check "vm bit round-trips on columnar rel (block $blk)" "$r" "t"
done

# A second call on an already-set block returns false (bit was set before), which
# confirms the write is persistent and the reader sees prior state.
first="$(q "SELECT pgcolumnar.vm_selftest('t_col', 7);")"
second="$(q "SELECT pgcolumnar.vm_selftest('t_col', 7);")"
check "first set on fresh block succeeds" "$first" "t"
check "second set on same block sees prior bit" "$second" "f"

# ---------------------------------------------------------------------------
# Phase 3: lazy vacuum (relation_vacuum, ShareUpdateExclusiveLock) sets the
# all-visible bit for all-visible chunk groups, and a delete leaves its block
# not-all-visible. Blocks: a synthetic block covers ~291 row numbers; a chunk
# group is 10000 rows. Block 10 (~row 2900) is deep in group 0; block 50
# (~row 14550) is in group 1; block 120 (~row 34900) is in group 3 -- all far
# from group boundaries, so the checks are robust to a 0/1-based row-number
# offset.
# ---------------------------------------------------------------------------
echo "-- phase 3: lazy vacuum sets all-visible bits"
psql_run "CREATE TABLE iv (id int, v int) USING pgcolumnar;"
psql_run "INSERT INTO iv SELECT g, g * 2 FROM generate_series(1, 50000) g;"
check "iv row count" "$(q "SELECT count(*) FROM iv;")" "50000"

# Freshly written groups are never all-visible until a vacuum runs.
check "before vacuum: block 50 not all-visible" "$(q "SELECT pgcolumnar.vm_is_visible('iv', 50);")" "f"

# Plain VACUUM (ShareUpdateExclusiveLock) drives columnar_relation_vacuum, which
# marks the all-visible groups.
psql_run "VACUUM iv;"
check "after vacuum: block 10 all-visible"  "$(q "SELECT pgcolumnar.vm_is_visible('iv', 10);")"  "t"
check "after vacuum: block 50 all-visible"  "$(q "SELECT pgcolumnar.vm_is_visible('iv', 50);")"  "t"
check "after vacuum: block 120 all-visible" "$(q "SELECT pgcolumnar.vm_is_visible('iv', 120);")" "t"

# Delete the whole of group 1 (rows ~10000-20000). Clear-on-write clears those
# blocks immediately; a re-vacuum must NOT re-mark them (the group has deletes),
# while clean groups stay all-visible.
psql_run "DELETE FROM iv WHERE id BETWEEN 10001 AND 20000;"
check "after delete: block 50 cleared by write" "$(q "SELECT pgcolumnar.vm_is_visible('iv', 50);")" "f"
psql_run "VACUUM iv;"
check "after delete+vacuum: deleted block 50 not all-visible" "$(q "SELECT pgcolumnar.vm_is_visible('iv', 50);")" "f"
check "after delete+vacuum: clean block 10 still all-visible"  "$(q "SELECT pgcolumnar.vm_is_visible('iv', 10);")"  "t"
check "after delete+vacuum: clean block 120 still all-visible" "$(q "SELECT pgcolumnar.vm_is_visible('iv', 120);")" "t"

# Reads still return correct rows (the VM state must never change query results).
dh="$(pgc_set_hash "SELECT id, v FROM iv")"
psql_run "CREATE TABLE iv_heap (id int, v int) USING heap;"
psql_run "INSERT INTO iv_heap SELECT g, g*2 FROM generate_series(1,50000) g WHERE g NOT BETWEEN 10001 AND 20000;"
oh="$(pgc_set_hash "SELECT id, v FROM iv_heap")"
check "iv contents match heap oracle after vacuum/delete" "$dh" "$oh"

# ---------------------------------------------------------------------------
# Phase 4: the planner builds a real index-only scan for a columnar table when
# pgcolumnar.enable_index_only_scan is on, the executor skips the fetch for
# all-visible blocks (Heap Fetches: 0), and results still match the heap oracle.
#
# The GUC (and planner knobs that steer the plan to the index) are set at the
# database level so the fresh connection opened by q/pgc_set_hash inherits them;
# a per-session SET would not survive across those helpers.
# ---------------------------------------------------------------------------
echo "-- phase 4: index-only scan chosen + served from the VM fork"
psql_run "CREATE TABLE ios (id int, v int) USING pgcolumnar;"
psql_run "INSERT INTO ios SELECT g, g*3 FROM generate_series(1,50000) g;"
psql_run "CREATE INDEX ios_id ON ios (id);"
psql_run "CREATE TABLE ios_h (id int, v int) USING heap;"
psql_run "INSERT INTO ios_h SELECT g, g*3 FROM generate_series(1,50000) g;"
psql_run "CREATE INDEX ios_h_id ON ios_h (id);"

# Baseline: with the GUC explicitly off the planner must NOT build an index-only
# scan even with seqscan disabled -- canreturn is cleared, so it falls back.
planoff="$(q "SET pgcolumnar.enable_index_only_scan=off; SET enable_seqscan=off; EXPLAIN (COSTS OFF) SELECT id FROM ios WHERE id BETWEEN 100 AND 2000;")"
check "GUC off: no index-only scan" "$(printf '%s' "$planoff" | grep -c 'Index Only Scan')" "0"

# Enable index-only scans and steer the planner to the index for the rest.
psql_run "ALTER DATABASE $PGC_DB SET pgcolumnar.enable_index_only_scan = on;"
psql_run "ALTER DATABASE $PGC_DB SET pgcolumnar.enable_custom_scan = off;"
psql_run "ALTER DATABASE $PGC_DB SET enable_seqscan = off;"
psql_run "ALTER DATABASE $PGC_DB SET enable_bitmapscan = off;"

psql_run "VACUUM ios;"   # mark the all-visible groups in the VM fork

# Use an interior id range (1000..5000): a synthetic block spans ~291 row
# numbers, so this range maps to blocks that sit wholly inside chunk group 0.
# The all-visible bit is only set for blocks fully within an all-visible group
# (the boundary block that would straddle the group edge or the unused row-0
# slot is conservatively left unmarked and falls back to the fetch -- correct,
# just not fetch-free), so an interior range is where "zero heap fetches" holds.
planon="$(q "EXPLAIN (COSTS OFF) SELECT id FROM ios WHERE id BETWEEN 1000 AND 5000;")"
check "GUC on: index-only scan chosen" "$(printf '%s' "$planon" | grep -c 'Index Only Scan')" "1"

# All-visible interior blocks after vacuum -> the executor skips every fetch.
eaav="$(q "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT id FROM ios WHERE id BETWEEN 1000 AND 5000;")"
check "all-visible interior: zero heap fetches" \
	"$(printf '%s' "$eaav" | grep -oE 'Heap Fetches: [0-9]+' | grep -oE '[0-9]+' | head -1)" "0"

# Correctness of the index-only path against the heap oracle.
check "index-only results match heap oracle (all-visible)" \
	"$(pgc_set_hash "SELECT id FROM ios WHERE id BETWEEN 1000 AND 5000")" \
	"$(pgc_set_hash "SELECT id FROM ios_h WHERE id BETWEEN 1000 AND 5000")"

# A delete clears the VM bit for the affected blocks (clear-on-write); the scan
# must fall back to the fetch for those and never return a deleted row.
psql_run "DELETE FROM ios   WHERE id BETWEEN 2000 AND 2200;"
psql_run "DELETE FROM ios_h WHERE id BETWEEN 2000 AND 2200;"
check "index-only results match heap oracle after delete (fallback)" \
	"$(pgc_set_hash "SELECT id FROM ios WHERE id BETWEEN 1000 AND 5000")" \
	"$(pgc_set_hash "SELECT id FROM ios_h WHERE id BETWEEN 1000 AND 5000")"
eadel="$(q "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT id FROM ios WHERE id BETWEEN 1000 AND 5000;")"
hf="$(printf '%s' "$eadel" | grep -oE 'Heap Fetches: [0-9]+' | grep -oE '[0-9]+' | head -1)"
check "after delete: some heap fetches occur" "$([ "${hf:-0}" -gt 0 ] && echo yes || echo no)" "yes"

# ---------------------------------------------------------------------------
# Phase 5: MVCC correctness of the index-only path under a concurrent writer.
# An old REPEATABLE READ snapshot must never see rows committed after it, even
# when a later VACUUM has run -- the VM all-visible horizon accounts for the
# open snapshot, so the new rows' block is left not-all-visible and the scan
# falls back to the snapshot-checked fetch (which does not see them either).
#
# Session A is a persistent psql fed through a FIFO; a sentinel token in its
# output file lets us wait for each step without racing the concurrent writer.
# ---------------------------------------------------------------------------
echo "-- phase 5: old snapshot never sees post-snapshot rows via index-only scan"
psql_run "CREATE TABLE mv (id int, v int) USING pgcolumnar;"
psql_run "INSERT INTO mv SELECT g, g*3 FROM generate_series(1,10000) g;"   # batch 1
psql_run "CREATE INDEX mv_id ON mv (id);"
psql_run "VACUUM mv;"   # batch 1 marked all-visible

A_IN="$PGC_WORKDIR/sessA.in"; A_OUT="$PGC_WORKDIR/sessA.out"
mkfifo "$A_IN"; : > "$A_OUT"
env PATH="$PGC_BINDIR:$PATH" psql -h 127.0.0.1 -p "$PGC_PORT" -U postgres \
	-d "$PGC_DB" -At -f "$A_IN" >> "$A_OUT" 2>&1 &
A_PID=$!
exec 8> "$A_IN"                        # hold the write end open so psql waits
a_send() { printf '%s\n' "$*" >&8; }
a_wait() {                             # poll A_OUT for a token (~20s budget)
	local token="$1" i
	for i in $(seq 1 200); do
		grep -q "$token" "$A_OUT" && return 0
		sleep 0.1
	done
	return 1
}

# A opens a REPEATABLE READ snapshot (materialized by a first read) that sees
# only batch 1. The GUC/planner knobs are inherited from the ALTER DATABASE above.
a_send "BEGIN ISOLATION LEVEL REPEATABLE READ;"
a_send "SELECT 'A_SNAP_' || count(id) FROM mv WHERE id BETWEEN 1 AND 20000;"
if a_wait "A_SNAP_"; then
	snap="$(grep -o 'A_SNAP_[0-9]*' "$A_OUT" | tail -1 | sed 's/A_SNAP_//')"
	check "session A baseline sees batch 1 only" "$snap" "10000"

	# Concurrent writer commits batch 2, then vacuums. The vacuum must NOT mark
	# batch 2 all-visible because A's snapshot pins the removable horizon.
	psql_run "INSERT INTO mv SELECT g, g*3 FROM generate_series(10001,20000) g;"
	psql_run "VACUUM mv;"

	# A's index-only scan under its old snapshot must still see batch 1 only.
	a_send "SELECT 'A_SEE_' || count(id) FROM mv WHERE id BETWEEN 1 AND 20000;"
	a_send "EXPLAIN (COSTS OFF) SELECT id FROM mv WHERE id BETWEEN 1 AND 20000;"
	a_send "SELECT 'A_PLAN_DONE';"
	if a_wait "A_PLAN_DONE"; then
		see="$(grep -o 'A_SEE_[0-9]*' "$A_OUT" | tail -1 | sed 's/A_SEE_//')"
		check "old snapshot IOS does not see post-snapshot rows" "$see" "10000"
		check "session A used an index-only scan" \
			"$(grep -c 'Index Only Scan' "$A_OUT")" "1"
	else
		check "session A responded to post-commit query" "timeout" "ok"
	fi
	a_send "COMMIT;"
else
	check "session A opened its snapshot" "timeout" "ok"
fi

# A now sees both batches (new snapshot); after a vacuum it also matches the
# heap oracle, confirming batch 2 is fully readable once the snapshot advances.
a_send "SELECT 'A_FINAL_' || count(id) FROM mv WHERE id BETWEEN 1 AND 20000;"
a_wait "A_FINAL_" || true
final="$(grep -o 'A_FINAL_[0-9]*' "$A_OUT" | tail -1 | sed 's/A_FINAL_//')"
check "new snapshot sees both batches" "$final" "20000"

exec 8>&-                              # close write end -> psql gets EOF and exits
wait "$A_PID" 2>/dev/null || true

pgc_summary
