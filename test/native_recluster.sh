#!/usr/bin/env bash
#
# pgColumnar online reclustering (Phase F3c). pgcolumnar.recluster() re-establishes
# global Z-order clustering over several columns ONLINE, under
# ShareUpdateExclusiveLock (concurrent reads and writes), unlike the eager
# cluster() which holds AccessExclusiveLock. This suite proves it tightens the
# multi-column zone maps just as eager clustering does (a 2D box skips far more
# groups after), results match a heap mirror, indexes are maintained online (an
# index scan returns each live row exactly once), and the lock is
# ShareUpdateExclusiveLock, never AccessExclusiveLock.
#
# Usage:  test/native_recluster.sh [PG_CONFIG]
# Written fresh for pgColumnar.

set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

# 40960 rows in 20 row groups; x and y scattered in insert order, so before
# reclustering a small 2D box skips nothing.
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
psql_run "CREATE UNIQUE INDEX n_id_uq ON n (id);"

skipped() {
	env PATH="$PGC_BINDIR:$PATH" psql -h 127.0.0.1 -p "$PGC_PORT" -U postgres \
		-d "$PGC_DB" -At -c "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) $1" 2>/dev/null \
		| grep 'Columnar Chunk Groups Removed by Filter' | grep -oE '[0-9]+' | head -1
}
idxcount() {
	env PATH="$PGC_BINDIR:$PATH" psql -h 127.0.0.1 -p "$PGC_PORT" -U postgres \
		-d "$PGC_DB" -At 2>/dev/null -c \
		"SET enable_seqscan=off; SET pgcolumnar.enable_custom_scan=off;
		 SELECT count(*) FROM n WHERE id BETWEEN 1 AND 40960;" | tail -1
}

BOX="x BETWEEN 20 AND 40 AND y BETWEEN 20 AND 40"

check "row count" "$(q 'SELECT count(*) FROM n;')" "40960"
check "box parity (pre-recluster)" \
	"$(pgc_set_hash "SELECT id, x, y, payload FROM n WHERE $BOX")" \
	"$(pgc_set_hash "SELECT id, x, y, payload FROM h WHERE $BOX")"

before="$(skipped "SELECT id FROM n WHERE $BOX")"; before="${before:-0}"

# Online recluster by (x, y).
rec="$(q "SELECT pgcolumnar.recluster('n', 'x', 'y');")"
echo "  (recluster processed $rec groups; box skip before=$before)"
check "reclustered the groups" "$([ "$rec" -gt 0 ] && echo yes || echo no)" "yes"

after="$(skipped "SELECT id FROM n WHERE $BOX")"; after="${after:-0}"
echo "  (box groups skipped: before=$before after=$after)"
check "reclustering increases 2D skipping" "$([ "$after" -gt "$before" ] && echo yes || echo no)" "yes"
check "reclustering skips most groups" "$([ "$after" -ge 10 ] && echo yes || echo no)" "yes"

# Results unchanged.
check "full-table parity after recluster" \
	"$(pgc_set_hash 'SELECT id, x, y, payload FROM n ORDER BY id')" \
	"$(pgc_set_hash 'SELECT id, x, y, payload FROM h ORDER BY id')"
check "row count after recluster" "$(q 'SELECT count(*) FROM n;')" "40960"

# Online index maintenance: index scan returns each live row exactly once.
check "index scan returns each row once" "$(idxcount)" "40960"
check "unique still enforced" \
	"$(env PATH="$PGC_BINDIR:$PATH" psql -h 127.0.0.1 -p "$PGC_PORT" -U postgres -d "$PGC_DB" -At -v ON_ERROR_STOP=1 \
	    -c "INSERT INTO n VALUES (1, 0, 0, 'x');" >/dev/null 2>&1 && echo no || echo yes)" \
	"yes"

# Lock level: ShareUpdateExclusiveLock, never AccessExclusiveLock.
locks="$(env PATH="$PGC_BINDIR:$PATH" psql -h 127.0.0.1 -p "$PGC_PORT" -U postgres \
	-d "$PGC_DB" -At 2>/dev/null <<'SQL' | grep -E '^[0-9]+\|[0-9]+$'
BEGIN;
SELECT pgcolumnar.recluster('n', 'x', 'y');
SELECT count(*) FILTER (WHERE mode='ShareUpdateExclusiveLock')::text || '|' ||
       count(*) FILTER (WHERE mode='AccessExclusiveLock')::text
  FROM pg_locks WHERE relation='n'::regclass AND locktype='relation';
ROLLBACK;
SQL
)"
check "recluster holds ShareUpdateExclusiveLock" "${locks%%|*}" "1"
check "recluster holds no AccessExclusiveLock" "${locks##*|}" "0"

pgc_summary
