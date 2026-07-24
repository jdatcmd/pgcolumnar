#!/usr/bin/env bash
#
# pgColumnar Parquet column-projection pushdown (Phase G).
#
# The FDW scan now decodes only the column chunks the query references; an
# unreferenced column's chunk is never decompressed or decoded. This suite checks
# that projected results are correct against a heap oracle, that the EXPLAIN
# "Columns Read" counter reflects the projection (including 0 for count(*) and all
# for SELECT *), and -- the load-bearing proof -- that a column which would error
# on decode is not read when it is projected out.
#
# Usage:  test/native_parquet_projection.sh [PG_CONFIG]
# Written fresh for pgColumnar.

set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

if ! python3 -c 'import pyarrow.parquet' 2>/dev/null; then
	echo "SKIP  pyarrow not available; projection suite needs it"
	pgc_summary
	exit 0
fi

W="$PGC_WORKDIR"
psql_run "CREATE SERVER pq FOREIGN DATA WRAPPER pgcolumnar_parquet;"

python3 - "$W" <<'PY'
import sys, pyarrow as pa, pyarrow.parquet as pq
W = sys.argv[1]
n = 5000
pq.write_table(pa.table({
    "a": pa.array(range(n), pa.int32()),
    "b": pa.array([i * 2 for i in range(n)], pa.int64()),
    "c": pa.array([f"s{i%20}" for i in range(n)]),
    "d": pa.array([i / 3.0 for i in range(n)], pa.float64()),
}), f"{W}/proj.parquet", row_group_size=1000)
PY

cols="a int, b int8, c text, d float8"
psql_run "CREATE FOREIGN TABLE ft ($cols) SERVER pq OPTIONS (path '$W/proj.parquet');"
psql_run "CREATE TABLE h ($cols);"
psql_run "INSERT INTO h SELECT * FROM pgcolumnar.read_parquet('$W/proj.parquet') AS t($cols);"

# ---- correctness: projected columns match the oracle -----------------------
for sel in "a" "c" "a, d" "b, c, d" "*"; do
	check "projection [$sel] == oracle" \
		"$(pgc_set_hash "SELECT $sel FROM ft")" "$(pgc_set_hash "SELECT $sel FROM h")"
done
# a qual column that is not selected must still be read for the recheck
check "qual on unselected column == oracle" \
	"$(pgc_set_hash "SELECT a FROM ft WHERE c = 's7'")" \
	"$(pgc_set_hash "SELECT a FROM h  WHERE c = 's7'")"

# ---- the EXPLAIN counter reflects the projection ---------------------------
cols_read() {  # cols_read QUERY
	q "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) $1" \
		| grep 'Columns Read:' | grep -oE '[0-9]+' | head -1
}
check "one column reads 1" "$(cols_read "SELECT a FROM ft")" "1"
check "two columns read 2" "$(cols_read "SELECT a, d FROM ft")" "2"
check "select star reads 4" "$(cols_read "SELECT * FROM ft")" "4"
check "count(*) reads 0"    "$(cols_read "SELECT count(*) FROM ft")" "0"
# a qual column counts as read even when not selected
check "selected 1 + qual 1 reads 2" "$(cols_read "SELECT a FROM ft WHERE c = 's7'")" "2"

# ---- proof that a projected-out column is not decoded -----------------------
# The counter reflects the decision; this proves the decode actually skips. A
# two-column file whose SECOND column's data page is corrupted: reading only the
# first column must succeed (the second is never decoded), while reading the
# second must fail. If projection did not skip decode, selecting only the first
# would also hit the corruption.
python3 - "$W" <<'PY'
import sys, pyarrow as pa, pyarrow.parquet as pq
W = sys.argv[1]
pq.write_table(pa.table({"g": pa.array(range(1000), pa.int32()),
                         "bad": pa.array(range(1000), pa.int32())}),
               f"{W}/corrupt.parquet", row_group_size=1000, use_dictionary=False)
md = pq.ParquetFile(f"{W}/corrupt.parquet").metadata.row_group(0).column(1)
off = md.data_page_offset
raw = bytearray(open(f"{W}/corrupt.parquet", "rb").read())
# smash the second column's page header + start of its data so decode fails,
# leaving the first column and the footer intact.
for i in range(off, off + 32):
    raw[i] = 0xFF
open(f"{W}/corrupt.parquet", "wb").write(bytes(raw))
print(f"  corrupted column 'bad' at offset {off}", file=sys.stderr)
PY

psql_run "CREATE FOREIGN TABLE ftc (g int, bad int) SERVER pq OPTIONS (path '$W/corrupt.parquet');"
psql_run "CREATE FUNCTION pgc_try_proj(q text) RETURNS text LANGUAGE plpgsql AS \$\$
          BEGIN EXECUTE q; RETURN 'OK';
          EXCEPTION WHEN OTHERS THEN RETURN 'ERROR'; END \$\$;"
tryq() { q "SELECT pgc_try_proj(\$q\$$1\$q\$);"; }

check "selecting the good column skips the corrupt one" \
	"$(tryq "SELECT count(g) FROM ftc")" "OK"
check "count(*) decodes nothing, survives corruption" \
	"$(tryq "SELECT count(*) FROM ftc")" "OK"
check "selecting the corrupt column does error" \
	"$(tryq "SELECT count(bad) FROM ftc")" "ERROR"
check "good-column values are still correct" \
	"$(q 'SELECT sum(g)::text FROM ftc;')" "499500"

pgc_summary
