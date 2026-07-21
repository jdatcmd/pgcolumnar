#!/usr/bin/env bash
#
# pgColumnar crash/recovery and WAL durability suite.
#
# Columnar data lives in the relation main fork through the buffer manager and
# WAL, and metadata lives in WAL-logged heap catalogs, so a committed write must
# survive an immediate crash via WAL replay, and an in-flight (uncommitted)
# write must leave nothing visible. This suite crashes the cluster with SIGKILL
# and verifies both, using a heap mirror (itself crash-safe) as the oracle.
#
# Usage:  test/recovery.sh [PG_CONFIG]
#
# Written fresh for pgColumnar; it does not reuse any upstream test file.

set -uo pipefail

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

# Hard-crash the test cluster: SIGKILL the whole postmaster process group so the
# backends die too. Backends inherit the postmaster's listen socket, so killing
# only the postmaster would leave the TCP port bound and block the restart with
# "address already in use". pg_ctl starts the postmaster as a session/group
# leader (PGID == PID), so a negative-PID kill reaches the entire cluster. We
# then wait for the postmaster PID to actually disappear before returning.
crash_cluster() {
	local pm i
	pm="$(head -1 "$PGC_PGDATA/postmaster.pid" 2>/dev/null || true)"
	echo "-- SIGKILL cluster (postmaster pid=$pm)"
	if [ -n "$pm" ]; then
		kill -9 -"$pm" 2>/dev/null || true   # whole process group
		kill -9 "$pm" 2>/dev/null || true    # fallback: just the postmaster
	fi
	pkill -9 -f -- "$PGC_PGDATA" 2>/dev/null || true
	for i in $(seq 1 100); do
		{ [ -n "$pm" ] && kill -0 "$pm" 2>/dev/null; } || break
		sleep 0.1
	done
	sleep 1
}

# Restart into crash recovery, retrying in case the port is briefly still held.
restart_cluster() {
	local attempt
	echo "-- restart (crash recovery)"
	for attempt in 1 2 3 4 5 6; do
		if pgc_pg "pg_ctl -D '$PGC_PGDATA' -l '$PGC_LOGFILE' start -w" >/dev/null 2>&1; then
			return 0
		fi
		echo "-- restart attempt $attempt failed; retrying"
		sleep 2
	done
	echo "-- restart FAILED after retries"
	return 1
}

# Confirm recovery actually happened (a redo entry in the log).
recovery_ran() {
	pgc_pg "grep -c -E 'redo (starts|done)|database system was not properly shut down' '$PGC_LOGFILE'" 2>/dev/null || echo 0
}

make_pair "id int, v text, n numeric"
q "SELECT columnar.alter_columnar_table_set('t_col', chunk_group_row_limit => 1000, stripe_row_limit => 2000);" >/dev/null

# ---------------------------------------------------------------------------
# Scenario 1: committed writes survive an immediate crash.
#   batch1 is checkpointed (reaches disk); batch2 is committed but not
#   checkpointed (recoverable only from WAL). Both must be present after crash.
# ---------------------------------------------------------------------------
echo "-- scenario 1: committed durability"
load_pair "SELECT g, 'a'||g, g*1.5 FROM generate_series(1,3000) g"
psql_run "CHECKPOINT;"
psql_run "INSERT INTO t_heap SELECT g, 'b'||g, g*2.5 FROM generate_series(3001,5000) g;"
psql_run "INSERT INTO t_col  SELECT g, 'b'||g, g*2.5 FROM generate_series(3001,5000) g;"

crash_cluster
restart_cluster

check "s1 recovery ran"       "$([ "$(recovery_ran)" -ge 1 ] && echo ok)" "ok"
check "s1 col count"          "$(q 'SELECT count(*) FROM t_col;')" "5000"
check "s1 heap count"         "$(q 'SELECT count(*) FROM t_heap;')" "5000"
diff_query "s1 whole-row"     "SELECT * FROM %T"
diff_query "s1 aggregate"     "SELECT count(*), sum(n), min(v), max(v) FROM %T"
diff_query "s1 range"         "SELECT id FROM %T WHERE id BETWEEN 2500 AND 3500"

# ---------------------------------------------------------------------------
# Scenario 2: an uncommitted transaction leaves nothing visible after a crash.
#   A background session opens a transaction, inserts, and holds it open. We
#   checkpoint (to try to flush anything) and crash. After recovery the row
#   count must be unchanged and the pair must still agree.
# ---------------------------------------------------------------------------
echo "-- scenario 2: uncommitted atomicity"
env PATH="$PGC_BINDIR:$PATH" psql -h 127.0.0.1 -p "$PGC_PORT" -U postgres -d "$PGC_DB" \
	-c "BEGIN; INSERT INTO t_col SELECT g,'c'||g,g FROM generate_series(9001,11000) g; SELECT pg_sleep(30);" \
	>/dev/null 2>&1 &
BGPID=$!
sleep 5   # let the INSERT run inside the open transaction
psql_run "CHECKPOINT;"
crash_cluster
kill "$BGPID" 2>/dev/null || true
restart_cluster

check "s2 col count unchanged"  "$(q 'SELECT count(*) FROM t_col;')" "5000"
check "s2 heap count unchanged" "$(q 'SELECT count(*) FROM t_heap;')" "5000"
check "s2 no phantom rows"      "$(q 'SELECT count(*) FROM t_col WHERE id >= 9001;')" "0"
diff_query "s2 whole-row"       "SELECT * FROM %T"

# ---------------------------------------------------------------------------
# Scenario 3: committed deletes (row-mask writes) survive a crash.
# ---------------------------------------------------------------------------
echo "-- scenario 3: row-mask durability"
psql_run "DELETE FROM t_heap WHERE id % 7 = 0;"
psql_run "DELETE FROM t_col  WHERE id % 7 = 0;"
expect_after_delete="$(q 'SELECT count(*) FROM t_heap;')"

crash_cluster
restart_cluster

check "s3 col count == heap"  "$(q 'SELECT count(*) FROM t_col;')" "$expect_after_delete"
check "s3 deletes persisted"  "$(q 'SELECT count(*) FROM t_col WHERE id % 7 = 0;')" "0"
diff_query "s3 whole-row"     "SELECT * FROM %T"
diff_query "s3 aggregate"     "SELECT count(*), sum(n) FROM %T"

# ---------------------------------------------------------------------------
# Scenario 4: visibility-map (index-only-scan) durability across a crash.
#   Lazy vacuum sets all-visible bits and every write clears them, both
#   WAL-logged (log_newpage_buffer). The load-bearing correctness case: a bit
#   that was flushed to disk by a checkpoint and then CLEARED by a later delete
#   must stay cleared after a crash -- recovery must replay the clear over the
#   on-disk set bit, or an index-only scan would skip the fetch over a deleted
#   row. A clean block untouched by the delete keeps its all-visible bit.
#   Blocks: a synthetic block spans ~291 row numbers and a chunk group is 10000
#   rows, so block 20 (~row 5820) is deep in group 0 and block 120 (~row 34920)
#   is deep in group 3 -- both far from group boundaries.
# ---------------------------------------------------------------------------
echo "-- scenario 4: visibility-map durability"
psql_run "CREATE TABLE rv (id int, v int) USING columnar;"
psql_run "INSERT INTO rv SELECT g, g*2 FROM generate_series(1,50000) g;"
psql_run "CREATE INDEX rv_id ON rv (id);"
psql_run "VACUUM rv;"                       # set all-visible bits (WAL-logged)
check "s4 pre-crash group-0 block all-visible" "$(q "SELECT columnar.vm_is_visible('rv', 20);")" "t"
check "s4 pre-crash group-3 block all-visible" "$(q "SELECT columnar.vm_is_visible('rv', 120);")" "t"
psql_run "CHECKPOINT;"                       # flush the set bits to disk

# Delete all of group 3; clear-on-write clears block 120's bit. Do NOT checkpoint
# afterwards, so the clear is recovered from WAL replayed over the on-disk bit.
psql_run "DELETE FROM rv WHERE id BETWEEN 30001 AND 40000;"
check "s4 pre-crash deleted block cleared" "$(q "SELECT columnar.vm_is_visible('rv', 120);")" "f"

crash_cluster
restart_cluster
check "s4 recovery ran" "$([ "$(recovery_ran)" -ge 1 ] && echo ok)" "ok"

# The critical assertion: the cleared bit must not resurrect from the on-disk
# checkpointed page -- WAL replay of the clear must win.
check "s4 post-crash deleted block stays not-all-visible" "$(q "SELECT columnar.vm_is_visible('rv', 120);")" "f"
check "s4 post-crash clean block still all-visible"       "$(q "SELECT columnar.vm_is_visible('rv', 20);")"  "t"

# Contents are correct after recovery (deleted rows gone), against a heap oracle.
psql_run "CREATE TABLE rv_h (id int, v int) USING heap;"
psql_run "INSERT INTO rv_h SELECT g, g*2 FROM generate_series(1,50000) g WHERE g NOT BETWEEN 30001 AND 40000;"
check "s4 col count == heap oracle" "$(q 'SELECT count(*) FROM rv;')" "$(q 'SELECT count(*) FROM rv_h;')"
check "s4 contents match oracle"    "$(pgc_set_hash 'SELECT id,v FROM rv')" "$(pgc_set_hash 'SELECT id,v FROM rv_h')"

# ---------------------------------------------------------------------------
# Scenario 5: projection storage survives a crash (gap 26). A projection's
# stripes are written through the same WAL-logged path as the base, so a
# committed-but-not-checkpointed projection copy must replay. After recovery the
# projection must reproduce the base's live rows.
# ---------------------------------------------------------------------------
echo "-- scenario 5: projection durability"
psql_run "CREATE TABLE rp (a int, c int) USING columnar;"
psql_run "SELECT columnar.add_projection('rp', 'rpp', ARRAY['a','c'], ARRAY['c']);"
psql_run "INSERT INTO rp SELECT g, (g*7)%1000 FROM generate_series(1,4000) g;"
psql_run "CHECKPOINT;"
psql_run "INSERT INTO rp SELECT g, (g*7)%1000 FROM generate_series(4001,8000) g;"

crash_cluster
restart_cluster
check "s5 recovery ran" "$([ "$(recovery_ran)" -ge 1 ] && echo ok)" "ok"

psql_run "CREATE TABLE rp_h (a int, c int) USING heap;"
psql_run "INSERT INTO rp_h SELECT g, (g*7)%1000 FROM generate_series(1,8000) g;"
check "s5 base count == heap oracle" "$(q 'SELECT count(*) FROM rp;')" "$(q 'SELECT count(*) FROM rp_h;')"
check "s5 projection survives crash" \
	"$(pgc_set_hash "SELECT columnar.read_projection('rp','rpp')")" \
	"$(pgc_set_hash "SELECT a::text || '|' || c::text FROM rp_h")"
check "s5 projection scan matches oracle after recovery" \
	"$(pgc_set_hash "SELECT a, c FROM rp WHERE c BETWEEN 100 AND 200")" \
	"$(pgc_set_hash "SELECT a, c FROM rp_h WHERE c BETWEEN 100 AND 200")"

pgc_summary
