/*-------------------------------------------------------------------------
 *
 * columnar_metadata.c
 *		Access to the "columnar" metadata catalog tables and the storage-id
 *		sequence (spec 7). Metadata are ordinary heap tables keyed by storage
 *		id; we read and write them with direct catalog access so we do not
 *		depend on SPI reentrancy.
 *
 *-------------------------------------------------------------------------
 */
#include "columnar.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/sequence.h"
#include "miscadmin.h"
#include "storage/lock.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

/* attribute numbers for columnar.stripe (spec 7.1) */
#define Anum_stripe_storage_id 1
#define Anum_stripe_stripe_num 2
#define Anum_stripe_file_offset 3
#define Anum_stripe_data_length 4
#define Anum_stripe_column_count 5
#define Anum_stripe_chunk_row_count 6
#define Anum_stripe_row_count 7
#define Anum_stripe_chunk_group_count 8
#define Anum_stripe_first_row_number 9
#define Natts_stripe 9

/* attribute numbers for columnar.chunk (spec 7.2) */
#define Anum_chunk_storage_id 1
#define Anum_chunk_stripe_num 2
#define Anum_chunk_attr_num 3
#define Anum_chunk_chunk_group_num 4
#define Anum_chunk_minimum_value 5
#define Anum_chunk_maximum_value 6
#define Anum_chunk_value_stream_offset 7
#define Anum_chunk_value_stream_length 8
#define Anum_chunk_exists_stream_offset 9
#define Anum_chunk_exists_stream_length 10
#define Anum_chunk_value_compression_type 11
#define Anum_chunk_value_compression_level 12
#define Anum_chunk_value_decompressed_length 13
#define Anum_chunk_value_count 14
#define Anum_chunk_value_encoding_type 15
#define Anum_chunk_value_raw_length 16
#define Anum_chunk_bloom_filter 17
#define Natts_chunk 17

/* attribute numbers for columnar.chunk_group (spec 7.3) */
#define Anum_chunk_group_storage_id 1
#define Anum_chunk_group_stripe_num 2
#define Anum_chunk_group_chunk_group_num 3
#define Anum_chunk_group_row_count 4
#define Anum_chunk_group_deleted_rows 5
#define Natts_chunk_group 5

/* attribute numbers for columnar.options (spec 7.4) */
#define Anum_options_regclass 1
#define Anum_options_chunk_group_row_limit 2
#define Anum_options_stripe_row_limit 3
#define Anum_options_compression_level 4
#define Anum_options_compression 5
#define Anum_options_format_version 6

/* attribute numbers for columnar.projection (gap 26, format 2.2) */
#define Anum_projection_storage_id 1
#define Anum_projection_projection_id 2
#define Anum_projection_name 3
#define Anum_projection_proj_storage_id 4
#define Anum_projection_sort_key 5
#define Anum_projection_columns 6
#define Natts_projection 6

/* attribute numbers for the native format catalog (PGCN v1, native spec 11) */
#define Anum_native_storage_storage_id 1
#define Anum_native_storage_relation_oid 2
#define Anum_native_storage_format_version 3
#define Anum_native_storage_vector_length 4
#define Anum_native_storage_row_group_limit 5
#define Natts_native_storage 5

#define Anum_row_group_storage_id 1
#define Anum_row_group_group_number 2
#define Anum_row_group_file_offset 3
#define Anum_row_group_row_count 4
#define Anum_row_group_byte_length 5
#define Anum_row_group_first_row_number 6
#define Anum_row_group_sort_key 7
#define Natts_row_group 7

#define Anum_column_chunk_storage_id 1
#define Anum_column_chunk_group_number 2
#define Anum_column_chunk_column_index 3
#define Anum_column_chunk_value_count 4
#define Anum_column_chunk_encoding_descriptor 5
#define Anum_column_chunk_block_codec 6
#define Anum_column_chunk_page_offset 7
#define Anum_column_chunk_page_length 8
#define Natts_column_chunk 8

#define Anum_zone_map_storage_id 1
#define Anum_zone_map_group_number 2
#define Anum_zone_map_column_index 3
#define Anum_zone_map_vector_index 4
#define Anum_zone_map_minimum 5
#define Anum_zone_map_maximum 6
#define Anum_zone_map_sum 7
#define Anum_zone_map_value_count 8
#define Anum_zone_map_null_count 9
#define Natts_zone_map 9

#define Anum_bloom_storage_id 1
#define Anum_bloom_group_number 2
#define Anum_bloom_column_index 3
#define Anum_bloom_filter 4
#define Natts_bloom 4

/* attribute numbers for columnar.row_mask (spec 7.5) */
#define Anum_row_mask_id 1
#define Anum_row_mask_storage_id 2
#define Anum_row_mask_stripe_id 3
#define Anum_row_mask_chunk_id 4
#define Anum_row_mask_start_row_number 5
#define Anum_row_mask_end_row_number 6
#define Anum_row_mask_deleted_rows 7
#define Anum_row_mask_mask 8
#define Natts_row_mask 8

static Oid	columnar_schema_oid(void);
static Relation open_columnar_table(const char *name, LOCKMODE lockmode);

/*
 * columnar_schema_oid
 *		OID of the "columnar" schema. It always exists once the extension is
 *		installed.
 */
static Oid
columnar_schema_oid(void)
{
	return get_namespace_oid(COLUMNAR_SCHEMA_NAME, false);
}

static Relation
open_columnar_table(const char *name, LOCKMODE lockmode)
{
	Oid			nspOid = columnar_schema_oid();
	Oid			relOid = get_relname_relid(name, nspOid);

	if (!OidIsValid(relOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("columnar metadata table \"%s.%s\" does not exist",
						COLUMNAR_SCHEMA_NAME, name)));

	return table_open(relOid, lockmode);
}

/*
 * ColumnarNextStorageId
 *		Draw the next value from columnar.storageid_seq (spec 3, 7.6).
 */
uint64
ColumnarNextStorageId(void)
{
	Oid			nspOid = columnar_schema_oid();
	Oid			seqOid = get_relname_relid("storageid_seq", nspOid);
	int64		value;

	if (!OidIsValid(seqOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("columnar.storageid_seq does not exist")));

	value = nextval_internal(seqOid, false);
	return (uint64) value;
}

/*
 * ColumnarNextRowMaskId
 *		Draw the next value from columnar.row_mask_seq (spec 7.5, 7.6).
 */
uint64
ColumnarNextRowMaskId(void)
{
	Oid			nspOid = columnar_schema_oid();
	Oid			seqOid = get_relname_relid("row_mask_seq", nspOid);

	if (!OidIsValid(seqOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("columnar.row_mask_seq does not exist")));

	return (uint64) nextval_internal(seqOid, false);
}

/*
 * ColumnarCatalogSnapshot
 *		Return a snapshot for reading the columnar metadata catalog that also
 *		sees this transaction's own writes made in the current command (spec 9).
 *		curcid only affects visibility of the current transaction's own tuples,
 *		so advancing it yields read-your-writes without weakening isolation from
 *		other transactions.
 */
Snapshot
ColumnarCatalogSnapshot(Snapshot base)
{
	Snapshot	copy;
	CommandId	now;

	if (base == NULL || !IsMVCCSnapshot(base))
		return base;

	copy = (Snapshot) palloc(sizeof(SnapshotData));
	*copy = *base;

	now = GetCurrentCommandId(false);
	if (copy->curcid <= now)
		copy->curcid = now + 1;

	return copy;
}

/* -------------------------------------------------------------------------
 * inserts
 * ------------------------------------------------------------------------- */

void
ColumnarInsertStripeRow(const StripeMetadata *stripe)
{
	Relation	rel = open_columnar_table("stripe", RowExclusiveLock);
	TupleDesc	tupdesc = RelationGetDescr(rel);
	Datum		values[Natts_stripe];
	bool		nulls[Natts_stripe];
	HeapTuple	tuple;

	memset(nulls, false, sizeof(nulls));
	values[Anum_stripe_storage_id - 1] = Int64GetDatum((int64) stripe->storageId);
	values[Anum_stripe_stripe_num - 1] = Int64GetDatum((int64) stripe->stripeNum);
	values[Anum_stripe_file_offset - 1] = Int64GetDatum((int64) stripe->fileOffset);
	values[Anum_stripe_data_length - 1] = Int64GetDatum((int64) stripe->dataLength);
	values[Anum_stripe_column_count - 1] = Int32GetDatum(stripe->columnCount);
	values[Anum_stripe_chunk_row_count - 1] = Int32GetDatum(stripe->chunkRowCount);
	values[Anum_stripe_row_count - 1] = Int64GetDatum((int64) stripe->rowCount);
	values[Anum_stripe_chunk_group_count - 1] = Int32GetDatum(stripe->chunkGroupCount);
	values[Anum_stripe_first_row_number - 1] = Int64GetDatum((int64) stripe->firstRowNumber);

	tuple = heap_form_tuple(tupdesc, values, nulls);
	CatalogTupleInsert(rel, tuple);
	heap_freetuple(tuple);

	table_close(rel, RowExclusiveLock);
}

void
ColumnarInsertChunkGroupRow(uint64 storageId, const ChunkGroupMetadata *cg)
{
	Relation	rel = open_columnar_table("chunk_group", RowExclusiveLock);
	TupleDesc	tupdesc = RelationGetDescr(rel);
	Datum		values[Natts_chunk_group];
	bool		nulls[Natts_chunk_group];
	HeapTuple	tuple;

	memset(nulls, false, sizeof(nulls));
	values[Anum_chunk_group_storage_id - 1] = Int64GetDatum((int64) storageId);
	values[Anum_chunk_group_stripe_num - 1] = Int64GetDatum((int64) cg->stripeNum);
	values[Anum_chunk_group_chunk_group_num - 1] = Int32GetDatum(cg->chunkGroupNum);
	values[Anum_chunk_group_row_count - 1] = Int64GetDatum((int64) cg->rowCount);
	values[Anum_chunk_group_deleted_rows - 1] = Int64GetDatum((int64) cg->deletedRows);

	tuple = heap_form_tuple(tupdesc, values, nulls);
	CatalogTupleInsert(rel, tuple);
	heap_freetuple(tuple);

	table_close(rel, RowExclusiveLock);
}

void
ColumnarInsertChunkRow(uint64 storageId, const ChunkMetadata *chunk)
{
	Relation	rel = open_columnar_table("chunk", RowExclusiveLock);
	TupleDesc	tupdesc = RelationGetDescr(rel);
	Datum		values[Natts_chunk];
	bool		nulls[Natts_chunk];
	HeapTuple	tuple;

	memset(nulls, false, sizeof(nulls));
	values[Anum_chunk_storage_id - 1] = Int64GetDatum((int64) storageId);
	values[Anum_chunk_stripe_num - 1] = Int64GetDatum((int64) chunk->stripeNum);
	values[Anum_chunk_attr_num - 1] = Int32GetDatum(chunk->attrNum);
	values[Anum_chunk_chunk_group_num - 1] = Int32GetDatum(chunk->chunkGroupNum);

	/* min/max skip list (spec 7.2); NULL for non-orderable/empty chunks */
	if (chunk->minMaxValid)
	{
		bytea	   *minb = (bytea *) palloc(VARHDRSZ + chunk->minEncodedLen);
		bytea	   *maxb = (bytea *) palloc(VARHDRSZ + chunk->maxEncodedLen);

		SET_VARSIZE(minb, VARHDRSZ + chunk->minEncodedLen);
		memcpy(VARDATA(minb), chunk->minEncoded, chunk->minEncodedLen);
		values[Anum_chunk_minimum_value - 1] = PointerGetDatum(minb);

		SET_VARSIZE(maxb, VARHDRSZ + chunk->maxEncodedLen);
		memcpy(VARDATA(maxb), chunk->maxEncoded, chunk->maxEncodedLen);
		values[Anum_chunk_maximum_value - 1] = PointerGetDatum(maxb);
	}
	else
	{
		nulls[Anum_chunk_minimum_value - 1] = true;
		nulls[Anum_chunk_maximum_value - 1] = true;
	}

	values[Anum_chunk_value_stream_offset - 1] = Int64GetDatum((int64) chunk->valueStreamOffset);
	values[Anum_chunk_value_stream_length - 1] = Int64GetDatum((int64) chunk->valueStreamLength);
	values[Anum_chunk_exists_stream_offset - 1] = Int64GetDatum((int64) chunk->existsStreamOffset);
	values[Anum_chunk_exists_stream_length - 1] = Int64GetDatum((int64) chunk->existsStreamLength);
	values[Anum_chunk_value_compression_type - 1] = Int32GetDatum(chunk->valueCompressionType);
	values[Anum_chunk_value_compression_level - 1] = Int32GetDatum(chunk->valueCompressionLevel);
	values[Anum_chunk_value_decompressed_length - 1] = Int64GetDatum((int64) chunk->valueDecompressedLength);
	values[Anum_chunk_value_count - 1] = Int64GetDatum((int64) chunk->valueCount);
	values[Anum_chunk_value_encoding_type - 1] = Int32GetDatum(chunk->valueEncodingType);
	values[Anum_chunk_value_raw_length - 1] = Int64GetDatum((int64) chunk->valueRawLength);

	if (chunk->bloomFilter != NULL)
	{
		bytea	   *bl = (bytea *) palloc(VARHDRSZ + chunk->bloomLen);

		SET_VARSIZE(bl, VARHDRSZ + chunk->bloomLen);
		memcpy(VARDATA(bl), chunk->bloomFilter, chunk->bloomLen);
		values[Anum_chunk_bloom_filter - 1] = PointerGetDatum(bl);
	}
	else
		nulls[Anum_chunk_bloom_filter - 1] = true;

	tuple = heap_form_tuple(tupdesc, values, nulls);
	CatalogTupleInsert(rel, tuple);
	heap_freetuple(tuple);

	table_close(rel, RowExclusiveLock);
}

/* -------------------------------------------------------------------------
 * reads
 * ------------------------------------------------------------------------- */

/*
 * stripe_cmp
 *		qsort comparator ordering StripeMetadata by stripe number.
 */
static int
stripe_cmp(const ListCell *a, const ListCell *b)
{
	const StripeMetadata *sa = (const StripeMetadata *) lfirst(a);
	const StripeMetadata *sb = (const StripeMetadata *) lfirst(b);

	if (sa->stripeNum < sb->stripeNum)
		return -1;
	if (sa->stripeNum > sb->stripeNum)
		return 1;
	return 0;
}

static int
chunk_group_cmp(const ListCell *a, const ListCell *b)
{
	const ChunkGroupMetadata *ca = (const ChunkGroupMetadata *) lfirst(a);
	const ChunkGroupMetadata *cb = (const ChunkGroupMetadata *) lfirst(b);

	if (ca->chunkGroupNum < cb->chunkGroupNum)
		return -1;
	if (ca->chunkGroupNum > cb->chunkGroupNum)
		return 1;
	return 0;
}

List *
ColumnarReadStripeList(uint64 storageId, Snapshot snapshot)
{
	Relation	rel = open_columnar_table("stripe", AccessShareLock);
	TupleDesc	tupdesc = RelationGetDescr(rel);
	ScanKeyData key[1];
	SysScanDesc scan;
	HeapTuple	tuple;
	List	   *result = NIL;

	ScanKeyInit(&key[0], Anum_stripe_storage_id, BTEqualStrategyNumber,
				F_INT8EQ, Int64GetDatum((int64) storageId));

	scan = systable_beginscan(rel, InvalidOid, false, snapshot, 1, key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		StripeMetadata *stripe = palloc0(sizeof(StripeMetadata));
		bool		isnull;

		stripe->storageId = storageId;
		stripe->stripeNum = (uint64) DatumGetInt64(
			heap_getattr(tuple, Anum_stripe_stripe_num, tupdesc, &isnull));
		stripe->fileOffset = (uint64) DatumGetInt64(
			heap_getattr(tuple, Anum_stripe_file_offset, tupdesc, &isnull));
		stripe->dataLength = (uint64) DatumGetInt64(
			heap_getattr(tuple, Anum_stripe_data_length, tupdesc, &isnull));
		stripe->columnCount = DatumGetInt32(
			heap_getattr(tuple, Anum_stripe_column_count, tupdesc, &isnull));
		stripe->chunkRowCount = DatumGetInt32(
			heap_getattr(tuple, Anum_stripe_chunk_row_count, tupdesc, &isnull));
		stripe->rowCount = (uint64) DatumGetInt64(
			heap_getattr(tuple, Anum_stripe_row_count, tupdesc, &isnull));
		stripe->chunkGroupCount = DatumGetInt32(
			heap_getattr(tuple, Anum_stripe_chunk_group_count, tupdesc, &isnull));
		stripe->firstRowNumber = (uint64) DatumGetInt64(
			heap_getattr(tuple, Anum_stripe_first_row_number, tupdesc, &isnull));

		result = lappend(result, stripe);
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	list_sort(result, stripe_cmp);
	return result;
}

/*
 * ColumnarComputeAllVisibleGroups
 *		Return the chunk groups that are all-visible to every snapshot (gap 28
 *		phase 3): the covering stripe's insert xid is frozen or precedes
 *		`oldestXmin` (so every current/future snapshot sees the insert), and the
 *		group has no deletes -- committed *or* in progress. Deletes are checked
 *		under a dirty snapshot so a group being modified concurrently is excluded;
 *		combined with clear-on-write (which removes a bit for any later
 *		delete/insert), this keeps a set bit from ever covering a modified row.
 *		Returns a List of ColumnarRowRange * (one per all-visible group).
 */
List *
ColumnarComputeAllVisibleGroups(uint64 storageId, TransactionId oldestXmin)
{
	Relation	srel = open_columnar_table("stripe", AccessShareLock);
	TupleDesc	stupdesc = RelationGetDescr(srel);
	ScanKeyData skey[1];
	SysScanDesc sscan;
	HeapTuple	stuple;
	Snapshot	snap;
	SnapshotData dirty;
	List	   *ranges = NIL;

	/*
	 * Register the MVCC snapshot: PG18 asserts that a snapshot used for heap
	 * visibility in a scan is registered or active (heapam_visibility.c). Called
	 * from the vacuum path, there is no active snapshot to rely on.
	 */
	snap = RegisterSnapshot(GetLatestSnapshot());
	InitDirtySnapshot(dirty);

	ScanKeyInit(&skey[0], Anum_stripe_storage_id, BTEqualStrategyNumber,
				F_INT8EQ, Int64GetDatum((int64) storageId));
	sscan = systable_beginscan(srel, InvalidOid, false, snap, 1, skey);
	while (HeapTupleIsValid(stuple = systable_getnext(sscan)))
	{
		TransactionId xmin = HeapTupleHeaderGetXmin(stuple->t_data);
		bool		isnull;
		uint64		stripeNum,
					firstRow,
					rowCount;
		int			chunkRowCount,
					chunkGroupCount,
					g;
		bool	   *deleted;
		List	   *rmList;
		ListCell   *lc;

		/* the stripe (hence all its rows) must be visible to every snapshot */
		if (TransactionIdIsNormal(xmin) &&
			!TransactionIdPrecedes(xmin, oldestXmin))
			continue;

		stripeNum = (uint64) DatumGetInt64(
			heap_getattr(stuple, Anum_stripe_stripe_num, stupdesc, &isnull));
		chunkRowCount = DatumGetInt32(
			heap_getattr(stuple, Anum_stripe_chunk_row_count, stupdesc, &isnull));
		rowCount = (uint64) DatumGetInt64(
			heap_getattr(stuple, Anum_stripe_row_count, stupdesc, &isnull));
		chunkGroupCount = DatumGetInt32(
			heap_getattr(stuple, Anum_stripe_chunk_group_count, stupdesc, &isnull));
		firstRow = (uint64) DatumGetInt64(
			heap_getattr(stuple, Anum_stripe_first_row_number, stupdesc, &isnull));

		if (chunkGroupCount <= 0 || chunkRowCount <= 0)
			continue;

		/* mark groups that have any delete (committed or in progress) */
		deleted = palloc0(sizeof(bool) * chunkGroupCount);
		rmList = ColumnarReadRowMaskList(storageId, stripeNum, &dirty);
		foreach(lc, rmList)
		{
			RowMaskMetadata *rm = (RowMaskMetadata *) lfirst(lc);

			if (rm->deletedRows > 0 &&
				rm->chunkId >= 0 && rm->chunkId < chunkGroupCount)
				deleted[rm->chunkId] = true;
		}

		for (g = 0; g < chunkGroupCount; g++)
		{
			uint64		gStart = firstRow + (uint64) g * (uint64) chunkRowCount;
			uint64		gRows = rowCount - (uint64) g * (uint64) chunkRowCount;
			ColumnarRowRange *r;

			if (deleted[g])
				continue;
			if (gRows > (uint64) chunkRowCount)
				gRows = (uint64) chunkRowCount;
			if (gRows == 0)
				continue;

			r = palloc(sizeof(ColumnarRowRange));
			r->firstRowNumber = gStart;
			r->rowCount = gRows;
			ranges = lappend(ranges, r);
		}
		pfree(deleted);
	}
	systable_endscan(sscan);
	table_close(srel, AccessShareLock);
	UnregisterSnapshot(snap);

	return ranges;
}

List *
ColumnarReadChunkGroupList(uint64 storageId, uint64 stripeNum, Snapshot snapshot)
{
	Relation	rel = open_columnar_table("chunk_group", AccessShareLock);
	TupleDesc	tupdesc = RelationGetDescr(rel);
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tuple;
	List	   *result = NIL;

	ScanKeyInit(&key[0], Anum_chunk_group_storage_id, BTEqualStrategyNumber,
				F_INT8EQ, Int64GetDatum((int64) storageId));
	ScanKeyInit(&key[1], Anum_chunk_group_stripe_num, BTEqualStrategyNumber,
				F_INT8EQ, Int64GetDatum((int64) stripeNum));

	scan = systable_beginscan(rel, InvalidOid, false, snapshot, 2, key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		ChunkGroupMetadata *cg = palloc0(sizeof(ChunkGroupMetadata));
		bool		isnull;

		cg->stripeNum = stripeNum;
		cg->chunkGroupNum = DatumGetInt32(
			heap_getattr(tuple, Anum_chunk_group_chunk_group_num, tupdesc, &isnull));
		cg->rowCount = (uint64) DatumGetInt64(
			heap_getattr(tuple, Anum_chunk_group_row_count, tupdesc, &isnull));
		cg->deletedRows = (uint64) DatumGetInt64(
			heap_getattr(tuple, Anum_chunk_group_deleted_rows, tupdesc, &isnull));

		result = lappend(result, cg);
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	list_sort(result, chunk_group_cmp);
	return result;
}

List *
ColumnarReadChunkList(uint64 storageId, uint64 stripeNum, Snapshot snapshot)
{
	Relation	rel = open_columnar_table("chunk", AccessShareLock);
	TupleDesc	tupdesc = RelationGetDescr(rel);
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tuple;
	List	   *result = NIL;

	ScanKeyInit(&key[0], Anum_chunk_storage_id, BTEqualStrategyNumber,
				F_INT8EQ, Int64GetDatum((int64) storageId));
	ScanKeyInit(&key[1], Anum_chunk_stripe_num, BTEqualStrategyNumber,
				F_INT8EQ, Int64GetDatum((int64) stripeNum));

	scan = systable_beginscan(rel, InvalidOid, false, snapshot, 2, key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		ChunkMetadata *chunk = palloc0(sizeof(ChunkMetadata));
		bool		isnull;

		chunk->stripeNum = stripeNum;
		chunk->attrNum = DatumGetInt32(
			heap_getattr(tuple, Anum_chunk_attr_num, tupdesc, &isnull));
		chunk->chunkGroupNum = DatumGetInt32(
			heap_getattr(tuple, Anum_chunk_chunk_group_num, tupdesc, &isnull));
		chunk->valueStreamOffset = (uint64) DatumGetInt64(
			heap_getattr(tuple, Anum_chunk_value_stream_offset, tupdesc, &isnull));
		chunk->valueStreamLength = (uint64) DatumGetInt64(
			heap_getattr(tuple, Anum_chunk_value_stream_length, tupdesc, &isnull));
		chunk->existsStreamOffset = (uint64) DatumGetInt64(
			heap_getattr(tuple, Anum_chunk_exists_stream_offset, tupdesc, &isnull));
		chunk->existsStreamLength = (uint64) DatumGetInt64(
			heap_getattr(tuple, Anum_chunk_exists_stream_length, tupdesc, &isnull));
		chunk->valueCompressionType = DatumGetInt32(
			heap_getattr(tuple, Anum_chunk_value_compression_type, tupdesc, &isnull));
		chunk->valueCompressionLevel = DatumGetInt32(
			heap_getattr(tuple, Anum_chunk_value_compression_level, tupdesc, &isnull));
		chunk->valueDecompressedLength = (uint64) DatumGetInt64(
			heap_getattr(tuple, Anum_chunk_value_decompressed_length, tupdesc, &isnull));
		chunk->valueCount = (uint64) DatumGetInt64(
			heap_getattr(tuple, Anum_chunk_value_count, tupdesc, &isnull));

		/*
		 * Value-stream encoding (I1, format 2.1). Format 2.0 chunks predate
		 * these columns; a NULL encoding type means NONE, and the raw length
		 * then equals the decompressed length.
		 */
		{
			bool		encNull;
			bool		rawNull;
			Datum		encDatum = heap_getattr(tuple, Anum_chunk_value_encoding_type,
												tupdesc, &encNull);
			Datum		rawDatum = heap_getattr(tuple, Anum_chunk_value_raw_length,
												tupdesc, &rawNull);

			chunk->valueEncodingType = encNull ? COLUMNAR_ENCODING_NONE
				: DatumGetInt32(encDatum);
			chunk->valueRawLength = rawNull ? chunk->valueDecompressedLength
				: (uint64) DatumGetInt64(rawDatum);
		}

		/* optional per-chunk bloom filter (I7) */
		{
			bool		bloomNull;
			Datum		bloomDatum = heap_getattr(tuple, Anum_chunk_bloom_filter,
												  tupdesc, &bloomNull);

			if (!bloomNull)
			{
				bytea	   *bl = DatumGetByteaP(bloomDatum);

				chunk->bloomLen = VARSIZE(bl) >= VARHDRSZ ?
					(uint32) (VARSIZE(bl) - VARHDRSZ) : 0;
				chunk->bloomFilter = palloc(chunk->bloomLen + 1);
				memcpy(chunk->bloomFilter, VARDATA(bl), chunk->bloomLen);
			}
		}

		/* min/max skip list (spec 7.2), decoded on demand by the reader */
		{
			bool		minNull;
			bool		maxNull;
			Datum		minDatum = heap_getattr(tuple, Anum_chunk_minimum_value,
												tupdesc, &minNull);
			Datum		maxDatum = heap_getattr(tuple, Anum_chunk_maximum_value,
												tupdesc, &maxNull);

			if (!minNull && !maxNull)
			{
				bytea	   *minb = DatumGetByteaP(minDatum);
				bytea	   *maxb = DatumGetByteaP(maxDatum);

				chunk->minEncodedLen = VARSIZE(minb) >= VARHDRSZ ?
					(uint32) (VARSIZE(minb) - VARHDRSZ) : 0;
				chunk->minEncoded = palloc(chunk->minEncodedLen + 1);
				memcpy(chunk->minEncoded, VARDATA(minb), chunk->minEncodedLen);

				chunk->maxEncodedLen = VARSIZE(maxb) >= VARHDRSZ ?
					(uint32) (VARSIZE(maxb) - VARHDRSZ) : 0;
				chunk->maxEncoded = palloc(chunk->maxEncodedLen + 1);
				memcpy(chunk->maxEncoded, VARDATA(maxb), chunk->maxEncodedLen);

				chunk->minMaxValid = true;
			}
		}

		result = lappend(result, chunk);
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	return result;
}

/*
 * ColumnarReadRowMaskList
 *		Read all row_mask rows for a stripe (spec 7.5). Returns a list of
 *		RowMaskMetadata* allocated in the current memory context.
 */
List *
ColumnarReadRowMaskList(uint64 storageId, uint64 stripeId, Snapshot snapshot)
{
	Relation	rel = open_columnar_table("row_mask", AccessShareLock);
	TupleDesc	tupdesc = RelationGetDescr(rel);
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tuple;
	List	   *result = NIL;

	ScanKeyInit(&key[0], Anum_row_mask_storage_id, BTEqualStrategyNumber,
				F_INT8EQ, Int64GetDatum((int64) storageId));
	ScanKeyInit(&key[1], Anum_row_mask_stripe_id, BTEqualStrategyNumber,
				F_INT8EQ, Int64GetDatum((int64) stripeId));

	scan = systable_beginscan(rel, InvalidOid, false, snapshot, 2, key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		RowMaskMetadata *rm = palloc0(sizeof(RowMaskMetadata));
		bool		isnull;
		Datum		maskDatum;

		rm->id = (uint64) DatumGetInt64(
			heap_getattr(tuple, Anum_row_mask_id, tupdesc, &isnull));
		rm->stripeId = stripeId;
		rm->chunkId = DatumGetInt32(
			heap_getattr(tuple, Anum_row_mask_chunk_id, tupdesc, &isnull));
		rm->startRowNumber = (uint64) DatumGetInt64(
			heap_getattr(tuple, Anum_row_mask_start_row_number, tupdesc, &isnull));
		rm->endRowNumber = (uint64) DatumGetInt64(
			heap_getattr(tuple, Anum_row_mask_end_row_number, tupdesc, &isnull));
		rm->deletedRows = DatumGetInt32(
			heap_getattr(tuple, Anum_row_mask_deleted_rows, tupdesc, &isnull));

		maskDatum = heap_getattr(tuple, Anum_row_mask_mask, tupdesc, &isnull);
		if (!isnull)
		{
			bytea	   *maskb = DatumGetByteaP(maskDatum);

			rm->maskLen = VARSIZE(maskb) >= VARHDRSZ ?
				(uint32) (VARSIZE(maskb) - VARHDRSZ) : 0;
			rm->mask = palloc(rm->maskLen + 1);
			memcpy(rm->mask, VARDATA(maskb), rm->maskLen);
		}
		else
		{
			rm->mask = NULL;
			rm->maskLen = 0;
		}

		result = lappend(result, rm);
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	return result;
}

/*
 * rowmask_chunk_lock_key
 *		Mix the identity of a chunk group into a 64-bit advisory-lock key. The
 *		triple (storage_id, stripe_id, chunk_id) uniquely names a chunk group
 *		(start_row_number is a function of the stripe and chunk), so it is the
 *		full key. A finalizing avalanche spreads the bits so distinct chunk
 *		groups almost never share a key; a collision only makes two unrelated
 *		chunk groups serialize needlessly, it never affects correctness.
 */
static uint64
rowmask_chunk_lock_key(uint64 storageId, uint64 stripeId, int chunkId)
{
	uint64		h = 1469598103934665603UL;	/* FNV-1a 64-bit offset basis */

	h = (h ^ storageId) * 1099511628211UL;
	h = (h ^ stripeId) * 1099511628211UL;
	h = (h ^ (uint64) (uint32) chunkId) * 1099511628211UL;

	/* splitmix64/murmur3 finalizer for a good avalanche */
	h ^= h >> 33;
	h *= 0xff51afd7ed558ccdUL;
	h ^= h >> 33;
	h *= 0xc4ceb9fe1a85ec53UL;
	h ^= h >> 33;

	return h;
}

/*
 * rowmask_lock_chunk_group
 *		Take a transaction-scoped exclusive lock covering one chunk group's
 *		row_mask tuple, so that the read-modify-write in ColumnarUpsertRowMask
 *		serializes against any concurrent deleter or updater touching the SAME
 *		chunk group, while deletes to different chunk groups still proceed
 *		concurrently. The lock is held until this transaction ends (commit or
 *		abort), which is required: a concurrent deleter that acquires the lock
 *		after us must not run its SnapshotSelf read until our merged tuple is
 *		committed and therefore visible to it.
 *
 *		An advisory lock tag is used because the row_mask heap tuple to protect
 *		may not exist yet on the first delete of a chunk group; the key names
 *		the chunk group itself, so the first-insert race is serialized too. The
 *		lock is taken with a plain wait (deadlock-detector armed); callers must
 *		acquire chunk-group locks in a consistent global order (see
 *		rowmask_flush_buffer) so two transactions cannot form an AB-BA cycle.
 */
static void
rowmask_lock_chunk_group(uint64 storageId, uint64 stripeId, int chunkId)
{
	LOCKTAG		tag;
	uint64		key = rowmask_chunk_lock_key(storageId, stripeId, chunkId);

	SET_LOCKTAG_ADVISORY(tag, MyDatabaseId,
						 (uint32) (key >> 32), (uint32) (key & 0xFFFFFFFF), 1);

	(void) LockAcquire(&tag, ExclusiveLock, false /* transaction lock */ ,
					   false /* wait */ );
}

/*
 * ColumnarUpsertRowMask
 *		Insert or replace the row_mask row for one chunk group, identified by
 *		(storage_id, stripe_id, chunk_id, start_row_number). If a row already
 *		exists it is replaced with the merged mask carried in rm; otherwise a
 *		fresh row is inserted with a new id from row_mask_seq. Used at flush of
 *		the in-memory delete buffer (columnar_row_mask.c), at most once per
 *		chunk group per flush, so a single heap tuple is never updated twice in
 *		the same command.
 *
 *		Concurrency (issue #4): the whole read-modify-write is guarded by a
 *		transaction-scoped chunk-group lock (rowmask_lock_chunk_group), so two
 *		transactions deleting different rows in the same chunk group serialize
 *		instead of overwriting each other's delete bits. Once the lock is held,
 *		any earlier writer to this chunk group has committed, so the existing
 *		row is located with SnapshotSelf (which also sees this transaction's own
 *		prior modifications) and its bits are merged (bitwise OR) into rm->mask.
 *		Because no concurrent writer can touch the tuple while we hold the lock,
 *		the CatalogTupleUpdate cannot lose an update and the CatalogTupleInsert
 *		on the first-delete path cannot hit a duplicate-key race.
 */
void
ColumnarUpsertRowMask(uint64 storageId, RowMaskMetadata *rm)
{
	Relation	rel;
	TupleDesc	tupdesc;
	ScanKeyData key[4];
	SysScanDesc scan;
	HeapTuple	existing;
	HeapTuple	tuple;
	Datum		values[Natts_row_mask];
	bool		nulls[Natts_row_mask];
	bytea	   *maskb;
	uint64		id = rm->id;
	int			deletedRows = rm->deletedRows;
	ItemPointerData replaceTid;
	bool		haveReplace = false;

	rowmask_lock_chunk_group(storageId, rm->stripeId, rm->chunkId);

	rel = open_columnar_table("row_mask", RowExclusiveLock);
	tupdesc = RelationGetDescr(rel);

	ScanKeyInit(&key[0], Anum_row_mask_storage_id, BTEqualStrategyNumber,
				F_INT8EQ, Int64GetDatum((int64) storageId));
	ScanKeyInit(&key[1], Anum_row_mask_stripe_id, BTEqualStrategyNumber,
				F_INT8EQ, Int64GetDatum((int64) rm->stripeId));
	ScanKeyInit(&key[2], Anum_row_mask_chunk_id, BTEqualStrategyNumber,
				F_INT4EQ, Int32GetDatum(rm->chunkId));
	ScanKeyInit(&key[3], Anum_row_mask_start_row_number, BTEqualStrategyNumber,
				F_INT8EQ, Int64GetDatum((int64) rm->startRowNumber));

	scan = systable_beginscan(rel, InvalidOid, false, SnapshotSelf, 4, key);
	if (HeapTupleIsValid(existing = systable_getnext(scan)))
	{
		bool		isnull;
		Datum		existingMask;

		/* keep the existing id; merge the existing mask bits into rm->mask */
		id = (uint64) DatumGetInt64(
			heap_getattr(existing, Anum_row_mask_id, tupdesc, &isnull));

		existingMask = heap_getattr(existing, Anum_row_mask_mask, tupdesc, &isnull);
		if (!isnull)
		{
			bytea	   *eb = DatumGetByteaP(existingMask);
			uint32		elen = VARSIZE(eb) >= VARHDRSZ ?
				(uint32) (VARSIZE(eb) - VARHDRSZ) : 0;
			char	   *ebytes = VARDATA(eb);
			uint32		i;
			int			bit;

			deletedRows = 0;
			for (i = 0; i < rm->maskLen; i++)
			{
				if (i < elen)
					rm->mask[i] |= ebytes[i];
			}
			for (i = 0; i < rm->maskLen; i++)
				for (bit = 0; bit < 8; bit++)
					if (rm->mask[i] & (1 << bit))
						deletedRows++;
		}

		replaceTid = existing->t_self;
		haveReplace = true;
	}
	systable_endscan(scan);

	memset(nulls, false, sizeof(nulls));
	values[Anum_row_mask_id - 1] = Int64GetDatum((int64) id);
	values[Anum_row_mask_storage_id - 1] = Int64GetDatum((int64) storageId);
	values[Anum_row_mask_stripe_id - 1] = Int64GetDatum((int64) rm->stripeId);
	values[Anum_row_mask_chunk_id - 1] = Int32GetDatum(rm->chunkId);
	values[Anum_row_mask_start_row_number - 1] = Int64GetDatum((int64) rm->startRowNumber);
	values[Anum_row_mask_end_row_number - 1] = Int64GetDatum((int64) rm->endRowNumber);
	values[Anum_row_mask_deleted_rows - 1] = Int32GetDatum(deletedRows);

	maskb = (bytea *) palloc(VARHDRSZ + rm->maskLen);
	SET_VARSIZE(maskb, VARHDRSZ + rm->maskLen);
	memcpy(VARDATA(maskb), rm->mask, rm->maskLen);
	values[Anum_row_mask_mask - 1] = PointerGetDatum(maskb);

	tuple = heap_form_tuple(tupdesc, values, nulls);

	if (haveReplace)
	{
		tuple->t_self = replaceTid;
		CatalogTupleUpdate(rel, &replaceTid, tuple);
	}
	else
		CatalogTupleInsert(rel, tuple);

	heap_freetuple(tuple);
	table_close(rel, RowExclusiveLock);
}

/*
 * ColumnarDeleteMetadata
 *		Remove every metadata row for a storage id. Used when a columnar
 *		table is dropped or truncated.
 */
static void
delete_rows_by_storage_id(const char *tableName, AttrNumber storageAttno,
						  uint64 storageId)
{
	Relation	rel = open_columnar_table(tableName, RowExclusiveLock);
	ScanKeyData key[1];
	SysScanDesc scan;
	HeapTuple	tuple;

	ScanKeyInit(&key[0], storageAttno, BTEqualStrategyNumber,
				F_INT8EQ, Int64GetDatum((int64) storageId));

	scan = systable_beginscan(rel, InvalidOid, false, NULL, 1, key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
		CatalogTupleDelete(rel, &tuple->t_self);
	systable_endscan(scan);

	table_close(rel, RowExclusiveLock);
}

void
ColumnarDeleteMetadata(uint64 storageId)
{
	delete_rows_by_storage_id("chunk", Anum_chunk_storage_id, storageId);
	delete_rows_by_storage_id("chunk_group", Anum_chunk_group_storage_id, storageId);
	delete_rows_by_storage_id("row_mask", Anum_row_mask_storage_id, storageId);
	delete_rows_by_storage_id("stripe", Anum_stripe_storage_id, storageId);
	/* native format catalog (PGCN v1); no-op rows for 2.2-line tables */
	delete_rows_by_storage_id("column_chunk", Anum_column_chunk_storage_id, storageId);
	delete_rows_by_storage_id("zone_map", Anum_zone_map_storage_id, storageId);
	delete_rows_by_storage_id("bloom", Anum_bloom_storage_id, storageId);
	delete_rows_by_storage_id("row_group", Anum_row_group_storage_id, storageId);
	delete_rows_by_storage_id("storage", Anum_native_storage_storage_id, storageId);
}

/*
 * ColumnarInsertNativeStorageRow, ColumnarInsertRowGroupRow,
 * ColumnarInsertColumnChunkRow
 *		Record the native-format catalog rows (PGCN v1, native spec 11). Called
 *		by the native writer's flush. The 2.2-line writer does not use these.
 */
void
ColumnarInsertNativeStorageRow(const NativeStorageMetadata *s)
{
	Relation	rel = open_columnar_table("storage", RowExclusiveLock);
	TupleDesc	tupdesc = RelationGetDescr(rel);
	Datum		values[Natts_native_storage];
	bool		nulls[Natts_native_storage];
	HeapTuple	tuple;
	Snapshot	base;
	Snapshot	snapshot;
	ScanKeyData key[1];
	SysScanDesc scan;
	bool		exists;

	/* idempotent: the storage row is written once per storage id, but the
	 * native writer calls this on every row-group flush */
	base = ActiveSnapshotSet() ? GetActiveSnapshot() : GetTransactionSnapshot();
	snapshot = ColumnarCatalogSnapshot(base);
	ScanKeyInit(&key[0], Anum_native_storage_storage_id, BTEqualStrategyNumber,
				F_INT8EQ, Int64GetDatum((int64) s->storageId));
	scan = systable_beginscan(rel, InvalidOid, false, snapshot, 1, key);
	exists = HeapTupleIsValid(systable_getnext(scan));
	systable_endscan(scan);
	if (exists)
	{
		table_close(rel, RowExclusiveLock);
		return;
	}

	memset(nulls, false, sizeof(nulls));
	values[Anum_native_storage_storage_id - 1] = Int64GetDatum((int64) s->storageId);
	values[Anum_native_storage_relation_oid - 1] = ObjectIdGetDatum(s->relationOid);
	values[Anum_native_storage_format_version - 1] = Int32GetDatum(s->formatVersion);
	values[Anum_native_storage_vector_length - 1] = Int32GetDatum(s->vectorLength);
	values[Anum_native_storage_row_group_limit - 1] = Int32GetDatum(s->rowGroupLimit);

	tuple = heap_form_tuple(tupdesc, values, nulls);
	CatalogTupleInsert(rel, tuple);
	heap_freetuple(tuple);
	table_close(rel, RowExclusiveLock);
}

void
ColumnarInsertRowGroupRow(const NativeRowGroupMetadata *rg)
{
	Relation	rel = open_columnar_table("row_group", RowExclusiveLock);
	TupleDesc	tupdesc = RelationGetDescr(rel);
	Datum		values[Natts_row_group];
	bool		nulls[Natts_row_group];
	HeapTuple	tuple;

	memset(nulls, false, sizeof(nulls));
	values[Anum_row_group_storage_id - 1] = Int64GetDatum((int64) rg->storageId);
	values[Anum_row_group_group_number - 1] = Int64GetDatum((int64) rg->groupNumber);
	values[Anum_row_group_file_offset - 1] = Int64GetDatum((int64) rg->fileOffset);
	values[Anum_row_group_row_count - 1] = Int64GetDatum((int64) rg->rowCount);
	values[Anum_row_group_byte_length - 1] = Int64GetDatum((int64) rg->byteLength);
	values[Anum_row_group_first_row_number - 1] = Int64GetDatum((int64) rg->firstRowNumber);
	values[Anum_row_group_sort_key - 1] =
		PointerGetDatum(construct_empty_array(INT2OID));

	tuple = heap_form_tuple(tupdesc, values, nulls);
	CatalogTupleInsert(rel, tuple);
	heap_freetuple(tuple);
	table_close(rel, RowExclusiveLock);
}

void
ColumnarInsertColumnChunkRow(const NativeColumnChunkMetadata *cc)
{
	Relation	rel = open_columnar_table("column_chunk", RowExclusiveLock);
	TupleDesc	tupdesc = RelationGetDescr(rel);
	Datum		values[Natts_column_chunk];
	bool		nulls[Natts_column_chunk];
	HeapTuple	tuple;
	bytea	   *desc;

	memset(nulls, false, sizeof(nulls));
	desc = (bytea *) palloc(VARHDRSZ + cc->encodingDescriptorLen);
	SET_VARSIZE(desc, VARHDRSZ + cc->encodingDescriptorLen);
	if (cc->encodingDescriptorLen > 0)
		memcpy(VARDATA(desc), cc->encodingDescriptor, cc->encodingDescriptorLen);

	values[Anum_column_chunk_storage_id - 1] = Int64GetDatum((int64) cc->storageId);
	values[Anum_column_chunk_group_number - 1] = Int64GetDatum((int64) cc->groupNumber);
	values[Anum_column_chunk_column_index - 1] = Int16GetDatum((int16) cc->columnIndex);
	values[Anum_column_chunk_value_count - 1] = Int64GetDatum((int64) cc->valueCount);
	values[Anum_column_chunk_encoding_descriptor - 1] = PointerGetDatum(desc);
	values[Anum_column_chunk_block_codec - 1] = Int16GetDatum((int16) cc->blockCodec);
	values[Anum_column_chunk_page_offset - 1] = Int64GetDatum((int64) cc->pageOffset);
	values[Anum_column_chunk_page_length - 1] = Int64GetDatum((int64) cc->pageLength);

	tuple = heap_form_tuple(tupdesc, values, nulls);
	CatalogTupleInsert(rel, tuple);
	heap_freetuple(tuple);
	table_close(rel, RowExclusiveLock);
}

/*
 * ColumnarInsertZoneMapRow
 *		Record one native zone-map row (Small Materialized Aggregate) for a vector
 *		or for a whole column chunk (native spec 7.1, Phase D5). Called by the
 *		native writer's flush.
 */
void
ColumnarInsertZoneMapRow(const NativeZoneMapMetadata *z)
{
	Relation	rel = open_columnar_table("zone_map", RowExclusiveLock);
	TupleDesc	tupdesc = RelationGetDescr(rel);
	Datum		values[Natts_zone_map];
	bool		nulls[Natts_zone_map];
	HeapTuple	tuple;

	memset(nulls, false, sizeof(nulls));

	values[Anum_zone_map_storage_id - 1] = Int64GetDatum((int64) z->storageId);
	values[Anum_zone_map_group_number - 1] = Int64GetDatum((int64) z->groupNumber);
	values[Anum_zone_map_column_index - 1] = Int16GetDatum((int16) z->columnIndex);
	values[Anum_zone_map_vector_index - 1] = Int32GetDatum((int32) z->vectorIndex);

	if (z->hasMinMax)
	{
		bytea	   *mn = (bytea *) palloc(VARHDRSZ + z->minimumLen);
		bytea	   *mx = (bytea *) palloc(VARHDRSZ + z->maximumLen);

		SET_VARSIZE(mn, VARHDRSZ + z->minimumLen);
		SET_VARSIZE(mx, VARHDRSZ + z->maximumLen);
		if (z->minimumLen > 0)
			memcpy(VARDATA(mn), z->minimum, z->minimumLen);
		if (z->maximumLen > 0)
			memcpy(VARDATA(mx), z->maximum, z->maximumLen);
		values[Anum_zone_map_minimum - 1] = PointerGetDatum(mn);
		values[Anum_zone_map_maximum - 1] = PointerGetDatum(mx);
	}
	else
	{
		nulls[Anum_zone_map_minimum - 1] = true;
		nulls[Anum_zone_map_maximum - 1] = true;
	}

	if (z->hasSum)
		values[Anum_zone_map_sum - 1] = z->sum;
	else
		nulls[Anum_zone_map_sum - 1] = true;

	values[Anum_zone_map_value_count - 1] = Int64GetDatum((int64) z->valueCount);
	values[Anum_zone_map_null_count - 1] = Int64GetDatum((int64) z->nullCount);

	tuple = heap_form_tuple(tupdesc, values, nulls);
	CatalogTupleInsert(rel, tuple);
	heap_freetuple(tuple);
	table_close(rel, RowExclusiveLock);
}

/*
 * ColumnarInsertBloomRow
 *		Record one per-column-chunk bloom filter (native spec 7.2, Phase D5b).
 */
void
ColumnarInsertBloomRow(const NativeBloomMetadata *b)
{
	Relation	rel = open_columnar_table("bloom", RowExclusiveLock);
	TupleDesc	tupdesc = RelationGetDescr(rel);
	Datum		values[Natts_bloom];
	bool		nulls[Natts_bloom];
	HeapTuple	tuple;
	bytea	   *filt;

	memset(nulls, false, sizeof(nulls));
	filt = (bytea *) palloc(VARHDRSZ + b->filterLen);
	SET_VARSIZE(filt, VARHDRSZ + b->filterLen);
	if (b->filterLen > 0)
		memcpy(VARDATA(filt), b->filter, b->filterLen);

	values[Anum_bloom_storage_id - 1] = Int64GetDatum((int64) b->storageId);
	values[Anum_bloom_group_number - 1] = Int64GetDatum((int64) b->groupNumber);
	values[Anum_bloom_column_index - 1] = Int16GetDatum((int16) b->columnIndex);
	values[Anum_bloom_filter - 1] = PointerGetDatum(filt);

	tuple = heap_form_tuple(tupdesc, values, nulls);
	CatalogTupleInsert(rel, tuple);
	heap_freetuple(tuple);
	table_close(rel, RowExclusiveLock);
}

/*
 * ColumnarReadBloomList
 *		The per-column-chunk bloom filters of one row group (native spec 7.2,
 *		Phase D5b). The caller indexes the result by column_index; the filter
 *		bytes are copied into the current memory context.
 */
List *
ColumnarReadBloomList(uint64 storageId, uint64 groupNumber, Snapshot snapshot)
{
	Relation	rel = open_columnar_table("bloom", AccessShareLock);
	TupleDesc	tupdesc = RelationGetDescr(rel);
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tuple;
	List	   *result = NIL;

	ScanKeyInit(&key[0], Anum_bloom_storage_id, BTEqualStrategyNumber,
				F_INT8EQ, Int64GetDatum((int64) storageId));
	ScanKeyInit(&key[1], Anum_bloom_group_number, BTEqualStrategyNumber,
				F_INT8EQ, Int64GetDatum((int64) groupNumber));
	scan = systable_beginscan(rel, InvalidOid, false, snapshot, 2, key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		NativeBloomMetadata *b = palloc0(sizeof(NativeBloomMetadata));
		bool		isnull;
		Datum		d;

		b->storageId = storageId;
		b->groupNumber = groupNumber;
		b->columnIndex = DatumGetInt16(
			heap_getattr(tuple, Anum_bloom_column_index, tupdesc, &isnull));
		d = heap_getattr(tuple, Anum_bloom_filter, tupdesc, &isnull);
		if (!isnull)
		{
			bytea	   *bf = DatumGetByteaPP(d);

			b->filterLen = VARSIZE_ANY_EXHDR(bf);
			b->filter = (const char *) memcpy(palloc(b->filterLen + 1),
											  VARDATA_ANY(bf), b->filterLen);
		}
		result = lappend(result, b);
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	return result;
}

/*
 * ColumnarReadZoneMapVectors
 *		The per-vector zone maps (vector_index >= 0) of one row group, for
 *		per-vector skipping (native spec 7.1, Phase D5b). Only min/max and the
 *		vector/column indices are needed by the caller. The min/max bytes are
 *		copied into the current memory context.
 */
List *
ColumnarReadZoneMapVectors(uint64 storageId, uint64 groupNumber, Snapshot snapshot)
{
	Relation	rel = open_columnar_table("zone_map", AccessShareLock);
	TupleDesc	tupdesc = RelationGetDescr(rel);
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tuple;
	List	   *result = NIL;

	ScanKeyInit(&key[0], Anum_zone_map_storage_id, BTEqualStrategyNumber,
				F_INT8EQ, Int64GetDatum((int64) storageId));
	ScanKeyInit(&key[1], Anum_zone_map_group_number, BTEqualStrategyNumber,
				F_INT8EQ, Int64GetDatum((int64) groupNumber));
	scan = systable_beginscan(rel, InvalidOid, false, snapshot, 2, key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		NativeZoneMapMetadata *z;
		bool		isnull;
		int32		vecIndex;
		Datum		d;

		vecIndex = DatumGetInt32(
			heap_getattr(tuple, Anum_zone_map_vector_index, tupdesc, &isnull));
		if (vecIndex < 0)
			continue;			/* per-vector rows only */

		z = palloc0(sizeof(NativeZoneMapMetadata));
		z->storageId = storageId;
		z->groupNumber = groupNumber;
		z->columnIndex = DatumGetInt16(
			heap_getattr(tuple, Anum_zone_map_column_index, tupdesc, &isnull));
		z->vectorIndex = vecIndex;

		d = heap_getattr(tuple, Anum_zone_map_minimum, tupdesc, &isnull);
		if (!isnull)
		{
			bytea	   *bmin = DatumGetByteaPP(d);
			Datum		dmax = heap_getattr(tuple, Anum_zone_map_maximum,
											tupdesc, &isnull);

			if (!isnull)
			{
				bytea	   *bmax = DatumGetByteaPP(dmax);

				z->minimumLen = VARSIZE_ANY_EXHDR(bmin);
				z->minimum = (const char *) memcpy(palloc(z->minimumLen + 1),
												   VARDATA_ANY(bmin), z->minimumLen);
				z->maximumLen = VARSIZE_ANY_EXHDR(bmax);
				z->maximum = (const char *) memcpy(palloc(z->maximumLen + 1),
												   VARDATA_ANY(bmax), z->maximumLen);
				z->hasMinMax = true;
			}
		}
		z->valueCount = (uint64) DatumGetInt64(
			heap_getattr(tuple, Anum_zone_map_value_count, tupdesc, &isnull));
		z->nullCount = (uint64) DatumGetInt64(
			heap_getattr(tuple, Anum_zone_map_null_count, tupdesc, &isnull));
		result = lappend(result, z);
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	return result;
}

/* order native row groups by group_number */
static int
row_group_cmp(const ListCell *a, const ListCell *b)
{
	const NativeRowGroupMetadata *ra = lfirst(a);
	const NativeRowGroupMetadata *rb = lfirst(b);

	if (ra->groupNumber < rb->groupNumber)
		return -1;
	if (ra->groupNumber > rb->groupNumber)
		return 1;
	return 0;
}

/*
 * ColumnarReadRowGroupList
 *		The native row groups of a storage, ordered by group number.
 */
List *
ColumnarReadRowGroupList(uint64 storageId, Snapshot snapshot)
{
	Relation	rel = open_columnar_table("row_group", AccessShareLock);
	TupleDesc	tupdesc = RelationGetDescr(rel);
	ScanKeyData key[1];
	SysScanDesc scan;
	HeapTuple	tuple;
	List	   *result = NIL;

	ScanKeyInit(&key[0], Anum_row_group_storage_id, BTEqualStrategyNumber,
				F_INT8EQ, Int64GetDatum((int64) storageId));
	scan = systable_beginscan(rel, InvalidOid, false, snapshot, 1, key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		NativeRowGroupMetadata *rg = palloc0(sizeof(NativeRowGroupMetadata));
		bool		isnull;

		rg->storageId = storageId;
		rg->groupNumber = (uint64) DatumGetInt64(
			heap_getattr(tuple, Anum_row_group_group_number, tupdesc, &isnull));
		rg->fileOffset = (uint64) DatumGetInt64(
			heap_getattr(tuple, Anum_row_group_file_offset, tupdesc, &isnull));
		rg->rowCount = (uint64) DatumGetInt64(
			heap_getattr(tuple, Anum_row_group_row_count, tupdesc, &isnull));
		rg->byteLength = (uint64) DatumGetInt64(
			heap_getattr(tuple, Anum_row_group_byte_length, tupdesc, &isnull));
		rg->firstRowNumber = (uint64) DatumGetInt64(
			heap_getattr(tuple, Anum_row_group_first_row_number, tupdesc, &isnull));
		result = lappend(result, rg);
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	list_sort(result, row_group_cmp);
	return result;
}

/*
 * ColumnarReadColumnChunkList
 *		The native column chunks of one row group. The caller indexes the result
 *		by column_index; the encoding descriptor bytes are copied into the
 *		current memory context.
 */
List *
ColumnarReadColumnChunkList(uint64 storageId, uint64 groupNumber, Snapshot snapshot)
{
	Relation	rel = open_columnar_table("column_chunk", AccessShareLock);
	TupleDesc	tupdesc = RelationGetDescr(rel);
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tuple;
	List	   *result = NIL;

	ScanKeyInit(&key[0], Anum_column_chunk_storage_id, BTEqualStrategyNumber,
				F_INT8EQ, Int64GetDatum((int64) storageId));
	ScanKeyInit(&key[1], Anum_column_chunk_group_number, BTEqualStrategyNumber,
				F_INT8EQ, Int64GetDatum((int64) groupNumber));
	scan = systable_beginscan(rel, InvalidOid, false, snapshot, 2, key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		NativeColumnChunkMetadata *cc = palloc0(sizeof(NativeColumnChunkMetadata));
		bool		isnull;
		Datum		d;

		cc->storageId = storageId;
		cc->groupNumber = groupNumber;
		cc->columnIndex = DatumGetInt16(
			heap_getattr(tuple, Anum_column_chunk_column_index, tupdesc, &isnull));
		cc->valueCount = (uint64) DatumGetInt64(
			heap_getattr(tuple, Anum_column_chunk_value_count, tupdesc, &isnull));
		d = heap_getattr(tuple, Anum_column_chunk_encoding_descriptor, tupdesc, &isnull);
		if (!isnull)
		{
			bytea	   *b = DatumGetByteaPP(d);

			cc->encodingDescriptorLen = VARSIZE_ANY_EXHDR(b);
			cc->encodingDescriptor = (const char *)
				memcpy(palloc(cc->encodingDescriptorLen + 1),
					   VARDATA_ANY(b), cc->encodingDescriptorLen);
		}
		cc->blockCodec = DatumGetInt16(
			heap_getattr(tuple, Anum_column_chunk_block_codec, tupdesc, &isnull));
		cc->pageOffset = (uint64) DatumGetInt64(
			heap_getattr(tuple, Anum_column_chunk_page_offset, tupdesc, &isnull));
		cc->pageLength = (uint64) DatumGetInt64(
			heap_getattr(tuple, Anum_column_chunk_page_length, tupdesc, &isnull));
		result = lappend(result, cc);
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	return result;
}

/*
 * ColumnarReadZoneMapList
 *		The whole-chunk zone maps (vector_index -1) of one row group, for group
 *		skipping (native spec 7.1, Phase D5b). The caller indexes the result by
 *		column_index; the minimum/maximum bytes are copied into the current memory
 *		context. Per-vector rows (vector_index >= 0) are skipped by this reader.
 */
List *
ColumnarReadZoneMapList(uint64 storageId, uint64 groupNumber, Snapshot snapshot)
{
	Relation	rel = open_columnar_table("zone_map", AccessShareLock);
	TupleDesc	tupdesc = RelationGetDescr(rel);
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tuple;
	List	   *result = NIL;

	ScanKeyInit(&key[0], Anum_zone_map_storage_id, BTEqualStrategyNumber,
				F_INT8EQ, Int64GetDatum((int64) storageId));
	ScanKeyInit(&key[1], Anum_zone_map_group_number, BTEqualStrategyNumber,
				F_INT8EQ, Int64GetDatum((int64) groupNumber));
	scan = systable_beginscan(rel, InvalidOid, false, snapshot, 2, key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		NativeZoneMapMetadata *z;
		bool		isnull;
		int32		vecIndex;
		Datum		d;

		vecIndex = DatumGetInt32(
			heap_getattr(tuple, Anum_zone_map_vector_index, tupdesc, &isnull));
		if (vecIndex != -1)
			continue;			/* whole-chunk rows only, for group skipping */

		z = palloc0(sizeof(NativeZoneMapMetadata));
		z->storageId = storageId;
		z->groupNumber = groupNumber;
		z->columnIndex = DatumGetInt16(
			heap_getattr(tuple, Anum_zone_map_column_index, tupdesc, &isnull));
		z->vectorIndex = vecIndex;

		d = heap_getattr(tuple, Anum_zone_map_minimum, tupdesc, &isnull);
		if (!isnull)
		{
			bytea	   *bmin = DatumGetByteaPP(d);
			Datum		dmax = heap_getattr(tuple, Anum_zone_map_maximum,
											tupdesc, &isnull);

			if (!isnull)
			{
				bytea	   *bmax = DatumGetByteaPP(dmax);

				z->minimumLen = VARSIZE_ANY_EXHDR(bmin);
				z->minimum = (const char *) memcpy(palloc(z->minimumLen + 1),
												   VARDATA_ANY(bmin), z->minimumLen);
				z->maximumLen = VARSIZE_ANY_EXHDR(bmax);
				z->maximum = (const char *) memcpy(palloc(z->maximumLen + 1),
												   VARDATA_ANY(bmax), z->maximumLen);
				z->hasMinMax = true;
			}
		}

		d = heap_getattr(tuple, Anum_zone_map_sum, tupdesc, &isnull);
		if (!isnull)
		{
			z->sum = datumCopy(d, false, -1);	/* numeric is varlena */
			z->hasSum = true;
		}

		z->valueCount = (uint64) DatumGetInt64(
			heap_getattr(tuple, Anum_zone_map_value_count, tupdesc, &isnull));
		z->nullCount = (uint64) DatumGetInt64(
			heap_getattr(tuple, Anum_zone_map_null_count, tupdesc, &isnull));
		result = lappend(result, z);
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	return result;
}

/* -------------------------------------------------------------------------
 * per-table options (spec 7.4)
 * ------------------------------------------------------------------------- */

/*
 * columnar_compression_from_name
 *		Map a compression codec name to its code (spec 5). Returns -1 for an
 *		unrecognized name so the caller can fall back to the instance default.
 */
static int
columnar_compression_from_name(const char *name)
{
	if (strcmp(name, "none") == 0)
		return COLUMNAR_COMPRESSION_NONE;
	if (strcmp(name, "pglz") == 0)
		return COLUMNAR_COMPRESSION_PGLZ;
	if (strcmp(name, "lz4") == 0)
		return COLUMNAR_COMPRESSION_LZ4;
	if (strcmp(name, "zstd") == 0)
		return COLUMNAR_COMPRESSION_ZSTD;
	return -1;
}

/*
 * ColumnarReadOptions
 *		Load the per-table options row for a relation (spec 7.4) into *opts,
 *		setting a per-field "set" flag for each column that is present (not
 *		SQL NULL). Returns true when a row exists. The catalog is read with a
 *		command-id-advanced snapshot so options set earlier in this transaction
 *		take effect for subsequent writes (spec 9).
 */
bool
ColumnarReadOptions(Oid relid, ColumnarOptions *opts)
{
	Relation	rel = open_columnar_table("options", AccessShareLock);
	TupleDesc	tupdesc = RelationGetDescr(rel);
	ScanKeyData key[1];
	SysScanDesc scan;
	HeapTuple	tuple;
	Snapshot	base;
	Snapshot	snapshot;
	bool		found = false;

	memset(opts, 0, sizeof(ColumnarOptions));

	base = ActiveSnapshotSet() ? GetActiveSnapshot() : GetTransactionSnapshot();
	snapshot = ColumnarCatalogSnapshot(base);

	ScanKeyInit(&key[0], Anum_options_regclass, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(relid));

	scan = systable_beginscan(rel, InvalidOid, false, snapshot, 1, key);
	if (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		bool		isnull;
		Datum		d;

		found = true;

		d = heap_getattr(tuple, Anum_options_chunk_group_row_limit, tupdesc, &isnull);
		if (!isnull)
		{
			opts->chunkGroupRowLimitSet = true;
			opts->chunkGroupRowLimit = DatumGetInt32(d);
		}

		d = heap_getattr(tuple, Anum_options_stripe_row_limit, tupdesc, &isnull);
		if (!isnull)
		{
			opts->stripeRowLimitSet = true;
			opts->stripeRowLimit = DatumGetInt32(d);
		}

		d = heap_getattr(tuple, Anum_options_compression_level, tupdesc, &isnull);
		if (!isnull)
		{
			opts->compressionLevelSet = true;
			opts->compressionLevel = DatumGetInt32(d);
		}

		d = heap_getattr(tuple, Anum_options_compression, tupdesc, &isnull);
		if (!isnull)
		{
			int			code = columnar_compression_from_name(NameStr(*DatumGetName(d)));

			if (code >= 0)
			{
				opts->compressionSet = true;
				opts->compressionType = code;
			}
		}

		d = heap_getattr(tuple, Anum_options_format_version, tupdesc, &isnull);
		if (!isnull)
		{
			opts->formatVersionSet = true;
			opts->formatVersion = DatumGetInt32(d);
		}
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	return found;
}

/*
 * ColumnarTableFormatVersion
 *		The on-disk format a relation's writes use: 0 for the 1.0-dev line (the
 *		default, format 2.2), or the native major version (1) when the table's
 *		format_version option is set. The native writer consults this to choose
 *		the layout; until the native writer exists it is informational.
 */
int
ColumnarTableFormatVersion(Oid relid)
{
	ColumnarOptions opts;

	if (ColumnarReadOptions(relid, &opts) && opts.formatVersionSet)
		return opts.formatVersion;
	return 0;
}

/*
 * ColumnarDeleteOptions
 *		Remove a relation's per-table options row, called when the table is
 *		dropped. The options table is keyed by regclass (relation oid), not by
 *		storage id, so it is cleaned up separately from ColumnarDeleteMetadata.
 */
void
ColumnarDeleteOptions(Oid relid)
{
	Relation	rel = open_columnar_table("options", RowExclusiveLock);
	ScanKeyData key[1];
	SysScanDesc scan;
	HeapTuple	tuple;

	ScanKeyInit(&key[0], Anum_options_regclass, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(relid));

	scan = systable_beginscan(rel, InvalidOid, false, NULL, 1, key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
		CatalogTupleDelete(rel, &tuple->t_self);
	systable_endscan(scan);

	table_close(rel, RowExclusiveLock);
}

/*
 * ColumnarIsColumnarRelation
 *		Whether a relation uses the columnar table access method. The access
 *		method oid is resolved once and cached.
 */
bool
ColumnarIsColumnarRelation(Oid relid)
{
	static Oid	columnarAmOid = InvalidOid;

	if (columnarAmOid == InvalidOid)
		columnarAmOid = get_am_oid("pgcolumnar", true);

	return OidIsValid(columnarAmOid) && get_rel_relam(relid) == columnarAmOid;
}

/* -------------------------------------------------------------------------
 * projection catalog (gap 26, format 2.2)
 * ------------------------------------------------------------------------- */

/* build a smallint[] Datum from a C int16 array (empty array when n <= 0) */
static Datum
int16_array_datum(const int16 *vals, int n)
{
	ArrayType  *arr;

	if (n <= 0)
		arr = construct_empty_array(INT2OID);
	else
	{
		Datum	   *elems = palloc(sizeof(Datum) * n);
		int			i;

		for (i = 0; i < n; i++)
			elems[i] = Int16GetDatum(vals[i]);
		arr = construct_array(elems, n, INT2OID, 2, true, TYPALIGN_SHORT);
		pfree(elems);
	}
	return PointerGetDatum(arr);
}

/* read a smallint[] Datum into a palloc'd C int16 array; *len set to count */
static int16 *
int16_array_from_datum(Datum d, int *len)
{
	ArrayType  *arr = DatumGetArrayTypeP(d);
	Datum	   *elems;
	bool	   *nulls;
	int			n;
	int16	   *out;
	int			i;

	deconstruct_array(arr, INT2OID, 2, true, TYPALIGN_SHORT, &elems, &nulls, &n);
	out = (n > 0) ? palloc(sizeof(int16) * n) : NULL;
	for (i = 0; i < n; i++)
		out[i] = DatumGetInt16(elems[i]);
	*len = n;
	return out;
}

/* order a projection list by projection_id ascending (base first) */
static int
projection_cmp(const ListCell *a, const ListCell *b)
{
	const ColumnarProjection *pa = (const ColumnarProjection *) lfirst(a);
	const ColumnarProjection *pb = (const ColumnarProjection *) lfirst(b);

	if (pa->projectionId < pb->projectionId)
		return -1;
	if (pa->projectionId > pb->projectionId)
		return 1;
	return 0;
}

void
ColumnarInsertProjectionRow(const ColumnarProjection *proj)
{
	Relation	rel = open_columnar_table("projection", RowExclusiveLock);
	TupleDesc	tupdesc = RelationGetDescr(rel);
	Datum		values[Natts_projection];
	bool		nulls[Natts_projection];
	HeapTuple	tuple;

	memset(nulls, false, sizeof(nulls));
	values[Anum_projection_storage_id - 1] = Int64GetDatum((int64) proj->storageId);
	values[Anum_projection_projection_id - 1] = Int32GetDatum(proj->projectionId);
	values[Anum_projection_name - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(proj->name));
	values[Anum_projection_proj_storage_id - 1] =
		Int64GetDatum((int64) proj->projStorageId);
	values[Anum_projection_sort_key - 1] =
		int16_array_datum(proj->sortKey, proj->sortKeyLen);
	values[Anum_projection_columns - 1] =
		int16_array_datum(proj->columns, proj->columnsLen);

	tuple = heap_form_tuple(tupdesc, values, nulls);
	CatalogTupleInsert(rel, tuple);
	heap_freetuple(tuple);

	table_close(rel, RowExclusiveLock);
	CommandCounterIncrement();		/* make the row visible to later reads */
}

List *
ColumnarListProjections(uint64 storageId)
{
	Relation	rel = open_columnar_table("projection", AccessShareLock);
	TupleDesc	tupdesc = RelationGetDescr(rel);
	ScanKeyData key[1];
	SysScanDesc scan;
	HeapTuple	tuple;
	List	   *result = NIL;

	ScanKeyInit(&key[0], Anum_projection_storage_id, BTEqualStrategyNumber,
				F_INT8EQ, Int64GetDatum((int64) storageId));

	/* NULL snapshot -> catalog snapshot: sees committed rows plus this
	 * transaction's own writes after a CommandCounterIncrement (DDL semantics). */
	scan = systable_beginscan(rel, InvalidOid, false, NULL, 1, key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		ColumnarProjection *p = palloc0(sizeof(ColumnarProjection));
		bool		isnull;
		Datum		d;

		p->storageId = storageId;
		p->projectionId = DatumGetInt32(
			heap_getattr(tuple, Anum_projection_projection_id, tupdesc, &isnull));
		d = heap_getattr(tuple, Anum_projection_name, tupdesc, &isnull);
		p->name = pstrdup(NameStr(*DatumGetName(d)));
		p->projStorageId = (uint64) DatumGetInt64(
			heap_getattr(tuple, Anum_projection_proj_storage_id, tupdesc, &isnull));
		d = heap_getattr(tuple, Anum_projection_sort_key, tupdesc, &isnull);
		p->sortKey = int16_array_from_datum(d, &p->sortKeyLen);
		d = heap_getattr(tuple, Anum_projection_columns, tupdesc, &isnull);
		p->columns = int16_array_from_datum(d, &p->columnsLen);

		result = lappend(result, p);
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	list_sort(result, projection_cmp);
	return result;
}

void
ColumnarDeleteProjectionRow(uint64 storageId, int projectionId)
{
	Relation	rel = open_columnar_table("projection", RowExclusiveLock);
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tuple;

	ScanKeyInit(&key[0], Anum_projection_storage_id, BTEqualStrategyNumber,
				F_INT8EQ, Int64GetDatum((int64) storageId));
	ScanKeyInit(&key[1], Anum_projection_projection_id, BTEqualStrategyNumber,
				F_INT4EQ, Int32GetDatum(projectionId));

	scan = systable_beginscan(rel, InvalidOid, false, NULL, 2, key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
		CatalogTupleDelete(rel, &tuple->t_self);
	systable_endscan(scan);

	table_close(rel, RowExclusiveLock);
	CommandCounterIncrement();
}
