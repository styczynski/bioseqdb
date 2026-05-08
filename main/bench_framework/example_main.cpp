/**
 * @file bench_framework/example_main.cpp
 * @brief Minimal demonstration of the benchmark framework (V11).
 *
 * Two toy benchmarks driven by `example_bench_configuration.json`:
 *
 *   1. **bench_sort** — measures sorting performance for two algorithms
 *      (`std_sort`, `std_stable_sort`). One built-in `wall_clock_ns`
 *      metric, one rule comparing the two.
 *
 *   2. **bench_count** — counts even integers in a vector. One
 *      `evaluation` metric (the count) and one rule asserting the
 *      computed count is within tolerance of the known ground truth.
 *
 * V11 wiring: each `create_benchmark` call now also takes an
 * `on_estimate_resources` lambda before `on_input`. `on_input` itself
 * receives the framework's chosen `RunConstraints` so that future
 * implementations can size their input buffers accordingly. Both
 * estimators here return the input vector's bytes so the framework
 * accounts memory correctly.
 */
#include "common.h"

#include <simdjson.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <string_view>
#include <vector>

import mlogging;
import bioseq_instr;
import bench_framework;

#ifndef BIOSEQ_BENCH_FRAMEWORK_EXAMPLE_REPORT_DIR
#define BIOSEQ_BENCH_FRAMEWORK_EXAMPLE_REPORT_DIR "./reporting/bench_framework_example"
#endif
#ifndef BIOSEQ_BENCH_FRAMEWORK_EXAMPLE_DATA_DIR
#define BIOSEQ_BENCH_FRAMEWORK_EXAMPLE_DATA_DIR "."
#endif

namespace {

using bioseq::bench_framework::ConfigValue;
using bioseq::bench_framework::ParamMap;
using bioseq::bench_framework::RunConstraints;

// -------- bench_sort I/O types --------

struct SortInput {
    std::vector<std::int64_t> values;
};

struct SortOutput {
    std::int64_t first_value = 0;
    std::int64_t last_value  = 0;
    std::size_t  size        = 0;
};

[[nodiscard]] RunConstraints estimate_sort_resources(
    std::string_view /*data_dir*/,
    std::string_view /*test_case_name*/,
    simdjson::dom::element data_element)
{
    std::int64_t length = 0;
    (void)data_element["length"].get(length);
    const auto values_bytes = (length <= 0)
        ? std::size_t{0}
        : static_cast<std::size_t>(length) * sizeof(std::int64_t);
    // base: SortInput.values lives until cleanup.
    // extra: the working_copy in run_sort_algorithm doubles allocation
    // for the duration of the iteration loop only.
    return { .base_memory = values_bytes, .extra_memory = values_bytes };
}

[[nodiscard]] SortInput build_sort_input(std::string_view /*data_dir*/,
                                          std::string_view test_case_name,
                                          simdjson::dom::element data_element,
                                          const RunConstraints& /*constraints*/)
{
    SortInput input;
    std::int64_t length = 0;
    if (data_element["length"].get(length) != simdjson::SUCCESS || length <= 0) {
        loge("[example] sort test case '{}' missing 'length'", test_case_name);
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

[[nodiscard]] SortOutput run_sort_algorithm(std::string_view algorithm_name,
                                              const ParamMap& /*params*/,
                                              const SortInput& input)
{
    // One static region descriptor per algorithm so the framework's
    // by_regions split has something to attribute samples to.
    static const auto* const sort_region =
        bioseq::instr::register_region_static("sort_run");
    static const auto* const copy_region =
        bioseq::instr::register_region_static("sort_copy_input");

    std::vector<std::int64_t> working_copy;
    {
        auto _copy_scope = bioseq::instr::region(copy_region);
        working_copy = input.values;
    }
    {
        auto _sort_scope = bioseq::instr::region(sort_region);
        if (algorithm_name == "std_sort") {
            std::sort(working_copy.begin(), working_copy.end());
        } else if (algorithm_name == "std_stable_sort") {
            std::stable_sort(working_copy.begin(), working_copy.end());
        }
    }

    SortOutput output;
    output.size = working_copy.size();
    if (!working_copy.empty()) {
        output.first_value = working_copy.front();
        output.last_value  = working_copy.back();
    }
    return output;
}

[[nodiscard]] std::map<std::string, double> evaluate_sort_output(
    std::string_view /*algorithm_name*/,
    const ParamMap& /*params*/,
    const SortInput& /*input*/,
    const SortOutput& output)
{
    std::map<std::string, double> metrics;
    metrics.emplace("output_size", static_cast<double>(output.size));
    return metrics;
}

// -------- bench_count I/O types --------

struct CountInput {
    std::vector<std::int64_t> values;
    std::int64_t              expected_even_count = 0;
};

struct CountOutput {
    std::int64_t even_count = 0;
};

[[nodiscard]] RunConstraints estimate_count_resources(
    std::string_view /*data_dir*/,
    std::string_view /*test_case_name*/,
    simdjson::dom::element data_element)
{
    std::int64_t length = 0;
    (void)data_element["length"].get(length);
    const auto values_bytes = (length <= 0)
        ? std::size_t{0}
        : static_cast<std::size_t>(length) * sizeof(std::int64_t);
    // CountInput keeps `values`; run_count_algorithm only iterates,
    // no extra allocation in the iteration loop.
    return { .base_memory = values_bytes, .extra_memory = 0 };
}

[[nodiscard]] CountInput build_count_input(std::string_view /*data_dir*/,
                                            std::string_view test_case_name,
                                            simdjson::dom::element data_element,
                                            const RunConstraints& /*constraints*/)
{
    CountInput input;
    std::int64_t length = 0;
    if (data_element["length"].get(length) != simdjson::SUCCESS || length <= 0) {
        loge("[example] count test case '{}' missing 'length'", test_case_name);
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

[[nodiscard]] CountOutput run_count_algorithm(std::string_view /*algorithm_name*/,
                                                const ParamMap& /*params*/,
                                                const CountInput& input)
{
    // Loop descriptor — each `tick(1)` recorded one work unit (the
    // value we inspected). Lets the loop_total_iterations / loop_total_work
    // metrics produce non-zero data.
    static const auto* const count_loop =
        bioseq::instr::register_loop_static("count_main_loop");

    CountOutput output;
    auto loop_scope = bioseq::instr::loop(count_loop);
    for (const auto value : input.values) {
        loop_scope.tick(1);
        if ((value & 1) == 0) ++output.even_count;
    }
    return output;
}

/// Optional teardown hook. Runs once per `(bench_count, test_case,
/// group)` after every iteration on that batch's cached `Input` has
/// finished. Logs are useful here because V11 may call cleanup multiple
/// times for the same test_case (one call per parallel group).
void cleanup_count_input(std::string_view /*data_dir*/,
                          std::string_view test_case_name,
                          simdjson::dom::element /*data_element*/,
                          const CountInput& input)
{
    logi("[example] cleanup_count_input released test_case='{}' (n={})",
         test_case_name, input.values.size());
}

[[nodiscard]] std::map<std::string, double> evaluate_count_output(
    std::string_view /*algorithm_name*/,
    const ParamMap& /*params*/,
    const CountInput& input,
    const CountOutput& output)
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

int main(int argc, char** argv) {
    log_configure(true);
    fmtlog::setLogLevel(fmtlog::INF);

    const std::string config_path =
        std::string{BIOSEQ_BENCH_FRAMEWORK_EXAMPLE_DATA_DIR}
        + "/example_bench_configuration.json";

    return bioseq::bench_framework::run(
        argc, argv,
        std::string{BIOSEQ_BENCH_FRAMEWORK_EXAMPLE_REPORT_DIR},
        std::string{BIOSEQ_BENCH_FRAMEWORK_EXAMPLE_DATA_DIR},
        bioseq::bench_framework::create_benchmark<SortInput, SortOutput>(
            "Sorting algorithm comparison",
            "bench_sort",
            config_path,
            std::function<RunConstraints(std::string_view, std::string_view, simdjson::dom::element)>{
                estimate_sort_resources},
            std::function<SortInput(std::string_view, std::string_view, simdjson::dom::element, const RunConstraints&)>{
                build_sort_input},
            std::function<SortOutput(std::string_view, const ParamMap&, const SortInput&)>{
                run_sort_algorithm},
            std::function<std::map<std::string, double>(std::string_view, const ParamMap&, const SortInput&, const SortOutput&)>{
                evaluate_sort_output}),
        bioseq::bench_framework::create_benchmark<CountInput, CountOutput>(
            "Even-count parity check",
            "bench_count",
            config_path,
            std::function<RunConstraints(std::string_view, std::string_view, simdjson::dom::element)>{
                estimate_count_resources},
            std::function<CountInput(std::string_view, std::string_view, simdjson::dom::element, const RunConstraints&)>{
                build_count_input},
            std::function<CountOutput(std::string_view, const ParamMap&, const CountInput&)>{
                run_count_algorithm},
            std::function<std::map<std::string, double>(std::string_view, const ParamMap&, const CountInput&, const CountOutput&)>{
                evaluate_count_output},
            std::function<void(std::string_view, std::string_view, simdjson::dom::element, const CountInput&)>{
                cleanup_count_input}));
}
