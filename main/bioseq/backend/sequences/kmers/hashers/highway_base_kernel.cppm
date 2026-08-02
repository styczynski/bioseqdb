/** Canonical generator input for exact minimizer SIMD foundations. */
module;
#include <hwy/highway.h>

export module bioseq.highway.sources.hasher_base;

import std;
import bioseq.common.sequan_compat;
import bioseq.sequences;
import bioseq.sequences.kmers.hashers.base;

export namespace bioseq::simd {
namespace [[bioseq::simd]] target {
namespace hn = hwy::HWY_NAMESPACE;

template<
    typename Derived,
    sequence_alphabet Alphabet,
    typename Input,
    typename D>
struct simd_hasher_base
    : ::bioseq::simd::hasher_base<Derived, Alphabet, Input> {
    struct traits
        : public simd_hasher_base<Derived, Alphabet, Input, D>::
              hasher_base::traits {
        using input_d_t = D;
        using input_vec_t = hn::Vec<input_d_t>;
        using hasher_return_t = input_vec_t;
    };
};

template<
    template<typename> typename Hasher,
    typename Alphabet,
    typename Input = std::uint64_t,
    typename D = hn::ScalableTag<Input>,
    typename SimdHasher =
        Hasher<Alphabet>::template simd_hash<Input, D>>
concept supports_simd_hash = sequence_alphabet<Alphabet>
    && requires (SimdHasher& hasher, D& d, hn::Vec<D>&& incoming) {
        { hasher.next(
            d, std::move(incoming), std::move(incoming)) }
            -> std::same_as<typename SimdHasher::traits::hasher_return_t>;
    };

} // namespace target
} // namespace bioseq::simd
