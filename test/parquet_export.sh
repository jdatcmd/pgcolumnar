#!/usr/bin/env bash
#
# pgColumnar Parquet export suite (gap 27, piece 2).
#
# columnar.export_parquet(rel, path) writes a Parquet file. This suite exports a
# mixed-type columnar table (NULLs, boundary integers, NaN/Inf floats,
# empty/unicode text, bytea) that mirrors a heap table, reads it back, and
# asserts every value equals the heap oracle. pyarrow provides exact typed
# values for the comparison; if the DuckDB CLI is present it is used as a second
# independent reader (row count and a sum). Also covers multiple row groups, an
# empty table, and error cases.
#
# Requires pyarrow (python3-pyarrow); if it is not importable the suite skips
# with a note.
#
# Usage:  test/parquet_export.sh [PG_CONFIG]
#
# Written fresh for pgColumnar; it does not reuse any upstream test file.

set -uo pipefail

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

if ! python3 -c 'import pyarrow.parquet' 2>/dev/null; then
	echo "-- pyarrow not available; skipping Parquet export verification"
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

PARQUET="$PGC_WORKDIR/t.parquet"
PGCSV="$PGC_WORKDIR/pg.csv"

# > PARQUET_ROWGROUP_ROWS (65536) rows to force multiple row groups, plus
# explicit edge-case rows.
make_pair "id int, a int2, b int4, c int8, d float4, e float8, f bool, g text, h bytea"
load_pair "
  SELECT s AS id, (s%30000-15000)::int2, s*2, s::int8*1000,
         (s::float4/3), (s::float8/7), (s%2=0), 'r'||s,
         decode(lpad(to_hex(s),6,'0'),'hex')
  FROM generate_series(1,80000) s
  UNION ALL VALUES
   (100001, NULL::int2, NULL::int4, NULL::int8, NULL::float4, NULL::float8, NULL::bool, NULL::text, NULL::bytea),
   (100002, (-32768)::int2, (-2147483648)::int4, (-9223372036854775808)::int8, '-Infinity'::float4, 'NaN'::float8, false, ''::text, '\\x'::bytea),
   (100003, (32767)::int2, (2147483647)::int4, (9223372036854775807)::int8, 'Infinity'::float4, (3.14)::float8, true, 'unicode q'::text, '\\x00ff10'::bytea)
"

echo "-- export and verify against heap oracle via pyarrow"
rows_written="$(q "SELECT columnar.export_parquet('t_col', '$PARQUET');")"
check "export_parquet rows written" "$rows_written" "80003"

psql_run "COPY (SELECT id, a, b, c, (d::float8), e, f, g, encode(h,'hex')
                FROM t_heap ORDER BY id)
          TO '$PGCSV' WITH (FORMAT csv, NULL E'\\\\N')"

pyres="$(python3 - "$PARQUET" "$PGCSV" <<'PY'
import sys, csv, math
import pyarrow.parquet as pq
tbl = pq.read_table(sys.argv[1])
cols=['id','a','b','c','d','e','f','g','h']
if tbl.schema.names != cols:
    print("SCHEMA_NAMES_MISMATCH"); sys.exit(0)
ad=tbl.to_pydict()
ar={}
for i in range(tbl.num_rows):
    row=tuple(ad[c][i] for c in cols); ar[row[0]]=row
def pf(x): return None if x==r'\N' else float(x)
pg={}
for r in csv.reader(open(sys.argv[2],newline='')):
    i,a,b,c,d,e,fb,g,h=r
    pg[int(i)]=(int(i),None if a==r'\N' else int(a),None if b==r'\N' else int(b),
        None if c==r'\N' else int(c),pf(d),pf(e),None if fb==r'\N' else (fb=='t'),
        None if g==r'\N' else g,None if h==r'\N' else bytes.fromhex(h))
def feq(x,y):
    if isinstance(x,float) or isinstance(y,float):
        if x is None or y is None: return x is y
        if math.isnan(x) and math.isnan(y): return True
        return x==y
    return x==y
ok = set(ar)==set(pg) and all(all(feq(a,b) for a,b in zip(ar[k],pg[k])) for k in pg)
print("MATCH" if ok else "MISMATCH")
PY
)"
check "parquet values match heap oracle" "$pyres" "MATCH"

sch="$(python3 - "$PARQUET" <<'PY'
import sys, pyarrow.parquet as pq
s=pq.read_table(sys.argv[1]).schema
print(",".join(f"{f.name}:{f.type}" for f in s))
PY
)"
check "parquet schema" "$sch" "id:int32,a:int16,b:int32,c:int64,d:float,e:double,f:bool,g:string,h:binary"

# DuckDB as a second, independent reader.
if command -v duckdb >/dev/null 2>&1; then
	dcount="$(duckdb -noheader -list -c "SELECT count(*) FROM read_parquet('$PARQUET');" 2>/dev/null | tr -d '[:space:]')"
	check "duckdb row count" "$dcount" "80003"
	dsum_d="$(duckdb -noheader -list -c "SELECT count(b) FROM read_parquet('$PARQUET');" 2>/dev/null | tr -d '[:space:]')"
	pcnt="$(q "SELECT count(b) FROM t_col;")"
	check "duckdb non-null count(b)" "$dsum_d" "$pcnt"
fi

# Empty table.
echo "-- empty table"
psql_run "CREATE TABLE t_empty (a int, b text) USING columnar;"
n0="$(q "SELECT columnar.export_parquet('t_empty', '$PGC_WORKDIR/e.parquet');")"
check "empty export rows written" "$n0" "0"
er="$(python3 - "$PGC_WORKDIR/e.parquet" <<'PY'
import sys, pyarrow.parquet as pq
t=pq.read_table(sys.argv[1]); print(t.num_rows, ",".join(t.schema.names))
PY
)"
check "empty parquet readable" "$er" "0 a,b"

# Errors.
echo "-- argument validation"
expect_error "reject non-columnar table" "SELECT columnar.export_parquet('t_heap', '$PGC_WORKDIR/x.parquet');"
psql_run "CREATE TABLE t_js (a int, j json) USING columnar;"
psql_run "INSERT INTO t_js VALUES (1, '{}');"
expect_error "reject unsupported type" "SELECT columnar.export_parquet('t_js', '$PGC_WORKDIR/x.parquet');"

pgc_summary
