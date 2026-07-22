#!/usr/bin/env bash
#
# pgColumnar native zone maps, storage side (Phase D5a): a native-format (PGCN v1)
# table now records a Small Materialized Aggregate per 1024-value vector and per
# column chunk in pgcolumnar.zone_map (min, max, value_count, null_count; sum
# lands in D5b). D5a computes and stores them but does not yet read them, so this
# suite validates the catalog contents directly: the row structure, the exact
# value/null counts against the heap oracle, min/max presence and width, per-vector
# bounds, that the rows are removed when the table is dropped, and that adding zone
# maps did not perturb the read path (set-hash parity with a heap mirror).
#
# Usage:  test/native_zonemap.sh [PG_CONFIG]
# Written fresh for pgColumnar.

set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

# 6144 rows, a 2048-row group limit and the fixed 1024 vector length give a clean
# 3 groups x 2 vectors per column chunk. Four orderable columns with distinct null
# patterns exercise value_count/null_count and min/max.
GEN="SELECT g,
       (g * 10)::bigint,
       'lbl-' || (g % 50),
       CASE WHEN g % 7 = 0 THEN NULL ELSE g END
  FROM generate_series(1, 6144) g"

psql_run "CREATE TABLE h (id int, k bigint, label text, v int);"
psql_run "CREATE TABLE n (id int, k bigint, label text, v int) USING pgcolumnar;"
psql_run "SELECT pgcolumnar.alter_columnar_table_set('n', stripe_row_limit => 2048, chunk_group_row_limit => 1024);"
psql_run "INSERT INTO h $GEN;"
psql_run "INSERT INTO n $GEN;"

SID="$(q "SELECT pgcolumnar.get_storage_id('n');")"
zq() { q "SELECT $1 FROM pgcolumnar.zone_map WHERE storage_id = $SID;"; }

check "row count" "$(q 'SELECT count(*) FROM n;')" "6144"
check "read parity (no regression)" \
	"$(pgc_set_hash 'SELECT id, k, label, v FROM n')" \
	"$(pgc_set_hash 'SELECT id, k, label, v FROM h')"

# 3 groups x 4 columns whole-chunk rows (vector_index = -1).
check "whole-chunk zone rows" "$(zq 'count(*) FILTER (WHERE vector_index = -1)')" "12"
# 3 groups x 2 vectors x 4 columns per-vector rows.
check "per-vector zone rows" "$(zq 'count(*) FILTER (WHERE vector_index >= 0)')" "24"

# Every whole-chunk row covers its row group's rows; every vector covers 1024.
check "whole-chunk value+null = group rows" \
	"$(zq 'count(*) FILTER (WHERE vector_index = -1 AND value_count + null_count <> 2048)')" \
	"0"
check "per-vector value+null = 1024" \
	"$(zq 'count(*) FILTER (WHERE vector_index >= 0 AND value_count + null_count <> 1024)')" \
	"0"

# Counts must match the heap oracle exactly, summed over the whole-chunk rows.
# column_index is 0-based: id=0, k=1, label=2, v=3.
check "value_count sums (v, nullable)" \
	"$(zq 'sum(value_count) FILTER (WHERE vector_index = -1 AND column_index = 3)')" \
	"$(q 'SELECT count(v) FROM h;')"
check "null_count sums (v, nullable)" \
	"$(zq 'sum(null_count) FILTER (WHERE vector_index = -1 AND column_index = 3)')" \
	"$(q 'SELECT count(*) - count(v) FROM h;')"
check "value_count sums (id, no nulls)" \
	"$(zq 'sum(value_count) FILTER (WHERE vector_index = -1 AND column_index = 0)')" \
	"6144"
check "null_count zero (id, no nulls)" \
	"$(zq 'sum(null_count) FILTER (WHERE vector_index = -1 AND column_index = 0)')" \
	"0"

# Per-vector value counts of a column sum to its whole-chunk value count, per group.
check "per-vector value_count folds into chunk (k)" \
	"$(zq 'count(*) FILTER (WHERE column_index = 1 AND vector_index = -1
	        AND value_count <> (SELECT sum(z2.value_count)
	                            FROM pgcolumnar.zone_map z2
	                            WHERE z2.storage_id = pgcolumnar.zone_map.storage_id
	                              AND z2.group_number = pgcolumnar.zone_map.group_number
	                              AND z2.column_index = 1 AND z2.vector_index >= 0))')" \
	"0"

# Orderable columns carry min/max; fixed-width widths are exact.
check "min/max present for all columns" "$(zq 'count(*) FILTER (WHERE minimum IS NULL OR maximum IS NULL)')" "0"
check "id (int4) min width = 4 bytes" \
	"$(zq 'count(*) FILTER (WHERE column_index = 0 AND octet_length(minimum) <> 4)')" "0"
check "k (int8) max width = 8 bytes" \
	"$(zq 'count(*) FILTER (WHERE column_index = 1 AND octet_length(maximum) <> 8)')" "0"

# D5b populates sum for int2/int4 columns (id, v) as an exact numeric; other
# types (k bigint, label text) keep a null sum. column_index: id=0, v=3, k=1, label=2.
check "sum present for int4 columns" \
	"$(zq 'count(*) FILTER (WHERE column_index IN (0,3) AND value_count > 0 AND sum IS NULL)')" \
	"0"
check "sum null for non-int4 columns" \
	"$(zq 'count(*) FILTER (WHERE column_index IN (1,2) AND sum IS NOT NULL)')" \
	"0"
check "whole-chunk sum(id) matches heap" \
	"$(zq 'sum(sum) FILTER (WHERE column_index = 0 AND vector_index = -1)')" \
	"$(q 'SELECT sum(id) FROM h;')"

# Zone maps are removed with the relation.
psql_run "DROP TABLE n;"
check "zone maps cleaned on drop" \
	"$(q "SELECT count(*) FROM pgcolumnar.zone_map WHERE storage_id = $SID;")" "0"

pgc_summary
