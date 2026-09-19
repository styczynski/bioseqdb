/**
 * @file admin/rebuild/post_load.cppm
 * @brief Post-load policy lookup and index maintenance.
 */
module;

extern "C" {
#include "postgres.h"

#include "executor/spi.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/errcodes.h"
#include "varatt.h"
}

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <fmt/format.h>

export module bioseq.postgres.storage.indexing.admin:rebuild_post_load;
import bioseq.postgres.support;
import bioseq.indexing.type;
import bioseq.postgres.storage.indexing.index_relation_identity;
import bioseq.postgres.storage.indexing.admin.policy_catalog;
import bioseq.postgres.storage.indexing.supported_indexes;
static_assert(__cplusplus >= 202302L, "C++23-or-later");

export namespace bioseq::storage::indexing {

/**
 * @brief Check whether an index name is present in the PostgreSQL registry.
 * @param name Logical index name.
 * @return True only for a registered name.
 */
[[nodiscard]] bool name_is_registered(std::string_view name) {
    for (const auto& type : pg_supported_indexes::list_types()) {
        if (type.first == name) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Check whether inserts may maintain one registered index.
 * @param name Logical index name.
 * @return True only when the registry enables insertion-time maintenance.
 */
[[nodiscard]] bool index_allows_update_on_insert(std::string_view name) {
    const auto flags = pg_supported_indexes::allows_on_insert_flags();
    const auto types = pg_supported_indexes::list_types();
    for (std::size_t i = 0; i < types.size(); ++i) {
        if (types[i].first == name) {
            return flags[i];
        }
    }
    return false;
}

/**
 * @brief Return the physical layout registered for one PostgreSQL index name.
 * @param name Registered logical index name.
 * @return Its compile-time registry layout.
 */
[[nodiscard]] bioseq::indexing::sequence_index_layout layout_of_name(std::string_view name) {
    return pg_supported_indexes::layout_of(name);
}

/// Raise a post-load maintenance SPI failure with its owning operation.
[[noreturn]] inline void load_index_spi_error_as_cpp(std::string_view operation,
                                                     std::string_view detail) {
    bioseq::postgres::support::invoke_pg_as_cpp([operation, detail] {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq.load post-load index maintenance failed"),
                 errdetail("operation=%.*s %.*s",
                           static_cast<int>(operation.size()),
                           operation.data(),
                           static_cast<int>(detail.size()),
                           detail.data())));
    });
    pg_unreachable();
}

/**
 * @brief Execute one SPI scalar query and require exactly one nonnull result.
 * @tparam p_decoder Trivial noexcept Datum decoder.
 * @param sql Complete scalar statement.
 * @param read_only SPI read-only mode.
 * @param operation Diagnostic operation name.
 * @param decode Decoder invoked before SPI_finish.
 * @return The decoded scalar value.
 */
template <class p_decoder>
[[nodiscard]] inline auto strict_spi_select_one(const std::string& sql,
                                                bool read_only,
                                                std::string_view operation,
                                                p_decoder&& decode)
    -> std::invoke_result_t<p_decoder&, Datum> {
    using result_type = std::invoke_result_t<p_decoder&, Datum>;
    static_assert(!std::is_reference_v<result_type>);
    static_assert(std::is_trivially_copyable_v<result_type>);
    static_assert(std::is_default_constructible_v<result_type>);
    static_assert(std::is_nothrow_invocable_r_v<result_type, p_decoder&, Datum>,
                  "SPI scalar decoders run before SPI_finish and must not throw");
    struct spi_scalar_state {
        int connect_status = 0;
        int execute_status = 0;
        int finish_status = 0;
        std::uint64_t row_count = 0;
        bool table_present = false;
        bool is_null = true;
        result_type result{};
    } state{};
    const char* sql_data = sql.c_str();
    bioseq::postgres::support::invoke_pg_as_cpp([&state, sql_data, read_only, &decode] noexcept {
        state.connect_status = SPI_connect();
        if (state.connect_status != SPI_OK_CONNECT)
            return;
        state.execute_status = SPI_execute(sql_data, read_only, 0);
        state.row_count = SPI_processed;
        state.table_present = SPI_tuptable != nullptr;
        if (state.execute_status == SPI_OK_SELECT && state.row_count == 1 && state.table_present) {
            const Datum datum =
                SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &state.is_null);
            if (!state.is_null)
                state.result = decode(datum);
        }
        state.finish_status = SPI_finish();
    });
    if (state.connect_status != SPI_OK_CONNECT || state.execute_status != SPI_OK_SELECT ||
        state.row_count != 1 || !state.table_present || state.is_null ||
        state.finish_status != SPI_OK_FINISH) {
        load_index_spi_error_as_cpp(
            operation,
            fmt::format("SPI_connect_status={} SPI_execute_status={} rows={} "
                        "tuptable={} result_null={} SPI_finish_status={}",
                        state.connect_status,
                        state.execute_status,
                        state.row_count,
                        state.table_present ? "present" : "null",
                        state.is_null ? "true" : "false",
                        state.finish_status));
    }
    return state.result;
}

/// Load validated registered policy rows for post-load maintenance.
[[nodiscard]] inline std::vector<policy_row>
load_table_policy_strict_for_post_load(Oid user_relid, Oid relfilenode, std::int16_t attno) {
    constexpr std::size_t k_max_policy_rows = pg_supported_indexes::list_types().size();
    constexpr auto k_registered_types = pg_supported_indexes::list_types();
    struct raw_policy_row {
        std::uint32_t registry_ordinal = 0;
        bool enabled = false;
        bool update_on_insert = false;
    };
    struct spi_policy_state {
        int connect_status = 0;
        int execute_status = 0;
        int finish_status = 0;
        std::uint64_t row_count = 0;
        std::uint64_t matched_row_count = 0;
        std::uint64_t invalid_row = (std::numeric_limits<std::uint64_t>::max)();
        bool table_present = false;
        bool overflow = false;
        std::array<raw_policy_row, k_max_policy_rows> rows{};
    } state{};
    const std::string sql = fmt::format("SELECT index_name,enabled,update_on_insert "
                                        "FROM bioseq_internal.index_policy "
                                        "WHERE user_relid={}::oid AND main_relfilenode={}::oid "
                                        "AND column_attno={} ORDER BY index_name",
                                        static_cast<unsigned>(user_relid),
                                        static_cast<unsigned>(relfilenode),
                                        static_cast<int>(attno));
    const char* sql_data = sql.c_str();
    bioseq::postgres::support::invoke_pg_as_cpp([&state, &k_registered_types, sql_data] noexcept {
        state.connect_status = SPI_connect();
        if (state.connect_status != SPI_OK_CONNECT)
            return;
        state.execute_status = SPI_execute(sql_data, true, 0);
        state.row_count = SPI_processed;
        state.table_present = SPI_tuptable != nullptr;
        if (state.execute_status == SPI_OK_SELECT && state.table_present) {
            for (std::uint64_t row = 0; row < state.row_count; ++row) {
                bool name_null = false;
                bool enabled_null = false;
                bool update_null = false;
                const Datum name_datum =
                    SPI_getbinval(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 1, &name_null);
                const Datum enabled_datum =
                    SPI_getbinval(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 2, &enabled_null);
                const Datum update_datum =
                    SPI_getbinval(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 3, &update_null);
                if (name_null || enabled_null || update_null) {
                    state.invalid_row = row;
                    break;
                }
                text* name_text = DatumGetTextPP(name_datum);
                const std::size_t name_length =
                    static_cast<std::size_t>(VARSIZE_ANY_EXHDR(name_text));
                std::size_t registry_ordinal = k_registered_types.size();
                for (std::size_t candidate = 0; candidate < k_registered_types.size();
                     ++candidate) {
                    const std::string_view registered_name = k_registered_types[candidate].first;
                    if (name_length == registered_name.size() &&
                        std::memcmp(VARDATA_ANY(name_text), registered_name.data(), name_length) ==
                            0) {
                        registry_ordinal = candidate;
                        break;
                    }
                }
                if (registry_ordinal == k_registered_types.size())
                    continue;
                if (state.matched_row_count >= state.rows.size()) {
                    state.overflow = true;
                    break;
                }
                raw_policy_row& decoded =
                    state.rows[static_cast<std::size_t>(state.matched_row_count++)];
                decoded.registry_ordinal = static_cast<std::uint32_t>(registry_ordinal);
                decoded.enabled = DatumGetBool(enabled_datum);
                decoded.update_on_insert = DatumGetBool(update_datum);
            }
        }
        state.finish_status = SPI_finish();
    });
    if (state.connect_status != SPI_OK_CONNECT || state.execute_status != SPI_OK_SELECT ||
        !state.table_present || state.overflow ||
        state.invalid_row != (std::numeric_limits<std::uint64_t>::max)() ||
        state.finish_status != SPI_OK_FINISH) {
        load_index_spi_error_as_cpp(
            "load policy query",
            fmt::format("SPI_connect_status={} SPI_execute_status={} rows={} "
                        "matched_rows={} tuptable={} overflow={} invalid_row={} "
                        "SPI_finish_status={}",
                        state.connect_status,
                        state.execute_status,
                        state.row_count,
                        state.matched_row_count,
                        state.table_present ? "present" : "null",
                        state.overflow ? "true" : "false",
                        state.invalid_row,
                        state.finish_status));
    }
    std::vector<policy_row> result;
    result.reserve(static_cast<std::size_t>(state.matched_row_count));
    for (std::size_t row = 0; row < static_cast<std::size_t>(state.matched_row_count); ++row) {
        const std::size_t registry_ordinal = state.rows[row].registry_ordinal;
        result.push_back(policy_row{std::string{k_registered_types[registry_ordinal].first},
                                    state.rows[row].enabled,
                                    state.rows[row].update_on_insert});
    }
    return result;
}

/**
 * @brief Rebuild every enabled update-on-insert index after a streamed table load.
 * @param main_relid Loaded Bioseq relation.
 * @return Sum of rows reported by the invoked index rebuilds.
 */
[[nodiscard]] std::int64_t build_policy_update_on_insert_indexes(Oid main_relid) {
    const relation_identity rid = index_relation_identity(main_relid);
    pg_supported_indexes idx;
    idx.apply_table_defaults();

    // Overlay the persisted per-table policy. set_index_policy_impl stamps every
    // seq column identically, so the first seq column is a faithful table-level
    // proxy; if none is found the registry defaults stand.
    const std::string attno_sql =
        fmt::format("SELECT a.attnum FROM pg_attribute a JOIN pg_type t "
                    "ON a.atttypid=t.oid JOIN pg_namespace n ON t.typnamespace=n.oid "
                    "WHERE a.attrelid={}::oid AND t.typname='seq' "
                    "AND n.nspname IN ('bioseq','bioseq_internal') "
                    "AND a.attnum>0 AND NOT a.attisdropped "
                    "ORDER BY a.attnum LIMIT 1",
                    static_cast<unsigned>(main_relid));
    const std::int16_t first_seq_attno = strict_spi_select_one(
        attno_sql, /*read_only=*/true, "first sequence column", [](Datum value) noexcept {
            return DatumGetInt16(value);
        });
    if (first_seq_attno <= 0) {
        load_index_spi_error_as_cpp("first sequence column", "attribute number is not positive");
    }
    idx.merge_overrides(
        load_table_policy_strict_for_post_load(main_relid, rid.relfilenode, first_seq_attno));

    constexpr auto k_index_types = pg_supported_indexes::list_types();
    std::int64_t total = 0;
    for (std::size_t i = 0; i < k_index_types.size(); ++i) {
        if (!idx.enabled[i])
            continue;
        if (idx.policy[i] != bioseq::indexing::index_update_policy::update_on_insert)
            continue;
        // Use a distinct SPI portal/executor frame so sidecar DDL, snapshot
        // changes, and relcache invalidations are contained before bioseq.load resumes.
        const std::string index_name{k_index_types[i].first};
        const char* quoted_name = bioseq::postgres::support::invoke_pg_as_cpp(
            [&index_name] { return quote_literal_cstr(index_name.c_str()); });
        const std::string sql = fmt::format("SELECT bioseq.update_index({}::oid::regclass, {})",
                                            static_cast<unsigned>(main_relid),
                                            quoted_name);
        const std::int64_t written =
            strict_spi_select_one(sql,
                                  /*read_only=*/false,
                                  fmt::format("bioseq.update_index({})", index_name),
                                  [](Datum value) noexcept { return DatumGetInt64(value); });
        if (written < 0 || written > (std::numeric_limits<std::int64_t>::max)() - total) {
            load_index_spi_error_as_cpp(
                "bioseq.update_index result",
                fmt::format("index={} count={} accumulated={}", index_name, written, total));
        }
        total += written;
    }
    return total;
}

} // namespace bioseq::storage::indexing
