/**
 * @file
 * @brief PostgreSQL object-access and utility hooks for Bioseq tables.
 */
module;
#include <bioseq/pg_compat.h>

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
#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>

export module bioseq.postgres.table:access_hooks;

import bioseq.common.logging;
import bioseq.postgres.support;
import bioseq.postgres.support.sql_boundary;
import bioseq.postgres.scheduler.supervisor;
import bioseq.postgres.storage.metadata;
import bioseq.postgres.storage.manager;
import bioseq.postgres.column.table_access;
import bioseq.postgres.external.relation;
import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.table.sql_rows;
import :access_identity;

namespace {

static object_access_hook_type previous_object_access_hook = nullptr;
static ProcessUtility_hook_type previous_process_utility_hook = nullptr;
static bool extension_teardown_active = false;
/* Set while dropping a Bioseq table's generated child tables. Their recursive
 * DROP commands share the parent's completed checks. */
static bool internal_drop_cleanup_active = false;
/* Remember the table created by the  current external CREATE command so its
 * options can be saved afterward. Nested utility calls save and restore both
 * values. */
static bool external_create_capture_active = false;
static Oid captured_external_create_oid = InvalidOid;

/** Parent relation locked while resolving a DROP target.
 *
 * PostgreSQL locks a partition's parnt before the partition during DROP.
 * Keep that order during our early checks too, or concurrent partition changes
 * can deadlock before PostgreSQL runs its own checks.
 */
struct bioseq_drop_lock_state {
    Oid partition_parent_oid = InvalidOid;
};

/** Apply PostgreSQL's parent-first lock and permission checks to one DROP target. */
static void lock_bioseq_drop_target(const RangeVar* relation, Oid relid, Oid old_relid, void* arg) {
    auto* state = static_cast<bioseq_drop_lock_state*>(arg);
    if (relid != old_relid && OidIsValid(state->partition_parent_oid)) {
        UnlockRelationOid(state->partition_parent_oid, AccessExclusiveLock);
        state->partition_parent_oid = InvalidOid;
    }
    if (!OidIsValid(relid)) {
        return;
    }

    HeapTuple tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
    if (!HeapTupleIsValid(tuple)) {
        return;
    }
    Form_pg_class class_form = reinterpret_cast<Form_pg_class>(GETSTRUCT(tuple));
    const char relkind = class_form->relkind;
    const bool is_partition = class_form->relispartition;
    const Oid namespace_oid = class_form->relnamespace;

    if (relkind != RELKIND_RELATION && relkind != RELKIND_PARTITIONED_TABLE) {
        ereport(ERROR,
                (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                 errmsg("\"%s\" is not a table", relation->relname),
                 errhint("Use the matching DROP command for this relation type.")));
    }
    if (!object_ownercheck(RelationRelationId, relid, GetUserId()) &&
        !object_ownercheck(NamespaceRelationId, namespace_oid, GetUserId())) {
        aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(relkind), relation->relname);
    }
    if (!allowSystemTableMods && IsSystemClass(relid, class_form)) {
        ereport(ERROR,
                (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                 errmsg("permission denied: \"%s\" is a system catalog", relation->relname)));
    }
    ReleaseSysCache(tuple);

    if (is_partition && relid != old_relid) {
        state->partition_parent_oid = get_partition_parent(relid, /* even_if_detached = */ true);
        if (OidIsValid(state->partition_parent_oid)) {
            LockRelationOid(state->partition_parent_oid, AccessExclusiveLock);
        }
    }
}

/** Find and lock every inheritance child that DROP may remove.
 *
 * Include partitions awaiting DETACH completion, since PostgreSQL's dependency
 * list  can still include them. Visit children breadth first and keep their
 * locks. PostgreSQL lists hold the work queue so an ERROR cannot skip C++
 * object cleanup. Shared children are visited once.
 */
static void append_drop_inheritance_closure(Oid root_relid, List** target_relids) {
    List* closure = list_make1_oid(root_relid);
    ListCell* agenda_cell = nullptr;
    foreach (agenda_cell, closure) {
        const Oid parent_relid = lfirst_oid(agenda_cell);
        List* children = find_inheritance_children_extended(parent_relid,
                                                            /* omit_detached = */ false,
                                                            AccessExclusiveLock,
                                                            nullptr,
                                                            nullptr);
        ListCell* child_cell = nullptr;
        foreach (child_cell, children) {
            const Oid child_relid = lfirst_oid(child_cell);
            if (!list_member_oid(closure, child_relid)) {
                closure = lappend_oid(closure, child_relid);
            }
        }
    }

    foreach (agenda_cell, closure) {
        const Oid member = lfirst_oid(agenda_cell);
        if (!list_member_oid(*target_relids, member)) {
            *target_relids = lappend_oid(*target_relids, member);
        }
    }
}
/** Test counters for DROP handling in this backend.
 *
 * They record decisions after the checks and never affect those checks.
 * PostgreSQL keeps the library loaded across DROP/CREATE EXTENSION, so the
 * counts survive recreating the extension in the same backend.
 */
static std::uint64_t drop_lock_fallback_count = 0;
static std::uint64_t missing_relation_count = 0;

/** Check pg_class with SnapshotSelf before handling a dropped relation
 *
 * Recursive  cleanup can remove the catalog row while a cached Relation still
 * exists. The catalog scan sees the current command's changes.
 */
[[nodiscard]] static bool relation_catalog_row_exists(const Oid relid) {
    Relation class_relation = table_open(RelationRelationId, AccessShareLock);
    ScanKeyData key;
    ScanKeyInit(&key, Anum_pg_class_oid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(relid));
    SysScanDesc scan =
        systable_beginscan(class_relation, ClassOidIndexId, true, SnapshotSelf, 1, &key);
    const bool exists = HeapTupleIsValid(systable_getnext(scan));
    systable_endscan(scan);
    table_close(class_relation, AccessShareLock);
    return exists;
}

/** Check whether, this backend holds the Bioseq extension's DROP lock.
 *
 * The library can first load during DROP EXTENSION, after its utility hook
 * would have run. PostgreSQL has already locked the extension by theg. The
 * exact extension OID and AccessExclusiveLock identify that case.
 */
[[nodiscard]] static bool extension_drop_lock_is_held() {
    const Oid extension_oid = get_extension_oid("bioseq", true);
    if (!OidIsValid(extension_oid)) {
        return false;
    }

    LOCKTAG extension_lock;
    SET_LOCKTAG_OBJECT(extension_lock, MyDatabaseId, ExtensionRelationId, extension_oid, 0);
    return LockHeldByMe(&extension_lock,
                        AccessExclusiveLock,
                        /* orstronger = */ false);
}

/** Check whether this callback runs during DROP EXTENSION bioseq. */
[[nodiscard]] static bool in_extension_teardown() {
    /* The utility hook sets this flag when Bioseq was loaded before DROP. */
    if (extension_teardown_active) {
        return true;
    }
    if (extension_drop_lock_is_held()) {
        ++drop_lock_fallback_count;
        return true;
    }
    return false;
}

/** Return whether a utility parse tree drops the Bioseq extension by exact name. */
[[nodiscard]] static bool drops_bioseq_extension(Node* parsetree) {
    if (!IsA(parsetree, DropStmt)) {
        return false;
    }
    DropStmt* drop = reinterpret_cast<DropStmt*>(parsetree);
    if (drop->removeType != OBJECT_EXTENSION) {
        return false;
    }
    ListCell* cell = nullptr;
    foreach (cell, drop->objects) {
        /* DROP EXTENSION stores each target  as a String. Only the exact name
         * bioseq enables extension cleanup here. */
        Node* object = reinterpret_cast<Node*>(lfirst(cell));
        if (IsA(object, String) && std::strcmp(strVal(object), "bioseq") == 0) {
            return true;
        }
    }
    return false;
}

/** Validate cleanup catalogs before PostgreSQL starts dropping Bioseq tables.
 *
 * Lock named tables, then their inheritance children and partitions. Check both
 * cleanup catalogs for every Bioseq table. A failure after dropping starts can
 * leave backend-local storage state changed despite SQL rollback.
 *
 * Requires shared preloading. If this hook is present without it, reject the
 * Bioseq DROP. A hook installed during DROP cannot  check the command beforehand.
 */
static void preflight_bioseq_table_drop(Node* parsetree) {
    if (!IsA(parsetree, DropStmt)) {
        return;
    }
    DropStmt* drop = reinterpret_cast<DropStmt*>(parsetree);
    if (drop->removeType != OBJECT_TABLE) {
        return;
    }

    List* named_relids = NIL;
    ListCell* cell = nullptr;
    foreach (cell, drop->objects) {
        /* For OBJECT_TABLE each object is PostgreSQL's qualified-name List. */
        List* qualified_name = reinterpret_cast<List*>(lfirst(cell));
        RangeVar* relation = makeRangeVarFromNameList(qualified_name);
        /* Refresh catalog invalidations even when the relation lock was already
         * held, so the child scan sees the current relhassubclass value. */
        AcceptInvalidationMessages();
        bioseq_drop_lock_state lock_state{};
        const Oid relid = RangeVarGetRelidExtended(relation,
                                                   AccessExclusiveLock,
                                                   drop->missing_ok ? RVR_MISSING_OK : 0,
                                                   lock_bioseq_drop_target,
                                                   &lock_state);
        if (OidIsValid(relid) && !list_member_oid(named_relids, relid)) {
            named_relids = lappend_oid(named_relids, relid);
        }
    }

    /* Lock all named parents before their children to keep a shared child
     * from changing the named-target lock order Refresh catalog invalidations
     * received, while waiting for those locks before looking for hildren. */
    AcceptInvalidationMessages();
    List* target_relids = NIL;
    foreach (cell, named_relids) {
        append_drop_inheritance_closure(lfirst_oid(cell), &target_relids);
    }

    List* bioseq_relids = NIL;
    foreach (cell, target_relids) {
        const Oid relid = lfirst_oid(cell);
        if (relation_oid_uses_column_table_access_method(relid)) {
            bioseq_relids = lappend_oid(bioseq_relids, relid);
        }
    }

    if (bioseq_relids != NIL && !bioseq::postgres::scheduler::supervisor_available()) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq table DROP requires shared preload"),
                 errdetail("the ProcessUtility hook is active, but the bioseq shared-preload state "
                           "is unavailable"),
                 errhint("Add bioseq to shared_preload_libraries, restart PostgreSQL, and retry "
                         "the DROP.")));
    }

    foreach (cell, bioseq_relids) {
        /* Validate both cleanup catalogs for every locked Bioseq table. */
        (void)lfirst_oid(cell);
        bioseq::storage::bioseq_default_storage_manager::validate_drop_catalog_contracts();
    }
}

/** Remove Bioseq-owned storage and metadata for one PostgreSQL DROP callback. */
static void handle_column_table_drop(Oid relid) {
    bioseq_log<logging_level::debug>("[bioseq] pg: handle_column_table_drop() entered");
    const bool extension_teardown = in_extension_teardown();

    /* PostgreSQL computes the DROP EXTENSION dependency list in advance. Let
     * it remove generated child tables and indexes from  that list. A missing
     * relation is an error even during extension removal.
     *
     * Keep the relation open only long enough to identify its access method and
     * storage. Release the cache reference before catalog changes can refresh
     * it, and keep the relation lock until the transaction ends. */
    if (!relation_catalog_row_exists(relid)) {
        ++missing_relation_count;
        if (extension_teardown) {
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
    Relation drop_target =
        bioseq::postgres::support::try_open_postgres_relation(relid, AccessShareLock);
    if (drop_target == nullptr) {
        ++missing_relation_count;
        if (extension_teardown) {
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
    const bool is_bioseq_relation = relation_uses_column_table_access_method(drop_target);
    const bool is_external_relation =
        bioseq::postgres::external::uses_table_access_method(drop_target);
    const RelFileLocator relfilelocator =
        is_bioseq_relation ? drop_target->rd_locator : RelFileLocator{};
    const Oid relation_oid = RelationGetRelid(drop_target);

    /* Source invalidation can advance the command counter and rebuild cached
     * relations. Release our reference first, keeping the transaction  lock. */
    relation_close(drop_target, NoLock);
    drop_target = nullptr;

    if (is_external_relation) {
        (void)bioseq::postgres::external::delete_relation_options(relation_oid,
                                                                  /* missing_ok = */ true);
    }

    /* DROP EXTENSION removes source rows through PostgreSQL's dependency
     * list. Skip source invalidation then, since the loaded library may be
     * newer than the catalog layout being removed. */
    if (!extension_teardown &&
        bioseq::storage::internal_catalog_detail::bioseq_extension_installed() &&
        bioseq::storage::is_registered_sequence_fragment_relation(relid)) {
        bioseq::storage::sequence_source_version_manager::invalidate_for_fragment(relid);
    }

    if (is_bioseq_relation) {
        /* Check the fragment and source catalogs before changing parent metadata
         * or marking its storage as dropped. Child cleanu performs those checks
         * first. During DROP EXTENSION, PostgreSQL removes the children itself. */
        if (!extension_teardown) {
            const bool saved_internal_cleanup = internal_drop_cleanup_active;
            std::exception_ptr cpp_error;
            internal_drop_cleanup_active = true;
            PG_TRY();
            {
                try {
                    bioseq::storage::bioseq_default_storage_manager::drop_sub_tables_for_main(
                        relfilelocator.dbOid, relfilelocator.spcOid, relfilelocator.relNumber);
                } catch (...) {
                    cpp_error = std::current_exception();
                }
            }
            PG_CATCH();
            {
                internal_drop_cleanup_active = saved_internal_cleanup;
                PG_RE_THROW();
            }
            PG_END_TRY();
            internal_drop_cleanup_active = saved_internal_cleanup;
            if (cpp_error != nullptr) {
                std::rethrow_exception(cpp_error);
            }
        }

        delete_metadata_rows(relfilelocator);
        delete_column_table_options(relation_oid, true);
        mark_write_state_relation_dropped(relfilelocator.relNumber, GetCurrentSubTransactionId());
    }
}

/** Reject row-level AFTER triggers that the column table AM cannot maintain. */
static void validate_column_trigger(Oid tgid) {
    bioseq_log<logging_level::debug>("[bioseq] pg: validate_column_trigger() entered");

    std::array<ScanKeyData, 1> scan_key;
    Relation tgrel = table_open(TriggerRelationId, AccessShareLock);

    ScanKeyInit(scan_key.data(),
                Anum_pg_trigger_oid,
                BTEqualStrategyNumber,
                F_OIDEQ,
                ObjectIdGetDatum(tgid));

    SysScanDesc tgscan =
        systable_beginscan(tgrel, TriggerOidIndexId, true, SnapshotSelf, 1, scan_key.data());

    HeapTuple tgtup = systable_getnext(tgscan);

    if (!HeapTupleIsValid(tgtup)) {
        table_close(tgrel, AccessShareLock);
        return;
    }

    Form_pg_trigger tgrec = reinterpret_cast<Form_pg_trigger>(GETSTRUCT(tgtup));

    Oid tgrelid = tgrec->tgrelid;
    int16_t tgtype = tgrec->tgtype;

    systable_endscan(tgscan);
    table_close(tgrel, AccessShareLock);

    if (TRIGGER_FOR_ROW(tgtype) && TRIGGER_FOR_AFTER(tgtype) &&
        relation_oid_uses_column_table_access_method(tgrelid)) {
        bioseq_report<logging_level::error, logging_target::postgres>()
            .with_sql_error_code(ERRCODE_FEATURE_NOT_SUPPORTED)
            .with_message(
                "Foreign keys and AFTER ROW triggers are not supported for columnar tables")
            .with_hint("Consider an AFTER STATEMENT trigger instead.");
    }
}

/** Dispatch one PostgreSQL object-access event after the previously installed hook. */
static void handle_column_table_object_access(
    ObjectAccessType access, Oid class_id, Oid object_id, int sub_id, void* arg) {
    if (previous_object_access_hook) {
        previous_object_access_hook(access, class_id, object_id, sub_id, arg);
    }

    if (access == OAT_DROP && class_id == RelationRelationId && !OidIsValid(sub_id)) {
        handle_column_table_drop(object_id);
    } else if (access == OAT_POST_CREATE && class_id == RelationRelationId && sub_id == 0 &&
               external_create_capture_active) {
        Relation created_relation = relation_open(object_id, NoLock);
        const bool is_external_relation =
            bioseq::postgres::external::uses_table_access_method(created_relation);
        relation_close(created_relation, NoLock);
        if (is_external_relation) {
            if (OidIsValid(captured_external_create_oid) &&
                captured_external_create_oid != object_id) {
                ereport(
                    ERROR,
                    (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                     errmsg("bioseq_external CREATE emitted multiple relation creation events")));
            }
            captured_external_create_oid = object_id;
        }
    } else if (access == OAT_POST_CREATE && class_id == TriggerRelationId) {
        validate_column_trigger(object_id);
    }
}

/** Convert C++ exceptions from the object-access hook into PostgreSQL errors. */
static void column_table_object_access_hook(
    ObjectAccessType access, Oid class_id, Oid object_id, int sub_id, void* arg) {
    const bool saved_extension_teardown = extension_teardown_active;
    const bool saved_internal_cleanup = internal_drop_cleanup_active;
    ErrorData* exact_error = nullptr;
    std::array<char, 512> detail{"unknown C++ exception"};
    try {
        handle_column_table_object_access(access, class_id, object_id, sub_id, arg);
        return;
    } catch (bioseq::postgres::support::postgres_error& error) {
        exact_error = error.release_error_data();
    } catch (const std::exception& error) {
        strlcpy(detail.data(),
                error.what() == nullptr ? "std::exception without detail" : error.what(),
                detail.size());
    } catch (...) {
    }
    extension_teardown_active = saved_extension_teardown;
    internal_drop_cleanup_active = saved_internal_cleanup;
    if (exact_error != nullptr) {
        ReThrowError(exact_error);
    }
    ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("bioseq object-access hook raised a C++ exception"),
             errdetail_internal("%s", detail.data())));
}

/** Reject text COPY FROM into letter columns with no declared alphabet before  writing rows. */
static void preflight_typmodless_letter_copy(Node* parsetree) {
    if (!IsA(parsetree, CopyStmt)) {
        return;
    }
    auto* copy = reinterpret_cast<CopyStmt*>(parsetree);
    if (!copy->is_from || copy->relation == nullptr) {
        return;
    }

    CopyFormatOptions options{};
    ProcessCopyOptions(nullptr, &options, true, copy->options);
#if PG_VERSION_NUM >= 190000
    const bool binary_input = options.format == COPY_FORMAT_BINARY;
#else
    const bool binary_input = options.binary;
#endif
    if (binary_input) {
        /* Binary letter input carries and validates its physical alphabet. */
        return;
    }

    const Oid namespace_oid = get_namespace_oid("bioseq", true);
    if (!OidIsValid(namespace_oid)) {
        return;
    }
    const Oid letter_oid = GetSysCacheOid2(
        TYPENAMENSP, Anum_pg_type_oid, PointerGetDatum("letter"), ObjectIdGetDatum(namespace_oid));
    if (!OidIsValid(letter_oid)) {
        return;
    }

    Relation relation = table_openrv(copy->relation, RowExclusiveLock);
    List* attribute_numbers = CopyGetAttnums(RelationGetDescr(relation), relation, copy->attlist);
    ListCell* cell = nullptr;
    foreach (cell, attribute_numbers) {
        const AttrNumber attribute_number = static_cast<AttrNumber>(lfirst_int(cell));
        if (attribute_number <= 0 || attribute_number > RelationGetDescr(relation)->natts) {
            continue;
        }
        Form_pg_attribute attribute =
            bioseq::postgres::support::tuple_descriptor_attribute(RelationGetDescr(relation), attribute_number - 1);
        if (!attribute->attisdropped && attribute->atttypid == letter_oid &&
            attribute->atttypmod < 0) {
            char* attribute_name = pstrdup(NameStr(attribute->attname));
            list_free(attribute_numbers);
            table_close(relation, NoLock);
            ereport(
                ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bioseq.letter text COPY requires an alphabet typmod"),
                 errdetail(
                     "target column \"%s\" has base type bioseq.letter without an alphabet typmod",
                     attribute_name),
                 errhint("declare the target as bioseq.letter('dna4') or another supported "
                         "alphabet, or use validated binary COPY")));
        }
    }
    list_free(attribute_numbers);
    table_close(relation, NoLock);
}

/** Check Bioseq DDL and COPY commands, then pass them to, the previous utility hook */
static void process_column_table_utility(PlannedStmt* pstmt,
                                         const char* query_string,
                                         bool read_only_tree,
                                         ProcessUtilityContext context,
                                         ParamListInfo params,
                                         QueryEnvironment* query_environment,
                                         DestReceiver* dest,
                                         QueryCompletion* completion_tag) {
    if (read_only_tree) {
        pstmt = copyObject(pstmt);
    }

    Node* parsetree = pstmt->utilityStmt;
    bioseq::postgres::external::create_relation_options external_options{};
    CreateStmt* external_create = nullptr;
    if (IsA(parsetree, CreateStmt) && bioseq::postgres::external::is_external_create_statement(
                                          reinterpret_cast<CreateStmt*>(parsetree))) {
        /* Copy the caller's parse tree before removing external-table options.
         * A read-only tree copied above is already ours to change. */
        if (!read_only_tree) {
            pstmt = copyObject(pstmt);
            parsetree = pstmt->utilityStmt;
        }
        external_create = reinterpret_cast<CreateStmt*>(parsetree);
        external_options =
            bioseq::postgres::external::consume_create_relation_options(external_create);
    }

    preflight_typmodless_letter_copy(parsetree);

    if (IsA(parsetree, IndexStmt)) {
        IndexStmt* index_statement = reinterpret_cast<IndexStmt*>(parsetree);

        Relation rel =
            relation_openrv(index_statement->relation,
                            index_statement->concurrent ? ShareUpdateExclusiveLock : ShareLock);

        if (relation_oid_uses_column_table_access_method(RelationGetRelid(rel)) &&
            !supports_index_access_method(index_statement->accessMethod)) {
            bioseq_report<logging_level::error, logging_target::postgres>()
                .with_sql_error_code(ERRCODE_FEATURE_NOT_SUPPORTED)
                .with_message("unsupported access method for the index on columnar table {} ({})",
                              RelationGetRelationName(rel),
                              index_statement->accessMethod);
        }

        RelationClose(rel);
    }

    if (!internal_drop_cleanup_active) {
        preflight_bioseq_table_drop(parsetree);
    }

    /* Nested DROP commands re-enter this hook. Restore the outer command's
     * flags on success and on PostgreSQL errors. */
    const bool saved_teardown_context = extension_teardown_active;
    if (drops_bioseq_extension(parsetree)) {
        extension_teardown_active = true;
    }
    const bool saved_external_create_capture = external_create_capture_active;
    const Oid saved_captured_external_create_oid = captured_external_create_oid;
    if (external_options.applies) {
        external_create_capture_active = true;
        captured_external_create_oid = InvalidOid;
    }
    std::exception_ptr cpp_error;
    PG_TRY();
    {
        try {
            previous_process_utility_hook(pstmt,
                                          query_string,
                                          read_only_tree,
                                          context,
                                          params,
                                          query_environment,
                                          dest,
                                          completion_tag);
            if (external_options.applies) {
                const Oid created_relation_oid = captured_external_create_oid;
                external_create_capture_active = saved_external_create_capture;
                captured_external_create_oid = saved_captured_external_create_oid;
                if (OidIsValid(created_relation_oid)) {
                    bioseq::postgres::external::persist_create_relation_options(
                        created_relation_oid, external_options);
                } else if (!external_create->if_not_exists) {
                    ereport(ERROR,
                            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                             errmsg("bioseq_external CREATE completed without a relation creation "
                                    "event")));
                }
            }
        } catch (...) {
            external_create_capture_active = saved_external_create_capture;
            captured_external_create_oid = saved_captured_external_create_oid;
            cpp_error = std::current_exception();
        }
    }
    PG_CATCH();
    {
        extension_teardown_active = saved_teardown_context;
        external_create_capture_active = saved_external_create_capture;
        captured_external_create_oid = saved_captured_external_create_oid;
        PG_RE_THROW();
    }
    PG_END_TRY();
    extension_teardown_active = saved_teardown_context;
    external_create_capture_active = saved_external_create_capture;
    captured_external_create_oid = saved_captured_external_create_oid;
    if (cpp_error != nullptr) {
        std::rethrow_exception(cpp_error);
    }
}

/** Convert C++ exceptions from the utility hook into PostgreSQL errors. */
static void column_table_process_utility_hook(PlannedStmt* pstmt,
                                              const char* query_string,
                                              bool read_only_tree,
                                              ProcessUtilityContext context,
                                              ParamListInfo params,
                                              QueryEnvironment* query_environment,
                                              DestReceiver* dest,
                                              QueryCompletion* completion_tag) {
    const bool saved_extension_teardown = extension_teardown_active;
    const bool saved_internal_cleanup = internal_drop_cleanup_active;
    ErrorData* exact_error = nullptr;
    std::array<char, 512> detail{"unknown C++ exception"};
    try {
        process_column_table_utility(pstmt,
                                     query_string,
                                     read_only_tree,
                                     context,
                                     params,
                                     query_environment,
                                     dest,
                                     completion_tag);
        return;
    } catch (bioseq::postgres::support::postgres_error& error) {
        exact_error = error.release_error_data();
    } catch (const std::exception& error) {
        strlcpy(detail.data(),
                error.what() == nullptr ? "std::exception without detail" : error.what(),
                detail.size());
    } catch (...) {
    }
    extension_teardown_active = saved_extension_teardown;
    internal_drop_cleanup_active = saved_internal_cleanup;
    if (exact_error != nullptr) {
        ReThrowError(exact_error);
    }
    ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("bioseq ProcessUtility hook raised a C++ exception"),
             errdetail_internal("%s", detail.data())));
}

} // namespace

/** Install Bioseq object-access and utility hooks, saving the previous hooks. */
export void install_column_table_hooks() {
    previous_object_access_hook = object_access_hook;
    object_access_hook = column_table_object_access_hook;

    previous_process_utility_hook =
        ProcessUtility_hook ? ProcessUtility_hook : standard_ProcessUtility;
    ProcessUtility_hook = column_table_process_utility_hook;
}

export namespace bioseq::table {

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::volatile_,
    .parallel = bioseq::sql::parallel_safety::unsafe,
})]] [[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/** Return this backend's extension-lock fallback and missing-relation counts. */
[[nodiscard]] bioseq::postgres::support::named_output<teardown_diagnostics_row>
    teardown_diagnostics() noexcept {
    const auto clamp_counter = [](std::uint64_t value) noexcept {
        return static_cast<std::int64_t>(std::min<std::uint64_t>(
            value, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())));
    };
    return bioseq::postgres::support::named_output<teardown_diagnostics_row>{
        teardown_diagnostics_row{
            .lock_fallback_classifications = clamp_counter(drop_lock_fallback_count),
            .missing_relation_observations = clamp_counter(missing_relation_count),
        }};
}

} // namespace bioseq::table
