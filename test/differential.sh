#!/usr/bin/env bash
#
# pgColumnar differential type-matrix and boundary suite.
#
# The governing property: a columnar table and a heap table loaded with the
# same data must answer every query identically. Heap is the oracle. This
# catches encode/decode, null-handling, and chunk-skipping bugs generically,
# across a broad matrix of data types, null patterns, and boundary sizes, which
# is exactly what upcoming lightweight-encoding work will stress.
#
# Usage:  test/differential.sh [PG_CONFIG]
#
# Written fresh for pgColumnar; it does not reuse any upstream test file.

set -uo pipefail

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

# ---------------------------------------------------------------------------
# Part 1: type matrix
#
# One column per type, a moderate null density, negatives and extremes, empty
# strings distinct from NULL, and a few toast-sized text values. Small chunk and
# stripe limits force many chunk groups and several stripes over modest data.
# ---------------------------------------------------------------------------
echo "-- part 1: type matrix"

MATRIX_DEFS="
	id        int,
	c_int     int,
	c_big     bigint,
	c_small   smallint,
	c_num     numeric(24,6),
	c_f4      real,
	c_f8      double precision,
	c_bool    boolean,
	c_date    date,
	c_ts      timestamp,
	c_tstz    timestamptz,
	c_uuid    uuid,
	c_bytea   bytea,
	c_vc      varchar(50),
	c_char    char(10),
	c_iv      interval,
	c_jsonb   jsonb,
	c_arr     int[],
	c_text    text,
	c_ztext   text"

make_pair "$MATRIX_DEFS"
q "SELECT columnar.alter_columnar_table_set('t_col', chunk_group_row_limit => 1000, stripe_row_limit => 5000);" >/dev/null

load_pair "SELECT
	g AS id,
	CASE WHEN g%5=0  THEN NULL ELSE (g*7-100) END,
	CASE WHEN g%17=0 THEN NULL ELSE (g::bigint*1000000000) END,
	CASE WHEN g%17=0 THEN NULL ELSE ((g%100)-50)::smallint END,
	CASE WHEN g%11=0 THEN NULL ELSE (g::numeric*1.250000-1000) END,
	CASE WHEN g%17=0 THEN NULL ELSE (g*0.5-10)::real END,
	CASE WHEN g%3=0  THEN NULL ELSE (sqrt(g::float8)*(CASE WHEN g%2=0 THEN -1 ELSE 1 END)) END,
	CASE WHEN g%17=0 THEN NULL ELSE (g%3=0) END,
	CASE WHEN g%17=0 THEN NULL ELSE (DATE '2000-01-01' + g) END,
	CASE WHEN g%17=0 THEN NULL ELSE (TIMESTAMP '2000-01-01' + make_interval(hours => g)) END,
	CASE WHEN g%17=0 THEN NULL ELSE (TIMESTAMPTZ '2000-01-01 00:00:00+00' + make_interval(hours => g)) END,
	CASE WHEN g%13=0 THEN NULL ELSE (md5(g::text)::uuid) END,
	CASE WHEN g%13=0 THEN NULL ELSE decode(md5(g::text),'hex') END,
	CASE WHEN g%7=0  THEN NULL ELSE ('v'||g) END,
	CASE WHEN g%17=0 THEN NULL ELSE lpad(g::text,10,'0') END,
	CASE WHEN g%17=0 THEN NULL ELSE make_interval(days => g%1000, hours => g%24) END,
	CASE WHEN g%13=0 THEN NULL ELSE jsonb_build_object('n',g,'neg',-g,'s','x'||g) END,
	CASE WHEN g%17=0 THEN NULL ELSE ARRAY[g, g+1, -g] END,
	CASE WHEN g%7=0  THEN NULL WHEN g%500=0 THEN repeat('abc',4000) ELSE ('t'||g) END,
	CASE WHEN g%2=0  THEN '' ELSE ('z'||g) END
	FROM generate_series(1,12000) g"

# Sanity: the data actually spans multiple chunk groups and stripes.
check "matrix row count"       "$(q 'SELECT count(*) FROM t_col;')" "12000"
check "matrix chunk groups>=12" "$([ "$(chunk_group_count)" -ge 12 ] && echo ok)" "ok"
check "matrix stripes>=2"       "$([ "$(stripe_count)" -ge 2 ] && echo ok)" "ok"

# Whole-row equality across the entire table.
diff_query "matrix whole-row" "SELECT * FROM %T"

# Every column: full projection, non-null count, and IS NULL positions.
ALL_COLS="c_int c_big c_small c_num c_f4 c_f8 c_bool c_date c_ts c_tstz c_uuid c_bytea c_vc c_char c_iv c_jsonb c_arr c_text c_ztext"
for c in $ALL_COLS; do
	diff_query "$c project"   "SELECT id, $c FROM %T"
	diff_query "$c count"     "SELECT count($c) FROM %T"
	diff_query "$c is null"   "SELECT id FROM %T WHERE $c IS NULL"
	diff_query "$c not null"  "SELECT id FROM %T WHERE $c IS NOT NULL"
done

# Ordered types with a min/max aggregate (uuid and bytea order under btree but
# have no min/max aggregate; their ordering is covered by the range predicates).
ORDERED="c_int c_big c_small c_num c_f4 c_f8 c_date c_ts c_tstz c_vc c_char c_iv c_text c_ztext"
for c in $ORDERED; do
	diff_query "$c min/max" "SELECT min($c), max($c) FROM %T"
done

# Numeric types: sum and avg.
for c in c_int c_big c_small c_num c_f4 c_f8; do
	diff_query "$c sum/avg" "SELECT sum($c), avg($c) FROM %T"
done

# Range predicates that drive chunk-group skipping, per type.
diff_query "c_int range"   "SELECT id FROM %T WHERE c_int BETWEEN -50 AND 5000"
diff_query "c_big range"   "SELECT id FROM %T WHERE c_big > 6000000000000"
diff_query "c_num range"   "SELECT id FROM %T WHERE c_num BETWEEN 0 AND 5000"
diff_query "c_f8 range"    "SELECT id FROM %T WHERE c_f8 < 0"
diff_query "c_date range"  "SELECT id FROM %T WHERE c_date BETWEEN DATE '2005-01-01' AND DATE '2010-01-01'"
diff_query "c_ts range"    "SELECT id FROM %T WHERE c_ts >= TIMESTAMP '2000-06-01'"
diff_query "c_tstz range"  "SELECT id FROM %T WHERE c_tstz < TIMESTAMPTZ '2000-03-01 00:00:00+00'"
diff_query "c_vc range"    "SELECT id FROM %T WHERE c_vc BETWEEN 'v100' AND 'v200'"
diff_query "c_uuid range"  "SELECT id FROM %T WHERE c_uuid > '80000000-0000-0000-0000-000000000000'::uuid"
diff_query "c_bytea range" "SELECT id FROM %T WHERE c_bytea > '\\\\x80'::bytea"
diff_query "c_text range"  "SELECT id FROM %T WHERE c_text > 't5000'"

# Equality predicates.
diff_query "c_int eq"   "SELECT id FROM %T WHERE c_int = 600"
diff_query "c_vc eq"    "SELECT id FROM %T WHERE c_vc = 'v500'"
diff_query "c_bool eq"  "SELECT id FROM %T WHERE c_bool = true"
diff_query "c_uuid eq"  "SELECT id FROM %T WHERE c_uuid = md5('999')::uuid"
diff_query "c_jsonb eq" "SELECT id FROM %T WHERE c_jsonb = jsonb_build_object('n',600,'neg',-600,'s','x600')"
diff_query "c_arr eq"   "SELECT id FROM %T WHERE c_arr = ARRAY[600,601,-600]"

# Ordered projections with LIMIT (deterministic order on unique id).
diff_query "order limit head" "SELECT id, c_int, c_text FROM %T ORDER BY id LIMIT 25"
diff_query "order limit tail" "SELECT id, c_num, c_vc FROM %T ORDER BY id DESC LIMIT 25"

# Compound predicate mixing several columns.
diff_query "compound" "SELECT id FROM %T WHERE c_int > 0 AND c_bool AND c_vc IS NOT NULL AND c_num < 8000"

# ---------------------------------------------------------------------------
# Part 2: boundary conditions
# ---------------------------------------------------------------------------
echo "-- part 2: boundary conditions"

# Empty table.
make_pair "id int, v text"
diff_query "empty scan"  "SELECT * FROM %T"
diff_query "empty count" "SELECT count(*) FROM %T"
diff_query "empty agg"   "SELECT min(id), max(id), sum(id) FROM %T"

# Single row.
make_pair "id int, v text"
load_pair "SELECT 1, 'only'"
diff_query "single scan"  "SELECT * FROM %T"
diff_query "single count" "SELECT count(*) FROM %T"

# Chunk-group boundary: with a 100-row chunk group limit, N rows must produce
# ceil(N/100) chunk groups, and results must match at N-1, N, N+1.
for N in 99 100 101 200 201 250; do
	make_pair "id int, v text"
	q "SELECT columnar.alter_columnar_table_set('t_col', chunk_group_row_limit => 100, stripe_row_limit => 100000);" >/dev/null
	load_pair "SELECT g, 'r'||g FROM generate_series(1,$N) g"
	want=$(( (N + 99) / 100 ))
	check "cg boundary N=$N groups"  "$(chunk_group_count)" "$want"
	diff_query "cg boundary N=$N scan"  "SELECT * FROM %T"
	diff_query "cg boundary N=$N range" "SELECT id FROM %T WHERE id BETWEEN 50 AND $((N-10))"
done

# Stripe boundary: the product enforces stripe_row_limit >= 1000, so use a
# 1000-row stripe limit with 100-row chunk groups and check N-1, N, N+1.
for N in 1000 1001 2000 2001; do
	make_pair "id int, v text"
	q "SELECT columnar.alter_columnar_table_set('t_col', chunk_group_row_limit => 100, stripe_row_limit => 1000);" >/dev/null
	load_pair "SELECT g, 'r'||g FROM generate_series(1,$N) g"
	want=$(( (N + 999) / 1000 ))
	check "stripe boundary N=$N stripes" "$(stripe_count)" "$want"
	diff_query "stripe boundary N=$N scan" "SELECT * FROM %T"
	diff_query "stripe boundary N=$N agg"  "SELECT count(*), min(id), max(id), sum(id) FROM %T"
done

# All-null column across many rows.
make_pair "id int, allnull int, v text"
q "SELECT columnar.alter_columnar_table_set('t_col', chunk_group_row_limit => 100);" >/dev/null
load_pair "SELECT g, NULL::int, 'r'||g FROM generate_series(1,350) g"
diff_query "allnull column scan"  "SELECT * FROM %T"
diff_query "allnull column count" "SELECT count(allnull) FROM %T"
diff_query "allnull column isnull" "SELECT count(*) FROM %T WHERE allnull IS NULL"
diff_query "allnull column minmax" "SELECT min(allnull), max(allnull) FROM %T"

# All-null chunk group: with 100-row groups, rows 101..200 have a NULL column,
# the rest have values, so exactly one whole chunk group is all-null.
make_pair "id int, sometimes int"
q "SELECT columnar.alter_columnar_table_set('t_col', chunk_group_row_limit => 100);" >/dev/null
load_pair "SELECT g, CASE WHEN g BETWEEN 101 AND 200 THEN NULL ELSE g END FROM generate_series(1,400) g"
diff_query "allnull chunk scan"   "SELECT * FROM %T"
diff_query "allnull chunk range"  "SELECT id FROM %T WHERE sometimes BETWEEN 150 AND 250"
diff_query "allnull chunk isnull" "SELECT id FROM %T WHERE sometimes IS NULL"

# Empty string vs NULL must stay distinct.
make_pair "id int, s text"
load_pair "SELECT g, CASE WHEN g%2=0 THEN '' WHEN g%3=0 THEN NULL ELSE 'x'||g END FROM generate_series(1,300) g"
diff_query "empty-vs-null scan"    "SELECT * FROM %T"
diff_query "empty-vs-null empties" "SELECT count(*) FROM %T WHERE s = ''"
diff_query "empty-vs-null nulls"   "SELECT count(*) FROM %T WHERE s IS NULL"

# Wide row: many columns.
WIDE_DEFS="id int"
WIDE_SEL="g"
for i in $(seq 1 60); do
	WIDE_DEFS="$WIDE_DEFS, c$i int"
	WIDE_SEL="$WIDE_SEL, (g*$i - $i)"
done
make_pair "$WIDE_DEFS"
load_pair "SELECT $WIDE_SEL FROM generate_series(1,500) g"
diff_query "wide row scan" "SELECT * FROM %T"
diff_query "wide row proj" "SELECT id, c1, c30, c60 FROM %T WHERE c30 > 5000"

# ---------------------------------------------------------------------------
# Part 3: lightweight encodings (I1)
#
# Data shaped so each encoding is chosen, then checked two ways: the oracle
# proves round-trip correctness, and columnar.chunk is inspected to prove the
# encoding was actually applied (guards against the layer silently regressing to
# none). A high-entropy column is expected to stay unencoded.
# ---------------------------------------------------------------------------
echo "-- part 3: lightweight encodings"

make_pair "id int, seqv bigint, lowcard int, constv int, rnd bigint"
q "SELECT columnar.alter_columnar_table_set('t_col', chunk_group_row_limit => 2000, stripe_row_limit => 20000, compression => 'none');" >/dev/null
load_pair "SELECT g, g::bigint*3, g%4, 42, ((g*2654435761)%1000000000)::bigint FROM generate_series(1,10000) g"

# correctness through the oracle
diff_query "enc whole-row"  "SELECT * FROM %T"
diff_query "enc seq range"  "SELECT id FROM %T WHERE seqv BETWEEN 100 AND 5000"
diff_query "enc lowcard eq" "SELECT id FROM %T WHERE lowcard = 2"
diff_query "enc const scan" "SELECT id FROM %T WHERE constv = 42"
diff_query "enc aggregate"  "SELECT sum(seqv), min(lowcard), max(lowcard), count(constv), sum(rnd) FROM %T"

# the encoding was actually applied for the shaped columns
enc_applied() {
	q "SELECT bool_and(value_encoding_type <> 0) FROM columnar.chunk
	   WHERE storage_id = columnar.get_storage_id('t_col') AND attr_num = $1;"
}
check "enc id encoded"      "$(enc_applied 1)" "t"
check "enc seqv encoded"    "$(enc_applied 2)" "t"
check "enc lowcard encoded" "$(enc_applied 3)" "t"
check "enc constv encoded"  "$(enc_applied 4)" "t"

# Gorilla (float XOR) and delta-of-delta (regular-interval timestamp), I4.
# alt: a random walk -> many distinct (dict bails), irregular bit deltas (for/
# delta lose), but small consecutive XOR -> Gorilla wins. tsreg: fixed-interval
# timestamps -> zero delta-of-delta -> DOD beats delta.
make_pair "id int, alt float8, tsreg timestamp, fr float8"
q "SELECT columnar.alter_columnar_table_set('t_col', chunk_group_row_limit => 2000, stripe_row_limit => 20000, compression => 'none');" >/dev/null
load_pair "SELECT g,
	(1000 + sum(random() - 0.5) OVER (ORDER BY g))::float8,
	TIMESTAMP '2020-01-01' + make_interval(mins => g),
	(100 + (g%7) * 0.25)::float8
	FROM generate_series(1,10000) g"
diff_query "i4 whole-row"  "SELECT * FROM %T"
diff_query "i4 float agg"  "SELECT count(alt), min(alt), max(alt), count(fr), min(fr), max(fr) FROM %T"
diff_query "i4 ts range"   "SELECT id FROM %T WHERE tsreg >= TIMESTAMP '2020-01-05'"
diff_query "i4 ts minmax"  "SELECT min(tsreg), max(tsreg) FROM %T"
enc_has() {
	q "SELECT bool_or(value_encoding_type = $2) FROM columnar.chunk
	   WHERE storage_id = columnar.get_storage_id('t_col') AND attr_num = $1;"
}
check "i4 alt uses gorilla" "$(enc_has 2 4)" "t"
check "i4 tsreg uses dod"   "$(enc_has 3 5)" "t"

# Dictionary (I5) for low-cardinality columns, including varlena/text which had
# no lightweight encoding before. cat/tag repeat a few distinct values.
make_pair "id int, cat text, tag varchar(16), code int, hicard text"
q "SELECT columnar.alter_columnar_table_set('t_col', chunk_group_row_limit => 2000, stripe_row_limit => 20000, compression => 'none');" >/dev/null
load_pair "SELECT g,
	(ARRAY['north','south','east','west'])[1 + g%4],
	('t' || (g%6))::varchar(16),
	g%5,
	md5(g::text)
	FROM generate_series(1,10000) g"
diff_query "dict whole-row"   "SELECT * FROM %T"
diff_query "dict text eq"     "SELECT id FROM %T WHERE cat = 'east'"
diff_query "dict text agg"    "SELECT count(cat), min(cat), max(cat), count(distinct tag) FROM %T"
diff_query "dict text group"  "SELECT cat, count(*) FROM %T GROUP BY cat"
check "dict cat uses dict"    "$(enc_has 2 6)" "t"
check "dict tag uses dict"    "$(enc_has 3 6)" "t"
check "dict code encoded"     "$(enc_applied 4)" "t"
# a high-cardinality text column stays unencoded (dict bails)
check "dict hicard none"      "$(enc_has 5 0)" "t"

# a NONE-compression table still round-trips (encoding independent of codec)
make_pair "id int, v bigint"
q "SELECT columnar.alter_columnar_table_set('t_col', compression => 'none');" >/dev/null
load_pair "SELECT g, (g%7)::bigint FROM generate_series(1,5000) g"
diff_query "enc+nocompress scan" "SELECT * FROM %T"
diff_query "enc+nocompress agg"  "SELECT count(*), sum(v), min(v), max(v) FROM %T"

# ---------------------------------------------------------------------------
# Part 4: compressed execution (I3)
#
# Aggregates over runs of the value stream must match the heap oracle whether
# the run path is on or off. Data has low-cardinality runs (run path), nulls
# (skipped by the value stream), and deletes (force the per-group fallback), so
# both the run path and its fallback are exercised. Setting the GUC per database
# makes it apply to the fresh session each query opens.
# ---------------------------------------------------------------------------
echo "-- part 4: compressed execution"

set_guc() { q "ALTER DATABASE $PGC_DB SET $1 = $2;" >/dev/null; }

make_pair "id int, k int, big int, s smallint, nv int"
q "SELECT columnar.alter_columnar_table_set('t_col', chunk_group_row_limit => 1000, stripe_row_limit => 5000);" >/dev/null
load_pair "SELECT g, g%6, g*2, ((g%100)-50)::smallint,
	CASE WHEN g%9=0 THEN NULL ELSE g%13 END
	FROM generate_series(1,20000) g"
psql_run "DELETE FROM t_heap WHERE id % 50 = 0;"
psql_run "DELETE FROM t_col  WHERE id % 50 = 0;"

for mode in on off; do
	set_guc columnar.enable_compressed_execution "$mode"
	diff_query "cexec=$mode count"  "SELECT count(*), count(k), count(nv) FROM %T"
	diff_query "cexec=$mode sum"    "SELECT sum(big), sum(k), sum(nv) FROM %T"
	diff_query "cexec=$mode avg"    "SELECT avg(k), avg(nv) FROM %T"
	diff_query "cexec=$mode minmax" "SELECT min(k), max(k), min(big), max(big), min(s), max(s), min(nv), max(nv) FROM %T"
done
q "ALTER DATABASE $PGC_DB RESET columnar.enable_compressed_execution;" >/dev/null

# ---------------------------------------------------------------------------
# Part 5: bloom-filter equality skipping (I7)
#
# Values are hash-spread so every chunk's min/max spans the domain and cannot
# skip an in-range equality probe; the bloom filter is what prunes it. Correct-
# ness is checked against the heap oracle; a bloom is confirmed built; and an
# absent in-range value is shown to remove more chunk groups with the filter on
# than off (min/max alone).
# ---------------------------------------------------------------------------
echo "-- part 5: bloom equality skipping"

make_pair "id int, k bigint, u uuid"
q "SELECT columnar.alter_columnar_table_set('t_col', chunk_group_row_limit => 1000, stripe_row_limit => 20000);" >/dev/null
load_pair "SELECT g, ((g*2654435761)%100000)::bigint, md5((g%99999)::text)::uuid
	FROM generate_series(1,20000) g"

diff_query "bloom k present" "SELECT id FROM %T WHERE k = ((7*2654435761)%100000)::bigint"
diff_query "bloom u eq"      "SELECT count(*) FROM %T WHERE u = md5('123')::uuid"
diff_query "bloom k range"   "SELECT count(*) FROM %T WHERE k < 50000"

bloom_built() {
	q "SELECT bool_or(bloom_filter IS NOT NULL) FROM columnar.chunk
	   WHERE storage_id = columnar.get_storage_id('t_col') AND attr_num = $1;"
}
check "bloom built for k" "$(bloom_built 2)" "t"
check "bloom built for u" "$(bloom_built 3)" "t"

# an absent value strictly inside the global min/max, so min/max alone cannot
# skip it (every hash-spread chunk's range contains it) -- only bloom can.
absent=$(q "SELECT v FROM generate_series((SELECT min(k)+1 FROM t_heap)::int,
											(SELECT max(k)-1 FROM t_heap)::int) v
			WHERE v NOT IN (SELECT k FROM t_heap) LIMIT 1;")
removed() {
	q "SET columnar.enable_bloom_filter=$1;
	   EXPLAIN (ANALYZE, TIMING off, SUMMARY off) SELECT id FROM t_col WHERE k = ${absent}::bigint;" \
		| grep -oiE 'Removed by Filter: [0-9]+' | grep -oE '[0-9]+$'
}
on=$(removed on)
off=$(removed off)
check "bloom absent correct"   "$(q "SELECT count(*) FROM t_col WHERE k = ${absent}::bigint;")" "0"
check "bloom removes >= minmax" "$([ "${on:-0}" -gt "${off:-0}" ] && echo yes)" "yes"

pgc_summary
