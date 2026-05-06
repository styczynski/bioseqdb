/**
 * @file instrumentation/access.cppm
 * @brief Consumer-side accessors. Summary writer dumps aggregates via JustLog.
 *
 * The summary writer takes a ProfilerBlock<Args>& as input. The non-templated
 * convenience overload `write_summary_to_justlog()` lives in
 * `instrumentation/detail/summary_writer.cppm` and routes to the persistent
 * process-wide block; ColumnarScan_EndCustomScan calls it once per scan.
 */
module;

#include "common.h"   // JustLog

#include <array>
#include <cstdint>
#include <limits>

export module bioseq_instr.access;

import mlogging;   // brings `enable_logging` and the fmtlog wrapper into scope for JustLog

import bioseq_instr.config;
import bioseq_instr.descriptor;
import bioseq_instr.aggregate;
import bioseq_instr.thread_state;

export namespace bioseq::instr::access {

namespace detail {

/// Estimate a percentile from the 32-bucket log2 histogram. Returns the
/// upper bound of the bucket that contains the cumulative-count at the
/// requested fraction. Coarse but enough to flag order-of-magnitude
/// outliers; with 32 buckets the saturated bucket only kicks in for
/// regions over ~2 s, which is well past anything we benchmark. p in
/// [0, 1].
[[nodiscard]] inline std::uint64_t hist_percentile(
    const Histogram16& h, double p) noexcept
{
    constexpr unsigned kBuckets = static_cast<unsigned>(h.buckets.size());
    std::uint64_t total = 0;
    for (auto v : h.buckets) total += v;
    if (total == 0) return 0;
    const std::uint64_t target = static_cast<std::uint64_t>(
        static_cast<double>(total) * p);
    std::uint64_t cum = 0;
    for (unsigned i = 0; i < kBuckets; ++i) {
        cum += h.buckets[i];
        if (cum >= target) {
            // Bucket i covers values with floor(log2(v)) == i, so the
            // upper bound is 2^(i+1) - 1. Guard against undefined
            // 1<<63 by clamping the last bucket.
            if (i + 1 >= 64) return std::numeric_limits<std::uint64_t>::max();
            return (1ull << (i + 1)) - 1ull;
        }
    }
    return std::numeric_limits<std::uint64_t>::max();
}

}  // namespace detail

/// Public re-export of the bucket-percentile helper. Estimates the p-th
/// percentile (p in [0, 1]) of the values represented by a 32-bucket log2
/// histogram. The result is the upper bound of the bucket that contains
/// the cumulative-count at the requested fraction; coarse, with up to a
/// factor-of-2 overshoot vs. the bucket midpoint. Suitable for "is the
/// tail latency in the right order of magnitude" checks; not a substitute
/// for full-resolution histograms when fine-grained percentiles matter.
[[nodiscard]] inline std::uint64_t hist_percentile(
    const Histogram16& histogram, double p) noexcept
{
    return detail::hist_percentile(histogram, p);
}

/// Dump every region/loop aggregate with non-zero visits to JustLog.
/// Output format mirrors Part 48.5 of NEW_PROFILING_RESEARCH.md:
///   [bioseq] profile <region_name> total=<ns> visits=<n> mean=<ns> p50=<ns> p99=<ns>
/// Loops add per-iteration fields. ColumnarScan_EndCustomScan calls the
/// non-templated overload (in detail/summary_writer.cppm) once per scan.
template<is_context_args Args>
void write_summary_to_justlog(ProfilerBlock<Args>& prof) noexcept {
    if constexpr (!Args::enable_profiling) {
        return;
    } else {
        const auto n_regions = descriptor_count();

        JustLog("[bioseq] profile === BEGIN summary ({} descriptors registered) ===",
                n_regions);

        for (std::uint32_t id = 1; id <= n_regions; ++id) {
            const auto* desc = descriptor_lookup(id);
            if (desc == nullptr) continue;

            // Loop aggregate first (loops also populate the region table
            // via the base aggregate fields, but the loop record is the
            // richer one to print).
            if (auto* la = prof.loop_aggregates.try_get(id)) {
                if (la->visits > 0) {
                    const auto mean_ns = la->total_ns / la->visits;
                    const auto p50 = detail::hist_percentile(la->ns_hist, 0.50);
                    const auto p99 = detail::hist_percentile(la->ns_hist, 0.99);
                    const auto per_iter_mean_ns = la->total_iterations > 0
                        ? la->total_ns / la->total_iterations
                        : std::uint64_t{0};
                    JustLog("[bioseq] profile {} total={} ns visits={} mean={} ns "
                            "p50={} ns p99={} ns min={} ns max={} ns "
                            "iters={} mean_per_iter={} ns work={} target={} lanes={} "
                            "cycles={} instructions={} branches={} branch_misses={}",
                            desc->name,
                            la->total_ns, la->visits, mean_ns,
                            p50, p99,
                            la->min_ns, la->max_ns,
                            la->total_iterations, per_iter_mean_ns,
                            la->total_work,
                            desc->highway_target ? desc->highway_target : "-",
                            desc->highway_lanes,
                            la->total_cycles, la->total_instructions,
                            la->total_branches, la->total_branch_misses);
                    continue;
                }
            }
            if (auto* ra = prof.aggregates.try_get(id)) {
                if (ra->visits > 0) {
                    const auto mean_ns = ra->total_ns / ra->visits;
                    const auto p50 = detail::hist_percentile(ra->ns_hist, 0.50);
                    const auto p99 = detail::hist_percentile(ra->ns_hist, 0.99);
                    JustLog("[bioseq] profile {} total={} ns visits={} mean={} ns "
                            "p50={} ns p99={} ns min={} ns max={} ns "
                            "cycles={} instructions={} branches={} branch_misses={}",
                            desc->name,
                            ra->total_ns, ra->visits, mean_ns,
                            p50, p99,
                            ra->min_ns, ra->max_ns,
                            ra->total_cycles, ra->total_instructions,
                            ra->total_branches, ra->total_branch_misses);
                }
            }
        }

        if constexpr (Args::enable_arena_tracking) {
            for (unsigned k = 1; k < kArenaKindCount; ++k) {
                const auto& c = prof.arena_counters[k];
                if (c.alloc_count == 0) continue;
                const char* kind_name = "?";
                switch (static_cast<arena_kind>(k)) {
                    case arena_kind::scratch: kind_name = "scratch"; break;
                    case arena_kind::chain:   kind_name = "chain"; break;
                    case arena_kind::align:   kind_name = "align"; break;
                    case arena_kind::result:  kind_name = "result"; break;
                    case arena_kind::hash:    kind_name = "hash"; break;
                    default:                  kind_name = "?"; break;
                }
                JustLog("[bioseq] profile.arena {} live={} bytes total={} bytes "
                        "allocs={} largest={} bytes",
                        kind_name, c.bytes_live, c.bytes_total,
                        c.alloc_count, c.largest);
            }
        }

        JustLog("[bioseq] profile === END summary ===");
    }
}

/// Reset all aggregates on the passed-in profiler block.
template<is_context_args Args>
void reset_all(ProfilerBlock<Args>& prof) noexcept {
    if constexpr (!Args::enable_profiling) {
        return;
    } else {
        const auto n = descriptor_count();
        for (std::uint32_t id = 1; id <= n; ++id) {
            if (auto* a = prof.aggregates.try_get(id)) aggregate_init(*a);
            if (auto* la = prof.loop_aggregates.try_get(id)) loop_aggregate_init(*la);
        }
        for (auto& c : prof.arena_counters) {
            c = ArenaCounter{};
        }
    }
}

}  // namespace bioseq::instr::access
