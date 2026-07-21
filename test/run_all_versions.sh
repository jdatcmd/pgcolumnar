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
# With no arguments it uses a default list covering PostgreSQL 13 through 19.
# Each PG_CONFIG must point at an assert-enabled build to exercise the asserts.
# Run as a user that may "runuser -u postgres" (e.g. root); the suites start
# throwaway clusters as the postgres OS user.
#
# Written fresh for pgColumnar. It reuses no upstream test harness.

set -uo pipefail

SRCDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SUITES=(smoke phase2 phase3 phase4 phase5 phase6 audit concurrency unique_conc \
	differential recovery fuzz hardening concurrent_diff parallel sorted_projection \
	arrow_export parquet_export read_stream corruption \
	generated_columns temporal arrow_import index_only projections arrow_nested)

# Default matrix: one assert-enabled pg_config per major, 13 through 19.
DEFAULT_CONFIGS=(
	/usr/local/pg13/bin/pg_config
	/usr/local/pg14/bin/pg_config
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
BASE_PORT="${PGC_BASE_PORT:-55400}"

overall=0
declare -a SUMMARY

for pgc in "${CONFIGS[@]}"; do
	if [ ! -x "$pgc" ]; then
		echo "SKIP  $pgc (not executable)"
		SUMMARY+=("SKIP   $pgc")
		continue
	fi

	ver="$("$pgc" --version)"
	major="$(echo "$ver" | sed -E 's/^[^0-9]*([0-9]+).*/\1/')"
	builddir="$(mktemp -d "/tmp/pgcolumnar-matrix-${major}.XXXXXX")"

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

	verfail=0
	results=""
	for s in "${SUITES[@]}"; do
		port=$((BASE_PORT++))
		if PGC_PORT="$port" bash "$builddir/test/${s}.sh" "$pgc" \
			>"$builddir/${s}.log" 2>&1; then
			echo "  PASS  $s"
			results+="$s=PASS "
		else
			echo "  FAIL  $s"
			tail -20 "$builddir/${s}.log" | sed 's/^/      /'
			results+="$s=FAIL "
			verfail=1
		fi
	done

	if [ "$verfail" = 0 ]; then
		SUMMARY+=("PASS   PG$major  ${results}")
	else
		SUMMARY+=("FAIL   PG$major  ${results}")
		overall=1
	fi
	rm -rf "$builddir"
done

echo
echo "===================== MATRIX SUMMARY ============================"
for line in "${SUMMARY[@]}"; do
	echo "  $line"
done
echo "================================================================"
if [ "$overall" = 0 ]; then
	echo "ALL VERSIONS PASSED"
else
	echo "SOME VERSIONS FAILED"
fi
exit "$overall"
