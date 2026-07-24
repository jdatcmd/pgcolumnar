#!/usr/bin/env bash
#
# Atomic dev loop: sync the source tree into a fresh build dir, clean-rebuild and
# install (test/rebuild.sh, which also verifies the .so's symbols), then run the
# named suites against that freshly built .so.
#
# This exists because the alternative -- editing source, copying a single test
# file into an existing build dir, and running with PGC_SKIP_BUILD=1 -- repeatedly
# tested a STALE .so: the test changed while the installed library did not. There
# is no way to make that mistake through this script, because it always resyncs
# the whole tree and always rebuilds before running anything.
#
# The source is taken from PGC_SRC (default: the read-only repo mount used in the
# dev container). The build happens in PGC_BUILD (default: /root/devbuild), which
# is wiped each run so nothing stale can survive.
#
# Usage:
#   test/devloop.sh PG_CONFIG suite [suite ...]
#   PGC_SRC=/path/to/repo PGC_BUILD=/tmp/b test/devloop.sh /usr/local/pg18/bin/pg_config native_parquet_flba
#
# With no suites it just does the clean build (a bare "does it compile + link").

set -uo pipefail

PGC="${1:-}"
if [ -z "$PGC" ] || [ ! -x "$PGC" ]; then
	echo "usage: test/devloop.sh PG_CONFIG [suite ...]   (PG_CONFIG must be executable)" >&2
	exit 2
fi
shift

SRC="${PGC_SRC:-/root/pgcolumnar_host}"
BUILD="${PGC_BUILD:-/root/devbuild}"

if [ ! -d "$SRC" ]; then
	echo "devloop: source tree not found: $SRC (set PGC_SRC)" >&2
	exit 2
fi

echo "== devloop: sync $SRC -> $BUILD"
rm -rf "$BUILD"
mkdir -p "$BUILD"
(cd "$SRC" && tar cf - --exclude=.git .) | (cd "$BUILD" && tar xf -)
cd "$BUILD" || exit 1

# Clean rebuild + install + symbol verification. Any failure here is fatal: there
# is no point running a suite against a build that did not complete.
if ! bash test/rebuild.sh "$PGC"; then
	echo "devloop: rebuild failed; not running suites" >&2
	exit 1
fi

rc=0
for s in "$@"; do
	echo "===================================================================="
	echo "== suite: $s"
	echo "===================================================================="
	# A distinct high port per suite; lib.sh's own guard corrects a collision.
	if ! PGC_SKIP_BUILD=1 PGC_PORT=$((50000 + RANDOM % 9000)) \
		bash "test/${s}.sh" "$PGC"; then
		rc=1
	fi
done
exit "$rc"
