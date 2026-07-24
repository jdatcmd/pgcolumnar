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

# errtext QUERY -> the raised ERROR line, or "NO ERROR"
errtext() {
	local out
	out="$(env PATH="$PGC_BINDIR:$PATH" psql -h 127.0.0.1 -p "$PGC_PORT" \
		-U postgres -d "$PGC_DB" -At -c "$1" 2>&1 | grep -m1 '^ERROR:')"
	[ -z "$out" ] && out="NO ERROR"
	echo "$out"
}

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

if ! python3 - "$W" <<'PY'
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
rmeta, rmetalen, rbody = parts(req)

ZSIZE = 1600 * 1024 * 1024
with open(os.path.join(W, "zero_pages.parquet"), "wb") as f:
    f.write(b"PAR1")
    f.truncate(ZSIZE - 8 - rmetalen)      # the zero region, sparse
    f.seek(ZSIZE - 8 - rmetalen)
    f.write(rmeta)
    f.write(rmetalen.to_bytes(4, "little"))
    f.write(b"PAR1")

# 2. a page whose header declares a compressed_size far past end of file.
#
#    Truncating the body cannot produce this: the footer we append always lands
#    after the cut, so the bytes are still "there" as far as the page loop can
#    tell, and the decode reads footer bytes as page data instead of failing.
#    The size has to be a lie in the page header itself, so the header is built
#    by hand here (Thrift compact, PageHeader with a DataPageHeader) and written
#    over the start of the chunk the footer points at.
def zz(v):
    u = (v << 1) ^ (v >> 63) if v < 0 else (v << 1)
    out = bytearray()
    while True:
        b = u & 0x7F
        u >>= 7
        out.append(b | (0x80 if u else 0))
        if not u:
            return bytes(out)

def fh(delta, t):
    return bytes([(delta << 4) | t])

I32, STRUCT, STOP = 5, 12, b"\x00"
BIG = 500 * 1024 * 1024          # past end of file, but under MaxAllocSize

synth = (fh(1, I32) + zz(0)          # type = DATA_PAGE
         + fh(1, I32) + zz(64)       # uncompressed_page_size
         + fh(1, I32) + zz(BIG)      # compressed_page_size: the lie
         + fh(2, STRUCT)             # field 5: DataPageHeader
         + fh(1, I32) + zz(200)      # num_values
         + fh(1, I32) + zz(0)        # encoding PLAIN
         + fh(1, I32) + zz(3)        # definition_level_encoding RLE
         + fh(1, I32) + zz(3)        # repetition_level_encoding RLE
         + STOP + STOP)
build("bad_size.parquet", synth + body[len(synth):])

# 3. the page area is random noise, so the page header does not parse at all
#    and the window has already reached end of file.
build("noise_pages.parquet", bytes((i * 37 + 11) & 0xFF for i in range(len(body))))

# 4. the footer points a column chunk past end of file.
#
#    This needs surgery on the footer, not the body: any truncation still leaves
#    the appended footer after the cut, so the offset stays inside the file. A
#    Thrift compact struct has no length prefix, so an offset varint can be
#    replaced by a longer one in place, as long as the 4-byte footer length at
#    the end of the file is updated to match.
#
#    Two facts decide which offset to patch. The page loop starts at a chunk's
#    first page and then walks pages sequentially, so only the chunk's STARTING
#    offset is ever read from the footer; patching a later one would change
#    nothing. And the first column chunk always starts at offset 4, whose varint
#    is a single byte that occurs all over the footer. So the file has two
#    columns and the SECOND column's start offset is patched: it is large enough
#    to encode uniquely. Uniqueness is asserted rather than assumed.
def enc_i64(v):
    u = (v << 1) ^ (v >> 63)
    out = bytearray()
    while True:
        b = u & 0x7F
        u >>= 7
        out.append(b | (0x80 if u else 0))
        if not u:
            return bytes(out)

two = os.path.join(W, "two.parquet")
pq.write_table(pa.table({"id": pa.array(list(range(200)), pa.int32()),
                         "v": pa.array(["r%04d" % i for i in range(200)])}),
               two, compression="none", use_dictionary=False)
tmeta, tmetalen, tbody = parts(two)
tcol = pq.ParquetFile(two).metadata.row_group(0).column(1)
tstart = tcol.dictionary_page_offset or tcol.data_page_offset

#    The offset is patched NEGATIVE, not merely large. A start offset past end
#    of file is already handled by the page loop's own "pos < len" condition,
#    which exits and lets the closing short-chunk check reject the file, so a
#    huge offset would prove nothing about this guard. A negative offset passes
#    that condition and reaches the header read, which is the case the guard
#    exists for (and which would otherwise seek backwards past the file start).
NEG = -(1 << 20)
pat = enc_i64(tstart)
if tmeta.count(pat) != 1:
    sys.exit(5)                                  # ambiguous: refuse to guess
patched = tmeta.replace(pat, enc_i64(NEG))
build("bad_offset.parquet", tbody, patched, len(patched))

# 5. two dictionary pages in one chunk. The format allows at most one, and that
#    rule is what keeps a crafted run of them from walking the file now that an
#    empty dictionary page is legitimate (an all-null column has one). Built by
#    repeating the real dictionary page, whose length is the gap between the
#    chunk start and the first data page. The loop walks pages sequentially, so
#    the footer's now-stale data_page_offset is not consulted.
dmd = pq.ParquetFile(plain).metadata.row_group(0).column(0)
if not dmd.dictionary_page_offset or dmd.data_page_offset <= dmd.dictionary_page_offset:
    sys.exit(7)                                  # no dictionary page to duplicate
dlen = dmd.data_page_offset - dmd.dictionary_page_offset
doff = dmd.dictionary_page_offset - 4            # into body, which starts at 4
build("two_dicts.parquet",
      body[:doff] + body[doff:doff + dlen] * 2 + body[doff + dlen:])

# 6. a DATA_PAGE_V2 whose declared level lengths exceed the page.
#
#    Honest scope: this check asserts the file is rejected cleanly, and it does
#    NOT isolate the v2 level guard. Verified, not assumed: with the guard removed
#    the same file is still rejected with the same message, because the level
#    decode fails on the garbage it reads and, on the value path,
#    compressed_size - levLen goes negative into a size_t so valbuf + vallen wraps
#    and every bounds check fails closed.
#
#    An AddressSanitizer build does not separate them either: palloc sub-allocates
#    from a larger block, so a read past a 32-byte page stays inside the context's
#    own allocation and ASAN never sees it. A PostgreSQL built with USE_VALGRIND
#    marks palloc chunks, and would.
#
#    The guard stays because reading level bytes past the page and converting a
#    negative length to size_t are both wrong regardless of how they happen to
#    end today, and because the page buffer is now a tight allocation. But it is
#    recorded here as argued from the code, not proven by this check.
V2 = 3
DLEN = 64                                        # > compressed_size below
BOOL_FALSE = 2
synth_v2 = (fh(1, I32) + zz(V2)          # type = DATA_PAGE_V2
            + fh(1, I32) + zz(64)        # uncompressed_page_size
            + fh(1, I32) + zz(32)        # compressed_page_size
            + fh(5, STRUCT)              # field 8: DataPageHeaderV2
            + fh(1, I32) + zz(200)       # num_values
            + fh(1, I32) + zz(0)         # num_nulls
            + fh(1, I32) + zz(200)       # num_rows
            + fh(1, I32) + zz(0)         # encoding PLAIN
            + fh(1, I32) + zz(DLEN)      # definition_levels_byte_length: the lie
            + fh(1, I32) + zz(0)         # repetition_levels_byte_length
            + fh(1, BOOL_FALSE)          # is_compressed = false, so levels and
                                         # values are read straight from the page
            + STOP + STOP)
build("v2_levels.parquet", synth_v2 + body[len(synth_v2):])

for name in ("zero_pages.parquet", "bad_size.parquet", "noise_pages.parquet",
             "bad_offset.parquet", "two_dicts.parquet", "v2_levels.parquet"):
    if not os.path.exists(os.path.join(W, name)):
        sys.exit(6)

for name in ("zero_pages.parquet", "bad_size.parquet", "noise_pages.parquet",
             "bad_offset.parquet"):
    if not os.path.exists(os.path.join(W, name)):
        sys.exit(6)
PY
then
	# A crafted file that was never written would make every check below pass for
	# the wrong reason: the read fails with "could not open file", which is still
	# an error. Recorded through check() so it counts as a real failure, not just
	# a message, and the suite stops here rather than reporting vacuous passes.
	check "crafted files were built" "build failed" "built"
	pgc_summary
fi

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

# These two assert the error TEXT, not merely that something failed. Without
# their guards the file is still rejected, just for the wrong reason and after
# the damage: a declared size of 500MB is palloc'd and read past end of file, and
# a page offset of 2^40 is seek'd to. Both then fail in the I/O layer with
# "could not read", and on an assert-enabled build the offset case trips the
# assertion in pq_source_read first. Checking only that an error occurred would
# pass with the guards deleted, which is no test at all.
check "page size past end of file is rejected by the size bound" \
	"$(errtext "SELECT * FROM pgcolumnar.read_parquet('$W/bad_size.parquet') AS t(id int)")" \
	"ERROR:  could not decode Parquet column 0 in row group 0"
check "negative page offset is rejected by the offset check" \
	"$(errtext "SELECT * FROM pgcolumnar.read_parquet('$W/bad_offset.parquet') AS t(id int, v text)")" \
	"ERROR:  could not decode Parquet column 1 in row group 0"
check "unparseable page header is rejected" \
	"$(errs "SELECT * FROM pgcolumnar.read_parquet('$W/noise_pages.parquet') AS t(id int)")" "OK"
check "a second dictionary page in a chunk is rejected" \
	"$(errtext "SELECT * FROM pgcolumnar.read_parquet('$W/two_dicts.parquet') AS t(id int)")" \
	"ERROR:  could not decode Parquet column 0 in row group 0"
check "v2 level lengths past the page are rejected" \
	"$(errtext "SELECT * FROM pgcolumnar.read_parquet('$W/v2_levels.parquet') AS t(id int)")" \
	"ERROR:  could not decode Parquet column 0 in row group 0"

# An all-null column is written with a dictionary page carrying zero entries,
# so a progress guard that rejects any page with no values rejects a file
# pyarrow produces routinely. Nothing else in the matrix covers an all-null
# Parquet read, which is why that regression was invisible.
python3 - "$W" <<'PYNULL'
import os, sys
import pyarrow as pa, pyarrow.parquet as pq
W = sys.argv[1]
pq.write_table(pa.table({"c": pa.array([None] * 100, pa.int32())}),
               os.path.join(W, "allnull.parquet"),
               use_dictionary=True, compression="none")
PYNULL
check "all-null column (empty dictionary page) still reads" \
	"$(q "SELECT count(*), count(c) FROM pgcolumnar.read_parquet('$W/allnull.parquet') AS t(c int);" | tr '|' ' ')" \
	"100 0"
check "backend survived the crafted page areas" "$(q 'SELECT 1;')" "1"

# the legitimate file the crafted ones were built from must still read
check "the uncompressed source file still reads" \
	"$(q "SELECT count(*), sum(id) FROM pgcolumnar.read_parquet('$W/plain.parquet') AS t(id int);" | tr '|' ' ')" \
	"200 19900"

rm -f "$W/huge_sparse.parquet" "$W/zero_pages.parquet" "$W/bad_size.parquet" \
	"$W/noise_pages.parquet" "$W/bad_offset.parquet" "$W/two_dicts.parquet" \
	"$W/v2_levels.parquet" "$W/allnull.parquet"
pgc_summary
