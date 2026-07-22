#!/usr/bin/env bash
#
# pgColumnar phase 6 test: vectorized execution. Covers the vectorized scan and
# filter fast path and the vectorized aggregates (count, sum, avg, min, max)
# computed directly over decoded chunk-group arrays, the optional decompressed-
# chunk cache, and the scalar fallback for anything not vectorized.
#
# The governing property is that vectorization never changes a result: every
# check runs a query with pgcolumnar.enable_vectorization on and off and asserts
# the two outputs are identical (and, where a value is known, equal to it). The
# cache is checked the same way, on versus off. Aggregates and column types that
# the vectorized path does not handle (bigint/numeric sum and average, ordered-
# set and string aggregates, GROUP BY) must still return correct results through
# the ordinary scalar plan, which is asserted here too. A closing performance
# check times a large aggregate both ways and only logs the numbers; it never
# fails the suite on timing.
#
# Builds and installs the extension, spins up a throwaway PostgreSQL cluster as
# the postgres OS user, and exercises the phase 6 feature set. Written fresh for
# pgColumnar; it does not reuse any upstream test file or expected-output file.
#
# Usage:
#   test/phase6.sh [PG_CONFIG]
#
# PG_CONFIG defaults to /usr/local/pg17/bin/pg_config. Run as a user that may
# "runuser -u postgres" (e.g. root) when the current user is not postgres.

set -euo pipefail

PG_CONFIG="${1:-/usr/local/pg17/bin/pg_config}"
BINDIR="$("$PG_CONFIG" --bindir)"
PORT="${PGC_PORT:-54326}"
SRCDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

WORKDIR="$(mktemp -d /tmp/pgcolumnar-phase6.XXXXXX)"
PGDATA="$WORKDIR/data"
LOGFILE="$WORKDIR/server.log"

echo "== pgColumnar phase 6 test =="
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

cleanup() {
	run_pg "pg_ctl -D '$PGDATA' stop -m immediate -w" >/dev/null 2>&1 || true
	rm -rf "$WORKDIR"
}
trap cleanup EXIT

echo "-- initdb"
run_pg "initdb -D '$PGDATA' -A trust" >/dev/null 2>&1
run_pg "echo \"port=$PORT\" >> '$PGDATA/postgresql.conf'"
run_pg "echo \"shared_preload_libraries='pgcolumnar'\" >> '$PGDATA/postgresql.conf'"
echo "-- start"
run_pg "pg_ctl -D '$PGDATA' -l '$LOGFILE' start -w" >/dev/null
run_pg "createdb -p $PORT p6"

# -qAtX: quiet, unaligned, tuples-only, no psqlrc, so a value is exactly output.
PSQL="psql -p $PORT -d p6 -qAtX -v ON_ERROR_STOP=1"
q() { run_pg "$PSQL -c \"$1\""; }

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

# Run a query with vectorization on and with it off; assert the two results are
# identical (and non-empty), and optionally equal to a known expected value.
eq_on_off() {
	local name="$1" query="$2" expect="${3:-}"
	local on off
	on="$(run_pg "$PSQL -c \"SET pgcolumnar.enable_vectorization=on;  $query\"")"
	off="$(run_pg "$PSQL -c \"SET pgcolumnar.enable_vectorization=off; $query\"")"
	if [ -z "$on" ] || [ "$on" != "$off" ]; then
		echo "FAIL  $name: vectorized [$on] != scalar [$off]"
		fail=1
		return
	fi
	if [ -n "$expect" ] && [ "$on" != "$expect" ]; then
		echo "FAIL  $name: got [$on] want [$expect]"
		fail=1
		return
	fi
	echo "PASS  $name: $on"
}

# Run a query with the decompressed-chunk cache on and off; assert equality.
cache_on_off() {
	local name="$1" query="$2"
	local on off
	on="$(run_pg "$PSQL -c \"SET pgcolumnar.enable_column_cache=on;  $query\"")"
	off="$(run_pg "$PSQL -c \"SET pgcolumnar.enable_column_cache=off; $query\"")"
	if [ -z "$on" ] || [ "$on" != "$off" ]; then
		echo "FAIL  $name: cache-on [$on] != cache-off [$off]"
		fail=1
		return
	fi
	echo "PASS  $name: $on"
}

q "CREATE EXTENSION pgcolumnar;" >/dev/null

# ---------------------------------------------------------------------------
# A multi-chunk-group table: 50000 rows at the default 10000-row chunk group is
# 5 chunk groups, so the vectorized path really iterates several groups and the
# min/max skip list has something to remove under a filter.
# ---------------------------------------------------------------------------
echo "-- multi-chunk-group table"
q "CREATE TABLE t (id int, s smallint, big bigint, f float8, n numeric, label text) USING pgcolumnar;" >/dev/null
q "INSERT INTO t SELECT g, (g%1000)::smallint, g::bigint, g*0.5, (g||'.75')::numeric, 'row'||g
     FROM generate_series(1,50000) g;" >/dev/null
check "row count" "$(q "SELECT count(*) FROM t;")" "50000"
check "vectors formed" \
	"$(q "SELECT count(*) FROM pgcolumnar.zone_map WHERE storage_id=pgcolumnar.get_storage_id('t') AND vector_index >= 0 AND column_index = 0;")" "5"

# ---------------------------------------------------------------------------
# Vectorized aggregates equal the scalar aggregates, no filter.
# ---------------------------------------------------------------------------
echo "-- vectorized aggregates match scalar (no filter)"
eq_on_off "count(*)"        "SELECT count(*) FROM t;"                 "50000"
eq_on_off "count(id)"       "SELECT count(id) FROM t;"               "50000"
eq_on_off "sum(id)"         "SELECT sum(id) FROM t;"                 "1250025000"
eq_on_off "sum(smallint)"   "SELECT sum(s) FROM t;"
eq_on_off "avg(id)"         "SELECT avg(id) FROM t;"
eq_on_off "min(id),max(id)" "SELECT min(id), max(id) FROM t;"        "1|50000"
eq_on_off "min/max text"    "SELECT min(label), max(label) FROM t;"
eq_on_off "several at once"  "SELECT count(*), sum(id), avg(id), min(id), max(id) FROM t;"

# ---------------------------------------------------------------------------
# Vectorized aggregates equal the scalar aggregates with a pushed-down filter.
# The filter also drives chunk-group skipping, which must not change the answer.
# ---------------------------------------------------------------------------
echo "-- vectorized aggregates match scalar (with filter)"
eq_on_off "count filtered"  "SELECT count(*) FROM t WHERE id BETWEEN 12000 AND 12100;" "101"
eq_on_off "sum filtered"    "SELECT sum(id) FROM t WHERE id > 49990;"
eq_on_off "avg filtered"    "SELECT avg(id) FROM t WHERE id <= 25000;"
eq_on_off "min/max filtered" "SELECT min(id), max(id) FROM t WHERE id BETWEEN 30000 AND 40000;" "30000|40000"
eq_on_off "filter other col" "SELECT sum(id) FROM t WHERE s = 3;"
eq_on_off "multi-predicate"  "SELECT count(*), sum(id) FROM t WHERE id > 100 AND id < 200;" "99|14850"

# ---------------------------------------------------------------------------
# The vectorized scan (row output, not aggregate) returns exactly the same rows
# as the scalar scan. Compared over an md5 of the ordered, concatenated rows so
# any difference in row set, values, or nulls is caught.
# ---------------------------------------------------------------------------
echo "-- vectorized scan returns exactly the scalar rows"
eq_on_off "full table scan" \
	"SELECT md5(string_agg(id||'|'||label||'|'||big::text, ',' ORDER BY id)) FROM t;"
eq_on_off "filtered projected scan" \
	"SELECT md5(string_agg(id||'|'||label, ',' ORDER BY id)) FROM t WHERE id BETWEEN 12000 AND 12500;"
check "filtered scan exact value" "$(q "SELECT label FROM t WHERE id = 12345;")" "row12345"
check "filtered scan exact count" "$(q "SELECT count(*) FROM (SELECT id FROM t WHERE id BETWEEN 12000 AND 12500) x;")" "501"

# ---------------------------------------------------------------------------
# NULLs are handled correctly in both aggregates and filters. A fifth of the ids
# are NULL, and a third of the codes are NULL, in independent patterns.
# ---------------------------------------------------------------------------
echo "-- NULL handling in aggregates and filters"
q "CREATE TABLE nt (a int, code int, txt text) USING pgcolumnar;" >/dev/null
q "INSERT INTO nt SELECT
     CASE WHEN g % 5 = 0 THEN NULL ELSE g END,
     CASE WHEN g % 3 = 0 THEN NULL ELSE g % 10 END,
     CASE WHEN g % 7 = 0 THEN NULL ELSE 'v'||g END
     FROM generate_series(1,30000) g;" >/dev/null
eq_on_off "count(*) with nulls"   "SELECT count(*) FROM nt;"        "30000"
eq_on_off "count(a) skips nulls"  "SELECT count(a) FROM nt;"        "24000"
eq_on_off "sum(a) ignores nulls"  "SELECT sum(a) FROM nt;"
eq_on_off "avg(a) ignores nulls"  "SELECT avg(a) FROM nt;"
eq_on_off "min/max(a) with nulls" "SELECT min(a), max(a) FROM nt;"  "1|29999"
eq_on_off "min/max(txt) nulls"    "SELECT min(txt), max(txt) FROM nt;"
eq_on_off "count(code) nulls"     "SELECT count(code) FROM nt;"     "20000"
# filter on a nullable column: NULL rows are excluded by the comparison
eq_on_off "filter excludes nulls" "SELECT count(*), sum(a) FROM nt WHERE a > 100;"
# aggregate over an empty result is NULL for sum/avg/min/max, 0 for count
eq_on_off "count empty result"    "SELECT count(*) FROM nt WHERE a < 0;"   "0"
eq_on_off "sum empty is null"     "SELECT COALESCE(sum(a)::text,'NULL') FROM nt WHERE a < 0;" "NULL"
eq_on_off "min empty is null"     "SELECT COALESCE(min(a)::text,'NULL') FROM nt WHERE a < 0;" "NULL"

# ---------------------------------------------------------------------------
# The decompressed-chunk cache changes only where decompressed bytes live, never
# the answer: cache on and cache off produce identical results, for aggregates
# and for a filtered row scan.
# ---------------------------------------------------------------------------
echo "-- decompressed-chunk cache on vs off is identical"
cache_on_off "cache agg suite" \
	"SELECT count(*), sum(id), avg(id), min(id), max(id) FROM t WHERE id > 500;"
cache_on_off "cache filtered scan" \
	"SELECT md5(string_agg(id||'|'||label, ',' ORDER BY id)) FROM t WHERE id BETWEEN 20000 AND 21000;"
cache_on_off "cache with nulls" \
	"SELECT count(a), sum(a), min(txt) FROM nt WHERE a > 50;"
# a small cache budget still returns correct results (exercises LRU eviction)
small_on="$(q "SET pgcolumnar.enable_column_cache=on; SET pgcolumnar.column_cache_size=1; SELECT sum(id) FROM t;")"
check "tiny cache still correct" "$small_on" "1250025000"

# ---------------------------------------------------------------------------
# Fallback: aggregates and column types the vectorized path does not handle must
# still return correct results through the ordinary scalar plan. sum/avg over
# bigint and numeric, an ordered-set aggregate, a string aggregate, and GROUP BY
# all fall back. Correctness is asserted by comparing on vs off (the on run also
# falls back, but this confirms nothing is mis-routed) and to known values.
# ---------------------------------------------------------------------------
echo "-- unvectorized aggregates fall back and stay correct"
eq_on_off "sum(bigint)->numeric"  "SELECT sum(big) FROM t;"           "1250025000"
eq_on_off "avg(bigint)->numeric"  "SELECT avg(big) FROM t;"
eq_on_off "sum(numeric)"          "SELECT sum(n) FROM t;"
eq_on_off "avg(float8)"           "SELECT round(avg(f)::numeric,4) FROM t;"
eq_on_off "sum(float8)"           "SELECT sum(f) FROM t;"
eq_on_off "ordered-set agg"       "SELECT percentile_cont(0.5) WITHIN GROUP (ORDER BY id) FROM t;" "25000.5"
eq_on_off "string agg (fallback)" "SELECT length(string_agg(label,',')) > 0 FROM t;" "t"
eq_on_off "group by (fallback)"   "SELECT s, count(*) FROM t WHERE s < 3 GROUP BY s ORDER BY s;"
eq_on_off "count distinct"        "SELECT count(DISTINCT s) FROM t;" "1000"

# ---------------------------------------------------------------------------
# Interaction with deletes: the aggregate and scan must honor the row mask.
# ---------------------------------------------------------------------------
echo "-- aggregate and scan honor the row mask (deletes)"
q "DELETE FROM t WHERE id % 2 = 0;" >/dev/null
eq_on_off "count after delete"  "SELECT count(*) FROM t;"  "25000"
eq_on_off "sum after delete"    "SELECT sum(id) FROM t;"
eq_on_off "scan after delete"   "SELECT md5(string_agg(id::text, ',' ORDER BY id)) FROM t WHERE id < 1000;"

# ---------------------------------------------------------------------------
# Performance sanity: time a large aggregate with vectorization on and off. This
# is informational only and never fails the suite (timings can be noisy).
# ---------------------------------------------------------------------------
echo "-- performance sanity (informational, never fails the suite)"
q "CREATE TABLE big (a int, b int) USING pgcolumnar;" >/dev/null
q "INSERT INTO big SELECT g, g % 100 FROM generate_series(1,2000000) g;" >/dev/null
# warm the caches equally, then time three runs each way
q "SELECT sum(a) FROM big;" >/dev/null
t_on_start=$(date +%s%N)
q "SELECT sum(a), count(*), min(a), max(a) FROM big; SELECT sum(a), count(*), min(a), max(a) FROM big; SELECT sum(a), count(*), min(a), max(a) FROM big;" >/dev/null
t_on_ms=$(( ($(date +%s%N) - t_on_start) / 1000000 ))
t_off_start=$(date +%s%N)
q "SET pgcolumnar.enable_vectorization=off; SELECT sum(a), count(*), min(a), max(a) FROM big; SELECT sum(a), count(*), min(a), max(a) FROM big; SELECT sum(a), count(*), min(a), max(a) FROM big;" >/dev/null
t_off_ms=$(( ($(date +%s%N) - t_off_start) / 1000000 ))
echo "INFO  large aggregate x3: vectorized=${t_on_ms}ms scalar=${t_off_ms}ms"
if [ "$t_on_ms" -le "$t_off_ms" ]; then
	echo "INFO  vectorized aggregate was at least as fast as scalar"
else
	echo "INFO  vectorized aggregate was slower this run (not a failure; timings vary)"
fi

echo
if [ "$fail" = "0" ]; then
	echo "PHASE 6 TEST PASSED"
else
	echo "PHASE 6 TEST FAILED"
	echo "---- server log tail ----"
	run_pg "tail -60 '$LOGFILE'" || true
fi
exit $fail
