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

	/* bloom-filter probe for equality (I7, gap 25): set for a hashable equality
	 * predicate on a safe collation, matching how the filter was built */
	bool		hasHash;
	FmgrInfo	hashFn;
	Oid			hashCollation;
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

	/*
	 * Parallel scan work claiming (gap 23). When non-NULL, this shared atomic
	 * hands out the next stripe index across all workers; each worker scans the
	 * whole stripes it claims. NULL for a serial scan, which walks stripeIndex.
	 */
	pg_atomic_uint32 *parallelCounter;

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
static bool columnar_advance_group(ColumnarReadState *readState);
static int	columnar_next_stripe_index(ColumnarReadState *readState);
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
	return ColumnarBeginReadWithStorage(rel, snapshot, ColumnarStorageId(rel),
										RelationGetDescr(rel), parallelScan,
										projectedColumns, nkeys, keys);
}

ColumnarReadState *
ColumnarBeginReadWithStorage(Relation rel, Snapshot snapshot,
							 uint64 storageId, TupleDesc tupdesc,
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
	readState->tupdesc = tupdesc;
	readState->natts = readState->tupdesc->natts;
	readState->storageId = storageId;

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

		tce = lookup_type_cache(att->atttypid,
								TYPECACHE_CMP_PROC_FINFO |
								TYPECACHE_HASH_PROC_FINFO);
		if (!OidIsValid(tce->cmp_proc_finfo.fn_oid))
			continue;

		readState->predicates[n].attidx = attidx;
		readState->predicates[n].strategy = key->sk_strategy;
		readState->predicates[n].compareValue = key->sk_argument;
		fmgr_info_copy(&readState->predicates[n].cmpFn, &tce->cmp_proc_finfo,
					   readState->readContext);
		readState->predicates[n].collation = att->attcollation;

		/*
		 * For an equality predicate on a hashable column with a safe collation,
		 * enable the bloom-filter probe (I7, gap 25), matching how the filter was
		 * built. The scan key already matches the column collation (a
		 * differently collated predicate is not pushed; see ColumnarBuildScanKeys),
		 * so hashing the constant under the column collation is consistent.
		 */
		readState->predicates[n].hasHash = false;
		if (key->sk_strategy == BTEqualStrategyNumber &&
			OidIsValid(tce->hash_proc_finfo.fn_oid) &&
			ColumnarCollationIsDeterministic(att->attcollation))
		{
			readState->predicates[n].hasHash = true;
			fmgr_info_copy(&readState->predicates[n].hashFn,
						   &tce->hash_proc_finfo, readState->readContext);
			readState->predicates[n].hashCollation = att->attcollation;
		}
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

				/*
				 * min/max did not rule the group out; consult the bloom filter
				 * (I7). If the value is provably absent, skip the group -- this
				 * is what prunes equality probes on unsorted columns.
				 */
				if (columnar_enable_bloom_filter && pred->hasHash &&
					m->bloomFilter != NULL)
				{
					uint32		h = DatumGetUInt32(
						FunctionCall1Coll(&pred->hashFn, pred->hashCollation,
										  pred->compareValue));

					if (!ColumnarBloomProbe(m->bloomFilter, m->bloomLen, h))
						return false;
				}
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

		/* the stream offsets come from the catalog; reject any that fall outside
		 * the stripe buffer before they are used to index it */
		if ((uint64) m->existsStreamOffset + readState->groupRowCount >
			readState->stripe->dataLength ||
			(uint64) m->valueStreamOffset + m->valueStreamLength >
			readState->stripe->dataLength)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("columnar: corrupt chunk stream offset in stripe")));

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

		/* reject corrupt catalog attr/group numbers before indexing */
		if (m->attrNum >= 1 && m->attrNum <= natts &&
			m->chunkGroupNum >= 0 &&
			m->chunkGroupNum < readState->chunkGroupCount)
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
			int			si = columnar_next_stripe_index(readState);

			if (si < 0)
			{
				readState->exhausted = true;
				return false;
			}

			columnar_load_stripe(readState,
								 list_nth(readState->stripeList, si));

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
columnar_decode_group_columns(ColumnarReadState *readState, ColumnarVector *vec,
							  Bitmapset *cols, bool init, const bool *sel)
{
	MemoryContext oldContext = MemoryContextSwitchTo(readState->groupContext);
	int			natts = readState->natts;
	uint64		nrows = readState->groupRowCount;
	int			c;
	uint64		i;

	/*
	 * On the first call for a group, allocate the vector and resolve the
	 * per-row deleted flags; later calls decode more columns into the same
	 * vector (late materialization, I8). Only columns in `cols` are decoded
	 * (all projected columns when cols is NULL), and a column already decoded
	 * is skipped.
	 */
	if (init)
	{
		vec->nrows = nrows;
		vec->firstRowNumber = readState->stripe->firstRowNumber +
			readState->rowOffsetInStripe;
		vec->values = palloc0(sizeof(Datum *) * natts);
		vec->isnull = palloc0(sizeof(bool *) * natts);
		vec->deleted = palloc0(sizeof(bool) * (nrows == 0 ? 1 : nrows));

		for (i = 0; i < nrows; i++)
		{
			bool		deleted = false;

			if (readState->currentMask != NULL &&
				(i >> 3) < readState->currentMaskLen)
				deleted = (readState->currentMask[i >> 3] & (1 << (i & 7))) != 0;

			vec->deleted[i] = deleted;
		}
	}

	for (c = 0; c < natts; c++)
	{
		Datum	   *vals;
		bool	   *nulls;
		char	   *cursor;
		char	   *existsBase;
		Form_pg_attribute att;

		if (!columnar_is_projected(readState, c))
			continue;
		if (cols != NULL && !bms_is_member(c, cols))
			continue;
		if (vec->values[c] != NULL)
			continue;			/* already decoded for this group */

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
				if (sel == NULL || sel[i])
				{
					vals[i] = ColumnarDecodeValue(att, &cursor,
												  readState->groupContext);
					nulls[i] = false;
				}
				else
				{
					/*
					 * Position-list late materialization (gap 22): a present but
					 * unselected value is skipped -- advance the cursor past it
					 * without copying it (the win for by-ref/varlena output
					 * columns). The slot is never read (sel[i] is false).
					 */
					cursor += (att->attlen > 0) ? att->attlen
						: (int) VARSIZE_ANY(cursor);
					vals[i] = (Datum) 0;
					nulls[i] = true;
				}
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

	MemoryContextSwitchTo(oldContext);
}

/* decode all projected columns of the current group (the common path) */
static void
columnar_decode_group_to_vector(ColumnarReadState *readState, ColumnarVector *vec)
{
	columnar_decode_group_columns(readState, vec, NULL, true, NULL);
}

/*
 * ColumnarAdvanceGroup / ColumnarDecodeGroupColumns
 *		Public split of "position on the next group" from "decode its columns",
 *		so a scan can decode only the predicate columns, filter, and decode the
 *		remaining output columns only when the group has surviving rows (late
 *		materialization, I8). Pass cols = NULL to decode every projected column.
 */
bool
ColumnarAdvanceGroup(ColumnarReadState *readState)
{
	return columnar_advance_group(readState);
}

/*
 * columnar_next_stripe_index
 *		The next stripe to scan, or -1 when none remain. A parallel scan claims
 *		it from the shared atomic (each worker gets distinct stripes); a serial
 *		scan walks stripeIndex (gap 23).
 */
static int
columnar_next_stripe_index(ColumnarReadState *readState)
{
	int			nstripes = list_length(readState->stripeList);
	uint32		si;

	if (readState->parallelCounter != NULL)
		si = pg_atomic_fetch_add_u32(readState->parallelCounter, 1);
	else
		si = (uint32) readState->stripeIndex++;

	return (si < (uint32) nstripes) ? (int) si : -1;
}

void
ColumnarReadSetParallelCounter(ColumnarReadState *readState,
							   pg_atomic_uint32 *counter)
{
	readState->parallelCounter = counter;
}

void
ColumnarDecodeGroupColumns(ColumnarReadState *readState, ColumnarVector *vec,
						   Bitmapset *cols, bool init, const bool *sel)
{
	columnar_decode_group_columns(readState, vec, cols, init, sel);
}

/*
 * ColumnarReadNextVector
 *		Advance to the next chunk group that survives min/max skipping and decode
 *		it into a ColumnarVector (spec 9). Returns false at end of scan. This
 *		drives the same stripe/group state machine as ColumnarReadNextRow, so it
 *		reads exactly the same groups (including chunk-group skipping and the row
 *		mask) but hands back a whole group at a time for vectorized processing.
 */
/*
 * columnar_advance_group
 *		Drive the stripe/group state machine to the next chunk group that
 *		survives min/max skipping (spec 9), loading stripes as needed. Returns
 *		true when positioned on a readable group (readState->groupIndex), false
 *		when the scan is exhausted. Shared by the vector and raw-group readers.
 */
static bool
columnar_advance_group(ColumnarReadState *readState)
{
	columnar_read_start(readState);

	for (;;)
	{
		if (readState->exhausted)
			return false;

		if (readState->stripe == NULL)
		{
			int			si = columnar_next_stripe_index(readState);

			if (si < 0)
			{
				readState->exhausted = true;
				return false;
			}

			columnar_load_stripe(readState,
								 list_nth(readState->stripeList, si));
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

		return true;
	}
}

bool
ColumnarReadNextVector(ColumnarReadState *readState, ColumnarVector *vec)
{
	if (!columnar_advance_group(readState))
		return false;

	columnar_decode_group_to_vector(readState, vec);
	return true;
}

/*
 * ColumnarReadNextRawGroup
 *		Advance to the next readable chunk group and expose each projected
 *		column's raw value stream (packed non-null values) without decoding to
 *		Datums, for the aggregate's compressed-execution run path (I3).
 */
bool
ColumnarReadNextRawGroup(ColumnarReadState *readState, ColumnarRawGroup *rg)
{
	MemoryContext oldContext;
	int			natts = readState->natts;
	int			c;
	uint64		i;

	if (!columnar_advance_group(readState))
		return false;

	oldContext = MemoryContextSwitchTo(readState->groupContext);

	rg->nrows = readState->groupRowCount;
	rg->firstRowNumber = readState->stripe->firstRowNumber +
		readState->rowOffsetInStripe;
	rg->natts = natts;
	rg->valueCursor = palloc0(sizeof(char *) * natts);
	rg->groupValueCount = palloc0(sizeof(uint64) * natts);
	rg->columnAbsent = palloc0(sizeof(bool) * natts);

	for (c = 0; c < natts; c++)
	{
		ChunkMetadata *m = readState->chunkMap[c][readState->groupIndex];

		if (m == NULL)
		{
			rg->columnAbsent[c] = true;
			continue;
		}
		rg->valueCursor[c] = readState->valueCursor[c];
		rg->groupValueCount[c] = m->valueCount;
	}

	/* how many of this group's rows are row-mask-deleted (spec 7.5) */
	rg->deletedCount = 0;
	if (readState->currentMask != NULL)
	{
		for (i = 0; i < rg->nrows; i++)
			if ((i >> 3) < readState->currentMaskLen &&
				(readState->currentMask[i >> 3] & (1 << (i & 7))) != 0)
				rg->deletedCount++;
	}

	MemoryContextSwitchTo(oldContext);
	return true;
}

/*
 * ColumnarDecodeCurrentGroupVector
 *		Decode the currently positioned chunk group into a full vector, used by
 *		the aggregate to fall back from the run path when the group has deletes.
 */
void
ColumnarDecodeCurrentGroupVector(ColumnarReadState *readState,
								 ColumnarVector *vec)
{
	columnar_decode_group_to_vector(readState, vec);
}

/* -------------------------------------------------------------------------
 * Liveness cache (gap 26, phase 4): a projection scan must test each row's base
 * row number for deletion/visibility. Doing that per row via ColumnarRowIsLive
 * re-scans the stripe and row-mask catalogs every call -- O(rows x stripes).
 * The cache reads the base stripe list and row masks once (at the scan's
 * snapshot) into memory, then answers each test with a binary search over
 * stripes plus a bitmap probe. Consistent with the scan's fixed snapshot, the
 * same way ColumnarBeginRead reads those lists once at begin.
 * ------------------------------------------------------------------------- */
typedef struct LiveStripeEntry
{
	uint64		firstRowNumber;
	uint64		rowCount;
	int			chunkRowCount;
	int			chunkGroupCount;
	char	  **masks;			/* [chunkGroupCount], deleted bitmap or NULL */
	uint32	   *maskLens;		/* [chunkGroupCount] */
} LiveStripeEntry;

struct ColumnarLivenessCache
{
	LiveStripeEntry *stripes;	/* sorted ascending by firstRowNumber */
	int			nstripes;
	MemoryContext ctx;
};

static int
livestripe_cmp(const void *a, const void *b)
{
	const LiveStripeEntry *ea = (const LiveStripeEntry *) a;
	const LiveStripeEntry *eb = (const LiveStripeEntry *) b;

	if (ea->firstRowNumber < eb->firstRowNumber)
		return -1;
	if (ea->firstRowNumber > eb->firstRowNumber)
		return 1;
	return 0;
}

ColumnarLivenessCache *
ColumnarBuildLivenessCache(Relation rel, Snapshot snapshot)
{
	uint64		storageId = ColumnarStorageId(rel);
	Snapshot	metaSnapshot = ColumnarCatalogSnapshot(snapshot);
	MemoryContext ctx = AllocSetContextCreate(CurrentMemoryContext,
											  "columnar liveness cache",
											  ALLOCSET_DEFAULT_SIZES);
	MemoryContext oldContext = MemoryContextSwitchTo(ctx);
	List	   *stripeList = ColumnarReadStripeList(storageId, metaSnapshot);
	ColumnarLivenessCache *cache = palloc0(sizeof(ColumnarLivenessCache));
	ListCell   *lc;
	int			i = 0;

	cache->ctx = ctx;
	cache->nstripes = list_length(stripeList);
	cache->stripes = palloc0(sizeof(LiveStripeEntry) * Max(cache->nstripes, 1));

	foreach(lc, stripeList)
	{
		StripeMetadata *s = (StripeMetadata *) lfirst(lc);
		LiveStripeEntry *e = &cache->stripes[i++];
		List	   *rml;
		ListCell   *mc;

		e->firstRowNumber = s->firstRowNumber;
		e->rowCount = s->rowCount;
		e->chunkRowCount = s->chunkRowCount;
		e->chunkGroupCount = s->chunkGroupCount;
		e->masks = palloc0(sizeof(char *) * Max(s->chunkGroupCount, 1));
		e->maskLens = palloc0(sizeof(uint32) * Max(s->chunkGroupCount, 1));

		rml = ColumnarReadRowMaskList(storageId, s->stripeNum, metaSnapshot);
		foreach(mc, rml)
		{
			RowMaskMetadata *rm = (RowMaskMetadata *) lfirst(mc);

			if (rm->chunkId >= 0 && rm->chunkId < s->chunkGroupCount &&
				rm->mask != NULL && rm->maskLen > 0)
			{
				e->masks[rm->chunkId] = palloc(rm->maskLen);
				memcpy(e->masks[rm->chunkId], rm->mask, rm->maskLen);
				e->maskLens[rm->chunkId] = rm->maskLen;
			}
		}
	}

	if (cache->nstripes > 1)
		qsort(cache->stripes, cache->nstripes, sizeof(LiveStripeEntry),
			  livestripe_cmp);

	MemoryContextSwitchTo(oldContext);
	return cache;
}

bool
ColumnarLivenessCacheIsLive(ColumnarLivenessCache *cache, uint64 rowNumber)
{
	int			lo = 0;
	int			hi = cache->nstripes - 1;

	while (lo <= hi)
	{
		int			mid = (lo + hi) / 2;
		LiveStripeEntry *e = &cache->stripes[mid];

		if (rowNumber < e->firstRowNumber)
			hi = mid - 1;
		else if (rowNumber >= e->firstRowNumber + e->rowCount)
			lo = mid + 1;
		else
		{
			uint64		off = rowNumber - e->firstRowNumber;
			int			chunkId = (e->chunkRowCount > 0)
				? (int) (off / (uint64) e->chunkRowCount) : 0;
			uint64		inGroup = off - (uint64) chunkId * (uint64) e->chunkRowCount;

			if (chunkId >= 0 && chunkId < e->chunkGroupCount &&
				e->masks[chunkId] != NULL &&
				(inGroup >> 3) < e->maskLens[chunkId] &&
				(e->masks[chunkId][inGroup >> 3] & (1 << (inGroup & 7))) != 0)
				return false;	/* deleted */
			return true;		/* covered and not deleted */
		}
	}
	return false;				/* no covering stripe: not visible */
}

void
ColumnarFreeLivenessCache(ColumnarLivenessCache *cache)
{
	if (cache != NULL)
		MemoryContextDelete(cache->ctx);
}

/*
 * ColumnarRowIsLive
 *		Whether the base row addressed by rowNumber is visible to snapshot: a
 *		stripe covers it (its catalog row is visible) and it is not marked
 *		deleted in the row mask. Unlike ColumnarReadRowByNumber this decodes no
 *		column data, so a projection scan can filter deleted/invisible rows
 *		cheaply by their stored base row number (gap 26, phase 4). Uses the same
 *		catalog snapshot rule as the reader.
 */
bool
ColumnarRowIsLive(Relation rel, Snapshot snapshot, uint64 rowNumber)
{
	uint64		storageId = ColumnarStorageId(rel);
	MemoryContext tmp;
	MemoryContext oldContext;
	Snapshot	metaSnapshot;
	List	   *stripeList;
	ListCell   *lc;
	StripeMetadata *stripe = NULL;
	List	   *rowMaskList;
	uint64		offsetInStripe;
	int			chunkId;
	uint64		rowInGroup;
	bool		live = true;

	tmp = AllocSetContextCreate(CurrentMemoryContext, "columnar liveness",
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

	if (stripe == NULL || stripe->chunkRowCount <= 0)
	{
		MemoryContextSwitchTo(oldContext);
		MemoryContextDelete(tmp);
		return false;
	}

	offsetInStripe = rowNumber - stripe->firstRowNumber;
	chunkId = (int) (offsetInStripe / (uint64) stripe->chunkRowCount);
	rowInGroup = offsetInStripe - (uint64) chunkId * (uint64) stripe->chunkRowCount;

	rowMaskList = ColumnarReadRowMaskList(storageId, stripe->stripeNum,
										  metaSnapshot);
	foreach(lc, rowMaskList)
	{
		RowMaskMetadata *rm = (RowMaskMetadata *) lfirst(lc);

		if (rm->chunkId == chunkId && rm->mask != NULL &&
			(rowInGroup >> 3) < rm->maskLen &&
			(rm->mask[rowInGroup >> 3] & (1 << (rowInGroup & 7))) != 0)
		{
			live = false;
			break;
		}
	}

	MemoryContextSwitchTo(oldContext);
	MemoryContextDelete(tmp);
	return live;
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

	if (stripe->chunkRowCount <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("columnar: corrupt stripe chunk row count")));

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

		if (m->chunkGroupNum == chunkId &&
			m->attrNum >= 1 && m->attrNum <= natts)
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

		if ((uint64) m->existsStreamOffset + rowInGroup + 1 > stripe->dataLength ||
			(uint64) m->valueStreamOffset + m->valueStreamLength >
			stripe->dataLength)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("columnar: corrupt chunk stream offset in stripe")));

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
