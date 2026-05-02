/**
 * @file instrumentation_report.cppm
 * @brief Domain-agnostic profiler-table printers shared across benches.
 *
 * Every harness in the project wires `bioseq::instr::region(...)` /
 * `loop(...)` scopes inside its hot paths and wants the same five
 * tables in the post-run report:
 *
 *   1. **Region + loop summary** — name, visits, total_ns, mean,
 *      p50, p99, min, max, iter/s, throughput, lanes, target.
 *   2. **PMU summary** — cycles, IPC, instructions, branches,
 *      branch-miss rate.
 *   3. **Arena summary** — per-arena live / total bytes / alloc
 *      counts.
 *   4. **Top-N bottlenecks** by total_ns.
 *   5. **Top-N call-frequency** by visits.
 *
 * All five tables are pure functions over the global
 * `bioseq::instr::persistent_profiler_block<context_args_profiling_timing_only>`.
 * The only domain-specific input is the descriptor → display-name
 * mapping (the aligner prefixes `minimap_port.*` /
 * `tool_minimap2.*` based on the descriptor's source file). Callers
 * pass that mapping as a `DescriptorRemapFn`; the default returns
 * the descriptor's own `name` field unchanged.
 *
 * `InstalledProfiler` and `reset_aggregates()` are also exported here
 * — they're generic instrumentation glue every harness's TEST_CASEs
 * use to scope the profiler around a section.
 */
module;

#include "common.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

export module bench.scheduler.instrumentation_report;

import mlogging;
import bioseq_instr;
import bench.scheduler.format;
import bench.scheduler.stats;

export {

namespace bioseq::bench_scheduler {

/// Map an in-source descriptor to a render-time display name. Default
/// implementation just returns `desc->name`; harnesses that want
/// per-source-file prefixing (e.g. the aligner's
/// `minimap_port.*` / `tool_minimap2.*`) supply their own.
using DescriptorRemapFn =
    std::function<std::string(const bioseq::instr::RegionDescriptor*)>;

[[nodiscard]] inline DescriptorRemapFn default_descriptor_name_remap() {
    return [](const bioseq::instr::RegionDescriptor* desc) -> std::string {
        return desc != nullptr && desc->name != nullptr
            ? std::string{desc->name} : std::string{"<anonymous>"};
    };
}

/// The canonical bench profiler context. Every harness uses
/// `context_args_profiling_timing_only` so the
/// `bioseq::instr::region(...)` / `loop(...)` call sites in the
/// chaining / kmers / minimap_port code see a non-null
/// `tls_profiler<context_args_profiling_timing_only>`.
using profiler_args_t = bioseq::instr::context_args_profiling_timing_only;

/// Walk every visited descriptor and call `visitor(desc, region_agg,
/// loop_agg)`. `region_agg` / `loop_agg` are nullable; `loop_agg` is
/// non-null only when the descriptor's id is also present in the loop
/// table AND the loop has been visited.
template<class Fn>
inline void for_each_visited_descriptor(Fn&& visitor) noexcept {
    auto& prof = bioseq::instr::persistent_profiler_block<profiler_args_t>();
    const auto n = bioseq::instr::descriptor_count();
    for (std::uint32_t id = 1; id <= n; ++id) {
        const auto* desc = bioseq::instr::descriptor_lookup(id);
        if (desc == nullptr) continue;
        auto* la = prof.loop_aggregates.try_get(id);
        auto* ra = prof.aggregates.try_get(id);
        const bool loop_visited   = (la != nullptr && la->visits > 0);
        const bool region_visited = (ra != nullptr && ra->visits > 0);
        if (!loop_visited && !region_visited) continue;
        visitor(desc, region_visited ? ra : nullptr, loop_visited ? la : nullptr);
    }
}

/// p50 / p99 percentile estimate from a log2 histogram. Mirrors the
/// canonical implementation in `bioseq_instr.access`; pulled here so
/// the report's loop has the lookup against a non-const member ref
/// without depending on `access.cppm`'s exact layout.
[[nodiscard]] inline std::uint64_t hist_percentile(
    const bioseq::instr::Histogram16& h, double p) noexcept
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
            if (i + 1 >= 64) return std::uint64_t{0xFFFFFFFFFFFFFFFFull};
            return (1ull << (i + 1)) - 1ull;
        }
    }
    return std::uint64_t{0xFFFFFFFFFFFFFFFFull};
}

/// Region + loop summary.
inline void print_instrumentation_summary(
    std::string_view section_name,
    const DescriptorRemapFn& remap = default_descriptor_name_remap())
{
    using std::cout;
    cout << "\n" << format::rule() << "\n";
    cout << fmt::format("[bench] profile summary  section=\"{}\"\n", section_name);
    cout << format::rule() << "\n";
    cout << fmt::format(
        "{:<48} {:>9} {:>18} {:>11} {:>11} {:>11} {:>11} {:>11} {:>14} {:>14} {:>6} {:>9}\n",
        "name", "visits", "total_ns",
        "mean", "p50", "p99", "min", "max",
        "iter/s", "bytes/s", "lanes", "target");
    cout << format::rule() << "\n";

    bool printed_any = false;
    for_each_visited_descriptor(
        [&](const bioseq::instr::RegionDescriptor* desc,
            const bioseq::instr::RegionAggregate* ra,
            const bioseq::instr::LoopAggregate* la) {
        const auto* base = la != nullptr
            ? static_cast<const bioseq::instr::RegionAggregate*>(la) : ra;
        if (base == nullptr) return;
        const auto mean_ns = base->visits > 0 ? base->total_ns / base->visits : 0;
        // Single-sample percentiles are noise — the histogram has only
        // one bucket populated and the readout is just the bucket-end
        // constant. Suppress with `-` until at least two visits.
        const std::string p50_str = base->visits >= 2
            ? format::fmt_duration_10(hist_percentile(base->ns_hist, 0.50))
            : fmt::format("{:>11}", "-");
        const std::string p99_str = base->visits >= 2
            ? format::fmt_duration_10(hist_percentile(base->ns_hist, 0.99))
            : fmt::format("{:>11}", "-");

        std::string iters_str = fmt::format("{:>14}", "-");
        std::string thrpt_str = fmt::format("{:>14}", "-");
        std::string lanes_str = "-";
        std::string target_str= "-";
        if (la != nullptr) {
            iters_str = format::fmt_iters_per_sec(la->total_iterations, la->total_ns);
            // Bench-level loops tick by `bp_size()` (bytes), so GB/s is
            // correct for them. Loops that tick by `1` per iteration
            // (the default in the chaining inner-loops) get a
            // meaningless work value — gate throughput on the loop
            // name prefix `bench.` which is the byte-typed work
            // tracker the harness ships.
            const std::string_view nm{desc->name};
            const bool is_byte_unit = nm.starts_with("bench.");
            thrpt_str = fmt::format("{:>14}",
                format::fmt_throughput_10(la->total_work, la->total_ns, is_byte_unit));
            lanes_str  = desc->highway_lanes > 0
                ? fmt::format("{}", desc->highway_lanes) : std::string{"-"};
            target_str = desc->highway_target != nullptr ? desc->highway_target : "-";
        }

        cout << fmt::format(
            "{:<48} {:>9} {:>18} {:>11} {} {} {:>11} {:>11} {} {} {:>6} {:>9}\n",
            remap(desc),
            base->visits,
            stats::with_thousands_separator(base->total_ns),
            format::fmt_duration_10(mean_ns),
            p50_str,
            p99_str,
            format::fmt_duration_10(base->min_ns == std::uint64_t{0xFFFFFFFFFFFFFFFFull} ? 0 : base->min_ns),
            format::fmt_duration_10(base->max_ns),
            iters_str,
            thrpt_str,
            lanes_str,
            target_str);
        printed_any = true;
    });
    if (!printed_any) {
        cout << "  (no regions visited yet — instrumentation may be off)\n";
    }
    cout << format::rule() << "\n";
}

/// PMU summary — cycles, instructions, IPC, branches, branch-miss
/// rate. On hosts without PMU access (macOS without root, Linux with
/// `perf_event_paranoid > 2`) the counters are zero across the board
/// and the table compresses to a single "no PMU data" line.
inline void print_pmu_summary(
    std::string_view section_name,
    const DescriptorRemapFn& remap = default_descriptor_name_remap())
{
    using std::cout;

    bool has_any_pmu = false;
    for_each_visited_descriptor(
        [&](const bioseq::instr::RegionDescriptor* /*desc*/,
            const bioseq::instr::RegionAggregate* ra,
            const bioseq::instr::LoopAggregate* la) {
        const auto* base = la != nullptr
            ? static_cast<const bioseq::instr::RegionAggregate*>(la) : ra;
        if (base == nullptr) return;
        if (base->total_cycles > 0 || base->total_instructions > 0) {
            has_any_pmu = true;
        }
    });

    cout << format::rule() << "\n";
    cout << fmt::format(
        "[bench] PMU summary  section=\"{}\"  (counters via "
#if defined(__APPLE__)
        "kperf.framework"
#elif defined(__linux__)
        "perf_event_open"
#else
        "stub"
#endif
        ")\n",
        section_name);
    cout << format::rule() << "\n";

    if (!has_any_pmu) {
        cout <<
#if defined(__APPLE__)
            "  (no PMU data — kperf init failed; on macOS this normally requires running under sudo)\n"
#elif defined(__linux__)
            "  (no PMU data — perf_event_open failed; check /proc/sys/kernel/perf_event_paranoid)\n"
#else
            "  (no PMU data — no PMU backend on this platform)\n"
#endif
            ;
        cout << format::rule() << "\n";
        return;
    }

    cout << fmt::format(
        "{:<48} {:>9} {:>12} {:>12} {:>7} {:>12} {:>12} {:>12} {:>8}\n",
        "name", "visits",
        "cycles", "cyc/visit", "IPC",
        "instr", "branches", "br/visit", "br_miss%");
    cout << format::rule() << "\n";

    for_each_visited_descriptor(
        [&](const bioseq::instr::RegionDescriptor* desc,
            const bioseq::instr::RegionAggregate* ra,
            const bioseq::instr::LoopAggregate* la) {
        const auto* base = la != nullptr
            ? static_cast<const bioseq::instr::RegionAggregate*>(la) : ra;
        if (base == nullptr) return;
        if (base->total_cycles == 0 && base->total_instructions == 0
            && base->total_branches == 0 && base->total_branch_misses == 0) {
            return;
        }
        cout << fmt::format(
            "{:<48} {:>9} {} {} {} {} {} {} {}\n",
            remap(desc),
            base->visits,
            format::fmt_compact_count(base->total_cycles),
            format::fmt_per_visit(base->total_cycles, base->visits),
            format::fmt_ipc(base->total_cycles, base->total_instructions),
            format::fmt_compact_count(base->total_instructions),
            format::fmt_compact_count(base->total_branches),
            format::fmt_per_visit(base->total_branches, base->visits),
            format::fmt_branch_miss_pct(base->total_branches, base->total_branch_misses));
    });
    cout << format::rule() << "\n";
}

/// Arena summary — per-arena live / total bytes / alloc counts.
/// The aligner doesn't need a remap callable here (the kind names are
/// generic), but we accept one for API symmetry with the other
/// printers.
inline void print_arena_summary(
    const DescriptorRemapFn& /*remap*/ = default_descriptor_name_remap())
{
    using std::cout;
    auto& prof = bioseq::instr::persistent_profiler_block<profiler_args_t>();

    bool any = false;
    for (unsigned k = 1; k < bioseq::instr::kArenaKindCount; ++k) {
        if (prof.arena_counters[k].alloc_count != 0) { any = true; break; }
    }
    if (!any) {
        cout << "[bench] profile arena summary  (no arena-attributed allocations recorded)\n";
        cout << format::rule() << "\n";
        return;
    }

    cout << "[bench] profile arena summary\n";
    cout << fmt::format(
        "{:<10} {:>22} {:>22} {:>16} {:>22}\n",
        "kind", "live", "total", "allocs", "largest");
    cout << format::rule() << "\n";

    auto kind_name = [](unsigned k) -> std::string_view {
        using bioseq::instr::arena_kind;
        switch (static_cast<arena_kind>(k)) {
            case arena_kind::scratch: return "scratch";
            case arena_kind::chain:   return "chain";
            case arena_kind::align:   return "align";
            case arena_kind::result:  return "result";
            case arena_kind::hash:    return "hash";
            default:                  return "?";
        }
    };

    for (unsigned k = 1; k < bioseq::instr::kArenaKindCount; ++k) {
        const auto& c = prof.arena_counters[k];
        if (c.alloc_count == 0) continue;
        cout << fmt::format(
            "{:<10} {:>22} {:>22} {:>16} {:>22}\n",
            kind_name(k),
            format::fmt_bytes(c.bytes_live,  22),
            format::fmt_bytes(c.bytes_total, 22),
            format::fmt_count(c.alloc_count, 16),
            format::fmt_bytes(c.largest,     22));
    }
    cout << format::rule() << "\n";
}

/// Per-descriptor stats collected for the top-N tables.
struct DescStats {
    const bioseq::instr::RegionDescriptor* desc = nullptr;
    std::uint64_t visits           = 0;
    std::uint64_t total_ns         = 0;
    std::uint64_t total_iterations = 0;
    std::uint64_t total_work       = 0;
    bool          is_loop          = false;
};

/// Materialise per-descriptor stats from the profiler block. Used by
/// the top-N printers and by aligner-side cross-cuts that need the
/// same layout (e.g. the side-by-side phase comparison).
[[nodiscard]] inline std::vector<DescStats> collect_visited_stats() {
    std::vector<DescStats> out;
    for_each_visited_descriptor(
        [&](const bioseq::instr::RegionDescriptor* desc,
            const bioseq::instr::RegionAggregate* ra,
            const bioseq::instr::LoopAggregate* la) {
        DescStats s;
        s.desc = desc;
        if (la != nullptr) {
            s.visits           = la->visits;
            s.total_ns         = la->total_ns;
            s.total_iterations = la->total_iterations;
            s.total_work       = la->total_work;
            s.is_loop          = true;
        } else if (ra != nullptr) {
            s.visits   = ra->visits;
            s.total_ns = ra->total_ns;
        }
        out.push_back(s);
    });
    return out;
}

/// Top-N regions by total_ns. Regions nest (caller contains callees)
/// so the percentage column uses the *single longest* region as the
/// denominator — that's the wall time of the dominant call stack and
/// gives a meaningful "share of the longest call" reading instead of
/// nonsensical >100% sums.
inline void print_top_bottlenecks(
    std::size_t top_n = 8,
    const DescriptorRemapFn& remap = default_descriptor_name_remap())
{
    using std::cout;
    auto descriptor_stats = collect_visited_stats();
    if (descriptor_stats.empty()) {
        cout << "[bench] profile top-bottlenecks  (no data)\n";
        cout << format::rule() << "\n";
        return;
    }
    std::uint64_t denom_ns = 0;
    for (const auto& s : descriptor_stats) {
        if (s.total_ns > denom_ns) denom_ns = s.total_ns;
    }
    if (denom_ns == 0) denom_ns = 1;

    std::sort(descriptor_stats.begin(), descriptor_stats.end(),
              [](const DescStats& a, const DescStats& b) {
        return a.total_ns > b.total_ns;
    });
    if (descriptor_stats.size() > top_n) descriptor_stats.resize(top_n);

    cout << "[bench] cross-cut top-bottlenecks (by total_ns; pct = share of longest region)\n";
    cout << fmt::format(
        "{:<6} {:<58} {:>18} {:>11} {:>11} {:>10}\n",
        "rank", "region", "total_ns", "mean", "visits", "pct_of_max");
    cout << format::rule() << "\n";
    std::size_t rank = 1;
    for (const auto& s : descriptor_stats) {
        const auto mean_ns = s.visits > 0 ? s.total_ns / s.visits : 0;
        const double pct = 100.0 * static_cast<double>(s.total_ns)
                                / static_cast<double>(denom_ns);
        cout << fmt::format(
            "{:<6} {:<58} {:>18} {:>11} {:>11} {:>9.2f}%\n",
            rank, remap(s.desc),
            stats::with_thousands_separator(s.total_ns),
            format::fmt_duration_10(mean_ns),
            stats::with_thousands_separator(s.visits), pct);
        ++rank;
    }
    cout << format::rule() << "\n";
}

/// Top-N regions by visit count. `total_iterations` is populated for
/// loops only; non-loop regions show `-` in that column.
inline void print_top_call_frequency(
    std::size_t top_n = 8,
    const DescriptorRemapFn& remap = default_descriptor_name_remap())
{
    using std::cout;
    auto descriptor_stats = collect_visited_stats();
    if (descriptor_stats.empty()) {
        cout << "[bench] profile top-call-frequency  (no data)\n";
        cout << format::rule() << "\n";
        return;
    }
    std::sort(descriptor_stats.begin(), descriptor_stats.end(),
              [](const DescStats& a, const DescStats& b) {
        if (a.visits != b.visits) return a.visits > b.visits;
        return a.total_ns > b.total_ns;
    });
    if (descriptor_stats.size() > top_n) descriptor_stats.resize(top_n);

    cout << "[bench] cross-cut top-call-frequency (by visits; total_iterations populated for loops only)\n";
    cout << fmt::format(
        "{:<6} {:<58} {:>11} {:>20} {:>18} {:>11}\n",
        "rank", "region", "visits", "total_iterations", "total_ns", "mean");
    cout << format::rule() << "\n";
    std::size_t rank = 1;
    for (const auto& s : descriptor_stats) {
        const auto mean_ns = s.visits > 0 ? s.total_ns / s.visits : 0;
        std::string iters_str = s.is_loop
            ? format::fmt_count(s.total_iterations, 20)
            : fmt::format("{:>20}", "-");
        cout << fmt::format(
            "{:<6} {:<58} {:>11} {} {:>18} {:>11}\n",
            rank, remap(s.desc),
            stats::with_thousands_separator(s.visits), iters_str,
            stats::with_thousands_separator(s.total_ns),
            format::fmt_duration_10(mean_ns));
        ++rank;
    }
    cout << format::rule() << "\n";
}

/// Reset every region / loop / arena accumulator to zero. Call
/// between TEST_CASEs so per-section reports are independent.
inline void reset_aggregates() noexcept {
    bioseq::instr::access::reset_all<profiler_args_t>(
        bioseq::instr::persistent_profiler_block<profiler_args_t>());
}

/// RAII guard that installs the persistent timing-only profiler
/// block on the current thread. Every harness's TEST_CASE creates one
/// at scope start so all `bioseq::instr::region(...)` /
/// `loop(...)` scopes downstream see a non-null tls_profiler.
struct InstalledProfiler {
    bioseq::instr::installed_profiler_scope<profiler_args_t> scope;
    InstalledProfiler() noexcept
        : scope(bioseq::instr::persistent_profiler_block<profiler_args_t>()) {}
};

/// One-shot driver: emit all five generic instrumentation tables in
/// canonical order. Harnesses that interleave domain-specific tables
/// between sections call the individual printers directly; harnesses
/// that just want "the full instrumentation block" call this.
inline void print_instrumentation_report(
    std::string_view section_name,
    std::size_t top_n = 8,
    const DescriptorRemapFn& remap = default_descriptor_name_remap())
{
    print_instrumentation_summary(section_name, remap);
    print_pmu_summary(section_name, remap);
    print_arena_summary(remap);
    print_top_bottlenecks(top_n, remap);
    print_top_call_frequency(top_n, remap);
}

}  // namespace bioseq::bench_scheduler

}  // export
