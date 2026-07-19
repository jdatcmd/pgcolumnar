/*-------------------------------------------------------------------------
 *
 * columnar_write_state.c
 *		The columnar writer: batch rows into chunk groups and stripes, and
 *		flush a stripe (data pages + catalog rows) when it fills or at
 *		transaction pre-commit (spec 4, 9).
 *
 * Pending writes are held per relation for the life of the transaction. On
 * commit they are flushed at pre-commit; on abort they are discarded with
 * the transaction memory.
 *
 *-------------------------------------------------------------------------
 */
#include "columnar.h"

#include "access/table.h"
#include "storage/lmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

/* one column's two streams within one chunk group */
typedef struct ColumnChunkBuffer
{
	StringInfoData valueStream;
	StringInfoData existsStream;
	uint64		valueCount;
} ColumnChunkBuffer;

/* one chunk group: all columns for a horizontal slice of rows */
typedef struct ChunkGroupBuffer
{
	uint64		rowCount;
	ColumnChunkBuffer *columns;		/* array [natts] */
} ChunkGroupBuffer;

struct ColumnarWriteState
{
	Oid			relid;
	TupleDesc	tupdesc;			/* copy owned by writeContext */
	int			natts;
	int			stripeRowLimit;
	int			chunkGroupRowLimit;
	uint64		storageId;

	MemoryContext writeContext;		/* lives for the transaction */
	MemoryContext stripeContext;	/* reset after each stripe flush */

	List	   *chunkGroups;		/* list of ChunkGroupBuffer* */
	ChunkGroupBuffer *currentGroup;
	uint64		stripeRowCount;
};

/* per-backend registry of pending write states, in ColumnarWriteContext */
static MemoryContext ColumnarWriteContext = NULL;
static List *ColumnarWriteStates = NIL;

static void columnar_flush_stripe(ColumnarWriteState *writeState);
static ChunkGroupBuffer *columnar_start_chunk_group(ColumnarWriteState *writeState);

/*
 * ColumnarGetWriteState
 *		Find or create the pending write state for a relation.
 */
ColumnarWriteState *
ColumnarGetWriteState(Relation rel)
{
	Oid			relid = RelationGetRelid(rel);
	ListCell   *lc;
	MemoryContext oldContext;
	ColumnarWriteState *writeState;

	foreach(lc, ColumnarWriteStates)
	{
		writeState = (ColumnarWriteState *) lfirst(lc);
		if (writeState->relid == relid)
			return writeState;
	}

	if (ColumnarWriteContext == NULL)
		ColumnarWriteContext = AllocSetContextCreate(TopTransactionContext,
													 "columnar write",
													 ALLOCSET_DEFAULT_SIZES);

	oldContext = MemoryContextSwitchTo(ColumnarWriteContext);

	writeState = palloc0(sizeof(ColumnarWriteState));
	writeState->relid = relid;
	writeState->tupdesc = CreateTupleDescCopy(RelationGetDescr(rel));
	writeState->natts = writeState->tupdesc->natts;
	writeState->stripeRowLimit = columnar_stripe_row_limit;
	writeState->chunkGroupRowLimit = columnar_chunk_group_row_limit;
	writeState->storageId = ColumnarStorageId(rel);
	writeState->stripeContext = AllocSetContextCreate(ColumnarWriteContext,
													  "columnar stripe",
													  ALLOCSET_DEFAULT_SIZES);
	writeState->writeContext = ColumnarWriteContext;
	writeState->chunkGroups = NIL;
	writeState->currentGroup = NULL;
	writeState->stripeRowCount = 0;

	ColumnarWriteStates = lappend(ColumnarWriteStates, writeState);

	MemoryContextSwitchTo(oldContext);

	return writeState;
}

/*
 * columnar_start_chunk_group
 *		Begin a new chunk group inside the current stripe, allocated in the
 *		stripe memory context.
 */
static ChunkGroupBuffer *
columnar_start_chunk_group(ColumnarWriteState *writeState)
{
	MemoryContext oldContext = MemoryContextSwitchTo(writeState->stripeContext);
	ChunkGroupBuffer *group = palloc0(sizeof(ChunkGroupBuffer));
	int			c;

	group->rowCount = 0;
	group->columns = palloc0(sizeof(ColumnChunkBuffer) * writeState->natts);
	for (c = 0; c < writeState->natts; c++)
	{
		initStringInfo(&group->columns[c].valueStream);
		initStringInfo(&group->columns[c].existsStream);
		group->columns[c].valueCount = 0;
	}

	writeState->chunkGroups = lappend(writeState->chunkGroups, group);
	writeState->currentGroup = group;

	MemoryContextSwitchTo(oldContext);
	return group;
}

/*
 * ColumnarWriteRow
 *		Append one row to the current stripe, opening a new chunk group when
 *		the current one is full and flushing the stripe when it reaches the
 *		stripe row limit.
 */
void
ColumnarWriteRow(ColumnarWriteState *writeState, Datum *values, bool *nulls)
{
	ChunkGroupBuffer *group = writeState->currentGroup;
	int			c;

	if (group == NULL ||
		group->rowCount >= (uint64) writeState->chunkGroupRowLimit)
		group = columnar_start_chunk_group(writeState);

	for (c = 0; c < writeState->natts; c++)
	{
		ColumnChunkBuffer *col = &group->columns[c];
		Form_pg_attribute att = TupleDescAttr(writeState->tupdesc, c);

		if (nulls[c])
		{
			appendStringInfoChar(&col->existsStream, 0);
		}
		else
		{
			appendStringInfoChar(&col->existsStream, 1);
			ColumnarEncodeValue(&col->valueStream, att, values[c]);
			col->valueCount++;
		}
	}

	group->rowCount++;
	writeState->stripeRowCount++;

	if (writeState->stripeRowCount >= (uint64) writeState->stripeRowLimit)
		columnar_flush_stripe(writeState);
}

/*
 * columnar_flush_stripe
 *		Lay out the accumulated stripe as a single contiguous byte buffer,
 *		reserve space, write the data pages, and record the stripe, chunk
 *		group, and chunk rows in the catalog (spec 4, 7). Then reset the
 *		stripe accumulator.
 */
static void
columnar_flush_stripe(ColumnarWriteState *writeState)
{
	MemoryContext flushContext;
	MemoryContext oldContext;
	Relation	rel;
	int			natts = writeState->natts;
	int			numGroups;
	StringInfo	data;
	ChunkMetadata *chunkMeta;
	uint64		stripeId;
	uint64		firstRowNumber;
	uint64		fileOffset;
	uint64		dataLength;
	StripeMetadata stripe;
	ListCell   *lc;
	int			c;
	int			g;
	bool		pushedSnapshot = false;

	if (writeState->stripeRowCount == 0)
		return;

	/*
	 * Catalog inserts need an active snapshot. At transaction pre-commit the
	 * executor snapshot has already been popped, so push a transaction
	 * snapshot for the duration of the flush.
	 */
	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushedSnapshot = true;
	}

	numGroups = list_length(writeState->chunkGroups);

	flushContext = AllocSetContextCreate(CurrentMemoryContext,
										 "columnar flush",
										 ALLOCSET_DEFAULT_SIZES);
	oldContext = MemoryContextSwitchTo(flushContext);

	/*
	 * Build the stripe buffer column-major: for each column, the
	 * concatenation of that column's chunks across all chunk groups, each
	 * chunk laid out as [value stream][exists stream] (spec 4). Offsets and
	 * lengths are recorded relative to the stripe start.
	 */
	data = makeStringInfo();
	chunkMeta = palloc0(sizeof(ChunkMetadata) * natts * numGroups);

	for (c = 0; c < natts; c++)
	{
		g = 0;
		foreach(lc, writeState->chunkGroups)
		{
			ChunkGroupBuffer *group = (ChunkGroupBuffer *) lfirst(lc);
			ColumnChunkBuffer *col = &group->columns[c];
			ChunkMetadata *m = &chunkMeta[c * numGroups + g];

			m->attrNum = c + 1;
			m->chunkGroupNum = g;

			m->valueStreamOffset = data->len;
			appendBinaryStringInfo(data, col->valueStream.data,
								   col->valueStream.len);
			m->valueStreamLength = col->valueStream.len;

			m->existsStreamOffset = data->len;
			appendBinaryStringInfo(data, col->existsStream.data,
								   col->existsStream.len);
			m->existsStreamLength = col->existsStream.len;

			m->valueCompressionType = COLUMNAR_COMPRESSION_NONE;
			m->valueCompressionLevel = 0;
			m->valueDecompressedLength = col->valueStream.len;
			m->valueCount = col->valueCount;

			g++;
		}
	}

	dataLength = data->len;

	/* reserve and write the stripe's data pages under the extension lock */
	rel = table_open(writeState->relid, RowExclusiveLock);

	LockRelationForExtension(rel, ExclusiveLock);
	ColumnarReserveStripe(rel, writeState->stripeRowCount, dataLength,
						  &stripeId, &firstRowNumber, &fileOffset);
	if (dataLength > 0)
		ColumnarWriteLogicalData(rel, fileOffset, data->data, dataLength);
	UnlockRelationForExtension(rel, ExclusiveLock);

	/* record the stripe */
	stripe.storageId = writeState->storageId;
	stripe.stripeNum = stripeId;
	stripe.fileOffset = fileOffset;
	stripe.dataLength = dataLength;
	stripe.columnCount = natts;
	stripe.chunkRowCount = writeState->chunkGroupRowLimit;
	stripe.rowCount = writeState->stripeRowCount;
	stripe.chunkGroupCount = numGroups;
	stripe.firstRowNumber = firstRowNumber;
	ColumnarInsertStripeRow(&stripe);

	/* record chunk groups */
	g = 0;
	foreach(lc, writeState->chunkGroups)
	{
		ChunkGroupBuffer *group = (ChunkGroupBuffer *) lfirst(lc);
		ChunkGroupMetadata cg;

		cg.stripeNum = stripeId;
		cg.chunkGroupNum = g;
		cg.rowCount = group->rowCount;
		cg.deletedRows = 0;
		ColumnarInsertChunkGroupRow(writeState->storageId, &cg);
		g++;
	}

	/* record chunks */
	for (c = 0; c < natts; c++)
	{
		for (g = 0; g < numGroups; g++)
		{
			ChunkMetadata *m = &chunkMeta[c * numGroups + g];

			m->stripeNum = stripeId;
			ColumnarInsertChunkRow(writeState->storageId, m);
		}
	}

	table_close(rel, RowExclusiveLock);

	MemoryContextSwitchTo(oldContext);
	MemoryContextDelete(flushContext);

	/* reset stripe accumulation */
	MemoryContextReset(writeState->stripeContext);
	writeState->chunkGroups = NIL;
	writeState->currentGroup = NULL;
	writeState->stripeRowCount = 0;

	if (pushedSnapshot)
		PopActiveSnapshot();
}

/*
 * ColumnarFlushWriteStateForRelation
 *		Flush any pending partial stripe for a single relation. Used at scan
 *		start so data written earlier in this transaction is persisted.
 */
void
ColumnarFlushWriteStateForRelation(Oid relid)
{
	ListCell   *lc;

	foreach(lc, ColumnarWriteStates)
	{
		ColumnarWriteState *writeState = (ColumnarWriteState *) lfirst(lc);

		if (writeState->relid == relid && writeState->stripeRowCount > 0)
			columnar_flush_stripe(writeState);
	}
}

/*
 * ColumnarFlushAllPendingWrites
 *		Flush every pending write state. Called at transaction pre-commit.
 */
void
ColumnarFlushAllPendingWrites(void)
{
	ListCell   *lc;

	foreach(lc, ColumnarWriteStates)
		columnar_flush_stripe((ColumnarWriteState *) lfirst(lc));
}

/*
 * ColumnarDiscardAllPendingWrites
 *		Forget all pending write states. The backing memory is freed with the
 *		transaction context, so we only clear our static references.
 */
void
ColumnarDiscardAllPendingWrites(void)
{
	ColumnarWriteStates = NIL;
	ColumnarWriteContext = NULL;
}
