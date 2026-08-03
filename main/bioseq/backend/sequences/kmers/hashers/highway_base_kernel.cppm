/** Canonical generator input for exact minimizer SIMD foundations. */
module;
#include <hwy/highway.h>

export module bioseq.highway.sources.hasher_base;

import std;
import bioseq.sequences;
import bioseq.sequences.kmers.hashers.base;
import bioseq.highway.sources.sequence_ops;
import bioseq.highway.sources.simd_utils;

export namespace bioseq::simd {
namespace [[bioseq::simd]] target {
namespace hn = hwy::HWY_NAMESPACE;

using ::bioseq::simd::char_to_nt4;
using ::bioseq::simd::hasher_base;
using ::bioseq::simd::kNt4Sentinel;
using ::bioseq::simd::make_nt4_lut;
using ::bioseq::simd::nt4_lut_v;
using ::bioseq::simd::nt4_normalize;
using ::bioseq::simd::supports_scalar_hash;

template<
    typename Derived,
    sequence_alphabet Alphabet,
    typename Input,
    typename D>
struct simd_hasher_base
    : hasher_base<Derived, Alphabet, Input> {
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
concept supports_simd_hash = ::bioseq::sequence_alphabet<Alphabet>
    && requires (SimdHasher& hasher, D& d, hn::Vec<D>&& incoming) {
        { hasher.next(
            d, std::move(incoming), std::move(incoming)) }
            -> std::same_as<typename SimdHasher::traits::hasher_return_t>;
    };

} // namespace target
} // namespace bioseq::simd
