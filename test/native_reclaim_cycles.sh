#!/usr/bin/env bash
#
# pgColumnar physical reclaim, repeated compact_rewrite cycles (Phase F).
#
# Regression guard for the free-space allocator self-conflict fixed in PR #84:
# ColumnarAllocateFreeSpace consumed a free_space row without a
# CommandCounterIncrement, so a compact_rewrite that wrote MORE THAN ONE group in
# one command (many allocations) re-selected the just-consumed row and died with
# "tuple already updated by self". It only fires once reusable free space exists
# (the second compaction onward), so single-cycle and single-group tests miss it.
# native_reclaim.sh did not catch it because recluster advances the command
# counter between groups; compact_rewrite does not. This suite uses several small
# groups and repeated compact_rewrite so each command allocates several blocks
# from the free list.
#
# It asserts: every compact_rewrite past the first (i.e. with free space present)
# returns a group count instead of erroring, the live set always matches a heap
# mirror, and the file reaches a steady state instead of growing per cycle.
#
# Usage:  test/native_reclaim_cycles.sh [PG_CONFIG]
# Written fresh for pgColumnar.

set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

# 8000 rows in 8 groups of 1000, so each compaction rewrites several groups and
# each command performs several free-space allocations.
GEN="SELECT g AS id, (g % 100) AS v, md5(g::text) AS payload FROM generate_series(1, 8000) g"
psql_run "CREATE TABLE h (id int, v int, payload text);"
psql_run "CREATE TABLE n (id int, v int, payload text) USING pgcolumnar;"
psql_run "SELECT pgcolumnar.alter_columnar_table_set('n', stripe_row_limit => 1000, chunk_group_row_limit => 1000);"
psql_run "INSERT INTO h $GEN;"
psql_run "INSERT INTO n $GEN;"

fsize() { q "SELECT pg_relation_size('n');"; }
hash_n() { pgc_set_hash 'SELECT id, v, payload FROM n'; }
hash_h() { pgc_set_hash 'SELECT id, v, payload FROM h'; }

check "initial parity" "$(hash_n)" "$(hash_h)"

# Repeated {delete a rotating slice, compact_rewrite}. No inserts, so the only
# writes are compaction's and the file must plateau once reuse kicks in.
declare -a sizes
for r in 1 2 3 4 5; do
	psql_run "DELETE FROM h WHERE id % 8 = $((r % 8));"
	psql_run "DELETE FROM n WHERE id % 8 = $((r % 8));"
	# capture stderr: the pre-fix bug surfaced as an ERROR here, which would
	# otherwise be swallowed and leave rw empty.
	rw="$(env PATH="$PGC_BINDIR:$PATH" psql -h 127.0.0.1 -p "$PGC_PORT" -U postgres \
		-d "$PGC_DB" -At -c "SELECT pgcolumnar.compact_rewrite('n', 0.02);" 2>&1)"
	# a healthy call returns an integer group count; the bug returned an ERROR line
	check "compact_rewrite cycle $r returns a count (no self-conflict)" \
		"$(printf '%s' "$rw" | grep -Eq '^[0-9]+$' && echo ok || echo "bad:$rw")" "ok"
	check "parity after compact_rewrite cycle $r" "$(hash_n)" "$(hash_h)"
	sizes[$r]="$(fsize)"
done
echo "  (file sizes by cycle: ${sizes[*]})"

# Cycles 2..5 reuse the prior cycle's frees; the file must not grow past cycle 2.
check "file reaches steady state (cycle 5 <= cycle 2)" \
	"$([ "${sizes[5]}" -le "${sizes[2]}" ] && echo yes || echo no)" "yes"

pgc_summary
