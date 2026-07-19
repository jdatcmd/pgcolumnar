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
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "commands/sequence.h"
#include "utils/builtins.h"
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
#define Natts_chunk 14

/* attribute numbers for columnar.chunk_group (spec 7.3) */
#define Anum_chunk_group_storage_id 1
#define Anum_chunk_group_stripe_num 2
#define Anum_chunk_group_chunk_group_num 3
#define Anum_chunk_group_row_count 4
#define Anum_chunk_group_deleted_rows 5
#define Natts_chunk_group 5

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

	/* Phase 1 stores no min/max skip list (spec 7.2 allows their absence) */
	nulls[Anum_chunk_minimum_value - 1] = true;
	nulls[Anum_chunk_maximum_value - 1] = true;

	values[Anum_chunk_value_stream_offset - 1] = Int64GetDatum((int64) chunk->valueStreamOffset);
	values[Anum_chunk_value_stream_length - 1] = Int64GetDatum((int64) chunk->valueStreamLength);
	values[Anum_chunk_exists_stream_offset - 1] = Int64GetDatum((int64) chunk->existsStreamOffset);
	values[Anum_chunk_exists_stream_length - 1] = Int64GetDatum((int64) chunk->existsStreamLength);
	values[Anum_chunk_value_compression_type - 1] = Int32GetDatum(chunk->valueCompressionType);
	values[Anum_chunk_value_compression_level - 1] = Int32GetDatum(chunk->valueCompressionLevel);
	values[Anum_chunk_value_decompressed_length - 1] = Int64GetDatum((int64) chunk->valueDecompressedLength);
	values[Anum_chunk_value_count - 1] = Int64GetDatum((int64) chunk->valueCount);

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

		result = lappend(result, chunk);
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	return result;
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
	delete_rows_by_storage_id("stripe", Anum_stripe_storage_id, storageId);
}
