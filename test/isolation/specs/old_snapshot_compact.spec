# Phase F3 hazard H3, pinned deterministically: an old (REPEATABLE READ) snapshot
# must keep seeing rows through a concurrent online compaction that deletes and
# rewrites/retires their row group. Because visibility is heap MVCC on the metadata
# catalog and the data pages are append-only, the reader's snapshot still resolves
# the old row group and reads its pages, so its result is unchanged by the
# concurrent delete + compact_rewrite.

setup
{
	CREATE TABLE iso (id int, v int) USING pgcolumnar;
	SELECT pgcolumnar.alter_columnar_table_set('iso', stripe_row_limit => 1000);
	INSERT INTO iso SELECT g, g FROM generate_series(1, 50) g;
}

teardown { DROP TABLE iso; }

session reader
step r_begin  { BEGIN ISOLATION LEVEL REPEATABLE READ; }
step r_read1  { SELECT count(*) FROM iso; }
step r_read2  { SELECT count(*) FROM iso; }
step r_commit { COMMIT; }

session writer
step w_delete  { DELETE FROM iso WHERE id <= 10; }
step w_rewrite { SELECT pgcolumnar.compact_rewrite('iso', 0.0); }

# reader fixes its snapshot at 50 rows; the writer deletes 10 and rewrites+retires
# the group; the reader still sees all 50 rows under its old snapshot.
permutation "r_begin" "r_read1" "w_delete" "w_rewrite" "r_read2" "r_commit"
