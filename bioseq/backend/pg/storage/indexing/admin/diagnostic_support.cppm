/**
 * @file admin/diagnostic_support.cppm
 * @brief Report index inspection errors and find the sequence column to inspect.
 */
module;

extern "C" {
#include "postgres.h"
#include "executor/spi.h"
#include "utils/errcodes.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <cstdint>
#include <string>
#include <string_view>
#include <fmt/format.h>

export module bioseq.postgres.storage.indexing.admin.diagnostic_support;

import bioseq.postgres.support;

export namespace bioseq::storage::indexing {

/** Raise a PostgreSQL inspection error through the C++ error bridge. */
[[noreturn]] inline void
ready_minimizer_error_as_cpp(int sqlstate,
                             std::string_view message,
                             std::string_view detail = {}) {
    bioseq::postgres::support::invoke_pg_as_cpp([sqlstate, message, detail] {
        if (detail.empty()) {
            ereport(ERROR,
                    (errcode(sqlstate),
                     errmsg("%.*s", static_cast<int>(message.size()), message.data())));
        }
        ereport(ERROR,
                (errcode(sqlstate),
                 errmsg("%.*s", static_cast<int>(message.size()), message.data()),
                 errdetail("%.*s", static_cast<int>(detail.size()), detail.data())));
    });
    pg_unreachable();
}

/**
 * Find the table's only  live bioseq.seq column.
 * Raise an error when the table has zero or multiple, sequence columns.
 * @return One-brsed PostgreSQL column number.
 */
[[nodiscard]] inline std::int16_t single_seq_attno_for_inspection(Oid rel_oid) {
    const std::string sql = fmt::format("SELECT a.attnum FROM pg_attribute a "
                                        "JOIN pg_type t ON a.atttypid=t.oid "
                                        "JOIN pg_namespace n ON t.typnamespace=n.oid "
                                        "WHERE a.attrelid={}::oid AND t.typname='seq' "
                                        "AND n.nspname IN ('bioseq','bioseq_internal') "
                                        "AND a.attnum>0 AND NOT a.attisdropped ORDER BY a.attnum",
                                        static_cast<unsigned>(rel_oid));
    struct sequence_attribute_number_inspection_state {
        int connect_status = 0;
        int execute_status = 0;
        int finish_status = 0;
        std::uint64_t row_count = 0;
        std::int16_t result = 0;
        bool table_present = false;
        bool is_null = true;
    } state{};
    const char* sql_data = sql.c_str();
    bioseq::postgres::support::invoke_pg_as_cpp([&state, sql_data] noexcept {
        state.connect_status = SPI_connect();
        if (state.connect_status != SPI_OK_CONNECT)
            return;
        state.execute_status = SPI_execute(sql_data, true, 2);
        state.row_count = SPI_processed;
        state.table_present = SPI_tuptable != nullptr;
        if (state.execute_status == SPI_OK_SELECT && state.table_present && state.row_count == 1) {
            bool is_null = false;
            const Datum value =
                SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &is_null);
            state.is_null = is_null;
            if (!is_null)
                state.result = DatumGetInt16(value);
        }
        state.finish_status = SPI_finish();
    });
    if (state.connect_status != SPI_OK_CONNECT || state.finish_status != SPI_OK_FINISH) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INTERNAL_ERROR,
            "bioseq minimizer generation inspection could not inspect the source schema",
            fmt::format("SPI_connect_status={} SPI_finish_status={}",
                        state.connect_status,
                        state.finish_status));
    }
    if (state.execute_status != SPI_OK_SELECT || !state.table_present || state.row_count != 1 ||
        state.is_null || state.result <= 0) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "bioseq minimizer generation inspection requires exactly one bioseq.seq column",
            fmt::format("relation={} visible_seq_columns={}",
                        static_cast<unsigned>(rel_oid),
                        state.row_count));
    }
    return state.result;
}

} // namespace bioseq::storage::indexing
