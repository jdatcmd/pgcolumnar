#!/usr/bin/env bash
#
# pgColumnar online rewrite of partially-deleted groups (Phase F3b).
# pgcolumnar.compact_rewrite() rewrites groups whose deleted fraction exceeds a
# threshold into fresh groups (dropping the dead rows) with fresh row numbers,
# ONLINE under ShareUpdateExclusiveLock. This suite proves: results match a heap
# mirror; the dead rows' space is reclaimed (deletedrows drops); INDEX scans
# return each live row exactly once despite the transient old/new row-number
# overlap (unique constraint still holds, point lookups correct); and the lock is
# ShareUpdateExclusiveLock, never AccessExclusiveLock. Concurrency is covered by
# native_rewrite_conc.sh.
#
# Usage:  test/native_rewrite.sh [PG_CONFIG]
# Written fresh for pgColumnar.

set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

GEN="SELECT g, (g % 1000) AS v, 'p' || (g % 100) AS payload
  FROM generate_series(1, 8192) g"

psql_run "CREATE TABLE h (id int, v int, payload text);"
psql_run "CREATE TABLE n (id int, v int, payload text) USING pgcolumnar;"
psql_run "SELECT pgcolumnar.alter_columnar_table_set('n', stripe_row_limit => 1024, chunk_group_row_limit => 1024);"
psql_run "INSERT INTO h $GEN;"
psql_run "INSERT INTO n $GEN;"
psql_run "CREATE UNIQUE INDEX n_id_uq ON n (id);"
psql_run "CREATE INDEX n_v ON n (v);"

deletedrows() { q "SELECT COALESCE(sum(deletedrows), 0) FROM pgcolumnar.stats('n'::regclass);"; }

# Partially delete a third of the rows across every group (id % 3 = 0), commit.
psql_run "DELETE FROM h WHERE id % 3 = 0;"
psql_run "DELETE FROM n WHERE id % 3 = 0;"

check "row count after delete" "$(q 'SELECT count(*) FROM n;')" "$(q 'SELECT count(*) FROM h;')"
check "there are deleted rows to reclaim" "$([ "$(deletedrows)" -gt 0 ] && echo yes || echo no)" "yes"
check "parity after delete" \
	"$(pgc_set_hash 'SELECT id, v, payload FROM n ORDER BY id')" \
	"$(pgc_set_hash 'SELECT id, v, payload FROM h ORDER BY id')"

# Online rewrite of the partially-deleted groups.
rew="$(q "SELECT pgcolumnar.compact_rewrite('n', 0.2);")"
echo "  (compact_rewrite rewrote $rew groups; deletedrows now $(deletedrows))"
check "rewrote some groups" "$([ "$rew" -gt 0 ] && echo yes || echo no)" "yes"
check "dead-row space reclaimed" "$(deletedrows)" "0"

# Results unchanged.
check "parity after rewrite" \
	"$(pgc_set_hash 'SELECT id, v, payload FROM n ORDER BY id')" \
	"$(pgc_set_hash 'SELECT id, v, payload FROM h ORDER BY id')"
check "row count after rewrite" \
	"$(q 'SELECT count(*) FROM n;')" \
	"$(q 'SELECT count(*) FROM h;')"

# Index-scan correctness: force an index scan and confirm each live row is
# returned exactly once (no duplicate from lingering old-number index entries).
idxcount() {
	env PATH="$PGC_BINDIR:$PATH" psql -h 127.0.0.1 -p "$PGC_PORT" -U postgres \
		-d "$PGC_DB" -At 2>/dev/null -c \
		"SET enable_seqscan=off; SET pgcolumnar.enable_custom_scan=off;
		 SELECT count(*) FROM n WHERE id BETWEEN 1 AND 8192;" | tail -1
}
check "index scan returns each live row once" "$(idxcount)" "$(q 'SELECT count(*) FROM h;')"
# Point lookups via the unique index.
check "live id present via index" "$(q 'SELECT count(*) FROM n WHERE id = 4097;')" "1"
check "deleted id absent via index" "$(q 'SELECT count(*) FROM n WHERE id = 4098;')" "0"

# Uniqueness still enforced after the rewrite (no stale live duplicate).
dupfails() {
	if env PATH="$PGC_BINDIR:$PATH" psql -h 127.0.0.1 -p "$PGC_PORT" -U postgres \
		-d "$PGC_DB" -At -v ON_ERROR_STOP=1 -c "INSERT INTO n VALUES (4097, 0, 'x');" >/dev/null 2>&1; then
		echo no
	else
		echo yes
	fi
}
check "unique constraint still enforced" "$(dupfails)" "yes"

# Lock level: ShareUpdateExclusiveLock, never AccessExclusiveLock.
locks="$(env PATH="$PGC_BINDIR:$PATH" psql -h 127.0.0.1 -p "$PGC_PORT" -U postgres \
	-d "$PGC_DB" -At 2>/dev/null <<'SQL' | grep -E '^[0-9]+\|[0-9]+$'
BEGIN;
SELECT pgcolumnar.compact_rewrite('n', 0.0);
SELECT count(*) FILTER (WHERE mode='ShareUpdateExclusiveLock')::text || '|' ||
       count(*) FILTER (WHERE mode='AccessExclusiveLock')::text
  FROM pg_locks WHERE relation='n'::regclass AND locktype='relation';
ROLLBACK;
SQL
)"
check "compact_rewrite holds ShareUpdateExclusiveLock" "${locks%%|*}" "1"
check "compact_rewrite holds no AccessExclusiveLock" "${locks##*|}" "0"

pgc_summary
