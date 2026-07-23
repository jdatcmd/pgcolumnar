#!/usr/bin/env bash
#
# pgColumnar maintenance/DDL ownership gate. The maintenance functions rewrite
# data, reclaim space, or take strong locks (truncate takes AccessExclusiveLock),
# so they must be owner-only, like VACUUM and CLUSTER. This suite proves a
# non-owner role is refused by every one of them with "must be owner", and that
# the owner is allowed. The ownership check sits right after the relation is
# opened and confirmed columnar, before any work, so a non-owner hits it rather
# than a later catalog-permission error.
#
# Usage:  test/native_ownership.sh [PG_CONFIG]
# Written fresh for pgColumnar.

set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

# run a single statement AS alice (a non-owner). A direct connection keeps it a
# single-statement autocommit query, so truncate's transaction-block guard is not
# tripped first -- we reach the ownership check.
as_alice() { env PATH="$PGC_BINDIR:$PATH" psql -h 127.0.0.1 -p "$PGC_PORT" -U alice -d "$PGC_DB" -Atq -c "$1" 2>&1; }

psql_run "CREATE ROLE alice NOSUPERUSER LOGIN;"
psql_run "GRANT USAGE ON SCHEMA pgcolumnar TO alice;"
psql_run "CREATE TABLE n (id int, v int) USING pgcolumnar;"
psql_run "SELECT pgcolumnar.alter_columnar_table_set('n', stripe_row_limit => 1000, chunk_group_row_limit => 1000);"
psql_run "INSERT INTO n SELECT g, g FROM generate_series(1, 3000) g;"

# every maintenance/DDL function must refuse a non-owner with "must be owner".
refused() {
	local out
	out="$(as_alice "SELECT pgcolumnar.$1;")"
	check "non-owner refused: ${1%%(*}" \
		"$(printf '%s' "$out" | grep -qi 'must be owner' && echo yes || echo "no")" "yes"
}

refused "compact('n')"
refused "compact_rewrite('n', 0.0)"
refused "recluster('n', 'id')"
refused "vacuum('n')"
refused "vacuum_sorted('n', 'id')"
refused "cluster('n', 'id')"
refused "truncate('n')"
refused "add_projection('n', 'p', ARRAY['id','v'])"
refused "drop_projection('n', 'p')"

# the owner (here the superuser that created the table) is allowed through.
check "owner compact allowed" \
	"$(q "SELECT (pgcolumnar.compact('n') >= 0)::text;")" "true"
check "owner recluster allowed" \
	"$(q "SELECT (pgcolumnar.recluster('n', 'id') >= 0)::text;")" "true"

pgc_summary
