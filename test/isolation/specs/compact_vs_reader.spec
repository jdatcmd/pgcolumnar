# Phase F3a gate, pinned deterministically: pgcolumnar.compact() must NOT retire a
# fully-deleted row group while an older snapshot can still see its rows. A
# REPEATABLE READ reader fixes a snapshot, then a writer deletes every row and
# runs compact(); because the reader's snapshot precedes the delete, its
# oldest-xmin horizon holds the group back, so compact retires 0 groups and the
# reader still sees all its rows. (Once the reader ends, a later compact could
# retire the group; that horizon advance is not exercised here.)

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
step w_delete_all { DELETE FROM iso; }
step w_compact    { SELECT pgcolumnar.compact('iso'); }

# reader pins its snapshot at 50 rows; the writer deletes all and compacts, but
# compact retires 0 (the reader's horizon holds the group), and the reader still
# sees all 50 rows.
permutation "r_begin" "r_read1" "w_delete_all" "w_compact" "r_read2" "r_commit"
