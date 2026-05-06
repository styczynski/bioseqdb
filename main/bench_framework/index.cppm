/**
 * @file bench_framework/index.cppm
 * @brief Generic, algorithm-agnostic benchmark orchestration framework.
 *
 * Public surface lives in `bioseq::bench_framework`. Users describe a
 * benchmark with `create_benchmark<Input, Output>(...)` (three lambdas:
 * input builder, algorithm runner, output evaluator). They then pass any
 * number of those benchmark handles to `run(...)`. The framework parses
 * the JSON config, expands the parameter matrix, drives the iteration
 * loop in parallel via TBB while showing an ftxui progress dashboard,
 * collects three flavours of metric (built-in `wall_clock_ns`, named
 * counter snapshots, evaluation-handler outputs), evaluates rules from
 * the config, and writes a single `report.json`.
 *
 * Design choices documented in BENCH_V2_IMPL_PLAN.md. Keep that file in
 * sync with this implementation.
 */
module;

#include "common.h"

#include <simdjson.h>

#include <nlohmann/json.hpp>
#include <nlohmann/json-schema.hpp>

#include <oneapi/tbb/global_control.h>
#include <oneapi/tbb/parallel_for_each.h>
#include <oneapi/tbb/task_arena.h>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

export module bench_framework;

import mlogging;
import bioseq_instr;

// ---------------------------------------------------------------------------
//  Public surface
// ---------------------------------------------------------------------------

export namespace bioseq::bench_framework {

/**
 * @brief Variant-like wrapper for JSON-derived configuration values.
 *
 * Holds one of (string, int64, double, bool). Constructed from a simdjson
 * element via `from_simdjson` or directly from a typed source via the
 * templated `from<T>` factory. String content is owned (`std::string`)
 * so the value survives the simdjson parser's lifetime.
 *
 * Comparison operators compare against the active kind only — a string
 * value never compares equal to an integer, even if the integer's
 * decimal spelling matches the string.
 */
struct ConfigValue {
    enum class Kind : std::uint8_t { String, Integer, Double, Boolean };

    Kind         kind = Kind::String;
    std::string  str_v{};
    std::int64_t int_v = 0;
    double       dbl_v = 0.0;
    bool         bool_v = false;

    [[nodiscard]] constexpr bool is_string()  const noexcept { return kind == Kind::String; }
    [[nodiscard]] constexpr bool is_integer() const noexcept { return kind == Kind::Integer; }
    [[nodiscard]] constexpr bool is_double()  const noexcept { return kind == Kind::Double; }
    [[nodiscard]] constexpr bool is_boolean() const noexcept { return kind == Kind::Boolean; }

    [[nodiscard]] std::string_view as_string()  const noexcept { return str_v; }
    [[nodiscard]] std::int64_t     as_integer() const noexcept { return int_v; }
    [[nodiscard]] double           as_double()  const noexcept { return dbl_v; }
    [[nodiscard]] bool             as_boolean() const noexcept { return bool_v; }

    [[nodiscard]] bool operator==(std::string_view rhs) const noexcept {
        return is_string() && str_v == rhs;
    }
    [[nodiscard]] bool operator==(const char* rhs) const noexcept {
        return is_string() && str_v == rhs;
    }
    [[nodiscard]] bool operator==(std::int64_t rhs) const noexcept {
        return is_integer() && int_v == rhs;
    }
    [[nodiscard]] bool operator==(int rhs) const noexcept {
        return is_integer() && int_v == static_cast<std::int64_t>(rhs);
    }
    [[nodiscard]] bool operator==(double rhs) const noexcept {
        return is_double() && dbl_v == rhs;
    }
    [[nodiscard]] bool operator==(bool rhs) const noexcept {
        return is_boolean() && bool_v == rhs;
    }

    [[nodiscard]] bool operator==(const ConfigValue& other) const noexcept {
        if (kind != other.kind) return false;
        switch (kind) {
            case Kind::String:  return str_v == other.str_v;
            case Kind::Integer: return int_v == other.int_v;
            case Kind::Double:  return dbl_v == other.dbl_v;
            case Kind::Boolean: return bool_v == other.bool_v;
        }
        return false;
    }

    /// Templated factory replacing four near-identical `from_T` helpers.
    template<typename T>
    [[nodiscard]] static ConfigValue from(T raw_value) noexcept {
        ConfigValue out{};
        if constexpr (std::is_same_v<T, std::string>
                      || std::is_same_v<T, std::string_view>
                      || std::is_same_v<T, const char*>) {
            out.kind = ConfigValue::Kind::String;
            out.str_v = std::string{raw_value};
        } else if constexpr (std::is_same_v<T, bool>) {
            out.kind = ConfigValue::Kind::Boolean;
            out.bool_v = raw_value;
        } else if constexpr (std::is_integral_v<T>) {
            out.kind = ConfigValue::Kind::Integer;
            out.int_v = static_cast<std::int64_t>(raw_value);
        } else if constexpr (std::is_floating_point_v<T>) {
            out.kind = ConfigValue::Kind::Double;
            out.dbl_v = static_cast<double>(raw_value);
        } else {
            static_assert(sizeof(T) == 0, "unsupported ConfigValue source type");
        }
        return out;
    }

    /// Parse a simdjson element. Arrays and objects are not first-class
    /// configuration values; callers that want to accept them must walk
    /// the simdjson element directly. For exotic input we fall back to a
    /// stringified form so the value is preserved end-to-end.
    [[nodiscard]] static ConfigValue from_simdjson(simdjson::dom::element element) noexcept;
};

/// Per-algorithm parameter map: param_name -> value (with defaults filled in).
using ParamMap = std::map<std::string, ConfigValue>;

/**
 * @brief Type-erased benchmark handle returned by `create_benchmark`.
 *
 * Public so the variadic `run(...)` parameter pack can be deduced. Users
 * never construct this directly.
 */
struct ErasedBenchmark {
    std::string title;
    std::string benchmark_key;
    std::string config_path;
    /// Owns the typed lambdas + iteration loop. Invoked once by `run()`
    /// after concurrency arbitration.
    std::function<void(struct BenchmarkContext&)> execute;
};

namespace detail {

[[nodiscard]] int run_impl(int argc, char** argv,
                            std::string report_dir,
                            std::string data_dir,
                            std::vector<ErasedBenchmark> benchmarks);

}  // namespace detail

/**
 * @brief Build a benchmark spec.
 *
 * **Thread-safety contract (important).** When a benchmark's
 * `concurrency.concurrency == true` (the default), the framework runs many
 * iterations of `on_run` and `on_evaluate` concurrently on TBB worker
 * threads. The captured callable objects must therefore be safe to invoke
 * concurrently — typically pure functions of their arguments, or callables
 * that only read shared state. `on_input` is single-shot per test case
 * (guarded by `std::call_once`) so it has no concurrency requirement, but
 * it MUST NOT throw — the framework does not catch user exceptions.
 *
 * @param title             Human-readable label echoed in the report's `title` field.
 * @param benchmark_key     Unique key used (a) to look up the matching block in
 *                          `config_path`, and (b) as the report's
 *                          `benchmarks.<benchmark_key>` entry. One config file may
 *                          hold multiple benchmark blocks; the key disambiguates
 *                          which block this `create_benchmark` call describes.
 * @param config_path       Path to the bench configuration JSON (validated against
 *                          a built-in JSON schema before use).
 * @param on_input          Builds the test-case `Input` from the parsed JSON data.
 *                          Called at most once per (benchmark, test_case) — the
 *                          framework caches the result and reuses it across all
 *                          iterations of that test case. Must not throw.
 * @param on_run            Runs the algorithm. Wraps the timing window for the
 *                          built-in `wall_clock_ns` metric. Called concurrently
 *                          when the benchmark allows parallel iteration; treat
 *                          captured state accordingly.
 * @param on_evaluate       Returns evaluation metrics computed from the algorithm
 *                          output. Each map entry is a `(metric_name,
 *                          metric_value)` pair appended to the `all_regions`
 *                          bucket. Names that don't match any `metrics` key
 *                          declared with `type: "evaluation"` are silently
 *                          dropped (logged at DBG level so users can debug
 *                          missing-key issues). Called concurrently like
 *                          `on_run`; no shared mutable state.
 */
template<typename Input, typename Output>
[[nodiscard]] ErasedBenchmark create_benchmark(
    std::string title,
    std::string benchmark_key,
    std::string config_path,
    std::function<Input(std::string_view data_dir,
                        std::string_view test_case_name,
                        simdjson::dom::element test_case_data)> on_input,
    std::function<Output(std::string_view algorithm_name,
                         const ParamMap& params,
                         const Input& input)> on_run,
    std::function<std::map<std::string, double>(std::string_view algorithm_name,
                                                const ParamMap& params,
                                                const Output& output)> on_evaluate);

/**
 * @brief Run all benchmarks. Returns 0 on success, 1 if any rule violated
 *        or any config failed to parse / validate.
 *
 * Variadic for syntax fidelity with the user's `main.cpp` example;
 * collects the pack into `vector<ErasedBenchmark>` and delegates.
 */
template<typename... Benchmarks>
[[nodiscard]] int run(int argc, char** argv,
                     std::string report_dir,
                     std::string data_dir,
                     Benchmarks&&... benchmarks)
{
    std::vector<ErasedBenchmark> erased_list;
    erased_list.reserve(sizeof...(Benchmarks));
    (erased_list.push_back(std::forward<Benchmarks>(benchmarks)), ...);
    return detail::run_impl(argc, argv,
                            std::move(report_dir), std::move(data_dir),
                            std::move(erased_list));
}

}  // namespace bioseq::bench_framework

// ---------------------------------------------------------------------------
//  Internals
// ---------------------------------------------------------------------------

namespace bioseq::bench_framework {

/// Canonical compile-time profiler args used by the framework. Timing-only
/// is enough for `wall_clock_ns`; the framework does not need PMU counters
/// or sampling.
using profiler_args_t = bioseq::instr::context_args_profiling_timing_only;

namespace detail {

// ----------------- shared types -----------------

enum class CompareOp : std::uint8_t { LessThan, GreaterThan, Equal };
enum class MetricKind : std::uint8_t { WallClockNs, Counter, Evaluation };

[[nodiscard]] inline constexpr const char* metric_kind_name(MetricKind metric_kind) noexcept {
    switch (metric_kind) {
        case MetricKind::WallClockNs: return "builtin";
        case MetricKind::Counter:     return "counter";
        case MetricKind::Evaluation:  return "evaluation";
    }
    return "?";
}

[[nodiscard]] inline std::optional<MetricKind> parse_metric_kind(std::string_view kind_text) noexcept {
    if (kind_text == "builtin")    return MetricKind::WallClockNs;
    if (kind_text == "counter")    return MetricKind::Counter;
    if (kind_text == "evaluation") return MetricKind::Evaluation;
    return std::nullopt;
}

[[nodiscard]] inline std::optional<CompareOp> parse_compare_op(std::string_view op_text) noexcept {
    if (op_text == "<") return CompareOp::LessThan;
    if (op_text == ">") return CompareOp::GreaterThan;
    if (op_text == "=") return CompareOp::Equal;
    return std::nullopt;
}

/// Match expression — picks a subset of execution tuples by algorithm key
/// and a partial parameter assignment. Empty `match` map means "any tuple
/// for this algorithm".
struct RuleSelector {
    std::string algorithm_key;
    ParamMap    match{};
};

struct RuleComparator {
    CompareOp op = CompareOp::Equal;
    double    threshold_pct = 0.0;
    double    tolerance_pct = 0.0;
};

struct Rule {
    std::string     name;
    std::string     metric_name;
    MetricKind      metric_kind = MetricKind::WallClockNs;
    RuleSelector    lhs;
    RuleComparator  cmp;
    RuleSelector    rhs;
};

struct MetricSpec {
    std::string  name;
    MetricKind   kind = MetricKind::WallClockNs;
    /// For `kind == Counter`: canonical name of the instrumentation counter
    /// to snapshot. Defaults to `name` when the config omits the explicit
    /// `"counter"` field.
    std::string  counter_name{};
    std::vector<Rule> rules{};
};

struct ConcurrencyOpts {
    bool bench_lock  = false;
    bool concurrency = true;
};

struct ParamSchema {
    std::string algorithm_name;
    std::string param_name;
    std::string json_type;
    std::vector<ConfigValue>     enum_values{};
    std::optional<ConfigValue>   default_value{};
    std::string description;
};

struct AlgorithmSchema {
    std::string algorithm_name;
    std::vector<ParamSchema> params{};
};

struct TestCaseSpec {
    std::string name;
    std::string description;
    /// View into the parser pool of the owning `BenchmarkExecutionState`.
    simdjson::dom::element data{};
};

struct BenchmarkConfig {
    std::string config_path;
    std::string benchmark_key;
    std::string description;
    bool fast = false;
    ConcurrencyOpts concurrency{};
    bool by_regions = false;
    /// (test_case_name, iterations) pairs declared in `matrix.for`.
    std::vector<std::pair<std::string, std::size_t>> for_each{};
    /// algorithm_name -> param_name -> declared values list (one cross-product axis).
    std::map<std::string, std::map<std::string, std::vector<ConfigValue>>> matrix_over{};
    /// Schemas for every algorithm registered in top-level `params`. Cloned
    /// into the per-benchmark config so matrix expansion can fill defaults.
    std::map<std::string, AlgorithmSchema> algorithm_schemas{};
    /// Metrics declared by this benchmark. Iteration order preserved.
    std::vector<MetricSpec> metrics{};
};

struct ExecutionTuple {
    std::string algorithm_name;
    ParamMap    params{};      // every parameter resolved (declared or defaulted)
};

/// One (test_case × execution_tuple) cell holding the iteration metric
/// arrays plus the bookkeeping needed to fan out parallel iterations.
///
/// `Cell` is owned via `std::unique_ptr<Cell>` (see `BenchmarkExecutionState`)
/// because it embeds a `std::mutex` (non-movable) and stable pointers are
/// handed out as `Cell*` to TBB workers. Per CODE_STYLE.md §5, smart
/// pointers exist here because there is a clear ownership transfer (the
/// per-benchmark state owns every cell for the lifetime of the run); the
/// alternative would be a stable-iterator container like `std::list` plus
/// manual lookup, which is strictly worse.
struct Cell {
    const TestCaseSpec*  test_case = nullptr;
    const ExecutionTuple* tuple = nullptr;
    std::size_t          iterations = 0;
    /// Stable index into the test_cases vector. Lets the input cache key
    /// inputs by index without pointer arithmetic on hot path.
    std::size_t          test_case_index = 0;

    /// region_name -> metric_name -> sample values. "all_regions" is a
    /// synthetic bucket holding the iteration-level wall_clock_ns and every
    /// counter / evaluation metric. Per-region buckets only appear when
    /// `BenchmarkConfig::by_regions == true`.
    std::map<std::string, std::map<std::string, std::vector<double>>> samples{};
    std::mutex samples_mu{};

    Cell() = default;
    Cell(const Cell&) = delete;
    Cell& operator=(const Cell&) = delete;
    Cell(Cell&&) = delete;
    Cell& operator=(Cell&&) = delete;
};

/// Pinned (move + copy deleted) — `simdjson::dom::element` views handed to
/// the user reference `parser`, and any move would invalidate them. The
/// owning slot is `std::unique_ptr<BenchmarkExecutionState>` inside
/// `BenchmarkContext::reports`; the unique_ptr moves freely because it
/// only relocates an 8-byte heap pointer.
struct BenchmarkExecutionState {
    /// User-facing title from `create_benchmark`, copied here so the report
    /// writer doesn't need a separate channel.
    std::string                         title{};
    simdjson::dom::parser              parser{};
    std::string                         config_body{};
    BenchmarkConfig                     config{};
    std::vector<TestCaseSpec>           test_cases{};
    std::vector<ExecutionTuple>         execution_tuples{};
    std::vector<std::unique_ptr<Cell>>  cells{};

    BenchmarkExecutionState() = default;
    BenchmarkExecutionState(BenchmarkExecutionState&&) = delete;
    BenchmarkExecutionState(const BenchmarkExecutionState&) = delete;
    BenchmarkExecutionState& operator=(BenchmarkExecutionState&&) = delete;
    BenchmarkExecutionState& operator=(const BenchmarkExecutionState&) = delete;
};

struct ParsedCli {
    bool                       fast_filter = false;
    std::optional<unsigned>    threads_override{};
    std::optional<std::string> report_override{};
};

struct Violation {
    std::string config_path;
    std::string benchmark_key;
    std::string rule_name;
    std::string test_case;
    std::string metric;
    std::string lhs_algorithm;
    ParamMap    lhs_full_params{};
    std::string rhs_algorithm;
    ParamMap    rhs_full_params{};
    /// Index 0 = LHS aggregate; index 1 = RHS aggregate. Mirrors the spec's
    /// `"args": [...]` field on the output.
    std::array<double, 2> args{0.0, 0.0};
};

// Forward declarations used by BenchmarkContext.
struct ProgressState;

}  // namespace detail

/**
 * @brief Per-benchmark context passed to the `execute` closure.
 *
 * The closure does not see this directly — the type is forward-declared
 * publicly so the public `ErasedBenchmark` struct can carry a
 * `function<void(BenchmarkContext&)>` member without exposing the full
 * private layout.
 */
struct BenchmarkContext {
    std::string                                    data_dir;
    std::string                                    report_dir;
    bool                                            fast_filter = false;
    std::shared_mutex*                              cross_bench_mu = nullptr;
    detail::ProgressState*                          progress = nullptr;
    std::size_t                                     benchmark_index = 0;
    std::mutex*                                     violations_mu = nullptr;
    std::vector<detail::Violation>*                 violations = nullptr;
    std::mutex*                                     reports_mu = nullptr;
    /// Map from benchmark_key to the per-benchmark execution state. The
    /// value is `unique_ptr` because `BenchmarkExecutionState` is pinned
    /// (the simdjson parser inside is non-movable). A unique_ptr moves
    /// freely (8-byte pointer relocation) so it composes with std::map.
    std::map<std::string, std::unique_ptr<detail::BenchmarkExecutionState>>* reports = nullptr;
};

// ----------------- ConfigValue::from_simdjson -----------------

// ConfigValue helpers must be visible to translation units that import the module,
// so the body sits in the public namespace block.
[[nodiscard]] inline ConfigValue ConfigValue::from_simdjson(simdjson::dom::element element) noexcept {
    using element_type = simdjson::dom::element_type;
    ConfigValue out{};
    switch (element.type()) {
        case element_type::INT64: {
            std::int64_t parsed = 0;
            if (element.get(parsed) == simdjson::SUCCESS) {
                out.kind  = Kind::Integer;
                out.int_v = parsed;
            }
            return out;
        }
        case element_type::UINT64: {
            std::uint64_t parsed = 0;
            if (element.get(parsed) == simdjson::SUCCESS) {
                out.kind  = Kind::Integer;
                out.int_v = static_cast<std::int64_t>(parsed);
            }
            return out;
        }
        case element_type::DOUBLE: {
            double parsed = 0.0;
            if (element.get(parsed) == simdjson::SUCCESS) {
                out.kind  = Kind::Double;
                out.dbl_v = parsed;
            }
            return out;
        }
        case element_type::BOOL: {
            bool parsed = false;
            if (element.get(parsed) == simdjson::SUCCESS) {
                out.kind   = Kind::Boolean;
                out.bool_v = parsed;
            }
            return out;
        }
        case element_type::STRING: {
            std::string_view parsed{};
            if (element.get(parsed) == simdjson::SUCCESS) {
                out.kind  = Kind::String;
                out.str_v = std::string{parsed};
            }
            return out;
        }
        default:
            // Object / array / null: fall back to a stringified spelling so the
            // value is preserved verbatim in canonical IDs and rule selectors.
            out.kind  = Kind::String;
            out.str_v = simdjson::to_string(element);
            return out;
    }
}

namespace detail {

// ----------------- ProgressState -----------------

/// Per-benchmark progress counter snapshot used by the ftxui dashboard.
struct ProgressState {
    struct Per {
        std::string title;
        std::string benchmark_key;
        std::atomic<std::uint64_t> done{0};
        std::uint64_t total = 0;
    };
    std::vector<std::unique_ptr<Per>> per_benchmark;
    std::atomic<bool> running{true};
    std::atomic<bool> tty_render{false};   // set true if stderr is a TTY at startup

    /// Increment the done counter for benchmark at `benchmark_index`. Hot
    /// path; relaxed atomic.
    void tick(std::size_t benchmark_index) noexcept {
        if (benchmark_index < per_benchmark.size()) {
            per_benchmark[benchmark_index]->done.fetch_add(1, std::memory_order_relaxed);
        }
    }
};

// ----------------- Schema definition (built-in) -----------------

inline constexpr std::string_view kBenchConfigSchema = R"({
  "$schema": "http://json-schema.org/draft-07/schema#",
  "type": "object",
  "properties": {
    "params": {
      "type": "object",
      "additionalProperties": {
        "type": "object",
        "additionalProperties": {
          "type": "object",
          "properties": {
            "type":        { "type": "string" },
            "enum":        { "type": "array" },
            "default":     {},
            "description": { "type": "string" }
          },
          "required": ["type"]
        }
      }
    },
    "test_cases": {
      "type": "object",
      "additionalProperties": {
        "type": "object",
        "properties": {
          "description": { "type": "string" },
          "data":        { "type": "object" }
        },
        "required": ["data"]
      }
    }
  },
  "required": ["params", "test_cases"]
})";

[[nodiscard]] inline bool validate_config_against_schema(const std::string& json_text,
                                                          const std::string& source_path) noexcept
{
    // The schema validator throws on error; the user-side benchmark config is
    // not under our control, so we catch and surface the problem via JustLog.
    try {
        nlohmann::json document = nlohmann::json::parse(json_text);
        nlohmann::json schema = nlohmann::json::parse(kBenchConfigSchema);
        nlohmann::json_schema::json_validator validator;
        validator.set_root_schema(schema);
        validator.validate(document);
        return true;
    } catch (const std::exception& e) {
        loge("[bench_framework] schema validation failed for {}: {}",
                     source_path, e.what());
        return false;
    }
}

// ----------------- Config parsing helpers -----------------

/// Read the entire contents of `path` into a `std::string`. Returns an empty
/// string if the file cannot be opened (e.g. missing or permission denied);
/// callers must distinguish "missing" from "legitimately empty" by stat-ing
/// the path first if that matters.
[[nodiscard]] inline std::string slurp_file(const std::filesystem::path& path) noexcept {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

/// Parse the top-level `params` block (`{ algo_name: { param_name:
/// json-schema-entry, ... } }`) into a per-algorithm schema map. The schema
/// is what matrix expansion later consults to fill in defaults for any
/// parameter not explicitly listed in `matrix.over`. Sets `parse_ok = false`
/// on any malformed entry but keeps walking the rest of the document so the
/// user sees as many errors as possible per build.
[[nodiscard]] inline std::map<std::string, AlgorithmSchema> parse_top_level_params(
    simdjson::dom::object params_object,
    bool& parse_ok)
{
    std::map<std::string, AlgorithmSchema> result;
    for (auto algorithm_field : params_object) {
        AlgorithmSchema algorithm_schema;
        algorithm_schema.algorithm_name = std::string{algorithm_field.key};

        simdjson::dom::object param_object;
        if (algorithm_field.value.get(param_object) != simdjson::SUCCESS) {
            loge("[bench_framework] params.{} is not an object", algorithm_field.key);
            parse_ok = false;
            continue;
        }

        for (auto param_field : param_object) {
            ParamSchema schema;
            schema.algorithm_name = algorithm_schema.algorithm_name;
            schema.param_name     = std::string{param_field.key};

            simdjson::dom::object schema_obj;
            if (param_field.value.get(schema_obj) != simdjson::SUCCESS) {
                loge("[bench_framework] params.{}.{} is not an object",
                             algorithm_schema.algorithm_name, param_field.key);
                parse_ok = false;
                continue;
            }

            std::string_view type_text;
            if (schema_obj["type"].get(type_text) != simdjson::SUCCESS) {
                loge("[bench_framework] params.{}.{} missing 'type'",
                             algorithm_schema.algorithm_name, param_field.key);
                parse_ok = false;
                continue;
            }
            schema.json_type = std::string{type_text};

            simdjson::dom::array enum_array;
            if (schema_obj["enum"].get(enum_array) == simdjson::SUCCESS) {
                for (auto element : enum_array) {
                    schema.enum_values.push_back(ConfigValue::from_simdjson(element));
                }
            }

            simdjson::dom::element default_element;
            if (schema_obj["default"].get(default_element) == simdjson::SUCCESS) {
                schema.default_value = ConfigValue::from_simdjson(default_element);
            }

            std::string_view description_text;
            if (schema_obj["description"].get(description_text) == simdjson::SUCCESS) {
                schema.description = std::string{description_text};
            }

            algorithm_schema.params.push_back(std::move(schema));
        }

        result.emplace(algorithm_schema.algorithm_name, std::move(algorithm_schema));
    }
    return result;
}

/// Parse the top-level `test_cases` block. Each entry produces a
/// `TestCaseSpec` whose `data` element is a non-owning view into the parser
/// pool — the parser must outlive the returned vector, which is true because
/// both live as members on `BenchmarkExecutionState`.
[[nodiscard]] inline std::vector<TestCaseSpec> parse_test_cases(
    simdjson::dom::object test_cases_object,
    bool& parse_ok)
{
    std::vector<TestCaseSpec> result;
    for (auto field : test_cases_object) {
        simdjson::dom::object inner_obj;
        if (field.value.get(inner_obj) != simdjson::SUCCESS) {
            loge("[bench_framework] test_cases.{} is not an object", field.key);
            parse_ok = false;
            continue;
        }
        TestCaseSpec spec;
        spec.name = std::string{field.key};

        std::string_view description_text;
        if (inner_obj["description"].get(description_text) == simdjson::SUCCESS) {
            spec.description = std::string{description_text};
        }

        simdjson::dom::element data_element;
        if (inner_obj["data"].get(data_element) != simdjson::SUCCESS) {
            loge("[bench_framework] test_cases.{}.data missing", field.key);
            parse_ok = false;
            continue;
        }
        spec.data = data_element;

        result.push_back(std::move(spec));
    }
    return result;
}

/// Parse a rule's LHS or RHS selector. Selectors look like
/// `{ "key": "<algo>", "<param>": <value>, ... }` — the `key` field is
/// mandatory and names the algorithm; every other field is treated as a
/// (param_name, expected_value) match constraint. Empty constraint set
/// matches every tuple of that algorithm.
[[nodiscard]] inline RuleSelector parse_rule_selector(
    simdjson::dom::object selector_obj,
    bool& parse_ok)
{
    RuleSelector selector;
    for (auto field : selector_obj) {
        if (field.key == "key") {
            std::string_view algorithm_name;
            if (field.value.get(algorithm_name) != simdjson::SUCCESS) {
                loge("[bench_framework] rule selector 'key' is not a string");
                parse_ok = false;
                return selector;
            }
            selector.algorithm_key = std::string{algorithm_name};
        } else {
            selector.match.emplace(std::string{field.key},
                                   ConfigValue::from_simdjson(field.value));
        }
    }
    if (selector.algorithm_key.empty()) {
        loge("[bench_framework] rule selector missing 'key' field");
        parse_ok = false;
    }
    return selector;
}

/// Parse a rule's `{ compare, threshold%, tolerance% }` triple. `compare`
/// is mandatory; the two `%` fields default to zero. See `comparator_passes`
/// for how the values turn into a pass/fail decision.
[[nodiscard]] inline RuleComparator parse_rule_comparator(
    simdjson::dom::object cmp_obj,
    bool& parse_ok)
{
    RuleComparator out;
    std::string_view op_text;
    if (cmp_obj["compare"].get(op_text) != simdjson::SUCCESS) {
        loge("[bench_framework] rule comparator missing 'compare'");
        parse_ok = false;
        return out;
    }
    auto parsed_op = parse_compare_op(op_text);
    if (!parsed_op.has_value()) {
        loge("[bench_framework] unknown compare op '{}'", op_text);
        parse_ok = false;
        return out;
    }
    out.op = *parsed_op;

    double threshold_pct = 0.0;
    if (cmp_obj["threshold%"].get(threshold_pct) == simdjson::SUCCESS) {
        out.threshold_pct = threshold_pct;
    }
    double tolerance_pct = 0.0;
    if (cmp_obj["tolerance%"].get(tolerance_pct) == simdjson::SUCCESS) {
        out.tolerance_pct = tolerance_pct;
    }
    return out;
}

/// Parse one rule entry. Rules are 4-element JSON arrays:
/// `[name, lhs_selector, comparator, rhs_selector]`. Returns nullopt and
/// flips `parse_ok` to false on any structural problem.
[[nodiscard]] inline std::optional<Rule> parse_one_rule(
    simdjson::dom::array rule_array,
    const std::string& metric_name,
    MetricKind metric_kind,
    bool& parse_ok)
{
    std::vector<simdjson::dom::element> elements;
    for (auto entry : rule_array) elements.push_back(entry);
    if (elements.size() != 4) {
        loge("[bench_framework] rule array must have 4 elements (name, lhs, cmp, rhs); got {}",
                     elements.size());
        parse_ok = false;
        return std::nullopt;
    }

    Rule rule;
    rule.metric_name = metric_name;
    rule.metric_kind = metric_kind;

    std::string_view rule_name;
    if (elements[0].get(rule_name) != simdjson::SUCCESS) {
        loge("[bench_framework] rule[0] (name) is not a string");
        parse_ok = false;
        return std::nullopt;
    }
    rule.name = std::string{rule_name};

    simdjson::dom::object lhs_obj;
    if (elements[1].get(lhs_obj) != simdjson::SUCCESS) {
        loge("[bench_framework] rule[1] (lhs selector) is not an object");
        parse_ok = false;
        return std::nullopt;
    }
    rule.lhs = parse_rule_selector(lhs_obj, parse_ok);

    simdjson::dom::object cmp_obj;
    if (elements[2].get(cmp_obj) != simdjson::SUCCESS) {
        loge("[bench_framework] rule[2] (comparator) is not an object");
        parse_ok = false;
        return std::nullopt;
    }
    rule.cmp = parse_rule_comparator(cmp_obj, parse_ok);

    simdjson::dom::object rhs_obj;
    if (elements[3].get(rhs_obj) != simdjson::SUCCESS) {
        loge("[bench_framework] rule[3] (rhs selector) is not an object");
        parse_ok = false;
        return std::nullopt;
    }
    rule.rhs = parse_rule_selector(rhs_obj, parse_ok);

    return rule;
}

/// Parse the `matrix.metrics` block. Each entry contributes a `MetricSpec`
/// with optional `type`, optional `counter`, and a list of rules. Defaults:
/// `type="builtin"`, `counter=metric_name`.
[[nodiscard]] inline std::vector<MetricSpec> parse_metrics(
    simdjson::dom::object metrics_obj,
    bool& parse_ok)
{
    std::vector<MetricSpec> result;
    for (auto field : metrics_obj) {
        MetricSpec metric;
        metric.name = std::string{field.key};

        simdjson::dom::object inner_obj;
        if (field.value.get(inner_obj) != simdjson::SUCCESS) {
            loge("[bench_framework] metrics.{} is not an object", field.key);
            parse_ok = false;
            continue;
        }

        std::string_view kind_text = "builtin";
        std::string_view declared_kind;
        if (inner_obj["type"].get(declared_kind) == simdjson::SUCCESS) {
            kind_text = declared_kind;
        }
        auto parsed_kind = parse_metric_kind(kind_text);
        if (!parsed_kind.has_value()) {
            loge("[bench_framework] metrics.{}: unknown type '{}'", field.key, kind_text);
            parse_ok = false;
            continue;
        }
        metric.kind = *parsed_kind;

        std::string_view counter_name;
        if (inner_obj["counter"].get(counter_name) == simdjson::SUCCESS) {
            metric.counter_name = std::string{counter_name};
        } else {
            metric.counter_name = metric.name;
        }

        simdjson::dom::array rules_array;
        if (inner_obj["rules"].get(rules_array) == simdjson::SUCCESS) {
            for (auto entry : rules_array) {
                simdjson::dom::array as_array;
                if (entry.get(as_array) != simdjson::SUCCESS) {
                    loge("[bench_framework] metric '{}' rule entry is not an array", metric.name);
                    parse_ok = false;
                    continue;
                }
                auto parsed = parse_one_rule(as_array, metric.name, metric.kind, parse_ok);
                if (parsed.has_value()) {
                    metric.rules.push_back(std::move(*parsed));
                }
            }
        }

        result.push_back(std::move(metric));
    }
    return result;
}

/// Walk one benchmark block (`bench1`, `bench2`, etc.) inside the top-level
/// config and fill the `BenchmarkConfig`. The benchmark's identity, schema,
/// and test_cases are populated separately by the caller. Returns false on
/// any structural problem (missing `matrix`, missing `for`/`over`, etc.).
[[nodiscard]] inline bool parse_benchmark_block(
    simdjson::dom::object benchmark_obj,
    BenchmarkConfig& benchmark)
{
    bool parse_ok = true;

    std::string_view description_text;
    if (benchmark_obj["description"].get(description_text) == simdjson::SUCCESS) {
        benchmark.description = std::string{description_text};
    }
    bool fast_flag = false;
    if (benchmark_obj["fast"].get(fast_flag) == simdjson::SUCCESS) {
        benchmark.fast = fast_flag;
    }
    simdjson::dom::object concurrency_obj;
    if (benchmark_obj["concurrency"].get(concurrency_obj) == simdjson::SUCCESS) {
        bool bench_lock_flag = false;
        if (concurrency_obj["bench_lock"].get(bench_lock_flag) == simdjson::SUCCESS) {
            benchmark.concurrency.bench_lock = bench_lock_flag;
        }
        bool concurrency_flag = true;
        if (concurrency_obj["concurrency"].get(concurrency_flag) == simdjson::SUCCESS) {
            benchmark.concurrency.concurrency = concurrency_flag;
        }
    }

    simdjson::dom::object matrix_obj;
    if (benchmark_obj["matrix"].get(matrix_obj) != simdjson::SUCCESS) {
        loge("[bench_framework] benchmark '{}' missing 'matrix'", benchmark.benchmark_key);
        return false;
    }

    bool by_regions_flag = false;
    if (matrix_obj["by_regions"].get(by_regions_flag) == simdjson::SUCCESS) {
        benchmark.by_regions = by_regions_flag;
    }

    simdjson::dom::object for_obj;
    if (matrix_obj["for"].get(for_obj) != simdjson::SUCCESS) {
        loge("[bench_framework] benchmark '{}' missing 'matrix.for'", benchmark.benchmark_key);
        return false;
    }
    for (auto field : for_obj) {
        std::int64_t iterations = 0;
        if (field.value.get(iterations) != simdjson::SUCCESS || iterations < 0) {
            loge("[bench_framework] matrix.for.{} must be a non-negative integer", field.key);
            parse_ok = false;
            continue;
        }
        benchmark.for_each.emplace_back(std::string{field.key},
                                         static_cast<std::size_t>(iterations));
    }

    simdjson::dom::object over_obj;
    if (matrix_obj["over"].get(over_obj) != simdjson::SUCCESS) {
        loge("[bench_framework] benchmark '{}' missing 'matrix.over'", benchmark.benchmark_key);
        return false;
    }
    for (auto algo_field : over_obj) {
        simdjson::dom::object params_for_algo;
        if (algo_field.value.get(params_for_algo) != simdjson::SUCCESS) {
            loge("[bench_framework] matrix.over.{} is not an object", algo_field.key);
            parse_ok = false;
            continue;
        }
        std::map<std::string, std::vector<ConfigValue>> per_algo;
        for (auto param_field : params_for_algo) {
            simdjson::dom::array values_array;
            if (param_field.value.get(values_array) != simdjson::SUCCESS) {
                loge("[bench_framework] matrix.over.{}.{} is not an array",
                             algo_field.key, param_field.key);
                parse_ok = false;
                continue;
            }
            std::vector<ConfigValue> values;
            for (auto entry : values_array) {
                values.push_back(ConfigValue::from_simdjson(entry));
            }
            per_algo.emplace(std::string{param_field.key}, std::move(values));
        }
        benchmark.matrix_over.emplace(std::string{algo_field.key}, std::move(per_algo));
    }

    simdjson::dom::object metrics_obj;
    if (matrix_obj["metrics"].get(metrics_obj) == simdjson::SUCCESS) {
        benchmark.metrics = parse_metrics(metrics_obj, parse_ok);
    }

    return parse_ok;
}

/// Top-level config parser. Slurps `config_path`, validates against the
/// built-in JSON schema, parses with simdjson into `state.parser`, then walks
/// `params`, `test_cases`, and the requested `benchmark_key` block. The
/// resulting `BenchmarkExecutionState` owns the parser and config body so
/// the simdjson `dom::element` views handed to the user remain valid for
/// the entire run.
[[nodiscard]] inline bool parse_config_file(
    const std::string& config_path,
    const std::string& benchmark_key,
    BenchmarkExecutionState& state)
{
    state.config.config_path  = config_path;
    state.config.benchmark_key = benchmark_key;
    state.config_body = slurp_file(config_path);
    if (state.config_body.empty()) {
        loge("[bench_framework] config '{}' is empty or unreadable", config_path);
        return false;
    }

    if (!validate_config_against_schema(state.config_body, config_path)) {
        return false;
    }

    auto root_result = state.parser.parse(state.config_body);
    simdjson::dom::element root_element;
    if (root_result.get(root_element) != simdjson::SUCCESS) {
        loge("[bench_framework] simdjson failed to parse '{}'", config_path);
        return false;
    }

    simdjson::dom::object root_obj;
    if (root_element.get(root_obj) != simdjson::SUCCESS) {
        loge("[bench_framework] config '{}' is not a JSON object", config_path);
        return false;
    }

    bool parse_ok = true;

    simdjson::dom::object params_obj;
    if (root_obj["params"].get(params_obj) != simdjson::SUCCESS) {
        loge("[bench_framework] config '{}' missing 'params'", config_path);
        return false;
    }
    state.config.algorithm_schemas = parse_top_level_params(params_obj, parse_ok);

    simdjson::dom::object test_cases_obj;
    if (root_obj["test_cases"].get(test_cases_obj) != simdjson::SUCCESS) {
        loge("[bench_framework] config '{}' missing 'test_cases'", config_path);
        return false;
    }
    state.test_cases = parse_test_cases(test_cases_obj, parse_ok);

    simdjson::dom::element benchmark_element;
    if (root_obj[benchmark_key].get(benchmark_element) != simdjson::SUCCESS) {
        loge("[bench_framework] config '{}' has no top-level key '{}'",
                     config_path, benchmark_key);
        return false;
    }
    simdjson::dom::object benchmark_obj;
    if (benchmark_element.get(benchmark_obj) != simdjson::SUCCESS) {
        loge("[bench_framework] '{}.{}' is not an object", config_path, benchmark_key);
        return false;
    }
    if (!parse_benchmark_block(benchmark_obj, state.config)) {
        return false;
    }
    return parse_ok;
}

// ----------------- Matrix expansion -----------------

/// Cross-product of one algorithm's declared values. Every parameter not
/// listed in `declared` is filled in from the schema's default (or fails
/// the expansion if no default is available). Iteration order over
/// `declared` is the std::map alpha-sort, which we propagate to the report
/// so cross-build comparisons see a stable tuple order.
[[nodiscard]] inline std::vector<ExecutionTuple> expand_one_algorithm(
    const std::string& algorithm_name,
    const std::map<std::string, std::vector<ConfigValue>>& declared,
    const AlgorithmSchema& schema,
    bool& expansion_ok) noexcept
{
    std::vector<ExecutionTuple> seed;
    seed.push_back(ExecutionTuple{algorithm_name, ParamMap{}});

    // Cross-product over declared parameters in iteration order.
    for (const auto& [param_name, value_list] : declared) {
        if (value_list.empty()) {
            loge("[bench_framework] matrix.over.{}.{} has no values", algorithm_name, param_name);
            expansion_ok = false;
            continue;
        }
        std::vector<ExecutionTuple> next_round;
        next_round.reserve(seed.size() * value_list.size());
        for (const auto& accumulated_tuple : seed) {
            for (const auto& candidate_value : value_list) {
                ExecutionTuple expanded = accumulated_tuple;
                expanded.params[param_name] = candidate_value;
                next_round.push_back(std::move(expanded));
            }
        }
        seed = std::move(next_round);
    }

    // Fill in defaults for any schema parameter not declared.
    for (const auto& param_schema : schema.params) {
        const bool was_declared = (declared.find(param_schema.param_name) != declared.end());
        if (was_declared) continue;
        if (!param_schema.default_value.has_value()) {
            loge("[bench_framework] {}.{} not declared in matrix.over and has no default",
                         algorithm_name, param_schema.param_name);
            expansion_ok = false;
            continue;
        }
        for (auto& tuple : seed) {
            tuple.params[param_schema.param_name] = *param_schema.default_value;
        }
    }

    return seed;
}

/// Compose `expand_one_algorithm` over every algorithm declared in
/// `matrix.over`. The total tuple count is the **sum** (not product) of
/// per-algorithm cross-products — each (algorithm, params) pair is a
/// distinct row in the report.
[[nodiscard]] inline std::vector<ExecutionTuple> expand_matrix(
    const BenchmarkConfig& config,
    bool& expansion_ok) noexcept
{
    std::vector<ExecutionTuple> result;
    for (const auto& [algorithm_name, declared] : config.matrix_over) {
        auto schema_it = config.algorithm_schemas.find(algorithm_name);
        if (schema_it == config.algorithm_schemas.end()) {
            loge("[bench_framework] matrix.over.{} has no top-level schema in 'params'",
                         algorithm_name);
            expansion_ok = false;
            continue;
        }
        auto expanded = expand_one_algorithm(algorithm_name, declared, schema_it->second,
                                              expansion_ok);
        for (auto& expansion_tuple : expanded) {
            result.push_back(std::move(expansion_tuple));
        }
    }
    return result;
}

// ----------------- Cell construction -----------------

/// Construct the (test_case × execution_tuple) cell table. Each cell carries
/// the iteration count, samples map, and a stable test_case_index used by
/// the typed input cache.
[[nodiscard]] inline std::vector<std::unique_ptr<Cell>> build_cells(
    const BenchmarkExecutionState& state) noexcept
{
    std::vector<std::unique_ptr<Cell>> cells;
    cells.reserve(state.config.for_each.size() * state.execution_tuples.size());
    for (const auto& [test_case_name, iterations] : state.config.for_each) {
        std::size_t test_case_index = 0;
        bool found = false;
        for (std::size_t i = 0; i < state.test_cases.size(); ++i) {
            if (state.test_cases[i].name == test_case_name) {
                test_case_index = i;
                found = true;
                break;
            }
        }
        if (!found) {
            loge("[bench_framework] test_case '{}' referenced by matrix.for but not declared",
                         test_case_name);
            continue;
        }
        for (const auto& tuple : state.execution_tuples) {
            auto cell = std::make_unique<Cell>();
            cell->test_case        = &state.test_cases[test_case_index];
            cell->test_case_index  = test_case_index;
            cell->tuple            = &tuple;
            cell->iterations       = iterations;
            cells.push_back(std::move(cell));
        }
    }
    return cells;
}

// ----------------- Sample collection -----------------

/// Write one iteration's measurements into the cell's metric arrays:
/// (a) iteration-level wall-clock under `all_regions["wall_clock_ns"]`;
/// (b) per-counter delta under `all_regions[<metric_name>]`;
/// (c) every key the evaluator returned (matched against the configured
///     evaluation metric names);
/// (d) when `by_regions=true`, per-region wall-clock totals from
///     `profiler_block`.
/// Holds `cell.samples_mu` for the duration of the append so concurrent
/// iterations don't trample each other's vectors.
inline void append_iteration_samples(
    Cell& cell,
    bool by_regions,
    bioseq::instr::ProfilerBlock<profiler_args_t>& profiler_block,
    double iteration_wall_clock_ns,
    const std::vector<MetricSpec>& metrics,
    const std::vector<std::int64_t>& counters_before,
    const std::vector<std::int64_t>& counters_after,
    const std::map<std::string, double>& evaluation_metrics) noexcept
{
    std::lock_guard<std::mutex> guard(cell.samples_mu);
    auto& all_regions = cell.samples["all_regions"];
    all_regions["wall_clock_ns"].push_back(iteration_wall_clock_ns);

    std::size_t counter_index = 0;
    for (const auto& metric : metrics) {
        switch (metric.kind) {
            case MetricKind::WallClockNs:
                // Already covered by the iteration-level wall clock above.
                break;
            case MetricKind::Counter: {
                const auto delta = counters_after[counter_index]
                                  - counters_before[counter_index];
                all_regions[metric.name].push_back(static_cast<double>(delta));
                ++counter_index;
                break;
            }
            case MetricKind::Evaluation: {
                auto found = evaluation_metrics.find(metric.name);
                if (found != evaluation_metrics.end()) {
                    all_regions[metric.name].push_back(found->second);
                } else {
                    logd("[bench_framework] evaluator did not return metric '{}'", metric.name);
                }
                break;
            }
        }
    }

    if (by_regions) {
        const auto descriptor_total = bioseq::instr::descriptor_count();
        for (std::uint32_t descriptor_id = 1; descriptor_id <= descriptor_total; ++descriptor_id) {
            const auto* descriptor = bioseq::instr::descriptor_lookup(descriptor_id);
            if (descriptor == nullptr || descriptor->name == nullptr) continue;

            std::uint64_t total_ns_for_this_region = 0;
            std::uint64_t visits = 0;

            // Walk loop aggregates first (loop scopes also write into the
            // base region table, but the loop record carries iteration data
            // we don't need here — we just need total_ns).
            const auto* loop_agg = profiler_block.loop_aggregates.try_get(descriptor_id);
            if (loop_agg != nullptr && loop_agg->visits > 0) {
                total_ns_for_this_region = loop_agg->total_ns;
                visits = loop_agg->visits;
            } else {
                const auto* region_agg = profiler_block.aggregates.try_get(descriptor_id);
                if (region_agg != nullptr && region_agg->visits > 0) {
                    total_ns_for_this_region = region_agg->total_ns;
                    visits = region_agg->visits;
                }
            }
            if (visits == 0) continue;
            cell.samples[descriptor->name]["wall_clock_ns"]
                .push_back(static_cast<double>(total_ns_for_this_region));
        }
    }
}

// ----------------- Counter snapshotting -----------------

/// Look up `bioseq::instr::Counter*` for every metric whose kind is
/// `Counter`. Called once before the iteration loop because the underlying
/// `counter()` lookup is a cold-path mutex acquisition; we never want to
/// pay it per iteration.
[[nodiscard]] inline std::vector<bioseq::instr::Counter*> resolve_counter_handles(
    const std::vector<MetricSpec>& metrics) noexcept
{
    std::vector<bioseq::instr::Counter*> handles;
    handles.reserve(metrics.size());
    for (const auto& metric : metrics) {
        if (metric.kind != MetricKind::Counter) continue;
        handles.push_back(&bioseq::instr::counter(metric.counter_name));
    }
    return handles;
}

/// Read the current value of every resolved counter. Returns a vector
/// aligned with `handles` so the call site can do `after - before`
/// per-index. Counter reads are relaxed atomic loads.
[[nodiscard]] inline std::vector<std::int64_t> snapshot_counters(
    const std::vector<bioseq::instr::Counter*>& handles) noexcept
{
    std::vector<std::int64_t> values;
    values.reserve(handles.size());
    for (const auto* handle : handles) {
        values.push_back(bioseq::instr::counter_value(*handle));
    }
    return values;
}

// ----------------- Concurrency helpers -----------------

/// True iff any metric in this benchmark is `kind == Counter`. Such
/// benchmarks must run iterations serially so the global atomic counter
/// snapshots produce well-defined deltas.
[[nodiscard]] inline bool benchmark_uses_counter_metric(const BenchmarkConfig& config) noexcept {
    for (const auto& metric : config.metrics) {
        if (metric.kind == MetricKind::Counter) return true;
    }
    return false;
}

// ----------------- run_unit driver -----------------

template<typename Input, typename Output>
inline void run_typed_benchmark(
    BenchmarkExecutionState& state,
    BenchmarkContext& context,
    const std::function<Input(std::string_view, std::string_view, simdjson::dom::element)>& on_input,
    const std::function<Output(std::string_view, const ParamMap&, const Input&)>& on_run,
    const std::function<std::map<std::string, double>(std::string_view, const ParamMap&, const Output&)>& on_evaluate)
{
    using bioseq::instr::installed_profiler_scope;
    using bioseq::instr::ProfilerBlock;

    const auto test_case_count = state.test_cases.size();
    std::vector<std::optional<Input>> cached_inputs(test_case_count);
    std::vector<std::once_flag>       input_once_flags(test_case_count);
    const std::string                 data_dir = context.data_dir;

    auto ensure_input = [&](const Cell& cell) -> const Input& {
        std::call_once(input_once_flags[cell.test_case_index], [&] {
            cached_inputs[cell.test_case_index].emplace(
                on_input(data_dir, cell.test_case->name, cell.test_case->data));
        });
        return *cached_inputs[cell.test_case_index];
    };

    const auto counter_handles = resolve_counter_handles(state.config.metrics);
    const bool by_regions = state.config.by_regions;

    /// Run exactly one iteration of one cell. Hot path; called once per
    /// (cell × iter_index) work unit.
    auto run_one_iteration = [&](Cell& cell) {
        const auto& tuple = *cell.tuple;
        const Input& input = ensure_input(cell);

        // Per-iteration profiler block. Construction zero-inits aggregate
        // tables (see bioseq_instr.aggregate); no carry-over even if a
        // worker thread is reused between iterations.
        ProfilerBlock<profiler_args_t> profiler_block{};
        installed_profiler_scope<profiler_args_t> profiler_scope{profiler_block};

        const auto counters_before = snapshot_counters(counter_handles);

        const auto t_begin_ns = bioseq::instr::clock::now_ns();
        const Output output = on_run(tuple.algorithm_name, tuple.params, input);
        const auto t_end_ns = bioseq::instr::clock::now_ns();
        const auto iteration_wall_clock_ns = static_cast<double>(t_end_ns - t_begin_ns);

        const auto counters_after = snapshot_counters(counter_handles);

        const auto evaluation_metrics = on_evaluate(tuple.algorithm_name, tuple.params, output);

        append_iteration_samples(cell, by_regions, profiler_block,
                                  iteration_wall_clock_ns, state.config.metrics,
                                  counters_before, counters_after, evaluation_metrics);

        if (context.progress != nullptr) {
            context.progress->tick(context.benchmark_index);
        }
    };

    // Build a flat work-unit list (Cell*, iteration_index) so TBB can
    // distribute *iterations* across workers, not just whole cells. Without
    // this, a single-cell benchmark with 1000 iterations would pin to one
    // thread regardless of `--threads`.
    std::vector<Cell*> work_items;
    std::size_t total_units = 0;
    for (const auto& cell_ptr : state.cells) total_units += cell_ptr->iterations;
    work_items.reserve(total_units);
    for (auto& cell_ptr : state.cells) {
        for (std::size_t i = 0; i < cell_ptr->iterations; ++i) {
            work_items.push_back(cell_ptr.get());
        }
    }

    const bool use_parallel = state.config.concurrency.concurrency
                              && !benchmark_uses_counter_metric(state.config);

    if (use_parallel) {
        oneapi::tbb::parallel_for_each(
            work_items.begin(), work_items.end(),
            [&](Cell* cell_ptr) { run_one_iteration(*cell_ptr); });
    } else {
        for (auto* cell_ptr : work_items) {
            run_one_iteration(*cell_ptr);
        }
    }
}

// ----------------- Rule evaluation -----------------

/// Return the median of a sample vector via `std::nth_element`. Empty input
/// returns 0. Used as the rule comparison aggregator (mean is too sensitive
/// to tail outliers — see BENCH_V2_IMPL_PLAN.md §7a).
[[nodiscard]] inline double median_of(const std::vector<double>& samples) noexcept {
    if (samples.empty()) return 0.0;
    std::vector<double> copy = samples;
    const auto middle = copy.size() / 2;
    std::nth_element(copy.begin(), copy.begin() + middle, copy.end());
    return copy[middle];
}

/// True iff `tuple` satisfies every constraint in `selector`. The selector's
/// `algorithm_key` must equal the tuple's algorithm name, and every
/// `(param_name, expected_value)` in `selector.match` must appear in the
/// tuple's params with the expected value. Empty `match` matches everything.
[[nodiscard]] inline bool selector_matches(
    const RuleSelector& selector,
    const ExecutionTuple& tuple) noexcept
{
    if (selector.algorithm_key != tuple.algorithm_name) return false;
    for (const auto& [required_key, required_value] : selector.match) {
        auto it = tuple.params.find(required_key);
        if (it == tuple.params.end()) return false;
        if (!(it->second == required_value)) return false;
    }
    return true;
}

/// Apply the rule comparator to two aggregate values. Spec semantics:
/// - `<` : `lhs < rhs * (1 - threshold% + tolerance%)`
/// - `>` : `lhs > rhs * (1 + threshold% - tolerance%)` (extrapolated by
///         symmetry; spec only documents `<`/`=`)
/// - `=` : `|lhs - rhs| <= rhs * tolerance%` (asymmetric on rhs as the
///         baseline reference, matching the spec example).
[[nodiscard]] inline bool comparator_passes(
    const RuleComparator& cmp,
    double lhs_aggregate,
    double rhs_aggregate) noexcept
{
    const double threshold = cmp.threshold_pct / 100.0;
    const double tolerance = cmp.tolerance_pct / 100.0;
    switch (cmp.op) {
        case CompareOp::LessThan:
            return lhs_aggregate < rhs_aggregate * (1.0 - threshold + tolerance);
        case CompareOp::GreaterThan:
            return lhs_aggregate > rhs_aggregate * (1.0 + threshold - tolerance);
        case CompareOp::Equal:
            return std::abs(lhs_aggregate - rhs_aggregate)
                   <= std::abs(rhs_aggregate) * tolerance;
    }
    return false;
}

/// Apply every rule against every test case and collect failures.
/// For each (rule, test_case) pair we look up matching cells on the LHS
/// and RHS, take the median of each side's `all_regions[<metric>]` array,
/// run the comparator, and synthesise a `Violation` if it fails.
inline void evaluate_rules(const BenchmarkExecutionState& state,
                            std::vector<Violation>& violations)
{
    // Index cells by (test_case_name, &tuple) for quick lookup.
    for (const auto& metric : state.config.metrics) {
        for (const auto& rule : metric.rules) {
            for (const auto& [test_case_name, _iterations] : state.config.for_each) {
                std::vector<const Cell*> lhs_cells;
                std::vector<const Cell*> rhs_cells;
                for (const auto& cell_ptr : state.cells) {
                    if (cell_ptr->test_case->name != test_case_name) continue;
                    if (selector_matches(rule.lhs, *cell_ptr->tuple)) lhs_cells.push_back(cell_ptr.get());
                    if (selector_matches(rule.rhs, *cell_ptr->tuple)) rhs_cells.push_back(cell_ptr.get());
                }
                for (const auto* lhs_cell : lhs_cells) {
                    for (const auto* rhs_cell : rhs_cells) {
                        // Look up the metric values inside the all_regions bucket.
                        auto lhs_region_it = lhs_cell->samples.find("all_regions");
                        auto rhs_region_it = rhs_cell->samples.find("all_regions");
                        if (lhs_region_it == lhs_cell->samples.end()
                            || rhs_region_it == rhs_cell->samples.end()) continue;
                        auto lhs_metric_it = lhs_region_it->second.find(rule.metric_name);
                        auto rhs_metric_it = rhs_region_it->second.find(rule.metric_name);
                        if (lhs_metric_it == lhs_region_it->second.end()
                            || rhs_metric_it == rhs_region_it->second.end()) continue;
                        const double lhs_aggregate = median_of(lhs_metric_it->second);
                        const double rhs_aggregate = median_of(rhs_metric_it->second);
                        if (comparator_passes(rule.cmp, lhs_aggregate, rhs_aggregate)) continue;

                        Violation violation;
                        violation.config_path     = state.config.config_path;
                        violation.benchmark_key   = state.config.benchmark_key;
                        violation.rule_name       = rule.name;
                        violation.test_case       = test_case_name;
                        violation.metric          = rule.metric_name;
                        violation.lhs_algorithm   = lhs_cell->tuple->algorithm_name;
                        violation.lhs_full_params = lhs_cell->tuple->params;
                        violation.rhs_algorithm   = rhs_cell->tuple->algorithm_name;
                        violation.rhs_full_params = rhs_cell->tuple->params;
                        violation.args            = {lhs_aggregate, rhs_aggregate};
                        violations.push_back(std::move(violation));
                    }
                }
            }
        }
    }
}

// ----------------- Report writing -----------------
//
// All emit_* helpers here cooperate with simdjson::builder::string_builder.
// We deliberately track comma placement by hand because the builder leaves
// that to the caller (see SIMDJSON_REF.md §"Overview: string_builder").

/// Emit one ConfigValue scalar — switches on the runtime kind. Caller is
/// expected to have written the key + colon already.
inline void emit_value_scalar(simdjson::builder::string_builder& builder,
                                const ConfigValue& value)
{
    switch (value.kind) {
        case ConfigValue::Kind::String:
            builder.escape_and_append_with_quotes(std::string_view{value.str_v});
            return;
        case ConfigValue::Kind::Integer:
            builder.append(value.int_v);
            return;
        case ConfigValue::Kind::Double:
            builder.append(value.dbl_v);
            return;
        case ConfigValue::Kind::Boolean:
            builder.append(value.bool_v);
            return;
    }
}

/// Emit `"<key>":` and prime the builder for the value.
inline void emit_key(simdjson::builder::string_builder& builder, std::string_view key) {
    builder.escape_and_append_with_quotes(key);
    builder.append_colon();
}

/// Emit `"<key>": "<value>"`.
inline void emit_kv_string(simdjson::builder::string_builder& builder,
                            std::string_view key, std::string_view value)
{
    emit_key(builder, key);
    builder.escape_and_append_with_quotes(value);
}

/// Emit `"<key>": <body>` where `body` is a callable that writes the value.
template<typename BodyFn>
inline void emit_kv_with_body(simdjson::builder::string_builder& builder,
                                std::string_view key, BodyFn&& body_fn)
{
    emit_key(builder, key);
    body_fn(builder);
}

/// Emit a `{...}` object whose contents come from `body`.
template<typename BodyFn>
inline void emit_object_with_body(simdjson::builder::string_builder& builder,
                                    BodyFn&& body_fn)
{
    builder.start_object();
    body_fn(builder);
    builder.end_object();
}

/// Iterate a range and call `each(builder, item, is_first)` once per element,
/// writing commas between siblings. The element body is responsible for
/// emitting its own key + value (or whole object).
template<typename Range, typename EachFn>
inline void emit_range_with_commas(simdjson::builder::string_builder& builder,
                                    Range&& range, EachFn&& each_fn)
{
    bool first = true;
    for (const auto& element : range) {
        if (!first) builder.append_comma();
        first = false;
        each_fn(builder, element);
    }
}

/// Emit a JSON object whose keys are algorithm parameters.
inline void emit_param_object(simdjson::builder::string_builder& builder,
                               const ParamMap& params)
{
    emit_object_with_body(builder, [&](auto& inner) {
        emit_range_with_commas(inner, params, [](auto& b, const auto& kv_pair) {
            emit_key(b, std::string_view{kv_pair.first});
            emit_value_scalar(b, kv_pair.second);
        });
    });
}

/// Emit `[v0, v1, ...]` from a numeric vector. Used for sample arrays.
inline void emit_double_array(simdjson::builder::string_builder& builder,
                               const std::vector<double>& values)
{
    builder.start_array();
    emit_range_with_commas(builder, values, [](auto& b, double sample) {
        b.append(sample);
    });
    builder.end_array();
}

/// Serialise the per-benchmark execution state map plus the global
/// violations list into `<report_dir>/report.json`. Returns the resolved
/// path on success, empty string on I/O / serialisation failure. The output
/// matches the layout in BENCH_V2_CODE.md §"Output report.json".
[[nodiscard]] inline std::string write_report(
    const std::string& report_dir,
    const std::map<std::string, std::unique_ptr<BenchmarkExecutionState>>& reports,
    const std::vector<Violation>& violations)
{
    constexpr std::size_t kInitialCapacity = 1u << 18;  // 256 KiB up front
    simdjson::builder::string_builder builder{kInitialCapacity};

    /// Emit one violation as a JSON object — exactly the shape the spec
    /// shows in BENCH_V2_CODE.md (`location`, `benchmark`, `rule_name`,
    /// `execution_tuple`, `test_case`, `metric`, `args`).
    auto emit_one_violation = [&](auto& b, const Violation& violation) {
        emit_object_with_body(b, [&](auto& inner) {
            emit_kv_string(inner, "location",  violation.config_path);
            inner.append_comma();
            emit_kv_string(inner, "benchmark", violation.benchmark_key);
            inner.append_comma();
            emit_kv_string(inner, "rule_name", violation.rule_name);
            inner.append_comma();
            emit_kv_with_body(inner, "execution_tuple", [&](auto& tuple_b) {
                emit_object_with_body(tuple_b, [&](auto& tuple_inner) {
                    emit_key(tuple_inner, std::string_view{violation.lhs_algorithm});
                    emit_param_object(tuple_inner, violation.lhs_full_params);
                    if (!violation.rhs_algorithm.empty()) {
                        tuple_inner.append_comma();
                        emit_key(tuple_inner, std::string_view{violation.rhs_algorithm});
                        emit_param_object(tuple_inner, violation.rhs_full_params);
                    }
                });
            });
            inner.append_comma();
            emit_kv_string(inner, "test_case", violation.test_case);
            inner.append_comma();
            emit_kv_string(inner, "metric",    violation.metric);
            inner.append_comma();
            emit_kv_with_body(inner, "args", [&](auto& args_b) {
                args_b.start_array();
                args_b.append(violation.args[0]);
                args_b.append_comma();
                args_b.append(violation.args[1]);
                args_b.end_array();
            });
        });
    };

    /// Pick the cell that pairs a given (tuple, test_case_name). Returns
    /// nullptr if no cell exists (e.g. matrix expansion produced no entry
    /// for that test case under this algorithm — should not happen, but
    /// the writer is defensive).
    auto find_cell = [&](const BenchmarkExecutionState& state,
                          const ExecutionTuple* tuple,
                          std::string_view test_case_name) -> const Cell* {
        for (const auto& cell_ptr : state.cells) {
            if (cell_ptr->tuple != tuple) continue;
            if (cell_ptr->test_case->name != test_case_name) continue;
            return cell_ptr.get();
        }
        return nullptr;
    };

    /// Emit the per-test-case object for one cell: every region (or just
    /// "all_regions" when by_regions=false) and the metric arrays inside it.
    auto emit_test_case_block = [&](auto& b, const Cell& cell) {
        emit_object_with_body(b, [&](auto& region_outer) {
            emit_range_with_commas(region_outer, cell.samples,
                [](auto& region_b, const auto& region_kv) {
                    emit_kv_with_body(region_b,
                        std::string_view{region_kv.first},
                        [&](auto& metrics_b) {
                            emit_object_with_body(metrics_b, [&](auto& metric_outer) {
                                emit_range_with_commas(metric_outer, region_kv.second,
                                    [](auto& metric_b, const auto& metric_kv) {
                                        emit_kv_with_body(metric_b,
                                            std::string_view{metric_kv.first},
                                            [&](auto& arr_b) {
                                                emit_double_array(arr_b, metric_kv.second);
                                            });
                                    });
                            });
                        });
                });
        });
    };

    /// Emit one execution tuple's `{ "tuple": {...}, "test_cases": {...} }`.
    auto emit_one_execution_tuple = [&](auto& b,
                                          const BenchmarkExecutionState& state,
                                          const ExecutionTuple& tuple) {
        emit_object_with_body(b, [&](auto& outer) {
            emit_kv_with_body(outer, "tuple", [&](auto& tuple_b) {
                emit_object_with_body(tuple_b, [&](auto& alg_b) {
                    emit_key(alg_b, std::string_view{tuple.algorithm_name});
                    emit_param_object(alg_b, tuple.params);
                });
            });
            outer.append_comma();
            emit_kv_with_body(outer, "test_cases", [&](auto& cases_b) {
                emit_object_with_body(cases_b, [&](auto& cases_inner) {
                    bool first_tc_in_tuple = true;
                    for (const auto& [test_case_name, _iter] : state.config.for_each) {
                        const Cell* cell_for_pair = find_cell(state, &tuple, test_case_name);
                        if (cell_for_pair == nullptr) continue;
                        if (!first_tc_in_tuple) cases_inner.append_comma();
                        first_tc_in_tuple = false;
                        emit_kv_with_body(cases_inner,
                            std::string_view{test_case_name},
                            [&](auto& inner_b) { emit_test_case_block(inner_b, *cell_for_pair); });
                    }
                });
            });
        });
    };

    /// Emit one benchmark's full sub-object (title / description / location /
    /// test_case_descriptions / execution_tuples).
    auto emit_one_benchmark = [&](auto& b,
                                    std::string_view key,
                                    const BenchmarkExecutionState& state) {
        emit_kv_with_body(b, key, [&](auto& body_b) {
            emit_object_with_body(body_b, [&](auto& inner) {
                emit_kv_string(inner, "title",       state.title);
                inner.append_comma();
                emit_kv_string(inner, "description", state.config.description);
                inner.append_comma();
                emit_kv_string(inner, "location",    state.config.config_path);
                inner.append_comma();
                emit_kv_with_body(inner, "test_case_descriptions", [&](auto& tcd_b) {
                    emit_object_with_body(tcd_b, [&](auto& tcd_inner) {
                        emit_range_with_commas(tcd_inner, state.test_cases,
                            [](auto& entry_b, const auto& test_case) {
                                emit_kv_string(entry_b,
                                    std::string_view{test_case.name},
                                    std::string_view{test_case.description});
                            });
                    });
                });
                inner.append_comma();
                emit_kv_with_body(inner, "execution_tuples", [&](auto& tuples_b) {
                    tuples_b.start_array();
                    emit_range_with_commas(tuples_b, state.execution_tuples,
                        [&](auto& tuple_b, const ExecutionTuple& exec_tuple) {
                            emit_one_execution_tuple(tuple_b, state, exec_tuple);
                        });
                    tuples_b.end_array();
                });
            });
        });
    };

    emit_object_with_body(builder, [&](auto& root_b) {
        emit_kv_with_body(root_b, "rule_violations", [&](auto& arr_b) {
            arr_b.start_array();
            emit_range_with_commas(arr_b, violations,
                [&](auto& vb, const Violation& violation) {
                    emit_one_violation(vb, violation);
                });
            arr_b.end_array();
        });
        root_b.append_comma();
        emit_kv_with_body(root_b, "benchmarks", [&](auto& benchmarks_b) {
            emit_object_with_body(benchmarks_b, [&](auto& bench_inner) {
                emit_range_with_commas(bench_inner, reports,
                    [&](auto& entry_b, const auto& benchmark_kv) {
                        emit_one_benchmark(entry_b,
                            std::string_view{benchmark_kv.first},
                            *benchmark_kv.second);
                    });
            });
        });
    });

    std::string_view payload;
    if (builder.view().get(payload) != simdjson::SUCCESS) {
        loge("[bench_framework] simdjson string_builder reported allocation failure");
        return {};
    }

    std::filesystem::create_directories(report_dir);
    const auto report_path = std::filesystem::path{report_dir} / "report.json";
    std::ofstream output_file(report_path, std::ios::trunc | std::ios::binary);
    if (!output_file) {
        loge("[bench_framework] failed to open '{}' for writing", report_path.string());
        return {};
    }
    output_file.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    output_file.close();
    return report_path.string();
}

// ----------------- Stderr violations summary -----------------

/// Render a ParamMap as a human-readable single-line `k=v, k=v, ...` blob
/// for the stderr violations summary. Not meant for re-parsing — use
/// `emit_param_object` for the JSON report.
[[nodiscard]] inline std::string format_param_map(const ParamMap& params) noexcept {
    std::string result;
    bool first = true;
    for (const auto& [name, value] : params) {
        if (!first) result += ", ";
        first = false;
        result += name;
        result += "=";
        switch (value.kind) {
            case ConfigValue::Kind::String:  result += value.str_v; break;
            case ConfigValue::Kind::Integer: result += std::to_string(value.int_v); break;
            case ConfigValue::Kind::Double:  result += std::to_string(value.dbl_v); break;
            case ConfigValue::Kind::Boolean: result += value.bool_v ? "true" : "false"; break;
        }
    }
    return result;
}

/// Spelling of a `CompareOp` that the user typed in JSON. Used only by the
/// stderr summary printer.
[[nodiscard]] inline std::string format_op(CompareOp op) noexcept {
    switch (op) {
        case CompareOp::LessThan:    return "<";
        case CompareOp::GreaterThan: return ">";
        case CompareOp::Equal:       return "=";
    }
    return "?";
}

/// Pretty-print every violation to stderr at end-of-run. Uses raw stdio
/// (`std::fprintf`) instead of JustLog because the violation summary is
/// final-pass operator output, not part of the asynchronous log queue —
/// we want it visible even if the fmtlog background thread has already
/// drained and exited.
inline void print_violations(const std::vector<Violation>& violations) noexcept {
    if (violations.empty()) {
        std::fprintf(stderr,
            "============================================================\n"
            "BENCH FRAMEWORK — all rules passed (0 violations)\n"
            "============================================================\n");
        std::fflush(stderr);
        return;
    }

    std::fprintf(stderr,
        "============================================================\n"
        "BENCH FRAMEWORK — %zu rule violation%s\n"
        "============================================================\n",
        violations.size(), violations.size() == 1 ? "" : "s");

    for (const auto& violation : violations) {
        std::fprintf(stderr,
            "[%s] %s (test_case=%s, metric=%s)\n",
            violation.benchmark_key.c_str(),
            violation.rule_name.c_str(),
            violation.test_case.c_str(),
            violation.metric.c_str());
        std::fprintf(stderr,
            "    LHS %s { %s }\n        median = %.6g\n",
            violation.lhs_algorithm.c_str(),
            format_param_map(violation.lhs_full_params).c_str(),
            violation.args[0]);
        std::fprintf(stderr,
            "    RHS %s { %s }\n        median = %.6g\n",
            violation.rhs_algorithm.c_str(),
            format_param_map(violation.rhs_full_params).c_str(),
            violation.args[1]);
        std::fprintf(stderr, "\n");
    }

    std::fprintf(stderr,
        "============================================================\n");
    std::fflush(stderr);
}

// ----------------- CLI parsing -----------------

/// Parse the framework's three CLI flags out of `argv`. Unknown flags are
/// silently ignored (the user may layer their own flag parsing on top by
/// inspecting argv before calling `run`). Runs first inside `run_impl` so a
/// `--report` override is applied before any directory creation.
[[nodiscard]] inline ParsedCli parse_cli(int argc, char** argv) noexcept {
    ParsedCli parsed;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--fast") {
            parsed.fast_filter = true;
        } else if (arg == "--threads" && (i + 1) < argc) {
            const std::string thread_text{argv[i + 1]};
            try {
                const int threads = std::stoi(thread_text);
                if (threads > 0) parsed.threads_override = static_cast<unsigned>(threads);
            } catch (...) {
                loge("[bench_framework] --threads requires an integer (got '{}')", thread_text);
            }
            ++i;
        } else if (arg == "--report" && (i + 1) < argc) {
            parsed.report_override = std::string{argv[i + 1]};
            ++i;
        }
    }
    return parsed;
}

// ----------------- Progress dashboard -----------------

/// Background thread body for the progress dashboard. Two paths:
/// 1. **TTY**: render an ftxui document to stderr every 150 ms with one
///    gauge per benchmark. Cursor reset between frames keeps the screen
///    in place so output looks like a live UI.
/// 2. **Non-TTY (CI / pipes)**: emit a JustLog info line every 5 s with
///    `<benchmark_title> <done>/<total>` per benchmark, so log scrapers
///    still see progress.
/// Polls `progress.running` every 200 ms in the non-TTY path so the
/// shutdown signal kicks in within ~200 ms of the workers finishing.
inline void render_progress_loop(ProgressState& progress) noexcept {
    using namespace std::chrono_literals;
    using namespace ftxui;

    const bool tty = progress.tty_render.load(std::memory_order_relaxed);
    if (!tty) {
        // No TTY → periodic logi() lines every ~5 s. We poll `running` every
        // 200 ms so shutdown is responsive (a 5 s blocking sleep would force
        // every CI build to wait an extra five seconds at the end).
        constexpr auto kPollInterval = std::chrono::milliseconds{200};
        constexpr int  kTicksPerLog  = 25;     // 25 × 200 ms == 5 s
        int ticks_until_next_log = kTicksPerLog;
        while (progress.running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(kPollInterval);
            if (--ticks_until_next_log > 0) continue;
            ticks_until_next_log = kTicksPerLog;
            for (const auto& per : progress.per_benchmark) {
                const auto done_now = per->done.load(std::memory_order_relaxed);
                logi("[bench_framework] [{}] {}/{}", per->title, done_now, per->total);
            }
        }
        return;
    }

    std::string reset_position;
    auto render_once = [&] {
        Elements rows;
        rows.push_back(text("bench_framework — live progress") | bold);
        for (const auto& per : progress.per_benchmark) {
            const auto done_now = per->done.load(std::memory_order_relaxed);
            const float fraction = per->total == 0
                ? 1.0f
                : static_cast<float>(done_now) / static_cast<float>(per->total);
            const auto label = per->benchmark_key + " (" + per->title + ")";
            rows.push_back(hbox({
                text(label) | size(WIDTH, EQUAL, 48),
                gauge(fraction) | flex,
                text(" " + std::to_string(done_now) + "/" + std::to_string(per->total)),
            }));
        }
        auto document = vbox(std::move(rows));
        const int height = static_cast<int>(progress.per_benchmark.size()) + 1;
        auto screen = Screen::Create(Dimension::Fit(document), Dimension::Fixed(height));
        Render(screen, document);
        std::fputs(reset_position.c_str(), stderr);
        std::fputs(screen.ToString().c_str(), stderr);
        std::fflush(stderr);
        reset_position = screen.ResetPosition();
    };

    while (progress.running.load(std::memory_order_relaxed)) {
        render_once();
        std::this_thread::sleep_for(150ms);
    }
    render_once();   // final repaint with all bars at 100 %
    std::fputs("\n", stderr);
    std::fflush(stderr);
}

// ----------------- run_impl -----------------

/// Top-level orchestrator. Owns: CLI parsing, TBB arena setup, cross-bench
/// shared_mutex, the ProgressState lifetime, the violations + reports maps,
/// and the final report write + violations print. Returns 0 if every rule
/// passed and every config parsed; returns 1 otherwise (duplicate
/// benchmark_key, parse failure, or any rule violation).
[[nodiscard]] inline int run_impl(int argc, char** argv,
                                   std::string report_dir,
                                   std::string data_dir,
                                   std::vector<ErasedBenchmark> benchmarks)
{
    const auto cli = parse_cli(argc, argv);
    if (cli.report_override.has_value()) {
        report_dir = *cli.report_override;
    }

    logi("[bench_framework] starting: report_dir={} data_dir={} benchmarks={}",
                 report_dir, data_dir, benchmarks.size());

    const auto thread_count = cli.threads_override.has_value()
        ? *cli.threads_override
        : std::max<unsigned>(1u, std::thread::hardware_concurrency() == 0
                                  ? 1u
                                  : std::thread::hardware_concurrency() - 1u);
    logi("[bench_framework] thread_count={}", thread_count);

    // Filter benchmarks by --fast.
    std::vector<ErasedBenchmark> selected;
    selected.reserve(benchmarks.size());
    for (auto& erased : benchmarks) {
        // The filter is applied on the per-benchmark `fast` field, but we
        // can't know that until parse. To keep run_impl simple, we ALWAYS
        // parse, then the closure applies the filter inside before doing
        // expensive work. For cleaner semantics we move the filtering into
        // the closure (see typed dispatch).
        selected.push_back(std::move(erased));
    }

    // Uniqueness check on benchmark_key.
    std::set<std::string> seen_keys;
    for (const auto& erased : selected) {
        const auto inserted = seen_keys.insert(erased.benchmark_key);
        if (!inserted.second) {
            loge("[bench_framework] duplicate benchmark_key '{}' across create_benchmark calls",
                         erased.benchmark_key);
            return 1;
        }
    }

    oneapi::tbb::global_control concurrency_limit(
        oneapi::tbb::global_control::max_allowed_parallelism, thread_count);
    oneapi::tbb::task_arena bench_arena(static_cast<int>(thread_count));

    std::shared_mutex cross_bench_mu;
    std::mutex        violations_mu;
    std::mutex        reports_mu;
    std::vector<Violation> violations;
    std::map<std::string, std::unique_ptr<BenchmarkExecutionState>> reports;

    ProgressState progress;
    progress.tty_render.store(::isatty(STDERR_FILENO) != 0, std::memory_order_relaxed);
    progress.per_benchmark.reserve(selected.size());
    for (const auto& erased : selected) {
        auto entry = std::make_unique<ProgressState::Per>();
        entry->title = erased.title;
        entry->benchmark_key = erased.benchmark_key;
        entry->total = 0;       // populated after parse + matrix expansion
        progress.per_benchmark.push_back(std::move(entry));
    }

    std::thread progress_thread{[&progress] { render_progress_loop(progress); }};

    bench_arena.execute([&] {
        oneapi::tbb::parallel_for_each(
            selected.begin(), selected.end(),
            [&](ErasedBenchmark& erased) {
                const std::size_t benchmark_index = static_cast<std::size_t>(
                    &erased - selected.data());
                BenchmarkContext context;
                context.data_dir          = data_dir;
                context.report_dir        = report_dir;
                context.fast_filter       = cli.fast_filter;
                context.cross_bench_mu    = &cross_bench_mu;
                context.progress          = &progress;
                context.benchmark_index   = benchmark_index;
                context.violations_mu     = &violations_mu;
                context.violations        = &violations;
                context.reports_mu        = &reports_mu;
                context.reports           = &reports;

                erased.execute(context);
            });
    });

    progress.running.store(false, std::memory_order_relaxed);
    progress_thread.join();

    logi("[bench_framework] all benchmarks finished, writing report...");
    const auto report_path = write_report(report_dir, reports, violations);
    if (!report_path.empty()) {
        logi("[bench_framework] wrote {}", report_path);
    }

    print_violations(violations);

    return violations.empty() ? 0 : 1;
}

}  // namespace detail

// ----------------- create_benchmark template body -----------------

template<typename Input, typename Output>
[[nodiscard]] ErasedBenchmark create_benchmark(
    std::string title,
    std::string benchmark_key,
    std::string config_path,
    std::function<Input(std::string_view, std::string_view, simdjson::dom::element)> on_input,
    std::function<Output(std::string_view, const ParamMap&, const Input&)> on_run,
    std::function<std::map<std::string, double>(std::string_view, const ParamMap&, const Output&)> on_evaluate)
{
    ErasedBenchmark erased;
    erased.title         = std::move(title);
    erased.benchmark_key = std::move(benchmark_key);
    erased.config_path   = std::move(config_path);

    erased.execute = [erased_title         = erased.title,
                       erased_benchmark_key = erased.benchmark_key,
                       erased_config_path   = erased.config_path,
                       captured_on_input    = std::move(on_input),
                       captured_on_run      = std::move(on_run),
                       captured_on_evaluate = std::move(on_evaluate)
                      ](BenchmarkContext& context) {
        auto state_holder = std::make_unique<detail::BenchmarkExecutionState>();
        auto& state = *state_holder;
        state.title = erased_title;

        if (!detail::parse_config_file(erased_config_path, erased_benchmark_key, state)) {
            loge("[bench_framework] benchmark '{}' aborted (parse failure)",
                         erased_benchmark_key);
            return;
        }

        // --fast filter: skip if the user requested fast mode but this
        // benchmark is not flagged fast. Without --fast we run everything.
        if (context.fast_filter && !state.config.fast) {
            logi("[bench_framework] skipping '{}' under --fast (fast=false)",
                         erased_benchmark_key);
            return;
        }

        bool expansion_ok = true;
        state.execution_tuples = detail::expand_matrix(state.config, expansion_ok);
        if (!expansion_ok || state.execution_tuples.empty()) {
            loge("[bench_framework] benchmark '{}' produced no execution tuples",
                         erased_benchmark_key);
            return;
        }
        state.cells = detail::build_cells(state);

        std::uint64_t total_iterations = 0;
        for (const auto& cell_ptr : state.cells) total_iterations += cell_ptr->iterations;
        if (context.progress != nullptr
            && context.benchmark_index < context.progress->per_benchmark.size()) {
            context.progress->per_benchmark[context.benchmark_index]->total = total_iterations;
        }

        // Acquire the cross-benchmark lock per concurrency.bench_lock.
        std::shared_lock<std::shared_mutex> shared_guard;
        std::unique_lock<std::shared_mutex> unique_guard;
        if (state.config.concurrency.bench_lock) {
            unique_guard = std::unique_lock{*context.cross_bench_mu};
        } else {
            shared_guard = std::shared_lock{*context.cross_bench_mu};
        }

        detail::run_typed_benchmark<Input, Output>(state, context,
                                                    captured_on_input,
                                                    captured_on_run,
                                                    captured_on_evaluate);

        // Evaluate rules and stash this benchmark's report data.
        std::vector<detail::Violation> local_violations;
        detail::evaluate_rules(state, local_violations);

        if (context.violations != nullptr && context.violations_mu != nullptr) {
            std::lock_guard<std::mutex> guard(*context.violations_mu);
            for (auto& violation : local_violations) {
                context.violations->push_back(std::move(violation));
            }
        }
        if (context.reports != nullptr && context.reports_mu != nullptr) {
            std::lock_guard<std::mutex> guard(*context.reports_mu);
            context.reports->emplace(erased_benchmark_key, std::move(state_holder));
        }
    };

    return erased;
}

}  // namespace bioseq::bench_framework
