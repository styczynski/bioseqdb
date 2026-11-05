/**
 * @file storage/lazy_sequence_parts/transforms.cppm
 * @brief Transforms, compares, and hashes lazy sequence ranks in bounded chunks.
 */
export module bioseq.storage.lazy_sequence:transforms;

import std;
import bioseq.storage.lazy_sequence_view;
import bioseq.common.seqan_adapter;
import bioseq.storage.coordinates;
import bioseq.storage.scanner;
import bioseq.storage.size;
import bioseq.sequences;
import bioseq.sequences.rank_pack;
import bioseq.simd.utils;

export namespace bioseq::storage {

/**
 * Stream [from, to) through bounded checked native-rank reads.
 * Writer rejection stops normally; source failure is returned without publishing a partial result.
 * Reverse and complement operate only on the current tile, preserving the source owner.
 */
template <sequence_transform p_transform, sequence_alphabet p_alphabet, class p_output_writer>
[[nodiscard]] std::expected<void, rank_window_read_failure>
stream_transformed_ranks(lazy_sequence_view<p_alphabet> view,
                         std::uint64_t from,
                         std::uint64_t to,
                         p_output_writer& output) {
    if constexpr (bioseq::sequences::quality_alphabet<p_alphabet> && is_complemented(p_transform)) {
        constexpr auto normalized = is_reversed(p_transform)
            ? sequence_transform::reversed : sequence_transform::identity;
        return stream_transformed_ranks<normalized>(view, from, to, output);
    }
    if (from > to || to > view.bp_size() ||
        to > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
        return std::unexpected(rank_window_read_failure::invalid_range);
    }
    std::array<std::uint8_t, 256> scratch{};
    const auto source = view.template borrow_rank_source<false>();
    const auto append = [&output](std::span<const std::uint8_t> ranks) {
        if constexpr (std::same_as<decltype(output.append_ranks(ranks)),
                                   bioseq::sequences::rank_pack_status>) {
            return output.append_ranks(ranks) == bioseq::sequences::rank_pack_status::ok;
        } else if constexpr (std::same_as<decltype(output.append_ranks(ranks)), bool>) {
            return output.append_ranks(ranks);
        } else {
            output.append_ranks(ranks);
            return true;
        }
    };
    std::uint64_t consumed = 0u;
    while (consumed < to - from) {
        const auto count = static_cast<std::size_t>(
            std::min<std::uint64_t>(scratch.size(), to - from - consumed));
        const auto chunk_from = is_reversed(p_transform)
            ? to - consumed - count : from + consumed;
        auto ranks = source.read_checked(chunk_from, count, scratch);
        if (!ranks) return std::unexpected(ranks.error());
        if constexpr (is_reversed(p_transform) || is_complemented(p_transform)) {
            if (ranks->data() != scratch.data())
                std::copy(ranks->begin(), ranks->end(), scratch.begin());
            if constexpr (is_reversed(p_transform))
                std::reverse(scratch.begin(), scratch.begin() + count);
            if constexpr (is_complemented(p_transform)) {
                static_assert(lazy_sequence_detail::complementable_sequence_alphabet<p_alphabet>);
                for (std::size_t index = 0u; index < count; ++index) {
                    if (scratch[index] >= seqan3::alphabet_size<p_alphabet>)
                        return std::unexpected(rank_window_read_failure::source_read_failed);
                    scratch[index] = lazy_sequence_traits<p_alphabet>::letter_complement(scratch[index]);
                }
            }
            if (!append(std::span<const std::uint8_t>{scratch.data(), count})) return {};
        } else {
            if (!append(*ranks)) return {};
        }
        consumed += count;
    }
    return {};
}

/** Compares sequences by rank, using length to order equal prefixes. Returns -1, 0, or 1. */
template <sequence_alphabet p_alphabet>
[[nodiscard]] int compare_lazy_sequences(lazy_sequence_view<p_alphabet> left,
                                         lazy_sequence_view<p_alphabet> right,
                                         bool include_resident = false) {
    constexpr std::size_t capacity = bioseq::simd::sequence_rank_io_chunk_capacity;
    std::array<std::uint8_t, capacity> left_scratch{}, right_scratch{};
    // Streaming callers need not support resident probes. Batch sorting can
    // explicitly allow them when borrowing packed words is part of its policy.
    const auto left_source = left.template borrow_rank_source<false>(include_resident);
    const auto right_source = right.template borrow_rank_source<false>(include_resident);
    const auto common = std::min(left.bp_size(), right.bp_size());
    std::uint64_t offset = 0;
    // A resident borrow guarantees immutable words with the same logical ranks.
    // Skip equal complete native words; never use packed byte order as rank order.
    // A padding difference merely declines the shortcut and uses rank comparison.
    if (left_source.resident && right_source.resident) {
        const auto& a = *left_source.resident;
        const auto& b = *right_source.resident;
        if (a.storage_unit_width == b.storage_unit_width && a.bits_per_symbol == b.bits_per_symbol
            && a.leading_symbol_offset == 0 && b.leading_symbol_offset == 0) {
            const auto width = a.storage_unit_width;
            const auto symbols = width * 8u / a.bits_per_symbol;
            const auto words = common / symbols;
            const auto* first = static_cast<const std::uint8_t*>(a.units);
            const auto* second = static_cast<const std::uint8_t*>(b.units);
            std::uint64_t word = 0;
            while (word < words) {
                const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(256, words - word));
                if (std::memcmp(first + word * width, second + word * width, count * width) != 0) {
                    while (word < words && std::memcmp(first + word * width, second + word * width, width) == 0)
                        ++word;
                    break;
                }
                word += count;
            }
            offset = word * symbols;
        }
    }
    // Most comparisons differ near the first rank (or the first unequal
    // resident word). Probe a short bounded tile before decoding a full tile.
    std::size_t tile_capacity = std::min<std::size_t>(16, capacity);
    for (; offset < common;) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(tile_capacity, common - offset));
        const auto a = left_source.read_checked(offset, count, left_scratch);
        if (!a) throw std::runtime_error("lazy sequence comparison source read failed");
        // The readers can share an evictable fragment cache. Preserve the first window.
        if (a->data() != left_scratch.data()) std::copy(a->begin(), a->end(), left_scratch.begin());
        const auto b = right_source.read_checked(offset, count, right_scratch);
        if (!b) throw std::runtime_error("lazy sequence comparison source read failed");
        const auto first = bioseq::simd::first_unequal_sequence_rank(left_scratch.data(), b->data(), count);
        if (first != count) return left_scratch[first] < (*b)[first] ? -1 : 1;
        offset += count;
        tile_capacity = capacity;
    }
    return (left.bp_size() > right.bp_size()) - (left.bp_size() < right.bp_size());
}

namespace lazy_sequence_detail {

struct logical_hash_writer {
    static constexpr std::uint64_t k_fnv_offset_basis = 14695981039346656037ull;
    static constexpr std::uint64_t k_fnv_prime = 1099511628211ull;

    explicit logical_hash_writer(std::uint64_t seed) noexcept : hash(k_fnv_offset_basis ^ seed) {}

    void append_ranks(std::span<const std::uint8_t> ranks) noexcept {
        for (const std::uint8_t rank : ranks) {
            hash ^= rank;
            hash *= k_fnv_prime;
        }
    }

    [[nodiscard]] std::uint64_t finish(std::uint64_t logical_length) noexcept {
        for (unsigned shift = 0u; shift < 64u; shift += 8u) {
            hash ^= static_cast<std::uint8_t>(logical_length >> shift);
            hash *= k_fnv_prime;
        }
        hash ^= hash >> 30u;
        hash *= 0xbf58476d1ce4e5b9ull;
        hash ^= hash >> 27u;
        hash *= 0x94d049bb133111ebull;
        hash ^= hash >> 31u;
        return hash;
    }

    std::uint64_t hash;
};

} // namespace lazy_sequence_detail

template <std::ranges::input_range p_rank_range>
[[nodiscard]] std::uint64_t
hash_logical_ranks(p_rank_range&& ranks, std::uint64_t logical_length, std::uint64_t seed) {
    lazy_sequence_detail::logical_hash_writer writer{seed};
    for (auto&& rank : ranks) {
        const std::uint8_t value = static_cast<std::uint8_t>(rank);
        writer.append_ranks(std::span<const std::uint8_t>{&value, 1u});
    }
    return writer.finish(logical_length);
}

template <sequence_alphabet p_alphabet>
[[nodiscard]] std::uint64_t hash_logical_sequence(lazy_sequence_view<p_alphabet> view,
                                                  std::uint64_t seed) {
    lazy_sequence_detail::logical_hash_writer writer{seed};
    const auto streamed = stream_transformed_ranks<sequence_transform::identity>(view, 0u, view.bp_size(), writer);
    if (!streamed) throw std::runtime_error("lazy sequence hash source read failed");
    return writer.finish(view.bp_size());
}

/// Copies [from, to) into destination through the rank-window reader.
/// p_is_reversed uses reverse-complement coordinates. p_write_reversed
/// reverses the selected output interval again.
template <bool p_is_reversed, bool p_write_reversed = false, sequence_alphabet p_alphabet>
[[gnu::always_inline]] inline void copy_sequence_ranks(lazy_sequence_view<p_alphabet> sequence,
                                                       std::span<std::uint8_t> destination,
                                                       std::size_t from,
                                                       std::size_t to) {
    if (from > to || to > sequence.bp_size() || destination.size() < to - from) {
        abort_lazy_sequence_access("lazy sequence rank range is invalid");
    }
    if (from == to) {
        return;
    }
    const std::size_t length = to - from;
    auto reader = sequence.template rank_reader<p_is_reversed, p_write_reversed, std::uint8_t, 2>();
    const std::uint8_t* ranks =
        reader.read_at(static_cast<sequence_size_t>(from), length, destination);
    if (ranks != destination.data()) {
        std::memcpy(destination.data(), ranks, length);
    }
}

} // namespace bioseq::storage
