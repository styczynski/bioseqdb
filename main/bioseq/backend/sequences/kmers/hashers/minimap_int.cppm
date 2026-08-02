/** Target-neutral scalar minimap integer hasher. */
module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>

export module bioseq.sequences.kmers.hashers.minimap_int;

import bioseq.sequences.kmers.hashers.base;
import bioseq.sequences.kmers.hashers.lexic;
import bioseq.common.sequan_compat;
import bioseq.sequences;

export namespace bioseq::simd {

template<sequence_alphabet Alphabet>
struct minimap_int_hasher {
    using base_hasher_t = lexic_hasher<Alphabet>;

    template<typename Input>
    class scalar_hash
        : public hasher_base<scalar_hash<Input>, Alphabet, Input> {
    public:
        using traits = scalar_hash<Input>::hasher_base::traits;

        explicit scalar_hash(const std::size_t& kmer_length)
            : base_hasher(kmer_length),
              mask((static_cast<typename traits::input_t>(1)
                    << (2u * kmer_length)) - 1u) {}

        template<std::size_t Offset>
        static constexpr std::size_t shift =
            std::min(Offset, sizeof(Input) * 8u - 1u);

        constexpr traits::hasher_return_t next(
            traits::input_t incoming,
            traits::input_t outgoing)
        {
            auto [key, base_strand] = base_hasher.next(
                std::move(incoming), std::move(outgoing));

            key = (~key + (key << shift<21>)) & mask;
            key = key ^ key >> shift<24>;
            key = ((key + (key << shift<3>))
                    + (key << shift<8>)) & mask;
            key = key ^ key >> shift<14>;
            key = ((key + (key << shift<2>))
                    + (key << shift<4>)) & mask;
            key = key ^ key >> shift<28>;
            key = (key + (key << shift<31>)) & mask;
            return std::make_pair(key, base_strand);
        }

    private:
        typename base_hasher_t::template scalar_hash<Input> base_hasher;
        const typename traits::input_t mask;
    };

    template<typename Input>
    [[nodiscard]] static constexpr Input scramble(
        const Input& lex_kmer,
        const Input& kmer_mask)
    {
        constexpr std::size_t storage_max_shift = sizeof(Input) * 8u - 1u;
        constexpr auto clamp = [](std::size_t shift) constexpr {
            return shift > storage_max_shift ? storage_max_shift : shift;
        };
        Input key = lex_kmer;
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
