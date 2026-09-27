/**
 * @file vulkan.cppm
 * @brief Hardware-only Vulkan batch surface for the GateKeeper prefilter.
 *
 * The shared Vulkan runtime owns the mapped rank workspace, descriptor state,
 * push constants, recorded dispatch choice, allocations, and synchronization.
 * This module only exposes the selected-backend boundary and never substitutes
 * a CPU implementation.
 */
module;

#include <cstdint>
#include <span>
#include <vector>

export module bioseq.prefilter.gatekeeper.vulkan;

export import bioseq.prefilter.types;
import bioseq.gpu.vulkan.runtime;

export namespace bioseq::prefilter {

/**
 * @brief Reports whether the shared runtime owns a hardware Vulkan device.
 * @return `true` only when GateKeeper can use the hardware Vulkan backend.
 * @note A software Vulkan device does not satisfy this capability check.
 */
[[nodiscard]] inline bool is_vulkan_gatekeeper_supported() {
    auto& vulkan_runtime = bioseq::gpu::vulkan::runtime::instance();
    return bioseq::gpu::vulkan::hardware_device_available(vulkan_runtime);
}

/**
 * @brief Executes one selected Vulkan GateKeeper batch over flat rank streams.
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
