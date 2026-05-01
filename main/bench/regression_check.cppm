/**
 * @file regression_check.cppm
 * @brief Post-bench regression-rule enforcement.
 *
 * Loads `regression_thresholds.json` (see `main/bench/regression_thresholds.json`),
 * walks the per-cell aggregates produced by `bench.matrix_runner`, computes
 * the rule violations and emits two artefacts:
 *
 *   1. A console table on stderr listing every (rule, length_bucket,
 *      error_bucket) combination that failed, with baseline / actual /
 *      tolerance numbers.
 *   2. A `violations` array embedded in `report.json` with the same
 *      information in machine-readable form so dashboards / CI can pivot
 *      on it.
 *
 * Rule shapes (see schema in `regression_thresholds.json`):
 *
 *   - `wall_clock` + `equal_to`: |actual_mean_ns - baseline_mean_ns| /
 *     baseline_mean_ns must be <= tolerance_pct/100.
 *   - `wall_clock` + `faster_by_at_least_pct`: actual_mean_ns must be
 *     <= baseline_mean_ns * (1 - (value_pct - tolerance_pct)/100).
 *     I.e. the speed-up window is `[value_pct - tolerance, +inf)`.
 *   - `accuracy` + `equal_to`: |actual_correct_frac - baseline_correct_frac|
 *     must be <= tolerance_pct/100. Accuracy values are fractions in
 *     [0,1] so the tolerance is an absolute distance, not a relative
 *     percentage of the baseline (a 0% baseline accuracy with a 0%
 *     algorithm accuracy is "exactly equal" — the relative form would
 *     divide by zero).
 *
 * The JSON parser is hand-rolled to keep the bench harness dependency-
 * free (the project lists simdjson in vcpkg but doesn't wire it into
 * the bench target — see `main/bench/json_writer.cppm` for the same
 * reasoning on the writer side). The schema is shallow so a 200-line
 * recursive-descent parser is plenty.
 */
module;

#include "common.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

export module bench.regression_check;

import mlogging;
import bench.matrix_runner;
import bench.json_writer;

export {

namespace bioseq::bench {

// ---------------------------------------------------------------------
// Data types
// ---------------------------------------------------------------------

enum class RuleMetric : std::uint8_t {
    WALL_CLOCK = 0,
    ACCURACY   = 1,
};

[[nodiscard]] constexpr const char* rule_metric_name(RuleMetric metric) noexcept {
    switch (metric) {
        case RuleMetric::WALL_CLOCK: return "wall_clock";
        case RuleMetric::ACCURACY:   return "accuracy";
    }
    return "?";
}

enum class RuleKind : std::uint8_t {
    EQUAL_TO              = 0,
    FASTER_BY_AT_LEAST_PCT = 1,
};

[[nodiscard]] constexpr const char* rule_kind_name(RuleKind kind) noexcept {
    switch (kind) {
        case RuleKind::EQUAL_TO:               return "equal_to";
        case RuleKind::FASTER_BY_AT_LEAST_PCT: return "faster_by_at_least_pct";
    }
    return "?";
}

/**
 * @brief One regression rule loaded from the JSON config.
 *
 * Either `algorithm_label` (exact match) or `algorithm_prefix` (prefix
 * match — wildcard rule applied to every algorithm whose label starts
 * with this string and is not already covered by an exact rule with the
 * same metric) is non-empty; never both. The baseline is identified by
 * a logical key that maps via `RegressionConfig::baselines` to an
 * algorithm label, so the user can rename "minimap2 (DP chaining)" in
 * one place and have every rule pick up the change.
 */
struct RegressionRule {
    std::string algorithm_label;   ///< exact match against `BenchSample::algorithm_label`
    std::string algorithm_prefix;  ///< prefix wildcard (used iff `algorithm_label` is empty)
    std::string baseline_key;      ///< key into `RegressionConfig::baselines`
    RuleMetric  metric          = RuleMetric::WALL_CLOCK;
    RuleKind    kind            = RuleKind::EQUAL_TO;
    double      value_pct       = 0.0;  ///< only meaningful for FASTER_BY_AT_LEAST_PCT
    std::string comment;
};

/// Top-level JSON document parsed into POD form.
struct RegressionConfig {
    double tolerance_pct = 2.0;
    std::map<std::string, std::string> baselines;
    std::vector<RegressionRule> rules;
    std::vector<RegressionRule> wildcard_rules;
};

/**
 * @brief One concrete (rule × cell) violation.
 *
 * Carries enough context that the JSON consumer / console reader can
 * tell *exactly* what failed without cross-referencing back into the
 * config: which algorithm, which baseline, which dataset, what number
 * was observed and what the rule expected.
 */
struct RegressionViolation {
    std::string algorithm_label;      ///< the algorithm that broke the rule
    std::string baseline_label;       ///< the algorithm we compared against
    RuleMetric  metric          = RuleMetric::WALL_CLOCK;
    RuleKind    kind            = RuleKind::EQUAL_TO;
    std::string length_bucket_label; ///< e.g. "long (10k-1M)"
    std::string error_bucket_label;  ///< e.g. "ccs (~1.5%)"
    double      tolerance_pct   = 0.0;
    double      threshold_pct   = 0.0;  ///< rule's value_pct (0 for EQUAL_TO)
    double      baseline_value  = 0.0;  ///< baseline mean_ns (WALL_CLOCK) or correct_frac (ACCURACY)
    double      actual_value    = 0.0;  ///< observed mean_ns / correct_frac
    double      observed_pct    = 0.0;  ///< speed-up % (positive = faster) or accuracy delta-pct
    std::string rule_comment;           ///< human-readable rule description from JSON
};

// ---------------------------------------------------------------------
// JSON parser (hand-rolled, minimal — schema is shallow)
//
// Only the subset needed for `regression_thresholds.json` is supported:
// objects, arrays, strings (no \u escapes beyond the basic backslash
// escapes the writer emits), doubles, ints, true/false, null. Comments
// (//, /* */) are NOT accepted — keep the config strict-JSON so editors
// validate it without a custom mode. Keys starting with `$` are simply
// ignored, which lets the file include `$schema_comment` style annotations.
// ---------------------------------------------------------------------

namespace detail {

class JsonValue {
public:
    using Object = std::vector<std::pair<std::string, JsonValue>>;
    using Array  = std::vector<JsonValue>;

    enum class Kind { NUL, BOOL, NUMBER, STRING, ARRAY, OBJECT };

    Kind kind = Kind::NUL;
    bool bool_value = false;
    double number_value = 0.0;
    std::string string_value;
    Array array_value;
    Object object_value;

    [[nodiscard]] bool is_object() const noexcept { return kind == Kind::OBJECT; }
    [[nodiscard]] bool is_array()  const noexcept { return kind == Kind::ARRAY; }
    [[nodiscard]] bool is_string() const noexcept { return kind == Kind::STRING; }
    [[nodiscard]] bool is_number() const noexcept { return kind == Kind::NUMBER; }
    [[nodiscard]] bool is_bool()   const noexcept { return kind == Kind::BOOL; }

    [[nodiscard]] const JsonValue* find(std::string_view key) const {
        if (kind != Kind::OBJECT) return nullptr;
        for (const auto& [k, v] : object_value) {
            if (k == key) return &v;
        }
        return nullptr;
    }

    [[nodiscard]] std::string get_string_or(std::string_view fallback) const {
        return is_string() ? string_value : std::string{fallback};
    }
    [[nodiscard]] double get_number_or(double fallback) const noexcept {
        return is_number() ? number_value : fallback;
    }
};

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : text_{text} {}

    [[nodiscard]] bool parse(JsonValue& out) {
        skip_ws();
        if (!parse_value(out)) return false;
        skip_ws();
        return pos_ >= text_.size();
    }

    [[nodiscard]] const std::string& error() const noexcept { return error_; }

private:
    [[nodiscard]] bool parse_value(JsonValue& out) {
        skip_ws();
        if (pos_ >= text_.size()) return fail("unexpected end of input");
        const char c = text_[pos_];
        if (c == '{') return parse_object(out);
        if (c == '[') return parse_array(out);
        if (c == '"') {
            out.kind = JsonValue::Kind::STRING;
            return parse_string(out.string_value);
        }
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number(out);
        if (c == 't' || c == 'f') return parse_bool(out);
        if (c == 'n') return parse_null(out);
        return fail("unexpected character at value start");
    }

    [[nodiscard]] bool parse_object(JsonValue& out) {
        out.kind = JsonValue::Kind::OBJECT;
        out.object_value.clear();
        ++pos_;  // consume '{'
        skip_ws();
        if (peek() == '}') { ++pos_; return true; }
        while (pos_ < text_.size()) {
            skip_ws();
            if (peek() != '"') return fail("expected string key");
            std::string key;
            if (!parse_string(key)) return false;
            skip_ws();
            if (peek() != ':') return fail("expected ':' after key");
            ++pos_;
            JsonValue value;
            if (!parse_value(value)) return false;
            out.object_value.emplace_back(std::move(key), std::move(value));
            skip_ws();
            if (peek() == ',') { ++pos_; continue; }
            if (peek() == '}') { ++pos_; return true; }
            return fail("expected ',' or '}' in object");
        }
        return fail("unterminated object");
    }

    [[nodiscard]] bool parse_array(JsonValue& out) {
        out.kind = JsonValue::Kind::ARRAY;
        out.array_value.clear();
        ++pos_;  // consume '['
        skip_ws();
        if (peek() == ']') { ++pos_; return true; }
        while (pos_ < text_.size()) {
            JsonValue value;
            if (!parse_value(value)) return false;
            out.array_value.push_back(std::move(value));
            skip_ws();
            if (peek() == ',') { ++pos_; continue; }
            if (peek() == ']') { ++pos_; return true; }
            return fail("expected ',' or ']' in array");
        }
        return fail("unterminated array");
    }

    [[nodiscard]] bool parse_string(std::string& out) {
        if (peek() != '"') return fail("expected string");
        ++pos_;
        out.clear();
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == '"') { ++pos_; return true; }
            if (c == '\\') {
                ++pos_;
                if (pos_ >= text_.size()) return fail("unterminated escape");
                const char e = text_[pos_++];
                switch (e) {
                    case '"':  out.push_back('"');  break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/');  break;
                    case 'b':  out.push_back('\b'); break;
                    case 'f':  out.push_back('\f'); break;
                    case 'n':  out.push_back('\n'); break;
                    case 'r':  out.push_back('\r'); break;
                    case 't':  out.push_back('\t'); break;
                    default:   return fail("unsupported escape");
                }
                continue;
            }
            out.push_back(c);
            ++pos_;
        }
        return fail("unterminated string");
    }

    [[nodiscard]] bool parse_number(JsonValue& out) {
        const std::size_t start = pos_;
        if (peek() == '-') ++pos_;
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                ++pos_;
                continue;
            }
            break;
        }
        const std::string_view slice = text_.substr(start, pos_ - start);
        char* endp = nullptr;
        const std::string holder{slice};
        const double v = std::strtod(holder.c_str(), &endp);
        if (endp == holder.c_str()) return fail("malformed number");
        out.kind = JsonValue::Kind::NUMBER;
        out.number_value = v;
        return true;
    }

    [[nodiscard]] bool parse_bool(JsonValue& out) {
        if (text_.substr(pos_, 4) == "true") {
            pos_ += 4; out.kind = JsonValue::Kind::BOOL; out.bool_value = true; return true;
        }
        if (text_.substr(pos_, 5) == "false") {
            pos_ += 5; out.kind = JsonValue::Kind::BOOL; out.bool_value = false; return true;
        }
        return fail("expected 'true' or 'false'");
    }

    [[nodiscard]] bool parse_null(JsonValue& out) {
        if (text_.substr(pos_, 4) == "null") {
            pos_ += 4; out.kind = JsonValue::Kind::NUL; return true;
        }
        return fail("expected 'null'");
    }

    void skip_ws() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++pos_; continue; }
            break;
        }
    }

    [[nodiscard]] char peek() const noexcept {
        return pos_ < text_.size() ? text_[pos_] : '\0';
    }

    [[nodiscard]] bool fail(std::string_view msg) {
        if (error_.empty()) {
            error_ = fmt::format("{} at position {}", msg, pos_);
        }
        return false;
    }

    std::string_view text_;
    std::size_t pos_ = 0;
    std::string error_;
};

[[nodiscard]] inline std::optional<RuleMetric> parse_metric(std::string_view name) noexcept {
    if (name == "wall_clock") return RuleMetric::WALL_CLOCK;
    if (name == "accuracy")   return RuleMetric::ACCURACY;
    return std::nullopt;
}

[[nodiscard]] inline std::optional<RuleKind> parse_kind(std::string_view name) noexcept {
    if (name == "equal_to")               return RuleKind::EQUAL_TO;
    if (name == "faster_by_at_least_pct") return RuleKind::FASTER_BY_AT_LEAST_PCT;
    return std::nullopt;
}

[[nodiscard]] inline bool extract_rule(const JsonValue& obj, RegressionRule& out, bool is_wildcard) {
    if (!obj.is_object()) return false;
    if (const auto* a = obj.find("algorithm")) {
        if (a->is_string()) out.algorithm_label = a->string_value;
    }
    if (const auto* a = obj.find("algorithm_prefix")) {
        if (a->is_string()) out.algorithm_prefix = a->string_value;
    }
    if (out.algorithm_label.empty() && out.algorithm_prefix.empty()) {
        return false;
    }
    if (!is_wildcard && out.algorithm_label.empty()) {
        // Non-wildcard rule must have an exact label.
        return false;
    }
    if (const auto* b = obj.find("baseline")) {
        if (b->is_string()) out.baseline_key = b->string_value;
    }
    if (out.baseline_key.empty()) out.baseline_key = "default";
    if (const auto* m = obj.find("metric")) {
        if (m->is_string()) {
            if (auto kind = parse_metric(m->string_value)) out.metric = *kind;
            else return false;
        }
    }
    if (const auto* k = obj.find("rule")) {
        if (k->is_string()) {
            if (auto kind = parse_kind(k->string_value)) out.kind = *kind;
            else return false;
        }
    }
    if (const auto* v = obj.find("value_pct")) {
        out.value_pct = v->get_number_or(0.0);
    }
    if (const auto* c = obj.find("comment")) {
        if (c->is_string()) out.comment = c->string_value;
    }
    return true;
}

}  // namespace detail

// ---------------------------------------------------------------------
// Config loading
// ---------------------------------------------------------------------

/**
 * @brief Hard-coded fallback config used when the threshold JSON is
 *        missing or unparseable. Mirrors the schema that ships in
 *        `main/bench/regression_thresholds.json` so the bench still
 *        enforces the production rules even on a stripped checkout.
 */
[[nodiscard]] inline RegressionConfig default_config() {
    RegressionConfig cfg;
    cfg.tolerance_pct = 2.0;
    cfg.baselines["default"] = "minimap2 (DP chaining)";
    cfg.baselines["rmq"]     = "minimap2 (RMQ chaining)";

    auto add_rule = [&](
        std::string_view algorithm,
        std::string_view baseline_key,
        RuleMetric metric,
        RuleKind kind,
        double value_pct,
        std::string_view comment)
    {
        RegressionRule r;
        r.algorithm_label = std::string{algorithm};
        r.baseline_key    = std::string{baseline_key};
        r.metric          = metric;
        r.kind            = kind;
        r.value_pct       = value_pct;
        r.comment         = std::string{comment};
        cfg.rules.push_back(std::move(r));
    };

    add_rule("bioseq KSW2 + SIMD-DP + two-stack-nthash-mm", "default",
             RuleMetric::WALL_CLOCK, RuleKind::FASTER_BY_AT_LEAST_PCT, 5.0,
             "Production SIMD-DP minimizer must be >= 5% faster than minimap2 DP.");
    add_rule("bioseq Block + SIMD-DP + two-stack-nthash-mm", "default",
             RuleMetric::WALL_CLOCK, RuleKind::FASTER_BY_AT_LEAST_PCT, 8.0,
             "Block aligner SIMD-DP path must be >= 8% faster than minimap2 DP.");
    add_rule("bioseq KSW2 + scalar-DP + scalar-mm", "default",
             RuleMetric::WALL_CLOCK, RuleKind::EQUAL_TO, 0.0,
             "Scalar reference path must match minimap2 DP wall-clock.");
    add_rule("bioseq KSW2 + RMQ + scalar-mm", "rmq",
             RuleMetric::WALL_CLOCK, RuleKind::EQUAL_TO, 0.0,
             "RMQ chaining path must match minimap2 RMQ wall-clock.");
    add_rule("bioseq KSW2 + SIMD-DP + two-stack-nthash-mm", "default",
             RuleMetric::ACCURACY, RuleKind::EQUAL_TO, 0.0,
             "Production SIMD path must match minimap2 DP position accuracy.");
    add_rule("bioseq Block + SIMD-DP + two-stack-nthash-mm", "default",
             RuleMetric::ACCURACY, RuleKind::EQUAL_TO, 0.0,
             "Block aligner SIMD path must match minimap2 DP position accuracy.");

    {
        RegressionRule wildcard;
        wildcard.algorithm_prefix = "bioseq ";
        wildcard.baseline_key     = "default";
        wildcard.metric           = RuleMetric::ACCURACY;
        wildcard.kind             = RuleKind::EQUAL_TO;
        wildcard.value_pct        = 0.0;
        wildcard.comment          =
            "All bioseq algorithms must match minimap2 DP position accuracy unless overridden.";
        cfg.wildcard_rules.push_back(std::move(wildcard));
    }
    return cfg;
}

/**
 * @brief Try to parse `regression_thresholds.json` at the given path.
 *
 * Returns `default_config()` on any failure (missing file, parse error,
 * malformed schema) so the bench keeps enforcing the production rules
 * even when the JSON is broken. The caller should still surface the
 * failure via `JustLog` so the user notices.
 */
[[nodiscard]] inline RegressionConfig load_config_from_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        JustLog("[bench.regression_check] thresholds file not found at '{}' — using built-in defaults",
                path);
        return default_config();
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string text = buffer.str();
    detail::JsonValue root;
    detail::JsonParser parser{text};
    if (!parser.parse(root) || !root.is_object()) {
        JustLog("[bench.regression_check] failed to parse '{}': {} — using built-in defaults",
                path, parser.error().empty() ? "schema must be an object" : parser.error());
        return default_config();
    }

    RegressionConfig cfg;
    cfg.tolerance_pct = 2.0;
    if (const auto* tol = root.find("tolerance_pct")) {
        cfg.tolerance_pct = tol->get_number_or(2.0);
    }
    if (const auto* baselines = root.find("baselines"); baselines && baselines->is_object()) {
        for (const auto& [k, v] : baselines->object_value) {
            if (v.is_string()) cfg.baselines[k] = v.string_value;
        }
    }
    if (cfg.baselines.find("default") == cfg.baselines.end()) {
        cfg.baselines["default"] = "minimap2 (DP chaining)";
    }
    if (cfg.baselines.find("rmq") == cfg.baselines.end()) {
        cfg.baselines["rmq"] = "minimap2 (RMQ chaining)";
    }
    if (const auto* rules = root.find("rules"); rules && rules->is_array()) {
        for (const auto& r : rules->array_value) {
            RegressionRule rule;
            if (detail::extract_rule(r, rule, /*is_wildcard=*/false)) {
                cfg.rules.push_back(std::move(rule));
            } else {
                JustLog("[bench.regression_check] skipping malformed rule entry");
            }
        }
    }
    if (const auto* w = root.find("wildcard_rules"); w && w->is_array()) {
        for (const auto& r : w->array_value) {
            RegressionRule rule;
            if (detail::extract_rule(r, rule, /*is_wildcard=*/true)) {
                cfg.wildcard_rules.push_back(std::move(rule));
            } else {
                JustLog("[bench.regression_check] skipping malformed wildcard rule entry");
            }
        }
    }
    JustLog("[bench.regression_check] loaded {} explicit rule(s) and {} wildcard rule(s) from '{}' (tolerance {:.2f}%)",
            cfg.rules.size(), cfg.wildcard_rules.size(), path, cfg.tolerance_pct);
    return cfg;
}

// ---------------------------------------------------------------------
// Cell aggregation + violation computation
// ---------------------------------------------------------------------

namespace detail {

struct CellStats {
    double mean_ns_sum   = 0.0;   ///< sum of per-record mean latencies (ns)
    std::size_t mean_count = 0;
    std::size_t total_pairs = 0;
    std::size_t correct_pairs = 0;
};

using BucketKey = std::pair<int /*length*/, int /*error*/>;

[[nodiscard]] inline std::map<BucketKey, std::map<std::string, CellStats>>
aggregate_cells(const std::vector<BenchSample>& samples)
{
    std::map<BucketKey, std::map<std::string, CellStats>> out;
    for (const auto& s : samples) {
        const BucketKey key{
            static_cast<int>(s.length_bucket),
            static_cast<int>(s.error_bucket)};
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

[[nodiscard]] inline double cell_mean_ns(const CellStats& cell) noexcept {
    return cell.mean_count > 0 ? cell.mean_ns_sum / static_cast<double>(cell.mean_count) : 0.0;
}

[[nodiscard]] inline double cell_correct_frac(const CellStats& cell) noexcept {
    return cell.total_pairs > 0
        ? static_cast<double>(cell.correct_pairs) / static_cast<double>(cell.total_pairs)
        : 0.0;
}

[[nodiscard]] inline std::pair<std::string, std::string> bucket_labels_for(
    const std::vector<BenchSample>& samples,
    const BucketKey& key)
{
    for (const auto& s : samples) {
        if (static_cast<int>(s.length_bucket) == key.first &&
            static_cast<int>(s.error_bucket)  == key.second) {
            return {s.length_bucket_label, s.error_bucket_label};
        }
    }
    return {std::string{"length=?"}, std::string{"error=?"}};
}

[[nodiscard]] inline bool is_violation_wall_clock(
    const RegressionRule& rule,
    double tolerance_pct,
    double baseline_ns,
    double actual_ns,
    double& observed_pct)
{
    if (baseline_ns <= 0.0 || actual_ns <= 0.0) {
        observed_pct = 0.0;
        return false;  // missing data — treat as inconclusive, do not flag
    }
    // observed_pct = 100 * (baseline - actual) / baseline → positive when actual is faster.
    observed_pct = 100.0 * (baseline_ns - actual_ns) / baseline_ns;
    if (rule.kind == RuleKind::EQUAL_TO) {
        return std::fabs(observed_pct) > tolerance_pct;
    }
    // FASTER_BY_AT_LEAST_PCT: pass when observed_pct >= value_pct - tolerance.
    return observed_pct < (rule.value_pct - tolerance_pct);
}

[[nodiscard]] inline bool is_violation_accuracy(
    const RegressionRule& rule,
    double tolerance_pct,
    double baseline_frac,
    double actual_frac,
    double& observed_pct)
{
    // Accuracy is in [0, 1]; the JSON tolerance is interpreted as an
    // absolute percentage of correctness, not a relative deviation, so
    // a baseline of 0.0 (entire bucket failed) doesn't divide-by-zero.
    observed_pct = 100.0 * (actual_frac - baseline_frac);
    if (rule.kind == RuleKind::EQUAL_TO) {
        return std::fabs(observed_pct) > tolerance_pct;
    }
    // No FASTER_BY_AT_LEAST for accuracy — treat as EQUAL_TO if seen.
    return std::fabs(observed_pct) > tolerance_pct;
}

}  // namespace detail

/**
 * @brief Materialise the violation list.
 *
 * Walks every (rule × cell) and every wildcard-rule × algorithm × cell
 * pair. Wildcard rules skip algorithms that already have an explicit
 * rule for the same metric (so explicit overrides take precedence).
 * Cells where the baseline algorithm has no data are silently skipped —
 * a missing baseline is not a violation, just an incomplete dataset.
 */
[[nodiscard]] inline std::vector<RegressionViolation> compute_violations(
    const std::vector<BenchSample>& samples,
    const RegressionConfig& config)
{
    std::vector<RegressionViolation> out;
    if (samples.empty()) return out;

    const auto cells = detail::aggregate_cells(samples);

    auto resolve_baseline = [&](const std::string& key) -> std::string {
        auto it = config.baselines.find(key);
        return it != config.baselines.end() ? it->second : std::string{};
    };

    auto build_violation = [&](
        const RegressionRule& rule,
        const std::string& algorithm_label,
        const std::string& baseline_label,
        const detail::BucketKey& bucket_key,
        const detail::CellStats& algorithm_cell,
        const detail::CellStats& baseline_cell)
    {
        const auto [length_label, error_label] =
            detail::bucket_labels_for(samples, bucket_key);
        RegressionViolation v;
        v.algorithm_label = algorithm_label;
        v.baseline_label  = baseline_label;
        v.metric          = rule.metric;
        v.kind            = rule.kind;
        v.length_bucket_label = length_label;
        v.error_bucket_label  = error_label;
        v.tolerance_pct   = config.tolerance_pct;
        v.threshold_pct   = rule.value_pct;
        v.rule_comment    = rule.comment;
        if (rule.metric == RuleMetric::WALL_CLOCK) {
            const double baseline_ns = detail::cell_mean_ns(baseline_cell);
            const double actual_ns   = detail::cell_mean_ns(algorithm_cell);
            double observed_pct = 0.0;
            if (detail::is_violation_wall_clock(rule, config.tolerance_pct, baseline_ns, actual_ns, observed_pct)) {
                v.baseline_value = baseline_ns;
                v.actual_value   = actual_ns;
                v.observed_pct   = observed_pct;
                out.push_back(std::move(v));
            }
        } else {
            const double baseline_frac = detail::cell_correct_frac(baseline_cell);
            const double actual_frac   = detail::cell_correct_frac(algorithm_cell);
            double observed_pct = 0.0;
            if (detail::is_violation_accuracy(rule, config.tolerance_pct, baseline_frac, actual_frac, observed_pct)) {
                v.baseline_value = baseline_frac;
                v.actual_value   = actual_frac;
                v.observed_pct   = observed_pct;
                out.push_back(std::move(v));
            }
        }
    };

    // ---- Explicit rules ----------------------------------------------------
    for (const auto& rule : config.rules) {
        if (rule.algorithm_label.empty()) continue;
        const std::string baseline_label = resolve_baseline(rule.baseline_key);
        if (baseline_label.empty()) {
            JustLog("[bench.regression_check] rule for '{}' references unknown baseline '{}' — skipping",
                    rule.algorithm_label, rule.baseline_key);
            continue;
        }
        for (const auto& [bucket_key, by_algo] : cells) {
            auto algo_it = by_algo.find(rule.algorithm_label);
            auto base_it = by_algo.find(baseline_label);
            if (algo_it == by_algo.end() || base_it == by_algo.end()) continue;
            build_violation(rule, rule.algorithm_label, baseline_label,
                            bucket_key, algo_it->second, base_it->second);
        }
    }

    // ---- Wildcard rules ----------------------------------------------------
    // For each wildcard rule, find every algorithm whose label starts
    // with the prefix and isn't already covered by an explicit rule for
    // the same metric. Then evaluate the rule on every cell where both
    // the algorithm and the baseline have data.
    auto explicit_pair_exists = [&](std::string_view algorithm_label, RuleMetric metric) {
        for (const auto& r : config.rules) {
            if (r.metric == metric && r.algorithm_label == algorithm_label) return true;
        }
        return false;
    };

    for (const auto& rule : config.wildcard_rules) {
        if (rule.algorithm_prefix.empty()) continue;
        const std::string baseline_label = resolve_baseline(rule.baseline_key);
        if (baseline_label.empty()) {
            JustLog("[bench.regression_check] wildcard rule references unknown baseline '{}' — skipping",
                    rule.baseline_key);
            continue;
        }
        // Discover the matching algorithm labels across all cells.
        std::vector<std::string> matching_algorithms;
        for (const auto& [bucket_key, by_algo] : cells) {
            (void)bucket_key;
            for (const auto& [label, _] : by_algo) {
                (void)_;
                if (label.rfind(rule.algorithm_prefix, 0) != 0) continue;
                if (explicit_pair_exists(label, rule.metric)) continue;
                if (std::find(matching_algorithms.begin(), matching_algorithms.end(), label) ==
                    matching_algorithms.end()) {
                    matching_algorithms.push_back(label);
                }
            }
        }
        for (const auto& algorithm_label : matching_algorithms) {
            for (const auto& [bucket_key, by_algo] : cells) {
                auto algo_it = by_algo.find(algorithm_label);
                auto base_it = by_algo.find(baseline_label);
                if (algo_it == by_algo.end() || base_it == by_algo.end()) continue;
                build_violation(rule, algorithm_label, baseline_label,
                                bucket_key, algo_it->second, base_it->second);
            }
        }
    }

    return out;
}

// ---------------------------------------------------------------------
// JSON output (embedded in report.json)
// ---------------------------------------------------------------------

inline void emit_violations_json(JsonWriter& writer, const std::vector<RegressionViolation>& violations) {
    writer.key("violations");
    writer.begin_array();
    for (const auto& v : violations) {
        writer.begin_object();
        writer.kv_string("algorithm",       v.algorithm_label);
        writer.kv_string("baseline",        v.baseline_label);
        writer.kv_string("metric",          rule_metric_name(v.metric));
        writer.kv_string("rule",            rule_kind_name(v.kind));
        writer.kv_string("length_bucket",   v.length_bucket_label);
        writer.kv_string("error_bucket",    v.error_bucket_label);
        writer.kv_double("tolerance_pct",   v.tolerance_pct);
        writer.kv_double("threshold_pct",   v.threshold_pct);
        writer.kv_double("baseline_value",  v.baseline_value);
        writer.kv_double("actual_value",    v.actual_value);
        writer.kv_double("observed_pct",    v.observed_pct);
        writer.kv_string("comment",         v.rule_comment);
        writer.end_object();
    }
    writer.end_array();
}

// ---------------------------------------------------------------------
// Console table
// ---------------------------------------------------------------------

namespace detail {

[[nodiscard]] inline std::string format_metric_value(RuleMetric metric, double value) noexcept {
    if (metric == RuleMetric::WALL_CLOCK) {
        return fmt::format("{:.3f} ms", value / 1.0e6);
    }
    return fmt::format("{:.2f}%", value * 100.0);
}

[[nodiscard]] inline std::string format_rule_summary(const RegressionViolation& v) noexcept {
    if (v.kind == RuleKind::FASTER_BY_AT_LEAST_PCT) {
        return fmt::format(">= {:.1f}% faster (tol {:.1f}%)",
                           v.threshold_pct, v.tolerance_pct);
    }
    return fmt::format("equal to baseline (tol {:.1f}%)", v.tolerance_pct);
}

[[nodiscard]] inline std::string truncate_to_width(std::string_view s, std::size_t width) {
    if (s.size() <= width) return std::string{s};
    if (width <= 3) return std::string(width, '.');
    std::string out;
    out.reserve(width);
    out.append(s.substr(0, width - 3));
    out.append("...");
    return out;
}

}  // namespace detail

/**
 * @brief Pretty-print the violations as a fixed-width table on stderr.
 *
 * Layout (120 columns wide):
 *
 *   metric  | algorithm                        | baseline                  | length        | error    | rule                       | baseline | actual | obs%
 *
 * The algorithm column is the widest at 36 chars; longer labels are
 * truncated with an ellipsis. We deliberately use stderr (not JustLog)
 * so the table renders synchronously next to the regular bench output —
 * fmtlog's async queue would re-order this against the Catch2 summary.
 */
inline void print_violations_table(const std::vector<RegressionViolation>& violations) {
    if (violations.empty()) {
        std::fprintf(stderr, "\n[bench.regression_check] all regression rules passed.\n");
        std::fflush(stderr);
        return;
    }

    constexpr std::size_t metric_w   = 10;
    constexpr std::size_t algo_w     = 36;
    constexpr std::size_t baseline_w = 26;
    constexpr std::size_t length_w   = 18;
    constexpr std::size_t error_w    = 22;
    constexpr std::size_t rule_w     = 28;
    constexpr std::size_t basev_w    = 12;
    constexpr std::size_t actv_w     = 12;
    constexpr std::size_t obs_w      = 9;

    auto print_separator = [&]() {
        std::string line;
        line.append(metric_w + 2, '-');     line.append("+");
        line.append(algo_w + 2, '-');       line.append("+");
        line.append(baseline_w + 2, '-');   line.append("+");
        line.append(length_w + 2, '-');     line.append("+");
        line.append(error_w + 2, '-');      line.append("+");
        line.append(rule_w + 2, '-');       line.append("+");
        line.append(basev_w + 2, '-');      line.append("+");
        line.append(actv_w + 2, '-');       line.append("+");
        line.append(obs_w + 2, '-');
        std::fprintf(stderr, "%s\n", line.c_str());
    };

    std::fprintf(stderr, "\n[bench.regression_check] %zu regression rule(s) violated:\n",
                 violations.size());
    print_separator();
    std::fprintf(stderr, " %-*s | %-*s | %-*s | %-*s | %-*s | %-*s | %*s | %*s | %*s\n",
                 static_cast<int>(metric_w), "metric",
                 static_cast<int>(algo_w), "algorithm",
                 static_cast<int>(baseline_w), "baseline",
                 static_cast<int>(length_w), "length",
                 static_cast<int>(error_w), "error",
                 static_cast<int>(rule_w), "rule",
                 static_cast<int>(basev_w), "baseline",
                 static_cast<int>(actv_w), "actual",
                 static_cast<int>(obs_w), "obs%");
    print_separator();
    for (const auto& v : violations) {
        const std::string baseline_v = detail::format_metric_value(v.metric, v.baseline_value);
        const std::string actual_v   = detail::format_metric_value(v.metric, v.actual_value);
        const std::string obs_pct    = fmt::format("{:+.2f}", v.observed_pct);
        const std::string rule_summary = detail::format_rule_summary(v);
        std::fprintf(stderr,
            " %-*s | %-*s | %-*s | %-*s | %-*s | %-*s | %*s | %*s | %*s\n",
            static_cast<int>(metric_w), rule_metric_name(v.metric),
            static_cast<int>(algo_w),
            detail::truncate_to_width(v.algorithm_label, algo_w).c_str(),
            static_cast<int>(baseline_w),
            detail::truncate_to_width(v.baseline_label, baseline_w).c_str(),
            static_cast<int>(length_w),
            detail::truncate_to_width(v.length_bucket_label, length_w).c_str(),
            static_cast<int>(error_w),
            detail::truncate_to_width(v.error_bucket_label, error_w).c_str(),
            static_cast<int>(rule_w),
            detail::truncate_to_width(rule_summary, rule_w).c_str(),
            static_cast<int>(basev_w), baseline_v.c_str(),
            static_cast<int>(actv_w), actual_v.c_str(),
            static_cast<int>(obs_w), obs_pct.c_str());
    }
    print_separator();
    std::fprintf(stderr,
        "\n  obs%% = (baseline - actual)/baseline * 100 for wall_clock (positive = faster)\n"
        "         = (actual - baseline) * 100        for accuracy   (positive = more accurate)\n");
    std::fflush(stderr);
}

// ---------------------------------------------------------------------
// Convenience entry point used by bench_main
// ---------------------------------------------------------------------

/**
 * @brief Pick the regression-thresholds path.
 *
 * Reads `BIOSEQ_BENCH_REGRESSION_THRESHOLDS` if set; otherwise falls
 * back to `<source>/bench/regression_thresholds.json` if compiled
 * with `-DBIOSEQ_BENCH_REGRESSION_THRESHOLDS_PATH=...` (CMake-side
 * convenience), otherwise just `regression_thresholds.json` in the
 * current working directory. The bench is invoked from
 * `build/<mode>/` so a relative path cannot find the file by itself —
 * the env var is the canonical way to override.
 */
[[nodiscard]] inline std::string resolve_thresholds_path() {
    if (const char* env = std::getenv("BIOSEQ_BENCH_REGRESSION_THRESHOLDS")) {
        if (env[0] != '\0') return std::string{env};
    }
#ifdef BIOSEQ_BENCH_REGRESSION_THRESHOLDS_PATH
    return std::string{BIOSEQ_BENCH_REGRESSION_THRESHOLDS_PATH};
#else
    return std::string{"regression_thresholds.json"};
#endif
}

}  // namespace bioseq::bench

}  // export
