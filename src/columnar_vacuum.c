/*-------------------------------------------------------------------------
 *
 * columnar_vacuum.c
 *		Compaction, statistics, and storage-id lookup functions for pgColumnar
 *		(spec 8.2, 9). columnar.vacuum rewrites a columnar table's live rows
 *		into fresh, full stripes: this combines many small stripes into few and
 *		physically reclaims the space of rows marked deleted in the row mask,
 *		since deleted rows are simply not read back. columnar.stats reports the
 *		per-stripe layout (implemented in SQL over the catalog, using the
 *		get_storage_id function here to resolve a relation to its storage id).
 *
 * The rewrite swaps the relation to a new relfilenode (RelationSetNewRelfile-
 * number), which is transactional: on rollback the original storage remains.
 * Because compaction assigns fresh row numbers, indexes are rebuilt afterward
 * so their synthetic item pointers (spec 6) address the new rows.
 *
 * Independent MIT implementation built from design/FORMAT_AND_INTERFACE_SPEC.md
 * (format 2.0), design/REWRITE_PLAN.md section 6, and the public PostgreSQL 17
 * API only.
 *
 *-------------------------------------------------------------------------
 */
#include "columnar.h"

#include "fmgr.h"
#include "access/relation.h"
#include "access/table.h"
#include "catalog/index.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "storage/lockdefs.h"
#include "utils/builtins.h"
#include "utils/relcache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/tuplestore.h"

PG_FUNCTION_INFO_V1(columnar_relation_storageid);
PG_FUNCTION_INFO_V1(columnar_vacuum);

/*
 * columnar_relation_storageid
 *		SQL: columnar.get_storage_id(regclass) -> bigint. Reads the relation's
 *		metapage and returns its storage id (spec 3), so SQL-level functions
 *		such as columnar.stats can join the metadata catalog by storage id.
 */
Datum
columnar_relation_storageid(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	rel;
	uint64		storageId;

	rel = try_relation_open(relid, AccessShareLock);
	if (rel == NULL)
		PG_RETURN_NULL();

	if (!ColumnarIsColumnarRelation(relid))
	{
		relation_close(rel, AccessShareLock);
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" is not a columnar table",
						RelationGetRelationName(rel))));
	}

	storageId = ColumnarStorageId(rel);
	relation_close(rel, AccessShareLock);

	PG_RETURN_INT64((int64) storageId);
}

/*
 * columnar_compact_relation
 *		Rewrite every live row of a columnar relation into fresh stripes. The
 *		relation is already open with AccessExclusiveLock.
 */
static void
columnar_compact_relation(Relation rel)
{
	Oid			relid = RelationGetRelid(rel);
	TupleDesc	tupdesc = RelationGetDescr(rel);
	uint64		oldStorageId;
	Snapshot	snapshot;
	ColumnarReadState *readState;
	ColumnarWriteState *writeState;
	Tuplestorestate *tstore;
	TupleTableSlot *readSlot;
	TupleTableSlot *writeSlot;
	uint64		rowNumber;
	ReindexParams reindexParams = {0};

	/* persist any pending work so the read below sees it (spec 9) */
	ColumnarFlushWriteStateForRelation(relid);
	ColumnarFlushRowMaskForRelation(rel);

	oldStorageId = ColumnarStorageId(rel);

	snapshot = ActiveSnapshotSet() ? GetActiveSnapshot() : GetTransactionSnapshot();

	/*
	 * Read every live row (the reader skips row-mask-deleted rows) and
	 * materialize it into a tuplestore, copying values out of the scan so they
	 * survive the storage swap below. A virtual slot receives the reader's
	 * values; the tuplestore stores minimal tuples, so a separate minimal-tuple
	 * slot is used to read them back.
	 */
	tstore = tuplestore_begin_heap(false, false, work_mem);
	readSlot = MakeSingleTupleTableSlot(tupdesc, &TTSOpsVirtual);
	writeSlot = MakeSingleTupleTableSlot(tupdesc, &TTSOpsMinimalTuple);

	readState = ColumnarBeginRead(rel, snapshot, NULL, NULL, 0, NULL);
	while (ColumnarReadNextRow(readState, readSlot->tts_values,
							   readSlot->tts_isnull, &rowNumber))
	{
		CHECK_FOR_INTERRUPTS();
		ExecStoreVirtualTuple(readSlot);
		tuplestore_puttupleslot(tstore, readSlot);
		ExecClearTuple(readSlot);
	}
	ColumnarEndRead(readState);
	ExecDropSingleTupleTableSlot(readSlot);

	/*
	 * Swap to a brand-new relfilenode. This creates fresh, empty columnar
	 * storage (a new metapage with a new storage id) transactionally; the old
	 * storage is discarded at commit. Then forget the cached write state (it
	 * still points at the old storage id) and remove the old metadata rows.
	 */
	RelationSetNewRelfilenumber(rel, rel->rd_rel->relpersistence);
	ColumnarForgetWriteStateForRelation(relid);
	ColumnarDeleteMetadata(oldStorageId);

	/* write the live rows back into the fresh storage */
	writeState = ColumnarGetWriteState(rel);
	while (tuplestore_gettupleslot(tstore, true, false, writeSlot))
	{
		CHECK_FOR_INTERRUPTS();
		slot_getallattrs(writeSlot);
		(void) ColumnarWriteRow(writeState, rel, writeSlot->tts_values,
								writeSlot->tts_isnull);
		ExecClearTuple(writeSlot);
	}
	ColumnarFlushWriteStateForRelation(relid);

	tuplestore_end(tstore);
	ExecDropSingleTupleTableSlot(writeSlot);

	/*
	 * Rewrite assigned fresh row numbers, so rebuild the indexes to repoint
	 * their synthetic item pointers (spec 6). A relation with no indexes is a
	 * no-op here.
	 */
	reindex_relation(NULL, relid, REINDEX_REL_PROCESS_TOAST, &reindexParams);
}

/*
 * columnar_vacuum
 *		SQL: columnar.vacuum(tablename regclass, stripe_count int default 0).
 *		Compacts a columnar table by combining its stripes and reclaiming the
 *		space of deleted rows (spec 8.2, 9). stripe_count is accepted for
 *		interface compatibility; this implementation always rewrites the whole
 *		relation into full stripes, which is the strongest form of the "combine
 *		recent stripes" contract.
 */
Datum
columnar_vacuum(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	rel;

	rel = table_open(relid, AccessExclusiveLock);

	if (!ColumnarIsColumnarRelation(relid))
	{
		table_close(rel, AccessExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" is not a columnar table",
						RelationGetRelationName(rel))));
	}

	columnar_compact_relation(rel);

	/* keep the lock until end of transaction */
	table_close(rel, NoLock);

	PG_RETURN_VOID();
}
