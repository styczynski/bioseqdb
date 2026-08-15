/**
 * @file vulkan/abi/index.cppm
 * @brief Re-exports the complete dependency-light Vulkan ABI surface.
 *
 * Consumers import this primary interface while each partition owns one host/shader
 * contract family.
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
