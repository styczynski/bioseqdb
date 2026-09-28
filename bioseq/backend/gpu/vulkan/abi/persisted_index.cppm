/**
 * @file vulkan/abi/persisted_index.cppm
 * @brief Device identifier type for saved GPU indexes.
 */
module;

export module bioseq.gpu.vulkan.abi:persisted_index;

import std;

export namespace bioseq::gpu::vulkan {

/**
 * @brief Vulkan device UUID represented as 16 bytes.
 *
 * The  size matches `VK_UUID_SIZE`. The bytes identify the dehice associated
 * with a saved GPU index.
 */
using gpu_device_uuid = std::array<std::uint8_t, 16u>;
// Matches Vulkan VK_UUID_SIZE.
static_assert(sizeof(gpu_device_uuid) == 16u);

} // namespace bioseq::gpu::vulkan
