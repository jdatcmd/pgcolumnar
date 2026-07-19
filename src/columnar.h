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

#include "access/skey.h"
#include "access/tableam.h"
#include "lib/stringinfo.h"
#include "nodes/bitmapset.h"
#include "nodes/pg_list.h"
#include "storage/bufpage.h"
#include "storage/relfilelocator.h"
#include "utils/rel.h"
#include "utils/snapshot.h"

/* format version (spec 3) */
#define COLUMNAR_VERSION_MAJOR 2
#define COLUMNAR_VERSION_MINOR 0

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

/* schema that holds the metadata catalog */
#define COLUMNAR_SCHEMA_NAME "columnar"

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
} ColumnarOptions;

/* GUC-backed instance defaults (spec 8.3) */
extern int columnar_stripe_row_limit;
extern int columnar_chunk_group_row_limit;
extern int columnar_compression;		/* one of COLUMNAR_COMPRESSION_* */
extern int columnar_compression_level;	/* zstd level */
extern bool columnar_enable_qual_pushdown;
extern bool columnar_enable_custom_scan;

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
	uint64		valueDecompressedLength;
	uint64		valueCount;

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
 * metadata layer (columnar_metadata.c)
 * ------------------------------------------------------------------------- */
extern uint64 ColumnarNextStorageId(void);
extern void ColumnarInsertStripeRow(const StripeMetadata *stripe);
extern void ColumnarInsertChunkGroupRow(uint64 storageId,
										const ChunkGroupMetadata *cg);
extern void ColumnarInsertChunkRow(uint64 storageId, const ChunkMetadata *chunk);
extern List *ColumnarReadStripeList(uint64 storageId, Snapshot snapshot);
extern List *ColumnarReadChunkGroupList(uint64 storageId, uint64 stripeNum,
										Snapshot snapshot);
extern List *ColumnarReadChunkList(uint64 storageId, uint64 stripeNum,
								   Snapshot snapshot);
extern void ColumnarDeleteMetadata(uint64 storageId);

/* per-table options catalog (spec 7.4) */
extern bool ColumnarReadOptions(Oid relid, ColumnarOptions *opts);
extern void ColumnarDeleteOptions(Oid relid);

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
extern void ColumnarUpsertRowMask(uint64 storageId, RowMaskMetadata *rm);
extern uint64 ColumnarNextRowMaskId(void);

/* -------------------------------------------------------------------------
 * writer (columnar_write_state.c)
 * ------------------------------------------------------------------------- */
typedef struct ColumnarWriteState ColumnarWriteState;

extern ColumnarWriteState *ColumnarGetWriteState(Relation rel);
extern uint64 ColumnarWriteRow(ColumnarWriteState *writeState, Relation rel,
							   Datum *values, bool *nulls);
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
extern bool ColumnarReadNextRow(ColumnarReadState *readState,
								Datum *values, bool *nulls,
								uint64 *rowNumber);
extern void ColumnarRescanRead(ColumnarReadState *readState);
extern void ColumnarEndRead(ColumnarReadState *readState);

/*
 * Chunk-group skip counters for the current scan (spec 9), used by the custom
 * scan's EXPLAIN output to show how many chunk groups the min/max skip lists
 * removed. total = read + skipped over the groups the scan has reached.
 */
extern void ColumnarReadStats(ColumnarReadState *readState,
							  uint64 *groupsRead, uint64 *groupsSkipped,
							  uint64 *groupsTotal);

/*
 * Fetch a single row by its 1-based row number (spec 6), for the table AM's
 * fetch-by-tid callback used by UPDATE. Fills values/nulls (by-reference values
 * are allocated in the current memory context) and returns true when the row
 * exists and is not marked deleted in the row mask.
 */
extern bool ColumnarReadRowByNumber(Relation rel, Snapshot snapshot,
									uint64 rowNumber, Datum *values, bool *nulls);

/* value stream encode/decode shared by writer and reader */
extern void ColumnarEncodeValue(StringInfo buf, Form_pg_attribute att,
								Datum value);
extern Datum ColumnarDecodeValue(Form_pg_attribute att, char **cursor,
								 MemoryContext targetContext);

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
 * planner integration (columnar_customscan.c, spec 8.3, 9)
 * ------------------------------------------------------------------------- */
extern void ColumnarCustomScanInit(void);

#endif							/* PGCOLUMNAR_H */
