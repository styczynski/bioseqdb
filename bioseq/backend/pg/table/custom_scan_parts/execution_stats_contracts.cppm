/**
 * @file
 * @brief Declares shared EXPLAIN statistics rendering without PostgreSQL header fanout.
 */
module;
struct ExplainState;

export module bioseq.postgres.table:custom_scan_execution_stats_contracts;

import bioseq.highway.dispatch_contract;
import bioseq.pipeline.minimap_parameters_contract;
import bioseq.pipeline.query_sketch_plan_contract;
import bioseq.pipeline.runtime_contract;
import bioseq.pipeline.stats;

/** @brief Render pipeline counters and selected alignment policy for EXPLAIN ANALYZE. */
export void explain_pipeline_execution_stats(
    const bioseq::pipeline_stats::pipeline_execution_stats& stats,
    ExplainState* es,
    const bioseq::simd::minimap2_parameters* aligner_params,
    const bioseq::simd::pipe::pipeline_description* pipeline_desc,
    bioseq::simd::pipe::query_sketch_route query_sketch_route,
    bool highway_dispatch_already_rendered);
