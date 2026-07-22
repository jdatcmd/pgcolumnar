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

	/*
	 * Native format (re-origination line, PGCN v1) read state (Phase D3). When
	 * isNative, the scan reads row groups and column chunks from the native
	 * catalog instead of stripes/chunks. The current row group's bytes are read
	 * whole into nativeBuffer (in groupContext); nativeValidity[c] points at each
	 * column chunk's validity bitmap and nativeValueCursor[c] advances through its
	 * uncompressed values. Sequential scan only: skip predicates, the vectorized
	 * path, projection pushdown, and parallel scan are bypassed for native tables
	 * in D3 (correctness-neutral optimizations for later).
	 */
	bool		isNative;
	List	   *rowGroupList;		/* NativeRowGroupMetadata* */
	int			rowGroupIndex;		/* next row group to load */
	NativeRowGroupMetadata *nativeGroup;
	char	   *nativeBuffer;		/* whole current row group, in groupContext */
	char	  **nativeValidity;		/* [natts]; NULL if the column is absent */
	char	  **nativeValueCursor;	/* [natts]; advancing values cursor */

	/*
	 * Per-vector (1024-row) skipping within a loaded group (Phase D5b). When any
	 * predicate's per-vector zone map rules a vector out, its rows are neither
	 * decoded nor emitted. nativeSkipVec[v] flags a ruled-out vector;
	 * nativeVecRawLen[c][v] is column c's decoded byte length for vector v, used to
	 * step the value cursor past a skipped vector. Both NULL when per-vector
	 * skipping is inactive (no predicates or no per-vector zone maps).
	 */
	bool	   *nativeSkipVec;		/* [nativeVectorCount] or NULL */
	int			nativeVectorCount;
	uint32	  **nativeVecRawLen;	/* [natts][nativeVectorCount] or NULL */
	uint32	   *nativeVecStart;		/* [nativeVectorCount+1] cumulative row spans */
	int			nativeCurVec;		/* vector containing rowInGroup */
	uint64		vectorsSkipped;		/* for EXPLAIN */

	/*
	 * Native delete visibility (Phase D6b): the current row group's combined
	 * delete mask (one bit per row-in-group, set = deleted), from
	 * pgcolumnar.row_mask keyed by group number. NULL when the group has no
	 * deletes. The interim row mask; Phase F replaces it with a delete vector.
	 */
	char	   *nativeDeleteMask;
	uint32		nativeDeleteMaskLen;

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

	/*
	 * The storage being read is native iff it has a native storage catalog row
	 * (D6a). This is the storage's own format, not the base table's option, so a
	 * native base table's 2.2 projection storage is correctly read as 2.2, and the
	 * D6 default flip needs no change here (the reader follows the data). Read
	 * paths flush pending writes first, so a just-written native storage is seen.
	 */
	readState->isNative =
		ColumnarStorageIsNative(storageId, readState->metaSnapshot);
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

		if (readState->isNative)
		{
			readState->rowGroupList =
				ColumnarReadRowGroupList(readState->storageId,
										 readState->metaSnapshot);
			readState->rowGroupIndex = 0;
		}
		else
		{
			readState->stripeList = ColumnarReadStripeList(readState->storageId,
														   readState->metaSnapshot);
			readState->stripeIndex = 0;
		}
		MemoryContextSwitchTo(oldContext);
	}
}

/*
 * columnar_native_decode_chunk
 *		Reconstruct a native column chunk's raw present-value stream (D4) from its
 *		encoding descriptor. The on-disk values region is the per-1024-value-vector
 *		encoded streams concatenated, optionally block-compressed as a whole. This
 *		reverses the block codec, then decodes each vector with ColumnarDecodeChunk
 *		into one raw buffer byte-identical to what the writer buffered, so the
 *		per-row producer walks it exactly as it walks the D2b baseline. Allocated
 *		in the group context. The descriptor lengths are cross-checked so a corrupt
 *		chunk cannot drive a decoder past its buffers.
 */
static char *
columnar_native_decode_chunk(MemoryContext cx, Form_pg_attribute att,
							 char *values, uint32 valuesLen,
							 const char *desc, uint32 descLen, int blockCodec,
							 uint32 **outVecRawLen, int *outVecCount)
{
	uint32		vectorCount;
	uint32		v;
	const char *dp;
	uint64		encTotal = 0;
	uint64		rawTotal = 0;
	const char *encRegion;
	const char *encCursor;
	char	   *rawBuf;
	char	   *rawCursor;
	uint32	   *vecRawLen;

	if (descLen < COLUMNAR_NATIVE_ENCDESC_HEADER_LEN ||
		(uint8) desc[0] != COLUMNAR_NATIVE_ENCDESC_VERSION)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("pgcolumnar: unrecognized native encoding descriptor")));
	memcpy(&vectorCount, desc + 2, sizeof(uint32));

	if ((uint64) descLen != (uint64) COLUMNAR_NATIVE_ENCDESC_HEADER_LEN +
		(uint64) vectorCount * COLUMNAR_NATIVE_ENCDESC_ENTRY_LEN)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("pgcolumnar: native encoding descriptor length mismatch")));

	/* first pass: total encoded and raw lengths across the vectors */
	dp = desc + COLUMNAR_NATIVE_ENCDESC_HEADER_LEN;
	for (v = 0; v < vectorCount; v++)
	{
		uint32		rawLen;
		uint32		encLen;

		memcpy(&rawLen, dp + 1 + sizeof(uint32), sizeof(uint32));
		memcpy(&encLen, dp + 1 + 2 * sizeof(uint32), sizeof(uint32));
		encTotal += encLen;
		rawTotal += rawLen;
		dp += COLUMNAR_NATIVE_ENCDESC_ENTRY_LEN;
	}

	/* reverse the block codec to recover the concatenated encoded region */
	if (blockCodec != COLUMNAR_COMPRESSION_NONE)
		encRegion = ColumnarDecompressValueStream(values, valuesLen, blockCodec,
												  (uint32) encTotal,
												  cx);
	else
	{
		if ((uint64) valuesLen != encTotal)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("pgcolumnar: native chunk length does not match descriptor")));
		encRegion = values;
	}

	/* second pass: decode each vector into one raw present-value buffer */
	rawBuf = MemoryContextAlloc(cx, rawTotal > 0 ? rawTotal : 1);
	vecRawLen = (uint32 *) MemoryContextAlloc(cx,
											  sizeof(uint32) * (vectorCount > 0 ? vectorCount : 1));
	rawCursor = rawBuf;
	encCursor = encRegion;
	dp = desc + COLUMNAR_NATIVE_ENCDESC_HEADER_LEN;
	for (v = 0; v < vectorCount; v++)
	{
		uint8		encType;
		uint32		valueCount;
		uint32		rawLen;
		uint32		encLen;

		encType = (uint8) dp[0];
		memcpy(&valueCount, dp + 1, sizeof(uint32));
		memcpy(&rawLen, dp + 1 + sizeof(uint32), sizeof(uint32));
		memcpy(&encLen, dp + 1 + 2 * sizeof(uint32), sizeof(uint32));
		dp += COLUMNAR_NATIVE_ENCDESC_ENTRY_LEN;

		vecRawLen[v] = rawLen;
		if (rawLen > 0)
		{
			char	   *rawVec = ColumnarDecodeChunk(encCursor, encLen, encType,
													 att, valueCount, rawLen,
													 cx);

			memcpy(rawCursor, rawVec, rawLen);
			rawCursor += rawLen;
		}
		encCursor += encLen;
	}

	if (outVecRawLen != NULL)
		*outVecRawLen = vecRawLen;
	if (outVecCount != NULL)
		*outVecCount = (int) vectorCount;

	return rawBuf;
}

/*
 * native_zone_excludes
 *		Return true when a zone map's min/max prove that no value in its range can
 *		satisfy the predicate (so the vector or chunk can be skipped). A missing or
 *		non-orderable zone map returns false (cannot prove empty). Shared by
 *		whole-chunk (group) and per-vector skipping (native spec 7.1). Decodes the
 *		stored min/max in cx.
 */
static bool
native_zone_excludes(SkipPredicate *pred, Form_pg_attribute att,
					 NativeZoneMapMetadata *z, MemoryContext cx)
{
	char	   *cur;
	Datum		minv;
	Datum		maxv;
	int32		c1;
	int32		c2;

	if (z == NULL || !z->hasMinMax)
		return false;

	cur = (char *) z->minimum;
	minv = ColumnarDecodeValue(att, &cur, cx);
	cur = (char *) z->maximum;
	maxv = ColumnarDecodeValue(att, &cur, cx);

	switch (pred->strategy)
	{
		case BTLessStrategyNumber:	/* col < const : skip if min >= const */
			c1 = DatumGetInt32(FunctionCall2Coll(&pred->cmpFn, pred->collation,
												 minv, pred->compareValue));
			return (c1 >= 0);
		case BTLessEqualStrategyNumber: /* col <= const : skip if min > const */
			c1 = DatumGetInt32(FunctionCall2Coll(&pred->cmpFn, pred->collation,
												 minv, pred->compareValue));
			return (c1 > 0);
		case BTEqualStrategyNumber: /* col = const : skip if const<min or const>max */
			c1 = DatumGetInt32(FunctionCall2Coll(&pred->cmpFn, pred->collation,
												 pred->compareValue, minv));
			c2 = DatumGetInt32(FunctionCall2Coll(&pred->cmpFn, pred->collation,
												 pred->compareValue, maxv));
			return (c1 < 0 || c2 > 0);
		case BTGreaterEqualStrategyNumber:	/* col >= const : skip if max < const */
			c2 = DatumGetInt32(FunctionCall2Coll(&pred->cmpFn, pred->collation,
												 maxv, pred->compareValue));
			return (c2 < 0);
		case BTGreaterStrategyNumber:	/* col > const : skip if max <= const */
			c2 = DatumGetInt32(FunctionCall2Coll(&pred->cmpFn, pred->collation,
												 maxv, pred->compareValue));
			return (c2 <= 0);
		default:
			return false;
	}
}

/*
 * columnar_native_group_can_match
 *		Decide whether a native row group could hold a row satisfying every
 *		pushed-down predicate, using its whole-chunk zone maps (native spec 7.1,
 *		Phase D5b). Returns false when the group can be skipped. Mirrors the 2.2
 *		columnar_group_can_match, reading min/max from pgcolumnar.zone_map instead
 *		of the 2.2 chunk catalog. A missing or non-orderable zone map is treated
 *		conservatively as "may match". Runs in rs->skipContext (caller-reset).
 */
static bool
columnar_native_group_can_match(ColumnarReadState *rs, uint64 groupNumber)
{
	List	   *zones;
	List	   *blooms;
	NativeZoneMapMetadata **byCol;
	NativeBloomMetadata **byColBloom;
	ListCell   *lc;
	int			p;

	if (rs->numPredicates == 0)
		return true;

	zones = ColumnarReadZoneMapList(rs->storageId, groupNumber, rs->metaSnapshot);
	byCol = palloc0(sizeof(NativeZoneMapMetadata *) * rs->natts);
	foreach(lc, zones)
	{
		NativeZoneMapMetadata *z = (NativeZoneMapMetadata *) lfirst(lc);

		if (z->columnIndex >= 0 && z->columnIndex < rs->natts)
			byCol[z->columnIndex] = z;
	}

	blooms = ColumnarReadBloomList(rs->storageId, groupNumber, rs->metaSnapshot);
	byColBloom = palloc0(sizeof(NativeBloomMetadata *) * rs->natts);
	foreach(lc, blooms)
	{
		NativeBloomMetadata *b = (NativeBloomMetadata *) lfirst(lc);

		if (b->columnIndex >= 0 && b->columnIndex < rs->natts)
			byColBloom[b->columnIndex] = b;
	}

	for (p = 0; p < rs->numPredicates; p++)
	{
		SkipPredicate *pred = &rs->predicates[p];
		Form_pg_attribute att = TupleDescAttr(rs->tupdesc, pred->attidx);

		if (native_zone_excludes(pred, att, byCol[pred->attidx], rs->skipContext))
			return false;

		/*
		 * min/max did not rule the group out; for equality consult the per-chunk
		 * bloom filter (native spec 7.2), which prunes equality probes on unsorted
		 * columns that min/max cannot.
		 */
		if (pred->strategy == BTEqualStrategyNumber &&
			columnar_enable_bloom_filter && pred->hasHash)
		{
			NativeBloomMetadata *b = byColBloom[pred->attidx];

			if (b != NULL && b->filter != NULL)
			{
				uint32		h = DatumGetUInt32(
					FunctionCall1Coll(&pred->hashFn, pred->hashCollation,
									  pred->compareValue));

				if (!ColumnarBloomProbe(b->filter, b->filterLen, h))
					return false;
			}
		}
	}

	return true;
}

/*
 * columnar_native_build_skipvec
 *		Build the per-vector skip flags for a loaded row group (native spec 7.1,
 *		Phase D5b): vector v is skipped when any predicate's per-vector zone map
 *		proves no row in it can match. Also fills rs->nativeVecStart with the
 *		cumulative row spans (from the zone maps' value+null counts, so it is
 *		correct for any chunk-group size). Sets rs->nativeSkipVec (or NULL when
 *		nothing is skippable or no per-vector zone maps exist) and
 *		rs->nativeVectorCount. Runs in the group context (caller-switched); decodes
 *		min/max in rs->skipContext.
 */
static void
columnar_native_build_skipvec(ColumnarReadState *rs, uint64 groupNumber, int vecCount)
{
	List	   *zones;
	NativeZoneMapMetadata ***byColVec;
	uint32	   *span;
	bool	   *skip;
	bool		any = false;
	ListCell   *lc;
	int			v;
	int			p;

	rs->nativeSkipVec = NULL;
	rs->nativeVecStart = NULL;
	rs->nativeVectorCount = vecCount;
	rs->nativeCurVec = 0;

	if (rs->numPredicates == 0 || vecCount <= 0)
		return;

	zones = ColumnarReadZoneMapVectors(rs->storageId, groupNumber, rs->metaSnapshot);
	if (zones == NIL)
		return;					/* legacy: no per-vector zone maps */

	/* per-predicate-column lookup [column][vector] */
	byColVec = (NativeZoneMapMetadata ***)
		palloc0(sizeof(NativeZoneMapMetadata **) * rs->natts);
	for (p = 0; p < rs->numPredicates; p++)
	{
		int			col = rs->predicates[p].attidx;

		if (col >= 0 && col < rs->natts && byColVec[col] == NULL)
			byColVec[col] = (NativeZoneMapMetadata **)
				palloc0(sizeof(NativeZoneMapMetadata *) * vecCount);
	}

	span = (uint32 *) palloc0(sizeof(uint32) * vecCount);
	foreach(lc, zones)
	{
		NativeZoneMapMetadata *z = (NativeZoneMapMetadata *) lfirst(lc);

		if (z->vectorIndex < 0 || z->vectorIndex >= vecCount)
			continue;
		span[z->vectorIndex] = (uint32) (z->valueCount + z->nullCount);
		if (z->columnIndex >= 0 && z->columnIndex < rs->natts &&
			byColVec[z->columnIndex] != NULL)
			byColVec[z->columnIndex][z->vectorIndex] = z;
	}

	MemoryContextReset(rs->skipContext);
	skip = (bool *) palloc0(sizeof(bool) * vecCount);
	for (v = 0; v < vecCount; v++)
	{
		for (p = 0; p < rs->numPredicates; p++)
		{
			SkipPredicate *pred = &rs->predicates[p];
			Form_pg_attribute att = TupleDescAttr(rs->tupdesc, pred->attidx);
			NativeZoneMapMetadata *z = byColVec[pred->attidx]
				? byColVec[pred->attidx][v] : NULL;

			if (native_zone_excludes(pred, att, z, rs->skipContext))
			{
				skip[v] = true;
				any = true;
				break;
			}
		}
	}

	/* cumulative row spans, for mapping a row to its vector */
	rs->nativeVecStart = (uint32 *) palloc0(sizeof(uint32) * (vecCount + 1));
	for (v = 0; v < vecCount; v++)
		rs->nativeVecStart[v + 1] = rs->nativeVecStart[v] + span[v];

	rs->nativeSkipVec = any ? skip : NULL;
}

/*
 * columnar_native_load_group
 *		Load the next native row group (PGCN v1, Phase D3): read its bytes whole
 *		into the group context and set each column's validity-bitmap pointer and
 *		values cursor. Row groups the zone maps prove cannot match are skipped
 *		(Phase D5b). Returns false when no more row groups remain.
 */
static bool
columnar_native_load_group(ColumnarReadState *rs)
{
	MemoryContext oldContext;
	NativeRowGroupMetadata *rg;
	List	   *chunks;
	ListCell   *lc;
	int			validityBytes;
	int			maxVecCount;
	bool		allDescriptor;

	/* advance past row groups the zone maps rule out (native spec 7.1) */
	while (rs->rowGroupIndex < list_length(rs->rowGroupList))
	{
		bool		match = true;

		rg = (NativeRowGroupMetadata *) list_nth(rs->rowGroupList,
												 rs->rowGroupIndex);
		if (rs->numPredicates > 0)
		{
			MemoryContext old = MemoryContextSwitchTo(rs->skipContext);

			MemoryContextReset(rs->skipContext);
			match = columnar_native_group_can_match(rs, rg->groupNumber);
			MemoryContextSwitchTo(old);
		}
		if (match)
			break;

		rs->groupsSkipped++;
		rs->rowGroupIndex++;
	}

	if (rs->rowGroupIndex >= list_length(rs->rowGroupList))
		return false;

	rs->groupsRead++;

	MemoryContextReset(rs->groupContext);
	oldContext = MemoryContextSwitchTo(rs->groupContext);

	rg = (NativeRowGroupMetadata *) list_nth(rs->rowGroupList, rs->rowGroupIndex);
	rs->nativeGroup = rg;
	rs->nativeBuffer = palloc(rg->byteLength > 0 ? rg->byteLength : 1);
	if (rg->byteLength > 0)
		ColumnarReadLogicalData(rs->rel, rg->fileOffset, rs->nativeBuffer,
								rg->byteLength);

	chunks = ColumnarReadColumnChunkList(rs->storageId, rg->groupNumber,
										 rs->metaSnapshot);
	rs->nativeValidity = palloc0(sizeof(char *) * rs->natts);
	rs->nativeValueCursor = palloc0(sizeof(char *) * rs->natts);
	rs->nativeVecRawLen = (uint32 **) palloc0(sizeof(uint32 *) * rs->natts);
	validityBytes = (int) ((rg->rowCount + 7) / 8);
	maxVecCount = 0;
	allDescriptor = (chunks != NIL);

	foreach(lc, chunks)
	{
		NativeColumnChunkMetadata *cc = (NativeColumnChunkMetadata *) lfirst(lc);
		char	   *base;

		if (cc->columnIndex < 0 || cc->columnIndex >= rs->natts)
			continue;
		base = rs->nativeBuffer + (cc->pageOffset - rg->fileOffset);
		rs->nativeValidity[cc->columnIndex] = base;

		if (cc->encodingDescriptorLen == 1 &&
			(uint8) cc->encodingDescriptor[0] == COLUMNAR_NATIVE_ENCDESC_BASELINE)
		{
			/* D2b baseline: raw present values follow the validity bitmap; no
			 * per-vector structure, so per-vector skipping is disabled below */
			rs->nativeValueCursor[cc->columnIndex] = base + validityBytes;
			allDescriptor = false;
		}
		else
		{
			Form_pg_attribute att = TupleDescAttr(rs->tupdesc, cc->columnIndex);
			uint32	   *vraw = NULL;
			int			vcount = 0;

			/* D4: reconstruct the raw present-value stream from the descriptor */
			rs->nativeValueCursor[cc->columnIndex] =
				columnar_native_decode_chunk(rs->groupContext, att, base + validityBytes,
											 (uint32) (cc->pageLength - validityBytes),
											 cc->encodingDescriptor,
											 cc->encodingDescriptorLen,
											 cc->blockCodec, &vraw, &vcount);
			rs->nativeVecRawLen[cc->columnIndex] = vraw;
			if (vcount > maxVecCount)
				maxVecCount = vcount;
		}
	}

	/*
	 * Per-vector skipping (native spec 7.1, D5b): build the skip vector from the
	 * per-vector zone maps. Only when every column carries a descriptor (so the
	 * vector boundaries line up); a legacy baseline chunk disables it.
	 */
	if (allDescriptor)
		columnar_native_build_skipvec(rs, rg->groupNumber, maxVecCount);
	else
	{
		rs->nativeSkipVec = NULL;
		rs->nativeVecStart = NULL;
		rs->nativeVectorCount = maxVecCount;
		rs->nativeCurVec = 0;
	}

	/*
	 * Native delete visibility (D6b): combine this group's row-mask rows (keyed
	 * by group number, one bit per row-in-group) into a single delete mask that
	 * columnar_native_next_row consults to skip deleted rows.
	 */
	rs->nativeDeleteMask = NULL;
	rs->nativeDeleteMaskLen = 0;
	{
		List	   *maskList = ColumnarReadRowMaskList(rs->storageId,
													   rg->groupNumber,
													   rs->metaSnapshot);
		ListCell   *mlc;
		uint32		want = (uint32) ((rg->rowCount + 7) / 8);

		foreach(mlc, maskList)
		{
			RowMaskMetadata *rm = (RowMaskMetadata *) lfirst(mlc);
			uint32		i;

			if (rm->mask == NULL || rm->maskLen == 0)
				continue;
			if (rs->nativeDeleteMask == NULL)
			{
				rs->nativeDeleteMask = palloc0(want > 0 ? want : 1);
				rs->nativeDeleteMaskLen = want;
			}
			for (i = 0; i < rm->maskLen && i < want; i++)
				rs->nativeDeleteMask[i] |= rm->mask[i];
		}
	}

	rs->groupRowCount = rg->rowCount;
	rs->rowInGroup = 0;
	rs->rowGroupIndex++;

	MemoryContextSwitchTo(oldContext);
	return true;
}

/*
 * columnar_native_skip_current_vector
 *		Per-vector skipping (native spec 7.1, D5b): when rowInGroup sits at the
 *		start of a vector the zone maps rule out, step each column's value cursor
 *		past that vector's decoded bytes and jump rowInGroup to the next vector,
 *		neither decoding nor emitting its rows. Returns true when it advanced (the
 *		caller re-checks bounds), false when the current row must be emitted.
 */
static bool
columnar_native_skip_current_vector(ColumnarReadState *rs)
{
	int			v = rs->nativeCurVec;
	int			V = rs->nativeVectorCount;
	int			c;

	while (v < V && rs->rowInGroup >= rs->nativeVecStart[v + 1])
		v++;
	rs->nativeCurVec = v;

	if (v >= V || !rs->nativeSkipVec[v] ||
		rs->rowInGroup != rs->nativeVecStart[v])
		return false;

	for (c = 0; c < rs->natts; c++)
		if (rs->nativeValueCursor[c] != NULL && rs->nativeVecRawLen[c] != NULL)
			rs->nativeValueCursor[c] += rs->nativeVecRawLen[c][v];

	rs->rowInGroup = rs->nativeVecStart[v + 1];
	rs->nativeCurVec = v + 1;
	rs->vectorsSkipped++;
	return true;
}

/*
 * columnar_native_next_row
 *		Native-format sequential row production (Phase D3). Decodes one row from
 *		the current row group, reconstructing each column from its validity bit
 *		and, when present, the next value on its cursor. Vectors the zone maps rule
 *		out are stepped over without decoding (Phase D5b).
 */
static bool
columnar_native_next_row(ColumnarReadState *rs, Datum *values, bool *nulls,
						 uint64 *rowNumber)
{
	MemoryContext oldContext;
	int			c;

	if (rs->exhausted)
		return false;

	for (;;)
	{
		bool		deleted;

		if (rs->nativeGroup == NULL || rs->rowInGroup >= rs->groupRowCount)
		{
			if (!columnar_native_load_group(rs))
			{
				rs->exhausted = true;
				return false;
			}
		}

		if (rs->nativeSkipVec != NULL &&
			columnar_native_skip_current_vector(rs))
			continue;			/* stepped past a ruled-out vector; re-check */

		/*
		 * Read the row, advancing each present column's value cursor. This happens
		 * even for a deleted row so the cursors stay aligned for the next row; a
		 * deleted row is simply not emitted (D6b).
		 */
		MemoryContextReset(rs->rowContext);
		oldContext = MemoryContextSwitchTo(rs->rowContext);

		for (c = 0; c < rs->natts; c++)
		{
			Form_pg_attribute att = TupleDescAttr(rs->tupdesc, c);
			char	   *vbits = rs->nativeValidity[c];

			/* column absent from this group (added by a later ADD COLUMN) */
			if (vbits == NULL)
			{
				values[c] = rs->missingValues[c];
				nulls[c] = rs->missingIsnull[c];
				continue;
			}

			if ((vbits[rs->rowInGroup >> 3] >> (rs->rowInGroup & 7)) & 1)
			{
				values[c] = ColumnarDecodeValue(att, &rs->nativeValueCursor[c],
												rs->rowContext);
				nulls[c] = false;
			}
			else
			{
				values[c] = (Datum) 0;
				nulls[c] = true;
			}
		}

		MemoryContextSwitchTo(oldContext);

		deleted = (rs->nativeDeleteMask != NULL &&
				   (rs->rowInGroup >> 3) < rs->nativeDeleteMaskLen &&
				   (rs->nativeDeleteMask[rs->rowInGroup >> 3] &
					(1 << (rs->rowInGroup & 7))) != 0);

		*rowNumber = rs->nativeGroup->firstRowNumber + rs->rowInGroup;
		rs->rowInGroup++;

		if (deleted)
			continue;			/* row deleted: cursors advanced, do not emit */

		return true;
	}
}

bool
ColumnarReadNextRow(ColumnarReadState *readState, Datum *values, bool *nulls,
					uint64 *rowNumber)
{
	columnar_read_start(readState);

	if (readState->isNative)
		return columnar_native_next_row(readState, values, nulls, rowNumber);

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

	/*
	 * Native (PGCN v1) fetch-by-row-number (D6a): find the row group covering the
	 * row, reconstruct each column's value at its position. This is what index and
	 * bitmap scans and unique enforcement call, so it unblocks them on native.
	 * (D6b adds the native row-mask delete-visibility check here.)
	 */
	if (ColumnarStorageIsNative(storageId, metaSnapshot))
	{
		List	   *rgList = ColumnarReadRowGroupList(storageId, metaSnapshot);
		NativeRowGroupMetadata *rg = NULL;
		List	   *nchunks;
		NativeColumnChunkMetadata **ccForCol;
		char	   *groupBuffer;
		int			validityBytes;
		uint64		rowInGrp;
		ListCell   *nlc;

		foreach(nlc, rgList)
		{
			NativeRowGroupMetadata *g = (NativeRowGroupMetadata *) lfirst(nlc);

			if (rowNumber >= g->firstRowNumber &&
				rowNumber < g->firstRowNumber + g->rowCount)
			{
				rg = g;
				break;
			}
		}
		if (rg == NULL)
		{
			MemoryContextSwitchTo(oldContext);
			MemoryContextDelete(tmp);
			return false;
		}
		rowInGrp = rowNumber - rg->firstRowNumber;

		/* deleted rows are not visible (D6b): check the group's row mask, plus any
		 * not-yet-flushed buffered delete (so a same-key UPDATE's unique check sees
		 * the old row as gone) */
		{
			List	   *maskList = ColumnarReadRowMaskList(storageId,
													   rg->groupNumber,
													   metaSnapshot);
			ListCell   *mlc;
			bool		deleted = ColumnarRowMaskBufferedDeleted(rel, rowNumber);

			foreach(mlc, maskList)
			{
				RowMaskMetadata *rm = (RowMaskMetadata *) lfirst(mlc);

				if (rm->mask != NULL && (rowInGrp >> 3) < rm->maskLen &&
					(rm->mask[rowInGrp >> 3] & (1 << (rowInGrp & 7))) != 0)
					deleted = true;
			}
			if (deleted)
			{
				MemoryContextSwitchTo(oldContext);
				MemoryContextDelete(tmp);
				return false;
			}
		}

		groupBuffer = palloc(rg->byteLength > 0 ? rg->byteLength : 1);
		if (rg->byteLength > 0)
			ColumnarReadLogicalData(rel, rg->fileOffset, groupBuffer,
									rg->byteLength);
		nchunks = ColumnarReadColumnChunkList(storageId, rg->groupNumber,
											  metaSnapshot);
		validityBytes = (int) ((rg->rowCount + 7) / 8);

		ccForCol = palloc0(sizeof(NativeColumnChunkMetadata *) * natts);
		foreach(nlc, nchunks)
		{
			NativeColumnChunkMetadata *cc = (NativeColumnChunkMetadata *) lfirst(nlc);

			if (cc->columnIndex >= 0 && cc->columnIndex < natts)
				ccForCol[cc->columnIndex] = cc;
		}

		for (c = 0; c < natts; c++)
		{
			Form_pg_attribute att = TupleDescAttr(tupdesc, c);
			NativeColumnChunkMetadata *cc = ccForCol[c];
			char	   *base;
			char	   *vbits;
			char	   *rawBuf;
			char	   *cursor;
			uint64		present;
			uint64		i;

			/* column added after this row group was written: missing value */
			if (cc == NULL)
			{
				values[c] = getmissingattr(tupdesc, c + 1, &nulls[c]);
				continue;
			}

			base = groupBuffer + (cc->pageOffset - rg->fileOffset);
			vbits = base;
			if (((vbits[rowInGrp >> 3] >> (rowInGrp & 7)) & 1) == 0)
			{
				values[c] = (Datum) 0;
				nulls[c] = true;
				continue;
			}

			/* present-index of this row = number of present rows before it */
			present = 0;
			for (i = 0; i < rowInGrp; i++)
				if ((vbits[i >> 3] >> (i & 7)) & 1)
					present++;

			if (cc->encodingDescriptorLen == 1 &&
				(uint8) cc->encodingDescriptor[0] == COLUMNAR_NATIVE_ENCDESC_BASELINE)
				rawBuf = base + validityBytes;
			else
				rawBuf = columnar_native_decode_chunk(tmp, att, base + validityBytes,
													  (uint32) (cc->pageLength - validityBytes),
													  cc->encodingDescriptor,
													  cc->encodingDescriptorLen,
													  cc->blockCodec, NULL, NULL);

			cursor = rawBuf;
			for (i = 0; i < present; i++)
				(void) ColumnarDecodeValue(att, &cursor, tmp);
			values[c] = ColumnarDecodeValue(att, &cursor, target);
			nulls[c] = false;
		}

		MemoryContextSwitchTo(oldContext);
		MemoryContextDelete(tmp);
		return true;
	}

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

	/* deleted rows are not visible (spec 7.5), including a not-yet-flushed
	 * buffered delete so a same-key UPDATE's unique check sees the old row gone */
	if (ColumnarRowMaskBufferedDeleted(rel, rowNumber))
	{
		MemoryContextSwitchTo(oldContext);
		MemoryContextDelete(tmp);
		return false;
	}
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

	/* native format cursors (Phase D3) */
	readState->rowGroupList = NIL;
	readState->rowGroupIndex = 0;
	readState->nativeGroup = NULL;
	readState->nativeBuffer = NULL;
	readState->nativeValidity = NULL;
	readState->nativeValueCursor = NULL;
	readState->nativeSkipVec = NULL;
	readState->nativeVecStart = NULL;
	readState->nativeVecRawLen = NULL;
	readState->nativeVectorCount = 0;
	readState->nativeCurVec = 0;
	readState->nativeDeleteMask = NULL;
	readState->nativeDeleteMaskLen = 0;
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

/*
 * ColumnarVectorsSkipped
 *		How many 1024-value vectors the native scan skipped within read row groups
 *		via per-vector zone maps (native spec 7.1, D5b). Used by EXPLAIN.
 */
uint64
ColumnarVectorsSkipped(ColumnarReadState *readState)
{
	return readState->vectorsSkipped;
}
