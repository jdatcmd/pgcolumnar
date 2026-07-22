#!/usr/bin/env bash
#
# pgColumnar hardening suite: corrupted-input robustness for the native encoding
# and bloom-filter metadata. An invalid encoding descriptor must raise a clean
# error, not crash the backend (a following query still works). A malformed bloom
# filter must be ignored (conservatively "may match"), so results stay correct.
#
# Usage:  test/hardening.sh [PG_CONFIG]
#
# Written fresh for pgColumnar; it does not reuse any upstream test file.

set -uo pipefail

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

# An invalid encoding descriptor must raise a clean error, not crash the backend.
echo "-- invalid encoding descriptor"
make_pair "id int, v bigint"
load_pair "SELECT g, g*2 FROM generate_series(1,3000) g"
psql_run "UPDATE pgcolumnar.column_chunk SET encoding_descriptor = '\\\\xffffffff'::bytea
		  WHERE storage_id = pgcolumnar.get_storage_id('t_col') AND column_index = 1;"
badresult="$(q "SELECT * FROM t_col;")"
check "invalid descriptor raises error" "$([ -z "$badresult" ] && echo errored)" "errored"
check "backend alive after invalid descriptor" "$(q "SELECT 1;")" "1"

# A malformed bloom filter must be ignored, keeping results correct.
echo "-- malformed bloom filter"
make_pair "id int, k bigint"
q "SELECT pgcolumnar.alter_columnar_table_set('t_col', chunk_group_row_limit => 1000, stripe_row_limit => 20000);" >/dev/null
load_pair "SELECT g, ((g*2654435761)%100000)::bigint FROM generate_series(1,8000) g"
psql_run "UPDATE pgcolumnar.bloom SET filter = '\\\\x00'::bytea
		  WHERE storage_id = pgcolumnar.get_storage_id('t_col') AND column_index = 1;"
diff_query "corrupt bloom present" "SELECT id FROM %T WHERE k = ((7*2654435761)%100000)::bigint"
diff_query "corrupt bloom absent"  "SELECT count(*) FROM %T WHERE k = 424242424"
check "backend alive after corrupt bloom" "$(q "SELECT 1;")" "1"

pgc_summary
