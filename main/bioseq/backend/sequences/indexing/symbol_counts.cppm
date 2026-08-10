/**
 * @file symbol_counts.cppm
 * @brief Per-sequence symbol-rank histogram index.
 *
 * NO_POSTGRES. `letter_to_rank` (which needs bioseq::pg_types::seq::dispatch_instance) lives
 * on the pg side (index_admin_pg.cppm) — pg_compact_sequence is not a NO_POSTGRES
 * module, so it cannot be named here.
 */
module;
#include <libassert/assert.hpp>
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
export module bioseq.indexing.symbol_counts;
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.storage.lazy_sequence;     // canonical lazy_sequence_view
import bioseq.storage.size;     // sequence_size_t
import bioseq.sequences;        // compact_sequence_range, iterator_mode
static_assert(__cplusplus >= 202302L, "C++23-or-later");

export namespace bioseq::indexing {

/// Real per-rank histogram over a lazy sequence. The logical-rank iterator
/// masks `& char_mask`, so it yields a raw integer rank in [0, 2^bits_per_char).
template <sequence_alphabet p_alphabet>
auto tally_symbol_counts(
        bioseq::storage::lazy_sequence_view<p_alphabet> seq) {
    using traits = typename bioseq::storage::lazy_sequence_view<
        p_alphabet>::traits;
    constexpr std::size_t rank_count =
        std::size_t{1} << traits::logical_bits_per_symbol;
    std::array<std::int64_t, rank_count> counts{};
    for (auto logical_rank : seq.template iterable<iterator_mode::logical_rank>()) {
        const std::size_t rank = static_cast<std::size_t>(logical_rank);
        DEBUG_ASSERT(rank < rank_count, "tally_symbol_counts: rank in range");
        ++counts[rank];
    }
    return counts;
}

/// The registered INLINE_PER_SEQUENCE symbol-counts index.
template <class p_storage>
struct symbol_counts_index {
    static_assert(is_index_chunked_storage_for<
        p_storage, sequence_index_layout::sequence_inline>);
    static constexpr sequence_index_layout index_type = sequence_index_layout::sequence_inline;
    static constexpr std::string_view  index_name = k_symbol_counts_index_name;
    using storage_type = p_storage;

    template <sequence_alphabet p_alphabet>
    static void build(
            p_storage& storage,
            bioseq::storage::lazy_sequence_view<p_alphabet> seq) {
        auto counts = tally_symbol_counts(seq);                      // std::array<int64,N=2^bits_per_char>
        storage.resize(counts.size() * sizeof(std::int64_t));        // ONE chunk, N*sizeof(int64) bytes << MAX
        storage.template write_to_each<std::int64_t>([&counts](std::size_t c, std::int64_t* buf, std::size_t cap){
            DEBUG_ASSERT(c == 0 && cap == counts.size(), "symbol_counts: one inline chunk");
            std::copy(counts.begin(), counts.end(), buf); return c; });
    }
    template <sequence_alphabet p_alphabet>
    static void rebuild(
            p_storage& storage,
            bioseq::storage::lazy_sequence_view<p_alphabet> seq) {
        storage.clear_entity();
        build(storage, seq);
    }

    /// SQL-facing read. Returns nullopt when the index is ABSENT for this
    /// sequence (chunk_byte_size()==0) so the caller recomputes.
    static std::optional<std::int64_t> read_count(
            p_storage& storage,
            std::size_t rank) {
        const std::size_t bytes = storage.chunk_byte_size();
        if (bytes == 0) return std::nullopt;                                        // absent index => caller recomputes
        if (bytes % sizeof(std::int64_t) != 0)
            storage.index_error("symbol_counts: stored chunk byte size is not int64-aligned");
        const std::size_t n = bytes / sizeof(std::int64_t);
        if (rank >= n) storage.index_error("symbol_counts: rank outside stored count vector"); // present-but-truncated
        return storage.template read_chunk<std::int64_t>()[rank];
    }
};

}  // namespace bioseq::indexing

export namespace bioseq::indexing {
template <> struct index_descriptor<symbol_counts_index> {
    static constexpr sequence_index_layout index_type = sequence_index_layout::sequence_inline;
    static constexpr std::string_view  index_name = k_symbol_counts_index_name;
    static constexpr bool allows_update_on_insert = true;   // cheap INLINE tally; default on-insert
};
static_assert(requires { index_descriptor<symbol_counts_index>::index_name; },
              "symbol_counts_index index_descriptor must be exported");
}
