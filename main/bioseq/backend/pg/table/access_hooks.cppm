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
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "catalog/objectaddress.h"
#include "catalog/partition.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_type.h"
#include "commands/copy.h"
#include "storage/lock.h"
#include "utils/acl.h"
#include "utils/inval.h"
#include "utils/syscache.h"
}

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>

export module bioseq.table:access_hooks;

import bioseq.common.mlogging;
import bioseq.pg_utils;
import bioseq.scheduler.supervisor_pg;
import bioseq.storage.metadata;
import bioseq.storage.manager;
import bioseq.column.tam;
import :access_identity;

namespace {

static object_access_hook_type PrevObjectAccessHook = NULL;
static ProcessUtility_hook_type PrevProcessUtilityHook = NULL;
static bool BioseqExtensionTeardownActive = false;
/* True only while the parent OAT_DROP hook issues PostgreSQL-owned DROP TABLE
 * commands for its generated children. Those recursive commands have already
 * passed the parent's read-only preflight and must not be mistaken for a new
 * user-authored DROP statement. */
static bool BioseqInternalDropCleanupActive = false;

/* PostgreSQL's RemoveRelations() resolves a named partition with
 * RangeVarCallbackForDropRelation so its parent is locked before the child.
 * Query execution and partition DDL use that same parent-first order. This
 * table-only callback mirrors the PG17 ordering, permission and relkind checks
 * needed by our earlier read-only preflight; calling callback-free
 * RangeVarGetRelid(..., AccessExclusiveLock) here would introduce a child-first
 * deadlock edge before core gets a chance to run its own callback. */
struct BioseqDropLockState
{
	Oid partition_parent_oid = InvalidOid;
};

static void
RangeVarCallbackForBioseqDropPreflight(const RangeVar *relation,
										Oid relid, Oid old_relid, void *arg)
{
	auto *state = static_cast<BioseqDropLockState *>(arg);
	if (relid != old_relid && OidIsValid(state->partition_parent_oid))
	{
		UnlockRelationOid(state->partition_parent_oid, AccessExclusiveLock);
		state->partition_parent_oid = InvalidOid;
	}
	if (!OidIsValid(relid))
	{
		return;
	}

	HeapTuple tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
	{
		return;
	}
	Form_pg_class class_form =
		reinterpret_cast<Form_pg_class>(GETSTRUCT(tuple));
	const char relkind = class_form->relkind;
	const bool is_partition = class_form->relispartition;
	const Oid namespace_oid = class_form->relnamespace;

	if (relkind != RELKIND_RELATION
		&& relkind != RELKIND_PARTITIONED_TABLE)
	{
		ereport(ERROR,
			(errcode(ERRCODE_WRONG_OBJECT_TYPE),
			 errmsg("\"%s\" is not a table", relation->relname),
			 errhint("Use the matching DROP command for this relation type.")));
	}
	if (!object_ownercheck(RelationRelationId, relid, GetUserId())
		&& !object_ownercheck(NamespaceRelationId, namespace_oid, GetUserId()))
	{
		aclcheck_error(ACLCHECK_NOT_OWNER,
			get_relkind_objtype(relkind), relation->relname);
	}
	if (!allowSystemTableMods && IsSystemClass(relid, class_form))
	{
		ereport(ERROR,
			(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
			 errmsg("permission denied: \"%s\" is a system catalog",
				relation->relname)));
	}
	ReleaseSysCache(tuple);

	if (is_partition && relid != old_relid)
	{
		state->partition_parent_oid = get_partition_parent(
			relid, /* even_if_detached = */ true);
		if (OidIsValid(state->partition_parent_oid))
		{
			LockRelationOid(
				state->partition_parent_oid, AccessExclusiveLock);
		}
	}
}

/* PG17 find_all_inheritors() intentionally omits a committed
 * DETACH PARTITION CONCURRENTLY member under an active snapshot. DROP's
 * dependency set can still own that pending-detach partition, so the early
 * cleanup preflight must use the extended direct-child API with
 * omit_detached=false. The returned direct children are OID-sorted and locked
 * by PostgreSQL; this breadth-first traversal retains those locks and dedupes
 * multiple-inheritance paths without C++ state that ereport could bypass. */
static void
AppendCompleteDropInheritanceClosure(Oid root_relid, List **target_relids)
{
	List *closure = list_make1_oid(root_relid);
	ListCell *agenda_cell = nullptr;
	foreach(agenda_cell, closure)
	{
		const Oid parent_relid = lfirst_oid(agenda_cell);
		List *children = find_inheritance_children_extended(
			parent_relid,
			/* omit_detached = */ false,
			AccessExclusiveLock,
			nullptr,
			nullptr);
		ListCell *child_cell = nullptr;
		foreach(child_cell, children)
		{
			const Oid child_relid = lfirst_oid(child_cell);
			if (!list_member_oid(closure, child_relid))
			{
				closure = lappend_oid(closure, child_relid);
			}
		}
	}

	foreach(agenda_cell, closure)
	{
		const Oid member = lfirst_oid(agenda_cell);
		if (!list_member_oid(*target_relids, member))
		{
			*target_relids = lappend_oid(*target_relids, member);
		}
	}
}
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
 * OAT_DROP is suitable for last-moment cleanup and race revalidation, but a
 * relation DROP has already entered PostgreSQL/table-AM deletion machinery by
 * then.  A catalog-contract ERROR at that point can leave backend-local parent
 * storage state deformed even though SQL catalog changes roll back.
 *
 * Resolve and lock every named target first, preserving PostgreSQL's
 * missing_ok behaviour. A named table is not the complete deletion set:
 * PostgreSQL can implicitly process inheritance children and partitions. Use
 * its parent-first RangeVar callback contract for named partitions, then walk
 * direct children with omit_detached=false under AccessExclusiveLock and
 * deduplicate the resulting closure before classification. Only after the
 * complete locked closure is known do we read-only validate every bioseq
 * member's two cleanup catalogs.
 *
 * This early guarantee is a shared-preload contract. If the dylib is first
 * loaded while standard_ProcessUtility is already executing a top-level DROP
 * TABLE, no hook installed by that load can retroactively preflight the
 * command. When this hook is already present in a non-preloaded backend, reject
 * the bioseq DROP before core mutation instead of pretending the impossible
 * first-command boundary is covered. standard_ProcessUtility is not entered
 * unless the entire closure preflight succeeds.
 */
static void
PreflightBioseqTableDrop(Node *parsetree)
{
	if (!IsA(parsetree, DropStmt))
	{
		return;
	}
	DropStmt *drop = reinterpret_cast<DropStmt *>(parsetree);
	if (drop->removeType != OBJECT_TABLE)
	{
		return;
	}

	List *named_relids = NIL;
	ListCell *cell = nullptr;
	foreach(cell, drop->objects)
	{
		/* For OBJECT_TABLE each object is PostgreSQL's qualified-name List. */
		List *qualified_name = reinterpret_cast<List *>(lfirst(cell));
		RangeVar *relation = makeRangeVarFromNameList(qualified_name);
		/* Match RemoveRelations(): an already-held relation lock can otherwise
		 * let RangeVar lookup skip the invalidation flush and leave a stale
		 * relhassubclass=false in front of the closure scan. */
		AcceptInvalidationMessages();
		BioseqDropLockState lock_state{};
		const Oid relid = RangeVarGetRelidExtended(
			relation,
			AccessExclusiveLock,
			drop->missing_ok ? RVR_MISSING_OK : 0,
			RangeVarCallbackForBioseqDropPreflight,
			&lock_state);
		if (OidIsValid(relid) && !list_member_oid(named_relids, relid))
		{
			named_relids = lappend_oid(named_relids, relid);
		}
	}

	/* Resolve and lock every explicit root before taking any implicit child
	 * lock, so a child shared by multiple named inheritance roots cannot invert
	 * the caller's own named-target lock order. Process any invalidations that
	 * arrived while a named lock was awaited before consulting relhassubclass. */
	AcceptInvalidationMessages();
	List *target_relids = NIL;
	foreach(cell, named_relids)
	{
		AppendCompleteDropInheritanceClosure(
			lfirst_oid(cell), &target_relids);
	}

	List *bioseq_relids = NIL;
	foreach(cell, target_relids)
	{
		const Oid relid = lfirst_oid(cell);
		if (IsBioseqColumnTableAmTable(relid))
		{
			bioseq_relids = lappend_oid(bioseq_relids, relid);
		}
	}

	if (bioseq_relids != NIL
		&& !bioseq::parallelism::pg::supervisor_available())
	{
		ereport(ERROR,
			(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
			 errmsg("bioseq table DROP requires shared preload"),
			 errdetail("the ProcessUtility hook is active, but the bioseq shared-preload state is unavailable"),
			 errhint("Add bioseq to shared_preload_libraries, restart PostgreSQL, and retry the DROP.")));
	}

	foreach(cell, bioseq_relids)
	{
		/* The catalog contract is global today, but validate once per locked
		 * bioseq closure member so a future relation-scoped contract cannot be
		 * accidentally reduced to validation of only the named parent. */
		(void)lfirst_oid(cell);
		bioseq::storage::bioseq_default_storage_manager::
			validate_drop_catalog_contracts();
	}
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
		/* Ordinary parent DROP must validate both the fragment registry and
		 * source-version catalog before changing parent metadata or the
		 * backend-local dropped-relfilenode state. drop_sub_tables_for_main()
		 * performs those fail-closed validations before its own mutations.
		 * Running it first therefore leaves the visible parent untouched when
		 * either catalog is missing or malformed. Extension teardown continues
		 * to leave the complete dependency list to PostgreSQL. */
		if (!extension_teardown)
		{
			const bool saved_internal_cleanup = BioseqInternalDropCleanupActive;
			std::exception_ptr cpp_error;
			BioseqInternalDropCleanupActive = true;
			PG_TRY();
			{
				try
				{
					bioseq::storage::bioseq_default_storage_manager::drop_sub_tables_for_main(
						relfilelocator.dbOid, relfilelocator.spcOid,
						relfilelocator.relNumber);
				}
				catch (...)
				{
					cpp_error = std::current_exception();
				}
			}
			PG_CATCH();
			{
				BioseqInternalDropCleanupActive = saved_internal_cleanup;
				PG_RE_THROW();
			}
			PG_END_TRY();
			BioseqInternalDropCleanupActive = saved_internal_cleanup;
			if (cpp_error != nullptr)
			{
				std::rethrow_exception(cpp_error);
			}
		}

		DeleteMetadataRows(relfilelocator);
		DeleteBioseqColumnTableOptions(relation_oid, true);
		MarkRelfilenodeDropped(relfilelocator.relNumber, GetCurrentSubTransactionId());
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
BioseqColumnTableAMObjectAccessHookImpl(ObjectAccessType access, Oid classId, Oid objectId,
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
BioseqColumnTableAMObjectAccessHook(ObjectAccessType access, Oid classId,
									Oid objectId, int subId, void *arg)
{
	const bool saved_extension_teardown = BioseqExtensionTeardownActive;
	const bool saved_internal_cleanup = BioseqInternalDropCleanupActive;
	ErrorData* exact_error = nullptr;
	char detail[512] = "unknown C++ exception";
	try
	{
		BioseqColumnTableAMObjectAccessHookImpl(
			access, classId, objectId, subId, arg);
		return;
	}
	catch (bioseq::pg_utils::PostgresErrorAsCpp& error)
	{
		exact_error = error.release_error_data();
	}
	catch (const std::exception& error)
	{
		strlcpy(detail, error.what() == nullptr
			? "std::exception without detail" : error.what(), sizeof(detail));
	}
	catch (...)
	{
	}
	BioseqExtensionTeardownActive = saved_extension_teardown;
	BioseqInternalDropCleanupActive = saved_internal_cleanup;
	if (exact_error != nullptr)
	{
		ReThrowError(exact_error);
	}
	ereport(ERROR,
		(errcode(ERRCODE_INTERNAL_ERROR),
		 errmsg("bioseq object-access hook raised a C++ exception"),
		 errdetail_internal("%s", detail)));
}

static void
PreflightTypmodlessLetterTextCopy(Node *parsetree)
{
	if (!IsA(parsetree, CopyStmt))
	{
		return;
	}
	auto *copy = reinterpret_cast<CopyStmt *>(parsetree);
	if (!copy->is_from || copy->relation == nullptr)
	{
		return;
	}

	CopyFormatOptions options{};
	ProcessCopyOptions(nullptr, &options, true, copy->options);
	if (options.binary)
	{
		/* Binary letter input carries and validates its physical alphabet. */
		return;
	}

	const Oid namespace_oid = get_namespace_oid("bioseq", true);
	if (!OidIsValid(namespace_oid))
	{
		return;
	}
	const Oid letter_oid = GetSysCacheOid2(
		TYPENAMENSP, Anum_pg_type_oid,
		PointerGetDatum("letter"), ObjectIdGetDatum(namespace_oid));
	if (!OidIsValid(letter_oid))
	{
		return;
	}

	Relation relation = table_openrv(copy->relation, RowExclusiveLock);
	List *attribute_numbers = CopyGetAttnums(
		RelationGetDescr(relation), relation, copy->attlist);
	ListCell *cell = nullptr;
	foreach(cell, attribute_numbers)
	{
		const AttrNumber attribute_number = static_cast<AttrNumber>(
			lfirst_int(cell));
		if (attribute_number <= 0
			|| attribute_number > RelationGetDescr(relation)->natts)
		{
			continue;
		}
		Form_pg_attribute attribute = TupleDescAttr(
			RelationGetDescr(relation), attribute_number - 1);
		if (!attribute->attisdropped
			&& attribute->atttypid == letter_oid
			&& attribute->atttypmod < 0)
		{
			char *attribute_name = pstrdup(NameStr(attribute->attname));
			list_free(attribute_numbers);
			table_close(relation, NoLock);
			ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("bioseq.letter text COPY requires an alphabet typmod"),
				 errdetail("target column \"%s\" has base type bioseq.letter without an alphabet typmod",
					 attribute_name),
				 errhint("declare the target as bioseq.letter('dna4') or another supported alphabet, or use validated binary COPY")));
		}
	}
	list_free(attribute_numbers);
	table_close(relation, NoLock);
}

static void
BioseqColumnProcessUtilityImpl(PlannedStmt *pstmt,
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
	PreflightTypmodlessLetterTextCopy(parsetree);

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

	if (!BioseqInternalDropCleanupActive)
	{
		PreflightBioseqTableDrop(parsetree);
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
	std::exception_ptr cpp_error;
	PG_TRY();
	{
		try
		{
			PrevProcessUtilityHook_compat(pstmt, queryString, readOnlyTree, context,
									  params, queryEnv, dest, completionTag);
		}
		catch (...)
		{
			cpp_error = std::current_exception();
		}
	}
	PG_CATCH();
	{
		BioseqExtensionTeardownActive = saved_teardown_context;
		PG_RE_THROW();
	}
	PG_END_TRY();
	BioseqExtensionTeardownActive = saved_teardown_context;
	if (cpp_error != nullptr)
	{
		std::rethrow_exception(cpp_error);
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
	const bool saved_extension_teardown = BioseqExtensionTeardownActive;
	const bool saved_internal_cleanup = BioseqInternalDropCleanupActive;
	ErrorData* exact_error = nullptr;
	char detail[512] = "unknown C++ exception";
	try
	{
		BioseqColumnProcessUtilityImpl(
			pstmt, queryString, readOnlyTree, context,
			params, queryEnv, dest, completionTag);
		return;
	}
	catch (bioseq::pg_utils::PostgresErrorAsCpp& error)
	{
		exact_error = error.release_error_data();
	}
	catch (const std::exception& error)
	{
		strlcpy(detail, error.what() == nullptr
			? "std::exception without detail" : error.what(), sizeof(detail));
	}
	catch (...)
	{
	}
	BioseqExtensionTeardownActive = saved_extension_teardown;
	BioseqInternalDropCleanupActive = saved_internal_cleanup;
	if (exact_error != nullptr)
	{
		ReThrowError(exact_error);
	}
	ereport(ERROR,
		(errcode(ERRCODE_INTERNAL_ERROR),
		 errmsg("bioseq ProcessUtility hook raised a C++ exception"),
		 errdetail_internal("%s", detail)));
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
