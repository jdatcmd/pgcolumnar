#!/usr/bin/env bash
#
# pgColumnar parallel scan suite (gap 23).
#
# Forces a parallel plan and checks that (1) the planner puts a Gather over a
# parallel columnar scan, and (2) results are identical to the heap oracle
# whether the scan runs parallel or serial. Several backends claim distinct
# stripes from a shared counter, so correctness under concurrent claiming is the
# property.
#
# Usage:  test/parallel.sh [PG_CONFIG]
#
# Written fresh for pgColumnar; it does not reuse any upstream test file.

set -uo pipefail

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

# Many stripes so parallel workers have distinct work to claim.
make_pair "id int, k int, v bigint, t text"
q "SELECT pgcolumnar.alter_columnar_table_set('t_col', chunk_group_row_limit => 1000, stripe_row_limit => 2000);" >/dev/null
load_pair "SELECT g, g%100, g::bigint*2, 'r'||g FROM generate_series(1,50000) g"

setg() { q "ALTER DATABASE $PGC_DB SET $1 = $2;" >/dev/null; }

# Force parallelism.
setg max_parallel_workers_per_gather 4
setg parallel_setup_cost 0
setg parallel_tuple_cost 0
setg min_parallel_table_scan_size 0

# A row-returning scan should plan a Gather over a parallel ColumnarScan.
plan="$(q "EXPLAIN (COSTS off) SELECT id, v FROM t_col WHERE k = 5;")"
check "parallel plan has Gather"        "$(echo "$plan" | grep -c -i 'Gather')" "1"
check "parallel plan has ColumnarScan"  "$(echo "$plan" | grep -c -i 'ColumnarScan')" "1"

# Results must match the heap oracle under the parallel plan.
diff_query "par filtered scan"  "SELECT id, v, t FROM %T WHERE k = 7"
diff_query "par range scan"     "SELECT id FROM %T WHERE k < 30"
diff_query "par whole sample"   "SELECT * FROM %T WHERE id % 997 = 0"
diff_query "par count"          "SELECT count(*), sum(v) FROM %T WHERE k < 50"
diff_query "par no-filter"      "SELECT count(*) FROM %T"

# The same queries, forced serial, must give the same answers.
setg max_parallel_workers_per_gather 0
diff_query "serial filtered scan" "SELECT id, v, t FROM %T WHERE k = 7"
diff_query "serial range scan"    "SELECT id FROM %T WHERE k < 30"
diff_query "serial count"         "SELECT count(*), sum(v) FROM %T WHERE k < 50"

q "ALTER DATABASE $PGC_DB RESET ALL;" >/dev/null

pgc_summary
