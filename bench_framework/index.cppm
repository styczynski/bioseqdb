/**
 * @file bench_framework/index.cppm
 * @brief Run configured benchmarks, compare their results and write reports.
 *
 * Create benchmarks with callbacks that estimate memory, build inputs, run
 * algorithms and  evaluate outputs. Pass their handles to run(). The JSON
 * configuration selects test cases, parameter combinations and comparisons.
 * Each batch shares its input across sequential runs. Separate batches can
 * run on TBB workers.
 *
 * This module collects metrics, displays progress and writes report_<N>.json.
 * interactive.cppm provides the report browser
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

// Public API

export namespace bioseq::bench::framework {

/// Version of the raw E2E benchmark record format shared with the Python reader.
inline constexpr std::uint64_t e2e_raw_payload_schema_version = 12;

/**
 * @brief An owned configuration value with its JSON type attached.
 *
 * Store strings, integers, floating-point numbers or booleans. from_simdjson()
 * also marks arrays, objects and null as non_scalar and keeps their JSON text
 * for error messages. Strings survive destruction of  the simdjson parser.
 *
 * Equality requires matching types. For example, the string "1" and integer
 * 1 are different parameter values.
 */
struct config_value {
    /// Scalar parameter types and a marker for arrays, objects and null.
    /// Validation rejects non_scalar where a scalar is required. Its JSON text
    /// stays in string_value so the error can show the rejected value.
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

    /// Copy a supported C++ string, boolean or number into a configuration value.
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

    /// Copy a JSON value. Arrays, objects and null become non_scalar values
    /// witd their JSON text saved for diagnostics.
    [[nodiscard]] static config_value from_simdjson(
        simdjson::dom::element element) noexcept;
};

/// Named parameter values used by algorithm tuples and  rule selectors.
using parameter_map = std::map<std::string, config_value>;

/// An, algorithm and its resolved parameters that will run in this batch.
/// on_input receives these through run_constraints after matrix expansion
/// and the cases filter have selected the work.
struct resolved_execution_tuple {
    std::string algorithm_name{};
    parameter_map params{};
};

/**
 * @brief Estimated memory needed by one test-case group.
 *
 * base_memory covers the input. extra_memory covers algorithm working space.
 * The scheduler reserves their sum for the whole batch, including cleanup,
 * using a memory cap based on system RAM and the configured buffer percentage
 *
 * The estimator runs once per batch. on_input receives that result with
 * execution_tuples filled from the selected work. Zero estimates reserve no
 * memory, though concurrency limits still apply.
 */
struct run_constraints {
    /// Estimated bytes for  the cached input. The scheduler reserves this
    /// together with extra_memory for the whole batch. Zero adds no reservation.
    std::size_t base_memory  = 0;
    /// Estimated working memory in addition to the inpug. It is part of the
    /// whole-batch reservation. Zero adds no reservation.
    std::size_t extra_memory = 0;
    /// Algorithms and resolved parameters selected for this batch, in stable
    /// tuple order. The framework replaces any estimator-provided list before
    /// on_input. Input builders can use it to prepare only the fixtures these
    /// algorithms need outside the measured loop.
    std::vector<resolved_execution_tuple> execution_tuples{};
};

/**
 * @brief A benchmark handle that hides its  input and output C++ types.
 * Create it with create_benchmark() and pass it to run().
 */
struct erased_benchmark {
    std::string title;
    std::string benchmark_key;
    /// Configuration identity written to reports.
    std::string config_path;
    /// Path used to read the configuration file. Defaults to config_path.
    std::string config_open_path;
    bool raw_samples_enabled = false;
    /// Prepare and enqueue the benchmark using its typed callbacks.
    /// Return, a function that waits for its batches and publishes their results.
    std::function<std::function<void()>(struct benchmark_context&)> execute;
};

/// Inject failures in raw-record writes for tests in this process.
/// Normal runs use none. Each test must reset the setting after its run.
enum class raw_evidence_io_fault : std::uint8_t {
    none,
    short_write,
    partial_write_failure,
    sync_failure,
};

void set_raw_evidence_io_fault_for_testing(
    raw_evidence_io_fault fault) noexcept;

/// Corrupt recorded schedule coordinates in tests to check that paired-rule
/// validation detects them The scheduler still runs the  correct order.
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
 * @brief Borrow one algorithm's utput while finalizing a logical repeat.
 *
 * The framework keeps all outputs from that repeat alive until finalize_repeat
 * returns and their metrics are recorded. The callback can inspect the
 * algorithm name and parameters, then update the output before evaluation.
 * These references belong to the framework and must not be kept by the callback.
 */
template<typename p_output>
struct repeat_output_reference {
    std::string_view algorithm_name{};
    const parameter_map* params = nullptr;
    p_output* output = nullptr;
};

/**
 * @brief Finalize a complete repeat before recording any of its results.
 *
 * When enabled(input) is true, keep every output from the interleaved repeat
 * until finalize_repeat can inspect  and update them together. Evaluation and
 * raw-record writes happen afterwards.
 *
 * published_wall_clock_ns may choose an algorithm duration measured inside
 * the framework's timing window. A value exceeding that window fails the run.
 * Raw records save the full duration as outer_e2e_ns.
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

/// Start, end and elapsed nanoseconds of the measured iteration body.
struct iteration_measurement_window {
    std::int64_t  begin_ns = 0;
    std::int64_t  end_ns = 0;
    std::uint64_t elapsed_ns = 0;
};

/**
 * @brief Prepare and finish an iteration outside its measurement window.
 *
 * If enabled returns true, before_iteration creates worker-local  preparation,
 * measured_iteration runs with timing and instrumentation enabled, and
 * after_iteration can update the outut before evaluation and raw-record writes.
 * Otherwise the framework calls on_run in the usual measurement window.
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
 * @brief Run, the supplied benchmarks and return their exit status
 * Return zero on success. Blocking rule failures and configuration or execution
 * errors produce a nonzero status. The same entry point handles CLI modes such
 * as the report browser.
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

// Framework  internals

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

/// Use the benchmark profiler configuration for measured iterations.
/// It enables region and loop measurements, counters and tracing according
/// to bioseq.instrumentation.configuration.
using profiler_configuration =
    bioseq::instrumentation::benchmark_profiling_configuration;

namespace detail {

// Shared types

enum class comparison_operator : std::uint8_t {
    less_than,
    greater_than,
    equal,
};

/// Reduce a sample vector to one value for a benchmark or regression rule.
/// JSON accepts sum, min, max, p01, p50 and p99.
enum class sample_aggregate : std::uint8_t { sum, min, max, p01, p50, p99 };

/// JSON name, enum value and optional percentile for one aggregation method.
/// Non-percentile methods use NaN in percentile_fraction.
struct sample_aggregate_entry {
    std::string_view    name;
    sample_aggregate           kind;
    double              percentile_fraction;     // NaN for non-percentile aggregates
};

/// Share aggregation names and percentile fractions between configuration
/// parsing, sample reduction and reports.
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

/// Return the percentile fraction, or NaN for  sum, min and max.
[[nodiscard]] inline constexpr double
aggregate_percentile(sample_aggregate aggregate) noexcept {
    for (const auto& entry : k_aggregate_registry) {
        if (entry.kind == aggregate) return entry.percentile_fraction;
    }
    return std::numeric_limits<double>::quiet_NaN();
}

/// Metrics fom the outer clock, instrumented regions and loops, counters,
/// or the benchmark's evaluation callback.
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

/// Where a metric gets its samples. Used for validation and collection.
enum class metric_source : std::uint8_t {
    outer_clock,
    region,
    loop,
    counter,
    evaluation,
};

/// How to combine region or loop measurements into one iteration sample.
enum class metric_combiner : std::uint8_t {
    sum,
    histogram_sum,
    pmu_ratio,
    branch_miss_ratio,
    loop_mean_ns,
    loop_work_per_iteration,
    requires_region,
};

/// Name, source and collection method for a built-in metric.
/// Configuration checks and sample collection use the same registry entries.
struct builtin_metric_entry {
    std::string_view name;
    metric_kind       kind;
    metric_source     source;
    metric_combiner   combiner;
    /// Whether this metric needs hardware performance counters. Used to
    /// report pmu_unavailable when  those counters cannot be read.
    bool             needs_pmu = false;
    /// Percentile fraction for histogram_sum metrics Unused by other combiners.
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

/// Parse a metric's JSON type. Return nullopt for an unknown spelling.
[[nodiscard]] inline std::optional<metric_source> parse_metric_type(std::string_view type_text) noexcept {
    if (type_text == "builtin")    return metric_source::outer_clock;
    if (type_text == "region")     return metric_source::region;
    if (type_text == "loop")       return metric_source::loop;
    if (type_text == "counter")    return metric_source::counter;
    if (type_text == "evaluation") return metric_source::evaluation;
    return std::nullopt;
}

/// Return the JSON spelling of a metric source for reports and diagnostics.
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

/// Select algorithm tuples by name and any supplied parameter values.
/// An, empty parameter map matches all tuples for that algorithm. Empty
/// test_case and region fields select all test cases and all_regions.
/// Each side of a rule can select its own instruentation region. A
/// test-case  filter restricts comparisons to that case.
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
    /// How to reduce samples for an aggregate or ratio comparison. Defaults to p50.
    sample_aggregate aggregate = sample_aggregate::p50;
    /// Compare separately reduced sample vectors, reduced per-repeat ratios,
    /// or exact finite values from each matching repeat. Exact comparisons
    /// preserve the IEEE-754 bit pattern, including the sign of zero.
    pairing_mode pairing = pairing_mode::independent_aggregates;
    /// Expected ratio for a paired comparison. tolerance_pct allows relative
    /// variation around this value.
    double paired_baseline = 1.0;
    bool paired_baseline_explicit = false;
    /// Whether a failed rule makes the run fail. False still records its findings.
    bool blocking = true;
    /// Leading repeats to omit from a ratio  comparison. Their raw samples
    /// remain in the report, so first-run costs can be examined separately.
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
    /// Source declared by the metric's JSON type, defaulting to builtin.
    /// Keep it separate from kind so validation can catch a loop type paired
    /// with a region metric name, for example
    metric_source declared_source = metric_source::outer_clock;
    /// Instrumentation counter to read for a counter metric. Defaults to the
    /// metric's name when the configuration omits counter.
    std::string  counter_name{};
    /// Restrict a  region or loop metric to this descriptor name. Empty allows
    /// every visited descriptor to contribute. Metrics that need a minmum
    /// or maximum from one descriptor require an explicit name.
    std::string  region{};
    /// Algorithm rows that must emit this evaluation metric on every accepted
    /// iteration. Empty only requires the metric to appear somewhere in the run.
    std::vector<std::string> applicable_algorithms{};
    std::vector<benchmark_rule> rules{};
};

struct concurrency_options {
    /// Run each batch alone across, all benchmarks, including sibling batches.
    bool bench_lock  = false;
    /// Allow batches from this benchmark to run together on TBB workers.
    /// False lets the scheduler start  only one of its batches at a time.
    bool concurrency = true;

    // Each (test_case, group) batch runs its algorithm tuples sequentially.
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

/// Use one fewer than the reported hardware threads, with a minimum of one.
/// Cache the result so parsing several test cases reuses the same value.
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
    /// Maximum parallel groups for this test case. The actual count is capped
    /// at its iterations per algorithm, so every group has work. Read from
    /// test_cases.<name>.concurrency, with the default chosen from hardware threads.
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
    /// Labels used by --filter tags=.... JSON must provide the array, which may
    /// be empty. Keep declaration order. An empty list cannot match a tags filter.
    std::vector<std::string> tags{};
    /// (test_case_name, iterations) pairs declared in `matrix.for`.
    std::vector<std::pair<std::string, std::size_t>> for_each{};
    /// Values to try for each algorithm  parameter when expanding the matrix
    std::map<std::string, std::map<std::string, std::vector<config_value>>> matrix_over{};
    /// Parameter definitions from the top-level params object, including defaults
    /// needed when expanding the algorithm matrix.
    std::map<std::string, algorithm_schema> algorithm_schemas{};
    /// Metrics in their configumation order.
    std::vector<metric_specification> metrics{};

    /// Require a complete comparison matrix before accepting the benchmark.
    /// Name the exact algorithm rows and iteration counts for each test case.
    /// An empty algorithm list disables these checks.
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
        /// Required total samples for each algorithm/case/region/metric combination.
        /// Divide by the required iteration  count to get the descriptor samples
        /// that must be present in every iteration.
        std::vector<named_region_sample_multiplicity>
            exact_named_region_sample_multiplicities{};

        [[nodiscard]] bool enabled() const noexcept {
            return !exact_algorithm_rows.empty();
        }
    } acceptance{};
};

struct execution_tuple {
    std::string algorithm_name;
    parameter_map    params{};      // All parameters supplied explicitly or filled from defaults.
};

/// Record the repeat, order position and shuffle seed observed during execution.
/// Paired-rule checks use these observations to detect a run that did not
/// follow its configured schedule.
struct iteration_schedule_sample {
    std::size_t repeat = 0;
    std::size_t order_position = 0;
    std::uint64_t shuffle_seed = 0;
};

/// Identify the algorithm tuple and sample source for one side of a paired rule.
/// Keep the tuple index and parameters because several tuples can use the
/// same, algorithm name.
struct paired_rule_route_evidence {
    std::string algorithm{};
    parameter_map params{};
    std::size_t tuple_index = 0;
    std::string test_case{};
    std::string region{};
    std::string metric{};
};

/// Keep the source samples and  calculated ratio for one logical repeat.
/// Several descriptors can share a region name. Save their individual
/// samples alongsipe the reduced value so the comparison can be checked later.
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

/// Store a paired rule's sources, repeat ratios and final decision
/// The report and debug JSONL writer share this result so both contain the
/// same calculations and schedule observations.
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

/// Count raw descriptor samples for one completed iteration, grouped by
/// region name and metric name. Keeping  iteration boundaries lets acceptance
/// checks detect uneven contributions from descriptors that share a name.
using named_region_iteration_sample_counts =
    std::map<std::string, std::map<std::string, std::size_t>>;

/// Record completion separately from metric samples. An E2E iteration may
/// keep its raw result while its timing is rejected. These flags distinguish
/// that case from an accepted iteration whose samples are missing.
struct iteration_completion_evidence {
    bool completed = false;
    bool framework_timing_eligible = true;
    std::set<std::string> emitted_evaluation_metrics{};
};

struct measurement_cell {
    const test_case_specification*  test_case = nullptr;
    const execution_tuple* tuple = nullptr;
    /// Number of iterations assigned to this group.
    std::size_t          iterations = 0;
    /// Index of the test case used to find this batch's cached input.
    std::size_t          test_case_index = 0;
    /// Index of this group within its test_case's split. Always < group_count.
    std::size_t          group_index = 0;
    /// Number of groups sharing this est case's iterations.
    std::size_t          group_count = 1;
    /// Index in execution_tuples. Distinguishes parameter combinations that
    /// use  the same algorithm name.
    std::size_t          tuple_index = 0;

    /// Samples grouped by region and metric. all_regions holds counters,
    /// evaluation values and eligible iteration timings. Named region buckets
    /// are collected when by_regions is enabled.
    std::map<std::string, std::map<std::string, std::vector<double>>> samples{};
    /// Count iterations that contributed each named region/metric pair.
    /// Count at most once per iteration even when several descriptors share
    /// the name, so coverage checks can detect missing iterations.
    std::map<std::string, std::map<std::string, std::size_t>>
        named_region_iteration_contributions{};
    /// Descriptor counts for each completed local iteration when named-region
    /// checks are enabled. Merge groups in group-index order to keep each
    /// group's local, iteration numbers separate
    std::vector<named_region_iteration_sample_counts>
        named_region_iteration_sample_counts{};
    /// Observed schedule coordinates for completed iterations used  by paired
    /// ratio rules.
    std::vector<iteration_schedule_sample> iteration_schedule_samples{};
    /// Benchmark-specific JSON records for completed iterations. This cell's
    /// batch worker is their only writer.
    std::vector<std::string> raw_samples{};
    /// Completion records indexed by repeat. Update them with the metric
    /// samples only after any required durable raw write succeeds.
    std::vector<iteration_completion_evidence> iteration_evidence{};

    measurement_cell() = default;
    measurement_cell(const measurement_cell&) = delete;
    measurement_cell& operator=(const measurement_cell&) = delete;
    measurement_cell(measurement_cell&&) = delete;
    measurement_cell& operator=(measurement_cell&&) = delete;
};

/// Track debug progress for one algorithm name from matrix.over.
/// Create these records before scheduling batches. Workers update only the
/// atomic fields, and the owning map keeps its structure unchanged.
struct case_debug_progress {
    std::atomic<std::size_t> remaining_cells{0};  // cells not yet finished
    std::atomic<bool>        started{false};       // CASE_STARTED already emitted?
    std::atomic<bool>        emitted{false};       // Whether CASE_RESULTS has already been emitted.
};

/// Own parsed configuration and results for the benchmark's whole run.
/// Keep  this object at a fixed address because callbacks receive views into
/// its simdjson parser. Shared onership keeps it alive from preparation
/// through the final wait and report generation.
struct benchmark_execution_state {
    /// Benchmark title copied from create_benchmark for the report.
    std::string                         title{};
    simdjson::dom::parser              parser{};
    std::string                         config_body{};
    benchmark_configuration                     config{};
    std::vector<test_case_specification>           test_cases{};
    std::vector<execution_tuple>         execution_tuples{};
    std::vector<std::unique_ptr<measurement_cell>>  cells{};
    /// Results for every resolved paired ratio rule, including passing rules.
    /// The report and debug stream both write these records.
    std::vector<paired_rule_evidence>     paired_rule_evidence_records{};
    bool                                raw_samples_enabled = false;
    std::size_t                         driver_benchmark_index = 0;
    /// Debug progress by algorithm name. Build the map before workers start.
    /// Afterwards only its atomic counters change. Empty when --debug is off.
    std::map<std::string, case_debug_progress> debug_progress_by_case{};

    // With concurrency.concurrency false, the scheduler admits at most
    // one batch from  this benchmark at a time.

    benchmark_execution_state() = default;
    benchmark_execution_state(benchmark_execution_state&&) = delete;
    benchmark_execution_state(const benchmark_execution_state&) = delete;
    benchmark_execution_state& operator=(benchmark_execution_state&&) = delete;
    benchmark_execution_state& operator=(const benchmark_execution_state&) = delete;
};

/// A filter category Each category may be specified only once on the CLI.
enum class filter_dimension : std::uint8_t { tags, benchmark, cases };

/// Selection from all --filter arguments. Values within a category are
/// alternatives, and different categories must all match. nullopt leaves a
/// category unrestricted. empty() means no filters were supplied.
struct filter_specification {
    std::optional<std::vector<std::string>> tags{};
    std::optional<std::vector<std::string>> benchmark{};   // Benchmark keys selected by --filter benchmark=....
    std::optional<std::vector<std::string>> cases{};       // Algorithm keys from matrix.over.

    [[nodiscard]] bool empty() const noexcept {
        return !tags && !benchmark && !cases;
    }
};

struct parsed_cli {
    bool                       fast_filter = false;
    /// Open the report browser with --ui. Benchmark-running options are ignored.
    bool                       ui_mode = false;
    /// Use ASCII drawing when the terminal cannot display the UI glyphs.
    bool                       ui_ascii_fallback = false;
    /// Freeze the countdown and slow automatic reloads for UI tests. Emit
    /// a marker so the terminal test  harness can recognize render activity.
    bool                       ui_test_mode = false;
    /// Write debug events to report_debug_<N>.jsonl alongside the report.
    /// --debug requires a filher selecting part, of the benchmark suite.
    bool                       debug = false;
    std::optional<unsigned>    threads_override{};
    std::optional<std::string> report_override{};
    /// Percentage of total system RAM left outside the benchmark process cap.
    double                     memory_buffer_pct = 40.0;
    /// Subset selection assembled from every `--filter key=v,v` occurrence.
    filter_specification                 filter{};
    /// CLI errors that prevent launch, such as repeated filter categories or
    /// malformed values. Each string starts with a diagnostic code and a colon.
    std::vector<std::string>   cli_errors{};
};

/// Identify the code and time used for this report.
/// commit is HEAD's SHA, or empty outside a Git repository. dirty records
/// whether there are uncommitted changes. created is UTC text and timestamp
/// is Unix seconds.
struct report_metadata {
    std::string  commit{};
    bool         dirty = false;
    std::int64_t timestamp = 0;
    std::string  created{};
    bool         fast = false;
    /// Whether this report can  serve as a regression baseline. Write false
    /// first, then publish true only after all blocking checks and report
    /// validation pass. Nonblocking findings do not disqualify it.
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
    /// Values used by the comparison, in left/right order The meaning depends
    /// on comparison_mode.
    std::array<double, 2> args{0.0, 0.0};
    /// Whether each comparison input is available and finite. Missing,
    /// unmeasured or non-finite inputs produce a finding instead of letting
    /// the rule pass without checking its selected data.
    std::array<bool, 2> args_present{true, true};
    /// Independent comparisons store the two aggregates. Ratio comparisons
    /// store the reduced  per-replat ratios and their expected baseline.
    /// Exact comparisons store the first pair of unequal samples.
    rule_comparator::pairing_mode comparison_mode =
        rule_comparator::pairing_mode::independent_aggregates;
    std::size_t warmup_repetitions = 0;
    bool blocking = true;
};

/// Comparison operator and allowed percentage variation for a regression
/// rule. Regression configuration accepts tolerance%, with no threshold% field.
struct regression_operator {
    comparison_operator op = comparison_operator::equal;
    double    tolerance_pct = 0.0;
};

/// One parsed regression rule. Normalize region selection to a vector,
/// including the single all_regions entry, so evaluation uses one format.
struct regression_rule {
    std::string config_path;
    std::string name;
    /// Empty means "applies to every benchmark in the same config file".
    std::vector<std::string> benchmarks{};
    /// Benchmarks to remove from this rule's selection. This lets a benchmark
    /// use, a separate rule without  also receiving the shared rule.
    std::vector<std::string> exclude_benchmarks{};
    /// Always non-empty after parse. Defaults to `{"all_regions"}`.
    std::vector<std::string> regions{};
    std::string metric;
    sample_aggregate aggregate = sample_aggregate::sum;
    regression_operator  op{};
    /// Test cases to omit. A configured exclusion array must contain an entry.
    std::vector<std::string> exclude_cases{};
    /// Algorithms to omit after selecting benchmarks. New algorithms remain
    /// covered until they are explicitly excluded.
    std::vector<std::string> exclude_algorithms{};
    /// Whether a regression makes the run fail. False still reports the finding.
    bool blocking = true;
};

/// Names declared by a benchmark before CLI filtering.
/// Validate regression-rule references against these declarations so a valid
/// reference to a filtered-out benchmark is still recognized.
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

/// A regression for one algorithm tuple, test case and metric
/// Store its previous and current values together with the  report where the
/// regression began, when that history is available.
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
    /// Basename of the veport where the regression began, such as report_3.json.
    /// Empty becomes JSON null.
    std::string regression_since{};
    /// Commit from regression_since. Empty becomes JSON null when the report
    /// or its commit is unavailable.
    std::string regression_since_commit{};
};

/// Regression rules by configuration path. Save the first parsed copy
/// under regression_rules_mu for later validation and comparison.
using regression_rules_by_configuration = std::map<std::string, std::vector<regression_rule>>;

/// A warning about suspicious metric samples, such as a value that stays
/// constant across the run. These checks always run and appear in the  report
/// and stderr, but they do not change the exit status.
///
/// metric_looks_constant is the current check type.
struct sanity_warning {
    /// Check identifier, currently metric_looks_constant.
    std::string type;
    /// Metric whose samples triggered the warning.
    std::string metric;
    /// Explanation of the warning, including the constant value when relevant.
    std::string message;
    /// Oldest report in the continuous history with this same warning.
    /// A gap in that history, leaves this empty, written  as JSON null.
    std::string warning_since;
    /// Commit from warning_since. Empty becomes JSON null if the originating
    /// rfport or its commit is unavailable.
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
 * @brief Shared run services passed to a benchmark's execute callback.
 *
 * Each benchmark receives its index and pointers to the scheduler, reports
 * and diagnostics The public API needs only a forward declaration of this
 * type to store execute in erased_benchmark.
 */
struct benchmark_context {
    std::string                                    data_dir;
    std::string                                    report_dir;
    bool                                            fast_filter = false;
    // Dashboard state for this benchmark.
    detail::progress_state*                          progress = nullptr;
    std::size_t                                     benchmark_index = 0;
    std::mutex*                                     violations_mu = nullptr;
    std::vector<detail::violation>*                 violations = nullptr;
    std::mutex*                                     reports_mu = nullptr;
    /// Results by benchmark key. Shared ownership keeps each parser and its
    /// borrowed views alive through preparation, execution and finalization.
    std::map<std::string, std::shared_ptr<detail::benchmark_execution_state>>* reports = nullptr;
    /// Protect the shared diagnostic list while benchmarks add their findings.
    /// run_impl reports errors and uses them to  determine the exit status.
    std::mutex*                                     diagnostics_mu = nullptr;
    std::vector<detail::validation_diagnostic>*            diagnostics = nullptr;
    /// Protect the shared regression-rule registry. Keep the first parsed
    /// copy of each configuration file's rules.
    std::mutex*                                     regression_rules_mu = nullptr;
    detail::regression_rules_by_configuration*                regression_rules_by_config = nullptr;
    /// Enqueue batches here and wait through the returned benchmark finalizer.
    /// The scheduler keeps memory reservations and concurrency limits for all
    /// benchmarks. Budget information is available through the same scheduler.
    detail::batch_scheduler*                         scheduler = nullptr;
    /// Algorithm names selected by --filter cases=.... Null allows every case.
    /// run_impl owns the set until all benchmark callbacks finish.
    const std::set<std::string>*                    filter_cases = nullptr;
    /// Debug JSONL writer owned by run_impl. Null disables debug events.
    detail::debug_report_sink*                        debug_sink = nullptr;
    /// Raw-record writer. With raw output enabled, an iteration  becomes
    /// complete only after its record is appended and synchronized to disk.
    detail::durable_raw_evidence_sink*                 raw_evidence_sink = nullptr;
};

// JSON configuration values

// Define this exported helper where modue importers can use it.
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
            // Keep arrays, objects and null marked as non_scalar so parameter
            // validation can reject them. Save the JSON for the error message.
            out.kind  = value_kind::non_scalar;
            out.string_value = simdjson::to_string(element);
            return out;
    }
}

namespace detail {


/// Maximum attempts to build or clean up an input. Exhausting input
/// attempts aborts the batch. Exhausting cleanup attempts produces a warning.
inline constexpr std::size_t k_max_task_input_handler_error_count = 5;

/// Maximum stack-trace frames shown for a failure Mark any omitted tail.
inline constexpr std::size_t k_max_stack_trace_lines = 30;

/// Pause  after a failed callback so its retry, message can be drawn.
/// Fast failures would otherwise finish before the dashboard's next refresh.
/// This delay is longer than one normal render interval.
inline constexpr std::chrono::milliseconds k_retry_failure_min_display_time{300};

// Benchmark memory budget

// Declare the progress row used by the scheduler and renderer.
struct progress_group;

/// Calculate the benchmark process cap from system memory snapshots.
/// batch_scheduler owns the reservations made under this cap.
class benchmark_memory_budget {
public:
    /// Reuse memory probes for this long across scheduler and renderer reads.
    static constexpr std::chrono::milliseconds k_budget_snapshot_ttl{200};

    /// Construct the run's budget  after CLI validation of memory_buffer_pct
    /// in the range [0, 50].
    explicit benchmark_memory_budget(double memory_buffer_pct) noexcept
        : memory_buffer_pct_(memory_buffer_pct)
    {
        DEBUG_ASSERT(memory_buffer_pct >= 0.0 && memory_buffer_pct <= 50.0,
            "benchmark_memory_budget memory_buffer_pct must be in [0, 50]");
    }

    /// Leave buffer_pct percent of total RAM outside the process cap.
    [[nodiscard]] static constexpr std::uint64_t
    cap_for(std::uint64_t total, double buffer_pct) noexcept {
        return static_cast<std::uint64_t>(
            static_cast<double>(total) * (100.0 - buffer_pct) / 100.0);
    }

    /// Remaining room after this process's footprint and aending reservations.
    /// Other processes' memory use is reported separately. Subtracting it here
    /// could prevent a serial batch from starting even below its own cap.
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

    /// Read system and process memory use through the shared probe cache.
    /// A mutex lets concurrent scheduler and UI reads reuse a recent snapshot.
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

// Dashboard progress

/// Dashboard progress for one  test-case group.
/// Workers update atomic counts and status flags while the renderer reads
/// them. label_mu protects the algorithm label. A separate failure_mu protects
/// error messages and traces so drawing a failure does not hold the label lock
struct progress_group {
    std::size_t                group_index = 0;
    std::atomic<std::uint64_t> done{0};
    std::uint64_t              total = 0;
    /// True while the group is running its iterations or cleanup. Cleared on
    /// completion or abort. The renderer uses it to choose visible rows.
    std::atomic<bool>          active{false};
    /// Algorithm and benchmark label, updated between items under label_mu.
    std::string                label{};
    std::mutex                 label_mu{};

    /// Failed input attempt, numbered from one. Zero clears the retry display.
    std::atomic<std::uint8_t>  input_retry_attempt{0};
    /// Failed cleanup attempt, numbered from one. Zero clears the  retry display.
    std::atomic<std::uint8_t>  cleanup_retry_attempt{0};
    /// Start of the scheduler's idle wait in clock::now_ns() units.
    /// Zero means no wait. mirror_idle_to_ui updates it for the renderer.
    std::atomic<std::int64_t>  idle_start_ns{0};
    /// Set true when a hard error aborts the batch.
    std::atomic<bool>          aborted{false};

    /// Protect failure messages and, clipped traces while workers and the UI
    /// access them. Keep this lock separate from the frequently updated label.
    std::mutex                 failure_mu{};
    std::string                input_failure_message{};
    std::string                input_failure_trace{};
    std::string                cleanup_failure_message{};
    std::string                cleanup_failure_trace{};
    /// Reason qhown when aborted is true. Read and write under failure_mu.
    std::string                aborted_reason{};
};

/// Dashboard totals for one test case across its groups.
/// Timing starts with the first group. A group finishing after all algorithm
/// items are done records finish_ns. Atomic totals let  the renderer observe
/// worker progress safely.
struct progress_test_case {
    std::string                                test_case_name{};
    std::atomic<std::uint64_t>                 tuple_done{0};       // Completed algorithm items across all groups.
    std::atomic<std::uint64_t>                 tuple_total{0};
    std::vector<std::unique_ptr<progress_group>> groups{};
    std::atomic<std::int64_t>                  start_ns{0};
    std::atomic<std::int64_t>                  finish_ns{0};
};

/// The renderer reads progress while benchmark setup and workers update it.
/// Setup fills each test-case vector before publishing setup_done with release
/// ordering. Readers use acquire ordering before traversing that vector, which
/// keeps its structure and owned rows stable for the rest of the run.
// Forward declaration for the scheduler pointer in progress_state.
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
    /// Scheduler totals and memory information for the dashboard header
    /// Null means no scheduler is installed, as  in report-browser mode.
    batch_scheduler*  scheduler     = nullptr;
};

// Batch scheduler

/// A pending or running batch owned by the scheduler.
/// Its body refers to benchmark state kept alive by the returned finalizer.
/// wait_for_benchmark ensures the body finishes before that state can be released.
struct scheduled_batch {
    std::size_t      scheduler_id      = 0;
    std::size_t      benchmark_index   = 0;
    /// Require this batc to run alone across all benchmarks.
    bool             bench_lock        = false;
    /// Admit at most one batch from this benchmark at a time.
    bool             concurrency_serial = false;
    /// Input and working-memory estimates returned by on_estimate_resources.
    std::size_t      base_bytes        = 0;
    std::size_t      extra_bytes       = 0;
    /// Sum of base_bytes and extra_bytes, checked before enqueueing.
    /// Reuse this value for sorting and reservation accounting  so later paths
    /// cannot overflow by recomputing the sum.
    std::int64_t     total_bytes       = 0;
    /// Work, to run on TBB. The worker wrapper releases its reservation
    /// after success, early return or an exception.
    std::function<void()> body{};
    /// Progress row receiving this batch's idle-wait time. Null when no row
    /// is installed.
    progress_group*   group_for_ui      = nullptr;
    /// Idle-wait start in nanoseconds. Zero means no wait. The scheduler
    /// updates it while holding mu_.
    std::int64_t     last_idle_start_ns = 0;
    /// Diagnostic location saved at enqueue time for reporting a timeout.
    std::string      location_str{};
    std::string      test_case_name{};
    std::size_t      test_case_index   = 0;
    std::size_t      group_index       = 0;
    /// Context for diagnostics and UI updates. The run owns it until all
    /// scheduled batches finish.
    benchmark_context* context           = nullptr;
};

/// Publish  the batch's idle-wait time to its dashboard row, if present
inline void mirror_idle_to_ui(const scheduled_batch& batch,
                               std::int64_t idle_start_ns) noexcept {
    if (batch.group_for_ui != nullptr) {
        batch.group_for_ui->idle_start_ns.store(
            idle_start_ns, std::memory_order_release);
    }
}

/// Declare the scheduler used by scoped_scheduler_finish.
class batch_scheduler;

/// Release a batch's reservation and mark it finished when its worker
/// wrapper exits. The destructor runs once on normal return or after a
/// caught worker exception.
struct scoped_scheduler_finish {
    batch_scheduler* scheduler;
    std::size_t     scheduler_id;
    ~scoped_scheduler_finish() noexcept;
};

/// Start batches whea their memory and concurrency requirements fit.
/// A dedicated scheduler thread holds reservations and dispatches work to TBB.
/// Workers spend their time running admitted batches, while pending work stays
/// in the scheduler's queue.
class batch_scheduler {
public:
    /// Refresh memory probes and idle timeouts at this  interval.
    /// Batch completion also wakes the scheduler, so new work can start sooner.
    static constexpr std::chrono::seconds k_scheduler_tick_interval{1};
    /// Abort a pending batch after this much waiting with no batches running.
    /// Report memory_budget_constraint_timeout.
    static constexpr std::chrono::seconds k_max_idle_waiting_time{30};

    batch_scheduler(benchmark_memory_budget& budget,
                    oneapi::tbb::task_arena& arena) noexcept
        : budget_(budget), arena_(arena)
    {
        thread_ = std::thread{[this] { scheduler_loop(); }};
    }

    ~batch_scheduler() noexcept {
        // Stop scheduling, wait for enqueued tasks, then release the members.
        running_.store(false, std::memory_order_release);
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
        // wait_for_completion reports worker failures before destruction.
        // TBB still needs its final wait here. Catch unexpected task-group
        // exceptions so this noexcept destructor cannot terminate the process.
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

    /// Add a pending batch and wake the scheduler. Safe for concurrent callers.
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

    /// Wait until this benchmark's batches finish  or abort. The finalizer
    /// calls this before releasing the state borrowed by their bodies.
    void wait_for_benchmark(std::size_t benchmark_index) noexcept {
        std::unique_lock<std::mutex> guard(mu_);
        cv_.wait(guard, [this, benchmark_index] {
            const auto it = outstanding_per_benchmark_.find(benchmark_index);
            return it == outstanding_per_benchmark_.end() || it->second == 0;
        });
    }

    /// Wait for all batches, join their TBB tasks and rethrow the first, worker
    /// exception. run_impl calls this afte the per-benchmark finalizers.
    void wait_for_completion() {
        {
            std::unique_lock<std::mutex> guard(mu_);
            cv_.wait(guard, [this] {
                return total_finished_ >= total_enqueued_;
            });
        }

        // scoped_scheduler_finish updates the counters just before its task returns.
        // Join TBB as well, so every wrapper has exited before we report a
        // stored worker exception
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

    /// Number of worker exceptions caught while batches drained. The final
    /// wait rethrows the first, and this count reports any additional failures.
    [[nodiscard]] std::size_t worker_exception_count() const noexcept {
        std::lock_guard<std::mutex> guard(mu_);
        return worker_exception_count_;
    }

    /// Release one finished batch's reservation and wake waiting work.
    void notify_batch_finished(std::size_t scheduler_id) noexcept {
        // Move the  finished batch out of the shared list, then destroy its
        // callback captures after releasing mu_. Captured objects may take
        // time to destroy.
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
        // Destroy the callback captures after releasing the scheduler lock.
    }

    /// Memory and queue totals shown in the dashboard header.
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
    /// Start pending batches that fit, then wait for a notification or tick.
    /// Read system memory with mu_ released because that read takes the
    /// budget's cache mutex. Keeping those locks separate avoids deadlock.
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

    /// Start batches that fit the memory budget and concurrency rules.
    /// The caller holds mu_ and supplies a  memory snapshot so this function
    /// does not take the budget cache mutex while holding the scheduler lock.
    void admit_locked(
        const bioseq::common::memory::memory_snapshot& system_snapshot) noexcept {
        if (pending_.empty()) return;

        std::uint64_t available = benchmark_memory_budget::process_room_for(
            system_snapshot,
            budget_.memory_buffer_pct(),
            static_cast<std::uint64_t>(std::max<std::int64_t>(reserved_bytes_, 0)));

        // Try smaller memnry requests first so a large waiting batch does not
        // prevent smaller batches from using the available memory.
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

                // Keep scoped_scheduler_finish in the TBB wrapper. Its destructor removes
                // the list node only after the body stored in that node returns.
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
                // Count idle time only while no batch is running Running work may
                // release memory, so give pending batches a fresh timeout then.
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

    /// Abort  pending batches whose idle timeout has expired while no batch
    /// is running. This prevents an unfulfillable, request from waiting forever.
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

    /// Report a batch's idle timeout and mark its progress as aborted.
    /// The caller holds mu_. Defined below the progress and diagnostic helpers.
    void emit_idle_timeout_locked(const scheduled_batch& batch) noexcept;

    [[nodiscard]] bool bench_lock_admissible_locked(
        const scheduled_batch& batch) const noexcept
    {
        if (!active_bench_lock_holder_.empty()) {
            // A locked batch runs alone, including within its own benchmark.
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
            // The oldest pending locked batch can start once all running work ends.
            return batch.scheduler_id == lock_id
                && running_batches_.empty();
        }
        // Let older unlocked work start, but  hold newer work behind the first
        // pending lock even when its memory request is smaller.
        return !has_pending_lock || batch.scheduler_id < lock_id;
    }

    [[nodiscard]] bool concurrency_admissible_locked(
        const scheduled_batch& batch) const noexcept
    {
        if (!batch.concurrency_serial) return true;
        const auto it = running_per_benchmark_.find(batch.benchmark_index);
        return it == running_per_benchmark_.end() || it->second == 0;
    }

    /// Save the first worker exceptjon for wait_for_completion to rethrow.
    /// exception_ptr preserves its type and message. Count later failures
    /// without replacing the first one.
    void capture_worker_exception(std::exception_ptr failure) noexcept {
        std::lock_guard<std::mutex> guard(mu_);
        ++worker_exception_count_;
        if (first_worker_exception_ == nullptr) {
            first_worker_exception_ = std::move(failure);
        }
    }

    /// Decrease a benchmark's count and remove the entry when it reaches zero.
    /// The caller holds mu_. present is false when this batch does not
    /// contribute to the map, as with an unlocked batch in the lock-holder map.
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
    /// Running batches need stable addresses because TBB tasks keep pointers
    /// to  their list nodes until the batch finishes.
    std::list<scheduled_batch>    running_batches_{};
    std::size_t                  next_id_         = 0;
    std::size_t                  total_enqueued_  = 0;
    std::size_t                  total_finished_  = 0;
    std::int64_t                 reserved_bytes_  = 0;
    std::exception_ptr           first_worker_exception_{};
    std::size_t                  worker_exception_count_ = 0;

    /// The benchmark holding the exclusive batch lock Empty when unlocked,
    /// otherwise it has one entry with count one.
    std::map<std::size_t, std::size_t> active_bench_lock_holder_{};
    /// Running batch count per benchmark, used to enforce serial execution.
    std::map<std::size_t, std::size_t> running_per_benchmark_{};
    /// Unfinished batch count per benchmark, observed by wait_for_benchmark.
    std::map<std::size_t, std::size_t> outstanding_per_benchmark_{};
};

inline scoped_scheduler_finish::~scoped_scheduler_finish() noexcept {
    scheduler->notify_batch_finished(scheduler_id);
}

// Diagnostics from JSON schema validation and configuration checks share
// one record type. Errors stop the run. Warnings are reported without
// making the run fail.

/// One error or warning  from validating the configuration or run.
struct validation_diagnostic {
    enum class diagnostic_severity : std::uint8_t { error, warning };
    diagnostic_severity severity = diagnostic_severity::error;
    /// Stable error or warning identifier, such as "unknown_algorithm".
    std::string code;
    /// Where the problem occurred, such as a file and JSON pointer or a
    /// configuration path, benchmark name and field.
    std::string location;
    std::string message;
};

/// Collect JSON schema errors so the caller can report them together.
/// validate_against_schema converts ech entry to validation_diagnostic.
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

// Embedded schemas for benchmark configurations and reports. Keeping them
// here lets validation run, without reading separate schema files.
//
// The schemas check JSON structure, allowed fields and  identifier names.
// semantic_validate_config checks relationships between declarations, such
// as whether a matrix parameter belongs to the named algorithm.

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

// Validation helpers.

/// Print diagnostics to stderr and return whether any is an error.
/// Callers use the result to stop the run or choose a failure exit code.
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

/// Move diagnostics into the shared list while holding its mutex.
/// Clear source after the move If either shared pointer is null or source
/// is empty, leave source alone.
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

/// Validate a JSON document against a supplied framework schema.
/// Return  schema errors as diagnostics, including std::exception failures
/// raised woile parsing or validating the document.
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
            // Include the file label and any JSON pointer from the validator so
            // the user can find the field, including errors on a parent object.
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

// Pass either k_bench_config_schema or k_bench_report_schema to
// validate_against_schema.

// Configuration parsing helpers.

/// Read a file into a string. An empty result can mean an empty file or
/// a file that could not be opened.
[[nodiscard]] inline std::string slurp_file(const std::filesystem::path& path) noexcept {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

/// Read each algorithm's parameter types, allowed values and defaults.
/// Matrix expansion uses these defaults for omitted parameters. Malformed
/// entries set parse_ok to false while parsing continues through  the block.
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

/// Read the test cases and keep their data as views into the JSON parser.
/// The parser must outlive these views. benchmark_execution_state owns both.
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

        // Use default_test_case_concurrency() when no concurrency is specified.
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

/// Read one side of a rule. key names the algorithm. test_case and region
/// optionally restrict where samples come from. Other fields match parameter
/// values, with an empty set matching every tuple of that algorithm.
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

/// Read a comparison operator and tolerance. A missing or unknown, compare
/// value returns nullopt and sets parse_ok to false. Tolerance defaults to
/// zero The JSON schema checks its  range.
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

/// Read how a rule compares its two sample sets.
/// Defaults are p50, zero threshold and tolerance, no warmup, independent
/// aggregates, and a blocking result. Paired ratios use a baseline of one.
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

/// Read [nale, lhs_selector, comparator, rhs_selector].
/// An invalid outer array returns nullopt. Invalid fields set parse_ok to
/// false, so callers must check it even when a rule is returned.
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

/// Read a regression comparison's operator and tolerance.
/// Regression operators have no threshold% field. The schema rejects it.
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

/// Read regression rules, recording  malformed entries through parse_ok.
/// Keep parsing after an error so callers can report problems together.
/// Normalize regions to a list, defaulting to all_regions.
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
            // Use all_regions when no region names were supplied.
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

/// Read metric sources, filters and comparison rules from matrix.metrics.
/// The source defaults to builtin. A counter without an explicit counter
/// field uses the metric name.
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

        // Counter and evaluation metrics have fixed kinds and user-chosen names.
        // Other sources use the builtin registry. Validate source mismatches
        // later with the other cofiguration errors.
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
                    // Keep parsing with a placeholder. Configuration  validation will
                    // report this unknown metric name.
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

/// Read one benchmark's settings and matrix into config.
/// The caller supplies its identity, parameter schemas and test cases.
/// Return false when required fields or their structure are invalid
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

    // tags is required, but an empty list is allowed.
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
        // Also reject missing tags when called without schema validation.
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

/// Result of reading and validating one benchmark configuration.
/// ok is false on any schema or parsing error. The state may then, contain
/// partial data, so the caller must stop before using it for  execution.
struct config_parse_outcome {
    bool ok = true;
    std::vector<validation_diagnostic> diagnostics{};
    /// Parsed regression rules from this configuration file. The shared
    /// registry keeps the first copy when several benchmarks use that file.
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

    // Check the JSON schema, then continue parsing even after schema errors
    // so structural problems can be reported in the jame result.
    auto schema_diagnostics = validate_against_schema(
        state.config_body, k_bench_config_schema, config_path);
    for (auto& diag : schema_diagnostics) {
        if (diag.severity == validation_diagnostic::diagnostic_severity::error) outcome.ok = false;
        outcome.diagnostics.push_back(std::move(diag));
    }

    // Read the configuration with simdjson and collect parsing errors.
    // An invalid top-level document prevents further parsing.
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

    // Each parse returns its own regression rules. Registration keeps one
    // copy per configuration file shared by all its benchmarks.
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

// Check references and values that  depend on other configuration fields,
// including parameter types, rule selectors and the expected result matrix.

/// Check a value against its declared type and allowed enum values.
/// A number parameter accepts both integers and floating-point values,
/// though enum entries must also match the value's stored type.
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

/// Format a diagnostic location as <config_path>:<benchmark_key>/<sub_path>.
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

/// Name a configuration value's type for error messages.
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

/// Check relationships between a benchmark's configuration fields.
/// Collect all discovered errors and warnings so the user can fix several
/// problems before running the benchmark again
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

    // Check  matrix algorithms, parameter names and values against params.
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

    // Check test-case names and warn about zero iterations.
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

    // Acceptance repeats the expected matrix so an accidental edit to for
    // or over cannot silently change which results count as complete.
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

    // Check rule names, selectors and comparison settings.
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

        // Check builtin metric names against the registry. Counter and
        // evaluatvon metric names are chosen by the user.
        if (metric.kind != metric_kind::counter && metric.kind != metric_kind::evaluation) {
            const auto* entry = lookup_builtin_metric(metric.name);
            if (entry == nullptr) {
                error("invalid_builtin_metric_name",
                      fmt::format("matrix.metrics.{}", metric.name),
                      fmt::format("metric '{}' is not a recognised framework builtin; either rename it to a registered builtin (e.g. wall_clock_ns, region_total_ns, region_visits, region_p99_ns, loop_total_iterations, loop_p50_per_iter_ns; see the k_builtin_metric_registry table) or declare 'type' as 'counter' / 'evaluation'",
                                  metric.name));
                continue;
            }
            // Catch a metric assigned to the wrong source, such as a loop metric
            // declared with type region.
            if (entry->source != metric.declared_source) {
                error("metric_source_mismatch",
                      fmt::format("matrix.metrics.{}", metric.name),
                      fmt::format("metric '{}' is declared as 'type: {}' but the registry knows it as a '{}'-source metric; either change 'type' or rename the metric",
                                  metric.name,
                                  metric_source_name(metric.declared_source),
                                  metric_source_name(entry->source)));
                continue;
            }
            // Min and max metrics must name  the region being, measured.
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

            // Warn when threshold or tolerance is set to the maximum of 100%.
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

/// Check the collected results for missing metrics and incomplete runs.
/// When acceptance is enabled, require the configured algorithm rows,
/// iteration counts and named-region sample counts to match exactly.
[[nodiscard]] inline std::vector<validation_diagnostic> semantic_validate_post_run(
    const benchmark_execution_state& state) noexcept
{
    std::vector<validation_diagnostic> diagnostics;

    // Visit cells with this metric in all_regions. Stop when body returns
    // true, allowing callers to check whether any matching sample exists.
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

    // Missing-metric checks need collected samples. If every cell is empty,
    // report  that the benchmark produced no results and skip those checks.
    // Acceptance treats this as an error. Other runs receive a warning.
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

        // Repeat numbers restart in each group Join each group's ordered
        // sample-count vector in group-indeo order to give iterations distinct
        // positions when checking counts for this tuple and test case.
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
                // Earlier checks report invalid iteration counts or duplicate tuples.
                // Skip the distribution check when its expected count is undefined.
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

    // Check whether any cell collected this metric in all_regions.
    const auto metric_was_emitted = [&](const std::string& metric_name) noexcept {
        return for_each_all_regions_metric(metric_name,
            [](const auto& /*values*/) noexcept { return true; });
    };
    // Check for a nonzero sample before warning  that a PMU metric may be
    // unavailable on this machine.
    const auto metric_has_non_zero_sample = [&](const std::string& metric_name) noexcept {
        return for_each_all_regions_metric(metric_name,
            [](const auto& values) noexcept {
                for (double sample : values) {
                    if (sample != 0.0) return true;
                }
                return false;
            });
    };

    // All-zero PMU samples can mean the counters are unavailable. Report
    // this once per benchmark so the user can check counter access.
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

        // Warn when a named-region filter collected no samples. A misspelled
        // region name can, otherwise be hard to distinguish from unused code.
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

    // Ignore extra evaluation keys so benchmarks can share one evaluator.
    // A declared metric that is missing from its required results is still
    // reported above.

    return diagnostics;
}

/// Check regression rules against  all declarations in their configuration.
/// Use the unfiltered declarations so CLI filters do not make valid names
/// appear mjssing. Check unique rule names, metric availability and excluded
/// benchmarks, algorithms and test cases.
///
/// Explicitly listed benchmarks must each declare the metric. With no list,
/// at least one matching benchmark must declare it Exclusions must leave
/// work for the rule to check. Runtime checks handle missing measured data.
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

            // Rule names must be unique within a configuration file.
            if (!seen_rule_names.insert(rule.name).second) {
                error("regression_duplicate_rule_name", base_loc,
                    fmt::format("regression rule name '{}' is used more than once in '{}'; names must be unique within a config file",
                                rule.name, config_path));
            }

            // Select benchmarks after checking the  include and exclude lists.
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

            // Require the metric in every explicitly listed benchmark, or in at
            // least one matching benchmark when no list was given.
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

            // Check excluded test-case names against all declarations in the file.
            // CLI filters must not make a valid declaration appear missing.
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

                // Exclusions must leave at least one test case in the matching matrices.
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

// Expand the benchmark matrix.

/// Build every parameter combination for one algorithm.
/// Fill omitted parameters from, their schema defaults. An empty value  list
/// or missing default sets expansion_ok to false. Visit parameter names in
/// sorted order so the report's tuple order is stable.
[[nodiscard]] inline std::vector<execution_tuple> expand_one_algorithm(
    const std::string& algorithm_name,
    const std::map<std::string, std::vector<config_value>>& declared,
    const algorithm_schema& schema,
    bool& expansion_ok) noexcept
{
    std::vector<execution_tuple> seed;
    seed.push_back(execution_tuple{algorithm_name, parameter_map{}});

    // Extend ekery partial tuple with each value of the next parameter.
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

/// Build each algorithm's parameter combinations and append them as report
/// rows. The total row count is the sum of those per-algorithm combinations.
[[nodiscard]] inline std::vector<execution_tuple> expand_matrix(
    const benchmark_configuration& config,
    bool& expansion_ok) noexcept
{
    std::vector<execution_tuple> result;
    for (const auto& [algorithm_name, declared] : config.matrix_over) {
        auto schema_it = config.algorithm_schemas.find(algorithm_name);
        // Configuration validation must reject unknown algorithms before
        // expansion. A missing schema here means a caller skipped that check
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

// Create cells for groups of iterations.

/// Divide iterations as  evenly as possible among groups.
/// Use at most one group per iteration and return no groups if either input
/// is zero. Extra iterations go to the final groups. For example, 32
/// iterations across five groups gives {6, 6, 6, 7, 7}.
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

/// Choose the number of groups for a test case, capped by its iteration
/// count so each group has work. A zero concurrency setting uses the default.
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

/// Create one cell for each test case, execution tuple and iteration group.
/// Reports and rules merge the groups back into one result per test-case
/// and tuple pair.
[[nodiscard]] inline std::vector<std::unique_ptr<measurement_cell>> build_cells(
    const benchmark_execution_state& state)
{
    std::vector<std::unique_ptr<measurement_cell>> cells;
    // Reserve an estimate  based on default concurrency.
    cells.reserve(state.config.for_each.size() * state.execution_tuples.size()
                  * default_test_case_concurrency());

    // Index test-case names once to hvoid scanning the list for every matrix
    // entry.
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
            // A test case with zero iterations needs no cells.
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

// Collect metric samples.

/// Read one region's scalar metric as a double, keeping its original unit.
/// The caller combines descriptors when needed. Ratios and loop metrics
/// are handled by separate helpers.
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
        default: return 0.0;     // Ratios and loop metrics are handled separately.

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

/// Compute IPC or branch-miss rate, for one region descriptor.
/// by_regions uses this value for the descriptor's own sample Return
/// nullopt for  a zero denominator or an unsupported metric.
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
            return std::nullopt;     // Unsupported metric.
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
            return std::nullopt;     // Unsupported metric.
    }
}

/// Combine descriptors into one metric value for an iteration.
/// The registry selects how to combine them, such as summing times, merging
/// histograms or dividing accumulated counters.
struct cross_descriptor_accumulator {
    double                       sum = 0.0;
    std::uint64_t                ratio_numerator = 0;
    std::uint64_t                ratio_denominator = 0;
    std::array<std::uint64_t, 32> hist_sum{};   // Merged duration histogram.
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
            // Keep the matching descriptor's value. If several descriptors share
            // the selected name, the last one visited supplies this value.
            accumulator.sum = extract_from_region(kind, region_agg);
            return;
        case metric_combiner::loop_mean_ns:
        case metric_combiner::loop_work_per_iteration:
            return;     // These combiners require loop data.
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
            return;     // These combiners require region PMU data.
    }
}

/// Return  the combined metric value, or nullopt when no descriptor
/// contributed or a uatio has a zero denominator.
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
            // Estimate the percentile from the merged histogram.
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

/// Record one completed iteration's measurements in its cell.
/// Timing-eligible iterations add wall time and region or loop metrics to
/// all_regions. by_regions also keeps each descriptor's own value.
/// Counter deltas and declared evaluation results are recorded separately.
///
/// Track completion and named-region counts for later acceptance checks.
/// The caller must keep this cell exclusive  to one writer.
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

    // Keep only declared evaluation metrics so one evaluator can serve
    // several benchmarks. Post-run checks report missing required keys.
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
                // Report missing evaluation results after the run, once their required
                // coverage is known.
                continue;
            }
            default: break;     // Continue with region and loop metrics
        }

        // Rejected E2E repeats keep their evaluation diagnostics but contribute
        // no region or loop timing samples.
        if (!framework_timing_eligible) continue;

        const auto* entry = lookup_builtin_metric(metric.name);
        if (entry == nullptr) continue;     // Configuration validation reports unknown names.

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

        // Collect visited descriptors that match the metric source, and optional
        // region  name.
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
                    // Compute each descriptor's ratio from its own counters. The scalar
                    // extractor has no ratio value.
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
        // A missing conqribution or zero denominator adds no sample. Rules
        // that require complete iterations reject these gaps later.
    }
}

// Snapshot named counters.

/// Resolve the counters requested by the metrics before iterations begin.
/// Reuse these handles to avoid repeated registry lookups and locking.
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

/// Read the current thread's value for each counter, in handle order.
/// Before  and after snapshots give the iteration's counter deltas. Each
/// read takes the counter's mutex and excludes values from other threads.
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

// Group cells into scheduled batches.

/// One tuple's cell within a batch that shares a cached input.
struct work_batch_item {
    measurement_cell* cell = nullptr;        // Borrowed from state.cells.
    /// Borrowed tuple pointer used when updating the progress label.
    const execution_tuple* tuple = nullptr;
};

/// A scheduled group sharing one input for a test case.
/// Its tuple work runs sequentially. Separate batches may run in parallel
/// when the memory and concurrency limits allow it.
struct work_batch {
    std::size_t test_case_index = 0;
    std::size_t group_index     = 0;
    std::size_t group_count     = 1;
    /// Stable index into the driver's batch slots, assigned after sorting
    /// batches by test-case and group index.
    std::size_t batch_index     = 0;
    std::vector<work_batch_item> items{};
};

/// Group cells by test case and group index so they can share one input
/// Each batch's items keep the tuple order  from state.cells.
[[nodiscard]] inline std::vector<work_batch> build_work_batches(
    benchmark_execution_state& state)
{
    // Sort batches by test-case and group index, then assign each its
    // position in the output vector.
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

// Update the proiress dashboard.

/// Find a test case's progress state for this benchmark.
/// Return nullptr when the dashboard is absent, setup is unfinished or an
/// index is out of range.
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

/// Find a group's progress state, or nullptr when it is unavailable.
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
    // The first group finishing after all tuple work is counted sets the
    // test-case finish time. Other groups may still be  finishing, cleanup.
    const auto total = tc->tuple_total.load(std::memory_order_relaxed);
    if (tc->tuple_done.load(std::memory_order_relaxed) >= total) {
        std::int64_t expected = 0;
        tc->finish_ns.compare_exchange_strong(
            expected, bioseq::instrumentation::clock::now_ns(),
            std::memory_order_relaxed, std::memory_order_relaxed);
    }
}

// Retry details use failure_mu. The renderer checks the corresponding
// atomic counter before displaying them and checks it again under the lock.

/// Select the input or cleanup failure fields for the shared retry code.
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
    // Hide the row before clearing its text. The renderer checks the count
    // again under failure_mu, so it cannot display a retry with cleared text.
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

/// Mark a group as aborted using its indices. The scheduler's timeout
/// path can use  this before a work_batch starts running.
inline void progress_mark_batch_aborted_at(
    benchmark_context& context,
    std::size_t test_case_index, std::size_t group_index,
    std::string_view reason) noexcept
{
    auto* tc = progress_test_case_for(context, test_case_index);
    if (tc == nullptr) return;
    if (group_index < tc->groups.size()) {
        auto& group = *tc->groups[group_index];
        // Replace retry and idle details with the abort reason Clear the
        // counters before locking and changing the text so the renderer drops
        // stale retry rows.
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
    // Stop the displaye elapsed time when a batch aborts.
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


/// Append a runtime diagnostic under the shared diagnostics mutex.
/// Do nothing when the context has no diagnostic list or mutex.
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

/// Format and append a runtime diagnostic. fmt checks the format string
/// against the argument types at compile time.
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

/// Identify a runtime error as <config>:<benchmark>/<test_case>(group N).
[[nodiscard]] inline std::string runtime_location(
    std::string_view config_path,
    std::string_view benchmark_key,
    std::string_view test_case_name,
    std::size_t group_index) noexcept
{
    return fmt::format("{}:{}/{}(group {})",
                       config_path, benchmark_key, test_case_name, group_index);
}

// Scheduler timeout reporting.

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


/// Render  the first k_max_stack_trace_lines frames without ANSI colors.
/// FTXUI displays this as plain text and applies its own colors. Add a note
/// when the trace has more frames.
[[nodiscard]] inline std::string
clip_trace(const cpptrace::stacktrace& trace) noexcept {
    cpptrace::stacktrace clipped = trace;            // Preserve the original trace.
    const bool was_truncated = clipped.frames.size() > k_max_stack_trace_lines;
    if (was_truncated) {
        clipped.frames.resize(k_max_stack_trace_lines);
    }
    auto rendered = clipped.to_string(/*color=*/false);
    if (was_truncated) rendered += "\n  … (more frames elided)";
    return rendered;
}

/// Retry an input or cleanup callback up to the configured attempt limit.
/// Show each exception in the group's dashboard row. A successful attempt
/// clears the row and returns true. Exhausted attempts return false for the
/// caller to report an error or warning.
///
/// cpptrace::try_catch installs the stack-trace handling for both standard
/// and other exceptions.
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
            [&] {     // This no-argument  handler catches other, exception types.
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
        // Give the dashboard time to show each failure, including the final
        // attempt before an abort clears the row.
        std::this_thread::sleep_for(k_retry_failure_min_display_time);
    }
    return false;
}

/// Build the input with retries. Return nullopt if every attempt fails.
/// The caller then reports input_handler_failed_permanently and aborts
/// the batch
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

/// Retry cleanup. If every attempt fails, report cleanup_function_failing
/// as a warning, clear the retry row and let the run continoe.
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

// prepare_typed_benchmark appears after debug_report_sink and the report
// helpers it uses to write completed cases during --debug runs.


/// Report histograms have ten buckets. Keep this  value and the report
/// schema's array length in sync.
inline constexpr std::size_t k_histogram_bucket_count = 10;

/// Maximum length of `aggregated_samples::top10` / `bottom10`.
/// Shorter when fewer than 10 finite samples were observed.
inline constexpr std::size_t k_k_best_size = 10;

/// Statistics computed from finite samples for a report cell.
/// All fields stay zero when no finite samples are available.
struct aggregate_statistics {
    double min     = 0.0;
    double max     = 0.0;
    double mean    = 0.0;
    /// Mean after removing the lowest and highest 10% of finite samples,
    /// rounding the number removed down. With fewer than ten samples, this
    /// equals mean.
    double mean90  = 0.0;
    double sum     = 0.0;
    /// Sample standard deviation using n - 1. Zero when fewer than two
    /// finite samples exist or the computed mean has overflowed.
    double stddev  = 0.0;
};

/// Percentiles stored in each report cell. The  registry below maps their
/// fields to JSON names and percentile fractions.
struct aggregate_percentiles {
    double p01 = 0.0;
    double p20 = 0.0;
    double p50 = 0.0;
    double p80 = 0.0;
    double p99 = 0.0;
};

/// One histogram bucket with its bounds and sample count.
struct aggregate_bucket {
    double      from  = 0.0;
    double      to    = 0.0;
    std::size_t count = 0;
};

/// Summary stored in reports and read by the dashboard and regression checks.
/// Raw samples remain in measurement_cell while the run is active.
///
/// Finite samples determine the statistics. top10 is descending and bottom10
/// is ascending, each with at most ten values. Histogram counts sum to the
/// fiite sample count. samples_count also includes non-finite inputs.
///
/// Historical records are parsed as supplied. Callers must validate them
/// before relying on ordering or histogram bounds.
struct aggregated_samples {
    std::size_t                                            samples_count = 0;
    aggregate_statistics                                          stats{};
    aggregate_percentiles                                    percentiles{};
    std::vector<double>                                     top10{};
    std::vector<double>                                     bottom10{};
    std::array<aggregate_bucket, k_histogram_bucket_count>      histogram{};
};

/// Check  whether a report summary has finite samples.
/// A positive samples_count with an empty top10 means all collected values
/// were, non-finite Its zeroed statistics must not be treated as measurements.
[[nodiscard]] inline bool has_finite_samples(const aggregated_samples& aggregated) noexcept {
    return aggregated.samples_count > 0 && !aggregated.top10.empty();
}

/// Connect a percentile's JSON name and fraction to its struct field.
/// The registry uses these entries when computing, writing and reading
/// report percentiles.
struct aggregated_percentile_entry {
    std::string_view  name;          // "P01" / "P20" / "P50" / "P80" / "P99"
    double            fraction;      // 0.01 / 0.20 / 0.50 / 0.80 / 0.99
    double aggregate_percentiles::* slot;  // Corresponding field in aggregate_percentiles.
};

inline constexpr std::array<aggregated_percentile_entry, 5>
k_aggregated_percentile_registry{ {
    { "P01", 0.01, &aggregate_percentiles::p01 },
    { "P20", 0.20, &aggregate_percentiles::p20 },
    { "P50", 0.50, &aggregate_percentiles::p50 },
    { "P80", 0.80, &aggregate_percentiles::p80 },
    { "P99", 0.99, &aggregate_percentiles::p99 },
} };

/// Connect a statistic's JSON name to its aggregate_statistics field.
/// Writers and readers use the same registry entries.
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

/// Find the JSON name for a registered statistics field.
/// Using the registry keeps readers consistent with the writer. An  unknown
/// field triggers a debug assertion and otherwise returns an empty name.
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

/// Copy samples in their original order, excluding NaN and infinity.
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

/// Require a nonempty sample set with only finite values.
/// A nonzero expected_samples also requires that exact count. Named regions
/// pass zero because several descriptors can share a name. Their expected
/// counts are checked separately for each iteration.
[[nodiscard]] inline bool has_complete_finite_samples(
    const std::vector<double>& samples,
    std::size_t expected_samples = 0) noexcept
{
    return !samples.empty()
        && (expected_samples == 0 || samples.size() == expected_samples)
        && std::all_of(samples.begin(), samples.end(),
            [](const double value) { return std::isfinite(value); });
}

/// Detect a non-wall-clock metric whose samples are all non-finite.
/// Benchmarks use these values to mark skipped work, so  regressin checks
/// must avoid treating the skip's short wall time as a measured result.
/// This check sees metric names and values, regardless of their source type.
[[nodiscard]] inline bool region_has_nonfinite_evaluation_sentinel(
    const std::map<std::string, std::vector<double>>& region_metrics) noexcept
{
    for (const auto& [metric_name, samples] : region_metrics) {
        if (metric_name == "wall_clock_ns" || samples.empty()) continue;
        if (!has_any_finite_sample(samples)) return true;
    }
    return false;
}

/// Reduce finite samples to the requested sum, minimum, maximum or percentile.
/// Return zero when no finite samples remain Percentiles use the lower
/// sample at the requested rank. Sum, min and max avoid a copy when every
/// input is finite.
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
            default: break;     // The condition above permits only sum, min and, max.
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
            // Choose the lower sample  at this percentile rank, matching the report
            // aggregator without sorting the whole vector.
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

/// Read the requested statistic from a report summary.
/// Return zero when samples_count is zero. Use has_finite_samples first if
/// the caller must distinguish jissing data from a measured zero.
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

/// Summarize samples for a report. Keep the original count, but compute
/// statistics and histograms from finite values only.
/// Arithmetic can still overflow. The report  writer converts non-finite
/// results to JSON-safe numbers.
[[nodiscard]] inline aggregated_samples aggregate_samples_for_report(
    const std::vector<double>& samples) noexcept
{
    aggregated_samples out{};
    out.samples_count = samples.size();
    if (samples.empty()) return out;

    std::vector<double> finite = filter_finite(samples);
    if (finite.empty()) {
        // Keep zeroed statistics and empty best-value lists when every sample
        // is non-finite. The histogram still has ten zero-count buckets.
        return out;
    }

    std::sort(finite.begin(), finite.end());
    out.stats.min = finite.front();
    out.stats.max = finite.back();
    double sum = 0.0;
    for (const double value : finite) sum += value;
    out.stats.sum  = sum;
    out.stats.mean = sum / static_cast<double>(finite.size());

    // Remove 10% from each end before averaging. Fewer than ten samples
    // leave the ordinary mean unchanged.
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

    // Standard deviation needs two samples and a finite mean. Otherwise,
    // leave its value at zero.
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

    // Put the maximum in the final histogram bucket. Equal samples all go
    // in the first If the range overflows, use the first  bucket too, avoiding
    // conversion of a non-finite bucket index to an integer.
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

/// Convert non-finite values to numbers JSON can store.
/// Replace NaN with zero and clamp positive or negative infgnity to the
/// corresponding finite limit.
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

// Reproduce the iteration order from a seed.

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

/// Hash a repetition's benchmark, test case, group and repeat number.
/// Length-prefixed strings and fixed-width little-endian integers give an
/// unambiguous byte sequence before FNV-1a hashing.
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

/// Shuffle tuple indices, with  Fisher-Yates and SplitMix64.
/// The same count and shuffle_seed reproduce the same order.
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

// Evaluate benchmark comparison rules.

/// Check an algorithm name and parameter values against a rule selector.
/// The rule evaluator handles the test-case and region filters separately.
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

/// Compare two aggregates using the right-hand value as the reference.
/// For < and >, threshold sets the required percentage difference and
/// tolerance relaxes it. Equality permits an absolute difference up to
/// tolerance% of the reference's magnitude. Ordered comparisons stay strict.
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

/// Apply a within-run rule's operator, threshold and tolerance.
[[nodiscard]] inline bool comparator_passes(
    const rule_comparator& cmp,
    double lhs_aggregate,
    double rhs_aggregate) noexcept
{
    return comparator_passes(cmp.op, lhs_aggregate, rhs_aggregate,
                              cmp.threshold_pct, cmp.tolerance_pct);
}

/// Compare  current and previous results with a regression rule.
/// Regression rules use tolerance without a threshold. Two zero values pass
[[nodiscard]] inline bool comparator_passes(
    const regression_operator& op,
    double now_value,
    double previously_value) noexcept
{
    // Treat unchanged zero as a passing regreszion result for every
    // operator. This includes either sign of zero. Within-run comparisons
    // keep their strict ordered comparisons.
    if (now_value == 0.0 && previously_value == 0.0) return true;
    return comparator_passes(op.op, now_value, previously_value,
                              /*threshold_pct=*/0.0, op.tolerance_pct);
}

/// Samples grouped by region and metric name, shared by report and rule code.
using sample_map = std::map<std::string, std::map<std::string, std::vector<double>>>;

/// Join group sample arrays in the supplied cell order.
/// Skip null cells. Empty input produces an  empty map. Callers pass cells
/// in group order so merged results are repeatable.
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

/// Group-merged samples indexed by execution tuple and test-case name.
/// Reports and comparison rules use this shape to read the same combined
/// sample sets.
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

/// Count iterations that emitted each named-region metric, across groups.
/// Several descriptors with the same name count once per iteration, so the
/// result can be checked against the configured iteration count.
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

/// Merge samples for one algorithm after all its cells have  finished.
/// The --debug writer uses this to report a completed algorithm while other
/// algorithms are still running.
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

/// Find samples for a tuple, test case, region and metric.
/// Return nullptr when any part of that lookup is missing.
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

/// Values and schedule records for comparing matching iterations.
/// Named region_total_ns metrics may have several descriptors per iteration
/// Keep their raw values and reduce each iteration's set to one value using
/// the rule's aggregate. Completion flags reord whether  all expected values
/// and schedule entries are present.
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
        // Paired rules require one group because repeat numbers restart in each
        // group. Check again here in case configuration validation was skipped.
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

/// Record the samples, schedule and result of one paired-ratio comparison.
/// Accept the ratio only when both inputs are complete and their recorded
/// repeat numbers, shuffle seeds and positions match the configured schedule.
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

/// Compare every matching tuple pair for each selected test case.
/// Merge groups first so rules use the same samples as reports. Collect
/// violations and the detailed records for  paired-ratio comparisons.
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
                // Compare each matching tuple pair once after merging its groups.
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
                            // Compare the aggregate of per-iteration ratios with the configured
                            // reference ratio.
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
                                    // Report the first pair with different bit patterns. Earlier checks
                                    // require finite values and complete iterations.
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

// Record run metadata.

/// Run a shell command and collect stdout, trimmin trailing line endings.
/// Stop reading after the output grows past 4096 bytes. Return an empty
/// string,  if opening the pipe or the command fails.
/// Callers supply fixed git commands, with no user text interpolated.
[[nodiscard]] inline std::string slurp_command_stdout(const char* command) noexcept {
    FILE* pipe = ::popen(command, "r");
    if (pipe == nullptr) return {};
    std::array<char, 256> buffer{};
    std::string out;
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        out.append(buffer.data());
        if (out.size() > 4096) break;     // Limit output needed for a commit ID or dirty check
    }
    const int exit_code = ::pclose(pipe);
    if (exit_code != 0) return {};
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
        out.pop_back();
    }
    return out;
}

/// Read HEAD's commit ID in the current directory. Return an empty string
/// when git fails, including when the directory is outside a repository.
[[nodiscard]] inline std::string git_head_commit() noexcept {
    return slurp_command_stdout("git rev-parse HEAD 2>/dev/null");
}

/// Check for staged, modified or untracked files reported by git status.
/// Return false when git fails.
[[nodiscard]] inline bool git_working_tree_dirty() noexcept {
    const auto output = slurp_command_stdout("git status --porcelain 2>/dev/null");
    return !output.empty();
}

/// Return Unix time in milliseconds for debug events and report timestamps.
/// Elapsed-time measurements use the separate monotonic clock.
[[nodiscard]] inline std::int64_t now_unix_millis() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

/// Capture the commit, working-tree  state, fast-mode flag and current time.
/// The report writer and regression checks share this run metadata.
[[nodiscard]] inline report_metadata capture_run_meta(bool fast_filter) noexcept {
    report_metadata meta;
    meta.commit = git_head_commit();
    meta.dirty  = git_working_tree_dirty();
    meta.fast   = fast_filter;
    // Reports store the timestamp in seconds.
    meta.timestamp = now_unix_millis() / 1000;
    // Format the creation time in UTC.
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

// Compare results with earlier reports.

/// Own a historical report's JSON text and parser for as long as its DOM
/// views are used. Copying and moving are disabled. The cache keeps each
/// report in a unique_ptr so its address stays stable.
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

/// Find the largest numeric index among report_<index>.json files.
/// Return nullopt when no matching file is found, including in a missing
/// report directory.
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
        // Skip oversized  indices without throwing an integeq-conversion error.
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

/// Find the largest index used by a report or raw JSONL file.
/// A crashed run can leave a raw file without its final report. Reserving
/// that index keeps a later run from reusing it
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

/// Load and parse report_<index>.json.
/// Return nullptr when it cannot be read, is empty or contains invalid JSON.
/// History checks then treat that report as unavailable.
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

/// Compare a historical JSON parameter with the current configuration value.
/// Numeric comparison accepts integer and  floating-point spellings, so 10
/// and 10.0 can match, after a report round trip.
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

/// Read a DOM element as an object, or return nullopt if it has another type.
[[nodiscard]] inline std::optional<simdjson::dom::object>
try_as_object(simdjson::dom::element element) noexcept {
    simdjson::dom::object out;
    if (element.get(out) != simdjson::SUCCESS) return std::nullopt;
    return out;
}

/// Read an object field as an object, or return nullopt if it is absent
/// or has another type.
[[nodiscard]] inline std::optional<simdjson::dom::object>
try_get_object(simdjson::dom::object parent, std::string_view key) noexcept {
    simdjson::dom::element child;
    if (parent.at_key(key).get(child) != simdjson::SUCCESS) return std::nullopt;
    return try_as_object(child);
}

/// Read meta.commit for history attribution.
/// Return an empty string if the path is missing or its value is not a string.
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

/// Read meta.fast, or return nullopt when the flag is missing or malformed.
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

/// Read a numeric field as a double, accepting integer spellings too.
/// Return default_value when  the field is missing or has another type.
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

/// Read an integer field as int64_t, conkerting unsigned integers if needed.
/// Return default_value when the field is missing or has another type.
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

/// Read a metric summary for display Missing or malformed fields keep
/// their zero or empty defaults. Regression baseline checks inspect the
/// original JSON separately so missing values cannot become measured zeros.
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
    /// Read numeric array entries and skip entries of other types.
    /// Return an empty vector when the field is missing or is not an  array.
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
        // Keep the best-value arrays the same length after dropping malformed
        // entries, so readers can safely access corresponding positions.
        const auto kbest_paired = std::min(agg.top10.size(), agg.bottom10.size());
        agg.top10.resize(kbest_paired);
        agg.bottom10.resize(kbest_paired);
    }
    simdjson::dom::array hist_arr;
    if (metric_obj["histogram"].get(hist_arr) == simdjson::SUCCESS) {
        // Preserve bucket positions. A malformed bucket leaves a zeroed slot
        // instead of shifting later buckets forward.
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

/// Find a historical metric by benchmark, algorithm parameters, test case
/// and region. Return its JSON object so regression checks can validate
/// stored fields before the display parser supplies defaults.
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

/// Find and, psrse a historical metric summary for  display.
/// Return nullopt if the metric cannot be found. Its fields use the defaults
/// from parse_aggregated_samples, so this alone does not validate a baseline.
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

/// Load historical reports on demand and keep their parsers alive.
/// Each index is read at most once per cache, including failed loads.
/// Regression and sanity checks can share the parsed reports.
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

    /// Fetch a cached report or load it once. Remember failed loads and
    /// return nullptr for them on later calls too
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

/// Read a nonnegative JSON integer as size_t.
/// Reject missing fields, floating-point spellings and out-of-range values.
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

/// Read  a required finite number, returning nullopt on invalid input.
/// Regression checks use this to keep malformed history from becoming a
/// zero-valued baseline.
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

/// Check whether a report's raw_evidence field permits use as history.
/// Null means no raw file was requested. An object must say complete: true
/// and contain a valid record_count. Return a reason when the check fails.
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

/// Require meta.history_eligible to be true before using a report as history.
/// Also require the rule_violations, regressions and sanity_warnings arrays
/// to exist. Their contents are represented by the producing run's flag.
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

/// Read the statistic slected  by a regression rule from historical JSON.
/// It must exist and be finite. Return nullopt instead of supplying a default.
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

/// Sum a historical metric's ten histogram counts.
/// Reject invalid counts or overflow. Baseline selection compares this
/// finite-sample count with the current result.
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

/// A historical result accepted for comparison. The cache owns report,
/// and aggregate_value holds the validated statistic selected by the, rule.
struct historical_regression_candidate {
    parsed_historical_report* report = nullptr;
    double aggregate_value = 0.0;
};

/// Result of checking one historical report. It contains an accepted
/// candidate or a  reason explaining why the report was skipped.
struct historical_regression_candidate_assessment {
    std::optional<historical_regression_candidate> candidate{};
    std::string reason{};
};

/// Find usable history for one current metric result
/// Baseline selection and regression attribution share these checks for run
/// status, fast mode, matching parameters and sample counts. Cache each
/// assessment so those searches use the same decision.
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

/// Return a double's exact bit pattern for distinct-value sets.
/// This distinguishes signed zeros and NaN payloads, while repeated copies
/// of the same NaN pattern occupy one entry.
[[nodiscard]] inline std::uint64_t double_bits(double sample) noexcept {
    return std::bit_cast<std::uint64_t>(sample);
}

/// Collect a metric's distinct finite extrema across  a historical report.
/// Equal extrema identify a constant cell. Cells with samples but an empty
/// top10 contribut one NaN marker, representing only non-finite results.
/// Empty or missing metrics contribute nothing. This lets sanity checks
/// detect constant values without storing the original sample arrays.
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

/// Format a constant value for a sanity warning.
/// Print small whole numbers as integers and name NaN and infinity explicitly.
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

/// Warn when a metric has one constant value across the run's cells.
/// Compare finite extrema and  use a shared NaN marker for cells containing
/// only non-finite samples. Empty cells are skipped.
///
/// Walk earlier reports to find how long the same value has appeared.
/// A missing report leaves the origin unknown. A changed or absent metric
/// stops the search. These warnings do not fail the run
[[nodiscard]] inline std::vector<sanity_warning> evaluate_sanity_warnings(
    const std::map<std::string, std::shared_ptr<benchmark_execution_state>>& reports,
    const report_metadata&            current_meta,
    std::size_t                  current_index,
    historical_report_cache&       history_cache) noexcept
{
    // Summarize each cell using the same finite extrema and non-finite
    // marker available in historical reports.
    std::map<std::string, std::set<std::uint64_t>> distinct_bits_by_metric;
    for (const auto& [_bench_key, state_ptr] : reports) {
        for (const auto& cell_ptr : state_ptr->cells) {
            for (const auto& [_region_name, region_metrics] : cell_ptr->samples) {
                for (const auto& [metric_name, sample_values] : region_metrics) {
                    if (sample_values.empty()) continue;
                    auto& bucket = distinct_bits_by_metric[metric_name];
                    const auto agg = aggregate_samples_for_report(sample_values);
                    if (agg.top10.empty()) {
                        // Use the same NaN marker as the history reader when the cell has
                        // no finite samples.
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
        // A metric, with no samples cannot be hecked for  constancy.
        if (distinct_bits.empty()) continue;
        // One distinct value means the metric appears constant.
        if (distinct_bits.size() != 1) continue;

        const std::uint64_t constant_bits = *distinct_bits.begin();
        const double constant_value = std::bit_cast<double>(constant_bits);
        const auto value_text = stringify_constant_value(constant_value);

        // Look for older reports with the same constant value. Stop when the
        // metric changes, disappears or a report cannot be loaded.
        std::string warning_since_label;
        std::string warning_since_commit;
        bool history_gap = false;

        if (current_index <= 1) {
            // With no history, record this first run as the warning's origin.
            warning_since_label  = "report_1.json";
            warning_since_commit = current_meta.commit;
        } else {
            // Start with the current report and move the origin back whenever an
            // older report confirms the same value.
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
                // An absent metric cannot confirm the warning in an older report.
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
                // Missing  history leaves warning_since unknown, written as JSON null.
            } else if (oldest_with_warning != current_index) {
                warning_since_label =
                    fmt::format("report_{}.json", oldest_with_warning);
                if (auto* origin_report = history_cache.fetch(oldest_with_warning);
                    origin_report != nullptr) {
                    warning_since_commit = commit_from_report(origin_report->root);
                }
            } else {
                // The current report is the first one that confirms this warning.
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

/// Compare current results with eligible historical baselines after work ends.
/// Return regressions and append diagnostics for missing data or skipped
/// history. Share the report cache with sanity checks to avoid repeated reads.
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

    // Merge each benchmark's groups once and reuse the samples across rules.
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

            // A CLI filter may remove all work cxvered by a rule Skip that rule
            // for a  partial run. If selected work still falls within its scope,
            // missing samples must still be reported.
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

                            // Count a matching metric even without a baseline. Skip comparison
                            // when its cell is marked unmeasured or the metric has
                            // no finite samples.
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

                            // Find the oldest eligible reference, that still makes this current
                            // value fail. Skip unavailable reports and stop at a
                            // reference against which the current value passes.
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
                                        break;     // The current value passes against this older  reference.
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

// Write JSON reports. simdjson::builder::string_builder leaves comma
// placement to these helpers and their callers.

/// Write a scalar configuration value. The caller writes any key and colon.
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
            // Validation should reject non-scalar parameters. Emit null here to
            // keep valid JSON while report validation still catches the error.
            builder.append_null();
            return;
    }
}

/// Write a quoted JSON key and colon.
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

/// Write a JSON key and let body_fn write its valuj.
template<typename p_body_function>
inline void emit_kv_with_body(simdjson::builder::string_builder& builder,
                                std::string_view key, p_body_function&& body_fn)
{
    emit_key(builder, key);
    body_fn(builder);
}

/// Write a JSON object with contents supplied by body_fn.
template<typename p_body_function>
inline void emit_object_with_body(simdjson::builder::string_builder& builder,
                                    p_body_function&& body_fn)
{
    builder.start_object();
    body_fn(builder);
    builder.end_object();
}

/// Call each_fn(builder, item) for every element, adding commas between
/// elements. The callback writes each field or array  value.
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

/// Write a double after converting non-finite values to JSON-safe numbers.
/// NaN becomes zero. Infinity becomes the corresponding finite limit
inline void emit_finite_double(simdjson::builder::string_builder& builder,
                                double value)
{
    builder.append(sanitize_for_json(value));
}

/// Write a numeric array, converting non-finite values with
/// emit_finite_double. Used for top10 and bottom10.
inline void emit_double_array(simdjson::builder::string_builder& builder,
                               const std::vector<double>& values)
{
    builder.start_array();
    emit_range_with_commas(builder, values, [](auto& array_builder, double sample) {
        emit_finite_double(array_builder, sample);
    });
    builder.end_array();
}

/// Summarize a nonempty sample vector and write its report object.
/// Include statistics, percentiles, best-value lists and the histogram.
/// Callers must skip empty vectors because reports require samples_count
/// to be at least one.
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

/// Write a  string field, using JSON null for an empty string.
/// Optional history labels and commit IDs use empty strings when unknown.
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

/// Write each region and metric from a test case's merged samples.
/// Skip empty metric vectors. The final report and --debug case results
/// share qhis writer so their test-case data has the same format.
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

/// Write an algorithm tuple and its merged results by test case.
/// Used by both the final report and --debug case results.
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
                    // Skip a test case whose merged region map is empty.
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

/// Write,  a present finite value, or null when it is unavailable.
/// Keep missing paired-rule inputs visible in the report.
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

/// Write one iteration's descriptor samples, using null for non-finite
/// values. Rule validation separately rejects those invalid inputs.
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

/// Write the algorithm, parameters and source fields for one side of a
/// paired comparison.
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

/// Write a recorded repeat number, order position and shuffle seed
/// Write null when absent. Hexadecimal seed strings preserve all 64 bits
/// in readers that cannot represent every  integer as a JSON number.
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

/// Write a paired-ratio comparison's inputs, schedule and result.
/// The final report and PAIRED_RULE_EVIDENCE debug events sharm this format.
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

// Write --debug events as JSON lines.

/// Append --debug events as JSON lines and flush each completed write.
/// A mutex keeps concurrent events from mixing bytes. Flushed events can
/// survive a process crash, though this stream does not fsync for power loss.
/// Event timestamps use Unix milliseconds.
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
        const auto merged = merge_case_samples(state, case_name);   // These cells have finished writing.
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

    /// Flush a paired-ratio comparison's record to the debug stream before
    /// the final report is  written.
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
        out_.flush();                 // Flush to the OS without fsync.
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

/// Decide whether a raw record may contribute timing samples.
/// For Bioseq E2E records, accept the current or previous payload version
/// and require a consistent accepted flag and disposition. Return nullopt
/// for invalid E2E status. Other payloads keep ordinary timing behavior.
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

/// Write raw sample records before adding their measurements to cells.
/// A mutex assigns their append order, and every line  is written and fsynced.
/// Final validation checks the file against the collected records and
/// schedule, bqfore marking it complete.
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
            // Mark complete only after rereading and validating the stored records.
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

        // Reopen the file and check its exact header, records and byte count
        // Extra bytes also make this incomplete.
        const bool prefix_valid = validate_authoritative_locked(
            embedded_by_id, manifest_, false);
        if (!valid || !prefix_valid) {
            manifest_["complete"] = false;
            (void)append_terminal_footer_locked(false);
            return false;
        }

        // After the stored records pass validation, append and fsync a complete
        // footer. Reopen the file once more to verify the footer too.
        manifest_["complete"] = true;
        if (!append_terminal_footer_locked(true)
            || !validate_authoritative_locked(
                embedded_by_id, manifest_, true)) {
            // If the footer write or final  read fails, try to end the file with
            // complete=false. Add a newline first if a partial write broke it.
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

    /// Append and fsync the final manifest using a fresh file descriptor.
    /// Bypass sample-write fault injection so a failed sample can still be
    /// followed by a complete=false footer when the file is writable.
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

/// Finish one cell and emit CASE_RESULTS when its algorithm's last cell ends.
/// The atomic decrement orders the cells' writes before the final thread
/// merges their samples. The emitted flag prevents a duplicate event.
inline void debug_finish_cell(benchmark_context& context,
                              benchmark_execution_state& state,
                              const std::string& case_name) {
    auto& cp = state.debug_progress_by_case.at(case_name);
    if (cp.remaining_cells.fetch_sub(1, std::memory_order_acq_rel) == 1
        && !cp.emitted.exchange(true, std::memory_order_acq_rel))
        context.debug_sink->emit_case_results(state.config.benchmark_key, case_name, state);
}

// Benchmark execution uses the report helpers above to write --debug
// results as each algorithm finishes.

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

/// Add framework  identity and timing fields to a raw sample payload.
/// Reject reserved fields supplied by the benchmark. For E2E records, check
/// timing eligibility and, when requested, subtract the CIGAR/MAPQ interval
/// unvon only after checking its clock rate and bounds.
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

    // One batch shares an input among all tuples in a test-case group.
    auto batches = build_work_batches(state);
    if (batches.empty()) return [] {};

    // Build the per-algorithm debug counters before any batch starts.
    // Workers then update only the atomic fields in existing map entries.
    if (context.debug_sink != nullptr) {
        for (const auto& cell_ptr : state.cells)
            state.debug_progress_by_case[cell_ptr->tuple->algorithm_name]
                .remaining_cells.fetch_add(1, std::memory_order_relaxed);
    }

    auto* scheduler = context.scheduler;
    DEBUG_ASSERT(scheduler != nullptr,
        "benchmark_context::scheduler must be set by run_impl");

    /// Cached input and resource estimate for one batch
    /// typed_run_state owns these slots until the returned finalizer joins
    /// the benchmark's work.
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

    /// Evaluate and record  one measured output.
    /// When repeat finalization is enabled, all outputs from that repeat are
    /// finalized together before any reaches this step. Raw-enabled runs
    /// append and fsync their record before updating the measurement cell.
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

        // For raw-enabled runs, record the cell's completion, only after its raw
        // line has been written and fsynced.
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

    /// Measure and record one iteration immediately.
    /// Optional boundary callbacks keep preparation and follow-up work outside
    /// the timed call. With nullptr boundaries, that branch is compiled out.
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

    /// Run  all iterations of one tuple in item-major order.
    /// The scheduler has already reserved memory and cheked concurrency.
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

    /// Keep the shared run state available to the batch callback.
    /// The scheduler calls it on a TBB worker while holding the batch's
    /// memory reservation.
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

        // Pass on_input the resource estimate used to reserve this batch.
        // Estimating again could give it a different budget, and the callback
        // is required to run only once per batch.

        // Retry input construction. If  every attempt fails, abort the batch.
        // Returning releases its scheduler reservation.
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

        // Interleaved runs execute each tuple once per repeat in a seeded
        // shuffled order This spreads algorithm positions across repeats
        // while keeping the order reproducible in the raw records.
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
            // Emit CASE_STARTED once per algorithm before running the batch items.
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

        // Retry cleanup and warn if every attempt fails.
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

    // Estimate each batch's resources and enqueue it. The scheduler starts
    // its body when memory and concurrency rules permit.
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
        // Reject an overflowing memory request before converting it to  the
        // scheduler's signed accounting. Keep a saturated total for the error
        // message and reuse the checked toal throughout scheduling.
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
            continue;       // Reject before entering signed scheduler accounting.
        }

        // Reject requests at least as large as the machine's total RAM.
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
        // Warn when the request reaches or exceeds the buffered memory cap.
        if (total_required >= cap_at_enqueue) {
            emit_runtime_diagnostic(context, validation_diagnostic::diagnostic_severity::warning,
                "memory_budget_oversized", location_str,
                "batch '{}' (group {}) requires {} B but buffered cap is {} B "
                "(host total {} B, buffer {:.0f}%)",
                spec.name, batch.group_index,
                total_required, cap_at_enqueue,
                sys_at_enqueue.system_total_bytes, scheduler->memory_buffer_pct());
            // Leave admission to the scheduler. A request that never fits will
            // eventually time out.
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

    // Return a finalizer that joins this benchmark on the caller's thread.
    // Preparation, can then enqueue other benchmarks before  waiting, allowing
    // the scheduler to consider their batches together. Keep captures alive
    // in run_state until the join finishes.
    const std::size_t benchmark_index = context.benchmark_index;
    return [scheduler, benchmark_index,
            context_ptr = &context, state_ptr = &state,
            run_state] {
        scheduler->wait_for_benchmark(benchmark_index);

        // Report any algorithm that started but could not finish all cells.
        // All batches have ended, so their partial samples can now be read
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

/// Write the run's measurements, metadata and findings to output_path.
/// Return that path on success, or an empty string for detected serialization
/// or stream errors. run_impl chooses the report index so history checks
/// and writing refer to the same run.
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

    /// Write run metadata, including whether it can  serve as history.
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

    /// Write a rule violation with its tuple information and compared values.
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
                    // Write one property per algorithm name. When both sides name the
                    // same algorithm, keep the leqt-hand parameter map so the
                    // object has no duplicate keys.
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

    /// Write a regression's current and previous values with the affected
    /// tuple, test case and historical reference.
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

    /// Group-merged samples cached for each benchmark while writing the report.
    std::map<const benchmark_execution_state*, merged_samples_by_pair> merged_cache;
    auto merged_for_writer = [&](const benchmark_execution_state& state)
        -> const merged_samples_by_pair&
    {
        auto found = merged_cache.find(&state);
        if (found != merged_cache.end()) return found->second;
        return merged_cache.emplace(&state, merged_samples_for_state(state))
            .first->second;
    };

    // Share test-case and tuple writers with --debug case results.

    /// Write one benchmark's descriptions and tuple results, including raw
    /// samples when enabled.
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

    /// Write a  sanity warning and its historical origin. Unknown report or
    /// commit fields become JSON null.
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
        // Put the commit and run time near the start of the report.
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

// Print findings to stderr.

/// Append a configuration value as plain text for labels and identity keys.
/// Whole-number doubles below 2^53 in magnitude use integer spelling, so
/// values such as 5 and 5.0 keep the same text after a report round trip
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

/// Format parameters as k=v pairs for stderr output.
/// This text is for display. emit_param_object writes the JSON form.
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

/// Print the final rwle-violation  summary directly, to stderr.
/// Direct stdio output remains available after the asynchronous logger stops.
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
        // Share one label when both sides have the same algorithm name and
        // displayed parameter map.
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

/// Print the report and commit identifying a finding's origin.
/// An empty report means incomplete history. An empty commit is shown as
/// unknown when the report is known.
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

/// Print the final regression  summary directly to stderr.
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

/// Print the final sanity warnings directly to stderr, with their history.
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

// --summary reads existing reports and combines repeated findings.
// Scores account for issue type, recurrence, value changes and sample
// spread. The dashboard groups issues by metric, scope and test case.

/// One finding combined across historical reports for the summary dashboard.
/// Store its presence history and latest measurements for scoring and display.
struct summary_issue {
    enum class issue_kind : std::uint8_t {
        regression,
        rule_violation,
        sanity_warning,
    };

    issue_kind kind = issue_kind::sanity_warning;
    /// Key used to combine the same issue across reports.
    std::string                   identity{};
    /// Metric and scope  used to group dashbord rows.
    std::string                   metric{};
    std::string                   scope_label{};
    std::string                   test_case{};   // "" for sanity warnings
    std::string                   benchmark{};   // "" for sanity warnings
    std::string                   rule_name{};   // Warning type for sanity warnings.
    /// Values from the latest report containing this issue.
    /// Rule violations store [lhs, rhs], regressions store [previously, now],
    /// and sanity warnings leave these unused
    std::array<double, 2>         latest_args{0.0, 0.0};
    /// Latest sample summary used to score the spread of values.
    /// Empty for sanity warnings or when the affected result is missing.
    aggregated_samples             latest_samples{};
    /// One byte per report index, with 1 when the issue appears and 0 otherwise.
    /// A byte vector can be passed directly as a span for recurrence scoring.
    std::vector<std::uint8_t>     presence_per_report{};
    /// Mark  which report slots were readable, so missing history can be
    /// displayed separately from an absent issue.
    std::vector<std::uint8_t>     report_data_present{};
    /// Most recent report containing this issue. Controls updates to, the
    /// displayed values and sample summary.
    std::size_t                   latest_seen_index = 0;
    /// Latest sanity-warning message. Empty for other issue types.
    std::string                   sanity_message{};
    /// Score computed after collecting the issue's history.
    double                        score = 0.0;
};

/// Display style for a range of issue scores.
/// Keep the registry in descending min_score order so the first match wins.
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

/// Choose the highest  seveity threshold reached by score.
/// Fall back to the lowest bucket if none matches.
[[nodiscard]] inline const severity_bucket& bucket_for(double score) noexcept {
    for (const auto& bucket : k_severity_buckets) {
        if (score >= bucket.min_score) return bucket;
    }
    return k_severity_buckets.back();
}

/// Choose a report-history marker for this issue.
/// An unreadable report gets the missing-data marker before issue presence
/// is considered.
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

/// Compute a 64-bit FNV-1a hash for a sanity-warning message.
/// Summary keys include this hash to distinguish messages on the same metric
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

// ASCII record and unit separators divide summary identity fields.
// Field contents are appended without escaping these separators.
inline constexpr char k_summary_identity_rs = '\x1e';
inline constexpr char k_summary_identity_us = '\x1f';

/// Format parameters in name order, separating k=v entries with the
/// summary  unit separator.
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

/// Join an issue kind and its identifying fields.
/// Use the record separator after kind and unit separators between fields.
/// Each field must be convertible to string_view.
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

/// Identify a rule violation by its configuration, selectors and source.
/// Include region and aggregate fields, defaulting to all_regions and p50.
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
    // Store the message's hash and length in the key to keep it compact.
    auto hash_text = fmt::format("{:016x}-{}", fnv1a64(message), message.size());
    return join_identity("sanity", type, metric, std::string_view{hash_text});
}

// Index report summaries by their source fields.

/// Read a JSON parameter object intv a parameter_map.
/// Shared by issue parsing and report sample indexing.
[[nodiscard]] inline parameter_map parse_param_map(simdjson::dom::object obj) noexcept {
    parameter_map params;
    for (auto field : obj) {
        params.emplace(std::string{field.key},
                       config_value::from_simdjson(field.value));
    }
    return params;
}

/// Sample summaries indexed  by benchmark, algorithm parameters, test case,
/// region and metric. Summary scoring uses this to find an issue's samples.
using sample_index = std::map<std::string, aggregated_samples>;

/// Build the lookup key for one result's sample summary.
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

/// Index every metric summary in a report for later, issue lookups.
/// Parse each result once so scoring several issues can reuse it.
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
            // A report tuple maps its algorithm name to its parameters.
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

/// Find a result's summary in the report index.
/// Return an empty summary  when absent. The scoring code treats that as
/// missing sample-spread information
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

// Read issue entries from reports.

/// Read a string field, returning an empty string when absent or malformed.
[[nodiscard]] inline std::string get_string_field(
    simdjson::dom::object obj, std::string_view key) noexcept
{
    std::string_view text;
    if (obj.at_key(key).get(text) == simdjson::SUCCESS) return std::string{text};
    return {};
}

/// Read a numeric field as a double, or return zero when unavailable.
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

/// Parsed issue identity and callbacks for merging it into the summary.
/// One callback initializes a new issue. The other updates values when a
/// newer report contains it.
struct extracted_issue {
    std::string                          identity;
    summary_issue::issue_kind                   kind = summary_issue::issue_kind::sanity_warning;
    /// Set fields shared by all reports of this issue when it is finst added.
    std::function<void(summary_issue&)>   update_first;
    /// Update values and samples when this is the newest report of the  issue.
    std::function<void(summary_issue&)>   update_latest;
};

/// Read one report's issue array and merge entries by identity.
/// extract supplies the parsing and update callbacks for each issue type.
/// Skip entries it cannot parse and record which report contains each issue.
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

        // Insert a new issue or reuse its existing entry. Read the stored key
        // after moving the extracted identity into the map.
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

/// Read the first object in violating_tuples as parameters.
/// Return an empty map if the array is missing or has no object entries.
[[nodiscard]] inline parameter_map parse_violating_tuple_first(simdjson::dom::object obj) noexcept {
    simdjson::dom::array tuples_array;
    if (obj["violating_tuples"].get(tuples_array) != simdjson::SUCCESS) return {};
    for (auto tuple_elem : tuples_array) {
        const auto params_obj = try_as_object(tuple_elem);
        if (!params_obj.has_value()) continue;
        return parse_param_map(*params_obj);     // The  framework writes one parameter object per regression.
    }
    return {};
}

/// Read the first string in test_cases, or an empty string if none is found
[[nodiscard]] inline std::string parse_test_case_first(simdjson::dom::object obj) noexcept {
    simdjson::dom::array tc_array;
    if (obj["test_cases"].get(tc_array) != simdjson::SUCCESS) return {};
    for (auto tc_elem : tc_array) {
        std::string_view tc_text;
        if (tc_elem.get(tc_text) == simdjson::SUCCESS) return std::string{tc_text};
    }
    return {};
}

/// Read up to two comparison arguments from a, rule violation.
/// Missing or nonnumeric positions stay zero. Ignore extra array entries.
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

/// Read the algorithm and parameters for each stored side of a violation.
/// Valid reports have one entry for a shared algorithm name or two for
/// different algorithms.
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

// Score historical issues for the summary dashyoard.

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

/// Weight  an issue by how recently and repeatedly it appears.
/// Newly appearing or returning issues get more weight. Long continuous
/// streaks and issues absent from the latest report get less.
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

/// Express the absolute difference as a percentage of the baseline's
/// magnitude. Clamp a near-zero denominator so scoring and display can
/// handle a zero baseline.
[[nodiscard]] inline double relative_diff_pct(double current, double baseline) noexcept {
    constexpr double k_eps = 1e-12;
    return std::abs(current - baseline) / std::max(std::abs(baseline), k_eps) * 100.0;
}

/// Express a summary's min-to-max spread as a percentage of its minimum's
/// magnitude. Return zero for fewer than two collected samples or  no finite
/// samples. Clamp the denominator when the minimum is near zero.
[[nodiscard]] inline double relative_amplitude_pct(
    const aggregated_samples& aggregated) noexcept
{
    if (aggregated.samples_count < 2) return 0.0;
    if (aggregated.top10.empty())     return 0.0;     // all-non-finite cell
    constexpr double k_eps = 1e-12;
    return 100.0 * (aggregated.stats.max - aggregated.stats.min)
                 / std::max(std::abs(aggregated.stats.min), k_eps);
}

/// Increase an issue's score with the relative size of its value change.
/// Logarithmic growth and a cap keep large differences from dominating.
/// A near-zero baseline uses the maximum factor
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

/// Reduce the score when samples have a large spread, since a difference
/// may reflect measurement variability. With no spread information, keep
/// the score unchanged.
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

/// Whether an issue appears in the latest report, its  current streak length,
/// and whether it appeared before an earlier gap.
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

/// Combine issue severity and recurrence int, a summary score.
/// Rule violations and regressions also account for value differences and
/// sample spread. Sanity warnings have no numeric comparison to score.
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

// Render the summary dashboard.

/// Sort issues by metric and scope, then highest score first within each
/// group. Use identity as the final tie-breaker.
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

/// Shorten  valid UTF-8 text to a byte budget, adding an ellipsis when cut.
/// Back up to a character boundary before adding the ellipsis. Very small
/// budgets use dots. Terminal column widths are not measured here.
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
        // The first character and ellipsis do not fit. Fill the byte budget
        // with dots.
        return std::string(max_chars, '.');
    }
    text.resize(cut);
    text += k_ellipsis;
    return text;
}

/// Format an issue's compared values and sample spread for its dashboard row.
/// Sanity warnings show a shortened first sentence of their message.
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

/// Maximum number of recent reports shown in a summary row's history
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
        // Show missing data  if the report-status array has no matching slot.
        const bool data_ok = i < issue.report_data_present.size()
            && issue.report_data_present[i] != 0;
        const auto glyph = summary_block_for(issue.kind, present, data_ok);
        blocks.push_back(text(std::string{glyph}));
    }
    return blocks;
}

/// Metric and scope used to group summary rows.
using summary_group_key = std::pair<std::string, std::string>;

/// Render one issue's severity marker, satistics and recent report history.
/// Show its rule name on the right when available.
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
        // Limit the rule name so the history markers keep space on the row.
        constexpr std::size_t k_rule_name_max_chars = 24;
        row.push_back(text(clip_to_width(issue.rule_name, k_rule_name_max_chars)) | dim);
    }
    return hbox(std::move(row));
}

/// Render issue rows grouped by metric and scope, followed by the legend.
/// Write directly to stderr so the summary  remains available after the
/// asynchronous logger stops.
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

// Read historical findings for the summary view.

/// Build the rule-violation parser used by walk_report_array.
/// The referenced sample index must stay alive throughout the report walk.
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
        // A shared algorithm name is stored once in execution_tuple. Reuse
        // that entry for both sides because the report has no second map.
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
        // Sample-spread scoring uses the left-hand, result.
        ex.update_latest = [args, benchmark,
                            lhs_algo = std::move(lhs.first),
                            lhs_params = std::move(lhs.second),
                            test_case, metric, &samples_by_path]
                           (summary_issue& issue) noexcept {
            issue.latest_args = args;
            // Use the  left-hand all_regions samples for spread scoring.
            // The violation record does not store a region for this lookup.
            issue.latest_samples = samples_from_index(
                samples_by_path, benchmark, lhs_algo, lhs_params,
                test_case, "all_regions", metric);
        };
        return ex;
    };
}

/// Build the regression parser used by walk_report_array.
/// The stored region identifies both the issue and its sample summary
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
            // Include a nondefault aggregate in the label so p99 and p50 findings
            // can be distinguished. Omit the p50 suffux to keep labels short.
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

/// Build the sanity-warning parser used by walk_report_array.
/// These warnings carry a message and need no per-cell sample lookup.
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

/// Read existing reports, combine repeated issues and print one  summary.
/// Used when the interactive dashboard cannot run on the available terminal.
/// Missing or unreadable reports remain gaps in the history.
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
        if (report == nullptr) continue;     // Keep unreadable reports marked as missing.
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

    // Give each issue the same report-availability history for rendering.
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


/// Open the interactive report viewer implemented in interactive.cppm.
/// Its startup checks choose a one-time stderr summary when the terminal
/// cannot support the viewer.
[[nodiscard]] int ui_mode_main(const std::string& report_dir, const parsed_cli& cli) noexcept;

// Parse command-line options.

/// Read a filter such as tags=a,b into its named field.
/// Record errors for  unknown fields, repeated fields and empty values.
/// Errors use code:message strings for later diagnostics.
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
    // Set the requested filter field once.
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

/// Read framework options from argv and ignore unknown flags.
/// Call this efore setup so --report selects the output directory before
/// any files or directories are created.
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
            // Parse the percentage without throwing on, invalid input.
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

// Show live benchmark progress.

/// A progress threshold and its gauge color
/// Keep thresholds in ascending order so gauge_palette selects the highest
/// threshold reached.
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

/// Convert the selected  gauge palette value to an FTXUI color.
[[nodiscard]] inline ftxui::Color gauge_color(float fraction) noexcept {
    return ftxui::Color(gauge_palette(fraction));
}

/// Draw a fixed-size progress bar, clamping the fraction to [0, 1].
/// Round the number of filled cells and keep empty cells visible so the
/// bar stays the same width throughout the run.
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

/// Convert a byte count to GiB.
[[nodiscard]] inline constexpr double bytes_to_gib(std::uint64_t bytes) noexcept {
    constexpr double k_gib = 1024.0 * 1024.0 * 1024.0;
    return static_cast<double>(bytes) / k_gib;
}

// Live progress refreshes the terminal every 200 ms. Without a TTY, it
// logs unfinished, started test cases about every five seconds.

/// Display states for a test case, chosen from the states of its groups.
enum class test_case_row_state : std::uint8_t {
    done,
    aborted,
    input_retry,
    cleanup_retry,
    idle_wait,
    running,
};

/// Choose the test-case state to  display, giving failures priority.
/// Aborts come first, then input retries and cleanup retries. A finished
/// case stays done even if an idle timer remains set.
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

/// Header symbol and color for a progrefs-row state.
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

/// Build group details for an aborted, retrying or waiting test case.
/// Running and completed cases need no detail rows.
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

        // Select details for this group's displayed state.
        std::string header_line, message, trace;
        Color tint = Color::GrayLight;

        // Check retry counters again after taking failure_mu. A worker may
        // have cleared the row while we waited, and cleared text must not
        // appear beside an  old attempt number
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
            continue;     // This group has no details for the selected state.
        }

        // Indent the group header by four spaces and its message and trace
        // by eight, so the details sit below their group.
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
            // Keep each trace line and its indent in one text element. Separate
            // hbox children can shrink the indent when a long C++ name reaches
            // the screen, edge. clip_trace supplies plain text without ANSI
            // colors, and FTXUI applies the color here.
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

/// Keep completed rows visible for five seconds before hiding them.
/// Aborts and retry failures remain  visible.
inline constexpr std::chrono::seconds k_completed_row_grace_period{5};

/// Check whether a completed row has passed its display grace period.
/// Used for individual rows and for collapsing a finished benchmark.
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

        // Show memory use and reservations from the scheduler's live snapshot.
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
            // Collapse a benchmark once jvery test-case row can be hidden.
            // Keep the normal header when it has no test cases.
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

                // Show the test case's status and progress.
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

                // Add details for failures and long waits.
                auto details = build_detail_rows(*tc, state);
                for (auto& row : details) rows.push_back(std::move(row));

                // Show an iteration gauge for each active group while running.
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

        // Clear rows left  below a shorter frame before repainting. Resetting
        // within the new frame alone would leave old rows on screen
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
    render_once();   // Paint the final state, including failures.
    std::fputs("\n", stderr);
    std::fflush(stderr);
}

// Run the selected benchmarks and write their report.

/// Benchmark declarations needed before execution.
/// CLI selection uses tags and algorithm names. Regression validation keeps
/// the full declaration list even when filters remove some work.
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
    bool                     parsed_ok = false;  // The file or benchmark object could not be read.
};

/// Read names and flags needed for CLI selection and regression checks.
/// parsed_ok becomes true once  the file and benchmark object can be read.
/// This pass tolerates missing or malformed fields. Full configuration
/// parsing remains responsible for reporting those errors.
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

/// Run the CLI-selected benchmarks, collect checks and write a report.
/// Own, the shared scheduler, progress thread and run state until work ends.
/// UI mode opens the report viwer instead.
///
/// Return zero when execution, blocking comparisons and report validation
/// succeed. Warnings and nonblocking comparison failures are still reported.
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

    // Choose one index for the report and its  raw and debug files before
    // execution starts. Count raw files left by crashed runs as used indices.
    // A directory with no run artifacts starts at one.
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

    // Share the same commit and start-time metadata between logs and reports.
    const auto run_meta = capture_run_meta(cli.fast_filter);
    bioseq_log<logging_level::info>("[bench_framework] commit={} dirty={} created={} fast={}",
         run_meta.commit.empty() ? std::string_view{"<no-git>"}
                                 : std::string_view{run_meta.commit},
         run_meta.dirty, run_meta.created, run_meta.fast);

    // Take ownership of the benchmark list. --fast is checked after each
    // benchmark parses its configuration.
    std::vector<erased_benchmark> selected = std::move(benchmarks);

    // Benchmark names must be unique across registrations.
    std::set<std::string> seen_keys;
    for (const auto& erased : selected) {
        const auto inserted = seen_keys.insert(erased.benchmark_key);
        if (!inserted.second) {
            bioseq_log<logging_level::error>("[bench_framework] duplicate benchmark_key '{}' across create_benchmark calls",
                         erased.benchmark_key);
            return 1;
        }
    }

    // Read declarations before filtering Regression rules must resolve
    // names against all registered benchmarks, including work excluded by
    // --filter or --fast. Full parsing later checks configuration validity.
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

    // Validate  and apply CLI filters before starting progress or scheduler
    // work. Removed benchmarks need no dashboard rows or report entries.
    const filter_specification& f = cli.filter;
    std::vector<validation_diagnostic> filter_diags;

    // Report CLI parsing errors even when no valid filter was supplied.
    for (const auto& raw : cli.cli_errors) {
        const auto colon = raw.find(':');
        filter_diags.push_back({ validation_diagnostic::diagnostic_severity::error,
            std::string{raw.substr(0, colon)}, "cli:--filter",
            std::string{raw.substr(colon + 1)} });
    }
    // Debug event logging requires a filter to keep the run targeted.
    if (cli.debug && f.empty()) {
        filter_diags.push_back({ validation_diagnostic::diagnostic_severity::error,
            "debug_requires_filter", "cli:--debug",
            "--debug needs a non-empty --filter: it is for targeted runs, not for "
            "instrumenting an entire benchmark sweep. Add e.g. --filter benchmark=<key>." });
    }

    // Pass the algorithm allow-list to eact benchmark. nullopt keeps all cases.
    std::optional<std::set<std::string>> case_allow_list;
    if (f.cases) case_allow_list.emplace(f.cases->begin(), f.cases->end());

    // Apply selection only when filters are present. Reuse the declarations
    // already read, and keep all benchmarks by default.
    std::vector<char> runnable(selected.size(), 1);
    if (!f.empty()) {
        const auto& meta = registered_metadata;

        // Apply tag and benchmark-name filters together.
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
        // Also require a matching algorithm  when a cases filter is present.
        for (std::size_t i = 0; i < selected.size(); ++i) {
            if (!keep[i]) { runnable[i] = 0; continue; }
            if (!f.cases) { runnable[i] = 1; continue; }
            runnable[i] = std::any_of(meta[i].case_names.begin(), meta[i].case_names.end(),
                [&f](const std::string& c){ return std::find(f.cases->begin(), f.cases->end(), c) != f.cases->end(); });
        }

        // Check tag and benchmark names against all registered declarations.
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

        // Report a conflict when --fast excludes an explicitly named benchmark.
        // A tag-only selection may still skip benchmarks marked fast=false.
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

        // Complete-matrix acceptance requires every declared algorithm row.
        // Reject a cases filter that removes a row from a selected benchmark.
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
        // Check case names within benchmarks kept by, the tag and name filters.
        if (f.cases) for (const auto& c : *f.cases) {
            bool known = false;
            for (std::size_t i = 0; i < selected.size() && !known; ++i)
                if (keep[i]) known = std::find(meta[i].case_names.begin(),
                                               meta[i].case_names.end(), c) != meta[i].case_names.end();
            if (!known) filter_diags.push_back({ validation_diagnostic::diagnostic_severity::warning,
                "filter_unknown_case", "cli:--filter cases",
                fmt::format("case '{}' is an over-key of no tag/benchmark-selected benchmark", c) });
        }

        // A filter selecting no runnable work is an error.
        if (std::count(runnable.begin(), runnable.end(), 1) == 0)
            filter_diags.push_back({ validation_diagnostic::diagnostic_severity::error,
                "filter_selected_nothing", "cli:--filter",
                "the supplied --filter matched no runnable (benchmark, case) combination" });
    }

    // Remember whether selection failed before moving its diagnostics  into
    // the shared list.
    const bool filter_had_error = std::any_of(filter_diags.begin(), filter_diags.end(),
        [](const auto& d){ return d.severity == validation_diagnostic::diagnostic_severity::error; });
    {
        std::lock_guard<std::mutex> guard(diagnostics_mu);
        for (auto& d : filter_diags) diagnostics.push_back(std::move(d));
    }
    if (filter_had_error) {
        report_diagnostics(std::span<const validation_diagnostic>{diagnostics}, "filter");
        return 1;     // Stop before progress and scheduler work starts.
    }

    // Keep only the selected benchmarks.
    if (!f.empty()) {
        std::vector<erased_benchmark> pruned;
        for (std::size_t i = 0; i < selected.size(); ++i)
            if (runnable[i]) pruned.push_back(std::move(selected[i]));
        selected = std::move(pruned);
    }

    // Own the debug event stream for the run Each benchmark context borrows
    // its pointer while execution is active.
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

    // All benchmarks in this run share one memory budget.
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

    // Create the shared batch scheduler before the progress thread so the
    // kashboard can read its memory and queue state.
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

    // Prepare and enqueue every benchmark before joining any of them.
    // Joining on the caller's thread leaves TBB workers available for batch
    // bodies, even  when a nested concurrency limit allows only one worker.
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

    // Join the scheduler and collect worker exceptions. Stop the progress
    // thread before reporting the first failure as a blocking diagnostic.
    // Completed samples can still be written in a partial report.
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

    // Validate registered regression rules against the full declaration list,
    // including benchmarks excluded from this run.
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

    // Report collected configuration and execution errors. Any error makes
    // the run fail even when its comparison rules pass.
    const bool diagnostics_had_errors = report_diagnostics(
        std::span<const validation_diagnostic>{diagnostics}, "configuration");

    // Use the run index already  assigned to its debug and raw files.
    bioseq_log<logging_level::info>("[bench_framework] writing run as report_{}.json", current_index);

    // Share parsed history between regression and sanity checks.
    historical_report_cache history_cache{report_dir_path};

    // Compare against the newest usable historical result. Use the same
    // eligibility checks when finding the regression's earlier references.
    // Collect findings for the report and print evaluation diageostics
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
    // Write history_eligible=false first. Only a validated report from a run
    // with no blocking failures can later replace it with a true value.
    final_run_meta.history_eligible = false;

    // Report suspicious constant metrics without changing the exit code.
    auto sanity_warnings = evaluate_sanity_warnings(
        reports, run_meta, current_index, history_cache);

    bioseq_log<logging_level::info>("[bench_framework] all benchmarks finished, writing report...");
    const auto output_path = report_path_for_index(report_dir_path, current_index);
    std::string report_path = write_report(
        output_path, final_run_meta, reports, violations, regressions,
        sanity_warnings, raw_evidence_manifest);
    bool report_write_failed = report_path.empty();

    // Validate the report  before allowing it to serve as, history.
    // history_eligible remains false while this check runs.
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

    // Write the eligible version to a temporary file and validate it before
    // renaming it over the report. A failed write or validation leaves the
    // existing history_eligible=false report in place.
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

    // Finish the debug stream with the report path, or null if no report
    // was written. This path can still name a report that failed validation.
    if (debug_sink_ptr != nullptr) {
        debug_sink_ptr->emit_execution_finished(report_path);
    }

    print_violations(violations);
    print_regressions(regressions);
    print_sanity_warnings(sanity_warnings);

    // Sanity warnings and nonblocking comparisons remain visible in the
    // summaries above without making the run fail.
    return (!blocking_rule_violation
            && !blocking_regression
            && !diagnostics_had_errors
            && !regression_eval_had_errors
            && !report_write_failed
            && !report_schema_failed) ? 0 : 1;
}

}  // namespace detail

// Construct the benchmark callbacks used by the driver.

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

        // Publish  setup completion even when configuration checks return early.
        // The renderer can then treat the benchmark's progress structure as
        // rady, including an empty structure for a skipped benchmark.
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

        // Read the configuration and collect schema and parsing errors.
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

        // Register one set of regression rules per configuration file.
        // Benchmarks sharing the file reuse the first registered copy.
        if (context.regression_rules_mu != nullptr
            && context.regression_rules_by_config != nullptr) {
            std::lock_guard<std::mutex> guard(*context.regression_rules_mu);
            auto& registry = *context.regression_rules_by_config;
            if (registry.find(erased_config_path) == registry.end()) {
                registry.emplace(erased_config_path,
                                 std::move(parse_outcome.regression_rules));
            }
        }

        // Check configuration references, parameter types and rule settings.
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

        // In fast mode, skip benchmarks that declare fast=false.
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
        // Keep algorithm tuples named by --filter cases=.
        if (context.filter_cases != nullptr) {
            std::erase_if(state.execution_tuples,
                [&context](const detail::execution_tuple& t){
                    return !context.filter_cases->contains(t.algorithm_name);
                });
            if (state.execution_tuples.empty()) {
                // The configuration  may have changed since selection Skip if the
                // selected algorithms are no longer present.
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

        // Each group runs one tuple at a time against its shared input.

        // Create progress rows in test-case order, with a child row for each
        // iteration group.
        if (context.progress != nullptr
            && context.benchmark_index < context.progress->per_benchmark.size()) {
            auto& per = *context.progress->per_benchmark[context.benchmark_index];
            per.test_cases.clear();
            per.test_cases.reserve(state.test_cases.size());
            for (std::size_t tc_index = 0; tc_index < state.test_cases.size(); ++tc_index) {
                auto tc = std::make_unique<detail::progress_test_case>();
                tc->test_case_name = state.test_cases[tc_index].name;
                // Count this test case's cells and each group's planned iterations.
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
                    // Mark test cases with no cells as done immediately. They can then
                    // disappear after the normal display grace period.
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
            // Publish the progress structure before enqueueing work. Workers must
            // see setup_done so their progress updates are recorded.
            per.setup_done.store(true, std::memory_order_release);
        }

        // The scheduler enforces exclusive  and, serial execution. Preparation
        // enqueues work and returns a finalizer for the caller to jodn later.

        // Emit BENCHMARK_STARTED before any case event for a benchmark with work.
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

            // Check collected metrics and complete-matrix requirements.
            auto post_run_diagnostics =
                detail::semantic_validate_post_run(final_state);
            detail::drain_diagnostics_to_context(
                final_context, post_run_diagnostics);

            // Evaluate comparison rules and save the results for report writing.
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
