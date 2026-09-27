/*-------------------------------------------------------------------------
 *
 * common/postgres/sql_boundary/call_context.cppm
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

export module bioseq.postgres.sql_boundary:call_context;

import :error_boundary;

export namespace bioseq::postgres::support {

/** Checked non-owning view of PostgreSQL's active fmgr call frame. */
class function_call_context final {
public:
    /** Construct a checked view for one fmgr invocation. */
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
    /** Return the declared OID for one zero-based argument. */
    [[nodiscard]] Oid argument_type_oid(int argument_index) const;
    /** Return the declared result type OID. */
    [[nodiscard]] Oid result_type_oid() const;
    /** Return the active set-result descriptor or raise a PostgreSQL error. */
    [[nodiscard]] ReturnSetInfo& set_result_info() const;
    /** Return the aggregate memory context or raise a PostgreSQL error. */
    [[nodiscard]] MemoryContext aggregate_memory_context() const;
    /** Return the active window object or raise a PostgreSQL error. */
    [[nodiscard]] WindowObject window_object() const;
    /** Return the function-cache payload slot. */
    [[nodiscard]] void*& function_extra() const noexcept;
    /** Return the function-cache memory context. */
    [[nodiscard]] MemoryContext function_memory_context() const noexcept;
    /** Require a non-atomic procedure invocation. */
    void require_nonatomic_call() const;
    /** Commit the active non-atomic procedure transaction. */
    void commit_transaction();
    /** Roll back and chain the active non-atomic procedure transaction. */
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

/** RAII owner for one PostgreSQL internal subtransaction. */
class subtransaction final {
public:
    /** Begin an internal subtransaction. */
    [[nodiscard]] static subtransaction begin();

    subtransaction(const subtransaction&) = delete;
    subtransaction& operator=(const subtransaction&) = delete;
    subtransaction(subtransaction&&) = delete;
    subtransaction& operator=(subtransaction&&) = delete;
    /** Roll back an active subtransaction; terminate on cleanup failure. */
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

/** RAII owner for one PostgreSQL Server Programming Interface session. */
class spi_session final {
public:
    /** Construct a disconnected SPI session owner. */
    spi_session() noexcept = default;
    spi_session(const spi_session&) = delete;
    spi_session& operator=(const spi_session&) = delete;
    spi_session(spi_session&&) = delete;
    spi_session& operator=(spi_session&&) = delete;
    /** Finish an active SPI session; terminate on cleanup failure. */
    ~spi_session() noexcept;

    /** Connect SPI with the requested PostgreSQL options. */
    [[nodiscard]] int connect(int options = 0);
    /** Finish the active SPI session. */
    [[nodiscard]] int finish();
    /** Return whether this object owns an active SPI connection. */
    [[nodiscard]] bool active() const noexcept {
        return active_;
    }

private:
    bool active_ = false;
};

}  // namespace bioseq::postgres::support
