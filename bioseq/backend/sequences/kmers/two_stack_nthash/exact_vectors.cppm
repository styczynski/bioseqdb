/**
 * @file exact_vectors.cppm
 * @brief Exact SIMD target admission and shared continuation support.
 */
module;

#include <hwy/highway.h>
#include <libassert/assert.hpp>

export module bioseq.highway.sources.minimizer_two_stack_nthash_exact_vectors;

import std;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.highway.sources.minimizer_two_stack_nthash_state;

export namespace bioseq::simd {
namespace [[bioseq::simd]] target {
namespace hn = hwy::HWY_NAMESPACE;

// =============================================================================
namespace two_stack_internal {

#if HWY_TARGET == HWY_SCALAR
/// @brief Whether the current translation targets Highway's scalar backend.
inline constexpr bool exact_compiled_target_is_scalar = true;
#else
/// @brief Whether the current translation targets Highway's scalar backend.
inline constexpr bool exact_compiled_target_is_scalar = false;
#endif

/**
 * @brief Tests whether an output layout admits the packed K15/W10/B8 kernel.
 *
 * The packed kernel stores candidate hashes in u32 lanes and raw
 * minimizers in u64 lanes.  Both persisted-index representations used by the
 * index layouts satisfy that contract:
 *
 *  * `minimizer_representation<33, 15>` for target postings (30 hash bits).
 *  * `minimap_sketch_data == minimizer_representation<28, 15>` for query sketches
 *    (35 hash bits, of which minimap-int uses the low 30).
 *
 * Admission is limited to these two persisted-compatible layouts. The raw
 * construction is parameterized by `k_hash_shift`, `k_position_field_mask`,
 * and `k_position_shift`. Arbitrary `p_minimizer_type` encodings are rejected.
 * @tparam p_minimizer_type Candidate output representation.
 */
template <class p_minimizer_type>
inline constexpr bool exact_k15_w10_b8_representation_v =
    []() constexpr {
        if constexpr (!exact_simd_minimizer_representation<p_minimizer_type>) {
            return false;
        } else {
            return std::same_as<
                       typename p_minimizer_type::storage_type, std::uint64_t>
                && p_minimizer_type::k_position_shift == 1u
                && ((static_cast<std::uint64_t>(
                         p_minimizer_type::k_hash_field_mask) == 0x3fffffffull
                     && static_cast<std::uint64_t>(
                         p_minimizer_type::k_position_field_mask) == 0x1ffffffffull
                     && p_minimizer_type::k_hash_shift == 34u)
                    || (static_cast<std::uint64_t>(
                            p_minimizer_type::k_hash_field_mask) == 0x7ffffffffull
                        && static_cast<std::uint64_t>(
                            p_minimizer_type::k_position_field_mask) == 0x0fffffffull
                        && p_minimizer_type::k_hash_shift == 29u));
        }
    }();

/// @brief Selects the exact SIMD kernel admitted by type and input length.
/// @tparam p_minimizer_type Candidate output representation.
/// @tparam p_alphabet_traits Packed-sequence alphabet and storage traits.
/// @param consumed_bp Number of biological symbols in the request.
/// @return Specialized K15/W10 kernel when all layout constraints hold.
template<class p_minimizer_type, class p_alphabet_traits>
[[nodiscard]] inline exact_simd_kernel select_exact_simd_kernel_kind(
    std::size_t consumed_bp) noexcept
{
    if constexpr (std::same_as<typename p_alphabet_traits::alphabet_type, seq::dna4> &&
                  std::same_as<typename p_alphabet_traits::storage_unit, std::uint32_t> &&
                  exact_k15_w10_b8_representation_v<p_minimizer_type>) {
        const hn::ScalableTag<std::uint32_t> d32;
        const hn::ScalableTag<std::uint64_t> d64;
        if (!exact_compiled_target_is_scalar && hn::Lanes(d32) == 4 && hn::Lanes(d64) == 2 &&
            consumed_bp <= static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())) {
            return exact_simd_kernel::kmer_15_window_10_byte_8_uint32_hash;
        }
    }
    return exact_simd_kernel::generic_u64;
}

/// @brief Tests whether a u64 lane count has an admitted square geometry.
/// @param lanes Active u64 lane count.
/// @param lane_capacity Maximum lanes retained by continuation storage.
/// @return Whether the lane count is supported and within capacity.
[[nodiscard]] constexpr bool exact_vector_lane_count_supported(
    std::size_t lanes,
    std::size_t lane_capacity) noexcept
{
    return lanes >= 2 && lanes <= lane_capacity
        && (lanes == 2 || lanes == 4 || lanes == 8 || lanes == 16);
}

/**
 * @brief Validates the target geometry before exact continuation work begins.
 *
 * The exact continuation is a SIMD-only contract. Target rejection differs
 * from an invalid biological request: it happens before reset/consume work and
 * therefore deliberately leaves every execution-stats counter at zero.
 * @tparam p_state_type Continuation state carrying rejection status.
 * @param state State updated when the shape is rejected.
 * @param lanes Active u64 lane count.
 * @param lane_capacity Maximum lanes retained by @p state.
 * @param compiled_target_is_scalar Whether the active backend is scalar.
 * @param transpose_shape_supported Whether the square transpose is available.
 * @return Exact continuation admission status.
 */
template<class p_state_type>
[[nodiscard]] constexpr two_stack_continuation_status
validate_exact_vector_shape(
    p_state_type& state,
    std::size_t lanes,
    std::size_t lane_capacity,
    bool compiled_target_is_scalar,
    bool transpose_shape_supported) noexcept
{
    if (compiled_target_is_scalar
        || !exact_vector_lane_count_supported(lanes, lane_capacity)
        || !transpose_shape_supported) {
        state.status = two_stack_continuation_status::unsupported_vector_shape;
        return state.status;
    }
    return two_stack_continuation_status::ok;
}


/// @brief Records a fail-closed continuation status with saturating evidence.
/// @tparam p_state_type Continuation state carrying status and counters.
/// @param state State updated with the rejection.
/// @param status Failure status to retain.
/// @return @p status.
template<class p_state_type>
[[nodiscard]] inline two_stack_continuation_status reject_continuation(
    p_state_type& state,
    two_stack_continuation_status status) noexcept
{
    state.status = status;
    if (state.stats.fail_closed_rejections
        != std::numeric_limits<std::uint64_t>::max()) {
        ++state.stats.fail_closed_rejections;
    }
    return status;
}

/// @brief Adds to an execution counter with saturation at `uint64_t` maximum.
/// @param counter Counter updated in place.
/// @param increment Amount to add.
inline void add_stats_counter(
    std::uint64_t& counter,
    std::uint64_t increment) noexcept
{
    const std::uint64_t maximum =
        std::numeric_limits<std::uint64_t>::max();
    counter = increment > maximum - counter
        ? maximum
        : counter + increment;
}


}  // namespace two_stack_internal

}  // namespace target
}  // namespace bioseq::simd
