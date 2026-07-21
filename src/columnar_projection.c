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
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

PG_FUNCTION_INFO_V1(columnar_add_projection);
PG_FUNCTION_INFO_V1(columnar_drop_projection);

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

	rel = table_open(relid, ShareUpdateExclusiveLock);
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

	table_close(rel, ShareUpdateExclusiveLock);
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
	storageId = ColumnarStorageId(rel);
	existing = ColumnarListProjections(storageId);

	foreach(lc, existing)
	{
		ColumnarProjection *p = (ColumnarProjection *) lfirst(lc);

		if (strcmp(p->name, projname) == 0)
		{
			targetId = p->projectionId;
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
	 * free yet; later phases free the projection's stripes here. */
	ColumnarDeleteProjectionRow(storageId, targetId);

	table_close(rel, ShareUpdateExclusiveLock);
	PG_RETURN_VOID();
}
