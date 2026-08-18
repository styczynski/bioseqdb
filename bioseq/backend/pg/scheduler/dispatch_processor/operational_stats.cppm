/**
 * @file operational_stats.cppm
 * @brief Updates saturating counters shared by typed dispatch stages.
 */
export module bioseq.postgres.scheduler.dispatch_processor.operational_stats;

import std;
export import bioseq.pipeline.stats;

export namespace bioseq::simd::typed_dispatch::typed_support_detail {

/**
 * @brief Adds one operational counter and records saturation.
 * @param[in,out] stats Execution statistics that own the saturation flag.
 * @param[in,out] destination Counter to update.
 * @param[in] increment Value added to the counter.
 */
inline void add_operational_counter(::bioseq::pipeline_stats::pipeline_execution_stats& stats,
                                    std::uint64_t& destination,
                                    const std::uint64_t increment = 1) noexcept {
    stats.operational.are_stats_saturated =
        ::bioseq::pipeline_stats::merge_counter(destination, increment) ||
        stats.operational.are_stats_saturated;
}

} // namespace bioseq::simd::typed_dispatch::typed_support_detail
