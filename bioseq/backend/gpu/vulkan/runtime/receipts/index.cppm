/**
 * @file index.cppm
 * @brief Re-exports Vulkan runtime receipt partitions under one module name.
 */

export module bioseq.gpu.vulkan.runtime:receipts;

export import bioseq.gpu.vulkan.shader_cache;
export import bioseq.gpu.vulkan.shader_layout;
export import bioseq.gpu.vulkan.shader_variants;
export import bioseq.gpu.vulkan.abi;
export import bioseq.prefilter.types;
export import :receipts.core;
export import :receipts.metal_archive;
export import :receipts.pipeline;
