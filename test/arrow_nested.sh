#!/usr/bin/env bash
#
# pgColumnar Arrow nested-type export (gap 27): 1-D arrays -> Arrow List and
# named composites -> Arrow Struct. Exports a columnar table with an int[], a
# text[], and a composite column (covering NULL arrays, empty arrays, NULL
# elements, and NULL structs), reads the file back with pyarrow, and checks the
# field types are List/Struct and every value matches an expectation recomputed
# in Python from the deterministic generator.
#
# Requires pyarrow; skips with a note if it is not importable.
#
# Usage:  test/arrow_nested.sh [PG_CONFIG]
#
# Written fresh for pgColumnar; it does not reuse any upstream test file.

set -uo pipefail

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

if ! python3 -c 'import pyarrow' 2>/dev/null; then
	echo "-- pyarrow not available; skipping Arrow nested export verification"
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

NF="$PGC_WORKDIR/nt.arrows"

echo "-- nested export: int[], text[], composite"
psql_run "CREATE TYPE ntc AS (x int, y text);"
psql_run "CREATE TABLE nt (id int, ia int[], ta text[], c ntc) USING pgcolumnar;"
psql_run "INSERT INTO nt SELECT g,
    CASE WHEN g % 10 = 0 THEN NULL
         WHEN g % 7 = 0 THEN '{}'::int[]
         ELSE ARRAY[g, NULL, g + 3] END,
    ARRAY['a' || g, 'b' || g],
    CASE WHEN g % 5 = 0 THEN NULL ELSE ROW(g * 2, 'c' || g)::ntc END
  FROM generate_series(1, 5000) g;"

check "nested export rows written" "$(q "SELECT pgcolumnar.export_arrow('nt', '$NF');")" "5000"

# Field types: List / List / Struct, with the right value/field types.
types="$(python3 - "$NF" <<'PY'
import sys, pyarrow as pa, pyarrow.ipc as ipc
s = ipc.open_stream(pa.OSFile(sys.argv[1], 'rb')).read_all().schema
ia, ta, c = s.field('ia').type, s.field('ta').type, s.field('c').type
print(pa.types.is_list(ia), ia.value_type,
      pa.types.is_list(ta), ta.value_type,
      pa.types.is_struct(c), c.num_fields,
      c.field(0).name, c.field(0).type, c.field(1).name, c.field(1).type)
PY
)"
check "nested field types" "$types" "True int32 True string True 2 x int32 y string"

# Every value matches an expectation recomputed from the generator.
vals="$(python3 - "$NF" <<'PY'
import sys, pyarrow as pa, pyarrow.ipc as ipc
d = ipc.open_stream(pa.OSFile(sys.argv[1], 'rb')).read_all().to_pydict()
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
check "nested values match expectation" "$vals" "MATCH"

# An empty table with nested columns still writes a readable schema-only stream.
echo "-- empty nested table"
psql_run "CREATE TABLE nte (id int, ia int[], c ntc) USING pgcolumnar;"
check "empty nested rows" "$(q "SELECT pgcolumnar.export_arrow('nte', '$PGC_WORKDIR/nte.arrows');")" "0"
check "empty nested readable" "$(python3 - "$PGC_WORKDIR/nte.arrows" <<'PY'
import sys, pyarrow as pa, pyarrow.ipc as ipc
t = ipc.open_stream(pa.OSFile(sys.argv[1], 'rb')).read_all()
print(t.num_rows, pa.types.is_list(t.schema.field('ia').type), pa.types.is_struct(t.schema.field('c').type))
PY
)" "0 True True"

# Multi-dimensional arrays are rejected (not silently flattened).
echo "-- multi-dimensional array rejected"
psql_run "CREATE TABLE ntm (id int, m int[]) USING pgcolumnar;"
psql_run "INSERT INTO ntm VALUES (1, ARRAY[[1,2],[3,4]]);"
expect_error "reject multi-dim array" "SELECT pgcolumnar.export_arrow('ntm', '$PGC_WORKDIR/ntm.arrows');"

pgc_summary
