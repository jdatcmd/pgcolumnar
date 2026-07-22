#!/usr/bin/env bash
#
# pgColumnar native round-trip (Phase D3): a native-format (PGCN v1) table is
# read back through the native reader and must match a heap mirror. This is the
# first read of native data. Insert-only (delete/update visibility on native
# tables is a later sub-phase). Scalars with nulls across several row groups,
# with a filter and a projection, compared by an order-independent set hash so
# heap is the correctness oracle.
#
# Usage:  test/native_roundtrip.sh [PG_CONFIG]
# Written fresh for pgColumnar.

set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

# heap mirror and a native columnar table with the same rows; small row-group
# limit so the read crosses several row groups.
psql_run "CREATE TABLE h (id int, k bigint, v text, f float8, b bool);"
psql_run "CREATE TABLE n (id int, k bigint, v text, f float8, b bool) USING pgcolumnar;"
psql_run "SELECT pgcolumnar.alter_columnar_table_set('n', stripe_row_limit => 1000);"
GEN="SELECT g,
       (g * 100000)::bigint,
       CASE WHEN g % 9 = 0 THEN NULL ELSE 'row-' || g END,
       CASE WHEN g % 6 = 0 THEN NULL ELSE (g::float8 / 7) END,
       (g % 2 = 0)
  FROM generate_series(1, 5000) g"
psql_run "INSERT INTO h $GEN;"
psql_run "INSERT INTO n $GEN;"

check "native row count" "$(q 'SELECT count(*) FROM n;')" "5000"
check "native reads back all columns" \
	"$(pgc_set_hash 'SELECT id, k, v, f, b FROM n')" \
	"$(pgc_set_hash 'SELECT id, k, v, f, b FROM h')"
check "native null handling (v)" \
	"$(q 'SELECT count(*) FROM n WHERE v IS NULL;')" \
	"$(q 'SELECT count(*) FROM h WHERE v IS NULL;')"
check "native null handling (f)" \
	"$(q 'SELECT count(*) FROM n WHERE f IS NULL;')" \
	"$(q 'SELECT count(*) FROM h WHERE f IS NULL;')"
check "native filtered scan" \
	"$(pgc_set_hash 'SELECT id, v FROM n WHERE k BETWEEN 100000000 AND 200000000')" \
	"$(pgc_set_hash 'SELECT id, v FROM h WHERE k BETWEEN 100000000 AND 200000000')"
check "native projection (subset of columns)" \
	"$(pgc_set_hash 'SELECT v, b FROM n')" \
	"$(pgc_set_hash 'SELECT v, b FROM h')"
check "native aggregate (sum over a scanned column)" \
	"$(q 'SELECT sum(k) FROM n;')" \
	"$(q 'SELECT sum(k) FROM h;')"
check "native count with filter" \
	"$(q 'SELECT count(*) FROM n WHERE b;')" \
	"$(q 'SELECT count(*) FROM h WHERE b;')"

pgc_summary
