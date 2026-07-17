/**
 * @file gatekeeper_vulkan.cppm
 * @brief Hardware-only Vulkan batch surface for the GateKeeper prefilter.
 *
 * The shared Vulkan runtime owns the mapped rank workspace, descriptor state,
 * recorded command, and synchronization. This module is only the selected-
 * backend boundary and never substitutes a CPU implementation.
 */
module;

#include "common.h"

#include <cstdint>
#include <span>
#include <vector>

export module bioseq.sequences.prefilters.gatekeeper_vulkan;

export import bioseq.sequences.prefilters.prefilter_types;
import bioseq.gpu.vulkan.runtime;

export namespace bioseq::sequences::prefilters {

/// True only when the shared runtime owns a hardware Vulkan device.
[[nodiscard]] inline bool vulkan_gatekeeper_supported() {
    auto& runtime = bioseq::gpu::vulkan::Runtime::instance();
    return bioseq::gpu::vulkan::hardware_device_available(runtime);
}

/// Execute one selected Vulkan GateKeeper batch over flat uint8 rank streams.
/// Offsets in every job are symbol offsets into these streams.
[[nodiscard]] inline std::vector<PrefilterResult>
run_gatekeeper_vulkan_batch(
    std::span<const std::uint8_t> query_ranks,
    std::span<const std::uint8_t> target_ranks,
    std::span<const PrefilterJob> jobs)
{
    auto& runtime = bioseq::gpu::vulkan::Runtime::instance();
    bioseq::gpu::vulkan::require_hardware_device(
        runtime, "Vulkan GateKeeper prefilter");
    return runtime.run_gatekeeper_rank_batch_roundtrip(
        query_ranks, target_ranks, jobs);
}

}  // namespace bioseq::sequences::prefilters
