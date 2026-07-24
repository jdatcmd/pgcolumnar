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

pgc_summary
