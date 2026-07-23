# Physical end-truncation, corruption-critical hazard: a reader holding an OLD
# snapshot must never have data it can still resolve truncated away.
#
# The delete commits FIRST; then the reader pins its snapshot (so it sees the rows
# deleted, but still sees the row group as live -- the compaction that retires it
# has not happened yet). The writer then compacts (which CAN retire now, since the
# delete precedes the reader's snapshot) and tries to truncate. Because the reader
# still resolves that group, the oldest-xmin horizon has not passed the retirement,
# so truncate is a no-op (reclaimed = f). The reader reads correctly throughout.
# Only after the reader commits does truncate reclaim the space (reclaimed = t).

setup
{
	CREATE TABLE iso (id int, v text) USING pgcolumnar;
	SELECT pgcolumnar.alter_columnar_table_set('iso', stripe_row_limit => 1000, chunk_group_row_limit => 1000);
	INSERT INTO iso SELECT g, md5(g::text) FROM generate_series(1, 3000) g;
}

teardown { DROP TABLE iso; }

session writer
step w_delete { SET pgcolumnar.enable_end_truncation = on; DELETE FROM iso WHERE id > 2000; }
step w_compact { SELECT pgcolumnar.compact('iso'); }
step w_trunc_held { SELECT (pgcolumnar.truncate('iso') > 0) AS reclaimed; }
step w_trunc_free { SELECT (pgcolumnar.truncate('iso') > 0) AS reclaimed; }

session reader
step r_begin  { BEGIN ISOLATION LEVEL REPEATABLE READ; }
step r_read1  { SELECT count(*) FROM iso; }
step r_read2  { SELECT count(*) FROM iso; }
step r_commit { COMMIT; }

permutation w_delete r_begin r_read1 w_compact w_trunc_held r_read2 r_commit w_trunc_free
