/**
 * @file storage/lazy_sequence_parts/transforms.cppm
 * @brief Bounded logical-rank transforms, comparison, and hashing.
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

export namespace bioseq::storage {

/** @brief Stream a logical subrange in the requested orientation without flattening it. */
template <sequence_transform p_transform, sequence_alphabet p_alphabet, class p_output_writer>
void stream_transformed_ranks(lazy_sequence_view<p_alphabet> view,
                              std::uint64_t from,
                              std::uint64_t to,
                              p_output_writer& output) {
    if (from > to || to > view.bp_size() ||
        to > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
        throw std::invalid_argument("transformed sequence stream coordinates are unsupported");
    }
    std::array<std::uint8_t, 256> scratch{};
    auto append = [&output](std::span<const std::uint8_t> ranks) {
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

    if constexpr (is_reversed(p_transform)) {
        std::uint64_t position = to;
        while (position > from) {
            const auto count =
                static_cast<std::size_t>(std::min<std::uint64_t>(scratch.size(), position - from));
            const std::uint64_t chunk_from = position - count;
            view.template copy_ranks<false, true>(std::span<std::uint8_t>{scratch.data(), count},
                                                  static_cast<std::size_t>(chunk_from),
                                                  static_cast<std::size_t>(position));
            if constexpr (is_complemented(p_transform)) {
                static_assert(lazy_sequence_detail::complementable_sequence_alphabet<p_alphabet>);
                for (std::size_t index = 0u; index < count; ++index) {
                    scratch[index] =
                        lazy_sequence_traits<p_alphabet>::letter_complement(scratch[index]);
                }
            }
            if (!append(std::span<const std::uint8_t>{scratch.data(), count})) {
                return;
            }
            position = chunk_from;
        }
    } else {
        std::uint64_t position = from;
        while (position < to) {
            const auto count =
                static_cast<std::size_t>(std::min<std::uint64_t>(scratch.size(), to - position));
            view.template copy_ranks<false, false>(std::span<std::uint8_t>{scratch.data(), count},
                                                   static_cast<std::size_t>(position),
                                                   static_cast<std::size_t>(position + count));
            if constexpr (is_complemented(p_transform)) {
                static_assert(lazy_sequence_detail::complementable_sequence_alphabet<p_alphabet>);
                for (std::size_t index = 0u; index < count; ++index) {
                    scratch[index] =
                        lazy_sequence_traits<p_alphabet>::letter_complement(scratch[index]);
                }
            }
            if (!append(std::span<const std::uint8_t>{scratch.data(), count})) {
                return;
            }
            position += count;
        }
    }
}

/** @brief Lexicographically compare two logical-rank sequences. */
template <sequence_alphabet p_alphabet>
[[nodiscard]] int compare_lazy_sequences(lazy_sequence_view<p_alphabet> left,
                                         lazy_sequence_view<p_alphabet> right) {
    struct comparison_writer {
        typename lazy_sequence_view<p_alphabet>::rank_iterator cursor;
        typename lazy_sequence_view<p_alphabet>::rank_iterator end;
        int result{};

        bool append_ranks(std::span<const std::uint8_t> ranks) {
            if (result != 0) {
                return false;
            }
            for (const std::uint8_t rank : ranks) {
                if (cursor == end) {
                    result = 1;
                    return false;
                }
                const std::uint8_t other = *cursor;
                if (rank < other) {
                    result = -1;
                    return false;
                }
                if (rank > other) {
                    result = 1;
                    return false;
                }
                ++cursor;
            }
            return true;
        }
    } writer{right.begin(), right.end(), 0};
    stream_transformed_ranks<sequence_transform::identity>(left, 0u, left.bp_size(), writer);
    if (writer.result != 0) {
        return writer.result;
    }
    return writer.cursor == writer.end ? 0 : -1;
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
    stream_transformed_ranks<sequence_transform::identity>(view, 0u, view.bp_size(), writer);
    return writer.finish(view.bp_size());
}

/// Copy ranks through the bounded rank-window reader into contiguous output.
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
