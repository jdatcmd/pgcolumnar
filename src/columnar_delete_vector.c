/*-------------------------------------------------------------------------
 *
 * columnar_delete_vector.c
 *		Delete and update marking for pgColumnar (spec 7.5, 9). Deletes do not
 *		rewrite stripes; instead a bit is set in the columnar.delete_vector entry for
 *		the affected chunk group. Update is delete-plus-insert, so it also uses
 *		this path for the old row.
 *
 * Marks are accumulated in a per-(sub)transaction in-memory buffer and applied
 * to the catalog in a single pass at flush time. Buffering is required because
 * many rows in one chunk group are typically deleted by a single command, and
 * a shared catalog tuple must not be heap-updated more than once per command.
 * The buffer is flushed at scan start of the same relation (read-your-writes)
 * and at transaction pre-commit, and discarded on rollback and on the rollback
 * of the subtransaction that made the marks.
 *
 * Independent MIT implementation built from design/FORMAT_AND_INTERFACE_SPEC.md
 * (format 2.0) and the public PostgreSQL 17 API only.
 *
 *-------------------------------------------------------------------------
 */
#include "columnar.h"

#include "access/xact.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

/* one chunk group's accumulated delete bits */
typedef struct DeleteVectorChunkBuffer
{
	uint64		stripeId;
	int			chunkId;
	uint64		startRowNumber;
	uint64		endRowNumber;
	uint64		rowCount;
	char	   *mask;			/* maskLen bytes */
	uint32		maskLen;
} DeleteVectorChunkBuffer;

/* pending delete marks for one storage id under one subtransaction */
typedef struct DeleteVectorBuffer
{
	Oid			relid;
	uint64		storageId;
	SubTransactionId subid;
	List	   *chunks;			/* list of DeleteVectorChunkBuffer* */
	List	   *rowGroupCache;	/* cached NativeRowGroupMetadata* for resolution */
} DeleteVectorBuffer;

static MemoryContext ColumnarDeleteVectorContext = NULL;
static List *ColumnarDeleteVectorBuffers = NIL;

static DeleteVectorBuffer *delete_vector_get_buffer(Relation rel, uint64 storageId);
static NativeRowGroupMetadata *delete_vector_find_row_group(DeleteVectorBuffer *buf,
													  uint64 rowNumber);
static DeleteVectorChunkBuffer *delete_vector_get_chunk(DeleteVectorBuffer *buf,
											 uint64 stripeId, int chunkId,
											 uint64 startRowNumber,
											 uint64 endRowNumber,
											 uint64 rowCount);
static void delete_vector_flush_buffer(DeleteVectorBuffer *buf);

/*
 * delete_vector_chunk_cmp
 *		Total order over chunk-group buffers by (stripeId, chunkId,
 *		startRowNumber). Flushing in this order makes every transaction acquire
 *		the per-chunk-group locks (columnar_metadata.c) in the same global
 *		order, so two concurrent deleters cannot form an AB-BA deadlock cycle.
 */
static int
delete_vector_chunk_cmp(const ListCell *a, const ListCell *b)
{
	const DeleteVectorChunkBuffer *ca = (const DeleteVectorChunkBuffer *) lfirst(a);
	const DeleteVectorChunkBuffer *cb = (const DeleteVectorChunkBuffer *) lfirst(b);

	if (ca->stripeId != cb->stripeId)
		return ca->stripeId < cb->stripeId ? -1 : 1;
	if (ca->chunkId != cb->chunkId)
		return ca->chunkId < cb->chunkId ? -1 : 1;
	if (ca->startRowNumber != cb->startRowNumber)
		return ca->startRowNumber < cb->startRowNumber ? -1 : 1;
	return 0;
}

/*
 * delete_vector_get_buffer
 *		Find or create the delete buffer for a storage id under the current
 *		subtransaction.
 */
static DeleteVectorBuffer *
delete_vector_get_buffer(Relation rel, uint64 storageId)
{
	SubTransactionId subid = GetCurrentSubTransactionId();
	ListCell   *lc;
	MemoryContext oldContext;
	DeleteVectorBuffer *buf;

	foreach(lc, ColumnarDeleteVectorBuffers)
	{
		buf = (DeleteVectorBuffer *) lfirst(lc);
		if (buf->storageId == storageId && buf->subid == subid)
			return buf;
	}

	if (ColumnarDeleteVectorContext == NULL)
		ColumnarDeleteVectorContext = AllocSetContextCreate(TopTransactionContext,
													   "columnar delete vector",
													   ALLOCSET_DEFAULT_SIZES);

	oldContext = MemoryContextSwitchTo(ColumnarDeleteVectorContext);
	buf = palloc0(sizeof(DeleteVectorBuffer));
	buf->relid = RelationGetRelid(rel);
	buf->storageId = storageId;
	buf->subid = subid;
	buf->chunks = NIL;
	buf->rowGroupCache = NIL;
	ColumnarDeleteVectorBuffers = lappend(ColumnarDeleteVectorBuffers, buf);
	MemoryContextSwitchTo(oldContext);

	return buf;
}


/*
 * delete_vector_find_row_group
 *		Native (PGCN v1) analog of delete_vector_find_stripe: return the row group that
 *		contains rowNumber, rebuilding the cache from the catalog on a miss.
 */
static NativeRowGroupMetadata *
delete_vector_find_row_group(DeleteVectorBuffer *buf, uint64 rowNumber)
{
	ListCell   *lc;
	int			attempt;

	for (attempt = 0; attempt < 2; attempt++)
	{
		foreach(lc, buf->rowGroupCache)
		{
			NativeRowGroupMetadata *g = (NativeRowGroupMetadata *) lfirst(lc);

			if (rowNumber >= g->firstRowNumber &&
				rowNumber < g->firstRowNumber + g->rowCount)
				return g;
		}

		if (attempt == 0)
		{
			MemoryContext oldContext =
				MemoryContextSwitchTo(ColumnarDeleteVectorContext);
			Snapshot	snap = ColumnarCatalogSnapshot(GetActiveSnapshot());

			buf->rowGroupCache = ColumnarReadRowGroupList(buf->storageId, snap);
			MemoryContextSwitchTo(oldContext);
		}
	}

	return NULL;
}

/*
 * delete_vector_get_chunk
 *		Find or create the chunk-group delete buffer for a chunk group.
 */
static DeleteVectorChunkBuffer *
delete_vector_get_chunk(DeleteVectorBuffer *buf, uint64 stripeId, int chunkId,
				  uint64 startRowNumber, uint64 endRowNumber, uint64 rowCount)
{
	ListCell   *lc;
	MemoryContext oldContext;
	DeleteVectorChunkBuffer *chunk;

	foreach(lc, buf->chunks)
	{
		chunk = (DeleteVectorChunkBuffer *) lfirst(lc);
		if (chunk->stripeId == stripeId && chunk->chunkId == chunkId &&
			chunk->startRowNumber == startRowNumber)
			return chunk;
	}

	oldContext = MemoryContextSwitchTo(ColumnarDeleteVectorContext);
	chunk = palloc0(sizeof(DeleteVectorChunkBuffer));
	chunk->stripeId = stripeId;
	chunk->chunkId = chunkId;
	chunk->startRowNumber = startRowNumber;
	chunk->endRowNumber = endRowNumber;
	chunk->rowCount = rowCount;
	chunk->maskLen = (uint32) ((rowCount + 7) / 8);
	chunk->mask = palloc0(chunk->maskLen);
	buf->chunks = lappend(buf->chunks, chunk);
	MemoryContextSwitchTo(oldContext);

	return chunk;
}

/*
 * ColumnarMarkRowDeleted
 *		Record that the row with the given 1-based row number is deleted, by
 *		setting its bit in the in-memory delete buffer for its chunk group.
 *		Chunk-group boundaries are computed arithmetically: every chunk group
 *		but the last in a stripe holds exactly chunkRowCount rows (the writer
 *		fills a group before starting the next), so no chunk_group catalog read
 *		is needed here.
 */
void
ColumnarMarkRowDeleted(Relation rel, uint64 rowNumber)
{
	uint64		storageId = ColumnarStorageId(rel);
	DeleteVectorBuffer *buf = delete_vector_get_buffer(rel, storageId);
	uint64		startRowNumber;
	uint64		endRowNumber;
	uint64		bitIndex;
	DeleteVectorChunkBuffer *chunk;
	NativeRowGroupMetadata *rg = delete_vector_find_row_group(buf, rowNumber);

	/*
	 * The whole row group is one bitmap (chunk id 0), sized to its row count. The
	 * vestigial stripe_id/chunk_id/start/end columns and group-number re-keying
	 * are pending cleanup (see design/PHASE_F_PLAN.md, F1).
	 */
	if (rg == NULL)
		elog(ERROR,
			 "columnar: cannot delete row " UINT64_FORMAT
			 ": no row group covers it", rowNumber);

	startRowNumber = rg->firstRowNumber;
	endRowNumber = startRowNumber + rg->rowCount - 1;
	chunk = delete_vector_get_chunk(buf, rg->groupNumber, 0,
							  startRowNumber, endRowNumber, rg->rowCount);
	bitIndex = rowNumber - startRowNumber;
	chunk->mask[bitIndex >> 3] |= (char) (1 << (bitIndex & 7));

	/*
	 * The deleted row makes its block not all-visible; clear any VM bit so an
	 * index-only scan never skips the fetch for a block with a dead row (gap 28).
	 * A no-op unless a prior vacuum had marked the block visible.
	 */
	ColumnarVMClearForRow(rel, rowNumber);
}

/*
 * ColumnarDeleteVectorBufferedDeleted
 *		True when the row is marked deleted in an in-memory row-mask buffer that
 *		has not yet been flushed to the catalog. The unique/primary-key check runs
 *		as part of an insert (or the insert half of an update) and fetches a
 *		conflicting row through ColumnarReadRowByNumber before the delete of the
 *		old row is flushed; consulting the buffer here lets a same-key UPDATE (the
 *		old row is buffered-deleted) proceed. Checks every buffer for the relation,
 *		across subtransactions.
 */
bool
ColumnarDeleteVectorBufferedDeleted(Relation rel, uint64 rowNumber)
{
	Oid			relid = RelationGetRelid(rel);
	ListCell   *lc;

	foreach(lc, ColumnarDeleteVectorBuffers)
	{
		DeleteVectorBuffer *buf = (DeleteVectorBuffer *) lfirst(lc);
		ListCell   *cc;

		if (buf->relid != relid)
			continue;

		foreach(cc, buf->chunks)
		{
			DeleteVectorChunkBuffer *chunk = (DeleteVectorChunkBuffer *) lfirst(cc);
			uint64		bitIndex;

			if (rowNumber < chunk->startRowNumber ||
				rowNumber > chunk->endRowNumber)
				continue;
			bitIndex = rowNumber - chunk->startRowNumber;
			if ((bitIndex >> 3) < chunk->maskLen &&
				(chunk->mask[bitIndex >> 3] & (1 << (bitIndex & 7))) != 0)
				return true;
		}
	}

	return false;
}

/*
 * delete_vector_flush_buffer
 *		Apply one buffer's accumulated marks to the catalog and empty it. Each
 *		chunk group is upserted exactly once, so no catalog tuple is updated
 *		more than once in this command.
 */
static void
delete_vector_flush_buffer(DeleteVectorBuffer *buf)
{
	ListCell   *lc;
	bool		pushedSnapshot = false;

	if (buf->chunks == NIL)
		return;

	/* deterministic lock-acquisition order across transactions (issue #4) */
	list_sort(buf->chunks, delete_vector_chunk_cmp);

	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushedSnapshot = true;
	}

	foreach(lc, buf->chunks)
	{
		DeleteVectorChunkBuffer *chunk = (DeleteVectorChunkBuffer *) lfirst(lc);
		DeleteVectorMetadata rm;
		uint32		i;
		int			bit;
		int			deleted = 0;

		for (i = 0; i < chunk->maskLen; i++)
			for (bit = 0; bit < 8; bit++)
				if (chunk->mask[i] & (1 << bit))
					deleted++;

		rm.id = ColumnarNextDeleteVectorId();
		rm.stripeId = chunk->stripeId;
		rm.chunkId = chunk->chunkId;
		rm.startRowNumber = chunk->startRowNumber;
		rm.endRowNumber = chunk->endRowNumber;
		rm.deletedRows = deleted;
		rm.mask = chunk->mask;
		rm.maskLen = chunk->maskLen;

		ColumnarUpsertDeleteVector(buf->storageId, &rm);
	}

	if (pushedSnapshot)
		PopActiveSnapshot();

	buf->chunks = NIL;
	buf->rowGroupCache = NIL;
}

/*
 * ColumnarFlushDeleteVectorForRelation
 *		Flush pending delete marks for one relation. Called at scan start so a
 *		delete made earlier in this transaction is visible to a later scan.
 */
void
ColumnarFlushDeleteVectorForRelation(Relation rel)
{
	Oid			relid = RelationGetRelid(rel);
	ListCell   *lc;

	foreach(lc, ColumnarDeleteVectorBuffers)
	{
		DeleteVectorBuffer *buf = (DeleteVectorBuffer *) lfirst(lc);

		if (buf->relid == relid)
			delete_vector_flush_buffer(buf);
	}
}

/*
 * ColumnarFlushAllDeleteVectors
 *		Flush every pending delete buffer. Called at transaction pre-commit.
 */
void
ColumnarFlushAllDeleteVectors(void)
{
	ListCell   *lc;

	foreach(lc, ColumnarDeleteVectorBuffers)
		delete_vector_flush_buffer((DeleteVectorBuffer *) lfirst(lc));
}

/*
 * ColumnarDiscardAllDeleteVectors
 *		Forget all pending delete buffers (transaction end).
 */
void
ColumnarDiscardAllDeleteVectors(void)
{
	ColumnarDeleteVectorBuffers = NIL;
	ColumnarDeleteVectorContext = NULL;
}

/*
 * ColumnarDeleteVectorDiscardSubXact
 *		Drop delete buffers made in an aborting subtransaction. The catalog
 *		rows they would have produced were never written (or, if a scan flushed
 *		them, are made invisible by the subtransaction abort itself).
 */
void
ColumnarDeleteVectorDiscardSubXact(SubTransactionId subid)
{
	List	   *kept = NIL;
	ListCell   *lc;
	MemoryContext oldContext;

	if (ColumnarDeleteVectorBuffers == NIL)
		return;

	oldContext = MemoryContextSwitchTo(ColumnarDeleteVectorContext);
	foreach(lc, ColumnarDeleteVectorBuffers)
	{
		DeleteVectorBuffer *buf = (DeleteVectorBuffer *) lfirst(lc);

		if (buf->subid != subid)
			kept = lappend(kept, buf);
	}
	MemoryContextSwitchTo(oldContext);

	ColumnarDeleteVectorBuffers = kept;
}

/*
 * ColumnarDeleteVectorPromoteSubXact
 *		On subtransaction commit, reassign its delete buffers to the parent so
 *		they survive until the parent resolves.
 */
void
ColumnarDeleteVectorPromoteSubXact(SubTransactionId subid, SubTransactionId parent)
{
	ListCell   *lc;

	foreach(lc, ColumnarDeleteVectorBuffers)
	{
		DeleteVectorBuffer *buf = (DeleteVectorBuffer *) lfirst(lc);

		if (buf->subid == subid)
			buf->subid = parent;
	}
}
