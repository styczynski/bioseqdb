/**
 * @file highway_base_kernel.cppm
 * @brief Adds Highway vector types to the shared minimizer hasher interface.
 * The generator compiles these types for each selected CPU target.
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

/** @brief Adds SIMD vector types to the scalar hasher's alphabet traits. */
template <sequence_alphabet p_alphabet, typename p_input_type, typename p_simd_tag>
struct simd_hasher_base : hasher_base<p_alphabet, p_input_type> {
    /** @brief Scalar policy reused for alphabet limits and strand selection. */
    using scalar_base_type = hasher_base<p_alphabet, p_input_type>;

    /** @brief Vector and result types for this Highway target. */
    struct traits : public scalar_base_type::traits {
        /** @brief Highway descriptor for the SIMD lane type and count. */
        using input_simd_tag_type = p_simd_tag;
        /** @brief SIMD register holding one packed k-mer code per lane. */
        using input_vector_type = hn::Vec<input_simd_tag_type>;
        /** @brief Vector of hashes returned by simd_hash::next. */
        using hasher_return_type = input_vector_type;
    };
};

/** @brief Requires a SIMD hasher whose next method returns its declared vector type. */
template <template <typename> typename p_hasher_template,
          typename p_alphabet,
          typename p_input_type = std::uint64_t,
          typename p_simd_tag = hn::ScalableTag<p_input_type>,
          typename p_simd_hasher_type =
              p_hasher_template<p_alphabet>::template simd_hash<p_input_type, p_simd_tag>>
concept supports_simd_hash =
    ::sequence_alphabet<p_alphabet> &&
    requires(p_simd_hasher_type& hasher, p_simd_tag& simd_tag, hn::Vec<p_simd_tag>&& incoming) {
        {
            hasher.next(simd_tag, std::move(incoming), std::move(incoming))
        } -> std::same_as<typename p_simd_hasher_type::traits::hasher_return_type>;
    };

} // namespace target
} // namespace bioseq::simd
