/**
 * @file admin/policy_catalog.cppm
 * @brief Index policy, catalog and shared publication failure contracts.
 */
module;

extern "C" {
#include "postgres.h"

#include "executor/spi.h"
#include "utils/errcodes.h"
}

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.postgres.storage.indexing.admin.policy_catalog;
export import bioseq.postgres.storage.indexing.admin.index_build_runtime;
export import bioseq.postgres.storage.indexing.admin.index_sketch_backend_policy;
import bioseq.common.logging;
import bioseq.postgres.storage.indexing.admin.minimizer_kernel_selection;
import bioseq.postgres.storage.indexing.admin.scheme_untagged_fragment_guard;
import bioseq.postgres.support;
import bioseq.postgres.sql_boundary;
import bioseq.postgres.configuration.aligner;
import bioseq.postgres.configuration.core;
import bioseq.postgres.storage.manager; // seq_caches, sequence_locator, is_locator_stub, decode_locator_from_stub
import bioseq.storage.layout;
import bioseq.postgres.types.compact_sequence; // bioseq::pg_types::seq, dispatch_instance
import bioseq.sequences;
import bioseq.common.seqan_adapter; // seqan3::char_is_valid_for
import bioseq.pipeline.runtime_contract; // minimizer_algorithm
import bioseq.sequences.kmers.highway_minimizer_contract;
static_assert(__cplusplus >= 202302L, "C++23-or-later");

export namespace bioseq::storage::indexing {

// ---- policy catalog -------------------------------------------------------
/// One enabled and maintenance-policy row from the index policy catalog.
struct policy_row {
    std::string name;
    bool enabled;
    bool update_on_insert;
};

/// Raise a PostgreSQL error for an invalid policy SPI stage or result row.
[[noreturn]] inline void
table_policy_spi_error_as_cpp(const char* stage,
                              int status,
                              std::uint64_t row = (std::numeric_limits<std::uint64_t>::max)()) {
    bioseq::postgres::support::invoke_pg_as_cpp([stage, status, row] {
        if (row == (std::numeric_limits<std::uint64_t>::max)()) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("could not load the index policy"),
                     errdetail("SPI stage=%s status=%d", stage, status)));
        } else {
            ereport(
                ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("index policy row is invalid"),
                 errdetail("SPI stage=%s row=%llu", stage, static_cast<unsigned long long>(row))));
        }
        pg_unreachable();
    });
    pg_unreachable();
}

/**
 * @brief Load every policy override for one physical sequence column.
 * @param user_relid Source Bioseq relation.
 * @param relfn Current relation storage incarnation.
 * @param attno Sequence attribute number.
 * @return Enabled and update-on-insert policy rows in catalog order.
 */
[[nodiscard]] inline std::vector<policy_row>
load_table_policy(Oid user_relid, Oid relfn, std::int16_t attno) {
    const std::string sql = fmt::format("SELECT index_name, enabled, update_on_insert "
                                        "FROM bioseq_internal.index_policy "
                                        "WHERE user_relid = {}::oid AND main_relfilenode = {}::oid "
                                        "AND column_attno = {}",
                                        static_cast<unsigned>(user_relid),
                                        static_cast<unsigned>(relfn),
                                        static_cast<int>(attno));

    std::vector<policy_row> out;
    bioseq::postgres::support::spi_session session;
    const int connect_status = session.connect();
    if (connect_status != SPI_OK_CONNECT) {
        table_policy_spi_error_as_cpp("SPI_connect", connect_status);
    }

    const int execute_status = bioseq::postgres::support::invoke_pg_as_cpp(
        [&sql] { return SPI_execute(sql.c_str(), /*read_only=*/true, /*tcount=*/0); });
    if (execute_status != SPI_OK_SELECT || SPI_tuptable == nullptr) {
        table_policy_spi_error_as_cpp("SPI_execute", execute_status);
    }

    const std::uint64_t row_count = SPI_processed;
    out.reserve(static_cast<std::size_t>(row_count));
    for (std::uint64_t row = 0; row < row_count; ++row) {
        bool enabled_is_null = false;
        bool update_is_null = false;
        const Datum enabled = bioseq::postgres::support::invoke_pg_as_cpp([row, &enabled_is_null] {
            return SPI_getbinval(
                SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 2, &enabled_is_null);
        });
        const Datum update_on_insert =
            bioseq::postgres::support::invoke_pg_as_cpp([row, &update_is_null] {
                return SPI_getbinval(
                    SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 3, &update_is_null);
            });
        if (enabled_is_null || update_is_null) {
            table_policy_spi_error_as_cpp("SPI_getbinval", execute_status, row);
        }

        char* name_data = bioseq::postgres::support::invoke_pg_as_cpp(
            [row] { return SPI_getvalue(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 1); });
        if (name_data == nullptr) {
            table_policy_spi_error_as_cpp("SPI_getvalue", execute_status, row);
        }

        std::string name;
        try {
            name.assign(name_data);
        } catch (...) {
            bioseq::postgres::support::invoke_pg_as_cpp([name_data] { pfree(name_data); });
            throw;
        }
        bioseq::postgres::support::invoke_pg_as_cpp([name_data] { pfree(name_data); });
        out.push_back(
            policy_row{std::move(name), DatumGetBool(enabled), DatumGetBool(update_on_insert)});
    }

    const int finish_status = session.finish();
    if (finish_status != SPI_OK_FINISH) {
        table_policy_spi_error_as_cpp("SPI_finish", finish_status);
    }
    return out;
}

// ---- age log + policy upsert ---------------------------------------------
/// Record the successful publication age for one persisted index.
void note_successful_write(Oid user_relid, Oid relfn, std::int16_t attno, std::string_view name);

/**
 * Bind the process-local table for one PostgreSQL maintenance operation.
 *
 * The cold plan chooses one runnable target once at the owning operation root.
 * The generated exact-target lookup is a direct table rebind, not a nested
 * Highway dispatch. scalar and Vulkan producers do not require this table.
 */
[[nodiscard]] inline const bioseq::minimizers::minimizer_kernels*
selected_minimizer_kernels_for_operation(bioseq::minimizers::minimizer_sketch_backend backend,
                                         std::string_view operation) {
    return bind_selected_minimizer_kernels_for_operation(backend, operation);
}

/// Scheme-untagged minimizer fragment and sequence rows do not carry a
/// minimizer-scheme identity.
/// Persisting bounded ntHash into that fragment layout would let a later
/// Exact reader (or the reverse) accept conflicting hashes under the same
/// index name. PostgreSQL-locator rebuilds use the scheme-tagged grouped-SoA
/// generation route. Only an inline scheme-untagged source reaches this
/// guard from update_index. The INSERT hook uses the same fragment layout and
/// keeps the guard until it can source a published PostgreSQL locator.
/// Silently substituting scalar or Exact is never permitted.
[[noreturn]] inline void reject_bounded_nthash_for_scheme_untagged_fragment_layout(
    bioseq::minimizers::minimizer_sketch_backend backend, std::string_view operation) {
    reject_bounded_nthash_for_scheme_untagged_fragment_storage(backend, operation);
}

// ---- INSERT-time build ----------------------------------------------------
/// Build every eligible per-insert index for one locator-backed sequence row.
void index_build_on_insert(Oid user_relid,
                           std::int16_t attno,
                           std::uint64_t rowno,
                           const bioseq::pg_types::seq* hdr,
                           const std::optional<sequence_locator>& loc_opt,
                           MemoryContext mctx);

/// Per-row driver invoked by the TAM insert hooks: builds on-insert indexes for
/// every SEQ column. `detoasted_values` carries the ORIGINAL (pre-stub) payload.
inline void
index_build_on_insert_columns(Oid user_relid,
                              const std::vector<std::int16_t>& seq_attnos,
                              const bool* tts_isnull,
                              Datum* detoasted_values,
                              const std::vector<std::optional<sequence_locator>>& per_col_locator,
                              std::uint64_t rowno,
                              MemoryContext mctx) {
    if (!bioseq::settings::index::insert_maintenance::enabled)
        return;
    for (std::size_t c = 0; c < seq_attnos.size() && c < per_col_locator.size(); ++c) {
        if (!per_col_locator[c].has_value())
            continue;
        const std::int16_t attno = seq_attnos[c];
        if (tts_isnull[attno - 1])
            continue;
        const bioseq::pg_types::seq* hdr = reinterpret_cast<const bioseq::pg_types::seq*>(
            DatumGetPointer(detoasted_values[attno - 1]));
        index_build_on_insert(user_relid, attno, rowno, hdr, per_col_locator[c], mctx);
    }
}

// ---- letter_to_rank -------------------------------------------------------
namespace detail {
constexpr std::size_t k_num_alphabets =
    static_cast<std::size_t>(pgseq_supported_alphabets::alphabet_count);
constexpr std::size_t k_char_domain = 256;
/// Build an alphabet-indexed byte-to-rank lookup table.
inline auto build_letter_rank_table() {
    std::array<std::array<std::int8_t, k_char_domain>, k_num_alphabets> table{};
    for (auto& row : table)
        row.fill(static_cast<std::int8_t>(-1));
    for (std::uint8_t alphabet_code = 0; alphabet_code < k_num_alphabets; ++alphabet_code) {
        // Rank conversion depends only on the alphabet.  Dispatching a
        // fabricated storage width here violates bioseq::pg_types::seq's active width
        // policy in optimized PostgreSQL builds (which intentionally enable
        // only 8-byte storage units).
        pgseq_supported_alphabets::dispatch_alphabet_by_code(
            alphabet_code, [&table, alphabet_code]<class p_alphabet>() {
                for (int char_code = 0; char_code < static_cast<int>(k_char_domain); ++char_code) {
                    const char letter = static_cast<char>(char_code);
                    if (!seqan3::char_is_valid_for<p_alphabet>(letter))
                        continue;
                    p_alphabet symbol{};
                    symbol.assign_char(letter);
                    table[alphabet_code][static_cast<std::uint8_t>(char_code)] =
                        static_cast<std::int8_t>(symbol.to_rank());
                }
            });
    }
    return table;
}
/// Return the process-local immutable byte-to-rank lookup table.
inline const auto& letter_rank_table() {
    static const auto t = build_letter_rank_table();
    return t;
}
} // namespace detail
/// Convert one encoded alphabet letter to its rank or return negative one.
[[nodiscard]] inline int letter_to_rank(std::uint8_t alphabet_code, char letter) {
    if (alphabet_code >= detail::k_num_alphabets)
        return -1;
    return static_cast<int>(
        detail::letter_rank_table()[alphabet_code][static_cast<std::uint8_t>(letter)]);
}

// ---- per-row index-present probe -----------------------------------------
/// Check the publication witness for one fragment index row.
[[nodiscard]] bool index_present(Oid user_relid,
                                 Oid db,
                                 Oid spc,
                                 Oid relfn,
                                 std::int16_t attno,
                                 std::string_view index_name,
                                 const sequence_locator& loc,
                                 std::int64_t row_key,
                                 seq_caches* caches,
                                 MemoryContext mctx);

// ---- estimate_index_presence (planner cost bias only) ---------------------
/// Estimate table coverage from shard statistics without scanning payloads.
[[nodiscard]] float
estimate_index_presence(Oid user_relid, double estimated_total_rows, std::string_view index_name);

} // namespace bioseq::storage::indexing
