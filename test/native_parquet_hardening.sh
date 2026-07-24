#!/usr/bin/env bash
#
# pgColumnar Parquet decode-hardening suite (Phase G).
#
# The Parquet reader binds several PostgreSQL types to a wider physical type,
# because Parquet has no narrower one: int2 and date ride on INT32, time and the
# timestamps on INT64. A value outside the target type's range must raise, not
# wrap into a different value. Separately, the row-group column-chunk list is
# indexed by the schema's leaf count, so a row group that carries a different
# number of chunks must be rejected at parse time rather than read out of bounds.
#
# This complements test/corruption.sh (native catalog fields) and
# test/hardening.sh (native on-disk bytes); neither covers Parquet input.
# Run on an assert-enabled build so an out-of-bounds access trips an assertion
# rather than passing silently.
#
# Usage:  test/native_parquet_hardening.sh [PG_CONFIG]
# Written fresh for pgColumnar.

set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

if ! python3 -c 'import pyarrow.parquet' 2>/dev/null; then
	echo "SKIP  pyarrow not available; Parquet hardening suite needs it"
	pgc_summary
	exit 0
fi

W="$PGC_WORKDIR"
psql_run "CREATE SERVER pq FOREIGN DATA WRAPPER pgcolumnar_parquet;"

# errtext QUERY -> the raised error text, or "NO ERROR"
errtext() {
	local out
	out="$(env PATH="$PGC_BINDIR:$PATH" psql -h 127.0.0.1 -p "$PGC_PORT" -U postgres \
		-d "$PGC_DB" -At -c "$1" 2>&1 | grep -m1 '^ERROR:')"
	[ -z "$out" ] && out="NO ERROR"
	echo "$out"
}

# errs QUERY -> "OK" when the statement raises, "NO ERROR: <rows>" when it does not
errs() {
	local out
	out="$(env PATH="$PGC_BINDIR:$PATH" psql -h 127.0.0.1 -p "$PGC_PORT" -U postgres \
		-d "$PGC_DB" -At -c "$1" 2>&1)"
	case "$out" in
		*ERROR*) echo "OK" ;;
		*) echo "NO ERROR: $out" ;;
	esac
}

# ---- narrowing: out-of-range values must raise, not wrap --------------------
python3 - "$W" <<'PY'
import sys, pyarrow as pa, pyarrow.parquet as pq
W = sys.argv[1]
# int2 bound over INT32: 40000 does not fit int16 (would wrap to -25536)
pq.write_table(pa.table({"c": pa.array([30000, 40000], pa.int32())}),
               f"{W}/i2_bad.parquet")
# same bind, all values in range: must keep working
pq.write_table(pa.table({"c": pa.array([-30000, 0, 30000], pa.int32())}),
               f"{W}/i2_ok.parquet")
# time bound over INT64 micros: beyond one day is not a time
pq.write_table(pa.table({"c": pa.array([0, 86400000000 + 1], pa.int64())}),
               f"{W}/time_bad.parquet")
pq.write_table(pa.table({"c": pa.array([0, 3600000000], pa.int64())}),
               f"{W}/time_ok.parquet")
# date bound over INT32 days since epoch. INT32 max does NOT overflow the epoch
# offset (2147483647 - 10957 stays in range); it is rejected by IS_VALID_DATE.
pq.write_table(pa.table({"c": pa.array([0, 2147483647], pa.int32())}),
               f"{W}/date_bad.parquet")
# INT32 min is what overflows the subtraction itself, exercising the other branch
pq.write_table(pa.table({"c": pa.array([0, -2147483648], pa.int32())}),
               f"{W}/date_ovf.parquet")
pq.write_table(pa.table({"c": pa.array([0, 19000], pa.int32())}),
               f"{W}/date_ok.parquet")
# timestamp bound over INT64 micros: INT64 min overflows the epoch-offset
# subtraction. INT64 max does NOT -- it still lands inside PostgreSQL's timestamp
# range (year ~294276), so it is deliberately not the case tested here.
pq.write_table(pa.table({"c": pa.array([0, -9223372036854775808], pa.int64())}),
               f"{W}/ts_bad.parquet")
pq.write_table(pa.table({"c": pa.array([0, 1700000000000000], pa.int64())}),
               f"{W}/ts_ok.parquet")
PY

psql_run "CREATE FOREIGN TABLE f_i2_bad   (c int2)      SERVER pq OPTIONS (path '$W/i2_bad.parquet');"
psql_run "CREATE FOREIGN TABLE f_i2_ok    (c int2)      SERVER pq OPTIONS (path '$W/i2_ok.parquet');"
psql_run "CREATE FOREIGN TABLE f_time_bad (c time)      SERVER pq OPTIONS (path '$W/time_bad.parquet');"
psql_run "CREATE FOREIGN TABLE f_time_ok  (c time)      SERVER pq OPTIONS (path '$W/time_ok.parquet');"
psql_run "CREATE FOREIGN TABLE f_date_bad (c date)      SERVER pq OPTIONS (path '$W/date_bad.parquet');"
psql_run "CREATE FOREIGN TABLE f_date_ovf (c date)     SERVER pq OPTIONS (path '$W/date_ovf.parquet');"
psql_run "CREATE FOREIGN TABLE f_date_ok  (c date)      SERVER pq OPTIONS (path '$W/date_ok.parquet');"
psql_run "CREATE FOREIGN TABLE f_ts_bad   (c timestamp) SERVER pq OPTIONS (path '$W/ts_bad.parquet');"
psql_run "CREATE FOREIGN TABLE f_ts_ok    (c timestamp) SERVER pq OPTIONS (path '$W/ts_ok.parquet');"

# Pin the SQLSTATE rather than accepting any error, so an unrelated failure
# cannot pass these: 22003 numeric_value_out_of_range, 22008 datetime_field_overflow.
psql_run "CREATE FUNCTION pgc_try(q text) RETURNS text LANGUAGE plpgsql AS \$\$
          BEGIN EXECUTE q; RETURN 'NO ERROR';
          EXCEPTION WHEN OTHERS THEN RETURN SQLSTATE; END \$\$;"
sqlstate() { q "SELECT pgc_try(\$q\$$1\$q\$);"; }

check "int2 out of range -> 22003"        "$(sqlstate 'SELECT * FROM f_i2_bad')"   "22003"
check "time out of day -> 22008"          "$(sqlstate 'SELECT * FROM f_time_bad')" "22008"
check "date invalid -> 22008"             "$(sqlstate 'SELECT * FROM f_date_bad')" "22008"
check "date offset overflow -> 22008"     "$(sqlstate 'SELECT * FROM f_date_ovf')" "22008"
check "timestamp overflow -> 22008"       "$(sqlstate 'SELECT * FROM f_ts_bad')"   "22008"

# the legitimate binds must still work: the check must not disable them
check "int2 in range still reads"      "$(q "SELECT string_agg(c::text, ' ' ORDER BY c) FROM f_i2_ok;")" "-30000 0 30000"
check "time in range still reads"      "$(q 'SELECT count(*) FROM f_time_ok;')" "2"
check "date in range still reads"      "$(q 'SELECT count(*) FROM f_date_ok;')" "2"
check "timestamp in range still reads" "$(q 'SELECT count(*) FROM f_ts_ok;')"   "2"

# no wrapped value may ever be observable
check "no wrapped -25536 appears" \
	"$(q "SELECT count(*) FROM f_i2_ok WHERE c = (-25536)::int2;")" "0"

# ---- the same decoder is shared by read_parquet and import_parquet ----------
check "read_parquet also raises 22003" \
	"$(sqlstate "SELECT * FROM pgcolumnar.read_parquet('$W/i2_bad.parquet') AS t(c int2)")" "22003"
check "read_parquet in range still reads" \
	"$(q "SELECT count(*) FROM pgcolumnar.read_parquet('$W/i2_ok.parquet') AS t(c int2);")" "3"

psql_run "CREATE TABLE imp_bad (c int2);"
check "import_parquet also raises 22003" \
	"$(sqlstate "SELECT pgcolumnar.import_parquet('imp_bad'::regclass, '$W/i2_bad.parquet')")" "22003"
check "import_parquet left no wrapped rows" "$(q 'SELECT count(*) FROM imp_bad;')" "0"

# ---- malformed row-group chunk list ----------------------------------------
# Truncate the column-chunk list of a valid two-column file so the row group
# carries fewer chunks than the schema has leaves. The reader indexes chunks[] by
# the schema leaf count, so this must be rejected at parse time.
python3 - "$W" <<'PY'
import sys, pyarrow as pa, pyarrow.parquet as pq
W = sys.argv[1]
pq.write_table(pa.table({"a": pa.array([1, 2, 3], pa.int32()),
                         "b": pa.array([4, 5, 6], pa.int32())}),
               f"{W}/two.parquet")

# Walk the Thrift-compact footer to the exact byte holding the row group's
# `columns` list header, then claim it holds one fewer chunk than it does.
# Blind byte-searching is not reliable here: the same byte value occurs in the
# data pages, so the header has to be located structurally.
raw = bytearray(open(f"{W}/two.parquet", "rb").read())
flen = int.from_bytes(raw[-8:-4], "little")
base = len(raw) - 8 - flen          # start of FileMetaData
pos = base

def varint():
    global pos
    shift = 0; out = 0
    while True:
        b = raw[pos]; pos += 1
        out |= (b & 0x7F) << shift
        if not b & 0x80:
            return out
        shift += 7

def zigzag():
    u = varint()
    return (u >> 1) ^ -(u & 1)

def list_hdr():
    global pos
    b = raw[pos]; pos += 1
    size = (b >> 4) & 0x0F
    et = b & 0x0F
    if size == 0x0F:
        size = varint()
    return size, et

def skip(t):
    global pos
    if t in (1, 2):          return                 # bool: value is in the header
    if t == 3:               pos += 1               # byte
    elif t in (4, 5, 6):     zigzag()               # i16/i32/i64
    elif t == 7:             pos += 8               # double
    elif t == 8:             pos += varint()        # binary
    elif t in (9, 10):
        n, et = list_hdr()
        for _ in range(n): skip(et)
    elif t == 12:
        last = 0
        while True:
            ft, _fid, last = field(last)
            if ft == 0: break
            skip(ft)

def field(last):
    global pos
    b = raw[pos]; pos += 1
    if b == 0:
        return 0, 0, last
    t = b & 0x0F
    delta = b >> 4
    fid = last + delta if delta else zigzag()
    return t, fid, fid

# FileMetaData -> field 4 (row_groups) -> first RowGroup -> field 1 (columns)
target = None
last = 0
while True:
    ft, fid, last = field(last)
    if ft == 0:
        break
    if fid == 4 and ft == 9:                 # row_groups: list<RowGroup>
        nrg, _et = list_hdr()
        assert nrg >= 1, nrg
        rlast = 0
        while True:
            rft, rfid, rlast = field(rlast)
            if rft == 0:
                break
            if rfid == 1 and rft == 9:       # columns: list<ColumnChunk>
                target = pos                 # byte holding this list's header
                n, et = list_hdr()
                assert n == 2, f"expected 2 column chunks, got {n}"
                skip(et)                     # past ColumnChunk[0]
                start2 = pos
                skip(et)                     # past ColumnChunk[1]
                end2 = pos
                break
            skip(rft)
        break
    skip(ft)

assert target is not None, "row group columns list not found"
# Remove ColumnChunk[1] outright and say so in the header, so the footer stays a
# well-formed Thrift struct. Decrementing the count alone would desync the parser
# and fail for an unrelated reason; the only defect must be that the row group
# carries fewer chunks than the schema has leaves.
assert (raw[target] >> 4) == 2, hex(raw[target])
raw[target] = (1 << 4) | (raw[target] & 0x0F)
removed = end2 - start2
del raw[start2:end2]
raw[-8:-4] = (flen - removed).to_bytes(4, "little")
open(f"{W}/short_chunks.parquet", "wb").write(bytes(raw))
print(f"  columns list at {target}: 2 chunks -> 1, removed {removed} footer bytes",
      file=sys.stderr)
PY

psql_run "CREATE FOREIGN TABLE f_short (a int4, b int4) SERVER pq OPTIONS (path '$W/short_chunks.parquet');"
# Must be the specific malformed-row-group rejection. Before the guard this read
# chunks[1] out of bounds and failed incidentally, e.g. "invalid memory alloc
# request size", an out-of-bounds read that merely tripped an allocator check.
case "$(errtext 'SELECT * FROM f_short;')" in
	*"has a malformed row group"*) short_verdict="CLEAN REJECT" ;;
	*) short_verdict="$(errtext 'SELECT * FROM f_short;')" ;;
esac
check "short chunk list rejected with a specific error" "$short_verdict" "CLEAN REJECT"
check "backend survived short chunk list" "$(q 'SELECT 1;')" "1"
# The validation gates the read paths, not parsing, so the diagnostic function
# still works on exactly the file one would be diagnosing.
check "parquet_schema still describes a bad chunk list" \
	"$(q "SELECT count(*) FROM pgcolumnar.parquet_schema('$W/short_chunks.parquet');")" "2"
check "valid two-column file still reads" \
	"$(q "SELECT count(*) FROM pgcolumnar.read_parquet('$W/two.parquet') AS t(a int4, b int4);")" "3"

pgc_summary
