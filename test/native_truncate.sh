#!/usr/bin/env bash
#
# pgColumnar physical end-truncation (Phase F): pgcolumnar.truncate() returns
# trailing reclaimed blocks to the OS. This functional suite proves the file
# actually shrinks after deletes+compaction, data stays correct against a heap
# mirror, the table is fully usable afterward, truncation is idempotent, the GUC
# disables it, and -- critically for the corruption-safety of the abort path --
# truncate REFUSES to run inside a transaction block (its physical shrink is not
# transactional, so a rollback around it must be impossible), and a normal
# truncate followed by reinsert and compaction stays corruption-free (the
# assert-build no-overlap validator runs inside compact/truncate).
#
# Concurrency is covered separately by the isolation specs
# (specs/truncate_vs_*.spec).
#
# Usage:  test/native_truncate.sh [PG_CONFIG]
# Written fresh for pgColumnar.

set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

raw() { env PATH="$PGC_BINDIR:$PATH" psql -h 127.0.0.1 -p "$PGC_PORT" -U postgres -d "$PGC_DB" -Atq -c "$1" 2>&1; }
size() { q "SELECT pg_relation_size('n');"; }
hash_n() { pgc_set_hash 'SELECT id, v FROM n'; }
hash_h() { pgc_set_hash 'SELECT id, v FROM h'; }

# end-truncation is opt-in (GUC default off); enable it for the whole test db.
psql_run "ALTER DATABASE $PGC_DB SET pgcolumnar.enable_end_truncation = on;"

psql_run "CREATE TABLE h (id int, v text);"
psql_run "CREATE TABLE n (id int, v text) USING pgcolumnar;"
psql_run "SELECT pgcolumnar.alter_columnar_table_set('n', stripe_row_limit => 1000, chunk_group_row_limit => 1000);"
psql_run "INSERT INTO h SELECT g, md5(g::text) FROM generate_series(1, 30000) g;"
psql_run "INSERT INTO n SELECT g, md5(g::text) FROM generate_series(1, 30000) g;"
check "load parity" "$(hash_n)" "$(hash_h)"
size_full="$(size)"

# nothing has been freed yet: truncate is a no-op.
check "truncate before any delete is a no-op" "$(q "SELECT pgcolumnar.truncate('n');")" "0"

# fully delete the trailing half (groups 16..30), compact to free them.
psql_run "DELETE FROM h WHERE id > 15000;"
psql_run "DELETE FROM n WHERE id > 15000;"
psql_run "SELECT pgcolumnar.compact('n');"
size_before="$(size)"

trunc="$(q "SELECT pgcolumnar.truncate('n');")"
echo "  (truncate returned $trunc blocks; size full=$size_full before-trunc=$size_before after=$(size))"
check "truncate returned blocks" "$([ "$trunc" -gt 0 ] && echo yes || echo no)" "yes"
check "file shrank" "$([ "$(size)" -lt "$size_before" ] && echo yes || echo no)" "yes"
check "parity after truncate" "$(hash_n)" "$(hash_h)"
check "row count after truncate" "$(q 'SELECT count(*) FROM n;')" "15000"

# idempotent: a second truncate has nothing left to do.
check "second truncate is a no-op" "$(q "SELECT pgcolumnar.truncate('n');")" "0"

# fully usable after truncation: reinsert, index, indexed lookup, delete.
psql_run "INSERT INTO h SELECT g, md5(g::text) FROM generate_series(30001, 40000) g;"
psql_run "INSERT INTO n SELECT g, md5(g::text) FROM generate_series(30001, 40000) g;"
check "parity after post-truncate insert" "$(hash_n)" "$(hash_h)"
psql_run "CREATE INDEX n_id ON n (id);"
check "indexed lookup after truncate" \
	"$(q 'SELECT count(*) FROM n WHERE id BETWEEN 1 AND 40000;')" "25000"

# GUC off: truncate is a no-op even with reclaimable trailing space.
psql_run "DELETE FROM h WHERE id > 35000;"
psql_run "DELETE FROM n WHERE id > 35000;"
psql_run "SELECT pgcolumnar.compact('n');"
size_guard="$(size)"
psql_run "SET pgcolumnar.enable_end_truncation = off; SELECT pgcolumnar.truncate('n');" >/dev/null
check "GUC off: truncate does nothing" "$(size)" "$size_guard"
check "GUC on: truncate reclaims" \
	"$([ "$(q "SELECT pgcolumnar.truncate('n');")" -gt 0 ] && echo yes || echo no)" "yes"
check "GUC on: file shrank" "$([ "$(size)" -lt "$size_guard" ] && echo yes || echo no)" "yes"
check "parity after guard cycle" "$(hash_n)" "$(hash_h)"

# TRANSACTION-BLOCK GUARD: truncate must refuse inside a transaction block, so a
# ROLLBACK can never leave the metapage highwater lowered while the transactional
# free_space deletes roll back (the inverted, corruptible state).
psql_run "DELETE FROM h WHERE id BETWEEN 30001 AND 33000;"
psql_run "DELETE FROM n WHERE id BETWEEN 30001 AND 33000;"
psql_run "SELECT pgcolumnar.compact('n');"
before_guard="$(size)"
errout="$(raw "BEGIN; SELECT pgcolumnar.truncate('n'); ROLLBACK;")"
check "truncate refused inside a transaction block" \
	"$(printf '%s' "$errout" | grep -qi 'cannot run inside a transaction block' && echo yes || echo no)" "yes"
check "file unchanged after refused in-txn truncate" "$(size)" "$before_guard"
check "parity after refused in-txn truncate" "$(hash_n)" "$(hash_h)"

# a normal (autocommit) truncate, then reinsert into the reclaimed low space, then
# compaction: the no-overlap validator runs inside compact_rewrite/compact and
# must stay green, and parity must hold -- i.e. no double-allocation over reused
# blocks.
psql_run "SELECT pgcolumnar.truncate('n');" >/dev/null
psql_run "INSERT INTO h SELECT g, md5(g::text) FROM generate_series(41001, 43000) g;"
psql_run "INSERT INTO n SELECT g, md5(g::text) FROM generate_series(41001, 43000) g;"
psql_run "DELETE FROM h WHERE id > 42000;"
psql_run "DELETE FROM n WHERE id > 42000;"
psql_run "SELECT pgcolumnar.compact_rewrite('n', 0.0);"
psql_run "SELECT pgcolumnar.compact('n');"
check "parity after truncate + reinsert + compaction" "$(hash_n)" "$(hash_h)"

pgc_summary
