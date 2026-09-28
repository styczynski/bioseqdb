/**
 * @file vulkan/abi/runtime.cppm
 * @brief Shared Vulkan buffer alignment, dispatch limits, and pipeline counts.
 */
module;

export module bioseq.gpu.vulkan.abi:runtime;

import std;
import bioseq.common.checked_arithmetic;

export namespace bioseq::gpu::vulkan {

/** @brief Required byte alignment of suballocated device-buffer offsets. */
inline constexpr std::uint32_t k_gpu_buffer_offset_alignment = 256u;
/**
 * @brief Allocation rounding reserved whe estimating peak GPU memory.
 *
 * A buffer may use extra memory for alignment. This allowance represents
 * that padding when planning allocations.
 */
inline constexpr std::uint32_t k_gpu_peak_allocation_alignment = 64u * 1024u;
/**
 * @brief Calculates an X workgroup count within the device limit.
 * @param[in] logical_items Number of items to process.
 * @param[in] local_size Number of invocations per workgroup.
 * @param[in] max_workgroups Device limit along X.
 * @return Zero if any input is zero, otherwise the capped workgroup count.
 *
 * When the count  is capped, the shader must loop over the remaining items
 * using the total dispatched group or invocation count as its stride.
 */
[[nodiscard]] constexpr std::uint32_t grid_stride_dispatch_group_count(
    std::uint64_t logical_items, std::uint32_t local_size, std::uint32_t max_workgroups) noexcept {
    if (logical_items == 0u || local_size == 0u || max_workgroups == 0u) {
        return 0u;
    }
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(
        bioseq::common::ceil_div(logical_items, local_size), max_workgroups));
}
/**
 * @brief Counts records and overflows at stages of, the GPU pipeline.
 */
struct gpu_pipeline_counters {
    std::uint64_t query_minimizers;  ///< Query minimizer count.
    std::uint64_t target_minimizers; ///< Target minimizer count.
    std::uint64_t seed_hits;         ///< Seed-hit count.
    std::uint64_t anchors;           ///< Anchor-record count.
    std::uint64_t chain_ranges;      ///< Chain-range count.
    std::uint64_t sketch_overflows;  ///< Sketch-stage overflow count.
    std::uint64_t seed_overflows;    ///< Seed-stage overflow count.
    std::uint64_t reserved;          ///< Reserved for future fields.
};

} // namespace bioseq::gpu::vulkan
