/**
 * @file report.cppm
 * @brief Aligner-bench-specific report sections.
 *
 * After Round 5 the generic instrumentation tables (region + loop
 * summary, PMU counters, arena summary, top-N bottlenecks, top-N
 * call-frequency) live in `bench.scheduler.instrumentation_report`,
 * and the number-formatting helpers (`fmt_duration_10`, `fmt_count`,
 * `fmt_per_visit`, …) live in `bench.scheduler.format`. This module
 * keeps only the three cross-cuts that depend on aligner-domain
 * types (`EvaluationResults`, MAPQ thresholds, the bioseq-vs-minimap2
 * phase delta), plus the `render_name` callable that prefixes
 * `minimap_port.*` / `tool_minimap2.*` based on the descriptor's
 * source file.
 *
 * Output blocks emitted per aligner section:
 *   1. Generic instrumentation report (5 scheduler tables)
 *   2. Match-quality (per MAPQ threshold)
 *   3. Length-bucket cross-cut
 *   4. bioseq vs minimap2 side-by-side
 *
 * The report uses `std::cout` (via `fmt::print`) instead of
 * `JustLog` so the Catch2 BENCHMARK output and our extras share one
 * stream — keeps line ordering predictable when tests are piped
 * through `tee` or compared across runs.
 */
module;

#include <hwy/highway.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

export module bench.aligners.report;

import mlogging;
import bioseq_instr;
import bench.aligners.evaluation;
import bench.scheduler.data_utils.sequence_generator;
import bench.scheduler.format;
import bench.scheduler.instrumentation_report;
import bench.scheduler.stats;

// The bench evaluator types (EvaluationResults, SingleEvalResult,
// AlignerAggregatedStats) live in `bioseq::bench::HWY_NAMESPACE` —
// evaluation.cppm wraps its content with HWY_BEFORE/AFTER_NAMESPACE
// so the dispatch fork can swap targets at runtime. Mirror the same
// shape here so the type names resolve without explicit prefixes.
HWY_BEFORE_NAMESPACE();

namespace bioseq::bench::HWY_NAMESPACE::report {

namespace fmt_ns  = bioseq::bench_scheduler::format;
namespace instr_report = bioseq::bench_scheduler;

/// Map an in-source descriptor name to a render-time display name. The
/// bioseq port and the minimap2 reference both register descriptors
/// like `mm_sketch`, `mm_set_parent`, `mg_lchain_dp`,
/// `mm_align_skeleton`; the bench-level shim prefixes its own with
/// `tool_minimap2.`. Without a corresponding prefix on the bioseq
/// side, the two sets interleave in the report and the reader can't
/// tell which row belongs to which aligner. We don't want to rename
/// at registration (that reaches into bioseq core); instead we add a
/// `minimap_port.` prefix at render time using the descriptor's
/// source file as the discriminator.
[[nodiscard]] inline std::string render_name(const bioseq::instr::RegionDescriptor* desc) {
    if (desc == nullptr || desc->name == nullptr) return std::string{"<anonymous>"};
    const std::string name{desc->name};
    if (desc->file != nullptr) {
        const std::string_view src{desc->file};
        if (src.contains("/bench/tool_minimap2/") ||
            src.contains("/bench/aligners/tool_minimap2/")) {
            return fmt::format("tool_minimap2.{}", name);
        }
    }
    return fmt::format("minimap_port.{}", name);
}

/// `DescriptorRemapFn` value the scheduler printers consume. Returns
/// a fresh `std::function` per call — the only allocation is at
/// `print_section_summary` time, well outside any hot loop.
[[nodiscard]] inline instr_report::DescriptorRemapFn aligner_name_remap() {
    return [](const bioseq::instr::RegionDescriptor* desc) {
        return render_name(desc);
    };
}

// ---------------------------------------------------------------------
// Match-quality (SEAL-style) per MAPQ threshold
// ---------------------------------------------------------------------

inline void print_match_quality(const EvaluationResults& results) {
    using std::cout;
    cout << "[bench.aligners] cross-cut match-quality\n";
    cout << fmt::format(
        "{:<10} {:>6} {:>9} {:>9} {:>9} {:>6} {:>6} {:>6} {:>6} {:>10} {:>10} {:>10} {:>14}\n",
        "aligner", "mapq>=", "mapped", "correct", "incorrect",
        "TP", "FP", "FN", "TN",
        "precision", "recall", "used_read", "mean_offset");
    cout << fmt_ns::rule() << "\n";

    auto print_aligner = [&](std::string_view name, const AlignerAggregatedStats& stats) {
        for (const auto& m : stats.per_threshold) {
            cout << fmt::format(
                "{:<10} {:>6} {:>9} {:>9} {:>9} {:>6} {:>6} {:>6} {:>6} {:>10} {:>10} {:>10} {:>11.2f} bp\n",
                name, m.threshold,
                stats.mapped, stats.correct, stats.incorrect,
                m.true_positives, m.false_positives,
                m.false_negatives, m.true_negatives,
                fmt_ns::fmt_pct(m.precision),
                fmt_ns::fmt_pct(m.recall),
                fmt_ns::fmt_pct(m.used_read_ratio),
                stats.mean_absolute_offset);
        }
    };
    print_aligner("bioseq",   results.bioseq);
    print_aligner("minimap2", results.minimap2);
    cout << fmt_ns::rule() << "\n";
}

// ---------------------------------------------------------------------
// Length-bucket cross-cut. Bins by query length: [0,500) / [500,2000)
// / [2000,+inf).
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

    cout << "[bench.aligners] cross-cut length-bucket\n";
    cout << fmt::format(
        "{:<14} {:>5} {:>16} {:>16} {:>11}\n",
        "length_bp", "n", "mean_bioseq", "mean_mm2", "delta_pct");
    cout << fmt_ns::rule() << "\n";
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
            delta_str = fmt_ns::fmt_signed_pct(100.0 * (mean_bs - mean_mm) / mean_mm);
        }
        cout << fmt::format(
            "{:<14} {:>5} {:>16} {:>16} {:>11}\n",
            b.name, b.n,
            fmt_ns::fmt_duration_10(static_cast<std::uint64_t>(mean_bs)),
            fmt_ns::fmt_duration_10(static_cast<std::uint64_t>(mean_mm)),
            delta_str);
    }
    cout << fmt_ns::rule() << "\n";
}

// ---------------------------------------------------------------------
// bioseq vs minimap2 side-by-side (per-aligner sums of elapsed_ns)
// ---------------------------------------------------------------------

namespace detail {

/// Per-region totals indexed by descriptor name (after `render_name`).
/// When multiple descriptors share the same rendered name (e.g.
/// `minimap_port.mm_sketch` is registered at three call sites in the
/// bioseq port), summing them would double-count the children inside
/// the outer site's region. Use the maximum total_ns and the matching
/// visit count instead — that gives the dominant-call total for the
/// phase, which is what the side-by-side delta wants to compare
/// against minimap2's single registration of the same name.
[[nodiscard]] inline std::unordered_map<std::string, std::pair<std::uint64_t, std::uint64_t>>
collect_region_totals_by_name() {
    std::unordered_map<std::string, std::pair<std::uint64_t, std::uint64_t>> out;
    auto descriptor_stats = instr_report::collect_visited_stats();
    for (const auto& s : descriptor_stats) {
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

}  // namespace detail

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

    cout << "[bench.aligners] cross-cut side-by-side delta (matched phases between bioseq port and minimap2 reference)\n";
    cout << fmt::format(
        "{:<32} {:>18} {:>11} {:>18} {:>11} {:>14}\n",
        "phase", "bioseq_total_ns", "bioseq_mean",
        "mm2_total_ns", "mm2_mean", "speed_delta");
    cout << fmt_ns::rule() << "\n";

    auto delta_pct = [](double a, double b) -> std::string {
        if (b == 0.0) return fmt::format("{:>14}", "-");
        return fmt::format("{:>14}", fmt_ns::fmt_signed_pct(100.0 * (a - b) / b));
    };

    auto totals = detail::collect_region_totals_by_name();
    auto sum_total_ns = [&](std::initializer_list<std::string> names)
        -> std::pair<std::uint64_t, std::uint64_t>
    {
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
        if (bs_t == 0 && mm_t == 0) return;
        cout << fmt::format(
            "{:<32} {:>18} {:>11} {:>18} {:>11} {}\n",
            phase,
            bench_scheduler::stats::with_thousands_separator(bs_t),
            fmt_ns::fmt_duration_10(bs_m),
            bench_scheduler::stats::with_thousands_separator(mm_t),
            fmt_ns::fmt_duration_10(mm_m),
            delta_pct(static_cast<double>(bs_t), static_cast<double>(mm_t)));
    };

    cout << fmt::format(
        "{:<32} {:>18} {:>11} {:>18} {:>11} {}\n",
        "alignment (eval wall-clock)",
        bench_scheduler::stats::with_thousands_separator(bs_total),
        fmt_ns::fmt_duration_10(static_cast<std::uint64_t>(bs_mean)),
        bench_scheduler::stats::with_thousands_separator(mm_total),
        fmt_ns::fmt_duration_10(static_cast<std::uint64_t>(mm_mean)),
        delta_pct(static_cast<double>(bs_total), static_cast<double>(mm_total)));

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

    cout << fmt_ns::rule() << "\n";
}

// ---------------------------------------------------------------------
// Re-export the generic profiler glue so existing callers
// (`report::InstalledProfiler{}` / `report::reset_aggregates()` in
// bench_test.cpp) keep compiling against the same names.
// ---------------------------------------------------------------------

export using bioseq::bench_scheduler::InstalledProfiler;
export inline void reset_aggregates() noexcept {
    bioseq::bench_scheduler::reset_aggregates();
}

// ---------------------------------------------------------------------
// Public entry points: print the full report block.
// ---------------------------------------------------------------------

export inline void print_section_summary(
    std::string_view section_name,
    const EvaluationResults& results) noexcept
{
    instr_report::print_instrumentation_report(section_name, /*top_n=*/8, aligner_name_remap());
    print_match_quality(results);
    print_length_buckets(results);
    print_side_by_side(results);
    std::cout.flush();
}

/// Variant for single-pair sections — skips the cross-cuts that need
/// a batch.
export inline void print_section_summary(
    std::string_view section_name,
    const SingleEvalResult& /*result*/) noexcept
{
    instr_report::print_instrumentation_report(section_name, /*top_n=*/8, aligner_name_remap());
    std::cout.flush();
}

}  // namespace bioseq::bench::HWY_NAMESPACE::report

HWY_AFTER_NAMESPACE();
