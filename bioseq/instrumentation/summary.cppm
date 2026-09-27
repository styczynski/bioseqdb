/**
 * @file instrumentation/summary.cppm
 * @brief Estimate duration percentiles, print measurements, and reset profiler state.
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

/** @brief Printed order of memory categories, starting with none. */
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
 * @brief Return the name printed for a memory category.
 * @param[in] kind Memory category.
 * @return Category name, or ? for none and unknown values.
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
 * @brief Estimate a percentile from duration buckets.
 *
 * Use the first bucket that reaches the requested fraction. Its normal upper
 * bound is the estimate, even for the final  bucket that also holds larger values.
 *
 * @param[in] histogram Duration counts in nanoseconds.
 * @param[in] percentile Requested fraction, clamped to [0, 1].
 * @return Bucket estimate, or zero for an wmpty histogram.
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
            // Use the normal upper bound of this bucket The final bucket may
            // also contain larger durations. Check the shift width before shifting.
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
 * @brief Estimate a duration percentile from the histogram.
 *
 * The result is approximate because each bucket covers a range of durations.
 * The final bucket returns 2^32 - 1 nanoseconds even when  it contains longer visits.
 *
 * @param[in] histogram Duration counts in nanoseconds.
 * @param[in] percentile Requested fraction, clamped to [0, 1].
 * @return Bucket estimate, or zero for an empty histogram.
 */
[[nodiscard]] inline std::uint64_t estimate_percentile(const duration_histogram& histogram,
                                                       double percentile) noexcept {
    return detail::estimate_percentile(histogram, percentile);
}

/**
 * @brief Write recorded region, loop, and memory-category measurements to the debug log.
 *
 * Durations use nanoseconds. Hardware counters report raw changes.
 * For a descriptor with loop measurements, print that row and skip its region row.
 * Disabled profiling produces no output.
 *
 * @tparam p_configuration_type Profiling configuration.
 * @param[in] profiler State to read after all writers have stopped updating it.
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

            // Prefer the loop row, because it also includes iteration and work counts.
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
 * @brief Clear recorded region, loop, and  memory-category measurements.
 * @tparam p_configuration_type Profiling configuration.
 * @param[in,out] profiler State to reset after its writers stop updating it.
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
