/**
 * @file vulkan.cppm
 * @brief Vulkan rank-buffer batch surface for the SneakySnake prefilter.
 *
 * The runtime owns descriptor bindings, push constants, mapped allocations,
 * command replay, synchronization, and the existing grid-strided batch
 * dispatch. This module only provides the selected-backend policy boundary:
 * an unavailable or software Vulkan device fails loudly and never routes the
 * work through a CPU implementation.
 */
module;

#include <cstdint>
#include <span>
#include <vector>

export module bioseq.prefilter.sneaky_snake.vulkan;

export import bioseq.prefilter.types;
import bioseq.gpu.vulkan.runtime;

export namespace bioseq::prefilter {

/**
 * @brief Reports whether the shared runtime owns a hardware Vulkan device.
 * @return `true` only when SneakySnake can use the hardware Vulkan backend.
 * @note A software Vulkan device does not satisfy this capability check.
 */
[[nodiscard]] inline bool is_vulkan_sneaky_snake_supported() {
    auto& vulkan_runtime = bioseq::gpu::vulkan::runtime::instance();
    return bioseq::gpu::vulkan::hardware_device_available(vulkan_runtime);
}

/**
 * @brief Executes one selected Vulkan SneakySnake batch over flat rank streams.
 * @param[in] query_ranks Flat query ranks addressed by job symbol offsets.
 * @param[in] target_ranks Flat target ranks addressed by job symbol offsets.
 * @param[in] jobs Ordered slice geometry and edit thresholds.
 * @return One prefilter result per job in input order.
 *
 * The shared runtime retains ownership of descriptor bindings, push constants,
 * dispatch geometry, mapped allocations, command replay, and synchronization.
 * Hardware availability is required before the existing rank-batch roundtrip is
 * invoked. This boundary never selects a CPU fallback.
 */
[[nodiscard]] inline std::vector<prefilter_result>
run_sneaky_snake_vulkan_batch(
    std::span<const std::uint8_t> query_ranks,
    std::span<const std::uint8_t> target_ranks,
    std::span<const prefilter_job> jobs)
{
    auto& vulkan_runtime = bioseq::gpu::vulkan::runtime::instance();
    bioseq::gpu::vulkan::require_hardware_device(
        vulkan_runtime, "Vulkan SneakySnake prefilter");
    return vulkan_runtime.run_sneaky_snake_rank_batch_roundtrip(
        query_ranks, target_ranks, jobs);
}

}  // namespace bioseq::prefilter
