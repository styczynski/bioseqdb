/**
 * @file common/postgres/support/spi.cppm
 * @brief Keep an SPI connection open for a C++ scope and close it on exit.
 */
module;

extern "C" {
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_extension.h"
#include "commands/extension.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "utils/guc.h"
#include "utils/syscache.h"
#include "utils/errcodes.h"
}

#include <functional>
#include <type_traits>
#include <utility>

export module bioseq.postgres.support:spi;

import :error_boundary;

export namespace bioseq::postgres::support {

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
