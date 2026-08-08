/*-------------------------------------------------------------------------
 *
 * pgcolumnar_customscan.c
 *		Planner and executor integration for pgColumnar (spec 8.3, 9).
 *
 * A set_rel_pathlist_hook replaces the sequential-scan path of a columnar
 * base relation with a custom scan path. The custom scan reads only the
 * columns the query references (projection pushdown) and translates the
 * relation's restriction clauses into scan keys, which the reader uses to
 * skip chunk groups whose stored min/max prove they cannot match (qual
 * pushdown, spec 9). The executor always re-applies the full restriction
 * clauses as a filter, so chunk-group skipping is a pure optimization and
 * never changes results: a filtered query returns the same rows whether or
 * not columnar.enable_qual_pushdown is set.
 *
 * The scan is parallel-aware (gap 23): a parallel-aware partial custom path lets
 * the planner Gather over several workers that each claim distinct stripes from
 * a shared atomic counter in the scan's DSM segment.
 *
 * Independent MIT implementation built from
 * design/NATIVE_FORMAT_AND_INTERFACE_SPEC.md and the public PostgreSQL API (the
 * custom-scan provider contract in nodes/extensible.h and
 * executor/nodeCustom.c) only.
 *
 *-------------------------------------------------------------------------
 */
#include "columnar.h"

#include <math.h>

#include "access/htup_details.h"
#include "access/parallel.h"
#include "access/relation.h"
#include "access/relscan.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "storage/shm_toc.h"
#include "commands/explain.h"
#include "optimizer/optimizer.h"
#if PG_VERSION_NUM >= 180000
/* PG18 split the ExplainProperty* helpers out into explain_format.h. */
#include "commands/explain_format.h"
#endif
#include "executor/executor.h"
#include "nodes/execnodes.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "nodes/plannodes.h"
#include "nodes/value.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/restrictinfo.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

/* GUC: use the columnar custom scan path (spec 8.3) */
bool		pgcolumnar_enable_custom_scan = true;
/* GUC: let the planner scan a covering projection instead of the base (gap 26) */
bool		pgcolumnar_enable_projection_scan = true;
/* GUC: price a columnar index scan's per-row heap fetch (#355) */
bool		pgcolumnar_enable_index_fetch_penalty = true;

static set_rel_pathlist_hook_type prev_set_rel_pathlist_hook = NULL;

/* our executor-time scan state embeds CustomScanState as its first field */
typedef struct PgColumnarCustomScanState
{
	CustomScanState css;
	PgColumnarReadState *readState;
	Bitmapset  *projectedColumns;	/* 0-based; NULL means all columns */
	ScanKey		scanKeys;
	int			nScanKeys;
	int			nProjected;			/* for EXPLAIN */
	int			nTotalColumns;

	/* shared row-group claimer for a parallel scan (gap 23) */
	pg_atomic_uint32 *parallelCounter;

	/*
	 * Projection scan (gap 26, phase 4). When projScan is true this scan reads a
	 * projection's storage instead of the base: readState is opened on the
	 * projection's storage id with a synthetic [rownumber, cols...] descriptor,
	 * covered columns are mapped into the base-shaped output slot, and rows whose
	 * stored base row number is deleted/invisible are filtered via the liveness
	 * cache. Selection requires the projection to cover every referenced column,
	 * so no base reconstruction fetch is needed here.
	 */
	char	   *projName;			/* chosen projection name (EXPLAIN), or NULL */
	bool		projScan;

	/*
	 * Late materialization (#452 Phase 1a). qualCols marks the columns the node's
	 * qual reads; lateMat says the two-pass producer is in use for this scan. Both
	 * are settled at Begin, because whether it is legal cannot change mid-scan.
	 */
	bool	   *qualCols;
	bool		lateMat;
	int			projNcols;			/* projection column count (K) */
	int		   *projColMap;			/* base attno-1 -> index into projValues, or -1 */
	Datum	   *projValues;			/* scratch, length K+1 (index 0 = rownumber) */
	bool	   *projNulls;
	PgColumnarLivenessCache *livenessCache;	/* cached base liveness for the scan */
} PgColumnarCustomScanState;

/* path -> plan */
static Plan *PgColumnarPlanCustomPath(PlannerInfo *root, RelOptInfo *rel,
									CustomPath *best_path, List *tlist,
									List *clauses, List *custom_plans);

/* plan -> scan state (dispatches base vs vectorized-aggregate) */
static Node *PgColumnarCreateScanState(CustomScan *cscan);
static Node *PgColumnarCreateBaseScanState(CustomScan *cscan);

/* executor callbacks */
static void PgColumnarBeginCustomScan(CustomScanState *node, EState *estate,
									int eflags);
static TupleTableSlot *PgColumnarExecCustomScan(CustomScanState *node);
static void PgColumnarEndCustomScan(CustomScanState *node);
static void PgColumnarReScanCustomScan(CustomScanState *node);
static Size PgColumnarEstimateDSMCustomScan(CustomScanState *node,
										  ParallelContext *pcxt);
static void PgColumnarInitializeDSMCustomScan(CustomScanState *node,
											ParallelContext *pcxt,
											void *coordinate);
static void PgColumnarReInitializeDSMCustomScan(CustomScanState *node,
											  ParallelContext *pcxt,
											  void *coordinate);
static void PgColumnarInitializeWorkerCustomScan(CustomScanState *node,
											   shm_toc *toc, void *coordinate);
static void PgColumnarExplainCustomScan(CustomScanState *node, List *ancestors,
									  ExplainState *es);

/* ExecScan helpers */
static TupleTableSlot *PgColumnarScanNext(ScanState *ss);
static bool PgColumnarScanRecheck(ScanState *ss, TupleTableSlot *slot);

static const CustomPathMethods pgcolumnar_path_methods = {
	.CustomName = "PgColumnarScan",
	.PlanCustomPath = PgColumnarPlanCustomPath,
	.ReparameterizeCustomPathByChild = NULL,
};

const CustomScanMethods pgcolumnar_scan_methods = {
	.CustomName = "PgColumnarScan",
	.CreateCustomScanState = PgColumnarCreateScanState,
};

static const CustomExecMethods pgcolumnar_exec_methods = {
	.CustomName = "PgColumnarScan",
	.BeginCustomScan = PgColumnarBeginCustomScan,
	.ExecCustomScan = PgColumnarExecCustomScan,
	.EndCustomScan = PgColumnarEndCustomScan,
	.ReScanCustomScan = PgColumnarReScanCustomScan,
	.EstimateDSMCustomScan = PgColumnarEstimateDSMCustomScan,
	.InitializeDSMCustomScan = PgColumnarInitializeDSMCustomScan,
	.ReInitializeDSMCustomScan = PgColumnarReInitializeDSMCustomScan,
	.InitializeWorkerCustomScan = PgColumnarInitializeWorkerCustomScan,
	.ExplainCustomScan = PgColumnarExplainCustomScan,
};

/* -------------------------------------------------------------------------
 * planning
 * ------------------------------------------------------------------------- */

/*
 * pgcolumnar_projected_columns
 *		Build the 0-based set of columns the plan actually references, from the
 *		Vars in its target list and its restriction clauses. Returns NULL when
 *		a whole-row or system column is requested, meaning "all columns", so the
 *		reader stays correct for whole-row references and UPDATE/DELETE (which
 *		carry a ctid system Var). This is the projection pushed into the reader
 *		(spec 9).
 */
/*
 * PgColumnarProjectionFromAttnos
 *		Turn a set of needed attribute numbers, in pull_varattnos' offset form,
 *		into the reader's 0-based projection. Returns NULL for "read every
 *		column", which is what a system column, a whole-row Var, or an empty set
 *		all mean.
 *
 *		Shared because index_build_range_scan needs the same computation over a
 *		different source (#413): its columns come from IndexInfo rather than from
 *		a plan. The escapes are the interesting part and are worth having once.
 */
Bitmapset *
PgColumnarProjectionFromAttnos(Bitmapset *needed, int natts, int *nProjected)
{
	Bitmapset  *projected = NULL;
	int			attno;

	/* a system column or whole-row Var forces reading every column */
	for (attno = FirstLowInvalidHeapAttributeNumber + 1; attno <= 0; attno++)
	{
		if (bms_is_member(attno - FirstLowInvalidHeapAttributeNumber, needed))
		{
			*nProjected = natts;
			return NULL;
		}
	}

	for (attno = 1; attno <= natts; attno++)
	{
		if (bms_is_member(attno - FirstLowInvalidHeapAttributeNumber, needed))
			projected = bms_add_member(projected, attno - 1);
	}

	/*
	 * No column referenced at all (e.g. count(*)): read every column, which is
	 * correct if wasteful. Reported as "all" for EXPLAIN.
	 */
	if (projected == NULL)
	{
		*nProjected = natts;
		return NULL;
	}

	*nProjected = bms_num_members(projected);
	return projected;
}

static Bitmapset *
pgcolumnar_projected_columns(CustomScan *cscan, int natts, int *nProjected)
{
	Bitmapset  *needed = NULL;
	Index		scanrelid = cscan->scan.scanrelid;

	pull_varattnos((Node *) cscan->scan.plan.targetlist, scanrelid, &needed);
	pull_varattnos((Node *) cscan->scan.plan.qual, scanrelid, &needed);

	return PgColumnarProjectionFromAttnos(needed, natts, nProjected);
}


/*
 * pgcolumnar_commute_strategy
 *		The btree comparison strategy for "value op column" given the strategy
 *		for "column op value", used when the constant is on the left.
 */
static StrategyNumber
pgcolumnar_commute_strategy(StrategyNumber s)
{
	switch (s)
	{
		case BTLessStrategyNumber:
			return BTGreaterStrategyNumber;
		case BTLessEqualStrategyNumber:
			return BTGreaterEqualStrategyNumber;
		case BTEqualStrategyNumber:
			return BTEqualStrategyNumber;
		case BTGreaterEqualStrategyNumber:
			return BTLessEqualStrategyNumber;
		case BTGreaterStrategyNumber:
			return BTLessStrategyNumber;
		default:
			return InvalidStrategy;
	}
}

/*
 * pgcolumnar_clause_to_scankey
 *		Translate a single restriction clause of the form "column op const"
 *		(or "const op column") into a scan key for chunk-group skipping, when
 *		op is a btree comparison operator in the column type's default btree
 *		family and both sides share the column's type. Returns false for any
 *		other clause, so unsupported quals simply do not drive skipping; the
 *		executor still applies them as a filter, so results are unaffected.
 */
static bool
pgcolumnar_clause_to_scankey(Node *clause, Index scanrelid, TupleDesc tupdesc,
						   ScanKey key)
{
	OpExpr	   *op;
	Node	   *leftop;
	Node	   *rightop;
	Var		   *var;
	Const	   *con;
	bool		varOnLeft;
	TypeCacheEntry *tce;
	StrategyNumber strat;

	if (!IsA(clause, OpExpr))
		return false;
	op = (OpExpr *) clause;
	if (list_length(op->args) != 2)
		return false;

	leftop = (Node *) linitial(op->args);
	rightop = (Node *) lsecond(op->args);
	if (IsA(leftop, RelabelType))
		leftop = (Node *) ((RelabelType *) leftop)->arg;
	if (IsA(rightop, RelabelType))
		rightop = (Node *) ((RelabelType *) rightop)->arg;

	if (IsA(leftop, Var) && IsA(rightop, Const))
	{
		var = (Var *) leftop;
		con = (Const *) rightop;
		varOnLeft = true;
	}
	else if (IsA(rightop, Var) && IsA(leftop, Const))
	{
		var = (Var *) rightop;
		con = (Const *) leftop;
		varOnLeft = false;
	}
	else
		return false;

	if (var->varno != scanrelid)
		return false;
	if (var->varattno < 1 || var->varattno > tupdesc->natts)
		return false;
	if (con->constisnull)
		return false;

	/*
	 * The stored per-chunk min/max are ordered under the column's own
	 * collation (that is what the writer used, pgcolumnar_write_state.c), and the
	 * reader evaluates the skip under that same collation. Only push a
	 * predicate whose comparison uses that collation; otherwise a differently
	 * collated comparison (for example an explicit COLLATE in the query) could
	 * order the values differently and wrongly skip a group that in fact
	 * contains matching rows. When the collations differ we simply do not push
	 * the clause; the executor still applies it as a filter, so results are
	 * unaffected (spec 9).
	 */
	if (op->inputcollid !=
		TupleDescAttr(tupdesc, var->varattno - 1)->attcollation)
		return false;

	tce = lookup_type_cache(var->vartype, TYPECACHE_BTREE_OPFAMILY);
	if (!OidIsValid(tce->btree_opf))
		return false;

	strat = get_op_opfamily_strategy(op->opno, tce->btree_opf);
	if (strat == InvalidStrategy)
		return false;
	if (!varOnLeft)
		strat = pgcolumnar_commute_strategy(strat);
	if (strat == InvalidStrategy)
		return false;

	/*
	 * Fill the scan key directly. The reader (pgcolumnar_build_predicates) uses
	 * sk_attno, sk_strategy, sk_subtype and sk_argument, and looks up the
	 * column type's own comparison proc; it never calls sk_func, so we leave
	 * that zeroed rather than build one for a possibly-cross-type operator.
	 */
	MemSet(key, 0, sizeof(ScanKeyData));
	key->sk_flags = 0;
	key->sk_attno = var->varattno;
	key->sk_strategy = strat;
	key->sk_subtype = con->consttype;
	key->sk_collation = con->constcollid;
	key->sk_argument = con->constvalue;

	return true;
}

/*
 * PgColumnarBuildScanKeys
 *		Build the scan-key array for chunk-group skipping from a plan's
 *		restriction clauses (spec 9). Clauses that are not simple comparisons
 *		are skipped. Shared with the vectorized aggregate (pgcolumnar_vector.c).
 */
ScanKey
PgColumnarBuildScanKeys(List *qual, Index scanrelid, TupleDesc tupdesc,
					  int *nkeys)
{
	ScanKey		keys;
	ListCell   *lc;
	int			n = 0;

	*nkeys = 0;
	if (qual == NIL)
		return NULL;

	keys = (ScanKey) palloc0(sizeof(ScanKeyData) * list_length(qual));
	foreach(lc, qual)
	{
		if (pgcolumnar_clause_to_scankey((Node *) lfirst(lc), scanrelid, tupdesc,
									   &keys[n]))
			n++;
	}

	*nkeys = n;
	return keys;
}

/*
 * PgColumnarPlanCustomPath
 *		Convert the CustomPath to a CustomScan plan node. The restriction
 *		clauses become the scan's qual so the executor re-applies them (this is
 *		what makes chunk-group skipping safe); custom_scan_tlist stays NIL so
 *		the scan tuple is the full base-relation rowtype.
 */
static Plan *
PgColumnarPlanCustomPath(PlannerInfo *root, RelOptInfo *rel,
					   CustomPath *best_path, List *tlist,
					   List *clauses, List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);

	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = extract_actual_clauses(clauses, false);
	cscan->scan.scanrelid = rel->relid;
	cscan->flags = best_path->flags;
	cscan->custom_plans = custom_plans;
	cscan->custom_exprs = NIL;
	/* carry the chosen projection name (gap 26), if any, into the plan */
	cscan->custom_private = best_path->custom_private;
	cscan->custom_scan_tlist = NIL;
	cscan->methods = &pgcolumnar_scan_methods;

	return &cscan->scan.plan;
}

/*
 * pgcolumnar_choose_projection
 *		Pick a projection that serves this scan better than the base: it must
 *		cover every referenced column (so no base reconstruction is needed at
 *		scan time) and its leading sort column must appear in a restriction
 *		clause (so the sorted per-chunk min/max prunes tightly). Returns the
 *		projection name (palloc'd) or NULL. A system-column or whole-row
 *		reference disqualifies a projection scan.
 */
static char *
pgcolumnar_choose_projection(PlannerInfo *root, RelOptInfo *rel, Oid relid)
{
	uint64		storageId;
	Relation	r;
	List	   *projs;
	Bitmapset  *needed = NULL;
	Bitmapset  *restrictCols = NULL;
	ListCell   *lc;
	char	   *best = NULL;
	int			bestNcols = PG_INT32_MAX;
	int			x;
	bool		haveAdditional = false;

	if (!pgcolumnar_enable_projection_scan)
		return NULL;

	r = table_open(relid, AccessShareLock);
	storageId = PgColumnarStorageId(r);
	table_close(r, AccessShareLock);

	projs = PgColumnarListProjections(storageId);
	foreach(lc, projs)
		if (((PgColumnarProjection *) lfirst(lc))->projectionId > 0)
			haveAdditional = true;
	if (!haveAdditional)
		return NULL;

	pull_varattnos((Node *) rel->reltarget->exprs, rel->relid, &needed);
	foreach(lc, rel->baserestrictinfo)
	{
		RestrictInfo *ri = (RestrictInfo *) lfirst(lc);

		pull_varattnos((Node *) ri->clause, rel->relid, &needed);
		pull_varattnos((Node *) ri->clause, rel->relid, &restrictCols);
	}

	x = -1;
	while ((x = bms_next_member(needed, x)) >= 0)
		if (x + FirstLowInvalidHeapAttributeNumber <= 0)
			return NULL;		/* system column or whole-row reference */

	foreach(lc, projs)
	{
		PgColumnarProjection *p = (PgColumnarProjection *) lfirst(lc);
		bool		covers = true;
		bool		skips;
		int			y;

		if (p->projectionId == 0)
			continue;

		y = -1;
		while ((y = bms_next_member(needed, y)) >= 0)
		{
			int			attno = y + FirstLowInvalidHeapAttributeNumber;
			bool		found = false;
			int			k;

			for (k = 0; k < p->columnsLen; k++)
				if ((int) p->columns[k] == attno)
				{
					found = true;
					break;
				}
			if (!found)
			{
				covers = false;
				break;
			}
		}
		if (!covers)
			continue;

		skips = (p->sortKeyLen > 0 &&
				 bms_is_member(p->sortKey[0] - FirstLowInvalidHeapAttributeNumber,
							   restrictCols));
		if (!skips)
			continue;

		if (p->columnsLen < bestNcols)
		{
			best = pstrdup(p->name);
			bestNcols = p->columnsLen;
		}
	}

	return best;
}

/*
 * pgcolumnar_index_correlation
 *		|correlation| of the index's leading key against heap (row-number) order,
 *		read from pg_statistic exactly as btcostestimate does (selfuncs.c). A value
 *		near 1 means rows an ordered index scan visits are already clustered into a
 *		few row groups; near 0 means they are scattered across all of them.
 *
 * We take only the leading key and, for a multi-column index, damp it: trailing
 * keys reorder within a leading-key run, so the run's rows land less tightly than
 * the leading correlation alone suggests. Returns 0.0 (treat as scattered, the
 * conservative-for-us direction) whenever a statistic is missing -- an expression
 * key, no ANALYZE, no correlation slot.
 */
static double
pgcolumnar_index_correlation(IndexOptInfo *index, Oid heapRelid)
{
	AttrNumber	attno;
	Oid			sortop;
	HeapTuple	st;
	AttStatsSlot sslot;
	double		corr = 0.0;

	if (index->indexkeys == NULL || index->nkeycolumns < 1)
		return 0.0;
	attno = index->indexkeys[0];
	if (attno <= 0)				/* 0 == expression key, no column statistic */
		return 0.0;

	sortop = get_opfamily_member(index->opfamily[0], index->opcintype[0],
								 index->opcintype[0], BTLessStrategyNumber);
	if (!OidIsValid(sortop))
		return 0.0;

	st = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(heapRelid),
						 Int16GetDatum(attno), BoolGetDatum(false));
	if (!HeapTupleIsValid(st))
		return 0.0;

	if (get_attstatsslot(&sslot, st, STATISTIC_KIND_CORRELATION, sortop,
						 ATTSTATSSLOT_NUMBERS))
	{
		if (sslot.nnumbers == 1)
		{
			corr = sslot.numbers[0];
			if (index->reverse_sort[0])
				corr = -corr;
			if (index->nkeycolumns > 1)
				corr *= 0.75;
		}
		free_attstatsslot(&sslot);
	}
	ReleaseSysCache(st);
	return corr;
}

/*
 * pgcolumnar_scan_decode_shape
 *		How much a by-row-number fetch of this rel actually decodes: the number of
 *		columns and their summed width.
 *
 *		Not the columns the scan emits. The deferred index-fetch slot decodes the
 *		attribute *prefix* 0..max-referenced, because slot_getsomeattrs asks for a
 *		prefix and cannot ask for a set (pgcolumnar_tableam.c,
 *		pgcolumnar_slot_decode_upto). A query referencing only a late column therefore
 *		decodes every column before it, and sizing this from reltarget -- the
 *		emitted columns -- understates the decode by the ratio of the prefix to the
 *		projection (issue #363).
 *
 *		Measured on a ten-text-column table, same 300 fetched rows, same emitted
 *		width, same plan: max(a1) 975 ms against max(a10) 194,798 ms. The two sit on
 *		opposite sides of the fetch cache cap while reltarget->width is identical
 *		for both, so the cap-crossing branch below could not tell them apart.
 *
 *		Widths come from pg_statistic where ANALYZE has run and from the type's
 *		average width otherwise, the way set_rel_width does it, because the
 *		unreferenced columns in the prefix are not in reltarget at all.
 */
static void
pgcolumnar_scan_decode_shape(RelOptInfo *rel, Index rti, Oid relid,
						   int *nprefix, double *prefixWidth)
{
	Bitmapset  *attrs = NULL;
	ListCell   *lc;
	int			maxatt = 0;
	int			x;
	double		width = 0.0;
	int			i;

	pull_varattnos((Node *) rel->reltarget->exprs, rti, &attrs);
	foreach(lc, rel->baserestrictinfo)
		pull_varattnos((Node *) lfirst_node(RestrictInfo, lc)->clause, rti, &attrs);

	x = -1;
	while ((x = bms_next_member(attrs, x)) >= 0)
	{
		AttrNumber	att = x + FirstLowInvalidHeapAttributeNumber;

		/*
		 * A whole-row reference reads every column, so the prefix is the whole
		 * tuple. System columns cost no decode and are ignored.
		 */
		if (att == InvalidAttrNumber)
		{
			maxatt = rel->max_attr;
			break;
		}
		if (att > maxatt)
			maxatt = att;
	}
	bms_free(attrs);

	if (maxatt < 1)
	{
		*nprefix = 1;
		*prefixWidth = (rel->reltarget->width > 0) ? rel->reltarget->width : 1.0;
		return;
	}

	for (i = 1; i <= maxatt; i++)
	{
		int32		w = get_attavgwidth(relid, i);

		if (w <= 0)
		{
			Oid			typid;
			int32		typmod;
			Oid			collid;

			get_atttypetypmodcoll(relid, i, &typid, &typmod, &collid);

			/* a dropped column occupies its place in the prefix but decodes nothing */
			if (!OidIsValid(typid))
				continue;
			w = get_typavgwidth(typid, typmod);
		}
		width += (double) w;
	}

	*nprefix = maxatt;
	*prefixWidth = (width > 0.0) ? width : 1.0;
}

/*
 * pgcolumnar_index_fetch_penalty
 *		extra cost to add to a heap-fetching columnar index scan for the row-group
 *		decodes its per-row fetches force. Core's cost_index prices a heap fetch as
 *		a page or two; a columnar fetch decodes the whole row group the row lives in
 *		(pgcolumnar_reader.c, PgColumnarReadRowByNumber), which is why the planner picks
 *		an index scan for an unclustered ORDER BY and then runs for minutes (#355).
 *
 * A statement-scoped cache (issue #143) means a group is decoded once per scan, not
 * once per row, so the count that matters is how many *distinct* groups the fetched
 * rows fall into:
 *
 *   - fully clustered (rho -> 1, or a TID-ordered bitmap heap scan): the rows sit in
 *     ceil(rows / R) adjacent groups, the floor.
 *   - fully scattered (rho -> 0): every fetched row can land in its own group, up to
 *     one decode per row, the ceiling.
 *   - between: interpolate on rho^2, the share of ordering variance the correlation
 *     explains, matching how cost_index already blends min and max page cost.
 *
 * The one exception is the fetch cache cliff (#359): when one group's decoded width
 * exceeds the cache cap it is never retained, so every fetch re-decodes regardless of
 * clustering -- the ceiling, unconditionally. (When #359 makes that overflow
 * proportional rather than total, this branch should soften with it.)
 *
 * decode_per_group is one group's cost: its pages read once, plus the per-value
 * decode of R rows across the columns the scan needs.
 */
static Cost
pgcolumnar_index_fetch_penalty(RelOptInfo *rel, double rows, double rho,
							 int nproj, double decodedWidth, bool tid_ordered)
{
	double		R = (double) pgcolumnar_stripe_row_limit;
	double		N = (rel->tuples > 0) ? rel->tuples : rows;
	double		n_groups,
				pages_per_stripe,
				decode_per_group;
	double		groups_min,
				groups_max,
				groups_decoded,
				decoded_width,
				csq;

	if (rows <= 0 || R < 1)
		return 0.0;

	n_groups = ceil(N / R);
	if (n_groups < 1)
		n_groups = 1;
	pages_per_stripe = ceil((double) rel->pages / n_groups);
	if (pages_per_stripe < 1)
		pages_per_stripe = 1;
	decode_per_group = seq_page_cost * pages_per_stripe
		+ cpu_operator_cost * R * (double) nproj;

	groups_min = ceil(rows / R);
	groups_max = rows;
	if (tid_ordered)
		groups_decoded = groups_min;
	else
	{
		csq = rho * rho;
		if (csq > 1.0)
			csq = 1.0;
		groups_decoded = groups_min + (groups_max - groups_min) * (1.0 - csq);
	}

	/*
	 * A group too wide to hold entirely in the fetch cache re-decodes the part
	 * that did not fit, once per fetch rather than once per group.
	 *
	 * This was a cliff -- the whole entry was dropped, so exceeding the cap by
	 * any margin meant every group re-decoded, and the model said so. #359 made
	 * the cache admit columns until the cap and re-decode only the remainder, so
	 * the extra decoding is now the overflow *fraction* of the projection. Model
	 * it the same way: blend between decoding each group once and decoding one
	 * per fetch, by how much of the decoded group does not fit.
	 */
	decoded_width = decodedWidth * R;
	if (decoded_width > (double) COLUMNAR_FETCH_CACHE_MAX_BYTES)
	{
		double		resident = (double) COLUMNAR_FETCH_CACHE_MAX_BYTES /
			decoded_width;

		groups_decoded += (groups_max - groups_decoded) * (1.0 - resident);
	}

	if (groups_decoded < 0)
		groups_decoded = 0;
	return groups_decoded * decode_per_group;
}

/*
 * How far above one full scan a fetching index path may be priced (issue #376).
 * See pgcolumnar_penalize_index_fetches for why this is a bound rather than a
 * better model, and for the two measurements that fix the window it sits in.
 */
#define COLUMNAR_INDEX_FETCH_PENALTY_MAX_SCANS	20.0

/*
 * pgcolumnar_full_scan_cost
 *		What one full scan of this relation costs.
 *
 *		The seqscan's own number when core still has one, and the same work priced
 *		directly when it does not -- add_path frees the seqscan whenever an index
 *		path dominates it, which is exactly the selective queries. One definition,
 *		two callers: the columnar path's own cost and the bound on the fetch
 *		penalty below.
 */
static Cost
pgcolumnar_full_scan_cost(RelOptInfo *rel, Path *seqpath)
{
	QualCost	qcost;
	double		ntuples;
	Cost		run;

	if (seqpath != NULL)
		return seqpath->total_cost;

	qcost = rel->baserestrictcost;
	ntuples = (rel->tuples >= 0) ? rel->tuples : rel->rows;
	run = seq_page_cost * (double) rel->pages;
	run += (cpu_tuple_cost + qcost.per_tuple) * ntuples;
	return qcost.startup + run;
}

/*
 * pgcolumnar_projected_width_fraction
 *		The share of a row's stored width this scan actually reads, in (0, 1].
 *		One means every column is referenced.
 *
 *		A columnar scan decodes only the columns the query names (spec 9), which
 *		is the reason to store data this way at all. The cost model did not know
 *		it: the path inherits the sequential scan's cost, which prices every page
 *		of the relation, so reading one int column out of fourteen was quoted the
 *		same as reading all fourteen. Measured in test/column_projection.sh on
 *		300,000 rows: 221 buffers against 2,671, priced 1391.08 against 1391.44.
 *
 *		The consequence is not a rounding error. On a 200,000-row table with a
 *		2 KB payload, an index-only scan priced 15,521 ran 373 ms while the
 *		columnar scan priced 27,682 ran 8 ms -- the plan 44x faster was declined
 *		because it was quoted as reading a relation it never touches.
 *
 *		Width comes from ANALYZE's stored average where there is one, because a
 *		varlena column's TYPE width says nothing useful (a text column is "we do
 *		not know"), and the whole point here is the ratio between a fixed-width
 *		key and a wide payload. get_typavgwidth is the fallback for a column
 *		never analysed.
 *
 *		Both the target list and the restriction clauses count: a qual on a
 *		column that is not selected still has to be decoded to be evaluated.
 */
static double
pgcolumnar_projected_width_fraction(RelOptInfo *rel, Oid heapRelid)
{
	Bitmapset  *needed = NULL;
	Relation	r;
	TupleDesc	tupdesc;
	double		total = 0.0;
	double		used = 0.0;
	int			i;
	ListCell   *lc;

	pull_varattnos((Node *) rel->reltarget->exprs, rel->relid, &needed);
	foreach(lc, rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

		pull_varattnos((Node *) rinfo->clause, rel->relid, &needed);
	}

	r = table_open(heapRelid, AccessShareLock);
	tupdesc = RelationGetDescr(r);

	for (i = 1; i <= tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i - 1);
		int32		w;

		if (att->attisdropped)
			continue;

		w = get_attavgwidth(heapRelid, att->attnum);
		if (w <= 0)
			w = get_typavgwidth(att->atttypid, att->atttypmod);
		if (w <= 0)
			w = 1;

		total += w;
		if (bms_is_member(i - FirstLowInvalidHeapAttributeNumber, needed))
			used += w;
	}

	table_close(r, AccessShareLock);
	bms_free(needed);

	if (total <= 0.0)
		return 1.0;

	/*
	 * A scan that names no column at all -- `SELECT count(*)` with no
	 * restriction -- still reads something: the row count comes from metadata,
	 * but nothing here should price a scan at zero. #171 is what a
	 * floorless discount does: a full scan priced at 1.00 beat an index scan
	 * priced at 174.29 and turned a point lookup into 1251.88 ms.
	 */
	if (used <= 0.0)
		return 1.0;

	return (used >= total) ? 1.0 : (used / total);
}

/*
 * pgcolumnar_zonemap_survival
 *		The fraction of the relation a restricted columnar scan must actually
 *		read, in (0, 1]. One means no pruning is expected.
 *
 *		The scan skips a row group whose stored minimum and maximum prove the
 *		restriction cannot match, so what it reads is decided by how the matching
 *		values are LAID OUT, not only by how many there are. Perfectly clustered,
 *		a predicate selecting a fraction s of the rows touches about s of the
 *		groups. Perfectly scattered, every group holds the full value range and
 *		nothing prunes at all.
 *
 *		Interpolating on rho^2 is the same relation and the same curve the index
 *		fetch penalty uses above. It has to be: the penalty says a clustered index
 *		fetch decodes few groups, and this says a clustered restriction skips many
 *		groups. Those are one physical fact, and pricing them on two different
 *		curves is how the planner ends up preferring a plan it also believes is
 *		expensive.
 *
 *		Why this exists (#434). Without it the columnar path is quoted at its
 *		worst case while an index scan on the same relation is quoted at its best,
 *		so the planner picked a plan that ran 11.6x slower than one it declined:
 *		index scan priced 12,407 and ran 500 ms, custom scan priced 17,326 and ran
 *		43 ms, on a correlated bigint key with 80% of rows excluded.
 *
 *		The floor is not decoration. Issue #171 was a full scan priced at 1.00
 *		beating an index scan priced at 174.29, which turned a point lookup into
 *		1251.88 ms; a discount with no floor is the same defect with a different
 *		multiplier. Never discount below one row group's share, because a scan
 *		that matches anything at all must read the group the match is in.
 */
static double
pgcolumnar_zonemap_survival(RelOptInfo *rel, Oid heapRelid)
{
	double		survival;
	double		floorFrac;
	double		groups;

	if (rel->baserestrictinfo == NIL || rel->tuples <= 0)
		return 1.0;

	/*
	 * One row group's share, from the group size THIS TABLE was written with.
	 *
	 * The first version read pgcolumnar_stripe_row_limit, the GUC, on the
	 * reasoning that a catalog lookup at plan time would be paid by every query.
	 * The reasoning was right and the value was wrong: stripe_row_limit is a
	 * per-table option that overrides the GUC, and it is the one that decides how
	 * many groups exist. On a table written at 10,000 rows per group with the GUC
	 * left at its default, this computed 2 groups where there were 20, so the
	 * floor came out at 0.5 and clamped every case. The formula above never ran:
	 * sel = 0.053 with rho = 1 was reported as a survival of 0.5. Measured with
	 * the function instrumented, because reading it off the plan looked like the
	 * model working.
	 *
	 * The lookup is placed after the early returns above, so it happens only when
	 * a discount is actually about to be applied -- a relation with no prunable
	 * restriction never pays for it. That is once per baserel per plan, against a
	 * cost model that is otherwise wrong by an order of magnitude.
	 */
	{
		PgColumnarOptions opts;
		int			limit = pgcolumnar_stripe_row_limit;

		if (PgColumnarReadOptions(heapRelid, &opts) &&
			opts.stripeRowLimitSet && opts.stripeRowLimit > 0)
			limit = opts.stripeRowLimit;
		if (limit <= 0)
			return 1.0;
		groups = ceil(rel->tuples / (double) limit);
	}
	if (groups < 1.0)
		return 1.0;

	/*
	 * Measure, rather than infer from correlation (#461).
	 *
	 * This read pg_stats.correlation and interpolated on rho^2. Correlation is a
	 * GLOBAL agreement between value order and physical order; pruning is LOCAL,
	 * needing only each group's own min and max to be narrow. The two come apart
	 * exactly where this engine is aimed: batch-loaded time-series is globally
	 * unsorted and locally tight, and #391 measured a TSBS `time` column with
	 * correlation 0.0133 removing 82 percent of chunk groups. Under the old model
	 * that column earned nothing. #381 made this same mistake in documentation and
	 * #391 corrected it; the planner then reintroduced it.
	 *
	 * So ask the reader's question with the reader's own code: build the same skip
	 * predicates and evaluate them against a bounded sample of the groups' zone
	 * maps. A proxy's error direction is unknowable, a sample's is just sampling
	 * error, and a discount taken by a different rule than the one that priced it
	 * is how a plan gets chosen for a saving it never realises.
	 */
	{
		Relation	r = table_open(heapRelid, AccessShareLock);

		/*
		 * extract_actual_clauses(..., false), NOT get_actual_clauses.
		 *
		 * get_actual_clauses carries Assert(!rinfo->pseudoconstant), and a
		 * pseudoconstant qual is not exotic: `WHERE (SELECT false)` produces one,
		 * as a one-time filter. On an assert-enabled build that took down two
		 * native_groupagg checks with a bare QUERY_ERROR. Excluding them is also
		 * right on the merits -- a one-time filter is evaluated once for the whole
		 * scan and prunes no chunk group, so it has nothing to contribute here.
		 */
		survival = PgColumnarEstimatePruneSurvival(PgColumnarStorageId(r),
												   RelationGetDescr(r),
												   extract_actual_clauses(rel->baserestrictinfo,
																		  false),
												   rel->relid,
												   (uint64) groups,
												   PGCOLUMNAR_PRUNE_SAMPLE_GROUPS);
		table_close(r, AccessShareLock);
	}

	/*
	 * The floor is not decoration, and it matters more now than it did. A sample
	 * that happens to hit only excluded groups reports a survival of zero, and a
	 * scan matching anything at all must still read the group its match is in.
	 * #171 was a full scan priced at 1.00 beating an index scan priced at 174.29,
	 * turning a point lookup into 1251.88 ms.
	 */
	floorFrac = (groups >= 1.0) ? (1.0 / groups) : 1.0;
	if (survival < floorFrac)
		survival = floorFrac;

	return (survival > 1.0) ? 1.0 : survival;
}

/*
 * pgcolumnar_path_order_cmp
 *		Order two paths the way add_path keeps rel->pathlist ordered.
 *
 *		PG18 sorts by disabled_nodes and then total_cost; before that there is no
 *		disabled_nodes field and the order is total_cost alone. Getting this wrong
 *		does not fail to compile -- it silently hands add_path a list ordered by the
 *		wrong key, so it is spelled out per version rather than assumed.
 */
static int
pgcolumnar_path_order_cmp(const ListCell *a, const ListCell *b)
{
	const Path *pa = (const Path *) lfirst(a);
	const Path *pb = (const Path *) lfirst(b);

#if PG_VERSION_NUM >= 180000
	if (pa->disabled_nodes != pb->disabled_nodes)
		return (pa->disabled_nodes < pb->disabled_nodes) ? -1 : 1;
#endif
	if (pa->total_cost < pb->total_cost)
		return -1;
	if (pa->total_cost > pb->total_cost)
		return 1;
	return 0;
}

/*
 * pgcolumnar_penalize_index_fetches
 *		Price the per-row heap fetch of the surviving index and bitmap paths
 *		(#355), and restore the ordering add_path expects.
 *
 *		This must run BEFORE the columnar paths are offered to add_path, not after
 *		(issue #362). add_path frees a path it judges dominated, so a columnar path
 *		offered while the index paths still carry their un-penalized costs is
 *		discarded there and then; raising those costs afterwards changes what
 *		EXPLAIN prints and leaves the planner with nothing to switch to. Measured
 *		before the reorder: the planner chose an index scan it priced at 13,954,742
 *		over a columnar path it priced at 589,348 -- one it could no longer see --
 *		and ran 224 s where the columnar path runs 4.7 s.
 *
 *		The reason it used to run last was that mutating total_cost in place unsorts
 *		rel->pathlist and no add_path may see an unsorted list. That is real, and it
 *		is why the list is re-sorted here rather than left as the mutation leaves it.
 *		At this point the list holds only index and bitmap paths -- the seqscans have
 *		just been dropped -- so the sort is over a handful of entries.
 *
 *		Only non-parameterized paths are touched. A parameterized index scan is a
 *		nested-loop inner side rescanned per outer row; the fetch cache spans those
 *		rescans (it is released at executor end, not per rescan), so the distinct-
 *		group count this model assumes for a single pass understates the reuse and
 *		would over-penalize the join. #355 is the standalone ordering/lookup case,
 *		which is where param_info is NULL.
 *
 *		total_cost only, never startup_cost: the fetch cost is paid as rows are
 *		pulled, so a LIMIT that stops the scan early pays proportionally, which the
 *		planner models by fractioning (total - startup).
 */
static void
pgcolumnar_penalize_index_fetches(RelOptInfo *rel, Index rti, Oid relid,
								Cost fullScanCost)
{
	int			nproj;
	double		decodedWidth;
	Cost		cap;
	bool		mutated = false;
	ListCell   *lc;

	if (!pgcolumnar_enable_index_fetch_penalty)
		return;

	pgcolumnar_scan_decode_shape(rel, rti, relid, &nproj, &decodedWidth);

	/*
	 * The penalty is bounded by a multiple of one full scan (issue #376).
	 *
	 * The model prices a fetch as a row-group decode and multiplies by the rows
	 * the path returns. That is right when the plan above consumes the whole
	 * path, and it over-counts without limit when the plan above stops early: a
	 * DISTINCT ON reads one row per group, and a skip scan over the index reads
	 * 3,998 rows of 100,000,000. The un-bounded penalty reached 502,598,685,066
	 * on that query -- 207,000 times the un-penalized path -- so even the 1/25000
	 * of it that the skip scan fractions to still lost to a full scan and a sort.
	 * Measured: 44,058 ms for the plan chosen, 769 ms for the one refused.
	 *
	 * Why bound rather than model it better: the two cases cannot be told apart
	 * from this hook. Both have a leading-key correlation of about zero. What
	 * differs is which groups the fetched rows land in, and that is not knowable
	 * before the consumer exists. Measured both ways on the same shape -- a
	 * consumer that reads every row makes the penalty right by 36x (4,710 ms
	 * against 170,965), and one that stops early makes it wrong by 57x.
	 *
	 * Past some multiple of a full scan the number stops carrying information a
	 * planner can use. Any path priced above the scan already loses to it, so
	 * further inflation only harms a consumer that fractions the path. The bound
	 * keeps the direction and drops the part that only does damage.
	 *
	 * The multiple is empirical, not derived. It is chosen to sit inside a window
	 * both measurements agree on: the early-stopping case needs the penalty cut by
	 * at least 1.45x, and the read-everything case tolerates roughly 16,000x
	 * before it picks the wrong plan. Twenty is near the conservative end of that
	 * window, so the penalty keeps steering where it was steering correctly.
	 * test/analyze_stats.sh pins both directions.
	 */
	cap = COLUMNAR_INDEX_FETCH_PENALTY_MAX_SCANS * fullScanCost;

	foreach(lc, rel->pathlist)
	{
		Path	   *p = (Path *) lfirst(lc);
		Cost		add = 0.0;

		if (p->param_info != NULL)
			continue;
		if (p->pathtype == T_IndexScan)
		{
			IndexPath  *ip = castNode(IndexPath, p);
			double		rho = pgcolumnar_index_correlation(ip->indexinfo, relid);

			add = pgcolumnar_index_fetch_penalty(rel, p->rows, rho, nproj,
											   decodedWidth, false);
		}
		else if (p->pathtype == T_BitmapHeapScan)
		{
			/* a bitmap heap scan fetches in TID (row-number) order */
			add = pgcolumnar_index_fetch_penalty(rel, p->rows, 1.0, nproj,
											   decodedWidth, true);
		}
		/* T_IndexOnlyScan and the custom scans do no heap fetch */

		/* never price a fetching path above the bound (issue #376) */
		if (add > 0.0 && cap > 0.0)
		{
			if (p->total_cost >= cap)
				add = 0.0;
			else if (p->total_cost + add > cap)
				add = cap - p->total_cost;
		}

		if (add > 0.0)
		{
			p->total_cost += add;
			mutated = true;
		}
	}

	if (mutated)
		list_sort(rel->pathlist, pgcolumnar_path_order_cmp);
}

/*
 * PgColumnarSetRelPathlist
 *		set_rel_pathlist_hook: for a columnar base relation, replace the
 *		sequential-scan path with the columnar custom scan and drop parallel
 *		paths. Index and bitmap paths are left in place, so ordinary index
 *		scans still compete. The custom scan inherits the sequential scan's
 *		cost (which already reflects enable_seqscan), so SET enable_seqscan=off
 *		steers the planner to an index scan exactly as before.
 */
static void
PgColumnarSetRelPathlist(PlannerInfo *root, RelOptInfo *rel, Index rti,
					   RangeTblEntry *rte)
{
	CustomPath *cpath;
	Cost		serialStartupCost;
	Cost		serialTotalCost;
	Path	   *seqpath = NULL;
	List	   *keep = NIL;
	ListCell   *lc;

	if (prev_set_rel_pathlist_hook)
		prev_set_rel_pathlist_hook(root, rel, rti, rte);

	if (!pgcolumnar_enable_custom_scan)
		return;
	if (rte->rtekind != RTE_RELATION || rte->relkind != RELKIND_RELATION)
		return;
	if (rel->reloptkind != RELOPT_BASEREL)
		return;
	if (!OidIsValid(rte->relid) || !PgColumnarIsColumnarRelation(rte->relid))
		return;

	/*
	 * The custom scan is the scalar per-row path (PgColumnarReadNextRow), so
	 * pushed-down predicates drive zone-map row-group and per-vector skipping
	 * (native spec 7.1). Ungrouped aggregates are answered from the zone maps by
	 * the separate aggregate path (pgcolumnar_vector.c).
	 */

	/* find a non-parameterized seqscan path to inherit its costs from */
	foreach(lc, rel->pathlist)
	{
		Path	   *p = (Path *) lfirst(lc);

		if (p->pathtype == T_SeqScan && p->param_info == NULL)
			seqpath = p;
	}

	/* drop every seqscan path; keep index/bitmap/other paths */
	foreach(lc, rel->pathlist)
	{
		Path	   *p = (Path *) lfirst(lc);

		if (p->pathtype != T_SeqScan)
			keep = lappend(keep, p);
	}
	rel->pathlist = keep;

	/* drop the seqscan partial paths; a columnar partial path is added below */
	rel->partial_pathlist = NIL;

	/*
	 * Price the index/bitmap fetches now, while the only paths in the list are
	 * the ones core built, and before any columnar path is offered below. #362:
	 * doing this after the add_path calls meant the columnar path was judged
	 * against index costs that had not yet been penalized, and freed.
	 */
	pgcolumnar_penalize_index_fetches(rel, rti, rte->relid,
								   pgcolumnar_full_scan_cost(rel, seqpath));

	cpath = makeNode(CustomPath);
	cpath->path.pathtype = T_CustomScan;
	cpath->path.parent = rel;
	cpath->path.pathtarget = rel->reltarget;
	cpath->path.param_info = NULL;
	cpath->path.parallel_aware = false;
	cpath->path.parallel_safe = false;
	cpath->path.parallel_workers = 0;
	cpath->path.rows = rel->rows;

	/*
	 * Inherit the sequential scan's cost when there is one, since this path
	 * reads the same relation and the comparison against every other path
	 * should turn on what it does differently, not on a different cost model.
	 *
	 * When there is none, cost the work: every page read once and the
	 * restriction evaluated on every row. The fallback used to be rel->rows,
	 * which is an output row count and not a cost at all.
	 *
	 * That fallback was not the rare case it looks like. add_path frees a path
	 * it finds dominated, so the seqscan is gone from rel->pathlist by the time
	 * this hook runs exactly when some index path beat it on both cost and
	 * pathkeys -- which is to say, precisely on the selective lookups where
	 * using the index matters most. Before ANALYZE the row estimate was a large
	 * default and the resulting "cost" was accidentally large enough to lose. Once
	 * ANALYZE supplied real statistics (#159), a selective predicate estimated
	 * one row, so a full scan of the table was priced at 1.00, beat an index scan
	 * of the same query costed at 174.29, and the planner stopped using the index.
	 * That is issue #171: a point lookup went from 23.75 ms to 1251.88 ms the
	 * moment the table had statistics. Regression-tested in test/analyze_stats.sh.
	 */
	cpath->path.startup_cost = (seqpath != NULL) ? seqpath->startup_cost
		: rel->baserestrictcost.startup;

	/*
	 * The run cost is scaled by what the zone maps leave to read (#434). Only
	 * the run part: the startup is qual setup and is paid whatever gets skipped.
	 */
	{
		Cost		full = pgcolumnar_full_scan_cost(rel, seqpath);
		double		survival = pgcolumnar_zonemap_survival(rel, rte->relid);
		double		widthFrac = pgcolumnar_projected_width_fraction(rel, rte->relid);
		Cost		pageCost = seq_page_cost * (double) rel->pages;
		Cost		run = full - cpath->path.startup_cost;

		/*
		 * Only the page-read part scales with the projected width: the CPU terms
		 * are per row and are paid whichever columns are decoded. Subtracting the
		 * unread share of the I/O rather than scaling the whole run cost keeps a
		 * narrow projection from being priced below the row-handling it still
		 * does, which is the shape of #171.
		 *
		 * Guarded rather than assumed: rel->pages can exceed what the inherited
		 * seqscan cost accounts for, and a negative run cost would make a full
		 * scan free.
		 */
		if (pageCost > 0.0 && widthFrac < 1.0)
		{
			Cost		saved = pageCost * (1.0 - widthFrac);

			if (saved > run)
				saved = run;
			run -= saved;
		}

		cpath->path.total_cost = cpath->path.startup_cost + run * survival;
	}
	cpath->path.pathkeys = NIL;
	cpath->flags = 0;
	cpath->custom_paths = NIL;
	cpath->custom_private = NIL;
#if PG_VERSION_NUM >= 170000
	/*
	 * PG17+ lets the path declare which restriction clauses it carries into the
	 * plan; core hands exactly these to PlanCustomPath as its "clauses". Before
	 * PG17 there is no such field and core passes the relation's
	 * baserestrictinfo, which is the same list, so the plan is identical.
	 */
	cpath->custom_restrictinfo = rel->baserestrictinfo;
#endif
	cpath->methods = &pgcolumnar_path_methods;

	/*
	 * Keep the costs before offering the path. add_path FREES a path it judges
	 * dominated, so cpath is not safe to read afterwards -- the parallel path
	 * below is costed from these copies rather than from cpath. Reading the
	 * struct after add_path is a use-after-free that shows up as a zero-cost
	 * path rather than a crash, which is how it was found: a point lookup came
	 * back as "Parallel Custom Scan ... (cost=0.00..0.00)" instead of an index
	 * scan.
	 */
	serialStartupCost = cpath->path.startup_cost;
	serialTotalCost = cpath->path.total_cost;

	add_path(rel, &cpath->path);

	/*
	 * Offer a projection scan (gap 26) as a competing path when a covering
	 * projection with a restricted sort key exists. It shares the base scan's
	 * costs but discounts the run cost, since the sorted per-chunk min/max prunes
	 * chunks for the sort-key restriction; the planner picks by cost, and the
	 * result is correct whichever path wins (the executor re-applies the qual).
	 */
	{
		char	   *projName = pgcolumnar_choose_projection(root, rel, rte->relid);

		if (projName != NULL)
		{
			CustomPath *ppath = makeNode(CustomPath);

			ppath->path.pathtype = T_CustomScan;
			ppath->path.parent = rel;
			ppath->path.pathtarget = rel->reltarget;
			ppath->path.param_info = NULL;
			ppath->path.parallel_aware = false;
			ppath->path.parallel_safe = false;
			ppath->path.parallel_workers = 0;
			ppath->path.rows = rel->rows;
			/*
			 * From the captured costs, not from cpath: add_path above may have
			 * freed it. This read is older than #362 and has the same failure
			 * mode -- a projection path costed from freed memory whenever an
			 * index path dominated the base columnar scan.
			 */
			ppath->path.startup_cost = serialStartupCost;
			ppath->path.total_cost = serialStartupCost +
				(serialTotalCost - serialStartupCost) * 0.5;
			ppath->path.pathkeys = NIL;
			ppath->flags = 0;
			ppath->custom_paths = NIL;
			ppath->custom_private = list_make1(makeString(projName));
#if PG_VERSION_NUM >= 170000
			ppath->custom_restrictinfo = rel->baserestrictinfo;
#endif
			ppath->methods = &pgcolumnar_path_methods;

			add_path(rel, &ppath->path);
		}
	}

	/*
	 * Add a parallel-aware partial path (gap 23) so the planner can put a Gather
	 * over a parallel columnar scan. Workers each claim distinct stripes from a
	 * shared counter set up by the DSM callbacks. The cost model mirrors a
	 * parallel seqscan: the per-tuple work is divided among the workers.
	 *
	 * Costed from the serial columnar path rather than from the seqscan, and no
	 * longer conditional on a seqscan surviving (#362). add_path frees the
	 * seqscan when an index path beats it -- precisely the selective queries
	 * where the index is attractive -- so keying the partial path on seqpath
	 * meant no parallel columnar path existed exactly there. That was invisible
	 * while the index path won those queries anyway; once the fetch penalty
	 * prices it out, the only alternative left was the *serial* columnar scan.
	 * Measured on the 100M fixture: the serial path chosen at 2,350,535 and
	 * 25.3 s, with a parallel path available at 589,348 and 4.6 s.
	 *
	 * The costs come from serialStartupCost/serialTotalCost, captured before
	 * add_path, because add_path frees a dominated path and cpath cannot be read
	 * after it. They carry the seqscan's costs when there was one and the
	 * computed costs when there was not, so this is identical wherever the old
	 * condition fired and defined wherever it did not.
	 */
	if (rel->consider_parallel)
	{
		int			workers;
		BlockNumber workPages = rel->pages;

		/*
		 * Size the scan from the WORK, not from the bytes on disk (#451).
		 *
		 * compute_parallel_worker() walks a log3 ladder over relpages, which is a
		 * fair proxy for heap: a heap page is a fixed amount of deform. Ours is
		 * not. A columnar page holds compressed bytes that must be decompressed
		 * before anything can use them, so it is MORE work than a heap page and
		 * we report fewer of them. Handing relpages to that ladder therefore
		 * grants fewer workers the better we compress, and a future encoding win
		 * would silently cost parallelism.
		 *
		 * Measured on ClickBench, same 11.1M rows both sides: relpages 180,418
		 * against heap's 937,344, giving 5 workers where heap got 7, with no cap
		 * binding.
		 *
		 * So estimate the pages this data would occupy uncompressed, which is
		 * what a heap of the same rows reports and therefore what makes the two
		 * comparable. Full row width rather than the projection's: heap's
		 * relpages does not shrink because a query selects two columns, and a
		 * degree that moved with the target list would make the same table
		 * parallelise differently per query.
		 *
		 * Never below rel->pages. This may only correct an under-estimate of the
		 * work; it must not talk the planner down.
		 */
		if (rel->tuples > 0 && rte != NULL && OidIsValid(rte->relid))
		{
			int32		rowWidth = 0;
			AttrNumber	attno;

			/*
			 * get_attavgwidth, not rel->attr_widths. attr_widths carries only the
			 * attributes THIS QUERY references, so `SELECT count(*) WHERE k = 5`
			 * reports one int and the estimate lands below rel->pages, leaving
			 * the degree untouched. That was the first version of this and it
			 * changed nothing at all. The relation's width does not depend on
			 * which columns a query happens to read, and neither does heap's
			 * relpages, which is what this has to be comparable with.
			 */
			for (attno = 1; attno <= rel->max_attr; attno++)
			{
				int32		w = get_attavgwidth(rte->relid, attno);

				if (w > 0)
					rowWidth += w;
			}

			if (rowWidth > 0)
			{
				/*
				 * Per-tuple overhead, because the ladder is calibrated on HEAP
				 * pages and the input therefore has to be in heap-equivalent
				 * ones. This is a unit conversion, not a fudge: the constants are
				 * PostgreSQL's own, and without them the estimate is short by the
				 * header a heap page carries and a column chunk does not.
				 *
				 * Measured on the fixture: 59 bytes of column data per row
				 * against 93 on disk in the heap, so 34 unaccounted, of which 28
				 * is exactly this and the rest is body alignment.
				 *
				 * We do not literally pay a tuple header, and that is not the
				 * question. The question is how much work this scan is relative
				 * to the heap scan the ladder was tuned against, and decompressing
				 * a column chunk is not cheaper than deforming the tuple it
				 * replaces.
				 */
				double		perRow = (double) rowWidth +
					MAXALIGN(SizeofHeapTupleHeader) + sizeof(ItemIdData);
				double		bytes = rel->tuples * perRow;
				double		pages = ceil(bytes / (double) BLCKSZ);

				if (pages > (double) workPages)
					workPages = (BlockNumber) Min(pages, (double) MaxBlockNumber);
			}
		}

		workers = compute_parallel_worker(rel, workPages, -1,
										  max_parallel_workers_per_gather);

		if (workers > 0)
		{
			CustomPath *ppath = makeNode(CustomPath);
			double		divisor = (double) workers;

			ppath->path.pathtype = T_CustomScan;
			ppath->path.parent = rel;
			ppath->path.pathtarget = rel->reltarget;
			ppath->path.param_info = NULL;
			ppath->path.parallel_aware = true;
			ppath->path.parallel_safe = true;
			ppath->path.parallel_workers = workers;
			ppath->path.rows = rel->rows / divisor;
			ppath->path.startup_cost = serialStartupCost;
			ppath->path.total_cost = serialStartupCost +
				(serialTotalCost - serialStartupCost) / divisor;
			ppath->path.pathkeys = NIL;
			ppath->flags = 0;
			ppath->custom_paths = NIL;
			ppath->custom_private = NIL;
#if PG_VERSION_NUM >= 170000
			ppath->custom_restrictinfo = rel->baserestrictinfo;
#endif
			ppath->methods = &pgcolumnar_path_methods;
			add_partial_path(rel, &ppath->path);
		}
	}
}

/* -------------------------------------------------------------------------
 * execution
 * ------------------------------------------------------------------------- */

/*
 * PgColumnarCreateScanState
 *		Shared create-state callback for the one registered CustomScanMethods. A
 *		scanrelid==0 plan is the vectorized aggregate upper node; anything else
 *		is a base-relation columnar scan.
 */
static Node *
PgColumnarCreateScanState(CustomScan *cscan)
{
	if (cscan->scan.scanrelid == 0)
	{
		/*
		 * Both upper aggregate paths are scanrelid==0 custom scans sharing these
		 * registered methods. The grouped path (#289) carries a length-5
		 * custom_private (rti, quals, relid, keys, output map); the ungrouped
		 * path carries length 3.
		 */
		if (list_length(cscan->custom_private) == 5)
			return PgColumnarCreateGroupAggScanState(cscan);
		return PgColumnarCreateAggScanState(cscan);
	}

	return PgColumnarCreateBaseScanState(cscan);
}

static Node *
PgColumnarCreateBaseScanState(CustomScan *cscan)
{
	PgColumnarCustomScanState *cstate =
		(PgColumnarCustomScanState *) palloc0(sizeof(PgColumnarCustomScanState));

	cstate->css.ss.ps.type = T_CustomScanState;
	cstate->css.methods = &pgcolumnar_exec_methods;

	return (Node *) cstate;
}

/* the projection name the planner chose, or NULL for a base scan (gap 26) */
static char *
pgcolumnar_chosen_projection(CustomScan *cscan)
{
	if (cscan->custom_private == NIL)
		return NULL;
	return strVal(linitial(cscan->custom_private));
}

/*
 * pgcolumnar_setup_projection_scan
 *		Open a read on the named projection's storage with a synthetic
 *		[rownumber, cols...] descriptor, build the base<->projection column map,
 *		and translate the pushed-down scan keys to the projection's attnums so the
 *		reader prunes chunks by the projection's min/max. The planner guaranteed
 *		the projection covers every referenced column, so the scan needs no base
 *		reconstruction fetch. Falls back to a base read if the projection vanished
 *		since planning.
 */
static void
pgcolumnar_setup_projection_scan(PgColumnarCustomScanState *cstate, Relation rel,
							   Snapshot snapshot, const char *projName)
{
	uint64		storageId = PgColumnarStorageId(rel);
	TupleDesc	tableDesc = RelationGetDescr(rel);
	List	   *projs = PgColumnarListProjections(storageId);
	PgColumnarProjection *proj = NULL;
	ListCell   *lc;
	TupleDesc	projTupdesc;
	ScanKey		projKeys = NULL;
	int			nProjKeys = 0;
	int			i;

	foreach(lc, projs)
	{
		PgColumnarProjection *p = (PgColumnarProjection *) lfirst(lc);

		if (p->projectionId > 0 && strcmp(p->name, projName) == 0)
		{
			proj = p;
			break;
		}
	}
	if (proj == NULL)
	{
		cstate->readState = PgColumnarBeginRead(rel, snapshot, NULL,
											  cstate->projectedColumns,
											  cstate->nScanKeys, cstate->scanKeys);
		return;
	}

	cstate->projNcols = proj->columnsLen;
	cstate->projValues = palloc(sizeof(Datum) * (proj->columnsLen + 1));
	cstate->projNulls = palloc(sizeof(bool) * (proj->columnsLen + 1));

	/* base attno-1 -> index into projValues (1..K), or -1 if not stored */
	cstate->projColMap = palloc(sizeof(int) * cstate->nTotalColumns);
	for (i = 0; i < cstate->nTotalColumns; i++)
		cstate->projColMap[i] = -1;
	for (i = 0; i < proj->columnsLen; i++)
		cstate->projColMap[proj->columns[i] - 1] = i + 1;

	projTupdesc = CreateTemplateTupleDesc(proj->columnsLen + 1);
	TupleDescInitEntry(projTupdesc, 1, "rownumber", INT8OID, -1, 0);
	for (i = 0; i < proj->columnsLen; i++)
		TupleDescCopyEntry(projTupdesc, i + 2, tableDesc,
						   (AttrNumber) proj->columns[i]);

	if (cstate->nScanKeys > 0)
	{
		projKeys = palloc0(sizeof(ScanKeyData) * cstate->nScanKeys);
		for (i = 0; i < cstate->nScanKeys; i++)
		{
			AttrNumber	baseAtt = cstate->scanKeys[i].sk_attno;

			if (baseAtt >= 1 && baseAtt <= cstate->nTotalColumns &&
				cstate->projColMap[baseAtt - 1] > 0)
			{
				projKeys[nProjKeys] = cstate->scanKeys[i];
				projKeys[nProjKeys].sk_attno =
					(AttrNumber) (cstate->projColMap[baseAtt - 1] + 1);
				nProjKeys++;
			}
		}
	}

	cstate->readState = PgColumnarBeginReadWithStorage(rel, snapshot,
													 proj->projStorageId,
													 projTupdesc, NULL, NULL,
													 nProjKeys, projKeys);
	/* cache base liveness once so the per-row deletion test is a binary search,
	 * not a per-row catalog scan */
	cstate->livenessCache = PgColumnarBuildLivenessCache(rel, snapshot);
	cstate->projScan = true;
}

/*
 * pgcolumnar_setup_late_materialization
 *		Decide whether this scan can build the qual's columns first (#452), and
 *		if so which columns those are.
 *
 * Refused, in order, when: the feature is off; there is no qual to filter with;
 * the qual reads a system column, whose attnos do not address the value cursors;
 * the qual is volatile, because ExecScan re-applies the qual to every row this
 * returns and a volatile expression would then run twice per surviving row; or
 * the qual reads every projected column, in which case there is nothing left to
 * defer and the second pass would be pure overhead.
 */
static void
pgcolumnar_setup_late_materialization(PgColumnarCustomScanState *cstate,
									CustomScan *cscan, TupleDesc tupdesc)
{
	Bitmapset  *qualAttrs = NULL;
	int			natts = tupdesc->natts;
	int			deferrable = 0;
	int			x = -1;
	int			c;

	cstate->qualCols = NULL;
	cstate->lateMat = false;

	if (!pgcolumnar_enable_late_materialization)
		return;
	if (cscan->scan.plan.qual == NIL)
		return;
	if (contain_volatile_functions((Node *) cscan->scan.plan.qual))
		return;

	pull_varattnos((Node *) cscan->scan.plan.qual, cscan->scan.scanrelid,
				   &qualAttrs);

	cstate->qualCols = (bool *) palloc0(sizeof(bool) * natts);

	while ((x = bms_next_member(qualAttrs, x)) >= 0)
	{
		AttrNumber	attno = x + FirstLowInvalidHeapAttributeNumber;

		/* whole-row or system reference: not addressable as a value cursor */
		if (attno <= 0 || attno > natts)
		{
			pfree(cstate->qualCols);
			cstate->qualCols = NULL;
			return;
		}
		cstate->qualCols[attno - 1] = true;
	}

	/*
	 * Something must actually be deferred. A column the scan does not project is
	 * not built either way, so only projected non-qual columns count.
	 */
	for (c = 0; c < natts; c++)
	{
		if (cstate->qualCols[c])
			continue;
		/*
		 * projectedColumns holds ZERO-based attribute indexes
		 * (PgColumnarProjectionFromAttnos adds attno - 1), and NULL means every
		 * column is projected. Not the FirstLowInvalidHeapAttributeNumber-offset
		 * convention that pull_varattnos uses just above, which is why the two
		 * loops in this function index differently.
		 */
		if (cstate->projectedColumns == NULL ||
			bms_is_member(c, cstate->projectedColumns))
			deferrable++;
	}

	if (deferrable == 0)
	{
		pfree(cstate->qualCols);
		cstate->qualCols = NULL;
		return;
	}

	cstate->lateMat = true;
}

/*
 * pgcolumnar_scan_row_filter
 *		The reader's callback: does the row pass the node's qual, given only the
 *		columns the qual reads?
 *
 * The slot is stored as a virtual tuple so ExecQual can read it. Its Datums point
 * into the reader's row context, which outlives this call. The per-tuple context
 * is reset per evaluation exactly as ExecScan resets it per fetched tuple, so a
 * scan that rejects millions of rows does not accumulate their qual allocations.
 */
static bool
pgcolumnar_scan_row_filter(void *arg)
{
	ScanState  *ss = (ScanState *) arg;
	ExprContext *econtext = ss->ps.ps_ExprContext;
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;

	ExecClearTuple(slot);
	ExecStoreVirtualTuple(slot);

	ResetExprContext(econtext);
	econtext->ecxt_scantuple = slot;

	if (ExecQual(ss->ps.qual, econtext))
		return true;

	/*
	 * Count the rejection where the executor would have counted it.
	 *
	 * ExecScan increments nfiltered1 for every tuple its own qual rejects, and
	 * that is what EXPLAIN prints as "Rows Removed by Filter". Filtering here
	 * instead means ExecScan never sees the rejected rows, so without this the
	 * line silently reads 0 on every columnar scan with a qual -- an
	 * instrumentation counter that stops counting while the plan still looks
	 * right. The five-major gate caught exactly that: pushdown_report and
	 * analyze_stats both read this line, and both failed on all five majors.
	 */
	InstrCountFiltered1(ss, 1);
	return false;
}

static void
PgColumnarBeginCustomScan(CustomScanState *node, EState *estate, int eflags)
{
	PgColumnarCustomScanState *cstate = (PgColumnarCustomScanState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	Relation	rel = node->ss.ss_currentRelation;
	TupleDesc	tupdesc = RelationGetDescr(rel);

	cstate->nTotalColumns = tupdesc->natts;

	/*
	 * Compute the projection and the pushdown scan keys even in EXPLAIN-only
	 * mode (they do no I/O), so EXPLAIN can report them. Only the reader, which
	 * touches the catalog and data pages, is skipped when we will not execute.
	 */
	cstate->projectedColumns =
		pgcolumnar_projected_columns(cscan, tupdesc->natts, &cstate->nProjected);
	cstate->scanKeys =
		PgColumnarBuildScanKeys(cscan->scan.plan.qual, cscan->scan.scanrelid,
							  tupdesc, &cstate->nScanKeys);

	/* the projection the planner chose (gap 26); reported by EXPLAIN */
	cstate->projName = pgcolumnar_chosen_projection(cscan);

	pgcolumnar_setup_late_materialization(cstate, cscan, tupdesc);

	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	/*
	 * Persist rows and delete marks written earlier in this transaction so the
	 * reader's catalog snapshot sees them (read-your-writes, spec 9), matching
	 * the table AM's own scan_begin.
	 */
	PgColumnarFlushWriteStateForRelation(RelationGetRelid(rel));
	PgColumnarFlushDeleteVectorForRelation(rel);

	if (cstate->projName != NULL)
	{
		/* gap 26: read a covering projection instead of the base. */
		pgcolumnar_setup_projection_scan(cstate, rel, estate->es_snapshot,
									   cstate->projName);
		if (!cstate->projScan)
			cstate->projName = NULL;	/* projection vanished; base fallback */
	}
	else
	{
		cstate->readState = PgColumnarBeginRead(rel, estate->es_snapshot, NULL,
											  cstate->projectedColumns,
											  cstate->nScanKeys, cstate->scanKeys);
	}
}

/*
 * PgColumnarScanNext
 *		ExecScan access method: fetch the next columnar row into the scan slot.
 *		The row's synthetic item pointer (spec 6) is stored on the slot so an
 *		UPDATE/DELETE above the scan can identify the row by its ctid.
 */

static TupleTableSlot *
pgcolumnar_projection_scan_next(ScanState *ss)
{
	PgColumnarCustomScanState *cstate = (PgColumnarCustomScanState *) ss;
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;
	Relation	rel = ss->ss_currentRelation;
	int			natts = cstate->nTotalColumns;

	for (;;)
	{
		uint64		projRowNum;
		uint64		baseRow;
		int			c;

		if (!PgColumnarReadNextRow(cstate->readState, cstate->projValues,
								 cstate->projNulls, &projRowNum))
			return NULL;

		/* deletes/visibility come from the base (gap 26): the projection stores
		 * no delete vector, so filter by the stored base row number via the cache */
		baseRow = (uint64) DatumGetInt64(cstate->projValues[0]);
		if (!PgColumnarLivenessCacheIsLive(cstate->livenessCache, baseRow))
			continue;

		ExecClearTuple(slot);
		for (c = 0; c < natts; c++)
		{
			int			vi = cstate->projColMap[c];

			if (vi > 0)
			{
				slot->tts_values[c] = cstate->projValues[vi];
				slot->tts_isnull[c] = cstate->projNulls[vi];
			}
			else
			{
				slot->tts_values[c] = (Datum) 0;
				slot->tts_isnull[c] = true;		/* not covered / not referenced */
			}
		}
		ExecStoreVirtualTuple(slot);
		PgColumnarRowNumberToItemPointer(baseRow, &slot->tts_tid);
		slot->tts_tableOid = RelationGetRelid(rel);
		return slot;
	}
}

static TupleTableSlot *
PgColumnarScanNext(ScanState *ss)
{
	PgColumnarCustomScanState *cstate = (PgColumnarCustomScanState *) ss;
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;
	uint64		rowNumber;

	if (cstate->projScan)
		return pgcolumnar_projection_scan_next(ss);

	ExecClearTuple(slot);

	if (cstate->lateMat)
	{
		/*
		 * Late materialization (#452): the reader applies the qual once the
		 * columns it reads are decoded, and builds the rest only for a row that
		 * survives. The filter leaves the slot stored with the qual columns; the
		 * second pass writes the remaining Datums straight into tts_values, so
		 * the slot is re-stored here to publish the complete row.
		 *
		 * ExecScan will apply the same qual again to what this returns. That is
		 * redundant but correct, and it is why a volatile qual refuses this path
		 * at Begin -- it would otherwise be evaluated twice per surviving row.
		 */
		if (!PgColumnarReadNextRowFiltered(cstate->readState, slot->tts_values,
										 slot->tts_isnull, &rowNumber,
										 cstate->qualCols,
										 pgcolumnar_scan_row_filter, ss))
			return NULL;

		ExecClearTuple(slot);
	}
	else if (!PgColumnarReadNextRow(cstate->readState, slot->tts_values,
									slot->tts_isnull, &rowNumber))
		return NULL;

	ExecStoreVirtualTuple(slot);
	PgColumnarRowNumberToItemPointer(rowNumber, &slot->tts_tid);
	slot->tts_tableOid = RelationGetRelid(ss->ss_currentRelation);

	return slot;
}

static bool
PgColumnarScanRecheck(ScanState *ss, TupleTableSlot *slot)
{
	return true;
}

static TupleTableSlot *
PgColumnarExecCustomScan(CustomScanState *node)
{
	return ExecScan(&node->ss,
					(ExecScanAccessMtd) PgColumnarScanNext,
					(ExecScanRecheckMtd) PgColumnarScanRecheck);
}

static void
PgColumnarReScanCustomScan(CustomScanState *node)
{
	PgColumnarCustomScanState *cstate = (PgColumnarCustomScanState *) node;

	if (cstate->readState != NULL)
	{
		PgColumnarRescanRead(cstate->readState);
		/* a parallel rescan keeps sharing the same stripe counter */
		if (cstate->parallelCounter != NULL)
			PgColumnarReadSetParallelCounter(cstate->readState,
										   cstate->parallelCounter);
	}

	ExecScanReScan(&node->ss);
}

/* -------------------------------------------------------------------------
 * parallel scan (gap 23): a shared atomic hands out stripe indices so several
 * workers scanning the same relation each claim distinct stripes. The custom
 * scan framework sizes and allocates the DSM chunk from EstimateDSM and passes
 * its address as `coordinate` to the DSM/worker init callbacks.
 * ------------------------------------------------------------------------- */

static Size
PgColumnarEstimateDSMCustomScan(CustomScanState *node, ParallelContext *pcxt)
{
	return sizeof(pg_atomic_uint32);
}

static void
PgColumnarInitializeDSMCustomScan(CustomScanState *node, ParallelContext *pcxt,
								void *coordinate)
{
	PgColumnarCustomScanState *cstate = (PgColumnarCustomScanState *) node;
	pg_atomic_uint32 *counter = (pg_atomic_uint32 *) coordinate;

	pg_atomic_init_u32(counter, 0);
	cstate->parallelCounter = counter;
	if (cstate->readState != NULL)
		PgColumnarReadSetParallelCounter(cstate->readState, counter);
}

static void
PgColumnarReInitializeDSMCustomScan(CustomScanState *node, ParallelContext *pcxt,
								  void *coordinate)
{
	pg_atomic_uint32 *counter = (pg_atomic_uint32 *) coordinate;

	pg_atomic_write_u32(counter, 0);
}

static void
PgColumnarInitializeWorkerCustomScan(CustomScanState *node, shm_toc *toc,
								   void *coordinate)
{
	PgColumnarCustomScanState *cstate = (PgColumnarCustomScanState *) node;
	pg_atomic_uint32 *counter = (pg_atomic_uint32 *) coordinate;

	cstate->parallelCounter = counter;
	if (cstate->readState != NULL)
		PgColumnarReadSetParallelCounter(cstate->readState, counter);
}

static void
PgColumnarEndCustomScan(CustomScanState *node)
{
	PgColumnarCustomScanState *cstate = (PgColumnarCustomScanState *) node;

	if (cstate->readState != NULL)
	{
		PgColumnarEndRead(cstate->readState);
		cstate->readState = NULL;
	}
	if (cstate->livenessCache != NULL)
	{
		PgColumnarFreeLivenessCache(cstate->livenessCache);
		cstate->livenessCache = NULL;
	}
}

static void
PgColumnarExplainCustomScan(CustomScanState *node, List *ancestors,
						  ExplainState *es)
{
	PgColumnarCustomScanState *cstate = (PgColumnarCustomScanState *) node;

	if (cstate->projName != NULL)
		ExplainPropertyText("Columnar Projection", cstate->projName, es);

	ExplainPropertyInteger("Columnar Projected Columns", NULL,
						   cstate->nProjected, es);
	ExplainPropertyInteger("Columnar Total Columns", NULL,
						   cstate->nTotalColumns, es);
	/*
	 * Report what the scan pushes down, not what the planner handed it.
	 *
	 * cstate->nScanKeys is the count the planner produced, and it is the same
	 * whether or not pushdown is enabled. But pgcolumnar_enable_qual_pushdown
	 * gates pgcolumnar_build_predicates in PgColumnarBeginRead, so with the setting
	 * off the reader builds no predicates and skips no chunk groups: nothing is
	 * pushed down in any sense the scan acts on. Someone turning the setting off
	 * to test a theory, and checking EXPLAIN to confirm it took effect, was told
	 * it had not (issue #191).
	 *
	 * Every other "Columnar ..." line here describes the run rather than the
	 * plan -- Projected Columns, Chunk Groups Total and the counters below it --
	 * so this one line meant something different from all of its neighbours.
	 */
	ExplainPropertyInteger("Columnar Pushed-Down Filters", NULL,
						   pgcolumnar_enable_qual_pushdown ? cstate->nScanKeys : 0,
						   es);

	if (cstate->readState != NULL)
	{
		uint64		groupsRead = 0;
		uint64		groupsSkipped = 0;
		uint64		groupsTotal = 0;

		PgColumnarReadStats(cstate->readState, &groupsRead, &groupsSkipped,
						  &groupsTotal);

		/*
		 * How many of those filters the reader can actually exclude a chunk
		 * group with (#479). The line above counts the scan keys the scan was
		 * GIVEN; pgcolumnar_make_predicates then drops any it cannot evaluate
		 * against the stored min/max, and a dropped key skips nothing.
		 *
		 * Reported separately rather than replacing the line above, because the
		 * two answer different questions and #191 shows both get asked: that one
		 * says whether pgcolumnar.enable_qual_pushdown took effect, this one says
		 * whether the predicates it pushed can prune. A single number cannot say
		 * both, and saying only the first is how #477 stayed invisible for a
		 * year -- "Pushed-Down Filters: 1" beside "Chunk Groups Removed by
		 * Filter: 0" reads as an unselective predicate and meant an unusable one.
		 *
		 * No enable_qual_pushdown ternary here: with the setting off the reader
		 * builds no predicates, so this is already 0. It describes the run.
		 */
		ExplainPropertyInteger("Columnar Usable Skip Predicates", NULL,
							   PgColumnarReadUsablePredicates(cstate->readState),
							   es);

		ExplainPropertyInteger("Columnar Chunk Groups Total", NULL,
							   (int64) groupsTotal, es);
		ExplainPropertyInteger("Columnar Chunk Groups Read", NULL,
							   (int64) groupsRead, es);
		ExplainPropertyInteger("Columnar Chunk Groups Removed by Filter", NULL,
							   (int64) groupsSkipped, es);
		ExplainPropertyInteger("Columnar Vectors Skipped", NULL,
							   (int64) PgColumnarVectorsSkipped(cstate->readState), es);

		/*
		 * Rows the qual rejected before their remaining projected columns were
		 * built (#452). Distinct from the counters above: those are rows never
		 * READ, this is rows read but not materialized.
		 */
		ExplainPropertyInteger("Columnar Rows Filtered Before Materialization", NULL,
							   (int64) PgColumnarRowsFilteredEarly(cstate->readState),
							   es);
	}
}

/* -------------------------------------------------------------------------
 * registration
 * ------------------------------------------------------------------------- */

void
PgColumnarCustomScanInit(void)
{
	RegisterCustomScanMethods(&pgcolumnar_scan_methods);

	prev_set_rel_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = PgColumnarSetRelPathlist;
}
