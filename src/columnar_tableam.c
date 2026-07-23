/*-------------------------------------------------------------------------
 *
 * columnar_tableam.c
 *		Table access method handler for pgColumnar and extension glue:
 *		GUCs, the pre-commit flush hook, and drop-time metadata cleanup.
 *
 * Implements the subset of TableAmRoutine built through phase 3: create, bulk
 * insert, sequential scan, delete and update via the delete vector, fetch by tid,
 * size estimation, and non-transactional truncate. Index, vacuum, and sample
 * callbacks are stubbed for later phases.
 *
 *-------------------------------------------------------------------------
 */
#include "columnar.h"

#include "access/multixact.h"
#include "access/relation.h"
#include "access/relscan.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_class.h"
#include "catalog/storage.h"
#include "commands/defrem.h"
#include "commands/vacuum.h"
#include "executor/executor.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "nodes/pathnodes.h"
#include "optimizer/pathnode.h"
#include "optimizer/plancat.h"
#include "port/atomics.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/smgr.h"
#include "storage/spin.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

PG_MODULE_MAGIC;

/* GUC-backed instance defaults (spec 8.3) */
int			columnar_stripe_row_limit = 150000;
int			columnar_chunk_group_row_limit = 10000;

int			columnar_compression = COLUMNAR_COMPRESSION_ZSTD;
int			columnar_compression_level = 3;
bool		columnar_enable_qual_pushdown = true;
bool		columnar_enable_bloom_filter = true;

/* value set for columnar.compression (spec 5, 8.3) */
static const struct config_enum_entry columnar_compression_options[] = {
	{"none", COLUMNAR_COMPRESSION_NONE, false},
	{"pglz", COLUMNAR_COMPRESSION_PGLZ, false},
	{"lz4", COLUMNAR_COMPRESSION_LZ4, false},
	{"zstd", COLUMNAR_COMPRESSION_ZSTD, false},
	{NULL, 0, false}
};

/* forward declaration of the AM routine so hooks can compare against it */
static const TableAmRoutine columnar_am_methods;

static object_access_hook_type prev_object_access_hook = NULL;
static ExecutorEnd_hook_type prev_executor_end_hook = NULL;
#if PG_VERSION_NUM >= 190000
static build_simple_rel_hook_type prev_build_simple_rel_hook = NULL;
#else
static get_relation_info_hook_type prev_get_relation_info_hook = NULL;
#endif

/* cached OID of the "columnar" table access method (index-only-scan hook) */
static Oid	columnar_am_oid_cache = InvalidOid;

/* our scan descriptor wraps the base scan and the reader state */
typedef struct ColumnarScanDescData
{
	TableScanDescData rs_base;
	ColumnarReadState *readState;
} ColumnarScanDescData;
typedef struct ColumnarScanDescData *ColumnarScanDesc;

PG_FUNCTION_INFO_V1(columnar_handler);

/* -------------------------------------------------------------------------
 * slot / scan callbacks
 * ------------------------------------------------------------------------- */

static const TupleTableSlotOps *
columnar_slot_callbacks(Relation relation)
{
	return &TTSOpsVirtual;
}

static TableScanDesc
columnar_scan_begin(Relation rel, Snapshot snapshot, int nkeys,
					ScanKey key, ParallelTableScanDesc pscan, uint32 flags)
{
	ColumnarScanDesc scan;

	RelationIncrementReferenceCount(rel);

	/*
	 * Persist any data and delete marks written earlier in this transaction so
	 * they reach the catalog before this scan reads it. The reader consults the
	 * catalog with a command-id-advanced snapshot (ColumnarCatalogSnapshot), so
	 * these become visible to this same scan: same-transaction read-your-writes
	 * (spec 9).
	 */
	ColumnarFlushWriteStateForRelation(RelationGetRelid(rel));
	ColumnarFlushDeleteVectorForRelation(rel);

	scan = (ColumnarScanDesc) palloc0(sizeof(ColumnarScanDescData));
	scan->rs_base.rs_rd = rel;
	scan->rs_base.rs_snapshot = snapshot;
	scan->rs_base.rs_nkeys = nkeys;
	scan->rs_base.rs_flags = flags;
	scan->rs_base.rs_parallel = pscan;

	/*
	 * Phase 2 projects all columns for a plain sequential scan (there is no
	 * per-scan projection channel in the table AM without the custom scan of
	 * a later phase), so we pass a NULL projection set. Any ScanKeys the
	 * executor supplies are forwarded for chunk-group skipping.
	 */
	scan->readState = ColumnarBeginRead(rel, snapshot, pscan, NULL, nkeys, key);

	return (TableScanDesc) scan;
}

static void
columnar_scan_end(TableScanDesc sscan)
{
	ColumnarScanDesc scan = (ColumnarScanDesc) sscan;

	ColumnarEndRead(scan->readState);

	/* release a snapshot restored+registered for a parallel worker */
	if (scan->rs_base.rs_flags & SO_TEMP_SNAPSHOT)
		UnregisterSnapshot(scan->rs_base.rs_snapshot);

	RelationDecrementReferenceCount(scan->rs_base.rs_rd);
	pfree(scan);
}

static void
columnar_scan_rescan(TableScanDesc sscan, ScanKey key, bool set_params,
					 bool allow_strat, bool allow_sync, bool allow_pagemode)
{
	ColumnarScanDesc scan = (ColumnarScanDesc) sscan;

	ColumnarRescanRead(scan->readState);
}

static bool
columnar_scan_getnextslot(TableScanDesc sscan, ScanDirection direction,
						  TupleTableSlot *slot)
{
	ColumnarScanDesc scan = (ColumnarScanDesc) sscan;
	uint64		rowNumber;

	ExecClearTuple(slot);

	if (!ColumnarReadNextRow(scan->readState, slot->tts_values,
							 slot->tts_isnull, &rowNumber))
		return false;

	ExecStoreVirtualTuple(slot);
	ColumnarRowNumberToItemPointer(rowNumber, &slot->tts_tid);
	slot->tts_tableOid = RelationGetRelid(scan->rs_base.rs_rd);

	return true;
}

/* -------------------------------------------------------------------------
 * parallel scan: single-worker claim (see columnar_reader.c)
 * ------------------------------------------------------------------------- */

static Size
columnar_parallelscan_estimate(Relation rel)
{
	return sizeof(ParallelBlockTableScanDescData);
}

static Size
columnar_parallelscan_initialize(Relation rel, ParallelTableScanDesc pscan)
{
	ParallelBlockTableScanDesc bpscan = (ParallelBlockTableScanDesc) pscan;

	memset(bpscan, 0, sizeof(ParallelBlockTableScanDescData));
	COLUMNAR_PARALLELSCAN_SET_REL(bpscan, rel);
	bpscan->phs_nblocks = 0;
	SpinLockInit(&bpscan->phs_mutex);
	bpscan->phs_startblock = InvalidBlockNumber;
	pg_atomic_init_u64(&bpscan->phs_nallocated, 0);

	return sizeof(ParallelBlockTableScanDescData);
}

static void
columnar_parallelscan_reinitialize(Relation rel, ParallelTableScanDesc pscan)
{
	ParallelBlockTableScanDesc bpscan = (ParallelBlockTableScanDesc) pscan;

	pg_atomic_write_u64(&bpscan->phs_nallocated, 0);
}

/* -------------------------------------------------------------------------
 * insert callbacks
 * ------------------------------------------------------------------------- */

static void
columnar_tuple_insert(Relation rel, TupleTableSlot *slot, CommandId cid,
					  COLUMNAR_TABLE_OPTIONS options,
					  struct BulkInsertStateData *bistate)
{
	ColumnarWriteState *writeState = ColumnarGetWriteState(rel);
	uint64		rowNumber;

	slot_getallattrs(slot);

	/*
	 * Serialize concurrent inserters of the same unique key (issue #5) before
	 * the executor runs its btree uniqueness check on this row, so the check
	 * runs only after any conflicting transaction has committed and flushed.
	 */
	ColumnarLockUniqueKeys(rel, slot);

	rowNumber = ColumnarWriteRow(writeState, rel, slot->tts_values,
								 slot->tts_isnull);

	/* fan the row out to every additional projection of this table (gap 26) */
	ColumnarProjectionFanoutRow(rel, writeState, rowNumber, slot->tts_values,
								slot->tts_isnull);

	/*
	 * A new row makes its block not all-visible; clear any VM bit so an
	 * index-only scan never skips the fetch for a block that just changed
	 * (gap 28). A no-op unless a prior vacuum had marked the block visible.
	 */
	ColumnarVMClearForRow(rel, rowNumber);

	/*
	 * Publish the row's synthetic item pointer (spec 6) so the executor can
	 * insert correct (index value, TID) entries into any indexes on this
	 * relation and enforce unique constraints (spec 9).
	 */
	ColumnarRowNumberToItemPointer(rowNumber, &slot->tts_tid);
	slot->tts_tableOid = RelationGetRelid(rel);
}

static void
columnar_multi_insert(Relation rel, TupleTableSlot **slots, int nslots,
					  CommandId cid, COLUMNAR_TABLE_OPTIONS options,
					  struct BulkInsertStateData *bistate)
{
	ColumnarWriteState *writeState = ColumnarGetWriteState(rel);
	int			i;

	for (i = 0; i < nslots; i++)
	{
		uint64		rowNumber;

		slot_getallattrs(slots[i]);
		ColumnarLockUniqueKeys(rel, slots[i]);	/* issue #5 */
		rowNumber = ColumnarWriteRow(writeState, rel, slots[i]->tts_values,
									 slots[i]->tts_isnull);
		ColumnarProjectionFanoutRow(rel, writeState, rowNumber,
									slots[i]->tts_values, slots[i]->tts_isnull);
		ColumnarVMClearForRow(rel, rowNumber);	/* gap 28: block changed */
		ColumnarRowNumberToItemPointer(rowNumber, &slots[i]->tts_tid);
		slots[i]->tts_tableOid = RelationGetRelid(rel);
	}
}

static void
columnar_finish_bulk_insert(Relation rel, COLUMNAR_TABLE_OPTIONS options)
{
	/*
	 * End of a bulk-load path (COPY, CREATE TABLE AS, ALTER TABLE rewrite).
	 * Flush now, under this operation's subtransaction, so the buffer never
	 * spans a later statement or savepoint boundary (spec 9).
	 */
	ColumnarFlushWriteStateForRelation(RelationGetRelid(rel));
	ColumnarFlushDeleteVectorForRelation(rel);
}

/* -------------------------------------------------------------------------
 * DDL callbacks
 * ------------------------------------------------------------------------- */

static void
columnar_relation_set_new_filelocator(Relation rel,
									  const RelFileLocator *newrlocator,
									  char persistence,
									  TransactionId *freezeXid,
									  MultiXactId *minmulti)
{
	SMgrRelation srel;
	uint64		storageId;

	*freezeXid = InvalidTransactionId;
	*minmulti = InvalidMultiXactId;

	if (persistence == RELPERSISTENCE_UNLOGGED)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("unlogged columnar tables are not supported")));

	srel = ColumnarRelationCreateStorage(*newrlocator, persistence);
	storageId = ColumnarNextStorageId();
	ColumnarWriteNewMetapage(newrlocator, srel, persistence, storageId);
}

static void
columnar_relation_nontransactional_truncate(Relation rel)
{
	uint64		storageId = ColumnarStorageId(rel);

	ColumnarDeleteMetadata(storageId);
	RelationTruncate(rel, 2);
	ColumnarResetMetapage(rel);
}

/* -------------------------------------------------------------------------
 * miscellaneous callbacks
 * ------------------------------------------------------------------------- */

static uint64
columnar_relation_size(Relation rel, ForkNumber forkNumber)
{
	SMgrRelation srel = RelationGetSmgr(rel);

	/*
	 * Compute size from smgr directly. We must not call
	 * RelationGetNumberOfBlocksInFork here: for a table AM it dispatches back
	 * into this very callback.
	 */
	if (forkNumber != MAIN_FORKNUM && !smgrexists(srel, forkNumber))
		return 0;

	return (uint64) smgrnblocks(srel, forkNumber) * BLCKSZ;
}

static bool
columnar_relation_needs_toast_table(Relation rel)
{
	/* the writer detoasts and stores values inline in the value stream */
	return false;
}

static void
columnar_relation_estimate_size(Relation rel, int32 *attr_widths,
								BlockNumber *pages, double *tuples,
								double *allvisfrac)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(rel);
	uint64		storageId = ColumnarStorageId(rel);
	Snapshot	snapshot;
	List	   *rowGroupList;
	ListCell   *lc;
	double		liveRows = 0;

	/*
	 * Estimate the row count from row-group metadata, not from the metapage
	 * reservation high-water mark: row numbers are reserved a whole row group at
	 * a time, so the reservation overcounts. An accurate estimate keeps the
	 * planner from mis-costing scans (spec 6, 9).
	 */
	snapshot = ActiveSnapshotSet() ? GetActiveSnapshot() : GetTransactionSnapshot();
	rowGroupList = ColumnarReadRowGroupList(storageId, ColumnarCatalogSnapshot(snapshot));

	foreach(lc, rowGroupList)
		liveRows += (double) ((NativeRowGroupMetadata *) lfirst(lc))->rowCount;

	*pages = Max(nblocks, 1);
	*tuples = Max(liveRows, 0);
	*allvisfrac = 0.0;
}

/* ANALYZE: phase 1 collects no statistics rather than crash */
static bool
columnar_scan_analyze_next_block(COLUMNAR_ANALYZE_NEXT_BLOCK_ARGS)
{
	return false;
}

static bool
columnar_scan_analyze_next_tuple(COLUMNAR_ANALYZE_NEXT_TUPLE_ARGS)
{
	return false;
}

/* VACUUM: nothing to do in phase 1 (delete vector / compaction arrive later) */
static void
columnar_relation_vacuum(Relation rel, COLUMNAR_VACUUM_PARAMS params,
						 BufferAccessStrategy bstrategy)
{
	/*
	 * Lazy vacuum (gap 28 phase 3): mark all-visible chunk groups in the VM
	 * fork so index-only scans can skip the columnar fetch. This only reads
	 * committed state and writes the VM fork -- no data rewrite -- so it runs
	 * fine under the ShareUpdateExclusiveLock a plain VACUUM/autovacuum holds,
	 * concurrent with readers and writers. The space-reclaiming rewrite stays in
	 * columnar.vacuum (AccessExclusiveLock, the VACUUM-FULL analog).
	 */
	ColumnarVMSetVisibleForRelation(rel);

	/*
	 * Online compaction (Phase F3a): retire row groups that are fully deleted
	 * as-of the oldest-xmin horizon, dropping their metadata so scans skip them.
	 * This is also read-mostly on data (it only deletes catalog rows for groups
	 * every snapshot agrees are dead) and is safe under ShareUpdateExclusiveLock,
	 * so a plain VACUUM / autovacuum reclaims fully-deleted groups online without
	 * the AccessExclusiveLock rewrite.
	 */
	ColumnarRetireFullyDeletedGroups(rel);
}

/* -------------------------------------------------------------------------
 * not-yet-supported callbacks (later phases)
 * ------------------------------------------------------------------------- */

#define COLUMNAR_UNSUPPORTED(feature) \
	ereport(ERROR, \
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED), \
			 errmsg("columnar: %s is not supported yet", \
					feature)))

/* our index-fetch descriptor is just the base plus nothing extra */
typedef struct ColumnarIndexFetchData
{
	IndexFetchTableData xs_base;
} ColumnarIndexFetchData;

static struct IndexFetchTableData *
columnar_index_fetch_begin(COLUMNAR_INDEX_FETCH_BEGIN_ARGS)
{
	ColumnarIndexFetchData *scan = palloc0(sizeof(ColumnarIndexFetchData));

	scan->xs_base.rel = rel;
	return &scan->xs_base;
}

static void
columnar_index_fetch_reset(struct IndexFetchTableData *scan)
{
}

static void
columnar_index_fetch_end(struct IndexFetchTableData *scan)
{
	pfree(scan);
}

/*
 * columnar_index_fetch_tuple
 *		Fetch the columnar row addressed by an index item pointer (spec 6) into
 *		the slot. Returns false when the row is marked deleted in the delete vector
 *		or does not exist, so an index scan never returns a deleted row and a
 *		unique check does not treat a deleted row as a live duplicate (spec 9).
 *
 *		The row is looked up first in the flushed stripes, then in any unflushed
 *		write buffer for the relation. The latter lets a unique constraint catch
 *		two duplicate rows inserted within a single statement, where the first
 *		row's item pointer is fetched while both rows are still buffered. This
 *		function acquires no relation extension or metapage locks, so it is safe
 *		to call while the caller holds an index buffer lock (the uniqueness
 *		check path).
 */
static bool
columnar_index_fetch_tuple(struct IndexFetchTableData *scan, ItemPointer tid,
						   Snapshot snapshot, TupleTableSlot *slot,
						   bool *call_again, bool *all_dead)
{
	Relation	rel = scan->rel;
	uint64		rowNumber = ColumnarItemPointerToRowNumber(tid);

	/* columnar rows are 1:1 with item pointers: no chain, never dead here */
	*call_again = false;
	if (all_dead != NULL)
		*all_dead = false;

	ExecClearTuple(slot);

	if (!ColumnarReadRowByNumber(rel, snapshot, rowNumber,
								 slot->tts_values, slot->tts_isnull) &&
		!ColumnarBufferedRowByNumber(rel, rowNumber,
									 slot->tts_values, slot->tts_isnull))
		return false;

	ExecStoreVirtualTuple(slot);
	ColumnarRowNumberToItemPointer(rowNumber, &slot->tts_tid);
	slot->tts_tableOid = RelationGetRelid(rel);

	return true;
}

/*
 * columnar_tuple_fetch_row_version
 *		Fetch the row addressed by tid into slot (spec 6). Used by UPDATE, which
 *		re-fetches the old row by its item pointer. Returns false when the row
 *		does not exist or is marked deleted.
 */
static bool
columnar_tuple_fetch_row_version(Relation rel, ItemPointer tid,
								 Snapshot snapshot, TupleTableSlot *slot)
{
	uint64		rowNumber = ColumnarItemPointerToRowNumber(tid);

	ExecClearTuple(slot);

	if (!ColumnarReadRowByNumber(rel, snapshot, rowNumber,
								 slot->tts_values, slot->tts_isnull))
		return false;

	ExecStoreVirtualTuple(slot);
	ColumnarRowNumberToItemPointer(rowNumber, &slot->tts_tid);
	slot->tts_tableOid = RelationGetRelid(rel);

	return true;
}

static bool
columnar_tuple_tid_valid(TableScanDesc scan, ItemPointer tid)
{
	return true;
}

static void
columnar_tuple_get_latest_tid(TableScanDesc scan, ItemPointer tid)
{
	COLUMNAR_UNSUPPORTED("get latest tid");
}

static bool
columnar_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot,
								  Snapshot snapshot)
{
	/* stripes are visible per their metadata snapshot; slots are visible */
	return true;
}

/*
 * columnar_index_delete_tuples
 *		Opportunistic index tuple deletion. An index entry is deletable exactly
 *		when its row is no longer visible, i.e. ColumnarReadRowByNumber cannot
 *		return it (deleted via the delete vector). Reporting deletability by actual
 *		liveness is required for correctness: nbtree's deletion pass (including
 *		bottom-up deletion of duplicate keys, which a same-key UPDATE produces)
 *		marks candidate items and calls this callback as the authority; leaving a
 *		genuinely dead item marked non-deletable would make nbtree assert
 *		(ndeletable > 0 || nupdatable > 0). Entries left in place are still
 *		filtered on fetch, so either way is correct. The snapshot conflict horizon
 *		is reported as invalid (no conflict), matching the delete vector's own MVCC on
 *		the catalog.
 */
#if PG_VERSION_NUM < 140000
/*
 * PG13 spelling of the same policy: the callback is
 * compute_xid_horizon_for_tuples, which reports the snapshot conflict horizon
 * for a batch of index tuples the caller would like to remove. We never remove
 * index entries opportunistically, so an invalid (no-conflict) horizon is the
 * correct and always-safe answer.
 */
static TransactionId
columnar_compute_xid_horizon_for_tuples(Relation rel, ItemPointerData *tids,
										int nitems)
{
	return InvalidTransactionId;
}
#else
static TransactionId
columnar_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate)
{
	Snapshot	snapshot = ActiveSnapshotSet() ? GetActiveSnapshot()
		: GetTransactionSnapshot();
	TupleDesc	tupdesc = RelationGetDescr(rel);
	Datum	   *values = (Datum *) palloc(sizeof(Datum) * tupdesc->natts);
	bool	   *nulls = (bool *) palloc(sizeof(bool) * tupdesc->natts);
	int			i;

	for (i = 0; i < delstate->ndeltids; i++)
	{
		uint64		rowNumber =
			ColumnarItemPointerToRowNumber(&delstate->deltids[i].tid);
		bool		live = ColumnarReadRowByNumber(rel, snapshot, rowNumber,
												   values, nulls);

		delstate->status[delstate->deltids[i].id].knowndeletable = !live;
	}

	pfree(values);
	pfree(nulls);
	return InvalidTransactionId;
}
#endif

static void
columnar_tuple_insert_speculative(Relation rel, TupleTableSlot *slot,
								  CommandId cid, COLUMNAR_TABLE_OPTIONS options,
								  struct BulkInsertStateData *bistate,
								  uint32 specToken)
{
	COLUMNAR_UNSUPPORTED("speculative insert");
}

static void
columnar_tuple_complete_speculative(Relation rel, TupleTableSlot *slot,
									uint32 specToken, bool succeeded)
{
	COLUMNAR_UNSUPPORTED("speculative insert");
}

/*
 * columnar_tuple_delete
 *		Mark the row addressed by tid as deleted in the delete vector (spec 9). The
 *		stripe is not rewritten. The tid is the synthetic item pointer the scan
 *		produced, which maps back to the row number.
 */
static TM_Result
columnar_tuple_delete(COLUMNAR_TUPLE_DELETE_ARGS)
{
	uint64		rowNumber = ColumnarItemPointerToRowNumber(tid);

	ColumnarMarkRowDeleted(rel, rowNumber);
	return TM_Ok;
}

/*
 * columnar_tuple_update
 *		Update is delete-plus-insert (spec 9): mark the old row deleted in the
 *		delete vector and append the new tuple as a fresh row with a new row number.
 *		The new row's item pointer is published on the slot and index
 *		maintenance is requested, so the new row gets fresh index entries. The
 *		old row's index entries remain but are filtered on fetch because the old
 *		row is now marked deleted (spec 6, 9).
 */
static TM_Result
columnar_tuple_update(COLUMNAR_TUPLE_UPDATE_ARGS)
{
	uint64		oldRowNumber = ColumnarItemPointerToRowNumber(otid);
	ColumnarWriteState *writeState;
	uint64		rowNumber;

	ColumnarMarkRowDeleted(rel, oldRowNumber);

	writeState = ColumnarGetWriteState(rel);
	slot_getallattrs(slot);

	/* the new row version is a fresh insert: serialize its unique keys too */
	ColumnarLockUniqueKeys(rel, slot);		/* issue #5 */

	rowNumber = ColumnarWriteRow(writeState, rel, slot->tts_values,
								 slot->tts_isnull);

	ColumnarRowNumberToItemPointer(rowNumber, &slot->tts_tid);
	slot->tts_tableOid = RelationGetRelid(rel);

	*lockmode = LockTupleExclusive;
	*update_indexes = COLUMNAR_TU_ALL;
	return TM_Ok;
}

static TM_Result
columnar_tuple_lock(Relation rel, ItemPointer tid, Snapshot snapshot,
					TupleTableSlot *slot, CommandId cid, LockTupleMode mode,
					LockWaitPolicy wait_policy, uint8 flags,
					TM_FailureData *tmfd)
{
	COLUMNAR_UNSUPPORTED("row locking");
	return TM_Ok;
}

static void
columnar_relation_copy_data(Relation rel, const RelFileLocator *newrlocator)
{
	COLUMNAR_UNSUPPORTED("relation copy (ALTER TABLE SET TABLESPACE)");
}

static void
columnar_relation_copy_for_cluster(COLUMNAR_COPY_FOR_CLUSTER_ARGS)
{
	COLUMNAR_UNSUPPORTED("CLUSTER / VACUUM FULL");
}

/*
 * columnar_index_build_range_scan
 *		Scan every live row of the columnar table and hand it to the index
 *		build callback, so CREATE INDEX (btree or hash) works over a columnar
 *		table (spec 9). Deleted rows (delete vector) are skipped by the reader, so
 *		they are not indexed. Each row's synthetic item pointer (spec 6) is the
 *		TID recorded in the index.
 *
 *		Only a full-table build is supported: a partial block range would have
 *		no meaning for synthetic item pointers, and concurrent validation uses a
 *		separate callback. Pending writes are flushed first so buffered rows are
 *		included in the build.
 */
static double
columnar_index_build_range_scan(Relation table_rel, Relation index_rel,
								struct IndexInfo *index_info, bool allow_sync,
								bool anyvisible, bool progress,
								BlockNumber start_blockno, BlockNumber numblocks,
								IndexBuildCallback callback, void *callback_state,
								TableScanDesc scan)
{
	ColumnarReadState *readState;
	bool		ownReadState;
	EState	   *estate;
	ExprContext *econtext;
	ExprState  *predicate;
	TupleTableSlot *slot;
	Datum		indexValues[INDEX_MAX_KEYS];
	bool		indexNulls[INDEX_MAX_KEYS];
	double		reltuples = 0;
	uint64		rowNumber;

	if (start_blockno != 0 || numblocks != InvalidBlockNumber)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("columnar: partial-range index build is not supported")));

	/* persist buffered rows and delete marks so the build sees them (spec 9) */
	ColumnarFlushWriteStateForRelation(RelationGetRelid(table_rel));
	ColumnarFlushDeleteVectorForRelation(table_rel);

	estate = CreateExecutorState();
	econtext = GetPerTupleExprContext(estate);
	slot = table_slot_create(table_rel, NULL);
	econtext->ecxt_scantuple = slot;

	/* a partial index only indexes rows satisfying its predicate */
	predicate = ExecPrepareQual(index_info->ii_Predicate, estate);

	/*
	 * Obtain the reader. A parallel index build passes the TableScanDesc it
	 * opened with table_beginscan_parallel; that scan already holds a reader
	 * bound to the shared parallel scan, whose single-participant claim (see
	 * columnar_read_start) makes exactly one participant read the whole table.
	 * We must read through that reader, not a private one: a private full-table
	 * reader in every participant would index every row once per participant,
	 * producing duplicate (key, TID) entries. When no scan is supplied (a serial
	 * build), open a private reader under an MVCC snapshot: the active snapshot
	 * when one is set (planning/DDL always has one), otherwise the transaction
	 * snapshot. The reader advances the command id internally for
	 * read-your-writes.
	 */
	if (scan != NULL)
	{
		readState = ((ColumnarScanDesc) scan)->readState;
		ownReadState = false;
	}
	else
	{
		Snapshot	snapshot;

		if (ActiveSnapshotSet())
			snapshot = GetActiveSnapshot();
		else
			snapshot = GetTransactionSnapshot();

		readState = ColumnarBeginRead(table_rel, snapshot, NULL, NULL, 0, NULL);
		ownReadState = true;
	}

	while (true)
	{
		CHECK_FOR_INTERRUPTS();

		ExecClearTuple(slot);
		if (!ColumnarReadNextRow(readState, slot->tts_values, slot->tts_isnull,
								 &rowNumber))
			break;
		ExecStoreVirtualTuple(slot);

		reltuples += 1;

		MemoryContextReset(econtext->ecxt_per_tuple_memory);

		if (predicate != NULL && !ExecQual(predicate, econtext))
			continue;

		FormIndexDatum(index_info, slot, estate, indexValues, indexNulls);

		ColumnarRowNumberToItemPointer(rowNumber, &slot->tts_tid);

		callback(index_rel, &slot->tts_tid, indexValues, indexNulls, true,
				 callback_state);
	}

	if (ownReadState)
		ColumnarEndRead(readState);
	ExecDropSingleTupleTableSlot(slot);
	FreeExecutorState(estate);

	/*
	 * The table AM contract makes index_build_range_scan the owner of a scan the
	 * caller supplied: it must end it, exactly as heapam_index_build_range_scan
	 * calls table_endscan on the passed scan whether or not it created the scan
	 * itself. columnar_scan_begin took a relation reference (and, for a worker,
	 * a registered snapshot) and created the reader used above; table_endscan
	 * runs columnar_scan_end, which ends that reader and releases the reference.
	 * Omitting this leaked one relation reference per build participant, which
	 * surfaced at commit as "resource was not closed: relation".
	 */
	if (scan != NULL)
		table_endscan(scan);

	return reltuples;
}

static void
columnar_index_validate_scan(Relation table_rel, Relation index_rel,
							 struct IndexInfo *index_info, Snapshot snapshot,
							 struct ValidateIndexState *state)
{
	COLUMNAR_UNSUPPORTED("concurrent index validate");
}

static bool
columnar_scan_sample_next_block(TableScanDesc scan,
								struct SampleScanState *scanstate)
{
	COLUMNAR_UNSUPPORTED("TABLESAMPLE");
	return false;
}

static bool
columnar_scan_sample_next_tuple(TableScanDesc scan,
								struct SampleScanState *scanstate,
								TupleTableSlot *slot)
{
	COLUMNAR_UNSUPPORTED("TABLESAMPLE");
	return false;
}

/* -------------------------------------------------------------------------
 * the routine
 * ------------------------------------------------------------------------- */

static const TableAmRoutine columnar_am_methods = {
	.type = T_TableAmRoutine,

	.slot_callbacks = columnar_slot_callbacks,

	.scan_begin = columnar_scan_begin,
	.scan_end = columnar_scan_end,
	.scan_rescan = columnar_scan_rescan,
	.scan_getnextslot = columnar_scan_getnextslot,

	.parallelscan_estimate = columnar_parallelscan_estimate,
	.parallelscan_initialize = columnar_parallelscan_initialize,
	.parallelscan_reinitialize = columnar_parallelscan_reinitialize,

	.index_fetch_begin = columnar_index_fetch_begin,
	.index_fetch_reset = columnar_index_fetch_reset,
	.index_fetch_end = columnar_index_fetch_end,
	.index_fetch_tuple = columnar_index_fetch_tuple,

	.tuple_fetch_row_version = columnar_tuple_fetch_row_version,
	.tuple_tid_valid = columnar_tuple_tid_valid,
	.tuple_get_latest_tid = columnar_tuple_get_latest_tid,
	.tuple_satisfies_snapshot = columnar_tuple_satisfies_snapshot,
#if PG_VERSION_NUM < 140000
	.COLUMNAR_AM_INDEX_DELETE_FIELD = columnar_compute_xid_horizon_for_tuples,
#else
	.COLUMNAR_AM_INDEX_DELETE_FIELD = columnar_index_delete_tuples,
#endif

	.tuple_insert = columnar_tuple_insert,
	.tuple_insert_speculative = columnar_tuple_insert_speculative,
	.tuple_complete_speculative = columnar_tuple_complete_speculative,
	.multi_insert = columnar_multi_insert,
	.tuple_delete = columnar_tuple_delete,
	.tuple_update = columnar_tuple_update,
	.tuple_lock = columnar_tuple_lock,
	.finish_bulk_insert = columnar_finish_bulk_insert,

	.COLUMNAR_AM_SET_NEW_FILE_FIELD = columnar_relation_set_new_filelocator,
	.relation_nontransactional_truncate = columnar_relation_nontransactional_truncate,
	.relation_copy_data = columnar_relation_copy_data,
	.relation_copy_for_cluster = columnar_relation_copy_for_cluster,
	.relation_vacuum = columnar_relation_vacuum,
	.scan_analyze_next_block = columnar_scan_analyze_next_block,
	.scan_analyze_next_tuple = columnar_scan_analyze_next_tuple,
	.index_build_range_scan = columnar_index_build_range_scan,
	.index_validate_scan = columnar_index_validate_scan,

	.relation_size = columnar_relation_size,
	.relation_needs_toast_table = columnar_relation_needs_toast_table,

	.relation_estimate_size = columnar_relation_estimate_size,

	.scan_sample_next_block = columnar_scan_sample_next_block,
	.scan_sample_next_tuple = columnar_scan_sample_next_tuple,
};

Datum
columnar_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(&columnar_am_methods);
}

/* -------------------------------------------------------------------------
 * transaction callback: flush pending writes at pre-commit
 * ------------------------------------------------------------------------- */

static void
columnar_xact_callback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
		case XACT_EVENT_PARALLEL_PRE_COMMIT:
		case XACT_EVENT_PREPARE:
			ColumnarFlushAllPendingWrites();
			ColumnarFlushAllDeleteVectors();
			break;
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_COMMIT:
		case XACT_EVENT_PARALLEL_ABORT:
			ColumnarDiscardAllPendingWrites();
			ColumnarDiscardAllDeleteVectors();
			break;
		default:
			break;
	}
}

/* -------------------------------------------------------------------------
 * subtransaction callback: discard or promote pending work of a savepoint
 * ------------------------------------------------------------------------- */

static void
columnar_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
						  SubTransactionId parentSubid, void *arg)
{
	switch (event)
	{
		case SUBXACT_EVENT_ABORT_SUB:
			ColumnarWriteStateDiscardSubXact(mySubid);
			ColumnarDeleteVectorDiscardSubXact(mySubid);
			break;
		case SUBXACT_EVENT_COMMIT_SUB:
			ColumnarWriteStatePromoteSubXact(mySubid, parentSubid);
			ColumnarDeleteVectorPromoteSubXact(mySubid, parentSubid);
			break;
		default:
			break;
	}
}

/* -------------------------------------------------------------------------
 * executor end hook: flush pending writes at statement end
 *
 * INSERT/UPDATE/DELETE do not call finish_bulk_insert, so flush here at the
 * end of each executed statement. Flushing while the writing statement's
 * subtransaction is still current is what makes savepoint rollback correct:
 * a buffer written before a savepoint is persisted (and attributed) under the
 * outer subtransaction, so it survives a later ROLLBACK TO, while a buffer
 * written after the savepoint is attributed to the inner subtransaction and
 * is correctly discarded on its rollback (spec 9).
 * ------------------------------------------------------------------------- */

static void
columnar_executor_end(QueryDesc *queryDesc)
{
	if (prev_executor_end_hook)
		prev_executor_end_hook(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);

	ColumnarFlushAllPendingWrites();
	ColumnarFlushAllDeleteVectors();
}

/* -------------------------------------------------------------------------
 * object access hook: clean up metadata when a columnar table is dropped
 * ------------------------------------------------------------------------- */

static void
columnar_object_access(ObjectAccessType access, Oid classId, Oid objectId,
					   int subId, void *arg)
{
	if (prev_object_access_hook)
		prev_object_access_hook(access, classId, objectId, subId, arg);

	if (access == OAT_DROP && classId == RelationRelationId && subId == 0)
	{
		Relation	rel;

		if (get_rel_relkind(objectId) != RELKIND_RELATION)
			return;

		/* DROP already holds AccessExclusiveLock on the relation */
		rel = relation_open(objectId, NoLock);

		if (rel->rd_tableam == &columnar_am_methods)
		{
			uint64		storageId = ColumnarStorageId(rel);

			ColumnarDeleteMetadata(storageId);
			ColumnarDeleteOptions(objectId);
		}

		relation_close(rel, NoLock);
	}
}

/* -------------------------------------------------------------------------
 * planner hook: forbid index-only scans on columnar tables
 *
 * A columnar table has no visibility map, so an index-only scan cannot decide
 * visibility from the map and is not supported (spec 9). An ordinary index scan
 * is fine because it fetches each row through index_fetch_tuple, which applies
 * the delete vector. We forbid index-only scans by clearing each candidate index's
 * per-column "can return" flags for a columnar table, before the planner builds
 * any path; the planner then builds a plain index scan instead of an index-only
 * scan for the same index.
 *
 * The clearing must happen after get_relation_info() has populated the base
 * relation's indexlist. Through PG18 that is get_relation_info_hook; PG19
 * removed it and added build_simple_rel_hook, which fires at the same point
 * (right after get_relation_info in build_simple_rel) for exactly this kind of
 * editorializing on the index list. Both routes yield an identical plan.
 * ------------------------------------------------------------------------- */

static bool
columnar_relation_is_columnar(Oid relid)
{
	if (columnar_am_oid_cache == InvalidOid)
		columnar_am_oid_cache = get_am_oid("pgcolumnar", true);

	return OidIsValid(columnar_am_oid_cache) &&
		get_rel_relam(relid) == columnar_am_oid_cache;
}

/* GUC: when on, allow the planner to build index-only-scan paths for columnar
 * tables, served by the VM fork (gap 28). Default on: the phase-5 MVCC,
 * concurrency, and crash-recovery suites prove the all-visible protocol (the
 * horizon accounts for open snapshots and every write clears the bit, both
 * WAL-logged), and a not-all-visible block always falls back to the
 * snapshot-checked fetch, so results are correct regardless. */
bool		columnar_enable_index_only_scan = true;

/* clear the "can return" flags of every index on a columnar relation */
static void
columnar_forbid_index_only_scan(Oid relid, RelOptInfo *rel)
{
	ListCell   *lc;

	/* when index-only scans are enabled, leave the index canreturn flags intact
	 * so the planner may choose an IOS; the VM fork (set by lazy vacuum) drives
	 * whether the executor skips the fetch, and a not-all-visible block still
	 * falls back to columnar_index_fetch_tuple, so results are always correct. */
	if (columnar_enable_index_only_scan)
		return;

	if (!OidIsValid(relid) || !columnar_relation_is_columnar(relid))
		return;

	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *index = (IndexOptInfo *) lfirst(lc);
		int			i;

		if (index->canreturn == NULL)
			continue;

		for (i = 0; i < index->ncolumns; i++)
			index->canreturn[i] = false;
	}
}

#if PG_VERSION_NUM >= 190000
static void
columnar_build_simple_rel(PlannerInfo *root, RelOptInfo *rel,
						  RangeTblEntry *rte)
{
	if (prev_build_simple_rel_hook)
		prev_build_simple_rel_hook(root, rel, rte);

	if (rte->rtekind == RTE_RELATION)
		columnar_forbid_index_only_scan(rte->relid, rel);
}
#else
static void
columnar_get_relation_info(PlannerInfo *root, Oid relationObjectId,
						   bool inhparent, RelOptInfo *rel)
{
	if (prev_get_relation_info_hook)
		prev_get_relation_info_hook(root, relationObjectId, inhparent, rel);

	columnar_forbid_index_only_scan(relationObjectId, rel);
}
#endif

/* -------------------------------------------------------------------------
 * module init
 * ------------------------------------------------------------------------- */

void
_PG_init(void)
{
	DefineCustomIntVariable("pgcolumnar.stripe_row_limit",
							"Maximum number of rows per stripe.",
							NULL,
							&columnar_stripe_row_limit,
							150000,
							1000, INT_MAX,
							PGC_USERSET,
							0,
							NULL, NULL, NULL);

	DefineCustomIntVariable("pgcolumnar.chunk_group_row_limit",
							"Maximum number of rows per chunk group.",
							NULL,
							&columnar_chunk_group_row_limit,
							10000,
							100, INT_MAX,
							PGC_USERSET,
							0,
							NULL, NULL, NULL);

	DefineCustomEnumVariable("pgcolumnar.compression",
							 "Default compression codec for new chunks.",
							 NULL,
							 &columnar_compression,
							 COLUMNAR_COMPRESSION_ZSTD,
							 columnar_compression_options,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomIntVariable("pgcolumnar.compression_level",
							"Compression level for the zstd codec.",
							NULL,
							&columnar_compression_level,
							3,
							1, 22,
							PGC_USERSET,
							0,
							NULL, NULL, NULL);

	DefineCustomBoolVariable("pgcolumnar.enable_qual_pushdown",
							 "Push scan qualifiers down for chunk-group skipping.",
							 NULL,
							 &columnar_enable_qual_pushdown,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("pgcolumnar.enable_custom_scan",
							 "Use the columnar custom scan path for columnar tables.",
							 NULL,
							 &columnar_enable_custom_scan,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("pgcolumnar.enable_vectorization",
							 "Use the vectorized aggregate fast path.",
							 NULL,
							 &columnar_enable_vectorization,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("pgcolumnar.enable_metadata_count",
							 "Answer count(*) from catalog metadata without scanning.",
							 NULL,
							 &columnar_enable_metadata_count,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("pgcolumnar.enable_bloom_filter",
							 "Skip chunk groups on equality using per-chunk bloom filters.",
							 NULL,
							 &columnar_enable_bloom_filter,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("pgcolumnar.enable_column_cache",
							 "Cache decompressed chunk groups to reuse across reads.",
							 NULL,
							 &columnar_enable_column_cache,
							 false,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("pgcolumnar.reclaim_coalesce",
							 "Split oversized freed ranges on reuse and coalesce "
							 "adjacent freed ranges, so compaction reclaims space "
							 "under fragmentation. Off reverts to whole-range reuse.",
							 NULL,
							 &columnar_reclaim_coalesce,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("pgcolumnar.enable_end_truncation",
							 "Allow pgcolumnar.truncate() to physically return "
							 "trailing reclaimed blocks to the OS. Off (the default) "
							 "makes truncate() a no-op.",
							 NULL,
							 &columnar_enable_end_truncation,
							 false,
							 PGC_SUSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("pgcolumnar.enable_read_stream",
							 "Prefetch block reads with the read stream API (PostgreSQL 17+).",
							 NULL,
							 &columnar_enable_read_stream,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("pgcolumnar.enable_index_only_scan",
							 "Allow index-only scans on columnar tables, served by the "
							 "visibility-map fork (gap 28). On by default; set off to force "
							 "a plain index scan.",
							 NULL,
							 &columnar_enable_index_only_scan,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("pgcolumnar.enable_projection_scan",
							 "Let the planner scan a covering projection instead of the "
							 "base table when one serves the query better (gap 26).",
							 NULL,
							 &columnar_enable_projection_scan,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomIntVariable("pgcolumnar.column_cache_size",
							"Size of the decompressed-chunk cache, in megabytes.",
							NULL,
							&columnar_column_cache_size,
							200,
							1, INT_MAX,
							PGC_USERSET,
							GUC_UNIT_MB,
							NULL, NULL, NULL);

	DefineCustomBoolVariable("pgcolumnar.enable_unique_insert_lock",
							 "Serialize concurrent inserts of the same unique key.",
							 "Takes a transaction-scoped advisory lock per unique "
							 "index key so overlapping same-key inserts conflict "
							 "correctly (issue #5). Turning it off restores the "
							 "prior racy behavior.",
							 &columnar_enable_unique_lock,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomIntVariable("pgcolumnar.unique_lock_buckets",
							"Advisory-lock buckets per unique index for same-key "
							"insert serialization.",
							"Bounds the transaction's held advisory locks to at "
							"most this many per unique index. Equal keys always "
							"share a bucket; unrelated keys may share one, which "
							"only over-serializes.",
							&columnar_unique_lock_buckets,
							128,
							1, 1048576,
							PGC_USERSET,
							0,
							NULL, NULL, NULL);

	MarkGUCPrefixReserved("pgcolumnar");

	RegisterXactCallback(columnar_xact_callback, NULL);
	RegisterSubXactCallback(columnar_subxact_callback, NULL);

	prev_object_access_hook = object_access_hook;
	object_access_hook = columnar_object_access;

	prev_executor_end_hook = ExecutorEnd_hook;
	ExecutorEnd_hook = columnar_executor_end;

	/*
	 * Forbid index-only scans on columnar tables. PG19 replaced
	 * get_relation_info_hook with build_simple_rel_hook; both fire right after
	 * get_relation_info has built the base relation's index list.
	 */
#if PG_VERSION_NUM >= 190000
	prev_build_simple_rel_hook = build_simple_rel_hook;
	build_simple_rel_hook = columnar_build_simple_rel;
#else
	prev_get_relation_info_hook = get_relation_info_hook;
	get_relation_info_hook = columnar_get_relation_info;
#endif

	/* register the custom scan provider and install the pathlist hook */
	ColumnarCustomScanInit();

	/* install the vectorized-aggregate upper-path hook (spec 9) */
	ColumnarVectorInit();

	/* set up the optional decompressed-chunk cache (spec 8.3) */
	ColumnarCacheInit();

	/* register the unique-index cache invalidation callback (issue #5) */
	ColumnarUniqueInit();
}
