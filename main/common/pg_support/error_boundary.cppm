/*-------------------------------------------------------------------------
 *
 * common/pg_support/error_boundary.cppm
 *
 * PostgreSQL ERROR/C++ exception translation shared by the generic
 * PostgreSQL utilities and generated SQL boundaries.
 *
 *-------------------------------------------------------------------------
 */
module;

extern "C" {
#include "postgres.h"
#include "utils/elog.h"
#include "utils/errcodes.h"
#include "utils/memutils.h"
#include "utils/palloc.h"
#include "utils/resowner.h"
}

#include <concepts>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

export module bioseq.pg_support:error_boundary;

import :safety_runtime_probe;

export namespace bioseq::pg_utils {

/**
 * C++ representation of one PostgreSQL ERROR.
 *
 * PostgreSQL leaf boundaries throw this only after PG_END_TRY so callers
 * unwind ordinary C++ owners before an outer C ABI rethrows the ErrorData.
 */
class PostgresErrorAsCpp final : public std::exception {
public:
    explicit PostgresErrorAsCpp(ErrorData* error) noexcept
        : error_(error) {}

    PostgresErrorAsCpp(const PostgresErrorAsCpp&) = delete;
    PostgresErrorAsCpp& operator=(const PostgresErrorAsCpp&) = delete;
    PostgresErrorAsCpp(PostgresErrorAsCpp&& other) noexcept
        : error_(std::exchange(other.error_, nullptr)),
          records_destructor_{
              std::exchange(
                  other.records_destructor_,
                  false)} {}
    PostgresErrorAsCpp& operator=(PostgresErrorAsCpp&&) = delete;

    ~PostgresErrorAsCpp() noexcept override {
        ErrorData* unreleased = std::exchange(error_, nullptr);
        if (unreleased != nullptr) {
            FreeErrorData(unreleased);
        }
        if (records_destructor_) {
            safety_runtime_probe::record_destructor(
                "postgres_error_as_cpp");
        }
    }

    [[nodiscard]] const char* what() const noexcept override {
        return error_ == nullptr || error_->message == nullptr
            ? "PostgreSQL ERROR without a message"
            : error_->message;
    }

    [[nodiscard]] int sqlerrcode() const noexcept {
        return error_ == nullptr ? 0 : error_->sqlerrcode;
    }

    [[nodiscard]] ErrorData* release_error_data() noexcept {
        return std::exchange(error_, nullptr);
    }

private:
    ErrorData* error_ = nullptr;
    bool records_destructor_ = true;
};

/**
 * Raised after a secondary PostgreSQL ERROR prevents CopyErrorData from
 * preserving the original error for the C++ unwind.
 */
class PostgresErrorCaptureFailure final : public std::exception {
public:
    PostgresErrorCaptureFailure() noexcept = default;
    PostgresErrorCaptureFailure(
        const PostgresErrorCaptureFailure&) = delete;
    PostgresErrorCaptureFailure& operator=(
        const PostgresErrorCaptureFailure&) = delete;
    PostgresErrorCaptureFailure(
        PostgresErrorCaptureFailure&& other) noexcept
        : records_destructor_{
              std::exchange(
                  other.records_destructor_,
                  false)} {}
    PostgresErrorCaptureFailure& operator=(
        PostgresErrorCaptureFailure&&) = delete;

    ~PostgresErrorCaptureFailure() noexcept override {
        if (records_destructor_) {
            safety_runtime_probe::record_destructor(
                "postgres_error_capture_failure");
        }
    }

    [[nodiscard]] const char* what() const noexcept override {
        return "could not preserve PostgreSQL ERROR data";
    }

private:
    bool records_destructor_ = true;
};

using PgCppInvokeBody = void (*)(void*);

#if defined(__GNUC__) && !defined(__clang__)
[[gnu::noinline, gnu::noipa]]
#else
[[gnu::noinline]]
#endif
inline void invoke_pg_as_cpp_erased(
    void* callback_context,
    PgCppInvokeBody body);

#if defined(__GNUC__) && !defined(__clang__)
[[gnu::noinline, gnu::noipa]]
#else
[[gnu::noinline]]
#endif
inline void invoke_pg_across_transactions_as_cpp_erased(
    void* callback_context,
    PgCppInvokeBody body);

/** Invoke one PostgreSQL leaf and return or throw through the C++ ABI. */
template<class F>
auto invoke_pg_as_cpp(F&& function)
    -> std::invoke_result_t<std::remove_reference_t<F>&> {
    using Function = std::remove_reference_t<F>;
    using Result = std::invoke_result_t<Function&>;
    static_assert(
        !std::is_reference_v<Result>,
        "invoke_pg_as_cpp leaf callbacks must return a value or void");

    if constexpr (std::is_void_v<Result>) {
        struct call_state {
            Function* function = nullptr;
        } state{.function = std::addressof(function)};

        invoke_pg_as_cpp_erased(
            std::addressof(state),
            +[](void* opaque) {
                auto* call = static_cast<call_state*>(opaque);
                std::invoke(*call->function);
            });
        return;
    } else {
        struct call_state {
            Function* function = nullptr;
            std::optional<Result> result;
        } state{.function = std::addressof(function)};

        invoke_pg_as_cpp_erased(
            std::addressof(state),
            +[](void* opaque) {
                auto* call = static_cast<call_state*>(opaque);
                call->result.emplace(std::invoke(*call->function));
            });
        return std::move(*state.result);
    }
}

/** Invoke one void PostgreSQL operation that may replace the transaction. */
template<class F>
void invoke_pg_across_transactions_as_cpp(F&& function) {
    using Function = std::remove_reference_t<F>;
    static_assert(
        std::is_void_v<std::invoke_result_t<Function&>>,
        "transaction-crossing PostgreSQL callbacks must return void");

    struct call_state {
        Function* function = nullptr;
    } state{.function = std::addressof(function)};

    invoke_pg_across_transactions_as_cpp_erased(
        std::addressof(state),
        +[](void* opaque) {
            auto* call = static_cast<call_state*>(opaque);
            std::invoke(*call->function);
        });
}

struct sql_error_fields {
    std::string message;
    std::optional<std::string> detail = std::nullopt;
    std::optional<std::string> hint = std::nullopt;
};

[[noreturn]] inline void raise_sql_error(
        int sqlstate,
        sql_error_fields fields) {
    const char* message = fields.message.c_str();
    const char* detail =
        fields.detail ? fields.detail->c_str() : nullptr;
    const char* hint =
        fields.hint ? fields.hint->c_str() : nullptr;

    invoke_pg_as_cpp([sqlstate, message, detail, hint] {
        if (detail != nullptr && hint != nullptr) {
            ereport(ERROR,
                (errcode(sqlstate),
                 errmsg_internal("%s", message),
                 errdetail_internal("%s", detail),
                 errhint("%s", hint)));
        } else if (detail != nullptr) {
            ereport(ERROR,
                (errcode(sqlstate),
                 errmsg_internal("%s", message),
                 errdetail_internal("%s", detail)));
        } else if (hint != nullptr) {
            ereport(ERROR,
                (errcode(sqlstate),
                 errmsg_internal("%s", message),
                 errhint("%s", hint)));
        } else {
            ereport(ERROR,
                (errcode(sqlstate),
                 errmsg_internal("%s", message)));
        }
    });
    std::terminate();
}

/**
 * Emit a PostgreSQL ERROR only after callers have released every C++ owner.
 *
 * Keeping ereport inside this protected PostgreSQL module prevents importers
 * from expanding PostgreSQL compiler-builtin macros after importing another
 * PostgreSQL-backed module.
 */
[[noreturn]] inline void emit_postgres_error_after_cleanup(
        int sqlstate,
        const char* message) noexcept {
    ereport(
        ERROR,
        (errcode(sqlstate),
         errmsg_internal(
             "%s",
             message == nullptr
                 ? "PostgreSQL error without a message"
                 : message)));
    pg_unreachable();
}

/**
 * Re-raise preserved PostgreSQL ErrorData after caller-owned cleanup.
 *
 * PostgreSQL retains the original SQLSTATE, including cancellation and
 * shutdown classes. A missing payload is itself an internal boundary error.
 */
[[noreturn]] inline void rethrow_postgres_error_after_cleanup(
        ErrorData* error) noexcept {
    if (error != nullptr) {
        ReThrowError(error);
    }
    emit_postgres_error_after_cleanup(
        ERRCODE_INTERNAL_ERROR,
        "bioseq could not preserve PostgreSQL error data");
}

}  // namespace bioseq::pg_utils

namespace bioseq::pg_utils {
namespace {

struct pg_cpp_invoke_state {
    void* callback_context = nullptr;
    PgCppInvokeBody body = nullptr;
    std::exception_ptr cpp_error;
    ErrorData* pg_error = nullptr;
    bool force_error_capture_failure = false;
    bool error_capture_failed = false;
};

/*
 * CopyErrorData can allocate and therefore can itself raise ERROR. Keep that
 * secondary ERROR behind a nested PostgreSQL exception frame so the erased
 * bridge always returns to C++ and its caller-owned objects can unwind.
 * MemoryContextSwitchTo and FlushErrorState are the non-raising state-reset
 * leaves reviewed by the compiler-fact bridge contract.
 */
void capture_current_pg_error_as_cpp(
        pg_cpp_invoke_state* state,
        MemoryContext error_owner,
        MemoryContext restore_context,
        ResourceOwner restore_resource_owner) {
    if (restore_resource_owner != nullptr) {
        CurrentResourceOwner = restore_resource_owner;
    }
    MemoryContextSwitchTo(error_owner);

    PG_TRY();
    {
        if (state->force_error_capture_failure) {
            ereport(
                ERROR,
                (errcode(ERRCODE_OUT_OF_MEMORY),
                 errmsg_internal(
                     "bioseq safety runtime injected a PostgreSQL "
                     "ERROR capture failure")));
        }
        state->pg_error = CopyErrorData();
        MemoryContextSwitchTo(restore_context);
        FlushErrorState();
    }
    PG_CATCH();
    {
        state->error_capture_failed = true;
        if (restore_resource_owner != nullptr) {
            CurrentResourceOwner = restore_resource_owner;
        }
        MemoryContextSwitchTo(restore_context);
        FlushErrorState();
    }
    PG_END_TRY();
}

}  // namespace

inline void invoke_pg_as_cpp_erased(
        void* callback_context,
        PgCppInvokeBody body) {
    auto* state = new pg_cpp_invoke_state{
        .callback_context = callback_context,
        .body = body,
    };
    MemoryContext saved_context = CurrentMemoryContext;
    ResourceOwner saved_resource_owner = CurrentResourceOwner;

    PG_TRY();
    {
        try {
            if (safety_runtime_probe::consume_fault(
                    "invoke_pg_as_cpp.error_capture")) {
                state->force_error_capture_failure = true;
                safety_runtime_probe::record_site_visit(
                    "invoke_pg_as_cpp.error_capture");
                ereport(
                    ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg_internal(
                         "bioseq safety runtime injected an "
                         "invoke_pg_as_cpp capture-path error")));
            }
            if (safety_runtime_probe::consume_fault(
                    "invoke_pg_as_cpp.vacuum_cleanup_restore")) {
                safety_runtime_probe::record_site_visit(
                    "invoke_pg_as_cpp.vacuum_cleanup_restore");
                ereport(
                    ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg_internal(
                         "bioseq safety runtime injected a "
                         "VACUUM bridge error")));
            }
            if (safety_runtime_probe::consume_fault(
                    "invoke_pg_as_cpp.cleanup_restore")) {
                safety_runtime_probe::record_site_visit(
                    "invoke_pg_as_cpp.cleanup_restore");
                ereport(
                    ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg_internal(
                         "bioseq safety runtime injected an "
                         "invoke_pg_as_cpp error")));
            }
            if (state->body != nullptr) {
                state->body(state->callback_context);
            }
        } catch (...) {
            state->cpp_error = std::current_exception();
        }
    }
    PG_CATCH();
    {
        MemoryContext error_owner = TopTransactionContext != nullptr
            ? TopTransactionContext
            : TopMemoryContext;
        capture_current_pg_error_as_cpp(
            state,
            error_owner,
            saved_context,
            saved_resource_owner);
    }
    PG_END_TRY();

    CurrentResourceOwner = saved_resource_owner;
    MemoryContextSwitchTo(saved_context);
    if (state->pg_error != nullptr
        || state->cpp_error != nullptr
        || state->error_capture_failed) {
        safety_runtime_probe::record_cleanup_complete(
            "resource_owner.restore");
        safety_runtime_probe::record_cleanup_complete(
            "memory_context.restore");
    }

    if (state->pg_error != nullptr) {
        ErrorData* error = std::exchange(state->pg_error, nullptr);
        delete state;
        throw PostgresErrorAsCpp{error};
    }
    if (state->error_capture_failed) {
        delete state;
        throw PostgresErrorCaptureFailure{};
    }
    if (state->cpp_error != nullptr) {
        std::exception_ptr error = std::move(state->cpp_error);
        delete state;
        std::rethrow_exception(error);
    }
    delete state;
}

inline void invoke_pg_across_transactions_as_cpp_erased(
        void* callback_context,
        PgCppInvokeBody body) {
    auto* state = new pg_cpp_invoke_state{
        .callback_context = callback_context,
        .body = body,
    };
    MemoryContext crossing_context =
        PortalContext != nullptr ? PortalContext : TopMemoryContext;
    MemoryContextSwitchTo(crossing_context);

    PG_TRY();
    {
        try {
            if (safety_runtime_probe::consume_fault(
                    "invoke_pg_across_transactions_as_cpp."
                    "error_capture")) {
                state->force_error_capture_failure = true;
                safety_runtime_probe::record_site_visit(
                    "invoke_pg_across_transactions_as_cpp."
                    "error_capture");
                ereport(
                    ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg_internal(
                         "bioseq safety runtime injected a "
                         "transaction-crossing capture-path error")));
            }
            if (state->body != nullptr) {
                state->body(state->callback_context);
            }
        } catch (...) {
            state->cpp_error = std::current_exception();
        }
    }
    PG_CATCH();
    {
        capture_current_pg_error_as_cpp(
            state,
            TopMemoryContext,
            crossing_context,
            CurrentResourceOwner);
    }
    PG_END_TRY();

    MemoryContextSwitchTo(crossing_context);
    if (state->pg_error != nullptr
        || state->cpp_error != nullptr
        || state->error_capture_failed) {
        safety_runtime_probe::record_cleanup_complete(
            "memory_context.restore");
    }

    if (state->pg_error != nullptr) {
        ErrorData* error = std::exchange(state->pg_error, nullptr);
        delete state;
        throw PostgresErrorAsCpp{error};
    }
    if (state->error_capture_failed) {
        delete state;
        throw PostgresErrorCaptureFailure{};
    }
    if (state->cpp_error != nullptr) {
        std::exception_ptr error = std::move(state->cpp_error);
        delete state;
        std::rethrow_exception(error);
    }
    delete state;
}

}  // namespace bioseq::pg_utils

namespace bioseq::pg_utils::detail {

template<class Error>
concept mutable_only_error_data_release =
    requires(Error& error) {
        { error.release_error_data() } -> std::same_as<ErrorData*>;
    }
    && !requires(const Error& error) { error.release_error_data(); };

static_assert(!std::is_copy_constructible_v<PostgresErrorAsCpp>);
static_assert(std::is_nothrow_move_constructible_v<PostgresErrorAsCpp>);
static_assert(!std::is_move_assignable_v<PostgresErrorAsCpp>);
static_assert(std::is_nothrow_destructible_v<PostgresErrorAsCpp>);
static_assert(
    std::is_nothrow_move_constructible_v<
        PostgresErrorCaptureFailure>);
static_assert(
    std::is_nothrow_destructible_v<PostgresErrorCaptureFailure>);
static_assert(noexcept(
    std::declval<PostgresErrorAsCpp&>().release_error_data()));
static_assert(mutable_only_error_data_release<PostgresErrorAsCpp>);

}  // namespace bioseq::pg_utils::detail
