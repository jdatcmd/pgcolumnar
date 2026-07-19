#!/usr/bin/env bash
#
# pgColumnar concurrent unique-key insert test (tracking issue #5).
#
# A columnar row's DATA is invisible to other backends until its stripe is
# flushed at statement end, but its btree index entry (with the eagerly reserved
# synthetic TID) is written immediately. So while transaction T1's inserting
# statement is still in flight (row buffered, not flushed), a second transaction
# T2 inserting the SAME key finds T1's index entry but cannot resolve the row:
# columnar_index_fetch_tuple returns false for a row still in T1's private write
# buffer. The btree dirty-snapshot uniqueness check then treats the entry as dead
# and T2 inserts a duplicate -- two live rows with the same unique key.
#
# The fix serializes inserters of the same unique key with a transaction-scoped
# advisory lock taken inside the table AM insert path (columnar_unique.c), before
# the executor's btree uniqueness check runs. The second inserter blocks until
# the first commits (and has therefore flushed its row), at which point the
# ordinary btree check sees the committed duplicate and raises unique_violation.
#
# Making the race deterministic
# ------------------------------
# The bug only manifests while T1's row is UNFLUSHED. A completed INSERT
# statement flushes at executor-end, so a plain "T1 inserts, then T2 inserts"
# would let T2 see T1's flushed-but-uncommitted stripe via the dirty snapshot and
# block on T1's xid even without the fix. To hold T1 mid-statement -- key row
# buffered and indexed, but statement not yet ended, so not yet flushed -- T1
# runs a two-row INSERT whose SECOND row calls a function that blocks on an
# advisory lock held by a separate "holder" session. The first row (the real
# key) is inserted and indexed; the second row's evaluation blocks. While T1
# hangs there:
#   * lock DISABLED (columnar.enable_unique_insert_lock=off in T2): T2 does NOT
#     block, inserts a duplicate -> final count 2. This reproduces the bug.
#   * lock ENABLED (default): T2 blocks on the key lock T1 holds -> after T1
#     commits, T2 gets a unique_violation -> final count 1. This is the fix.
# The barrier that makes it deterministic is a poll on pg_stat_activity for a
# real heavyweight-lock wait, never a sleep.
#
# Scenarios: same-key (fails-before/passes-after via the GUC), opclass-equal-but-
# byte-different keys (numeric 1.0 vs 1.00, and citext case when available) to
# prove opclass-safe hashing, different keys not blocking (preserved
# concurrency), multi-column unique index, partial unique index (inside and
# outside the predicate), NULLS DISTINCT (no false conflict) and NULLS NOT
# DISTINCT (PG15+), a same-statement duplicate, and an aborted first inserter.
#
# Written fresh for pgColumnar; it reuses no upstream test file or expected
# output. Derived from the format/interface spec, the issue #5 design analysis,
# and the public PostgreSQL API.
#
# Usage:
#   test/unique_conc.sh [PG_CONFIG]
#
# PG_CONFIG defaults to /usr/local/pg17/bin/pg_config. Run as a user that may
# "runuser -u postgres" (e.g. root) when the current user is not postgres.

set -uo pipefail

PG_CONFIG="${1:-/usr/local/pg17/bin/pg_config}"
BINDIR="$("$PG_CONFIG" --bindir)"
SRCDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PG_MAJOR="$("$PG_CONFIG" --version | sed -E 's/^[^0-9]*([0-9]+).*/\1/')"

WORKDIR="$(mktemp -d /tmp/pgcolumnar-uconc.XXXXXX)"
PGDATA="$WORKDIR/data"
LOGFILE="$WORKDIR/server.log"

port_is_free() {
	if command -v ss >/dev/null 2>&1; then
		! ss -Htln "sport = :$1" 2>/dev/null | grep -q ":$1"
	else
		! (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null
	fi
}
pick_port() {
	local p i
	for i in $(seq 1 100); do
		p=$(( (RANDOM % 20000) + 30000 ))
		if port_is_free "$p"; then echo "$p"; return 0; fi
	done
	echo $(( (RANDOM % 20000) + 30000 ))
}
PORT="${PGC_PORT:-$(pick_port)}"

echo "== pgColumnar concurrent unique-key insert test (issue #5) =="
echo "PG_CONFIG=$PG_CONFIG (major $PG_MAJOR)"
echo "workdir=$WORKDIR"
echo "port=$PORT (private unix socket in workdir; TCP disabled)"

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
	for p in "${SESS_PIDS[@]:-}"; do kill "$p" >/dev/null 2>&1 || true; done
	run_pg "pg_ctl -D '$PGDATA' stop -m immediate -w" >/dev/null 2>&1 || true
	if [ -f "$PGDATA/postmaster.pid" ]; then
		pmpid="$(head -n1 "$PGDATA/postmaster.pid" 2>/dev/null)"
		if [ -n "${pmpid:-}" ] && [ "$pmpid" -gt 1 ] 2>/dev/null; then
			kill -9 "$pmpid" >/dev/null 2>&1 || true
		fi
	fi
	if command -v pkill >/dev/null 2>&1; then
		pkill -9 -f "$WORKDIR" >/dev/null 2>&1 || true
	fi
	rm -rf "$WORKDIR"
}
trap cleanup EXIT

echo "-- initdb"
run_pg "initdb -D '$PGDATA' -A trust" >/dev/null 2>&1
run_pg "echo \"port=$PORT\" >> '$PGDATA/postgresql.conf'"
run_pg "echo \"shared_preload_libraries='columnar'\" >> '$PGDATA/postgresql.conf'"
run_pg "echo \"listen_addresses=''\" >> '$PGDATA/postgresql.conf'"
run_pg "echo \"unix_socket_directories='$WORKDIR'\" >> '$PGDATA/postgresql.conf'"
# Cap any real hang so a bug cannot masquerade as a pass by blocking forever.
run_pg "echo \"lock_timeout=60000\" >> '$PGDATA/postgresql.conf'"
echo "-- start"
run_pg "pg_ctl -D '$PGDATA' -l '$LOGFILE' start -w" >/dev/null
run_pg "createdb -h '$WORKDIR' -p $PORT uconc"

PSQL="psql -h '$WORKDIR' -p $PORT -d uconc -qAtX -v ON_ERROR_STOP=1"
SPSQL="psql -h '$WORKDIR' -p $PORT -d uconc -qAtX"
ctl_q() { run_pg "$PSQL -c \"$1\""; }

fail=0
check() {  # name got want
	if [ "$2" = "$3" ]; then
		echo "PASS  $1: $2"
	else
		echo "FAIL  $1: got [$2] want [$3]"
		fail=1
	fi
}

# --- persistent interactive sessions over FIFOs (as in test/concurrency.sh) ---
start_session() {  # name
	local name="$1"
	local infile="$WORKDIR/$name.in"
	local outfile="$WORKDIR/$name.out"
	run_pg "mkfifo '$infile'; touch '$outfile'"
	run_pg "$SPSQL >'$outfile' 2>&1 <'$infile'" &
	SESS_PIDS+=("$!")
	exec {fd}<>"$infile"
	eval "FD_$name=$fd"
}
send() {  # name sql...
	local name="$1"; shift
	local fd; eval "fd=\$FD_$name"
	printf '%s\n' "$*" >&"$fd"
}
send_wait() {  # name label sql...
	local name="$1" label="$2"; shift 2
	local fd outfile="$WORKDIR/$name.out" i=0
	eval "fd=\$FD_$name"
	printf '%s\n' "$*" >&"$fd"
	printf '\\echo <<%s>>\n' "$label" >&"$fd"
	while ! grep -q "<<$label>>" "$outfile" 2>/dev/null; do
		sleep 0.05; i=$((i + 1))
		if [ "$i" -ge 1200 ]; then
			echo "FAIL  timeout waiting for $name/$label"; fail=1; return 1
		fi
	done
	return 0
}
wait_sentinel() {  # name label
	local name="$1" label="$2" outfile i=0
	outfile="$WORKDIR/$name.out"
	while ! grep -q "<<$label>>" "$outfile" 2>/dev/null; do
		sleep 0.05; i=$((i + 1))
		if [ "$i" -ge 1200 ]; then
			echo "FAIL  timeout waiting for $name/$label sentinel"; fail=1; return 1
		fi
	done
	return 0
}
wait_blocked() {  # application_name
	local app="$1" i=0 n
	while :; do
		n="$(ctl_q "SELECT count(*) FROM pg_stat_activity WHERE application_name='$app' AND wait_event_type='Lock';")"
		[ "$n" = "1" ] && return 0
		sleep 0.05; i=$((i + 1))
		if [ "$i" -ge 1200 ]; then
			echo "FAIL  timeout waiting for $app to block"; fail=1; return 1
		fi
	done
}
wait_idle_intx() {  # application_name
	local app="$1" i=0 st
	while :; do
		st="$(ctl_q "SELECT state FROM pg_stat_activity WHERE application_name='$app';")"
		[ "$st" = "idle in transaction" ] && return 0
		sleep 0.05; i=$((i + 1))
		if [ "$i" -ge 1200 ]; then
			echo "FAIL  timeout waiting for $app idle-in-transaction"; fail=1; return 1
		fi
	done
}
# does session $1's cumulative output contain the substring $2?
out_has() { grep -qF -- "$2" "$WORKDIR/$1.out" 2>/dev/null; }

ctl_q "CREATE EXTENSION columnar;" >/dev/null
# A row whose second value blocks on a holder-session advisory lock, holding T1
# mid-INSERT (its first, real-key row already buffered and indexed but the
# statement not yet ended, hence not yet flushed). plpgsql so it is never
# inlined or constant-folded; anyelement so one function serves every key type.
# The body is single-quoted rather than dollar-quoted on purpose: this command
# passes through two bash layers (ctl_q -> run_pg -> bash -lc), and a $$ tag
# would be expanded to a PID by the inner shell. The body contains no single
# quotes, so single-quoting is safe.
ctl_q "CREATE FUNCTION cq_block(k bigint, v anyelement) RETURNS anyelement LANGUAGE plpgsql VOLATILE AS 'BEGIN PERFORM pg_advisory_xact_lock(k); RETURN v; END;';" >/dev/null

# Fail fast if the helper is not usable (e.g. a quoting regression), rather than
# letting every lock-wait barrier time out.
if [ "$(ctl_q "SELECT cq_block(1::bigint, 42);")" != "42" ]; then
	echo "FATAL: cq_block helper is not callable"
	exit 1
fi

CITEXT=0
if run_pg "$SPSQL -c \"CREATE EXTENSION IF NOT EXISTS citext;\"" >/dev/null 2>&1; then
	CITEXT=1
fi

start_session s1
start_session s2
start_session sh   # holder of the mid-statement pause lock
send s1 "SET application_name='cc_s1';"
send s2 "SET application_name='cc_s2';"
send s1 "SET columnar.unique_lock_buckets=100003;"   # avoid false bucket sharing
send s2 "SET columnar.unique_lock_buckets=100003;"

HKEY=1000   # holder advisory-lock key, unique per pause

# pause_s1 <insert-sql-using-cq_block(HKEY,...)>
# Leaves T1 blocked mid-INSERT with its real-key row buffered+indexed, unflushed.
pause_s1() {
	send_wait sh "lk_$HKEY" "SELECT pg_advisory_lock($HKEY);"
	send_wait s1 "bg_$HKEY" "BEGIN;"
	send s1 "$1"
	send s1 "\\echo <<s1done_$HKEY>>"
	wait_blocked cc_s1
}
# release the pause and let T1's INSERT finish (flush at executor-end)
release_s1() {
	send_wait sh "ul_$HKEY" "SELECT pg_advisory_unlock($HKEY);"
	wait_sentinel s1 "s1done_$HKEY"
}

# ===========================================================================
# Scenario 1: same key. Fails-before (lock disabled) and passes-after (default).
# ===========================================================================
ctl_q "CREATE TABLE s_int (k int) USING columnar;" >/dev/null
ctl_q "CREATE UNIQUE INDEX s_int_uidx ON s_int (k);" >/dev/null

# --- 1a: lock DISABLED reproduces the bug (T2 does not block; duplicate) ------
HKEY=1001
pause_s1 "INSERT INTO s_int SELECT CASE WHEN g=2 THEN cq_block($HKEY, -1) ELSE 1 END FROM generate_series(1,2) g;"
send_wait s2 "s2off_beg" "BEGIN;"
send s2 "SET LOCAL columnar.enable_unique_insert_lock = off;"
# T1 holds key 1 unflushed. With the lock off, T2 must NOT block; it inserts a
# duplicate and commits. (With the fix on, it would block here instead.)
send_wait s2 "s2off_ins" "INSERT INTO s_int VALUES (1);"
send_wait s2 "s2off_com" "COMMIT;"
release_s1
send_wait s1 "s1_com_1a" "COMMIT;"
check "1a pre-fix: lock disabled lets both same-key inserts commit (bug)" \
	"$(ctl_q "SELECT count(*) FROM s_int WHERE k = 1;")" "2"

# --- 1b: lock ENABLED (default) serializes and raises unique_violation --------
ctl_q "TRUNCATE s_int;" >/dev/null
HKEY=1002
pause_s1 "INSERT INTO s_int SELECT CASE WHEN g=2 THEN cq_block($HKEY, -1) ELSE 1 END FROM generate_series(1,2) g;"
send_wait s2 "s2on_beg" "BEGIN;"
send s2 "INSERT INTO s_int VALUES (1);"
send s2 "\\echo <<s2on_ins>>"
wait_blocked cc_s2
check "1b post-fix: same-key second inserter blocks" \
	"$(ctl_q "SELECT count(*) FROM pg_stat_activity WHERE application_name='cc_s2' AND wait_event_type='Lock';")" "1"
release_s1
send_wait s1 "s1_com_1b" "COMMIT;"   # releases the key lock; T2 unblocks
wait_sentinel s2 s2on_ins             # T2's INSERT completed (with an error)
check "1b post-fix: blocked inserter got a unique_violation" \
	"$(out_has s2 's_int_uidx' && echo yes || echo no)" "yes"
send_wait s2 "s2on_rb" "ROLLBACK;"
check "1b post-fix: exactly one row with key 1" \
	"$(ctl_q "SELECT count(*) FROM s_int WHERE k = 1;")" "1"

# ===========================================================================
# Scenario 2: opclass-equal but byte-different keys must serialize.
# numeric 1.0 vs 1.00 are equal under numeric equality; a raw-byte hash would
# split them and miss the conflict. The fix hashes with the numeric hash proc.
# ===========================================================================
ctl_q "CREATE TABLE s_num (v numeric) USING columnar;" >/dev/null
ctl_q "CREATE UNIQUE INDEX s_num_uidx ON s_num (v);" >/dev/null

# pre-fix contrast: byte-different equal keys both commit when the lock is off
HKEY=1003
pause_s1 "INSERT INTO s_num SELECT CASE WHEN g=2 THEN cq_block($HKEY, (-1)::numeric) ELSE 1.0 END FROM generate_series(1,2) g;"
send_wait s2 "n_off_beg" "BEGIN;"
send s2 "SET LOCAL columnar.enable_unique_insert_lock = off;"
send_wait s2 "n_off_ins" "INSERT INTO s_num VALUES (1.00);"
send_wait s2 "n_off_com" "COMMIT;"
release_s1
send_wait s1 "s1_com_2a" "COMMIT;"
check "2a pre-fix: numeric 1.0 and 1.00 both commit with lock off (bug)" \
	"$(ctl_q "SELECT count(*) FROM s_num WHERE v = 1.0;")" "2"

ctl_q "TRUNCATE s_num;" >/dev/null
HKEY=1004
pause_s1 "INSERT INTO s_num SELECT CASE WHEN g=2 THEN cq_block($HKEY, (-1)::numeric) ELSE 1.0 END FROM generate_series(1,2) g;"
send_wait s2 "n_on_beg" "BEGIN;"
send s2 "INSERT INTO s_num VALUES (1.00);"   # byte-different, opclass-equal to 1.0
send s2 "\\echo <<n_on_ins>>"
wait_blocked cc_s2
check "2b post-fix: numeric 1.00 serializes behind 1.0 (opclass-safe hash)" \
	"$(ctl_q "SELECT count(*) FROM pg_stat_activity WHERE application_name='cc_s2' AND wait_event_type='Lock';")" "1"
release_s1
send_wait s1 "s1_com_2b" "COMMIT;"
wait_sentinel s2 n_on_ins
check "2b post-fix: numeric equal-but-different-bytes got unique_violation" \
	"$(out_has s2 's_num_uidx' && echo yes || echo no)" "yes"
send_wait s2 "n_on_rb" "ROLLBACK;"
check "2b post-fix: exactly one row equal to 1.0" \
	"$(ctl_q "SELECT count(*) FROM s_num WHERE v = 1.0;")" "1"

# citext case difference, when the extension is available
if [ "$CITEXT" = 1 ]; then
	ctl_q "CREATE TABLE s_ci (v citext) USING columnar;" >/dev/null
	ctl_q "CREATE UNIQUE INDEX s_ci_uidx ON s_ci (v);" >/dev/null
	HKEY=1005
	pause_s1 "INSERT INTO s_ci SELECT CASE WHEN g=2 THEN cq_block($HKEY, 'zzz'::citext) ELSE 'ABC'::citext END FROM generate_series(1,2) g;"
	send_wait s2 "ci_beg" "BEGIN;"
	send s2 "INSERT INTO s_ci VALUES ('abc');"   # equals 'ABC' under citext
	send s2 "\\echo <<ci_ins>>"
	wait_blocked cc_s2
	check "2c post-fix: citext 'abc' serializes behind 'ABC' (case-folded hash)" \
		"$(ctl_q "SELECT count(*) FROM pg_stat_activity WHERE application_name='cc_s2' AND wait_event_type='Lock';")" "1"
	release_s1
	send_wait s1 "s1_com_2c" "COMMIT;"
	wait_sentinel s2 ci_ins
	check "2c post-fix: citext case-insensitive duplicate got unique_violation" \
		"$(out_has s2 's_ci_uidx' && echo yes || echo no)" "yes"
	send_wait s2 "ci_rb" "ROLLBACK;"
	check "2c post-fix: exactly one row equal to 'abc'" \
		"$(ctl_q "SELECT count(*) FROM s_ci WHERE v = 'abc';")" "1"
else
	echo "SKIP  2c citext case test (citext extension not available)"
fi

# ===========================================================================
# Scenario 3: different keys must NOT block each other (preserved concurrency).
# ===========================================================================
ctl_q "CREATE TABLE s_diff (k int) USING columnar;" >/dev/null
ctl_q "CREATE UNIQUE INDEX s_diff_uidx ON s_diff (k);" >/dev/null
HKEY=1006
pause_s1 "INSERT INTO s_diff SELECT CASE WHEN g=2 THEN cq_block($HKEY, -1) ELSE 10 END FROM generate_series(1,2) g;"
send_wait s2 "d_beg" "BEGIN;"
send_wait s2 "d_ins" "INSERT INTO s_diff VALUES (20);"   # different key: must not block
wait_idle_intx cc_s2
check "3 different keys do not block each other" \
	"$(ctl_q "SELECT count(*) FROM pg_stat_activity WHERE application_name='cc_s2' AND wait_event_type='Lock';")" "0"
send_wait s2 "d_com" "COMMIT;"
release_s1
send_wait s1 "s1_com_3" "COMMIT;"
check "3 both distinct keys present" \
	"$(ctl_q "SELECT string_agg(k::text, ',' ORDER BY k) FROM s_diff WHERE k IN (10,20);")" "10,20"

# ===========================================================================
# Scenario 4: multi-column unique index.
# ===========================================================================
ctl_q "CREATE TABLE s_mc (a int, b int) USING columnar;" >/dev/null
ctl_q "CREATE UNIQUE INDEX s_mc_uidx ON s_mc (a, b);" >/dev/null
# equal composite key (1,2) must serialize and conflict
HKEY=1007
pause_s1 "INSERT INTO s_mc SELECT CASE WHEN g=2 THEN cq_block($HKEY, -1) ELSE 1 END, 2 FROM generate_series(1,2) g;"
send_wait s2 "mc_beg" "BEGIN;"
send s2 "INSERT INTO s_mc VALUES (1, 2);"
send s2 "\\echo <<mc_ins>>"
wait_blocked cc_s2
check "4 multi-column equal key serializes" \
	"$(ctl_q "SELECT count(*) FROM pg_stat_activity WHERE application_name='cc_s2' AND wait_event_type='Lock';")" "1"
release_s1
send_wait s1 "s1_com_4a" "COMMIT;"
wait_sentinel s2 mc_ins
check "4 multi-column duplicate got unique_violation" \
	"$(out_has s2 's_mc_uidx' && echo yes || echo no)" "yes"
send_wait s2 "mc_rb" "ROLLBACK;"
check "4 exactly one (1,2) row" \
	"$(ctl_q "SELECT count(*) FROM s_mc WHERE a=1 AND b=2;")" "1"
# a composite key differing in one column must NOT block
HKEY=1008
pause_s1 "INSERT INTO s_mc SELECT CASE WHEN g=2 THEN cq_block($HKEY, -1) ELSE 5 END, 6 FROM generate_series(1,2) g;"
send_wait s2 "mc2_beg" "BEGIN;"
send_wait s2 "mc2_ins" "INSERT INTO s_mc VALUES (5, 7);"   # (5,7) != (5,6)
wait_idle_intx cc_s2
check "4 multi-column differing key does not block" \
	"$(ctl_q "SELECT count(*) FROM pg_stat_activity WHERE application_name='cc_s2' AND wait_event_type='Lock';")" "0"
send_wait s2 "mc2_com" "COMMIT;"
release_s1
send_wait s1 "s1_com_4b" "COMMIT;"

# ===========================================================================
# Scenario 5: partial unique index (uniqueness only where active).
# ===========================================================================
ctl_q "CREATE TABLE s_part (k int, active boolean) USING columnar;" >/dev/null
ctl_q "CREATE UNIQUE INDEX s_part_uidx ON s_part (k) WHERE active;" >/dev/null
# inside the predicate: (1,true) vs (1,true) must serialize and conflict
HKEY=1009
pause_s1 "INSERT INTO s_part SELECT CASE WHEN g=2 THEN cq_block($HKEY, -1) ELSE 1 END, true FROM generate_series(1,2) g;"
send_wait s2 "pt_beg" "BEGIN;"
send s2 "INSERT INTO s_part VALUES (1, true);"
send s2 "\\echo <<pt_ins>>"
wait_blocked cc_s2
check "5 partial index, row inside predicate serializes" \
	"$(ctl_q "SELECT count(*) FROM pg_stat_activity WHERE application_name='cc_s2' AND wait_event_type='Lock';")" "1"
release_s1
send_wait s1 "s1_com_5a" "COMMIT;"
wait_sentinel s2 pt_ins
check "5 partial index in-predicate duplicate got unique_violation" \
	"$(out_has s2 's_part_uidx' && echo yes || echo no)" "yes"
send_wait s2 "pt_rb" "ROLLBACK;"
check "5 exactly one active row with k=1" \
	"$(ctl_q "SELECT count(*) FROM s_part WHERE k=1 AND active;")" "1"
# outside the predicate: (2,false) held by T1, T2 inserts (2,false); the index
# does not cover these rows, so T2 must NOT block and both rows are kept.
HKEY=1010
pause_s1 "INSERT INTO s_part SELECT CASE WHEN g=2 THEN cq_block($HKEY, -1) ELSE 2 END, false FROM generate_series(1,2) g;"
send_wait s2 "pt2_beg" "BEGIN;"
send_wait s2 "pt2_ins" "INSERT INTO s_part VALUES (2, false);"
wait_idle_intx cc_s2
check "5 partial index, rows outside predicate do not serialize" \
	"$(ctl_q "SELECT count(*) FROM pg_stat_activity WHERE application_name='cc_s2' AND wait_event_type='Lock';")" "0"
send_wait s2 "pt2_com" "COMMIT;"
release_s1
send_wait s1 "s1_com_5b" "COMMIT;"
check "5 both inactive k=2 rows kept (predicate not enforced)" \
	"$(ctl_q "SELECT count(*) FROM s_part WHERE k=2 AND NOT active;")" "2"

# ===========================================================================
# Scenario 6: NULL handling.
# ===========================================================================
# NULLS DISTINCT (default): two NULL keys never conflict -> no block, both kept.
ctl_q "CREATE TABLE s_nd (k int) USING columnar;" >/dev/null
ctl_q "CREATE UNIQUE INDEX s_nd_uidx ON s_nd (k);" >/dev/null
HKEY=1011
pause_s1 "INSERT INTO s_nd SELECT CASE WHEN g=2 THEN cq_block($HKEY, -1) ELSE NULL END FROM generate_series(1,2) g;"
send_wait s2 "nd_beg" "BEGIN;"
send_wait s2 "nd_ins" "INSERT INTO s_nd VALUES (NULL);"
wait_idle_intx cc_s2
check "6 NULLS DISTINCT: two NULL keys do not block" \
	"$(ctl_q "SELECT count(*) FROM pg_stat_activity WHERE application_name='cc_s2' AND wait_event_type='Lock';")" "0"
send_wait s2 "nd_com" "COMMIT;"
release_s1
send_wait s1 "s1_com_6a" "COMMIT;"
check "6 NULLS DISTINCT: both NULL rows kept" \
	"$(ctl_q "SELECT count(*) FROM s_nd WHERE k IS NULL;")" "2"

# NULLS NOT DISTINCT (PG15+): NULL keys DO conflict -> serialize and violate.
if [ "$PG_MAJOR" -ge 15 ]; then
	ctl_q "CREATE TABLE s_nn (k int) USING columnar;" >/dev/null
	ctl_q "CREATE UNIQUE INDEX s_nn_uidx ON s_nn (k) NULLS NOT DISTINCT;" >/dev/null
	HKEY=1012
	pause_s1 "INSERT INTO s_nn SELECT CASE WHEN g=2 THEN cq_block($HKEY, -1) ELSE NULL END FROM generate_series(1,2) g;"
	send_wait s2 "nn_beg" "BEGIN;"
	send s2 "INSERT INTO s_nn VALUES (NULL);"
	send s2 "\\echo <<nn_ins>>"
	wait_blocked cc_s2
	check "6 NULLS NOT DISTINCT: second NULL key serializes" \
		"$(ctl_q "SELECT count(*) FROM pg_stat_activity WHERE application_name='cc_s2' AND wait_event_type='Lock';")" "1"
	release_s1
	send_wait s1 "s1_com_6b" "COMMIT;"
	wait_sentinel s2 nn_ins
	check "6 NULLS NOT DISTINCT: second NULL got unique_violation" \
		"$(out_has s2 's_nn_uidx' && echo yes || echo no)" "yes"
	send_wait s2 "nn_rb" "ROLLBACK;"
	check "6 NULLS NOT DISTINCT: exactly one NULL row" \
		"$(ctl_q "SELECT count(*) FROM s_nn WHERE k IS NULL;")" "1"
else
	echo "SKIP  6 NULLS NOT DISTINCT test (PostgreSQL < 15)"
fi

# ===========================================================================
# Scenario 7: same-statement duplicate is still caught (no regression).
# ===========================================================================
ctl_q "CREATE TABLE s_ss (k int) USING columnar;" >/dev/null
ctl_q "CREATE UNIQUE INDEX s_ss_uidx ON s_ss (k);" >/dev/null
ss_err="$(run_pg "$SPSQL -c \"INSERT INTO s_ss SELECT 7 FROM generate_series(1,2);\" 2>&1" )"
check "7 same-statement duplicate raises unique_violation" \
	"$(echo "$ss_err" | grep -qF 's_ss_uidx' && echo yes || echo no)" "yes"
check "7 same-statement duplicate inserted no rows" \
	"$(ctl_q "SELECT count(*) FROM s_ss;")" "0"

# ===========================================================================
# Scenario 8: aborted first inserter -> second inserter succeeds.
# Here T1's INSERT completes and flushes (uncommitted) and holds the key lock;
# T2 blocks on that lock; T1 ROLLBACK releases it and discards the stripe, so
# T2's re-check finds no committed row and the insert succeeds.
# ===========================================================================
ctl_q "CREATE TABLE s_ab (k int) USING columnar;" >/dev/null
ctl_q "CREATE UNIQUE INDEX s_ab_uidx ON s_ab (k);" >/dev/null
send_wait s1 "ab_beg" "BEGIN;"
send_wait s1 "ab_ins" "INSERT INTO s_ab VALUES (1);"   # completes, flushes, holds lock
send_wait s2 "ab_beg2" "BEGIN;"
send s2 "INSERT INTO s_ab VALUES (1);"
send s2 "\\echo <<ab_ins2>>"
wait_blocked cc_s2
check "8 second inserter blocks behind uncommitted first" \
	"$(ctl_q "SELECT count(*) FROM pg_stat_activity WHERE application_name='cc_s2' AND wait_event_type='Lock';")" "1"
send_wait s1 "ab_rb" "ROLLBACK;"   # first inserter aborts; row never committed
wait_sentinel s2 ab_ins2           # T2's INSERT now completes (should succeed)
send_wait s2 "ab_com" "COMMIT;"
check "8 after first aborts, second insert succeeds; one row" \
	"$(ctl_q "SELECT count(*) FROM s_ab WHERE k=1;")" "1"

send s1 "\\q"
send s2 "\\q"
send sh "\\q"

echo
if [ "$fail" = 0 ]; then
	echo "UNIQUE_CONC TEST PASSED"
else
	echo "UNIQUE_CONC TEST FAILED"
fi
exit "$fail"
