/*-------------------------------------------------------------------------
 *
 * table/access_hooks.cppm
 *
 * PostgreSQL hook glue for the bioseq table access method. This partition is
 * intentionally separate from :access_method so routine assembly is not mixed
 * with DDL/drop/utility callback wiring.
 *
 *-------------------------------------------------------------------------
 */
module;
#include "common.h"

export module bioseq_table:access_hooks;

import mlogging;
import storage_metadata;
import bioseq_storage;
import bioseq_column_tam;
import :access_identity;

namespace {

static object_access_hook_type PrevObjectAccessHook = NULL;
static ProcessUtility_hook_type PrevProcessUtilityHook = NULL;

/*
 * Clean-up resources for bioseq column tables.
 */
static void
BioseqColumnTableDropHook(Oid relid)
{
	bioseq_log<DBG>("[bioseq] pg: BioseqColumnTableDropHook() entered");

	LockRelationOid(relid, AccessShareLock);

	if (IsBioseqColumnTableAmTable(relid))
	{
		Relation rel = table_open(relid, AccessExclusiveLock);
		RelFileLocator relfilelocator = rel->rd_locator;
		DeleteMetadataRows(relfilelocator);
		DeleteBioseqColumnTableOptions(rel->rd_id, true);
		MarkRelfilenodeDropped(relfilelocator.relNumber, GetCurrentSubTransactionId());

		bioseq::storage::bioseq_default_storage_manager::drop_sub_tables_for_main(
			relfilelocator.dbOid, relfilelocator.spcOid,
			relfilelocator.relNumber);

		table_close(rel, NoLock);
	}
}

/*
 * Reject AFTER ... FOR EACH ROW triggers on bioseq column tables.
 */
static void
BioseqColumnTriggerCreateHook(Oid tgid)
{
	bioseq_log<DBG>("[bioseq] pg: BioseqColumnTriggerCreateHook() entered");

	ScanKeyData skey[1];
	Relation tgrel = table_open(TriggerRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_pg_trigger_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(tgid));

	SysScanDesc tgscan = systable_beginscan(tgrel, TriggerOidIndexId, true,
											SnapshotSelf, 1, skey);

	HeapTuple tgtup = systable_getnext(tgscan);

	if (!HeapTupleIsValid(tgtup))
	{
		table_close(tgrel, AccessShareLock);
		return;
	}

	Form_pg_trigger tgrec = reinterpret_cast<Form_pg_trigger>(GETSTRUCT(tgtup));

	Oid tgrelid = tgrec->tgrelid;
	int16_t tgtype = tgrec->tgtype;

	systable_endscan(tgscan);
	table_close(tgrel, AccessShareLock);

	if (TRIGGER_FOR_ROW(tgtype) && TRIGGER_FOR_AFTER(tgtype) &&
		IsBioseqColumnTableAmTable(tgrelid))
	{
		bioseq_report<ERR, PG>().code(ERRCODE_FEATURE_NOT_SUPPORTED)
			.msg("Foreign keys and AFTER ROW triggers are not supported for columnar tables")
			.hint("Consider an AFTER STATEMENT trigger instead.");
	}
}

static void
BioseqColumnTableAMObjectAccessHook(ObjectAccessType access, Oid classId, Oid objectId,
									int subId, void *arg)
{
	if (PrevObjectAccessHook)
	{
		PrevObjectAccessHook(access, classId, objectId, subId, arg);
	}

	if (access == OAT_DROP && classId == RelationRelationId && !OidIsValid(subId))
	{
		BioseqColumnTableDropHook(objectId);
	}
	else if (access == OAT_POST_CREATE && classId == TriggerRelationId)
	{
		BioseqColumnTriggerCreateHook(objectId);
	}
}

static void
BioseqColumnProcessUtility(PlannedStmt *pstmt,
						   const char *queryString,
						   bool readOnlyTree,
						   ProcessUtilityContext context,
						   ParamListInfo params,
						   QueryEnvironment *queryEnv,
						   DestReceiver *dest,
						   QueryCompletionCompat *completionTag)
{
	if (readOnlyTree)
	{
		pstmt = copyObject(pstmt);
	}

	Node *parsetree = pstmt->utilityStmt;

	if (IsA(parsetree, IndexStmt))
	{
		IndexStmt *indexStmt = reinterpret_cast<IndexStmt *>(parsetree);

		Relation rel = relation_openrv(indexStmt->relation,
									   indexStmt->concurrent ? ShareUpdateExclusiveLock :
									   ShareLock);

		if (IsBioseqColumnTableAmTable(RelationGetRelid(rel)) &&
			!BioseqColumnSupportsIndexAM(indexStmt->accessMethod))
		{
			bioseq_report<ERR, PG>().code(ERRCODE_FEATURE_NOT_SUPPORTED)
				.msg("unsupported access method for the index on columnar table {} ({})",
					 RelationGetRelationName(rel), indexStmt->accessMethod);
		}

		RelationClose(rel);
	}

	PrevProcessUtilityHook_compat(pstmt, queryString, false, context,
								  params, queryEnv, dest, completionTag);
}

}

export void
InstallBioseqColumnTableHooks()
{
	PrevObjectAccessHook = object_access_hook;
	object_access_hook = BioseqColumnTableAMObjectAccessHook;

	PrevProcessUtilityHook = ProcessUtility_hook ?
							 ProcessUtility_hook :
							 standard_ProcessUtility;
	ProcessUtility_hook = BioseqColumnProcessUtility;
}
