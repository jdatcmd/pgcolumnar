#!/usr/bin/env bash
#
# pgColumnar multi-file / directory Parquet reads (Phase G).
#
# A path that names a directory reads every *.parquet in it as one relation; a
# path with glob metacharacters expands the same way; a plain file is unchanged.
# This suite checks that the union of a directory's files matches a single-file
# oracle across all read surfaces (read_parquet, import_parquet, FDW), that a
# glob selects the right subset in sorted order, that per-file predicate pushdown
# and the aggregated EXPLAIN counters work, and that mismatched or empty inputs
# fail cleanly.
#
# Usage:  test/native_parquet_multifile.sh [PG_CONFIG]
# Written fresh for pgColumnar.

set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

if ! python3 -c 'import pyarrow.parquet' 2>/dev/null; then
	echo "SKIP  pyarrow not available; multi-file suite needs it"
	pgc_summary
	exit 0
fi

W="$PGC_WORKDIR"
DIR="$W/parts"
mkdir -p "$DIR"
psql_run "CREATE SERVER pq FOREIGN DATA WRAPPER pgcolumnar_parquet;"

# Three part files with disjoint id ranges, plus one combined oracle file. Each
# part is its own row group range so predicate pushdown can skip whole files.
python3 - "$W" "$DIR" <<'PY'
import sys, pyarrow as pa, pyarrow.parquet as pq
W, DIR = sys.argv[1], sys.argv[2]
def tbl(lo, hi):
    return pa.table({"id": pa.array(range(lo, hi), pa.int32()),
                     "v":  pa.array([i * 2 for i in range(lo, hi)], pa.int64())})
# part-0: 0..999, part-1: 1000..1999, part-2: 2000..2999
for i in range(3):
    pq.write_table(tbl(i * 1000, i * 1000 + 1000), f"{DIR}/part-{i}.parquet",
                   row_group_size=1000)
# a non-parquet file in the dir must be ignored
open(f"{DIR}/README.txt", "w").write("not parquet\n")
# the oracle: all 3000 rows in one file
pq.write_table(tbl(0, 3000), f"{W}/all.parquet", row_group_size=1000)
PY

cols="id int, v int8"
oracle="$(pgc_set_hash "SELECT * FROM pgcolumnar.read_parquet('$W/all.parquet') AS t($cols)")"

# ---- directory read == single-file oracle, across surfaces -----------------
check "read_parquet(directory) == oracle" \
	"$(pgc_set_hash "SELECT * FROM pgcolumnar.read_parquet('$DIR') AS t($cols)")" "$oracle"

psql_run "CREATE TABLE imp ($cols);"
psql_run "SELECT pgcolumnar.import_parquet('imp'::regclass, '$DIR');"
check "import_parquet(directory) == oracle" \
	"$(pgc_set_hash "SELECT * FROM imp")" "$oracle"
check "import_parquet(directory) row count" "$(q 'SELECT count(*) FROM imp;')" "3000"

psql_run "CREATE FOREIGN TABLE ftd ($cols) SERVER pq OPTIONS (path '$DIR');"
check "FDW(directory) == oracle" \
	"$(pgc_set_hash "SELECT * FROM ftd")" "$oracle"

# the non-parquet file is ignored, not read as data
check "directory read ignores non-parquet files" "$(q 'SELECT count(*) FROM ftd;')" "3000"

# ---- glob selects a subset -------------------------------------------------
check "glob part-[01] == first two files" \
	"$(pgc_set_hash "SELECT * FROM pgcolumnar.read_parquet('$DIR/part-[01].parquet') AS t($cols)")" \
	"$(pgc_set_hash "SELECT * FROM pgcolumnar.read_parquet('$W/all.parquet') AS t($cols) WHERE id < 2000")"

# ---- per-file predicate pushdown across the directory ----------------------
# id >= 2500 falls only in part-2; the other two files' groups must skip.
skipped_for() {
	q "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF)
	   SELECT count(*) FROM ftd WHERE $1" | grep 'Row Groups Skipped' | grep -oE '[0-9]+' | head -1
}
groups_for() {
	q "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF)
	   SELECT count(*) FROM ftd WHERE $1" | grep 'Row Groups:' | grep -oE '[0-9]+' | head -1
}
files_for() {
	q "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF)
	   SELECT count(*) FROM ftd WHERE $1" | grep 'Files:' | grep -oE '[0-9]+' | head -1
}
check "EXPLAIN reports 3 files" "$(files_for 'true')" "3"
check "EXPLAIN sums row groups across files" "$(groups_for 'true')" "3"
check "predicate skips the two non-matching files' groups" "$(skipped_for 'id >= 2500')" "2"
check "cross-file predicate result == oracle" \
	"$(pgc_set_hash "SELECT * FROM ftd WHERE id >= 2500")" \
	"$(pgc_set_hash "SELECT * FROM pgcolumnar.read_parquet('$W/all.parquet') AS t($cols) WHERE id >= 2500")"

# ---- projection composes with multi-file -----------------------------------
# The "Columns Read" projection counter (same set per file) alongside directory
# reads: count(*) over the directory decodes no columns, one-column select one.
cols_read() {
	q "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) $1" \
		| grep 'Columns Read:' | grep -oE '[0-9]+' | head -1
}
check "count(*) over directory reads 0 columns" "$(cols_read "SELECT count(*) FROM ftd")" "0"
check "one column over directory reads 1"       "$(cols_read "SELECT id FROM ftd")" "1"
check "count(*) over directory is correct"      "$(q 'SELECT count(*) FROM ftd;')" "3000"

# ---- error cases -----------------------------------------------------------
errs() {
	# Capture the whole output first: piping into `grep -q` under `pipefail`
	# closes the pipe early, SIGPIPEs psql, and makes the pipeline non-zero.
	local out
	out="$(env PATH="$PGC_BINDIR:$PATH" psql -h 127.0.0.1 -p "$PGC_PORT" \
		-U postgres -d "$PGC_DB" -At -c "$1" 2>&1)"
	case "$out" in
		*ERROR*) echo OK ;;
		*) echo "NO ERROR" ;;
	esac
}
mkdir -p "$W/empty"
check "empty directory errors" "$(errs "SELECT * FROM pgcolumnar.read_parquet('$W/empty') AS t($cols)")" "OK"
check "non-matching glob errors" "$(errs "SELECT * FROM pgcolumnar.read_parquet('$DIR/nope-*.parquet') AS t($cols)")" "OK"

# a directory with a schema-mismatched file must fail, not silently skip/garble
mkdir -p "$W/mixed"
cp "$DIR/part-0.parquet" "$W/mixed/a.parquet"
python3 - "$W/mixed" <<'PY'
import sys, pyarrow as pa, pyarrow.parquet as pq
pq.write_table(pa.table({"x": pa.array(["a", "b"])}), f"{sys.argv[1]}/b.parquet")
PY
check "schema-mismatched file in directory errors" \
	"$(errs "SELECT * FROM pgcolumnar.read_parquet('$W/mixed') AS t($cols)")" "OK"

# a subdirectory whose name ends in .parquet is skipped, not read and failed on
mkdir -p "$DIR/sub.parquet"
check "directory ignores a subdir named *.parquet" "$(q 'SELECT count(*) FROM ftd;')" "3000"
rmdir "$DIR/sub.parquet"

# a glob that catches a directory (part-9.parquet/) skips it and reads the files
mkdir -p "$DIR/part-9.parquet"
check "glob skips a directory match, reads the files" \
	"$(pgc_set_hash "SELECT * FROM pgcolumnar.read_parquet('$DIR/part-*.parquet') AS t($cols)")" \
	"$oracle"
rmdir "$DIR/part-9.parquet"

# a glob whose every match is a directory has nothing to read and must say so
mkdir -p "$W/onlydirs/a.parquet" "$W/onlydirs/b.parquet"
check "glob matching only directories errors" \
	"$(errs "SELECT * FROM pgcolumnar.read_parquet('$W/onlydirs/*.parquet') AS t($cols)")" "OK"
check "directory of only *.parquet subdirs errors" \
	"$(errs "SELECT * FROM pgcolumnar.read_parquet('$W/onlydirs') AS t($cols)")" "OK"
rmdir "$W/onlydirs/a.parquet" "$W/onlydirs/b.parquet"

# A path we cannot stat must NOT be dropped: a dangling symlink named *.parquet
# has to surface as an error naming the file, not as a query that quietly returns
# the other files' rows. Silent row loss is the failure this reader must not have.
ln -s "$DIR/does-not-exist.parquet" "$DIR/broken.parquet"
check "dangling symlink in directory errors, not skipped" \
	"$(errs "SELECT count(*) FROM pgcolumnar.read_parquet('$DIR') AS t($cols)")" "OK"
check "dangling symlink in a glob errors, not skipped" \
	"$(errs "SELECT count(*) FROM pgcolumnar.read_parquet('$DIR/*.parquet') AS t($cols)")" "OK"
rm -f "$DIR/broken.parquet"

# A FIFO must be skipped rather than opened: AllocateFile() on a FIFO blocks in
# open(2) until a writer appears, and with SA_RESTART handlers a query cancel
# does not reliably return the backend. The timeout is the real assertion here;
# without it a regression hangs the suite instead of failing it.
if mkfifo "$DIR/pipe.parquet" 2>/dev/null; then
	fifo_out="$(timeout 30 env PATH="$PGC_BINDIR:$PATH" psql -h 127.0.0.1 \
		-p "$PGC_PORT" -U postgres -d "$PGC_DB" -At \
		-c "SELECT count(*) FROM pgcolumnar.read_parquet('$DIR') AS t($cols)" 2>&1)"
	fifo_rc=$?
	if [ "$fifo_rc" = 124 ]; then
		fifo_out="TIMED OUT (blocked opening the FIFO)"
	fi
	check "FIFO named *.parquet is skipped, does not block" "$fifo_out" "3000"
	rm -f "$DIR/pipe.parquet"
else
	echo "SKIP  mkfifo unavailable; FIFO case not exercised"
fi

# ---- recursive walk --------------------------------------------------------
#
# A directory now reads *.parquet at any depth below it, not only directly
# inside. The cases that matter are the ones where recursion can go wrong:
# unbounded depth, symlink cycles, and entries that are neither.

TREE="$W/tree"
mkdir -p "$TREE/a/b" "$TREE/c"
cp "$DIR/part-0.parquet" "$TREE/a/"
cp "$DIR/part-1.parquet" "$TREE/a/b/"
cp "$DIR/part-2.parquet" "$TREE/c/"
echo "not parquet" > "$TREE/a/b/notes.txt"

check "nested tree reads every level as one relation" \
	"$(pgc_set_hash "SELECT * FROM pgcolumnar.read_parquet('$TREE') AS t($cols)")" \
	"$oracle"
check "nested tree row count" \
	"$(q "SELECT count(*) FROM pgcolumnar.read_parquet('$TREE') AS t($cols);")" "3000"
check "a non-parquet file deep in the tree is ignored" \
	"$(q "SELECT count(*) FROM pgcolumnar.read_parquet('$TREE') AS t($cols);")" "3000"

# Order is by full path, not by directory-entry order, which varies by
# filesystem. Sorting the three paths puts a/b/part-1 first, then a/part-0, then
# c/part-2, so the first rows are part-1's. Asserting the actual ids pins the
# ordering rule; comparing two reads to each other would pass even if both were
# empty, which is what it did on the pre-change build.
check "tree read order follows the sorted full path" \
	"$(q "SELECT id FROM pgcolumnar.read_parquet('$TREE') AS t($cols) LIMIT 3;" | tr '\n' ' ')" \
	"1000 1001 1002 "

# A symlink to an ancestor is the classic infinite walk. The rule is that a
# directory reached through a symlink is not descended, so this terminates and
# returns the tree's real files. The timeout is the assertion: without the rule
# this never returns.
ln -s "$TREE" "$TREE/a/loop"
loop_out="$(timeout 60 env PATH="$PGC_BINDIR:$PATH" psql -h 127.0.0.1 -p "$PGC_PORT" \
	-U postgres -d "$PGC_DB" -At \
	-c "SELECT count(*) FROM pgcolumnar.read_parquet('$TREE') AS t($cols)" 2>&1)"
loop_rc=$?
[ "$loop_rc" = 124 ] && loop_out="TIMED OUT (symlink loop followed)"
check "a symlink loop terminates and does not multiply rows" "$loop_out" "3000"
rm -f "$TREE/a/loop"

# A symlinked directory holding a parquet file is not descended either, which is
# the cost of the rule and is asserted so the behaviour is deliberate.
mkdir -p "$W/elsewhere"
cp "$DIR/part-0.parquet" "$W/elsewhere/extra.parquet"
ln -s "$W/elsewhere" "$TREE/linkdir"
check "a symlinked directory is not descended" \
	"$(q "SELECT count(*) FROM pgcolumnar.read_parquet('$TREE') AS t($cols);")" "3000"
rm -f "$TREE/linkdir"

# A symlink to a FILE is still followed, as it was before the walk existed.
ln -s "$W/elsewhere/extra.parquet" "$TREE/c/linked.parquet"
check "a symlinked file is still read" \
	"$(q "SELECT count(*) FROM pgcolumnar.read_parquet('$TREE') AS t($cols);")" "4000"
rm -f "$TREE/c/linked.parquet"

# Depth is bounded, and the bound errors rather than silently truncating: a tree
# that is too deep must not read as "the part we felt like walking".
deep="$W/deep"
mkdir -p "$deep"
d="$deep"
for i in $(seq 1 40); do d="$d/l$i"; done
mkdir -p "$d"
cp "$DIR/part-0.parquet" "$d/"
check "a tree deeper than the bound is rejected, not truncated" \
	"$(errs "SELECT count(*) FROM pgcolumnar.read_parquet('$deep') AS t($cols)")" "OK"

pgc_summary
