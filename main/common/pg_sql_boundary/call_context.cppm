/*-------------------------------------------------------------------------
 *
 * common/pg_sql_boundary/call_context.cppm
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

export module bioseq.pg_sql_boundary:call_context;

import :error_boundary;

export namespace bioseq::pg_utils {

struct sql_boundary_access;

class sql_call_context final {
public:
    [[nodiscard]] Oid function_oid() const noexcept {
        return call_ == nullptr || call_->flinfo == nullptr
            ? InvalidOid
            : call_->flinfo->fn_oid;
    }
    [[nodiscard]] Oid collation() const noexcept;
    [[nodiscard]] Oid argument_type(int zero_based_index) const;
    [[nodiscard]] Oid result_type() const;
    [[nodiscard]] ReturnSetInfo& return_set_info() const;
    [[nodiscard]] MemoryContext aggregate_memory_context() const;
    [[nodiscard]] WindowObject window_object() const;
    [[nodiscard]] void*& fn_extra() const noexcept;
    [[nodiscard]] MemoryContext fn_mcxt() const noexcept;
    void require_nonatomic_procedure_call() const;
    void commit_transaction();
    void rollback_transaction();

    sql_call_context(const sql_call_context&) = delete;
    sql_call_context& operator=(const sql_call_context&) = delete;

private:
    friend struct sql_boundary_access;

    explicit sql_call_context(FunctionCallInfo call) noexcept
        : call_{call} {}

    FunctionCallInfo call_;
};

struct sql_boundary_access final {
    [[nodiscard]] static sql_call_context
    make_context(FunctionCallInfo call) noexcept {
        return sql_call_context{call};
    }
};

struct table_am_handler_context final {};

struct subscript_handler_context final {};

class sql_subtransaction final {
public:
    [[nodiscard]] static sql_subtransaction begin();

    sql_subtransaction(const sql_subtransaction&) = delete;
    sql_subtransaction& operator=(const sql_subtransaction&) = delete;
    sql_subtransaction(sql_subtransaction&&) = delete;
    sql_subtransaction& operator=(sql_subtransaction&&) = delete;
    ~sql_subtransaction() noexcept;

    void commit();
    void rollback();

private:
    sql_subtransaction(
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

class scoped_spi_session final {
public:
    scoped_spi_session() noexcept = default;
    scoped_spi_session(const scoped_spi_session&) = delete;
    scoped_spi_session& operator=(const scoped_spi_session&) = delete;
    scoped_spi_session(scoped_spi_session&&) = delete;
    scoped_spi_session& operator=(scoped_spi_session&&) = delete;
    ~scoped_spi_session() noexcept;

    [[nodiscard]] int connect(int options = 0);
    [[nodiscard]] int finish();
    [[nodiscard]] bool active() const noexcept {
        return active_;
    }

private:
    bool active_ = false;
};

}  // namespace bioseq::pg_utils
