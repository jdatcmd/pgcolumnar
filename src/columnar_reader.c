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

	bool		started;
	bool		exhausted;

	ParallelTableScanDesc parallelScan;

	/*
	 * Parallel scan work claiming (gap 23). When non-NULL, this shared atomic
	 * hands out the next row group across all workers; each worker scans the row
	 * groups it claims. NULL for a serial scan, which walks rowGroupIndex.
	 */
	pg_atomic_uint32 *parallelCounter;

	/* current group row count and position, shared by the native producer */
	uint64		groupRowCount;
	uint64		rowInGroup;

	/* chunk-group skip counters over the groups reached so far (spec 9) */
	uint64		groupsRead;
	uint64		groupsSkipped;

	/*
	 * Native format (PGCN v1) read state. The scan reads row groups and column
	 * chunks from the native catalog. The current row group's bytes are read
	 * whole into nativeBuffer (in groupContext); nativeValidity[c] points at each
	 * column chunk's validity bitmap and nativeValueCursor[c] advances through its
	 * uncompressed values.
	 */
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

static void columnar_build_predicates(ColumnarReadState *readState,
									  int nkeys, ScanKey keys);
static int64 columnar_next_group_index(ColumnarReadState *readState);

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
	readState->started = false;
	readState->exhausted = false;
	readState->parallelScan = parallelScan;
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

/*
 * columnar_setup_group
 *		Position on a chunk group: decompress each projected column's value
 *		stream into the group context and point the column cursors at the
 *		decompressed bytes and the (uncompressed) exists bytes. Non-projected
 *		columns are left un-decoded (column projection, spec 9).
 */

/*
 * columnar_position_group
 *		Advance from the current groupIndex to the next chunk group that could
 *		match the pushed-down predicates, skipping groups whose min/max rule
 *		them out (spec 9). Returns true when positioned on a readable group,
 *		false when the stripe has no more matching groups.
 */

/*
 * columnar_load_stripe
 *		Read a stripe's metadata and data into memory and position at its
 *		first chunk group.
 */

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

		readState->rowGroupList =
			ColumnarReadRowGroupList(readState->storageId,
									 readState->metaSnapshot);
		readState->rowGroupIndex = 0;
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

	/*
	 * Claim the next row group and advance past any the zone maps rule out
	 * (native spec 7.1). Under a parallel custom scan each worker claims distinct
	 * groups from the shared counter (columnar_next_group_index), so a group is
	 * read by exactly one backend; serially it walks rowGroupIndex. Without the
	 * counter every worker read every group and a parallel scan returned each row
	 * once per participating backend (D6e).
	 */
	rg = NULL;
	for (;;)
	{
		int64		gi = columnar_next_group_index(rs);
		bool		match = true;

		if (gi < 0)
			return false;

		rg = (NativeRowGroupMetadata *) list_nth(rs->rowGroupList, (int) gi);
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
	}

	rs->groupsRead++;

	MemoryContextReset(rs->groupContext);
	oldContext = MemoryContextSwitchTo(rs->groupContext);
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
	return columnar_native_next_row(readState, values, nulls, rowNumber);
}

/*
 * columnar_next_group_index
 *		The next native row group to scan, or -1 when none remain. The native
 *		counterpart of columnar_next_stripe_index: a parallel custom scan claims
 *		it from the shared atomic so each worker reads distinct row groups (gap
 *		23, D6e); a serial scan walks rowGroupIndex.
 */
static int64
columnar_next_group_index(ColumnarReadState *readState)
{
	int			ngroups = list_length(readState->rowGroupList);
	uint32		gi;

	if (readState->parallelCounter != NULL)
		gi = pg_atomic_fetch_add_u32(readState->parallelCounter, 1);
	else
		gi = (uint32) readState->rowGroupIndex++;

	return (gi < (uint32) ngroups) ? (int64) gi : -1;
}

void
ColumnarReadSetParallelCounter(ColumnarReadState *readState,
							   pg_atomic_uint32 *counter)
{
	readState->parallelCounter = counter;
}

/* -------------------------------------------------------------------------
 * Liveness cache (gap 26, phase 4): a projection scan must test each row's base
 * row number for deletion/visibility. The cache reads the base row-group list
 * and row masks once (at the scan's snapshot) into memory, then answers each
 * test with a binary search over row groups plus a bitmap probe. Consistent
 * with the scan's fixed snapshot, the same way ColumnarBeginRead reads those
 * lists once at begin.
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
	List	   *rgList = ColumnarReadRowGroupList(storageId, metaSnapshot);
	ColumnarLivenessCache *cache = palloc0(sizeof(ColumnarLivenessCache));
	ListCell   *lc;
	int			i = 0;

	/*
	 * Each native row group is one liveness entry with a single whole-group
	 * delete mask (the row mask is keyed by group number, chunk id 0). Modeling
	 * it as chunkGroupCount 1 with chunkRowCount == rowCount makes the shared
	 * ColumnarLivenessCacheIsLive map every row to chunk 0.
	 */
	cache->ctx = ctx;
	cache->nstripes = list_length(rgList);
	cache->stripes = palloc0(sizeof(LiveStripeEntry) * Max(cache->nstripes, 1));

	foreach(lc, rgList)
	{
		NativeRowGroupMetadata *rg = (NativeRowGroupMetadata *) lfirst(lc);
		LiveStripeEntry *e = &cache->stripes[i++];
		List	   *rml;
		ListCell   *mc;
		uint32		want = (uint32) ((rg->rowCount + 7) / 8);

		e->firstRowNumber = rg->firstRowNumber;
		e->rowCount = rg->rowCount;
		e->chunkRowCount = (int) rg->rowCount;
		e->chunkGroupCount = 1;
		e->masks = palloc0(sizeof(char *) * 1);
		e->maskLens = palloc0(sizeof(uint32) * 1);

		rml = ColumnarReadRowMaskList(storageId, rg->groupNumber, metaSnapshot);
		foreach(mc, rml)
		{
			RowMaskMetadata *rm = (RowMaskMetadata *) lfirst(mc);
			uint32		b;

			if (rm->mask == NULL || rm->maskLen == 0)
				continue;
			if (e->masks[0] == NULL)
			{
				e->masks[0] = palloc0(want > 0 ? want : 1);
				e->maskLens[0] = want;
			}
			for (b = 0; b < rm->maskLen && b < want; b++)
				e->masks[0][b] |= rm->mask[b];
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
	List	   *rgList;
	NativeRowGroupMetadata *rg = NULL;
	List	   *nchunks;
	NativeColumnChunkMetadata **ccForCol;
	char	   *groupBuffer;
	int			validityBytes;
	uint64		rowInGrp;
	ListCell   *nlc;
	int			c;

	tmp = AllocSetContextCreate(CurrentMemoryContext, "columnar fetch",
								ALLOCSET_SMALL_SIZES);
	oldContext = MemoryContextSwitchTo(tmp);

	metaSnapshot = ColumnarCatalogSnapshot(snapshot);

	/*
	 * Native (PGCN v1) fetch-by-row-number: find the row group covering the row
	 * and reconstruct each column's value at its position. Index and bitmap scans
	 * and unique enforcement call this. A deleted row (in the group's row mask or
	 * a not-yet-flushed buffered delete) is not visible.
	 */
	rgList = ColumnarReadRowGroupList(storageId, metaSnapshot);
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

void
ColumnarRescanRead(ColumnarReadState *readState)
{
	MemoryContextReset(readState->stripeContext);
	readState->started = false;
	readState->exhausted = false;

	/* native format cursors */
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
