/**
 * @file base.cppm
 * @brief Target-neutral scalar foundations for minimizer hashers.
 *
 * @details Defines the shared alphabet traits, canonical strand tie policy, and minimap2-compatible
 * nt4 normalization used by scalar and fixed-target hashers.
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

/**
 * @brief Supplies alphabet traits and canonical strand selection for scalar hashers.
 * @tparam p_alphabet Sequence alphabet whose rank and complement rules define each k-mer.
 * @tparam p_input_type Scalar word that stores the rolling k-mer state.
 */
template <sequence_alphabet p_alphabet, typename p_input_type> struct hasher_base {
    /** @brief Describes the compile-time sequence and scalar return-type contract. */
    struct traits : public compact_sequence_range<p_alphabet>::compact_sequence_range_base::traits {
        /** @brief Scalar word that stores one rolling k-mer code. */
        using input_type = p_input_type;
        /** @brief Canonical hash code paired with the winning strand classification. */
        using hasher_return_type = std::pair<input_type, sequence_strand_mode>;

        /** @brief Longest k-mer length storable in `input_type` at the alphabet bit width. */
        static constexpr std::size_t k_max_kmer_length =
            sizeof(p_input_type) * 8u /
            compact_sequence_range<p_alphabet>::compact_sequence_range_base::traits::bits_per_char;

        /**
         * @brief Selects the canonical hash and reports its strand without collapsing a tie.
         * @tparam p_input_like Forwarded scalar state convertible to `input_type`.
         * @param[in] forward Forward-strand k-mer code.
         * @param[in] reverse_complement Reverse-complement k-mer code.
         * @return The smaller code and its strand, or `sequence_strand_mode::equal` when both codes
         * are identical.
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

/**
 * @brief Checks that a hasher template implements the scalar rolling-hash contract.
 * @tparam p_hasher_template Alphabet-parameterized hasher policy.
 * @tparam p_alphabet Sequence alphabet accepted by the policy.
 * @tparam p_input_type Scalar word passed through the rolling update.
 * @tparam p_scalar_hasher_type Concrete scalar hasher specialization to validate.
 */
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
 * @brief Converts one character to minimap2's nt4 code space.
 * @param[in] base ASCII nucleotide character.
 * @return Rank zero through three for A/C/G/T, otherwise `k_nt4_sentinel`.
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

/**
 * @brief Builds an alphabet-rank to minimap2-nt4 lookup table.
 * @tparam p_alphabet Sequence alphabet whose characters are normalized.
 * @return Array indexed by `p_alphabet` rank and containing nt4 codes or the sentinel.
 */
template <sequence_alphabet p_alphabet> [[nodiscard]] constexpr auto make_nt4_lut() noexcept {
    std::array<std::uint8_t, seqan3::alphabet_size<p_alphabet>> lut{};
    for (std::size_t rank = 0u; rank < seqan3::alphabet_size<p_alphabet>; ++rank) {
        p_alphabet letter{};
        letter.assign_rank(static_cast<seqan3::alphabet_rank_t<p_alphabet>>(rank));
        lut[rank] = char_to_nt4(letter.to_char());
    }
    return lut;
}

/**
 * @brief Stores the compile-time minimap2-nt4 lookup for one alphabet.
 * @tparam p_alphabet Sequence alphabet represented by the table indices.
 */
template <sequence_alphabet p_alphabet>
inline constexpr auto k_nt4_lut = make_nt4_lut<p_alphabet>();

/**
 * @brief Normalizes one alphabet rank to minimap2's nt4 code space.
 * @tparam p_alphabet Sequence alphabet that defines `rank`.
 * @param[in] rank Valid rank in `[0, seqan3::alphabet_size<p_alphabet>)`.
 * @return The corresponding nt4 code or `k_nt4_sentinel`.
 */
template <sequence_alphabet p_alphabet>
[[nodiscard, gnu::always_inline]] constexpr std::uint8_t nt4_normalize(std::uint8_t rank) noexcept {
    return k_nt4_lut<p_alphabet>[rank];
}

} // namespace bioseq::simd
