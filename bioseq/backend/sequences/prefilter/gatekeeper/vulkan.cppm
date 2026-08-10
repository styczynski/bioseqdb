/**
 * @file vulkan.cppm
 * @brief Hardware-only Vulkan batch surface for the GateKeeper prefilter.
 *
 * The shared Vulkan runtime owns the mapped rank workspace, descriptor state,
 * recorded command, and synchronization. This module is only the selected-
 * backend boundary and never substitutes a CPU implementation.
 */
module;

#include <cstdint>
#include <span>
#include <vector>

export module bioseq.prefilter.gatekeeper.vulkan;

export import bioseq.prefilter.types;
import bioseq.gpu.vulkan.runtime;

export namespace bioseq::prefilter {

/// True only when the shared runtime owns a hardware Vulkan device.
[[nodiscard]] inline bool is_vulkan_gatekeeper_supported() {
    auto& vulkan_runtime = bioseq::gpu::vulkan::runtime::instance();
    return bioseq::gpu::vulkan::hardware_device_available(vulkan_runtime);
}

/// Execute one selected Vulkan GateKeeper batch over flat uint8 rank streams.
/// Offsets in every job are symbol offsets into these streams.
[[nodiscard]] inline std::vector<prefilter_result>
run_gatekeeper_vulkan_batch(
    std::span<const std::uint8_t> query_ranks,
    std::span<const std::uint8_t> target_ranks,
    std::span<const prefilter_job> jobs)
{
    auto& vulkan_runtime = bioseq::gpu::vulkan::runtime::instance();
    bioseq::gpu::vulkan::require_hardware_device(
        vulkan_runtime, "Vulkan GateKeeper prefilter");
    return vulkan_runtime.run_gatekeeper_rank_batch_roundtrip(
        query_ranks, target_ranks, jobs);
}

}  // namespace bioseq::prefilter
