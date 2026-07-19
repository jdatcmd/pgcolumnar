/*-------------------------------------------------------------------------
 *
 * columnar_customscan.c
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
 * Parallel sequential scans are disabled for columnar relations by dropping
 * the relation's partial paths, so plans are deterministic.
 *
 * Independent MIT implementation built from design/FORMAT_AND_INTERFACE_SPEC.md
 * (format 2.0), design/REWRITE_PLAN.md section 6, and the public PostgreSQL 17
 * API (the custom-scan provider contract in nodes/extensible.h and
 * executor/nodeCustom.c) only.
 *
 *-------------------------------------------------------------------------
 */
#include "columnar.h"

#include "access/relation.h"
#include "access/relscan.h"
#include "access/stratnum.h"
#include "commands/explain.h"
#include "executor/executor.h"
#include "nodes/execnodes.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "nodes/plannodes.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/restrictinfo.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/typcache.h"

/* GUC: use the columnar custom scan path (spec 8.3) */
bool		columnar_enable_custom_scan = true;

static set_rel_pathlist_hook_type prev_set_rel_pathlist_hook = NULL;

/* our executor-time scan state embeds CustomScanState as its first field */
typedef struct ColumnarCustomScanState
{
	CustomScanState css;
	ColumnarReadState *readState;
	Bitmapset  *projectedColumns;	/* 0-based; NULL means all columns */
	ScanKey		scanKeys;
	int			nScanKeys;
	int			nProjected;			/* for EXPLAIN */
	int			nTotalColumns;
} ColumnarCustomScanState;

/* path -> plan */
static Plan *ColumnarPlanCustomPath(PlannerInfo *root, RelOptInfo *rel,
									CustomPath *best_path, List *tlist,
									List *clauses, List *custom_plans);

/* plan -> scan state */
static Node *ColumnarCreateScanState(CustomScan *cscan);

/* executor callbacks */
static void ColumnarBeginCustomScan(CustomScanState *node, EState *estate,
									int eflags);
static TupleTableSlot *ColumnarExecCustomScan(CustomScanState *node);
static void ColumnarEndCustomScan(CustomScanState *node);
static void ColumnarReScanCustomScan(CustomScanState *node);
static void ColumnarExplainCustomScan(CustomScanState *node, List *ancestors,
									  ExplainState *es);

/* ExecScan helpers */
static TupleTableSlot *ColumnarScanNext(ScanState *ss);
static bool ColumnarScanRecheck(ScanState *ss, TupleTableSlot *slot);

static const CustomPathMethods columnar_path_methods = {
	.CustomName = "ColumnarScan",
	.PlanCustomPath = ColumnarPlanCustomPath,
	.ReparameterizeCustomPathByChild = NULL,
};

static const CustomScanMethods columnar_scan_methods = {
	.CustomName = "ColumnarScan",
	.CreateCustomScanState = ColumnarCreateScanState,
};

static const CustomExecMethods columnar_exec_methods = {
	.CustomName = "ColumnarScan",
	.BeginCustomScan = ColumnarBeginCustomScan,
	.ExecCustomScan = ColumnarExecCustomScan,
	.EndCustomScan = ColumnarEndCustomScan,
	.ReScanCustomScan = ColumnarReScanCustomScan,
	.ExplainCustomScan = ColumnarExplainCustomScan,
};

/* -------------------------------------------------------------------------
 * planning
 * ------------------------------------------------------------------------- */

/*
 * columnar_projected_columns
 *		Build the 0-based set of columns the plan actually references, from the
 *		Vars in its target list and its restriction clauses. Returns NULL when
 *		a whole-row or system column is requested, meaning "all columns", so the
 *		reader stays correct for whole-row references and UPDATE/DELETE (which
 *		carry a ctid system Var). This is the projection pushed into the reader
 *		(spec 9).
 */
static Bitmapset *
columnar_projected_columns(CustomScan *cscan, int natts, int *nProjected)
{
	Bitmapset  *needed = NULL;
	Bitmapset  *projected = NULL;
	Index		scanrelid = cscan->scan.scanrelid;
	int			attno;

	pull_varattnos((Node *) cscan->scan.plan.targetlist, scanrelid, &needed);
	pull_varattnos((Node *) cscan->scan.plan.qual, scanrelid, &needed);

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

/*
 * columnar_commute_strategy
 *		The btree comparison strategy for "value op column" given the strategy
 *		for "column op value", used when the constant is on the left.
 */
static StrategyNumber
columnar_commute_strategy(StrategyNumber s)
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
 * columnar_clause_to_scankey
 *		Translate a single restriction clause of the form "column op const"
 *		(or "const op column") into a scan key for chunk-group skipping, when
 *		op is a btree comparison operator in the column type's default btree
 *		family and both sides share the column's type. Returns false for any
 *		other clause, so unsupported quals simply do not drive skipping; the
 *		executor still applies them as a filter, so results are unaffected.
 */
static bool
columnar_clause_to_scankey(Node *clause, Index scanrelid, TupleDesc tupdesc,
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

	tce = lookup_type_cache(var->vartype, TYPECACHE_BTREE_OPFAMILY);
	if (!OidIsValid(tce->btree_opf))
		return false;

	strat = get_op_opfamily_strategy(op->opno, tce->btree_opf);
	if (strat == InvalidStrategy)
		return false;
	if (!varOnLeft)
		strat = columnar_commute_strategy(strat);
	if (strat == InvalidStrategy)
		return false;

	/*
	 * Fill the scan key directly. The reader (columnar_build_predicates) uses
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
 * columnar_build_scan_keys
 *		Build the scan-key array for chunk-group skipping from a plan's
 *		restriction clauses (spec 9). Clauses that are not simple comparisons
 *		are skipped.
 */
static ScanKey
columnar_build_scan_keys(List *qual, Index scanrelid, TupleDesc tupdesc,
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
		if (columnar_clause_to_scankey((Node *) lfirst(lc), scanrelid, tupdesc,
									   &keys[n]))
			n++;
	}

	*nkeys = n;
	return keys;
}

/*
 * ColumnarPlanCustomPath
 *		Convert the CustomPath to a CustomScan plan node. The restriction
 *		clauses become the scan's qual so the executor re-applies them (this is
 *		what makes chunk-group skipping safe); custom_scan_tlist stays NIL so
 *		the scan tuple is the full base-relation rowtype.
 */
static Plan *
ColumnarPlanCustomPath(PlannerInfo *root, RelOptInfo *rel,
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
	cscan->custom_private = NIL;
	cscan->custom_scan_tlist = NIL;
	cscan->methods = &columnar_scan_methods;

	return &cscan->scan.plan;
}

/*
 * ColumnarSetRelPathlist
 *		set_rel_pathlist_hook: for a columnar base relation, replace the
 *		sequential-scan path with the columnar custom scan and drop parallel
 *		paths. Index and bitmap paths are left in place, so ordinary index
 *		scans still compete. The custom scan inherits the sequential scan's
 *		cost (which already reflects enable_seqscan), so SET enable_seqscan=off
 *		steers the planner to an index scan exactly as before.
 */
static void
ColumnarSetRelPathlist(PlannerInfo *root, RelOptInfo *rel, Index rti,
					   RangeTblEntry *rte)
{
	CustomPath *cpath;
	Path	   *seqpath = NULL;
	List	   *keep = NIL;
	ListCell   *lc;

	if (prev_set_rel_pathlist_hook)
		prev_set_rel_pathlist_hook(root, rel, rti, rte);

	if (!columnar_enable_custom_scan)
		return;
	if (rte->rtekind != RTE_RELATION || rte->relkind != RELKIND_RELATION)
		return;
	if (rel->reloptkind != RELOPT_BASEREL)
		return;
	if (!OidIsValid(rte->relid) || !ColumnarIsColumnarRelation(rte->relid))
		return;

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

	/* disable parallel sequential scan: no partial paths for columnar */
	rel->partial_pathlist = NIL;

	cpath = makeNode(CustomPath);
	cpath->path.pathtype = T_CustomScan;
	cpath->path.parent = rel;
	cpath->path.pathtarget = rel->reltarget;
	cpath->path.param_info = NULL;
	cpath->path.parallel_aware = false;
	cpath->path.parallel_safe = false;
	cpath->path.parallel_workers = 0;
	cpath->path.rows = rel->rows;
	cpath->path.startup_cost = seqpath ? seqpath->startup_cost : 0;
	cpath->path.total_cost = seqpath ? seqpath->total_cost : rel->rows;
	cpath->path.pathkeys = NIL;
	cpath->flags = 0;
	cpath->custom_paths = NIL;
	cpath->custom_private = NIL;
	cpath->custom_restrictinfo = rel->baserestrictinfo;
	cpath->methods = &columnar_path_methods;

	add_path(rel, &cpath->path);
}

/* -------------------------------------------------------------------------
 * execution
 * ------------------------------------------------------------------------- */

static Node *
ColumnarCreateScanState(CustomScan *cscan)
{
	ColumnarCustomScanState *cstate =
		(ColumnarCustomScanState *) palloc0(sizeof(ColumnarCustomScanState));

	cstate->css.ss.ps.type = T_CustomScanState;
	cstate->css.methods = &columnar_exec_methods;

	return (Node *) cstate;
}

static void
ColumnarBeginCustomScan(CustomScanState *node, EState *estate, int eflags)
{
	ColumnarCustomScanState *cstate = (ColumnarCustomScanState *) node;
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
		columnar_projected_columns(cscan, tupdesc->natts, &cstate->nProjected);
	cstate->scanKeys =
		columnar_build_scan_keys(cscan->scan.plan.qual, cscan->scan.scanrelid,
								 tupdesc, &cstate->nScanKeys);

	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	/*
	 * Persist rows and delete marks written earlier in this transaction so the
	 * reader's catalog snapshot sees them (read-your-writes, spec 9), matching
	 * the table AM's own scan_begin.
	 */
	ColumnarFlushWriteStateForRelation(RelationGetRelid(rel));
	ColumnarFlushRowMaskForRelation(rel);

	cstate->readState = ColumnarBeginRead(rel, estate->es_snapshot, NULL,
										  cstate->projectedColumns,
										  cstate->nScanKeys, cstate->scanKeys);
}

/*
 * ColumnarScanNext
 *		ExecScan access method: fetch the next columnar row into the scan slot.
 *		The row's synthetic item pointer (spec 6) is stored on the slot so an
 *		UPDATE/DELETE above the scan can identify the row by its ctid.
 */
static TupleTableSlot *
ColumnarScanNext(ScanState *ss)
{
	ColumnarCustomScanState *cstate = (ColumnarCustomScanState *) ss;
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;
	uint64		rowNumber;

	ExecClearTuple(slot);

	if (!ColumnarReadNextRow(cstate->readState, slot->tts_values,
							 slot->tts_isnull, &rowNumber))
		return NULL;

	ExecStoreVirtualTuple(slot);
	ColumnarRowNumberToItemPointer(rowNumber, &slot->tts_tid);
	slot->tts_tableOid = RelationGetRelid(ss->ss_currentRelation);

	return slot;
}

static bool
ColumnarScanRecheck(ScanState *ss, TupleTableSlot *slot)
{
	return true;
}

static TupleTableSlot *
ColumnarExecCustomScan(CustomScanState *node)
{
	return ExecScan(&node->ss,
					(ExecScanAccessMtd) ColumnarScanNext,
					(ExecScanRecheckMtd) ColumnarScanRecheck);
}

static void
ColumnarReScanCustomScan(CustomScanState *node)
{
	ColumnarCustomScanState *cstate = (ColumnarCustomScanState *) node;

	if (cstate->readState != NULL)
		ColumnarRescanRead(cstate->readState);

	ExecScanReScan(&node->ss);
}

static void
ColumnarEndCustomScan(CustomScanState *node)
{
	ColumnarCustomScanState *cstate = (ColumnarCustomScanState *) node;

	if (cstate->readState != NULL)
	{
		ColumnarEndRead(cstate->readState);
		cstate->readState = NULL;
	}
}

static void
ColumnarExplainCustomScan(CustomScanState *node, List *ancestors,
						  ExplainState *es)
{
	ColumnarCustomScanState *cstate = (ColumnarCustomScanState *) node;

	ExplainPropertyInteger("Columnar Projected Columns", NULL,
						   cstate->nProjected, es);
	ExplainPropertyInteger("Columnar Total Columns", NULL,
						   cstate->nTotalColumns, es);
	ExplainPropertyInteger("Columnar Pushed-Down Filters", NULL,
						   cstate->nScanKeys, es);

	if (cstate->readState != NULL)
	{
		uint64		groupsRead = 0;
		uint64		groupsSkipped = 0;
		uint64		groupsTotal = 0;

		ColumnarReadStats(cstate->readState, &groupsRead, &groupsSkipped,
						  &groupsTotal);

		ExplainPropertyInteger("Columnar Chunk Groups Total", NULL,
							   (int64) groupsTotal, es);
		ExplainPropertyInteger("Columnar Chunk Groups Read", NULL,
							   (int64) groupsRead, es);
		ExplainPropertyInteger("Columnar Chunk Groups Removed by Filter", NULL,
							   (int64) groupsSkipped, es);
	}
}

/* -------------------------------------------------------------------------
 * registration
 * ------------------------------------------------------------------------- */

void
ColumnarCustomScanInit(void)
{
	RegisterCustomScanMethods(&columnar_scan_methods);

	prev_set_rel_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = ColumnarSetRelPathlist;
}
