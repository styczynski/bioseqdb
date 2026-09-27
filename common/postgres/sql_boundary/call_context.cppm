/**
 * @file common/postgres/sql_boundary/call_context.cppm
 * @brief Access to the current SQL call, with helpers that own subtransactions and SPI sessions.
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

export module bioseq.postgres.sql_boundary:call_context;

import :error_boundary;

export namespace bioseq::postgres::support {

/**
 * Borrow the current PostgreSQL function call.
 * The call frame must outlive this view. Accessors check the context where
 * needed, while function_extra and function_memory_context require a valid
 * call frame with flinfo.
 */
class function_call_context final {
public:
    /** Borrow the supplied call frame. Construction does not validate it. */
    [[nodiscard]] static function_call_context from_call(
            FunctionCallInfo call) noexcept {
        return function_call_context{call};
    }

    /** Return the invoked function OID, or InvalidOid without a call frame. */
    [[nodiscard]] Oid function_oid() const noexcept {
        return call_ == nullptr || call_->flinfo == nullptr
            ? InvalidOid
            : call_->flinfo->fn_oid;
    }
    /** Return the call collation OID, or InvalidOid without a call frame. */
    [[nodiscard]] Oid collation_oid() const noexcept;
    /** Get an argument's declared SQL type, rejecting an invalid index or missing type. */
    [[nodiscard]] Oid argument_type_oid(int argument_index) const;
    /** Get the declared SQL result type, rejecting a missing call or type. */
    [[nodiscard]] Oid result_type_oid() const;
    /** Get the set-returning result context, rejecting other kinds of call. */
    [[nodiscard]] ReturnSetInfo& set_result_info() const;
    /** Get the aggregate memory context, rejecting calls outside an aggregate. */
    [[nodiscard]] MemoryContext aggregate_memory_context() const;
    /** Get the window object, rejecting calls outside a window function. */
    [[nodiscard]] WindowObject window_object() const;
    /** Access fn_extra, where the function can keep data between calls. */
    [[nodiscard]] void*& function_extra() const noexcept;
    /** Get fn_mcxt, the memory context for data kept between function calls. */
    [[nodiscard]] MemoryContext function_memory_context() const noexcept;
    /** Reject a procedure call that is not allowed to control transactions. */
    void require_nonatomic_call() const;
    /** Commit the transaction after checking that this procedure may do so. */
    void commit_transaction();
    /** Roll back and begin a new transaction with the same transaction characteristics. */
    void rollback_transaction();

    function_call_context(const function_call_context&) = delete;
    function_call_context& operator=(const function_call_context&) = delete;

private:
    explicit function_call_context(FunctionCallInfo call) noexcept
        : call_{call} {}

    FunctionCallInfo call_;
};

/** Marker for PostgreSQL table access-method handler pseudo-arguments. */
struct table_access_method_handler final {};

/** Marker for PostgreSQL subscripting handler pseudo-arguments. */
struct subscript_handler final {};

/**
 * Own an internal PostgreSQL subtransaction.
 * Commit or roll back explicitly, or let destruction roll it back on C++ scope
 * exit. Cleanup restores the outer memory context and resource owner.
 */
class subtransaction final {
public:
    /** Begin an internal subtransaction. */
    [[nodiscard]] static subtransaction begin();

    subtransaction(const subtransaction&) = delete;
    subtransaction& operator=(const subtransaction&) = delete;
    subtransaction(subtransaction&&) = delete;
    subtransaction& operator=(subtransaction&&) = delete;
    /** Roll back if still active. Exit the backend if cleanup fails. */
    ~subtransaction() noexcept;

    /** Commit and release the owned subtransaction. */
    void commit();
    /** Roll back and release the owned subtransaction. */
    void rollback();

private:
    subtransaction(
        MemoryContext outer_context,
        ResourceOwner outer_owner,
        int outer_nest_level,
        ResourceOwner subtransaction_owner) noexcept
        : outer_context_{outer_context},
          outer_owner_{outer_owner},
          outer_nest_level_{outer_nest_level},
          subtransaction_owner_{subtransaction_owner} {}

    MemoryContext outer_context_;
    ResourceOwner outer_owner_;
    int outer_nest_level_;
    ResourceOwner subtransaction_owner_;
    bool active_ = true;
};

/** Own an SPI session and finish it on C++ scope exit if still connected. */
class spi_session final {
public:
    /** Start with no SPI connection. */
    spi_session() noexcept = default;
    spi_session(const spi_session&) = delete;
    spi_session& operator=(const spi_session&) = delete;
    spi_session(spi_session&&) = delete;
    spi_session& operator=(spi_session&&) = delete;
    /** Finish an active SPI session. Exit the backend if cleanup fails. */
    ~spi_session() noexcept;

    /** Connect to SPI with these options, rejecting an already active session. */
    [[nodiscard]] int connect(int options = 0);
    /** Finish SPI, or return SPI_ERROR_UNCONNECTED if no session is active. */
    [[nodiscard]] int finish();
    /** Return whether this object owns an active SPI connection. */
    [[nodiscard]] bool active() const noexcept {
        return active_;
    }

private:
    bool active_ = false;
};

}  // namespace bioseq::postgres::support
