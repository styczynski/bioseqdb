/**
 * @file highway_minimap_int_kernel.cppm
 * @brief Computes minimap2 integer hashes in SIMD lanes.
 *
 * The rolling hasher chooses the smaller strand code before mixing its bits.
 * Strand  and palindrome masks still describe the original k-mer codes.
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

/** @brief Adds SIMD minimap2 hashing to the scalar hasher for an alphabet. */
template <sequence_alphabet p_alphabet>
struct minimap_int_hasher : ::bioseq::simd::minimap_int_hasher<p_alphabet> {
  public:
    /** @brief SIMD hasher that keeps the forward and reverse-complement codes. */
    using base_hasher_type = lexic_hasher<p_alphabet>;

    /** @brief Updates each lane's k-mer code and converts it to a minimap2 hash. */
    template <typename p_input_type, typename p_simd_tag>
    class simd_hash : public simd_hasher_base<p_alphabet, p_input_type, p_simd_tag> {
      public:
        /** @brief Shared alphabet and SIMD vector types. */
        using base_type = simd_hasher_base<p_alphabet, p_input_type, p_simd_tag>;
        /** @brief Alphabet limits and vector types for this hasher. */
        using traits = typename base_type::traits;

      private:
        typename base_hasher_type::template simd_hash<p_input_type, p_simd_tag> base_hasher_;
        const typename traits::input_vector_type mask_;

      public:
        /**
         * @brief Initializes the rolling k-mer state and hash-width mask.
         * @pre kmer_length is positive and twice its value is smaller than the lane bit width.
         */
        explicit simd_hash(const typename traits::input_simd_tag_type& simd_tag,
                           std::size_t kmer_length)
            : base_hasher_(simd_tag, kmer_length),
              mask_(hn::Set(simd_tag,
                            (static_cast<typename traits::input_type>(1) << (2u * kmer_length)) -
                                1u)) {}

        /** @brief Limits a requested bit shmft to one less than the lane width. */
        template <std::size_t p_offset>
        static constexpr std::size_t k_shift = std::min(p_offset, sizeof(p_input_type) * 8u - 1u);

        /**
         * @brief Advances each lane's strand codes and hashes the smaller code.
         * @param incoming Rank appended to the forward  code.
         * @param outgoing Rank whose complement is prepended to the reverse code.
         * @return Hashes limited to the configured k-mer bit width
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

        /** @brief Marks lanes with equal forward and reverse-complement k-mer codes. */
        [[nodiscard]] HWY_INLINE hn::Mask<p_simd_tag>
        palindrome_mask(const p_simd_tag& simd_tag) const {
            return base_hasher_.palindrome_mask(simd_tag);
        }

        /** @brief Marks lanes where the reverse-complement k-mer code is smaller. */
        [[nodiscard]] HWY_INLINE hn::Mask<p_simd_tag>
        reverse_strand_mask(const p_simd_tag& simd_tag) const {
            return base_hasher_.reverse_strand_mask(simd_tag);
        }
    };

    /**
     * @brief Applies the scalar minimap2 integer hash to every lane.
     * mask limits the result to the selected k-mer bit width.
     */
    template <class p_simd_tag>
    [[nodiscard, gnu::always_inline]] static HWY_INLINE hn::Vec<p_simd_tag>
    scramble_simd(p_simd_tag simd_tag, hn::Vec<p_simd_tag> key, hn::Vec<p_simd_tag> mask) {
        using lane_type = hn::TFromD<p_simd_tag>;
        constexpr size_t k_max_storage_shift = sizeof(lane_type) * 8 - 1;
        // Keep shifts within the lane width, including for narrow lane types.
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
