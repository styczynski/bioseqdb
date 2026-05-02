/**
 * @file chart_builder.cppm
 * @brief Convert collected `BenchSample` rows into plotly figure JSON.
 *
 * Each chart is one entry under the top-level `charts` map. The layout
 * is fixed by the `report.json` schema:
 *
 *   {
 *     "charts": {
 *       "<chart_id>": {
 *         "short_description": "<one line>",
 *         "long_description": "<paragraph>",
 *         "plotly": {
 *           "data": [...trace objects...],
 *           "layout": {...layout object...}
 *         }
 *       },
 *       ...
 *     }
 *   }
 *
 * Plotly figures are validated by Python's `plotly.io.from_json` —
 * the parser is forgiving about layout fields it doesn't recognise but
 * insists on (a) `data` being an array of objects each with a `type`
 * key, and (b) numeric arrays being well-formed JSON numbers (no NaN
 * literals — emit `null` instead, the JsonWriter does that). We only
 * use trace types in the plotly v6 schema actually emitted here:
 * `scatter`, `bar`, `box`, `heatmap`, `histogram`, `table`. No
 * custom schema is needed downstream.
 *
 * The chart catalogue mirrors the prompt plus suggestions from the
 * critical-review subagent:
 *   - sequence length vs. speed (mean + P99 traces; both pool samples
 *     across all pairs in a length bucket so percentile estimates are
 *     statistically meaningful — the original per-pair P99 of 8
 *     samples lerps `sorted[6..7]` and isn't a tail estimator.)
 *   - sequence length vs. memory consumption
 *   - sequence length vs. accuracy
 *   - error rate vs. accuracy
 *   - accuracy heatmaps (algorithm × length, algorithm × error)
 *   - P50 vs P99 latency (box plot per algorithm)
 *   - throughput (Mbp/s)
 *   - latency CDF per algorithm (subagent suggestion — strictly more
 *     informative than P99 alone)
 *   - Pareto frontier (mean latency vs accuracy)
 *   - tail-amplification ratio
 *   - position-offset histogram
 *   - speedup vs minimap2 baseline
 *   - MAPQ distribution and MAPQ calibration curve
 *   - per-algorithm summary table
 */
module;

#include "common.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

export module bench.aligners.chart_builder;

import mlogging;
import bench.aligners.matrix_runner;
import bench.aligners.samples;
import bench.scheduler.json_writer;
import bench.scheduler.regression;
import bench.scheduler.charting;
import bench.scheduler.stats;
import bench.scheduler.report_envelope;

export {

namespace bioseq::bench {

// Aliases for types relocated into bench_scheduler. Keeping the
// unqualified names lets the chart-emit code below read like a normal
// translation unit; only the boundary types (writer, violation) and
// the violations-array helper need this routing.
using JsonWriter           = bioseq::bench_scheduler::JsonWriter;
using RegressionViolation  = bioseq::bench_scheduler::regression::Violation;
using bioseq::bench_scheduler::regression::emit_violations_json;

namespace detail {

/// Aliases for the generic helpers now living in `bench.scheduler.*`.
/// Keeping the unqualified names lets the chart-emit code below read
/// like before; only this aliasing block knows the helpers came from
/// the scheduler module.
using bench_scheduler::stats::percentile_sorted;
using bench_scheduler::stats::mean_of;

/// Re-export the scheduler's deterministic palette helper under the
/// `detail::` namespace so per-chart bodies keep reading
/// `detail::colour_for_key(algorithm_label)` rather than the longer
/// `bench_scheduler::charting::colour_for_key(...)`. Pure aliasing —
/// zero overhead, no wrapper function.
using bench_scheduler::charting::colour_for_key;

/// Stable ordering of algorithm labels with the two minimap2 baselines
/// pinned to the front so chart legends always read "baselines first,
/// then bioseq variants" regardless of insertion order in the sample
/// vector. The "pin some labels first" primitive is generic; the
/// specific labels we pin are aligner-specific.
[[nodiscard]] inline std::vector<std::string> ordered_algorithm_labels(
    const std::vector<BenchSample>& samples)
{
    std::set<std::string> seen;
    for (const auto& s : samples) seen.insert(s.algorithm_label);
    std::vector<std::string> ordered{seen.begin(), seen.end()};
    static constexpr std::array<std::string_view, 2> pinned = {
        "minimap2 (DP chaining)",
        "minimap2 (RMQ chaining)"};
    bench_scheduler::charting::pin_labels_first(ordered, pinned);
    return ordered;
}

[[nodiscard]] inline std::string length_bucket_short_label(
    SequenceLengthBucket bucket) noexcept
{
    switch (bucket) {
        case SequenceLengthBucket::SHORT:     return "short";
        case SequenceLengthBucket::MEDIUM:    return "medium";
        case SequenceLengthBucket::LONG:      return "long";
        case SequenceLengthBucket::VERY_LONG: return "very_long";
        case SequenceLengthBucket::XL:        return "xl";
        case SequenceLengthBucket::XXL:       return "xxl";
    }
    return "?";
}

}  // namespace detail

/**
 * @brief Top-level emitter — populates the writer with the full
 *        `report.json` document.
 *
 * The optional `violations` vector is forwarded straight into a
 * top-level `violations` array so dashboards and CI scripts can pivot
 * on it without re-running the regression checker themselves. Empty
 * vectors still emit `"violations": []` so consumers can tell "no
 * violations found" apart from "schema missing the field".
 *
 * `commit_hash` is the git SHA the build script captured before
 * launching the bench. It lands at the top of the document so reports
 * can be matched back to the source state that produced them. Pass
 * "unknown" when running outside a git checkout.
 */
inline void emit_report(
    JsonWriter& writer,
    std::span<const RegressionViolation> violations = {},
    std::string_view commit_hash = "unknown")
{
    const auto& samples = benchmark_samples();
    const auto algorithm_labels = detail::ordered_algorithm_labels(samples);

    bench_scheduler::ReportMetadata meta;
    meta.schema          = "bioseq-bench-report-v1";
    meta.commit_hash     = commit_hash;
    meta.sample_count    = samples.size();
    meta.algorithm_count = algorithm_labels.size();
    meta.violations      = violations;

    bench_scheduler::emit_report_envelope(writer, meta,
        [&]() {
            // The envelope opens the `charts` object before the body
            // runs and closes it after; charts emit one entry each
            // via the helpers below. The writer is captured so the
            // per-chart code reads exactly like the pre-refactor
            // `writer.kv_*` chain.

    // ----------------------------------------------------------------
    // Local lambdas — thin wrappers over the scheduler's chart
    // primitives so the per-chart bodies below stay readable. The
    // lambdas pre-bind the writer reference; the scheduler helpers
    // take it explicitly so they can be reused outside this function.
    // ----------------------------------------------------------------
    auto emit_chart = [&writer](
        std::string_view chart_id,
        std::string_view short_description,
        std::string_view long_description,
        auto&& body)
    {
        bench_scheduler::charting::emit_chart_envelope(
            writer, chart_id, short_description, long_description,
            std::forward<decltype(body)>(body));
    };

    auto emit_layout = [&writer](
        std::string_view title,
        std::string_view x_title, bool x_log,
        std::string_view y_title, bool y_log,
        std::string_view barmode = "")
    {
        bench_scheduler::charting::emit_plotly_layout(
            writer, title, x_title, x_log, y_title, y_log, barmode);
    };

    auto emit_marker = [&writer](std::string_view algorithm_label) {
        bench_scheduler::charting::emit_plotly_marker(
            writer, bench_scheduler::charting::colour_for_key(algorithm_label));
    };

    // ================================================================
    // Headline algorithm subset.
    //
    // The default 14-algo grid is too crowded for the "are we faster
    // / more accurate than minimap2?" headline charts; the reader's
    // eye gets lost across overlapping traces. We pick four traces
    // for the headline charts: the two minimap2 baselines + the two
    // bioseq KSW2 variants whose chaining choice (scalar-DP, SIMD-DP)
    // matches what minimap2's mm_chain_dp does. These are the
    // apples-to-apples comparisons. The full-grid charts further down
    // the report show every variant for users that want the detail.
    //
    // Note: SIMD-mm minimizer variants are deliberately excluded from
    // the headline. Production output documented in
    // CODE_GUIDELINES.md §9 uses scalar minimizers (the SIMD path is
    // a separate experiment), and the previous bench run showed the
    // SIMD-mm variants at notably worse accuracy on SHORT/MEDIUM —
    // they belong in the full-grid charts where the regression is
    // visible, not in the headline answer.
    static constexpr std::array<std::string_view, 4> headline_algorithms = {
        "minimap2 (DP chaining)",
        "minimap2 (RMQ chaining)",
        "bioseq KSW2 + scalar-DP + scalar-mm",
        "bioseq KSW2 + SIMD-DP + scalar-mm",
    };

    auto is_headline_algorithm = [](std::string_view label) {
        for (const auto& candidate : headline_algorithms) {
            if (candidate == label) return true;
        }
        return false;
    };

    auto headline_algorithm_labels = [&]() {
        std::vector<std::string> out;
        for (const auto& label : algorithm_labels) {
            if (is_headline_algorithm(label)) out.push_back(label);
        }
        return out;
    }();

    // ================================================================
    // Headline 1: How fast is bioseq vs. minimap2 by length bucket?
    //
    // Per (algorithm, length-bucket) cell we plot the mean per-pair
    // latency in ms. Only the four headline algorithms participate so
    // the chart reads as a 1-glance speedometer rather than the dense
    // 14-trace soup of the full-grid version.
    // ================================================================
    emit_chart("headline_speed_vs_minimap2",
        "Headline: mean alignment latency vs. minimap2 (4 algorithms)",
        "First chart by design — the headline 'how fast are we vs. "
        "minimap2?' answer. Y axis: mean per-pair alignment latency in "
        "milliseconds (lower is better, log scale). X axis: sequence "
        "length bucket (SHORT/MEDIUM/LONG/VERY_LONG). Only the two "
        "minimap2 baselines and the two bioseq KSW2 variants whose "
        "chaining matches minimap2 (scalar-DP, SIMD-DP) participate, "
        "so the visual comparison is apples-to-apples. The full 14-"
        "algorithm grid is plotted further down (`seq_length_vs_speed_mean`) "
        "for users who want every variant.",
        [&]() {
            writer.key("data");
            writer.begin_array();
            for (const auto& algorithm_label : headline_algorithm_labels) {
                std::map<int, std::vector<double>> bucket_latencies;
                for (const auto& s : samples) {
                    if (s.algorithm_label != algorithm_label) continue;
                    if (s.sample_latencies_ns.empty()) continue;
                    bucket_latencies[static_cast<int>(s.length_bucket)]
                        .push_back(detail::mean_of(s.sample_latencies_ns) / 1e6);
                }
                if (bucket_latencies.empty()) continue;
                std::vector<std::string> xs;
                std::vector<double> ys;
                for (const auto& [bucket, latencies] : bucket_latencies) {
                    if (latencies.empty()) continue;
                    xs.push_back(std::string{sequence_length_bucket_name(
                        static_cast<SequenceLengthBucket>(bucket))});
                    ys.push_back(detail::mean_of(latencies));
                }
                writer.begin_object();
                writer.kv_string("type", "scatter");
                writer.kv_string("mode", "lines+markers");
                writer.kv_string("name", algorithm_label);
                writer.kv_string_array("x", xs);
                writer.kv_double_array("y", ys);
                emit_marker(algorithm_label);
                writer.end_object();
            }
            writer.end_array();
            emit_layout(
                "Headline: mean latency by length bucket — bioseq vs. minimap2",
                "length bucket", false,
                "mean latency per call (ms, log)", true);
        });

    // ================================================================
    // Headline 2: How accurate is bioseq vs. minimap2 by length?
    // ================================================================
    emit_chart("headline_accuracy_vs_minimap2_per_length",
        "Headline: position accuracy vs. minimap2 (by length bucket)",
        "Position-accuracy fraction by length bucket for the two "
        "minimap2 baselines and the two bioseq KSW2 variants. A flat "
        "line at 1.0 means the algorithm is indifferent to read "
        "length; a downward slope at the long end usually points at "
        "chaining failures or seed drop-out. Buckets the runner "
        "skipped (the `should_skip_cell` rules — see `matrix_runner.cppm`) "
        "show as missing points; lines connect across the gap.",
        [&]() {
            constexpr std::array<SequenceLengthBucket, 6> all_buckets = {
                SequenceLengthBucket::SHORT,
                SequenceLengthBucket::MEDIUM,
                SequenceLengthBucket::LONG,
                SequenceLengthBucket::VERY_LONG,
                SequenceLengthBucket::XL,
                SequenceLengthBucket::XXL,
            };
            writer.key("data");
            writer.begin_array();
            for (const auto& algorithm_label : headline_algorithm_labels) {
                std::array<std::pair<std::size_t, std::size_t>, 6> per_bucket{};
                for (const auto& s : samples) {
                    if (s.algorithm_label != algorithm_label) continue;
                    const auto bidx = static_cast<std::size_t>(s.length_bucket);
                    if (bidx >= per_bucket.size()) continue;
                    auto& counts = per_bucket[bidx];
                    counts.second++;
                    if (s.position_correct) counts.first++;
                }
                bool any = false;
                for (const auto& [c, t] : per_bucket) if (t > 0) { any = true; break; }
                if (!any) continue;
                writer.begin_object();
                writer.kv_string("type", "scatter");
                writer.kv_string("mode", "lines+markers");
                writer.kv_string("name", algorithm_label);
                writer.key("x");
                writer.begin_array();
                for (auto bucket : all_buckets) {
                    writer.value_string(sequence_length_bucket_name(bucket));
                }
                writer.end_array();
                writer.key("y");
                writer.begin_array();
                for (std::size_t i = 0; i < per_bucket.size(); ++i) {
                    if (per_bucket[i].second == 0) {
                        writer.value_null();
                    } else {
                        writer.value_double(
                            static_cast<double>(per_bucket[i].first) /
                            static_cast<double>(per_bucket[i].second));
                    }
                }
                writer.end_array();
                writer.kv_bool("connectgaps", false);
                emit_marker(algorithm_label);
                writer.end_object();
            }
            writer.end_array();
            emit_layout(
                "Headline: position accuracy by length bucket",
                "length bucket", false,
                "fraction of correctly-placed primaries", false);
        });

    // ================================================================
    // Headline 3: How robust is bioseq vs. minimap2 across error rates?
    //
    // Pivots on error_bucket on x-axis with one trace per headline
    // algorithm. Reads as the 'SEAL-style robustness curve' across
    // simulated error rates from HIFI to ONT_LEGACY.
    // ================================================================
    emit_chart("headline_accuracy_vs_minimap2_per_error",
        "Headline: position accuracy vs. minimap2 (by error rate bucket)",
        "Position-accuracy fraction across the six error-rate buckets, "
        "ordered HIFI -> CCS -> ONT_MODERN -> ONT_LEGACY -> "
        "HIGH_INDEL_LOW_SUB -> HIGH_SUB_LOW_INDEL. The first four "
        "buckets are calibrated against published long-read error "
        "profiles (see SOME_BENCHMARKING_PAPERS.md and the "
        "ErrorRateBucket enum in matrix_runner.cppm); the last two "
        "are exotic indel-only / sub-only stress tests inspired by "
        "the PeerJ Fig. 7 sweep. Watch for accuracy collapse at the "
        "ONT_LEGACY / HIGH_INDEL crossover — that's where minimap2's "
        "default k/w (15, 10) starts to lose seeds.",
        [&]() {
            constexpr std::array<ErrorRateBucket, 6> all_errors = {
                ErrorRateBucket::HIFI,
                ErrorRateBucket::CCS,
                ErrorRateBucket::ONT_MODERN,
                ErrorRateBucket::ONT_LEGACY,
                ErrorRateBucket::HIGH_INDEL_LOW_SUB,
                ErrorRateBucket::HIGH_SUB_LOW_INDEL,
            };
            writer.key("data");
            writer.begin_array();
            for (const auto& algorithm_label : headline_algorithm_labels) {
                std::array<std::pair<std::size_t, std::size_t>, 6> per_bucket{};
                for (const auto& s : samples) {
                    if (s.algorithm_label != algorithm_label) continue;
                    auto& counts = per_bucket[static_cast<std::size_t>(s.error_bucket)];
                    counts.second++;
                    if (s.position_correct) counts.first++;
                }
                bool any = false;
                for (const auto& [c, t] : per_bucket) if (t > 0) { any = true; break; }
                if (!any) continue;
                writer.begin_object();
                writer.kv_string("type", "scatter");
                writer.kv_string("mode", "lines+markers");
                writer.kv_string("name", algorithm_label);
                writer.key("x");
                writer.begin_array();
                for (auto bucket : all_errors) {
                    writer.value_string(error_rate_bucket_name(bucket));
                }
                writer.end_array();
                writer.key("y");
                writer.begin_array();
                for (std::size_t i = 0; i < 6; ++i) {
                    if (per_bucket[i].second == 0) {
                        writer.value_null();
                    } else {
                        writer.value_double(
                            static_cast<double>(per_bucket[i].first) /
                            static_cast<double>(per_bucket[i].second));
                    }
                }
                writer.end_array();
                writer.kv_bool("connectgaps", false);
                emit_marker(algorithm_label);
                writer.end_object();
            }
            writer.end_array();
            emit_layout(
                "Headline: position accuracy by error rate",
                "error rate bucket", false,
                "fraction of correctly-placed primaries", false);
        });

    // ================================================================
    // Headline 4: Pareto — best-of-both view restricted to the four
    // headline algorithms. This is the same shape as the full Pareto
    // chart further down, but with only 4 algorithms × 4 length
    // buckets = up to 16 markers, so the frontier is readable.
    // ================================================================
    emit_chart("headline_pareto_speed_vs_accuracy",
        "Headline: speed vs. accuracy Pareto (4 algorithms)",
        "Per (algorithm, length-bucket) marker: X is mean latency in "
        "ms (lower is better), Y is position-accuracy fraction "
        "(higher is better). Points in the upper-left dominate "
        "(faster AND more accurate); points in the lower-right are "
        "dominated. The full 14-algorithm Pareto further down "
        "(`pareto_accuracy_vs_latency`) preserves every variant.",
        [&]() {
            writer.key("data");
            writer.begin_array();
            // 5 symbols: SHORT, MEDIUM, LONG, VERY_LONG, XL — keeping
            // the array sized to match the SequenceLengthBucket enum
            // ensures `bidx` indexing below stays in-bounds when the
            // bench's XL bucket is included (e.g. by the long-only
            // iteration benches).
            const std::array<std::string_view, 5> length_symbols = {
                "circle", "square", "diamond", "triangle-up", "star"};
            for (const auto& algorithm_label : headline_algorithm_labels) {
                std::array<std::pair<std::vector<double>, std::vector<double>>, 5> per_bucket;
                std::array<std::pair<std::size_t, std::size_t>, 6> correctness;
                for (const auto& s : samples) {
                    if (s.algorithm_label != algorithm_label) continue;
                    if (s.sample_latencies_ns.empty()) continue;
                    const auto bidx = static_cast<std::size_t>(s.length_bucket);
                    if (bidx >= per_bucket.size()) continue;
                    per_bucket[bidx].first.push_back(detail::mean_of(s.sample_latencies_ns) / 1e6);
                    correctness[bidx].second++;
                    if (s.position_correct) correctness[bidx].first++;
                }
                for (std::size_t i = 0; i < per_bucket.size(); ++i) {
                    if (per_bucket[i].first.empty()) continue;
                    const double mean_latency_ms = detail::mean_of(per_bucket[i].first);
                    const double accuracy = correctness[i].second > 0
                        ? static_cast<double>(correctness[i].first) /
                          static_cast<double>(correctness[i].second)
                        : 0.0;
                    writer.begin_object();
                    writer.kv_string("type", "scatter");
                    writer.kv_string("mode", "markers");
                    writer.kv_string("name",
                        algorithm_label + " — " +
                        std::string{detail::length_bucket_short_label(
                            static_cast<SequenceLengthBucket>(i))});
                    writer.kv_string("legendgroup", algorithm_label);
                    writer.key("x");
                    writer.begin_array(); writer.value_double(mean_latency_ms); writer.end_array();
                    writer.key("y");
                    writer.begin_array(); writer.value_double(accuracy); writer.end_array();
                    writer.key("marker");
                    writer.begin_object();
                    writer.kv_string("color", detail::colour_for_key(algorithm_label));
                    writer.kv_int("size", 13);
                    writer.kv_string("symbol", length_symbols[i]);
                    writer.end_object();
                    writer.end_object();
                }
            }
            writer.end_array();
            emit_layout(
                "Headline: speed vs. accuracy Pareto",
                "mean latency per call (ms, log)", true,
                "position-accuracy fraction", false);
        });

    // ================================================================
    // k/w sweep charts. These come from a separate test case
    // (`run_kw_sweep`) that runs a small set of (kmer_size,
    // window_size) configurations on a fixed length×error bucket so
    // the resulting accuracy/speed depends only on (k, w).
    //
    // The chart filters samples to those whose `algorithm_label`
    // matches the k/w-sweep label pattern (`(k=...,w=...)` suffix).
    // The default-grid samples — which all share k=15, w=10 — are
    // excluded; mixing them in would conflate "default grid baseline"
    // with "k=15,w=10 from the sweep" and the heatmap rows would
    // double up.
    // ================================================================

    auto is_kw_sweep_label = [](std::string_view label) {
        // The k/w-sweep labels end with ` (k=..,w=..)`; the default
        // grid never injects parentheses into its labels.
        return label.find("(k=") != std::string_view::npos &&
               label.find(",w=") != std::string_view::npos;
    };

    auto emit_kw_heatmap = [&](
        std::string_view chart_id,
        std::string_view short_description,
        std::string_view long_description,
        std::string_view algorithm_kind_filter,
        bool emit_accuracy)
    {
        emit_chart(chart_id, short_description, long_description, [&]() {
            // Distinct k and w values pulled from the samples.
            std::set<int32_t> kmer_set, window_set;
            for (const auto& s : samples) {
                if (!is_kw_sweep_label(s.algorithm_label)) continue;
                if (s.algorithm_kind != algorithm_kind_filter) continue;
                kmer_set.insert(s.kmer_size);
                window_set.insert(s.window_size);
            }
            std::vector<int32_t> kmer_axis(kmer_set.begin(), kmer_set.end());
            std::vector<int32_t> window_axis(window_set.begin(), window_set.end());
            // (kmer × window) cell -> (correct, total) for accuracy
            // or sum/count of mean latencies for speed.
            const std::size_t n_k = kmer_axis.size();
            const std::size_t n_w = window_axis.size();
            std::vector<std::vector<std::pair<std::size_t, std::size_t>>> accuracy_grid(
                n_k, std::vector<std::pair<std::size_t, std::size_t>>(n_w));
            std::vector<std::vector<std::vector<double>>> latency_grid(
                n_k, std::vector<std::vector<double>>(n_w));
            for (const auto& s : samples) {
                if (!is_kw_sweep_label(s.algorithm_label)) continue;
                if (s.algorithm_kind != algorithm_kind_filter) continue;
                const auto k_it = std::find(kmer_axis.begin(), kmer_axis.end(), s.kmer_size);
                const auto w_it = std::find(window_axis.begin(), window_axis.end(), s.window_size);
                if (k_it == kmer_axis.end() || w_it == window_axis.end()) continue;
                const std::size_t kk = static_cast<std::size_t>(std::distance(kmer_axis.begin(), k_it));
                const std::size_t ww = static_cast<std::size_t>(std::distance(window_axis.begin(), w_it));
                accuracy_grid[kk][ww].second++;
                if (s.position_correct) accuracy_grid[kk][ww].first++;
                if (!s.sample_latencies_ns.empty()) {
                    latency_grid[kk][ww].push_back(detail::mean_of(s.sample_latencies_ns) / 1e6);
                }
            }
            // String labels for axes — Plotly heatmaps want strings to
            // disambiguate categorical axes; otherwise they auto-scale
            // and the (10, 5) and (15, 10) cells render at unequal
            // pixel widths.
            std::vector<std::string> kmer_labels;
            for (auto k : kmer_axis) kmer_labels.push_back("k=" + std::to_string(k));
            std::vector<std::string> window_labels;
            for (auto w : window_axis) window_labels.push_back("w=" + std::to_string(w));
            writer.key("data");
            writer.begin_array();
            writer.begin_object();
            writer.kv_string("type", "heatmap");
            writer.kv_string("colorscale", emit_accuracy ? "Viridis" : "Cividis");
            writer.kv_string_array("x", window_labels);
            writer.kv_string_array("y", kmer_labels);
            writer.key("z");
            writer.begin_array();
            for (std::size_t kk = 0; kk < n_k; ++kk) {
                writer.begin_array();
                for (std::size_t ww = 0; ww < n_w; ++ww) {
                    if (emit_accuracy) {
                        const auto [correct, total] = accuracy_grid[kk][ww];
                        if (total == 0) {
                            writer.value_null();
                        } else {
                            writer.value_double(static_cast<double>(correct) /
                                               static_cast<double>(total));
                        }
                    } else {
                        const auto& latencies = latency_grid[kk][ww];
                        if (latencies.empty()) {
                            writer.value_null();
                        } else {
                            writer.value_double(detail::mean_of(latencies));
                        }
                    }
                }
                writer.end_array();
            }
            writer.end_array();
            if (emit_accuracy) {
                writer.kv_double("zmin", 0.0);
                writer.kv_double("zmax", 1.0);
            }
            writer.key("colorbar");
            writer.begin_object();
            writer.key("title");
            writer.begin_object();
            writer.kv_string("text", emit_accuracy
                ? "fraction correct" : "mean latency (ms)");
            writer.end_object();
            writer.end_object();
            writer.end_object();
            writer.end_array();
            emit_layout(
                std::string{short_description},
                "window size (w)", false,
                "k-mer size (k)", false);
        });
    };

    emit_kw_heatmap(
        "kmer_window_accuracy_minimap2",
        "k/w sweep — accuracy heatmap (minimap2 reference)",
        "Accuracy fraction for the C minimap2 reference as (k, w) "
        "vary over the canonical preset combinations: k in {10, 15, "
        "19, 21, 25}, w in {5, 10, 11}. Run on the MEDIUM/CCS bucket "
        "(5 kbp pairs at ~1.5% error) so that every cell completes in "
        "seconds and the signal isolates the parameter impact. "
        "Brighter (higher) cells = better accuracy. Sweep cells with "
        "zero samples render as missing (Plotly null); see "
        "`run_kw_sweep` in matrix_runner.cppm for which (k, w) "
        "combinations are exercised.",
        "minimap2",
        true);

    emit_kw_heatmap(
        "kmer_window_accuracy_bioseq",
        "k/w sweep — accuracy heatmap (bioseq KSW2 + scalar-DP + scalar-mm)",
        "Same axes as the minimap2 k/w heatmap, restricted to the "
        "bioseq KSW2 + scalar-DP + scalar-mm variant (the closest "
        "algorithmic match to minimap2's default chain+align "
        "pipeline). Direct compare: a cell that's brighter here than "
        "in the minimap2 heatmap means our port is more sensitive at "
        "that (k, w) point; darker = less sensitive. Use to spot "
        "(k, w) regions where the port has a bug or has gained an "
        "advantage.",
        "bioseq",
        true);

    emit_kw_heatmap(
        "kmer_window_speed_minimap2",
        "k/w sweep — mean latency heatmap (minimap2 reference)",
        "Mean per-pair latency (ms) for the C minimap2 reference as "
        "(k, w) vary. Cividis colourmap — darker = faster. Smaller k "
        "produces more minimizers (denser hit table, slower seeding); "
        "smaller w produces more minimizers per window (denser still). "
        "Read together with the accuracy heatmap to pick the "
        "(k, w) point with the best sensitivity / speed trade-off.",
        "minimap2",
        false);

    emit_kw_heatmap(
        "kmer_window_speed_bioseq",
        "k/w sweep — mean latency heatmap (bioseq KSW2 + scalar-DP + scalar-mm)",
        "Same axes as the minimap2 k/w speed heatmap, restricted to "
        "the bioseq KSW2 + scalar-DP + scalar-mm variant. Compare to "
        "the minimap2 version above: cells where bioseq is darker "
        "(faster) indicate the port has a real speed advantage at "
        "that (k, w); cells where bioseq is lighter (slower) are the "
        "candidates for further optimization work.",
        "bioseq",
        false);

    // ================================================================
    // Chart 1: sequence length vs. mean speed (latency / pair).
    // Each algorithm contributes one trace; x is per-pair effective
    // query length, y is the mean of that pair's sample latencies.
    // ================================================================
    emit_chart("seq_length_vs_speed_mean",
        "Mean alignment latency vs. effective query length",
        "Mean wall-clock latency per (query, target) pair as a function of "
        "the effective query length (post-error length, in bp). One trace "
        "per algorithm configuration; both axes log-scale because the "
        "length axis spans short reads (300 bp) through assembly fragments "
        "(2 Mbp). Lower lines = faster algorithms. Use this chart to spot "
        "algorithmic complexity: linear-in-length lines indicate O(n) "
        "behaviour, super-linear curves point at O(n log n) or worse "
        "regions in the chosen pipeline.",
        [&]() {
            writer.key("data");
            writer.begin_array();
            for (const auto& algorithm_label : algorithm_labels) {
                std::vector<double> xs, ys;
                for (const auto& s : samples) {
                    if (s.algorithm_label != algorithm_label) continue;
                    if (s.sample_latencies_ns.empty()) continue;
                    xs.push_back(static_cast<double>(s.effective_query_length));
                    ys.push_back(detail::mean_of(s.sample_latencies_ns) / 1e6);  // ms
                }
                if (xs.empty()) continue;
                writer.begin_object();
                writer.kv_string("type", "scatter");
                writer.kv_string("mode", "markers");
                writer.kv_string("name", algorithm_label);
                writer.kv_double_array("x", xs);
                writer.kv_double_array("y", ys);
                emit_marker(algorithm_label);
                writer.end_object();
            }
            writer.end_array();
            emit_layout(
                "Mean alignment latency vs. effective query length",
                "effective query length (bp)", true,
                "mean latency per pair (ms)", true);
        });

    // ================================================================
    // Chart 2: per-(algorithm, length-bucket) P50 / P99 latency.
    //
    // The original implementation computed the P99 of each per-pair
    // 8-sample vector — mathematically meaningless. Here we pool all
    // per-sample latencies across every pair in a length bucket
    // (16 pairs × 8 samples = 128 samples per SHORT cell) before
    // taking percentiles. Two trace families per algorithm: solid
    // line for P50, dashed line for P99 — Plotly renders them
    // distinguishably with `line.dash`.
    // ================================================================
    emit_chart("seq_length_vs_speed_p99",
        "Pooled P50 and P99 latency per (algorithm, length bucket)",
        "Per-bucket pooled P50 and P99 latencies. For each algorithm "
        "and length bucket we pool every per-sample timing across all "
        "pairs in that bucket (16 pairs × 8 samples = 128 samples for "
        "SHORT, 24 for MEDIUM, 12 for LONG, 1 for VERY_LONG) before "
        "computing percentiles, so the P99 is over a sufficient "
        "sample size (except VERY_LONG, where it degenerates to the "
        "single observation). Solid markers = P50, hollow markers = "
        "P99. The vertical gap between the two for one algorithm is "
        "the tail width: large gaps indicate a heavy-tailed latency "
        "distribution and small gaps indicate a tight distribution.",
        [&]() {
            writer.key("data");
            writer.begin_array();
            for (const auto& algorithm_label : algorithm_labels) {
                std::map<int, std::vector<double>> bucket_pool;
                for (const auto& s : samples) {
                    if (s.algorithm_label != algorithm_label) continue;
                    auto& pool = bucket_pool[static_cast<int>(s.length_bucket)];
                    pool.insert(pool.end(),
                                s.sample_latencies_ns.begin(),
                                s.sample_latencies_ns.end());
                }
                if (bucket_pool.empty()) continue;
                std::vector<std::string> xs;
                std::vector<double> p50s, p99s;
                for (auto& [bucket, pool] : bucket_pool) {
                    if (pool.empty()) continue;
                    std::sort(pool.begin(), pool.end());
                    xs.push_back(std::string{sequence_length_bucket_name(
                        static_cast<SequenceLengthBucket>(bucket))});
                    p50s.push_back(detail::percentile_sorted(pool, 0.50) / 1e6);
                    p99s.push_back(detail::percentile_sorted(pool, 0.99) / 1e6);
                }
                writer.begin_object();
                writer.kv_string("type", "scatter");
                writer.kv_string("mode", "lines+markers");
                writer.kv_string("name", algorithm_label + " P50");
                writer.kv_string("legendgroup", algorithm_label);
                writer.kv_string_array("x", xs);
                writer.kv_double_array("y", p50s);
                emit_marker(algorithm_label);
                writer.end_object();
                writer.begin_object();
                writer.kv_string("type", "scatter");
                writer.kv_string("mode", "lines+markers");
                writer.kv_string("name", algorithm_label + " P99");
                writer.kv_string("legendgroup", algorithm_label);
                writer.kv_string_array("x", xs);
                writer.kv_double_array("y", p99s);
                writer.key("line");
                writer.begin_object();
                writer.kv_string("color", detail::colour_for_key(algorithm_label));
                writer.kv_string("dash", "dash");
                writer.kv_int("width", 2);
                writer.end_object();
                writer.key("marker");
                writer.begin_object();
                writer.kv_string("color", detail::colour_for_key(algorithm_label));
                writer.kv_int("size", 7);
                writer.kv_string("symbol", "circle-open");
                writer.end_object();
                writer.end_object();
            }
            writer.end_array();
            emit_layout(
                "P50 (solid) and P99 (dashed open) per length bucket — pooled",
                "length bucket", false,
                "latency (ms)", true);
        });

    // ================================================================
    // Chart 3: sequence length vs. memory consumption.
    // Y is the mean current-RSS delta (bytes) per call. The matrix
    // runner now reads instantaneous RSS via task_info / statm
    // instead of getrusage's monotonic ru_maxrss, so per-call deltas
    // reflect actual working-set churn. Traces whose mean is exactly
    // zero across every sample are dropped — usually means the
    // platform doesn't expose a current-RSS reading, or the call
    // returned memory to the allocator immediately.
    // ================================================================
    emit_chart("seq_length_vs_memory",
        "Working-set delta (current RSS) vs. effective query length",
        "Mean increase in current resident-set size per aligner "
        "invocation, plotted against the effective query length. The "
        "RSS reading uses `task_info(TASK_BASIC_INFO).resident_size` "
        "on macOS and `/proc/self/statm` on Linux — instantaneous "
        "current-RSS, not lifetime peak — so the per-call delta is a "
        "live working-set measurement. Negative deltas (the allocator "
        "released pages between calls) are clamped to zero so the y "
        "axis stays positive. Traces with all-zero deltas are hidden.",
        [&]() {
            writer.key("data");
            writer.begin_array();
            for (const auto& algorithm_label : algorithm_labels) {
                std::vector<double> xs, ys;
                bool any_nonzero = false;
                for (const auto& s : samples) {
                    if (s.algorithm_label != algorithm_label) continue;
                    if (s.sample_peak_rss_delta_bytes.empty()) continue;
                    const double mean_bytes = detail::mean_of(s.sample_peak_rss_delta_bytes);
                    xs.push_back(static_cast<double>(s.effective_query_length));
                    ys.push_back(mean_bytes);
                    if (mean_bytes > 0.0) any_nonzero = true;
                }
                if (xs.empty() || !any_nonzero) continue;
                writer.begin_object();
                writer.kv_string("type", "scatter");
                writer.kv_string("mode", "markers");
                writer.kv_string("name", algorithm_label);
                writer.kv_double_array("x", xs);
                writer.kv_double_array("y", ys);
                emit_marker(algorithm_label);
                writer.end_object();
            }
            writer.end_array();
            emit_layout(
                "Mean RSS delta vs. effective query length",
                "effective query length (bp)", true,
                "mean RSS delta per call (bytes)", true);
        });

    // ================================================================
    // Chart 4: sequence length vs. accuracy (per algorithm).
    // Bins by length bucket and reports the fraction of correctly-
    // placed primaries (within 25 bp of the truth) per bucket.
    // ================================================================
    emit_chart("seq_length_vs_accuracy",
        "Position accuracy vs. sequence length bucket",
        "Fraction of (query, target) pairs whose primary alignment "
        "lands within tolerance of the simulated truth, bucketed by "
        "sequence length. Every algorithm trace spans all four length "
        "buckets — buckets with no samples (e.g. RMQ + VERY_LONG when "
        "skipped) are emitted as JSON null so the line connects the "
        "remaining points without misrepresenting "
        "missing data as zero. A flat line near 1.0 means the "
        "algorithm is indifferent to read length; a downward slope at "
        "the long / very-long end usually points at chaining failures "
        "or seed drop-out. The minimap2 reference traces are the "
        "de-facto upper bound — bioseq variants below them indicate a "
        "regression in the corresponding phase.",
        [&]() {
            constexpr std::array<SequenceLengthBucket, 6> all_buckets = {
                SequenceLengthBucket::SHORT,
                SequenceLengthBucket::MEDIUM,
                SequenceLengthBucket::LONG,
                SequenceLengthBucket::VERY_LONG,
                SequenceLengthBucket::XL,
                SequenceLengthBucket::XXL,
            };
            writer.key("data");
            writer.begin_array();
            for (const auto& algorithm_label : algorithm_labels) {
                std::array<std::pair<std::size_t, std::size_t>, 6> per_bucket{};
                for (const auto& s : samples) {
                    if (s.algorithm_label != algorithm_label) continue;
                    const auto bidx = static_cast<std::size_t>(s.length_bucket);
                    if (bidx >= per_bucket.size()) continue;
                    auto& counts = per_bucket[bidx];
                    counts.second++;
                    if (s.position_correct) counts.first++;
                }
                bool any = false;
                for (const auto& [c, t] : per_bucket) if (t > 0) { any = true; break; }
                if (!any) continue;
                writer.begin_object();
                writer.kv_string("type", "scatter");
                writer.kv_string("mode", "lines+markers");
                writer.kv_string("name", algorithm_label);
                writer.key("x");
                writer.begin_array();
                for (auto bucket : all_buckets) {
                    writer.value_string(sequence_length_bucket_name(bucket));
                }
                writer.end_array();
                writer.key("y");
                writer.begin_array();
                for (std::size_t i = 0; i < per_bucket.size(); ++i) {
                    if (per_bucket[i].second == 0) {
                        writer.value_null();
                    } else {
                        writer.value_double(
                            static_cast<double>(per_bucket[i].first) /
                            static_cast<double>(per_bucket[i].second));
                    }
                }
                writer.end_array();
                writer.kv_bool("connectgaps", false);
                emit_marker(algorithm_label);
                writer.end_object();
            }
            writer.end_array();
            emit_layout(
                "Position accuracy vs. sequence length bucket",
                "length bucket", false,
                "fraction of correctly-placed primaries", false);
        });

    // ================================================================
    // Chart 5: error rate vs. accuracy (grouped bar).
    // X = error rate bucket; one bar per algorithm; height = fraction
    // of correctly-placed primaries within that error bucket.
    // ================================================================
    emit_chart("error_rate_vs_accuracy",
        "Position accuracy vs. simulated error rate",
        "Grouped bar chart of position accuracy across the three error "
        "buckets (low / medium / high). Each bar group corresponds to "
        "one error rate; bars within a group are the algorithm "
        "configurations. Use this chart to identify configurations that "
        "degrade gracefully (their bar shrinks slowly across buckets) "
        "vs. those that collapse beyond the medium bucket. The error "
        "rate is the sum of substitution + insertion + deletion rates "
        "applied to each fragment by the SEAL-style generator.",
        [&]() {
            writer.key("data");
            writer.begin_array();
            for (const auto& algorithm_label : algorithm_labels) {
                std::map<int, std::pair<std::size_t, std::size_t>> bucket_counts;
                for (const auto& s : samples) {
                    if (s.algorithm_label != algorithm_label) continue;
                    auto& counts = bucket_counts[static_cast<int>(s.error_bucket)];
                    counts.second++;
                    if (s.position_correct) counts.first++;
                }
                if (bucket_counts.empty()) continue;
                std::vector<std::string> xs;
                std::vector<double> ys;
                for (const auto& [bucket, counts] : bucket_counts) {
                    xs.push_back(error_rate_bucket_name(
                        static_cast<ErrorRateBucket>(bucket)));
                    ys.push_back(counts.second > 0
                        ? static_cast<double>(counts.first) /
                          static_cast<double>(counts.second)
                        : 0.0);
                }
                writer.begin_object();
                writer.kv_string("type", "bar");
                writer.kv_string("name", algorithm_label);
                writer.kv_string_array("x", xs);
                writer.kv_double_array("y", ys);
                writer.key("marker");
                writer.begin_object();
                writer.kv_string("color", detail::colour_for_key(algorithm_label));
                writer.end_object();
                writer.end_object();
            }
            writer.end_array();
            emit_layout(
                "Position accuracy vs. simulated error rate",
                "error rate bucket", false,
                "fraction of correctly-placed primaries", false,
                "group");
        });

    // ================================================================
    // Chart 6: accuracy heatmap — algorithm × length bucket.
    // Z = fraction of correctly-placed primaries. One row per algo,
    // one column per length bucket. Quick visual answer to "which
    // (algorithm, length) cells regress?".
    // ================================================================
    emit_chart("accuracy_heatmap_algorithm_vs_length",
        "Position accuracy heatmap (algorithm × length bucket)",
        "Heatmap whose rows are algorithm configurations and columns "
        "are length buckets; cell colour encodes the fraction of "
        "correctly-placed primaries. White / yellow cells highlight "
        "(algorithm, length) combinations that need attention. The "
        "heatmap collapses the full per-pair detail into a single grid "
        "so reviewer attention can be drawn to outlier cells before "
        "drilling into per-pair charts.",
        [&]() {
            std::vector<std::string> col_labels = {
                sequence_length_bucket_name(SequenceLengthBucket::SHORT),
                sequence_length_bucket_name(SequenceLengthBucket::MEDIUM),
                sequence_length_bucket_name(SequenceLengthBucket::LONG),
                sequence_length_bucket_name(SequenceLengthBucket::VERY_LONG),
                sequence_length_bucket_name(SequenceLengthBucket::XL),
                sequence_length_bucket_name(SequenceLengthBucket::XXL),
            };
            // z[row][col] — outer rows = algorithms, inner = buckets.
            std::vector<std::vector<double>> z;
            std::vector<std::string> row_labels;
            for (const auto& algorithm_label : algorithm_labels) {
                std::array<std::pair<std::size_t, std::size_t>, 6> per_bucket{};
                for (const auto& s : samples) {
                    if (s.algorithm_label != algorithm_label) continue;
                    const auto bidx = static_cast<std::size_t>(s.length_bucket);
                    if (bidx >= per_bucket.size()) continue;
                    auto& counts = per_bucket[bidx];
                    counts.second++;
                    if (s.position_correct) counts.first++;
                }
                std::vector<double> row;
                bool any = false;
                for (const auto& [correct, total] : per_bucket) {
                    if (total > 0) any = true;
                    row.push_back(total > 0
                        ? static_cast<double>(correct) /
                          static_cast<double>(total)
                        : 0.0);
                }
                if (!any) continue;
                z.push_back(std::move(row));
                row_labels.push_back(algorithm_label);
            }
            writer.key("data");
            writer.begin_array();
            writer.begin_object();
            writer.kv_string("type", "heatmap");
            writer.kv_string("colorscale", "Viridis");
            writer.kv_string_array("x", col_labels);
            writer.kv_string_array("y", row_labels);
            writer.key("z");
            writer.begin_array();
            for (const auto& row : z) {
                writer.begin_array();
                for (double v : row) writer.value_double(v);
                writer.end_array();
            }
            writer.end_array();
            writer.kv_double("zmin", 0.0);
            writer.kv_double("zmax", 1.0);
            writer.key("colorbar");
            writer.begin_object();
            writer.key("title");
            writer.begin_object();
            writer.kv_string("text", "fraction correct");
            writer.end_object();
            writer.end_object();
            writer.end_object();
            writer.end_array();
            emit_layout(
                "Position accuracy by algorithm and length bucket",
                "length bucket", false,
                "algorithm", false);
        });

    // ================================================================
    // Chart 7: P50 vs P99 latency per algorithm (per length bucket).
    // Each algorithm contributes one box per bucket — Plotly box
    // type renders min/Q1/median/Q3/max + outliers. Reads as "show me
    // the latency distribution shape for each (algorithm, bucket)".
    // ================================================================
    emit_chart("latency_distribution_box",
        "Latency distribution per (algorithm, length bucket)",
        "Box-and-whisker plot per (algorithm, length-bucket) cell. The "
        "box renders the 25th-75th percentile range, the whiskers "
        "extend to 1.5× IQR (Plotly default), and the median is the "
        "horizontal line inside the box. Outliers are drawn as "
        "individual points. Tall whiskers / many outlier dots indicate "
        "an algorithm with high tail-latency variance for that length "
        "class. Compare to Chart 1 (mean) and Chart 2 (P99) to "
        "triangulate where the P99 - mean gap comes from.",
        [&]() {
            writer.key("data");
            writer.begin_array();
            for (const auto& algorithm_label : algorithm_labels) {
                std::map<int, std::vector<double>> bucket_to_latencies;
                for (const auto& s : samples) {
                    if (s.algorithm_label != algorithm_label) continue;
                    for (double lat_ns : s.sample_latencies_ns) {
                        bucket_to_latencies[static_cast<int>(s.length_bucket)]
                            .push_back(lat_ns / 1e6);
                    }
                }
                if (bucket_to_latencies.empty()) continue;
                std::vector<std::string> xs;
                std::vector<double> ys;
                for (const auto& [bucket, latencies] : bucket_to_latencies) {
                    const auto label = std::string{sequence_length_bucket_name(
                        static_cast<SequenceLengthBucket>(bucket))};
                    for (double lat : latencies) {
                        xs.push_back(label);
                        ys.push_back(lat);
                    }
                }
                writer.begin_object();
                writer.kv_string("type", "box");
                writer.kv_string("name", algorithm_label);
                writer.kv_string_array("x", xs);
                writer.kv_double_array("y", ys);
                writer.key("marker");
                writer.begin_object();
                writer.kv_string("color", detail::colour_for_key(algorithm_label));
                writer.end_object();
                writer.end_object();
            }
            writer.end_array();
            emit_layout(
                "Per-sample latency distribution (box plot)",
                "length bucket", false,
                "latency per call (ms)", true,
                "group");
        });

    // ================================================================
    // Chart 8: throughput Mbp/s vs sequence length.
    // Reciprocal of the mean-latency chart, normalised by query bp.
    // Higher = faster. Useful when comparing across length classes
    // (latency curves bend with input size, throughput curves do not).
    // ================================================================
    emit_chart("seq_length_vs_throughput",
        "Throughput (Mbp/s) vs. effective query length",
        "Aligner throughput, in megabases-per-second, derived as "
        "effective_query_length / mean_latency. Throughput-normalised "
        "view of Chart 1: a flat horizontal trace means the algorithm's "
        "per-base cost is constant across length classes; a sloping "
        "trace means the per-base cost changes (e.g. cache effects on "
        "long reads, or super-linear chaining). Use this view when "
        "comparing pipelines whose absolute latency differs by orders "
        "of magnitude — the units cancel out the length dependence.",
        [&]() {
            writer.key("data");
            writer.begin_array();
            for (const auto& algorithm_label : algorithm_labels) {
                std::vector<double> xs, ys;
                for (const auto& s : samples) {
                    if (s.algorithm_label != algorithm_label) continue;
                    if (s.sample_latencies_ns.empty()) continue;
                    const double mean_ns = detail::mean_of(s.sample_latencies_ns);
                    if (mean_ns <= 0.0) continue;
                    xs.push_back(static_cast<double>(s.effective_query_length));
                    // bp / ns == Gbp/s; convert to Mbp/s.
                    ys.push_back(static_cast<double>(s.effective_query_length) /
                                 mean_ns * 1000.0);
                }
                if (xs.empty()) continue;
                writer.begin_object();
                writer.kv_string("type", "scatter");
                writer.kv_string("mode", "markers");
                writer.kv_string("name", algorithm_label);
                writer.kv_double_array("x", xs);
                writer.kv_double_array("y", ys);
                emit_marker(algorithm_label);
                writer.end_object();
            }
            writer.end_array();
            emit_layout(
                "Throughput (Mbp/s) vs. effective query length",
                "effective query length (bp)", true,
                "throughput (Mbp/s)", true);
        });

    // ================================================================
    // Chart 9: P99 / mean tail-amplification ratio per algorithm.
    // For each algorithm and length bucket we plot P99/mean — a value
    // of 1.0 means the distribution is tight, > 5 means many outliers.
    // ================================================================
    emit_chart("p99_over_mean_tail_amplification",
        "Tail-amplification (P99 / mean) by algorithm and length bucket",
        "P99 / mean ratio per algorithm and length bucket. Values "
        "close to 1 indicate tight latency distributions; values "
        "above 3-5 flag aligners that are dominated by a small fraction "
        "of slow inputs (relevant when sizing latency budgets for "
        "interactive workloads). The chart is rendered as one trace "
        "per algorithm with the length bucket on the x-axis.",
        [&]() {
            writer.key("data");
            writer.begin_array();
            for (const auto& algorithm_label : algorithm_labels) {
                std::map<int, std::vector<double>> bucket_p99s;
                std::map<int, std::vector<double>> bucket_means;
                for (const auto& s : samples) {
                    if (s.algorithm_label != algorithm_label) continue;
                    if (s.sample_latencies_ns.empty()) continue;
                    bucket_p99s[static_cast<int>(s.length_bucket)].push_back(
                        detail::percentile_sorted(s.sample_latencies_ns, 0.99));
                    bucket_means[static_cast<int>(s.length_bucket)].push_back(
                        detail::mean_of(s.sample_latencies_ns));
                }
                if (bucket_p99s.empty()) continue;
                std::vector<std::string> xs;
                std::vector<double> ys;
                for (const auto& [bucket, p99s] : bucket_p99s) {
                    const double agg_p99 = detail::mean_of(p99s);
                    const double agg_mean = detail::mean_of(bucket_means[bucket]);
                    const double ratio = agg_mean > 0.0 ? agg_p99 / agg_mean : 0.0;
                    xs.push_back(std::string{sequence_length_bucket_name(
                        static_cast<SequenceLengthBucket>(bucket))});
                    ys.push_back(ratio);
                }
                writer.begin_object();
                writer.kv_string("type", "scatter");
                writer.kv_string("mode", "lines+markers");
                writer.kv_string("name", algorithm_label);
                writer.kv_string_array("x", xs);
                writer.kv_double_array("y", ys);
                emit_marker(algorithm_label);
                writer.end_object();
            }
            writer.end_array();
            emit_layout(
                "Tail-amplification (P99 / mean) by length bucket",
                "length bucket", false,
                "P99 / mean ratio", false);
        });

    // ================================================================
    // Chart 10: position-offset histogram per algorithm.
    // Pools all per-sample offsets (bp) — a sharp peak at 0 = aligner
    // lands on the truth; long tails = misplacement. Rendered as one
    // overlaid histogram per algorithm with semi-transparent bars.
    // ================================================================
    emit_chart("position_offset_histogram",
        "Position offset histogram (aligned - truth, bp)",
        "Distribution of signed position offsets (aligner_start - "
        "truth_start, in bp) across every benchmark sample, broken "
        "down by algorithm. A sharp spike at 0 with thin tails = the "
        "aligner places primaries on or next to the simulated truth. "
        "Wide skirts = drift; bimodal distributions = the aligner "
        "frequently locks onto an alternate repeat. Bins are clipped "
        "to ±100 bp; offsets beyond that fold into the edge bins.",
        [&]() {
            writer.key("data");
            writer.begin_array();
            for (const auto& algorithm_label : algorithm_labels) {
                std::vector<double> offsets;
                for (const auto& s : samples) {
                    if (s.algorithm_label != algorithm_label) continue;
                    if (!s.produced_primary) continue;
                    const double clamped = static_cast<double>(
                        std::clamp(s.position_offset_bp, -100, 100));
                    offsets.push_back(clamped);
                }
                if (offsets.empty()) continue;
                writer.begin_object();
                writer.kv_string("type", "histogram");
                writer.kv_string("name", algorithm_label);
                writer.kv_string("histnorm", "probability");
                writer.kv_double_array("x", offsets);
                writer.kv_double("opacity", 0.45);
                writer.key("xbins");
                writer.begin_object();
                writer.kv_int("start", -100);
                writer.kv_int("end", 100);
                writer.kv_int("size", 5);
                writer.end_object();
                writer.key("marker");
                writer.begin_object();
                writer.kv_string("color", detail::colour_for_key(algorithm_label));
                writer.end_object();
                writer.end_object();
            }
            writer.end_array();
            emit_layout(
                "Position offset histogram (probability density)",
                "aligner_start - truth_start (bp, clipped to ±100)", false,
                "probability density", false,
                "overlay");
        });

    // ================================================================
    // Chart 11: paired speedup vs minimap2 reference (DP variant).
    //
    // The naive averaging-of-means version was statistically weak —
    // bioseq variants and minimap2 baseline run on the SAME
    // (length, error, seed) cells, so the right comparison is a
    // *paired* per-cell ratio: mm2_DP_mean / variant_mean per cell,
    // then mean of those ratios. This is robust to slow-pair outliers
    // that dominate one operand of the unpaired ratio.
    // ================================================================
    emit_chart("speedup_vs_minimap2_reference_dp",
        "Paired speedup vs. minimap2 (DP chaining) per length bucket",
        "Per-bucket paired speedup ratio vs. the minimap2 (DP) "
        "baseline. For each (length_bucket, error_bucket) cell we "
        "compute the cell's mean of `mm2_DP_mean_ns / variant_mean_ns` "
        "(equivalently: per-pair speedup, averaged over pairs). Bars "
        "above 1 = bioseq faster than minimap2; bars below 1 = "
        "slower. Pairing makes the comparison robust to one slow "
        "outlier dominating either operand. The two minimap2 traces "
        "are excluded (DP would always be at 1.0 by construction; "
        "RMQ is a different reference).",
        [&]() {
            // Build a (cell_key -> mean_ns) map for the baseline so
            // each variant cell can look up its paired baseline mean.
            using cell_key = std::pair<int, int>;  // (length_bucket, error_bucket)
            std::map<cell_key, std::vector<double>> baseline_cell_means;
            for (const auto& s : samples) {
                if (s.algorithm_label != "minimap2 (DP chaining)") continue;
                if (s.sample_latencies_ns.empty()) continue;
                baseline_cell_means[{static_cast<int>(s.length_bucket),
                                      static_cast<int>(s.error_bucket)}]
                    .push_back(detail::mean_of(s.sample_latencies_ns));
            }
            std::map<cell_key, double> baseline_cell_avg;
            for (const auto& [k, v] : baseline_cell_means) {
                baseline_cell_avg[k] = v.empty() ? 0.0 : detail::mean_of(v);
            }

            writer.key("data");
            writer.begin_array();
            for (const auto& algorithm_label : algorithm_labels) {
                if (algorithm_label == "minimap2 (DP chaining)" ||
                    algorithm_label == "minimap2 (RMQ chaining)") continue;
                // Per (length_bucket): collect the per-cell paired ratios
                // and emit their mean as the bar height.
                std::array<std::vector<double>, 5> per_length_ratios;
                for (const auto& s : samples) {
                    if (s.algorithm_label != algorithm_label) continue;
                    if (s.sample_latencies_ns.empty()) continue;
                    const auto bidx = static_cast<std::size_t>(s.length_bucket);
                    if (bidx >= per_length_ratios.size()) continue;
                    const cell_key key{static_cast<int>(s.length_bucket),
                                       static_cast<int>(s.error_bucket)};
                    auto baseline_it = baseline_cell_avg.find(key);
                    if (baseline_it == baseline_cell_avg.end() ||
                        baseline_it->second <= 0.0) continue;
                    const double variant_mean = detail::mean_of(s.sample_latencies_ns);
                    if (variant_mean <= 0.0) continue;
                    per_length_ratios[bidx].push_back(baseline_it->second / variant_mean);
                }
                std::vector<std::string> xs;
                std::vector<double> ys;
                for (std::size_t i = 0; i < per_length_ratios.size(); ++i) {
                    if (per_length_ratios[i].empty()) continue;
                    xs.push_back(detail::length_bucket_short_label(
                        static_cast<SequenceLengthBucket>(i)));
                    ys.push_back(detail::mean_of(per_length_ratios[i]));
                }
                if (xs.empty()) continue;
                writer.begin_object();
                writer.kv_string("type", "bar");
                writer.kv_string("name", algorithm_label);
                writer.kv_string_array("x", xs);
                writer.kv_double_array("y", ys);
                writer.key("marker");
                writer.begin_object();
                writer.kv_string("color", detail::colour_for_key(algorithm_label));
                writer.end_object();
                writer.end_object();
            }
            writer.end_array();
            emit_layout(
                "Paired speedup ratio vs. minimap2 (DP chaining)",
                "length bucket", false,
                "mean(paired baseline / variant)", false,
                "group");
        });

    // ================================================================
    // Chart 12: MAPQ distribution per algorithm.
    // X = MAPQ score (0-60), Y = count. Indirect quality signal: a
    // healthy aligner produces a sharp peak at high MAPQ for low-error
    // pairs and a broad distribution for high-error pairs.
    // ================================================================
    emit_chart("mapq_distribution",
        "Primary MAPQ distribution per algorithm",
        "Histogram of primary mapping qualities (MAPQ, 0-60) per "
        "algorithm. Aligners that bin most primaries at MAPQ=60 (or "
        "near it) have high confidence in their placements; aligners "
        "with mass spread across the lower buckets are uncertain about "
        "where reads belong (often the right call when the input is "
        "ambiguous). MAPQ is a Phred-scaled score — 60 corresponds to "
        "1-in-1,000,000 expected misplacement.",
        [&]() {
            writer.key("data");
            writer.begin_array();
            for (const auto& algorithm_label : algorithm_labels) {
                std::vector<double> mapqs;
                for (const auto& s : samples) {
                    if (s.algorithm_label != algorithm_label) continue;
                    if (!s.produced_primary) continue;
                    mapqs.push_back(static_cast<double>(s.primary_mapq));
                }
                if (mapqs.empty()) continue;
                writer.begin_object();
                writer.kv_string("type", "histogram");
                writer.kv_string("name", algorithm_label);
                writer.kv_string("histnorm", "probability");
                writer.kv_double_array("x", mapqs);
                writer.kv_double("opacity", 0.45);
                writer.key("xbins");
                writer.begin_object();
                writer.kv_int("start", 0);
                writer.kv_int("end", 65);
                writer.kv_int("size", 5);
                writer.end_object();
                writer.key("marker");
                writer.begin_object();
                writer.kv_string("color", detail::colour_for_key(algorithm_label));
                writer.end_object();
                writer.end_object();
            }
            writer.end_array();
            emit_layout(
                "Primary MAPQ distribution",
                "MAPQ score (0-60)", false,
                "probability density", false,
                "overlay");
        });

    // ================================================================
    // Chart 13: latency CDF per algorithm.
    //
    // Strictly more informative than P99 alone. For each algorithm we
    // pool every per-sample latency (across all pairs/buckets), sort,
    // and emit (sorted_latency, ECDF_percentile) as a line trace. The
    // reader can read off P50, P90, P99, P99.9 visually, and trace
    // crossings tell you which algorithm wins at which percentile.
    // ================================================================
    emit_chart("latency_cdf_per_algorithm",
        "Empirical CDF of per-sample latency per algorithm",
        "Empirical cumulative distribution function of per-sample "
        "latency, with all samples pooled across length buckets and "
        "error rates. X axis is the sorted latency in ms (log scale); "
        "Y axis is the cumulative probability. Read horizontally to "
        "find any percentile (P50 = where the trace crosses Y=0.5, "
        "P99 at Y=0.99, etc.). Two traces crossing means one "
        "algorithm wins at low percentiles but the other wins at the "
        "tail — common pattern when one variant has lower mean but "
        "longer tail. Strictly more informative than the single-"
        "number P99 charts.",
        [&]() {
            writer.key("data");
            writer.begin_array();
            for (const auto& algorithm_label : algorithm_labels) {
                std::vector<double> latencies;
                for (const auto& s : samples) {
                    if (s.algorithm_label != algorithm_label) continue;
                    for (double lat_ns : s.sample_latencies_ns) {
                        latencies.push_back(lat_ns / 1e6);
                    }
                }
                if (latencies.empty()) continue;
                std::sort(latencies.begin(), latencies.end());
                std::vector<double> ys;
                ys.reserve(latencies.size());
                const double n = static_cast<double>(latencies.size());
                for (std::size_t i = 0; i < latencies.size(); ++i) {
                    ys.push_back(static_cast<double>(i + 1) / n);
                }
                writer.begin_object();
                writer.kv_string("type", "scatter");
                writer.kv_string("mode", "lines");
                writer.kv_string("name", algorithm_label);
                writer.kv_double_array("x", latencies);
                writer.kv_double_array("y", ys);
                writer.key("line");
                writer.begin_object();
                writer.kv_string("color", detail::colour_for_key(algorithm_label));
                writer.kv_int("width", 2);
                writer.end_object();
                writer.end_object();
            }
            writer.end_array();
            emit_layout(
                "Per-algorithm latency CDF",
                "latency (ms, log)", true,
                "cumulative probability", false);
        });

    // ================================================================
    // Chart 14: Pareto frontier — accuracy vs. mean latency.
    //
    // One marker per (algorithm, length_bucket). X axis = mean latency
    // (lower is better), Y axis = position-accuracy fraction (higher
    // is better). Pareto-optimal points (no other point is strictly
    // better in both dimensions) are the candidates for "no
    // free lunch": dominated points are strictly worse choices.
    // ================================================================
    emit_chart("pareto_accuracy_vs_latency",
        "Pareto frontier — accuracy vs. mean latency",
        "Per (algorithm, length-bucket) marker: X is mean latency in "
        "ms (lower is better), Y is position-accuracy fraction "
        "(higher is better). The 'Pareto frontier' is the set of "
        "points where no other configuration is strictly better in "
        "both dimensions — those are the live trade-offs. "
        "Configurations strictly dominated by another (worse on both "
        "axes) are bad choices regardless of the operator's "
        "accuracy/latency preference. Marker symbol encodes the "
        "length bucket so the reader can visually group by input class.",
        [&]() {
            writer.key("data");
            writer.begin_array();
            const std::array<std::string_view, 5> length_symbols = {
                "circle", "square", "diamond", "triangle-up", "star"};
            for (const auto& algorithm_label : algorithm_labels) {
                std::array<std::pair<std::vector<double>, std::vector<double>>, 5> per_bucket;
                std::array<std::pair<std::size_t, std::size_t>, 6> correctness;
                for (const auto& s : samples) {
                    if (s.algorithm_label != algorithm_label) continue;
                    if (s.sample_latencies_ns.empty()) continue;
                    const auto bidx = static_cast<std::size_t>(s.length_bucket);
                    if (bidx >= per_bucket.size()) continue;
                    per_bucket[bidx].first.push_back(detail::mean_of(s.sample_latencies_ns) / 1e6);
                    correctness[bidx].second++;
                    if (s.position_correct) correctness[bidx].first++;
                }
                for (std::size_t i = 0; i < per_bucket.size(); ++i) {
                    if (per_bucket[i].first.empty()) continue;
                    const double mean_latency_ms = detail::mean_of(per_bucket[i].first);
                    const double accuracy = correctness[i].second > 0
                        ? static_cast<double>(correctness[i].first) /
                          static_cast<double>(correctness[i].second)
                        : 0.0;
                    writer.begin_object();
                    writer.kv_string("type", "scatter");
                    writer.kv_string("mode", "markers");
                    writer.kv_string("name",
                        algorithm_label + " — " +
                        std::string{detail::length_bucket_short_label(
                            static_cast<SequenceLengthBucket>(i))});
                    writer.kv_string("legendgroup", algorithm_label);
                    writer.key("x");
                    writer.begin_array(); writer.value_double(mean_latency_ms); writer.end_array();
                    writer.key("y");
                    writer.begin_array(); writer.value_double(accuracy); writer.end_array();
                    writer.key("marker");
                    writer.begin_object();
                    writer.kv_string("color", detail::colour_for_key(algorithm_label));
                    writer.kv_int("size", 11);
                    writer.kv_string("symbol", length_symbols[i]);
                    writer.end_object();
                    writer.end_object();
                }
            }
            writer.end_array();
            emit_layout(
                "Accuracy vs. mean latency (Pareto view)",
                "mean latency per call (ms, log)", true,
                "position-accuracy fraction", false);
        });

    // ================================================================
    // Chart 15: accuracy heatmap broken out by error rate.
    //
    // The original heatmap pools across LOW/MED/HIGH error buckets so
    // an algorithm that's perfect at LOW and 0% at HIGH averages to
    // 50% — the failure is invisible. This second heatmap rows-by-
    // algorithm, columns-by-(length × error) so the cell granularity
    // matches the matrix actually executed.
    //
    // Now sized 4 lengths × 6 error buckets = 24 cells — sparse cells
    // (e.g. ONT_LEGACY × VERY_LONG, skipped by `should_skip_cell`)
    // render as 0 with `total == 0`; the colour-scale floor at 0 means
    // those skipped combinations show up as the same dark colour as
    // genuine 0% accuracy cells. To distinguish a "we never ran it"
    // cell from a "we ran but the aligner failed" one, look at the
    // hover tooltip — `customdata` carries the per-cell sample count.
    // ================================================================
    emit_chart("accuracy_heatmap_algorithm_vs_length_error",
        "Accuracy heatmap (algorithm × (length, error))",
        "Heatmap rows are algorithm configurations; columns are the "
        "(length-bucket, error-rate) cells in the sweep matrix. "
        "Cell colour encodes the position-accuracy fraction in that "
        "cell. Unlike the algorithm × length-only heatmap, this view "
        "preserves the error-rate axis — a configuration that's "
        "perfect on HIFI-error but collapses on ONT_LEGACY-error "
        "appears as two distinct columns rather than averaging to "
        "50%. Cells the runner skipped (ONT_LEGACY/HIGH_INDEL × LONG, "
        "RMQ × VERY_LONG, etc.) show up as 0 — hover for the "
        "per-cell sample count to disambiguate.",
        [&]() {
            constexpr std::array<SequenceLengthBucket, 6> all_lengths = {
                SequenceLengthBucket::SHORT,
                SequenceLengthBucket::MEDIUM,
                SequenceLengthBucket::LONG,
                SequenceLengthBucket::VERY_LONG,
                SequenceLengthBucket::XL,
                SequenceLengthBucket::XXL,
            };
            constexpr std::array<ErrorRateBucket, 6> all_errors = {
                ErrorRateBucket::HIFI,
                ErrorRateBucket::CCS,
                ErrorRateBucket::ONT_MODERN,
                ErrorRateBucket::ONT_LEGACY,
                ErrorRateBucket::HIGH_INDEL_LOW_SUB,
                ErrorRateBucket::HIGH_SUB_LOW_INDEL,
            };
            constexpr std::size_t kCellCount = all_lengths.size() * all_errors.size();
            std::vector<std::string> col_labels;
            std::array<std::pair<int, int>, kCellCount> col_keys;
            std::size_t idx = 0;
            for (auto length_bucket : all_lengths) {
                for (auto error_bucket : all_errors) {
                    col_keys[idx] = {static_cast<int>(length_bucket),
                                     static_cast<int>(error_bucket)};
                    col_labels.push_back(
                        std::string{detail::length_bucket_short_label(length_bucket)} +
                        "/" + error_rate_bucket_short_label(error_bucket));
                    idx++;
                }
            }
            std::vector<std::vector<double>> z;
            std::vector<std::string> row_labels;
            for (const auto& algorithm_label : algorithm_labels) {
                std::array<std::pair<std::size_t, std::size_t>, kCellCount> per_cell{};
                for (const auto& s : samples) {
                    if (s.algorithm_label != algorithm_label) continue;
                    for (std::size_t k = 0; k < kCellCount; ++k) {
                        if (col_keys[k].first == static_cast<int>(s.length_bucket) &&
                            col_keys[k].second == static_cast<int>(s.error_bucket)) {
                            per_cell[k].second++;
                            if (s.position_correct) per_cell[k].first++;
                            break;
                        }
                    }
                }
                std::vector<double> row;
                bool any = false;
                for (const auto& [correct, total] : per_cell) {
                    if (total > 0) any = true;
                    row.push_back(total > 0
                        ? static_cast<double>(correct) /
                          static_cast<double>(total)
                        : 0.0);
                }
                if (!any) continue;
                z.push_back(std::move(row));
                row_labels.push_back(algorithm_label);
            }
            writer.key("data");
            writer.begin_array();
            writer.begin_object();
            writer.kv_string("type", "heatmap");
            writer.kv_string("colorscale", "Viridis");
            writer.kv_string_array("x", col_labels);
            writer.kv_string_array("y", row_labels);
            writer.key("z");
            writer.begin_array();
            for (const auto& row : z) {
                writer.begin_array();
                for (double v : row) writer.value_double(v);
                writer.end_array();
            }
            writer.end_array();
            writer.kv_double("zmin", 0.0);
            writer.kv_double("zmax", 1.0);
            writer.key("colorbar");
            writer.begin_object();
            writer.key("title");
            writer.begin_object();
            writer.kv_string("text", "fraction correct");
            writer.end_object();
            writer.end_object();
            writer.end_object();
            writer.end_array();
            emit_layout(
                "Position accuracy by algorithm and (length, error) cell",
                "(length, error) cell", false,
                "algorithm", false);
        });

    // ================================================================
    // Chart 16: MAPQ calibration curve.
    //
    // Per algorithm we bin samples by MAPQ (0-9, 10-19, ..., 50-60),
    // then compute fraction correct in each bin. A well-calibrated
    // aligner produces a monotonic curve from low accuracy at low
    // MAPQ to ~1.0 at MAPQ=60. Diagnoses MAPQ inflation / deflation.
    // ================================================================
    emit_chart("mapq_calibration",
        "MAPQ calibration curve (accuracy by MAPQ bin)",
        "For each algorithm we bin primaries into 10-wide MAPQ bins "
        "(0-9, 10-19, ..., 50-60) and report the fraction correctly "
        "placed in each bin. A well-calibrated aligner produces a "
        "monotonic curve climbing from low accuracy at low MAPQ to "
        "~1.0 at MAPQ=60 (since MAPQ is Phred-scaled and 60 means "
        "1-in-1,000,000 expected misplacement). Curves that cluster "
        "at high accuracy across all MAPQs indicate MAPQ deflation "
        "(the aligner is more confident than its scores reflect); "
        "curves with low accuracy at high MAPQ indicate inflation.",
        [&]() {
            writer.key("data");
            writer.begin_array();
            for (const auto& algorithm_label : algorithm_labels) {
                std::array<std::pair<std::size_t, std::size_t>, 7> bins{};  // 0-9..60+
                for (const auto& s : samples) {
                    if (s.algorithm_label != algorithm_label) continue;
                    if (!s.produced_primary) continue;
                    const std::size_t bin_index = std::min<std::size_t>(
                        static_cast<std::size_t>(s.primary_mapq / 10), 6);
                    bins[bin_index].second++;
                    if (s.position_correct) bins[bin_index].first++;
                }
                std::vector<std::string> xs;
                std::vector<double> ys;
                static constexpr std::array<const char*, 7> bin_labels = {
                    "0-9","10-19","20-29","30-39","40-49","50-59","60+"};
                bool any = false;
                for (std::size_t i = 0; i < bins.size(); ++i) {
                    if (bins[i].second == 0) continue;
                    any = true;
                    xs.push_back(bin_labels[i]);
                    ys.push_back(static_cast<double>(bins[i].first) /
                                 static_cast<double>(bins[i].second));
                }
                if (!any) continue;
                writer.begin_object();
                writer.kv_string("type", "scatter");
                writer.kv_string("mode", "lines+markers");
                writer.kv_string("name", algorithm_label);
                writer.kv_string_array("x", xs);
                writer.kv_double_array("y", ys);
                emit_marker(algorithm_label);
                writer.end_object();
            }
            writer.end_array();
            emit_layout(
                "MAPQ calibration — accuracy by MAPQ bin",
                "primary MAPQ bin", false,
                "fraction correctly placed", false);
        });

    // ================================================================
    // Chart 16.5: per-phase pipeline-time breakdown.
    //
    // Stacked bar chart, one chart per length_bucket. X axis lists the
    // bioseq algorithm configurations (the C minimap2 reference rows are
    // intentionally skipped — minimap2's instrumentation aggregates aren't
    // attributed via our `register_region_static` API and would render as
    // a single "minimap2 (DP chaining)" total without phase decomposition).
    // Y axis is the mean wall-clock per timed sample in milliseconds, with
    // each bar stacked by pipeline phase (`mm_sketch`, `target_index_build`,
    // `collect_seed_hits`, `mg_lchain_*`, `mm_align_skeleton`, `mm_set_mapq`,
    // etc.). The visual answers "for THIS algorithm at THIS length, where
    // does the wall-clock time go?" — and is the primary chart to consult
    // when triaging "long sequences are slow" reports. The phase totals
    // come from `BenchSample::phase_mean_ns_per_sample`, which is filled
    // by snapshotting the persistent profiler block before/after the
    // timed sample loop in `matrix_runner.cppm`.
    // ================================================================
    {
        auto emit_phase_breakdown_chart = [&](
            std::string_view chart_id,
            std::string_view long_description_extra,
            SequenceLengthBucket bucket)
        {
            const std::string chart_title = fmt::format(
                "Pipeline phase breakdown — {} sequences",
                sequence_length_bucket_name(bucket));
            const std::string short_desc = fmt::format(
                "Per-pipeline-phase mean ns per timed sample, stacked, for "
                "every bioseq config in the {} length bucket",
                sequence_length_bucket_name(bucket));
            const std::string long_desc = fmt::format(
                "Stacked bar chart with one X-axis category per bioseq "
                "algorithm config (minimap2 reference rows omitted — their "
                "instrumentation aggregates are not attributed by phase). "
                "Each stack segment is a pipeline phase ({}, {}, {}, {}, "
                "{}, ... — taken from the `register_region_static` literals "
                "in the source) and its height is the mean ns per timed "
                "sample for that phase, averaged across every pair in the "
                "{} length bucket. Total bar height ≈ end-to-end mean "
                "latency for that config. Use this chart first when "
                "investigating 'why is bioseq slow on {}-length inputs?': "
                "the dominant segment is the bottleneck. {}",
                "mm_sketch", "target_index_build", "collect_seed_hits",
                "mg_lchain_*", "mm_align_skeleton",
                sequence_length_bucket_name(bucket),
                sequence_length_bucket_name(bucket),
                long_description_extra);

            emit_chart(chart_id, short_desc, long_desc, [&]() {
                // Pivot the samples to (algorithm, phase) → list of mean ns.
                // Algorithms get sorted in the same order as `algorithm_labels`
                // for legend continuity with the rest of the report.
                std::vector<std::string> algos;
                for (const auto& algorithm_label : algorithm_labels) {
                    bool any_for_bucket = false;
                    bool is_minimap2 = false;
                    for (const auto& s : samples) {
                        if (s.algorithm_label != algorithm_label) continue;
                        if (s.length_bucket != bucket) continue;
                        if (!s.phase_mean_ns_per_sample.empty()) {
                            any_for_bucket = true;
                        }
                        if (s.algorithm_kind == "minimap2") {
                            is_minimap2 = true;
                        }
                    }
                    // Skip configs that produced no phase data on this
                    // bucket (e.g. minimap2 reference rows whose C-side
                    // regions don't go through our `register_region_static`
                    // API, or configs that hit the per-call budget skip).
                    if (!any_for_bucket || is_minimap2) continue;
                    algos.push_back(algorithm_label);
                }

                // The aggregate snapshot includes the outer "wrapper" regions
                // (`minimap_port`, `minimap2_port`, `minimap2_port_with_index`)
                // and the "sub-region" decomposition of bigger phases
                // (`target_index_lookup`, `collect_seed_hits_sort`,
                // `mg_lchain_dp_forward`, etc.) — naively stacking all of
                // them double-counts time, because a wrapper's `total_ns`
                // already covers its children. Restrict to a hand-picked
                // set of leaf-level phase names that approximate disjoint
                // time slices of one minimap_port call. The set covers
                // ~all observed wall-clock; phases not listed here either
                // double-count an ancestor or are too small to show as a
                // legible stack segment.
                static const std::vector<std::string_view> kLeafPhases = {
                    // Sketching: scalar (`mm_sketch`) and SIMD (`mm_sketch_simd`)
                    // are mutually exclusive per call — only one fires per
                    // (target/query) sketch — so summing in the chart works.
                    "mm_sketch",
                    "mm_sketch_simd",
                    // Index construction (target side).
                    "target_index_build",
                    // Anchor seeding (covers the per-minimizer hash lookup
                    // loop and the post-loop sort).
                    "collect_seed_hits",
                    // Chaining variants: only one of these fires per call,
                    // determined by `Minimap2Params::chaining_algorithm`.
                    "mg_lchain_dp",
                    "mg_lchain_rmq",
                    "mg_lchain_simd",
                    // Region generation + classification + filtering.
                    "mm_gen_regs",
                    "mm_set_parent",
                    "mm_select_sub",
                    // Divergence estimation (gated on
                    // `Minimap2Params::compute_divergence`; off for some
                    // configs so the segment will be missing).
                    "mm_est_err",
                    // Base-level alignment.
                    "mm_align_skeleton",
                    // MAPQ.
                    "mm_set_mapq",
                };

                // Discover the union of leaf phase names that actually fired
                // on this bucket. Iteration order matches `kLeafPhases` so
                // the stack reads bottom-up in pipeline order (sketch first,
                // then index, then seeding, ..., then mapq).
                std::vector<std::string> phase_names;
                for (const auto& candidate : kLeafPhases) {
                    bool fired = false;
                    for (const auto& algorithm_label : algos) {
                        if (fired) break;
                        for (const auto& s : samples) {
                            if (s.algorithm_label != algorithm_label) continue;
                            if (s.length_bucket != bucket) continue;
                            for (const auto& [name, ns] : s.phase_mean_ns_per_sample) {
                                if (name == candidate) { fired = true; break; }
                            }
                            if (fired) break;
                        }
                    }
                    if (fired) phase_names.emplace_back(candidate);
                }

                writer.key("data");
                writer.begin_array();
                // One trace per phase. Each trace is a bar going across
                // every algorithm on the X axis. Bars stack via
                // `barmode="stack"` in the layout block below.
                for (const auto& phase : phase_names) {
                    std::vector<double> ms_per_algo;
                    ms_per_algo.reserve(algos.size());
                    for (const auto& algorithm_label : algos) {
                        // Mean across pairs (each pair contributed one
                        // mean-ns-per-sample reading via the snapshot in
                        // matrix_runner). Empty pair means "no time"; skip
                        // and treat as zero — the stack just doesn't grow
                        // on that segment for that algo.
                        double sum_ns = 0.0;
                        std::size_t count = 0;
                        for (const auto& s : samples) {
                            if (s.algorithm_label != algorithm_label) continue;
                            if (s.length_bucket != bucket) continue;
                            for (const auto& [name, ns] : s.phase_mean_ns_per_sample) {
                                if (name == phase) {
                                    sum_ns += ns;
                                    ++count;
                                    break;
                                }
                            }
                        }
                        ms_per_algo.push_back(count == 0 ? 0.0
                            : (sum_ns / static_cast<double>(count)) / 1e6);
                    }
                    writer.begin_object();
                    writer.kv_string("type", "bar");
                    writer.kv_string("name", phase);
                    writer.kv_string_array("x", algos);
                    writer.kv_double_array("y", ms_per_algo);
                    writer.end_object();
                }
                writer.end_array();
                emit_layout(
                    chart_title,
                    "algorithm config", false,
                    "mean wall-clock ms per sample (stacked by phase)", false,
                    "stack");
            });
        };
        emit_phase_breakdown_chart("phase_breakdown_short",
            "Short sequences (<1kb) — the alignment phase tends to be "
            "negligible here; bottlenecks usually surface in seed-hit "
            "collection and chaining setup.",
            SequenceLengthBucket::SHORT);
        emit_phase_breakdown_chart("phase_breakdown_medium",
            "Medium sequences (1k-10k) — the working point most "
            "long-read benchmarks publish in. Sketching and chaining "
            "should dominate; if `mm_align_skeleton` is the tallest "
            "segment, the base aligner is a candidate for tuning.",
            SequenceLengthBucket::MEDIUM);
        emit_phase_breakdown_chart("phase_breakdown_long",
            "Long sequences (10k-1M) — `mm_align_skeleton` typically "
            "moves into the dominant slot. Compare KSW2 vs Block "
            "aligner stacks to see whether the align phase or the "
            "chaining phase explains the gap to minimap2.",
            SequenceLengthBucket::LONG);
        emit_phase_breakdown_chart("phase_breakdown_very_long",
            "Very long sequences (1M-3M) — for assembly-to-assembly "
            "pairs. Even the cheapest phase blows up here; the "
            "stacked breakdown reveals whether the slowdown is the "
            "alignment, the chain DP, or the per-sketch hashing "
            "amortising poorly across millions of bases.",
            SequenceLengthBucket::VERY_LONG);
    }

    // ================================================================
    // Largest-bucket head-to-head charts.
    //
    // Compare bioseq vs minimap2 STAGE-BY-STAGE on the largest sequence
    // bucket the bench produced data for (typically VERY_LONG, with XL
    // taking precedence when present). Per the user's V4 task: we want
    // the same blocks visible on both sides so the reader can spot the
    // bottleneck at a glance — wall-clock per stage *plus* PMU counter
    // data (instructions, branches, branch-misses, cycles) — for the
    // fastest bioseq config and minimap2.
    //
    // The "logical stage" abstraction joins phase names from each
    // algorithm's source (e.g. bioseq's `mm_sketch` and `mm_sketch_simd`
    // both contribute to the "Minimizer generation" stage; minimap2's
    // `tool_minimap2.mm_sketch` and `tool_minimap2.base_collect_minimizers`
    // both contribute to the same stage). This lets the
    // bar-and-table view show "the chaining stage takes X ms here, Y ms
    // there" without requiring perfect 1:1 phase-name parity, which the
    // two pipelines do not have (e.g. minimap2 runs `mg_lchain_dp` only,
    // bioseq runs one of {mg_lchain_dp, mg_lchain_simd, mg_lchain_rmq}
    // depending on config; both should fold into "Chaining").
    // ================================================================
    {
        // ---- Stage definition: name + the phase aliases that count
        //      toward it on each side. Both sides may contribute zero
        //      or more phases; the cell value is the sum across all
        //      matching phases.
        struct LogicalStage {
            std::string_view              display_name;
            std::vector<std::string_view> phase_aliases;
        };
        const std::vector<LogicalStage> kLogicalStages = {
            {"Minimizer generation", {
                "mm_sketch", "mm_sketch_simd",
                "tool_minimap2.mm_sketch",
                "tool_minimap2.base_collect_minimizers"}},
            {"Target index build", {
                "target_index_build",
                "tool_minimap2.target_index_build"}},
            {"Seed collection", {
                "collect_seed_hits",
                "tool_minimap2.collect_seed_hits"}},
            {"Chaining", {
                "mg_lchain_dp", "mg_lchain_simd", "mg_lchain_rmq",
                "tool_minimap2.mg_lchain_dp",
                "tool_minimap2.mg_lchain_rmq"}},
            {"Region generation/filtering", {
                "mm_gen_regs", "mm_set_parent", "mm_select_sub",
                "tool_minimap2.mm_gen_regs",
                "tool_minimap2.mm_set_parent",
                "tool_minimap2.mm_select_sub"}},
            {"Divergence estimation", {
                "mm_est_err",
                "tool_minimap2.mm_est_err"}},
            {"Alignment", {
                "mm_align_skeleton",
                "tool_minimap2.mm_align_skeleton"}},
            {"MAPQ", {
                "mm_set_mapq",
                "tool_minimap2.mm_set_mapq"}},
        };

        // Pick the largest length bucket present in the samples. XXL
        // wins over XL which wins over VERY_LONG, etc. The chart layer
        // rebuilds the chart with the same data when no samples
        // exist (the bar arrays end up empty and Plotly renders a
        // blank chart with the title — the legend still tells the
        // reader what was *meant* to be shown).
        auto pick_largest_bucket = [&]() -> SequenceLengthBucket {
            std::array<bool, 6> have = {false, false, false, false, false, false};
            for (const auto& s : samples) {
                have[static_cast<int>(s.length_bucket)] = true;
            }
            if (have[static_cast<int>(SequenceLengthBucket::XXL)])       return SequenceLengthBucket::XXL;
            if (have[static_cast<int>(SequenceLengthBucket::XL)])        return SequenceLengthBucket::XL;
            if (have[static_cast<int>(SequenceLengthBucket::VERY_LONG)]) return SequenceLengthBucket::VERY_LONG;
            if (have[static_cast<int>(SequenceLengthBucket::LONG)])      return SequenceLengthBucket::LONG;
            if (have[static_cast<int>(SequenceLengthBucket::MEDIUM)])    return SequenceLengthBucket::MEDIUM;
            return SequenceLengthBucket::SHORT;
        };
        const SequenceLengthBucket largest_bucket = pick_largest_bucket();

        // Mean phase data (timing + counters) per (algorithm, phase).
        // Aggregate per algorithm by averaging across all pairs in the
        // bucket. Phases not seen on this (algo, bucket) cell are
        // simply not added — we treat them as 0 for sum purposes,
        // exactly as the existing phase-breakdown chart does.
        struct PhaseStats {
            double mean_ns          = 0.0;
            double mean_cycles      = 0.0;
            double mean_instructions = 0.0;
            double mean_branches    = 0.0;
            double mean_branch_misses = 0.0;
        };
        // Indexed by [algorithm_label][phase_name]
        std::map<std::string, std::map<std::string, PhaseStats>> per_algo_phase;
        for (const auto& s : samples) {
            if (s.length_bucket != largest_bucket) continue;
            // For each phase, accumulate by averaging across pairs.
            // Build a lookup by name from phase_counters_per_sample so
            // we can pair timing + counters in a single pass.
            std::unordered_map<std::string, const BenchSample::PhaseCounters*> ctr_by_name;
            for (const auto& pc : s.phase_counters_per_sample) {
                ctr_by_name[pc.name] = &pc;
            }
            for (const auto& [name, ns] : s.phase_mean_ns_per_sample) {
                auto& acc = per_algo_phase[s.algorithm_label][name];
                acc.mean_ns += ns;
                if (auto it = ctr_by_name.find(name); it != ctr_by_name.end()) {
                    acc.mean_cycles        += it->second->mean_cycles_per_sample;
                    acc.mean_instructions  += it->second->mean_instructions_per_sample;
                    acc.mean_branches      += it->second->mean_branches_per_sample;
                    acc.mean_branch_misses += it->second->mean_branch_misses_per_sample;
                }
            }
        }
        // Divide by sample count per (algo, phase) to get the mean.
        // Track sample count separately because phases that didn't fire
        // for a given pair shouldn't drag the mean to zero — only
        // sample-count *of pairs that emitted this phase at least once*
        // is the correct denominator, mirroring the phase_breakdown
        // chart's behaviour.
        std::map<std::string, std::map<std::string, std::size_t>> per_algo_phase_count;
        for (const auto& s : samples) {
            if (s.length_bucket != largest_bucket) continue;
            for (const auto& [name, _ns] : s.phase_mean_ns_per_sample) {
                per_algo_phase_count[s.algorithm_label][name] += 1;
            }
        }
        for (auto& [algo, phases] : per_algo_phase) {
            auto& counts = per_algo_phase_count[algo];
            for (auto& [phase, st] : phases) {
                const std::size_t n = counts[phase];
                if (n == 0) continue;
                st.mean_ns          /= static_cast<double>(n);
                st.mean_cycles      /= static_cast<double>(n);
                st.mean_instructions /= static_cast<double>(n);
                st.mean_branches    /= static_cast<double>(n);
                st.mean_branch_misses /= static_cast<double>(n);
            }
        }

        // Reduce to logical stages: sum the matching phases per stage.
        // `stage_value(algo, stage, metric)` returns the mean of the
        // chosen metric across all phases in that stage.
        auto stage_value = [&](const std::string& algo,
                               const LogicalStage& stage,
                               int metric_id) -> double {
            const auto algo_it = per_algo_phase.find(algo);
            if (algo_it == per_algo_phase.end()) return 0.0;
            double total = 0.0;
            for (const auto& alias : stage.phase_aliases) {
                const auto it = algo_it->second.find(std::string{alias});
                if (it == algo_it->second.end()) continue;
                switch (metric_id) {
                    case 0: total += it->second.mean_ns;          break;  // ns
                    case 1: total += it->second.mean_cycles;      break;
                    case 2: total += it->second.mean_instructions; break;
                    case 3: total += it->second.mean_branches;    break;
                    case 4: total += it->second.mean_branch_misses; break;
                }
            }
            return total;
        };

        // Choose the algorithms to display in the head-to-head charts.
        // 1. minimap2 (DP chaining) is always the baseline.
        // 2. Then every bioseq config that produced phase data on this
        //    bucket — sorted in the same order as `algorithm_labels`
        //    so the chart legend matches the rest of the report.
        const std::string mm2_dp_label = "minimap2 (DP chaining)";
        std::vector<std::string> head_to_head_algos;
        if (per_algo_phase.contains(mm2_dp_label)) {
            head_to_head_algos.push_back(mm2_dp_label);
        }
        for (const auto& algo : algorithm_labels) {
            if (algo == mm2_dp_label) continue;
            // Skip the second minimap2 reference; the bar chart is
            // already cluttered with all bioseq variants and the user's
            // primary baseline is `minimap2 (DP chaining)`. The "RMQ"
            // variant is still in the table further down.
            if (!per_algo_phase.contains(algo)) continue;
            head_to_head_algos.push_back(algo);
        }

        // Determine which bioseq configs to highlight in the bar chart.
        // The production minimizer (the dual-stream two-stack nthash
        // collector) is exposed under
        // `bioseq KSW2 + SIMD-DP + two-stack-nthash-mm`. We emit:
        //   1. minimap2 (DP chaining) — the canonical baseline
        //   2. The "fastest bioseq" (lowest total wall-clock among
        //      bioseq variants in the report). This is the practical
        //      "best we have today" for the largest bucket.
        //   3. The production target
        //      (`bioseq KSW2 + SIMD-DP + two-stack-nthash-mm`) whenever
        //      it's in the report. May coincide with #2; in that case
        //      the bar chart de-dups so #3 is suppressed.
        auto total_ns_for_algo = [&](const std::string& algo) {
            double total = 0.0;
            for (const auto& stage : kLogicalStages) {
                total += stage_value(algo, stage, 0);
            }
            return total;
        };
        std::string fastest_bioseq_label;
        {
            double fastest_bioseq_total_ns = std::numeric_limits<double>::infinity();
            for (const auto& algo : head_to_head_algos) {
                if (algo.find("minimap2") != std::string::npos) continue;
                const double t = total_ns_for_algo(algo);
                if (t > 0.0 && t < fastest_bioseq_total_ns) {
                    fastest_bioseq_total_ns = t;
                    fastest_bioseq_label = algo;
                }
            }
        }
        const std::string two_stack_label = "bioseq KSW2 + SIMD-DP + two-stack-nthash-mm";
        const bool production_present = per_algo_phase.contains(two_stack_label);

        // ---- Chart: per-stage time bar chart (bioseq fastest vs minimap2 DP)
        const std::string headline_chart_id =
            "headline_largest_per_stage_time";
        const std::string headline_title = fmt::format(
            "Per-stage wall-clock — fastest bioseq vs minimap2 ({} bucket)",
            sequence_length_bucket_name(largest_bucket));
        emit_chart(headline_chart_id,
            "Headline: per-stage wall-clock comparison, fastest bioseq vs minimap2 on largest sequences",
            "Two grouped bars per logical stage (Minimizer generation, "
            "Chaining, Alignment, ...) — left bar is minimap2 (DP "
            "chaining), right bar is the fastest bioseq config (lowest "
            "summed wall-clock across stages). Y is mean wall-clock ms "
            "per call on the largest sequence bucket the bench ran. Use "
            "this chart to spot which stage is dragging bioseq behind "
            "minimap2 (or where bioseq has already overtaken).",
            [&]() {
                std::vector<std::string> stage_names;
                stage_names.reserve(kLogicalStages.size());
                for (const auto& stage : kLogicalStages) {
                    stage_names.emplace_back(stage.display_name);
                }
                auto algo_stage_ms = [&](const std::string& algo) {
                    std::vector<double> out;
                    out.reserve(kLogicalStages.size());
                    for (const auto& stage : kLogicalStages) {
                        out.push_back(stage_value(algo, stage, 0) / 1e6);
                    }
                    return out;
                };
                writer.key("data");
                writer.begin_array();
                // Bar 1: minimap2 baseline.
                writer.begin_object();
                writer.kv_string("type", "bar");
                writer.kv_string("name", mm2_dp_label);
                writer.kv_string_array("x", stage_names);
                writer.kv_double_array("y", algo_stage_ms(mm2_dp_label));
                writer.end_object();
                // Bar 2: the fastest bioseq variant currently in the
                // report (always emitted when any bioseq config exists).
                if (!fastest_bioseq_label.empty()) {
                    writer.begin_object();
                    writer.kv_string("type", "bar");
                    writer.kv_string("name",
                        fmt::format("fastest bioseq: {}", fastest_bioseq_label));
                    writer.kv_string_array("x", stage_names);
                    writer.kv_double_array("y", algo_stage_ms(fastest_bioseq_label));
                    writer.end_object();
                }
                // Bar 3: the production target (two-stack nthash) —
                // only when present in the report and not already
                // represented as the "fastest bioseq" bar above. Showing
                // the production candidate beside the canonical fastest
                // bar makes it obvious whether the production minimizer
                // is in fact winning across stages.
                if (production_present && fastest_bioseq_label != two_stack_label) {
                    writer.begin_object();
                    writer.kv_string("type", "bar");
                    writer.kv_string("name",
                        fmt::format("production: {}", two_stack_label));
                    writer.kv_string_array("x", stage_names);
                    writer.kv_double_array("y", algo_stage_ms(two_stack_label));
                    writer.end_object();
                }
                writer.end_array();
                emit_layout(headline_title,
                    "logical stage", false,
                    "mean ms / call", false,
                    "group");
            });

        // ---- Chart helper: per-stage counter bar chart for an arbitrary
        //      metric (cycles, instructions, branches, branch_misses).
        //      Identical structure to the timing chart above.
        auto emit_counter_chart = [&](
            std::string_view chart_id,
            std::string_view chart_short_desc,
            std::string_view chart_long_desc,
            std::string_view y_axis_label,
            int metric_id,
            double scale)
        {
            emit_chart(chart_id, chart_short_desc, chart_long_desc, [&]() {
                std::vector<std::string> stage_names;
                stage_names.reserve(kLogicalStages.size());
                for (const auto& stage : kLogicalStages) {
                    stage_names.emplace_back(stage.display_name);
                }
                writer.key("data");
                writer.begin_array();
                for (const auto& algo : head_to_head_algos) {
                    std::vector<double> ys;
                    ys.reserve(kLogicalStages.size());
                    for (const auto& stage : kLogicalStages) {
                        ys.push_back(stage_value(algo, stage, metric_id) * scale);
                    }
                    writer.begin_object();
                    writer.kv_string("type", "bar");
                    writer.kv_string("name", algo);
                    writer.kv_string_array("x", stage_names);
                    writer.kv_double_array("y", ys);
                    writer.end_object();
                }
                writer.end_array();
                emit_layout(
                    fmt::format("Per-stage {} ({} bucket)",
                                y_axis_label,
                                sequence_length_bucket_name(largest_bucket)),
                    "logical stage", false,
                    y_axis_label, false,
                    "group");
            });
        };
        // metric_id: 1=cycles, 2=instructions, 3=branches, 4=branch_misses
        emit_counter_chart(
            "headline_largest_per_stage_cycles",
            "Per-stage CPU cycles (every algorithm) — largest sequences",
            "Grouped bar chart: one bar per algorithm per logical stage, "
            "Y is mean PMU 'cycles' counter delta per call. Zero bars on "
            "every algo indicate the host has no PMU access (macOS without "
            "root, Linux with perf_event_paranoid > 1). Compare the bar "
            "heights side by side to see which algorithm is paying more "
            "CPU cycles for the same logical stage.",
            "mean CPU cycles / call",
            1, 1.0);
        emit_counter_chart(
            "headline_largest_per_stage_instructions",
            "Per-stage retired instructions (every algorithm) — largest sequences",
            "Same shape as the cycles chart but Y is mean retired-"
            "instruction counter per call. Useful complement to cycles "
            "for IPC analysis: instructions / cycles ≈ IPC.",
            "mean instructions / call",
            2, 1.0);
        emit_counter_chart(
            "headline_largest_per_stage_branches",
            "Per-stage retired branches (every algorithm) — largest sequences",
            "Same shape as the cycles chart but Y is mean branch-"
            "instruction counter per call. Read this beside the branch-"
            "misses chart to compute branch miss rate per stage.",
            "mean branches / call",
            3, 1.0);
        emit_counter_chart(
            "headline_largest_per_stage_branch_misses",
            "Per-stage branch mispredictions (every algorithm) — largest sequences",
            "Same shape as the cycles chart but Y is mean branch-"
            "mispredict counter per call. The bigger the bar, the more "
            "the stage is hurt by branch prediction noise. SIMD code "
            "with horizontal mask reductions (e.g. the slow legacy "
            "SIMD-mm) typically shows up as taller bars than its scalar "
            "peers — the production two-stack-nthash path mostly avoids "
            "this by using a uniform per-w rebuild instead of a "
            "data-dependent rescan.",
            "mean branch mispredicts / call",
            4, 1.0);
        // Branch miss rate (branch_misses / branches) per stage. Useful
        // because it normalises out the absolute call count and surfaces
        // the underlying branch-prediction quality. Computed inline
        // because it isn't a single PMU counter.
        emit_chart(
            "headline_largest_per_stage_branch_miss_rate",
            "Per-stage branch miss rate (every algorithm) — largest sequences",
            "Branch-miss rate = mispredicts / branches per stage and "
            "algorithm. Normalises out the per-stage call count so "
            "two algorithms running the same number of calls can be "
            "compared on prediction quality. A 5% miss rate on chaining "
            "is bad; a 1% miss rate on minimizer generation is normal.",
            [&]() {
                std::vector<std::string> stage_names;
                stage_names.reserve(kLogicalStages.size());
                for (const auto& stage : kLogicalStages) {
                    stage_names.emplace_back(stage.display_name);
                }
                writer.key("data");
                writer.begin_array();
                for (const auto& algo : head_to_head_algos) {
                    std::vector<double> ys;
                    ys.reserve(kLogicalStages.size());
                    for (const auto& stage : kLogicalStages) {
                        const double br  = stage_value(algo, stage, 3);
                        const double bm  = stage_value(algo, stage, 4);
                        ys.push_back(br > 0.0 ? (bm / br) : 0.0);
                    }
                    writer.begin_object();
                    writer.kv_string("type", "bar");
                    writer.kv_string("name", algo);
                    writer.kv_string_array("x", stage_names);
                    writer.kv_double_array("y", ys);
                    writer.end_object();
                }
                writer.end_array();
                emit_layout(
                    fmt::format("Per-stage branch miss rate ({} bucket)",
                                sequence_length_bucket_name(largest_bucket)),
                    "logical stage", false,
                    "branch_misses / branches", false,
                    "group");
            });

        // ---- Table: stage × algorithm grid with green→red coloring per row.
        //
        // The user's request: "color cells in the table to mark the
        // slowest ones and fastest ones like in the histogram (green to
        // red)". The plotly `table` trace supports per-cell fill colours
        // via `cells.fill.color = [[col0_row0, col0_row1, ...], ...]`.
        // We compute a colour per cell based on the rank of its value
        // within its row (across all algorithms), interpolating from
        // green (fastest) through yellow to red (slowest). Rows where
        // every algorithm reports zero (e.g. the divergence-estimation
        // row when `compute_divergence` is off) get a neutral grey
        // background so the reader doesn't read a colour signal where
        // there isn't one.
        //
        // Cell content shows the dominant phase (longest-firing on
        // that side) plus the time. Format: "<phase>: XXX ms". Falls
        // back to "—" when no data is present for that (stage, algo)
        // cell.
        auto fmt_phase_label = [](std::string_view raw) -> std::string {
            // Drop the "tool_minimap2." prefix so the cell label reads
            // like "mm_sketch: 13.2 ms" not
            // "tool_minimap2.mm_sketch: 13.2 ms" — saves horizontal
            // table space.
            constexpr std::string_view kMm2Prefix = "tool_minimap2.";
            if (raw.size() > kMm2Prefix.size() &&
                raw.substr(0, kMm2Prefix.size()) == kMm2Prefix) {
                return std::string{raw.substr(kMm2Prefix.size())};
            }
            return std::string{raw};
        };

        auto cell_text = [&](const std::string& algo, const LogicalStage& stage) {
            // Find the dominant phase that fired for this algorithm
            // within this logical stage, then format
            // "<dominant_phase>: XX.X ms". Total cell time is the sum
            // across all phases; we annotate which phase dominates so
            // the reader can see whether (e.g.) "Chaining" is from
            // mg_lchain_dp or mg_lchain_simd.
            const auto algo_it = per_algo_phase.find(algo);
            if (algo_it == per_algo_phase.end()) return std::string{"—"};
            double total_ns = 0.0;
            std::string dominant_phase;
            double dominant_ns = 0.0;
            for (const auto& alias : stage.phase_aliases) {
                const auto p_it = algo_it->second.find(std::string{alias});
                if (p_it == algo_it->second.end()) continue;
                total_ns += p_it->second.mean_ns;
                if (p_it->second.mean_ns > dominant_ns) {
                    dominant_ns = p_it->second.mean_ns;
                    dominant_phase = std::string{alias};
                }
            }
            if (total_ns <= 0.0) return std::string{"—"};
            return fmt::format("{}: {:.2f} ms",
                fmt_phase_label(dominant_phase), total_ns / 1e6);
        };

        // Colour helper. value in [vmin, vmax] → hex string.
        // Linear interpolation: green (#2ca02c) for vmin → yellow
        // (#ffeb3b) for the median → red (#d62728) for vmax. Returns a
        // light grey for missing data (dash text); plotly accepts
        // either string colours or RGB arrays — strings are simpler.
        auto interp_color = [](double value, double vmin, double vmax) -> std::string {
            if (!(vmax > vmin)) {
                return std::string{"#e0e0e0"};
            }
            double t = (value - vmin) / (vmax - vmin);
            if (t < 0.0) t = 0.0;
            if (t > 1.0) t = 1.0;
            // Two-segment lerp green→yellow→red.
            // green:  (44, 160, 44)
            // yellow: (255, 235, 59)
            // red:    (214, 39, 40)
            int r, g, b;
            if (t <= 0.5) {
                const double s = t / 0.5;
                r = static_cast<int>(44   + s * (255 - 44));
                g = static_cast<int>(160  + s * (235 - 160));
                b = static_cast<int>(44   + s * (59  - 44));
            } else {
                const double s = (t - 0.5) / 0.5;
                r = static_cast<int>(255 + s * (214 - 255));
                g = static_cast<int>(235 + s * (39  - 235));
                b = static_cast<int>(59  + s * (40  - 59));
            }
            char buf[8];
            std::snprintf(buf, sizeof(buf), "#%02x%02x%02x",
                          r & 0xff, g & 0xff, b & 0xff);
            return std::string{buf};
        };

        const std::string table_chart_id =
            "headline_largest_stage_table";
        const std::string table_title = fmt::format(
            "Stage × algorithm head-to-head ({} bucket)",
            sequence_length_bucket_name(largest_bucket));
        emit_chart(table_chart_id,
            "Headline: stage × algorithm comparison table (largest sequences, color-coded green→red)",
            "Each row is a logical pipeline stage; each column is one "
            "algorithm. Cells contain the dominant phase name + the "
            "mean wall-clock per call (e.g. 'mm_sketch_simd: 13.45 ms'). "
            "Cell background colour is a per-row gradient — green for "
            "the fastest algorithm in that stage, red for the slowest, "
            "yellow in the middle. Rows where every algorithm reports "
            "zero are coloured grey. Use this table to spot which "
            "stages bioseq dominates and which still trail minimap2.",
            [&]() {
                writer.key("data");
                writer.begin_array();
                writer.begin_object();
                writer.kv_string("type", "table");

                // Column 0 is the stage name; columns 1..N are the algorithms.
                std::vector<std::string> header_values;
                header_values.emplace_back("Stage");
                for (const auto& algo : head_to_head_algos) {
                    header_values.push_back(algo);
                }
                writer.key("header");
                writer.begin_object();
                writer.kv_string_array("values", header_values);
                writer.kv_string("align", "left");
                writer.end_object();

                // cells.values is column-major: one inner array per column.
                // cells.fill.color is also column-major, same shape.
                writer.key("cells");
                writer.begin_object();
                writer.key("values");
                writer.begin_array();

                // Stage column — contains all the row labels.
                writer.begin_array();
                for (const auto& stage : kLogicalStages) {
                    writer.value_string(stage.display_name);
                }
                writer.end_array();

                // Per-algorithm columns.
                for (const auto& algo : head_to_head_algos) {
                    writer.begin_array();
                    for (const auto& stage : kLogicalStages) {
                        writer.value_string(cell_text(algo, stage));
                    }
                    writer.end_array();
                }
                writer.end_array();

                // cells.fill.color — per-row colour, computed from the
                // mean ns of each (stage, algo) cell. Plotly expects
                // column-major arrays here too, so we emit one array
                // per column with the per-row colours.
                writer.key("fill");
                writer.begin_object();
                writer.key("color");
                writer.begin_array();

                // Stage column gets a fixed light-grey backing so the
                // reader's eye reads it as "row label" not "datum".
                writer.begin_array();
                for (std::size_t i = 0; i < kLogicalStages.size(); ++i) {
                    writer.value_string("#f5f5f5");
                }
                writer.end_array();

                // Per-algo cell colours.
                for (const auto& algo : head_to_head_algos) {
                    writer.begin_array();
                    for (const auto& stage : kLogicalStages) {
                        // Compute the row's vmin/vmax across all
                        // algorithms in head_to_head_algos so the
                        // gradient is consistent within a row.
                        double vmin = std::numeric_limits<double>::infinity();
                        double vmax = -std::numeric_limits<double>::infinity();
                        bool any_value = false;
                        for (const auto& other : head_to_head_algos) {
                            const double v = stage_value(other, stage, 0);
                            if (v <= 0.0) continue;
                            any_value = true;
                            if (v < vmin) vmin = v;
                            if (v > vmax) vmax = v;
                        }
                        const double v = stage_value(algo, stage, 0);
                        if (!any_value || v <= 0.0) {
                            writer.value_string("#e0e0e0");
                        } else if (vmax == vmin) {
                            // Every algorithm reports the same value —
                            // colour neutral so we don't manufacture a
                            // signal.
                            writer.value_string("#cccccc");
                        } else {
                            writer.value_string(interp_color(v, vmin, vmax));
                        }
                    }
                    writer.end_array();
                }

                writer.end_array();
                writer.end_object();  // fill

                writer.kv_string("align", "left");
                writer.end_object();  // cells

                writer.end_object();  // table trace
                writer.end_array();   // data

                // Table layout — set a height so the reader can see all rows.
                writer.key("layout");
                writer.begin_object();
                writer.key("title");
                writer.begin_object();
                writer.kv_string("text", table_title);
                writer.end_object();
                writer.kv_string("template", "plotly_white");
                writer.end_object();
            });
    }

    // ================================================================
    // Chart 17: per-algorithm summary table — counts, mean ms, median
    // ms, P99 ms, accuracy. Plotly `table` trace renders this as a
    // grid in the report.
    // ================================================================
    emit_chart("algorithm_summary_table",
        "Per-algorithm summary table",
        "Tabular roll-up of every algorithm's pair count, mean / "
        "median / P99 latency in milliseconds, and overall position-"
        "accuracy fraction. Useful as a one-glance sanity check before "
        "diving into the per-chart detail; the row ordering matches the "
        "chart legend order so the reader can cross-reference.",
        [&]() {
            std::vector<std::string> col_algo;
            std::vector<std::string> col_kind;
            std::vector<std::int64_t> col_pairs;
            std::vector<double> col_mean_ms;
            std::vector<double> col_median_ms;
            std::vector<double> col_p99_ms;
            std::vector<double> col_accuracy;
            for (const auto& algorithm_label : algorithm_labels) {
                std::vector<double> all_latencies;
                std::size_t pair_count = 0;
                std::size_t correct_count = 0;
                std::string kind = "?";
                for (const auto& s : samples) {
                    if (s.algorithm_label != algorithm_label) continue;
                    kind = s.algorithm_kind;
                    pair_count++;
                    if (s.position_correct) correct_count++;
                    for (double lat : s.sample_latencies_ns) {
                        all_latencies.push_back(lat / 1e6);  // ms
                    }
                }
                if (pair_count == 0) continue;
                std::sort(all_latencies.begin(), all_latencies.end());
                col_algo.push_back(algorithm_label);
                col_kind.push_back(kind);
                col_pairs.push_back(static_cast<std::int64_t>(pair_count));
                col_mean_ms.push_back(detail::mean_of(all_latencies));
                col_median_ms.push_back(detail::percentile_sorted(all_latencies, 0.50));
                col_p99_ms.push_back(detail::percentile_sorted(all_latencies, 0.99));
                col_accuracy.push_back(pair_count > 0
                    ? static_cast<double>(correct_count) /
                      static_cast<double>(pair_count)
                    : 0.0);
            }
            writer.key("data");
            writer.begin_array();
            writer.begin_object();
            writer.kv_string("type", "table");
            writer.key("header");
            writer.begin_object();
            writer.key("values");
            writer.begin_array();
            writer.value_string("algorithm");
            writer.value_string("kind");
            writer.value_string("pairs");
            writer.value_string("mean ms");
            writer.value_string("median ms");
            writer.value_string("P99 ms");
            writer.value_string("accuracy");
            writer.end_array();
            writer.kv_string("align", "left");
            writer.end_object();
            writer.key("cells");
            writer.begin_object();
            writer.key("values");
            writer.begin_array();
            // Each entry is one column.
            writer.begin_array(); for (const auto& v : col_algo) writer.value_string(v); writer.end_array();
            writer.begin_array(); for (const auto& v : col_kind) writer.value_string(v); writer.end_array();
            writer.begin_array(); for (auto v : col_pairs) writer.value_int(v); writer.end_array();
            writer.begin_array(); for (auto v : col_mean_ms) writer.value_double(v); writer.end_array();
            writer.begin_array(); for (auto v : col_median_ms) writer.value_double(v); writer.end_array();
            writer.begin_array(); for (auto v : col_p99_ms) writer.value_double(v); writer.end_array();
            writer.begin_array(); for (auto v : col_accuracy) writer.value_double(v); writer.end_array();
            writer.end_array();
            writer.kv_string("align", "left");
            writer.end_object();
            writer.end_object();
            writer.end_array();
            // Tables don't use xaxis/yaxis; the title-only layout
            // helper keeps `template = "plotly_white"` consistent
            // with every other chart.
            bench_scheduler::charting::emit_plotly_layout_titled(
                writer, "Per-algorithm summary table");
        });

        });  // end of charts_body lambda passed to emit_report_envelope
}

/**
 * @brief Convenience: build the report directly into a JsonWriter and
 *        write it to disk. Returns false on I/O failure.
 *
 * `violations` is the regression-checker output (see
 * `bench.regression_check`); pass an empty vector when there are no
 * violations or when the bench is being run without rule enforcement.
 *
 * `commit_hash` is stamped into the document so report files keep
 * their provenance after agents move them between directories.
 */
[[nodiscard]] inline bool write_report_json_to_file(
    const std::string& path,
    std::span<const RegressionViolation> violations = {},
    std::string_view commit_hash = "unknown")
{
    JsonWriter writer;
    emit_report(writer, violations, commit_hash);
    return bench_scheduler::write_report_to_file(
        writer, path, "bench.aligners.chart_builder");
}

}  // namespace bioseq::bench

}  // export
