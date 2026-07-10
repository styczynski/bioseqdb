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

extern "C" {
#include "storage/lock.h"
}

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>

export module bioseq.table:access_hooks;

import bioseq.common.mlogging;
import bioseq.pg_utils;
import bioseq.storage.metadata;
import bioseq.storage.manager;
import bioseq.column.tam;
import :access_identity;

namespace {

static object_access_hook_type PrevObjectAccessHook = NULL;
static ProcessUtility_hook_type PrevProcessUtilityHook = NULL;
static bool BioseqExtensionTeardownActive = false;
/*
 * Backend-local, read-only test observability.  These counters are deliberately
 * downstream of the classification decisions: neither value is ever consulted
 * to decide whether teardown is active.  They survive DROP/CREATE EXTENSION in
 * one backend because PostgreSQL does not unload an already-loaded library.
 */
static std::uint64_t BioseqExtensionDropLockFallbackClassifications = 0;
static std::uint64_t BioseqExtensionMissingRelationObservations = 0;

/* SnapshotSelf, rather than relcache/syscache state, is the authoritative
 * answer for PostgreSQL's precomputed dependency list: a recursive cleanup can
 * delete pg_class while a stale relcache entry remains pinned in this backend. */
[[nodiscard]] static bool
RelationCatalogRowExists(const Oid relid)
{
	Relation class_relation = table_open(RelationRelationId, AccessShareLock);
	ScanKeyData key;
	ScanKeyInit(&key, Anum_pg_class_oid, BTEqualStrategyNumber, F_OIDEQ,
		ObjectIdGetDatum(relid));
	SysScanDesc scan = systable_beginscan(
		class_relation, ClassOidIndexId, true, SnapshotSelf, 1, &key);
	const bool exists = HeapTupleIsValid(systable_getnext(scan));
	systable_endscan(scan);
	table_close(class_relation, AccessShareLock);
	return exists;
}

/*
 * A backend can load bioseq.dylib lazily while PostgreSQL is already inside
 * standard_ProcessUtility for DROP EXTENSION bioseq.  In that case installing
 * our ProcessUtility hook cannot retroactively wrap the active command, but
 * RemoveObjects() has already taken and retained an AccessExclusive database-
 * object lock on the exact pg_extension OID.  Reconstruct the same LOCKTAG as
 * LockDatabaseObject() so later OAT_DROP callbacks can prove this one narrow
 * teardown context without treating an ordinary missing catalog as benign.
 */
[[nodiscard]] static bool
BioseqExtensionDropLockHeldByMe()
{
	const Oid extension_oid = get_extension_oid("bioseq", true);
	if (!OidIsValid(extension_oid))
	{
		return false;
	}

	LOCKTAG extension_lock;
	SET_LOCKTAG_OBJECT(extension_lock, MyDatabaseId,
					   ExtensionRelationId, extension_oid, 0);
	return LockHeldByMe(&extension_lock, AccessExclusiveLock,
					/* orstronger = */ false);
}

[[nodiscard]] static bool
BioseqExtensionTeardownContext()
{
	/* Preserve the direct-String ProcessUtility fast path for backends that
	 * loaded bioseq before entering DROP EXTENSION. */
	if (BioseqExtensionTeardownActive)
	{
		return true;
	}
	if (BioseqExtensionDropLockHeldByMe())
	{
		++BioseqExtensionDropLockFallbackClassifications;
		return true;
	}
	return false;
}

[[nodiscard]] static bool
DropsBioseqExtension(Node *parsetree)
{
	if (!IsA(parsetree, DropStmt))
	{
		return false;
	}
	DropStmt *drop = reinterpret_cast<DropStmt *>(parsetree);
	if (drop->removeType != OBJECT_EXTENSION)
	{
		return false;
	}
	ListCell *cell = nullptr;
	foreach(cell, drop->objects)
	{
		/*
		 * PostgreSQL represents each DROP EXTENSION target directly as a String
		 * (unlike object classes whose targets are qualified-name lists).  Match
		 * only the exact extension name so this teardown allowance cannot be
		 * activated by an unrelated DROP statement or a malformed parse tree.
		 */
		Node *object = reinterpret_cast<Node *>(lfirst(cell));
		if (IsA(object, String)
			&& std::strcmp(strVal(object), "bioseq") == 0)
		{
			return true;
		}
	}
	return false;
}

/*
 * Clean-up resources for bioseq column tables.
 */
static void
BioseqColumnTableDropHook(Oid relid)
{
	bioseq_log<DBG>("[bioseq] pg: BioseqColumnTableDropHook() entered");
	const bool extension_teardown = BioseqExtensionTeardownContext();

	/*
	 * DROP EXTENSION builds one dependency-deletion list before invoking the
	 * per-object hooks.  Dropping a parent bioseq table below also removes its
	 * generated sidecars (and therefore their implicit primary-key indexes).
	 * Extension teardown now leaves those children to PostgreSQL's one owning
	 * CASCADE list. Any missing entry is therefore an invariant violation, not
	 * an idempotent condition to hide from core deletion.
	 *
	 * Keep the returned relation reference only through AM classification and
	 * identity capture.  Its AccessShareLock is retained to transaction end,
	 * while the relcache pin is released before any catalog operation that can
	 * advance the command counter.
	 */
	if (!RelationCatalogRowExists(relid))
	{
		++BioseqExtensionMissingRelationObservations;
		if (extension_teardown)
		{
			ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("bioseq extension teardown encountered an already-missing relation"),
				 errdetail("relation_oid=%u", relid)));
		}
		ereport(ERROR,
			(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
			 errmsg("bioseq drop hook received a missing relation outside extension teardown"),
			 errdetail("relation_oid=%u", relid)));
	}
	Relation drop_target = try_safe_relation_open(relid, AccessShareLock);
	if (drop_target == nullptr)
	{
		++BioseqExtensionMissingRelationObservations;
		if (extension_teardown)
		{
			ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("bioseq extension teardown lost a relation after catalog validation"),
				 errdetail("relation_oid=%u", relid)));
		}
		ereport(ERROR,
			(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
			 errmsg("bioseq drop hook received a missing relation outside extension teardown"),
			 errdetail("relation_oid=%u", relid)));
	}
	const bool is_bioseq_relation =
		IsBioseqColumnTableAmRelation(drop_target);
	const RelFileLocator relfilelocator = is_bioseq_relation
		? drop_target->rd_locator
		: RelFileLocator{};
	const Oid relation_oid = RelationGetRelid(drop_target);

	/* Release the relcache pin before source invalidation: that path performs a
	 * CommandCounterIncrement(), which may rebuild invalidated relcache entries.
	 * The AccessShareLock acquired by try_relation_open remains held to xact end. */
	relation_close(drop_target, NoLock);
	drop_target = nullptr;

	/* PostgreSQL owns the complete extension dependency-deletion list. During
	 * an attested DROP EXTENSION, source rows and their catalog disappear with
	 * that list, so invalidating them is both unnecessary and unsafe: a newly
	 * installed library must not deform an older on-disk catalog contract merely
	 * to let PostgreSQL remove it. Ordinary relation drops remain fail-closed. */
	if (!extension_teardown
		&& bioseq::storage::internal_catalog_detail::
			bioseq_extension_installed()
		&& bioseq::storage::is_registered_sequence_fragment_relation(
			relid))
	{
		bioseq::storage::SequenceSourceVersionManager::invalidate_for_fragment(
			relid);
	}

	if (is_bioseq_relation)
	{
		DeleteMetadataRows(relfilelocator);
		DeleteBioseqColumnTableOptions(relation_oid, true);
		MarkRelfilenodeDropped(relfilelocator.relNumber, GetCurrentSubTransactionId());

		/* DROP EXTENSION already owns a complete dependency-deletion list for
		 * generated fragments, sidecars, and their indexes.  Recursive DROP TABLE
		 * here would delete those objects early and leave stale OIDs for core to
		 * revisit.  Ordinary parent DROP still needs deterministic child cleanup. */
		if (!extension_teardown)
		{
			bioseq::storage::bioseq_default_storage_manager::drop_sub_tables_for_main(
				relfilelocator.dbOid, relfilelocator.spcOid,
				relfilelocator.relNumber);
		}
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

	/*
	 * Recursive utility commands issued by the parent drop hook re-enter this
	 * hook. Save/restore the prior value rather than unconditionally clearing a
	 * flag, and restore it on PostgreSQL ERROR longjmp as well as success.
	 */
	const bool saved_teardown_context = BioseqExtensionTeardownActive;
	if (DropsBioseqExtension(parsetree))
	{
		BioseqExtensionTeardownActive = true;
	}
	PG_TRY();
	{
		PrevProcessUtilityHook_compat(pstmt, queryString, false, context,
								  params, queryEnv, dest, completionTag);
		BioseqExtensionTeardownActive = saved_teardown_context;
	}
	PG_CATCH();
	{
		BioseqExtensionTeardownActive = saved_teardown_context;
		PG_RE_THROW();
	}
	PG_END_TRY();
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

/*
 * Integration-only proof that a fresh backend used the lazy-load lock
 * fallback without manufacturing stale relation/OID revisits. This function
 * is diagnostic only: it snapshots the two counters and cannot reset or
 * influence teardown state.
 */
POSTGRES_FN(bioseq_internal_teardown_diagnostics)
{
	TupleDesc tupdesc = nullptr;
	if (get_call_result_type(fcinfo, nullptr, &tupdesc) != TYPEFUNC_COMPOSITE)
	{
		ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("bioseq_internal.teardown_diagnostics() must be called as a composite-returning function")));
	}
	tupdesc = BlessTupleDesc(tupdesc);

	Datum values[2]{};
	bool nulls[2]{};
	values[0] = Int64GetDatum(static_cast<std::int64_t>(
		std::min<std::uint64_t>(
			BioseqExtensionDropLockFallbackClassifications,
			static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))));
	values[1] = Int64GetDatum(static_cast<std::int64_t>(
		std::min<std::uint64_t>(
			BioseqExtensionMissingRelationObservations,
			static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))));

	HeapTuple tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}
