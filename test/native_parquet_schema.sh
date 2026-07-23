#!/usr/bin/env bash
#
# pgColumnar external-Parquet scan core (Phase G). pgcolumnar.parquet_schema(path)
# reads a server-side Parquet file's footer and reports its leaf columns with the
# PostgreSQL type each maps to. This suite is the round-trip differential: it
# exports a columnar table covering every type the Parquet exporter emits, then
# asserts parquet_schema reports the source column types back. The exporter and
# the scan core share the type mapping, so a faithful mapping is a byte-for-byte
# inverse.
#
# A pyarrow-guarded tail writes a file with a REQUIRED (non-null) column to
# confirm the nullable flag reports both ways (the exporter only ever writes
# OPTIONAL columns).
#
# Usage:  test/native_parquet_schema.sh [PG_CONFIG]
# Written fresh for pgColumnar.

set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

PARQUET="$PGC_WORKDIR/schema.parquet"

psql_run "CREATE TABLE n (
	c_i2  int2,
	c_i4  int4,
	c_i8  int8,
	c_f4  float4,
	c_f8  float8,
	c_bool bool,
	c_text text,
	c_bytea bytea,
	c_date date,
	c_time time,
	c_ts  timestamp,
	c_uuid uuid,
	c_num numeric(10,2)
) USING pgcolumnar;"

# one populated row plus an all-NULL row (columns are exported OPTIONAL either way)
psql_run "INSERT INTO n VALUES
	(2::int2, 3, 4::int8, 1.5::float4, 2.5::float8, true, 'hi',
	 '\\xdeadbeef'::bytea, '2020-01-01'::date, '12:34:56'::time,
	 '2020-01-01 12:34:56'::timestamp,
	 '11111111-1111-1111-1111-111111111111'::uuid, 123.45::numeric(10,2)),
	(NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);"

rows="$(q "SELECT pgcolumnar.export_parquet('n', '$PARQUET');")"
check "export_parquet rows written" "$rows" "2"

# leaf-column count matches the table
check "parquet_schema column count" \
	"$(q "SELECT count(*) FROM pgcolumnar.parquet_schema('$PARQUET');")" "13"

# name-keyed so the assertion does not depend on result ordering. Each entry is
# "column_name|expected_data_type" (all exporter columns are nullable=t).
assert_col() {  # assert_col NAME EXPECTED_TYPE
	local name="$1" want="$2" got
	got="$(q "SELECT data_type FROM pgcolumnar.parquet_schema('$PARQUET') WHERE column_name = '$name';")"
	check "type of $name" "$got" "$want"
	got="$(q "SELECT nullable FROM pgcolumnar.parquet_schema('$PARQUET') WHERE column_name = '$name';")"
	check "nullable of $name" "$got" "t"
}

assert_col c_i2   "smallint"
assert_col c_i4   "integer"
assert_col c_i8   "bigint"
assert_col c_f4   "real"
assert_col c_f8   "double precision"
assert_col c_bool "boolean"
assert_col c_text "text"
assert_col c_bytea "bytea"
assert_col c_date "date"
assert_col c_time "time without time zone"
assert_col c_ts   "timestamp without time zone"
assert_col c_uuid "uuid"
assert_col c_num  "numeric(10,2)"

# unsupported / corrupt inputs are rejected, not silently empty
notpq="$PGC_WORKDIR/not.parquet"
psql_run "SELECT 1;" >/dev/null   # noop to keep ordering readable
printf 'this is not a parquet file at all' > "$notpq"
chmod 644 "$notpq"
err="$(q "SELECT count(*) FROM pgcolumnar.parquet_schema('$notpq');" 2>&1)"
check "bad magic rejected (empty At output)" "$err" ""

# ---- nullable=false path (needs a REQUIRED column; only pyarrow writes one) ---
if python3 -c 'import pyarrow.parquet' 2>/dev/null; then
	REQ="$PGC_WORKDIR/required.parquet"
	python3 - "$REQ" <<'PY'
import sys, pyarrow as pa, pyarrow.parquet as pq
schema = pa.schema([
    pa.field("req_i", pa.int32(), nullable=False),
    pa.field("opt_s", pa.string(), nullable=True),
])
tbl = pa.table({"req_i": pa.array([1, 2, 3], pa.int32()),
                "opt_s": pa.array(["a", None, "c"], pa.string())}, schema=schema)
pq.write_table(tbl, sys.argv[1])
PY
	check "REQUIRED column reports nullable=f" \
		"$(q "SELECT nullable FROM pgcolumnar.parquet_schema('$REQ') WHERE column_name='req_i';")" "f"
	check "OPTIONAL column reports nullable=t" \
		"$(q "SELECT nullable FROM pgcolumnar.parquet_schema('$REQ') WHERE column_name='opt_s';")" "t"
	check "REQUIRED int32 maps to integer" \
		"$(q "SELECT data_type FROM pgcolumnar.parquet_schema('$REQ') WHERE column_name='req_i';")" "integer"
else
	echo "SKIP  pyarrow not available; REQUIRED-column nullable check skipped"
fi

pgc_summary
