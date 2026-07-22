/*-------------------------------------------------------------------------
 *
 * columnar_vector.c
 *		Vectorized execution for pgColumnar (spec 9): a column-at-a-time filter
 *		and vectorized aggregates over decoded chunk-group arrays.
 *
 * Two things live here. First, a shared filter: a plan's simple "column op
 * const" restriction clauses are turned into predicates that are evaluated
 * column-at-a-time over a decoded chunk group to build a selection vector.
 * Second, a vectorized aggregate custom scan: for the common shape
 * SELECT agg(col) FROM t [WHERE ...] with no GROUP BY or HAVING, a custom path
 * on the grouping upper relation computes count, sum, avg, min and max from the
 * zone-map metadata, or by scanning when the group has deletes, skipping the
 * per-tuple executor path.
 *
 * Correctness is the invariant. The vectorized aggregate is only chosen when
 * every aggregate, every column type, and every filter clause is one this module
 * fully supports; anything else falls back to the ordinary scalar Agg plan. The
 * accumulation reproduces PostgreSQL's own aggregate semantics exactly (integer
 * sum overflow behaviour, average as numeric via numeric_div, min/max by the
 * type's default ordering), so a vectorized result equals the scalar result for
 * every query. The toggle columnar.enable_vectorization disables this path so
 * tests can assert that equality.
 *
 * The aggregate custom scan reuses the same registered CustomScanMethods as the
 * base custom scan (so both show as "Custom Scan (ColumnarScan)"); the shared
 * create-state callback in columnar_customscan.c dispatches to the aggregate
 * variant when the plan is a scanrelid==0 upper node.
 *
 * Independent MIT implementation built from
 * design/NATIVE_FORMAT_AND_INTERFACE_SPEC.md and the public PostgreSQL API (the
 * custom-scan provider contract, create_upper_paths_hook, and the documented
 * aggregate result types) only.
 *
 *-------------------------------------------------------------------------
 */
#include "columnar.h"

#include "access/relation.h"
#include "access/table.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#if PG_VERSION_NUM >= 180000
/* PG18 split the ExplainProperty* helpers out into explain_format.h. */
#include "commands/explain_format.h"
#endif
#include "executor/executor.h"
#include "executor/tuptable.h"
#include "nodes/execnodes.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "nodes/plannodes.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/planner.h"
#include "optimizer/restrictinfo.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "access/stratnum.h"
#include "access/tupmacs.h"
#include "utils/rel.h"
#include "utils/typcache.h"

/* GUC: use the vectorized aggregate path (spec 8.3 scan control) */
bool		columnar_enable_vectorization = true;

/* GUC: answer count(*) from catalog metadata without scanning data (gap 28) */
bool		columnar_enable_metadata_count = true;

/* -------------------------------------------------------------------------
 * shared column-at-a-time filter
 * ------------------------------------------------------------------------- */

/*
 * columnar_clause_to_predicate
 *		Turn one "column op const" (or "const op column") clause into a predicate
 *		we can evaluate row by row. Requires a strict boolean operator and a
 *		non-null constant, so that a null column value or a failed comparison
 *		excludes the row, matching SQL WHERE semantics. Returns false for any
 *		other clause.
 */
static bool
columnar_clause_to_predicate(Node *clause, Index scanrelid, TupleDesc tupdesc,
							 ColumnarVecPredicate *pred)
{
	OpExpr	   *op;
	Node	   *leftop;
	Node	   *rightop;
	Var		   *var;
	Const	   *con;
	bool		varOnLeft;
	Oid			opfuncid;

	if (!IsA(clause, OpExpr))
		return false;
	op = (OpExpr *) clause;
	if (list_length(op->args) != 2)
		return false;
	if (op->opresulttype != BOOLOID)
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

	opfuncid = get_opcode(op->opno);
	if (!OidIsValid(opfuncid) || !func_strict(opfuncid))
		return false;

	pred->attidx = var->varattno - 1;
	pred->varOnLeft = varOnLeft;
	fmgr_info(opfuncid, &pred->opFn);
	pred->constValue = con->constvalue;
	pred->collation = op->inputcollid;

	/*
	 * Resolve a typed fast path (I6): a storage kind from the column type and a
	 * btree strategy from the operator, normalized to "column op const". Only
	 * used when the constant has exactly the column's type (so the raw Datum can
	 * be read as that C type); anything else keeps the operator-function path.
	 * The eligible types compare byte-for-byte without collation, so the fast
	 * path is collation-agnostic.
	 */
	pred->fastKind = COLUMNAR_VECFAST_NONE;
	pred->strategy = 0;
	if (con->consttype == var->vartype)
	{
		int			fk = COLUMNAR_VECFAST_NONE;

		switch (var->vartype)
		{
			case INT2OID:
				fk = COLUMNAR_VECFAST_I16;
				break;
			case INT4OID:
			case DATEOID:
				fk = COLUMNAR_VECFAST_I32;
				break;
			case INT8OID:
			case TIMESTAMPOID:
			case TIMESTAMPTZOID:
				fk = COLUMNAR_VECFAST_I64;
				break;
			case FLOAT4OID:
				fk = COLUMNAR_VECFAST_F32;
				break;
			case FLOAT8OID:
				fk = COLUMNAR_VECFAST_F64;
				break;
			default:
				break;
		}

		if (fk != COLUMNAR_VECFAST_NONE)
		{
			TypeCacheEntry *tce = lookup_type_cache(var->vartype,
													TYPECACHE_BTREE_OPFAMILY);
			int			strat = 0;

			/* match the operator to a btree strategy of the type's opfamily */
			if (OidIsValid(tce->btree_opf))
			{
				int			s;

				for (s = BTLessStrategyNumber; s <= BTGreaterStrategyNumber; s++)
				{
					if (get_opfamily_member(tce->btree_opf, tce->btree_opintype,
											tce->btree_opintype, s) == op->opno)
					{
						strat = s;
						break;
					}
				}
			}

			if (strat != 0)
			{
				if (!varOnLeft)
				{
					switch (strat)
					{
						case BTLessStrategyNumber:
							strat = BTGreaterStrategyNumber;
							break;
						case BTLessEqualStrategyNumber:
							strat = BTGreaterEqualStrategyNumber;
							break;
						case BTGreaterEqualStrategyNumber:
							strat = BTLessEqualStrategyNumber;
							break;
						case BTGreaterStrategyNumber:
							strat = BTLessStrategyNumber;
							break;
						default:
							break;	/* equal is symmetric */
					}
				}
				pred->fastKind = fk;
				pred->strategy = strat;
			}
		}
	}

	return true;
}

ColumnarVecPredicate *
ColumnarBuildVecPredicates(List *qual, Index scanrelid, TupleDesc tupdesc,
						   int *npreds, bool *allConvertible)
{
	ColumnarVecPredicate *preds;
	ListCell   *lc;
	int			n = 0;

	*npreds = 0;
	*allConvertible = true;
	if (qual == NIL)
		return NULL;

	preds = (ColumnarVecPredicate *)
		palloc0(sizeof(ColumnarVecPredicate) * list_length(qual));

	foreach(lc, qual)
	{
		if (columnar_clause_to_predicate((Node *) lfirst(lc), scanrelid, tupdesc,
										 &preds[n]))
			n++;
		else
			*allConvertible = false;
	}

	*npreds = n;
	return preds;
}

bool
ColumnarVecRowPasses(ColumnarVecPredicate *preds, int npreds,
					 ColumnarVector *vec, uint64 i)
{
	int			p;

	for (p = 0; p < npreds; p++)
	{
		ColumnarVecPredicate *pred = &preds[p];
		Datum		colval;
		Datum		result;

		/* the predicate column is always projected, so its array is present */
		if (vec->isnull[pred->attidx][i])
			return false;		/* strict op over NULL: row excluded */

		colval = vec->values[pred->attidx][i];
		if (pred->varOnLeft)
			result = FunctionCall2Coll(&pred->opFn, pred->collation,
									   colval, pred->constValue);
		else
			result = FunctionCall2Coll(&pred->opFn, pred->collation,
									   pred->constValue, colval);

		if (!DatumGetBool(result))
			return false;
	}

	return true;
}

/*
 * Typed, branch-free predicate loops (I6). Each ANDs one predicate's result
 * into sel over the whole column array. The comparison is chosen once (outside
 * the inner loop) so the loop body has no data-dependent branch beyond the
 * comparison, which the compiler can auto-vectorize.
 */
#define VEC_CMP_BODY(GET, OP) \
	{ for (i = 0; i < n; i++) { sel[i] = sel[i] & !isn[i] & (GET(vals[i]) OP c); } break; }
#define VEC_CMP(NAME, CTYPE, GET) \
static void \
NAME(bool *sel, const Datum *vals, const bool *isn, uint64 n, int strat, Datum cdat) \
{ \
	CTYPE c = GET(cdat); \
	uint64 i; \
	switch (strat) \
	{ \
		case BTLessStrategyNumber: VEC_CMP_BODY(GET, <) \
		case BTLessEqualStrategyNumber: VEC_CMP_BODY(GET, <=) \
		case BTEqualStrategyNumber: VEC_CMP_BODY(GET, ==) \
		case BTGreaterEqualStrategyNumber: VEC_CMP_BODY(GET, >=) \
		case BTGreaterStrategyNumber: VEC_CMP_BODY(GET, >) \
	} \
}

VEC_CMP(vecsel_i16, int16, DatumGetInt16)
VEC_CMP(vecsel_i32, int32, DatumGetInt32)
VEC_CMP(vecsel_i64, int64, DatumGetInt64)
VEC_CMP(vecsel_f32, float4, DatumGetFloat4)
VEC_CMP(vecsel_f64, float8, DatumGetFloat8)

void
ColumnarVecSelect(ColumnarVecPredicate *preds, int npreds,
				  ColumnarVector *vec, bool *sel)
{
	uint64		n = vec->nrows;
	uint64		i;
	int			p;

	/* start from the non-deleted rows */
	for (i = 0; i < n; i++)
		sel[i] = !vec->deleted[i];

	for (p = 0; p < npreds; p++)
	{
		ColumnarVecPredicate *pred = &preds[p];
		const Datum *vals = vec->values[pred->attidx];
		const bool *isn = vec->isnull[pred->attidx];

		switch (pred->fastKind)
		{
			case COLUMNAR_VECFAST_I16:
				vecsel_i16(sel, vals, isn, n, pred->strategy, pred->constValue);
				break;
			case COLUMNAR_VECFAST_I32:
				vecsel_i32(sel, vals, isn, n, pred->strategy, pred->constValue);
				break;
			case COLUMNAR_VECFAST_I64:
				vecsel_i64(sel, vals, isn, n, pred->strategy, pred->constValue);
				break;
			case COLUMNAR_VECFAST_F32:
				vecsel_f32(sel, vals, isn, n, pred->strategy, pred->constValue);
				break;
			case COLUMNAR_VECFAST_F64:
				vecsel_f64(sel, vals, isn, n, pred->strategy, pred->constValue);
				break;
			default:
				/* operator-function fallback, still column-at-a-time */
				for (i = 0; i < n; i++)
				{
					Datum		r;

					if (!sel[i])
						continue;
					if (isn[i])
					{
						sel[i] = false;
						continue;
					}
					if (pred->varOnLeft)
						r = FunctionCall2Coll(&pred->opFn, pred->collation,
											  vals[i], pred->constValue);
					else
						r = FunctionCall2Coll(&pred->opFn, pred->collation,
											  pred->constValue, vals[i]);
					sel[i] = DatumGetBool(r);
				}
				break;
		}
	}
}

/* -------------------------------------------------------------------------
 * vectorized aggregate: classification
 * ------------------------------------------------------------------------- */

typedef enum ColumnarAggKind
{
	COLUMNAR_AGG_COUNT_STAR,
	COLUMNAR_AGG_COUNT_COL,
	COLUMNAR_AGG_SUM_INT,
	COLUMNAR_AGG_AVG_INT,
	COLUMNAR_AGG_MIN,
	COLUMNAR_AGG_MAX
} ColumnarAggKind;

typedef struct ColumnarAggSpec
{
	ColumnarAggKind kind;
	int			attidx;			/* 0-based column, or -1 for count(*) */
	Oid			inputType;		/* column type (min/max/sum/avg) */

	/* min/max helpers */
	FmgrInfo	cmpFn;			/* type default btree comparison */
	Oid			collation;
	bool		byval;
	int16		typlen;

	/* accumulators */
	int64		count;			/* count(*), count(col), avg count */
	int64		sum;			/* integer sum / avg sum */
	bool		sawValue;		/* any non-null value contributed */
	Datum		extreme;		/* min/max running value (in resultContext) */
} ColumnarAggSpec;

/*
 * columnar_classify_aggref
 *		Decide whether an Aggref is one we can compute vectorized, and if so fill
 *		its spec. expectedVarno is the scan relation's range-table index at plan
 *		time (to check the argument Var), or a negative value at execution time
 *		where only the attribute number matters. Returns false to force the
 *		scalar fallback.
 */
static bool
columnar_classify_aggref(Aggref *agg, int expectedVarno, ColumnarAggSpec *spec)
{
	char	   *name;
	Oid			nsp;
	Var		   *argVar = NULL;

	if (agg->aggorder != NIL || agg->aggdistinct != NIL ||
		agg->aggfilter != NULL || agg->aggvariadic ||
		agg->aggkind != AGGKIND_NORMAL || agg->aggsplit != AGGSPLIT_SIMPLE)
		return false;

	nsp = get_func_namespace(agg->aggfnoid);
	if (nsp != PG_CATALOG_NAMESPACE)
		return false;
	name = get_func_name(agg->aggfnoid);
	if (name == NULL)
		return false;

	/* recover the single column argument, when there is one */
	if (list_length(agg->args) == 1)
	{
		TargetEntry *tle = (TargetEntry *) linitial(agg->args);
		Node	   *arg = (Node *) tle->expr;

		if (IsA(arg, RelabelType))
			arg = (Node *) ((RelabelType *) arg)->arg;
		if (IsA(arg, Var))
			argVar = (Var *) arg;
	}

	memset(spec, 0, sizeof(*spec));
	spec->attidx = -1;

	if (strcmp(name, "count") == 0)
	{
		if (agg->aggstar || list_length(agg->args) == 0)
		{
			spec->kind = COLUMNAR_AGG_COUNT_STAR;
			return true;
		}
		if (argVar == NULL)
			return false;
		if (expectedVarno >= 0 && argVar->varno != (Index) expectedVarno)
			return false;
		spec->kind = COLUMNAR_AGG_COUNT_COL;
		spec->attidx = argVar->varattno - 1;
		return spec->attidx >= 0;
	}

	if (argVar == NULL)
		return false;
	if (expectedVarno >= 0 && argVar->varno != (Index) expectedVarno)
		return false;
	if (argVar->varattno < 1)
		return false;
	spec->attidx = argVar->varattno - 1;
	spec->inputType = argVar->vartype;

	if (strcmp(name, "sum") == 0)
	{
		if (spec->inputType == INT2OID || spec->inputType == INT4OID)
		{
			spec->kind = COLUMNAR_AGG_SUM_INT;
			return true;
		}
		return false;			/* int8->numeric, float, numeric: fall back */
	}

	if (strcmp(name, "avg") == 0)
	{
		if (spec->inputType == INT2OID || spec->inputType == INT4OID)
		{
			spec->kind = COLUMNAR_AGG_AVG_INT;
			return true;
		}
		return false;
	}

	if (strcmp(name, "min") == 0 || strcmp(name, "max") == 0)
	{
		TypeCacheEntry *tce = lookup_type_cache(spec->inputType,
												TYPECACHE_CMP_PROC_FINFO);

		if (!OidIsValid(tce->cmp_proc_finfo.fn_oid))
			return false;
		spec->kind = (name[1] == 'i') ? COLUMNAR_AGG_MIN : COLUMNAR_AGG_MAX;
		return true;
	}

	return false;
}

/* -------------------------------------------------------------------------
 * vectorized aggregate: executor state
 * ------------------------------------------------------------------------- */

typedef struct ColumnarAggScanState
{
	CustomScanState css;

	Oid			relid;			/* base relation to scan */
	List	   *quals;			/* restriction clauses (original varnos) */
	Index		scanrelid;		/* their range-table index */

	ColumnarAggSpec *specs;
	int			naggs;

	ColumnarVecPredicate *preds;
	int			npreds;

	MemoryContext resultContext;	/* holds min/max running values */
	bool		done;			/* the single result row was emitted */

	/* chunk-group skip counters captured for EXPLAIN */
	bool		haveStats;
	uint64		groupsRead;
	uint64		groupsSkipped;
	uint64		groupsTotal;
} ColumnarAggScanState;

static const CustomExecMethods columnar_agg_exec_methods;

/* -------------------------------------------------------------------------
 * vectorized aggregate: planning
 * ------------------------------------------------------------------------- */

static Plan *
ColumnarPlanAggPath(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
					List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);

	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = NIL;	/* WHERE is applied inside the scan */
	cscan->scan.scanrelid = 0;		/* not a base-relation scan */
	cscan->flags = best_path->flags;
	cscan->custom_plans = NIL;
	cscan->custom_exprs = NIL;
	cscan->custom_private = best_path->custom_private;
	cscan->custom_scan_tlist = tlist;	/* defines the output tuple shape */
	cscan->methods = &columnar_scan_methods;	/* shared registered methods */

	return &cscan->scan.plan;
}

static const CustomPathMethods columnar_agg_path_methods = {
	.CustomName = "ColumnarAgg",
	.PlanCustomPath = ColumnarPlanAggPath,
	.ReparameterizeCustomPathByChild = NULL,
};

static create_upper_paths_hook_type prev_create_upper_paths_hook = NULL;

/*
 * ColumnarCreateUpperPaths
 *		create_upper_paths_hook: for a plain SELECT agg(col) FROM columnar_table
 *		[WHERE simple quals] with no grouping or HAVING, add a custom path that
 *		computes the aggregates vectorized. Every aggregate, column type and
 *		filter clause must be fully supported, or we add nothing and the ordinary
 *		Agg plan runs, so results are never at risk.
 */
static void
ColumnarCreateUpperPaths(PlannerInfo *root, UpperRelationKind stage,
						 RelOptInfo *input_rel, RelOptInfo *output_rel,
						 void *extra)
{
	Query	   *parse = root->parse;
	RangeTblEntry *rte;
	Oid			relid;
	List	   *tlist = output_rel->reltarget->exprs;
	ListCell   *lc;
	int			naggs;
	int			i;
	ColumnarAggSpec *specs;
	List	   *quals;
	int			npreds;
	bool		allConvertible;
	Path	   *cheapest;
	CustomPath *cpath;

	if (prev_create_upper_paths_hook)
		prev_create_upper_paths_hook(root, stage, input_rel, output_rel, extra);

	if (stage != UPPERREL_GROUP_AGG)
		return;
	if (!columnar_enable_vectorization || !columnar_enable_custom_scan)
		return;

	/* plain, ungrouped aggregation only (spec 9) */
	if (!parse->hasAggs)
		return;
	if (parse->groupClause != NIL || parse->groupingSets != NIL ||
		parse->havingQual != NULL || parse->distinctClause != NIL ||
		parse->hasWindowFuncs || parse->hasTargetSRFs)
		return;

	/* a single columnar base relation with no joins */
	if (input_rel->reloptkind != RELOPT_BASEREL)
		return;
	if (bms_membership(input_rel->relids) != BMS_SINGLETON)
		return;
	if (input_rel->relid == 0 ||
		input_rel->relid >= (Index) root->simple_rel_array_size)
		return;
	rte = root->simple_rte_array[input_rel->relid];
	if (rte == NULL || rte->rtekind != RTE_RELATION ||
		rte->relkind != RELKIND_RELATION)
		return;
	if (!OidIsValid(rte->relid) || !ColumnarIsColumnarRelation(rte->relid))
		return;
	relid = rte->relid;

	/* every target entry must be a bare, supported aggregate */
	naggs = list_length(tlist);
	if (naggs == 0)
		return;
	specs = (ColumnarAggSpec *) palloc0(sizeof(ColumnarAggSpec) * naggs);
	i = 0;
	foreach(lc, tlist)
	{
		Node	   *expr = (Node *) lfirst(lc);

		if (!IsA(expr, Aggref))
			return;
		if (!columnar_classify_aggref((Aggref *) expr, (int) input_rel->relid,
									  &specs[i]))
			return;
		i++;
	}

	/*
	 * Every restriction clause must convert to a predicate we evaluate exactly,
	 * so the vectorized filter is the complete WHERE. Otherwise fall back.
	 */
	quals = extract_actual_clauses(input_rel->baserestrictinfo, false);

	/*
	 * A native table answers an ungrouped aggregate from its zone maps without a
	 * data scan (native spec 7.1), but only when there is no residual filter; with
	 * a filter, fall back to the ordinary Agg over the skipping-enabled custom
	 * scan.
	 */
	if (quals != NIL)
		return;

	{
		Relation	rel = table_open(relid, AccessShareLock);
		TupleDesc	tupdesc = RelationGetDescr(rel);

		(void) ColumnarBuildVecPredicates(quals, input_rel->relid, tupdesc,
										  &npreds, &allConvertible);
		table_close(rel, AccessShareLock);
	}
	if (!allConvertible)
		return;

	cheapest = input_rel->cheapest_total_path;
	if (cheapest == NULL)
		return;

	cpath = makeNode(CustomPath);
	cpath->path.pathtype = T_CustomScan;
	cpath->path.parent = output_rel;
	cpath->path.pathtarget = output_rel->reltarget;
	cpath->path.param_info = NULL;
	cpath->path.parallel_aware = false;
	cpath->path.parallel_safe = false;
	cpath->path.parallel_workers = 0;
	cpath->path.rows = 1;

	/*
	 * The bare scan cost, with no per-tuple aggregate overhead: strictly cheaper
	 * than any Agg-over-scan path, so the planner prefers this one.
	 */
	cpath->path.startup_cost = cheapest->total_cost;
	cpath->path.total_cost = cheapest->total_cost;
	cpath->path.pathkeys = NIL;
	cpath->flags = 0;
	cpath->custom_paths = NIL;
#if PG_VERSION_NUM >= 170000
	cpath->custom_restrictinfo = NIL;
#endif
	cpath->custom_private =
		list_make3(makeInteger((int) input_rel->relid),
				   copyObject(quals),
				   makeConst(OIDOID, -1, InvalidOid, sizeof(Oid),
							 ObjectIdGetDatum(relid), false, true));
	cpath->methods = &columnar_agg_path_methods;

	add_path(output_rel, &cpath->path);
}

/* -------------------------------------------------------------------------
 * vectorized aggregate: execution
 * ------------------------------------------------------------------------- */

Node *
ColumnarCreateAggScanState(CustomScan *cscan)
{
	ColumnarAggScanState *state =
		(ColumnarAggScanState *) palloc0(sizeof(ColumnarAggScanState));
	int			naggs = list_length(cscan->custom_scan_tlist);
	ListCell   *lc;
	int			i = 0;

	state->css.ss.ps.type = T_CustomScanState;
	state->css.methods = &columnar_agg_exec_methods;

	/* custom_private: rti (Integer), quals (List), relid (Const OIDOID) */
	state->scanrelid = (Index) intVal(linitial(cscan->custom_private));
	state->quals = (List *) lsecond(cscan->custom_private);
	state->relid = DatumGetObjectId(((Const *) lthird(cscan->custom_private))->constvalue);

	/* rebuild the aggregate specs from the output tuple's aggregates */
	state->naggs = naggs;
	state->specs = (ColumnarAggSpec *) palloc0(sizeof(ColumnarAggSpec) * naggs);
	foreach(lc, cscan->custom_scan_tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);

		/* classified successfully at plan time; -1 skips the varno check */
		(void) columnar_classify_aggref((Aggref *) tle->expr, -1,
										&state->specs[i]);
		i++;
	}

	return (Node *) state;
}

static void
ColumnarBeginAggScan(CustomScanState *node, EState *estate, int eflags)
{
	ColumnarAggScanState *state = (ColumnarAggScanState *) node;
	Relation	rel;
	TupleDesc	tupdesc;
	bool		allConvertible;
	int			a;

	state->resultContext = AllocSetContextCreate(estate->es_query_cxt,
												 "columnar vec agg result",
												 ALLOCSET_SMALL_SIZES);
	state->done = false;
	state->haveStats = false;

	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	rel = table_open(state->relid, AccessShareLock);
	tupdesc = RelationGetDescr(rel);

	/* finish setting up min/max comparison info now that we have the tupdesc */
	for (a = 0; a < state->naggs; a++)
	{
		ColumnarAggSpec *spec = &state->specs[a];

		if (spec->kind == COLUMNAR_AGG_MIN || spec->kind == COLUMNAR_AGG_MAX)
		{
			Form_pg_attribute att = TupleDescAttr(tupdesc, spec->attidx);
			TypeCacheEntry *tce = lookup_type_cache(att->atttypid,
													TYPECACHE_CMP_PROC_FINFO);

			fmgr_info_copy(&spec->cmpFn, &tce->cmp_proc_finfo, estate->es_query_cxt);
			spec->collation = att->attcollation;
			spec->byval = att->attbyval;
			spec->typlen = att->attlen;
		}
	}

	state->preds = ColumnarBuildVecPredicates(state->quals, state->scanrelid,
											  tupdesc, &state->npreds,
											  &allConvertible);

	table_close(rel, AccessShareLock);
}

/*
 * columnar_apply_one
 *		Fold one value (or a null) into an aggregate accumulator. This is the
 *		reference per-row semantics, shared by the vectorized per-row path and
 *		the run path's fallback and min/max handling.
 */
static void
columnar_apply_one(ColumnarAggScanState *state, ColumnarAggSpec *spec,
				   Datum val, bool isnull)
{
	switch (spec->kind)
	{
		case COLUMNAR_AGG_COUNT_STAR:
			spec->count++;
			break;

		case COLUMNAR_AGG_COUNT_COL:
			if (!isnull)
				spec->count++;
			break;

		case COLUMNAR_AGG_SUM_INT:
			if (!isnull)
			{
				int64		v = (spec->inputType == INT2OID)
					? (int64) DatumGetInt16(val)
					: (int64) DatumGetInt32(val);

				spec->sum += v;
				spec->sawValue = true;
			}
			break;

		case COLUMNAR_AGG_AVG_INT:
			if (!isnull)
			{
				int64		v = (spec->inputType == INT2OID)
					? (int64) DatumGetInt16(val)
					: (int64) DatumGetInt32(val);

				spec->sum += v;
				spec->count++;
			}
			break;

		case COLUMNAR_AGG_MIN:
		case COLUMNAR_AGG_MAX:
			if (!isnull)
			{
				bool		take;

				if (!spec->sawValue)
					take = true;
				else
				{
					int32		cmp = DatumGetInt32(
						FunctionCall2Coll(&spec->cmpFn, spec->collation,
										  val, spec->extreme));

					take = (spec->kind == COLUMNAR_AGG_MIN)
						? (cmp < 0) : (cmp > 0);
				}

				if (take)
				{
					MemoryContext old =
						MemoryContextSwitchTo(state->resultContext);

					if (spec->sawValue && !spec->byval)
						pfree(DatumGetPointer(spec->extreme));
					spec->extreme = datumCopy(val, spec->byval, spec->typlen);
					spec->sawValue = true;
					MemoryContextSwitchTo(old);
				}
			}
			break;
	}
}


/*
 * columnar_run_agg
 *		Scan the base relation once and fold every chunk group into the aggregate
 *		accumulators. The reader (ColumnarBeginRead) applies min/max chunk-group
 *		skipping and the row mask. With no pushed-down predicates and fixed-width
 *		aggregate columns, groups are folded run-at-a-time over the value stream
 *		(I3 compressed execution); otherwise, and for groups with deletes, the
 *		per-row vectorized path is used. Returns the read state so the caller can
 *		read skip counters for EXPLAIN before ending it.
 */


/*
 * columnar_agg_finalize
 *		Turn one accumulator into its output Datum, reproducing PostgreSQL's
 *		aggregate result types and empty-input behaviour exactly.
 */
static Datum
columnar_agg_finalize(ColumnarAggSpec *spec, bool *isnull)
{
	*isnull = false;

	switch (spec->kind)
	{
		case COLUMNAR_AGG_COUNT_STAR:
		case COLUMNAR_AGG_COUNT_COL:
			return Int64GetDatum(spec->count);

		case COLUMNAR_AGG_SUM_INT:
			if (!spec->sawValue)
			{
				*isnull = true;
				return (Datum) 0;
			}
			return Int64GetDatum(spec->sum);	/* sum(int2/int4) -> int8 */

		case COLUMNAR_AGG_AVG_INT:
			if (spec->count == 0)
			{
				*isnull = true;
				return (Datum) 0;
			}
			else
			{
				/* avg(int) -> numeric, exactly as int8_avg: sum/count in numeric */
				Datum		sumd = DirectFunctionCall1(int8_numeric,
													   Int64GetDatum(spec->sum));
				Datum		cntd = DirectFunctionCall1(int8_numeric,
													   Int64GetDatum(spec->count));

				return DirectFunctionCall2(numeric_div, sumd, cntd);
			}

		case COLUMNAR_AGG_MIN:
		case COLUMNAR_AGG_MAX:
			if (!spec->sawValue)
			{
				*isnull = true;
				return (Datum) 0;
			}
			return spec->extreme;
	}

	*isnull = true;
	return (Datum) 0;
}

/*
 * columnar_fill_native_metadata_agg
 *		Answer an ungrouped, unfiltered aggregate over a native (PGCN v1) table
 *		entirely from its whole-chunk zone maps (native spec 7.1, D5b): count(*)
 *		from row-group row counts, count(col) and the avg count from value_count,
 *		sum and the avg sum from the zone int sum (int2/int4), and min/max from the
 *		zone min/max. The upper-path hook adds this path only when every aggregate
 *		is so answerable and there is no filter, so no data pages are read.
 */
static void
columnar_fill_native_metadata_agg(ColumnarAggScanState *state)
{
	EState	   *estate = state->css.ss.ps.state;
	Relation	rel;
	TupleDesc	tupdesc;
	Snapshot	snap;
	uint64		storageId;
	List	   *groups;
	ListCell   *lc;

	ColumnarFlushWriteStateForRelation(state->relid);
	rel = table_open(state->relid, AccessShareLock);
	tupdesc = RelationGetDescr(rel);
	snap = ColumnarCatalogSnapshot(estate->es_snapshot);
	storageId = ColumnarStorageId(rel);

	groups = ColumnarReadRowGroupList(storageId, snap);
	foreach(lc, groups)
	{
		NativeRowGroupMetadata *rg = (NativeRowGroupMetadata *) lfirst(lc);
		List	   *zones = ColumnarReadZoneMapList(storageId, rg->groupNumber,
												   snap);
		NativeZoneMapMetadata **byCol =
			palloc0(sizeof(NativeZoneMapMetadata *) * tupdesc->natts);
		ListCell   *zc;
		int			a;

		foreach(zc, zones)
		{
			NativeZoneMapMetadata *z = (NativeZoneMapMetadata *) lfirst(zc);

			if (z->columnIndex >= 0 && z->columnIndex < tupdesc->natts)
				byCol[z->columnIndex] = z;
		}

		for (a = 0; a < state->naggs; a++)
		{
			ColumnarAggSpec *spec = &state->specs[a];
			NativeZoneMapMetadata *z =
				(spec->attidx >= 0 && spec->attidx < tupdesc->natts)
				? byCol[spec->attidx] : NULL;

			switch (spec->kind)
			{
				case COLUMNAR_AGG_COUNT_STAR:
					spec->count += (int64) rg->rowCount;
					break;
				case COLUMNAR_AGG_COUNT_COL:
					if (z != NULL)
						spec->count += (int64) z->valueCount;
					break;
				case COLUMNAR_AGG_SUM_INT:
					if (z != NULL && z->hasSum)
					{
						spec->sum += DatumGetInt64(
							DirectFunctionCall1(numeric_int8, z->sum));
						if (z->valueCount > 0)
							spec->sawValue = true;
					}
					break;
				case COLUMNAR_AGG_AVG_INT:
					if (z != NULL)
					{
						if (z->hasSum)
							spec->sum += DatumGetInt64(
								DirectFunctionCall1(numeric_int8, z->sum));
						spec->count += (int64) z->valueCount;
					}
					break;
				case COLUMNAR_AGG_MIN:
				case COLUMNAR_AGG_MAX:
					if (z != NULL && z->hasMinMax)
					{
						Form_pg_attribute att = TupleDescAttr(tupdesc, spec->attidx);
						MemoryContext oldcx =
							MemoryContextSwitchTo(state->resultContext);
						char	   *cur = (spec->kind == COLUMNAR_AGG_MIN)
							? (char *) z->minimum : (char *) z->maximum;
						Datum		v = ColumnarDecodeValue(att, &cur,
														state->resultContext);

						if (!spec->sawValue)
						{
							spec->extreme = v;
							spec->sawValue = true;
						}
						else
						{
							int32		c = DatumGetInt32(
								FunctionCall2Coll(&spec->cmpFn, spec->collation,
												  v, spec->extreme));

							if ((spec->kind == COLUMNAR_AGG_MIN && c < 0) ||
								(spec->kind == COLUMNAR_AGG_MAX && c > 0))
								spec->extreme = v;
						}
						MemoryContextSwitchTo(oldcx);
					}
					break;
			}
		}
	}

	table_close(rel, AccessShareLock);
}

/*
 * columnar_native_scan_agg
 *		Fold an ungrouped, unfiltered aggregate over a native table by scanning it
 *		one row at a time (ColumnarReadNextRow applies the delete mask), for the
 *		case where the zone-map-only path cannot be used because the storage has
 *		deletes (D6b). No quals: the upper-path hook only adds the native agg path
 *		when there is no filter.
 */
static void
columnar_native_scan_agg(ColumnarAggScanState *state)
{
	EState	   *estate = state->css.ss.ps.state;
	Relation	rel = table_open(state->relid, AccessShareLock);
	TupleDesc	tupdesc = RelationGetDescr(rel);
	Bitmapset  *projected = NULL;
	ColumnarReadState *rs;
	Datum	   *values = (Datum *) palloc(sizeof(Datum) * tupdesc->natts);
	bool	   *nulls = (bool *) palloc(sizeof(bool) * tupdesc->natts);
	uint64		rowNumber;
	int			a;

	for (a = 0; a < state->naggs; a++)
		if (state->specs[a].attidx >= 0)
			projected = bms_add_member(projected, state->specs[a].attidx);
	if (projected == NULL)
		projected = bms_make_singleton(0);	/* count(*) only: one column */

	ColumnarFlushWriteStateForRelation(state->relid);
	ColumnarFlushRowMaskForRelation(rel);

	rs = ColumnarBeginRead(rel, estate->es_snapshot, NULL, projected, 0, NULL);
	while (ColumnarReadNextRow(rs, values, nulls, &rowNumber))
	{
		for (a = 0; a < state->naggs; a++)
		{
			ColumnarAggSpec *spec = &state->specs[a];

			if (spec->attidx >= 0)
				columnar_apply_one(state, spec, values[spec->attidx],
								   nulls[spec->attidx]);
			else
				columnar_apply_one(state, spec, (Datum) 0, true);
		}
	}
	ColumnarEndRead(rs);
	table_close(rel, AccessShareLock);
}

static TupleTableSlot *
ColumnarExecAggScan(CustomScanState *node)
{
	ColumnarAggScanState *state = (ColumnarAggScanState *) node;
	TupleTableSlot *scanSlot = node->ss.ss_ScanTupleSlot;
	ExprContext *econtext = node->ss.ps.ps_ExprContext;
	TupleTableSlot *result;
	int			a;
	EState	   *estate = node->ss.ps.state;
	Relation	frel;
	bool		hasDeletes;

	if (state->done)
		return NULL;
	state->done = true;

	/*
	 * A native table answers from zone maps when no rows are deleted (native spec
	 * 7.1): the upper-path hook added this path only when every aggregate is
	 * zone-map answerable and there is no filter, so no data pages are read. When
	 * the storage has deletes the zone maps include deleted rows, so fall back to a
	 * scan that applies the delete mask.
	 */
	frel = table_open(state->relid, AccessShareLock);
	ColumnarFlushWriteStateForRelation(state->relid);
	ColumnarFlushRowMaskForRelation(frel);
	hasDeletes = ColumnarStorageHasRowMask(ColumnarStorageId(frel),
										   ColumnarCatalogSnapshot(estate->es_snapshot));
	table_close(frel, AccessShareLock);

	if (hasDeletes)
		columnar_native_scan_agg(state);
	else
		columnar_fill_native_metadata_agg(state);
	state->haveStats = false;

	/* build the single result row from the finalized aggregates */
	ExecClearTuple(scanSlot);
	for (a = 0; a < state->naggs; a++)
		scanSlot->tts_values[a] =
			columnar_agg_finalize(&state->specs[a], &scanSlot->tts_isnull[a]);
	ExecStoreVirtualTuple(scanSlot);

	/*
	 * Project to the result tuple when the executor built a projection; when the
	 * output matches the scan tuple exactly it left ps_ProjInfo NULL and the scan
	 * slot is the result.
	 */
	if (node->ss.ps.ps_ProjInfo != NULL)
	{
		econtext->ecxt_scantuple = scanSlot;
		result = ExecProject(node->ss.ps.ps_ProjInfo);
	}
	else
		result = scanSlot;

	return result;
}

static void
ColumnarEndAggScan(CustomScanState *node)
{
	/* the reader is ended inside ColumnarExecAggScan; nothing else to free */
}

static void
ColumnarReScanAggScan(CustomScanState *node)
{
	ColumnarAggScanState *state = (ColumnarAggScanState *) node;
	int			a;

	state->done = false;
	state->haveStats = false;
	MemoryContextReset(state->resultContext);
	for (a = 0; a < state->naggs; a++)
	{
		ColumnarAggSpec *spec = &state->specs[a];

		spec->count = 0;
		spec->sum = 0;
		spec->sawValue = false;
		spec->extreme = (Datum) 0;
	}
}

static void
ColumnarExplainAggScan(CustomScanState *node, List *ancestors, ExplainState *es)
{
	ColumnarAggScanState *state = (ColumnarAggScanState *) node;

	ExplainPropertyInteger("Columnar Vectorized Aggregates", NULL,
						   state->naggs, es);
	ExplainPropertyInteger("Columnar Pushed-Down Filters", NULL,
						   state->npreds, es);

	if (state->haveStats)
	{
		ExplainPropertyInteger("Columnar Chunk Groups Total", NULL,
							   (int64) state->groupsTotal, es);
		ExplainPropertyInteger("Columnar Chunk Groups Read", NULL,
							   (int64) state->groupsRead, es);
		ExplainPropertyInteger("Columnar Chunk Groups Removed by Filter", NULL,
							   (int64) state->groupsSkipped, es);
	}
}

static const CustomExecMethods columnar_agg_exec_methods = {
	.CustomName = "ColumnarScan",
	.BeginCustomScan = ColumnarBeginAggScan,
	.ExecCustomScan = ColumnarExecAggScan,
	.EndCustomScan = ColumnarEndAggScan,
	.ReScanCustomScan = ColumnarReScanAggScan,
	.ExplainCustomScan = ColumnarExplainAggScan,
};

/* -------------------------------------------------------------------------
 * registration
 * ------------------------------------------------------------------------- */

void
ColumnarVectorInit(void)
{
	prev_create_upper_paths_hook = create_upper_paths_hook;
	create_upper_paths_hook = ColumnarCreateUpperPaths;
}
