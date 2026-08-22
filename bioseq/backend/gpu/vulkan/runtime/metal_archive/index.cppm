/**
 * @file index.cppm
 * @brief Preserves the runtime Metal archive boundary across semantic partitions.
 *
 * Re-exports configuration admission, interposition hooks, and the live gate
 * as one stable import surface for Vulkan runtime consumers.
 */

export module bioseq.gpu.vulkan.runtime:metal_archive;

export import :metal_archive.configuration;
export import :metal_archive.interpose;
export import :metal_archive.live_gate;
