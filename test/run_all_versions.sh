#!/usr/bin/env bash
#
# pgColumnar multi-version build-and-test matrix.
#
# For each pg_config given, this builds the extension fresh in a per-major build
# directory (so nothing leaks between majors) and runs every suite
# (smoke + phase2..phase6 + audit + concurrency + unique_conc + differential +
# recovery + fuzz) against it. It prints a
# per-version PASS/FAIL line and
# a final summary table, and exits non-zero if any version fails to build or any
# suite fails.
#
# Usage:
#   test/run_all_versions.sh [PG_CONFIG ...]
#
# With no arguments it uses a default list covering PostgreSQL 15 through 19.
# PostgreSQL 13 (end of life) and 14 are no longer in the default matrix.
# Each PG_CONFIG must point at an assert-enabled build to exercise the asserts.
# Run as a user that may "runuser -u postgres" (e.g. root); the suites start
# throwaway clusters as the postgres OS user.
#
# Written fresh for pgColumnar. It reuses no upstream test harness.

set -uo pipefail

# One name per line, and it must stay that way.
#
# This was a single backslash-continued line, so every pull request that adds a
# suite edited the same line and any two of them conflicted by construction. It
# happened four times in one day (#459 vs #462, #460 vs #462, #462 vs #468, and
# #444 behind them) and four more times the night #446, #468 and #444 landed.
#
# The resolution was the dangerous part, not the conflict. Appending the new name
# after the closing paren is valid shell that `bash -n` accepts, and it does not
# merely leave a stray command: `NAME=value cmd` scopes the assignment to that
# command, and an array literal is no exception, so `SUITES=(...) my_suite` leaves
# SUITES UNSET and the matrix runs nothing at all. harness_selftest pins that (#469).
#
# One name per line means two pull requests adding two suites touch two different
# lines and merge cleanly. Do not re-flow this into one line to save space.
SUITES=(
	advisory_lock_class
	alter_column_type
	analyze_differential
	analyze_function
	analyze_reltuples
	analyze_stats
	arrow_export
	arrow_import
	arrow_nested
	arrow_nested_import
	audit
	bench_guards
	bloom_lazy
	bloom_setting
	bloom_sizing
	cancel_decode
	column_projection
	concurrency
	concurrent_diff
	corruption
	decode_interrupts
	differential
	docs_style
	drop_cleanup
	encode_effort
	encode_invariants
	fk_referencing
	fsst_margin
	fsst_verdict_cache
	fuzz
	fuzz_arrow
	fuzz_parquet
	generated_columns
	hardening
	harness_selftest
	import_deferred
	import_exclusion
	index_only
	isolation
	logical_subscriber
	native_agg
	native_agg_addcolumn
	native_agg_deletes
	native_backend_crash
	native_bloom
	native_cancel
	native_cluster
	native_compact
	native_ctas
	native_dml
	native_encoding
	native_fastdecode
	native_fetch_bigcap
	native_fetch_cache
	native_fetch_interrupt
	native_fetch_position
	native_fetch_projection
	native_format
	native_gap
	native_groupagg
	native_index
	native_index_projection
	native_ios
	native_late_materialization
	native_lazy_slot
	native_ownership
	native_parquet_codecs
	native_parquet_fdw
	native_parquet_flba
	native_parquet_hardening
	native_parquet_multifile
	native_parquet_partition
	native_parquet_projection
	native_parquet_pushdown
	native_parquet_schema
	native_parquet_stack
	native_parquet_streaming
	native_parquet_units
	native_projection
	native_read_parquet
	native_reclaim
	native_reclaim_cycles
	native_reclaim_frag
	native_reclaim_reconcile
	native_recluster
	native_repack
	native_rewrite
	native_rewrite_conc
	native_roundtrip
	native_skip
	native_sort_by
	native_truncate
	native_vacuum_race
	native_vecskip
	native_writer
	native_zonemap
	objstore_module
	objstore_stash_recovery
	parallel
	parallel_copy
	parallel_degree
	parallel_export_parquet
	parallel_vector_agg
	parquet_export
	parquet_import
	parquet_nested
	parquet_nested_import
	pg19_vacuum_options
	pg_dump_roundtrip
	phase2
	phase3
	phase4
	phase5
	phase6
	planner_choice_quality
	projections
	pushdown_report
	read_stream
	recluster_extent
	recovery
	replication
	rewrite_group_scan
	row_triggers
	server_file_privilege
	smoke
	sort_status
	sorted_projection
	temporal
	ungrouped_vector_agg
	unique_conc
	wal_envelope
	write_fsst_compressed
	write_minmax_fastpath
	zonemap_cost
)


# ---------------------------------------------------------------------------
# Run from a private copy of this script, and refuse to run twice at once.
#
# Both guards exist because both failures happened, and neither announced
# itself as what it was.
#
# bash reads a script incrementally as it executes it, so editing this file
# while a run is in progress corrupts the run in place. A gate died at
# "line 139: `done'" with the file on disk perfectly valid, because the bytes
# had moved under the interpreter between one read and the next. Re-executing
# from a copy makes an in-flight run immune to whatever happens to the original.
#
# And two runs at once quietly ruin each other: they contend for clusters and
# ports, and the symptom is a suite failing with no named check -- a wall of
# ERROR: database "regress" already exists and a red result that looks exactly
# like a real one. Three false reds in one day were traced to this. A run now
# leaves a lock naming its pid, so the second one says so and stops instead of
# producing a result nobody can trust.
# ---------------------------------------------------------------------------

PGC_RUN_LOCK="${PGC_RUN_LOCK:-/tmp/pgcolumnar-run_all_versions.lock}"

# --stop: the supported way to end a run in progress.
#
# It exists because the alternative was pkill, and pkill is wrong here twice: the
# driver re-executes itself from /tmp under a generated name, so the obvious
# pattern misses it, and killing postmasters directly bypasses pg_ctl. This reads
# the lock, signals the owner, and lets the owner's own trap stop the suites and
# their clusters properly.
# --list-suites: print the matrix's suite list, one name per line, then exit.
#
# It exists so that nothing has to parse this array a second time. A gate that
# re-implements bash's array parsing in awk disagrees with bash on the very
# mistake this array invites: a name after the closing paren is a stray COMMAND
# to bash and a member to a text parser, so the gate passed while the suite
# silently never ran. Asking the runner means there is one parser, bash's, and
# no way for the two to drift.
#
# Handled BEFORE the run lock on purpose. harness_selftest calls this from
# inside a running matrix, and taking the lock there would refuse to answer.
if [ "${1:-}" = "--list-suites" ]; then
	printf '%s\n' "${SUITES[@]}"
	exit 0
fi

if [ "${1:-}" = "--stop" ]; then
	if [ ! -e "$PGC_RUN_LOCK" ]; then
		echo "no matrix run in progress (no lock at $PGC_RUN_LOCK)"
		exit 0
	fi
	_owner="$(sed -n 1p "$PGC_RUN_LOCK" 2>/dev/null)"
	if [ -z "$_owner" ] || ! kill -0 "$_owner" 2>/dev/null; then
		echo "stale lock from pid ${_owner:-unknown}; removing it"
		rm -f "$PGC_RUN_LOCK"
		exit 0
	fi
	echo "stopping matrix run (pid $_owner)"
	kill -TERM "$_owner" 2>/dev/null
	for _i in $(seq 1 60); do
		kill -0 "$_owner" 2>/dev/null || break
		sleep 1
	done
	if kill -0 "$_owner" 2>/dev/null; then
		echo "pid $_owner did not exit after 60s; sending KILL" >&2
		kill -KILL "$_owner" 2>/dev/null
	fi
	rm -f "$PGC_RUN_LOCK"
	echo "stopped"
	exit 0
fi

if [ -z "${PGC_RUN_REEXEC:-}" ]; then
	# Take the lock before copying, so two starts cannot both decide they are first.
	if [ -e "$PGC_RUN_LOCK" ]; then
		_holder="$(sed -n 1p "$PGC_RUN_LOCK" 2>/dev/null)"
		if [ -n "$_holder" ] && kill -0 "$_holder" 2>/dev/null; then
			echo "FATAL: a matrix run is already in progress (pid $_holder)" >&2
			sed -n '2,$p' "$PGC_RUN_LOCK" >&2 2>/dev/null
			echo "       wait for it, or kill $_holder, or set PGC_RUN_LOCK to run" >&2
			echo "       against a separate tree on a different port." >&2
			exit 1
		fi
		echo "note: taking over a stale lock from pid ${_holder:-unknown}" >&2
		rm -f "$PGC_RUN_LOCK"
	fi

	{
		echo "$$"
		echo "       started: $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
		echo "       tree:    $(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
		echo "       args:    ${*:-<default matrix>}"
	} > "$PGC_RUN_LOCK"

	# The lock is removed only by the process that took it, so a stale-lock
	# takeover cannot have its lock deleted by the run it replaced.
	# INT and TERM as well as EXIT. A bash EXIT trap does not run when the shell
	# is terminated by an untrapped signal, so without these a killed run left
	# its lock behind and the next run refused to start against a pid that was
	# already gone.
	trap 'if [ "$(sed -n 1p "$PGC_RUN_LOCK" 2>/dev/null)" = "$$" ]; then rm -f "$PGC_RUN_LOCK"; fi' EXIT INT TERM

	_self="$(mktemp "/tmp/pgcolumnar-run_all_versions.$$.XXXXXX.sh")"
	cp "${BASH_SOURCE[0]}" "$_self"
	chmod +x "$_self"
	# Run the copy in the background and forward signals to it, rather than
	# calling it directly. --stop signals this parent, but the child is the one
	# holding the suite jobs, so a TERM that stops here and not there is exactly
	# the orphaning this is meant to prevent.
	PGC_RUN_REEXEC=1 \
	PGC_RUN_SRCDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)" \
	PGC_RUN_LOCK="$PGC_RUN_LOCK" \
	PGC_RUN_OWNER="$$" \
		bash "$_self" "$@" &
	_child=$!
	_forward_stop() {
		kill -TERM "$_child" 2>/dev/null
		wait "$_child" 2>/dev/null
		if [ "$(sed -n 1p "$PGC_RUN_LOCK" 2>/dev/null)" = "$$" ]; then
			rm -f "$PGC_RUN_LOCK"
		fi
		exit 130
	}
	trap _forward_stop INT TERM
	wait "$_child"
	_rc=$?
	rm -f "$_self"
	exit $_rc
fi

# Re-executed from the copy: the lock belongs to the parent, which removes it,
# and the tree to test is the original one rather than wherever the copy landed.
trap - EXIT

# ---------------------------------------------------------------------------
# Stopping a run
# ---------------------------------------------------------------------------
#
# This driver used to have no cleanup at all -- the line above dropped the
# parent's EXIT trap and put nothing in its place -- and the suites it starts run
# as background jobs. So killing the driver did not stop them: they were
# reparented and carried on, holding their clusters and their ports, writing into
# the log someone was reading. Both of those cost real time on this branch. One
# orphaned run's output interleaved into a live run's log and produced a FAIL that
# belonged to neither; another held the run lock for twenty minutes after it was
# "killed", so three matrices refused to start and reported nothing.
#
# The only way to stop a run was pkill, which is the wrong instrument twice over:
# it misses this process (re-executed from /tmp under a generated name) and it
# kills postmasters out from under pg_ctl instead of asking them to stop.
#
# So: a signal now stops the suites, then stops their clusters with pg_ctl -- the
# supported path, using the pg_ctl matching each cluster's own major, read from
# its PG_VERSION rather than guessed.
pgc_stop_clusters() {
	local d sub datadir ver pgctl stopped=0
	for d in /tmp/pgcolumnar-test.*/; do
		[ -d "$d" ] || continue
		for sub in data standby restore; do
			datadir="$d$sub"
			[ -f "$datadir/postmaster.pid" ] || continue
			ver="$(sed -n 1p "$datadir/PG_VERSION" 2>/dev/null)"
			pgctl="/usr/local/pg${ver}/bin/pg_ctl"
			# Fall back to any pg_ctl only if the versioned one is absent; a
			# mismatched pg_ctl refuses rather than corrupting anything.
			[ -x "$pgctl" ] || pgctl="$(command -v pg_ctl 2>/dev/null)"
			[ -n "$pgctl" ] && [ -x "$pgctl" ] || continue
			if [ "$(id -u)" = 0 ] && id postgres >/dev/null 2>&1; then
				su postgres -c "'$pgctl' -D '$datadir' stop -m immediate -w -t 30" >/dev/null 2>&1
			else
				"$pgctl" -D "$datadir" stop -m immediate -w -t 30 >/dev/null 2>&1
			fi
			stopped=$((stopped + 1))
		done
	done
	[ "$stopped" -gt 0 ] && echo "stopped $stopped cluster(s) with pg_ctl" >&2
	return 0
}

pgc_run_cleanup() {
	local j
	trap - INT TERM EXIT
	echo "" >&2
	echo "matrix interrupted -- stopping suites and their clusters" >&2
	# Stop the suites first so they cannot start anything else, then their
	# clusters. Their own EXIT traps handle the ones they can still reach.
	for j in $(jobs -p 2>/dev/null); do kill -TERM "$j" 2>/dev/null; done
	sleep 2
	for j in $(jobs -p 2>/dev/null); do kill -KILL "$j" 2>/dev/null; done
	pgc_stop_clusters
	if [ -n "${PGC_CUR_BUILDDIR:-}" ] && [ -d "$PGC_CUR_BUILDDIR" ]; then
		rm -rf "$PGC_CUR_BUILDDIR"
		echo "removed the in-progress build directory" >&2
	fi
	exit 130
}
trap pgc_run_cleanup INT TERM

SRCDIR="${PGC_RUN_SRCDIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

# Default matrix: one assert-enabled pg_config per major, 15 through 19.
DEFAULT_CONFIGS=(
	/usr/local/pg15/bin/pg_config
	/usr/local/pg16/bin/pg_config
	/usr/local/pg17/bin/pg_config
	/usr/local/pgsql/bin/pg_config
	/usr/local/pg19/bin/pg_config
)

if [ "$#" -gt 0 ]; then
	CONFIGS=("$@")
else
	CONFIGS=("${DEFAULT_CONFIGS[@]}")
fi

# A private base port per run, bumped per suite, to avoid clashes.
#
# Derived from the run's own pid rather than fixed, because a fixed default is
# not private: two runs on one box then start at the same port and fight over
# every cluster. The lock above makes that refuse rather than happen, but the
# suites are also run individually, and they should not collide either.
#
# Below the ephemeral floor, deliberately. This is the second copy of the
# arithmetic in portlib.sh, kept because the driver re-executes itself from /tmp
# and a tree-relative source would be the fragility that re-exec removes.
#
# The old band, 40000-59999, was entirely inside /proc/sys/net/ip_local_port_range
# (32768-60999 here), so the kernel could hand a cluster's port to an outbound
# connection between the free-check and the bind. See portlib.sh for the full
# account; it is the cause of the intermittent replication failure.
_pgc_eph_floor="$(awk '{print $1}' /proc/sys/net/ipv4/ip_local_port_range 2>/dev/null)"
case "$_pgc_eph_floor" in ''|*[!0-9]*) _pgc_eph_floor=32768 ;; esac
if [ "$_pgc_eph_floor" -lt 20000 ]; then
	echo "FATAL: ephemeral floor $_pgc_eph_floor too low to carve a private port band" >&2
	echo "       /proc/sys/net/ipv4/ip_local_port_range starts at $_pgc_eph_floor; this" >&2
	echo "       needs at least 20000 so cluster ports sit below the range the kernel" >&2
	echo "       assigns to outbound connections. Raise it:" >&2
	echo "           sysctl -w net.ipv4.ip_local_port_range=\"32768 60999\"" >&2
	exit 1
fi
# Mirrors portlib.sh: main band below the auxiliary band, both below the floor.
_pgc_aux_hi=$(( _pgc_eph_floor - 1000 ))
_pgc_aux_lo=$(( _pgc_aux_hi - 2000 ))
PGC_PORT_LO=10000
PGC_PORT_HI=$(( _pgc_aux_lo - 200 ))
_pgc_walk=1500
_pgc_span=$(( PGC_PORT_HI - PGC_PORT_LO - _pgc_walk ))
BASE_PORT="${PGC_BASE_PORT:-$(( PGC_PORT_LO + (${PGC_RUN_OWNER:-$$} % _pgc_span) ))}"

# The band must hold one port per suite per major, plus the auxiliary clusters a
# suite stands up beyond its own. Asserted rather than assumed: the walk is a
# constant here and the suite list grows, so a silent overrun would put a cluster
# back inside the ephemeral range -- the exact failure this layout removes, and
# one that costs days to recognise.
_pgc_need=$(( ${#SUITES[@]} * ${#CONFIGS[@]} + 16 ))
if [ $(( BASE_PORT + _pgc_need )) -ge "$PGC_PORT_HI" ]; then
	echo "FATAL: port band too small for this run" >&2
	echo "       ${#SUITES[@]} suites x ${#CONFIGS[@]} majors needs $_pgc_need ports from $BASE_PORT," >&2
	echo "       which passes the top of the band ($PGC_PORT_HI)." >&2
	echo "       Raise net.ipv4.ip_local_port_range's floor, or set PGC_BASE_PORT lower." >&2
	exit 1
fi

overall=0
declare -a SUMMARY

# Suites whose checks compare wall-clock times, and which therefore cannot be run
# beside five others.
#
# The rest of the matrix runs PGC_JOBS suites at once, each with its own cluster,
# so every ratio in those suites is measured under six-way contention. That is
# not a fixture problem and no threshold survives it: the same check on the same
# build measures 1.11 alone and 2.11 inside the matrix, against a bound of 2.0.
# Three suites produced red gates that way in one session, each costing a re-run
# to disprove, which is how a matrix teaches its readers to discount red.
#
# Tuning the fixtures was tried first and does not work. On the group-doubling
# check, raising the fetch count to dilute the shared decode makes the ratio
# worse rather than better -- 1.08 at 6,000 fetches, 1.23 at 20,000, 1.29 at
# 40,000 -- because per-fetch cost is itself a function of group size, so more
# fetches amplify the difference instead of averaging it away. The comment in
# native_fetch_position.sh that recommended exactly that has been corrected.
#
# Not every wall-clock ratio belongs here, and the distinction is measurable
# rather than a matter of taste. native_fetch_cache asserts one on the same
# index-driven fetch with the same stopwatch, and is deliberately absent: it
# compares one big group against ten small ones, and the cache equalises
# per-fetch cost across both sides, so contention is common-mode and cancels in
# the ratio. Measured, six-way: absolute times roughly doubled and the ratio
# stayed near 1 against a bound of 3, worst case 1.15.
#
# The three below measure quantities whose per-fetch or per-query cost is itself
# a function of the thing being varied, so contention is differential and does
# not cancel. That is the test for membership: does the load move both sides of
# the ratio together?
#
# So they run alone. It costs a few minutes per major and it buys a timing
# result that means something.
is_timing_suite() {
	case "$1" in
		native_fetch_position|native_cancel|native_agg_deletes) return 0 ;;
		# planner_choice_quality's only assertion is a wall-clock ratio. Left out
		# of this list it still RAN under PGC_SKIP_TIMING, skipped the ratio, and
		# reported PASS on the strength of its premises -- so a regression of #434
		# would have been reported green by the suite that exists to catch it. A
		# suite whose subject is dropped has not passed, and the driver already
		# knows how to say that.
		planner_choice_quality) return 0 ;;
		*) return 1 ;;
	esac
}

# Suites that must not run beside five others, which is a wider set than the
# timing ones and for a second reason.
#
# The timing suites are here because a wall-clock ratio cannot be measured under
# contention. replication is here because it is heavy rather than because it
# measures anything: it stands up a SECOND cluster, runs pg_basebackup, and
# streams between the two, so at PGC_JOBS=6 it is competing with six other
# clusters for the same box. It failed roughly one full matrix in three, on a
# different major each time -- PG18, then PG19, then PG16 -- which is the
# signature of resource contention rather than a defect in the suite or the
# major.
#
# Deliberately NOT the same predicate as is_timing_suite, because
# PGC_SKIP_TIMING drops those in CI and replication must still run there. Serial
# and skipped are different properties and were one list until this suite needed
# one without the other.
# planner_choice_quality wants the same split, the other way around from
# replication. Its subject is a wall-clock ratio, so it cannot run beside five
# others and mean anything. But it is deliberately not in is_timing_suite: its
# premises are plan-shape assertions, and those are worth running in CI. Under
# PGC_SKIP_TIMING the suite drops the ratios itself, through check_timing, and
# does not execute the timed queries at all.
runs_alone() {
	case "$1" in
		replication) return 0 ;;
		# objstore_module moves the INSTALLED module aside to test the
		# not-installed and broken paths. That file is shared by every suite in
		# the run, so doing it while others execute would break them, and the
		# breakage would look like a defect in whichever suite happened to load
		# the extension at the wrong moment.
		objstore_module) return 0 ;;
		# objstore_stash_recovery arranges that same shared file into the states
		# an interrupted run leaves behind, and runs objstore_module against each.
		# It moves the module for the same reason and must be alone for it.
		objstore_stash_recovery) return 0 ;;
		*) is_timing_suite "$1" ;;
	esac
}

# How many majors were actually built and run. A matrix that ran nothing is not
# a matrix that passed, and until #418 it said "ALL VERSIONS PASSED" and exited
# 0 when every configured pg_config was missing. See the summary block.
VERSIONS_RUN=0

for pgc in "${CONFIGS[@]}"; do
	if [ ! -x "$pgc" ]; then
		echo "SKIP  $pgc (not executable)"
		SUMMARY+=("SKIP   $pgc")
		continue
	fi
	VERSIONS_RUN=$((VERSIONS_RUN + 1))

	ver="$("$pgc" --version)"
	major="$(echo "$ver" | sed -E 's/^[^0-9]*([0-9]+).*/\1/')"
	builddir="$(mktemp -d "/tmp/pgcolumnar-matrix-${major}.XXXXXX")"
	# Recorded so an interrupted run can remove it. A completed major removes
	# its own at the bottom of the loop; one that is stopped part-way used to
	# leave a full source tree and install behind in /tmp.
	PGC_CUR_BUILDDIR="$builddir"

	echo "==================================================================="
	echo "== $ver"
	echo "== pg_config=$pgc"
	echo "== builddir=$builddir"
	echo "==================================================================="

	# Fresh copy of the tree so each major builds in isolation.
	cp -a "$SRCDIR/." "$builddir/"
	make -C "$builddir" clean PG_CONFIG="$pgc" >/dev/null 2>&1 || true

	if ! make -C "$builddir" PG_CONFIG="$pgc" >/dev/null 2>"$builddir/build.err"; then
		echo "BUILD FAILED"
		sed 's/^/    /' "$builddir/build.err"
		SUMMARY+=("FAIL   PG$major  (build)")
		overall=1
		continue
	fi
	# Any compiler warning is a failure for this matrix.
	if grep -q "warning:" "$builddir/build.err"; then
		echo "BUILD WARNINGS"
		grep "warning:" "$builddir/build.err" | sed 's/^/    /'
		SUMMARY+=("FAIL   PG$major  (warnings)")
		overall=1
		continue
	fi

	# Install the extension once for this version; the suites then skip their own
	# build/install (PGC_SKIP_BUILD) and run in parallel, each in its own throwaway
	# cluster on its own port. This keeps per-suite cluster isolation (crash and
	# recovery suites need it) while removing the redundant per-suite rebuild and
	# the serial initdb/start bottleneck. PGC_JOBS controls the degree.
	if ! make -C "$builddir" install PG_CONFIG="$pgc" >/dev/null 2>>"$builddir/build.err"; then
		echo "INSTALL FAILED"
		sed 's/^/    /' "$builddir/build.err"
		SUMMARY+=("FAIL   PG$major  (install)")
		overall=1
		continue
	fi

	verfail=0
	results=""
	maxjobs="${PGC_JOBS:-6}"
	for s in "${SUITES[@]}"; do
		if runs_alone "$s"; then
			continue
		fi
		# throttle to maxjobs concurrent suites
		while [ "$(jobs -rp | wc -l)" -ge "$maxjobs" ]; do wait -n; done
		port=$((BASE_PORT++))
		(
			PGC_SKIP_BUILD=1 PGC_PORT="$port" \
				bash "$builddir/test/${s}.sh" "$pgc" >"$builddir/${s}.log" 2>&1
			echo $? >"$builddir/${s}.rc"
		) &
	done
	wait

	# Then the timing-sensitive suites, one at a time, with nothing else running.
	#
	# PGC_SKIP_TIMING=1 drops them entirely. That exists for shared CI hardware,
	# where the wall-clock ratios these three assert cannot be trusted: a runner
	# is noisy by construction and a gate that goes red for reasons unrelated to
	# the change teaches its readers to discount red, which is worse than not
	# running it. They stay in every local run, which is where the numbers mean
	# something.
	for s in "${SUITES[@]}"; do
		if ! runs_alone "$s"; then
			continue
		fi
		# PGC_SKIP_TIMING drops the wall-clock suites only. A suite that runs
		# alone for a resource reason rather than a measurement one -- replication
		# -- still runs, because there is nothing about a shared runner that makes
		# its assertions untrustworthy, only slower.
		if [ "${PGC_SKIP_TIMING:-0}" = 1 ] && is_timing_suite "$s"; then
			echo "  SKIP  $s (PGC_SKIP_TIMING)"
			# 66, not 0. This suite did not run, and the collector has a state
			# that says so. Recording it as a pass was the same lie the zero-check
			# suites were telling, just written by the driver.
			#
			# The log is written too, because the collector requires the marker as
			# well as the status: this branch never executes the suite, so nothing
			# else would produce one and the run would be classified a failure.
			echo 66 >"$builddir/${s}.rc"
			echo "$s.sh: SKIPPED (ran no checks)" >"$builddir/${s}.log"
			continue
		fi
		port=$((BASE_PORT++))
		PGC_SKIP_BUILD=1 PGC_PORT="$port" \
			bash "$builddir/test/${s}.sh" "$pgc" >"$builddir/${s}.log" 2>&1
		echo $? >"$builddir/${s}.rc"
	done

	# collect results in suite order for a stable, readable summary
	suites_ran=0
	suites_skipped=0
	skipped_names=""
	for s in "${SUITES[@]}"; do
		_rc="$(cat "$builddir/${s}.rc" 2>/dev/null)"
		if [ "$_rc" = 0 ]; then
			echo "  PASS  $s"
			results+="$s=PASS "
			suites_ran=$((suites_ran + 1))
		elif [ "$_rc" = 66 ] && grep -q 'SKIPPED (ran no checks)' "$builddir/${s}.log" 2>/dev/null; then
			# Exit 2 is pgc_summary's third state: the suite ran no checks (#447).
			# Not a pass, because it asserted nothing. Not a failure, because a
			# major without the feature and a box without an optional dependency
			# are both supported. Counted, so the total below can say so.
			echo "  SKIP  $s (ran no checks)"
			results+="$s=SKIP "
			suites_skipped=$((suites_skipped + 1))
			skipped_names="$skipped_names $s"
		else
			echo "  FAIL  $s"
			# The failing check first, then the tail. A suite that prints a
			# diagnostic and a server-log dump on failure pushes its own FAIL
			# lines out of a 20-line tail, which is how an intermittent
			# replication failure stayed unreadable across many matrices: the
			# evidence was in the log and the summary showed everything but.
			if grep -qE '^FAIL' "$builddir/${s}.log"; then
				grep -E '^FAIL' "$builddir/${s}.log" | sed 's/^/      >> /'
			fi
			# 60, not 20: a suite that prints a failure diagnostic and a
			# server-log dump needs more room than 20 lines, and truncating it
			# is how the replication failures stayed unreadable.
			tail -60 "$builddir/${s}.log" | sed 's/^/      /'
			results+="$s=FAIL "
			# A failed suite RAN. Counting only passes here made the tally
			# contradict itself in the one case that matters. The five-major
			# matrix reported
			#
			#     PG19  suites that ran: 121 of 122 (skipped: 0)
			#
			# with temporal failing: 121 + 0 is not 122, and the failing suite was
			# in neither bucket of the count that exists to say what ran. Four
			# majors hid it, because a tally only disagrees with itself once
			# something actually fails.
			suites_ran=$((suites_ran + 1))
			verfail=1
		fi
	done

	# How many suites actually asserted something, said out loud (#447).
	#
	# #422 added this one level up, after a matrix reported ALL VERSIONS PASSED
	# having run none of them. The same hole existed per-suite: fifteen suites
	# report a verdict without running a check when pyarrow is absent, and the old
	# per-version line counted them among the passes. A count that includes suites
	# nobody ran is the thing this project keeps having to unlearn.
	echo "  suites that ran: $suites_ran of ${#SUITES[@]} (skipped: $suites_skipped)"
	if [ "$suites_skipped" != 0 ]; then
		echo "  skipped:${skipped_names}"
	fi
	if [ "$suites_ran" = 0 ]; then
		echo "  NO SUITES RAN on PG$major, which is not a pass"
		verfail=1
	fi

	if [ "$verfail" = 0 ]; then
		SUMMARY+=("PASS   PG$major  ($suites_ran ran, $suites_skipped skipped)  ${results}")
	else
		SUMMARY+=("FAIL   PG$major  ($suites_ran ran, $suites_skipped skipped)  ${results}")
		overall=1
	fi
	rm -rf "$builddir"
done

# ---------------------------------------------------------------------------
# Cross-major upgrade (opt-in: PGC_RUN_UPGRADE=1)
# ---------------------------------------------------------------------------
#
# Off by default and deliberately not part of the per-PR gate. pg_upgrade needs
# two majors at once, runs the upgrade twice per pair (copy and link), and is
# expensive -- the same reason run_san.sh sits beside the matrix rather than in
# it.
#
# It is here rather than only in a checklist because docs/limitations.md now makes
# a cross-major claim -- that data written by one build reads back identically on
# any build of the same format version, across every supported major -- and
# test/pg_upgrade.sh is the only thing that tests it. A claim backed by a suite
# nobody runs is the failure mode that left native_scale dark (#257).
#
# Adjacent pairs of the majors being tested, both transfer modes: link shares the
# data files with the old cluster and copy does not, and they fail differently.
if [ "${PGC_RUN_UPGRADE:-0}" = 1 ]; then
	echo "==================================================================="
	echo "== cross-major upgrade (PGC_RUN_UPGRADE=1)"
	echo "==================================================================="

	_ucount=0
	for _i in $(seq 0 $(( ${#CONFIGS[@]} - 2 ))); do
		_old="${CONFIGS[$_i]}"
		_new="${CONFIGS[$((_i + 1))]}"
		[ -x "$_old" ] && [ -x "$_new" ] || continue
		_omaj="$("$_old" --version | sed -E 's/^[^0-9]*([0-9]+).*/\1/')"
		_nmaj="$("$_new" --version | sed -E 's/^[^0-9]*([0-9]+).*/\1/')"
		for _mode in copy link; do
			_ucount=$((_ucount + 1))
			_ulog="$(mktemp "/tmp/pgcolumnar-upgrade-${_omaj}-${_nmaj}-${_mode}.XXXXXX.log")"
			if bash "$SRCDIR/test/pg_upgrade.sh" "$_old" "$_new" "$_mode" \
				>"$_ulog" 2>&1; then
				echo "  PASS  pg_upgrade PG$_omaj -> PG$_nmaj ($_mode)"
				SUMMARY+=("PASS   upgrade PG$_omaj->PG$_nmaj ($_mode)")
				rm -f "$_ulog"
			else
				echo "  FAIL  pg_upgrade PG$_omaj -> PG$_nmaj ($_mode)"
				if grep -qE '^FAIL' "$_ulog"; then
					grep -E '^FAIL' "$_ulog" | sed 's/^/      >> /'
				fi
				tail -40 "$_ulog" | sed 's/^/      /'
				echo "      full log: $_ulog"
				SUMMARY+=("FAIL   upgrade PG$_omaj->PG$_nmaj ($_mode)")
				overall=1
			fi
		done
	done

	# Asked for the gate and got nothing: that is a failure, not a quiet pass.
	# One pg_config short of a pair, or a typo in the list, would otherwise report
	# green having upgraded nothing.
	if [ "$_ucount" = 0 ]; then
		echo "  FAIL  PGC_RUN_UPGRADE=1 but no adjacent pair of majors was runnable"
		SUMMARY+=("FAIL   upgrade (no runnable pair)")
		overall=1
	fi

	# Extension upgrade, on the same opt-in switch and for the same reason.
	# test/extension_upgrade.sh had pg_upgrade's exemption from the registration
	# check without pg_upgrade's invocation, so nothing ran it (#396). A guard
	# nobody runs is the failure mode #257 existed to close, and it matters more
	# here: the break it catches is invisible until a user upgrades.
	#
	# One major is enough, so this runs once against the first config rather than
	# per pair. It builds the previous release from a throwaway clone, so it needs
	# a checkout with tags.
	_ex="${CONFIGS[0]}"
	if [ -x "$_ex" ]; then
		_exmaj="$("$_ex" --version | sed -E 's/^[^0-9]*([0-9]+).*/\1/')"
		_exlog="$(mktemp "/tmp/pgcolumnar-extupgrade-${_exmaj}.XXXXXX.log")"
		# The suite builds a previous release, so it has to be told where to find one.
		# PGC_UPGRADE_OLD_SRC carries a directory through, which is the only form that
		# works where the tree has no .git, and the documented container loop is exactly
		# that. Without it the suite falls back to a ref and cannot build one there.
		bash "$SRCDIR/test/extension_upgrade.sh" "$_ex" \
			${PGC_UPGRADE_OLD_SRC:+"$PGC_UPGRADE_OLD_SRC"} >"$_exlog" 2>&1
		_exrc=$?
		# Exit 2 is "the environment could not supply an old source", which is not a
		# product failure. It is reported as SKIP and not as PASS, because a gate that
		# reports green having run nothing is the defect this suite was written for.
		if [ "$_exrc" = 0 ]; then
			echo "  PASS  extension_upgrade PG$_exmaj"
			SUMMARY+=("PASS   extension_upgrade PG$_exmaj")
			rm -f "$_exlog"
		elif [ "$_exrc" = 2 ]; then
			echo "  SKIP  extension_upgrade PG$_exmaj"
			grep -E '^\s*(SKIP|  )' "$_exlog" | sed 's/^/      /'
			SUMMARY+=("SKIP   extension_upgrade PG$_exmaj")
			rm -f "$_exlog"
		else
			echo "  FAIL  extension_upgrade PG$_exmaj"
			grep -E '^\s*FAIL' "$_exlog" | sed 's/^/      >> /'
			tail -30 "$_exlog" | sed 's/^/      /'
			echo "      full log: $_exlog"
			SUMMARY+=("FAIL   extension_upgrade PG$_exmaj")
			overall=1
		fi
	else
		echo "  FAIL  PGC_RUN_UPGRADE=1 but no runnable pg_config for extension_upgrade"
		SUMMARY+=("FAIL   extension_upgrade (no runnable pg_config)")
		overall=1
	fi
fi

echo
echo "===================== MATRIX SUMMARY ============================"
for line in "${SUMMARY[@]}"; do
	echo "  $line"
done
echo "  versions run: $VERSIONS_RUN of ${#CONFIGS[@]} configured"
echo "================================================================"

# A run that built nothing is not a pass (#418).
#
# The default list names /usr/local/pg15 through /usr/local/pg19. On a box whose
# assert builds are pg15a through pg19a, every entry misses, each prints one
# SKIP line, and this block used to print ALL VERSIONS PASSED and exit 0. That
# output then gets pasted into a pull request as the gate. It is the same defect
# as check "" "" one level up, and it is worse, because this is the line people
# read instead of the checks.
#
# Reported rather than merely counted, because the count is what nobody looks at.
if [ "$VERSIONS_RUN" = 0 ]; then
	echo "NO VERSIONS RAN: every configured pg_config was missing or not executable."
	echo "  configured: ${CONFIGS[*]}"
	echo "  Pass the pg_configs this box has, e.g. test/run_all_versions.sh /usr/local/pg18a/bin/pg_config"
	exit 1
fi
if [ "$overall" = 0 ]; then
	if [ "$VERSIONS_RUN" -lt "${#CONFIGS[@]}" ]; then
		echo "VERSIONS RUN PASSED ($VERSIONS_RUN of ${#CONFIGS[@]}; the rest were skipped)"
	else
		echo "ALL VERSIONS PASSED"
	fi
else
	echo "SOME VERSIONS FAILED"
fi
exit "$overall"
