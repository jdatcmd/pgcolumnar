#!/usr/bin/env bash
#
# pgColumnar Parquet foreign-data wrapper (Phase G). The pgcolumnar_parquet FDW
# exposes a single Parquet file as a foreign table, binding its column definitions
# against the file like read_parquet's column list and producing rows through the
# same scan core. This suite exports a columnar table, reads it back through a
# foreign table, and asserts identity with the source across full scan, projection,
# predicate (executor-applied), self-join (rescan), and a nested-type table. It also
# checks EXPLAIN shows a Foreign Scan and the option validator/scan errors.
#
# Usage:  test/native_parquet_fdw.sh [PG_CONFIG]
# Written fresh for pgColumnar.

set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

expect_error() {  # expect_error LABEL SQL
	local label="$1" sql="$2"
	if psql_run "$sql" >/dev/null 2>&1; then
		check "$label (expected error)" "succeeded" "error"
	else
		check "$label" "error" "error"
	fi
}

psql_run "CREATE SERVER pqsrv FOREIGN DATA WRAPPER pgcolumnar_parquet;"

# ---- scalar foreign table ---------------------------------------------------
SCALAR="$PGC_WORKDIR/scalar.parquet"
scalar_cols="c_i2 int2, c_i4 int4, c_i8 int8, c_f4 float4, c_f8 float8, c_bool bool,
             c_text text, c_bytea bytea, c_date date, c_time time, c_ts timestamp"

psql_run "CREATE TABLE s ($scalar_cols) USING pgcolumnar;"
psql_run "INSERT INTO s
	SELECT (g%100)::int2, g, g::int8*1000, (g/2.0)::float4, (g/4.0)::float8, (g%2=0),
	       'row'||g, decode(lpad(to_hex(g),8,'0'),'hex'), date '2020-01-01' + g,
	       time '00:00:00' + (g||' seconds')::interval,
	       timestamp '2020-01-01' + (g||' minutes')::interval
	FROM generate_series(1, 5000) g;"
psql_run "INSERT INTO s VALUES (NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL);"
check "scalar export rows" "$(q "SELECT pgcolumnar.export_parquet('s', '$SCALAR');")" "5001"

psql_run "CREATE FOREIGN TABLE fs ($scalar_cols) SERVER pqsrv OPTIONS (path '$SCALAR');"

check "full scan: fdw == source" \
	"$(pgc_set_hash "SELECT * FROM fs")" "$(pgc_set_hash "SELECT * FROM s")"
check "fdw row count" "$(q "SELECT count(*) FROM fs")" "5001"

# projection (executor discards other columns)
check "projection: fdw == source" \
	"$(pgc_set_hash "SELECT c_text FROM fs")" "$(pgc_set_hash "SELECT c_text FROM s")"

# predicate applied by the executor
check "predicate: fdw == source" \
	"$(pgc_set_hash "SELECT c_i4, c_text FROM fs WHERE c_i4 > 2500 AND c_bool")" \
	"$(pgc_set_hash "SELECT c_i4, c_text FROM s  WHERE c_i4 > 2500 AND c_bool")"

# self-join forces the scan to be re-executed (ReScan); result must match source
psql_run "SET enable_hashjoin=off; SET enable_mergejoin=off;"
check "nested-loop self-join (rescan): fdw == source" \
	"$(q "SELECT count(*) FROM fs a JOIN fs b ON a.c_i4 = b.c_i4 WHERE a.c_i4 < 200")" \
	"$(q "SELECT count(*) FROM s  a JOIN s  b ON a.c_i4 = b.c_i4 WHERE a.c_i4 < 200")"
psql_run "RESET enable_hashjoin; RESET enable_mergejoin;"

# the plan is a Foreign Scan
check "EXPLAIN shows Foreign Scan" \
	"$(q "EXPLAIN (COSTS OFF) SELECT * FROM fs" | grep -c 'Foreign Scan')" "1"

# ---- nested foreign table (int[], text[], composite) ------------------------
psql_run "CREATE TYPE fdw_npc AS (x int, y text);"
psql_run "CREATE TABLE np (id int, ia int[], ta text[], c fdw_npc) USING pgcolumnar;"
psql_run "INSERT INTO np
	SELECT g,
	       CASE WHEN g%11=0 THEN NULL WHEN g%7=0 THEN '{}'::int[] ELSE ARRAY[g,g+1] END,
	       CASE WHEN g%13=0 THEN NULL ELSE ARRAY['a'||g,'b'||g] END,
	       CASE WHEN g%5=0 THEN NULL ELSE ROW(g,'c'||g)::fdw_npc END
	FROM generate_series(1, 3000) g;"
NESTED="$PGC_WORKDIR/nested.parquet"
check "nested export rows" "$(q "SELECT pgcolumnar.export_parquet('np', '$NESTED');")" "3000"
psql_run "CREATE FOREIGN TABLE fnp (id int, ia int[], ta text[], c fdw_npc)
	SERVER pqsrv OPTIONS (path '$NESTED');"
check "nested full scan: fdw == source" \
	"$(pgc_set_hash "SELECT * FROM fnp")" "$(pgc_set_hash "SELECT * FROM np")"

# ---- option validation + scan errors ----------------------------------------
expect_error "reject invalid table option" \
	"CREATE FOREIGN TABLE fbad ($scalar_cols) SERVER pqsrv OPTIONS (path '$SCALAR', bogus 'x');"
expect_error "reject empty path option" \
	"CREATE FOREIGN TABLE fempty ($scalar_cols) SERVER pqsrv OPTIONS (path '');"

# a foreign table with no path errors at scan time
psql_run "CREATE FOREIGN TABLE fnopath ($scalar_cols) SERVER pqsrv;"
expect_error "scan without path option errors" "SELECT * FROM fnopath"

# a path that is not a Parquet file errors at scan time
printf 'not parquet' > "$PGC_WORKDIR/bad.dat"
chmod 644 "$PGC_WORKDIR/bad.dat"
psql_run "CREATE FOREIGN TABLE fbadfile ($scalar_cols) SERVER pqsrv OPTIONS (path '$PGC_WORKDIR/bad.dat');"
expect_error "scan of non-parquet file errors" "SELECT * FROM fbadfile"

# a declared type incompatible with the file column errors at scan time
psql_run "CREATE FOREIGN TABLE fwrongtype (c_i2 int2, c_i4 text, c_i8 int8, c_f4 float4,
	c_f8 float8, c_bool bool, c_text text, c_bytea bytea, c_date date, c_time time,
	c_ts timestamp) SERVER pqsrv OPTIONS (path '$SCALAR');"
expect_error "scan with incompatible declared type errors" "SELECT * FROM fwrongtype"

pgc_summary
