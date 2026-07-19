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

## Building and testing

pgColumnar builds with PGXS against a PostgreSQL 17 installation:

    make PG_CONFIG=/path/to/pg_config
    make install PG_CONFIG=/path/to/pg_config

The `lz4` and `zstd` codecs are linked automatically when the system
development libraries (`liblz4`, `libzstd`) are found with `pkg-config`. When a
library is absent the codec is compiled out and a request for it falls back to a
codec that is present; `pglz` (built into PostgreSQL) and `none` are always
available.

Then, in a database:

    CREATE EXTENSION columnar;
    CREATE TABLE t (a int, b text) USING columnar;

The end-to-end tests build, install, spin up a throwaway cluster, and check
behavior. Phase 1 covers create/insert/scan/drop; phase 2 covers compression,
projection, min/max skip lists, and filtering; phase 3 covers delete, update,
read-your-writes, transaction and savepoint rollback, and temporary tables:

    test/smoke.sh  /path/to/pg_config
    test/phase2.sh /path/to/pg_config
    test/phase3.sh /path/to/pg_config

For full functionality (drop-time metadata cleanup, which runs from an object
access hook) add the library to `shared_preload_libraries = 'columnar'`, as is
standard for a table access method extension.

Phase 1 implements a format-2.0-compatible metapage and storage layer, the
stripe/chunk/chunk_group catalog, uncompressed bulk insert batched into chunk
groups and stripes, and a sequential scan.

Phase 2 adds value-stream compression with four codecs (`none`, `pglz`, `lz4`,
`zstd` with a compression level), independent per chunk, with a fall back to
uncompressed storage when compression does not shrink the data. The exists
(null) stream is never compressed. It computes and stores a per-chunk min/max
skip list for orderable types in `columnar.chunk`, adds a reader that
decompresses per column and projects only referenced columns, and adds the
chunk-group skipping machinery that uses the min/max skip list to skip groups
that cannot match pushed-down scan qualifiers. The compression, compression
level, chunk-group row limit, stripe row limit, and qual-pushdown GUCs from the
specification are honored.

Phase 3 adds update and delete without rewriting stripes. Deletes and the old
side of updates are marked in the `columnar.row_mask` table (spec 7.5); an
update inserts the new row with a fresh row number. Scans apply the row mask and
skip deleted rows. Metadata is read with a command-id-advanced snapshot so a
transaction sees its own inserts and deletes made earlier in the same
transaction (read-your-writes) while other transactions stay isolated. Pending
writes and delete marks are flushed at each statement end and at pre-commit, and
are discarded on transaction rollback and on savepoint (subtransaction) rollback,
with correct attribution so work done before a savepoint survives ROLLBACK TO.
Temporary columnar tables are supported.

Known limitations at this phase: a bulk UPDATE re-fetches each old row by item
pointer to fill unchanged columns (as the executor requires), which is O(rows x
stripe) and not yet optimized; two concurrent transactions deleting rows in the
same chunk group can conflict on the shared row-mask row. Automatic qualifier
pushdown for plain sequential scans, along with the custom scan path that
carries a projection and quals into the scan, arrives in phase 5 (planner
integration). Indexes and constraints arrive in phase 4 (see the plan).
