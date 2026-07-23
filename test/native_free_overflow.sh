#!/usr/bin/env bash
#
# pgColumnar reclaim free-list overflow (Phase F, block-1 free list). The free
# list lives in one page (block 1), so it holds a bounded number of entries
# (COLUMNAR_FREELIST_MAX, about 255). When it is full, a further freed range is
# simply not recorded (reclaimed later by a full rewrite): graceful degradation,
# never corruption. This OPT-IN suite (not in the default matrix -- it needs many
# groups) drives the list past capacity with many NON-adjacent, non-coalescible
# retirements and asserts: data stays correct against a heap mirror, the free-list
# entry count is capped (never exceeds the page), and the assert-build no-overlap
# validator (run inside compact) stays green -- i.e. no double-allocation.
#
# Usage:  test/native_free_overflow.sh [PG_CONFIG]
# Written fresh for pgColumnar.

set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

# 520 groups of 1000 rows. Deleting every OTHER whole group leaves 260 retired,
# non-adjacent ranges (live groups sit between them, so they cannot coalesce),
# which is past the ~255-entry page capacity.
N=520000
psql_run "CREATE TABLE h (id int, v text);"
psql_run "CREATE TABLE n (id int, v text) USING pgcolumnar;"
psql_run "SELECT pgcolumnar.alter_columnar_table_set('n', stripe_row_limit => 1000, chunk_group_row_limit => 1000);"
psql_run "INSERT INTO h SELECT g, md5(g::text) FROM generate_series(1, $N) g;"
psql_run "INSERT INTO n SELECT g, md5(g::text) FROM generate_series(1, $N) g;"

hash_n() { pgc_set_hash 'SELECT id, v FROM n'; }
hash_h() { pgc_set_hash 'SELECT id, v FROM h'; }
freecount() { q "SELECT count(*) FROM pgcolumnar.free_list('n');"; }
maxentries() { q "SELECT (current_setting('block_size')::int - 24) / 32;"; }

check "load parity" "$(hash_n)" "$(hash_h)"

# fully delete every other group (ids in even-numbered 1000-blocks), then compact.
psql_run "DELETE FROM h WHERE (id - 1) / 1000 % 2 = 0;"
psql_run "DELETE FROM n WHERE (id - 1) / 1000 % 2 = 0;"
psql_run "SELECT pgcolumnar.compact('n');"

fc="$(freecount)"
cap="$(maxentries)"
echo "  (free-list entries=$fc  page capacity=$cap)"
check "data correct after overflowing compaction" "$(hash_n)" "$(hash_h)"
check "free-list entry count never exceeds the page" \
	"$([ "$fc" -le "$cap" ] && echo yes || echo no)" "yes"
check "free list actually filled (overflow exercised)" \
	"$([ "$fc" -ge "$((cap - 5))" ] && echo yes || echo no)" "yes"

# still fully usable: a full recluster rebuilds every group (rebuilding the free
# list from scratch), the no-overlap validator runs inside it, and data holds.
psql_run "SELECT pgcolumnar.recluster('n', 'id');"
check "data correct after recluster over overflowed list" "$(hash_n)" "$(hash_h)"
check "row count intact" "$(q 'SELECT count(*) FROM n;')" "$(q 'SELECT count(*) FROM h;')"

pgc_summary
