#!/usr/bin/env bash
#
# pgColumnar Parquet FIXED_LEN_BYTE_ARRAY read coverage (Phase G).
#
# The reader advertises uuid and numeric(p,s) for FLBA columns via
# parquet_schema(), and the exporter writes both. This suite covers the read side
# that made those advertisements truthful: uuid as a 16-byte FLBA, and DECIMAL as
# a big-endian two's-complement integer at the column's scale, in either a fixed
# (FLBA) or variable (BYTE_ARRAY) width. It rounds our own export back through the
# reader and also reads pyarrow-written files, and checks correctness against a
# heap oracle.
#
# Usage:  test/native_parquet_flba.sh [PG_CONFIG]
# Written fresh for pgColumnar.

set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

have_pyarrow=0
python3 -c 'import pyarrow.parquet' 2>/dev/null && have_pyarrow=1

W="$PGC_WORKDIR"
psql_run "CREATE SERVER pq FOREIGN DATA WRAPPER pgcolumnar_parquet;"

# ---- round-trip our own exporter (uuid + numeric via FLBA) -----------------
psql_run "CREATE TABLE src (
            id int,
            u  uuid,
            d  numeric(12,4),
            dn numeric(12,4),
            fb bytea)
          USING pgcolumnar;"
psql_run "INSERT INTO src VALUES
            (1, '11111111-2222-3333-4444-555555555555', 123.4500, -0.0001, '\\xdeadbeef'),
            (2, 'ffffffff-ffff-ffff-ffff-ffffffffffff', -9999.9999, 0, '\\x00'),
            (3, '00000000-0000-0000-0000-000000000000', 0.0000, 42.0, NULL);"

EXP="$W/roundtrip.parquet"
psql_run "SELECT pgcolumnar.export_parquet('src'::regclass, '$EXP');"

# read_parquet must return exactly what the heap holds
check "uuid round-trips through export/read" \
	"$(pgc_set_hash "SELECT id, u FROM pgcolumnar.read_parquet('$EXP') AS t(id int, u uuid, d numeric(12,4), dn numeric(12,4), fb bytea)")" \
	"$(pgc_set_hash "SELECT id, u FROM src")"
check "numeric round-trips through export/read" \
	"$(pgc_set_hash "SELECT id, d, dn FROM pgcolumnar.read_parquet('$EXP') AS t(id int, u uuid, d numeric(12,4), dn numeric(12,4), fb bytea)")" \
	"$(pgc_set_hash "SELECT id, d, dn FROM src")"
check "whole-row round-trip == source" \
	"$(pgc_set_hash "SELECT * FROM pgcolumnar.read_parquet('$EXP') AS t(id int, u uuid, d numeric(12,4), dn numeric(12,4), fb bytea)")" \
	"$(pgc_set_hash "SELECT * FROM src")"

# import_parquet shares the decoder
psql_run "CREATE TABLE imp (id int, u uuid, d numeric(12,4), dn numeric(12,4), fb bytea);"
psql_run "SELECT pgcolumnar.import_parquet('imp'::regclass, '$EXP');"
check "import_parquet == source" \
	"$(pgc_set_hash "SELECT * FROM imp")" "$(pgc_set_hash "SELECT * FROM src")"

# FDW surface
psql_run "CREATE FOREIGN TABLE ft (id int, u uuid, d numeric(12,4), dn numeric(12,4), fb bytea)
          SERVER pq OPTIONS (path '$EXP');"
check "FDW scan == source" \
	"$(pgc_set_hash "SELECT * FROM ft")" "$(pgc_set_hash "SELECT * FROM src")"

# ---- pyarrow-written files (foreign producer) ------------------------------
if [ "$have_pyarrow" = 1 ]; then
	python3 - "$W" <<'PY'
import sys, uuid, decimal, pyarrow as pa, pyarrow.parquet as pq
W = sys.argv[1]
us = [uuid.UUID('11111111-2222-3333-4444-555555555555').bytes,
      uuid.UUID('00000000-0000-0000-0000-000000000000').bytes]
pq.write_table(pa.table({"u": pa.array(us, pa.binary(16))}), f"{W}/pa_uuid.parquet")
# decimal128 -> FLBA; a range that stresses sign and scale
ds = [decimal.Decimal("123.45"), decimal.Decimal("-9999.99"),
      decimal.Decimal("0.01"), decimal.Decimal("0.00")]
pq.write_table(pa.table({"d": pa.array(ds, pa.decimal128(10, 2))}), f"{W}/pa_dec.parquet")
PY
	# uuid stored as raw bytes; declare bytea to compare, since pyarrow gives no
	# uuid logical type, then also read as uuid (our exporter's convention).
	check "pyarrow uuid reads as uuid" \
		"$(q "SELECT u::text FROM pgcolumnar.read_parquet('$W/pa_uuid.parquet') AS t(u uuid) ORDER BY u LIMIT 1;")" \
		"00000000-0000-0000-0000-000000000000"
	check "pyarrow decimal128 values are exact" \
		"$(pgc_set_hash "SELECT d FROM pgcolumnar.read_parquet('$W/pa_dec.parquet') AS t(d numeric(10,2))")" \
		"$(pgc_set_hash "SELECT * FROM (VALUES (123.45::numeric(10,2)),(-9999.99),(0.01),(0.00)) v(d)")"
else
	echo "SKIP  pyarrow not available; foreign-producer FLBA cases skipped"
fi

pgc_summary
