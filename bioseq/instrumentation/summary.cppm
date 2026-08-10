/**
 * @file instrumentation/summary.cppm
 * @brief Percentile estimation, metric summaries, and reset operations.
 *
 * The summary writer takes a profiler_state<p_configuration_type>& as input. The non-templated
 * convenience overload routes to the persistent production profiler state.
 */
module;

#include <cmath>
#include <cstdint>
#include <limits>

export module bioseq.instrumentation.summary;

import bioseq.common.logging;

import bioseq.instrumentation.configuration;
import bioseq.instrumentation.descriptor;
import bioseq.instrumentation.aggregation;
import bioseq.instrumentation.profiler_state;

namespace bioseq::instrumentation::summary::detail {

/// Estimate a percentile from the 32-bucket log2 histogram. Returns the
/// upper bound of the bucket that contains the cumulative-count at the
/// requested fraction. Coarse but enough to flag order-of-magnitude
/// outliers; with 32 buckets the saturated bucket only kicks in for
/// regions over ~2 s, which is well past anything we benchmark. p in
/// [0, 1].
[[nodiscard]] inline std::uint64_t estimate_percentile(
    const duration_histogram& histogram,
    double percentile) noexcept
{
    constexpr unsigned bucket_count =
        static_cast<unsigned>(histogram.buckets.size());
    std::uint64_t total = 0;
    for (auto bucket_value : histogram.buckets) total += bucket_value;
    if (total == 0) return 0;
    const double bounded_percentile = percentile < 0.0
        ? 0.0
        : (percentile > 1.0 ? 1.0 : percentile);
    const std::uint64_t target = static_cast<std::uint64_t>(
        std::ceil(static_cast<double>(total) * bounded_percentile));
    const std::uint64_t required_count = target == 0 ? 1 : target;
    std::uint64_t cumulative_count = 0;
    for (unsigned bucket_index = 0; bucket_index < bucket_count; ++bucket_index) {
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

}  // namespace bioseq::instrumentation::summary::detail

export namespace bioseq::instrumentation::summary {

/// Public re-export of the bucket-percentile helper. Estimates the p-th
/// percentile (p in [0, 1]) of the values represented by a 32-bucket log2
/// histogram. The result is the upper bound of the bucket that contains
/// the cumulative-count at the requested fraction; coarse, with up to a
/// factor-of-2 overshoot vs. the bucket midpoint. Suitable for "is the
/// tail latency in the right order of magnitude" checks; not a substitute
/// for full-resolution histograms when fine-grained percentiles matter.
[[nodiscard]] inline std::uint64_t estimate_percentile(
    const duration_histogram& histogram,
    double percentile) noexcept
{
    return detail::estimate_percentile(histogram, percentile);
}

/// Dump every region/loop aggregate with non-zero visits to the debug log:
///   [bioseq] profile <region_name> total=<ns> visits=<n> mean=<ns> p50=<ns> p99=<ns>
/// Loops add per-iteration fields. ColumnarScan_EndCustomScan calls the
/// non-templated overload (in detail/summary_writer.cppm) once per scan.
template<profiling_configuration p_configuration_type>
void write_summary_to_log(profiler_state<p_configuration_type>& profiler) noexcept {
    if constexpr (!p_configuration_type::enable_profiling) {
        return;
    } else {
        const auto region_count = registered_descriptor_count();

        bioseq_log<logging_level::debug>(
            "[bioseq] profile === BEGIN summary ({} descriptors registered) ===",
            region_count);

        for (std::uint32_t descriptor_id = 1;
             descriptor_id <= region_count;
             ++descriptor_id) {
            const auto* descriptor = find_descriptor(descriptor_id);
            if (descriptor == nullptr) continue;

            // Loop aggregate first (loops also populate the region table
            // via the base aggregate fields, but the loop record is the
            // richer one to print).
            if (auto* loop_metrics = profiler.loop_aggregates.find(descriptor_id)) {
                if (loop_metrics->visits > 0) {
                    const auto mean_ns = loop_metrics->total_ns / loop_metrics->visits;
                    const auto p50 = detail::estimate_percentile(
                        loop_metrics->duration_distribution, 0.50);
                    const auto p99 = detail::estimate_percentile(
                        loop_metrics->duration_distribution, 0.99);
                    const auto mean_per_iteration_ns = loop_metrics->total_iterations > 0
                        ? loop_metrics->total_ns / loop_metrics->total_iterations
                        : std::uint64_t{0};
                    bioseq_log<logging_level::debug>(
                            "[bioseq] profile {} total={} ns visits={} mean={} ns "
                            "p50={} ns p99={} ns min={} ns max={} ns "
                            "iters={} mean_per_iter={} ns work={} target={} lanes={} "
                            "cycles={} instructions={} branches={} branch_misses={}",
                            descriptor->name,
                            loop_metrics->total_ns, loop_metrics->visits, mean_ns,
                            p50, p99,
                            loop_metrics->min_ns, loop_metrics->max_ns,
                            loop_metrics->total_iterations, mean_per_iteration_ns,
                            loop_metrics->total_work,
                            descriptor->highway_target ? descriptor->highway_target : "-",
                            descriptor->highway_lanes,
                            loop_metrics->total_cycles, loop_metrics->total_instructions,
                            loop_metrics->total_branches, loop_metrics->total_branch_misses);
                    continue;
                }
            }
            if (auto* region_metrics = profiler.aggregates.find(descriptor_id)) {
                if (region_metrics->visits > 0) {
                    const auto mean_ns = region_metrics->total_ns / region_metrics->visits;
                    const auto p50 = detail::estimate_percentile(
                        region_metrics->duration_distribution, 0.50);
                    const auto p99 = detail::estimate_percentile(
                        region_metrics->duration_distribution, 0.99);
                    bioseq_log<logging_level::debug>(
                            "[bioseq] profile {} total={} ns visits={} mean={} ns "
                            "p50={} ns p99={} ns min={} ns max={} ns "
                            "cycles={} instructions={} branches={} branch_misses={}",
                            descriptor->name,
                            region_metrics->total_ns, region_metrics->visits, mean_ns,
                            p50, p99,
                            region_metrics->min_ns, region_metrics->max_ns,
                            region_metrics->total_cycles, region_metrics->total_instructions,
                            region_metrics->total_branches, region_metrics->total_branch_misses);
                }
            }
        }

        if constexpr (p_configuration_type::enable_arena_tracking) {
            for (unsigned arena_index = 1; arena_index < arena_kind_count; ++arena_index) {
                const auto& counter = profiler.arena_counters[arena_index];
                if (counter.allocation_count == 0) continue;
                const char* kind_name = "?";
                switch (static_cast<arena_kind>(arena_index)) {
                    case arena_kind::scratch: kind_name = "scratch"; break;
                    case arena_kind::chain:   kind_name = "chain"; break;
                    case arena_kind::align:   kind_name = "align"; break;
                    case arena_kind::result:  kind_name = "result"; break;
                    case arena_kind::hash:    kind_name = "hash"; break;
                    default:                  kind_name = "?"; break;
                }
                bioseq_log<logging_level::debug>(
                        "[bioseq] profile.arena {} live={} bytes total={} bytes "
                        "allocs={} largest={} bytes",
                        kind_name, counter.bytes_live, counter.bytes_total,
                        counter.allocation_count, counter.largest_allocation_bytes);
            }
        }

        bioseq_log<logging_level::debug>("[bioseq] profile === END summary ===");
    }
}

/// Reset all aggregates on the passed-in profiler block.
template<profiling_configuration p_configuration_type>
void reset_metrics(profiler_state<p_configuration_type>& profiler) noexcept {
    if constexpr (!p_configuration_type::enable_profiling) {
        return;
    } else {
        const auto descriptor_count = registered_descriptor_count();
        for (std::uint32_t descriptor_id = 1;
             descriptor_id <= descriptor_count;
             ++descriptor_id) {
            if (auto* aggregate = profiler.aggregates.find(descriptor_id)) {
                initialize_aggregate(*aggregate);
            }
            if (auto* loop_metrics = profiler.loop_aggregates.find(descriptor_id)) {
                initialize_loop_aggregate(*loop_metrics);
            }
        }
        for (auto& counter : profiler.arena_counters) {
            counter = arena_counter{};
        }
    }
}

}  // namespace bioseq::instrumentation::summary
