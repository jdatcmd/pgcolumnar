# Benchmarks

`bench/run_bench.sh` builds and installs the extension into a throwaway cluster,
loads one identical dataset into a heap table and into columnar tables, and
reports on-disk size, query latency, and import and export throughput. Run it
against a non-assert build:

```sh
bench/run_bench.sh /path/to/pg_config
```

Environment variables: `BENCH_SCALE` (rows, default 6000000), `BENCH_REPS`
(timed repetitions, median reported, default 5), `BENCH_PORT`, and `BENCH_DUCKDB`
(set to 1 to add a DuckDB comparison when `duckdb` is on `PATH`).

The numbers below are one run on one machine: PostgreSQL 17.10 (non-assert),
6,000,000 rows, an 8-column table, median of 5 timed runs after a warm-up. They
show the shape of the tradeoff, not a precise score. Regenerate them with the
command above.

## Storage

Total relation size, including indexes:

| table | size |
| --- | --- |
| heap | 707 MB |
| columnar (zstd) | 176 MB |
| columnar (none) | 81 MB |

Table-only size, excluding indexes:

| table | size | note |
| --- | --- | --- |
| heap | 579 MB | |
| columnar (none) | 81 MB | encodings, no block codec |
| columnar (zstd) | 48 MB | encodings plus zstd |

The `columnar (none)` line has no block compression, so 81 MB against heap's
579 MB is the encoding layer alone. zstd on top brings the table to 48 MB, about
12 times smaller than the heap table.

## Query latency

Heap versus columnar (zstd), median milliseconds:

| query | heap | columnar | heap / columnar |
| --- | --- | --- | --- |
| count(*) full table | 199.74 | 0.03 | 6658 |
| sum/avg over one int column | 290.65 | 142.34 | 2.04 |
| filtered agg, min/max-skippable range | 210.62 | 89.33 | 2.36 |
| point lookup by indexed id | 0.01 | 3.05 | 0.00 |
| projection: 3 of 8 cols, 1% filter | 212.87 | 48.00 | 4.43 |

`count(*)` over the whole table is answered from catalog metadata and does not
scan. The point lookup is the one shape where heap wins.

## Feature toggles

Vectorization on versus off (columnar zstd, median ms):

| query | on | off | speedup |
| --- | --- | --- | --- |
| sum/avg over int | 141.32 | 353.10 | 2.50 |
| filtered agg (range) | 41.64 | 48.30 | 1.16 |

Index-only scan on versus off (covering range count, median ms):

| query | on | off | speedup |
| --- | --- | --- | --- |
| covering count, id range (~2%) | 5.15 | 58589.72 | 11377 |

Projection scan on versus off (covering scan on a scattered sort key, median ms):

| query | on | off | speedup |
| --- | --- | --- | --- |
| sortk, val where sortk in ~0.1% range | 43.61 | 144.57 | 3.32 |

Sorted storage (`pgcolumnar.vacuum_sorted`), narrow range scan on a key not
correlated with insert order, median ms:

| state | ms |
| --- | --- |
| before vacuum_sorted | 141.39 |
| after vacuum_sorted | 45.91 |

Compression none versus zstd (columnar table-only): 81 MB versus 48 MB, with scan
latency essentially unchanged, because the encoded stream is already small.

## Import and export

Export, 6,000,000 rows, 5 columns:

| format | ms | file size | M rows/s |
| --- | --- | --- | --- |
| arrow | 731.5 | 186 MB | 8.2 |
| parquet | 784.2 | 186 MB | 7.7 |

Import, 6,000,000 rows, 5 columns:

| format | ms |
| --- | --- |
| arrow | 10989 |
| parquet | 11055 |

Nested round-trip, 1,000,000 rows, one `int[3]` array column and one composite
column:

| format | export ms | import ms | file size |
| --- | --- | --- | --- |
| arrow | 228.7 | 888.6 | 38 MB |
| parquet | 211.8 | 973.1 | 35 MB |

The reconstructed tables matched the source (zero differing rows) for both
formats.

## Cross-engine read

Reading the Parquet file pgColumnar wrote, 6,000,000 rows, count and sum:

| reader | time |
| --- | --- |
| DuckDB `read_parquet` (count and sum, stats-accelerated) | 7 ms |
| pyarrow `read_table` (full materialization) | 91 ms |

These confirm the Parquet output is read by other engines without conversion.

## Reading the results

Columnar wins on analytic shapes: `count(*)` from metadata, aggregates, filtered
aggregates the minimum and maximum and bloom skipping can prune, wide-table
projections, and index-only covering scans. The size reduction comes mostly from
the encoding layer before zstd. Vectorization adds a further speedup on
aggregates, and storing a table sorted on its range key improves skipping. Heap
wins on a single-row index fetch. Columnar is the wrong choice for write-heavy
OLTP and the right choice for scan-heavy and aggregate-heavy analytics over wide,
append-mostly tables.
