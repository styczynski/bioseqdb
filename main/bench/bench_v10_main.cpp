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
import bioseq_sequences.pipe;            // for minimap_port entry point (DOC0249 §9.7)
import bioseq_storage_lazy;             // DOC0263 — make_lazy_in_memory / lazy wrapper
import bioseq_instr;
import bench_framework;
import bench.tool_minimap2;
import bioseq_sequences.generation;

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
using bioseq::bench_framework::RunConstraints;
using bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType;
using bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType;
using bioseq::simd::HWY_NAMESPACE::MatchResultCollection;
using bioseq::simd::HWY_NAMESPACE::Minimap2Params;
using bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType;
using bioseq::sequences::generation::HWY_NAMESPACE::GeneratedBenchSequencePair;
using bioseq::sequences::generation::HWY_NAMESPACE::GeneratorParams;
using bioseq::sequences::generation::HWY_NAMESPACE::generate_test_sequences;

constexpr std::int32_t kPositionTolerance = 10;

struct AlignerInput {
    /// Generated pairs are read-only after `on_input` returns, but
    /// `tool_minimap2` / `minimap_port` take non-const `compact_sequence_range&`
    /// refs even though they don't mutate. `mutable` lets concurrent
    /// `on_run` calls access them through their declared signatures
    /// without forcing a `const_cast` at every call site.
    mutable std::vector<GeneratedBenchSequencePair<seq::dna4, std::uint32_t>> pairs;
    /// Pre-decoded ACGT strings in lock-step with `pairs`. Populated
    /// once in `build_input` so the per-iteration timing window of
    /// `run_aligner` does not include the O(bp_size) compact-to-ACGT
    /// conversion that `tool_minimap2` used to do inline. See issue
    /// 1b in main/DOCS/REPORTS/STRANGE_TIMING_COMPREHESIVE_REPORT__DOC0215.md.
    std::vector<std::string> queries_acgt;
    std::vector<std::string> targets_acgt;
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

/// Estimate the memory required by one `(benchmark, test_case, group)`
/// batch so the framework can throttle parallel batches against the
/// global memory budget (see `main/DOCS/BENCHMARK_FRAMEWORK/BENCH_V11_IMPL_PLAN__DOC0224.md` §3.5 / §4 and
/// `main/DOCS/BENCHMARK_FRAMEWORK/BENCH_V12_IMPL_PLAN__DOC0226.md` §3.4).
///
/// We size off the data already in the JSON blob, before any sequence
/// generation. The cached `AlignerInput` keeps every generated pair in compact
/// dna4/u32 storage and also keeps pre-decoded ACGT strings for the minimap2
/// reference path. `generate_test_sequences()` now packs each target from the
/// single unpacked reference without making a second full-reference temporary,
/// so the transient generation peak is the unpacked reference plus one compact
/// target/query pair, not `8 * total_bp`.
[[nodiscard]] RunConstraints estimate_aligner_resources(
    std::string_view /*data_dir*/,
    std::string_view /*test_case_name*/,
    simdjson::dom::element data) noexcept
{
    const auto fragment_count = static_cast<std::size_t>(
        read_int_field(data, "fragment_count", 100));
    const auto fragment_length_mean = static_cast<std::size_t>(
        read_int_field(data, "fragment_length_mean", 150));
    const auto reference_length = static_cast<std::size_t>(
        read_int_field(data, "reference_length", 10000));

    using range_t = compact_sequence_range<
        sequence_custom_alphabet<seq::dna4, std::uint32_t>>;
    using traits = typename range_t::traits;
    const auto packed_bytes_for = [] (std::size_t bp) noexcept -> std::size_t {
        return ((bp + traits::letters_per_unit - 1) / traits::letters_per_unit)
             * sizeof(typename traits::storage_unit);
    };

    const std::size_t target_packed = packed_bytes_for(reference_length);
    const std::size_t query_packed  = packed_bytes_for(fragment_length_mean);
    const std::size_t acgt_bytes_per_pair = reference_length + fragment_length_mean;
    const std::size_t cached_per_pair =
        target_packed + query_packed + acgt_bytes_per_pair;
    const std::size_t cached_input_bytes = fragment_count * cached_per_pair;

    // One unpacked dna4 reference is live while the compact pairs are produced.
    // Add a modest scratch allowance for DP/anchor vectors used by the timed
    // alignment call without letting the estimate dominate scheduling.
    const std::size_t generation_peak_extra = reference_length;
    const std::size_t aligner_scratch_extra =
        fragment_count * (fragment_length_mean / 2 + 1024);
    return {
        .base_memory  = cached_input_bytes,
        .extra_memory = generation_peak_extra + aligner_scratch_extra,
    };
}

/// Materialise a `(query, target)` batch from the `data` JSON blob.
/// Lives in `on_input`; runs once per `(benchmark, test_case, group)`
/// batch.  The `constraints` argument carries the same numbers
/// `estimate_aligner_resources` returned, but we don't need them — the
/// generator dimensions come straight from the JSON blob — so the
/// parameter is unused.
[[nodiscard]] AlignerInput build_input(std::string_view /*data_dir*/,
                                        std::string_view test_case_name,
                                        simdjson::dom::element data,
                                        const RunConstraints& /*constraints*/) {
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
    input.pairs = generate_test_sequences<seq::dna4, std::uint32_t>(params);
    input.kmer_size_override = static_cast<std::int32_t>(
        read_int_field(data, "kmer_size", 0));

    // Pre-decode each pair's compact representation into ACGT so the
    // tool_minimap2 cell does not pay the O(bp_size) decompress
    // inside its timing window. Cost lives in on_input (excluded from
    // wall_clock_ns by `std::call_once`-guarded caching). Memory cost
    // is one ASCII byte per base per pair; for XXL_HIFI's 500 Mbp
    // target that's an extra ~500 MB beyond the existing packed
    // representation. Acceptable: the cached input is held only for
    // the duration of the benchmark.
    input.queries_acgt.reserve(input.pairs.size());
    input.targets_acgt.reserve(input.pairs.size());
    for (auto& pair : input.pairs) {
        input.queries_acgt.push_back(
            bioseq::bench::HWY_NAMESPACE::compact_sequence_to_acgt(pair.query));
        input.targets_acgt.push_back(
            bioseq::bench::HWY_NAMESPACE::compact_sequence_to_acgt(pair.target));
    }

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
    } else if (algo == "bioseq_ksw2_batched_simd_dp_two_stack_nthash") {
        // DOC0254 §11.9: KSW2_BATCHED selected via runtime params.
        dispatch.params.chaining_algorithm       = ChainingAlgorithmType::SIMD_MM2;
        dispatch.params.base_alignment_algorithm = BaseAlignmentAlgorithmType::KSW2_BATCHED;
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
    for (std::size_t pair_index = 0; pair_index < input.pairs.size(); ++pair_index) {
        auto& pair = input.pairs[pair_index];     // pairs are `mutable` in AlignerInput; see field comment
        MatchResultCollection results;
        // Defensive try-block: a few algorithm × test_case combinations
        // (notably wide-substitution low-indel patterns on LONG / VERY_LONG
        // / XL inputs in `bench_aligners_grid`) currently throw
        // `std::length_error` from inside the chaining or alignment
        // back-end. Without this catch, the unhandled exception unwinds
        // through the framework's TBB worker, calls `std::terminate`, and
        // aborts the whole bench mid-grid.  We log the offending tuple so
        // a follow-up can drill in, and surface a zero-result iteration
        // so the run can still complete.
        try {
            if (dispatch.is_minimap2_reference) {
                results = bioseq::bench::HWY_NAMESPACE::tool_minimap2(
                    input.queries_acgt[pair_index],
                    input.targets_acgt[pair_index],
                    dispatch.params);
            } else {
                // DOC0263 — minimap_port now takes lazy_compact_sequence&; wrap the
                // in-memory ranges in the zero-overhead no-fragment lazy wrapper.
                auto bench_lazy_query  = bioseq::storage::make_lazy_in_memory(pair.query);
                auto bench_lazy_target = bioseq::storage::make_lazy_in_memory(pair.target);
                results = bioseq::simd::HWY_NAMESPACE::minimap_port(
                    bench_lazy_query, bench_lazy_target, dispatch.params);
            }
        } catch (const std::exception& exception) {
            loge("[bench_v10] aligner threw exception (algo='{}', pair_index={}, "
                 "query_len={}, target_len={}, k={}): {}",
                 algo, pair_index,
                 input.queries_acgt[pair_index].size(),
                 input.targets_acgt[pair_index].size(),
                 dispatch.params.kmer_size,
                 exception.what());
            continue;
        } catch (...) {
            loge("[bench_v10] aligner threw non-std exception (algo='{}', pair_index={})",
                 algo, pair_index);
            continue;
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
    using bioseq::bench_framework::RunConstraints;
    using bioseq::bench_framework::create_benchmark;

    // Mandatory `std::function<...>` casts (README §3e) — `create_benchmark`'s
    // template-arg deduction won't bind a lambda or function pointer to a
    // `std::function` parameter without help. Reused across the four blocks
    // below since all four share the same I/O types and lambdas.
    auto estimate = std::function<RunConstraints(
        std::string_view, std::string_view, simdjson::dom::element)>{
            estimate_aligner_resources};
    auto build = std::function<AlignerInput(
        std::string_view, std::string_view, simdjson::dom::element,
        const RunConstraints&)>{build_input};
    auto run = std::function<AlignerOutput(
        std::string_view, const ParamMap&, const AlignerInput&)>{run_aligner};
    auto eval = std::function<std::map<std::string, double>(
        std::string_view, const ParamMap&,
        const AlignerInput&, const AlignerOutput&)>{evaluate_aligner};

    return bioseq::bench_framework::run(
        argc, argv, report_dir, data_dir,
        create_benchmark<AlignerInput, AlignerOutput>(
            "Aligner full grid (length × error × algorithm)",
            "bench_aligners_grid", config_path, estimate, build, run, eval),
        create_benchmark<AlignerInput, AlignerOutput>(
            "MinimizerRepr storage exercise (K=7 / K=27)",
            "bench_aligners_repr_storage", config_path, estimate, build, run, eval),
        create_benchmark<AlignerInput, AlignerOutput>(
            "RMQ chaining LONG/CCS regression repro",
            "bench_aligners_rmq_repro", config_path, estimate, build, run, eval),
        create_benchmark<AlignerInput, AlignerOutput>(
            "Curated correctness probes",
            "bench_aligners_curated", config_path, estimate, build, run, eval));
}
