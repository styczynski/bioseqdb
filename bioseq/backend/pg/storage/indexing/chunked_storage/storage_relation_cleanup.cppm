/**
 * @file storage_relation_cleanup.cppm
 * @brief Find and drop internal index tables.
 */
module;
#include <bioseq/pg_compat.h>

extern "C" {
#include "catalog/namespace.h"
#include "catalog/pg_type_d.h"
#include "executor/spi.h"
#include "storage/ipc.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

#include "miscadmin.h"
}

export module bioseq.postgres.storage.indexing.chunked_storage:storage_relation_cleanup;
import std;
import bioseq.common.logging;
import bioseq.postgres.support;
import :catalog;
export import :storage_adapter_contract;
namespace bioseq::storage::indexing {
namespace {

/// Cleanup step that produced an SPI status code.
enum class drop_index_spi_stage : std::uint8_t { connect, discovery, drop, finish };

/// How cleanup should respond to an SPI status code.
enum class drop_index_return_code_action : std::uint8_t {
    continue_operation,
    return_without_error,
    finish_then_return,
    raise_strict_error
};

[[nodiscard]] constexpr int expected_drop_index_spi_status(drop_index_spi_stage stage) noexcept {
    switch (stage) {
    case drop_index_spi_stage::connect:
        return SPI_OK_CONNECT;
    case drop_index_spi_stage::discovery:
        return SPI_OK_SELECT;
    case drop_index_spi_stage::drop:
        return SPI_OK_UTILITY;
    case drop_index_spi_stage::finish:
        return SPI_OK_FINISH;
    }
    return SPI_ERROR_OPUNKNOWN;
}

[[nodiscard]] constexpr drop_index_return_code_action
drop_index_return_code_policy(drop_index_relation_selector::kind selector_kind,
                              drop_index_spi_stage stage,
                              int status) noexcept {
    if (status == expected_drop_index_spi_status(stage))
        return drop_index_return_code_action::continue_operation;
    if (selector_kind == drop_index_relation_selector::kind::prefix_strict) {
        return drop_index_return_code_action::raise_strict_error;
    }
    if (stage == drop_index_spi_stage::discovery || stage == drop_index_spi_stage::drop) {
        return drop_index_return_code_action::finish_then_return;
    }
    return drop_index_return_code_action::return_without_error;
}

[[nodiscard]] constexpr const char* drop_index_spi_stage_name(drop_index_spi_stage stage) noexcept {
    switch (stage) {
    case drop_index_spi_stage::connect:
        return "SPI_connect";
    case drop_index_spi_stage::discovery:
    case drop_index_spi_stage::drop:
        return "SPI_execute";
    case drop_index_spi_stage::finish:
        return "SPI_finish";
    }
    return "unknown SPI stage";
}

using drop_index_selector_kind = drop_index_relation_selector::kind;
using drop_index_status_action = drop_index_return_code_action;
static_assert(drop_index_return_code_policy(drop_index_selector_kind::all_index_tables_best_effort,
                                            drop_index_spi_stage::connect,
                                            SPI_ERROR_CONNECT) ==
              drop_index_status_action::return_without_error);
static_assert(drop_index_return_code_policy(drop_index_selector_kind::all_index_tables_best_effort,
                                            drop_index_spi_stage::discovery,
                                            SPI_ERROR_ARGUMENT) ==
              drop_index_status_action::finish_then_return);
static_assert(drop_index_return_code_policy(drop_index_selector_kind::all_index_tables_best_effort,
                                            drop_index_spi_stage::drop,
                                            SPI_ERROR_TRANSACTION) ==
              drop_index_status_action::finish_then_return);
static_assert(drop_index_return_code_policy(drop_index_selector_kind::all_index_tables_best_effort,
                                            drop_index_spi_stage::finish,
                                            SPI_ERROR_UNCONNECTED) ==
              drop_index_status_action::return_without_error);
static_assert(drop_index_return_code_policy(drop_index_selector_kind::prefix_strict,
                                            drop_index_spi_stage::connect,
                                            SPI_ERROR_CONNECT) ==
              drop_index_status_action::raise_strict_error);
static_assert(drop_index_return_code_policy(drop_index_selector_kind::prefix_strict,
                                            drop_index_spi_stage::discovery,
                                            SPI_ERROR_ARGUMENT) ==
              drop_index_status_action::raise_strict_error);
static_assert(drop_index_return_code_policy(drop_index_selector_kind::prefix_strict,
                                            drop_index_spi_stage::drop,
                                            SPI_ERROR_TRANSACTION) ==
              drop_index_status_action::raise_strict_error);
static_assert(drop_index_return_code_policy(drop_index_selector_kind::prefix_strict,
                                            drop_index_spi_stage::finish,
                                            SPI_ERROR_UNCONNECTED) ==
              drop_index_status_action::raise_strict_error);
static_assert(drop_index_return_code_policy(drop_index_selector_kind::all_index_tables_best_effort,
                                            drop_index_spi_stage::connect,
                                            SPI_OK_CONNECT) ==
              drop_index_status_action::continue_operation);
static_assert(drop_index_return_code_policy(drop_index_selector_kind::prefix_strict,
                                            drop_index_spi_stage::discovery,
                                            SPI_OK_SELECT) ==
              drop_index_status_action::continue_operation);
static_assert(drop_index_return_code_policy(drop_index_selector_kind::all_index_tables_best_effort,
                                            drop_index_spi_stage::drop,
                                            SPI_OK_UTILITY) ==
              drop_index_status_action::continue_operation);
static_assert(drop_index_return_code_policy(drop_index_selector_kind::prefix_strict,
                                            drop_index_spi_stage::finish,
                                            SPI_OK_FINISH) ==
              drop_index_status_action::continue_operation);

/// Close the cleanup SPI session on success or while unwinding an exception.
class drop_index_spi_session {
    bool active_ = false;

  public:
    drop_index_spi_session() = default;
    drop_index_spi_session(const drop_index_spi_session&) = delete;
    drop_index_spi_session& operator=(const drop_index_spi_session&) = delete;

    [[nodiscard]] int connect() {
        const int status =
            bioseq::postgres::support::invoke_pg_as_cpp([] noexcept { return SPI_connect(); });
        active_ = status == SPI_OK_CONNECT;
        return status;
    }

    [[nodiscard]] int finish() {
        const int status =
            bioseq::postgres::support::invoke_pg_as_cpp([] noexcept { return SPI_finish(); });
        active_ = false;
        return status;
    }

    [[nodiscard]] bool handle_returned_status(drop_index_relation_selector selector,
                                              drop_index_spi_stage stage,
                                              int status) {
        switch (drop_index_return_code_policy(selector.selected_kind, stage, status)) {
        case drop_index_return_code_action::continue_operation:
            return true;
        case drop_index_return_code_action::return_without_error:
            return false;
        case drop_index_return_code_action::finish_then_return:
            (void)finish();
            return false;
        case drop_index_return_code_action::raise_strict_error:
            index_clear_spi_error_as_cpp(drop_index_spi_stage_name(stage), status);
        }
        pg_unreachable();
    }

    ~drop_index_spi_session() noexcept {
        if (!active_)
            return;

        volatile int finish_status = SPI_ERROR_UNCONNECTED;
        PG_TRY();
        {
            finish_status = SPI_finish();
        }
        PG_CATCH();
        {
            FlushErrorState();
            proc_exit(1);
        }
        PG_END_TRY();

        active_ = false;
        if (finish_status != SPI_OK_FINISH)
            proc_exit(1);
    }
};

/// One relation name returned by the cleanup discovery query.
struct discovered_index_relation_name {
    NameData value{};
    bool is_null = true;
};

[[nodiscard]] std::string quote_index_relation_name(const std::string& relation_name) {
    char* quoted = bioseq::postgres::support::invoke_pg_as_cpp(
        [relation_name_data = relation_name.c_str()] noexcept {
            return quote_qualified_identifier("bioseq_internal", relation_name_data);
        });

    std::string result;
    try {
        result.assign(quoted);
    } catch (...) {
        bioseq::postgres::support::invoke_pg_as_cpp([quoted] noexcept { pfree(quoted); });
        throw;
    }
    bioseq::postgres::support::invoke_pg_as_cpp([quoted] noexcept { pfree(quoted); });
    return result;
}

} // namespace
} // namespace bioseq::storage::indexing

export namespace bioseq::storage::indexing {
/// Drop the selected index tables.
/// Prefix cleanup treats failed SPI statuses as errors. Global cleanup stops
/// when a returned status indicates failure. PostgreSQL exceptions propagate.
void drop_index_relations(drop_index_relation_selector selector) {
    static constexpr char k_all_index_tables_query[] =
        "SELECT c.relname "
        "FROM pg_catalog.pg_class AS c "
        "WHERE c.relnamespace = $1::oid "
        "AND c.relkind = 'r'::\"char\" "
        "AND (starts_with(c.relname::text, 'seqidx_') "
        "OR starts_with(c.relname::text, 'seqidxtab_'))";
    static constexpr char k_prefix_query[] = "SELECT c.relname "
                                             "FROM pg_catalog.pg_class AS c "
                                             "WHERE c.relnamespace = $1::oid "
                                             "AND starts_with(c.relname::text, $2::text) "
                                             "ORDER BY (c.relkind = 'r'::\"char\") DESC, c.oid";

    const std::string owned_prefix{selector.prefix};
    const Oid internal_namespace = bioseq::postgres::support::invoke_pg_as_cpp(
        [] noexcept { return get_namespace_oid("bioseq_internal", false); });

    drop_index_spi_session spi_session;
    const int connect_status = spi_session.connect();
    if (!spi_session.handle_returned_status(
            selector, drop_index_spi_stage::connect, connect_status)) {
        return;
    }

    std::vector<std::string> relation_names;
    const int discovery_status =
        selector.selected_kind == drop_index_relation_selector::kind::all_index_tables_best_effort
            ? bioseq::postgres::support::invoke_pg_as_cpp([internal_namespace] noexcept {
                  Oid argument_types[1] = {OIDOID};
                  Datum argument_values[1] = {ObjectIdGetDatum(internal_namespace)};
                  return SPI_execute_with_args(k_all_index_tables_query,
                                               1,
                                               argument_types,
                                               argument_values,
                                               nullptr,
                                               true,
                                               0);
              })
            : bioseq::postgres::support::invoke_pg_as_cpp(
                  [internal_namespace, prefix_data = owned_prefix.c_str()] noexcept {
                      Oid argument_types[2] = {OIDOID, TEXTOID};
                      Datum argument_values[2] = {ObjectIdGetDatum(internal_namespace),
                                                  CStringGetTextDatum(prefix_data)};
                      const int status = SPI_execute_with_args(
                          k_prefix_query, 2, argument_types, argument_values, nullptr, true, 0);
                      pfree(DatumGetPointer(argument_values[1]));
                      return status;
                  });

    if (discovery_status != SPI_OK_SELECT || SPI_tuptable == nullptr) {
        const int effective_status =
            discovery_status == SPI_OK_SELECT ? SPI_ERROR_NOATTRIBUTE : discovery_status;
        (void)spi_session.handle_returned_status(
            selector, drop_index_spi_stage::discovery, effective_status);
        return;
    }

    bool invalid_discovery_row = false;
    {
        SPITupleTable* const discovery_table = SPI_tuptable;
        const std::uint64_t discovered_count = SPI_processed;
        relation_names.reserve(static_cast<std::size_t>(discovered_count));
        for (std::uint64_t row = 0; row < discovered_count; ++row) {
            const discovered_index_relation_name discovered =
                bioseq::postgres::support::invoke_pg_as_cpp([discovery_table, row] noexcept {
                    discovered_index_relation_name result;
                    const Datum name = SPI_getbinval(
                        discovery_table->vals[row], discovery_table->tupdesc, 1, &result.is_null);
                    if (!result.is_null)
                        result.value = *DatumGetName(name);
                    return result;
                });
            if (discovered.is_null) {
                invalid_discovery_row = true;
                break;
            }
            relation_names.emplace_back(NameStr(discovered.value));
        }
        bioseq::postgres::support::invoke_pg_as_cpp(
            [discovery_table] noexcept { SPI_freetuptable(discovery_table); });
    }

    if (invalid_discovery_row) {
        (void)spi_session.handle_returned_status(
            selector, drop_index_spi_stage::discovery, SPI_ERROR_NOATTRIBUTE);
        return;
    }

    for (const std::string& relation_name : relation_names) {
        const std::string qualified_name = quote_index_relation_name(relation_name);
        const std::string drop_statement =
            fmt::format("DROP TABLE IF EXISTS {} CASCADE", qualified_name);
        const int drop_status = bioseq::postgres::support::invoke_pg_as_cpp(
            [statement = drop_statement.c_str()] noexcept {
                return SPI_execute(statement, false, 0);
            });
        if (!spi_session.handle_returned_status(
                selector, drop_index_spi_stage::drop, drop_status)) {
            return;
        }
    }

    const int finish_status = spi_session.finish();
    (void)spi_session.handle_returned_status(selector, drop_index_spi_stage::finish, finish_status);
}

inline void clear_with_prefix_(const std::string& prefix) {
    drop_index_relations(drop_index_relation_selector::with_prefix(prefix));
}

void fragment_index_storage::clear(std::string_view name) {
    clear_with_prefix_(fmt::format(
        "seqidx_{}_{:016x}_",
        short_index_for(name),
        identity_hash64(db_oid_, tablespace_oid_, main_relfilenode_, column_attno_, name)));
    index_relid_ = InvalidOid;
}
void sequence_index_storage::clear(std::string_view name) {
    clear_with_prefix_(fmt::format(
        "seqidx_{}_{:016x}_",
        short_index_for(name),
        identity_hash64(db_oid_, tablespace_oid_, main_relfilenode_, column_attno_, name)));
    index_relid_ = InvalidOid;
    decoded_valid_ = false;
}
void table_index_storage::clear(std::string_view name) {
    clear_with_prefix_(fmt::format(
        "seqidxtab_{}_{:016x}_",
        short_index_for(name),
        identity_hash64(db_oid_, tablespace_oid_, main_relfilenode_, column_attno_, name)));
    index_relid_ = InvalidOid;
}

} // namespace bioseq::storage::indexing
