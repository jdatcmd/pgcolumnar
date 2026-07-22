#!/usr/bin/env bash
#
# pgColumnar format 2.1 hardening suite: on-disk backward compatibility and
# corrupted-input robustness for the encoding and bloom-filter metadata.
#
#   1. Format 2.0 compatibility. A 2.0 chunk row carries no value_encoding_type,
#      value_raw_length, or bloom_filter. The reader must treat a NULL encoding
#      type as "none" with raw length equal to the decompressed length, and a
#      NULL bloom as absent. We simulate 2.0 rows by NULLing those columns for
#      the chunks that were actually stored with encoding none, then check reads
#      still match the heap oracle.
#   2. Corrupted input. An invalid encoding type must raise a clean error, not
#      crash the backend (a following query still works). A malformed bloom
#      filter must be ignored (conservatively "may match"), so results stay
#      correct.
#
# Usage:  test/hardening.sh [PG_CONFIG]
#
# Written fresh for pgColumnar; it does not reuse any upstream test file.

set -uo pipefail

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

# ---------------------------------------------------------------------------
# Native mode (D6e): the format 2.0 compatibility path (part 1) is 2.2-only and
# does not apply. Cover the corrupted-input intent against the native catalogs:
# an invalid encoding descriptor must raise a clean error (not crash), and a
# malformed native bloom must be ignored so results stay correct.
# ---------------------------------------------------------------------------
if native_mode; then
	echo "-- native: corrupted input robustness"

	make_pair "id int, v bigint"
	load_pair "SELECT g, g*2 FROM generate_series(1,3000) g"
	psql_run "UPDATE pgcolumnar.column_chunk SET encoding_descriptor = '\\\\xffffffff'::bytea
			  WHERE storage_id = pgcolumnar.get_storage_id('t_col') AND column_index = 1;"
	badresult="$(q "SELECT * FROM t_col;")"
	check "invalid descriptor raises error" "$([ -z "$badresult" ] && echo errored)" "errored"
	check "backend alive after invalid descriptor" "$(q "SELECT 1;")" "1"

	make_pair "id int, k bigint"
	q "SELECT pgcolumnar.alter_columnar_table_set('t_col', chunk_group_row_limit => 1000, stripe_row_limit => 20000);" >/dev/null
	load_pair "SELECT g, ((g*2654435761)%100000)::bigint FROM generate_series(1,8000) g"
	psql_run "UPDATE pgcolumnar.bloom SET filter = '\\\\x00'::bytea
			  WHERE storage_id = pgcolumnar.get_storage_id('t_col') AND column_index = 1;"
	diff_query "corrupt bloom present" "SELECT id FROM %T WHERE k = ((7*2654435761)%100000)::bigint"
	diff_query "corrupt bloom absent"  "SELECT count(*) FROM %T WHERE k = 424242424"
	check "backend alive after corrupt bloom" "$(q "SELECT 1;")" "1"

	pgc_summary
fi

# ---------------------------------------------------------------------------
# Part 1: format 2.0 compatibility (NULL-default reader path)
# ---------------------------------------------------------------------------
echo "-- part 1: format 2.0 compatibility"

make_pair "id int, lowc int, rnd bigint, txt text"
q "SELECT pgcolumnar.alter_columnar_table_set('t_col', chunk_group_row_limit => 1000, stripe_row_limit => 20000);" >/dev/null
load_pair "SELECT g, g%4, ((g*2654435761)%1000000000)::bigint, md5(g::text)
	FROM generate_series(1,8000) g"

# Simulate 2.0 chunk rows: drop the 2.1 columns for chunks stored with no
# encoding (so NULL genuinely means encoding none, matching a 2.0 writer).
psql_run "UPDATE pgcolumnar.chunk
		  SET value_encoding_type = NULL, value_raw_length = NULL, bloom_filter = NULL
		  WHERE storage_id = pgcolumnar.get_storage_id('t_col')
			AND value_encoding_type = 0;"

check "compat some chunks look like 2.0" \
	"$([ "$(q "SELECT count(*) FROM pgcolumnar.chunk WHERE storage_id=pgcolumnar.get_storage_id('t_col') AND value_encoding_type IS NULL;")" -gt 0 ] && echo yes)" \
	"yes"
diff_query "compat whole-row" "SELECT * FROM %T"
diff_query "compat aggregate" "SELECT count(*), sum(rnd), min(txt), max(txt) FROM %T"
diff_query "compat filter"    "SELECT id FROM %T WHERE lowc = 2"

# ---------------------------------------------------------------------------
# Part 2: corrupted input robustness
# ---------------------------------------------------------------------------
echo "-- part 2: corrupted input"

# An invalid encoding type must error cleanly, not crash the backend.
make_pair "id int, v bigint"
load_pair "SELECT g, g*2 FROM generate_series(1,3000) g"
psql_run "UPDATE pgcolumnar.chunk SET value_encoding_type = 99
		  WHERE storage_id = pgcolumnar.get_storage_id('t_col')
			AND attr_num = 2 AND chunk_group_num = 0;"
badresult="$(q "SELECT * FROM t_col;")"
check "invalid encoding raises error" "$([ -z "$badresult" ] && echo errored)" "errored"
check "backend alive after invalid encoding" "$(q "SELECT 1;")" "1"

# A malformed bloom filter must be ignored, keeping results correct.
make_pair "id int, k bigint"
q "SELECT pgcolumnar.alter_columnar_table_set('t_col', chunk_group_row_limit => 1000, stripe_row_limit => 20000);" >/dev/null
load_pair "SELECT g, ((g*2654435761)%100000)::bigint FROM generate_series(1,8000) g"
psql_run "UPDATE pgcolumnar.chunk SET bloom_filter = '\\\\x00'::bytea
		  WHERE storage_id = pgcolumnar.get_storage_id('t_col') AND attr_num = 2;"
diff_query "corrupt bloom present" "SELECT id FROM %T WHERE k = ((7*2654435761)%100000)::bigint"
diff_query "corrupt bloom absent"  "SELECT count(*) FROM %T WHERE k = 424242424"
check "backend alive after corrupt bloom" "$(q "SELECT 1;")" "1"

pgc_summary
