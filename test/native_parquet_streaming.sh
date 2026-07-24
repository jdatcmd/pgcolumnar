#!/usr/bin/env bash
#
# pgColumnar streaming external-Parquet reads (Phase G).
#
# The reader used to palloc() the whole file and fread() it in one go, so a file
# of MaxAllocSize (1GB - 1) or more could not be read at all: the palloc raised
# "invalid memory alloc request size" before any Parquet logic ran. This suite
# pins the streaming model that replaced it, one surface at a time as they are
# ported.
#
# The oversized file here is sparse: a leading PAR1, a hole, then a real footer.
# It costs a few KB on disk but is over the old ceiling by logical size, which is
# what the allocation was sized from. Nothing in the footer path reads the hole.
# The page offsets in that footer do not describe real pages, so a value read of
# this file must fail cleanly rather than crash, which is also checked.
#
# Usage:  test/native_parquet_streaming.sh [PG_CONFIG]
# Written fresh for pgColumnar.

set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

if ! python3 -c 'import pyarrow.parquet' 2>/dev/null; then
	echo "SKIP  pyarrow not available; streaming suite needs it"
	pgc_summary
	exit 0
fi

W="$PGC_WORKDIR"

# A real small file, and an oversized sparse one carrying that file's footer.
if ! python3 - "$W" <<'PY'
import os, sys
import pyarrow as pa, pyarrow.parquet as pq

W = sys.argv[1]
src = os.path.join(W, "small.parquet")
pq.write_table(pa.table({"id": pa.array([1, 2, 3], pa.int32()),
                         "v":  pa.array(["a", "b", "c"])}), src)

data = open(src, "rb").read()
metalen = int.from_bytes(data[-8:-4], "little")
meta = data[-8 - metalen:-8]

# comfortably over MaxAllocSize (1GB - 1), which is what palloc() rejected
SIZE = 1600 * 1024 * 1024
st = os.statvfs(W)
if st.f_bavail * st.f_frsize < 64 * 1024 * 1024:
    sys.exit(3)                      # no room even for a sparse file's metadata

big = os.path.join(W, "huge_sparse.parquet")
with open(big, "wb") as f:
    f.write(b"PAR1")
    f.truncate(SIZE)                 # the hole; never read by the footer path
    f.seek(SIZE - 8 - metalen)
    f.write(meta)
    f.write(metalen.to_bytes(4, "little"))
    f.write(b"PAR1")

if os.path.getsize(big) != SIZE:
    sys.exit(4)
PY
then
	echo "SKIP  could not build the oversized sparse file"
	pgc_summary
	exit 0
fi

errs() {
	local out
	out="$(env PATH="$PGC_BINDIR:$PATH" psql -h 127.0.0.1 -p "$PGC_PORT" \
		-U postgres -d "$PGC_DB" -At -c "$1" 2>&1)"
	case "$out" in
		*ERROR*) echo OK ;;
		*) echo "NO ERROR" ;;
	esac
}

# The headline: the footer path never allocates the file, so a file over the old
# ceiling describes itself. This FAILS on the pre-streaming code with
# "invalid memory alloc request size".
check "parquet_schema reads a file over the 1GB palloc ceiling" \
	"$(q "SELECT count(*) FROM pgcolumnar.parquet_schema('$W/huge_sparse.parquet');")" "2"

check "parquet_schema reports the columns of the oversized file" \
	"$(q "SELECT string_agg(column_name, ',' ORDER BY column_name) FROM pgcolumnar.parquet_schema('$W/huge_sparse.parquet');")" \
	"id,v"

# The same file's footer describes pages that are not there. A value read must
# report that, not crash the backend or return rows.
check "value read of the sparse file fails cleanly" \
	"$(errs "SELECT * FROM pgcolumnar.read_parquet('$W/huge_sparse.parquet') AS t(id int, v text)")" "OK"
check "backend survived the failed read" "$(q 'SELECT 1;')" "1"

# The ordinary path is unchanged.
check "parquet_schema still reads a normal file" \
	"$(q "SELECT count(*) FROM pgcolumnar.parquet_schema('$W/small.parquet');")" "2"
check "read_parquet still reads a normal file" \
	"$(q "SELECT count(*) FROM pgcolumnar.read_parquet('$W/small.parquet') AS t(id int, v text);")" "3"

# ---- crafted files: the guards the page loop added -------------------------
#
# Each of these keeps a valid footer (so the file binds and the read starts) and
# breaks something the page loop must not trust. The footer is taken from a real
# uncompressed file, so a zeroed page area really does parse as page headers
# rather than failing in the codec first.

python3 - "$W" <<'PY'
import os, sys
import pyarrow as pa, pyarrow.parquet as pq

W = sys.argv[1]
plain = os.path.join(W, "plain.parquet")
pq.write_table(pa.table({"id": pa.array(list(range(200)), pa.int32())}),
               plain, compression="none")
def parts(path):
    raw = open(path, "rb").read()
    n = int.from_bytes(raw[-8:-4], "little")
    return raw[-8 - n:-8], n, raw[4:-8 - n]   # footer, its length, page area

meta, metalen, body = parts(plain)

def build(name, body_bytes, m=None, ml=None):
    with open(os.path.join(W, name), "wb") as f:
        f.write(b"PAR1")
        f.write(body_bytes)
        f.write(m if m is not None else meta)
        f.write((ml if ml is not None else metalen).to_bytes(4, "little"))
        f.write(b"PAR1")

# 1. a large zero-filled page area. A zero byte is a Thrift STOP, so each
#    "header" parses as a v1 data page with num_values 0 and compressed_size 0:
#    a page that consumes one byte and produces nothing. Without the num_values
#    guard the loop advances one byte at a time, so the work is proportional to
#    the size of the zero region rather than to the data.
#
#    Two things make this file exercise that and not something else. The column
#    is REQUIRED, so the page carries no definition levels; with a nullable
#    column, decoding levels out of a zero-length page fails first and the file
#    is rejected before the loop can spin. And the zero region is large (sparse,
#    so it costs almost nothing on disk); a small one merely reaches end of file
#    and is caught by the closing short-chunk check, which is a different guard.
req = os.path.join(W, "plain_req.parquet")
pq.write_table(pa.table({"id": pa.array(list(range(200)), pa.int32())},
                        schema=pa.schema([pa.field("id", pa.int32(),
                                                   nullable=False)])),
               req, compression="none")
rmeta, rmetalen, _rbody = parts(req)

ZSIZE = 1600 * 1024 * 1024
with open(os.path.join(W, "zero_pages.parquet"), "wb") as f:
    f.write(b"PAR1")
    f.truncate(ZSIZE - 8 - rmetalen)      # the zero region, sparse
    f.seek(ZSIZE - 8 - rmetalen)
    f.write(rmeta)
    f.write(rmetalen.to_bytes(4, "little"))
    f.write(b"PAR1")

# 2. the page area is truncated to a few bytes, so the first page's declared
#    compressed_size runs past end of file.
build("short_body.parquet", body[:16])

# 3. the page area is random noise, so the page header does not parse at all
#    and the window has already reached end of file.
build("noise_pages.parquet", bytes((i * 37 + 11) & 0xFF for i in range(len(body))))
PY

# The timeout is the assertion for the first one: without the guard the page loop
# runs once per byte of a 1600MB zero region, so a regression hangs rather than
# errors, and the timeout is what turns that into a failure.
zero_out="$(timeout 60 env PATH="$PGC_BINDIR:$PATH" psql -h 127.0.0.1 -p "$PGC_PORT" \
	-U postgres -d "$PGC_DB" -At \
	-c "SELECT count(*) FROM pgcolumnar.read_parquet('$W/zero_pages.parquet') AS t(id int)" 2>&1)"
zero_rc=$?
case "$zero_rc:$zero_out" in
	124:*) zero_verdict="TIMED OUT (page loop made no progress)" ;;
	*ERROR*) zero_verdict="CLEAN REJECT" ;;
	*) zero_verdict="NO ERROR: $zero_out" ;;
esac
check "zero-filled page area is rejected, not walked byte by byte" "$zero_verdict" "CLEAN REJECT"

check "page running past end of file is rejected" \
	"$(errs "SELECT * FROM pgcolumnar.read_parquet('$W/short_body.parquet') AS t(id int)")" "OK"
check "unparseable page header is rejected" \
	"$(errs "SELECT * FROM pgcolumnar.read_parquet('$W/noise_pages.parquet') AS t(id int)")" "OK"
check "backend survived the crafted page areas" "$(q 'SELECT 1;')" "1"

# the legitimate file the crafted ones were built from must still read
check "the uncompressed source file still reads" \
	"$(q "SELECT count(*), sum(id) FROM pgcolumnar.read_parquet('$W/plain.parquet') AS t(id int);" | tr '|' ' ')" \
	"200 19900"

rm -f "$W/huge_sparse.parquet" "$W/zero_pages.parquet" "$W/short_body.parquet" \
	"$W/noise_pages.parquet"
pgc_summary
