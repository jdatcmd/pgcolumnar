/*-------------------------------------------------------------------------
 *
 * columnar_unique.c
 *		Concurrent unique-key insert serialization for pgColumnar (issue #5).
 *
 * A columnar row's data is invisible to other backends until its stripe is
 * flushed at statement end, but its btree index entry (with the eagerly
 * reserved synthetic TID) is written immediately. PostgreSQL's dirty-snapshot
 * uniqueness check (_bt_check_unique -> table_index_fetch_tuple_check) resolves
 * that TID through columnar_index_fetch_tuple, which returns false for a row
 * still buffered in another backend's private write state. So two transactions
 * inserting the same unique key in overlapping windows can both miss the
 * conflict and both commit (see design/ISSUE_5_ANALYSIS.md, section A).
 *
 * The fix here serializes inserters of the SAME unique key. Before a row is
 * handed back to the executor's index maintenance, the table AM insert paths
 * call ColumnarLockUniqueKeys, which takes a transaction-scoped advisory lock
 * (the same SET_LOCKTAG_ADVISORY primitive used by the issue #4 row_mask lock)
 * keyed by the row's unique key value(s). Because the lock is held to commit,
 * when a second inserter finally acquires it the first inserter has either
 * committed (its statement-end flush ran, so its row is now visible and the
 * ordinary btree check raises the unique violation) or aborted (its stripe rows
 * are MVCC-invisible, so the second inserter proceeds correctly).
 *
 * Correctness rests on the invariant that two key values that are EQUAL under
 * the index's equality operator map to the SAME lock. Raw bytes are unsafe
 * (numeric 1.0 vs 1.00, citext case, collation-equal text). We therefore hash
 * each key column with the key type's default hash support function, which is
 * defined to be consistent with the type's default equality (numeric normalizes
 * scale, citext folds case, text uses the collation-aware hash). Whenever we
 * cannot prove the index's operator class matches that default equality, or the
 * key type has no hash support, the index is handled with a single coarse
 * per-index lock instead: correctness-preserving over-serialization.
 *
 * This module derives everything from design/FORMAT_AND_INTERFACE_SPEC.md, the
 * issue #5 design analysis, and the public PostgreSQL API. See PROVENANCE.md.
 *
 *-------------------------------------------------------------------------
 */
#include "columnar.h"

#include "access/genam.h"
#include "catalog/index.h"
#include "catalog/pg_am_d.h"
#include "commands/defrem.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "storage/lock.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/typcache.h"

/* GUCs (spec 8.3): default on, bounded bucket count to bound the lock budget */
bool		columnar_enable_unique_lock = true;
int			columnar_unique_lock_buckets = 128;

/*
 * Advisory-lock discriminator in locktag_field4. The issue #4 row_mask lock
 * uses 1; the unique-key lock uses 2 so the two lock spaces never false-share.
 */
#define COLUMNAR_UNIQUE_LOCK_CLASS 2

/* 64-bit FNV-1a basis/prime, matching rowmask_chunk_lock_key's mixer */
#define COLUMNAR_FNV_OFFSET UINT64CONST(1469598103934665603)
#define COLUMNAR_FNV_PRIME  UINT64CONST(1099511628211)

/*
 * Canonical hash contribution for a NULL key column under NULLS NOT DISTINCT
 * (PG15+). Any fixed value works as long as it is used for every NULL, so two
 * rows whose keys are NULL in the same position hash alike.
 */
#define COLUMNAR_NULL_KEY_HASH 0x9E3779B9U

/* one key column of a cached unique index */
typedef struct UniqueKeyCol
{
	FmgrInfo	hashFn;			/* type default hash proc, consistent with = */
	Oid			collation;		/* index column collation for the hash call */
} UniqueKeyCol;

/* one applicable unique index on a relation */
typedef struct UniqueIndexEntry
{
	Oid			indexOid;
	IndexInfo  *indexInfo;		/* for FormIndexDatum (plain and expression) */
	ExprState  *predicate;		/* partial-index predicate, or NULL */
	int			nKeyCols;		/* indnkeyatts: key columns, excludes INCLUDE */
	bool		nullsNotDistinct;	/* PG15+ NULLS NOT DISTINCT */
	bool		coarse;			/* single-bucket lock (hash not provably safe) */
	UniqueKeyCol *cols;			/* [nKeyCols]; unused when coarse */
} UniqueIndexEntry;

/* per-relation cache entry, keyed by relid in RelUniqueCache */
typedef struct RelUniqueCacheEntry
{
	Oid			relid;			/* hash key (must be first) */
	MemoryContext cxt;			/* holds estate, indexInfo, cols */
	EState	   *estate;			/* shared executor state for the row */
	int			nIndexes;
	UniqueIndexEntry *indexes;
} RelUniqueCacheEntry;

static HTAB *RelUniqueCache = NULL;

/* -------------------------------------------------------------------------
 * cache management
 * ------------------------------------------------------------------------- */

/*
 * Drop every cached entry. Called on any relcache invalidation: rebuilding on
 * demand is cheap and correctness must not depend on catching the precise relid
 * (an invalidation on an index carries the index's relid, not the table's).
 */
static void
columnar_unique_invalidate(Datum arg, Oid relid)
{
	HASH_SEQ_STATUS status;
	RelUniqueCacheEntry *entry;

	if (RelUniqueCache == NULL)
		return;

	hash_seq_init(&status, RelUniqueCache);
	while ((entry = (RelUniqueCacheEntry *) hash_seq_search(&status)) != NULL)
	{
		MemoryContext cxt = entry->cxt;

		if (hash_search(RelUniqueCache, &entry->relid, HASH_REMOVE, NULL) == NULL)
			elog(ERROR, "columnar unique cache corrupted");
		MemoryContextDelete(cxt);
	}
}

static void
columnar_unique_cache_init(void)
{
	HASHCTL		ctl;

	if (RelUniqueCache != NULL)
		return;

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(RelUniqueCacheEntry);
	ctl.hcxt = CacheMemoryContext;
	RelUniqueCache = hash_create("columnar unique index cache", 16, &ctl,
								 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

/*
 * Decide how one key column is hashed. Returns true and fills *out when the
 * index's operator family for the column matches the type's default btree
 * opclass family (so the type's default hash is consistent with the index's
 * equality) and that type has a hash proc. Returns false to request the coarse
 * per-index fallback: a non-default opclass we cannot vouch for, or a type with
 * no hash support.
 */
static bool
columnar_key_col_hash(Relation indexRel, int c, MemoryContext cxt,
					  UniqueKeyCol *out)
{
	Oid			keyType = indexRel->rd_opcintype[c];
	Oid			indexFamily = indexRel->rd_opfamily[c];
	Oid			defaultOpc;
	TypeCacheEntry *tc;

	defaultOpc = GetDefaultOpClass(keyType, BTREE_AM_OID);
	if (!OidIsValid(defaultOpc))
		return false;

	/*
	 * Same operator family and input type => same equality operator, so the
	 * type's default hash (consistent with that equality) is safe. A different
	 * family (e.g. text_pattern_ops) may compare differently; coarsen.
	 */
	if (get_opclass_family(defaultOpc) != indexFamily)
		return false;

	tc = lookup_type_cache(keyType, TYPECACHE_HASH_PROC_FINFO);
	if (!OidIsValid(tc->hash_proc_finfo.fn_oid))
		return false;

	fmgr_info_copy(&out->hashFn, &tc->hash_proc_finfo, cxt);
	out->collation = indexRel->rd_indcollation[c];
	return true;
}

/* Build (or rebuild) the cache entry for rel. */
static RelUniqueCacheEntry *
columnar_unique_build(Relation rel)
{
	Oid			relid = RelationGetRelid(rel);
	MemoryContext cxt;
	MemoryContext old;
	RelUniqueCacheEntry *entry;
	List	   *indexOids;
	ListCell   *lc;
	int			nIndexes = 0;
	bool		found;

	columnar_unique_cache_init();

	cxt = AllocSetContextCreate(CacheMemoryContext,
								"columnar unique index info",
								ALLOCSET_SMALL_SIZES);
	old = MemoryContextSwitchTo(cxt);

	entry = (RelUniqueCacheEntry *) palloc0(sizeof(RelUniqueCacheEntry));
	entry->estate = CreateExecutorState();

	/* RelationGetIndexList returns a fresh list the caller owns */
	indexOids = RelationGetIndexList(rel);
	entry->indexes = (UniqueIndexEntry *)
		palloc0(Max(list_length(indexOids), 1) * sizeof(UniqueIndexEntry));

	foreach(lc, indexOids)
	{
		Oid			indexOid = lfirst_oid(lc);
		Relation	indexRel;
		Form_pg_index idxForm;
		UniqueIndexEntry *uidx;
		int			c;

		indexRel = index_open(indexOid, AccessShareLock);
		idxForm = indexRel->rd_index;

		/* only immediate, valid, ready, unique indexes enforce uniqueness */
		if (!idxForm->indisunique || !idxForm->indimmediate ||
			!idxForm->indisvalid || !idxForm->indisready)
		{
			index_close(indexRel, AccessShareLock);
			continue;
		}

		uidx = &entry->indexes[nIndexes];
		uidx->indexOid = indexOid;
		uidx->indexInfo = BuildIndexInfo(indexRel);
		uidx->predicate = uidx->indexInfo->ii_Predicate != NIL
			? ExecPrepareQual(uidx->indexInfo->ii_Predicate, entry->estate)
			: NULL;
		uidx->nKeyCols = idxForm->indnkeyatts;
#if PG_VERSION_NUM >= 150000
		uidx->nullsNotDistinct = idxForm->indnullsnotdistinct;
#else
		uidx->nullsNotDistinct = false;
#endif
		uidx->coarse = false;
		uidx->cols = (UniqueKeyCol *)
			palloc0(uidx->nKeyCols * sizeof(UniqueKeyCol));

		for (c = 0; c < uidx->nKeyCols; c++)
		{
			if (!columnar_key_col_hash(indexRel, c, cxt, &uidx->cols[c]))
			{
				uidx->coarse = true;
				break;
			}
		}

		nIndexes++;
		index_close(indexRel, AccessShareLock);
	}

	list_free(indexOids);
	entry->nIndexes = nIndexes;
	entry->relid = relid;
	entry->cxt = cxt;

	MemoryContextSwitchTo(old);

	{
		RelUniqueCacheEntry *slot;

		slot = (RelUniqueCacheEntry *) hash_search(RelUniqueCache, &relid,
												   HASH_ENTER, &found);
		if (found)
		{
			/* concurrent build within this backend: keep the existing one */
			MemoryContextDelete(cxt);
			return slot;
		}
		memcpy(slot, entry, sizeof(RelUniqueCacheEntry));
		slot->relid = relid;
		return slot;
	}
}

static RelUniqueCacheEntry *
columnar_unique_lookup(Relation rel)
{
	Oid			relid = RelationGetRelid(rel);
	RelUniqueCacheEntry *entry = NULL;

	if (RelUniqueCache != NULL)
		entry = (RelUniqueCacheEntry *) hash_search(RelUniqueCache, &relid,
													HASH_FIND, NULL);
	if (entry != NULL)
		return entry;

	return columnar_unique_build(rel);
}

/* -------------------------------------------------------------------------
 * lock-key computation and acquisition
 * ------------------------------------------------------------------------- */

/* splitmix64/murmur3 finalizer, matching rowmask_chunk_lock_key */
static inline uint64
columnar_hash_finalize(uint64 h)
{
	h ^= h >> 33;
	h *= UINT64CONST(0xff51afd7ed558ccd);
	h ^= h >> 33;
	h *= UINT64CONST(0xc4ceb9fe1a85ec53);
	h ^= h >> 33;
	return h;
}

static void
columnar_acquire_key_lock(Oid indexOid, uint32 bucket)
{
	LOCKTAG		tag;

	SET_LOCKTAG_ADVISORY(tag, MyDatabaseId, (uint32) indexOid, bucket,
						 COLUMNAR_UNIQUE_LOCK_CLASS);

	(void) LockAcquire(&tag, ExclusiveLock, false /* transaction lock */ ,
					   false /* wait */ );
}

/*
 * ColumnarLockUniqueKeys
 *		For a row about to be inserted into rel (through the table AM), take a
 *		transaction-scoped advisory lock for each applicable unique index's key,
 *		so a concurrent inserter of an equal key serializes behind this one until
 *		commit (issue #5). Idempotent within a backend: advisory locks are
 *		re-entrant, so a same-statement duplicate simply re-locks the same tag.
 *
 *		Handled per index:
 *		  - unique, immediate, valid, ready indexes only;
 *		  - partial indexes: locked only when the row satisfies the predicate;
 *		  - expression indexes: the indexed expression value is hashed;
 *		  - NULLS DISTINCT (default): a key with any NULL never conflicts and is
 *		    not locked; NULLS NOT DISTINCT (PG15+): NULLs are locked with a
 *		    canonical hash;
 *		  - multi-column keys: all key columns hashed and combined;
 *		  - an index whose opclass we cannot prove matches the type default
 *		    equality, or whose key type has no hash proc, falls back to a single
 *		    coarse per-index lock (over-serializes, always correct).
 */
void
ColumnarLockUniqueKeys(Relation rel, TupleTableSlot *slot)
{
	RelUniqueCacheEntry *entry;
	ExprContext *econtext;
	TupleTableSlot *saveScanTuple;
	uint32		numBuckets;
	int			i;

	if (!columnar_enable_unique_lock)
		return;

	entry = columnar_unique_lookup(rel);
	if (entry->nIndexes == 0)
		return;

	numBuckets = (uint32) Max(1, columnar_unique_lock_buckets);

	econtext = GetPerTupleExprContext(entry->estate);
	saveScanTuple = econtext->ecxt_scantuple;
	econtext->ecxt_scantuple = slot;

	for (i = 0; i < entry->nIndexes; i++)
	{
		UniqueIndexEntry *uidx = &entry->indexes[i];
		Datum		values[INDEX_MAX_KEYS];
		bool		isnull[INDEX_MAX_KEYS];
		bool		anyNull = false;
		uint32		bucket;
		MemoryContext oldcxt;
		int			c;

		ResetPerTupleExprContext(entry->estate);
		oldcxt = MemoryContextSwitchTo(GetPerTupleMemoryContext(entry->estate));

		/* a partial index only enforces uniqueness on rows it indexes */
		if (uidx->predicate != NULL && !ExecQual(uidx->predicate, econtext))
		{
			MemoryContextSwitchTo(oldcxt);
			continue;
		}

		FormIndexDatum(uidx->indexInfo, slot, entry->estate, values, isnull);

		for (c = 0; c < uidx->nKeyCols; c++)
		{
			if (isnull[c])
			{
				anyNull = true;
				break;
			}
		}

		/* NULLS DISTINCT: a key containing a NULL can never conflict */
		if (anyNull && !uidx->nullsNotDistinct)
		{
			MemoryContextSwitchTo(oldcxt);
			continue;
		}

		if (uidx->coarse)
		{
			bucket = 0;
		}
		else
		{
			uint64		combined = COLUMNAR_FNV_OFFSET;

			for (c = 0; c < uidx->nKeyCols; c++)
			{
				uint32		h;

				if (isnull[c])
					h = COLUMNAR_NULL_KEY_HASH;
				else
					h = DatumGetUInt32(FunctionCall1Coll(&uidx->cols[c].hashFn,
														 uidx->cols[c].collation,
														 values[c]));

				combined = (combined ^ (uint64) h) * COLUMNAR_FNV_PRIME;
			}

			combined = columnar_hash_finalize(combined);
			bucket = (uint32) (combined % (uint64) numBuckets);
		}

		MemoryContextSwitchTo(oldcxt);

		columnar_acquire_key_lock(uidx->indexOid, bucket);
	}

	econtext->ecxt_scantuple = saveScanTuple;
}

/*
 * ColumnarUniqueInit
 *		Register the relcache invalidation callback that keeps the per-relation
 *		unique-index cache coherent across DDL. Called once from _PG_init.
 */
void
ColumnarUniqueInit(void)
{
	CacheRegisterRelcacheCallback(columnar_unique_invalidate, (Datum) 0);
}
