/**
 * @file symbol_counts.cppm
 * @brief Per-sequence symbol-rank histogram index.
 *
 * NO_POSTGRES. `letter_to_rank` (which needs bioseq::pg_types::seq::dispatch_instance) lives
 * on the pg side (index_admin_pg.cppm) — pg_compact_sequence is not a NO_POSTGRES
 * module, so it cannot be named here.
 */
module;
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <libassert/assert.hpp>
#include <optional>
#include <string_view>
export module bioseq.indexing.symbol_counts;
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.sequences;
static_assert(__cplusplus >= 202302L, "C++23-or-later");

export namespace bioseq::indexing {

/**
 * @brief Counts each logical symbol rank in a lazy sequence.
 * @tparam p_alphabet Sequence alphabet whose ranks are counted
 * @param seq Sequence view to scan
 * @return Fixed-size histogram indexed by logical symbol rank
 */
template <sequence_alphabet p_alphabet>
auto tally_symbol_counts(
        bioseq::storage::lazy_sequence_view<p_alphabet> seq) {
    using traits = typename bioseq::storage::lazy_sequence_view<
        p_alphabet>::traits;
    constexpr std::size_t rank_count =
        std::size_t{1} << traits::k_logical_bits_per_symbol;
    std::array<std::int64_t, rank_count> counts{};
    for (auto logical_rank : seq.template iterable<iterator_mode::logical_rank>()) {
        const std::size_t rank = static_cast<std::size_t>(logical_rank);
        DEBUG_ASSERT(rank < rank_count, "tally_symbol_counts: rank in range");
        ++counts[rank];
    }
    return counts;
}

/**
 * @brief Stores the per-sequence logical symbol histogram.
 * @tparam p_storage Inline index storage implementation
 */
template <class p_storage>
struct symbol_counts_index {
    static_assert(is_index_chunked_storage_for<
        p_storage, sequence_index_layout::sequence_inline>);
    /// @brief Storage layout used by this index.
    static constexpr sequence_index_layout index_type = sequence_index_layout::sequence_inline;
    /// @brief Registry name of this index.
    static constexpr std::string_view index_name = k_symbol_counts_index_name;
    /// @brief Concrete storage type accepted by this index.
    using storage_type = p_storage;

    /**
     * @brief Builds and persists the histogram for one sequence.
     * @tparam p_alphabet Sequence alphabet whose ranks are counted
     * @param storage Destination inline storage
     * @param seq Sequence view to scan
     */
    template <sequence_alphabet p_alphabet>
    static void build(
            p_storage& storage,
            bioseq::storage::lazy_sequence_view<p_alphabet> seq) {
        const auto counts = tally_symbol_counts(seq);
        storage.resize(counts.size() * sizeof(std::int64_t));
        storage.template write_to_each<std::int64_t>(
            [&counts](std::size_t chunk_no, std::int64_t* output, std::size_t capacity) {
                DEBUG_ASSERT(chunk_no == 0 && capacity == counts.size(),
                             "symbol_counts: one inline chunk");
                std::copy(counts.begin(), counts.end(), output);
                return chunk_no;
            });
    }
    /**
     * @brief Replaces the persisted histogram for one sequence.
     * @tparam p_alphabet Sequence alphabet whose ranks are counted
     * @param storage Destination inline storage
     * @param seq Sequence view to scan
     */
    template <sequence_alphabet p_alphabet>
    static void rebuild(
            p_storage& storage,
            bioseq::storage::lazy_sequence_view<p_alphabet> seq) {
        storage.clear_entity();
        build(storage, seq);
    }

    /**
     * @brief Reads one persisted rank count when the index is present.
     * @param storage Source inline storage
     * @param rank Logical symbol rank to read
     * @return Stored count, or `std::nullopt` when no histogram is present
     */
    static std::optional<std::int64_t> read_count(
            p_storage& storage,
            std::size_t rank) {
        const std::size_t bytes = storage.chunk_byte_size();
        if (bytes == 0) {
            return std::nullopt;
        }
        if (bytes % sizeof(std::int64_t) != 0) {
            storage.index_error("symbol_counts: stored chunk byte size is not int64-aligned");
        }
        const std::size_t rank_count = bytes / sizeof(std::int64_t);
        if (rank >= rank_count) {
            storage.index_error("symbol_counts: rank outside stored count vector");
        }
        return storage.template read_chunk<std::int64_t>()[rank];
    }
};

}  // namespace bioseq::indexing

export namespace bioseq::indexing {
/** @brief Describes the symbol-count index to the compile-time registry. */
template <> struct index_descriptor<symbol_counts_index> {
    /// @brief Storage layout registered for symbol counts.
    static constexpr sequence_index_layout index_type = sequence_index_layout::sequence_inline;
    /// @brief Registry name of the symbol-count index.
    static constexpr std::string_view index_name = k_symbol_counts_index_name;
    /// @brief Allows inexpensive histogram maintenance during insertion.
    static constexpr bool allows_update_on_insert = true;
};
static_assert(
    requires { index_descriptor<symbol_counts_index>::index_name; },
    "symbol_counts_index index_descriptor must be exported");
} // namespace bioseq::indexing
