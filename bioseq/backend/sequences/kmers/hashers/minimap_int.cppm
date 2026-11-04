/**
 * @file minimap_int.cppm
 * @brief Computes scalar minimap2 integer hashes from rolling k-mer codes.
 * The smaller strand code is mixed into a hash while its strand label is kept.
 */
module;

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
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
         * @pre The complete rank bits fit in `p_input_type`.
         */
        explicit scalar_hash(std::size_t kmer_length)
            : base_hasher_(kmer_length),
              mask_(traits::exact_kmer_mask(kmer_length)) {}

        /** @brief Limits a requested bit shift  to one less than the scalar word width. */
        template <std::size_t p_offset>
        static constexpr std::size_t k_shift = std::min(p_offset, sizeof(p_input_type) * 8u - 1u);

        /**
         * @brief Updates the strand codes and hashes the smaller code.
         * @param incoming Rank appended to the forward code.
         * @return The mixed hash with the strand chosen before mixing.
         */
        constexpr typename traits::hasher_return_type next(typename traits::input_type incoming) {
            auto [key, base_strand] = base_hasher_.next(std::move(incoming));
            key = minimap_int_hasher::template scramble<p_input_type>(key, mask_);
            return std::make_pair(key, base_strand);
        }

        /** Restart the shared exact rolling state after an unseedable rank. */
        constexpr void reset() noexcept { base_hasher_.reset(); }

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

    /**
     * @brief Recovers the packed code whose integer mix equals `hash`.
     * Reverses the odd affine multipliers and right-xorshifts in `scramble`.
     * @param hash Mixed value within `kmer_mask`.
     * @param kmer_mask Nonzero contiguous low-bit mask for the exact key domain.
     * @return Unique preimage in that domain, with the same word-width shift caps.
     * @pre `hash <= kmer_mask`; `kmer_mask` fits `p_input_type`.
     * Uses non-promoting unsigned words, matching the resident u32/u64 mixers.
     */
    template <std::unsigned_integral p_input_type>
        requires std::same_as<p_input_type, decltype(+p_input_type{})>
    [[nodiscard]] static constexpr p_input_type unscramble(
        const p_input_type hash, const p_input_type kmer_mask) {
        using calculation_type = p_input_type;
        constexpr std::size_t word_bits = std::numeric_limits<p_input_type>::digits;
        constexpr auto clamp = [](const std::size_t shift) constexpr {
            return std::min(shift, word_bits - 1u);
        };
        const calculation_type mask = kmer_mask;
        // Newton lifting doubles the known inverse bits at each step.
        const auto inverse_odd = [mask](const calculation_type multiplier) constexpr {
            calculation_type inverse = 1u;
            for (std::size_t known_bits = 1u; known_bits < word_bits; known_bits *= 2u)
                inverse *= calculation_type{2u} - multiplier * inverse;
            return inverse & mask;
        };
        // Invert x ^ (x >> shift) by composing doubled right shifts.
        constexpr auto undo_right_xor = [](calculation_type value,
                                          const std::size_t shift) constexpr {
            for (std::size_t distance = shift; distance < word_bits; distance *= 2u)
                value ^= value >> distance;
            return value;
        };
        constexpr calculation_type one = 1u;
        calculation_type key = hash;
        key = (key * inverse_odd(one + (one << clamp(31u)))) & mask;
        key = undo_right_xor(key, clamp(28u));
        key = (key * inverse_odd(one + (one << clamp(2u)) + (one << clamp(4u)))) & mask;
        key = undo_right_xor(key, clamp(14u));
        key = (key * inverse_odd(one + (one << clamp(3u)) + (one << clamp(8u)))) & mask;
        key = undo_right_xor(key, clamp(24u));
        // The first forward step is ((2^shift - 1) * key - 1) modulo 2^B.
        key = ((key + one) * inverse_odd((one << clamp(21u)) - one)) & mask;
        return static_cast<p_input_type>(key);
    }
};

static_assert(supports_scalar_hash<minimap_int_hasher, seq::dna4>);

} // namespace bioseq::simd
