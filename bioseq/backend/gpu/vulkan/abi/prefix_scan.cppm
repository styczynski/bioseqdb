/**
 * @file vulkan/abi/prefix_scan.cppm
 * @brief Block size shared by CPU dispatch code and GPU prefix scans.
 */
module;

export module bioseq.gpu.vulkan.abi:prefix_scan;

import std;

export namespace bioseq::gpu::vulkan {

/**
 * @brief Number of 32-bit values in one prefix-scan block
 *
 * The host passes this as `block_size` and rounds up the element count to
 * determine  how many blocks to dispatch.
 */
inline constexpr std::uint32_t k_prefix_scan_block_size = 256u;

} // namespace bioseq::gpu::vulkan
