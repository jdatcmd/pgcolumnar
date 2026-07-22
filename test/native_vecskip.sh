#!/usr/bin/env bash
#
# pgColumnar native per-vector skipping (Phase D5b): within a row group that a
# predicate does not prune wholesale, individual 1024-value vectors whose per-
# vector zone map rules them out are neither decoded nor emitted (native spec
# 7.1). This suite forces a single large row group (so nothing is pruned at the
# row-group level) with many vectors, and shows a selective range prunes vectors:
# "Columnar Vectors Skipped" > 0 while "Chunk Groups Removed by Filter"
# stays 0. Results always match a heap mirror, since skipping is a pure
# optimization the executor's re-check makes correctness-neutral.
#
# Usage:  test/native_vecskip.sh [PG_CONFIG]
# Written fresh for pgColumnar.

set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

# One row group (stripe limit above the row count) of 8 vectors x 1024 rows.
# id is monotonic, so each vector's min/max is tight and non-overlapping.
GEN="SELECT g AS id, (g % 97) AS v FROM generate_series(1, 8192) g"

psql_run "CREATE TABLE h (id int, v int);"
psql_run "CREATE TABLE n (id int, v int) USING pgcolumnar;"
psql_run "SELECT pgcolumnar.alter_columnar_table_set('n', stripe_row_limit => 16384, chunk_group_row_limit => 1024);"
psql_run "INSERT INTO h $GEN;"
psql_run "INSERT INTO n $GEN;"

# EXPLAIN ANALYZE counters for a query.
counter() {
	env PATH="$PGC_BINDIR:$PATH" psql -h 127.0.0.1 -p "$PGC_PORT" -U postgres \
		-d "$PGC_DB" -At -c "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) $2" 2>/dev/null \
		| grep "$1" | grep -oE '[0-9]+' | head -1
}
gt0() { [ "${1:-0}" -gt 0 ] && echo yes || echo no; }

check "row count" "$(q 'SELECT count(*) FROM n;')" "8192"
check "single row group" \
	"$(q "SELECT count(*) FROM pgcolumnar.row_group WHERE storage_id = pgcolumnar.get_storage_id('n');")" \
	"1"

# A range confined to one vector: correct, prunes vectors, prunes no whole group.
Q="SELECT id, v FROM n WHERE id BETWEEN 100 AND 200"
check "range parity" \
	"$(pgc_set_hash "$Q")" \
	"$(pgc_set_hash 'SELECT id, v FROM h WHERE id BETWEEN 100 AND 200')"
check "vectors removed > 0" "$(gt0 "$(counter 'Columnar Vectors Skipped' "SELECT id FROM n WHERE id BETWEEN 100 AND 200")")" "yes"
check "no whole group removed" "$(counter 'Columnar Chunk Groups Removed by Filter' "SELECT id FROM n WHERE id BETWEEN 100 AND 200")" "0"

# A predicate matching every vector prunes none.
check "non-selective removes no vectors" \
	"$(counter 'Columnar Vectors Skipped' 'SELECT id FROM n WHERE id > 0')" "0"

# Boundary and cross-vector ranges still return exactly the heap rows.
check "cross-vector range parity" \
	"$(pgc_set_hash 'SELECT id, v FROM n WHERE id BETWEEN 2000 AND 5000')" \
	"$(pgc_set_hash 'SELECT id, v FROM h WHERE id BETWEEN 2000 AND 5000')"
check "last-vector range parity" \
	"$(pgc_set_hash 'SELECT id, v FROM n WHERE id BETWEEN 8000 AND 9000')" \
	"$(pgc_set_hash 'SELECT id, v FROM h WHERE id BETWEEN 8000 AND 9000')"
check "empty range parity" \
	"$(q 'SELECT count(*) FROM n WHERE id BETWEEN 100000 AND 200000;')" \
	"0"

pgc_summary
