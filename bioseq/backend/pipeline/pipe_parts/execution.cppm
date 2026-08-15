/**
 * @file pipeline/pipe_parts/execution.cppm
 * @brief Re-exports the execution partitions.
 */
export module bioseq.pipeline.pipe:execution;

export import :execution_plan_context;
export import :execution_scalar_highway;
export import :execution_vulkan;
export import :execution_results;
