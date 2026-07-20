# Gap 27 (impl): Arrow / Parquet export type coverage

Extends `columnar.export_arrow` and `columnar.export_parquet` beyond the first
slice (int2/4/8, float4/8, bool, text/varchar, bytea) with the common warehouse
scalar types. Additive: no on-disk format change, no change to existing query
results, existing exports byte-compatible. Little-endian hosts only, as before.

## Type mapping added

| PostgreSQL | Arrow | Parquet |
| --- | --- | --- |
| `date` | Date32 (days, unit=DAY) | INT32, ConvertedType DATE |
| `time` | Time64 (unit=MICROSECOND) | INT64, ConvertedType TIME_MICROS |
| `timestamp` | Timestamp (unit=us, no tz) | INT64, ConvertedType TIMESTAMP_MICROS |
| `timestamptz` | Timestamp (unit=us, tz="UTC") | INT64, ConvertedType TIMESTAMP_MICROS |
| `uuid` | FixedSizeBinary(16) | FIXED_LEN_BYTE_ARRAY(16) |
| `numeric(p,s)`, p≤38, s∈[0,p] | Decimal128(p,s) | FIXED_LEN_BYTE_ARRAY(16), ConvertedType DECIMAL |
| `numeric` unconstrained / p>38 / NaN / Inf | Utf8 (text) | BYTE_ARRAY, ConvertedType UTF8 |
| `json`, `jsonb` | Utf8 (text) | BYTE_ARRAY, ConvertedType UTF8 |

### Epoch and unit conversions

- `date`: PostgreSQL stores int32 days since 2000-01-01. Both targets count days
  since 1970-01-01, so add `POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE` (10957).
- `time`: PostgreSQL stores int64 microseconds since midnight; both targets use
  microseconds directly.
- `timestamp`/`timestamptz`: PostgreSQL stores int64 microseconds since
  2000-01-01 (timestamptz is held in UTC). Both targets count microseconds since
  1970-01-01 UTC, so add `(POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * USECS_PER_DAY`
  (946684800000000). `timestamptz` additionally carries the `UTC` zone in Arrow
  and is implicitly UTC-adjusted in Parquet's TIMESTAMP_MICROS.
- `uuid`: 16 raw bytes, copied verbatim.

### Decimal

`numeric(p,s)` with `1 ≤ p ≤ 38` and `0 ≤ s ≤ p` maps to a 128-bit decimal. The
unscaled integer is produced by formatting the value (`numeric_out`), splitting on
the decimal point, and padding the fraction to exactly `s` digits — the stored
value already carries scale `s`, so no rounding is required. Arrow Decimal128
values are 16-byte little-endian two's complement; Parquet DECIMAL in a
FIXED_LEN_BYTE_ARRAY is 16-byte big-endian two's complement. `NaN`/`Infinity`
(which a decimal cannot represent) and unconstrained or over-precision `numeric`
fall back to the text representation, matched in both writers so a column's type
is stable across formats.

## Not in this slice

Arrays and composite/row types require nested Arrow List buffers and Parquet
repetition levels — the flat writers emit neither today. `jsonb` maps to text
here; a structured mapping is out of scope. These remain follow-ups on the interop
gap.

## Testing

`test/arrow_export.sh` and `test/parquet_export.sh` gain a mixed-type table
covering every mapping above (including nulls, NaN/Inf numeric, unconstrained
numeric, and the decimal path), exported and read back with pyarrow / the DuckDB
CLI, and compared to the heap oracle. Gated like the existing export checks.
