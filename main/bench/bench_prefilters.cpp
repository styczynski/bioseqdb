/**
 * @file bench_prefilters.cpp
 * @brief Stand-alone SneakySnake prefilter benchmark registered in bioseq_bench.
 *
 * Dataset parsing, byte-rank materialisation, the bundled-source oracle, and
 * thresholded edit-distance truth are prepared in on_input.  The measured
 * backend-batch region contains only the complete selected
 * original/scalar/SIMD/Vulkan prefilter invocation.  Runtime correctness
 * validation runs after that region.  The Vulkan-labelled row is explicitly
 * unavailable on hosts without a hardware Vulkan device and never falls back
 * to CPU.
 */
#include "common_simd.h"
#include "common.h"
#include "SneakySnake.h"
#include "prefilter_pipeline_truth.hpp"

#include <simdjson.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

import bench_framework;
import bioseq.instr;
import bioseq.sequences.prefilters.sneaky_snake;
import bioseq.sequences.prefilters.sneaky_snake_vulkan;

HWY_BEFORE_NAMESPACE();

namespace bioseq::bench_prefilters {
namespace {

using bioseq::bench_framework::ParamMap;
using bioseq::bench_framework::RunConstraints;
using bioseq::simd::HWY_NAMESPACE::SneakySnakeBatchInput;
using bioseq::simd::HWY_NAMESPACE::SneakySnakeJob;
using bioseq::simd::HWY_NAMESPACE::SneakySnakeResult;
using bioseq::simd::HWY_NAMESPACE::SneakySnakeStatus;
namespace vulkan_prefilter = bioseq::sequences::prefilters;

constexpr std::uint32_t kDefaultMaxEdits = 8u;
constexpr std::size_t kDefaultPairLimit = 1'024u;
constexpr std::uint32_t kVulkanMaxEdits =
    bioseq::gpu::vulkan::k_gpu_sneaky_snake_max_edits;

struct PreparedThreshold {
    std::vector<SneakySnakeJob> cpu_jobs;
    std::vector<vulkan_prefilter::SneakySnakeVulkanJob> vulkan_jobs;
    std::vector<SneakySnakeResult> scalar_oracle_results;
    std::vector<std::uint8_t> reference_accepted;
    std::vector<std::uint8_t> truth_within_threshold;
};

struct PrefilterInput {
    std::vector<std::string> queries;
    std::vector<std::string> targets;
    std::vector<std::uint8_t> query_ranks;
    std::vector<std::uint8_t> target_ranks;
    std::map<std::uint32_t, PreparedThreshold> prepared;
    std::uint64_t total_bases = 0u;
    /// One-time, shared batch input construction. It is not attributable to
    /// any one algorithm/threshold callback.
    std::uint64_t input_preparation_ns = 0u;
    std::uint64_t oracle_preparation_ns = 0u;
    bool valid = false;
    std::string error;
};

struct PrefilterOutput {
    std::vector<std::uint8_t> accepted;
    std::vector<std::uint32_t> lower_bounds;
    std::vector<std::uint32_t> statuses;
    /// Complete selected batch API call. For Vulkan this deliberately includes
    /// upload, dispatch, synchronization, and result readback.
    std::uint64_t backend_batch_ns = 0u;
    std::uint64_t callback_ns = 0u;
    std::uint64_t validation_ns = 0u;
    bool backend_available = true;
    bool lower_bounds_available = false;
    bool runtime_validation_performed = false;
    bool runtime_validation_passed = false;
};

[[nodiscard]] std::int64_t read_int_field(
    simdjson::dom::element data,
    std::string_view name,
    std::int64_t fallback) noexcept
{
    std::int64_t value = fallback;
    std::int64_t parsed = 0;
    if (data[name].get(parsed) == simdjson::SUCCESS) {
        value = parsed;
    }
    return value;
}

[[nodiscard]] std::string read_string_field(
    simdjson::dom::element data,
    std::string_view name,
    std::string_view fallback)
{
    std::string_view parsed;
    if (data[name].get(parsed) == simdjson::SUCCESS) {
        return std::string{parsed};
    }
    return std::string{fallback};
}

[[nodiscard]] std::uint32_t max_edits_from_params(
    const ParamMap& params) noexcept
{
    const auto found = params.find("max_edits");
    if (found == params.end() || !found->second.is_integer()) {
        return kDefaultMaxEdits;
    }
    const std::int64_t raw = found->second.as_integer();
    if (raw <= 0) {
        return raw == 0 ? 0u : kDefaultMaxEdits;
    }
    // The bundled reference ABI receives E as signed int, so every compared
    // backend uses the same admitted range.
    return static_cast<std::uint32_t>(std::min<std::int64_t>(
        raw,
        static_cast<std::int64_t>(
            (std::numeric_limits<int>::max)() - 1)));
}

[[nodiscard]] std::uint32_t max_edits_from_tuple(
    const bioseq::bench_framework::ResolvedExecutionTuple& tuple) noexcept
{
    return max_edits_from_params(tuple.params);
}

[[nodiscard]] RunConstraints estimate_prefilter_resources(
    std::string_view /*data_dir*/,
    std::string_view /*test_case_name*/,
    simdjson::dom::element data) noexcept
{
    const auto raw_pairs = read_int_field(
        data, "pair_limit", static_cast<std::int64_t>(kDefaultPairLimit));
    const auto raw_length = read_int_field(data, "sequence_length", 250);
    const std::size_t pair_count = raw_pairs > 0
        ? static_cast<std::size_t>(raw_pairs) : kDefaultPairLimit;
    const std::size_t sequence_length = raw_length > 0
        ? static_cast<std::size_t>(raw_length) : 250u;
    const std::size_t sequence_bytes = pair_count * sequence_length * 4u;
    // Covers jobs plus source/truth/scalar-oracle records for the configured
    // threshold sweep (currently at most six distinct E values).
    const std::size_t job_and_truth_bytes = pair_count * 512u;
    return RunConstraints{
        .base_memory = sequence_bytes + job_and_truth_bytes,
        .extra_memory = sequence_bytes + job_and_truth_bytes,
    };
}

void append_rank_bytes(
    std::string_view sequence,
    std::vector<std::uint8_t>& ranks)
{
    ranks.reserve(ranks.size() + sequence.size());
    for (const unsigned char value : sequence) {
        ranks.push_back(static_cast<std::uint8_t>(value));
    }
}

[[nodiscard]] PrefilterInput build_prefilter_input(
    std::string_view data_dir,
    std::string_view /*test_case_name*/,
    simdjson::dom::element data,
    const RunConstraints& constraints)
{
    PrefilterInput input;
    try {
        const auto input_preparation_start = std::chrono::steady_clock::now();
        const std::string relative_path = read_string_field(data, "path", {});
        if (relative_path.empty()) {
            throw std::invalid_argument(
                "SneakySnake benchmark test case has no dataset path");
        }
        const std::int64_t raw_limit = read_int_field(
            data, "pair_limit", static_cast<std::int64_t>(kDefaultPairLimit));
        const std::size_t pair_limit = raw_limit > 0
            ? static_cast<std::size_t>(raw_limit) : kDefaultPairLimit;
        const std::filesystem::path dataset_path =
            std::filesystem::path{std::string{data_dir}} / relative_path;
        std::ifstream dataset{dataset_path};
        if (!dataset) {
            throw std::runtime_error(
                "cannot open SneakySnake dataset: " + dataset_path.string());
        }

        input.queries.reserve(pair_limit);
        input.targets.reserve(pair_limit);
        std::string line;
        while (input.queries.size() < pair_limit
               && std::getline(dataset, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            const std::size_t separator = line.find('\t');
            if (separator == std::string::npos
                || line.find('\t', separator + 1u) != std::string::npos) {
                throw std::runtime_error(
                    "SneakySnake dataset row is not exactly two tab-separated fields");
            }
            std::string query = line.substr(0u, separator);
            std::string target = line.substr(separator + 1u);
            if (query.empty() || query.size() != target.size()) {
                throw std::runtime_error(
                    "SneakySnake benchmark requires non-empty equal-length pairs");
            }
            if (query.size() > static_cast<std::size_t>(
                    (std::numeric_limits<int>::max)())) {
                throw std::runtime_error(
                    "SneakySnake dataset sequence exceeds the reference int ABI");
            }
            input.total_bases += static_cast<std::uint64_t>(query.size()) * 2u;
            append_rank_bytes(query, input.query_ranks);
            append_rank_bytes(target, input.target_ranks);
            input.queries.push_back(std::move(query));
            input.targets.push_back(std::move(target));
        }
        if (input.queries.empty()) {
            throw std::runtime_error("SneakySnake dataset contains no pairs");
        }

        std::vector<std::uint32_t> thresholds;
        thresholds.reserve(constraints.execution_tuples.size());
        for (const auto& tuple : constraints.execution_tuples) {
            thresholds.push_back(max_edits_from_tuple(tuple));
        }
        if (thresholds.empty()) {
            thresholds.push_back(kDefaultMaxEdits);
        }
        std::sort(thresholds.begin(), thresholds.end());
        thresholds.erase(
            std::unique(thresholds.begin(), thresholds.end()),
            thresholds.end());

        for (const std::uint32_t threshold : thresholds) {
            PreparedThreshold prepared;
            prepared.cpu_jobs.reserve(input.queries.size());
            prepared.vulkan_jobs.reserve(input.queries.size());
            std::uint32_t query_offset = 0u;
            std::uint32_t target_offset = 0u;
            for (std::size_t index = 0u; index < input.queries.size(); ++index) {
                const auto length = static_cast<std::uint32_t>(
                    input.queries[index].size());
                prepared.cpu_jobs.push_back(SneakySnakeJob{
                    .query_offset = query_offset,
                    .target_offset = target_offset,
                    .query_length = length,
                    .target_length = length,
                    .max_edits = threshold,
                });
                prepared.vulkan_jobs.push_back(
                    vulkan_prefilter::SneakySnakeVulkanJob{
                        .query_offset = query_offset,
                        .target_offset = target_offset,
                        .query_length = length,
                        .target_length = length,
                        .max_edits = threshold,
                    });
                if ((std::numeric_limits<std::uint32_t>::max)() - query_offset
                        < length
                    || (std::numeric_limits<std::uint32_t>::max)() - target_offset
                        < length) {
                    throw std::runtime_error(
                        "SneakySnake flattened dataset exceeds the uint32 offset ABI");
                }
                query_offset += length;
                target_offset += length;
            }
            input.prepared.emplace(threshold, std::move(prepared));
        }
        const auto input_preparation_end = std::chrono::steady_clock::now();
        input.input_preparation_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                input_preparation_end - input_preparation_start).count());

        const auto oracle_start = std::chrono::steady_clock::now();
        for (auto& [threshold, prepared] : input.prepared) {
            prepared.scalar_oracle_results =
                bioseq::simd::HWY_NAMESPACE::sneaky_snake_scalar_batch(
                    SneakySnakeBatchInput{
                        .query_ranks = input.query_ranks,
                        .target_ranks = input.target_ranks,
                        .jobs = prepared.cpu_jobs,
                    });
            if (prepared.scalar_oracle_results.size()
                != input.queries.size()) {
                throw std::runtime_error(
                    "SneakySnake scalar oracle returned the wrong result count");
            }
            prepared.reference_accepted.resize(input.queries.size());
            prepared.truth_within_threshold.resize(input.queries.size());
            for (std::size_t index = 0u; index < input.queries.size(); ++index) {
                const int length = static_cast<int>(input.queries[index].size());
                const int geometry = length;
                // The bundled API predates const-correctness but does not mutate
                // either input. These strings remain alive for the whole batch.
                const int accepted = SneakySnake(
                    length,
                    input.targets[index].data(),
                    input.queries[index].data(),
                    static_cast<int>(threshold),
                    geometry,
                    0,
                    geometry);
                prepared.reference_accepted[index] = accepted != 0 ? 1u : 0u;
                prepared.truth_within_threshold[index] =
                    bioseq::bench_prefilter_truth::edit_distance_within(
                        input.queries[index], input.targets[index], threshold)
                    ? 1u : 0u;
            }
        }
        const auto oracle_end = std::chrono::steady_clock::now();
        input.oracle_preparation_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                oracle_end - oracle_start).count());
        input.valid = true;
    } catch (const std::exception& exception) {
        input.error = exception.what();
    } catch (...) {
        input.error = "unknown SneakySnake input-preparation failure";
    }
    return input;
}

[[noreturn]] void throw_validation_failure(
    std::string_view algorithm,
    std::uint32_t threshold,
    std::size_t index,
    std::string_view reason)
{
    throw std::runtime_error(
        "SneakySnake runtime validation failed for algorithm='"
        + std::string{algorithm}
        + "' E=" + std::to_string(threshold)
        + " pair=" + std::to_string(index)
        + ": " + std::string{reason});
}

[[nodiscard]] constexpr bool same_result(
    const SneakySnakeResult& lhs,
    const SneakySnakeResult& rhs) noexcept
{
    return lhs.lower_bound == rhs.lower_bound
        && lhs.status == rhs.status
        && lhs.accepted == rhs.accepted
        && lhs.reserved == rhs.reserved;
}

void validate_available_backend(
    std::string_view algorithm,
    std::uint32_t threshold,
    const PrefilterInput& input,
    const PreparedThreshold& prepared,
    const std::vector<SneakySnakeResult>& cpu_results,
    const std::vector<vulkan_prefilter::SneakySnakeVulkanResult>& vulkan_results,
    const PrefilterOutput& output)
{
    const std::size_t expected_count = input.queries.size();
    if (output.accepted.size() != expected_count) {
        throw_validation_failure(
            algorithm, threshold, output.accepted.size(),
            "output decision count differs from input pair count");
    }

    if (algorithm == "sneakysnake_scalar"
        || algorithm == "sneakysnake_simd") {
        if (cpu_results.size() != prepared.scalar_oracle_results.size()) {
            throw_validation_failure(
                algorithm, threshold, cpu_results.size(),
                "CPU result count differs from the untimed scalar oracle");
        }
        for (std::size_t index = 0u; index < cpu_results.size(); ++index) {
            if (!same_result(
                    cpu_results[index],
                    prepared.scalar_oracle_results[index])) {
                throw_validation_failure(
                    algorithm, threshold, index,
                    "lower-bound/status/decision differs from scalar oracle");
            }
        }
    } else if (algorithm == "sneakysnake_vulkan") {
        if (threshold > kVulkanMaxEdits) {
            throw_validation_failure(
                algorithm, threshold, 0u,
                "configured threshold exceeds the Vulkan evaluated domain");
        }
        if (vulkan_results.size() != prepared.scalar_oracle_results.size()) {
            throw_validation_failure(
                algorithm, threshold, vulkan_results.size(),
                "Vulkan result count differs from the untimed scalar oracle");
        }
        for (std::size_t index = 0u; index < vulkan_results.size(); ++index) {
            const auto& actual = vulkan_results[index];
            const auto& expected = prepared.scalar_oracle_results[index];
            if (actual.lower_bound != expected.lower_bound
                || actual.status != expected.status
                || actual.accepted != expected.accepted
                || actual.reserved != expected.reserved) {
                throw_validation_failure(
                    algorithm, threshold, index,
                    "Vulkan lower-bound/status/decision differs from scalar oracle");
            }
            if (actual.status != static_cast<std::uint32_t>(
                    SneakySnakeStatus::success)) {
                throw_validation_failure(
                    algorithm, threshold, index,
                    "Vulkan row did not execute the evaluated maze path");
            }
        }
    }

    for (std::size_t index = 0u; index < expected_count; ++index) {
        const bool accepted = output.accepted[index] != 0u;
        if (accepted != (prepared.reference_accepted[index] != 0u)) {
            throw_validation_failure(
                algorithm, threshold, index,
                "decision differs from the bundled-source compatible fixture oracle");
        }
        if (!accepted && prepared.truth_within_threshold[index] != 0u) {
            throw_validation_failure(
                algorithm, threshold, index,
                "false rejection against exact thresholded edit-distance truth");
        }
    }
}

[[nodiscard]] PrefilterOutput run_prefilter(
    std::string_view algorithm,
    const ParamMap& params,
    const PrefilterInput& input)
{
    const auto callback_start = std::chrono::steady_clock::now();
    if (!input.valid) {
        throw std::runtime_error("invalid SneakySnake benchmark input: " + input.error);
    }
    const std::uint32_t threshold = max_edits_from_params(params);
    const auto prepared_it = input.prepared.find(threshold);
    if (prepared_it == input.prepared.end()) {
        throw std::logic_error(
            "SneakySnake threshold was not prepared by on_input");
    }
    const PreparedThreshold& prepared = prepared_it->second;
    PrefilterOutput output;
    output.accepted.resize(input.queries.size());

    if (algorithm == "sneakysnake_vulkan"
        && !vulkan_prefilter::vulkan_sneaky_snake_supported()) {
        output.backend_available = false;
        output.accepted.clear();
        return output;
    }

    static const auto* backend_batch_region =
        bioseq::instr::register_region_static("sneaky_snake_backend_batch");
    const auto backend_batch_start = std::chrono::steady_clock::now();
    std::vector<SneakySnakeResult> cpu_results;
    std::vector<vulkan_prefilter::SneakySnakeVulkanResult> vulkan_results;
    {
        auto backend_batch_scope = bioseq::instr::region(backend_batch_region);
        if (algorithm == "sneakysnake_original") {
            for (std::size_t index = 0u; index < input.queries.size(); ++index) {
                const int length = static_cast<int>(input.queries[index].size());
                output.accepted[index] = SneakySnake(
                    length,
                    const_cast<char*>(input.targets[index].data()),
                    const_cast<char*>(input.queries[index].data()),
                    static_cast<int>(threshold),
                    length,
                    0,
                    length) != 0 ? 1u : 0u;
            }
        } else if (algorithm == "sneakysnake_scalar") {
            cpu_results = bioseq::simd::HWY_NAMESPACE::sneaky_snake_scalar_batch(
                SneakySnakeBatchInput{
                    .query_ranks = input.query_ranks,
                    .target_ranks = input.target_ranks,
                    .jobs = prepared.cpu_jobs,
                });
        } else if (algorithm == "sneakysnake_simd") {
            cpu_results = bioseq::simd::HWY_NAMESPACE::sneaky_snake_simd_batch(
                SneakySnakeBatchInput{
                    .query_ranks = input.query_ranks,
                    .target_ranks = input.target_ranks,
                    .jobs = prepared.cpu_jobs,
                });
        } else if (algorithm == "sneakysnake_vulkan") {
            vulkan_results = vulkan_prefilter::run_sneaky_snake_vulkan_batch(
                input.query_ranks,
                input.target_ranks,
                prepared.vulkan_jobs);
        } else {
            throw std::invalid_argument(
                "unknown SneakySnake benchmark algorithm: "
                + std::string{algorithm});
        }
    }
    const auto backend_batch_end = std::chrono::steady_clock::now();
    output.backend_batch_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            backend_batch_end - backend_batch_start).count());

    if (!cpu_results.empty()) {
        output.lower_bounds_available = true;
        output.accepted.resize(cpu_results.size());
        output.lower_bounds.resize(cpu_results.size());
        output.statuses.resize(cpu_results.size());
        for (std::size_t index = 0u; index < cpu_results.size(); ++index) {
            output.accepted[index] =
                bioseq::simd::HWY_NAMESPACE::sneaky_snake_accepted(
                    cpu_results[index]) ? 1u : 0u;
            output.lower_bounds[index] = cpu_results[index].lower_bound;
            output.statuses[index] = cpu_results[index].status;
        }
    } else if (!vulkan_results.empty()) {
        output.lower_bounds_available = true;
        output.accepted.resize(vulkan_results.size());
        output.lower_bounds.resize(vulkan_results.size());
        output.statuses.resize(vulkan_results.size());
        for (std::size_t index = 0u; index < vulkan_results.size(); ++index) {
            output.accepted[index] = vulkan_results[index].accepted != 0u ? 1u : 0u;
            output.lower_bounds[index] = vulkan_results[index].lower_bound;
            output.statuses[index] = vulkan_results[index].status;
        }
    }
    const auto validation_start = std::chrono::steady_clock::now();
    validate_available_backend(
        algorithm,
        threshold,
        input,
        prepared,
        cpu_results,
        vulkan_results,
        output);
    output.runtime_validation_performed = true;
    output.runtime_validation_passed = true;
    output.validation_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - validation_start).count());
    output.callback_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - callback_start).count());
    return output;
}

[[nodiscard]] std::map<std::string, double> evaluate_prefilter(
    std::string_view algorithm,
    const ParamMap& params,
    const PrefilterInput& input,
    const PrefilterOutput& output)
{
    const double not_measured = std::numeric_limits<double>::quiet_NaN();
    std::map<std::string, double> metrics;
    metrics.emplace("input_valid", input.valid ? 1.0 : 0.0);
    metrics.emplace("backend_available", output.backend_available ? 1.0 : 0.0);
    metrics.emplace("input_preparation_ns", static_cast<double>(
        input.input_preparation_ns));
    metrics.emplace("oracle_preparation_ns", static_cast<double>(
        input.oracle_preparation_ns));
    metrics.emplace("pair_count", static_cast<double>(input.queries.size()));
    metrics.emplace("base_count", static_cast<double>(input.total_bases));
    metrics.emplace("runtime_validation_performed",
        output.runtime_validation_performed ? 1.0 : 0.0);
    metrics.emplace("runtime_validation_passed",
        output.backend_available
            ? (output.runtime_validation_passed ? 1.0 : 0.0)
            : not_measured);
    metrics.emplace("vulkan_validation_required",
        algorithm == "sneakysnake_vulkan" && output.backend_available
            ? 1.0 : 0.0);
    if (!output.backend_available) {
        for (std::string_view name : {
                 "backend_batch_ns", "callback_non_backend_ns",
                 "callback_total_ns", "validation_ns", "result_count_valid",
                 "accepted_count", "rejected_count", "rejection_rate",
                 "evaluated_count", "reference_decision_mismatches",
                 "reference_decision_mismatch_rate", "true_similar_count",
                 "true_dissimilar_count", "false_accept_count",
                 "false_reject_count", "false_accept_rate",
                 "false_reject_rate", "backend_pairs_per_second",
                 "backend_bases_per_second", "callback_pairs_per_second",
                 "callback_bases_per_second", "lower_bound_sum",
                 "lower_bound_max"}) {
            metrics.emplace(std::string{name}, not_measured);
        }
        return metrics;
    }

    const std::uint32_t threshold = max_edits_from_params(params);
    const PreparedThreshold& prepared = input.prepared.at(threshold);
    const bool result_count_valid = output.accepted.size() == input.queries.size();
    std::uint64_t accepted_count = 0u;
    std::uint64_t rejected_count = 0u;
    std::uint64_t evaluated_count = 0u;
    std::uint64_t reference_mismatches = 0u;
    std::uint64_t true_similar = 0u;
    std::uint64_t true_dissimilar = 0u;
    std::uint64_t false_accepts = 0u;
    std::uint64_t false_rejects = 0u;
    std::uint64_t lower_bound_sum = 0u;
    std::uint32_t lower_bound_maximum = 0u;

    const std::size_t comparable_count = std::min(
        output.accepted.size(), prepared.reference_accepted.size());
    for (std::size_t index = 0u; index < comparable_count; ++index) {
        const bool accepted = output.accepted[index] != 0u;
        const bool similar = prepared.truth_within_threshold[index] != 0u;
        accepted_count += accepted ? 1u : 0u;
        rejected_count += accepted ? 0u : 1u;
        reference_mismatches += accepted
                != (prepared.reference_accepted[index] != 0u)
            ? 1u : 0u;
        true_similar += similar ? 1u : 0u;
        true_dissimilar += similar ? 0u : 1u;
        false_rejects += !accepted && similar ? 1u : 0u;
        false_accepts += accepted && !similar ? 1u : 0u;
        if (index < output.statuses.size()) {
            evaluated_count += output.statuses[index]
                    == static_cast<std::uint32_t>(SneakySnakeStatus::success)
                ? 1u : 0u;
        } else {
            ++evaluated_count;
        }
        if (index < output.lower_bounds.size()) {
            lower_bound_sum += output.lower_bounds[index];
            lower_bound_maximum = std::max(
                lower_bound_maximum, output.lower_bounds[index]);
        }
    }

    const double pair_count = static_cast<double>(input.queries.size());
    const double backend_batch_ns = static_cast<double>(
        output.backend_batch_ns);
    const double callback_non_backend_ns = static_cast<double>(
        output.callback_ns >= output.backend_batch_ns
            ? output.callback_ns - output.backend_batch_ns : 0u);
    const double callback_total_ns = static_cast<double>(output.callback_ns);
    const auto rate = [] (double count, double nanoseconds) noexcept {
        return nanoseconds > 0.0 ? count * 1.0e9 / nanoseconds : 0.0;
    };
    metrics.emplace("backend_batch_ns", backend_batch_ns);
    metrics.emplace("callback_non_backend_ns", callback_non_backend_ns);
    metrics.emplace("callback_total_ns", callback_total_ns);
    metrics.emplace("validation_ns", static_cast<double>(output.validation_ns));
    metrics.emplace("result_count_valid", result_count_valid ? 1.0 : 0.0);
    metrics.emplace("accepted_count", static_cast<double>(accepted_count));
    metrics.emplace("rejected_count", static_cast<double>(rejected_count));
    metrics.emplace("rejection_rate", pair_count == 0.0 ? 0.0
        : static_cast<double>(rejected_count) / pair_count);
    metrics.emplace("evaluated_count", static_cast<double>(evaluated_count));
    metrics.emplace("reference_decision_mismatches",
        static_cast<double>(reference_mismatches));
    metrics.emplace("reference_decision_mismatch_rate",
        pair_count == 0.0 ? 0.0
        : static_cast<double>(reference_mismatches) / pair_count);
    metrics.emplace("true_similar_count", static_cast<double>(true_similar));
    metrics.emplace("true_dissimilar_count", static_cast<double>(true_dissimilar));
    metrics.emplace("false_accept_count", static_cast<double>(false_accepts));
    metrics.emplace("false_reject_count", static_cast<double>(false_rejects));
    metrics.emplace("false_accept_rate", true_dissimilar == 0u ? 0.0
        : static_cast<double>(false_accepts)
            / static_cast<double>(true_dissimilar));
    metrics.emplace("false_reject_rate", true_similar == 0u ? 0.0
        : static_cast<double>(false_rejects)
            / static_cast<double>(true_similar));
    metrics.emplace("backend_pairs_per_second", rate(
        pair_count, backend_batch_ns));
    metrics.emplace("backend_bases_per_second", rate(
        static_cast<double>(input.total_bases), backend_batch_ns));
    metrics.emplace("callback_pairs_per_second", rate(
        pair_count, callback_total_ns));
    metrics.emplace("callback_bases_per_second", rate(
        static_cast<double>(input.total_bases), callback_total_ns));
    metrics.emplace("lower_bound_sum", output.lower_bounds_available
        ? static_cast<double>(lower_bound_sum) : not_measured);
    metrics.emplace("lower_bound_max", output.lower_bounds_available
        ? static_cast<double>(lower_bound_maximum) : not_measured);
    return metrics;
}

}  // namespace

[[nodiscard]] bioseq::bench_framework::ErasedBenchmark
make_prefilter_kernel_benchmark(std::string config_path)
{
    using bioseq::bench_framework::create_benchmark;
    auto estimate = std::function<RunConstraints(
        std::string_view, std::string_view, simdjson::dom::element)>{
            estimate_prefilter_resources};
    auto build = std::function<PrefilterInput(
        std::string_view, std::string_view, simdjson::dom::element,
        const RunConstraints&)>{build_prefilter_input};
    auto run = std::function<PrefilterOutput(
        std::string_view, const ParamMap&, const PrefilterInput&)>{
            run_prefilter};
    auto evaluate = std::function<std::map<std::string, double>(
        std::string_view, const ParamMap&, const PrefilterInput&,
        const PrefilterOutput&)>{evaluate_prefilter};
    return create_benchmark<PrefilterInput, PrefilterOutput>(
        "SneakySnake prefilter kernels (original/scalar/SIMD/Vulkan)",
        "bench_prefilter_kernels",
        std::move(config_path),
        std::move(estimate),
        std::move(build),
        std::move(run),
        std::move(evaluate));
}

}  // namespace bioseq::bench_prefilters

HWY_AFTER_NAMESPACE();
