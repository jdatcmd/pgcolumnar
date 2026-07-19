#!/usr/bin/env bash
#
# pgColumnar concurrency regression test (tracking issue #4).
#
# Deletes are recorded by merging bits into a single shared columnar.row_mask
# heap tuple per (storage id, stripe, chunk group). Before the fix, the delete
# path did an unguarded read-modify-write of that tuple: two transactions
# deleting different rows in the SAME chunk group could both read the old mask
# and then collide, so one transaction's delete bits were lost (a deleted row
# stayed visible) or the second deleter aborted with "tuple concurrently
# updated" / a duplicate-key error. The fix serializes the read-modify-write of
# a given chunk group with a transaction-scoped chunk-group lock and re-reads
# the committed mask before merging, so both sets of delete bits survive.
#
# This test forces the exact interleaving deterministically. It relies on the
# fact that a columnar DELETE flushes its delete marks to the catalog at the
# statement's executor-end, inside the still-open transaction. So:
#
#   session 1:  BEGIN; DELETE row A;   -- flush runs here, holds the chunk-group
#                                         write and its lock, uncommitted
#   session 2:  BEGIN; DELETE row B;   -- flush blocks: same chunk group
#   (barrier: wait until session 2 is blocked on a lock, via pg_stat_activity)
#   session 1:  COMMIT;                -- releases; session 2 unblocks
#   session 2:  COMMIT;
#
# After both commit, BOTH rows must be gone. Before the fix, session 2 either
# lost its bit or aborted, leaving its row visible; the final count then differs
# and the check fails. The barrier is a poll on pg_stat_activity for a real lock
# wait, not a fixed sleep, so the interleaving is forced, not raced.
#
# Two chunk-group states are exercised:
#   A. a row_mask tuple already exists for the chunk group (update path)
#   B. no row_mask tuple exists yet     (first-delete insert race)
#
# It also checks the intended concurrency is preserved: two deletes to
# DIFFERENT chunk groups do not block each other.
#
# Written fresh for pgColumnar; it reuses no upstream test file or expected
# output. Derived from the format/interface spec and the public PostgreSQL API.
#
# Usage:
#   test/concurrency.sh [PG_CONFIG]
#
# PG_CONFIG defaults to /usr/local/pg17/bin/pg_config. Run as a user that may
# "runuser -u postgres" (e.g. root) when the current user is not postgres.

set -uo pipefail

PG_CONFIG="${1:-/usr/local/pg17/bin/pg_config}"
BINDIR="$("$PG_CONFIG" --bindir)"
PORT="${PGC_PORT:-54329}"
SRCDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

WORKDIR="$(mktemp -d /tmp/pgcolumnar-conc.XXXXXX)"
PGDATA="$WORKDIR/data"
LOGFILE="$WORKDIR/server.log"

echo "== pgColumnar concurrency test (issue #4) =="
echo "PG_CONFIG=$PG_CONFIG"
echo "workdir=$WORKDIR"

echo "-- building"
make -C "$SRCDIR" PG_CONFIG="$PG_CONFIG" >/dev/null
echo "-- installing"
make -C "$SRCDIR" install PG_CONFIG="$PG_CONFIG" >/dev/null

if [ "$(id -u)" = "0" ]; then
	RUNPG=(runuser -u postgres --)
	chown -R postgres "$WORKDIR"
else
	RUNPG=(env)
fi

run_pg() { "${RUNPG[@]}" env PATH="$BINDIR:$PATH" bash -lc "$1"; }

SESS_PIDS=()
cleanup() {
	for p in "${SESS_PIDS[@]:-}"; do
		kill "$p" >/dev/null 2>&1 || true
	done
	run_pg "pg_ctl -D '$PGDATA' stop -m immediate -w" >/dev/null 2>&1 || true
	rm -rf "$WORKDIR"
}
trap cleanup EXIT

echo "-- initdb"
run_pg "initdb -D '$PGDATA' -A trust" >/dev/null 2>&1
run_pg "echo \"port=$PORT\" >> '$PGDATA/postgresql.conf'"
run_pg "echo \"shared_preload_libraries='columnar'\" >> '$PGDATA/postgresql.conf'"
# Do not let a real hang masquerade as a pass: cap any lock wait.
run_pg "echo \"lock_timeout=60000\" >> '$PGDATA/postgresql.conf'"
echo "-- start"
run_pg "pg_ctl -D '$PGDATA' -l '$LOGFILE' start -w" >/dev/null
run_pg "createdb -p $PORT conc"

# Controller connection: stop on error, so setup problems surface immediately.
PSQL="psql -p $PORT -d conc -qAtX -v ON_ERROR_STOP=1"
# Session connections: NO ON_ERROR_STOP, so a pre-fix concurrency error prints
# and the session keeps running (the final data check is what asserts), instead
# of the session dying and the test hanging on timeouts.
SPSQL="psql -p $PORT -d conc -qAtX"
ctl_q() { run_pg "$PSQL -c \"$1\""; }

fail=0
check() {
	local name="$1" got="$2" want="$3"
	if [ "$got" = "$want" ]; then
		echo "PASS  $name: $got"
	else
		echo "FAIL  $name: got [$got] want [$want]"
		fail=1
	fi
}

# --- persistent interactive sessions, driven over FIFOs --------------------
#
# Each session is a psql reading from a FIFO. We keep the write end open on a
# dedicated fd so the session stays alive between commands, append a sentinel
# after each command, and poll the session's output file for it. A blocked
# command never prints its sentinel; we detect the block through
# pg_stat_activity instead.

start_session() {  # name
	local name="$1"
	local infile="$WORKDIR/$name.in"
	local outfile="$WORKDIR/$name.out"
	run_pg "mkfifo '$infile'; touch '$outfile'"
	run_pg "$SPSQL >'$outfile' 2>&1 <'$infile'" &
	SESS_PIDS+=("$!")
	# Hold the FIFO open read-write so the open never blocks and the session
	# only sees EOF when we send \q.
	exec {fd}<>"$infile"
	eval "FD_$name=$fd"
}

send() {  # name sql...
	local name="$1"; shift
	local fd
	eval "fd=\$FD_$name"
	printf '%s\n' "$*" >&"$fd"
}

# send a command and wait (bounded) for its sentinel to appear in the output
send_wait() {  # name label sql...
	local name="$1" label="$2"; shift 2
	local fd outfile="$WORKDIR/$name.out" i=0
	eval "fd=\$FD_$name"
	printf '%s\n' "$*" >&"$fd"
	printf '\\echo <<%s>>\n' "$label" >&"$fd"
	while ! grep -q "<<$label>>" "$outfile" 2>/dev/null; do
		sleep 0.05; i=$((i + 1))
		if [ "$i" -ge 1200 ]; then
			echo "FAIL  timeout waiting for $name/$label"
			fail=1
			return 1
		fi
	done
	return 0
}

# wait (bounded) for a sentinel already queued behind a blocked command
wait_sentinel() {  # name label
	local name="$1" label="$2" outfile i=0
	outfile="$WORKDIR/$name.out"
	while ! grep -q "<<$label>>" "$outfile" 2>/dev/null; do
		sleep 0.05; i=$((i + 1))
		if [ "$i" -ge 1200 ]; then
			echo "FAIL  timeout waiting for $name/$label sentinel"
			fail=1
			return 1
		fi
	done
	return 0
}

# poll until the named backend is blocked waiting on a heavyweight lock
wait_blocked() {  # application_name
	local app="$1" i=0 n
	while :; do
		n="$(ctl_q "SELECT count(*) FROM pg_stat_activity WHERE application_name='$app' AND wait_event_type='Lock';")"
		[ "$n" = "1" ] && return 0
		sleep 0.05; i=$((i + 1))
		if [ "$i" -ge 1200 ]; then
			echo "FAIL  timeout waiting for $app to block"
			fail=1
			return 1
		fi
	done
}

# poll until the named backend is idle in an open transaction (command done)
wait_idle_intx() {  # application_name
	local app="$1" i=0 st
	while :; do
		st="$(ctl_q "SELECT state FROM pg_stat_activity WHERE application_name='$app';")"
		[ "$st" = "idle in transaction" ] && return 0
		sleep 0.05; i=$((i + 1))
		if [ "$i" -ge 1200 ]; then
			echo "FAIL  timeout waiting for $app to go idle-in-transaction"
			fail=1
			return 1
		fi
	done
}

ctl_q "CREATE EXTENSION columnar;" >/dev/null

# ---------------------------------------------------------------------------
# Scenario A: row_mask tuple already exists for the chunk group.
# All rows land in one stripe / one chunk group (default 10000 rows/group).
# An initial committed delete creates the row_mask tuple; then two concurrent
# deletes of different rows in that same group must both survive.
# ---------------------------------------------------------------------------
ctl_q "CREATE TABLE t (id int) USING columnar;" >/dev/null
ctl_q "INSERT INTO t SELECT g FROM generate_series(1,6) g;" >/dev/null
ctl_q "DELETE FROM t WHERE id = 6;" >/dev/null   # creates the row_mask tuple

start_session s1
start_session s2
send s1 "SET application_name='cc_s1';"
send s2 "SET application_name='cc_s2';"

send_wait s1 a_begin "BEGIN;"
# S1 deletes id=1; its executor-end flush merges the bit and holds the lock.
send_wait s1 a_del "DELETE FROM t WHERE id = 1;"

send s2 "BEGIN;"
# S2 deletes id=2 in the same chunk group; the flush blocks behind S1.
send s2 "DELETE FROM t WHERE id = 2;"
send s2 "\\echo <<a_del2>>"
wait_blocked cc_s2 || true
check "A same-group second deleter blocks" \
	"$(ctl_q "SELECT count(*) FROM pg_stat_activity WHERE application_name='cc_s2' AND wait_event_type='Lock';")" \
	"1"

# Release S1; S2 must now merge (not overwrite) and commit cleanly.
send_wait s1 a_commit "COMMIT;"
wait_sentinel s2 a_del2         # S2's DELETE (a_del2) unblocked and completed
send_wait s2 a_commit "COMMIT;"

check "A both concurrent deletes survived (ids 1,2,6 gone)" \
	"$(ctl_q "SELECT string_agg(id::text, ',' ORDER BY id) FROM t;")" \
	"3,4,5"
check "A row count after concurrent deletes" \
	"$(ctl_q "SELECT count(*) FROM t;")" "3"

# ---------------------------------------------------------------------------
# Scenario B: no row_mask tuple exists yet (first-delete insert race).
# Two concurrent first deletes of different rows in one chunk group race to
# create the initial row_mask row; both bits must survive.
# ---------------------------------------------------------------------------
ctl_q "CREATE TABLE t2 (id int) USING columnar;" >/dev/null
ctl_q "INSERT INTO t2 SELECT g FROM generate_series(1,6) g;" >/dev/null

send_wait s1 b_begin "BEGIN;"
send_wait s1 b_del "DELETE FROM t2 WHERE id = 1;"

send s2 "BEGIN;"
send s2 "DELETE FROM t2 WHERE id = 2;"
send s2 "\\echo <<b_del2>>"
wait_blocked cc_s2 || true
check "B first-delete second deleter blocks" \
	"$(ctl_q "SELECT count(*) FROM pg_stat_activity WHERE application_name='cc_s2' AND wait_event_type='Lock';")" \
	"1"

send_wait s1 b_commit "COMMIT;"
wait_sentinel s2 b_del2
send_wait s2 b_commit "COMMIT;"

check "B both first deletes survived (ids 1,2 gone)" \
	"$(ctl_q "SELECT string_agg(id::text, ',' ORDER BY id) FROM t2;")" \
	"3,4,5,6"

# ---------------------------------------------------------------------------
# Scenario C: deletes to DIFFERENT chunk groups must not serialize.
# Two stripes -> two chunk groups (each INSERT flushes its own stripe). A
# delete in each group, with the first transaction held open, must not block
# the second.
# ---------------------------------------------------------------------------
ctl_q "CREATE TABLE t3 (id int) USING columnar;" >/dev/null
ctl_q "INSERT INTO t3 SELECT g FROM generate_series(1,4) g;" >/dev/null   # stripe 1
ctl_q "INSERT INTO t3 SELECT g FROM generate_series(5,8) g;" >/dev/null   # stripe 2

send_wait s1 c_begin "BEGIN;"
send_wait s1 c_del "DELETE FROM t3 WHERE id = 1;"    # chunk group in stripe 1

# S2 deletes a row in the other stripe/chunk group; it must NOT block.
send_wait s2 c_begin "BEGIN;"
send_wait s2 c_del "DELETE FROM t3 WHERE id = 5;"    # chunk group in stripe 2
wait_idle_intx cc_s2
check "C different-group deleter does not block" \
	"$(ctl_q "SELECT count(*) FROM pg_stat_activity WHERE application_name='cc_s2' AND wait_event_type='Lock';")" \
	"0"

send_wait s1 c_commit "COMMIT;"
send_wait s2 c_commit "COMMIT;"
check "C both different-group deletes survived (ids 1,5 gone)" \
	"$(ctl_q "SELECT string_agg(id::text, ',' ORDER BY id) FROM t3;")" \
	"2,3,4,6,7,8"

send s1 "\\q"
send s2 "\\q"

echo
if [ "$fail" = 0 ]; then
	echo "ALL CONCURRENCY CHECKS PASSED"
else
	echo "SOME CONCURRENCY CHECKS FAILED"
fi
exit "$fail"
