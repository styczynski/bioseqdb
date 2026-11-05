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
#include <stdexcept>
export module bioseq.indexing.symbol_counts;
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.sequences;
import bioseq.sequences.quality_summary;
import bioseq.sequences.cigar;
import bioseq.sequences.simd;
import bioseq.simd.utils;
import bioseq.common.checked_arithmetic;
static_assert(__cplusplus >= 202302L, "C++23-or-later");

export namespace bioseq::indexing {

/**
 * @brief Counts checked native ranks in bounded windows without publishing partial counts.
 * @param sequence Source view retained by the caller through this synchronous traversal.
 * @return Rank-indexed counts, or the first source/rank failure with its logical position.
 */
template <sequence_alphabet p_alphabet>
    requires (!bioseq::cigar_operation_alphabet<p_alphabet>)
[[nodiscard]] auto tally_symbol_counts(
        bioseq::storage::lazy_sequence_view<p_alphabet> sequence) {
    using traits = typename bioseq::storage::lazy_sequence_view<
        p_alphabet>::traits;
    constexpr std::size_t rank_count =
        std::size_t{1} << traits::k_logical_bits_per_symbol;
    using counts_type = std::array<std::int64_t, rank_count>;
    using error_type = bioseq::sequences::sequence_conversion_error;
    using error_code = bioseq::sequences::sequence_conversion_error_code;
    using result_type = std::expected<counts_type, error_type>;
    const auto source = sequence.template borrow_rank_source<false>();
    if (source.symbol_count > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())
        || source.symbol_count > (std::numeric_limits<std::size_t>::max)())
        return result_type{std::unexpected(error_type{error_code::size_overflow})};
    if (source.read_at == nullptr)
        return result_type{std::unexpected(error_type{error_code::source_read_failed, 0u})};
    counts_type counts{};
    std::array<std::uint8_t, bioseq::simd::sequence_rank_io_chunk_capacity> scratch{};
    const auto length = static_cast<std::size_t>(source.symbol_count);
    for (std::size_t position = 0u; position < length;) {
        const auto count = std::min(scratch.size(), length - position);
        const auto ranks = source.read_checked(position, count, scratch);
        if (!ranks)
            return result_type{std::unexpected(error_type{error_code::source_read_failed, position})};
        for (std::size_t offset = 0u; offset < count; ++offset) {
            const auto rank = (*ranks)[offset];
            if (rank >= seqan3::alphabet_size<p_alphabet>)
                return result_type{std::unexpected(error_type{
                    .code = error_code::invalid_source_rank,
                    .input_position = position + offset,
                    .source_rank = rank})};
            ++counts[rank];
        }
        position += count;
    }
    return result_type{counts};
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

    /** Build the tagged quality payload using the existing single-pass histogram. */
    template<bioseq::sequences::quality_alphabet Alphabet>
    static void build(p_storage& storage, bioseq::storage::lazy_sequence_view<Alphabet> sequence,
            std::uint64_t source_epoch) {
        using namespace bioseq::sequences;
        const auto counts = tally_symbol_counts(sequence);
        if (!counts) { storage.index_error("symbol_counts: invalid quality source"); return; }
        const auto summary = summarize_quality_histogram<Alphabet>(*counts);
        if (!summary) { storage.index_error("symbol_counts: invalid quality histogram"); return; }
        const auto bytes = encode_quality_index(quality_index_payload<Alphabet>{*summary, source_epoch, *counts});
        if (!bytes) { storage.index_error("symbol_counts: invalid quality source epoch or summary"); return; }
        storage.resize(bytes->size());
        storage.template write_to_each<std::int64_t>([&bytes](std::size_t chunk,
                std::int64_t* output, std::size_t capacity) {
            if (chunk != 0 || capacity * sizeof(std::int64_t) != bytes->size())
                throw std::runtime_error("symbol_counts quality output geometry mismatch");
            std::copy(bytes->begin(), bytes->end(), reinterpret_cast<std::byte*>(output));
            return chunk;
        });
    }

    /** Fixed-size quality records are replaced in full after validating the new source. */
    template<bioseq::sequences::quality_alphabet Alphabet>
    static void rebuild(p_storage& storage, bioseq::storage::lazy_sequence_view<Alphabet> sequence,
            std::uint64_t source_epoch) {
        build(storage, sequence, source_epoch);
    }

    template<bioseq::sequences::quality_alphabet Alphabet>
    [[nodiscard]] static std::expected<std::optional<bioseq::sequences::quality_index_payload<Alphabet>>,
        bioseq::sequences::quality_error> read_quality(p_storage& storage,
            std::uint64_t epoch, std::uint64_t length) {
        const auto bytes = storage.chunk_byte_size();
        if (!bytes) return std::optional<bioseq::sequences::quality_index_payload<Alphabet>>{};
        const auto* data = reinterpret_cast<const std::byte*>(storage.template read_chunk<std::int64_t>());
        auto value = bioseq::sequences::decode_quality_index<Alphabet>({data, bytes}, epoch, length);
        if (!value) {
            if (value.error() == bioseq::sequences::quality_error::stale_source)
                return std::optional<bioseq::sequences::quality_index_payload<Alphabet>>{};
            return std::unexpected(value.error());
        }
        return std::optional{*value};
    }

    template<bioseq::sequences::quality_alphabet Alphabet>
    [[nodiscard]] static auto read_summary(p_storage& storage, std::uint64_t epoch, std::uint64_t length)
        -> std::expected<std::optional<bioseq::sequences::quality_summary>, bioseq::sequences::quality_error> {
        const auto value = read_quality<Alphabet>(storage, epoch, length);
        if (!value) return std::unexpected(value.error());
        if (!*value) return std::nullopt;
        return (**value).summary;
    }

    template<bioseq::sequences::quality_alphabet Alphabet>
    [[nodiscard]] static auto read_count(p_storage& storage, std::size_t rank,
            std::uint64_t epoch, std::uint64_t length)
        -> std::expected<std::optional<std::int64_t>, bioseq::sequences::quality_error> {
        if (rank >= seqan3::alphabet_size<Alphabet>)
            return std::unexpected(bioseq::sequences::quality_error::invalid_rank);
        const auto value = read_quality<Alphabet>(storage, epoch, length);
        if (!value) return std::unexpected(value.error());
        if (!*value) return std::nullopt;
        return (**value).counts[rank];
    }
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
        requires (!bioseq::cigar_operation_alphabet<Alphabet> && !bioseq::sequences::quality_alphabet<Alphabet>)
    static void build(p_storage& storage, bioseq::storage::lazy_sequence_view<Alphabet> sequence) {
        const auto counts = tally_symbol_counts(sequence);
        if (!counts) {
            storage.index_error("symbol_counts: native source read, rank, or length is invalid");
            return;
        }
        write_counts(storage, *counts);
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

    /** Validate all native source counts before replacing the existing stored histogram. */
    template<sequence_alphabet Alphabet>
        requires (!bioseq::cigar_operation_alphabet<Alphabet> && !bioseq::sequences::quality_alphabet<Alphabet>)
    static void rebuild(p_storage& storage, bioseq::storage::lazy_sequence_view<Alphabet> sequence) {
        const auto counts = tally_symbol_counts(sequence);
        if (!counts) {
            storage.index_error("symbol_counts: native source read, rank, or length is invalid");
            return;
        }
        storage.clear_entity();
        write_counts(storage, *counts);
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
        // QSC1 has a header plus a power-of-two histogram. Its two sizes are
        // disjoint from every legacy raw histogram; never reinterpret its header.
        if (bytes == 64 + 8 * bioseq::sequences::quality_histogram_slots<seqan3::phred42>
            || bytes == 64 + 8 * bioseq::sequences::quality_histogram_slots<seqan3::phred68solexa>) {
            storage.index_error("symbol_counts: quality payload requires a typed source epoch and length");
            return std::nullopt;
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
