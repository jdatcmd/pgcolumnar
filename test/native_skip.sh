#!/usr/bin/env bash
#
# pgColumnar native zone-map row-group skipping (Phase D5b): a native-format
# (PGCN v1) table now takes the custom scan's scalar path, so pushed-down
# predicates drive zone-map skipping of whole row groups whose min/max prove no
# row can match (native spec 7.1). Skipping is a performance optimization only:
# the executor re-applies the full qual, so results are identical whether a group
# is skipped or not. This suite proves both halves: result parity with a heap
# mirror across range and equality predicates, and that groups are actually
# skipped (via the EXPLAIN ANALYZE "Columnar Chunk Groups Removed by Filter"
# counter), including that a non-selective scan skips nothing.
#
# Usage:  test/native_skip.sh [PG_CONFIG]
# Written fresh for pgColumnar.

set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

# 20480 rows, 2048-row groups -> 10 row groups with contiguous, non-overlapping
# id ranges (group k holds ids k*2048+1 .. (k+1)*2048), so range and equality
# predicates on id are highly skippable.
GEN="SELECT g, (g * 10)::bigint, 'lbl-' || (g % 100),
         g::nsk_acct4, (g * 10)::nsk_acct8
  FROM generate_series(1, 20480) g"

# d4 and d8 are domains over int and bigint holding the same values as id and k.
# A domain is ordinary modelling, and it must prune exactly as its base type does
# (#483).
psql_run "CREATE DOMAIN nsk_acct4 AS int;"
psql_run "CREATE DOMAIN nsk_acct8 AS bigint;"
psql_run "CREATE TABLE h (id int, k bigint, label text, d4 nsk_acct4, d8 nsk_acct8);"
psql_run "CREATE TABLE n (id int, k bigint, label text, d4 nsk_acct4, d8 nsk_acct8) USING pgcolumnar;"
psql_run "SELECT pgcolumnar.set_options('n', stripe_row_limit => 2048, chunk_group_row_limit => 1024);"
psql_run "INSERT INTO h $GEN;"
psql_run "INSERT INTO n $GEN;"

# Count of row groups skipped by the zone maps for a query, from EXPLAIN ANALYZE.
skipped() {
	env PATH="$PGC_BINDIR:$PATH" psql -h 127.0.0.1 -p "$PGC_PORT" -U postgres \
		-d "$PGC_DB" -At -c "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) $1" 2>/dev/null \
		| grep 'Columnar Chunk Groups Removed by Filter' | grep -oE '[0-9]+' | head -1
}
gt0() { [ "${1:-0}" -gt 0 ] && echo yes || echo no; }

# The node, before any counter read out of it: every skipped() below returns a
# number only the columnar custom scan reports.
check "the plan under test is a columnar custom scan" \
	"$(pgc_is_columnar_scan 'SELECT id FROM n WHERE id BETWEEN 5000 AND 5100')" "yes"

check "row count" "$(q 'SELECT count(*) FROM n;')" "20480"
check "no-predicate scan returns all rows" "$(q 'SELECT count(*) FROM n;')" "$(q 'SELECT count(*) FROM h;')"

# Range predicate confined to one group: correct, and skips the other groups.
check "range result parity" \
	"$(pgc_set_hash 'SELECT id, k, label FROM n WHERE id BETWEEN 5000 AND 5100')" \
	"$(pgc_set_hash 'SELECT id, k, label FROM h WHERE id BETWEEN 5000 AND 5100')"
check "range skips groups" "$(gt0 "$(skipped 'SELECT id FROM n WHERE id BETWEEN 5000 AND 5100')")" "yes"

# Equality on the monotonic column skips groups too.
check "equality result parity" \
	"$(q 'SELECT count(*) FROM n WHERE id = 12345;')" \
	"$(q 'SELECT count(*) FROM h WHERE id = 12345;')"
check "equality skips groups" "$(gt0 "$(skipped 'SELECT id FROM n WHERE id = 12345')")" "yes"

# Predicate on the bigint column (k = id*10) is equally skippable.
check "bigint range parity" \
	"$(pgc_set_hash 'SELECT id FROM n WHERE k BETWEEN 100000::bigint AND 101000::bigint')" \
	"$(pgc_set_hash 'SELECT id FROM h WHERE k BETWEEN 100000::bigint AND 101000::bigint')"
check "bigint range skips groups" "$(gt0 "$(skipped 'SELECT id FROM n WHERE k BETWEEN 100000::bigint AND 101000::bigint')")" "yes"

# ---- the same predicate without the casts (#477) -----------------------------
#
# Those literals were cast to bigint so the comparison would be same-type, and
# this comment used to say cross-type operators were "deliberately not pushed
# down". They were not pushed down, and the consequence was not deliberate: an
# int8 column compared against a bare integer literal skipped NOTHING, which is
# what ordinary SQL looks like. Measured on identical data in two columns, one
# int and one bigint: `idi > 16000` removed 7 groups of 10, `seq > 16000` removed
# 0, and `seq > 16000::bigint` removed 7 again.
#
# EXPLAIN gave no way to see it. `Columnar Pushed-Down Filters` counts scan keys
# handed to the reader, not predicates able to exclude anything, so it read 1
# while zero groups were skipped. test/zonemap_cost.sh's correlated arm has been
# in that state since it was written.
#
# The parity check is first and is not decoration: the risk in comparing an int8
# column against an int4 constant is a WRONG answer, not a slow one, so rows come
# before skipping in the order these are asserted.
check "bigint vs integer literal returns the same rows as heap (#477)" \
	"$(pgc_set_hash 'SELECT id FROM n WHERE k BETWEEN 100000 AND 101000')" \
	"$(pgc_set_hash 'SELECT id FROM h WHERE k BETWEEN 100000 AND 101000')"

check "and it skips groups, which the cast version already did (#477)" \
	"$(gt0 "$(skipped 'SELECT id FROM n WHERE k BETWEEN 100000 AND 101000')")" "yes"

# ---- a domain column must prune like its base type (#483) --------------------
#
# Found while building #479's fixture, which needed a predicate the reader could
# not use. This was that predicate, and it should not have been one.
#
# pgcolumnar_clause_to_scankey and pgcolumnar_make_predicates resolved a domain
# differently, so the key passed the first and was dropped by the second. The key
# is built because lookup_type_cache reaches GetDefaultOpClass, which resolves a
# domain to its base type, so the domain finds integer_ops and a valid strategy.
# It was then dropped because the column type is the domain while sk_subtype is
# the base type, making the key cross-type, and integer_ops has no ordering proc
# for (domain, int4). #478's fallback is right for a genuinely uncomparable pair
# and a domain is not one.
#
# Measured before the fix, same values in one table, 20 groups: int and bigint
# pruned 19, a domain over either pruned 0, and all three reported
# "Columnar Pushed-Down Filters: 1". Ordinary SQL silently reading the whole
# table.
#
# Parity first, and not as decoration. Comparing a domain's stored values with
# the wrong comparison proc risks a WRONG answer, not a slow one, so rows are
# asserted before skipping in every arm below.
check "domain over int returns the same rows as heap (#483)" \
	"$(pgc_set_hash 'SELECT id FROM n WHERE d4 BETWEEN 5000 AND 5100')" \
	"$(pgc_set_hash 'SELECT id FROM h WHERE d4 BETWEEN 5000 AND 5100')"
check "and it skips groups, as the same predicate on int does (#483)" \
	"$(gt0 "$(skipped 'SELECT id FROM n WHERE d4 BETWEEN 5000 AND 5100')")" "yes"

check "domain over bigint returns the same rows as heap (#483)" \
	"$(pgc_set_hash 'SELECT id FROM n WHERE d8 BETWEEN 100000 AND 101000')" \
	"$(pgc_set_hash 'SELECT id FROM h WHERE d8 BETWEEN 100000 AND 101000')"
check "and it skips groups too (#483)" \
	"$(gt0 "$(skipped 'SELECT id FROM n WHERE d8 BETWEEN 100000 AND 101000')")" "yes"

# One-sided and equality take different branches of native_zone_excludes, and
# equality additionally gates the bloom probe, which is where a wrong answer
# would come from if the domain resolved to a different hash than the writer used.
check "one-sided domain predicate skips groups (#483)" \
	"$(gt0 "$(skipped 'SELECT id FROM n WHERE d4 > 15000')")" "yes"
check "one-sided domain parity" \
	"$(q 'SELECT count(*) FROM n WHERE d4 > 15000;')" \
	"$(q 'SELECT count(*) FROM h WHERE d4 > 15000;')"

check "domain equality skips groups (#483)" \
	"$(gt0 "$(skipped 'SELECT id FROM n WHERE d4 = 12345')")" "yes"
check "domain equality parity" \
	"$(q 'SELECT count(*) FROM n WHERE d4 = 12345;')" \
	"$(q 'SELECT count(*) FROM h WHERE d4 = 12345;')"

# An absent value must still come back empty. This is the arm a wrong bloom probe
# breaks: the filter hashes the values the writer stored, and a probe that hashed
# under a different type would skip a group that holds the row.
check "domain equality on an absent value returns nothing" \
	"$(q 'SELECT count(*) FROM n WHERE d4 = 20481;')" "0"
check "domain equality on a present boundary value finds it" \
	"$(q 'SELECT count(*) FROM n WHERE d4 = 20480;')" "1"

# One-sided and equality too, because they take different branches of
# native_zone_excludes and equality additionally gates the bloom probe.
check "one-sided bigint vs integer literal skips groups (#477)" \
	"$(gt0 "$(skipped 'SELECT id FROM n WHERE k > 150000')")" "yes"
check "one-sided parity" \
	"$(q 'SELECT count(*) FROM n WHERE k > 150000;')" \
	"$(q 'SELECT count(*) FROM h WHERE k > 150000;')"

check "bigint equality vs integer literal skips groups (#477)" \
	"$(gt0 "$(skipped 'SELECT id FROM n WHERE k = 123450')")" "yes"
check "bigint equality parity" \
	"$(q 'SELECT count(*) FROM n WHERE k = 123450;')" \
	"$(q 'SELECT count(*) FROM h WHERE k = 123450;')"

# A value no row holds must still come back empty. Cross-type equality disables
# the bloom probe (the filter hashes column-type values, so an int4 constant
# would probe the wrong slot), so this rides on min/max alone.
check "bigint equality on an absent value returns nothing" \
	"$(q 'SELECT count(*) FROM n WHERE k = 123451;')" "0"

# A predicate every group satisfies must skip nothing (correctness of the bound).
check "non-selective scan skips nothing" "$(skipped 'SELECT id FROM n WHERE id > 0')" "0"
check "non-selective parity" \
	"$(q 'SELECT count(*) FROM n WHERE id > 0;')" \
	"$(q 'SELECT count(*) FROM h WHERE id > 0;')"

# A predicate no group satisfies is fully skipped and returns nothing.
check "out-of-range returns nothing" "$(q 'SELECT count(*) FROM n WHERE id > 1000000;')" "0"

# ---- an anchored LIKE is a range, and should prune like one (#426) -----------
#
# `t LIKE 'abc%'` matches exactly the strings that start with those bytes, so
# every match satisfies `t >= 'abc'` and a zone map can skip any group whose max
# is below it. It pruned nothing, because `~~` has no btree opfamily strategy and
# the clause was dropped by pgcolumnar_clause_to_scankey before any zone map saw
# it.
#
# COLLATE "C" is not incidental, it is the correctness condition. Under any other
# collation "starts with these bytes" does not imply "sorts at or after them" --
# ignorable characters are the usual counterexample -- so the range would not be
# conservative and could skip a group that really matches. The stored min/max are
# ordered under the column's own collation, so both have to be C.
psql_run "CREATE TABLE hl (t text COLLATE \"C\");"
psql_run "CREATE TABLE nl (t text COLLATE \"C\") USING pgcolumnar;"
psql_run "SELECT pgcolumnar.set_options('nl', stripe_row_limit => 2048, chunk_group_row_limit => 1024);"
NSK_LGEN="SELECT 'k' || lpad(g::text, 8, '0') FROM generate_series(1, 20480) g"
psql_run "INSERT INTO hl $NSK_LGEN;"
psql_run "INSERT INTO nl $NSK_LGEN;"

# Premise, and the one that makes the rest mean anything: the hand-written range
# the LIKE is equivalent to DOES prune on this fixture. Without it, a LIKE that
# prunes nothing could be an unprunable layout rather than a dropped operator.
check "premise: the equivalent hand-written range prunes (#426)" \
	"$(gt0 "$(skipped "SELECT t FROM nl WHERE t >= 'k00005000' AND t < 'k00005001'")")" "yes"

check "anchored LIKE result parity (#426)" \
	"$(pgc_set_hash "SELECT t FROM nl WHERE t LIKE 'k00005000%'")" \
	"$(pgc_set_hash "SELECT t FROM hl WHERE t LIKE 'k00005000%'")"

check "anchored LIKE prunes groups (#426)" \
	"$(gt0 "$(skipped "SELECT t FROM nl WHERE t LIKE 'k00005000%'")")" "yes"

# The negative, which is the half that matters for correctness: an unanchored
# pattern has no fixed prefix, so there is no range to derive and pruning it
# would be WRONG rather than merely unhelpful. Parity is asserted alongside,
# because "did not prune" and "returned the right rows" are different claims.
check "unanchored LIKE does not prune (#426)" \
	"$(gt0 "$(skipped "SELECT t FROM nl WHERE t LIKE '%5000%'")")" "no"
check "unanchored LIKE result parity (#426)" \
	"$(pgc_set_hash "SELECT t FROM nl WHERE t LIKE '%5000%'")" \
	"$(pgc_set_hash "SELECT t FROM hl WHERE t LIKE '%5000%'")"

# An escaped wildcard is a literal, so the prefix must not stop early at it and
# must not treat it as a wildcard. No row matches; the check is that the answer
# agrees with heap rather than that it is empty.
check "escaped-wildcard LIKE result parity (#426)" \
	"$(pgc_set_hash "SELECT t FROM nl WHERE t LIKE 'k0000\\%50%'")" \
	"$(pgc_set_hash "SELECT t FROM hl WHERE t LIKE 'k0000\\%50%'")"

# A trailing single-character wildcard, which is the case that catches a prefix
# scanner that does not stop at '_'.
#
# '%' cannot catch it on this fixture and that is why this check exists: '%' is
# 0x25, below every byte in the data, so a bogus prefix of '%5000%' yields a
# lower bound every row already satisfies and nothing is lost. '_' is 0x5F, ABOVE
# the digits it would sit next to, so a scanner that swallowed it would derive
# `>= 'k0000500_'` and exclude exactly the ten rows that match. Parity is the
# assertion with teeth here; the pruning check beside it is the useful half.
check "trailing _ wildcard result parity (#426)" \
	"$(pgc_set_hash "SELECT t FROM nl WHERE t LIKE 'k0000500_'")" \
	"$(pgc_set_hash "SELECT t FROM hl WHERE t LIKE 'k0000500_'")"
check "trailing _ wildcard still prunes on its prefix (#426)" \
	"$(gt0 "$(skipped "SELECT t FROM nl WHERE t LIKE 'k0000500_'")")" "yes"

# The one that actually discriminates, and it took two attempts to find.
#
# A wrong scan key cannot lose rows unless it prunes a WHOLE GROUP, because the
# executor re-applies the qual to everything a surviving group returns. So the
# bogus prefix has to exceed a group's maximum, not merely some rows. With four
# wildcards it does: swallowed, the prefix becomes 'k0000____' and 0x5F outranks
# every digit, so every group holding a match has a maximum below it and is
# skipped -- all 9,999 matching rows disappear. The correct prefix is 'k0000',
# which prunes nothing here and returns them all.
check "swallowed _ wildcards would prune away every match (#426)" \
	"$(pgc_set_hash "SELECT t FROM nl WHERE t LIKE 'k0000____'")" \
	"$(pgc_set_hash "SELECT t FROM hl WHERE t LIKE 'k0000____'")"

# A prefix at the LOW end of the data, which only an upper bound can prune.
#
# `>= 'k00000001'` is satisfied by every row in the table, so a lower bound alone
# skips nothing here however anchored the pattern is. Everything the scan can
# avoid comes from knowing the match cannot exceed the prefix's successor. That
# makes this the check that distinguishes a half-derived range from a whole one,
# rather than a second instance of the check above.
check "low-end prefix parity (#426)" \
	"$(pgc_set_hash "SELECT t FROM nl WHERE t LIKE 'k00000001%'")" \
	"$(pgc_set_hash "SELECT t FROM hl WHERE t LIKE 'k00000001%'")"
check "low-end prefix prunes, which needs the upper bound (#426)" \
	"$(gt0 "$(skipped "SELECT t FROM nl WHERE t LIKE 'k00000001%'")")" "yes"

# The collation guard, which is the line that makes any of this safe and which
# nothing above reaches: every table so far is COLLATE "C", so deleting the guard
# would not redden a single check. This column takes the database default
# collation instead, and the prefix bound must NOT be derived for it.
#
# The bound is only conservative under byte ordering. Under a collation with
# ignorable characters, a value starting with the prefix bytes can sort BEFORE
# the prefix, so `>= prefix` would skip a group that really matches and the scan
# would silently lose rows. Declining to prune costs a full read; pruning wrongly
# costs correctness, so this side is the one to be sure of.
#
# Deliberately conservative in a second way, worth stating so it is not read as a
# bug: this database is initdb'd --locale=C, so the default collation IS byte
# ordering underneath, and the guard still refuses it because the column's
# collation OID is the default rather than C. Recognising that case needs a
# stable "is this collation C" test across five majors, which the server headers
# do not export; refusing is the safe half of that trade.
psql_run "CREATE TABLE hd (t text);"
psql_run "CREATE TABLE nd (t text) USING pgcolumnar;"
psql_run "SELECT pgcolumnar.set_options('nd', stripe_row_limit => 2048, chunk_group_row_limit => 1024);"
psql_run "INSERT INTO hd $NSK_LGEN;"
psql_run "INSERT INTO nd $NSK_LGEN;"

check "premise: the range prunes on the default-collation table too (#426)" \
	"$(gt0 "$(skipped "SELECT t FROM nd WHERE t >= 'k00005000' AND t < 'k00005001'")")" "yes"

# THIS cluster is not a C-collation database and that is worth stating, because
# I assumed it was. lib.sh runs initdb with no locale, so it inherits LANG --
# C.UTF-8 here -- and PostgreSQL does not report C.UTF-8 as C collation even
# though its ordering is by code point. So a default-collation column here is
# correctly refused, and this check pins that rather than the reverse.
check "default collation that is not C is refused (#426)" \
	"$(gt0 "$(skipped "SELECT t FROM nd WHERE t LIKE 'k00005000%'")")" "no"
check "default-collation LIKE result parity (#426)" \
	"$(pgc_set_hash "SELECT t FROM nd WHERE t LIKE 'k00005000%'")" \
	"$(pgc_set_hash "SELECT t FROM hd WHERE t LIKE 'k00005000%'")"

# ...and the case the widening exists for, which needs a database this suite does
# not otherwise create. A column with the DEFAULT collation in a --locale=C
# database is byte ordering, so the bound is sound and must be derived. Keying
# the guard on an explicit COLLATE "C" instead would refuse it, and refuse every
# deployment initdb'd that way.
psql_admin "DROP DATABASE IF EXISTS nsk_cdb;" >/dev/null 2>&1
psql_admin "CREATE DATABASE nsk_cdb LC_COLLATE 'C' LC_CTYPE 'C' TEMPLATE template0;" >/dev/null 2>&1
_cdb() {  # run against the C-collation database
	env PATH="$PGC_BINDIR:$PATH" psql -h 127.0.0.1 -p "$PGC_PORT" -U postgres \
		-d nsk_cdb -At -c "$1" 2>/dev/null
}
if [ "$(_cdb "SELECT datcollate FROM pg_database WHERE datname = current_database();")" = "C" ]; then
	_cdb "CREATE EXTENSION IF NOT EXISTS pgcolumnar;" >/dev/null
	_cdb "CREATE TABLE nc (t text) USING pgcolumnar;" >/dev/null
	_cdb "SELECT pgcolumnar.set_options('nc', stripe_row_limit => 2048, chunk_group_row_limit => 1024);" >/dev/null
	_cdb "INSERT INTO nc $NSK_LGEN;" >/dev/null
	# Usable Skip Predicates rather than the removal count: it reports whether the
	# keys were DERIVED, which is what the guard decides, and it does not depend on
	# how the fixture happens to be laid out.
	check "a default-collation column in a --locale=C database derives the bounds (#426)" \
		"$(_cdb "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF)
			SELECT t FROM nc WHERE t LIKE 'k00005000%';" \
			| grep -oE 'Usable Skip Predicates: [0-9]+' | grep -oE '[0-9]+$')" \
		"2"
	check "and it returns the right rows (#426)" \
		"$(_cdb "SELECT count(*) FROM nc WHERE t LIKE 'k00005000%';")" "1"
else
	echo "-- SKIP: could not create a --locale=C database, so the widened guard is"
	echo "         NOT covered by this run"
fi

# ...and the refusal, on a collation that is genuinely not byte ordering. This is
# the check the narrow version could not have: with the guard keyed on the
# collation OID, every table here was C by construction and deleting the guard
# reddened nothing.
#
# Under a non-C collation "starts with these bytes" does not imply "sorts at or
# after them", so the bound would not be conservative and could skip a group that
# really matches. Parity is asserted with it, because "did not prune" and
# "returned the right rows" are different claims and only the second is
# correctness.
# The collation is DISCOVERED rather than named, because which ones work is a
# property of the build and the host: this container has the `unicode` entry in
# the catalog but no ICU ("ICU is not supported in this build"), and `en_US.utf8`
# depends on a locale someone generated. Naming one and hoping is how temporal.sh
# reports a red on a box missing btree_gist (#505). If none is usable the checks
# say so and are skipped, rather than passing vacuously or failing for the
# environment.
NSK_NONC=""
for _c in "en_US.utf8" "en_US.UTF-8" "unicode" "pg_unicode_fast"; do
	if [ "$(q "SELECT 'x' LIKE 'x' COLLATE \"$_c\";" 2>/dev/null)" = "t" ] &&
	   [ "$(q "SELECT ('b' < 'a' COLLATE \"$_c\');" 2>/dev/null)" = "f" ]; then
		NSK_NONC="$_c"; break
	fi
done

if [ -z "$NSK_NONC" ]; then
	echo "-- SKIP: no usable non-C collation on this build, so the refusal half of"
	echo "         the #426 collation guard is NOT covered by this run"
else
	echo "-- non-C collation under test: $NSK_NONC"
	psql_run "CREATE TABLE hu (t text COLLATE \"$NSK_NONC\");"
	psql_run "CREATE TABLE nu (t text COLLATE \"$NSK_NONC\") USING pgcolumnar;"
	psql_run "SELECT pgcolumnar.set_options('nu', stripe_row_limit => 2048, chunk_group_row_limit => 1024);"
	psql_run "INSERT INTO hu $NSK_LGEN;"
	psql_run "INSERT INTO nu $NSK_LGEN;"

	check "premise: a range still prunes under a non-C collation (#426)" \
		"$(gt0 "$(skipped "SELECT t FROM nu WHERE t >= 'k00005000' AND t < 'k00005001'")")" "yes"
	check "anchored LIKE does not prune under a non-C collation (#426)" \
		"$(gt0 "$(skipped "SELECT t FROM nu WHERE t LIKE 'k00005000%'")")" "no"
	check "non-C collation LIKE result parity (#426)" \
		"$(pgc_set_hash "SELECT t FROM nu WHERE t LIKE 'k00005000%'")" \
		"$(pgc_set_hash "SELECT t FROM hu WHERE t LIKE 'k00005000%'")"
fi

pgc_summary
