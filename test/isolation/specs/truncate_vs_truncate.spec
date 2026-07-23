# Two concurrent end-truncations must serialize, not corrupt. Both take
# ShareUpdateExclusiveLock (like the other lazy maintenance ops) for the duration
# of their transaction, so the second blocks until the first commits. The trailing
# group is freed first (o_prep compacts it, in its own transaction). The first
# truncation reclaims it (reclaimed = t); the second, once unblocked, finds nothing
# left to do (reclaimed = f). No overlap, no double-free.

setup
{
	CREATE TABLE iso (id int, v text) USING pgcolumnar;
	SELECT pgcolumnar.alter_columnar_table_set('iso', stripe_row_limit => 1000, chunk_group_row_limit => 1000);
	INSERT INTO iso SELECT g, md5(g::text) FROM generate_series(1, 3000) g;
	DELETE FROM iso WHERE id > 2000;
}

teardown { DROP TABLE iso; }

session one
step o_prep   { SELECT pgcolumnar.compact('iso'); }
step o_begin  { BEGIN; }
step o_trunc  { SELECT (pgcolumnar.truncate('iso') > 0) AS reclaimed; }
step o_commit { COMMIT; }

session two
step t_trunc  { SELECT (pgcolumnar.truncate('iso') > 0) AS reclaimed; }

permutation o_prep o_begin o_trunc t_trunc o_commit
