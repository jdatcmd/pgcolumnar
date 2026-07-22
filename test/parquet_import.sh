#!/usr/bin/env bash
#
# pgColumnar Parquet import suite (gap 27). pgcolumnar.import_parquet(rel, path)
# reads a Parquet file (self-contained: Thrift metadata, Snappy, PLAIN and
# dictionary encodings, DATA_PAGE v1 and v2) and inserts its rows into a target
# table. pyarrow writes the reference files (its default snappy + dictionary +
# data-page-v1, and an explicit data-page-v2 run); the imported columnar table is
# compared against a heap oracle loaded from the identical data.
#
# Requires pyarrow; skips with a note if it is not importable.
#
# Usage:  test/parquet_import.sh [PG_CONFIG]
#
# Written fresh for pgColumnar; it does not reuse any upstream test file.

set -uo pipefail

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

if ! python3 -c 'import pyarrow' 2>/dev/null; then
	echo "-- pyarrow not available; skipping Parquet import verification"
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

# Generate identical data as a Parquet file (given page version) and a CSV.
gen_py="$PGC_WORKDIR/gen.py"
cat > "$gen_py" <<'PY'
import sys, csv, datetime
import pyarrow as pa, pyarrow.parquet as pq
pv = sys.argv[1]            # '1.0' or '2.0'
pf = sys.argv[2]; cf = sys.argv[3]
N = 20000
ids=[]; big=[]; f4=[]; f8=[]; s=[]; bl=[]; dt=[]; ts=[]; by=[]
base = datetime.date(2020,1,1); bts = datetime.datetime(2020,1,1)
for g in range(1, N+1):
    ids.append(g)
    big.append(None if g%11==0 else g*100000)
    f4.append(g*0.5)                       # exact in binary32
    f8.append(g*0.25)
    s.append(None if g%13==0 else 'v%d'%g)
    bl.append(None if g%7==0 else (g%2==0))
    dt.append(base + datetime.timedelta(days=g))
    ts.append(bts + datetime.timedelta(seconds=g))
    by.append(None if g%17==0 else bytes([g&0xff, (g>>8)&0xff]))
t = pa.table({
    'id': pa.array(ids, pa.int32()),
    'big': pa.array(big, pa.int64()),
    'f4': pa.array(f4, pa.float32()),
    'f8': pa.array(f8, pa.float64()),
    's': pa.array(s, pa.string()),
    'bl': pa.array(bl, pa.bool_()),
    'dt': pa.array(dt, pa.date32()),
    'ts': pa.array(ts, pa.timestamp('us')),
    'by': pa.array(by, pa.binary()),
})
pq.write_table(t, pf, compression='snappy', use_dictionary=True,
               data_page_version=pv)
with open(cf,'w',newline='') as fh:
    w=csv.writer(fh)
    for i in range(N):
        w.writerow([
            ids[i],
            r'\N' if big[i] is None else big[i],
            f4[i], f8[i],
            r'\N' if s[i] is None else s[i],
            r'\N' if bl[i] is None else ('t' if bl[i] else 'f'),
            dt[i].isoformat(),
            ts[i].isoformat(sep=' '),
            r'\N' if by[i] is None else ('\\x'+by[i].hex()),
        ])
PY

SCHEMA="id int, big bigint, f4 real, f8 double precision, s text, bl bool, dt date, ts timestamp, by bytea"
SEL="SELECT id, big, f4, f8, s, bl, dt, ts, encode(by,'hex') FROM"

run_case() {
	local label="$1" pv="$2"
	local pfile="$PGC_WORKDIR/d_$pv.parquet" cfile="$PGC_WORKDIR/d_$pv.csv"

	python3 "$gen_py" "$pv" "$pfile" "$cfile"

	psql_run "DROP TABLE IF EXISTS pc; DROP TABLE IF EXISTS po;"
	psql_run "CREATE TABLE pc ($SCHEMA) USING pgcolumnar;"
	psql_run "CREATE TABLE po ($SCHEMA) USING heap;"
	psql_run "COPY po FROM '$cfile' WITH (FORMAT csv, NULL E'\\\\N');"

	local n
	n="$(q "SELECT pgcolumnar.import_parquet('pc', '$pfile');")"
	check "$label rows imported" "$n" "20000"
	check "$label row count matches oracle" "$(q 'SELECT count(*) FROM pc;')" "$(q 'SELECT count(*) FROM po;')"
	check "$label contents match heap oracle" \
		"$(pgc_set_hash "$SEL pc")" \
		"$(pgc_set_hash "$SEL po")"
}

echo "-- Parquet import: snappy + dictionary, data page v1 (pyarrow default)"
run_case "v1" "1.0"

echo "-- Parquet import: snappy + dictionary, data page v2"
run_case "v2" "2.0"

echo "-- Parquet import: uncompressed + no dictionary"
cat > "$PGC_WORKDIR/gen2.py" <<'PY'
import sys
import pyarrow as pa, pyarrow.parquet as pq
pf=sys.argv[1]
t=pa.table({'id':pa.array(list(range(1,5001)),pa.int32()),
            's':pa.array(['x%d'%g for g in range(1,5001)],pa.string())})
pq.write_table(t, pf, compression='none', use_dictionary=False)
PY
python3 "$PGC_WORKDIR/gen2.py" "$PGC_WORKDIR/plain.parquet"
psql_run "CREATE TABLE pp (id int, s text) USING pgcolumnar;"
check "plain import rows" "$(q "SELECT pgcolumnar.import_parquet('pp', '$PGC_WORKDIR/plain.parquet');")" "5000"
check "plain import sum(id)" "$(q 'SELECT sum(id) FROM pp;')" "12502500"
check "plain import first s" "$(q "SELECT s FROM pp WHERE id = 42;")" "x42"

echo "-- argument validation"
expect_error "reject non-parquet file" "SELECT pgcolumnar.import_parquet('pp', '$gen_py');"
psql_run "CREATE TABLE pmismatch (id int, s text, extra int) USING pgcolumnar;"
expect_error "reject column-count mismatch" "SELECT pgcolumnar.import_parquet('pmismatch', '$PGC_WORKDIR/plain.parquet');"

pgc_summary
