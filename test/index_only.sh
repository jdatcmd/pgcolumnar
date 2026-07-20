#!/usr/bin/env bash
#
# pgColumnar index-only-scan coverage (gap 28 direction 1).
#
# Phase 1 (this file for now): prove the load-bearing assumption that a
# visibility-map bit written by pgColumnar's custom VM writer on a columnar
# relation -- whose TIDs are synthetic and have no heap page -- is read back by
# the backend's own visibilitymap_get_status (the exact call the index-only-scan
# executor makes). columnar.vm_selftest sets a bit for a synthetic block and
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
	r="$(q "SELECT columnar.vm_selftest('t_col', $blk);")"
	check "vm bit round-trips on columnar rel (block $blk)" "$r" "t"
done

# A second call on an already-set block returns false (bit was set before), which
# confirms the write is persistent and the reader sees prior state.
first="$(q "SELECT columnar.vm_selftest('t_col', 7);")"
second="$(q "SELECT columnar.vm_selftest('t_col', 7);")"
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
psql_run "CREATE TABLE iv (id int, v int) USING columnar;"
psql_run "INSERT INTO iv SELECT g, g * 2 FROM generate_series(1, 50000) g;"
check "iv row count" "$(q "SELECT count(*) FROM iv;")" "50000"

# Freshly written groups are never all-visible until a vacuum runs.
check "before vacuum: block 50 not all-visible" "$(q "SELECT columnar.vm_is_visible('iv', 50);")" "f"

# Plain VACUUM (ShareUpdateExclusiveLock) drives columnar_relation_vacuum, which
# marks the all-visible groups.
psql_run "VACUUM iv;"
check "after vacuum: block 10 all-visible"  "$(q "SELECT columnar.vm_is_visible('iv', 10);")"  "t"
check "after vacuum: block 50 all-visible"  "$(q "SELECT columnar.vm_is_visible('iv', 50);")"  "t"
check "after vacuum: block 120 all-visible" "$(q "SELECT columnar.vm_is_visible('iv', 120);")" "t"

# Delete the whole of group 1 (rows ~10000-20000). Clear-on-write clears those
# blocks immediately; a re-vacuum must NOT re-mark them (the group has deletes),
# while clean groups stay all-visible.
psql_run "DELETE FROM iv WHERE id BETWEEN 10001 AND 20000;"
check "after delete: block 50 cleared by write" "$(q "SELECT columnar.vm_is_visible('iv', 50);")" "f"
psql_run "VACUUM iv;"
check "after delete+vacuum: deleted block 50 not all-visible" "$(q "SELECT columnar.vm_is_visible('iv', 50);")" "f"
check "after delete+vacuum: clean block 10 still all-visible"  "$(q "SELECT columnar.vm_is_visible('iv', 10);")"  "t"
check "after delete+vacuum: clean block 120 still all-visible" "$(q "SELECT columnar.vm_is_visible('iv', 120);")" "t"

# Reads still return correct rows (the VM state must never change query results).
dh="$(pgc_set_hash "SELECT id, v FROM iv")"
psql_run "CREATE TABLE iv_heap (id int, v int) USING heap;"
psql_run "INSERT INTO iv_heap SELECT g, g*2 FROM generate_series(1,50000) g WHERE g NOT BETWEEN 10001 AND 20000;"
oh="$(pgc_set_hash "SELECT id, v FROM iv_heap")"
check "iv contents match heap oracle after vacuum/delete" "$dh" "$oh"

pgc_summary
