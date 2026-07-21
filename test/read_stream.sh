#!/usr/bin/env bash
#
# pgColumnar read stream / prefetch suite (gap 29).
#
# The columnar block reads go through the read stream API on PostgreSQL 17+
# (pgcolumnar.enable_read_stream, on by default), which must return byte-identical
# data to the synchronous ReadBuffer path. This runs a battery of scan shapes
# against a heap oracle with the stream on and off, over a multi-stripe table so
# the streamed block range spans many blocks. On PostgreSQL 13-16 the streaming
# path is compiled out and the GUC has no effect; both modes still match.
#
# Usage:  test/read_stream.sh [PG_CONFIG]
#
# Written fresh for pgColumnar; it does not reuse any upstream test file.

set -uo pipefail

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

# Small stripes and wide-ish rows so each stripe spans many blocks.
make_pair "id int, k int, v bigint, t text"
q "SELECT pgcolumnar.alter_columnar_table_set('t_col', stripe_row_limit => 2000, chunk_group_row_limit => 1000);" >/dev/null
load_pair "SELECT g, g%100, g::bigint*3, repeat('x', (g%40)+1) FROM generate_series(1,50000) g"
check "multiple stripes present" "$([ "$(stripe_count t_col)" -ge 10 ] && echo yes || echo no)" "yes"

setg() { q "ALTER DATABASE $PGC_DB SET $1 = $2;" >/dev/null; }

for mode in on off; do
	setg pgcolumnar.enable_read_stream "$mode"
	diff_query "[$mode] full scan"     "SELECT id, k, v, t FROM %T"
	diff_query "[$mode] filtered"      "SELECT id, v FROM %T WHERE k = 7"
	diff_query "[$mode] range"         "SELECT id, t FROM %T WHERE id BETWEEN 10000 AND 20000"
	diff_query "[$mode] aggregate"     "SELECT k, count(*), sum(v) FROM %T GROUP BY k"
	diff_query "[$mode] wide sample"   "SELECT * FROM %T WHERE id % 997 = 0"
done

setg pgcolumnar.enable_read_stream on
q "ALTER DATABASE $PGC_DB RESET ALL;" >/dev/null

pgc_summary
