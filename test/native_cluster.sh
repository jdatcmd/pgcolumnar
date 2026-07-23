#!/usr/bin/env bash
#
# pgColumnar Z-order clustering (Phase F2). pgcolumnar.cluster(table, cols)
# physically reorders rows by the Z-order (Morton) space-filling curve over
# several columns, so the per-group min/max zone maps of ALL clustered columns
# tighten at once. A multi-column box predicate then skips far more row groups
# than in insert order, where the columns are scattered. Clustering only reorders
# storage, so results are identical to a heap mirror. This suite proves parity,
# the skipping improvement, single-column clustering, and a clean error on an
# unsupported clustering type. It is the EAGER reorg (AccessExclusiveLock); the
# online incremental path is Phase F3.
#
# Usage:  test/native_cluster.sh [PG_CONFIG]
# Written fresh for pgColumnar.

set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

# 40960 rows in 20 row groups (2048 each). x and y are scattered in insert order
# via coprime multiplicative hashes, so before clustering each group's x and y
# ranges span most of [0,200) and a small 2D box skips nothing. Z-order
# clustering co-locates nearby (x,y).
GEN="SELECT g,
       ((g::bigint * 7919) % 200)::int   AS x,
       ((g::bigint * 104729) % 200)::int AS y,
       'p' || (g % 97)                    AS payload
  FROM generate_series(1, 40960) g"

psql_run "CREATE TABLE h (id int, x int, y int, payload text);"
psql_run "CREATE TABLE n (id int, x int, y int, payload text) USING pgcolumnar;"
psql_run "SELECT pgcolumnar.alter_columnar_table_set('n', stripe_row_limit => 2048, chunk_group_row_limit => 1024);"
psql_run "INSERT INTO h $GEN;"
psql_run "INSERT INTO n $GEN;"

# Count of row groups skipped by the zone maps for a query, from EXPLAIN ANALYZE.
skipped() {
	env PATH="$PGC_BINDIR:$PATH" psql -h 127.0.0.1 -p "$PGC_PORT" -U postgres \
		-d "$PGC_DB" -At -c "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) $1" 2>/dev/null \
		| grep 'Columnar Chunk Groups Removed by Filter' | grep -oE '[0-9]+' | head -1
}
# Does a statement fail (for the unsupported-type check)?
fails() {
	if env PATH="$PGC_BINDIR:$PATH" psql -h 127.0.0.1 -p "$PGC_PORT" -U postgres \
		-d "$PGC_DB" -At -v ON_ERROR_STOP=1 -c "$1" >/dev/null 2>&1; then
		echo no
	else
		echo yes
	fi
}

BOX="x BETWEEN 20 AND 40 AND y BETWEEN 20 AND 40"

check "row count" "$(q 'SELECT count(*) FROM n;')" "40960"
check "box parity (pre-cluster)" \
	"$(pgc_set_hash "SELECT id, x, y, payload FROM n WHERE $BOX")" \
	"$(pgc_set_hash "SELECT id, x, y, payload FROM h WHERE $BOX")"

before="$(skipped "SELECT id FROM n WHERE $BOX")"; before="${before:-0}"

# Cluster by (x, y) -- the eager Z-order reorg.
psql_run "SELECT pgcolumnar.cluster('n', 'x', 'y');"

check "row count after cluster" "$(q 'SELECT count(*) FROM n;')" "40960"
check "box parity (post-cluster)" \
	"$(pgc_set_hash "SELECT id, x, y, payload FROM n WHERE $BOX")" \
	"$(pgc_set_hash "SELECT id, x, y, payload FROM h WHERE $BOX")"
check "full-table parity after cluster" \
	"$(pgc_set_hash 'SELECT id, x, y, payload FROM n ORDER BY id')" \
	"$(pgc_set_hash 'SELECT id, x, y, payload FROM h ORDER BY id')"

after="$(skipped "SELECT id FROM n WHERE $BOX")"; after="${after:-0}"

echo "  (chunk groups skipped for the box: before=$before after=$after of 20)"
# Z-order tightens both columns' zone maps, so the box skips strictly more groups
# after clustering, and skips most of the 20 groups.
check "clustering increases 2D skipping" "$([ "$after" -gt "$before" ] && echo yes || echo no)" "yes"
check "clustering skips most groups" "$([ "$after" -ge 10 ] && echo yes || echo no)" "yes"

# Single-column clustering works and preserves results.
psql_run "SELECT pgcolumnar.cluster('n', 'id');"
check "single-column cluster parity" \
	"$(pgc_set_hash 'SELECT id, x, y, payload FROM n ORDER BY id')" \
	"$(pgc_set_hash 'SELECT id, x, y, payload FROM h ORDER BY id')"

# An unsupported clustering type (text) must error cleanly, not crash.
check "unsupported type errors cleanly" "$(fails "SELECT pgcolumnar.cluster('n', 'payload')")" "yes"
# A non-existent column errors cleanly too.
check "missing column errors cleanly" "$(fails "SELECT pgcolumnar.cluster('n', 'nope')")" "yes"

pgc_summary
