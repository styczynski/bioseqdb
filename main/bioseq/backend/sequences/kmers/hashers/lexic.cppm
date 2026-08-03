/** Target-neutral scalar lexicographic minimizer hasher. */
module;

#include <algorithm>
#include <cstddef>
#include <utility>

export module bioseq.sequences.kmers.hashers.lexic;

import bioseq.sequences.kmers.hashers.base;
import bioseq.common.sequan_compat;
import bioseq.sequences;

export namespace bioseq::simd {

template<sequence_alphabet Alphabet>
struct lexic_hasher {
    template<typename Input>
    class scalar_hash
        : public hasher_base<scalar_hash<Input>, Alphabet, Input> {
    public:
        using traits = scalar_hash<Input>::hasher_base::traits;

        explicit scalar_hash(const std::size_t& kmer_length)
            : mask(traits::template kmer_mask<Input>(
                  std::min(kmer_length, traits::max_kmer_length))),
              rev_shift(traits::bits_per_char
                  * (std::min(kmer_length, traits::max_kmer_length) - 1u)),
              state_forward(0),
              state_reverse_compl(0) {}

        constexpr traits::hasher_return_t next(
            traits::input_t incoming,
            traits::input_t outgoing)
        {
            state_forward =
                (state_forward << traits::bits_per_char | incoming) & mask;
            // Promote before shifting so wide dna5 k-mers do not shift a
            // uint8_t-promoted int by 32 or more bits.
            state_reverse_compl =
                (state_reverse_compl >> traits::bits_per_char)
                | (static_cast<Input>(traits::letter_complement(outgoing))
                    << rev_shift);
            return traits::select_min_hash_strand(
                std::move(state_forward), std::move(state_reverse_compl));
        }

    private:
        Input mask;
        std::size_t rev_shift;
        Input state_forward;
        Input state_reverse_compl;
    };

    template<typename Input>
    [[nodiscard]] static constexpr Input scramble(
        const Input& lex_kmer,
        [[maybe_unused]] const Input& kmer_mask)
    {
        return lex_kmer;
    }
};

static_assert(supports_scalar_hash<lexic_hasher, seq::dna4>);

} // namespace bioseq::simd
