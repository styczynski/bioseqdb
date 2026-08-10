/**
 * @file lexic.cppm
 * @brief Target-neutral scalar lexicographic minimizer hasher.
 */
module;

#include <algorithm>
#include <cstddef>
#include <utility>

export module bioseq.sequences.kmers.hashers.lexic;

import bioseq.sequences.kmers.hashers.base;
import bioseq.common.seqan_adapter;
import bioseq.sequences;

export namespace bioseq::simd {

template<sequence_alphabet p_alphabet>
struct lexic_hasher {
    /** Stateful scalar canonical-kmer hash. */
    template<typename p_input_type>
    class scalar_hash
        : public hasher_base<p_alphabet, p_input_type> {
    public:
        using base_type = hasher_base<p_alphabet, p_input_type>;
        using traits = typename base_type::traits;

        /** Initialize the rolling state for the requested k-mer length. */
        explicit scalar_hash(std::size_t kmer_length)
            : mask_(traits::template kmer_mask<p_input_type>(
                  std::min(kmer_length, traits::k_max_kmer_length))),
              reverse_shift_(traits::bits_per_char
                  * (std::min(kmer_length, traits::k_max_kmer_length) - 1u)),
              forward_state_(0),
              reverse_complement_state_(0) {}

        /** Consume one incoming and outgoing alphabet code. */
        constexpr typename traits::hasher_return_type next(
            typename traits::input_type incoming,
            typename traits::input_type outgoing)
        {
            forward_state_ =
                (forward_state_ << traits::bits_per_char | incoming) & mask_;
            // Promote before shifting so wide dna5 k-mers do not shift a
            // uint8_t-promoted int by 32 or more bits.
            reverse_complement_state_ =
                (reverse_complement_state_ >> traits::bits_per_char)
                | (static_cast<p_input_type>(traits::letter_complement(outgoing))
                    << reverse_shift_);
            return traits::select_min_hash_strand(
                std::move(forward_state_),
                std::move(reverse_complement_state_));
        }

    private:
        p_input_type mask_;
        std::size_t reverse_shift_;
        p_input_type forward_state_;
        p_input_type reverse_complement_state_;
    };

    /** Preserve the lexicographic canonical code without post-mixing. */
    template<typename p_input_type>
    [[nodiscard]] static constexpr p_input_type scramble(
        const p_input_type& lex_kmer,
        [[maybe_unused]] const p_input_type& kmer_mask)
    {
        return lex_kmer;
    }
};

static_assert(supports_scalar_hash<lexic_hasher, seq::dna4>);

} // namespace bioseq::simd
