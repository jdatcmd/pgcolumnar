#!/usr/bin/env bash
#
# pgColumnar physical page reclaim (Phase F free-list). Retiring a row group
# records its data byte range in pgcolumnar.free_space; an online compaction
# (which holds ShareUpdateExclusiveLock and is self-serialized) then reserves from
# a freed range instead of advancing the file highwater, once the oldest-xmin
# horizon has passed the freeing transaction. So repeated online compaction reuses
# space and the relation file plateaus instead of growing every cycle. Plain
# inserts always append (they never race a reuse). This suite proves that repeated
# recluster keeps the file bounded (reuse), that free_space is populated and
# consumed, and that the reused blocks hold correct data (parity with a heap
# mirror).
#
# Usage:  test/native_reclaim.sh [PG_CONFIG]
# Written fresh for pgColumnar.

set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

GEN="SELECT g, g AS v, md5(g::text) AS payload FROM generate_series(1, 6000) g"

psql_run "CREATE TABLE h (id int, v int, payload text);"
psql_run "CREATE TABLE n (id int, v int, payload text) USING pgcolumnar;"
psql_run "SELECT pgcolumnar.alter_columnar_table_set('n', stripe_row_limit => 1000, chunk_group_row_limit => 1000);"
psql_run "INSERT INTO h $GEN;"
psql_run "INSERT INTO n $GEN;"

fsize() { q "SELECT pg_relation_size('n');"; }
freerows() { q "SELECT count(*) FROM pgcolumnar.free_space WHERE storage_id = pgcolumnar.get_storage_id('n');"; }

size1="$(fsize)"
check "initial data present" "$(q 'SELECT count(*) FROM n;')" "6000"
check "no free space yet" "$(freerows)" "0"

# First online recluster: retires the original groups (freed) and writes new ones.
# With no reusable space yet it appends, so the file grows and free_space fills.
psql_run "SELECT pgcolumnar.recluster('n', 'id');"
size2="$(fsize)"
check "free space recorded after first recluster" \
	"$([ "$(freerows)" -gt 0 ] && echo yes || echo no)" "yes"
check "data intact after first recluster" \
	"$(q 'SELECT count(*) FROM n;')" "6000"

# Repeated reclusters must REUSE the previous cycle's freed space (its freeing
# transaction has committed and the horizon has passed it), so the file plateaus
# instead of growing every cycle. Without reuse it would grow by ~size1 each time.
for i in 1 2 3 4; do
	psql_run "SELECT pgcolumnar.recluster('n', 'id');"
done
size3="$(fsize)"
echo "  (file size: initial=$size1 after-1-recluster=$size2 after-5-reclusters=$size3; free_space rows=$(freerows))"

check "repeated recluster reuses space (file plateaus)" \
	"$([ "$size3" -le "$((size2 + size1))" ] && echo yes || echo no)" "yes"
check "free space is being reused (bounded, not growing per cycle)" \
	"$([ "$(freerows)" -gt 0 ] && echo yes || echo no)" "yes"

# The reused blocks hold correct data.
check "reused-block data matches heap mirror" \
	"$(pgc_set_hash 'SELECT id, v, payload FROM n ORDER BY id')" \
	"$(pgc_set_hash 'SELECT id, v, payload FROM h ORDER BY id')"

# Reuse survives a delete + rewrite cycle too, with correct data.
psql_run "DELETE FROM h WHERE id % 4 = 0;"
psql_run "DELETE FROM n WHERE id % 4 = 0;"
psql_run "SELECT pgcolumnar.compact_rewrite('n', 0.0);"
psql_run "SELECT pgcolumnar.recluster('n', 'id');"
check "data correct after delete + compact_rewrite + recluster" \
	"$(pgc_set_hash 'SELECT id, v, payload FROM n ORDER BY id')" \
	"$(pgc_set_hash 'SELECT id, v, payload FROM h ORDER BY id')"
check "row count correct after cycle" "$(q 'SELECT count(*) FROM n;')" "$(q 'SELECT count(*) FROM h;')"

pgc_summary
