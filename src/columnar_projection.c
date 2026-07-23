/*-------------------------------------------------------------------------
 *
 * columnar_projection.c
 *		DDL for multiple physical projections (gap 26, format 2.2).
 *
 * A projection is a named, ordered subset of a table's columns stored as its
 * own columnar storage, sorted on its sort key, sharing the table's row-number
 * identity space (the C-Store model; see design/gaps/26-*). Phase 1 provides the
 * catalog (columnar.projection) and the add/drop DDL only: declaring a
 * projection allocates its storage id and records the row, but no data is
 * written to a projection's storage yet (write fan-out is phase 2).
 *
 * projection_id 0 is the implicit base projection (all live columns, insert
 * order). A table with no columnar.projection rows has a single implicit base
 * projection, so pre-existing and 2.0/2.1 tables are unaffected. The base row is
 * recorded lazily the first time a projection is added.
 *
 * Written fresh for pgColumnar; it does not reuse any upstream file.
 *
 *-------------------------------------------------------------------------
 */
#include "columnar.h"

#include "access/table.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/tuplestore.h"

PG_FUNCTION_INFO_V1(columnar_add_projection);
PG_FUNCTION_INFO_V1(columnar_drop_projection);
PG_FUNCTION_INFO_V1(columnar_read_projection);

/*
 * Collect the live (non-dropped) attribute numbers of a relation, in attnum
 * order. Returns a palloc'd int16 array and sets *n.
 */
static int16 *
live_attnums(Relation rel, int *n)
{
	TupleDesc	tupdesc = RelationGetDescr(rel);
	int16	   *out = palloc(sizeof(int16) * tupdesc->natts);
	int			count = 0;
	int			i;

	for (i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		if (att->attisdropped)
			continue;
		out[count++] = (int16) att->attnum;
	}
	*n = count;
	return out;
}

/*
 * Resolve a text[] of column names to an int16 array of attnums against relid,
 * validating that each names a live user column and that there are no
 * duplicates. Returns a palloc'd array and sets *n. Errors on any problem.
 */
static int16 *
resolve_columns(Oid relid, ArrayType *names, const char *what, int *n)
{
	Datum	   *elems;
	bool	   *nulls;
	int			count;
	int16	   *out;
	int			i,
				j;

	deconstruct_array(names, TEXTOID, -1, false, TYPALIGN_INT,
					  &elems, &nulls, &count);

	out = (count > 0) ? palloc(sizeof(int16) * count) : NULL;
	for (i = 0; i < count; i++)
	{
		char	   *colname;
		AttrNumber	attno;

		if (nulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("%s must not contain NULL", what)));

		colname = text_to_cstring(DatumGetTextPP(elems[i]));
		attno = get_attnum(relid, colname);
		if (attno == InvalidAttrNumber || attno < 0)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" does not exist", colname)));

		for (j = 0; j < i; j++)
			if (out[j] == (int16) attno)
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_COLUMN),
						 errmsg("column \"%s\" appears more than once in %s",
								colname, what)));
		out[i] = (int16) attno;
	}
	*n = count;
	return out;
}

/*
 * Ensure the base projection row (projection_id 0) exists for this table,
 * recording all live columns in insert order. No-op if already present.
 */
static void
record_base_projection(Relation rel, uint64 storageId, List *existing)
{
	ListCell   *lc;
	ColumnarProjection base;

	foreach(lc, existing)
	{
		ColumnarProjection *p = (ColumnarProjection *) lfirst(lc);

		if (p->projectionId == 0)
			return;
	}

	memset(&base, 0, sizeof(base));
	base.storageId = storageId;
	base.projectionId = 0;
	base.name = "base";
	base.projStorageId = storageId;		/* base shares the table's storage */
	base.sortKey = NULL;
	base.sortKeyLen = 0;
	base.columns = live_attnums(rel, &base.columnsLen);
	ColumnarInsertProjectionRow(&base);
}

/*
 * columnar.add_projection(rel, name, columns text[], sort_key text[])
 *		Declare a projection: a named column subset sorted on sort_key.
 */
Datum
columnar_add_projection(PG_FUNCTION_ARGS)
{
	Oid			relid;
	char	   *projname;
	ArrayType  *colsArr;
	ArrayType  *sortArr;
	Relation	rel;
	uint64		storageId;
	List	   *existing;
	ListCell   *lc;
	ColumnarProjection proj;
	int			nextId = 1;
	int			i,
				j;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("rel, name, and columns must not be NULL")));

	relid = PG_GETARG_OID(0);
	projname = text_to_cstring(PG_GETARG_TEXT_PP(1));
	colsArr = PG_GETARG_ARRAYTYPE_P(2);
	sortArr = PG_ARGISNULL(3) ? NULL : PG_GETARG_ARRAYTYPE_P(3);

	if (!ColumnarIsColumnarRelation(relid))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a columnar table",
						get_rel_name(relid))));

	if (strlen(projname) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("projection name must not be empty")));
	if (strlen(projname) >= NAMEDATALEN)
		ereport(ERROR,
				(errcode(ERRCODE_NAME_TOO_LONG),
				 errmsg("projection name \"%s\" is too long", projname)));

	/*
	 * ShareLock: block concurrent INSERT/UPDATE/DELETE (RowExclusiveLock) while
	 * we back-fill the projection from existing rows, so no concurrently written
	 * row is missed -- the same lock non-concurrent CREATE INDEX takes. Reads are
	 * unaffected. (A CONCURRENTLY variant is future work.)
	 */
	rel = table_open(relid, ShareLock);
	ColumnarRequireTableOwner(rel);
	storageId = ColumnarStorageId(rel);

	existing = ColumnarListProjections(storageId);
	record_base_projection(rel, storageId, existing);
	/* re-read so the base row is included when picking the next id / name check */
	existing = ColumnarListProjections(storageId);

	memset(&proj, 0, sizeof(proj));
	proj.storageId = storageId;
	proj.name = projname;
	proj.columns = resolve_columns(relid, colsArr, "columns", &proj.columnsLen);
	if (proj.columnsLen == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a projection must have at least one column")));

	if (sortArr != NULL)
		proj.sortKey = resolve_columns(relid, sortArr, "sort_key", &proj.sortKeyLen);

	/* every sort_key column must be part of the projection's columns */
	for (i = 0; i < proj.sortKeyLen; i++)
	{
		bool		found = false;

		for (j = 0; j < proj.columnsLen; j++)
			if (proj.columns[j] == proj.sortKey[i])
			{
				found = true;
				break;
			}
		if (!found)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("sort_key column \"%s\" is not in the projection column list",
							get_attname(relid, proj.sortKey[i], false))));
	}

	/* name must be unique for this table; next id is max + 1 */
	foreach(lc, existing)
	{
		ColumnarProjection *p = (ColumnarProjection *) lfirst(lc);

		if (strcmp(p->name, projname) == 0)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("projection \"%s\" already exists on \"%s\"",
							projname, get_rel_name(relid))));
		if (p->projectionId >= nextId)
			nextId = p->projectionId + 1;
	}

	proj.projectionId = nextId;
	proj.projStorageId = ColumnarNextStorageId();
	ColumnarInsertProjectionRow(&proj);

	/* populate the projection from the table's existing rows (gap 26 back-fill) */
	ColumnarBackfillProjection(rel, &proj);

	table_close(rel, ShareLock);
	PG_RETURN_VOID();
}

/*
 * columnar.drop_projection(rel, name)
 *		Drop a declared projection. The base projection cannot be dropped.
 */
Datum
columnar_drop_projection(PG_FUNCTION_ARGS)
{
	Oid			relid;
	char	   *projname;
	Relation	rel;
	uint64		storageId;
	List	   *existing;
	ListCell   *lc;
	int			targetId = -1;
	uint64		targetStorageId = 0;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("rel and name must not be NULL")));

	relid = PG_GETARG_OID(0);
	projname = text_to_cstring(PG_GETARG_TEXT_PP(1));

	if (!ColumnarIsColumnarRelation(relid))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a columnar table",
						get_rel_name(relid))));

	rel = table_open(relid, ShareUpdateExclusiveLock);
	ColumnarRequireTableOwner(rel);
	storageId = ColumnarStorageId(rel);
	existing = ColumnarListProjections(storageId);

	foreach(lc, existing)
	{
		ColumnarProjection *p = (ColumnarProjection *) lfirst(lc);

		if (strcmp(p->name, projname) == 0)
		{
			targetId = p->projectionId;
			targetStorageId = p->projStorageId;
			break;
		}
	}

	if (targetId < 0)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("projection \"%s\" does not exist on \"%s\"",
						projname, get_rel_name(relid))));
	if (targetId == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the base projection cannot be dropped")));

	/* phase 1 writes no data to a projection's storage, so there is nothing to
	 * free yet; later phases free the projection's stripes here. Purge any block-1
	 * free entries for the dropped storage so they cannot orphan (offset 0 = all). */
	ColumnarDeleteProjectionRow(storageId, targetId);
	ColumnarDeleteFreeSpaceAtOrAbove(rel, targetStorageId, 0);

	table_close(rel, ShareUpdateExclusiveLock);
	PG_RETURN_VOID();
}

/*
 * columnar.read_projection(rel, name) -> setof text
 *		Debug/verification reader for a projection's storage (gap 26, phase 2).
 *		Scans the projection's stripes (in stored sort order), skips rows whose
 *		base row number is deleted or invisible per the base delete_vector/visibility,
 *		and returns each live row's projection columns rendered by their output
 *		functions and joined by '|' (a NULL column renders as \N). The base is
 *		flushed first so rows written earlier in this transaction are visible.
 */
Datum
columnar_read_projection(PG_FUNCTION_ARGS)
{
	Oid			relid;
	char	   *projname;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	rel;
	uint64		storageId;
	List	   *projs;
	ListCell   *lc;
	ColumnarProjection *proj = NULL;
	TupleDesc	projTupdesc;
	TupleDesc	retdesc;
	Tuplestorestate *tupstore;
	MemoryContext oldContext;
	FmgrInfo   *outFns;
	int			ncols;
	int			tnatts;
	int			i;
	Snapshot	snap;
	ColumnarReadState *readState;
	Datum	   *rvals;
	bool	   *rnulls;
	Datum	   *basevals;
	bool	   *basenulls;
	uint64		projRowNum;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("rel and name must not be NULL")));
	relid = PG_GETARG_OID(0);
	projname = text_to_cstring(PG_GETARG_TEXT_PP(1));

	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo) ||
		!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));

	if (!ColumnarIsColumnarRelation(relid))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a columnar table", get_rel_name(relid))));

	rel = table_open(relid, AccessShareLock);
	/* persist pending base + projection writes so this read sees them */
	ColumnarFlushWriteStateForRelation(relid);
	storageId = ColumnarStorageId(rel);

	projs = ColumnarListProjections(storageId);
	foreach(lc, projs)
	{
		ColumnarProjection *p = (ColumnarProjection *) lfirst(lc);

		if (strcmp(p->name, projname) == 0)
		{
			proj = p;
			break;
		}
	}
	if (proj == NULL || proj->projectionId == 0)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("projection \"%s\" does not exist on \"%s\"",
						projname, get_rel_name(relid))));

	ncols = proj->columnsLen;
	tnatts = RelationGetDescr(rel)->natts;

	/* projection storage layout: [rownumber int8, projcol1..projcolK] */
	projTupdesc = CreateTemplateTupleDesc(ncols + 1);
	TupleDescInitEntry(projTupdesc, 1, "rownumber", INT8OID, -1, 0);
	for (i = 0; i < ncols; i++)
		TupleDescCopyEntry(projTupdesc, i + 2, RelationGetDescr(rel),
						   (AttrNumber) proj->columns[i]);

	outFns = palloc(sizeof(FmgrInfo) * ncols);
	for (i = 0; i < ncols; i++)
	{
		Form_pg_attribute att = TupleDescAttr(projTupdesc, i + 1);
		Oid			outOid;
		bool		isVarlena;

		getTypeOutputInfo(att->atttypid, &outOid, &isVarlena);
		fmgr_info(outOid, &outFns[i]);
	}

	/* one text column result, materialized into a tuplestore */
	retdesc = CreateTemplateTupleDesc(1);
	TupleDescInitEntry(retdesc, 1, "row", TEXTOID, -1, 0);

	oldContext = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);
	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = retdesc;
	MemoryContextSwitchTo(oldContext);

	snap = GetActiveSnapshot();
	rvals = palloc(sizeof(Datum) * (ncols + 1));
	rnulls = palloc(sizeof(bool) * (ncols + 1));
	basevals = palloc(sizeof(Datum) * tnatts);
	basenulls = palloc(sizeof(bool) * tnatts);

	readState = ColumnarBeginReadWithStorage(rel, snap, proj->projStorageId,
											 projTupdesc, NULL, NULL, 0, NULL);

	while (ColumnarReadNextRow(readState, rvals, rnulls, &projRowNum))
	{
		uint64		baseRow = (uint64) DatumGetInt64(rvals[0]);
		StringInfoData buf;
		Datum		result;
		bool		resnull = false;

		/* deletes/visibility come from the base */
		if (!ColumnarReadRowByNumber(rel, snap, baseRow, basevals, basenulls))
			continue;

		initStringInfo(&buf);
		for (i = 0; i < ncols; i++)
		{
			if (i > 0)
				appendStringInfoChar(&buf, '|');
			if (rnulls[i + 1])
				appendStringInfoString(&buf, "\\N");
			else
				appendStringInfoString(&buf,
									   OutputFunctionCall(&outFns[i], rvals[i + 1]));
		}

		result = CStringGetTextDatum(buf.data);
		tuplestore_putvalues(tupstore, retdesc, &result, &resnull);
		pfree(buf.data);
	}

	ColumnarEndRead(readState);
	table_close(rel, AccessShareLock);

	return (Datum) 0;
}

/*
 * columnar.reconstruct_via_projection(rel, name) -> setof text
 *		Read every live row through a projection and reconstruct the full base
 *		row (gap 26, phase 3): columns stored in the projection come from the
 *		projection's own storage, and any remaining table columns are fetched
 *		from the base by the projection's stored row number. This exercises the
 *		row-number join that the planner uses (phase 4) when a chosen projection
 *		does not cover every referenced column. All live table columns are
 *		rendered by their output functions and joined by '|'.
 */
PG_FUNCTION_INFO_V1(columnar_reconstruct_via_projection);
Datum
columnar_reconstruct_via_projection(PG_FUNCTION_ARGS)
{
	Oid			relid;
	char	   *projname;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	rel;
	TupleDesc	tableDesc;
	uint64		storageId;
	List	   *projs;
	ListCell   *lc;
	ColumnarProjection *proj = NULL;
	TupleDesc	projTupdesc;
	TupleDesc	retdesc;
	Tuplestorestate *tupstore;
	MemoryContext oldContext;
	FmgrInfo   *outFns;			/* per table column */
	int		   *covered;		/* table col -> index into projection rvals, or -1 */
	int			ncols;
	int			tnatts;
	int			i;
	Snapshot	snap;
	ColumnarReadState *readState;
	Datum	   *rvals;
	bool	   *rnulls;
	Datum	   *basevals;
	bool	   *basenulls;
	uint64		projRowNum;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("rel and name must not be NULL")));
	relid = PG_GETARG_OID(0);
	projname = text_to_cstring(PG_GETARG_TEXT_PP(1));

	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo) ||
		!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));

	if (!ColumnarIsColumnarRelation(relid))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a columnar table", get_rel_name(relid))));

	rel = table_open(relid, AccessShareLock);
	ColumnarFlushWriteStateForRelation(relid);
	tableDesc = RelationGetDescr(rel);
	tnatts = tableDesc->natts;
	storageId = ColumnarStorageId(rel);

	projs = ColumnarListProjections(storageId);
	foreach(lc, projs)
	{
		ColumnarProjection *p = (ColumnarProjection *) lfirst(lc);

		if (strcmp(p->name, projname) == 0)
		{
			proj = p;
			break;
		}
	}
	if (proj == NULL || proj->projectionId == 0)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("projection \"%s\" does not exist on \"%s\"",
						projname, get_rel_name(relid))));

	ncols = proj->columnsLen;

	/* projection storage layout: [rownumber int8, projcol1..projcolK] */
	projTupdesc = CreateTemplateTupleDesc(ncols + 1);
	TupleDescInitEntry(projTupdesc, 1, "rownumber", INT8OID, -1, 0);
	for (i = 0; i < ncols; i++)
		TupleDescCopyEntry(projTupdesc, i + 2, tableDesc,
						   (AttrNumber) proj->columns[i]);

	/* map each table column to its position in the projection row, or -1 */
	covered = palloc(sizeof(int) * tnatts);
	for (i = 0; i < tnatts; i++)
	{
		int			p;

		covered[i] = -1;
		for (p = 0; p < ncols; p++)
			if ((int) proj->columns[p] == i + 1)
			{
				covered[i] = p + 1;		/* +1: rvals[0] is the row number */
				break;
			}
	}

	outFns = palloc(sizeof(FmgrInfo) * tnatts);
	for (i = 0; i < tnatts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tableDesc, i);
		Oid			outOid;
		bool		isVarlena;

		if (att->attisdropped)
			continue;
		getTypeOutputInfo(att->atttypid, &outOid, &isVarlena);
		fmgr_info(outOid, &outFns[i]);
	}

	retdesc = CreateTemplateTupleDesc(1);
	TupleDescInitEntry(retdesc, 1, "row", TEXTOID, -1, 0);

	oldContext = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);
	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = retdesc;
	MemoryContextSwitchTo(oldContext);

	snap = GetActiveSnapshot();
	rvals = palloc(sizeof(Datum) * (ncols + 1));
	rnulls = palloc(sizeof(bool) * (ncols + 1));
	basevals = palloc(sizeof(Datum) * tnatts);
	basenulls = palloc(sizeof(bool) * tnatts);

	readState = ColumnarBeginReadWithStorage(rel, snap, proj->projStorageId,
											 projTupdesc, NULL, NULL, 0, NULL);

	while (ColumnarReadNextRow(readState, rvals, rnulls, &projRowNum))
	{
		uint64		baseRow = (uint64) DatumGetInt64(rvals[0]);
		StringInfoData buf;
		Datum		result;
		bool		resnull = false;
		bool		first = true;

		/* fetch the base row (liveness + any non-covered columns) */
		if (!ColumnarReadRowByNumber(rel, snap, baseRow, basevals, basenulls))
			continue;

		initStringInfo(&buf);
		for (i = 0; i < tnatts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(tableDesc, i);
			Datum		v;
			bool		isnull;

			if (att->attisdropped)
				continue;
			if (!first)
				appendStringInfoChar(&buf, '|');
			first = false;

			if (covered[i] >= 0)
			{
				v = rvals[covered[i]];
				isnull = rnulls[covered[i]];
			}
			else
			{
				v = basevals[i];
				isnull = basenulls[i];
			}

			if (isnull)
				appendStringInfoString(&buf, "\\N");
			else
				appendStringInfoString(&buf, OutputFunctionCall(&outFns[i], v));
		}

		result = CStringGetTextDatum(buf.data);
		tuplestore_putvalues(tupstore, retdesc, &result, &resnull);
		pfree(buf.data);
	}

	ColumnarEndRead(readState);
	table_close(rel, AccessShareLock);

	return (Datum) 0;
}
