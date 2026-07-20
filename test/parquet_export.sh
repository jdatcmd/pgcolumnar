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
psql_run "CREATE TABLE t_un (a int, p point) USING columnar;"
psql_run "INSERT INTO t_un VALUES (1, '(1,2)');"
expect_error "reject unsupported type" "SELECT columnar.export_parquet('t_un', '$PGC_WORKDIR/x.parquet');"

# ---------------------------------------------------------------------------
# Extended type coverage: date, time, timestamp(+tz), uuid, numeric(p,s) as
# DECIMAL, unconstrained numeric as text, json, jsonb. Verified against a heap
# oracle rendered to the canonical form each type maps to. Parquet's
# TIMESTAMP_MICROS is UTC-normalized, so timestamps are compared by their raw
# microsecond value rather than by schema. Non-finite dates/timestamps and NaN
# numerics export as null.
# ---------------------------------------------------------------------------
echo "-- extended type coverage"
EXT="$PGC_WORKDIR/ext.parquet"
EXTCSV="$PGC_WORKDIR/ext.csv"
psql_run "CREATE TABLE t_ext_heap (id int, dt date, tm time, ts timestamp,
          tz timestamptz, u uuid, num numeric(20,4), numun numeric,
          j json, jb jsonb);"
psql_run "CREATE TABLE t_ext_col (LIKE t_ext_heap) USING columnar;"
psql_run "INSERT INTO t_ext_heap VALUES
  (1,'2021-03-04','12:34:56.789012','2021-03-04 12:34:56.789012',
     '2021-03-04 12:34:56.789012+00','11111111-2222-3333-4444-555555555555',
     12345.6789, 9999999999999999.1234, '{\"a\": 1}', '{\"b\": 2}'),
  (2,'1970-01-01','00:00:00','2000-01-01 00:00:00','2000-01-01 00:00:00+00',
     '00000000-0000-0000-0000-000000000000', -0.0001, -1234567890.5,
     '[1, 2, 3]', '[3, 2, 1]'),
  (3, NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL),
  (4,'infinity','23:59:59.999999','infinity','-infinity',
     'ffffffff-ffff-ffff-ffff-ffffffffffff', 'NaN', 'NaN', 'null', 'true');"
psql_run "INSERT INTO t_ext_col SELECT * FROM t_ext_heap;"

extrows="$(q "SELECT columnar.export_parquet('t_ext_col', '$EXT');")"
check "extended export rows written" "$extrows" "4"

psql_run "COPY (SELECT id,
    CASE WHEN dt IS NULL OR NOT isfinite(dt) THEN NULL
         ELSE (dt - DATE '1970-01-01') END,
    CASE WHEN tm IS NULL THEN NULL
         ELSE (extract(epoch from tm)*1000000)::bigint END,
    CASE WHEN ts IS NULL OR NOT isfinite(ts) THEN NULL
         ELSE (extract(epoch from ts)*1000000)::bigint END,
    CASE WHEN tz IS NULL OR NOT isfinite(tz) THEN NULL
         ELSE (extract(epoch from tz)*1000000)::bigint END,
    CASE WHEN u IS NULL THEN NULL ELSE replace(u::text,'-','') END,
    CASE WHEN num IS NULL OR num = 'NaN'::numeric THEN NULL ELSE num::text END,
    CASE WHEN numun IS NULL THEN NULL ELSE numun::text END,
    j::text, jb::text
   FROM t_ext_heap ORDER BY id)
   TO '$EXTCSV' WITH (FORMAT csv, NULL E'\\\\N')"

extres="$(python3 - "$EXT" "$EXTCSV" <<'PY'
import sys, csv
import pyarrow as pa, pyarrow.parquet as pq
t = pq.read_table(sys.argv[1])
def col(n): return t.column(n)
def i(n): return col(n).cast(pa.int64()).to_pylist()
ids = col('id').to_pylist()
dt = col('dt').cast(pa.int32()).to_pylist()   # date32 casts to int32, not int64
tm = i('tm'); ts = i('ts'); tz = i('tz')
u  = [None if v is None else v.hex() for v in col('u').to_pylist()]
num= [None if v is None else str(v) for v in col('num').to_pylist()]
nun= [None if v is None else str(v) for v in col('numun').to_pylist()]
j  = col('j').to_pylist(); jb = col('jb').to_pylist()
ar = {}
for k in range(t.num_rows):
    ar[ids[k]] = (ids[k], dt[k], tm[k], ts[k], tz[k], u[k], num[k], nun[k], j[k], jb[k])
def N(x): return None if x==r'\N' else x
def I(x): return None if x==r'\N' else int(x)
pg = {}
for r in csv.reader(open(sys.argv[2],newline='')):
    pg[int(r[0])] = (int(r[0]), I(r[1]), I(r[2]), I(r[3]), I(r[4]),
                     N(r[5]), N(r[6]), N(r[7]), N(r[8]), N(r[9]))
print("MATCH" if ar==pg else "MISMATCH %r %r"%(ar,pg))
PY
)"
check "extended values match heap oracle" "$extres" "MATCH"

# Key type mappings visible in the Parquet schema.
exttypes="$(python3 - "$EXT" <<'PY'
import sys, pyarrow.parquet as pq
s=pq.read_table(sys.argv[1]).schema
d=dict((f.name,str(f.type)) for f in s)
print("%s|%s|%s|%s" % (d['dt'], d['tm'], d['u'], d['num']))
PY
)"
check "extended parquet types" "$exttypes" \
"date32[day]|time64[us]|fixed_size_binary[16]|decimal128(20, 4)"

if command -v duckdb >/dev/null 2>&1; then
	dnum="$(duckdb -noheader -list -c "SELECT num FROM read_parquet('$EXT') WHERE id=1;" 2>/dev/null | tr -d '[:space:]')"
	check "duckdb reads decimal" "$dnum" "12345.6789"
	dext="$(duckdb -noheader -list -c "SELECT count(*) FROM read_parquet('$EXT');" 2>/dev/null | tr -d '[:space:]')"
	check "duckdb extended row count" "$dext" "4"
fi

pgc_summary
