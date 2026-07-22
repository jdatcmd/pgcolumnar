#!/usr/bin/env bash
#
# pgColumnar native writer: a columnar table writes the native format (PGCN v1).
# This validates the writer by its catalog output: the pgcolumnar.storage /
# row_group / column_chunk rows it produces, and that dropping the table clears
# them. It does not SELECT data from the table (see native_roundtrip.sh).
#
# Usage:  test/native_writer.sh [PG_CONFIG]
# Written fresh for pgColumnar.

set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

# A columnar table with a small row-group limit so several row groups are written.
psql_run "CREATE TABLE nw (id int, v text) USING pgcolumnar;"
psql_run "SELECT pgcolumnar.alter_columnar_table_set('nw', stripe_row_limit => 1000);"
psql_run "INSERT INTO nw SELECT g, CASE WHEN g % 4 = 0 THEN NULL ELSE 'v'||g END FROM generate_series(1, 5000) g;"

SID="$(q "SELECT pgcolumnar.get_storage_id('nw');")"
check "native storage row written" \
	"$(q "SELECT count(*) FROM pgcolumnar.storage WHERE storage_id = $SID;")" "1"
check "native format version recorded" \
	"$(q "SELECT format_version FROM pgcolumnar.storage WHERE storage_id = $SID;")" "1"
check "native vector length recorded" \
	"$(q "SELECT vector_length FROM pgcolumnar.storage WHERE storage_id = $SID;")" "1024"
check "native row groups (5000 rows / 1000 limit)" \
	"$(q "SELECT count(*) FROM pgcolumnar.row_group WHERE storage_id = $SID;")" "5"
check "native row-group row total" \
	"$(q "SELECT sum(row_count) FROM pgcolumnar.row_group WHERE storage_id = $SID;")" "5000"
check "native column chunks (5 groups x 2 cols)" \
	"$(q "SELECT count(*) FROM pgcolumnar.column_chunk WHERE storage_id = $SID;")" "10"
check "native column-chunk value total" \
	"$(q "SELECT sum(value_count) FROM pgcolumnar.column_chunk WHERE storage_id = $SID;")" "10000"

# Drop cleanup removes the native catalog rows for the storage.
psql_run "DROP TABLE nw;"
check "drop clears native storage row" \
	"$(q "SELECT count(*) FROM pgcolumnar.storage WHERE storage_id = $SID;")" "0"
check "drop clears native row groups" \
	"$(q "SELECT count(*) FROM pgcolumnar.row_group WHERE storage_id = $SID;")" "0"
check "drop clears native column chunks" \
	"$(q "SELECT count(*) FROM pgcolumnar.column_chunk WHERE storage_id = $SID;")" "0"

pgc_summary
