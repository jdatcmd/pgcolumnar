#!/usr/bin/env bash
#
# pgColumnar Parquet nested-type export (gap 27): 1-D arrays -> Parquet LIST and
# named composites -> Parquet group, using Dremel repetition/definition levels.
# Exports a columnar table with an int[], a text[], and a composite column
# (covering NULL arrays, empty arrays, NULL elements, and NULL structs), reads
# the file back with pyarrow, and checks the field types are list/struct and
# every value matches an expectation recomputed in Python from the generator.
#
# Requires pyarrow; skips with a note if it is not importable.
#
# Usage:  test/parquet_nested.sh [PG_CONFIG]
#
# Written fresh for pgColumnar; it does not reuse any upstream test file.

set -uo pipefail

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

if ! python3 -c 'import pyarrow' 2>/dev/null; then
	echo "-- pyarrow not available; skipping Parquet nested export verification"
	pgc_summary
	exit 0
fi

expect_error() {
	local label="$1" sql="$2"
	if psql_run "$sql" >/dev/null 2>&1; then
		check "$label (expected error)" "succeeded" "error"
	else
		check "$label" "error" "error"
	fi
}

NF="$PGC_WORKDIR/nt.parquet"

echo "-- nested Parquet export: int[], text[], composite"
psql_run "CREATE TYPE npc AS (x int, y text);"
psql_run "CREATE TABLE np (id int, ia int[], ta text[], c npc) USING pgcolumnar;"
psql_run "INSERT INTO np SELECT g,
    CASE WHEN g % 10 = 0 THEN NULL
         WHEN g % 7 = 0 THEN '{}'::int[]
         ELSE ARRAY[g, NULL, g + 3] END,
    ARRAY['a' || g, 'b' || g],
    CASE WHEN g % 5 = 0 THEN NULL ELSE ROW(g * 2, 'c' || g)::npc END
  FROM generate_series(1, 5000) g;"

check "nested Parquet export rows written" "$(q "SELECT pgcolumnar.export_parquet('np', '$NF');")" "5000"

types="$(python3 - "$NF" <<'PY'
import sys, pyarrow as pa, pyarrow.parquet as pq
s = pq.read_schema(sys.argv[1])
ia, ta, c = s.field('ia').type, s.field('ta').type, s.field('c').type
print(pa.types.is_list(ia), ia.value_type,
      pa.types.is_list(ta), ta.value_type,
      pa.types.is_struct(c), c.num_fields,
      c.field(0).name, c.field(0).type, c.field(1).name, c.field(1).type)
PY
)"
check "nested Parquet field types" "$types" "True int32 True string True 2 x int32 y string"

vals="$(python3 - "$NF" <<'PY'
import sys, pyarrow as pa, pyarrow.parquet as pq
d = pq.read_table(sys.argv[1]).to_pydict()
bad = None
for i in range(len(d['id'])):
    g = d['id'][i]
    ia = None if g % 10 == 0 else ([] if g % 7 == 0 else [g, None, g + 3])
    ta = ['a%d' % g, 'b%d' % g]
    c = None if g % 5 == 0 else {'x': g * 2, 'y': 'c%d' % g}
    if d['ia'][i] != ia or d['ta'][i] != ta or d['c'][i] != c:
        bad = g
        break
print("MATCH" if bad is None else ("MISMATCH@%d" % bad))
PY
)"
check "nested Parquet values match expectation" "$vals" "MATCH"

# Larger data to exercise multiple row groups (page-per-column-chunk still one
# per row group here, but multiple row groups stress the leaf/level pipeline).
echo "-- nested Parquet export: multiple row groups"
psql_run "CREATE TABLE np2 (id int, ia int[]) USING pgcolumnar;"
psql_run "INSERT INTO np2 SELECT g, ARRAY[g, g*2] FROM generate_series(1, 70000) g;"
check "np2 export rows" "$(q "SELECT pgcolumnar.export_parquet('np2', '$PGC_WORKDIR/np2.parquet');")" "70000"
check "np2 values match" "$(python3 - "$PGC_WORKDIR/np2.parquet" <<'PY'
import sys, pyarrow.parquet as pq
d = pq.read_table(sys.argv[1]).to_pydict()
ok = all(d['ia'][i] == [d['id'][i], d['id'][i]*2] for i in range(len(d['id'])))
print("MATCH" if ok and len(d['id'])==70000 else "MISMATCH")
PY
)" "MATCH"

echo "-- multi-dimensional array rejected"
psql_run "CREATE TABLE npm (id int, m int[]) USING pgcolumnar;"
psql_run "INSERT INTO npm VALUES (1, ARRAY[[1,2],[3,4]]);"
expect_error "reject multi-dim array" "SELECT pgcolumnar.export_parquet('npm', '$PGC_WORKDIR/npm.parquet');"

pgc_summary
