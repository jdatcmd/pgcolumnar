#!/usr/bin/env bash
#
# pgColumnar nested-type Parquet import round-trip (gap 27): export a table with
# array and composite columns to Parquet, import it back into a fresh table, and
# assert the two tables are identical (arrays reconstructed from the LIST
# repetition/definition levels, composites from the group field leaves, including
# NULL arrays/elements/structs and empty arrays). A second case imports a nested
# Parquet file written by pyarrow, to prove interop beyond our own writer.
#
# Usage:  test/parquet_nested_import.sh [PG_CONFIG]
# Written fresh for pgColumnar; it does not reuse any upstream test file.

set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

PF="$PGC_WORKDIR/ni.parquet"
psql_run "CREATE TYPE pnic AS (x int, y text);"
psql_run "CREATE TABLE pna (id int, ia int[], ta text[], c pnic) USING columnar;"
psql_run "INSERT INTO pna SELECT g,
    CASE WHEN g % 10 = 0 THEN NULL
         WHEN g % 7 = 0 THEN '{}'::int[]
         ELSE ARRAY[g, NULL, g + 3] END,
    ARRAY['a' || g, 'b' || g],
    CASE WHEN g % 5 = 0 THEN NULL ELSE ROW(g * 2, 'c' || g)::pnic END
  FROM generate_series(1, 5000) g;"

check "export rows" "$(q "SELECT columnar.export_parquet('pna', '$PF');")" "5000"
psql_run "CREATE TABLE pna2 (id int, ia int[], ta text[], c pnic) USING columnar;"
check "import rows" "$(q "SELECT columnar.import_parquet('pna2', '$PF');")" "5000"

# the reconstructed table must equal the original (canonical text of every column)
SEL="SELECT id, ia::text, ta::text, c::text FROM"
check "nested import round-trips (arrays + composite)" \
	"$(pgc_set_hash "$SEL pna2")" \
	"$(pgc_set_hash "$SEL pna")"
check "import count matches" "$(q 'SELECT count(*) FROM pna2;')" "$(q 'SELECT count(*) FROM pna;')"

# after a delete on the source, a fresh export+import still round-trips the live set
psql_run "DELETE FROM pna WHERE id BETWEEN 1000 AND 2000;"
psql_run "SELECT columnar.export_parquet('pna', '$PF');"
psql_run "TRUNCATE pna2;"
psql_run "SELECT columnar.import_parquet('pna2', '$PF');"
check "round-trips after delete" \
	"$(pgc_set_hash "$SEL pna2")" \
	"$(pgc_set_hash "$SEL pna")"

# ---------------------------------------------------------------------------
# pyarrow-written nested Parquet file: list<int32> and struct<x:int,y:string>.
# This proves we read the Dremel levels the reference writer produces, not just
# our own exporter's byte layout.
# ---------------------------------------------------------------------------
PYF="$PGC_WORKDIR/pyarrow_nested.parquet"
if python3 - "$PYF" <<'PY'
import sys
try:
    import pyarrow as pa
    import pyarrow.parquet as pq
except Exception:
    sys.exit(3)
ids = list(range(1, 201))
ia = [None if g % 10 == 0 else ([] if g % 7 == 0 else [g, None, g + 3]) for g in ids]
c = [None if g % 5 == 0 else {"x": g * 2, "y": "c" + str(g)} for g in ids]
t = pa.table({
    "id": pa.array(ids, pa.int32()),
    "ia": pa.array(ia, pa.list_(pa.int32())),
    "c":  pa.array(c, pa.struct([("x", pa.int32()), ("y", pa.string())])),
})
pq.write_table(t, sys.argv[1])
PY
then
	psql_run "CREATE TABLE pna3 (id int, ia int[], c pnic) USING columnar;"
	check "pyarrow import rows" "$(q "SELECT columnar.import_parquet('pna3', '$PYF');")" "200"
	# build the same expected set in the heap oracle for comparison
	psql_run "CREATE TABLE pna3_o (id int, ia int[], c pnic);"
	psql_run "INSERT INTO pna3_o SELECT g,
	    CASE WHEN g % 10 = 0 THEN NULL
	         WHEN g % 7 = 0 THEN '{}'::int[]
	         ELSE ARRAY[g, NULL, g + 3] END,
	    CASE WHEN g % 5 = 0 THEN NULL ELSE ROW(g * 2, 'c' || g)::pnic END
	  FROM generate_series(1, 200) g;"
	SEL3="SELECT id, ia::text, c::text FROM"
	check "pyarrow nested file matches oracle" \
		"$(pgc_set_hash "$SEL3 pna3")" \
		"$(pgc_set_hash "$SEL3 pna3_o")"
else
	rc=$?
	if [ "$rc" = 3 ]; then
		echo "SKIP: pyarrow not available for the reference-writer case"
	else
		echo "FAIL: pyarrow nested file generation errored (rc=$rc)"
		PGC_FAIL=1
	fi
fi

pgc_summary
