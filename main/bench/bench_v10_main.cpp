/**
 * @file bench_v10_main.cpp
 * @brief Aligner bench entry point on top of bench_framework (V10).
 *
 * Replaces the legacy Catch2-based harness in `bench/bench_main.cpp` and
 * its surrounding `bench/aligners/` + `bench/bench_scheduler/` trees.
 * One TU per binary, three lambdas (`build_input` / `run_aligner` /
 * `evaluate_aligner`), one `bioseq::bench_framework::run(...)` call.
 *
 * Four benchmark blocks declared in `bench_v10_configuration.json`:
 *   * bench_aligners_grid          — full length × error × algorithm matrix
 *   * bench_aligners_repr_storage  — 32-bit / 128-bit MinimizerRepr cases
 *   * bench_aligners_rmq_repro     — pinned LONG/CCS RMQ-chaining seed
 *   * bench_aligners_curated       — curated correctness probes
 *
 * Sample input reference: bench_framework/example_main.cpp.
 */
#include "common_simd.h"
#include "common.h"

#include <simdjson.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

import mlogging;
import sequan_compat;
import bioseq_sequences;
import bioseq_sequences.minimap_port;
import bioseq_instr;
import bench_framework;
import bench.tool_minimap2;
import bench.sequence_generator;

#ifndef BIOSEQ_BENCH_REPORT_DIR
#error "BIOSEQ_BENCH_REPORT_DIR must be defined by the build system"
#endif
#ifndef BIOSEQ_BENCH_DATA_DIR
#error "BIOSEQ_BENCH_DATA_DIR must be defined by the build system"
#endif
#ifndef BIOSEQ_BENCH_CONFIG_PATH
#error "BIOSEQ_BENCH_CONFIG_PATH must be defined by the build system"
#endif

HWY_BEFORE_NAMESPACE();

namespace {

using bioseq::bench_framework::ParamMap;
using bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType;
using bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType;
using bioseq::simd::HWY_NAMESPACE::MatchResultCollection;
using bioseq::simd::HWY_NAMESPACE::Minimap2Params;
using bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType;
using bioseq::bench::HWY_NAMESPACE::GeneratedBenchSequencePair;
using bioseq::bench::HWY_NAMESPACE::GeneratorParams;
using bioseq::bench::HWY_NAMESPACE::generate_test_sequences;

constexpr std::int32_t kPositionTolerance = 10;

struct AlignerInput {
    /// Generated pairs are read-only after `on_input` returns, but
    /// `tool_minimap2` / `minimap_port` take non-const `compact_sequence_range&`
    /// refs even though they don't mutate. `mutable` lets concurrent
    /// `on_run` calls access them through their declared signatures
    /// without forcing a `const_cast` at every call site.
    mutable std::vector<GeneratedBenchSequencePair> pairs;
    /// Optional kmer-size override pulled from the test case `data`
    /// blob. 0 means "use the algorithm default (15)". Set on the
    /// REPR_STORAGE_* test cases to force a specific MinimizerRepr
    /// storage instantiation (K=7 → 32-bit, K=27 → 128-bit).
    std::int32_t kmer_size_override = 0;
};

struct AlignerOutput {
    std::size_t total_results          = 0;
    std::size_t correct_pairs          = 0;
    std::size_t total_pairs            = 0;
    double      sum_abs_position_offset = 0.0;
};

/// Pull a numeric field out of a simdjson element with a fallback.
[[nodiscard]] double read_double_field(simdjson::dom::element data,
                                        std::string_view name,
                                        double fallback) noexcept {
    double value = fallback;
    auto element_or_err = data[name];
    if (element_or_err.error() == simdjson::SUCCESS) {
        const auto& element = element_or_err.value_unsafe();
        if (element.is<double>()) {
            value = element.get_double().value_unsafe();
        } else if (element.is<std::int64_t>()) {
            value = static_cast<double>(element.get_int64().value_unsafe());
        }
    }
    return value;
}

[[nodiscard]] std::int64_t read_int_field(simdjson::dom::element data,
                                            std::string_view name,
                                            std::int64_t fallback) noexcept {
    std::int64_t value = fallback;
    std::int64_t parsed = 0;
    if (data[name].get(parsed) == simdjson::SUCCESS) {
        value = parsed;
    }
    return value;
}

/// Materialise a `(query, target)` batch from the `data` JSON blob.
/// Lives in `on_input`; runs at most once per (benchmark, test_case).
[[nodiscard]] AlignerInput build_input(std::string_view /*data_dir*/,
                                        std::string_view test_case_name,
                                        simdjson::dom::element data) {
    GeneratorParams params{};
    params.reference_length = static_cast<std::size_t>(
        read_int_field(data, "reference_length",
                        static_cast<std::int64_t>(params.reference_length)));
    params.fragment_count = static_cast<std::size_t>(
        read_int_field(data, "fragment_count",
                        static_cast<std::int64_t>(params.fragment_count)));
    params.fragment_length_mean = static_cast<std::size_t>(
        read_int_field(data, "fragment_length_mean",
                        static_cast<std::int64_t>(params.fragment_length_mean)));
    params.fragment_length_stddev = read_double_field(
        data, "fragment_length_stddev", params.fragment_length_stddev);
    params.substitution_rate = read_double_field(
        data, "substitution_rate", params.substitution_rate);
    params.insertion_rate = read_double_field(
        data, "insertion_rate", params.insertion_rate);
    params.deletion_rate = read_double_field(
        data, "deletion_rate", params.deletion_rate);
    params.insertion_length_mean = read_double_field(
        data, "insertion_length_mean", params.insertion_length_mean);
    params.deletion_length_mean = read_double_field(
        data, "deletion_length_mean", params.deletion_length_mean);
    params.reverse_complement_rate = read_double_field(
        data, "reverse_complement_rate", params.reverse_complement_rate);
    params.seed = static_cast<std::uint64_t>(
        read_int_field(data, "seed", static_cast<std::int64_t>(params.seed)));

    AlignerInput input;
    input.pairs = generate_test_sequences(params);
    input.kmer_size_override = static_cast<std::int32_t>(
        read_int_field(data, "kmer_size", 0));
    JustLog("[bench_v10] build_input '{}' produced {} pairs (k_override={})",
             test_case_name, input.pairs.size(), input.kmer_size_override);
    return input;
}

/// Map an algorithm key from the JSON config to a fully-resolved
/// `Minimap2Params` plus a flag selecting the C reference vs the bioseq
/// port. A fixed kmer_size variant is selected for the special K=7 / K=27
/// REPR_STORAGE test cases via the test-case name passed via the input.
struct AlgorithmDispatch {
    bool is_minimap2_reference = false;
    Minimap2Params params{};
};

[[nodiscard]] AlgorithmDispatch dispatch_algorithm(std::string_view algo,
                                                    std::int32_t kmer_override) {
    AlgorithmDispatch dispatch;
    dispatch.params.kmer_size   = kmer_override > 0 ? kmer_override : 15;
    dispatch.params.window_size = 10;

    if (algo == "minimap2_dp") {
        dispatch.is_minimap2_reference = true;
        dispatch.params.chaining_algorithm = ChainingAlgorithmType::SCALAR_MM2;
    } else if (algo == "minimap2_rmq") {
        dispatch.is_minimap2_reference = true;
        dispatch.params.chaining_algorithm = ChainingAlgorithmType::RMQ;
    } else if (algo == "bioseq_ksw2_scalar_dp_scalar_mm") {
        dispatch.params.chaining_algorithm       = ChainingAlgorithmType::SCALAR_MM2;
        dispatch.params.base_alignment_algorithm = BaseAlignmentAlgorithmType::KSW2;
        dispatch.params.minimizer_algorithm      = MinimizerAlgorithmType::SCALAR;
    } else if (algo == "bioseq_ksw2_simd_dp_scalar_mm") {
        dispatch.params.chaining_algorithm       = ChainingAlgorithmType::SIMD_MM2;
        dispatch.params.base_alignment_algorithm = BaseAlignmentAlgorithmType::KSW2;
        dispatch.params.minimizer_algorithm      = MinimizerAlgorithmType::SCALAR;
    } else if (algo == "bioseq_ksw2_simd_dp_two_stack_nthash") {
        dispatch.params.chaining_algorithm       = ChainingAlgorithmType::SIMD_MM2;
        dispatch.params.base_alignment_algorithm = BaseAlignmentAlgorithmType::KSW2;
        dispatch.params.minimizer_algorithm      = MinimizerAlgorithmType::TWO_STACK_NTHASH;
    } else if (algo == "bioseq_ksw2_simd_dp_simd_minimap2_slow") {
        dispatch.params.chaining_algorithm       = ChainingAlgorithmType::SIMD_MM2;
        dispatch.params.base_alignment_algorithm = BaseAlignmentAlgorithmType::KSW2;
        dispatch.params.minimizer_algorithm      = MinimizerAlgorithmType::SIMD_MINIMAP2_SLOW;
    } else if (algo == "bioseq_block_simd_dp_scalar_mm") {
        dispatch.params.chaining_algorithm       = ChainingAlgorithmType::SIMD_MM2;
        dispatch.params.base_alignment_algorithm = BaseAlignmentAlgorithmType::BLOCK_ALIGNER;
        dispatch.params.minimizer_algorithm      = MinimizerAlgorithmType::SCALAR;
    } else if (algo == "bioseq_block_simd_dp_two_stack_nthash") {
        dispatch.params.chaining_algorithm       = ChainingAlgorithmType::SIMD_MM2;
        dispatch.params.base_alignment_algorithm = BaseAlignmentAlgorithmType::BLOCK_ALIGNER;
        dispatch.params.minimizer_algorithm      = MinimizerAlgorithmType::TWO_STACK_NTHASH;
    } else if (algo == "bioseq_ksw2_rmq_scalar_mm") {
        dispatch.params.chaining_algorithm       = ChainingAlgorithmType::RMQ;
        dispatch.params.base_alignment_algorithm = BaseAlignmentAlgorithmType::KSW2;
        dispatch.params.minimizer_algorithm      = MinimizerAlgorithmType::SCALAR;
    } else {
        loge("[bench_v10] unknown algorithm '{}'", algo);
    }
    return dispatch;
}

/// Hand the (query, target) pair off to either the C reference or the
/// bioseq port, then aggregate the per-pair primary-correctness flags
/// into the AlignerOutput.
[[nodiscard]] AlignerOutput run_aligner(std::string_view algo,
                                          const ParamMap& /*params*/,
                                          const AlignerInput& input) {
    const AlgorithmDispatch dispatch = dispatch_algorithm(algo, input.kmer_size_override);

    AlignerOutput output;
    output.total_pairs = input.pairs.size();
    for (auto& pair : input.pairs) {  // pairs are `mutable` in AlignerInput; see field comment
        MatchResultCollection results;
        if (dispatch.is_minimap2_reference) {
            results = bioseq::bench::HWY_NAMESPACE::tool_minimap2(
                pair.query, pair.target, dispatch.params);
        } else {
            results = bioseq::simd::HWY_NAMESPACE::minimap_port(
                pair.query, pair.target, dispatch.params);
        }
        bool found_primary = false;
        for (const auto& match : results.view()) {
            output.total_results += 1;
            if (!match.is_primary || found_primary) continue;
            found_primary = true;
            const std::int32_t offset =
                match.target_start - static_cast<std::int32_t>(pair.truth_target_start);
            const std::int32_t abs_offset = std::abs(offset);
            output.sum_abs_position_offset += static_cast<double>(abs_offset);
            if (abs_offset <= kPositionTolerance &&
                match.is_reverse == pair.is_reverse) {
                output.correct_pairs += 1;
            }
        }
    }
    return output;
}

/// Translate `Output` into the evaluation metrics declared in the JSON
/// config. The framework drops keys that aren't declared on the active
/// benchmark, so we always emit the full set; only the keys the running
/// benchmark consumes land in the report.
[[nodiscard]] std::map<std::string, double> evaluate_aligner(
    std::string_view /*algo*/,
    const ParamMap& /*params*/,
    const AlignerInput& /*input*/,
    const AlignerOutput& output)
{
    std::map<std::string, double> metrics;
    metrics.emplace("match_count", static_cast<double>(output.total_results));
    const double pairs_d = static_cast<double>(output.total_pairs);
    metrics.emplace("primary_correct_fraction",
                     output.total_pairs == 0 ? 0.0
                     : static_cast<double>(output.correct_pairs) / pairs_d);
    metrics.emplace("mean_position_offset_bp",
                     output.total_pairs == 0 ? 0.0
                     : output.sum_abs_position_offset / pairs_d);
    return metrics;
}

}  // namespace

HWY_AFTER_NAMESPACE();

int main(int argc, char** argv) {
    log_configure(true);
    fmtlog::setLogLevel(fmtlog::INF);

    const std::string config_path{BIOSEQ_BENCH_CONFIG_PATH};
    const std::string report_dir{BIOSEQ_BENCH_REPORT_DIR};
    const std::string data_dir{BIOSEQ_BENCH_DATA_DIR};

    using bioseq::bench_framework::ParamMap;
    using bioseq::bench_framework::create_benchmark;

    // Mandatory `std::function<...>` casts (README §3e) — `create_benchmark`'s
    // template-arg deduction won't bind a lambda or function pointer to a
    // `std::function` parameter without help. Reused across the four blocks
    // below since all four share the same I/O types and lambdas.
    auto build = std::function<AlignerInput(
        std::string_view, std::string_view, simdjson::dom::element)>{build_input};
    auto run = std::function<AlignerOutput(
        std::string_view, const ParamMap&, const AlignerInput&)>{run_aligner};
    auto eval = std::function<std::map<std::string, double>(
        std::string_view, const ParamMap&,
        const AlignerInput&, const AlignerOutput&)>{evaluate_aligner};

    return bioseq::bench_framework::run(
        argc, argv, report_dir, data_dir,
        create_benchmark<AlignerInput, AlignerOutput>(
            "Aligner full grid (length × error × algorithm)",
            "bench_aligners_grid", config_path, build, run, eval),
        create_benchmark<AlignerInput, AlignerOutput>(
            "MinimizerRepr storage exercise (K=7 / K=27)",
            "bench_aligners_repr_storage", config_path, build, run, eval),
        create_benchmark<AlignerInput, AlignerOutput>(
            "RMQ chaining LONG/CCS regression repro",
            "bench_aligners_rmq_repro", config_path, build, run, eval),
        create_benchmark<AlignerInput, AlignerOutput>(
            "Curated correctness probes",
            "bench_aligners_curated", config_path, build, run, eval));
}
