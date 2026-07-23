#!/usr/bin/env bash
#
# pgColumnar Phase F scale validation (OPT-IN; not in the default matrix -- run it
# manually or nightly). Exercises online compaction, rewrite, reclustering, and
# physical page reclaim at a large row count and asserts correctness against a
# heap mirror plus a bounded, steady-state file under repeated compaction. The
# small matrix suites prove logic on a handful of groups; this proves the
# mutation/clustering/reclaim machinery holds at scale and across many cycles
# (no O(rows) blowups, and reclaim actually reaches a plateau rather than
# growing without bound). Uses order-independent aggregates for parity (a full
# set-hash of half a million rows is needless here).
#
# It was this suite (at SCALE_N=200000, repeated compaction) that surfaced the
# free-space allocator's missing CommandCounterIncrement: without it a second
# allocation in one compaction command re-selected the just-consumed free_space
# row and failed with "tuple already updated by self". The steady-state plateau
# check below is the regression guard.
#
# Usage:  SCALE_N=500000 test/native_scale.sh [PG_CONFIG]
#         (SCALE_N=6000000 for a full-scale run; expect the recluster to be slow.)
# Written fresh for pgColumnar.

set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

N="${SCALE_N:-500000}"
# compute the products in bigint so large g does not overflow int32
GEN="SELECT g AS id, (g % 1000)::int AS a, (g::bigint * 7) AS b,
       ((g::bigint * 7919) % 100000)::int AS c
  FROM generate_series(1, $N) g"

# order-independent fingerprint of the live set (same query on both tables)
FP="SELECT count(*), sum(a)::bigint, sum(b)::bigint, min(c), max(c),
           bit_xor(('x' || substr(md5(id||'|'||a||'|'||b||'|'||c), 1, 16))::bit(64)::bigint)"
fp_n() { q "$FP FROM n;"; }
fp_h() { q "$FP FROM h;"; }
fsize() { q "SELECT pg_relation_size('n');"; }

echo "-- loading $N rows into heap + columnar"
psql_run "CREATE TABLE h (id int, a int, b bigint, c int);"
psql_run "CREATE TABLE n (id int, a int, b bigint, c int) USING pgcolumnar;"
psql_run "SELECT pgcolumnar.alter_columnar_table_set('n', stripe_row_limit => 50000, chunk_group_row_limit => 10000);"
psql_run "INSERT INTO h $GEN;"
psql_run "INSERT INTO n $GEN;"
psql_run "CREATE INDEX n_id ON n (id);"

check "bulk load count" "$(q 'SELECT count(*) FROM n;')" "$N"
check "bulk load parity" "$(fp_n)" "$(fp_h)"

# ---- correctness under mutation / clustering -------------------------------
# Delete ~25%, then online rewrite of the partially-deleted groups.
psql_run "DELETE FROM h WHERE id % 4 = 0;"
psql_run "DELETE FROM n WHERE id % 4 = 0;"
check "parity after delete" "$(fp_n)" "$(fp_h)"
rew="$(q "SELECT pgcolumnar.compact_rewrite('n', 0.1);")"
echo "  (compact_rewrite rewrote $rew groups)"
check "parity after compact_rewrite" "$(fp_n)" "$(fp_h)"
# online index maintenance: rewritten rows carry new row numbers and must have
# fresh index entries; an indexed range lookup must match the heap exactly.
check "indexed lookup after rewrite" \
	"$(q "SELECT count(*) FROM n WHERE id BETWEEN 1 AND $((N/2));")" \
	"$(q "SELECT count(*) FROM h WHERE id BETWEEN 1 AND $((N/2));")"

# Online recluster by c; results unchanged. (This is the slow step at scale.)
rec="$(q "SELECT pgcolumnar.recluster('n', 'c');")"
echo "  (recluster processed $rec groups)"
check "parity after recluster" "$(fp_n)" "$(fp_h)"

# ---- physical reclaim: steady state across repeated compaction -------------
# Rounds of {delete a rotating 1/8 slice, compact_rewrite} with NO inserts, so
# the only writes are compaction's. After the first round establishes a
# generation, the file must PLATEAU: each round reuses the prior round's freed
# space instead of extending. A growing file here means reuse regressed.
sizes=()
for r in 1 2 3 4; do
	psql_run "DELETE FROM h WHERE id % 8 = $r;"
	psql_run "DELETE FROM n WHERE id % 8 = $r;"
	q "SELECT pgcolumnar.compact_rewrite('n', 0.02);" >/dev/null
	check "parity after reclaim round $r" "$(fp_n)" "$(fp_h)"
	sizes+=("$(fsize)")
	echo "  (round $r file size: ${sizes[-1]})"
done
# Rounds 2..4 reuse prior frees; the file must not grow past round 2's size.
check "reclaim reached steady state (round 4 <= round 2)" \
	"$([ "${sizes[3]}" -le "${sizes[1]}" ] && echo yes || echo no)" "yes"

pgc_summary
