# Phase F3b hazard H1, pinned deterministically: a DELETE whose snapshot predates
# an online rewrite (pgcolumnar.compact_rewrite) of its row group must be aborted
# with a serialization failure by the conflict protocol, never lost and never
# allowed to resurrect a row. s1 fixes a REPEATABLE READ snapshot that still sees
# the old group, s2 rewrites and retires that group into a new one, then s1 deletes
# a row it still sees in the old group; s1's commit flushes the mark and the
# conflict check detects the retirement and raises a serialization failure. s1
# would retry (re-resolving the row at its new location); the abort is the point.

setup
{
	CREATE TABLE iso (id int, v int) USING pgcolumnar;
	SELECT pgcolumnar.alter_columnar_table_set('iso', stripe_row_limit => 1000);
	INSERT INTO iso SELECT g, g FROM generate_series(1, 50) g;
	-- one pre-existing committed delete makes the group a rewrite candidate
	DELETE FROM iso WHERE id = 1;
}

teardown { DROP TABLE iso; }

session s1
step s1_begin  { BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s1_touch  { SELECT count(*) FROM iso; }
step s1_delete { DELETE FROM iso WHERE id = 25; }
step s1_commit { COMMIT; }

session s2
step s2_rewrite { SELECT pgcolumnar.compact_rewrite('iso', 0.0); }

# s1 fixes its snapshot (still sees the old group), s2 rewrites+retires the group,
# s1 deletes a row it still sees, and s1's commit must fail with a serialization
# error because the group it targeted was compacted away.
permutation "s1_begin" "s1_touch" "s2_rewrite" "s1_delete" "s1_commit"
