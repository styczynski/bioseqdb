/**
 * @file runtime.cppm
 * @brief Exposes the Vulkan runtime API and its operation receipt contracts.
 *
 * The facade preserves the public module name while semantic partitions own
 * receipt types, frontend wire types, runtime layout, device lifetime, caches,
 * and GPU algorithm implementations.
 */

export module bioseq.gpu.vulkan.runtime;

export import bioseq.gpu.vulkan.shader_cache;
export import bioseq.gpu.vulkan.shader_layout;
export import bioseq.gpu.vulkan.shader_variants;
export import bioseq.gpu.vulkan.abi;
export import bioseq.prefilter.types;
export import :receipts;
export import :frontend_types;
