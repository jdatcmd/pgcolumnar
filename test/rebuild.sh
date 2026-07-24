#!/usr/bin/env bash
#
# Clean rebuild + install of pgcolumnar for one PG_CONFIG.
#
# Two stale-artifact failures are easy to hit and both produce confusing results
# rather than honest errors:
#
#   1. `make clean` without PG_CONFIG uses whatever pg_config is on PATH. If that
#      is a different major (or absent), nothing is cleaned and the previous
#      major's .o files are relinked into this major's .so. The result installs
#      fine and then fails at load with an undefined symbol, e.g. smgrtruncate2
#      when PG15-17 objects are linked for PG18.
#   2. Installing from one build tree and then running a suite from another leaves
#      the tests measuring the wrong .so entirely, silently.
#
# This script removes both possibilities: it cleans with the correct PG_CONFIG,
# deletes the installed artifacts before rebuilding so a failed install cannot
# leave the old one in place, and then verifies every undefined symbol in the
# built .so resolves against the target postgres binary or its own shared libs.
# That last check is what catches case 1 before a cluster ever starts.
#
# Usage:  test/rebuild.sh [PG_CONFIG] [SRCDIR]
#         test/rebuild.sh /usr/local/pg18/bin/pg_config
#
# Exits non-zero on any failure, so it is safe to chain with &&.

set -uo pipefail

PG_CONFIG="${1:-/usr/local/pg17/bin/pg_config}"
SRCDIR="${2:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

if [ ! -x "$PG_CONFIG" ]; then
	echo "rebuild: no such pg_config: $PG_CONFIG" >&2
	exit 1
fi

PGVER="$("$PG_CONFIG" --version)"
PKGLIB="$("$PG_CONFIG" --pkglibdir)"
SHAREDIR="$("$PG_CONFIG" --sharedir)"
BINDIR="$("$PG_CONFIG" --bindir)"
SO="$PKGLIB/pgcolumnar.so"

echo "== rebuild: $PGVER"
echo "== srcdir:  $SRCDIR"
echo "== target:  $SO"

cd "$SRCDIR" || exit 1

# ---- 1. clean the tree, with the right PG_CONFIG ---------------------------
make PG_CONFIG="$PG_CONFIG" clean >/dev/null 2>&1
# belt and braces: PGXS clean can be a no-op if the Makefile did not load
rm -f src/*.o src/*.bc ./*.o ./*.bc ./*.so 2>/dev/null

leftover="$(find . -name '*.o' -o -name '*.so' -o -name '*.bc' 2>/dev/null | head -5)"
if [ -n "$leftover" ]; then
	echo "rebuild: tree still dirty after clean:" >&2
	echo "$leftover" >&2
	exit 1
fi
echo "-- clean: tree has no .o/.so/.bc"

# ---- 2. remove the installed artifacts -------------------------------------
# So a build or install failure cannot leave the previous .so loadable and make
# a suite silently test stale code. The paths come from pg_config, so sanity-check
# them before deleting through a glob: an empty or unexpected SHAREDIR would turn
# the next line into a much wider delete than intended.
case "$SHAREDIR" in
	/*/*) ;;
	*) echo "rebuild: refusing to delete from implausible sharedir '$SHAREDIR'" >&2
	   exit 1 ;;
esac
[ -d "$SHAREDIR/extension" ] || {
	echo "rebuild: no extension dir under '$SHAREDIR'" >&2; exit 1; }

rm -f "$SO"
rm -f "$SHAREDIR"/extension/pgcolumnar--*.sql "$SHAREDIR"/extension/pgcolumnar.control
echo "-- uninstalled previous artifacts"

# ---- 3. build --------------------------------------------------------------
buildlog="$(mktemp -t pgc_rebuild.XXXXXX)"
trap 'rm -f "$buildlog"' EXIT
if ! make PG_CONFIG="$PG_CONFIG" -j"$(nproc)" > "$buildlog" 2>&1; then
	echo "rebuild: BUILD FAILED" >&2
	grep -E 'error:|Error' "$buildlog" | head -20 >&2
	exit 1
fi
warns="$(grep -cE 'warning:' "$buildlog")"
echo "-- build: OK ($warns warnings)"

if ! make PG_CONFIG="$PG_CONFIG" install >/dev/null 2>&1; then
	echo "rebuild: INSTALL FAILED" >&2
	exit 1
fi
[ -f "$SO" ] || { echo "rebuild: $SO missing after install" >&2; exit 1; }
echo "-- install: OK"

# ---- 4. verify the .so resolves against THIS postgres ----------------------
# Every undefined symbol must be satisfied by the server binary or by one of the
# .so's own shared libraries. An object built against another major shows up here
# as an unresolved PostgreSQL symbol, before any cluster tries to load it.
if command -v nm >/dev/null 2>&1; then
	# Symbol names are compared with any @GLIBC_x.y version suffix stripped, since
	# the reference copy in libc is versioned and the reference in postgres is not.
	# The ignored names are toolchain symbols that are undefined by design (weak
	# ITM/gmon hooks, and __cxa_finalize which the loader supplies).
	strip_ver() { sed 's/@.*//'; }
	IGNORE='^(_ITM_|__gmon_start__$|__cxa_finalize$)'

	undef="$(nm -D --undefined-only "$SO" 2>/dev/null | awk '{print $NF}' |
		strip_ver | grep -Ev "$IGNORE" | sort -u)"
	# The .so is dlopen'd into the running postgres, so its symbols resolve against
	# the server binary, everything the server itself links (libm, libssl, ...), and
	# the .so's own dependencies. All three belong in the reference set.
	defined="$(nm -D --defined-only "$BINDIR/postgres" 2>/dev/null | awk '{print $NF}')"
	for lib in $(ldd "$SO" "$BINDIR/postgres" 2>/dev/null |
			awk '/=>/ {print $3}' | grep -v '^$' | sort -u); do
		defined="$defined
$(nm -D --defined-only "$lib" 2>/dev/null | awk '{print $NF}')"
	done
	defined="$(echo "$defined" | strip_ver | sort -u)"
	missing="$(comm -23 <(echo "$undef") <(echo "$defined"))"
	if [ -n "$missing" ]; then
		echo "rebuild: UNRESOLVED SYMBOLS against $PGVER:" >&2
		echo "$missing" | head -20 >&2
		echo "(this usually means objects from another major were linked in)" >&2
		exit 1
	fi
	echo "-- symbols: all resolve against $(basename "$BINDIR")/postgres"
else
	echo "-- symbols: nm unavailable, skipped"
fi

echo "== rebuild OK: $PGVER"
