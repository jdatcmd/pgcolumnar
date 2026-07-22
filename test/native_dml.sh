#!/usr/bin/env bash
#
# pgColumnar native delete and update (Phase D6b): DELETE, UPDATE and MERGE on a
# native-format (PGCN v1) table, via the interim row mask keyed by row group. Every
# result is compared to a heap mirror: the surviving rows, counts, aggregates after
# deletes (which fall back from the zone-map path to a delete-applying scan), and
# index lookups of deleted vs live rows. The interim row mask; Phase F replaces it
# with a first-class delete vector.
#
# Usage:  test/native_dml.sh [PG_CONFIG]
# Written fresh for pgColumnar.

set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

psql_run "CREATE TABLE h (id int PRIMARY KEY, k bigint, v text);"
psql_run "CREATE TABLE n (id int PRIMARY KEY, k bigint, v text) USING pgcolumnar;"
psql_run "SELECT pgcolumnar.alter_columnar_table_set('n', stripe_row_limit => 1000, format_version => 1);"
GEN="SELECT g, (g * 10)::bigint, 'row-' || g FROM generate_series(1, 5000) g"
psql_run "INSERT INTO h $GEN;"
psql_run "INSERT INTO n $GEN;"

# DELETE across several row groups.
psql_run "DELETE FROM h WHERE id % 7 = 0;"
psql_run "DELETE FROM n WHERE id % 7 = 0;"

check "count after delete" "$(q 'SELECT count(*) FROM n;')" "$(q 'SELECT count(*) FROM h;')"
check "rows after delete match heap" \
	"$(pgc_set_hash 'SELECT id, k, v FROM n')" \
	"$(pgc_set_hash 'SELECT id, k, v FROM h')"
# Aggregates after delete: the zone-map path is bypassed (deletes present), so this
# exercises the delete-applying scan fold.
check "sum(k) after delete" "$(q 'SELECT sum(k) FROM n;')" "$(q 'SELECT sum(k) FROM h;')"
check "min/max after delete" \
	"$(q 'SELECT min(id), max(id) FROM n;')" \
	"$(q 'SELECT min(id), max(id) FROM h;')"
check "deleted rows gone" "$(q 'SELECT count(*) FROM n WHERE id % 7 = 0;')" "0"

# Index lookup of a deleted row returns nothing; a live row still resolves.
idx() { env PATH="$PGC_BINDIR:$PATH" psql -h 127.0.0.1 -p "$PGC_PORT" -U postgres -d "$PGC_DB" -q -At -c "SET enable_seqscan=off" -c "$1" 2>/dev/null; }
# id 4444 is live (4444 % 7 = 6); id 4445 was deleted (4445 % 7 = 0).
check "index finds live row"    "$(idx 'SELECT v FROM n WHERE id = 4444;')" "$(q 'SELECT v FROM h WHERE id = 4444;')"
check "index skips deleted row" "$(idx 'SELECT count(*) FROM n WHERE id = 4445;')" "0"

# UPDATE (delete old + insert new), including a plain column and a re-scan.
psql_run "UPDATE h SET k = k + 1 WHERE id % 5 = 0;"
psql_run "UPDATE n SET k = k + 1 WHERE id % 5 = 0;"
check "count after update" "$(q 'SELECT count(*) FROM n;')" "$(q 'SELECT count(*) FROM h;')"
check "rows after update match heap" \
	"$(pgc_set_hash 'SELECT id, k, v FROM n')" \
	"$(pgc_set_hash 'SELECT id, k, v FROM h')"
check "sum(k) after update" "$(q 'SELECT sum(k) FROM n;')" "$(q 'SELECT sum(k) FROM h;')"

# The primary key still rejects a duplicate of a surviving row and admits a new id.
check "PK still enforced after DML" \
	"$(psql_run 'INSERT INTO n VALUES (4444, 0, '"'"'x'"'"');' 2>&1 | grep -c -i 'duplicate key\|unique')" \
	"1"

# MERGE (PostgreSQL 15+): update matched, insert not-matched.
if [ "$(q 'SELECT current_setting('"'"'server_version_num'"'"')::int >= 150000;')" = "t" ]; then
	psql_run "CREATE TABLE src (id int, k bigint);"
	psql_run "INSERT INTO src VALUES (1, 999), (999001, 5), (999002, 6);"
	MERGE="MERGE INTO %T t USING src s ON t.id = s.id
	       WHEN MATCHED THEN UPDATE SET k = s.k
	       WHEN NOT MATCHED THEN INSERT (id, k, v) VALUES (s.id, s.k, 'm');"
	psql_run "${MERGE//%T/h}"
	psql_run "${MERGE//%T/n}"
	check "count after merge" "$(q 'SELECT count(*) FROM n;')" "$(q 'SELECT count(*) FROM h;')"
	check "rows after merge match heap" \
		"$(pgc_set_hash 'SELECT id, k, v FROM n')" \
		"$(pgc_set_hash 'SELECT id, k, v FROM h')"
fi

pgc_summary
