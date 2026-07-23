# Configuration reference

pgColumnar has two kinds of settings:

- Server settings under the `pgcolumnar.` prefix, listed below. Each can be set in
  `postgresql.conf`, per session with `SET`, per role, or per database. All of
  them are `USERSET`, so a session may change them without special privileges.
- Per-table storage options, set with `pgcolumnar.alter_columnar_table_set`. These
  apply to one table and are used when that table writes new data.

## Server settings

### Storage layout

| Setting | Type | Default | Description |
| --- | --- | --- | --- |
| `pgcolumnar.stripe_row_limit` | integer | `150000` | Maximum rows per row group. The row group is the unit of write and the granularity at which whole segments are appended. Range 1000 to INT_MAX. |
| `pgcolumnar.chunk_group_row_limit` | integer | `10000` | Maximum rows per vector. The vector is the unit of encoding and of min/max skipping. Range 100 to INT_MAX. |

### Compression

| Setting | Type | Default | Description |
| --- | --- | --- | --- |
| `pgcolumnar.compression` | enum | `zstd` | Default codec for new chunks. One of `none`, `pglz`, `lz4`, `zstd`. `lz4` and `zstd` are available only when the extension was built with those libraries. |
| `pgcolumnar.compression_level` | integer | `3` | Level for the `zstd` codec. Range 1 to 22. Higher levels compress more and write more slowly. |

### Scan and execution

| Setting | Type | Default | Description |
| --- | --- | --- | --- |
| `pgcolumnar.enable_custom_scan` | boolean | `on` | Use the columnar custom scan path for columnar tables. |
| `pgcolumnar.enable_qual_pushdown` | boolean | `on` | Push scan qualifiers down so per-chunk min and max values can skip chunk groups. |
| `pgcolumnar.enable_vectorization` | boolean | `on` | Use the vectorized aggregate path for supported ungrouped aggregates. |
| `pgcolumnar.enable_bloom_filter` | boolean | `on` | Skip chunk groups on equality filters using per-chunk bloom filters. |
| `pgcolumnar.enable_metadata_count` | boolean | `on` | Answer `count(*)` from catalog metadata without scanning the table. |
| `pgcolumnar.enable_read_stream` | boolean | `on` | Prefetch block reads with the read stream API. Effective on PostgreSQL 17 and later. |

### Index-only scan and projections

| Setting | Type | Default | Description |
| --- | --- | --- | --- |
| `pgcolumnar.enable_index_only_scan` | boolean | `on` | Allow index-only scans on columnar tables, served by the columnar visibility-map fork. Set to `off` to force a plain index scan. |
| `pgcolumnar.enable_projection_scan` | boolean | `on` | Let the planner scan a covering projection instead of the base table when one serves the query better. |

### Column cache

| Setting | Type | Default | Description |
| --- | --- | --- | --- |
| `pgcolumnar.enable_column_cache` | boolean | `off` | Cache decompressed chunk groups so they can be reused across reads. |
| `pgcolumnar.column_cache_size` | integer (MB) | `200` | Size of the decompressed-chunk cache. Applies when the column cache is enabled. Range 1 to INT_MAX. |

### Maintenance and disk reclaim

| Setting | Type | Default | Description |
| --- | --- | --- | --- |
| `pgcolumnar.reclaim_coalesce` | boolean | `on` | During online compaction, split an oversized freed range on reuse and coalesce adjacent freed ranges, so space is reclaimed under fragmentation. Off reverts to whole-range reuse. |
| `pgcolumnar.enable_end_truncation` | boolean | `off` | Allow `pgcolumnar.truncate()` to return trailing reclaimed blocks to the operating system. Off makes `pgcolumnar.truncate()` a no-op. Requires superuser to set. |

### Concurrent unique inserts

| Setting | Type | Default | Description |
| --- | --- | --- | --- |
| `pgcolumnar.enable_unique_insert_lock` | boolean | `on` | Serialize concurrent inserts of the same unique-index key with a transaction-scoped advisory lock, so overlapping same-key inserts conflict correctly. |
| `pgcolumnar.unique_lock_buckets` | integer | `128` | Advisory-lock buckets per unique index. Bounds how many advisory locks a transaction holds per unique index. Equal keys always share a bucket; unrelated keys may share one, which only over-serializes. Range 1 to 1048576. |

## Per-table storage options

`pgcolumnar.alter_columnar_table_set` sets storage options on one table. New data
written after the change uses the new values; data already written is unchanged
until the table is rewritten (for example by `pgcolumnar.vacuum`).

```sql
SELECT pgcolumnar.alter_columnar_table_set(
    'events',
    chunk_group_row_limit => 20000,
    stripe_row_limit      => 300000,
    compression           => 'zstd',
    compression_level     => 6);
```

| Argument | Type | Description |
| --- | --- | --- |
| `table_name` | regclass | The columnar table to change. |
| `chunk_group_row_limit` | integer | Per-table override of `pgcolumnar.chunk_group_row_limit`. |
| `stripe_row_limit` | integer | Per-table override of `pgcolumnar.stripe_row_limit`. |
| `compression` | name | One of `none`, `pglz`, `lz4`, `zstd`. |
| `compression_level` | integer | Level for the `zstd` codec, 1 to 22. |

Arguments left at their default (`NULL`) are not changed. A value outside the
valid range for a limit or level is rejected.

`pgcolumnar.alter_columnar_table_reset` returns options to the server defaults:

```sql
SELECT pgcolumnar.alter_columnar_table_reset(
    'events',
    chunk_group_row_limit => true,
    compression           => true);
```

Each boolean argument, when true, resets that option on the table.
