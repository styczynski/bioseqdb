/**
 * @file vulkan.cppm
 * @brief Vulkan rank-buffer batch surface for the SneakySnake prefilter.
 *
 * The runtime owns all Vulkan resources and executes one grid-strided batch
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

/// True only when the shared runtime owns a hardware Vulkan device.
[[nodiscard]] inline bool is_vulkan_sneaky_snake_supported() {
    auto& vulkan_runtime = bioseq::gpu::vulkan::runtime::instance();
    return bioseq::gpu::vulkan::hardware_device_available(vulkan_runtime);
}

/// Execute one selected Vulkan SneakySnake batch over flat uint8 rank streams.
/// Offsets in every job are symbol offsets into these streams.
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
