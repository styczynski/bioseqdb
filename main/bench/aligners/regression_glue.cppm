/**
 * @file regression_glue.cppm
 * @brief Adapt the aligner bench's `BenchSample` data into the generic
 *        regression engine's cell-and-metric-lookup surface.
 *
 * The generic `bench.scheduler.regression` engine doesn't know what a
 * BenchSample is, what a length bucket is, or how to compute mean
 * latency from a sample vector. This module supplies all three:
 *
 *   * `aligner_default_config()` — the pre-refactor "production rules"
 *     that used to live as a hard-coded table inside the old
 *     `regression_check.cppm`. Used as a fallback when the threshold
 *     file is missing or unparseable.
 *
 *   * `AlignerCellKey` — the (length_bucket, error_bucket) tuple that
 *     identifies one cell in the aligner's matrix.
 *
 *   * `AlignerCellTable` — pre-computed `(cell, algorithm) -> latency
 *     stats / accuracy stats` aggregation over `benchmark_samples()`.
 *     Built once per regression-check call.
 *
 *   * `make_lookup` / `make_label_fn` — the two callables the engine
 *     needs. Pure adapters; no logic of their own.
 *
 * The aligner-only schema mapping happens here:
 *
 *   metric "wall_clock"  → mean of `BenchSample::sample_latencies_ns`
 *                          across pairs in the cell  (RELATIVE)
 *   metric "accuracy"    → fraction of samples with
 *                          `BenchSample::position_correct == true`
 *                          across pairs in the cell  (ABSOLUTE)
 *
 * The metric-name → CompareKind mapping in
 * `aligner_default_config()` keeps the same defaults the old engine
 * had baked in, but anyone editing
 * `regression_thresholds.json` can override per-rule by adding
 * `"compare": "absolute"` (or "relative") to a rule entry.
 */
module;

#include "common.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bench.aligners.regression_glue;

import mlogging;
import bench.scheduler.regression;
import bench.aligners.samples;

export {

namespace bioseq::bench {

/// Cell key for the aligner bench: a (length_bucket, error_bucket) pair.
/// Equality and ordering come for free from std::pair, which is what
/// the regression engine's iteration order relies on.
using AlignerCellKey = std::pair<SequenceLengthBucket, ErrorRateBucket>;

namespace detail {

/// Pre-aggregated stats per (cell, algorithm). The engine's lookup
/// callable returns the metric value from this table; building it
/// once amortises the per-rule iteration cost when the rule list and
/// the algorithm × cell matrix are both large.
struct AlignerCellStats {
    /// Sum of per-record mean latencies (ns). Divided by `mean_count`
    /// at lookup time so caller's "mean over cells" is correct.
    double mean_ns_sum   = 0.0;
    /// Number of records that contributed a non-empty
    /// sample_latencies_ns vector. Records with empty latencies (skipped
    /// or aborted tasks) don't pollute the mean.
    std::size_t mean_count = 0;
    /// Per-pair counters used by the accuracy rule.
    std::size_t total_pairs = 0;
    std::size_t correct_pairs = 0;
};

}  // namespace detail

/// One entry per (cell, algorithm). Outer key is (length, error);
/// inner is the algorithm label. Constructed once via
/// `aggregate_aligner_cells` and then handed to the engine alongside
/// the lookup / label callables.
using AlignerCellTable =
    std::map<AlignerCellKey, std::map<std::string, detail::AlignerCellStats>>;

/**
 * @brief Aggregate `benchmark_samples()` into per-cell × per-algorithm stats.
 *
 * Empty `sample_latencies_ns` records still bump `total_pairs` but do
 * NOT contribute to `mean_ns_sum` — the chart layer uses the same rule
 * so the regression-check view and the chart view stay consistent.
 */
[[nodiscard]] inline AlignerCellTable aggregate_aligner_cells(
    std::span<const BenchSample> samples)
{
    AlignerCellTable out;
    for (const auto& s : samples) {
        const AlignerCellKey key{s.length_bucket, s.error_bucket};
        auto& cell = out[key][s.algorithm_label];
        cell.total_pairs += 1;
        if (s.position_correct) cell.correct_pairs += 1;
        if (!s.sample_latencies_ns.empty()) {
            double sum_ns = 0.0;
            for (double v : s.sample_latencies_ns) sum_ns += v;
            cell.mean_ns_sum += sum_ns / static_cast<double>(s.sample_latencies_ns.size());
            cell.mean_count  += 1;
        }
    }
    return out;
}

/**
 * @brief Pull the human-readable cell labels for one (length, error).
 *
 * Walks `benchmark_samples()` to find the first record matching the
 * cell key — that record carries pre-formatted `length_bucket_label`
 * and `error_bucket_label` strings populated by `populate_bench_sample_meta`.
 * Falls back to enum names if no record matches (defensive — should
 * not happen because `aggregate_aligner_cells` only emits keys for
 * cells that have at least one record).
 */
[[nodiscard]] inline std::pair<std::string, std::string>
aligner_cell_string_labels(const AlignerCellKey& key,
                           std::span<const BenchSample> samples)
{
    for (const auto& s : samples) {
        if (s.length_bucket == key.first && s.error_bucket == key.second) {
            return {s.length_bucket_label, s.error_bucket_label};
        }
    }
    return {std::string{sequence_length_bucket_name(key.first)},
            std::string{error_rate_bucket_name(key.second)}};
}

/**
 * @brief Build a metric-lookup callable bound to the cell table.
 *
 * The callable returns:
 *   * `std::nullopt` when the (cell, algorithm) tuple has no data,
 *   * mean ns per pair for `metric == "wall_clock"`,
 *   * fraction of correct pairs for `metric == "accuracy"`,
 *   * `std::nullopt` for any other metric name (the IO bench can
 *     register its own metric names by passing a different lookup;
 *     this glue is aligner-only).
 */
[[nodiscard]] inline bench_scheduler::regression::MetricLookup<AlignerCellKey>
make_aligner_metric_lookup(const AlignerCellTable& cells)
{
    return [&cells](const AlignerCellKey& key,
                    std::string_view algorithm,
                    std::string_view metric) -> std::optional<double> {
        const auto cell_it = cells.find(key);
        if (cell_it == cells.end()) return std::nullopt;
        const auto algo_it = cell_it->second.find(std::string{algorithm});
        if (algo_it == cell_it->second.end()) return std::nullopt;
        const auto& stats = algo_it->second;
        if (metric == "wall_clock") {
            if (stats.mean_count == 0) return std::nullopt;
            return stats.mean_ns_sum / static_cast<double>(stats.mean_count);
        }
        if (metric == "accuracy") {
            if (stats.total_pairs == 0) return std::nullopt;
            return static_cast<double>(stats.correct_pairs) /
                   static_cast<double>(stats.total_pairs);
        }
        return std::nullopt;
    };
}

/**
 * @brief Build a cell-label callable that names cell components
 *        `length_bucket` and `error_bucket`.
 *
 * Those names are byte-equivalent to the pre-refactor JSON schema —
 * existing report consumers see the same fields they always did.
 */
[[nodiscard]] inline bench_scheduler::regression::CellLabelFn<AlignerCellKey>
make_aligner_cell_label_fn(std::span<const BenchSample> samples)
{
    return [samples](const AlignerCellKey& key) {
        const auto [length_label, error_label] =
            aligner_cell_string_labels(key, samples);
        std::vector<bench_scheduler::regression::CellLabel> out;
        out.reserve(2);
        out.push_back({std::string{"length_bucket"}, length_label});
        out.push_back({std::string{"error_bucket"},  error_label});
        return out;
    };
}

/**
 * @brief Hard-coded fallback rule set — the production rules that used
 *        to live in `regression_check.cppm::default_config()`.
 *
 * Mirrors the schema that ships in `regression_thresholds.json` so the
 * bench still enforces the production rules even on a stripped checkout
 * where the JSON file is missing. Edit only when the human operator
 * asks (CLAUDE.md hard rule).
 */
[[nodiscard]] inline bench_scheduler::regression::Config aligner_default_config() {
    using namespace bench_scheduler::regression;
    Config cfg;
    cfg.tolerance_pct = 2.0;
    cfg.baselines["default"] = "minimap2 (DP chaining)";
    cfg.baselines["rmq"]     = "minimap2 (RMQ chaining)";

    auto add_rule = [&](
        std::string_view algorithm, std::string_view baseline_key,
        std::string_view metric, CompareKind compare, RuleKind kind,
        double value_pct, std::string_view comment)
    {
        Rule r;
        r.algorithm_label = std::string{algorithm};
        r.baseline_key    = std::string{baseline_key};
        r.metric          = std::string{metric};
        r.compare         = compare;
        r.kind            = kind;
        r.value_pct       = value_pct;
        r.comment         = std::string{comment};
        cfg.rules.push_back(std::move(r));
    };

    add_rule("bioseq KSW2 + SIMD-DP + two-stack-nthash-mm", "default",
             "wall_clock", CompareKind::RELATIVE, RuleKind::FASTER_BY_AT_LEAST_PCT, 5.0,
             "Production SIMD-DP minimizer must be >= 5% faster than minimap2 DP.");
    add_rule("bioseq Block + SIMD-DP + two-stack-nthash-mm", "default",
             "wall_clock", CompareKind::RELATIVE, RuleKind::FASTER_BY_AT_LEAST_PCT, 8.0,
             "Block aligner SIMD-DP path must be >= 8% faster than minimap2 DP.");
    add_rule("bioseq KSW2 + scalar-DP + scalar-mm", "default",
             "wall_clock", CompareKind::RELATIVE, RuleKind::EQUAL_TO, 0.0,
             "Scalar reference path must match minimap2 DP wall-clock.");
    add_rule("bioseq KSW2 + RMQ + scalar-mm", "rmq",
             "wall_clock", CompareKind::RELATIVE, RuleKind::EQUAL_TO, 0.0,
             "RMQ chaining path must match minimap2 RMQ wall-clock.");
    add_rule("bioseq KSW2 + SIMD-DP + two-stack-nthash-mm", "default",
             "accuracy", CompareKind::ABSOLUTE, RuleKind::EQUAL_TO, 0.0,
             "Production SIMD path must match minimap2 DP position accuracy.");
    add_rule("bioseq Block + SIMD-DP + two-stack-nthash-mm", "default",
             "accuracy", CompareKind::ABSOLUTE, RuleKind::EQUAL_TO, 0.0,
             "Block aligner SIMD path must match minimap2 DP position accuracy.");

    Rule wildcard;
    wildcard.algorithm_prefix = "bioseq ";
    wildcard.baseline_key     = "default";
    wildcard.metric           = "accuracy";
    wildcard.compare          = CompareKind::ABSOLUTE;
    wildcard.kind             = RuleKind::EQUAL_TO;
    wildcard.value_pct        = 0.0;
    wildcard.comment          =
        "All bioseq algorithms must match minimap2 DP position accuracy unless overridden.";
    cfg.wildcard_rules.push_back(std::move(wildcard));
    return cfg;
}

/**
 * @brief Top-level aligner-bench convenience: compute violations for
 *        the live `benchmark_samples()` accumulator.
 *
 * Falls back to `aligner_default_config()` if the supplied config has
 * no rules (the engine returns an empty config for a missing /
 * malformed JSON file; we want to keep enforcing on stripped checkouts).
 */
[[nodiscard]] inline std::vector<bench_scheduler::regression::Violation>
compute_aligner_violations(std::span<const BenchSample> samples,
                           const bench_scheduler::regression::Config& cfg_in)
{
    using namespace bench_scheduler::regression;
    if (samples.empty()) return {};

    Config effective = cfg_in;
    if (effective.rules.empty() && effective.wildcard_rules.empty()) {
        JustLog("[bench.aligners.regression_glue] no rules in config — falling back to defaults");
        effective = aligner_default_config();
    }

    const auto cells = aggregate_aligner_cells(samples);
    std::vector<AlignerCellKey> cell_keys;
    cell_keys.reserve(cells.size());
    std::vector<std::string> algorithms;
    for (const auto& [key, by_algo] : cells) {
        cell_keys.push_back(key);
        for (const auto& [label, _] : by_algo) {
            (void)_;
            if (std::find(algorithms.begin(), algorithms.end(), label) ==
                algorithms.end()) {
                algorithms.push_back(label);
            }
        }
    }
    std::sort(algorithms.begin(), algorithms.end());

    auto lookup = make_aligner_metric_lookup(cells);
    auto label_fn = make_aligner_cell_label_fn(samples);
    return compute_violations<AlignerCellKey>(
        std::span<const AlignerCellKey>{cell_keys},
        std::span<const std::string>{algorithms},
        effective, lookup, label_fn);
}

}  // namespace bioseq::bench

}  // export
