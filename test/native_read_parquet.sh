#!/usr/bin/env bash
#
# pgColumnar external-Parquet read (Phase G). pgcolumnar.read_parquet(path) streams
# a server-side Parquet file's rows in place, with a caller-supplied column
# definition list. This suite is the round-trip differential: it exports a columnar
# table (scalars, then nested int[]/text[]/composite), reads the file back with
# read_parquet, and asserts the rows are identical to the source. The exporter and
# the scan core share the decode, so a faithful reader reproduces the source rows.
# It also checks the argument-validation errors (no column list, wrong count/type).
#
# Usage:  test/native_read_parquet.sh [PG_CONFIG]
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

# ---- scalar round-trip ------------------------------------------------------
# The shared decoder supports the pq_want_phys type set (int/float/bool/text/bytea/
# date/time/timestamp). uuid and numeric(DECIMAL) are reported by parquet_schema but
# not yet decodable by import/read_parquet -- a shared-decoder follow-up.
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
# an all-NULL row so nullability round-trips
psql_run "INSERT INTO s VALUES (NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL);"

check "scalar export rows" "$(q "SELECT pgcolumnar.export_parquet('s', '$SCALAR');")" "5001"

# read the file back and hash-compare (order-independent) against the source table
rp_scalar() { pgc_set_hash "SELECT * FROM pgcolumnar.read_parquet('$SCALAR')
	AS t($scalar_cols)"; }
src_scalar() { pgc_set_hash "SELECT * FROM s"; }
check "scalar round-trip: read_parquet == source" "$(rp_scalar)" "$(src_scalar)"
check "scalar read_parquet row count" \
	"$(q "SELECT count(*) FROM pgcolumnar.read_parquet('$SCALAR') AS t($scalar_cols)")" "5001"

# a projection over the function still returns the projected column faithfully
check "projected column round-trips" \
	"$(pgc_set_hash "SELECT c_text FROM pgcolumnar.read_parquet('$SCALAR') AS t($scalar_cols)")" \
	"$(pgc_set_hash "SELECT c_text FROM s")"

# ---- nested round-trip (int[], text[], composite) ---------------------------
psql_run "CREATE TYPE rp_npc AS (x int, y text);"
psql_run "CREATE TABLE np (id int, ia int[], ta text[], c rp_npc) USING pgcolumnar;"
psql_run "INSERT INTO np
	SELECT g,
	       CASE WHEN g%11=0 THEN NULL WHEN g%7=0 THEN '{}'::int[]
	            ELSE ARRAY[g, g+1, g+2] END,
	       CASE WHEN g%13=0 THEN NULL ELSE ARRAY['a'||g, 'b'||g] END,
	       CASE WHEN g%5=0 THEN NULL ELSE ROW(g, 'c'||g)::rp_npc END
	FROM generate_series(1, 3000) g;"
NESTED="$PGC_WORKDIR/nested.parquet"
check "nested export rows" "$(q "SELECT pgcolumnar.export_parquet('np', '$NESTED');")" "3000"

nested_cols="id int, ia int[], ta text[], c rp_npc"
check "nested round-trip: read_parquet == source" \
	"$(pgc_set_hash "SELECT * FROM pgcolumnar.read_parquet('$NESTED') AS t($nested_cols)")" \
	"$(pgc_set_hash "SELECT * FROM np")"

# ---- argument validation ----------------------------------------------------
# no column definition list -> record-returning function has no shape
expect_error "reject missing column definition list" \
	"SELECT * FROM pgcolumnar.read_parquet('$SCALAR')"

# too few columns declared (file has 11 leaves)
expect_error "reject fewer columns than file" \
	"SELECT * FROM pgcolumnar.read_parquet('$SCALAR') AS t(c_i2 int2)"

# incompatible declared type (c_i4 is INT32, not text)
expect_error "reject incompatible declared type" \
	"SELECT * FROM pgcolumnar.read_parquet('$SCALAR')
	 AS t(c_i2 int2, c_i4 text, c_i8 int8, c_f4 float4, c_f8 float8, c_bool bool,
	      c_text text, c_bytea bytea, c_date date, c_time time, c_ts timestamp)"

# not a Parquet file
printf 'not parquet' > "$PGC_WORKDIR/bad.dat"
chmod 644 "$PGC_WORKDIR/bad.dat"
expect_error "reject non-parquet file" \
	"SELECT * FROM pgcolumnar.read_parquet('$PGC_WORKDIR/bad.dat') AS t(a int)"

# ---- import parity: read_parquet == import_parquet of the same file ---------
psql_run "CREATE TABLE imp ($scalar_cols) USING pgcolumnar;"
check "import parity rows" "$(q "SELECT pgcolumnar.import_parquet('imp', '$SCALAR');")" "5001"
check "read_parquet == import_parquet" \
	"$(rp_scalar)" "$(pgc_set_hash "SELECT * FROM imp")"

pgc_summary
