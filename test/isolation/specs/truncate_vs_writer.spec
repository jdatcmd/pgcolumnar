# Physical end-truncation is best-effort: it takes AccessExclusiveLock only
# CONDITIONALLY for the brief physical step, so it must never block on a
# concurrent writer. After the trailing group is freed (t_prep compacts it, in its
# own transaction so the setup delete is now old enough to retire), a writer holds
# an uncommitted DELETE -- RowExclusiveLock, no data appended, so the freed space
# stays trailing. truncate cannot get its exclusive lock and yields immediately
# (reclaimed = f), without waiting. After the writer commits, truncate reclaims
# (reclaimed = t).

setup
{
	CREATE TABLE iso (id int, v text) USING pgcolumnar;
	SELECT pgcolumnar.alter_columnar_table_set('iso', stripe_row_limit => 1000, chunk_group_row_limit => 1000);
	INSERT INTO iso SELECT g, md5(g::text) FROM generate_series(1, 3000) g;
	DELETE FROM iso WHERE id > 2000;
}

teardown { DROP TABLE iso; }

session writer
step w_begin  { BEGIN; }
step w_delete { DELETE FROM iso WHERE id = 1; }
step w_commit { COMMIT; }

session trunc
step t_prep { SELECT pgcolumnar.compact('iso'); }
step t_held { SELECT (pgcolumnar.truncate('iso') > 0) AS reclaimed; }
step t_free { SELECT (pgcolumnar.truncate('iso') > 0) AS reclaimed; }

permutation t_prep w_begin w_delete t_held w_commit t_free
