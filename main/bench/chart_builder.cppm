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
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

export module bench.chart_builder;

import mlogging;
import bench.matrix_runner;
import bench.json_writer;

export {

namespace bioseq::bench {

namespace detail {

/// Linear interpolation percentile estimator over a pre-sorted vector.
/// Returns 0.0 when the vector is empty so the caller can short-circuit
/// without separate guard logic.
[[nodiscard]] inline double percentile_sorted(
    const std::vector<double>& sorted_values, double percentile) noexcept
{
    if (sorted_values.empty()) return 0.0;
    if (sorted_values.size() == 1) return sorted_values.front();
    const double rank = percentile * static_cast<double>(sorted_values.size() - 1);
    const std::size_t low_index = static_cast<std::size_t>(std::floor(rank));
    const std::size_t high_index = static_cast<std::size_t>(std::ceil(rank));
    const double fraction = rank - static_cast<double>(low_index);
    return sorted_values[low_index] +
           (sorted_values[high_index] - sorted_values[low_index]) * fraction;
}

[[nodiscard]] inline double mean_of(const std::vector<double>& values) noexcept {
    if (values.empty()) return 0.0;
    double sum = 0.0;
    for (double v : values) sum += v;
    return sum / static_cast<double>(values.size());
}

/// Stable ordering of algorithm labels, with the two minimap2 baselines
/// pinned to the front so chart legends always read "baselines first,
/// then bioseq variants" regardless of insertion order in the sample
/// vector.
[[nodiscard]] inline std::vector<std::string> ordered_algorithm_labels(
    const std::vector<BenchSample>& samples)
{
    std::set<std::string> seen;
    for (const auto& s : samples) seen.insert(s.algorithm_label);
    std::vector<std::string> out;
    out.reserve(seen.size());
    auto take = [&](std::string_view label) {
        const std::string label_str{label};
        if (seen.contains(label_str)) {
            out.push_back(label_str);
            seen.erase(label_str);
        }
    };
    take("minimap2 (DP chaining)");
    take("minimap2 (RMQ chaining)");
    out.insert(out.end(), seen.begin(), seen.end());
    return out;
}

/// Map an algorithm label to a stable colour. Keeps each algorithm's
/// trace colour consistent across charts so the reader can follow the
/// same line across the deck.
[[nodiscard]] inline std::string colour_for(std::string_view algorithm_label) noexcept {
    static constexpr std::array<std::string_view, 16> palette = {
        "#1f77b4", "#ff7f0e", "#2ca02c", "#d62728",
        "#9467bd", "#8c564b", "#e377c2", "#7f7f7f",
        "#bcbd22", "#17becf", "#393b79", "#637939",
        "#8c6d31", "#843c39", "#7b4173", "#3182bd"};
    // Deterministic hash based on string contents — std::hash is fine
    // since we do not need cryptographic stability.
    std::size_t hash_val = 1469598103934665603ull;
    for (char c : algorithm_label) {
        hash_val ^= static_cast<std::size_t>(static_cast<unsigned char>(c));
        hash_val *= 1099511628211ull;
    }
    return std::string{palette[hash_val % palette.size()]};
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
    }
    return "?";
}

}  // namespace detail

/**
 * @brief Top-level emitter — populates the writer with the full
 *        `report.json` document.
 */
inline void emit_report(JsonWriter& writer) {
    const auto& samples = benchmark_samples();
    const auto algorithm_labels = detail::ordered_algorithm_labels(samples);

    writer.begin_object();  // root
    writer.kv_string("schema", "bioseq-bench-report-v1");
    writer.kv_uint("sample_count", samples.size());
    writer.kv_uint("algorithm_count", algorithm_labels.size());

    writer.key("charts");
    writer.begin_object();  // charts

    // ----------------------------------------------------------------
    // Helper: emit one chart entry { short_description, long_description,
    // plotly: {data, layout} }. The body callback is responsible for
    // emitting the `data` and `layout` members.
    // ----------------------------------------------------------------
    auto emit_chart = [&](
        std::string_view chart_id,
        std::string_view short_description,
        std::string_view long_description,
        auto&& body)
    {
        writer.key(chart_id);
        writer.begin_object();
        writer.kv_string("short_description", short_description);
        writer.kv_string("long_description",  long_description);
        writer.key("plotly");
        writer.begin_object();
        body();  // emits data + layout members of the plotly figure
        writer.end_object();
        writer.end_object();
    };

    // Helper: emit a layout block with shared {title, xaxis, yaxis,
    // legend} settings. Caller passes axis titles + scale.
    auto emit_layout = [&](
        std::string_view title,
        std::string_view x_title, bool x_log,
        std::string_view y_title, bool y_log,
        std::string_view barmode = "")
    {
        writer.key("layout");
        writer.begin_object();
        writer.key("title");
        writer.begin_object();
        writer.kv_string("text", title);
        writer.end_object();
        writer.key("xaxis");
        writer.begin_object();
        writer.key("title");
        writer.begin_object();
        writer.kv_string("text", x_title);
        writer.end_object();
        if (x_log) writer.kv_string("type", "log");
        writer.end_object();
        writer.key("yaxis");
        writer.begin_object();
        writer.key("title");
        writer.begin_object();
        writer.kv_string("text", y_title);
        writer.end_object();
        if (y_log) writer.kv_string("type", "log");
        writer.end_object();
        writer.key("legend");
        writer.begin_object();
        writer.kv_string("orientation", "v");
        writer.end_object();
        writer.kv_bool("showlegend", true);
        if (!barmode.empty()) writer.kv_string("barmode", barmode);
        writer.kv_string("template", "plotly_white");
        writer.end_object();
    };

    // Helper: emit a marker object with a deterministic colour pulled
    // from the per-algorithm palette. Used by every scatter/bar trace.
    auto emit_marker = [&](std::string_view algorithm_label) {
        writer.key("marker");
        writer.begin_object();
        writer.kv_string("color", detail::colour_for(algorithm_label));
        writer.kv_int("size", 7);
        writer.end_object();
        writer.key("line");
        writer.begin_object();
        writer.kv_string("color", detail::colour_for(algorithm_label));
        writer.kv_int("width", 2);
        writer.end_object();
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
            constexpr std::array<SequenceLengthBucket, 4> all_buckets = {
                SequenceLengthBucket::SHORT,
                SequenceLengthBucket::MEDIUM,
                SequenceLengthBucket::LONG,
                SequenceLengthBucket::VERY_LONG,
            };
            writer.key("data");
            writer.begin_array();
            for (const auto& algorithm_label : headline_algorithm_labels) {
                std::array<std::pair<std::size_t, std::size_t>, 4> per_bucket{};
                for (const auto& s : samples) {
                    if (s.algorithm_label != algorithm_label) continue;
                    auto& counts = per_bucket[static_cast<std::size_t>(s.length_bucket)];
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
                for (std::size_t i = 0; i < 4; ++i) {
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
            const std::array<std::string_view, 4> length_symbols = {
                "circle", "square", "diamond", "triangle-up"};
            for (const auto& algorithm_label : headline_algorithm_labels) {
                std::array<std::pair<std::vector<double>, std::vector<double>>, 4> per_bucket;
                std::array<std::pair<std::size_t, std::size_t>, 4> correctness;
                for (const auto& s : samples) {
                    if (s.algorithm_label != algorithm_label) continue;
                    if (s.sample_latencies_ns.empty()) continue;
                    const auto bidx = static_cast<std::size_t>(s.length_bucket);
                    per_bucket[bidx].first.push_back(detail::mean_of(s.sample_latencies_ns) / 1e6);
                    correctness[bidx].second++;
                    if (s.position_correct) correctness[bidx].first++;
                }
                for (std::size_t i = 0; i < 4; ++i) {
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
                    writer.kv_string("color", detail::colour_for(algorithm_label));
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
                writer.kv_string("color", detail::colour_for(algorithm_label));
                writer.kv_string("dash", "dash");
                writer.kv_int("width", 2);
                writer.end_object();
                writer.key("marker");
                writer.begin_object();
                writer.kv_string("color", detail::colour_for(algorithm_label));
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
            constexpr std::array<SequenceLengthBucket, 4> all_buckets = {
                SequenceLengthBucket::SHORT,
                SequenceLengthBucket::MEDIUM,
                SequenceLengthBucket::LONG,
                SequenceLengthBucket::VERY_LONG,
            };
            writer.key("data");
            writer.begin_array();
            for (const auto& algorithm_label : algorithm_labels) {
                std::array<std::pair<std::size_t, std::size_t>, 4> per_bucket{};
                for (const auto& s : samples) {
                    if (s.algorithm_label != algorithm_label) continue;
                    auto& counts = per_bucket[static_cast<std::size_t>(s.length_bucket)];
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
                for (std::size_t i = 0; i < 4; ++i) {
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
                writer.kv_string("color", detail::colour_for(algorithm_label));
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
            };
            // z[row][col] — outer rows = algorithms, inner = buckets.
            std::vector<std::vector<double>> z;
            std::vector<std::string> row_labels;
            for (const auto& algorithm_label : algorithm_labels) {
                std::array<std::pair<std::size_t, std::size_t>, 4> per_bucket{};
                for (const auto& s : samples) {
                    if (s.algorithm_label != algorithm_label) continue;
                    auto& counts = per_bucket[static_cast<std::size_t>(s.length_bucket)];
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
                writer.kv_string("color", detail::colour_for(algorithm_label));
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
                writer.kv_string("color", detail::colour_for(algorithm_label));
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
                std::array<std::vector<double>, 4> per_length_ratios;
                for (const auto& s : samples) {
                    if (s.algorithm_label != algorithm_label) continue;
                    if (s.sample_latencies_ns.empty()) continue;
                    const cell_key key{static_cast<int>(s.length_bucket),
                                       static_cast<int>(s.error_bucket)};
                    auto baseline_it = baseline_cell_avg.find(key);
                    if (baseline_it == baseline_cell_avg.end() ||
                        baseline_it->second <= 0.0) continue;
                    const double variant_mean = detail::mean_of(s.sample_latencies_ns);
                    if (variant_mean <= 0.0) continue;
                    per_length_ratios[static_cast<std::size_t>(s.length_bucket)]
                        .push_back(baseline_it->second / variant_mean);
                }
                std::vector<std::string> xs;
                std::vector<double> ys;
                for (std::size_t i = 0; i < 4; ++i) {
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
                writer.kv_string("color", detail::colour_for(algorithm_label));
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
                writer.kv_string("color", detail::colour_for(algorithm_label));
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
                writer.kv_string("color", detail::colour_for(algorithm_label));
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
            const std::array<std::string_view, 4> length_symbols = {
                "circle", "square", "diamond", "triangle-up"};
            for (const auto& algorithm_label : algorithm_labels) {
                std::array<std::pair<std::vector<double>, std::vector<double>>, 4> per_bucket;
                std::array<std::pair<std::size_t, std::size_t>, 4> correctness;
                for (const auto& s : samples) {
                    if (s.algorithm_label != algorithm_label) continue;
                    if (s.sample_latencies_ns.empty()) continue;
                    const auto bidx = static_cast<std::size_t>(s.length_bucket);
                    per_bucket[bidx].first.push_back(detail::mean_of(s.sample_latencies_ns) / 1e6);
                    correctness[bidx].second++;
                    if (s.position_correct) correctness[bidx].first++;
                }
                for (std::size_t i = 0; i < 4; ++i) {
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
                    writer.kv_string("color", detail::colour_for(algorithm_label));
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
            constexpr std::array<SequenceLengthBucket, 4> all_lengths = {
                SequenceLengthBucket::SHORT,
                SequenceLengthBucket::MEDIUM,
                SequenceLengthBucket::LONG,
                SequenceLengthBucket::VERY_LONG,
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
            // Tables don't use xaxis/yaxis but Plotly accepts a layout
            // block; emit a minimal one for chart-title consistency.
            writer.key("layout");
            writer.begin_object();
            writer.key("title");
            writer.begin_object();
            writer.kv_string("text", "Per-algorithm summary table");
            writer.end_object();
            writer.kv_string("template", "plotly_white");
            writer.end_object();
        });

    writer.end_object();  // charts
    writer.end_object();  // root
}

/**
 * @brief Convenience: build the report directly into a JsonWriter and
 *        write it to disk. Returns false on I/O failure.
 */
[[nodiscard]] inline bool write_report_json_to_file(const std::string& path) {
    JsonWriter writer;
    emit_report(writer);
    if (!writer.write_to_file(path)) {
        JustLog("[bench.chart_builder] FAILED to write report.json to {}", path);
        return false;
    }
    JustLog("[bench.chart_builder] Wrote report.json ({} bytes) to {}",
            writer.str().size(), path);
    return true;
}

}  // namespace bioseq::bench

}  // export
