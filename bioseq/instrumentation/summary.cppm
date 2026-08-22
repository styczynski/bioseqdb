/**
 * @file instrumentation/summary.cppm
 * @brief Percentile estimation, metric summaries, and reset operations.
 *
 * Templated operations consume explicit profiler state. Convenience entry
 * points use the persistent production state.
 */
module;

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

export module bioseq.instrumentation.summary;

import bioseq.common.logging;

import bioseq.instrumentation.configuration;
import bioseq.instrumentation.descriptor;
import bioseq.instrumentation.aggregation;
import bioseq.instrumentation.profiler_state;

namespace bioseq::instrumentation::summary::detail {

/** @brief Arena values in the stable summary-output order, including the sentinel. */
inline constexpr std::array k_arena_kinds{
    arena_kind::none,
    arena_kind::scratch,
    arena_kind::chain,
    arena_kind::align,
    arena_kind::result,
    arena_kind::hash,
};
static_assert(k_arena_kinds.size() == arena_kind_count);

/**
 * @brief Map an arena enum to its stable summary field value.
 *
 * @param[in] kind Arena category to render.
 * @return Static string used by the stable summary format.
 */
[[nodiscard]] constexpr const char* arena_kind_name(arena_kind kind) noexcept {
    switch (kind) {
    case arena_kind::none:
        return "?";
    case arena_kind::scratch:
        return "scratch";
    case arena_kind::chain:
        return "chain";
    case arena_kind::align:
        return "align";
    case arena_kind::result:
        return "result";
    case arena_kind::hash:
        return "hash";
    default:
        return "?";
    }
}

/**
 * @brief Estimate a percentile from a saturated 32-bucket base-two histogram.
 *
 * The result is the upper bound of the first bucket whose cumulative count
 * reaches the requested fraction. This intentionally preserves the coarse,
 * order-of-magnitude estimate used by summary logging.
 *
 * @param[in] histogram Nanosecond-duration histogram to inspect.
 * @param[in] percentile Requested fraction, clamped to `[0, 1]`.
 * @return Inclusive upper bound of the selected bucket, or zero when empty.
 */
[[nodiscard]] inline std::uint64_t estimate_percentile(const duration_histogram& histogram,
                                                       double percentile) noexcept {
    constexpr std::size_t bucket_count = histogram.buckets.size();
    std::uint64_t total_count = 0;
    for (const auto bucket_value : histogram.buckets) {
        total_count += bucket_value;
    }
    if (total_count == 0) {
        return 0;
    }
    const double bounded_percentile =
        percentile < 0.0 ? 0.0 : (percentile > 1.0 ? 1.0 : percentile);
    const std::uint64_t target_count = static_cast<std::uint64_t>(
        std::ceil(static_cast<double>(total_count) * bounded_percentile));
    const std::uint64_t required_count = target_count == 0 ? 1 : target_count;
    std::uint64_t cumulative_count = 0;
    for (std::size_t bucket_index = 0; bucket_index < bucket_count; ++bucket_index) {
        cumulative_count += histogram.buckets[bucket_index];
        if (cumulative_count >= required_count) {
            // Bucket i covers values with floor(log2(v)) == i, so the
            // upper bound is 2^(i+1) - 1. Guard against undefined
            // 1<<63 by clamping the last bucket.
            if (bucket_index + 1 >= 64) {
                return std::numeric_limits<std::uint64_t>::max();
            }
            return (1ull << (bucket_index + 1)) - 1ull;
        }
    }
    return std::numeric_limits<std::uint64_t>::max();
}

} // namespace bioseq::instrumentation::summary::detail

export namespace bioseq::instrumentation::summary {

/**
 * @brief Estimate a percentile from a 32-bucket base-two duration histogram.
 *
 * The estimate returns a bucket upper bound and can overshoot the bucket
 * midpoint by up to a factor of two. It is intended for order-of-magnitude
 * latency summaries rather than fine-grained percentile analysis.
 *
 * @param[in] histogram Nanosecond-duration histogram to inspect.
 * @param[in] percentile Requested fraction, clamped to `[0, 1]`.
 * @return Inclusive upper bound of the selected bucket, or zero when empty.
 */
[[nodiscard]] inline std::uint64_t estimate_percentile(const duration_histogram& histogram,
                                                       double percentile) noexcept {
    return detail::estimate_percentile(histogram, percentile);
}

/**
 * @brief Write every visited region, loop, and tracked arena to the debug log.
 *
 * Descriptor order, field names, units, and formatting are stable: durations
 * are nanoseconds, PMU fields are raw deltas, and loop rows precede region rows
 * for the same descriptor. Disabled configurations compile to a no-op.
 *
 * @tparam p_configuration_type Compile-time instrumentation feature policy.
 * @param[in] profiler Quiescent profiler state to summarize without resetting.
 */
template <profiling_configuration p_configuration_type>
void write_summary_to_log(profiler_state<p_configuration_type>& profiler) noexcept {
    if constexpr (!p_configuration_type::enable_profiling) {
        return;
    } else {
        const auto descriptor_count = registered_descriptor_count();

        bioseq_log<logging_level::debug>(
            "[bioseq] profile === BEGIN summary ({} descriptors registered) ===", descriptor_count);

        for (std::uint32_t descriptor_id = 1; descriptor_id <= descriptor_count; ++descriptor_id) {
            const auto* descriptor = find_descriptor(descriptor_id);
            if (descriptor == nullptr) {
                continue;
            }

            // Loop aggregate first (loops also populate the region table
            // via the base aggregate fields, but the loop record is the
            // richer one to print).
            if (auto* const loop_metrics = profiler.loop_aggregates.find(descriptor_id);
                loop_metrics != nullptr && loop_metrics->visits > 0) {
                const auto mean_ns = loop_metrics->total_ns / loop_metrics->visits;
                const auto p50_ns =
                    detail::estimate_percentile(loop_metrics->duration_distribution, 0.50);
                const auto p99_ns =
                    detail::estimate_percentile(loop_metrics->duration_distribution, 0.99);
                const auto mean_per_iteration_ns =
                    loop_metrics->total_iterations > 0
                        ? loop_metrics->total_ns / loop_metrics->total_iterations
                        : std::uint64_t{0};
                bioseq_log<logging_level::debug>(
                    "[bioseq] profile {} total={} ns visits={} mean={} ns "
                    "p50={} ns p99={} ns min={} ns max={} ns "
                    "iters={} mean_per_iter={} ns work={} target={} lanes={} "
                    "cycles={} instructions={} branches={} branch_misses={}",
                    descriptor->name,
                    loop_metrics->total_ns,
                    loop_metrics->visits,
                    mean_ns,
                    p50_ns,
                    p99_ns,
                    loop_metrics->min_ns,
                    loop_metrics->max_ns,
                    loop_metrics->total_iterations,
                    mean_per_iteration_ns,
                    loop_metrics->total_work,
                    descriptor->highway_target ? descriptor->highway_target : "-",
                    descriptor->highway_lanes,
                    loop_metrics->total_cycles,
                    loop_metrics->total_instructions,
                    loop_metrics->total_branches,
                    loop_metrics->total_branch_misses);
                continue;
            }
            if (auto* const region_metrics = profiler.aggregates.find(descriptor_id);
                region_metrics != nullptr && region_metrics->visits > 0) {
                const auto mean_ns = region_metrics->total_ns / region_metrics->visits;
                const auto p50_ns =
                    detail::estimate_percentile(region_metrics->duration_distribution, 0.50);
                const auto p99_ns =
                    detail::estimate_percentile(region_metrics->duration_distribution, 0.99);
                bioseq_log<logging_level::debug>(
                    "[bioseq] profile {} total={} ns visits={} mean={} ns "
                    "p50={} ns p99={} ns min={} ns max={} ns "
                    "cycles={} instructions={} branches={} branch_misses={}",
                    descriptor->name,
                    region_metrics->total_ns,
                    region_metrics->visits,
                    mean_ns,
                    p50_ns,
                    p99_ns,
                    region_metrics->min_ns,
                    region_metrics->max_ns,
                    region_metrics->total_cycles,
                    region_metrics->total_instructions,
                    region_metrics->total_branches,
                    region_metrics->total_branch_misses);
            }
        }

        if constexpr (p_configuration_type::enable_arena_tracking) {
            for (const arena_kind kind : std::span{detail::k_arena_kinds}.subspan(1)) {
                const auto arena_index = static_cast<std::size_t>(kind);
                const auto& arena_metrics = profiler.arena_counters[arena_index];
                if (arena_metrics.allocation_count == 0) {
                    continue;
                }
                bioseq_log<logging_level::debug>(
                    "[bioseq] profile.arena {} live={} bytes total={} bytes "
                    "allocs={} largest={} bytes",
                    detail::arena_kind_name(kind),
                    arena_metrics.bytes_live,
                    arena_metrics.bytes_total,
                    arena_metrics.allocation_count,
                    arena_metrics.largest_allocation_bytes);
            }
        }

        bioseq_log<logging_level::debug>("[bioseq] profile === END summary ===");
    }
}

/**
 * @brief Reset all descriptor and arena aggregates in a profiler block.
 *
 * @tparam p_configuration_type Compile-time instrumentation feature policy.
 * @param[in,out] profiler Profiler state whose recorded measurements are discarded.
 */
template <profiling_configuration p_configuration_type>
void reset_metrics(profiler_state<p_configuration_type>& profiler) noexcept {
    if constexpr (!p_configuration_type::enable_profiling) {
        return;
    } else {
        const auto descriptor_count = registered_descriptor_count();
        for (std::uint32_t descriptor_id = 1; descriptor_id <= descriptor_count; ++descriptor_id) {
            if (auto* const aggregate = profiler.aggregates.find(descriptor_id)) {
                initialize_aggregate(*aggregate);
            }
            if (auto* const loop_metrics = profiler.loop_aggregates.find(descriptor_id)) {
                initialize_loop_aggregate(*loop_metrics);
            }
        }
        for (auto& arena_metrics : profiler.arena_counters) {
            arena_metrics = arena_counter{};
        }
    }
}

} // namespace bioseq::instrumentation::summary
