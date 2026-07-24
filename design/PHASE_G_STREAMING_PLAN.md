# Phase G: streaming Parquet reads

Plan written 2026-07-24, before any code. Replaces the read-the-whole-file model
in the external Parquet reader with a footer-plus-page model whose memory is
bounded by the largest single page rather than by the file.

## The problem

`pq_slurp_and_parse` (`src/columnar_parquet_reader.c`) opens the file, sizes it,
and does `palloc(filelen)` followed by one `fread` of the whole thing. Every read
surface goes through it: `read_parquet`, `import_parquet`, `parquet_schema`, and
the foreign-data wrapper.

Two consequences, and the first is worse than the documented limitation:

1. **A file of 1GB or more cannot be read at all.** `palloc` rejects a request
   above `MaxAllocSize` (1GB minus 1), so the read fails with `invalid memory
   alloc request size` before any Parquet logic runs. `docs/limitations.md`
   currently says each file is read fully into memory and there is no streaming,
   which understates it: this is a hard ceiling, not a memory-pressure warning.
2. Below that ceiling, one backend holds the entire file resident for the whole
   scan, and a multi-file directory read holds one file at a time on top of
   whatever the executor needs. A `count(*)` over a directory of 900MB files
   reads and holds every byte while decoding zero columns.

`parquet_schema` is the sharpest case: it slurps the entire file and then reads
nothing but the footer.

## What the current code already does well

The decode path is not the problem, and this plan does not touch its shape:

- `pq_read_rows` decodes per row group into `groupCtx`, which it resets each
  group, so decoded values are already bounded by one row group.
- Projection pushdown means an unreferenced column's chunk is never decoded.
- Predicate pushdown skips whole row groups before decoding.

What is unbounded is only the raw file image underneath those decisions. A
skipped row group is still read off disk today; after this change it is not,
which is a second win the pushdown work cannot currently deliver.

## Design

Replace the `(uint8 *filebuf, long filelen)` pair threaded through the reader
with a source handle, and read three kinds of things on demand.

### PqSource

```c
typedef struct PqSource
{
    FILE       *f;              /* AllocateFile handle, buffered */
    const char *path;           /* for error messages */
    int64       len;            /* file length */
    uint8      *meta;           /* footer metadata image, scan-lifetime */
    uint32      metalen;
} PqSource;
```

`pq_source_open` opens the file, sizes it, validates both `PAR1` magics by
reading the first and last 8 bytes, reads the 4-byte metadata length, bounds it
(`metalen + 8 <= len`, and `metalen <= MaxAllocSize`), reads exactly `metalen`
bytes into `meta`, and parses the footer from that buffer.

This matters for statistics lifetime. `PqChunk.stat_min` and `stat_max` point
into the parsed metadata, and `pqfdw_clause_excludes_group` dereferences them
long after the footer is parsed. Today they point into the whole-file buffer;
after this change they point into `meta`, which lives exactly as long. The
comment on those fields gets updated to say so.

`pq_source_close` frees the handle. The file stays open for the duration of one
file's scan, which is what the FDW already assumes per file.

### Page-at-a-time reads

`decode_leaf_entries` currently takes `(filebuf, filelen)` and walks absolute
file offsets from `dict_page_offset` or `data_page_offset`. It becomes a loop
over pages that pulls each page from the source:

1. Read a header window (start at 4KB, or less at end of file) at the current
   offset and run `parse_page_header` on it.
2. If the thrift parse ran out of input rather than failing on structure, grow
   the window (double it, capped) and retry. A page header that will not parse
   within the cap is a corrupt-file error, not an infinite loop.
3. Read `h.compressed_size` bytes at `offset + hdrlen` into a page buffer, then
   decompress and decode exactly as today.
4. Advance `offset` by `hdrlen + compressed_size`.

Deliberately **not** sizing the read from `total_compressed_size`. That field is
writer-supplied and would have to be trusted for a single large allocation; the
page loop needs only `compressed_size` per page, which `pq_decompress` already
bounds against `MaxAllocSize` and which we additionally bound against the bytes
remaining in the file. This keeps to the rule the earlier hardening work
established: a file-declared value gets a range check before it sizes anything.

Peak raw memory becomes one page header window plus one compressed page plus its
decompressed image, per column being decoded, rather than the file.

### Where each surface lands

- `parquet_schema`: footer only. No page reads at all.
- `read_parquet` and `import_parquet`: unchanged behaviour, streaming underneath.
- FDW: same, and a skipped row group now costs no I/O.

## Guards (each needs a test that fails without it)

The reader's bug history is one class: a file-declared value used without a range
check. The new ones this introduces:

- `metalen` bounded against both the file length and `MaxAllocSize`.
- Each page's `compressed_size` bounded against the bytes remaining after its
  header, before any allocation.
- A page header that does not parse within the window cap is an error.
- A chunk whose page loop reaches end of file before `num_values` entries is an
  error, not a short read that silently returns fewer rows. This is the same
  failure shape as issue #114: fewer rows with no error is the outcome to refuse.
- A `dict_page_offset` or `data_page_offset` outside the file is an error.

## Tests

New suite `test/native_parquet_streaming.sh`, plus additions to the existing
Parquet suites:

1. **Correctness is unchanged.** The existing `native_parquet_*` suites are the
   real regression net; they must pass untouched. That is the primary evidence.
2. **A file above the old ceiling reads.** Generate a Parquet file larger than
   `MaxAllocSize` (pyarrow, low-entropy but incompressible-enough columns), then
   `count(*)` and a filtered aggregate over it. Verified to FAIL on the pre-fix
   code with `invalid memory alloc request size`. This is the headline test and
   the one that proves the ceiling is gone.
3. **Memory does not scale with file size.** Compare peak backend memory (a
   `pg_backend_memory_contexts` sample, or RSS around the scan) for a small file
   against the large one. Assert the large-file scan stays within a bound rather
   than asserting an exact number.
4. **Skipped row groups cost no reads.** With a predicate that excludes all but
   one row group, assert the scan is faster than a full scan by a wide margin, or
   instrument a read counter behind the existing EXPLAIN counters. Prefer the
   counter: a timing assertion is flaky in a test suite.
5. **Crafted files** for each guard above, each verified to fail on the pre-guard
   code.

Test-size discipline: the large file is generated at suite runtime and removed
after, never committed. If generating it is too slow or the environment cannot
spare the disk, the suite skips with a note rather than silently passing.

## Sequencing

1. `PqSource` with footer-only parse; port `parquet_schema` to it. Small, and it
   proves the metadata lifetime change in isolation.
2. Page-at-a-time `decode_leaf_entries`, with the guards and their tests.
3. Port `read_parquet`, `import_parquet`, and the FDW; delete
   `pq_slurp_and_parse`.
4. The large-file test and the memory-bound test.
5. Docs: `limitations.md` loses the no-streaming limit and gains whatever bound
   is real; `features.md` and `ARCHITECTURE.md` describe the source model.

Each step is its own commit; the whole lands as one PR gated on PostgreSQL 18 and
19, with the full 15 through 19 matrix once at the end, per the cadence in
PHASE_G_FOLLOWON_HANDOFF.md.

## Open questions to settle during step 2

- Whether to keep a small readahead buffer in `PqSource` rather than relying on
  stdio buffering. Measure before adding: `AllocateFile` returns a buffered
  `FILE *`, and the access pattern within a chunk is sequential.
- Whether a row group whose chunks are decoded in column order should read them
  in file-offset order instead, to keep the access pattern sequential across
  columns. Only matters once projection selects a subset.
