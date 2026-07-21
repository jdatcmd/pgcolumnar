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

#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "executor/tuptable.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
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

	/*
	 * Bloom-filter support (I7): hashable and non-collatable, so a value's hash
	 * is collation-independent and a probe of the same type is consistent. Only
	 * such columns accumulate hashes for a per-chunk bloom filter.
	 */
	bool		bloomable;
	FmgrInfo	hashFn;
	Oid			hashCollation;	/* collation to hash under (InvalidOid if none) */
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

	/* accumulated 4-byte value hashes for the per-chunk bloom filter (I7) */
	StringInfoData hashBuf;
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

	/*
	 * Reservation for the stripe currently being buffered (spec 2.2, 6). The
	 * stripe id and the first row number are reserved eagerly, when the first
	 * row of a stripe is buffered, so every row has a stable row number (and
	 * item pointer) at insert time for indexing. haveReservation is false
	 * between stripes; the file offset is reserved separately at flush.
	 */
	bool		haveReservation;
	uint64		stripeId;
	uint64		stripeFirstRowNumber;

	/*
	 * Phase 2 (gap 26): additional projections fanned out from this relation's
	 * inserts. projWriters hangs off the base write state so it shares the
	 * (relid, subid) lifecycle -- flush, discard, subxact abort/promote all
	 * follow the base automatically. projInited guards the one-time catalog
	 * lookup that builds the list.
	 */
	bool		projInited;
	List	   *projWriters;	/* list of ColumnarProjWriter * */
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
	 * Per-table options (spec 7.4) override the instance-wide GUC defaults for
	 * this relation's writes. They are read at write-state creation, so a value
	 * set with columnar.alter_columnar_table_set takes effect for subsequent
	 * inserts (spec 9).
	 */
	{
		ColumnarOptions opts;

		if (ColumnarReadOptions(relid, &opts))
		{
			if (opts.stripeRowLimitSet)
				writeState->stripeRowLimit = opts.stripeRowLimit;
			if (opts.chunkGroupRowLimitSet)
				writeState->chunkGroupRowLimit = opts.chunkGroupRowLimit;
			if (opts.compressionSet)
				writeState->compressionType = opts.compressionType;
			if (opts.compressionLevelSet)
				writeState->compressionLevel = opts.compressionLevel;
		}
	}

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

			tce = lookup_type_cache(att->atttypid,
									TYPECACHE_CMP_PROC_FINFO |
									TYPECACHE_HASH_PROC_FINFO);
			if (OidIsValid(tce->cmp_proc_finfo.fn_oid))
			{
				writeState->colDefs[c].orderable = true;
				fmgr_info_copy(&writeState->colDefs[c].cmpFn,
							   &tce->cmp_proc_finfo, ColumnarWriteContext);
				writeState->colDefs[c].collation = att->attcollation;
			}

			/*
			 * Bloom filter for hashable columns whose collation is safe (I7,
			 * gap 25): non-collatable types and deterministic collations, so a
			 * value hashes consistently between this build and an equality
			 * probe. Values are hashed under the column's collation. A
			 * nondeterministic collation is left unbloomed.
			 */
			if (OidIsValid(tce->hash_proc_finfo.fn_oid) &&
				ColumnarCollationIsDeterministic(att->attcollation))
			{
				writeState->colDefs[c].bloomable = true;
				fmgr_info_copy(&writeState->colDefs[c].hashFn,
							   &tce->hash_proc_finfo, ColumnarWriteContext);
				writeState->colDefs[c].hashCollation = att->attcollation;
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
	writeState->haveReservation = false;
	writeState->stripeId = 0;
	writeState->stripeFirstRowNumber = 0;

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
		initStringInfo(&group->columns[c].hashBuf);
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
 *		stripe row limit. Returns the stable 1-based row number assigned to the
 *		row (spec 6), so the caller can set the row's item pointer for indexing.
 */
uint64
ColumnarWriteRow(ColumnarWriteState *writeState, Relation rel,
				 Datum *values, bool *nulls)
{
	ChunkGroupBuffer *group = writeState->currentGroup;
	uint64		rowNumber;
	int			c;

	/*
	 * Reserve this stripe's id and row-number range when its first row is
	 * buffered (spec 2.2, 6). A whole stripe_row_limit worth of row numbers is
	 * reserved up front so the stripe's rows are numbered contiguously from
	 * stripeFirstRowNumber; the writer flushes at stripe_row_limit, so the run
	 * is never overrun. Any unused tail (a stripe flushed early) is a harmless
	 * gap in the row-number space.
	 */
	if (!writeState->haveReservation)
	{
		ColumnarReserveRowNumbers(rel, (uint64) writeState->stripeRowLimit,
								  &writeState->stripeId,
								  &writeState->stripeFirstRowNumber);
		writeState->haveReservation = true;
	}

	rowNumber = writeState->stripeFirstRowNumber + writeState->stripeRowCount;

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

			/* accumulate the value's hash for the per-chunk bloom filter (I7) */
			if (writeState->colDefs[c].bloomable)
			{
				uint32		h = DatumGetUInt32(
					FunctionCall1Coll(&writeState->colDefs[c].hashFn,
									  writeState->colDefs[c].hashCollation,
									  values[c]));

				appendBinaryStringInfo(&col->hashBuf, (char *) &h, sizeof(uint32));
			}

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

	return rowNumber;
}

/*
 * ColumnarBufferedRowByNumber
 *		Reconstruct a single row that is still held in an unflushed write buffer,
 *		addressed by its row number (spec 6). Returns true and fills values/nulls
 *		(by-reference values copied into the current memory context) when the row
 *		is present in a pending stripe buffer for this relation; false otherwise.
 *
 *		This lets an index fetch see rows written earlier in the same statement
 *		but not yet flushed, which is what makes a unique constraint reject two
 *		duplicate rows inserted by a single statement: the btree uniqueness check
 *		fetches the first row's item pointer while both rows are still buffered.
 *		It reads only process-local memory, so it acquires no locks and is safe
 *		to call while the caller holds an index buffer lock.
 */
bool
ColumnarBufferedRowByNumber(Relation rel, uint64 rowNumber,
							Datum *values, bool *nulls)
{
	Oid			relid = RelationGetRelid(rel);
	MemoryContext target = CurrentMemoryContext;
	ListCell   *lc;

	foreach(lc, ColumnarWriteStates)
	{
		ColumnarWriteState *ws = (ColumnarWriteState *) lfirst(lc);
		uint64		offset;
		uint64		accumulated;
		ListCell   *glc;

		if (ws->relid != relid || !ws->haveReservation)
			continue;
		if (rowNumber < ws->stripeFirstRowNumber ||
			rowNumber >= ws->stripeFirstRowNumber + ws->stripeRowCount)
			continue;

		offset = rowNumber - ws->stripeFirstRowNumber;

		accumulated = 0;
		foreach(glc, ws->chunkGroups)
		{
			ChunkGroupBuffer *group = (ChunkGroupBuffer *) lfirst(glc);
			uint64		posInGroup;
			int			c;

			if (offset >= accumulated + group->rowCount)
			{
				accumulated += group->rowCount;
				continue;
			}

			posInGroup = offset - accumulated;

			for (c = 0; c < ws->natts; c++)
			{
				ColumnChunkBuffer *col = &group->columns[c];
				Form_pg_attribute att = TupleDescAttr(ws->tupdesc, c);
				char	   *existsBytes = col->existsStream.data;
				char	   *cursor = col->valueStream.data;
				uint64		i;

				/* walk to posInGroup, skipping earlier present values */
				for (i = 0; i < posInGroup; i++)
				{
					if (existsBytes[i])
						(void) ColumnarDecodeValue(att, &cursor, target);
				}

				if (existsBytes[posInGroup])
				{
					values[c] = ColumnarDecodeValue(att, &cursor, target);
					nulls[c] = false;
				}
				else
				{
					values[c] = (Datum) 0;
					nulls[c] = true;
				}
			}

			return true;
		}
	}

	return false;
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
			char	   *encData;
			uint32		encLen;
			int			encType;
			char	   *compData;
			uint32		compLen;
			int			usedType;
			int			usedLevel;

			m->attrNum = c + 1;
			m->chunkGroupNum = g;

			/*
			 * Apply a lightweight, type-aware encoding to the raw value stream
			 * (I1), then block-compress the encoded form (spec 5). Both steps
			 * fall back to "none" when they do not shrink the data. The exists
			 * stream is neither encoded nor compressed.
			 */
			encType = ColumnarEncodeChunk(col->valueStream.data,
										  col->valueStream.len, att,
										  col->valueCount, &encData, &encLen);

			ColumnarCompressValueStream(encData, encLen,
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
			m->valueDecompressedLength = encLen;
			m->valueCount = col->valueCount;
			m->valueEncodingType = encType;
			m->valueRawLength = col->valueStream.len;

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

			/* build the per-chunk bloom filter from accumulated hashes (I7) */
			if (col->hashBuf.len > 0)
			{
				char	   *bloom;
				uint32		bloomLen;

				if (ColumnarBloomBuild((const uint32 *) col->hashBuf.data,
									   col->hashBuf.len / sizeof(uint32),
									   &bloom, &bloomLen))
				{
					m->bloomFilter = bloom;
					m->bloomLen = bloomLen;
				}
			}

			g++;
		}
	}

	dataLength = data->len;

	/*
	 * The stripe id and first row number were reserved eagerly when the first
	 * row was buffered (spec 6). Reserve only the data byte range now, once its
	 * size is known, under the relation extension lock, and write the pages.
	 */
	stripeId = writeState->stripeId;
	firstRowNumber = writeState->stripeFirstRowNumber;

	rel = table_open(writeState->relid, RowExclusiveLock);

	LockRelationForExtension(rel, ExclusiveLock);
	ColumnarReserveOffset(rel, dataLength, &fileOffset);
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

	/* reset stripe accumulation; the next row reserves a fresh stripe */
	MemoryContextReset(writeState->stripeContext);
	writeState->chunkGroups = NIL;
	writeState->currentGroup = NULL;
	writeState->stripeRowCount = 0;
	writeState->haveReservation = false;

	if (pushedSnapshot)
		PopActiveSnapshot();
}

/* -------------------------------------------------------------------------
 * projection write fan-out (gap 26, phase 2)
 *
 * Each additional projection of a table has its own storage id but shares the
 * table's relation file and row-number space. On insert, the projected columns
 * plus the base row number are buffered; at flush the batch is sorted on the
 * projection's sort key and written as a stripe to the projection's storage,
 * reusing the base stripe encoder (ColumnarWriteRow + columnar_flush_stripe).
 * The base row number is stored as a leading int8 column so the projection can
 * be joined back to the base; deletes/visibility come from the base row_mask, so
 * only INSERT fans out (see design/gaps/26-IMPL-projections-phase2-plan.md).
 * ------------------------------------------------------------------------- */

/* one buffered projection row: [rownumber, projcol1..projcolK] */
typedef struct ProjRow
{
	Datum	   *values;
	bool	   *nulls;
} ProjRow;

typedef struct ColumnarProjWriter
{
	uint64		projStorageId;
	int			ncols;			/* number of projection columns (K) */
	AttrNumber *colAttnums;		/* table attnums of the K columns (1-based) */
	TupleDesc	projTupdesc;	/* [rownumber int8, projcol1..projcolK] */

	int			nsort;
	int		   *sortBufIdx;		/* index into a row's values[] for each sort col */
	FmgrInfo   *sortCmp;		/* btree cmp proc per sort col */
	Oid		   *sortColl;		/* collation per sort col */

	int			stripeRowLimit;
	int			chunkGroupRowLimit;
	int			compType;
	int			compLevel;

	ProjRow    *rows;			/* buffered rows (capacity stripeRowLimit) */
	int			nrows;
	MemoryContext ctx;			/* persists: struct arrays, projTupdesc */
	MemoryContext rowCtx;		/* reset after each stripe flush: row datums */
	ColumnarWriteState *innerWs;	/* reused stripe encoder for this projection */
} ColumnarProjWriter;

/*
 * columnar_build_write_state
 *		Allocate a standalone stripe encoder for the given tuple descriptor and
 *		storage id, not registered in ColumnarWriteStates. Used for a
 *		projection's inner writer; colDefs are zeroed (no min/max or bloom in
 *		phase 2 -- projection storage is sorted but carries no skip metadata yet).
 */
static ColumnarWriteState *
columnar_build_write_state(Oid relid, TupleDesc srcTupdesc, uint64 storageId,
						   int stripeRowLimit, int chunkGroupRowLimit,
						   int compType, int compLevel)
{
	MemoryContext oldContext;
	ColumnarWriteState *ws;

	if (ColumnarWriteContext == NULL)
		ColumnarWriteContext = AllocSetContextCreate(TopTransactionContext,
													 "columnar write",
													 ALLOCSET_DEFAULT_SIZES);
	oldContext = MemoryContextSwitchTo(ColumnarWriteContext);

	ws = palloc0(sizeof(ColumnarWriteState));
	ws->relid = relid;
	ws->subid = GetCurrentSubTransactionId();
	ws->tupdesc = CreateTupleDescCopy(srcTupdesc);
	ws->natts = ws->tupdesc->natts;
	ws->stripeRowLimit = stripeRowLimit;
	ws->chunkGroupRowLimit = chunkGroupRowLimit;
	ws->compressionType = compType;
	ws->compressionLevel = compLevel;
	ws->storageId = storageId;
	ws->colDefs = palloc0(sizeof(ColumnarColumnDef) * ws->natts);
	ws->stripeContext = AllocSetContextCreate(ColumnarWriteContext,
											  "columnar proj stripe",
											  ALLOCSET_DEFAULT_SIZES);
	ws->writeContext = ColumnarWriteContext;
	ws->chunkGroups = NIL;
	ws->currentGroup = NULL;
	ws->stripeRowCount = 0;
	ws->haveReservation = false;

	MemoryContextSwitchTo(oldContext);
	return ws;
}

/* qsort_arg comparator: ascending, NULLS LAST, over the projection sort key */
static int
proj_row_cmp(const void *a, const void *b, void *arg)
{
	const ProjRow *ra = (const ProjRow *) a;
	const ProjRow *rb = (const ProjRow *) b;
	ColumnarProjWriter *w = (ColumnarProjWriter *) arg;
	int			i;

	for (i = 0; i < w->nsort; i++)
	{
		int			idx = w->sortBufIdx[i];
		bool		na = ra->nulls[idx];
		bool		nb = rb->nulls[idx];
		int32		c;

		if (na && nb)
			continue;
		if (na)
			return 1;			/* nulls last */
		if (nb)
			return -1;
		c = DatumGetInt32(FunctionCall2Coll(&w->sortCmp[i], w->sortColl[i],
											ra->values[idx], rb->values[idx]));
		if (c != 0)
			return c;
	}
	return 0;
}

/*
 * flush_proj_writer
 *		Sort the buffered rows on the projection's sort key and write them as one
 *		stripe to the projection's storage, then reset the buffer.
 */
static void
flush_proj_writer(ColumnarProjWriter *w, Relation tableRel)
{
	int			i;

	if (w->nrows == 0)
		return;

	if (w->nsort > 0)
		qsort_arg(w->rows, w->nrows, sizeof(ProjRow), proj_row_cmp, w);

	if (w->innerWs == NULL)
		w->innerWs = columnar_build_write_state(RelationGetRelid(tableRel),
												w->projTupdesc, w->projStorageId,
												w->stripeRowLimit,
												w->chunkGroupRowLimit,
												w->compType, w->compLevel);

	for (i = 0; i < w->nrows; i++)
		ColumnarWriteRow(w->innerWs, tableRel, w->rows[i].values, w->rows[i].nulls);

	if (w->innerWs->stripeRowCount > 0)
		columnar_flush_stripe(w->innerWs);

	MemoryContextReset(w->rowCtx);
	w->nrows = 0;
}

/*
 * build_proj_writer
 *		Construct a ColumnarProjWriter for one projection catalog row.
 */
static ColumnarProjWriter *
build_proj_writer(Relation rel, const ColumnarProjection *proj,
				  int stripeRowLimit, int chunkGroupRowLimit,
				  int compType, int compLevel)
{
	TupleDesc	tableDesc = RelationGetDescr(rel);
	MemoryContext ctx;
	MemoryContext oldContext;
	ColumnarProjWriter *w;
	int			i;

	ctx = AllocSetContextCreate(ColumnarWriteContext, "columnar proj writer",
								ALLOCSET_DEFAULT_SIZES);
	oldContext = MemoryContextSwitchTo(ctx);

	w = palloc0(sizeof(ColumnarProjWriter));
	w->projStorageId = proj->projStorageId;
	w->ncols = proj->columnsLen;
	w->stripeRowLimit = stripeRowLimit;
	w->chunkGroupRowLimit = chunkGroupRowLimit;
	w->compType = compType;
	w->compLevel = compLevel;
	w->ctx = ctx;
	w->rowCtx = AllocSetContextCreate(ctx, "columnar proj rows",
									  ALLOCSET_DEFAULT_SIZES);
	w->rows = palloc0(sizeof(ProjRow) * stripeRowLimit);
	w->nrows = 0;
	w->innerWs = NULL;

	w->colAttnums = palloc(sizeof(AttrNumber) * w->ncols);
	for (i = 0; i < w->ncols; i++)
		w->colAttnums[i] = (AttrNumber) proj->columns[i];

	/* synthetic tuple descriptor: rownumber int8, then the projection columns */
	w->projTupdesc = CreateTemplateTupleDesc(w->ncols + 1);
	TupleDescInitEntry(w->projTupdesc, 1, "rownumber", INT8OID, -1, 0);
	for (i = 0; i < w->ncols; i++)
		TupleDescCopyEntry(w->projTupdesc, i + 2, tableDesc, w->colAttnums[i]);

	/* sort-key comparators; each sort attnum is one of the projection columns */
	w->nsort = proj->sortKeyLen;
	if (w->nsort > 0)
	{
		w->sortBufIdx = palloc(sizeof(int) * w->nsort);
		w->sortCmp = palloc(sizeof(FmgrInfo) * w->nsort);
		w->sortColl = palloc(sizeof(Oid) * w->nsort);
		for (i = 0; i < w->nsort; i++)
		{
			int16		attno = proj->sortKey[i];
			Form_pg_attribute att = TupleDescAttr(tableDesc, attno - 1);
			TypeCacheEntry *tce;
			int			p;

			/* position of this sort column within the projection's columns */
			w->sortBufIdx[i] = -1;
			for (p = 0; p < w->ncols; p++)
				if (w->colAttnums[p] == attno)
				{
					w->sortBufIdx[i] = p + 1;	/* +1 for the leading rownumber */
					break;
				}
			if (w->sortBufIdx[i] < 0)
				elog(ERROR, "columnar: sort column not in projection columns");

			tce = lookup_type_cache(att->atttypid, TYPECACHE_CMP_PROC_FINFO);
			if (!OidIsValid(tce->cmp_proc_finfo.fn_oid))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot sort projection on column of type %s",
								format_type_be(att->atttypid))));
			fmgr_info_copy(&w->sortCmp[i], &tce->cmp_proc_finfo, ctx);
			w->sortColl[i] = att->attcollation;
		}
	}

	MemoryContextSwitchTo(oldContext);
	return w;
}

/*
 * ColumnarProjectionFanoutRow
 *		Buffer a freshly inserted row into each additional projection of the
 *		relation. rowNumber is the base row number returned by ColumnarWriteRow.
 *		The projection writers hang off the base write state, so they share its
 *		(relid, subid) lifecycle.
 */
void
ColumnarProjectionFanoutRow(Relation rel, ColumnarWriteState *baseWs,
							uint64 rowNumber, Datum *values, bool *nulls)
{
	TupleDesc	tableDesc = RelationGetDescr(rel);
	ListCell   *lc;

	if (!baseWs->projInited)
	{
		List	   *projs = ColumnarListProjections(baseWs->storageId);
		MemoryContext oldContext = MemoryContextSwitchTo(ColumnarWriteContext);
		ListCell   *pc;

		foreach(pc, projs)
		{
			ColumnarProjection *p = (ColumnarProjection *) lfirst(pc);

			if (p->projectionId == 0)
				continue;		/* base projection is the table itself */
			baseWs->projWriters =
				lappend(baseWs->projWriters,
						build_proj_writer(rel, p, baseWs->stripeRowLimit,
										  baseWs->chunkGroupRowLimit,
										  baseWs->compressionType,
										  baseWs->compressionLevel));
		}
		baseWs->projInited = true;
		MemoryContextSwitchTo(oldContext);
	}

	if (baseWs->projWriters == NIL)
		return;

	foreach(lc, baseWs->projWriters)
	{
		ColumnarProjWriter *w = (ColumnarProjWriter *) lfirst(lc);
		MemoryContext oldContext = MemoryContextSwitchTo(w->rowCtx);
		ProjRow    *r = &w->rows[w->nrows];
		int			i;

		r->values = palloc(sizeof(Datum) * (w->ncols + 1));
		r->nulls = palloc(sizeof(bool) * (w->ncols + 1));
		r->values[0] = Int64GetDatum((int64) rowNumber);
		r->nulls[0] = false;
		for (i = 0; i < w->ncols; i++)
		{
			AttrNumber	a = w->colAttnums[i];
			Form_pg_attribute att = TupleDescAttr(tableDesc, a - 1);

			if (nulls[a - 1])
			{
				r->nulls[i + 1] = true;
				r->values[i + 1] = (Datum) 0;
			}
			else
			{
				r->nulls[i + 1] = false;
				r->values[i + 1] = datumCopy(values[a - 1], att->attbyval,
											 att->attlen);
			}
		}
		w->nrows++;
		MemoryContextSwitchTo(oldContext);

		if (w->nrows >= w->stripeRowLimit)
			flush_proj_writer(w, rel);
	}
}

/* Flush all projection writers hanging off a base write state. */
static void
flush_ws_projections(ColumnarWriteState *ws)
{
	ListCell   *lc;
	Relation	rel;
	bool		any = false;

	foreach(lc, ws->projWriters)
		if (((ColumnarProjWriter *) lfirst(lc))->nrows > 0)
			any = true;
	if (!any)
		return;

	rel = table_open(ws->relid, RowExclusiveLock);
	foreach(lc, ws->projWriters)
		flush_proj_writer((ColumnarProjWriter *) lfirst(lc), rel);
	table_close(rel, RowExclusiveLock);
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

		if (writeState->relid != relid)
			continue;
		if (writeState->stripeRowCount > 0)
			columnar_flush_stripe(writeState);
		flush_ws_projections(writeState);
	}
}

/*
 * ColumnarForgetWriteStateForRelation
 *		Drop the cached write state for a relation without flushing it. Used
 *		after the relation's storage is swapped (columnar.vacuum): the cached
 *		state holds the old storage id, so it must be discarded and a fresh one
 *		created for the new storage. The caller must have flushed first if any
 *		buffered rows still needed persisting.
 */
void
ColumnarForgetWriteStateForRelation(Oid relid)
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

		if (writeState->relid != relid)
			kept = lappend(kept, writeState);
	}
	MemoryContextSwitchTo(oldContext);

	ColumnarWriteStates = kept;
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
	{
		ColumnarWriteState *writeState = (ColumnarWriteState *) lfirst(lc);

		columnar_flush_stripe(writeState);
		flush_ws_projections(writeState);
	}
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
