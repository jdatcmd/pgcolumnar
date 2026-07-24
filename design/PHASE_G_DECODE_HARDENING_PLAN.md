# Phase G: Parquet decode hardening

Two correctness defects on `main`, both recorded as follow-ups when #101 merged.
Neither is caused by pushdown; #101's inversion guard only stopped the skip path
from trusting the first one's output.

## Defect 1: unchecked narrowing in `plain_value_to_datum()`

`pq_want_phys()` binds several PostgreSQL types to a wider Parquet physical type,
and the decoder converts with an unchecked cast. A value outside the target
type's range silently becomes a different value rather than raising an error.

Confirmed: an `int2` column over a Parquet INT32 column holding 30000 and 40000
reads back as `[-25536, 30000]`. PostgreSQL itself rejects `40000::int2` with
"smallint out of range", so the read path is inconsistent with the type it claims
to produce.

Four conversions in the same function share the defect:

| Target | Physical | Conversion | Failure |
|---|---|---|---|
| `int2` | INT32 | `(int16) v` | wraps, e.g. 40000 -> -25536 |
| `date` | INT32 | `(DateADT) (v - PG_TO_UNIX_DAYS)` | int32 subtraction overflows; result may be outside PostgreSQL's date range |
| `timestamp`, `timestamptz` | INT64 | `(Timestamp) (v - PG_TO_UNIX_USECS)` | int64 subtraction overflows; result may be outside PostgreSQL's timestamp range |
| `time` | INT64 | `(TimeADT) v` | `TimeADT` must be within `[0, USECS_PER_DAY]`; any other value is an invalid time |

### Approach

Range-check and raise, rather than refusing the bind in `build_imp_targets()`.
Binding `int2` to INT32 is legitimate and necessary, since Parquet has no 16-bit
physical type and represents `int2` as INT32 with an `INT(16, true)` logical
annotation. Refusing the bind would break every file whose values do fit. Erroring
on the offending value matches what PostgreSQL does for the same conversion.

The function has two classes of caller with different needs:

- **Data path** (dictionary decode, plain decode): must raise a specific error.
  Today an out-of-range value would have to be reported through the existing
  `*ok = false` path, whose message is "could not decode Parquet column N in row
  group M". That is misleading: the file is well-formed, the value simply does not
  fit the declared column type.
- **Statistics path** (`pqfdw_clause_excludes_group`): must **not** raise. A group
  whose statistics fall outside the bound type may still be legitimately skipped,
  or may never be read at all. Erroring at `BeginForeignScan` would fail queries
  that would otherwise succeed.

So `plain_value_to_datum()` gains a `bool strict` parameter:
- `strict = true` -> `ereport(ERROR, ...)` naming the value and target type.
- `strict = false` -> `*ok = false`, caller declines to skip.

Error codes: `ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE` for `int2`,
`ERRCODE_DATETIME_FIELD_OVERFLOW` for the date/time/timestamp cases.

Overflow-safe subtraction uses `pg_sub_s32_overflow` / `pg_sub_s64_overflow` from
`common/int.h` (present in PG13 through 19). Range validity uses `IS_VALID_DATE`
and `IS_VALID_TIMESTAMP` from `datatype/timestamp.h` (present on all majors).

## Defect 2: row-group chunk list is trusted

`pq_slurp_and_parse()` allocates `rg->chunks` from the row group's **own** column
count, and leaves it `NULL` when the `columns` field is absent:

```c
rg->chunks = NULL;
...
rg->chunks = palloc0(sizeof(PqChunk) * Max(cn, 1));
```

Every consumer indexes it by the **schema's** leaf count instead:

- `pq_read_rows()` iterates `i < pf->ncols` and takes `&g->chunks[i]`.
- `pqfdw_compute_skip()` takes `&pf->rgs[rg].chunks[top->firstLeaf]`.

A file whose row group carries fewer column chunks than the schema has leaves is
an out-of-bounds read; one carrying none is a NULL dereference. Both are reachable
from a crafted or truncated file.

### Approach

Record the parsed count per row group, then validate once the schema-derived
`pf->ncols` is known. `ncols` is computed after the row-group loop, so the check
belongs after that derivation, not inline. A mismatch fails the parse, which the
caller already turns into a clean error.

This is a validation, not a repair: a row group that disagrees with the schema is
malformed and there is no safe interpretation to fall back on.

## Tests

New suite `test/native_parquet_hardening.sh`, registered in
`run_all_versions.sh`. `corruption.sh` covers the native catalog and `hardening.sh`
mutates native on-disk bytes; neither covers Parquet input, so this is a new
surface rather than an extension of either.

Narrowing, built with pyarrow:
- `int2` over INT32 values beyond `int16` -> error, not a wrapped value.
- `int2` over INT32 values within range -> still works, proving the check did not
  disable the legitimate bind.
- `time` over INT64 micros beyond a day -> error.
- `date` and `timestamp` at extreme values -> error rather than a garbage date.
- The same files through `read_parquet()` and `import_parquet()`, since all three
  surfaces share the decoder.

Malformed chunk list, by byte-editing a valid file's Thrift footer:
- a row group with a short `columns` list -> clean error, backend survives.
- a row group with no `columns` field -> clean error, backend survives.

Run on an assert-enabled build so an out-of-bounds access trips an assertion
rather than passing silently.

## Knock-on: the pushdown suite's inversion test

`native_parquet_pushdown.sh` reached the `min > max` guard added in #101 by
binding `int2` over out-of-range INT32 values. Fixing defect 1 makes that raise,
so the scenario can no longer produce readable inverted statistics and the test
had to change.

Replaced with Parquet's `UINT_32` logical type, which is the other inversion the
guard covers and stays readable: stored as physical INT32 but with statistics
ordered unsigned, a group spanning the sign boundary reports `min=1,
max=-1294967296` while the values themselves decode as ordinary `int4`. Verified
to still fail with the guard removed, so it remains a real regression test.

## Confirmed pre-existing defect, not fixed here

`pq_leaf_to_pgtype()` maps both `TIMESTAMP_MILLIS` and `TIMESTAMP_MICROS` to
`timestamp`, but `converted_type` never reaches `PqColPlan` and the decoder treats
the raw int64 as microseconds. A millisecond column therefore reads 1000x small.
Confirmed: a file holding 2026-07-24 00:00:00 as `timestamp('ms')`, with
`parquet_schema()` advising `timestamp`, reads back as `1970-01-21 15:47:31.2`.

`TIME_MILLIS` has the same gap. This is the same class as defect 1, a read path
inconsistent with the type it claims to produce, but it is pre-existing and needs
`converted_type` plumbed into the column plan plus a scaling step, so it gets its
own change rather than widening this one.

## Gate

Full 15 through 19 matrix, in the `pgcolumnar-dev` container, via
`test/run_all_versions.sh` (which builds each major in its own tree).
