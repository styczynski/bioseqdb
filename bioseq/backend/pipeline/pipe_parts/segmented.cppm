/**
 * @file pipeline/pipe_parts/segmented.cppm
 * @brief Exports planning and execution for targets processed in segments.
 */
export module bioseq.pipeline.pipe:segmented;

export import :segmented_planning;
export import :segmented_finalization;
export import :segmented_execution;
