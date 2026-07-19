/*-------------------------------------------------------------------------
 *
 * columnar_storage.c
 *		Physical storage layer for pgColumnar: metapage, the logical-to-
 *		physical byte mapping, and the append-only reservation model.
 *
 * Implements spec sections 2, 2.1, 2.2, 3, and 6. All storage lives in the
 * relation's main fork using standard PostgreSQL pages, so the buffer
 * manager, WAL, and checksums apply.
 *
 *-------------------------------------------------------------------------
 */
#include "columnar.h"

#include "access/xlog.h"
#include "access/xloginsert.h"
#include "catalog/storage.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/smgr.h"
#include "utils/rel.h"

/* the metapage struct lives right after the page header on block 0 */
#define COLUMNAR_METAPAGE_BLOCKNO 0
#define COLUMNAR_EMPTY_BLOCKNO 1
#define ColumnarMetapagePointer(page) ((ColumnarMetapage *) PageGetContents(page))

/*
 * ColumnarWriteNewMetapage
 *		Initialize a freshly created relation's storage: block 0 holds the
 *		metapage with the initial reserved values from spec 3, block 1 is
 *		reserved and left empty. Written with a WAL full-page image and an
 *		immediate sync because the new storage does not pass through shared
 *		buffers here.
 */
void
ColumnarWriteNewMetapage(const RelFileLocator *newrlocator,
						 SMgrRelation srel, char persistence,
						 uint64 storageId)
{
	Page		page = (Page) palloc_aligned(BLCKSZ, PG_IO_ALIGN_SIZE,
											 MCXT_ALLOC_ZERO);
	ColumnarMetapage *meta;
	bool		needsWAL = (persistence == RELPERSISTENCE_PERMANENT);

	/* block 0: metapage */
	PageInit(page, BLCKSZ, 0);
	meta = ColumnarMetapagePointer(page);
	meta->versionMajor = COLUMNAR_VERSION_MAJOR;
	meta->versionMinor = COLUMNAR_VERSION_MINOR;
	meta->storageId = storageId;
	meta->reservedStripeId = 1;
	meta->reservedRowNumber = COLUMNAR_FIRST_ROW_NUMBER;
	meta->reservedOffset = COLUMNAR_FIRST_LOGICAL_OFFSET;
	meta->unloggedReset = false;
	((PageHeader) page)->pd_lower =
		((char *) meta - (char *) page) + sizeof(ColumnarMetapage);

	if (needsWAL)
		log_newpage(&srel->smgr_rlocator.locator, MAIN_FORKNUM,
					COLUMNAR_METAPAGE_BLOCKNO, page, true);
	PageSetChecksumInplace(page, COLUMNAR_METAPAGE_BLOCKNO);
	smgrextend(srel, MAIN_FORKNUM, COLUMNAR_METAPAGE_BLOCKNO, page, true);

	/* block 1: reserved, empty */
	PageInit(page, BLCKSZ, 0);
	if (needsWAL)
		log_newpage(&srel->smgr_rlocator.locator, MAIN_FORKNUM,
					COLUMNAR_EMPTY_BLOCKNO, page, true);
	PageSetChecksumInplace(page, COLUMNAR_EMPTY_BLOCKNO);
	smgrextend(srel, MAIN_FORKNUM, COLUMNAR_EMPTY_BLOCKNO, page, true);

	smgrimmedsync(srel, MAIN_FORKNUM);

	pfree(page);
}

/*
 * ColumnarReadMetapage
 *		Read the metapage of an existing relation into *meta.
 */
void
ColumnarReadMetapage(Relation rel, ColumnarMetapage *meta)
{
	Buffer		buffer;
	Page		page;

	buffer = ReadBuffer(rel, COLUMNAR_METAPAGE_BLOCKNO);
	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buffer);
	memcpy(meta, ColumnarMetapagePointer(page), sizeof(ColumnarMetapage));
	UnlockReleaseBuffer(buffer);

	if (meta->versionMajor != COLUMNAR_VERSION_MAJOR)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("unsupported columnar format version %u.%u",
						meta->versionMajor, meta->versionMinor)));
}

uint64
ColumnarStorageId(Relation rel)
{
	ColumnarMetapage meta;

	ColumnarReadMetapage(rel, &meta);
	return meta.storageId;
}

/*
 * ColumnarReserveStripe
 *		Advance the three metapage high-water marks and hand back a reserved
 *		stripe id, first row number, and page-aligned file offset (spec 2.2).
 *		New reservations start on a fresh page (spec 2.1).
 *
 *		The caller must already hold the relation extension lock and must
 *		write the reserved data immediately (via ColumnarWriteLogicalData)
 *		before releasing it, so that reservation is serialized and the
 *		P_NEW extends match the reserved blocks.
 */
void
ColumnarReserveStripe(Relation rel, uint64 rowCount, uint64 dataLength,
					  uint64 *stripeId, uint64 *firstRowNumber,
					  uint64 *fileOffset)
{
	Buffer		buffer;
	Page		page;
	ColumnarMetapage *meta;
	uint64		alignedOffset;

	buffer = ReadBuffer(rel, COLUMNAR_METAPAGE_BLOCKNO);
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(buffer);
	meta = ColumnarMetapagePointer(page);

	/* align the start of this reservation up to a page boundary */
	alignedOffset = ((meta->reservedOffset + COLUMNAR_BYTES_PER_PAGE - 1) /
					 COLUMNAR_BYTES_PER_PAGE) * COLUMNAR_BYTES_PER_PAGE;

	*stripeId = meta->reservedStripeId;
	*firstRowNumber = meta->reservedRowNumber;
	*fileOffset = alignedOffset;

	meta->reservedStripeId += 1;
	meta->reservedRowNumber += rowCount;
	meta->reservedOffset = alignedOffset + dataLength;

	START_CRIT_SECTION();
	MarkBufferDirty(buffer);
	if (RelationNeedsWAL(rel))
	{
		XLogRecPtr	recptr = log_newpage_buffer(buffer, true);

		PageSetLSN(page, recptr);
	}
	END_CRIT_SECTION();

	UnlockReleaseBuffer(buffer);
}

/*
 * ColumnarWriteLogicalData
 *		Write a contiguous logical byte range starting at a page-aligned
 *		logical offset, splitting it across new physical pages by the mapping
 *		in spec 2.1. Because stripes start on a page boundary and are
 *		append-only, every page written here is newly extended.
 */
void
ColumnarWriteLogicalData(Relation rel, uint64 logicalOffset,
						 char *data, uint64 length)
{
	uint64		L = logicalOffset;
	uint64		remaining = length;
	char	   *src = data;

	Assert(logicalOffset % COLUMNAR_BYTES_PER_PAGE == 0);

	while (remaining > 0)
	{
		BlockNumber blockno = (BlockNumber) (L / COLUMNAR_BYTES_PER_PAGE);
		uint32		pageOffset = (uint32) (L % COLUMNAR_BYTES_PER_PAGE);
		uint32		n = (uint32) Min(COLUMNAR_BYTES_PER_PAGE - pageOffset,
									 remaining);
		Buffer		buffer;
		Page		page;
		PageHeader	phdr;

		buffer = ReadBufferExtended(rel, MAIN_FORKNUM, P_NEW,
									RBM_NORMAL, NULL);
		if (BufferGetBlockNumber(buffer) != blockno)
			elog(ERROR,
				 "columnar: unexpected block %u while writing stripe at "
				 "logical offset " UINT64_FORMAT " (expected %u)",
				 BufferGetBlockNumber(buffer), logicalOffset, blockno);

		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buffer);
		PageInit(page, BLCKSZ, 0);
		memcpy((char *) page + SizeOfPageHeaderData + pageOffset, src, n);

		phdr = (PageHeader) page;
		phdr->pd_lower = SizeOfPageHeaderData + pageOffset + n;

		START_CRIT_SECTION();
		MarkBufferDirty(buffer);
		if (RelationNeedsWAL(rel))
		{
			XLogRecPtr	recptr = log_newpage_buffer(buffer, true);

			PageSetLSN(page, recptr);
		}
		END_CRIT_SECTION();

		UnlockReleaseBuffer(buffer);

		src += n;
		L += n;
		remaining -= n;
	}
}

/*
 * ColumnarReadLogicalData
 *		Read a contiguous logical byte range into dest by walking the pages
 *		it maps to (spec 2.1).
 */
void
ColumnarReadLogicalData(Relation rel, uint64 logicalOffset,
						char *dest, uint64 length)
{
	uint64		L = logicalOffset;
	uint64		remaining = length;
	char	   *dst = dest;

	while (remaining > 0)
	{
		BlockNumber blockno = (BlockNumber) (L / COLUMNAR_BYTES_PER_PAGE);
		uint32		pageOffset = (uint32) (L % COLUMNAR_BYTES_PER_PAGE);
		uint32		n = (uint32) Min(COLUMNAR_BYTES_PER_PAGE - pageOffset,
									 remaining);
		Buffer		buffer;
		Page		page;

		buffer = ReadBuffer(rel, blockno);
		LockBuffer(buffer, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buffer);
		memcpy(dst, (char *) page + SizeOfPageHeaderData + pageOffset, n);
		UnlockReleaseBuffer(buffer);

		dst += n;
		L += n;
		remaining -= n;
	}
}

/*
 * ColumnarResetMetapage
 *		Reset the reserved high-water marks to their initial values, keeping
 *		the storage id. Used by non-transactional truncate.
 */
void
ColumnarResetMetapage(Relation rel)
{
	Buffer		buffer;
	Page		page;
	ColumnarMetapage *meta;

	buffer = ReadBuffer(rel, COLUMNAR_METAPAGE_BLOCKNO);
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(buffer);
	meta = ColumnarMetapagePointer(page);

	meta->reservedStripeId = 1;
	meta->reservedRowNumber = COLUMNAR_FIRST_ROW_NUMBER;
	meta->reservedOffset = COLUMNAR_FIRST_LOGICAL_OFFSET;

	START_CRIT_SECTION();
	MarkBufferDirty(buffer);
	if (RelationNeedsWAL(rel))
	{
		XLogRecPtr	recptr = log_newpage_buffer(buffer, true);

		PageSetLSN(page, recptr);
	}
	END_CRIT_SECTION();

	UnlockReleaseBuffer(buffer);
}

/*
 * Row-number <-> item-pointer mapping (spec 6). Row number 0 is invalid.
 */
void
ColumnarRowNumberToItemPointer(uint64 rowNumber, ItemPointer tid)
{
	BlockNumber blockno;
	OffsetNumber offset;

	Assert(rowNumber > 0);
	blockno = (BlockNumber) (rowNumber / COLUMNAR_VALID_ITEMPOINTER_OFFSETS);
	offset = (OffsetNumber) ((rowNumber % COLUMNAR_VALID_ITEMPOINTER_OFFSETS) +
							 FirstOffsetNumber);
	ItemPointerSet(tid, blockno, offset);
}

uint64
ColumnarItemPointerToRowNumber(ItemPointer tid)
{
	BlockNumber blockno = ItemPointerGetBlockNumber(tid);
	OffsetNumber offset = ItemPointerGetOffsetNumber(tid);

	return (uint64) blockno * COLUMNAR_VALID_ITEMPOINTER_OFFSETS +
		(offset - FirstOffsetNumber);
}
