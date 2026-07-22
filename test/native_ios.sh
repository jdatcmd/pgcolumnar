#!/usr/bin/env bash
#
# pgColumnar native index-only scan (Phase D6c): after VACUUM marks a native
# (PGCN v1) table's all-visible row groups in the visibility map, an index-only
# scan over an all-visible range skips the fetch (Heap Fetches: 0) and returns the
# same rows as a heap mirror. A delete clears the VM bit (clear-on-write), so the
# scan falls back to the fetch and never returns a deleted row.
#
# Usage:  test/native_ios.sh [PG_CONFIG]
# Written fresh for pgColumnar.

set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

# One row group (large stripe limit) so an interior id range maps to blocks wholly
# inside an all-visible group.
psql_run "CREATE TABLE ioh (id int, v text);"
psql_run "CREATE TABLE ios (id int, v text) USING pgcolumnar;"
psql_run "SELECT pgcolumnar.alter_columnar_table_set('ios', stripe_row_limit => 16384, format_version => 1);"
GEN="SELECT g, 'r'||g FROM generate_series(1, 8000) g"
psql_run "INSERT INTO ioh $GEN;"
psql_run "INSERT INTO ios $GEN;"
psql_run "CREATE INDEX ios_id ON ios (id);"

psql_run "ALTER DATABASE $PGC_DB SET pgcolumnar.enable_index_only_scan = on;"
psql_run "ALTER DATABASE $PGC_DB SET pgcolumnar.enable_custom_scan = off;"
psql_run "ALTER DATABASE $PGC_DB SET enable_seqscan = off;"
psql_run "ALTER DATABASE $PGC_DB SET enable_bitmapscan = off;"

psql_run "VACUUM ios;"   # mark all-visible row groups in the VM fork

check "row count" "$(q 'SELECT count(*) FROM ios;')" "8000"

planon="$(q 'EXPLAIN (COSTS OFF) SELECT id FROM ios WHERE id BETWEEN 1000 AND 5000;')"
check "index-only scan chosen" "$(printf '%s' "$planon" | grep -c 'Index Only Scan')" "1"

eaav="$(q 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT id FROM ios WHERE id BETWEEN 1000 AND 5000;')"
check "all-visible interior: zero heap fetches" \
	"$(printf '%s' "$eaav" | grep -oE 'Heap Fetches: [0-9]+' | grep -oE '[0-9]+' | head -1)" "0"

check "index-only results match heap (all-visible)" \
	"$(pgc_set_hash 'SELECT id FROM ios WHERE id BETWEEN 1000 AND 5000')" \
	"$(pgc_set_hash 'SELECT id FROM ioh WHERE id BETWEEN 1000 AND 5000')"

# A delete clears the VM bit for the affected blocks; the scan falls back to the
# fetch there and must never return a deleted row.
psql_run "DELETE FROM ios WHERE id BETWEEN 2000 AND 2200;"
psql_run "DELETE FROM ioh WHERE id BETWEEN 2000 AND 2200;"
check "index-only results match heap after delete" \
	"$(pgc_set_hash 'SELECT id FROM ios WHERE id BETWEEN 1000 AND 5000')" \
	"$(pgc_set_hash 'SELECT id FROM ioh WHERE id BETWEEN 1000 AND 5000')"
eadel="$(q 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT id FROM ios WHERE id BETWEEN 1000 AND 5000;')"
hf="$(printf '%s' "$eadel" | grep -oE 'Heap Fetches: [0-9]+' | grep -oE '[0-9]+' | head -1)"
check "after delete: some heap fetches occur" "$([ "${hf:-0}" -gt 0 ] && echo yes || echo no)" "yes"

pgc_summary
