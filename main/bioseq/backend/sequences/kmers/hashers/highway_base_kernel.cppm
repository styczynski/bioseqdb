/**
 * @file highway_base_kernel.cppm
 * @brief Fixed-target Highway foundations shared by minimizer hashers.
 */
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
using ::bioseq::simd::k_nt4_sentinel;
using ::bioseq::simd::make_nt4_lut;
using ::bioseq::simd::k_nt4_lut;
using ::bioseq::simd::nt4_normalize;
using ::bioseq::simd::supports_scalar_hash;

/** Attach Highway vector traits to the target-neutral scalar hasher policy. */
template<sequence_alphabet p_alphabet, typename p_input_type, typename p_simd_tag>
struct simd_hasher_base
    : hasher_base<p_alphabet, p_input_type> {
    using scalar_base_type = hasher_base<p_alphabet, p_input_type>;

    /** Highway lane and return types for one fixed target. */
    struct traits
        : public scalar_base_type::traits {
        using input_simd_tag_type = p_simd_tag;
        using input_vector_type = hn::Vec<input_simd_tag_type>;
        using hasher_return_type = input_vector_type;
    };
};

/** True when a hasher template supplies the fixed-target SIMD contract. */
template<
    template<typename> typename p_hasher_template,
    typename p_alphabet,
    typename p_input_type = std::uint64_t,
    typename p_simd_tag = hn::ScalableTag<p_input_type>,
    typename p_simd_hasher_type =
        p_hasher_template<p_alphabet>::template simd_hash<p_input_type, p_simd_tag>>
concept supports_simd_hash = ::sequence_alphabet<p_alphabet>
    && requires (
        p_simd_hasher_type& hasher,
        p_simd_tag& simd_tag,
        hn::Vec<p_simd_tag>&& incoming) {
        { hasher.next(
            simd_tag, std::move(incoming), std::move(incoming)) }
            -> std::same_as<typename p_simd_hasher_type::traits::hasher_return_type>;
    };

} // namespace target
} // namespace bioseq::simd
