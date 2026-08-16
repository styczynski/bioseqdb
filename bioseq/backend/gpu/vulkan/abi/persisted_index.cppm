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
 * @brief Opaque Vulkan device UUID stored with a persisted GPU index.
 *
 * The 16-byte layout matches `VK_UUID_SIZE`. Readers compare the bytes directly
 * and reject an index built on a different physical device.
 */
using gpu_device_uuid = std::array<std::uint8_t, 16u>;
// Matches Vulkan VK_UUID_SIZE.
static_assert(sizeof(gpu_device_uuid) == 16u);

} // namespace bioseq::gpu::vulkan
