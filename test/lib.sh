#!/usr/bin/env bash
#
# pgColumnar shared test harness.
#
# Sourced by the differential/recovery/fuzz suites. Provides a throwaway
# cluster lifecycle, a heap-vs-columnar differential oracle, and pass/fail
# accounting. Written fresh for pgColumnar; it does not reuse any upstream test
# file or expected-output file.
#
# The differential oracle is the core idea: every table under test has a heap
# mirror loaded with identical data, and a query is run against both. The two
# result sets are compared as order-independent hashes, so heap acts as the
# reference oracle for pgcolumnar. This catches encode/decode and skipping bugs
# generically instead of via hardcoded expected values.
#
# Conventions for suites that source this file:
#   - Call pgc_setup "$@" once (passes through the optional PG_CONFIG arg).
#   - Use q "SQL" for a scalar, psql_run for a statement, psql_file FILE.
#   - Use diff_query LABEL "SQL with %T" to compare a heap/columnar pair.
#   - Finish with pgc_summary (exits non-zero if any check failed).
#
# Client SQL runs as the current (root) user over TCP with trust auth, so
# -f files never have postgres-ownership problems; only initdb and pg_ctl run
# as the postgres OS user.

# Do not set -e here: the suite sets its own shell options. The oracle helpers
# must not abort the run on a single mismatch or a SQL error; they record a
# failure and continue.

PGC_FAIL=0
PGC_CHECKS=0

# ---- setup / teardown ------------------------------------------------------

pgc_setup() {
	PGC_PG_CONFIG="${1:-/usr/local/pg17/bin/pg_config}"
	PGC_BINDIR="$("$PGC_PG_CONFIG" --bindir)"
	PGC_PORT="${PGC_PORT:-54329}"
	PGC_DB="${PGC_DB:-regress}"
	PGC_LIBDIR="$(dirname "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)")"
	PGC_SRCDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

	PGC_WORKDIR="$(mktemp -d /tmp/pgcolumnar-test.XXXXXX)"
	PGC_PGDATA="$PGC_WORKDIR/data"
	PGC_LOGFILE="$PGC_WORKDIR/server.log"
	PGC_SQLDIR="$PGC_WORKDIR/sql"
	mkdir -p "$PGC_SQLDIR"
	chmod 777 "$PGC_WORKDIR" "$PGC_SQLDIR"

	echo "== pgColumnar test: $(basename "$0") =="
	echo "PG_CONFIG=$PGC_PG_CONFIG"
	echo "version=$("$PGC_PG_CONFIG" --version)"
	echo "workdir=$PGC_WORKDIR"

	echo "-- building"
	make -C "$PGC_SRCDIR" PG_CONFIG="$PGC_PG_CONFIG" >/dev/null
	echo "-- installing"
	make -C "$PGC_SRCDIR" install PG_CONFIG="$PGC_PG_CONFIG" >/dev/null

	# initdb and pg_ctl cannot run as root; use postgres when we are root.
	if [ "$(id -u)" = "0" ]; then
		PGC_RUNPG=(runuser -u postgres --)
		chown -R postgres "$PGC_WORKDIR"
		chmod 777 "$PGC_WORKDIR" "$PGC_SQLDIR"
	else
		PGC_RUNPG=(env)
	fi

	trap pgc_teardown EXIT

	echo "-- initdb"
	pgc_pg "initdb -D '$PGC_PGDATA' -A trust" >/dev/null 2>&1
	{
		echo "port=$PGC_PORT"
		echo "listen_addresses='127.0.0.1'"
		echo "shared_preload_libraries='pgcolumnar'"
		# Deterministic text output so heap and columnar hashes match.
		echo "extra_float_digits=3"
		echo "timezone='UTC'"
		echo "datestyle='ISO, MDY'"
		echo "bytea_output='hex'"
		# Keep planner honest but let small tables use the custom scan.
		echo "max_parallel_workers_per_gather=0"
		# The native (PGCN v1) format is the instance default (D6f): a bare
		# columnar table with no explicit format_version option is written
		# native. A run selects the legacy 2.2 line for the lib.sh-sourcing
		# suites with PGC_NATIVE=0. A table's own option still wins, and a
		# storage that already holds data keeps its on-disk format.
		if native_mode; then
			echo "pgcolumnar.default_format_version=1"
		else
			echo "pgcolumnar.default_format_version=0"
		fi
	} | pgc_pg "cat >> '$PGC_PGDATA/postgresql.conf'"

	# Start, retrying a few times: under rapid cluster churn (the version matrix
	# runs many throwaway clusters back to back) a start can transiently fail to
	# bind or acquire resources before the previous cluster is fully gone.
	echo "-- start"
	{
		local _a
		for _a in 1 2 3 4 5 6 7 8; do
			if pgc_pg "pg_ctl -D '$PGC_PGDATA' -l '$PGC_LOGFILE' start -w" >/dev/null 2>&1; then
				break
			fi
			echo "-- start attempt $_a failed; retrying on a fresh port"
			pgc_pg "pg_ctl -D '$PGC_PGDATA' stop -m immediate -w" >/dev/null 2>&1 || true
			# the assigned port was taken by another cluster; pick a new one
			PGC_PORT=$(( 2048 + (PGC_PORT + 1 + RANDOM % 40000) % 60000 ))
			pgc_pg "sed -i 's/^port=.*/port=$PGC_PORT/' '$PGC_PGDATA/postgresql.conf'"
			sleep 1
		done
	}
	# Wait until the postmaster actually accepts connections, then create the
	# test database and verify it exists. Under heavy parallel churn pg_ctl -w
	# can return just before the server is connectable, which would otherwise
	# leave CREATE DATABASE a silent no-op and every later query failing with
	# "database does not exist".
	{
		local _a

		for _a in $(seq 1 30); do
			if psql_admin "SELECT 1;" >/dev/null 2>&1; then
				break
			fi
			sleep 1
		done
		for _a in $(seq 1 10); do
			if [ "$(psql_admin "SELECT 1 FROM pg_database WHERE datname = '$PGC_DB';" 2>/dev/null | tr -dc 0-9)" = "1" ]; then
				break
			fi
			psql_admin "CREATE DATABASE $PGC_DB;" >/dev/null 2>&1 || true
			sleep 1
		done
	}
	psql_run "CREATE EXTENSION pgcolumnar;" >/dev/null
}

pgc_teardown() {
	pgc_pg "pg_ctl -D '$PGC_PGDATA' stop -m immediate -w" >/dev/null 2>&1 || true
	rm -rf "$PGC_WORKDIR"
}

# Run a command as the postgres OS user (for initdb/pg_ctl).
pgc_pg() {
	"${PGC_RUNPG[@]}" env PATH="$PGC_BINDIR:$PATH" bash -lc "$1"
}

# ---- SQL helpers (run as root over TCP, trust auth) ------------------------

PGC_PSQL_BASE() {
	echo "psql -h 127.0.0.1 -p $PGC_PORT -U postgres"
}

# Run a statement against the test database, stop on error.
psql_run() {
	env PATH="$PGC_BINDIR:$PATH" psql -h 127.0.0.1 -p "$PGC_PORT" -U postgres \
		-d "$PGC_DB" -v ON_ERROR_STOP=1 -q -c "$1"
}

# Run a statement against the maintenance database (for CREATE DATABASE etc.).
psql_admin() {
	env PATH="$PGC_BINDIR:$PATH" psql -h 127.0.0.1 -p "$PGC_PORT" -U postgres \
		-d postgres -v ON_ERROR_STOP=1 -q -c "$1"
}

# Scalar query: echo a single value (empty on error).
q() {
	env PATH="$PGC_BINDIR:$PATH" psql -h 127.0.0.1 -p "$PGC_PORT" -U postgres \
		-d "$PGC_DB" -At -c "$1" 2>/dev/null || true
}

# Run a SQL file, returning At output.
psql_file() {
	env PATH="$PGC_BINDIR:$PATH" psql -h 127.0.0.1 -p "$PGC_PORT" -U postgres \
		-d "$PGC_DB" -At -f "$1" 2>/dev/null || true
}

# ---- assertions ------------------------------------------------------------

check() {
	local name="$1" got="$2" want="$3"
	PGC_CHECKS=$((PGC_CHECKS + 1))
	if [ "$got" = "$want" ]; then
		echo "PASS  $name"
	else
		echo "FAIL  $name: got [$got] want [$want]"
		PGC_FAIL=1
	fi
}

# Order-independent set hash of an arbitrary query's result. The row is cast to
# text as a whole composite so any column list works. No single quotes are used
# in the wrapper (dollar-quoting + chr(10)) so the inner query may contain its
# own quotes freely.
pgc_set_hash() {
	local query="$1" out
	out="$PGC_SQLDIR/h.$$.$RANDOM.sql"
	cat > "$out" <<SQL
SELECT coalesce(md5(string_agg(t, chr(10) ORDER BY t)), \$e\$EMPTY\$e\$)
FROM (SELECT _row::text AS t FROM ( $query ) _row) _s;
SQL
	psql_file "$out"
	rm -f "$out"
}

# diff_query LABEL "QUERY with %T placeholder for the table name"
# Runs QUERY against t_heap and t_col and asserts identical result sets.
diff_query() {
	local label="$1" tmpl="$2"
	local hq hc
	hq="$(pgc_set_hash "${tmpl//%T/t_heap}")"
	hc="$(pgc_set_hash "${tmpl//%T/t_col}")"
	# A blank result means the query errored; surface it as a distinct value.
	[ -z "$hq" ] && hq="HEAP_ERROR"
	[ -z "$hc" ] && hc="COL_ERROR"
	check "$label" "$hc" "$hq"
}

# ---- pair construction -----------------------------------------------------

# make_pair "COLUMN DEFS" ["WITH options for columnar"]
# Creates t_heap (heap) and t_col (columnar) with the same schema.
make_pair() {
	local defs="$1"
	psql_run "DROP TABLE IF EXISTS t_heap; DROP TABLE IF EXISTS t_col;"
	psql_run "CREATE TABLE t_heap ($defs) USING heap;"
	psql_run "CREATE TABLE t_col  ($defs) USING pgcolumnar;"
}

# load_pair "INSERT SELECT body" : insert identical rows into both. The body is
# everything after INSERT INTO <t>, e.g. "SELECT g, g::text FROM generate_series(1,10) g".
# Data is generated once into heap, then copied to columnar, so both hold
# byte-identical logical contents regardless of any volatile generators.
load_pair() {
	local body="$1"
	psql_run "INSERT INTO t_heap $body;"
	psql_run "INSERT INTO t_col SELECT * FROM t_heap;"
}

# Storage id for a columnar relation by name.
storage_id_of() {
	q "SELECT pgcolumnar.get_storage_id('$1');"
}

# True when the harness is running in native-format mode. Native (PGCN v1) is
# the default (D6f); a run selects the legacy 2.2 line with PGC_NATIVE=0.
native_mode() { [ "${PGC_NATIVE:-1}" != "0" ]; }

# Number of stripes physically written for a columnar relation (default t_col).
# The native (PGCN v1) row group is the stripe's counterpart and honors the same
# stripe_row_limit, so in native mode this counts row groups (D6e).
stripe_count() {
	local rel="${1:-t_col}"
	if native_mode; then
		q "SELECT count(*) FROM pgcolumnar.row_group
		   WHERE storage_id = pgcolumnar.get_storage_id('$rel');"
	else
		q "SELECT count(*) FROM pgcolumnar.stripe
		   WHERE storage_id = pgcolumnar.get_storage_id('$rel');"
	fi
}

# Number of chunk groups written for a columnar relation (default t_col). The
# native vector is the chunk group's counterpart and honors the same
# chunk_group_row_limit, so in native mode this counts vectors, one per column 0
# per-vector zone map, across all row groups (D6e).
chunk_group_count() {
	local rel="${1:-t_col}"
	if native_mode; then
		q "SELECT count(*) FROM pgcolumnar.zone_map
		   WHERE storage_id = pgcolumnar.get_storage_id('$rel')
		     AND vector_index >= 0 AND column_index = 0;"
	else
		q "SELECT count(*) FROM pgcolumnar.chunk_group
		   WHERE storage_id = pgcolumnar.get_storage_id('$rel');"
	fi
}

# ---- summary ---------------------------------------------------------------

pgc_summary() {
	echo
	echo "checks run: $PGC_CHECKS"
	if [ "$PGC_FAIL" = "0" ]; then
		echo "$(basename "$0"): PASSED"
	else
		echo "$(basename "$0"): FAILED"
		echo "---- server log tail ----"
		pgc_pg "tail -40 '$PGC_LOGFILE'" 2>/dev/null || true
	fi
	exit $PGC_FAIL
}
