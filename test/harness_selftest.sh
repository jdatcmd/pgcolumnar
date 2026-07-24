#!/usr/bin/env bash
#
# Self-test for the harness's own cluster-identity guard.
#
# lib.sh retries a failed start on a fresh port, and pg_ctl -w only proves that
# *something* answers there. Without a guard, a suite whose port is already owned
# by another postmaster runs every statement against that cluster: its log grows a
# stray "database already exists" while this suite's own objects are invisible.
# That hides real failures as easily as it invents fake ones, so the guard is
# load-bearing and gets a test of its own.
#
# This stands up a squatter cluster on a known port, points a suite straight at
# it, and asserts the suite ends up on a cluster it owns, that the squatter is
# left alone, and that the suite's own objects are actually there.
#
# Usage:  test/harness_selftest.sh [PG_CONFIG]
# Written fresh for pgColumnar.

set -uo pipefail

PGC_SELFTEST_PG_CONFIG="${1:-/usr/local/pg17/bin/pg_config}"
_bindir="$("$PGC_SELFTEST_PG_CONFIG" --bindir)"

# ---- stand up a squatter on a port we will then hand to the suite -----------
SQ_DIR="$(mktemp -d /tmp/pgc-squatter.XXXXXX)"
# A port nothing is already listening on. Drawing blindly would let a collision
# turn into a silent SKIP, and since the self-test runs first in the matrix, a
# quiet skip means the guard stops being tested that run without anyone noticing.
_port_free() { ! (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null; }
SQ_PORT=0
for _try in $(seq 1 20); do
	_cand=$(( 20000 + RANDOM % 20000 ))
	if _port_free "$_cand"; then
		SQ_PORT=$_cand
		break
	fi
done
if [ "$SQ_PORT" = 0 ]; then
	echo "SKIP  could not find a free port for the squatter cluster"
	rm -rf "$SQ_DIR"
	exit 0
fi
_runpg=(env)
if [ "$(id -u)" = "0" ]; then
	_runpg=(runuser -u postgres --)
	chown -R postgres "$SQ_DIR"
fi
chmod 711 "$SQ_DIR"

"${_runpg[@]}" env PATH="$_bindir:$PATH" \
	initdb -D "$SQ_DIR/data" -A trust >/dev/null 2>&1
echo "port=$SQ_PORT" >> "$SQ_DIR/data/postgresql.conf"
echo "listen_addresses='127.0.0.1'" >> "$SQ_DIR/data/postgresql.conf"
"${_runpg[@]}" env PATH="$_bindir:$PATH" \
	pg_ctl -D "$SQ_DIR/data" -l "$SQ_DIR/log" -w start >/dev/null 2>&1

squatter_down() {
	"${_runpg[@]}" env PATH="$_bindir:$PATH" \
		pg_ctl -D "$SQ_DIR/data" -m immediate -w stop >/dev/null 2>&1 || true
	rm -rf "$SQ_DIR"
}
# Arm cleanup immediately: pgc_setup can exit 1 on its own (that is the behaviour
# under test), and until it installs its trap this is the only thing that would
# stop the squatter.
trap squatter_down EXIT

sq_datadir() {
	env PATH="$_bindir:$PATH" psql -h 127.0.0.1 -p "$SQ_PORT" -U postgres \
		-d postgres -At -c 'SHOW data_directory' 2>/dev/null
}

if [ -z "$(sq_datadir)" ]; then
	echo "SKIP  could not stand up a squatter cluster to test against"
	squatter_down
	exit 0
fi
echo "-- squatter listening on $SQ_PORT ($(sq_datadir))"

# ---- point a real suite setup at exactly that port --------------------------
export PGC_PORT="$SQ_PORT"
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
pgc_setup "$PGC_SELFTEST_PG_CONFIG"

# pgc_setup replaced our trap with its own (pgc_teardown); chain both back on.
trap 'pgc_teardown; squatter_down' EXIT

# ---- assertions -------------------------------------------------------------
check "suite did not settle on the squatter's port" \
	"$([ "$PGC_PORT" = "$SQ_PORT" ] && echo same || echo moved)" "moved"

check "suite's cluster is its own" \
	"$(pgc_norm_path "$(pgc_cluster_datadir)")" "$(pgc_norm_path "$PGC_PGDATA")"

check "squatter survived untouched" "$(pgc_norm_path "$(sq_datadir)")" \
	"$(pgc_norm_path "$SQ_DIR/data")"

# The point of the guard: objects this suite creates are visible to this suite.
psql_run "CREATE TABLE selftest_marker (id int);"
psql_run "INSERT INTO selftest_marker VALUES (42);"
check "suite's own objects are visible to it" \
	"$(q 'SELECT id FROM selftest_marker;')" "42"

# and are absent from the squatter, i.e. nothing leaked across
check "nothing leaked into the squatter" \
	"$(env PATH="$_bindir:$PATH" psql -h 127.0.0.1 -p "$SQ_PORT" -U postgres \
		-d postgres -At -c "SELECT count(*) FROM pg_database WHERE datname = '$PGC_DB';" 2>/dev/null)" \
	"0"

# pgc_port_free must agree with reality on both a used and an unused port
check "pgc_port_free says the squatter's port is busy" \
	"$(pgc_port_free "$SQ_PORT" && echo free || echo busy)" "busy"

# ---- the detection primitive itself -----------------------------------------
# The assertions above are invariants: they hold even with the identity check
# removed, because a squatter holding the port makes bind genuinely fail and the
# retry happens anyway. They do not, on their own, prove the guard works. The
# case the guard exists for is pg_ctl -w reporting success while another
# postmaster owns the port, and that timing cannot be synthesised reliably here.
#
# So pin the mechanism directly instead: pointed at a foreign cluster,
# pgc_cluster_datadir must report *that* cluster, which is exactly what makes the
# comparison in pgc_setup reject it. If this reports our own directory, or
# nothing, the guard would wave a foreign cluster through.
_saved_port="$PGC_PORT"
PGC_PORT="$SQ_PORT"
_foreign="$(pgc_cluster_datadir)"
PGC_PORT="$_saved_port"

check "detection reports a foreign cluster's directory" \
	"$(pgc_norm_path "$_foreign")" "$(pgc_norm_path "$SQ_DIR/data")"
check "detection distinguishes it from ours" \
	"$([ "$(pgc_norm_path "$_foreign")" = "$(pgc_norm_path "$PGC_PGDATA")" ] \
		&& echo same || echo different)" "different"

# Drive the guard predicate itself, not just its inputs: it must accept our own
# cluster and reject the foreign one. This is the decision the start loop makes.
check "guard accepts our own cluster" \
	"$(pgc_cluster_is_ours && echo ours || echo foreign)" "ours"
_saved_port="$PGC_PORT"
PGC_PORT="$SQ_PORT"
_verdict="$(pgc_cluster_is_ours && echo ours || echo foreign)"
PGC_PORT="$_saved_port"
check "guard rejects a foreign cluster" "$_verdict" "foreign"

pgc_summary
