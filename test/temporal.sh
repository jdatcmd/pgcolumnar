#!/usr/bin/env bash
#
# pgColumnar temporal-constraint coverage.
#
# PostgreSQL 18 adds WITHOUT OVERLAPS primary keys/unique constraints; 19 adds
# UPDATE ... FOR PORTION OF. Both run through the index and constraint machinery
# pgColumnar integrates with, so a columnar table must enforce them exactly as a
# heap table does. This suite builds a heap/columnar pair with a WITHOUT OVERLAPS
# primary key and asserts identical behavior: non-overlapping rows accepted,
# overlapping rows rejected, and (on 19+) a FOR PORTION OF update produces the
# same result set. WITHOUT OVERLAPS needs a GiST index over the scalar key part,
# so btree_gist must be available; the suite skips with a note if it is not.
#
# Usage:  test/temporal.sh [PG_CONFIG]
#
# Written fresh for pgColumnar; it does not reuse any upstream test file.

set -uo pipefail

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

major="$("$PGC_PG_CONFIG" --version | sed -E 's/^[^0-9]*([0-9]+).*/\1/')"

if [ "$major" -lt 18 ]; then
	echo "-- temporal constraints: skipped (PostgreSQL < 18)"
	pgc_summary
	exit 0
fi

if ! psql_run "CREATE EXTENSION IF NOT EXISTS btree_gist;" >/dev/null 2>&1; then
	echo "-- btree_gist not available; skipping temporal-constraint verification"
	pgc_summary
	exit 0
fi

# expect_ok / expect_err run the same statement against heap and columnar and
# assert both agree (both succeed, or both reject).
both() {
	local label="$1" want="$2" sql_h="$3" sql_c="$4" rh rc
	if psql_run "$sql_h" >/dev/null 2>&1; then rh=ok; else rh=err; fi
	if psql_run "$sql_c" >/dev/null 2>&1; then rc=ok; else rc=err; fi
	check "$label (heap)" "$rh" "$want"
	check "$label (columnar)" "$rc" "$want"
}

echo "-- WITHOUT OVERLAPS primary key"
psql_run "CREATE TABLE tt_heap (id int, valid daterange,
          PRIMARY KEY (id, valid WITHOUT OVERLAPS)) USING heap;"
psql_run "CREATE TABLE tt_col (id int, valid daterange,
          PRIMARY KEY (id, valid WITHOUT OVERLAPS)) USING pgcolumnar;"

both "non-overlapping insert accepted" ok \
	"INSERT INTO tt_heap VALUES (1,'[2020-01-01,2020-06-01)'),(1,'[2020-06-01,2021-01-01)'),(2,'[2020-01-01,2021-01-01)');" \
	"INSERT INTO tt_col  VALUES (1,'[2020-01-01,2020-06-01)'),(1,'[2020-06-01,2021-01-01)'),(2,'[2020-01-01,2021-01-01)');"

both "overlapping insert rejected" err \
	"INSERT INTO tt_heap VALUES (1,'[2020-03-01,2020-09-01)');" \
	"INSERT INTO tt_col  VALUES (1,'[2020-03-01,2020-09-01)');"

th="$(pgc_set_hash "SELECT id, valid FROM tt_heap")"
tc="$(pgc_set_hash "SELECT id, valid FROM tt_col")"
check "temporal PK contents match" "$tc" "$th"

# --- FOR PORTION OF (PostgreSQL 19+) ---------------------------------------
if [ "$major" -ge 19 ]; then
	echo "-- UPDATE ... FOR PORTION OF"
	both "for portion of update applied" ok \
		"UPDATE tt_heap FOR PORTION OF valid FROM '2020-03-01' TO '2020-05-01' SET id = 100 WHERE id = 2;" \
		"UPDATE tt_col  FOR PORTION OF valid FROM '2020-03-01' TO '2020-05-01' SET id = 100 WHERE id = 2;"

	ph="$(pgc_set_hash "SELECT id, valid FROM tt_heap")"
	pc="$(pgc_set_hash "SELECT id, valid FROM tt_col")"
	check "for portion of result matches" "$pc" "$ph"
else
	echo "-- FOR PORTION OF: skipped (PostgreSQL < 19)"
fi

pgc_summary
