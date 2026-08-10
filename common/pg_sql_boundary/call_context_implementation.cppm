/*-------------------------------------------------------------------------
 *
 * common/pg_sql_boundary/call_context_implementation.cppm
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

#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

module bioseq.pg_sql_boundary;

import :call_context;
import :error_boundary;

namespace bioseq::postgres::support {
namespace {

[[noreturn]] void raise_invalid_call_context(const char* message) {
    raise_sql_error(
        ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
        sql_error_fields{.message = message});
}

/** Stable state that survives PostgreSQL subtransaction error unwinding. */
struct subtransaction_error_state {
    MemoryContext outer_context = nullptr;
    ResourceOwner outer_owner = nullptr;
    int outer_nest_level = 0;
    ResourceOwner subtransaction_owner = nullptr;
    ErrorData* error = nullptr;
};

static_assert(
    std::is_trivially_destructible_v<subtransaction_error_state>);
static_assert(
    std::is_standard_layout_v<subtransaction_error_state>);

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

void restore_outer_subtransaction_state(
        const subtransaction_error_state* state) noexcept {
    MemoryContextSwitchTo(
        state->outer_context != nullptr
        ? state->outer_context
        : TopMemoryContext);
    CurrentResourceOwner = state->outer_owner;
}

void capture_subtransaction_error(
        subtransaction_error_state* state) {
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

[[noreturn]] void throw_subtransaction_error(
        std::unique_ptr<subtransaction_error_state> state) {
    ErrorData* error = std::exchange(state->error, nullptr);
    throw postgres_error{error};
}

void rollback_failed_subtransaction(
        subtransaction_error_state* state) noexcept {
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
}

}  // namespace

Oid function_call_context::collation_oid() const noexcept {
    return call_ == nullptr ? InvalidOid : call_->fncollation;
}

Oid function_call_context::argument_type_oid(int argument_index) const {
    if (call_ == nullptr
        || call_->flinfo == nullptr
        || argument_index < 0
        || argument_index >= call_->nargs) {
        raise_invalid_call_context(
            "SQL argument type requested outside the active call signature");
    }

    const Oid type_oid = invoke_pg_as_cpp(
        [this, argument_index] {
            return get_fn_expr_argtype(
                call_->flinfo,
                argument_index);
        });
    if (!OidIsValid(type_oid)) {
        raise_invalid_call_context(
            "SQL argument type is unavailable for this call");
    }
    return type_oid;
}

Oid function_call_context::result_type_oid() const {
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

ReturnSetInfo& function_call_context::set_result_info() const {
    if (call_ == nullptr
        || call_->resultinfo == nullptr
        || !IsA(call_->resultinfo, ReturnSetInfo)) {
        raise_invalid_call_context(
            "SQL call does not provide a set-returning result context");
    }
    return *reinterpret_cast<ReturnSetInfo*>(call_->resultinfo);
}

MemoryContext function_call_context::aggregate_memory_context() const {
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

WindowObject function_call_context::window_object() const {
    WindowObject result = call_ == nullptr
        ? nullptr
        : reinterpret_cast<WindowObject>(call_->context);
    if (!WindowObjectIsValid(result)) {
        raise_invalid_call_context(
            "SQL call is not running in a window context");
    }
    return result;
}

void*& function_call_context::function_extra() const noexcept {
    return call_->flinfo->fn_extra;
}

MemoryContext function_call_context::function_memory_context() const noexcept {
    return call_->flinfo->fn_mcxt;
}

void function_call_context::require_nonatomic_call() const {
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

void function_call_context::commit_transaction() {
    require_nonatomic_call();
    invoke_pg_across_transactions_as_cpp(
        [] {
            SPI_commit();
        });
}

void function_call_context::rollback_transaction() {
    require_nonatomic_call();
    invoke_pg_across_transactions_as_cpp(
        [] {
            SPI_rollback_and_chain();
        });
}

subtransaction subtransaction::begin() {
    /*
     * Only the stable pointer may span PG_TRY: PostgreSQL ERROR longjmps
     * across the frame, so release the C++ owner before entering and
     * reacquire it immediately after PG_END_TRY.
     */
    auto* state =
        std::make_unique<subtransaction_error_state>(
            subtransaction_error_state{
                .outer_context = CurrentMemoryContext,
                .outer_owner = CurrentResourceOwner,
                .outer_nest_level =
                    GetCurrentTransactionNestLevel(),
            })
            .release();

    PG_TRY();
    {
        BeginInternalSubTransaction(nullptr);
    }
    PG_CATCH();
    {
        capture_subtransaction_error(state);
    }
    PG_END_TRY();
    std::unique_ptr<subtransaction_error_state> state_owner{
        state};

    if (state->error != nullptr) {
        rollback_failed_subtransaction(state);
        throw_subtransaction_error(std::move(state_owner));
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
    return subtransaction{
        outer_context,
        outer_owner,
        outer_nest_level,
        subtransaction_owner};
}

void subtransaction::commit() {
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
        std::make_unique<subtransaction_error_state>(
            subtransaction_error_state{
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
    }
    PG_CATCH();
    {
        capture_subtransaction_error(state);
    }
    PG_END_TRY();
    std::unique_ptr<subtransaction_error_state> state_owner{
        state};

    if (state->error != nullptr) {
        rollback_failed_subtransaction(state);
        active_ = false;
        throw_subtransaction_error(std::move(state_owner));
    }

    restore_outer_subtransaction_state(state);
    active_ = false;
}

void subtransaction::rollback() {
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
        std::make_unique<subtransaction_error_state>(
            subtransaction_error_state{
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
        capture_subtransaction_error(state);
    }
    PG_END_TRY();
    std::unique_ptr<subtransaction_error_state> state_owner{
        state};

    if (state->error != nullptr) {
        rollback_failed_subtransaction(state);
        active_ = false;
        throw_subtransaction_error(std::move(state_owner));
    }

    restore_outer_subtransaction_state(state);
    active_ = false;
}

subtransaction::~subtransaction() noexcept {
    if (!active_) {
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
}

int spi_session::connect(int options) {
    if (active_) {
        throw std::logic_error{
            "SPI session is already active"};
    }

    const int status = invoke_pg_as_cpp(
        [options] {
            return SPI_connect_ext(options);
        });
    if (status == SPI_OK_CONNECT) {
        active_ = true;
    }
    return status;
}

int spi_session::finish() {
    if (!active_) {
        return SPI_ERROR_UNCONNECTED;
    }

    const int status = invoke_pg_as_cpp(
        [] {
            return SPI_finish();
        });
    if (status == SPI_OK_FINISH) {
        active_ = false;
    }
    return status;
}

spi_session::~spi_session() noexcept {
    if (!active_) {
        return;
    }

    MemoryContext saved_context = CurrentMemoryContext;
    ResourceOwner saved_owner = CurrentResourceOwner;
    volatile int status = SPI_ERROR_UNCONNECTED;

    PG_TRY();
    {
        status = SPI_finish();
    }
    PG_CATCH();
    {
        /*
         * SPI cleanup failed and may have left its private state partially
         * unwound. Do not install a potentially mismatched saved owner before
         * terminating the backend.
         */
        FlushErrorState();
        proc_exit(1);
    }
    PG_END_TRY();

    MemoryContextSwitchTo(saved_context);
    CurrentResourceOwner = saved_owner;
    if (status != SPI_OK_FINISH) {
        proc_exit(1);
    }
    active_ = false;
}

}  // namespace bioseq::postgres::support
