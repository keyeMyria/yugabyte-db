/*--------------------------------------------------------------------------------------------------
 *
 * ybcModifyTable.c
 *        YB routines to stmt_handle ModifyTable nodes.
 *
 * Copyright (c) YugaByte, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
 * in compliance with the License.  You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License
 * is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
 * or implied.  See the License for the specific language governing permissions and limitations
 * under the License.
 *
 * IDENTIFICATION
 *        src/backend/executor/ybcModifyTable.c
 *
 *--------------------------------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "catalog/ybctype.h"
#include "utils/relcache.h"
#include "utils/rel.h"
#include "utils/lsyscache.h"
#include "nodes/execnodes.h"
#include "commands/dbcommands.h"
#include "executor/tuptable.h"
#include "executor/ybcExpr.h"
#include "executor/ybcModifyTable.h"
#include "miscadmin.h"
#include "catalog/catalog.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_database.h"
#include "utils/catcache.h"
#include "utils/inval.h"
#include "utils/relcache.h"
#include "utils/rel.h"
#include "executor/tuptable.h"
#include "executor/ybcExpr.h"

#include "utils/syscache.h"
#include "yb/yql/pggate/ybc_pggate.h"
#include "pg_yb_utils.h"

/*
 * Hack to ensure that the next CommandCounterIncrement() will call
 * CommandEndInvalidationMessages(). The result of this call is not
 * needed on the yb side, however the side effects are.
 */
void MarkCurrentCommandUsed() {
	(void) GetCurrentCommandId(true);
}

/*
 * Returns whether a relation's attribute is a real column in the backing
 * YugaByte table. (It implies we can both read from and write to it).
 */
bool IsRealYBColumn(Relation rel, int attrNum)
{
	return (attrNum > 0 && !TupleDescAttr(rel->rd_att, attrNum - 1)->attisdropped) ||
	       (rel->rd_rel->relhasoids && attrNum == ObjectIdAttributeNumber);
}

/*
 * Returns whether a relation's attribute is a YB system column.
 */
bool IsYBSystemColumn(int attrNum)
{
	return (attrNum == YBRowIdAttributeNumber ||
			attrNum == YBIdxBaseTupleIdAttributeNumber ||
			attrNum == YBUniqueIdxKeySuffixAttributeNumber);
}

/*
 * Get the type ID of a real or virtual attribute (column).
 * Returns InvalidOid if the attribute number is invalid.
 */
static Oid GetTypeId(int attrNum, TupleDesc tupleDesc)
{
	switch (attrNum)
	{
		case SelfItemPointerAttributeNumber:
			return TIDOID;
		case ObjectIdAttributeNumber:
			return OIDOID;
		case MinTransactionIdAttributeNumber:
			return XIDOID;
		case MinCommandIdAttributeNumber:
			return CIDOID;
		case MaxTransactionIdAttributeNumber:
			return XIDOID;
		case MaxCommandIdAttributeNumber:
			return CIDOID;
		case TableOidAttributeNumber:
			return OIDOID;
		default:
			if (attrNum > 0 && attrNum <= tupleDesc->natts)
				return TupleDescAttr(tupleDesc, attrNum - 1)->atttypid;
			else
				return InvalidOid;
	}
}

/*
 * Get primary key columns as bitmap of a table.
 */
static Bitmapset *GetTablePrimaryKey(Relation rel,
									 AttrNumber minattr,
									 bool includeYBSystemColumns)
{
	Oid            dboid         = YBCGetDatabaseOid(rel);
	Oid            relid         = RelationGetRelid(rel);
	int            natts         = RelationGetNumberOfAttributes(rel);
	Bitmapset      *pkey         = NULL;
	YBCPgTableDesc ybc_tabledesc = NULL;

	/* Get the primary key columns 'pkey' from YugaByte. */
	HandleYBStatus(YBCPgGetTableDesc(ybc_pg_session, dboid, relid, &ybc_tabledesc));
	for (AttrNumber attnum = minattr; attnum <= natts; attnum++)
	{
		if ((!includeYBSystemColumns && !IsRealYBColumn(rel, attnum)) ||
			(!IsRealYBColumn(rel, attnum) && !IsYBSystemColumn(attnum)))
		{
			continue;
		}

		bool is_primary = false;
		bool is_hash    = false;
		HandleYBTableDescStatus(YBCPgGetColumnInfo(ybc_tabledesc,
		                                           attnum,
		                                           &is_primary,
		                                           &is_hash), ybc_tabledesc);
		if (is_primary)
		{
			pkey = bms_add_member(pkey, attnum - minattr);
		}
	}
	HandleYBStatus(YBCPgDeleteTableDesc(ybc_tabledesc));

	return pkey;
}

/*
 * Get primary key columns as bitmap of a table for real YB columns.
 */
static Bitmapset *GetYBTablePrimaryKey(Relation rel)
{
	return GetTablePrimaryKey(rel, FirstLowInvalidHeapAttributeNumber + 1 /* minattr */,
							  false /* includeYBSystemColumns */);
}

/*
 * Get primary key columns as bitmap of a table for real and system YB columns.
 */
static Bitmapset *GetFullYBTablePrimaryKey(Relation rel)
{
	return GetTablePrimaryKey(rel, YBSystemFirstLowInvalidAttributeNumber + 1 /* minattr */,
							  true /* includeYBSystemColumns */);
}

/*
 * Get the ybctid from a YB scan slot for UPDATE/DELETE.
 */
Datum YBCGetYBTupleIdFromSlot(TupleTableSlot *slot)
{
	/*
	 * Look for ybctid in the tuple first if the slot contains a tuple packed with ybctid.
	 * Otherwise, look for it in the attribute list as a junk attribute.
	 */
	if (slot->tts_tuple != NULL && slot->tts_tuple->t_ybctid != 0)
	{
		return slot->tts_tuple->t_ybctid;
	}

	for (int idx = 0; idx < slot->tts_nvalid; idx++)
	{
		Form_pg_attribute att = TupleDescAttr(slot->tts_tupleDescriptor, idx);
		if (strcmp(NameStr(att->attname), "ybctid") == 0 && !slot->tts_isnull[idx])
		{
			Assert(att->atttypid == BYTEAOID);
			return slot->tts_values[idx];
		}
	}

	return 0;
}

/*
 * Get the ybctid from a tuple.
 */
Datum YBCGetYBTupleIdFromTuple(YBCPgStatement pg_stmt,
							   Relation rel,
							   HeapTuple tuple,
							   TupleDesc tupleDesc) {
	Bitmapset *pkey = GetFullYBTablePrimaryKey(rel);
	AttrNumber minattr = YBSystemFirstLowInvalidAttributeNumber + 1;

	int col = -1;
	while ((col = bms_next_member(pkey, col)) >= 0) {
		AttrNumber attnum = col + minattr;

		uint64_t datum = 0;
		bool is_null = false;
		const YBCPgTypeEntity *type_entity = NULL;

		/*
		 * Don't need to fill in for the DocDB RowId column, however we still
		 * need to add the column to the statement to construct the ybctid.
		 */
		if (attnum != YBRowIdAttributeNumber) {
			Oid	type_id = (attnum > 0) ?
				TupleDescAttr(tupleDesc, attnum - 1)->atttypid : InvalidOid;

			type_entity = YBCDataTypeFromOidMod(attnum, type_id);
			datum = heap_getattr(tuple, attnum, tupleDesc, &is_null);
		}

		HandleYBStmtStatus(YBCPgDmlAddYBTupleIdColumn(pg_stmt, attnum, datum, is_null, type_entity),
						   pg_stmt);
	}

	uint64_t tuple_id = 0;
	HandleYBStmtStatus(YBCPgDmlGetYBTupleId(pg_stmt, &tuple_id), pg_stmt);
	return (Datum)tuple_id;
}

/*
 * Bind ybctid to the statement.
 */
static void YBCBindTupleId(YBCPgStatement pg_stmt, Datum tuple_id) {
	YBCPgExpr ybc_expr = YBCNewConstant(pg_stmt, BYTEAOID, tuple_id,
										false /* is_null */);
	HandleYBStmtStatus(YBCPgDmlBindColumn(pg_stmt, YBTupleIdAttributeNumber, ybc_expr),
					   pg_stmt);
}

/*
 * Check if operation changes a system table, ignore changes during
 * initialization (bootstrap mode).
 */
static bool IsSystemCatalogChange(Relation rel)
{
	return IsSystemRelation(rel) && !IsBootstrapProcessingMode();
}

/*
 * Utility method to execute a prepared write statement.
 * Will handle the case if the write changes the system catalogs meaning
 * we need to increment the catalog versions accordingly.
 */
static YBCStatus YBCExecWriteStmt(YBCPgStatement ybc_stmt, Relation rel)
{
	bool is_syscatalog_change = IsSystemCatalogChange(rel);
	bool modifies_row = false;
	HandleYBStmtStatus(YBCPgDmlModifiesRow(ybc_stmt, &modifies_row), ybc_stmt);

	/*
	 * If this write may invalidate catalog cache tuples (i.e. UPDATE or DELETE),
	 * or this write may insert into a cached list, we must increment the
	 * cache version so other sessions can invalidate their caches.
	 * NOTE: If this relation caches lists, an INSERT could effectively be
	 * UPDATINGing the list object.
	 */
	bool is_syscatalog_version_change = is_syscatalog_change
			&& (modifies_row || RelationHasCachedLists(rel));

	/* Let the master know if this should increment the catalog version. */
	if (is_syscatalog_version_change)
	{
		HandleYBStmtStatus(YBCPgSetIsSysCatalogVersionChange(ybc_stmt), ybc_stmt);
	}

	HandleYBStmtStatus(YBCPgSetCatalogCacheVersion(ybc_stmt,
		                                           yb_catalog_cache_version),
		               ybc_stmt);

	/* Execute the insert. */
	YBCStatus status = YBCPgDmlExecWriteOp(ybc_stmt);

	/*
	 * Optimization to increment the catalog version for the local cache as
	 * this backend is already aware of this change and should update its
	 * catalog caches accordingly (without needing to ask the master).
	 * Note that, since the master catalog version should have been identically
	 * incremented, it will continue to match with the local cache version if
	 * and only if no other master changes occurred in the meantime (i.e. from
	 * other backends).
	 * If changes occurred, then a cache refresh will be needed as usual.
	 */
	if (!status && is_syscatalog_version_change)
	{
		// TODO(shane) also update the shared memory catalog version here.
		yb_catalog_cache_version += 1;
	}

	return status;
}

/*
 * Utility method to handle the status of an insert statement to return unique
 * constraint violation error message due to duplicate key in primary key or
 * unique index / constraint.
 */
static void YBCHandleInsertStatus(YBCStatus status, Relation rel, YBCPgStatement stmt)
{
	if (!status)
		return;

	HandleYBStatus(YBCPgDeleteStatement(stmt));

	if (YBCStatusIsAlreadyPresent(status))
	{
		char *constraint;

		/*
		 * If this is the base table and there is a primary key, the primary key is
		 * the constraint. Otherwise, the rel is the unique index constraint.
		 */
		if (!rel->rd_index && rel->rd_pkindex != InvalidOid)
		{
			Relation pkey = RelationIdGetRelation(rel->rd_pkindex);

			constraint = pstrdup(RelationGetRelationName(pkey));

			RelationClose(pkey);
		}
		else
		{
			constraint = pstrdup(RelationGetRelationName(rel));
		}

		YBCFreeStatus(status);
		ereport(ERROR,
				(errcode(ERRCODE_UNIQUE_VIOLATION),
				 errmsg("duplicate key value violates unique constraint \"%s\"",
						constraint)));
	}
	else
	{
		HandleYBStatus(status);
	}
}

/*
 * Utility method to insert a tuple into the relation's backing YugaByte table.
 */
static Oid YBCExecuteInsertInternal(Relation rel,
                                    TupleDesc tupleDesc,
                                    HeapTuple tuple,
                                    bool is_single_row_txn)
{
	Oid            dboid    = YBCGetDatabaseOid(rel);
	Oid            relid    = RelationGetRelid(rel);
	AttrNumber     minattr  = FirstLowInvalidHeapAttributeNumber + 1;
	int            natts    = RelationGetNumberOfAttributes(rel);
	Bitmapset      *pkey    = GetYBTablePrimaryKey(rel);
	YBCPgStatement insert_stmt = NULL;
	bool           is_null  = false;

	/* Generate a new oid for this row if needed */
	if (rel->rd_rel->relhasoids)
	{
		if (!OidIsValid(HeapTupleGetOid(tuple)))
			HeapTupleSetOid(tuple, GetNewOid(rel));
	}

	/* Create the INSERT request and add the values from the tuple. */
	HandleYBStatus(YBCPgNewInsert(ybc_pg_session,
	                              dboid,
	                              relid,
	                              is_single_row_txn,
	                              &insert_stmt));

	/* Get the ybctid for the tuple and bind to statement */
	tuple->t_ybctid = YBCGetYBTupleIdFromTuple(insert_stmt, rel, tuple, tupleDesc);
	YBCBindTupleId(insert_stmt, tuple->t_ybctid);

	for (AttrNumber attnum = minattr; attnum <= natts; attnum++)
	{
		/* Skip virtual (system) and dropped columns */
		if (!IsRealYBColumn(rel, attnum))
		{
			continue;
		}

		Oid   type_id = GetTypeId(attnum, tupleDesc);
		Datum datum   = heap_getattr(tuple, attnum, tupleDesc, &is_null);

		/* Check not-null constraint on primary key early */
		if (is_null && bms_is_member(attnum - minattr, pkey))
		{
			HandleYBStatus(YBCPgDeleteStatement(insert_stmt));
			ereport(ERROR,
			        (errcode(ERRCODE_NOT_NULL_VIOLATION), errmsg(
					        "Missing/null value for primary key column")));
		}

		/* Add the column value to the insert request */
		YBCPgExpr ybc_expr = YBCNewConstant(insert_stmt, type_id, datum, is_null);
		HandleYBStmtStatus(YBCPgDmlBindColumn(insert_stmt, attnum, ybc_expr),
		                   insert_stmt);
	}

	/*
	 * For system tables, mark tuple for invalidation from system caches 
	 * at next command boundary. Do this now so if there is an error with insert 
	 * we will re-query to get the correct state from the master.
	 */
	if (IsCatalogRelation(rel))
	{
		MarkCurrentCommandUsed();
		CacheInvalidateHeapTuple(rel, tuple, NULL);
	}

	/* Execute the insert */
	YBCHandleInsertStatus(YBCExecWriteStmt(insert_stmt, rel), rel, insert_stmt);

	/* Clean up */
	HandleYBStatus(YBCPgDeleteStatement(insert_stmt));
	insert_stmt = NULL;

	return HeapTupleGetOid(tuple);
}

/*
 * Utility method to bind const to column
 */
static void BindColumn(YBCPgStatement stmt, int attr_num, Oid type_id, Datum datum, bool is_null)
{
  YBCPgExpr expr = YBCNewConstant(stmt, type_id, datum, is_null);
  HandleYBStmtStatus(YBCPgDmlBindColumn(stmt, attr_num, expr), stmt);
}

/*
 * Utility method to set keys and value to index write statement
 */
static void PrepareIndexWriteStmt(YBCPgStatement stmt,
                                  Relation index,
                                  Datum *values,
                                  bool *isnull,
                                  int natts,
                                  Datum ybbasectid,
                                  bool ybctid_as_value)
{
	TupleDesc tupdesc = RelationGetDescr(index);

	if (ybbasectid == 0)
	{
		ereport(ERROR,
		(errcode(ERRCODE_INTERNAL_ERROR), errmsg(
			"Missing base table ybctid in index write request")));
	}

	bool has_null_attr = false;
	for (AttrNumber attnum = 1; attnum <= natts; ++attnum)
	{
		Oid   type_id = GetTypeId(attnum, tupdesc);
		Datum value   = values[attnum - 1];
		bool  is_null = isnull[attnum - 1];
		has_null_attr = has_null_attr || is_null;
		BindColumn(stmt, attnum, type_id, value, is_null);
	}

	const bool unique_index = index->rd_index->indisunique;

	/*
	 * For unique indexes we need to set the key suffix system column:
	 * - to ybbasectid if at least one index key column is null.
	 * - to NULL otherwise (setting is_null to true is enough).
	 */
	if (unique_index)
		BindColumn(stmt,
		           YBUniqueIdxKeySuffixAttributeNumber,
		           BYTEAOID,
		           ybbasectid,
		           !has_null_attr /* is_null */);

	/*
	 * We may need to set the base ctid column:
	 * - for unique indexes only if we need it as a value (i.e. for inserts)
	 * - for non-unique indexes always (it is a key column).
	 */
	if (ybctid_as_value || !unique_index)
		BindColumn(stmt,
		           YBIdxBaseTupleIdAttributeNumber,
		           BYTEAOID,
		           ybbasectid,
		           false /* is_null */);
}

Oid YBCExecuteInsert(Relation rel,
                     TupleDesc tupleDesc,
                     HeapTuple tuple)
{
	return YBCExecuteInsertInternal(rel,
	                                tupleDesc,
	                                tuple,
	                                false /* is_single_row_txn */);
}

Oid YBCExecuteSingleRowTxnInsert(Relation rel,
                                 TupleDesc tupleDesc,
                                 HeapTuple tuple)
{
	return YBCExecuteInsertInternal(rel,
	                                tupleDesc,
	                                tuple,
	                                true /* is_single_row_txn */);
}

Oid YBCHeapInsert(TupleTableSlot *slot,
									HeapTuple tuple,
									EState *estate) {
	/*
	 * get information on the (current) result relation
	 */
	ResultRelInfo *resultRelInfo = estate->es_result_relation_info;
	Relation resultRelationDesc = resultRelInfo->ri_RelationDesc;
	bool has_triggers = resultRelInfo->ri_TrigDesc && resultRelInfo->ri_TrigDesc->numtriggers > 0;
	bool has_indices = YBCRelInfoHasSecondaryIndices(resultRelInfo);
	bool is_single_row_txn = estate->es_yb_is_single_row_modify_txn && !has_indices && !has_triggers;

	if (is_single_row_txn)
	{
		/*
		 * Try to execute the statement as a single row transaction (rather
		 * than a distributed transaction) if it is safe to do so.
		 * I.e. if we are in a single-statement transaction that targets a
		 * single row (i.e. single-row-modify txn), and there are no indices
		 * or triggers on the target table.
		 */
		return YBCExecuteSingleRowTxnInsert(resultRelationDesc, slot->tts_tupleDescriptor, tuple);
	}
	else
	{
		return YBCExecuteInsert(resultRelationDesc, slot->tts_tupleDescriptor, tuple);
	}
}

void YBCExecuteInsertIndex(Relation index, Datum *values, bool *isnull, Datum ybctid)
{
	Assert(index->rd_rel->relkind == RELKIND_INDEX);
	Assert(ybctid != 0);

	Oid            dboid    = YBCGetDatabaseOid(index);
	Oid            relid    = RelationGetRelid(index);
	YBCPgStatement insert_stmt = NULL;

	/* Create the INSERT request and add the values from the tuple. */
	HandleYBStatus(YBCPgNewInsert(ybc_pg_session,
	                              dboid,
	                              relid,
	                              false /* is_single_row_txn */,
	                              &insert_stmt));

	PrepareIndexWriteStmt(insert_stmt, index, values, isnull,
						  RelationGetNumberOfAttributes(index),
						  ybctid, true /* ybctid_as_value */);

	/* Execute the insert and clean up. */
	YBCHandleInsertStatus(YBCExecWriteStmt(insert_stmt, index), index, insert_stmt);
	HandleYBStatus(YBCPgDeleteStatement(insert_stmt));
}

void YBCExecuteDelete(Relation rel, TupleTableSlot *slot)
{
	Oid            dboid       = YBCGetDatabaseOid(rel);
	Oid            relid       = RelationGetRelid(rel);
	YBCPgStatement delete_stmt = NULL;

	/* Find ybctid value. Raise error if ybctid is not found. */
	Datum  ybctid = YBCGetYBTupleIdFromSlot(slot);
	if (ybctid == 0)
	{
		ereport(ERROR,
		        (errcode(ERRCODE_UNDEFINED_COLUMN), errmsg(
				        "Missing column ybctid in DELETE request to YugaByte database")));
	}

	/* Execute DELETE. */
	HandleYBStatus(YBCPgNewDelete(ybc_pg_session, dboid, relid, &delete_stmt));

	/* Bind ybctid to identify the current row. */
	YBCPgExpr ybctid_expr = YBCNewConstant(delete_stmt, BYTEAOID, ybctid,
										   false /* is_null */);
	HandleYBStmtStatus(YBCPgDmlBindColumn(delete_stmt,
										  YBTupleIdAttributeNumber,
										  ybctid_expr), delete_stmt);
	HandleYBStmtStatus(YBCExecWriteStmt(delete_stmt, rel), delete_stmt);

	/* Complete execution */
	HandleYBStatus(YBCPgDeleteStatement(delete_stmt));
	delete_stmt = NULL;
}

void YBCExecuteDeleteIndex(Relation index, Datum *values, bool *isnull, Datum ybctid)
{
  Assert(index->rd_rel->relkind == RELKIND_INDEX);

	Oid            dboid    = YBCGetDatabaseOid(index);
	Oid            relid    = RelationGetRelid(index);
	YBCPgStatement delete_stmt = NULL;

	/* Create the DELETE request and add the values from the tuple. */
	HandleYBStatus(YBCPgNewDelete(ybc_pg_session, dboid, relid, &delete_stmt));

	PrepareIndexWriteStmt(delete_stmt, index, values, isnull,
	                      IndexRelationGetNumberOfKeyAttributes(index),
	                      ybctid, false /* ybctid_as_value */);
	HandleYBStmtStatus(YBCExecWriteStmt(delete_stmt, index), delete_stmt);

	HandleYBStatus(YBCPgDeleteStatement(delete_stmt));
}

void YBCExecuteUpdate(Relation rel, TupleTableSlot *slot, HeapTuple tuple)
{
	TupleDesc      tupleDesc   = slot->tts_tupleDescriptor;
	Oid            dboid       = YBCGetDatabaseOid(rel);
	Oid            relid       = RelationGetRelid(rel);
	YBCPgStatement update_stmt = NULL;

	/* Look for ybctid. Raise error if ybctid is not found. */
	Datum ybctid = YBCGetYBTupleIdFromSlot(slot);
	if (ybctid == 0)
	{
		ereport(ERROR,
		        (errcode(ERRCODE_UNDEFINED_COLUMN), errmsg(
				        "Missing column ybctid in UPDATE request to YugaByte database")));
	}

	/* Create update statement. */
	HandleYBStatus(YBCPgNewUpdate(ybc_pg_session, dboid, relid, &update_stmt));

	/* Bind ybctid to identify the current row. */
	YBCPgExpr ybctid_expr = YBCNewConstant(update_stmt, BYTEAOID, ybctid,
										   false /* is_null */);
	HandleYBStmtStatus(YBCPgDmlBindColumn(update_stmt,
	                                      YBTupleIdAttributeNumber,
	                                      ybctid_expr), update_stmt);

	/* Assign new values to columns for updating the current row. */
	tupleDesc = RelationGetDescr(rel);
	for (int idx = 0; idx < tupleDesc->natts; idx++)
	{
		AttrNumber attnum = TupleDescAttr(tupleDesc, idx)->attnum;

		bool is_null = false;
		Datum d = heap_getattr(tuple, attnum, tupleDesc, &is_null);
		YBCPgExpr ybc_expr = YBCNewConstant(update_stmt, TupleDescAttr(tupleDesc, idx)->atttypid,
		                                    d, is_null);
		HandleYBStmtStatus(YBCPgDmlAssignColumn(update_stmt, attnum, ybc_expr), update_stmt);
	}

	/* Execute the statement and clean up */
	HandleYBStmtStatus(YBCExecWriteStmt(update_stmt, rel), update_stmt);
	HandleYBStatus(YBCPgDeleteStatement(update_stmt));
	update_stmt = NULL;

	/*
	 * If the relation has indexes, save the ybctid to insert the updated row into the indexes.
	 */
	if (YBCRelHasSecondaryIndices(rel))
	{
		tuple->t_ybctid = ybctid;
	}
}

void YBCDeleteSysCatalogTuple(Relation rel, HeapTuple tuple)
{
	Oid            dboid       = YBCGetDatabaseOid(rel);
	Oid            relid       = RelationGetRelid(rel);
	YBCPgStatement delete_stmt = NULL;

	if (tuple->t_ybctid == 0)
		ereport(ERROR,
		        (errcode(ERRCODE_UNDEFINED_COLUMN), errmsg(
				        "Missing column ybctid in DELETE request to YugaByte database")));

	/* Prepare DELETE statement. */
	HandleYBStatus(YBCPgNewDelete(ybc_pg_session, dboid, relid, &delete_stmt));

	/* Bind ybctid to identify the current row. */
	YBCPgExpr ybctid_expr = YBCNewConstant(delete_stmt, BYTEAOID, tuple->t_ybctid,
										   false /* is_null */);
	HandleYBStmtStatus(YBCPgDmlBindColumn(delete_stmt,
										  YBTupleIdAttributeNumber,
										  ybctid_expr), delete_stmt);

	/*
	 * Mark tuple for invalidation from system caches at next command
	 * boundary. Do this now so if there is an error with delete we will
	 * re-query to get the correct state from the master.
	 */
	MarkCurrentCommandUsed();
	CacheInvalidateHeapTuple(rel, tuple, NULL);

	HandleYBStmtStatus(YBCExecWriteStmt(delete_stmt, rel), delete_stmt);

	/* Complete execution */
	HandleYBStatus(YBCPgDeleteStatement(delete_stmt));
	delete_stmt = NULL;
}

void YBCUpdateSysCatalogTuple(Relation rel, HeapTuple oldtuple, HeapTuple tuple)
{
	Oid            dboid       = YBCGetDatabaseOid(rel);
	Oid            relid       = RelationGetRelid(rel);
	TupleDesc      tupleDesc   = RelationGetDescr(rel);
	int            natts       = RelationGetNumberOfAttributes(rel);
	YBCPgStatement update_stmt = NULL;

	/* Create update statement. */
	HandleYBStatus(YBCPgNewUpdate(ybc_pg_session, dboid, relid, &update_stmt));

	AttrNumber minattr = FirstLowInvalidHeapAttributeNumber + 1;
	Bitmapset  *pkey   = GetYBTablePrimaryKey(rel);

	/* Bind the ybctid to the statement. */
	tuple->t_ybctid = YBCGetYBTupleIdFromTuple(update_stmt, rel, tuple, tupleDesc);
	YBCBindTupleId(update_stmt, tuple->t_ybctid);

	/* Assign new values to columns for updating the current row. */
	for (int idx = 0; idx < natts; idx++)
	{
		AttrNumber attnum = TupleDescAttr(tupleDesc, idx)->attnum;

		/* Skip primary-key columns */
		if (bms_is_member(attnum - minattr, pkey))
		{
			continue;
		}

		bool is_null = false;
		Datum d = heap_getattr(tuple, attnum, tupleDesc, &is_null);
		YBCPgExpr ybc_expr = YBCNewConstant(update_stmt, TupleDescAttr(tupleDesc, idx)->atttypid,
											d, is_null);
		HandleYBStmtStatus(YBCPgDmlAssignColumn(update_stmt, attnum, ybc_expr), update_stmt);
	}

	/*
	 * Mark old tuple for invalidation from system caches at next command
	 * boundary, and mark the new tuple for invalidation in case we abort.
	 * In case when there is no old tuple, we will invalidate with the
	 * new tuple at next command boundary instead. Do these now so if there
	 * is an error with update we will re-query to get the correct state
	 * from the master.
	 */
	MarkCurrentCommandUsed();
	if (oldtuple)
		CacheInvalidateHeapTuple(rel, oldtuple, tuple);
	else
		CacheInvalidateHeapTuple(rel, tuple, NULL);

	/* Execute the statement and clean up */
	HandleYBStmtStatus(YBCExecWriteStmt(update_stmt, rel), update_stmt);
	HandleYBStatus(YBCPgDeleteStatement(update_stmt));
	update_stmt = NULL;
}

void YBCStartBufferingWriteOperations()
{
	HandleYBStatus(YBCPgStartBufferingWriteOperations(ybc_pg_session));
}

void YBCFlushBufferedWriteOperations()
{
	HandleYBStatus(YBCPgFlushBufferedWriteOperations(ybc_pg_session));
}

bool
YBCRelInfoHasSecondaryIndices(ResultRelInfo *resultRelInfo)
{
	return resultRelInfo->ri_NumIndices > 1 ||
			(resultRelInfo->ri_NumIndices == 1 &&
			 !resultRelInfo->ri_IndexRelationDescs[0]->rd_index->indisprimary);
}

bool
YBCRelHasSecondaryIndices(Relation relation)
{
	if (!relation->rd_rel->relhasindex)
		return false;

	bool	 has_indices = false;
	List	 *indexlist = RelationGetIndexList(relation);
	ListCell *lc;

	foreach(lc, indexlist)
	{
		if (lfirst_oid(lc) == relation->rd_pkindex)
			continue;
		has_indices = true;
		break;
	}

	list_free(indexlist);

	return has_indices;
}
