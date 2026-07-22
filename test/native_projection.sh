#!/usr/bin/env bash
#
# pgColumnar native projections (Phase D6d): a projection of a native-format
# (PGCN v1) table is itself written in the native format (to its own storage id)
# and read back in that format. Before D6d the projection writer was hard-wired to
# 2.2 while the reader took the base table's format, so a native base's projection
# scan returned nothing. This suite checks the write fan-out reproduces the base's
# live rows, the projection storage is native, deletes are reflected via the base
# row mask, and fan-out spans multiple projection row groups.
#
# Usage:  test/native_projection.sh [PG_CONFIG]
# Written fresh for pgColumnar.

set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

psql_run "CREATE TABLE fo (a int, b text, c int) USING pgcolumnar;"
psql_run "SELECT pgcolumnar.alter_columnar_table_set('fo', stripe_row_limit => 1000, format_version => 1);"
psql_run "SELECT pgcolumnar.add_projection('fo', 'fp', ARRAY['a','c'], ARRAY['c']);"
psql_run "SELECT pgcolumnar.add_projection('fo', 'fq', ARRAY['b']);"
psql_run "INSERT INTO fo SELECT g, 'r'||g, (g*7)%100 FROM generate_series(1,5000) g;"

check "fp fan-out matches base (a,c)" \
	"$(pgc_set_hash "SELECT pgcolumnar.read_projection('fo','fp')")" \
	"$(pgc_set_hash "SELECT a::text || '|' || c::text FROM fo")"
check "fq fan-out matches base (b)" \
	"$(pgc_set_hash "SELECT pgcolumnar.read_projection('fo','fq')")" \
	"$(pgc_set_hash "SELECT b FROM fo")"
check "fp row count matches base" \
	"$(q "SELECT count(*) FROM pgcolumnar.read_projection('fo','fp');")" \
	"$(q 'SELECT count(*) FROM fo;')"

# The projection storage has its own native storage catalog row and carries zone
# maps (native skip metadata).
check "fp storage is native" \
	"$(q "SELECT count(*) FROM pgcolumnar.storage WHERE storage_id = (SELECT proj_storage_id FROM pgcolumnar.projection WHERE storage_id = pgcolumnar.get_storage_id('fo') AND name='fp');")" \
	"1"
check "fp has zone maps (native skip metadata)" \
	"$([ "$(q "SELECT count(*) FROM pgcolumnar.zone_map WHERE storage_id = (SELECT proj_storage_id FROM pgcolumnar.projection WHERE storage_id = pgcolumnar.get_storage_id('fo') AND name='fp');")" -ge 1 ] && echo yes || echo no)" "yes"

# Deletes on the base are reflected in the projection (via the base row mask).
psql_run "DELETE FROM fo WHERE a BETWEEN 1000 AND 2000;"
check "fp reflects deletes (a,c)" \
	"$(pgc_set_hash "SELECT pgcolumnar.read_projection('fo','fp')")" \
	"$(pgc_set_hash "SELECT a::text || '|' || c::text FROM fo")"
check "fp count after delete matches base" \
	"$(q "SELECT count(*) FROM pgcolumnar.read_projection('fo','fp');")" \
	"$(q 'SELECT count(*) FROM fo;')"

# Fan-out spans multiple projection row groups (small stripe limit, 5000 rows).
check "fp spans multiple projection row groups" \
	"$([ "$(q "SELECT count(*) FROM pgcolumnar.row_group WHERE storage_id = (SELECT proj_storage_id FROM pgcolumnar.projection WHERE storage_id = pgcolumnar.get_storage_id('fo') AND name='fp');")" -ge 2 ] && echo yes || echo no)" "yes"

pgc_summary
