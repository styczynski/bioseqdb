/**
 * @file instrumentation/access.cppm
 * @brief Consumer-side accessors. Summary writer dumps aggregates via JustLog.
 *
 * The summary writer takes the profiling context directly as a parameter,
 * so it does not depend on the thread-local context lookup. The dispatch
 * fork in BatchProcessor::process_sequences calls it after the pipeline
 * runs, before the context destructs.
 */
module;

#include "common.h"   // JustLog

#include <cstdint>

export module bioseq_instr.access;

import mlogging;   // brings `enable_logging` and the fmtlog wrapper into scope for JustLog

import bioseq_instr.config;
import bioseq_instr.descriptor;
import bioseq_instr.aggregate;
import bioseq_instr.thread_state;

export namespace bioseq::instr::access {

/// Dump every region/loop aggregate with non-zero visits to JustLog.
/// Called from the dispatch fork inside BatchProcessor::process_sequences
/// (Part 48.5 of the design doc). The profiler block is read directly
/// from the passed-in profiler reference, so the writer does not depend
/// on the thread-local context lookup.
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
                    const auto per_iter_mean_ns = la->total_iterations > 0
                        ? la->total_ns / la->total_iterations
                        : std::uint64_t{0};
                    JustLog("[bioseq] profile loop {} total={} ns visits={} mean={} ns "
                            "min={} ns max={} ns iters={} mean_per_iter={} ns "
                            "work={} target={} lanes={}",
                            desc->name,
                            la->total_ns, la->visits, mean_ns,
                            la->min_ns, la->max_ns,
                            la->total_iterations, per_iter_mean_ns,
                            la->total_work,
                            desc->highway_target ? desc->highway_target : "-",
                            desc->highway_lanes);
                    continue;
                }
            }
            if (auto* ra = prof.aggregates.try_get(id)) {
                if (ra->visits > 0) {
                    const auto mean_ns = ra->total_ns / ra->visits;
                    JustLog("[bioseq] profile region {} total={} ns visits={} mean={} ns "
                            "min={} ns max={} ns",
                            desc->name,
                            ra->total_ns, ra->visits, mean_ns,
                            ra->min_ns, ra->max_ns);
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
                JustLog("[bioseq] profile arena {} live={} bytes total={} bytes "
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
