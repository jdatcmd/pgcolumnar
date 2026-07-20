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

pgc_summary
