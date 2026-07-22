#!/usr/bin/env bash
#
# pgColumnar native per-chunk bloom filters (Phase D5b): a native-format (PGCN v1)
# table carries a per-column-chunk bloom filter over hashable columns, so an
# equality probe on an unsorted column skips row groups that provably lack the
# value (native spec 7.2), even when the min/max zone map cannot (overlapping
# ranges). This suite proves result parity with a heap mirror and isolates the
# bloom's effect: with values spread so every group's min/max spans the domain,
# an equality probe skips groups only when the bloom is enabled.
#
# Usage:  test/native_bloom.sh [PG_CONFIG]
# Written fresh for pgColumnar.

set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

# tag is a well-spread pseudo-random int over a large domain, so each 2048-row
# group's [min,max] spans almost the whole domain and min/max skipping cannot
# prune an in-range probe; the bloom can. 20480 rows -> 10 row groups.
GEN="SELECT g, ((g * 104729) % 1000000) AS tag FROM generate_series(1, 20480) g"

psql_run "CREATE TABLE h (id int, tag int);"
psql_run "CREATE TABLE n (id int, tag int) USING pgcolumnar;"
psql_run "SELECT pgcolumnar.alter_columnar_table_set('n', stripe_row_limit => 2048, chunk_group_row_limit => 1024);"
psql_run "INSERT INTO h $GEN;"
psql_run "INSERT INTO n $GEN;"

# A tag value that really occurs (so the answer is non-empty and present in only
# a few groups), taken from the heap mirror.
PROBE="$(q 'SELECT tag FROM h WHERE id = 7777;')"

skipped() {
	env PATH="$PGC_BINDIR:$PATH" psql -h 127.0.0.1 -p "$PGC_PORT" -U postgres \
		-d "$PGC_DB" -At -c "SET pgcolumnar.enable_bloom_filter = $2; EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) $1" 2>/dev/null \
		| grep 'Columnar Chunk Groups Removed by Filter' | grep -oE '[0-9]+' | head -1
}
gt0() { [ "${1:-0}" -gt 0 ] && echo yes || echo no; }

check "row count" "$(q 'SELECT count(*) FROM n;')" "20480"

# The bloom catalog is populated for the hashable tag column (10 groups) and the
# id column; a non-hashable case would be absent, but both int4 here are bloomed.
check "bloom rows written" \
	"$(q "SELECT count(*) FROM pgcolumnar.bloom WHERE storage_id = pgcolumnar.get_storage_id('n') AND column_index = 1;")" \
	"10"

# Equality probe result parity.
check "equality probe parity" \
	"$(pgc_set_hash "SELECT id, tag FROM n WHERE tag = $PROBE")" \
	"$(pgc_set_hash "SELECT id, tag FROM h WHERE tag = $PROBE")"

# The bloom, not min/max, is what prunes: groups are skipped with the bloom on and
# (nearly) none with it off, because the spread values make min/max ranges overlap.
check "bloom on skips groups" "$(gt0 "$(skipped "SELECT id FROM n WHERE tag = $PROBE" on)")" "yes"
check "bloom off skips nothing (min/max cannot)" "$(skipped "SELECT id FROM n WHERE tag = $PROBE" off)" "0"

# A value that never occurs is pruned everywhere and returns nothing.
check "absent value parity" \
	"$(q 'SELECT count(*) FROM n WHERE tag = 999999999;')" \
	"$(q 'SELECT count(*) FROM h WHERE tag = 999999999;')"

pgc_summary
