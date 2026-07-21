#!/usr/bin/env bash
#
# pgColumnar Arrow nested-type import round-trip (gap 27): export a table with
# array and composite columns to Arrow, import it back into a fresh table, and
# assert the two tables are identical (arrays and composites reconstructed from
# the Arrow List/Struct buffers, including NULL arrays/elements/structs).
#
# Usage:  test/arrow_nested_import.sh [PG_CONFIG]
# Written fresh for pgColumnar; it does not reuse any upstream test file.

set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

AF="$PGC_WORKDIR/ni.arrows"
psql_run "CREATE TYPE nic AS (x int, y text);"
psql_run "CREATE TABLE na (id int, ia int[], ta text[], c nic) USING pgcolumnar;"
psql_run "INSERT INTO na SELECT g,
    CASE WHEN g % 10 = 0 THEN NULL
         WHEN g % 7 = 0 THEN '{}'::int[]
         ELSE ARRAY[g, NULL, g + 3] END,
    ARRAY['a' || g, 'b' || g],
    CASE WHEN g % 5 = 0 THEN NULL ELSE ROW(g * 2, 'c' || g)::nic END
  FROM generate_series(1, 5000) g;"

check "export rows" "$(q "SELECT pgcolumnar.export_arrow('na', '$AF');")" "5000"
psql_run "CREATE TABLE na2 (id int, ia int[], ta text[], c nic) USING pgcolumnar;"
check "import rows" "$(q "SELECT pgcolumnar.import_arrow('na2', '$AF');")" "5000"

# the reconstructed table must equal the original (canonical text of every column)
SEL="SELECT id, ia::text, ta::text, c::text FROM"
check "nested import round-trips (arrays + composite)" \
	"$(pgc_set_hash "$SEL na2")" \
	"$(pgc_set_hash "$SEL na")"
check "import count matches" "$(q 'SELECT count(*) FROM na2;')" "$(q 'SELECT count(*) FROM na;')"

# after a delete on the source, a fresh export+import still round-trips the live set
psql_run "DELETE FROM na WHERE id BETWEEN 1000 AND 2000;"
psql_run "SELECT pgcolumnar.export_arrow('na', '$AF');"
psql_run "TRUNCATE na2;"
psql_run "SELECT pgcolumnar.import_arrow('na2', '$AF');"
check "round-trips after delete" \
	"$(pgc_set_hash "$SEL na2")" \
	"$(pgc_set_hash "$SEL na")"

pgc_summary
