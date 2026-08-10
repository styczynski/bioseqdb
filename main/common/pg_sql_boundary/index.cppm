/*-------------------------------------------------------------------------
 *
 * common/pg_sql_boundary/index.cppm
 *
 * Public dependency surface for generated PostgreSQL SQL boundaries.
 *
 *-------------------------------------------------------------------------
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

export module bioseq.pg_sql_boundary;

export import :call_context;
export import :error_boundary;
export import :owning_values;
export import :result_materialization;
export import :scalar_conversion;
export import :shape;

export namespace bioseq::postgres::support {

/**
 * Invoke one typed C++ callback and translate failures to PostgreSQL's C ABI.
 *
 * The callback must be trivially destructible because PostgreSQL ERROR uses
 * non-local control flow. PostgreSQL errors captured as C++ exceptions retain
 * their original ErrorData; standard C++ failures become internal SQL errors.
 *
 * @tparam callback_type Trivially destructible nullary Datum callable.
 * @param callback Callback invoked exactly once.
 * @return Datum returned by the callback.
 */
template<class callback_type>
    requires std::invocable<callback_type&>
          && std::same_as<std::invoke_result_t<callback_type&>, Datum>
          && std::is_trivially_destructible_v<
              std::remove_cvref_t<callback_type>>
[[nodiscard]] inline Datum invoke_postgres_callback(
        callback_type&& callback) {
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
