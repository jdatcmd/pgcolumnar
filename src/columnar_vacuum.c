/*-------------------------------------------------------------------------
 *
 * columnar_vacuum.c
 *		Compaction, statistics, and storage-id lookup functions for pgColumnar
 *		(spec 8.2, 9). columnar.vacuum rewrites a columnar table's live rows
 *		into fresh, full stripes: this combines many small stripes into few and
 *		physically reclaims the space of rows marked deleted in the delete vector,
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
#include "access/genam.h"
#include "access/relation.h"
#include "access/table.h"
#include "catalog/index.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
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
PG_FUNCTION_INFO_V1(columnar_cluster);
PG_FUNCTION_INFO_V1(columnar_compact);
PG_FUNCTION_INFO_V1(columnar_compact_rewrite);
PG_FUNCTION_INFO_V1(columnar_recluster);

/* Z-order helpers (defined later, used by the online recluster below) */
static bool cluster_type_supported(Oid typid);
static bytea *cluster_zorder_key(Datum *values, bool *isnull, AttrNumber *atts,
								 int ncols, TupleDesc tupdesc);

/* v1 refuses tables with more groups than this (one advisory lock per group) */
#define RECLUSTER_MAX_GROUPS 8192

static int
uint64_cmp(const void *a, const void *b)
{
	uint64		x = *(const uint64 *) a;
	uint64		y = *(const uint64 *) b;

	return (x < y) ? -1 : (x > y) ? 1 : 0;
}

/* -------------------------------------------------------------------------
 * Online rewrite of partially-deleted row groups (Phase F3b)
 *
 * Reclaims the space of deleted rows in a group that is only partially deleted
 * (F3a retires only fully-dead groups) by rewriting the group's live rows into a
 * new group with fresh row numbers, then retiring the old group -- all under
 * ShareUpdateExclusiveLock, concurrent with readers and writers. Correctness
 * rests on the protocol in design/PHASE_F3B_PLAN.md: the rewrite serializes with
 * deleters on the per-chunk-group advisory lock, and a delete that races a
 * rewrite of its group is aborted with a serialization failure by the
 * conflict check in ColumnarUpsertDeleteVector.
 * ------------------------------------------------------------------------- */

/* the relation's ready+valid indexes, opened once for a rewrite pass */
typedef struct RewriteIndexState
{
	int			n;
	Relation   *rels;
	IndexInfo **infos;
	ExprState **predicates;		/* partial-index predicate, or NULL */
	EState	   *estate;
	TupleTableSlot *slot;
} RewriteIndexState;

static void
rewrite_index_open(Relation rel, RewriteIndexState *ris)
{
	List	   *oids = RelationGetIndexList(rel);
	int			cap = Max(list_length(oids), 1);
	ListCell   *lc;

	ris->n = 0;
	ris->rels = palloc(cap * sizeof(Relation));
	ris->infos = palloc(cap * sizeof(IndexInfo *));
	ris->predicates = palloc(cap * sizeof(ExprState *));
	ris->estate = CreateExecutorState();
	ris->slot = MakeSingleTupleTableSlot(RelationGetDescr(rel), &TTSOpsVirtual);

	foreach(lc, oids)
	{
		Oid			ioid = lfirst_oid(lc);
		Relation	irel = index_open(ioid, RowExclusiveLock);
		IndexInfo  *info;

		if (!irel->rd_index->indisready || !irel->rd_index->indisvalid)
		{
			index_close(irel, RowExclusiveLock);
			continue;
		}
		info = BuildIndexInfo(irel);
		ris->rels[ris->n] = irel;
		ris->infos[ris->n] = info;
		ris->predicates[ris->n] = (info->ii_Predicate != NIL)
			? ExecPrepareQual(info->ii_Predicate, ris->estate)
			: NULL;
		ris->n++;
	}
	list_free(oids);
}

static void
rewrite_index_close(RewriteIndexState *ris)
{
	int			i;

	for (i = 0; i < ris->n; i++)
		index_close(ris->rels[i], RowExclusiveLock);
	ExecDropSingleTupleTableSlot(ris->slot);
	FreeExecutorState(ris->estate);
}

/* insert index entries for one rewritten row at its new row number */
static void
rewrite_index_insert_row(RewriteIndexState *ris, Relation rel,
						 Datum *values, bool *isnull, uint64 rowNumber)
{
	int			natts = RelationGetDescr(rel)->natts;
	ExprContext *econtext = GetPerTupleExprContext(ris->estate);
	ItemPointerData tid;
	int			i;

	ColumnarRowNumberToItemPointer(rowNumber, &tid);

	ExecClearTuple(ris->slot);
	memcpy(ris->slot->tts_values, values, natts * sizeof(Datum));
	memcpy(ris->slot->tts_isnull, isnull, natts * sizeof(bool));
	ExecStoreVirtualTuple(ris->slot);
	econtext->ecxt_scantuple = ris->slot;

	for (i = 0; i < ris->n; i++)
	{
		Datum		ivalues[INDEX_MAX_KEYS];
		bool		inulls[INDEX_MAX_KEYS];

		/* skip rows a partial index does not cover */
		if (ris->predicates[i] != NULL && !ExecQual(ris->predicates[i], econtext))
			continue;

		FormIndexDatum(ris->infos[i], ris->slot, ris->estate, ivalues, inulls);
		index_insert(ris->rels[i], ivalues, inulls, &tid, rel,
					 UNIQUE_CHECK_NO, false, ris->infos[i]);
	}
	ResetPerTupleExprContext(ris->estate);
}

/*
 * Rewrite one group's live rows into a fresh group and retire the old group.
 * Returns the number of live rows moved. Caller holds ShareUpdateExclusiveLock
 * on rel and has opened the indexes in ris.
 */
static int64
rewrite_one_group(Relation rel, RewriteIndexState *ris, uint64 storageId,
				  uint64 groupNumber, uint64 firstRow, uint64 rowCount)
{
	Oid			relid = RelationGetRelid(rel);
	int			natts = RelationGetDescr(rel)->natts;
	Datum	   *values = palloc(natts * sizeof(Datum));
	bool	   *isnull = palloc(natts * sizeof(bool));
	ColumnarWriteState *ws;
	Snapshot	snap;
	uint64		r;
	int64		moved = 0;

	/* serialize with concurrent deleters to this group (see ColumnarUpsertDeleteVector) */
	ColumnarLockChunkGroup(storageId, groupNumber);

	/* Read the group's live set under a snapshot taken after the lock, so every
	 * delete committed before the lock is reflected. Register it (not just push
	 * active): ColumnarReadRowByNumber derives a catalog snapshot copy that must
	 * inherit a nonzero regd_count for PG18's heap-visibility assertion, which a
	 * merely-active GetLatestSnapshot does not provide. */
	snap = RegisterSnapshot(GetLatestSnapshot());
	PushActiveSnapshot(snap);

	ws = ColumnarGetWriteState(rel);
	for (r = firstRow; r < firstRow + rowCount; r++)
	{
		uint64		newRn;

		CHECK_FOR_INTERRUPTS();
		if (!ColumnarReadRowByNumber(rel, snap, r, values, isnull))
			continue;			/* deleted or absent: drop it */

		newRn = ColumnarWriteRow(ws, rel, values, isnull);
		ColumnarProjectionFanoutRow(rel, ws, newRn, values, isnull);
		rewrite_index_insert_row(ris, rel, values, isnull, newRn);
		moved++;
	}
	ColumnarFlushWriteStateForRelation(relid);

	/* atomically (same transaction) the new group is now in the catalog; drop the
	 * old one. Heap MVCC keeps the old group readable to older snapshots. */
	ColumnarRetireGroup(storageId, groupNumber);

	PopActiveSnapshot();
	UnregisterSnapshot(snap);
	pfree(values);
	pfree(isnull);
	return moved;
}

/* one candidate group to rewrite */
typedef struct RewriteCandidate
{
	uint64		groupNumber;
	uint64		firstRow;
	uint64		rowCount;
} RewriteCandidate;

/*
 * columnar_rewrite_partial_groups
 *		Rewrite up to maxGroups groups whose deleted fraction is at least
 *		minDeletedFraction (and which are not fully dead -- F3a handles those).
 *		maxGroups <= 0 means all. Returns the number of groups rewritten.
 */
static int64
columnar_rewrite_partial_groups(Relation rel, double minDeletedFraction,
								int maxGroups)
{
	uint64		storageId = ColumnarStorageId(rel);
	Oid			relid = RelationGetRelid(rel);
	Snapshot	snap;
	List	   *rgList;
	ListCell   *lc;
	List	   *cands = NIL;
	RewriteIndexState ris;
	int64		rewritten = 0;

	/* persist own pending work so the group list and deletes are current */
	ColumnarFlushWriteStateForRelation(relid);
	ColumnarFlushDeleteVectorForRelation(rel);

	/* collect candidate groups first (do not mutate the catalog mid-scan) */
	snap = RegisterSnapshot(GetLatestSnapshot());
	rgList = ColumnarReadRowGroupList(storageId, snap);
	foreach(lc, rgList)
	{
		NativeRowGroupMetadata *rg = (NativeRowGroupMetadata *) lfirst(lc);
		List	   *rmList;
		ListCell   *lc2;
		int64		deleted = 0;

		if (rg->rowCount == 0)
			continue;
		rmList = ColumnarReadDeleteVectorList(storageId, rg->groupNumber, snap);
		foreach(lc2, rmList)
			deleted += ((DeleteVectorMetadata *) lfirst(lc2))->deletedCount;

		/* partially deleted and past the threshold; fully-dead is F3a's job */
		if (deleted > 0 && deleted < (int64) rg->rowCount &&
			(double) deleted / (double) rg->rowCount >= minDeletedFraction)
		{
			RewriteCandidate *c = palloc(sizeof(RewriteCandidate));

			c->groupNumber = rg->groupNumber;
			c->firstRow = rg->firstRowNumber;
			c->rowCount = rg->rowCount;
			cands = lappend(cands, c);
		}
	}
	UnregisterSnapshot(snap);

	if (cands == NIL)
		return 0;

	rewrite_index_open(rel, &ris);
	foreach(lc, cands)
	{
		RewriteCandidate *c = (RewriteCandidate *) lfirst(lc);

		if (maxGroups > 0 && rewritten >= maxGroups)
			break;
		rewrite_one_group(rel, &ris, storageId, c->groupNumber,
						  c->firstRow, c->rowCount);
		rewritten++;
	}
	rewrite_index_close(&ris);

	COLUMNAR_ASSERT_NO_OVERLAP(storageId);
	return rewritten;
}

/*
 * columnar_compact_rewrite
 *		SQL: pgcolumnar.compact_rewrite(tablename regclass,
 *			 min_deleted_fraction float8 default 0.2, max_groups int default 0).
 *		The lazy online space-reclaiming path (Phase F3b): rewrite partially
 *		deleted groups to drop their dead rows, under ShareUpdateExclusiveLock
 *		(concurrent reads and writes). Returns the number of groups rewritten.
 */
/*
 * columnar_recluster_online
 *		Re-establish global Z-order clustering over the relation's live rows
 *		online (Phase F3c): read all live rows under a snapshot taken after
 *		advisory-locking every group, Morton-sort them, write them back as fresh
 *		groups with online index maintenance, and retire the old groups in the same
 *		transaction. Holds ShareUpdateExclusiveLock (the caller's), so reads never
 *		block; deletes to the reclustered groups serialize and retry via the F3b
 *		conflict protocol. Returns the number of groups retired.
 */
static int64
columnar_recluster_online(Relation rel, int ncols, AttrNumber *atts)
{
	uint64		storageId = ColumnarStorageId(rel);
	Oid			relid = RelationGetRelid(rel);
	TupleDesc	tupdesc = RelationGetDescr(rel);
	int			natts = tupdesc->natts;
	AttrNumber	zAtt = (AttrNumber) (natts + 1);
	Snapshot	listSnap;
	List	   *rgList;
	ListCell   *lc;
	uint64	   *oldGroups;
	int			nGroups = 0;
	int			i;
	Snapshot	snap;
	TupleDesc	augdesc;
	Tuplesortstate *tsort;
	TupleTableSlot *readSlot;
	TupleTableSlot *putSlot;
	TupleTableSlot *augSlot;
	TypeCacheEntry *tce;
	Oid			byteaLt;
	Oid			sortColl = InvalidOid;
	bool		nullsFirst = false;
	ColumnarReadState *readState;
	ColumnarWriteState *writeState;
	RewriteIndexState ris;
	uint64		rowNumber;

	/* persist own pending work so the group list and deletes are current */
	ColumnarFlushWriteStateForRelation(relid);
	ColumnarFlushDeleteVectorForRelation(rel);

	/* capture the current groups (retired at the end, after the new ones exist) */
	listSnap = RegisterSnapshot(GetLatestSnapshot());
	rgList = ColumnarReadRowGroupList(storageId, listSnap);
	oldGroups = palloc(sizeof(uint64) * (list_length(rgList) > 0 ? list_length(rgList) : 1));
	foreach(lc, rgList)
	{
		NativeRowGroupMetadata *rg = (NativeRowGroupMetadata *) lfirst(lc);

		oldGroups[nGroups++] = rg->groupNumber;
	}
	UnregisterSnapshot(listSnap);

	if (nGroups == 0)
		return 0;
	if (nGroups > RECLUSTER_MAX_GROUPS)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("table has %d row groups, above the online recluster limit of %d",
						nGroups, RECLUSTER_MAX_GROUPS),
				 errhint("Use pgcolumnar.cluster() for a one-shot reorg of a very large table.")));

	/* lock every group in ascending order (deadlock-safe), held to commit */
	qsort(oldGroups, nGroups, sizeof(uint64), uint64_cmp);
	for (i = 0; i < nGroups; i++)
		ColumnarLockChunkGroup(storageId, oldGroups[i]);

	/* read all live rows into a Morton-keyed tuplesort (as in eager cluster).
	 * Register the snapshot (not just push active) so the catalog snapshot copies
	 * the reader derives satisfy PG18's heap-visibility assertion. */
	snap = RegisterSnapshot(GetLatestSnapshot());
	PushActiveSnapshot(snap);

	augdesc = CreateTemplateTupleDesc(natts + 1);
	for (i = 1; i <= natts; i++)
		TupleDescCopyEntry(augdesc, (AttrNumber) i, tupdesc, (AttrNumber) i);
	TupleDescInitEntry(augdesc, zAtt, "__zorder", BYTEAOID, -1, 0);
#if PG_VERSION_NUM >= 190000
	/* PG19 requires a manually-built TupleDesc to be finalized before use, which
	 * computes firstNonCachedOffsetAttr (asserted by the tuple routines) after the
	 * TupleDescCopyEntry calls filled the FormData array directly. */
	TupleDescFinalize(augdesc);
#endif

	tce = lookup_type_cache(BYTEAOID, TYPECACHE_LT_OPR);
	byteaLt = tce->lt_opr;
	tsort = tuplesort_begin_heap(augdesc, 1, &zAtt, &byteaLt, &sortColl,
								 &nullsFirst, maintenance_work_mem, NULL,
								 COLUMNAR_TUPLESORT_NONACCESS);

	readSlot = MakeSingleTupleTableSlot(tupdesc, &TTSOpsVirtual);
	putSlot = MakeSingleTupleTableSlot(augdesc, &TTSOpsVirtual);
	augSlot = MakeSingleTupleTableSlot(augdesc, &TTSOpsMinimalTuple);

	readState = ColumnarBeginRead(rel, snap, NULL, NULL, 0, NULL);
	while (ColumnarReadNextRow(readState, readSlot->tts_values,
							   readSlot->tts_isnull, &rowNumber))
	{
		bytea	   *zkey;

		CHECK_FOR_INTERRUPTS();
		memcpy(putSlot->tts_values, readSlot->tts_values, natts * sizeof(Datum));
		memcpy(putSlot->tts_isnull, readSlot->tts_isnull, natts * sizeof(bool));
		zkey = cluster_zorder_key(readSlot->tts_values, readSlot->tts_isnull,
								  atts, ncols, tupdesc);
		putSlot->tts_values[natts] = PointerGetDatum(zkey);
		putSlot->tts_isnull[natts] = false;
		ExecStoreVirtualTuple(putSlot);
		tuplesort_puttupleslot(tsort, putSlot);
		ExecClearTuple(putSlot);
	}
	ColumnarEndRead(readState);
	ExecDropSingleTupleTableSlot(readSlot);
	ExecDropSingleTupleTableSlot(putSlot);
	tuplesort_performsort(tsort);

	/* write the sorted rows back as fresh groups, with online index maintenance */
	rewrite_index_open(rel, &ris);
	writeState = ColumnarGetWriteState(rel);
	while (tuplesort_gettupleslot(tsort, true, false, augSlot, NULL))
	{
		uint64		newRn;

		CHECK_FOR_INTERRUPTS();
		slot_getallattrs(augSlot);
		newRn = ColumnarWriteRow(writeState, rel, augSlot->tts_values,
								 augSlot->tts_isnull);
		ColumnarProjectionFanoutRow(rel, writeState, newRn, augSlot->tts_values,
									augSlot->tts_isnull);
		rewrite_index_insert_row(&ris, rel, augSlot->tts_values,
								 augSlot->tts_isnull, newRn);
	}
	ColumnarFlushWriteStateForRelation(relid);
	rewrite_index_close(&ris);
	tuplesort_end(tsort);
	ExecDropSingleTupleTableSlot(augSlot);

	/* retire the old groups; heap MVCC keeps them readable to older snapshots */
	for (i = 0; i < nGroups; i++)
		ColumnarRetireGroup(storageId, oldGroups[i]);

	PopActiveSnapshot();
	UnregisterSnapshot(snap);
	pfree(oldGroups);

	COLUMNAR_ASSERT_NO_OVERLAP(storageId);
	return nGroups;
}

/*
 * columnar_recluster
 *		SQL: pgcolumnar.recluster(tablename regclass, VARIADIC columns name[]).
 *		The lazy online counterpart to cluster(): re-establish global Z-order
 *		clustering under ShareUpdateExclusiveLock (concurrent reads and writes),
 *		not the AccessExclusiveLock the eager cluster() reorg takes. Returns the
 *		number of groups reclustered.
 */
Datum
columnar_recluster(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	ArrayType  *colArray;
	Datum	   *colDatums;
	bool	   *colNulls;
	int			ncols;
	Relation	rel;
	TupleDesc	tupdesc;
	AttrNumber *atts;
	int64		reclustered;
	int			i;

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("table name cannot be null")));
	if (PG_ARGISNULL(1))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("at least one clustering column is required")));

	colArray = PG_GETARG_ARRAYTYPE_P(1);
	deconstruct_array(colArray, NAMEOID, NAMEDATALEN, false, 'c',
					  &colDatums, &colNulls, &ncols);
	if (ncols < 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("at least one clustering column is required")));
	if (ncols > 8)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("Z-order clustering supports at most 8 columns")));

	/* the lazy lock: concurrent reads and writes during the recluster */
	rel = table_open(relid, ShareUpdateExclusiveLock);

	if (!ColumnarIsColumnarRelation(relid))
	{
		table_close(rel, ShareUpdateExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" is not a columnar table",
						RelationGetRelationName(rel))));
	}

	tupdesc = RelationGetDescr(rel);
	atts = palloc(ncols * sizeof(AttrNumber));
	for (i = 0; i < ncols; i++)
	{
		char	   *colname;
		AttrNumber	attno;
		Form_pg_attribute att;

		if (colNulls[i])
		{
			table_close(rel, ShareUpdateExclusiveLock);
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("clustering column name cannot be null")));
		}
		colname = NameStr(*DatumGetName(colDatums[i]));
		attno = get_attnum(relid, colname);
		if (attno == InvalidAttrNumber || attno <= 0 ||
			TupleDescAttr(tupdesc, attno - 1)->attisdropped)
		{
			table_close(rel, ShareUpdateExclusiveLock);
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" does not exist in table \"%s\"",
							colname, RelationGetRelationName(rel))));
		}
		att = TupleDescAttr(tupdesc, attno - 1);
		if (!cluster_type_supported(att->atttypid))
		{
			table_close(rel, ShareUpdateExclusiveLock);
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("column \"%s\" of type %s cannot be used as a clustering key",
							colname, format_type_be(att->atttypid)),
					 errhint("Z-order clustering supports integer, date/time, boolean, and floating-point columns.")));
		}
		atts[i] = attno;
	}

	reclustered = columnar_recluster_online(rel, ncols, atts);

	table_close(rel, NoLock);
	PG_RETURN_INT64(reclustered);
}

Datum
columnar_compact_rewrite(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	double		minFrac = PG_ARGISNULL(1) ? 0.2 : PG_GETARG_FLOAT8(1);
	int			maxGroups = PG_ARGISNULL(2) ? 0 : PG_GETARG_INT32(2);
	Relation	rel;
	int64		rewritten;

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("table name cannot be null")));
	if (minFrac < 0.0 || minFrac > 1.0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("min_deleted_fraction must be between 0 and 1")));

	rel = table_open(relid, ShareUpdateExclusiveLock);

	if (!ColumnarIsColumnarRelation(relid))
	{
		table_close(rel, ShareUpdateExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" is not a columnar table",
						RelationGetRelationName(rel))));
	}

	rewritten = columnar_rewrite_partial_groups(rel, minFrac, maxGroups);

	table_close(rel, NoLock);
	PG_RETURN_INT64(rewritten);
}

/* -------------------------------------------------------------------------
 * Z-order (Morton) clustering (Phase F2)
 *
 * Space-filling-curve clustering orders rows so that rows close in a
 * multi-column key space are close on disk, which tightens every clustered
 * column's per-vector min/max zone maps at once (unlike a single-column sort,
 * which only tightens the lead column). We map each clustered column value to
 * an order-preserving unsigned 64-bit ordinal, then bit-interleave the ordinals
 * most-significant-round-first into a byte string whose memcmp order is the
 * Z-order. Rows are rewritten sorted by that key (spec 9). Clean-room from the
 * public description of Morton/Z-order codes.
 * ------------------------------------------------------------------------- */

/*
 * Map a value of a supported clustering type to an order-preserving uint64:
 * a < b (in the type's default order) iff ordinal(a) < ordinal(b). Signed
 * integers flip the sign bit; floats flip the sign bit when positive and all
 * bits when negative (the standard radix-sort float transform). NULL is mapped
 * by the caller to 0 (sorts first).
 */
static uint64
cluster_type_ordinal(Datum value, Oid typid)
{
	switch (typid)
	{
		case BOOLOID:
			return DatumGetBool(value) ? 1 : 0;
		case INT2OID:
			return (uint64) ((int64) DatumGetInt16(value)) ^ UINT64CONST(0x8000000000000000);
		case INT4OID:
		case DATEOID:
			return (uint64) ((int64) DatumGetInt32(value)) ^ UINT64CONST(0x8000000000000000);
		case INT8OID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			return (uint64) DatumGetInt64(value) ^ UINT64CONST(0x8000000000000000);
		case FLOAT8OID:
			{
				uint64		b;
				float8		f = DatumGetFloat8(value);

				memcpy(&b, &f, sizeof(b));
				return b ^ ((b >> 63) ? UINT64CONST(0xFFFFFFFFFFFFFFFF)
							: UINT64CONST(0x8000000000000000));
			}
		case FLOAT4OID:
			{
				uint32		b;
				float4		f = DatumGetFloat4(value);
				uint32		o;

				memcpy(&b, &f, sizeof(b));
				o = b ^ ((b >> 31) ? 0xFFFFFFFFu : 0x80000000u);
				return ((uint64) o) << 32;	/* keep the ordering in the high bits */
			}
		default:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("column type %s cannot be used as a clustering key",
							format_type_be(typid)),
					 errhint("Z-order clustering supports integer, date/time, boolean, and floating-point columns.")));
			return 0;				/* keep the compiler happy */
	}
}

/* true when a type is usable as a Z-order clustering key */
static bool
cluster_type_supported(Oid typid)
{
	switch (typid)
	{
		case BOOLOID:
		case INT2OID:
		case INT4OID:
		case DATEOID:
		case INT8OID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case FLOAT8OID:
		case FLOAT4OID:
			return true;
		default:
			return false;
	}
}

/*
 * Build the Z-order key for one row: interleave the ncols column ordinals
 * MSB-first into an 8*ncols-byte string. Output bit stream is
 * ord[0].bit63, ord[1].bit63, ..., ord[n-1].bit63, ord[0].bit62, ... packed
 * MSB-first, so lexicographic (memcmp) order over the bytea equals Z-order.
 */
static bytea *
cluster_zorder_key(Datum *values, bool *isnull, AttrNumber *atts, int ncols,
				   TupleDesc tupdesc)
{
	int			keybytes = ncols * 8;
	bytea	   *result = (bytea *) palloc(VARHDRSZ + keybytes);
	unsigned char *out = (unsigned char *) VARDATA(result);
	uint64	   *ord = (uint64 *) palloc(ncols * sizeof(uint64));
	int			c;
	int			r;
	int			outbit = 0;

	SET_VARSIZE(result, VARHDRSZ + keybytes);
	memset(out, 0, keybytes);

	for (c = 0; c < ncols; c++)
	{
		AttrNumber	a = atts[c];
		Form_pg_attribute att = TupleDescAttr(tupdesc, a - 1);

		ord[c] = isnull[a - 1] ? 0
			: cluster_type_ordinal(values[a - 1], att->atttypid);
	}

	for (r = 63; r >= 0; r--)
	{
		for (c = 0; c < ncols; c++)
		{
			if ((ord[c] >> r) & 1)
				out[outbit >> 3] |= (unsigned char) (0x80 >> (outbit & 7));
			outbit++;
		}
	}

	pfree(ord);
	return result;
}

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
	ColumnarFlushDeleteVectorForRelation(rel);

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
 * columnar_compact_relation_zorder
 *		Rewrite every live row of a columnar relation ordered by the Z-order
 *		(Morton) code over atts[0..ncols-1] (Phase F2). Mirrors
 *		columnar_compact_relation, but sorts by a computed key carried as a
 *		trailing bytea column of an augmented tuple, so the sort still spills to
 *		disk through tuplesort. The relation is already open AccessExclusiveLock.
 */
static void
columnar_compact_relation_zorder(Relation rel, int ncols, AttrNumber *atts)
{
	Oid			relid = RelationGetRelid(rel);
	TupleDesc	tupdesc = RelationGetDescr(rel);
	int			natts = tupdesc->natts;
	AttrNumber	zAtt = (AttrNumber) (natts + 1);
	uint64		oldStorageId;
	Snapshot	snapshot;
	ColumnarReadState *readState;
	ColumnarWriteState *writeState;
	Tuplesortstate *tsort;
	TupleDesc	augdesc;
	TupleTableSlot *readSlot;
	TupleTableSlot *putSlot;
	TupleTableSlot *augSlot;
	TypeCacheEntry *tce;
	Oid			byteaLt;
	Oid			sortColl = InvalidOid;
	bool		nullsFirst = false;
	uint64		rowNumber;
	List	   *oldProjs;
	int			i;

	/* persist pending work so the read below sees it (spec 9) */
	ColumnarFlushWriteStateForRelation(relid);
	ColumnarFlushDeleteVectorForRelation(rel);

	oldStorageId = ColumnarStorageId(rel);
	oldProjs = ColumnarListProjections(oldStorageId);
	snapshot = ActiveSnapshotSet() ? GetActiveSnapshot() : GetTransactionSnapshot();

	/* augmented descriptor: the table's columns plus a trailing bytea Z-order key */
	augdesc = CreateTemplateTupleDesc(natts + 1);
	for (i = 1; i <= natts; i++)
		TupleDescCopyEntry(augdesc, (AttrNumber) i, tupdesc, (AttrNumber) i);
	TupleDescInitEntry(augdesc, zAtt, "__zorder", BYTEAOID, -1, 0);
#if PG_VERSION_NUM >= 190000
	/* PG19 requires a manually-built TupleDesc to be finalized before use, which
	 * computes firstNonCachedOffsetAttr (asserted by the tuple routines) after the
	 * TupleDescCopyEntry calls filled the FormData array directly. */
	TupleDescFinalize(augdesc);
#endif

	tce = lookup_type_cache(BYTEAOID, TYPECACHE_LT_OPR);
	byteaLt = tce->lt_opr;
	tsort = tuplesort_begin_heap(augdesc, 1, &zAtt, &byteaLt, &sortColl,
								 &nullsFirst, maintenance_work_mem, NULL,
								 COLUMNAR_TUPLESORT_NONACCESS);

	readSlot = MakeSingleTupleTableSlot(tupdesc, &TTSOpsVirtual);
	putSlot = MakeSingleTupleTableSlot(augdesc, &TTSOpsVirtual);
	augSlot = MakeSingleTupleTableSlot(augdesc, &TTSOpsMinimalTuple);

	readState = ColumnarBeginRead(rel, snapshot, NULL, NULL, 0, NULL);
	while (ColumnarReadNextRow(readState, readSlot->tts_values,
							   readSlot->tts_isnull, &rowNumber))
	{
		bytea	   *zkey;

		CHECK_FOR_INTERRUPTS();
		memcpy(putSlot->tts_values, readSlot->tts_values, natts * sizeof(Datum));
		memcpy(putSlot->tts_isnull, readSlot->tts_isnull, natts * sizeof(bool));
		zkey = cluster_zorder_key(readSlot->tts_values, readSlot->tts_isnull,
								  atts, ncols, tupdesc);
		putSlot->tts_values[natts] = PointerGetDatum(zkey);
		putSlot->tts_isnull[natts] = false;
		ExecStoreVirtualTuple(putSlot);
		tuplesort_puttupleslot(tsort, putSlot);
		ExecClearTuple(putSlot);
	}
	ColumnarEndRead(readState);
	ExecDropSingleTupleTableSlot(readSlot);
	ExecDropSingleTupleTableSlot(putSlot);
	tuplesort_performsort(tsort);

	/* swap to fresh storage and drop old metadata (as in compact_relation) */
	RelationSetNewRelfilenumber(rel, rel->rd_rel->relpersistence);
	ColumnarForgetWriteStateForRelation(relid);
	ColumnarDeleteMetadata(oldStorageId);

	/* realign projections to the compacted base (as in compact_relation) */
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

	/* write the live rows back in Z-order; the trailing key column is ignored */
	writeState = ColumnarGetWriteState(rel);
	while (tuplesort_gettupleslot(tsort, true, false, augSlot, NULL))
	{
		uint64		newRowNumber;

		CHECK_FOR_INTERRUPTS();
		slot_getallattrs(augSlot);
		newRowNumber = ColumnarWriteRow(writeState, rel, augSlot->tts_values,
										augSlot->tts_isnull);
		ColumnarProjectionFanoutRow(rel, writeState, newRowNumber,
									augSlot->tts_values, augSlot->tts_isnull);
		ExecClearTuple(augSlot);
	}
	ColumnarFlushWriteStateForRelation(relid);
	tuplesort_end(tsort);
	ExecDropSingleTupleTableSlot(augSlot);

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

/*
 * columnar_cluster
 *		SQL: pgcolumnar.cluster(tablename regclass, VARIADIC columns name[]).
 *		Physically reorders a columnar table by the Z-order (Morton) space-filling
 *		curve over the named columns (Phase F2, spec 9). Unlike vacuum_sorted's
 *		single lead-column sort, Z-order clustering tightens the min/max zone maps
 *		of ALL clustered columns at once, so multi-column range and point
 *		predicates skip far more vectors and chunks. Results are unchanged; this
 *		only reorders physical storage.
 *
 *		This is the EAGER / offline reorg: it rewrites the whole relation and
 *		swaps the relfilenode, so it holds AccessExclusiveLock for the duration
 *		(like PostgreSQL's own CLUSTER / VACUUM FULL) and blocks concurrent reads
 *		and writes. Use it for an initial bulk reorg. The routine, online path that
 *		reclusters incrementally under ShareUpdateExclusiveLock (concurrent reads
 *		and writes allowed) is Phase F3; every maintenance op must offer that lazy
 *		path.
 */
Datum
columnar_cluster(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	ArrayType  *colArray;
	Datum	   *colDatums;
	bool	   *colNulls;
	int			ncols;
	Relation	rel;
	TupleDesc	tupdesc;
	AttrNumber *atts;
	int			i;

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("table name cannot be null")));
	if (PG_ARGISNULL(1))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("at least one clustering column is required")));

	colArray = PG_GETARG_ARRAYTYPE_P(1);
	deconstruct_array(colArray, NAMEOID, NAMEDATALEN, false, 'c',
					  &colDatums, &colNulls, &ncols);
	if (ncols < 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("at least one clustering column is required")));
	if (ncols > 8)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("Z-order clustering supports at most 8 columns")));

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
	atts = palloc(ncols * sizeof(AttrNumber));

	for (i = 0; i < ncols; i++)
	{
		char	   *colname;
		AttrNumber	attno;
		Form_pg_attribute att;

		if (colNulls[i])
		{
			table_close(rel, AccessExclusiveLock);
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("clustering column name cannot be null")));
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

		att = TupleDescAttr(tupdesc, attno - 1);
		if (!cluster_type_supported(att->atttypid))
		{
			table_close(rel, AccessExclusiveLock);
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("column \"%s\" of type %s cannot be used as a clustering key",
							colname, format_type_be(att->atttypid)),
					 errhint("Z-order clustering supports integer, date/time, boolean, and floating-point columns.")));
		}
		atts[i] = attno;
	}

	columnar_compact_relation_zorder(rel, ncols, atts);

	/* keep the lock until end of transaction */
	table_close(rel, NoLock);

	PG_RETURN_VOID();
}

/*
 * columnar_compact
 *		SQL: pgcolumnar.compact(tablename regclass) -> bigint. The LAZY / online
 *		maintenance path (Phase F3a): retire every row group that is fully deleted
 *		as-of the oldest-xmin horizon, dropping its catalog rows so scans no longer
 *		read it. Runs under ShareUpdateExclusiveLock, so it is concurrent with
 *		readers and writers -- unlike vacuum / cluster, it never takes
 *		AccessExclusiveLock. Returns the number of groups retired. Physical page
 *		reclaim of retired groups is deferred; run the eager vacuum to reclaim the
 *		file, or a later F3 pass. Rewriting partially-deleted groups online is
 *		Phase F3b.
 */
Datum
columnar_compact(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	rel;
	int64		retired;

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("table name cannot be null")));

	/* the lazy lock: concurrent reads and writes are allowed during compaction */
	rel = table_open(relid, ShareUpdateExclusiveLock);

	if (!ColumnarIsColumnarRelation(relid))
	{
		table_close(rel, ShareUpdateExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" is not a columnar table",
						RelationGetRelationName(rel))));
	}

	retired = ColumnarRetireFullyDeletedGroups(rel);

	COLUMNAR_ASSERT_NO_OVERLAP(ColumnarStorageId(rel));

	/* keep the lock until end of transaction */
	table_close(rel, NoLock);

	PG_RETURN_INT64(retired);
}
