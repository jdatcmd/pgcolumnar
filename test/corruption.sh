#!/usr/bin/env bash
#
# pgColumnar catalog-corruption robustness suite.
#
# The value-stream decoders and the reader trust lengths, counts, offsets, and
# codes stored in the columnar.* catalog. This suite corrupts those exact fields
# and asserts the extension raises a clean error (or degrades safely) and the
# backend survives, rather than reading or writing out of bounds. It complements
# test/hardening.sh (which mutates on-disk bytes) by mutating the catalog
# directly, which a superuser can do and which stands in for bit rot or a crafted
# format-2.0 file. Run on an assert-enabled build so an out-of-bounds access
# would trip an assertion or crash and fail the survival check.
#
# Usage:  test/corruption.sh [PG_CONFIG]
#
# Written fresh for pgColumnar; it does not reuse any upstream test file.

set -uo pipefail

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

alive() { [ "$(q 'SELECT 1;')" = "1" ] && echo yes || echo no; }
errors() { if psql_run "$1" >/dev/null 2>&1; then echo no; else echo yes; fi; }
sid() { q "SELECT columnar.get_storage_id('c');"; }

mkc() {
	psql_run "DROP TABLE IF EXISTS c;" >/dev/null 2>&1
	psql_run "CREATE TABLE c (id int, r bigint, t text) USING columnar;"
	# r random -> poorly compressible; id monotonic; t low-cardinality -> dictionary
	psql_run "INSERT INTO c SELECT g, (random()*9e18)::bigint, 'v'||(g%5)
	          FROM generate_series(1,20000) g;"
}

echo "-- value_raw_length corruption must error, not crash"
mkc
psql_run "UPDATE columnar.chunk SET value_raw_length = value_raw_length + 123456
          WHERE storage_id = $(sid);"
check "raw_length corruption errors"    "$(errors 'SELECT count(*), sum(r) FROM c;')" "yes"
check "alive after raw_length"          "$(alive)" "yes"

echo "-- value_decompressed_length corruption must error, not crash"
mkc
psql_run "UPDATE columnar.chunk SET value_decompressed_length = value_decompressed_length + 4096
          WHERE storage_id = $(sid);"
check "decompressed_length corruption errors" "$(errors 'SELECT sum(r) FROM c;')" "yes"
check "alive after decompressed_length"       "$(alive)" "yes"

echo "-- value_count corruption must error, not crash"
mkc
psql_run "UPDATE columnar.chunk SET value_count = value_count + 100000
          WHERE storage_id = $(sid);"
check "value_count corruption errors"   "$(errors 'SELECT sum(r) FROM c;')" "yes"
check "alive after value_count"          "$(alive)" "yes"

echo "-- attr_num = 0 (negative index guard) must not crash"
mkc
psql_run "UPDATE columnar.chunk SET attr_num = 0 WHERE storage_id = $(sid) AND attr_num = 1;"
q "SELECT count(*) FROM c;" >/dev/null 2>&1
check "alive after attr_num=0"           "$(alive)" "yes"

echo "-- corrupt bloom header (huge nbits, short buffer) is treated as no filter"
mkc
# 5-byte bloom: nbits = 0x40000000 (2^30) little-endian + k = 6, with no bitset
psql_run "UPDATE columnar.chunk SET bloom_filter = '\\x0000004006'::bytea
          WHERE storage_id = $(sid) AND bloom_filter IS NOT NULL;"
check "corrupt bloom equality still correct" "$(q 'SELECT count(*) FROM c WHERE r = -999;')" "0"
check "alive after bloom corruption"         "$(alive)" "yes"

pgc_summary
