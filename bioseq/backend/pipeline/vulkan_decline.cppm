/**
 * @file pipeline/vulkan_decline.cppm
 * @brief Narrow fail-closed diagnostic contract for mandatory Vulkan execution.
 */
export module bioseq.pipeline.vulkan_decline;

import std;
import bioseq.common.logging;
import bioseq.gpu.vulkan.runtime;

export namespace bioseq::simd::pipe {

/// Report that a mandatory Vulkan stage did not execute.
[[noreturn]] inline void throw_vulkan_declined(std::string_view stage) {
    auto& runtime = bioseq::gpu::vulkan::runtime::instance();
    const std::string detail =
        !runtime.is_usable()
            ? fmt::format("runtime unavailable: {}", runtime.initialization_error())
        : runtime.features().software_device
            ? fmt::format("active Vulkan device '{}' is a CPU/software device "
                          "(VkPhysicalDeviceType={}); explicit GPU execution requires a "
                          "hardware Vulkan device",
                          runtime.features().device_name.data(),
                          runtime.features().device_type)
            : "GPU path declined";
    throw bioseq::gpu::vulkan::vulkan_runtime_error(
        fmt::format("{} selected but Vulkan execution did not run ({})", stage, detail));
}

} // namespace bioseq::simd::pipe
