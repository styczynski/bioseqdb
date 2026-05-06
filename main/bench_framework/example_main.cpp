/**
 * @file bench_framework/example_main.cpp
 * @brief Minimal demonstration of the benchmark framework.
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
 * The point is to show the user-side wiring: three lambdas + one
 * `bioseq::bench_framework::run(...)` call. No bioseq-specific imports.
 */
#include "common.h"

#include <simdjson.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <numeric>
#include <random>
#include <string>
#include <string_view>
#include <vector>

import mlogging;
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

// -------- bench_sort I/O types --------

struct SortInput {
    std::vector<std::int64_t> values;
};

struct SortOutput {
    std::int64_t first_value = 0;
    std::int64_t last_value  = 0;
    std::size_t  size        = 0;
};

[[nodiscard]] SortInput build_sort_input(std::string_view /*data_dir*/,
                                          std::string_view test_case_name,
                                          simdjson::dom::element data_element)
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
    std::vector<std::int64_t> working_copy = input.values;
    if (algorithm_name == "std_sort") {
        std::sort(working_copy.begin(), working_copy.end());
    } else if (algorithm_name == "std_stable_sort") {
        std::stable_sort(working_copy.begin(), working_copy.end());
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

[[nodiscard]] CountInput build_count_input(std::string_view /*data_dir*/,
                                            std::string_view test_case_name,
                                            simdjson::dom::element data_element)
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
    CountOutput output;
    for (const auto value : input.values) {
        if ((value & 1) == 0) ++output.even_count;
    }
    return output;
}

[[nodiscard]] std::map<std::string, double> evaluate_count_output(
    std::string_view /*algorithm_name*/,
    const ParamMap& /*params*/,
    const CountOutput& output)
{
    std::map<std::string, double> metrics;
    metrics.emplace("even_count", static_cast<double>(output.even_count));
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
            std::function<SortInput(std::string_view, std::string_view, simdjson::dom::element)>{
                build_sort_input},
            std::function<SortOutput(std::string_view, const ParamMap&, const SortInput&)>{
                run_sort_algorithm},
            std::function<std::map<std::string, double>(std::string_view, const ParamMap&, const SortOutput&)>{
                evaluate_sort_output}),
        bioseq::bench_framework::create_benchmark<CountInput, CountOutput>(
            "Even-count parity check",
            "bench_count",
            config_path,
            std::function<CountInput(std::string_view, std::string_view, simdjson::dom::element)>{
                build_count_input},
            std::function<CountOutput(std::string_view, const ParamMap&, const CountInput&)>{
                run_count_algorithm},
            std::function<std::map<std::string, double>(std::string_view, const ParamMap&, const CountOutput&)>{
                evaluate_count_output}));
}
