/**
 * @file highway_minimap_int_kernel.cppm
 * @brief Fixed-target Highway minimap2 integer hasher.
 *
 * @details Applies minimap2's width-clamped seven-step permutation to canonical k-mer vectors and
 * forwards strand and palindrome state from the lexicographic rolling hasher.
 */
module;
#include <hwy/highway.h>

export module bioseq.highway.sources.hasher_minimap_int;

import std;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.sequences.kmers.hashers.base;
import bioseq.sequences.kmers.hashers.minimap_int;
import bioseq.highway.sources.hasher_base;
import bioseq.highway.sources.hasher_lexic;
import bioseq.highway.sources.simd_utils;

export namespace bioseq::simd {
namespace [[bioseq::simd]] target {
namespace hn = hwy::HWY_NAMESPACE;

/**
 * @brief Adds a fixed-target minimap2 integer permutation to the lexicographic hasher.
 * @tparam p_alphabet Sequence alphabet processed independently in each vector lane.
 */
template <sequence_alphabet p_alphabet>
struct minimap_int_hasher : ::bioseq::simd::minimap_int_hasher<p_alphabet> {
  public:
    /** @brief Lexicographic SIMD hasher that owns canonical rolling state. */
    using base_hasher_type = lexic_hasher<p_alphabet>;

    /**
     * @brief Maintains a rolling canonical k-mer and emits its permuted SIMD hash.
     * @tparam p_input_type Unsigned lane type holding a packed k-mer code.
     * @tparam p_simd_tag Highway descriptor for the generated target.
     */
    template <typename p_input_type, typename p_simd_tag>
    class simd_hash : public simd_hasher_base<p_alphabet, p_input_type, p_simd_tag> {
      public:
        /** @brief Fixed-target SIMD base supplying vector traits. */
        using base_type = simd_hasher_base<p_alphabet, p_input_type, p_simd_tag>;
        /** @brief Lane, mask, and alphabet limits for this specialization. */
        using traits = typename base_type::traits;

      private:
        typename base_hasher_type::template simd_hash<p_input_type, p_simd_tag> base_hasher_;
        const typename traits::input_vector_type mask_;

      public:
        /**
         * @brief Initializes the lexicographic state and k-mer mask.
         * @param[in] simd_tag Highway descriptor selecting the fixed-target operations.
         * @param[in] kmer_length Positive k-mer width representable by `p_input_type`.
         */
        explicit simd_hash(const typename traits::input_simd_tag_type& simd_tag,
                           std::size_t kmer_length)
            : base_hasher_(simd_tag, kmer_length),
              mask_(hn::Set(simd_tag,
                            (static_cast<typename traits::input_type>(1) << (2u * kmer_length)) -
                                1u)) {}

        /**
         * @brief Clamps one permutation shift to the selected lane width.
         * @tparam p_offset Requested compile-time shift distance.
         */
        template <std::size_t p_offset>
        static constexpr std::size_t k_shift = std::min(p_offset, sizeof(p_input_type) * 8u - 1u);

        /**
         * @brief Advances each rolling k-mer and applies the minimap2 integer permutation.
         * @param[in] simd_tag Highway descriptor selecting the fixed-target operations.
         * @param[in] incoming Alphabet rank entering the right edge of each lane's k-mer.
         * @param[in] outgoing Alphabet rank leaving the left edge of each lane's k-mer.
         * @return Per-lane permuted canonical hashes masked to the k-mer width.
         */
        HWY_INLINE typename traits::hasher_return_type
        next(const typename traits::input_simd_tag_type& simd_tag,
             typename traits::input_vector_type incoming,
             typename traits::input_vector_type outgoing) {
            auto key = base_hasher_.next(simd_tag, std::move(incoming), std::move(outgoing));
            key = hn::And(hn::Add(hn::Not(key), hn::ShiftLeft<k_shift<21>>(key)), mask_);
            key = hn::Xor(key, hn::ShiftRight<k_shift<24>>(key));
            key = hn::And(hn::Add(hn::Add(key, hn::ShiftLeft<k_shift<3>>(key)),
                                  hn::ShiftLeft<k_shift<8>>(key)),
                          mask_);
            key = hn::Xor(key, hn::ShiftRight<k_shift<14>>(key));
            key = hn::And(hn::Add(hn::Add(key, hn::ShiftLeft<k_shift<2>>(key)),
                                  hn::ShiftLeft<k_shift<4>>(key)),
                          mask_);
            key = hn::Xor(key, hn::ShiftRight<k_shift<28>>(key));
            return hn::And(hn::Add(key, hn::ShiftLeft<k_shift<31>>(key)), mask_);
        }

        /**
         * @brief Forwards the canonical state's palindrome classification.
         * @param[in] simd_tag Highway descriptor forwarded to the lexicographic hasher.
         * @return Per-lane mask identifying equal forward and reverse-complement codes.
         */
        [[nodiscard]] HWY_INLINE hn::Mask<p_simd_tag>
        palindrome_mask(const p_simd_tag& simd_tag) const {
            return base_hasher_.palindrome_mask(simd_tag);
        }

        /**
         * @brief Forwards the canonical state's strict reverse-strand classification.
         * @param[in] simd_tag Highway descriptor forwarded to the lexicographic hasher.
         * @return Per-lane mask identifying a strictly smaller reverse-complement code.
         */
        [[nodiscard]] HWY_INLINE hn::Mask<p_simd_tag>
        reverse_strand_mask(const p_simd_tag& simd_tag) const {
            return base_hasher_.reverse_strand_mask(simd_tag);
        }
    };

    /**
     * @brief Applies the scalar-equivalent seven-step integer mix to every SIMD lane.
     * @tparam p_simd_tag Highway descriptor whose lane type determines shift clamping.
     * @param[in] simd_tag Fixed-target Highway descriptor.
     * @param[in] key Per-lane canonical k-mer codes.
     * @param[in] mask Per-lane mask limiting results to the k-mer bit width.
     * @return Per-lane mixed hashes ready for two-stack window selection.
     */
    template <class p_simd_tag>
    [[nodiscard, gnu::always_inline]] static HWY_INLINE hn::Vec<p_simd_tag>
    scramble_simd(p_simd_tag simd_tag, hn::Vec<p_simd_tag> key, hn::Vec<p_simd_tag> mask) {
        using lane_type = hn::TFromD<p_simd_tag>;
        constexpr size_t k_max_storage_shift = sizeof(lane_type) * 8 - 1;
        // Clamps permutation shifts to the lane bit width.
        constexpr auto clamp = [](size_t shift) constexpr -> size_t {
            return shift > k_max_storage_shift ? k_max_storage_shift : shift;
        };
        key = hn::And(hn::Add(hn::Not(key), hn::ShiftLeft<clamp(21)>(key)), mask);
        key = hn::Xor(key, hn::ShiftRight<clamp(24)>(key));
        key = hn::And(
            hn::Add(hn::Add(key, hn::ShiftLeft<clamp(3)>(key)), hn::ShiftLeft<clamp(8)>(key)),
            mask);
        key = hn::Xor(key, hn::ShiftRight<clamp(14)>(key));
        key = hn::And(
            hn::Add(hn::Add(key, hn::ShiftLeft<clamp(2)>(key)), hn::ShiftLeft<clamp(4)>(key)),
            mask);
        key = hn::Xor(key, hn::ShiftRight<clamp(28)>(key));
        key = hn::And(hn::Add(key, hn::ShiftLeft<clamp(31)>(key)), mask);
        (void)simd_tag;
        return key;
    }
};
static_assert(::bioseq::simd::supports_scalar_hash<minimap_int_hasher, seq::dna4>);
static_assert(supports_simd_hash<minimap_int_hasher, seq::dna4>);

} // namespace target
} // namespace bioseq::simd
