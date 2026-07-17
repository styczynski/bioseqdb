/**
 * @file sneaky_snake_vulkan.cppm
 * @brief Vulkan rank-buffer batch surface for the SneakySnake prefilter.
 *
 * The runtime owns all Vulkan resources and executes one grid-strided batch
 * dispatch. This module only provides the selected-backend policy boundary:
 * an unavailable or software Vulkan device fails loudly and never routes the
 * work through a CPU implementation.
 */
module;

#include "common.h"

#include <cstdint>
#include <span>
#include <vector>

export module bioseq.sequences.prefilters.sneaky_snake_vulkan;

export import bioseq.sequences.prefilters.prefilter_types;
import bioseq.gpu.vulkan.runtime;

export namespace bioseq::sequences::prefilters {

/// True only when the shared runtime owns a hardware Vulkan device.
[[nodiscard]] inline bool vulkan_sneaky_snake_supported() {
    auto& runtime = bioseq::gpu::vulkan::Runtime::instance();
    return bioseq::gpu::vulkan::hardware_device_available(runtime);
}

/// Execute one selected Vulkan SneakySnake batch over flat uint8 rank streams.
/// Offsets in every job are symbol offsets into these streams.
[[nodiscard]] inline std::vector<PrefilterResult>
run_sneaky_snake_vulkan_batch(
    std::span<const std::uint8_t> query_ranks,
    std::span<const std::uint8_t> target_ranks,
    std::span<const PrefilterJob> jobs)
{
    auto& runtime = bioseq::gpu::vulkan::Runtime::instance();
    bioseq::gpu::vulkan::require_hardware_device(
        runtime, "Vulkan SneakySnake prefilter");
    return runtime.run_sneaky_snake_rank_batch_roundtrip(
        query_ranks, target_ranks, jobs);
}

}  // namespace bioseq::sequences::prefilters
