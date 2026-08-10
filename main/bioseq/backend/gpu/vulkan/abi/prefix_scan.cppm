/**
 * @file
 * @brief Defines the Vulkan prefix-scan ABI constants.
 */
module;

#include <cstdint>

export module bioseq.gpu.vulkan.abi:prefix_scan;

export namespace bioseq::gpu::vulkan {

/** @brief Element count processed by one prefix-scan block. */
inline constexpr std::uint32_t k_prefix_scan_block_size = 256u;

} // namespace bioseq::gpu::vulkan
