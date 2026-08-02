/** Canonical generator input for exact minimizer SIMD foundations. */
#ifndef BIOSEQ_HASHER_BASE_STATIC_FACADE
module;
#include <hwy/highway.h>

export module bioseq.highway.sources.hasher_base;

import std;
import bioseq.common.sequan_compat;
import bioseq.sequences;
import bioseq.highway.sources.sequence_ops;
import bioseq.highway.sources.simd_utils;
#endif

export namespace bioseq::simd {
namespace [[bioseq::simd]] target {
namespace hn = hwy::HWY_NAMESPACE;

template<
    typename Derived,
    sequence_alphabet Alphabet,
    typename Input>
struct hasher_base {
    struct traits
        : public compact_sequence_range<Alphabet>::
              compact_sequence_range_base::traits {
        using input_t = Input;
        using hasher_return_t = std::pair<input_t, sequence_strand_mode>;

        static constexpr std::size_t max_kmer_length =
            sizeof(Input) * 8u
            / compact_sequence_range<Alphabet>::
                  compact_sequence_range_base::traits::bits_per_char;

        template <typename InputLike>
        [[gnu::always_inline]] static constexpr hasher_return_t
        select_min_hash_strand(
            InputLike&& forward,
            InputLike&& reverse_complement) noexcept
        {
            if (forward == reverse_complement) [[unlikely]] {
                return std::make_pair(
                    static_cast<input_t>(forward), SEQUENCE_STRAND_EQUAL);
            }
            if (forward < reverse_complement) {
                return std::make_pair(
                    static_cast<input_t>(forward), SEQUENCE_STRAND_FORWARD);
            }
            return std::make_pair(
                static_cast<input_t>(reverse_complement),
                SEQUENCE_STRAND_REVERSE_COMPLEMENT);
        }
    };
};

template<
    template<typename> typename Hasher,
    typename Alphabet,
    typename Input = std::uint64_t,
    typename ScalarHasher =
        Hasher<Alphabet>::template scalar_hash<Input>>
concept supports_scalar_hash = sequence_alphabet<Alphabet>
    && requires (ScalarHasher& hasher, Input&& incoming) {
        { hasher.next(
            std::move(incoming), std::move(incoming)) }
            -> std::same_as<typename ScalarHasher::traits::hasher_return_t>;
    };

template<
    typename Derived,
    sequence_alphabet Alphabet,
    typename Input,
    typename D>
struct simd_hasher_base
    : hasher_base<Derived, Alphabet, Input> {
    struct traits
        : public simd_hasher_base<Derived, Alphabet, Input, D>::
              hasher_base::traits {
        using input_d_t = D;
        using input_vec_t = hn::Vec<input_d_t>;
        using hasher_return_t = input_vec_t;
    };
};

template<
    template<typename> typename Hasher,
    typename Alphabet,
    typename Input = std::uint64_t,
    typename D = hn::ScalableTag<Input>,
    typename SimdHasher =
        Hasher<Alphabet>::template simd_hash<Input, D>>
concept supports_simd_hash = sequence_alphabet<Alphabet>
    && requires (SimdHasher& hasher, D& d, hn::Vec<D>&& incoming) {
        { hasher.next(
            d, std::move(incoming), std::move(incoming)) }
            -> std::same_as<typename SimdHasher::traits::hasher_return_t>;
    };

// DOC0286 §10.2 — minimap2-compatible nt4 normalization. SeqAn nucleotide
// ranks are alphabet-specific (dna5: A=0,C=1,G=2,N=3,T=4), whereas minimap2
// maps A/C/G/T to 0..3 and ambiguity to sentinel 4.
inline constexpr std::uint8_t kNt4Sentinel = 4u;

[[nodiscard]] constexpr std::uint8_t char_to_nt4(char base) noexcept {
    switch (base) {
        case 'A': case 'a': return 0u;
        case 'C': case 'c': return 1u;
        case 'G': case 'g': return 2u;
        case 'T': case 't': return 3u;
        default: return kNt4Sentinel;
    }
}

template <sequence_alphabet Alphabet>
[[nodiscard]] constexpr auto make_nt4_lut() noexcept {
    std::array<std::uint8_t, seqan3::alphabet_size<Alphabet>> lut{};
    for (std::size_t rank = 0u;
         rank < seqan3::alphabet_size<Alphabet>;
         ++rank) {
        Alphabet letter{};
        letter.assign_rank(
            static_cast<seqan3::alphabet_rank_t<Alphabet>>(rank));
        lut[rank] = char_to_nt4(letter.to_char());
    }
    return lut;
}

template <sequence_alphabet Alphabet>
inline constexpr auto nt4_lut_v = make_nt4_lut<Alphabet>();

template <sequence_alphabet Alphabet>
[[nodiscard, gnu::always_inline]] constexpr std::uint8_t
nt4_normalize(std::uint8_t rank) noexcept {
    return nt4_lut_v<Alphabet>[rank];
}

} // namespace target
} // namespace bioseq::simd
