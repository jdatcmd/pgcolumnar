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
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/relcache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/tuplesort.h"
#include "utils/tuplestore.h"
#include "utils/typcache.h"

PG_FUNCTION_INFO_V1(columnar_relation_storageid);
PG_FUNCTION_INFO_V1(columnar_vacuum);
PG_FUNCTION_INFO_V1(columnar_vacuum_sorted);

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
 *
 *		When nsortkeys == 0 the rows are rewritten in their existing order
 *		(plain compaction). When nsortkeys > 0 the live rows are first sorted
 *		ascending / NULLS LAST on the attributes in sortAtts[] (in order), so
 *		the table is stored physically sorted on that key (gap 26, piece 1).
 *		Sorting is a one-shot physical reorder; it is not persisted or
 *		auto-maintained, so rows inserted afterward append in insert order.
 */
static void
columnar_compact_relation(Relation rel, int nsortkeys, AttrNumber *sortAtts)
{
	Oid			relid = RelationGetRelid(rel);
	TupleDesc	tupdesc = RelationGetDescr(rel);
	uint64		oldStorageId;
	Snapshot	snapshot;
	ColumnarReadState *readState;
	ColumnarWriteState *writeState;
	Tuplestorestate *tstore = NULL;
	Tuplesortstate *tsort = NULL;
	TupleTableSlot *readSlot;
	TupleTableSlot *writeSlot;
	uint64		rowNumber;

	List	   *oldProjs;

	/* persist any pending work so the read below sees it (spec 9) */
	ColumnarFlushWriteStateForRelation(relid);
	ColumnarFlushRowMaskForRelation(rel);

	oldStorageId = ColumnarStorageId(rel);

	/*
	 * Capture the table's projections (gap 26) before the storage swap. Compaction
	 * reassigns base row numbers, so each projection must be rebuilt from the
	 * compacted base; we re-record the definitions under the new storage id below
	 * and the rewrite loop re-fans-out every live row into them.
	 */
	oldProjs = ColumnarListProjections(oldStorageId);

	snapshot = ActiveSnapshotSet() ? GetActiveSnapshot() : GetTransactionSnapshot();

	/*
	 * Read every live row (the reader skips row-mask-deleted rows) and
	 * materialize it, copying values out of the scan so they survive the
	 * storage swap below. A virtual slot receives the reader's values. Without a
	 * sort key the rows go into a tuplestore in read order; with a sort key they
	 * go into a tuplesort so they come back ordered. Either way a minimal-tuple
	 * slot reads them back.
	 */
	if (nsortkeys > 0)
	{
		Oid		   *sortOps = palloc(nsortkeys * sizeof(Oid));
		Oid		   *sortColls = palloc(nsortkeys * sizeof(Oid));
		bool	   *nullsFirst = palloc0(nsortkeys * sizeof(bool));
		int			i;

		for (i = 0; i < nsortkeys; i++)
		{
			Form_pg_attribute att = TupleDescAttr(tupdesc, sortAtts[i] - 1);
			TypeCacheEntry *tce = lookup_type_cache(att->atttypid,
													TYPECACHE_LT_OPR);

			if (!OidIsValid(tce->lt_opr))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_FUNCTION),
						 errmsg("column \"%s\" of type %s has no default ordering operator",
								NameStr(att->attname),
								format_type_be(att->atttypid)),
						 errhint("Sorting requires a type with a default btree operator class.")));

			sortOps[i] = tce->lt_opr;
			sortColls[i] = att->attcollation;
			/* ascending, NULLS LAST (PostgreSQL's default for ASC) */
			nullsFirst[i] = false;
		}

		tsort = tuplesort_begin_heap(tupdesc, nsortkeys, sortAtts, sortOps,
									 sortColls, nullsFirst, maintenance_work_mem,
									 NULL, COLUMNAR_TUPLESORT_NONACCESS);
	}
	else
		tstore = tuplestore_begin_heap(false, false, work_mem);

	readSlot = MakeSingleTupleTableSlot(tupdesc, &TTSOpsVirtual);
	writeSlot = MakeSingleTupleTableSlot(tupdesc, &TTSOpsMinimalTuple);

	readState = ColumnarBeginRead(rel, snapshot, NULL, NULL, 0, NULL);
	while (ColumnarReadNextRow(readState, readSlot->tts_values,
							   readSlot->tts_isnull, &rowNumber))
	{
		CHECK_FOR_INTERRUPTS();
		ExecStoreVirtualTuple(readSlot);
		if (tsort != NULL)
			tuplesort_puttupleslot(tsort, readSlot);
		else
			tuplestore_puttupleslot(tstore, readSlot);
		ExecClearTuple(readSlot);
	}
	ColumnarEndRead(readState);
	ExecDropSingleTupleTableSlot(readSlot);

	if (tsort != NULL)
		tuplesort_performsort(tsort);

	/*
	 * Swap to a brand-new relfilenode. This creates fresh, empty columnar
	 * storage (a new metapage with a new storage id) transactionally; the old
	 * storage is discarded at commit. Then forget the cached write state (it
	 * still points at the old storage id) and remove the old metadata rows.
	 */
	RelationSetNewRelfilenumber(rel, rel->rd_rel->relpersistence);
	ColumnarForgetWriteStateForRelation(relid);
	ColumnarDeleteMetadata(oldStorageId);

	/*
	 * Realign projections to the compacted base (gap 26): drop each old
	 * projection's storage and its now-stale catalog row (keyed by the old base
	 * storage id), then re-record the definitions under the new base storage id
	 * with fresh projection storage ids. The rewrite loop below re-fans-out every
	 * live row into them, so they end up sorted and aligned to the new row
	 * numbers. No-op for a table without projections.
	 */
	if (oldProjs != NIL)
	{
		uint64		newStorageId = ColumnarStorageId(rel);
		ListCell   *lc;

		foreach(lc, oldProjs)
		{
			ColumnarProjection *p = (ColumnarProjection *) lfirst(lc);

			if (p->projStorageId != oldStorageId)
				ColumnarDeleteMetadata(p->projStorageId);
			ColumnarDeleteProjectionRow(oldStorageId, p->projectionId);
		}
		foreach(lc, oldProjs)
		{
			ColumnarProjection *p = (ColumnarProjection *) lfirst(lc);
			ColumnarProjection np = *p;

			np.storageId = newStorageId;
			np.projStorageId = (p->projectionId == 0) ? newStorageId
				: ColumnarNextStorageId();
			ColumnarInsertProjectionRow(&np);
		}
	}

	/* write the live rows back into the fresh storage, in sorted order if any */
	writeState = ColumnarGetWriteState(rel);
	if (tsort != NULL)
	{
		while (tuplesort_gettupleslot(tsort, true, false, writeSlot, NULL))
		{
			uint64		newRowNumber;

			CHECK_FOR_INTERRUPTS();
			slot_getallattrs(writeSlot);
			newRowNumber = ColumnarWriteRow(writeState, rel, writeSlot->tts_values,
											writeSlot->tts_isnull);
			ColumnarProjectionFanoutRow(rel, writeState, newRowNumber,
										writeSlot->tts_values, writeSlot->tts_isnull);
			ExecClearTuple(writeSlot);
		}
	}
	else
	{
		while (tuplestore_gettupleslot(tstore, true, false, writeSlot))
		{
			uint64		newRowNumber;

			CHECK_FOR_INTERRUPTS();
			slot_getallattrs(writeSlot);
			newRowNumber = ColumnarWriteRow(writeState, rel, writeSlot->tts_values,
											writeSlot->tts_isnull);
			ColumnarProjectionFanoutRow(rel, writeState, newRowNumber,
										writeSlot->tts_values, writeSlot->tts_isnull);
			ExecClearTuple(writeSlot);
		}
	}
	ColumnarFlushWriteStateForRelation(relid);

	if (tsort != NULL)
		tuplesort_end(tsort);
	else
		tuplestore_end(tstore);
	ExecDropSingleTupleTableSlot(writeSlot);

	/*
	 * Rewrite assigned fresh row numbers, so rebuild the indexes to repoint
	 * their synthetic item pointers (spec 6). A relation with no indexes is a
	 * no-op here.
	 */
	ColumnarReindexRelation(relid, REINDEX_REL_PROCESS_TOAST);
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

	columnar_compact_relation(rel, 0, NULL);

	/* keep the lock until end of transaction */
	table_close(rel, NoLock);

	PG_RETURN_VOID();
}

/*
 * columnar_vacuum_sorted
 *		SQL: columnar.vacuum_sorted(tablename regclass, VARIADIC sort_columns name[]).
 *		Like columnar.vacuum, but rewrites the live rows physically sorted
 *		ascending / NULLS LAST on the named columns, in order (gap 26, piece 1).
 *		Storing the table sorted on a key tightens per-chunk-group min/max ranges
 *		and helps the RLE/DELTA encodings on that key, so range predicates and
 *		ordered scans skip far more chunk groups. Results are unchanged; this only
 *		reorders physical storage. It is a one-shot reorder (not auto-maintained).
 */
Datum
columnar_vacuum_sorted(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	ArrayType  *colArray;
	Datum	   *colDatums;
	bool	   *colNulls;
	int			ncols;
	Relation	rel;
	TupleDesc	tupdesc;
	AttrNumber *sortAtts;
	int			i;

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("table name cannot be null")));
	if (PG_ARGISNULL(1))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("at least one sort column is required")));

	colArray = PG_GETARG_ARRAYTYPE_P(1);
	deconstruct_array(colArray, NAMEOID, NAMEDATALEN, false, 'c',
					  &colDatums, &colNulls, &ncols);
	if (ncols < 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("at least one sort column is required")));

	rel = table_open(relid, AccessExclusiveLock);

	if (!ColumnarIsColumnarRelation(relid))
	{
		table_close(rel, AccessExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" is not a columnar table",
						RelationGetRelationName(rel))));
	}

	tupdesc = RelationGetDescr(rel);
	sortAtts = palloc(ncols * sizeof(AttrNumber));

	for (i = 0; i < ncols; i++)
	{
		char	   *colname;
		AttrNumber	attno;

		if (colNulls[i])
		{
			table_close(rel, AccessExclusiveLock);
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("sort column name cannot be null")));
		}

		colname = NameStr(*DatumGetName(colDatums[i]));
		attno = get_attnum(relid, colname);
		if (attno == InvalidAttrNumber || attno <= 0 ||
			TupleDescAttr(tupdesc, attno - 1)->attisdropped)
		{
			table_close(rel, AccessExclusiveLock);
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" does not exist in table \"%s\"",
							colname, RelationGetRelationName(rel))));
		}
		sortAtts[i] = attno;
	}

	columnar_compact_relation(rel, ncols, sortAtts);

	/* keep the lock until end of transaction */
	table_close(rel, NoLock);

	PG_RETURN_VOID();
}
