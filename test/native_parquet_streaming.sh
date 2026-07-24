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

rm -f "$W/huge_sparse.parquet"
pgc_summary
