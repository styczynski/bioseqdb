/**
 * @file report.cppm
 * @brief 120-column bench report emitter.
 *
 * Reads the persistent process-wide ProfilerBlock<context_args_profiling_full>
 * (bioseq::instr) and emits human-readable tables to stdout. Designed to
 * print *after* the Catch2 BENCHMARK harness's own table, so the output
 * stream stacks like:
 *
 *   <Catch2 BENCHMARK table>      ← Catch2 prints this itself
 *   [bioseq] profile ...          ← we add this block
 *   ...
 *
 * Output blocks (see main/bioseq/RESEARCH/REPORT_BENCH_IMPROVEMENTS.md
 * for the full layout spec):
 *   1. Region summary
 *   2. Loop summary
 *   3. Arena summary
 *   4. Top-N bottlenecks
 *   5. Top-N call frequency
 *   6. Match-quality (per MAPQ threshold)
 *   7. Length-bucket cross-cut
 *   8. bioseq vs minimap2 side-by-side
 *
 * The report intentionally uses std::cout (via fmt::print) instead of
 * JustLog so the Catch2 BENCHMARK output and our extras share one stream
 * — that keeps line ordering predictable when tests are piped through
 * `tee` or compared across runs.
 */
module;

#include <hwy/highway.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <iterator>
#include <numeric>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

export module bench.report;

// Pull fmt in via the mlogging module — it owns the fmt module
// attachment in this codebase. Including <fmt/format.h> directly here
// would re-attach fmt's symbols to bench.report and conflict with
// mlogging's existing attachment.
import mlogging;
import bioseq_instr;
import bench.evaluation;
import bench.generator;

// The bench evaluator types (EvaluationResults, SingleEvalResult,
// AlignerAggregatedStats) live in `bioseq::bench::HWY_NAMESPACE` —
// evaluation.cppm wraps its content with HWY_BEFORE/AFTER_NAMESPACE so
// the dispatch fork can swap targets at runtime. Mirror the same shape
// here so the type names resolve without explicit target prefixes.
HWY_BEFORE_NAMESPACE();

namespace bioseq::bench::HWY_NAMESPACE::report {

// ---------------------------------------------------------------------
// Number-formatting helpers. Each returns a fixed-width string so
// columns align even when values span 10 orders of magnitude.
// ---------------------------------------------------------------------

/// Format a nanosecond duration into a 10-char unit-aware string. The
/// cut-overs match Catch2's own table so identical magnitudes always
/// render with identical units across the two layers of output.
inline std::string fmt_duration_10(std::uint64_t ns) noexcept {
    if (ns < 10'000ull)
        return fmt::format("{:>7} ns", ns);
    if (ns < 10'000'000ull)
        return fmt::format("{:>6.2f} us", static_cast<double>(ns) / 1'000.0);
    if (ns < 10'000'000'000ull)
        return fmt::format("{:>6.3f} ms", static_cast<double>(ns) / 1'000'000.0);
    return fmt::format("{:>7.3f} s", static_cast<double>(ns) / 1'000'000'000.0);
}

/// Insert thousands separators (single-quote per ISO-C++14) into a
/// non-negative integer's decimal representation. Locale-independent so
/// the report stays bit-identical across hosts.
inline std::string with_thousands_separator(std::uint64_t value) noexcept {
    std::string digits = fmt::format("{}", value);
    if (digits.size() <= 3) return digits;
    std::string out;
    out.reserve(digits.size() + digits.size() / 3);
    const std::size_t lead = digits.size() % 3;
    std::size_t i = 0;
    if (lead != 0) {
        out.append(digits, 0, lead);
        i = lead;
    }
    while (i < digits.size()) {
        if (!out.empty()) out.push_back('\'');
        out.append(digits, i, 3);
        i += 3;
    }
    return out;
}

/// Format a count to a right-aligned, width-padded, thousands-separated
/// string.
inline std::string fmt_count(std::uint64_t value, int width) noexcept {
    return fmt::format("{:>{}}", with_thousands_separator(value), width);
}

/// Format a non-negative byte count with thousands separators and a
/// trailing " B" inside a fixed-width column.
inline std::string fmt_bytes(std::uint64_t bytes, int width) noexcept {
    return fmt::format("{:>{}}", with_thousands_separator(bytes) + " B", width);
}

/// Format a signed delta percentage with a forced sign so columns align.
inline std::string fmt_signed_pct(double pct) noexcept {
    return fmt::format("{:+8.2f}%", pct);
}

/// Format an unsigned percentage, e.g. `92.00%`.
inline std::string fmt_pct(double frac01) noexcept {
    return fmt::format("{:6.2f}%", frac01 * 100.0);
}

/// Format a throughput rate (work units / time) with auto-scaled units.
/// `bytes/ns == GB/s` (1 GB == 1e9 bytes, 1 s == 1e9 ns), so for byte-
/// typed loops `total_work_bytes / total_ns` *is* GB/s. The bench-level
/// loop's per-pair work is small enough that GB/s rounds to 0.000;
/// auto-step down to MB/s and KB/s so the column is informative across
/// magnitudes. Returns `-` when units are not bytes (caller passes
/// is_byte_unit=false) — see issue #14: GB/s on iteration-counted loops
/// is meaningless.
inline std::string fmt_throughput_10(
    std::uint64_t total_work, std::uint64_t total_ns,
    bool is_byte_unit) noexcept
{
    if (!is_byte_unit) return fmt::format("{:>10}", "-");
    if (total_ns == 0) return fmt::format("{:>10}", "-");
    const double bytes_per_ns =
        static_cast<double>(total_work) / static_cast<double>(total_ns);
    // bytes_per_ns == GB/s. Auto-scale.
    if (bytes_per_ns >= 1.0)        return fmt::format("{:6.2f} GB/s", bytes_per_ns);
    if (bytes_per_ns >= 0.001)      return fmt::format("{:6.2f} MB/s", bytes_per_ns * 1000.0);
    if (bytes_per_ns >= 0.000001)   return fmt::format("{:6.2f} kB/s", bytes_per_ns * 1'000'000.0);
    return fmt::format("{:6.2f}  B/s", bytes_per_ns * 1'000'000'000.0);
}

/// Format an iter/s rate.
inline std::string fmt_iters_per_sec(std::uint64_t total_iters, std::uint64_t total_ns) noexcept {
    if (total_ns == 0) return fmt::format("{:>14}", "-");
    const double iters_per_sec =
        static_cast<double>(total_iters) * 1e9 / static_cast<double>(total_ns);
    return fmt::format("{:14.2f}", iters_per_sec);
}

/// Compact representation of a counter in Mc / Bc (mega-/giga-counts) so
/// the wide PMU columns fit in 12 chars without scientific notation.
inline std::string fmt_compact_count(std::uint64_t value) noexcept {
    if (value == 0) return fmt::format("{:>12}", "0");
    if (value < 1'000ull)             return fmt::format("{:>12}", value);
    if (value < 1'000'000ull)         return fmt::format("{:>9.2f} k", value / 1'000.0);
    if (value < 1'000'000'000ull)     return fmt::format("{:>9.2f} M", value / 1'000'000.0);
    if (value < 1'000'000'000'000ull) return fmt::format("{:>9.2f} G", value / 1'000'000'000.0);
    return fmt::format("{:>9.2f} T", value / 1'000'000'000'000.0);
}

/// IPC = total_instructions / total_cycles. Returns "-" when cycles==0
/// (no PMU data, or zero-time region). 6.3 chars (e.g. " 2.471"); we
/// pad to the column width at format time.
inline std::string fmt_ipc(std::uint64_t total_cycles, std::uint64_t total_instructions) noexcept {
    if (total_cycles == 0) return fmt::format("{:>7}", "-");
    const double ipc =
        static_cast<double>(total_instructions) / static_cast<double>(total_cycles);
    return fmt::format("{:>7.3f}", ipc);
}

/// Branch miss rate as a percent of total branches.
inline std::string fmt_branch_miss_pct(std::uint64_t total_branches,
                                       std::uint64_t total_branch_misses) noexcept {
    if (total_branches == 0) return fmt::format("{:>8}", "-");
    const double pct =
        100.0 * static_cast<double>(total_branch_misses)
              / static_cast<double>(total_branches);
    return fmt::format("{:>7.3f}%", pct);
}

/// Mean cycles per visit. We surface this so the reader can spot the
/// "this region runs for 30k cycles per visit, 408 visits, no wonder
/// it's hot" reading at a glance without doing the division by hand.
inline std::string fmt_per_visit(std::uint64_t total, std::uint64_t visits) noexcept {
    if (visits == 0) return fmt::format("{:>12}", "-");
    return fmt_compact_count(total / visits);
}

/// Width of the wide tables. Picked to match Catch2's own BENCHMARK
/// rule width (CATCH_CONFIG_CONSOLE_WIDTH-1 == 199 after our rebuild)
/// so the reader sees a single visual column when the report block
/// follows a Catch2 BENCHMARK section. Data rows clock in around 184
/// chars; the extra slack on the rule emphasises the column structure.
inline constexpr int kTableWidth = 199;

inline std::string rule() noexcept {
    return std::string(kTableWidth, '-');
}

/// Map an in-source descriptor name to a render-time display name. The
/// bioseq port and the minimap2 reference both register descriptors
/// like `mm_sketch`, `mm_set_parent`, `mg_lchain_dp`, `mm_align_skeleton`,
/// etc.; the bench-level shim prefixes its own with `tool_minimap2.`.
/// Without a corresponding prefix on the bioseq side, the two sets
/// interleave in the report and the reader can't tell which row belongs
/// to which aligner. We don't want to rename at registration (that
/// reaches into bioseq core); instead we add a `minimap_port.` prefix at
/// render time using the descriptor's source file as the discriminator.
inline std::string render_name(const bioseq::instr::RegionDescriptor* desc) noexcept {
    if (desc == nullptr) return std::string{"<null>"};
    const std::string_view name{desc->name};
    // Already prefixed (tool_minimap2.* / bench.*) — leave as-is.
    if (name.starts_with("tool_minimap2.") ||
        name.starts_with("bench.")) {
        return std::string{name};
    }
    // Anything coming out of the bench/tool_minimap2 directory should
    // also be prefixed with tool_minimap2 — but the C-shim already does
    // that, so this branch is effectively unreachable. Keep it as
    // belt-and-braces against a future shim site that forgets the
    // prefix.
    if (desc->file != nullptr) {
        const std::string_view f{desc->file};
        if (f.find("/bench/tool_minimap2/") != std::string_view::npos) {
            return fmt::format("tool_minimap2.{}", name);
        }
    }
    // Otherwise: assume it's a bioseq-side region (minimap_port.cppm,
    // chaining/*.cppm, kmers/*.cppm, etc.) and prefix accordingly.
    return fmt::format("minimap_port.{}", name);
}

// ---------------------------------------------------------------------
// Aggregate-side accessors. We pin to the canonical
// context_args_profiling_timing_only block — the bench installs it via
// InstalledProfiler so all the non-templated `region(...)` / `loop(...)`
// call sites in the chaining / kmers / minimap_port code paths see a
// non-null tls_profiler<context_args_profiling_timing_only>. Counters
// (PMU) are deliberately disabled in the bench config so per-region
// overhead is timing-only — see config.cppm and the engineer note in
// SPEC_BENCHMARKING_SPEEDUP_PROPOSAL_1.md §1.2.
// ---------------------------------------------------------------------

using args_t = bioseq::instr::context_args_profiling_timing_only;

/// Walk every registered descriptor and call `visitor(desc, region_agg,
/// loop_agg)`. region_agg / loop_agg are nullable; loop_agg is non-null
/// only when the descriptor's id is also present in the loop table AND
/// the loop has been visited.
template<class Fn>
inline void for_each_visited_descriptor(Fn&& visitor) noexcept {
    auto& prof = bioseq::instr::persistent_profiler_block<args_t>();
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

/// p50 / p99 percentile estimates from the log2 histogram. Mirrors the
/// canonical implementation in `bioseq_instr.access` but lives here too
/// because the report's loop does the lookup against a non-const member
/// reference, and we want to keep the report self-contained.
inline std::uint64_t hist_percentile_local(
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

// ---------------------------------------------------------------------
// 1+2. Region + loop summary tables (combined into one walk so the
//      reader sees regions and loops grouped under one header).
// ---------------------------------------------------------------------

inline void print_instrumentation_summary(std::string_view section_name) {
    using std::cout;
    cout << "\n" << rule() << "\n";
    cout << fmt::format("[bioseq] profile summary  section=\"{}\"\n", section_name);
    cout << rule() << "\n";
    // Column widths: 48 + 9 + 18 + 11 + 11 + 11 + 11 + 11 + 14 + 14 + 6
    // + 9 + 11 separator chars  = 168.
    cout << fmt::format(
        "{:<48} {:>9} {:>18} {:>11} {:>11} {:>11} {:>11} {:>11} {:>14} {:>14} {:>6} {:>9}\n",
        "name", "visits", "total_ns",
        "mean", "p50", "p99", "min", "max",
        "iter/s", "bytes/s", "lanes", "target");
    cout << rule() << "\n";

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
        // constant. Suppress with `-` until we have at least two
        // visits.
        const std::string p50_str = base->visits >= 2
            ? fmt_duration_10(hist_percentile_local(base->ns_hist, 0.50))
            : fmt::format("{:>11}", "-");
        const std::string p99_str = base->visits >= 2
            ? fmt_duration_10(hist_percentile_local(base->ns_hist, 0.99))
            : fmt::format("{:>11}", "-");

        std::string iters_str = fmt::format("{:>14}", "-");
        std::string thrpt_str = fmt::format("{:>14}", "-");
        std::string lanes_str = "-";
        std::string target_str= "-";
        if (la != nullptr) {
            iters_str  = fmt_iters_per_sec(la->total_iterations, la->total_ns);
            // Bench-level loops tick by `bp_size()` (bytes), so GB/s is
            // correct for them. Loops that tick by `1` per iteration
            // (the default in the chaining inner-loops) get a
            // meaningless work value — the descriptor's
            // `work_unit_kind` field would carry the unit type if we
            // populated it. For now, gate throughput on the loop name
            // prefix `bench.` which is the only byte-typed work
            // tracker we ship.
            const std::string_view nm{desc->name};
            const bool is_byte_unit = nm.starts_with("bench.");
            thrpt_str = fmt::format("{:>14}",
                fmt_throughput_10(la->total_work, la->total_ns, is_byte_unit));
            lanes_str  = desc->highway_lanes > 0
                ? fmt::format("{}", desc->highway_lanes) : std::string{"-"};
            target_str = desc->highway_target != nullptr ? desc->highway_target : "-";
        }

        cout << fmt::format(
            "{:<48} {:>9} {:>18} {:>11} {} {} {:>11} {:>11} {} {} {:>6} {:>9}\n",
            render_name(desc),
            base->visits,
            with_thousands_separator(base->total_ns),
            fmt_duration_10(mean_ns),
            p50_str,
            p99_str,
            fmt_duration_10(base->min_ns == std::uint64_t{0xFFFFFFFFFFFFFFFFull} ? 0 : base->min_ns),
            fmt_duration_10(base->max_ns),
            iters_str,
            thrpt_str,
            lanes_str,
            target_str);
        printed_any = true;
    });
    if (!printed_any) {
        cout << "  (no regions visited yet — instrumentation may be off)\n";
    }
    cout << rule() << "\n";
}

// ---------------------------------------------------------------------
// 2b. PMU summary — cycles, instructions, IPC, branches, branch_miss%.
//     Separated from the timing summary so the timing table stays
//     readable; on macOS without root and on Linux without
//     perf_event_paranoid<=2 these columns are all zero (or `-`)
//     because read_thread_counters() falls back to zeros when the
//     backend can't open the PMU.
// ---------------------------------------------------------------------

inline void print_pmu_summary(std::string_view section_name) {
    using std::cout;

    // Sample one region's PMU values to decide whether to print the
    // table at all — when no backend is available every aggregate has
    // zeros across the board and the table is just noise.
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

    cout << rule() << "\n";
    cout << fmt::format(
        "[bioseq] PMU summary  section=\"{}\"  (counters via "
#if defined(__APPLE__)
        "kperf.framework"
#elif defined(__linux__)
        "perf_event_open"
#else
        "stub"
#endif
        ")\n",
        section_name);
    cout << rule() << "\n";

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
        cout << rule() << "\n";
        return;
    }

    // Columns: 48 + 9 + 12 + 12 + 7 + 12 + 12 + 12 + 8 + separators
    cout << fmt::format(
        "{:<48} {:>9} {:>12} {:>12} {:>7} {:>12} {:>12} {:>12} {:>8}\n",
        "name", "visits",
        "cycles", "cyc/visit", "IPC",
        "instr", "branches", "br/visit", "br_miss%");
    cout << rule() << "\n";

    for_each_visited_descriptor(
        [&](const bioseq::instr::RegionDescriptor* desc,
            const bioseq::instr::RegionAggregate* ra,
            const bioseq::instr::LoopAggregate* la) {
        const auto* base = la != nullptr
            ? static_cast<const bioseq::instr::RegionAggregate*>(la) : ra;
        if (base == nullptr) return;
        // Skip rows with no PMU data — they would print all-`-` and
        // visually distract from the rows that do have data.
        if (base->total_cycles == 0 && base->total_instructions == 0
            && base->total_branches == 0 && base->total_branch_misses == 0) {
            return;
        }
        cout << fmt::format(
            "{:<48} {:>9} {} {} {} {} {} {} {}\n",
            render_name(desc),
            base->visits,
            fmt_compact_count(base->total_cycles),
            fmt_per_visit(base->total_cycles, base->visits),
            fmt_ipc(base->total_cycles, base->total_instructions),
            fmt_compact_count(base->total_instructions),
            fmt_compact_count(base->total_branches),
            fmt_per_visit(base->total_branches, base->visits),
            fmt_branch_miss_pct(base->total_branches, base->total_branch_misses));
    });
    cout << rule() << "\n";
}

// ---------------------------------------------------------------------
// 3. Arena summary
// ---------------------------------------------------------------------

inline void print_arena_summary() {
    using std::cout;
    auto& prof = bioseq::instr::persistent_profiler_block<args_t>();

    bool any = false;
    for (unsigned k = 1; k < bioseq::instr::kArenaKindCount; ++k) {
        if (prof.arena_counters[k].alloc_count != 0) { any = true; break; }
    }
    if (!any) {
        cout << "[bioseq] profile arena summary  (no arena-attributed allocations recorded — "
                "v2: wire foonathan_memory tracker into the bench arenas)\n";
        cout << rule() << "\n";
        return;
    }

    cout << "[bioseq] profile arena summary\n";
    cout << fmt::format(
        "{:<10} {:>22} {:>22} {:>16} {:>22}\n",
        "kind", "live", "total", "allocs", "largest");
    cout << rule() << "\n";

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
            fmt_bytes(c.bytes_live,  22),
            fmt_bytes(c.bytes_total, 22),
            fmt_count(c.alloc_count, 16),
            fmt_bytes(c.largest,     22));
    }
    cout << rule() << "\n";
}

// ---------------------------------------------------------------------
// 4. Top-N bottlenecks by total_ns
// ---------------------------------------------------------------------

struct DescStats {
    const bioseq::instr::RegionDescriptor* desc = nullptr;
    std::uint64_t visits           = 0;
    std::uint64_t total_ns         = 0;
    std::uint64_t total_iterations = 0;
    std::uint64_t total_work       = 0;
    bool          is_loop          = false;
};

inline std::vector<DescStats> collect_visited_stats() {
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

inline void print_top_bottlenecks(std::size_t top_n = 8) {
    using std::cout;
    auto stats = collect_visited_stats();
    if (stats.empty()) {
        cout << "[bioseq] profile top-bottlenecks  (no data)\n";
        cout << rule() << "\n";
        return;
    }
    // Regions nest (minimap_port contains mm_sketch which contains the
    // inner loops, etc.) so summing every visited region's total_ns
    // would be double-counting and the percentages would exceed 100%.
    // Use the longest single-region total as the denominator instead —
    // that's the wall time of the dominant call stack and gives a
    // meaningful "share of the longest call" reading.
    std::uint64_t denom_ns = 0;
    for (const auto& s : stats) {
        if (s.total_ns > denom_ns) denom_ns = s.total_ns;
    }
    if (denom_ns == 0) denom_ns = 1;

    std::sort(stats.begin(), stats.end(), [](const DescStats& a, const DescStats& b) {
        return a.total_ns > b.total_ns;
    });
    if (stats.size() > top_n) stats.resize(top_n);

    cout << "[bioseq] cross-cut top-bottlenecks (by total_ns; pct = share of longest region)\n";
    cout << fmt::format(
        "{:<6} {:<58} {:>18} {:>11} {:>11} {:>10}\n",
        "rank", "region", "total_ns", "mean", "visits", "pct_of_max");
    cout << rule() << "\n";
    std::size_t rank = 1;
    for (const auto& s : stats) {
        const auto mean_ns = s.visits > 0 ? s.total_ns / s.visits : 0;
        const double pct = 100.0 * static_cast<double>(s.total_ns)
                                / static_cast<double>(denom_ns);
        cout << fmt::format(
            "{:<6} {:<58} {:>18} {:>11} {:>11} {:>9.2f}%\n",
            rank, render_name(s.desc),
            with_thousands_separator(s.total_ns), fmt_duration_10(mean_ns),
            with_thousands_separator(s.visits), pct);
        ++rank;
    }
    cout << rule() << "\n";
}

// ---------------------------------------------------------------------
// 5. Top-N most-frequently-called regions
// ---------------------------------------------------------------------

inline void print_top_call_frequency(std::size_t top_n = 8) {
    using std::cout;
    auto stats = collect_visited_stats();
    if (stats.empty()) {
        cout << "[bioseq] profile top-call-frequency  (no data)\n";
        cout << rule() << "\n";
        return;
    }
    std::sort(stats.begin(), stats.end(), [](const DescStats& a, const DescStats& b) {
        if (a.visits != b.visits) return a.visits > b.visits;
        return a.total_ns > b.total_ns;
    });
    if (stats.size() > top_n) stats.resize(top_n);

    cout << "[bioseq] cross-cut top-call-frequency (by visits; total_iterations populated for loops only)\n";
    cout << fmt::format(
        "{:<6} {:<58} {:>11} {:>20} {:>18} {:>11}\n",
        "rank", "region", "visits", "total_iterations", "total_ns", "mean");
    cout << rule() << "\n";
    std::size_t rank = 1;
    for (const auto& s : stats) {
        const auto mean_ns = s.visits > 0 ? s.total_ns / s.visits : 0;
        std::string iters_str = s.is_loop
            ? fmt_count(s.total_iterations, 20)
            : fmt::format("{:>20}", "-");
        cout << fmt::format(
            "{:<6} {:<58} {:>11} {} {:>18} {:>11}\n",
            rank, render_name(s.desc),
            with_thousands_separator(s.visits), iters_str,
            with_thousands_separator(s.total_ns), fmt_duration_10(mean_ns));
        ++rank;
    }
    cout << rule() << "\n";
}

// ---------------------------------------------------------------------
// 6. Match-quality (SEAL-style) per MAPQ threshold
// ---------------------------------------------------------------------

inline void print_match_quality(const EvaluationResults& results) {
    using std::cout;
    cout << "[bioseq] cross-cut match-quality\n";
    cout << fmt::format(
        "{:<10} {:>6} {:>9} {:>9} {:>9} {:>6} {:>6} {:>6} {:>6} {:>10} {:>10} {:>10} {:>14}\n",
        "aligner", "mapq>=", "mapped", "correct", "incorrect",
        "TP", "FP", "FN", "TN",
        "precision", "recall", "used_read", "mean_offset");
    cout << rule() << "\n";

    auto print_aligner = [&](std::string_view name, const AlignerAggregatedStats& stats) {
        for (const auto& m : stats.per_threshold) {
            cout << fmt::format(
                "{:<10} {:>6} {:>9} {:>9} {:>9} {:>6} {:>6} {:>6} {:>6} {:>10} {:>10} {:>10} {:>11.2f} bp\n",
                name, m.threshold,
                stats.mapped, stats.correct, stats.incorrect,
                m.true_positives, m.false_positives,
                m.false_negatives, m.true_negatives,
                fmt_pct(m.precision),
                fmt_pct(m.recall),
                fmt_pct(m.used_read_ratio),
                stats.mean_absolute_offset);
        }
    };
    print_aligner("bioseq",   results.bioseq);
    print_aligner("minimap2", results.minimap2);
    cout << rule() << "\n";
}

// ---------------------------------------------------------------------
// 7. Length-bucket cross-cut. Bins by query length: [0,500) / [500,2000) /
//    [2000,+inf), per the spec at /tmp/bench_output_spec.md §3.
// ---------------------------------------------------------------------

inline void print_length_buckets(const EvaluationResults& results) {
    using std::cout;
    if (results.per_pair.empty()) return;
    struct Bucket {
        std::string_view name;
        std::size_t      lo;
        std::size_t      hi;     // inclusive of lo, exclusive of hi
        std::size_t      n           = 0;
        double           sum_bs_ns   = 0.0;
        double           sum_mm_ns   = 0.0;
    };
    std::array<Bucket, 3> buckets{
        Bucket{"[0,500)",    0,    500},
        Bucket{"[500,2000)", 500,  2000},
        Bucket{"[2000,+inf)",2000, std::size_t{0xFFFFFFFFFFFFFFFFull}},
    };
    for (const auto& p : results.per_pair) {
        const std::size_t L = p.original_query_length;
        for (auto& b : buckets) {
            if (L >= b.lo && L < b.hi) {
                ++b.n;
                b.sum_bs_ns += p.bioseq.elapsed_ns;
                b.sum_mm_ns += p.minimap2.elapsed_ns;
                break;
            }
        }
    }

    cout << "[bioseq] cross-cut length-bucket\n";
    cout << fmt::format(
        "{:<14} {:>5} {:>16} {:>16} {:>11}\n",
        "length_bp", "n", "mean_bioseq", "mean_mm2", "delta_pct");
    cout << rule() << "\n";
    for (const auto& b : buckets) {
        if (b.n == 0) {
            cout << fmt::format(
                "{:<14} {:>5} {:>16} {:>16} {:>11}\n",
                b.name, 0, "-", "-", "-");
            continue;
        }
        const double mean_bs = b.sum_bs_ns / static_cast<double>(b.n);
        const double mean_mm = b.sum_mm_ns / static_cast<double>(b.n);
        std::string delta_str = "-";
        if (mean_mm > 0.0) {
            delta_str = fmt_signed_pct(100.0 * (mean_bs - mean_mm) / mean_mm);
        }
        cout << fmt::format(
            "{:<14} {:>5} {:>16} {:>16} {:>11}\n",
            b.name, b.n,
            fmt_duration_10(static_cast<std::uint64_t>(mean_bs)),
            fmt_duration_10(static_cast<std::uint64_t>(mean_mm)),
            delta_str);
    }
    cout << rule() << "\n";
}

// ---------------------------------------------------------------------
// 8. bioseq vs minimap2 side-by-side (per-aligner sums of elapsed_ns)
// ---------------------------------------------------------------------

/// Per-region totals indexed by descriptor name (after render_name).
/// When multiple descriptors share the same rendered name (e.g.
/// `minimap_port.mm_sketch` is registered at three call sites in the
/// bioseq port), summing them would double-count the children inside
/// the outer site's region. Use the maximum total_ns and the matching
/// visit count instead — that gives the dominant-call total for the
/// phase, which is what the side-by-side delta wants to compare
/// against minimap2's single registration of the same name.
inline std::unordered_map<std::string, std::pair<std::uint64_t, std::uint64_t>>
collect_region_totals_by_name() {
    std::unordered_map<std::string, std::pair<std::uint64_t, std::uint64_t>> out;
    auto stats = collect_visited_stats();
    for (const auto& s : stats) {
        const auto rendered = render_name(s.desc);
        auto it = out.find(rendered);
        if (it == out.end()) {
            out.emplace(rendered, std::pair{s.total_ns, s.visits});
        } else if (s.total_ns > it->second.first) {
            it->second = std::pair{s.total_ns, s.visits};
        }
    }
    return out;
}

inline void print_side_by_side(const EvaluationResults& results) {
    using std::cout;
    if (results.per_pair.empty()) return;

    std::uint64_t bs_total = 0, mm_total = 0;
    for (const auto& p : results.per_pair) {
        bs_total += static_cast<std::uint64_t>(p.bioseq.elapsed_ns);
        mm_total += static_cast<std::uint64_t>(p.minimap2.elapsed_ns);
    }
    const auto bs_mean = results.bioseq.mean_alignment_ns;
    const auto mm_mean = results.minimap2.mean_alignment_ns;

    cout << "[bioseq] cross-cut side-by-side delta (matched phases between bioseq port and minimap2 reference)\n";
    cout << fmt::format(
        "{:<32} {:>18} {:>11} {:>18} {:>11} {:>14}\n",
        "phase", "bioseq_total_ns", "bioseq_mean",
        "mm2_total_ns", "mm2_mean", "speed_delta");
    cout << rule() << "\n";

    auto delta_pct = [](double a, double b) -> std::string {
        if (b == 0.0) return fmt::format("{:>14}", "-");
        return fmt::format("{:>14}", fmt_signed_pct(100.0 * (a - b) / b));
    };

    auto totals = collect_region_totals_by_name();
    auto sum_total_ns = [&](std::initializer_list<std::string> names) -> std::pair<std::uint64_t, std::uint64_t> {
        std::uint64_t total = 0, visits = 0;
        for (const auto& n : names) {
            auto it = totals.find(n);
            if (it != totals.end()) {
                total  += it->second.first;
                visits += it->second.second;
            }
        }
        return {total, visits};
    };

    auto print_phase = [&](std::string_view phase,
                           std::initializer_list<std::string> bioseq_names,
                           std::initializer_list<std::string> mm2_names) {
        const auto [bs_t, bs_v] = sum_total_ns(bioseq_names);
        const auto [mm_t, mm_v] = sum_total_ns(mm2_names);
        const auto bs_m  = bs_v > 0 ? bs_t / bs_v : 0;
        const auto mm_m  = mm_v > 0 ? mm_t / mm_v : 0;
        if (bs_t == 0 && mm_t == 0) return;  // skip phases with no data
        cout << fmt::format(
            "{:<32} {:>18} {:>11} {:>18} {:>11} {}\n",
            phase,
            with_thousands_separator(bs_t),
            fmt_duration_10(bs_m),
            with_thousands_separator(mm_t),
            fmt_duration_10(mm_m),
            delta_pct(static_cast<double>(bs_t), static_cast<double>(mm_t)));
    };

    // Per-pair eval timings (whole-aligner wall clock from the eval pass)
    cout << fmt::format(
        "{:<32} {:>18} {:>11} {:>18} {:>11} {}\n",
        "alignment (eval wall-clock)",
        with_thousands_separator(bs_total),
        fmt_duration_10(static_cast<std::uint64_t>(bs_mean)),
        with_thousands_separator(mm_total),
        fmt_duration_10(static_cast<std::uint64_t>(mm_mean)),
        delta_pct(static_cast<double>(bs_total), static_cast<double>(mm_total)));

    // Phase-level matched pairs. The bioseq port keeps short names
    // (mm_sketch, mg_lchain_dp, mm_align_skeleton, mm_set_mapq) which
    // we render-prefix as `minimap_port.*`. The minimap2 reference's
    // shim prefixes the same algorithm names with `tool_minimap2.*`.
    // For phases that are split across multiple instrumentation sites
    // (chaining can take the linear-DP or RMQ path; chain_post wraps
    // both), pass all names so the sums match what each side actually
    // spent in the phase. `collect_region_totals_by_name` deduplicates
    // multi-site registrations of the same rendered name via max(),
    // not sum(), so a name that appears at three call sites is
    // counted once per print_phase invocation.
    print_phase("sketch (minimizer collection)",
                {"minimap_port.mm_sketch"},
                {"tool_minimap2.mm_sketch", "tool_minimap2.base_collect_minimizers"});
    print_phase("chain (linear DP + RMQ + post)",
                {"minimap_port.mg_lchain_dp",
                 "minimap_port.mg_lchain_rmq",
                 "minimap_port.collect_seed_hits",
                 "minimap_port.backtrack_chains"},
                {"tool_minimap2.mg_lchain_dp",
                 "tool_minimap2.mg_lchain_rmq",
                 "tool_minimap2.collect_seed_hits",
                 "tool_minimap2.chain_post"});
    print_phase("align (KSW2 skeleton)",
                {"minimap_port.mm_align_skeleton"},
                {"tool_minimap2.mm_align_skeleton",
                 "tool_minimap2.align_regs"});
    print_phase("mapq (final scoring)",
                {"minimap_port.mm_set_mapq"},
                {"tool_minimap2.mm_set_mapq"});
    print_phase("est_err (error estimation)",
                {"minimap_port.mm_est_err"},
                {"tool_minimap2.mm_est_err"});

    cout << rule() << "\n";
}

// ---------------------------------------------------------------------
// 0. Per-section reset. Call before a new BENCHMARK pair so its
//    aggregates are not blended with the previous section.
// ---------------------------------------------------------------------

export inline void reset_aggregates() noexcept {
    bioseq::instr::access::reset_all<args_t>(
        bioseq::instr::persistent_profiler_block<args_t>());
}

/// Install the persistent profiler block on the current thread for the
/// lifetime of the returned guard. The bench harness creates one of these
/// at the top of each TEST_CASE so all `bioseq::instr::region(...)` /
/// `loop(...)` scopes downstream see a non-null tls_profiler.
export struct InstalledProfiler {
    bioseq::instr::installed_profiler_scope<args_t> scope;
    InstalledProfiler() noexcept
        : scope(bioseq::instr::persistent_profiler_block<args_t>()) {}
};

// ---------------------------------------------------------------------
// Public entry point: print the full report block.
// ---------------------------------------------------------------------

export inline void print_section_summary(
    std::string_view section_name,
    const EvaluationResults& results) noexcept
{
    print_instrumentation_summary(section_name);
    print_pmu_summary(section_name);
    print_arena_summary();
    print_top_bottlenecks();
    print_top_call_frequency();
    print_match_quality(results);
    print_length_buckets(results);
    print_side_by_side(results);
    std::cout.flush();
}

/// Variant for single-pair sections — skips the cross-cuts that need a
/// batch.
export inline void print_section_summary(
    std::string_view section_name,
    const SingleEvalResult& /*result*/) noexcept
{
    print_instrumentation_summary(section_name);
    print_pmu_summary(section_name);
    print_arena_summary();
    print_top_bottlenecks();
    print_top_call_frequency();
    std::cout.flush();
}

}  // namespace bioseq::bench::HWY_NAMESPACE::report

HWY_AFTER_NAMESPACE();
