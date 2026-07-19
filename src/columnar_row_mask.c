/*-------------------------------------------------------------------------
 *
 * columnar_row_mask.c
 *		Delete and update marking for pgColumnar (spec 7.5, 9). Deletes do not
 *		rewrite stripes; instead a bit is set in the columnar.row_mask entry for
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
typedef struct RowMaskChunkBuffer
{
	uint64		stripeId;
	int			chunkId;
	uint64		startRowNumber;
	uint64		endRowNumber;
	uint64		rowCount;
	char	   *mask;			/* maskLen bytes */
	uint32		maskLen;
} RowMaskChunkBuffer;

/* pending delete marks for one storage id under one subtransaction */
typedef struct RowMaskBuffer
{
	Oid			relid;
	uint64		storageId;
	SubTransactionId subid;
	List	   *chunks;			/* list of RowMaskChunkBuffer* */
	List	   *stripeCache;	/* cached StripeMetadata* for resolution */
} RowMaskBuffer;

static MemoryContext ColumnarRowMaskContext = NULL;
static List *ColumnarRowMaskBuffers = NIL;

static RowMaskBuffer *rowmask_get_buffer(Relation rel, uint64 storageId);
static StripeMetadata *rowmask_find_stripe(RowMaskBuffer *buf, uint64 rowNumber);
static RowMaskChunkBuffer *rowmask_get_chunk(RowMaskBuffer *buf,
											 uint64 stripeId, int chunkId,
											 uint64 startRowNumber,
											 uint64 endRowNumber,
											 uint64 rowCount);
static void rowmask_flush_buffer(RowMaskBuffer *buf);

/*
 * rowmask_chunk_cmp
 *		Total order over chunk-group buffers by (stripeId, chunkId,
 *		startRowNumber). Flushing in this order makes every transaction acquire
 *		the per-chunk-group locks (columnar_metadata.c) in the same global
 *		order, so two concurrent deleters cannot form an AB-BA deadlock cycle.
 */
static int
rowmask_chunk_cmp(const ListCell *a, const ListCell *b)
{
	const RowMaskChunkBuffer *ca = (const RowMaskChunkBuffer *) lfirst(a);
	const RowMaskChunkBuffer *cb = (const RowMaskChunkBuffer *) lfirst(b);

	if (ca->stripeId != cb->stripeId)
		return ca->stripeId < cb->stripeId ? -1 : 1;
	if (ca->chunkId != cb->chunkId)
		return ca->chunkId < cb->chunkId ? -1 : 1;
	if (ca->startRowNumber != cb->startRowNumber)
		return ca->startRowNumber < cb->startRowNumber ? -1 : 1;
	return 0;
}

/*
 * rowmask_get_buffer
 *		Find or create the delete buffer for a storage id under the current
 *		subtransaction.
 */
static RowMaskBuffer *
rowmask_get_buffer(Relation rel, uint64 storageId)
{
	SubTransactionId subid = GetCurrentSubTransactionId();
	ListCell   *lc;
	MemoryContext oldContext;
	RowMaskBuffer *buf;

	foreach(lc, ColumnarRowMaskBuffers)
	{
		buf = (RowMaskBuffer *) lfirst(lc);
		if (buf->storageId == storageId && buf->subid == subid)
			return buf;
	}

	if (ColumnarRowMaskContext == NULL)
		ColumnarRowMaskContext = AllocSetContextCreate(TopTransactionContext,
													   "columnar row mask",
													   ALLOCSET_DEFAULT_SIZES);

	oldContext = MemoryContextSwitchTo(ColumnarRowMaskContext);
	buf = palloc0(sizeof(RowMaskBuffer));
	buf->relid = RelationGetRelid(rel);
	buf->storageId = storageId;
	buf->subid = subid;
	buf->chunks = NIL;
	buf->stripeCache = NIL;
	ColumnarRowMaskBuffers = lappend(ColumnarRowMaskBuffers, buf);
	MemoryContextSwitchTo(oldContext);

	return buf;
}

/*
 * rowmask_find_stripe
 *		Return the stripe metadata for the stripe that contains rowNumber,
 *		rebuilding the buffer's cached stripe list from the catalog when the
 *		cache is empty or does not cover the row (e.g. after a stripe was
 *		flushed later in this same transaction).
 */
static StripeMetadata *
rowmask_find_stripe(RowMaskBuffer *buf, uint64 rowNumber)
{
	ListCell   *lc;
	int			attempt;

	for (attempt = 0; attempt < 2; attempt++)
	{
		foreach(lc, buf->stripeCache)
		{
			StripeMetadata *s = (StripeMetadata *) lfirst(lc);

			if (rowNumber >= s->firstRowNumber &&
				rowNumber < s->firstRowNumber + s->rowCount)
				return s;
		}

		/* miss: rebuild the cache once with an up-to-date catalog snapshot */
		if (attempt == 0)
		{
			MemoryContext oldContext =
				MemoryContextSwitchTo(ColumnarRowMaskContext);
			Snapshot	snap = ColumnarCatalogSnapshot(GetActiveSnapshot());

			buf->stripeCache = ColumnarReadStripeList(buf->storageId, snap);
			MemoryContextSwitchTo(oldContext);
		}
	}

	return NULL;
}

/*
 * rowmask_get_chunk
 *		Find or create the chunk-group delete buffer for a chunk group.
 */
static RowMaskChunkBuffer *
rowmask_get_chunk(RowMaskBuffer *buf, uint64 stripeId, int chunkId,
				  uint64 startRowNumber, uint64 endRowNumber, uint64 rowCount)
{
	ListCell   *lc;
	MemoryContext oldContext;
	RowMaskChunkBuffer *chunk;

	foreach(lc, buf->chunks)
	{
		chunk = (RowMaskChunkBuffer *) lfirst(lc);
		if (chunk->stripeId == stripeId && chunk->chunkId == chunkId &&
			chunk->startRowNumber == startRowNumber)
			return chunk;
	}

	oldContext = MemoryContextSwitchTo(ColumnarRowMaskContext);
	chunk = palloc0(sizeof(RowMaskChunkBuffer));
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
	RowMaskBuffer *buf = rowmask_get_buffer(rel, storageId);
	StripeMetadata *stripe = rowmask_find_stripe(buf, rowNumber);
	uint64		offsetInStripe;
	int			chunkId;
	uint64		startRowNumber;
	uint64		groupRowCount;
	uint64		endRowNumber;
	uint64		bitIndex;
	RowMaskChunkBuffer *chunk;

	if (stripe == NULL)
		elog(ERROR,
			 "columnar: cannot delete row " UINT64_FORMAT
			 ": no stripe covers it", rowNumber);

	offsetInStripe = rowNumber - stripe->firstRowNumber;
	chunkId = (int) (offsetInStripe / (uint64) stripe->chunkRowCount);
	startRowNumber = stripe->firstRowNumber +
		(uint64) chunkId * (uint64) stripe->chunkRowCount;
	groupRowCount = stripe->rowCount - (uint64) chunkId * (uint64) stripe->chunkRowCount;
	if (groupRowCount > (uint64) stripe->chunkRowCount)
		groupRowCount = (uint64) stripe->chunkRowCount;
	endRowNumber = startRowNumber + groupRowCount - 1;

	chunk = rowmask_get_chunk(buf, stripe->stripeNum, chunkId,
							  startRowNumber, endRowNumber, groupRowCount);

	bitIndex = rowNumber - startRowNumber;
	chunk->mask[bitIndex >> 3] |= (char) (1 << (bitIndex & 7));
}

/*
 * rowmask_flush_buffer
 *		Apply one buffer's accumulated marks to the catalog and empty it. Each
 *		chunk group is upserted exactly once, so no catalog tuple is updated
 *		more than once in this command.
 */
static void
rowmask_flush_buffer(RowMaskBuffer *buf)
{
	ListCell   *lc;
	bool		pushedSnapshot = false;

	if (buf->chunks == NIL)
		return;

	/* deterministic lock-acquisition order across transactions (issue #4) */
	list_sort(buf->chunks, rowmask_chunk_cmp);

	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushedSnapshot = true;
	}

	foreach(lc, buf->chunks)
	{
		RowMaskChunkBuffer *chunk = (RowMaskChunkBuffer *) lfirst(lc);
		RowMaskMetadata rm;
		uint32		i;
		int			bit;
		int			deleted = 0;

		for (i = 0; i < chunk->maskLen; i++)
			for (bit = 0; bit < 8; bit++)
				if (chunk->mask[i] & (1 << bit))
					deleted++;

		rm.id = ColumnarNextRowMaskId();
		rm.stripeId = chunk->stripeId;
		rm.chunkId = chunk->chunkId;
		rm.startRowNumber = chunk->startRowNumber;
		rm.endRowNumber = chunk->endRowNumber;
		rm.deletedRows = deleted;
		rm.mask = chunk->mask;
		rm.maskLen = chunk->maskLen;

		ColumnarUpsertRowMask(buf->storageId, &rm);
	}

	if (pushedSnapshot)
		PopActiveSnapshot();

	buf->chunks = NIL;
	buf->stripeCache = NIL;
}

/*
 * ColumnarFlushRowMaskForRelation
 *		Flush pending delete marks for one relation. Called at scan start so a
 *		delete made earlier in this transaction is visible to a later scan.
 */
void
ColumnarFlushRowMaskForRelation(Relation rel)
{
	Oid			relid = RelationGetRelid(rel);
	ListCell   *lc;

	foreach(lc, ColumnarRowMaskBuffers)
	{
		RowMaskBuffer *buf = (RowMaskBuffer *) lfirst(lc);

		if (buf->relid == relid)
			rowmask_flush_buffer(buf);
	}
}

/*
 * ColumnarFlushAllRowMasks
 *		Flush every pending delete buffer. Called at transaction pre-commit.
 */
void
ColumnarFlushAllRowMasks(void)
{
	ListCell   *lc;

	foreach(lc, ColumnarRowMaskBuffers)
		rowmask_flush_buffer((RowMaskBuffer *) lfirst(lc));
}

/*
 * ColumnarDiscardAllRowMasks
 *		Forget all pending delete buffers (transaction end).
 */
void
ColumnarDiscardAllRowMasks(void)
{
	ColumnarRowMaskBuffers = NIL;
	ColumnarRowMaskContext = NULL;
}

/*
 * ColumnarRowMaskDiscardSubXact
 *		Drop delete buffers made in an aborting subtransaction. The catalog
 *		rows they would have produced were never written (or, if a scan flushed
 *		them, are made invisible by the subtransaction abort itself).
 */
void
ColumnarRowMaskDiscardSubXact(SubTransactionId subid)
{
	List	   *kept = NIL;
	ListCell   *lc;
	MemoryContext oldContext;

	if (ColumnarRowMaskBuffers == NIL)
		return;

	oldContext = MemoryContextSwitchTo(ColumnarRowMaskContext);
	foreach(lc, ColumnarRowMaskBuffers)
	{
		RowMaskBuffer *buf = (RowMaskBuffer *) lfirst(lc);

		if (buf->subid != subid)
			kept = lappend(kept, buf);
	}
	MemoryContextSwitchTo(oldContext);

	ColumnarRowMaskBuffers = kept;
}

/*
 * ColumnarRowMaskPromoteSubXact
 *		On subtransaction commit, reassign its delete buffers to the parent so
 *		they survive until the parent resolves.
 */
void
ColumnarRowMaskPromoteSubXact(SubTransactionId subid, SubTransactionId parent)
{
	ListCell   *lc;

	foreach(lc, ColumnarRowMaskBuffers)
	{
		RowMaskBuffer *buf = (RowMaskBuffer *) lfirst(lc);

		if (buf->subid == subid)
			buf->subid = parent;
	}
}
