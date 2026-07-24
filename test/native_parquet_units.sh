#!/usr/bin/env bash
#
# pgColumnar Parquet TIME/TIMESTAMP unit handling (Phase G).
#
# Parquet stores a time unit alongside TIME and TIMESTAMP columns, two ways: the
# deprecated ConvertedType (TIME_MILLIS ... TIMESTAMP_MICROS) and the current
# LogicalType union, whose TimeUnit can also say NANOS. PostgreSQL stores
# microseconds, so a reader that ignores the unit silently returns values off by
# a factor of 1000 -- in range, so nothing errors and nothing looks wrong.
#
# pyarrow writes ConvertedType for millis and micros but only LogicalType for
# nanos, so this suite exercises both metadata paths.
#
# Usage:  test/native_parquet_units.sh [PG_CONFIG]
# Written fresh for pgColumnar.

set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
pgc_setup "${1:-/usr/local/pg17/bin/pg_config}"

if ! python3 -c 'import pyarrow.parquet' 2>/dev/null; then
	echo "SKIP  pyarrow not available; Parquet unit suite needs it"
	pgc_summary
	exit 0
fi

W="$PGC_WORKDIR"
psql_run "CREATE SERVER pq FOREIGN DATA WRAPPER pgcolumnar_parquet;"

# 2026-07-24 00:00:00 UTC, written in each unit. The same instant every time, so
# any unit mishandling shows up as a different timestamp rather than an error.
python3 - "$W" <<'PY'
import sys, pyarrow as pa, pyarrow.parquet as pq
W = sys.argv[1]
secs = 1784851200
for unit, mult, ver in [("ms", 10**3, "1.0"), ("us", 10**6, "1.0"), ("ns", 10**9, "2.6")]:
    pq.write_table(pa.table({"t": pa.array([secs * mult], pa.int64()).cast(pa.timestamp(unit))}),
                   f"{W}/ts_{unit}.parquet", version=ver)
# time-of-day: 01:02:03.000004 in micros (TIME_MICROS, physically INT64)
pq.write_table(pa.table({"t": pa.array([3723000004], pa.int64()).cast(pa.time64('us'))}),
               f"{W}/time_us.parquet")
# the same time to millisecond resolution (TIME_MILLIS, physically INT32)
pq.write_table(pa.table({"t": pa.array([3723000], pa.int32()).cast(pa.time32('ms'))}),
               f"{W}/time_ms.parquet")
# A genuine TIMESTAMP_MILLIS column whose conversion to microseconds overflows
# int64. An unannotated int64 would not do: with no unit there is no scaling, so
# nothing would overflow and the test would pass for the wrong reason.
pq.write_table(pa.table({"t": pa.array([9223372036854776], pa.int64()).cast(pa.timestamp('ms'))}),
               f"{W}/ms_overflow.parquet")

# Nanos truncation. pq_scale_to_usecs divides by 1000 with C semantics (toward
# zero), which the code comment reasons about but the round-number cases above do
# not exercise: they are exact multiples of 1000 and non-negative. Write a value
# with sub-microsecond digits, and a pre-epoch (negative) one, as nanos; write the
# toward-zero-truncated microseconds directly as the reference. Reading the two as
# timestamp must agree, which pins the rounding direction.
def trunc_us(ns):                       # toward zero, unlike Python's floor //
    return -(-ns // 1000) if ns < 0 else ns // 1000
for tag, ns in [("frac", 1784851200_123_456_789), ("neg", -1784851200_123_456_789)]:
    pq.write_table(pa.table({"t": pa.array([ns], pa.int64()).cast(pa.timestamp('ns'))}),
                   f"{W}/tsns_{tag}.parquet", version="2.6")
    pq.write_table(pa.table({"t": pa.array([trunc_us(ns)], pa.int64()).cast(pa.timestamp('us'))}),
                   f"{W}/tsus_{tag}.parquet", version="1.0")

# Row-group skipping over a unit-scaled column: two groups of millis timestamps
# with disjoint ranges. The FDW decodes each group's min/max through the same
# scaling as its values, so a predicate must skip the group it cannot match. If
# the stats were read unscaled while the constant is a real timestamp, the compare
# would be nonsense and the skip wrong.
HOUR = 3600000
g0base = 1704067200000                    # 2024-01-01 00:00:00 UTC, in millis
g1base = 1751328000000                    # 2025-07-01 00:00:00 UTC, in millis
g0 = [g0base + i * HOUR for i in range(1000)]   # Jan-Feb 2024, hourly
g1 = [g1base + i * HOUR for i in range(1000)]   # Jul-Aug 2025, hourly
pq.write_table(pa.table({"t": pa.array(g0 + g1, pa.int64()).cast(pa.timestamp('ms'))}),
               f"{W}/ms_groups.parquet", row_group_size=1000)
assert pq.ParquetFile(f"{W}/ms_groups.parquet").num_row_groups == 2
PY

for u in ms us ns; do
	psql_run "CREATE FOREIGN TABLE fts_$u (t timestamp) SERVER pq OPTIONS (path '$W/ts_$u.parquet');"
done
psql_run "CREATE FOREIGN TABLE ftime (t time) SERVER pq OPTIONS (path '$W/time_us.parquet');"
psql_run "CREATE FOREIGN TABLE ftime_ms (t time) SERVER pq OPTIONS (path '$W/time_ms.parquet');"

# ---- the same instant, whatever the unit -----------------------------------
# Before the unit was honoured, millis read 1000x small (1970-01-21 15:47:31.2)
# and nanos declared as timestamp read 1000x large, both silently.
for u in ms us ns; do
	check "ts_$u decodes to the right instant" \
		"$(q "SELECT t::text FROM fts_$u;")" "2026-07-24 00:00:00"
done

check "time micros decodes to the right time" \
	"$(q 'SELECT t::text FROM ftime;')" "01:02:03.000004"

# TIME_MILLIS is physically INT32, not INT64. Before the unit was resolved from
# the schema, that branch could never fire for a spec-conforming file: the column
# fell through to int4, so parquet_schema advised a number for a time column and
# declaring `time` was rejected as an incompatible physical type.
check "time millis decodes to the right time" \
	"$(q 'SELECT t::text FROM ftime_ms;')" "01:02:03"
check "parquet_schema advises time for TIME_MILLIS" \
	"$(q "SELECT data_type FROM pgcolumnar.parquet_schema('$W/time_ms.parquet');")" \
	"time without time zone"

# ---- schema advice ---------------------------------------------------------
# millis and micros convert to timestamp without loss, so parquet_schema advises
# it. Nanos cannot: PostgreSQL has no nanosecond timestamp, so the advice stays
# bigint, which is exact. Declaring timestamp anyway is allowed and truncates.
check "parquet_schema advises timestamp for millis" \
	"$(q "SELECT data_type FROM pgcolumnar.parquet_schema('$W/ts_ms.parquet');")" \
	"timestamp without time zone"
check "parquet_schema advises timestamp for micros" \
	"$(q "SELECT data_type FROM pgcolumnar.parquet_schema('$W/ts_us.parquet');")" \
	"timestamp without time zone"
# bigint, not timestamp: nanos cannot reach microseconds without loss, and not
# float8 either, which has 53 mantissa bits for a value needing about 61.
check "parquet_schema advises bigint for nanos (lossless)" \
	"$(q "SELECT data_type FROM pgcolumnar.parquet_schema('$W/ts_ns.parquet');")" \
	"bigint"
check "nanos read as bigint is exact" \
	"$(q "SELECT t::text FROM pgcolumnar.read_parquet('$W/ts_ns.parquet') AS t(t int8);")" \
	"1784851200000000000"

# ---- overflow --------------------------------------------------------------
# INT64 max milliseconds cannot be expressed in microseconds; it must raise
# rather than wrap. 22008 is datetime_field_overflow.
psql_run "CREATE FUNCTION pgc_try(q text) RETURNS text LANGUAGE plpgsql AS \$\$
          BEGIN EXECUTE q; RETURN 'NO ERROR';
          EXCEPTION WHEN OTHERS THEN RETURN SQLSTATE; END \$\$;"
sqlstate() { q "SELECT pgc_try(\$q\$$1\$q\$);"; }

check "millis-to-micros overflow raises 22008" \
	"$(sqlstate "SELECT * FROM pgcolumnar.read_parquet('$W/ms_overflow.parquet') AS t(t timestamp)")" \
	"22008"

# ---- all three surfaces share the decoder ----------------------------------
check "read_parquet honours the unit" \
	"$(q "SELECT t::text FROM pgcolumnar.read_parquet('$W/ts_ms.parquet') AS t(t timestamp);")" \
	"2026-07-24 00:00:00"

psql_run "CREATE TABLE imp_ms (t timestamp);"
psql_run "SELECT pgcolumnar.import_parquet('imp_ms'::regclass, '$W/ts_ms.parquet');"
check "import_parquet honours the unit" \
	"$(q 'SELECT t::text FROM imp_ms;')" "2026-07-24 00:00:00"

# ---- oracle: every unit agrees with every other -----------------------------
check "millis == micros" \
	"$(pgc_set_hash 'SELECT t FROM fts_ms')" "$(pgc_set_hash 'SELECT t FROM fts_us')"
check "nanos == micros" \
	"$(pgc_set_hash 'SELECT t FROM fts_ns')" "$(pgc_set_hash 'SELECT t FROM fts_us')"

# ---- nanos truncation direction (toward zero) ------------------------------
# The nanos value carries sub-microsecond digits the micros reference cannot, so
# equality holds only if the reader truncates toward zero exactly as the reference
# was computed. The negative case is the one that distinguishes toward-zero from
# floor.
for tag in frac neg; do
	check "nanos sub-us truncates toward zero [$tag]" \
		"$(q "SELECT t::text FROM pgcolumnar.read_parquet('$W/tsns_$tag.parquet') AS t(t timestamp);")" \
		"$(q "SELECT t::text FROM pgcolumnar.read_parquet('$W/tsus_$tag.parquet') AS t(t timestamp);")"
done

# ---- pushdown over a unit-scaled column ------------------------------------
# The predicate falls entirely in the second group's range, so the first group's
# stats (Jan-Mar 2024) prove it empty and it must be skipped -- but only if those
# stats are scaled from millis the same way the values are.
psql_run "CREATE FOREIGN TABLE fmsg (t timestamp) SERVER pq OPTIONS (path '$W/ms_groups.parquet');"
skipped_ms() {
	q "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF)
	   SELECT count(*) FROM fmsg WHERE $1" \
		| grep 'Row Groups Skipped' | grep -oE '[0-9]+' | head -1
}
# 2025-06-01 is inside group 1 only.
check "millis-scaled stats skip the non-matching group" \
	"$(skipped_ms "t >= timestamp '2025-06-01'")" "1"
# correctness: the skip must not drop a matching row (oracle = a heap mirror)
psql_run "CREATE TABLE hmsg (t timestamp);"
psql_run "INSERT INTO hmsg SELECT t FROM pgcolumnar.read_parquet('$W/ms_groups.parquet') AS t(t timestamp);"
check "millis pushdown result == oracle" \
	"$(pgc_set_hash "SELECT t FROM fmsg WHERE t >= timestamp '2025-06-01'")" \
	"$(pgc_set_hash "SELECT t FROM hmsg WHERE t >= timestamp '2025-06-01'")"

pgc_summary
