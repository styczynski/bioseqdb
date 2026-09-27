/**
 * @file minimap_int.cppm
 * @brief Computes scalar minimap2 integer hashes from rolling k-mer codes.
 * The smaller strand code is mixed into a hash while its strand label is kept.
 */
module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>

export module bioseq.sequences.kmers.hashers.minimap_int;

import bioseq.sequences.kmers.hashers.base;
import bioseq.sequences.kmers.hashers.lexic;
import bioseq.common.seqan_adapter;
import bioseq.sequences;

export namespace bioseq::simd {

/** @brief Hashes an alphabet's k-mers with the minimap2 integer mixing function. */
template <sequence_alphabet p_alphabet> struct minimap_int_hasher {
    /** @brief Rolling hasher that selects the smaller strand code. */
    using base_hasher_type = lexic_hasher<p_alphabet>;

    /** @brief Keeps rolling k-mer codes and returns their minimap2 hashes. */
    template <typename p_input_type>
    class scalar_hash : public hasher_base<p_alphabet, p_input_type> {
      public:
        /** @brief Scalar hasher base supplying alphabet limits and strand selection. */
        using base_type = hasher_base<p_alphabet, p_input_type>;
        /** @brief Mask, shift, and alphabet helpers shared by rolling updates. */
        using traits = typename base_type::traits;

        /**
         * @brief Initializes the rolling lexicographic state and k-mer mask.
         * @param[in] kmer_length Positive symbol count representable by `p_input_type`.
         * @pre Twice `kmer_length` is smaller than the bit width of `p_input_type`.
         */
        explicit scalar_hash(std::size_t kmer_length)
            : base_hasher_(kmer_length),
              mask_((static_cast<typename traits::input_type>(1) << (2u * kmer_length)) - 1u) {}

        /** @brief Limits a requested bit shift  to one less than the scalar word width. */
        template <std::size_t p_offset>
        static constexpr std::size_t k_shift = std::min(p_offset, sizeof(p_input_type) * 8u - 1u);

        /**
         * @brief Updates the strand codes and hashes the smaller code.
         * @param incoming Rank appended to the forward code.
         * @param outgoing Rank whose complement is prepended to the reverse code.
         * @return The mixed hash with the strand chosen before mixing.
         */
        constexpr typename traits::hasher_return_type next(typename traits::input_type incoming,
                                                           typename traits::input_type outgoing) {
            auto [key, base_strand] = base_hasher_.next(std::move(incoming), std::move(outgoing));
            key = minimap_int_hasher::template scramble<p_input_type>(key, mask_);
            return std::make_pair(key, base_strand);
        }

      private:
        typename base_hasher_type::template scalar_hash<p_input_type> base_hasher_;
        const typename traits::input_type mask_;
    };

    /**
     * @brief Mixes a packed k-mer code with the minimap2 integer hash function.
     * kmer_mask limits the, result to the chosen hash wldth. Shifts are capped
     * for narrow integer types.
     */
    template <typename p_input_type>
    [[nodiscard]] static constexpr p_input_type scramble(const p_input_type& lex_kmer,
                                                         const p_input_type& kmer_mask) {
        constexpr std::size_t k_max_storage_shift = sizeof(p_input_type) * 8u - 1u;
        // Keep shifts within the scalar word width.
        constexpr auto clamp = [](std::size_t shift) constexpr {
            return shift > k_max_storage_shift ? k_max_storage_shift : shift;
        };
        p_input_type key = lex_kmer;
        key = (~key + (key << clamp(21u))) & kmer_mask;
        key = key ^ (key >> clamp(24u));
        key = ((key + (key << clamp(3u))) + (key << clamp(8u))) & kmer_mask;
        key = key ^ (key >> clamp(14u));
        key = ((key + (key << clamp(2u))) + (key << clamp(4u))) & kmer_mask;
        key = key ^ (key >> clamp(28u));
        key = (key + (key << clamp(31u))) & kmer_mask;
        return key;
    }
};

static_assert(supports_scalar_hash<minimap_int_hasher, seq::dna4>);

} // namespace bioseq::simd
