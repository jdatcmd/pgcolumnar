#!/usr/bin/env bash
#
# pgColumnar Parquet compression-codec read coverage (Phase G).
#
# The shared decoder handled only UNCOMPRESSED and SNAPPY. This suite covers the
# other codecs a foreign Parquet writer commonly uses -- GZIP, ZSTD, and LZ4_RAW
# -- reading each back and comparing to an uncompressed reference of the same
# data, so a codec that decompresses to the wrong bytes is caught. It also checks
# that a codec whose library is not built in fails cleanly rather than crashing.
#
# Usage:  test/native_parquet_codecs.sh [PG_CONFIG]
# Written fresh for pgColumnar.

set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

if ! python3 -c 'import pyarrow.parquet' 2>/dev/null; then
	echo "SKIP  pyarrow not available; codec suite needs it to write compressed files"
	pgc_summary
	exit 0
fi

W="$PGC_WORKDIR"
psql_run "CREATE SERVER pq FOREIGN DATA WRAPPER pgcolumnar_parquet;"

# Enough rows and enough repetition that compression actually engages and pages
# are non-trivial (dictionary + multiple data pages), across a few types.
python3 - "$W" <<'PY'
import sys, pyarrow as pa, pyarrow.parquet as pq
W = sys.argv[1]
n = 20000
tbl = pa.table({
    "id":  pa.array(range(n), pa.int32()),
    "grp": pa.array([i % 50 for i in range(n)], pa.int32()),
    "val": pa.array([i * 1.5 for i in range(n)], pa.float64()),
    "s":   pa.array([f"row-{i % 100}" for i in range(n)]),
})
for codec in ["none", "gzip", "zstd", "lz4"]:
    pq.write_table(tbl, f"{W}/c_{codec}.parquet", compression=codec)
# report which codec each file actually used, so the test knows what it exercises
for codec in ["gzip", "zstd", "lz4"]:
    c = pq.ParquetFile(f"{W}/c_{codec}.parquet").metadata.row_group(0).column(0).compression
    print(f"{codec} {c}", file=sys.stderr)
PY

cols="id int, grp int, val float8, s text"
ref="$(pgc_set_hash "SELECT * FROM pgcolumnar.read_parquet('$W/c_none.parquet') AS t($cols)")"

for codec in gzip zstd lz4; do
	got="$(pgc_set_hash "SELECT * FROM pgcolumnar.read_parquet('$W/c_${codec}.parquet') AS t($cols)")"
	check "$codec decodes to the same rows as uncompressed" "$got" "$ref"
done

# a count sanity-check per codec via the FDW surface too
for codec in gzip zstd lz4; do
	psql_run "CREATE FOREIGN TABLE fc_$codec ($cols) SERVER pq OPTIONS (path '$W/c_${codec}.parquet');"
	check "$codec FDW count" "$(q "SELECT count(*) FROM fc_$codec;")" "20000"
done

# an aggregate that forces every value through the decoder, not just row counts
check "gzip sum matches uncompressed" \
	"$(q "SELECT sum(id)::text FROM pgcolumnar.read_parquet('$W/c_gzip.parquet') AS t($cols);")" \
	"$(q "SELECT sum(id)::text FROM pgcolumnar.read_parquet('$W/c_none.parquet') AS t($cols);")"

# ---- crafted page-header size must yield a clean decode error --------------
# uncompressed_page_size comes from the page header unvalidated; pq_decompress
# now consumes it. Patch it to -1 (a single-byte in-place edit) so it arrives as
# an enormous size_t. The guard must reject with the reader's own "could not
# decode" error, not a generic "invalid string enlargement" from enlargeStringInfo.
python3 - "$W" <<'PY'
import sys, pyarrow as pa, pyarrow.parquet as pq
W = sys.argv[1]
pq.write_table(pa.table({"a": pa.array([1, 2, 3], pa.int32())}),
               f"{W}/badhdr.parquet", compression="gzip", use_dictionary=False)
raw = bytearray(open(f"{W}/badhdr.parquet", "rb").read())
# First PageHeader begins just past the 4-byte "PAR1" magic. Walk its i32 fields
# to uncompressed_page_size (field 2) and rewrite that one byte to zigzag(-1)=0x01.
pos = 4
def field(last):
    global pos
    b = raw[pos]; pos += 1
    t = b & 0x0F; d = b >> 4
    fid = last + d if d else None
    return t, fid
def zz_span():
    global pos
    start = pos
    while raw[pos] & 0x80: pos += 1
    pos += 1
    return start, pos
last = 0; patched = False
for _ in range(6):
    t, fid = field(last)
    if t == 0: break
    last = fid
    s, e = zz_span()
    if fid == 2:
        assert e - s == 1, (s, e, raw[s:e].hex())   # value 18 -> zigzag 36 = 0x24, 1 byte
        raw[s] = 0x01                                 # zigzag(-1)
        patched = True
        break
assert patched, "did not find uncompressed_page_size"
open(f"{W}/badhdr.parquet", "wb").write(bytes(raw))
print("  patched uncompressed_page_size -> -1", file=sys.stderr)
PY

errtext() {
	env PATH="$PGC_BINDIR:$PATH" psql -h 127.0.0.1 -p "$PGC_PORT" -U postgres \
		-d "$PGC_DB" -At -c "$1" 2>&1 | grep -m1 '^ERROR:'
}
case "$(errtext "SELECT * FROM pgcolumnar.read_parquet('$W/badhdr.parquet') AS t(a int)")" in
	*"could not decode"*) hdr_verdict="CLEAN DECODE ERROR" ;;
	*) hdr_verdict="$(errtext "SELECT * FROM pgcolumnar.read_parquet('$W/badhdr.parquet') AS t(a int)")" ;;
esac
check "crafted uncompressed size gives a clean decode error" "$hdr_verdict" "CLEAN DECODE ERROR"
check "backend survived the crafted header" "$(q 'SELECT 1;')" "1"

pgc_summary
