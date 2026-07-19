# pgColumnar

A column-oriented storage extension for PostgreSQL, implemented as a table
access method. pgColumnar is an independent implementation. It is not derived
from the source of any other columnar project. It is built from a functional
specification of the on-disk format and SQL interface, recorded in
[design/FORMAT_AND_INTERFACE_SPEC.md](design/FORMAT_AND_INTERFACE_SPEC.md).

Licensed under the MIT License. See [LICENSE](LICENSE).

Status: early construction. See [design/REWRITE_PLAN.md](design/REWRITE_PLAN.md)
for the delivery phases and task list, and [PROVENANCE.md](PROVENANCE.md) for how
the implementation stays independent.

## Building and testing (phase 1)

pgColumnar builds with PGXS against a PostgreSQL 17 installation:

    make PG_CONFIG=/path/to/pg_config
    make install PG_CONFIG=/path/to/pg_config

Then, in a database:

    CREATE EXTENSION columnar;
    CREATE TABLE t (a int, b text) USING columnar;

The end-to-end smoke test builds, installs, spins up a throwaway cluster, and
checks create/insert/scan/drop:

    test/smoke.sh /path/to/pg_config

For full functionality (drop-time metadata cleanup, which runs from an object
access hook) add the library to `shared_preload_libraries = 'columnar'`, as is
standard for a table access method extension.

Phase 1 implements a format-2.0-compatible metapage and storage layer, the
stripe/chunk/chunk_group catalog, uncompressed bulk insert batched into chunk
groups and stripes, and a sequential scan. Compression, projection, chunk-group
skipping, update/delete, indexes, and full transaction/savepoint visibility
arrive in later phases (see the plan).
