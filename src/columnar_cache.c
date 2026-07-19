/*-------------------------------------------------------------------------
 *
 * columnar_cache.c
 *		Optional decompressed-chunk cache for pgColumnar (spec 8.3, 9).
 *
 * When columnar.enable_column_cache is on, the reader keeps decompressed value
 * streams in a backend-local cache so that repeated reads of the same chunk
 * group reuse the decompressed bytes instead of decompressing again. The cache
 * is a pure optimization: with it on or off, every query returns exactly the
 * same rows. It is off by default and bounded by columnar.column_cache_size
 * megabytes with least-recently-used eviction.
 *
 * Safety of the key. A cache entry is keyed by (storageId, absOffset), where
 * absOffset is the value stream's absolute logical offset in the relation. A
 * stripe's data is written append-only and never rewritten, so within one
 * storage id an absolute offset holds exactly one immutable value stream. Two
 * events can make a key stale: a truncate resets the metapage so offsets are
 * reused, and vacuum swaps to a new storage id. Both fire a relcache
 * invalidation, and this module flushes the entire cache on any relcache
 * invalidation, so a stale entry is never read. The returned buffer is always a
 * fresh copy in the caller's context, so eviction can free cache memory without
 * touching a buffer a scan is still using.
 *
 * Independent MIT implementation built from design/FORMAT_AND_INTERFACE_SPEC.md
 * (format 2.0), design/REWRITE_PLAN.md section 6, and the public PostgreSQL 17
 * API only.
 *
 *-------------------------------------------------------------------------
 */
#include "columnar.h"

#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/memutils.h"

/* GUCs (spec 8.3); registered in columnar_tableam.c _PG_init */
bool		columnar_enable_column_cache = false;
int			columnar_column_cache_size = 200;	/* megabytes */

typedef struct ColumnarCacheKey
{
	uint64		storageId;
	uint64		absOffset;		/* stripe file_offset + value_stream_offset */
} ColumnarCacheKey;

typedef struct ColumnarCacheEntry
{
	ColumnarCacheKey key;		/* must be first: dynahash hashes on it */
	uint32		rawLen;			/* decompressed length */
	int			compressionType;
	char	   *buffer;			/* rawLen decompressed bytes, in cacheContext */
	struct ColumnarCacheEntry *lruPrev;
	struct ColumnarCacheEntry *lruNext;
} ColumnarCacheEntry;

static HTAB *cacheHash = NULL;
static MemoryContext cacheContext = NULL;
static Size cacheBytes = 0;
static ColumnarCacheEntry *lruHead = NULL;	/* most recently used */
static ColumnarCacheEntry *lruTail = NULL;	/* least recently used */

static void
columnar_cache_flush_all(Datum arg, Oid relid)
{
	/*
	 * Drop everything on any relcache invalidation. This is heavy-handed but
	 * always correct, and the events that matter (truncate reusing offsets,
	 * vacuum swapping storage) are exactly relcache invalidations. Plain reads
	 * do not invalidate the relcache, so a warm cache stays warm across repeated
	 * queries that do no DDL.
	 */
	if (cacheHash == NULL)
		return;

	hash_destroy(cacheHash);
	cacheHash = NULL;
	MemoryContextReset(cacheContext);
	cacheBytes = 0;
	lruHead = NULL;
	lruTail = NULL;
}

void
ColumnarCacheInit(void)
{
	/*
	 * Create the long-lived context now; the hash table is created lazily on
	 * first use (and recreated after a flush). Register the invalidation
	 * callback once at load time.
	 */
	cacheContext = AllocSetContextCreate(TopMemoryContext,
										 "columnar decompressed cache",
										 ALLOCSET_DEFAULT_SIZES);
	CacheRegisterRelcacheCallback(columnar_cache_flush_all, (Datum) 0);
}

static void
columnar_cache_create_hash(void)
{
	HASHCTL		info;

	MemSet(&info, 0, sizeof(info));
	info.keysize = sizeof(ColumnarCacheKey);
	info.entrysize = sizeof(ColumnarCacheEntry);
	info.hcxt = cacheContext;

	cacheHash = hash_create("columnar decompressed chunk cache", 128, &info,
							HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

/* unlink an entry from the LRU list */
static void
columnar_lru_unlink(ColumnarCacheEntry *e)
{
	if (e->lruPrev != NULL)
		e->lruPrev->lruNext = e->lruNext;
	else
		lruHead = e->lruNext;

	if (e->lruNext != NULL)
		e->lruNext->lruPrev = e->lruPrev;
	else
		lruTail = e->lruPrev;

	e->lruPrev = NULL;
	e->lruNext = NULL;
}

/* push an entry to the most-recently-used end */
static void
columnar_lru_push_front(ColumnarCacheEntry *e)
{
	e->lruPrev = NULL;
	e->lruNext = lruHead;
	if (lruHead != NULL)
		lruHead->lruPrev = e;
	lruHead = e;
	if (lruTail == NULL)
		lruTail = e;
}

static void
columnar_lru_touch(ColumnarCacheEntry *e)
{
	if (lruHead == e)
		return;
	columnar_lru_unlink(e);
	columnar_lru_push_front(e);
}

/* evict least-recently-used entries until within budget, never evicting keep */
static void
columnar_cache_evict(Size budget, ColumnarCacheEntry *keep)
{
	while (cacheBytes > budget && lruTail != NULL && lruTail != keep)
	{
		ColumnarCacheEntry *victim = lruTail;
		bool		found;

		columnar_lru_unlink(victim);
		cacheBytes -= victim->rawLen;
		pfree(victim->buffer);
		hash_search(cacheHash, &victim->key, HASH_REMOVE, &found);
	}
}

/*
 * ColumnarGetDecompressedStream
 *		Return rawLen decompressed bytes of a value stream in targetContext. With
 *		the cache off, this simply decompresses. With the cache on, a hit copies
 *		the cached decompressed bytes (skipping decompression) and a miss
 *		decompresses and stores a copy for next time. The result is always a
 *		fresh buffer owned by the caller, so cache eviction is always safe.
 */
char *
ColumnarGetDecompressedStream(uint64 storageId, uint64 absOffset,
							  const char *comp, uint32 compLen,
							  int compressionType, uint32 rawLen,
							  MemoryContext targetContext)
{
	ColumnarCacheKey key;
	ColumnarCacheEntry *entry;
	bool		found;
	char	   *result;
	Size		budget;

	/* an all-null chunk has an empty value stream (spec 4) */
	if (rawLen == 0)
		return NULL;

	if (!columnar_enable_column_cache)
		return ColumnarDecompressValueStream(comp, compLen, compressionType,
											 rawLen, targetContext);

	if (cacheHash == NULL)
		columnar_cache_create_hash();

	key.storageId = storageId;
	key.absOffset = absOffset;

	entry = (ColumnarCacheEntry *) hash_search(cacheHash, &key, HASH_FIND, &found);
	if (found && entry->rawLen == rawLen &&
		entry->compressionType == compressionType)
	{
		result = MemoryContextAlloc(targetContext, rawLen);
		memcpy(result, entry->buffer, rawLen);
		columnar_lru_touch(entry);
		return result;
	}

	/* miss (or a defensive key collision): decompress into the caller's buffer */
	result = ColumnarDecompressValueStream(comp, compLen, compressionType,
										   rawLen, targetContext);

	/* do not cache a single stream larger than the whole budget */
	budget = (Size) columnar_column_cache_size * 1024L * 1024L;
	if ((Size) rawLen > budget)
		return result;

	/* store a copy in the cache for future reads */
	entry = (ColumnarCacheEntry *) hash_search(cacheHash, &key, HASH_ENTER, &found);
	if (found)
	{
		/* replacing a stale entry for the same key: free its old buffer */
		columnar_lru_unlink(entry);
		cacheBytes -= entry->rawLen;
		pfree(entry->buffer);
	}
	entry->rawLen = rawLen;
	entry->compressionType = compressionType;
	entry->buffer = MemoryContextAlloc(cacheContext, rawLen);
	memcpy(entry->buffer, result, rawLen);
	entry->lruPrev = NULL;
	entry->lruNext = NULL;
	columnar_lru_push_front(entry);
	cacheBytes += rawLen;

	columnar_cache_evict(budget, entry);

	return result;
}
