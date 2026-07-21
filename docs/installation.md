# Installation

pgColumnar builds with PGXS against an installed PostgreSQL server, versions 13
through 19.

## Requirements

- A PostgreSQL server and its development headers, reachable through `pg_config`.
- A C compiler and `make`.
- `pkg-config`. It is used to detect the optional compression libraries.
- Optional: `liblz4` and `libzstd` development libraries. When present, the `lz4`
  and `zstd` codecs are compiled in. When absent, those codecs are compiled out
  and a request for them falls back to a codec that is present.
- A little-endian host is required for the Arrow and Parquet import and export
  functions. The rest of the extension runs on any host PostgreSQL supports.

## Build and install

Point the build at the `pg_config` of the target server:

```sh
make PG_CONFIG=/path/to/pg_config
make install PG_CONFIG=/path/to/pg_config
```

`make install` copies `pgcolumnar.so`, the control file, and the SQL script into
the server's library and extension directories.

## Load the library

pgColumnar installs planner and executor hooks at library load time. Add it to
`shared_preload_libraries` and restart the server:

```
shared_preload_libraries = 'pgcolumnar'
```

Set this in `postgresql.conf` (or with `ALTER SYSTEM SET shared_preload_libraries
= 'pgcolumnar'`), then restart. If other libraries are already preloaded, add
`pgcolumnar` to the comma-separated list.

## Create the extension

In each database that will hold columnar tables:

```sql
CREATE EXTENSION pgcolumnar;
```

This creates the `pgcolumnar` schema, the `pgcolumnar` table access method, the
catalog tables, and the `columnar.*` functions. The extension is not
relocatable; its objects stay in the `pgcolumnar` schema.

## Verify

```sql
-- the access method is registered
SELECT amname FROM pg_am WHERE amname = 'pgcolumnar';

-- a columnar table can be created and read
CREATE TABLE install_check (id int, v text) USING pgcolumnar;
INSERT INTO install_check VALUES (1, 'ok');
SELECT * FROM install_check;
DROP TABLE install_check;
```

## Upgrade

To install a new build of the extension:

1. Run `make install` with the same `PG_CONFIG`.
2. Restart the server so the new library is loaded.

The on-disk format version is recorded in the source and in
[../design/FORMAT_AND_INTERFACE_SPEC.md](../design/FORMAT_AND_INTERFACE_SPEC.md).
A build that keeps the same format version reads tables written by earlier builds
of that version without conversion.

## Remove

Drop the extension from a database, then remove the files if no database still
uses it:

```sql
DROP EXTENSION pgcolumnar;   -- fails if columnar tables still exist; drop them first
```

Removing `pgcolumnar` from `shared_preload_libraries` and restarting unloads the
library. Do this only after no database contains columnar tables, because reading
a columnar table requires the access method to be present.
