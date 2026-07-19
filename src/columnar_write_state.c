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
#include "access/xact.h"
#include "storage/lmgr.h"
#include "utils/datum.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/typcache.h"

/* per-column, per-write-state facts needed for the min/max skip list */
typedef struct ColumnarColumnDef
{
	bool		orderable;		/* type has a default btree comparison proc */
	FmgrInfo	cmpFn;			/* the comparison proc, when orderable */
	Oid			collation;		/* collation to compare under */
} ColumnarColumnDef;

/* one column's two streams within one chunk group */
typedef struct ColumnChunkBuffer
{
	StringInfoData valueStream;
	StringInfoData existsStream;
	uint64		valueCount;

	/* running min/max of the non-null values seen in this chunk */
	bool		hasMinMax;
	Datum		minValue;		/* held in the stripe context */
	Datum		maxValue;
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
	SubTransactionId subid;			/* subtransaction that owns the buffer */
	TupleDesc	tupdesc;			/* copy owned by writeContext */
	int			natts;
	int			stripeRowLimit;
	int			chunkGroupRowLimit;
	int			compressionType;	/* columnar.compression at open time */
	int			compressionLevel;	/* columnar.compression_level at open time */
	uint64		storageId;
	ColumnarColumnDef *colDefs;		/* array [natts], in writeContext */

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
	SubTransactionId subid = GetCurrentSubTransactionId();
	ListCell   *lc;
	MemoryContext oldContext;
	ColumnarWriteState *writeState;

	/*
	 * A write state is keyed by (relation, subtransaction) so that a buffer
	 * never mixes rows written under different subtransactions. That keeps the
	 * rollback of a subtransaction a simple matter of dropping its buffers
	 * (spec 9).
	 */
	foreach(lc, ColumnarWriteStates)
	{
		writeState = (ColumnarWriteState *) lfirst(lc);
		if (writeState->relid == relid && writeState->subid == subid)
			return writeState;
	}

	if (ColumnarWriteContext == NULL)
		ColumnarWriteContext = AllocSetContextCreate(TopTransactionContext,
													 "columnar write",
													 ALLOCSET_DEFAULT_SIZES);

	oldContext = MemoryContextSwitchTo(ColumnarWriteContext);

	writeState = palloc0(sizeof(ColumnarWriteState));
	writeState->relid = relid;
	writeState->subid = subid;
	writeState->tupdesc = CreateTupleDescCopy(RelationGetDescr(rel));
	writeState->natts = writeState->tupdesc->natts;
	writeState->stripeRowLimit = columnar_stripe_row_limit;
	writeState->chunkGroupRowLimit = columnar_chunk_group_row_limit;
	writeState->compressionType = columnar_compression;
	writeState->compressionLevel = columnar_compression_level;
	writeState->storageId = ColumnarStorageId(rel);

	/*
	 * Resolve, once per relation, the comparison proc for each column's type
	 * so the writer can maintain a per-chunk min/max skip list (spec 7.2). A
	 * type is "orderable" for our purpose when it has a default btree
	 * comparison proc in the type cache.
	 */
	writeState->colDefs = palloc0(sizeof(ColumnarColumnDef) * writeState->natts);
	{
		int			c;

		for (c = 0; c < writeState->natts; c++)
		{
			Form_pg_attribute att = TupleDescAttr(writeState->tupdesc, c);
			TypeCacheEntry *tce;

			if (att->attisdropped)
				continue;

			tce = lookup_type_cache(att->atttypid, TYPECACHE_CMP_PROC_FINFO);
			if (OidIsValid(tce->cmp_proc_finfo.fn_oid))
			{
				writeState->colDefs[c].orderable = true;
				fmgr_info_copy(&writeState->colDefs[c].cmpFn,
							   &tce->cmp_proc_finfo, ColumnarWriteContext);
				writeState->colDefs[c].collation = att->attcollation;
			}
		}
	}

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

			/* maintain the per-chunk min/max for orderable types */
			if (writeState->colDefs[c].orderable)
			{
				ColumnarColumnDef *def = &writeState->colDefs[c];
				MemoryContext oldContext =
					MemoryContextSwitchTo(writeState->stripeContext);

				if (!col->hasMinMax)
				{
					col->minValue = datumCopy(values[c], att->attbyval,
											  att->attlen);
					col->maxValue = datumCopy(values[c], att->attbyval,
											  att->attlen);
					col->hasMinMax = true;
				}
				else
				{
					int32		cmpMin = DatumGetInt32(
						FunctionCall2Coll(&def->cmpFn, def->collation,
										  values[c], col->minValue));
					int32		cmpMax = DatumGetInt32(
						FunctionCall2Coll(&def->cmpFn, def->collation,
										  values[c], col->maxValue));

					if (cmpMin < 0)
					{
						if (!att->attbyval)
							pfree(DatumGetPointer(col->minValue));
						col->minValue = datumCopy(values[c], att->attbyval,
												  att->attlen);
					}
					if (cmpMax > 0)
					{
						if (!att->attbyval)
							pfree(DatumGetPointer(col->maxValue));
						col->maxValue = datumCopy(values[c], att->attbyval,
												  att->attlen);
					}
				}

				MemoryContextSwitchTo(oldContext);
			}
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
		Form_pg_attribute att = TupleDescAttr(writeState->tupdesc, c);

		g = 0;
		foreach(lc, writeState->chunkGroups)
		{
			ChunkGroupBuffer *group = (ChunkGroupBuffer *) lfirst(lc);
			ColumnChunkBuffer *col = &group->columns[c];
			ChunkMetadata *m = &chunkMeta[c * numGroups + g];
			char	   *compData;
			uint32		compLen;
			int			usedType;
			int			usedLevel;

			m->attrNum = c + 1;
			m->chunkGroupNum = g;

			/*
			 * Compress the value stream independently (spec 5). The codec may
			 * fall back to "none" when it does not shrink the data. The exists
			 * stream is never compressed.
			 */
			ColumnarCompressValueStream(col->valueStream.data,
										col->valueStream.len,
										writeState->compressionType,
										writeState->compressionLevel,
										&compData, &compLen,
										&usedType, &usedLevel);

			m->valueStreamOffset = data->len;
			if (compLen > 0)
				appendBinaryStringInfo(data, compData, compLen);
			m->valueStreamLength = compLen;

			m->existsStreamOffset = data->len;
			appendBinaryStringInfo(data, col->existsStream.data,
								   col->existsStream.len);
			m->existsStreamLength = col->existsStream.len;

			m->valueCompressionType = usedType;
			m->valueCompressionLevel = usedLevel;
			m->valueDecompressedLength = col->valueStream.len;
			m->valueCount = col->valueCount;

			/* encode the min/max skip list for orderable types (spec 7.2) */
			if (col->hasMinMax)
			{
				StringInfoData minBuf;
				StringInfoData maxBuf;

				initStringInfo(&minBuf);
				initStringInfo(&maxBuf);
				ColumnarEncodeValue(&minBuf, att, col->minValue);
				ColumnarEncodeValue(&maxBuf, att, col->maxValue);

				m->minMaxValid = true;
				m->minEncoded = minBuf.data;
				m->minEncodedLen = minBuf.len;
				m->maxEncoded = maxBuf.data;
				m->maxEncodedLen = maxBuf.len;
			}
			else
			{
				m->minMaxValid = false;
			}

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

/*
 * ColumnarWriteStateDiscardSubXact
 *		Drop buffered (unflushed) writes made in an aborting subtransaction.
 *		Stripes already flushed by that subtransaction are made invisible by
 *		the subtransaction abort itself (their catalog rows), so only the
 *		in-memory buffers need discarding here (spec 9).
 */
void
ColumnarWriteStateDiscardSubXact(SubTransactionId subid)
{
	List	   *kept = NIL;
	ListCell   *lc;
	MemoryContext oldContext;

	if (ColumnarWriteStates == NIL)
		return;

	oldContext = MemoryContextSwitchTo(ColumnarWriteContext);
	foreach(lc, ColumnarWriteStates)
	{
		ColumnarWriteState *writeState = (ColumnarWriteState *) lfirst(lc);

		if (writeState->subid != subid)
			kept = lappend(kept, writeState);
	}
	MemoryContextSwitchTo(oldContext);

	ColumnarWriteStates = kept;
}

/*
 * ColumnarWriteStatePromoteSubXact
 *		On subtransaction commit, reassign its buffered writes to the parent so
 *		they are flushed when the parent (eventually the top transaction)
 *		commits.
 */
void
ColumnarWriteStatePromoteSubXact(SubTransactionId subid, SubTransactionId parent)
{
	ListCell   *lc;

	foreach(lc, ColumnarWriteStates)
	{
		ColumnarWriteState *writeState = (ColumnarWriteState *) lfirst(lc);

		if (writeState->subid == subid)
			writeState->subid = parent;
	}
}
