<picture>
  <source media="(prefers-color-scheme: dark)" srcset="logo/pgcolumnar-logo-dark.svg">
  <img src="logo/pgcolumnar-logo.svg" alt="pgColumnar" width="340">
</picture>

# pgColumnar

Column-oriented storage for PostgreSQL, implemented as a table access method. A
table created `USING columnar` stores its data by column, with per-column
compression, chunk-group skipping, and a vectorized scan and aggregate path. It
is for analytic workloads: large scans, aggregates, and column projections over
append-mostly data.

pgColumnar builds from one source tree on PostgreSQL 13 through 19 and is
licensed under the [MIT License](LICENSE). It is pre-release; the current marker
is `1.0-dev` (on-disk format 2.2), recorded in `VERSION`.

## Documentation

| | |
| --- | --- |
| [Features](docs/features.md) | What pgColumnar provides |
| [Installation](docs/installation.md) | Build, load, and create the extension |
| [User guide](docs/user-guide.md) | Create tables, load data, and query |
| [Administration](docs/administration.md) | Operate columnar tables in production |
| [Configuration](docs/configuration.md) | Settings and per-table options |
| [SQL reference](docs/sql-reference.md) | The `columnar.*` functions |
| [Limitations](docs/limitations.md) | Compatibility and known constraints |
| [Benchmarks](docs/benchmarks.md) | Size and latency numbers |
| [Testing](docs/testing.md) | The test suite and version matrix |
| [Changelog](CHANGELOG.md) | Notable changes |
| [Architecture](docs/ARCHITECTURE.md) | Source map for developers |

## Quick start

Build with PGXS against the target server, add the library to
`shared_preload_libraries`, and restart:

```sh
make PG_CONFIG=/path/to/pg_config
make install PG_CONFIG=/path/to/pg_config
```

```
shared_preload_libraries = 'columnar'
```

Then, in a database:

```sql
CREATE EXTENSION columnar;

CREATE TABLE events (id bigint, ts timestamptz, kind int, payload text)
  USING columnar;

INSERT INTO events
  SELECT g, now(), g % 8, 'p' || g
  FROM generate_series(1, 1000000) g;

SELECT count(*), avg(kind) FROM events WHERE kind = 3;
```

See the [installation guide](docs/installation.md) for requirements and the
[user guide](docs/user-guide.md) for loading and querying.

## Independence

pgColumnar is an independent implementation. It is not derived from the source of
any other columnar project. It is built from a functional specification of the
on-disk format and SQL interface, recorded in
[design/FORMAT_AND_INTERFACE_SPEC.md](design/FORMAT_AND_INTERFACE_SPEC.md), by the
clean-room method described in [PROVENANCE.md](PROVENANCE.md).

## License

MIT. See [LICENSE](LICENSE).
