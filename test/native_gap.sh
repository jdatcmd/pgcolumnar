#!/usr/bin/env bash
#
# pgColumnar gap-tolerant write path (Phase F, prerequisite for end-truncation).
#
# The metapage highwater (reservedOffset) can legitimately sit AHEAD of the
# physical EOF: that is the state an aborted or crash-interrupted end-truncation
# leaves (the file was shortened but the highwater was not). A write whose target
# block is beyond EOF must fill the gap with empty pages and succeed, so the state
# self-heals on the next write. This suite forces that state with a test-only hook
# (columnar_debug_advance_reserved_offset, bound here rather than shipped) and
# asserts writes across the gap succeed, data stays correct against a heap mirror,
# the file physically materializes the gap, and the table is fully usable after.
#
# Usage:  test/native_gap.sh [PG_CONFIG]
# Written fresh for pgColumnar.

set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

# bind the internal test hook (deliberately not in the shipped catalog).
psql_run "CREATE FUNCTION pgcolumnar.debug_advance_reserved_offset(regclass, int)
  RETURNS void AS 'pgcolumnar', 'columnar_debug_advance_reserved_offset'
  LANGUAGE C;"

psql_run "CREATE TABLE h (id int, v text);"
psql_run "CREATE TABLE n (id int, v text) USING pgcolumnar;"
psql_run "SELECT pgcolumnar.alter_columnar_table_set('n', stripe_row_limit => 1000, chunk_group_row_limit => 1000);"

hash_n() { pgc_set_hash 'SELECT id, v FROM n'; }
hash_h() { pgc_set_hash 'SELECT id, v FROM h'; }

psql_run "INSERT INTO h SELECT g, md5(g::text) FROM generate_series(1, 3000) g;"
psql_run "INSERT INTO n SELECT g, md5(g::text) FROM generate_series(1, 3000) g;"
check "baseline row count" "$(q 'SELECT count(*) FROM n;')" "3000"
check "baseline parity" "$(hash_n)" "$(hash_h)"
size1="$(q 'SELECT pg_relation_size('"'"'n'"'"');')"

# push the highwater 50 pages past the physical EOF, creating a gap.
psql_run "SELECT pgcolumnar.debug_advance_reserved_offset('n', 50);"

# writes now target blocks beyond EOF: they must gap-fill and succeed.
psql_run "INSERT INTO h SELECT g, md5(g::text) FROM generate_series(3001, 6000) g;"
psql_run "INSERT INTO n SELECT g, md5(g::text) FROM generate_series(3001, 6000) g;"
check "row count after gapped insert" "$(q 'SELECT count(*) FROM n;')" "6000"
check "parity after gapped insert" "$(hash_n)" "$(hash_h)"
size2="$(q 'SELECT pg_relation_size('"'"'n'"'"');')"
echo "  (file size: before-gap=$size1 after-gap=$size2)"
# the 50-page gap plus new data must have physically extended the file well past
# the pre-gap size (allow slack; assert at least ~40 pages of growth).
check "gap was physically materialized" \
	"$([ "$size2" -gt "$((size1 + 40 * 8192))" ] && echo yes || echo no)" "yes"

# the table stays fully usable: more inserts, an index, and an indexed lookup.
psql_run "INSERT INTO h SELECT g, md5(g::text) FROM generate_series(6001, 7000) g;"
psql_run "INSERT INTO n SELECT g, md5(g::text) FROM generate_series(6001, 7000) g;"
check "parity after post-gap insert" "$(hash_n)" "$(hash_h)"
psql_run "CREATE INDEX n_id ON n (id);"
check "indexed lookup after gap" \
	"$(q 'SELECT count(*) FROM n WHERE id BETWEEN 1 AND 7000;')" "7000"

# a delete + reader still resolves correctly across the gapped layout.
psql_run "DELETE FROM h WHERE id % 7 = 0;"
psql_run "DELETE FROM n WHERE id % 7 = 0;"
check "parity after delete on gapped layout" "$(hash_n)" "$(hash_h)"

pgc_summary
