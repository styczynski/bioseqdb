/**
 * @file common/postgres/sql_boundary/index.cppm
 * @brief Shared conversions and error handling for generated PostgreSQL entry points.
 */
module;

extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "utils/elog.h"
#include "utils/errcodes.h"
}

#include <concepts>
#include <exception>
#include <functional>
#include <type_traits>

export module bioseq.postgres.sql_boundary;

export import :call_context;
export import :error_boundary;
export import :owning_values;
export import :result_materialization;
export import :scalar_conversion;
export import :shape;

export namespace bioseq::postgres::support {

/**
 * Run a C++ SQL callback and report its exceptions as PostgreSQL errors.
 *
 * Preserve captured PostgreSQL ErrorData. Report other exceptions as internal
 * SQL errors, after leaving the C++ catch blocks so exception objects can be
 * destroyed before PostgreSQL jumps out.
 *
 * @tparam p_callback_type A  callable taking no arguments and returning Datum.
 * Its captured state must need no destructor.
 * @param callback Called exactly once.
 * @return The callback's Datum result.
 */
template<class p_callback_type>
    requires std::invocable<p_callback_type&>
          && std::same_as<std::invoke_result_t<p_callback_type&>, Datum>
          && std::is_trivially_destructible_v<
              std::remove_cvref_t<p_callback_type>>
[[nodiscard]] inline Datum invoke_postgres_callback(
        p_callback_type&& callback) {
    ErrorData* exact_error = nullptr;
    bool standard_error = false;
    bool unknown_error = false;
    bool error_capture_failed = false;
    char detail[512] = "std::exception without detail";

    try {
        return std::invoke(callback);
    } catch (postgres_error& error) {
        exact_error = error.release_error_data();
    } catch (const postgres_error_capture_failure& error) {
        strlcpy(
            detail,
            error.what(),
            sizeof(detail));
        standard_error = true;
        error_capture_failed = true;
    } catch (const std::exception& error) {
        strlcpy(
            detail,
            error.what() == nullptr
                ? "std::exception without detail"
                : error.what(),
            sizeof(detail));
        standard_error = true;
    } catch (...) {
        unknown_error = true;
    }

    if (exact_error != nullptr) {
        ReThrowError(exact_error);
    }

    ereport(
        ERROR,
        (errcode(ERRCODE_INTERNAL_ERROR),
         errmsg("bioseq SQL callback raised a C++ exception"),
         errdetail(
             "%s",
             error_capture_failed
                 ? "PostgreSQL error capture failed"
                 : unknown_error
                 ? "unknown non-standard C++ exception"
                 : (standard_error
                        ? detail
                        : "empty C++ exception state"))));
    pg_unreachable();
}

}  // namespace bioseq::postgres::support
