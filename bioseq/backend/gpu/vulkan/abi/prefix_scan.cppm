/**
 * @file vulkan/abi/prefix_scan.cppm
 * @brief Defines the host/shader prefix-scan geometry contract.
 */
module;

#include <cstdint>

export module bioseq.gpu.vulkan.abi:prefix_scan;

export namespace bioseq::gpu::vulkan {

/** @brief Number of 32-bit elements assigned to each first-pass prefix-scan block. */
inline constexpr std::uint32_t k_prefix_scan_block_size = 256u;

} // namespace bioseq::gpu::vulkan
