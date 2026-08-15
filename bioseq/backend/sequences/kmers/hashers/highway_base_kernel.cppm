/**
 * @file highway_base_kernel.cppm
 * @brief Fixed-target Highway foundations shared by minimizer hashers.
 *
 * @details Extends the target-neutral hasher contract with lane and vector types inside the
 * generator-owned target namespace. It introduces no dispatch boundary.
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

/**
 * @brief Attaches fixed-target Highway vector traits to the scalar hasher policy.
 * @tparam p_alphabet Sequence alphabet processed by each lane.
 * @tparam p_input_type Scalar lane type holding a rolling k-mer code.
 * @tparam p_simd_tag Highway descriptor for the generated target.
 */
template <sequence_alphabet p_alphabet, typename p_input_type, typename p_simd_tag>
struct simd_hasher_base : hasher_base<p_alphabet, p_input_type> {
    using scalar_base_type = hasher_base<p_alphabet, p_input_type>;

    /** @brief Describes the lane, vector, and return types for one fixed target. */
    struct traits : public scalar_base_type::traits {
        using input_simd_tag_type = p_simd_tag;
        using input_vector_type = hn::Vec<input_simd_tag_type>;
        using hasher_return_type = input_vector_type;
    };
};

/**
 * @brief Checks that a hasher template implements the fixed-target SIMD update contract.
 * @tparam p_hasher_template Alphabet-parameterized hasher policy.
 * @tparam p_alphabet Sequence alphabet accepted by the policy.
 * @tparam p_input_type Scalar type stored in each vector lane.
 * @tparam p_simd_tag Highway descriptor used by the specialization.
 * @tparam p_simd_hasher_type Concrete SIMD hasher specialization to validate.
 */
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
