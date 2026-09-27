/**
 * @file base.cppm
 * @brief Shared alphabet and strand handling for minimizer hashes.
 *
 * Chooses the smaller forward or reverse-complement code and converts
 * alphabet ranks to minimap2's A/C/G/T codes.
 */
module;

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

export module bioseq.sequences.kmers.hashers.base;

import bioseq.common.seqan_adapter;
import bioseq.sequences;

export namespace bioseq::simd {

/** @brief Supplies alphabet limits and strand selection for scalar k-mer hashes. */
template <sequence_alphabet p_alphabet, typename p_input_type> struct hasher_base {
    /** @brief Alphabet properties, scalar word type and hash result type. */
    struct traits : public compact_sequence_range<p_alphabet>::compact_sequence_range_base::traits {
        /** @brief Scalar word that stores one rolling k-mer code. */
        using input_type = p_input_type;
        /** @brief Selected k-mer, code and its strand, including an equal-strand result. */
        using hasher_return_type = std::pair<input_type, sequence_strand_mode>;

        /** @brief Longest k-mer length storable in `input_type` at the alphabet bit width. */
        static constexpr std::size_t k_max_kmer_length =
            sizeof(p_input_type) * 8u /
            compact_sequence_range<p_alphabet>::compact_sequence_range_base::traits::bits_per_char;

        /**
         * @brief Returns the smaller strand code and identifies which strand it  came from.
         * Equal codes return sequence_strand_mode::equal.
         */
        template <typename p_input_like>
        [[gnu::always_inline]] static constexpr hasher_return_type
        select_min_hash_strand(p_input_like&& forward, p_input_like&& reverse_complement) noexcept {
            if (forward == reverse_complement) [[unlikely]] {
                return std::make_pair(static_cast<input_type>(forward),
                                      sequence_strand_mode::equal);
            }
            if (forward < reverse_complement) {
                return std::make_pair(static_cast<input_type>(forward),
                                      sequence_strand_mode::forward);
            }
            return std::make_pair(static_cast<input_type>(reverse_complement),
                                  sequence_strand_mode::reverse_complement);
        }
    };
};

/** @brief Requires a scalar hasher whose next method returns its declared result type. */
template <template <typename> typename p_hasher_template,
          typename p_alphabet,
          typename p_input_type = std::uint64_t,
          typename p_scalar_hasher_type =
              p_hasher_template<p_alphabet>::template scalar_hash<p_input_type>>
concept supports_scalar_hash =
    sequence_alphabet<p_alphabet> &&
    requires(p_scalar_hasher_type& hasher, p_input_type&& incoming) {
        {
            hasher.next(std::move(incoming), std::move(incoming))
        } -> std::same_as<typename p_scalar_hasher_type::traits::hasher_return_type>;
    };

/** @brief Marks symbols outside minimap2's A/C/G/T code space. */
inline constexpr std::uint8_t k_nt4_sentinel = 4u;

/**
 * @brief Converts A, C, G and T to minimap2 codes 0, 1, 2 and 3.
 * Accepts either letter case. Other characters return k_nt4_sentinel.
 */
[[nodiscard]] constexpr std::uint8_t char_to_nt4(char base) noexcept {
    switch (base) {
    case 'A':
    case 'a':
        return 0u;
    case 'C':
    case 'c':
        return 1u;
    case 'G':
    case 'g':
        return 2u;
    case 'T':
    case 't':
        return 3u;
    default:
        return k_nt4_sentinel;
    }
}

/** @brief Maps every alphabet rank to its minimap2 code or the unsupported-symbol sentinel */
template <sequence_alphabet p_alphabet> [[nodiscard]] constexpr auto make_nt4_lut() noexcept {
    std::array<std::uint8_t, seqan3::alphabet_size<p_alphabet>> lut{};
    for (std::size_t rank = 0u; rank < seqan3::alphabet_size<p_alphabet>; ++rank) {
        p_alphabet letter{};
        letter.assign_rank(static_cast<seqan3::alphabet_rank_t<p_alphabet>>(rank));
        lut[rank] = char_to_nt4(letter.to_char());
    }
    return lut;
}

/** @brief Compile-time lookup from alphabet ranks to minimap2 codes. */
template <sequence_alphabet p_alphabet>
inline constexpr auto k_nt4_lut = make_nt4_lut<p_alphabet>();

/**
 * @brief Converts a valid alphabet rank to its minimap2 cods or the sentinel.
 * @pre rank is less than seqan3::alphabet_size<p_alphabet>.
 */
template <sequence_alphabet p_alphabet>
[[nodiscard, gnu::always_inline]] constexpr std::uint8_t nt4_normalize(std::uint8_t rank) noexcept {
    return k_nt4_lut<p_alphabet>[rank];
}

} // namespace bioseq::simd
