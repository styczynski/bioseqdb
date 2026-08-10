/**
 * @file bench_framework/example/index.cppm
 * @brief Demonstration of the benchmark framework.
 *
 * Three toy benchmarks driven by `example_bench_configuration.json`:
 *
 *   1. **bench_sort** — measures sorting performance for two algorithms
 *      (`std_sort`, `std_stable_sort`). One built-in `wall_clock_ns`
 *      metric, one rule comparing the two. Each iteration sleeps a few
 *      milliseconds so the demo runs ≥ 15 s of wall-clock time and a
 *      human can watch the dashboard.
 *
 *   2. **bench_count** — counts even integers in a vector. One
 *      `evaluation` metric (the count) and one rule asserting the
 *      computed count matches the known ground truth. Includes a
 *      "BUGGY_INPUT" test case that throws from `on_input` on the
 *      first 2 attempts so the operator can see the retry UI.
 *
 *   3. **bench_buggy_cleanup** — every `on_cleanup` call throws.
 *      Demonstrates the cleanup-retry UI and the
 *      `cleanup_function_failing` warning.
 */
module;

#include <simdjson.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

export module bioseq.bench.framework.example;

import bioseq.common.logging;
import bioseq.instrumentation;
import bioseq.bench.framework;

#ifndef BIOSEQ_BENCH_FRAMEWORK_EXAMPLE_REPORT_DIR
#define BIOSEQ_BENCH_FRAMEWORK_EXAMPLE_REPORT_DIR "./reporting/bench_framework_example"
#endif
#ifndef BIOSEQ_BENCH_FRAMEWORK_EXAMPLE_DATA_DIR
#define BIOSEQ_BENCH_FRAMEWORK_EXAMPLE_DATA_DIR "."
#endif

namespace {

using bioseq::bench::framework::parameter_map;
using bioseq::bench::framework::run_constraints;

// -------- bench_sort I/O types --------

struct sort_input {
    std::vector<std::int64_t> values;
};

struct sort_output {
    std::int64_t first_value = 0;
    std::int64_t last_value  = 0;
    std::size_t  size        = 0;
};

[[nodiscard]] run_constraints estimate_sort_resources(
    std::string_view /*data_dir*/,
    std::string_view /*test_case_name*/,
    simdjson::dom::element data_element)
{
    std::int64_t length = 0;
    (void)data_element["length"].get(length);
    const auto values_bytes = (length <= 0)
        ? std::size_t{0}
        : static_cast<std::size_t>(length) * sizeof(std::int64_t);
    // base: sort_input.values lives until cleanup.
    // extra: the working_copy in run_sort_algorithm doubles allocation
    // for the duration of the iteration loop only.
    return { .base_memory = values_bytes, .extra_memory = values_bytes };
}

[[nodiscard]] sort_input build_sort_input(std::string_view /*data_dir*/,
                                          std::string_view test_case_name,
                                          simdjson::dom::element data_element,
                                          const run_constraints& /*constraints*/)
{
    sort_input input;
    std::int64_t length = 0;
    if (data_element["length"].get(length) != simdjson::SUCCESS || length <= 0) {
        bioseq_log<logging_level::error>("[example] sort test case '{}' missing 'length'", test_case_name);
        return input;
    }
    std::int64_t random_seed = 42;
    (void)data_element["seed"].get(random_seed);

    std::mt19937_64 rng{static_cast<std::uint64_t>(random_seed)};
    input.values.resize(static_cast<std::size_t>(length));
    std::uniform_int_distribution<std::int64_t> dist(-1'000'000, 1'000'000);
    for (auto& value : input.values) value = dist(rng);
    return input;
}

[[nodiscard]] sort_output run_sort_algorithm(std::string_view algorithm_name,
                                              const parameter_map& /*params*/,
                                              const sort_input& input)
{
    // One static region descriptor per algorithm so the framework's
    // by_regions split has something to attribute samples to.
    static const auto* const sort_region =
        bioseq::instrumentation::register_static_region("sort_run");
    static const auto* const copy_region =
        bioseq::instrumentation::register_static_region("sort_copy_input");

    std::vector<std::int64_t> working_copy;
    {
        auto _copy_scope = bioseq::instrumentation::region(copy_region);
        working_copy = input.values;
    }
    {
        auto _sort_scope = bioseq::instrumentation::region(sort_region);
        // Sleep ~250 ms per iteration so the dashboard is visible
        // for ≥ 15 s end-to-end. The sleep lives inside the timing window
        // intentionally so the rule `sort_within_2x_of_stable_sort`
        // (tolerance 99%) keeps passing — both algorithms incur the same
        // delay so their p50s converge.
        for (size_t dummy_iteration = 0; dummy_iteration < 250; ++dummy_iteration) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
            if (algorithm_name == "std_sort") {
                std::sort(working_copy.begin(), working_copy.end());
            } else if (algorithm_name == "std_stable_sort") {
                std::stable_sort(working_copy.begin(), working_copy.end());
            }
        }
    }

    sort_output output;
    output.size = working_copy.size();
    if (!working_copy.empty()) {
        output.first_value = working_copy.front();
        output.last_value  = working_copy.back();
    }
    return output;
}

[[nodiscard]] std::map<std::string, double> evaluate_sort_output(
    std::string_view /*algorithm_name*/,
    const parameter_map& /*params*/,
    const sort_input& /*input*/,
    const sort_output& output)
{
    std::map<std::string, double> metrics;
    metrics.emplace("output_size", static_cast<double>(output.size));
    return metrics;
}

// -------- bench_count I/O types --------

struct count_input {
    std::vector<std::int64_t> values;
    std::int64_t              expected_even_count = 0;
};

struct count_output {
    std::int64_t even_count = 0;
};

[[nodiscard]] run_constraints estimate_count_resources(
    std::string_view /*data_dir*/,
    std::string_view /*test_case_name*/,
    simdjson::dom::element data_element)
{
    std::int64_t length = 0;
    (void)data_element["length"].get(length);
    const auto values_bytes = (length <= 0)
        ? std::size_t{0}
        : static_cast<std::size_t>(length) * sizeof(std::int64_t);
    // count_input keeps `values`; run_count_algorithm only iterates,
    // no extra allocation in the iteration loop.
    return { .base_memory = values_bytes, .extra_memory = 0 };
}

/// return the remaining count. When the test_case JSON object carries
/// `"buggy_input": N`, the first `N` calls to `build_count_input` for
/// that specific `test_case_name` throw a `std::runtime_error`. The
/// framework's retry loop catches the exception, surfaces the retry
/// row in the dashboard, and tries again — eventually succeeding.
///
/// The counter is keyed by `test_case_name` so adding more buggy-input
/// test cases doesn't deplete a global shared budget. The whole
/// fetch-and-decrement happens under `std::mutex` so concurrent
/// builders for the same test_case_name don't race.
///
/// Returns the remaining count BEFORE the decrement — i.e. > 0 means
/// "throw"; <= 0 means "succeed". Caller never decrements outside the
/// helper.
inline int consume_buggy_input_failure(std::string_view test_case_name,
                                         int initial_failures) noexcept {
    static std::mutex mu;
    static std::map<std::string, int> per_test_case;
    std::lock_guard<std::mutex> guard(mu);
    auto [it, inserted] = per_test_case.emplace(std::string{test_case_name},
                                                  initial_failures);
    const int before = it->second;
    if (before > 0) --it->second;
    return before;
}

[[nodiscard]] count_input build_count_input(std::string_view /*data_dir*/,
                                            std::string_view test_case_name,
                                            simdjson::dom::element data_element,
                                            const run_constraints& /*constraints*/)
{
    // `"buggy_input": <int>` field giving the number of failures to
    // inject for this specific test_case. We consume one slot from the
    // per-test-case counter; when it falls to 0, on_input succeeds.
    std::int64_t initial_failures = 0;
    (void)data_element["buggy_input"].get(initial_failures);
    if (initial_failures > 0) {
        const int before = consume_buggy_input_failure(
            test_case_name, static_cast<int>(initial_failures));
        if (before > 0) {
            throw std::runtime_error(fmt::format(
                "simulated on_input failure for test_case '{}' "
                "(remaining injected failures: {})",
                test_case_name, before - 1));
        }
    }

    count_input input;
    std::int64_t length = 0;
    if (data_element["length"].get(length) != simdjson::SUCCESS || length <= 0) {
        bioseq_log<logging_level::error>("[example] count test case '{}' missing 'length'", test_case_name);
        return input;
    }
    std::int64_t random_seed = 7;
    (void)data_element["seed"].get(random_seed);

    std::mt19937_64 rng{static_cast<std::uint64_t>(random_seed)};
    input.values.resize(static_cast<std::size_t>(length));
    std::uniform_int_distribution<std::int64_t> dist(0, 1'000);
    std::int64_t expected = 0;
    for (auto& value : input.values) {
        value = dist(rng);
        if ((value & 1) == 0) ++expected;
    }
    input.expected_even_count = expected;
    return input;
}

[[nodiscard]] count_output run_count_algorithm(std::string_view /*algorithm_name*/,
                                                const parameter_map& /*params*/,
                                                const count_input& input)
{
    // Loop descriptor records one work unit for each inspected value. The
    // value we inspected). Lets the loop_total_iterations / loop_total_work
    // metrics produce non-zero data.
    static const auto* const count_loop =
        bioseq::instrumentation::register_static_loop("count_main_loop");

    count_output output;
    auto loop_scope = bioseq::instrumentation::loop(count_loop);
    for (const auto value : input.values) {
        loop_scope.record_iteration(1);
        if ((value & 1) == 0) ++output.even_count;
    }
    return output;
}

/// Optional teardown hook. Runs once per `(bench_count, test_case,
/// group)` after every iteration on that batch's cached input has
/// finished. Cleanup may run multiple
/// times for the same test_case (one call per parallel group).
void cleanup_count_input(std::string_view /*data_dir*/,
                          std::string_view test_case_name,
                          simdjson::dom::element /*data_element*/,
                          const count_input& input)
{
    bioseq_log<logging_level::info>("[example] cleanup_count_input released test_case='{}' (n={})",
         test_case_name, input.values.size());
}

/// An `on_cleanup` callback that always throws drives the
/// `▶ 🧹 ... [retry N/5]` UI row and the terminal
/// `cleanup_function_failing` warning at end-of-run.
void cleanup_always_throws(std::string_view /*data_dir*/,
                            std::string_view test_case_name,
                            simdjson::dom::element /*data_element*/,
                            const count_input& /*input*/)
{
    throw std::runtime_error(fmt::format(
        "simulated cleanup failure for test_case '{}' — "
        "demonstrates the cleanup retry path",
        test_case_name));
}

[[nodiscard]] std::map<std::string, double> evaluate_count_output(
    std::string_view /*algorithm_name*/,
    const parameter_map& /*params*/,
    const count_input& input,
    const count_output& output)
{
    // Run an independent reference count from the input and compare to
    // the algorithm's output. The metric `even_count` mirrors the
    // algorithm output; `even_count_match` is a 0/1 sentinel.
    std::int64_t reference = 0;
    for (const auto value : input.values) {
        if ((value & 1) == 0) ++reference;
    }
    std::map<std::string, double> metrics;
    metrics.emplace("even_count", static_cast<double>(output.even_count));
    metrics.emplace("even_count_match",
                    static_cast<double>(output.even_count == reference ? 1 : 0));
    return metrics;
}

}  // namespace

export namespace bioseq::bench::framework::example {

int run(int argc, char** argv) {
    configure_logging(true);
    bioseq_set_fallback_debug_logging_enabled(false);

    const std::string config_path =
        std::string{BIOSEQ_BENCH_FRAMEWORK_EXAMPLE_DATA_DIR}
        + "/example_bench_configuration.json";

    return bioseq::bench::framework::run(
        argc, argv,
        std::string{BIOSEQ_BENCH_FRAMEWORK_EXAMPLE_REPORT_DIR},
        std::string{BIOSEQ_BENCH_FRAMEWORK_EXAMPLE_DATA_DIR},
        bioseq::bench::framework::create_benchmark<sort_input, sort_output>(
            "Sorting algorithm comparison",
            "bench_sort",
            config_path,
            std::function<run_constraints(std::string_view, std::string_view, simdjson::dom::element)>{
                estimate_sort_resources},
            std::function<sort_input(std::string_view, std::string_view, simdjson::dom::element, const run_constraints&)>{
                build_sort_input},
            std::function<sort_output(std::string_view, const parameter_map&, const sort_input&)>{
                run_sort_algorithm},
            std::function<std::map<std::string, double>(std::string_view, const parameter_map&, const sort_input&, const sort_output&)>{
                evaluate_sort_output}),
        bioseq::bench::framework::create_benchmark<count_input, count_output>(
            "Even-count parity check",
            "bench_count",
            config_path,
            std::function<run_constraints(std::string_view, std::string_view, simdjson::dom::element)>{
                estimate_count_resources},
            std::function<count_input(std::string_view, std::string_view, simdjson::dom::element, const run_constraints&)>{
                build_count_input},
            std::function<count_output(std::string_view, const parameter_map&, const count_input&)>{
                run_count_algorithm},
            std::function<std::map<std::string, double>(std::string_view, const parameter_map&, const count_input&, const count_output&)>{
                evaluate_count_output},
            std::function<void(std::string_view, std::string_view, simdjson::dom::element, const count_input&)>{
                cleanup_count_input}),
        // Cleanup always throws, producing five retries and a warning.
        bioseq::bench::framework::create_benchmark<count_input, count_output>(
            "Buggy cleanup demo",
            "bench_buggy_cleanup",
            config_path,
            std::function<run_constraints(std::string_view, std::string_view, simdjson::dom::element)>{
                estimate_count_resources},
            std::function<count_input(std::string_view, std::string_view, simdjson::dom::element, const run_constraints&)>{
                build_count_input},
            std::function<count_output(std::string_view, const parameter_map&, const count_input&)>{
                run_count_algorithm},
            std::function<std::map<std::string, double>(std::string_view, const parameter_map&, const count_input&, const count_output&)>{
                evaluate_count_output},
            std::function<void(std::string_view, std::string_view, simdjson::dom::element, const count_input&)>{
                cleanup_always_throws}));
}

}  // namespace bioseq::bench::framework::example
