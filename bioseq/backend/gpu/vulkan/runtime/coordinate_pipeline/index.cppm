/**
 * @file index.cppm
 * @brief Exports the single-query and batched GPU coordinate pipelines.
 */

export module bioseq.gpu.vulkan.runtime:coordinate_pipeline;

export import :coordinate_pipeline.batch;
export import :coordinate_pipeline.single;
