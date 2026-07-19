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
\echo Use "CREATE EXTENSION columnar" to load this file. \quit

/* ---------------------------------------------------------------------------
 * Sequences (spec 7.6)
 * ------------------------------------------------------------------------- */

CREATE SEQUENCE columnar.storageid_seq
	MINVALUE 10000000000
	NO CYCLE;

CREATE SEQUENCE columnar.row_mask_seq;

/* ---------------------------------------------------------------------------
 * columnar.stripe (spec 7.1)
 * ------------------------------------------------------------------------- */

CREATE TABLE columnar.stripe (
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
	ON columnar.stripe USING btree (storage_id, stripe_num);

CREATE UNIQUE INDEX stripe_first_row_number_idx
	ON columnar.stripe USING btree (storage_id, first_row_number);

/* ---------------------------------------------------------------------------
 * columnar.chunk (spec 7.2)
 * ------------------------------------------------------------------------- */

CREATE TABLE columnar.chunk (
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
	value_count bigint NOT NULL
);

CREATE UNIQUE INDEX chunk_pkey
	ON columnar.chunk USING btree (storage_id, stripe_num, attr_num, chunk_group_num);

/* ---------------------------------------------------------------------------
 * columnar.chunk_group (spec 7.3)
 * ------------------------------------------------------------------------- */

CREATE TABLE columnar.chunk_group (
	storage_id bigint NOT NULL,
	stripe_num bigint NOT NULL,
	chunk_group_num integer NOT NULL,
	row_count bigint NOT NULL,
	deleted_rows bigint NOT NULL
);

CREATE UNIQUE INDEX chunk_group_pkey
	ON columnar.chunk_group USING btree (storage_id, stripe_num, chunk_group_num);

/* ---------------------------------------------------------------------------
 * columnar.row_mask (spec 7.5)
 *
 * Tracks deleted rows for updates and deletes without rewriting stripes. One
 * row per chunk group covers the row-number range [start_row_number,
 * end_row_number]; a set bit in "mask" marks a deleted row.
 * ------------------------------------------------------------------------- */

CREATE TABLE columnar.row_mask (
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
	ON columnar.row_mask USING btree (id, storage_id, start_row_number, end_row_number);

CREATE UNIQUE INDEX row_mask_chunk_unique
	ON columnar.row_mask USING btree (storage_id, stripe_id, chunk_id, start_row_number);

CREATE UNIQUE INDEX row_mask_stripe_unique
	ON columnar.row_mask USING btree (storage_id, stripe_id, start_row_number);

/* ---------------------------------------------------------------------------
 * Access method (spec 8.1)
 * ------------------------------------------------------------------------- */

CREATE FUNCTION columnar.columnar_handler(internal)
	RETURNS table_am_handler
	LANGUAGE C
	AS 'MODULE_PATHNAME', 'columnar_handler';

CREATE ACCESS METHOD columnar
	TYPE TABLE
	HANDLER columnar.columnar_handler;

COMMENT ON ACCESS METHOD columnar IS 'pgColumnar column-oriented storage';
