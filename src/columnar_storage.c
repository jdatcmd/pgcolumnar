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

#include "fmgr.h"
#include "columnar_compat.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "catalog/storage.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "storage/smgr.h"
#include "utils/rel.h"

#if PG_VERSION_NUM >= 170000
#include "storage/read_stream.h"
#endif

/* the metapage struct lives right after the page header on block 0 */
#define COLUMNAR_METAPAGE_BLOCKNO 0
#define COLUMNAR_EMPTY_BLOCKNO 1
#define ColumnarMetapagePointer(page) ((ColumnarMetapage *) PageGetContents(page))

/*
 * Stream/prefetch the block reads in ColumnarReadLogicalData via the read
 * stream API (PostgreSQL 17+), which lets the PostgreSQL 18 asynchronous I/O
 * subsystem read ahead. On by default; off falls back to synchronous ReadBuffer.
 * On PostgreSQL 16 and earlier the streaming path is compiled out and this GUC
 * has no effect.
 */
bool		columnar_enable_read_stream = true;

#if PG_VERSION_NUM >= 170000
/* contiguous ascending block range for the read stream callback */
typedef struct ColumnarBlockRange
{
	BlockNumber next;
	BlockNumber last;
}			ColumnarBlockRange;

static BlockNumber
columnar_read_stream_next(ReadStream *stream, void *private_data,
						  void *per_buffer_data)
{
	ColumnarBlockRange *range = (ColumnarBlockRange *) private_data;

	if (range->next > range->last)
		return InvalidBlockNumber;
	return range->next++;
}
#endif

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
	Page		page = ColumnarAllocPage();
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
		log_newpage(&COLUMNAR_SMGR_LOCATOR(srel), MAIN_FORKNUM,
					COLUMNAR_METAPAGE_BLOCKNO, page, true);
	PageSetChecksumInplace(page, COLUMNAR_METAPAGE_BLOCKNO);
	smgrextend(srel, MAIN_FORKNUM, COLUMNAR_METAPAGE_BLOCKNO, page, true);

	/* block 1: reserved, empty */
	PageInit(page, BLCKSZ, 0);
	if (needsWAL)
		log_newpage(&COLUMNAR_SMGR_LOCATOR(srel), MAIN_FORKNUM,
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
 * ColumnarReserveRowNumbers
 *		Reserve a stripe id and a contiguous run of "rowCount" row numbers by
 *		advancing the metapage's reservedStripeId and reservedRowNumber marks
 *		(spec 2.2, 6). Returns the reserved stripe id and the first row number
 *		of the run.
 *
 *		This is done when a writer begins buffering a stripe, so that every
 *		row gets its final, stable row number (and therefore its item pointer)
 *		at insert time. That is what lets an index carry a correct TID for a
 *		freshly inserted row (spec 6, 9). The byte offset is reserved
 *		separately, at flush, once the stripe's size is known
 *		(ColumnarReserveOffset). Row numbers not used by a short stripe are
 *		simply left as a gap; row numbers need only be unique and stable.
 *
 *		Serialized by the exclusive lock on the metapage buffer; no relation
 *		extension lock is needed here because no data pages are extended.
 */
void
ColumnarReserveRowNumbers(Relation rel, uint64 rowCount,
						  uint64 *stripeId, uint64 *firstRowNumber)
{
	Buffer		buffer;
	Page		page;
	ColumnarMetapage *meta;

	buffer = ReadBuffer(rel, COLUMNAR_METAPAGE_BLOCKNO);
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(buffer);
	meta = ColumnarMetapagePointer(page);

	*stripeId = meta->reservedStripeId;
	*firstRowNumber = meta->reservedRowNumber;

	meta->reservedStripeId += 1;
	meta->reservedRowNumber += rowCount;

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
 * ColumnarReserveOffset
 *		Reserve a page-aligned logical byte range of "dataLength" bytes for a
 *		stripe's data and return its file offset (spec 2.1, 2.2). New
 *		reservations start on a fresh page.
 *
 *		The caller must already hold the relation extension lock and must
 *		write the reserved data immediately (via ColumnarWriteLogicalData)
 *		before releasing it, so that reservation is serialized and the P_NEW
 *		extends match the reserved blocks.
 */
void
ColumnarReserveOffset(Relation rel, uint64 dataLength, uint64 *fileOffset)
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

	*fileOffset = alignedOffset;
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
 * ColumnarAdvanceReservedOffset
 *		Increase the metapage highwater by addBytes without writing any data,
 *		leaving a gap between the physical EOF and the new highwater. This is a
 *		test hook for the gap-tolerant write path (it deliberately produces the
 *		"highwater ahead of EOF" state that an aborted end-truncation leaves).
 *		Increase-only, so it can never make the highwater overlap live data.
 */
void
ColumnarAdvanceReservedOffset(Relation rel, uint64 addBytes)
{
	Buffer		buffer;
	Page		page;
	ColumnarMetapage *meta;

	buffer = ReadBuffer(rel, COLUMNAR_METAPAGE_BLOCKNO);
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(buffer);
	meta = ColumnarMetapagePointer(page);

	meta->reservedOffset += addBytes;

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
 *		logical offset, splitting it across the physical pages it maps to
 *		(spec 2.1). Blocks past the current end of the relation are extended;
 *		blocks below it are reused freed ranges (Phase F reclaim) and written in
 *		place. Reuse is only ever handed out for ranges no snapshot can still read
 *		(oldest-xmin gated), so overwriting the old contents is safe.
 *
 *		The target block may be strictly BEYOND the current EOF (a gap) when the
 *		metapage highwater is ahead of the physical file -- this is the state left
 *		by an aborted or crash-interrupted end-truncation (the file was shortened
 *		but the highwater was not, or the shortening's catalog changes rolled back).
 *		Such a gap is filled with empty pages up to the target block, so the state
 *		self-heals: the next write simply re-extends the fork. The caller holds the
 *		relation extension lock, so the P_NEW extensions here are serialized.
 */
void
ColumnarWriteLogicalData(Relation rel, uint64 logicalOffset,
						 char *data, uint64 length)
{
	uint64		L = logicalOffset;
	uint64		remaining = length;
	char	   *src = data;
	BlockNumber nblocks = RelationGetNumberOfBlocksInFork(rel, MAIN_FORKNUM);

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

		if (blockno < nblocks)
		{
			/* reused freed range: overwrite the existing block in place */
			buffer = ReadBufferExtended(rel, MAIN_FORKNUM, blockno,
										RBM_NORMAL, NULL);
		}
		else
		{
			/*
			 * Fill any gap between the current EOF and the target block with
			 * empty, WAL-logged pages so a standby extends identically, then the
			 * final P_NEW lands exactly on the target block. In the common append
			 * case (blockno == nblocks) the gap loop does nothing.
			 */
			while (nblocks < blockno)
			{
				Buffer		gap = ReadBufferExtended(rel, MAIN_FORKNUM, P_NEW,
													 RBM_NORMAL, NULL);
				Page		gpage;

				LockBuffer(gap, BUFFER_LOCK_EXCLUSIVE);
				gpage = BufferGetPage(gap);
				PageInit(gpage, BLCKSZ, 0);
				START_CRIT_SECTION();
				MarkBufferDirty(gap);
				if (RelationNeedsWAL(rel))
					PageSetLSN(gpage, log_newpage_buffer(gap, true));
				END_CRIT_SECTION();
				UnlockReleaseBuffer(gap);
				nblocks++;
			}

			/* append: extend the relation by one block onto the target */
			buffer = ReadBufferExtended(rel, MAIN_FORKNUM, P_NEW,
										RBM_NORMAL, NULL);
			if (BufferGetBlockNumber(buffer) != blockno)
				elog(ERROR,
					 "columnar: unexpected block %u while writing stripe at "
					 "logical offset " UINT64_FORMAT " (expected %u)",
					 BufferGetBlockNumber(buffer), logicalOffset, blockno);
			nblocks = blockno + 1;
		}

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

	if (remaining == 0)
		return;

#if PG_VERSION_NUM >= 170000
	if (columnar_enable_read_stream)
	{
		/*
		 * The blocks map to a contiguous ascending range, so a read stream can
		 * prefetch them (and use asynchronous I/O on PostgreSQL 18+).
		 * read_stream_next_buffer returns the pinned buffers in request order,
		 * so the same L/pageOffset walk drives the copy; the buffers are share-
		 * locked here just as the synchronous path does.
		 */
		ColumnarBlockRange range;
		ReadStream *stream;

		range.next = (BlockNumber) (L / COLUMNAR_BYTES_PER_PAGE);
		range.last = (BlockNumber) ((L + remaining - 1) / COLUMNAR_BYTES_PER_PAGE);

		stream = read_stream_begin_relation(READ_STREAM_SEQUENTIAL, NULL, rel,
											MAIN_FORKNUM,
											columnar_read_stream_next, &range, 0);

		while (remaining > 0)
		{
			uint32		pageOffset = (uint32) (L % COLUMNAR_BYTES_PER_PAGE);
			uint32		n = (uint32) Min(COLUMNAR_BYTES_PER_PAGE - pageOffset,
										 remaining);
			Buffer		buffer = read_stream_next_buffer(stream, NULL);
			Page		page;

			/*
			 * The stream must yield exactly one buffer per block in the range.
			 * Guard at run time (not just Assert) so a corrupt offset that
			 * miscomputes the range ends the stream early with a clean error
			 * instead of LockBuffer(InvalidBuffer) dereferencing out of bounds.
			 */
			if (!BufferIsValid(buffer))
			{
				read_stream_end(stream);
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("columnar: read stream ended before the requested range")));
			}
			Assert(BufferGetBlockNumber(buffer) ==
				   (BlockNumber) (L / COLUMNAR_BYTES_PER_PAGE));
			LockBuffer(buffer, BUFFER_LOCK_SHARE);
			page = BufferGetPage(buffer);
			memcpy(dst, (char *) page + SizeOfPageHeaderData + pageOffset, n);
			UnlockReleaseBuffer(buffer);

			dst += n;
			L += n;
			remaining -= n;
		}
		read_stream_end(stream);
		return;
	}
#endif

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
