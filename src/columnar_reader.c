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
#include "access/htup_details.h"
#include "access/relscan.h"
#include "access/tupmacs.h"
#include "port/atomics.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/typcache.h"

/*
 * One pushed-down comparison predicate used for chunk-group skipping. Built
 * from a scan key of the form "column btree-op constant" (spec 9). The
 * comparison proc is the column type's default btree comparison, matching the
 * proc used to build the stored min/max, so skipping is conservative and
 * correct: a group is skipped only when its min/max prove no row can match.
 */
typedef struct SkipPredicate
{
	int			attidx;			/* 0-based column index */
	StrategyNumber strategy;	/* BTLess/LessEqual/Equal/GreaterEqual/Greater */
	Datum		compareValue;	/* the constant */
	FmgrInfo	cmpFn;			/* column type's default btree comparison */
	Oid			collation;
} SkipPredicate;

struct ColumnarReadState
{
	Relation	rel;
	Snapshot	snapshot;
	Snapshot	metaSnapshot;	/* catalog reads: sees our own writes (spec 9) */
	TupleDesc	tupdesc;
	int			natts;
	uint64		storageId;

	/*
	 * Per-column "missing" value for columns added by ALTER TABLE ADD COLUMN
	 * after a stripe was written (spec 6). Such a stripe has no chunk for the
	 * new column; the reader then produces the attribute's missing value
	 * (attmissingval when the add carried a constant default, otherwise NULL),
	 * matching the semantics heap gives via its fast-default mechanism.
	 */
	Datum	   *missingValues;		/* [natts] */
	bool	   *missingIsnull;		/* [natts] */

	Bitmapset  *projectedColumns;	/* 0-based; NULL means all columns */
	SkipPredicate *predicates;		/* [numPredicates], in readContext */
	int			numPredicates;

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
	char	  **valueCursor;		/* [natts]; decompressed value stream */
	char	  **existsBase;			/* [natts]; into the raw stripe buffer */

	/* row mask (spec 7.5): per chunk group, the delete bitmap or NULL */
	char	  **groupMask;			/* [chunkGroupCount] */
	uint32	   *groupMaskLen;		/* [chunkGroupCount] */
	char	   *currentMask;		/* mask of the current group, or NULL */
	uint32		currentMaskLen;

	/* chunk-group skip counters over the groups reached so far (spec 9) */
	uint64		groupsRead;
	uint64		groupsSkipped;

	MemoryContext readContext;		/* whole scan */
	MemoryContext stripeContext;	/* reset per stripe */
	MemoryContext groupContext;		/* reset per chunk group (decompressed) */
	MemoryContext rowContext;		/* reset per row */
	MemoryContext skipContext;		/* scratch for skip-list evaluation */
};

static void columnar_load_stripe(ColumnarReadState *readState,
								 StripeMetadata *stripe);
static void columnar_setup_group(ColumnarReadState *readState, int groupIndex);
static bool columnar_position_group(ColumnarReadState *readState);
static bool columnar_group_can_match(ColumnarReadState *readState, int groupIndex);
static bool columnar_is_projected(ColumnarReadState *readState, int attidx);
static void columnar_build_predicates(ColumnarReadState *readState,
									  int nkeys, ScanKey keys);

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
				  ParallelTableScanDesc parallelScan,
				  Bitmapset *projectedColumns, int nkeys, ScanKey keys)
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
	readState->metaSnapshot = ColumnarCatalogSnapshot(snapshot);
	readState->tupdesc = RelationGetDescr(rel);
	readState->natts = readState->tupdesc->natts;
	readState->storageId = ColumnarStorageId(rel);

	/*
	 * Resolve each column's missing value once, for stripes that predate an
	 * ADD COLUMN and therefore carry no chunk for the column (spec 6). A table
	 * with no added-with-default columns yields all-NULL here.
	 */
	readState->missingValues = palloc0(sizeof(Datum) * readState->natts);
	readState->missingIsnull = palloc0(sizeof(bool) * readState->natts);
	{
		int			mc;

		for (mc = 0; mc < readState->natts; mc++)
			readState->missingValues[mc] =
				getmissingattr(readState->tupdesc, mc + 1,
							   &readState->missingIsnull[mc]);
	}

	readState->projectedColumns = bms_copy(projectedColumns);
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
	readState->groupContext = AllocSetContextCreate(readContext,
													"columnar read group",
													ALLOCSET_DEFAULT_SIZES);
	readState->rowContext = AllocSetContextCreate(readContext,
												  "columnar read row",
												  ALLOCSET_DEFAULT_SIZES);
	readState->skipContext = AllocSetContextCreate(readContext,
												   "columnar read skip",
												   ALLOCSET_DEFAULT_SIZES);

	if (columnar_enable_qual_pushdown)
		columnar_build_predicates(readState, nkeys, keys);

	MemoryContextSwitchTo(oldContext);
	return readState;
}

/*
 * columnar_is_projected
 *		Whether a 0-based column should be decoded. A NULL projection set means
 *		all columns are projected (the plain sequential-scan default).
 */
static bool
columnar_is_projected(ColumnarReadState *readState, int attidx)
{
	return readState->projectedColumns == NULL ||
		bms_is_member(attidx, readState->projectedColumns);
}

/*
 * columnar_build_predicates
 *		Translate the scan's ScanKeys into skip predicates for chunk-group
 *		filtering (spec 9). Only simple, same-type btree comparison keys on an
 *		orderable column are used; anything else is ignored, so skipping stays
 *		conservative. Runs in readContext.
 */
static void
columnar_build_predicates(ColumnarReadState *readState, int nkeys, ScanKey keys)
{
	int			i;
	int			n = 0;

	if (nkeys <= 0 || keys == NULL)
		return;

	readState->predicates = palloc0(sizeof(SkipPredicate) * nkeys);

	for (i = 0; i < nkeys; i++)
	{
		ScanKey		key = &keys[i];
		int			attidx;
		Form_pg_attribute att;
		TypeCacheEntry *tce;

		/* only plain "column op const" comparison keys are usable */
		if (key->sk_flags & (SK_ISNULL | SK_ROW_HEADER | SK_ROW_MEMBER |
							 SK_ROW_END | SK_SEARCHNULL | SK_SEARCHNOTNULL |
							 SK_ORDER_BY))
			continue;
		if (key->sk_attno < 1 || key->sk_attno > readState->natts)
			continue;
		if (key->sk_strategy < BTLessStrategyNumber ||
			key->sk_strategy > BTGreaterStrategyNumber)
			continue;

		attidx = key->sk_attno - 1;
		att = TupleDescAttr(readState->tupdesc, attidx);

		/* avoid cross-type comparisons that our column cmp proc cannot do */
		if (OidIsValid(key->sk_subtype) && key->sk_subtype != att->atttypid)
			continue;

		tce = lookup_type_cache(att->atttypid, TYPECACHE_CMP_PROC_FINFO);
		if (!OidIsValid(tce->cmp_proc_finfo.fn_oid))
			continue;

		readState->predicates[n].attidx = attidx;
		readState->predicates[n].strategy = key->sk_strategy;
		readState->predicates[n].compareValue = key->sk_argument;
		fmgr_info_copy(&readState->predicates[n].cmpFn, &tce->cmp_proc_finfo,
					   readState->readContext);
		readState->predicates[n].collation = att->attcollation;
		n++;
	}

	readState->numPredicates = n;
}

/*
 * columnar_group_can_match
 *		Decide whether a chunk group could contain a row satisfying every
 *		pushed-down predicate, using the stored per-chunk min/max (spec 9). A
 *		return of false means the group can be skipped. Missing min/max, or a
 *		non-orderable column, is treated conservatively as "may match".
 */
static bool
columnar_group_can_match(ColumnarReadState *readState, int groupIndex)
{
	int			p;

	if (readState->numPredicates == 0)
		return true;

	MemoryContextReset(readState->skipContext);

	for (p = 0; p < readState->numPredicates; p++)
	{
		SkipPredicate *pred = &readState->predicates[p];
		ChunkMetadata *m = readState->chunkMap[pred->attidx][groupIndex];
		Form_pg_attribute att = TupleDescAttr(readState->tupdesc, pred->attidx);
		char	   *cur;
		Datum		minv;
		Datum		maxv;
		int32		c1;
		int32		c2;

		if (m == NULL || !m->minMaxValid)
			continue;			/* cannot prove empty; keep the group */

		cur = m->minEncoded;
		minv = ColumnarDecodeValue(att, &cur, readState->skipContext);
		cur = m->maxEncoded;
		maxv = ColumnarDecodeValue(att, &cur, readState->skipContext);

		switch (pred->strategy)
		{
			case BTLessStrategyNumber:	/* col < const : skip if min >= const */
				c1 = DatumGetInt32(FunctionCall2Coll(&pred->cmpFn,
													 pred->collation,
													 minv, pred->compareValue));
				if (c1 >= 0)
					return false;
				break;
			case BTLessEqualStrategyNumber: /* col <= const : skip if min > const */
				c1 = DatumGetInt32(FunctionCall2Coll(&pred->cmpFn,
													 pred->collation,
													 minv, pred->compareValue));
				if (c1 > 0)
					return false;
				break;
			case BTEqualStrategyNumber: /* col = const : skip if const<min or >max */
				c1 = DatumGetInt32(FunctionCall2Coll(&pred->cmpFn,
													 pred->collation,
													 pred->compareValue, minv));
				c2 = DatumGetInt32(FunctionCall2Coll(&pred->cmpFn,
													 pred->collation,
													 pred->compareValue, maxv));
				if (c1 < 0 || c2 > 0)
					return false;
				break;
			case BTGreaterEqualStrategyNumber:	/* col >= const : skip if max<const */
				c2 = DatumGetInt32(FunctionCall2Coll(&pred->cmpFn,
													 pred->collation,
													 maxv, pred->compareValue));
				if (c2 < 0)
					return false;
				break;
			case BTGreaterStrategyNumber:	/* col > const : skip if max<=const */
				c2 = DatumGetInt32(FunctionCall2Coll(&pred->cmpFn,
													 pred->collation,
													 maxv, pred->compareValue));
				if (c2 <= 0)
					return false;
				break;
			default:
				break;
		}
	}

	return true;
}

/*
 * columnar_setup_group
 *		Position on a chunk group: decompress each projected column's value
 *		stream into the group context and point the column cursors at the
 *		decompressed bytes and the (uncompressed) exists bytes. Non-projected
 *		columns are left un-decoded (column projection, spec 9).
 */
static void
columnar_setup_group(ColumnarReadState *readState, int groupIndex)
{
	ChunkGroupMetadata *cg = list_nth(readState->chunkGroupList, groupIndex);
	int			c;

	readState->groupRowCount = cg->rowCount;
	readState->rowInGroup = 0;

	/* the delete bitmap for this chunk group, if any (spec 7.5) */
	readState->currentMask = readState->groupMask[groupIndex];
	readState->currentMaskLen = readState->groupMaskLen[groupIndex];

	MemoryContextReset(readState->groupContext);

	for (c = 0; c < readState->natts; c++)
	{
		ChunkMetadata *m = readState->chunkMap[c][groupIndex];

		/*
		 * A NULL chunk means this column did not exist when the stripe was
		 * written (ALTER TABLE ADD COLUMN, spec 6). Mark the column absent for
		 * this group; the row/vector producers substitute the missing value.
		 */
		if (m == NULL)
		{
			readState->existsBase[c] = NULL;
			readState->valueCursor[c] = NULL;
			continue;
		}

		readState->existsBase[c] = readState->stripeBuffer + m->existsStreamOffset;

		if (columnar_is_projected(readState, c))
		{
			Form_pg_attribute att = TupleDescAttr(readState->tupdesc, c);
			char	   *decompressed =
				ColumnarGetDecompressedStream(readState->storageId,
											  readState->stripe->fileOffset +
											  m->valueStreamOffset,
											  readState->stripeBuffer +
											  m->valueStreamOffset,
											  m->valueStreamLength,
											  m->valueCompressionType,
											  m->valueDecompressedLength,
											  readState->groupContext);

			/* reverse the lightweight encoding to the raw value stream (I1) */
			readState->valueCursor[c] =
				ColumnarDecodeChunk(decompressed, m->valueDecompressedLength,
									m->valueEncodingType, att, m->valueCount,
									m->valueRawLength, readState->groupContext);
		}
		else
			readState->valueCursor[c] = NULL;
	}
}

/*
 * columnar_position_group
 *		Advance from the current groupIndex to the next chunk group that could
 *		match the pushed-down predicates, skipping groups whose min/max rule
 *		them out (spec 9). Returns true when positioned on a readable group,
 *		false when the stripe has no more matching groups.
 */
static bool
columnar_position_group(ColumnarReadState *readState)
{
	while (readState->groupIndex < readState->chunkGroupCount)
	{
		int			g = readState->groupIndex;

		if (columnar_group_can_match(readState, g))
		{
			readState->groupsRead++;
			columnar_setup_group(readState, g);
			return true;
		}

		/* skip this group: account for its rows and move on */
		{
			ChunkGroupMetadata *cg = list_nth(readState->chunkGroupList, g);

			readState->rowOffsetInStripe += cg->rowCount;
		}
		readState->groupsSkipped++;
		readState->groupIndex++;
	}

	return false;
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
														   readState->metaSnapshot);
	chunkList = ColumnarReadChunkList(readState->storageId, stripe->stripeNum,
									  readState->metaSnapshot);

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

	/*
	 * Load the row mask for this stripe (spec 7.5) and index it by chunk-group
	 * number, so deleted rows can be skipped during the scan. A NULL entry
	 * means the chunk group has no deletes.
	 */
	readState->groupMask = palloc0(sizeof(char *) * readState->chunkGroupCount);
	readState->groupMaskLen = palloc0(sizeof(uint32) * readState->chunkGroupCount);
	{
		List	   *rowMaskList = ColumnarReadRowMaskList(readState->storageId,
														  stripe->stripeNum,
														  readState->metaSnapshot);
		ListCell   *mlc;

		foreach(mlc, rowMaskList)
		{
			RowMaskMetadata *rm = (RowMaskMetadata *) lfirst(mlc);

			if (rm->chunkId >= 0 && rm->chunkId < readState->chunkGroupCount &&
				rm->mask != NULL)
			{
				readState->groupMask[rm->chunkId] = rm->mask;
				readState->groupMaskLen[rm->chunkId] = rm->maskLen;
			}
		}
	}

	/* read the stripe's data area into a contiguous buffer */
	readState->stripeBuffer = palloc(stripe->dataLength);
	ColumnarReadLogicalData(readState->rel, stripe->fileOffset,
							readState->stripeBuffer, stripe->dataLength);

	/* the caller positions on the first matching group via columnar_position_group */

	MemoryContextSwitchTo(oldContext);
}

/*
 * columnar_read_start
 *		Lazily load the stripe list on the first fetch. For a parallel scan a
 *		single worker claims the whole scan and the others see it exhausted,
 *		which is a correct (if not parallel-accelerated) behaviour.
 */
static void
columnar_read_start(ColumnarReadState *readState)
{
	if (readState->started)
		return;

	readState->started = true;

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
													   readState->metaSnapshot);
		readState->stripeIndex = 0;
		MemoryContextSwitchTo(oldContext);
	}
}

bool
ColumnarReadNextRow(ColumnarReadState *readState, Datum *values, bool *nulls,
					uint64 *rowNumber)
{
	columnar_read_start(readState);

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

			readState->groupIndex = 0;
			if (!columnar_position_group(readState))
			{
				readState->stripe = NULL;
				continue;
			}
		}

		if (readState->rowInGroup >= readState->groupRowCount)
		{
			readState->rowOffsetInStripe += readState->groupRowCount;
			readState->groupIndex++;

			if (!columnar_position_group(readState))
			{
				readState->stripe = NULL;
				continue;
			}
		}

		/* produce the current row */
		MemoryContextReset(readState->rowContext);

		{
			int			c;

			for (c = 0; c < readState->natts; c++)
			{
				char		exists;

				/*
				 * A value still has to be consumed from the stream even for a
				 * present row so the cursor stays aligned; but for a column
				 * that is not projected we never decoded it, so return NULL.
				 */
				if (!columnar_is_projected(readState, c))
				{
					values[c] = (Datum) 0;
					nulls[c] = true;
					continue;
				}

				/*
				 * Column absent from this stripe (added by ALTER TABLE ADD
				 * COLUMN after it was written): produce the missing value
				 * (spec 6).
				 */
				if (readState->existsBase[c] == NULL)
				{
					values[c] = readState->missingValues[c];
					nulls[c] = readState->missingIsnull[c];
					continue;
				}

				exists = readState->existsBase[c][readState->rowInGroup];

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

		/*
		 * If this row is marked deleted in the row mask (spec 7.5), skip it.
		 * The value cursors were already advanced above, so the stream stays
		 * aligned for the following rows.
		 */
		{
			uint64		bitIndex = readState->rowInGroup;
			bool		deleted = false;

			if (readState->currentMask != NULL &&
				(bitIndex >> 3) < readState->currentMaskLen)
				deleted = (readState->currentMask[bitIndex >> 3] &
						   (1 << (bitIndex & 7))) != 0;

			readState->rowInGroup++;

			if (deleted)
				continue;
		}

		return true;
	}
}

/*
 * columnar_decode_group_to_vector
 *		Decode the chunk group the read state is currently positioned on into
 *		flat per-column arrays, plus the per-row deleted flag from the row mask.
 *		Each projected column is decoded in a tight loop over the whole group;
 *		non-projected columns are left as NULL arrays. Everything is allocated in
 *		the group context, which the next group's setup resets, so the batch is
 *		valid only until the following ColumnarReadNextVector call.
 */
static void
columnar_decode_group_to_vector(ColumnarReadState *readState, ColumnarVector *vec)
{
	MemoryContext oldContext = MemoryContextSwitchTo(readState->groupContext);
	int			natts = readState->natts;
	uint64		nrows = readState->groupRowCount;
	int			c;
	uint64		i;

	vec->nrows = nrows;
	vec->firstRowNumber = readState->stripe->firstRowNumber +
		readState->rowOffsetInStripe;
	vec->values = palloc0(sizeof(Datum *) * natts);
	vec->isnull = palloc0(sizeof(bool *) * natts);
	vec->deleted = palloc0(sizeof(bool) * (nrows == 0 ? 1 : nrows));

	for (c = 0; c < natts; c++)
	{
		Datum	   *vals;
		bool	   *nulls;
		char	   *cursor;
		char	   *existsBase;
		Form_pg_attribute att;

		if (!columnar_is_projected(readState, c))
			continue;

		vals = palloc(sizeof(Datum) * (nrows == 0 ? 1 : nrows));
		nulls = palloc(sizeof(bool) * (nrows == 0 ? 1 : nrows));
		cursor = readState->valueCursor[c];
		existsBase = readState->existsBase[c];
		att = TupleDescAttr(readState->tupdesc, c);

		/*
		 * Column absent from this stripe (added by ALTER TABLE ADD COLUMN
		 * after it was written): fill the whole group with the missing value
		 * (spec 6).
		 */
		if (existsBase == NULL)
		{
			for (i = 0; i < nrows; i++)
			{
				vals[i] = readState->missingValues[c];
				nulls[i] = readState->missingIsnull[c];
			}
			vec->values[c] = vals;
			vec->isnull[c] = nulls;
			continue;
		}

		for (i = 0; i < nrows; i++)
		{
			if (existsBase[i])
			{
				vals[i] = ColumnarDecodeValue(att, &cursor,
											  readState->groupContext);
				nulls[i] = false;
			}
			else
			{
				vals[i] = (Datum) 0;
				nulls[i] = true;
			}
		}

		vec->values[c] = vals;
		vec->isnull[c] = nulls;
	}

	for (i = 0; i < nrows; i++)
	{
		bool		deleted = false;

		if (readState->currentMask != NULL &&
			(i >> 3) < readState->currentMaskLen)
			deleted = (readState->currentMask[i >> 3] & (1 << (i & 7))) != 0;

		vec->deleted[i] = deleted;
	}

	MemoryContextSwitchTo(oldContext);
}

/*
 * ColumnarReadNextVector
 *		Advance to the next chunk group that survives min/max skipping and decode
 *		it into a ColumnarVector (spec 9). Returns false at end of scan. This
 *		drives the same stripe/group state machine as ColumnarReadNextRow, so it
 *		reads exactly the same groups (including chunk-group skipping and the row
 *		mask) but hands back a whole group at a time for vectorized processing.
 */
bool
ColumnarReadNextVector(ColumnarReadState *readState, ColumnarVector *vec)
{
	columnar_read_start(readState);

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
			readState->groupIndex = 0;

			if (!columnar_position_group(readState))
			{
				readState->stripe = NULL;
				continue;
			}
		}
		else
		{
			/* the previous call delivered this group; advance to the next */
			readState->rowOffsetInStripe += readState->groupRowCount;
			readState->groupIndex++;

			if (!columnar_position_group(readState))
			{
				readState->stripe = NULL;
				continue;
			}
		}

		columnar_decode_group_to_vector(readState, vec);
		return true;
	}
}

/*
 * ColumnarReadRowByNumber
 *		Fetch a single row addressed by its row number (spec 6). Used by the
 *		table AM's fetch-by-tid callback (UPDATE re-fetches the old row). Reads
 *		only the one chunk group that holds the row and decodes each column up
 *		to the row's position. Returns false when no stripe covers the row or
 *		the row is marked deleted in the row mask (spec 7.5).
 */
bool
ColumnarReadRowByNumber(Relation rel, Snapshot snapshot, uint64 rowNumber,
						Datum *values, bool *nulls)
{
	uint64		storageId = ColumnarStorageId(rel);
	TupleDesc	tupdesc = RelationGetDescr(rel);
	int			natts = tupdesc->natts;
	MemoryContext target = CurrentMemoryContext;
	MemoryContext tmp;
	MemoryContext oldContext;
	Snapshot	metaSnapshot;
	List	   *stripeList;
	ListCell   *lc;
	StripeMetadata *stripe = NULL;
	List	   *chunkList;
	List	   *rowMaskList;
	ChunkMetadata **chunkForGroup;
	char	   *stripeBuffer;
	uint64		offsetInStripe;
	int			chunkId;
	uint64		rowInGroup;
	int			c;
	bool		found = true;

	tmp = AllocSetContextCreate(CurrentMemoryContext, "columnar fetch",
								ALLOCSET_SMALL_SIZES);
	oldContext = MemoryContextSwitchTo(tmp);

	metaSnapshot = ColumnarCatalogSnapshot(snapshot);

	stripeList = ColumnarReadStripeList(storageId, metaSnapshot);
	foreach(lc, stripeList)
	{
		StripeMetadata *s = (StripeMetadata *) lfirst(lc);

		if (rowNumber >= s->firstRowNumber &&
			rowNumber < s->firstRowNumber + s->rowCount)
		{
			stripe = s;
			break;
		}
	}

	if (stripe == NULL)
	{
		MemoryContextSwitchTo(oldContext);
		MemoryContextDelete(tmp);
		return false;
	}

	offsetInStripe = rowNumber - stripe->firstRowNumber;
	chunkId = (int) (offsetInStripe / (uint64) stripe->chunkRowCount);
	rowInGroup = offsetInStripe - (uint64) chunkId * (uint64) stripe->chunkRowCount;

	/* deleted rows are not visible (spec 7.5) */
	rowMaskList = ColumnarReadRowMaskList(storageId, stripe->stripeNum,
										  metaSnapshot);
	foreach(lc, rowMaskList)
	{
		RowMaskMetadata *rm = (RowMaskMetadata *) lfirst(lc);

		if (rm->chunkId == chunkId && rm->mask != NULL &&
			(rowInGroup >> 3) < rm->maskLen &&
			(rm->mask[rowInGroup >> 3] & (1 << (rowInGroup & 7))) != 0)
		{
			MemoryContextSwitchTo(oldContext);
			MemoryContextDelete(tmp);
			return false;
		}
	}

	/* index this chunk group's chunks by attribute */
	chunkList = ColumnarReadChunkList(storageId, stripe->stripeNum, metaSnapshot);
	chunkForGroup = palloc0(sizeof(ChunkMetadata *) * natts);
	foreach(lc, chunkList)
	{
		ChunkMetadata *m = (ChunkMetadata *) lfirst(lc);

		if (m->chunkGroupNum == chunkId && m->attrNum - 1 < natts)
			chunkForGroup[m->attrNum - 1] = m;
	}

	stripeBuffer = palloc(stripe->dataLength);
	ColumnarReadLogicalData(rel, stripe->fileOffset, stripeBuffer,
							stripe->dataLength);

	for (c = 0; c < natts; c++)
	{
		ChunkMetadata *m = chunkForGroup[c];
		Form_pg_attribute att = TupleDescAttr(tupdesc, c);
		char	   *existsBase;
		char	   *cursor;
		uint64		i;

		/*
		 * Column absent from this stripe (added by ALTER TABLE ADD COLUMN
		 * after it was written): produce the attribute's missing value, the
		 * same value a sequential scan yields for this row (spec 6).
		 */
		if (m == NULL)
		{
			values[c] = getmissingattr(tupdesc, c + 1, &nulls[c]);
			continue;
		}

		existsBase = stripeBuffer + m->existsStreamOffset;
		cursor = ColumnarDecompressValueStream(stripeBuffer + m->valueStreamOffset,
											   m->valueStreamLength,
											   m->valueCompressionType,
											   m->valueDecompressedLength,
											   tmp);
		/* reverse the lightweight encoding to the raw value stream (I1) */
		cursor = ColumnarDecodeChunk(cursor, m->valueDecompressedLength,
									 m->valueEncodingType, att, m->valueCount,
									 m->valueRawLength, tmp);

		for (i = 0; i <= rowInGroup; i++)
		{
			char		exists = existsBase[i];

			if (i == rowInGroup)
			{
				if (exists)
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
			else if (exists)
			{
				/* advance the cursor past earlier present values */
				(void) ColumnarDecodeValue(att, &cursor, tmp);
			}
		}
	}

	MemoryContextSwitchTo(oldContext);
	MemoryContextDelete(tmp);

	return found;
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

/*
 * ColumnarReadStats
 *		Report how many chunk groups the scan has read versus skipped by the
 *		min/max skip lists (spec 9). Used by the custom scan's EXPLAIN output.
 */
void
ColumnarReadStats(ColumnarReadState *readState, uint64 *groupsRead,
				  uint64 *groupsSkipped, uint64 *groupsTotal)
{
	*groupsRead = readState->groupsRead;
	*groupsSkipped = readState->groupsSkipped;
	*groupsTotal = readState->groupsRead + readState->groupsSkipped;
}
