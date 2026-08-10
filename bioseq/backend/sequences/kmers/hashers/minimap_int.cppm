/**
 * @file minimap_int.cppm
 * @brief Target-neutral scalar minimap2 integer hasher.
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

template<sequence_alphabet p_alphabet>
struct minimap_int_hasher {
    using base_hasher_type = lexic_hasher<p_alphabet>;

    /** Stateful scalar minimap2 integer hash. */
    template<typename p_input_type>
    class scalar_hash
        : public hasher_base<p_alphabet, p_input_type> {
    public:
        using base_type = hasher_base<p_alphabet, p_input_type>;
        using traits = typename base_type::traits;

        /** Initialize the rolling lexicographic state and k-mer mask. */
        explicit scalar_hash(std::size_t kmer_length)
            : base_hasher_(kmer_length),
              mask_((static_cast<typename traits::input_type>(1)
                    << (2u * kmer_length)) - 1u) {}

        /** Clamp a fixed scramble shift to the selected storage width. */
        template<std::size_t p_offset>
        static constexpr std::size_t k_shift =
            std::min(p_offset, sizeof(p_input_type) * 8u - 1u);

        /** Consume one incoming/outgoing code and return the mixed canonical hash. */
        constexpr typename traits::hasher_return_type next(
            typename traits::input_type incoming,
            typename traits::input_type outgoing)
        {
            auto [key, base_strand] = base_hasher_.next(
                std::move(incoming), std::move(outgoing));
            key = minimap_int_hasher::template scramble<p_input_type>(key, mask_);
            return std::make_pair(key, base_strand);
        }

    private:
        typename base_hasher_type::template scalar_hash<p_input_type> base_hasher_;
        const typename traits::input_type mask_;
    };

    /** Apply minimap2's width-clamped integer permutation. */
    template<typename p_input_type>
    [[nodiscard]] static constexpr p_input_type scramble(
        const p_input_type& lex_kmer,
        const p_input_type& kmer_mask)
    {
        constexpr std::size_t k_max_storage_shift =
            sizeof(p_input_type) * 8u - 1u;
        constexpr auto clamp = [](std::size_t shift) constexpr {
            return shift > k_max_storage_shift ? k_max_storage_shift : shift;
        };
        p_input_type key = lex_kmer;
        key = (~key + (key << clamp(21u))) & kmer_mask;
        key = key ^ (key >> clamp(24u));
        key = ((key + (key << clamp(3u)))
                + (key << clamp(8u))) & kmer_mask;
        key = key ^ (key >> clamp(14u));
        key = ((key + (key << clamp(2u)))
                + (key << clamp(4u))) & kmer_mask;
        key = key ^ (key >> clamp(28u));
        key = (key + (key << clamp(31u))) & kmer_mask;
        return key;
    }
};

static_assert(supports_scalar_hash<minimap_int_hasher, seq::dna4>);

} // namespace bioseq::simd
