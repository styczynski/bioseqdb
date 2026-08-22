/**
 * @file vulkan/abi/runtime.cppm
 * @brief Defines Vulkan runtime allocation and dispatch ABI contracts.
 */
module;

export module bioseq.gpu.vulkan.abi:runtime;

import std;
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
 * @brief Computes physical X workgroups for a grid-striding compute dispatch.
 * @param[in] logical_items Number of logical work items to cover.
 * @param[in] local_size Shader invocations in one workgroup along X.
 * @param[in] max_workgroups Device limit for the X workgroup dimension.
 * @return A dispatch count capped by `max_workgroups`. Shaders advance with
 *         `id += gl_NumWorkGroups.x` so every logical item remains covered.
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
 * @brief Aggregates sketch, seed, anchor, and chain counts in the device ABI.
 */
struct gpu_pipeline_counters {
    std::uint64_t query_minimizers;  ///< Query minimizer count.
    std::uint64_t target_minimizers; ///< Target minimizer count.
    std::uint64_t seed_hits;         ///< Seed-hit count.
    std::uint64_t anchors;           ///< Anchor-record count.
    std::uint64_t chain_ranges;      ///< Chain-range count.
    std::uint64_t sketch_overflows;  ///< Sketch-stage overflow count.
    std::uint64_t seed_overflows;    ///< Seed-stage overflow count.
    std::uint64_t reserved;          ///< Reserved wire field for future ABI extensions.
};

} // namespace bioseq::gpu::vulkan
