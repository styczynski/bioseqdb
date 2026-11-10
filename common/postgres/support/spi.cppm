/**
 * @file common/postgres/support/spi.cppm
 * @brief Keep an SPI connection open for a C++ scope and close it on exit.
 */
module;

extern "C" {
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_class.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_extension.h"
#include "commands/extension.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/errcodes.h"
}

#include <cstring>
#include <functional>
#include <type_traits>
#include <utility>

export module bioseq.postgres.support:spi;

import :error_boundary;

export namespace bioseq::postgres::support {

/** Stabilize one table's qualified name while ordinary utility hooks execute.
 * The table lock lasts until transaction end; the namespace-catalog ShareLock
 * lasts only for this scope. PostgreSQL namespace DDL takes RowExclusiveLock,
 * so another backend cannot replace the schema name during the operation.
 * Our permanent object-access hook also rejects same-backend rename/move/drop
 * before the nested DDL can return to the outer lookup. Other utility/event
 * hooks still run normally. Intentional command rewriting by a trusted utility
 * hook is outside this name-stability contract.
 *
 * Construct outside invoke_pg_as_cpp and bridge every PG operation while this
 * owner is live. The scope stack belongs only to the object-access hook; SQL
 * functions never inspect it or switch their behavior based on it.
 */
class scoped_relation_identity final {
  public:
    /** Lock the OID, then capture names under both locks. */
    explicit scoped_relation_identity(Oid relation_id) : relation_id_(relation_id) {
        try {
            invoke_pg_as_cpp([this] {
                LockRelationOid(relation_id_, AccessExclusiveLock);
                LockRelationOid(NamespaceRelationId, ShareLock);
                namespace_locked_ = true;
                namespace_id_ = get_rel_namespace(relation_id_);
                const auto* relation_name = get_rel_name(relation_id_);
                const auto* namespace_name = get_namespace_name(namespace_id_);
                if (relation_name == nullptr || namespace_name == nullptr)
                    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE),
                        errmsg("table %u disappeared before automatic column creation", relation_id_)));
                namestrcpy(&relation_name_, relation_name);
                namestrcpy(&namespace_name_, namespace_name);
            });
        } catch (...) {
            release_namespace_lock();
            throw;
        }
        previous_ = active_;
        active_ = this;
    }

    /** Restore the outer guard and release only this namespace lock acquisition. */
    ~scoped_relation_identity() noexcept {
        active_ = previous_;
        release_namespace_lock();
    }

    scoped_relation_identity(const scoped_relation_identity&) = delete;
    scoped_relation_identity& operator=(const scoped_relation_identity&) = delete;
    scoped_relation_identity(scoped_relation_identity&&) = delete;
    scoped_relation_identity& operator=(scoped_relation_identity&&) = delete;

    /** Quote the stabilized name in PostgreSQL's current memory context. */
    [[nodiscard]] const char* qualified_name() const {
        return invoke_pg_as_cpp([this] {
            return quote_qualified_identifier(NameStr(namespace_name_), NameStr(relation_name_));
        });
    }

    /** PG-leaf object-hook check, called on both sides of the previous hook.
     * SnapshotSelf sees the catalog tuple just changed by the current command;
     * a syscache lookup could still see the pre-command identity here.
     */
    static void validate_object_access(ObjectAccessType access, Oid class_id,
                                      Oid object_id, int sub_id) {
        if (access != OAT_DROP && access != OAT_POST_ALTER) return;
        for (const auto* guard = active_; guard != nullptr; guard = guard->previous_) {
            const bool target_relation = class_id == RelationRelationId && object_id == guard->relation_id_;
            const bool target_namespace = class_id == NamespaceRelationId && object_id == guard->namespace_id_;
            if (!target_relation && !target_namespace) continue;
            if ((access == OAT_DROP && sub_id == 0)
                || (access == OAT_POST_ALTER && !guard->identity_matches(class_id)))
                ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                    errmsg("table or schema identity changed during automatic column creation"),
                    errhint("Complete table and schema renames before loading, or predeclare the destination columns.")));
        }
    }

  private:
    /** Compare one just-altered catalog tuple with the captured identity. */
    [[nodiscard]] bool identity_matches(Oid class_id) const {
        const bool is_relation = class_id == RelationRelationId;
        const auto catalog = table_open(class_id, AccessShareLock);
        ScanKeyData key{};
        ScanKeyInit(&key, is_relation ? Anum_pg_class_oid : Anum_pg_namespace_oid,
            BTEqualStrategyNumber, F_OIDEQ,
            ObjectIdGetDatum(is_relation ? relation_id_ : namespace_id_));
        const auto scan = systable_beginscan(catalog,
            is_relation ? ClassOidIndexId : NamespaceOidIndexId,
            true, SnapshotSelf, 1, &key);
        const auto tuple = systable_getnext(scan);
        bool matches = false;
        if (HeapTupleIsValid(tuple)) {
            if (is_relation) {
                const auto* value = reinterpret_cast<Form_pg_class>(GETSTRUCT(tuple));
                matches = value->relnamespace == namespace_id_
                    && std::strcmp(NameStr(value->relname), NameStr(relation_name_)) == 0;
            } else {
                const auto* value = reinterpret_cast<Form_pg_namespace>(GETSTRUCT(tuple));
                matches = std::strcmp(NameStr(value->nspname), NameStr(namespace_name_)) == 0;
            }
        }
        systable_endscan(scan);
        table_close(catalog, AccessShareLock);
        return matches;
    }

    /** Release our catalog lock after success or error without hiding the error. */
    void release_namespace_lock() noexcept {
        if (!namespace_locked_) return;
        namespace_locked_ = false;
        try {
            invoke_pg_as_cpp([] { UnlockRelationOid(NamespaceRelationId, ShareLock); });
        } catch (...) {
            // Transaction abort owns any lock left after a cleanup failure.
        }
    }

    inline static scoped_relation_identity* active_ = nullptr;
    scoped_relation_identity* previous_ = nullptr;
    Oid relation_id_ = InvalidOid;
    Oid namespace_id_ = InvalidOid;
    NameData relation_name_{};
    NameData namespace_name_{};
    bool namespace_locked_ = false;
};

/** Execute trusted internal SQL with its installed extension owner's privileges.
 * This C++ boundary accepts only repository-generated private catalog/storage
 * operations, never caller SQL, user-table scans, defaults or constraints.
 * Callers own SPI and their transaction. The supplied PG leaf runs behind the
 * existing error bridge so both PostgreSQL ERROR and C++ failure restore the
 * original role before escaping. Roll back every scoped GUC change, including
 * changes made by DDL callbacks; no session setting survives this operation.
 * Fixed heap/database defaults also prevent caller GUCs from redirecting private
 * storage creation. Explicit access methods/tablespaces in generated DDL win.
 */
template<class Function>
auto invoke_extension_internal_sql_as_cpp(const char* extension_name, Function&& function)
    -> std::invoke_result_t<std::remove_reference_t<Function>&> {
    using result_type = std::invoke_result_t<std::remove_reference_t<Function>&>;
    static_assert(!std::is_reference_v<result_type>);
    Oid saved_user = InvalidOid;
    int saved_security_context = 0;
    GetUserIdAndSecContext(&saved_user, &saved_security_context);
    int settings_level = 0;
    const auto restore = [saved_user, saved_security_context, &settings_level] {
        // PostgreSQL explicitly guarantees this setter cannot throw an error.
        SetUserIdAndSecContext(saved_user, saved_security_context);
        if (settings_level != 0) {
            invoke_pg_as_cpp([level = settings_level] { AtEOXact_GUC(false, level); });
            settings_level = 0;
        }
    };
    try {
        const auto operation = [extension_name, &function, &settings_level,
                                saved_security_context]() -> result_type {
            const Oid extension_oid = get_extension_oid(extension_name, false);
            const HeapTuple tuple = SearchSysCache1(EXTENSIONOID, ObjectIdGetDatum(extension_oid));
            if (!HeapTupleIsValid(tuple))
                elog(ERROR, "cache lookup failed for extension %u", extension_oid);
            const Oid owner = reinterpret_cast<Form_pg_extension>(GETSTRUCT(tuple))->extowner;
            ReleaseSysCache(tuple);
            settings_level = NewGUCNestLevel();
            SetUserIdAndSecContext(owner,
                saved_security_context | SECURITY_LOCAL_USERID_CHANGE | SECURITY_RESTRICTED_OPERATION);
            RestrictSearchPath();
            static_cast<void>(set_config_option("default_table_access_method", "heap",
                PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, ERROR, false));
            static_cast<void>(set_config_option("default_tablespace", "",
                PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, ERROR, false));
            return std::invoke(function);
        };
        if constexpr (std::is_void_v<result_type>) {
            invoke_pg_as_cpp(operation);
            restore();
        } else {
            auto result = invoke_pg_as_cpp(operation);
            restore();
            return result;
        }
    } catch (...) {
        try {
            restore();
        } catch (...) {
            // Identity was restored first. Preserve the operation error; the
            // enclosing PG transaction abort owns any remaining GUC rollback.
        }
        throw;
    }
}

/**
 * Own one nested SPI connection.
 * These methods turn PostgreSQL errors into C++ exceptions so the owner can
 * be destroyed normally. Call finish to check the SPI status yourself. The
 * destructor attempts cleanup and discards any failure.
 */
class scoped_spi_connection final {
  public:
    /** Open SPI, converting a PostgreSQL connection error into a C++ exception. */
    scoped_spi_connection() {
        invoke_pg_as_cpp([] {
            const int status = SPI_connect();
            if (status != SPI_OK_CONNECT) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg_internal("could not connect to SPI: status %d", status)));
            }
        });
        is_connected_ = true;
    }

    /** Attempt to finish an open connection and discard cleanup errors. */
    ~scoped_spi_connection() noexcept {
        if (!is_connected_) {
            return;
        }
        try {
            static_cast<void>(finish());
        } catch (...) {
            // Keep the destructor noexcept by discarding the, cleanup exception.
        }
    }

    scoped_spi_connection(const scoped_spi_connection&) = delete;
    scoped_spi_connection& operator=(const scoped_spi_connection&) = delete;
    scoped_spi_connection(scoped_spi_connection&&) = delete;
    scoped_spi_connection& operator=(scoped_spi_connection&&) = delete;

    /** Returns whether this owner currently holds an SPI connection. */
    [[nodiscard]] bool is_connected() const noexcept {
        return is_connected_;
    }

    /**
     * Run a parameterized SQL statement through this SPI connection.
     * PostgreSQL errors become  C++ exceptions.
     *
     * @return The status from SPI_execute_with_args.
     */
    [[nodiscard]] int execute_with_args(const char* command,
                                        int argument_count,
                                        Oid* argument_types,
                                        Datum* argument_values,
                                        const char* nulls,
                                        bool is_read_only,
                                        long tuple_count) {
        const bool connection_is_open = is_connected_;
        return invoke_pg_as_cpp([command,
                                 argument_count,
                                 argument_types,
                                 argument_values,
                                 nulls,
                                 is_read_only,
                                 tuple_count,
                                 connection_is_open] {
            if (!connection_is_open) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg_internal("cannot execute through a closed SPI scope")));
            }
            return SPI_execute_with_args(command,
                                         argument_count,
                                         argument_types,
                                         argument_values,
                                         nulls,
                                         is_read_only,
                                         tuple_count);
        });
    }

    /**
     * Finish this SPI connection and return its status.
     * Calling finish again after it returns yields SPI_OK_FINISH.
     */
    [[nodiscard]] int finish() {
        if (!is_connected_) {
            return SPI_OK_FINISH;
        }
        const int status = invoke_pg_as_cpp([] { return SPI_finish(); });
        is_connected_ = false;
        return status;
    }

  private:
    bool is_connected_ = false;
};

static_assert(!std::is_copy_constructible_v<scoped_spi_connection>);
static_assert(!std::is_move_constructible_v<scoped_spi_connection>);
static_assert(std::is_nothrow_destructible_v<scoped_spi_connection>);

} // namespace bioseq::postgres::support
