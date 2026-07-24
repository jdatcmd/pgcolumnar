#!/usr/bin/env bash
#
# pgColumnar Parquet FDW predicate / row-group skipping (Phase G). The FDW reads
# each row group's min/max statistics and skips groups that a pushable col-op-const
# clause proves empty; the executor still rechecks every returned row, so a skip can
# only save work, never drop rows. This suite needs a statistics-bearing file, which
# our exporter does not write, so it uses pyarrow to produce a multi-row-group file
# with per-group stats. It asserts correctness against a heap oracle (skipping never
# changes results) AND that groups are actually skipped (via EXPLAIN ANALYZE's
# "Row Groups Skipped" counter).
#
# Usage:  test/native_parquet_pushdown.sh [PG_CONFIG]
# Written fresh for pgColumnar.

set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

if ! python3 -c 'import pyarrow.parquet' 2>/dev/null; then
	echo "SKIP  pyarrow not available; predicate-pushdown suite needs it to write stats"
	pgc_summary
	exit 0
fi

PARQ="$PGC_WORKDIR/stats.parquet"
# 200000 rows sorted by id, 4 row groups of 50000 -> disjoint id ranges
# [0,50000) [50000,100000) [100000,150000) [150000,200000), each with min/max stats.
python3 - "$PARQ" <<'PY'
import sys, pyarrow as pa, pyarrow.parquet as pq
n = 200000
ids = pa.array(range(n), pa.int32())
vals = pa.array([i * 1.5 for i in range(n)], pa.float64())
tbl = pa.table({"id": ids, "val": vals})
pq.write_table(tbl, sys.argv[1], row_group_size=50000)
t = pq.ParquetFile(sys.argv[1])
assert t.num_row_groups == 4, t.num_row_groups
PY

# heap oracle with identical data
psql_run "CREATE TABLE h (id int, val float8);"
psql_run "INSERT INTO h SELECT g, g*1.5 FROM generate_series(0,199999) g;"

psql_run "CREATE SERVER pq FOREIGN DATA WRAPPER pgcolumnar_parquet;"
psql_run "CREATE FOREIGN TABLE ft (id int4, val float8) SERVER pq OPTIONS (path '$PARQ');"

# ---- correctness: skipping never changes results (vs heap oracle) -----------
check "full scan == oracle" \
	"$(pgc_set_hash "SELECT * FROM ft")" "$(pgc_set_hash "SELECT * FROM h")"
for pred in \
	"id = 60000" \
	"id BETWEEN 60000 AND 70000" \
	"id < 100" \
	"id >= 199900" \
	"id > 60000 AND id < 60010" \
	"id <= 5 OR id >= 199995" \
	"val > 90000 AND val < 90030"
do
	check "predicate [$pred] == oracle" \
		"$(pgc_set_hash "SELECT * FROM ft WHERE $pred")" \
		"$(pgc_set_hash "SELECT * FROM h  WHERE $pred")"
done

# ---- skipping actually happens (EXPLAIN ANALYZE counter) --------------------
skipped_for() {  # skipped_for WHERE
	q "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF)
	   SELECT count(*) FROM ft WHERE $1" \
		| grep 'Row Groups Skipped' | grep -oE '[0-9]+' | head -1
}
skipped_for_t() {  # skipped_for_t TABLE WHERE
	q "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF)
	   SELECT count(*) FROM $1 WHERE $2" \
		| grep 'Row Groups Skipped' | grep -oE '[0-9]+' | head -1
}
groups_for() {  # groups_for WHERE  -> total groups reported
	q "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF)
	   SELECT count(*) FROM ft WHERE $1" \
		| grep 'Row Groups:' | grep -oE '[0-9]+' | head -1
}

check "reports 4 row groups" "$(groups_for 'id >= 0')" "4"
# a value in group 1 ([50000,100000)) -> other 3 groups skip
check "range in one group skips 3" "$(skipped_for 'id BETWEEN 60000 AND 70000')" "3"
# equality in group 0 -> 3 skipped
check "point in group 0 skips 3" "$(skipped_for 'id = 123')" "3"
# open range hitting only the last group -> 3 skipped
check "id >= 199900 skips 3" "$(skipped_for 'id >= 199900')" "3"
# open range hitting only the first group -> 3 skipped
check "id < 100 skips 3" "$(skipped_for 'id < 100')" "3"
# A float predicate confined to group 1 (val = id*1.5; [90000,90030] -> id in
# [60000,60020]). Only 2 groups skip, not 3: float >= and > are never pushed down,
# because Parquet excludes NaN from min/max while PostgreSQL sorts NaN above every
# value, so a NaN row can satisfy them while the stats look empty. The <= side
# still skips the two groups above the range.
check "float predicate skips 2 (>= not pushed, NaN-unsafe)" \
	"$(skipped_for 'val >= 90000 AND val <= 90030')" "2"
check "float < still skips" "$(skipped_for 'val < 1.0')" "3"
# a predicate spanning all groups -> 0 skipped
check "spanning predicate skips 0" "$(skipped_for 'id >= 0 AND id < 200000')" "0"
# no predicate -> 0 skipped
check "no predicate skips 0" "$(skipped_for 'true')" "0"

# ---- NaN soundness: skipping must never drop a NaN row ---------------------
# Parquet writers exclude NaN when computing min/max, so a group holding a NaN
# advertises ordinary finite bounds. PostgreSQL orders NaN above every value and
# treats NaN = NaN as true, so col > c, col >= c and col = 'NaN' can all match a
# row the statistics do not describe. Skipping on those would drop it silently,
# and the executor's recheck cannot recover a group that emitted nothing.
NANP="$PGC_WORKDIR/nan.parquet"
python3 - "$NANP" <<'PY'
import sys, pyarrow as pa, pyarrow.parquet as pq
vals = [float(i) for i in range(4000)]
vals[500] = float('nan')          # one NaN, in row group 0
tbl = pa.table({"id": pa.array(range(4000), pa.int32()),
                "val": pa.array(vals, pa.float64())})
pq.write_table(tbl, sys.argv[1], row_group_size=1000)
assert pq.ParquetFile(sys.argv[1]).num_row_groups == 4
PY

psql_run "CREATE FOREIGN TABLE ftn (id int4, val float8) SERVER pq OPTIONS (path '$NANP');"
psql_run "CREATE TABLE hn (id int, val float8);"
psql_run "INSERT INTO hn SELECT g, CASE WHEN g = 500 THEN 'NaN'::float8 ELSE g END
          FROM generate_series(0,3999) g;"

for pred in \
	"val > 1e300" \
	"val >= 1e300" \
	"val = 'NaN'::float8" \
	"val > 3998" \
	"val < 10" \
	"val <= 10" \
	"val = 1234"
do
	check "NaN-safe [$pred] == oracle" \
		"$(pgc_set_hash "SELECT * FROM ftn WHERE $pred")" \
		"$(pgc_set_hash "SELECT * FROM hn  WHERE $pred")"
done

# ---- inverted statistics must never drive a skip ---------------------------
# A PG int2 column binds to a Parquet INT32 column, and the decoder narrows with
# an unchecked cast. A group holding 30000 and 40000 therefore decodes to
# min=30000, max=-25536: an inverted interval. Every skip test assumes min <= max,
# so without a guard the equality test skips a group that really does contain the
# row. The data path applies the same narrowing, so those rows are genuine
# matches. (Parquet UINT_32/UINT_64 invert the same way, being unsigned-ordered.)
WIDEP="$PGC_WORKDIR/wide.parquet"
python3 - "$WIDEP" <<'PY'
import sys, pyarrow as pa, pyarrow.parquet as pq
pq.write_table(pa.table({"c": pa.array([30000, 40000], pa.int32())}),
               sys.argv[1], row_group_size=2)
PY

psql_run "CREATE FOREIGN TABLE ftw (c int2) SERVER pq OPTIONS (path '$WIDEP');"
# oracle: exactly what the narrowing read path yields for those two INT32 values
psql_run "CREATE TABLE hw (c int2);"
psql_run "INSERT INTO hw VALUES (30000::int2), ((-25536)::int2);"

for pred in \
	"c = 30000::int2" \
	"c = (-25536)::int2" \
	"c >= 30000::int2" \
	"c <= (-25536)::int2" \
	"c < 0::int2" \
	"c > 0::int2"
do
	check "inverted-stats [$pred] == oracle" \
		"$(pgc_set_hash "SELECT * FROM ftw WHERE $pred")" \
		"$(pgc_set_hash "SELECT * FROM hw  WHERE $pred")"
done
check "inverted stats skip nothing" "$(skipped_for_t ftw 'c = 30000::int2')" "0"

# ---- read_parquet over the same file ignores stats (reads all) but matches --
check "read_parquet same file == oracle" \
	"$(pgc_set_hash "SELECT * FROM pgcolumnar.read_parquet('$PARQ') AS t(id int4, val float8)")" \
	"$(pgc_set_hash "SELECT * FROM h")"

pgc_summary
