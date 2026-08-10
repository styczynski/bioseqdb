/**
 * @file
 * @brief Defines the Vulkan persisted-index identity ABI boundary.
 */
module;

#include <array>
#include <cstdint>

export module bioseq.gpu.vulkan.abi:persisted_index;

export namespace bioseq::gpu::vulkan {

/** @brief Stores the Vulkan device UUID bound to a persisted GPU index. */
using gpu_device_uuid = std::array<std::uint8_t, 16u>;
static_assert(sizeof(gpu_device_uuid) == 16u); // Vulkan VK_UUID_SIZE.

} // namespace bioseq::gpu::vulkan
