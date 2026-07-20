/*-------------------------------------------------------------------------
 *
 * columnar_visibilitymap.c
 *		Visibility-map fork maintenance for pgColumnar index-only scans
 *		(gap 28 direction 1, phase 1: the load-bearing prototype).
 *
 *		PostgreSQL's index-only-scan executor (nodeIndexonlyscan.c) is
 *		hard-wired to read the table's visibility-map fork via VM_ALL_VISIBLE /
 *		visibilitymap_get_status; there is no table-AM hook to substitute a
 *		different all-visible source. So to make the executor skip the columnar
 *		fetch for an all-visible range, pgColumnar must maintain a real VM fork
 *		on its relation, keyed by the same synthetic block numbers its TIDs use
 *		(block = rowNumber / MaxHeapTuplesPerPage).
 *
 *		The catch: those TIDs are synthetic (columnar data is not stored as heap
 *		pages at those blocks), so the stock write path visibilitymap_set() --
 *		which requires the heap buffer and sets PD_ALL_VISIBLE on it, and whose
 *		signature differs across majors -- cannot be used. This file writes the
 *		VM bit directly: pin/extend the VM page with the stock visibilitymap_pin,
 *		set the all-visible bit using the (stable since 9.6) two-bits-per-block
 *		layout, WAL-log the whole page for crash safety, and leave reads to the
 *		stock visibilitymap_get_status. This decouples the write from any heap
 *		page and from the per-version visibilitymap_set signature.
 *
 *		Phase 1 exposes columnar.vm_selftest(rel, blk) to prove empirically that
 *		a bit written here is read back by visibilitymap_get_status on a columnar
 *		relation. Later phases wire set-in-vacuum and clear-on-write.
 *
 * Independent MIT implementation from the public PostgreSQL API and the
 * documented visibility-map on-disk layout.
 *
 *-------------------------------------------------------------------------
 */
#include "columnar.h"

#include "fmgr.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/visibilitymap.h"
#include "access/xloginsert.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "utils/rel.h"

PG_FUNCTION_INFO_V1(columnar_vm_selftest);

/*
 * Visibility-map on-disk layout. These mirror the private macros in
 * src/backend/access/heap/visibilitymap.c; the two-bits-per-heap-block layout
 * has been stable since PostgreSQL 9.6, and columnar.vm_selftest verifies at
 * run time that a bit written with this layout is read back by the backend's
 * own visibilitymap_get_status (which uses the real macros), so any divergence
 * would surface immediately in the matrix rather than silently.
 */
#define COLUMNAR_VM_MAPSIZE			(BLCKSZ - MAXALIGN(SizeOfPageHeaderData))
#define COLUMNAR_VM_BITS_PER_BLOCK	2
#define COLUMNAR_VM_BLOCKS_PER_BYTE	(BITS_PER_BYTE / COLUMNAR_VM_BITS_PER_BLOCK)
#define COLUMNAR_VM_BLOCKS_PER_PAGE	(COLUMNAR_VM_MAPSIZE * COLUMNAR_VM_BLOCKS_PER_BYTE)
#define COLUMNAR_VM_TO_MAPBYTE(x) \
	(((x) % COLUMNAR_VM_BLOCKS_PER_PAGE) / COLUMNAR_VM_BLOCKS_PER_BYTE)
#define COLUMNAR_VM_TO_OFFSET(x) \
	(((x) % COLUMNAR_VM_BLOCKS_PER_BYTE) * COLUMNAR_VM_BITS_PER_BLOCK)

/*
 * ColumnarVMSetVisible
 *		Mark the synthetic block `blk` all-visible in the relation's VM fork,
 *		WAL-logged. Idempotent: a no-op if the bit is already set. This writes
 *		only the VM fork -- there is no heap page to flag -- which is why it does
 *		not go through visibilitymap_set().
 */
void
ColumnarVMSetVisible(Relation rel, BlockNumber blk)
{
	Buffer		vmbuf = InvalidBuffer;
	Page		page;
	char	   *map;
	uint32		mapByte = COLUMNAR_VM_TO_MAPBYTE(blk);
	uint8		mapBit = (uint8) (VISIBILITYMAP_ALL_VISIBLE << COLUMNAR_VM_TO_OFFSET(blk));

	/* pin (extending the VM fork as needed) the page that holds blk's bit */
	visibilitymap_pin(rel, blk, &vmbuf);
	LockBuffer(vmbuf, BUFFER_LOCK_EXCLUSIVE);

	page = BufferGetPage(vmbuf);
	map = PageGetContents(page);

	if (!(map[mapByte] & mapBit))
	{
		START_CRIT_SECTION();
		map[mapByte] |= mapBit;
		MarkBufferDirty(vmbuf);
		if (RelationNeedsWAL(rel))
		{
			XLogRecPtr	recptr = log_newpage_buffer(vmbuf, false);

			PageSetLSN(page, recptr);
		}
		END_CRIT_SECTION();
	}

	LockBuffer(vmbuf, BUFFER_LOCK_UNLOCK);
	ReleaseBuffer(vmbuf);
}

/*
 * ColumnarVMClearVisible
 *		Clear the all-visible (and all-frozen) bits for `blk`, WAL-logged. Used
 *		by write paths so a modified range is never reported all-visible.
 */
void
ColumnarVMClearVisible(Relation rel, BlockNumber blk)
{
	Buffer		vmbuf = InvalidBuffer;
	Page		page;
	char	   *map;
	uint32		mapByte = COLUMNAR_VM_TO_MAPBYTE(blk);
	uint8		mapBits = (uint8) ((VISIBILITYMAP_ALL_VISIBLE | VISIBILITYMAP_ALL_FROZEN)
								   << COLUMNAR_VM_TO_OFFSET(blk));

	/* nothing to clear if the VM fork does not yet cover blk */
	if (visibilitymap_get_status(rel, blk, &vmbuf) == 0)
	{
		if (BufferIsValid(vmbuf))
			ReleaseBuffer(vmbuf);
		return;
	}

	LockBuffer(vmbuf, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(vmbuf);
	map = PageGetContents(page);

	if (map[mapByte] & mapBits)
	{
		START_CRIT_SECTION();
		map[mapByte] &= ~mapBits;
		MarkBufferDirty(vmbuf);
		if (RelationNeedsWAL(rel))
		{
			XLogRecPtr	recptr = log_newpage_buffer(vmbuf, false);

			PageSetLSN(page, recptr);
		}
		END_CRIT_SECTION();
	}

	LockBuffer(vmbuf, BUFFER_LOCK_UNLOCK);
	ReleaseBuffer(vmbuf);
}

/*
 * ColumnarVMIsVisible
 *		True if `blk` is marked all-visible in the VM fork. Thin wrapper over the
 *		stock reader (the same call the index-only-scan executor makes).
 */
bool
ColumnarVMIsVisible(Relation rel, BlockNumber blk)
{
	Buffer		vmbuf = InvalidBuffer;
	uint8		status = visibilitymap_get_status(rel, blk, &vmbuf);

	if (BufferIsValid(vmbuf))
		ReleaseBuffer(vmbuf);
	return (status & VISIBILITYMAP_ALL_VISIBLE) != 0;
}

/*
 * columnar_vm_selftest(rel regclass, blk int) -> bool
 *		Phase-1 proof: set the all-visible bit for a synthetic block on a
 *		columnar relation, then read it back through the backend's own
 *		visibilitymap_get_status. Returns true iff the round trip succeeds,
 *		which validates that the custom VM write is layout-compatible with the
 *		reader the index-only-scan executor uses.
 */
Datum
columnar_vm_selftest(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	BlockNumber blk = (BlockNumber) PG_GETARG_INT32(1);
	Relation	rel;
	bool		before,
				after;

	rel = table_open(relid, RowExclusiveLock);

	before = ColumnarVMIsVisible(rel, blk);
	ColumnarVMSetVisible(rel, blk);
	after = ColumnarVMIsVisible(rel, blk);

	table_close(rel, RowExclusiveLock);

	/* success: not set before (fresh block), set after */
	PG_RETURN_BOOL(!before && after);
}
