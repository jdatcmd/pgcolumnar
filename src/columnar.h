/*-------------------------------------------------------------------------
 *
 * columnar.h
 *		Shared declarations for the pgColumnar table access method.
 *
 * pgColumnar is an independent MIT implementation built solely from
 * design/FORMAT_AND_INTERFACE_SPEC.md (format 2.0) and the public PostgreSQL
 * API. It reads and writes the format described in that specification.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PGCOLUMNAR_H
#define PGCOLUMNAR_H

#include "postgres.h"

#include "columnar_compat.h"

#include "access/skey.h"
#include "access/tableam.h"
#include "port/atomics.h"
#include "lib/stringinfo.h"
#include "nodes/bitmapset.h"
#include "nodes/pg_list.h"
#include "nodes/extensible.h"
#include "storage/bufpage.h"
#include "utils/rel.h"
#include "utils/snapshot.h"

/* format version (spec 3). Minor 1 adds per-chunk value encodings (I1); minor 2
 * adds multiple projections (gap 26; the columnar.projection catalog, no
 * metapage layout change). 2.0/2.1 files still read: only the major version is
 * validated, a chunk with no encoding type is treated as NONE, and a table with
 * no columnar.projection rows has a single implicit base projection. */
#define COLUMNAR_VERSION_MAJOR 2
#define COLUMNAR_VERSION_MINOR 2

/*
 * Native format (re-origination line). A separate format line from the 2.2 line
 * above, designed from public research and the open Arrow/Parquet/ORC specs; see
 * design/NATIVE_FORMAT_AND_INTERFACE_SPEC.md. It is identified by its own
 * metapage magic and major version and has no requirement to read the 2.2 line.
 * These are the Phase D1 scaffolding constants; the native metapage layout,
 * writer, and reader land in D2/D3, and the cascade encoding and zone maps in
 * D4/D5. Nothing writes the native format yet, so these are unused so far.
 */
#define COLUMNAR_NATIVE_MAGIC "PGCN"		/* native metapage magic tag */
#define COLUMNAR_NATIVE_VERSION_MAJOR 1
#define COLUMNAR_NATIVE_VECTOR_LENGTH 1024	/* values per vector (fixed) */

/*
 * Native column-chunk encoding descriptor (D4). A column chunk's
 * pgcolumnar.column_chunk.encoding_descriptor is either the D2b baseline (a
 * single 0 byte: raw present values, no per-vector encoding, block_codec 0) or a
 * D4 descriptor with this leading version byte, recording the lightweight
 * encoding chosen per 1024-value vector so the reader reconstructs the exact raw
 * value stream. The layout is: uint8 version, uint8 reserved, uint32 vectorCount,
 * then per vector { uint8 encodingType, uint32 valueCount, uint32 rawLen,
 * uint32 encLen }. Integers are host-endian (little-endian hosts assumed, spec 3).
 */
#define COLUMNAR_NATIVE_ENCDESC_BASELINE 0
#define COLUMNAR_NATIVE_ENCDESC_VERSION 1
#define COLUMNAR_NATIVE_ENCDESC_HEADER_LEN 6	/* version + reserved + vectorCount */
#define COLUMNAR_NATIVE_ENCDESC_ENTRY_LEN 13	/* encodingType + 3 * uint32 */

/* first row number is 1 (spec 3) */
#define COLUMNAR_FIRST_ROW_NUMBER 1

/*
 * Logical/physical mapping constants (spec 2, 2.1).
 *
 * BYTES_PER_PAGE is the number of logical data bytes carried by one physical
 * page. The first logical byte lives at the start of block 2.
 */
#define COLUMNAR_BYTES_PER_PAGE ((uint64) (BLCKSZ - SizeOfPageHeaderData))
#define COLUMNAR_FIRST_LOGICAL_OFFSET (2 * COLUMNAR_BYTES_PER_PAGE)

/*
 * Row-number <-> item-pointer mapping (spec 6).
 *
 * VALID_ITEMPOINTER_OFFSETS is the count of item-pointer offsets we use per
 * synthetic block. The spec only requires it to be bounded by MaxOffsetNumber
 * and to yield a reversible mapping; we use MaxHeapTuplesPerPage, which is
 * comfortably below MaxOffsetNumber. These TIDs are synthetic row addresses;
 * they are never used to locate bytes in the data file. (Implementation
 * choice, noted per PROVENANCE clean-room rules.)
 */
#define COLUMNAR_VALID_ITEMPOINTER_OFFSETS ((uint64) MaxHeapTuplesPerPage)

/* compression codes (spec 5) */
#define COLUMNAR_COMPRESSION_NONE 0
#define COLUMNAR_COMPRESSION_PGLZ 1
#define COLUMNAR_COMPRESSION_LZ4 2
#define COLUMNAR_COMPRESSION_ZSTD 3

/*
 * Value-stream encoding codes (I1, format 2.1). An encoding is a reversible
 * transform of the raw value-stream bytes, applied before block compression on
 * write and reversed after decompression on read (columnar_encoding.c).
 */
#define COLUMNAR_ENCODING_NONE 0
#define COLUMNAR_ENCODING_RLE 1		/* run-length of a fixed-width value */
#define COLUMNAR_ENCODING_FOR 2		/* frame-of-reference + bit-packing */
#define COLUMNAR_ENCODING_DELTA 3	/* delta + zigzag + bit-packing */
#define COLUMNAR_ENCODING_GORILLA 4 /* Gorilla XOR for float4/float8 */
#define COLUMNAR_ENCODING_DOD 5		/* delta-of-delta + zigzag + bit-packing */
#define COLUMNAR_ENCODING_DICT 6	/* dictionary of distinct values + codes */

/* schema that holds the metadata catalog */
#define COLUMNAR_SCHEMA_NAME "pgcolumnar"

/* -------------------------------------------------------------------------
 * Per-table options (spec 7.4). A "set" flag distinguishes an explicitly
 * stored per-table value from the instance-wide GUC default.
 * ------------------------------------------------------------------------- */
typedef struct ColumnarOptions
{
	bool		chunkGroupRowLimitSet;
	int			chunkGroupRowLimit;
	bool		stripeRowLimitSet;
	int			stripeRowLimit;
	bool		compressionSet;
	int			compressionType;	/* one of COLUMNAR_COMPRESSION_* */
	bool		compressionLevelSet;
	int			compressionLevel;
	bool		formatVersionSet;
	int			formatVersion;		/* native format major version, 1 = PGCN v1 */
} ColumnarOptions;

/* GUC-backed instance defaults (spec 8.3) */
extern int columnar_stripe_row_limit;
extern int columnar_chunk_group_row_limit;
extern int columnar_compression;		/* one of COLUMNAR_COMPRESSION_* */
extern int columnar_compression_level;	/* zstd level */
extern bool columnar_enable_qual_pushdown;
extern bool columnar_enable_custom_scan;
extern bool columnar_enable_bloom_filter;	/* bloom equality skipping (I7) */
extern bool columnar_enable_late_materialization;	/* decode outputs after filter (I8) */

/* Phase 6 GUCs (spec 8.3) */
extern bool columnar_enable_vectorization;	/* vectorized scan/aggregate path */
extern bool columnar_enable_compressed_execution;	/* run-based aggregate path (I3) */
extern bool columnar_enable_metadata_count;	/* count(*) from catalog metadata (gap 28) */
extern bool columnar_enable_column_cache;	/* decompressed-chunk cache */
extern bool columnar_enable_read_stream;	/* stream/prefetch block reads (PG17+) */
extern bool columnar_enable_index_only_scan;	/* allow index-only scans (gap 28) */
extern bool columnar_enable_projection_scan;	/* scan a covering projection (gap 26) */
extern int columnar_column_cache_size;		/* cache budget in megabytes */

/* issue #5: concurrent unique-key insert serialization */
extern bool columnar_enable_unique_lock;	/* serialize same-key inserters */
extern int columnar_unique_lock_buckets;	/* advisory-lock buckets per index */

/* -------------------------------------------------------------------------
 * Metapage (spec 3)
 * ------------------------------------------------------------------------- */
typedef struct ColumnarMetapage
{
	uint32		versionMajor;
	uint32		versionMinor;
	uint64		storageId;
	uint64		reservedStripeId;
	uint64		reservedRowNumber;
	uint64		reservedOffset;
	bool		unloggedReset;
} ColumnarMetapage;

/* -------------------------------------------------------------------------
 * Catalog row shapes, as C structs (spec 7)
 * ------------------------------------------------------------------------- */
typedef struct StripeMetadata
{
	uint64		storageId;
	uint64		stripeNum;
	uint64		fileOffset;
	uint64		dataLength;
	int			columnCount;
	int			chunkRowCount;		/* chunk_group_row_limit at write time */
	uint64		rowCount;
	int			chunkGroupCount;
	uint64		firstRowNumber;
} StripeMetadata;

typedef struct ChunkGroupMetadata
{
	uint64		stripeNum;
	int			chunkGroupNum;
	uint64		rowCount;
	uint64		deletedRows;
} ChunkGroupMetadata;

/* -------------------------------------------------------------------------
 * Native format catalog row shapes (re-origination line, PGCN v1). These map
 * to pgcolumnar.storage / row_group / column_chunk (native spec section 11).
 * ------------------------------------------------------------------------- */
typedef struct NativeStorageMetadata
{
	uint64		storageId;
	Oid			relationOid;
	int			formatVersion;
	int			vectorLength;
	int			rowGroupLimit;
} NativeStorageMetadata;

typedef struct NativeRowGroupMetadata
{
	uint64		storageId;
	uint64		groupNumber;
	uint64		fileOffset;
	uint64		rowCount;
	uint64		byteLength;
	uint64		firstRowNumber;
} NativeRowGroupMetadata;

typedef struct NativeColumnChunkMetadata
{
	uint64		storageId;
	uint64		groupNumber;
	int			columnIndex;
	uint64		valueCount;
	const char *encodingDescriptor;	/* bytea payload */
	uint32		encodingDescriptorLen;
	int			blockCodec;			/* 0 = none */
	uint64		pageOffset;
	uint64		pageLength;
} NativeColumnChunkMetadata;

/*
 * One pgcolumnar.zone_map row (native spec 7.1, Phase D5): a Small Materialized
 * Aggregate for one vector of a column chunk (vectorIndex 0-based) or for the
 * whole column chunk (vectorIndex -1). minimum and maximum are the column's
 * value serialized with ColumnarEncodeValue (NULL when the type has no btree
 * ordering); sum is a numeric Datum (D5a leaves it unset, hasSum false; the
 * zone-map-only aggregate that consumes it lands in D5b). value_count and
 * null_count are always present.
 */
typedef struct NativeZoneMapMetadata
{
	uint64		storageId;
	uint64		groupNumber;
	int			columnIndex;
	int			vectorIndex;		/* 0-based vector; -1 for the whole chunk */
	bool		hasMinMax;
	const char *minimum;			/* ColumnarEncodeValue bytes, when hasMinMax */
	uint32		minimumLen;
	const char *maximum;
	uint32		maximumLen;
	bool		hasSum;				/* D5a: false; sum computed in D5b */
	Datum		sum;				/* numeric Datum when hasSum */
	uint64		valueCount;
	uint64		nullCount;
} NativeZoneMapMetadata;

/*
 * One pgcolumnar.bloom row (native spec 7.2, Phase D5b): a per-column-chunk bloom
 * filter over the chunk's hashable values, for equality skipping on unsorted
 * columns. filter is the ColumnarBloomBuild byte image.
 */
typedef struct NativeBloomMetadata
{
	uint64		storageId;
	uint64		groupNumber;
	int			columnIndex;
	const char *filter;
	uint32		filterLen;
} NativeBloomMetadata;

/*
 * One columnar.row_mask row (spec 7.5). Covers the row-number range
 * [startRowNumber, endRowNumber] of a single chunk group; a set bit in mask
 * marks a deleted row. Bit i (0-based) corresponds to row number
 * startRowNumber + i and is stored LSB-first in byte i/8.
 */
typedef struct RowMaskMetadata
{
	uint64		id;
	uint64		stripeId;
	int			chunkId;
	uint64		startRowNumber;
	uint64		endRowNumber;
	int			deletedRows;
	char	   *mask;			/* maskLen bytes, in the caller's context */
	uint32		maskLen;
} RowMaskMetadata;

/*
 * One columnar.projection row (gap 26, format 2.2). A projection is a named,
 * ordered column subset stored as its own columnar storage (projStorageId),
 * sorted on sortKey, sharing the table's row-number identity space.
 * projectionId 0 is the implicit base projection. attnums are 1-based; sortKey
 * attnums are a subset of columns.
 */
typedef struct ColumnarProjection
{
	uint64		storageId;			/* the table's base storage id */
	int			projectionId;		/* 0 = base, 1..N additional */
	char	   *name;				/* projection name (caller's context) */
	uint64		projStorageId;		/* this projection's own storage id */
	int16	   *sortKey;			/* attnums in sort order, sortKeyLen entries */
	int			sortKeyLen;
	int16	   *columns;			/* stored attnums, columnsLen entries */
	int			columnsLen;
} ColumnarProjection;

typedef struct ChunkMetadata
{
	uint64		stripeNum;
	int			attrNum;			/* 1-based attribute number */
	int			chunkGroupNum;
	uint64		valueStreamOffset;	/* relative to stripe file_offset */
	uint64		valueStreamLength;
	uint64		existsStreamOffset;
	uint64		existsStreamLength;
	int			valueCompressionType;
	int			valueCompressionLevel;
	uint64		valueDecompressedLength;	/* length after decompression (= encoded
										 * stream length; for NONE encoding this
										 * equals the raw length) */
	uint64		valueCount;

	/*
	 * Value-stream encoding (I1, format 2.1). valueEncodingType is one of
	 * COLUMNAR_ENCODING_*; valueRawLength is the length of the fully decoded raw
	 * value stream (what decode reconstructs). For format 2.0 chunks the catalog
	 * columns are absent/NULL and the reader defaults to NONE with
	 * valueRawLength == valueDecompressedLength.
	 */
	int			valueEncodingType;
	uint64		valueRawLength;

	/*
	 * Per-chunk min/max skip list (spec 7.2). Stored only for orderable types;
	 * minMaxValid is false when the column type is not orderable or the chunk
	 * has no non-null values, in which case the catalog columns are SQL NULL.
	 * The bytes are the single min/max value encoded with the value-stream
	 * codec (ColumnarEncodeValue), so the reader can decode them back to
	 * Datums for chunk-group skipping.
	 */
	bool		minMaxValid;
	char	   *minEncoded;
	uint32		minEncodedLen;
	char	   *maxEncoded;
	uint32		maxEncodedLen;

	/*
	 * Optional per-chunk bloom filter over the column's non-null values (I7),
	 * for equality chunk-group skipping. NULL when absent (older chunks, or a
	 * collatable/non-hashable column, or a chunk too small to be worth it).
	 */
	char	   *bloomFilter;
	uint32		bloomLen;
} ChunkMetadata;

/* -------------------------------------------------------------------------
 * storage layer (columnar_storage.c)
 * ------------------------------------------------------------------------- */
struct SMgrRelationData;

extern void ColumnarWriteNewMetapage(const RelFileLocator *newrlocator,
									 struct SMgrRelationData *srel,
									 char persistence, uint64 storageId);
extern void ColumnarReadMetapage(Relation rel, ColumnarMetapage *meta);
extern uint64 ColumnarStorageId(Relation rel);
extern void ColumnarReserveRowNumbers(Relation rel, uint64 rowCount,
									  uint64 *stripeId, uint64 *firstRowNumber);
extern void ColumnarReserveOffset(Relation rel, uint64 dataLength,
								  uint64 *fileOffset);
extern void ColumnarWriteLogicalData(Relation rel, uint64 logicalOffset,
									 char *data, uint64 length);
extern void ColumnarReadLogicalData(Relation rel, uint64 logicalOffset,
									char *dest, uint64 length);
extern void ColumnarResetMetapage(Relation rel);

/* row number <-> item pointer (spec 6) */
extern void ColumnarRowNumberToItemPointer(uint64 rowNumber, ItemPointer tid);
extern uint64 ColumnarItemPointerToRowNumber(ItemPointer tid);

/* -------------------------------------------------------------------------
 * visibility map for index-only scans (columnar_visibilitymap.c, gap 28)
 * ------------------------------------------------------------------------- */
extern void ColumnarVMSetVisible(Relation rel, BlockNumber blk);
extern void ColumnarVMClearVisible(Relation rel, BlockNumber blk);
extern void ColumnarVMClearForRow(Relation rel, uint64 rowNumber);
extern bool ColumnarVMIsVisible(Relation rel, BlockNumber blk);
extern void ColumnarVMSetVisibleForRelation(Relation rel);

/* a contiguous run of all-visible row numbers (gap 28 phase 3) */
typedef struct ColumnarRowRange
{
	uint64		firstRowNumber;
	uint64		rowCount;
}			ColumnarRowRange;

/* all-visible chunk-group row ranges: stripe committed past the horizon and no
 * deletes (committed or in-progress). Returns a List of ColumnarRowRange *. */
extern List *ColumnarComputeAllVisibleGroups(uint64 storageId,
											 TransactionId oldestXmin);

/* -------------------------------------------------------------------------
 * metadata layer (columnar_metadata.c)
 * ------------------------------------------------------------------------- */
extern uint64 ColumnarNextStorageId(void);
extern void ColumnarInsertStripeRow(const StripeMetadata *stripe);
extern void ColumnarInsertChunkGroupRow(uint64 storageId,
										const ChunkGroupMetadata *cg);
extern void ColumnarInsertChunkRow(uint64 storageId, const ChunkMetadata *chunk);
extern void ColumnarInsertNativeStorageRow(const NativeStorageMetadata *s);
extern bool ColumnarStorageIsNative(uint64 storageId, Snapshot snapshot);
extern void ColumnarInsertRowGroupRow(const NativeRowGroupMetadata *rg);
extern void ColumnarInsertColumnChunkRow(const NativeColumnChunkMetadata *cc);
extern void ColumnarInsertZoneMapRow(const NativeZoneMapMetadata *z);
extern void ColumnarInsertBloomRow(const NativeBloomMetadata *b);
extern List *ColumnarReadRowGroupList(uint64 storageId, Snapshot snapshot);
extern List *ColumnarReadColumnChunkList(uint64 storageId, uint64 groupNumber,
										 Snapshot snapshot);
extern List *ColumnarReadZoneMapList(uint64 storageId, uint64 groupNumber,
									 Snapshot snapshot);
extern List *ColumnarReadZoneMapVectors(uint64 storageId, uint64 groupNumber,
										Snapshot snapshot);
extern List *ColumnarReadBloomList(uint64 storageId, uint64 groupNumber,
								   Snapshot snapshot);
extern List *ColumnarReadStripeList(uint64 storageId, Snapshot snapshot);
extern List *ColumnarReadChunkGroupList(uint64 storageId, uint64 stripeNum,
										Snapshot snapshot);
extern List *ColumnarReadChunkList(uint64 storageId, uint64 stripeNum,
								   Snapshot snapshot);
extern void ColumnarDeleteMetadata(uint64 storageId);

/* per-table options catalog (spec 7.4) */
extern bool ColumnarReadOptions(Oid relid, ColumnarOptions *opts);
extern int	ColumnarTableFormatVersion(Oid relid);
extern void ColumnarDeleteOptions(Oid relid);

/* projection catalog (gap 26, format 2.2). List entries are ColumnarProjection*
 * palloc'd in the current context, ordered by projection_id. */
extern List *ColumnarListProjections(uint64 storageId);
extern void ColumnarInsertProjectionRow(const ColumnarProjection *proj);
extern void ColumnarDeleteProjectionRow(uint64 storageId, int projectionId);

/* whether a relation uses the columnar table access method */
extern bool ColumnarIsColumnarRelation(Oid relid);

/*
 * A snapshot suitable for reading the columnar metadata catalog during a scan
 * or a DML operation. It is the given base snapshot with its command id
 * advanced so that catalog rows written earlier in this same transaction (even
 * in the current command, e.g. flushed at scan start) are visible, giving
 * same-transaction read-your-writes while preserving isolation from other
 * transactions (spec 9). Returns the base snapshot unchanged when it is not an
 * MVCC snapshot. The result is palloc'd in the current context and shares the
 * base snapshot's arrays, so the base must outlive it.
 */
extern Snapshot ColumnarCatalogSnapshot(Snapshot base);

/* row_mask catalog access (spec 7.5) */
extern List *ColumnarReadRowMaskList(uint64 storageId, uint64 stripeId,
									 Snapshot snapshot);
extern bool ColumnarStorageHasRowMask(uint64 storageId, Snapshot snapshot);
extern void ColumnarUpsertRowMask(uint64 storageId, RowMaskMetadata *rm);
extern uint64 ColumnarNextRowMaskId(void);

/* -------------------------------------------------------------------------
 * writer (columnar_write_state.c)
 * ------------------------------------------------------------------------- */
typedef struct ColumnarWriteState ColumnarWriteState;

extern ColumnarWriteState *ColumnarGetWriteState(Relation rel);
extern uint64 ColumnarWriteRow(ColumnarWriteState *writeState, Relation rel,
							   Datum *values, bool *nulls);
extern void ColumnarProjectionFanoutRow(Relation rel, ColumnarWriteState *baseWs,
										uint64 rowNumber, Datum *values,
										bool *nulls);
extern void ColumnarBackfillProjection(Relation rel,
									   const ColumnarProjection *proj);
extern bool ColumnarBufferedRowByNumber(Relation rel, uint64 rowNumber,
										Datum *values, bool *nulls);
extern void ColumnarFlushWriteStateForRelation(Oid relid);
extern void ColumnarForgetWriteStateForRelation(Oid relid);
extern void ColumnarFlushAllPendingWrites(void);
extern void ColumnarDiscardAllPendingWrites(void);
extern void ColumnarWriteStateDiscardSubXact(SubTransactionId subid);
extern void ColumnarWriteStatePromoteSubXact(SubTransactionId subid,
											 SubTransactionId parent);

/* -------------------------------------------------------------------------
 * row mask / delete tracking (columnar_row_mask.c, spec 7.5, 9)
 * ------------------------------------------------------------------------- */
extern void ColumnarMarkRowDeleted(Relation rel, uint64 rowNumber);
extern bool ColumnarRowMaskBufferedDeleted(Relation rel, uint64 rowNumber);
extern void ColumnarFlushRowMaskForRelation(Relation rel);
extern void ColumnarFlushAllRowMasks(void);
extern void ColumnarDiscardAllRowMasks(void);
extern void ColumnarRowMaskDiscardSubXact(SubTransactionId subid);
extern void ColumnarRowMaskPromoteSubXact(SubTransactionId subid,
										  SubTransactionId parent);

/* -------------------------------------------------------------------------
 * reader (columnar_reader.c)
 * ------------------------------------------------------------------------- */
typedef struct ColumnarReadState ColumnarReadState;

extern ColumnarReadState *ColumnarBeginRead(Relation rel, Snapshot snapshot,
											ParallelTableScanDesc parallelScan,
											Bitmapset *projectedColumns,
											int nkeys, ScanKey keys);
/* like ColumnarBeginRead but reads an explicit storage id with an explicit
 * tuple descriptor -- used to read a projection's storage (gap 26) */
extern ColumnarReadState *ColumnarBeginReadWithStorage(Relation rel,
													   Snapshot snapshot,
													   uint64 storageId,
													   TupleDesc tupdesc,
													   ParallelTableScanDesc parallelScan,
													   Bitmapset *projectedColumns,
													   int nkeys, ScanKey keys);
extern bool ColumnarReadNextRow(ColumnarReadState *readState,
								Datum *values, bool *nulls,
								uint64 *rowNumber);
extern void ColumnarRescanRead(ColumnarReadState *readState);
extern void ColumnarEndRead(ColumnarReadState *readState);

/*
 * Parallel scan (gap 23): point the read state at a shared atomic that hands out
 * stripe indices, so several workers scanning the same relation each claim
 * distinct stripes. Set by the custom scan's DSM init callbacks.
 */
extern void ColumnarReadSetParallelCounter(ColumnarReadState *readState,
										   pg_atomic_uint32 *counter);

/*
 * Chunk-group skip counters for the current scan (spec 9), used by the custom
 * scan's EXPLAIN output to show how many chunk groups the min/max skip lists
 * removed. total = read + skipped over the groups the scan has reached.
 */
extern void ColumnarReadStats(ColumnarReadState *readState,
							  uint64 *groupsRead, uint64 *groupsSkipped,
							  uint64 *groupsTotal);
extern uint64 ColumnarVectorsSkipped(ColumnarReadState *readState);

/*
 * Fetch a single row by its 1-based row number (spec 6), for the table AM's
 * fetch-by-tid callback used by UPDATE. Fills values/nulls (by-reference values
 * are allocated in the current memory context) and returns true when the row
 * exists and is not marked deleted in the row mask.
 */
extern bool ColumnarRowIsLive(Relation rel, Snapshot snapshot,
							  uint64 rowNumber);
/* cached base-liveness for a projection scan (gap 26): build once per scan,
 * probe per row with a binary search instead of a per-row catalog scan */
typedef struct ColumnarLivenessCache ColumnarLivenessCache;
extern ColumnarLivenessCache *ColumnarBuildLivenessCache(Relation rel,
														 Snapshot snapshot);
extern bool ColumnarLivenessCacheIsLive(ColumnarLivenessCache *cache,
										uint64 rowNumber);
extern void ColumnarFreeLivenessCache(ColumnarLivenessCache *cache);
extern bool ColumnarReadRowByNumber(Relation rel, Snapshot snapshot,
									uint64 rowNumber, Datum *values, bool *nulls);

/* -------------------------------------------------------------------------
 * vectorized batch reader (columnar_reader.c, spec 9 vectorized execution)
 *
 * A ColumnarVector is one decoded chunk group: for each projected column, the
 * whole group's values and null flags as flat arrays, plus the per-row deleted
 * flag resolved from the row mask. ColumnarReadNextVector advances to the next
 * chunk group that survives min/max skipping (spec 9) and decodes it. The arrays
 * live in the read state's per-group context and stay valid only until the next
 * ColumnarReadNextVector call. A vectorized scan or aggregate applies its own
 * filter and row-mask handling on top of this, so it returns exactly what the
 * scalar per-row reader (ColumnarReadNextRow) would.
 * ------------------------------------------------------------------------- */
typedef struct ColumnarVector
{
	uint64		nrows;			/* rows in this chunk group */
	uint64		firstRowNumber; /* row number of local row 0 */
	Datum	  **values;			/* [natts]; values[c] is Datum[nrows] or NULL */
	bool	  **isnull;			/* [natts]; isnull[c] is bool[nrows] or NULL */
	bool	   *deleted;		/* [nrows]; true when row-mask-deleted */
} ColumnarVector;

extern bool ColumnarReadNextVector(ColumnarReadState *readState,
								   ColumnarVector *vec);

/*
 * Late materialization (I8): position on the next readable chunk group without
 * decoding, then decode a chosen subset of columns into the vector. A scan
 * decodes only the predicate columns, builds the selection vector, and decodes
 * the remaining output columns only when the group has surviving rows. Pass
 * cols = NULL to decode every projected column; init = true on the first decode
 * of a group (allocates the vector and resolves the deleted flags).
 */
extern bool ColumnarAdvanceGroup(ColumnarReadState *readState);
extern void ColumnarDecodeGroupColumns(ColumnarReadState *readState,
									   ColumnarVector *vec,
									   Bitmapset *cols, bool init,
									   const bool *sel);

/* -------------------------------------------------------------------------
 * raw-group reader (columnar_reader.c, I3 compressed execution)
 *
 * Like ColumnarReadNextVector but hands back each projected column's raw value
 * stream (packed non-null values) instead of a decoded Datum array, so an
 * aggregate can run over runs (ColumnarBlockReader) without materializing
 * Datums. Only the non-null values are present in valueCursor; deletedCount
 * reports how many of the group's rows are row-mask-deleted. When deletedCount
 * is non-zero the caller falls back to ColumnarDecodeCurrentGroupVector, which
 * decodes the currently positioned group into a full vector (with per-row null
 * and deleted flags). The arrays live in the read state's per-group context and
 * are valid only until the next raw-group / vector call.
 * ------------------------------------------------------------------------- */
typedef struct ColumnarRawGroup
{
	uint64		nrows;			/* rows in this chunk group */
	uint64		firstRowNumber;
	uint64		deletedCount;	/* rows in this group marked deleted */
	int			natts;
	char	  **valueCursor;	/* [natts]; raw value stream, or NULL */
	uint64	   *groupValueCount;	/* [natts]; non-null values per column */
	bool	   *columnAbsent;	/* [natts]; true if column predates the stripe */
} ColumnarRawGroup;

extern bool ColumnarReadNextRawGroup(ColumnarReadState *readState,
									 ColumnarRawGroup *rg);
extern void ColumnarDecodeCurrentGroupVector(ColumnarReadState *readState,
											 ColumnarVector *vec);

/* value stream encode/decode shared by writer and reader */
extern void ColumnarEncodeValue(StringInfo buf, Form_pg_attribute att,
								Datum value);
extern Datum ColumnarDecodeValue(Form_pg_attribute att, char **cursor,
								 MemoryContext targetContext);

/* -------------------------------------------------------------------------
 * lightweight value-stream encodings (columnar_encoding.c, I1)
 * ------------------------------------------------------------------------- */
extern int ColumnarEncodeChunk(const char *raw, uint32 rawLen,
							   Form_pg_attribute att, uint64 valueCount,
							   char **out, uint32 *outLen);
extern char *ColumnarDecodeChunk(const char *enc, uint32 encLen,
								 int encodingType, Form_pg_attribute att,
								 uint64 valueCount, uint32 rawLen,
								 MemoryContext cx);
extern const char *ColumnarEncodingName(int encodingType);

/* -------------------------------------------------------------------------
 * per-chunk bloom filters (columnar_bloom.c, I7)
 * ------------------------------------------------------------------------- */
extern bool ColumnarBloomBuild(const uint32 *hashes, uint32 n,
							   char **out, uint32 *outLen);
extern bool ColumnarBloomProbe(const char *bloom, uint32 bloomLen, uint32 hash);

/*
 * True when a column of the given collation can carry a bloom filter (I7/gap 25):
 * a non-collatable type (InvalidOid), or a deterministic collation, so equal
 * values are byte-identical and hash consistently between build and probe.
 * Nondeterministic collations return false and are left unbloomed.
 */
extern bool ColumnarCollationIsDeterministic(Oid collid);

/* -------------------------------------------------------------------------
 * compression-block run iterator (columnar_encoding.c, I2)
 *
 * Exposes a column chunk's (non-null) values as a sequence of (value, run
 * length) pairs so operators run once per run instead of once per row (I3
 * compressed execution). It iterates the decoded raw value stream and coalesces
 * adjacent equal fixed-width values, so a repetitive or run-length-encoded
 * column yields long runs. Fixed-width columns only.
 * ------------------------------------------------------------------------- */
typedef struct ColumnarBlockReader
{
	const char *raw;			/* raw value stream (packed fixed-width values) */
	uint64		valueCount;		/* number of values in the stream */
	int			width;			/* bytes per value (attlen) */
	uint64		pos;			/* next value index */
} ColumnarBlockReader;

extern void ColumnarBlockReaderInit(ColumnarBlockReader *br, const char *raw,
									uint64 valueCount, int width);

/*
 * Yield the next run: *valBytes points at the run's value (width bytes, valid
 * while the underlying stream is), *runLen is how many consecutive values equal
 * it. Returns false at end of stream.
 */
extern bool ColumnarBlockNextRun(ColumnarBlockReader *br,
								 const char **valBytes, uint64 *runLen);

/* -------------------------------------------------------------------------
 * compression (columnar_compression.c, spec 5)
 * ------------------------------------------------------------------------- */
extern bool ColumnarCodecAvailable(int compressionType);
extern void ColumnarCompressValueStream(const char *raw, uint32 rawLen,
										int requestedType, int level,
										char **outData, uint32 *outLen,
										int *usedType, int *usedLevel);
extern char *ColumnarDecompressValueStream(const char *comp, uint32 compLen,
										   int compressionType, uint32 rawLen,
										   MemoryContext targetContext);

/* -------------------------------------------------------------------------
 * decompressed-chunk cache (columnar_cache.c, spec 8.3, 9)
 *
 * An optional, backend-local cache of decompressed value streams, keyed by the
 * relation's storage id and the stream's absolute logical offset (both stable
 * and never reused within a storage id, except across a truncate, which fires a
 * relcache invalidation that flushes the whole cache). Off by default; when on
 * it only avoids repeated decompression and never changes results.
 * ------------------------------------------------------------------------- */
extern void ColumnarCacheInit(void);
extern char *ColumnarGetDecompressedStream(uint64 storageId, uint64 absOffset,
										   const char *comp, uint32 compLen,
										   int compressionType, uint32 rawLen,
										   MemoryContext targetContext);

/* -------------------------------------------------------------------------
 * concurrent unique-key insert serialization (columnar_unique.c, issue #5)
 *
 * Before an inserted row is handed to the executor's index maintenance, the
 * table AM insert paths call ColumnarLockUniqueKeys to take a transaction-
 * scoped advisory lock per applicable unique index key, so a concurrent
 * inserter of an equal key serializes behind this transaction until it commits
 * (and has therefore flushed its row), at which point the ordinary btree
 * uniqueness check catches the duplicate. See columnar_unique.c.
 * ------------------------------------------------------------------------- */
extern void ColumnarLockUniqueKeys(Relation rel, TupleTableSlot *slot);
extern void ColumnarUniqueInit(void);

/* -------------------------------------------------------------------------
 * planner integration (columnar_customscan.c, spec 8.3, 9)
 * ------------------------------------------------------------------------- */
extern void ColumnarCustomScanInit(void);

/*
 * The single registered CustomScanMethods, shared by the base custom scan and
 * the vectorized aggregate. The create-state callback dispatches on scanrelid:
 * a scanrelid==0 upper node is the vectorized aggregate.
 */
extern const CustomScanMethods columnar_scan_methods;
extern Node *ColumnarCreateAggScanState(CustomScan *cscan);

/*
 * Build the chunk-group skip scan keys from a plan's restriction clauses.
 * Shared by the base custom scan and the vectorized aggregate (spec 9). Clauses
 * that are not simple "column op const" comparisons are ignored.
 */
extern ScanKey ColumnarBuildScanKeys(List *qual, Index scanrelid,
									 TupleDesc tupdesc, int *nkeys);

/* -------------------------------------------------------------------------
 * vectorized aggregation and filtering (columnar_vector.c, spec 9)
 * ------------------------------------------------------------------------- */
extern void ColumnarVectorInit(void);

/*
 * A single pushed-down comparison predicate "column op const" evaluated
 * column-at-a-time over a decoded chunk group to build a selection vector.
 */
typedef struct ColumnarVecPredicate
{
	int			attidx;			/* 0-based column index */
	bool		varOnLeft;		/* column op const, else const op column */
	FmgrInfo	opFn;			/* the operator function (returns bool) */
	Datum		constValue;
	Oid			collation;

	/*
	 * Typed fast path (I6). fastKind is one of COLUMNAR_VECFAST_* (0 = none,
	 * use opFn); strategy is the btree strategy 1..5 (Less..Greater) already
	 * normalized to "column op const". When fastKind is set, the predicate is
	 * evaluated column-at-a-time with a branch-free typed loop instead of fmgr.
	 */
	int			fastKind;
	int			strategy;
} ColumnarVecPredicate;

#define COLUMNAR_VECFAST_NONE 0
#define COLUMNAR_VECFAST_I16 1
#define COLUMNAR_VECFAST_I32 2
#define COLUMNAR_VECFAST_I64 3
#define COLUMNAR_VECFAST_F32 4
#define COLUMNAR_VECFAST_F64 5

/*
 * Build the array of evaluable predicates from a plan's restriction clauses.
 * Clauses that are not simple, strict "column op const" comparisons on the scan
 * relation are left out; *allConvertible reports whether every clause converted
 * (the vectorized aggregate requires that, so it can rely on the predicates
 * being the complete filter; the vectorized scan does not and lets the executor
 * re-apply the rest). Allocated in the current memory context.
 */
extern ColumnarVecPredicate *ColumnarBuildVecPredicates(List *qual,
														Index scanrelid,
														TupleDesc tupdesc,
														int *npreds,
														bool *allConvertible);

/* whether row i of a decoded chunk group passes every predicate (AND) */
extern bool ColumnarVecRowPasses(ColumnarVecPredicate *preds, int npreds,
								 ColumnarVector *vec, uint64 i);

/*
 * Build a selection vector for a whole chunk group column-at-a-time (I6):
 * sel[i] is true when row i is not deleted and passes every predicate. Uses a
 * branch-free typed loop per predicate where possible, falling back to the
 * operator function otherwise. sel must have room for vec->nrows entries.
 */
extern void ColumnarVecSelect(ColumnarVecPredicate *preds, int npreds,
							  ColumnarVector *vec, bool *sel);

#endif							/* PGCOLUMNAR_H */
