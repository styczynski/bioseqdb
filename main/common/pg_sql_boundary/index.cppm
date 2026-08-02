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
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

export module bioseq.pg_sql_boundary;

export import :call_context;
export import :error_boundary;
export import :owning_values;
export import :result_materialization;
export import :scalar_conversion;
export import :shape;

export namespace bioseq::pg_utils {

struct sql_boundary_probe_descriptor final {
    std::string_view site_id;
    std::string_view fault_key;
};

/**
 * Translate one generated, typed SQL callback back to PostgreSQL's C ABI.
 */
template<class F>
    requires std::invocable<F&>
          && std::same_as<std::invoke_result_t<F&>, Datum>
          && std::is_trivially_destructible_v<std::remove_cvref_t<F>>
[[nodiscard]] inline Datum invoke_sql_boundary(
        sql_boundary_probe_descriptor probe,
        F&& call) {
    ErrorData* exact_error = nullptr;
    bool standard_error = false;
    bool unknown_error = false;
    bool bridge_capture_failure = false;
    char detail[512] = "std::exception without detail";

    try {
        const Datum result = std::invoke(call);
        if (safety_runtime_probe::consume_fault(
                probe.fault_key)) {
            safety_runtime_probe::destructor_sentinel
                boundary_sentinel{
                    "generated_boundary_callable"};
            throw std::runtime_error{
                "bioseq safety runtime injected boundary exception"};
        }
        return result;
    } catch (PostgresErrorAsCpp& error) {
        exact_error = error.release_error_data();
    } catch (const PostgresErrorCaptureFailure& error) {
        strlcpy(
            detail,
            error.what(),
            sizeof(detail));
        standard_error = true;
        bridge_capture_failure = true;
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

    if (!bridge_capture_failure) {
        safety_runtime_probe::record_site_visit(probe.site_id);
    }
    ereport(
        ERROR,
        (errcode(ERRCODE_INTERNAL_ERROR),
         errmsg("bioseq SQL callback raised a C++ exception"),
         errdetail(
             "%s",
             unknown_error
                 ? "unknown non-standard C++ exception"
                 : (standard_error
                        ? detail
                        : "empty C++ exception state"))));
    pg_unreachable();
}

}  // namespace bioseq::pg_utils

namespace bioseq::pg_utils::detail {

struct sql_boundary_probe_callable {
    [[nodiscard]] Datum operator()() const noexcept {
        return static_cast<Datum>(0);
    }
};

struct rejected_sql_boundary_probe_callable {
    ~rejected_sql_boundary_probe_callable() {}

    [[nodiscard]] Datum operator()() const noexcept {
        return static_cast<Datum>(0);
    }
};

template<class F>
concept accepted_sql_boundary_callable =
    requires(F&& call) {
        {
            invoke_sql_boundary(
                sql_boundary_probe_descriptor{
                    .site_id = "probe:v2:fixture",
                    .fault_key = "fixture.fault",
                },
                std::forward<F>(call))
        } -> std::same_as<Datum>;
    };

template<class F>
concept descriptor_free_sql_boundary_callable =
    requires(F&& call) {
        invoke_sql_boundary(std::forward<F>(call));
    };

using sql_boundary_probe_signature =
    Datum (*)(
        sql_boundary_probe_descriptor,
        sql_boundary_probe_callable&&);

[[maybe_unused]] inline constexpr sql_boundary_probe_signature
    sql_boundary_instantiation =
        &invoke_sql_boundary<sql_boundary_probe_callable>;

static_assert(std::is_trivially_copyable_v<
              sql_boundary_probe_descriptor>);
static_assert(accepted_sql_boundary_callable<
              sql_boundary_probe_callable>);
static_assert(!descriptor_free_sql_boundary_callable<
              sql_boundary_probe_callable>);
static_assert(!accepted_sql_boundary_callable<
              rejected_sql_boundary_probe_callable>);
static_assert(sql_boundary_instantiation != nullptr);

}  // namespace bioseq::pg_utils::detail
