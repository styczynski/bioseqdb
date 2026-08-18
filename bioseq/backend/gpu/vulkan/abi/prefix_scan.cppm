/**
 * @file vulkan/abi/prefix_scan.cppm
 * @brief Defines the host/shader prefix-scan geometry contract.
 */
module;

export module bioseq.gpu.vulkan.abi:prefix_scan;

import std;

export namespace bioseq::gpu::vulkan {

/**
 * @brief Number of 32-bit elements scanned by one prefix-scan block.
 *
 * Host planners pass this value as the `block_size` push constant and derive
 * `num_blocks` with a ceiling division over the logical element count.
 */
inline constexpr std::uint32_t k_prefix_scan_block_size = 256u;

} // namespace bioseq::gpu::vulkan
