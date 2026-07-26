/**
 * @file symbol_counts_index.cppm
 * @brief DOC0279 §12.5 — `tally_symbol_counts` + the `SymbolCountsIndex`
 *        INLINE_PER_SEQUENCE index. Real per-rank histogram (NOT the 49 stub).
 *
 * NO_POSTGRES. `letter_to_rank` (which needs bioseq::pg_types::seq::dispatch_instance) lives
 * on the pg side (index_admin_pg.cppm) — pg_compact_sequence is not a NO_POSTGRES
 * module, so it cannot be named here.
 */
module;
#include "common.h"                       // DEBUG_ASSERT
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
import bioseq.storage.lazy;     // canonical basic_lazy_sequence_view
import bioseq.storage.size;     // sequence_size_t
import bioseq.sequences;        // compact_sequence_range, IteratorMode{BP,SYMBOL}
static_assert(__cplusplus >= 202302L, "C++23-or-later");

export namespace bioseq::indexing {

/// Real per-rank histogram over a lazy sequence. The BP-iterator deref already
/// masks `& char_mask`, so it yields a raw integer rank in [0, 2^bits_per_char).
template <sequence_alphabet Alphabet>
auto tally_symbol_counts(
        bioseq::storage::basic_lazy_sequence_view<Alphabet> seq) {
    using traits = typename bioseq::storage::basic_lazy_sequence_view<
        Alphabet>::traits;
    constexpr std::size_t N =
        std::size_t{1} << traits::logical_bits_per_symbol;
    std::array<std::int64_t, N> counts{};
    for (auto bp : seq.template iterable<BP>()) {
        const std::size_t rank = static_cast<std::size_t>(bp);
        DEBUG_ASSERT(rank < N, "tally_symbol_counts: rank in range");
        ++counts[rank];
    }
    return counts;
}

/// The registered INLINE_PER_SEQUENCE symbol-counts index.
template <class S>
struct SymbolCountsIndex {
    static_assert(is_index_chunked_storage_for<S, SequenceIndexType::INLINE_PER_SEQUENCE>);
    static constexpr SequenceIndexType index_type = SequenceIndexType::INLINE_PER_SEQUENCE;
    static constexpr std::string_view  index_name = SYMBOL_COUNTS_INDEX_NAME;
    using storage_type = S;

    template <sequence_alphabet Alphabet>
    static void build(
            S& storage,
            bioseq::storage::basic_lazy_sequence_view<Alphabet> seq) {
        auto counts = tally_symbol_counts(seq);                      // std::array<int64,N=2^bits_per_char>
        storage.resize(counts.size() * sizeof(std::int64_t));        // ONE chunk, N*sizeof(int64) bytes << MAX
        storage.template write_to_each<std::int64_t>([&counts](std::size_t c, std::int64_t* buf, std::size_t cap){
            DEBUG_ASSERT(c == 0 && cap == counts.size(), "symbol_counts: one inline chunk");
            std::copy(counts.begin(), counts.end(), buf); return c; });
    }
    template <sequence_alphabet Alphabet>
    static void rebuild(
            S& storage,
            bioseq::storage::basic_lazy_sequence_view<Alphabet> seq) {
        storage.clear_entity();
        build(storage, seq);
    }

    /// SQL-facing read. Returns nullopt when the index is ABSENT for this
    /// sequence (chunk_byte_size()==0) so the caller recomputes.
    static std::optional<std::int64_t> read_count(S& storage, std::size_t rank) {
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
template <> struct index_descriptor<SymbolCountsIndex> {
    static constexpr SequenceIndexType index_type = SequenceIndexType::INLINE_PER_SEQUENCE;
    static constexpr std::string_view  index_name = SYMBOL_COUNTS_INDEX_NAME;
    static constexpr bool allows_update_on_insert = true;   // cheap INLINE tally; default on-insert
};
static_assert(requires { index_descriptor<SymbolCountsIndex>::index_name; },
              "SymbolCountsIndex index_descriptor must be exported");
}
