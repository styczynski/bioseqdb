/**
 * @file vulkan/abi/persisted_index.cppm
 * @brief Defines the Vulkan persisted-index identity ABI boundary.
 */
module;

#include <array>
#include <cstdint>

export module bioseq.gpu.vulkan.abi:persisted_index;

export namespace bioseq::gpu::vulkan {

/**
 * @brief Represents the Vulkan device UUID bound to a persisted GPU index.
 *
 * The bytes preserve Vulkan's device identity without text encoding so readers can reject an
 * index created for a different physical device.
 */
using gpu_device_uuid = std::array<std::uint8_t, 16u>;
static_assert(sizeof(gpu_device_uuid) == 16u); // Vulkan VK_UUID_SIZE.

} // namespace bioseq::gpu::vulkan
