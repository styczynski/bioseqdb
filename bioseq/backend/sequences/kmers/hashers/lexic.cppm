/**
 * @file lexic.cppm
 * @brief Updates packed k-mer codes for the forward and reverse-complement strands.
 * Each new symbol updates the codes in constant time. The smaller code
 * supplies the hash, with equal codes reported as a strand tie.
 */
module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>

export module bioseq.sequences.kmers.hashers.lexic;

import bioseq.sequences.kmers.hashers.base;
import bioseq.common.seqan_adapter;
import bioseq.sequences;

export namespace bioseq::simd {

/** @brief Builds scalar rollin k-mer codes using an alphabet's rank order. */
template <sequence_alphabet p_alphabet> struct lexic_hasher {
    /**
     * @brief Maintains forward and reverse-complement packed k-mer states.
     * @tparam p_input_type Scalar word used for both rolling states.
     */
    template <typename p_input_type>
    class scalar_hash : public hasher_base<p_alphabet, p_input_type> {
      public:
        /** @brief Scalar hasher base supplying alphabet limits and strand selection. */
        using base_type = hasher_base<p_alphabet, p_input_type>;
        /** @brief Mask, shift, and alphabet helpers shared by rolling updates. */
        using traits = typename base_type::traits;

        /**
         * @brief Initializes the rolling state for the requested k-mer length.
         * @param[in] kmer_length Positive symbol count within the scalar word capacity.
         * @pre `kmer_length > 0`.
         */
        explicit scalar_hash(std::size_t kmer_length)
            : mask_(traits::exact_kmer_mask(kmer_length)),
              reverse_shift_(traits::bits_per_char *
                             (kmer_length - 1u)),
              forward_state_(0), reverse_complement_state_{} {}

        /**
         * @brief Updates both strand codes and returns the smaller code with its strand.
         * @param incoming Rank appended to the forward code.
         * Equal codes return sequence_strand_mode::equal.
         */
        constexpr typename traits::hasher_return_type next(typename traits::input_type incoming) {
            forward_state_ = (forward_state_ << traits::bits_per_char | incoming) & mask_;
            if constexpr (traits::canonical_reverse_complement) {
                reverse_complement_state_[0] =
                    (reverse_complement_state_[0] >> traits::bits_per_char) |
                    (static_cast<p_input_type>(traits::letter_complement(incoming))
                        << reverse_shift_);
                return traits::select_min_hash_strand(
                    std::move(forward_state_), std::move(reverse_complement_state_[0]));
            } else {
                return {forward_state_, sequence_strand_mode::forward};
            }
        }

        /** Restart exact k-mer construction after an unseedable rank. */
        constexpr void reset() noexcept {
            forward_state_ = 0u;
            if constexpr (traits::canonical_reverse_complement) {
                reverse_complement_state_[0] = 0u;
            }
        }

      private:
        p_input_type mask_;
        std::size_t reverse_shift_;
        p_input_type forward_state_;
        [[no_unique_address]] std::array<p_input_type,
            traits::canonical_reverse_complement ? 1u : 0u> reverse_complement_state_;
    };

    /**
     * @brief Returns the selected k-mer code unchanged.
     * kmer_mask is accepted to match the integer hasher's interface
     */
    template <typename p_input_type>
    [[nodiscard]] static constexpr p_input_type
    scramble(const p_input_type& lex_kmer, [[maybe_unused]] const p_input_type& kmer_mask) {
        return lex_kmer;
    }
};

static_assert(supports_scalar_hash<lexic_hasher, seq::dna4>);

} // namespace bioseq::simd
