/*-------------------------------------------------------------------------
 *
 * columnar_reader.c
 *		The columnar reader: a sequential scan that reads all columns of all
 *		stripes and reconstructs rows (spec 4, 6). Also holds the value-stream
 *		codec shared with the writer.
 *
 * Phase 1 stores value streams uncompressed. Each chunk carries an exists
 * (null bitmap) stream of one byte per row; present rows draw their value
 * from the value stream in order.
 *
 *-------------------------------------------------------------------------
 */
#include "columnar.h"

#include "fmgr.h"
#include "access/detoast.h"
#include "access/relscan.h"
#include "access/tupmacs.h"
#include "port/atomics.h"
#include "utils/memutils.h"
#include "utils/rel.h"

struct ColumnarReadState
{
	Relation	rel;
	Snapshot	snapshot;
	TupleDesc	tupdesc;
	int			natts;
	uint64		storageId;

	List	   *stripeList;			/* list of StripeMetadata* */
	int			stripeIndex;		/* next stripe to load */
	bool		started;
	bool		exhausted;

	ParallelTableScanDesc parallelScan;

	/* current stripe decode state (in stripeContext) */
	StripeMetadata *stripe;
	char	   *stripeBuffer;
	List	   *chunkGroupList;		/* ChunkGroupMetadata* */
	ChunkMetadata ***chunkMap;		/* [natts][chunkGroupCount] */
	int			chunkGroupCount;
	int			groupIndex;
	uint64		groupRowCount;
	uint64		rowInGroup;
	uint64		rowOffsetInStripe;	/* rows before the current group */
	char	  **valueCursor;		/* [natts] */
	char	  **existsBase;			/* [natts] */

	MemoryContext readContext;		/* whole scan */
	MemoryContext stripeContext;	/* reset per stripe */
	MemoryContext rowContext;		/* reset per row */
};

static void columnar_load_stripe(ColumnarReadState *readState,
								 StripeMetadata *stripe);
static void columnar_setup_group(ColumnarReadState *readState, int groupIndex);

/* -------------------------------------------------------------------------
 * value stream codec (shared with the writer)
 * ------------------------------------------------------------------------- */

/*
 * ColumnarEncodeValue
 *		Append a non-null value to a column's value stream. Fixed-length
 *		values are stored as their raw bytes; varlena values are detoasted
 *		and stored with a full 4-byte header so the reader can size them.
 */
void
ColumnarEncodeValue(StringInfo buf, Form_pg_attribute att, Datum value)
{
	if (att->attbyval)
	{
		char		tmp[8];

		Assert(att->attlen >= 1 && att->attlen <= 8);
		store_att_byval(tmp, value, att->attlen);
		appendBinaryStringInfo(buf, tmp, att->attlen);
	}
	else if (att->attlen > 0)
	{
		appendBinaryStringInfo(buf, DatumGetPointer(value), att->attlen);
	}
	else if (att->attlen == -1)
	{
		struct varlena *detoasted =
			pg_detoast_datum((struct varlena *) DatumGetPointer(value));

		appendBinaryStringInfo(buf, (char *) detoasted, VARSIZE(detoasted));
		if ((Pointer) detoasted != DatumGetPointer(value))
			pfree(detoasted);
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("columnar phase 1 does not support column type with attlen %d",
						att->attlen)));
	}
}

/*
 * ColumnarDecodeValue
 *		Read one value from a column's value stream, advancing *cursor.
 *		By-reference values are copied into targetContext so they outlive the
 *		stripe buffer's next reset.
 */
Datum
ColumnarDecodeValue(Form_pg_attribute att, char **cursor,
					MemoryContext targetContext)
{
	char	   *p = *cursor;
	Datum		result;

	if (att->attbyval)
	{
		result = fetch_att(p, true, att->attlen);
		*cursor = p + att->attlen;
	}
	else if (att->attlen > 0)
	{
		char	   *copy = MemoryContextAlloc(targetContext, att->attlen);

		memcpy(copy, p, att->attlen);
		result = PointerGetDatum(copy);
		*cursor = p + att->attlen;
	}
	else
	{
		Size		len = VARSIZE_ANY(p);
		char	   *copy = MemoryContextAlloc(targetContext, len);

		memcpy(copy, p, len);
		result = PointerGetDatum(copy);
		*cursor = p + len;
	}

	return result;
}

/* -------------------------------------------------------------------------
 * sequential scan
 * ------------------------------------------------------------------------- */

ColumnarReadState *
ColumnarBeginRead(Relation rel, Snapshot snapshot,
				  ParallelTableScanDesc parallelScan)
{
	ColumnarReadState *readState;
	MemoryContext readContext;
	MemoryContext oldContext;

	readContext = AllocSetContextCreate(CurrentMemoryContext,
										"columnar read",
										ALLOCSET_DEFAULT_SIZES);
	oldContext = MemoryContextSwitchTo(readContext);

	readState = palloc0(sizeof(ColumnarReadState));
	readState->rel = rel;
	readState->snapshot = snapshot;
	readState->tupdesc = RelationGetDescr(rel);
	readState->natts = readState->tupdesc->natts;
	readState->storageId = ColumnarStorageId(rel);
	readState->stripeList = NIL;
	readState->stripeIndex = 0;
	readState->started = false;
	readState->exhausted = false;
	readState->parallelScan = parallelScan;
	readState->stripe = NULL;
	readState->readContext = readContext;
	readState->stripeContext = AllocSetContextCreate(readContext,
													 "columnar read stripe",
													 ALLOCSET_DEFAULT_SIZES);
	readState->rowContext = AllocSetContextCreate(readContext,
												  "columnar read row",
												  ALLOCSET_DEFAULT_SIZES);

	MemoryContextSwitchTo(oldContext);
	return readState;
}

/*
 * columnar_setup_group
 *		Point each column's cursor at the start of its value and exists
 *		streams for a chunk group.
 */
static void
columnar_setup_group(ColumnarReadState *readState, int groupIndex)
{
	ChunkGroupMetadata *cg = list_nth(readState->chunkGroupList, groupIndex);
	int			c;

	readState->groupRowCount = cg->rowCount;
	readState->rowInGroup = 0;

	for (c = 0; c < readState->natts; c++)
	{
		ChunkMetadata *m = readState->chunkMap[c][groupIndex];

		if (m == NULL)
			elog(ERROR,
				 "columnar: missing chunk for attr %d, chunk group %d",
				 c + 1, groupIndex);

		readState->valueCursor[c] = readState->stripeBuffer + m->valueStreamOffset;
		readState->existsBase[c] = readState->stripeBuffer + m->existsStreamOffset;
	}
}

/*
 * columnar_load_stripe
 *		Read a stripe's metadata and data into memory and position at its
 *		first chunk group.
 */
static void
columnar_load_stripe(ColumnarReadState *readState, StripeMetadata *stripe)
{
	MemoryContext oldContext;
	List	   *chunkList;
	ListCell   *lc;
	int			natts = readState->natts;
	int			c;

	MemoryContextReset(readState->stripeContext);
	oldContext = MemoryContextSwitchTo(readState->stripeContext);

	readState->stripe = stripe;
	readState->chunkGroupCount = stripe->chunkGroupCount;
	readState->groupIndex = 0;
	readState->rowOffsetInStripe = 0;

	readState->chunkGroupList = ColumnarReadChunkGroupList(readState->storageId,
														   stripe->stripeNum,
														   readState->snapshot);
	chunkList = ColumnarReadChunkList(readState->storageId, stripe->stripeNum,
									  readState->snapshot);

	/* index chunks by (attr, chunk group) */
	readState->chunkMap = palloc0(sizeof(ChunkMetadata **) * natts);
	for (c = 0; c < natts; c++)
		readState->chunkMap[c] = palloc0(sizeof(ChunkMetadata *) *
										 readState->chunkGroupCount);

	foreach(lc, chunkList)
	{
		ChunkMetadata *m = (ChunkMetadata *) lfirst(lc);

		if (m->attrNum - 1 < natts && m->chunkGroupNum < readState->chunkGroupCount)
			readState->chunkMap[m->attrNum - 1][m->chunkGroupNum] = m;
	}

	readState->valueCursor = palloc0(sizeof(char *) * natts);
	readState->existsBase = palloc0(sizeof(char *) * natts);

	/* read the stripe's data area into a contiguous buffer */
	readState->stripeBuffer = palloc(stripe->dataLength);
	ColumnarReadLogicalData(readState->rel, stripe->fileOffset,
							readState->stripeBuffer, stripe->dataLength);

	if (readState->chunkGroupCount > 0)
		columnar_setup_group(readState, 0);

	MemoryContextSwitchTo(oldContext);
}

bool
ColumnarReadNextRow(ColumnarReadState *readState, Datum *values, bool *nulls,
					uint64 *rowNumber)
{
	if (!readState->started)
	{
		readState->started = true;

		/*
		 * For a parallel scan, let a single worker return all rows. This is
		 * a correct (if not parallel-accelerated) phase 1 behaviour: other
		 * workers see the scan as already claimed and return nothing.
		 */
		if (readState->parallelScan != NULL)
		{
			ParallelBlockTableScanDesc bpscan =
				(ParallelBlockTableScanDesc) readState->parallelScan;
			uint64		claim = pg_atomic_fetch_add_u64(&bpscan->phs_nallocated, 1);

			if (claim != 0)
				readState->exhausted = true;
		}

		if (!readState->exhausted)
		{
			MemoryContext oldContext = MemoryContextSwitchTo(readState->readContext);

			readState->stripeList = ColumnarReadStripeList(readState->storageId,
														   readState->snapshot);
			readState->stripeIndex = 0;
			MemoryContextSwitchTo(oldContext);
		}
	}

	for (;;)
	{
		if (readState->exhausted)
			return false;

		if (readState->stripe == NULL)
		{
			if (readState->stripeIndex >= list_length(readState->stripeList))
			{
				readState->exhausted = true;
				return false;
			}

			columnar_load_stripe(readState,
								 list_nth(readState->stripeList,
										  readState->stripeIndex));
			readState->stripeIndex++;

			if (readState->chunkGroupCount == 0)
			{
				readState->stripe = NULL;
				continue;
			}
		}

		if (readState->rowInGroup >= readState->groupRowCount)
		{
			readState->rowOffsetInStripe += readState->groupRowCount;
			readState->groupIndex++;

			if (readState->groupIndex >= readState->chunkGroupCount)
			{
				readState->stripe = NULL;
				continue;
			}

			columnar_setup_group(readState, readState->groupIndex);
			continue;
		}

		/* produce the current row */
		MemoryContextReset(readState->rowContext);

		{
			int			c;

			for (c = 0; c < readState->natts; c++)
			{
				char		exists = readState->existsBase[c][readState->rowInGroup];

				if (exists)
				{
					Form_pg_attribute att = TupleDescAttr(readState->tupdesc, c);

					values[c] = ColumnarDecodeValue(att, &readState->valueCursor[c],
													readState->rowContext);
					nulls[c] = false;
				}
				else
				{
					values[c] = (Datum) 0;
					nulls[c] = true;
				}
			}
		}

		*rowNumber = readState->stripe->firstRowNumber +
			readState->rowOffsetInStripe + readState->rowInGroup;
		readState->rowInGroup++;

		return true;
	}
}

void
ColumnarRescanRead(ColumnarReadState *readState)
{
	MemoryContextReset(readState->stripeContext);
	readState->stripe = NULL;
	readState->stripeIndex = 0;
	readState->started = false;
	readState->exhausted = false;
	readState->stripeList = NIL;
}

void
ColumnarEndRead(ColumnarReadState *readState)
{
	MemoryContextDelete(readState->readContext);
}
