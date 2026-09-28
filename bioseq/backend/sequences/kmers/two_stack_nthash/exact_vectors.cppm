/**
 * @file exact_vectors.cppm
 * @brief Checks SIMD widths and output layouts for minimizer kernels.
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


namespace two_stack_internal {

#if HWY_TARGET == HWY_SCALAR
/// @brief Whether the current translation targets Highway's scalar backend.
inline constexpr bool exact_compiled_target_is_scalar = true;
#else
/// @brief Whether the current translation targets Highway's scalar backend.
inline constexpr bool exact_compiled_target_is_scalar = false;
#endif

/**
 * @brief Checks output layouts supported by the eight-candidate K15/W10 kernel.
 *
 * The kernel uses 32-bit hash lanes and 64-bit output records. It accepts
 * the field layouts of minimizer_representation<33, 15> for target postings
 * and minimap_sketch_data for query sketches. These reserve 30 and 35 hash
 * bits respectively, while the K15 hash uses 30 bits.
 *
 * The masks and shifts must match one of those layouts.
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

/**
 * @brief Selects the fast K15/W10 kernel when packing and SIMD width permit it.
 *
 * Requires DNA4 in 32-bit units, a supported output layout, four u32 lanes,
 * and two u64 lanes. consumed_bp must fit in uint32_t. Other inputs select
 * the generic u64 kernel. The caller checks the k-mer and window sizes
 */
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

/// @brief Accepts 2, 4, 8, or 16 u64 lanes when they fit  in lane_capacity.
[[nodiscard]] constexpr bool exact_vector_lane_count_supported(
    std::size_t lanes,
    std::size_t lane_capacity) noexcept
{
    return lanes >= 2 && lanes <= lane_capacity
        && (lanes == 2 || lanes == 4 || lanes == 8 || lanes == 16);
}

/**
 * @brief Rejects scalar targets or unsupported SIMD transpose widths.
 *
 * Stores unsupported_vector_shape in state on failure. This check leaves
 * execution counters unchanged, so a freshly reset state still has zero
 * work recorded.
 * @return ok when the target can run the exact kernel.
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


/**
 * @brief Stores a failure status and counts the rejection.
 *
 * The rejection counter stops at uint64_t maximxm.
 * @return status.
 */
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
