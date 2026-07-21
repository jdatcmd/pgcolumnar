# Limitations and compatibility

## PostgreSQL versions

pgColumnar builds from one source tree on PostgreSQL 13, 14, 15, 16, 17, 18, and
19. Every test suite runs on all seven majors.

Two behaviors depend on the major:

- `ALTER TABLE ... SET ACCESS METHOD` exists on PostgreSQL 15 and later. On 13 and
  14, `columnar.alter_table_set_access_method` rebuilds the table instead and does
  not preserve its OID or dependent objects (views, foreign keys). See the
  [SQL reference](sql-reference.md#columnaralter_table_set_access_methodt-text-method-text).
- The read stream prefetch path (`columnar.enable_read_stream`) is effective on
  PostgreSQL 17 and later. On earlier majors the setting has no effect.

## Host architecture

The Arrow and Parquet import and export functions run on little-endian hosts
only. The rest of the extension runs on any architecture PostgreSQL supports.

## Workload

Columnar storage is built for append-mostly data. Updates and deletes are
supported, but they mark rows rather than rewriting data, and the space is
reclaimed only by [`columnar.vacuum`](sql-reference.md#columnarvacuumtablename-regclass-stripe_count-int-default-0).
For high-rate single-row updates and deletes, and for workloads that read whole
rows by key, the default heap is the better fit.

## Replication and backup

- Physical replication and physical backups (`pg_basebackup`, snapshots) include
  columnar tables, which are WAL-logged relations.
- `pg_dump` and `pg_restore` handle columnar tables. The target server must have
  the extension installed and preloaded.
- Logical decoding reads heap-tuple WAL records. Changes to columnar tables are
  not emitted through logical decoding, so logical replication does not carry
  them. Use physical replication for columnar tables.

## Import and export type coverage

The import and export functions require superuser and run on little-endian hosts.
They support one-dimensional arrays and composite types built from the scalar
types below, with nulls at every level. Multi-dimensional arrays and types not
listed are rejected.

| Type | Arrow export | Parquet export | Arrow import | Parquet import |
| --- | --- | --- | --- | --- |
| `int2`, `int4`, `int8` | yes | yes | yes | yes |
| `float4`, `float8` | yes | yes | yes | yes |
| `bool` | yes | yes | yes | yes |
| `text`, `varchar` | yes | yes | yes | yes |
| `bytea` | yes | yes | yes | yes |
| `date`, `time`, `timestamp`, `timestamptz` | yes | yes | yes | yes |
| `uuid` | yes | yes | yes | no |
| `numeric` | yes | yes | yes | no |
| `json`, `jsonb` | yes | yes | yes | no |
| one-dimensional array of the above | yes | yes | yes | yes |
| composite of the above | yes | yes | yes | yes |

`uuid`, `numeric`, and `json` can be exported to Parquet and read back with other
tools, but pgColumnar does not currently import those three types from Parquet.
They are supported end to end through Arrow.

## Compression codecs

`lz4` and `zstd` are available only when the extension was built with the
corresponding system libraries. When a codec is not built in, a request for it
falls back to a codec that is present. `pglz` and `none` are always available.
See [Installation](installation.md#requirements).
