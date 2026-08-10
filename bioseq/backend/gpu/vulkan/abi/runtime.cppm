/**
 * @file
 * @brief Defines Vulkan runtime allocation and dispatch ABI contracts.
 */
module;

#include <algorithm>
#include <cstdint>

export module bioseq.gpu.vulkan.abi:runtime;

import bioseq.common.checked_arithmetic;

export namespace bioseq::gpu::vulkan {

/** @brief Required byte alignment of suballocated device-buffer offsets. */
inline constexpr std::uint32_t k_gpu_buffer_offset_alignment = 256u;
/**
 * @brief Conservative device-memory allocation quantum used by peak planning.
 *
 * Runtime allocation still verifies the allocator-reported byte total before
 * dispatch. This bound accounts for per-buffer padding instead of assuming
 * that a requested range equals its allocation size.
 */
inline constexpr std::uint32_t k_gpu_peak_allocation_alignment = 64u * 1024u;
/**
 * @brief Computes physical X groups for a grid-striding shader.
 * @return A device-legal group count without truncating logical coverage.
 */
[[nodiscard]] constexpr std::uint32_t grid_stride_dispatch_group_count(
    std::uint64_t logical_items,
    std::uint32_t local_size,
    std::uint32_t max_workgroups) noexcept
{
    if (logical_items == 0u || local_size == 0u || max_workgroups == 0u) {
        return 0u;
    }
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(
        bioseq::common::ceil_div(logical_items, local_size),
        max_workgroups));
}
/** @brief Aggregates sketch, seed, anchor, and chain counts from the device. */
struct gpu_pipeline_counters {
    std::uint64_t query_minimizers;
    std::uint64_t target_minimizers;
    std::uint64_t seed_hits;
    std::uint64_t anchors;
    std::uint64_t chain_ranges;
    std::uint64_t sketch_overflows;
    std::uint64_t seed_overflows;
    std::uint64_t reserved;
};

} // namespace bioseq::gpu::vulkan
