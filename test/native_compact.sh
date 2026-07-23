#!/usr/bin/env bash
#
# pgColumnar online compaction (Phase F3a, the lazy path). pgcolumnar.compact()
# retires row groups that are fully deleted as-of the oldest-xmin horizon,
# dropping their catalog rows so scans skip them, WITHOUT rewriting data and
# WITHOUT AccessExclusiveLock -- it holds only ShareUpdateExclusiveLock, so it is
# concurrent with readers and writers. This suite proves: results match a heap
# mirror after deletes + compact; a fully-deleted group is retired (row_group
# count drops); a partially-deleted group is NOT retired; and the lock taken is
# ShareUpdateExclusiveLock, never AccessExclusiveLock.
#
# Usage:  test/native_compact.sh [PG_CONFIG]
# Written fresh for pgColumnar.

set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

# 8192 rows in 8 row groups of 1024 (group k covers ids k*1024+1 .. (k+1)*1024).
GEN="SELECT g, (g % 100) AS v, 'p' || (g % 50) AS payload
  FROM generate_series(1, 8192) g"

psql_run "CREATE TABLE h (id int, v int, payload text);"
psql_run "CREATE TABLE n (id int, v int, payload text) USING pgcolumnar;"
psql_run "SELECT pgcolumnar.alter_columnar_table_set('n', stripe_row_limit => 1024, chunk_group_row_limit => 1024);"
psql_run "INSERT INTO h $GEN;"
psql_run "INSERT INTO n $GEN;"

groups() { q "SELECT count(*) FROM pgcolumnar.row_group WHERE storage_id = pgcolumnar.get_storage_id('n');"; }

check "row count" "$(q 'SELECT count(*) FROM n;')" "8192"
check "initial group count" "$(groups)" "8"

# Delete two whole groups (ids 2049..4096 = groups 2 and 3) in both tables, commit.
psql_run "DELETE FROM h WHERE id BETWEEN 2049 AND 4096;"
psql_run "DELETE FROM n WHERE id BETWEEN 2049 AND 4096;"
# Partially delete one group (half of group 5: ids 5121..5632).
psql_run "DELETE FROM h WHERE id BETWEEN 5121 AND 5632;"
psql_run "DELETE FROM n WHERE id BETWEEN 5121 AND 5632;"

check "parity after deletes" \
	"$(pgc_set_hash 'SELECT id, v, payload FROM n ORDER BY id')" \
	"$(pgc_set_hash 'SELECT id, v, payload FROM h ORDER BY id')"

# Online compaction: the two fully-deleted groups are retired, the partially
# deleted one is kept. Returns the number retired.
retired="$(q "SELECT pgcolumnar.compact('n');")"
echo "  (compact retired $retired groups; group count now $(groups))"
check "compact retired the two fully-deleted groups" "$retired" "2"
check "group count dropped by two" "$(groups)" "6"

# Results are unchanged by compaction (deleted rows already gone; live rows stay).
check "parity after compact" \
	"$(pgc_set_hash 'SELECT id, v, payload FROM n ORDER BY id')" \
	"$(pgc_set_hash 'SELECT id, v, payload FROM h ORDER BY id')"
check "live row count after compact" \
	"$(q 'SELECT count(*) FROM n;')" \
	"$(q 'SELECT count(*) FROM h;')"

# The partially-deleted group's live rows are still readable.
check "partial-group rows still present" \
	"$(q 'SELECT count(*) FROM n WHERE id BETWEEN 4097 AND 6144;')" \
	"$(q 'SELECT count(*) FROM h WHERE id BETWEEN 4097 AND 6144;')"

# A second compact with nothing newly dead retires nothing.
check "second compact retires nothing" "$(q "SELECT pgcolumnar.compact('n');")" "0"

# Lock level: compact must hold ShareUpdateExclusiveLock, never AccessExclusiveLock.
locks="$(env PATH="$PGC_BINDIR:$PATH" psql -h 127.0.0.1 -p "$PGC_PORT" -U postgres \
	-d "$PGC_DB" -At 2>/dev/null <<'SQL' | grep -E '^[0-9]+\|[0-9]+$'
BEGIN;
SELECT pgcolumnar.compact('n');
SELECT count(*) FILTER (WHERE mode='ShareUpdateExclusiveLock')::text || '|' ||
       count(*) FILTER (WHERE mode='AccessExclusiveLock')::text
  FROM pg_locks WHERE relation='n'::regclass AND locktype='relation';
ROLLBACK;
SQL
)"
check "compact holds ShareUpdateExclusiveLock" "${locks%%|*}" "1"
check "compact holds no AccessExclusiveLock" "${locks##*|}" "0"

pgc_summary
