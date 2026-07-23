#!/usr/bin/env bash
#
# pgColumnar deterministic MVCC isolation tests. Runs PostgreSQL's
# pg_isolation_regress over the specs in test/isolation/specs against a throwaway
# cluster, pinning the concurrency hazards of the online compaction paths (Phase
# F3) with exact, repeatable interleavings and automatic blocking detection --
# stronger than the randomized concurrent stress suites for the precise race
# windows. Specs:
#   delete_vs_rewrite   -- a DELETE racing compact_rewrite of its group aborts
#                          with a serialization failure (H1), never lost.
#   old_snapshot_compact-- an old snapshot keeps seeing rows through a concurrent
#                          delete + compact_rewrite (H3).
#
# Usage:  test/isolation.sh [PG_CONFIG]
# Written fresh for pgColumnar.

set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

SRCDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INPUTDIR="$SRCDIR/test/isolation"
PGXS="$("$PGC_PG_CONFIG" --pgxs)"
ISO="$(dirname "$PGXS")/../test/isolation/pg_isolation_regress"

if [ ! -x "$ISO" ]; then
	echo "pg_isolation_regress not found at $ISO; SKIP"
	pgc_summary
	exit 0
fi

SPECS="delete_vs_rewrite old_snapshot_compact compact_vs_reader recluster_vs_delete"
OUTDIR="$PGC_WORKDIR/iso_out"
mkdir -p "$OUTDIR"

# Run against the already-running throwaway cluster (extension preloaded and
# created by pgc_setup). pg_isolation_regress compares each permutation's output
# to test/isolation/expected/<spec>.out and writes a regression.diffs on mismatch.
if PGHOST=127.0.0.1 PGPORT="$PGC_PORT" PGUSER=postgres \
	"$ISO" --inputdir="$INPUTDIR" --outputdir="$OUTDIR" \
		--bindir="$PGC_BINDIR" --use-existing --dbname="$PGC_DB" $SPECS; then
	check "isolation specs" "pass" "pass"
else
	echo "---- regression.diffs ----"
	sed 's/^/    /' "$OUTDIR/regression.diffs" 2>/dev/null | head -80
	check "isolation specs" "fail" "pass"
fi

pgc_summary
