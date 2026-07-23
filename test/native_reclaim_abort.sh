#!/usr/bin/env bash
#
# pgColumnar reclaim: aborted compaction must not corrupt (Phase F, block-1 free
# list). The free list is non-transactional (block 1), but a compaction's
# row_group deletes are transactional. If a compaction aborts, the groups come
# back live while the recorded free entries persist, so those entries now overlap
# live data. ColumnarReconcileFreeList purges such stale entries at the start of
# every reuse, before ColumnarAllocateFreeSpace can hand one out on top of a live
# group. This suite aborts a compaction mid-flight (BEGIN; compact_rewrite;
# ROLLBACK), confirms the stale entries persist, then runs a real compaction and
# asserts data is intact against a heap mirror (the assert-build no-overlap
# validator also runs inside the compaction, so any double-allocation would fire).
#
# Usage:  test/native_reclaim_abort.sh [PG_CONFIG]
# Written fresh for pgColumnar.

set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

raw() { env PATH="$PGC_BINDIR:$PATH" psql -h 127.0.0.1 -p "$PGC_PORT" -U postgres -d "$PGC_DB" -Atq -c "$1" 2>&1; }
hash_n() { pgc_set_hash 'SELECT id, v FROM n'; }
hash_h() { pgc_set_hash 'SELECT id, v FROM h'; }
freecount() { q "SELECT count(*) FROM pgcolumnar.free_list('n');"; }

psql_run "CREATE TABLE h (id int, v text);"
psql_run "CREATE TABLE n (id int, v text) USING pgcolumnar;"
psql_run "SELECT pgcolumnar.alter_columnar_table_set('n', stripe_row_limit => 1000, chunk_group_row_limit => 1000);"
psql_run "INSERT INTO h SELECT g, md5(g::text) FROM generate_series(1, 8000) g;"
psql_run "INSERT INTO n SELECT g, md5(g::text) FROM generate_series(1, 8000) g;"
# partially delete each group so every group is a rewrite candidate
psql_run "DELETE FROM h WHERE id % 3 = 0;"
psql_run "DELETE FROM n WHERE id % 3 = 0;"
check "parity before" "$(hash_n)" "$(hash_h)"

# abort a compaction mid-flight: the row_group deletes roll back, but the block-1
# free entries recorded during it persist (non-transactional).
raw "BEGIN; SELECT pgcolumnar.compact_rewrite('n', 0.1); ROLLBACK;" >/dev/null
check "parity after aborted compaction (rollback restored the groups)" \
	"$(hash_n)" "$(hash_h)"
stale="$(freecount)"
echo "  (stale free entries left by the aborted compaction: $stale)"
check "aborted compaction did leave stale free entries" \
	"$([ "$stale" -gt 0 ] && echo yes || echo no)" "yes"

# a real compaction reconciles (purges the stale, overlapping entries) before
# reusing, so it cannot double-allocate over a live group.
psql_run "SELECT pgcolumnar.compact_rewrite('n', 0.0);"
check "data intact after recompaction (no double-allocation)" \
	"$(hash_n)" "$(hash_h)"
check "row count intact" "$(q 'SELECT count(*) FROM n;')" "$(q 'SELECT count(*) FROM h;')"

# reuse keeps working across further delete+compact cycles, still correct.
for r in 1 2 3; do
	psql_run "DELETE FROM h WHERE id % 5 = $r;"
	psql_run "DELETE FROM n WHERE id % 5 = $r;"
	psql_run "SELECT pgcolumnar.compact_rewrite('n', 0.0);"
	check "parity after reuse cycle $r" "$(hash_n)" "$(hash_h)"
done

pgc_summary
