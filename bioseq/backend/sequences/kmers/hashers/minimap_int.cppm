/**
 * @file minimap_int.cppm
 * @brief Target-neutral scalar minimap2 integer hasher.
 *
 * @details Composes canonical lexicographic rolling state with minimap2's width-clamped seven-step
 * integer permutation while retaining the canonical strand result.
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

/**
 * @brief Defines the scalar minimap2 integer hash policy for one sequence alphabet.
 * @tparam p_alphabet Sequence alphabet consumed by the canonical rolling hasher.
 */
template <sequence_alphabet p_alphabet> struct minimap_int_hasher {
    /** @brief Lexicographic scalar hasher supplying canonical codes and strand tags. */
    using base_hasher_type = lexic_hasher<p_alphabet>;

    /**
     * @brief Maintains canonical rolling state and emits minimap2-permuted hashes.
     * @tparam p_input_type Scalar word used for the packed k-mer and hash.
     */
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

        /**
         * @brief Clamps one permutation shift to the selected scalar width.
         * @tparam p_offset Requested compile-time shift distance.
         */
        template <std::size_t p_offset>
        static constexpr std::size_t k_shift = std::min(p_offset, sizeof(p_input_type) * 8u - 1u);

        /**
         * @brief Advances the canonical state and applies the minimap2 integer permutation.
         * @param[in] incoming Alphabet rank entering the right edge of the k-mer.
         * @param[in] outgoing Alphabet rank leaving the left edge of the k-mer.
         * @return The mixed canonical hash and the unchanged canonical strand classification.
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
     * @brief Applies minimap2's width-clamped seven-step integer permutation.
     * @tparam p_input_type Scalar word containing the canonical k-mer.
     * @param[in] lex_kmer Canonical packed k-mer code.
     * @param[in] kmer_mask Mask retaining only the k-mer's occupied bits after mixing.
     * @return Permuted hash masked to the k-mer width.
     */
    template <typename p_input_type>
    [[nodiscard]] static constexpr p_input_type scramble(const p_input_type& lex_kmer,
                                                         const p_input_type& kmer_mask) {
        constexpr std::size_t k_max_storage_shift = sizeof(p_input_type) * 8u - 1u;
        // Clamps permutation shifts to the scalar word bit width.
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
