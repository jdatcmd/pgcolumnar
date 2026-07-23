# Phase F3c hazard, pinned deterministically: a DELETE whose snapshot predates a
# concurrent online recluster of its group must be aborted with a serialization
# failure, never lost. Same conflict protocol as delete_vs_rewrite, exercised
# through pgcolumnar.recluster (which retires and rewrites every group). s1 fixes a
# REPEATABLE READ snapshot that still sees the old group, s2 reclusters (retiring
# it), then s1 deletes a row it still sees; s1's delete flush detects the
# retirement and raises a serialization failure. s1 would retry against the new
# group; the abort is the point.

setup
{
	CREATE TABLE iso (id int, v int) USING pgcolumnar;
	SELECT pgcolumnar.alter_columnar_table_set('iso', stripe_row_limit => 1000);
	INSERT INTO iso SELECT g, g FROM generate_series(1, 50) g;
}

teardown { DROP TABLE iso; }

session s1
step s1_begin  { BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s1_touch  { SELECT count(*) FROM iso; }
step s1_delete { DELETE FROM iso WHERE id = 25; }
step s1_commit { COMMIT; }

session s2
step s2_recluster { SELECT pgcolumnar.recluster('iso', 'id'); }

permutation "s1_begin" "s1_touch" "s2_recluster" "s1_delete" "s1_commit"
