/**
 * @file admin/policy_catalog.cppm
 * @brief Read index policies and support index maintenance during insertion.
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

// Index policies.
/// An index name and its enabled and update-on-insert settings.
struct policy_row {
    std::string name;
    bool enabled;
    bool update_on_insert;
};

/// Report a policy-query failure or invalid row as a PostgreSQL error.
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
 * Read index settings for one sequence  column in the current table file.
 * Reject NULL fields and SPI failures. Rows have no specified order.
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

// Publication age and kernel selection.
/// Save the time when this index was successfully written
void note_successful_write(Oid user_relid, Oid relfn, std::int16_t attno, std::string_view name);

/**
 * Select the process-local SIMD kernel table for this maintenance operation.
 * Scalar and Vulkan builds return null because they do not use this table.
 */
[[nodiscard]] inline const bioseq::minimizers::minimizer_kernels*
selected_minimizer_kernels_for_operation(bioseq::minimizers::minimizer_sketch_backend backend,
                                         std::string_view operation) {
    return bind_selected_minimizer_kernels_for_operation(backend, operation);
}

/// Reject bounded ntHash when the storage format cannot record its  hash scheme.
/// Otherwise an Exact, reader could accept incompatible hashes uder the same
/// index name. Locator-based READY builds use a format that records the scheme.
[[noreturn]] inline void reject_bounded_nthash_for_scheme_untagged_fragment_layout(
    bioseq::minimizers::minimizer_sketch_backend backend, std::string_view operation) {
    reject_bounded_nthash_for_scheme_untagged_fragment_storage(backend, operation);
}

// Index maintenance during insertion.
/// Build the enabled update-on-insert indexes using the sequence payload and its locator.
void index_build_on_insert(Oid user_relid,
                           std::int16_t attno,
                           std::uint64_t rowno,
                           const bioseq::pg_types::seq* hdr,
                           const std::optional<sequence_locator>& loc_opt,
                           MemoryContext mctx);

/// Build insert-time indexes for each non-null sequence column with a locator.
/// detoasted_values must contain the full sequence values from before stub replacement.
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

// Alphabet  letter lookup.
namespace detail {
constexpr std::size_t k_num_alphabets =
    static_cast<std::size_t>(pgseq_supported_alphabets::alphabet_count);
constexpr std::size_t k_char_domain = 256;
/// Build a byte-to-rank table for each alphabet, using -1 for invalid letters.
inline auto build_letter_rank_table() {
    std::array<std::array<std::int8_t, k_char_domain>, k_num_alphabets> table{};
    for (auto& row : table)
        row.fill(static_cast<std::int8_t>(-1));
    for (std::uint8_t alphabet_code = 0; alphabet_code < k_num_alphabets; ++alphabet_code) {
        // Choose only the alphabet here. Rank conversion is independent of sequence
        // packing, and a made-up storage width can be disabled in this build.
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
/// Return the letter rank, or -1 for an unknown alphabet or invalid letter.
[[nodiscard]] inline int letter_to_rank(std::uint8_t alphabet_code, char letter) {
    if (alphabet_code >= detail::k_num_alphabets)
        return -1;
    return static_cast<int>(
        detail::letter_rank_table()[alphabet_code][static_cast<std::uint8_t>(letter)]);
}

// Stored-index presence.
/// Check whether the index has a directory chunk for this source row.
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

// Planner coverage estimate
/// Estimate  indexed-row coverage from one shard statistic for planner costing.
[[nodiscard]] float
estimate_index_presence(Oid user_relid, double estimated_total_rows, std::string_view index_name);

} // namespace bioseq::storage::indexing
