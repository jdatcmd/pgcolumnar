#!/usr/bin/env bash
#
# Build and run the standalone codec property test (test/pbt/test_encoding.c).
# Compiles src/columnar_encoding.c against the minimal PG shim in this directory
# and runs the randomized + boundary round-trip driver. No PostgreSQL needed;
# runs anywhere with a C compiler.
#
# Usage: test/pbt/run.sh [seed] [iterations]

set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
cc="${CC:-cc}"

b="$(mktemp -d /tmp/pgc-pbt.XXXXXX)"
cp "$repo/src/columnar_encoding.c" "$b/"
cp "$here/pgstub.h" "$here/columnar.h" "$here/test_encoding.c" "$b/"
mkdir -p "$b/catalog" "$b/utils"
cp "$here/catalog/pg_type.h" "$b/catalog/"
cp "$here/utils/memutils.h" "$b/utils/"

echo "-- compiling codec + harness"
"$cc" -O2 -Wall -I"$b" -c "$b/columnar_encoding.c" -o "$b/columnar_encoding.o"
"$cc" -O2 -Wall -I"$b" "$b/test_encoding.c" "$b/columnar_encoding.o" -o "$b/test_encoding"

"$b/test_encoding" "${1:-1}" "${2:-200000}"
rc=$?

rm -rf "$b"
exit $rc
