/**
 * @file common/postgres/support/error_boundary.cppm
 * @brief PostgreSQL ERROR and C++ exception translation boundaries.
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

export module bioseq.postgres.support:error_boundary;

import :safety_runtime_probe;

export namespace bioseq::postgres::support {

/**
 * Owns one PostgreSQL ErrorData payload during ordinary C++ unwinding.
 *
 * PostgreSQL leaf boundaries throw this object only after PG_END_TRY. An
 * outer C boundary may then release and rethrow the original ErrorData.
 */
class postgres_error final : public std::exception {
  public:
    /** Takes ownership of @p error. */
    explicit postgres_error(ErrorData* error) noexcept : error_(error) {}

    postgres_error(const postgres_error&) = delete;
    postgres_error& operator=(const postgres_error&) = delete;

    /** Transfers the ErrorData owner and disables duplicate instrumentation. */
    postgres_error(postgres_error&& other) noexcept
        : error_(std::exchange(other.error_, nullptr)),
          records_destructor_(std::exchange(other.records_destructor_, false)) {}

    postgres_error& operator=(postgres_error&&) = delete;

    /** Frees an unreleased ErrorData payload. */
    ~postgres_error() noexcept override {
        ErrorData* unreleased = std::exchange(error_, nullptr);
        if (unreleased != nullptr) {
            FreeErrorData(unreleased);
        }
        if (records_destructor_) {
            safety_runtime_probe::record_destructor("postgres_error");
        }
    }

    /** Returns PostgreSQL's message, or a stable fallback when absent. */
    [[nodiscard]] const char* what() const noexcept override {
        return error_ == nullptr || error_->message == nullptr
                   ? "PostgreSQL ERROR without a message"
                   : error_->message;
    }

    /** Returns PostgreSQL's packed SQLSTATE code, or zero when absent. */
    [[nodiscard]] int get_sql_state_code() const noexcept {
        return error_ == nullptr ? 0 : error_->sqlerrcode;
    }

    /** Releases the owned ErrorData payload to a PostgreSQL C boundary. */
    [[nodiscard]] ErrorData* release_error_data() noexcept {
        return std::exchange(error_, nullptr);
    }

  private:
    ErrorData* error_ = nullptr;
    bool records_destructor_ = true;
};

/**
 * Reports that a secondary PostgreSQL ERROR prevented ErrorData capture.
 */
class postgres_error_capture_failure final : public std::exception {
  public:
    /** Constructs the stable capture-failure sentinel. */
    postgres_error_capture_failure() noexcept = default;
    postgres_error_capture_failure(const postgres_error_capture_failure&) = delete;
    postgres_error_capture_failure& operator=(const postgres_error_capture_failure&) = delete;

    /** Transfers instrumentation ownership. */
    postgres_error_capture_failure(postgres_error_capture_failure&& other) noexcept
        : records_destructor_(std::exchange(other.records_destructor_, false)) {}

    postgres_error_capture_failure& operator=(postgres_error_capture_failure&&) = delete;

    /** Records ordinary C++ unwinding when instrumentation is active. */
    ~postgres_error_capture_failure() noexcept override {
        if (records_destructor_) {
            safety_runtime_probe::record_destructor("postgres_error_capture_failure");
        }
    }

    /** Returns a stable diagnostic message. */
    [[nodiscard]] const char* what() const noexcept override {
        return "could not preserve PostgreSQL ERROR data";
    }

  private:
    bool records_destructor_ = true;
};

/** Type-erased callback invoked inside a PostgreSQL exception frame. */
using postgres_callback_body = void (*)(void*);

} // namespace bioseq::postgres::support

namespace bioseq::postgres::support {
namespace {

struct postgres_invoke_state final {
    void* callback_context = nullptr;
    postgres_callback_body body = nullptr;
    std::exception_ptr cpp_exception;
    ErrorData* postgres_error_data = nullptr;
    bool force_error_capture_failure = false;
    bool error_capture_failed = false;
};

static_assert(std::is_nothrow_destructible_v<postgres_invoke_state>);

/**
 * Copies the active PostgreSQL error behind a nested exception frame.
 * CopyErrorData may allocate and raise a secondary ERROR; this helper always
 * restores PostgreSQL's context and resource-owner state before returning.
 */
void capture_current_postgres_error_as_cpp(postgres_invoke_state* state,
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
            ereport(ERROR,
                    (errcode(ERRCODE_OUT_OF_MEMORY),
                     errmsg_internal("bioseq safety runtime injected a PostgreSQL "
                                     "ERROR capture failure")));
        }
        state->postgres_error_data = CopyErrorData();
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

} // namespace
} // namespace bioseq::postgres::support

export namespace bioseq::postgres::support {

/**
 * Invokes a type-erased PostgreSQL leaf and converts ERROR into C++ state.
 * This function is intentionally non-inlined across the setjmp boundary.
 */
#if defined(__GNUC__) && !defined(__clang__)
[[gnu::noinline, gnu::noipa]]
#else
[[gnu::noinline]]
#endif
inline void invoke_pg_as_cpp_erased(void* callback_context, postgres_callback_body body) {
    auto* state = std::make_unique<postgres_invoke_state>(postgres_invoke_state{
                                                              .callback_context = callback_context,
                                                              .body = body,
                                                          })
                      .release();
    MemoryContext saved_context = CurrentMemoryContext;
    ResourceOwner saved_resource_owner = CurrentResourceOwner;

    PG_TRY();
    {
        try {
            if (safety_runtime_probe::consume_fault("invoke_pg_as_cpp.error_capture")) {
                state->force_error_capture_failure = true;
                safety_runtime_probe::record_site_visit("invoke_pg_as_cpp.error_capture");
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg_internal("bioseq safety runtime injected an "
                                         "invoke_pg_as_cpp capture-path error")));
            }
            if (safety_runtime_probe::consume_fault("invoke_pg_as_cpp.vacuum_cleanup_restore")) {
                safety_runtime_probe::record_site_visit("invoke_pg_as_cpp.vacuum_cleanup_restore");
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg_internal("bioseq safety runtime injected a "
                                         "VACUUM bridge error")));
            }
            if (safety_runtime_probe::consume_fault("invoke_pg_as_cpp.cleanup_restore")) {
                safety_runtime_probe::record_site_visit("invoke_pg_as_cpp.cleanup_restore");
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg_internal("bioseq safety runtime injected an "
                                         "invoke_pg_as_cpp error")));
            }
            if (state->body != nullptr) {
                state->body(state->callback_context);
            }
        } catch (...) {
            state->cpp_exception = std::current_exception();
        }
    }
    PG_CATCH();
    {
        MemoryContext error_owner =
            TopTransactionContext != nullptr ? TopTransactionContext : TopMemoryContext;
        capture_current_postgres_error_as_cpp(
            state, error_owner, saved_context, saved_resource_owner);
    }
    PG_END_TRY();
    std::unique_ptr<postgres_invoke_state> state_owner{state};

    CurrentResourceOwner = saved_resource_owner;
    MemoryContextSwitchTo(saved_context);
    if (state->postgres_error_data != nullptr || state->cpp_exception != nullptr ||
        state->error_capture_failed) {
        safety_runtime_probe::record_cleanup_complete("resource_owner.restore");
        safety_runtime_probe::record_cleanup_complete("memory_context.restore");
    }

    if (state->postgres_error_data != nullptr) {
        ErrorData* error = std::exchange(state->postgres_error_data, nullptr);
        throw postgres_error{error};
    }
    if (state->error_capture_failed) {
        throw postgres_error_capture_failure{};
    }
    if (state->cpp_exception != nullptr) {
        std::exception_ptr error = std::move(state->cpp_exception);
        std::rethrow_exception(error);
    }
}

/**
 * Invokes a transaction-replacing PostgreSQL leaf through a stable context.
 * This function is intentionally non-inlined across the setjmp boundary.
 */
#if defined(__GNUC__) && !defined(__clang__)
[[gnu::noinline, gnu::noipa]]
#else
[[gnu::noinline]]
#endif
inline void invoke_pg_across_transactions_as_cpp_erased(void* callback_context,
                                                        postgres_callback_body body) {
    auto* state = std::make_unique<postgres_invoke_state>(postgres_invoke_state{
                                                              .callback_context = callback_context,
                                                              .body = body,
                                                          })
                      .release();
    MemoryContext crossing_context = PortalContext != nullptr ? PortalContext : TopMemoryContext;
    MemoryContextSwitchTo(crossing_context);

    PG_TRY();
    {
        try {
            if (safety_runtime_probe::consume_fault("invoke_pg_across_transactions_as_cpp."
                                                    "error_capture")) {
                state->force_error_capture_failure = true;
                safety_runtime_probe::record_site_visit("invoke_pg_across_transactions_as_cpp."
                                                        "error_capture");
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg_internal("bioseq safety runtime injected a "
                                         "transaction-crossing capture-path error")));
            }
            if (state->body != nullptr) {
                state->body(state->callback_context);
            }
        } catch (...) {
            state->cpp_exception = std::current_exception();
        }
    }
    PG_CATCH();
    {
        capture_current_postgres_error_as_cpp(
            state, TopMemoryContext, crossing_context, CurrentResourceOwner);
    }
    PG_END_TRY();
    std::unique_ptr<postgres_invoke_state> state_owner{state};

    MemoryContextSwitchTo(crossing_context);
    if (state->postgres_error_data != nullptr || state->cpp_exception != nullptr ||
        state->error_capture_failed) {
        safety_runtime_probe::record_cleanup_complete("memory_context.restore");
    }

    if (state->postgres_error_data != nullptr) {
        ErrorData* error = std::exchange(state->postgres_error_data, nullptr);
        throw postgres_error{error};
    }
    if (state->error_capture_failed) {
        throw postgres_error_capture_failure{};
    }
    if (state->cpp_exception != nullptr) {
        std::exception_ptr error = std::move(state->cpp_exception);
        std::rethrow_exception(error);
    }
}

/** Invokes one PostgreSQL leaf and returns or throws through the C++ ABI. */
template <class p_function>
auto invoke_pg_as_cpp(p_function&& function)
    -> std::invoke_result_t<std::remove_reference_t<p_function>&> {
    using function_type = std::remove_reference_t<p_function>;
    using result_type = std::invoke_result_t<function_type&>;
    static_assert(!std::is_reference_v<result_type>,
                  "invoke_pg_as_cpp callbacks must return a value or void");

    if constexpr (std::is_void_v<result_type>) {
        struct call_state final {
            function_type* function = nullptr;
        } state{.function = std::addressof(function)};

        invoke_pg_as_cpp_erased(
            std::addressof(state), +[](void* opaque) {
                auto* call = static_cast<call_state*>(opaque);
                std::invoke(*call->function);
            });
        return;
    } else {
        struct call_state final {
            function_type* function = nullptr;
            std::optional<result_type> result;
        } state{.function = std::addressof(function)};

        invoke_pg_as_cpp_erased(
            std::addressof(state), +[](void* opaque) {
                auto* call = static_cast<call_state*>(opaque);
                call->result.emplace(std::invoke(*call->function));
            });
        return std::move(*state.result);
    }
}

/** Invokes one void PostgreSQL operation that may replace the transaction. */
template <class p_function> void invoke_pg_across_transactions_as_cpp(p_function&& function) {
    using function_type = std::remove_reference_t<p_function>;
    static_assert(std::is_void_v<std::invoke_result_t<function_type&>>,
                  "transaction-crossing PostgreSQL callbacks must return void");

    struct call_state final {
        function_type* function = nullptr;
    } state{.function = std::addressof(function)};

    invoke_pg_across_transactions_as_cpp_erased(
        std::addressof(state), +[](void* opaque) {
            auto* call = static_cast<call_state*>(opaque);
            std::invoke(*call->function);
        });
}

/** Owned strings used to raise one PostgreSQL SQL error. */
struct sql_error_fields final {
    std::string message;
    std::optional<std::string> detail = std::nullopt;
    std::optional<std::string> hint = std::nullopt;
};

/** Raises a PostgreSQL ERROR from owned strings inside the protected bridge. */
[[noreturn]] inline void raise_sql_error(int sql_state, sql_error_fields fields) {
    const char* message = fields.message.c_str();
    const char* detail = fields.detail ? fields.detail->c_str() : nullptr;
    const char* hint = fields.hint ? fields.hint->c_str() : nullptr;

    invoke_pg_as_cpp([sql_state, message, detail, hint] {
        if (detail != nullptr && hint != nullptr) {
            ereport(ERROR,
                    (errcode(sql_state),
                     errmsg_internal("%s", message),
                     errdetail_internal("%s", detail),
                     errhint("%s", hint)));
        } else if (detail != nullptr) {
            ereport(ERROR,
                    (errcode(sql_state),
                     errmsg_internal("%s", message),
                     errdetail_internal("%s", detail)));
        } else if (hint != nullptr) {
            ereport(ERROR,
                    (errcode(sql_state), errmsg_internal("%s", message), errhint("%s", hint)));
        } else {
            ereport(ERROR, (errcode(sql_state), errmsg_internal("%s", message)));
        }
    });
    std::terminate();
}

/** Emits a PostgreSQL ERROR after callers release every C++ owner. */
[[noreturn]] inline void emit_postgres_error_after_cleanup(int sql_state,
                                                           const char* message) noexcept {
    ereport(ERROR,
            (errcode(sql_state),
             errmsg_internal("%s",
                             message == nullptr ? "PostgreSQL error without a message" : message)));
    pg_unreachable();
}

/** Re-raises preserved ErrorData after caller-owned cleanup is complete. */
[[noreturn]] inline void rethrow_postgres_error_after_cleanup(ErrorData* error) noexcept {
    if (error != nullptr) {
        ReThrowError(error);
    }
    emit_postgres_error_after_cleanup(ERRCODE_INTERNAL_ERROR,
                                      "bioseq could not preserve PostgreSQL error data");
}

} // namespace bioseq::postgres::support

namespace bioseq::postgres::support::detail {

template <class p_error>
concept releases_mutable_error_data_only = requires(p_error& error) {
    { error.release_error_data() } -> std::same_as<ErrorData*>;
} && !requires(const p_error& error) { error.release_error_data(); };

static_assert(!std::is_copy_constructible_v<postgres_error>);
static_assert(std::is_nothrow_move_constructible_v<postgres_error>);
static_assert(!std::is_move_assignable_v<postgres_error>);
static_assert(std::is_nothrow_destructible_v<postgres_error>);
static_assert(std::is_nothrow_move_constructible_v<postgres_error_capture_failure>);
static_assert(std::is_nothrow_destructible_v<postgres_error_capture_failure>);
static_assert(noexcept(std::declval<postgres_error&>().release_error_data()));
static_assert(releases_mutable_error_data_only<postgres_error>);

} // namespace bioseq::postgres::support::detail
