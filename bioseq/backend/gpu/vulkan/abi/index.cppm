/**
 * @file vulkan/abi/index.cppm
 * @brief Re-exports the complete dependency-light Vulkan ABI surface.
 *
 * Consumers import this primary interface instead of individual partitions.
 * Each re-export owns one host and shader contract family for sketching,
 * seed and anchor sorting, sequence batches, persisted and grouped indices,
 * prefix scans, alignment, rank prefilters, and runtime allocation helpers.
 */
export module bioseq.gpu.vulkan.abi;

export import :sketch;
export import :seed_anchor_sort;
export import :sequence_batch;
export import :persisted_index;
export import :grouped_index;
export import :prefix_scan;
export import :alignment;
export import :prefilter;
export import :runtime;
