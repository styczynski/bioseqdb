/**
 * @file
 * @brief Shared alignment statistics for EXPLAIN output
 */
module;
#include <cstdint>

struct ExplainState;

export module bioseq.postgres.table:custom_scan_execution_stats_contracts;

import bioseq.highway.dispatch_contract;
import bioseq.pipeline.minimap_parameters_contract;
import bioseq.pipeline.query_sketch_plan_contract;
import bioseq.pipeline.runtime_contract;
import bioseq.pipeline.stats;

/**
 * @brief Describe the selected alphabet and query-sketch route for EXPLAIN.
 *
 * The implementation owns alphabet dispatch and the full pipeline graph. Scan
 * executors import this declaration without loading those implementation modules.
 */
export [[nodiscard]] bioseq::simd::pipe::pipeline_description
describe_pipeline_for_pg_alphabet(
    const bioseq::simd::pipe::statement_execution_plan_owner& execution_plan,
    const std::uint8_t alphabet_code,
    const bioseq::simd::pipe::query_sketch_route route);

/** Show alignment settings, timing, and work counters in EXPLAIN ANALYZE. */
export void explain_pipeline_execution_stats(
    const bioseq::pipeline_stats::pipeline_execution_stats& stats,
    ExplainState* es,
    const bioseq::simd::minimap2_parameters* aligner_params,
    const bioseq::simd::pipe::pipeline_description* pipeline_desc,
    bioseq::simd::pipe::query_sketch_route query_sketch_route,
    bool highway_dispatch_already_rendered);
