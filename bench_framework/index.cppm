/**
 * @file bench_framework/index.cppm
 * @brief Generic, algorithm-agnostic benchmark orchestration framework.
 *
 * Public surface lives in `bioseq::bench::framework`. Users describe a
 * benchmark with `create_benchmark<Input, Output>(...)` (three lambdas:
 * input builder, algorithm runner, output evaluator). They then pass any
 * number of those benchmark handles to `run(...)`. The framework parses
 * the JSON config, expands the parameter matrix, drives the iteration
 * loop in parallel via TBB while showing an ftxui progress dashboard,
 * collects three flavours of metric (built-in `wall_clock_ns`, named
 * counter snapshots, evaluation-handler outputs), evaluates rules from
 * the config, and writes a single `report.json`.
 *
 * Configuration, scheduling, measurement, and report production are owned by
 * this module; interactive rendering is supplied by its implementation unit.
 */
module;

#include <libassert/assert.hpp>
#include <simdjson.h>

#include <nlohmann/json.hpp>
#include <nlohmann/json-schema.hpp>

#include <oneapi/tbb/global_control.h>
#include <oneapi/tbb/task_arena.h>
#include <oneapi/tbb/task_group.h>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include <cpptrace/cpptrace.hpp>
#include <cpptrace/from_current.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <list>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <sys/stat.h>
#include <thread>
#include <tuple>
#include <type_traits>
#include <ctime>
#include <unistd.h>
#include <utility>
#include <vector>

export module bioseq.bench.framework;

import bioseq.common.logging;
import bioseq.common.memory_budget;
import bioseq.instrumentation;

// ---------------------------------------------------------------------------
//  Public surface
// ---------------------------------------------------------------------------

export namespace bioseq::bench::framework {

/// Source-owned benchmark wire revision, also read by the Python consumer.
inline constexpr std::uint64_t e2e_raw_payload_schema_version = 12;

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
struct config_value {
    /// Recognised scalar kinds plus a sentinel for arrays, objects, and
    /// nulls. The framework refuses to treat non-scalar values as
    /// matching any declared parameter type, so semantic validation surfaces
    /// `value_type_mismatch` for nested structures used where a scalar was
    /// expected. `string_value` carries the source JSON spelling for
    /// nested data so error messages can show what the user wrote.
    enum class value_kind : std::uint8_t {
        string,
        integer,
        floating_point,
        boolean,
        non_scalar,
    };

    value_kind   kind = value_kind::string;
    std::string  string_value{};
    std::int64_t integer_value = 0;
    double       double_value = 0.0;
    bool         boolean_value = false;

    [[nodiscard]] constexpr bool is_string() const noexcept {
        return kind == value_kind::string;
    }
    [[nodiscard]] constexpr bool is_integer() const noexcept {
        return kind == value_kind::integer;
    }
    [[nodiscard]] constexpr bool is_double() const noexcept {
        return kind == value_kind::floating_point;
    }
    [[nodiscard]] constexpr bool is_boolean() const noexcept {
        return kind == value_kind::boolean;
    }
    [[nodiscard]] constexpr bool is_non_scalar() const noexcept {
        return kind == value_kind::non_scalar;
    }

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

    [[nodiscard]] bool operator==(const config_value& other) const noexcept {
        if (kind != other.kind) return false;
        switch (kind) {
            case value_kind::string: return string_value == other.string_value;
            case value_kind::integer: return integer_value == other.integer_value;
            case value_kind::floating_point: return double_value == other.double_value;
            case value_kind::boolean: return boolean_value == other.boolean_value;
            case value_kind::non_scalar: return string_value == other.string_value;
        }
        return false;
    }

    /// Templated factory replacing four near-identical `from_T` helpers.
    template<typename p_value_type>
    [[nodiscard]] static config_value from(p_value_type raw_value) noexcept {
        config_value out{};
        if constexpr (std::is_same_v<p_value_type, std::string>
                      || std::is_same_v<p_value_type, std::string_view>
                      || std::is_same_v<p_value_type, const char*>) {
            out.kind = config_value::value_kind::string;
            out.string_value = std::string{raw_value};
        } else if constexpr (std::is_same_v<p_value_type, bool>) {
            out.kind = config_value::value_kind::boolean;
            out.boolean_value = raw_value;
        } else if constexpr (std::is_integral_v<p_value_type>) {
            out.kind = config_value::value_kind::integer;
            out.integer_value = static_cast<std::int64_t>(raw_value);
        } else if constexpr (std::is_floating_point_v<p_value_type>) {
            out.kind = config_value::value_kind::floating_point;
            out.double_value = static_cast<double>(raw_value);
        } else {
            static_assert(sizeof(p_value_type) == 0, "unsupported config_value source type");
        }
        return out;
    }

    /// Parse a simdjson element. Arrays and objects are not first-class
    /// configuration values; callers that want to accept them must walk
    /// the simdjson element directly. For exotic input we fall back to a
    /// stringified form so the value is preserved end-to-end.
    [[nodiscard]] static config_value from_simdjson(
        simdjson::dom::element element) noexcept;
};

/// Per-algorithm parameter map: param_name -> value (with defaults filled in).
using parameter_map = std::map<std::string, config_value>;

/// Framework-resolved execution tuple exposed read-only to `on_input` through
/// run_constraints after matrix expansion and `cases=` pruning.
struct resolved_execution_tuple {
    std::string algorithm_name{};
    parameter_map params{};
};

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
struct run_constraints {
    /// Bytes that must remain reserved between `on_input` and
    /// `on_cleanup` (i.e. for the lifetime of the cached `Input`).
    /// Default 0 = no reservation.
    std::size_t base_memory  = 0;
    /// Bytes additionally reserved while the iteration loop is in
    /// flight on this batch. Released as soon as the loop exits.
    /// Default 0 = no reservation.
    std::size_t extra_memory = 0;
    /// Framework-owned, stable-order execution tuples that will actually run
    /// against this batch's cached Input. The framework fills this after
    /// matrix expansion and `cases=` pruning, replacing any estimator-provided
    /// contents before `on_input` observes the value. Input builders may use
    /// it to avoid preparing algorithm/policy-specific untimed fixtures for
    /// rows that cannot run in this batch.
    std::vector<resolved_execution_tuple> execution_tuples{};
};

/**
 * @brief Type-erased benchmark handle returned by `create_benchmark`.
 *
 * Public so the variadic `run(...)` parameter pack can be deduced. Users
 * never construct this directly.
 */
struct erased_benchmark {
    std::string title;
    std::string benchmark_key;
    /// Stable report/provenance identity for the configuration.
    std::string config_path;
    /// File-open path; equal to config_path unless a validated launcher
    /// supplies a separate absolute read path.
    std::string config_open_path;
    bool raw_samples_enabled = false;
    /// Owns the typed lambdas + iteration loop. The first call prepares and
    /// enqueues this benchmark without blocking a oneTBB worker; the returned
    /// closure joins its batches and publishes the completed report state.
    std::function<std::function<void()>(struct benchmark_context&)> execute;
};

/// Test-only process-local fault seam for the durable raw-evidence syscalls.
/// Production callers leave this at `none`, so inherited process state cannot
/// enable it. Tests must reset the seam after each injected run.
enum class raw_evidence_io_fault : std::uint8_t {
    none,
    short_write,
    partial_write_failure,
    sync_failure,
};

void set_raw_evidence_io_fault_for_testing(
    raw_evidence_io_fault fault) noexcept;

/// Test-only corruption seam for proving paired schedule evidence fails
/// closed. It changes only the retained evidence coordinates after the real
/// scheduler has selected the correct execution order.
enum class paired_schedule_evidence_fault : std::uint8_t {
    none,
    wrong_shuffle_seed,
    wrong_first_tuple_order_position,
};

void set_paired_schedule_evidence_fault_for_testing(
    paired_schedule_evidence_fault fault) noexcept;

namespace detail {

[[nodiscard]] int run_impl(int argc, char** argv,
                            std::string report_dir,
                            std::string data_dir,
                            std::vector<erased_benchmark> benchmarks);

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
 * `on_estimate_resources` MUST NOT throw because it also runs on the driver
 * during enqueue. Worker-side callback failures are caught at the scheduler
 * boundary: independent batches drain, the original exception is rethrown at
 * the explicit join, and `run()` writes completed samples as a failed partial
 * report instead of allowing `~batch_scheduler() noexcept` to terminate.
 *
 * @param title             Human-readable label echoed in the report's `title` field.
 * @param benchmark_key     Unique key used (a) to look up the matching block in
 *                          `config_path`, and (b) as the report's
 *                          `benchmarks.<benchmark_key>` entry. One config file may
 *                          hold multiple benchmark blocks; the key disambiguates
 *                          which block this `create_benchmark` call describes.
 * @param config_path       Stable identity reported for the bench configuration
 *                          JSON (validated against a built-in JSON schema before
 *                          use).
 * @param on_estimate_resources  Returns a `run_constraints` describing the memory
 *                          this batch will need. Called at most once per
 *                          `(test_case, group)` batch, before `on_input`. The
 *                          framework reserves `base_memory` from a shared 5 GiB
 *                          pool before calling `on_input` and `extra_memory` for
 *                          the iteration loop only. Called exactly once per
 *                          batch; RNG inside is harmless because the
 *                          framework only invokes the lambda once per
 *                          `(test_case, group)` pair. Must not throw.
 * @param on_input          Builds the test-case `Input` from the parsed JSON data
 *                          and the framework-determined `run_constraints`. Called
 *                          exactly once per `(test_case, group)` batch — when a
 *                          test_case is split into N parallel groups, the framework
 *                          calls `on_input` once per group, each with its own
 *                          `Input` instance. Must not throw.
 * @param on_run            Runs the algorithm. Wraps the timing window for the
 *                          built-in `wall_clock_ns` metric. Called sequentially
 *                          inside one batch, concurrently across batches. An
 *                          escaped exception aborts the current batch and is
 *                          owned by the scheduler until the explicit join;
 *                          it never first emerges from scheduler destruction.
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
 * @param config_open_path  Optional path used only to open/read configuration
 *                          bytes. Empty preserves the historical behavior of
 *                          reading config_path directly; config_path remains
 *                          the report/provenance identity in either case.
 */
template<typename p_output>
struct repeat_output_reference {
    std::string_view algorithm_name{};
    const parameter_map* params = nullptr;
    p_output* output = nullptr;
};

/**
 * @brief Opt-in logical-repeat finalization for paired benchmark evidence.
 *
 * The empty default preserves the historical immediate-commit path.  When
 * `enabled(input)` is true, an interleaved repeat is measured completely,
 * `finalize_repeat` receives every mutable output before any evaluation/raw
 * append, and `published_wall_clock_ns` may select a previously measured
 * algorithm-only duration inside the framework envelope.  The framework
 * retains the unmodified envelope in `outer_e2e_ns` and rejects an override
 * which exceeds it.
 */
template<typename p_input, typename p_output>
struct repeat_finalization_callbacks {
    std::function<bool(const p_input&)> enabled{};
    std::function<std::uint64_t(
        std::string_view algorithm_name,
        const parameter_map& params,
        const p_input& input,
        const p_output& output)> published_wall_clock_ns{};
    std::function<void(
        const p_input& input,
        std::size_t repeat,
        std::span<repeat_output_reference<p_output>> outputs)>
        finalize_repeat{};
};

/// Exact framework-owned clock envelope for one measured iteration body.
struct iteration_measurement_window {
    std::int64_t  begin_ns = 0;
    std::int64_t  end_ns = 0;
    std::uint64_t elapsed_ns = 0;
};

/**
 * @brief Opt-in per-iteration preparation and post-measurement mutation.
 *
 * `enabled` is evaluated outside the measurement window.  When it returns
 * true, `before_iteration` constructs iteration-local preparation on the
 * worker stack, `measured_iteration` alone occupies the framework clock and
 * counter/profiler envelope, and `after_iteration` runs after that envelope
 * but before evaluation or raw-sample publication.  A false result preserves
 * the benchmark's ordinary `on_run` path.
 */
template<typename p_input, typename p_preparation, typename p_output>
struct iteration_boundary_callbacks {
    using preparation_type = p_preparation;

    std::function<bool(
        std::string_view algorithm_name,
        const parameter_map& params,
        const p_input& input)> enabled{};
    std::function<p_preparation(
        std::string_view algorithm_name,
        const parameter_map& params,
        const p_input& input,
        std::size_t repeat)> before_iteration{};
    std::function<p_output(
        std::string_view algorithm_name,
        const parameter_map& params,
        const p_input& input,
        p_preparation& preparation)> measured_iteration{};
    std::function<void(
        std::string_view algorithm_name,
        const parameter_map& params,
        const p_input& input,
        p_preparation& preparation,
        p_output& output,
        const iteration_measurement_window& window)> after_iteration{};
};

template<typename p_input, typename p_output>
[[nodiscard]] erased_benchmark create_benchmark(
    std::string title,
    std::string benchmark_key,
    std::string config_path,
    std::function<run_constraints(std::string_view data_dir,
                                  std::string_view test_case_name,
                                  simdjson::dom::element test_case_data)> on_estimate_resources,
    std::function<p_input(std::string_view data_dir,
                        std::string_view test_case_name,
                        simdjson::dom::element test_case_data,
                        const run_constraints& constraints)> on_input,
    std::function<p_output(std::string_view algorithm_name,
                         const parameter_map& params,
                         const p_input& input)> on_run,
    std::function<std::map<std::string, double>(std::string_view algorithm_name,
                                                const parameter_map& params,
                                                const p_input& input,
                                                const p_output& output)> on_evaluate,
    std::function<void(std::string_view data_dir,
                       std::string_view test_case_name,
                       simdjson::dom::element test_case_data,
                       const p_input& input)> on_cleanup = {},
    std::function<std::string(std::string_view algorithm_name,
                              const parameter_map& params,
                              const p_input& input,
                              const p_output& output)>
        serialize_raw_sample_payload = {},
    std::string config_open_path = {},
    repeat_finalization_callbacks<p_input, p_output>
        repeat_finalization = {});

template<typename p_input, typename p_preparation, typename p_output>
[[nodiscard]] erased_benchmark create_benchmark_with_iteration_boundaries(
    std::string title,
    std::string benchmark_key,
    std::string config_path,
    std::function<run_constraints(std::string_view data_dir,
                                  std::string_view test_case_name,
                                  simdjson::dom::element test_case_data)> on_estimate_resources,
    std::function<p_input(std::string_view data_dir,
                        std::string_view test_case_name,
                        simdjson::dom::element test_case_data,
                        const run_constraints& constraints)> on_input,
    std::function<p_output(std::string_view algorithm_name,
                         const parameter_map& params,
                         const p_input& input)> on_run,
    std::function<std::map<std::string, double>(std::string_view algorithm_name,
                                                const parameter_map& params,
                                                const p_input& input,
                                                const p_output& output)> on_evaluate,
    iteration_boundary_callbacks<p_input, p_preparation, p_output>
        iteration_boundaries,
    std::function<void(std::string_view data_dir,
                       std::string_view test_case_name,
                       simdjson::dom::element test_case_data,
                       const p_input& input)> on_cleanup = {},
    std::function<std::string(std::string_view algorithm_name,
                              const parameter_map& params,
                              const p_input& input,
                              const p_output& output)>
        serialize_raw_sample_payload = {},
    std::string config_open_path = {},
    repeat_finalization_callbacks<p_input, p_output>
        repeat_finalization = {});

/**
 * @brief Run all benchmarks. Returns 0 on success, 1 if any rule violated
 *        or any config failed to parse / validate.
 *
 * Variadic for syntax fidelity with the user's `main.cpp` example;
 * collects the pack into `vector<erased_benchmark>` and delegates.
 */
template<typename... p_benchmark_types>
[[nodiscard]] int run(int argc, char** argv,
                     std::string report_dir,
                     std::string data_dir,
                     p_benchmark_types&&... benchmarks)
{
    std::vector<erased_benchmark> erased_list;
    erased_list.reserve(sizeof...(p_benchmark_types));
    (erased_list.push_back(
         std::forward<p_benchmark_types>(benchmarks)), ...);
    return detail::run_impl(argc, argv,
                            std::move(report_dir), std::move(data_dir),
                            std::move(erased_list));
}

}  // namespace bioseq::bench::framework

// ---------------------------------------------------------------------------
//  Internals
// ---------------------------------------------------------------------------

namespace bioseq::bench::framework {

namespace detail {
inline std::atomic<raw_evidence_io_fault>
    raw_evidence_io_fault_for_testing{
        raw_evidence_io_fault::none};
inline std::atomic<paired_schedule_evidence_fault>
    paired_schedule_evidence_fault_for_testing{
        paired_schedule_evidence_fault::none};
}  // namespace detail

void set_raw_evidence_io_fault_for_testing(
        const raw_evidence_io_fault fault) noexcept {
    detail::raw_evidence_io_fault_for_testing.store(
        fault, std::memory_order_release);
}

void set_paired_schedule_evidence_fault_for_testing(
        const paired_schedule_evidence_fault fault) noexcept {
    detail::paired_schedule_evidence_fault_for_testing.store(
        fault, std::memory_order_release);
}

/// Canonical compile-time profiler args used by the framework. Timing-only
/// is enough for `wall_clock_ns`; the framework does not need PMU counters
/// or sampling.
using profiler_configuration =
    bioseq::instrumentation::benchmark_profiling_configuration;

namespace detail {

// ----------------- shared types -----------------

enum class comparison_operator : std::uint8_t {
    less_than,
    greater_than,
    equal,
};

/// Aggregation function applied to a sample vector when reducing it to a
/// single value before a comparator runs. Used by both per-rule
/// comparators (the LHS / RHS aggregates of a `benchmark_rule`) and regression
/// rules (the `aggregate` field on a `regression_rule`). Matches the six
/// values allowed by the JSON enum: sum / min / max / p01 / p50 / p99.
enum class sample_aggregate : std::uint8_t { sum, min, max, p01, p50, p99 };

/// One row of the aggregate registry. `percentile_fraction` is meaningful
/// only for the three percentile entries; the other three carry NaN to
/// signal "not a percentile". Adding a new percentile just means adding a
/// row — `aggregate_samples` reads the fraction from here.
struct sample_aggregate_entry {
    std::string_view    name;
    sample_aggregate           kind;
    double              percentile_fraction;     // NaN for non-percentile aggregates
};

/// Compile-time table linking the JSON spelling to the enumerator. Single
/// source of truth for parsers, validators, the `aggregate_samples`
/// dispatch, and the report writer; mirrors the
/// `k_builtin_metric_registry` pattern.
inline constexpr std::array<sample_aggregate_entry, 6> k_aggregate_registry{ {
    { "sum", sample_aggregate::sum, std::numeric_limits<double>::quiet_NaN() },
    { "min", sample_aggregate::min, std::numeric_limits<double>::quiet_NaN() },
    { "max", sample_aggregate::max, std::numeric_limits<double>::quiet_NaN() },
    { "p01", sample_aggregate::p01, 0.01 },
    { "p50", sample_aggregate::p50, 0.50 },
    { "p99", sample_aggregate::p99, 0.99 },
} };

[[nodiscard]] inline constexpr std::optional<sample_aggregate>
parse_aggregate(std::string_view text) noexcept {
    for (const auto& entry : k_aggregate_registry) {
        if (entry.name == text) return entry.kind;
    }
    return std::nullopt;
}

[[nodiscard]] inline constexpr std::string_view
aggregate_name(sample_aggregate aggregate) noexcept {
    for (const auto& entry : k_aggregate_registry) {
        if (entry.kind == aggregate) return entry.name;
    }
    return "?";
}

/// Returns the percentile fraction (0.01, 0.50, 0.99) for percentile
/// aggregates, or NaN for the additive Sum / Min / Max kinds. The NaN
/// sentinel keeps `aggregate_samples`' percentile branch table-driven.
[[nodiscard]] inline constexpr double
aggregate_percentile(sample_aggregate aggregate) noexcept {
    for (const auto& entry : k_aggregate_registry) {
        if (entry.kind == aggregate) return entry.percentile_fraction;
    }
    return std::numeric_limits<double>::quiet_NaN();
}

/// Every recognised metric flavour. The first batch are produced by the
/// framework from the iteration-local `profiler_state<Args>` aggregates;
/// Counter and evaluation metrics route through the user's instrumentation
/// counters / evaluation lambda.
enum class metric_kind : std::uint8_t {
    wall_clock_ns,
    region_total_ns,
    region_visits,
    region_min_ns,
    region_max_ns,
    region_p50_ns,
    region_p99_ns,
    region_total_cycles,
    region_total_instructions,
    region_total_branches,
    region_total_branch_misses,
    region_ipc,
    region_branch_miss_rate,
    loop_total_iterations,
    loop_total_work,
    loop_empty_visits,
    loop_min_per_iteration_ns,
    loop_max_per_iteration_ns,
    loop_p50_per_iteration_ns,
    loop_p99_per_iteration_ns,
    loop_mean_per_iteration_ns,
    loop_work_per_iteration,
    counter,
    evaluation,
};

/// Source family (drives schema validation, parsing, and which descriptor
/// table to walk).
enum class metric_source : std::uint8_t {
    outer_clock,
    region,
    loop,
    counter,
    evaluation,
};

/// Cross-descriptor combiner (only meaningful for Region / Loop). Drives
/// the per-iteration "fold every visited descriptor's value into one
/// number" step in `append_iteration_samples`.
enum class metric_combiner : std::uint8_t {
    sum,
    histogram_sum,
    pmu_ratio,
    branch_miss_ratio,
    loop_mean_ns,
    loop_work_per_iteration,
    requires_region,
};

/// Static registry of every framework-built-in metric. Schema validation,
/// metric parsing, semantic checks, and the iteration extraction step all
/// consume this single source of truth.
struct builtin_metric_entry {
    std::string_view name;
    metric_kind       kind;
    metric_source     source;
    metric_combiner   combiner;
    /// True iff the metric reads any PMU-derived field. Used by the
    /// `pmu_unavailable` runtime warning.
    bool             needs_pmu = false;
    /// p-fraction for HistogramSum-combined metrics; ignored otherwise.
    double           percentile = 0.0;
};

inline constexpr std::array<builtin_metric_entry, 22> k_builtin_metric_registry{ {
    { "wall_clock_ns",                metric_kind::wall_clock_ns,             metric_source::outer_clock, metric_combiner::sum, false, 0.0 },
    { "region_total_ns",              metric_kind::region_total_ns,           metric_source::region,     metric_combiner::sum, false, 0.0 },
    { "region_visits",                metric_kind::region_visits,            metric_source::region,     metric_combiner::sum, false, 0.0 },
    { "region_min_ns",                metric_kind::region_min_ns,             metric_source::region,     metric_combiner::requires_region, false, 0.0 },
    { "region_max_ns",                metric_kind::region_max_ns,             metric_source::region,     metric_combiner::requires_region, false, 0.0 },
    { "region_p50_ns",                metric_kind::region_p50_ns,             metric_source::region,     metric_combiner::histogram_sum, false, 0.50 },
    { "region_p99_ns",                metric_kind::region_p99_ns,             metric_source::region,     metric_combiner::histogram_sum, false, 0.99 },
    { "region_total_cycles",          metric_kind::region_total_cycles,       metric_source::region,     metric_combiner::sum, true,  0.0 },
    { "region_total_instructions",    metric_kind::region_total_instructions, metric_source::region,     metric_combiner::sum, true,  0.0 },
    { "region_total_branches",        metric_kind::region_total_branches,     metric_source::region,     metric_combiner::sum, true,  0.0 },
    { "region_total_branch_misses",   metric_kind::region_total_branch_misses, metric_source::region,     metric_combiner::sum, true,  0.0 },
    { "region_ipc",                   metric_kind::region_ipc,               metric_source::region,     metric_combiner::pmu_ratio, true, 0.0 },
    { "region_branch_miss_rate",      metric_kind::region_branch_miss_rate,    metric_source::region,     metric_combiner::branch_miss_ratio, true, 0.0 },
    { "loop_total_iterations",        metric_kind::loop_total_iterations,     metric_source::loop,       metric_combiner::sum, false, 0.0 },
    { "loop_total_work",              metric_kind::loop_total_work,           metric_source::loop,       metric_combiner::sum, false, 0.0 },
    { "loop_empty_visits",            metric_kind::loop_empty_visits,         metric_source::loop,       metric_combiner::sum, false, 0.0 },
    { "loop_min_per_iter_ns",         metric_kind::loop_min_per_iteration_ns,        metric_source::loop,       metric_combiner::requires_region, false, 0.0 },
    { "loop_max_per_iter_ns",         metric_kind::loop_max_per_iteration_ns,        metric_source::loop,       metric_combiner::requires_region, false, 0.0 },
    { "loop_p50_per_iter_ns",         metric_kind::loop_p50_per_iteration_ns,        metric_source::loop,       metric_combiner::histogram_sum, false, 0.50 },
    { "loop_p99_per_iter_ns",         metric_kind::loop_p99_per_iteration_ns,        metric_source::loop,       metric_combiner::histogram_sum, false, 0.99 },
    { "loop_mean_per_iter_ns",        metric_kind::loop_mean_per_iteration_ns,       metric_source::loop,       metric_combiner::loop_mean_ns, false, 0.0 },
    { "loop_work_per_iter",           metric_kind::loop_work_per_iteration,         metric_source::loop,       metric_combiner::loop_work_per_iteration, false, 0.0 },
} };

/// Look up a builtin metric by canonical name. Returns nullptr if absent.
[[nodiscard]] inline constexpr const builtin_metric_entry* lookup_builtin_metric(
    std::string_view metric_name) noexcept
{
    for (const auto& entry : k_builtin_metric_registry) {
        if (entry.name == metric_name) return &entry;
    }
    return nullptr;
}

[[nodiscard]] inline constexpr const char* metric_kind_name(metric_kind kind) noexcept {
    switch (kind) {
        case metric_kind::counter:    return "counter";
        case metric_kind::evaluation: return "evaluation";
        default: break;
    }
    for (const auto& entry : k_builtin_metric_registry) {
        if (entry.kind == kind) return entry.name.data();
    }
    return "?";
}

/// Parse the JSON `"type"` string into a source family. Returns
/// `std::nullopt` for unrecognised strings; the caller distinguishes
/// "counter" / "evaluation" from registry-driven sources by inspecting
/// the source enumerator.
[[nodiscard]] inline std::optional<metric_source> parse_metric_type(std::string_view type_text) noexcept {
    if (type_text == "builtin")    return metric_source::outer_clock;
    if (type_text == "region")     return metric_source::region;
    if (type_text == "loop")       return metric_source::loop;
    if (type_text == "counter")    return metric_source::counter;
    if (type_text == "evaluation") return metric_source::evaluation;
    return std::nullopt;
}

/// Inverse of `parse_metric_type`. Renders a `metric_source` as its
/// JSON-side spelling (`"region"`, `"loop"`, `"builtin"`, `"counter"`,
/// `"evaluation"`). Used by diagnostic messages — the strings round-trip
/// with `parse_metric_type`.
[[nodiscard]] inline constexpr std::string_view metric_source_name(metric_source source) noexcept {
    switch (source) {
        case metric_source::outer_clock: return "builtin";
        case metric_source::region:     return "region";
        case metric_source::loop:       return "loop";
        case metric_source::counter:    return "counter";
        case metric_source::evaluation: return "evaluation";
    }
    return "?";
}

[[nodiscard]] inline std::optional<comparison_operator> parse_compare_op(std::string_view op_text) noexcept {
    if (op_text == "<") return comparison_operator::less_than;
    if (op_text == ">") return comparison_operator::greater_than;
    if (op_text == "=") return comparison_operator::equal;
    return std::nullopt;
}

/// Match expression — picks a subset of execution tuples by algorithm key,
/// partial parameter assignment, and optional source coordinates. Empty
/// `match` means "any tuple for this algorithm"; empty `test_case` and
/// `region` mean every configured test case and `all_regions`, respectively.
/// Keeping the source coordinates on each side lets one within-run rule
/// compare two differently named instrumentation regions without deriving or
/// replacing either raw metric in the report.
struct rule_selector {
    std::string algorithm_key;
    parameter_map    match{};
    std::string test_case{};
    std::string region{};
};

struct rule_comparator {
    enum class pairing_mode : std::uint8_t {
        independent_aggregates,
        by_iteration_ratio,
        by_iteration_exact,
    };

    comparison_operator op = comparison_operator::equal;
    double    threshold_pct = 0.0;
    double    tolerance_pct = 0.0;
    /// Reduction applied to each side's sample vector before the
    /// comparator runs. Defaults to the framework's p50 percentile.
    sample_aggregate aggregate = sample_aggregate::p50;
    /// Independent pairing reduces LHS and RHS separately.
    /// Ratio-paired rules build one LHS/RHS ratio for each logical repeat and
    /// reduce that vector. Exact-paired rules compare the finite IEEE-754 value
    /// of every aligned repeat without reducing either source vector.
    pairing_mode pairing = pairing_mode::independent_aggregates;
    /// Reference ratio used as the ratio-paired comparator's RHS. Keeping this
    /// separate from `tolerance_pct` makes a rule state "report_76 ratio R
    /// with N% relative noise" directly, including baselines greater than 1.
    double paired_baseline = 1.0;
    bool paired_baseline_explicit = false;
    /// Diagnostic rules still emit findings/evidence but do not fail the
    /// process. Blocking is the default.
    bool blocking = true;
    /// Number of leading logical repeats excluded from a ratio-paired rule. The
    /// raw samples remain intact, so repeat zero can be reported separately
    /// as first-repeat evidence while steady-state comparison starts at one.
    std::size_t warmup_repetitions = 0;
};

[[nodiscard]] inline constexpr std::string_view pairing_mode_report_name(
    rule_comparator::pairing_mode mode) noexcept
{
    switch (mode) {
        case rule_comparator::pairing_mode::independent_aggregates:
            return "independent_aggregates";
        case rule_comparator::pairing_mode::by_iteration_ratio:
            return "paired_iteration_ratio";
        case rule_comparator::pairing_mode::by_iteration_exact:
            return "paired_iteration_exact";
    }
    return "independent_aggregates";
}

struct benchmark_rule {
    std::string     name;
    std::string     metric_name;
    metric_kind      kind = metric_kind::wall_clock_ns;
    rule_selector    lhs;
    rule_comparator  cmp;
    rule_selector    rhs;
};

struct metric_specification {
    std::string  name;
    metric_kind   kind = metric_kind::wall_clock_ns;
    /// `"type"` string the user declared (or the implicit "builtin"
    /// default). Stored separately from `kind` so semantic validation can
    /// detect cross-source typos like `"type": "loop"` with key
    /// `region_total_ns` — the registry resolves the kind from the name
    /// regardless of `type`, but a mismatch is still a config error.
    metric_source declared_source = metric_source::outer_clock;
    /// For `kind == Counter`: canonical name of the instrumentation counter
    /// to snapshot. Defaults to `name` when the config omits the explicit
    /// `"counter"` field.
    std::string  counter_name{};
    /// For `metric_source::region` / `metric_source::loop` metrics: when
    /// non-empty, restrict the metric to descriptors whose name equals
    /// this string. When empty, every visited descriptor contributes via
    /// the registry's combiner (Sum, HistogramSum, PmuRatio, etc.).
    /// Mandatory for metrics whose combiner is `RequiresRegion`
    /// (region_min_ns, region_max_ns, loop_min_per_iter_ns,
    /// loop_max_per_iter_ns) — semantic_validate_config rejects those if
    /// `region` is empty.
    std::string  region{};
    /// Optional exact execution-row applicability for evaluation metrics.
    /// Empty preserves the legacy report-global emission check. A non-empty
    /// list opts into fail-closed per-accepted-iteration validation for the
    /// named algorithm rows without changing ordinary benchmark semantics.
    std::vector<std::string> applicable_algorithms{};
    std::vector<benchmark_rule> rules{};
};

struct concurrency_options {
    /// If true, this benchmark runs alone (every item-iteration loop
    /// acquires the cross-bench unique lock; siblings queue behind it).
    bool bench_lock  = false;
    /// If true (default), batches of this benchmark fan out across TBB
    /// workers. False forces every item to take the per-benchmark
    /// serial mutex, so all iterations of all items run on whichever
    /// worker happens to pick them up next, but never two at once.
    bool concurrency = true;

    // batched scheduler runs at most one tuple at a time per
    // (test_case, group), so the contention this flag was guarding
    // against is impossible by construction.
};

struct parameter_schema {
    std::string algorithm_name;
    std::string param_name;
    std::string json_type;
    std::vector<config_value>     enum_values{};
    std::optional<config_value>   default_value{};
    std::string description;
};

struct algorithm_schema {
    std::string algorithm_name;
    std::vector<parameter_schema> params{};
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

struct test_case_specification {
    std::string name;
    std::string description;
    /// Maximum number of parallel groups this test case may be split
    /// across. The framework caps the actual group count at the total
    /// iteration count across all tuples sharing this test_case (a
    /// group with zero iterations is meaningless, so we never create
    /// one). Read from `test_cases.<name>.concurrency`; defaults to
    /// `default_test_case_concurrency()` when the key is omitted.
    std::size_t concurrency = 0;
    /// View into the parser pool of the owning `benchmark_execution_state`.
    simdjson::dom::element data{};
};

struct benchmark_configuration {
    enum class iteration_order_mode : std::uint8_t {
        item_major,
        interleaved_seeded,
    };
    std::string config_path;
    std::string benchmark_key;
    std::string description;
    bool fast = false;
    concurrency_options concurrency{};
    bool by_regions = false;
    iteration_order_mode iteration_order = iteration_order_mode::item_major;
    std::uint64_t iteration_order_seed = 0;
    /// Tag labels for `--filter tags=...` selection. REQUIRED in the config
    /// JSON (may be an empty array). Order preserved as declared; duplicates
    /// allowed but meaningless. A benchmark with an empty `tags` list can
    /// never be selected by a `tags=` filter (only by `benchmark=` / no filter).
    std::vector<std::string> tags{};
    /// (test_case_name, iterations) pairs declared in `matrix.for`.
    std::vector<std::pair<std::string, std::size_t>> for_each{};
    /// algorithm_name -> param_name -> declared values list (one cross-product axis).
    std::map<std::string, std::map<std::string, std::vector<config_value>>> matrix_over{};
    /// Schemas for every algorithm registered in top-level `params`. Cloned
    /// into the per-benchmark config so matrix expansion can fill defaults.
    std::map<std::string, algorithm_schema> algorithm_schemas{};
    /// Metrics declared by this benchmark. Iteration order preserved.
    std::vector<metric_specification> metrics{};

    /// Optional fail-closed acceptance contract for benchmarks whose result is
    /// only meaningful when the complete declared comparison matrix ran.  The
    /// algorithm list names exact report rows (one expanded tuple per name),
    /// while the test-case map fixes the exact iteration count for each row.
    /// An empty algorithm list disables the acceptance contract.
    struct acceptance_contract {
        struct named_region_sample_multiplicity {
            std::string algorithm{};
            std::string test_case{};
            std::string region{};
            std::string metric{};
            std::size_t exact_samples = 0;

            [[nodiscard]] auto coordinate() const noexcept {
                return std::tie(algorithm, test_case, region, metric);
            }
        };

        std::vector<std::string> exact_algorithm_rows{};
        std::map<std::string, std::size_t> exact_test_case_iterations{};
        bool require_named_region_iteration_coverage = false;
        /// Configured descriptor-weighted total. For every coordinate the
        /// total must divide by its accepted iteration count; that quotient is
        /// the exact raw descriptor count required in each logical iteration.
        std::vector<named_region_sample_multiplicity>
            exact_named_region_sample_multiplicities{};

        [[nodiscard]] bool enabled() const noexcept {
            return !exact_algorithm_rows.empty();
        }
    } acceptance{};
};

struct execution_tuple {
    std::string algorithm_name;
    parameter_map    params{};      // every parameter resolved (declared or defaulted)
};

/// Exact scheduler coordinates observed by one completed measurement_cell iteration.
/// Paired-rule evidence records these values rather than reconstructing the
/// configured shuffle after the run, so a malformed execution order cannot be
/// presented as if it followed the requested interleaved schedule.
struct iteration_schedule_sample {
    std::size_t repeat = 0;
    std::size_t order_position = 0;
    std::uint64_t shuffle_seed = 0;
};

/// Fully resolved source route for one side of a ratio-paired rule.
/// The tuple index and complete parameter map distinguish two expanded tuples
/// that happen to share an algorithm name.
struct paired_rule_route_evidence {
    std::string algorithm{};
    parameter_map params{};
    std::size_t tuple_index = 0;
    std::string test_case{};
    std::string region{};
    std::string metric{};
};

/// Lossless inputs used to form one logical repeat ratio. Named regions may
/// contain multiple same-name descriptors, so the descriptor samples are
/// retained beside the reduced logical values used by the comparator.
struct paired_rule_repeat_evidence {
    std::size_t repeat = 0;
    bool included_after_warmup = false;
    std::vector<double> numerator_samples{};
    std::vector<double> denominator_samples{};
    bool numerator_value_present = false;
    bool denominator_value_present = false;
    double numerator_value = 0.0;
    double denominator_value = 0.0;
    bool ratio_present = false;
    double ratio = 0.0;
    bool numerator_schedule_present = false;
    bool denominator_schedule_present = false;
    iteration_schedule_sample numerator_schedule{};
    iteration_schedule_sample denominator_schedule{};
    bool schedule_identity_matches = false;
};

/// Reconstructable result of one resolved ratio-paired gate. This is the
/// single evidence object shared by the final report and the crash-resilient
/// debug JSONL stream; neither writer derives its own ratios or identities.
struct paired_rule_evidence {
    std::string config_path{};
    std::string benchmark_key{};
    std::string rule_name{};
    std::string test_case{};
    std::string metric{};
    rule_comparator comparator{};
    paired_rule_route_evidence numerator_route{};
    paired_rule_route_evidence denominator_route{};
    std::size_t benchmark_index = 0;
    std::size_t test_case_index = 0;
    std::size_t group_index = 0;
    std::string schedule_mode{};
    std::uint64_t configured_schedule_seed = 0;
    std::vector<paired_rule_repeat_evidence> repeats{};
    std::array<bool, 2> inputs_present{false, false};
    bool observed_paired_ratio_present = false;
    double observed_paired_ratio = 0.0;
    bool accepted = false;
};

/// One (test_case × group × execution_tuple) cell holding iteration
/// metrics for a single group's slice of iterations.
///
/// `measurement_cell` is owned via `std::unique_ptr<measurement_cell>` (see `benchmark_execution_state`)
/// because the framework hands out stable `measurement_cell*` to TBB workers and the
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
/// items inside a batch run sequentially. A `measurement_cell` is therefore never
/// touched by two threads at once, so `samples` does not need any
/// flat work-unit list let two threads race the same cell across
/// Raw descriptor emission counts for one completed logical iteration:
/// region name -> metric name -> number of same-name descriptor samples.
/// Unlike the aggregate `samples` vectors this retains iteration boundaries,
/// which strict acceptance needs to reject skewed descriptor-weighted P50s.
using named_region_iteration_sample_counts =
    std::map<std::string, std::map<std::string, std::size_t>>;

/// Completion evidence retained independently of numeric sample vectors.
/// E2E raw records can be diagnostic/history-eligible while their rejected
/// timing is deliberately absent; this per-repeat evidence lets validation
/// distinguish that disposition from an accidentally missing accepted sample.
struct iteration_completion_evidence {
    bool completed = false;
    bool framework_timing_eligible = true;
    std::set<std::string> emitted_evaluation_metrics{};
};

struct measurement_cell {
    const test_case_specification*  test_case = nullptr;
    const execution_tuple* tuple = nullptr;
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
    /// Stable index in benchmark_execution_state::execution_tuples. Required
    /// because distinct parameter tuples may share one algorithm name.
    std::size_t          tuple_index = 0;

    /// region_name -> metric_name -> sample values. "all_regions" is a
    /// synthetic bucket holding every counter / evaluation metric and the
    /// iteration-level wall_clock_ns for timing-eligible iterations.
    /// Per-region buckets only appear when
    /// `benchmark_configuration::by_regions == true`.
    std::map<std::string, std::map<std::string, std::vector<double>>> samples{};
    /// Named instrumentation buckets may contain more than one descriptor
    /// with the same name. Keep their raw samples unchanged for ratio
    /// evaluation, but separately count at most one contribution per
    /// (iteration, region name, metric).  Strict acceptance can therefore
    /// prove all scheduled iterations contributed without assuming raw sample
    /// count == iteration count.
    std::map<std::string, std::map<std::string, std::size_t>>
        named_region_iteration_contributions{};
    /// One entry per completed local iteration when strict named-region
    /// acceptance is active. The outer vector supplies collision-free local
    /// ordinals; group merging rebases them by concatenating in group-index
    /// order instead of treating each group's local repeat 0 as the same run.
    std::vector<named_region_iteration_sample_counts>
        named_region_iteration_sample_counts{};
    /// Actual scheduler coordinates for every completed iteration when this
    /// benchmark contains at least one paired ratio rule.
    std::vector<iteration_schedule_sample> iteration_schedule_samples{};
    /// Lossless benchmark-specific JSON objects, one per completed
    /// iteration. Written only by this cell's single batch worker.
    std::vector<std::string> raw_samples{};
    /// Indexed by logical repeat. Populated only after any durable raw append
    /// succeeds, in the same commit step as the aggregate sample vectors.
    std::vector<iteration_completion_evidence> iteration_evidence{};

    measurement_cell() = default;
    measurement_cell(const measurement_cell&) = delete;
    measurement_cell& operator=(const measurement_cell&) = delete;
    measurement_cell(measurement_cell&&) = delete;
    measurement_cell& operator=(measurement_cell&&) = delete;
};

/// Per-case (per matrix.over key) debug progress. Populated once, single-
/// threaded, before batches are enqueued; thereafter only the atomics mutate,
/// concurrently across TBB workers (the map structure is read-only). Present
/// only when --debug is active (empty otherwise).
struct case_debug_progress {
    std::atomic<std::size_t> remaining_cells{0};  // cells not yet finished
    std::atomic<bool>        started{false};       // CASE_STARTED already emitted?
    std::atomic<bool>        emitted{false};       // CASE_RESULTS already emitted? (exactly-once)
};

/// Pinned (move + copy deleted) — `simdjson::dom::element` views handed to
/// the user reference `parser`, and any move would invalidate them. The
/// owning slot is `std::shared_ptr<benchmark_execution_state>` inside
/// `benchmark_context::reports`; the shared_ptr lets the non-blocking prepare
/// phase and its later finalizer retain the same stable heap allocation.
struct benchmark_execution_state {
    /// User-facing title from `create_benchmark`, copied here so the report
    /// writer doesn't need a separate channel.
    std::string                         title{};
    simdjson::dom::parser              parser{};
    std::string                         config_body{};
    benchmark_configuration                     config{};
    std::vector<test_case_specification>           test_cases{};
    std::vector<execution_tuple>         execution_tuples{};
    std::vector<std::unique_ptr<measurement_cell>>  cells{};
    /// Every resolved paired ratio gate, including accepted gates. The
    /// report writer emits this vector and the debug sink streams the same
    /// objects as soon as post-run evaluation completes.
    std::vector<paired_rule_evidence>     paired_rule_evidence_records{};
    bool                                raw_samples_enabled = false;
    std::size_t                         driver_benchmark_index = 0;
    /// Per-case (per matrix.over key) debug progress for `--debug`. Keyed by
    /// execution_tuple::algorithm_name (== case name). std::map keeps stable
    /// references; populated before the parallel run, read-only structure
    /// after (only the per-node atomics mutate). Empty unless --debug is on.
    std::map<std::string, case_debug_progress> debug_progress_by_case{};

    // `concurrency.concurrency: false` by admitting at most one batch
    // per benchmark at a time (see batch_scheduler::concurrency_admissible_locked).

    benchmark_execution_state() = default;
    benchmark_execution_state(benchmark_execution_state&&) = delete;
    benchmark_execution_state(const benchmark_execution_state&) = delete;
    benchmark_execution_state& operator=(benchmark_execution_state&&) = delete;
    benchmark_execution_state& operator=(const benchmark_execution_state&) = delete;
};

/// One axis of the `--filter` selector. Each axis may be set at most once.
enum class filter_dimension : std::uint8_t { tags, benchmark, cases };

/// Whole-run filter assembled from every `--filter key=v,v` occurrence.
/// An absent (nullopt) axis means "do not constrain on this axis". A present
/// axis is a non-empty OR-set. `empty()` is true iff no axis is constrained.
struct filter_specification {
    std::optional<std::vector<std::string>> tags{};
    std::optional<std::vector<std::string>> benchmark{};   // benchmark KEYS (singular CLI spelling)
    std::optional<std::vector<std::string>> cases{};       // matrix.over keys

    [[nodiscard]] bool empty() const noexcept {
        return !tags && !benchmark && !cases;
    }
};

struct parsed_cli {
    bool                       fast_filter = false;
    /// `--fast` / `--threads` (warn-and-ignore on collision).
    bool                       ui_mode = false;
    /// Useful when the terminal locale is `C` or DejaVu / JetBrains
    /// Mono is unavailable.
    bool                       ui_ascii_fallback = false;
    /// animation and the periodic 30-s reload tick, and emits a
    /// `\x1b]666;render-N\x07` OSC sentinel after every render so a
    /// PTY-driven Python harness can synchronise on completed frames
    bool                       ui_test_mode = false;
    /// `--debug`: stream a crash-resilient `report_debug_<N>.jsonl` event
    /// log alongside the normal report. Requires a non-empty `filter`.
    bool                       debug = false;
    std::optional<unsigned>    threads_override{};
    std::optional<std::string> report_override{};
    /// much headroom to leave above peak system usage when computing
    /// the dynamic memory cap. See `benchmark_memory_budget::cap_for`.
    double                     memory_buffer_pct = 40.0;
    /// Subset selection assembled from every `--filter key=v,v` occurrence.
    filter_specification                 filter{};
    /// Hard launch errors found while parsing (e.g. duplicate dimension,
    /// malformed value). Each becomes a `validation_diagnostic` Error in run_impl
    /// with code carried as the first ':'-separated token.
    std::vector<std::string>   cli_errors{};
};

/// Identity card for one report. Captured once in `run_impl` and
/// emitted as the first key of the output JSON. `commit` is the SHA of
/// the working-tree HEAD (or empty if not a git repo); `dirty` is
/// `true` whenever `git status --porcelain` reports any uncommitted
/// changes — distinguishing a pristine SHA from a dirty-tree run is
/// essential for `regression_since_commit` cross-reference. `created`
/// is an ISO 8601 UTC string built from `gmtime_r` + `strftime`;
/// `timestamp` is unix seconds for machine consumers.
struct report_metadata {
    std::string  commit{};
    bool         dirty = false;
    std::int64_t timestamp = 0;
    std::string  created{};
    bool         fast = false;
    /// True only when this completed report is safe to use as a future
    /// regression baseline. Blocking findings and evaluation/configuration
    /// errors make the whole capture ineligible; nonblocking diagnostics do
    /// not. Persisted false first, then promoted only after the report passes
    /// its own schema validation.
    bool         history_eligible = false;
};

struct violation {
    std::string config_path;
    std::string benchmark_key;
    std::string rule_name;
    std::string test_case;
    std::string metric;
    std::string lhs_algorithm;
    parameter_map    lhs_full_params{};
    std::string rhs_algorithm;
    parameter_map    rhs_full_params{};
    /// Index 0 = LHS aggregate; index 1 = RHS aggregate. Mirrors the spec's
    /// `"args": [...]` field on the output.
    std::array<double, 2> args{0.0, 0.0};
    /// False marks a selected source that is missing, explicitly not measured,
    /// or contains a non-finite sample. Unavailable rule inputs are themselves
    /// violations, preventing hardware skips, NaNs, or a renamed/bypassed
    /// instrumented stage from silently disabling its gate.
    std::array<bool, 2> args_present{true, true};
    /// Independent rules report [lhs aggregate, rhs aggregate]. Ratio-paired
    /// rules report [aggregate(lhs[i] / rhs[i]), configured paired baseline].
    /// Exact-paired rules report the first unequal [lhs[i], rhs[i]] witnesses.
    /// Keep the mode explicit so report consumers do not conflate these shapes.
    rule_comparator::pairing_mode comparison_mode =
        rule_comparator::pairing_mode::independent_aggregates;
    std::size_t warmup_repetitions = 0;
    bool blocking = true;
};

/// Comparator payload for a regression rule. `threshold%` is forbidden by
/// the schema, so we only carry `tolerance%` — the underlying compare op
/// reuses the existing `comparison_operator` family.
struct regression_operator {
    comparison_operator op = comparison_operator::equal;
    double    tolerance_pct = 0.0;
};

/// Parsed `regression_rules[*]` entry. `regions` is normalised at parse
/// time so the evaluator never has to special-case the
/// `"all_regions"` shorthand: the literal string lands in the vector as
/// the single element.
struct regression_rule {
    std::string config_path;
    std::string name;
    /// Empty means "applies to every benchmark in the same config file".
    std::vector<std::string> benchmarks{};
    /// Benchmarks removed from the include/default-all set. This keeps a
    /// config-global policy authoritative while permitting one benchmark to
    /// carry a more suitable, separately named policy.
    std::vector<std::string> exclude_benchmarks{};
    /// Always non-empty after parse. Defaults to `{"all_regions"}`.
    std::vector<std::string> regions{};
    std::string metric;
    sample_aggregate aggregate = sample_aggregate::sum;
    regression_operator  op{};
    /// Test cases excluded from this rule. Schema requires non-empty if
    /// the array is present at all.
    std::vector<std::string> exclude_cases{};
    /// Algorithms excluded after benchmark selection. This is intentionally
    /// a deny-list: newly added algorithms remain covered by blocking rules
    /// unless they are explicitly classified otherwise.
    std::vector<std::string> exclude_algorithms{};
    /// Diagnostic regressions remain in the report and stderr but do not
    /// affect process success. Blocking is the default.
    bool blocking = true;
};

/// Unfiltered declaration metadata for one registered benchmark. Regression
/// rule references must be validated against this universe rather than the
/// post-CLI-filter report map; otherwise a valid rule scoped to a filtered-out
/// benchmark is misdiagnosed as a typo.
struct regression_benchmark_declaration {
    std::string benchmark_key;
    std::vector<std::string> config_algorithms{};
    std::vector<std::string> config_test_cases{};
    std::vector<std::string> matrix_algorithms{};
    std::vector<std::string> matrix_test_cases{};
    std::vector<std::string> metrics{};
};

using regression_validation_universe =
    std::map<std::string, std::vector<regression_benchmark_declaration>>;

/// One detected regression. Mirrors the user-visible JSON shape: arrays of
/// size 1 for `violating_tuples` / `test_cases` so the writer doesn't need
/// to special-case singletons. `regression_since` is empty-string when
/// the rule wants `null` in the output.
struct regression_entry {
    std::string config_path;
    std::string benchmark;
    std::string rule_name;
    std::string algorithm_name;
    parameter_map    violating_params{};
    std::string test_case;
    std::string region;
    std::string metric;
    sample_aggregate aggregate = sample_aggregate::sum;
    bool        blocking = true;
    double      previously = 0.0;
    double      now = 0.0;
    /// Empty string ⇒ JSON `null`. Otherwise the file basename like
    /// `"report_3.json"`.
    std::string regression_since{};
    /// Commit SHA captured by the historical report identified by
    /// `regression_since`. Empty string ⇒ JSON `null` when
    /// `regression_since` itself is null or that report's commit is empty.
    std::string regression_since_commit{};
};

/// Registry mapping `config_path` → list of regression rules parsed from
/// that file. Populated lazily by the per-benchmark closure under
/// `regression_rules_mu`; consumed by the global validator and the
/// regression evaluator after the parallel benchmark loop has joined.
using regression_rules_by_configuration = std::map<std::string, std::vector<regression_rule>>;

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
struct sanity_warning {
    /// Stable identifier for the check kind. `"metric_looks_constant"`
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
    /// `warning_since`. Empty string ⇒ JSON `null` when `warning_since`
    /// is null or the commit is unavailable or empty.
    std::string warning_since_commit;
};

// Forward declarations used by benchmark_context.
struct progress_state;
struct validation_diagnostic;
class  benchmark_memory_budget;
class  batch_scheduler;
class  debug_report_sink;
class  durable_raw_evidence_sink;

}  // namespace detail

/**
 * @brief Per-benchmark context passed to the `execute` closure.
 *
 * The closure does not see this directly — the type is forward-declared
 * publicly so the public `erased_benchmark` struct can carry a
 * `function<void(benchmark_context&)>` member without exposing the full
 * private layout.
 */
struct benchmark_context {
    std::string                                    data_dir;
    std::string                                    report_dir;
    bool                                            fast_filter = false;
    // the scheduler's admission predicates (see
    // batch_scheduler::bench_lock_admissible_locked).
    detail::progress_state*                          progress = nullptr;
    std::size_t                                     benchmark_index = 0;
    std::mutex*                                     violations_mu = nullptr;
    std::vector<detail::violation>*                 violations = nullptr;
    std::mutex*                                     reports_mu = nullptr;
    /// Map from benchmark_key to the per-benchmark execution state. The
    /// value is `shared_ptr` because `benchmark_execution_state` is pinned
    /// (the simdjson parser inside is non-movable) and must remain alive from
    /// the non-blocking prepare phase through the later finalizer.
    std::map<std::string, std::shared_ptr<detail::benchmark_execution_state>>* reports = nullptr;
    /// Aggregated validation diagnostics across all benchmarks. Each
    /// benchmark's closure appends its schema + semantic findings under
    /// `diagnostics_mu`; `run_impl` prints them and uses the error count
    /// to drive the exit code.
    std::mutex*                                     diagnostics_mu = nullptr;
    std::vector<detail::validation_diagnostic>*            diagnostics = nullptr;
    /// Shared registry of regression rules keyed by config_path. The
    /// closure inserts under `regression_rules_mu` the first time it sees
    /// each unique config; subsequent benchmarks sharing the same config
    /// skip parsing.
    std::mutex*                                     regression_rules_mu = nullptr;
    detail::regression_rules_by_configuration*                regression_rules_by_config = nullptr;
    /// enqueues batches into this scheduler; its returned finalizer drains
    /// them before publishing results, so captured state (`measurement_cell::samples`,
    /// `benchmark_execution_state`, etc.) outlives the bodies. The
    /// scheduler also owns a non-owning reference to the host's
    /// `benchmark_memory_budget` — callers that need budget metadata go through
    /// the scheduler instead of duplicating the pointer here.
    detail::batch_scheduler*                         scheduler = nullptr;
    /// Case allow-list from `--filter cases=...`. nullptr == run all cases.
    /// Points at run_impl's `case_allow_list` (outlives every closure).
    const std::set<std::string>*                    filter_cases = nullptr;
    /// Crash-resilient JSONL event sink for `--debug`. nullptr == no debug
    /// stream. Points at run_impl's `debug_report_sink` (outlives every closure).
    detail::debug_report_sink*                        debug_sink = nullptr;
    /// Durable raw sidecar sink. A raw-enabled iteration is incomplete until
    /// its append and fsync succeed.
    detail::durable_raw_evidence_sink*                 raw_evidence_sink = nullptr;
};

// ----------------- config_value::from_simdjson -----------------

// config_value helpers must be visible to translation units that import the module,
// so the body sits in the public namespace block.
[[nodiscard]] inline config_value config_value::from_simdjson(simdjson::dom::element element) noexcept {
    using element_type = simdjson::dom::element_type;
    config_value out{};
    switch (element.type()) {
        case element_type::INT64: {
            std::int64_t parsed = 0;
            if (element.get(parsed) == simdjson::SUCCESS) {
                out.kind  = value_kind::integer;
                out.integer_value = parsed;
            }
            return out;
        }
        case element_type::UINT64: {
            std::uint64_t parsed = 0;
            if (element.get(parsed) == simdjson::SUCCESS) {
                out.kind  = value_kind::integer;
                out.integer_value = static_cast<std::int64_t>(parsed);
            }
            return out;
        }
        case element_type::DOUBLE: {
            double parsed = 0.0;
            if (element.get(parsed) == simdjson::SUCCESS) {
                out.kind  = value_kind::floating_point;
                out.double_value = parsed;
            }
            return out;
        }
        case element_type::BOOL: {
            bool parsed = false;
            if (element.get(parsed) == simdjson::SUCCESS) {
                out.kind   = value_kind::boolean;
                out.boolean_value = parsed;
            }
            return out;
        }
        case element_type::STRING: {
            std::string_view parsed{};
            if (element.get(parsed) == simdjson::SUCCESS) {
                out.kind  = value_kind::string;
                out.string_value = std::string{parsed};
            }
            return out;
        }
        default:
            // Object / array / null: not a scalar configuration value.
            // Mark with the non-scalar sentinel so semantic validation can
            // refuse it against any declared parameter type. We still keep
            // the JSON text in `string_value` for diagnostic messages.
            out.kind  = value_kind::non_scalar;
            out.string_value = simdjson::to_string(element);
            return out;
    }
}

namespace detail {


/// Hardcoded ceiling on the number of consecutive failed attempts at
/// `on_input` (G2) and `on_cleanup` (G3) before the framework declares
/// the batch unrecoverable. Set to 5 per spec; not exposed via CLI.
inline constexpr std::size_t k_max_task_input_handler_error_count = 5;

/// Maximum number of stack-trace frames to render in the dashboard
/// per failure. Anything past this is dropped with an "elided" marker.
inline constexpr std::size_t k_max_stack_trace_lines = 30;

/// Minimum time the retry-failure row stays in the dashboard before
/// the next attempt is taken. Without this, a fast-failing user
/// lambda (e.g. a `throw` immediately after entry) burns through all
/// 5 retries in microseconds — the dashboard repaints at ~5 Hz, so
/// `retry_attempt > 0` is never observed and the operator sees no UI
/// for the retry sequence at all. 300 ms is slightly longer than the
/// 200 ms render cadence so every attempt's failure state is
/// guaranteed to show up in at least one frame.
inline constexpr std::chrono::milliseconds k_retry_failure_min_display_time{300};

// ----------------- benchmark_memory_budget -----------------

// Forward declaration kept so the renderer can store a pointer.
struct progress_group;

/// Applies the benchmark process cap to snapshots from the common memory
/// budget service. Reservation ownership remains in `batch_scheduler`.
class benchmark_memory_budget {
public:
    /// Cache platform probes so concurrent scheduler and renderer reads
    /// collapse to one sample per window.
    static constexpr std::chrono::milliseconds k_budget_snapshot_ttl{200};

    /// Constructed once in `run_impl`. `memory_buffer_pct` is already
    /// validated to `[0, 50]` by `parse_cli`.
    explicit benchmark_memory_budget(double memory_buffer_pct) noexcept
        : memory_buffer_pct_(memory_buffer_pct)
    {
        DEBUG_ASSERT(memory_buffer_pct >= 0.0 && memory_buffer_pct <= 50.0,
            "benchmark_memory_budget memory_buffer_pct must be in [0, 50]");
    }

    /// Compute the cap (`C = T·(1 − buffer/100)`) for an arbitrary `T`.
    [[nodiscard]] static constexpr std::uint64_t
    cap_for(std::uint64_t total, double buffer_pct) noexcept {
        return static_cast<std::uint64_t>(
            static_cast<double>(total) * (100.0 - buffer_pct) / 100.0);
    }

    /// Scheduler admission room under the process-footprint cap. The live
    /// system-available value is still surfaced in logs/UI, but admission must not
    /// subtract every other process and reclaimable page from this benchmark's
    /// cap: that can make a large serial batch unschedulable even
    /// when the benchmark process itself stayed below the configured cap.
    [[nodiscard]] static constexpr std::uint64_t
    process_room_for(const bioseq::common::memory::memory_snapshot& system_snapshot,
                     double buffer_pct,
                     std::uint64_t reserved) noexcept {
        const auto cap = cap_for(system_snapshot.system_total_bytes, buffer_pct);
        if (reserved > std::numeric_limits<std::uint64_t>::max()
                         - system_snapshot.process_footprint_bytes) {
            return 0;
        }
        const auto committed = system_snapshot.process_footprint_bytes + reserved;
        return cap > committed ? cap - committed : std::uint64_t{0};
    }

    [[nodiscard]] double memory_buffer_pct() const noexcept { return memory_buffer_pct_; }

    /// Live system-memory snapshot. Reads through a 200 ms cache so
    /// many concurrent callers (scheduler + renderer) collapse to one
    /// syscall per cache window. `const` because the cache fields are
    /// `mutable`; observable state of the budget is unchanged.
    [[nodiscard]] bioseq::common::memory::memory_snapshot
    current_system_snapshot() const noexcept {
        std::lock_guard<std::mutex> guard(cache_mu_);
        const auto now_ns =
            static_cast<std::int64_t>(bioseq::instrumentation::clock::now_ns());
        constexpr auto ttl_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    k_budget_snapshot_ttl).count();
        if (cached_snapshot_ns_ == 0 || (now_ns - cached_snapshot_ns_) >= ttl_ns) {
            cached_snapshot_ = system_budget_.snapshot();
            if (cached_snapshot_.system_total_bytes == 0) {
                cached_snapshot_.system_total_bytes =
                    std::numeric_limits<std::uint64_t>::max();
            }
            cached_snapshot_.system_available_bytes = std::min(
                cached_snapshot_.system_available_bytes,
                cached_snapshot_.system_total_bytes);
            cached_snapshot_ns_ = now_ns;
        }
        return cached_snapshot_;
    }

    benchmark_memory_budget(const benchmark_memory_budget&) = delete;
    benchmark_memory_budget(benchmark_memory_budget&&) = delete;
    benchmark_memory_budget& operator=(const benchmark_memory_budget&) = delete;
    benchmark_memory_budget& operator=(benchmark_memory_budget&&) = delete;

private:
    mutable std::mutex                            cache_mu_{};
    mutable bioseq::common::memory::memory_budget system_budget_{};
    mutable bioseq::common::memory::memory_snapshot cached_snapshot_{};
    mutable std::int64_t                          cached_snapshot_ns_ = 0;
    double                                        memory_buffer_pct_;
};

// ----------------- progress_state -----------------

/// One group of one test_case. Hot-path counter is `done`; the
/// scheduler updates `label` only at item boundaries (cold path).
///
/// Failure / wait state observed by the renderer:
///   - `input_retry_attempt`  — current `on_input` retry index (0 = clear).
///   - `cleanup_retry_attempt`— current `on_cleanup` retry index (0 = clear).
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
struct progress_group {
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

    /// Input retry status. 0 = clear; 1..k_max_task_input_handler_error_count
    /// = current attempt (1-based).
    std::atomic<std::uint8_t>  input_retry_attempt{0};
    /// Cleanup retry status. Same shape as input_retry_attempt.
    std::atomic<std::uint8_t>  cleanup_retry_attempt{0};
    /// Nanoseconds since `bioseq::instrumentation::clock::now_ns()` epoch at
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
struct progress_test_case {
    std::string                                test_case_name{};
    std::atomic<std::uint64_t>                 tuple_done{0};       // cumulative items finished
    std::atomic<std::uint64_t>                 tuple_total{0};
    std::vector<std::unique_ptr<progress_group>> groups{};
    std::atomic<std::int64_t>                  start_ns{0};
    std::atomic<std::int64_t>                  finish_ns{0};
};

/// Full progress model. The renderer reads everything; the scheduler
/// writes via three helpers below.
///
/// Per-benchmark `test_cases` is populated by the closure that drives a
/// single `erased_benchmark` (running on some TBB worker) AFTER the
/// renderer thread has already started polling. To avoid a data race
/// on the vector's structural state, the writer signals completion via
/// `setup_done.store(true, release)` and readers gate every traversal
/// on `setup_done.load(acquire)`. After the writer signals, the
/// vector and every `unique_ptr` it holds are stable for the rest of
/// the run.
// Forward-declared so progress_state can carry a non-owning pointer to it
// even though the class body lives further down in the module.
class batch_scheduler;

struct progress_state {
    struct per_benchmark_state {
        std::string                                       title{};
        std::string                                       benchmark_key{};
        std::vector<std::unique_ptr<progress_test_case>>    test_cases{};
        std::atomic<bool>                                 setup_done{false};
    };
    std::vector<std::unique_ptr<per_benchmark_state>> per_benchmark;
    std::atomic<bool> running{true};
    std::atomic<bool> tty_render{false};   // set true if stderr is a TTY at startup
    /// totals consumed by the dashboard header. nullptr = no scheduler
    /// installed (e.g. in `--ui` review mode). Memory-budget access is
    /// routed through the scheduler too, so there's a single owner.
    batch_scheduler*  scheduler     = nullptr;
};

// ----------------- batch_scheduler -----------------

/// One pending or running batch, type-erased. Owned by the scheduler.
/// The body is constructed by `prepare_typed_benchmark` and captures
/// references back into the per-benchmark `benchmark_execution_state` —
/// so the scheduler must guarantee the body has run to completion
/// before the returned finalizer releases that state.
/// `wait_for_benchmark` enforces that contract.
struct scheduled_batch {
    std::size_t      scheduler_id      = 0;
    std::size_t      benchmark_index   = 0;
    /// `concurrency.bench_lock` from the parsed config — true ⇒
    /// scheduler treats the owning benchmark as exclusive across
    /// benchmarks (other benchmarks' batches do not run until every
    /// bench-locked batch from this benchmark has drained).
    bool             bench_lock        = false;
    /// `!concurrency.concurrency` — true ⇒ scheduler admits at most
    /// "Strict latency-sensitive run" semantics).
    bool             concurrency_serial = false;
    /// Memory reservation requested by `on_estimate_resources`.
    std::size_t      base_bytes        = 0;
    std::size_t      extra_bytes       = 0;
    /// Checked once on the enqueueing thread from `base_bytes + extra_bytes`.
    /// Keeping the canonical total here prevents the scheduler's sort,
    /// admission, finish, and diagnostic paths from independently repeating
    /// potentially-overflowing arithmetic.  Enqueue rejects any value that
    /// cannot be represented by this signed accounting type.
    std::int64_t     total_bytes       = 0;
    /// Body executed by the TBB task pool. Wrapped at admission time
    /// by an enclosing lambda that owns a `scoped_scheduler_finish` so
    /// the reservation always releases on every return path.
    std::function<void()> body{};
    /// UI mirror — the renderer reads `idle_start_ns` to colour the
    /// "🕑 waiting Ns / 30s" row when this batch is the sole holdout.
    /// May be nullptr (no progress dashboard installed).
    progress_group*   group_for_ui      = nullptr;
    /// Per-batch idle clock (ns). 0 = "not idle yet". Mutated only by
    /// the scheduler thread under `mu_`.
    std::int64_t     last_idle_start_ns = 0;
    /// Captured at enqueue so `emit_idle_timeout_locked` can mark the
    /// progress UI without re-resolving every chain of indices.
    std::string      location_str{};
    std::string      test_case_name{};
    std::size_t      test_case_index   = 0;
    std::size_t      group_index       = 0;
    /// Non-owning pointer to the benchmark_context used to push
    /// diagnostics + UI updates. Lifetime is the same as `run_impl`.
    benchmark_context* context           = nullptr;
};

/// Free helper used inside the scheduler when mirroring the idle clock
/// `benchmark_memory_budget::mirror_idle_to_ui`.
inline void mirror_idle_to_ui(const scheduled_batch& batch,
                               std::int64_t idle_start_ns) noexcept {
    if (batch.group_for_ui != nullptr) {
        batch.group_for_ui->idle_start_ns.store(
            idle_start_ns, std::memory_order_release);
    }
}

/// Forward declaration so `scoped_scheduler_finish` can refer to the
/// scheduler before its body is defined.
class batch_scheduler;

/// RAII guard installed by the scheduler around every enqueued body.
/// `~scoped_scheduler_finish` calls `batch_scheduler::notify_batch_finished`
/// so any return path (normal, diagnostic-emit, exception inside
/// `cpptrace::try_catch` catchers, or an escaped worker exception captured by
/// the scheduler wrapper) decrements reservations exactly once.
struct scoped_scheduler_finish {
    batch_scheduler* scheduler;
    std::size_t     scheduler_id;
    ~scoped_scheduler_finish() noexcept;
};

/// `tbb::parallel_for_each(batches…)` model.
///
/// Memory reservations live exclusively here — bodies never block on
/// `benchmark_memory_budget` directly. Bench-lock + serial-concurrency rules are
/// also enforced by admission predicates instead of process-global
class batch_scheduler {
public:
    /// Scheduler tick cadence. Wakeups also fire on every batch
    /// completion, so the wall-clock latency between "a slot opens"
    /// and "the next batch is admitted" is dominated by
    /// `notify_batch_finished` rather than the tick (the tick exists
    /// only to refresh memory probes and re-check idle timeouts).
    static constexpr std::chrono::seconds k_scheduler_tick_interval{1};
    /// A pending batch whose constraints don't fit AND whose peers
    /// are all idle for this long aborts with
    /// `memory_budget_constraint_timeout`.
    static constexpr std::chrono::seconds k_max_idle_waiting_time{30};

    batch_scheduler(benchmark_memory_budget& budget,
                    oneapi::tbb::task_arena& arena) noexcept
        : budget_(budget), arena_(arena)
    {
        thread_ = std::thread{[this] { scheduler_loop(); }};
    }

    ~batch_scheduler() noexcept {
        // 1. Stop the scheduler thread.
        // 2. Drain any TBB tasks already enqueued.
        // 3. Members destruct.
        running_.store(false, std::memory_order_release);
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
        // The normal owner calls wait_for_completion(), whose non-noexcept
        // boundary is the one and only place allowed to publish a worker
        // exception. Keep the defensive destructor drain (TBB requires a
        // wait before task_group destruction), but never let an unexpected
        // task-group exception turn noexcept destruction into std::terminate.
        // Worker bodies themselves are caught and stored by the admission
        // wrapper below, so this is only a last-resort TBB guard.
        try {
            (void)tg_.wait();
        } catch (const std::exception& error) {
            std::fprintf(stderr,
                "[bench_framework] batch_scheduler destructor suppressed an "
                "unexpected task-group exception after the explicit join "
                "boundary was bypassed: %s\n",
                error.what());
            std::fflush(stderr);
        } catch (...) {
            std::fputs(
                "[bench_framework] batch_scheduler destructor suppressed an "
                "unexpected non-std task-group exception after the explicit "
                "join boundary was bypassed\n",
                stderr);
            std::fflush(stderr);
        }
    }

    batch_scheduler(const batch_scheduler&) = delete;
    batch_scheduler(batch_scheduler&&) = delete;
    batch_scheduler& operator=(const batch_scheduler&) = delete;
    batch_scheduler& operator=(batch_scheduler&&) = delete;

    /// Add a batch to the pending queue. Thread-safe — called from
    /// every per-benchmark closure during the enqueue phase.
    void enqueue_batch(scheduled_batch batch) noexcept {
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
    /// aborted, join the TBB task group, then rethrow the first captured
    /// worker exception. Called once by `run_impl` after the outer benchmark
    /// loop joins (which, given `wait_for_benchmark` is also called
    /// per-closure, should already be a no-op for lifetime purposes). This is
    /// intentionally non-noexcept: it is the worker-error ownership boundary.
    void wait_for_completion() {
        {
            std::unique_lock<std::mutex> guard(mu_);
            cv_.wait(guard, [this] {
                return total_finished_ >= total_enqueued_;
            });
        }

        // `notify_batch_finished` runs from a scope guard just before its TBB
        // wrapper returns. Waiting on the counters alone can therefore beat
        // the wrapper by a few instructions. Join the task group here, at a
        // deliberately non-noexcept boundary, before observing the captured
        // worker error. Because every worker body is caught below, TBB has no
        // user exception of its own to defer into batch_scheduler's destructor.
        (void)tg_.wait();

        std::exception_ptr worker_exception;
        {
            std::lock_guard<std::mutex> guard(mu_);
            worker_exception = first_worker_exception_;
        }
        if (worker_exception != nullptr) {
            std::rethrow_exception(worker_exception);
        }
    }

    /// Number of worker bodies that escaped through the user callback seam.
    /// The explicit join rethrows the first exception; this count lets the
    /// report diagnostic disclose whether further failures were captured
    /// while the scheduler drained the remaining independent batches.
    [[nodiscard]] std::size_t worker_exception_count() const noexcept {
        std::lock_guard<std::mutex> guard(mu_);
        return worker_exception_count_;
    }

    /// Called from the body's `scoped_scheduler_finish` destructor.
    void notify_batch_finished(std::size_t scheduler_id) noexcept {
        // Splice the finished node into a local list so its destructor
        // (which destroys `body`'s captures — potentially a chain of
        // std::function copies) runs OUTSIDE `mu_`. Keeps lock-hold
        // duration bounded by counter updates only.
        std::list<scheduled_batch> to_drop;
        {
            std::lock_guard<std::mutex> guard(mu_);
            const auto it = std::find_if(running_batches_.begin(),
                                          running_batches_.end(),
                [scheduler_id](const scheduled_batch& candidate) noexcept {
                    return candidate.scheduler_id == scheduler_id;
                });
            DEBUG_ASSERT(it != running_batches_.end(),
                "batch_scheduler::notify_batch_finished: id not found");
            const auto need = it->total_bytes;
            DEBUG_ASSERT(need >= 0,
                "batch_scheduler stored a negative total byte requirement");
            DEBUG_ASSERT(reserved_bytes_ >= need,
                "batch_scheduler reserved_bytes_ underflow");
            reserved_bytes_ -= need;
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
    /// fresh platform probe (cached by `benchmark_memory_budget`).
    struct live_snapshot {
        std::uint64_t total_bytes;
        std::uint64_t cap_bytes;
        std::uint64_t used_bytes;
        std::uint64_t system_available_bytes;
        std::uint64_t reserved_bytes;
        std::uint64_t available_bytes;
        std::size_t   pending_count;
        std::size_t   running_count;
    };
    [[nodiscard]] live_snapshot capture_live_snapshot() const noexcept {
        const auto sys = budget_.current_system_snapshot();
        const auto cap = benchmark_memory_budget::cap_for(sys.system_total_bytes,
                                                budget_.memory_buffer_pct());
        std::lock_guard<std::mutex> guard(mu_);
        const auto reserved = static_cast<std::uint64_t>(
                                  std::max<std::int64_t>(reserved_bytes_, 0));
        const auto available = benchmark_memory_budget::process_room_for(
            sys, budget_.memory_buffer_pct(), reserved);
        return live_snapshot{
            .total_bytes     = sys.system_total_bytes,
            .cap_bytes       = cap,
            .used_bytes      = sys.process_footprint_bytes,
            .system_available_bytes = sys.system_available_bytes,
            .reserved_bytes  = reserved,
            .available_bytes = available,
            .pending_count   = pending_.size(),
            .running_count   = running_batches_.size(),
        };
    }

    [[nodiscard]] double memory_buffer_pct() const noexcept {
        return budget_.memory_buffer_pct();
    }

    [[nodiscard]] bioseq::common::memory::memory_snapshot
    current_system_snapshot() const noexcept {
        return budget_.current_system_snapshot();
    }

private:
    /// Scheduler thread body — admits whatever fits, then sleeps until
    /// either a notify or the next tick. The platform snapshot is taken
    /// OUTSIDE `mu_` so the only path that takes both `mu_` and
    /// `cache_mu_` is `capture_live_snapshot()`/`scheduler_loop`, both in the
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
            cv_.wait_for(guard, k_scheduler_tick_interval);
        }
    }

    /// Greedy admission. The system memory snapshot is provided by
    /// `scheduler_loop` so this body
    /// never re-enters `current_system_snapshot()` while holding `mu_`.
    void admit_locked(
        const bioseq::common::memory::memory_snapshot& system_snapshot) noexcept {
        if (pending_.empty()) return;

        std::uint64_t available = benchmark_memory_budget::process_room_for(
            system_snapshot,
            budget_.memory_buffer_pct(),
            static_cast<std::uint64_t>(std::max<std::int64_t>(reserved_bytes_, 0)));

        // Smallest-first greedy: a small batch admits even if the next
        // larger one would push us over `available`. With closed-set
        // enqueue (no streaming), starvation is impossible (see plan
        // §4.17).
        std::stable_sort(pending_.begin(), pending_.end(),
            [](const scheduled_batch& lhs, const scheduled_batch& rhs) noexcept {
                return lhs.total_bytes < rhs.total_bytes;
            });

        auto pending_iter = pending_.begin();
        while (pending_iter != pending_.end()) {
            const bool total_nonnegative = pending_iter->total_bytes >= 0;
            DEBUG_ASSERT(total_nonnegative,
                "batch_scheduler stored a negative total byte requirement");
            const auto need = total_nonnegative
                ? static_cast<std::uint64_t>(pending_iter->total_bytes)
                : (std::numeric_limits<std::uint64_t>::max)();
            const bool signed_accounting_fits =
                total_nonnegative && reserved_bytes_ >= 0
                && pending_iter->total_bytes
                    <= (std::numeric_limits<std::int64_t>::max)()
                        - reserved_bytes_;
            const bool fits = need <= available && signed_accounting_fits;
            const bool bench_ok = bench_lock_admissible_locked(*pending_iter);
            const bool concurrency_ok =
                concurrency_admissible_locked(*pending_iter);

            if (fits && bench_ok && concurrency_ok) {
                available -= need;
                reserved_bytes_ += pending_iter->total_bytes;
                if (pending_iter->bench_lock) {
                    ++active_bench_lock_holder_[pending_iter->benchmark_index];
                }
                ++running_per_benchmark_[pending_iter->benchmark_index];

                pending_iter->last_idle_start_ns = 0;
                mirror_idle_to_ui(*pending_iter, /*idle_start=*/0);

                running_batches_.push_back(std::move(*pending_iter));
                pending_iter = pending_.erase(pending_iter);

                // The enqueued lambda is an enclosing wrapper that
                // owns the scoped_scheduler_finish on its own stack. The
                // wrapper lives in TBB's task_handle storage, the
                // inner body lives in the std::list node. Erasing the
                // node from notify_batch_finished only destroys the
                // inner body (which has already returned by then).
                auto* running_ptr = std::addressof(running_batches_.back());
                const auto running_id = running_ptr->scheduler_id;
                arena_.enqueue([this, running_ptr, running_id] {
                    scoped_scheduler_finish finish_guard{this, running_id};
                    try {
                        running_ptr->body();
                    } catch (...) {
                        capture_worker_exception(std::current_exception());
                    }
                }, tg_);
            } else {
                // Update idle clock for the rejected batch. While
                // peers are running we RESET the timer to "now" (it
                // never crosses k_max_idle_waiting_time). When this is
                // the sole holdout we LATCH the timer at the first
                // observation.
                const auto now_ns = static_cast<std::int64_t>(
                                         bioseq::instrumentation::clock::now_ns());
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
    /// holdout for longer than `k_max_idle_waiting_time`.
    void check_idle_timeouts_locked() noexcept {
        if (!running_batches_.empty()) return;
        const auto now_ns = static_cast<std::int64_t>(
                                bioseq::instrumentation::clock::now_ns());
        constexpr auto budget_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                k_max_idle_waiting_time).count();
        auto pending_iter = pending_.begin();
        bool aborted_any = false;
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
                aborted_any = true;
            } else {
                ++pending_iter;
            }
        }
        if (aborted_any) cv_.notify_all();
    }

    /// Forward declaration: defined out-of-line below the
    /// `progress_mark_batch_aborted` / `push_runtime_diagnostic`
    /// helpers (which the scheduler depends on but which appear
    /// later in this module).
    void emit_idle_timeout_locked(const scheduled_batch& batch) noexcept;

    [[nodiscard]] bool bench_lock_admissible_locked(
        const scheduled_batch& batch) const noexcept
    {
        if (!active_bench_lock_holder_.empty()) {
            // A lock holder is one globally exclusive batch. No second locked
            // or unlocked batch may join it, including work from the same
            // benchmark.
            return false;
        }

        const auto oldest_pending_lock = std::min_element(
            pending_.begin(), pending_.end(),
            [](const scheduled_batch& lhs,
               const scheduled_batch& rhs) noexcept {
                const auto lhs_id = lhs.bench_lock
                    ? lhs.scheduler_id
                    : (std::numeric_limits<std::size_t>::max)();
                const auto rhs_id = rhs.bench_lock
                    ? rhs.scheduler_id
                    : (std::numeric_limits<std::size_t>::max)();
                return lhs_id < rhs_id;
            });
        const bool has_pending_lock = oldest_pending_lock != pending_.end()
            && oldest_pending_lock->bench_lock;
        const std::size_t lock_id = has_pending_lock
            ? oldest_pending_lock->scheduler_id
            : (std::numeric_limits<std::size_t>::max)();

        if (batch.bench_lock) {
            // Only the oldest lock may acquire, and it must wait until every
            // previously admitted unlocked batch has drained.
            return batch.scheduler_id == lock_id
                && running_batches_.empty();
        }
        // Pending-lock barrier: older unlocked work may drain/admit, but work
        // enqueued after the oldest lock cannot leapfrog it due to the
        // scheduler's size-based sort.
        return !has_pending_lock || batch.scheduler_id < lock_id;
    }

    [[nodiscard]] bool concurrency_admissible_locked(
        const scheduled_batch& batch) const noexcept
    {
        if (!batch.concurrency_serial) return true;
        const auto it = running_per_benchmark_.find(batch.benchmark_index);
        return it == running_per_benchmark_.end() || it->second == 0;
    }

    /// Capture every escaped worker failure before the TBB task returns.
    /// `std::exception_ptr` keeps the original dynamic exception (including
    /// typed memory-admission diagnostics) alive without translating it on a
    /// worker thread. The first failure is rethrown by wait_for_completion;
    /// the count records additional failures without replacing root cause.
    void capture_worker_exception(std::exception_ptr failure) noexcept {
        std::lock_guard<std::mutex> guard(mu_);
        ++worker_exception_count_;
        if (first_worker_exception_ == nullptr) {
            first_worker_exception_ = std::move(failure);
        }
    }

    /// Helper used by `notify_batch_finished` to decrement-or-erase a
    /// `benchmark_index → count` map, with `[[present]]` guarding the
    /// `active_bench_lock_holder_` case where only `bench_lock=true`
    /// batches contribute. Caller holds `mu_`.
    template <typename p_map>
    static void decrement_or_erase_locked(
        p_map& map, typename p_map::key_type key, bool present) noexcept
    {
        if (!present) return;
        const auto it = map.find(key);
        DEBUG_ASSERT(it != map.end(),
            "batch_scheduler counter map underflow");
        if (it != map.end() && --it->second == 0) {
            map.erase(it);
        }
    }

    benchmark_memory_budget&                budget_;
    oneapi::tbb::task_arena&     arena_;
    oneapi::tbb::task_group      tg_{};
    std::thread                  thread_{};
    mutable std::mutex           mu_{};
    std::condition_variable      cv_{};
    std::atomic<bool>            running_{true};

    std::vector<scheduled_batch>  pending_{};
    /// `std::list` (not vector) so iterator/pointer stability holds
    /// across subsequent pushes. The lambda enqueued into TBB
    /// captures `&running_batches_.back()`; a vector reallocation
    /// would invalidate the pointer.
    std::list<scheduled_batch>    running_batches_{};
    std::size_t                  next_id_         = 0;
    std::size_t                  total_enqueued_  = 0;
    std::size_t                  total_finished_  = 0;
    std::int64_t                 reserved_bytes_  = 0;
    std::exception_ptr           first_worker_exception_{};
    std::size_t                  worker_exception_count_ = 0;

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

inline scoped_scheduler_finish::~scoped_scheduler_finish() noexcept {
    scheduler->notify_batch_finished(scheduler_id);
}

// ----------------- Diagnostics -----------------
//
// Validation produces a flat list of `validation_diagnostic` records. Both
// schema-driven errors (from the JSON schema validator) and post-parse
// semantic checks share this type so `report_diagnostics` can pretty-print
// them uniformly. `Severity::Error` aborts; `Severity::Warning` is logged
// but does not stop the run.

/// One validation finding — schema or semantic.
struct validation_diagnostic {
    enum class diagnostic_severity : std::uint8_t { error, warning };
    diagnostic_severity severity = diagnostic_severity::error;
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
/// per validation pass; collected entries are converted to `validation_diagnostic`
/// records by `validate_against_schema`.
class collecting_error_handler : public nlohmann::json_schema::basic_error_handler {
public:
    void error(const nlohmann::json::json_pointer& pointer,
               const nlohmann::json& /*instance*/,
               const std::string& message) override
    {
        nlohmann::json_schema::basic_error_handler::error(pointer, {}, message);
        captured_.push_back({pointer.to_string(), message});
    }

    struct entry { std::string pointer; std::string message; };
    [[nodiscard]] const std::vector<entry>& entries() const noexcept { return captured_; }

private:
    std::vector<entry> captured_;
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

inline constexpr std::string_view k_bench_config_schema = R"json({
  "$schema": "http://json-schema.org/draft-07/schema#",
  "title": "BenchFrameworkConfig",
  "description": "Configuration for one or more benchmarks driven by bioseq::bench::framework. The top-level object always carries the keys 'params' and 'test_cases'; every other top-level key names a benchmark and matches the BenchmarkBlock schema. Benchmark identifiers must be non-empty.",
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
        "description": "Per-algorithm parameter map. Each key is a parameter name; the value follows the parameter_schema definition.",
        "propertyNames": { "minLength": 1 },
        "additionalProperties": { "$ref": "#/definitions/param_schema" }
      },
      "examples": [
        {
          "bioseq_aligner": {
            "minimizer_algorithm": {
              "type": "string",
              "enum": ["simd", "scalar"],
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
      "description": "Optional list of regression rules. Each rule compares aggregated samples in this run to the newest eligible historical report on disk and emits a 'regressions' entry whenever the configured operator/tolerance is violated. Eligibility requires an accepted prior run with meta.history_eligible=true and closed finding arrays, matching run mode, and complete cardinality-consistent evidence for the selected cell; failed, malformed, or incomplete reports are skipped with a non-blocking diagnostic. Regression rules are global to the config file: by default they apply to every benchmark in the same file. The framework writes its run as report_<N>.json (postfixed integer starting at 1).",
      "type": "array",
      "items": { "$ref": "#/definitions/regression_rule" }
    }
  },
  "additionalProperties": { "$ref": "#/definitions/benchmark" },
  "definitions": {
    "param_schema": {
      "title": "parameter_schema",
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
      "required": ["matrix", "tags"],
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
        "tags": {
          "type": "array",
          "description": "Tag labels used by `--filter tags=a,b`. Required; may be empty. Each tag is a non-empty string. A benchmark matches a `tags=` filter iff its tag set intersects the filter's tag set.",
          "items": { "type": "string", "minLength": 1 },
          "examples": [ ["gpu", "e2e"], [] ]
        },
        "concurrency": { "$ref": "#/definitions/concurrency" },
        "iteration_order": {
          "type": "object",
          "required": ["mode"],
          "additionalProperties": false,
          "properties": {
            "mode": { "type": "string", "enum": ["item_major", "interleaved_seeded"] },
            "seed": { "type": "integer", "minimum": 0 }
          },
          "oneOf": [
            {
              "properties": { "mode": { "const": "item_major" } },
              "not": { "required": ["seed"] }
            },
            {
              "required": ["seed"],
              "properties": { "mode": { "const": "interleaved_seeded" } }
            }
          ]
        },
        "matrix": { "$ref": "#/definitions/matrix" }
      },
      "examples": [
        {
          "description": "Compare aligner runtimes",
          "fast": false,
          "tags": ["aligner", "grid"],
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
      "description": "Per-benchmark concurrency knobs. Defaults: bench_lock=false, concurrency=true. The batched scheduler runs at most one tuple at a time against a given (test_case, group) cached input.",
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
                "minimizer_algorithm": ["simd", "scalar"],
                "w": [10, 15, 20]
              }
            }
          ]
        },
        "by_regions": {
          "type": "boolean",
          "default": false,
          "description": "If true, the report splits per-iteration wall_clock_ns by instrumentation region (every named bioseq::instrumentation::region scope contributes its own bucket). When false, only the synthetic 'all_regions' bucket is emitted."
        },
        "acceptance": { "$ref": "#/definitions/acceptance_contract" },
        "metrics": {
          "title": "MetricSpecs",
          "type": "object",
          "minProperties": 1,
          "description": "Metrics to collect, keyed by metric name. The framework recognises three flavours selected via the 'type' field; see the metric_specification oneOf for details.",
          "propertyNames": { "minLength": 1 },
          "additionalProperties": { "$ref": "#/definitions/metric_spec" }
        }
      }
    },
    "acceptance_contract": {
      "title": "CompleteMatrixAcceptanceContract",
      "description": "Fail-closed contract for a comparison benchmark that is meaningful only when its exact matrix completes. The expanded matrix must contain exactly one tuple for each listed algorithm, matrix.for must exactly equal exact_test_case_iterations, case filters may not prune rows, and every row/test-case pair must emit the configured number of finite wall-clock samples.",
      "type": "object",
      "required": ["exact_algorithm_rows", "exact_test_case_iterations", "require_named_region_iteration_coverage", "exact_named_region_sample_multiplicities"],
      "additionalProperties": false,
      "properties": {
        "exact_algorithm_rows": {
          "type": "array",
          "minItems": 1,
          "uniqueItems": true,
          "items": { "type": "string", "minLength": 1 }
        },
        "exact_test_case_iterations": {
          "type": "object",
          "minProperties": 1,
          "propertyNames": { "minLength": 1 },
          "additionalProperties": { "type": "integer", "minimum": 1 }
        },
        "require_named_region_iteration_coverage": {
          "type": "boolean"
        },
        "exact_named_region_sample_multiplicities": {
          "type": "array",
          "items": { "$ref": "#/definitions/named_region_sample_multiplicity" }
        }
      }
    },
    "named_region_sample_multiplicity": {
      "title": "named_region_sample_multiplicity",
      "description": "Exact raw descriptor-weighted sample count for one named rule source. The count must divide evenly by that test case's accepted iteration count; the quotient is also enforced for every individual iteration. This preserves the configured P50 weighting and per-run topology rather than allowing duplicate descriptors to move between iterations.",
      "type": "object",
      "required": ["algorithm", "test_case", "region", "metric", "exact_samples"],
      "additionalProperties": false,
      "properties": {
        "algorithm": { "type": "string", "minLength": 1 },
        "test_case": { "type": "string", "minLength": 1 },
        "region": { "type": "string", "minLength": 1, "not": { "const": "all_regions" } },
        "metric": { "type": "string", "minLength": 1 },
        "exact_samples": { "type": "integer", "minimum": 1 }
      }
    },
    "metric_spec": {
      "title": "metric_specification",
      "description": "One metric. 'type' selects the flavour. Five families: (a) BUILTIN — outer wall_clock_ns measured around on_run; metric key MUST be 'wall_clock_ns' and 'type' is implicit when omitted. (b) REGION — a scalar derived from one or every visited bioseq::instrumentation::region's region_aggregate (e.g. region_total_ns, region_visits, region_p50_ns, region_max_ns, region_total_cycles, region_total_instructions, region_total_branches, region_total_branch_misses, region_ipc, region_branch_miss_rate). (c) LOOP — a scalar from one or every visited bioseq::instrumentation::loop's loop_aggregate (e.g. loop_total_iterations, loop_total_work, loop_p50_per_iter_ns, loop_max_per_iter_ns, loop_mean_per_iter_ns, loop_work_per_iter, loop_empty_visits). (d) COUNTER — snapshot a global bioseq::instrumentation counter; forces serial iteration. (e) EVALUATION — value returned by the user's on_evaluate handler. The metric KEY must match one of the registered builtin names for REGION/LOOP/BUILTIN and is enforced semantically.",
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
      "description": "Built-in wall-clock metric. The metric key MUST be 'wall_clock_ns'; the framework times every iteration with bioseq::instrumentation::clock::now_ns() around on_run and writes the result under 'all_regions.wall_clock_ns'. The 'type' field is optional ('builtin' is implicit when omitted).",
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
      "title": "region_aggregateMetric",
      "description": "A scalar derived from each iteration's bioseq::instrumentation::region aggregates. Recognised metric keys: region_total_ns, region_visits, region_min_ns, region_max_ns, region_p50_ns, region_p99_ns, region_total_cycles, region_total_instructions, region_total_branches, region_total_branch_misses, region_ipc, region_branch_miss_rate. Without 'region', cross-descriptor combiners apply: Sum for additive metrics, summed-histogram + estimate_percentile for p50/p99, event-weighted ratios for IPC and branch_miss_rate. region_min_ns / region_max_ns REQUIRE 'region' (cross-region min/max is meaningless). PMU-derived metrics (cycles, instructions, branches, branch_misses, ipc, branch_miss_rate) report 0 on platforms without PMU access; the framework warns once via the pmu_unavailable diagnostic. region_p50_ns / region_p99_ns return the upper bound of the 32-bucket log2 histogram bucket containing the requested percentile and overshoot the true percentile by up to a factor of 2 — treat them as order-of-magnitude tail-latency indicators, not exact quantiles.",
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
      "title": "loop_aggregateMetric",
      "description": "A scalar derived from each iteration's bioseq::instrumentation::loop aggregates. Recognised metric keys: loop_total_iterations, loop_total_work, loop_empty_visits, loop_min_per_iter_ns, loop_max_per_iter_ns, loop_p50_per_iter_ns, loop_p99_per_iter_ns, loop_mean_per_iter_ns, loop_work_per_iter. Without 'region', cross-descriptor combiners apply (Sum, histogram-summed percentile, ratio-weighted). loop_min_per_iter_ns / loop_max_per_iter_ns REQUIRE 'region'. loop_p50_per_iter_ns / loop_p99_per_iter_ns are coarse percentiles from the 32-bucket log2 per-iteration histogram (bucket-upper-bound, up to factor-of-2 overshoot) — treat as order-of-magnitude only.",
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
      "description": "Snapshot-based counter metric. The framework resolves bioseq::instrumentation::get_named_counter('<counter>'), reads it with current_thread_counter_value before and after each iteration, and records the delta. The delta reflects the current worker's per-thread counter increments, so counter benchmarks may run in parallel.",
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
      "description": "User-computed metric. The metric key MUST appear in the std::map<std::string,double> returned by on_evaluate — the framework reads it from there each iteration. Metrics declared but never emitted by the handler raise a post-run error. applicable_algorithms opts into exact per-accepted-iteration coverage for named matrix rows; omission preserves the legacy report-global contract.",
      "type": "object",
      "required": ["type"],
      "additionalProperties": false,
      "properties": {
        "type": { "type": "string", "const": "evaluation" },
        "applicable_algorithms": {
          "type": "array",
          "minItems": 1,
          "uniqueItems": true,
          "items": { "type": "string", "minLength": 1 }
        },
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
      "title": "benchmark_rule",
      "description": "A 4-tuple [name, lhs_selector, comparator, rhs_selector] expressed as a JSON array. The rule iterates over every (lhs_cell, rhs_cell) pair where the selectors match. Selectors may independently scope the source test_case and instrumentation region; omitted source coordinates mean every configured test case and 'all_regions'. By default the configured aggregate is taken independently from each untouched source metric array. pairing=by_iteration_ratio forms lhs[i]/rhs[i] for every aligned logical repeat (after warmup_repetitions) and aggregates those ratios. pairing=by_iteration_exact requires exact finite IEEE-754 equality at every aligned logical repeat and performs no aggregate reduction.",
      "type": "array",
      "minItems": 4,
      "maxItems": 4,
      "items": [
        {
          "type": "string",
          "minLength": 1,
          "description": "benchmark_rule name. Reported in violations and the stderr summary. Must be unique within the benchmark."
        },
        { "$ref": "#/definitions/selector" },
        { "$ref": "#/definitions/comparator" },
        { "$ref": "#/definitions/selector" }
      ],
      "examples": [
        ["bioseq_faster_than_minimap",
         { "key": "bioseq", "minimizer_algorithm": "scalar" },
         { "compare": "<", "threshold%": 10, "tolerance%": 1 },
         { "key": "minimap2" }]
      ]
    },
    "selector": {
      "title": "rule_selector",
      "description": "Partial-match selector for one side of a rule. The 'key' field names the algorithm. Reserved optional fields 'test_case' and 'region' select the source cell and instrumentation bucket; every other field is a (param_name, expected_value) constraint. Only constrained params must match. The algorithm, source test case, and parameter values are validated semantically.",
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
        },
        "test_case": {
          "type": "string",
          "minLength": 1,
          "description": "Optional exact test-case scope. The name must be present in this benchmark's matrix.for map."
        },
        "region": {
          "type": "string",
          "minLength": 1,
          "description": "Optional exact report region. Omit for all_regions; named regions require a region/loop metric and matrix.by_regions=true."
        }
      },
      "examples": [
        { "key": "bioseq", "minimizer_algorithm": "scalar", "w": 10 },
        { "key": "bioseq", "test_case": "XXL_HIFI", "region": "mm_sketch_simd" },
        { "key": "minimap2" }
      ]
    },
    "aggregate_enum": {
      "title": "sample_aggregate",
      "type": "string",
      "enum": ["sum", "min", "max", "p01", "p50", "p99"],
      "description": "Reduction applied to a per-iteration sample vector. Percentiles select the sample at index floor(p * (n - 1)) after a partial sort via std::nth_element (matches NumPy's 'lower' interpolation for discrete indices)."
    },
    "comparator": {
      "title": "rule_comparator",
      "description": "Two operator families: ordered ('<' or '>') and equality ('='). Spec semantics:\n  '<': pass iff lhs < rhs * (1 - threshold%/100 + tolerance%/100).\n  '>': pass iff lhs > rhs * (1 + threshold%/100 - tolerance%/100).\n  '=': pass iff |lhs - rhs| <= rhs * tolerance%/100.\nBy default both source arrays are reduced independently via aggregate. pairing=by_iteration_ratio first forms the logical-repeat vector lhs[i]/rhs[i], discards warmup_repetitions leading pairs, reduces that vector via aggregate, and compares the result against paired_baseline (default 1.0). This makes tolerance% a direct relative noise allowance around the configured ratio. If a named region has duplicate same-name descriptors, aggregate first reduces that repeat's descriptor partition to one logical value. Ratio-paired rules require an exclusive serial, one-group, interleaved benchmark and fail closed on missing/non-finite samples or a zero RHS. pairing=by_iteration_exact is equality-only, zero-tolerance, evaluation-only, all_regions-only, one-group samplewise identity; it permits concurrent benchmark scheduling because it does not compare timing ratios.\nthreshold% and tolerance% are bounded to [0, 100]. A threshold% or tolerance% of exactly 100 makes the rule trivially pass and produces a warning.",
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
            "aggregate": { "$ref": "#/definitions/aggregate_enum" },
            "pairing": {
              "type": "string",
              "enum": ["independent", "by_iteration_ratio"],
              "default": "independent"
            },
            "paired_baseline": {
              "type": "number",
              "exclusiveMinimum": 0,
              "default": 1.0,
              "description": "Configured reference ratio used as the RHS only for by_iteration_ratio."
            },
            "blocking": {
              "type": "boolean",
              "default": true,
              "description": "False records a diagnostic finding without failing the benchmark process."
            },
            "warmup_repetitions": {
              "type": "integer",
              "minimum": 0,
              "default": 0,
              "description": "Leading logical repeats excluded only from a paired ratio; raw samples remain available."
            }
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
            "aggregate": { "$ref": "#/definitions/aggregate_enum" },
            "pairing": {
              "type": "string",
              "enum": ["independent", "by_iteration_ratio", "by_iteration_exact"],
              "default": "independent",
              "description": "by_iteration_exact compares every aligned finite evaluation sample bit-for-bit and requires zero tolerance."
            },
            "paired_baseline": {
              "type": "number",
              "exclusiveMinimum": 0,
              "default": 1.0,
              "description": "Configured reference ratio used as the RHS only for by_iteration_ratio."
            },
            "blocking": {
              "type": "boolean",
              "default": true,
              "description": "False records a diagnostic finding without failing the benchmark process."
            },
            "warmup_repetitions": {
              "type": "integer",
              "minimum": 0,
              "default": 0,
              "description": "Leading logical repeats excluded only from a paired ratio; raw samples remain available."
            }
          },
          "examples": [
            { "compare": "=", "tolerance%": 1 }
          ]
        }
      ]
    },
    "regression_rule": {
      "title": "regression_rule",
      "description": "One regression check. Iterates a target metric across (benchmark, execution_tuple, test_case, region) combos in the current run, aggregates the samples per the 'aggregate' field, and compares to the same combo in the newest eligible historical report. Candidate selection walks backward over failed, malformed, incomplete, mode-mismatched, or cardinality-incompatible reports, and the same eligibility rule governs regression_since attribution. Operator semantics are inherited from the rule comparator with 'threshold%' fixed at 0; the schema rejects any explicit 'threshold%' in the operator block.",
      "type": "object",
      "required": ["name", "metric", "aggregate", "operator"],
      "additionalProperties": false,
      "properties": {
        "name": {
          "type": "string",
          "minLength": 1,
          "description": "benchmark_rule name. Must be unique within the config file. Reported in 'regressions' entries and the stderr summary."
        },
        "benchmarks": {
          "type": "array",
          "minItems": 1,
          "description": "Optional list of benchmark identifiers this rule applies to. Each entry must name a benchmark block declared in the same config file. When the field is absent, the rule applies to every benchmark in the file.",
          "items": { "type": "string", "minLength": 1 }
        },
        "exclude_benchmarks": {
          "type": "array",
          "minItems": 1,
          "description": "Optional benchmark deny-list applied after 'benchmarks' (or the default all-benchmarks set). Newly added benchmarks remain covered unless explicitly excluded.",
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
        },
        "exclude_algorithms": {
          "type": "array",
          "minItems": 1,
          "description": "Optional algorithm deny-list applied inside the selected benchmarks. Newly added algorithms remain covered unless explicitly excluded.",
          "items": { "type": "string", "minLength": 1 }
        },
        "blocking": {
          "type": "boolean",
          "default": true,
          "description": "False records and prints regression findings as diagnostics without failing the benchmark process. Omitted means true."
        }
      },
      "examples": [
        {
          "name": "minimizer_count_stable",
          "benchmarks": ["bench1"],
          "metric": "minimizer_count",
          "aggregate": "sum",
          "operator": { "compare": "=", "tolerance%": 2 },
          "exclude_cases": ["SMALL"],
          "blocking": true
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
      "title": "regression_operator",
      "description": "Comparator for a regression rule. Accepts the same three operators as the per-rule comparator (<, >, =) but does NOT accept 'threshold%' — that knob is implicitly zero, leaving 'tolerance%' as the only relaxation. An exactly unchanged zero (including -0 versus +0) passes every regression operator before ordered comparison; otherwise, with t = tolerance% / 100: '<' passes iff now < previously*(1+t); '>' passes iff now > previously*(1-t); '=' passes iff |now - previously| <= |previously|*t.",
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

inline constexpr std::string_view k_bench_report_schema = R"json({
  "$schema": "http://json-schema.org/draft-07/schema#",
  "title": "BenchFrameworkReport",
  "description": "Output report.json produced by bioseq::bench::framework. Top-level fields include 'meta' (run identity), 'raw_evidence' (durable raw sidecar authority), 'paired_rule_evidence' (reconstructable paired-ratio gate inputs and outcomes), 'rule_violations' (every rule that didn't pass), 'regressions' (cross-run regression entries), 'sanity_warnings' (hardcoded end-of-run anomaly checks; non-blocking) and 'benchmarks' (per-benchmark sample arrays). The framework re-validates its own report against this schema after writing as a sanity check.",
  "type": "object",
  "required": ["meta", "raw_evidence", "paired_rule_evidence", "rule_violations", "regressions", "sanity_warnings", "benchmarks"],
  "additionalProperties": false,
  "properties": {
    "meta": { "$ref": "#/definitions/meta" },
    "raw_evidence": {
      "description": "Durable raw sidecar manifest, or null when no selected benchmark requested raw evidence.",
      "oneOf": [
        { "type": "null" },
        { "$ref": "#/definitions/raw_evidence_manifest" }
      ]
    },
    "paired_rule_evidence": {
      "type": "array",
      "description": "Lossless evaluator inputs and outcomes for every resolved by_iteration_ratio rule, including all_regions and accepted rules.",
      "items": { "$ref": "#/definitions/paired_rule_evidence" }
    },
    "rule_violations": {
      "type": "array",
      "description": "Every rule that failed to pass. Empty when all rules were satisfied.",
      "items": { "$ref": "#/definitions/violation" }
    },
    "regressions": {
      "type": "array",
      "description": "Every regression rule violation detected against the newest eligible historical report. Historical candidates must be accepted captures with meta.history_eligible=true and closed rule_violations, regressions, and sanity_warnings arrays, match meta.fast, carry complete raw-evidence authority when a sidecar exists, contain the selected cell and finite aggregate, and match the current total/finite sample cardinalities. Empty on the first run after a clean report_dir, when no regression rules are configured, when no eligible baseline exists, or when nothing regressed.",
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
      "title": "report_metadata",
      "description": "Identity and history-authority card for one report. Run identity is captured at start; history_eligible is promoted only after configuration, raw-evidence, within-run-rule, regression, report-write, and report-schema validation.",
      "type": "object",
      "required": ["commit", "dirty", "timestamp", "created", "fast", "history_eligible"],
      "additionalProperties": false,
      "properties": {
        "commit":    { "type": "string", "description": "git rev-parse HEAD output, or empty string when the working dir is not a git repo." },
        "dirty":     { "type": "boolean", "description": "true when git status --porcelain reported any uncommitted changes; false otherwise (also false when not a git repo)." },
        "timestamp": { "type": "integer", "description": "Unix seconds since epoch at run start." },
        "created":   { "type": "string", "description": "ISO 8601 UTC string built from the same instant as timestamp (e.g. 2026-05-07T12:34:56Z)." },
        "fast":      { "type": "boolean", "description": "true when the run was launched with --fast (only fast benchmarks executed)." },
        "history_eligible": { "type": "boolean", "description": "true only when the completed capture has no blocking within-run violation, no blocking regression, no configuration/raw/regression-evaluation error, and its persisted report passed self-schema validation; false reports are retained but skipped as future regression baselines." }
      }
    },
    "param_value": {
      "description": "A single parameter value. config_value serialises into one of these scalar JSON types: string, number (covers integer + double), or boolean.",
      "type": ["string", "number", "boolean"]
    },
    "param_value_object": {
      "description": "Map from parameter name to its concrete value (string / integer / number / boolean).",
      "type": "object",
      "additionalProperties": { "$ref": "#/definitions/param_value" }
    },
    "paired_rule_route": {
      "type": "object",
      "required": ["algorithm", "params", "tuple_index", "test_case", "region", "metric"],
      "additionalProperties": false,
      "properties": {
        "algorithm": { "type": "string" },
        "params": { "$ref": "#/definitions/param_value_object" },
        "tuple_index": { "type": "integer", "minimum": 0 },
        "test_case": { "type": "string" },
        "region": { "type": "string" },
        "metric": { "type": "string" }
      }
    },
    "paired_schedule_sample": {
      "type": "object",
      "required": ["repeat", "order_position", "shuffle_seed"],
      "additionalProperties": false,
      "properties": {
        "repeat": { "type": "integer", "minimum": 0 },
        "order_position": { "type": "integer", "minimum": 0 },
        "shuffle_seed": { "type": "string", "pattern": "^[0-9a-f]{16}$" }
      }
    },
    "paired_rule_repeat": {
      "type": "object",
      "required": ["repeat", "included_after_warmup", "numerator_samples", "denominator_samples", "numerator_value", "denominator_value", "ratio", "numerator_schedule", "denominator_schedule", "schedule_identity_matches"],
      "additionalProperties": false,
      "properties": {
        "repeat": { "type": "integer", "minimum": 0 },
        "included_after_warmup": { "type": "boolean" },
        "numerator_samples": { "type": "array", "items": { "type": ["number", "null"] } },
        "denominator_samples": { "type": "array", "items": { "type": ["number", "null"] } },
        "numerator_value": { "type": ["number", "null"] },
        "denominator_value": { "type": ["number", "null"] },
        "ratio": { "type": ["number", "null"] },
        "numerator_schedule": {
          "oneOf": [
            { "type": "null" },
            { "$ref": "#/definitions/paired_schedule_sample" }
          ]
        },
        "denominator_schedule": {
          "oneOf": [
            { "type": "null" },
            { "$ref": "#/definitions/paired_schedule_sample" }
          ]
        },
        "schedule_identity_matches": { "type": "boolean" }
      }
    },
    "paired_rule_evidence": {
      "title": "paired_rule_evidence",
      "type": "object",
      "required": ["location", "benchmark", "rule_name", "test_case", "metric", "comparison_mode", "numerator_route", "denominator_route", "comparator", "schedule_identity", "repeats", "inputs_present", "observed_paired_ratio", "accepted"],
      "additionalProperties": false,
      "properties": {
        "location": { "type": "string" },
        "benchmark": { "type": "string" },
        "rule_name": { "type": "string" },
        "test_case": { "type": "string" },
        "metric": { "type": "string" },
        "comparison_mode": { "const": "paired_iteration_ratio" },
        "numerator_route": { "$ref": "#/definitions/paired_rule_route" },
        "denominator_route": { "$ref": "#/definitions/paired_rule_route" },
        "comparator": {
          "type": "object",
          "required": ["operator", "threshold_pct", "tolerance_pct", "aggregate", "paired_baseline", "warmup_repetitions", "blocking"],
          "additionalProperties": false,
          "properties": {
            "operator": { "type": "string", "enum": ["<", ">", "="] },
            "threshold_pct": { "type": "number", "minimum": 0, "maximum": 100 },
            "tolerance_pct": { "type": "number", "minimum": 0, "maximum": 100 },
            "aggregate": { "type": "string" },
            "paired_baseline": { "type": "number" },
            "warmup_repetitions": { "type": "integer", "minimum": 0 },
            "blocking": { "type": "boolean" }
          }
        },
        "schedule_identity": {
          "type": "object",
          "required": ["mode", "configured_seed", "benchmark_index", "test_case_index", "group_index"],
          "additionalProperties": false,
          "properties": {
            "mode": { "type": "string", "enum": ["interleaved_seeded", "item_major"] },
            "configured_seed": { "type": "string", "pattern": "^[0-9a-f]{16}$" },
            "benchmark_index": { "type": "integer", "minimum": 0 },
            "test_case_index": { "type": "integer", "minimum": 0 },
            "group_index": { "type": "integer", "minimum": 0 }
          }
        },
        "repeats": { "type": "array", "minItems": 1, "items": { "$ref": "#/definitions/paired_rule_repeat" } },
        "inputs_present": { "type": "array", "minItems": 2, "maxItems": 2, "items": { "type": "boolean" } },
        "observed_paired_ratio": { "type": ["number", "null"] },
        "accepted": { "type": "boolean" }
      }
    },
    "violation": {
      "title": "RuleViolation",
      "description": "One failed within-run rule, with the (LHS, RHS) execution_tuple and the values passed to its comparator.",
      "type": "object",
      "required": ["location", "benchmark", "rule_name", "execution_tuple", "test_case", "metric", "comparison_mode", "warmup_repetitions", "blocking", "args", "args_present"],
      "additionalProperties": false,
      "properties": {
        "location":  { "type": "string", "description": "Path to the config file the rule comes from." },
        "benchmark": { "type": "string", "description": "benchmark_key the rule belongs to." },
        "rule_name": { "type": "string", "description": "benchmark_rule name as declared in the config." },
        "execution_tuple": {
          "description": "Both LHS and RHS algorithm params that triggered this violation. 1-2 keys (LHS-only when both selectors named the same algorithm).",
          "type": "object",
          "minProperties": 1,
          "maxProperties": 2,
          "additionalProperties": { "$ref": "#/definitions/param_value_object" }
        },
        "test_case": { "type": "string", "description": "Name of the test_case the rule fired against." },
        "metric":    { "type": "string", "description": "Metric name compared by the rule." },
        "comparison_mode": {
          "type": "string",
          "enum": ["independent_aggregates", "paired_iteration_ratio", "paired_iteration_exact"],
          "description": "Independent mode reports the two source aggregates. Ratio-paired mode reports an aggregate of per-repeat ratios against its configured paired_baseline. Exact-paired mode reports the first unequal per-repeat value pair."
        },
        "warmup_repetitions": {
          "type": "integer",
          "minimum": 0,
          "description": "Number of leading repeats excluded from a ratio-paired comparison (zero for independent and exact-paired comparisons)."
        },
        "blocking": {
          "type": "boolean",
          "description": "True means this finding contributes to the process exit status; false is diagnostic-only."
        },
        "args": {
          "description": "Independent: [lhs_aggregate, rhs_aggregate]. Ratio-paired: [aggregate(lhs[i]/rhs[i]), paired_baseline]. Exact-paired: the first unequal [lhs[i], rhs[i]] pair. Raw per-iteration source metrics remain unchanged in the benchmark evidence.",
          "type": "array",
          "minItems": 2,
          "maxItems": 2,
          "items": { "type": "number" }
        },
        "args_present": {
          "description": "[lhs_present, rhs_present]. A false entry means the selected source is missing, explicitly not measured, or contains a non-finite sample. Unavailability always fails the rule and the corresponding args entry is the JSON-safe placeholder 0.",
          "type": "array",
          "minItems": 2,
          "maxItems": 2,
          "items": { "type": "boolean" }
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
        },
        "raw_samples": {
          "type": "array",
          "description": "Lossless framework envelopes plus benchmark-specific raw evidence for raw-enabled benchmarks.",
          "items": { "$ref": "#/definitions/raw_sample" }
        }
      }
    },
    "raw_sample": {
      "title": "RawSample",
      "type": "object",
      "required": ["schema", "benchmark", "test_case", "algorithm", "group_index", "group_count", "tuple_index", "repeat", "params", "outer_e2e_ns", "published_wall_clock_ns", "raw_record_id", "schedule", "record_checksum"],
      "additionalProperties": true,
      "properties": {
        "schema": { "type": "integer", "const": 1 },
        "benchmark": { "type": "string", "minLength": 1 },
        "test_case": { "type": "string", "minLength": 1 },
        "algorithm": { "type": "string", "minLength": 1 },
        "group_index": { "type": "integer", "minimum": 0 },
        "group_count": { "type": "integer", "minimum": 1 },
        "tuple_index": { "type": "integer", "minimum": 0 },
        "repeat": { "type": "integer", "minimum": 0 },
        "params": { "$ref": "#/definitions/param_value_object" },
        "outer_e2e_ns": {
          "type": ["integer", "null"],
          "minimum": 0,
          "description": "Framework outer duration for timing-eligible records. Current E2E records with an exact rejected tuple_status carry null and are excluded from every framework timing aggregate."
        },
        "published_wall_clock_ns": {
          "type": ["integer", "null"],
          "minimum": 0,
          "description": "Duration admitted to the wall_clock_ns metric. Equal to outer_e2e_ns by default; an opt-in logical-repeat finalizer may select a previously measured algorithm-only sub-envelope, never a value greater than outer_e2e_ns."
        },
        "raw_record_id": { "type": "integer", "minimum": 0 },
        "schedule": { "$ref": "#/definitions/raw_schedule" },
        "record_checksum": { "type": "string", "pattern": "^[0-9a-f]{16}$" }
      }
    },
    "raw_schedule": {
      "type": "object",
      "required": ["benchmark_index", "test_case_index", "tuple_index", "group_index", "repeat", "mode", "configured_seed", "shuffle_seed", "order_position"],
      "additionalProperties": false,
      "properties": {
        "benchmark_index": { "type": "integer", "minimum": 0 },
        "test_case_index": { "type": "integer", "minimum": 0 },
        "tuple_index": { "type": "integer", "minimum": 0 },
        "group_index": { "type": "integer", "minimum": 0 },
        "repeat": { "type": "integer", "minimum": 0 },
        "mode": { "type": "string", "enum": ["item_major", "interleaved_seeded"] },
        "configured_seed": { "type": "string", "pattern": "^[0-9a-f]{16}$" },
        "shuffle_seed": { "type": "string", "pattern": "^[0-9a-f]{16}$" },
        "order_position": { "type": "integer", "minimum": 0 }
      }
    },
    "raw_duration_summary": {
      "type": "object",
      "required": ["count", "min", "p50", "mean", "p95"],
      "additionalProperties": false,
      "properties": {
        "count": { "type": "integer", "minimum": 0 },
        "min": { "type": ["integer", "null"], "minimum": 0 },
        "p50": { "type": ["integer", "null"], "minimum": 0 },
        "mean": { "type": ["number", "null"], "minimum": 0 },
        "p95": { "type": ["integer", "null"], "minimum": 0 }
      }
    },
    "raw_evidence_cell": {
      "type": "object",
      "required": ["benchmark", "test_case", "algorithm", "tuple_index", "group_index", "record_count", "schedule_sorted_checksum", "outer_e2e_ns", "first_repeat_outer_e2e_ns", "steady_state_outer_e2e_ns", "index_and_map_ns"],
      "additionalProperties": false,
      "properties": {
        "benchmark": { "type": "string", "minLength": 1 },
        "test_case": { "type": "string", "minLength": 1 },
        "algorithm": { "type": "string", "minLength": 1 },
        "tuple_index": { "type": "integer", "minimum": 0 },
        "group_index": { "type": "integer", "minimum": 0 },
        "record_count": { "type": "integer", "minimum": 0 },
        "schedule_sorted_checksum": { "type": "string", "pattern": "^[0-9a-f]{16}$" },
        "outer_e2e_ns": { "$ref": "#/definitions/raw_duration_summary" },
        "first_repeat_outer_e2e_ns": {
          "$ref": "#/definitions/raw_duration_summary",
          "description": "Repeat-zero duration kept separate from steady state. This is not necessarily globally cold because rows may share process/runtime state."
        },
        "steady_state_outer_e2e_ns": {
          "$ref": "#/definitions/raw_duration_summary",
          "description": "Durations for repeats one and later."
        },
        "index_and_map_ns": { "$ref": "#/definitions/raw_duration_summary" }
      }
    },
    "raw_evidence_manifest": {
      "type": "object",
      "required": ["schema", "path", "run_id", "complete", "record_count", "prefix_bytes", "append_order_checksum", "schedule_sorted_checksum", "outer_e2e_ns", "index_and_map_ns", "cells"],
      "additionalProperties": false,
      "properties": {
        "schema": { "type": "integer", "const": 1 },
        "path": { "type": "string", "pattern": "^report_[0-9]+\\.raw\\.jsonl$" },
        "run_id": { "type": "string", "minLength": 1 },
        "complete": { "type": "boolean", "const": true },
        "record_count": { "type": "integer", "minimum": 0 },
        "prefix_bytes": { "type": "integer", "minimum": 1 },
        "append_order_checksum": { "type": "string", "pattern": "^[0-9a-f]{16}$" },
        "schedule_sorted_checksum": { "type": "string", "pattern": "^[0-9a-f]{16}$" },
        "outer_e2e_ns": { "$ref": "#/definitions/raw_duration_summary" },
        "index_and_map_ns": { "$ref": "#/definitions/raw_duration_summary" },
        "cells": {
          "type": "array",
          "items": { "$ref": "#/definitions/raw_evidence_cell" }
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
      "description": "Per-test-case sample buckets. The 'all_regions' key always exists and holds every counter and evaluation metric plus iteration-level wall-clock for timing-eligible iterations. Current rejected E2E iterations contribute no framework timing sample. When 'by_regions' is true on the benchmark, additional keys appear — one per visited bioseq::instrumentation region — each carrying timing-eligible per-iteration wall_clock_ns.",
      "type": "object",
      "minProperties": 1,
      "additionalProperties": { "$ref": "#/definitions/region_block" }
    },
    "region_block": {
      "type": "object",
      "description": "Map from metric name to its fixed-shape aggregated samples, keeping report.json compact even for benchmarks that emit thousands of samples per cell.",
      "additionalProperties": { "$ref": "#/definitions/aggregated_samples" }
    },
    "aggregated_samples": {
      "title": "aggregated_samples",
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
      "title": "aggregate_statistics",
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
      "title": "aggregate_percentiles",
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
      "title": "regression_entry",
      "description": "One detected regression. Granularity: one entry per (rule, benchmark, algorithm, tuple, test_case, region). 'violating_tuples' and 'test_cases' are arrays of size 1 in every entry — they exist as arrays so a future aggregation pass can group entries without breaking the schema. 'regression_since' is the basename of the oldest report on disk where the rule still fails when compared to the current run, or null when the framework hit a missing report (gap) or a tuple/region that disappeared in a historical run.",
      "type": "object",
      "required": ["location", "benchmark", "rule_name", "algorithm_name", "violating_tuples", "test_cases", "region", "metric", "aggregate", "blocking", "previously", "now", "regression_since", "regression_since_commit"],
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
        "blocking":   { "type": "boolean", "description": "True when this finding contributes to process failure; false when it is an explicit diagnostic/confirmation signal." },
        "previously": { "type": "number", "description": "Aggregated metric value from the newest eligible historical report's matching cell." },
        "now":        { "type": "number", "description": "Aggregated metric value from the current run's matching cell." },
        "regression_since": {
          "type": ["string", "null"],
          "description": "Basename of the eligible report where the regression was first observable (oldest eligible historical report still failing against current). Ineligible reports are skipped and can never become attribution origins."
        },
        "regression_since_commit": {
          "type": ["string", "null"],
          "description": "Commit SHA captured by the historical report identified by 'regression_since' (its meta.commit field). Null when 'regression_since' is null or meta.commit was empty in that report."
        }
      }
    },
    "sanity_warning": {
      "title": "sanity_warning",
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
          "description": "meta.commit field of the historical report identified by 'warning_since'. Null when warning_since is null or the commit is unavailable or empty."
        }
      }
    }
  }
})json";

// ----------------- Validation utilities -----------------

/// Format a list of `validation_diagnostic` to stderr. Returns true iff any
/// diagnostic has Severity::Error (caller can use this to drive the exit
/// code / abort flag).
[[nodiscard]] inline bool report_diagnostics(
    std::span<const validation_diagnostic> diagnostics,
    std::string_view phase_label) noexcept
{
    if (diagnostics.empty()) return false;
    int error_count = 0;
    int warning_count = 0;
    for (const auto& diag : diagnostics) {
        if (diag.severity == validation_diagnostic::diagnostic_severity::error) ++error_count;
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
            (diag.severity == validation_diagnostic::diagnostic_severity::error) ? "ERROR" : "WARN ";
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
    benchmark_context& context,
    std::vector<validation_diagnostic>& source) noexcept
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
/// and convert any findings to `validation_diagnostic` records. Catches thrown
/// `std::exception`s from the underlying nlohmann library so the framework
/// itself stays exception-free.
[[nodiscard]] inline std::vector<validation_diagnostic> validate_against_schema(
    const std::string& json_text,
    std::string_view schema_text,
    std::string_view source_label) noexcept
{
    std::vector<validation_diagnostic> diagnostics;
    try {
        const auto schema_document  = nlohmann::json::parse(schema_text);
        const auto target_document  = nlohmann::json::parse(json_text);
        nlohmann::json_schema::json_validator validator;
        validator.set_root_schema(schema_document);
        collecting_error_handler handler;
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
                validation_diagnostic::diagnostic_severity::error,
                "schema",
                std::move(location_text),
                entry.message
            });
        }
    } catch (const std::exception& exc) {
        diagnostics.push_back({
            validation_diagnostic::diagnostic_severity::error,
            "schema_parse",
            std::string{source_label},
            exc.what()
        });
    }
    return diagnostics;
}

// `validate_against_schema` is the single entry point: callers pass either
// `k_bench_config_schema` or `k_bench_report_schema` directly.

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
[[nodiscard]] inline std::map<std::string, algorithm_schema> parse_top_level_params(
    simdjson::dom::object params_object,
    bool& parse_ok)
{
    std::map<std::string, algorithm_schema> result;
    for (auto algorithm_field : params_object) {
        algorithm_schema parsed_algorithm_schema;
        parsed_algorithm_schema.algorithm_name = std::string{algorithm_field.key};

        simdjson::dom::object param_object;
        if (algorithm_field.value.get(param_object) != simdjson::SUCCESS) {
            bioseq_log<logging_level::error>("[bench_framework] params.{} is not an object", algorithm_field.key);
            parse_ok = false;
            continue;
        }

        for (auto param_field : param_object) {
            parameter_schema schema;
            schema.algorithm_name = parsed_algorithm_schema.algorithm_name;
            schema.param_name     = std::string{param_field.key};

            simdjson::dom::object schema_obj;
            if (param_field.value.get(schema_obj) != simdjson::SUCCESS) {
                bioseq_log<logging_level::error>("[bench_framework] params.{}.{} is not an object",
                             parsed_algorithm_schema.algorithm_name, param_field.key);
                parse_ok = false;
                continue;
            }

            std::string_view type_text;
            if (schema_obj["type"].get(type_text) != simdjson::SUCCESS) {
                bioseq_log<logging_level::error>("[bench_framework] params.{}.{} missing 'type'",
                             parsed_algorithm_schema.algorithm_name, param_field.key);
                parse_ok = false;
                continue;
            }
            schema.json_type = std::string{type_text};

            simdjson::dom::array enum_array;
            if (schema_obj["enum"].get(enum_array) == simdjson::SUCCESS) {
                for (auto element : enum_array) {
                    schema.enum_values.push_back(config_value::from_simdjson(element));
                }
            }

            simdjson::dom::element default_element;
            if (schema_obj["default"].get(default_element) == simdjson::SUCCESS) {
                schema.default_value = config_value::from_simdjson(default_element);
            }

            std::string_view description_text;
            if (schema_obj["description"].get(description_text) == simdjson::SUCCESS) {
                schema.description = std::string{description_text};
            }

            parsed_algorithm_schema.params.push_back(std::move(schema));
        }

        result.emplace(parsed_algorithm_schema.algorithm_name, std::move(parsed_algorithm_schema));
    }
    return result;
}

/// Parse the top-level `test_cases` block. Each entry produces a
/// `test_case_specification` whose `data` element is a non-owning view into the parser
/// pool — the parser must outlive the returned vector, which is true because
/// both live as members on `benchmark_execution_state`.
[[nodiscard]] inline std::vector<test_case_specification> parse_test_cases(
    simdjson::dom::object test_cases_object,
    bool& parse_ok)
{
    std::vector<test_case_specification> result;
    for (auto field : test_cases_object) {
        simdjson::dom::object inner_obj;
        if (field.value.get(inner_obj) != simdjson::SUCCESS) {
            bioseq_log<logging_level::error>("[bench_framework] test_cases.{} is not an object", field.key);
            parse_ok = false;
            continue;
        }
        test_case_specification spec;
        spec.name = std::string{field.key};

        std::string_view description_text;
        if (inner_obj["description"].get(description_text) == simdjson::SUCCESS) {
            spec.description = std::string{description_text};
        }

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
            bioseq_log<logging_level::error>("[bench_framework] test_cases.{}.data missing", field.key);
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
[[nodiscard]] inline rule_selector parse_rule_selector(
    simdjson::dom::object selector_obj,
    bool& parse_ok)
{
    rule_selector selector;
    for (auto field : selector_obj) {
        if (field.key == "key") {
            std::string_view algorithm_name;
            if (field.value.get(algorithm_name) != simdjson::SUCCESS) {
                bioseq_log<logging_level::error>("[bench_framework] rule selector 'key' is not a string");
                parse_ok = false;
                return selector;
            }
            selector.algorithm_key = std::string{algorithm_name};
        } else if (field.key == "test_case") {
            std::string_view test_case_name;
            if (field.value.get(test_case_name) != simdjson::SUCCESS) {
                bioseq_log<logging_level::error>("[bench_framework] rule selector 'test_case' is not a string");
                parse_ok = false;
                return selector;
            }
            selector.test_case = std::string{test_case_name};
        } else if (field.key == "region") {
            std::string_view region_name;
            if (field.value.get(region_name) != simdjson::SUCCESS) {
                bioseq_log<logging_level::error>("[bench_framework] rule selector 'region' is not a string");
                parse_ok = false;
                return selector;
            }
            selector.region = std::string{region_name};
        } else {
            selector.match.emplace(std::string{field.key},
                                   config_value::from_simdjson(field.value));
        }
    }
    if (selector.algorithm_key.empty()) {
        bioseq_log<logging_level::error>("[bench_framework] rule selector missing 'key' field");
        parse_ok = false;
    }
    return selector;
}

/// Parse the `compare` op and `tolerance%` slot shared by every
/// comparator object in the framework (per-rule and regression). Returns
/// nullopt + flips `parse_ok` on missing / unknown `compare`. `tolerance%`
/// defaults to 0 silently — the JSON schema enforces both bounds and
/// presence rules upstream, so omission means "use the default".
[[nodiscard]] inline std::optional<std::pair<comparison_operator, double>>
parse_compare_and_tolerance(
    simdjson::dom::object op_obj,
    std::string_view diag_label,
    bool& parse_ok)
{
    std::string_view op_text;
    if (op_obj["compare"].get(op_text) != simdjson::SUCCESS) {
        bioseq_log<logging_level::error>("[bench_framework] {} missing 'compare'", diag_label);
        parse_ok = false;
        return std::nullopt;
    }
    const auto parsed_op = parse_compare_op(op_text);
    if (!parsed_op.has_value()) {
        bioseq_log<logging_level::error>("[bench_framework] {}: unknown compare op '{}'", diag_label, op_text);
        parse_ok = false;
        return std::nullopt;
    }
    double tolerance_pct = 0.0;
    (void)op_obj["tolerance%"].get(tolerance_pct);
    return std::pair{*parsed_op, tolerance_pct};
}

/// Parse a rule's comparator object. `compare` is mandatory; the two `%`
/// fields and `warmup_repetitions` default to zero, and `aggregate` defaults
/// to `"p50"`. See `comparator_passes` for the pass/fail decision and
/// `aggregate_samples` for the reduction.
[[nodiscard]] inline rule_comparator parse_rule_comparator(
    simdjson::dom::object cmp_obj,
    bool& parse_ok)
{
    rule_comparator out;
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
            bioseq_log<logging_level::error>("[bench_framework] rule comparator: unknown aggregate '{}'",
                 aggregate_text);
            parse_ok = false;
        } else {
            out.aggregate = *parsed;
        }
    }

    std::string_view pairing_text;
    if (cmp_obj["pairing"].get(pairing_text) == simdjson::SUCCESS) {
        if (pairing_text == "independent") {
            out.pairing =
                rule_comparator::pairing_mode::independent_aggregates;
        } else if (pairing_text == "by_iteration_ratio") {
            out.pairing = rule_comparator::pairing_mode::by_iteration_ratio;
        } else if (pairing_text == "by_iteration_exact") {
            out.pairing = rule_comparator::pairing_mode::by_iteration_exact;
        } else {
            bioseq_log<logging_level::error>(
                "[bench_framework] rule comparator: unknown pairing '{}'",
                pairing_text);
            parse_ok = false;
        }
    }
    double paired_baseline = 1.0;
    if (cmp_obj["paired_baseline"].get(paired_baseline)
        == simdjson::SUCCESS) {
        out.paired_baseline = paired_baseline;
        out.paired_baseline_explicit = true;
    }
    bool blocking = true;
    if (cmp_obj["blocking"].get(blocking) == simdjson::SUCCESS) {
        out.blocking = blocking;
    }
    std::uint64_t warmup_repetitions = 0;
    if (cmp_obj["warmup_repetitions"].get(warmup_repetitions)
        == simdjson::SUCCESS) {
        if (warmup_repetitions
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::size_t>::max)())) {
            parse_ok = false;
        } else {
            out.warmup_repetitions =
                static_cast<std::size_t>(warmup_repetitions);
        }
    }
    return out;
}

/// Parse one rule entry. Rules are 4-element JSON arrays:
/// `[name, lhs_selector, comparator, rhs_selector]`. Returns nullopt and
/// flips `parse_ok` to false on any structural problem.
[[nodiscard]] inline std::optional<benchmark_rule> parse_one_rule(
    simdjson::dom::array rule_array,
    const std::string& metric_name,
    metric_kind kind,
    bool& parse_ok)
{
    std::vector<simdjson::dom::element> elements;
    for (auto entry : rule_array) elements.push_back(entry);
    if (elements.size() != 4) {
        bioseq_log<logging_level::error>("[bench_framework] rule array must have 4 elements (name, lhs, cmp, rhs); got {}",
                     elements.size());
        parse_ok = false;
        return std::nullopt;
    }

    benchmark_rule rule;
    rule.metric_name = metric_name;
    rule.kind = kind;

    std::string_view rule_name;
    if (elements[0].get(rule_name) != simdjson::SUCCESS) {
        bioseq_log<logging_level::error>("[bench_framework] rule[0] (name) is not a string");
        parse_ok = false;
        return std::nullopt;
    }
    rule.name = std::string{rule_name};

    simdjson::dom::object lhs_obj;
    if (elements[1].get(lhs_obj) != simdjson::SUCCESS) {
        bioseq_log<logging_level::error>("[bench_framework] rule[1] (lhs selector) is not an object");
        parse_ok = false;
        return std::nullopt;
    }
    rule.lhs = parse_rule_selector(lhs_obj, parse_ok);

    simdjson::dom::object cmp_obj;
    if (elements[2].get(cmp_obj) != simdjson::SUCCESS) {
        bioseq_log<logging_level::error>("[bench_framework] rule[2] (comparator) is not an object");
        parse_ok = false;
        return std::nullopt;
    }
    rule.cmp = parse_rule_comparator(cmp_obj, parse_ok);

    simdjson::dom::object rhs_obj;
    if (elements[3].get(rhs_obj) != simdjson::SUCCESS) {
        bioseq_log<logging_level::error>("[bench_framework] rule[3] (rhs selector) is not an object");
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
[[nodiscard]] inline std::optional<regression_operator> parse_regression_operator(
    simdjson::dom::object op_obj, bool& parse_ok)
{
    const auto compare_and_tolerance = parse_compare_and_tolerance(
        op_obj, "regression operator", parse_ok);
    if (!compare_and_tolerance.has_value()) return std::nullopt;
    return regression_operator{
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
[[nodiscard]] inline std::vector<regression_rule> parse_regression_rules(
    simdjson::dom::array rules_array,
    const std::string& config_path,
    bool& parse_ok)
{
    std::vector<regression_rule> result;
    for (auto entry : rules_array) {
        simdjson::dom::object rule_obj;
        if (entry.get(rule_obj) != simdjson::SUCCESS) {
            bioseq_log<logging_level::error>("[bench_framework] regression_rules entry is not an object");
            parse_ok = false;
            continue;
        }

        regression_rule rule;
        rule.config_path = config_path;

        std::string_view name_text;
        if (rule_obj["name"].get(name_text) != simdjson::SUCCESS) {
            bioseq_log<logging_level::error>("[bench_framework] regression rule missing 'name'");
            parse_ok = false;
            continue;
        }
        rule.name = std::string{name_text};

        simdjson::dom::array benchmarks_array;
        if (rule_obj["benchmarks"].get(benchmarks_array) == simdjson::SUCCESS) {
            for (auto bench_entry : benchmarks_array) {
                std::string_view bench_key;
                if (bench_entry.get(bench_key) != simdjson::SUCCESS) {
                    bioseq_log<logging_level::error>("[bench_framework] regression rule '{}' benchmarks[*] is not a string",
                         rule.name);
                    parse_ok = false;
                    continue;
                }
                rule.benchmarks.emplace_back(bench_key);
            }
        }

        simdjson::dom::array exclude_benchmarks_array;
        if (rule_obj["exclude_benchmarks"].get(exclude_benchmarks_array)
            == simdjson::SUCCESS) {
            for (auto bench_entry : exclude_benchmarks_array) {
                std::string_view bench_key;
                if (bench_entry.get(bench_key) != simdjson::SUCCESS) {
                    bioseq_log<logging_level::error>("[bench_framework] regression rule '{}' exclude_benchmarks[*] is not a string",
                         rule.name);
                    parse_ok = false;
                    continue;
                }
                rule.exclude_benchmarks.emplace_back(bench_key);
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
                        bioseq_log<logging_level::error>("[bench_framework] regression rule '{}' regions[*] is not a string",
                             rule.name);
                        parse_ok = false;
                        continue;
                    }
                    rule.regions.emplace_back(region_name);
                }
            } else {
                bioseq_log<logging_level::error>("[bench_framework] regression rule '{}' regions must be a string or array",
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
            bioseq_log<logging_level::error>("[bench_framework] regression rule '{}' missing 'metric'", rule.name);
            parse_ok = false;
            continue;
        }
        rule.metric = std::string{metric_text};

        std::string_view aggregate_text;
        if (rule_obj["aggregate"].get(aggregate_text) != simdjson::SUCCESS) {
            bioseq_log<logging_level::error>("[bench_framework] regression rule '{}' missing 'aggregate'", rule.name);
            parse_ok = false;
            continue;
        }
        const auto parsed_aggregate = parse_aggregate(aggregate_text);
        if (!parsed_aggregate.has_value()) {
            bioseq_log<logging_level::error>("[bench_framework] regression rule '{}' has unknown aggregate '{}'",
                 rule.name, aggregate_text);
            parse_ok = false;
            continue;
        }
        rule.aggregate = *parsed_aggregate;

        simdjson::dom::object op_obj;
        if (rule_obj["operator"].get(op_obj) != simdjson::SUCCESS) {
            bioseq_log<logging_level::error>("[bench_framework] regression rule '{}' missing 'operator'", rule.name);
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
                    bioseq_log<logging_level::error>("[bench_framework] regression rule '{}' exclude_cases[*] is not a string",
                         rule.name);
                    parse_ok = false;
                    continue;
                }
                rule.exclude_cases.emplace_back(exclude_name);
            }
        }

        simdjson::dom::array exclude_algorithms_array;
        if (rule_obj["exclude_algorithms"].get(exclude_algorithms_array)
            == simdjson::SUCCESS) {
            for (auto exclude_entry : exclude_algorithms_array) {
                std::string_view exclude_name;
                if (exclude_entry.get(exclude_name) != simdjson::SUCCESS) {
                    bioseq_log<logging_level::error>("[bench_framework] regression rule '{}' exclude_algorithms[*] is not a string",
                         rule.name);
                    parse_ok = false;
                    continue;
                }
                rule.exclude_algorithms.emplace_back(exclude_name);
            }
        }

        bool blocking = true;
        if (rule_obj["blocking"].get(blocking) == simdjson::SUCCESS) {
            rule.blocking = blocking;
        }

        result.push_back(std::move(rule));
    }
    return result;
}

/// Parse the `matrix.metrics` block. Each entry contributes a `metric_specification`
/// with optional `type`, optional `counter`, and a list of rules. Defaults:
/// `type="builtin"`, `counter=metric_name`.
[[nodiscard]] inline std::vector<metric_specification> parse_metrics(
    simdjson::dom::object metrics_obj,
    bool& parse_ok)
{
    std::vector<metric_specification> result;
    for (auto field : metrics_obj) {
        metric_specification metric;
        metric.name = std::string{field.key};

        simdjson::dom::object inner_obj;
        if (field.value.get(inner_obj) != simdjson::SUCCESS) {
            bioseq_log<logging_level::error>("[bench_framework] metrics.{} is not an object", field.key);
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
            bioseq_log<logging_level::error>("[bench_framework] metrics.{}: unknown type '{}'", field.key, type_text);
            parse_ok = false;
            continue;
        }
        metric.declared_source = *parsed_source;

        // Map (source, metric_name) → metric_kind. Counter and Evaluation
        // bypass the registry — their metric_kind is fixed. Builtin /
        // Region / Loop look up the canonical entry; mismatches between
        // the declared `type` and the entry's source are deferred to
        // semantic_validate_config so all errors collect in one pass.
        switch (*parsed_source) {
            case metric_source::counter:
                metric.kind = metric_kind::counter;
                break;
            case metric_source::evaluation:
                metric.kind = metric_kind::evaluation;
                break;
            case metric_source::outer_clock:
            case metric_source::region:
            case metric_source::loop: {
                const auto* entry = lookup_builtin_metric(metric.name);
                if (entry != nullptr) {
                    metric.kind = entry->kind;
                } else {
                    // Unknown name — fall back to a placeholder that
                    // semantic_validate_config rejects with a precise
                    // message based on the declared `type`.
                    metric.kind = metric_kind::wall_clock_ns;
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
        simdjson::dom::array applicable_algorithms;
        if (inner_obj["applicable_algorithms"].get(applicable_algorithms)
                == simdjson::SUCCESS) {
            for (auto algorithm_value : applicable_algorithms) {
                std::string_view algorithm;
                if (algorithm_value.get(algorithm) != simdjson::SUCCESS) {
                    bioseq_log<logging_level::error>(
                        "[bench_framework] metrics.{}.applicable_algorithms[*] is not a string",
                        field.key);
                    parse_ok = false;
                    continue;
                }
                metric.applicable_algorithms.emplace_back(algorithm);
            }
        }

        simdjson::dom::array rules_array;
        if (inner_obj["rules"].get(rules_array) == simdjson::SUCCESS) {
            for (auto entry : rules_array) {
                simdjson::dom::array as_array;
                if (entry.get(as_array) != simdjson::SUCCESS) {
                    bioseq_log<logging_level::error>("[bench_framework] metric '{}' rule entry is not an array", metric.name);
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
/// config and fill the `benchmark_configuration`. The benchmark's identity, schema,
/// and test_cases are populated separately by the caller. Returns false on
/// any structural problem (missing `matrix`, missing `for`/`over`, etc.).
[[nodiscard]] inline bool parse_benchmark_block(
    simdjson::dom::object benchmark_obj,
    benchmark_configuration& benchmark)
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

    // tags: required array of non-empty strings (schema already enforced the
    // shape; this is the structural walk). An empty array is legal.
    simdjson::dom::array tags_array;
    if (benchmark_obj["tags"].get(tags_array) == simdjson::SUCCESS) {
        for (auto entry : tags_array) {
            std::string_view tag_text;
            if (entry.get(tag_text) != simdjson::SUCCESS) {
                bioseq_log<logging_level::error>("[bench_framework] benchmark '{}' tag entry is not a string",
                                benchmark.benchmark_key);
                parse_ok = false;
                continue;
            }
            benchmark.tags.emplace_back(tag_text);
        }
    } else {
        // Schema makes this unreachable, but fail loudly if a caller bypassed it.
        bioseq_log<logging_level::error>("[bench_framework] benchmark '{}' missing required 'tags' array",
                        benchmark.benchmark_key);
        parse_ok = false;
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

    simdjson::dom::object iteration_order_obj;
    if (benchmark_obj["iteration_order"].get(iteration_order_obj)
        == simdjson::SUCCESS) {
        std::string_view mode;
        if (iteration_order_obj["mode"].get(mode) != simdjson::SUCCESS) {
            parse_ok = false;
        } else if (mode == "item_major") {
            benchmark.iteration_order =
                benchmark_configuration::iteration_order_mode::item_major;
            simdjson::dom::element explicit_seed;
            if (iteration_order_obj["seed"].get(explicit_seed)
                == simdjson::SUCCESS) {
                parse_ok = false;
            }
        } else if (mode == "interleaved_seeded") {
            benchmark.iteration_order =
                benchmark_configuration::iteration_order_mode::interleaved_seeded;
            std::uint64_t seed = 0;
            if (iteration_order_obj["seed"].get(seed)
                != simdjson::SUCCESS) {
                parse_ok = false;
            } else {
                benchmark.iteration_order_seed = seed;
            }
        } else {
            parse_ok = false;
        }
        if (!parse_ok) {
            bioseq_log<logging_level::error>(
                "[bench_framework] benchmark '{}' has invalid iteration_order",
                benchmark.benchmark_key);
        }
    }

    simdjson::dom::object matrix_obj;
    if (benchmark_obj["matrix"].get(matrix_obj) != simdjson::SUCCESS) {
        bioseq_log<logging_level::error>("[bench_framework] benchmark '{}' missing 'matrix'", benchmark.benchmark_key);
        return false;
    }

    bool by_regions_flag = false;
    if (matrix_obj["by_regions"].get(by_regions_flag) == simdjson::SUCCESS) {
        benchmark.by_regions = by_regions_flag;
    }

    simdjson::dom::object acceptance_obj;
    if (matrix_obj["acceptance"].get(acceptance_obj) == simdjson::SUCCESS) {
        simdjson::dom::array exact_algorithms;
        if (acceptance_obj["exact_algorithm_rows"].get(exact_algorithms)
            != simdjson::SUCCESS) {
            parse_ok = false;
        } else {
            for (auto entry : exact_algorithms) {
                std::string_view algorithm;
                if (entry.get(algorithm) != simdjson::SUCCESS) {
                    parse_ok = false;
                    continue;
                }
                benchmark.acceptance.exact_algorithm_rows.emplace_back(
                    algorithm);
            }
        }

        simdjson::dom::object exact_test_cases;
        if (acceptance_obj["exact_test_case_iterations"].get(exact_test_cases)
            != simdjson::SUCCESS) {
            parse_ok = false;
        } else {
            for (auto field : exact_test_cases) {
                std::int64_t iterations = 0;
                if (field.value.get(iterations) != simdjson::SUCCESS
                    || iterations <= 0) {
                    parse_ok = false;
                    continue;
                }
                benchmark.acceptance.exact_test_case_iterations.emplace(
                    std::string{field.key},
                    static_cast<std::size_t>(iterations));
            }
        }

        bool require_named_coverage = false;
        if (acceptance_obj["require_named_region_iteration_coverage"].get(
                require_named_coverage) != simdjson::SUCCESS) {
            parse_ok = false;
        } else {
            benchmark.acceptance.require_named_region_iteration_coverage =
                require_named_coverage;
        }

        simdjson::dom::array exact_named_multiplicities;
        if (acceptance_obj["exact_named_region_sample_multiplicities"].get(
                exact_named_multiplicities) != simdjson::SUCCESS) {
            parse_ok = false;
        } else {
            for (auto entry : exact_named_multiplicities) {
                simdjson::dom::object object;
                if (entry.get(object) != simdjson::SUCCESS) {
                    parse_ok = false;
                    continue;
                }
                benchmark_configuration::acceptance_contract::
                    named_region_sample_multiplicity multiplicity;
                std::string_view algorithm;
                std::string_view test_case;
                std::string_view region;
                std::string_view metric;
                std::int64_t exact_samples = 0;
                if (object["algorithm"].get(algorithm) != simdjson::SUCCESS
                    || object["test_case"].get(test_case) != simdjson::SUCCESS
                    || object["region"].get(region) != simdjson::SUCCESS
                    || object["metric"].get(metric) != simdjson::SUCCESS
                    || object["exact_samples"].get(exact_samples)
                        != simdjson::SUCCESS
                    || exact_samples <= 0) {
                    parse_ok = false;
                    continue;
                }
                multiplicity.algorithm = std::string{algorithm};
                multiplicity.test_case = std::string{test_case};
                multiplicity.region = std::string{region};
                multiplicity.metric = std::string{metric};
                multiplicity.exact_samples =
                    static_cast<std::size_t>(exact_samples);
                benchmark.acceptance
                    .exact_named_region_sample_multiplicities.push_back(
                        std::move(multiplicity));
            }
        }
        if (!parse_ok) {
            bioseq_log<logging_level::error>(
                "[bench_framework] benchmark '{}' has invalid matrix.acceptance",
                benchmark.benchmark_key);
        }
    }

    simdjson::dom::object for_obj;
    if (matrix_obj["for"].get(for_obj) != simdjson::SUCCESS) {
        bioseq_log<logging_level::error>("[bench_framework] benchmark '{}' missing 'matrix.for'", benchmark.benchmark_key);
        return false;
    }
    for (auto field : for_obj) {
        std::int64_t iterations = 0;
        if (field.value.get(iterations) != simdjson::SUCCESS || iterations < 0) {
            bioseq_log<logging_level::error>("[bench_framework] matrix.for.{} must be a non-negative integer", field.key);
            parse_ok = false;
            continue;
        }
        benchmark.for_each.emplace_back(std::string{field.key},
                                         static_cast<std::size_t>(iterations));
    }

    simdjson::dom::object over_obj;
    if (matrix_obj["over"].get(over_obj) != simdjson::SUCCESS) {
        bioseq_log<logging_level::error>("[bench_framework] benchmark '{}' missing 'matrix.over'", benchmark.benchmark_key);
        return false;
    }
    for (auto algo_field : over_obj) {
        simdjson::dom::object params_for_algo;
        if (algo_field.value.get(params_for_algo) != simdjson::SUCCESS) {
            bioseq_log<logging_level::error>("[bench_framework] matrix.over.{} is not an object", algo_field.key);
            parse_ok = false;
            continue;
        }
        std::map<std::string, std::vector<config_value>> per_algo;
        for (auto param_field : params_for_algo) {
            simdjson::dom::array values_array;
            if (param_field.value.get(values_array) != simdjson::SUCCESS) {
                bioseq_log<logging_level::error>("[bench_framework] matrix.over.{}.{} is not an array",
                             algo_field.key, param_field.key);
                parse_ok = false;
                continue;
            }
            std::vector<config_value> values;
            for (auto entry : values_array) {
                values.push_back(config_value::from_simdjson(entry));
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
/// resulting `benchmark_execution_state` owns the parser and config body so
/// the simdjson `dom::element` views handed to the user remain valid for
/// the entire run.
/// Outcome of `parse_config_file`. `ok=true` means the parser walked the
/// document; `diagnostics` carries every issue found (schema violations
/// from the JSON-schema pass plus structural problems from the simdjson
/// walk). On parse failure `ok=false` and the caller must abort *before*
/// reading anything else from the state — the partial parse is undefined.
struct config_parse_outcome {
    bool ok = true;
    std::vector<validation_diagnostic> diagnostics{};
    /// Top-level `regression_rules` array, parsed and normalised. The
    /// closure registers these into the global `regression_rules_by_configuration`
    /// registry the first time it sees a given `config_path` (subsequent
    /// benchmarks sharing the file see the entry already present and
    /// skip).
    std::vector<regression_rule> regression_rules{};
};

[[nodiscard]] inline config_parse_outcome parse_config_file(
    const std::string& config_path,
    const std::string& config_open_path,
    const std::string& benchmark_key,
    benchmark_execution_state& state)
{
    config_parse_outcome outcome;
    state.config.config_path   = config_path;
    state.config.benchmark_key = benchmark_key;
    state.config_body = slurp_file(config_open_path);
    if (state.config_body.empty()) {
        outcome.ok = false;
        outcome.diagnostics.push_back({
            validation_diagnostic::diagnostic_severity::error,
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
        state.config_body, k_bench_config_schema, config_path);
    for (auto& diag : schema_diagnostics) {
        if (diag.severity == validation_diagnostic::diagnostic_severity::error) outcome.ok = false;
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
            validation_diagnostic::diagnostic_severity::error,
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
            validation_diagnostic::diagnostic_severity::error,
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
            validation_diagnostic::diagnostic_severity::error,
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
            validation_diagnostic::diagnostic_severity::error,
            "missing_test_cases",
            config_path,
            "config has no top-level 'test_cases' object"
        });
    }

    simdjson::dom::element benchmark_element;
    if (root_obj[benchmark_key].get(benchmark_element) != simdjson::SUCCESS) {
        outcome.ok = false;
        outcome.diagnostics.push_back({
            validation_diagnostic::diagnostic_severity::error,
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
            validation_diagnostic::diagnostic_severity::error,
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

    if (!parse_ok) {
        outcome.ok = false;
        const bool already_has_error = std::any_of(
            outcome.diagnostics.begin(), outcome.diagnostics.end(),
            [](const validation_diagnostic& diagnostic) {
                return diagnostic.severity
                    == validation_diagnostic::diagnostic_severity::error;
            });
        if (!already_has_error) {
            outcome.diagnostics.push_back({
                validation_diagnostic::diagnostic_severity::error,
                "structural_parse_failed",
                std::string{config_path} + ":" + benchmark_key,
                "configuration failed structural parsing"
            });
        }
    }
    return outcome;
}

// ----------------- Semantic config validation -----------------
//
// Once the JSON schema and the simdjson walk have populated
// `benchmark_execution_state`, the framework runs cross-field checks the
// schema cannot express:
//   - matrix.over.<algorithm> references an algorithm declared in 'params'
//   - matrix.over.<algorithm>.<parameter> references a declared parameter
//   - every value in matrix.over.<algorithm>.<parameter> matches the
//     parameter's declared type (and 'enum', if present)
//   - matrix.for.<test_case> references a declared test case
//   - rule selectors point at a declared algorithm/test case and only mention
//     declared parameters with type-correct values; named source regions are
//     used only with by-regions region/loop metrics
//   - rule names are unique within a benchmark
//   - threshold% / tolerance% of exactly 100 raises a warning ("rule will
//     always pass")
//   - test_cases declared with zero iterations raise a warning

/// True iff `value` matches the declared `param`. Type promotion: a
/// `number`-typed parameter accepts both Integer and Double values.
[[nodiscard]] inline bool value_matches_param_schema(
    const config_value& value, const parameter_schema& param) noexcept
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
[[nodiscard]] inline const parameter_schema* find_param_schema(
    const std::map<std::string, algorithm_schema>& schemas,
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
    const benchmark_execution_state& state, std::string_view sub_path) noexcept
{
    std::string out = state.config.config_path;
    out += ':';
    out += state.config.benchmark_key;
    out += '/';
    out += sub_path;
    return out;
}

/// Render a config_value's *kind* as a JSON-Schema-style type name, used by
/// `value_type_mismatch` diagnostics so the user sees what was passed vs.
/// what was declared.
[[nodiscard]] inline std::string_view config_value_kind_name(const config_value& value) noexcept {
    switch (value.kind) {
        case config_value::value_kind::string:    return "string";
        case config_value::value_kind::integer:   return "integer";
        case config_value::value_kind::floating_point:    return "number";
        case config_value::value_kind::boolean:   return "boolean";
        case config_value::value_kind::non_scalar: return "non-scalar (object/array/null)";
    }
    return "?";
}

/// Walk every cross-reference inside a parsed `benchmark_execution_state`
/// and emit `validation_diagnostic` records for anything inconsistent. Returns a
/// flat vector covering every benchmark-level concern (no early-exit so
/// the user sees every issue in one pass).
[[nodiscard]] inline std::vector<validation_diagnostic> semantic_validate_config(
    const benchmark_execution_state& state) noexcept
{
    std::vector<validation_diagnostic> diagnostics;
    const auto& config  = state.config;
    const auto& schemas = config.algorithm_schemas;

    auto emit = [&](validation_diagnostic::diagnostic_severity severity,
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
        emit(validation_diagnostic::diagnostic_severity::error,   std::move(code),
             std::move(sub_path), std::move(message));
    };
    auto warning = [&](std::string code, std::string sub_path, std::string message) {
        emit(validation_diagnostic::diagnostic_severity::warning, std::move(code),
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

    // 2a. A complete-matrix acceptance contract is deliberately redundant
    // with matrix.for/matrix.over: the second declaration freezes the exact
    // acceptance shape so an accidental config edit cannot silently redefine
    // what a "complete" report means.
    if (config.acceptance.enabled()) {
        const std::set<std::string> expected_algorithms{
            config.acceptance.exact_algorithm_rows.begin(),
            config.acceptance.exact_algorithm_rows.end()};
        std::set<std::string> configured_algorithms;
        for (const auto& [algorithm, ignored] : config.matrix_over) {
            (void)ignored;
            configured_algorithms.insert(algorithm);
        }
        if (configured_algorithms != expected_algorithms) {
            error("acceptance_algorithm_matrix_mismatch",
                  "matrix.acceptance.exact_algorithm_rows",
                  "exact_algorithm_rows must equal matrix.over keys exactly");
        }

        const std::map<std::string, std::size_t> configured_test_cases{
            config.for_each.begin(), config.for_each.end()};
        if (configured_test_cases
            != config.acceptance.exact_test_case_iterations) {
            error("acceptance_test_case_matrix_mismatch",
                  "matrix.acceptance.exact_test_case_iterations",
                  "exact_test_case_iterations must equal matrix.for exactly");
        }
        if (config.acceptance.require_named_region_iteration_coverage
            && !config.by_regions) {
            error("acceptance_named_region_coverage_requires_by_regions",
                  "matrix.acceptance.require_named_region_iteration_coverage",
                  "named-region iteration coverage requires matrix.by_regions=true");
        }

        using named_source_coordinate =
            std::tuple<std::string, std::string, std::string, std::string>;
        std::set<named_source_coordinate> named_rule_sources;
        const auto add_named_rule_source =
            [&config, &named_rule_sources](
                const rule_selector& selector,
                const std::string& metric_name) {
                if (selector.region.empty()
                    || selector.region == "all_regions") return;
                if (!selector.test_case.empty()) {
                    named_rule_sources.emplace(
                        selector.algorithm_key, selector.test_case,
                        selector.region, metric_name);
                    return;
                }
                for (const auto& [test_case, ignored] : config.for_each) {
                    (void)ignored;
                    named_rule_sources.emplace(
                        selector.algorithm_key, test_case,
                        selector.region, metric_name);
                }
            };
        for (const auto& metric : config.metrics) {
            for (const auto& rule : metric.rules) {
                add_named_rule_source(rule.lhs, metric.name);
                add_named_rule_source(rule.rhs, metric.name);
            }
        }

        std::set<named_source_coordinate> configured_multiplicities;
        for (const auto& multiplicity :
             config.acceptance.exact_named_region_sample_multiplicities) {
            const named_source_coordinate coordinate{
                multiplicity.algorithm,
                multiplicity.test_case,
                multiplicity.region,
                multiplicity.metric};
            if (!configured_multiplicities.insert(coordinate).second) {
                error("acceptance_duplicate_named_region_multiplicity",
                      "matrix.acceptance.exact_named_region_sample_multiplicities",
                      fmt::format("duplicate exact multiplicity for '{}'/{} region '{}' metric '{}'",
                                  multiplicity.algorithm,
                                  multiplicity.test_case,
                                  multiplicity.region,
                                  multiplicity.metric));
            }
            if (!expected_algorithms.contains(multiplicity.algorithm)) {
                error("acceptance_named_region_unknown_algorithm",
                      "matrix.acceptance.exact_named_region_sample_multiplicities",
                      fmt::format("named-region multiplicity references algorithm '{}' outside exact_algorithm_rows",
                                  multiplicity.algorithm));
            }
            const auto test_case_it =
                config.acceptance.exact_test_case_iterations.find(
                    multiplicity.test_case);
            if (test_case_it
                == config.acceptance.exact_test_case_iterations.end()) {
                error("acceptance_named_region_unknown_test_case",
                      "matrix.acceptance.exact_named_region_sample_multiplicities",
                      fmt::format("named-region multiplicity references test case '{}' outside exact_test_case_iterations",
                                  multiplicity.test_case));
            } else if (multiplicity.exact_samples < test_case_it->second) {
                error("acceptance_named_region_multiplicity_below_iterations",
                      "matrix.acceptance.exact_named_region_sample_multiplicities",
                      fmt::format("named-region multiplicity {} for '{}'/{} region '{}' is below its {} required contributing iterations",
                                  multiplicity.exact_samples,
                                  multiplicity.algorithm,
                                  multiplicity.test_case,
                                  multiplicity.region,
                                  test_case_it->second));
            } else if (multiplicity.exact_samples % test_case_it->second
                       != 0) {
                error("acceptance_named_region_multiplicity_not_uniform",
                      "matrix.acceptance.exact_named_region_sample_multiplicities",
                      fmt::format("named-region multiplicity {} for '{}'/{} region '{}' cannot be distributed uniformly across {} required iterations",
                                  multiplicity.exact_samples,
                                  multiplicity.algorithm,
                                  multiplicity.test_case,
                                  multiplicity.region,
                                  test_case_it->second));
            }
            const bool metric_declared = std::any_of(
                config.metrics.begin(), config.metrics.end(),
                [&multiplicity](const metric_specification& metric) {
                    return metric.name == multiplicity.metric;
                });
            if (!metric_declared) {
                error("acceptance_named_region_unknown_metric",
                      "matrix.acceptance.exact_named_region_sample_multiplicities",
                      fmt::format("named-region multiplicity references undeclared metric '{}'",
                                  multiplicity.metric));
            }
        }
        if (config.acceptance.require_named_region_iteration_coverage) {
            if (configured_multiplicities != named_rule_sources) {
                error("acceptance_named_region_multiplicity_set_mismatch",
                      "matrix.acceptance.exact_named_region_sample_multiplicities",
                      "exact named-region multiplicities must cover every named rule source exactly, with no missing or unused coordinates");
            }
        } else if (!configured_multiplicities.empty()) {
            error("acceptance_named_region_multiplicity_without_coverage",
                  "matrix.acceptance.exact_named_region_sample_multiplicities",
                  "exact named-region multiplicities require require_named_region_iteration_coverage=true");
        }
    }

    // 3. Walk every rule. Selector validation, rule name uniqueness,
    //    threshold/tolerance always-pass warnings.
    std::set<std::string> seen_rule_names;
    auto validate_selector = [&schemas, &config, &error](const rule_selector& selector,
                                  const metric_specification& metric,
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
        const bool test_case_is_configured = std::any_of(
            config.for_each.begin(), config.for_each.end(),
            [&selector](const auto& entry) noexcept {
                return entry.first == selector.test_case;
            });
        if (!selector.test_case.empty() && !test_case_is_configured) {
            error("unknown_rule_test_case",
                  base_loc + ".test_case",
                  fmt::format("rule selector scopes test_case '{}' which is not present in this benchmark's matrix.for map",
                              selector.test_case));
        }
        if (!selector.region.empty() && selector.region != "all_regions") {
            const bool source_has_named_regions =
                metric.declared_source == metric_source::region
                || metric.declared_source == metric_source::loop;
            if (!source_has_named_regions) {
                error("rule_region_requires_region_metric",
                      base_loc + ".region",
                      fmt::format("rule selector scopes region '{}' but metric '{}' is not a region/loop metric",
                                  selector.region, metric.name));
            } else if (!config.by_regions) {
                error("rule_region_requires_by_regions",
                      base_loc + ".region",
                      fmt::format("rule selector scopes region '{}' but matrix.by_regions is false",
                                  selector.region));
            } else if (!metric.region.empty()
                       && metric.region != selector.region) {
                error("rule_region_conflicts_with_metric_filter",
                      base_loc + ".region",
                      fmt::format("rule selector scopes region '{}' but metric '{}' is collected only for region '{}'",
                                  selector.region, metric.name, metric.region));
            }
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
        if (!metric.applicable_algorithms.empty()) {
            if (metric.kind != metric_kind::evaluation) {
                error("metric_applicability_requires_evaluation",
                      fmt::format("matrix.metrics.{}.applicable_algorithms",
                                  metric.name),
                      "per-row applicability is supported only for evaluation metrics");
            }
            std::set<std::string> seen_applicable_algorithms;
            for (const auto& algorithm : metric.applicable_algorithms) {
                if (!seen_applicable_algorithms.insert(algorithm).second) {
                    error("duplicate_metric_applicable_algorithm",
                          fmt::format("matrix.metrics.{}.applicable_algorithms",
                                      metric.name),
                          fmt::format("metric '{}' repeats applicable algorithm '{}'",
                                      metric.name, algorithm));
                }
                if (!config.matrix_over.contains(algorithm)) {
                    error("unknown_metric_applicable_algorithm",
                          fmt::format("matrix.metrics.{}.applicable_algorithms",
                                      metric.name),
                          fmt::format("metric '{}' names algorithm '{}' outside this benchmark's matrix.over map",
                                      metric.name, algorithm));
                }
            }
        }

        // 4a. Builtin metric name + source compatibility. Counter and
        //     Evaluation metrics carry user-chosen names; Region / Loop /
        //     OuterClock metric keys must match a registry entry whose
        //     source matches the declared `type`.
        // (Counter / Evaluation flavours skip these checks — their metric
        // names are user-chosen and the schema's `additionalProperties:
        // false` already rejects a `region` field on those branches.)
        if (metric.kind != metric_kind::counter && metric.kind != metric_kind::evaluation) {
            const auto* entry = lookup_builtin_metric(metric.name);
            if (entry == nullptr) {
                error("invalid_builtin_metric_name",
                      fmt::format("matrix.metrics.{}", metric.name),
                      fmt::format("metric '{}' is not a recognised framework builtin; either rename it to a registered builtin (e.g. wall_clock_ns, region_total_ns, region_visits, region_p99_ns, loop_total_iterations, loop_p50_per_iter_ns; see the k_builtin_metric_registry table) or declare 'type' as 'counter' / 'evaluation'",
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
            if (entry->combiner == metric_combiner::requires_region && metric.region.empty()) {
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
            validate_selector(rule.lhs, metric, rule.name, metric.name, "lhs");
            validate_selector(rule.rhs, metric, rule.name, metric.name, "rhs");
            if (!rule.lhs.test_case.empty() && !rule.rhs.test_case.empty()
                && rule.lhs.test_case != rule.rhs.test_case) {
                error("rule_test_case_mismatch",
                      fmt::format("matrix.metrics.{}.rules.{}", metric.name, rule.name),
                      fmt::format("rule selectors name different test cases ('{}' vs '{}'); within-run rules compare sources from one test case",
                                  rule.lhs.test_case, rule.rhs.test_case));
            }

            const bool ratio_paired = rule.cmp.pairing
                == rule_comparator::pairing_mode::by_iteration_ratio;
            const bool exact_paired = rule.cmp.pairing
                == rule_comparator::pairing_mode::by_iteration_exact;
            const bool iteration_paired = ratio_paired || exact_paired;
            const std::string comparator_location = fmt::format(
                "matrix.metrics.{}.rules.{}.comparator",
                metric.name, rule.name);
            if (!ratio_paired && rule.cmp.warmup_repetitions != 0) {
                error("warmup_requires_paired_rule",
                      comparator_location + ".warmup_repetitions",
                      "warmup_repetitions is meaningful only when pairing is by_iteration_ratio");
            }
            if (!ratio_paired && rule.cmp.paired_baseline_explicit) {
                error("paired_baseline_requires_paired_rule",
                      comparator_location + ".paired_baseline",
                      "paired_baseline is meaningful only when pairing is by_iteration_ratio");
            }
            if (ratio_paired) {
                if (!std::isfinite(rule.cmp.paired_baseline)
                    || rule.cmp.paired_baseline <= 0.0) {
                    error("paired_baseline_invalid",
                          comparator_location + ".paired_baseline",
                          "paired_baseline must be finite and greater than zero");
                }
                if (!config.concurrency.bench_lock
                    || config.concurrency.concurrency
                    || config.iteration_order
                        != benchmark_configuration::iteration_order_mode::interleaved_seeded) {
                    error("paired_rule_requires_serial_interleaving",
                          comparator_location + ".pairing",
                          "by_iteration_ratio requires bench_lock=true, concurrency=false and iteration_order.mode=interleaved_seeded");
                }

                const bool lhs_named = !rule.lhs.region.empty()
                    && rule.lhs.region != "all_regions";
                const bool rhs_named = !rule.rhs.region.empty()
                    && rule.rhs.region != "all_regions";
                if ((lhs_named || rhs_named)
                    && metric.kind != metric_kind::region_total_ns) {
                    error("paired_named_region_requires_total_ns",
                          comparator_location + ".pairing",
                          "paired named-region rules currently support only region_total_ns, whose duplicate same-name descriptors can be partitioned and reduced exactly per repeat");
                }
                if ((lhs_named || rhs_named)
                    && (!config.acceptance.enabled()
                        || !config.acceptance
                                .require_named_region_iteration_coverage)) {
                    error("paired_named_region_requires_strict_provenance",
                          comparator_location + ".pairing",
                          "paired named-region rules require the strict acceptance contract and per-iteration named-region provenance");
                }

            }
            if (exact_paired) {
                if (rule.cmp.op != comparison_operator::equal
                    || rule.cmp.threshold_pct != 0.0
                    || rule.cmp.tolerance_pct != 0.0) {
                    error("exact_iteration_rule_requires_zero_tolerance_equality",
                          comparator_location,
                          "by_iteration_exact requires compare='=', threshold%=0, and tolerance%=0");
                }
                const bool lhs_named = !rule.lhs.region.empty()
                    && rule.lhs.region != "all_regions";
                const bool rhs_named = !rule.rhs.region.empty()
                    && rule.rhs.region != "all_regions";
                if (metric.kind != metric_kind::evaluation
                    || lhs_named || rhs_named) {
                    error("exact_iteration_rule_requires_all_regions_evaluation",
                          comparator_location + ".pairing",
                          "by_iteration_exact is restricted to all_regions evaluation metrics");
                }
            }
            if (iteration_paired) {
                for (const auto& [test_case_name, iterations] :
                     config.for_each) {
                    if ((!rule.lhs.test_case.empty()
                         && rule.lhs.test_case != test_case_name)
                        || (!rule.rhs.test_case.empty()
                            && rule.rhs.test_case != test_case_name)) {
                        continue;
                    }
                    const auto test_case_it = std::find_if(
                        state.test_cases.begin(), state.test_cases.end(),
                        [&test_case_name](const test_case_specification& spec) {
                            return spec.name == test_case_name;
                        });
                    if (test_case_it == state.test_cases.end()
                        || test_case_it->concurrency != 1) {
                        error("paired_rule_requires_one_group",
                              comparator_location + ".pairing",
                              fmt::format(
                                  "{} requires test case '{}' to declare concurrency=1 so repeat ordinals are globally unique",
                                  ratio_paired
                                      ? "by_iteration_ratio"
                                      : "by_iteration_exact",
                                  test_case_name));
                    }
                    if (ratio_paired
                        && iterations <= rule.cmp.warmup_repetitions) {
                        error("paired_rule_warmup_consumes_all_repeats",
                              comparator_location + ".warmup_repetitions",
                              fmt::format(
                                  "warmup_repetitions={} leaves no paired samples for test case '{}' with {} iterations",
                                  rule.cmp.warmup_repetitions,
                                  test_case_name, iterations));
                    }
                }
            }

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

/// Post-run validation. Checks evaluation-metric emission and, when present,
/// the exact complete-matrix acceptance contract. These checks require the
/// final merged iteration state and therefore cannot run during config parse.
[[nodiscard]] inline std::vector<validation_diagnostic> semantic_validate_post_run(
    const benchmark_execution_state& state) noexcept
{
    std::vector<validation_diagnostic> diagnostics;

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

    if (state.config.acceptance.enabled()) {
        const auto& contract = state.config.acceptance;
        std::map<std::string, std::size_t> tuple_counts;
        for (const auto& tuple : state.execution_tuples) {
            ++tuple_counts[tuple.algorithm_name];
        }
        const std::set<std::string> expected_algorithms{
            contract.exact_algorithm_rows.begin(),
            contract.exact_algorithm_rows.end()};
        std::set<std::string> actual_algorithms;
        for (const auto& [algorithm, count] : tuple_counts) {
            actual_algorithms.insert(algorithm);
            if (count != 1) {
                diagnostics.push_back({
                    validation_diagnostic::diagnostic_severity::error,
                    "acceptance_execution_tuple_multiplicity",
                    semantic_location(state, "matrix.acceptance"),
                    fmt::format("acceptance row '{}' expanded to {} tuples; exactly one is required",
                                algorithm, count),
                });
            }
        }
        if (actual_algorithms != expected_algorithms
            || state.execution_tuples.size()
                != contract.exact_algorithm_rows.size()) {
            diagnostics.push_back({
                validation_diagnostic::diagnostic_severity::error,
                "acceptance_execution_rows_incomplete",
                semantic_location(state, "matrix.acceptance"),
                fmt::format("complete-matrix acceptance requires exactly {} configured rows; {} expanded rows were present",
                            contract.exact_algorithm_rows.size(),
                            state.execution_tuples.size()),
            });
        }

        std::map<std::pair<std::string, std::string>, std::size_t>
            completed_wall_samples;
        std::map<std::pair<std::string, std::string>, bool>
            wall_samples_all_finite;
        std::size_t completed_total = 0;
        for (const auto& cell_ptr : state.cells) {
            const auto key = std::pair{
                cell_ptr->tuple->algorithm_name,
                cell_ptr->test_case->name};
            auto& all_finite = wall_samples_all_finite[key];
            if (!completed_wall_samples.contains(key)) all_finite = true;
            const auto region_it = cell_ptr->samples.find("all_regions");
            if (region_it == cell_ptr->samples.end()) continue;
            const auto wall_it = region_it->second.find("wall_clock_ns");
            if (wall_it == region_it->second.end()) continue;
            completed_wall_samples[key] += wall_it->second.size();
            completed_total += wall_it->second.size();
            all_finite = all_finite
                && std::all_of(wall_it->second.begin(), wall_it->second.end(),
                    [](double value) noexcept { return std::isfinite(value); });
        }

        std::size_t expected_total = 0;
        for (const auto& algorithm : contract.exact_algorithm_rows) {
            for (const auto& [test_case, iterations] :
                 contract.exact_test_case_iterations) {
                expected_total += iterations;
                const auto key = std::pair{algorithm, test_case};
                const std::size_t completed = completed_wall_samples[key];
                const bool all_finite = wall_samples_all_finite.contains(key)
                    && wall_samples_all_finite.at(key);
                if (completed != iterations || !all_finite) {
                    diagnostics.push_back({
                        validation_diagnostic::diagnostic_severity::error,
                        "acceptance_iteration_matrix_incomplete",
                        semantic_location(state, "matrix.acceptance"),
                        fmt::format("acceptance row '{}'/{} completed {} finite wall-clock samples; exactly {} are required",
                                    algorithm, test_case, completed,
                                    iterations),
                    });
                }
            }
        }
        if (completed_total != expected_total) {
            diagnostics.push_back({
                validation_diagnostic::diagnostic_severity::error,
                "acceptance_execution_total_incomplete",
                semantic_location(state, "matrix.acceptance"),
                fmt::format("complete-matrix acceptance requires exactly {} executions; {} completed wall-clock samples were recorded",
                            expected_total, completed_total),
                });
        }


        using named_source_coordinate =
            std::tuple<std::string, std::string, std::string, std::string>;
        std::map<named_source_coordinate, std::size_t> raw_named_sample_counts;

        // Preserve each logical iteration's same-name descriptor count.
        // `repeat` is local to a group, so merging by that numeric ID would
        // collide (every group has a repeat zero). Instead concatenate each
        // cell's already ordered local vector in explicit group-index order;
        // the resulting vector positions are fresh global ordinals for this
        // accepted (tuple, test-case) pair.
        using iteration_multiplicity_pair =
            std::pair<const execution_tuple*, std::string>;
        std::map<iteration_multiplicity_pair, std::vector<const measurement_cell*>>
            multiplicity_cells_by_pair;
        for (const auto& cell_ptr : state.cells) {
            multiplicity_cells_by_pair[
                {cell_ptr->tuple, cell_ptr->test_case->name}]
                    .push_back(cell_ptr.get());
            for (const auto& [region_name, metric_samples] :
                 cell_ptr->samples) {
                if (region_name == "all_regions") continue;
                for (const auto& [metric_name, samples] : metric_samples) {
                    raw_named_sample_counts[named_source_coordinate{
                        cell_ptr->tuple->algorithm_name,
                        cell_ptr->test_case->name,
                        region_name,
                        metric_name}] += samples.size();
                }
            }
        }

        std::map<iteration_multiplicity_pair,
                 std::vector<named_region_iteration_sample_counts>>
            merged_iteration_multiplicities;
        for (auto& [pair_key, pair_cells] : multiplicity_cells_by_pair) {
            std::sort(
                pair_cells.begin(), pair_cells.end(),
                [](const measurement_cell* lhs, const measurement_cell* rhs) noexcept {
                    return lhs->group_index < rhs->group_index;
                });
            auto& merged = merged_iteration_multiplicities[pair_key];
            for (const measurement_cell* cell : pair_cells) {
                merged.insert(
                    merged.end(),
                    cell->named_region_iteration_sample_counts.begin(),
                    cell->named_region_iteration_sample_counts.end());
            }
        }

        std::map<std::string, const execution_tuple*> unique_tuple_by_algorithm;
        std::set<std::string> duplicate_tuple_algorithms;
        for (const auto& tuple : state.execution_tuples) {
            if (!unique_tuple_by_algorithm.emplace(
                    tuple.algorithm_name, &tuple).second) {
                duplicate_tuple_algorithms.insert(tuple.algorithm_name);
            }
        }
        for (const auto& algorithm : duplicate_tuple_algorithms) {
            unique_tuple_by_algorithm.erase(algorithm);
        }

        const auto count_for_iteration = [](
                const named_region_iteration_sample_counts& counts,
                const std::string& region,
                const std::string& metric) noexcept {
            const auto region_it = counts.find(region);
            if (region_it == counts.end()) return std::size_t{0};
            const auto metric_it = region_it->second.find(metric);
            return metric_it == region_it->second.end()
                ? std::size_t{0}
                : metric_it->second;
        };
        for (const auto& multiplicity :
             contract.exact_named_region_sample_multiplicities) {
            const named_source_coordinate coordinate{
                multiplicity.algorithm,
                multiplicity.test_case,
                multiplicity.region,
                multiplicity.metric};
            const std::size_t actual = raw_named_sample_counts[coordinate];
            if (actual != multiplicity.exact_samples) {
                diagnostics.push_back({
                    validation_diagnostic::diagnostic_severity::error,
                    "acceptance_named_region_multiplicity_mismatch",
                    semantic_location(state, "matrix.acceptance"),
                    fmt::format("acceptance source '{}'/{} region '{}' metric '{}' emitted {} raw descriptor samples; exactly {} are required",
                                multiplicity.algorithm,
                                multiplicity.test_case,
                                multiplicity.region,
                                multiplicity.metric,
                                actual,
                                multiplicity.exact_samples),
                });
            }

            const auto iterations_it =
                contract.exact_test_case_iterations.find(
                    multiplicity.test_case);
            const auto tuple_it = unique_tuple_by_algorithm.find(
                multiplicity.algorithm);
            if (iterations_it
                    == contract.exact_test_case_iterations.end()
                || iterations_it->second == 0
                || multiplicity.exact_samples % iterations_it->second != 0
                || tuple_it == unique_tuple_by_algorithm.end()) {
                // Pre-run semantic validation already reports malformed
                // contracts/tuple multiplicity. Avoid manufacturing a second
                // distribution diagnosis from an undefined quotient.
                continue;
            }

            const std::size_t expected_iterations = iterations_it->second;
            const std::size_t expected_per_iteration =
                multiplicity.exact_samples / expected_iterations;
            const auto merged_it = merged_iteration_multiplicities.find(
                {tuple_it->second, multiplicity.test_case});
            static const std::vector<named_region_iteration_sample_counts>
                empty_iteration_counts;
            const auto& merged_counts =
                merged_it == merged_iteration_multiplicities.end()
                    ? empty_iteration_counts
                    : merged_it->second;

            std::vector<std::size_t> distribution;
            distribution.reserve(merged_counts.size());
            bool distribution_matches =
                merged_counts.size() == expected_iterations;
            for (const auto& iteration_counts : merged_counts) {
                const std::size_t count = count_for_iteration(
                    iteration_counts,
                    multiplicity.region,
                    multiplicity.metric);
                distribution.push_back(count);
                distribution_matches = distribution_matches
                    && count == expected_per_iteration;
            }
            if (!distribution_matches) {
                std::string rendered_distribution{"["};
                for (std::size_t index = 0;
                     index < distribution.size(); ++index) {
                    if (index != 0) rendered_distribution += ',';
                    rendered_distribution +=
                        std::to_string(distribution[index]);
                }
                rendered_distribution += ']';
                diagnostics.push_back({
                    validation_diagnostic::diagnostic_severity::error,
                    "acceptance_named_region_iteration_multiplicity_mismatch",
                    semantic_location(state, "matrix.acceptance"),
                    fmt::format("acceptance source '{}'/{} region '{}' metric '{}' emitted per-iteration raw descriptor distribution {}; exactly {} samples in each of {} iterations are required",
                                multiplicity.algorithm,
                                multiplicity.test_case,
                                multiplicity.region,
                                multiplicity.metric,
                                rendered_distribution,
                                expected_per_iteration,
                                expected_iterations),
                });
            }
        }
    }

    if (!any_iteration_ran) {
        diagnostics.push_back({
            state.config.acceptance.enabled()
                ? validation_diagnostic::diagnostic_severity::error
                : validation_diagnostic::diagnostic_severity::warning,
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
        if (metric.kind == metric_kind::counter || metric.kind == metric_kind::evaluation) continue;
        const auto* entry = lookup_builtin_metric(metric.name);
        if (entry == nullptr || !entry->needs_pmu) continue;
        if (metric_was_emitted(metric.name) && !metric_has_non_zero_sample(metric.name)) {
            if (!pmu_warning_emitted) {
                diagnostics.push_back({
                    validation_diagnostic::diagnostic_severity::warning,
                    "pmu_unavailable",
                    semantic_location(state, "matrix.metrics"),
                    "PMU counters were unavailable on this platform — every sample of region_total_cycles / region_total_instructions / region_total_branches / region_total_branch_misses / region_ipc / region_branch_miss_rate read as zero. On macOS arm64 the kpc interface requires entitlements; on Linux the perf_event_open syscall may be sandbox-restricted. Re-run with the appropriate privileges or remove the PMU-derived metrics from this benchmark."
                });
                pmu_warning_emitted = true;
            }
        }
    }

    for (const auto& metric : state.config.metrics) {

        if (metric.kind == metric_kind::evaluation) {
            if (metric.applicable_algorithms.empty()) {
                if (!metric_was_emitted(metric.name)) {
                    diagnostics.push_back({
                        validation_diagnostic::diagnostic_severity::error,
                        "missing_evaluation_metric",
                        semantic_location(state,
                            fmt::format("matrix.metrics.{}", metric.name)),
                        fmt::format("metric '{}' is declared as 'type: evaluation' but the on_evaluate handler never returned this key for any iteration",
                                    metric.name)
                    });
                }
                continue;
            }

            for (const auto& cell_ptr : state.cells) {
                if (std::find(metric.applicable_algorithms.begin(),
                              metric.applicable_algorithms.end(),
                              cell_ptr->tuple->algorithm_name)
                        == metric.applicable_algorithms.end()) {
                    continue;
                }
                std::size_t accepted_iterations = 0;
                std::size_t emitted_for_accepted_iterations = 0;
                for (const auto& completion : cell_ptr->iteration_evidence) {
                    if (!completion.completed
                        || !completion.framework_timing_eligible) {
                        continue;
                    }
                    ++accepted_iterations;
                    if (completion.emitted_evaluation_metrics.contains(
                            metric.name)) {
                        ++emitted_for_accepted_iterations;
                    }
                }
                if (accepted_iterations == 0
                    || emitted_for_accepted_iterations
                        == accepted_iterations) {
                    continue;
                }
                diagnostics.push_back({
                    validation_diagnostic::diagnostic_severity::error,
                    "missing_evaluation_metric_for_accepted_cell",
                    semantic_location(state,
                        fmt::format("matrix.metrics.{}", metric.name)),
                    fmt::format("metric '{}' applies to accepted row '{}'/{} but on_evaluate emitted it for only {} of {} accepted iterations",
                                metric.name,
                                cell_ptr->tuple->algorithm_name,
                                cell_ptr->test_case->name,
                                emitted_for_accepted_iterations,
                                accepted_iterations)
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
                validation_diagnostic::diagnostic_severity::warning,
                "region_filter_never_matched",
                semantic_location(state,
                    fmt::format("matrix.metrics.{}", metric.name)),
                fmt::format("metric '{}' restricts to region '{}', but no descriptor with that name was visited during the benchmark; rules over this metric will see an empty sample array",
                            metric.name, metric.region)
            });
        }
    }

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
///   - benchmark_rule names must be unique within the same config file.
///   - Every entry of `benchmarks` (when present) must name a benchmark
///     that is registered for this config_path.
///   - Every benchmark/algorithm deny-list entry must name a declaration in
///     this config and cannot remove every cell the rule could cover.
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
[[nodiscard]] inline std::vector<validation_diagnostic> semantic_validate_regression_rules(
    const regression_rules_by_configuration& regression_rules_by_config,
    const regression_validation_universe& validation_universe) noexcept
{
    std::vector<validation_diagnostic> diagnostics;
    auto error = [&diagnostics](
        std::string code, std::string location, std::string message) {
        diagnostics.push_back({
            validation_diagnostic::diagnostic_severity::error,
            std::move(code), std::move(location), std::move(message)
        });
    };

    for (const auto& [config_path, rules] : regression_rules_by_config) {
        std::vector<const regression_benchmark_declaration*>
            declarations_for_config;
        if (const auto universe_it = validation_universe.find(config_path);
            universe_it != validation_universe.end()) {
            declarations_for_config.reserve(universe_it->second.size());
            for (const auto& declaration : universe_it->second) {
                declarations_for_config.push_back(&declaration);
            }
        }

        std::set<std::string> seen_rule_names;
        for (const auto& rule : rules) {
            const std::string base_loc = fmt::format(
                "{}/regression_rules/{}", config_path, rule.name);

            // 1. benchmark_rule name uniqueness within the config file.
            if (!seen_rule_names.insert(rule.name).second) {
                error("regression_duplicate_rule_name", base_loc,
                    fmt::format("regression rule name '{}' is used more than once in '{}'; names must be unique within a config file",
                                rule.name, config_path));
            }

            // 2. Determine matching benchmarks within this config_path.
            std::vector<const regression_benchmark_declaration*>
                matching_declarations;
            const std::set<std::string> excluded_benchmarks(
                rule.exclude_benchmarks.begin(), rule.exclude_benchmarks.end());
            for (const auto& excluded_bench : excluded_benchmarks) {
                const bool found = std::any_of(
                    declarations_for_config.begin(),
                    declarations_for_config.end(),
                    [&excluded_bench](const auto* declaration) {
                        return declaration->benchmark_key == excluded_bench;
                    });
                if (!found) {
                    error("regression_unknown_benchmark", base_loc,
                        fmt::format("exclude_benchmarks entry '{}' is not declared in '{}'",
                                    excluded_bench, config_path));
                }
            }
            const std::set<std::string> included_benchmarks(
                rule.benchmarks.begin(), rule.benchmarks.end());
            for (const auto& declared_bench : included_benchmarks) {
                const bool found = std::any_of(
                    declarations_for_config.begin(),
                    declarations_for_config.end(),
                    [&declared_bench](const auto* declaration) {
                        return declaration->benchmark_key == declared_bench;
                    });
                if (!found) {
                    error("regression_unknown_benchmark", base_loc,
                        fmt::format("benchmark '{}' is not declared in '{}'",
                                    declared_bench, config_path));
                }
            }
            for (const auto* declaration : declarations_for_config) {
                const bool included = included_benchmarks.empty()
                    || included_benchmarks.contains(declaration->benchmark_key);
                if (included
                    && !excluded_benchmarks.contains(
                        declaration->benchmark_key)) {
                    matching_declarations.push_back(declaration);
                }
            }
            if (matching_declarations.empty()
                && !declarations_for_config.empty()) {
                error("regression_excludes_all_benchmarks", base_loc,
                    fmt::format("regression rule '{}' excludes every matching benchmark; the rule can never fire",
                                rule.name));
            }

            // 3. Metric existence check.
            const auto metric_in_declaration = [&rule](
                    const regression_benchmark_declaration& declaration) noexcept {
                return std::find(
                    declaration.metrics.begin(), declaration.metrics.end(),
                    rule.metric) != declaration.metrics.end();
            };
            if (!rule.benchmarks.empty()) {
                for (const auto* declaration : matching_declarations) {
                    if (!metric_in_declaration(*declaration)) {
                        error("regression_metric_missing_in_benchmark", base_loc,
                            fmt::format("regression rule '{}' targets metric '{}' but benchmark '{}' (explicitly listed in 'benchmarks') does not declare it under matrix.metrics",
                                        rule.name, rule.metric,
                                        declaration->benchmark_key));
                    }
                }
            } else if (!matching_declarations.empty()) {
                bool found_any = false;
                for (const auto* declaration : matching_declarations) {
                    if (metric_in_declaration(*declaration)) {
                        found_any = true;
                        break;
                    }
                }
                if (!found_any) {
                    error("regression_metric_unused", base_loc,
                        fmt::format("regression rule '{}' (no 'benchmarks' filter) targets metric '{}' but no benchmark registered from '{}' declares it",
                                    rule.name, rule.metric, config_path));
                }
            }

            // 4. exclude_cases membership check (and over-coverage).
            //    Test-case declarations are config-global; use the registered
            //    unfiltered universe so a CLI filter cannot turn a valid name
            //    into a false unknown-reference error.
            if (!matching_declarations.empty()) {
                std::set<std::string> declared_names;
                for (const auto* declaration : declarations_for_config) {
                    declared_names.insert(
                        declaration->config_test_cases.begin(),
                        declaration->config_test_cases.end());
                }
                for (const auto& excluded : rule.exclude_cases) {
                    if (!declared_names.contains(excluded)) {
                        error("regression_unknown_test_case", base_loc,
                            fmt::format("exclude_cases entry '{}' is not declared in 'test_cases'",
                                        excluded));
                    }
                }

                // Over-coverage: union of every for_each in matching
                // benchmarks minus exclude_cases must be non-empty.
                std::set<std::string> covered_test_cases;
                for (const auto* declaration : matching_declarations) {
                    covered_test_cases.insert(
                        declaration->matrix_test_cases.begin(),
                        declaration->matrix_test_cases.end());
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

                std::set<std::string> declared_algorithms;
                for (const auto* declaration : declarations_for_config) {
                    declared_algorithms.insert(
                        declaration->config_algorithms.begin(),
                        declaration->config_algorithms.end());
                }
                const std::set<std::string> excluded_algorithms(
                    rule.exclude_algorithms.begin(),
                    rule.exclude_algorithms.end());
                for (const auto& excluded : excluded_algorithms) {
                    if (!declared_algorithms.contains(excluded)) {
                        error("regression_unknown_algorithm", base_loc,
                            fmt::format("exclude_algorithms entry '{}' is not declared in 'params'",
                                        excluded));
                    }
                }
                bool any_algorithm_remains = false;
                for (const auto* declaration : matching_declarations) {
                    for (const auto& algorithm_name :
                         declaration->matrix_algorithms) {
                        if (!excluded_algorithms.contains(
                                algorithm_name)) {
                            any_algorithm_remains = true;
                            break;
                        }
                    }
                    if (any_algorithm_remains) break;
                }
                if (!any_algorithm_remains) {
                    error("regression_excludes_all_algorithms", base_loc,
                        fmt::format("regression rule '{}' excludes every algorithm in its matching benchmarks; the rule can never fire",
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
[[nodiscard]] inline std::vector<execution_tuple> expand_one_algorithm(
    const std::string& algorithm_name,
    const std::map<std::string, std::vector<config_value>>& declared,
    const algorithm_schema& schema,
    bool& expansion_ok) noexcept
{
    std::vector<execution_tuple> seed;
    seed.push_back(execution_tuple{algorithm_name, parameter_map{}});

    // Cross-product over declared parameters in iteration order.
    for (const auto& [param_name, value_list] : declared) {
        if (value_list.empty()) {
            bioseq_log<logging_level::error>("[bench_framework] matrix.over.{}.{} has no values", algorithm_name, param_name);
            expansion_ok = false;
            continue;
        }
        std::vector<execution_tuple> next_round;
        next_round.reserve(seed.size() * value_list.size());
        for (const auto& accumulated_tuple : seed) {
            for (const auto& candidate_value : value_list) {
                execution_tuple expanded = accumulated_tuple;
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
            bioseq_log<logging_level::error>("[bench_framework] {}.{} not declared in matrix.over and has no default",
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
[[nodiscard]] inline std::vector<execution_tuple> expand_matrix(
    const benchmark_configuration& config,
    bool& expansion_ok) noexcept
{
    std::vector<execution_tuple> result;
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

// ----------------- measurement_cell construction -----------------

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
    const benchmark_configuration& config,
    const test_case_specification&    test_case_spec,
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
[[nodiscard]] inline std::vector<std::unique_ptr<measurement_cell>> build_cells(
    const benchmark_execution_state& state)
{
    std::vector<std::unique_ptr<measurement_cell>> cells;
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
            bioseq_log<logging_level::error>("[bench_framework] test_case '{}' referenced by matrix.for but not declared",
                         test_case_name);
            continue;
        }
        const std::size_t test_case_index = found_it->second;
        const auto& test_case_spec = state.test_cases[test_case_index];
        const std::size_t group_count = group_count_for_test_case(
            state.config, test_case_spec, iterations);

        if (group_count == 0) {
            // Zero-iteration test case — emit no cells so the writer's
            // behaviour, which also produced no samples for these).
            continue;
        }

        const auto split = split_iterations(iterations, group_count);
        for (std::size_t tuple_index = 0;
             tuple_index < state.execution_tuples.size(); ++tuple_index) {
            const auto& tuple = state.execution_tuples[tuple_index];
            for (std::size_t group_index = 0; group_index < split.size(); ++group_index) {
                auto cell = std::make_unique<measurement_cell>();
                cell->test_case        = &state.test_cases[test_case_index];
                cell->test_case_index  = test_case_index;
                cell->tuple            = &tuple;
                cell->iterations       = split[group_index];
                cell->group_index      = group_index;
                cell->group_count      = split.size();
                cell->tuple_index      = tuple_index;
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
    metric_kind kind,
    const bioseq::instrumentation::region_aggregate& region_agg) noexcept
{
    using bioseq::instrumentation::summary::estimate_percentile;
    switch (kind) {
        case metric_kind::region_total_ns:           return static_cast<double>(region_agg.total_ns);
        case metric_kind::region_visits:            return static_cast<double>(region_agg.visits);
        case metric_kind::region_min_ns:             return static_cast<double>(region_agg.min_ns);
        case metric_kind::region_max_ns:             return static_cast<double>(region_agg.max_ns);
        case metric_kind::region_p50_ns:             return static_cast<double>(estimate_percentile(region_agg.duration_distribution, 0.50));
        case metric_kind::region_p99_ns:             return static_cast<double>(estimate_percentile(region_agg.duration_distribution, 0.99));
        case metric_kind::region_total_cycles:       return static_cast<double>(region_agg.total_cycles);
        case metric_kind::region_total_instructions: return static_cast<double>(region_agg.total_instructions);
        case metric_kind::region_total_branches:     return static_cast<double>(region_agg.total_branches);
        case metric_kind::region_total_branch_misses: return static_cast<double>(region_agg.total_branch_misses);
        default: return 0.0;     // ratios/loop-only metrics use the
                                  // accumulator below, never this scalar
                                  // path.
    }
}

[[nodiscard]] inline double extract_from_loop(
    metric_kind kind,
    const bioseq::instrumentation::loop_aggregate& loop_agg) noexcept
{
    using bioseq::instrumentation::summary::estimate_percentile;
    switch (kind) {
        case metric_kind::loop_total_iterations: return static_cast<double>(loop_agg.total_iterations);
        case metric_kind::loop_total_work:       return static_cast<double>(loop_agg.total_work);
        case metric_kind::loop_empty_visits:     return static_cast<double>(loop_agg.empty_visits);
        case metric_kind::loop_min_per_iteration_ns:    return static_cast<double>(loop_agg.min_per_iter_ns);
        case metric_kind::loop_max_per_iteration_ns:    return static_cast<double>(loop_agg.max_per_iter_ns);
        case metric_kind::loop_p50_per_iteration_ns:    return static_cast<double>(estimate_percentile(loop_agg.per_iteration_duration_distribution, 0.50));
        case metric_kind::loop_p99_per_iteration_ns:    return static_cast<double>(estimate_percentile(loop_agg.per_iteration_duration_distribution, 0.99));
        default: return 0.0;
    }
}

/// Per-descriptor ratio extraction. Used by the by_regions emit site so
/// per-region buckets carry the actual ratio for that one descriptor
/// rather than a literal zero from the scalar `extract_from_*` switch's
/// default arm. Returns nullopt when the denominator is zero (the per-
/// region sample is then suppressed).
[[nodiscard]] inline std::optional<double> ratio_per_region(
    metric_kind kind,
    const bioseq::instrumentation::region_aggregate& region_agg) noexcept
{
    switch (kind) {
        case metric_kind::region_ipc:
            if (region_agg.total_cycles == 0) return std::nullopt;
            return static_cast<double>(region_agg.total_instructions)
                 / static_cast<double>(region_agg.total_cycles);
        case metric_kind::region_branch_miss_rate:
            if (region_agg.total_branches == 0) return std::nullopt;
            return static_cast<double>(region_agg.total_branch_misses)
                 / static_cast<double>(region_agg.total_branches);
        default:
            return std::nullopt;     // not a region-ratio metric
    }
}

[[nodiscard]] inline std::optional<double> ratio_per_loop(
    metric_kind kind,
    const bioseq::instrumentation::loop_aggregate& loop_agg) noexcept
{
    switch (kind) {
        case metric_kind::loop_mean_per_iteration_ns:
            if (loop_agg.total_iterations == 0) return std::nullopt;
            return static_cast<double>(loop_agg.total_ns)
                 / static_cast<double>(loop_agg.total_iterations);
        case metric_kind::loop_work_per_iteration:
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
struct cross_descriptor_accumulator {
    double                       sum = 0.0;
    std::uint64_t                ratio_numerator = 0;
    std::uint64_t                ratio_denominator = 0;
    std::array<std::uint64_t, 32> hist_sum{};   // for HistogramSum combiner
    bool                          any_contribution = false;
};

inline void combine_region(
    cross_descriptor_accumulator& accumulator,
    metric_combiner combiner,
    metric_kind kind,
    const bioseq::instrumentation::region_aggregate& region_agg) noexcept
{
    accumulator.any_contribution = true;
    switch (combiner) {
        case metric_combiner::sum:
            accumulator.sum += extract_from_region(kind, region_agg);
            return;
        case metric_combiner::histogram_sum: {
            const auto& source_hist = region_agg.duration_distribution.buckets;
            for (std::size_t i = 0; i < source_hist.size(); ++i) {
                accumulator.hist_sum[i] += source_hist[i];
            }
            return;
        }
        case metric_combiner::pmu_ratio:
            accumulator.ratio_numerator   += region_agg.total_instructions;
            accumulator.ratio_denominator += region_agg.total_cycles;
            return;
        case metric_combiner::branch_miss_ratio:
            accumulator.ratio_numerator   += region_agg.total_branch_misses;
            accumulator.ratio_denominator += region_agg.total_branches;
            return;
        case metric_combiner::requires_region:
            // With explicit `region` filter, only one descriptor
            // contributes — pass the value straight through.
            accumulator.sum = extract_from_region(kind, region_agg);
            return;
        case metric_combiner::loop_mean_ns:
        case metric_combiner::loop_work_per_iteration:
            return;     // not applicable to region-source metrics.
    }
}

inline void combine_loop(
    cross_descriptor_accumulator& accumulator,
    metric_combiner combiner,
    metric_kind kind,
    const bioseq::instrumentation::loop_aggregate& loop_agg) noexcept
{
    accumulator.any_contribution = true;
    switch (combiner) {
        case metric_combiner::sum:
            accumulator.sum += extract_from_loop(kind, loop_agg);
            return;
        case metric_combiner::histogram_sum: {
            const auto& source_hist = loop_agg.per_iteration_duration_distribution.buckets;
            for (std::size_t i = 0; i < source_hist.size(); ++i) {
                accumulator.hist_sum[i] += source_hist[i];
            }
            return;
        }
        case metric_combiner::loop_mean_ns:
            accumulator.ratio_numerator   += loop_agg.total_ns;
            accumulator.ratio_denominator += loop_agg.total_iterations;
            return;
        case metric_combiner::loop_work_per_iteration:
            accumulator.ratio_numerator   += loop_agg.total_work;
            accumulator.ratio_denominator += loop_agg.total_iterations;
            return;
        case metric_combiner::requires_region:
            accumulator.sum = extract_from_loop(kind, loop_agg);
            return;
        case metric_combiner::pmu_ratio:
        case metric_combiner::branch_miss_ratio:
            return;     // loop metrics never use PMU ratios directly.
    }
}

/// Finalise the accumulator into a single double, or `nullopt` when the
/// metric should be skipped this iteration (zero-denominator ratios, no
/// contributions, etc.).
[[nodiscard]] inline std::optional<double> finalise_combined_metric(
    const cross_descriptor_accumulator& accumulator,
    metric_combiner combiner,
    double percentile) noexcept
{
    if (!accumulator.any_contribution) return std::nullopt;
    switch (combiner) {
        case metric_combiner::sum:
        case metric_combiner::requires_region:
            return accumulator.sum;
        case metric_combiner::histogram_sum: {
            // Build a temporary duration_histogram view over the summed buckets
            // and run the public percentile estimator.
            bioseq::instrumentation::duration_histogram summed{};
            summed.buckets = accumulator.hist_sum;
            return static_cast<double>(
                bioseq::instrumentation::summary::estimate_percentile(summed, percentile));
        }
        case metric_combiner::pmu_ratio:
        case metric_combiner::branch_miss_ratio:
        case metric_combiner::loop_mean_ns:
        case metric_combiner::loop_work_per_iteration:
            if (accumulator.ratio_denominator == 0) return std::nullopt;
            return static_cast<double>(accumulator.ratio_numerator)
                 / static_cast<double>(accumulator.ratio_denominator);
    }
    return std::nullopt;
}

/// Write one iteration's measurements into the cell's metric arrays:
/// - iteration-level outer `wall_clock_ns` lands in
///   `all_regions["wall_clock_ns"]` only for timing-eligible iterations.
/// - region/loop builtin metrics walk every visited descriptor, fold per
///   descriptor through the metric's combiner, then push the finalised
///   value into `all_regions[<metric_name>]`. When `by_regions: true`,
///   each visited descriptor's per-descriptor value also lands in
///   `<descriptor_name>[<metric_name>]`.
/// - counters use the snapshotted before/after deltas.
/// - evaluation metrics read the user's returned map.
/// against a unique `measurement_cell*`, so `samples` has a single writer.
inline void append_iteration_samples(
    measurement_cell& cell,
    std::size_t repeat,
    bool framework_timing_eligible,
    bool by_regions,
    bool track_named_region_iteration_sample_counts,
    bioseq::instrumentation::profiler_state<profiler_configuration>& profiler_block,
    double iteration_wall_clock_ns,
    const std::vector<metric_specification>& metrics,
    const std::vector<std::int64_t>& counters_before,
    const std::vector<std::int64_t>& counters_after,
    const std::map<std::string, double>& evaluation_metrics) noexcept
{
    if (cell.iteration_evidence.size() != cell.iterations) {
        cell.iteration_evidence.resize(cell.iterations);
    }
    DEBUG_ASSERT(repeat < cell.iteration_evidence.size(),
        "logical repeat must fit the measurement cell evidence vector");
    if (repeat >= cell.iteration_evidence.size()) return;
    auto& completion = cell.iteration_evidence[repeat];
    DEBUG_ASSERT(!completion.completed,
        "one measurement cell may complete each logical repeat only once");
    completion.completed = true;
    completion.framework_timing_eligible = framework_timing_eligible;
    for (const auto& [metric_name, _value] : evaluation_metrics) {
        completion.emitted_evaluation_metrics.insert(metric_name);
    }

    auto& all_regions = cell.samples["all_regions"];
    if (framework_timing_eligible) {
        all_regions["wall_clock_ns"].push_back(iteration_wall_clock_ns);
    }

    named_region_iteration_sample_counts* iteration_named_sample_counts = nullptr;
    if (track_named_region_iteration_sample_counts
        && framework_timing_eligible) {
        cell.named_region_iteration_sample_counts.emplace_back();
        iteration_named_sample_counts =
            &cell.named_region_iteration_sample_counts.back();
    }

    const auto descriptor_total = bioseq::instrumentation::registered_descriptor_count();
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
            case metric_kind::wall_clock_ns:
                // Already pushed above.
                continue;
            case metric_kind::counter: {
                const auto delta = counters_after[counter_index]
                                  - counters_before[counter_index];
                all_regions[metric.name].push_back(static_cast<double>(delta));
                ++counter_index;
                continue;
            }
            case metric_kind::evaluation: {
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

        // E2E capability/correctness rejections remain visible through raw
        // records and evaluation diagnostics, but they are not performance
        // samples. Suppress every framework-derived region/loop number for
        // those repeats. Ordinary benchmarks always pass true here.
        if (!framework_timing_eligible) continue;

        const auto* entry = lookup_builtin_metric(metric.name);
        if (entry == nullptr) continue;     // already-flagged by semantic validation

        cross_descriptor_accumulator accumulator;
        const bool restrict_to_one_region = !metric.region.empty();
        std::set<std::string> named_regions_contributed;
        const auto append_named_region_sample =
            [&cell, &metric, &named_regions_contributed,
             iteration_named_sample_counts](const char* region_name,
                                             double value) {
                cell.samples[region_name][metric.name].push_back(value);
                named_regions_contributed.emplace(region_name);
                if (iteration_named_sample_counts != nullptr) {
                    ++(*iteration_named_sample_counts)[region_name][metric.name];
                }
            };

        // Walk every visited descriptor whose name and kind match the
        // metric's source family.
        for (std::uint32_t descriptor_id = 1; descriptor_id <= descriptor_total; ++descriptor_id) {
            const auto* descriptor = bioseq::instrumentation::find_descriptor(descriptor_id);
            if (descriptor == nullptr || descriptor->name == nullptr) continue;
            if (restrict_to_one_region
                && std::string_view{descriptor->name} != std::string_view{metric.region}) continue;

            if (entry->source == metric_source::region) {
                const auto* region_agg = profiler_block.aggregates.find(descriptor_id);
                if (region_agg == nullptr || region_agg->visits == 0) continue;
                combine_region(accumulator, entry->combiner, entry->kind, *region_agg);
                if (by_regions) {
                    // Ratio metrics need a per-descriptor numerator/
                    // denominator computation; the scalar `extract_*` path
                    // only knows how to dump a single field, so it would
                    // emit literal 0 for IPC / branch_miss_rate.
                    if (entry->combiner == metric_combiner::pmu_ratio
                     || entry->combiner == metric_combiner::branch_miss_ratio) {
                        if (auto ratio = ratio_per_region(entry->kind, *region_agg);
                            ratio.has_value()) {
                            append_named_region_sample(descriptor->name, *ratio);
                        }
                    } else {
                        append_named_region_sample(
                            descriptor->name,
                            extract_from_region(entry->kind, *region_agg));
                    }
                }
            } else if (entry->source == metric_source::loop) {
                const auto* loop_agg = profiler_block.loop_aggregates.find(descriptor_id);
                if (loop_agg == nullptr || loop_agg->visits == 0) continue;
                combine_loop(accumulator, entry->combiner, entry->kind, *loop_agg);
                if (by_regions) {
                    if (entry->combiner == metric_combiner::loop_mean_ns
                     || entry->combiner == metric_combiner::loop_work_per_iteration) {
                        if (auto ratio = ratio_per_loop(entry->kind, *loop_agg);
                            ratio.has_value()) {
                            append_named_region_sample(descriptor->name, *ratio);
                        }
                    } else {
                        append_named_region_sample(
                            descriptor->name,
                            extract_from_loop(entry->kind, *loop_agg));
                    }
                }
            }
        }

        for (const auto& region_name : named_regions_contributed) {
            ++cell.named_region_iteration_contributions[region_name][metric.name];
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

/// Look up `bioseq::instrumentation::named_counter*` for every metric whose kind is
/// `Counter`. Called once before the iteration loop because the underlying
/// `counter()` lookup is a cold-path mutex acquisition; we never want to
/// pay it per iteration.
[[nodiscard]] inline std::vector<bioseq::instrumentation::named_counter*> resolve_counter_handles(
    const std::vector<metric_specification>& metrics) noexcept
{
    std::vector<bioseq::instrumentation::named_counter*> handles;
    handles.reserve(metrics.size());
    for (const auto& metric : metrics) {
        if (metric.kind != metric_kind::counter) continue;
        handles.push_back(&bioseq::instrumentation::get_named_counter(metric.counter_name));
    }
    return handles;
}

/// Read the current value of every resolved counter. Returns a vector
/// aligned with `handles` so the call site can do `after - before`
/// per-index. Counter reads are relaxed atomic loads.
[[nodiscard]] inline std::vector<std::int64_t> snapshot_counters(
    const std::vector<bioseq::instrumentation::named_counter*>& handles) noexcept
{
    std::vector<std::int64_t> values;
    values.reserve(handles.size());
    for (const auto* handle : handles) {
        values.push_back(bioseq::instrumentation::current_thread_counter_value(*handle));
    }
    return values;
}

// ----------------- work_batch + scheduler -----------------

/// One entry inside a batch — one tuple's iteration loop bound to the
/// batch's cached `Input`.
struct work_batch_item {
    measurement_cell* cell = nullptr;        // points back into state.cells
    /// Cached strong reference to the underlying tuple. Saves a hop in
    /// the UI label updater on the cold path.
    const execution_tuple* tuple = nullptr;
};

/// One scheduling unit: every item in the batch shares one cached
/// `Input` instance built from the same `(test_case, group)`. Items
/// run sequentially within the batch; batches run in parallel.
struct work_batch {
    std::size_t test_case_index = 0;
    std::size_t group_index     = 0;
    std::size_t group_count     = 1;
    /// Sequence number of this batch in `state.cells`-order. Used as a
    /// stable index into the per-batch slot vector by the scheduler.
    std::size_t batch_index     = 0;
    std::vector<work_batch_item> items{};
};

/// Walk `state.cells` once and group entries by `(test_case_index,
/// group_index)`. The result is one batch per group; items inside each
/// batch are tuples in `state.execution_tuples`-declared order.
[[nodiscard]] inline std::vector<work_batch> build_work_batches(
    benchmark_execution_state& state)
{
    // Use an ordered map keyed by the (tc_index, group_index) pair so
    // batches come out in a deterministic order. The driver later treats
    // `batches[i].batch_index` as i (assigned below in a final pass).
    std::map<std::pair<std::size_t, std::size_t>, work_batch> by_key;
    for (auto& cell_ptr : state.cells) {
        const auto key = std::pair{cell_ptr->test_case_index, cell_ptr->group_index};
        auto& batch = by_key[key];
        if (batch.items.empty()) {
            batch.test_case_index = cell_ptr->test_case_index;
            batch.group_index     = cell_ptr->group_index;
            batch.group_count     = cell_ptr->group_count;
        }
        batch.items.push_back(work_batch_item{ cell_ptr.get(), cell_ptr->tuple });
    }

    std::vector<work_batch> batches;
    batches.reserve(by_key.size());
    for (auto& [key, batch] : by_key) {
        (void)key;
        batch.batch_index = batches.size();
        batches.push_back(std::move(batch));
    }
    return batches;
}

// ----------------- Progress helpers (single source of writes) -----------------

/// Lazily resolve the `progress_test_case` for the current benchmark
/// closure. Returns nullptr when the progress dashboard wasn't
/// installed, the per-benchmark slot hasn't completed setup yet, or
/// any index is out of bounds. Centralised so the five `progress_*`
/// helpers don't repeat the same five-line guard chain.
[[nodiscard]] inline progress_test_case* progress_test_case_for(
    const benchmark_context& context, std::size_t test_case_index) noexcept
{
    if (context.progress == nullptr) return nullptr;
    if (context.benchmark_index >= context.progress->per_benchmark.size())
        return nullptr;
    auto& per = *context.progress->per_benchmark[context.benchmark_index];
    if (!per.setup_done.load(std::memory_order_acquire)) return nullptr;
    if (test_case_index >= per.test_cases.size()) return nullptr;
    return per.test_cases[test_case_index].get();
}

/// Lazily resolve the `progress_group` for the current benchmark
/// closure. Returns nullptr when no test case or group exists at the
/// supplied indices.
[[nodiscard]] inline progress_group* progress_group_for(
    const benchmark_context& context,
    std::size_t test_case_index, std::size_t group_index) noexcept
{
    auto* tc = progress_test_case_for(context, test_case_index);
    if (tc == nullptr) return nullptr;
    if (group_index >= tc->groups.size()) return nullptr;
    return tc->groups[group_index].get();
}

inline void progress_mark_test_case_start(
    benchmark_context& context, const work_batch& batch) noexcept
{
    auto* tc = progress_test_case_for(context, batch.test_case_index);
    if (tc == nullptr) return;
    std::int64_t expected = 0;
    tc->start_ns.compare_exchange_strong(
        expected, bioseq::instrumentation::clock::now_ns(),
        std::memory_order_relaxed, std::memory_order_relaxed);
    if (batch.group_index < tc->groups.size()) {
        tc->groups[batch.group_index]->active.store(true, std::memory_order_relaxed);
    }
}

inline void progress_update_label(
    benchmark_context& context, const work_batch& batch,
    const work_batch_item& item, std::string_view benchmark_key) noexcept
{
    auto* group = progress_group_for(context, batch.test_case_index, batch.group_index);
    if (group == nullptr) return;
    auto label = fmt::format("algo={}, bench={}",
                             item.tuple->algorithm_name, benchmark_key);
    std::lock_guard<std::mutex> guard(group->label_mu);
    group->label = std::move(label);
}

inline void progress_tick_iteration(
    benchmark_context& context, const measurement_cell& cell) noexcept
{
    auto* group = progress_group_for(context, cell.test_case_index, cell.group_index);
    if (group == nullptr) return;
    group->done.fetch_add(1, std::memory_order_relaxed);
}

inline void progress_tick_item_done(
    benchmark_context& context, const work_batch& batch) noexcept
{
    auto* tc = progress_test_case_for(context, batch.test_case_index);
    if (tc == nullptr) return;
    tc->tuple_done.fetch_add(1, std::memory_order_relaxed);
}

inline void progress_mark_group_done(
    benchmark_context& context, const work_batch& batch) noexcept
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
            expected, bioseq::instrumentation::clock::now_ns(),
            std::memory_order_relaxed, std::memory_order_relaxed);
    }
}

//
// All of these follow the same nullptr-safe lookup pattern through
// `progress_group_for`. Failure-string fields are guarded by
// `progress_group::failure_mu`; the renderer only consults them when the
// matching atomic counter is non-zero.

/// Retry-failure category; templated over the "kind" so input and
/// cleanup paths share one body. Members map onto matching progress_group
/// fields.
enum class retry_kind : std::uint8_t { input, cleanup };

inline void progress_record_retry_failure(
    benchmark_context& context, const work_batch& batch,
    retry_kind kind, std::uint8_t attempt,
    std::string_view error_message,
    std::string_view stack_trace) noexcept
{
    auto* group = progress_group_for(context,
                                      batch.test_case_index, batch.group_index);
    if (group == nullptr) return;
    {
        std::lock_guard<std::mutex> guard(group->failure_mu);
        if (kind == retry_kind::input) {
            group->input_failure_message = std::string{error_message};
            group->input_failure_trace   = std::string{stack_trace};
        } else {
            group->cleanup_failure_message = std::string{error_message};
            group->cleanup_failure_trace   = std::string{stack_trace};
        }
    }
    auto& counter = (kind == retry_kind::input)
                       ? group->input_retry_attempt
                       : group->cleanup_retry_attempt;
    counter.store(attempt, std::memory_order_release);
}

inline void progress_clear_retry_failure(
    benchmark_context& context, const work_batch& batch,
    retry_kind kind) noexcept
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
    auto& counter = (kind == retry_kind::input)
                       ? group->input_retry_attempt
                       : group->cleanup_retry_attempt;
    counter.store(0, std::memory_order_release);
    std::lock_guard<std::mutex> guard(group->failure_mu);
    if (kind == retry_kind::input) {
        group->input_failure_message.clear();
        group->input_failure_trace.clear();
    } else {
        group->cleanup_failure_message.clear();
        group->cleanup_failure_trace.clear();
    }
}

/// Mark a batch as aborted using bare indices. Shared between the
/// timeout path (the latter has no live `work_batch`).
inline void progress_mark_batch_aborted_at(
    benchmark_context& context,
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
        expected, bioseq::instrumentation::clock::now_ns(),
        std::memory_order_relaxed, std::memory_order_relaxed);
}

inline void progress_mark_batch_aborted(
    benchmark_context& context, const work_batch& batch,
    std::string_view reason) noexcept
{
    progress_mark_batch_aborted_at(context, batch.test_case_index,
                                    batch.group_index, reason);
}


/// Push one validation_diagnostic into `*context.diagnostics` under
/// `*context.diagnostics_mu`. nullptr-safe so the helper composes with
/// places where diagnostics aren't wired (none today, but future test
/// fixtures might construct a bare context).
inline void push_runtime_diagnostic(
    benchmark_context& context,
    validation_diagnostic::diagnostic_severity severity,
    std::string_view code,
    std::string_view location,
    std::string message) noexcept
{
    if (context.diagnostics == nullptr || context.diagnostics_mu == nullptr) return;
    validation_diagnostic diag;
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
template<typename... p_argument_types>
inline void emit_runtime_diagnostic(
    benchmark_context& context,
    validation_diagnostic::diagnostic_severity severity,
    std::string_view code,
    std::string_view location,
    fmt::format_string<p_argument_types...> fmt_str,
    p_argument_types&&... arguments) noexcept
{
    push_runtime_diagnostic(context, severity, code, location,
        fmt::format(fmt_str,
                    std::forward<p_argument_types>(arguments)...));
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

// ----------------- batch_scheduler out-of-line bodies -----------------

inline void batch_scheduler::emit_idle_timeout_locked(
    const scheduled_batch& batch) noexcept
{
    if (batch.context == nullptr) return;
    auto reason = fmt::format(
        "batch '{}' (group {}) waited {}s for {} B (base+extra) with no "
        "peer batches running — giving up",
        batch.test_case_name, batch.group_index,
        k_max_idle_waiting_time.count(),
        batch.total_bytes);
    push_runtime_diagnostic(*batch.context,
        validation_diagnostic::diagnostic_severity::error,
        "memory_budget_constraint_timeout", batch.location_str, reason);
    progress_mark_batch_aborted_at(*batch.context,
        batch.test_case_index, batch.group_index, reason);
}


/// Render `trace` clipped to the first `k_max_stack_trace_lines` frames
/// using cpptrace's built-in `to_string(color=false)`. We always
/// produce uncoloured output because the rendered string is later
/// embedded inside ftxui `text()` elements, which do NOT interpret
/// ANSI escape sequences — colour codes would render as literal
/// `[34m…[0m` text. The dashboard's visual tint for trace lines comes
/// from ftxui's `| color(Color::GrayDark)` decorator instead.
[[nodiscard]] inline std::string
clip_trace(const cpptrace::stacktrace& trace) noexcept {
    cpptrace::stacktrace clipped = trace;            // implicit copy
    const bool was_truncated = clipped.frames.size() > k_max_stack_trace_lines;
    if (was_truncated) {
        clipped.frames.resize(k_max_stack_trace_lines);
    }
    auto rendered = clipped.to_string(/*color=*/false);
    if (was_truncated) rendered += "\n  … (more frames elided)";
    return rendered;
}

/// Generic retry helper used by both `on_input` and `on_cleanup` paths.
///
/// Runs `attempt()` up to `k_max_task_input_handler_error_count` times. Each
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
template<retry_kind p_kind, typename p_attempt>
[[nodiscard]] inline bool retry_user_callback(
    benchmark_context& context, const work_batch& batch,
    p_attempt&& attempt) noexcept
{
    for (std::size_t i = 0; i < k_max_task_input_handler_error_count; ++i) {
        bool failed = false;
        cpptrace::try_catch(
            [&] { attempt(); },
            [&] (const std::exception& e) {
                progress_record_retry_failure(
                    context, batch, p_kind,
                    static_cast<std::uint8_t>(i + 1),
                    e.what(),
                    clip_trace(cpptrace::from_current_exception()));
                failed = true;
            },
            [&] {     // void(): cpptrace's idiom for "catch (...)"
                progress_record_retry_failure(
                    context, batch, p_kind,
                    static_cast<std::uint8_t>(i + 1),
                    "<non-std exception>", "<no trace>");
                failed = true;
            }
        );
        if (!failed) {
            progress_clear_retry_failure(context, batch, p_kind);
            return true;
        }
        // Hold the retry-failure row visible for at least one render
        // cycle — see `k_retry_failure_min_display_time`'s comment. Applied
        // after every failed attempt (including the last) so the final
        // [retry 5/5] frame survives long enough for the dashboard to
        // paint it before the caller marks the batch aborted (which
        // wipes the retry counter).
        std::this_thread::sleep_for(k_retry_failure_min_display_time);
    }
    return false;
}

/// Try `on_input` up to `k_max_task_input_handler_error_count` times. Returns
/// `nullopt` on every-attempt-failure; the caller is expected to emit
/// `input_handler_failed_permanently` and abort the batch.
template<typename p_input, typename p_builder>
[[nodiscard]] inline std::optional<p_input>
build_input_with_retry(benchmark_context& context,
                        const work_batch& batch,
                        p_builder&& builder) noexcept
{
    std::optional<p_input> result;
    const bool ok = retry_user_callback<retry_kind::input>(
        context, batch, [&] { result.emplace(builder()); });
    if (!ok) result.reset();
    return result;
}

/// Try `on_cleanup` up to `k_max_task_input_handler_error_count` times. On
/// every-attempt-failure emits the `cleanup_function_failing` warning
/// and resumes — cleanup leaks are accepted because the OS / developer
/// will reclaim resources externally (per spec).
template<typename p_cleaner>
inline void cleanup_with_retry(benchmark_context& context,
                                const work_batch& batch,
                                std::string_view test_case_name,
                                std::string_view location,
                                p_cleaner&& cleaner) noexcept
{
    if (retry_user_callback<retry_kind::cleanup>(context, batch,
                                                  std::forward<p_cleaner>(cleaner)))
    {
        return;
    }
    emit_runtime_diagnostic(context, validation_diagnostic::diagnostic_severity::warning,
        "cleanup_function_failing", location,
        "batch '{}' (group {}) cleanup gave up after {} attempts — "
        "assuming OS / developer reclaim",
        test_case_name, batch.group_index, k_max_task_input_handler_error_count);
    progress_clear_retry_failure(context, batch, retry_kind::cleanup);
}

// `prepare_typed_benchmark` was relocated below (just before write_report) so its
// per-case `--debug` hooks can call debug_report_sink, whose CASE_RESULTS
// serializer reuses emit_execution_tuple_record + the aggregation helpers
// defined further down. See the run_unit driver section there.


/// Number of histogram buckets in `aggregated_samples::histogram`.
/// Constant by spec — the schema enforces `minItems == maxItems == 10`,
/// so changing this requires a coordinated schema bump.
inline constexpr std::size_t k_histogram_bucket_count = 10;

/// Maximum length of `aggregated_samples::top10` / `bottom10`.
/// Shorter when fewer than 10 finite samples were observed.
inline constexpr std::size_t k_k_best_size = 10;

/// Per-cell summary stats. Computed by `aggregate_samples_for_report`
/// over the *finite subset* of the sample vector (NaN / ±inf are
/// dropped before sorting). Default-constructed values (all-zero)
/// are emitted when no finite samples were observed.
struct aggregate_statistics {
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
struct aggregate_percentiles {
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
struct aggregate_bucket {
    double      from  = 0.0;
    double      to    = 0.0;
    std::size_t count = 0;
};

/// Pre-aggregated view of a sample vector. Replaces raw
/// `std::vector<double>` everywhere a consumer reads samples for
/// reporting / UI purposes. The current run still collects raw
/// samples in `measurement_cell::samples` (so per-rule comparators keep using
/// `aggregate_samples(sample_aggregate, vector<double>)` without an
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
struct aggregated_samples {
    std::size_t                                            samples_count = 0;
    aggregate_statistics                                          stats{};
    aggregate_percentiles                                    percentiles{};
    std::vector<double>                                     top10{};
    std::vector<double>                                     bottom10{};
    std::array<aggregate_bucket, k_histogram_bucket_count>      histogram{};
};

/// True iff `aggregated` carries at least one *finite* sample.
/// `samples_count == 0` ⇒ the cell wasn't emitted at all.
/// `samples_count > 0 && top10.empty()` ⇒ every emitted sample
/// was non-finite (NaN / ±inf), so every aggregate field is
/// default-zero and consumers must NOT treat the percentile slots
/// as data. Used by the renderer, the regression evaluator, and
/// any other reader that needs "is there real data here?". Single
/// definition next to the type so all consumers stay aligned.
[[nodiscard]] inline bool has_finite_samples(const aggregated_samples& aggregated) noexcept {
    return aggregated.samples_count > 0 && !aggregated.top10.empty();
}

/// Compile-time table linking each percentile to its
/// `aggregate_percentiles` field. Single source of truth used by
/// the aggregator (writes the field), the writer (emits the JSON
/// key), the historical-report reader (parses the same key), and
/// `aggregate_samples(sample_aggregate, aggregated_samples)` for the
/// rule-eval percentile lookups (P01 / P50 / P99 only — the
/// table also drives `aggregated_from_report`).
struct aggregated_percentile_entry {
    std::string_view  name;          // "P01" / "P20" / "P50" / "P80" / "P99"
    double            fraction;      // 0.01 / 0.20 / 0.50 / 0.80 / 0.99
    double aggregate_percentiles::* slot;  // pointer-to-member into the struct
};

inline constexpr std::array<aggregated_percentile_entry, 5>
k_aggregated_percentile_registry{ {
    { "P01", 0.01, &aggregate_percentiles::p01 },
    { "P20", 0.20, &aggregate_percentiles::p20 },
    { "P50", 0.50, &aggregate_percentiles::p50 },
    { "P80", 0.80, &aggregate_percentiles::p80 },
    { "P99", 0.99, &aggregate_percentiles::p99 },
} };

/// Compile-time table linking each `stats` field to its JSON
/// spelling. Drives both the writer (serialises in this order)
/// and the reader (looks each field up by name). Adding a new
/// per-cell statistic is a one-row edit (plus a slot in
/// `aggregate_statistics`) that propagates to both sides automatically.
struct aggregated_statistics_entry {
    std::string_view  name;
    double aggregate_statistics::* slot;
};

inline constexpr std::array<aggregated_statistics_entry, 6>
k_aggregated_stats_registry{ {
    { "min",    &aggregate_statistics::min    },
    { "max",    &aggregate_statistics::max    },
    { "mean",   &aggregate_statistics::mean   },
    { "mean90", &aggregate_statistics::mean90 },
    { "sum",    &aggregate_statistics::sum    },
    { "stddev", &aggregate_statistics::stddev },
} };

/// Compile-time reverse lookup `slot → JSON key`. Lets every
/// reader path (sanity detector, future stats consumers) refer
/// to a stat field by its `&aggregate_statistics::min` pointer-to-member
/// and resolve the spelling through the same registry the writer
/// uses, so a renamed field flows everywhere automatically.
/// DEBUG_ASSERTs on registry miss so a future rename that drops a
/// slot trips loudly during testing instead of silently turning a
/// `json_double_or(*stats_obj, "?", 0.0)` lookup into a
/// constant-zero false-positive.
[[nodiscard]] inline std::string_view aggregated_stat_name(
    double aggregate_statistics::* slot) noexcept
{
    for (const auto& entry : k_aggregated_stats_registry) {
        if (entry.slot == slot) return entry.name;
    }
    DEBUG_ASSERT(false,
        "aggregated_stat_name: slot is not registered in k_aggregated_stats_registry");
    return std::string_view{};
}

/// Copy of `samples` containing only finite entries (NaN / ±inf
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

[[nodiscard]] inline bool has_any_finite_sample(
    const std::vector<double>& samples) noexcept
{
    return std::any_of(samples.begin(), samples.end(),
        [](double value) { return std::isfinite(value); });
}

/// Strict availability predicate for within-run relative gates. Regression
/// report aggregation may intentionally summarize the finite subset, but a
/// controlled comparison cannot discard a failed/NaN iteration and still
/// claim the configured relationship held. `expected_samples == 0` is used by
/// named instrumentation regions because two descriptors may intentionally
/// share one name (for example scalar XXL_HIFI has two `mm_sketch` descriptor
/// samples per iteration). Strict matrices validate those sources with the
/// separate per-iteration named-region provenance map. `all_regions` sources
/// pass the configured iteration count and therefore require exactly one
/// completed finite value per scheduled iteration.
[[nodiscard]] inline bool has_complete_finite_samples(
    const std::vector<double>& samples,
    std::size_t expected_samples = 0) noexcept
{
    return !samples.empty()
        && (expected_samples == 0 || samples.size() == expected_samples)
        && std::all_of(samples.begin(), samples.end(),
            [](const double value) { return std::isfinite(value); });
}

/// A benchmark can mark an execution cell as "not measured" by returning only
/// non-finite evaluation metrics. Wall-clock still exists because the framework
/// times `on_run`, but regression rules must not compare a deliberate skip's
/// microsecond-scale bookkeeping time against measured GPU work.
[[nodiscard]] inline bool region_has_nonfinite_evaluation_sentinel(
    const std::map<std::string, std::vector<double>>& region_metrics) noexcept
{
    for (const auto& [metric_name, samples] : region_metrics) {
        if (metric_name == "wall_clock_ns" || samples.empty()) continue;
        if (!has_any_finite_sample(samples)) return true;
    }
    return false;
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
///     historical side reads via the aggregated_samples overload).
///
/// Returns 0.0 when no finite samples remain. Sum/Min/Max take a
/// fast path that skips the filter copy when every element is
/// already finite (the common case); the percentile path always
/// needs a mutable copy for `nth_element`.
[[nodiscard]] inline double aggregate_samples(
    sample_aggregate aggregate, const std::vector<double>& samples) noexcept
{
    if (samples.empty()) return 0.0;
    const bool all_finite = std::all_of(samples.begin(), samples.end(),
        [](double value) { return std::isfinite(value); });
    if (all_finite && (aggregate == sample_aggregate::sum
                        || aggregate == sample_aggregate::min
                        || aggregate == sample_aggregate::max))
    {
        switch (aggregate) {
            case sample_aggregate::sum: {
                double total = 0.0;
                for (const double value : samples) total += value;
                return total;
            }
            case sample_aggregate::min: return *std::min_element(samples.begin(), samples.end());
            case sample_aggregate::max: return *std::max_element(samples.begin(), samples.end());
            default: break;     // unreachable (gated above)
        }
    }
    std::vector<double> finite = all_finite ? samples : filter_finite(samples);
    if (finite.empty()) return 0.0;
    switch (aggregate) {
        case sample_aggregate::sum: {
            double total = 0.0;
            for (const double value : finite) total += value;
            return total;
        }
        case sample_aggregate::min:
            return *std::min_element(finite.begin(), finite.end());
        case sample_aggregate::max:
            return *std::max_element(finite.begin(), finite.end());
        case sample_aggregate::p01:
        case sample_aggregate::p50:
        case sample_aggregate::p99: {
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
    sample_aggregate aggregate, const aggregated_samples& aggregated) noexcept
{
    if (aggregated.samples_count == 0) return 0.0;
    switch (aggregate) {
        case sample_aggregate::sum: return aggregated.stats.sum;
        case sample_aggregate::min: return aggregated.stats.min;
        case sample_aggregate::max: return aggregated.stats.max;
        case sample_aggregate::p01: return aggregated.percentiles.p01;
        case sample_aggregate::p50: return aggregated.percentiles.p50;
        case sample_aggregate::p99: return aggregated.percentiles.p99;
    }
    return 0.0;
}

/// Build the `aggregated_samples` view of a raw sample vector.
/// Filters non-finite samples (NaN / ±inf) before any
/// arithmetic. `samples_count` is the original (pre-filter) size
/// — downstream consumers use it for the user-visible iteration
/// tally; finite-only fields use the filtered subset.
///
/// Pathological inputs (e.g. a billion 1e300 samples that
/// over/underflow `mean` to ±inf) are still possible. The writer
/// pipes every emitted double through `sanitize_for_json`
/// downstream so the on-disk form remains JSON-legal.
[[nodiscard]] inline aggregated_samples aggregate_samples_for_report(
    const std::vector<double>& samples) noexcept
{
    aggregated_samples out{};
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

    for (const auto& entry : k_aggregated_percentile_registry) {
        const auto idx = static_cast<std::size_t>(
            std::floor(entry.fraction
                       * static_cast<double>(finite.size() - 1)));
        out.percentiles.*(entry.slot) = finite[idx];
    }

    const auto kbest = std::min(k_k_best_size, finite.size());
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
                         / static_cast<double>(k_histogram_bucket_count);
    for (std::size_t i = 0; i < k_histogram_bucket_count; ++i) {
        out.histogram[i].from = out.stats.min + width * static_cast<double>(i);
        out.histogram[i].to   = (i + 1 == k_histogram_bucket_count)
            ? out.stats.max
            : (out.stats.min + width * static_cast<double>(i + 1));
    }
    if (std::isfinite(width) && width > 0.0) {
        for (const double value : finite) {
            auto bucket = static_cast<std::size_t>(
                std::floor((value - out.stats.min) / width));
            if (bucket >= k_histogram_bucket_count) bucket = k_histogram_bucket_count - 1;
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

// ----------------- Deterministic iteration schedule -----------------

[[nodiscard]] constexpr std::uint64_t raw_fnv1a64_update(
        std::uint64_t state, std::string_view text) noexcept {
    for (const unsigned char byte : text) {
        state ^= byte;
        state *= 1099511628211ULL;
    }
    return state;
}

[[nodiscard]] constexpr std::uint64_t raw_fnv1a64_update_u64(
        std::uint64_t state, const std::uint64_t value) noexcept {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        state ^= static_cast<unsigned char>(value >> shift);
        state *= 1099511628211ULL;
    }
    return state;
}

/// Collision-safe canonical FNV-1a identity for one repetition. Strings are
/// length-prefixed and integers are fixed-width little-endian, so identities
/// cannot alias through delimiter characters or decimal formatting.
[[nodiscard]] constexpr std::uint64_t raw_schedule_identity(
        std::string_view benchmark,
        std::string_view test_case,
        const std::size_t group_index,
        const std::size_t repeat) noexcept {
    std::uint64_t state = 14695981039346656037ULL;
    state = raw_fnv1a64_update_u64(state, benchmark.size());
    state = raw_fnv1a64_update(state, benchmark);
    state = raw_fnv1a64_update_u64(state, test_case.size());
    state = raw_fnv1a64_update(state, test_case);
    state = raw_fnv1a64_update_u64(state, group_index);
    return raw_fnv1a64_update_u64(state, repeat);
}

[[nodiscard]] constexpr std::uint64_t raw_splitmix64_next(
        std::uint64_t& state) noexcept {
    std::uint64_t value = (state += 0x9e3779b97f4a7c15ULL);
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

[[nodiscard]] constexpr std::uint64_t raw_schedule_shuffle_seed(
        const std::uint64_t configured_seed,
        std::string_view benchmark,
        std::string_view test_case,
        const std::size_t group_index,
        const std::size_t repeat) noexcept {
    std::uint64_t state = configured_seed ^ raw_schedule_identity(
        benchmark, test_case, group_index, repeat);
    return raw_splitmix64_next(state);
}
static_assert(raw_schedule_identity("bench_raw", "ONE", 0, 0)
    == 0xaf43da99ec9a27b8ULL);
static_assert(raw_schedule_shuffle_seed(
    370, "bench_raw", "ONE", 0, 0) == 0xd0a0823f87782282ULL);

/// Exact deterministic schedule: descending Fisher-Yates driven by a
/// SplitMix64 stream whose recorded seed is derived from the configured seed
/// and the FNV identity above.
[[nodiscard]] inline std::vector<std::size_t> raw_fisher_yates_order(
        const std::size_t count, const std::uint64_t shuffle_seed) {
    std::vector<std::size_t> order(count);
    for (std::size_t index = 0; index < count; ++index) order[index] = index;
    std::uint64_t state = shuffle_seed;
    for (std::size_t remaining = count; remaining > 1; --remaining) {
        const std::size_t swap_index = static_cast<std::size_t>(
            raw_splitmix64_next(state) % remaining);
        std::swap(order[remaining - 1], order[swap_index]);
    }
    return order;
}

// ----------------- benchmark_rule evaluation -----------------

/// True iff `tuple` satisfies the algorithm/parameter portion of `selector`.
/// Source coordinates (`test_case`, `region`) are applied by the rule
/// evaluator after it selects the corresponding report cell.
[[nodiscard]] inline bool selector_matches(
    const rule_selector& selector,
    const execution_tuple& tuple) noexcept
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
/// `rule_comparator`) and regression rules (which always pass
/// `threshold_pct = 0` because the schema forbids the field).
[[nodiscard]] inline bool comparator_passes(
    comparison_operator op,
    double lhs_aggregate, double rhs_aggregate,
    double threshold_pct, double tolerance_pct) noexcept
{
    const double threshold = threshold_pct / 100.0;
    const double tolerance = tolerance_pct / 100.0;
    switch (op) {
        case comparison_operator::less_than:
            return lhs_aggregate < rhs_aggregate * (1.0 - threshold + tolerance);
        case comparison_operator::greater_than:
            return lhs_aggregate > rhs_aggregate * (1.0 + threshold - tolerance);
        case comparison_operator::equal:
            return std::abs(lhs_aggregate - rhs_aggregate)
                   <= std::abs(rhs_aggregate) * tolerance;
    }
    return false;
}

/// Convenience overload for the per-rule comparator path. Forwards into
/// the explicit-args overload so `rule_comparator` and `regression_operator`
/// share one implementation.
[[nodiscard]] inline bool comparator_passes(
    const rule_comparator& cmp,
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
    const regression_operator& op,
    double now_value,
    double previously_value) noexcept
{
    // Ordered regression operators express a direction of change, but an
    // exactly unchanged zero (including either sign of IEEE zero) is not a
    // change at all.  Keep this exception local to regression rules: the
    // within-run rule_comparator path intentionally retains strict `<` / `>`
    // semantics, as do every non-zero ordered boundary below.
    if (now_value == 0.0 && previously_value == 0.0) return true;
    return comparator_passes(op.op, now_value, previously_value,
                              /*threshold_pct=*/0.0, op.tolerance_pct);
}

/// Per-cell map shape: `region -> metric -> samples`. Reused by the
/// writer and rule evaluator to expose group-merged samples.
using sample_map = std::map<std::string, std::map<std::string, std::vector<double>>>;

/// Concatenate every group cell's `samples` map into one `sample_map`.
/// Sample arrays append in cell-declaration order (= group_index order),
/// so the result is deterministic. Empty input yields an empty map.
[[nodiscard]] inline sample_map merge_group_samples(
    std::span<const measurement_cell* const> group_cells)
{
    sample_map merged;
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
using merged_samples_by_pair =
    std::map<std::pair<const execution_tuple*, std::string>, sample_map>;

using named_region_iteration_coverage =
    std::map<std::string, std::map<std::string, std::size_t>>;
using merged_named_region_coverage_by_pair =
    std::map<std::pair<const execution_tuple*, std::string>,
             named_region_iteration_coverage>;

[[nodiscard]] inline merged_samples_by_pair merged_samples_for_state(
    const benchmark_execution_state& state)
{
    std::map<std::pair<const execution_tuple*, std::string>,
             std::vector<const measurement_cell*>> by_pair;
    for (const auto& cell_ptr : state.cells) {
        by_pair[{cell_ptr->tuple, cell_ptr->test_case->name}]
            .push_back(cell_ptr.get());
    }
    merged_samples_by_pair merged;
    for (auto& [pair_key, cells_for_pair] : by_pair) {
        merged[pair_key] = merge_group_samples(
            std::span<const measurement_cell* const>(
                cells_for_pair.data(), cells_for_pair.size()));
    }
    return merged;
}

/// Sum the per-cell, per-iteration named-region contribution markers across
/// groups. Duplicate descriptors with the same name contribute one marker per
/// iteration, so the final value can be compared directly with matrix.for.
[[nodiscard]] inline merged_named_region_coverage_by_pair
merged_named_region_coverage_for_state(
    const benchmark_execution_state& state)
{
    merged_named_region_coverage_by_pair merged;
    for (const auto& cell_ptr : state.cells) {
        auto& target = merged[{cell_ptr->tuple, cell_ptr->test_case->name}];
        for (const auto& [region_name, metric_counts] :
             cell_ptr->named_region_iteration_contributions) {
            for (const auto& [metric_name, count] : metric_counts) {
                target[region_name][metric_name] += count;
            }
        }
    }
    return merged;
}

[[nodiscard]] inline std::size_t fetch_merged_named_region_coverage(
    const merged_named_region_coverage_by_pair& merged,
    const execution_tuple* tuple,
    const std::string& test_case_name,
    const std::string& region_name,
    const std::string& metric_name) noexcept
{
    const auto pair_it = merged.find({tuple, test_case_name});
    if (pair_it == merged.end()) return 0;
    const auto region_it = pair_it->second.find(region_name);
    if (region_it == pair_it->second.end()) return 0;
    const auto metric_it = region_it->second.find(metric_name);
    if (metric_it == region_it->second.end()) return 0;
    return metric_it->second;
}

/// Merge only the cells belonging to `case_name` (== execution_tuple::
/// algorithm_name) into a (tuple,test_case)->sample_map index. Used by the
/// `--debug` sink to serialize one completed case. Safe because the case's
/// cells are all finished and immutable when this runs (§4.5).
[[nodiscard]] inline merged_samples_by_pair merge_case_samples(
    const benchmark_execution_state& state, std::string_view case_name) {
    std::map<std::pair<const execution_tuple*, std::string>,
             std::vector<const measurement_cell*>> by_pair;
    for (const auto& cell_ptr : state.cells)
        if (cell_ptr->tuple->algorithm_name == case_name)
            by_pair[{cell_ptr->tuple, cell_ptr->test_case->name}].push_back(cell_ptr.get());
    merged_samples_by_pair merged;
    for (auto& [pair_key, cells_for_pair] : by_pair)
        merged[pair_key] = merge_group_samples(
            std::span<const measurement_cell* const>(cells_for_pair.data(), cells_for_pair.size()));
    return merged;
}

/// Look up `region_name -> metric_name -> samples` inside a merged
/// view. Returns nullptr when any link in the chain is missing.
[[nodiscard]] inline const std::vector<double>* fetch_merged_samples(
    const merged_samples_by_pair& merged,
    const execution_tuple* tuple,
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

/// One value per logical repeat for ratio-paired rule evaluation. The ordinary
/// report arrays retain their independent-aggregate shape. Named
/// `region_total_ns` sources can contain multiple descriptors with the same
/// name, so their strict per-iteration descriptor counts partition the raw
/// vector and each partition is reduced with the rule's configured aggregate
/// into one repeat value. This retains the descriptor semantics
/// without pairing unrelated run-level percentiles.
struct logical_iteration_series {
    std::vector<double> values{};
    std::vector<std::vector<double>> descriptor_samples{};
    std::vector<iteration_schedule_sample> schedule_samples{};
    std::size_t tuple_index = 0;
    std::size_t test_case_index = 0;
    std::size_t group_index = 0;
    bool schedule_complete = false;
    bool complete = false;
};

[[nodiscard]] inline logical_iteration_series build_logical_iteration_series(
    const benchmark_execution_state& state,
    const execution_tuple* tuple,
    const std::string& test_case_name,
    const std::string& region_name,
    const std::string& metric_name,
    const sample_aggregate descriptor_aggregate)
{
    logical_iteration_series result;
    const measurement_cell* selected_cell = nullptr;
    for (const auto& cell_ptr : state.cells) {
        if (cell_ptr->tuple != tuple
            || cell_ptr->test_case->name != test_case_name) {
            continue;
        }
        // Paired rules are semantically restricted to one group. Retain this
        // runtime guard so a malformed/bypassed config cannot collide local
        // repeat ordinals while still producing an accepted ratio.
        if (selected_cell != nullptr || cell_ptr->group_count != 1
            || cell_ptr->group_index != 0) {
            return result;
        }
        selected_cell = cell_ptr.get();
    }
    if (selected_cell == nullptr) return result;
    result.tuple_index = selected_cell->tuple_index;
    result.test_case_index = selected_cell->test_case_index;
    result.group_index = selected_cell->group_index;
    if (selected_cell->iteration_schedule_samples.size()
            == selected_cell->iterations) {
        result.schedule_samples = selected_cell->iteration_schedule_samples;
        result.schedule_complete = true;
        for (std::size_t repeat = 0;
             repeat < result.schedule_samples.size(); ++repeat) {
            if (result.schedule_samples[repeat].repeat != repeat) {
                result.schedule_complete = false;
                break;
            }
        }
    }

    const auto region_it = selected_cell->samples.find(region_name);
    if (region_it == selected_cell->samples.end()) return result;
    const auto metric_it = region_it->second.find(metric_name);
    if (metric_it == region_it->second.end()) return result;
    const auto& raw_values = metric_it->second;

    if (region_name == "all_regions") {
        if (raw_values.size() != selected_cell->iterations) return result;
        result.values = raw_values;
        result.descriptor_samples.reserve(raw_values.size());
        for (const double raw_value : raw_values) {
            result.descriptor_samples.push_back({raw_value});
        }
        result.complete = true;
        return result;
    }

    if (metric_name != "region_total_ns"
        || selected_cell->named_region_iteration_sample_counts.size()
            != selected_cell->iterations) {
        return result;
    }
    result.values.reserve(selected_cell->iterations);
    result.descriptor_samples.reserve(selected_cell->iterations);
    std::size_t raw_cursor = 0;
    for (const auto& iteration_counts :
         selected_cell->named_region_iteration_sample_counts) {
        const auto named_it = iteration_counts.find(region_name);
        if (named_it == iteration_counts.end()) return {};
        const auto count_it = named_it->second.find(metric_name);
        if (count_it == named_it->second.end() || count_it->second == 0
            || count_it->second > raw_values.size() - raw_cursor) {
            return {};
        }
        std::vector<double> descriptor_values;
        descriptor_values.reserve(count_it->second);
        for (std::size_t descriptor_index = 0;
             descriptor_index < count_it->second; ++descriptor_index) {
            descriptor_values.push_back(raw_values[raw_cursor++]);
        }
        result.values.push_back(
            aggregate_samples(descriptor_aggregate, descriptor_values));
        result.descriptor_samples.push_back(std::move(descriptor_values));
    }
    if (raw_cursor != raw_values.size()) return {};
    result.complete = true;
    return result;
}

[[nodiscard]] inline std::optional<double> aggregate_paired_ratios(
    const rule_comparator& comparator,
    const logical_iteration_series& lhs,
    const logical_iteration_series& rhs)
{
    if (!lhs.complete || !rhs.complete
        || lhs.values.size() != rhs.values.size()
        || comparator.warmup_repetitions >= lhs.values.size()) {
        return std::nullopt;
    }
    std::vector<double> ratios;
    ratios.reserve(lhs.values.size() - comparator.warmup_repetitions);
    for (std::size_t repeat = comparator.warmup_repetitions;
         repeat < lhs.values.size(); ++repeat) {
        const double numerator = lhs.values[repeat];
        const double denominator = rhs.values[repeat];
        if (!std::isfinite(numerator) || !std::isfinite(denominator)
            || denominator == 0.0) {
            return std::nullopt;
        }
        const double ratio = numerator / denominator;
        if (!std::isfinite(ratio)) return std::nullopt;
        ratios.push_back(ratio);
    }
    if (ratios.empty()) return std::nullopt;
    return aggregate_samples(comparator.aggregate, ratios);
}

/// Materialize the exact inputs and result of one resolved ratio-paired rule.
/// Schedule matching is part of acceptance: a numerical ratio cannot be
/// accepted when the two source cells lack the actual interleaved coordinates
/// needed to prove that repeat N was compared with repeat N.
[[nodiscard]] inline paired_rule_evidence make_paired_rule_evidence(
    const benchmark_execution_state& state,
    const benchmark_rule& rule,
    const std::string& test_case_name,
    const std::string& lhs_region,
    const std::string& rhs_region,
    const execution_tuple* lhs_tuple,
    const execution_tuple* rhs_tuple,
    const std::size_t iterations,
    const bool lhs_source_present,
    const bool rhs_source_present,
    const logical_iteration_series& lhs,
    const logical_iteration_series& rhs,
    const std::optional<double> observed_paired_ratio)
{
    paired_rule_evidence evidence;
    evidence.config_path = state.config.config_path;
    evidence.benchmark_key = state.config.benchmark_key;
    evidence.rule_name = rule.name;
    evidence.test_case = test_case_name;
    evidence.metric = rule.metric_name;
    evidence.comparator = rule.cmp;
    evidence.benchmark_index = state.driver_benchmark_index;
    evidence.group_index = 0;
    evidence.schedule_mode = state.config.iteration_order
            == benchmark_configuration::iteration_order_mode::interleaved_seeded
        ? "interleaved_seeded" : "item_major";
    evidence.configured_schedule_seed = state.config.iteration_order_seed;

    const auto lhs_tuple_index = static_cast<std::size_t>(
        lhs_tuple - state.execution_tuples.data());
    const auto rhs_tuple_index = static_cast<std::size_t>(
        rhs_tuple - state.execution_tuples.data());
    evidence.numerator_route = paired_rule_route_evidence{
        .algorithm = lhs_tuple->algorithm_name,
        .params = lhs_tuple->params,
        .tuple_index = lhs_tuple_index,
        .test_case = test_case_name,
        .region = lhs_region,
        .metric = rule.metric_name,
    };
    evidence.denominator_route = paired_rule_route_evidence{
        .algorithm = rhs_tuple->algorithm_name,
        .params = rhs_tuple->params,
        .tuple_index = rhs_tuple_index,
        .test_case = test_case_name,
        .region = rhs_region,
        .metric = rule.metric_name,
    };

    bool test_case_found = false;
    for (std::size_t test_case_index = 0;
         test_case_index < state.test_cases.size(); ++test_case_index) {
        if (state.test_cases[test_case_index].name == test_case_name) {
            evidence.test_case_index = test_case_index;
            test_case_found = true;
            break;
        }
    }

    std::vector<std::size_t> scheduled_tuple_indices;
    bool schedule_topology_matches = test_case_found
        && state.config.iteration_order
            == benchmark_configuration::iteration_order_mode::interleaved_seeded;
    for (const auto& cell : state.cells) {
        if (cell->test_case->name != test_case_name) continue;
        if (cell->group_count != 1 || cell->group_index != 0
            || cell->test_case_index != evidence.test_case_index) {
            schedule_topology_matches = false;
            continue;
        }
        scheduled_tuple_indices.push_back(cell->tuple_index);
    }
    const auto tuple_is_scheduled = [&scheduled_tuple_indices](
            const std::size_t tuple_index) {
        return std::find(
            scheduled_tuple_indices.begin(),
            scheduled_tuple_indices.end(), tuple_index)
            != scheduled_tuple_indices.end();
    };
    schedule_topology_matches = schedule_topology_matches
        && !scheduled_tuple_indices.empty()
        && tuple_is_scheduled(lhs_tuple_index)
        && tuple_is_scheduled(rhs_tuple_index)
        && std::set<std::size_t>(
               scheduled_tuple_indices.begin(),
               scheduled_tuple_indices.end()).size()
            == scheduled_tuple_indices.size();

    evidence.repeats.reserve(iterations);
    bool every_schedule_identity_matches = schedule_topology_matches
        && lhs.schedule_complete && rhs.schedule_complete
        && lhs.group_index == 0 && rhs.group_index == 0
        && lhs.test_case_index == evidence.test_case_index
        && rhs.test_case_index == evidence.test_case_index
        && lhs.schedule_samples.size() == iterations
        && rhs.schedule_samples.size() == iterations;
    for (std::size_t repeat = 0; repeat < iterations; ++repeat) {
        paired_rule_repeat_evidence repeat_evidence;
        repeat_evidence.repeat = repeat;
        repeat_evidence.included_after_warmup =
            repeat >= rule.cmp.warmup_repetitions;

        if (repeat < lhs.descriptor_samples.size()) {
            repeat_evidence.numerator_samples = lhs.descriptor_samples[repeat];
        }
        if (repeat < rhs.descriptor_samples.size()) {
            repeat_evidence.denominator_samples = rhs.descriptor_samples[repeat];
        }
        if (repeat < lhs.values.size()
            && std::isfinite(lhs.values[repeat])) {
            repeat_evidence.numerator_value_present = true;
            repeat_evidence.numerator_value = lhs.values[repeat];
        }
        if (repeat < rhs.values.size()
            && std::isfinite(rhs.values[repeat])) {
            repeat_evidence.denominator_value_present = true;
            repeat_evidence.denominator_value = rhs.values[repeat];
        }
        if (repeat_evidence.numerator_value_present
            && repeat_evidence.denominator_value_present
            && repeat_evidence.denominator_value != 0.0) {
            const double ratio = repeat_evidence.numerator_value
                / repeat_evidence.denominator_value;
            if (std::isfinite(ratio)) {
                repeat_evidence.ratio_present = true;
                repeat_evidence.ratio = ratio;
            }
        }

        if (lhs.schedule_complete
            && repeat < lhs.schedule_samples.size()) {
            repeat_evidence.numerator_schedule_present = true;
            repeat_evidence.numerator_schedule = lhs.schedule_samples[repeat];
        }
        if (rhs.schedule_complete
            && repeat < rhs.schedule_samples.size()) {
            repeat_evidence.denominator_schedule_present = true;
            repeat_evidence.denominator_schedule = rhs.schedule_samples[repeat];
        }
        const std::uint64_t expected_shuffle_seed =
            raw_schedule_shuffle_seed(
                state.config.iteration_order_seed,
                state.config.benchmark_key,
                test_case_name, evidence.group_index, repeat);
        const auto expected_order = raw_fisher_yates_order(
            scheduled_tuple_indices.size(), expected_shuffle_seed);
        std::optional<std::size_t> expected_numerator_position;
        std::optional<std::size_t> expected_denominator_position;
        for (std::size_t position = 0;
             position < expected_order.size(); ++position) {
            const std::size_t scheduled_index = expected_order[position];
            if (scheduled_index >= scheduled_tuple_indices.size()) {
                schedule_topology_matches = false;
                continue;
            }
            const std::size_t scheduled_tuple =
                scheduled_tuple_indices[scheduled_index];
            if (scheduled_tuple == lhs_tuple_index) {
                expected_numerator_position = position;
            }
            if (scheduled_tuple == rhs_tuple_index) {
                expected_denominator_position = position;
            }
        }
        const bool numerator_schedule_matches =
            repeat_evidence.numerator_schedule_present
            && repeat_evidence.numerator_schedule.repeat == repeat
            && repeat_evidence.numerator_schedule.shuffle_seed
                == expected_shuffle_seed
            && expected_numerator_position.has_value()
            && repeat_evidence.numerator_schedule.order_position
                == *expected_numerator_position;
        const bool denominator_schedule_matches =
            repeat_evidence.denominator_schedule_present
            && repeat_evidence.denominator_schedule.repeat == repeat
            && repeat_evidence.denominator_schedule.shuffle_seed
                == expected_shuffle_seed
            && expected_denominator_position.has_value()
            && repeat_evidence.denominator_schedule.order_position
                == *expected_denominator_position;
        repeat_evidence.schedule_identity_matches =
            schedule_topology_matches
            && numerator_schedule_matches
            && denominator_schedule_matches;
        every_schedule_identity_matches =
            every_schedule_identity_matches
            && repeat_evidence.schedule_identity_matches;
        evidence.repeats.push_back(std::move(repeat_evidence));
    }

    evidence.inputs_present = {
        lhs_source_present && lhs.complete && lhs.schedule_complete,
        rhs_source_present && rhs.complete && rhs.schedule_complete,
    };
    evidence.observed_paired_ratio_present =
        evidence.inputs_present[0] && evidence.inputs_present[1]
        && every_schedule_identity_matches
        && observed_paired_ratio.has_value()
        && std::isfinite(*observed_paired_ratio);
    if (evidence.observed_paired_ratio_present) {
        evidence.observed_paired_ratio = *observed_paired_ratio;
        evidence.accepted = comparator_passes(
            rule.cmp, evidence.observed_paired_ratio,
            rule.cmp.paired_baseline);
    }
    return evidence;
}

/// Apply every rule against every test case and collect failures. With
/// test_case)` *before* the rule loop so LHS/RHS aggregates are computed
/// from the same merged sample arrays the report writer will emit.
inline void evaluate_rules(
    const benchmark_execution_state& state,
    std::vector<violation>& violations,
    std::vector<paired_rule_evidence>& evidence_records)
{
    const auto merged = merged_samples_for_state(state);
    const auto named_region_coverage =
        merged_named_region_coverage_for_state(state);

    for (const auto& metric : state.config.metrics) {
        for (const auto& rule : metric.rules) {
            for (const auto& [test_case_name, iterations] : state.config.for_each) {
                if (iterations == 0) continue;
                if ((!rule.lhs.test_case.empty()
                     && rule.lhs.test_case != test_case_name)
                    || (!rule.rhs.test_case.empty()
                        && rule.rhs.test_case != test_case_name)) {
                    continue;
                }
                // Collect tuple* on each side so we evaluate the rule
                // exactly once per (LHS_tuple, RHS_tuple) pair, never
                // duplicated across groups.
                std::vector<const execution_tuple*> lhs_tuples;
                std::vector<const execution_tuple*> rhs_tuples;
                for (const auto& tuple : state.execution_tuples) {
                    if (selector_matches(rule.lhs, tuple)) lhs_tuples.push_back(&tuple);
                    if (selector_matches(rule.rhs, tuple)) rhs_tuples.push_back(&tuple);
                }
                const std::string lhs_region = rule.lhs.region.empty()
                    ? std::string{"all_regions"}
                    : rule.lhs.region;
                const std::string rhs_region = rule.rhs.region.empty()
                    ? std::string{"all_regions"}
                    : rule.rhs.region;
                for (const auto* lhs_tuple : lhs_tuples) {
                    for (const auto* rhs_tuple : rhs_tuples) {
                        const auto* lhs_samples = fetch_merged_samples(
                            merged, lhs_tuple, test_case_name,
                            lhs_region, rule.metric_name);
                        const auto* rhs_samples = fetch_merged_samples(
                            merged, rhs_tuple, test_case_name,
                            rhs_region, rule.metric_name);
                        const auto source_is_measured =
                            [&merged, &test_case_name](
                                const execution_tuple* tuple) noexcept {
                                const auto source_it = merged.find(
                                    {tuple, test_case_name});
                                if (source_it == merged.end()) return false;
                                const auto all_regions_it =
                                    source_it->second.find("all_regions");
                                return all_regions_it != source_it->second.end()
                                    && !region_has_nonfinite_evaluation_sentinel(
                                        all_regions_it->second);
                            };
                        const std::size_t lhs_expected_samples =
                            lhs_region == "all_regions" ? iterations : 0;
                        const std::size_t rhs_expected_samples =
                            rhs_region == "all_regions" ? iterations : 0;
                        const auto named_region_is_complete =
                            [&state, &named_region_coverage, &test_case_name,
                             iterations](const execution_tuple* tuple,
                                         const std::string& region,
                                         const std::string& metric_name) noexcept {
                                return region == "all_regions"
                                    || !state.config.acceptance
                                            .require_named_region_iteration_coverage
                                    || fetch_merged_named_region_coverage(
                                           named_region_coverage, tuple,
                                           test_case_name, region, metric_name)
                                           == iterations;
                            };
                        bool lhs_present = lhs_samples != nullptr
                            && has_complete_finite_samples(
                                *lhs_samples, lhs_expected_samples)
                            && named_region_is_complete(
                                lhs_tuple, lhs_region, rule.metric_name)
                            && source_is_measured(lhs_tuple);
                        bool rhs_present = rhs_samples != nullptr
                            && has_complete_finite_samples(
                                *rhs_samples, rhs_expected_samples)
                            && named_region_is_complete(
                                rhs_tuple, rhs_region, rule.metric_name)
                            && source_is_measured(rhs_tuple);
                        double lhs_aggregate = lhs_present
                            ? aggregate_samples(rule.cmp.aggregate, *lhs_samples)
                            : 0.0;
                        double rhs_aggregate = rhs_present
                            ? aggregate_samples(rule.cmp.aggregate, *rhs_samples)
                            : 0.0;
                        std::optional<bool> paired_evidence_accepted;
                        std::optional<bool> exact_iteration_accepted;
                        if (rule.cmp.pairing
                            == rule_comparator::pairing_mode::by_iteration_ratio) {
                            const bool lhs_source_present = lhs_present;
                            const bool rhs_source_present = rhs_present;
                            const auto lhs_logical = build_logical_iteration_series(
                                state, lhs_tuple, test_case_name,
                                lhs_region, rule.metric_name,
                                rule.cmp.aggregate);
                            const auto rhs_logical = build_logical_iteration_series(
                                state, rhs_tuple, test_case_name,
                                rhs_region, rule.metric_name,
                                rule.cmp.aggregate);
                            const auto paired_ratio = aggregate_paired_ratios(
                                rule.cmp, lhs_logical, rhs_logical);
                            bool paired_present = lhs_present
                                && rhs_present && paired_ratio.has_value();
                            auto evidence = make_paired_rule_evidence(
                                state, rule, test_case_name,
                                lhs_region, rhs_region,
                                lhs_tuple, rhs_tuple, iterations,
                                lhs_source_present, rhs_source_present,
                                lhs_logical, rhs_logical, paired_ratio);
                            paired_present =
                                evidence.observed_paired_ratio_present;
                            paired_evidence_accepted = evidence.accepted;
                            evidence_records.push_back(
                                std::move(evidence));
                            lhs_present = paired_present;
                            rhs_present = paired_present;
                            lhs_aggregate = paired_present
                                ? *paired_ratio : 0.0;
                            // A ratio-paired rule compares
                            // aggregate(lhs[i]/rhs[i]) against its explicit
                            // configured reference ratio.
                            rhs_aggregate = paired_present
                                ? rule.cmp.paired_baseline : 0.0;
                        } else if (rule.cmp.pairing
                                   == rule_comparator::pairing_mode::
                                       by_iteration_exact) {
                            exact_iteration_accepted = false;
                            if (lhs_present && rhs_present
                                && lhs_samples != nullptr
                                && rhs_samples != nullptr
                                && lhs_samples->size() == rhs_samples->size()) {
                                exact_iteration_accepted = true;
                                for (std::size_t repeat = 0;
                                     repeat < lhs_samples->size(); ++repeat) {
                                    const double lhs_value = (*lhs_samples)[repeat];
                                    const double rhs_value = (*rhs_samples)[repeat];
                                    if (std::bit_cast<std::uint64_t>(lhs_value)
                                        == std::bit_cast<std::uint64_t>(
                                            rhs_value)) {
                                        continue;
                                    }
                                    exact_iteration_accepted = false;
                                    // Preserve the first unequal repeat as the
                                    // durable violation witness. Completeness
                                    // checks above already rejected non-finite
                                    // values and missing iterations.
                                    lhs_aggregate = lhs_value;
                                    rhs_aggregate = rhs_value;
                                    break;
                                }
                            }
                        }
                        const bool rule_accepted =
                            exact_iteration_accepted.has_value()
                            ? *exact_iteration_accepted
                            : paired_evidence_accepted.has_value()
                            ? *paired_evidence_accepted
                            : lhs_present && rhs_present
                                && comparator_passes(
                                    rule.cmp, lhs_aggregate, rhs_aggregate);
                        if (rule_accepted) {
                            continue;
                        }

                        violation detected_violation;
                        detected_violation.config_path     = state.config.config_path;
                        detected_violation.benchmark_key   = state.config.benchmark_key;
                        detected_violation.rule_name       = rule.name;
                        detected_violation.test_case       = test_case_name;
                        detected_violation.metric          = rule.metric_name;
                        detected_violation.lhs_algorithm   = lhs_tuple->algorithm_name;
                        detected_violation.lhs_full_params = lhs_tuple->params;
                        detected_violation.rhs_algorithm   = rhs_tuple->algorithm_name;
                        detected_violation.rhs_full_params = rhs_tuple->params;
                        detected_violation.args            = {lhs_aggregate, rhs_aggregate};
                        detected_violation.args_present    = {lhs_present, rhs_present};
                        detected_violation.comparison_mode = rule.cmp.pairing;
                        detected_violation.warmup_repetitions =
                            rule.cmp.warmup_repetitions;
                        detected_violation.blocking = rule.cmp.blocking;
                        violations.push_back(std::move(detected_violation));
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

/// Wall-clock Unix epoch in milliseconds. The single wall-clock-epoch helper
/// in the framework: the debug event log uses it directly (ms granularity for
/// sub-second ordering within a run), and `capture_run_meta` sources its
/// epoch-seconds `meta.timestamp` from it (`/ 1000`). (`bioseq::instrumentation::clock::
/// now_ns` is a *monotonic* steady clock, unsuitable for wall-clock event
/// timestamps, so it is not reused here.)
[[nodiscard]] inline std::int64_t now_unix_millis() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

/// Capture this run's identity in a single struct. Called once from
/// `run_impl`; the resulting `report_metadata` is forwarded to both the
/// report writer (for the `meta` block) and the regression evaluator
/// (which doesn't need most fields, but takes the whole struct for
/// future-proofing).
[[nodiscard]] inline report_metadata capture_run_meta(bool fast_filter) noexcept {
    report_metadata meta;
    meta.commit = git_head_commit();
    meta.dirty  = git_working_tree_dirty();
    meta.fast   = fast_filter;
    // Epoch seconds, sourced from the single wall-clock helper (dedup).
    meta.timestamp = now_unix_millis() / 1000;
    // Keep a `time_point` for the ISO-8601 `created` string (gmtime_r needs it).
    const auto now = std::chrono::system_clock::now();
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
struct parsed_historical_report {
    std::string                body;
    simdjson::dom::parser      parser;
    simdjson::dom::element     root;
    std::size_t                index = 0;

    parsed_historical_report() = default;
    parsed_historical_report(const parsed_historical_report&) = delete;
    parsed_historical_report& operator=(const parsed_historical_report&) = delete;
    parsed_historical_report(parsed_historical_report&&) = delete;
    parsed_historical_report& operator=(parsed_historical_report&&) = delete;
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
        constexpr std::string_view k_prefix = "report_";
        constexpr std::string_view k_suffix = ".json";
        if (filename.size() <= k_prefix.size() + k_suffix.size()) continue;
        if (filename.compare(0, k_prefix.size(), k_prefix.data(), k_prefix.size()) != 0) continue;
        if (filename.compare(filename.size() - k_suffix.size(),
                              k_suffix.size(),
                              k_suffix.data(), k_suffix.size()) != 0) continue;
        const auto digits = filename.substr(
            k_prefix.size(),
            filename.size() - k_prefix.size() - k_suffix.size());
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

/// Allocation guard for crash-left raw sidecars.  A sidecar is created with
/// O_EXCL before any benchmark work, so a crashed run may have no matching
/// report_<N>.json.  Treat both completed reports and orphan sidecars as used
/// indices; gaps are evidence and must never be overwritten.
[[nodiscard]] inline std::optional<std::size_t> max_existing_run_artifact_index(
    const std::filesystem::path& report_dir) noexcept
{
    std::error_code ec;
    if (!std::filesystem::exists(report_dir, ec) || ec) return std::nullopt;
    std::optional<std::size_t> max_index =
        max_existing_report_index(report_dir);
    for (const auto& entry : std::filesystem::directory_iterator(report_dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec) || ec) continue;
        const std::string filename = entry.path().filename().string();
        constexpr std::string_view prefix = "report_";
        constexpr std::string_view suffix = ".raw.jsonl";
        if (!filename.starts_with(prefix) || !filename.ends_with(suffix)
            || filename.size() <= prefix.size() + suffix.size()) {
            continue;
        }
        const std::string_view digits{filename.data() + prefix.size(),
            filename.size() - prefix.size() - suffix.size()};
        std::size_t parsed = 0;
        bool valid = !digits.empty();
        for (const char ch : digits) {
            if (ch < '0' || ch > '9'
                || parsed > ((std::numeric_limits<std::size_t>::max)()
                        - static_cast<std::size_t>(ch - '0')) / 10) {
                valid = false;
                break;
            }
            parsed = parsed * 10 + static_cast<std::size_t>(ch - '0');
        }
        if (valid && (!max_index.has_value() || parsed > *max_index)) {
            max_index = parsed;
        }
    }
    return max_index;
}

/// Load and parse `report_<index>.json` from disk. Returns nullptr when
/// the file is missing or the JSON body fails to parse — both of which
/// are downgraded to "history gap" by the regression evaluator.
[[nodiscard]] inline std::unique_ptr<parsed_historical_report> load_report_at_index(
    const std::filesystem::path& report_dir, std::size_t index) noexcept
{
    const auto path = report_path_for_index(report_dir, index);
    auto body = slurp_file(path);
    if (body.empty()) return nullptr;
    auto holder = std::make_unique<parsed_historical_report>();
    holder->index = index;
    holder->body  = std::move(body);
    if (holder->parser.parse(holder->body).get(holder->root) != simdjson::SUCCESS) {
        return nullptr;
    }
    return holder;
}

/// Compare a JSON value (from a parsed historical report) against a
/// `config_value` (from the current run's tuple). Numbers are matched
/// across the integer/double divide so a round-trip like 10 → 10.0 → 10
/// still compares equal.
[[nodiscard]] inline bool simdjson_equals_config_value(
    simdjson::dom::element json_value, const config_value& cv) noexcept
{
    switch (cv.kind) {
        case config_value::value_kind::string: {
            std::string_view sv;
            return json_value.get(sv) == simdjson::SUCCESS && sv == cv.string_value;
        }
        case config_value::value_kind::integer: {
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
        case config_value::value_kind::floating_point: {
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
        case config_value::value_kind::boolean: {
            bool b_value = false;
            return json_value.get(b_value) == simdjson::SUCCESS
                   && b_value == cv.boolean_value;
        }
        case config_value::value_kind::non_scalar:
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

/// Pull the `meta.fast` flag from a parsed report. Missing or malformed
/// metadata is incomparable and returns no value.
[[nodiscard]] inline std::optional<bool> fast_from_report(
    simdjson::dom::element report_root) noexcept
{
    const auto root_obj = try_as_object(report_root);
    if (!root_obj.has_value()) return std::nullopt;
    const auto meta_obj = try_get_object(*root_obj, "meta");
    if (!meta_obj.has_value()) return std::nullopt;
    bool fast = false;
    if ((*meta_obj).at_key("fast").get(fast) != simdjson::SUCCESS) {
        return std::nullopt;
    }
    return fast;
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
/// `aggregated_samples`. Missing optional sub-objects collapse to
/// default-constructed values; consumers that need the full shape
/// (sanity warnings, regression eval, --ui) check
/// `samples_count > 0` and `top10.empty()` upfront.
[[nodiscard]] inline aggregated_samples parse_aggregated_samples(
    simdjson::dom::object metric_obj) noexcept
{
    aggregated_samples agg{};
    agg.samples_count = static_cast<std::size_t>(std::max<std::int64_t>(
        0, json_int_or(metric_obj, "samples_count", 0)));
    if (const auto stats_obj = try_get_object(metric_obj, "stats");
        stats_obj.has_value())
    {
        for (const auto& entry : k_aggregated_stats_registry) {
            agg.stats.*(entry.slot) = json_double_or(*stats_obj, entry.name, 0.0);
        }
    }
    if (const auto pct_obj = try_get_object(metric_obj, "percentiles");
        pct_obj.has_value())
    {
        for (const auto& entry : k_aggregated_percentile_registry) {
            agg.percentiles.*(entry.slot) =
                json_double_or(*pct_obj, entry.name, 0.0);
        }
    }
    /// Pull `parent[key]` as a `vector<double>`. Non-numeric items
    /// (corrupt historical reports) are dropped silently — the
    /// resulting array may shrink below the source length, breaking
    /// the documented `top10[0] == stats.max` invariant of an
    /// aggregator-built `aggregated_samples`. Consumers that depend
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
            if (bucket_index >= k_histogram_bucket_count) break;
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
/// path and return the matching metric object.  Keeping the raw DOM object
/// available lets regression-history eligibility validate persisted numeric
/// fields before `parse_aggregated_samples` applies its intentionally
/// permissive UI defaults.
[[nodiscard]] inline std::optional<simdjson::dom::object>
aggregated_metric_object_from_report(
    simdjson::dom::element report_root,
    std::string_view benchmark,
    std::string_view algorithm_name,
    const parameter_map&  params,
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

        return metric_obj;
    }
    return std::nullopt;
}

/// Pull one pre-aggregated record from a parsed report. Returns nullopt on
/// any path miss. The general historical-report UI remains permissive here;
/// regression candidate selection performs its stricter integrity checks on
/// `aggregated_metric_object_from_report` before accepting a baseline.
[[nodiscard]] inline std::optional<aggregated_samples> aggregated_from_report(
    simdjson::dom::element report_root,
    std::string_view benchmark,
    std::string_view algorithm_name,
    const parameter_map&  params,
    std::string_view test_case,
    std::string_view region,
    std::string_view metric)
{
    const auto metric_obj = aggregated_metric_object_from_report(
        report_root, benchmark, algorithm_name, params,
        test_case, region, metric);
    if (!metric_obj.has_value()) return std::nullopt;
    return parse_aggregated_samples(*metric_obj);
}

/// Lazy parser-cache for historical reports. Each `report_<N>.json`
/// is parsed at most once per cache instance even if multiple
/// consumers (regressions, sanity warnings) ask for the same index.
/// `parsed_historical_report` is held via `unique_ptr` because it pins
/// (the inner simdjson parser is non-movable). Stack-allocated;
/// freed at the end of `run_impl`'s evaluation phase.
struct historical_report_cache {
    std::filesystem::path                                                report_dir;
    std::map<std::size_t, std::unique_ptr<parsed_historical_report>>      cached{};
    std::set<std::size_t>                                                known_missing{};

    explicit historical_report_cache(std::filesystem::path dir) noexcept
        : report_dir(std::move(dir)) {}

    historical_report_cache(const historical_report_cache&) = delete;
    historical_report_cache& operator=(const historical_report_cache&) = delete;
    historical_report_cache(historical_report_cache&&) = delete;
    historical_report_cache& operator=(historical_report_cache&&) = delete;

    /// Fetch (loading on demand) the report at `index`. Returns nullptr
    /// once for a missing report and remembers that miss so subsequent
    /// fetches don't re-touch the filesystem.
    [[nodiscard]] parsed_historical_report* fetch(std::size_t index) noexcept {
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

/// Parse a non-negative JSON integer into `size_t` without accepting a
/// floating-point spelling or overflowing the host type.
[[nodiscard]] inline std::optional<std::size_t> strict_json_size_field(
    simdjson::dom::object object, std::string_view key) noexcept
{
    simdjson::dom::element value;
    if (object.at_key(key).get(value) != simdjson::SUCCESS) {
        return std::nullopt;
    }
    std::uint64_t unsigned_value = 0;
    if (value.get(unsigned_value) == simdjson::SUCCESS) {
        if (unsigned_value > (std::numeric_limits<std::size_t>::max)()) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(unsigned_value);
    }
    std::int64_t signed_value = 0;
    if (value.get(signed_value) != simdjson::SUCCESS || signed_value < 0) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(signed_value);
}

/// Parse a JSON number and reject missing, malformed, or non-finite values.
/// Regression baselines use this instead of `json_double_or` so corrupt
/// aggregate fields can never become an eligible synthetic zero.
[[nodiscard]] inline std::optional<double> strict_finite_json_number_field(
    simdjson::dom::object object, std::string_view key) noexcept
{
    simdjson::dom::element value;
    if (object.at_key(key).get(value) != simdjson::SUCCESS) {
        return std::nullopt;
    }
    double number = 0.0;
    if (value.get(number) == simdjson::SUCCESS && std::isfinite(number)) {
        return number;
    }
    std::int64_t signed_value = 0;
    if (value.get(signed_value) == simdjson::SUCCESS) {
        return static_cast<double>(signed_value);
    }
    std::uint64_t unsigned_value = 0;
    if (value.get(unsigned_value) == simdjson::SUCCESS) {
        number = static_cast<double>(unsigned_value);
        if (std::isfinite(number)) return number;
    }
    return std::nullopt;
}

/// Validate the top-level durable raw-evidence authority of one historical
/// report. `null` is the explicit no-sidecar state and is eligible. An object
/// is eligible only when it explicitly records `complete: true` and a valid
/// non-negative `record_count`; missing or malformed states fail closed.
[[nodiscard]] inline std::optional<std::string>
historical_raw_evidence_ineligibility_reason(
    simdjson::dom::element report_root) noexcept
{
    const auto root_obj = try_as_object(report_root);
    if (!root_obj.has_value()) return std::string{"report root is not an object"};

    simdjson::dom::element raw_evidence;
    if ((*root_obj).at_key("raw_evidence").get(raw_evidence)
        != simdjson::SUCCESS) {
        return std::string{"raw_evidence is missing"};
    }
    if (raw_evidence.type() == simdjson::dom::element_type::NULL_VALUE) {
        return std::nullopt;
    }

    const auto raw_obj = try_as_object(raw_evidence);
    if (!raw_obj.has_value()) {
        return std::string{"raw_evidence is neither null nor an object"};
    }
    bool complete = false;
    if ((*raw_obj).at_key("complete").get(complete) != simdjson::SUCCESS) {
        return std::string{"raw_evidence.complete is missing or malformed"};
    }
    if (!complete) return std::string{"raw_evidence.complete is false"};
    if (!strict_json_size_field(*raw_obj, "record_count").has_value()) {
        return std::string{"raw_evidence.record_count is missing or malformed"};
    }
    return std::nullopt;
}

/// Reject captures that the producing run did not accept. A current report is
/// authoritative only when its explicit `meta.history_eligible` seal is true
/// and all finding arrays are closed. A failed or incomplete row must never
/// ratchet an unrelated cell's future baseline.
[[nodiscard]] inline std::optional<std::string>
historical_run_outcome_ineligibility_reason(
    simdjson::dom::element report_root) noexcept
{
    const auto root_obj = try_as_object(report_root);
    if (!root_obj.has_value()) return std::string{"report root is not an object"};
    const auto meta_obj = try_get_object(*root_obj, "meta");
    if (!meta_obj.has_value()) return std::string{"meta is missing or malformed"};

    simdjson::dom::element seal_element;
    const auto seal_status =
        (*meta_obj).at_key("history_eligible").get(seal_element);
    if (seal_status == simdjson::NO_SUCH_FIELD) {
        return std::string{"meta.history_eligible is missing"};
    }
    if (seal_status != simdjson::SUCCESS) {
        return std::string{"meta.history_eligible is malformed"};
    }
    bool history_eligible = false;
    if (seal_element.get(history_eligible) != simdjson::SUCCESS) {
        return std::string{"meta.history_eligible is malformed"};
    }
    if (!history_eligible) {
        return std::string{"meta.history_eligible is false"};
    }

    for (const std::string_view finding_key :
         {std::string_view{"rule_violations"},
          std::string_view{"regressions"},
          std::string_view{"sanity_warnings"}}) {
        simdjson::dom::array findings;
        const auto findings_status =
            (*root_obj).at_key(finding_key).get(findings);
        if (findings_status == simdjson::NO_SUCH_FIELD) {
            return fmt::format("{} is missing", finding_key);
        }
        if (findings_status != simdjson::SUCCESS) {
            return fmt::format("{} is malformed", finding_key);
        }
    }
    return std::nullopt;
}

/// Read the aggregate field selected by one regression rule directly from
/// its persisted JSON object. Unlike the general report/UI parser, every
/// selected field is mandatory and finite on this authoritative path.
[[nodiscard]] inline std::optional<double>
strict_historical_aggregate_value(
    simdjson::dom::object metric_obj, sample_aggregate aggregate) noexcept
{
    if (aggregate == sample_aggregate::p01
        || aggregate == sample_aggregate::p50
        || aggregate == sample_aggregate::p99) {
        const auto percentiles = try_get_object(metric_obj, "percentiles");
        if (!percentiles.has_value()) return std::nullopt;
        switch (aggregate) {
            case sample_aggregate::p01:
                return strict_finite_json_number_field(*percentiles, "P01");
            case sample_aggregate::p50:
                return strict_finite_json_number_field(*percentiles, "P50");
            case sample_aggregate::p99:
                return strict_finite_json_number_field(*percentiles, "P99");
            default:
                return std::nullopt;
        }
    }

    const auto stats = try_get_object(metric_obj, "stats");
    if (!stats.has_value()) return std::nullopt;
    switch (aggregate) {
        case sample_aggregate::sum:
            return strict_finite_json_number_field(*stats, "sum");
        case sample_aggregate::min:
            return strict_finite_json_number_field(*stats, "min");
        case sample_aggregate::max:
            return strict_finite_json_number_field(*stats, "max");
        case sample_aggregate::p01:
        case sample_aggregate::p50:
        case sample_aggregate::p99:
            return std::nullopt;
    }
    return std::nullopt;
}

/// Sum the ten persisted histogram counts, rejecting malformed buckets and
/// arithmetic overflow. The result is the historical cell's finite-sample
/// cardinality and must match the current raw vector's finite count.
[[nodiscard]] inline std::optional<std::size_t>
strict_historical_finite_sample_count(
    simdjson::dom::object metric_obj) noexcept
{
    simdjson::dom::array histogram;
    if (metric_obj.at_key("histogram").get(histogram) != simdjson::SUCCESS
        || histogram.size() != k_histogram_bucket_count) {
        return std::nullopt;
    }
    std::size_t total = 0;
    for (const auto bucket_element : histogram) {
        const auto bucket = try_as_object(bucket_element);
        if (!bucket.has_value()) return std::nullopt;
        const auto count = strict_json_size_field(*bucket, "count");
        if (!count.has_value()
            || *count > (std::numeric_limits<std::size_t>::max)() - total) {
            return std::nullopt;
        }
        total += *count;
    }
    return total;
}

/// One accepted historical comparison point. The report pointer is pinned by
/// `historical_report_cache`; `aggregate_value` is the strictly parsed statistic
/// selected by the active regression rule.
struct historical_regression_candidate {
    parsed_historical_report* report = nullptr;
    double aggregate_value = 0.0;
};

/// Cached result for one report index. `candidate` is populated only when all
/// eligibility gates pass; otherwise `reason` is suitable for the required
/// non-blocking skipped-candidate diagnostic.
struct historical_regression_candidate_assessment {
    std::optional<historical_regression_candidate> candidate{};
    std::string reason{};
};

/// Per-current-cell historical candidate selector. Baseline selection and the
/// `regression_since` walk share this exact instance, so they cannot disagree
/// about completeness, mode, tuple identity, or sample topology. Assessments
/// are cached by report index to avoid re-walking DOM trees.
struct historical_regression_candidate_selector {
    historical_report_cache& reports;
    bool current_fast = false;
    std::string_view benchmark;
    std::string_view algorithm_name;
    const parameter_map& params;
    std::string_view test_case;
    std::string_view region;
    std::string_view metric;
    sample_aggregate aggregate = sample_aggregate::sum;
    std::size_t expected_sample_count = 0;
    std::size_t expected_finite_count = 0;
    std::map<std::size_t, historical_regression_candidate_assessment> cached{};

    /// Assess `report_index` once and return the cached result thereafter.
    [[nodiscard]] const historical_regression_candidate_assessment& assess(
        std::size_t report_index) noexcept
    {
        if (const auto found = cached.find(report_index); found != cached.end()) {
            return found->second;
        }

        historical_regression_candidate_assessment assessment;
        auto* report = reports.fetch(report_index);
        if (report == nullptr) {
            assessment.reason = "report is missing or malformed";
        } else if (const auto historical_fast = fast_from_report(report->root);
                   !historical_fast.has_value()) {
            assessment.reason = "meta.fast is missing or malformed";
        } else if (*historical_fast != current_fast) {
            assessment.reason = fmt::format(
                "meta.fast={} does not match current meta.fast={}",
                *historical_fast ? "true" : "false",
                current_fast ? "true" : "false");
        } else if (const auto outcome_reason =
                       historical_run_outcome_ineligibility_reason(report->root);
                   outcome_reason.has_value()) {
            assessment.reason = *outcome_reason;
        } else if (const auto raw_reason =
                       historical_raw_evidence_ineligibility_reason(report->root);
                   raw_reason.has_value()) {
            assessment.reason = *raw_reason;
        } else {
            const auto metric_obj = aggregated_metric_object_from_report(
                report->root, benchmark, algorithm_name, params,
                test_case, region, metric);
            if (!metric_obj.has_value()) {
                assessment.reason = "matching tuple/case/region/metric is missing";
            } else {
                const auto sample_count =
                    strict_json_size_field(*metric_obj, "samples_count");
                const auto finite_count =
                    strict_historical_finite_sample_count(*metric_obj);
                const auto selected_value =
                    strict_historical_aggregate_value(*metric_obj, aggregate);
                if (!sample_count.has_value()) {
                    assessment.reason = "samples_count is missing or malformed";
                } else if (*sample_count != expected_sample_count) {
                    assessment.reason = fmt::format(
                        "sample cardinality {} does not match current cardinality {}",
                        *sample_count, expected_sample_count);
                } else if (!finite_count.has_value()) {
                    assessment.reason = "histogram finite count is malformed";
                } else if (*finite_count != expected_finite_count) {
                    assessment.reason = fmt::format(
                        "finite histogram count {} does not match current finite count {}",
                        *finite_count, expected_finite_count);
                } else if (!selected_value.has_value()) {
                    assessment.reason = fmt::format(
                        "selected {} aggregate is missing, malformed, or non-finite",
                        aggregate_name(aggregate));
                } else {
                    assessment.candidate = historical_regression_candidate{
                        .report = report,
                        .aggregate_value = *selected_value,
                    };
                }
            }
        }

        return cached.emplace(report_index, std::move(assessment))
            .first->second;
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
/// shape*. Returns an empty set when the metric was absent in that
/// report (or when the report's structure is unparseable). Linear
/// in the report size; called at most once per `(metric,
/// hist_report)` pair via the per-rule cache in
/// `evaluate_sanity_warnings`.
///
/// The aggregate per cell carries `min` and `max` (over the finite
/// subset) plus an empty-vs-non-empty `top10`. From these we can
/// vector walk:
///
///   * `samples_count == 0` → cell is empty, contributes nothing.
///   * `top10` empty (`samples_count > 0`) → every emitted sample
///     was non-finite. Insert a NaN sentinel so an all-NaN run
///     reports as `metric_looks_constant` with a NaN-flavoured
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
                        aggregated_stat_name(&aggregate_statistics::min), 0.0);
                    const double max_value = json_double_or(*stats_obj,
                        aggregated_stat_name(&aggregate_statistics::max), 0.0);
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
[[nodiscard]] inline std::vector<sanity_warning> evaluate_sanity_warnings(
    const std::map<std::string, std::shared_ptr<benchmark_execution_state>>& reports,
    const report_metadata&            current_meta,
    std::size_t                  current_index,
    historical_report_cache&       history_cache) noexcept
{
    // 1. Collect distinct bit-patterns per metric name across every
    //    cell of the current run. We funnel through
    //    `aggregate_samples_for_report` so the constancy lens
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

    std::vector<sanity_warning> warnings;
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

        sanity_warning warning;
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
/// shared `historical_report_cache` is owned by the caller so the
/// sanity-warning pass can reuse the parsed report bodies.
[[nodiscard]] inline std::vector<regression_entry> evaluate_regressions(
    const regression_rules_by_configuration& regression_rules_by_config,
    const std::map<std::string, std::shared_ptr<benchmark_execution_state>>& reports,
    const report_metadata&              current_meta,
    std::size_t                    current_index,
    historical_report_cache&         history_cache,
    std::vector<validation_diagnostic>&   diagnostics_out,
    bool                           selection_is_partial) noexcept
{
    std::vector<regression_entry> entries;

    // Per-state merged-samples cache built once per `(config_path,
    // benchmark)` and reused across every rule that touches the same state,
    // keeping the work at O(N_cells) rather than O(N_rules × N_cells).
    std::map<const benchmark_execution_state*, merged_samples_by_pair> merged_cache;
    auto merged_for = [&](const benchmark_execution_state* state)
        -> const merged_samples_by_pair&
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

            std::vector<const benchmark_execution_state*> matching_states;
            const std::set<std::string> excluded_benchmarks(
                rule.exclude_benchmarks.begin(), rule.exclude_benchmarks.end());
            for (const auto& [_bench_key, state_ptr] : reports) {
                if (state_ptr->config.config_path != config_path) continue;
                if (excluded_benchmarks.contains(
                        state_ptr->config.benchmark_key)) continue;
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
            const std::set<std::string> excluded_algorithms(
                rule.exclude_algorithms.begin(),
                rule.exclude_algorithms.end());

            const std::string rule_location =
                fmt::format("{}/regression_rules/{}", config_path, rule.name);

            // A targeted selection legitimately omits whole rule partitions.
            // Skip only when the selected states contain no statically
            // applicable cell for this rule. Once a selected cell is in
            // scope, the runtime matched-nothing diagnostic remains
            // fail-closed for missing regions/samples.
            const auto selected_state_has_rule_scope = [
                &rule,
                &excluded_test_cases,
                &excluded_algorithms
            ](const benchmark_execution_state* state) noexcept {
                const bool metric_declared = std::any_of(
                    state->config.metrics.begin(), state->config.metrics.end(),
                    [&rule](const metric_specification& metric) {
                        return metric.name == rule.metric;
                    });
                const bool algorithm_remains = std::any_of(
                    state->execution_tuples.begin(),
                    state->execution_tuples.end(),
                    [&excluded_algorithms](const execution_tuple& tuple) {
                        return !excluded_algorithms.contains(
                            tuple.algorithm_name);
                    });
                const bool test_case_remains = std::any_of(
                    state->config.for_each.begin(),
                    state->config.for_each.end(),
                    [&excluded_test_cases](const auto& test_case) {
                        return !excluded_test_cases.contains(test_case.first);
                    });
                return metric_declared && algorithm_remains
                    && test_case_remains;
            };
            if (selection_is_partial
                && std::none_of(
                    matching_states.begin(), matching_states.end(),
                    selected_state_has_rule_scope)) {
                continue;
            }

            for (const auto* state : matching_states) {
                const auto& merged_index = merged_for(state);
                for (const auto& tuple : state->execution_tuples) {
                    if (excluded_algorithms.contains(
                            tuple.algorithm_name)) continue;
                    for (const auto& [test_case_name, _iter] : state->config.for_each) {
                        if (excluded_test_cases.count(test_case_name) > 0) continue;
                        for (const auto& region_name : rule.regions) {
                            const auto merged_it = merged_index.find(
                                std::pair{&tuple, test_case_name});
                            if (merged_it == merged_index.end()) continue;
                            const auto& merged_samples = merged_it->second;

                            const auto region_it = merged_samples.find(region_name);
                            if (region_it == merged_samples.end()) continue;
                            const bool not_measured_cell =
                                region_has_nonfinite_evaluation_sentinel(region_it->second);

                            const auto metric_it = region_it->second.find(rule.metric);
                            if (metric_it == region_it->second.end()
                                || metric_it->second.empty()) continue;

                            // measurement_cell has data → counts toward matched_count
                            // regardless of baseline presence. All-non-finite
                            // current samples mean "not measured" (for example,
                            // explicit Vulkan rows on a software ICD), so the
                            // regression rule is satisfied for this cell.
                            ++matched_count;
                            if (not_measured_cell
                                || !has_any_finite_sample(metric_it->second)) {
                                continue;
                            }
                            const double now_value =
                                aggregate_samples(rule.aggregate, metric_it->second);

                            if (current_index <= 1) {
                                diagnostics_out.push_back({
                                    validation_diagnostic::diagnostic_severity::warning,
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

                            const auto current_finite_count =
                                static_cast<std::size_t>(std::count_if(
                                    metric_it->second.begin(),
                                    metric_it->second.end(),
                                    [](double sample) noexcept {
                                        return std::isfinite(sample);
                                    }));
                            historical_regression_candidate_selector candidates{
                                .reports = history_cache,
                                .current_fast = current_meta.fast,
                                .benchmark = state->config.benchmark_key,
                                .algorithm_name = tuple.algorithm_name,
                                .params = tuple.params,
                                .test_case = test_case_name,
                                .region = region_name,
                                .metric = rule.metric,
                                .aggregate = rule.aggregate,
                                .expected_sample_count = metric_it->second.size(),
                                .expected_finite_count = current_finite_count,
                            };
                            auto diagnose_skipped_candidate = [
                                &diagnostics_out,
                                &rule_location,
                                state,
                                &tuple,
                                &test_case_name,
                                &region_name,
                                &rule](std::size_t report_index,
                                      std::string_view reason) {
                                diagnostics_out.push_back({
                                    validation_diagnostic::diagnostic_severity::warning,
                                    "regression_history_candidate_skipped",
                                    rule_location,
                                    fmt::format(
                                        "skipped report_{}.json as a regression-history candidate for benchmark '{}', tuple algo '{}', test_case '{}', region '{}', metric '{}': {}",
                                        report_index,
                                        state->config.benchmark_key,
                                        tuple.algorithm_name,
                                        test_case_name, region_name,
                                        rule.metric, reason)
                                });
                            };

                            const historical_regression_candidate* baseline = nullptr;
                            std::size_t baseline_index = 0;
                            for (std::size_t historical_index = current_index - 1;;
                                 --historical_index) {
                                const auto& assessment =
                                    candidates.assess(historical_index);
                                if (assessment.candidate.has_value()) {
                                    baseline = &*assessment.candidate;
                                    baseline_index = historical_index;
                                    break;
                                }
                                diagnose_skipped_candidate(
                                    historical_index, assessment.reason);
                                if (historical_index == 1) break;
                            }
                            if (baseline == nullptr) {
                                diagnostics_out.push_back({
                                    validation_diagnostic::diagnostic_severity::warning,
                                    "regression_baseline_missing",
                                    rule_location,
                                    fmt::format("no eligible historical baseline exists before report_{}.json for benchmark '{}', tuple algo '{}', test_case '{}', region '{}', metric '{}'; rule treated as satisfied",
                                                current_index,
                                                state->config.benchmark_key,
                                                tuple.algorithm_name,
                                                test_case_name, region_name,
                                                rule.metric)
                                });
                                continue;
                            }

                            const double previously_value = baseline->aggregate_value;

                            if (comparator_passes(rule.op, now_value, previously_value)) {
                                continue;     // no regression
                            }

                            // Walk back through the same cached selector to
                            // compute regression_since. Ineligible reports are
                            // evidence gaps, never attribution origins.
                            std::size_t oldest_failing = baseline_index;
                            if (baseline_index > 1) {
                                for (std::size_t historical_index = baseline_index - 1;;
                                     --historical_index) {
                                    const auto& assessment =
                                        candidates.assess(historical_index);
                                    if (!assessment.candidate.has_value()) {
                                        diagnose_skipped_candidate(
                                            historical_index, assessment.reason);
                                    } else if (comparator_passes(
                                                   rule.op, now_value,
                                                   assessment.candidate->aggregate_value)) {
                                        break;     // found an eligible passing report
                                    } else {
                                        oldest_failing = historical_index;
                                    }
                                    if (historical_index == 1) break;
                                }
                            }

                            std::string regression_since_label;
                            std::string regression_since_commit;
                            regression_since_label =
                                fmt::format("report_{}.json", oldest_failing);
                            const auto& origin = candidates.assess(oldest_failing);
                            DEBUG_ASSERT(origin.candidate.has_value(),
                                "regression_since origin must be an eligible candidate");
                            if (origin.candidate.has_value()) {
                                regression_since_commit = commit_from_report(
                                    origin.candidate->report->root);
                            }
                            if (oldest_failing == 1) {
                                diagnostics_out.push_back({
                                    validation_diagnostic::diagnostic_severity::warning,
                                    "regression_pre_existing",
                                    rule_location,
                                    fmt::format("regression has been present in every eligible report on disk back to report_1.json for benchmark '{}', tuple algo '{}', test_case '{}', region '{}', metric '{}' — almost certainly not introduced by recent changes",
                                                state->config.benchmark_key,
                                                tuple.algorithm_name,
                                                test_case_name, region_name,
                                                rule.metric)
                                });
                            }

                            regression_entry entry;
                            entry.config_path     = config_path;
                            entry.benchmark       = state->config.benchmark_key;
                            entry.rule_name       = rule.name;
                            entry.algorithm_name  = tuple.algorithm_name;
                            entry.violating_params = tuple.params;
                            entry.test_case       = test_case_name;
                            entry.region          = region_name;
                            entry.metric          = rule.metric;
                            entry.aggregate       = rule.aggregate;
                            entry.blocking        = rule.blocking;
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
                    validation_diagnostic::diagnostic_severity::error,
                    "regression_rule_matched_nothing",
                    rule_location,
                    fmt::format("regression rule '{}' did not match any (benchmark, tuple, test_case, region) combination with current samples; check benchmark/algorithm filters, 'regions', 'metric', and 'exclude_cases'",
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

/// Emit one config_value scalar — switches on the runtime kind. Caller is
/// expected to have written the key + colon already.
inline void emit_value_scalar(simdjson::builder::string_builder& builder,
                                const config_value& value)
{
    switch (value.kind) {
        case config_value::value_kind::string:
            builder.escape_and_append_with_quotes(std::string_view{value.string_value});
            return;
        case config_value::value_kind::integer:
            builder.append(value.integer_value);
            return;
        case config_value::value_kind::floating_point:
            builder.append(value.double_value);
            return;
        case config_value::value_kind::boolean:
            builder.append(value.boolean_value);
            return;
        case config_value::value_kind::non_scalar:
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
template<typename p_body_function>
inline void emit_kv_with_body(simdjson::builder::string_builder& builder,
                                std::string_view key, p_body_function&& body_fn)
{
    emit_key(builder, key);
    body_fn(builder);
}

/// Emit a `{...}` object whose contents come from `body`.
template<typename p_body_function>
inline void emit_object_with_body(simdjson::builder::string_builder& builder,
                                    p_body_function&& body_fn)
{
    builder.start_object();
    body_fn(builder);
    builder.end_object();
}

/// Iterate a range and call `each(builder, item, is_first)` once per element,
/// writing commas between siblings. The element body is responsible for
/// emitting its own key + value (or whole object).
template<typename p_range, typename p_each_function>
inline void emit_range_with_commas(simdjson::builder::string_builder& builder,
                                    p_range&& range,
                                    p_each_function&& each_fn)
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
                               const parameter_map& params)
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

/// Emit one `(metric -> aggregated_samples)` leaf. Replaces the
/// `std::vector<double>` (so the rule evaluator and the rest of
/// the framework keep working unchanged), but the on-disk shape
/// is the fixed-size aggregate documented under
/// `k_bench_report_schema#/definitions/aggregated_samples`.
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
                for (const auto& entry : k_aggregated_stats_registry) {
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
                for (const auto& entry : k_aggregated_percentile_registry) {
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
                [](auto& bucket_builder, const aggregate_bucket& bucket) {
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

/// Emit the per-test-case object from a group-merged sample map: every region
/// (or just "all_regions" when by_regions=false) and the per-metric
/// aggregated_samples object inside it. Empty sample vectors are pruned (the
/// schema requires `samples_count >= 1`). Lifted out of `write_report`'s local
/// lambda so the report writer and the `--debug` CASE_RESULTS sink emit
/// byte-identical per-test-case blocks (one copy, no duplicated emit logic).
inline void emit_test_case_block(simdjson::builder::string_builder& region_block_builder,
                                 const sample_map& merged) {
    emit_object_with_body(region_block_builder, [&merged](auto& region_outer) {
        emit_range_with_commas(region_outer, merged,
            [](auto& region_builder, const auto& region_kv) {
                emit_kv_with_body(region_builder,
                    std::string_view{region_kv.first},
                    [&region_kv](auto& metrics_builder) {
                        emit_object_with_body(metrics_builder, [&region_kv](auto& metric_outer) {
                            bool first_metric = true;
                            for (const auto& metric_kv : region_kv.second) {
                                if (metric_kv.second.empty()) continue;
                                if (!first_metric) metric_outer.append_comma();
                                first_metric = false;
                                emit_kv_with_body(metric_outer,
                                    std::string_view{metric_kv.first},
                                    [&metric_kv](auto& aggregate_builder) {
                                        emit_aggregated_samples(
                                            aggregate_builder, metric_kv.second);
                                    });
                            }
                        });
                    });
            });
    });
}

/// Emit one execution_tuple_record: `{ "tuple": {...}, "test_cases": {...} }`.
/// `merged` is the (tuple,test_case)->sample_map index. Lifted out of
/// `write_report`'s local lambda (merged-index parameterised) so the report
/// writer and the `--debug` CASE_RESULTS sink share one serializer.
inline void emit_execution_tuple_record(simdjson::builder::string_builder& builder,
                                        const benchmark_execution_state& state,
                                        const execution_tuple& tuple,
                                        const merged_samples_by_pair& merged) {
    emit_object_with_body(builder, [&state, &tuple, &merged](auto& outer) {
        emit_kv_with_body(outer, "tuple", [&tuple](auto& tuple_b) {
            emit_object_with_body(tuple_b, [&tuple](auto& alg_b) {
                emit_key(alg_b, std::string_view{tuple.algorithm_name});
                emit_param_object(alg_b, tuple.params);
            });
        });
        outer.append_comma();
        emit_kv_with_body(outer, "test_cases", [&state, &tuple, &merged](auto& cases_b) {
            emit_object_with_body(cases_b, [&state, &tuple, &merged](auto& cases_inner) {
                bool first_tc_in_tuple = true;
                for (const auto& [test_case_name, _iter] : state.config.for_each) {
                    const auto pair_it = merged.find({&tuple, test_case_name});
                    if (pair_it == merged.end()) continue;
                    // Skip if every group produced no samples (e.g.
                    // matrix.for declared zero iterations).
                    if (pair_it->second.empty()) continue;
                    if (!first_tc_in_tuple) cases_inner.append_comma();
                    first_tc_in_tuple = false;
                    emit_kv_with_body(cases_inner,
                        std::string_view{test_case_name},
                        [&pair_it](auto& inner_b) {
                            emit_test_case_block(inner_b, pair_it->second);
                        });
                }
            });
        });
    });
}

/// Emit a finite floating-point value or JSON null when the evaluator marked
/// the value unavailable/non-finite. Reports therefore remain valid JSON while
/// preserving fail-closed availability evidence.
inline void emit_paired_finite_or_null(
    simdjson::builder::string_builder& builder,
    const bool present,
    const double value)
{
    if (present && std::isfinite(value)) {
        builder.append(value);
    } else {
        builder.append_null();
    }
}

/// Emit every raw descriptor sample in one logical repeat. A non-finite source
/// is represented by null and separately causes the rule input to be absent.
inline void emit_paired_descriptor_samples(
    simdjson::builder::string_builder& builder,
    const std::vector<double>& samples)
{
    builder.start_array();
    bool first = true;
    for (const double sample : samples) {
        if (!first) builder.append_comma();
        first = false;
        emit_paired_finite_or_null(
            builder, std::isfinite(sample), sample);
    }
    builder.end_array();
}

/// Emit one resolved numerator/denominator route identity.
inline void emit_paired_rule_route(
    simdjson::builder::string_builder& builder,
    const paired_rule_route_evidence& route)
{
    emit_object_with_body(builder, [&route](auto& inner) {
        emit_kv_string(inner, "algorithm", route.algorithm);
        inner.append_comma();
        emit_kv_with_body(inner, "params", [&route](auto& params_builder) {
            emit_param_object(params_builder, route.params);
        });
        inner.append_comma();
        emit_key(inner, "tuple_index");
        inner.append(route.tuple_index);
        inner.append_comma();
        emit_kv_string(inner, "test_case", route.test_case);
        inner.append_comma();
        emit_kv_string(inner, "region", route.region);
        inner.append_comma();
        emit_kv_string(inner, "metric", route.metric);
    });
}

/// Emit an actually observed scheduler coordinate, or null if the cell did not
/// retain one. Seeds are fixed-width hexadecimal strings to avoid JSON-number
/// precision loss in independent JavaScript readers.
inline void emit_paired_schedule_sample(
    simdjson::builder::string_builder& builder,
    const bool present,
    const iteration_schedule_sample& schedule)
{
    if (!present) {
        builder.append_null();
        return;
    }
    emit_object_with_body(builder, [&schedule](auto& inner) {
        emit_key(inner, "repeat");
        inner.append(schedule.repeat);
        inner.append_comma();
        emit_key(inner, "order_position");
        inner.append(schedule.order_position);
        inner.append_comma();
        emit_kv_string(
            inner, "shuffle_seed",
            fmt::format("{:016x}", schedule.shuffle_seed));
    });
}

/// Serialize one ratio-paired gate. This exact serializer is used by the
/// final report and by each PAIRED_RULE_EVIDENCE debug event.
inline void emit_paired_rule_evidence_record(
    simdjson::builder::string_builder& builder,
    const paired_rule_evidence& evidence)
{
    std::string_view operator_text = "=";
    switch (evidence.comparator.op) {
        case comparison_operator::less_than: operator_text = "<"; break;
        case comparison_operator::greater_than: operator_text = ">"; break;
        case comparison_operator::equal: operator_text = "="; break;
    }
    emit_object_with_body(builder,
        [&evidence, operator_text](auto& inner) {
            emit_kv_string(inner, "location", evidence.config_path);
            inner.append_comma();
            emit_kv_string(inner, "benchmark", evidence.benchmark_key);
            inner.append_comma();
            emit_kv_string(inner, "rule_name", evidence.rule_name);
            inner.append_comma();
            emit_kv_string(inner, "test_case", evidence.test_case);
            inner.append_comma();
            emit_kv_string(inner, "metric", evidence.metric);
            inner.append_comma();
            emit_kv_string(
                inner, "comparison_mode", "paired_iteration_ratio");
            inner.append_comma();
            emit_kv_with_body(
                inner, "numerator_route", [&evidence](auto& route_builder) {
                    emit_paired_rule_route(
                        route_builder, evidence.numerator_route);
                });
            inner.append_comma();
            emit_kv_with_body(
                inner, "denominator_route", [&evidence](auto& route_builder) {
                    emit_paired_rule_route(
                        route_builder, evidence.denominator_route);
                });
            inner.append_comma();
            emit_kv_with_body(
                inner, "comparator",
                [&evidence, operator_text](auto& comparator_builder) {
                    emit_object_with_body(
                        comparator_builder,
                        [&evidence, operator_text](auto& comparator) {
                            emit_kv_string(
                                comparator, "operator", operator_text);
                            comparator.append_comma();
                            emit_key(comparator, "threshold_pct");
                            comparator.append(
                                evidence.comparator.threshold_pct);
                            comparator.append_comma();
                            emit_key(comparator, "tolerance_pct");
                            comparator.append(
                                evidence.comparator.tolerance_pct);
                            comparator.append_comma();
                            emit_kv_string(
                                comparator, "aggregate",
                                aggregate_name(evidence.comparator.aggregate));
                            comparator.append_comma();
                            emit_key(comparator, "paired_baseline");
                            comparator.append(
                                evidence.comparator.paired_baseline);
                            comparator.append_comma();
                            emit_key(comparator, "warmup_repetitions");
                            comparator.append(
                                evidence.comparator.warmup_repetitions);
                            comparator.append_comma();
                            emit_key(comparator, "blocking");
                            comparator.append(evidence.comparator.blocking);
                        });
                });
            inner.append_comma();
            emit_kv_with_body(
                inner, "schedule_identity", [&evidence](auto& schedule_builder) {
                    emit_object_with_body(
                        schedule_builder, [&evidence](auto& schedule) {
                            emit_kv_string(
                                schedule, "mode", evidence.schedule_mode);
                            schedule.append_comma();
                            emit_kv_string(
                                schedule, "configured_seed",
                                fmt::format(
                                    "{:016x}",
                                    evidence.configured_schedule_seed));
                            schedule.append_comma();
                            emit_key(schedule, "benchmark_index");
                            schedule.append(evidence.benchmark_index);
                            schedule.append_comma();
                            emit_key(schedule, "test_case_index");
                            schedule.append(evidence.test_case_index);
                            schedule.append_comma();
                            emit_key(schedule, "group_index");
                            schedule.append(evidence.group_index);
                        });
                });
            inner.append_comma();
            emit_kv_with_body(inner, "repeats", [&evidence](auto& repeats) {
                repeats.start_array();
                bool first_repeat = true;
                for (const auto& repeat : evidence.repeats) {
                    if (!first_repeat) repeats.append_comma();
                    first_repeat = false;
                    emit_object_with_body(repeats, [&repeat](auto& item) {
                        emit_key(item, "repeat");
                        item.append(repeat.repeat);
                        item.append_comma();
                        emit_key(item, "included_after_warmup");
                        item.append(repeat.included_after_warmup);
                        item.append_comma();
                        emit_kv_with_body(
                            item, "numerator_samples",
                            [&repeat](auto& samples) {
                                emit_paired_descriptor_samples(
                                    samples, repeat.numerator_samples);
                            });
                        item.append_comma();
                        emit_kv_with_body(
                            item, "denominator_samples",
                            [&repeat](auto& samples) {
                                emit_paired_descriptor_samples(
                                    samples, repeat.denominator_samples);
                            });
                        item.append_comma();
                        emit_key(item, "numerator_value");
                        emit_paired_finite_or_null(
                            item, repeat.numerator_value_present,
                            repeat.numerator_value);
                        item.append_comma();
                        emit_key(item, "denominator_value");
                        emit_paired_finite_or_null(
                            item, repeat.denominator_value_present,
                            repeat.denominator_value);
                        item.append_comma();
                        emit_key(item, "ratio");
                        emit_paired_finite_or_null(
                            item, repeat.ratio_present, repeat.ratio);
                        item.append_comma();
                        emit_kv_with_body(
                            item, "numerator_schedule",
                            [&repeat](auto& schedule) {
                                emit_paired_schedule_sample(
                                    schedule,
                                    repeat.numerator_schedule_present,
                                    repeat.numerator_schedule);
                            });
                        item.append_comma();
                        emit_kv_with_body(
                            item, "denominator_schedule",
                            [&repeat](auto& schedule) {
                                emit_paired_schedule_sample(
                                    schedule,
                                    repeat.denominator_schedule_present,
                                    repeat.denominator_schedule);
                            });
                        item.append_comma();
                        emit_key(item, "schedule_identity_matches");
                        item.append(repeat.schedule_identity_matches);
                    });
                }
                repeats.end_array();
            });
            inner.append_comma();
            emit_kv_with_body(
                inner, "inputs_present", [&evidence](auto& present) {
                    present.start_array();
                    present.append(evidence.inputs_present[0]);
                    present.append_comma();
                    present.append(evidence.inputs_present[1]);
                    present.end_array();
                });
            inner.append_comma();
            emit_key(inner, "observed_paired_ratio");
            emit_paired_finite_or_null(
                inner, evidence.observed_paired_ratio_present,
                evidence.observed_paired_ratio);
            inner.append_comma();
            emit_key(inner, "accepted");
            inner.append(evidence.accepted);
        });
}

// ----------------- --debug JSONL event sink -----------------

/// Append-only, crash-resilient JSONL event sink for `--debug`. One instance
/// per run, owned by run_impl; a non-owning pointer is threaded through
/// benchmark_context::debug_sink. Every emit_* builds one line with the existing
/// simdjson string_builder helpers and appends it under `mu_` + flush, so a
/// process crash preserves all already-emitted events. `timestamp` is Unix
/// epoch milliseconds (sub-second ordering within a run).
class debug_report_sink {
public:
    explicit debug_report_sink(std::filesystem::path path) : path_(std::move(path)) {
        if (path_.has_parent_path())
            std::filesystem::create_directories(path_.parent_path());
        out_.open(path_, std::ios::app | std::ios::binary);
        if (!out_) bioseq_log<logging_level::error>("[bench_framework] --debug: cannot open '{}'", path_.string());
    }
    [[nodiscard]] bool ok() const noexcept { return static_cast<bool>(out_); }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    void emit_benchmark_started(std::string_view benchmark) {
        simdjson::builder::string_builder b;
        emit_object_with_body(b, [&benchmark](auto& o){
            emit_kv_string(o, "event", "BENCHMARK_STARTED"); o.append_comma();
            emit_key(o, "timestamp"); o.append(now_unix_millis()); o.append_comma();
            emit_kv_string(o, "benchmark_name", benchmark);
        });
        write_line_(b);
    }

    void emit_case_started(std::string_view benchmark, std::string_view case_name) {
        simdjson::builder::string_builder b;
        emit_object_with_body(b, [&benchmark, &case_name](auto& o){
            emit_kv_string(o, "event", "CASE_STARTED"); o.append_comma();
            emit_key(o, "timestamp"); o.append(now_unix_millis()); o.append_comma();
            emit_kv_string(o, "benchmark_name", benchmark); o.append_comma();
            emit_kv_string(o, "case_name", case_name);
        });
        write_line_(b);
    }

    void emit_case_results(std::string_view benchmark, std::string_view case_name,
                           const benchmark_execution_state& state) {
        const auto merged = merge_case_samples(state, case_name);   // §4.5 (immutable cells)
        simdjson::builder::string_builder b;
        emit_object_with_body(b, [&benchmark, &case_name, &state, &merged](auto& o){
            emit_kv_string(o, "event", "CASE_RESULTS"); o.append_comma();
            emit_key(o, "timestamp"); o.append(now_unix_millis()); o.append_comma();
            emit_kv_string(o, "benchmark_name", benchmark); o.append_comma();
            emit_kv_string(o, "case_name", case_name); o.append_comma();
            emit_kv_with_body(o, "results", [&state, &case_name, &merged](auto& rb){
                emit_object_with_body(rb, [&state, &case_name, &merged](auto& ri){
                    emit_kv_with_body(ri, "execution_tuples", [&state, &case_name, &merged](auto& arr){
                        arr.start_array();
                        bool first = true;
                        for (const auto& tuple : state.execution_tuples) {
                            if (tuple.algorithm_name != case_name) continue;
                            if (!first) arr.append_comma();
                            first = false;
                            emit_execution_tuple_record(arr, state, tuple, merged);
                        }
                        arr.end_array();
                    });
                });
            });
        });
        write_line_(b);
    }

    /// Persist one complete ratio-paired evaluation immediately after
    /// the benchmark finishes. write_line_ flushes the independent JSON object
    /// before the final report is assembled, preserving the evidence on crash.
    void emit_paired_rule_evidence(const paired_rule_evidence& evidence) {
        simdjson::builder::string_builder builder;
        emit_object_with_body(builder, [&evidence](auto& object) {
            emit_kv_string(
                object, "event", "PAIRED_RULE_EVIDENCE");
            object.append_comma();
            emit_key(object, "timestamp");
            object.append(now_unix_millis());
            object.append_comma();
            emit_kv_with_body(
                object, "evidence", [&evidence](auto& evidence_builder) {
                    emit_paired_rule_evidence_record(
                        evidence_builder, evidence);
                });
        });
        write_line_(builder);
    }

    void emit_execution_finished(std::string_view results_path) {
        simdjson::builder::string_builder b;
        emit_object_with_body(b, [&results_path](auto& o){
            emit_kv_string(o, "event", "EXECUTION_FINISHED"); o.append_comma();
            emit_key(o, "timestamp"); o.append(now_unix_millis()); o.append_comma();
            emit_kv_string_or_null(o, "results_path", results_path);  // null if empty
        });
        write_line_(b);
    }

private:
    void write_line_(simdjson::builder::string_builder& b) {
        std::string_view payload;
        if (b.view().get(payload) != simdjson::SUCCESS) {
            bioseq_log<logging_level::error>("[bench_framework] --debug: string_builder allocation failure");
            return;
        }
        std::lock_guard<std::mutex> guard(mu_);
        if (!out_) return;
        out_.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        out_.put('\n');
        out_.flush();                 // push to OS: survives a process crash (not power loss)
    }

    std::filesystem::path path_;
    std::ofstream          out_;
    std::mutex             mu_;
};

[[nodiscard]] inline std::string raw_hex64(std::uint64_t value) {
    return fmt::format("{:016x}", value);
}

[[nodiscard]] inline std::string raw_record_checksum(
        nlohmann::json record) {
    record.erase("record_checksum");
    return raw_hex64(raw_fnv1a64_update(
        14695981039346656037ULL, record.dump()));
}

[[nodiscard]] inline std::optional<std::uint64_t> raw_json_u64(
        const nlohmann::json& object, std::string_view key) noexcept {
    const auto found = object.find(std::string{key});
    if (found == object.end()) return std::nullopt;
    if (found->is_number_unsigned()) return found->get<std::uint64_t>();
    if (found->is_number_integer()) {
        const auto value = found->get<std::int64_t>();
        if (value >= 0) return static_cast<std::uint64_t>(value);
    }
    return std::nullopt;
}

struct raw_timing_disposition {
    bool e2e = false;
    bool framework_timing_eligible = true;
};

/// Classify only the current E2E raw payload contract. Non-E2E payloads keep
/// ordinary benchmark semantics. Once the E2E schema is claimed, malformed or
/// contradictory acceptance evidence fails closed instead of defaulting to a
/// numeric timing sample.
[[nodiscard]] inline std::optional<raw_timing_disposition>
classify_raw_timing_disposition(const nlohmann::json& record) noexcept {
    const auto payload_schema = record.find("payload_schema");
    if (payload_schema == record.end()
        || !payload_schema->is_string()
        || *payload_schema != "bioseq.e2e.raw-sample") {
        return raw_timing_disposition{};
    }
    const auto payload_version = raw_json_u64(
        record, "payload_schema_version");
    const auto status = record.find("tuple_status");
    if (!payload_version.has_value()
        || (*payload_version != e2e_raw_payload_schema_version - 1u
            && *payload_version != e2e_raw_payload_schema_version)
        || status == record.end() || !status->is_object()) {
        return std::nullopt;
    }
    const auto accepted = raw_json_u64(*status, "accepted");
    const auto disposition = status->find("disposition");
    if (!accepted.has_value() || *accepted > 1u
        || disposition == status->end() || !disposition->is_string()) {
        return std::nullopt;
    }
    constexpr std::array<std::string_view, 3> accepted_dispositions{
        "accepted", "accepted_fallback", "accepted_behavior_difference",
    };
    constexpr std::array<std::string_view, 6> rejected_dispositions{
        "rejected_setup", "rejected_sql", "invalid",
        "rejected_attestation", "rejected_correctness",
        "skipped_after_prior_attestation_rejection",
    };
    const std::string_view disposition_text =
        disposition->get_ref<const std::string&>();
    const auto contains = [disposition_text](const auto& values) noexcept {
        return std::find(values.begin(), values.end(), disposition_text)
            != values.end();
    };
    const bool eligible = *accepted == 1u;
    if ((eligible && !contains(accepted_dispositions))
        || (!eligible && !contains(rejected_dispositions))) {
        return std::nullopt;
    }
    return raw_timing_disposition{
        .e2e = true,
        .framework_timing_eligible = eligible,
    };
}

[[nodiscard]] inline nlohmann::json raw_duration_summary(
        std::vector<std::uint64_t> samples) {
    nlohmann::json result = nlohmann::json::object();
    result["count"] = samples.size();
    if (samples.empty()) {
        result["min"] = nullptr;
        result["p50"] = nullptr;
        result["mean"] = nullptr;
        result["p95"] = nullptr;
        return result;
    }
    std::sort(samples.begin(), samples.end());
    const auto percentile = [&samples](const std::size_t percent) {
        const std::size_t index =
            (percent * samples.size() + 99) / 100 - 1;
        return samples[index];
    };
    long double sum = 0;
    for (const auto sample : samples) sum += sample;
    result["min"] = samples.front();
    result["p50"] = percentile(50);
    result["mean"] = static_cast<double>(
        sum / static_cast<long double>(samples.size()));
    result["p95"] = percentile(95);
    return result;
}

[[nodiscard]] constexpr std::size_t raw_nearest_rank_index(
        std::size_t count, std::size_t percentile) noexcept {
    return count == 0 ? 0 : (percentile * count + 99) / 100 - 1;
}
static_assert(raw_nearest_rank_index(3, 50) == 1);
static_assert(raw_nearest_rank_index(4, 50) == 1);
static_assert(raw_nearest_rank_index(20, 95) == 18);

/// Power-loss-resilient raw evidence stream.  The mutex establishes the one
/// authoritative completion order; a measurement_cell is mutated only after the exact
/// line and fsync have succeeded.
class durable_raw_evidence_sink {
public:
    durable_raw_evidence_sink(
            std::filesystem::path path,
            std::string run_id)
        : path_(std::move(path)), run_id_(std::move(run_id)),
          io_fault_(raw_evidence_io_fault_for_testing.load(
              std::memory_order_acquire)) {
        std::error_code ec;
        if (path_.has_parent_path()) {
            std::filesystem::create_directories(path_.parent_path(), ec);
        }
        fd_ = ::open(path_.c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC,
            0600);
        if (fd_ < 0) {
            poisoned_ = true;
            bioseq_log<logging_level::error>(
                "[bench_framework] raw evidence O_EXCL open failed for '{}': {}",
                path_.string(), std::strerror(errno));
            return;
        }
        const nlohmann::json header{
            {"kind", "header"},
            {"schema", 1},
            {"run_id", run_id_},
        };
        header_line_ = header.dump();
        if (!append_line_locked(header_line_)) {
            poisoned_ = true;
        }
    }

    ~durable_raw_evidence_sink() noexcept {
        if (fd_ >= 0) (void)::close(fd_);
    }

    durable_raw_evidence_sink(const durable_raw_evidence_sink&) = delete;
    durable_raw_evidence_sink& operator=(const durable_raw_evidence_sink&) = delete;

    [[nodiscard]] bool ok() const noexcept {
        std::lock_guard<std::mutex> guard(mu_);
        return fd_ >= 0 && !poisoned_;
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

    [[nodiscard]] std::optional<std::string> append_sample(
            std::string_view unfinalized_record,
            const std::size_t benchmark_index,
            const std::size_t test_case_index,
            const std::size_t tuple_index,
            const std::size_t group_index,
            const std::size_t repeat,
            const std::string_view order_mode,
            const std::uint64_t configured_seed,
            const std::uint64_t shuffle_seed,
            const std::size_t order_position) {
        std::lock_guard<std::mutex> guard(mu_);
        if (poisoned_ || sealed_ || fd_ < 0) return std::nullopt;
        nlohmann::json record = nlohmann::json::parse(
            unfinalized_record, nullptr, false);
        if (record.is_discarded() || !record.is_object()
            || record.contains("raw_record_id")
            || record.contains("schedule")
            || record.contains("record_checksum")) {
            poisoned_ = true;
            return std::nullopt;
        }
        record["raw_record_id"] = next_record_id_;
        record["schedule"] = nlohmann::json{
            {"benchmark_index", benchmark_index},
            {"test_case_index", test_case_index},
            {"tuple_index", tuple_index},
            {"group_index", group_index},
            {"repeat", repeat},
            {"mode", order_mode},
            {"configured_seed", raw_hex64(configured_seed)},
            {"shuffle_seed", raw_hex64(shuffle_seed)},
            {"order_position", order_position},
        };
        record["record_checksum"] = raw_record_checksum(record);
        const std::string finalized = record.dump();
        if (!append_line_locked(finalized)) {
            poisoned_ = true;
            return std::nullopt;
        }
        records_.push_back(finalized);
        ++next_record_id_;
        return finalized;
    }

    [[nodiscard]] bool seal_and_validate(
        const std::map<std::string,
            std::shared_ptr<benchmark_execution_state>>& reports) {
        std::lock_guard<std::mutex> guard(mu_);
        if (sealed_) return manifest_.value("complete", false);
        bool valid = !poisoned_ && fd_ >= 0;
        std::map<std::uint64_t, std::string> embedded_by_id;
        std::vector<std::pair<std::string, std::string>> scheduled_checksums;
        std::vector<std::uint64_t> all_durations;
        std::vector<std::uint64_t> all_index_and_map_durations;
        using raw_batch_key = std::tuple<std::size_t, std::size_t, std::size_t>;
        using raw_order_entry =
            std::tuple<std::uint64_t, std::size_t, std::size_t,
                       std::size_t, std::uint64_t>;
        std::map<raw_batch_key, std::vector<raw_order_entry>> actual_order;
        nlohmann::json cell_summaries = nlohmann::json::array();

        for (const auto& [benchmark_key, state_ptr] : reports) {
            const auto& state = *state_ptr;
            if (!state.raw_samples_enabled) continue;
            for (const auto& cell_ptr : state.cells) {
                const measurement_cell& cell = *cell_ptr;
                const auto region = cell.samples.find("all_regions");
                const bool has_wall = region != cell.samples.end()
                    && region->second.contains("wall_clock_ns");
                const std::vector<double>* wall = has_wall
                    ? &region->second.at("wall_clock_ns") : nullptr;
                const bool has_measured_region = region != cell.samples.end()
                    && region->second.contains("measured_region_ns_manual");
                const std::vector<double>* measured_region = has_measured_region
                    ? &region->second.at("measured_region_ns_manual") : nullptr;
                const bool completion_evidence_valid =
                    cell.iteration_evidence.size() == cell.iterations
                    && std::all_of(
                        cell.iteration_evidence.begin(),
                        cell.iteration_evidence.end(),
                        [](const iteration_completion_evidence& evidence) {
                            return evidence.completed;
                        });
                const std::size_t timing_eligible_iterations =
                    completion_evidence_valid
                    ? static_cast<std::size_t>(std::count_if(
                        cell.iteration_evidence.begin(),
                        cell.iteration_evidence.end(),
                        [](const iteration_completion_evidence& evidence) {
                            return evidence.framework_timing_eligible;
                        }))
                    : 0;
                if (cell.raw_samples.size() != cell.iterations
                    || !completion_evidence_valid
                    || (timing_eligible_iterations == 0
                        ? (wall != nullptr && !wall->empty())
                        : (wall == nullptr
                            || wall->size() != timing_eligible_iterations))
                    || (measured_region != nullptr
                        && measured_region->size()
                            != timing_eligible_iterations)) {
                    valid = false;
                }
                std::vector<std::uint64_t> cell_durations;
                std::vector<std::uint64_t> cell_first_repeat_durations;
                std::vector<std::uint64_t> cell_steady_state_durations;
                std::vector<std::uint64_t> cell_index_and_map_durations;
                std::vector<std::string> cell_checksums;
                std::set<std::size_t> seen_repeats;
                std::size_t wall_sample_index = 0;
                std::size_t measured_region_sample_index = 0;
                for (const std::string& serialized : cell.raw_samples) {
                    nlohmann::json record = nlohmann::json::parse(
                        serialized, nullptr, false);
                    if (record.is_discarded() || !record.is_object()) {
                        valid = false;
                        continue;
                    }
                    const auto id = raw_json_u64(record, "raw_record_id");
                    const auto repeat = raw_json_u64(record, "repeat");
                    const auto duration = raw_json_u64(record, "outer_e2e_ns");
                    const auto published_duration = raw_json_u64(
                        record, "published_wall_clock_ns");
                    const auto index_and_map = raw_json_u64(
                        record, "index_and_map_ns");
                    const auto group = raw_json_u64(record, "group_index");
                    const auto tuple = raw_json_u64(record, "tuple_index");
                    const auto checksum_it = record.find("record_checksum");
                    const auto schedule_it = record.find("schedule");
                    const auto timing_disposition = record.is_discarded()
                        ? std::nullopt
                        : classify_raw_timing_disposition(record);
                    if (!id.has_value() || !repeat.has_value()
                        || !group.has_value()
                        || !tuple.has_value() || checksum_it == record.end()
                        || !checksum_it->is_string()
                        || schedule_it == record.end()
                        || !schedule_it->is_object()
                        || !timing_disposition.has_value()
                        || !record.contains("outer_e2e_ns")
                        || !record.contains("published_wall_clock_ns")
                        || (duration.has_value()
                            != timing_disposition
                                ->framework_timing_eligible)
                        || (published_duration.has_value()
                            != timing_disposition
                                ->framework_timing_eligible)
                        || (!duration.has_value()
                            && !record["outer_e2e_ns"].is_null())
                        || (!published_duration.has_value()
                            && !record["published_wall_clock_ns"].is_null())
                        || (duration.has_value()
                            && published_duration.has_value()
                            && *published_duration > *duration)) {
                        valid = false;
                        continue;
                    }
                    const auto schedule_benchmark = raw_json_u64(
                        *schedule_it, "benchmark_index");
                    const auto schedule_case = raw_json_u64(
                        *schedule_it, "test_case_index");
                    const auto schedule_tuple = raw_json_u64(
                        *schedule_it, "tuple_index");
                    const auto schedule_group = raw_json_u64(
                        *schedule_it, "group_index");
                    const auto schedule_repeat = raw_json_u64(
                        *schedule_it, "repeat");
                    const auto schedule_order = raw_json_u64(
                        *schedule_it, "order_position");
                    const auto shuffle_seed_it =
                        schedule_it->find("shuffle_seed");
                    const std::uint64_t expected_shuffle_seed =
                        raw_schedule_shuffle_seed(
                            state.config.iteration_order_seed,
                            state.config.benchmark_key,
                            cell.test_case->name,
                            cell.group_index,
                            static_cast<std::size_t>(*repeat));
                    const std::string expected_mode = state.config.iteration_order
                            == benchmark_configuration::iteration_order_mode::interleaved_seeded
                        ? "interleaved_seeded" : "item_major";
                    const bool identity_valid =
                        record.value("benchmark", std::string{}) == benchmark_key
                        && record.value("test_case", std::string{})
                            == cell.test_case->name
                        && record.value("algorithm", std::string{})
                            == cell.tuple->algorithm_name
                        && *group == cell.group_index
                        && *tuple == cell.tuple_index
                        && *repeat < cell.iterations
                        && schedule_benchmark.has_value()
                        && *schedule_benchmark
                            == state.driver_benchmark_index
                        && schedule_case == cell.test_case_index
                        && schedule_tuple == cell.tuple_index
                        && schedule_group == cell.group_index
                        && schedule_repeat == repeat
                        && schedule_order.has_value()
                        && *schedule_order < state.execution_tuples.size()
                        && schedule_it->value("mode", std::string{})
                            == expected_mode
                        && schedule_it->value(
                                "configured_seed", std::string{})
                            == raw_hex64(state.config.iteration_order_seed)
                        && shuffle_seed_it != schedule_it->end()
                        && shuffle_seed_it->is_string()
                        && shuffle_seed_it->get<std::string>()
                            == raw_hex64(expected_shuffle_seed)
                        && schedule_it->size() == 9
                        && checksum_it->get<std::string>()
                            == raw_record_checksum(record)
                        && seen_repeats.insert(
                            static_cast<std::size_t>(*repeat)).second;
                    const bool repeat_evidence_valid =
                        *repeat < cell.iteration_evidence.size()
                        && cell.iteration_evidence[*repeat].completed
                        && cell.iteration_evidence[*repeat]
                                .framework_timing_eligible
                            == timing_disposition
                                ->framework_timing_eligible;
                    if (!identity_valid || !repeat_evidence_valid) {
                        valid = false;
                    }
                    if (timing_disposition->framework_timing_eligible) {
                        if (wall == nullptr || !published_duration.has_value()
                            || wall_sample_index >= wall->size()
                            || (*wall)[wall_sample_index]
                                != static_cast<double>(*published_duration)) {
                            valid = false;
                        }
                        ++wall_sample_index;
                    }
                    if (schedule_benchmark.has_value()
                        && schedule_case.has_value()
                        && schedule_group.has_value()
                        && schedule_order.has_value()) {
                        actual_order[raw_batch_key{
                            static_cast<std::size_t>(*schedule_benchmark),
                            static_cast<std::size_t>(*schedule_case),
                            static_cast<std::size_t>(*schedule_group)}]
                            .emplace_back(
                                *id,
                                static_cast<std::size_t>(*repeat),
                                static_cast<std::size_t>(*schedule_order),
                                cell.tuple_index,
                                expected_shuffle_seed);
                    }
                    if (!timing_disposition->framework_timing_eligible) {
                        if (!record.contains("index_and_map_ns")
                            || !record["index_and_map_ns"].is_null()) {
                            valid = false;
                        }
                    } else {
                        if (index_and_map.has_value()
                                != (measured_region != nullptr)
                            || (index_and_map.has_value()
                                && (measured_region_sample_index
                                        >= measured_region->size()
                                    || (*measured_region)[
                                            measured_region_sample_index]
                                        != static_cast<double>(
                                            *index_and_map)))) {
                            valid = false;
                        }
                        if (index_and_map.has_value()) {
                            ++measured_region_sample_index;
                        }
                    }
                    if (!embedded_by_id.emplace(*id, serialized).second) {
                        valid = false;
                    }
                    const std::string checksum = checksum_it->get<std::string>();
                    const std::string schedule_key = fmt::format(
                        "{:020}:{:020}:{:020}:{:020}:{:020}",
                        schedule_benchmark.value_or(0),
                        schedule_case.value_or(0),
                        schedule_tuple.value_or(0),
                        schedule_group.value_or(0),
                        schedule_repeat.value_or(0));
                    scheduled_checksums.emplace_back(schedule_key, checksum);
                    cell_checksums.push_back(checksum);
                    if (duration.has_value()) {
                        cell_durations.push_back(*duration);
                        if (*repeat == 0) {
                            cell_first_repeat_durations.push_back(*duration);
                        } else {
                            cell_steady_state_durations.push_back(*duration);
                        }
                        all_durations.push_back(*duration);
                    }
                    if (index_and_map.has_value()) {
                        cell_index_and_map_durations.push_back(*index_and_map);
                        all_index_and_map_durations.push_back(*index_and_map);
                    }
                }
                if ((wall != nullptr && wall_sample_index != wall->size())
                    || (measured_region != nullptr
                        && measured_region_sample_index
                            != measured_region->size())) {
                    valid = false;
                }
                std::sort(cell_checksums.begin(), cell_checksums.end());
                std::uint64_t cell_hash = 14695981039346656037ULL;
                for (const auto& checksum : cell_checksums) {
                    cell_hash = raw_fnv1a64_update(cell_hash, checksum);
                }
                cell_summaries.push_back(nlohmann::json{
                    {"benchmark", benchmark_key},
                    {"test_case", cell.test_case->name},
                    {"algorithm", cell.tuple->algorithm_name},
                    {"tuple_index", cell.tuple_index},
                    {"group_index", cell.group_index},
                    {"record_count", cell.raw_samples.size()},
                    {"schedule_sorted_checksum", raw_hex64(cell_hash)},
                    {"outer_e2e_ns", raw_duration_summary(cell_durations)},
                    {"first_repeat_outer_e2e_ns",
                        raw_duration_summary(cell_first_repeat_durations)},
                    {"steady_state_outer_e2e_ns",
                        raw_duration_summary(cell_steady_state_durations)},
                    {"index_and_map_ns",
                        raw_duration_summary(cell_index_and_map_durations)},
                });
            }
        }

        for (const auto& [benchmark_key, state_ptr] : reports) {
            (void)benchmark_key;
            const auto& state = *state_ptr;
            if (!state.raw_samples_enabled) continue;
            std::map<std::pair<std::size_t, std::size_t>,
                std::vector<const measurement_cell*>> expected_batches;
            for (const auto& cell : state.cells) {
                expected_batches[{cell->test_case_index, cell->group_index}]
                    .push_back(cell.get());
            }
            for (auto& [case_group, cells] : expected_batches) {
                std::sort(cells.begin(), cells.end(),
                    [](const measurement_cell* lhs, const measurement_cell* rhs) {
                        return lhs->tuple_index < rhs->tuple_index;
                    });
                auto found = actual_order.find(raw_batch_key{
                    state.driver_benchmark_index,
                    case_group.first,
                    case_group.second});
                if (found == actual_order.end()) {
                    if (std::any_of(cells.begin(), cells.end(),
                            [](const measurement_cell* cell) {
                                return cell->iterations != 0;
                            })) valid = false;
                    continue;
                }
                auto actual = found->second;
                std::sort(actual.begin(), actual.end());
                using observed_order = std::tuple<
                    std::size_t, std::size_t, std::size_t, std::uint64_t>;
                std::vector<observed_order> observed;
                for (const auto& [raw_id, repeat, order_position,
                                  tuple_index, shuffle_seed] : actual) {
                    (void)raw_id;
                    observed.emplace_back(
                        repeat, order_position, tuple_index, shuffle_seed);
                }
                std::vector<observed_order> expected;
                if (state.config.iteration_order
                    == benchmark_configuration::iteration_order_mode::interleaved_seeded) {
                    const std::size_t iterations = cells.empty()
                        ? 0 : cells.front()->iterations;
                    for (std::size_t repeat = 0; repeat < iterations; ++repeat) {
                        const std::uint64_t shuffle_seed =
                            raw_schedule_shuffle_seed(
                                state.config.iteration_order_seed,
                                state.config.benchmark_key,
                                cells.front()->test_case->name,
                                case_group.second, repeat);
                        const auto order = raw_fisher_yates_order(
                            cells.size(), shuffle_seed);
                        for (std::size_t position = 0;
                             position < order.size(); ++position) {
                            expected.emplace_back(
                                repeat,
                                position,
                                cells[order[position]]->tuple_index,
                                shuffle_seed);
                        }
                    }
                } else {
                    for (std::size_t position = 0;
                         position < cells.size(); ++position) {
                        const measurement_cell* cell = cells[position];
                        for (std::size_t repeat = 0;
                             repeat < cell->iterations; ++repeat) {
                            const std::uint64_t shuffle_seed =
                                raw_schedule_shuffle_seed(
                                    state.config.iteration_order_seed,
                                    state.config.benchmark_key,
                                    cell->test_case->name,
                                    case_group.second, repeat);
                            expected.emplace_back(
                                repeat, position, cell->tuple_index,
                                shuffle_seed);
                        }
                    }
                }
                if (observed != expected) valid = false;
            }
        }

        if (embedded_by_id.size() != records_.size()) valid = false;
        std::uint64_t append_hash = 14695981039346656037ULL;
        for (std::size_t index = 0; index < records_.size(); ++index) {
            nlohmann::json record = nlohmann::json::parse(
                records_[index], nullptr, false);
            const auto id = record.is_discarded()
                ? std::nullopt : raw_json_u64(record, "raw_record_id");
            if (!id.has_value() || *id != index
                || !embedded_by_id.contains(index)
                || embedded_by_id[index] != records_[index]) {
                valid = false;
            }
            const std::string checksum_value = record.is_discarded()
                ? std::string{}
                : record.value("record_checksum", std::string{});
            append_hash = raw_fnv1a64_update(
                append_hash, checksum_value);
        }
        std::sort(scheduled_checksums.begin(), scheduled_checksums.end());
        std::uint64_t schedule_hash = 14695981039346656037ULL;
        for (const auto& [key, checksum] : scheduled_checksums) {
            schedule_hash = raw_fnv1a64_update(schedule_hash, key);
            schedule_hash = raw_fnv1a64_update(schedule_hash, checksum);
        }
        manifest_ = nlohmann::json{
            {"schema", 1},
            {"path", path_.filename().string()},
            {"run_id", run_id_},
            // Completion is deliberately false until the close/reopen prefix
            // validation below has made the sidecar bytes authoritative.
            {"complete", false},
            {"record_count", embedded_by_id.size()},
            {"prefix_bytes", header_line_.size() + 1
                + std::accumulate(records_.begin(), records_.end(),
                    std::size_t{0}, [](const std::size_t total,
                                      const std::string& record) {
                        return total + record.size() + 1;
                    })},
            {"append_order_checksum", raw_hex64(append_hash)},
            {"schedule_sorted_checksum", raw_hex64(schedule_hash)},
            {"outer_e2e_ns", raw_duration_summary(all_durations)},
            {"index_and_map_ns",
                raw_duration_summary(all_index_and_map_durations)},
            {"cells", std::move(cell_summaries)},
        };
        sealed_ = true;
        if (fd_ >= 0) {
            (void)::close(fd_);
            fd_ = -1;
        }

        // validate the exact header + record prefix (including byte count and
        // absence of any suffix) while completion remains false everywhere.
        const bool prefix_valid = validate_authoritative_locked(
            embedded_by_id, manifest_, false);
        if (!valid || !prefix_valid) {
            manifest_["complete"] = false;
            (void)append_terminal_footer_locked(false);
            return false;
        }

        // Only the successful prefix reread authorises a complete=true
        // footer. Append it durably, then perform a second close/reopen parse.
        manifest_["complete"] = true;
        if (!append_terminal_footer_locked(true)
            || !validate_authoritative_locked(
                embedded_by_id, manifest_, true)) {
            // A torn footer or post-prefix mutation must never leave true as
            // the terminal authority. Best-effort append an explicit false
            // footer after first restoring line framing.
            manifest_["complete"] = false;
            (void)append_terminal_footer_locked(false);
            return false;
        }
        return true;
    }

    [[nodiscard]] const nlohmann::json& manifest() const noexcept {
        return manifest_;
    }

private:
    [[nodiscard]] bool validate_authoritative_locked(
            const std::map<std::uint64_t, std::string>& embedded_by_id,
            const nlohmann::json& expected_manifest,
            const bool expect_completion_footer) const {
        const auto authoritative = read_authoritative_file_locked();
        if (!authoritative.has_value() || authoritative->empty()
            || authoritative->back() != '\n') {
            return false;
        }
        const std::size_t expected_records = expected_manifest.value(
            "record_count", std::size_t{0});
        const std::size_t prefix_bytes = expected_manifest.value(
            "prefix_bytes", std::size_t{0});
        const nlohmann::json expected_footer{
            {"kind", "footer"},
            {"schema", 1},
            {"manifest", expected_manifest},
        };
        const std::string footer_line = expected_footer.dump();
        const std::size_t expected_bytes = prefix_bytes
            + (expect_completion_footer ? footer_line.size() + 1 : 0);
        if (prefix_bytes == 0 || authoritative->size() != expected_bytes) {
            return false;
        }

        std::vector<std::string_view> lines;
        std::size_t begin = 0;
        while (begin < authoritative->size()) {
            const std::size_t newline = authoritative->find('\n', begin);
            if (newline == std::string::npos || newline == begin) return false;
            lines.emplace_back(
                authoritative->data() + begin, newline - begin);
            begin = newline + 1;
        }
        const std::size_t expected_line_count = expected_records + 1
            + (expect_completion_footer ? 1 : 0);
        if (lines.size() != expected_line_count) return false;

        const nlohmann::json header = nlohmann::json::parse(
            lines.front(), nullptr, false);
        const nlohmann::json expected_header = nlohmann::json::parse(
            header_line_, nullptr, false);
        if (header.is_discarded() || !header.is_object()
            || lines.front() != header_line_
            || header.size() != 3 || header != expected_header
            || header.value("kind", std::string{}) != "header"
            || header.value("schema", 0) != 1
            || header.value("run_id", std::string{}) != run_id_) {
            return false;
        }
        if (expect_completion_footer) {
            const nlohmann::json footer = nlohmann::json::parse(
                lines.back(), nullptr, false);
            if (footer.is_discarded() || !footer.is_object()
                || lines.back() != footer_line
                || footer.size() != 3 || footer != expected_footer
                || footer.value("kind", std::string{}) != "footer"
                || footer.value("schema", 0) != 1
                || !footer.contains("manifest")
                || footer["manifest"] != expected_manifest) {
                return false;
            }
        }

        std::uint64_t disk_append_hash = 14695981039346656037ULL;
        std::uint64_t disk_schedule_hash = 14695981039346656037ULL;
        std::vector<std::pair<std::string, std::string>>
            disk_scheduled_checksums;
        std::vector<std::uint64_t> disk_outer;
        std::vector<std::uint64_t> disk_index_and_map;
        for (std::size_t index = 0; index < expected_records; ++index) {
            const nlohmann::json record = nlohmann::json::parse(
                lines[index + 1], nullptr, false);
            const auto id = record.is_discarded()
                ? std::nullopt : raw_json_u64(record, "raw_record_id");
            const auto outer = record.is_discarded()
                ? std::nullopt : raw_json_u64(record, "outer_e2e_ns");
            const auto published = record.is_discarded()
                ? std::nullopt
                : raw_json_u64(record, "published_wall_clock_ns");
            const auto measured = record.is_discarded()
                ? std::nullopt : raw_json_u64(record, "index_and_map_ns");
            const auto timing_disposition = record.is_discarded()
                ? std::nullopt
                : classify_raw_timing_disposition(record);
            if (record.is_discarded() || !record.is_object()
                || record.contains("kind") || !id.has_value()
                || *id != index || !timing_disposition.has_value()
                || !record.contains("outer_e2e_ns")
                || !record.contains("published_wall_clock_ns")
                || (outer.has_value()
                    != timing_disposition->framework_timing_eligible)
                || (published.has_value()
                    != timing_disposition->framework_timing_eligible)
                || (!outer.has_value()
                    && !record["outer_e2e_ns"].is_null())
                || (!published.has_value()
                    && !record["published_wall_clock_ns"].is_null())
                || (outer.has_value() && published.has_value()
                    && *published > *outer)
                || !record.contains("schedule")
                || !record["schedule"].is_object()
                || record["schedule"].size() != 9
                || !record.contains("record_checksum")
                || !record["record_checksum"].is_string()
                || record["record_checksum"].get<std::string>()
                    != raw_record_checksum(record)
                || !embedded_by_id.contains(index)
                || lines[index + 1] != embedded_by_id.at(index)
                || nlohmann::json::parse(
                        embedded_by_id.at(index), nullptr, false) != record) {
                return false;
            }
            const auto& schedule = record["schedule"];
            if (!raw_json_u64(schedule, "benchmark_index").has_value()
                || !raw_json_u64(schedule, "test_case_index").has_value()
                || !raw_json_u64(schedule, "tuple_index").has_value()
                || !raw_json_u64(schedule, "group_index").has_value()
                || !raw_json_u64(schedule, "repeat").has_value()
                || !raw_json_u64(schedule, "order_position").has_value()
                || !schedule.contains("mode")
                || !schedule["mode"].is_string()
                || !schedule.contains("configured_seed")
                || !schedule["configured_seed"].is_string()
                || !schedule.contains("shuffle_seed")
                || !schedule["shuffle_seed"].is_string()) {
                return false;
            }
            const std::string checksum =
                record["record_checksum"].get<std::string>();
            disk_append_hash = raw_fnv1a64_update(
                disk_append_hash, checksum);
            if (outer.has_value()) disk_outer.push_back(*outer);
            if (measured.has_value()) disk_index_and_map.push_back(*measured);
            const std::string schedule_key = fmt::format(
                "{:020}:{:020}:{:020}:{:020}:{:020}",
                *raw_json_u64(schedule, "benchmark_index"),
                *raw_json_u64(schedule, "test_case_index"),
                *raw_json_u64(schedule, "tuple_index"),
                *raw_json_u64(schedule, "group_index"),
                *raw_json_u64(schedule, "repeat"));
            disk_scheduled_checksums.emplace_back(schedule_key, checksum);
        }
        std::sort(disk_scheduled_checksums.begin(),
            disk_scheduled_checksums.end());
        for (const auto& [key, checksum] : disk_scheduled_checksums) {
            disk_schedule_hash = raw_fnv1a64_update(disk_schedule_hash, key);
            disk_schedule_hash = raw_fnv1a64_update(
                disk_schedule_hash, checksum);
        }
        return expected_manifest.value(
                    "append_order_checksum", std::string{})
                    == raw_hex64(disk_append_hash)
            && expected_manifest.value(
                    "schedule_sorted_checksum", std::string{})
                    == raw_hex64(disk_schedule_hash)
            && expected_manifest["outer_e2e_ns"]
                    == raw_duration_summary(disk_outer)
            && expected_manifest["index_and_map_ns"]
                    == raw_duration_summary(disk_index_and_map);
    }

    [[nodiscard]] static bool write_all_fd(
            const int fd, std::string_view bytes) noexcept {
        std::size_t written = 0;
        while (written < bytes.size()) {
            const ssize_t count = ::write(
                fd, bytes.data() + written, bytes.size() - written);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) return false;
            written += static_cast<std::size_t>(count);
        }
        return true;
    }

    /// Terminal footers use a fresh descriptor and the real robust syscalls,
    /// intentionally independent of the sample-write test seam. This lets a
    /// poisoned/torn sample path still publish an explicit complete=false
    /// terminal authority whenever the filesystem remains writable.
    [[nodiscard]] bool append_terminal_footer_locked(
            const bool complete) noexcept {
        manifest_["complete"] = complete;
        const std::string footer = nlohmann::json{
            {"kind", "footer"},
            {"schema", 1},
            {"manifest", manifest_},
        }.dump() + '\n';
        const int footer_fd = ::open(
            path_.c_str(), O_RDWR | O_APPEND | O_CLOEXEC);
        if (footer_fd < 0) return false;
        bool ok = true;
        struct stat status{};
        if (::fstat(footer_fd, &status) != 0 || status.st_size < 0) {
            ok = false;
        } else if (status.st_size > 0) {
            char last = '\0';
            ssize_t read_count = -1;
            do {
                read_count = ::pread(
                    footer_fd, &last, 1, status.st_size - 1);
            } while (read_count < 0 && errno == EINTR);
            if (read_count != 1) {
                ok = false;
            } else if (last != '\n') {
                ok = write_all_fd(footer_fd, "\n");
            }
        }
        if (ok) ok = write_all_fd(footer_fd, footer);
        if (ok) {
            while (::fsync(footer_fd) != 0) {
                if (errno == EINTR) continue;
                ok = false;
                break;
            }
        }
        (void)::close(footer_fd);
        return ok;
    }

    [[nodiscard]] std::optional<std::string>
    read_authoritative_file_locked() const {
        const int read_fd = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC);
        if (read_fd < 0) return std::nullopt;
        struct stat status{};
        if (::fstat(read_fd, &status) != 0 || !S_ISREG(status.st_mode)
            || status.st_size < 0) {
            (void)::close(read_fd);
            return std::nullopt;
        }
        std::string bytes(static_cast<std::size_t>(status.st_size), '\0');
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const ssize_t count = ::read(
                read_fd, bytes.data() + offset, bytes.size() - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) {
                (void)::close(read_fd);
                return std::nullopt;
            }
            offset += static_cast<std::size_t>(count);
        }
        char extra = '\0';
        ssize_t suffix = -1;
        do {
            suffix = ::read(read_fd, &extra, 1);
        } while (suffix < 0 && errno == EINTR);
        (void)::close(read_fd);
        if (suffix != 0) return std::nullopt;
        return bytes;
    }

    [[nodiscard]] bool append_line_locked(std::string_view payload) noexcept {
        if (fd_ < 0 || poisoned_) return false;
        std::string line{payload};
        line.push_back('\n');
        std::size_t written = 0;
        while (written < line.size()) {
            std::size_t request = line.size() - written;
            if (io_fault_
                == raw_evidence_io_fault::short_write) {
                request = std::min<std::size_t>(request, 3);
            } else if (io_fault_
                           == raw_evidence_io_fault::partial_write_failure
                       && append_line_calls_ > 0) {
                request = std::min<std::size_t>(request, 3);
            }
            const ssize_t result = ::write(
                fd_, line.data() + written, request);
            if (result < 0 && errno == EINTR) continue;
            if (result <= 0) return false;
            written += static_cast<std::size_t>(result);
            if (io_fault_
                    == raw_evidence_io_fault::partial_write_failure
                && append_line_calls_ > 0) {
                errno = EIO;
                return false;
            }
        }
        if (io_fault_ == raw_evidence_io_fault::sync_failure
            && append_line_calls_ > 0) {
            errno = EIO;
            return false;
        }
        while (::fsync(fd_) != 0) {
            if (errno == EINTR) continue;
            return false;
        }
        ++append_line_calls_;
        return true;
    }

    std::filesystem::path path_{};
    std::string run_id_{};
    mutable std::mutex mu_{};
    int fd_ = -1;
    bool poisoned_ = false;
    bool sealed_ = false;
    std::uint64_t next_record_id_ = 0;
    std::size_t append_line_calls_ = 0;
    raw_evidence_io_fault io_fault_ =
        raw_evidence_io_fault::none;
    std::string header_line_{};
    std::vector<std::string> records_{};
    nlohmann::json manifest_ = nlohmann::json::object();
};

/// Decrement one cell's case counter; on the 0-transition claim the single
/// CASE_RESULTS emit. Reaching 0 here means every cell of the case ran its
/// item loop (so the case necessarily `started`). Safe under concurrency: the
/// acq_rel fetch_sub gives the to-zero thread a happens-after over every
/// sibling cell write, so merge_case_samples sees a fully-written, immutable
/// cell set.
inline void debug_finish_cell(benchmark_context& context,
                              benchmark_execution_state& state,
                              const std::string& case_name) {
    auto& cp = state.debug_progress_by_case.at(case_name);
    if (cp.remaining_cells.fetch_sub(1, std::memory_order_acq_rel) == 1
        && !cp.emitted.exchange(true, std::memory_order_acq_rel))
        context.debug_sink->emit_case_results(state.config.benchmark_key, case_name, state);
}

// ----------------- run_unit driver -----------------
//
// Defined here (after debug_report_sink / debug_finish_cell) rather than next to
// the other run-machinery so the per-case `--debug` hooks below can call the
// sink: the sink's CASE_RESULTS serializer reuses emit_execution_tuple_record
// + the aggregation helpers, all of which are defined above this point.

[[nodiscard]] inline nlohmann::json raw_param_map_json(const parameter_map& params) {
    nlohmann::json result = nlohmann::json::object();
    for (const auto& [name, value] : params) {
        switch (value.kind) {
            case config_value::value_kind::string:
                result[name] = value.string_value;
                break;
            case config_value::value_kind::integer:
                result[name] = value.integer_value;
                break;
            case config_value::value_kind::floating_point:
                result[name] = value.double_value;
                break;
            case config_value::value_kind::boolean:
                result[name] = value.boolean_value;
                break;
            case config_value::value_kind::non_scalar:
                result[name] = value.string_value;
                break;
        }
    }
    return result;
}

struct finalized_raw_sample_record {
    std::string serialized{};
    std::optional<std::uint64_t> e2e_adjusted_outer_ns{};
    bool framework_timing_eligible = true;
    std::optional<bool> e2e_tuple_accepted{};
};

/// Attach the framework-owned repeat envelope and finalize E2E-only derived
/// timing evidence before the record enters either aggregation or the durable
/// raw sink. Other benchmarks retain their existing open payload contract.
[[nodiscard]] inline std::optional<finalized_raw_sample_record>
make_raw_sample_record(
        const std::string_view payload,
        const benchmark_execution_state& state,
        const measurement_cell& cell,
        const std::size_t repeat,
        const std::uint64_t outer_e2e_ns,
        const std::uint64_t published_wall_clock_ns) noexcept {
    auto record = nlohmann::json::parse(payload, nullptr, false);
    if (record.is_discarded() || !record.is_object()) {
        return std::nullopt;
    }
    constexpr std::array<std::string_view, 15> reserved{
        "schema", "benchmark", "test_case", "algorithm", "group_index",
        "group_count", "tuple_index", "repeat", "params", "outer_e2e_ns",
        "published_wall_clock_ns",
        "outer_e2e_minus_cigar_or_mapq_union_ns", "raw_record_id",
        "schedule", "record_checksum",
    };
    for (const std::string_view key : reserved) {
        if (record.contains(std::string{key})) {
            return std::nullopt;
        }
    }
    const auto timing_disposition = classify_raw_timing_disposition(record);
    if (!timing_disposition.has_value()) return std::nullopt;
    record["schema"] = 1;
    record["benchmark"] = state.config.benchmark_key;
    record["test_case"] = cell.test_case->name;
    record["algorithm"] = cell.tuple->algorithm_name;
    record["group_index"] = cell.group_index;
    record["group_count"] = cell.group_count;
    record["tuple_index"] = cell.tuple_index;
    record["repeat"] = repeat;
    record["params"] = raw_param_map_json(cell.tuple->params);
    record["outer_e2e_ns"] = timing_disposition->framework_timing_eligible
        ? nlohmann::json(outer_e2e_ns)
        : nlohmann::json(nullptr);
    record["published_wall_clock_ns"] =
        timing_disposition->framework_timing_eligible
            ? nlohmann::json(published_wall_clock_ns)
            : nlohmann::json(nullptr);

    std::optional<std::uint64_t> adjusted_outer_ns;
    const auto payload_schema = record.find("payload_schema");
    if (payload_schema != record.end()
        && payload_schema->is_string()
        && *payload_schema == "bioseq.e2e.raw-sample") {
        const auto payload_version = raw_json_u64(
            record, "payload_schema_version");
        const auto contract = record.find("e2e_interval_metric_contract");
        if (!payload_version.has_value()
            || (*payload_version != e2e_raw_payload_schema_version - 1u
                && *payload_version != e2e_raw_payload_schema_version)
            || contract == record.end() || !contract->is_object()
            || contract->value("schema", std::string{})
                != "bioseq.e2e.interval-metrics"
            || raw_json_u64(*contract, "version") != 2u) {
            return std::nullopt;
        }
        const auto finalize = contract->find("framework_finalize_adjusted_metric");
        if (finalize == contract->end() || !finalize->is_boolean()) {
            return std::nullopt;
        }
        if (finalize->get<bool>()) {
            const auto tuple_status = record.find("tuple_status");
            if (tuple_status == record.end() || !tuple_status->is_object()) {
                return std::nullopt;
            }
            const auto accepted = raw_json_u64(*tuple_status, "accepted");
            if (!accepted.has_value() || *accepted > 1u
                || (*accepted == 1u)
                    != timing_disposition->framework_timing_eligible) {
                return std::nullopt;
            }
            const auto union_ns = raw_json_u64(
                record, "cigar_or_mapq_interval_union_ns");
            if (*accepted == 1u) {
                const auto domains = contract->find("duration_clock_domains");
                if (domains == contract->end() || !domains->is_object()) {
                    return std::nullopt;
                }
                const auto interval_domain = domains->find("interval_union");
                if (interval_domain == domains->end()
                    || !interval_domain->is_object()) {
                    return std::nullopt;
                }
                const std::string rate_compatibility = interval_domain->value(
                    "rate_compatibility", std::string{});
                const std::string interval_clock = interval_domain->value(
                    "clock", std::string{});
                const bool compatible_duration_rate =
                    (rate_compatibility == "same_clock_implementation"
                        && interval_clock ==
                            "bioseq.instrumentation.clock::now_ns")
                    || (rate_compatibility ==
                            "same_kernel_monotonic_nanosecond_rate"
                        && interval_clock ==
                            "helper-declared CLOCK_MONOTONIC");
                if (!union_ns.has_value() || *union_ns > outer_e2e_ns
                    || !compatible_duration_rate) {
                    return std::nullopt;
                }
                adjusted_outer_ns = outer_e2e_ns - *union_ns;
                record["outer_e2e_minus_cigar_or_mapq_union_ns"] =
                    *adjusted_outer_ns;
            } else {
                const auto union_value = record.find(
                    "cigar_or_mapq_interval_union_ns");
                if (union_value == record.end() || !union_value->is_null()) {
                    return std::nullopt;
                }
                record["outer_e2e_minus_cigar_or_mapq_union_ns"] = nullptr;
            }
        }
    }
    return finalized_raw_sample_record{
        .serialized = record.dump(),
        .e2e_adjusted_outer_ns = adjusted_outer_ns,
        .framework_timing_eligible =
            timing_disposition->framework_timing_eligible,
        .e2e_tuple_accepted = timing_disposition->e2e
            ? std::optional<bool>{
                timing_disposition->framework_timing_eligible}
            : std::nullopt,
    };
}

template<typename p_input, typename p_output, typename p_iteration_boundaries>
[[nodiscard]] inline std::function<void()> prepare_typed_benchmark(
    benchmark_execution_state& state,
    benchmark_context& context,
    const std::function<run_constraints(std::string_view, std::string_view, simdjson::dom::element)>& on_estimate_resources,
    const std::function<p_input(std::string_view, std::string_view, simdjson::dom::element, const run_constraints&)>& on_input,
    const std::function<p_output(std::string_view, const parameter_map&, const p_input&)>& on_run,
    const std::function<std::map<std::string, double>(std::string_view, const parameter_map&, const p_input&, const p_output&)>& on_evaluate,
    const std::function<void(std::string_view, std::string_view, simdjson::dom::element, const p_input&)>& on_cleanup,
    const std::function<std::string(std::string_view, const parameter_map&, const p_input&, const p_output&)>& serialize_raw_sample_payload,
    const repeat_finalization_callbacks<p_input, p_output>&
        repeat_finalization,
    const p_iteration_boundaries& iteration_boundaries)
{
    using bioseq::instrumentation::installed_profiler_scope;
    using bioseq::instrumentation::profiler_state;

    const std::string                 data_dir = context.data_dir;
    const auto counter_handles = resolve_counter_handles(state.config.metrics);
    const bool by_regions = state.config.by_regions;
    const bool track_named_region_iteration_sample_counts =
        state.config.acceptance.enabled()
        && state.config.acceptance
               .require_named_region_iteration_coverage;
    bool track_paired_rule_evidence = false;
    for (const auto& metric : state.config.metrics) {
        for (const auto& rule : metric.rules) {
            if (rule.cmp.pairing
                == rule_comparator::pairing_mode::by_iteration_ratio) {
                track_paired_rule_evidence = true;
                break;
            }
        }
        if (track_paired_rule_evidence) break;
    }
    const auto injected_schedule_fault =
        paired_schedule_evidence_fault_for_testing.load(
            std::memory_order_acquire);

    // Pre-built batch list: one batch per (test_case, group). Items
    // inside a batch run sequentially against one cached Input.
    auto batches = build_work_batches(state);
    if (batches.empty()) return [] {};

    // --debug: count cells per case (algorithm name). The map is fully shaped
    // here (single-threaded, before enqueue); thereafter only the per-node
    // atomics mutate, so concurrent reads of nodes + atomic updates are safe.
    if (context.debug_sink != nullptr) {
        for (const auto& cell_ptr : state.cells)
            state.debug_progress_by_case[cell_ptr->tuple->algorithm_name]
                .remaining_cells.fetch_add(1, std::memory_order_relaxed);
    }

    auto* scheduler = context.scheduler;
    DEBUG_ASSERT(scheduler != nullptr,
        "benchmark_context::scheduler must be set by run_impl");

    /// Heap-owned cache of per-batch input/constraint slots. Preparation
    /// returns before the bodies finish, so every object referenced by an
    /// enqueued body lives in this shared run state until finalization joins
    /// the benchmark.
    struct group_slot {
        std::optional<p_input> input{};
        run_constraints       constraints{};
    };
    struct typed_run_state {
        std::vector<group_slot> group_slots{};
        std::vector<work_batch> batches{};
        std::function<void(work_batch&)> run_batch{};
    };
    auto run_state = std::make_shared<typed_run_state>();
    run_state->group_slots.resize(batches.size());
    run_state->batches = std::move(batches);

    /// Commit one already-measured output.  The paired path calls this only
    /// after every output in the logical repeat has been finalized, so no raw
    /// record or metric can escape ahead of a same-repeat invalid verdict.
    auto commit_iteration = [&context, &state, by_regions,
                                track_named_region_iteration_sample_counts,
                                track_paired_rule_evidence,
                                injected_schedule_fault,
                                &on_evaluate, &serialize_raw_sample_payload,
                                &repeat_finalization](
                                    measurement_cell& cell,
                                    const p_input& input,
                                    const p_output& output,
                                    profiler_state<profiler_configuration>& profiler_block,
                                    const std::vector<std::int64_t>& counters_before,
                                    const std::vector<std::int64_t>& counters_after,
                                    const std::int64_t t_begin_ns,
                                    const std::int64_t t_end_ns,
                                    const std::size_t repeat,
                                    const std::size_t order_position,
                                    const std::uint64_t shuffle_seed,
                                    const bool use_repeat_finalization) -> bool {
        const auto& tuple = *cell.tuple;
        const std::uint64_t outer_elapsed_ns = t_end_ns >= t_begin_ns
            ? static_cast<std::uint64_t>(t_end_ns - t_begin_ns)
            : std::uint64_t{0};
        std::uint64_t published_wall_clock_ns = outer_elapsed_ns;
        if (use_repeat_finalization
            && repeat_finalization.published_wall_clock_ns) {
            published_wall_clock_ns =
                repeat_finalization.published_wall_clock_ns(
                    tuple.algorithm_name, tuple.params, input, output);
            if (published_wall_clock_ns > outer_elapsed_ns) {
                push_runtime_diagnostic(
                    context, validation_diagnostic::diagnostic_severity::error,
                    "published_wall_clock_exceeds_framework_envelope",
                    runtime_location(state.config.config_path,
                        state.config.benchmark_key,
                        cell.test_case->name, cell.group_index),
                    "opt-in published algorithm duration must be contained "
                    "by the same framework callback envelope");
                return false;
            }
        }
        const auto iteration_wall_clock_ns =
            static_cast<double>(published_wall_clock_ns);
        auto evaluation_metrics =
            on_evaluate(tuple.algorithm_name, tuple.params, input, output);

        std::optional<std::string> raw_sample;
        bool framework_timing_eligible = true;
        if (serialize_raw_sample_payload) {
            const std::string payload = serialize_raw_sample_payload(
                tuple.algorithm_name, tuple.params, input, output);
            auto finalized_raw_sample = make_raw_sample_record(
                payload, state, cell, repeat, outer_elapsed_ns,
                published_wall_clock_ns);
            if (!finalized_raw_sample.has_value()) {
                push_runtime_diagnostic(
                    context, validation_diagnostic::diagnostic_severity::error,
                    "raw_sample_payload_invalid",
                    runtime_location(state.config.config_path,
                        state.config.benchmark_key,
                        cell.test_case->name, cell.group_index),
                    "raw payload must be one JSON object without reserved "
                    "framework fields and with a current valid benchmark-"
                    "specific finalization contract");
                return false;
            }
            if (finalized_raw_sample->e2e_tuple_accepted.has_value()) {
                const auto evaluated_acceptance =
                    evaluation_metrics.find("tuple_accepted");
                const double expected_acceptance =
                    *finalized_raw_sample->e2e_tuple_accepted ? 1.0 : 0.0;
                if (evaluated_acceptance == evaluation_metrics.end()
                    || !std::isfinite(evaluated_acceptance->second)
                    || evaluated_acceptance->second != expected_acceptance) {
                    push_runtime_diagnostic(
                        context,
                        validation_diagnostic::diagnostic_severity::error,
                        "e2e_timing_disposition_evidence_mismatch",
                        state.config.benchmark_key,
                        "E2E raw tuple_status.accepted and evaluator "
                        "tuple_accepted must be identical integer evidence");
                    return false;
                }
            }
            framework_timing_eligible =
                finalized_raw_sample->framework_timing_eligible;
            if (finalized_raw_sample->e2e_adjusted_outer_ns.has_value()) {
                const bool inserted = evaluation_metrics.emplace(
                    "outer_e2e_minus_cigar_or_mapq_union_ns",
                    static_cast<double>(
                        *finalized_raw_sample->e2e_adjusted_outer_ns)).second;
                if (!inserted) {
                    push_runtime_diagnostic(
                        context,
                        validation_diagnostic::diagnostic_severity::error,
                        "raw_sample_derived_metric_collision",
                        state.config.benchmark_key,
                        "E2E evaluator must not pre-populate the framework-"
                        "finalized adjusted outer metric");
                    return false;
                }
            }
            raw_sample = std::move(finalized_raw_sample->serialized);
            if (context.raw_evidence_sink == nullptr) {
                push_runtime_diagnostic(
                    context, validation_diagnostic::diagnostic_severity::error,
                    "raw_evidence_sink_missing",
                    state.config.benchmark_key,
                    "raw-enabled benchmark has no durable evidence sink");
                return false;
            }
            raw_sample = context.raw_evidence_sink->append_sample(
                *raw_sample,
                context.benchmark_index,
                cell.test_case_index,
                cell.tuple_index,
                cell.group_index,
                repeat,
                state.config.iteration_order
                        == benchmark_configuration::iteration_order_mode::interleaved_seeded
                    ? std::string_view{"interleaved_seeded"}
                    : std::string_view{"item_major"},
                state.config.iteration_order_seed,
                shuffle_seed,
                order_position);
            if (!raw_sample.has_value()) {
                push_runtime_diagnostic(
                    context, validation_diagnostic::diagnostic_severity::error,
                    "raw_evidence_append_failed",
                    context.raw_evidence_sink->path().string(),
                    "durable raw evidence append or fsync failed; sink is "
                    "poisoned and the batch will run cleanup before failing");
                return false;
            }
        }

        // measurement_cell completion is downstream of durable append.  A crash can leave
        // a sidecar prefix, never an in-memory/report sample with no durable
        // counterpart.
        append_iteration_samples(
            cell, repeat, framework_timing_eligible, by_regions,
            track_named_region_iteration_sample_counts,
            profiler_block,
            iteration_wall_clock_ns, state.config.metrics,
            counters_before, counters_after, evaluation_metrics);
        if (track_paired_rule_evidence) {
            std::size_t evidence_order_position = order_position;
            std::uint64_t evidence_shuffle_seed = shuffle_seed;
            if (injected_schedule_fault
                    == paired_schedule_evidence_fault::wrong_shuffle_seed) {
                ++evidence_shuffle_seed;
            } else if (injected_schedule_fault
                           == paired_schedule_evidence_fault::wrong_first_tuple_order_position
                       && cell.tuple_index == 0) {
                ++evidence_order_position;
            }
            cell.iteration_schedule_samples.push_back(
                iteration_schedule_sample{
                    .repeat = repeat,
                    .order_position = evidence_order_position,
                    .shuffle_seed = evidence_shuffle_seed,
                });
        }
        if (raw_sample.has_value()) {
            cell.raw_samples.push_back(std::move(*raw_sample));
        }

        progress_tick_iteration(context, cell);
        return true;
    };

    /// Historical immediate path.  The legacy factory instantiates this with
    /// `std::nullptr_t`, so the boundary branch is discarded at compile time.
    auto run_one_iteration = [&on_run, commit_iteration, counter_handles,
                              &iteration_boundaries](
                                  measurement_cell& cell,
                                  const p_input& input,
                                  const std::size_t repeat,
                                  const std::size_t order_position,
                                  const std::uint64_t shuffle_seed) -> bool {
        const auto& tuple = *cell.tuple;
        if constexpr (!std::is_same_v<p_iteration_boundaries, std::nullptr_t>) {
            if (iteration_boundaries.enabled
                && iteration_boundaries.enabled(
                    tuple.algorithm_name, tuple.params, input)) {
                using preparation_type =
                    typename p_iteration_boundaries::preparation_type;
                profiler_state<profiler_configuration> profiler_block{};
                std::vector<std::int64_t> counters_before;
                std::vector<std::int64_t> counters_after;
                std::int64_t t_begin_ns = 0;
                std::int64_t t_end_ns = 0;
                std::optional<p_output> output;
                {
                    preparation_type preparation =
                        iteration_boundaries.before_iteration(
                            tuple.algorithm_name, tuple.params, input, repeat);
                    {
                        installed_profiler_scope<profiler_configuration>
                            profiler_scope{profiler_block};
                        counters_before = snapshot_counters(counter_handles);
                        t_begin_ns = bioseq::instrumentation::clock::now_ns();
                        output.emplace(
                            iteration_boundaries.measured_iteration(
                                tuple.algorithm_name, tuple.params, input,
                                preparation));
                        t_end_ns = bioseq::instrumentation::clock::now_ns();
                        counters_after = snapshot_counters(counter_handles);
                    }
                    const iteration_measurement_window window{
                        .begin_ns = t_begin_ns,
                        .end_ns = t_end_ns,
                        .elapsed_ns = t_end_ns >= t_begin_ns
                            ? static_cast<std::uint64_t>(
                                t_end_ns - t_begin_ns)
                            : std::uint64_t{0},
                    };
                    iteration_boundaries.after_iteration(
                        tuple.algorithm_name, tuple.params, input,
                        preparation, *output, window);
                }
                return commit_iteration(
                    cell, input, *output, profiler_block,
                    counters_before, counters_after, t_begin_ns, t_end_ns,
                    repeat, order_position, shuffle_seed,
                    /*use_repeat_finalization=*/false);
            }
        }
        profiler_state<profiler_configuration> profiler_block{};
        installed_profiler_scope<profiler_configuration> profiler_scope{
            profiler_block};
        const auto counters_before = snapshot_counters(counter_handles);
        const auto t_begin_ns = bioseq::instrumentation::clock::now_ns();
        const p_output output = on_run(
            tuple.algorithm_name, tuple.params, input);
        const auto t_end_ns = bioseq::instrumentation::clock::now_ns();
        const auto counters_after = snapshot_counters(counter_handles);
        return commit_iteration(
            cell, input, output, profiler_block,
            counters_before, counters_after, t_begin_ns, t_end_ns,
            repeat, order_position, shuffle_seed,
            /*use_repeat_finalization=*/false);
    };

    struct deferred_iteration {
        measurement_cell* cell = nullptr;
        std::size_t repeat = 0;
        std::size_t order_position = 0;
        std::uint64_t shuffle_seed = 0;
        profiler_state<profiler_configuration> profiler_block{};
        std::vector<std::int64_t> counters_before{};
        std::vector<std::int64_t> counters_after{};
        std::int64_t t_begin_ns = 0;
        std::int64_t t_end_ns = 0;
        std::optional<p_output> output{};
    };

    auto measure_deferred_iteration = [&on_run, counter_handles,
                                       &iteration_boundaries](
                                            measurement_cell& cell,
                                            const p_input& input,
                                            const std::size_t repeat,
                                            const std::size_t order_position,
                                            const std::uint64_t shuffle_seed) {
        auto pending = std::make_unique<deferred_iteration>();
        pending->cell = &cell;
        pending->repeat = repeat;
        pending->order_position = order_position;
        pending->shuffle_seed = shuffle_seed;
        if constexpr (!std::is_same_v<p_iteration_boundaries, std::nullptr_t>) {
            const auto& tuple = *cell.tuple;
            if (iteration_boundaries.enabled
                && iteration_boundaries.enabled(
                    tuple.algorithm_name, tuple.params, input)) {
                using preparation_type =
                    typename p_iteration_boundaries::preparation_type;
                preparation_type preparation =
                    iteration_boundaries.before_iteration(
                        tuple.algorithm_name, tuple.params, input, repeat);
                {
                    installed_profiler_scope<profiler_configuration>
                        profiler_scope{pending->profiler_block};
                    pending->counters_before =
                        snapshot_counters(counter_handles);
                    pending->t_begin_ns =
                        bioseq::instrumentation::clock::now_ns();
                    pending->output.emplace(
                        iteration_boundaries.measured_iteration(
                            tuple.algorithm_name, tuple.params, input,
                            preparation));
                    pending->t_end_ns =
                        bioseq::instrumentation::clock::now_ns();
                    pending->counters_after =
                        snapshot_counters(counter_handles);
                }
                const iteration_measurement_window window{
                    .begin_ns = pending->t_begin_ns,
                    .end_ns = pending->t_end_ns,
                    .elapsed_ns = pending->t_end_ns >= pending->t_begin_ns
                        ? static_cast<std::uint64_t>(
                            pending->t_end_ns - pending->t_begin_ns)
                        : std::uint64_t{0},
                };
                iteration_boundaries.after_iteration(
                    tuple.algorithm_name, tuple.params, input, preparation,
                    *pending->output, window);
                return pending;
            }
        }
        {
            installed_profiler_scope<profiler_configuration> profiler_scope{
                pending->profiler_block};
            pending->counters_before = snapshot_counters(counter_handles);
            pending->t_begin_ns = bioseq::instrumentation::clock::now_ns();
            const auto& tuple = *cell.tuple;
            pending->output.emplace(on_run(
                tuple.algorithm_name, tuple.params, input));
            pending->t_end_ns = bioseq::instrumentation::clock::now_ns();
            pending->counters_after = snapshot_counters(counter_handles);
        }
        return pending;
    };

    /// serial-concurrency exclusion are enforced by the scheduler's
    /// admission predicates, so the body never blocks on a process-
    /// global mutex. This is what restores cross-batch parallelism
    auto run_item_iterations = [&state, run_one_iteration](
        const work_batch_item& item, const p_input& input,
        const std::size_t order_position,
        const std::size_t group_index)
    {
        for (std::size_t iter = 0; iter < item.cell->iterations; ++iter) {
            const std::uint64_t shuffle_seed = raw_schedule_shuffle_seed(
                state.config.iteration_order_seed,
                state.config.benchmark_key,
                item.cell->test_case->name,
                group_index,
                iter);
            if (!run_one_iteration(
                    *item.cell, input, iter, order_position,
                    shuffle_seed)) return false;
        }
        return true;
    };

    /// Called from TBB worker after the scheduler has admitted the
    /// batch. The reservation lives entirely scheduler-side.
    auto* const run_state_ptr = run_state.get();
    run_state->run_batch = [&context, &state,
                            &on_input, &on_cleanup,
                            &repeat_finalization,
                            data_dir, run_state_ptr,
                            run_item_iterations,
                            run_one_iteration,
                            measure_deferred_iteration,
                            commit_iteration](work_batch& batch)
    {
        const auto& spec = state.test_cases[batch.test_case_index];
        auto& slot = run_state_ptr->group_slots[batch.batch_index];
        const auto location_str = runtime_location(
            state.config.config_path, state.config.benchmark_key,
            spec.name, batch.group_index);

        // 1. `slot.constraints` is the exact value computed once on the
        //    enqueueing thread and used by scheduler admission. Re-running the
        //    estimator here could reserve one amount and hand a different
        //    amount to `on_input`, and it violates the public once-only
        //    callback contract.

        // 2. on_input with retries. Permanent failure aborts the
        //    batch (scheduler releases on body return).
        auto built_input = build_input_with_retry<p_input>(
            context, batch,
            [&] { return on_input(data_dir, spec.name, spec.data,
                                   slot.constraints); });
        if (!built_input.has_value()) {
            auto reason = fmt::format(
                "batch '{}' (group {}) gave up after {} attempts at on_input — "
                "see UI for last error / trace",
                spec.name, batch.group_index, k_max_task_input_handler_error_count);
            push_runtime_diagnostic(context, validation_diagnostic::diagnostic_severity::error,
                "input_handler_failed_permanently", location_str, reason);
            progress_mark_batch_aborted(context, batch, reason);
            return;
        }
        slot.input = std::move(built_input);

        // 3. Iterate items × iterations. Raw E2E comparison blocks declare a
        //    seeded repetition-major order: every algorithm runs once before
        //    any receives its next repetition. Each repetition uses the exact
        //    identity-derived SplitMix64 / descending Fisher-Yates schedule
        //    recorded beside the raw sample. This prevents
        //    backend/cache position from being confounded with algorithm.
        progress_mark_test_case_start(context, batch);
        const bool interleaved = state.config.iteration_order
            == benchmark_configuration::iteration_order_mode::interleaved_seeded;
        const bool finalize_logical_repeats =
            static_cast<bool>(repeat_finalization.enabled)
            && repeat_finalization.enabled(*slot.input);
        if (finalize_logical_repeats
            && (!repeat_finalization.finalize_repeat
                || !repeat_finalization.published_wall_clock_ns)) {
            const std::string reason =
                "enabled repeat finalization requires both a finalizer and "
                "a published-duration selector";
            push_runtime_diagnostic(
                context, validation_diagnostic::diagnostic_severity::error,
                "invalid_repeat_finalization_callbacks", location_str,
                reason);
            progress_mark_batch_aborted(context, batch, reason);
            if (on_cleanup) {
                cleanup_with_retry(context, batch, spec.name, location_str,
                    [&] { on_cleanup(data_dir, spec.name, spec.data,
                        *slot.input); });
            }
            slot.input.reset();
            return;
        }
        if (interleaved
            && (!state.config.concurrency.bench_lock
                || state.config.concurrency.concurrency
                || batch.items.empty()
                || std::any_of(batch.items.begin(), batch.items.end(),
                    [&batch](const work_batch_item& item) {
                        return item.cell->iterations
                            != batch.items.front().cell->iterations;
                    }))) {
            const std::string reason =
                "interleaved_seeded requires bench_lock=true, "
                "concurrency=false and equal positive item iteration counts";
            push_runtime_diagnostic(context, validation_diagnostic::diagnostic_severity::error,
                "invalid_interleaved_schedule", location_str, reason);
            progress_mark_batch_aborted(context, batch, reason);
            if (on_cleanup) {
                cleanup_with_retry(context, batch, spec.name, location_str,
                    [&] { on_cleanup(data_dir, spec.name, spec.data,
                        *slot.input); });
            }
            slot.input.reset();
            return;
        }
        if (finalize_logical_repeats && !interleaved) {
            const std::string reason =
                "logical-repeat finalization requires interleaved_seeded "
                "so every same-repeat output is present before commit";
            push_runtime_diagnostic(
                context, validation_diagnostic::diagnostic_severity::error,
                "repeat_finalization_requires_interleaving", location_str,
                reason);
            progress_mark_batch_aborted(context, batch, reason);
            if (on_cleanup) {
                cleanup_with_retry(context, batch, spec.name, location_str,
                    [&] { on_cleanup(data_dir, spec.name, spec.data,
                        *slot.input); });
            }
            slot.input.reset();
            return;
        }
        for (auto& item : batch.items) {
            progress_update_label(context, batch, item, state.config.benchmark_key);
            // --debug CASE_STARTED: first time any cell of this case begins.
            if (context.debug_sink != nullptr) {
                auto& cp = state.debug_progress_by_case.at(item.cell->tuple->algorithm_name);
                if (!cp.started.exchange(true, std::memory_order_relaxed))
                    context.debug_sink->emit_case_started(state.config.benchmark_key,
                                                          item.cell->tuple->algorithm_name);
            }
        }
        bool iteration_failed = false;
        if (interleaved) {
            const std::size_t iterations =
                batch.items.front().cell->iterations;
            for (std::size_t repeat = 0; repeat < iterations; ++repeat) {
                const std::uint64_t shuffle_seed =
                    raw_schedule_shuffle_seed(
                        state.config.iteration_order_seed,
                        state.config.benchmark_key,
                        spec.name,
                        batch.group_index,
                        repeat);
                const auto order = raw_fisher_yates_order(
                    batch.items.size(), shuffle_seed);
                std::vector<std::unique_ptr<deferred_iteration>> pending;
                if (finalize_logical_repeats) {
                    pending.reserve(order.size());
                }
                for (std::size_t position = 0;
                     position < order.size(); ++position) {
                    auto& item = batch.items[order[position]];
                    progress_update_label(
                        context, batch, item, state.config.benchmark_key);
                    if (finalize_logical_repeats) {
                        pending.push_back(measure_deferred_iteration(
                            *item.cell, *slot.input, repeat,
                            position, shuffle_seed));
                    } else {
                        if (!run_one_iteration(
                                *item.cell, *slot.input, repeat,
                                position, shuffle_seed)) {
                            iteration_failed = true;
                            break;
                        }
                    }
                }
                if (finalize_logical_repeats && !iteration_failed) {
                    std::vector<repeat_output_reference<p_output>> outputs;
                    outputs.reserve(pending.size());
                    for (auto& item : pending) {
                        const auto& tuple = *item->cell->tuple;
                        outputs.push_back(repeat_output_reference<p_output>{
                            .algorithm_name = tuple.algorithm_name,
                            .params = &tuple.params,
                            .output = &*item->output,
                        });
                    }
                    repeat_finalization.finalize_repeat(
                        *slot.input, repeat, outputs);
                    for (auto& item : pending) {
                        if (!commit_iteration(
                                *item->cell, *slot.input, *item->output,
                                item->profiler_block,
                                item->counters_before,
                                item->counters_after,
                                item->t_begin_ns, item->t_end_ns,
                                item->repeat, item->order_position,
                                item->shuffle_seed,
                                /*use_repeat_finalization=*/true)) {
                            iteration_failed = true;
                            break;
                        }
                    }
                }
                if (iteration_failed) break;
            }
        } else {
            for (std::size_t position = 0;
                 position < batch.items.size(); ++position) {
                auto& item = batch.items[position];
                if (!run_item_iterations(
                        item, *slot.input, position, batch.group_index)) {
                    iteration_failed = true;
                    break;
                }
            }
        }
        if (!iteration_failed) {
            for (auto& item : batch.items) {
                progress_tick_item_done(context, batch);
                if (context.debug_sink != nullptr) {
                    debug_finish_cell(
                        context, state, item.cell->tuple->algorithm_name);
                }
            }
        }

        // 4. Cleanup with retries — warning on permanent failure.
        if (on_cleanup) {
            cleanup_with_retry(context, batch, spec.name, location_str,
                [&] { on_cleanup(data_dir, spec.name, spec.data, *slot.input); });
        }
        slot.input.reset();
        if (iteration_failed) {
            progress_mark_batch_aborted(
                context, batch,
                "durable raw evidence failure aborted the batch after cleanup");
        } else {
            progress_mark_group_done(context, batch);
        }
    };

    // batch loop. Each batch is enqueued into the scheduler with its
    // pre-computed memory + lock requirements; bodies are dispatched
    // by the scheduler when admissible.
    const auto sys_at_enqueue = scheduler->current_system_snapshot();
    const auto cap_at_enqueue = benchmark_memory_budget::cap_for(
        sys_at_enqueue.system_total_bytes, scheduler->memory_buffer_pct());

    for (auto& batch : run_state->batches) {
        const auto& spec = state.test_cases[batch.test_case_index];
        auto constraints = on_estimate_resources(
            data_dir, spec.name, spec.data);
        constraints.execution_tuples.clear();
        constraints.execution_tuples.reserve(batch.items.size());
        for (const auto& item : batch.items) {
            DEBUG_ASSERT(item.tuple != nullptr,
                "work-batch item must retain its expanded execution tuple");
            constraints.execution_tuples.push_back(resolved_execution_tuple{
                .algorithm_name = item.tuple->algorithm_name,
                .params = item.tuple->params,
            });
        }
        auto& slot = run_state->group_slots[batch.batch_index];
        slot.constraints = constraints;
        const bool total_overflow = constraints.extra_memory
            > (std::numeric_limits<std::size_t>::max)()
                - constraints.base_memory;
        // Saturate the diagnostic value on overflow, but reject before the
        // scheduler sees the batch.  This is the sole base+extra computation;
        // every later scheduler path consumes `scheduled_batch::total_bytes`.
        const std::size_t total_required = total_overflow
            ? (std::numeric_limits<std::size_t>::max)()
            : constraints.base_memory + constraints.extra_memory;
        const auto location_str = runtime_location(
            state.config.config_path, state.config.benchmark_key,
            spec.name, batch.group_index);

        constexpr std::size_t max_signed_scheduler_bytes =
            static_cast<std::size_t>(
                (std::numeric_limits<std::int64_t>::max)());
        if (total_overflow || total_required > max_signed_scheduler_bytes) {
            auto reason = fmt::format(
                "batch '{}' (group {}) has an unrepresentable memory request: "
                "base={} B, extra={} B, saturated_total={} B",
                spec.name, batch.group_index,
                constraints.base_memory, constraints.extra_memory,
                total_required);
            push_runtime_diagnostic(context, validation_diagnostic::diagnostic_severity::error,
                "memory_budget_not_satisfiable", location_str, reason);
            progress_mark_batch_aborted(context, batch, reason);
            continue;       // do not enqueue or convert to signed accounting
        }

        // G1.d  Hard error: combined requirements exceed total system RAM.
        if (total_required >= sys_at_enqueue.system_total_bytes) {
            auto reason = fmt::format(
                "batch '{}' (group {}) requires {} B (base+extra) but the host "
                "has only {} B total RAM",
                spec.name, batch.group_index,
                total_required, sys_at_enqueue.system_total_bytes);
            push_runtime_diagnostic(context, validation_diagnostic::diagnostic_severity::error,
                "memory_budget_not_satisfiable", location_str, reason);
            progress_mark_batch_aborted(context, batch, reason);
            continue;       // do not enqueue
        }
        // G1.c  Warning: combined requirements exceed buffered cap.
        if (total_required >= cap_at_enqueue) {
            emit_runtime_diagnostic(context, validation_diagnostic::diagnostic_severity::warning,
                "memory_budget_oversized", location_str,
                "batch '{}' (group {}) requires {} B but buffered cap is {} B "
                "(host total {} B, buffer {:.0f}%)",
                spec.name, batch.group_index,
                total_required, cap_at_enqueue,
                sys_at_enqueue.system_total_bytes, scheduler->memory_buffer_pct());
            // Still enqueue — scheduler will admit once peers free up.
        }

        scheduled_batch sb{};
        sb.benchmark_index    = context.benchmark_index;
        sb.bench_lock         = state.config.concurrency.bench_lock;
        sb.concurrency_serial = !state.config.concurrency.concurrency;
        sb.base_bytes         = constraints.base_memory;
        sb.extra_bytes        = constraints.extra_memory;
        sb.total_bytes        = static_cast<std::int64_t>(total_required);
        sb.group_for_ui       = progress_group_for(
            context, batch.test_case_index, batch.group_index);
        sb.location_str       = location_str;
        sb.test_case_name     = spec.name;
        sb.test_case_index    = batch.test_case_index;
        sb.group_index        = batch.group_index;
        sb.context            = &context;
        work_batch* const batch_ptr = &batch;
        sb.body = [run_state, batch_ptr] {
            run_state->run_batch(*batch_ptr);
        };
        scheduler->enqueue_batch(std::move(sb));
    }

    // Return a caller-thread finalizer instead of waiting from a oneTBB
    // driver task. All selected benchmarks finish preparation first, so the
    // scheduler can arbitrate their batches together. The shared run state
    // keeps every body capture alive until this join completes.
    const std::size_t benchmark_index = context.benchmark_index;
    return [scheduler, benchmark_index,
            context_ptr = &context, state_ptr = &state,
            run_state] {
        scheduler->wait_for_benchmark(benchmark_index);

        // --debug end-of-benchmark sweep: flush any started-but-unemitted
        // case (counter never hit 0 because some batches aborted / skipped /
        // idle-timed-out). All bodies have joined, so this is race-free.
        if (context_ptr->debug_sink != nullptr) {
            for (auto& [case_name, cp] : state_ptr->debug_progress_by_case) {
                if (cp.started.load(std::memory_order_relaxed)
                    && !cp.emitted.exchange(true, std::memory_order_acq_rel)) {
                    context_ptr->debug_sink->emit_case_results(
                        state_ptr->config.benchmark_key, case_name, *state_ptr);
                }
            }
        }

        run_state->run_batch = {};
        run_state->batches.clear();
        run_state->group_slots.clear();
    };
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
    const report_metadata& meta,
    const std::map<std::string, std::shared_ptr<benchmark_execution_state>>& reports,
    const std::vector<violation>& violations,
    const std::vector<regression_entry>& regressions,
    const std::vector<sanity_warning>& sanity_warnings,
    const nlohmann::json& raw_evidence_manifest)
{
    constexpr std::size_t k_initial_capacity = 1u << 18;  // 256 KiB up front
    simdjson::builder::string_builder builder{k_initial_capacity};

    /// Emit the run-identity/history-authority `meta` block.
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
            inner.append_comma();
            emit_key(inner, "history_eligible");
            inner.append(meta.history_eligible);
        });
    };

    /// Emit one violation as a JSON object — exactly the shape the spec
    /// `execution_tuple`, `test_case`, `metric`, `args`).
    auto emit_one_violation = [&](auto& b, const violation& rule_violation) {
        emit_object_with_body(b, [&](auto& inner) {
            emit_kv_string(inner, "location",  rule_violation.config_path);
            inner.append_comma();
            emit_kv_string(inner, "benchmark", rule_violation.benchmark_key);
            inner.append_comma();
            emit_kv_string(inner, "rule_name", rule_violation.rule_name);
            inner.append_comma();
            emit_kv_with_body(inner, "execution_tuple", [&](auto& tuple_b) {
                emit_object_with_body(tuple_b, [&](auto& tuple_inner) {
                    emit_key(tuple_inner, std::string_view{rule_violation.lhs_algorithm});
                    emit_param_object(tuple_inner, rule_violation.lhs_full_params);
                    // Emit RHS as a separate JSON property only when it
                    // names a different algorithm. When LHS and RHS share
                    // an algorithm name (e.g. a self-comparison rule), a
                    // second emit would produce duplicate keys → invalid
                    // JSON. The schema's `maxProperties: 2` tolerates the
                    // 1-key form already.
                    if (!rule_violation.rhs_algorithm.empty()
                        && rule_violation.rhs_algorithm != rule_violation.lhs_algorithm) {
                        tuple_inner.append_comma();
                        emit_key(tuple_inner, std::string_view{rule_violation.rhs_algorithm});
                        emit_param_object(tuple_inner, rule_violation.rhs_full_params);
                    }
                });
            });
            inner.append_comma();
            emit_kv_string(inner, "test_case", rule_violation.test_case);
            inner.append_comma();
            emit_kv_string(inner, "metric",    rule_violation.metric);
            inner.append_comma();
            emit_kv_string(
                inner, "comparison_mode",
                pairing_mode_report_name(rule_violation.comparison_mode));
            inner.append_comma();
            emit_key(inner, "warmup_repetitions");
            inner.append(rule_violation.warmup_repetitions);
            inner.append_comma();
            emit_key(inner, "blocking");
            inner.append(rule_violation.blocking);
            inner.append_comma();
            emit_kv_with_body(inner, "args", [&](auto& args_b) {
                args_b.start_array();
                args_b.append(rule_violation.args[0]);
                args_b.append_comma();
                args_b.append(rule_violation.args[1]);
                args_b.end_array();
            });
            inner.append_comma();
            emit_kv_with_body(inner, "args_present", [&](auto& present_b) {
                present_b.start_array();
                present_b.append(rule_violation.args_present[0]);
                present_b.append_comma();
                present_b.append(rule_violation.args_present[1]);
                present_b.end_array();
            });
        });
    };

    /// Emit one regression entry. Field order mirrors the user spec:
    /// location, benchmark, rule_name, algorithm_name, violating_tuples,
    /// test_cases, region, metric, aggregate, blocking, previously, now,
    /// regression_since.
    auto emit_one_regression = [&](auto& b, const regression_entry& entry) {
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
            emit_key(inner, "blocking");
            inner.append(entry.blocking);
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
    std::map<const benchmark_execution_state*, merged_samples_by_pair> merged_cache;
    auto merged_for_writer = [&](const benchmark_execution_state& state)
        -> const merged_samples_by_pair&
    {
        auto found = merged_cache.find(&state);
        if (found != merged_cache.end()) return found->second;
        return merged_cache.emplace(&state, merged_samples_for_state(state))
            .first->second;
    };

    // `emit_test_case_block` and `emit_one_execution_tuple` were lifted to the
    // free functions `emit_test_case_block` / `emit_execution_tuple_record`
    // above (shared with the --debug CASE_RESULTS sink — one serializer).

    /// Emit one benchmark's full sub-object (title / description / location /
    /// test_case_descriptions / execution_tuples).
    auto emit_one_benchmark = [&](auto& b,
                                    std::string_view key,
                                    const benchmark_execution_state& state) {
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
                        [&](auto& tuple_b, const execution_tuple& exec_tuple) {
                            emit_execution_tuple_record(tuple_b, state, exec_tuple,
                                                        merged_for_writer(state));
                        });
                    tuples_b.end_array();
                });
                if (state.raw_samples_enabled) {
                    inner.append_comma();
                    emit_kv_with_body(inner, "raw_samples", [&](auto& raw_b) {
                        raw_b.start_array();
                        std::vector<std::pair<std::uint64_t, const std::string*>>
                            ordered_raw;
                        for (const auto& cell : state.cells) {
                            for (const std::string& raw_sample : cell->raw_samples) {
                                const nlohmann::json parsed =
                                    nlohmann::json::parse(
                                        raw_sample, nullptr, false);
                                const auto id = parsed.is_discarded()
                                    ? std::nullopt
                                    : raw_json_u64(parsed, "raw_record_id");
                                DEBUG_ASSERT(id.has_value(),
                                    "validated raw sample lost raw_record_id");
                                if (id.has_value()) {
                                    ordered_raw.emplace_back(*id, &raw_sample);
                                }
                            }
                        }
                        std::sort(ordered_raw.begin(), ordered_raw.end(),
                            [](const auto& lhs, const auto& rhs) {
                                return lhs.first < rhs.first;
                            });
                        bool first_raw = true;
                        for (const auto& [id, raw_sample] : ordered_raw) {
                            (void)id;
                            if (!first_raw) raw_b.append_comma();
                            first_raw = false;
                            raw_b.append_raw(*raw_sample);
                        }
                        raw_b.end_array();
                    });
                }
            });
        });
    };

    /// Emit one sanity-warning entry. Field order:
    /// type, metric, message, warning_since, warning_since_commit.
    /// `warning_since` and `warning_since_commit` use the same
    /// empty-string ⇒ JSON-null convention as regression entries.
    auto emit_one_sanity_warning = [&](auto& b, const sanity_warning& warning) {
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
        emit_key(root_b, "raw_evidence");
        if (raw_evidence_manifest.is_null()) {
            root_b.append_null();
        } else {
            root_b.append_raw(raw_evidence_manifest.dump());
        }
        root_b.append_comma();
        emit_kv_with_body(
            root_b, "paired_rule_evidence", [&reports](auto& evidence_array) {
                evidence_array.start_array();
                bool first_evidence = true;
                for (const auto& [benchmark_key, state] : reports) {
                    (void)benchmark_key;
                    for (const auto& evidence :
                         state->paired_rule_evidence_records) {
                        if (!first_evidence) {
                            evidence_array.append_comma();
                        }
                        first_evidence = false;
                        emit_paired_rule_evidence_record(
                            evidence_array, evidence);
                    }
                }
                evidence_array.end_array();
            });
        root_b.append_comma();
        emit_kv_with_body(root_b, "rule_violations", [&](auto& arr_b) {
            arr_b.start_array();
            emit_range_with_commas(arr_b, violations,
                [&](auto& vb, const violation& rule_violation) {
                    emit_one_violation(vb, rule_violation);
                });
            arr_b.end_array();
        });
        root_b.append_comma();
        emit_kv_with_body(root_b, "regressions", [&](auto& arr_b) {
            arr_b.start_array();
            emit_range_with_commas(arr_b, regressions,
                [&](auto& vb, const regression_entry& entry) {
                    emit_one_regression(vb, entry);
                });
            arr_b.end_array();
        });
        root_b.append_comma();
        emit_kv_with_body(root_b, "sanity_warnings", [&](auto& arr_b) {
            arr_b.start_array();
            emit_range_with_commas(arr_b, sanity_warnings,
                [&](auto& vb, const sanity_warning& warning) {
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
        bioseq_log<logging_level::error>("[bench_framework] simdjson string_builder reported allocation failure");
        return {};
    }

    if (output_path.has_parent_path()) {
        std::filesystem::create_directories(output_path.parent_path());
    }
    std::ofstream output_file(output_path, std::ios::trunc | std::ios::binary);
    if (!output_file) {
        bioseq_log<logging_level::error>("[bench_framework] failed to open '{}' for writing", output_path.string());
        return {};
    }
    output_file.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    if (!output_file) {
        bioseq_log<logging_level::error>(
            "[bench_framework] failed while writing '{}'",
            output_path.string());
        output_file.close();
        return {};
    }
    output_file.close();
    if (!output_file) {
        bioseq_log<logging_level::error>(
            "[bench_framework] failed while closing '{}'",
            output_path.string());
        return {};
    }
    return output_path.string();
}

// ----------------- Stderr violations summary -----------------

/// Render a single `config_value` scalar to its plain-text form. Shared
/// builder — both want the value as a string but with different field
/// separators around it. Doubles use `{:.17g}` so an int → double
/// round-trip in a report (`5` parsed back as `5.0`) canonicalises onto the
/// integer form via the `floor(value) == value && |value| < 2^53` guard,
/// preventing identity-key drift after parsing and writing.
inline void append_config_value_text(std::string& out, const config_value& value) noexcept {
    switch (value.kind) {
        case config_value::value_kind::string:    out += value.string_value; return;
        case config_value::value_kind::integer:   out += std::to_string(value.integer_value); return;
        case config_value::value_kind::floating_point: {
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
        case config_value::value_kind::boolean:   out += value.boolean_value ? "true" : "false"; return;
        case config_value::value_kind::non_scalar: out += "<non-scalar>"; return;
    }
}

/// Render a parameter_map as a human-readable single-line `k=v, k=v, ...` blob
/// for the stderr violations summary. Not meant for re-parsing — use
/// `emit_param_object` for the JSON report.
[[nodiscard]] inline std::string format_param_map(const parameter_map& params) noexcept {
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
/// (`std::fprintf`) instead of bioseq_log<logging_level::debug> because the violation summary is
/// final-pass operator output, not part of the asynchronous log queue —
/// we want it visible even if the fmtlog background thread has already
/// drained and exited.
inline void print_violations(const std::vector<violation>& violations) noexcept {
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

    for (const auto& rule_violation : violations) {
        const std::string_view comparison_mode =
            pairing_mode_report_name(rule_violation.comparison_mode);
        const bool ratio_paired = rule_violation.comparison_mode
            == rule_comparator::pairing_mode::by_iteration_ratio;
        const bool exact_paired = rule_violation.comparison_mode
            == rule_comparator::pairing_mode::by_iteration_exact;
        const char* lhs_value_label = ratio_paired
            ? "paired-ratio aggregate"
            : exact_paired ? "first unequal iteration" : "aggregate";
        const char* rhs_value_label = ratio_paired
            ? "paired baseline"
            : exact_paired ? "first unequal iteration" : "aggregate";
        std::fprintf(stderr,
            "[%s] %s (test_case=%s, metric=%s, comparison=%s, warmup=%zu, %s)\n",
            rule_violation.benchmark_key.c_str(),
            rule_violation.rule_name.c_str(),
            rule_violation.test_case.c_str(),
            rule_violation.metric.c_str(),
            comparison_mode.data(),
            rule_violation.warmup_repetitions,
            rule_violation.blocking ? "BLOCKING" : "diagnostic");
        if (!rule_violation.args_present[0] || !rule_violation.args_present[1]) {
            std::fprintf(stderr,
                "    selected source unavailable: LHS=%s RHS=%s\n",
                rule_violation.args_present[0] ? "present" : "UNAVAILABLE",
                rule_violation.args_present[1] ? "present" : "UNAVAILABLE");
        }
        const auto lhs_param_text = format_param_map(rule_violation.lhs_full_params);
        const auto rhs_param_text = format_param_map(rule_violation.rhs_full_params);
        // Mirror the JSON writer's RHS-dedup: when both selectors named the
        // same algorithm with the same param map, print one combined line
        // instead of two visually identical sides.
        const bool same_side =
            rule_violation.lhs_algorithm == rule_violation.rhs_algorithm
            && lhs_param_text == rhs_param_text;
        if (same_side) {
            std::fprintf(stderr,
                "    self-compare on %s { %s }\n        LHS %s = %.6g\n        RHS %s = %.6g\n",
                rule_violation.lhs_algorithm.c_str(),
                lhs_param_text.c_str(),
                lhs_value_label,
                rule_violation.args[0],
                rhs_value_label,
                rule_violation.args[1]);
        } else {
            std::fprintf(stderr,
                "    LHS %s { %s }\n        %s = %.6g\n",
                rule_violation.lhs_algorithm.c_str(),
                lhs_param_text.c_str(),
                lhs_value_label,
                rule_violation.args[0]);
            std::fprintf(stderr,
                "    RHS %s { %s }\n        %s = %.6g\n",
                rule_violation.rhs_algorithm.c_str(),
                rhs_param_text.c_str(),
                rhs_value_label,
                rule_violation.args[1]);
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
inline void print_regressions(const std::vector<regression_entry>& regressions) noexcept {
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
            "[%s] %s (rule=%s, classification=%s, test_case=%s, region=%s, metric=%s, aggregate=%s)\n",
            entry.benchmark.c_str(),
            entry.algorithm_name.c_str(),
            entry.rule_name.c_str(),
            entry.blocking ? "BLOCKING" : "diagnostic",
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
inline void print_sanity_warnings(const std::vector<sanity_warning>& warnings) noexcept {
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
struct summary_issue {
    enum class issue_kind : std::uint8_t {
        regression,
        rule_violation,
        sanity_warning,
    };

    issue_kind kind = issue_kind::sanity_warning;
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
    /// benchmark_rule:        [lhs_aggregate, rhs_aggregate]
    /// Regression:  [previously, now]
    /// Sanity:      ignored
    std::array<double, 2>         latest_args{0.0, 0.0};
    /// Latest-report aggregated samples used by `amplitude_factor`.
    /// Default-constructed (`samples_count == 0`) for sanity
    /// warnings or when the matching cell wasn't found.
    aggregated_samples             latest_samples{};
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
struct severity_bucket {
    double            min_score;
    std::string_view  emoji;
    ftxui::Color      color;
    std::string_view  label;
};

inline const std::array<severity_bucket, 5> k_severity_buckets{ {
    { 1500.0, "\xe2\x9b\x94",      ftxui::Color::Red,         "critical"  }, // ⛔
    { 1000.0, "\xf0\x9f\x94\xb4",  ftxui::Color::RedLight,    "high"      }, // 🔴
    {  600.0, "\xe2\xad\x95",      ftxui::Color::Magenta,     "medium-hi" }, // ⭕
    {  300.0, "\xf0\x9f\x9f\xa0",  ftxui::Color::Yellow,      "medium"    }, // 🟠
    {    0.0, "\xf0\x9f\x9f\xa1",  ftxui::Color::YellowLight, "low"       }, // 🟡
} };

/// Look up the bucket whose `min_score` is the highest one ≤ `score`.
/// Sentinel: the last entry has `min_score == 0`, so the loop always
/// finds a hit for any non-negative score.
[[nodiscard]] inline const severity_bucket& bucket_for(double score) noexcept {
    for (const auto& bucket : k_severity_buckets) {
        if (score >= bucket.min_score) return bucket;
    }
    return k_severity_buckets.back();
}

/// Per-slot block glyph for an issue's presence row.
/// Order of preconditions: missing report (🟦) wins over presence
/// state because we cannot say anything about an unparseable file.
[[nodiscard]] inline std::string_view summary_block_for(
    summary_issue::issue_kind kind,
    bool present,
    bool report_data_present) noexcept
{
    if (!report_data_present) return "\xf0\x9f\x9f\xa6";   // 🟦
    if (!present)             return "\xf0\x9f\x9f\xa9";   // 🟩
    if (kind == summary_issue::issue_kind::sanity_warning) return "\xf0\x9f\x9f\xa8"; // 🟨
    return "\xf0\x9f\x9f\xa5";                              // 🟥
}

/// 64-bit FNV-1a hash. Used to fingerprint the sanity-warning
/// `message` field so two genuinely different constant values on the
/// same metric (e.g. "490" vs "0") don't collide in the deduplicated
/// view.
[[nodiscard]] constexpr std::uint64_t fnv1a64(std::string_view text) noexcept {
    constexpr std::uint64_t k_fnv_offset_basis = 0xcbf29ce484222325ULL;
    constexpr std::uint64_t k_fnv_prime       = 0x00000100000001B3ULL;
    std::uint64_t hash = k_fnv_offset_basis;
    for (const char ch : text) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
        hash *= k_fnv_prime;
    }
    return hash;
}

// Field separators for identity strings. ASCII 0x1e (record
// separator) and 0x1f (unit separator) are reserved control
// characters in JSON-clean input, so config-supplied identifiers
// cannot inject ambiguity here.
inline constexpr char k_summary_identity_rs = '\x1e';
inline constexpr char k_summary_identity_us = '\x1f';

/// `k1=v1\x1fk2=v2\x1f...`. Iteration is alphabetical (`parameter_map` is
/// `std::map`).
[[nodiscard]] inline std::string format_param_map_stable(const parameter_map& params) noexcept {
    std::string result;
    bool first = true;
    for (const auto& [name, value] : params) {
        if (!first) result += k_summary_identity_us;
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
/// Used by the three `make_*_identity` callers below so every issue kind uses
/// the same delimiter contract.
template<typename... p_fields>
[[nodiscard]] inline std::string join_identity(
    std::string_view kind, const p_fields&... fields) noexcept
{
    std::string identity{kind};
    identity += k_summary_identity_rs;
    bool first = true;
    auto append_one = [&](std::string_view field) noexcept {
        if (!first) identity += k_summary_identity_us;
        first = false;
        identity += field;
    };
    (append_one(std::string_view{fields}), ...);
    return identity;
}

/// Identity helpers — one tiny call to `join_identity` each. See
/// on `make_violation_identity` reserve slots in the identity tuple. Callers
/// pass the constant defaults `"all_regions"` / `"p50"`; these slots are part
/// of the identity contract even though per-region rules do not exist yet.
[[nodiscard]] inline std::string make_violation_identity(
    std::string_view config_path,
    std::string_view benchmark,
    std::string_view rule_name,
    std::string_view lhs_algorithm,
    const parameter_map&  lhs_params,
    std::string_view rhs_algorithm,
    const parameter_map&  rhs_params,
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
    const parameter_map&  params,
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
/// sites (rule-violation sides, regression `violating_tuples[0]`,
/// and `build_sample_index`'s `tuple` block); extracting the loop
/// keeps the framework's three callers from drifting out of sync.
[[nodiscard]] inline parameter_map parse_param_map(simdjson::dom::object obj) noexcept {
    parameter_map params;
    for (auto field : obj) {
        params.emplace(std::string{field.key},
                       config_value::from_simdjson(field.value));
    }
    return params;
}

/// amplitude lookup. The path is
/// `benchmark\x1falgorithm\x1fparams\x1ftest_case\x1fregion\x1fmetric`
/// (a subset of the issue identity, minus the kind/rule_name
/// fields) and matches what the issue walk produces when it asks
/// for the aggregated_samples of "the cell that triggered this
/// pre-aggregated records instead of raw vectors.
using sample_index = std::map<std::string, aggregated_samples>;

/// Build the per-cell sample-array path key.
[[nodiscard]] inline std::string make_sample_path(
    std::string_view benchmark,
    std::string_view algorithm,
    const parameter_map&  params,
    std::string_view test_case,
    std::string_view region,
    std::string_view metric) noexcept
{
    std::string path;
    path += benchmark;   path += k_summary_identity_us;
    path += algorithm;   path += k_summary_identity_us;
    path += format_param_map_stable(params);
    path += k_summary_identity_us;
    path += test_case;   path += k_summary_identity_us;
    path += region;      path += k_summary_identity_us;
    path += metric;
    return path;
}

/// Walk a parsed report's full `benchmarks.<>.execution_tuples[*].
/// test_cases.<>.<region>.<metric>` tree once, building a flat
/// `sample_index` keyed by the same path the summary's identity
/// uses. Each leaf is parsed via `parse_aggregated_samples` so
/// size); called at most once per report regardless of how many
/// issues need amplitude data.
[[nodiscard]] inline sample_index build_sample_index(
    simdjson::dom::element report_root) noexcept
{
    sample_index index;
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
                parameter_map params = parse_param_map(*params_obj);
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

/// Look up an `aggregated_samples` in a per-report `sample_index`.
/// Returns a default-constructed aggregate (`samples_count == 0`)
/// when the cell is absent — `amplitude_factor` already treats
/// `samples_count < 2` as "no amplitude info" and falls back to 1.0.
[[nodiscard]] inline aggregated_samples samples_from_index(
    const sample_index& index,
    std::string_view benchmark,
    std::string_view algorithm,
    const parameter_map&  params,
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
struct extracted_issue {
    std::string                          identity;
    summary_issue::issue_kind                   kind = summary_issue::issue_kind::sanity_warning;
    /// Initialiser for fields that are intrinsic to the issue's
    /// identity (metric / rule_name / scope_label / etc.). Run
    /// once on first insertion.
    std::function<void(summary_issue&)>   update_first;
    /// Updater for fields that may move between reports
    /// (latest_args, latest_samples, sanity_message). Run only when
    /// `report_index > issue.latest_seen_index`.
    std::function<void(summary_issue&)>   update_latest;
};

/// Generic per-report-array walker. The three issue kinds (rule
/// violations, regressions, sanity warnings) all follow the same
/// shape: pull `report_root[array_key]`, iterate entries, build an
/// identity, and merge into `aggregation`. Only the per-entry
/// extraction differs — encoded in the `Extract` callable.
template<typename p_extract>
inline void walk_report_array(
    simdjson::dom::element                report_root,
    std::string_view                       array_key,
    std::size_t                            report_index,
    std::size_t                            max_index,
    std::map<std::string, summary_issue>&   aggregation,
    p_extract                              extract) noexcept
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
        extracted_issue ex = std::move(*extracted);

        // `try_emplace` distinguishes "first encounter" from "merge
        // into existing" cleanly. We move the identity string into
        // the map key on insert; on the merge path the identity
        // already lives both as the map key and on the existing
        // summary_issue (set the first time), so the moved-from
        // `ex.identity` is not consulted again.
        auto [it, inserted] = aggregation.try_emplace(std::move(ex.identity));
        summary_issue& issue = it->second;
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
/// Returns an empty `parameter_map` when the array is missing or empty.
[[nodiscard]] inline parameter_map parse_violating_tuple_first(simdjson::dom::object obj) noexcept {
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

/// Pull every `(<algo_name>, <parameter_map>)` side from a
/// `rule_violations[*].execution_tuple`. May return one (self-compare)
/// or two entries.
[[nodiscard]] inline std::vector<std::pair<std::string, parameter_map>>
parse_violation_sides(simdjson::dom::object obj) noexcept
{
    std::vector<std::pair<std::string, parameter_map>> sides;
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

inline constexpr double k_summary_base_regression    = 1000.0;
inline constexpr double k_summary_base_rule_violation =  700.0;
inline constexpr double k_summary_base_sanity_warning =  300.0;

[[nodiscard]] constexpr double base_severity(summary_issue::issue_kind kind) noexcept {
    switch (kind) {
        case summary_issue::issue_kind::regression:     return k_summary_base_regression;
        case summary_issue::issue_kind::rule_violation:  return k_summary_base_rule_violation;
        case summary_issue::issue_kind::sanity_warning:  return k_summary_base_sanity_warning;
    }
    return k_summary_base_sanity_warning;
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
/// dashboard display). The `k_eps` clamp on the denominator
/// prevents the divide-by-zero when `baseline == 0` while keeping
/// the displayed percentage stable across consumers.
[[nodiscard]] inline double relative_diff_pct(double current, double baseline) noexcept {
    constexpr double k_eps = 1e-12;
    return std::abs(current - baseline) / std::max(std::abs(baseline), k_eps) * 100.0;
}

/// Relative amplitude percentage `100 * (max - min) / |min|` of
/// a pre-aggregated sample set. Used by both `amplitude_factor`
/// and the dashboard's stat blurb. Returns 0 for fewer than two
/// finite samples (no amplitude to speak of). The `max(|min|, k_eps)`
/// clamp prevents divide-by-zero when min == 0 (e.g. all-zero
/// in-memory and historical data uniformly.
[[nodiscard]] inline double relative_amplitude_pct(
    const aggregated_samples& aggregated) noexcept
{
    if (aggregated.samples_count < 2) return 0.0;
    if (aggregated.top10.empty())     return 0.0;     // all-non-finite cell
    constexpr double k_eps = 1e-12;
    return 100.0 * (aggregated.stats.max - aggregated.stats.min)
                 / std::max(std::abs(aggregated.stats.min), k_eps);
}

/// Multiplicative value-percentage factor. log-shaped so a 555%
/// overshoot and a 5550% overshoot don't collapse onto the same
/// number. A near-zero denominator (`baseline == 0` for a
/// regression rising to anything) saturates at `k_factor_ceil`
/// directly — the log path would clamp there anyway, but the
/// explicit branch makes intent obvious.
[[nodiscard]] inline double value_pct_factor(double current, double baseline) noexcept {
    constexpr double k_eps         = 1e-12;
    constexpr double k_slope       = 0.5;
    constexpr double k_factor_ceil  = 3.0;
    if (std::abs(baseline) < k_eps) return k_factor_ceil;
    const double pct = relative_diff_pct(current, baseline);
    const double bonus = k_slope * std::log10(1.0 + pct / 100.0);
    const double factor = 1.0 + bonus;
    return factor < k_factor_ceil ? factor : k_factor_ceil;
}

/// Multiplicative amplitude factor. High-noise samples reduce
/// the score because the regression / rule violation may be
/// within natural variability. Curve mirrors the user spec:
/// factor = 1.0 at amp = 0%, factor = 1/6 at amp = 10000%.
[[nodiscard]] inline double amplitude_factor(
    const aggregated_samples& aggregated) noexcept
{
    if (aggregated.samples_count < 2) return 1.0;
    constexpr double k_cap_pct = 10000.0;
    const double rel_amp_pct = relative_amplitude_pct(aggregated);
    const double clipped     = rel_amp_pct < k_cap_pct ? rel_amp_pct : k_cap_pct;
    const double normalised  = clipped / 1000.0;
    return (12.0 - normalised) / 12.0;
}

/// Compute (recent_streak, any_old_gap) from a presence array.
struct presence_pattern {
    bool        present_in_latest = false;
    std::size_t recent_streak     = 0;
    bool        any_old_gap       = false;
};

[[nodiscard]] inline presence_pattern compute_presence_pattern(
    std::span<const std::uint8_t> presence) noexcept
{
    presence_pattern result;
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
[[nodiscard]] inline double score_summary_issue(const summary_issue& issue) noexcept {
    const auto pattern = compute_presence_pattern(issue.presence_per_report);
    const double recurrence = recurrence_factor(
        pattern.present_in_latest,
        pattern.recent_streak,
        pattern.any_old_gap,
        issue.presence_per_report.size());
    const double base = base_severity(issue.kind);
    if (issue.kind == summary_issue::issue_kind::sanity_warning) {
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
[[nodiscard]] inline std::vector<const summary_issue*> sort_summary_issues(
    const std::map<std::string, summary_issue>& aggregation) noexcept
{
    std::vector<const summary_issue*> sorted;
    sorted.reserve(aggregation.size());
    for (const auto& [_identity, issue] : aggregation) sorted.push_back(&issue);
    std::sort(sorted.begin(), sorted.end(),
        [](const summary_issue* lhs, const summary_issue* rhs) noexcept {
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
    constexpr std::string_view k_ellipsis = "\xe2\x80\xa6";   // "…" (3 bytes)
    if (max_chars <= k_ellipsis.size()) return std::string(max_chars, '.');
    std::size_t cut = max_chars - k_ellipsis.size();
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
    text += k_ellipsis;
    return text;
}

/// Build the per-issue stat blurb shown on each row. Reuses the
/// `relative_diff_pct` / `relative_amplitude_pct` helpers so the
/// numbers in the dashboard align with the numbers in the score.
[[nodiscard]] inline std::string format_stat_blurb(const summary_issue& issue) noexcept {
    const double amp = relative_amplitude_pct(issue.latest_samples);
    switch (issue.kind) {
        case summary_issue::issue_kind::rule_violation: {
            const double signed_diff = issue.latest_args[1] == 0.0
                ? relative_diff_pct(issue.latest_args[0], issue.latest_args[1])
                : (issue.latest_args[0] - issue.latest_args[1])
                  / std::abs(issue.latest_args[1]) * 100.0;
            return fmt::format("rule  lhs={:.6g} vs rhs={:.6g}  (Δ {:+.1f}%, amp ±{:.0f}%)",
                issue.latest_args[0], issue.latest_args[1], signed_diff, amp);
        }
        case summary_issue::issue_kind::regression: {
            const double signed_diff = issue.latest_args[0] == 0.0
                ? relative_diff_pct(issue.latest_args[1], issue.latest_args[0])
                : (issue.latest_args[1] - issue.latest_args[0])
                  / std::abs(issue.latest_args[0]) * 100.0;
            return fmt::format("regr  now={:.6g} vs prev={:.6g}  (Δ {:+.1f}%, amp ±{:.0f}%)",
                issue.latest_args[1], issue.latest_args[0], signed_diff, amp);
        }
        case summary_issue::issue_kind::sanity_warning: {
            std::string_view message{issue.sanity_message};
            if (auto dot = message.find('.'); dot != std::string_view::npos) {
                message = message.substr(0, dot);
            }
            constexpr std::size_t k_sanity_blurb_max_chars = 40;
            return clip_to_width(fmt::format("sanity  {}", message), k_sanity_blurb_max_chars);
        }
    }
    return {};
}

/// Build the per-issue presence-block ftxui Element row. Shows the
/// last `k_block_window` slots, oldest on the left → newest on the
/// right. Glyphs are emitted as one element-per-cell so ftxui sizes
/// them uniformly.
inline constexpr std::size_t k_summary_block_window = 8;

[[nodiscard]] inline ftxui::Elements presence_blocks(const summary_issue& issue) noexcept {
    using namespace ftxui;
    const std::size_t total = issue.presence_per_report.size();
    const std::size_t take  = total < k_summary_block_window ? total : k_summary_block_window;
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
using summary_group_key = std::pair<std::string, std::string>;

/// Convert one fully-scored issue into its dashboard row. Severity
/// emoji + colour, kind label (bold), stat blurb, presence row, dim
/// rule name on the right.
[[nodiscard]] inline ftxui::Element render_issue_row(const summary_issue& issue) noexcept {
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
        constexpr std::size_t k_rule_name_max_chars = 24;
        row.push_back(text(clip_to_width(issue.rule_name, k_rule_name_max_chars)) | dim);
    }
    return hbox(std::move(row));
}

/// Render the whole dashboard. Two-pass aggregation: outer
/// metric-section header, inner scope-section sub-header, leaf
/// issue rows. Trailing legend doubles as the colour key.
///
/// We write the rendered ftxui screen via `std::fputs(stderr)` rather
/// than `bioseq_log<logging_level::debug>` — this matches `print_violations` /
/// printers) which already bypass fmtlog so end-of-run operator output
/// stays visible after the asynchronous log queue drains.
inline void render_summary(
    std::span<const summary_issue* const> sorted_issues,
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
    for (const auto& bucket : k_severity_buckets) {
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
[[nodiscard]] inline auto rule_violation_extractor(const sample_index& samples_by_path) noexcept {
    return [&samples_by_path](simdjson::dom::object obj)
        -> std::optional<extracted_issue>
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
        std::pair<std::string, parameter_map> lhs = std::move(sides.front());
        std::pair<std::string, parameter_map> rhs = sides.size() > 1
            ? std::move(sides[1])
            : lhs;
        const auto args = parse_violation_args(obj);

        extracted_issue ex;
        ex.kind     = summary_issue::issue_kind::rule_violation;
        ex.identity = make_violation_identity(
            location, benchmark, rule_name,
            lhs.first, lhs.second, rhs.first, rhs.second,
            test_case, metric);

        ex.update_first = [metric, test_case, benchmark, rule_name,
                           lhs_algo = lhs.first, rhs_algo = rhs.first]
                          (summary_issue& issue) noexcept {
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
                            test_case, metric, &samples_by_path]
                           (summary_issue& issue) noexcept {
            issue.latest_args = args;
            // Amplitude reads the LHS cell's samples (the side that
            // against `all_regions`; per-region rules would need this
            // hard-coded "all_regions" replaced with the rule's region.
            issue.latest_samples = samples_from_index(
                samples_by_path, benchmark, lhs_algo, lhs_params,
                test_case, "all_regions", metric);
        };
        return ex;
    };
}

/// Build the regression extractor lambda. Same shape as the rule
/// extractor; the regression's stored `region` flows into both the
/// identity and the sample lookup.
[[nodiscard]] inline auto regression_extractor(const sample_index& samples_by_path) noexcept {
    return [&samples_by_path](simdjson::dom::object obj)
        -> std::optional<extracted_issue>
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

        extracted_issue ex;
        ex.kind     = summary_issue::issue_kind::regression;
        ex.identity = make_regression_identity(
            location, benchmark, rule_name, algorithm_name, params,
            test_case, region, metric, aggregate);

        ex.update_first = [metric, test_case, benchmark, rule_name,
                           algorithm_name, region, aggregate]
                          (summary_issue& issue) noexcept {
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
                            metric, &samples_by_path]
                           (summary_issue& issue) noexcept {
            issue.latest_args = {previously, now_value};
            issue.latest_samples = samples_from_index(
                samples_by_path, benchmark, algorithm_name, params,
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
    return [](simdjson::dom::object obj) -> std::optional<extracted_issue> {
        const auto type    = get_string_field(obj, "type");
        const auto metric  = get_string_field(obj, "metric");
        const auto message = get_string_field(obj, "message");

        extracted_issue ex;
        ex.kind     = summary_issue::issue_kind::sanity_warning;
        ex.identity = make_sanity_identity(type, metric, message);
        ex.update_first = [type, metric, message](summary_issue& issue) noexcept {
            issue.metric         = metric;
            issue.scope_label    = "(global)";
            issue.rule_name      = type;
            issue.sanity_message = message;
        };
        ex.update_latest = [message](summary_issue& issue) noexcept {
            issue.sanity_message = message;
        };
        return ex;
    };
}

/// scores issues, and renders the aggregated dashboard to stderr via
/// `render_summary`. Used as the read-only fallback when `--ui` is
/// and from `ui_mode_main`'s startup probe when the user explicitly
/// requested ASCII-only output but no TTY is attached.
[[nodiscard]] inline int render_summary_to_stderr_once(const std::string& report_dir) noexcept {
    const std::filesystem::path report_dir_path{report_dir};
    const auto max_index = max_existing_report_index(report_dir_path).value_or(0);
    if (max_index == 0) {
        bioseq_log<logging_level::info>("[bench_framework] summary: no reports under '{}'", report_dir);
        return 0;
    }

    historical_report_cache cache{report_dir_path};
    std::map<std::string, summary_issue> aggregation;
    std::vector<std::uint8_t> report_data_present(max_index, 0);
    std::size_t reports_present_count = 0;

    for (std::size_t report_index = 1; report_index <= max_index; ++report_index) {
        auto* report = cache.fetch(report_index);
        if (report == nullptr) continue;     // gap → presence stays 0
        report_data_present[report_index - 1] = 1;
        ++reports_present_count;
        const auto samples_by_path = build_sample_index(report->root);
        walk_report_array(report->root, "rule_violations",
            report_index, max_index, aggregation,
            rule_violation_extractor(samples_by_path));
        walk_report_array(report->root, "regressions",
            report_index, max_index, aggregation,
            regression_extractor(samples_by_path));
        walk_report_array(report->root, "sanity_warnings",
            report_index, max_index, aggregation,
            sanity_warning_extractor());
    }

    if (reports_present_count == 0) {
        bioseq_log<logging_level::info>("[bench_framework] summary: every report under '{}' failed to parse",
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
    render_summary(std::span<const summary_issue* const>{
                       sorted_issues.data(), sorted_issues.size()},
                   max_index, reports_present_count);
    return 0;
}


/// Forward declaration for the interactive ftxui dashboard. Defined in
/// the sibling implementation unit `interactive.cppm`. Invoked from
/// `run_impl` when `--ui` is on the command line and stdin / stderr
/// are interactive TTYs. When the TTY check fails the entry point
/// itself dispatches `render_summary_to_stderr_once` so a CI-piped
[[nodiscard]] int ui_mode_main(const std::string& report_dir, const parsed_cli& cli) noexcept;

// ----------------- CLI parsing -----------------

/// Parse one `key=v1,v2,...` spec into `filter`. Records human-readable
/// "<code>:<message>" strings into `errors` on any problem. Empty values,
/// empty items, unknown keys, and repeated dimensions are all errors.
inline void parse_one_filter_into(std::string_view spec,
                                  filter_specification& filter,
                                  std::vector<std::string>& errors) noexcept {
    const auto eq = spec.find('=');
    if (eq == std::string_view::npos || eq == 0) {
        errors.emplace_back(fmt::format(
            "filter_malformed:--filter '{}' is not of the form key=value", spec));
        return;
    }
    const std::string_view key = spec.substr(0, eq);
    const std::string_view rhs = spec.substr(eq + 1);
    if (rhs.empty()) {
        errors.emplace_back(fmt::format(
            "filter_malformed:--filter '{}' has an empty value list", spec));
        return;
    }
    // Split rhs on ',', rejecting empty items (e.g. "a,,b" or "a,").
    std::vector<std::string> values;
    std::size_t start = 0;
    while (start <= rhs.size()) {
        const auto comma = rhs.find(',', start);
        const auto end = (comma == std::string_view::npos) ? rhs.size() : comma;
        const std::string_view item = rhs.substr(start, end - start);
        if (item.empty()) {
            errors.emplace_back(fmt::format(
                "filter_malformed:--filter '{}' has an empty item", spec));
            return;
        }
        values.emplace_back(item);
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    // Bind to the right axis; reject a second use of the same axis.
    auto bind = [&values, &errors](std::optional<std::vector<std::string>>& slot,
                                   std::string_view dim_name) {
        if (slot.has_value()) {
            errors.emplace_back(fmt::format(
                "filter_duplicate_dimension:--filter {} given more than once", dim_name));
            return;
        }
        slot = std::move(values);
    };
    if      (key == "tags")      bind(filter.tags,      "tags");
    else if (key == "benchmark") bind(filter.benchmark, "benchmark");
    else if (key == "cases")     bind(filter.cases,     "cases");
    else errors.emplace_back(fmt::format(
        "filter_unknown_key:--filter key '{}' must be tags|benchmark|cases", key));
}

/// Parse the framework CLI flags out of `argv`. Unknown flags are
/// silently ignored (the user may layer their own flag parsing on top by
/// inspecting argv before calling `run`). Runs first inside `run_impl` so a
/// `--report` override is applied before any directory creation.
[[nodiscard]] inline parsed_cli parse_cli(int argc, char** argv) noexcept {
    parsed_cli parsed;
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
                bioseq_log<logging_level::error>("[bench_framework] --threads requires a positive integer (got '{}')",
                     text);
            } else {
                parsed.threads_override = static_cast<unsigned>(threads);
            }
            ++i;
        } else if (arg == "--report" && (i + 1) < argc) {
            parsed.report_override = std::string{argv[i + 1]};
            ++i;
        } else if (arg == "--memory-buffer" && (i + 1) < argc) {
            // in framework code" — the existing --threads parser used
            // try/catch around std::stoi, but we don't extend that
            // pattern.
            const std::string_view text{argv[i + 1]};
            int pct = 0;
            const auto* const begin = text.data();
            const auto* const end   = text.data() + text.size();
            const auto result = std::from_chars(begin, end, pct);
            if (result.ec != std::errc{} || result.ptr != end) {
                bioseq_log<logging_level::error>("[bench_framework] --memory-buffer requires an integer (got '{}')",
                     text);
            } else if (pct < 0 || pct > 50) {
                bioseq_log<logging_level::error>("[bench_framework] --memory-buffer must be in [0, 50] "
                     "(got {}); falling back to default 10", pct);
            } else {
                parsed.memory_buffer_pct = static_cast<double>(pct);
            }
            ++i;
        } else if (arg == "--debug") {
            parsed.debug = true;
        } else if (arg == "--filter" && (i + 1) < argc) {
            const std::string_view spec{argv[i + 1]};
            ++i;
            parse_one_filter_into(spec, parsed.filter, parsed.cli_errors);
        } else if (arg == "--filter") {
            parsed.cli_errors.emplace_back(
                "filter_malformed:--filter requires an argument like tags=a,b");
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
struct gauge_bucket {
    float                   min_fraction;
    ftxui::Color::Palette256 palette;
};
inline constexpr std::array<gauge_bucket, 4> k_gauge_buckets{ {
    { 0.00f, static_cast<ftxui::Color::Palette256>(ftxui::Color::Red)     },
    { 0.25f, ftxui::Color::Orange1                                       },
    { 0.50f, static_cast<ftxui::Color::Palette256>(ftxui::Color::Yellow) },
    { 0.75f, static_cast<ftxui::Color::Palette256>(ftxui::Color::Green)  },
} };

[[nodiscard]] inline constexpr ftxui::Color::Palette256
gauge_palette(float fraction) noexcept {
    auto picked = k_gauge_buckets.front().palette;
    for (const auto& bucket : k_gauge_buckets) {
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
    constexpr std::string_view k_full  = "\xe2\x96\x88";   // █
    constexpr std::string_view k_empty = "\xe2\x96\x91";   // ░
    if (total_cells <= 0) return ftxui::text("[]");
    const float clamped = std::clamp(fraction, 0.0f, 1.0f);
    const int filled_cells = std::clamp(
        static_cast<int>(std::lround(
            static_cast<double>(clamped) * static_cast<double>(total_cells))),
        0, total_cells);
    std::string bar;
    bar.reserve(static_cast<std::size_t>(total_cells) * k_full.size() + 2);
    bar.push_back('[');
    for (int i = 0; i < total_cells; ++i) bar += (i < filled_cells ? k_full : k_empty);
    bar.push_back(']');
    return ftxui::text(bar) | ftxui::color(gauge_color(clamped));
}

/// Convert raw bytes to a `double` in GiB. Inlined as a `constexpr`
/// helper so callers don't repeat the `1024^3` divisor.
[[nodiscard]] inline constexpr double bytes_to_gib(std::uint64_t bytes) noexcept {
    constexpr double k_gib = 1024.0 * 1024.0 * 1024.0;
    return static_cast<double>(bytes) / k_gib;
}

/// Background thread body for the progress dashboard. Two paths:
/// 1. **TTY**: render an ftxui document to stderr every 200 ms with
///    one row per test_case (active groups underneath), a colour-
///    shifting gauge per row, and a `[memory X / Y GiB]` header.
/// 2. **Non-TTY (CI / pipes)**: emit a bioseq_log<logging_level::debug> info line every 5 s
///    with one line per test_case still in flight.

/// Possible states of a test_case row in the dashboard. Renderer picks
/// the most-urgent state across the test case's groups.
enum class test_case_row_state : std::uint8_t {
    done,
    aborted,
    input_retry,
    cleanup_retry,
    idle_wait,
    running,
};

/// Walk every group of `tc` and pick the most-urgent state. The order
/// (Aborted > InputRetry > CleanupRetry > Done > IdleWait > Running)
/// mirrors the user spec — failures and aborts surface first; an
/// already-finished test case stays "Done" even if its idle clock
/// briefly armed before it completed.
[[nodiscard]] inline test_case_row_state classify_test_case(
    const progress_test_case& tc) noexcept
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
                                        bioseq::instrumentation::clock::now_ns()) - idle_start;
            const auto half_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    batch_scheduler::k_max_idle_waiting_time).count() / 2;
            if (elapsed_ns > half_ns) any_idle_long = true;
        }
    }
    if (any_aborted)         return test_case_row_state::aborted;
    if (any_input_retry)     return test_case_row_state::input_retry;
    if (any_cleanup_retry)   return test_case_row_state::cleanup_retry;
    if (tc.finish_ns.load(std::memory_order_acquire) != 0)
                             return test_case_row_state::done;
    if (any_idle_long)       return test_case_row_state::idle_wait;
    return test_case_row_state::running;
}

/// Header glyph + colour per row state. Table-driven so adding a new
/// state means adding a row, not editing branch ladders.
struct row_glyph {
    test_case_row_state  state;
    std::string_view  glyph;        // UTF-8 (may include emoji)
    ftxui::Color      tint;
};

inline const std::array<row_glyph, 6> k_row_glyphs{ {
    { test_case_row_state::aborted,
      "\xe2\x9d\x8c",                      ftxui::Color::RedLight   },  // ❌
    { test_case_row_state::input_retry,
      "\xe2\x96\xb6 \xe2\x9b\x94",         ftxui::Color::Red        },  // ▶ ⛔
    { test_case_row_state::cleanup_retry,
      "\xe2\x96\xb6 \xf0\x9f\xa7\xb9",     ftxui::Color::Magenta    },  // ▶ 🧹
    { test_case_row_state::idle_wait,
      "\xe2\x96\xb6 \xf0\x9f\x95\x91",     ftxui::Color::Yellow     },  // ▶ 🕑
    { test_case_row_state::running,
      "\xe2\x96\xb6 \xf0\x9f\x94\x84",     ftxui::Color::Yellow     },  // ▶ 🔄
    { test_case_row_state::done,
      "\xe2\x9c\x85",                      ftxui::Color::Green      },  // ✅
} };

[[nodiscard]] inline const row_glyph& glyph_for(test_case_row_state state) noexcept {
    for (const auto& entry : k_row_glyphs) {
        if (entry.state == state) return entry;
    }
    return k_row_glyphs.front();
}

/// Build the detail-rows that hang off a failing/idle row. Returned as
/// a list of ftxui Elements ready to be flattened into the renderer's
/// `rows` vector.
[[nodiscard]] inline ftxui::Elements build_detail_rows(
    const progress_test_case& tc, test_case_row_state state) noexcept
{
    using namespace ftxui;
    Elements detail;
    if (state == test_case_row_state::running || state == test_case_row_state::done) {
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
        if (state == test_case_row_state::aborted && aborted) {
            std::lock_guard<std::mutex> guard(group_ptr->failure_mu);
            message = group_ptr->aborted_reason;
            header_line = fmt::format(
                "group {} aborted",
                group_ptr->group_index + 1);
            tint = Color::RedLight;
        } else if (state == test_case_row_state::input_retry && input_n > 0) {
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
                k_max_task_input_handler_error_count);
            tint = Color::Red;
        } else if (state == test_case_row_state::cleanup_retry && cleanup_n > 0) {
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
                k_max_task_input_handler_error_count);
            tint = Color::Magenta;
        } else if (state == test_case_row_state::idle_wait && idle_start != 0) {
            const auto elapsed_ns = static_cast<std::int64_t>(
                                        bioseq::instrumentation::clock::now_ns()) - idle_start;
            const auto half_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    batch_scheduler::k_max_idle_waiting_time).count() / 2;
            if (elapsed_ns <= half_ns) continue;
            const auto elapsed_s = elapsed_ns / 1'000'000'000;
            const auto budget_s  = std::chrono::duration_cast<std::chrono::seconds>(
                                       batch_scheduler::k_max_idle_waiting_time).count();
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

/// `finish_ns` is stamped, then disappears. Failures (Aborted /
/// InputRetry / CleanupRetry) are *never* hidden — the operator must
/// see them.
inline constexpr std::chrono::seconds k_completed_row_grace_period{5};

/// Free helper — the auto-hide predicate. Used by both the per-row
/// loop (skip rendering this row) and the all-hidden collapse loop
/// (collapse the entire benchmark when every row is past the grace).
[[nodiscard]] inline bool row_is_hideable(test_case_row_state state,
                                            std::int64_t finish_ns,
                                            std::int64_t now_ns) noexcept
{
    if (state != test_case_row_state::done) return false;
    if (finish_ns == 0) return false;
    constexpr auto grace_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            k_completed_row_grace_period).count();
    return (now_ns - finish_ns) > grace_ns;
}

inline void render_progress_loop(progress_state& progress) noexcept {
    using namespace std::chrono_literals;
    using namespace ftxui;
    constexpr int k_bar_cells = 16;

    const bool tty = progress.tty_render.load(std::memory_order_relaxed);
    if (!tty) {
        constexpr auto k_poll_interval = std::chrono::milliseconds{200};
        constexpr int  k_ticks_per_log  = 25;     // 25 × 200 ms == 5 s
        int ticks_until_next_log = k_ticks_per_log;
        while (progress.running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(k_poll_interval);
            if (--ticks_until_next_log > 0) continue;
            ticks_until_next_log = k_ticks_per_log;
            for (const auto& per : progress.per_benchmark) {
                if (!per->setup_done.load(std::memory_order_acquire)) continue;
                for (const auto& tc : per->test_cases) {
                    const auto done = tc->tuple_done.load(std::memory_order_relaxed);
                    const auto total = tc->tuple_total.load(std::memory_order_relaxed);
                    if (tc->finish_ns.load(std::memory_order_relaxed) != 0) continue;
                    if (tc->start_ns.load(std::memory_order_relaxed) == 0) continue;
                    bioseq_log<logging_level::info>("[bench_framework] [{}/{}] tuples {}/{}",
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
                                       bioseq::instrumentation::clock::now_ns());

        // reservation totals from the scheduler when present, falling
        // back to benchmark_memory_budget for the system-wide numbers.
        rows.push_back(text("bench_framework — live progress") | bold);
        if (progress.scheduler != nullptr) {
            const auto snap = progress.scheduler->capture_live_snapshot();
            const auto over_cap = snap.used_bytes >= snap.cap_bytes;
            rows.push_back(hbox({
                text("[budget room ") | color(Color::GrayLight),
                text(fmt::format("{:.2f}", bytes_to_gib(snap.available_bytes)))
                    | color(snap.available_bytes > 0 ? Color::Green : Color::Red),
                text(fmt::format(" / cap {:.2f} GiB · process ",
                                 bytes_to_gib(snap.cap_bytes))),
                text(fmt::format("{:.2f}", bytes_to_gib(snap.used_bytes)))
                    | color(over_cap ? Color::RedLight : Color::Yellow),
                text(fmt::format(" / {:.2f} GiB · sys available ",
                                 bytes_to_gib(snap.total_bytes))),
                text(fmt::format("{:.2f}", bytes_to_gib(snap.system_available_bytes)))
                    | color(Color::Green),
                text(" GiB · reserved "),
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

                if (state == test_case_row_state::done) {
                    headline.push_back(text(fmt::format("tuples {}/{}",
                        tuple_done, tuple_total)));
                    headline.push_back(text(fmt::format("  done in {} s", elapsed_s))
                                       | color(Color::GrayLight));
                } else if (state == test_case_row_state::aborted) {
                    headline.push_back(text(fmt::format("aborted at {}/{}",
                        tuple_done, tuple_total)) | color(Color::RedLight));
                } else if (state == test_case_row_state::input_retry
                        || state == test_case_row_state::cleanup_retry
                        || state == test_case_row_state::idle_wait) {
                    headline.push_back(text(fmt::format("tuples {}/{}",
                        tuple_done, tuple_total)));
                } else {
                    headline.push_back(text(fmt::format("tuples {}/{}  ",
                        tuple_done, tuple_total)));
                    headline.push_back(coloured_gauge(fraction, k_bar_cells));
                    headline.push_back(text(fmt::format("  {:>3.0f}%  Running... {}s",
                        static_cast<double>(fraction) * 100.0, elapsed_s))
                        | color(Color::GrayLight));
                }
                rows.push_back(hbox(std::move(headline)));

                // Detail rows for non-running states (failures / idle).
                auto details = build_detail_rows(*tc, state);
                for (auto& row : details) rows.push_back(std::move(row));

                // Per-group gauge rows still appear for normal Running.
                if (state == test_case_row_state::running) {
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
                            coloured_gauge(gfrac, k_bar_cells),
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

/// Lightweight declaration view of one benchmark block. The `--filter`
/// pre-pass uses tags/over-keys for selection; regression-rule validation
/// uses the remaining fields as its unfiltered registered universe.
struct benchmark_filter_metadata {
    std::string              benchmark_key;
    std::vector<std::string> tags;               // <key>.tags
    std::vector<std::string> case_names;          // <key>.matrix.over keys
    std::vector<std::string> config_algorithms;   // top-level params keys
    std::vector<std::string> config_test_cases;   // top-level test_cases keys
    std::vector<std::string> matrix_test_cases;   // <key>.matrix.for keys
    std::vector<std::string> metric_names;        // <key>.matrix.metrics keys
    bool                     fast = false;
    bool                     complete_matrix_acceptance = false;
    bool                     parsed_ok = false;  // false => treat as no declarations
};

/// Best-effort extraction of selection/acceptance metadata for one benchmark.
/// Never fails the run — on any parse problem returns parsed_ok=false and the
/// closure's full parse will surface the real schema error later. Reads
/// `fast`, `tags`, `matrix.over` keys and the presence of an exact
/// acceptance contract, and the declaration names needed for regression-rule
/// validation; keep these reads in sync with parse_config_file and
/// parse_benchmark_block.
[[nodiscard]] inline benchmark_filter_metadata extract_filter_metadata(
    const std::string& config_open_path,
    const std::string& benchmark_key) noexcept {
    benchmark_filter_metadata meta;
    meta.benchmark_key = benchmark_key;
    const std::string body = slurp_file(
        std::filesystem::path{config_open_path});
    if (body.empty()) return meta;
    simdjson::dom::parser parser;
    simdjson::dom::element root;
    if (parser.parse(body).get(root) != simdjson::SUCCESS) return meta;
    simdjson::dom::object root_obj;
    if (root.get(root_obj) != simdjson::SUCCESS) return meta;
    simdjson::dom::object params_obj;
    if (root_obj["params"].get(params_obj) == simdjson::SUCCESS) {
        for (auto field : params_obj) {
            meta.config_algorithms.emplace_back(field.key);
        }
    }
    simdjson::dom::object test_cases_obj;
    if (root_obj["test_cases"].get(test_cases_obj) == simdjson::SUCCESS) {
        for (auto field : test_cases_obj) {
            meta.config_test_cases.emplace_back(field.key);
        }
    }
    simdjson::dom::object bench_obj;
    if (root_obj[benchmark_key].get(bench_obj) != simdjson::SUCCESS) return meta;
    bool fast = false;
    if (bench_obj["fast"].get(fast) == simdjson::SUCCESS) meta.fast = fast;
    simdjson::dom::array tags_array;
    if (bench_obj["tags"].get(tags_array) == simdjson::SUCCESS) {
        for (auto t : tags_array) {
            std::string_view sv;
            if (t.get(sv) == simdjson::SUCCESS) meta.tags.emplace_back(sv);
        }
    }
    simdjson::dom::object matrix_obj, over_obj;
    if (bench_obj["matrix"].get(matrix_obj) == simdjson::SUCCESS) {
        if (matrix_obj["over"].get(over_obj) == simdjson::SUCCESS) {
            for (auto kv : over_obj) meta.case_names.emplace_back(kv.key);
        }
        simdjson::dom::object for_obj;
        if (matrix_obj["for"].get(for_obj) == simdjson::SUCCESS) {
            for (auto field : for_obj) {
                meta.matrix_test_cases.emplace_back(field.key);
            }
        }
        simdjson::dom::object metrics_obj;
        if (matrix_obj["metrics"].get(metrics_obj) == simdjson::SUCCESS) {
            for (auto field : metrics_obj) {
                meta.metric_names.emplace_back(field.key);
            }
        }
        simdjson::dom::object acceptance_obj;
        simdjson::dom::array exact_rows;
        if (matrix_obj["acceptance"].get(acceptance_obj)
                == simdjson::SUCCESS
            && acceptance_obj["exact_algorithm_rows"].get(exact_rows)
                == simdjson::SUCCESS) {
            meta.complete_matrix_acceptance = true;
        }
    }
    meta.parsed_ok = true;
    return meta;
}

/// Top-level orchestrator. Owns: CLI parsing, TBB arena setup, scheduler,
/// the progress_state lifetime, the violations + reports maps,
/// and the final report write + violations print. Returns 0 if every rule
/// passed and every config parsed; returns 1 otherwise (duplicate
/// benchmark_key, parse failure, or any rule violation).
[[nodiscard]] inline int run_impl(int argc, char** argv,
                                   std::string report_dir,
                                   std::string data_dir,
                                   std::vector<erased_benchmark> benchmarks)
{
    const auto cli = parse_cli(argc, argv);
    if (cli.report_override.has_value()) {
        report_dir = *cli.report_override;
    }

    if (cli.ui_mode) {
        if (cli.fast_filter) {
            bioseq_log<logging_level::warning>("[bench_framework] --ui ignores --fast (no benchmarks run)");
        }
        if (cli.threads_override.has_value()) {
            bioseq_log<logging_level::warning>("[bench_framework] --ui ignores --threads (no benchmarks run)");
        }
        if (!cli.filter.empty()) {
            bioseq_log<logging_level::warning>("[bench_framework] --ui ignores --filter (no benchmarks run)");
        }
        if (cli.debug) {
            bioseq_log<logging_level::warning>("[bench_framework] --ui ignores --debug (no benchmarks run)");
        }
        return ui_mode_main(report_dir, cli);
    }

    // Determine the index for this run up front (non-UI path only). Reads
    // existing report_<N>.json files; writes report_<max + 1>.json. First run
    // on a clean dir → 1. Hoisted here (from the late report-write site) so the
    // `--debug` file name report_debug_<N>.jsonl matches the eventual
    // report_<N>.json. `max_existing_report_index` is a read-only noexcept
    // reader and nothing writes report_<N>.json before the write site below,
    // so the hoist is behaviour-preserving.
    const std::filesystem::path report_dir_path{report_dir};
    const std::size_t current_index =
        max_existing_run_artifact_index(report_dir_path).value_or(0) + 1;

    bioseq_log<logging_level::info>("[bench_framework] starting: report_dir={} data_dir={} benchmarks={}",
                 report_dir, data_dir, benchmarks.size());

    const auto thread_count = cli.threads_override.has_value()
        ? *cli.threads_override
        : std::max<unsigned>(1u, std::thread::hardware_concurrency() == 0
                                  ? 1u
                                  : std::thread::hardware_concurrency() - 1u);
    bioseq_log<logging_level::info>("[bench_framework] thread_count={}", thread_count);

    // Capture the run's identity once, up front. Used both by the
    // report writer (the `meta` block) and surfaced in stderr logs so
    // the operator can correlate a run with a commit.
    const auto run_meta = capture_run_meta(cli.fast_filter);
    bioseq_log<logging_level::info>("[bench_framework] commit={} dirty={} created={} fast={}",
         run_meta.commit.empty() ? std::string_view{"<no-git>"}
                                 : std::string_view{run_meta.commit},
         run_meta.dirty, run_meta.created, run_meta.fast);

    // Filter benchmarks by --fast. The `--fast` field lives on the per-
    // benchmark config, not on `erased_benchmark`, so the filter actually
    // runs inside each closure after parse. `selected` therefore mirrors
    // `benchmarks` 1:1 — we just take ownership.
    std::vector<erased_benchmark> selected = std::move(benchmarks);

    // Uniqueness check on benchmark_key.
    std::set<std::string> seen_keys;
    for (const auto& erased : selected) {
        const auto inserted = seen_keys.insert(erased.benchmark_key);
        if (!inserted.second) {
            bioseq_log<logging_level::error>("[bench_framework] duplicate benchmark_key '{}' across create_benchmark calls",
                         erased.benchmark_key);
            return 1;
        }
    }

    // Parse the declaration-only view before any CLI selection can prune a
    // benchmark. Semantic regression-rule validation must resolve names
    // against all registered benchmarks, including rows omitted by
    // --filter/--fast. The full closure parse remains authoritative for
    // schema/semantic errors.
    std::vector<benchmark_filter_metadata> registered_metadata;
    registered_metadata.reserve(selected.size());
    regression_validation_universe validation_universe;
    for (const auto& erased : selected) {
        registered_metadata.push_back(extract_filter_metadata(
            erased.config_open_path, erased.benchmark_key));
        const auto& metadata = registered_metadata.back();
        regression_benchmark_declaration declaration;
        declaration.benchmark_key = metadata.benchmark_key;
        declaration.config_algorithms = metadata.config_algorithms;
        declaration.config_test_cases = metadata.config_test_cases;
        declaration.matrix_algorithms = metadata.case_names;
        declaration.matrix_test_cases = metadata.matrix_test_cases;
        declaration.metrics = metadata.metric_names;
        validation_universe[erased.config_path].push_back(
            std::move(declaration));
    }

    oneapi::tbb::global_control concurrency_limit(
        oneapi::tbb::global_control::max_allowed_parallelism, thread_count);
    oneapi::tbb::task_arena bench_arena(static_cast<int>(thread_count));

    std::mutex        violations_mu;
    std::mutex        reports_mu;
    std::mutex        diagnostics_mu;
    std::mutex        regression_rules_mu;
    std::vector<violation> violations;
    std::vector<validation_diagnostic> diagnostics;
    std::map<std::string, std::shared_ptr<benchmark_execution_state>> reports;
    regression_rules_by_configuration regression_rules_by_config;

    // ---- --filter / --debug pre-pass: validate, select, prune. ----
    // Runs single-threaded before progress/scheduler are wired, so a
    // filtered-out benchmark never appears on the dashboard or in the report.
    const filter_specification& f = cli.filter;
    std::vector<validation_diagnostic> filter_diags;

    // (1) CLI-level launch errors — ALWAYS checked, even with no filter.
    for (const auto& raw : cli.cli_errors) {
        const auto colon = raw.find(':');
        filter_diags.push_back({ validation_diagnostic::diagnostic_severity::error,
            std::string{raw.substr(0, colon)}, "cli:--filter",
            std::string{raw.substr(colon + 1)} });
    }
    // --debug requires a non-empty filter (§4.2).
    if (cli.debug && f.empty()) {
        filter_diags.push_back({ validation_diagnostic::diagnostic_severity::error,
            "debug_requires_filter", "cli:--debug",
            "--debug needs a non-empty --filter: it is for targeted runs, not for "
            "instrumenting an entire benchmark sweep. Add e.g. --filter benchmark=<key>." });
    }

    // The case allow-list handed to every closure (nullopt == "all cases").
    std::optional<std::set<std::string>> case_allow_list;
    if (f.cases) case_allow_list.emplace(f.cases->begin(), f.cases->end());

    // (2) Selection + warnings — ONLY when a filter is present. `runnable`
    // defaults to keep-everything; the declaration pre-pass above is also
    // reused here, so selection itself performs no additional config reads.
    std::vector<char> runnable(selected.size(), 1);
    if (!f.empty()) {
        const auto& meta = registered_metadata;

        // keep[i] = passes tag+benchmark filter.
        std::vector<char> keep(selected.size(), 1);
        for (std::size_t i = 0; i < selected.size(); ++i) {
            if (f.tags) {
                const auto& bt = meta[i].tags;
                const bool hit = std::any_of(f.tags->begin(), f.tags->end(),
                    [&bt](const std::string& t){ return std::find(bt.begin(), bt.end(), t) != bt.end(); });
                if (!hit) keep[i] = 0;
            }
            if (f.benchmark) {
                const bool hit = std::find(f.benchmark->begin(), f.benchmark->end(),
                                           selected[i].benchmark_key) != f.benchmark->end();
                if (!hit) keep[i] = 0;
            }
        }
        // runnable[i] = keep[i] AND has >=1 case after the cases filter.
        for (std::size_t i = 0; i < selected.size(); ++i) {
            if (!keep[i]) { runnable[i] = 0; continue; }
            if (!f.cases) { runnable[i] = 1; continue; }
            runnable[i] = std::any_of(meta[i].case_names.begin(), meta[i].case_names.end(),
                [&f](const std::string& c){ return std::find(f.cases->begin(), f.cases->end(), c) != f.cases->end(); });
        }

        // Warnings: unknown tag / benchmark (UNIVERSE scope).
        if (f.tags) for (const auto& t : *f.tags) {
            const bool known = std::any_of(meta.begin(), meta.end(), [&t](const auto& m){
                return std::find(m.tags.begin(), m.tags.end(), t) != m.tags.end(); });
            if (!known) filter_diags.push_back({ validation_diagnostic::diagnostic_severity::warning,
                "filter_unknown_tag", "cli:--filter tags",
                fmt::format("tag '{}' is declared by no benchmark", t) });
        }
        if (f.benchmark) for (const auto& k : *f.benchmark) {
            const bool known = std::any_of(selected.begin(), selected.end(),
                [&k](const auto& e){ return e.benchmark_key == k; });
            if (!known) filter_diags.push_back({ validation_diagnostic::diagnostic_severity::warning,
                "filter_unknown_benchmark", "cli:--filter benchmark",
                fmt::format("benchmark key '{}' matches no registered benchmark", k) });
        }

        // An explicitly named benchmark must never turn into a successful
        // empty report merely because --fast excludes it. Tag-only fast
        // sweeps retain the standard skip behaviour.
        if (cli.fast_filter && f.benchmark) {
            for (std::size_t i = 0; i < selected.size(); ++i) {
                if (!meta[i].parsed_ok || meta[i].fast) continue;
                const bool explicitly_selected = std::find(
                    f.benchmark->begin(), f.benchmark->end(),
                    selected[i].benchmark_key) != f.benchmark->end();
                if (explicitly_selected) {
                    filter_diags.push_back({
                        validation_diagnostic::diagnostic_severity::error,
                        "explicit_benchmark_excluded_by_fast",
                        "cli:--fast",
                        fmt::format("benchmark '{}' was explicitly selected but declares fast=false; rerun without --fast",
                                    selected[i].benchmark_key),
                    });
                }
            }
        }

        // A complete-matrix gate may be targeted with an all-row cases list,
        // but a filter that removes even one declared row invalidates the
        // acceptance claim and is rejected before any work starts.
        if (f.cases) {
            for (std::size_t i = 0; i < selected.size(); ++i) {
                if (!runnable[i] || !meta[i].complete_matrix_acceptance) continue;
                const bool keeps_every_row = std::all_of(
                    meta[i].case_names.begin(), meta[i].case_names.end(),
                    [&f](const std::string& row) {
                        return std::find(f.cases->begin(), f.cases->end(), row)
                            != f.cases->end();
                    });
                if (!keeps_every_row) {
                    filter_diags.push_back({
                        validation_diagnostic::diagnostic_severity::error,
                        "complete_matrix_case_filter_prunes_rows",
                        "cli:--filter cases",
                        fmt::format("benchmark '{}' declares complete-matrix acceptance; cases= must retain all {} algorithm rows",
                                    selected[i].benchmark_key,
                                    meta[i].case_names.size()),
                    });
                }
            }
        }
        // Warning: unknown case (KEPT-subset scope, per the prompt).
        if (f.cases) for (const auto& c : *f.cases) {
            bool known = false;
            for (std::size_t i = 0; i < selected.size() && !known; ++i)
                if (keep[i]) known = std::find(meta[i].case_names.begin(),
                                               meta[i].case_names.end(), c) != meta[i].case_names.end();
            if (!known) filter_diags.push_back({ validation_diagnostic::diagnostic_severity::warning,
                "filter_unknown_case", "cli:--filter cases",
                fmt::format("case '{}' is an over-key of no tag/benchmark-selected benchmark", c) });
        }

        // Hard error: nothing left to run.
        if (std::count(runnable.begin(), runnable.end(), 1) == 0)
            filter_diags.push_back({ validation_diagnostic::diagnostic_severity::error,
                "filter_selected_nothing", "cli:--filter",
                "the supplied --filter matched no runnable (benchmark, case) combination" });
    }

    // (3) Drain + abort. Compute had_error BEFORE the move (a moved-from
    //     vector is empty, so checking it post-drain would always pass).
    const bool filter_had_error = std::any_of(filter_diags.begin(), filter_diags.end(),
        [](const auto& d){ return d.severity == validation_diagnostic::diagnostic_severity::error; });
    {
        std::lock_guard<std::mutex> guard(diagnostics_mu);
        for (auto& d : filter_diags) diagnostics.push_back(std::move(d));
    }
    if (filter_had_error) {
        report_diagnostics(std::span<const validation_diagnostic>{diagnostics}, "filter");
        return 1;     // early exit BEFORE spawning progress thread / scheduler work
    }

    // (4) Prune `selected` to the runnable set (no-op when no filter).
    if (!f.empty()) {
        std::vector<erased_benchmark> pruned;
        for (std::size_t i = 0; i < selected.size(); ++i)
            if (runnable[i]) pruned.push_back(std::move(selected[i]));
        selected = std::move(pruned);
    }

    // ---- Construct the --debug sink (only when requested), before the
    //      progress thread. A non-owning pointer is threaded into each
    //      benchmark_context (§3.7). ----
    std::unique_ptr<debug_report_sink> debug_sink;
    debug_report_sink* debug_sink_ptr = nullptr;
    if (cli.debug) {
        const auto debug_path =
            report_dir_path / fmt::format("report_debug_{}.jsonl", current_index);
        debug_sink = std::make_unique<debug_report_sink>(debug_path);
        if (debug_sink->ok()) debug_sink_ptr = debug_sink.get();
        bioseq_log<logging_level::info>("[bench_framework] --debug: streaming events to {}",
                        debug_path.string());
    }

    std::unique_ptr<durable_raw_evidence_sink> raw_evidence_sink;
    durable_raw_evidence_sink* raw_evidence_sink_ptr = nullptr;
    const bool raw_evidence_requested = std::any_of(
        selected.begin(), selected.end(),
        [](const erased_benchmark& benchmark) {
            return benchmark.raw_samples_enabled;
        });
    if (raw_evidence_requested) {
        const auto raw_path = report_dir_path
            / fmt::format("report_{}.raw.jsonl", current_index);
        const std::string run_id = fmt::format(
            "{}:{}:{}", current_index, run_meta.created, ::getpid());
        raw_evidence_sink = std::make_unique<durable_raw_evidence_sink>(
            raw_path, run_id);
        if (!raw_evidence_sink->ok()) {
            std::fprintf(stderr,
                "[bench_framework] RAW-EVIDENCE-OPEN-FAIL path=%s\n",
                raw_path.c_str());
            std::fflush(stderr);
            return 1;
        }
        raw_evidence_sink_ptr = raw_evidence_sink.get();
    }

    // Process-wide memory budget shared by every benchmark closure.
    benchmark_memory_budget memory_budget(cli.memory_buffer_pct);
    {
        const auto sys = memory_budget.current_system_snapshot();
        const auto cap = benchmark_memory_budget::cap_for(sys.system_total_bytes,
                                                cli.memory_buffer_pct);
        bioseq_log<logging_level::info>("[bench_framework] memory budget: total={:.2f} GiB available={:.2f} GiB "
             "process={:.2f} GiB buffer={:.0f}% cap={:.2f} GiB",
             bytes_to_gib(sys.system_total_bytes),
             bytes_to_gib(sys.system_available_bytes),
             bytes_to_gib(sys.process_footprint_bytes),
             cli.memory_buffer_pct,
             bytes_to_gib(cap));
        if (sys.system_total_bytes == std::numeric_limits<std::uint64_t>::max()) {
            bioseq_log<logging_level::warning>("[bench_framework] memory probe returned 0 for total RAM — budget "
                 "checks are effectively disabled (treated as infinite memory)");
        }
    }

    // Process-wide scheduler owns batch admission and the TBB
    // task pool that runs the bodies. Constructed BEFORE the progress
    // thread so the renderer can read `scheduler.capture_live_snapshot()`.
    batch_scheduler scheduler(memory_budget, bench_arena);

    progress_state progress;
    progress.tty_render.store(::isatty(STDERR_FILENO) != 0, std::memory_order_relaxed);
    progress.scheduler     = &scheduler;
    progress.per_benchmark.reserve(selected.size());
    for (const auto& erased : selected) {
        auto entry = std::make_unique<progress_state::per_benchmark_state>();
        entry->title = erased.title;
        entry->benchmark_key = erased.benchmark_key;
        progress.per_benchmark.push_back(std::move(entry));
    }

    std::thread progress_thread{[&progress] { render_progress_loop(progress); }};

    // Two-phase driver protocol. Preparation parses/configures and enqueues
    // every selected benchmark from the caller thread without waiting. Only
    // after all work is visible to the shared scheduler do caller-thread
    // finalizers join and publish results. This retains cross-benchmark
    // admission while preventing a TBB driver from blocking a worker that a
    // nested process-wide global_control may leave as the sole worker slot.
    std::vector<std::unique_ptr<benchmark_context>> benchmark_contexts;
    benchmark_contexts.reserve(selected.size());
    std::vector<std::function<void()>> benchmark_finalizers;
    benchmark_finalizers.reserve(selected.size());

    for (std::size_t benchmark_index = 0;
         benchmark_index < selected.size(); ++benchmark_index) {
        benchmark_contexts.push_back(std::make_unique<benchmark_context>());
        auto& context = *benchmark_contexts.back();
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
        context.filter_cases = case_allow_list ? &*case_allow_list : nullptr;
        context.debug_sink   = debug_sink_ptr;
        context.raw_evidence_sink = raw_evidence_sink_ptr;

        benchmark_finalizers.push_back(
            selected[benchmark_index].execute(context));
    }

    for (auto& finalize_benchmark : benchmark_finalizers) {
        finalize_benchmark();
    }

    // Defence-in-depth: every finalizer drains its own batches via
    // `wait_for_benchmark`. This explicit, non-noexcept global join
    // is also the ownership boundary for exceptions captured from TBB worker
    // bodies. Keep the exception until after the progress thread joins, then
    // turn it into a blocking diagnostic so completed samples still produce a
    // useful partial report instead of terminating in ~batch_scheduler().
    std::exception_ptr worker_exception;
    try {
        scheduler.wait_for_completion();
    } catch (...) {
        worker_exception = std::current_exception();
    }

    progress.running.store(false, std::memory_order_relaxed);
    progress_thread.join();

    if (worker_exception != nullptr) {
        const auto failure_count = scheduler.worker_exception_count();
        try {
            std::rethrow_exception(worker_exception);
        } catch (const std::exception& error) {
            diagnostics.push_back(validation_diagnostic{
                validation_diagnostic::diagnostic_severity::error,
                "batch_worker_exception",
                "batch_scheduler::wait_for_completion",
                fmt::format(
                    "captured {} worker-task exception(s); first failure: {}. "
                    "Completed samples are retained in this partial report",
                    failure_count, error.what()),
            });
        } catch (...) {
            diagnostics.push_back(validation_diagnostic{
                validation_diagnostic::diagnostic_severity::error,
                "batch_worker_exception",
                "batch_scheduler::wait_for_completion",
                fmt::format(
                    "captured {} worker-task exception(s); first failure was "
                    "non-std. Completed samples are retained in this partial "
                    "report",
                    failure_count),
            });
        }
    }

    // Cross-config validation of regression rules. Runs serially after
    // every closure has registered its state (so we can resolve
    // `benchmarks` membership and metric existence in one place).
    {
        auto regression_validation_diags = semantic_validate_regression_rules(
            regression_rules_by_config, validation_universe);
        std::lock_guard<std::mutex> guard(diagnostics_mu);
        for (auto& diag : regression_validation_diags) {
            diagnostics.push_back(std::move(diag));
        }
    }

    nlohmann::json raw_evidence_manifest = nullptr;
    if (raw_evidence_sink_ptr != nullptr) {
        const bool raw_evidence_valid =
            raw_evidence_sink_ptr->seal_and_validate(reports);
        raw_evidence_manifest = raw_evidence_sink_ptr->manifest();
        if (!raw_evidence_valid) {
            diagnostics.push_back(validation_diagnostic{
                validation_diagnostic::diagnostic_severity::error,
                "raw_evidence_validation_failed",
                raw_evidence_sink_ptr->path().string(),
                "durable raw evidence did not match every measurement_cell, schedule, "
                "checksum, record id and wall-clock sample",
            });
        }
    }

    // Print all collected validation diagnostics first (config + semantic +
    // post-run + regression-rule). Errors here force a non-zero exit code
    // regardless of whether any rules failed.
    const bool diagnostics_had_errors = report_diagnostics(
        std::span<const validation_diagnostic>{diagnostics}, "configuration");

    // `report_dir_path` / `current_index` were computed up front (hoisted so
    // the --debug file name report_debug_<N>.jsonl matches report_<N>.json).
    bioseq_log<logging_level::info>("[bench_framework] writing run as report_{}.json", current_index);

    // Single shared parser-cache for every consumer that wants to walk
    // historical reports (regressions + sanity warnings). Each
    // `report_<N>.json` is parsed at most once per run.
    historical_report_cache history_cache{report_dir_path};

    // Evaluate regression rules against the newest eligible historical
    // candidate (walking backward over ineligible reports), then reuse that
    // exact predicate for `regression_since`. The evaluator never writes
    // anything; we collect entries and flush them to the report below
    // alongside any diagnostics it produced.
    std::vector<validation_diagnostic> regression_eval_diagnostics;
    auto regressions = evaluate_regressions(
        regression_rules_by_config, reports,
        run_meta, current_index, history_cache, regression_eval_diagnostics,
        !cli.filter.empty() || cli.fast_filter);
    const bool regression_eval_had_errors = report_diagnostics(
        std::span<const validation_diagnostic>{regression_eval_diagnostics},
        "regression");

    const bool blocking_rule_violation = std::any_of(
        violations.begin(), violations.end(),
        [](const violation& rule_violation) { return rule_violation.blocking; });
    const bool blocking_regression = std::any_of(
        regressions.begin(), regressions.end(),
        [](const regression_entry& regression) {
            return regression.blocking;
        });
    const bool evaluation_history_eligible =
        !blocking_rule_violation
        && !blocking_regression
        && !diagnostics_had_errors
        && !regression_eval_had_errors;
    report_metadata final_run_meta = run_meta;
    // Fail closed on disk. The report is first persisted with a false seal,
    // self-validated, and only then rewritten with a true seal when every
    // evaluation gate above passed. A crash or write/schema failure can
    // therefore never leave an authoritative failed-run baseline behind.
    final_run_meta.history_eligible = false;

    // Sanity warnings: hardcoded checks against the just-finished run's
    // sample arrays plus history. Non-blocking (do not affect the exit
    // code), but always surface to stderr + the report so a human
    // notices broken instrumentation.
    auto sanity_warnings = evaluate_sanity_warnings(
        reports, run_meta, current_index, history_cache);

    bioseq_log<logging_level::info>("[bench_framework] all benchmarks finished, writing report...");
    const auto output_path = report_path_for_index(report_dir_path, current_index);
    std::string report_path = write_report(
        output_path, final_run_meta, reports, violations, regressions,
        sanity_warnings, raw_evidence_manifest);
    bool report_write_failed = report_path.empty();

    // Sanity-check our own report against the output schema. Failures here
    // mean a bug in the writer, not user error. The persisted seal is still
    // false at this point, so such a report is never historical authority.
    bool report_schema_failed = false;
    if (!report_write_failed) {
        const auto report_text = slurp_file(std::filesystem::path{report_path});
        if (report_text.empty()) {
            bioseq_log<logging_level::error>(
                "[bench_framework] wrote an empty or unreadable report '{}'",
                report_path);
            report_schema_failed = true;
        } else {
            const auto report_check_diagnostics =
                validate_against_schema(report_text, k_bench_report_schema, report_path);
            report_schema_failed = report_diagnostics(
                std::span<const validation_diagnostic>{report_check_diagnostics}, "report");
        }
    }

    // Promotion changes only one boolean field, but it must not truncate the
    // authoritative false-seal report in place. Write and validate a sibling
    // temporary file, then use POSIX rename as the atomic commit point. Any
    // write, close, validation, or rename failure leaves the false report
    // untouched and therefore ineligible.
    if (!report_write_failed
        && !report_schema_failed
        && evaluation_history_eligible) {
        final_run_meta.history_eligible = true;
        auto promotion_path = output_path;
        promotion_path += ".history-promotion.tmp";
        const auto promoted_report_path = write_report(
            promotion_path, final_run_meta, reports, violations, regressions,
            sanity_warnings, raw_evidence_manifest);
        if (promoted_report_path.empty()) {
            report_write_failed = true;
            std::error_code cleanup_ec;
            std::filesystem::remove(promotion_path, cleanup_ec);
        } else {
            bool promotion_schema_failed = false;
            const auto promoted_text = slurp_file(promotion_path);
            if (promoted_text.empty()) {
                bioseq_log<logging_level::error>(
                    "[bench_framework] wrote an empty or unreadable report "
                    "promotion '{}'",
                    promotion_path.string());
                promotion_schema_failed = true;
            } else {
                const auto promotion_diagnostics = validate_against_schema(
                    promoted_text, k_bench_report_schema,
                    promotion_path.string());
                promotion_schema_failed = report_diagnostics(
                    std::span<const validation_diagnostic>{promotion_diagnostics},
                    "report promotion");
            }

            if (promotion_schema_failed) {
                report_schema_failed = true;
            } else if (std::rename(
                           promotion_path.c_str(), output_path.c_str()) != 0) {
                const int rename_errno = errno;
                bioseq_log<logging_level::error>(
                    "[bench_framework] failed to atomically promote '{}' to "
                    "'{}': {}",
                    promotion_path.string(), output_path.string(),
                    std::strerror(rename_errno));
                report_write_failed = true;
            } else {
                report_path = output_path.string();
            }

            if (promotion_schema_failed || report_write_failed) {
                std::error_code cleanup_ec;
                std::filesystem::remove(promotion_path, cleanup_ec);
            }
        }
    }

    if (!report_path.empty()) {
        bioseq_log<logging_level::info>("[bench_framework] wrote {}", report_path);
    }

    // --debug: final event. `report_path` is the absolute report path, or ""
    // (→ JSON null) if the write failed. Last line of the jsonl stream.
    if (debug_sink_ptr != nullptr) {
        debug_sink_ptr->emit_execution_finished(report_path);
    }

    print_violations(violations);
    print_regressions(regressions);
    print_sanity_warnings(sanity_warnings);

    // Sanity warnings are non-blocking: their presence does NOT
    // contribute to the exit code (the user spec calls them out as
    // "not blocking, however they should be printed to stderr at the
    // end"). The summary above is the operator-visible surface.
    return (!blocking_rule_violation
            && !blocking_regression
            && !diagnostics_had_errors
            && !regression_eval_had_errors
            && !report_write_failed
            && !report_schema_failed) ? 0 : 1;
}

}  // namespace detail

// ----------------- create_benchmark template body -----------------

namespace detail {

template<typename p_input, typename p_output, typename p_iteration_boundaries>
[[nodiscard]] erased_benchmark create_benchmark_impl(
    std::string title,
    std::string benchmark_key,
    std::string config_path,
    std::function<run_constraints(std::string_view, std::string_view, simdjson::dom::element)> on_estimate_resources,
    std::function<p_input(std::string_view, std::string_view, simdjson::dom::element, const run_constraints&)> on_input,
    std::function<p_output(std::string_view, const parameter_map&, const p_input&)> on_run,
    std::function<std::map<std::string, double>(std::string_view, const parameter_map&, const p_input&, const p_output&)> on_evaluate,
    std::function<void(std::string_view, std::string_view, simdjson::dom::element, const p_input&)> on_cleanup,
    std::function<std::string(std::string_view, const parameter_map&, const p_input&, const p_output&)>
        serialize_raw_sample_payload,
    std::string config_open_path,
    repeat_finalization_callbacks<p_input, p_output>
        repeat_finalization,
    p_iteration_boundaries iteration_boundaries)
{
    erased_benchmark erased;
    erased.title         = std::move(title);
    erased.benchmark_key = std::move(benchmark_key);
    erased.config_path   = std::move(config_path);
    erased.config_open_path = config_open_path.empty()
        ? erased.config_path
        : std::move(config_open_path);
    erased.raw_samples_enabled = static_cast<bool>(serialize_raw_sample_payload);

    erased.execute = [erased_title              = erased.title,
                       erased_benchmark_key      = erased.benchmark_key,
                       erased_config_path        = erased.config_path,
                       erased_config_open_path   = erased.config_open_path,
                       captured_on_estimate      = std::move(on_estimate_resources),
                       captured_on_input         = std::move(on_input),
                       captured_on_run           = std::move(on_run),
                       captured_on_evaluate      = std::move(on_evaluate),
                       captured_on_cleanup       = std::move(on_cleanup),
                       captured_raw_serializer   = std::move(serialize_raw_sample_payload),
                       captured_repeat_finalization =
                           std::move(repeat_finalization),
                       captured_iteration_boundaries =
                           std::move(iteration_boundaries)
                      ](benchmark_context& context) -> std::function<void()> {
        const auto no_op_finalizer = [] {
            return std::function<void()>{[] {}};
        };
        auto state_holder = std::make_shared<detail::benchmark_execution_state>();
        auto& state = *state_holder;
        state.title = erased_title;
        state.raw_samples_enabled = static_cast<bool>(captured_raw_serializer);
        state.driver_benchmark_index = context.benchmark_index;

        // Always publish a `setup_done` signal to the renderer on the
        // way out, even from early-return paths. Otherwise an aborted
        // benchmark would leave the dashboard's per-benchmark slot
        // stuck on "header only, no rows" until `progress.running`
        // flips false. The renderer treats `setup_done == true` plus
        // an empty `test_cases` as "this benchmark is silent" and
        // simply omits the inner walk.
        struct publish_on_exit {
            detail::progress_state::per_benchmark_state* slot = nullptr;
            ~publish_on_exit() noexcept {
                if (slot != nullptr) {
                    slot->setup_done.store(true, std::memory_order_release);
                }
            }
        };
        publish_on_exit setup_publish_guard{
            (context.progress != nullptr
             && context.benchmark_index < context.progress->per_benchmark.size())
                ? context.progress->per_benchmark[context.benchmark_index].get()
                : nullptr};

        // 1. Parse the config file. The outcome carries schema diagnostics
        //    plus structural problems found during the simdjson walk.
        auto parse_outcome = detail::parse_config_file(
            erased_config_path, erased_config_open_path,
            erased_benchmark_key, state);
        const bool parse_failed = !parse_outcome.ok;
        detail::drain_diagnostics_to_context(context, parse_outcome.diagnostics);
        if (parse_failed) {
            bioseq_log<logging_level::error>("[bench_framework] benchmark '{}' aborted (parse / schema failure)",
                 erased_benchmark_key);
            return no_op_finalizer();
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
                return diag.severity == detail::validation_diagnostic::diagnostic_severity::error;
            });
        detail::drain_diagnostics_to_context(context, semantic_diagnostics);
        if (semantic_failed) {
            bioseq_log<logging_level::error>("[bench_framework] benchmark '{}' aborted (semantic validation failure)",
                 erased_benchmark_key);
            return no_op_finalizer();
        }

        // 3. --fast filter: skip if the user requested fast mode but this
        //    benchmark is not flagged fast. Without --fast we run everything.
        if (context.fast_filter && !state.config.fast) {
            bioseq_log<logging_level::info>("[bench_framework] skipping '{}' under --fast (fast=false)",
                 erased_benchmark_key);
            return no_op_finalizer();
        }

        bool expansion_ok = true;
        state.execution_tuples = detail::expand_matrix(state.config, expansion_ok);
        if (!expansion_ok) {
            bioseq_log<logging_level::error>("[bench_framework] benchmark '{}' matrix expansion failed",
                 erased_benchmark_key);
            return no_op_finalizer();
        }
        // Apply `--filter cases=`: keep only allow-listed over-keys (algorithm names).
        if (context.filter_cases != nullptr) {
            std::erase_if(state.execution_tuples,
                [&context](const detail::execution_tuple& t){
                    return !context.filter_cases->contains(t.algorithm_name);
                });
            if (state.execution_tuples.empty()) {
                // Benign: the pre-pass already proved >=1 case matches, but a
                // config edited between pre-pass and parse could differ. Skip.
                bioseq_log<logging_level::info>("[bench_framework] '{}' has no cases matching --filter cases=; skipping",
                                erased_benchmark_key);
                return no_op_finalizer();
            }
        } else if (state.execution_tuples.empty()) {
            bioseq_log<logging_level::error>("[bench_framework] benchmark '{}' produced no execution tuples",
                 erased_benchmark_key);
            return no_op_finalizer();
        }
        state.cells = detail::build_cells(state);

        // runs at most one tuple at a time per (test_case, group), so
        // there is no Input contention left to protect against.

        // Populate the progress dashboard's structure for this benchmark.
        // One progress_test_case per test_case in declared order, one
        // progress_group per group of that test_case.
        if (context.progress != nullptr
            && context.benchmark_index < context.progress->per_benchmark.size()) {
            auto& per = *context.progress->per_benchmark[context.benchmark_index];
            per.test_cases.clear();
            per.test_cases.reserve(state.test_cases.size());
            for (std::size_t tc_index = 0; tc_index < state.test_cases.size(); ++tc_index) {
                auto tc = std::make_unique<detail::progress_test_case>();
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
                    tc->finish_ns.store(bioseq::instrumentation::clock::now_ns(),
                                        std::memory_order_relaxed);
                }
                tc->groups.reserve(max_group_count);
                for (std::size_t group_index = 0; group_index < max_group_count;
                     ++group_index) {
                    auto group_ptr = std::make_unique<detail::progress_group>();
                    group_ptr->group_index = group_index;
                    group_ptr->total = per_group_iters[group_index];
                    tc->groups.push_back(std::move(group_ptr));
                }
                per.test_cases.push_back(std::move(tc));
            }
            // `progress_*` helpers (and the renderer) start observing
            // tuple/group counters as `prepare_typed_benchmark` progresses.
            // publish_on_exit still fires on every closure exit and is
            // setup_done false until the closure returned, so every
            // progress_tick_* call returned early via the
            // `progress_test_case_for` setup_done check, leaving every
            // gauge frozen at 0/N for the entire run.)
            per.setup_done.store(true, std::memory_order_release);
        }

        // Bench-lock + serial-concurrency exclusion are enforced by the
        // batch_scheduler's admission predicates. Preparation enqueues this
        // benchmark and returns a caller-thread finalizer; no oneTBB driver
        // worker blocks while the scheduler still needs workers for bodies.

        // --debug: BENCHMARK_STARTED — emitted single-threaded before any
        // CASE_* event for this benchmark (only when it actually has cells).
        if (context.debug_sink != nullptr && !state.cells.empty()) {
            context.debug_sink->emit_benchmark_started(erased_benchmark_key);
        }

        auto typed_finalizer =
            detail::prepare_typed_benchmark<p_input, p_output>(
                state, context,
                captured_on_estimate,
                captured_on_input,
                captured_on_run,
                captured_on_evaluate,
                captured_on_cleanup,
                captured_raw_serializer,
                captured_repeat_finalization,
                captured_iteration_boundaries);

        return [typed_finalizer = std::move(typed_finalizer),
                state_holder,
                context_ptr = &context,
                benchmark_key = erased_benchmark_key]() mutable {
            typed_finalizer();

            auto& final_state = *state_holder;
            auto& final_context = *context_ptr;

            // 4. Post-run validation: missing-evaluation-metric check.
            auto post_run_diagnostics =
                detail::semantic_validate_post_run(final_state);
            detail::drain_diagnostics_to_context(
                final_context, post_run_diagnostics);

            // 5. Evaluate rules and stash this benchmark's report data.
            std::vector<detail::violation> local_violations;
            detail::evaluate_rules(
                final_state, local_violations,
                final_state.paired_rule_evidence_records);

            if (final_context.debug_sink != nullptr) {
                for (const auto& evidence : final_state.paired_rule_evidence_records) {
                    final_context.debug_sink->emit_paired_rule_evidence(evidence);
                }
            }

            if (final_context.violations != nullptr
                && final_context.violations_mu != nullptr) {
                std::lock_guard<std::mutex> guard(
                    *final_context.violations_mu);
                for (auto& rule_violation : local_violations) {
                    final_context.violations->push_back(std::move(rule_violation));
                }
            }
            if (final_context.reports != nullptr
                && final_context.reports_mu != nullptr) {
                std::lock_guard<std::mutex> guard(*final_context.reports_mu);
                final_context.reports->emplace(benchmark_key, state_holder);
            }
        };
    };

    return erased;
}

}  // namespace detail

template<typename p_input, typename p_output>
[[nodiscard]] erased_benchmark create_benchmark(
    std::string title,
    std::string benchmark_key,
    std::string config_path,
    std::function<run_constraints(std::string_view, std::string_view, simdjson::dom::element)> on_estimate_resources,
    std::function<p_input(std::string_view, std::string_view, simdjson::dom::element, const run_constraints&)> on_input,
    std::function<p_output(std::string_view, const parameter_map&, const p_input&)> on_run,
    std::function<std::map<std::string, double>(std::string_view, const parameter_map&, const p_input&, const p_output&)> on_evaluate,
    std::function<void(std::string_view, std::string_view, simdjson::dom::element, const p_input&)> on_cleanup,
    std::function<std::string(std::string_view, const parameter_map&, const p_input&, const p_output&)>
        serialize_raw_sample_payload,
    std::string config_open_path,
    repeat_finalization_callbacks<p_input, p_output> repeat_finalization)
{
    return detail::create_benchmark_impl<p_input, p_output>(
        std::move(title), std::move(benchmark_key), std::move(config_path),
        std::move(on_estimate_resources), std::move(on_input),
        std::move(on_run), std::move(on_evaluate), std::move(on_cleanup),
        std::move(serialize_raw_sample_payload), std::move(config_open_path),
        std::move(repeat_finalization), nullptr);
}

template<typename p_input, typename p_preparation, typename p_output>
[[nodiscard]] erased_benchmark create_benchmark_with_iteration_boundaries(
    std::string title,
    std::string benchmark_key,
    std::string config_path,
    std::function<run_constraints(std::string_view, std::string_view, simdjson::dom::element)> on_estimate_resources,
    std::function<p_input(std::string_view, std::string_view, simdjson::dom::element, const run_constraints&)> on_input,
    std::function<p_output(std::string_view, const parameter_map&, const p_input&)> on_run,
    std::function<std::map<std::string, double>(std::string_view, const parameter_map&, const p_input&, const p_output&)> on_evaluate,
    iteration_boundary_callbacks<p_input, p_preparation, p_output>
        iteration_boundaries,
    std::function<void(std::string_view, std::string_view, simdjson::dom::element, const p_input&)> on_cleanup,
    std::function<std::string(std::string_view, const parameter_map&, const p_input&, const p_output&)>
        serialize_raw_sample_payload,
    std::string config_open_path,
    repeat_finalization_callbacks<p_input, p_output> repeat_finalization)
{
    return detail::create_benchmark_impl<p_input, p_output>(
        std::move(title), std::move(benchmark_key), std::move(config_path),
        std::move(on_estimate_resources), std::move(on_input),
        std::move(on_run), std::move(on_evaluate), std::move(on_cleanup),
        std::move(serialize_raw_sample_payload), std::move(config_open_path),
        std::move(repeat_finalization), std::move(iteration_boundaries));
}

}  // namespace bioseq::bench::framework
