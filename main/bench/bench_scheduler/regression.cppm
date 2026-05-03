/**
 * @file regression.cppm
 * @brief Generic regression-rule engine for bench harnesses.
 *
 * The engine is domain-agnostic. It loads a JSON rule file, walks a
 * caller-supplied list of `(cell_key, algorithm)` cells via a
 * `MetricLookup` callable, and emits `Violation` records for any rule
 * whose actual-vs-baseline arithmetic is out of tolerance. The aligner
 * bench uses `cell_key = pair<length_bucket, error_bucket>` and
 * registers `wall_clock` (relative comparison) + `accuracy` (absolute
 * comparison) metrics; an IO bench could use
 * `cell_key = pair<format, file_size_bucket>` and register
 * `throughput_mb_per_sec` (relative) + `peak_rss_mb` (relative)
 * without modifying the engine.
 *
 * Three things to know:
 *
 * 1. **No metric-name string-dispatch.** The comparison strategy
 *    (relative-percentage vs absolute-distance) is encoded *on the
 *    rule itself* as `Rule::compare`. The metric name is just a
 *    free-form string the engine threads through to the user's
 *    `MetricLookup` callable. New metrics don't require engine code
 *    changes.
 *
 * 2. **`{name, value}` cell labels.** The user names every component of
 *    a cell key (e.g. "length_bucket", "error_bucket") and the JSON
 *    emitter writes those names as top-level fields on each violation
 *    object. That keeps the per-violation schema flexible without a
 *    forked "legacy" emitter.
 *
 * 3. **Single source of truth for arithmetic.** `is_violation_relative`
 *    and `is_violation_absolute` live here; aligner glue does not
 *    duplicate the (baseline - actual) / baseline math.
 *
 * Parser: `simdjson::dom::parser`. The threshold file is small (a few
 * kB), so DOM-style parsing is the right shape — random access into
 * the `baselines` map and a stable order over `rules` and
 * `wildcard_rules` is what the engine needs.
 */
module;

#include <simdjson.h>

#include "common.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bench.scheduler.regression;

import mlogging;
import bench.scheduler.json_writer;

export {

namespace bioseq::bench_scheduler::regression {

// ---------------------------------------------------------------------
// Data types
// ---------------------------------------------------------------------

/**
 * @brief How an actual value should be compared against its baseline.
 *
 *   * `RELATIVE` — percentage delta:
 *     `observed_pct = 100 * (baseline - actual) / baseline`.
 *     Tolerance is interpreted as a relative percentage of baseline.
 *     Use for ratio metrics (latency, throughput, memory).
 *
 *   * `ABSOLUTE` — direct difference:
 *     `observed_pct = 100 * (actual - baseline)`.
 *     Tolerance is an absolute distance in the metric's native unit.
 *     Use for metrics defined on `[0, 1]` where a 0 baseline cannot
 *     divide by zero (e.g. accuracy fractions).
 *
 * Sign convention: positive `observed_pct` means "actual is preferred
 * over baseline". For `RELATIVE` that means actual is *smaller* (faster /
 * less memory); for `ABSOLUTE` it means actual is *larger* (more
 * accurate). The console table prints `obs%` so this convention is
 * legible to the operator without mental gymnastics.
 */
enum class CompareKind : std::uint8_t {
    RELATIVE = 0,
    ABSOLUTE = 1,
};

[[nodiscard]] constexpr const char* compare_kind_name(CompareKind kind) noexcept {
    switch (kind) {
        case CompareKind::RELATIVE: return "relative";
        case CompareKind::ABSOLUTE: return "absolute";
    }
    return "?";
}

enum class RuleKind : std::uint8_t {
    EQUAL_TO              = 0,
    FASTER_BY_AT_LEAST_PCT = 1,
    /// Hard absolute floor: passes when `actual_value >= min_value`
    /// (with `tolerance_pct` of slack on the floor). No baseline lookup
    /// — the rule is self-contained. Useful for "design goal" checks
    /// where the comparison shouldn't drift with the baseline (e.g. a
    /// gzip reader's decompressed-MiB/s target shouldn't relax just
    /// because the kseqpp baseline got slower).
    MIN_ABSOLUTE           = 2,
};

[[nodiscard]] constexpr const char* rule_kind_name(RuleKind kind) noexcept {
    switch (kind) {
        case RuleKind::EQUAL_TO:               return "equal_to";
        case RuleKind::FASTER_BY_AT_LEAST_PCT: return "faster_by_at_least_pct";
        case RuleKind::MIN_ABSOLUTE:           return "min_absolute";
    }
    return "?";
}

[[nodiscard]] constexpr std::optional<RuleKind> parse_rule_kind(std::string_view name) noexcept {
    if (name == "equal_to")               return RuleKind::EQUAL_TO;
    if (name == "faster_by_at_least_pct") return RuleKind::FASTER_BY_AT_LEAST_PCT;
    if (name == "min_absolute")           return RuleKind::MIN_ABSOLUTE;
    return std::nullopt;
}

/**
 * @brief One rule loaded from the threshold JSON.
 *
 * Either `algorithm_label` (exact) or `algorithm_prefix` (wildcard) is
 * non-empty; never both. The baseline is identified by a logical key
 * that maps via `Config::baselines` to a real algorithm label, so the
 * user can rename a baseline (e.g. "minimap2 (DP chaining)" →
 * "minimap2 v2.30 DP") in one place and have every rule pick up the
 * change.
 */
struct Rule {
    std::string algorithm_label;
    std::string algorithm_prefix;
    std::string baseline_key;
    std::string metric;
    CompareKind compare = CompareKind::RELATIVE;
    RuleKind    kind    = RuleKind::EQUAL_TO;
    double      value_pct = 0.0;
    /// Floor used by MIN_ABSOLUTE rules. Read from the JSON's
    /// `min_value` key. Native units of the rule's metric (e.g. MiB/s).
    double      min_value = 0.0;
    std::string comment;
};

/**
 * @brief Top-level config parsed from `regression_thresholds.json`.
 *
 * Tolerance is global; per-rule overrides are deliberately not
 * supported — the same numeric should mean the same wiggle-room
 * regardless of which rule produced it.
 */
struct Config {
    double tolerance_pct = 2.0;
    std::map<std::string, std::string> baselines;
    std::vector<Rule> rules;
    std::vector<Rule> wildcard_rules;
};

/// Caller-named cell-key component. E.g. for the aligner bench:
///   `{ "length_bucket", "long (10k-1M)" }`
///   `{ "error_bucket",  "ccs (~1.5%)" }`
/// The JSON emitter writes each entry as a top-level field on the
/// violation object so the resulting per-violation shape is whatever
/// the caller chooses.
struct CellLabel {
    std::string name;
    std::string value;
};

struct Violation {
    std::string algorithm_label;
    std::string baseline_label;
    std::string metric;
    CompareKind compare = CompareKind::RELATIVE;
    RuleKind    kind    = RuleKind::EQUAL_TO;
    /// Caller-named cell-key components.
    std::vector<CellLabel> cell_labels;
    double tolerance_pct  = 0.0;
    double threshold_pct  = 0.0;
    double baseline_value = 0.0;
    double actual_value   = 0.0;
    /// Sign: positive = actual preferred over baseline (faster /
    /// more accurate). The console table prints this as `obs%` so the
    /// human reader doesn't have to recompute the convention.
    double observed_pct   = 0.0;
    std::string rule_comment;
};

/**
 * @brief Per-(cell, algorithm, metric) value lookup.
 *
 * Returns `std::nullopt` when the tuple has no data. The engine
 * silently skips violations on missing data — the caller decides
 * whether to surface that elsewhere.
 *
 * The CellKey template parameter is the user's choice (any
 * stringifiable / hashable / equality-comparable type works). The
 * engine never inspects it directly; it just forwards to this
 * callable.
 */
template<typename CellKey>
using MetricLookup = std::function<
    std::optional<double>(const CellKey&,
                          std::string_view algorithm,
                          std::string_view metric)>;

/// Function returning the human-readable component labels for a CellKey.
/// Used both by the JSON emitter (per-violation cell_labels) and the
/// console table (column headers + per-row values).
template<typename CellKey>
using CellLabelFn = std::function<std::vector<CellLabel>(const CellKey&)>;

// ---------------------------------------------------------------------
// Hard-coded fallback config — used when the threshold file is missing
// or unparseable. The aligner bench's `regression_glue.cppm` builds the
// real defaults; this is just a no-op that keeps the engine functional
// in tests and on stripped checkouts.
// ---------------------------------------------------------------------

[[nodiscard]] inline Config empty_config() {
    Config cfg;
    cfg.tolerance_pct = 2.0;
    return cfg;
}

// ---------------------------------------------------------------------
// JSON parsing (simdjson DOM)
// ---------------------------------------------------------------------

namespace detail {

/// Slurp a file into a `simdjson::padded_string`. Returns nullopt on
/// I/O failure; the caller logs and falls back to defaults.
[[nodiscard]] inline std::optional<simdjson::padded_string> load_padded(
    const std::string& path)
{
    auto loaded = simdjson::padded_string::load(path);
    if (loaded.error() != simdjson::SUCCESS) return std::nullopt;
    return std::optional<simdjson::padded_string>{std::move(loaded.value())};
}

/// Extract a string member, or the supplied fallback when missing.
[[nodiscard]] inline std::string string_or(
    simdjson::dom::element obj, std::string_view key, std::string_view fallback)
{
    std::string_view view;
    auto err = obj[key].get_string().get(view);
    if (err != simdjson::SUCCESS) return std::string{fallback};
    return std::string{view};
}

/// Extract a double member, or the supplied fallback. Tolerates int
/// representations (the JSON file uses `0` for unused thresholds).
[[nodiscard]] inline double double_or(
    simdjson::dom::element obj, std::string_view key, double fallback)
{
    double d = 0.0;
    if (obj[key].get_double().get(d) == simdjson::SUCCESS) return d;
    std::int64_t i = 0;
    if (obj[key].get_int64().get(i) == simdjson::SUCCESS) return static_cast<double>(i);
    return fallback;
}

/// Decide the compare kind from a metric name. Falls back to RELATIVE
/// (the most common choice) so a typo in the JSON's metric name
/// doesn't silently flip a rule's semantics.
///
/// This is *one* of two places where the engine has any built-in
/// knowledge of metric names — and it's only used as a default when
/// the JSON omits the explicit `compare` field. The schema lets the
/// user override it on every rule.
[[nodiscard]] inline CompareKind default_compare_for(std::string_view metric_name) noexcept {
    if (metric_name == "accuracy") return CompareKind::ABSOLUTE;
    return CompareKind::RELATIVE;
}

[[nodiscard]] inline bool extract_rule(
    simdjson::dom::element obj, Rule& out, bool is_wildcard)
{
    if (obj.type() != simdjson::dom::element_type::OBJECT) return false;
    out.algorithm_label  = string_or(obj, "algorithm",        "");
    out.algorithm_prefix = string_or(obj, "algorithm_prefix", "");
    if (out.algorithm_label.empty() && out.algorithm_prefix.empty()) return false;
    if (!is_wildcard && out.algorithm_label.empty()) return false;
    out.baseline_key = string_or(obj, "baseline", "default");
    out.metric       = string_or(obj, "metric",   "wall_clock");
    {
        const std::string kind_name = string_or(obj, "rule", "equal_to");
        if (auto k = parse_rule_kind(kind_name)) {
            out.kind = *k;
        } else {
            return false;
        }
    }
    {
        const std::string compare_name = string_or(obj, "compare", "");
        if (compare_name == "relative")      out.compare = CompareKind::RELATIVE;
        else if (compare_name == "absolute") out.compare = CompareKind::ABSOLUTE;
        else                                 out.compare = default_compare_for(out.metric);
    }
    out.value_pct = double_or(obj, "value_pct", 0.0);
    out.min_value = double_or(obj, "min_value", 0.0);
    out.comment   = string_or(obj, "comment",  "");
    return true;
}

}  // namespace detail

/**
 * @brief Parse `regression_thresholds.json` at `path`.
 *
 * Returns `empty_config()` on any failure. Logs the failure via
 * `JustLog` so the operator notices the run reverted to defaults.
 * The aligner bench provides its own production defaults via
 * `regression_glue.cppm`; this function returning an empty config
 * just means "the engine has nothing to enforce" — it is not a fatal
 * error.
 */
[[nodiscard]] inline Config load_config_from_file(const std::string& path) {
    auto padded = detail::load_padded(path);
    if (!padded) {
        JustLog("[bench.scheduler.regression] thresholds file not found at '{}' — using empty defaults",
                path);
        return empty_config();
    }
    simdjson::dom::parser parser;
    simdjson::dom::element root;
    if (parser.parse(*padded).get(root) != simdjson::SUCCESS) {
        JustLog("[bench.scheduler.regression] failed to parse '{}' — using empty defaults",
                path);
        return empty_config();
    }
    if (root.type() != simdjson::dom::element_type::OBJECT) {
        JustLog("[bench.scheduler.regression] '{}' top-level is not an object — using empty defaults",
                path);
        return empty_config();
    }

    Config cfg;
    cfg.tolerance_pct = detail::double_or(root, "tolerance_pct", 2.0);

    simdjson::dom::object baselines;
    if (root["baselines"].get_object().get(baselines) == simdjson::SUCCESS) {
        for (auto field : baselines) {
            std::string_view value;
            if (field.value.get_string().get(value) == simdjson::SUCCESS) {
                cfg.baselines.emplace(std::string{field.key}, std::string{value});
            }
        }
    }

    simdjson::dom::array rules;
    if (root["rules"].get_array().get(rules) == simdjson::SUCCESS) {
        for (auto entry : rules) {
            Rule rule;
            if (detail::extract_rule(entry, rule, /*is_wildcard=*/false)) {
                cfg.rules.push_back(std::move(rule));
            } else {
                JustLog("[bench.scheduler.regression] skipping malformed rule entry");
            }
        }
    }
    simdjson::dom::array wildcard_rules;
    if (root["wildcard_rules"].get_array().get(wildcard_rules) == simdjson::SUCCESS) {
        for (auto entry : wildcard_rules) {
            Rule rule;
            if (detail::extract_rule(entry, rule, /*is_wildcard=*/true)) {
                cfg.wildcard_rules.push_back(std::move(rule));
            } else {
                JustLog("[bench.scheduler.regression] skipping malformed wildcard rule entry");
            }
        }
    }
    JustLog("[bench.scheduler.regression] loaded {} explicit rule(s) and {} wildcard rule(s) from '{}' (tolerance {:.2f}%)",
            cfg.rules.size(), cfg.wildcard_rules.size(), path, cfg.tolerance_pct);
    return cfg;
}

// ---------------------------------------------------------------------
// Comparison primitives
// ---------------------------------------------------------------------

namespace detail {

/// Returns true if (rule, baseline_value, actual_value) is a violation
/// under RELATIVE semantics. Sign: `out_observed_pct > 0` means actual
/// is *faster / smaller* than baseline.
[[nodiscard]] inline bool is_violation_relative(
    const Rule& rule, double tolerance_pct,
    double baseline_value, double actual_value,
    double& out_observed_pct)
{
    if (baseline_value <= 0.0 || actual_value <= 0.0) {
        out_observed_pct = 0.0;
        return false;  // missing data — inconclusive, never flag
    }
    out_observed_pct = 100.0 * (baseline_value - actual_value) / baseline_value;
    if (rule.kind == RuleKind::EQUAL_TO) {
        return std::fabs(out_observed_pct) > tolerance_pct;
    }
    // FASTER_BY_AT_LEAST_PCT: pass when observed_pct >= value_pct - tolerance.
    return out_observed_pct < (rule.value_pct - tolerance_pct);
}

/// ABSOLUTE semantics — for metrics like accuracy fractions where a
/// 0 baseline cannot divide by zero.
[[nodiscard]] inline bool is_violation_absolute(
    const Rule& rule, double tolerance_pct,
    double baseline_value, double actual_value,
    double& out_observed_pct)
{
    out_observed_pct = 100.0 * (actual_value - baseline_value);
    // EQUAL_TO and FASTER_BY_AT_LEAST_PCT both collapse to the same
    // |delta| <= tolerance check on absolute metrics — the
    // "faster by at least" idiom doesn't have a meaningful equivalent
    // for an absolute fraction (nobody asks "is accuracy 5pp better").
    return std::fabs(out_observed_pct) > tolerance_pct;
}

/// Hard floor: violation when `actual_value < min_value * (1 - slack)`.
/// Sign convention for `out_observed_pct`: percent above (positive) /
/// below (negative) the floor, i.e. `(actual - min) / min * 100`.
///
/// The global `tolerance_pct` is calibrated for `EQUAL_TO` semantics
/// where 150% loosens the comparison to a 2.5× window — applied
/// directly to a hard floor it lets any positive value pass. Hard
/// floors instead use `min(tolerance_pct, 5)` so a small amount of
/// run-to-run noise (≤5%) doesn't fire the rule, but the floor
/// remains meaningful even under loose-EQUAL_TO configs.
[[nodiscard]] inline bool is_violation_min_absolute(
    const Rule& rule, double tolerance_pct,
    double actual_value, double& out_observed_pct)
{
    if (rule.min_value <= 0.0 || actual_value <= 0.0) {
        out_observed_pct = 0.0;
        return false;  // missing data — inconclusive, never flag
    }
    out_observed_pct = 100.0 * (actual_value - rule.min_value) / rule.min_value;
    constexpr double kMinAbsoluteMaxSlackPct = 5.0;
    const double effective_slack_pct =
        std::min(tolerance_pct, kMinAbsoluteMaxSlackPct);
    return actual_value < rule.min_value * (1.0 - effective_slack_pct / 100.0);
}

[[nodiscard]] inline bool is_violation(
    const Rule& rule, double tolerance_pct,
    double baseline_value, double actual_value,
    double& out_observed_pct)
{
    if (rule.kind == RuleKind::MIN_ABSOLUTE) {
        return is_violation_min_absolute(rule, tolerance_pct,
                                         actual_value, out_observed_pct);
    }
    if (rule.compare == CompareKind::RELATIVE) {
        return is_violation_relative(rule, tolerance_pct, baseline_value,
                                     actual_value, out_observed_pct);
    }
    return is_violation_absolute(rule, tolerance_pct, baseline_value,
                                 actual_value, out_observed_pct);
}

}  // namespace detail

// ---------------------------------------------------------------------
// Violation computation
// ---------------------------------------------------------------------

/**
 * @brief Walk every (rule × cell × algorithm) tuple and collect violations.
 *
 * The caller materialises the cell list once (typically by aggregating
 * its sample storage); the engine does not re-materialise it per rule.
 * Wildcard rules are skipped for any (algorithm, metric) pair that an
 * explicit rule already covers — explicit overrides take precedence.
 *
 * @tparam CellKey  Any equality-comparable, copyable type the caller
 *                  uses to index its cells.
 *
 * @param cells           Stable iteration order of cells. The output
 *                        violation order follows (rule registration
 *                        order × cell input order × matched-algorithm
 *                        order) so the report is reproducible.
 * @param algorithms      Set of every algorithm label that appears in
 *                        the cell data. Used to expand wildcard rules.
 * @param cfg             Loaded threshold config.
 * @param lookup          `(cell, algorithm, metric) -> std::optional<double>`.
 * @param cell_label_fn   `cell -> vector<CellLabel>`, populates the
 *                        `Violation::cell_labels` field.
 *
 * @return Violations in deterministic order.
 */
template<typename CellKey>
[[nodiscard]] inline std::vector<Violation> compute_violations(
    std::span<const CellKey> cells,
    std::span<const std::string> algorithms,
    const Config& cfg,
    const MetricLookup<CellKey>& lookup,
    const CellLabelFn<CellKey>& cell_label_fn)
{
    std::vector<Violation> out;

    auto resolve_baseline = [&](const std::string& key) -> std::string {
        auto it = cfg.baselines.find(key);
        return it != cfg.baselines.end() ? it->second : std::string{};
    };

    auto build_violation = [&](
        const Rule& rule,
        const std::string& algorithm_label,
        const std::string& baseline_label,
        const CellKey& cell,
        double baseline_value,
        double actual_value)
    {
        double observed_pct = 0.0;
        if (!detail::is_violation(rule, cfg.tolerance_pct,
                                  baseline_value, actual_value, observed_pct)) {
            return;
        }
        Violation v;
        v.algorithm_label = algorithm_label;
        v.baseline_label  = baseline_label;
        v.metric          = rule.metric;
        v.compare         = rule.compare;
        v.kind            = rule.kind;
        v.cell_labels     = cell_label_fn(cell);
        v.tolerance_pct   = cfg.tolerance_pct;
        v.threshold_pct   = rule.value_pct;
        v.baseline_value  = baseline_value;
        v.actual_value    = actual_value;
        v.observed_pct    = observed_pct;
        v.rule_comment    = rule.comment;
        out.push_back(std::move(v));
    };

    // ---- Explicit rules --------------------------------------------
    for (const auto& rule : cfg.rules) {
        if (rule.algorithm_label.empty()) continue;
        // MIN_ABSOLUTE rules have no baseline — they assert a hard
        // floor on the algorithm's own value. Everything else needs a
        // baseline lookup.
        if (rule.kind == RuleKind::MIN_ABSOLUTE) {
            for (const auto& cell : cells) {
                const auto algo_value = lookup(cell, rule.algorithm_label,
                                               rule.metric);
                if (!algo_value.has_value()) continue;
                // Pass `min_value` as the "baseline" so the table /
                // JSON columns render the floor next to the actual
                // value. is_violation_min_absolute ignores this value
                // (it reads `rule.min_value` directly).
                build_violation(rule, rule.algorithm_label,
                                std::string{"(min_absolute floor)"},
                                cell, /*baseline_value=*/rule.min_value,
                                *algo_value);
            }
            continue;
        }
        const std::string baseline_label = resolve_baseline(rule.baseline_key);
        if (baseline_label.empty()) {
            JustLog("[bench.scheduler.regression] rule for '{}' references unknown baseline '{}' — skipping",
                    rule.algorithm_label, rule.baseline_key);
            continue;
        }
        for (const auto& cell : cells) {
            const auto algo_value = lookup(cell, rule.algorithm_label, rule.metric);
            const auto base_value = lookup(cell, baseline_label,        rule.metric);
            if (!algo_value.has_value() || !base_value.has_value()) continue;
            build_violation(rule, rule.algorithm_label, baseline_label,
                            cell, *base_value, *algo_value);
        }
    }

    // ---- Wildcard rules --------------------------------------------
    auto explicit_pair_exists = [&](std::string_view algorithm_label,
                                    std::string_view metric) {
        for (const auto& r : cfg.rules) {
            if (r.metric == metric && r.algorithm_label == algorithm_label) return true;
        }
        return false;
    };

    for (const auto& rule : cfg.wildcard_rules) {
        if (rule.algorithm_prefix.empty()) continue;
        const std::string baseline_label = resolve_baseline(rule.baseline_key);
        if (baseline_label.empty()) {
            JustLog("[bench.scheduler.regression] wildcard rule references unknown baseline '{}' — skipping",
                    rule.baseline_key);
            continue;
        }
        for (const auto& algorithm_label : algorithms) {
            if (algorithm_label.rfind(rule.algorithm_prefix, 0) != 0) continue;
            if (explicit_pair_exists(algorithm_label, rule.metric)) continue;
            for (const auto& cell : cells) {
                const auto algo_value = lookup(cell, algorithm_label, rule.metric);
                const auto base_value = lookup(cell, baseline_label,  rule.metric);
                if (!algo_value.has_value() || !base_value.has_value()) continue;
                build_violation(rule, algorithm_label, baseline_label,
                                cell, *base_value, *algo_value);
            }
        }
    }

    return out;
}

// ---------------------------------------------------------------------
// JSON emitter — single canonical schema (no "legacy" fork)
// ---------------------------------------------------------------------

/**
 * @brief Emit `"violations": [...]` under the writer's current cursor.
 *
 * The schema:
 *
 *     {"algorithm":     "<algorithm_label>",
 *      "baseline":      "<baseline_label>",
 *      "metric":        "<metric>",
 *      "rule":          "<rule_kind_name>",
 *      "compare":       "<compare_kind_name>",
 *      "<cell_label_name_0>": "<cell_label_value_0>",
 *      "<cell_label_name_1>": "<cell_label_value_1>",
 *      ...,
 *      "tolerance_pct":  ...,
 *      "threshold_pct":  ...,
 *      "baseline_value": ...,
 *      "actual_value":   ...,
 *      "observed_pct":   ...,
 *      "comment":        "..."}
 *
 * The aligner glue passes `length_bucket` and `error_bucket` as the
 * cell-label component names, which produces a per-violation shape
 * byte-equivalent to the pre-refactor schema. Other domains pick the
 * names they want; the engine doesn't care.
 */
inline void emit_violations_json(JsonWriter& writer,
                                 std::span<const Violation> violations) {
    writer.key("violations");
    writer.begin_array();
    for (const auto& v : violations) {
        writer.begin_object();
        writer.kv_string("algorithm",     v.algorithm_label);
        writer.kv_string("baseline",      v.baseline_label);
        writer.kv_string("metric",        v.metric);
        writer.kv_string("rule",          rule_kind_name(v.kind));
        writer.kv_string("compare",       compare_kind_name(v.compare));
        for (const auto& label : v.cell_labels) {
            writer.kv_string(label.name, label.value);
        }
        writer.kv_double("tolerance_pct",  v.tolerance_pct);
        writer.kv_double("threshold_pct",  v.threshold_pct);
        writer.kv_double("baseline_value", v.baseline_value);
        writer.kv_double("actual_value",   v.actual_value);
        writer.kv_double("observed_pct",   v.observed_pct);
        writer.kv_string("comment",        v.rule_comment);
        writer.end_object();
    }
    writer.end_array();
}

// ---------------------------------------------------------------------
// Console table
// ---------------------------------------------------------------------

namespace detail {

[[nodiscard]] inline std::string truncate_to_width(std::string_view s, std::size_t width) {
    if (s.size() <= width) return std::string{s};
    if (width <= 3) return std::string(width, '.');
    std::string out;
    out.reserve(width);
    out.append(s.substr(0, width - 3));
    out.append("...");
    return out;
}

[[nodiscard]] inline std::string format_metric_value(
    CompareKind compare, RuleKind kind, double value) noexcept {
    if (kind == RuleKind::MIN_ABSOLUTE) {
        // MIN_ABSOLUTE values are in the metric's native unit (e.g.
        // MiB/s for throughput floors). The engine doesn't know the
        // unit, so render the raw number — the rule's `comment`
        // documents the unit for the human reader.
        return fmt::format("{:.2f}", value);
    }
    if (compare == CompareKind::RELATIVE) {
        // For relative comparisons the metric is most often a wall-clock
        // duration in nanoseconds; render it as milliseconds so columns
        // stay legible. Other relative metrics (throughput, memory)
        // would render less cleanly here, but the bench's primary
        // RELATIVE metric is wall-clock so this is the right default.
        return fmt::format("{:.3f} ms", value / 1.0e6);
    }
    // Absolute: the metric is in [0, 1]. Render as percent so the
    // column reads naturally next to the percentage-based observed_pct.
    return fmt::format("{:.2f}%", value * 100.0);
}

[[nodiscard]] inline std::string format_rule_summary(const Violation& v) noexcept {
    if (v.kind == RuleKind::FASTER_BY_AT_LEAST_PCT) {
        return fmt::format(">= {:.1f}% faster (tol {:.1f}%)",
                           v.threshold_pct, v.tolerance_pct);
    }
    if (v.kind == RuleKind::MIN_ABSOLUTE) {
        // Match `is_violation_min_absolute`'s clamp: the global
        // tolerance can be too loose for a hard floor, so the engine
        // caps it at 5% internally. Show the same effective number
        // here so the table matches the rule that actually fired.
        const double effective_tol_pct =
            std::min(v.tolerance_pct, 5.0);
        return fmt::format(">= {:.2f} floor (tol {:.1f}%)",
                           v.baseline_value, effective_tol_pct);
    }
    return fmt::format("equal to baseline (tol {:.1f}%)", v.tolerance_pct);
}

}  // namespace detail

/**
 * @brief Pretty-print violations as a fixed-width table on stderr.
 *
 * The cell-label columns are inferred from the first violation's
 * `cell_labels` vector — every violation in a run shares the same set
 * of cell-label component names because the caller's `cell_label_fn`
 * is consistent. We deliberately use stderr (not JustLog) so the table
 * renders synchronously next to the regular bench output; fmtlog's
 * async queue would re-order this against the Catch2 summary.
 */
inline void print_violations_table(std::span<const Violation> violations) {
    if (violations.empty()) {
        std::fprintf(stderr, "\n[bench.scheduler.regression] all regression rules passed.\n");
        std::fflush(stderr);
        return;
    }

    constexpr std::size_t metric_w   = 12;
    constexpr std::size_t algo_w     = 36;
    constexpr std::size_t baseline_w = 26;
    constexpr std::size_t cell_w     = 22;
    constexpr std::size_t rule_w     = 28;
    constexpr std::size_t basev_w    = 12;
    constexpr std::size_t actv_w     = 12;
    constexpr std::size_t obs_w      = 9;

    // Collect cell-label column names from the first violation. Every
    // violation in this run uses the same `cell_label_fn`, so the
    // names are stable across the table.
    const auto& first = violations.front();
    const auto cell_label_count = first.cell_labels.size();

    auto print_separator = [&]() {
        std::string line;
        line.append(metric_w + 2,   '-'); line.append("+");
        line.append(algo_w + 2,     '-'); line.append("+");
        line.append(baseline_w + 2, '-'); line.append("+");
        for (std::size_t i = 0; i < cell_label_count; ++i) {
            line.append(cell_w + 2, '-'); line.append("+");
        }
        line.append(rule_w + 2,     '-'); line.append("+");
        line.append(basev_w + 2,    '-'); line.append("+");
        line.append(actv_w + 2,     '-'); line.append("+");
        line.append(obs_w + 2,      '-');
        std::fprintf(stderr, "%s\n", line.c_str());
    };

    std::fprintf(stderr, "\n[bench.scheduler.regression] %zu regression rule(s) violated:\n",
                 violations.size());
    print_separator();
    std::fprintf(stderr, " %-*s | %-*s | %-*s",
                 static_cast<int>(metric_w),   "metric",
                 static_cast<int>(algo_w),     "algorithm",
                 static_cast<int>(baseline_w), "baseline");
    for (const auto& cell : first.cell_labels) {
        std::fprintf(stderr, " | %-*s",
                     static_cast<int>(cell_w),
                     detail::truncate_to_width(cell.name, cell_w).c_str());
    }
    std::fprintf(stderr, " | %-*s | %*s | %*s | %*s\n",
                 static_cast<int>(rule_w),  "rule",
                 static_cast<int>(basev_w), "baseline",
                 static_cast<int>(actv_w),  "actual",
                 static_cast<int>(obs_w),   "obs%");
    print_separator();
    for (const auto& v : violations) {
        const std::string baseline_v = detail::format_metric_value(
            v.compare, v.kind, v.baseline_value);
        const std::string actual_v   = detail::format_metric_value(
            v.compare, v.kind, v.actual_value);
        const std::string obs_pct    = fmt::format("{:+.2f}", v.observed_pct);
        const std::string rule_summary = detail::format_rule_summary(v);
        std::fprintf(stderr, " %-*s | %-*s | %-*s",
                     static_cast<int>(metric_w),
                     detail::truncate_to_width(v.metric, metric_w).c_str(),
                     static_cast<int>(algo_w),
                     detail::truncate_to_width(v.algorithm_label, algo_w).c_str(),
                     static_cast<int>(baseline_w),
                     detail::truncate_to_width(v.baseline_label, baseline_w).c_str());
        // Pad missing cell labels with empty strings so the table
        // stays rectangular even when a stray violation has fewer
        // labels than the rest (shouldn't happen in normal use).
        for (std::size_t i = 0; i < cell_label_count; ++i) {
            const std::string val = i < v.cell_labels.size()
                ? v.cell_labels[i].value : std::string{};
            std::fprintf(stderr, " | %-*s",
                         static_cast<int>(cell_w),
                         detail::truncate_to_width(val, cell_w).c_str());
        }
        std::fprintf(stderr, " | %-*s | %*s | %*s | %*s\n",
                     static_cast<int>(rule_w),
                     detail::truncate_to_width(rule_summary, rule_w).c_str(),
                     static_cast<int>(basev_w), baseline_v.c_str(),
                     static_cast<int>(actv_w),  actual_v.c_str(),
                     static_cast<int>(obs_w),   obs_pct.c_str());
    }
    print_separator();
    std::fprintf(stderr,
        "\n  obs%% = (baseline - actual)/baseline * 100 for relative metrics (positive = faster/less)\n"
        "         = (actual - baseline) * 100        for absolute metrics (positive = better)\n");
    std::fflush(stderr);
}

}  // namespace bioseq::bench_scheduler::regression

}  // export
