/*-------------------------------------------------------------------------
 *
 * columnar_compat.h
 *		PostgreSQL major-version compatibility shims for pgColumnar.
 *
 * pgColumnar keeps a single source tree that builds on PostgreSQL 13 through
 * 19. PostgreSQL changed several of its own API and callback contracts across
 * those majors; this header centralizes the differences so the .c files can be
 * written against one spelling. Every shim here only selects the correct core
 * API for the running major, it never changes pgColumnar's behavior.
 *
 * Each shim is derived solely from the public PostgreSQL headers of the target
 * version (access/tableam.h, catalog/storage.h, storage/smgr.h,
 * access/xloginsert.h, access/heapam.h, commands/vacuum.h). See PROVENANCE.md.
 *
 *-------------------------------------------------------------------------
 */
#ifndef COLUMNAR_COMPAT_H
#define COLUMNAR_COMPAT_H

#include "postgres.h"

/* -------------------------------------------------------------------------
 * RelFileLocator (PG16+) was called RelFileNode before PG16. The physical
 * identifier field on SMgrRelation was renamed at the same commit: the
 * relation-node was smgr_rnode (a RelFileNodeBackend, whose .node is the
 * RelFileNode) and became smgr_rlocator (a RelFileLocatorBackend, whose
 * .locator is the RelFileLocator).
 *
 * We spell the type "RelFileLocator" everywhere and, on pre-16, alias it to
 * RelFileNode. No pre-16 header defines the identifier "RelFileLocator", so the
 * macro alias is unambiguous.
 * ------------------------------------------------------------------------- */
#if PG_VERSION_NUM < 160000
#include "storage/relfilenode.h"
#define RelFileLocator RelFileNode
#define COLUMNAR_SMGR_LOCATOR(srel) ((srel)->smgr_rnode.node)
/* xl_smgr_truncate's physical-id field was named rnode before PG16 */
#define COLUMNAR_XLREC_SET_LOCATOR(xlrec, srel) \
	((xlrec).rnode = COLUMNAR_SMGR_LOCATOR(srel))
#else
#include "storage/relfilelocator.h"
#define COLUMNAR_SMGR_LOCATOR(srel) ((srel)->smgr_rlocator.locator)
#define COLUMNAR_XLREC_SET_LOCATOR(xlrec, srel) \
	((xlrec).rlocator = COLUMNAR_SMGR_LOCATOR(srel))
#endif

/* -------------------------------------------------------------------------
 * smgrtruncate gained an old_nblocks argument in PG18. PG15-17 ship the 5-arg
 * variant as smgrtruncate2 (backpatched with the extension-vs-truncate race
 * fix); PG18+ renamed that signature back to smgrtruncate. We always pass the
 * old block count, which the caller has in hand.
 * ------------------------------------------------------------------------- */
#if PG_VERSION_NUM >= 180000
#define COLUMNAR_SMGRTRUNCATE(reln, forks, nforks, oldnb, newnb) \
	smgrtruncate((reln), (forks), (nforks), (oldnb), (newnb))
#else
#define COLUMNAR_SMGRTRUNCATE(reln, forks, nforks, oldnb, newnb) \
	smgrtruncate2((reln), (forks), (nforks), (oldnb), (newnb))
#endif

/* -------------------------------------------------------------------------
 * pg_class_ownercheck (a relation-specific helper) was generalized to
 * object_ownercheck(classid, objectid, roleid) in PG16. Both return true for a
 * superuser, so an owner-or-superuser gate needs no separate superuser bypass.
 * ------------------------------------------------------------------------- */
#if PG_VERSION_NUM >= 160000
#define COLUMNAR_TABLE_OWNERCHECK(relid) \
	object_ownercheck(RelationRelationId, (relid), GetUserId())
#else
#define COLUMNAR_TABLE_OWNERCHECK(relid) \
	pg_class_ownercheck((relid), GetUserId())
#endif

/* -------------------------------------------------------------------------
 * RelationCreateStorage() gained a third argument (register_delete, whether to
 * schedule the physical file for unlink on abort) in PG15. Before PG15 it took
 * only (locator, persistence). Heap passes true, and so do we.
 * ------------------------------------------------------------------------- */
#if PG_VERSION_NUM < 150000
#define ColumnarRelationCreateStorage(loc, persistence) \
	RelationCreateStorage((loc), (persistence))
#else
#define ColumnarRelationCreateStorage(loc, persistence) \
	RelationCreateStorage((loc), (persistence), true)
#endif

/* -------------------------------------------------------------------------
 * The table AM's opportunistic index-tuple deletion callback is
 * index_delete_tuples(Relation, TM_IndexDeleteOp *) in PG14+. In PG13 the slot
 * was compute_xid_horizon_for_tuples(Relation, ItemPointerData *, int). The two
 * take different arguments, so the callback itself is compiled per major (see
 * columnar_tableam.c); this macro only selects the struct field name.
 * ------------------------------------------------------------------------- */
#if PG_VERSION_NUM < 140000
#define COLUMNAR_AM_INDEX_DELETE_FIELD compute_xid_horizon_for_tuples
#else
#define COLUMNAR_AM_INDEX_DELETE_FIELD index_delete_tuples
#endif

/* -------------------------------------------------------------------------
 * relation_set_new_filelocator() (PG16+) was relation_set_new_filenode()
 * before PG16. The signatures are otherwise identical once RelFileLocator is
 * aliased above, so only the struct field name differs.
 * ------------------------------------------------------------------------- */
#if PG_VERSION_NUM < 160000
#define COLUMNAR_AM_SET_NEW_FILE_FIELD relation_set_new_filenode
#else
#define COLUMNAR_AM_SET_NEW_FILE_FIELD relation_set_new_filelocator
#endif

/* -------------------------------------------------------------------------
 * tuple_update() reports which indexes to maintain through a pointer whose
 * target changed from bool (PG13-15) to the TU_UpdateIndexes enum (PG16+). We
 * want "maintain all indexes", which is true / TU_All. ColumnarUpdateIndexes is
 * a macro (not a typedef) so the enum name is not referenced until the .c file
 * has already included access/tableam.h, which defines it.
 * ------------------------------------------------------------------------- */
#if PG_VERSION_NUM < 160000
#define ColumnarUpdateIndexes bool
#define COLUMNAR_TU_ALL true
#else
#define ColumnarUpdateIndexes TU_UpdateIndexes
#define COLUMNAR_TU_ALL TU_All
#endif

/* -------------------------------------------------------------------------
 * PG19 reworked the row-modification table AM callbacks: the "options" bitmask
 * widened from int to uint32 (tuple_insert, tuple_insert_speculative,
 * multi_insert, finish_bulk_insert), and both tuple_delete and tuple_update
 * gained a leading uint32 "options" argument (folding tuple_delete's former
 * trailing bool "changingPart" into a TABLE_MODIFY_* flag). We ignore the
 * options in every case, so the shims only reconcile the parameter lists.
 * ------------------------------------------------------------------------- */
#if PG_VERSION_NUM >= 190000
#define COLUMNAR_TABLE_OPTIONS uint32
#else
#define COLUMNAR_TABLE_OPTIONS int
#endif

#if PG_VERSION_NUM >= 190000
#define COLUMNAR_TUPLE_DELETE_ARGS \
	Relation rel, ItemPointer tid, CommandId cid, uint32 options, \
	Snapshot snapshot, Snapshot crosscheck, bool wait, TM_FailureData *tmfd
#else
#define COLUMNAR_TUPLE_DELETE_ARGS \
	Relation rel, ItemPointer tid, CommandId cid, \
	Snapshot snapshot, Snapshot crosscheck, bool wait, TM_FailureData *tmfd, \
	bool changingPart
#endif

#if PG_VERSION_NUM >= 190000
#define COLUMNAR_TUPLE_UPDATE_ARGS \
	Relation rel, ItemPointer otid, TupleTableSlot *slot, CommandId cid, \
	uint32 options, Snapshot snapshot, Snapshot crosscheck, bool wait, \
	TM_FailureData *tmfd, LockTupleMode *lockmode, \
	ColumnarUpdateIndexes *update_indexes
#else
#define COLUMNAR_TUPLE_UPDATE_ARGS \
	Relation rel, ItemPointer otid, TupleTableSlot *slot, CommandId cid, \
	Snapshot snapshot, Snapshot crosscheck, bool wait, \
	TM_FailureData *tmfd, LockTupleMode *lockmode, \
	ColumnarUpdateIndexes *update_indexes
#endif

/* -------------------------------------------------------------------------
 * index_fetch_begin() gained a uint32 "flags" argument in PG19. We open a
 * fetch the same way regardless, so the flags are ignored.
 * ------------------------------------------------------------------------- */
#if PG_VERSION_NUM >= 190000
#define COLUMNAR_INDEX_FETCH_BEGIN_ARGS Relation rel, uint32 flags
#else
#define COLUMNAR_INDEX_FETCH_BEGIN_ARGS Relation rel
#endif

/* -------------------------------------------------------------------------
 * relation_copy_for_cluster() gained a Snapshot argument in PG19. We do not
 * support CLUSTER on columnar, so the callback only has to match the type.
 * ------------------------------------------------------------------------- */
#if PG_VERSION_NUM >= 190000
#define COLUMNAR_COPY_FOR_CLUSTER_ARGS \
	Relation OldTable, Relation NewTable, Relation OldIndex, bool use_sort, \
	TransactionId OldestXmin, Snapshot snapshot, TransactionId *xid_cutoff, \
	MultiXactId *multi_cutoff, double *num_tuples, double *tups_vacuumed, \
	double *tups_recently_dead
#else
#define COLUMNAR_COPY_FOR_CLUSTER_ARGS \
	Relation OldTable, Relation NewTable, Relation OldIndex, bool use_sort, \
	TransactionId OldestXmin, TransactionId *xid_cutoff, \
	MultiXactId *multi_cutoff, double *num_tuples, double *tups_vacuumed, \
	double *tups_recently_dead
#endif

/* -------------------------------------------------------------------------
 * scan_analyze_next_tuple() dropped its TransactionId OldestXmin argument in
 * PG19 (the caller now derives it). Our stub collects no statistics.
 * ------------------------------------------------------------------------- */
#if PG_VERSION_NUM >= 190000
#define COLUMNAR_ANALYZE_NEXT_TUPLE_ARGS \
	TableScanDesc scan, double *liverows, double *deadrows, TupleTableSlot *slot
#else
#define COLUMNAR_ANALYZE_NEXT_TUPLE_ARGS \
	TableScanDesc scan, TransactionId OldestXmin, double *liverows, \
	double *deadrows, TupleTableSlot *slot
#endif

/* -------------------------------------------------------------------------
 * scan_analyze_next_block() took (BlockNumber, BufferAccessStrategy) through
 * PG16 and (ReadStream *) from PG17 (the read-stream ANALYZE rework). Pre-17
 * has no ReadStream type. The callback is compiled per major in
 * columnar_tableam.c; this macro supplies its parameter list.
 * ------------------------------------------------------------------------- */
#if PG_VERSION_NUM < 170000
#define COLUMNAR_ANALYZE_NEXT_BLOCK_ARGS \
	TableScanDesc scan, BlockNumber blockno, BufferAccessStrategy bstrategy
#else
#define COLUMNAR_ANALYZE_NEXT_BLOCK_ARGS \
	TableScanDesc scan, ReadStream *stream
#endif

/* -------------------------------------------------------------------------
 * relation_vacuum()'s VacuumParams pointer became const in PG19.
 * ------------------------------------------------------------------------- */
#if PG_VERSION_NUM >= 190000
#define COLUMNAR_VACUUM_PARAMS const VacuumParams *
#else
#define COLUMNAR_VACUUM_PARAMS struct VacuumParams *
#endif

/* -------------------------------------------------------------------------
 * Oldest-xmin horizon for all-visible determination. GetOldestXmin(rel, flags)
 * was replaced by GetOldestNonRemovableTransactionId(rel) in PG14. A stripe
 * whose insert xid precedes this is visible to every current and future
 * snapshot. Callers include "storage/procarray.h".
 * ------------------------------------------------------------------------- */
#if PG_VERSION_NUM >= 140000
#define ColumnarOldestXmin(rel) GetOldestNonRemovableTransactionId(rel)
#else
#define ColumnarOldestXmin(rel) GetOldestXmin((rel), PROCARRAY_FLAGS_VACUUM)
#endif

/* -------------------------------------------------------------------------
 * palloc_aligned() and PG_IO_ALIGN_SIZE arrived in PG16 (for direct I/O). We
 * use them to allocate the metapage buffer. Pre-16, a plain zeroed palloc is
 * correct: palloc memory is MAXALIGN'd, which is all the page routines and
 * smgrextend require on those majors.
 * ------------------------------------------------------------------------- */
#if PG_VERSION_NUM < 160000
#define ColumnarAllocPage() ((Page) palloc0(BLCKSZ))
#else
#define ColumnarAllocPage() \
	((Page) palloc_aligned(BLCKSZ, PG_IO_ALIGN_SIZE, MCXT_ALLOC_ZERO))
#endif

/* -------------------------------------------------------------------------
 * get_rel_relam(relid) -> the relation's table/index AM oid was added to
 * lsyscache.h in PG17. Provide the identical lookup on older majors: read
 * pg_class.relam from the relcache-backed syscache, InvalidOid if not found.
 * ------------------------------------------------------------------------- */
#if PG_VERSION_NUM < 170000
#include "access/htup_details.h"
#include "catalog/pg_class.h"
#include "utils/syscache.h"

static inline Oid
get_rel_relam(Oid relid)
{
	HeapTuple	tp;
	Oid			result = InvalidOid;

	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_class reltup = (Form_pg_class) GETSTRUCT(tp);

		result = reltup->relam;
		ReleaseSysCache(tp);
	}
	return result;
}
#endif

/* -------------------------------------------------------------------------
 * EmitWarningsOnPlaceholders() was renamed MarkGUCPrefixReserved() in PG15
 * (PG15+ keep the old name as a macro alias). We use the new name everywhere
 * and alias it back on the two majors that predate it.
 * ------------------------------------------------------------------------- */
#if PG_VERSION_NUM < 150000
#define MarkGUCPrefixReserved(className) EmitWarningsOnPlaceholders(className)
#endif

/* -------------------------------------------------------------------------
 * fmgr.h started declaring the _PG_init prototype centrally in PG16. Declare
 * it ourselves on older majors so the module compiles clean under
 * -Wmissing-prototypes.
 * ------------------------------------------------------------------------- */
#if PG_VERSION_NUM < 160000
extern void _PG_init(void);
#endif

/* -------------------------------------------------------------------------
 * RelationSetNewRelfilenumber() (PG16+) was RelationSetNewRelfilenode() before
 * PG16 (the relfilenode -> relfilenumber rename). Same (Relation, char) args.
 * ------------------------------------------------------------------------- */
#if PG_VERSION_NUM < 160000
#define RelationSetNewRelfilenumber(rel, persistence) \
	RelationSetNewRelfilenode((rel), (persistence))
#endif

/* -------------------------------------------------------------------------
 * reindex_relation() changed its parameter list twice:
 *   PG13:  reindex_relation(Oid relid, int flags, int options)
 *   PG14-16: reindex_relation(Oid relid, int flags, ReindexParams *params)
 *   PG17+: reindex_relation(const ReindexStmt *stmt, Oid relid, int flags,
 *                           const ReindexParams *params)
 * We only ever want a plain rebuild (no special options / no statement), so a
 * thin wrapper hides the difference. ReindexParams did not exist before PG14.
 * catalog/index.h supplies the declaration and the type.
 * ------------------------------------------------------------------------- */
#include "catalog/index.h"

static inline void
ColumnarReindexRelation(Oid relid, int flags)
{
#if PG_VERSION_NUM < 140000
	reindex_relation(relid, flags, 0);
#elif PG_VERSION_NUM < 170000
	ReindexParams params = {0};

	reindex_relation(relid, flags, &params);
#else
	ReindexParams params = {0};

	reindex_relation(NULL, relid, flags, &params);
#endif
}

/* -------------------------------------------------------------------------
 * The shared parallel block-scan descriptor identified the relation by Oid
 * (phs_relid) through PG17; PG18 replaced it with the physical locator
 * (phs_locator, a RelFileLocator). We never actually run a parallel columnar
 * scan (the planner path is suppressed), but the initializer must still set the
 * right field so it compiles and mirrors heap's own initializer.
 * ------------------------------------------------------------------------- */
#if PG_VERSION_NUM >= 180000
#define COLUMNAR_PARALLELSCAN_SET_REL(bpscan, rel) \
	((bpscan)->base.phs_locator = (rel)->rd_locator)
#else
#define COLUMNAR_PARALLELSCAN_SET_REL(bpscan, rel) \
	((bpscan)->base.phs_relid = RelationGetRelid(rel))
#endif

/* -------------------------------------------------------------------------
 * PageSetChecksumInplace() was renamed PageSetChecksum() in PG19 (there is no
 * longer a separate copying variant). Same (Page, BlockNumber) arguments and
 * same in-place effect.
 * ------------------------------------------------------------------------- */
#if PG_VERSION_NUM >= 190000
#include "storage/bufpage.h"
#define PageSetChecksumInplace(page, blkno) PageSetChecksum((page), (blkno))
#endif

/* -------------------------------------------------------------------------
 * tuplesort_begin_heap()'s final argument was a bool randomAccess through
 * PG15; PG16 replaced it with an int sortopt bitmask. We never need random
 * access to the sorted output (we read it once, forward), so pass the
 * "no special options" value under both spellings.
 * ------------------------------------------------------------------------- */
#if PG_VERSION_NUM >= 160000
#define COLUMNAR_TUPLESORT_NONACCESS TUPLESORT_NONE
#else
#define COLUMNAR_TUPLESORT_NONACCESS false
#endif

/* -------------------------------------------------------------------------
 * create_foreignscan_path() gained parameters over time: PG17 inserted a
 * fdw_restrictinfo list before fdw_private, and PG19 inserted an int
 * disabled_nodes after rows. We push no restrictinfo and no disabled nodes, so
 * this wrapper takes the historical ten arguments and fills the extras.
 * ------------------------------------------------------------------------- */
#if PG_VERSION_NUM >= 190000
#define COLUMNAR_CREATE_FOREIGNSCAN_PATH(root, rel, target, rows, startup, total, pathkeys, req_outer, fdw_outer, fdw_priv) \
	create_foreignscan_path((root), (rel), (target), (rows), 0, (startup), (total), \
							(pathkeys), (req_outer), (fdw_outer), NIL, (fdw_priv))
#elif PG_VERSION_NUM >= 170000
#define COLUMNAR_CREATE_FOREIGNSCAN_PATH(root, rel, target, rows, startup, total, pathkeys, req_outer, fdw_outer, fdw_priv) \
	create_foreignscan_path((root), (rel), (target), (rows), (startup), (total), \
							(pathkeys), (req_outer), (fdw_outer), NIL, (fdw_priv))
#else
#define COLUMNAR_CREATE_FOREIGNSCAN_PATH(root, rel, target, rows, startup, total, pathkeys, req_outer, fdw_outer, fdw_priv) \
	create_foreignscan_path((root), (rel), (target), (rows), (startup), (total), \
							(pathkeys), (req_outer), (fdw_outer), (fdw_priv))
#endif

#endif							/* COLUMNAR_COMPAT_H */
