#!/usr/bin/env bash
#
# pgColumnar reclaim reconciliation (Phase F). The free_space catalog is
# transactional, so normal retirement is atomic. The one seam is physical
# end-truncation: a crash in its narrow window can leave a free_space row that
# overlaps a live group (the highwater was lowered but the row's delete rolled
# back, and a later insert placed a live group there). ColumnarReconcileFreeList
# runs at the start of every reuse op (compact_rewrite, recluster) and drops any
# free_space row overlapping a live row-group footprint, so it cannot be handed
# out on top of a live group.
#
# This suite synthesizes that post-crash state directly: it INSERTs a stale
# free_space row over a live group's byte range, then runs compact_rewrite and
# asserts the reconcile purged it and data is intact. (Without the reconcile the
# assert-build no-overlap validator fires on the overlapping row.)
#
# Usage:  test/native_reclaim_reconcile.sh [PG_CONFIG]
# Written fresh for pgColumnar.

set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

hash_n() { pgc_set_hash 'SELECT id, v FROM n'; }
hash_h() { pgc_set_hash 'SELECT id, v FROM h'; }

psql_run "CREATE TABLE h (id int, v text);"
psql_run "CREATE TABLE n (id int, v text) USING pgcolumnar;"
psql_run "SELECT pgcolumnar.alter_columnar_table_set('n', stripe_row_limit => 1000, chunk_group_row_limit => 1000);"
psql_run "INSERT INTO h SELECT g, md5(g::text) FROM generate_series(1, 8000) g;"
psql_run "INSERT INTO n SELECT g, md5(g::text) FROM generate_series(1, 8000) g;"
# partial deletes so every group is a compact_rewrite candidate
psql_run "DELETE FROM h WHERE id % 3 = 0;"
psql_run "DELETE FROM n WHERE id % 3 = 0;"
check "parity before" "$(hash_n)" "$(hash_h)"

# a live group's byte range, and the storage id
sid="$(q "SELECT pgcolumnar.get_storage_id('n');")"
off="$(q "SELECT fileoffset FROM pgcolumnar.stats('n') ORDER BY fileoffset LIMIT 1;")"
len="$(q "SELECT datalength FROM pgcolumnar.stats('n') ORDER BY fileoffset LIMIT 1;")"

# synthesize the post-crash inverted state: a reusable (ancient freed_xid) free
# range sitting on top of a live group.
psql_run "INSERT INTO pgcolumnar.free_space VALUES ($sid, $off, $len, 1);"
check "stale overlapping free row injected" \
	"$(q "SELECT count(*) FROM pgcolumnar.free_space WHERE storage_id=$sid AND file_offset=$off AND freed_xid=1;")" "1"

# compact_rewrite reconciles first, so it neither reuses the stale row nor trips
# the no-overlap validator; data stays correct.
psql_run "SELECT pgcolumnar.compact_rewrite('n', 0.1);"
check "reconcile purged the stale overlapping row" \
	"$(q "SELECT count(*) FROM pgcolumnar.free_space WHERE freed_xid=1;")" "0"
check "data intact after reconcile + rewrite" "$(hash_n)" "$(hash_h)"
check "row count intact" "$(q 'SELECT count(*) FROM n;')" "$(q 'SELECT count(*) FROM h;')"

# reuse keeps working correctly afterward.
for r in 1 2; do
	psql_run "DELETE FROM h WHERE id % 5 = $r;"
	psql_run "DELETE FROM n WHERE id % 5 = $r;"
	psql_run "SELECT pgcolumnar.compact_rewrite('n', 0.0);"
	check "parity after reuse cycle $r" "$(hash_n)" "$(hash_h)"
done

pgc_summary
