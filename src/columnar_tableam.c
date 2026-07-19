/*-------------------------------------------------------------------------
 *
 * columnar_tableam.c
 *		Table access method handler for pgColumnar and extension glue:
 *		GUCs, the pre-commit flush hook, and drop-time metadata cleanup.
 *
 * Implements the subset of TableAmRoutine built through phase 3: create, bulk
 * insert, sequential scan, delete and update via the row mask, fetch by tid,
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
#include "catalog/objectaccess.h"
#include "catalog/pg_class.h"
#include "catalog/storage.h"
#include "commands/vacuum.h"
#include "executor/executor.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
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
	ColumnarFlushRowMaskForRelation(rel);

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
	bpscan->base.phs_relid = RelationGetRelid(rel);
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
					  int options, struct BulkInsertStateData *bistate)
{
	ColumnarWriteState *writeState = ColumnarGetWriteState(rel);

	slot_getallattrs(slot);
	ColumnarWriteRow(writeState, slot->tts_values, slot->tts_isnull);
}

static void
columnar_multi_insert(Relation rel, TupleTableSlot **slots, int nslots,
					  CommandId cid, int options,
					  struct BulkInsertStateData *bistate)
{
	ColumnarWriteState *writeState = ColumnarGetWriteState(rel);
	int			i;

	for (i = 0; i < nslots; i++)
	{
		slot_getallattrs(slots[i]);
		ColumnarWriteRow(writeState, slots[i]->tts_values, slots[i]->tts_isnull);
	}
}

static void
columnar_finish_bulk_insert(Relation rel, int options)
{
	/*
	 * End of a bulk-load path (COPY, CREATE TABLE AS, ALTER TABLE rewrite).
	 * Flush now, under this operation's subtransaction, so the buffer never
	 * spans a later statement or savepoint boundary (spec 9).
	 */
	ColumnarFlushWriteStateForRelation(RelationGetRelid(rel));
	ColumnarFlushRowMaskForRelation(rel);
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

	srel = RelationCreateStorage(*newrlocator, persistence, true);
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
	ColumnarMetapage meta;
	BlockNumber nblocks = RelationGetNumberOfBlocks(rel);

	ColumnarReadMetapage(rel, &meta);

	*pages = Max(nblocks, 1);
	*tuples = (double) (meta.reservedRowNumber - COLUMNAR_FIRST_ROW_NUMBER);
	*allvisfrac = 0.0;
}

/* ANALYZE: phase 1 collects no statistics rather than crash */
static bool
columnar_scan_analyze_next_block(TableScanDesc scan, ReadStream *stream)
{
	return false;
}

static bool
columnar_scan_analyze_next_tuple(TableScanDesc scan, TransactionId OldestXmin,
								 double *liverows, double *deadrows,
								 TupleTableSlot *slot)
{
	return false;
}

/* VACUUM: nothing to do in phase 1 (row mask / compaction arrive later) */
static void
columnar_relation_vacuum(Relation rel, struct VacuumParams *params,
						 BufferAccessStrategy bstrategy)
{
}

/* -------------------------------------------------------------------------
 * not-yet-supported callbacks (later phases)
 * ------------------------------------------------------------------------- */

#define COLUMNAR_UNSUPPORTED(feature) \
	ereport(ERROR, \
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED), \
			 errmsg("columnar: %s is not supported yet", \
					feature)))

static struct IndexFetchTableData *
columnar_index_fetch_begin(Relation rel)
{
	COLUMNAR_UNSUPPORTED("index scan");
	return NULL;
}

static void
columnar_index_fetch_reset(struct IndexFetchTableData *scan)
{
}

static void
columnar_index_fetch_end(struct IndexFetchTableData *scan)
{
}

static bool
columnar_index_fetch_tuple(struct IndexFetchTableData *scan, ItemPointer tid,
						   Snapshot snapshot, TupleTableSlot *slot,
						   bool *call_again, bool *all_dead)
{
	COLUMNAR_UNSUPPORTED("index fetch");
	return false;
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

static TransactionId
columnar_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate)
{
	COLUMNAR_UNSUPPORTED("index delete");
	return InvalidTransactionId;
}

static void
columnar_tuple_insert_speculative(Relation rel, TupleTableSlot *slot,
								  CommandId cid, int options,
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
 *		Mark the row addressed by tid as deleted in the row mask (spec 9). The
 *		stripe is not rewritten. The tid is the synthetic item pointer the scan
 *		produced, which maps back to the row number.
 */
static TM_Result
columnar_tuple_delete(Relation rel, ItemPointer tid, CommandId cid,
					  Snapshot snapshot, Snapshot crosscheck, bool wait,
					  TM_FailureData *tmfd, bool changingPart)
{
	uint64		rowNumber = ColumnarItemPointerToRowNumber(tid);

	ColumnarMarkRowDeleted(rel, rowNumber);
	return TM_Ok;
}

/*
 * columnar_tuple_update
 *		Update is delete-plus-insert (spec 9): mark the old row deleted in the
 *		row mask and append the new tuple as a fresh row. The new row gets a new
 *		row number when the pending write is flushed. Columnar has no indexes in
 *		this phase, so no index maintenance is requested.
 */
static TM_Result
columnar_tuple_update(Relation rel, ItemPointer otid, TupleTableSlot *slot,
					  CommandId cid, Snapshot snapshot, Snapshot crosscheck,
					  bool wait, TM_FailureData *tmfd,
					  LockTupleMode *lockmode, TU_UpdateIndexes *update_indexes)
{
	uint64		oldRowNumber = ColumnarItemPointerToRowNumber(otid);
	ColumnarWriteState *writeState;

	ColumnarMarkRowDeleted(rel, oldRowNumber);

	writeState = ColumnarGetWriteState(rel);
	slot_getallattrs(slot);
	ColumnarWriteRow(writeState, slot->tts_values, slot->tts_isnull);

	*lockmode = LockTupleExclusive;
	*update_indexes = TU_None;
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
columnar_relation_copy_for_cluster(Relation OldTable, Relation NewTable,
								   Relation OldIndex, bool use_sort,
								   TransactionId OldestXmin,
								   TransactionId *xid_cutoff,
								   MultiXactId *multi_cutoff,
								   double *num_tuples, double *tups_vacuumed,
								   double *tups_recently_dead)
{
	COLUMNAR_UNSUPPORTED("CLUSTER / VACUUM FULL");
}

static double
columnar_index_build_range_scan(Relation table_rel, Relation index_rel,
								struct IndexInfo *index_info, bool allow_sync,
								bool anyvisible, bool progress,
								BlockNumber start_blockno, BlockNumber numblocks,
								IndexBuildCallback callback, void *callback_state,
								TableScanDesc scan)
{
	COLUMNAR_UNSUPPORTED("index build");
	return 0;
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
	.index_delete_tuples = columnar_index_delete_tuples,

	.tuple_insert = columnar_tuple_insert,
	.tuple_insert_speculative = columnar_tuple_insert_speculative,
	.tuple_complete_speculative = columnar_tuple_complete_speculative,
	.multi_insert = columnar_multi_insert,
	.tuple_delete = columnar_tuple_delete,
	.tuple_update = columnar_tuple_update,
	.tuple_lock = columnar_tuple_lock,
	.finish_bulk_insert = columnar_finish_bulk_insert,

	.relation_set_new_filelocator = columnar_relation_set_new_filelocator,
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
			ColumnarFlushAllRowMasks();
			break;
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_COMMIT:
		case XACT_EVENT_PARALLEL_ABORT:
			ColumnarDiscardAllPendingWrites();
			ColumnarDiscardAllRowMasks();
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
			ColumnarRowMaskDiscardSubXact(mySubid);
			break;
		case SUBXACT_EVENT_COMMIT_SUB:
			ColumnarWriteStatePromoteSubXact(mySubid, parentSubid);
			ColumnarRowMaskPromoteSubXact(mySubid, parentSubid);
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
	ColumnarFlushAllRowMasks();
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
		}

		relation_close(rel, NoLock);
	}
}

/* -------------------------------------------------------------------------
 * module init
 * ------------------------------------------------------------------------- */

void
_PG_init(void)
{
	DefineCustomIntVariable("columnar.stripe_row_limit",
							"Maximum number of rows per stripe.",
							NULL,
							&columnar_stripe_row_limit,
							150000,
							1000, INT_MAX,
							PGC_USERSET,
							0,
							NULL, NULL, NULL);

	DefineCustomIntVariable("columnar.chunk_group_row_limit",
							"Maximum number of rows per chunk group.",
							NULL,
							&columnar_chunk_group_row_limit,
							10000,
							100, INT_MAX,
							PGC_USERSET,
							0,
							NULL, NULL, NULL);

	DefineCustomEnumVariable("columnar.compression",
							 "Default compression codec for new chunks.",
							 NULL,
							 &columnar_compression,
							 COLUMNAR_COMPRESSION_ZSTD,
							 columnar_compression_options,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomIntVariable("columnar.compression_level",
							"Compression level for the zstd codec.",
							NULL,
							&columnar_compression_level,
							3,
							1, 22,
							PGC_USERSET,
							0,
							NULL, NULL, NULL);

	DefineCustomBoolVariable("columnar.enable_qual_pushdown",
							 "Push scan qualifiers down for chunk-group skipping.",
							 NULL,
							 &columnar_enable_qual_pushdown,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	MarkGUCPrefixReserved("columnar");

	RegisterXactCallback(columnar_xact_callback, NULL);
	RegisterSubXactCallback(columnar_subxact_callback, NULL);

	prev_object_access_hook = object_access_hook;
	object_access_hook = columnar_object_access;

	prev_executor_end_hook = ExecutorEnd_hook;
	ExecutorEnd_hook = columnar_executor_end;
}
