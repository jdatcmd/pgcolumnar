#!/usr/bin/env bash
#
# pgColumnar native cascade encoding (Phase D4): a native-format (PGCN v1) table
# now encodes each 1024-value vector with the lightweight adaptive selector and,
# when a block codec is set, block-compresses the encoded region. The reader
# reconstructs the exact raw values, so an encoded native table must still match a
# heap mirror. This suite proves round-trip parity across the block codecs
# (including compression=none, the encoded-but-not-block-compressed path that
# native_roundtrip does not exercise) and asserts that the encoding actually
# engages: a constant column's chunk shrinks far below its raw size, and every
# native chunk now carries a real (non-baseline) encoding descriptor.
#
# Usage:  test/native_encoding.sh [PG_CONFIG]
# Written fresh for pgColumnar.

set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

# Data with a distinct compressibility per column so the selector has something to
# choose: a monotonic id (delta/for), a constant bigint (rle/for/dict), a
# low-cardinality label (dict), a genuinely random-ish text (likely none per
# vector, still block-compressible), some nulls throughout. Small row-group limit
# so the write crosses several row groups and each column chunk spans several
# 1024-value vectors.
GEN="SELECT g,
       42::bigint,
       (ARRAY['cat','dog','bird'])[1 + g % 3],
       md5(g::text),
       CASE WHEN g % 7 = 0 THEN NULL ELSE g * 3 END
  FROM generate_series(1, 6000) g"

psql_run "CREATE TABLE h (id int, k bigint, label text, rnd text, v int);"
psql_run "INSERT INTO h $GEN;"

for codec in none pglz lz4 zstd; do
	tbl="n_$codec"
	psql_run "CREATE TABLE $tbl (id int, k bigint, label text, rnd text, v int) USING pgcolumnar;"
	psql_run "SELECT pgcolumnar.alter_columnar_table_set('$tbl', stripe_row_limit => 1500, compression => '$codec');"
	psql_run "INSERT INTO $tbl $GEN;"

	check "[$codec] row count" "$(q "SELECT count(*) FROM $tbl;")" "6000"
	check "[$codec] all columns round-trip" \
		"$(pgc_set_hash "SELECT id, k, label, rnd, v FROM $tbl")" \
		"$(pgc_set_hash 'SELECT id, k, label, rnd, v FROM h')"
	check "[$codec] nulls round-trip" \
		"$(q "SELECT count(*) FROM $tbl WHERE v IS NULL;")" \
		"$(q 'SELECT count(*) FROM h WHERE v IS NULL;')"
	check "[$codec] filtered scan" \
		"$(pgc_set_hash "SELECT id, label FROM $tbl WHERE k = 42 AND id BETWEEN 2000 AND 4000")" \
		"$(pgc_set_hash 'SELECT id, label FROM h WHERE k = 42 AND id BETWEEN 2000 AND 4000')"
	check "[$codec] aggregate over encoded column" \
		"$(q "SELECT sum(v) FROM $tbl;")" \
		"$(q 'SELECT sum(v) FROM h;')"

	# Every native column chunk now carries a real encoding descriptor (>= 6-byte
	# header), never the single-byte D2b baseline.
	check "[$codec] no baseline descriptors remain" \
		"$(q "SELECT count(*) FROM pgcolumnar.column_chunk
		      WHERE storage_id = pgcolumnar.get_storage_id('$tbl')
		        AND octet_length(encoding_descriptor) < 6;")" \
		"0"

	# The constant bigint column (column_index 1) must shrink far below its raw
	# size (6000 * 8 = 48000 bytes of values plus 750 validity per row group);
	# encoding engaging drives every chunk's page_length well under 4000.
	check "[$codec] constant column encoded small" \
		"$(q "SELECT count(*) FROM pgcolumnar.column_chunk
		      WHERE storage_id = pgcolumnar.get_storage_id('$tbl')
		        AND column_index = 1 AND page_length > 4000;")" \
		"0"

	# compression=none must leave block_codec unset on every chunk; the encoding
	# is independent of the block codec.
	if [ "$codec" = "none" ]; then
		check "[none] no block codec used" \
			"$(q "SELECT count(*) FROM pgcolumnar.column_chunk
			      WHERE storage_id = pgcolumnar.get_storage_id('$tbl')
			        AND block_codec <> 0;")" \
			"0"
	fi
done

pgc_summary
