/**
 * @file symbol_counts.cppm
 * @brief Counts occurrences of each symbol in a sequence.
 *
 * NO_POSTGRES. The index uses numeric alphabet ranks. PostgreSQL code
 * converts query letters to those ranks before looking up their counts.
 */
module;
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
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
import bioseq.sequences.cigar;
import bioseq.common.checked_arithmetic;
static_assert(__cplusplus >= 202302L, "C++23-or-later");

export namespace bioseq::indexing {

/**
 * @brief Counts each symbol in a lazy sequence.
 * @return Array indexed by the alphabet's numeric symbol ranks.
 */
template <sequence_alphabet p_alphabet>
    requires (!bioseq::cigar_operation_alphabet<p_alphabet>)
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

/** Counts selected by a canonical cursor use the existing lazy-rank slot dimensions. */
template<bioseq::cigar_run_cursor Cursor>
[[nodiscard]] auto tally_symbol_counts(Cursor& cursor) {
    using alphabet = typename Cursor::run_type::alphabet_type;
    using traits = typename bioseq::storage::lazy_sequence_view<alphabet>::traits;
    constexpr std::size_t rank_count = std::size_t{1} << traits::k_logical_bits_per_symbol;
    using counts_type = std::array<std::int64_t, rank_count>;
    counts_type counts{};
    for (;;) {
        auto next = cursor.next();
        if (!next) return std::expected<counts_type, bioseq::cigar_error>{std::unexpected(next.error())};
        if (!*next) return std::expected<counts_type, bioseq::cigar_error>{counts};
        const auto run = **next;
        const auto rank = static_cast<std::size_t>(seqan3::to_rank(run.operation));
        if (run.length == 0u || rank >= seqan3::alphabet_size<alphabet>)
            return std::expected<counts_type, bioseq::cigar_error>{std::unexpected(
                bioseq::cigar_error{bioseq::cigar_error_code::invalid_encoding})};
        const auto combined = bioseq::common::checked_add(
            static_cast<bioseq::storage::sequence_size_t>(counts[rank]),
            static_cast<bioseq::storage::sequence_size_t>(run.length));
        if (!combined || *combined > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()))
            return std::expected<counts_type, bioseq::cigar_error>{std::unexpected(
                bioseq::cigar_error{bioseq::cigar_error_code::length_overflow})};
        counts[rank] = static_cast<std::int64_t>(*combined);
    }
}

/** Borrow a validated resident CIGAR range as a weighted cursor, with no new owning buffer. */
template<bioseq::cigar_operation_alphabet Alphabet>
[[nodiscard]] auto tally_symbol_counts(const compact_sequence_range<Alphabet>& sequence) {
    struct range_cursor {
        using run_type = bioseq::cigar_run_value<Alphabet>;
        typename compact_sequence_range<Alphabet>::run_iterator current;
        typename compact_sequence_range<Alphabet>::run_iterator end;
        /** Return one canonical run and preserve clean exhaustion. */
        [[nodiscard]] std::expected<std::optional<run_type>, bioseq::cigar_error> next() {
            if (current == end) return std::optional<run_type>{};
            return std::optional<run_type>{*current++};
        }
    } cursor{sequence.begin(), sequence.end()};
    return tally_symbol_counts(cursor);
}

/** @brief Stores each sequence's symbol counts in one inline index row. */
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

    /** Write the shared histogram format regardless of the logical source codec. */
    template<std::size_t SlotCount>
    static void write_counts(p_storage& storage, const std::array<std::int64_t, SlotCount>& counts) {
        storage.resize(counts.size() * sizeof(std::int64_t));
        storage.template write_to_each<std::int64_t>(
            [&counts](std::size_t chunk_no, std::int64_t* output, std::size_t capacity) {
                DEBUG_ASSERT(chunk_no == 0 && capacity == counts.size(),
                             "symbol_counts: one inline chunk");
                std::copy(counts.begin(), counts.end(), output);
                return chunk_no;
            });
    }

    /** Count ordinary positional sequences and write the existing inline histogram. */
    template<sequence_alphabet Alphabet>
        requires (!bioseq::cigar_operation_alphabet<Alphabet>)
    static void build(p_storage& storage, bioseq::storage::lazy_sequence_view<Alphabet> sequence) {
        write_counts(storage, tally_symbol_counts(sequence));
    }

    /** Build the same index from canonical weighted operations, propagating source failures. */
    template<bioseq::cigar_run_cursor Cursor>
    [[nodiscard]] static std::expected<void, bioseq::cigar_error> build(p_storage& storage, Cursor& cursor) {
        auto counts = tally_symbol_counts(cursor);
        if (!counts) return std::unexpected(counts.error());
        write_counts(storage, *counts);
        return {};
    }

    /** Build directly from a resident run range without a positional compatibility facade. */
    template<bioseq::cigar_operation_alphabet Alphabet>
    [[nodiscard]] static std::expected<void, bioseq::cigar_error> build(
            p_storage& storage, const compact_sequence_range<Alphabet>& sequence) {
        auto counts = tally_symbol_counts(sequence);
        if (!counts) return std::unexpected(counts.error());
        write_counts(storage, *counts);
        return {};
    }

    /** Rebuild ordinary sequences through their existing rank reader. */
    template<sequence_alphabet Alphabet>
        requires (!bioseq::cigar_operation_alphabet<Alphabet>)
    static void rebuild(p_storage& storage, bioseq::storage::lazy_sequence_view<Alphabet> sequence) {
        storage.clear_entity();
        build(storage, sequence);
    }

    /** Rebuild a weighted stream using the identical serialized histogram. */
    template<bioseq::cigar_run_cursor Cursor>
    [[nodiscard]] static std::expected<void, bioseq::cigar_error> rebuild(p_storage& storage, Cursor& cursor) {
        auto counts = tally_symbol_counts(cursor);
        if (!counts) return std::unexpected(counts.error());
        storage.clear_entity();
        write_counts(storage, *counts);
        return {};
    }

    /** Rebuild a resident RLE value without constructing expanded ranks. */
    template<bioseq::cigar_operation_alphabet Alphabet>
    [[nodiscard]] static std::expected<void, bioseq::cigar_error> rebuild(
            p_storage& storage, const compact_sequence_range<Alphabet>& sequence) {
        auto counts = tally_symbol_counts(sequence);
        if (!counts) return std::unexpected(counts.error());
        storage.clear_entity();
        write_counts(storage, *counts);
        return {};
    }

    /**
     * @brief Reads the stored count for one numeric symbol rank.
     * @return The count, or std::nullopt when the index is absent
     * Reports a storage error for a malformed array or an out-of-range rank.
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
    /// @brief Allows symbol counts to be built when the sequence is inserted.
    static constexpr bool allows_update_on_insert = true;
};
static_assert(
    requires { index_descriptor<symbol_counts_index>::index_name; },
    "symbol_counts_index index_descriptor must be exported");
} // namespace bioseq::indexing
