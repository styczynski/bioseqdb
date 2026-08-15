/**
 * @file pipeline/minimap_port.cppm
 * @brief Public facade for the minimap2-compatible mapping pipeline.
 */
export module bioseq.pipeline.minimap_port;

export import bioseq.highway.dispatch_contract;
export import bioseq.prefilter.execution_contract;
export import :results;
export import :seeding_chaining;
export import :alignment;
export import :postprocessing;
export import :execution_stats;
