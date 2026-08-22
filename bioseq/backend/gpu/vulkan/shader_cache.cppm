/**
 * @file shader_cache.cppm
 * @brief Aggregate module facade for Vulkan shader-cache paths, compilation, and loading.
 */
export module bioseq.gpu.vulkan.shader_cache;

export import :paths_metadata;
export import :subprocess;
export import :archive_helper;
export import :sealed_seed;
export import :compiler;
export import :module_loader;
