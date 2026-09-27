/**
 * @file operational_stats.cppm
 * @brief Keeps execution counters within the range of PostgreSQL bigint.
 */
module;

#include <cstdint>

export module bioseq.postgres.scheduler.dispatch_processor.operational_stats;

export import bioseq.pipeline.stats;

export namespace bioseq::simd::typed_dispatch::typed_support_detail {

/**
 * @brief Adds to a counter, clamping the sum to the PostgreSQL bigint maximum.
 *
 * Records clamping in stats so readers can tell that the count is incomplete.
 * An earlier clamping event remains recorded.
 * @param[in,out] stats Execution statistics that own the saturation flag.
 * @param[in,out] destination Counter to update.
 * @param[in] increment Value to add.
 */
inline void add_operational_counter(::bioseq::pipeline_stats::pipeline_execution_stats& stats,
                                    std::uint64_t& destination,
                                    const std::uint64_t increment = 1) noexcept {
    stats.operational.are_stats_saturated =
        ::bioseq::pipeline_stats::merge_counter(destination, increment) ||
        stats.operational.are_stats_saturated;
}

} // namespace bioseq::simd::typed_dispatch::typed_support_detail
