/* pgColumnar 1.0 - format 2.0 metadata catalog and access method registration.
 *
 * This script matches section 7 (metadata catalog) and section 8 (SQL
 * interface) of design/FORMAT_AND_INTERFACE_SPEC.md. Column order and index
 * definitions are part of the on-disk/interoperable format.
 *
 * Phase 1 scope: stripe, chunk, chunk_group tables, the storageid_seq
 * sequence, the columnar_handler function, and the columnar access method.
 * Phase 3 adds the row_mask table and row_mask_seq for delete/update marking.
 * The remaining options table and management functions arrive in later phases.
 */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pgcolumnar" to load this file. \quit

/* ---------------------------------------------------------------------------
 * Sequences (spec 7.6)
 * ------------------------------------------------------------------------- */

CREATE SEQUENCE pgcolumnar.storageid_seq
	MINVALUE 10000000000
	NO CYCLE;

CREATE SEQUENCE pgcolumnar.row_mask_seq;

/* ---------------------------------------------------------------------------
 * pgcolumnar.stripe (spec 7.1)
 * ------------------------------------------------------------------------- */

CREATE TABLE pgcolumnar.stripe (
	storage_id bigint NOT NULL,
	stripe_num bigint NOT NULL,
	file_offset bigint NOT NULL,
	data_length bigint NOT NULL,
	column_count integer NOT NULL,
	chunk_row_count integer NOT NULL,
	row_count bigint NOT NULL,
	chunk_group_count integer NOT NULL,
	first_row_number bigint NOT NULL
);

CREATE UNIQUE INDEX stripe_pkey
	ON pgcolumnar.stripe USING btree (storage_id, stripe_num);

CREATE UNIQUE INDEX stripe_first_row_number_idx
	ON pgcolumnar.stripe USING btree (storage_id, first_row_number);

/* ---------------------------------------------------------------------------
 * pgcolumnar.chunk (spec 7.2)
 * ------------------------------------------------------------------------- */

CREATE TABLE pgcolumnar.chunk (
	storage_id bigint NOT NULL,
	stripe_num bigint NOT NULL,
	attr_num integer NOT NULL,
	chunk_group_num integer NOT NULL,
	minimum_value bytea,
	maximum_value bytea,
	value_stream_offset bigint NOT NULL,
	value_stream_length bigint NOT NULL,
	exists_stream_offset bigint NOT NULL,
	exists_stream_length bigint NOT NULL,
	value_compression_type integer NOT NULL,
	value_compression_level integer NOT NULL,
	value_decompressed_length bigint NOT NULL,
	value_count bigint NOT NULL,
	value_encoding_type integer,
	value_raw_length bigint,
	bloom_filter bytea
);

CREATE UNIQUE INDEX chunk_pkey
	ON pgcolumnar.chunk USING btree (storage_id, stripe_num, attr_num, chunk_group_num);

/* ---------------------------------------------------------------------------
 * pgcolumnar.chunk_group (spec 7.3)
 * ------------------------------------------------------------------------- */

CREATE TABLE pgcolumnar.chunk_group (
	storage_id bigint NOT NULL,
	stripe_num bigint NOT NULL,
	chunk_group_num integer NOT NULL,
	row_count bigint NOT NULL,
	deleted_rows bigint NOT NULL
);

CREATE UNIQUE INDEX chunk_group_pkey
	ON pgcolumnar.chunk_group USING btree (storage_id, stripe_num, chunk_group_num);

/* ---------------------------------------------------------------------------
 * pgcolumnar.row_mask (spec 7.5)
 *
 * Tracks deleted rows for updates and deletes without rewriting stripes. One
 * row per chunk group covers the row-number range [start_row_number,
 * end_row_number]; a set bit in "mask" marks a deleted row.
 * ------------------------------------------------------------------------- */

CREATE TABLE pgcolumnar.row_mask (
	id bigint NOT NULL,
	storage_id bigint NOT NULL,
	stripe_id bigint NOT NULL,
	chunk_id integer NOT NULL,
	start_row_number bigint NOT NULL,
	end_row_number bigint NOT NULL,
	deleted_rows integer NOT NULL,
	mask bytea
);

CREATE UNIQUE INDEX row_mask_pkey
	ON pgcolumnar.row_mask USING btree (id, storage_id, start_row_number, end_row_number);

CREATE UNIQUE INDEX row_mask_chunk_unique
	ON pgcolumnar.row_mask USING btree (storage_id, stripe_id, chunk_id, start_row_number);

CREATE UNIQUE INDEX row_mask_stripe_unique
	ON pgcolumnar.row_mask USING btree (storage_id, stripe_id, start_row_number);

/* ---------------------------------------------------------------------------
 * pgcolumnar.options (spec 7.4)
 *
 * Per-table overrides of the instance-wide compression, compression level,
 * chunk-group row limit, and stripe row limit. A NULL column means the table
 * uses the instance default (the GUC) for that option. Keyed by regclass.
 * ------------------------------------------------------------------------- */

CREATE TABLE pgcolumnar.options (
	regclass regclass NOT NULL,
	chunk_group_row_limit integer,
	stripe_row_limit integer,
	compression_level integer,
	compression name,
	format_version integer   -- NULL = 1.0-dev (2.2); 1 = native (PGCN v1)
);

CREATE UNIQUE INDEX options_pkey
	ON pgcolumnar.options USING btree (regclass);

/* ---------------------------------------------------------------------------
 * pgcolumnar.projection (gap 26, format 2.2)
 *
 * Multiple physical projections per table (C-Store). Each projection is a named,
 * ordered subset of the table's columns stored as its own columnar storage
 * (proj_storage_id) sorted on sort_key, sharing the row-number identity space.
 * projection_id 0 is the implicit base projection (all columns, insert order);
 * a table with no rows here has a single implicit base projection, so 2.0/2.1
 * tables and tables with no declared projections behave exactly as before.
 *
 * Phase 1 records the catalog and DDL only; no rows are written to a
 * projection's storage yet (write fan-out arrives in phase 2).
 * ------------------------------------------------------------------------- */

CREATE TABLE pgcolumnar.projection (
	storage_id bigint NOT NULL,       -- the table's base storage id
	projection_id integer NOT NULL,   -- 0 = base, 1..N additional
	name name NOT NULL,
	proj_storage_id bigint NOT NULL,  -- this projection's own storage id
	sort_key smallint[] NOT NULL,     -- attnums in sort order ({} = insert order)
	columns smallint[] NOT NULL       -- attnums stored (base = all live columns)
);

CREATE UNIQUE INDEX projection_pkey
	ON pgcolumnar.projection USING btree (storage_id, projection_id);

CREATE UNIQUE INDEX projection_name_idx
	ON pgcolumnar.projection USING btree (storage_id, name);

CREATE UNIQUE INDEX projection_storage_idx
	ON pgcolumnar.projection USING btree (proj_storage_id);

/* ---------------------------------------------------------------------------
 * Native format catalog (re-origination line, format PGCN v1).
 *
 * Additive scaffolding for the native on-disk format
 * (design/NATIVE_FORMAT_AND_INTERFACE_SPEC.md section 11). These tables are
 * empty until the native writer (Phase D2) populates them; the 2.2-line catalog
 * above is unaffected. Both format lines coexist per table until the native
 * format becomes the default (Phase D6). Dropped with the extension; per-table
 * row cleanup is wired into ColumnarDeleteMetadata when D2 begins writing them.
 * ------------------------------------------------------------------------- */

CREATE TABLE pgcolumnar.storage (
	storage_id bigint NOT NULL,       -- native relation storage id
	relation_oid oid NOT NULL,
	format_version integer NOT NULL,  -- native format major version (1)
	vector_length integer NOT NULL,   -- values per vector (1024)
	row_group_limit integer NOT NULL  -- max rows per row group
);
CREATE UNIQUE INDEX storage_pkey
	ON pgcolumnar.storage USING btree (storage_id);

CREATE TABLE pgcolumnar.row_group (
	storage_id bigint NOT NULL,
	group_number bigint NOT NULL,     -- 0-based row group ordinal
	file_offset bigint NOT NULL,      -- logical byte offset of the group
	row_count bigint NOT NULL,
	byte_length bigint NOT NULL,
	first_row_number bigint NOT NULL, -- row number of the group's first row
	sort_key smallint[] NOT NULL DEFAULT '{}'  -- attnums the group is sorted on
);
CREATE UNIQUE INDEX row_group_pkey
	ON pgcolumnar.row_group USING btree (storage_id, group_number);

CREATE TABLE pgcolumnar.column_chunk (
	storage_id bigint NOT NULL,
	group_number bigint NOT NULL,
	column_index smallint NOT NULL,   -- 0-based attribute position
	value_count bigint NOT NULL,
	encoding_descriptor bytea NOT NULL, -- the chosen cascade (Phase D4)
	block_codec smallint NOT NULL,    -- optional final block codec (0 = none)
	page_offset bigint NOT NULL,      -- logical byte offset of the chunk's page
	page_length bigint NOT NULL
);
CREATE UNIQUE INDEX column_chunk_pkey
	ON pgcolumnar.column_chunk USING btree (storage_id, group_number, column_index);

CREATE TABLE pgcolumnar.zone_map (
	storage_id bigint NOT NULL,
	group_number bigint NOT NULL,
	column_index smallint NOT NULL,
	vector_index integer NOT NULL,    -- -1 for the whole-chunk aggregate
	minimum bytea,                    -- encoded per the column type
	maximum bytea,
	sum numeric,                      -- NULL when the type has no sum
	value_count bigint NOT NULL,
	null_count bigint NOT NULL
);
CREATE UNIQUE INDEX zone_map_pkey
	ON pgcolumnar.zone_map USING btree (storage_id, group_number, column_index, vector_index);

-- Per-column-chunk bloom filter for equality skipping on hashable columns
-- (native spec 7.2). One row per (storage_id, group_number, column_index).
CREATE TABLE pgcolumnar.bloom (
	storage_id bigint NOT NULL,
	group_number bigint NOT NULL,
	column_index smallint NOT NULL,
	filter bytea NOT NULL
);
CREATE UNIQUE INDEX bloom_pkey
	ON pgcolumnar.bloom USING btree (storage_id, group_number, column_index);

/* ---------------------------------------------------------------------------
 * Access method (spec 8.1)
 * ------------------------------------------------------------------------- */

CREATE FUNCTION pgcolumnar.columnar_handler(internal)
	RETURNS table_am_handler
	LANGUAGE C
	AS 'MODULE_PATHNAME', 'columnar_handler';

CREATE ACCESS METHOD pgcolumnar
	TYPE TABLE
	HANDLER pgcolumnar.columnar_handler;

COMMENT ON ACCESS METHOD pgcolumnar IS 'pgColumnar column-oriented storage';

/* ---------------------------------------------------------------------------
 * Conversion between heap and columnar (spec 8.2)
 *
 * alter_table_set_access_method converts a table between heap and columnar by
 * driving PostgreSQL's own ALTER TABLE ... SET ACCESS METHOD, which rewrites
 * the table through the target access method (columnar's insert path when
 * converting to columnar, its scan path when converting away). Row counts and
 * values round-trip. "t" is a table name (optionally schema-qualified);
 * "method" is "pgcolumnar" or "heap" (or any other table access method).
 * ------------------------------------------------------------------------- */

CREATE FUNCTION pgcolumnar.alter_table_set_access_method(t text, method text)
	RETURNS void
	LANGUAGE plpgsql
	AS $alter_table_set_access_method$
DECLARE
	rel regclass := t::regclass;
	nsp text;
	tbl text;
	tmp text;
BEGIN
	/*
	 * PostgreSQL 15 introduced ALTER TABLE ... SET ACCESS METHOD, which
	 * rewrites the table in place through the target access method and
	 * preserves the relation's identity and dependents. Use it when available.
	 */
	IF current_setting('server_version_num')::int >= 150000 THEN
		EXECUTE format('ALTER TABLE %s SET ACCESS METHOD %I', rel::text, method);
		RETURN;
	END IF;

	/*
	 * PostgreSQL 13 and 14 have no ALTER TABLE ... SET ACCESS METHOD. Convert
	 * by building a sibling table that uses the target access method, copying
	 * every row through it, and swapping names. Column definitions, defaults,
	 * NOT NULL and CHECK constraints, and indexes are carried over
	 * (LIKE ... INCLUDING ALL). This does not preserve the original table's OID
	 * or objects that depend on it (views, foreign keys); on those majors that
	 * is a documented limitation of the conversion helper.
	 */
	SELECT n.nspname, c.relname INTO nsp, tbl
	  FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
	 WHERE c.oid = rel;
	tmp := tbl || '_pgcolumnar_conv';

	EXECUTE format('CREATE TABLE %I.%I (LIKE %I.%I INCLUDING ALL) USING %I',
				   nsp, tmp, nsp, tbl, method);
	EXECUTE format('INSERT INTO %I.%I SELECT * FROM %I.%I',
				   nsp, tmp, nsp, tbl);
	EXECUTE format('DROP TABLE %I.%I', nsp, tbl);
	EXECUTE format('ALTER TABLE %I.%I RENAME TO %I', nsp, tmp, tbl);
END;
$alter_table_set_access_method$;

COMMENT ON FUNCTION pgcolumnar.alter_table_set_access_method(text, text)
	IS 'convert a table between heap and columnar storage';

/* ---------------------------------------------------------------------------
 * Per-table option set and reset (spec 8.2)
 *
 * alter_columnar_table_set stores per-table option overrides; a NULL argument
 * leaves that option unchanged. alter_columnar_table_reset clears an option
 * back to the instance default when its boolean argument is true. Options take
 * effect for writes that begin after they are set.
 * ------------------------------------------------------------------------- */

CREATE FUNCTION pgcolumnar.alter_columnar_table_set(
	table_name regclass,
	chunk_group_row_limit int DEFAULT NULL,
	stripe_row_limit int DEFAULT NULL,
	compression name DEFAULT NULL,
	compression_level int DEFAULT NULL,
	format_version int DEFAULT NULL)
	RETURNS void
	LANGUAGE plpgsql
	AS $alter_columnar_table_set$
BEGIN
	IF compression IS NOT NULL AND
	   compression NOT IN ('none', 'pglz', 'lz4', 'zstd') THEN
		RAISE EXCEPTION 'unknown columnar compression "%"', compression;
	END IF;

	/*
	 * format_version selects the on-disk format for this table's later writes.
	 * NULL leaves it unchanged (the instance default, the 1.0-dev 2.2 format).
	 * 1 is the native format (PGCN v1). The write path honors this starting in
	 * the native writer phase; until then a value of 1 is recorded but writes
	 * continue in the 2.2 format.
	 */
	IF format_version IS NOT NULL AND format_version <> 1 THEN
		RAISE EXCEPTION 'unsupported format_version %, expected 1', format_version;
	END IF;

	/*
	 * Bound the integer limits to the same valid ranges as the instance-wide
	 * GUCs (pgcolumnar.chunk_group_row_limit, pgcolumnar.stripe_row_limit,
	 * pgcolumnar.compression_level). A per-table value outside these ranges is
	 * rejected here rather than stored: a limit of zero or below would produce
	 * a stripe whose recorded chunk_row_count is zero and make the row-number
	 * arithmetic (chunk id = offset / chunk_row_count) divide by zero on
	 * delete, update, and index fetch.
	 */
	IF chunk_group_row_limit IS NOT NULL AND chunk_group_row_limit < 100 THEN
		RAISE EXCEPTION 'chunk_group_row_limit must be at least 100';
	END IF;
	IF stripe_row_limit IS NOT NULL AND stripe_row_limit < 1000 THEN
		RAISE EXCEPTION 'stripe_row_limit must be at least 1000';
	END IF;
	IF compression_level IS NOT NULL AND
	   (compression_level < 1 OR compression_level > 22) THEN
		RAISE EXCEPTION 'compression_level must be between 1 and 22';
	END IF;

	INSERT INTO pgcolumnar.options AS o
		(regclass, chunk_group_row_limit, stripe_row_limit,
		 compression, compression_level, format_version)
	VALUES (table_name, chunk_group_row_limit, stripe_row_limit,
			compression, compression_level, format_version)
	ON CONFLICT (regclass) DO UPDATE SET
		chunk_group_row_limit =
			COALESCE(EXCLUDED.chunk_group_row_limit, o.chunk_group_row_limit),
		stripe_row_limit =
			COALESCE(EXCLUDED.stripe_row_limit, o.stripe_row_limit),
		compression =
			COALESCE(EXCLUDED.compression, o.compression),
		compression_level =
			COALESCE(EXCLUDED.compression_level, o.compression_level),
		format_version =
			COALESCE(EXCLUDED.format_version, o.format_version);
END;
$alter_columnar_table_set$;

COMMENT ON FUNCTION pgcolumnar.alter_columnar_table_set(regclass, int, int, name, int, int)
	IS 'set per-table columnar options; NULL leaves a value unchanged';

CREATE FUNCTION pgcolumnar.alter_columnar_table_reset(
	table_name regclass,
	chunk_group_row_limit bool DEFAULT false,
	stripe_row_limit bool DEFAULT false,
	compression bool DEFAULT false,
	compression_level bool DEFAULT false,
	format_version bool DEFAULT false)
	RETURNS void
	LANGUAGE plpgsql
	AS $alter_columnar_table_reset$
BEGIN
	UPDATE pgcolumnar.options o SET
		chunk_group_row_limit = CASE
			WHEN alter_columnar_table_reset.chunk_group_row_limit
			THEN NULL ELSE o.chunk_group_row_limit END,
		stripe_row_limit = CASE
			WHEN alter_columnar_table_reset.stripe_row_limit
			THEN NULL ELSE o.stripe_row_limit END,
		compression = CASE
			WHEN alter_columnar_table_reset.compression
			THEN NULL ELSE o.compression END,
		compression_level = CASE
			WHEN alter_columnar_table_reset.compression_level
			THEN NULL ELSE o.compression_level END,
		format_version = CASE
			WHEN alter_columnar_table_reset.format_version
			THEN NULL ELSE o.format_version END
	WHERE o.regclass = table_name;
END;
$alter_columnar_table_reset$;

COMMENT ON FUNCTION pgcolumnar.alter_columnar_table_reset(regclass, bool, bool, bool, bool, bool)
	IS 'reset per-table columnar options to the instance defaults';

/* ---------------------------------------------------------------------------
 * Storage-id lookup, statistics, and vacuum (spec 8.2)
 * ------------------------------------------------------------------------- */

CREATE FUNCTION pgcolumnar.get_storage_id(rel regclass)
	RETURNS bigint
	LANGUAGE C STABLE STRICT
	AS 'MODULE_PATHNAME', 'columnar_relation_storageid';

COMMENT ON FUNCTION pgcolumnar.get_storage_id(regclass)
	IS 'storage id linking a columnar table to its metadata rows';

CREATE FUNCTION pgcolumnar.add_projection(
	rel regclass,
	name text,
	columns text[],
	sort_key text[] DEFAULT '{}')
	RETURNS void
	LANGUAGE C
	AS 'MODULE_PATHNAME', 'columnar_add_projection';

COMMENT ON FUNCTION pgcolumnar.add_projection(regclass, text, text[], text[])
	IS 'declare a physical projection: a named column subset sorted on sort_key (gap 26)';

CREATE FUNCTION pgcolumnar.drop_projection(rel regclass, name text)
	RETURNS void
	LANGUAGE C
	AS 'MODULE_PATHNAME', 'columnar_drop_projection';

COMMENT ON FUNCTION pgcolumnar.drop_projection(regclass, text)
	IS 'drop a declared projection and free its storage (gap 26)';

CREATE FUNCTION pgcolumnar.read_projection(rel regclass, name text)
	RETURNS SETOF text
	LANGUAGE C STABLE
	AS 'MODULE_PATHNAME', 'columnar_read_projection';

COMMENT ON FUNCTION pgcolumnar.read_projection(regclass, text)
	IS 'read a projection''s stored columns (live rows), joined by | -- verification/debug (gap 26)';

CREATE FUNCTION pgcolumnar.reconstruct_via_projection(rel regclass, name text)
	RETURNS SETOF text
	LANGUAGE C STABLE
	AS 'MODULE_PATHNAME', 'columnar_reconstruct_via_projection';

COMMENT ON FUNCTION pgcolumnar.reconstruct_via_projection(regclass, text)
	IS 'read all live rows via a projection, reconstructing non-covered columns from the base by row number (gap 26)';

CREATE FUNCTION pgcolumnar.stats(
	rel regclass,
	OUT stripeid bigint,
	OUT fileoffset bigint,
	OUT rowcount bigint,
	OUT deletedrows bigint,
	OUT chunkcount integer,
	OUT datalength bigint)
	RETURNS SETOF record
	LANGUAGE sql STABLE
	AS $stats$
	-- Native (PGCN v1) tables report one row per row group from the native
	-- catalog; earlier-line tables report one row per stripe. A table populates
	-- exactly one of the two catalogs, so the union yields that table's rows.
	SELECT rg.group_number,
		   rg.file_offset,
		   rg.row_count,
		   COALESCE((SELECT sum(rm.deleted_rows)::bigint
					 FROM pgcolumnar.row_mask rm
					 WHERE rm.storage_id = rg.storage_id
					   AND rm.stripe_id = rg.group_number), 0::bigint),
		   (SELECT count(DISTINCT zm.vector_index)::int
			FROM pgcolumnar.zone_map zm
			WHERE zm.storage_id = rg.storage_id
			  AND zm.group_number = rg.group_number
			  AND zm.vector_index >= 0),
		   rg.byte_length
	FROM pgcolumnar.row_group rg
	WHERE rg.storage_id = pgcolumnar.get_storage_id(rel)
	UNION ALL
	SELECT s.stripe_num,
		   s.file_offset,
		   s.row_count,
		   COALESCE((SELECT sum(rm.deleted_rows)::bigint
					 FROM pgcolumnar.row_mask rm
					 WHERE rm.storage_id = s.storage_id
					   AND rm.stripe_id = s.stripe_num), 0::bigint),
		   s.chunk_group_count,
		   s.data_length
	FROM pgcolumnar.stripe s
	WHERE s.storage_id = pgcolumnar.get_storage_id(rel)
	ORDER BY 1;
$stats$;

COMMENT ON FUNCTION pgcolumnar.stats(regclass)
	IS 'per-row-group (native) or per-stripe statistics for a columnar table';

CREATE FUNCTION pgcolumnar.vacuum(tablename regclass, stripe_count int DEFAULT 0)
	RETURNS void
	LANGUAGE C STRICT
	AS 'MODULE_PATHNAME', 'columnar_vacuum';

COMMENT ON FUNCTION pgcolumnar.vacuum(regclass, int)
	IS 'compact a columnar table by combining stripes and reclaiming deleted rows';

CREATE FUNCTION pgcolumnar.vacuum_sorted(
	tablename regclass,
	VARIADIC sort_columns name[])
	RETURNS void
	LANGUAGE C
	AS 'MODULE_PATHNAME', 'columnar_vacuum_sorted';

COMMENT ON FUNCTION pgcolumnar.vacuum_sorted(regclass, name[])
	IS 'compact a columnar table, storing rows sorted ascending on the given columns';

CREATE FUNCTION pgcolumnar.export_arrow(rel regclass, path text)
	RETURNS bigint
	LANGUAGE C
	AS 'MODULE_PATHNAME', 'columnar_export_arrow';

COMMENT ON FUNCTION pgcolumnar.export_arrow(regclass, text)
	IS 'export a columnar table to an Arrow IPC stream file; returns rows written';

CREATE FUNCTION pgcolumnar.export_parquet(rel regclass, path text)
	RETURNS bigint
	LANGUAGE C
	AS 'MODULE_PATHNAME', 'columnar_export_parquet';

COMMENT ON FUNCTION pgcolumnar.export_parquet(regclass, text)
	IS 'export a columnar table to a Parquet file; returns rows written';

CREATE FUNCTION pgcolumnar.import_arrow(rel regclass, path text)
	RETURNS bigint
	LANGUAGE C
	AS 'MODULE_PATHNAME', 'columnar_import_arrow';

COMMENT ON FUNCTION pgcolumnar.import_arrow(regclass, text)
	IS 'insert rows from an Arrow IPC stream file into a columnar table; returns rows inserted';

CREATE FUNCTION pgcolumnar.import_parquet(rel regclass, path text)
	RETURNS bigint
	LANGUAGE C STRICT
	AS 'MODULE_PATHNAME', 'columnar_import_parquet';

COMMENT ON FUNCTION pgcolumnar.import_parquet(regclass, text)
	IS 'insert rows from a Parquet file into a table; returns rows inserted (gap 27)';

CREATE FUNCTION pgcolumnar.vm_selftest(rel regclass, blk int)
	RETURNS boolean
	LANGUAGE C
	AS 'MODULE_PATHNAME', 'columnar_vm_selftest';

COMMENT ON FUNCTION pgcolumnar.vm_selftest(regclass, int)
	IS 'gap 28 phase-1 self-test: set a VM-fork all-visible bit and read it back';

CREATE FUNCTION pgcolumnar.vm_is_visible(rel regclass, blk int)
	RETURNS boolean
	LANGUAGE C
	AS 'MODULE_PATHNAME', 'columnar_vm_is_visible';

COMMENT ON FUNCTION pgcolumnar.vm_is_visible(regclass, int)
	IS 'gap 28: is the synthetic block marked all-visible in the VM fork?';

CREATE FUNCTION pgcolumnar.vacuum_full(
	schema name DEFAULT 'public',
	sleep_time real DEFAULT 0.0,
	stripe_count int DEFAULT 0)
	RETURNS void
	LANGUAGE plpgsql
	AS $vacuum_full$
DECLARE
	r record;
BEGIN
	FOR r IN
		SELECT c.oid AS reloid
		FROM pg_class c
		JOIN pg_am a ON a.oid = c.relam
		JOIN pg_namespace n ON n.oid = c.relnamespace
		WHERE a.amname = 'pgcolumnar'
		  AND c.relkind = 'r'
		  AND n.nspname = vacuum_full.schema
	LOOP
		PERFORM pgcolumnar.vacuum(r.reloid::regclass, stripe_count);
		IF sleep_time > 0 THEN
			PERFORM pg_sleep(sleep_time);
		END IF;
	END LOOP;
END;
$vacuum_full$;

COMMENT ON FUNCTION pgcolumnar.vacuum_full(name, real, int)
	IS 'compact every columnar table in a schema';
