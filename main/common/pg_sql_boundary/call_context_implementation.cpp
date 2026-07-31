/*-------------------------------------------------------------------------
 *
 * common/pg_sql_boundary/call_context_implementation.cpp
 *
 * Checked PostgreSQL call-context views and transaction/SPI owners used by
 * generated SQL boundaries.
 *
 *-------------------------------------------------------------------------
 */
module;

extern "C" {
#include "postgres.h"
#include "access/xact.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"
#include "nodes/parsenodes.h"
#include "storage/ipc.h"
#include "utils/elog.h"
#include "utils/errcodes.h"
#include "utils/memutils.h"
#include "utils/palloc.h"
#include "utils/resowner.h"
#include "windowapi.h"
}

#include <concepts>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

module bioseq.pg_sql_boundary;

import :call_context;
import :error_boundary;

namespace bioseq::pg_utils {
namespace {

[[noreturn]] void raise_invalid_call_context(const char* message) {
    raise_sql_error(
        ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
        sql_error_fields{.message = message});
}

struct specialized_pg_error_state {
    MemoryContext outer_context = nullptr;
    ResourceOwner outer_owner = nullptr;
    int outer_nest_level = 0;
    ResourceOwner subtransaction_owner = nullptr;
    ErrorData* error = nullptr;
};

static_assert(
    std::is_trivially_destructible_v<specialized_pg_error_state>);
static_assert(
    std::is_standard_layout_v<specialized_pg_error_state>);

[[nodiscard]] constexpr bool subtransaction_requires_cleanup(
        int current_nest_level,
        int outer_nest_level) noexcept {
    return current_nest_level > outer_nest_level;
}

[[nodiscard]] constexpr bool subtransaction_is_current(
        int current_nest_level,
        int outer_nest_level) noexcept {
    return current_nest_level == outer_nest_level + 1;
}

[[nodiscard]] constexpr bool subtransaction_transition_is_valid(
        int current_nest_level,
        int outer_nest_level) noexcept {
    return current_nest_level == outer_nest_level
        || subtransaction_is_current(
            current_nest_level, outer_nest_level);
}

static_assert(!subtransaction_requires_cleanup(1, 1));
static_assert(subtransaction_requires_cleanup(2, 1));
static_assert(subtransaction_is_current(2, 1));
static_assert(!subtransaction_is_current(3, 1));
static_assert(subtransaction_transition_is_valid(1, 1));
static_assert(subtransaction_transition_is_valid(2, 1));
static_assert(!subtransaction_transition_is_valid(3, 1));

void restore_outer_subtransaction_state(
        const specialized_pg_error_state* state) noexcept {
    MemoryContextSwitchTo(
        state->outer_context != nullptr
        ? state->outer_context
        : TopMemoryContext);
    CurrentResourceOwner = state->outer_owner;
}

void capture_specialized_pg_error(
        specialized_pg_error_state* state) {
    const int current_nest_level =
        GetCurrentTransactionNestLevel();
    if (!subtransaction_transition_is_valid(
            current_nest_level,
            state->outer_nest_level)) {
        proc_exit(1);
    }
    const MemoryContext error_owner =
        state->outer_context != nullptr
        ? state->outer_context
        : TopMemoryContext;

    /*
     * Copy into memory that survives subtransaction rollback, but do not
     * change CurrentResourceOwner after the failed terminal operation.
     * PostgreSQL may already have advanced owner state before raising ERROR;
     * recovery therefore changes only the memory context and leaves the
     * error-time owner untouched until terminal cleanup completes.
     */
    MemoryContextSwitchTo(error_owner);
    state->error = CopyErrorData();
    FlushErrorState();
}

[[noreturn]] void throw_specialized_pg_error(
        std::unique_ptr<specialized_pg_error_state> state) {
    ErrorData* error = std::exchange(state->error, nullptr);
    throw PostgresErrorAsCpp{error};
}

void rollback_failed_subtransaction(
        specialized_pg_error_state* state) noexcept {
    const int current_nest_level =
        GetCurrentTransactionNestLevel();
    if (!subtransaction_transition_is_valid(
            current_nest_level,
            state->outer_nest_level)) {
        proc_exit(1);
    }
    if (!subtransaction_requires_cleanup(
            current_nest_level, state->outer_nest_level)) {
        restore_outer_subtransaction_state(state);
        return;
    }

    MemoryContextSwitchTo(
        state->outer_context != nullptr
        ? state->outer_context
        : TopMemoryContext);
    PG_TRY();
    {
        RollbackAndReleaseCurrentSubTransaction();
    }
    PG_CATCH();
    {
        /*
         * The inner transaction could not be terminated. Restoring the outer
         * owner here would create a mismatched transaction/resource-owner
         * state, so terminate without attempting to continue.
         */
        FlushErrorState();
        proc_exit(1);
    }
    PG_END_TRY();

    restore_outer_subtransaction_state(state);
    safety_runtime_probe::record_cleanup_complete(
        "sql_subtransaction.rollback");
}

[[nodiscard]] int invoke_spi_connect_ext_with_probe(
        bool* active,
        int options) {
    const int status = SPI_connect_ext(options);
    if (status == SPI_OK_CONNECT) {
        *active = true;
        safety_runtime_probe::record_cleanup_complete(
            "spi.connect");
        if (safety_runtime_probe::consume_fault(
                "scoped_spi_session.connect")) {
            safety_runtime_probe::record_site_visit(
                "scoped_spi_session.connect");
            ereport(
                ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg_internal(
                     "bioseq safety runtime injected an SPI "
                     "connect error")));
        }
    }
    return status;
}

[[nodiscard]] int invoke_spi_finish_with_probe(bool* active) {
    const int status = SPI_finish();
    if (status == SPI_OK_FINISH) {
        *active = false;
        safety_runtime_probe::record_cleanup_complete(
            "spi.finish");
        if (safety_runtime_probe::consume_fault(
                "scoped_spi_session.finish")) {
            safety_runtime_probe::record_site_visit(
                "scoped_spi_session.finish");
            ereport(
                ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg_internal(
                     "bioseq safety runtime injected an SPI "
                     "finish error")));
        }
    }
    return status;
}

}  // namespace

Oid sql_call_context::collation() const noexcept {
    return call_ == nullptr ? InvalidOid : call_->fncollation;
}

Oid sql_call_context::argument_type(int zero_based_index) const {
    if (call_ == nullptr
        || call_->flinfo == nullptr
        || zero_based_index < 0
        || zero_based_index >= call_->nargs) {
        raise_invalid_call_context(
            "SQL argument type requested outside the active call signature");
    }

    const Oid type_oid = invoke_pg_as_cpp(
        [this, zero_based_index] {
            return get_fn_expr_argtype(
                call_->flinfo,
                zero_based_index);
        });
    if (!OidIsValid(type_oid)) {
        raise_invalid_call_context(
            "SQL argument type is unavailable for this call");
    }
    return type_oid;
}

Oid sql_call_context::result_type() const {
    if (call_ == nullptr || call_->flinfo == nullptr) {
        raise_invalid_call_context(
            "SQL result type requested outside an active call");
    }

    const Oid type_oid = invoke_pg_as_cpp(
        [this] {
            return get_fn_expr_rettype(call_->flinfo);
        });
    if (!OidIsValid(type_oid)) {
        raise_invalid_call_context(
            "SQL result type is unavailable for this call");
    }
    return type_oid;
}

ReturnSetInfo& sql_call_context::return_set_info() const {
    if (call_ == nullptr
        || call_->resultinfo == nullptr
        || !IsA(call_->resultinfo, ReturnSetInfo)) {
        raise_invalid_call_context(
            "SQL call does not provide a set-returning result context");
    }
    return *reinterpret_cast<ReturnSetInfo*>(call_->resultinfo);
}

MemoryContext sql_call_context::aggregate_memory_context() const {
    if (call_ == nullptr) {
        raise_invalid_call_context(
            "aggregate memory context requested outside an active call");
    }

    MemoryContext aggregate_context = nullptr;
    const int context_kind = invoke_pg_as_cpp(
        [this, &aggregate_context] {
            return AggCheckCallContext(
                call_,
                &aggregate_context);
        });
    if (context_kind == 0 || aggregate_context == nullptr) {
        raise_invalid_call_context(
            "SQL call is not running in an aggregate context");
    }
    return aggregate_context;
}

WindowObject sql_call_context::window_object() const {
    WindowObject result = call_ == nullptr
        ? nullptr
        : reinterpret_cast<WindowObject>(call_->context);
    if (!WindowObjectIsValid(result)) {
        raise_invalid_call_context(
            "SQL call is not running in a window context");
    }
    return result;
}

void*& sql_call_context::fn_extra() const noexcept {
    return call_->flinfo->fn_extra;
}

MemoryContext sql_call_context::fn_mcxt() const noexcept {
    return call_->flinfo->fn_mcxt;
}

void sql_call_context::require_nonatomic_procedure_call() const {
    if (call_ == nullptr
        || call_->context == nullptr
        || !IsA(call_->context, CallContext)
        || reinterpret_cast<CallContext*>(call_->context)->atomic) {
        raise_sql_error(
            ERRCODE_INVALID_TRANSACTION_TERMINATION,
            sql_error_fields{
                .message =
                    "transaction control requires a non-atomic procedure call",
            });
    }
}

void sql_call_context::commit_transaction() {
    require_nonatomic_procedure_call();
    invoke_pg_across_transactions_as_cpp(
        [] {
            SPI_commit();
            safety_runtime_probe::record_cleanup_complete(
                "sql_call_context.commit");
            if (safety_runtime_probe::consume_fault(
                    "sql_call_context.commit")) {
                safety_runtime_probe::record_site_visit(
                    "sql_call_context.commit");
                ereport(
                    ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg_internal(
                         "bioseq safety runtime injected a "
                         "transaction commit error")));
            }
        });
}

void sql_call_context::rollback_transaction() {
    require_nonatomic_procedure_call();
    invoke_pg_across_transactions_as_cpp(
        [] {
            SPI_rollback_and_chain();
        });
}

sql_subtransaction sql_subtransaction::begin() {
    /*
     * Only the stable pointer may span PG_TRY: PostgreSQL ERROR longjmps
     * across the frame, so release the C++ owner before entering and
     * reacquire it immediately after PG_END_TRY.
     */
    auto* state =
        std::make_unique<specialized_pg_error_state>(
            specialized_pg_error_state{
                .outer_context = CurrentMemoryContext,
                .outer_owner = CurrentResourceOwner,
                .outer_nest_level =
                    GetCurrentTransactionNestLevel(),
            })
            .release();

    PG_TRY();
    {
        BeginInternalSubTransaction(nullptr);
        safety_runtime_probe::record_cleanup_complete(
            "sql_subtransaction.begin");
        if (safety_runtime_probe::consume_fault(
                "sql_subtransaction.begin_abort")) {
            safety_runtime_probe::record_site_visit(
                "sql_subtransaction.begin_abort");
            ereport(
                ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg_internal(
                     "bioseq safety runtime injected a "
                     "subtransaction begin error")));
        }
    }
    PG_CATCH();
    {
        capture_specialized_pg_error(state);
    }
    PG_END_TRY();
    std::unique_ptr<specialized_pg_error_state> state_owner{
        state};

    if (state->error != nullptr) {
        rollback_failed_subtransaction(state);
        throw_specialized_pg_error(std::move(state_owner));
    }
    if (!subtransaction_is_current(
            GetCurrentTransactionNestLevel(),
            state->outer_nest_level)) {
        proc_exit(1);
    }

    const MemoryContext outer_context = state->outer_context;
    const ResourceOwner outer_owner = state->outer_owner;
    const int outer_nest_level = state->outer_nest_level;
    const ResourceOwner subtransaction_owner =
        CurrentResourceOwner;
    MemoryContextSwitchTo(
        outer_context != nullptr
        ? outer_context
        : TopMemoryContext);
    return sql_subtransaction{
        outer_context,
        outer_owner,
        outer_nest_level,
        subtransaction_owner};
}

void sql_subtransaction::commit() {
    if (!active_) {
        throw std::logic_error{
            "SQL subtransaction is already complete"};
    }
    if (!subtransaction_is_current(
            GetCurrentTransactionNestLevel(),
            outer_nest_level_)) {
        throw std::logic_error{
            "SQL subtransaction is not the current subtransaction"};
    }

    auto* state =
        std::make_unique<specialized_pg_error_state>(
            specialized_pg_error_state{
                .outer_context = outer_context_,
                .outer_owner = outer_owner_,
                .outer_nest_level = outer_nest_level_,
                .subtransaction_owner =
                    subtransaction_owner_,
            })
            .release();

    MemoryContextSwitchTo(
        state->outer_context != nullptr
        ? state->outer_context
        : TopMemoryContext);
    CurrentResourceOwner = state->subtransaction_owner;
    PG_TRY();
    {
        ReleaseCurrentSubTransaction();
        safety_runtime_probe::record_cleanup_complete(
            "sql_subtransaction.commit");
        if (safety_runtime_probe::consume_fault(
                "sql_subtransaction.commit")) {
            safety_runtime_probe::record_site_visit(
                "sql_subtransaction.commit");
            ereport(
                ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg_internal(
                     "bioseq safety runtime injected a "
                     "subtransaction commit error")));
        }
    }
    PG_CATCH();
    {
        capture_specialized_pg_error(state);
    }
    PG_END_TRY();
    std::unique_ptr<specialized_pg_error_state> state_owner{
        state};

    if (state->error != nullptr) {
        rollback_failed_subtransaction(state);
        active_ = false;
        throw_specialized_pg_error(std::move(state_owner));
    }

    restore_outer_subtransaction_state(state);
    active_ = false;
}

void sql_subtransaction::rollback() {
    if (!active_) {
        throw std::logic_error{
            "SQL subtransaction is already complete"};
    }
    if (!subtransaction_is_current(
            GetCurrentTransactionNestLevel(),
            outer_nest_level_)) {
        throw std::logic_error{
            "SQL subtransaction is not the current subtransaction"};
    }

    auto* state =
        std::make_unique<specialized_pg_error_state>(
            specialized_pg_error_state{
                .outer_context = outer_context_,
                .outer_owner = outer_owner_,
                .outer_nest_level = outer_nest_level_,
                .subtransaction_owner =
                    subtransaction_owner_,
            })
            .release();

    MemoryContextSwitchTo(
        state->outer_context != nullptr
        ? state->outer_context
        : TopMemoryContext);
    CurrentResourceOwner = state->subtransaction_owner;
    PG_TRY();
    {
        RollbackAndReleaseCurrentSubTransaction();
    }
    PG_CATCH();
    {
        capture_specialized_pg_error(state);
    }
    PG_END_TRY();
    std::unique_ptr<specialized_pg_error_state> state_owner{
        state};

    if (state->error != nullptr) {
        rollback_failed_subtransaction(state);
        active_ = false;
        throw_specialized_pg_error(std::move(state_owner));
    }

    restore_outer_subtransaction_state(state);
    active_ = false;
    safety_runtime_probe::record_cleanup_complete(
        "sql_subtransaction.rollback");
}

sql_subtransaction::~sql_subtransaction() noexcept {
    if (!active_) {
        safety_runtime_probe::record_destructor(
            "sql_subtransaction");
        return;
    }
    if (!subtransaction_is_current(
            GetCurrentTransactionNestLevel(),
            outer_nest_level_)) {
        proc_exit(1);
    }

    MemoryContextSwitchTo(
        outer_context_ != nullptr
        ? outer_context_
        : TopMemoryContext);
    CurrentResourceOwner = subtransaction_owner_;
    PG_TRY();
    {
        RollbackAndReleaseCurrentSubTransaction();
    }
    PG_CATCH();
    {
        /*
         * Cleanup may have stopped before or after changing transaction
         * ownership. Never force the saved outer owner onto that partially
         * transitioned state from a noexcept destructor.
         */
        FlushErrorState();
        proc_exit(1);
    }
    PG_END_TRY();

    MemoryContextSwitchTo(
        outer_context_ != nullptr
        ? outer_context_
        : TopMemoryContext);
    CurrentResourceOwner = outer_owner_;
    active_ = false;
    safety_runtime_probe::record_cleanup_complete(
        "sql_subtransaction.rollback");
    safety_runtime_probe::record_destructor(
        "sql_subtransaction");
}

int scoped_spi_session::connect(int options) {
    if (active_) {
        throw std::logic_error{
            "SPI session is already active"};
    }

    const int status = invoke_pg_as_cpp(
        [this, options] {
            return invoke_spi_connect_ext_with_probe(
                &active_,
                options);
        });
    if (status == SPI_OK_CONNECT) {
        active_ = true;
    }
    return status;
}

int scoped_spi_session::finish() {
    if (!active_) {
        return SPI_ERROR_UNCONNECTED;
    }

    const int status = invoke_pg_as_cpp(
        [this] {
            return invoke_spi_finish_with_probe(&active_);
        });
    if (status == SPI_OK_FINISH) {
        active_ = false;
    }
    return status;
}

scoped_spi_session::~scoped_spi_session() noexcept {
    if (!active_) {
        safety_runtime_probe::record_destructor(
            "scoped_spi_session");
        return;
    }

    MemoryContext saved_context = CurrentMemoryContext;
    ResourceOwner saved_owner = CurrentResourceOwner;
    volatile int status = SPI_ERROR_UNCONNECTED;

    PG_TRY();
    {
#if defined(BIOSEQ_SAFETY_RUNTIME_PROBES)
        if (safety_runtime_probe::consume_fault(
                "scoped_spi_session.emergency_finish_error")) {
            safety_runtime_probe::record_site_visit(
                "scoped_spi_session.emergency_finish_error");
            ereport(
                ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg_internal(
                     "bioseq safety runtime injected an SPI "
                     "emergency-finish error")));
        }
#endif
        status = SPI_finish();
#if defined(BIOSEQ_SAFETY_RUNTIME_PROBES)
        if (safety_runtime_probe::consume_fault(
                "scoped_spi_session.emergency_finish_non_ok")) {
            safety_runtime_probe::record_site_visit(
                "scoped_spi_session.emergency_finish_non_ok");
            status = SPI_ERROR_UNCONNECTED;
        }
#endif
    }
    PG_CATCH();
    {
        /*
         * SPI cleanup failed and may have left its private state partially
         * unwound. Do not install a potentially mismatched saved owner before
         * terminating the backend.
         */
        FlushErrorState();
        safety_runtime_probe::record_cleanup_complete(
            "scoped_spi_session.terminal_guard.finish_error");
        proc_exit(1);
    }
    PG_END_TRY();

    MemoryContextSwitchTo(saved_context);
    CurrentResourceOwner = saved_owner;
    if (status != SPI_OK_FINISH) {
        safety_runtime_probe::record_cleanup_complete(
            "scoped_spi_session.terminal_guard.finish_non_ok");
        proc_exit(1);
    }
    active_ = false;
    safety_runtime_probe::record_cleanup_complete(
        "spi.finish");
    safety_runtime_probe::record_destructor(
        "scoped_spi_session");
}

}  // namespace bioseq::pg_utils

namespace bioseq::pg_utils::detail {

template<class Context>
concept publicly_constructible_call_context =
    requires(FunctionCallInfo call) {
        Context{call};
    };

using context_factory_signature =
    sql_call_context (*)(FunctionCallInfo) noexcept;

[[maybe_unused]] inline constexpr context_factory_signature
    context_factory_probe = &sql_boundary_access::make_context;

static_assert(!publicly_constructible_call_context<sql_call_context>);
static_assert(!std::is_copy_constructible_v<sql_call_context>);
static_assert(!std::is_copy_assignable_v<sql_call_context>);
static_assert(!std::is_move_constructible_v<sql_call_context>);
static_assert(!std::is_move_assignable_v<sql_call_context>);
static_assert(std::is_empty_v<table_am_handler_context>);
static_assert(std::is_empty_v<subscript_handler_context>);
static_assert(!std::is_copy_constructible_v<sql_subtransaction>);
static_assert(!std::is_copy_assignable_v<sql_subtransaction>);
static_assert(!std::is_move_constructible_v<sql_subtransaction>);
static_assert(!std::is_move_assignable_v<sql_subtransaction>);
static_assert(std::is_nothrow_destructible_v<sql_subtransaction>);
static_assert(!std::is_copy_constructible_v<scoped_spi_session>);
static_assert(!std::is_copy_assignable_v<scoped_spi_session>);
static_assert(!std::is_move_constructible_v<scoped_spi_session>);
static_assert(!std::is_move_assignable_v<scoped_spi_session>);
static_assert(std::is_nothrow_destructible_v<scoped_spi_session>);
static_assert(noexcept(
    std::declval<const scoped_spi_session&>().active()));
static_assert(noexcept(
    std::declval<const sql_call_context&>().function_oid()));
static_assert(context_factory_probe != nullptr);

}  // namespace bioseq::pg_utils::detail
