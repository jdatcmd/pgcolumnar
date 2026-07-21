# Testing

The test suite builds and installs the extension, starts a throwaway cluster,
exercises the access method, and checks results. Each script takes a `pg_config`
and is self-contained:

```sh
test/smoke.sh   /path/to/pg_config   # create, insert, scan, drop
test/phase2.sh  /path/to/pg_config   # compression, projection, min/max skip, filter
test/phase3.sh  /path/to/pg_config   # delete, update, MVCC, savepoints, temp tables
test/phase4.sh  /path/to/pg_config   # btree/hash indexes, constraints, conversion
test/phase5.sh  /path/to/pg_config   # custom scan, pushdown, options, vacuum
test/phase6.sh  /path/to/pg_config   # vectorized scan and aggregates, column cache
test/audit.sh   /path/to/pg_config   # regression tests for audited defects
test/concurrency.sh      /path/to/pg_config  # concurrent same-chunk-group deletes
test/unique_conc.sh      /path/to/pg_config  # concurrent same-unique-key inserts
test/differential.sh     /path/to/pg_config  # heap-vs-columnar oracle
test/recovery.sh         /path/to/pg_config  # crash recovery and atomicity
test/fuzz.sh             /path/to/pg_config  # seeded randomized differential
test/hardening.sh        /path/to/pg_config  # format 2.0 compat and corrupt-input robustness
test/concurrent_diff.sh  /path/to/pg_config  # concurrent DML vs a heap oracle
test/parallel.sh         /path/to/pg_config  # parallel scan plan and results vs a heap oracle
test/sorted_projection.sh /path/to/pg_config # columnar.vacuum_sorted results and skipping
test/index_only.sh       /path/to/pg_config  # index-only scan and the visibility-map fork
test/projections.sh      /path/to/pg_config  # multiple projections and projection scan
test/arrow_export.sh     /path/to/pg_config  # Arrow IPC export read back with pyarrow
test/parquet_export.sh   /path/to/pg_config  # Parquet export read back with pyarrow and DuckDB
test/arrow_import.sh     /path/to/pg_config  # Arrow IPC import
test/parquet_import.sh   /path/to/pg_config  # Parquet import
test/arrow_nested.sh     /path/to/pg_config  # nested Arrow export
test/parquet_nested.sh   /path/to/pg_config  # nested Parquet export
test/arrow_nested_import.sh   /path/to/pg_config  # nested Arrow import
test/parquet_nested_import.sh /path/to/pg_config  # nested Parquet import
```

## Differential oracle

`test/differential.sh`, `recovery`, `fuzz`, `hardening`, and `concurrent_diff`
share `test/lib.sh`, a heap-versus-columnar differential oracle: a query runs
against a heap mirror and the columnar table, and the results are compared as an
order-independent result-set hash, so heap is the correctness oracle.

`test/pbt/run.sh` is a separate, PostgreSQL-independent C property test of the
value-stream codecs (round-trip over randomized and boundary inputs):

```sh
test/pbt/run.sh [seed] [iterations]
```

## The version matrix

To build and run every suite across a set of PostgreSQL majors in one pass, each
in its own fresh build directory, pass their `pg_config` paths to the matrix
helper. With no arguments it uses PostgreSQL 13 through 19:

```sh
test/run_all_versions.sh /usr/local/pg13/bin/pg_config ... /usr/local/pg19/bin/pg_config
```

All suites pass on PostgreSQL 13 through 19. PostgreSQL 19 is validated against
19beta2; revalidation against the final PostgreSQL 19 release is pending that
release.
