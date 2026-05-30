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
 * Design choices documented in main/DOCS/BENCHMARK_FRAMEWORK/BENCH_V2_IMPL_PLAN__DOC0198.md. Keep that file in
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
#include <oneapi/tbb/task_group.h>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include <hwinfo/platform.h>
#include <hwinfo/ram.h>

#include <cpptrace/cpptrace.hpp>
#include <cpptrace/from_current.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
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
    /// Recognised scalar kinds plus a sentinel `NonScalar` for arrays /
    /// objects / nulls. The framework refuses to treat NonScalar values as
    /// matching any declared parameter type, so semantic validation surfaces
    /// `value_type_mismatch` for nested structures used where a scalar was
    /// expected. `string_value` carries the source JSON spelling for
    /// NonScalar so error messages can show what the user wrote.
    enum class Kind : std::uint8_t { String, Integer, Double, Boolean, NonScalar };

    Kind         kind = Kind::String;
    std::string  string_value{};
    std::int64_t integer_value = 0;
    double       double_value = 0.0;
    bool         boolean_value = false;

    [[nodiscard]] constexpr bool is_string()    const noexcept { return kind == Kind::String; }
    [[nodiscard]] constexpr bool is_integer()   const noexcept { return kind == Kind::Integer; }
    [[nodiscard]] constexpr bool is_double()    const noexcept { return kind == Kind::Double; }
    [[nodiscard]] constexpr bool is_boolean()   const noexcept { return kind == Kind::Boolean; }
    [[nodiscard]] constexpr bool is_non_scalar() const noexcept { return kind == Kind::NonScalar; }

    [[nodiscard]] std::string_view as_string()  const noexcept { return string_value; }
    [[nodiscard]] std::int64_t     as_integer() const noexcept { return integer_value; }
    [[nodiscard]] double           as_double()  const noexcept { return double_value; }
    [[nodiscard]] bool             as_boolean() const noexcept { return boolean_value; }

    [[nodiscard]] bool operator==(std::string_view rhs) const noexcept {
        return is_string() && string_value == rhs;
    }
    [[nodiscard]] bool operator==(const char* rhs) const noexcept {
        return is_string() && string_value == rhs;
    }
    [[nodiscard]] bool operator==(std::int64_t rhs) const noexcept {
        return is_integer() && integer_value == rhs;
    }
    [[nodiscard]] bool operator==(int rhs) const noexcept {
        return is_integer() && integer_value == static_cast<std::int64_t>(rhs);
    }
    [[nodiscard]] bool operator==(double rhs) const noexcept {
        return is_double() && double_value == rhs;
    }
    [[nodiscard]] bool operator==(bool rhs) const noexcept {
        return is_boolean() && boolean_value == rhs;
    }

    [[nodiscard]] bool operator==(const ConfigValue& other) const noexcept {
        if (kind != other.kind) return false;
        switch (kind) {
            case Kind::String:    return string_value == other.string_value;
            case Kind::Integer:   return integer_value == other.integer_value;
            case Kind::Double:    return double_value == other.double_value;
            case Kind::Boolean:   return boolean_value == other.boolean_value;
            case Kind::NonScalar: return string_value == other.string_value;
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
            out.string_value = std::string{raw_value};
        } else if constexpr (std::is_same_v<T, bool>) {
            out.kind = ConfigValue::Kind::Boolean;
            out.boolean_value = raw_value;
        } else if constexpr (std::is_integral_v<T>) {
            out.kind = ConfigValue::Kind::Integer;
            out.integer_value = static_cast<std::int64_t>(raw_value);
        } else if constexpr (std::is_floating_point_v<T>) {
            out.kind = ConfigValue::Kind::Double;
            out.double_value = static_cast<double>(raw_value);
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
 * @brief Resource budget hints returned by the user's resource estimator.
 *
 * The framework uses this to schedule batches inside a hardcoded 5 GiB
 * memory budget. `base_memory` covers the lifetime of the cached `Input`
 * (allocated by `on_input`, released after `on_cleanup`); `extra_memory`
 * covers only the iteration loop (acquired before the first iteration of
 * a batch, released as soon as the last iteration of that batch returns
 * from `on_run`). Both default to zero, which means "this batch has no
 * memory constraints" — the framework will schedule it whenever a TBB
 * worker is free.
 *
 * The estimator is called exactly once per batch (per `(test_case,
 * group)` pair); the framework records that one answer on the batch
 * and uses it for both the budget reservation and the `on_input` /
 * `on_cleanup` lifecycle. RNG inside the estimator is therefore
 * harmless — the second call that would yield a different value never
 * happens.
 */
struct RunConstraints {
    /// Bytes that must remain reserved between `on_input` and
    /// `on_cleanup` (i.e. for the lifetime of the cached `Input`).
    /// Default 0 = no reservation.
    std::size_t base_memory  = 0;
    /// Bytes additionally reserved while the iteration loop is in
    /// flight on this batch. Released as soon as the loop exits.
    /// Default 0 = no reservation.
    std::size_t extra_memory = 0;
};

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
 * threads — but only across different `(test_case, group)` batches. Within
 * one batch, items run sequentially against one cached `Input`. The
 * captured callable objects must be safe to invoke concurrently across
 * batches (typically pure functions of their arguments).
 *
 * `on_estimate_resources`, `on_input`, `on_cleanup`, and `on_evaluate`
 * MUST NOT throw — the framework does not catch user exceptions.
 *
 * @param title             Human-readable label echoed in the report's `title` field.
 * @param benchmark_key     Unique key used (a) to look up the matching block in
 *                          `config_path`, and (b) as the report's
 *                          `benchmarks.<benchmark_key>` entry. One config file may
 *                          hold multiple benchmark blocks; the key disambiguates
 *                          which block this `create_benchmark` call describes.
 * @param config_path       Path to the bench configuration JSON (validated against
 *                          a built-in JSON schema before use).
 * @param on_estimate_resources  Returns a `RunConstraints` describing the memory
 *                          this batch will need. Called at most once per
 *                          `(test_case, group)` batch, before `on_input`. The
 *                          framework reserves `base_memory` from a shared 5 GiB
 *                          pool before calling `on_input` and `extra_memory` for
 *                          the iteration loop only. Called exactly once per
 *                          batch; RNG inside is harmless because the
 *                          framework only invokes the lambda once per
 *                          `(test_case, group)` pair. Must not throw.
 * @param on_input          Builds the test-case `Input` from the parsed JSON data
 *                          and the framework-determined `RunConstraints`. Called
 *                          exactly once per `(test_case, group)` batch — when a
 *                          test_case is split into N parallel groups, the framework
 *                          calls `on_input` once per group, each with its own
 *                          `Input` instance. Must not throw.
 * @param on_run            Runs the algorithm. Wraps the timing window for the
 *                          built-in `wall_clock_ns` metric. Called sequentially
 *                          inside one batch, concurrently across batches.
 * @param on_evaluate       Returns evaluation metrics computed from the algorithm
 *                          output. Receives the same `Input` that was fed to
 *                          `on_run` so the evaluator can run an independent
 *                          reference pass without polluting the timing window.
 *                          Each map entry is a `(metric_name, metric_value)` pair
 *                          appended to the `all_regions` bucket. Names that
 *                          don't match any `metrics` key declared with
 *                          `type: "evaluation"` are silently dropped.
 *                          Declared-but-not-returned metrics fail the run via
 *                          `missing_evaluation_metric`. Called like `on_run`.
 * @param on_cleanup        Optional teardown callback. Defaults to an empty
 *                          `std::function` (no-op). When non-empty, runs
 *                          once per `(test_case, group)` batch after every
 *                          iteration on that batch's cached `Input` has
 *                          finished. Same first three parameters as
 *                          `on_input` plus the cached `Input&`. Note: when a
 *                          test case is split into N groups, `on_cleanup` is
 *                          called N times (once per group). Runs on the
 *                          batch's TBB worker, so cleanup must not assume
 *                          single-thread serialisation across cleanups —
 *                          but it is single-threaded *within* one batch.
 *                          **Must not throw**, same contract as `on_input`.
 */
template<typename Input, typename Output>
[[nodiscard]] ErasedBenchmark create_benchmark(
    std::string title,
    std::string benchmark_key,
    std::string config_path,
    std::function<RunConstraints(std::string_view data_dir,
                                  std::string_view test_case_name,
                                  simdjson::dom::element test_case_data)> on_estimate_resources,
    std::function<Input(std::string_view data_dir,
                        std::string_view test_case_name,
                        simdjson::dom::element test_case_data,
                        const RunConstraints& constraints)> on_input,
    std::function<Output(std::string_view algorithm_name,
                         const ParamMap& params,
                         const Input& input)> on_run,
    std::function<std::map<std::string, double>(std::string_view algorithm_name,
                                                const ParamMap& params,
                                                const Input&  input,
                                                const Output& output)> on_evaluate,
    std::function<void(std::string_view data_dir,
                       std::string_view test_case_name,
                       simdjson::dom::element test_case_data,
                       const Input& input)> on_cleanup = {});

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

/// Aggregation function applied to a sample vector when reducing it to a
/// single value before a comparator runs. Used by both per-rule
/// comparators (the LHS / RHS aggregates of a `Rule`) and regression
/// rules (the `aggregate` field on a `RegressionRule`). Matches the six
/// values allowed by the JSON enum: sum / min / max / p01 / p50 / p99.
enum class Aggregate : std::uint8_t { Sum, Min, Max, P01, P50, P99 };

/// One row of the aggregate registry. `percentile_fraction` is meaningful
/// only for the three percentile entries; the other three carry NaN to
/// signal "not a percentile". Adding a new percentile just means adding a
/// row — `aggregate_samples` reads the fraction from here.
struct AggregateEntry {
    std::string_view    name;
    Aggregate           kind;
    double              percentile_fraction;     // NaN for non-percentile aggregates
};

/// Compile-time table linking the JSON spelling to the enumerator. Single
/// source of truth for parsers, validators, the `aggregate_samples`
/// dispatch, and the report writer; mirrors the
/// `kBuiltinMetricRegistry` pattern.
inline constexpr std::array<AggregateEntry, 6> kAggregateRegistry{ {
    { "sum", Aggregate::Sum, std::numeric_limits<double>::quiet_NaN() },
    { "min", Aggregate::Min, std::numeric_limits<double>::quiet_NaN() },
    { "max", Aggregate::Max, std::numeric_limits<double>::quiet_NaN() },
    { "p01", Aggregate::P01, 0.01 },
    { "p50", Aggregate::P50, 0.50 },
    { "p99", Aggregate::P99, 0.99 },
} };

[[nodiscard]] inline constexpr std::optional<Aggregate>
parse_aggregate(std::string_view text) noexcept {
    for (const auto& entry : kAggregateRegistry) {
        if (entry.name == text) return entry.kind;
    }
    return std::nullopt;
}

[[nodiscard]] inline constexpr std::string_view
aggregate_name(Aggregate aggregate) noexcept {
    for (const auto& entry : kAggregateRegistry) {
        if (entry.kind == aggregate) return entry.name;
    }
    return "?";
}

/// Returns the percentile fraction (0.01, 0.50, 0.99) for percentile
/// aggregates, or NaN for the additive Sum / Min / Max kinds. The NaN
/// sentinel keeps `aggregate_samples`' percentile branch table-driven.
[[nodiscard]] inline constexpr double
aggregate_percentile(Aggregate aggregate) noexcept {
    for (const auto& entry : kAggregateRegistry) {
        if (entry.kind == aggregate) return entry.percentile_fraction;
    }
    return std::numeric_limits<double>::quiet_NaN();
}

/// Every recognised metric flavour. The first batch are produced by the
/// framework from the iteration-local `ProfilerBlock<Args>` aggregates;
/// `Counter` and `Evaluation` route through the user's instrumentation
/// counters / evaluation lambda.
enum class MetricKind : std::uint8_t {
    WallClockNs,                 // outer clock around on_run
    RegionTotalNs,
    RegionVisits,
    RegionMinNs,
    RegionMaxNs,
    RegionP50Ns,
    RegionP99Ns,
    RegionTotalCycles,
    RegionTotalInstructions,
    RegionTotalBranches,
    RegionTotalBranchMisses,
    RegionIpc,
    RegionBranchMissRate,
    LoopTotalIterations,
    LoopTotalWork,
    LoopEmptyVisits,
    LoopMinPerIterNs,
    LoopMaxPerIterNs,
    LoopP50PerIterNs,
    LoopP99PerIterNs,
    LoopMeanPerIterNs,
    LoopWorkPerIter,
    Counter,
    Evaluation,
};

/// Source family (drives schema validation, parsing, and which descriptor
/// table to walk).
enum class MetricSource : std::uint8_t {
    OuterClock,    // WallClockNs only
    Region,        // walks RegionAggregate
    Loop,          // walks LoopAggregate
    Counter,       // walks resolved Counter handles
    Evaluation,    // walks the evaluation_metrics map
};

/// Cross-descriptor combiner (only meaningful for Region / Loop). Drives
/// the per-iteration "fold every visited descriptor's value into one
/// number" step in `append_iteration_samples`.
enum class MetricCombiner : std::uint8_t {
    Sum,                 // additive; e.g. visits, total_ns
    HistogramSum,        // sum the 32-bucket log2 histograms then run hist_percentile
    PmuRatio,            // (Σ instructions) / (Σ cycles); skip iteration when denom == 0
    BranchMissRatio,     // (Σ branch_misses) / (Σ branches); skip when denom == 0
    LoopMeanNs,          // (Σ total_ns) / (Σ total_iterations); skip when denom == 0
    LoopWorkPerIter,     // (Σ total_work) / (Σ total_iterations); skip when denom == 0
    RequiresRegion,      // min / max — must have explicit `region` filter; rejected at config load
};

/// Static registry of every framework-built-in metric. Schema validation,
/// metric parsing, semantic checks, and the iteration extraction step all
/// consume this single source of truth.
struct BuiltinMetricEntry {
    std::string_view name;
    MetricKind       kind;
    MetricSource     source;
    MetricCombiner   combiner;
    /// True iff the metric reads any PMU-derived field. Used by the
    /// `pmu_unavailable` runtime warning.
    bool             needs_pmu = false;
    /// p-fraction for HistogramSum-combined metrics; ignored otherwise.
    double           percentile = 0.0;
};

inline constexpr std::array<BuiltinMetricEntry, 22> kBuiltinMetricRegistry{ {
    { "wall_clock_ns",                MetricKind::WallClockNs,             MetricSource::OuterClock, MetricCombiner::Sum, false, 0.0 },
    { "region_total_ns",              MetricKind::RegionTotalNs,           MetricSource::Region,     MetricCombiner::Sum, false, 0.0 },
    { "region_visits",                MetricKind::RegionVisits,            MetricSource::Region,     MetricCombiner::Sum, false, 0.0 },
    { "region_min_ns",                MetricKind::RegionMinNs,             MetricSource::Region,     MetricCombiner::RequiresRegion, false, 0.0 },
    { "region_max_ns",                MetricKind::RegionMaxNs,             MetricSource::Region,     MetricCombiner::RequiresRegion, false, 0.0 },
    { "region_p50_ns",                MetricKind::RegionP50Ns,             MetricSource::Region,     MetricCombiner::HistogramSum, false, 0.50 },
    { "region_p99_ns",                MetricKind::RegionP99Ns,             MetricSource::Region,     MetricCombiner::HistogramSum, false, 0.99 },
    { "region_total_cycles",          MetricKind::RegionTotalCycles,       MetricSource::Region,     MetricCombiner::Sum, true,  0.0 },
    { "region_total_instructions",    MetricKind::RegionTotalInstructions, MetricSource::Region,     MetricCombiner::Sum, true,  0.0 },
    { "region_total_branches",        MetricKind::RegionTotalBranches,     MetricSource::Region,     MetricCombiner::Sum, true,  0.0 },
    { "region_total_branch_misses",   MetricKind::RegionTotalBranchMisses, MetricSource::Region,     MetricCombiner::Sum, true,  0.0 },
    { "region_ipc",                   MetricKind::RegionIpc,               MetricSource::Region,     MetricCombiner::PmuRatio, true, 0.0 },
    { "region_branch_miss_rate",      MetricKind::RegionBranchMissRate,    MetricSource::Region,     MetricCombiner::BranchMissRatio, true, 0.0 },
    { "loop_total_iterations",        MetricKind::LoopTotalIterations,     MetricSource::Loop,       MetricCombiner::Sum, false, 0.0 },
    { "loop_total_work",              MetricKind::LoopTotalWork,           MetricSource::Loop,       MetricCombiner::Sum, false, 0.0 },
    { "loop_empty_visits",            MetricKind::LoopEmptyVisits,         MetricSource::Loop,       MetricCombiner::Sum, false, 0.0 },
    { "loop_min_per_iter_ns",         MetricKind::LoopMinPerIterNs,        MetricSource::Loop,       MetricCombiner::RequiresRegion, false, 0.0 },
    { "loop_max_per_iter_ns",         MetricKind::LoopMaxPerIterNs,        MetricSource::Loop,       MetricCombiner::RequiresRegion, false, 0.0 },
    { "loop_p50_per_iter_ns",         MetricKind::LoopP50PerIterNs,        MetricSource::Loop,       MetricCombiner::HistogramSum, false, 0.50 },
    { "loop_p99_per_iter_ns",         MetricKind::LoopP99PerIterNs,        MetricSource::Loop,       MetricCombiner::HistogramSum, false, 0.99 },
    { "loop_mean_per_iter_ns",        MetricKind::LoopMeanPerIterNs,       MetricSource::Loop,       MetricCombiner::LoopMeanNs, false, 0.0 },
    { "loop_work_per_iter",           MetricKind::LoopWorkPerIter,         MetricSource::Loop,       MetricCombiner::LoopWorkPerIter, false, 0.0 },
} };

/// Look up a builtin metric by canonical name. Returns nullptr if absent.
[[nodiscard]] inline constexpr const BuiltinMetricEntry* lookup_builtin_metric(
    std::string_view metric_name) noexcept
{
    for (const auto& entry : kBuiltinMetricRegistry) {
        if (entry.name == metric_name) return &entry;
    }
    return nullptr;
}

[[nodiscard]] inline constexpr const char* metric_kind_name(MetricKind metric_kind) noexcept {
    switch (metric_kind) {
        case MetricKind::Counter:    return "counter";
        case MetricKind::Evaluation: return "evaluation";
        default: break;
    }
    for (const auto& entry : kBuiltinMetricRegistry) {
        if (entry.kind == metric_kind) return entry.name.data();
    }
    return "?";
}

/// Parse the JSON `"type"` string into a source family. Returns
/// `std::nullopt` for unrecognised strings; the caller distinguishes
/// "counter" / "evaluation" from registry-driven sources by inspecting
/// the source enumerator.
[[nodiscard]] inline std::optional<MetricSource> parse_metric_type(std::string_view type_text) noexcept {
    if (type_text == "builtin")    return MetricSource::OuterClock;
    if (type_text == "region")     return MetricSource::Region;
    if (type_text == "loop")       return MetricSource::Loop;
    if (type_text == "counter")    return MetricSource::Counter;
    if (type_text == "evaluation") return MetricSource::Evaluation;
    return std::nullopt;
}

/// Inverse of `parse_metric_type`. Renders a `MetricSource` as its
/// JSON-side spelling (`"region"`, `"loop"`, `"builtin"`, `"counter"`,
/// `"evaluation"`). Used by diagnostic messages — the strings round-trip
/// with `parse_metric_type`.
[[nodiscard]] inline constexpr std::string_view metric_source_name(MetricSource source) noexcept {
    switch (source) {
        case MetricSource::OuterClock: return "builtin";
        case MetricSource::Region:     return "region";
        case MetricSource::Loop:       return "loop";
        case MetricSource::Counter:    return "counter";
        case MetricSource::Evaluation: return "evaluation";
    }
    return "?";
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
    /// Reduction applied to each side's sample vector before the
    /// comparator runs. Defaults to `P50` (the framework's percentile
    /// median) for backward-compatibility with pre-V5 rules that
    /// hardcoded median behaviour.
    Aggregate aggregate = Aggregate::P50;
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
    /// `"type"` string the user declared (or the implicit "builtin"
    /// default). Stored separately from `kind` so semantic validation can
    /// detect cross-source typos like `"type": "loop"` with key
    /// `region_total_ns` — the registry resolves the kind from the name
    /// regardless of `type`, but a mismatch is still a config error.
    MetricSource declared_source = MetricSource::OuterClock;
    /// For `kind == Counter`: canonical name of the instrumentation counter
    /// to snapshot. Defaults to `name` when the config omits the explicit
    /// `"counter"` field.
    std::string  counter_name{};
    /// For `MetricSource::Region` / `MetricSource::Loop` metrics: when
    /// non-empty, restrict the metric to descriptors whose name equals
    /// this string. When empty, every visited descriptor contributes via
    /// the registry's combiner (Sum, HistogramSum, PmuRatio, etc.).
    /// Mandatory for metrics whose combiner is `RequiresRegion`
    /// (region_min_ns, region_max_ns, loop_min_per_iter_ns,
    /// loop_max_per_iter_ns) — semantic_validate_config rejects those if
    /// `region` is empty.
    std::string  region{};
    std::vector<Rule> rules{};
};

struct ConcurrencyOpts {
    /// If true, this benchmark runs alone (every item-iteration loop
    /// acquires the cross-bench unique lock; siblings queue behind it).
    bool bench_lock  = false;
    /// If true (default), batches of this benchmark fan out across TBB
    /// workers. False forces every item to take the per-benchmark
    /// serial mutex, so all iterations of all items run on whichever
    /// worker happens to pick them up next, but never two at once.
    bool concurrency = true;

    // V11 NOTE: `test_case_lock` (V10 default true) is deleted. The new
    // batched scheduler runs at most one tuple at a time per
    // (test_case, group), so the contention this flag was guarding
    // against is impossible by construction.
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

/// Default per-test-case concurrency: max(1, hw_concurrency-1). Cached
/// once per process so `parse_test_cases` does not re-query the OS for
/// every entry.
[[nodiscard]] inline std::size_t default_test_case_concurrency() noexcept {
    static const std::size_t cached = [] {
        const auto hw = std::thread::hardware_concurrency();
        return std::max<std::size_t>(1u, hw <= 1u ? 1u : hw - 1u);
    }();
    return cached;
}

struct TestCaseSpec {
    std::string name;
    std::string description;
    /// Maximum number of parallel groups this test case may be split
    /// across. The framework caps the actual group count at the total
    /// iteration count across all tuples sharing this test_case (a
    /// group with zero iterations is meaningless, so we never create
    /// one). Read from `test_cases.<name>.concurrency`; defaults to
    /// `default_test_case_concurrency()` when the key is omitted.
    std::size_t concurrency = 0;
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

/// One (test_case × group × execution_tuple) cell holding iteration
/// metrics for a single group's slice of iterations.
///
/// `Cell` is owned via `std::unique_ptr<Cell>` (see `BenchmarkExecutionState`)
/// because the framework hands out stable `Cell*` to TBB workers and the
/// pointer must outlive any vector reallocation. Per main/DOCS/GUIDELINES/CODE_STYLE__DOC0145.md §5,
/// smart pointers exist here because the per-benchmark state owns every
/// cell for the lifetime of the run; the alternative would be a
/// stable-iterator container like `std::list` plus manual lookup, which
/// is strictly worse.
///
/// Groups are an implementation detail driven by
/// `test_cases.<name>.concurrency` and the per-group iteration split
/// (see `split_iterations`). The user-visible report contains one
/// entry per `(test_case, tuple)` with samples concatenated across
/// every group of the same `(test_case, tuple)` pair — see
/// `merge_group_samples` in the writer / rule evaluator.
///
/// Thread-safety: V11 schedules at most one batch per `(tc, group)`, and
/// items inside a batch run sequentially. A `Cell` is therefore never
/// touched by two threads at once, so `samples` does not need any
/// mutex around its writes. (Pre-V11 needed `samples_mu` because the
/// flat work-unit list let two threads race the same cell across
/// concurrent iterations of a single tuple — V11 deletes that path.)
struct Cell {
    const TestCaseSpec*  test_case = nullptr;
    const ExecutionTuple* tuple = nullptr;
    /// Iterations *for this group* (= `split_iterations(total, group_count)[group_index]`).
    std::size_t          iterations = 0;
    /// Stable index into the test_cases vector. Lets the per-batch
    /// input cache key inputs by index without pointer arithmetic.
    std::size_t          test_case_index = 0;
    /// Index of this group within its test_case's split. Always < group_count.
    std::size_t          group_index = 0;
    /// Total number of groups for this test_case (the group_index
    /// upper-bound + 1). Cached on every cell so the writer does not
    /// have to re-derive it from the global state.
    std::size_t          group_count = 1;

    /// region_name -> metric_name -> sample values. "all_regions" is a
    /// synthetic bucket holding the iteration-level wall_clock_ns and every
    /// counter / evaluation metric. Per-region buckets only appear when
    /// `BenchmarkConfig::by_regions == true`.
    std::map<std::string, std::map<std::string, std::vector<double>>> samples{};

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

    // V16: `serial_mu` deleted — the scheduler enforces
    // `concurrency.concurrency: false` by admitting at most one batch
    // per benchmark at a time (see BatchScheduler::concurrency_admissible_locked).

    BenchmarkExecutionState() = default;
    BenchmarkExecutionState(BenchmarkExecutionState&&) = delete;
    BenchmarkExecutionState(const BenchmarkExecutionState&) = delete;
    BenchmarkExecutionState& operator=(BenchmarkExecutionState&&) = delete;
    BenchmarkExecutionState& operator=(const BenchmarkExecutionState&) = delete;
};

struct ParsedCli {
    bool                       fast_filter = false;
    /// V14: `--ui` opens a long-running interactive ftxui dashboard.
    /// Replaces the V8 `--summary` one-shot. Mutually exclusive with
    /// `--fast` / `--threads` (warn-and-ignore on collision).
    bool                       ui_mode = false;
    /// V14: `--ui-ascii` substitutes ASCII fallbacks for emoji glyphs.
    /// Useful when the terminal locale is `C` or DejaVu / JetBrains
    /// Mono is unavailable.
    bool                       ui_ascii_fallback = false;
    /// V18: `--ui-test-mode` disables the 1-Hz reload-countdown
    /// animation and the periodic 30-s reload tick, and emits a
    /// `\x1b]666;render-N\x07` OSC sentinel after every render so a
    /// PTY-driven Python harness can synchronise on completed frames
    /// (main/DOCS/BENCHMARK_FRAMEWORK/BENCH_V18_IMPL_DESIGN__DOC0245.md §10.4.1). Manual `Ctrl-R` and the
    /// V14 selection-debounce-reparse path remain functional.
    bool                       ui_test_mode = false;
    std::optional<unsigned>    threads_override{};
    std::optional<std::string> report_override{};
    /// V12: `--memory-buffer N` (0..50, default 40). Determines how
    /// much headroom to leave above peak system usage when computing
    /// the dynamic memory cap. See `MemoryBudget::cap_for`.
    double                     memory_buffer_pct = 40.0;
};

/// Identity card for one report. Captured once in `run_impl` and
/// emitted as the first key of the output JSON. `commit` is the SHA of
/// the working-tree HEAD (or empty if not a git repo); `dirty` is
/// `true` whenever `git status --porcelain` reports any uncommitted
/// changes — distinguishing a pristine SHA from a dirty-tree run is
/// essential for `regression_since_commit` cross-reference. `created`
/// is an ISO 8601 UTC string built from `gmtime_r` + `strftime`;
/// `timestamp` is unix seconds for machine consumers.
struct ReportMeta {
    std::string  commit{};
    bool         dirty = false;
    std::int64_t timestamp = 0;
    std::string  created{};
    bool         fast = false;
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

/// Comparator payload for a regression rule. `threshold%` is forbidden by
/// the schema, so we only carry `tolerance%` — the underlying compare op
/// reuses the existing `CompareOp` family.
struct RegressionOperator {
    CompareOp op = CompareOp::Equal;
    double    tolerance_pct = 0.0;
};

/// Parsed `regression_rules[*]` entry. `regions` is normalised at parse
/// time so the evaluator never has to special-case the
/// `"all_regions"` shorthand: the literal string lands in the vector as
/// the single element.
struct RegressionRule {
    std::string config_path;
    std::string name;
    /// Empty means "applies to every benchmark in the same config file".
    std::vector<std::string> benchmarks{};
    /// Always non-empty after parse. Defaults to `{"all_regions"}`.
    std::vector<std::string> regions{};
    std::string metric;
    Aggregate aggregate = Aggregate::Sum;
    RegressionOperator  op{};
    /// Test cases excluded from this rule. Schema requires non-empty if
    /// the array is present at all.
    std::vector<std::string> exclude_cases{};
};

/// One detected regression. Mirrors the user-visible JSON shape: arrays of
/// size 1 for `violating_tuples` / `test_cases` so the writer doesn't need
/// to special-case singletons. `regression_since` is empty-string when
/// the rule wants `null` in the output.
struct RegressionEntry {
    std::string config_path;
    std::string benchmark;
    std::string rule_name;
    std::string algorithm_name;
    ParamMap    violating_params{};
    std::string test_case;
    std::string region;
    std::string metric;
    Aggregate aggregate = Aggregate::Sum;
    double      previously = 0.0;
    double      now = 0.0;
    /// Empty string ⇒ JSON `null`. Otherwise the file basename like
    /// `"report_3.json"`.
    std::string regression_since{};
    /// Commit SHA captured by the historical report identified by
    /// `regression_since` (its `meta.commit` field). Empty string ⇒
    /// JSON `null` — happens when `regression_since` itself is null,
    /// when the historical report was written before V5 (no `meta`
    /// block), or when `meta.commit` was empty in that report (the
    /// older run wasn't in a git repo).
    std::string regression_since_commit{};
};

/// Registry mapping `config_path` → list of regression rules parsed from
/// that file. Populated lazily by the per-benchmark closure under
/// `regression_rules_mu`; consumed by the global validator and the
/// regression evaluator after the parallel benchmark loop has joined.
using RegressionRulesByConfig = std::map<std::string, std::vector<RegressionRule>>;

/// One sanity-warning finding emitted at end-of-run. Sanity checks
/// catch suspicious uniformity that no other validation pass could
/// have flagged (e.g. an `add_counter(0)` pattern, an evaluator that
/// always returns the same hardcoded value, or a frozen clock). They
/// are hardcoded, cannot be disabled, and never affect the exit code,
/// but are surfaced both in `report.json` (under `sanity_warnings`)
/// and on stderr at the very end of the run.
///
/// Currently the only emitted `type` is `"metric_looks_constant"`;
/// the field is a free-form string so additional checks can land
/// without touching consumers' parsers.
struct SanityWarning {
    /// Stable identifier for the check kind. `"metric_looks_constant"`
    /// is the only value emitted in V6.
    std::string type;
    /// Metric name whose data tripped the check.
    std::string metric;
    /// Human-readable explanation including, where applicable, the
    /// stringified offending value (e.g. "always produces '0'").
    std::string message;
    /// Basename of the oldest report on disk that already exhibited
    /// this warning when joined with the current run's data, e.g.
    /// `"report_3.json"`. Empty string ⇒ JSON `null` (history walk
    /// hit a missing report).
    std::string warning_since;
    /// `meta.commit` of the historical report identified by
    /// `warning_since`. Empty string ⇒ JSON `null` (warning_since
    /// itself null, or the older report had no `meta` block, or its
    /// `meta.commit` was empty).
    std::string warning_since_commit;
};

// Forward declarations used by BenchmarkContext.
struct ProgressState;
struct ValidationDiag;
class  MemoryBudget;
class  BatchScheduler;

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
    // V16: `cross_bench_mu` deleted — bench-lock semantics enforced by
    // the scheduler's admission predicates (see
    // BatchScheduler::bench_lock_admissible_locked).
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
    /// Aggregated validation diagnostics across all benchmarks. Each
    /// benchmark's closure appends its schema + semantic findings under
    /// `diagnostics_mu`; `run_impl` prints them and uses the error count
    /// to drive the exit code.
    std::mutex*                                     diagnostics_mu = nullptr;
    std::vector<detail::ValidationDiag>*            diagnostics = nullptr;
    /// Shared registry of regression rules keyed by config_path. The
    /// closure inserts under `regression_rules_mu` the first time it sees
    /// each unique config; subsequent benchmarks sharing the same config
    /// skip parsing.
    std::mutex*                                     regression_rules_mu = nullptr;
    detail::RegressionRulesByConfig*                regression_rules_by_config = nullptr;
    /// V16: process-wide batch scheduler. The per-benchmark closure
    /// enqueues batches into this scheduler and waits for them to
    /// drain before returning, so captured state (`Cell::samples`,
    /// `BenchmarkExecutionState`, etc.) outlives the bodies. The
    /// scheduler also owns a non-owning reference to the host's
    /// `MemoryBudget` — callers that need budget metadata go through
    /// the scheduler instead of duplicating the pointer here.
    detail::BatchScheduler*                         scheduler = nullptr;
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
                out.integer_value = parsed;
            }
            return out;
        }
        case element_type::UINT64: {
            std::uint64_t parsed = 0;
            if (element.get(parsed) == simdjson::SUCCESS) {
                out.kind  = Kind::Integer;
                out.integer_value = static_cast<std::int64_t>(parsed);
            }
            return out;
        }
        case element_type::DOUBLE: {
            double parsed = 0.0;
            if (element.get(parsed) == simdjson::SUCCESS) {
                out.kind  = Kind::Double;
                out.double_value = parsed;
            }
            return out;
        }
        case element_type::BOOL: {
            bool parsed = false;
            if (element.get(parsed) == simdjson::SUCCESS) {
                out.kind   = Kind::Boolean;
                out.boolean_value = parsed;
            }
            return out;
        }
        case element_type::STRING: {
            std::string_view parsed{};
            if (element.get(parsed) == simdjson::SUCCESS) {
                out.kind  = Kind::String;
                out.string_value = std::string{parsed};
            }
            return out;
        }
        default:
            // Object / array / null: not a scalar configuration value.
            // Mark with the NonScalar sentinel so semantic validation can
            // refuse it against any declared parameter type. We still keep
            // the JSON text in `string_value` for diagnostic messages.
            out.kind  = Kind::NonScalar;
            out.string_value = simdjson::to_string(element);
            return out;
    }
}

namespace detail {

// ----------------- V12 framework constants -----------------

/// Hardcoded ceiling on the number of consecutive failed attempts at
/// `on_input` (G2) and `on_cleanup` (G3) before the framework declares
/// the batch unrecoverable. Set to 5 per spec; not exposed via CLI.
inline constexpr std::size_t kMaxTaskInputHandlerErrorCount = 5;

/// Maximum number of stack-trace frames to render in the dashboard
/// per failure. Anything past this is dropped with an "elided" marker.
inline constexpr std::size_t kMaxStackTraceLines = 30;

/// Minimum time the retry-failure row stays in the dashboard before
/// the next attempt is taken. Without this, a fast-failing user
/// lambda (e.g. a `throw` immediately after entry) burns through all
/// 5 retries in microseconds — the dashboard repaints at ~5 Hz, so
/// `retry_attempt > 0` is never observed and the operator sees no UI
/// for the retry sequence at all. 300 ms is slightly longer than the
/// 200 ms render cadence so every attempt's failure state is
/// guaranteed to show up in at least one frame.
inline constexpr std::chrono::milliseconds kRetryFailureMinDisplayTime{300};

// ----------------- hwinfo bridge -----------------

/// Best-effort `(total, free)` bytes reading the host's physical RAM.
/// Computed via the cross-platform `hwinfo::Memory` class. The exact
/// semantics of `Memory::size()` / `Memory::free()` / `Memory::available()`
/// vary by platform (see hwinfo-main/src/{apple,linux,windows}/ram.cpp);
/// this helper picks the most-useful values per platform via
/// compile-time `#ifdef HWINFO_*` from `<hwinfo/platform.h>`. On
/// totally-broken hwinfo we fall back to "infinite memory" so the
/// framework over-trusts the host instead of refusing to run.
struct SystemMemorySnapshot {
    std::uint64_t total_bytes = 0;
    std::uint64_t free_bytes  = 0;
};

[[nodiscard]] inline SystemMemorySnapshot snapshot_system_memory() noexcept {
    hwinfo::Memory mem{};
    SystemMemorySnapshot snap{};

    // The vcpkg-installed hwinfo (lfreist-hwinfo, vcpkg edition) exposes
    // total_Bytes() / free_Bytes() / available_Bytes() as `int64_t`.
    // Negative values are sentinel "I don't know"; treat as zero.
    auto clamp_positive = [] (std::int64_t v) noexcept -> std::uint64_t {
        return v <= 0 ? std::uint64_t{0} : static_cast<std::uint64_t>(v);
    };

    // DOC0276 — PRIMARY source is the direct mach/POSIX probe in
    // `bioseq::instr::detail::sysinfo` (the same probe the production extension's
    // MemoryBudget uses). It exists precisely because the vcpkg hwinfo
    // `free_Bytes()` routes through `host_statistics64`, which returns -1
    // (failure) on the vcpkg ARM-macOS build — so the old code's "free" read 0
    // and fell back to ~total, i.e. the framework's memory estimates were always
    // (effectively) zero-pressure. `system_available_bytes()` calls
    // host_statistics64(HOST_VM_INFO64) directly (which DOES work) and reports
    // (free + inactive + speculative + purgeable) pages. hwinfo stays only as a
    // last-resort fallback when the direct probe is unavailable.
    namespace sysinfo = bioseq::instr::detail::sysinfo;
    snap.total_bytes = sysinfo::system_total_bytes();
    if (snap.total_bytes == 0) {
        snap.total_bytes = clamp_positive(mem.total_Bytes());
        if (snap.total_bytes == 0) snap.total_bytes = clamp_positive(mem.available_Bytes());
    }

    snap.free_bytes = sysinfo::system_available_bytes();
    if (snap.free_bytes == 0) {
        // Direct probe unavailable — fall back to hwinfo per platform.
#if defined(HWINFO_UNIX) && !defined(HWINFO_APPLE)
        snap.free_bytes = clamp_positive(mem.available_Bytes());
#elif defined(HWINFO_APPLE)
        snap.free_bytes = clamp_positive(mem.free_Bytes());
        if (snap.free_bytes == 0) snap.free_bytes = clamp_positive(mem.available_Bytes());
#else
        snap.free_bytes = clamp_positive(mem.free_Bytes());
#endif
    }

    // Defensive clamps: never let `free` exceed `total`. If both are
    // zero (broken hwinfo), pretend infinite total so the framework
    // can still run; the operator gets a startup log line so the
    // anomaly is visible.
    if (snap.total_bytes == 0) {
        snap.total_bytes = std::numeric_limits<std::uint64_t>::max();
    }
    if (snap.free_bytes > snap.total_bytes) {
        snap.free_bytes = snap.total_bytes;
    }
    return snap;
}

// ----------------- MemoryBudget -----------------

// Forward declaration kept so the renderer can store a pointer.
struct ProgressGroup;

/// V16 slim form: hwinfo snapshotter plus the buffered-cap math. All
/// reservation tracking moved to `BatchScheduler` (§3.7 of
/// main/DOCS/BENCHMARK_FRAMEWORK/BENCH_V16_IMPL_PLAN__DOC0233.md). Removing the per-batch acquire/release
/// path eliminates the worker-blocking cv-wait loop that was the
/// secondary bottleneck under the V12 design.
class MemoryBudget {
public:
    /// hwinfo TTL — every snapshot reads /proc/meminfo / sysctl / WMI.
    /// Cache the last one so concurrent renderer paints + scheduler
    /// ticks collapse to one syscall per 200 ms window.
    static constexpr std::chrono::milliseconds kBudgetSnapshotTtl{200};

    /// Constructed once in `run_impl`. `memory_buffer_pct` is already
    /// validated to `[0, 50]` by `parse_cli`.
    explicit MemoryBudget(double memory_buffer_pct) noexcept
        : memory_buffer_pct_(memory_buffer_pct)
    {
        DEBUG_ASSERT(memory_buffer_pct >= 0.0 && memory_buffer_pct <= 50.0,
            "MemoryBudget memory_buffer_pct must be in [0, 50]");
    }

    /// Compute the cap (`C = T·(1 − buffer/100)`) for an arbitrary `T`.
    [[nodiscard]] static constexpr std::uint64_t
    cap_for(std::uint64_t total, double buffer_pct) noexcept {
        return static_cast<std::uint64_t>(
            static_cast<double>(total) * (100.0 - buffer_pct) / 100.0);
    }

    [[nodiscard]] double memory_buffer_pct() const noexcept { return memory_buffer_pct_; }

    /// Live system-memory snapshot. Reads through a 200 ms cache so
    /// many concurrent callers (scheduler + renderer) collapse to one
    /// syscall per cache window. `const` because the cache fields are
    /// `mutable`; observable state of the budget is unchanged.
    [[nodiscard]] SystemMemorySnapshot current_system_snapshot() const noexcept {
        std::lock_guard<std::mutex> guard(cache_mu_);
        const auto now_ns = static_cast<std::int64_t>(bioseq::instr::clock::now_ns());
        constexpr auto ttl_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    kBudgetSnapshotTtl).count();
        if (cached_snapshot_ns_ == 0 || (now_ns - cached_snapshot_ns_) >= ttl_ns) {
            cached_snapshot_    = snapshot_system_memory();
            cached_snapshot_ns_ = now_ns;
        }
        return cached_snapshot_;
    }

    MemoryBudget(const MemoryBudget&) = delete;
    MemoryBudget(MemoryBudget&&) = delete;
    MemoryBudget& operator=(const MemoryBudget&) = delete;
    MemoryBudget& operator=(MemoryBudget&&) = delete;

private:
    mutable std::mutex             cache_mu_{};
    mutable SystemMemorySnapshot   cached_snapshot_{};
    mutable std::int64_t           cached_snapshot_ns_ = 0;
    double                         memory_buffer_pct_;
};

// ----------------- ProgressState -----------------

/// One group of one test_case. Hot-path counter is `done`; the
/// scheduler updates `label` only at item boundaries (cold path).
///
/// Failure / wait state observed by the renderer:
///   - `input_retry_attempt`  — current `on_input` retry index (0 = clear).
///   - `cleanup_retry_attempt`— current `on_cleanup` retry index (0 = clear).
///   - `idle_start_ns`        — V16: written by the scheduler via
///                               `mirror_idle_to_ui` while a batch is
///                               pending and starves. The renderer
///                               shows a "🕑 waiting Ns / 30s" row
///                               when this batch is the sole holdout.
///   - `aborted`              — true after a hard error has aborted the
///                               batch; renderer renders an `❌` row
///                               with the diagnostic message.
/// All four are atomic so the renderer thread can read them while the
/// batch worker thread mutates them. The four failure-string fields
/// (`input_failure_message` / `_trace`, `cleanup_failure_message` /
/// `_trace`) are guarded by a separate `failure_mu` rather than the
/// hot-path `label_mu` — the latter sits inside the iteration loop and
/// must not be widened.
struct ProgressGroup {
    std::size_t                group_index = 0;
    std::atomic<std::uint64_t> done{0};
    std::uint64_t              total = 0;
    /// Scheduler sets this true when the batch starts, false when the
    /// batch's iteration loop exits. Renderer uses it to decide whether
    /// to draw the row (collapsed test cases hide all groups).
    std::atomic<bool>          active{false};
    /// Brief label `algo=<>, bench=<>` updated at item boundaries.
    /// Held under `label_mu` (one short string copy) — never blocks
    /// the iteration loop.
    std::string                label{};
    std::mutex                 label_mu{};

    /// Input retry status. 0 = clear; 1..kMaxTaskInputHandlerErrorCount
    /// = current attempt (1-based).
    std::atomic<std::uint8_t>  input_retry_attempt{0};
    /// Cleanup retry status. Same shape as input_retry_attempt.
    std::atomic<std::uint8_t>  cleanup_retry_attempt{0};
    /// Nanoseconds since `bioseq::instr::clock::now_ns()` epoch at
    /// which the batch entered the idle-wait state. Written by the
    /// scheduler via `mirror_idle_to_ui`. 0 = not waiting on memory.
    std::atomic<std::int64_t>  idle_start_ns{0};
    /// Set true when a hard error aborts the batch.
    std::atomic<bool>          aborted{false};

    /// Failure-message + clipped stack trace surfaces. The renderer
    /// only consults these when the corresponding atomic counter
    /// (`input_retry_attempt` / `cleanup_retry_attempt`) is non-zero,
    /// so the lock is taken at most once per failed attempt + once
    /// per render at most. Keeping it separate from `label_mu`
    /// prevents blocking the iteration hot path on a slow reader.
    std::mutex                 failure_mu{};
    std::string                input_failure_message{};
    std::string                input_failure_trace{};
    std::string                cleanup_failure_message{};
    std::string                cleanup_failure_trace{};
    /// Diagnostic blurb when `aborted == true` (e.g. "memory budget
    /// constraint timeout"). Read under `failure_mu`.
    std::string                aborted_reason{};
};

/// One test_case row in the dashboard. Aggregates progress across all
/// of its groups. `start_ns` / `finish_ns` measure wall-clock duration
/// from "first group started" to "last group done". `tuple_total` is
/// atomic so the renderer thread (started in `run_impl` before the
/// per-benchmark closures populate this struct) sees a consistent
/// value on weakly-ordered hardware.
struct ProgressTestCase {
    std::string                                test_case_name{};
    std::atomic<std::uint64_t>                 tuple_done{0};       // cumulative items finished
    std::atomic<std::uint64_t>                 tuple_total{0};
    std::vector<std::unique_ptr<ProgressGroup>> groups{};
    std::atomic<std::int64_t>                  start_ns{0};
    std::atomic<std::int64_t>                  finish_ns{0};
};

/// Full progress model. The renderer reads everything; the scheduler
/// writes via three helpers below.
///
/// Per-benchmark `test_cases` is populated by the closure that drives a
/// single `ErasedBenchmark` (running on some TBB worker) AFTER the
/// renderer thread has already started polling. To avoid a data race
/// on the vector's structural state, the writer signals completion via
/// `setup_done.store(true, release)` and readers gate every traversal
/// on `setup_done.load(acquire)`. After the writer signals, the
/// vector and every `unique_ptr` it holds are stable for the rest of
/// the run.
// Forward-declared so ProgressState can carry a non-owning pointer to it
// even though the class body lives further down in the module.
class BatchScheduler;

struct ProgressState {
    struct Per {
        std::string                                       title{};
        std::string                                       benchmark_key{};
        std::vector<std::unique_ptr<ProgressTestCase>>    test_cases{};
        std::atomic<bool>                                 setup_done{false};
    };
    std::vector<std::unique_ptr<Per>> per_benchmark;
    std::atomic<bool> running{true};
    std::atomic<bool> tty_render{false};   // set true if stderr is a TTY at startup
    /// V16: scheduler exposes pending/running counters + reservation
    /// totals consumed by the dashboard header. nullptr = no scheduler
    /// installed (e.g. in `--ui` review mode). Memory-budget access is
    /// routed through the scheduler too, so there's a single owner.
    BatchScheduler*  scheduler     = nullptr;
};

// ----------------- BatchScheduler -----------------

/// One pending or running batch, type-erased. Owned by the scheduler.
/// The body is constructed by `run_typed_benchmark` and captures
/// references back into the per-benchmark `BenchmarkExecutionState` —
/// so the scheduler must guarantee the body has run to completion
/// before the closure that built it returns. `wait_for_benchmark`
/// enforces that contract.
struct ScheduledBatch {
    std::size_t      scheduler_id      = 0;
    std::size_t      benchmark_index   = 0;
    /// `concurrency.bench_lock` from the parsed config — true ⇒
    /// scheduler treats the owning benchmark as exclusive across
    /// benchmarks (other benchmarks' batches do not run until every
    /// bench-locked batch from this benchmark has drained).
    bool             bench_lock        = false;
    /// `!concurrency.concurrency` — true ⇒ scheduler admits at most
    /// one batch from this benchmark at a time (preserves V12's
    /// "Strict latency-sensitive run" semantics).
    bool             concurrency_serial = false;
    /// Memory reservation requested by `on_estimate_resources`.
    std::size_t      base_bytes        = 0;
    std::size_t      extra_bytes       = 0;
    /// Body executed by the TBB task pool. Wrapped at admission time
    /// by an enclosing lambda that owns a `ScopedSchedulerFinish` so
    /// the reservation always releases on every return path.
    std::function<void()> body{};
    /// UI mirror — the renderer reads `idle_start_ns` to colour the
    /// "🕑 waiting Ns / 30s" row when this batch is the sole holdout.
    /// May be nullptr (no progress dashboard installed).
    ProgressGroup*   group_for_ui      = nullptr;
    /// Per-batch idle clock (ns). 0 = "not idle yet". Mutated only by
    /// the scheduler thread under `mu_`.
    std::int64_t     last_idle_start_ns = 0;
    /// Captured at enqueue so `emit_idle_timeout_locked` can mark the
    /// progress UI without re-resolving every chain of indices.
    std::string      location_str{};
    std::string      test_case_name{};
    std::size_t      test_case_index   = 0;
    std::size_t      group_index       = 0;
    /// Non-owning pointer to the BenchmarkContext used to push
    /// diagnostics + UI updates. Lifetime is the same as `run_impl`.
    BenchmarkContext* context           = nullptr;
};

/// Free helper used inside the scheduler when mirroring the idle clock
/// to the per-group UI atomic. Replaces V12's
/// `MemoryBudget::mirror_idle_to_ui`.
inline void mirror_idle_to_ui(const ScheduledBatch& batch,
                               std::int64_t idle_start_ns) noexcept {
    if (batch.group_for_ui != nullptr) {
        batch.group_for_ui->idle_start_ns.store(
            idle_start_ns, std::memory_order_release);
    }
}

/// Forward declaration so `ScopedSchedulerFinish` can refer to the
/// scheduler before its body is defined.
class BatchScheduler;

/// RAII guard installed by the scheduler around every enqueued body.
/// `~ScopedSchedulerFinish` calls `BatchScheduler::notify_batch_finished`
/// so any return path (normal, diagnostic-emit, exception inside
/// `cpptrace::try_catch` catchers) decrements reservations exactly once.
struct ScopedSchedulerFinish {
    BatchScheduler* scheduler;
    std::size_t     scheduler_id;
    ~ScopedSchedulerFinish() noexcept;
};

/// Single scheduler thread + TBB task pool. Replaces the V12
/// `tbb::parallel_for_each(batches…)` model.
///
/// Memory reservations live exclusively here — bodies never block on
/// `MemoryBudget` directly. Bench-lock + serial-concurrency rules are
/// also enforced by admission predicates instead of process-global
/// mutexes; see main/DOCS/BENCHMARK_FRAMEWORK/BENCH_V16_IMPL_PLAN__DOC0233.md §3.4.
class BatchScheduler {
public:
    /// Scheduler tick cadence. Wakeups also fire on every batch
    /// completion, so the wall-clock latency between "a slot opens"
    /// and "the next batch is admitted" is dominated by
    /// `notify_batch_finished` rather than the tick (the tick exists
    /// only to re-poll hwinfo and re-check idle timeouts).
    static constexpr std::chrono::seconds kSchedulerTickInterval{1};
    /// `MAX_TASK_IDLE_WAITING_TIME` — preserved verbatim from V12.
    /// A pending batch whose constraints don't fit AND whose peers
    /// are all idle for this long aborts with
    /// `memory_budget_constraint_timeout`.
    static constexpr std::chrono::seconds kMaxIdleWaitingTime{30};

    BatchScheduler(MemoryBudget& budget,
                    oneapi::tbb::task_arena& arena) noexcept
        : budget_(budget), arena_(arena)
    {
        thread_ = std::thread{[this] { scheduler_loop(); }};
    }

    ~BatchScheduler() noexcept {
        // Dtor sequence (see main/DOCS/BENCHMARK_FRAMEWORK/BENCH_V16_IMPL_PLAN__DOC0233.md §4.5):
        // 1. Stop the scheduler thread.
        // 2. Drain any TBB tasks already enqueued.
        // 3. Members destruct.
        running_.store(false, std::memory_order_release);
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
        tg_.wait();      // tg_'s dtor would throw missing_wait otherwise
    }

    BatchScheduler(const BatchScheduler&) = delete;
    BatchScheduler(BatchScheduler&&) = delete;
    BatchScheduler& operator=(const BatchScheduler&) = delete;
    BatchScheduler& operator=(BatchScheduler&&) = delete;

    /// Add a batch to the pending queue. Thread-safe — called from
    /// every per-benchmark closure during the enqueue phase.
    void enqueue_batch(ScheduledBatch batch) noexcept {
        {
            std::lock_guard<std::mutex> guard(mu_);
            batch.scheduler_id = next_id_++;
            ++total_enqueued_;
            ++outstanding_per_benchmark_[batch.benchmark_index];
            pending_.push_back(std::move(batch));
        }
        cv_.notify_all();
    }

    /// Block until every batch belonging to `benchmark_index` has
    /// either completed or been aborted. Called by the per-benchmark
    /// closure before it returns, so the body's captured references
    /// stay valid.
    void wait_for_benchmark(std::size_t benchmark_index) noexcept {
        std::unique_lock<std::mutex> guard(mu_);
        cv_.wait(guard, [this, benchmark_index] {
            const auto it = outstanding_per_benchmark_.find(benchmark_index);
            return it == outstanding_per_benchmark_.end() || it->second == 0;
        });
    }

    /// Block until every enqueued batch has either completed or been
    /// aborted. Called once by `run_impl` after the outer benchmark
    /// loop joins (which, given `wait_for_benchmark` is also called
    /// per-closure, should already be a no-op — kept for defence in
    /// depth).
    void wait_for_completion() noexcept {
        std::unique_lock<std::mutex> guard(mu_);
        cv_.wait(guard, [this] {
            return total_finished_ >= total_enqueued_;
        });
    }

    /// Called from the body's `ScopedSchedulerFinish` destructor.
    void notify_batch_finished(std::size_t scheduler_id) noexcept {
        // Splice the finished node into a local list so its destructor
        // (which destroys `body`'s captures — potentially a chain of
        // std::function copies) runs OUTSIDE `mu_`. Keeps lock-hold
        // duration bounded by counter updates only.
        std::list<ScheduledBatch> to_drop;
        {
            std::lock_guard<std::mutex> guard(mu_);
            const auto it = std::find_if(running_batches_.begin(),
                                          running_batches_.end(),
                [scheduler_id](const ScheduledBatch& candidate) noexcept {
                    return candidate.scheduler_id == scheduler_id;
                });
            DEBUG_ASSERT(it != running_batches_.end(),
                "BatchScheduler::notify_batch_finished: id not found");
            const auto need = static_cast<std::int64_t>(
                                   it->base_bytes + it->extra_bytes);
            reserved_bytes_ -= need;
            DEBUG_ASSERT(reserved_bytes_ >= 0,
                "BatchScheduler reserved_bytes_ underflow");
            decrement_or_erase_locked(active_bench_lock_holder_,
                                       it->benchmark_index, it->bench_lock);
            decrement_or_erase_locked(running_per_benchmark_,
                                       it->benchmark_index, /*present=*/true);
            decrement_or_erase_locked(outstanding_per_benchmark_,
                                       it->benchmark_index, /*present=*/true);
            to_drop.splice(to_drop.end(), running_batches_, it);
            ++total_finished_;
        }
        cv_.notify_all();
        // ~to_drop runs here; the body's captures (potentially heavy)
        // are destroyed without holding the scheduler mutex.
    }

    /// Snapshot exposed to the renderer header. Recomputes from a
    /// fresh hwinfo read (cached by `MemoryBudget`).
    struct LiveSnapshot {
        std::uint64_t total_bytes;
        std::uint64_t cap_bytes;
        std::uint64_t used_bytes;
        std::uint64_t reserved_bytes;
        std::uint64_t available_bytes;
        std::size_t   pending_count;
        std::size_t   running_count;
    };
    [[nodiscard]] LiveSnapshot live_snapshot() const noexcept {
        const auto sys = budget_.current_system_snapshot();
        const auto cap = MemoryBudget::cap_for(sys.total_bytes,
                                                budget_.memory_buffer_pct());
        const auto used = (sys.total_bytes > sys.free_bytes)
                              ? (sys.total_bytes - sys.free_bytes)
                              : std::uint64_t{0};
        std::lock_guard<std::mutex> guard(mu_);
        const auto reserved = static_cast<std::uint64_t>(
                                  std::max<std::int64_t>(reserved_bytes_, 0));
        const auto available = (cap > used + reserved)
                                  ? (cap - used - reserved)
                                  : std::uint64_t{0};
        return LiveSnapshot{
            .total_bytes     = sys.total_bytes,
            .cap_bytes       = cap,
            .used_bytes      = used,
            .reserved_bytes  = reserved,
            .available_bytes = available,
            .pending_count   = pending_.size(),
            .running_count   = running_batches_.size(),
        };
    }

    [[nodiscard]] double memory_buffer_pct() const noexcept {
        return budget_.memory_buffer_pct();
    }

private:
    /// Scheduler thread body — admits whatever fits, then sleeps until
    /// either a notify or the next tick. The hwinfo snapshot is taken
    /// OUTSIDE `mu_` so the only path that takes both `mu_` and
    /// `cache_mu_` is `live_snapshot()`/`scheduler_loop`, both in the
    /// `mu_`-released → `cache_mu_`-acquired order. Keeps the lock
    /// hierarchy single-direction and deadlock-free.
    void scheduler_loop() noexcept {
        std::unique_lock<std::mutex> guard(mu_);
        while (running_.load(std::memory_order_acquire)) {
            guard.unlock();
            const auto system_snapshot = budget_.current_system_snapshot();
            guard.lock();
            check_idle_timeouts_locked();
            admit_locked(system_snapshot);
            cv_.wait_for(guard, kSchedulerTickInterval);
        }
    }

    /// Greedy admission. See main/DOCS/BENCHMARK_FRAMEWORK/BENCH_V16_IMPL_PLAN__DOC0233.md §3.3. The system
    /// memory snapshot is provided by `scheduler_loop` so this body
    /// never re-enters `current_system_snapshot()` while holding `mu_`.
    void admit_locked(const SystemMemorySnapshot& sys) noexcept {
        if (pending_.empty()) return;

        const auto cap = MemoryBudget::cap_for(sys.total_bytes,
                                                budget_.memory_buffer_pct());
        const auto used = (sys.total_bytes > sys.free_bytes)
                              ? (sys.total_bytes - sys.free_bytes)
                              : std::uint64_t{0};
        std::uint64_t available =
            (cap > used + static_cast<std::uint64_t>(reserved_bytes_))
                ? (cap - used - static_cast<std::uint64_t>(reserved_bytes_))
                : std::uint64_t{0};

        // Smallest-first greedy: a small batch admits even if the next
        // larger one would push us over `available`. With closed-set
        // enqueue (no streaming), starvation is impossible (see plan
        // §4.17).
        std::stable_sort(pending_.begin(), pending_.end(),
            [](const ScheduledBatch& lhs, const ScheduledBatch& rhs) noexcept {
                return (lhs.base_bytes + lhs.extra_bytes)
                     < (rhs.base_bytes + rhs.extra_bytes);
            });

        auto pending_iter = pending_.begin();
        while (pending_iter != pending_.end()) {
            const auto need = static_cast<std::uint64_t>(
                                   pending_iter->base_bytes
                                 + pending_iter->extra_bytes);
            const bool fits = need <= available;
            const bool bench_ok = bench_lock_admissible_locked(*pending_iter);
            const bool concurrency_ok =
                concurrency_admissible_locked(*pending_iter);

            if (fits && bench_ok && concurrency_ok) {
                available -= need;
                reserved_bytes_ += static_cast<std::int64_t>(need);
                if (pending_iter->bench_lock) {
                    ++active_bench_lock_holder_[pending_iter->benchmark_index];
                }
                ++running_per_benchmark_[pending_iter->benchmark_index];

                pending_iter->last_idle_start_ns = 0;
                mirror_idle_to_ui(*pending_iter, /*idle_start=*/0);

                running_batches_.push_back(std::move(*pending_iter));
                pending_iter = pending_.erase(pending_iter);

                // The enqueued lambda is an enclosing wrapper that
                // owns the ScopedSchedulerFinish on its own stack. The
                // wrapper lives in TBB's task_handle storage, the
                // inner body lives in the std::list node. Erasing the
                // node from notify_batch_finished only destroys the
                // inner body (which has already returned by then).
                auto& running_ref = running_batches_.back();
                const auto running_id = running_ref.scheduler_id;
                arena_.enqueue([this, &running_ref, running_id] {
                    ScopedSchedulerFinish finish_guard{this, running_id};
                    running_ref.body();
                }, tg_);
            } else {
                // Update idle clock for the rejected batch. While
                // peers are running we RESET the timer to "now" (it
                // never crosses kMaxIdleWaitingTime). When this is
                // the sole holdout we LATCH the timer at the first
                // observation.
                const auto now_ns = static_cast<std::int64_t>(
                                         bioseq::instr::clock::now_ns());
                if (!running_batches_.empty()) {
                    pending_iter->last_idle_start_ns = now_ns;
                } else if (pending_iter->last_idle_start_ns == 0) {
                    pending_iter->last_idle_start_ns = now_ns;
                }
                mirror_idle_to_ui(*pending_iter,
                                   pending_iter->last_idle_start_ns);
                ++pending_iter;
            }
        }
    }

    /// Walk pending and abort any batch that has been the sole
    /// holdout for longer than `kMaxIdleWaitingTime`.
    void check_idle_timeouts_locked() noexcept {
        if (!running_batches_.empty()) return;
        const auto now_ns = static_cast<std::int64_t>(
                                bioseq::instr::clock::now_ns());
        constexpr auto budget_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                kMaxIdleWaitingTime).count();
        auto pending_iter = pending_.begin();
        while (pending_iter != pending_.end()) {
            if (pending_iter->last_idle_start_ns != 0
                && (now_ns - pending_iter->last_idle_start_ns) > budget_ns) {
                emit_idle_timeout_locked(*pending_iter);
                const auto bench_idx = pending_iter->benchmark_index;
                pending_iter = pending_.erase(pending_iter);
                const auto outstanding_it =
                    outstanding_per_benchmark_.find(bench_idx);
                if (outstanding_it != outstanding_per_benchmark_.end()
                    && --outstanding_it->second == 0)
                {
                    outstanding_per_benchmark_.erase(outstanding_it);
                }
                ++total_finished_;
            } else {
                ++pending_iter;
            }
        }
    }

    /// Forward declaration: defined out-of-line below the
    /// `progress_mark_batch_aborted` / `push_runtime_diagnostic`
    /// helpers (which the scheduler depends on but which appear
    /// later in this module).
    void emit_idle_timeout_locked(const ScheduledBatch& batch) noexcept;

    [[nodiscard]] bool bench_lock_admissible_locked(
        const ScheduledBatch& batch) const noexcept
    {
        if (active_bench_lock_holder_.empty()) return true;
        // Some benchmark currently holds the bench-lock. Only
        // batches from THE SAME benchmark may join it. (This
        // preserves V12's "isolate from siblings" semantics while
        // permitting intra-benchmark parallelism.)
        const auto& holder_pair = *active_bench_lock_holder_.begin();
        return batch.benchmark_index == holder_pair.first;
    }

    [[nodiscard]] bool concurrency_admissible_locked(
        const ScheduledBatch& batch) const noexcept
    {
        if (!batch.concurrency_serial) return true;
        const auto it = running_per_benchmark_.find(batch.benchmark_index);
        return it == running_per_benchmark_.end() || it->second == 0;
    }

    /// Helper used by `notify_batch_finished` to decrement-or-erase a
    /// `benchmark_index → count` map, with `[[present]]` guarding the
    /// `active_bench_lock_holder_` case where only `bench_lock=true`
    /// batches contribute. Caller holds `mu_`.
    template <typename Map>
    static void decrement_or_erase_locked(
        Map& map, typename Map::key_type key, bool present) noexcept
    {
        if (!present) return;
        const auto it = map.find(key);
        DEBUG_ASSERT(it != map.end(),
            "BatchScheduler counter map underflow");
        if (it != map.end() && --it->second == 0) {
            map.erase(it);
        }
    }

    MemoryBudget&                budget_;
    oneapi::tbb::task_arena&     arena_;
    oneapi::tbb::task_group      tg_{};
    std::thread                  thread_{};
    mutable std::mutex           mu_{};
    std::condition_variable      cv_{};
    std::atomic<bool>            running_{true};

    std::vector<ScheduledBatch>  pending_{};
    /// `std::list` (not vector) so iterator/pointer stability holds
    /// across subsequent pushes. The lambda enqueued into TBB
    /// captures `&running_batches_.back()`; a vector reallocation
    /// would invalidate the pointer.
    std::list<ScheduledBatch>    running_batches_{};
    std::size_t                  next_id_         = 0;
    std::size_t                  total_enqueued_  = 0;
    std::size_t                  total_finished_  = 0;
    std::int64_t                 reserved_bytes_  = 0;

    /// `benchmark_index → count of running bench_lock=true batches`.
    /// Empty ⇒ no benchmark holds the bench-lock. Non-empty ⇒ exactly
    /// one key (the holder) — `bench_lock_admissible_locked` rejects
    /// anyone else.
    std::map<std::size_t, std::size_t> active_bench_lock_holder_{};
    /// `benchmark_index → count of running batches` — used to honour
    /// `concurrency.concurrency: false`.
    std::map<std::size_t, std::size_t> running_per_benchmark_{};
    /// `benchmark_index → enqueued − finished` count. Drained by
    /// `wait_for_benchmark`.
    std::map<std::size_t, std::size_t> outstanding_per_benchmark_{};
};

inline ScopedSchedulerFinish::~ScopedSchedulerFinish() noexcept {
    scheduler->notify_batch_finished(scheduler_id);
}

// ----------------- Diagnostics -----------------
//
// Validation produces a flat list of `ValidationDiag` records. Both
// schema-driven errors (from the JSON schema validator) and post-parse
// semantic checks share this type so `report_diagnostics` can pretty-print
// them uniformly. `Severity::Error` aborts; `Severity::Warning` is logged
// but does not stop the run.

/// One validation finding — schema or semantic.
struct ValidationDiag {
    enum class Severity : std::uint8_t { Error, Warning };
    Severity    severity = Severity::Error;
    /// Short machine-readable token (e.g. "unknown_algorithm",
    /// "value_type_mismatch", "schema").
    std::string code;
    /// Human-readable origin string. For schema errors this contains the
    /// JSON Pointer into the offending document; for semantic errors it
    /// names the config path + benchmark key + relative location.
    std::string location;
    std::string message;
};

/// Custom error handler that captures every validation error (vs. the
/// default `basic_error_handler` that throws on the first). One instance
/// per validation pass; collected entries are converted to `ValidationDiag`
/// records by `validate_against_schema`.
class CollectingErrorHandler : public nlohmann::json_schema::basic_error_handler {
public:
    void error(const nlohmann::json::json_pointer& pointer,
               const nlohmann::json& /*instance*/,
               const std::string& message) override
    {
        nlohmann::json_schema::basic_error_handler::error(pointer, {}, message);
        captured_.push_back({pointer.to_string(), message});
    }

    struct Entry { std::string pointer; std::string message; };
    [[nodiscard]] const std::vector<Entry>& entries() const noexcept { return captured_; }

private:
    std::vector<Entry> captured_;
};

// ----------------- Schema definitions (built-in) -----------------
//
// Both schemas are inlined as raw string literals so the framework has no
// external dependency at runtime. They double as documentation: every
// nontrivial property carries a `description` and many carry `examples`.
//
// Conventions inside the schemas:
//   * `propertyNames: { minLength: 1 }` rejects empty identifiers (empty
//     benchmark keys, empty algorithm names, empty rule names, etc.).
//   * `oneOf` is used to make alternatives readable to a human (the metric
//     spec's three flavours, the comparator's `<`/`>`/`=` split).
//   * `additionalProperties: false` is preferred over open-ended objects
//     except where the data is genuinely free-form (e.g. test_case `data`
//     blob, rule selector partial matches, parameter values).
//
// Anything the schema cannot express (cross-references between
// `matrix.over.<algo>` and `params.<algo>`, value-vs-declared-type checks,
// per-rule duplicate-name checks) is enforced by `semantic_validate_config`.

inline constexpr std::string_view kBenchConfigSchema = R"json({
  "$schema": "http://json-schema.org/draft-07/schema#",
  "title": "BenchFrameworkConfig",
  "description": "Configuration for one or more benchmarks driven by bioseq::bench_framework. The top-level object always carries the keys 'params' and 'test_cases'; every other top-level key names a benchmark and matches the BenchmarkBlock schema. Benchmark identifiers must be non-empty.",
  "type": "object",
  "required": ["params", "test_cases"],
  "propertyNames": {
    "minLength": 1,
    "description": "Top-level keys (params, test_cases, plus benchmark identifiers) must be non-empty strings."
  },
  "properties": {
    "params": {
      "title": "AlgorithmParameterSchemas",
      "description": "Parameter schemas grouped by algorithm. Each algorithm key is a free-form name (the same name the user passes to on_run / on_evaluate). Parameter values used in 'matrix.over' and rule selectors are validated against these declarations.",
      "type": "object",
      "minProperties": 1,
      "propertyNames": { "minLength": 1 },
      "additionalProperties": {
        "type": "object",
        "minProperties": 1,
        "description": "Per-algorithm parameter map. Each key is a parameter name; the value follows the ParamSchema definition.",
        "propertyNames": { "minLength": 1 },
        "additionalProperties": { "$ref": "#/definitions/param_schema" }
      },
      "examples": [
        {
          "bioseq_aligner": {
            "MinimizerAlgorithmType": {
              "type": "string",
              "enum": ["SIMD", "SCALAR"],
              "description": "Minimizer-extraction backend"
            },
            "w": {
              "type": "integer",
              "default": 10,
              "description": "Minimizer window size"
            }
          }
        }
      ]
    },
    "test_cases": {
      "title": "TestCases",
      "description": "Abstract test cases. Each entry's 'data' field is an opaque JSON object handed verbatim to the user's on_input lambda; the framework itself does not interpret it. The optional 'concurrency' integer caps how many parallel groups the framework may split this test case into; the actual group count is min(concurrency, total_iterations_for_test_case).",
      "type": "object",
      "minProperties": 1,
      "propertyNames": { "minLength": 1 },
      "additionalProperties": {
        "type": "object",
        "required": ["data"],
        "additionalProperties": false,
        "properties": {
          "description": {
            "type": "string",
            "description": "Human-readable label shown in the report. May be empty."
          },
          "concurrency": {
            "type": "integer",
            "minimum": 1,
            "description": "Maximum number of parallel groups this test case may be split across. Defaults to max(1, hardware_concurrency() - 1) when omitted. Each group gets its own cached Input, so heavy inputs are duplicated across groups — set this to 1 for inputs that are too big to clone."
          },
          "data": {
            "type": "object",
            "description": "Application-defined object passed to on_input. Schema is up to the user."
          }
        }
      },
      "examples": [
        { "SMALL": { "description": "100bp random sequence", "concurrency": 4, "data": { "length": 100 } } }
      ]
    },
    "regression_rules": {
      "title": "RegressionRules",
      "description": "Optional list of regression rules. Each rule compares aggregated samples in this run to those in the immediately preceding report on disk (report_<N-1>.json) and emits a 'regressions' entry whenever the configured operator/tolerance is violated. Regression rules are global to the config file: by default they apply to every benchmark in the same file. The framework writes its run as report_<N>.json (postfixed integer starting at 1).",
      "type": "array",
      "items": { "$ref": "#/definitions/regression_rule" }
    }
  },
  "additionalProperties": { "$ref": "#/definitions/benchmark" },
  "definitions": {
    "param_schema": {
      "title": "ParamSchema",
      "description": "Subset of JSON Schema describing one algorithm parameter. 'type' is mandatory; 'default' is required for any parameter not enumerated in 'matrix.over' (otherwise matrix expansion has nothing to fill in).",
      "type": "object",
      "required": ["type"],
      "additionalProperties": false,
      "properties": {
        "type": {
          "type": "string",
          "enum": ["string", "integer", "number", "boolean"],
          "description": "JSON-Schema-style type. Used to validate values supplied in 'matrix.over' and in rule selectors."
        },
        "default": {
          "description": "Default value used when this parameter is not enumerated in 'matrix.over'. Must match 'type'."
        },
        "enum": {
          "type": "array",
          "minItems": 1,
          "description": "Optional whitelist of allowed values. When present, every value in 'matrix.over' and every rule-selector value for this parameter must appear here."
        },
        "description": {
          "type": "string",
          "description": "Human-readable label for the parameter."
        }
      },
      "examples": [
        { "type": "integer", "default": 10, "description": "minimizer window size" },
        { "type": "string", "enum": ["A", "B"], "description": "algorithm variant" }
      ]
    },
    "benchmark": {
      "title": "BenchmarkBlock",
      "description": "Definition of one benchmark. Lives under a free-form key at the top level (e.g. 'bench_perf'). Multiple benchmark blocks may share one config file; each is invoked by a separate create_benchmark<...>(...) call referencing the matching key.",
      "type": "object",
      "required": ["matrix"],
      "additionalProperties": false,
      "properties": {
        "description": {
          "type": "string",
          "description": "Human-readable summary. May be empty."
        },
        "fast": {
          "type": "boolean",
          "default": false,
          "description": "If true, this benchmark runs only when the binary is invoked with --fast. If false (default), it runs unless --fast is passed."
        },
        "concurrency": { "$ref": "#/definitions/concurrency" },
        "matrix": { "$ref": "#/definitions/matrix" }
      },
      "examples": [
        {
          "description": "Compare aligner runtimes",
          "fast": false,
          "concurrency": { "bench_lock": false, "concurrency": true },
          "matrix": {
            "for": { "SMALL": 1000 },
            "over": { "bioseq_aligner": {} },
            "metrics": { "wall_clock_ns": { "rules": [] } }
          }
        }
      ]
    },
    "concurrency": {
      "title": "ConcurrencyOptions",
      "description": "Per-benchmark concurrency knobs. Defaults: bench_lock=false, concurrency=true. (V11 dropped 'test_case_lock' — the new batched scheduler runs at most one tuple at a time against a given (test_case, group) cached Input, so the lock had no purpose.)",
      "type": "object",
      "additionalProperties": false,
      "properties": {
        "bench_lock": {
          "type": "boolean",
          "default": false,
          "description": "If true, while any of this benchmark's batches is running the scheduler will not admit batches from other benchmarks. Multiple batches of THIS benchmark may still run concurrently when concurrency.concurrency is true (intra-benchmark parallelism is preserved). Set concurrency.concurrency to false in addition for strict latency-stable per-iteration measurement."
        },
        "concurrency": {
          "type": "boolean",
          "default": true,
          "description": "If true (default), this benchmark's batches are distributed across TBB workers. If false, all items inside this benchmark run serially behind a per-benchmark mutex (one batch's iteration loop at a time, even across different test_cases)."
        }
      }
    },
    "matrix": {
      "title": "BenchmarkMatrix",
      "description": "The cross-product specification: which test cases to run, which algorithm parameter combinations to enumerate, what to measure, and what rules to validate.",
      "type": "object",
      "required": ["for", "over", "metrics"],
      "additionalProperties": false,
      "properties": {
        "for": {
          "title": "MatrixFor",
          "type": "object",
          "minProperties": 1,
          "description": "Maps each test_case name to its iteration count. Keys must appear in the top-level 'test_cases' map (validated semantically; the JSON schema only enforces non-empty + non-empty-keys).",
          "propertyNames": { "minLength": 1 },
          "additionalProperties": {
            "type": "integer",
            "minimum": 0,
            "description": "Number of iterations. Zero is legal (test case is skipped) but produces a warning."
          },
          "examples": [ { "SMALL": 1000, "MEDIUM": 100 } ]
        },
        "over": {
          "title": "MatrixOver",
          "type": "object",
          "minProperties": 1,
          "description": "Cross-product axes per algorithm. Each top-level key is an algorithm name (must appear in top-level 'params'); each inner key is a parameter name (must appear in 'params.<algorithm>'); each value is the array of values to enumerate. Parameters not listed here use the default from 'params.<algorithm>.<param>.default'. An empty inner object {} means 'use defaults for everything'.",
          "propertyNames": { "minLength": 1 },
          "additionalProperties": {
            "type": "object",
            "description": "Per-algorithm cross-product axis map. Empty {} means use only defaults.",
            "propertyNames": { "minLength": 1 },
            "additionalProperties": {
              "type": "array",
              "minItems": 1,
              "description": "List of parameter values to enumerate. Each value must match the parameter's declared 'type' (and 'enum', if present)."
            }
          },
          "examples": [
            {
              "bioseq_aligner": {
                "MinimizerAlgorithmType": ["SIMD", "SCALAR"],
                "w": [10, 15, 20]
              }
            }
          ]
        },
        "by_regions": {
          "type": "boolean",
          "default": false,
          "description": "If true, the report splits per-iteration wall_clock_ns by instrumentation region (every named bioseq::instr::region scope contributes its own bucket). When false, only the synthetic 'all_regions' bucket is emitted."
        },
        "metrics": {
          "title": "MetricSpecs",
          "type": "object",
          "minProperties": 1,
          "description": "Metrics to collect, keyed by metric name. The framework recognises three flavours selected via the 'type' field; see the MetricSpec oneOf for details.",
          "propertyNames": { "minLength": 1 },
          "additionalProperties": { "$ref": "#/definitions/metric_spec" }
        }
      }
    },
    "metric_spec": {
      "title": "MetricSpec",
      "description": "One metric. 'type' selects the flavour. Five families: (a) BUILTIN — outer wall_clock_ns measured around on_run; metric key MUST be 'wall_clock_ns' and 'type' is implicit when omitted. (b) REGION — a scalar derived from one or every visited bioseq::instr::region's RegionAggregate (e.g. region_total_ns, region_visits, region_p50_ns, region_max_ns, region_total_cycles, region_total_instructions, region_total_branches, region_total_branch_misses, region_ipc, region_branch_miss_rate). (c) LOOP — a scalar from one or every visited bioseq::instr::loop's LoopAggregate (e.g. loop_total_iterations, loop_total_work, loop_p50_per_iter_ns, loop_max_per_iter_ns, loop_mean_per_iter_ns, loop_work_per_iter, loop_empty_visits). (d) COUNTER — snapshot a global bioseq::instr counter; forces serial iteration. (e) EVALUATION — value returned by the user's on_evaluate handler. The metric KEY must match one of the registered builtin names for REGION/LOOP/BUILTIN and is enforced semantically.",
      "oneOf": [
        { "$ref": "#/definitions/metric_spec_builtin" },
        { "$ref": "#/definitions/metric_spec_region" },
        { "$ref": "#/definitions/metric_spec_loop" },
        { "$ref": "#/definitions/metric_spec_counter" },
        { "$ref": "#/definitions/metric_spec_evaluation" }
      ]
    },
    "metric_spec_builtin": {
      "title": "BuiltinMetric",
      "description": "Built-in wall-clock metric. The metric key MUST be 'wall_clock_ns'; the framework times every iteration with bioseq::instr::clock::now_ns() around on_run and writes the result under 'all_regions.wall_clock_ns'. The 'type' field is optional ('builtin' is implicit when omitted).",
      "type": "object",
      "additionalProperties": false,
      "properties": {
        "type": {
          "type": "string",
          "const": "builtin",
          "description": "Optional; defaults to 'builtin' when absent."
        },
        "rules": { "$ref": "#/definitions/rule_array" }
      }
    },
    "metric_spec_region": {
      "title": "RegionAggregateMetric",
      "description": "A scalar derived from each iteration's bioseq::instr::region aggregates. Recognised metric keys: region_total_ns, region_visits, region_min_ns, region_max_ns, region_p50_ns, region_p99_ns, region_total_cycles, region_total_instructions, region_total_branches, region_total_branch_misses, region_ipc, region_branch_miss_rate. Without 'region', cross-descriptor combiners apply: Sum for additive metrics, summed-histogram + hist_percentile for p50/p99, event-weighted ratios for IPC and branch_miss_rate. region_min_ns / region_max_ns REQUIRE 'region' (cross-region min/max is meaningless). PMU-derived metrics (cycles, instructions, branches, branch_misses, ipc, branch_miss_rate) report 0 on platforms without PMU access; the framework warns once via the pmu_unavailable diagnostic. region_p50_ns / region_p99_ns return the upper bound of the 32-bucket log2 histogram bucket containing the requested percentile and overshoot the true percentile by up to a factor of 2 — treat them as order-of-magnitude tail-latency indicators, not exact quantiles.",
      "type": "object",
      "required": ["type"],
      "additionalProperties": false,
      "properties": {
        "type": { "type": "string", "const": "region" },
        "region": {
          "type": "string",
          "minLength": 1,
          "description": "Restrict this metric to a single descriptor whose name equals this string (exact match). Mandatory for region_min_ns / region_max_ns; optional for other region metrics."
        },
        "rules": { "$ref": "#/definitions/rule_array" }
      },
      "examples": [
        { "type": "region", "region": "mm_sketch", "rules": [] },
        { "type": "region", "rules": [] }
      ]
    },
    "metric_spec_loop": {
      "title": "LoopAggregateMetric",
      "description": "A scalar derived from each iteration's bioseq::instr::loop aggregates. Recognised metric keys: loop_total_iterations, loop_total_work, loop_empty_visits, loop_min_per_iter_ns, loop_max_per_iter_ns, loop_p50_per_iter_ns, loop_p99_per_iter_ns, loop_mean_per_iter_ns, loop_work_per_iter. Without 'region', cross-descriptor combiners apply (Sum, histogram-summed percentile, ratio-weighted). loop_min_per_iter_ns / loop_max_per_iter_ns REQUIRE 'region'. loop_p50_per_iter_ns / loop_p99_per_iter_ns are coarse percentiles from the 32-bucket log2 per-iteration histogram (bucket-upper-bound, up to factor-of-2 overshoot) — treat as order-of-magnitude only.",
      "type": "object",
      "required": ["type"],
      "additionalProperties": false,
      "properties": {
        "type": { "type": "string", "const": "loop" },
        "region": {
          "type": "string",
          "minLength": 1,
          "description": "Restrict this metric to a single loop-descriptor whose name equals this string. Mandatory for loop_min_per_iter_ns / loop_max_per_iter_ns; optional otherwise."
        },
        "rules": { "$ref": "#/definitions/rule_array" }
      },
      "examples": [
        { "type": "loop", "region": "mm_sketch_inner", "rules": [] }
      ]
    },
    "metric_spec_counter": {
      "title": "CounterMetric",
      "description": "Snapshot-based counter metric. The framework reads bioseq::instr::counter('<counter>').value before and after each iteration and records the delta. Counters use per-thread storage as of V5, so the before/after delta on a TBB worker only reflects that worker's increments — counter benchmarks may run in parallel.",
      "type": "object",
      "required": ["type"],
      "additionalProperties": false,
      "properties": {
        "type": { "type": "string", "const": "counter" },
        "counter": {
          "type": "string",
          "minLength": 1,
          "description": "Canonical counter name to snapshot. Defaults to the metric key when omitted; provide explicitly when the counter's in-source name differs from the metric label you want in the report."
        },
        "rules": { "$ref": "#/definitions/rule_array" }
      },
      "examples": [
        { "type": "counter", "counter": "bioseq.minimizers.emitted", "rules": [] }
      ]
    },
    "metric_spec_evaluation": {
      "title": "EvaluationMetric",
      "description": "User-computed metric. The metric key MUST appear in the std::map<std::string,double> returned by on_evaluate — the framework reads it from there each iteration. Metrics declared but never emitted by the handler raise a post-run error.",
      "type": "object",
      "required": ["type"],
      "additionalProperties": false,
      "properties": {
        "type": { "type": "string", "const": "evaluation" },
        "rules": { "$ref": "#/definitions/rule_array" }
      },
      "examples": [
        { "type": "evaluation", "rules": [
            ["accuracy_above_threshold",
             { "key": "my_algo" },
             { "compare": ">", "threshold%": 0 },
             { "key": "baseline" }]
          ]
        }
      ]
    },
    "rule_array": {
      "type": "array",
      "items": { "$ref": "#/definitions/rule" },
      "description": "List of rules to evaluate against this metric. May be empty."
    },
    "rule": {
      "title": "Rule",
      "description": "A 4-tuple [name, lhs_selector, comparator, rhs_selector] expressed as a JSON array. The rule iterates over every (lhs_cell, rhs_cell) pair where the selectors match for the same test_case, takes the median of each side's metric array under 'all_regions', and applies the comparator.",
      "type": "array",
      "minItems": 4,
      "maxItems": 4,
      "items": [
        {
          "type": "string",
          "minLength": 1,
          "description": "Rule name. Reported in violations and the stderr summary. Must be unique within the benchmark."
        },
        { "$ref": "#/definitions/selector" },
        { "$ref": "#/definitions/comparator" },
        { "$ref": "#/definitions/selector" }
      ],
      "examples": [
        ["bioseq_faster_than_minimap",
         { "key": "bioseq", "MinimizerAlgorithmType": "SCALAR" },
         { "compare": "<", "threshold%": 10, "tolerance%": 1 },
         { "key": "minimap2" }]
      ]
    },
    "selector": {
      "title": "RuleSelector",
      "description": "Partial-match selector for one side of a rule. The 'key' field names the algorithm; every other field is a (param_name, expected_value) constraint. Only the constrained params must match — others are wildcarded. The algorithm name and every constraint key + value are validated semantically against the algorithm's parameter schema in 'params'.",
      "type": "object",
      "required": ["key"],
      "additionalProperties": true,
      "propertyNames": {
        "minLength": 1,
        "description": "Selector field names ('key' plus any partial-match parameter names) cannot be empty strings."
      },
      "properties": {
        "key": {
          "type": "string",
          "minLength": 1,
          "description": "Algorithm name (must appear in top-level 'params')."
        }
      },
      "examples": [
        { "key": "bioseq", "MinimizerAlgorithmType": "SCALAR", "w": 10 },
        { "key": "minimap2" }
      ]
    },
    "aggregate_enum": {
      "title": "Aggregate",
      "type": "string",
      "enum": ["sum", "min", "max", "p01", "p50", "p99"],
      "description": "Reduction applied to a per-iteration sample vector. Percentiles select the sample at index floor(p * (n - 1)) after a partial sort via std::nth_element (matches NumPy's 'lower' interpolation for discrete indices)."
    },
    "comparator": {
      "title": "RuleComparator",
      "description": "Two operator families: ordered ('<' or '>') and equality ('='). Spec semantics:\n  '<': pass iff lhs < rhs * (1 - threshold%/100 + tolerance%/100).\n  '>': pass iff lhs > rhs * (1 + threshold%/100 - tolerance%/100).\n  '=': pass iff |lhs - rhs| <= rhs * tolerance%/100.\nBoth sides are reduced via the optional 'aggregate' field (default 'p50' for backward compatibility with pre-V5 median behaviour).\nthreshold% and tolerance% are bounded to [0, 100]. A threshold% or tolerance% of exactly 100 makes the rule trivially pass and produces a warning.",
      "oneOf": [
        {
          "title": "OrderedComparator",
          "description": "Less-than or greater-than.",
          "type": "object",
          "required": ["compare"],
          "additionalProperties": false,
          "properties": {
            "compare": { "type": "string", "enum": ["<", ">"] },
            "threshold%": {
              "type": "number",
              "minimum": 0,
              "maximum": 100,
              "default": 0,
              "description": "Required improvement margin as a fraction of RHS (in percent). 0 means strict less-than / greater-than."
            },
            "tolerance%": {
              "type": "number",
              "minimum": 0,
              "maximum": 100,
              "default": 0,
              "description": "Slack relaxing the threshold. 100 makes the rule always pass — flagged as a warning."
            },
            "aggregate": { "$ref": "#/definitions/aggregate_enum" }
          },
          "examples": [
            { "compare": "<", "threshold%": 10, "tolerance%": 1, "aggregate": "p99" }
          ]
        },
        {
          "title": "EqualityComparator",
          "description": "Numeric equality. Does NOT take a threshold (use tolerance%).",
          "type": "object",
          "required": ["compare"],
          "additionalProperties": false,
          "properties": {
            "compare": { "type": "string", "const": "=" },
            "tolerance%": {
              "type": "number",
              "minimum": 0,
              "maximum": 100,
              "default": 0,
              "description": "Tolerance window around RHS, in percent. 100 makes the rule always pass — flagged as a warning."
            },
            "aggregate": { "$ref": "#/definitions/aggregate_enum" }
          },
          "examples": [
            { "compare": "=", "tolerance%": 1 }
          ]
        }
      ]
    },
    "regression_rule": {
      "title": "RegressionRule",
      "description": "One regression check. Iterates a target metric across (benchmark, execution_tuple, test_case, region) combos in the current run, aggregates the samples per the 'aggregate' field, and compares to the same combo in the previous report (report_<N-1>.json). Operator semantics are inherited from the rule comparator with 'threshold%' fixed at 0; the schema rejects any explicit 'threshold%' in the operator block.",
      "type": "object",
      "required": ["name", "metric", "aggregate", "operator"],
      "additionalProperties": false,
      "properties": {
        "name": {
          "type": "string",
          "minLength": 1,
          "description": "Rule name. Must be unique within the config file. Reported in 'regressions' entries and the stderr summary."
        },
        "benchmarks": {
          "type": "array",
          "minItems": 1,
          "description": "Optional list of benchmark identifiers this rule applies to. Each entry must name a benchmark block declared in the same config file. When the field is absent, the rule applies to every benchmark in the file.",
          "items": { "type": "string", "minLength": 1 }
        },
        "regions": { "$ref": "#/definitions/regression_regions" },
        "metric": {
          "type": "string",
          "minLength": 1,
          "description": "Metric name. Must be declared in the 'metrics' map of every benchmark this rule applies to (when 'benchmarks' is explicit), or in at least one benchmark in the file (when 'benchmarks' is absent)."
        },
        "aggregate": { "$ref": "#/definitions/aggregate_enum" },
        "operator": { "$ref": "#/definitions/regression_operator" },
        "exclude_cases": {
          "type": "array",
          "minItems": 1,
          "description": "Optional list of test_case names to skip when this rule fires. Useful when a metric is too small (e.g. wall-clock in nanoseconds) for the tolerance to make sense on a particular case. Cannot be empty if present.",
          "items": { "type": "string", "minLength": 1 }
        }
      },
      "examples": [
        {
          "name": "minimizer_count_stable",
          "benchmarks": ["bench1"],
          "metric": "minimizer_count",
          "aggregate": "sum",
          "operator": { "compare": "=", "tolerance%": 2 },
          "exclude_cases": ["SMALL"]
        }
      ]
    },
    "regression_regions": {
      "title": "RegressionRegions",
      "description": "Set of region buckets to read from each cell. Either the literal string 'all_regions' (default when omitted) or a non-empty array of region names. Each entry must match a key under 'test_cases.<name>.<here>' in the report — for the synthetic combined bucket use 'all_regions'.",
      "oneOf": [
        { "type": "string", "const": "all_regions" },
        {
          "type": "array",
          "minItems": 1,
          "items": { "type": "string", "minLength": 1 }
        }
      ]
    },
    "regression_operator": {
      "title": "RegressionOperator",
      "description": "Comparator for a regression rule. Accepts the same three operators as the per-rule comparator (<, >, =) but does NOT accept 'threshold%' — that knob is implicitly zero, leaving 'tolerance%' as the only relaxation. Pass conditions (with t = tolerance% / 100): '<' passes iff now < previously*(1+t); '>' passes iff now > previously*(1-t); '=' passes iff |now - previously| <= |previously|*t.",
      "oneOf": [
        {
          "title": "RegressionOrderedOperator",
          "type": "object",
          "required": ["compare"],
          "additionalProperties": false,
          "properties": {
            "compare": { "type": "string", "enum": ["<", ">"] },
            "tolerance%": {
              "type": "number",
              "minimum": 0,
              "maximum": 100,
              "default": 0,
              "description": "Tolerance window applied to the previous-report aggregate (additive on the comparison side). 100 makes the rule always pass — surfaced as a warning by semantic validation."
            }
          },
          "examples": [
            { "compare": "<", "tolerance%": 5 }
          ]
        },
        {
          "title": "RegressionEqualityOperator",
          "type": "object",
          "required": ["compare"],
          "additionalProperties": false,
          "properties": {
            "compare": { "type": "string", "const": "=" },
            "tolerance%": {
              "type": "number",
              "minimum": 0,
              "maximum": 100,
              "default": 0,
              "description": "Symmetric tolerance window around the previous-report aggregate. 100 makes the rule always pass — surfaced as a warning."
            }
          },
          "examples": [
            { "compare": "=", "tolerance%": 2 }
          ]
        }
      ]
    }
  }
})json";

inline constexpr std::string_view kBenchReportSchema = R"json({
  "$schema": "http://json-schema.org/draft-07/schema#",
  "title": "BenchFrameworkReport",
  "description": "Output report.json produced by bioseq::bench_framework. Top-level fields: 'meta' (run identity), 'rule_violations' (every rule that didn't pass), 'regressions' (cross-run regression entries), 'sanity_warnings' (hardcoded end-of-run anomaly checks; non-blocking) and 'benchmarks' (per-benchmark sample arrays). The framework re-validates its own report against this schema after writing as a sanity check.",
  "type": "object",
  "required": ["meta", "rule_violations", "regressions", "sanity_warnings", "benchmarks"],
  "additionalProperties": false,
  "properties": {
    "meta": { "$ref": "#/definitions/meta" },
    "rule_violations": {
      "type": "array",
      "description": "Every rule that failed to pass. Empty when all rules were satisfied.",
      "items": { "$ref": "#/definitions/violation" }
    },
    "regressions": {
      "type": "array",
      "description": "Every regression rule violation detected against the previous report (report_<N-1>.json). Empty on the first run after a clean report_dir, when no regression rules are configured, or when nothing regressed.",
      "items": { "$ref": "#/definitions/regression_entry" }
    },
    "sanity_warnings": {
      "type": "array",
      "description": "Hardcoded sanity-check findings emitted at the very end of the run. Catches anomalies that no other validation pass could have flagged (a metric whose values are bit-identical across every benchmark/algorithm/test_case/region looks suspicious — broken instrumentation like add_counter(0), an evaluator stuck on a constant, a frozen clock). Cannot be disabled and never affects the exit code; surfaced here and on stderr so the operator notices.",
      "items": { "$ref": "#/definitions/sanity_warning" }
    },
    "benchmarks": {
      "type": "object",
      "description": "Per-benchmark report data, keyed by benchmark_key (the same key passed to create_benchmark).",
      "additionalProperties": { "$ref": "#/definitions/benchmark_report" }
    }
  },
  "definitions": {
    "meta": {
      "title": "ReportMeta",
      "description": "Identity card for one report. Captured once at run start and emitted as the first key of the output JSON. Distinguishes reports by commit, working-tree state, wall-clock timestamp, and whether --fast was active.",
      "type": "object",
      "required": ["commit", "dirty", "timestamp", "created", "fast"],
      "additionalProperties": false,
      "properties": {
        "commit":    { "type": "string", "description": "git rev-parse HEAD output, or empty string when the working dir is not a git repo." },
        "dirty":     { "type": "boolean", "description": "true when git status --porcelain reported any uncommitted changes; false otherwise (also false when not a git repo)." },
        "timestamp": { "type": "integer", "description": "Unix seconds since epoch at run start." },
        "created":   { "type": "string", "description": "ISO 8601 UTC string built from the same instant as timestamp (e.g. 2026-05-07T12:34:56Z)." },
        "fast":      { "type": "boolean", "description": "true when the run was launched with --fast (only fast benchmarks executed)." }
      }
    },
    "param_value": {
      "description": "A single parameter value. ConfigValue serialises into one of these scalar JSON types: string, number (covers integer + double), or boolean.",
      "type": ["string", "number", "boolean"]
    },
    "param_value_object": {
      "description": "Map from parameter name to its concrete value (string / integer / number / boolean).",
      "type": "object",
      "additionalProperties": { "$ref": "#/definitions/param_value" }
    },
    "violation": {
      "title": "RuleViolation",
      "description": "One failed rule, with the (LHS, RHS) execution_tuple that triggered the failure and the median aggregate values that were compared.",
      "type": "object",
      "required": ["location", "benchmark", "rule_name", "execution_tuple", "test_case", "metric", "args"],
      "additionalProperties": false,
      "properties": {
        "location":  { "type": "string", "description": "Path to the config file the rule comes from." },
        "benchmark": { "type": "string", "description": "benchmark_key the rule belongs to." },
        "rule_name": { "type": "string", "description": "Rule name as declared in the config." },
        "execution_tuple": {
          "description": "Both LHS and RHS algorithm params that triggered this violation. 1-2 keys (LHS-only when both selectors named the same algorithm).",
          "type": "object",
          "minProperties": 1,
          "maxProperties": 2,
          "additionalProperties": { "$ref": "#/definitions/param_value_object" }
        },
        "test_case": { "type": "string", "description": "Name of the test_case the rule fired against." },
        "metric":    { "type": "string", "description": "Metric name compared by the rule." },
        "args": {
          "description": "[lhs_aggregate, rhs_aggregate]. Both numbers are the median of the metric's sample array under 'all_regions'.",
          "type": "array",
          "minItems": 2,
          "maxItems": 2,
          "items": { "type": "number" }
        }
      }
    },
    "benchmark_report": {
      "title": "BenchmarkReport",
      "type": "object",
      "required": ["title", "description", "location", "test_case_descriptions", "execution_tuples"],
      "additionalProperties": false,
      "properties": {
        "title":       { "type": "string", "description": "Title from create_benchmark." },
        "description": { "type": "string", "description": "Description from the benchmark block (may be empty)." },
        "location":    { "type": "string", "description": "Path of the config this benchmark was loaded from." },
        "test_case_descriptions": {
          "type": "object",
          "description": "Mirrors the 'description' fields from top-level 'test_cases'.",
          "additionalProperties": { "type": "string" }
        },
        "execution_tuples": {
          "type": "array",
          "items": { "$ref": "#/definitions/execution_tuple_record" }
        }
      }
    },
    "execution_tuple_record": {
      "title": "ExecutionTupleRecord",
      "description": "One (algorithm, params) row plus its measured samples. Single-key 'tuple' (one algorithm per row) — rule violations carry a 2-key version when comparing across algorithms.",
      "type": "object",
      "required": ["tuple", "test_cases"],
      "additionalProperties": false,
      "properties": {
        "tuple": {
          "type": "object",
          "minProperties": 1,
          "maxProperties": 1,
          "description": "Single key: algorithm name → its full parameter map (defaults inlined).",
          "additionalProperties": { "$ref": "#/definitions/param_value_object" }
        },
        "test_cases": {
          "type": "object",
          "description": "Per-test-case sample buckets.",
          "additionalProperties": { "$ref": "#/definitions/test_case_block" }
        }
      }
    },
    "test_case_block": {
      "title": "TestCaseBlock",
      "description": "Per-test-case sample buckets. The 'all_regions' key always exists and holds the iteration-level wall-clock plus every counter and evaluation metric. When 'by_regions' is true on the benchmark, additional keys appear — one per visited bioseq::instr region — each carrying that region's per-iteration wall_clock_ns.",
      "type": "object",
      "minProperties": 1,
      "additionalProperties": { "$ref": "#/definitions/region_block" }
    },
    "region_block": {
      "type": "object",
      "description": "Map from metric name to its aggregated samples. V17 replaced the per-iteration array with this fixed-shape summary so report.json stays compact even for benchmarks that emit thousands of samples per cell.",
      "additionalProperties": { "$ref": "#/definitions/aggregated_samples" }
    },
    "aggregated_samples": {
      "title": "AggregatedSamples",
      "description": "Fixed-shape summary of one (benchmark, tuple, test_case, region, metric) cell's per-iteration samples. Always reflects the FINITE subset (NaN / +inf / -inf are filtered before any arithmetic). 'samples_count' is the original emission count; 'k-best.top10'/'bottom10' and the histogram bucket counts together let downstream consumers reason about the data without the raw vector.",
      "type": "object",
      "required": ["samples_count", "stats", "percentiles", "k-best", "histogram"],
      "additionalProperties": false,
      "properties": {
        "samples_count": {
          "type": "integer",
          "minimum": 1,
          "description": "Number of per-iteration samples emitted into this cell. Includes non-finite samples (NaN / +inf / -inf); the finite subset that backs the other fields is implicit in the histogram (Σ histogram[i].count == finite_count <= samples_count)."
        },
        "stats":       { "$ref": "#/definitions/aggregate_stats" },
        "percentiles": { "$ref": "#/definitions/aggregate_percentiles" },
        "k-best":      { "$ref": "#/definitions/aggregate_kbest" },
        "histogram":   { "$ref": "#/definitions/aggregate_histogram" }
      }
    },
    "aggregate_stats": {
      "title": "AggregateStats",
      "description": "Per-cell summary statistics over the finite subset. Default-constructed (every field 0.0) when no finite samples were observed. 'mean90' is the trimmed mean: drop floor(0.1 * finite_count) samples from each end of the sorted finite subset, average the rest. Equals 'mean' when finite_count < 10. 'stddev' uses the n-1 denominator and falls to 0 when finite_count < 2 OR when 'mean' is non-finite (sum overflowed).",
      "type": "object",
      "required": ["min", "max", "mean", "mean90", "sum", "stddev"],
      "additionalProperties": false,
      "properties": {
        "min":    { "type": "number" },
        "max":    { "type": "number" },
        "mean":   { "type": "number" },
        "mean90": { "type": "number" },
        "sum":    { "type": "number" },
        "stddev": { "type": "number" }
      }
    },
    "aggregate_percentiles": {
      "title": "AggregatePercentiles",
      "description": "Discrete-index percentiles over the sorted finite subset: P{n} = sorted_finite[floor(n/100 * (finite_count - 1))]. P01 / P50 / P99 match the canonical aggregate semantics used by per-rule comparators and regression rules. P20 / P80 are reported for downstream UI consumers and are not currently rule-addressable.",
      "type": "object",
      "required": ["P01", "P20", "P50", "P80", "P99"],
      "additionalProperties": false,
      "properties": {
        "P01": { "type": "number" },
        "P20": { "type": "number" },
        "P50": { "type": "number" },
        "P80": { "type": "number" },
        "P99": { "type": "number" }
      }
    },
    "aggregate_kbest": {
      "title": "AggregateKBest",
      "description": "Top-N / bottom-N finite samples. 'top10' is sorted descending so top10[0] == stats.max; 'bottom10' is sorted ascending so bottom10[0] == stats.min. Both arrays carry min(10, finite_count) entries and have the same length. Empty arrays appear ONLY when the cell emitted no finite samples (every sample was NaN / inf); the JSON-schema 'minItems: 0' permits this case while 'maxItems: 10' enforces the cap.",
      "type": "object",
      "required": ["top10", "bottom10"],
      "additionalProperties": false,
      "properties": {
        "top10": {
          "type": "array",
          "minItems": 0,
          "maxItems": 10,
          "items": { "type": "number" }
        },
        "bottom10": {
          "type": "array",
          "minItems": 0,
          "maxItems": 10,
          "items": { "type": "number" }
        }
      }
    },
    "aggregate_histogram": {
      "title": "AggregateHistogram",
      "description": "Linear-binned 10-bucket histogram of the finite subset. Bucket width is (stats.max - stats.min) / 10; bucket i covers [from, to) with from = min + i*width and to = min + (i+1)*width, except the last bucket which is closed on the right (from <= x <= to) so 'max' lands inside it. When width == 0 (all finite samples equal, OR finite_count <= 1) every count goes to bucket 0; remaining buckets render from == to with count == 0. When finite_count == 0 every bucket is from = to = 0 with count = 0. Σ count <= finite_count <= samples_count.",
      "type": "array",
      "minItems": 10,
      "maxItems": 10,
      "items": {
        "type": "object",
        "required": ["from", "to", "count"],
        "additionalProperties": false,
        "properties": {
          "from":  { "type": "number" },
          "to":    { "type": "number" },
          "count": { "type": "integer", "minimum": 0 }
        }
      }
    },
    "regression_entry": {
      "title": "RegressionEntry",
      "description": "One detected regression. Granularity: one entry per (rule, benchmark, algorithm, tuple, test_case, region). 'violating_tuples' and 'test_cases' are arrays of size 1 in every entry — they exist as arrays so a future aggregation pass can group entries without breaking the schema. 'regression_since' is the basename of the oldest report on disk where the rule still fails when compared to the current run, or null when the framework hit a missing report (gap) or a tuple/region that disappeared in a historical run.",
      "type": "object",
      "required": ["location", "benchmark", "rule_name", "algorithm_name", "violating_tuples", "test_cases", "region", "metric", "aggregate", "previously", "now", "regression_since", "regression_since_commit"],
      "additionalProperties": false,
      "properties": {
        "location":       { "type": "string", "description": "Path to the config file the regression rule was declared in." },
        "benchmark":      { "type": "string", "description": "benchmark_key whose data violated the rule." },
        "rule_name":      { "type": "string", "description": "Regression rule name as declared in 'regression_rules'." },
        "algorithm_name": { "type": "string", "description": "Algorithm name from the violating execution tuple." },
        "violating_tuples": {
          "type": "array",
          "minItems": 1,
          "description": "Single-element list (one entry per violation). Each item is the parameter map of the violating tuple.",
          "items": { "$ref": "#/definitions/param_value_object" }
        },
        "test_cases": {
          "type": "array",
          "minItems": 1,
          "description": "Single-element list (one entry per violation). Names the violating test_case.",
          "items": { "type": "string" }
        },
        "region":     { "type": "string", "description": "Region key inside the test_case bucket where the regression was detected (e.g. 'all_regions' or a custom descriptor name)." },
        "metric":     { "type": "string", "description": "Metric name from the regression rule." },
        "aggregate":  { "type": "string", "enum": ["sum", "min", "max", "p01", "p50", "p99"] },
        "previously": { "type": "number", "description": "Aggregated metric value from the previous report's matching cell." },
        "now":        { "type": "number", "description": "Aggregated metric value from the current run's matching cell." },
        "regression_since": {
          "type": ["string", "null"],
          "description": "Basename of the report where the regression was first observable (oldest historical report still failing against current). Null when the walk hit a history gap."
        },
        "regression_since_commit": {
          "type": ["string", "null"],
          "description": "Commit SHA captured by the historical report identified by 'regression_since' (its meta.commit field). Null when 'regression_since' is null, when the historical report was written by a pre-V5 framework (no meta block), or when meta.commit was empty in that report."
        }
      }
    },
    "sanity_warning": {
      "title": "SanityWarning",
      "description": "One sanity-check finding. The framework runs a small set of hardcoded checks looking for suspicious uniformity in the metric data. Findings cannot be disabled and never affect the exit code, but are surfaced both here and on stderr at end-of-run so the operator notices.",
      "type": "object",
      "required": ["type", "metric", "message", "warning_since", "warning_since_commit"],
      "additionalProperties": false,
      "properties": {
        "type": {
          "type": "string",
          "minLength": 1,
          "description": "Sanity-check identifier. Currently the only emitted value is 'metric_looks_constant'; the schema admits any non-empty string so future check kinds can land without a schema change."
        },
        "metric": {
          "type": "string",
          "minLength": 1,
          "description": "Metric whose data tripped the check."
        },
        "message": {
          "type": "string",
          "minLength": 1,
          "description": "Human-readable explanation including, where applicable, the stringified offending value (e.g. 'always produces 0')."
        },
        "warning_since": {
          "type": ["string", "null"],
          "description": "Basename of the oldest report on disk that already exhibited this warning when joined with the current run's data (e.g. 'report_3.json'). Null when the history walk hit a missing report."
        },
        "warning_since_commit": {
          "type": ["string", "null"],
          "description": "meta.commit field of the historical report identified by 'warning_since'. Null when warning_since is null, when the older report was written by a pre-V5 framework (no meta block), or when meta.commit was empty in that report."
        }
      }
    }
  }
})json";

// ----------------- Validation utilities -----------------

/// Format a list of `ValidationDiag` to stderr. Returns true iff any
/// diagnostic has Severity::Error (caller can use this to drive the exit
/// code / abort flag).
[[nodiscard]] inline bool report_diagnostics(
    std::span<const ValidationDiag> diagnostics,
    std::string_view phase_label) noexcept
{
    if (diagnostics.empty()) return false;
    int error_count = 0;
    int warning_count = 0;
    for (const auto& diag : diagnostics) {
        if (diag.severity == ValidationDiag::Severity::Error) ++error_count;
        else ++warning_count;
    }

    std::fprintf(stderr,
        "============================================================\n"
        "BENCH FRAMEWORK — %.*s validation: %d error%s, %d warning%s\n"
        "============================================================\n",
        static_cast<int>(phase_label.size()), phase_label.data(),
        error_count,   error_count   == 1 ? "" : "s",
        warning_count, warning_count == 1 ? "" : "s");
    for (const auto& diag : diagnostics) {
        const char* severity_text =
            (diag.severity == ValidationDiag::Severity::Error) ? "ERROR" : "WARN ";
        std::fprintf(stderr, "  [%s] %s\n",
            severity_text, diag.code.c_str());
        std::fprintf(stderr, "          at: %s\n", diag.location.c_str());
        std::fprintf(stderr, "          %s\n",     diag.message.c_str());
    }
    std::fputs("\n", stderr);
    std::fflush(stderr);
    return error_count > 0;
}

/// Move every diagnostic from `source` into the shared `*context.diagnostics`
/// list under `context.diagnostics_mu`. Empty / nullptr inputs are no-ops
/// so the call site doesn't have to guard. The source vector is cleared
/// after the move so callers can reuse it without leaking stale entries.
inline void drain_diagnostics_to_context(
    BenchmarkContext& context,
    std::vector<ValidationDiag>& source) noexcept
{
    if (source.empty()
        || context.diagnostics == nullptr
        || context.diagnostics_mu == nullptr) return;
    std::lock_guard<std::mutex> guard(*context.diagnostics_mu);
    for (auto& diag : source) {
        context.diagnostics->push_back(std::move(diag));
    }
    source.clear();
}

/// Run a JSON document through one of the framework's hardcoded schemas
/// and convert any findings to `ValidationDiag` records. Catches thrown
/// `std::exception`s from the underlying nlohmann library so the framework
/// itself stays exception-free.
[[nodiscard]] inline std::vector<ValidationDiag> validate_against_schema(
    const std::string& json_text,
    std::string_view schema_text,
    std::string_view source_label) noexcept
{
    std::vector<ValidationDiag> diagnostics;
    try {
        const auto schema_document  = nlohmann::json::parse(schema_text);
        const auto target_document  = nlohmann::json::parse(json_text);
        nlohmann::json_schema::json_validator validator;
        validator.set_root_schema(schema_document);
        CollectingErrorHandler handler;
        validator.validate(target_document, handler);
        for (const auto& entry : handler.entries()) {
            // The validator's error callback delivers a JSON pointer to the
            // failing instance. Many of nlohmann_json_schema_validator's
            // composite errors (oneOf branch failures, propertyNames checks)
            // report the parent pointer and embed the inner path inside the
            // message text — so we always render the file label and append
            // the pointer only when it adds new information beyond the
            // message body.
            std::string location_text = std::string{source_label};
            if (!entry.pointer.empty()) {
                location_text += " (";
                location_text += entry.pointer;
                location_text += ')';
            }
            diagnostics.push_back({
                ValidationDiag::Severity::Error,
                "schema",
                std::move(location_text),
                entry.message
            });
        }
    } catch (const std::exception& exc) {
        diagnostics.push_back({
            ValidationDiag::Severity::Error,
            "schema_parse",
            std::string{source_label},
            exc.what()
        });
    }
    return diagnostics;
}

// `validate_against_schema` is the single entry point: callers pass
// either `kBenchConfigSchema` or `kBenchReportSchema` directly. The
// previous one-line wrappers were pure forwarders.

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

        // V11: optional `concurrency` integer (schema enforces minimum: 1).
        // Default is the cached `default_test_case_concurrency()`.
        std::int64_t concurrency_raw = 0;
        if (inner_obj["concurrency"].get(concurrency_raw) == simdjson::SUCCESS
            && concurrency_raw > 0) {
            spec.concurrency = static_cast<std::size_t>(concurrency_raw);
        } else {
            spec.concurrency = default_test_case_concurrency();
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

/// Parse the `compare` op and `tolerance%` slot shared by every
/// comparator object in the framework (per-rule and regression). Returns
/// nullopt + flips `parse_ok` on missing / unknown `compare`. `tolerance%`
/// defaults to 0 silently — the JSON schema enforces both bounds and
/// presence rules upstream, so omission means "use the default".
[[nodiscard]] inline std::optional<std::pair<CompareOp, double>>
parse_compare_and_tolerance(
    simdjson::dom::object op_obj,
    std::string_view diag_label,
    bool& parse_ok)
{
    std::string_view op_text;
    if (op_obj["compare"].get(op_text) != simdjson::SUCCESS) {
        loge("[bench_framework] {} missing 'compare'", diag_label);
        parse_ok = false;
        return std::nullopt;
    }
    const auto parsed_op = parse_compare_op(op_text);
    if (!parsed_op.has_value()) {
        loge("[bench_framework] {}: unknown compare op '{}'", diag_label, op_text);
        parse_ok = false;
        return std::nullopt;
    }
    double tolerance_pct = 0.0;
    (void)op_obj["tolerance%"].get(tolerance_pct);
    return std::pair{*parsed_op, tolerance_pct};
}

/// Parse a rule's `{ compare, threshold%, tolerance%, aggregate }`
/// quad. `compare` is mandatory; the two `%` fields default to zero;
/// `aggregate` defaults to `"p50"` (the legacy median behaviour for
/// pre-V5 rules). See `comparator_passes` for how the values turn into
/// a pass/fail decision and `aggregate_samples` for the reduction.
[[nodiscard]] inline RuleComparator parse_rule_comparator(
    simdjson::dom::object cmp_obj,
    bool& parse_ok)
{
    RuleComparator out;
    const auto compare_and_tolerance = parse_compare_and_tolerance(
        cmp_obj, "rule comparator", parse_ok);
    if (!compare_and_tolerance.has_value()) return out;
    out.op = compare_and_tolerance->first;
    out.tolerance_pct = compare_and_tolerance->second;
    double threshold_pct = 0.0;
    (void)cmp_obj["threshold%"].get(threshold_pct);
    out.threshold_pct = threshold_pct;

    std::string_view aggregate_text;
    if (cmp_obj["aggregate"].get(aggregate_text) == simdjson::SUCCESS) {
        const auto parsed = parse_aggregate(aggregate_text);
        if (!parsed.has_value()) {
            loge("[bench_framework] rule comparator: unknown aggregate '{}'",
                 aggregate_text);
            parse_ok = false;
        } else {
            out.aggregate = *parsed;
        }
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

/// Parse a regression rule's `operator` block. Shares the `compare` /
/// `tolerance%` parsing path with `parse_rule_comparator` via the
/// `parse_compare_and_tolerance` helper. The schema's
/// `additionalProperties: false` rejects any explicit `threshold%` field
/// before we get here, so we don't read it.
[[nodiscard]] inline std::optional<RegressionOperator> parse_regression_operator(
    simdjson::dom::object op_obj, bool& parse_ok)
{
    const auto compare_and_tolerance = parse_compare_and_tolerance(
        op_obj, "regression operator", parse_ok);
    if (!compare_and_tolerance.has_value()) return std::nullopt;
    return RegressionOperator{
        .op = compare_and_tolerance->first,
        .tolerance_pct = compare_and_tolerance->second,
    };
}

/// Parse the top-level `regression_rules` array. The result vector is
/// populated even when individual entries fail; callers report the
/// `parse_ok` flip as a hard error after walking the rest of the
/// document. `regions` is normalised here: the literal `"all_regions"`
/// shorthand and an array form both end up as a non-empty vector of
/// region names.
[[nodiscard]] inline std::vector<RegressionRule> parse_regression_rules(
    simdjson::dom::array rules_array,
    const std::string& config_path,
    bool& parse_ok)
{
    std::vector<RegressionRule> result;
    for (auto entry : rules_array) {
        simdjson::dom::object rule_obj;
        if (entry.get(rule_obj) != simdjson::SUCCESS) {
            loge("[bench_framework] regression_rules entry is not an object");
            parse_ok = false;
            continue;
        }

        RegressionRule rule;
        rule.config_path = config_path;

        std::string_view name_text;
        if (rule_obj["name"].get(name_text) != simdjson::SUCCESS) {
            loge("[bench_framework] regression rule missing 'name'");
            parse_ok = false;
            continue;
        }
        rule.name = std::string{name_text};

        simdjson::dom::array benchmarks_array;
        if (rule_obj["benchmarks"].get(benchmarks_array) == simdjson::SUCCESS) {
            for (auto bench_entry : benchmarks_array) {
                std::string_view bench_key;
                if (bench_entry.get(bench_key) != simdjson::SUCCESS) {
                    loge("[bench_framework] regression rule '{}' benchmarks[*] is not a string",
                         rule.name);
                    parse_ok = false;
                    continue;
                }
                rule.benchmarks.emplace_back(bench_key);
            }
        }

        simdjson::dom::element regions_element;
        if (rule_obj["regions"].get(regions_element) == simdjson::SUCCESS) {
            std::string_view single_region;
            simdjson::dom::array regions_array;
            if (regions_element.get(single_region) == simdjson::SUCCESS) {
                rule.regions.emplace_back(single_region);
            } else if (regions_element.get(regions_array) == simdjson::SUCCESS) {
                for (auto region_entry : regions_array) {
                    std::string_view region_name;
                    if (region_entry.get(region_name) != simdjson::SUCCESS) {
                        loge("[bench_framework] regression rule '{}' regions[*] is not a string",
                             rule.name);
                        parse_ok = false;
                        continue;
                    }
                    rule.regions.emplace_back(region_name);
                }
            } else {
                loge("[bench_framework] regression rule '{}' regions must be a string or array",
                     rule.name);
                parse_ok = false;
            }
        }
        if (rule.regions.empty()) {
            // Default per spec: "all_regions". Schema oneOf only emits the
            // array form when the user wrote one; the implicit default
            // (or omitted field) lands here.
            rule.regions.emplace_back("all_regions");
        }

        std::string_view metric_text;
        if (rule_obj["metric"].get(metric_text) != simdjson::SUCCESS) {
            loge("[bench_framework] regression rule '{}' missing 'metric'", rule.name);
            parse_ok = false;
            continue;
        }
        rule.metric = std::string{metric_text};

        std::string_view aggregate_text;
        if (rule_obj["aggregate"].get(aggregate_text) != simdjson::SUCCESS) {
            loge("[bench_framework] regression rule '{}' missing 'aggregate'", rule.name);
            parse_ok = false;
            continue;
        }
        const auto parsed_aggregate = parse_aggregate(aggregate_text);
        if (!parsed_aggregate.has_value()) {
            loge("[bench_framework] regression rule '{}' has unknown aggregate '{}'",
                 rule.name, aggregate_text);
            parse_ok = false;
            continue;
        }
        rule.aggregate = *parsed_aggregate;

        simdjson::dom::object op_obj;
        if (rule_obj["operator"].get(op_obj) != simdjson::SUCCESS) {
            loge("[bench_framework] regression rule '{}' missing 'operator'", rule.name);
            parse_ok = false;
            continue;
        }
        const auto parsed_op = parse_regression_operator(op_obj, parse_ok);
        if (!parsed_op.has_value()) continue;
        rule.op = *parsed_op;

        simdjson::dom::array exclude_array;
        if (rule_obj["exclude_cases"].get(exclude_array) == simdjson::SUCCESS) {
            for (auto exclude_entry : exclude_array) {
                std::string_view exclude_name;
                if (exclude_entry.get(exclude_name) != simdjson::SUCCESS) {
                    loge("[bench_framework] regression rule '{}' exclude_cases[*] is not a string",
                         rule.name);
                    parse_ok = false;
                    continue;
                }
                rule.exclude_cases.emplace_back(exclude_name);
            }
        }

        result.push_back(std::move(rule));
    }
    return result;
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

        std::string_view type_text = "builtin";
        std::string_view declared_type;
        if (inner_obj["type"].get(declared_type) == simdjson::SUCCESS) {
            type_text = declared_type;
        }
        const auto parsed_source = parse_metric_type(type_text);
        if (!parsed_source.has_value()) {
            loge("[bench_framework] metrics.{}: unknown type '{}'", field.key, type_text);
            parse_ok = false;
            continue;
        }
        metric.declared_source = *parsed_source;

        // Map (source, metric_name) → MetricKind. Counter and Evaluation
        // bypass the registry — their MetricKind is fixed. Builtin /
        // Region / Loop look up the canonical entry; mismatches between
        // the declared `type` and the entry's source are deferred to
        // semantic_validate_config so all errors collect in one pass.
        switch (*parsed_source) {
            case MetricSource::Counter:
                metric.kind = MetricKind::Counter;
                break;
            case MetricSource::Evaluation:
                metric.kind = MetricKind::Evaluation;
                break;
            case MetricSource::OuterClock:
            case MetricSource::Region:
            case MetricSource::Loop: {
                const auto* entry = lookup_builtin_metric(metric.name);
                if (entry != nullptr) {
                    metric.kind = entry->kind;
                } else {
                    // Unknown name — fall back to a placeholder that
                    // semantic_validate_config rejects with a precise
                    // message based on the declared `type`.
                    metric.kind = MetricKind::WallClockNs;
                }
                break;
            }
        }

        std::string_view counter_name;
        if (inner_obj["counter"].get(counter_name) == simdjson::SUCCESS) {
            metric.counter_name = std::string{counter_name};
        } else {
            metric.counter_name = metric.name;
        }
        std::string_view region_filter_text;
        if (inner_obj["region"].get(region_filter_text) == simdjson::SUCCESS) {
            metric.region = std::string{region_filter_text};
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
        // V11: `test_case_lock` is gone. Schema's
        // `additionalProperties: false` rejects the key, so older
        // configs surface as validation errors.
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
/// Outcome of `parse_config_file`. `ok=true` means the parser walked the
/// document; `diagnostics` carries every issue found (schema violations
/// from the JSON-schema pass plus structural problems from the simdjson
/// walk). On parse failure `ok=false` and the caller must abort *before*
/// reading anything else from the state — the partial parse is undefined.
struct ConfigParseOutcome {
    bool ok = true;
    std::vector<ValidationDiag> diagnostics{};
    /// Top-level `regression_rules` array, parsed and normalised. The
    /// closure registers these into the global `RegressionRulesByConfig`
    /// registry the first time it sees a given `config_path` (subsequent
    /// benchmarks sharing the file see the entry already present and
    /// skip).
    std::vector<RegressionRule> regression_rules{};
};

[[nodiscard]] inline ConfigParseOutcome parse_config_file(
    const std::string& config_path,
    const std::string& benchmark_key,
    BenchmarkExecutionState& state)
{
    ConfigParseOutcome outcome;
    state.config.config_path   = config_path;
    state.config.benchmark_key = benchmark_key;
    state.config_body = slurp_file(config_path);
    if (state.config_body.empty()) {
        outcome.ok = false;
        outcome.diagnostics.push_back({
            ValidationDiag::Severity::Error,
            "config_unreadable",
            config_path,
            "config file is empty or unreadable"
        });
        return outcome;
    }

    // 1. JSON schema pass — validates structure (required fields, types,
    //    ranges, propertyNames, oneOf branches). Schema errors do not stop
    //    the walk: we still try to populate as much of the state as
    //    possible so semantic_validate_config can layer further checks
    //    on top.
    auto schema_diagnostics = validate_against_schema(
        state.config_body, kBenchConfigSchema, config_path);
    for (auto& diag : schema_diagnostics) {
        if (diag.severity == ValidationDiag::Severity::Error) outcome.ok = false;
        outcome.diagnostics.push_back(std::move(diag));
    }

    // 2. simdjson walk. If the top-level structure is unrecognisable we
    //    bail; less catastrophic problems (missing benchmark key, etc.)
    //    surface as additional diagnostics so the user sees one
    //    consolidated report.
    auto root_result = state.parser.parse(state.config_body);
    simdjson::dom::element root_element;
    if (root_result.get(root_element) != simdjson::SUCCESS) {
        outcome.ok = false;
        outcome.diagnostics.push_back({
            ValidationDiag::Severity::Error,
            "simdjson_parse",
            config_path,
            "simdjson failed to parse the config file"
        });
        return outcome;
    }

    simdjson::dom::object root_obj;
    if (root_element.get(root_obj) != simdjson::SUCCESS) {
        outcome.ok = false;
        outcome.diagnostics.push_back({
            ValidationDiag::Severity::Error,
            "not_object",
            config_path,
            "top-level value is not a JSON object"
        });
        return outcome;
    }

    bool parse_ok = true;

    simdjson::dom::object params_obj;
    if (root_obj["params"].get(params_obj) == simdjson::SUCCESS) {
        state.config.algorithm_schemas = parse_top_level_params(params_obj, parse_ok);
    } else {
        outcome.ok = false;
        outcome.diagnostics.push_back({
            ValidationDiag::Severity::Error,
            "missing_params",
            config_path,
            "config has no top-level 'params' object"
        });
    }

    simdjson::dom::object test_cases_obj;
    if (root_obj["test_cases"].get(test_cases_obj) == simdjson::SUCCESS) {
        state.test_cases = parse_test_cases(test_cases_obj, parse_ok);
    } else {
        outcome.ok = false;
        outcome.diagnostics.push_back({
            ValidationDiag::Severity::Error,
            "missing_test_cases",
            config_path,
            "config has no top-level 'test_cases' object"
        });
    }

    simdjson::dom::element benchmark_element;
    if (root_obj[benchmark_key].get(benchmark_element) != simdjson::SUCCESS) {
        outcome.ok = false;
        outcome.diagnostics.push_back({
            ValidationDiag::Severity::Error,
            "missing_benchmark_block",
            std::string{config_path} + ":" + benchmark_key,
            "config has no top-level key for the requested benchmark"
        });
        return outcome;
    }
    simdjson::dom::object benchmark_obj;
    if (benchmark_element.get(benchmark_obj) != simdjson::SUCCESS) {
        outcome.ok = false;
        outcome.diagnostics.push_back({
            ValidationDiag::Severity::Error,
            "benchmark_not_object",
            std::string{config_path} + ":" + benchmark_key,
            "benchmark block is not a JSON object"
        });
        return outcome;
    }
    if (!parse_benchmark_block(benchmark_obj, state.config)) {
        parse_ok = false;
    }

    // 3. Optional top-level regression_rules array. Empty / absent are
    //    both legal. Parse here so we always return a fresh copy in
    //    `outcome.regression_rules`; the closure decides whether to
    //    register them (only the first benchmark per config_path
    //    succeeds; subsequent ones discard their copy via dedup).
    simdjson::dom::array regression_rules_array;
    if (root_obj["regression_rules"].get(regression_rules_array) == simdjson::SUCCESS) {
        outcome.regression_rules = parse_regression_rules(
            regression_rules_array, config_path, parse_ok);
    }

    if (!parse_ok) outcome.ok = false;
    return outcome;
}

// ----------------- Semantic config validation -----------------
//
// Once the JSON schema and the simdjson walk have populated
// `BenchmarkExecutionState`, the framework runs cross-field checks the
// schema cannot express:
//   - matrix.over.<algorithm> references an algorithm declared in 'params'
//   - matrix.over.<algorithm>.<parameter> references a declared parameter
//   - every value in matrix.over.<algorithm>.<parameter> matches the
//     parameter's declared type (and 'enum', if present)
//   - matrix.for.<test_case> references a declared test case
//   - rule selectors point at a declared algorithm and only mention
//     declared parameters with type-correct values
//   - rule names are unique within a benchmark
//   - threshold% / tolerance% of exactly 100 raises a warning ("rule will
//     always pass")
//   - test_cases declared with zero iterations raise a warning

/// True iff `value` matches the declared `param`. Type promotion: a
/// `number`-typed parameter accepts both Integer and Double values.
[[nodiscard]] inline bool value_matches_param_schema(
    const ConfigValue& value, const ParamSchema& param) noexcept
{
    bool type_ok = false;
    if      (param.json_type == "string"  && value.is_string())  type_ok = true;
    else if (param.json_type == "integer" && value.is_integer()) type_ok = true;
    else if (param.json_type == "number"  && (value.is_double() || value.is_integer())) type_ok = true;
    else if (param.json_type == "boolean" && value.is_boolean()) type_ok = true;
    if (!type_ok) return false;
    if (param.enum_values.empty()) return true;
    for (const auto& allowed : param.enum_values) {
        if (allowed == value) return true;
    }
    return false;
}

/// Look up one parameter's schema inside an algorithm's parameter list.
/// Returns nullptr if either the algorithm or the parameter is unknown.
[[nodiscard]] inline const ParamSchema* find_param_schema(
    const std::map<std::string, AlgorithmSchema>& schemas,
    const std::string& algorithm_name,
    const std::string& param_name) noexcept
{
    const auto algo_it = schemas.find(algorithm_name);
    if (algo_it == schemas.end()) return nullptr;
    for (const auto& param : algo_it->second.params) {
        if (param.param_name == param_name) return &param;
    }
    return nullptr;
}

/// Helper for building human-readable diagnostic locations. Returns
/// `<config_path>:<benchmark_key>/<sub_path>`.
[[nodiscard]] inline std::string semantic_location(
    const BenchmarkExecutionState& state, std::string_view sub_path) noexcept
{
    std::string out = state.config.config_path;
    out += ':';
    out += state.config.benchmark_key;
    out += '/';
    out += sub_path;
    return out;
}

/// Render a ConfigValue's *kind* as a JSON-Schema-style type name, used by
/// `value_type_mismatch` diagnostics so the user sees what was passed vs.
/// what was declared.
[[nodiscard]] inline std::string_view config_value_kind_name(const ConfigValue& value) noexcept {
    switch (value.kind) {
        case ConfigValue::Kind::String:    return "string";
        case ConfigValue::Kind::Integer:   return "integer";
        case ConfigValue::Kind::Double:    return "number";
        case ConfigValue::Kind::Boolean:   return "boolean";
        case ConfigValue::Kind::NonScalar: return "non-scalar (object/array/null)";
    }
    return "?";
}

/// Walk every cross-reference inside a parsed `BenchmarkExecutionState`
/// and emit `ValidationDiag` records for anything inconsistent. Returns a
/// flat vector covering every benchmark-level concern (no early-exit so
/// the user sees every issue in one pass).
[[nodiscard]] inline std::vector<ValidationDiag> semantic_validate_config(
    const BenchmarkExecutionState& state) noexcept
{
    std::vector<ValidationDiag> diagnostics;
    const auto& config  = state.config;
    const auto& schemas = config.algorithm_schemas;

    auto emit = [&](ValidationDiag::Severity severity,
                    std::string code,
                    std::string sub_path,
                    std::string message) {
        diagnostics.push_back({
            severity,
            std::move(code),
            semantic_location(state, sub_path),
            std::move(message),
        });
    };
    auto error   = [&](std::string code, std::string sub_path, std::string message) {
        emit(ValidationDiag::Severity::Error,   std::move(code),
             std::move(sub_path), std::move(message));
    };
    auto warning = [&](std::string code, std::string sub_path, std::string message) {
        emit(ValidationDiag::Severity::Warning, std::move(code),
             std::move(sub_path), std::move(message));
    };

    // 1. Cross-check `matrix.over.<algo>.<param> = [values]` against
    //    `params.<algo>.<param>`. Every algorithm must be declared, every
    //    parameter must be declared, every value must be type-correct.
    for (const auto& [algorithm_name, params_axes] : config.matrix_over) {
        if (schemas.find(algorithm_name) == schemas.end()) {
            error("unknown_algorithm",
                  "matrix.over." + algorithm_name,
                  fmt::format("algorithm '{}' is not declared in top-level 'params'",
                              algorithm_name));
            continue;
        }
        for (const auto& [param_name, value_list] : params_axes) {
            const auto* param_schema = find_param_schema(schemas, algorithm_name, param_name);
            if (param_schema == nullptr) {
                error("unknown_parameter",
                      fmt::format("matrix.over.{}.{}", algorithm_name, param_name),
                      fmt::format("parameter '{}' is not declared in 'params.{}'",
                                  param_name, algorithm_name));
                continue;
            }
            for (std::size_t i = 0; i < value_list.size(); ++i) {
                if (!value_matches_param_schema(value_list[i], *param_schema)) {
                    error("value_type_mismatch",
                          fmt::format("matrix.over.{}.{}[{}]",
                                      algorithm_name, param_name, i),
                          fmt::format("value of type '{}' does not match declared type '{}'"
                                      "{} for parameter '{}.{}'",
                                      config_value_kind_name(value_list[i]),
                                      param_schema->json_type,
                                      param_schema->enum_values.empty()
                                        ? std::string{}
                                        : std::string{" (or is outside the declared 'enum')"},
                                      algorithm_name, param_name));
                }
            }
        }
    }

    // 2. Cross-check `matrix.for.<test_case>` against top-level
    //    `test_cases`, and warn on zero iterations.
    std::set<std::string> declared_test_cases;
    for (const auto& spec : state.test_cases) declared_test_cases.insert(spec.name);
    for (const auto& [test_case_name, iterations] : config.for_each) {
        if (declared_test_cases.find(test_case_name) == declared_test_cases.end()) {
            error("unknown_test_case",
                  "matrix.for." + test_case_name,
                  fmt::format("test_case '{}' is not declared in top-level 'test_cases'",
                              test_case_name));
        }
        if (iterations == 0) {
            warning("zero_iterations",
                    "matrix.for." + test_case_name,
                    fmt::format("test_case '{}' has 0 iterations and will not run",
                                test_case_name));
        }
    }

    // 3. Walk every rule. Selector validation, rule name uniqueness,
    //    threshold/tolerance always-pass warnings.
    std::set<std::string> seen_rule_names;
    auto validate_selector = [&](const RuleSelector& selector,
                                  std::string_view rule_name,
                                  std::string_view metric_name,
                                  std::string_view side) {
        const auto schema_it = schemas.find(selector.algorithm_key);
        const std::string base_loc = fmt::format(
            "matrix.metrics.{}.rules.{}.{}", metric_name, rule_name, side);
        if (schema_it == schemas.end()) {
            error("unknown_algorithm",
                  base_loc + ".key",
                  fmt::format("rule selector references algorithm '{}' which is not declared in 'params'",
                              selector.algorithm_key));
            return;
        }
        for (const auto& [match_param, match_value] : selector.match) {
            const auto* param_schema = find_param_schema(
                schemas, selector.algorithm_key, match_param);
            if (param_schema == nullptr) {
                error("unknown_parameter",
                      fmt::format("{}.{}", base_loc, match_param),
                      fmt::format("rule selector for algorithm '{}' constrains unknown parameter '{}'",
                                  selector.algorithm_key, match_param));
                continue;
            }
            if (!value_matches_param_schema(match_value, *param_schema)) {
                error("value_type_mismatch",
                      fmt::format("{}.{}", base_loc, match_param),
                      fmt::format("rule selector value for '{}.{}' is of type '{}' but the parameter is declared as '{}'",
                                  selector.algorithm_key, match_param,
                                  config_value_kind_name(match_value),
                                  param_schema->json_type));
            }
        }
    };

    for (const auto& metric : config.metrics) {
        // 4a. Builtin metric name + source compatibility. Counter and
        //     Evaluation metrics carry user-chosen names; Region / Loop /
        //     OuterClock metric keys must match a registry entry whose
        //     source matches the declared `type`.
        // (Counter / Evaluation flavours skip these checks — their metric
        // names are user-chosen and the schema's `additionalProperties:
        // false` already rejects a `region` field on those branches.)
        if (metric.kind != MetricKind::Counter && metric.kind != MetricKind::Evaluation) {
            const auto* entry = lookup_builtin_metric(metric.name);
            if (entry == nullptr) {
                error("invalid_builtin_metric_name",
                      fmt::format("matrix.metrics.{}", metric.name),
                      fmt::format("metric '{}' is not a recognised framework builtin; either rename it to a registered builtin (e.g. wall_clock_ns, region_total_ns, region_visits, region_p99_ns, loop_total_iterations, loop_p50_per_iter_ns; see the kBuiltinMetricRegistry table) or declare 'type' as 'counter' / 'evaluation'",
                                  metric.name));
                continue;
            }
            // 4b. Cross-source typo check. `type: "region"` with key
            //     `loop_total_iterations` (or vice versa) gets rejected
            //     before run-time so the user sees a precise message
            //     instead of getting silently-empty samples.
            if (entry->source != metric.declared_source) {
                error("metric_source_mismatch",
                      fmt::format("matrix.metrics.{}", metric.name),
                      fmt::format("metric '{}' is declared as 'type: {}' but the registry knows it as a '{}'-source metric; either change 'type' or rename the metric",
                                  metric.name,
                                  metric_source_name(metric.declared_source),
                                  metric_source_name(entry->source)));
                continue;
            }
            // 4c. Min / Max metrics require an explicit `region` filter —
            //     cross-region "min of mins" / "max of maxes" would lose
            //     meaning.
            if (entry->combiner == MetricCombiner::RequiresRegion && metric.region.empty()) {
                error("min_max_metric_requires_region",
                      fmt::format("matrix.metrics.{}", metric.name),
                      fmt::format("metric '{}' aggregates min/max across descriptors and is meaningless without a 'region' filter; add 'region': '<descriptor_name>' to the metric block",
                                  metric.name));
            }
        }

        for (const auto& rule : metric.rules) {
            if (!seen_rule_names.insert(rule.name).second) {
                error("duplicate_rule_name",
                      fmt::format("matrix.metrics.{}.rules.{}", metric.name, rule.name),
                      fmt::format("rule name '{}' is used more than once in this benchmark; rule names must be unique",
                                  rule.name));
            }
            validate_selector(rule.lhs, rule.name, metric.name, "lhs");
            validate_selector(rule.rhs, rule.name, metric.name, "rhs");

            // Always-pass warnings.
            if (rule.cmp.threshold_pct == 100.0) {
                warning("threshold_max",
                        fmt::format("matrix.metrics.{}.rules.{}.comparator", metric.name, rule.name),
                        "threshold% is 100, the rule will always pass");
            }
            if (rule.cmp.tolerance_pct == 100.0) {
                warning("tolerance_max",
                        fmt::format("matrix.metrics.{}.rules.{}.comparator", metric.name, rule.name),
                        "tolerance% is 100, the rule will always pass");
            }
        }
    }

    return diagnostics;
}

/// Post-run validation. The only check here is "every declared
/// `type: evaluation` metric was emitted at least once by the user's
/// `on_evaluate` handler". We can only see this after the iteration loop
/// finished; missing keys are an error (the evaluation handler is the
/// authoritative source for evaluation-typed metrics).
[[nodiscard]] inline std::vector<ValidationDiag> semantic_validate_post_run(
    const BenchmarkExecutionState& state) noexcept
{
    std::vector<ValidationDiag> diagnostics;

    // Single-pass walk: invoke `body(samples)` for every cell that has
    // collected `metric_name` in `all_regions`, and stop early when
    // `body` returns true. Replaces three near-identical
    // for-loops-over-cells with a single shared traversal.
    const auto for_each_all_regions_metric = [&](
        const std::string& metric_name,
        auto&& body) noexcept -> bool
    {
        for (const auto& cell_ptr : state.cells) {
            const auto region_it = cell_ptr->samples.find("all_regions");
            if (region_it == cell_ptr->samples.end()) continue;
            const auto values_it = region_it->second.find(metric_name);
            if (values_it == region_it->second.end()) continue;
            if (body(values_it->second)) return true;
        }
        return false;
    };

    // If no cell ran (every test_case had 0 iterations, or no test_cases were
    // declared at all), the missing-metric check would false-positive on
    // every evaluation metric. Skip the check entirely and emit a single
    // warning so the situation is still surfaced.
    bool any_iteration_ran = false;
    for (const auto& cell_ptr : state.cells) {
        if (!cell_ptr->samples.empty()) { any_iteration_ran = true; break; }
    }
    if (!any_iteration_ran) {
        diagnostics.push_back({
            ValidationDiag::Severity::Warning,
            "benchmark_did_not_run",
            semantic_location(state, "matrix"),
            "benchmark did not execute any iteration (matrix.for entries either name unknown test cases or set iterations to 0); evaluation-metric coverage check skipped"
        });
        return diagnostics;
    }

    // Helper: did any cell emit a sample for this metric?
    const auto metric_was_emitted = [&](const std::string& metric_name) noexcept {
        return for_each_all_regions_metric(metric_name,
            [](const auto& /*values*/) noexcept { return true; });
    };
    // Helper: are any of a metric's collected samples non-zero? Used by
    // the PMU-availability check below — on platforms without PMU access,
    // the OS feeds back zeros for cycles/instructions/branches/branch_misses
    // and we want to surface that exactly once.
    const auto metric_has_non_zero_sample = [&](const std::string& metric_name) noexcept {
        return for_each_all_regions_metric(metric_name,
            [](const auto& values) noexcept {
                for (double sample : values) {
                    if (sample != 0.0) return true;
                }
                return false;
            });
    };

    // PMU availability: if any declared region metric needs PMU, but every
    // sample we collected for it is exactly zero, the OS denied PMU access
    // (typical on macOS arm64 without root). Emit one warning per benchmark
    // so the user understands why the values are degenerate.
    bool pmu_warning_emitted = false;
    for (const auto& metric : state.config.metrics) {
        if (metric.kind == MetricKind::Counter || metric.kind == MetricKind::Evaluation) continue;
        const auto* entry = lookup_builtin_metric(metric.name);
        if (entry == nullptr || !entry->needs_pmu) continue;
        if (metric_was_emitted(metric.name) && !metric_has_non_zero_sample(metric.name)) {
            if (!pmu_warning_emitted) {
                diagnostics.push_back({
                    ValidationDiag::Severity::Warning,
                    "pmu_unavailable",
                    semantic_location(state, "matrix.metrics"),
                    "PMU counters were unavailable on this platform — every sample of region_total_cycles / region_total_instructions / region_total_branches / region_total_branch_misses / region_ipc / region_branch_miss_rate read as zero. On macOS arm64 the kpc interface requires entitlements; on Linux the perf_event_open syscall may be sandbox-restricted. Re-run with the appropriate privileges or remove the PMU-derived metrics from this benchmark."
                });
                pmu_warning_emitted = true;
            }
        }
    }

    for (const auto& metric : state.config.metrics) {

        if (metric.kind == MetricKind::Evaluation) {
            if (!metric_was_emitted(metric.name)) {
                diagnostics.push_back({
                    ValidationDiag::Severity::Error,
                    "missing_evaluation_metric",
                    semantic_location(state,
                        fmt::format("matrix.metrics.{}", metric.name)),
                    fmt::format("metric '{}' is declared as 'type: evaluation' but the on_evaluate handler never returned this key for any iteration",
                                metric.name)
                });
            }
            continue;
        }

        // For region/loop metrics with a non-empty `region` filter, verify
        // that the named descriptor was actually visited at least once.
        // Without this check, a typo in the filter silently produces empty
        // sample arrays and rules over them quietly compare median(0) ==
        // median(0).
        if (!metric.region.empty() && !metric_was_emitted(metric.name)) {
            diagnostics.push_back({
                ValidationDiag::Severity::Warning,
                "region_filter_never_matched",
                semantic_location(state,
                    fmt::format("matrix.metrics.{}", metric.name)),
                fmt::format("metric '{}' restricts to region '{}', but no descriptor with that name was visited during the benchmark; rules over this metric will see an empty sample array",
                            metric.name, metric.region)
            });
        }
    }

    // V7 note: returned-but-undeclared evaluation keys are dropped
    // silently. The on_evaluate lambda is typically shared across
    // multiple benchmarks (one returns `accuracy` and `latency`,
    // another only declares `accuracy`); flagging extra keys would
    // make sharing painful. The opposite direction — declared but
    // not returned — is still flagged via `missing_evaluation_metric`
    // above because that case is almost always a typo.

    return diagnostics;
}

/// Cross-config validation for regression rules. Runs once from
/// `run_impl` after every benchmark closure has finished registering its
/// state. Each rule is checked against the benchmarks loaded from its
/// own `config_path`:
///   - Rule names must be unique within the same config file.
///   - Every entry of `benchmarks` (when present) must name a benchmark
///     that actually loaded from this config_path.
///   - The metric must appear in the benchmarks the rule applies to
///     (every listed benchmark when explicit; at least one when
///     implicit).
///   - Every entry of `exclude_cases` must name a declared test_case in
///     the rule's config (test_cases are config-global, so any state
///     sharing the config_path has the same list).
///   - `exclude_cases` cannot cover *every* test_case the rule could
///     possibly fire on (over-coverage). The runtime
///     `regression_rule_matched_nothing` check still catches semantic
///     emptiness (e.g. region listed but never visited); this catches
///     the syntactic case at parse time.
[[nodiscard]] inline std::vector<ValidationDiag> semantic_validate_regression_rules(
    const RegressionRulesByConfig& regression_rules_by_config,
    const std::map<std::string, std::unique_ptr<BenchmarkExecutionState>>& reports) noexcept
{
    std::vector<ValidationDiag> diagnostics;
    auto error = [&](std::string code, std::string location, std::string message) {
        diagnostics.push_back({
            ValidationDiag::Severity::Error,
            std::move(code), std::move(location), std::move(message)
        });
    };

    for (const auto& [config_path, rules] : regression_rules_by_config) {
        // Collect the benchmark states loaded from this config_path.
        std::vector<const BenchmarkExecutionState*> states_for_config;
        for (const auto& [bench_key, state_ptr] : reports) {
            if (state_ptr->config.config_path == config_path) {
                states_for_config.push_back(state_ptr.get());
            }
        }

        std::set<std::string> seen_rule_names;
        for (const auto& rule : rules) {
            const std::string base_loc = fmt::format(
                "{}/regression_rules/{}", config_path, rule.name);

            // 1. Rule name uniqueness within the config file.
            if (!seen_rule_names.insert(rule.name).second) {
                error("regression_duplicate_rule_name", base_loc,
                    fmt::format("regression rule name '{}' is used more than once in '{}'; names must be unique within a config file",
                                rule.name, config_path));
            }

            // 2. Determine matching benchmarks within this config_path.
            std::vector<const BenchmarkExecutionState*> matching_states;
            if (rule.benchmarks.empty()) {
                matching_states = states_for_config;
            } else {
                for (const auto& declared_bench : rule.benchmarks) {
                    bool found = false;
                    for (const auto* state : states_for_config) {
                        if (state->config.benchmark_key == declared_bench) {
                            matching_states.push_back(state);
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        error("regression_unknown_benchmark", base_loc,
                            fmt::format("benchmark '{}' is not declared in '{}'",
                                        declared_bench, config_path));
                    }
                }
            }

            // 3. Metric existence check.
            const auto metric_in_state = [&](const BenchmarkExecutionState& state) noexcept {
                for (const auto& metric : state.config.metrics) {
                    if (metric.name == rule.metric) return true;
                }
                return false;
            };
            if (!rule.benchmarks.empty()) {
                for (const auto* state : matching_states) {
                    if (!metric_in_state(*state)) {
                        error("regression_metric_missing_in_benchmark", base_loc,
                            fmt::format("regression rule '{}' targets metric '{}' but benchmark '{}' (explicitly listed in 'benchmarks') does not declare it under matrix.metrics",
                                        rule.name, rule.metric, state->config.benchmark_key));
                    }
                }
            } else if (!matching_states.empty()) {
                bool found_any = false;
                for (const auto* state : matching_states) {
                    if (metric_in_state(*state)) { found_any = true; break; }
                }
                if (!found_any) {
                    error("regression_metric_unused", base_loc,
                        fmt::format("regression rule '{}' (no 'benchmarks' filter) targets metric '{}' but no benchmark loaded from '{}' declares it",
                                    rule.name, rule.metric, config_path));
                }
            }

            // 4. exclude_cases membership check (and over-coverage).
            //    test_cases are config-global, so any state sharing the
            //    config_path has the full list — pick one and use its
            //    declared set.
            if (!matching_states.empty()) {
                const auto& declared_test_cases = matching_states.front()->test_cases;
                std::set<std::string> declared_names;
                for (const auto& spec : declared_test_cases) declared_names.insert(spec.name);
                for (const auto& excluded : rule.exclude_cases) {
                    if (declared_names.find(excluded) == declared_names.end()) {
                        error("regression_unknown_test_case", base_loc,
                            fmt::format("exclude_cases entry '{}' is not declared in 'test_cases'",
                                        excluded));
                    }
                }

                // Over-coverage: union of every for_each in matching
                // benchmarks minus exclude_cases must be non-empty.
                std::set<std::string> covered_test_cases;
                for (const auto* state : matching_states) {
                    for (const auto& [test_case_name, _iters] : state->config.for_each) {
                        covered_test_cases.insert(test_case_name);
                    }
                }
                std::set<std::string> excluded_set(
                    rule.exclude_cases.begin(), rule.exclude_cases.end());
                bool any_remains = false;
                for (const auto& tc : covered_test_cases) {
                    if (excluded_set.find(tc) == excluded_set.end()) {
                        any_remains = true; break;
                    }
                }
                if (!any_remains && !covered_test_cases.empty()) {
                    error("regression_excludes_all", base_loc,
                        fmt::format("regression rule '{}' excludes every test_case its matching benchmarks would run on; the rule can never fire (use a narrower 'exclude_cases' or remove it)",
                                    rule.name));
                }
            }
        }
    }

    return diagnostics;
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
        // Invariant: every algorithm in `matrix_over` was declared in
        // `params` — `semantic_validate_config` rejects with
        // `unknown_algorithm` long before we get here, and the
        // closure aborts on semantic-validation errors before invoking
        // `expand_matrix`. The assert documents that contract; if it
        // ever fires, the bug is upstream (a new code path bypassed
        // semantic validation).
        DEBUG_ASSERT(schema_it != config.algorithm_schemas.end(),
            "matrix.over names an algorithm with no schema in params; "
            "semantic_validate_config should have rejected this");
        auto expanded = expand_one_algorithm(algorithm_name, declared, schema_it->second,
                                              expansion_ok);
        for (auto& expansion_tuple : expanded) {
            result.push_back(std::move(expansion_tuple));
        }
    }
    return result;
}

// ----------------- Cell construction -----------------

/// Distribute `iterations` across `groups` groups, minimising max-min.
/// Returns a vector of size `min(groups, iterations)` summing to
/// `iterations`. Empty when either input is zero. Examples:
///   split_iterations(32, 5) == {6, 6, 6, 7, 7}      // remainder 2 → last 2 entries +1
///   split_iterations( 8, 1) == {8}                  // single group keeps everything
///   split_iterations(19, 20) == 19 ones             // capped at iterations
///   split_iterations( 0, 5) == {}                   // empty
[[nodiscard]] inline std::vector<std::size_t>
split_iterations(std::size_t iterations, std::size_t groups) {
    if (iterations == 0 || groups == 0) return {};
    const std::size_t effective_groups = std::min(groups, iterations);
    std::vector<std::size_t> out(effective_groups, iterations / effective_groups);
    const std::size_t remainder = iterations % effective_groups;
    for (std::size_t bonus_index = effective_groups - remainder;
         bonus_index < effective_groups; ++bonus_index) {
        out[bonus_index] += 1;
    }
    return out;
}

/// Compute the actual number of groups for one test_case in this
/// benchmark. Caps `test_case.concurrency` at the maximum iteration
/// count across all tuples for this test_case (a group that would have
/// 0 iterations is meaningless, so we never create one).
[[nodiscard]] inline std::size_t group_count_for_test_case(
    const BenchmarkConfig& config,
    const TestCaseSpec&    test_case_spec,
    std::size_t            iterations_per_tuple) noexcept
{
    if (iterations_per_tuple == 0) return 0;
    const std::size_t requested = test_case_spec.concurrency == 0
        ? default_test_case_concurrency()
        : test_case_spec.concurrency;
    const std::size_t cap_by_iterations = iterations_per_tuple;
    return std::min(requested, cap_by_iterations);
}

/// Construct the (test_case × group × execution_tuple) cell table. Each
/// cell holds one group's slice of iterations for one tuple. Groups
/// are merged by the report writer / rule evaluator so the
/// user-visible report still has one entry per (test_case, tuple).
[[nodiscard]] inline std::vector<std::unique_ptr<Cell>> build_cells(
    const BenchmarkExecutionState& state)
{
    std::vector<std::unique_ptr<Cell>> cells;
    // Rough upper bound: assume default concurrency for every (tc × tuple).
    cells.reserve(state.config.for_each.size() * state.execution_tuples.size()
                  * default_test_case_concurrency());

    // Build a name → index lookup once so the per-`for_each` resolution
    // is O(log N) instead of O(N) per entry.
    std::map<std::string_view, std::size_t> test_case_by_name;
    for (std::size_t test_case_index = 0;
         test_case_index < state.test_cases.size(); ++test_case_index)
    {
        test_case_by_name.emplace(
            std::string_view{state.test_cases[test_case_index].name},
            test_case_index);
    }

    for (const auto& [test_case_name, iterations] : state.config.for_each) {
        const auto found_it = test_case_by_name.find(
            std::string_view{test_case_name});
        if (found_it == test_case_by_name.end()) {
            loge("[bench_framework] test_case '{}' referenced by matrix.for but not declared",
                         test_case_name);
            continue;
        }
        const std::size_t test_case_index = found_it->second;
        const auto& test_case_spec = state.test_cases[test_case_index];
        const std::size_t group_count = group_count_for_test_case(
            state.config, test_case_spec, iterations);

        if (group_count == 0) {
            // Zero-iteration test case — emit no cells so the writer's
            // "skip empty samples" path treats it as absent (matches V10
            // behaviour, which also produced no samples for these).
            continue;
        }

        const auto split = split_iterations(iterations, group_count);
        for (const auto& tuple : state.execution_tuples) {
            for (std::size_t group_index = 0; group_index < split.size(); ++group_index) {
                auto cell = std::make_unique<Cell>();
                cell->test_case        = &state.test_cases[test_case_index];
                cell->test_case_index  = test_case_index;
                cell->tuple            = &tuple;
                cell->iterations       = split[group_index];
                cell->group_index      = group_index;
                cell->group_count      = split.size();
                cells.push_back(std::move(cell));
            }
        }
    }
    return cells;
}

// ----------------- Sample collection -----------------

/// Per-descriptor scalar extraction for a region-source metric. Returns
/// the value in the metric's natural unit (uint64 cast to double for sums,
/// uint64 for visits, etc.). Min/Max return the field directly even though
/// they require an explicit `region` filter — the caller's combiner handles
/// the cross-descriptor logic.
[[nodiscard]] inline double extract_from_region(
    MetricKind kind,
    const bioseq::instr::RegionAggregate& region_agg) noexcept
{
    using bioseq::instr::access::hist_percentile;
    switch (kind) {
        case MetricKind::RegionTotalNs:           return static_cast<double>(region_agg.total_ns);
        case MetricKind::RegionVisits:            return static_cast<double>(region_agg.visits);
        case MetricKind::RegionMinNs:             return static_cast<double>(region_agg.min_ns);
        case MetricKind::RegionMaxNs:             return static_cast<double>(region_agg.max_ns);
        case MetricKind::RegionP50Ns:             return static_cast<double>(hist_percentile(region_agg.ns_hist, 0.50));
        case MetricKind::RegionP99Ns:             return static_cast<double>(hist_percentile(region_agg.ns_hist, 0.99));
        case MetricKind::RegionTotalCycles:       return static_cast<double>(region_agg.total_cycles);
        case MetricKind::RegionTotalInstructions: return static_cast<double>(region_agg.total_instructions);
        case MetricKind::RegionTotalBranches:     return static_cast<double>(region_agg.total_branches);
        case MetricKind::RegionTotalBranchMisses: return static_cast<double>(region_agg.total_branch_misses);
        default: return 0.0;     // ratios/loop-only metrics use the
                                  // accumulator below, never this scalar
                                  // path.
    }
}

[[nodiscard]] inline double extract_from_loop(
    MetricKind kind,
    const bioseq::instr::LoopAggregate& loop_agg) noexcept
{
    using bioseq::instr::access::hist_percentile;
    switch (kind) {
        case MetricKind::LoopTotalIterations: return static_cast<double>(loop_agg.total_iterations);
        case MetricKind::LoopTotalWork:       return static_cast<double>(loop_agg.total_work);
        case MetricKind::LoopEmptyVisits:     return static_cast<double>(loop_agg.empty_visits);
        case MetricKind::LoopMinPerIterNs:    return static_cast<double>(loop_agg.min_per_iter_ns);
        case MetricKind::LoopMaxPerIterNs:    return static_cast<double>(loop_agg.max_per_iter_ns);
        case MetricKind::LoopP50PerIterNs:    return static_cast<double>(hist_percentile(loop_agg.per_iter_ns_hist, 0.50));
        case MetricKind::LoopP99PerIterNs:    return static_cast<double>(hist_percentile(loop_agg.per_iter_ns_hist, 0.99));
        default: return 0.0;
    }
}

/// Per-descriptor ratio extraction. Used by the by_regions emit site so
/// per-region buckets carry the actual ratio for that one descriptor
/// rather than a literal zero from the scalar `extract_from_*` switch's
/// default arm. Returns nullopt when the denominator is zero (the per-
/// region sample is then suppressed).
[[nodiscard]] inline std::optional<double> ratio_per_region(
    MetricKind kind,
    const bioseq::instr::RegionAggregate& region_agg) noexcept
{
    switch (kind) {
        case MetricKind::RegionIpc:
            if (region_agg.total_cycles == 0) return std::nullopt;
            return static_cast<double>(region_agg.total_instructions)
                 / static_cast<double>(region_agg.total_cycles);
        case MetricKind::RegionBranchMissRate:
            if (region_agg.total_branches == 0) return std::nullopt;
            return static_cast<double>(region_agg.total_branch_misses)
                 / static_cast<double>(region_agg.total_branches);
        default:
            return std::nullopt;     // not a region-ratio metric
    }
}

[[nodiscard]] inline std::optional<double> ratio_per_loop(
    MetricKind kind,
    const bioseq::instr::LoopAggregate& loop_agg) noexcept
{
    switch (kind) {
        case MetricKind::LoopMeanPerIterNs:
            if (loop_agg.total_iterations == 0) return std::nullopt;
            return static_cast<double>(loop_agg.total_ns)
                 / static_cast<double>(loop_agg.total_iterations);
        case MetricKind::LoopWorkPerIter:
            if (loop_agg.total_iterations == 0) return std::nullopt;
            return static_cast<double>(loop_agg.total_work)
                 / static_cast<double>(loop_agg.total_iterations);
        default:
            return std::nullopt;     // not a loop-ratio metric
    }
}

/// Running accumulator used to fold per-descriptor data into one
/// iteration-level value for region/loop metrics. The combiner field on
/// the registry entry decides which fields are populated and how they
/// finalise.
struct CrossDescriptorAccumulator {
    double                       sum = 0.0;
    std::uint64_t                ratio_numerator = 0;
    std::uint64_t                ratio_denominator = 0;
    std::array<std::uint64_t, 32> hist_sum{};   // for HistogramSum combiner
    bool                          any_contribution = false;
};

inline void combine_region(
    CrossDescriptorAccumulator& accumulator,
    MetricCombiner combiner,
    MetricKind kind,
    const bioseq::instr::RegionAggregate& region_agg) noexcept
{
    accumulator.any_contribution = true;
    switch (combiner) {
        case MetricCombiner::Sum:
            accumulator.sum += extract_from_region(kind, region_agg);
            return;
        case MetricCombiner::HistogramSum: {
            const auto& source_hist = region_agg.ns_hist.buckets;
            for (std::size_t i = 0; i < source_hist.size(); ++i) {
                accumulator.hist_sum[i] += source_hist[i];
            }
            return;
        }
        case MetricCombiner::PmuRatio:
            accumulator.ratio_numerator   += region_agg.total_instructions;
            accumulator.ratio_denominator += region_agg.total_cycles;
            return;
        case MetricCombiner::BranchMissRatio:
            accumulator.ratio_numerator   += region_agg.total_branch_misses;
            accumulator.ratio_denominator += region_agg.total_branches;
            return;
        case MetricCombiner::RequiresRegion:
            // With explicit `region` filter, only one descriptor
            // contributes — pass the value straight through.
            accumulator.sum = extract_from_region(kind, region_agg);
            return;
        case MetricCombiner::LoopMeanNs:
        case MetricCombiner::LoopWorkPerIter:
            return;     // not applicable to region-source metrics.
    }
}

inline void combine_loop(
    CrossDescriptorAccumulator& accumulator,
    MetricCombiner combiner,
    MetricKind kind,
    const bioseq::instr::LoopAggregate& loop_agg) noexcept
{
    accumulator.any_contribution = true;
    switch (combiner) {
        case MetricCombiner::Sum:
            accumulator.sum += extract_from_loop(kind, loop_agg);
            return;
        case MetricCombiner::HistogramSum: {
            const auto& source_hist = loop_agg.per_iter_ns_hist.buckets;
            for (std::size_t i = 0; i < source_hist.size(); ++i) {
                accumulator.hist_sum[i] += source_hist[i];
            }
            return;
        }
        case MetricCombiner::LoopMeanNs:
            accumulator.ratio_numerator   += loop_agg.total_ns;
            accumulator.ratio_denominator += loop_agg.total_iterations;
            return;
        case MetricCombiner::LoopWorkPerIter:
            accumulator.ratio_numerator   += loop_agg.total_work;
            accumulator.ratio_denominator += loop_agg.total_iterations;
            return;
        case MetricCombiner::RequiresRegion:
            accumulator.sum = extract_from_loop(kind, loop_agg);
            return;
        case MetricCombiner::PmuRatio:
        case MetricCombiner::BranchMissRatio:
            return;     // loop metrics never use PMU ratios directly.
    }
}

/// Finalise the accumulator into a single double, or `nullopt` when the
/// metric should be skipped this iteration (zero-denominator ratios, no
/// contributions, etc.).
[[nodiscard]] inline std::optional<double> finalise_combined_metric(
    const CrossDescriptorAccumulator& accumulator,
    MetricCombiner combiner,
    double percentile) noexcept
{
    if (!accumulator.any_contribution) return std::nullopt;
    switch (combiner) {
        case MetricCombiner::Sum:
        case MetricCombiner::RequiresRegion:
            return accumulator.sum;
        case MetricCombiner::HistogramSum: {
            // Build a temporary Histogram16 view over the summed buckets
            // and run the public hist_percentile helper.
            bioseq::instr::Histogram16 summed{};
            summed.buckets = accumulator.hist_sum;
            return static_cast<double>(
                bioseq::instr::access::hist_percentile(summed, percentile));
        }
        case MetricCombiner::PmuRatio:
        case MetricCombiner::BranchMissRatio:
        case MetricCombiner::LoopMeanNs:
        case MetricCombiner::LoopWorkPerIter:
            if (accumulator.ratio_denominator == 0) return std::nullopt;
            return static_cast<double>(accumulator.ratio_numerator)
                 / static_cast<double>(accumulator.ratio_denominator);
    }
    return std::nullopt;
}

/// Write one iteration's measurements into the cell's metric arrays:
/// - iteration-level outer `wall_clock_ns` always lands in
///   `all_regions["wall_clock_ns"]`.
/// - region/loop builtin metrics walk every visited descriptor, fold per
///   descriptor through the metric's combiner, then push the finalised
///   value into `all_regions[<metric_name>]`. When `by_regions: true`,
///   each visited descriptor's per-descriptor value also lands in
///   `<descriptor_name>[<metric_name>]`.
/// - counters use the snapshotted before/after deltas.
/// - evaluation metrics read the user's returned map.
/// V11: no per-cell mutex needed — items in a batch run sequentially
/// against a unique `Cell*`, so `samples` has a single writer.
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
    auto& all_regions = cell.samples["all_regions"];
    all_regions["wall_clock_ns"].push_back(iteration_wall_clock_ns);

    const auto descriptor_total = bioseq::instr::descriptor_count();
    std::size_t counter_index = 0;

    // Walk the declared metrics; pull each declared evaluation metric
    // out of `evaluation_metrics`. Returned-but-undeclared keys are
    // dropped silently here — `on_evaluate` lambdas are typically
    // generic across benchmarks so one returning more keys than this
    // benchmark uses is normal, not an error. Declared-but-not-returned
    // is still surfaced (via `semantic_validate_post_run`'s
    // `missing_evaluation_metric` check) — that direction usually means
    // a typo in the config.
    for (const auto& metric : metrics) {
        switch (metric.kind) {
            case MetricKind::WallClockNs:
                // Already pushed above.
                continue;
            case MetricKind::Counter: {
                const auto delta = counters_after[counter_index]
                                  - counters_before[counter_index];
                all_regions[metric.name].push_back(static_cast<double>(delta));
                ++counter_index;
                continue;
            }
            case MetricKind::Evaluation: {
                if (auto found = evaluation_metrics.find(metric.name);
                    found != evaluation_metrics.end())
                {
                    all_regions[metric.name].push_back(found->second);
                }
                // No log on miss: `semantic_validate_post_run` emits
                // `missing_evaluation_metric` once per cell after the
                // iteration loop joins, which is more useful and
                // doesn't flood the log with N copies of the same
                // message.
                continue;
            }
            default: break;     // builtin region/loop metric — fall through.
        }

        const auto* entry = lookup_builtin_metric(metric.name);
        if (entry == nullptr) continue;     // already-flagged by semantic validation

        CrossDescriptorAccumulator accumulator;
        const bool restrict_to_one_region = !metric.region.empty();

        // Walk every visited descriptor whose name and kind match the
        // metric's source family.
        for (std::uint32_t descriptor_id = 1; descriptor_id <= descriptor_total; ++descriptor_id) {
            const auto* descriptor = bioseq::instr::descriptor_lookup(descriptor_id);
            if (descriptor == nullptr || descriptor->name == nullptr) continue;
            if (restrict_to_one_region
                && std::string_view{descriptor->name} != std::string_view{metric.region}) continue;

            if (entry->source == MetricSource::Region) {
                const auto* region_agg = profiler_block.aggregates.try_get(descriptor_id);
                if (region_agg == nullptr || region_agg->visits == 0) continue;
                combine_region(accumulator, entry->combiner, entry->kind, *region_agg);
                if (by_regions) {
                    // Ratio metrics need a per-descriptor numerator/
                    // denominator computation; the scalar `extract_*` path
                    // only knows how to dump a single field, so it would
                    // emit literal 0 for IPC / branch_miss_rate.
                    if (entry->combiner == MetricCombiner::PmuRatio
                     || entry->combiner == MetricCombiner::BranchMissRatio) {
                        if (auto ratio = ratio_per_region(entry->kind, *region_agg);
                            ratio.has_value()) {
                            cell.samples[descriptor->name][metric.name].push_back(*ratio);
                        }
                    } else {
                        cell.samples[descriptor->name][metric.name]
                            .push_back(extract_from_region(entry->kind, *region_agg));
                    }
                }
            } else if (entry->source == MetricSource::Loop) {
                const auto* loop_agg = profiler_block.loop_aggregates.try_get(descriptor_id);
                if (loop_agg == nullptr || loop_agg->visits == 0) continue;
                combine_loop(accumulator, entry->combiner, entry->kind, *loop_agg);
                if (by_regions) {
                    if (entry->combiner == MetricCombiner::LoopMeanNs
                     || entry->combiner == MetricCombiner::LoopWorkPerIter) {
                        if (auto ratio = ratio_per_loop(entry->kind, *loop_agg);
                            ratio.has_value()) {
                            cell.samples[descriptor->name][metric.name].push_back(*ratio);
                        }
                    } else {
                        cell.samples[descriptor->name][metric.name]
                            .push_back(extract_from_loop(entry->kind, *loop_agg));
                    }
                }
            }
        }

        if (auto finalised = finalise_combined_metric(accumulator, entry->combiner, entry->percentile);
            finalised.has_value()) {
            all_regions[metric.name].push_back(*finalised);
        }
        // Else: skipped this iteration (zero-denominator ratio or no
        // contribution). The sample array stays sparse; the rule
        // evaluator's median-of-samples handles missing iterations
        // gracefully.
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

// ----------------- WorkBatch + scheduler -----------------

/// One entry inside a batch — one tuple's iteration loop bound to the
/// batch's cached `Input`.
struct WorkBatchItem {
    Cell* cell = nullptr;        // points back into state.cells
    /// Cached strong reference to the underlying tuple. Saves a hop in
    /// the UI label updater on the cold path.
    const ExecutionTuple* tuple = nullptr;
};

/// One scheduling unit: every item in the batch shares one cached
/// `Input` instance built from the same `(test_case, group)`. Items
/// run sequentially within the batch; batches run in parallel.
struct WorkBatch {
    std::size_t test_case_index = 0;
    std::size_t group_index     = 0;
    std::size_t group_count     = 1;
    /// Sequence number of this batch in `state.cells`-order. Used as a
    /// stable index into the per-batch slot vector by the scheduler.
    std::size_t batch_index     = 0;
    std::vector<WorkBatchItem> items{};
};

/// Walk `state.cells` once and group entries by `(test_case_index,
/// group_index)`. The result is one batch per group; items inside each
/// batch are tuples in `state.execution_tuples`-declared order.
[[nodiscard]] inline std::vector<WorkBatch> build_work_batches(
    BenchmarkExecutionState& state)
{
    // Use an ordered map keyed by the (tc_index, group_index) pair so
    // batches come out in a deterministic order. The driver later treats
    // `batches[i].batch_index` as i (assigned below in a final pass).
    std::map<std::pair<std::size_t, std::size_t>, WorkBatch> by_key;
    for (auto& cell_ptr : state.cells) {
        const auto key = std::pair{cell_ptr->test_case_index, cell_ptr->group_index};
        auto& batch = by_key[key];
        if (batch.items.empty()) {
            batch.test_case_index = cell_ptr->test_case_index;
            batch.group_index     = cell_ptr->group_index;
            batch.group_count     = cell_ptr->group_count;
        }
        batch.items.push_back(WorkBatchItem{ cell_ptr.get(), cell_ptr->tuple });
    }

    std::vector<WorkBatch> batches;
    batches.reserve(by_key.size());
    for (auto& [key, batch] : by_key) {
        (void)key;
        batch.batch_index = batches.size();
        batches.push_back(std::move(batch));
    }
    return batches;
}

// ----------------- Progress helpers (single source of writes) -----------------

/// Lazily resolve the `ProgressTestCase` for the current benchmark
/// closure. Returns nullptr when the progress dashboard wasn't
/// installed, the per-benchmark slot hasn't completed setup yet, or
/// any index is out of bounds. Centralised so the five `progress_*`
/// helpers don't repeat the same five-line guard chain.
[[nodiscard]] inline ProgressTestCase* progress_test_case_for(
    const BenchmarkContext& context, std::size_t test_case_index) noexcept
{
    if (context.progress == nullptr) return nullptr;
    if (context.benchmark_index >= context.progress->per_benchmark.size())
        return nullptr;
    auto& per = *context.progress->per_benchmark[context.benchmark_index];
    if (!per.setup_done.load(std::memory_order_acquire)) return nullptr;
    if (test_case_index >= per.test_cases.size()) return nullptr;
    return per.test_cases[test_case_index].get();
}

/// Lazily resolve the `ProgressGroup` for the current benchmark
/// closure. Returns nullptr when no test case or group exists at the
/// supplied indices.
[[nodiscard]] inline ProgressGroup* progress_group_for(
    const BenchmarkContext& context,
    std::size_t test_case_index, std::size_t group_index) noexcept
{
    auto* tc = progress_test_case_for(context, test_case_index);
    if (tc == nullptr) return nullptr;
    if (group_index >= tc->groups.size()) return nullptr;
    return tc->groups[group_index].get();
}

inline void progress_mark_test_case_start(
    BenchmarkContext& context, const WorkBatch& batch) noexcept
{
    auto* tc = progress_test_case_for(context, batch.test_case_index);
    if (tc == nullptr) return;
    std::int64_t expected = 0;
    tc->start_ns.compare_exchange_strong(
        expected, bioseq::instr::clock::now_ns(),
        std::memory_order_relaxed, std::memory_order_relaxed);
    if (batch.group_index < tc->groups.size()) {
        tc->groups[batch.group_index]->active.store(true, std::memory_order_relaxed);
    }
}

inline void progress_update_label(
    BenchmarkContext& context, const WorkBatch& batch,
    const WorkBatchItem& item, std::string_view benchmark_key) noexcept
{
    auto* group = progress_group_for(context, batch.test_case_index, batch.group_index);
    if (group == nullptr) return;
    auto label = fmt::format("algo={}, bench={}",
                             item.tuple->algorithm_name, benchmark_key);
    std::lock_guard<std::mutex> guard(group->label_mu);
    group->label = std::move(label);
}

inline void progress_tick_iteration(
    BenchmarkContext& context, const Cell& cell) noexcept
{
    auto* group = progress_group_for(context, cell.test_case_index, cell.group_index);
    if (group == nullptr) return;
    group->done.fetch_add(1, std::memory_order_relaxed);
}

inline void progress_tick_item_done(
    BenchmarkContext& context, const WorkBatch& batch) noexcept
{
    auto* tc = progress_test_case_for(context, batch.test_case_index);
    if (tc == nullptr) return;
    tc->tuple_done.fetch_add(1, std::memory_order_relaxed);
}

inline void progress_mark_group_done(
    BenchmarkContext& context, const WorkBatch& batch) noexcept
{
    auto* tc = progress_test_case_for(context, batch.test_case_index);
    if (tc == nullptr) return;
    if (batch.group_index < tc->groups.size()) {
        tc->groups[batch.group_index]->active.store(false, std::memory_order_relaxed);
    }
    // The test case is "done" once every batch's item-level tick has
    // fired. The first thread to observe `tuple_done >= tuple_total`
    // stamps `finish_ns` via CAS-from-zero; later observers see a
    // non-zero `expected` and skip. (`tuple_done` and `tuple_total`
    // are loaded separately, but post-completion no other thread is
    // mutating them, so the loads always converge to consistent values.)
    const auto total = tc->tuple_total.load(std::memory_order_relaxed);
    if (tc->tuple_done.load(std::memory_order_relaxed) >= total) {
        std::int64_t expected = 0;
        tc->finish_ns.compare_exchange_strong(
            expected, bioseq::instr::clock::now_ns(),
            std::memory_order_relaxed, std::memory_order_relaxed);
    }
}

// ----------------- V12 progress helpers (retry / abort) -----------------
//
// All of these follow the same nullptr-safe lookup pattern through
// `progress_group_for`. Failure-string fields are guarded by
// `ProgressGroup::failure_mu`; the renderer only consults them when the
// matching atomic counter is non-zero.

/// Retry-failure category; templated over the "kind" so input and
/// cleanup paths share one body. Members map onto matching ProgressGroup
/// fields.
enum class RetryKind : std::uint8_t { Input, Cleanup };

inline void progress_record_retry_failure(
    BenchmarkContext& context, const WorkBatch& batch,
    RetryKind kind, std::uint8_t attempt,
    std::string_view error_message,
    std::string_view stack_trace) noexcept
{
    auto* group = progress_group_for(context,
                                      batch.test_case_index, batch.group_index);
    if (group == nullptr) return;
    {
        std::lock_guard<std::mutex> guard(group->failure_mu);
        if (kind == RetryKind::Input) {
            group->input_failure_message = std::string{error_message};
            group->input_failure_trace   = std::string{stack_trace};
        } else {
            group->cleanup_failure_message = std::string{error_message};
            group->cleanup_failure_trace   = std::string{stack_trace};
        }
    }
    auto& counter = (kind == RetryKind::Input)
                       ? group->input_retry_attempt
                       : group->cleanup_retry_attempt;
    counter.store(attempt, std::memory_order_release);
}

inline void progress_clear_retry_failure(
    BenchmarkContext& context, const WorkBatch& batch,
    RetryKind kind) noexcept
{
    auto* group = progress_group_for(context,
                                      batch.test_case_index, batch.group_index);
    if (group == nullptr) return;
    // Counter is invalidated FIRST so a racing renderer's fast-path
    // load may see 0 immediately. The renderer additionally re-reads
    // the counter under `failure_mu` after locking; that is what
    // closes the "fast read N>0, then strings cleared, then locked,
    // then render with empty strings" race window. See `build_detail_rows`
    // for the read side; both writers (record and clear) and the reader
    // must agree on this protocol.
    auto& counter = (kind == RetryKind::Input)
                       ? group->input_retry_attempt
                       : group->cleanup_retry_attempt;
    counter.store(0, std::memory_order_release);
    std::lock_guard<std::mutex> guard(group->failure_mu);
    if (kind == RetryKind::Input) {
        group->input_failure_message.clear();
        group->input_failure_trace.clear();
    } else {
        group->cleanup_failure_message.clear();
        group->cleanup_failure_trace.clear();
    }
}

/// Mark a batch as aborted using bare indices. Shared between the
/// V12 `WorkBatch`-driven path and the V16 scheduler-driven idle-
/// timeout path (the latter has no live `WorkBatch`).
inline void progress_mark_batch_aborted_at(
    BenchmarkContext& context,
    std::size_t test_case_index, std::size_t group_index,
    std::string_view reason) noexcept
{
    auto* tc = progress_test_case_for(context, test_case_index);
    if (tc == nullptr) return;
    if (group_index < tc->groups.size()) {
        auto& group = *tc->groups[group_index];
        // Drop any in-flight retry rows so the renderer's `Aborted`
        // detail doesn't sit alongside a stale "▶ ⛔ [retry 5/5]" row
        // on the same group. Counters are zeroed first to invalidate
        // the renderer's fast path; strings (including the new
        // `aborted_reason`) are written under `failure_mu`.
        group.input_retry_attempt.store(0, std::memory_order_release);
        group.cleanup_retry_attempt.store(0, std::memory_order_release);
        group.idle_start_ns.store(0, std::memory_order_release);
        {
            std::lock_guard<std::mutex> guard(group.failure_mu);
            group.input_failure_message.clear();
            group.input_failure_trace.clear();
            group.cleanup_failure_message.clear();
            group.cleanup_failure_trace.clear();
            group.aborted_reason = std::string{reason};
        }
        group.aborted.store(true, std::memory_order_release);
        group.active.store(false, std::memory_order_relaxed);
    }
    // Stamp finish so the renderer doesn't keep counting elapsed time
    // for an aborted batch.
    std::int64_t expected = 0;
    tc->finish_ns.compare_exchange_strong(
        expected, bioseq::instr::clock::now_ns(),
        std::memory_order_relaxed, std::memory_order_relaxed);
}

inline void progress_mark_batch_aborted(
    BenchmarkContext& context, const WorkBatch& batch,
    std::string_view reason) noexcept
{
    progress_mark_batch_aborted_at(context, batch.test_case_index,
                                    batch.group_index, reason);
}

// ----------------- Diagnostic emit helper (V12) -----------------

/// Push one ValidationDiag into `*context.diagnostics` under
/// `*context.diagnostics_mu`. nullptr-safe so the helper composes with
/// places where diagnostics aren't wired (none today, but future test
/// fixtures might construct a bare context).
inline void push_runtime_diagnostic(
    BenchmarkContext& context,
    ValidationDiag::Severity severity,
    std::string_view code,
    std::string_view location,
    std::string message) noexcept
{
    if (context.diagnostics == nullptr || context.diagnostics_mu == nullptr) return;
    ValidationDiag diag;
    diag.severity = severity;
    diag.code     = std::string{code};
    diag.location = std::string{location};
    diag.message  = std::move(message);
    std::lock_guard<std::mutex> guard(*context.diagnostics_mu);
    context.diagnostics->push_back(std::move(diag));
}

/// Variadic facade — accepts `fmt::format_string<Args...>` so format
/// specifiers are checked at compile time. `noexcept`-style signature
/// matches the rest of the framework's progress helpers; a bad_alloc
/// from `fmt::format` would terminate, which is the same contract as
/// any other framework path.
template<typename... Args>
inline void emit_runtime_diagnostic(
    BenchmarkContext& context,
    ValidationDiag::Severity severity,
    std::string_view code,
    std::string_view location,
    fmt::format_string<Args...> fmt_str,
    Args&&... args) noexcept
{
    push_runtime_diagnostic(context, severity, code, location,
        fmt::format(fmt_str, std::forward<Args>(args)...));
}

/// Build a "<config>:<benchmark>/<test_case>(group N)" location string
/// for runtime diagnostics. Mirrors `semantic_location` for a similar
/// shape, reusable by every per-batch error path.
[[nodiscard]] inline std::string runtime_location(
    std::string_view config_path,
    std::string_view benchmark_key,
    std::string_view test_case_name,
    std::size_t group_index) noexcept
{
    return fmt::format("{}:{}/{}(group {})",
                       config_path, benchmark_key, test_case_name, group_index);
}

// ----------------- BatchScheduler out-of-line bodies -----------------

inline void BatchScheduler::emit_idle_timeout_locked(
    const ScheduledBatch& batch) noexcept
{
    if (batch.context == nullptr) return;
    auto reason = fmt::format(
        "batch '{}' (group {}) waited {}s for {} B (base+extra) with no "
        "peer batches running — giving up",
        batch.test_case_name, batch.group_index,
        kMaxIdleWaitingTime.count(),
        batch.base_bytes + batch.extra_bytes);
    push_runtime_diagnostic(*batch.context,
        ValidationDiag::Severity::Error,
        "memory_budget_constraint_timeout", batch.location_str, reason);
    progress_mark_batch_aborted_at(*batch.context,
        batch.test_case_index, batch.group_index, reason);
}

// ----------------- V12 retry helpers (G2 / G3) -----------------

/// Render `trace` clipped to the first `kMaxStackTraceLines` frames
/// using cpptrace's built-in `to_string(color=false)`. We always
/// produce uncoloured output because the rendered string is later
/// embedded inside ftxui `text()` elements, which do NOT interpret
/// ANSI escape sequences — colour codes would render as literal
/// `[34m…[0m` text. The dashboard's visual tint for trace lines comes
/// from ftxui's `| color(Color::GrayDark)` decorator instead.
[[nodiscard]] inline std::string
clip_trace(const cpptrace::stacktrace& trace) noexcept {
    cpptrace::stacktrace clipped = trace;            // implicit copy
    const bool was_truncated = clipped.frames.size() > kMaxStackTraceLines;
    if (was_truncated) {
        clipped.frames.resize(kMaxStackTraceLines);
    }
    auto rendered = clipped.to_string(/*color=*/false);
    if (was_truncated) rendered += "\n  … (more frames elided)";
    return rendered;
}

/// Generic retry helper used by both `on_input` and `on_cleanup` paths.
///
/// Runs `attempt()` up to `kMaxTaskInputHandlerErrorCount` times. Each
/// failure is recorded into the dashboard's per-batch failure slot
/// (keyed by `kind`). On success, the failure record is cleared and
/// the function returns `true`; on every-attempt-failure it returns
/// `false` and the caller decides whether to emit a hard error
/// (`Input` path) or a warning (`Cleanup` path).
///
/// `cpptrace::try_catch` is the multi-catch entry point — stacking
/// CPPTRACE_CATCH clauses on a single CPPTRACE_TRY does not compile
/// (each CPPTRACE_TRY closes its inner `try` with one `CATCH`). The
/// `try_catch` helper recursively wraps each catcher in its own
/// TRY/CATCH expansion so all of them install the unwind interceptor
/// `from_current_exception()` needs.
template<RetryKind kind, typename Attempt>
[[nodiscard]] inline bool retry_user_callback(
    BenchmarkContext& context, const WorkBatch& batch,
    Attempt&& attempt) noexcept
{
    for (std::size_t i = 0; i < kMaxTaskInputHandlerErrorCount; ++i) {
        bool failed = false;
        cpptrace::try_catch(
            [&] { attempt(); },
            [&] (const std::exception& e) {
                progress_record_retry_failure(
                    context, batch, kind,
                    static_cast<std::uint8_t>(i + 1),
                    e.what(),
                    clip_trace(cpptrace::from_current_exception()));
                failed = true;
            },
            [&] {     // void(): cpptrace's idiom for "catch (...)"
                progress_record_retry_failure(
                    context, batch, kind,
                    static_cast<std::uint8_t>(i + 1),
                    "<non-std exception>", "<no trace>");
                failed = true;
            }
        );
        if (!failed) {
            progress_clear_retry_failure(context, batch, kind);
            return true;
        }
        // Hold the retry-failure row visible for at least one render
        // cycle — see `kRetryFailureMinDisplayTime`'s comment. Applied
        // after every failed attempt (including the last) so the final
        // [retry 5/5] frame survives long enough for the dashboard to
        // paint it before the caller marks the batch aborted (which
        // wipes the retry counter).
        std::this_thread::sleep_for(kRetryFailureMinDisplayTime);
    }
    return false;
}

/// Try `on_input` up to `kMaxTaskInputHandlerErrorCount` times. Returns
/// `nullopt` on every-attempt-failure; the caller is expected to emit
/// `input_handler_failed_permanently` and abort the batch.
template<typename Input, typename Builder>
[[nodiscard]] inline std::optional<Input>
build_input_with_retry(BenchmarkContext& context,
                        const WorkBatch& batch,
                        Builder&& builder) noexcept
{
    std::optional<Input> result;
    const bool ok = retry_user_callback<RetryKind::Input>(
        context, batch, [&] { result.emplace(builder()); });
    if (!ok) result.reset();
    return result;
}

/// Try `on_cleanup` up to `kMaxTaskInputHandlerErrorCount` times. On
/// every-attempt-failure emits the `cleanup_function_failing` warning
/// and resumes — cleanup leaks are accepted because the OS / developer
/// will reclaim resources externally (per spec).
template<typename Cleaner>
inline void cleanup_with_retry(BenchmarkContext& context,
                                const WorkBatch& batch,
                                std::string_view test_case_name,
                                std::string_view location,
                                Cleaner&& cleaner) noexcept
{
    if (retry_user_callback<RetryKind::Cleanup>(context, batch,
                                                  std::forward<Cleaner>(cleaner)))
    {
        return;
    }
    emit_runtime_diagnostic(context, ValidationDiag::Severity::Warning,
        "cleanup_function_failing", location,
        "batch '{}' (group {}) cleanup gave up after {} attempts — "
        "assuming OS / developer reclaim",
        test_case_name, batch.group_index, kMaxTaskInputHandlerErrorCount);
    progress_clear_retry_failure(context, batch, RetryKind::Cleanup);
}

// ----------------- run_unit driver -----------------

template<typename Input, typename Output>
inline void run_typed_benchmark(
    BenchmarkExecutionState& state,
    BenchmarkContext& context,
    const std::function<RunConstraints(std::string_view, std::string_view, simdjson::dom::element)>& on_estimate_resources,
    const std::function<Input(std::string_view, std::string_view, simdjson::dom::element, const RunConstraints&)>& on_input,
    const std::function<Output(std::string_view, const ParamMap&, const Input&)>& on_run,
    const std::function<std::map<std::string, double>(std::string_view, const ParamMap&, const Input&, const Output&)>& on_evaluate,
    const std::function<void(std::string_view, std::string_view, simdjson::dom::element, const Input&)>& on_cleanup)
{
    using bioseq::instr::installed_profiler_scope;
    using bioseq::instr::ProfilerBlock;

    const std::string                 data_dir = context.data_dir;
    const auto counter_handles = resolve_counter_handles(state.config.metrics);
    const bool by_regions = state.config.by_regions;

    // Pre-built batch list: one batch per (test_case, group). Items
    // inside a batch run sequentially against one cached Input.
    auto batches = build_work_batches(state);
    if (batches.empty()) return;

    auto* scheduler = context.scheduler;
    DEBUG_ASSERT(scheduler != nullptr,
        "BenchmarkContext::scheduler must be set by run_impl");

    /// Heap-owned cache of per-batch input/constraint slots. The bodies
    /// enqueued into the scheduler reference the slots by index;
    /// lifetime extends through `wait_for_benchmark` (called below)
    /// which blocks until every body has finished.
    struct GroupSlot {
        std::optional<Input> input{};
        RunConstraints       constraints{};
    };
    auto group_slots = std::make_unique<std::vector<GroupSlot>>(batches.size());
    auto batches_ptr = std::make_unique<std::vector<WorkBatch>>(std::move(batches));

    /// Run exactly one iteration of one cell against `input`. Hot path.
    auto run_one_iteration = [&context, &state, by_regions, &counter_handles,
                                &on_run, &on_evaluate](Cell& cell,
                                                          const Input& input) {
        const auto& tuple = *cell.tuple;

        ProfilerBlock<profiler_args_t> profiler_block{};
        installed_profiler_scope<profiler_args_t> profiler_scope{profiler_block};

        const auto counters_before = snapshot_counters(counter_handles);

        const auto t_begin_ns = bioseq::instr::clock::now_ns();
        const Output output = on_run(tuple.algorithm_name, tuple.params, input);
        const auto t_end_ns = bioseq::instr::clock::now_ns();
        const auto iteration_wall_clock_ns = static_cast<double>(t_end_ns - t_begin_ns);

        const auto counters_after = snapshot_counters(counter_handles);

        const auto evaluation_metrics =
            on_evaluate(tuple.algorithm_name, tuple.params, input, output);

        append_iteration_samples(cell, by_regions, profiler_block,
                                  iteration_wall_clock_ns, state.config.metrics,
                                  counters_before, counters_after, evaluation_metrics);

        progress_tick_iteration(context, cell);
    };

    /// Run every iteration of one item. V16: NO LOCKS — bench-lock and
    /// serial-concurrency exclusion are enforced by the scheduler's
    /// admission predicates, so the body never blocks on a process-
    /// global mutex. This is what restores cross-batch parallelism
    /// (the V12 cross_bench_mu writer-lock was the dominant
    /// bottleneck on bench_v10 — see main/DOCS/BENCHMARK_FRAMEWORK/BENCH_V16_IMPL_PLAN__DOC0233.md §0.2).
    auto run_item_iterations = [run_one_iteration](
        const WorkBatchItem& item, const Input& input)
    {
        for (std::size_t iter = 0; iter < item.cell->iterations; ++iter) {
            run_one_iteration(*item.cell, input);
        }
    };

    /// V16 batch body — no memory acquire/release, no cross_bench_mu.
    /// Called from TBB worker after the scheduler has admitted the
    /// batch. The reservation lives entirely scheduler-side.
    auto run_one_batch_unblocked = [&context, &state, &on_estimate_resources,
                                       &on_input, &on_cleanup,
                                       &data_dir, &group_slots,
                                       run_item_iterations](WorkBatch& batch)
    {
        const auto& spec = state.test_cases[batch.test_case_index];
        auto& slot = (*group_slots)[batch.batch_index];
        const auto location_str = runtime_location(
            state.config.config_path, state.config.benchmark_key,
            spec.name, batch.group_index);

        // 1. Estimate resources (still needed: on_input wants
        //    `slot.constraints`). The scheduler already reserved the
        //    same numbers — re-evaluating here is cheap.
        slot.constraints = on_estimate_resources(
            data_dir, spec.name, spec.data);

        // 2. on_input with retries. Permanent failure aborts the
        //    batch (scheduler releases on body return).
        auto built_input = build_input_with_retry<Input>(
            context, batch,
            [&] { return on_input(data_dir, spec.name, spec.data,
                                   slot.constraints); });
        if (!built_input.has_value()) {
            auto reason = fmt::format(
                "batch '{}' (group {}) gave up after {} attempts at on_input — "
                "see UI for last error / trace",
                spec.name, batch.group_index, kMaxTaskInputHandlerErrorCount);
            push_runtime_diagnostic(context, ValidationDiag::Severity::Error,
                "input_handler_failed_permanently", location_str, reason);
            progress_mark_batch_aborted(context, batch, reason);
            return;
        }
        slot.input = std::move(built_input);

        // 3. Iterate items × iterations. Lock-free; scheduler enforced
        //    exclusion.
        progress_mark_test_case_start(context, batch);
        for (auto& item : batch.items) {
            progress_update_label(context, batch, item, state.config.benchmark_key);
            run_item_iterations(item, *slot.input);
            progress_tick_item_done(context, batch);
        }

        // 4. Cleanup with retries — warning on permanent failure.
        if (on_cleanup) {
            cleanup_with_retry(context, batch, spec.name, location_str,
                [&] { on_cleanup(data_dir, spec.name, spec.data, *slot.input); });
        }
        slot.input.reset();
        progress_mark_group_done(context, batch);
    };

    // V16 enqueue path — replaces the V12 `tbb::parallel_for_each`
    // batch loop. Each batch is enqueued into the scheduler with its
    // pre-computed memory + lock requirements; bodies are dispatched
    // by the scheduler when admissible.
    const auto sys_at_enqueue = snapshot_system_memory();
    const auto cap_at_enqueue = MemoryBudget::cap_for(
        sys_at_enqueue.total_bytes, scheduler->memory_buffer_pct());

    for (auto& batch : *batches_ptr) {
        const auto& spec = state.test_cases[batch.test_case_index];
        const auto constraints = on_estimate_resources(
            data_dir, spec.name, spec.data);
        const std::size_t total_required =
            constraints.base_memory + constraints.extra_memory;
        const auto location_str = runtime_location(
            state.config.config_path, state.config.benchmark_key,
            spec.name, batch.group_index);

        // G1.d  Hard error: combined requirements exceed total system RAM.
        if (total_required >= sys_at_enqueue.total_bytes) {
            auto reason = fmt::format(
                "batch '{}' (group {}) requires {} B (base+extra) but the host "
                "has only {} B total RAM",
                spec.name, batch.group_index,
                total_required, sys_at_enqueue.total_bytes);
            push_runtime_diagnostic(context, ValidationDiag::Severity::Error,
                "memory_budget_not_satisfiable", location_str, reason);
            progress_mark_batch_aborted(context, batch, reason);
            continue;       // do not enqueue
        }
        // G1.c  Warning: combined requirements exceed buffered cap.
        if (total_required >= cap_at_enqueue) {
            emit_runtime_diagnostic(context, ValidationDiag::Severity::Warning,
                "memory_budget_oversized", location_str,
                "batch '{}' (group {}) requires {} B but buffered cap is {} B "
                "(host total {} B, buffer {:.0f}%)",
                spec.name, batch.group_index,
                total_required, cap_at_enqueue,
                sys_at_enqueue.total_bytes, scheduler->memory_buffer_pct());
            // Still enqueue — scheduler will admit once peers free up.
        }

        ScheduledBatch sb{};
        sb.benchmark_index    = context.benchmark_index;
        sb.bench_lock         = state.config.concurrency.bench_lock;
        sb.concurrency_serial = !state.config.concurrency.concurrency;
        sb.base_bytes         = constraints.base_memory;
        sb.extra_bytes        = constraints.extra_memory;
        sb.group_for_ui       = progress_group_for(
            context, batch.test_case_index, batch.group_index);
        sb.location_str       = location_str;
        sb.test_case_name     = spec.name;
        sb.test_case_index    = batch.test_case_index;
        sb.group_index        = batch.group_index;
        sb.context            = &context;
        sb.body = [&run_one_batch_unblocked, &batch] {
            run_one_batch_unblocked(batch);
        };
        scheduler->enqueue_batch(std::move(sb));
    }

    // Block until every batch under this benchmark_index has finished
    // or aborted. Required to keep `state`, `*group_slots`, and the
    // captured references alive through every enqueued body's
    // execution. See main/DOCS/BENCHMARK_FRAMEWORK/BENCH_V16_IMPL_PLAN__DOC0233.md §3.6.
    scheduler->wait_for_benchmark(context.benchmark_index);

    // After wait_for_benchmark returns, all bodies have finished.
    // batches_ptr / group_slots are still owned by `run_typed_benchmark`'s
    // stack, so unique_ptr destructors run on the way out.
}

// ----------------- Sample aggregation (V17) -----------------

/// Number of histogram buckets in `AggregatedSamples::histogram`.
/// Constant by spec — the schema enforces `minItems == maxItems == 10`,
/// so changing this requires a coordinated schema bump.
inline constexpr std::size_t kHistogramBucketCount = 10;

/// Maximum length of `AggregatedSamples::top10` / `bottom10`.
/// Shorter when fewer than 10 finite samples were observed.
inline constexpr std::size_t kKBestSize = 10;

/// Per-cell summary stats. Computed by `aggregate_samples_for_report`
/// over the *finite subset* of the sample vector (NaN / ±inf are
/// dropped before sorting). Default-constructed values (all-zero)
/// are emitted when no finite samples were observed.
struct AggregateStats {
    double min     = 0.0;
    double max     = 0.0;
    double mean    = 0.0;
    /// Trimmed mean: drop floor(0.1 * finite_count) samples from
    /// each end of the sorted finite subset, average the rest.
    /// Equals `mean` when finite_count < 10 (trim == 0).
    double mean90  = 0.0;
    double sum     = 0.0;
    /// Sample standard deviation, n-1 denominator. 0 when
    /// finite_count < 2 or when `mean` is non-finite (sum
    /// overflowed): in the latter case (s - mean)² is NaN and
    /// would poison the result; bail to 0.
    double stddev  = 0.0;
};

/// Five canonical percentile slots reported per cell. Indexed via
/// the registry below so adding a new percentile is a one-row
/// edit.
struct AggregatePercentiles {
    double p01 = 0.0;
    double p20 = 0.0;
    double p50 = 0.0;
    double p80 = 0.0;
    double p99 = 0.0;
};

/// One bucket of the linear-binned histogram. `count` is `int64_t`
/// (rather than `size_t`) because that is what the JSON schema's
/// `integer` type maps to — and because non-finite values cannot
/// poison it the way they could a `double`.
struct AggregateBucket {
    double      from  = 0.0;
    double      to    = 0.0;
    std::size_t count = 0;
};

/// Pre-aggregated view of a sample vector. Replaces raw
/// `std::vector<double>` everywhere a consumer reads samples for
/// reporting / UI purposes. The current run still collects raw
/// samples in `Cell::samples` (so per-rule comparators keep using
/// `aggregate_samples(Aggregate, vector<double>)` without an
/// in-memory format change); the writer turns each cell's vector
/// into one of these at write time, and historical-report
/// readers parse this shape directly.
///
/// Invariants on a non-default-constructed value (set by
/// `aggregate_samples_for_report` when at least one finite sample
/// was present):
///   * `top10[0] == stats.max` and the array is sorted descending.
///   * `bottom10[0] == stats.min` and the array is sorted ascending.
///   * `top10.size() == bottom10.size()` (both `min(10, finite_count)`).
///   * `Σ histogram[i].count == finite_count` (= number of finite
///     samples; non-finite samples never enter any bucket).
///   * `histogram[i].to == histogram[i+1].from` for `i < 9`. Holds
///     for ordinary inputs. For pathological inputs where
///     `max - min` overflows to ±∞ the bucket bounds collapse to
///     `±∞`; the on-disk round-trip still preserves bucket-edge
///     equality because `sanitize_for_json` clamps `±∞` symmetrically
///     before serialisation.
///
/// `parse_aggregated_samples` (the inverse) **does not** guarantee
/// the bucket-edge or descending/ascending invariants on input it
/// can't trust — it returns whatever the historical report
/// contained, with default-constructed slots wherever the source
/// was malformed. Consumers that need the full invariants should
/// re-aggregate from raw samples instead of trusting parser output.
struct AggregatedSamples {
    std::size_t                                            samples_count = 0;
    AggregateStats                                          stats{};
    AggregatePercentiles                                    percentiles{};
    std::vector<double>                                     top10{};
    std::vector<double>                                     bottom10{};
    std::array<AggregateBucket, kHistogramBucketCount>      histogram{};
};

/// True iff `aggregated` carries at least one *finite* sample.
/// `samples_count == 0` ⇒ the cell wasn't emitted at all.
/// `samples_count > 0 && top10.empty()` ⇒ every emitted sample
/// was non-finite (NaN / ±inf), so every aggregate field is
/// default-zero and consumers must NOT treat the percentile slots
/// as data. Used by the renderer, the regression evaluator, and
/// any other reader that needs "is there real data here?". Single
/// definition next to the type so all consumers stay aligned.
[[nodiscard]] inline bool has_finite_samples(const AggregatedSamples& aggregated) noexcept {
    return aggregated.samples_count > 0 && !aggregated.top10.empty();
}

/// Compile-time table linking each percentile to its
/// `AggregatePercentiles` field. Single source of truth used by
/// the aggregator (writes the field), the writer (emits the JSON
/// key), the historical-report reader (parses the same key), and
/// `aggregate_samples(Aggregate, AggregatedSamples)` for the
/// rule-eval percentile lookups (P01 / P50 / P99 only — the
/// table also drives `aggregated_from_report`).
struct AggregatedPercentileEntry {
    std::string_view  name;          // "P01" / "P20" / "P50" / "P80" / "P99"
    double            fraction;      // 0.01 / 0.20 / 0.50 / 0.80 / 0.99
    double AggregatePercentiles::* slot;  // pointer-to-member into the struct
};

inline constexpr std::array<AggregatedPercentileEntry, 5>
kAggregatedPercentileRegistry{ {
    { "P01", 0.01, &AggregatePercentiles::p01 },
    { "P20", 0.20, &AggregatePercentiles::p20 },
    { "P50", 0.50, &AggregatePercentiles::p50 },
    { "P80", 0.80, &AggregatePercentiles::p80 },
    { "P99", 0.99, &AggregatePercentiles::p99 },
} };

/// Compile-time table linking each `stats` field to its JSON
/// spelling. Drives both the writer (serialises in this order)
/// and the reader (looks each field up by name). Adding a new
/// per-cell statistic is a one-row edit (plus a slot in
/// `AggregateStats`) that propagates to both sides automatically.
struct AggregatedStatsEntry {
    std::string_view  name;
    double AggregateStats::* slot;
};

inline constexpr std::array<AggregatedStatsEntry, 6>
kAggregatedStatsRegistry{ {
    { "min",    &AggregateStats::min    },
    { "max",    &AggregateStats::max    },
    { "mean",   &AggregateStats::mean   },
    { "mean90", &AggregateStats::mean90 },
    { "sum",    &AggregateStats::sum    },
    { "stddev", &AggregateStats::stddev },
} };

/// Compile-time reverse lookup `slot → JSON key`. Lets every
/// reader path (sanity detector, future stats consumers) refer
/// to a stat field by its `&AggregateStats::min` pointer-to-member
/// and resolve the spelling through the same registry the writer
/// uses, so a renamed field flows everywhere automatically.
/// DEBUG_ASSERTs on registry miss so a future rename that drops a
/// slot trips loudly during testing instead of silently turning a
/// `json_double_or(*stats_obj, "?", 0.0)` lookup into a
/// constant-zero false-positive.
[[nodiscard]] inline std::string_view aggregated_stat_name(
    double AggregateStats::* slot) noexcept
{
    for (const auto& entry : kAggregatedStatsRegistry) {
        if (entry.slot == slot) return entry.name;
    }
    DEBUG_ASSERT(false,
        "aggregated_stat_name: slot is not registered in kAggregatedStatsRegistry");
    return std::string_view{};
}

/// Copy of `samples` containing only finite entries (NaN / ±inf
/// dropped). Single source of truth for the V17 finite-only
/// invariant: any path that reduces a raw sample vector — the
/// per-rule comparator overload of `aggregate_samples`, the
/// report-time aggregator `aggregate_samples_for_report` — funnels
/// through this helper so the two interpretations stay aligned
/// byte-for-byte on the finite subset.
[[nodiscard]] inline std::vector<double> filter_finite(
    const std::vector<double>& samples) noexcept
{
    std::vector<double> finite;
    finite.reserve(samples.size());
    for (const double value : samples) {
        if (std::isfinite(value)) finite.push_back(value);
    }
    return finite;
}

/// Read the requested aggregate from a *raw sample vector*. Non-
/// finite samples (NaN / ±inf) are filtered out before any
/// arithmetic, matching `aggregate_samples_for_report` so a rule
/// that runs against the raw vector and a rule that runs against
/// the persisted aggregate produce the same number on the same
/// in-memory data.
///
/// Used by:
///   * per-rule comparators inside one run (LHS / RHS aggregates);
///   * the regression evaluator's *current-run* side (the
///     historical side reads via the AggregatedSamples overload).
///
/// Returns 0.0 when no finite samples remain. Sum/Min/Max take a
/// fast path that skips the filter copy when every element is
/// already finite (the common case); the percentile path always
/// needs a mutable copy for `nth_element`.
[[nodiscard]] inline double aggregate_samples(
    Aggregate aggregate, const std::vector<double>& samples) noexcept
{
    if (samples.empty()) return 0.0;
    const bool all_finite = std::all_of(samples.begin(), samples.end(),
        [](double value) { return std::isfinite(value); });
    if (all_finite && (aggregate == Aggregate::Sum
                        || aggregate == Aggregate::Min
                        || aggregate == Aggregate::Max))
    {
        switch (aggregate) {
            case Aggregate::Sum: {
                double total = 0.0;
                for (const double value : samples) total += value;
                return total;
            }
            case Aggregate::Min: return *std::min_element(samples.begin(), samples.end());
            case Aggregate::Max: return *std::max_element(samples.begin(), samples.end());
            default: break;     // unreachable (gated above)
        }
    }
    std::vector<double> finite = all_finite ? samples : filter_finite(samples);
    if (finite.empty()) return 0.0;
    switch (aggregate) {
        case Aggregate::Sum: {
            double total = 0.0;
            for (const double value : finite) total += value;
            return total;
        }
        case Aggregate::Min:
            return *std::min_element(finite.begin(), finite.end());
        case Aggregate::Max:
            return *std::max_element(finite.begin(), finite.end());
        case Aggregate::P01:
        case Aggregate::P50:
        case Aggregate::P99: {
            // Discrete-index percentile: pick the sample at
            // `floor(p * (n - 1))` after a partial sort via
            // std::nth_element. Matches NumPy's "lower"
            // interpolation mode and the offline aggregator in
            // `aggregate_samples_for_report`.
            const double percentile_fraction =
                aggregate_percentile(aggregate);
            const std::size_t target_index = static_cast<std::size_t>(
                std::floor(percentile_fraction
                           * static_cast<double>(finite.size() - 1)));
            std::nth_element(finite.begin(),
                             finite.begin() + static_cast<std::ptrdiff_t>(target_index),
                             finite.end());
            return finite[target_index];
        }
    }
    return 0.0;
}

/// Read the requested aggregate from a *pre-aggregated record*.
/// Used by the regression evaluator + summary mode + interactive
/// UI when consuming historical reports (which carry only the
/// aggregate, never the raw samples). Returns 0.0 on
/// default-constructed input (`samples_count == 0`) AND on
/// all-non-finite cells (`samples_count > 0 && top10.empty()`),
/// because the writer leaves every aggregate field at its
/// default-zero in that case. Callers that need to distinguish
/// "no real data" from "an aggregate that genuinely equals 0"
/// should gate on `has_finite_samples` first.
[[nodiscard]] inline double aggregate_samples(
    Aggregate aggregate, const AggregatedSamples& aggregated) noexcept
{
    if (aggregated.samples_count == 0) return 0.0;
    switch (aggregate) {
        case Aggregate::Sum: return aggregated.stats.sum;
        case Aggregate::Min: return aggregated.stats.min;
        case Aggregate::Max: return aggregated.stats.max;
        case Aggregate::P01: return aggregated.percentiles.p01;
        case Aggregate::P50: return aggregated.percentiles.p50;
        case Aggregate::P99: return aggregated.percentiles.p99;
    }
    return 0.0;
}

/// Build the `AggregatedSamples` view of a raw sample vector.
/// Filters non-finite samples (NaN / ±inf) before any
/// arithmetic. `samples_count` is the original (pre-filter) size
/// — downstream consumers use it for the user-visible iteration
/// tally; finite-only fields use the filtered subset.
///
/// Pathological inputs (e.g. a billion 1e300 samples that
/// over/underflow `mean` to ±inf) are still possible. The writer
/// pipes every emitted double through `sanitize_for_json`
/// downstream so the on-disk form remains JSON-legal.
[[nodiscard]] inline AggregatedSamples aggregate_samples_for_report(
    const std::vector<double>& samples) noexcept
{
    AggregatedSamples out{};
    out.samples_count = samples.size();
    if (samples.empty()) return out;

    std::vector<double> finite = filter_finite(samples);
    if (finite.empty()) {
        // All-non-finite: leave every aggregate field at its
        // default (zero / empty). The schema's `top10` /
        // `bottom10` use `minItems: 0` so empty arrays validate;
        // the histogram array carries 10 default-constructed
        // buckets (`count == 0`) which the schema accepts.
        return out;
    }

    std::sort(finite.begin(), finite.end());
    out.stats.min = finite.front();
    out.stats.max = finite.back();
    double sum = 0.0;
    for (const double value : finite) sum += value;
    out.stats.sum  = sum;
    out.stats.mean = sum / static_cast<double>(finite.size());

    // mean90 — drop floor(10%) on each side. For finite_count >= 1,
    // `trim * 2 < finite_count` is always true, so the inner sum
    // is well-defined. When trim == 0 the loop covers the whole
    // vector and mean90 == mean by construction.
    {
        const auto trim = static_cast<std::size_t>(
            std::floor(0.1 * static_cast<double>(finite.size())));
        double trimmed = 0.0;
        for (std::size_t i = trim; i < finite.size() - trim; ++i) {
            trimmed += finite[i];
        }
        const auto kept = finite.size() - 2 * trim;
        out.stats.mean90 = trimmed / static_cast<double>(kept);
    }

    // stddev — only meaningful for n >= 2 AND finite mean. A
    // non-finite mean (sum overflowed) would make (s - mean)²
    // produce NaN and silently wreck the result; bail to 0.
    if (finite.size() >= 2 && std::isfinite(out.stats.mean)) {
        double sq = 0.0;
        for (const double value : finite) {
            const double delta = value - out.stats.mean;
            sq += delta * delta;
        }
        out.stats.stddev =
            std::sqrt(sq / static_cast<double>(finite.size() - 1));
    }

    for (const auto& entry : kAggregatedPercentileRegistry) {
        const auto idx = static_cast<std::size_t>(
            std::floor(entry.fraction
                       * static_cast<double>(finite.size() - 1)));
        out.percentiles.*(entry.slot) = finite[idx];
    }

    const auto kbest = std::min(kKBestSize, finite.size());
    out.top10.reserve(kbest);
    out.bottom10.reserve(kbest);
    for (std::size_t i = 0; i < kbest; ++i) {
        out.top10.push_back(finite[finite.size() - 1 - i]);
        out.bottom10.push_back(finite[i]);
    }

    // Histogram: linear binning, last bucket inclusive on the
    // right so `max` lands inside it. When `width == 0` (all
    // finite samples equal, or n == 1) every count goes to bucket 0.
    // `std::isfinite(width)` guards the pathological max - min == ±inf
    // case (e.g. samples include `numeric_limits<double>::max()`):
    // a non-finite width would produce `(value - min) / inf == NaN`,
    // and `static_cast<size_t>(NaN)` is UB.
    const double width = (out.stats.max - out.stats.min)
                         / static_cast<double>(kHistogramBucketCount);
    for (std::size_t i = 0; i < kHistogramBucketCount; ++i) {
        out.histogram[i].from = out.stats.min + width * static_cast<double>(i);
        out.histogram[i].to   = (i + 1 == kHistogramBucketCount)
            ? out.stats.max
            : (out.stats.min + width * static_cast<double>(i + 1));
    }
    if (std::isfinite(width) && width > 0.0) {
        for (const double value : finite) {
            auto bucket = static_cast<std::size_t>(
                std::floor((value - out.stats.min) / width));
            if (bucket >= kHistogramBucketCount) bucket = kHistogramBucketCount - 1;
            out.histogram[bucket].count += 1;
        }
    } else {
        out.histogram[0].count = finite.size();
    }
    return out;
}

/// Map any double to a JSON-legal value. `NaN` becomes 0.0;
/// `±infinity` clamp to `numeric_limits<double>::max() / lowest()`.
/// JSON (RFC 8259) does not admit `NaN` or `Infinity` tokens, so
/// emitting them would produce strings that
/// `nlohmann::json::parse` (used by the framework's post-write
/// self-validation) and any standard parser reject. This helper
/// is applied at every numeric-emit site inside the writer so
/// pathological-input runs still produce a valid report.
[[nodiscard]] inline double sanitize_for_json(double value) noexcept {
    if (std::isnan(value)) return 0.0;
    if (value ==  std::numeric_limits<double>::infinity()) {
        return  std::numeric_limits<double>::max();
    }
    if (value == -std::numeric_limits<double>::infinity()) {
        return  std::numeric_limits<double>::lowest();
    }
    return value;
}

// ----------------- Rule evaluation -----------------

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

/// Apply a comparator to two aggregate values with explicit threshold /
/// tolerance percentages. Spec semantics:
/// - `<` : `lhs < rhs * (1 - threshold% + tolerance%)`
/// - `>` : `lhs > rhs * (1 + threshold% - tolerance%)` (extrapolated by
///         symmetry; spec only documents `<`/`=`)
/// - `=` : `|lhs - rhs| <= rhs * tolerance%` (asymmetric on rhs as the
///         baseline reference, matching the spec example).
///
/// Used by both per-rule comparisons (with both knobs from
/// `RuleComparator`) and regression rules (which always pass
/// `threshold_pct = 0` because the schema forbids the field).
[[nodiscard]] inline bool comparator_passes(
    CompareOp op,
    double lhs_aggregate, double rhs_aggregate,
    double threshold_pct, double tolerance_pct) noexcept
{
    const double threshold = threshold_pct / 100.0;
    const double tolerance = tolerance_pct / 100.0;
    switch (op) {
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

/// Convenience overload for the per-rule comparator path. Forwards into
/// the explicit-args overload so `RuleComparator` and `RegressionOperator`
/// share one implementation.
[[nodiscard]] inline bool comparator_passes(
    const RuleComparator& cmp,
    double lhs_aggregate,
    double rhs_aggregate) noexcept
{
    return comparator_passes(cmp.op, lhs_aggregate, rhs_aggregate,
                              cmp.threshold_pct, cmp.tolerance_pct);
}

/// Convenience overload for the regression-rule path. Threshold is
/// implicitly zero — the regression schema rejects an explicit
/// `threshold%` field, so there's no value to forward.
[[nodiscard]] inline bool comparator_passes(
    const RegressionOperator& op,
    double now_value,
    double previously_value) noexcept
{
    return comparator_passes(op.op, now_value, previously_value,
                              /*threshold_pct=*/0.0, op.tolerance_pct);
}

/// Per-cell map shape: `region -> metric -> samples`. Reused by the
/// writer and rule evaluator to expose group-merged samples.
using SampleMap = std::map<std::string, std::map<std::string, std::vector<double>>>;

/// Concatenate every group cell's `samples` map into one `SampleMap`.
/// Sample arrays append in cell-declaration order (= group_index order),
/// so the result is deterministic. Empty input yields an empty map.
[[nodiscard]] inline SampleMap merge_group_samples(
    std::span<const Cell* const> group_cells)
{
    SampleMap merged;
    for (const auto* cell_ptr : group_cells) {
        if (cell_ptr == nullptr) continue;
        for (const auto& [region_name, metric_map] : cell_ptr->samples) {
            auto& target_region = merged[region_name];
            for (const auto& [metric_name, samples] : metric_map) {
                auto& target_metric = target_region[metric_name];
                target_metric.insert(target_metric.end(),
                                      samples.begin(), samples.end());
            }
        }
    }
    return merged;
}

/// `(tuple, test_case_name)` -> merged samples view of one benchmark
/// state. Built once per consumer; shared by `evaluate_rules`,
/// `evaluate_regressions`, and the report writer so all three see the
/// same group-merged numbers (reading multiple group cells separately
/// would skew aggregates and Cartesian-product rule evaluations).
using MergedSamplesByPair =
    std::map<std::pair<const ExecutionTuple*, std::string>, SampleMap>;

[[nodiscard]] inline MergedSamplesByPair merged_samples_for_state(
    const BenchmarkExecutionState& state)
{
    std::map<std::pair<const ExecutionTuple*, std::string>,
             std::vector<const Cell*>> by_pair;
    for (const auto& cell_ptr : state.cells) {
        by_pair[{cell_ptr->tuple, cell_ptr->test_case->name}]
            .push_back(cell_ptr.get());
    }
    MergedSamplesByPair merged;
    for (auto& [pair_key, cells_for_pair] : by_pair) {
        merged[pair_key] = merge_group_samples(
            std::span<const Cell* const>(
                cells_for_pair.data(), cells_for_pair.size()));
    }
    return merged;
}

/// Look up `region_name -> metric_name -> samples` inside a merged
/// view. Returns nullptr when any link in the chain is missing.
[[nodiscard]] inline const std::vector<double>* fetch_merged_samples(
    const MergedSamplesByPair& merged,
    const ExecutionTuple* tuple,
    const std::string& test_case_name,
    const std::string& region_name,
    const std::string& metric_name) noexcept
{
    const auto pair_it = merged.find({tuple, test_case_name});
    if (pair_it == merged.end()) return nullptr;
    const auto region_it = pair_it->second.find(region_name);
    if (region_it == pair_it->second.end()) return nullptr;
    const auto metric_it = region_it->second.find(metric_name);
    if (metric_it == region_it->second.end()) return nullptr;
    return &metric_it->second;
}

/// Apply every rule against every test case and collect failures. With
/// V11's per-group cells, samples are merged once per `(tuple,
/// test_case)` *before* the rule loop so LHS/RHS aggregates are computed
/// from the same merged sample arrays the report writer will emit.
inline void evaluate_rules(const BenchmarkExecutionState& state,
                            std::vector<Violation>& violations)
{
    const auto merged = merged_samples_for_state(state);

    for (const auto& metric : state.config.metrics) {
        for (const auto& rule : metric.rules) {
            for (const auto& [test_case_name, _iterations] : state.config.for_each) {
                // Collect tuple* on each side so we evaluate the rule
                // exactly once per (LHS_tuple, RHS_tuple) pair, never
                // duplicated across groups.
                std::vector<const ExecutionTuple*> lhs_tuples;
                std::vector<const ExecutionTuple*> rhs_tuples;
                for (const auto& tuple : state.execution_tuples) {
                    if (selector_matches(rule.lhs, tuple)) lhs_tuples.push_back(&tuple);
                    if (selector_matches(rule.rhs, tuple)) rhs_tuples.push_back(&tuple);
                }
                for (const auto* lhs_tuple : lhs_tuples) {
                    for (const auto* rhs_tuple : rhs_tuples) {
                        const auto* lhs_samples = fetch_merged_samples(
                            merged, lhs_tuple, test_case_name,
                            "all_regions", rule.metric_name);
                        const auto* rhs_samples = fetch_merged_samples(
                            merged, rhs_tuple, test_case_name,
                            "all_regions", rule.metric_name);
                        if (lhs_samples == nullptr || rhs_samples == nullptr) continue;
                        const double lhs_aggregate = aggregate_samples(
                            rule.cmp.aggregate, *lhs_samples);
                        const double rhs_aggregate = aggregate_samples(
                            rule.cmp.aggregate, *rhs_samples);
                        if (comparator_passes(rule.cmp, lhs_aggregate, rhs_aggregate)) continue;

                        Violation violation;
                        violation.config_path     = state.config.config_path;
                        violation.benchmark_key   = state.config.benchmark_key;
                        violation.rule_name       = rule.name;
                        violation.test_case       = test_case_name;
                        violation.metric          = rule.metric_name;
                        violation.lhs_algorithm   = lhs_tuple->algorithm_name;
                        violation.lhs_full_params = lhs_tuple->params;
                        violation.rhs_algorithm   = rhs_tuple->algorithm_name;
                        violation.rhs_full_params = rhs_tuple->params;
                        violation.args            = {lhs_aggregate, rhs_aggregate};
                        violations.push_back(std::move(violation));
                    }
                }
            }
        }
    }
}

// ----------------- Run identity (meta block) -----------------

/// Read the trimmed first 64 bytes of `command`'s stdout via popen.
/// Returns "" on any failure (popen null, exit status nonzero, empty
/// output). Used by `git_head_commit` and `git_working_tree_dirty` to
/// shell out to the local git client without pulling in libgit2.
///
/// **Trust note.** PATH-hijacking is theoretically possible (a
/// malicious `git` earlier in `$PATH` would run as the bench process).
/// The framework runs in developer environments only; we treat this as
/// the same trust boundary as any other dev tool that shells out to
/// `git`. No user input flows into the command string, so there is no
/// injection vector.
[[nodiscard]] inline std::string slurp_command_stdout(const char* command) noexcept {
    FILE* pipe = ::popen(command, "r");
    if (pipe == nullptr) return {};
    std::array<char, 256> buffer{};
    std::string out;
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        out.append(buffer.data());
        if (out.size() > 4096) break;     // sanity cap; we only want a SHA or a short status
    }
    const int exit_code = ::pclose(pipe);
    if (exit_code != 0) return {};
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
        out.pop_back();
    }
    return out;
}

/// Returns the working-tree HEAD commit, or "" if the cwd isn't a git
/// repo (or git isn't on PATH).
[[nodiscard]] inline std::string git_head_commit() noexcept {
    return slurp_command_stdout("git rev-parse HEAD 2>/dev/null");
}

/// Returns true iff the working tree has any uncommitted changes
/// (tracked-modified, staged, or untracked files visible to
/// `git status --porcelain`). Returns false on any error so we don't
/// spuriously flag a missing-git environment as dirty.
[[nodiscard]] inline bool git_working_tree_dirty() noexcept {
    const auto output = slurp_command_stdout("git status --porcelain 2>/dev/null");
    return !output.empty();
}

/// Capture this run's identity in a single struct. Called once from
/// `run_impl`; the resulting `ReportMeta` is forwarded to both the
/// report writer (for the `meta` block) and the regression evaluator
/// (which doesn't need most fields, but takes the whole struct for
/// future-proofing).
[[nodiscard]] inline ReportMeta capture_run_meta(bool fast_filter) noexcept {
    ReportMeta meta;
    meta.commit = git_head_commit();
    meta.dirty  = git_working_tree_dirty();
    meta.fast   = fast_filter;
    const auto now = std::chrono::system_clock::now();
    meta.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    const std::time_t now_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    ::gmtime_r(&now_time_t, &tm_buf);
    std::array<char, 32> created_buf{};
    const auto written = std::strftime(
        created_buf.data(), created_buf.size(),
        "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    meta.created.assign(created_buf.data(), written);
    return meta;
}

// ----------------- Regression evaluation -----------------

/// Pin-allocated wrapper around a parsed historical report. Holds the
/// simdjson parser, the report body string, and the parsed root element.
/// The body must outlive the parser; storing both inline guarantees this
/// regardless of how the wrapper is moved (it isn't — see deleted move
/// ctors). Heap allocation via `std::unique_ptr` (in the cache below)
/// gives stable pointers.
struct ParsedHistoricalReport {
    std::string                body;
    simdjson::dom::parser      parser;
    simdjson::dom::element     root;
    std::size_t                index = 0;

    ParsedHistoricalReport() = default;
    ParsedHistoricalReport(const ParsedHistoricalReport&) = delete;
    ParsedHistoricalReport& operator=(const ParsedHistoricalReport&) = delete;
    ParsedHistoricalReport(ParsedHistoricalReport&&) = delete;
    ParsedHistoricalReport& operator=(ParsedHistoricalReport&&) = delete;
};

/// Resolve the path of `report_<index>.json` inside `report_dir`.
[[nodiscard]] inline std::filesystem::path report_path_for_index(
    const std::filesystem::path& report_dir, std::size_t index) noexcept
{
    return report_dir / fmt::format("report_{}.json", index);
}

/// Scan `report_dir` for files matching `^report_(\d+)\.json$`. Returns
/// the largest index found, or `std::nullopt` when none exist (or the
/// directory itself is missing — first run on a fresh report path).
[[nodiscard]] inline std::optional<std::size_t> max_existing_report_index(
    const std::filesystem::path& report_dir) noexcept
{
    std::error_code ec;
    if (!std::filesystem::exists(report_dir, ec) || ec) return std::nullopt;
    std::optional<std::size_t> max_index;
    for (const auto& entry : std::filesystem::directory_iterator(report_dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec) || ec) continue;
        const auto filename = entry.path().filename().string();
        constexpr std::string_view kPrefix = "report_";
        constexpr std::string_view kSuffix = ".json";
        if (filename.size() <= kPrefix.size() + kSuffix.size()) continue;
        if (filename.compare(0, kPrefix.size(), kPrefix.data(), kPrefix.size()) != 0) continue;
        if (filename.compare(filename.size() - kSuffix.size(),
                              kSuffix.size(),
                              kSuffix.data(), kSuffix.size()) != 0) continue;
        const auto digits = filename.substr(
            kPrefix.size(),
            filename.size() - kPrefix.size() - kSuffix.size());
        if (digits.empty()) continue;
        bool digits_only = true;
        for (const char ch : digits) {
            if (!std::isdigit(static_cast<unsigned char>(ch))) {
                digits_only = false; break;
            }
        }
        if (!digits_only) continue;
        // `std::stoull` may throw on overflow; we don't use exceptions, so
        // accumulate manually with overflow guard.
        std::size_t parsed = 0;
        bool overflowed = false;
        for (const char ch : digits) {
            const std::size_t digit = static_cast<std::size_t>(ch - '0');
            if (parsed > (std::numeric_limits<std::size_t>::max() - digit) / 10) {
                overflowed = true; break;
            }
            parsed = parsed * 10 + digit;
        }
        if (overflowed) continue;
        if (!max_index.has_value() || parsed > *max_index) {
            max_index = parsed;
        }
    }
    return max_index;
}

/// Load and parse `report_<index>.json` from disk. Returns nullptr when
/// the file is missing or the JSON body fails to parse — both of which
/// are downgraded to "history gap" by the regression evaluator.
[[nodiscard]] inline std::unique_ptr<ParsedHistoricalReport> load_report_at_index(
    const std::filesystem::path& report_dir, std::size_t index) noexcept
{
    const auto path = report_path_for_index(report_dir, index);
    auto body = slurp_file(path);
    if (body.empty()) return nullptr;
    auto holder = std::make_unique<ParsedHistoricalReport>();
    holder->index = index;
    holder->body  = std::move(body);
    if (holder->parser.parse(holder->body).get(holder->root) != simdjson::SUCCESS) {
        return nullptr;
    }
    return holder;
}

/// Compare a JSON value (from a parsed historical report) against a
/// `ConfigValue` (from the current run's tuple). Numbers are matched
/// across the integer/double divide so a round-trip like 10 → 10.0 → 10
/// still compares equal.
[[nodiscard]] inline bool simdjson_equals_config_value(
    simdjson::dom::element json_value, const ConfigValue& cv) noexcept
{
    switch (cv.kind) {
        case ConfigValue::Kind::String: {
            std::string_view sv;
            return json_value.get(sv) == simdjson::SUCCESS && sv == cv.string_value;
        }
        case ConfigValue::Kind::Integer: {
            std::int64_t i_value = 0;
            if (json_value.get(i_value) == simdjson::SUCCESS) {
                return i_value == cv.integer_value;
            }
            std::uint64_t u_value = 0;
            if (json_value.get(u_value) == simdjson::SUCCESS) {
                return static_cast<std::int64_t>(u_value) == cv.integer_value;
            }
            double d_value = 0.0;
            if (json_value.get(d_value) == simdjson::SUCCESS) {
                return d_value == static_cast<double>(cv.integer_value);
            }
            return false;
        }
        case ConfigValue::Kind::Double: {
            double d_value = 0.0;
            if (json_value.get(d_value) == simdjson::SUCCESS) {
                return d_value == cv.double_value;
            }
            std::int64_t i_value = 0;
            if (json_value.get(i_value) == simdjson::SUCCESS) {
                return static_cast<double>(i_value) == cv.double_value;
            }
            return false;
        }
        case ConfigValue::Kind::Boolean: {
            bool b_value = false;
            return json_value.get(b_value) == simdjson::SUCCESS
                   && b_value == cv.boolean_value;
        }
        case ConfigValue::Kind::NonScalar:
            return false;
    }
    return false;
}

/// Try to coerce a simdjson element to a JSON object. Returns nullopt
/// when the cast fails. Used by the report walker so the per-step
/// boilerplate (`if (... .get(out) != SUCCESS) return nullopt;`) collapses
/// to a single line per descent.
[[nodiscard]] inline std::optional<simdjson::dom::object>
try_as_object(simdjson::dom::element element) noexcept {
    simdjson::dom::object out;
    if (element.get(out) != simdjson::SUCCESS) return std::nullopt;
    return out;
}

/// Helper for the same pattern at object-key entry points.
[[nodiscard]] inline std::optional<simdjson::dom::object>
try_get_object(simdjson::dom::object parent, std::string_view key) noexcept {
    simdjson::dom::element child;
    if (parent.at_key(key).get(child) != simdjson::SUCCESS) return std::nullopt;
    return try_as_object(child);
}

/// Pull the `meta.commit` string from a parsed report's root object.
/// Returns "" when the report has no `meta` block (pre-V5 format), no
/// `commit` field inside `meta`, or the field isn't a string. Used by
/// the regression evaluator to populate `regression_since_commit`.
[[nodiscard]] inline std::string commit_from_report(
    simdjson::dom::element report_root) noexcept
{
    const auto root_obj = try_as_object(report_root);
    if (!root_obj.has_value()) return {};
    const auto meta_obj = try_get_object(*root_obj, "meta");
    if (!meta_obj.has_value()) return {};
    std::string_view commit_text;
    if ((*meta_obj).at_key("commit").get(commit_text) != simdjson::SUCCESS) {
        return {};
    }
    return std::string{commit_text};
}

/// Read a JSON `number` field as a double, falling back to
/// `default_value` on any miss (key absent, wrong type, parse
/// failure). Coerces through `int64_t` / `uint64_t` so an
/// integer-spelled JSON value like `0` still resolves cleanly.
/// Tiny helper so every aggregate-field reader below stays one
/// line; consolidated in one place so its semantics (e.g. the
/// integer-coercion order) are auditable.
[[nodiscard]] inline double json_double_or(
    simdjson::dom::object obj, std::string_view key, double default_value) noexcept
{
    simdjson::dom::element child;
    if (obj.at_key(key).get(child) != simdjson::SUCCESS) return default_value;
    double parsed_double = 0.0;
    if (child.get(parsed_double) == simdjson::SUCCESS) return parsed_double;
    std::int64_t parsed_int = 0;
    if (child.get(parsed_int) == simdjson::SUCCESS) {
        return static_cast<double>(parsed_int);
    }
    std::uint64_t parsed_uint = 0;
    if (child.get(parsed_uint) == simdjson::SUCCESS) {
        return static_cast<double>(parsed_uint);
    }
    return default_value;
}

/// Read a JSON `integer` field as `int64_t` with the same
/// graceful-degradation contract as `json_double_or`.
[[nodiscard]] inline std::int64_t json_int_or(
    simdjson::dom::object obj, std::string_view key, std::int64_t default_value) noexcept
{
    simdjson::dom::element child;
    if (obj.at_key(key).get(child) != simdjson::SUCCESS) return default_value;
    std::int64_t parsed_int = 0;
    if (child.get(parsed_int) == simdjson::SUCCESS) return parsed_int;
    std::uint64_t parsed_uint = 0;
    if (child.get(parsed_uint) == simdjson::SUCCESS) {
        return static_cast<std::int64_t>(parsed_uint);
    }
    return default_value;
}

/// Parse one `region_block.<metric>` object into an
/// `AggregatedSamples`. Missing optional sub-objects collapse to
/// default-constructed values; consumers that need the full shape
/// (sanity warnings, regression eval, --ui) check
/// `samples_count > 0` and `top10.empty()` upfront.
[[nodiscard]] inline AggregatedSamples parse_aggregated_samples(
    simdjson::dom::object metric_obj) noexcept
{
    AggregatedSamples agg{};
    agg.samples_count = static_cast<std::size_t>(std::max<std::int64_t>(
        0, json_int_or(metric_obj, "samples_count", 0)));
    if (const auto stats_obj = try_get_object(metric_obj, "stats");
        stats_obj.has_value())
    {
        for (const auto& entry : kAggregatedStatsRegistry) {
            agg.stats.*(entry.slot) = json_double_or(*stats_obj, entry.name, 0.0);
        }
    }
    if (const auto pct_obj = try_get_object(metric_obj, "percentiles");
        pct_obj.has_value())
    {
        for (const auto& entry : kAggregatedPercentileRegistry) {
            agg.percentiles.*(entry.slot) =
                json_double_or(*pct_obj, entry.name, 0.0);
        }
    }
    /// Pull `parent[key]` as a `vector<double>`. Non-numeric items
    /// (corrupt historical reports) are dropped silently — the
    /// resulting array may shrink below the source length, breaking
    /// the documented `top10[0] == stats.max` invariant of an
    /// aggregator-built `AggregatedSamples`. Consumers that depend
    /// on exact alignment with the source must check
    /// `top10.size() == bottom10.size()` themselves; the regression
    /// evaluator and the interactive UI only consult percentile
    /// slots, so a truncated `top10` is harmless to them.
    auto parse_double_array = [](simdjson::dom::object parent,
                                  std::string_view key) noexcept
        -> std::vector<double>
    {
        std::vector<double> out;
        simdjson::dom::array arr;
        if (parent.at_key(key).get(arr) != simdjson::SUCCESS) return out;
        out.reserve(arr.size());
        for (auto value_elem : arr) {
            double parsed = 0.0;
            if (value_elem.get(parsed) == simdjson::SUCCESS) {
                out.push_back(parsed);
            }
        }
        return out;
    };
    if (const auto kbest_obj = try_get_object(metric_obj, "k-best");
        kbest_obj.has_value())
    {
        agg.top10    = parse_double_array(*kbest_obj, "top10");
        agg.bottom10 = parse_double_array(*kbest_obj, "bottom10");
        // The aggregator's invariant is `top10.size() == bottom10.size()`
        // (both are `min(10, finite_count)`). If a corrupt historical
        // report drops elements asymmetrically, truncate both sides
        // back to the shorter length so consumers never see a
        // size-mismatch (which would, e.g., let a future
        // "spread = top10[i] - bottom10[i]" walk read past the end).
        const auto kbest_paired = std::min(agg.top10.size(), agg.bottom10.size());
        agg.top10.resize(kbest_paired);
        agg.bottom10.resize(kbest_paired);
    }
    simdjson::dom::array hist_arr;
    if (metric_obj["histogram"].get(hist_arr) == simdjson::SUCCESS) {
        // Histogram positional alignment matters: bucket `i` in the
        // source becomes bucket `i` in `agg.histogram`. We advance
        // `bucket_index` unconditionally so a malformed bucket leaves
        // its slot at default-construction (`from == to == 0`,
        // `count == 0`) and later slots stay aligned with the source.
        std::size_t bucket_index = 0;
        for (auto bucket_elem : hist_arr) {
            if (bucket_index >= kHistogramBucketCount) break;
            if (const auto bucket_obj = try_as_object(bucket_elem);
                bucket_obj.has_value())
            {
                agg.histogram[bucket_index].from  = json_double_or(*bucket_obj, "from", 0.0);
                agg.histogram[bucket_index].to    = json_double_or(*bucket_obj, "to",   0.0);
                agg.histogram[bucket_index].count = static_cast<std::size_t>(
                    std::max<std::int64_t>(0, json_int_or(*bucket_obj, "count", 0)));
            }
            ++bucket_index;
        }
    }
    return agg;
}

/// Walk a parsed report's
/// `benchmarks.<bench>.execution_tuples[*].test_cases.<tc>.<region>.<metric>`
/// path and pull out the pre-aggregated record. Returns nullopt
/// on any miss (benchmark absent, no matching tuple, region
/// missing, etc.) — the regression evaluator and the interactive
/// UI both treat this as "no historical baseline for this
/// combo". V17: replaces the pre-V17 `samples_from_report` that
/// returned a raw sample vector.
[[nodiscard]] inline std::optional<AggregatedSamples> aggregated_from_report(
    simdjson::dom::element report_root,
    std::string_view benchmark,
    std::string_view algorithm_name,
    const ParamMap&  params,
    std::string_view test_case,
    std::string_view region,
    std::string_view metric)
{
    const auto root_obj = try_as_object(report_root);
    if (!root_obj.has_value()) return std::nullopt;
    const auto benchmarks_obj = try_get_object(*root_obj, "benchmarks");
    if (!benchmarks_obj.has_value()) return std::nullopt;
    const auto bench_obj = try_get_object(*benchmarks_obj, benchmark);
    if (!bench_obj.has_value()) return std::nullopt;

    simdjson::dom::array execution_tuples;
    if ((*bench_obj)["execution_tuples"].get(execution_tuples) != simdjson::SUCCESS) {
        return std::nullopt;
    }

    for (auto tuple_record : execution_tuples) {
        const auto record_obj = try_as_object(tuple_record);
        if (!record_obj.has_value()) continue;
        const auto tuple_inner = try_get_object(*record_obj, "tuple");
        if (!tuple_inner.has_value()) continue;
        const auto algo_params_obj = try_get_object(*tuple_inner, algorithm_name);
        if (!algo_params_obj.has_value()) continue;

        bool param_match = true;
        for (const auto& [param_name, param_value] : params) {
            simdjson::dom::element json_param_value;
            if ((*algo_params_obj).at_key(param_name).get(json_param_value)
                != simdjson::SUCCESS) {
                param_match = false; break;
            }
            if (!simdjson_equals_config_value(json_param_value, param_value)) {
                param_match = false; break;
            }
        }
        if (!param_match) continue;

        const auto test_cases_obj = try_get_object(*record_obj, "test_cases");
        if (!test_cases_obj.has_value()) continue;
        const auto test_case_obj = try_get_object(*test_cases_obj, test_case);
        if (!test_case_obj.has_value()) continue;
        const auto region_obj = try_get_object(*test_case_obj, region);
        if (!region_obj.has_value()) continue;

        simdjson::dom::object metric_obj;
        if ((*region_obj).at_key(metric).get(metric_obj) != simdjson::SUCCESS) continue;

        return parse_aggregated_samples(metric_obj);
    }
    return std::nullopt;
}

/// Lazy parser-cache for historical reports. Each `report_<N>.json`
/// is parsed at most once per cache instance even if multiple
/// consumers (regressions, sanity warnings) ask for the same index.
/// `ParsedHistoricalReport` is held via `unique_ptr` because it pins
/// (the inner simdjson parser is non-movable). Stack-allocated;
/// freed at the end of `run_impl`'s evaluation phase.
struct HistoricalReportCache {
    std::filesystem::path                                                report_dir;
    std::map<std::size_t, std::unique_ptr<ParsedHistoricalReport>>      cached{};
    std::set<std::size_t>                                                known_missing{};

    explicit HistoricalReportCache(std::filesystem::path dir) noexcept
        : report_dir(std::move(dir)) {}

    HistoricalReportCache(const HistoricalReportCache&) = delete;
    HistoricalReportCache& operator=(const HistoricalReportCache&) = delete;
    HistoricalReportCache(HistoricalReportCache&&) = delete;
    HistoricalReportCache& operator=(HistoricalReportCache&&) = delete;

    /// Fetch (loading on demand) the report at `index`. Returns nullptr
    /// once for a missing report and remembers that miss so subsequent
    /// fetches don't re-touch the filesystem.
    [[nodiscard]] ParsedHistoricalReport* fetch(std::size_t index) noexcept {
        if (known_missing.count(index) > 0) return nullptr;
        if (auto it = cached.find(index); it != cached.end()) {
            return it->second.get();
        }
        auto loaded = load_report_at_index(report_dir, index);
        if (loaded == nullptr) {
            known_missing.insert(index);
            return nullptr;
        }
        auto* raw = loaded.get();
        cached.emplace(index, std::move(loaded));
        return raw;
    }
};

/// Bit-exact identity for a `double`. Uses `std::bit_cast<uint64_t>`
/// so that NaN, ±0, and ±infinity hash consistently in the
/// distinct-value sets used by the sanity-warning detector. Plain
/// `double ==` would treat every NaN as never-equal-to-itself,
/// silencing a "metric always returns NaN" warning — which is the
/// opposite of what the user wants.
[[nodiscard]] inline std::uint64_t double_bits(double sample) noexcept {
    return std::bit_cast<std::uint64_t>(sample);
}

/// Walk an entire historical report's
/// `benchmarks.<bench>.execution_tuples[*].test_cases.<tc>.<region>.<metric_name>`
/// tree and collect a distinct-value set capturing every sample
/// seen for `metric_name`, *as observed through the V17 aggregate
/// shape*. Returns an empty set when the metric was absent in that
/// report (or when the report's structure is unparseable). Linear
/// in the report size; called at most once per `(metric,
/// hist_report)` pair via the per-rule cache in
/// `evaluate_sanity_warnings`.
///
/// The aggregate per cell carries `min` and `max` (over the finite
/// subset) plus an empty-vs-non-empty `top10`. From these we can
/// drive the existing detector identically to the pre-V17 raw-
/// vector walk:
///
///   * `samples_count == 0` → cell is empty, contributes nothing.
///   * `top10` empty (`samples_count > 0`) → every emitted sample
///     was non-finite. Insert a NaN sentinel so an all-NaN run
///     reports as `metric_looks_constant` with a NaN-flavoured
///     message — matching pre-V17 semantics where every NaN bit
///     pattern collapsed into one set entry.
///   * `min == max` → every finite sample in this cell shares one
///     value `V`; insert `bit_cast<u64>(V)`.
///   * `min != max` → cell is internally non-constant; insert
///     both bits so the "size > 1 ⇒ not constant" check trips.
[[nodiscard]] inline std::set<std::uint64_t> distinct_sample_bits_for_metric(
    simdjson::dom::element report_root,
    std::string_view       metric_name) noexcept
{
    std::set<std::uint64_t> result;
    const auto root_obj = try_as_object(report_root);
    if (!root_obj.has_value()) return result;
    const auto benchmarks_obj = try_get_object(*root_obj, "benchmarks");
    if (!benchmarks_obj.has_value()) return result;

    for (auto bench_field : *benchmarks_obj) {
        const auto bench_obj = try_as_object(bench_field.value);
        if (!bench_obj.has_value()) continue;
        simdjson::dom::array execution_tuples;
        if ((*bench_obj)["execution_tuples"].get(execution_tuples) != simdjson::SUCCESS) {
            continue;
        }
        for (auto tuple_record : execution_tuples) {
            const auto record_obj = try_as_object(tuple_record);
            if (!record_obj.has_value()) continue;
            const auto test_cases_obj = try_get_object(*record_obj, "test_cases");
            if (!test_cases_obj.has_value()) continue;
            for (auto tc_field : *test_cases_obj) {
                const auto tc_obj = try_as_object(tc_field.value);
                if (!tc_obj.has_value()) continue;
                for (auto region_field : *tc_obj) {
                    const auto region_obj = try_as_object(region_field.value);
                    if (!region_obj.has_value()) continue;
                    simdjson::dom::object metric_obj;
                    if ((*region_obj).at_key(metric_name).get(metric_obj)
                        != simdjson::SUCCESS) continue;
                    if (json_int_or(metric_obj, "samples_count", 0) <= 0) continue;
                    bool top10_empty = true;
                    if (const auto kbest_obj = try_get_object(metric_obj, "k-best");
                        kbest_obj.has_value())
                    {
                        simdjson::dom::array top10_arr;
                        if ((*kbest_obj)["top10"].get(top10_arr) == simdjson::SUCCESS) {
                            top10_empty = (top10_arr.size() == 0);
                        }
                    }
                    if (top10_empty) {
                        result.insert(double_bits(
                            std::numeric_limits<double>::quiet_NaN()));
                        continue;
                    }
                    const auto stats_obj = try_get_object(metric_obj, "stats");
                    if (!stats_obj.has_value()) continue;
                    const double min_value = json_double_or(*stats_obj,
                        aggregated_stat_name(&AggregateStats::min), 0.0);
                    const double max_value = json_double_or(*stats_obj,
                        aggregated_stat_name(&AggregateStats::max), 0.0);
                    result.insert(double_bits(min_value));
                    if (max_value != min_value) {
                        result.insert(double_bits(max_value));
                    }
                }
            }
        }
    }
    return result;
}

/// Format the constant value seen for a `metric_looks_constant`
/// warning. Integer-valued doubles in the safe IEEE range render
/// as `"490"`; everything else uses default `fmt` double formatting.
/// NaN / infinity surface explicitly so the diagnostic reads sensibly.
[[nodiscard]] inline std::string stringify_constant_value(double sample) noexcept {
    if (std::isnan(sample)) return "NaN";
    if (std::isinf(sample)) return sample < 0.0 ? "-infinity" : "infinity";
    if (std::isfinite(sample)
        && sample == std::floor(sample)
        && std::abs(sample) < static_cast<double>(1ULL << 53))
    {
        return fmt::format("{}", static_cast<std::int64_t>(sample));
    }
    return fmt::format("{}", sample);
}

/// Run hardcoded sanity checks against the just-finished run's
/// collected samples and walk history backwards to attribute
/// `warning_since` / `warning_since_commit`. Currently emits
/// `metric_looks_constant` only — the only suspicious-uniformity
/// case the user spec calls out.
///
/// **Detection.** A metric is "constant" when every sample seen
/// across every (benchmark, tuple, test_case, region) cell of the
/// current run hashes to one bit-pattern. Empty samples (metric
/// declared but never emitted) do NOT trip the check — earlier
/// validation already surfaces those (`region_filter_never_matched`,
/// `missing_evaluation_metric`).
///
/// **History walk.** For each constant metric we fetch
/// `report_<N-1>.json`, `report_<N-2>.json`, … and compare the
/// historical report's distinct-sample set with `{V}`. If the
/// historical set is a subset of `{V}`, that older report shares
/// the warning. We stop at the first historical index where the
/// sets diverge, or at index 1 (the warning has been present in
/// every report on disk). Missing reports lock `warning_since` to
/// JSON `null`.
///
/// **First run** (`current_index == 1`): no history is reachable.
/// We emit `warning_since: "report_1.json"` and the current run's
/// own commit; the warning is observable from a single run, unlike
/// regression rules whose entire premise needs a baseline.
///
/// **Non-blocking.** Sanity warnings are surfaced but never affect
/// the framework's exit code. The list is sorted alphabetically by
/// metric name for diff-friendly output.
[[nodiscard]] inline std::vector<SanityWarning> evaluate_sanity_warnings(
    const std::map<std::string, std::unique_ptr<BenchmarkExecutionState>>& reports,
    const ReportMeta&            current_meta,
    std::size_t                  current_index,
    HistoricalReportCache&       history_cache) noexcept
{
    // 1. Collect distinct bit-patterns per metric name across every
    //    cell of the current run. We funnel through
    //    `aggregate_samples_for_report` so the constancy lens
    //    (min/max + NaN-stuck sentinel) matches the V17 historical-
    //    side reader (`distinct_sample_bits_for_metric`) exactly.
    std::map<std::string, std::set<std::uint64_t>> distinct_bits_by_metric;
    for (const auto& [_bench_key, state_ptr] : reports) {
        for (const auto& cell_ptr : state_ptr->cells) {
            for (const auto& [_region_name, region_metrics] : cell_ptr->samples) {
                for (const auto& [metric_name, sample_values] : region_metrics) {
                    if (sample_values.empty()) continue;
                    auto& bucket = distinct_bits_by_metric[metric_name];
                    const auto agg = aggregate_samples_for_report(sample_values);
                    if (agg.top10.empty()) {
                        // All-non-finite cell: same sentinel the
                        // historical reader inserts so an all-NaN
                        // run still trips `metric_looks_constant`
                        // with a NaN-flavoured message.
                        bucket.insert(double_bits(
                            std::numeric_limits<double>::quiet_NaN()));
                        continue;
                    }
                    bucket.insert(double_bits(agg.stats.min));
                    if (agg.stats.max != agg.stats.min) {
                        bucket.insert(double_bits(agg.stats.max));
                    }
                }
            }
        }
    }

    std::vector<SanityWarning> warnings;
    warnings.reserve(distinct_bits_by_metric.size());

    for (const auto& [metric_name, distinct_bits] : distinct_bits_by_metric) {
        // Step 5 of the detector: skip metrics that produced no samples.
        if (distinct_bits.empty()) continue;
        // Constancy test: exactly one distinct bit-pattern.
        if (distinct_bits.size() != 1) continue;

        const std::uint64_t constant_bits = *distinct_bits.begin();
        const double constant_value = std::bit_cast<double>(constant_bits);
        const auto value_text = stringify_constant_value(constant_value);

        // 2. Walk history backwards. The walk stops as soon as a
        //    historical report's distinct samples for this metric are
        //    not a subset of `{constant_bits}`. `oldest_with_warning`
        //    tracks the deepest report still consistent with the
        //    current constant.
        std::string warning_since_label;
        std::string warning_since_commit;
        bool history_gap = false;

        if (current_index <= 1) {
            // First run: warning is trivially present since report_1.
            warning_since_label  = "report_1.json";
            warning_since_commit = current_meta.commit;
        } else {
            // `oldest_with_warning == current_index` is the sentinel
            // "no historical evidence yet — warning is brand new". As
            // we walk back and find consistent reports, the value
            // descends; we stop on the first divergence or absence.
            std::size_t oldest_with_warning = current_index;
            for (std::size_t historical_index = current_index - 1;
                 historical_index >= 1; --historical_index)
            {
                auto* hist_report = history_cache.fetch(historical_index);
                if (hist_report == nullptr) {
                    history_gap = true; break;
                }
                const auto historical_bits =
                    distinct_sample_bits_for_metric(hist_report->root, metric_name);
                // Empty historical set: metric absent (or empty samples)
                // in that report. Treat as a stop condition — the
                // warning needs the metric to have actually emitted a
                // value before we can say it was "the same constant"
                // back then. Pointing `warning_since` at a report that
                // didn't contain the metric would be misleading.
                if (historical_bits.empty()) break;
                bool subset_of_constant = true;
                for (std::uint64_t bits : historical_bits) {
                    if (bits != constant_bits) {
                        subset_of_constant = false; break;
                    }
                }
                if (!subset_of_constant) break;
                oldest_with_warning = historical_index;
                if (historical_index == 1) break;     // avoid size_t underflow
            }

            if (history_gap) {
                // warning_since stays empty → emit JSON null.
            } else if (oldest_with_warning != current_index) {
                warning_since_label =
                    fmt::format("report_{}.json", oldest_with_warning);
                if (auto* origin_report = history_cache.fetch(oldest_with_warning);
                    origin_report != nullptr) {
                    warning_since_commit = commit_from_report(origin_report->root);
                }
            } else {
                // No historical report confirmed the warning. It is
                // brand new in this run — point to the run we are
                // about to write and reuse the current commit.
                warning_since_label  = fmt::format("report_{}.json", current_index);
                warning_since_commit = current_meta.commit;
            }
        }

        SanityWarning warning;
        warning.type   = "metric_looks_constant";
        warning.metric = metric_name;
        warning.message = fmt::format(
            "Metric '{}' always produced the same value '{}' across every "
            "benchmark, algorithm, test case, and region in this report. "
            "This might be a false positive, but more likely the instrumentation "
            "is broken (e.g. add_counter(0) everywhere, an evaluator stuck on a "
            "hardcoded constant, a frozen clock). If the constant value is "
            "genuinely correct, add at least one benchmark or test case where "
            "this metric takes a different value to silence this warning. "
            "This warning cannot be disabled.",
            metric_name, value_text);
        warning.warning_since        = std::move(warning_since_label);
        warning.warning_since_commit = std::move(warning_since_commit);
        warnings.push_back(std::move(warning));
    }

    return warnings;
}

/// Evaluate every regression rule against the current run's collected
/// samples and the on-disk historical reports. Single-threaded —
/// invoked once from `run_impl` after the parallel benchmark loop has
/// joined. Pushes diagnostics (warnings + matched-nothing errors) into
/// `diagnostics_out`; returns the list of detected regressions. The
/// shared `HistoricalReportCache` is owned by the caller so the
/// sanity-warning pass can reuse the parsed report bodies.
[[nodiscard]] inline std::vector<RegressionEntry> evaluate_regressions(
    const RegressionRulesByConfig& regression_rules_by_config,
    const std::map<std::string, std::unique_ptr<BenchmarkExecutionState>>& reports,
    std::size_t                    current_index,
    HistoricalReportCache&         history_cache,
    std::vector<ValidationDiag>&   diagnostics_out) noexcept
{
    std::vector<RegressionEntry> entries;

    auto fetch_report = [&](std::size_t index) -> ParsedHistoricalReport* {
        return history_cache.fetch(index);
    };

    // Per-state merged-samples cache built once per `(config_path,
    // benchmark)` and reused across every rule that touches the same
    // state. Earlier versions rebuilt this map per-rule, which scaled
    // O(N_rules × N_cells).
    std::map<const BenchmarkExecutionState*, MergedSamplesByPair> merged_cache;
    auto merged_for = [&](const BenchmarkExecutionState* state)
        -> const MergedSamplesByPair&
    {
        auto found = merged_cache.find(state);
        if (found != merged_cache.end()) return found->second;
        auto inserted = merged_cache.emplace(
            state, merged_samples_for_state(*state));
        return inserted.first->second;
    };

    for (const auto& [config_path, rules] : regression_rules_by_config) {
        for (const auto& rule : rules) {
            std::size_t matched_count = 0;

            std::vector<const BenchmarkExecutionState*> matching_states;
            for (const auto& [_bench_key, state_ptr] : reports) {
                if (state_ptr->config.config_path != config_path) continue;
                if (!rule.benchmarks.empty()) {
                    bool listed = false;
                    for (const auto& declared : rule.benchmarks) {
                        if (declared == state_ptr->config.benchmark_key) {
                            listed = true; break;
                        }
                    }
                    if (!listed) continue;
                }
                matching_states.push_back(state_ptr.get());
            }

            const std::set<std::string> excluded_test_cases(
                rule.exclude_cases.begin(), rule.exclude_cases.end());

            const std::string rule_location =
                fmt::format("{}/regression_rules/{}", config_path, rule.name);

            for (const auto* state : matching_states) {
                const auto& merged_index = merged_for(state);
                for (const auto& tuple : state->execution_tuples) {
                    for (const auto& [test_case_name, _iter] : state->config.for_each) {
                        if (excluded_test_cases.count(test_case_name) > 0) continue;
                        for (const auto& region_name : rule.regions) {
                            const auto merged_it = merged_index.find(
                                std::pair{&tuple, test_case_name});
                            if (merged_it == merged_index.end()) continue;
                            const auto& merged_samples = merged_it->second;

                            const auto region_it = merged_samples.find(region_name);
                            if (region_it == merged_samples.end()) continue;
                            const auto metric_it = region_it->second.find(rule.metric);
                            if (metric_it == region_it->second.end()
                                || metric_it->second.empty()) continue;

                            // Cell has data → counts toward matched_count
                            // regardless of baseline presence.
                            ++matched_count;
                            const double now_value =
                                aggregate_samples(rule.aggregate, metric_it->second);

                            if (current_index <= 1) {
                                diagnostics_out.push_back({
                                    ValidationDiag::Severity::Warning,
                                    "regression_baseline_missing",
                                    rule_location,
                                    fmt::format("no historical baseline available for benchmark '{}', tuple algo '{}', test_case '{}', region '{}', metric '{}' (current run is index {}); rule treated as satisfied",
                                                state->config.benchmark_key,
                                                tuple.algorithm_name,
                                                test_case_name, region_name,
                                                rule.metric, current_index)
                                });
                                continue;
                            }

                            auto* baseline_report = fetch_report(current_index - 1);
                            std::optional<AggregatedSamples> baseline_aggregate;
                            if (baseline_report != nullptr) {
                                baseline_aggregate = aggregated_from_report(
                                    baseline_report->root,
                                    state->config.benchmark_key,
                                    tuple.algorithm_name, tuple.params,
                                    test_case_name, region_name, rule.metric);
                            }
                            if (!baseline_aggregate.has_value()
                                || baseline_aggregate->samples_count == 0) {
                                diagnostics_out.push_back({
                                    ValidationDiag::Severity::Warning,
                                    "regression_baseline_missing",
                                    rule_location,
                                    fmt::format("no matching samples in report_{}.json for benchmark '{}', tuple algo '{}', test_case '{}', region '{}', metric '{}'; rule treated as satisfied",
                                                current_index - 1,
                                                state->config.benchmark_key,
                                                tuple.algorithm_name,
                                                test_case_name, region_name,
                                                rule.metric)
                                });
                                continue;
                            }

                            const double previously_value =
                                aggregate_samples(rule.aggregate, *baseline_aggregate);

                            if (comparator_passes(rule.op, now_value, previously_value)) {
                                continue;     // no regression
                            }

                            // Walk back through history to compute
                            // regression_since.
                            std::size_t oldest_failing = current_index - 1;
                            bool history_gap = false;
                            // current_index >= 3 is required to enter the
                            // loop: with index = 2 the first predecessor
                            // we'd inspect is index 0 (which doesn't
                            // exist), so the walk has nothing to do and
                            // oldest_failing stays at 1.
                            for (std::size_t historical_index = current_index - 2;
                                 current_index >= 3 && historical_index >= 1;
                                 --historical_index)
                            {
                                auto* hist_report = fetch_report(historical_index);
                                if (hist_report == nullptr) {
                                    history_gap = true; break;
                                }
                                auto hist_aggregate = aggregated_from_report(
                                    hist_report->root,
                                    state->config.benchmark_key,
                                    tuple.algorithm_name, tuple.params,
                                    test_case_name, region_name, rule.metric);
                                if (!hist_aggregate.has_value()
                                    || hist_aggregate->samples_count == 0) {
                                    history_gap = true; break;
                                }
                                const double hist_value =
                                    aggregate_samples(rule.aggregate, *hist_aggregate);
                                if (comparator_passes(rule.op, now_value, hist_value)) {
                                    break;     // found a passing report
                                }
                                oldest_failing = historical_index;
                                // Decrementing past 1 underflows std::size_t;
                                // break before the for-statement's --historical_index
                                // can do that.
                                if (historical_index == 1) break;
                            }

                            std::string regression_since_label;
                            std::string regression_since_commit;
                            if (history_gap) {
                                diagnostics_out.push_back({
                                    ValidationDiag::Severity::Warning,
                                    "regression_history_incomplete",
                                    rule_location,
                                    fmt::format("could not walk full history while attributing regression for benchmark '{}', tuple algo '{}', test_case '{}', region '{}', metric '{}': a historical report was missing or did not contain matching samples",
                                                state->config.benchmark_key,
                                                tuple.algorithm_name,
                                                test_case_name, region_name,
                                                rule.metric)
                                });
                            } else {
                                regression_since_label =
                                    fmt::format("report_{}.json", oldest_failing);
                                if (auto* origin_report = fetch_report(oldest_failing);
                                    origin_report != nullptr) {
                                    regression_since_commit = commit_from_report(origin_report->root);
                                }
                                if (oldest_failing == 1) {
                                    diagnostics_out.push_back({
                                        ValidationDiag::Severity::Warning,
                                        "regression_pre_existing",
                                        rule_location,
                                        fmt::format("regression has been present in every report on disk back to report_1.json for benchmark '{}', tuple algo '{}', test_case '{}', region '{}', metric '{}' — almost certainly not introduced by recent changes",
                                                    state->config.benchmark_key,
                                                    tuple.algorithm_name,
                                                    test_case_name, region_name,
                                                    rule.metric)
                                    });
                                }
                            }

                            RegressionEntry entry;
                            entry.config_path     = config_path;
                            entry.benchmark       = state->config.benchmark_key;
                            entry.rule_name       = rule.name;
                            entry.algorithm_name  = tuple.algorithm_name;
                            entry.violating_params = tuple.params;
                            entry.test_case       = test_case_name;
                            entry.region          = region_name;
                            entry.metric          = rule.metric;
                            entry.aggregate       = rule.aggregate;
                            entry.previously      = previously_value;
                            entry.now             = now_value;
                            entry.regression_since        = std::move(regression_since_label);
                            entry.regression_since_commit = std::move(regression_since_commit);
                            entries.push_back(std::move(entry));
                        }
                    }
                }
            }

            if (matched_count == 0) {
                diagnostics_out.push_back({
                    ValidationDiag::Severity::Error,
                    "regression_rule_matched_nothing",
                    rule_location,
                    fmt::format("regression rule '{}' did not match any (benchmark, tuple, test_case, region) combination with current samples; check 'benchmarks', 'regions', 'metric', and 'exclude_cases'",
                                rule.name)
                });
            }
        }
    }

    return entries;
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
            builder.escape_and_append_with_quotes(std::string_view{value.string_value});
            return;
        case ConfigValue::Kind::Integer:
            builder.append(value.integer_value);
            return;
        case ConfigValue::Kind::Double:
            builder.append(value.double_value);
            return;
        case ConfigValue::Kind::Boolean:
            builder.append(value.boolean_value);
            return;
        case ConfigValue::Kind::NonScalar:
            // NonScalar values should have been rejected by semantic
            // validation long before we reach the report writer. If we're
            // here, validation was bypassed — emit JSON null so the report
            // still parses (the schema's param_value_object accepts
            // numbers/strings/booleans only, so this still flags as a bug
            // when the report is re-validated).
            builder.append_null();
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

/// Emit a `double` JSON-safely. RFC 8259 forbids `NaN` /
/// `Infinity` tokens; the simdjson builder would happily render
/// them and the post-write `validate_against_schema` step would
/// then refuse the report. `sanitize_for_json` collapses any
/// non-finite value to a representable substitute (NaN → 0,
/// ±∞ → numeric_limits clamp) so the writer always produces
/// schema-legal output regardless of how degenerate the input
/// data was.
inline void emit_finite_double(simdjson::builder::string_builder& builder,
                                double value)
{
    builder.append(sanitize_for_json(value));
}

/// Emit `[v0, v1, ...]` from a numeric vector. Routes every
/// element through `emit_finite_double` so the array is always
/// JSON-legal. Used for `top10` / `bottom10`.
inline void emit_double_array(simdjson::builder::string_builder& builder,
                               const std::vector<double>& values)
{
    builder.start_array();
    emit_range_with_commas(builder, values, [](auto& array_builder, double sample) {
        emit_finite_double(array_builder, sample);
    });
    builder.end_array();
}

/// Emit one `(metric -> AggregatedSamples)` leaf. Replaces the
/// pre-V17 raw array emit; the in-memory representation is still
/// `std::vector<double>` (so the rule evaluator and the rest of
/// the framework keep working unchanged), but the on-disk shape
/// is the fixed-size aggregate documented under
/// `kBenchReportSchema#/definitions/aggregated_samples`.
///
/// **Precondition**: `samples` must be non-empty. The schema's
/// `samples_count: minimum: 1` enforces "no empty cells in the
/// report" — call sites filter empty vectors upstream
/// (`emit_test_case_block` walks merged sample maps where empty
/// metric vectors are pruned by `merge_group_samples`'s emit
/// rule). DEBUG_ASSERTed so a future caller that drops the
/// upstream filter trips loudly instead of producing a
/// schema-invalid report.
inline void emit_aggregated_samples(simdjson::builder::string_builder& builder,
                                     const std::vector<double>& samples)
{
    DEBUG_ASSERT(!samples.empty(),
        "emit_aggregated_samples: caller must filter empty sample vectors "
        "(schema requires samples_count >= 1)");
    const auto aggregated = aggregate_samples_for_report(samples);
    emit_object_with_body(builder, [&](auto& root_inner) {
        emit_key(root_inner, "samples_count");
        root_inner.append(static_cast<std::int64_t>(aggregated.samples_count));
        root_inner.append_comma();

        emit_kv_with_body(root_inner, "stats", [&](auto& stats_builder) {
            emit_object_with_body(stats_builder, [&](auto& stats_inner) {
                bool first = true;
                for (const auto& entry : kAggregatedStatsRegistry) {
                    if (!first) stats_inner.append_comma();
                    first = false;
                    emit_key(stats_inner, entry.name);
                    emit_finite_double(stats_inner, aggregated.stats.*(entry.slot));
                }
            });
        });
        root_inner.append_comma();

        emit_kv_with_body(root_inner, "percentiles", [&](auto& pct_builder) {
            emit_object_with_body(pct_builder, [&](auto& pct_inner) {
                bool first = true;
                for (const auto& entry : kAggregatedPercentileRegistry) {
                    if (!first) pct_inner.append_comma();
                    first = false;
                    emit_key(pct_inner, entry.name);
                    emit_finite_double(pct_inner, aggregated.percentiles.*(entry.slot));
                }
            });
        });
        root_inner.append_comma();

        emit_kv_with_body(root_inner, "k-best", [&](auto& kbest_builder) {
            emit_object_with_body(kbest_builder, [&](auto& kbest_inner) {
                emit_kv_with_body(kbest_inner, "top10", [&](auto& array_builder) {
                    emit_double_array(array_builder, aggregated.top10);
                });
                kbest_inner.append_comma();
                emit_kv_with_body(kbest_inner, "bottom10", [&](auto& array_builder) {
                    emit_double_array(array_builder, aggregated.bottom10);
                });
            });
        });
        root_inner.append_comma();

        emit_kv_with_body(root_inner, "histogram", [&](auto& hist_builder) {
            hist_builder.start_array();
            emit_range_with_commas(hist_builder, aggregated.histogram,
                [](auto& bucket_builder, const AggregateBucket& bucket) {
                    emit_object_with_body(bucket_builder, [&](auto& bucket_inner) {
                        emit_key(bucket_inner, "from");
                        emit_finite_double(bucket_inner, bucket.from);
                        bucket_inner.append_comma();
                        emit_key(bucket_inner, "to");
                        emit_finite_double(bucket_inner, bucket.to);
                        bucket_inner.append_comma();
                        emit_key(bucket_inner, "count");
                        bucket_inner.append(static_cast<std::int64_t>(bucket.count));
                    });
                });
            hist_builder.end_array();
        });
    });
}

/// Emit `"<key>": "<value>"` when `value` is non-empty, or `"<key>": null`
/// when it is empty. The framework uses empty strings as the in-memory
/// sentinel for "JSON null" on optional metadata fields like
/// `regression_since`, `regression_since_commit`, `warning_since`, and
/// `warning_since_commit` — every such field follows this exact shape.
inline void emit_kv_string_or_null(simdjson::builder::string_builder& builder,
                                    std::string_view key,
                                    std::string_view value)
{
    emit_key(builder, key);
    if (value.empty()) {
        builder.append_null();
    } else {
        builder.escape_and_append_with_quotes(value);
    }
}

/// Serialise the per-benchmark execution state map plus the global
/// violations / regressions / sanity-warnings lists into the supplied
/// `output_path`. Returns the resolved path on success, empty string
/// on I/O / serialisation failure. The framework writes one report
/// per run as `report_<N>.json`; index allocation lives in `run_impl`
/// so the regression evaluator and the writer agree on
/// `current_index`.
[[nodiscard]] inline std::string write_report(
    const std::filesystem::path& output_path,
    const ReportMeta& meta,
    const std::map<std::string, std::unique_ptr<BenchmarkExecutionState>>& reports,
    const std::vector<Violation>& violations,
    const std::vector<RegressionEntry>& regressions,
    const std::vector<SanityWarning>& sanity_warnings)
{
    constexpr std::size_t kInitialCapacity = 1u << 18;  // 256 KiB up front
    simdjson::builder::string_builder builder{kInitialCapacity};

    /// Emit the run-identity `meta` block: commit, dirty, timestamp,
    /// created, fast.
    auto emit_meta = [&](auto& b) {
        emit_object_with_body(b, [&](auto& inner) {
            emit_kv_string(inner, "commit", meta.commit);
            inner.append_comma();
            emit_key(inner, "dirty");
            inner.append(meta.dirty);
            inner.append_comma();
            emit_key(inner, "timestamp");
            inner.append(meta.timestamp);
            inner.append_comma();
            emit_kv_string(inner, "created", meta.created);
            inner.append_comma();
            emit_key(inner, "fast");
            inner.append(meta.fast);
        });
    };

    /// Emit one violation as a JSON object — exactly the shape the spec
    /// shows in main/DOCS/BENCHMARK_FRAMEWORK/BENCH_V2_CODE__DOC0195.md (`location`, `benchmark`, `rule_name`,
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
                    // Emit RHS as a separate JSON property only when it
                    // names a different algorithm. When LHS and RHS share
                    // an algorithm name (e.g. a self-comparison rule), a
                    // second emit would produce duplicate keys → invalid
                    // JSON. The schema's `maxProperties: 2` tolerates the
                    // 1-key form already.
                    if (!violation.rhs_algorithm.empty()
                        && violation.rhs_algorithm != violation.lhs_algorithm) {
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

    /// Emit one regression entry. Field order mirrors the user spec:
    /// location, benchmark, rule_name, algorithm_name, violating_tuples,
    /// test_cases, region, metric, aggregate, previously, now,
    /// regression_since.
    auto emit_one_regression = [&](auto& b, const RegressionEntry& entry) {
        emit_object_with_body(b, [&](auto& inner) {
            emit_kv_string(inner, "location",       entry.config_path);
            inner.append_comma();
            emit_kv_string(inner, "benchmark",      entry.benchmark);
            inner.append_comma();
            emit_kv_string(inner, "rule_name",      entry.rule_name);
            inner.append_comma();
            emit_kv_string(inner, "algorithm_name", entry.algorithm_name);
            inner.append_comma();
            emit_kv_with_body(inner, "violating_tuples", [&](auto& list_b) {
                list_b.start_array();
                emit_param_object(list_b, entry.violating_params);
                list_b.end_array();
            });
            inner.append_comma();
            emit_kv_with_body(inner, "test_cases", [&](auto& list_b) {
                list_b.start_array();
                list_b.escape_and_append_with_quotes(std::string_view{entry.test_case});
                list_b.end_array();
            });
            inner.append_comma();
            emit_kv_string(inner, "region", entry.region);
            inner.append_comma();
            emit_kv_string(inner, "metric", entry.metric);
            inner.append_comma();
            emit_kv_string(inner, "aggregate",
                std::string_view{aggregate_name(entry.aggregate)});
            inner.append_comma();
            emit_key(inner, "previously");
            inner.append(entry.previously);
            inner.append_comma();
            emit_key(inner, "now");
            inner.append(entry.now);
            inner.append_comma();
            emit_kv_string_or_null(inner, "regression_since",
                std::string_view{entry.regression_since});
            inner.append_comma();
            emit_kv_string_or_null(inner, "regression_since_commit",
                std::string_view{entry.regression_since_commit});
        });
    };

    /// Per-state merged-samples cache. Built lazily so empty
    /// benchmarks pay nothing. Two reports share-by-config and the
    /// writer iterates per benchmark, so a one-shot cache is all we
    /// need.
    std::map<const BenchmarkExecutionState*, MergedSamplesByPair> merged_cache;
    auto merged_for_writer = [&](const BenchmarkExecutionState& state)
        -> const MergedSamplesByPair&
    {
        auto found = merged_cache.find(&state);
        if (found != merged_cache.end()) return found->second;
        return merged_cache.emplace(&state, merged_samples_for_state(state))
            .first->second;
    };

    /// Emit the per-test-case object from a group-merged sample map:
    /// every region (or just "all_regions" when by_regions=false) and
    /// the per-metric AggregatedSamples object inside it. V17 swapped
    /// the per-iteration array out for the fixed-shape aggregate.
    /// Empty sample vectors are pruned: the schema requires
    /// `samples_count >= 1`, and a metric that observed zero values
    /// is genuinely absent from the cell rather than present with a
    /// zeroed aggregate. The pre-V17 writer also dropped empty
    /// arrays, so this preserves the on-disk semantics.
    auto emit_test_case_block = [&](auto& region_block_builder,
                                      const SampleMap& merged) {
        emit_object_with_body(region_block_builder, [&](auto& region_outer) {
            emit_range_with_commas(region_outer, merged,
                [](auto& region_builder, const auto& region_kv) {
                    emit_kv_with_body(region_builder,
                        std::string_view{region_kv.first},
                        [&](auto& metrics_builder) {
                            emit_object_with_body(metrics_builder, [&](auto& metric_outer) {
                                bool first_metric = true;
                                for (const auto& metric_kv : region_kv.second) {
                                    if (metric_kv.second.empty()) continue;
                                    if (!first_metric) metric_outer.append_comma();
                                    first_metric = false;
                                    emit_kv_with_body(metric_outer,
                                        std::string_view{metric_kv.first},
                                        [&](auto& aggregate_builder) {
                                            emit_aggregated_samples(
                                                aggregate_builder, metric_kv.second);
                                        });
                                }
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
                    const auto& merged_index = merged_for_writer(state);
                    bool first_tc_in_tuple = true;
                    for (const auto& [test_case_name, _iter] : state.config.for_each) {
                        const auto pair_it = merged_index.find({&tuple, test_case_name});
                        if (pair_it == merged_index.end()) continue;
                        // Skip if every group produced no samples (e.g.
                        // matrix.for declared zero iterations).
                        if (pair_it->second.empty()) continue;
                        if (!first_tc_in_tuple) cases_inner.append_comma();
                        first_tc_in_tuple = false;
                        emit_kv_with_body(cases_inner,
                            std::string_view{test_case_name},
                            [&](auto& inner_b) {
                                emit_test_case_block(inner_b, pair_it->second);
                            });
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

    /// Emit one sanity-warning entry. Field order:
    /// type, metric, message, warning_since, warning_since_commit.
    /// `warning_since` and `warning_since_commit` use the same
    /// empty-string ⇒ JSON-null convention as regression entries.
    auto emit_one_sanity_warning = [&](auto& b, const SanityWarning& warning) {
        emit_object_with_body(b, [&](auto& inner) {
            emit_kv_string(inner, "type",    warning.type);
            inner.append_comma();
            emit_kv_string(inner, "metric",  warning.metric);
            inner.append_comma();
            emit_kv_string(inner, "message", warning.message);
            inner.append_comma();
            emit_kv_string_or_null(inner, "warning_since",
                std::string_view{warning.warning_since});
            inner.append_comma();
            emit_kv_string_or_null(inner, "warning_since_commit",
                std::string_view{warning.warning_since_commit});
        });
    };

    emit_object_with_body(builder, [&](auto& root_b) {
        // `meta` first — operators reading the report scan the top
        // for commit / timestamp before drilling deeper.
        emit_kv_with_body(root_b, "meta", [&](auto& meta_b) {
            emit_meta(meta_b);
        });
        root_b.append_comma();
        emit_kv_with_body(root_b, "rule_violations", [&](auto& arr_b) {
            arr_b.start_array();
            emit_range_with_commas(arr_b, violations,
                [&](auto& vb, const Violation& violation) {
                    emit_one_violation(vb, violation);
                });
            arr_b.end_array();
        });
        root_b.append_comma();
        emit_kv_with_body(root_b, "regressions", [&](auto& arr_b) {
            arr_b.start_array();
            emit_range_with_commas(arr_b, regressions,
                [&](auto& vb, const RegressionEntry& entry) {
                    emit_one_regression(vb, entry);
                });
            arr_b.end_array();
        });
        root_b.append_comma();
        emit_kv_with_body(root_b, "sanity_warnings", [&](auto& arr_b) {
            arr_b.start_array();
            emit_range_with_commas(arr_b, sanity_warnings,
                [&](auto& vb, const SanityWarning& warning) {
                    emit_one_sanity_warning(vb, warning);
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

    if (output_path.has_parent_path()) {
        std::filesystem::create_directories(output_path.parent_path());
    }
    std::ofstream output_file(output_path, std::ios::trunc | std::ios::binary);
    if (!output_file) {
        loge("[bench_framework] failed to open '{}' for writing", output_path.string());
        return {};
    }
    output_file.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    output_file.close();
    return output_path.string();
}

// ----------------- Stderr violations summary -----------------

/// Render a single `ConfigValue` scalar to its plain-text form. Shared
/// by the stderr pretty-printer and the V8 summary's stable identity
/// builder — both want the value as a string but with different field
/// separators around it. Doubles use `{:.17g}` so an int → double
/// round-trip in a historical report (`5` parsed back as `5.0`)
/// canonicalises onto the integer form via the `floor(value) == value
/// && |value| < 2^53` guard, preventing identity-key drift between
/// reports written by different framework versions.
inline void append_config_value_text(std::string& out, const ConfigValue& value) noexcept {
    switch (value.kind) {
        case ConfigValue::Kind::String:    out += value.string_value; return;
        case ConfigValue::Kind::Integer:   out += std::to_string(value.integer_value); return;
        case ConfigValue::Kind::Double: {
            const double dbl = value.double_value;
            if (std::isfinite(dbl)
                && dbl == std::floor(dbl)
                && std::abs(dbl) < static_cast<double>(1ULL << 53)) {
                out += std::to_string(static_cast<std::int64_t>(dbl));
            } else {
                out += fmt::format("{:.17g}", dbl);
            }
            return;
        }
        case ConfigValue::Kind::Boolean:   out += value.boolean_value ? "true" : "false"; return;
        case ConfigValue::Kind::NonScalar: out += "<non-scalar>"; return;
    }
}

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
        append_config_value_text(result, value);
    }
    return result;
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
        const auto lhs_param_text = format_param_map(violation.lhs_full_params);
        const auto rhs_param_text = format_param_map(violation.rhs_full_params);
        // Mirror the JSON writer's RHS-dedup: when both selectors named the
        // same algorithm with the same param map, print one combined line
        // instead of two visually identical sides.
        const bool same_side =
            violation.lhs_algorithm == violation.rhs_algorithm
            && lhs_param_text == rhs_param_text;
        if (same_side) {
            std::fprintf(stderr,
                "    self-compare on %s { %s }\n        LHS median = %.6g\n        RHS median = %.6g\n",
                violation.lhs_algorithm.c_str(),
                lhs_param_text.c_str(),
                violation.args[0],
                violation.args[1]);
        } else {
            std::fprintf(stderr,
                "    LHS %s { %s }\n        median = %.6g\n",
                violation.lhs_algorithm.c_str(),
                lhs_param_text.c_str(),
                violation.args[0]);
            std::fprintf(stderr,
                "    RHS %s { %s }\n        median = %.6g\n",
                violation.rhs_algorithm.c_str(),
                rhs_param_text.c_str(),
                violation.args[1]);
        }
        std::fprintf(stderr, "\n");
    }

    std::fprintf(stderr,
        "============================================================\n");
    std::fflush(stderr);
}

/// Render the trailing `<label> = report_<N>.json (commit=<sha>)` line
/// shared by `print_regressions` / `print_sanity_warnings`. Empty
/// `since` becomes `"<label> = null (history incomplete)"`; an empty
/// commit on a known report becomes `"<unknown>"`. Both printers carry
/// the same "where did this anomaly originate" semantics so the
/// formatting belongs in one place.
inline void print_since_line(const char* label,
                              const std::string& since,
                              const std::string& commit) noexcept
{
    if (since.empty()) {
        std::fprintf(stderr, "    %s = null (history incomplete)\n", label);
        return;
    }
    std::fprintf(stderr, "    %s = %s (commit=%s)\n",
        label, since.c_str(),
        commit.empty() ? "<unknown>" : commit.c_str());
}

/// Pretty-print every detected regression to stderr. Mirrors
/// `print_violations` for visual consistency. Uses raw stdio for the
/// same reason — this is final-pass operator output.
inline void print_regressions(const std::vector<RegressionEntry>& regressions) noexcept {
    if (regressions.empty()) {
        std::fprintf(stderr,
            "============================================================\n"
            "BENCH FRAMEWORK — no regressions detected (0 entries)\n"
            "============================================================\n");
        std::fflush(stderr);
        return;
    }

    std::fprintf(stderr,
        "============================================================\n"
        "BENCH FRAMEWORK — %zu regression%s\n"
        "============================================================\n",
        regressions.size(), regressions.size() == 1 ? "" : "s");

    for (const auto& entry : regressions) {
        std::fprintf(stderr,
            "[%s] %s (rule=%s, test_case=%s, region=%s, metric=%s, aggregate=%s)\n",
            entry.benchmark.c_str(),
            entry.algorithm_name.c_str(),
            entry.rule_name.c_str(),
            entry.test_case.c_str(),
            entry.region.c_str(),
            entry.metric.c_str(),
            std::string{aggregate_name(entry.aggregate)}.c_str());
        const auto params_text = format_param_map(entry.violating_params);
        std::fprintf(stderr,
            "    tuple { %s }\n",
            params_text.c_str());
        std::fprintf(stderr,
            "    previously = %.6g, now = %.6g\n",
            entry.previously, entry.now);
        print_since_line("regression_since",
            entry.regression_since, entry.regression_since_commit);
        std::fprintf(stderr, "\n");
    }

    std::fprintf(stderr,
        "============================================================\n");
    std::fflush(stderr);
}

/// Pretty-print every sanity warning to stderr. Mirrors
/// `print_regressions` / `print_violations` for visual consistency.
/// Uses raw stdio for the same reason — final-pass operator output
/// must remain visible after fmtlog has drained.
inline void print_sanity_warnings(const std::vector<SanityWarning>& warnings) noexcept {
    if (warnings.empty()) {
        std::fprintf(stderr,
            "============================================================\n"
            "BENCH FRAMEWORK — all sanity checks passed (0 warnings)\n"
            "============================================================\n");
        std::fflush(stderr);
        return;
    }

    std::fprintf(stderr,
        "============================================================\n"
        "BENCH FRAMEWORK — %zu sanity warning%s (non-blocking)\n"
        "============================================================\n",
        warnings.size(), warnings.size() == 1 ? "" : "s");

    for (const auto& warning : warnings) {
        std::fprintf(stderr,
            "[%s] %s\n",
            warning.type.c_str(),
            warning.metric.c_str());
        std::fprintf(stderr,
            "    %s\n",
            warning.message.c_str());
        print_since_line("warning_since",
            warning.warning_since, warning.warning_since_commit);
        std::fprintf(stderr, "\n");
    }

    std::fprintf(stderr,
        "============================================================\n");
    std::fflush(stderr);
}

// ----------------- V8: Cross-report summary dashboard -----------------
//
// The `--summary` mode walks every `report_<N>.json` already on disk
// in `report_dir`, deduplicates rule violations / regressions / sanity
// warnings across reports by a stable identity tuple (§ identity),
// scores each unique issue by base-severity × recurrence × value-pct ×
// amplitude, and renders an ftxui dashboard grouped by metric →
// scope → test_case. No benchmarks are executed; the framework is a
// pure consumer of prior reports.

/// One unique issue tracked across every historical report on disk.
/// All scoring inputs (presence pattern, latest values, latest
/// samples) live on the struct so `score_summary_issue` is a
/// pure-arithmetic constexpr-friendly helper.
struct SummaryIssue {
    enum class Kind : std::uint8_t { Regression, RuleViolation, SanityWarning };

    Kind                          kind = Kind::SanityWarning;
    /// Stable identity string (see `make_*_identity` helpers).
    std::string                   identity{};
    /// Layout groupers — drive the dashboard's two-level aggregation.
    std::string                   metric{};
    std::string                   scope_label{};
    std::string                   test_case{};   // "" for sanity warnings
    std::string                   benchmark{};   // "" for sanity warnings
    std::string                   rule_name{};   // "" for sanity warnings
    /// Latest-report values used by the value-pct factor and shown
    /// in the dashboard's stat blurb.
    /// Rule:        [lhs_aggregate, rhs_aggregate]
    /// Regression:  [previously, now]
    /// Sanity:      ignored
    std::array<double, 2>         latest_args{0.0, 0.0};
    /// Latest-report aggregated samples used by `amplitude_factor`.
    /// Default-constructed (`samples_count == 0`) for sanity
    /// warnings or when the matching cell wasn't found.
    AggregatedSamples             latest_samples{};
    /// One byte per report slot (0 = absent, 1 = present). Length =
    /// max_report_index after the walk finishes. We use uint8_t
    /// instead of `vector<bool>` to avoid the bit-packed
    /// specialisation's missing `data()` and friction with
    /// `std::span`.
    std::vector<std::uint8_t>     presence_per_report{};
    /// Distinguishes "report missing" from "report present, issue
    /// absent" for the renderer (🟦 vs 🟩 / 🟥 / 🟨).
    std::vector<std::uint8_t>     report_data_present{};
    /// Highest report index where this issue was observed; tells the
    /// walk loop when to overwrite `latest_args` / `latest_samples`.
    std::size_t                   latest_seen_index = 0;
    /// Last sanity warning's free-form message (the renderer prints
    /// the first line as the stat blurb). Empty for non-sanity.
    std::string                   sanity_message{};
    /// Cached score; populated once after the walk by
    /// `score_summary_issue`.
    double                        score = 0.0;
};

/// Severity bucket — the score → emoji + colour mapping. The table is
/// scanned in declaration order; the first entry whose `min_score`
/// the issue meets wins. List order is high → low, matching the user
/// spec.
struct SeverityBucket {
    double            min_score;
    std::string_view  emoji;
    ftxui::Color      color;
    std::string_view  label;
};

inline const std::array<SeverityBucket, 5> kSeverityBuckets{ {
    { 1500.0, "\xe2\x9b\x94",      ftxui::Color::Red,         "critical"  }, // ⛔
    { 1000.0, "\xf0\x9f\x94\xb4",  ftxui::Color::RedLight,    "high"      }, // 🔴
    {  600.0, "\xe2\xad\x95",      ftxui::Color::Magenta,     "medium-hi" }, // ⭕
    {  300.0, "\xf0\x9f\x9f\xa0",  ftxui::Color::Yellow,      "medium"    }, // 🟠
    {    0.0, "\xf0\x9f\x9f\xa1",  ftxui::Color::YellowLight, "low"       }, // 🟡
} };

/// Look up the bucket whose `min_score` is the highest one ≤ `score`.
/// Sentinel: the last entry has `min_score == 0`, so the loop always
/// finds a hit for any non-negative score.
[[nodiscard]] inline const SeverityBucket& bucket_for(double score) noexcept {
    for (const auto& bucket : kSeverityBuckets) {
        if (score >= bucket.min_score) return bucket;
    }
    return kSeverityBuckets.back();
}

/// Per-slot block glyph for an issue's presence row.
/// Order of preconditions: missing report (🟦) wins over presence
/// state because we cannot say anything about an unparseable file.
[[nodiscard]] inline std::string_view summary_block_for(
    SummaryIssue::Kind kind,
    bool present,
    bool report_data_present) noexcept
{
    if (!report_data_present) return "\xf0\x9f\x9f\xa6";   // 🟦
    if (!present)             return "\xf0\x9f\x9f\xa9";   // 🟩
    if (kind == SummaryIssue::Kind::SanityWarning) return "\xf0\x9f\x9f\xa8"; // 🟨
    return "\xf0\x9f\x9f\xa5";                              // 🟥
}

/// 64-bit FNV-1a hash. Used to fingerprint the sanity-warning
/// `message` field so two genuinely different constant values on the
/// same metric (e.g. "490" vs "0") don't collide in the deduplicated
/// view.
[[nodiscard]] constexpr std::uint64_t fnv1a64(std::string_view text) noexcept {
    constexpr std::uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ULL;
    constexpr std::uint64_t kFnvPrime       = 0x00000100000001B3ULL;
    std::uint64_t hash = kFnvOffsetBasis;
    for (const char ch : text) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
        hash *= kFnvPrime;
    }
    return hash;
}

// Field separators for identity strings. ASCII 0x1e (record
// separator) and 0x1f (unit separator) are reserved control
// characters in JSON-clean input, so config-supplied identifiers
// cannot inject ambiguity here.
inline constexpr char kSummaryIdentityRs = '\x1e';
inline constexpr char kSummaryIdentityUs = '\x1f';

/// Stable serialisation of a ParamMap into the V8 identity grammar:
/// `k1=v1\x1fk2=v2\x1f...`. Iteration is alphabetical (`ParamMap` is
/// `std::map`).
[[nodiscard]] inline std::string format_param_map_stable(const ParamMap& params) noexcept {
    std::string result;
    bool first = true;
    for (const auto& [name, value] : params) {
        if (!first) result += kSummaryIdentityUs;
        first = false;
        result += name;
        result += '=';
        append_config_value_text(result, value);
    }
    return result;
}

/// Variadic identity-string builder. Joins `kind` to the trailing
/// `fields` with `\x1e` after the kind and `\x1f` between subsequent
/// fields. Each `field` is implicitly convertible to `std::string_view`.
/// Used by the three `make_*_identity` callers below; the fold
/// expression collapses what was previously a per-kind 20-line
/// `string += sep` ritual.
template<typename... Fields>
[[nodiscard]] inline std::string join_identity(
    std::string_view kind, const Fields&... fields) noexcept
{
    std::string identity{kind};
    identity += kSummaryIdentityRs;
    bool first = true;
    auto append_one = [&](std::string_view field) noexcept {
        if (!first) identity += kSummaryIdentityUs;
        first = false;
        identity += field;
    };
    (append_one(std::string_view{fields}), ...);
    return identity;
}

/// Identity helpers — one tiny call to `join_identity` each. See
/// main/DOCS/BENCHMARK_FRAMEWORK/BENCH_V8_IMPL_PLAN__DOC0204.md §2. The `region` and `aggregate` parameters
/// on `make_violation_identity` reserve slots in the identity tuple
/// for the eventual per-region / per-aggregate rule extensions; V8
/// callers pass the constant defaults `"all_regions"` / `"p50"` (no
/// per-region rules exist yet, but the slots ensure forward
/// compatibility).
[[nodiscard]] inline std::string make_violation_identity(
    std::string_view config_path,
    std::string_view benchmark,
    std::string_view rule_name,
    std::string_view lhs_algorithm,
    const ParamMap&  lhs_params,
    std::string_view rhs_algorithm,
    const ParamMap&  rhs_params,
    std::string_view test_case,
    std::string_view metric,
    std::string_view region    = "all_regions",
    std::string_view aggregate = "p50") noexcept
{
    return join_identity("rule",
        config_path, benchmark, rule_name,
        lhs_algorithm, format_param_map_stable(lhs_params),
        rhs_algorithm, format_param_map_stable(rhs_params),
        test_case, metric, region, aggregate);
}

[[nodiscard]] inline std::string make_regression_identity(
    std::string_view config_path,
    std::string_view benchmark,
    std::string_view rule_name,
    std::string_view algorithm,
    const ParamMap&  params,
    std::string_view test_case,
    std::string_view region,
    std::string_view metric,
    std::string_view aggregate) noexcept
{
    return join_identity("regression",
        config_path, benchmark, rule_name,
        algorithm, format_param_map_stable(params),
        test_case, region, metric, aggregate);
}

[[nodiscard]] inline std::string make_sanity_identity(
    std::string_view type,
    std::string_view metric,
    std::string_view message) noexcept
{
    // 16-hex FNV-1a fingerprint plus the message length cheaply
    // discriminates same-prefix messages without storing the full
    // verbose body in the identity key.
    auto hash_text = fmt::format("{:016x}-{}", fnv1a64(message), message.size());
    return join_identity("sanity", type, metric, std::string_view{hash_text});
}

// ----------------- Per-report sample-array index -----------------

/// Walk an object whose fields are `(param_name, scalar_value)` pairs
/// and produce a `ParamMap`. The same shape appears in three V8 walk
/// sites (rule-violation sides, regression `violating_tuples[0]`,
/// and `build_sample_index`'s `tuple` block); extracting the loop
/// keeps the framework's three callers from drifting out of sync.
[[nodiscard]] inline ParamMap parse_param_map(simdjson::dom::object obj) noexcept {
    ParamMap params;
    for (auto field : obj) {
        params.emplace(std::string{field.key},
                       ConfigValue::from_simdjson(field.value));
    }
    return params;
}

/// Path → aggregated-samples map used by the V8 summary's
/// amplitude lookup. The path is
/// `benchmark\x1falgorithm\x1fparams\x1ftest_case\x1fregion\x1fmetric`
/// (a subset of the issue identity, minus the kind/rule_name
/// fields) and matches what the issue walk produces when it asks
/// for the AggregatedSamples of "the cell that triggered this
/// rule violation / regression in this report". V17: stores
/// pre-aggregated records instead of raw vectors.
using SampleIndex = std::map<std::string, AggregatedSamples>;

/// Build the per-cell sample-array path key.
[[nodiscard]] inline std::string make_sample_path(
    std::string_view benchmark,
    std::string_view algorithm,
    const ParamMap&  params,
    std::string_view test_case,
    std::string_view region,
    std::string_view metric) noexcept
{
    std::string path;
    path += benchmark;   path += kSummaryIdentityUs;
    path += algorithm;   path += kSummaryIdentityUs;
    path += format_param_map_stable(params);
    path += kSummaryIdentityUs;
    path += test_case;   path += kSummaryIdentityUs;
    path += region;      path += kSummaryIdentityUs;
    path += metric;
    return path;
}

/// Walk a parsed report's full `benchmarks.<>.execution_tuples[*].
/// test_cases.<>.<region>.<metric>` tree once, building a flat
/// `SampleIndex` keyed by the same path the summary's identity
/// uses. Each leaf is parsed via `parse_aggregated_samples` so
/// the index carries the V17 fixed-shape aggregate. O(report
/// size); called at most once per report regardless of how many
/// issues need amplitude data.
[[nodiscard]] inline SampleIndex build_sample_index(
    simdjson::dom::element report_root) noexcept
{
    SampleIndex index;
    const auto root_obj = try_as_object(report_root);
    if (!root_obj.has_value()) return index;
    const auto benchmarks_obj = try_get_object(*root_obj, "benchmarks");
    if (!benchmarks_obj.has_value()) return index;

    for (auto bench_field : *benchmarks_obj) {
        const auto bench_obj = try_as_object(bench_field.value);
        if (!bench_obj.has_value()) continue;
        simdjson::dom::array execution_tuples;
        if ((*bench_obj)["execution_tuples"].get(execution_tuples) != simdjson::SUCCESS) {
            continue;
        }
        for (auto tuple_record : execution_tuples) {
            const auto record_obj = try_as_object(tuple_record);
            if (!record_obj.has_value()) continue;
            const auto tuple_inner = try_get_object(*record_obj, "tuple");
            if (!tuple_inner.has_value()) continue;
            // The tuple sub-object has exactly one key — the algorithm
            // name — mapping to the param map.
            for (auto algo_field : *tuple_inner) {
                std::string algorithm{algo_field.key};
                const auto params_obj = try_as_object(algo_field.value);
                if (!params_obj.has_value()) continue;
                ParamMap params = parse_param_map(*params_obj);
                const auto test_cases_obj = try_get_object(*record_obj, "test_cases");
                if (!test_cases_obj.has_value()) continue;
                for (auto tc_field : *test_cases_obj) {
                    const auto tc_obj = try_as_object(tc_field.value);
                    if (!tc_obj.has_value()) continue;
                    for (auto region_field : *tc_obj) {
                        const auto region_obj = try_as_object(region_field.value);
                        if (!region_obj.has_value()) continue;
                        for (auto metric_field : *region_obj) {
                            simdjson::dom::object metric_obj;
                            if (metric_field.value.get(metric_obj) != simdjson::SUCCESS) continue;
                            auto path = make_sample_path(
                                std::string_view{bench_field.key},
                                algorithm, params,
                                std::string_view{tc_field.key},
                                std::string_view{region_field.key},
                                std::string_view{metric_field.key});
                            index.emplace(std::move(path),
                                           parse_aggregated_samples(metric_obj));
                        }
                    }
                }
            }
        }
    }
    return index;
}

/// Look up an `AggregatedSamples` in a per-report `SampleIndex`.
/// Returns a default-constructed aggregate (`samples_count == 0`)
/// when the cell is absent — `amplitude_factor` already treats
/// `samples_count < 2` as "no amplitude info" and falls back to 1.0.
[[nodiscard]] inline AggregatedSamples samples_from_index(
    const SampleIndex& index,
    std::string_view benchmark,
    std::string_view algorithm,
    const ParamMap&  params,
    std::string_view test_case,
    std::string_view region,
    std::string_view metric) noexcept
{
    auto path = make_sample_path(benchmark, algorithm, params, test_case, region, metric);
    if (auto it = index.find(path); it != index.end()) return it->second;
    return {};
}

// ----------------- Per-report walks -----------------

/// Helper: pull a string-typed field from an object. Returns "" when
/// the field is missing or non-string (matches the rest of the file's
/// graceful-degradation pattern for JSON walks).
[[nodiscard]] inline std::string get_string_field(
    simdjson::dom::object obj, std::string_view key) noexcept
{
    std::string_view text;
    if (obj.at_key(key).get(text) == simdjson::SUCCESS) return std::string{text};
    return {};
}

/// Helper: pull a double-typed field from an object. Returns 0.0 on
/// miss or wrong type.
[[nodiscard]] inline double get_double_field(
    simdjson::dom::object obj, std::string_view key) noexcept
{
    double value = 0.0;
    if (obj.at_key(key).get(value) == simdjson::SUCCESS) return value;
    std::int64_t ivalue = 0;
    if (obj.at_key(key).get(ivalue) == simdjson::SUCCESS) {
        return static_cast<double>(ivalue);
    }
    return 0.0;
}

/// One entry extracted from a per-report JSON array. The `Extract`
/// callable returns `std::nullopt` for malformed entries; otherwise
/// the framework merges the data into `aggregation` under
/// `update_first` (run once when the issue is brand new) and
/// `update_latest` (run whenever this report wins the latest-seen
/// race).
struct ExtractedIssue {
    std::string                          identity;
    SummaryIssue::Kind                   kind = SummaryIssue::Kind::SanityWarning;
    /// Initialiser for fields that are intrinsic to the issue's
    /// identity (metric / rule_name / scope_label / etc.). Run
    /// once on first insertion.
    std::function<void(SummaryIssue&)>   update_first;
    /// Updater for fields that may move between reports
    /// (latest_args, latest_samples, sanity_message). Run only when
    /// `report_index > issue.latest_seen_index`.
    std::function<void(SummaryIssue&)>   update_latest;
};

/// Generic per-report-array walker. The three issue kinds (rule
/// violations, regressions, sanity warnings) all follow the same
/// shape: pull `report_root[array_key]`, iterate entries, build an
/// identity, and merge into `aggregation`. Only the per-entry
/// extraction differs — encoded in the `Extract` callable. This
/// templated walker collapses ~120 lines of triplicated boilerplate
/// from the previous three `walk_report_*` functions into one.
template<typename Extract>
inline void walk_report_array(
    simdjson::dom::element                report_root,
    std::string_view                       array_key,
    std::size_t                            report_index,
    std::size_t                            max_index,
    std::map<std::string, SummaryIssue>&   aggregation,
    Extract                                extract) noexcept
{
    DEBUG_ASSERT(report_index >= 1, "report_index must be 1-based; 0 underflows the presence-array index");
    const auto root_obj = try_as_object(report_root);
    if (!root_obj.has_value()) return;
    simdjson::dom::array entries_array;
    if ((*root_obj)[array_key].get(entries_array) != simdjson::SUCCESS) return;

    for (auto entry : entries_array) {
        const auto obj = try_as_object(entry);
        if (!obj.has_value()) continue;
        auto extracted = extract(*obj);
        if (!extracted.has_value()) continue;
        ExtractedIssue ex = std::move(*extracted);

        // `try_emplace` distinguishes "first encounter" from "merge
        // into existing" cleanly. We move the identity string into
        // the map key on insert; on the merge path the identity
        // already lives both as the map key and on the existing
        // SummaryIssue (set the first time), so the moved-from
        // `ex.identity` is not consulted again.
        auto [it, inserted] = aggregation.try_emplace(std::move(ex.identity));
        SummaryIssue& issue = it->second;
        if (inserted) {
            issue.kind     = ex.kind;
            issue.identity = it->first;     // copy back from the stored key
            issue.presence_per_report.assign(max_index, 0);
            ex.update_first(issue);
        }
        issue.presence_per_report[report_index - 1] = 1;
        if (report_index > issue.latest_seen_index) {
            issue.latest_seen_index = report_index;
            ex.update_latest(issue);
        }
    }
}

/// Pull `violating_tuples[0]` (a single-element param-map array).
/// Returns an empty `ParamMap` when the array is missing or empty.
[[nodiscard]] inline ParamMap parse_violating_tuple_first(simdjson::dom::object obj) noexcept {
    simdjson::dom::array tuples_array;
    if (obj["violating_tuples"].get(tuples_array) != simdjson::SUCCESS) return {};
    for (auto tuple_elem : tuples_array) {
        const auto params_obj = try_as_object(tuple_elem);
        if (!params_obj.has_value()) continue;
        return parse_param_map(*params_obj);     // schema mandates exactly one
    }
    return {};
}

/// Pull `test_cases[0]` (a single-element string array).
[[nodiscard]] inline std::string parse_test_case_first(simdjson::dom::object obj) noexcept {
    simdjson::dom::array tc_array;
    if (obj["test_cases"].get(tc_array) != simdjson::SUCCESS) return {};
    for (auto tc_elem : tc_array) {
        std::string_view tc_text;
        if (tc_elem.get(tc_text) == simdjson::SUCCESS) return std::string{tc_text};
    }
    return {};
}

/// Pull both args from a `rule_violations[*].args` 2-element array.
/// Defaults to `{0, 0}` when the array is missing or wrong-shaped.
[[nodiscard]] inline std::array<double, 2> parse_violation_args(
    simdjson::dom::object obj) noexcept
{
    std::array<double, 2> args{0.0, 0.0};
    simdjson::dom::array args_array;
    if (obj["args"].get(args_array) != simdjson::SUCCESS) return args;
    std::size_t arg_index = 0;
    for (auto arg_elem : args_array) {
        if (arg_index >= args.size()) break;
        double sample_value = 0.0;
        if (arg_elem.get(sample_value) == simdjson::SUCCESS) {
            args[arg_index] = sample_value;
        }
        ++arg_index;
    }
    return args;
}

/// Pull every `(<algo_name>, <ParamMap>)` side from a
/// `rule_violations[*].execution_tuple`. May return one (self-compare)
/// or two entries.
[[nodiscard]] inline std::vector<std::pair<std::string, ParamMap>>
parse_violation_sides(simdjson::dom::object obj) noexcept
{
    std::vector<std::pair<std::string, ParamMap>> sides;
    const auto exec_obj = try_get_object(obj, "execution_tuple");
    if (!exec_obj.has_value()) return sides;
    for (auto algo_field : *exec_obj) {
        const auto params_inner = try_as_object(algo_field.value);
        if (!params_inner.has_value()) continue;
        sides.emplace_back(std::string{algo_field.key},
                            parse_param_map(*params_inner));
    }
    return sides;
}

// ----------------- Scoring -----------------

inline constexpr double kSummaryBaseRegression    = 1000.0;
inline constexpr double kSummaryBaseRuleViolation =  700.0;
inline constexpr double kSummaryBaseSanityWarning =  300.0;

[[nodiscard]] constexpr double base_severity(SummaryIssue::Kind kind) noexcept {
    switch (kind) {
        case SummaryIssue::Kind::Regression:     return kSummaryBaseRegression;
        case SummaryIssue::Kind::RuleViolation:  return kSummaryBaseRuleViolation;
        case SummaryIssue::Kind::SanityWarning:  return kSummaryBaseSanityWarning;
    }
    return kSummaryBaseSanityWarning;
}

/// Multiplicative time-pattern factor. Inputs are pre-extracted from
/// the presence array so this stays a pure function of small integers
/// + booleans.
[[nodiscard]] constexpr double recurrence_factor(
    bool        present_in_latest,
    std::size_t recent_streak,
    bool        any_old_gap,
    std::size_t total_reports) noexcept
{
    if (!present_in_latest)                                      return 0.20;
    if (total_reports > 0 && recent_streak == total_reports)     return 0.40;
    if (recent_streak == 1) {
        return any_old_gap ? 2.40 : 1.60;
    }
    const double base        = any_old_gap ? 1.80 : 1.30;
    const double floor_value = any_old_gap ? 0.70 : 0.40;
    const double slope       = any_old_gap ? 0.15 : 0.20;
    const double faded       = base - slope * static_cast<double>(recent_streak - 1);
    return faded < floor_value ? floor_value : faded;
}

/// Relative percentage difference
/// `|current - baseline| / |baseline| * 100`. Used by both
/// `value_pct_factor` (for scoring) and `format_stat_blurb` (for
/// dashboard display). The `kEps` clamp on the denominator
/// prevents the divide-by-zero when `baseline == 0` while keeping
/// the displayed percentage stable across consumers.
[[nodiscard]] inline double relative_diff_pct(double current, double baseline) noexcept {
    constexpr double kEps = 1e-12;
    return std::abs(current - baseline) / std::max(std::abs(baseline), kEps) * 100.0;
}

/// Relative amplitude percentage `100 * (max - min) / |min|` of
/// a pre-aggregated sample set. Used by both `amplitude_factor`
/// and the dashboard's stat blurb. Returns 0 for fewer than two
/// finite samples (no amplitude to speak of). The `max(|min|, kEps)`
/// clamp prevents divide-by-zero when min == 0 (e.g. all-zero
/// samples). V17: takes `AggregatedSamples` so it works on both
/// in-memory and historical data uniformly.
[[nodiscard]] inline double relative_amplitude_pct(
    const AggregatedSamples& aggregated) noexcept
{
    if (aggregated.samples_count < 2) return 0.0;
    if (aggregated.top10.empty())     return 0.0;     // all-non-finite cell
    constexpr double kEps = 1e-12;
    return 100.0 * (aggregated.stats.max - aggregated.stats.min)
                 / std::max(std::abs(aggregated.stats.min), kEps);
}

/// Multiplicative value-percentage factor. log-shaped so a 555%
/// overshoot and a 5550% overshoot don't collapse onto the same
/// number. A near-zero denominator (`baseline == 0` for a
/// regression rising to anything) saturates at `kFactorCeil`
/// directly — the log path would clamp there anyway, but the
/// explicit branch makes intent obvious.
[[nodiscard]] inline double value_pct_factor(double current, double baseline) noexcept {
    constexpr double kEps         = 1e-12;
    constexpr double kSlope       = 0.5;
    constexpr double kFactorCeil  = 3.0;
    if (std::abs(baseline) < kEps) return kFactorCeil;
    const double pct = relative_diff_pct(current, baseline);
    const double bonus = kSlope * std::log10(1.0 + pct / 100.0);
    const double factor = 1.0 + bonus;
    return factor < kFactorCeil ? factor : kFactorCeil;
}

/// Multiplicative amplitude factor. High-noise samples reduce
/// the score because the regression / rule violation may be
/// within natural variability. Curve mirrors the user spec:
/// factor = 1.0 at amp = 0%, factor = 1/6 at amp = 10000%.
[[nodiscard]] inline double amplitude_factor(
    const AggregatedSamples& aggregated) noexcept
{
    if (aggregated.samples_count < 2) return 1.0;
    constexpr double kCapPct = 10000.0;
    const double rel_amp_pct = relative_amplitude_pct(aggregated);
    const double clipped     = rel_amp_pct < kCapPct ? rel_amp_pct : kCapPct;
    const double normalised  = clipped / 1000.0;
    return (12.0 - normalised) / 12.0;
}

/// Compute (recent_streak, any_old_gap) from a presence array.
struct PresencePattern {
    bool        present_in_latest = false;
    std::size_t recent_streak     = 0;
    bool        any_old_gap       = false;
};

[[nodiscard]] inline PresencePattern presence_pattern(
    std::span<const std::uint8_t> presence) noexcept
{
    PresencePattern result;
    if (presence.empty()) return result;
    result.present_in_latest = presence.back() != 0;
    if (!result.present_in_latest) return result;
    std::size_t streak = 0;
    for (auto rit = presence.rbegin(); rit != presence.rend(); ++rit) {
        if (*rit == 0) break;
        ++streak;
    }
    result.recent_streak = streak;
    if (streak < presence.size()) {
        const std::size_t older_end = presence.size() - streak;
        for (std::size_t i = 0; i < older_end; ++i) {
            if (presence[i] != 0) { result.any_old_gap = true; break; }
        }
    }
    return result;
}

/// Composite scoring pipeline. Sanity warnings skip value-pct and
/// amplitude (no underlying numeric comparison); rules and
/// regressions use both.
[[nodiscard]] inline double score_summary_issue(const SummaryIssue& issue) noexcept {
    const auto pattern = presence_pattern(issue.presence_per_report);
    const double recurrence = recurrence_factor(
        pattern.present_in_latest,
        pattern.recent_streak,
        pattern.any_old_gap,
        issue.presence_per_report.size());
    const double base = base_severity(issue.kind);
    if (issue.kind == SummaryIssue::Kind::SanityWarning) {
        return base * recurrence;
    }
    const double value_pct = value_pct_factor(issue.latest_args[0], issue.latest_args[1]);
    const double amplitude = amplitude_factor(issue.latest_samples);
    return base * recurrence * value_pct * amplitude;
}

// ----------------- Render -----------------

/// Issues sorted by `(metric ↑, scope_label ↑, score ↓, identity ↑)`.
/// The score-descending pass is what drives the user's "most concerning
/// first" expectation.
[[nodiscard]] inline std::vector<const SummaryIssue*> sort_summary_issues(
    const std::map<std::string, SummaryIssue>& aggregation) noexcept
{
    std::vector<const SummaryIssue*> sorted;
    sorted.reserve(aggregation.size());
    for (const auto& [_identity, issue] : aggregation) sorted.push_back(&issue);
    std::sort(sorted.begin(), sorted.end(),
        [](const SummaryIssue* lhs, const SummaryIssue* rhs) noexcept {
            if (lhs->metric != rhs->metric)         return lhs->metric < rhs->metric;
            if (lhs->scope_label != rhs->scope_label) return lhs->scope_label < rhs->scope_label;
            if (lhs->score != rhs->score)           return lhs->score > rhs->score;
            return lhs->identity < rhs->identity;
        });
    return sorted;
}

/// Truncate `text` to at most `max_chars` *bytes* (a byte budget,
/// not a column count) and append the UTF-8 ellipsis when shortened.
/// When the cut would land inside a UTF-8 multi-byte sequence we
/// retract to the nearest leading byte (continuation bytes have the
/// `10xxxxxx` bit pattern) so we never emit a half-encoded character.
/// The dashboard mostly prints ASCII; the retract-loop adds at most
/// 3 bytes of overhead per call.
[[nodiscard]] inline std::string clip_to_width(std::string text, std::size_t max_chars) noexcept {
    if (text.size() <= max_chars) return text;
    constexpr std::string_view kEllipsis = "\xe2\x80\xa6";   // "…" (3 bytes)
    if (max_chars <= kEllipsis.size()) return std::string(max_chars, '.');
    std::size_t cut = max_chars - kEllipsis.size();
    while (cut > 0
        && (static_cast<unsigned char>(text[cut]) & 0xC0u) == 0x80u) {
        --cut;
    }
    if (cut == 0) {
        // Single multi-byte codepoint takes the entire budget — give
        // up and pad with dots so the row's column total stays within
        // `max_chars`.
        return std::string(max_chars, '.');
    }
    text.resize(cut);
    text += kEllipsis;
    return text;
}

/// Build the per-issue stat blurb shown on each row. Reuses the
/// `relative_diff_pct` / `relative_amplitude_pct` helpers so the
/// numbers in the dashboard align with the numbers in the score.
[[nodiscard]] inline std::string format_stat_blurb(const SummaryIssue& issue) noexcept {
    const double amp = relative_amplitude_pct(issue.latest_samples);
    switch (issue.kind) {
        case SummaryIssue::Kind::RuleViolation: {
            const double signed_diff = issue.latest_args[1] == 0.0
                ? relative_diff_pct(issue.latest_args[0], issue.latest_args[1])
                : (issue.latest_args[0] - issue.latest_args[1])
                  / std::abs(issue.latest_args[1]) * 100.0;
            return fmt::format("rule  lhs={:.6g} vs rhs={:.6g}  (Δ {:+.1f}%, amp ±{:.0f}%)",
                issue.latest_args[0], issue.latest_args[1], signed_diff, amp);
        }
        case SummaryIssue::Kind::Regression: {
            const double signed_diff = issue.latest_args[0] == 0.0
                ? relative_diff_pct(issue.latest_args[1], issue.latest_args[0])
                : (issue.latest_args[1] - issue.latest_args[0])
                  / std::abs(issue.latest_args[0]) * 100.0;
            return fmt::format("regr  now={:.6g} vs prev={:.6g}  (Δ {:+.1f}%, amp ±{:.0f}%)",
                issue.latest_args[1], issue.latest_args[0], signed_diff, amp);
        }
        case SummaryIssue::Kind::SanityWarning: {
            std::string_view message{issue.sanity_message};
            if (auto dot = message.find('.'); dot != std::string_view::npos) {
                message = message.substr(0, dot);
            }
            constexpr std::size_t kSanityBlurbMaxChars = 40;
            return clip_to_width(fmt::format("sanity  {}", message), kSanityBlurbMaxChars);
        }
    }
    return {};
}

/// Build the per-issue presence-block ftxui Element row. Shows the
/// last `kBlockWindow` slots, oldest on the left → newest on the
/// right. Glyphs are emitted as one element-per-cell so ftxui sizes
/// them uniformly.
inline constexpr std::size_t kSummaryBlockWindow = 8;

[[nodiscard]] inline ftxui::Elements presence_blocks(const SummaryIssue& issue) noexcept {
    using namespace ftxui;
    const std::size_t total = issue.presence_per_report.size();
    const std::size_t take  = total < kSummaryBlockWindow ? total : kSummaryBlockWindow;
    const std::size_t start = total - take;
    Elements blocks;
    blocks.reserve(take);
    for (std::size_t i = start; i < total; ++i) {
        const bool present = issue.presence_per_report[i] != 0;
        // Default to "no data" (🟦) when the parallel array isn't sized
        // — only happens if `summary_mode_main` skipped the
        // `report_data_present` stamp, which would itself be a bug we
        // want to surface visually rather than hide as 🟩.
        const bool data_ok = i < issue.report_data_present.size()
            && issue.report_data_present[i] != 0;
        const auto glyph = summary_block_for(issue.kind, present, data_ok);
        blocks.push_back(text(std::string{glyph}));
    }
    return blocks;
}

/// Group ordering helper: a tuple key for the "metric → scope" header
/// pass. `std::pair<string, string>` would suffice; we keep a tiny
/// alias for readability.
using SummaryGroupKey = std::pair<std::string, std::string>;

/// Convert one fully-scored issue into its dashboard row. Severity
/// emoji + colour, kind label (bold), stat blurb, presence row, dim
/// rule name on the right.
[[nodiscard]] inline ftxui::Element render_issue_row(const SummaryIssue& issue) noexcept {
    using namespace ftxui;
    const auto& bucket = bucket_for(issue.score);
    Elements row;
    row.push_back(text(fmt::format(" {} ", bucket.emoji)) | color(bucket.color));
    row.push_back(text(format_stat_blurb(issue)));
    row.push_back(filler());
    auto blocks = presence_blocks(issue);
    Elements blocks_box;
    blocks_box.reserve(blocks.size() + 2);
    blocks_box.push_back(text("  "));
    for (auto& blk : blocks) blocks_box.push_back(std::move(blk));
    blocks_box.push_back(text("  "));
    row.push_back(hbox(std::move(blocks_box)));
    if (!issue.rule_name.empty()) {
        // Cap the rule_name so a long identifier doesn't push the
        // presence blocks past the screen edge in ftxui's auto-fit.
        constexpr std::size_t kRuleNameMaxChars = 24;
        row.push_back(text(clip_to_width(issue.rule_name, kRuleNameMaxChars)) | dim);
    }
    return hbox(std::move(row));
}

/// Render the whole dashboard. Two-pass aggregation: outer
/// metric-section header, inner scope-section sub-header, leaf
/// issue rows. Trailing legend doubles as the colour key.
///
/// We write the rendered ftxui screen via `std::fputs(stderr)` rather
/// than `JustLog` — this matches `print_violations` /
/// `print_regressions` / `print_sanity_warnings` (the V5–V7 stderr
/// printers) which already bypass fmtlog so end-of-run operator output
/// stays visible after the asynchronous log queue drains.
inline void render_summary(
    std::span<const SummaryIssue* const> sorted_issues,
    std::size_t   max_report_index,
    std::size_t   reports_present_count) noexcept
{
    using namespace ftxui;

    const auto banner = fmt::format(
        "BENCH FRAMEWORK — summary across {} report{} (report_1.json … report_{}.json)",
        reports_present_count, reports_present_count == 1 ? "" : "s",
        max_report_index);

    Elements rows;
    rows.push_back(text(banner) | bold);
    rows.push_back(separator());

    if (sorted_issues.empty()) {
        rows.push_back(text("No rule violations, regressions, or sanity warnings on disk.")
                       | color(Color::Green) | bold);
    } else {
        std::string current_metric;
        std::string current_scope;
        for (const auto* issue : sorted_issues) {
            if (issue->metric != current_metric) {
                if (!current_metric.empty()) rows.push_back(text(""));
                rows.push_back(text(fmt::format("\xe2\x96\xb6 {}",  // ▶
                                                issue->metric)) | bold);
                current_metric = issue->metric;
                current_scope.clear();
            }
            if (issue->scope_label != current_scope) {
                rows.push_back(text(fmt::format("  \xe2\x94\x94 {}",     // └
                                                issue->scope_label))
                               | color(Color::Cyan));
                current_scope = issue->scope_label;
            }
            rows.push_back(hbox({text("      "), render_issue_row(*issue)}));
        }
    }

    rows.push_back(separator());
    rows.push_back(text(fmt::format("{} issue{} displayed (sorted by score, highest first).",
                                     sorted_issues.size(),
                                     sorted_issues.size() == 1 ? "" : "s")));
    Elements legend_severity;
    legend_severity.push_back(text("Legend:  "));
    for (const auto& bucket : kSeverityBuckets) {
        legend_severity.push_back(text(fmt::format(" {} {}  ",
                                                    bucket.emoji, bucket.label))
                                   | color(bucket.color));
    }
    rows.push_back(hbox(std::move(legend_severity)));
    rows.push_back(text(
        "         "
        "\xf0\x9f\x9f\xa9 ok   "         // 🟩
        "\xf0\x9f\x9f\xa5 violation/regression   "  // 🟥
        "\xf0\x9f\x9f\xa8 sanity warning   "        // 🟨
        "\xf0\x9f\x9f\xa6 missing report data"      // 🟦
    ));

    auto document = vbox(std::move(rows));
    auto screen = Screen::Create(Dimension::Fit(document));
    Render(screen, document);
    std::fputs("\n", stderr);
    std::fputs(screen.ToString().c_str(), stderr);
    std::fputs("\n", stderr);
    std::fflush(stderr);
}

// ----------------- summary_mode_main -----------------

/// Build the rule-violation extractor lambda. Returns a lambda
/// suitable for `walk_report_array<...>`, capturing `sample_index`
/// by reference (caller-owned, lives for the whole report walk).
[[nodiscard]] inline auto rule_violation_extractor(const SampleIndex& sample_index) noexcept {
    return [&sample_index](simdjson::dom::object obj)
        -> std::optional<ExtractedIssue>
    {
        const auto location  = get_string_field(obj, "location");
        const auto benchmark = get_string_field(obj, "benchmark");
        const auto rule_name = get_string_field(obj, "rule_name");
        const auto test_case = get_string_field(obj, "test_case");
        const auto metric    = get_string_field(obj, "metric");

        auto sides = parse_violation_sides(obj);
        if (sides.empty()) return std::nullopt;
        // Single-key execution_tuple = self-compare (LHS == RHS); the
        // writer emits one entry to avoid a duplicate JSON key. We
        // copy the single side into the RHS slot so identity + score
        // see both halves of the rule.
        std::pair<std::string, ParamMap> lhs = std::move(sides.front());
        std::pair<std::string, ParamMap> rhs = sides.size() > 1
            ? std::move(sides[1])
            : lhs;
        const auto args = parse_violation_args(obj);

        ExtractedIssue ex;
        ex.kind     = SummaryIssue::Kind::RuleViolation;
        ex.identity = make_violation_identity(
            location, benchmark, rule_name,
            lhs.first, lhs.second, rhs.first, rhs.second,
            test_case, metric);

        ex.update_first = [metric, test_case, benchmark, rule_name,
                           lhs_algo = lhs.first, rhs_algo = rhs.first]
                          (SummaryIssue& issue) noexcept {
            issue.metric    = metric;
            issue.test_case = test_case;
            issue.benchmark = benchmark;
            issue.rule_name = rule_name;
            issue.scope_label = (lhs_algo == rhs_algo)
                ? fmt::format("{} @ {}", lhs_algo, test_case)
                : fmt::format("{} vs {} @ {}", lhs_algo, rhs_algo, test_case);
        };
        // `update_latest` only needs the LHS cell's coordinates for
        // the sample-index lookup. RHS is dropped here.
        ex.update_latest = [args, benchmark,
                            lhs_algo = std::move(lhs.first),
                            lhs_params = std::move(lhs.second),
                            test_case, metric, &sample_index]
                           (SummaryIssue& issue) noexcept {
            issue.latest_args = args;
            // Amplitude reads the LHS cell's samples (the side that
            // exceeded the rule). V8 always evaluates rule violations
            // against `all_regions`; per-region rules would need this
            // hard-coded "all_regions" replaced with the rule's region.
            issue.latest_samples = samples_from_index(
                sample_index, benchmark, lhs_algo, lhs_params,
                test_case, "all_regions", metric);
        };
        return ex;
    };
}

/// Build the regression extractor lambda. Same shape as the rule
/// extractor; the regression's stored `region` flows into both the
/// identity and the sample lookup.
[[nodiscard]] inline auto regression_extractor(const SampleIndex& sample_index) noexcept {
    return [&sample_index](simdjson::dom::object obj)
        -> std::optional<ExtractedIssue>
    {
        const auto location       = get_string_field(obj, "location");
        const auto benchmark      = get_string_field(obj, "benchmark");
        const auto rule_name      = get_string_field(obj, "rule_name");
        const auto algorithm_name = get_string_field(obj, "algorithm_name");
        const auto region         = get_string_field(obj, "region");
        const auto metric         = get_string_field(obj, "metric");
        const auto aggregate      = get_string_field(obj, "aggregate");
        const double previously   = get_double_field(obj, "previously");
        const double now_value    = get_double_field(obj, "now");
        auto params = parse_violating_tuple_first(obj);
        auto test_case = parse_test_case_first(obj);

        ExtractedIssue ex;
        ex.kind     = SummaryIssue::Kind::Regression;
        ex.identity = make_regression_identity(
            location, benchmark, rule_name, algorithm_name, params,
            test_case, region, metric, aggregate);

        ex.update_first = [metric, test_case, benchmark, rule_name,
                           algorithm_name, region, aggregate]
                          (SummaryIssue& issue) noexcept {
            issue.metric    = metric;
            issue.test_case = test_case;
            issue.benchmark = benchmark;
            issue.rule_name = rule_name;
            // Two regressions on the same (algorithm, test_case,
            // region) but different aggregates (p50 vs p99) live as
            // separate issues; surface the aggregate suffix so the
            // dashboard rows are visually distinct. The default p50
            // is the regression-rule baseline so we omit it to keep
            // the common case terse.
            std::string region_suffix = (region == "all_regions")
                ? std::string{}
                : fmt::format(" ({})", region);
            std::string aggregate_suffix = (aggregate.empty() || aggregate == "p50")
                ? std::string{}
                : fmt::format(" [{}]", aggregate);
            issue.scope_label = fmt::format("{} @ {}{}{}",
                algorithm_name, test_case, region_suffix, aggregate_suffix);
        };
        ex.update_latest = [previously, now_value, benchmark, algorithm_name,
                            params = std::move(params), test_case, region,
                            metric, &sample_index]
                           (SummaryIssue& issue) noexcept {
            issue.latest_args = {previously, now_value};
            issue.latest_samples = samples_from_index(
                sample_index, benchmark, algorithm_name, params,
                test_case, region, metric);
        };
        return ex;
    };
}

/// Build the sanity-warning extractor lambda. Captures nothing —
/// sanity warnings carry no per-report sample lookup. Returned by
/// value so the call site can plug it directly into
/// `walk_report_array`.
[[nodiscard]] inline auto sanity_warning_extractor() noexcept {
    return [](simdjson::dom::object obj) -> std::optional<ExtractedIssue> {
        const auto type    = get_string_field(obj, "type");
        const auto metric  = get_string_field(obj, "metric");
        const auto message = get_string_field(obj, "message");

        ExtractedIssue ex;
        ex.kind     = SummaryIssue::Kind::SanityWarning;
        ex.identity = make_sanity_identity(type, metric, message);
        ex.update_first = [type, metric, message](SummaryIssue& issue) noexcept {
            issue.metric         = metric;
            issue.scope_label    = "(global)";
            issue.rule_name      = type;
            issue.sanity_message = message;
        };
        ex.update_latest = [message](SummaryIssue& issue) noexcept {
            issue.sanity_message = message;
        };
        return ex;
    };
}

/// One-shot V8-style summary printer. Walks every report on disk,
/// scores issues, and renders the aggregated dashboard to stderr via
/// `render_summary`. Used as the read-only fallback when `--ui` is
/// requested but stdin / stderr is not a TTY (per V14 §11.1 / §8.1)
/// and from `ui_mode_main`'s startup probe when the user explicitly
/// requested ASCII-only output but no TTY is attached.
[[nodiscard]] inline int render_summary_to_stderr_once(const std::string& report_dir) noexcept {
    const std::filesystem::path report_dir_path{report_dir};
    const auto max_index = max_existing_report_index(report_dir_path).value_or(0);
    if (max_index == 0) {
        logi("[bench_framework] summary: no reports under '{}'", report_dir);
        return 0;
    }

    HistoricalReportCache cache{report_dir_path};
    std::map<std::string, SummaryIssue> aggregation;
    std::vector<std::uint8_t> report_data_present(max_index, 0);
    std::size_t reports_present_count = 0;

    for (std::size_t report_index = 1; report_index <= max_index; ++report_index) {
        auto* report = cache.fetch(report_index);
        if (report == nullptr) continue;     // gap → presence stays 0
        report_data_present[report_index - 1] = 1;
        ++reports_present_count;
        const auto sample_index = build_sample_index(report->root);
        walk_report_array(report->root, "rule_violations",
            report_index, max_index, aggregation,
            rule_violation_extractor(sample_index));
        walk_report_array(report->root, "regressions",
            report_index, max_index, aggregation,
            regression_extractor(sample_index));
        walk_report_array(report->root, "sanity_warnings",
            report_index, max_index, aggregation,
            sanity_warning_extractor());
    }

    if (reports_present_count == 0) {
        logi("[bench_framework] summary: every report under '{}' failed to parse",
             report_dir);
        return 0;
    }

    // Stamp the data-presence array onto every issue (the writer
    // walks issues, not reports).
    for (auto& [_identity, issue] : aggregation) {
        issue.report_data_present = report_data_present;
        DEBUG_ASSERT(issue.report_data_present.size() == issue.presence_per_report.size(),
            "presence and report_data_present must be the same length");
        issue.score = score_summary_issue(issue);
    }

    auto sorted_issues = sort_summary_issues(aggregation);
    render_summary(std::span<const SummaryIssue* const>{
                       sorted_issues.data(), sorted_issues.size()},
                   max_index, reports_present_count);
    return 0;
}

// ----------------- V14: --ui interactive entry point -----------------

/// Forward declaration for the interactive ftxui dashboard. Defined in
/// the sibling implementation unit `interactive.cppm`. Invoked from
/// `run_impl` when `--ui` is on the command line and stdin / stderr
/// are interactive TTYs. When the TTY check fails the entry point
/// itself dispatches `render_summary_to_stderr_once` so a CI-piped
/// `--ui` invocation still produces V8-style output (per V14 §11.1).
[[nodiscard]] int ui_mode_main(const std::string& report_dir, const ParsedCli& cli) noexcept;

// ----------------- CLI parsing -----------------

/// Parse the framework CLI flags out of `argv`. Unknown flags are
/// silently ignored (the user may layer their own flag parsing on top by
/// inspecting argv before calling `run`). Runs first inside `run_impl` so a
/// `--report` override is applied before any directory creation.
[[nodiscard]] inline ParsedCli parse_cli(int argc, char** argv) noexcept {
    ParsedCli parsed;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--fast") {
            parsed.fast_filter = true;
        } else if (arg == "--ui") {
            parsed.ui_mode = true;
        } else if (arg == "--ui-ascii") {
            parsed.ui_ascii_fallback = true;
        } else if (arg == "--ui-test-mode") {
            parsed.ui_test_mode = true;
        } else if (arg == "--threads" && (i + 1) < argc) {
            const std::string_view text{argv[i + 1]};
            int threads = 0;
            const auto* const begin = text.data();
            const auto* const end   = text.data() + text.size();
            const auto result = std::from_chars(begin, end, threads);
            if (result.ec != std::errc{} || result.ptr != end || threads <= 0) {
                loge("[bench_framework] --threads requires a positive integer (got '{}')",
                     text);
            } else {
                parsed.threads_override = static_cast<unsigned>(threads);
            }
            ++i;
        } else if (arg == "--report" && (i + 1) < argc) {
            parsed.report_override = std::string{argv[i + 1]};
            ++i;
        } else if (arg == "--memory-buffer" && (i + 1) < argc) {
            // V12: `--memory-buffer N` (0..50, default 10). Use
            // std::from_chars to honour main/DOCS/GUIDELINES/CODE_STYLE__DOC0145.md's "no exceptions
            // in framework code" — the existing --threads parser used
            // try/catch around std::stoi, but we don't extend that
            // pattern.
            const std::string_view text{argv[i + 1]};
            int pct = 0;
            const auto* const begin = text.data();
            const auto* const end   = text.data() + text.size();
            const auto result = std::from_chars(begin, end, pct);
            if (result.ec != std::errc{} || result.ptr != end) {
                loge("[bench_framework] --memory-buffer requires an integer (got '{}')",
                     text);
            } else if (pct < 0 || pct > 50) {
                loge("[bench_framework] --memory-buffer must be in [0, 50] "
                     "(got {}); falling back to default 10", pct);
            } else {
                parsed.memory_buffer_pct = static_cast<double>(pct);
            }
            ++i;
        }
    }
    return parsed;
}

// ----------------- Progress dashboard -----------------

/// Severity buckets for the colour-shifting progress gauge. Picked by
/// percentage from the lowest matching threshold. Keep ordered low → high.
/// `palette` is the enumerator (literal type) — converting to
/// `ftxui::Color` happens at the call site since Color's constructor
/// is not constexpr.
struct GaugeBucket {
    float                   min_fraction;
    ftxui::Color::Palette256 palette;
};
inline constexpr std::array<GaugeBucket, 4> kGaugeBuckets{ {
    { 0.00f, static_cast<ftxui::Color::Palette256>(ftxui::Color::Red)     },
    { 0.25f, ftxui::Color::Orange1                                       },
    { 0.50f, static_cast<ftxui::Color::Palette256>(ftxui::Color::Yellow) },
    { 0.75f, static_cast<ftxui::Color::Palette256>(ftxui::Color::Green)  },
} };

[[nodiscard]] inline constexpr ftxui::Color::Palette256
gauge_palette(float fraction) noexcept {
    auto picked = kGaugeBuckets.front().palette;
    for (const auto& bucket : kGaugeBuckets) {
        if (fraction >= bucket.min_fraction) picked = bucket.palette;
    }
    return picked;
}

/// Wrap `gauge_palette` in an `ftxui::Color`. The Color constructor
/// itself is not `constexpr`, so this layer stays at runtime.
[[nodiscard]] inline ftxui::Color gauge_color(float fraction) noexcept {
    return ftxui::Color(gauge_palette(fraction));
}

/// Render a coloured gauge of fixed width. Returns an ftxui Element so
/// the caller can compose with hbox/text. Fill width is rounded to the
/// nearest integer; an entirely-empty bar still displays as
/// `[░░░░...]` so the layout doesn't shift. Values outside `[0, 1]`
/// are clamped before the lookup.
[[nodiscard]] inline ftxui::Element coloured_gauge(
    float fraction, int total_cells) noexcept
{
    constexpr std::string_view kFull  = "\xe2\x96\x88";   // █
    constexpr std::string_view kEmpty = "\xe2\x96\x91";   // ░
    if (total_cells <= 0) return ftxui::text("[]");
    const float clamped = std::clamp(fraction, 0.0f, 1.0f);
    const int filled_cells = std::clamp(
        static_cast<int>(std::lround(
            static_cast<double>(clamped) * static_cast<double>(total_cells))),
        0, total_cells);
    std::string bar;
    bar.reserve(static_cast<std::size_t>(total_cells) * kFull.size() + 2);
    bar.push_back('[');
    for (int i = 0; i < total_cells; ++i) bar += (i < filled_cells ? kFull : kEmpty);
    bar.push_back(']');
    return ftxui::text(bar) | ftxui::color(gauge_color(clamped));
}

/// Convert raw bytes to a `double` in GiB. Inlined as a `constexpr`
/// helper so callers don't repeat the `1024^3` divisor.
[[nodiscard]] inline constexpr double bytes_to_gib(std::uint64_t bytes) noexcept {
    constexpr double kGib = 1024.0 * 1024.0 * 1024.0;
    return static_cast<double>(bytes) / kGib;
}

/// Background thread body for the progress dashboard. Two paths:
/// 1. **TTY**: render an ftxui document to stderr every 200 ms with
///    one row per test_case (active groups underneath), a colour-
///    shifting gauge per row, and a `[memory X / Y GiB]` header.
/// 2. **Non-TTY (CI / pipes)**: emit a JustLog info line every 5 s
///    with one line per test_case still in flight.
// ----------------- V12 row classification + glyph table -----------------

/// Possible states of a test_case row in the dashboard. Renderer picks
/// the most-urgent state across the test case's groups.
enum class TestCaseRowState : std::uint8_t {
    Done,           // tc->finish_ns set, no failure flags ⇒ ✅
    Aborted,        // some group has aborted == true        ⇒ ❌
    InputRetry,     // some group has input_retry > 0        ⇒ ▶ ⛔
    CleanupRetry,   // some group has cleanup_retry > 0      ⇒ ▶ 🧹
    IdleWait,       // some group has been idle > kMaxIdle/2 ⇒ ▶ 🕑
    Running,        // default running                       ⇒ ▶ 🔄
};

/// Walk every group of `tc` and pick the most-urgent state. The order
/// (Aborted > InputRetry > CleanupRetry > Done > IdleWait > Running)
/// mirrors the user spec — failures and aborts surface first; an
/// already-finished test case stays "Done" even if its idle clock
/// briefly armed before it completed.
[[nodiscard]] inline TestCaseRowState classify_test_case(
    const ProgressTestCase& tc) noexcept
{
    bool any_aborted = false, any_input_retry = false;
    bool any_cleanup_retry = false, any_idle_long = false;
    for (const auto& group_ptr : tc.groups) {
        if (group_ptr->aborted.load(std::memory_order_acquire))
            any_aborted = true;
        if (group_ptr->input_retry_attempt.load(std::memory_order_acquire) > 0)
            any_input_retry = true;
        if (group_ptr->cleanup_retry_attempt.load(std::memory_order_acquire) > 0)
            any_cleanup_retry = true;
        const auto idle_start = group_ptr->idle_start_ns.load(std::memory_order_acquire);
        if (idle_start != 0) {
            const auto elapsed_ns = static_cast<std::int64_t>(
                                        bioseq::instr::clock::now_ns()) - idle_start;
            const auto half_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    BatchScheduler::kMaxIdleWaitingTime).count() / 2;
            if (elapsed_ns > half_ns) any_idle_long = true;
        }
    }
    if (any_aborted)         return TestCaseRowState::Aborted;
    if (any_input_retry)     return TestCaseRowState::InputRetry;
    if (any_cleanup_retry)   return TestCaseRowState::CleanupRetry;
    if (tc.finish_ns.load(std::memory_order_acquire) != 0)
                             return TestCaseRowState::Done;
    if (any_idle_long)       return TestCaseRowState::IdleWait;
    return TestCaseRowState::Running;
}

/// Header glyph + colour per row state. Table-driven so adding a new
/// state means adding a row, not editing branch ladders.
struct RowGlyph {
    TestCaseRowState  state;
    std::string_view  glyph;        // UTF-8 (may include emoji)
    ftxui::Color      tint;
};

inline const std::array<RowGlyph, 6> kRowGlyphs{ {
    { TestCaseRowState::Aborted,
      "\xe2\x9d\x8c",                      ftxui::Color::RedLight   },  // ❌
    { TestCaseRowState::InputRetry,
      "\xe2\x96\xb6 \xe2\x9b\x94",         ftxui::Color::Red        },  // ▶ ⛔
    { TestCaseRowState::CleanupRetry,
      "\xe2\x96\xb6 \xf0\x9f\xa7\xb9",     ftxui::Color::Magenta    },  // ▶ 🧹
    { TestCaseRowState::IdleWait,
      "\xe2\x96\xb6 \xf0\x9f\x95\x91",     ftxui::Color::Yellow     },  // ▶ 🕑
    { TestCaseRowState::Running,
      "\xe2\x96\xb6 \xf0\x9f\x94\x84",     ftxui::Color::Yellow     },  // ▶ 🔄
    { TestCaseRowState::Done,
      "\xe2\x9c\x85",                      ftxui::Color::Green      },  // ✅
} };

[[nodiscard]] inline const RowGlyph& glyph_for(TestCaseRowState state) noexcept {
    for (const auto& entry : kRowGlyphs) {
        if (entry.state == state) return entry;
    }
    return kRowGlyphs.front();
}

/// Build the detail-rows that hang off a failing/idle row. Returned as
/// a list of ftxui Elements ready to be flattened into the renderer's
/// `rows` vector.
[[nodiscard]] inline ftxui::Elements build_detail_rows(
    const ProgressTestCase& tc, TestCaseRowState state) noexcept
{
    using namespace ftxui;
    Elements detail;
    if (state == TestCaseRowState::Running || state == TestCaseRowState::Done) {
        return detail;
    }
    for (const auto& group_ptr : tc.groups) {
        const auto input_n   = group_ptr->input_retry_attempt.load(std::memory_order_acquire);
        const auto cleanup_n = group_ptr->cleanup_retry_attempt.load(std::memory_order_acquire);
        const auto aborted   = group_ptr->aborted.load(std::memory_order_acquire);
        const auto idle_start = group_ptr->idle_start_ns.load(std::memory_order_acquire);

        // Pick the matching detail content per state.
        std::string header_line, message, trace;
        Color tint = Color::GrayLight;

        // Re-read counters under `failure_mu` after locking so a race
        // between the fast-path acquire-load above and the writer's
        // store-zero (in `progress_clear_retry_failure`) doesn't
        // produce a "retry N/5 with empty strings" frame. The writer
        // protocol is "store(0, release) BEFORE clearing strings under
        // the lock"; revalidating under the lock means we never paint
        // a stale N when the strings are already empty.
        if (state == TestCaseRowState::Aborted && aborted) {
            std::lock_guard<std::mutex> guard(group_ptr->failure_mu);
            message = group_ptr->aborted_reason;
            header_line = fmt::format(
                "group {} aborted",
                group_ptr->group_index + 1);
            tint = Color::RedLight;
        } else if (state == TestCaseRowState::InputRetry && input_n > 0) {
            std::lock_guard<std::mutex> guard(group_ptr->failure_mu);
            const auto fresh_n = group_ptr->input_retry_attempt
                                    .load(std::memory_order_relaxed);
            if (fresh_n == 0) continue;       // record was cleared while we waited
            message = group_ptr->input_failure_message;
            trace   = group_ptr->input_failure_trace;
            header_line = fmt::format(
                "group {} on_input failure [retry {}/{}]",
                group_ptr->group_index + 1,
                static_cast<int>(fresh_n),
                kMaxTaskInputHandlerErrorCount);
            tint = Color::Red;
        } else if (state == TestCaseRowState::CleanupRetry && cleanup_n > 0) {
            std::lock_guard<std::mutex> guard(group_ptr->failure_mu);
            const auto fresh_n = group_ptr->cleanup_retry_attempt
                                    .load(std::memory_order_relaxed);
            if (fresh_n == 0) continue;       // record was cleared while we waited
            message = group_ptr->cleanup_failure_message;
            trace   = group_ptr->cleanup_failure_trace;
            header_line = fmt::format(
                "group {} on_cleanup failure [retry {}/{}]",
                group_ptr->group_index + 1,
                static_cast<int>(fresh_n),
                kMaxTaskInputHandlerErrorCount);
            tint = Color::Magenta;
        } else if (state == TestCaseRowState::IdleWait && idle_start != 0) {
            const auto elapsed_ns = static_cast<std::int64_t>(
                                        bioseq::instr::clock::now_ns()) - idle_start;
            const auto half_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    BatchScheduler::kMaxIdleWaitingTime).count() / 2;
            if (elapsed_ns <= half_ns) continue;
            const auto elapsed_s = elapsed_ns / 1'000'000'000;
            const auto budget_s  = std::chrono::duration_cast<std::chrono::seconds>(
                                       BatchScheduler::kMaxIdleWaitingTime).count();
            header_line = fmt::format(
                "group {} waiting {}s / {}s — no peers running, system memory unavailable",
                group_ptr->group_index + 1, elapsed_s, budget_s);
            tint = Color::Yellow;
        } else {
            continue;     // not the matching state for this group
        }

        // The `└ group X …` header sits at column 4 with a 1-cell
        // box-drawing glyph + a space, so its content starts at column 6.
        // Message and trace lines hang under it at column 8 — visibly
        // deeper than the header so the nesting reads correctly.
        detail.push_back(hbox({
            text("    \xe2\x94\x94 "),                  // └ at column 4
            text(header_line) | color(tint),
        }));
        if (!message.empty()) {
            detail.push_back(hbox({
                text("        "),                       // 8 spaces
                paragraph(message) | color(Color::GrayLight),
            }));
        }
        if (!trace.empty()) {
            // Split trace on '\n' and emit each line as a SINGLE text()
            // element with the indent prepended into the string. We do
            // NOT wrap it in `hbox({text(indent), text(line)})`: ftxui's
            // hbox shrinks children proportionally when the total width
            // exceeds the screen, and a long demangled C++ frame name
            // would collapse the indent prefix to 1-2 spaces for that
            // row. A single text() element renders left-to-right and
            // simply clips at the screen edge, so the indent stays a
            // stable 8 chars regardless of how long the line is.
            //
            // We always pass `color=false` to cpptrace's `to_string`
            // (see `clip_trace`) because ftxui's `text()` does NOT
            // interpret ANSI escape sequences — colour codes would
            // render as literal `[34m…[0m` text. The visual tint
            // comes from ftxui's `| color(Color::GrayDark)` decorator.
            std::size_t cursor = 0;
            while (cursor < trace.size()) {
                const auto newline = trace.find('\n', cursor);
                std::string_view line = (newline == std::string::npos)
                    ? std::string_view{trace}.substr(cursor)
                    : std::string_view{trace}.substr(cursor, newline - cursor);
                std::string indented{"        "};
                indented.append(line);
                detail.push_back(text(std::move(indented)) | color(Color::GrayDark));
                if (newline == std::string::npos) break;
                cursor = newline + 1;
            }
        }
    }
    return detail;
}

/// V16 grace period: a Done row stays visible for this long after
/// `finish_ns` is stamped, then disappears. Failures (Aborted /
/// InputRetry / CleanupRetry) are *never* hidden — the operator must
/// see them.
inline constexpr std::chrono::seconds kCompletedRowGracePeriod{5};

/// Free helper — the auto-hide predicate. Used by both the per-row
/// loop (skip rendering this row) and the all-hidden collapse loop
/// (collapse the entire benchmark when every row is past the grace).
[[nodiscard]] inline bool row_is_hideable(TestCaseRowState state,
                                            std::int64_t finish_ns,
                                            std::int64_t now_ns) noexcept
{
    if (state != TestCaseRowState::Done) return false;
    if (finish_ns == 0) return false;
    constexpr auto grace_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            kCompletedRowGracePeriod).count();
    return (now_ns - finish_ns) > grace_ns;
}

inline void render_progress_loop(ProgressState& progress) noexcept {
    using namespace std::chrono_literals;
    using namespace ftxui;
    constexpr int kBarCells = 16;

    const bool tty = progress.tty_render.load(std::memory_order_relaxed);
    if (!tty) {
        constexpr auto kPollInterval = std::chrono::milliseconds{200};
        constexpr int  kTicksPerLog  = 25;     // 25 × 200 ms == 5 s
        int ticks_until_next_log = kTicksPerLog;
        while (progress.running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(kPollInterval);
            if (--ticks_until_next_log > 0) continue;
            ticks_until_next_log = kTicksPerLog;
            for (const auto& per : progress.per_benchmark) {
                if (!per->setup_done.load(std::memory_order_acquire)) continue;
                for (const auto& tc : per->test_cases) {
                    const auto done = tc->tuple_done.load(std::memory_order_relaxed);
                    const auto total = tc->tuple_total.load(std::memory_order_relaxed);
                    if (tc->finish_ns.load(std::memory_order_relaxed) != 0) continue;
                    if (tc->start_ns.load(std::memory_order_relaxed) == 0) continue;
                    logi("[bench_framework] [{}/{}] tuples {}/{}",
                         per->benchmark_key, tc->test_case_name,
                         done, total);
                }
            }
        }
        return;
    }

    std::string reset_position;
    int previous_frame_height = 0;
    auto render_once = [&] {
        Elements rows;
        const auto now_ns_value = static_cast<std::int64_t>(
                                       bioseq::instr::clock::now_ns());

        // Header line + budget readout. V16: read pending/running +
        // reservation totals from the scheduler when present, falling
        // back to MemoryBudget for the system-wide numbers.
        rows.push_back(text("bench_framework — live progress") | bold);
        if (progress.scheduler != nullptr) {
            const auto snap = progress.scheduler->live_snapshot();
            const auto over_cap = snap.used_bytes >= snap.cap_bytes;
            rows.push_back(hbox({
                text("[budget free ") | color(Color::GrayLight),
                text(fmt::format("{:.2f}", bytes_to_gib(snap.available_bytes)))
                    | color(snap.available_bytes > 0 ? Color::Green : Color::Red),
                text(fmt::format(" / cap {:.2f} GiB · sys used ",
                                 bytes_to_gib(snap.cap_bytes))),
                text(fmt::format("{:.2f}", bytes_to_gib(snap.used_bytes)))
                    | color(over_cap ? Color::RedLight : Color::Yellow),
                text(fmt::format(" / {:.2f} GiB · reserved ",
                                 bytes_to_gib(snap.total_bytes))),
                text(fmt::format("{:.2f}", bytes_to_gib(snap.reserved_bytes)))
                    | color(Color::Cyan),
                text(fmt::format(" GiB · pending {} running {} · buffer {:.0f}%]",
                                 snap.pending_count, snap.running_count,
                                 progress.scheduler->memory_buffer_pct())),
            }) | color(Color::GrayLight));
        }
        rows.push_back(separator() | color(Color::GrayDark));

        for (const auto& per : progress.per_benchmark) {
            if (!per->setup_done.load(std::memory_order_acquire)) {
                rows.push_back(text(fmt::format("• {} ({})",
                                               per->benchmark_key, per->title))
                                | color(Color::Cyan));
                continue;
            }
            // V16 G2.b — collapse the benchmark when every test case is
            // past the grace period. Empty test_cases vectors stay as
            // "all done" too (degenerate but consistent).
            bool all_hidden = !per->test_cases.empty();
            for (const auto& tc : per->test_cases) {
                const auto state = classify_test_case(*tc);
                const auto finish_ns = tc->finish_ns.load(std::memory_order_relaxed);
                if (!row_is_hideable(state, finish_ns, now_ns_value)) {
                    all_hidden = false;
                    break;
                }
            }
            if (all_hidden) {
                rows.push_back(text(fmt::format("• {} \xe2\x9c\x93 (all done)",
                                               per->benchmark_key))
                                | color(Color::GrayLight));
                continue;
            }

            // One header row per benchmark.
            rows.push_back(text(fmt::format("• {} ({})", per->benchmark_key, per->title))
                            | color(Color::Cyan));
            for (const auto& tc : per->test_cases) {
                const auto tuple_done   = tc->tuple_done.load(std::memory_order_relaxed);
                const auto tuple_total  = tc->tuple_total.load(std::memory_order_relaxed);
                const auto start_ns     = tc->start_ns.load(std::memory_order_relaxed);
                const auto finish_ns    = tc->finish_ns.load(std::memory_order_relaxed);
                const auto state = classify_test_case(*tc);

                // V16 G2.a — auto-hide Done rows past the grace period.
                if (row_is_hideable(state, finish_ns, now_ns_value)) continue;

                const float fraction    = tuple_total == 0
                    ? 1.0f
                    : static_cast<float>(tuple_done) / static_cast<float>(tuple_total);
                const auto elapsed_ns   = start_ns == 0
                    ? std::int64_t{0}
                    : ((finish_ns == 0 ? now_ns_value : finish_ns) - start_ns);
                const auto elapsed_s    = elapsed_ns / 1'000'000'000;
                const auto& glyph = glyph_for(state);

                // Headline row, by state.
                Elements headline;
                headline.push_back(text(fmt::format("  {} ", glyph.glyph))
                                    | color(glyph.tint));
                headline.push_back(text(tc->test_case_name) | size(WIDTH, EQUAL, 24));

                if (state == TestCaseRowState::Done) {
                    headline.push_back(text(fmt::format("tuples {}/{}",
                        tuple_done, tuple_total)));
                    headline.push_back(text(fmt::format("  done in {} s", elapsed_s))
                                       | color(Color::GrayLight));
                } else if (state == TestCaseRowState::Aborted) {
                    headline.push_back(text(fmt::format("aborted at {}/{}",
                        tuple_done, tuple_total)) | color(Color::RedLight));
                } else if (state == TestCaseRowState::InputRetry
                        || state == TestCaseRowState::CleanupRetry
                        || state == TestCaseRowState::IdleWait) {
                    headline.push_back(text(fmt::format("tuples {}/{}",
                        tuple_done, tuple_total)));
                } else {
                    headline.push_back(text(fmt::format("tuples {}/{}  ",
                        tuple_done, tuple_total)));
                    headline.push_back(coloured_gauge(fraction, kBarCells));
                    headline.push_back(text(fmt::format("  {:>3.0f}%  Running... {}s",
                        static_cast<double>(fraction) * 100.0, elapsed_s))
                        | color(Color::GrayLight));
                }
                rows.push_back(hbox(std::move(headline)));

                // Detail rows for non-running states (failures / idle).
                auto details = build_detail_rows(*tc, state);
                for (auto& row : details) rows.push_back(std::move(row));

                // Per-group gauge rows still appear for normal Running.
                if (state == TestCaseRowState::Running) {
                    for (const auto& group_ptr : tc->groups) {
                        if (!group_ptr->active.load(std::memory_order_relaxed)) continue;
                        const auto done  = group_ptr->done.load(std::memory_order_relaxed);
                        const float gfrac = group_ptr->total == 0 ? 1.0f
                            : static_cast<float>(done) / static_cast<float>(group_ptr->total);
                        std::string copied_label;
                        {
                            std::lock_guard<std::mutex> guard(group_ptr->label_mu);
                            copied_label = group_ptr->label;
                        }
                        rows.push_back(hbox({
                            text("    \xe2\x94\x94 "),                       // └
                            text(fmt::format("group {}", group_ptr->group_index + 1))
                                | size(WIDTH, EQUAL, 10),
                            coloured_gauge(gfrac, kBarCells),
                            text(fmt::format("  {:>3.0f}%  ",
                                static_cast<double>(gfrac) * 100.0))
                                | color(Color::GrayLight),
                            text(copied_label) | color(Color::GrayLight),
                        }));
                    }
                }
            }
        }
        auto document = vbox(std::move(rows));
        auto screen = Screen::Create(
            Dimension::Fit(document, /*extend_beyond_screen=*/true));
        Render(screen, document);
        const int current_height = screen.dimy();

        // V16 G2.b — when the dashboard shrinks (a row hid), explicit
        // cursor-up + clear-to-end-of-screen prevents stale rows from
        // lingering below the new top. ResetPosition(clear=true) on
        // its own only clears the new screen's footprint.
        if (previous_frame_height > current_height && previous_frame_height > 0) {
            std::fputs(fmt::format("\x1b[{}A\x1b[J",
                                    previous_frame_height).c_str(), stderr);
        } else if (!reset_position.empty()) {
            std::fputs(reset_position.c_str(), stderr);
        }
        std::fputs(screen.ToString().c_str(), stderr);
        std::fflush(stderr);
        reset_position = screen.ResetPosition();
        previous_frame_height = current_height;
    };

    while (progress.running.load(std::memory_order_relaxed)) {
        render_once();
        std::this_thread::sleep_for(200ms);
    }
    render_once();   // final repaint with all bars at 100 %
    std::fputs("\n", stderr);
    std::fflush(stderr);
}

// ----------------- run_impl -----------------

/// Top-level orchestrator. Owns: CLI parsing, TBB arena setup, scheduler,
/// the ProgressState lifetime, the violations + reports maps,
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

    if (cli.ui_mode) {
        if (cli.fast_filter) {
            logw("[bench_framework] --ui ignores --fast (no benchmarks run)");
        }
        if (cli.threads_override.has_value()) {
            logw("[bench_framework] --ui ignores --threads (no benchmarks run)");
        }
        return ui_mode_main(report_dir, cli);
    }

    logi("[bench_framework] starting: report_dir={} data_dir={} benchmarks={}",
                 report_dir, data_dir, benchmarks.size());

    const auto thread_count = cli.threads_override.has_value()
        ? *cli.threads_override
        : std::max<unsigned>(1u, std::thread::hardware_concurrency() == 0
                                  ? 1u
                                  : std::thread::hardware_concurrency() - 1u);
    logi("[bench_framework] thread_count={}", thread_count);

    // Capture the run's identity once, up front. Used both by the
    // report writer (the `meta` block) and surfaced in stderr logs so
    // the operator can correlate a run with a commit.
    const auto run_meta = capture_run_meta(cli.fast_filter);
    logi("[bench_framework] commit={} dirty={} created={} fast={}",
         run_meta.commit.empty() ? std::string_view{"<no-git>"}
                                 : std::string_view{run_meta.commit},
         run_meta.dirty, run_meta.created, run_meta.fast);

    // Filter benchmarks by --fast. The `--fast` field lives on the per-
    // benchmark config, not on `ErasedBenchmark`, so the filter actually
    // runs inside each closure after parse. `selected` therefore mirrors
    // `benchmarks` 1:1 — we just take ownership.
    std::vector<ErasedBenchmark> selected = std::move(benchmarks);

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

    std::mutex        violations_mu;
    std::mutex        reports_mu;
    std::mutex        diagnostics_mu;
    std::mutex        regression_rules_mu;
    std::vector<Violation> violations;
    std::vector<ValidationDiag> diagnostics;
    std::map<std::string, std::unique_ptr<BenchmarkExecutionState>> reports;
    RegressionRulesByConfig regression_rules_by_config;

    // Process-wide memory budget shared by every benchmark closure.
    // V12: budget recomputed dynamically from hwinfo on every check;
    // the only knob is the safety buffer percentage.
    MemoryBudget memory_budget(cli.memory_buffer_pct);
    {
        const auto sys = snapshot_system_memory();
        const auto cap = MemoryBudget::cap_for(sys.total_bytes,
                                                cli.memory_buffer_pct);
        logi("[bench_framework] memory budget: hwinfo total={:.2f} GiB free={:.2f} GiB "
             "buffer={:.0f}% cap={:.2f} GiB",
             bytes_to_gib(sys.total_bytes),
             bytes_to_gib(sys.free_bytes),
             cli.memory_buffer_pct,
             bytes_to_gib(cap));
        if (sys.total_bytes == std::numeric_limits<std::uint64_t>::max()) {
            logw("[bench_framework] hwinfo returned 0 for total RAM — budget "
                 "checks are effectively disabled (treated as infinite memory)");
        }
    }

    // V16: process-wide scheduler. Owns batch admission + the TBB
    // task pool that runs the bodies. Constructed BEFORE the progress
    // thread so the renderer can read `scheduler.live_snapshot()`.
    BatchScheduler scheduler(memory_budget, bench_arena);

    ProgressState progress;
    progress.tty_render.store(::isatty(STDERR_FILENO) != 0, std::memory_order_relaxed);
    progress.scheduler     = &scheduler;
    progress.per_benchmark.reserve(selected.size());
    for (const auto& erased : selected) {
        auto entry = std::make_unique<ProgressState::Per>();
        entry->title = erased.title;
        entry->benchmark_key = erased.benchmark_key;
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
                context.data_dir                  = data_dir;
                context.report_dir                = report_dir;
                context.fast_filter               = cli.fast_filter;
                context.progress                  = &progress;
                context.benchmark_index           = benchmark_index;
                context.violations_mu             = &violations_mu;
                context.violations                = &violations;
                context.reports_mu                = &reports_mu;
                context.reports                   = &reports;
                context.diagnostics_mu            = &diagnostics_mu;
                context.diagnostics               = &diagnostics;
                context.regression_rules_mu       = &regression_rules_mu;
                context.regression_rules_by_config = &regression_rules_by_config;
                context.scheduler                  = &scheduler;

                erased.execute(context);
            });
    });

    // Defence-in-depth: every closure already drains its own batches
    // via `wait_for_benchmark` before returning. Should be a no-op,
    // but guarantees correctness if a future closure forgets the wait.
    scheduler.wait_for_completion();

    progress.running.store(false, std::memory_order_relaxed);
    progress_thread.join();

    // Cross-config validation of regression rules. Runs serially after
    // every closure has registered its state (so we can resolve
    // `benchmarks` membership and metric existence in one place).
    {
        auto regression_validation_diags = semantic_validate_regression_rules(
            regression_rules_by_config, reports);
        std::lock_guard<std::mutex> guard(diagnostics_mu);
        for (auto& diag : regression_validation_diags) {
            diagnostics.push_back(std::move(diag));
        }
    }

    // Print all collected validation diagnostics first (config + semantic +
    // post-run + regression-rule). Errors here force a non-zero exit code
    // regardless of whether any rules failed.
    const bool diagnostics_had_errors = report_diagnostics(
        std::span<const ValidationDiag>{diagnostics}, "configuration");

    // Determine the index for this run. Reads existing report_<N>.json
    // files; writes report_<max + 1>.json. First run on a clean dir → 1.
    const std::filesystem::path report_dir_path{report_dir};
    const std::size_t current_index =
        max_existing_report_index(report_dir_path).value_or(0) + 1;
    logi("[bench_framework] writing run as report_{}.json", current_index);

    // Single shared parser-cache for every consumer that wants to walk
    // historical reports (regressions + sanity warnings). Each
    // `report_<N>.json` is parsed at most once per run.
    HistoricalReportCache history_cache{report_dir_path};

    // Evaluate regression rules against report_<current_index - 1>.json
    // (and walk further back as needed for `regression_since`). The
    // evaluator never writes anything; we collect entries and flush
    // them to the report below alongside any diagnostics it produced.
    std::vector<ValidationDiag> regression_eval_diagnostics;
    auto regressions = evaluate_regressions(
        regression_rules_by_config, reports,
        current_index, history_cache, regression_eval_diagnostics);
    const bool regression_eval_had_errors = report_diagnostics(
        std::span<const ValidationDiag>{regression_eval_diagnostics},
        "regression");

    // Sanity warnings: hardcoded checks against the just-finished run's
    // sample arrays plus history. Non-blocking (do not affect the exit
    // code), but always surface to stderr + the report so a human
    // notices broken instrumentation.
    auto sanity_warnings = evaluate_sanity_warnings(
        reports, run_meta, current_index, history_cache);

    logi("[bench_framework] all benchmarks finished, writing report...");
    const auto output_path = report_path_for_index(report_dir_path, current_index);
    const auto report_path = write_report(output_path, run_meta, reports,
                                           violations, regressions, sanity_warnings);
    if (!report_path.empty()) {
        logi("[bench_framework] wrote {}", report_path);
    }

    // Sanity-check our own report against the output schema. Failures here
    // mean a bug in the writer, not user error — but we still surface them.
    bool report_schema_failed = false;
    if (!report_path.empty()) {
        const auto report_text = slurp_file(std::filesystem::path{report_path});
        if (!report_text.empty()) {
            const auto report_check_diagnostics =
                validate_against_schema(report_text, kBenchReportSchema, report_path);
            report_schema_failed = report_diagnostics(
                std::span<const ValidationDiag>{report_check_diagnostics}, "report");
        }
    }

    print_violations(violations);
    print_regressions(regressions);
    print_sanity_warnings(sanity_warnings);

    // Sanity warnings are non-blocking: their presence does NOT
    // contribute to the exit code (the user spec calls them out as
    // "not blocking, however they should be printed to stderr at the
    // end"). The summary above is the operator-visible surface.
    return (violations.empty()
            && regressions.empty()
            && !diagnostics_had_errors
            && !regression_eval_had_errors
            && !report_schema_failed) ? 0 : 1;
}

}  // namespace detail

// ----------------- create_benchmark template body -----------------

template<typename Input, typename Output>
[[nodiscard]] ErasedBenchmark create_benchmark(
    std::string title,
    std::string benchmark_key,
    std::string config_path,
    std::function<RunConstraints(std::string_view, std::string_view, simdjson::dom::element)> on_estimate_resources,
    std::function<Input(std::string_view, std::string_view, simdjson::dom::element, const RunConstraints&)> on_input,
    std::function<Output(std::string_view, const ParamMap&, const Input&)> on_run,
    std::function<std::map<std::string, double>(std::string_view, const ParamMap&, const Input&, const Output&)> on_evaluate,
    std::function<void(std::string_view, std::string_view, simdjson::dom::element, const Input&)> on_cleanup)
{
    ErasedBenchmark erased;
    erased.title         = std::move(title);
    erased.benchmark_key = std::move(benchmark_key);
    erased.config_path   = std::move(config_path);

    erased.execute = [erased_title              = erased.title,
                       erased_benchmark_key      = erased.benchmark_key,
                       erased_config_path        = erased.config_path,
                       captured_on_estimate      = std::move(on_estimate_resources),
                       captured_on_input         = std::move(on_input),
                       captured_on_run           = std::move(on_run),
                       captured_on_evaluate      = std::move(on_evaluate),
                       captured_on_cleanup       = std::move(on_cleanup)
                      ](BenchmarkContext& context) {
        auto state_holder = std::make_unique<detail::BenchmarkExecutionState>();
        auto& state = *state_holder;
        state.title = erased_title;

        // Always publish a `setup_done` signal to the renderer on the
        // way out, even from early-return paths. Otherwise an aborted
        // benchmark would leave the dashboard's per-benchmark slot
        // stuck on "header only, no rows" until `progress.running`
        // flips false. The renderer treats `setup_done == true` plus
        // an empty `test_cases` as "this benchmark is silent" and
        // simply omits the inner walk.
        struct PublishOnExit {
            detail::ProgressState::Per* slot = nullptr;
            ~PublishOnExit() noexcept {
                if (slot != nullptr) {
                    slot->setup_done.store(true, std::memory_order_release);
                }
            }
        };
        PublishOnExit publish_on_exit{
            (context.progress != nullptr
             && context.benchmark_index < context.progress->per_benchmark.size())
                ? context.progress->per_benchmark[context.benchmark_index].get()
                : nullptr};

        // 1. Parse the config file. The outcome carries schema diagnostics
        //    plus structural problems found during the simdjson walk.
        auto parse_outcome = detail::parse_config_file(
            erased_config_path, erased_benchmark_key, state);
        const bool parse_failed = !parse_outcome.ok;
        detail::drain_diagnostics_to_context(context, parse_outcome.diagnostics);
        if (parse_failed) {
            loge("[bench_framework] benchmark '{}' aborted (parse / schema failure)",
                 erased_benchmark_key);
            return;
        }

        // 1a. Register regression rules in the global registry. The first
        //     benchmark to load a given config_path inserts; subsequent
        //     benchmarks sharing the file see the entry and discard their
        //     parsed copy.
        if (context.regression_rules_mu != nullptr
            && context.regression_rules_by_config != nullptr) {
            std::lock_guard<std::mutex> guard(*context.regression_rules_mu);
            auto& registry = *context.regression_rules_by_config;
            if (registry.find(erased_config_path) == registry.end()) {
                registry.emplace(erased_config_path,
                                 std::move(parse_outcome.regression_rules));
            }
        }

        // 2. Semantic validation: cross-references, type-correctness in
        //    rules, duplicate rule names, always-pass warnings, etc.
        auto semantic_diagnostics = detail::semantic_validate_config(state);
        const bool semantic_failed = std::any_of(
            semantic_diagnostics.begin(), semantic_diagnostics.end(),
            [](const auto& diag) {
                return diag.severity == detail::ValidationDiag::Severity::Error;
            });
        detail::drain_diagnostics_to_context(context, semantic_diagnostics);
        if (semantic_failed) {
            loge("[bench_framework] benchmark '{}' aborted (semantic validation failure)",
                 erased_benchmark_key);
            return;
        }

        // 3. --fast filter: skip if the user requested fast mode but this
        //    benchmark is not flagged fast. Without --fast we run everything.
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

        // V11: per-test-case mutexes deleted — the new batched scheduler
        // runs at most one tuple at a time per (test_case, group), so
        // there is no Input contention left to protect against.

        // Populate the progress dashboard's structure for this benchmark.
        // One ProgressTestCase per test_case in declared order, one
        // ProgressGroup per group of that test_case.
        if (context.progress != nullptr
            && context.benchmark_index < context.progress->per_benchmark.size()) {
            auto& per = *context.progress->per_benchmark[context.benchmark_index];
            per.test_cases.clear();
            per.test_cases.reserve(state.test_cases.size());
            for (std::size_t tc_index = 0; tc_index < state.test_cases.size(); ++tc_index) {
                auto tc = std::make_unique<detail::ProgressTestCase>();
                tc->test_case_name = state.test_cases[tc_index].name;
                // Walk every cell that targets this test_case to gather
                // group count + per-group iteration totals + total tuple
                // count.
                std::size_t max_group_count = 0;
                std::map<std::size_t, std::uint64_t> per_group_iters;
                std::uint64_t tuple_total = 0;
                for (const auto& cell_ptr : state.cells) {
                    if (cell_ptr->test_case_index != tc_index) continue;
                    max_group_count = std::max(max_group_count, cell_ptr->group_count);
                    per_group_iters[cell_ptr->group_index] +=
                        static_cast<std::uint64_t>(cell_ptr->iterations);
                    ++tuple_total;
                }
                tc->tuple_total.store(tuple_total, std::memory_order_relaxed);
                if (tuple_total == 0) {
                    // Test case has no batches at all (zero iterations,
                    // or undeclared in matrix.for). Stamp finish_ns
                    // immediately so the renderer treats the row as
                    // collapsed instead of showing "Running... 0s".
                    tc->finish_ns.store(bioseq::instr::clock::now_ns(),
                                        std::memory_order_relaxed);
                }
                tc->groups.reserve(max_group_count);
                for (std::size_t group_index = 0; group_index < max_group_count;
                     ++group_index) {
                    auto group_ptr = std::make_unique<detail::ProgressGroup>();
                    group_ptr->group_index = group_index;
                    group_ptr->total = per_group_iters[group_index];
                    tc->groups.push_back(std::move(group_ptr));
                }
                per.test_cases.push_back(std::move(tc));
            }
            // V13: publish setup_done IMMEDIATELY after the writes so the
            // `progress_*` helpers (and the renderer) start observing
            // tuple/group counters as `run_typed_benchmark` progresses.
            // PublishOnExit still fires on every closure exit and is
            // idempotent — its second store is a no-op. (V12 left
            // setup_done false until the closure returned, so every
            // progress_tick_* call returned early via the
            // `progress_test_case_for` setup_done check, leaving every
            // gauge frozen at 0/N for the entire run.)
            per.setup_done.store(true, std::memory_order_release);
        }

        // V16: bench-lock + serial-concurrency exclusion are enforced
        // by the BatchScheduler's admission predicates. The closure
        // body just hands off to `run_typed_benchmark`, which enqueues
        // batches into the scheduler and blocks via
        // `wait_for_benchmark` before returning.

        detail::run_typed_benchmark<Input, Output>(state, context,
                                                    captured_on_estimate,
                                                    captured_on_input,
                                                    captured_on_run,
                                                    captured_on_evaluate,
                                                    captured_on_cleanup);

        // 4. Post-run validation: missing-evaluation-metric check.
        auto post_run_diagnostics = detail::semantic_validate_post_run(state);
        detail::drain_diagnostics_to_context(context, post_run_diagnostics);

        // 5. Evaluate rules and stash this benchmark's report data.
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
