/**
 * @file bench_v10_main.cpp
 * @brief Aligner bench entry point on top of bench_framework (V10).
 *
 * Replaces the legacy Catch2-based harness in `bench/bench_main.cpp` and
 * its surrounding `bench/aligners/` + `bench/bench_scheduler/` trees.
 * One TU per binary, three lambdas (`build_input` / `run_aligner` /
 * `evaluate_aligner`), one `bioseq::bench_framework::run(...)` call.
 *
 * Six benchmark blocks declared in `bench_v10_configuration.json`:
 *   * bench_aligners_grid          — full length × error × algorithm matrix
 *   * bench_aligners_repr_storage  — 32-bit / 128-bit MinimizerRepr cases
 *   * bench_aligners_rmq_repro     — pinned LONG/CCS RMQ-chaining seed
 *   * bench_aligners_curated       — curated correctness probes
 *   * bench_aligners_xxl_hifi_relative — report_76 relative-performance gate
 *   * bench_aligners_xxl_hifi_paired — serial paired-ratio companion gate
 *
 * Sample input reference: bench_framework/example_main.cpp.
 */
#include "common_simd.h"
#include "common.h"

#include <simdjson.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

import bioseq.common.mlogging;
import bioseq.common.sequan_compat;
import bioseq.sequences;
import bioseq.sequences.kmers.two_stacks;
import bioseq.sequences.chaining.simd;
import bioseq.alignment.block_aligner;
import bioseq.sequences.minimap_port;
import bioseq.sequences.pipe;            // for minimap_port entry point (DOC0249 §9.7)
import bioseq.storage.lazy;             // DOC0263 — make_lazy_in_memory / lazy wrapper
import bioseq.instr;
import bioseq.parallelism.memory_admission;
import bench_framework;
import bench.tool_minimap2;
import bioseq.sequences.generation;
import bioseq.gpu.vulkan.runtime;

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
using bioseq::simd::HWY_NAMESPACE::PartialVulkanExecutionProof;
using bioseq::simd::HWY_NAMESPACE::TwoStackExecutionProof;
using bioseq::simd::HWY_NAMESPACE::SimdChainingExecutionProof;
using bioseq::simd::HWY_NAMESPACE::BlockAlignerExecutionProof;
namespace gen = bioseq::sequences::generation;
using bench_generator_t = gen::alphabet_sequence_generator<seq::dna4, std::uint32_t>;

constexpr std::int32_t kPositionTolerance = 10;

struct AlignerInput {
    /// Generated pairs are read-only after `on_input` returns, but
    /// `tool_minimap2` / `minimap_port` take non-const `compact_sequence_range&`
    /// refs even though they don't mutate. `mutable` lets concurrent
    /// `on_run` calls access them through their declared signatures
    /// without forcing a `const_cast` at every call site.
    mutable std::vector<typename bench_generator_t::query_target_pair> pairs;
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
    bool        vulkan_hardware_unavailable = false;
    /// The three Vulkan-labelled rows in this historical report_76 matrix are
    /// deliberately mixed pipelines. None is evidence for the user's
    /// all-on-GPU acceptance condition.
    bool        historical_partial_gpu_probe = false;
    bool        all_gpu_acceptance_eligible = false;
    bool        phase_c_gpu_index_ownership_proof_accepted = false;
    PartialVulkanExecutionProof partial_vulkan_execution_proof{};
    TwoStackExecutionProof exact_simd_query_execution_proof{};
    TwoStackExecutionProof exact_simd_target_execution_proof{};
    SimdChainingExecutionProof simd_chaining_execution_proof{};
    BlockAlignerExecutionProof block_aligner_execution_proof{};
    bool exact_simd_query_execution_proof_present = false;
    bool exact_simd_target_execution_proof_present = false;
    bool simd_chaining_execution_proof_present = false;
    bool block_aligner_execution_proof_present = false;
    /// Set only after `require_selected_route_execution` accepts the
    /// operation-originated proof for every processed pair. The strict XXL
    /// raw companion persists this beside each duration.
    bool        selected_route_execution_proof_accepted = false;
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
/// reference path. DOC0335 generation now shares the compact target across
/// pairs and fills packed storage directly, so generation no longer carries a
/// per-pair full-reference copy.
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
    // Add DP/anchor scratch plus the possible resident target-index footprint.
    // Phase-C GPU builds hold a target index resident for the duration of the
    // timed run; omitting it let XXL_HIFI co-schedule two 500 Mbp GPU rows and
    // trip the fail-closed Vulkan admission gate before dispatch.
    const std::uint64_t generation_peak_extra = reference_length;
    const std::uint64_t aligner_scratch_extra =
        fragment_count * (fragment_length_mean / 2 + 1024);
    const std::uint64_t target_index_extra =
        bioseq::parallelism::minimizer_index_peak_bytes_for_bp(reference_length);
    const std::uint64_t extra_memory =
        bioseq::parallelism::saturating_add(
            bioseq::parallelism::saturating_add(
                generation_peak_extra,
                aligner_scratch_extra),
            target_index_extra);
    return {
        .base_memory  = cached_input_bytes,
        .extra_memory = static_cast<std::size_t>(
            std::min<std::uint64_t>(
                extra_memory,
                static_cast<std::uint64_t>(
                    (std::numeric_limits<std::size_t>::max)()))),
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
    gen::generator_params params{};
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
    input.pairs = bench_generator_t(params).generate_sequences_pairs(params.fragment_count);
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
            bioseq::bench::HWY_NAMESPACE::compact_sequence_to_acgt(*pair.target));
    }

    bioseq_log<DBG>("[bench_v10] build_input '{}' produced {} pairs (k_override={})",
             test_case_name, input.pairs.size(), input.kmer_size_override);
    return input;
}

/// Map an algorithm key from the JSON config to a fully-resolved
/// `Minimap2Params` plus a flag selecting the C reference vs the bioseq
/// port. A fixed kmer_size variant is selected for the special K=7 / K=27
/// REPR_STORAGE test cases via the test-case name passed via the input.
struct AlgorithmDispatch {
    bool is_minimap2_reference = false;
    bool explicit_vulkan = false;
    bool historical_partial_gpu_probe = false;
    Minimap2Params params{};
};

[[nodiscard]] bool is_explicit_vulkan_hardware_available(std::string_view algo) {
    auto& runtime = bioseq::gpu::vulkan::Runtime::instance();
    if (bioseq::gpu::vulkan::hardware_device_available(runtime)) {
        return true;
    }

    static std::atomic_bool logged_unavailable{false};
    if (!logged_unavailable.exchange(true, std::memory_order_relaxed)) {
        if (runtime.usable()) {
            bioseq_log<WRN>(
                "[bench_v10] explicit Vulkan benchmark rows are not measured on this host "
                "(first algo='{}'): active Vulkan device '{}' is a CPU/software device "
                "(VkPhysicalDeviceType={}); hardware Vulkan is required for GPU timing/parity",
                algo,
                runtime.features().device_name.data(),
                runtime.features().device_type);
        } else {
            bioseq_log<WRN>(
                "[bench_v10] explicit Vulkan benchmark rows are not measured on this host "
                "(first algo='{}'): Vulkan runtime is unavailable: {}",
                algo,
                runtime.initialization_error());
        }
    }
    return false;
}

[[nodiscard]] AlgorithmDispatch dispatch_algorithm(std::string_view algo,
                                                    std::int32_t kmer_override) {
    AlgorithmDispatch dispatch;
    dispatch.params.kmer_size   = kmer_override > 0 ? kmer_override : 15;
    dispatch.params.window_size = 10;

    if (algo == "minimap2_dp") {
        dispatch.is_minimap2_reference = true;
        dispatch.params.chaining_algorithm       = ChainingAlgorithmType::SCALAR_MM2;
        dispatch.params.base_alignment_algorithm = BaseAlignmentAlgorithmType::KSW2;
        dispatch.params.minimizer_algorithm      = MinimizerAlgorithmType::SCALAR;
    } else if (algo == "minimap2_rmq") {
        dispatch.is_minimap2_reference = true;
        dispatch.params.chaining_algorithm       = ChainingAlgorithmType::RMQ;
        dispatch.params.base_alignment_algorithm = BaseAlignmentAlgorithmType::KSW2;
        dispatch.params.minimizer_algorithm      = MinimizerAlgorithmType::SCALAR;
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
    } else if (algo == "bioseq_ksw2_vulkan_tiled_dp_two_stack_nthash") {
        // DOC0304 Phase A historical PARTIAL-GPU probe: CPU ExactSimd
        // minimizers, Vulkan score generation, CPU backtracking and CPU KSW2.
        // Explicit Vulkan selection fails loudly when no hardware device exists.
        // Identical minimizer (two-stack nthash) + base aligner (KSW2) to
        // bioseq_ksw2_simd_dp_two_stack_nthash, so the only varied dimension is
        // the chaining backend: SIMD_MM2 vs VULKAN_TILED_DP.
        dispatch.params.chaining_algorithm       = ChainingAlgorithmType::VULKAN_TILED_DP;
        dispatch.params.base_alignment_algorithm = BaseAlignmentAlgorithmType::KSW2;
        dispatch.params.minimizer_algorithm      = MinimizerAlgorithmType::TWO_STACK_NTHASH;
        dispatch.explicit_vulkan = true;
        dispatch.historical_partial_gpu_probe = true;
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
        dispatch.params.do_cigar                 = true;
    } else if (algo == "bioseq_block_simd_dp_two_stack_nthash") {
        dispatch.params.chaining_algorithm       = ChainingAlgorithmType::SIMD_MM2;
        dispatch.params.base_alignment_algorithm = BaseAlignmentAlgorithmType::BLOCK_ALIGNER;
        dispatch.params.minimizer_algorithm      = MinimizerAlgorithmType::TWO_STACK_NTHASH;
        dispatch.params.do_cigar                 = true;
    } else if (algo == "bioseq_block_vulkan_dp_vulkan_minimap_int") {
        // Historical PARTIAL-GPU probe: Vulkan minimap-int minimizers and seed
        // lookup, CPU target-index construction, Vulkan tiled-DP score
        // generation, CPU backtracking, and CPU Block Aligner. The minimizer
        // sketch is always the fused tiled Phase-C path
        // after DOC0351; this row keeps the older seed/index frontend settings
        // for comparison. The only varied
        // dimensions versus bioseq_block_simd_dp_two_stack_nthash (SIMD
        // minimizers + SIMD chaining + block aligner) are the seeding (GPU
        // sketch + GPU seed-hit lookup) and the chaining backend; the base
        // aligner stays block (no G3SA extension kernel in Phase B, §0/§10).
        // All GPU stages fail loudly when no hardware Vulkan device exists.
        dispatch.params.chaining_algorithm        = ChainingAlgorithmType::VULKAN_TILED_DP;
        dispatch.params.base_alignment_algorithm  = BaseAlignmentAlgorithmType::BLOCK_ALIGNER;
        dispatch.params.minimizer_algorithm       = MinimizerAlgorithmType::VULKAN_MINIMAP_INT;
        dispatch.params.vulkan_enabled            = true;
        dispatch.params.vulkan_seed_hits_enabled  = true;
        dispatch.params.vulkan_index_cache_enabled = true;
        dispatch.params.do_cigar                  = true;
        // Keep the resident two-submit seed/index frontend OFF for the legacy
        // comparison row. This no longer selects a Phase-B minimizer sketch.
        dispatch.params.vulkan_fused_frontend_enabled  = false;
        dispatch.params.vulkan_gpu_index_build_enabled = false;
        dispatch.explicit_vulkan = true;
        dispatch.historical_partial_gpu_probe = true;
    } else if (algo == "bioseq_block_vulkan_phase_c_frontend") {
        // DOC0310 Phase C historical PARTIAL-GPU probe — the resident two-submit
        // Vulkan frontend: parallel
        // ordered tile sketch + GPU target-index build (device radix sort +
        // race-free slot table) + GPU seed count/emit + device anchor sort +
        // Vulkan tiled-DP score generation, followed by CPU backtracking and CPU
        // Block Aligner (`vulkan_extend_enabled=false`). Same aligner as the
        // fastest CPU row; the varied dimensions are the GPU frontend and GPU
        // index build. It fails loudly when no hardware Vulkan device
        // exists. This row is compared against
        // `bioseq_block_simd_dp_two_stack_nthash` (SIMD minimizers + SIMD
        // chaining + block aligner) and `minimap2_dp`.
        dispatch.params.chaining_algorithm        = ChainingAlgorithmType::VULKAN_TILED_DP;
        dispatch.params.base_alignment_algorithm  = BaseAlignmentAlgorithmType::BLOCK_ALIGNER;
        dispatch.params.minimizer_algorithm       = MinimizerAlgorithmType::VULKAN_MINIMAP_INT;
        dispatch.params.vulkan_enabled            = true;
        dispatch.params.vulkan_seed_hits_enabled  = true;
        dispatch.params.vulkan_index_cache_enabled = true;
        dispatch.params.vulkan_fused_frontend_enabled  = true;
        dispatch.params.vulkan_gpu_index_build_enabled = true;
        dispatch.params.vulkan_extend_enabled          = false;
        dispatch.params.do_cigar                       = true;
        dispatch.explicit_vulkan = true;
        dispatch.historical_partial_gpu_probe = true;
    } else if (algo == "bioseq_ksw2_rmq_scalar_mm") {
        dispatch.params.chaining_algorithm       = ChainingAlgorithmType::RMQ;
        dispatch.params.base_alignment_algorithm = BaseAlignmentAlgorithmType::KSW2;
        dispatch.params.minimizer_algorithm      = MinimizerAlgorithmType::SCALAR;
    } else {
        bioseq_log<ERR>("[bench_v10] unknown algorithm '{}'", algo);
        throw std::invalid_argument(fmt::format(
            "bench_v10 has no dispatch route for algorithm key '{}'", algo));
    }
    return dispatch;
}

[[nodiscard]] constexpr bool
legacy_gpu_index_materialization_proof_accepted(
        const PartialVulkanExecutionProof& proof) noexcept {
    return !proof.saturated
        && proof.gpu_index_materializations == 1u
        && proof.gpu_index_open_addressed_materializations == 1u
        && proof.gpu_index_sorted_dense_materializations == 0u
        && proof.gpu_index_unique_keys != 0u
        && proof.gpu_index_slot_capacity != 0u
        && proof.gpu_index_logical_resident_bytes != 0u
        && proof.gpu_index_actual_allocation_bytes
            >= proof.gpu_index_logical_resident_bytes
        && proof.gpu_index_fully_touched_bytes
            == proof.gpu_index_actual_allocation_bytes
        && proof.gpu_index_staging_bytes == 0u
        && proof.gpu_index_max_load_ppm != 0u
        && proof.gpu_index_max_load_ppm <= 700'000u
        && proof.gpu_index_max_probe != 0u
        && proof.gpu_index_permit_releases == 1u
        && proof.gpu_index_process_footprint_reflections == 1u
        && proof.gpu_index_reflected_footprint_bytes
            >= proof.gpu_index_actual_allocation_bytes
        && proof.post_seed_owned_gpu_cache_evictions == 1u
        && proof.post_seed_owned_gpu_resident_bytes_released
            == proof.gpu_index_actual_allocation_bytes
        && proof.post_seed_owned_cpu_index_releases == 1u
        && proof.post_seed_owned_cpu_position_bytes_released != 0u;
}

static_assert([] {
    PartialVulkanExecutionProof proof{};
    proof.gpu_index_materializations = 1u;
    proof.gpu_index_open_addressed_materializations = 1u;
    proof.gpu_index_unique_keys = 3u;
    proof.gpu_index_slot_capacity = 8u;
    proof.gpu_index_logical_resident_bytes = 128u;
    proof.gpu_index_actual_allocation_bytes = 256u;
    proof.gpu_index_fully_touched_bytes = 256u;
    proof.gpu_index_permit_releases = 1u;
    proof.gpu_index_process_footprint_reflections = 1u;
    proof.gpu_index_reflected_footprint_bytes = 256u;
    proof.gpu_index_max_load_ppm = 375'000u;
    proof.gpu_index_max_probe = 3u;
    proof.post_seed_owned_gpu_cache_evictions = 1u;
    proof.post_seed_owned_gpu_resident_bytes_released = 256u;
    proof.post_seed_owned_cpu_index_releases = 1u;
    proof.post_seed_owned_cpu_position_bytes_released = 128u;
    if (!legacy_gpu_index_materialization_proof_accepted(proof)) return false;
    proof.gpu_index_permit_releases = 0u;
    if (legacy_gpu_index_materialization_proof_accepted(proof)) return false;
    proof.gpu_index_permit_releases = 1u;
    proof.gpu_index_process_footprint_reflections = 0u;
    if (legacy_gpu_index_materialization_proof_accepted(proof)) return false;
    proof.gpu_index_process_footprint_reflections = 1u;
    proof.gpu_index_reflected_footprint_bytes = 255u;
    return !legacy_gpu_index_materialization_proof_accepted(proof);
}());

static_assert(bioseq::gpu::vulkan::kPhaseCQueryGateWordCount == 2'097'152u);
static_assert(bioseq::gpu::vulkan::kPhaseCQueryGateMask == 67'108'863u);

[[nodiscard]] constexpr bool
phase_c_gpu_index_ownership_proof_accepted(
        const PartialVulkanExecutionProof& proof) noexcept {
    constexpr std::uint64_t gate_word_count =
        bioseq::gpu::vulkan::kPhaseCQueryGateWordCount;
    constexpr std::uint64_t gate_mask =
        bioseq::gpu::vulkan::kPhaseCQueryGateMask;
    return !proof.saturated
        && proof.target_sketch_dispatches == 1u
        && proof.target_index_build_dispatches == 1u
        && proof.gpu_index_count_phase_admissions == 1u
        && proof.gpu_index_main_peak_admissions == 1u
        && proof.gpu_index_resident_transfers == 1u
        && proof.gpu_index_adoption_rereserve_attempts == 0u
        && proof.gpu_index_adoption_successes == 1u
        && proof.gpu_index_owned_resident_bytes != 0u
        && proof.gpu_index_gate_enabled == 1u
        && proof.gpu_index_gate_word_count == gate_word_count
        && proof.gpu_index_gate_mask == gate_mask
        && proof.gpu_index_gate_set_bits != 0u
        && proof.gpu_index_gate_set_bits
            <= proof.gpu_index_gate_word_count * 32u
        && proof.gpu_index_gate_count_dispatches == 1u
        && proof.gpu_index_gated_emit_dispatches == 1u
        && proof.gpu_index_ungated_emit_dispatches == 0u
        && proof.gpu_index_retained_minimizer_count != 0u
        && proof.gpu_index_distinct_minimizer_count != 0u
        && proof.gpu_index_distinct_minimizer_count
            <= proof.gpu_index_retained_minimizer_count
        && proof.post_seed_owned_gpu_cache_evictions == 1u
        && proof.post_seed_owned_gpu_resident_bytes_released
            == proof.gpu_index_owned_resident_bytes
        && proof.post_seed_owned_cpu_index_releases == 0u
        && proof.post_seed_owned_cpu_position_bytes_released == 0u;
}

static_assert([] {
    PartialVulkanExecutionProof proof{};
    proof.target_sketch_dispatches = 1u;
    proof.target_index_build_dispatches = 1u;
    proof.gpu_index_count_phase_admissions = 1u;
    proof.gpu_index_main_peak_admissions = 1u;
    proof.gpu_index_resident_transfers = 1u;
    proof.gpu_index_adoption_successes = 1u;
    proof.gpu_index_owned_resident_bytes = 4096u;
    proof.gpu_index_gate_enabled = 1u;
    proof.gpu_index_gate_word_count = 2'097'152u;
    proof.gpu_index_gate_mask = 67'108'863u;
    proof.gpu_index_gate_set_bits = 17u;
    proof.gpu_index_gate_count_dispatches = 1u;
    proof.gpu_index_gated_emit_dispatches = 1u;
    proof.gpu_index_retained_minimizer_count = 53u;
    proof.gpu_index_distinct_minimizer_count = 17u;
    proof.post_seed_owned_gpu_cache_evictions = 1u;
    proof.post_seed_owned_gpu_resident_bytes_released = 4096u;
    if (!phase_c_gpu_index_ownership_proof_accepted(proof)) return false;

    auto invalid = proof;
    invalid.target_sketch_dispatches = 2u;
    if (phase_c_gpu_index_ownership_proof_accepted(invalid)) return false;
    invalid = proof;
    invalid.target_index_build_dispatches = 2u;
    if (phase_c_gpu_index_ownership_proof_accepted(invalid)) return false;
    invalid = proof;
    invalid.gpu_index_adoption_rereserve_attempts = 1u;
    if (phase_c_gpu_index_ownership_proof_accepted(invalid)) return false;
    invalid = proof;
    invalid.gpu_index_ungated_emit_dispatches = 1u;
    if (phase_c_gpu_index_ownership_proof_accepted(invalid)) return false;
    invalid = proof;
    invalid.gpu_index_gate_word_count = 64u;
    if (phase_c_gpu_index_ownership_proof_accepted(invalid)) return false;
    invalid = proof;
    invalid.gpu_index_gate_mask = 63u;
    if (phase_c_gpu_index_ownership_proof_accepted(invalid)) return false;
    invalid = proof;
    invalid.gpu_index_gate_set_bits = 67'108'865u;
    if (phase_c_gpu_index_ownership_proof_accepted(invalid)) return false;
    invalid = proof;
    invalid.gpu_index_distinct_minimizer_count = 54u;
    return !phase_c_gpu_index_ownership_proof_accepted(invalid);
}());

[[nodiscard]] constexpr bool seed_radix_sort_proof_accepted(
        const PartialVulkanExecutionProof& proof) noexcept {
    return !proof.saturated
        && proof.seed_lookup_dispatches != 0u
        && proof.seed_bitonic_sort_dispatches == 0u
        && proof.seed_radix_sort_operations == proof.seed_lookup_dispatches
        && proof.seed_radix_anchor_count > 1024u
        && proof.seed_radix_passes != 0u
        && proof.seed_radix_histogram_dispatches == proof.seed_radix_passes
        && proof.seed_radix_scan_dispatches == proof.seed_radix_passes
        && proof.seed_radix_scatter_dispatches == proof.seed_radix_passes
        && proof.seed_host_sort_fallbacks == 0u;
}

static_assert([] {
    PartialVulkanExecutionProof proof{};
    proof.seed_lookup_dispatches = 1u;
    proof.seed_radix_sort_operations = 1u;
    proof.seed_radix_anchor_count = 1'520'944u;
    proof.seed_radix_passes = 4u;
    proof.seed_radix_histogram_dispatches = 4u;
    proof.seed_radix_scan_dispatches = 4u;
    proof.seed_radix_scatter_dispatches = 4u;
    if (!seed_radix_sort_proof_accepted(proof)) return false;
    proof.seed_radix_sort_operations = 2u;
    if (seed_radix_sort_proof_accepted(proof)) return false;
    proof.seed_radix_sort_operations = 1u;
    proof.seed_bitonic_sort_dispatches = 1u;
    if (seed_radix_sort_proof_accepted(proof)) return false;
    proof.seed_bitonic_sort_dispatches = 0u;
    proof.seed_host_sort_fallbacks = 1u;
    return !seed_radix_sort_proof_accepted(proof);
}());

/// Fail-closed proof policy for one completed pair. The minimap2 reference is
/// intentionally outside Bioseq's proof ABI and therefore must emit no Bioseq
/// algorithm proof. Every Bioseq row is validated against the selectors that
/// fixed its template leaf. The strict XXL gate additionally requires positive
/// non-empty vector work; ordinary benchmark rows still reject missing,
/// malformed, forbidden, or unexpected evidence without requiring work from an
/// empty input. Routes that did not select an algorithm must not emit its proof.
void require_selected_route_execution(
        std::string_view algo,
        const AlgorithmDispatch& dispatch,
        const MatchResultCollection& results,
        bool strict_xxl_gate) {
    if (dispatch.is_minimap2_reference) {
        if (dispatch.params.minimizer_algorithm != MinimizerAlgorithmType::SCALAR
            || dispatch.params.base_alignment_algorithm
                != BaseAlignmentAlgorithmType::KSW2
            || results.has_any_algorithm_execution_proof()) {
            throw std::runtime_error(fmt::format(
                "minimap2 reference row '{}' violated scalar/KSW2/no-Bioseq-proof policy",
                algo));
        }
        return;
    }

    const bool exact_simd_sketch_selected =
        dispatch.params.minimizer_algorithm
            == MinimizerAlgorithmType::TWO_STACK_NTHASH;
    if (exact_simd_sketch_selected) {
        if (!results.exact_simd_query_execution_accepted(
                /*require_nonempty_vector_work=*/strict_xxl_gate)
            || !results.exact_simd_target_execution_accepted(
                /*require_nonempty_vector_work=*/strict_xxl_gate)) {
            throw std::runtime_error(
                "selected ExactSimd sketch route lacks accepted query/target proof");
        }
    } else if (results.exact_simd_query_execution_proof_present
               || results.exact_simd_target_execution_proof_present) {
        throw std::runtime_error(
            "non-ExactSimd sketch route emitted ExactSimd proof");
    }

    const bool simd_chaining_selected =
        dispatch.params.chaining_algorithm == ChainingAlgorithmType::SIMD_MM2;
    if (simd_chaining_selected) {
        if (!results.simd_chaining_execution_accepted(
                /*require_nonempty_vector_work=*/strict_xxl_gate)) {
            throw std::runtime_error(
                "selected SIMD chaining route lacks accepted score/backtrack proof");
        }
    } else if (results.simd_chaining_execution_proof_present) {
        throw std::runtime_error(
            "non-SIMD chaining route emitted SIMD chaining proof");
    }

    const bool block_aligner_selected =
        dispatch.params.base_alignment_algorithm
            == BaseAlignmentAlgorithmType::BLOCK_ALIGNER;
    if (block_aligner_selected) {
        if (!dispatch.params.do_cigar) {
            if (strict_xxl_gate) {
                throw std::runtime_error(fmt::format(
                    "strict XXL Block Aligner row '{}' must request CIGAR execution",
                    algo));
            }
            if (results.block_aligner_execution_proof_present) {
                throw std::runtime_error(fmt::format(
                    "Block Aligner row '{}' emitted proof although CIGAR was disabled",
                    algo));
            }
        } else if (!results.block_aligner_execution_accepted(
                       /*require_nonempty_simd_work=*/strict_xxl_gate)) {
            const auto& proof = results.block_aligner_execution_proof;
            throw std::runtime_error(fmt::format(
                "selected Block Aligner lacks accepted SIMD proof: "
                "algo='{}' cigar={} present={} accepted={} executed={} "
                "nonempty={} vector_batches={} cells={} ksw2={} scalar_fallbacks={}",
                algo, dispatch.params.do_cigar,
                results.block_aligner_execution_proof_present,
                proof.accepted(), proof.executed_calls(), proof.nonempty_calls,
                proof.simd_dp_vector_batches, proof.simd_dp_cells,
                proof.forbidden_ksw2_calls,
                proof.forbidden_scalar_biological_fallbacks));
        }
    } else if (results.block_aligner_execution_proof_present) {
        throw std::runtime_error(fmt::format(
            "non-Block row '{}' emitted Block Aligner execution proof", algo));
    }

    if (dispatch.explicit_vulkan) {
        bool expect_query_sketch = false;
        bool expect_target_sketch = false;
        bool expect_target_index_build = false;
        bool expect_seed_lookup = false;
        bool expect_chain_score = true;
        if (algo == "bioseq_ksw2_vulkan_tiled_dp_two_stack_nthash") {
            // CPU ExactSimd sketch/index/seed, Vulkan chain score only.
        } else if (algo
                   == "bioseq_block_vulkan_dp_vulkan_minimap_int") {
            expect_query_sketch = true;
            expect_target_sketch = true;
            expect_seed_lookup = true;
        } else if (algo == "bioseq_block_vulkan_phase_c_frontend") {
            expect_query_sketch = true;
            expect_target_sketch = true;
            expect_target_index_build = true;
            expect_seed_lookup = true;
        } else {
            throw std::logic_error(fmt::format(
                "explicit Vulkan row '{}' has no proof-stage contract", algo));
        }
        if (strict_xxl_gate
            && !results.partial_vulkan_execution_proof
                    .exact_stage_mask_accepted(
                        expect_query_sketch,
                        expect_target_sketch,
                        expect_target_index_build,
                        expect_seed_lookup,
                        expect_chain_score)) {
            const auto& proof = results.partial_vulkan_execution_proof;
            throw std::runtime_error(fmt::format(
                "strict XXL Vulkan row '{}' lacks exact positive dispatch proof: "
                "query_sketch={} target_sketch={} target_index_build={} "
                "seed_lookup={} chain_score={} saturated={}",
                algo,
                proof.query_sketch_dispatches,
                proof.target_sketch_dispatches,
                proof.target_index_build_dispatches,
                proof.seed_lookup_dispatches,
                proof.chain_score_dispatches,
                proof.saturated));
        }
        if (strict_xxl_gate
            && algo == "bioseq_block_vulkan_dp_vulkan_minimap_int") {
            const auto& proof = results.partial_vulkan_execution_proof;
            const bool materialization_proof_accepted =
                legacy_gpu_index_materialization_proof_accepted(proof);
            if (!materialization_proof_accepted) {
                throw std::runtime_error(fmt::format(
                    "strict XXL legacy Vulkan row lacks exact GPU-index "
                    "materialization proof: materializations={} open={} "
                    "sorted={} D={} capacity={} load_ppm={} max_probe={} "
                    "logical_bytes={} actual_bytes={} touched_bytes={} "
                    "staging_bytes={} permit_releases={} reflections={} "
                    "reflected_delta_bytes={} post_seed_gpu_evictions={} "
                    "post_seed_gpu_bytes={} post_seed_cpu_releases={} "
                    "post_seed_cpu_position_bytes={}",
                    proof.gpu_index_materializations,
                    proof.gpu_index_open_addressed_materializations,
                    proof.gpu_index_sorted_dense_materializations,
                    proof.gpu_index_unique_keys,
                    proof.gpu_index_slot_capacity,
                    proof.gpu_index_max_load_ppm,
                    proof.gpu_index_max_probe,
                    proof.gpu_index_logical_resident_bytes,
                    proof.gpu_index_actual_allocation_bytes,
                    proof.gpu_index_fully_touched_bytes,
                    proof.gpu_index_staging_bytes,
                    proof.gpu_index_permit_releases,
                    proof.gpu_index_process_footprint_reflections,
                    proof.gpu_index_reflected_footprint_bytes,
                    proof.post_seed_owned_gpu_cache_evictions,
                    proof.post_seed_owned_gpu_resident_bytes_released,
                    proof.post_seed_owned_cpu_index_releases,
                    proof.post_seed_owned_cpu_position_bytes_released));
            }
        }
        if (strict_xxl_gate
            && algo == "bioseq_block_vulkan_phase_c_frontend") {
            const auto& proof = results.partial_vulkan_execution_proof;
            if (!phase_c_gpu_index_ownership_proof_accepted(proof)) {
                throw std::runtime_error(fmt::format(
                    "strict XXL Phase-C row lacks exact GPU target-index "
                    "ownership/gated-build proof: target_sketches={} target_builds={} "
                    "count_admissions={} main_admissions={} transfers={} "
                    "rereserve={} adoptions={} resident_bytes={} "
                    "gate_enabled={} gate_words={} gate_mask={} gate_set_bits={} "
                    "gate_count={} gated_emit={} ungated_emit={} T={} D={} "
                    "post_seed_gpu_evictions={} post_seed_gpu_bytes={} "
                    "post_seed_cpu_releases={} post_seed_cpu_position_bytes={} "
                    "saturated={}",
                    proof.target_sketch_dispatches,
                    proof.target_index_build_dispatches,
                    proof.gpu_index_count_phase_admissions,
                    proof.gpu_index_main_peak_admissions,
                    proof.gpu_index_resident_transfers,
                    proof.gpu_index_adoption_rereserve_attempts,
                    proof.gpu_index_adoption_successes,
                    proof.gpu_index_owned_resident_bytes,
                    proof.gpu_index_gate_enabled,
                    proof.gpu_index_gate_word_count,
                    proof.gpu_index_gate_mask,
                    proof.gpu_index_gate_set_bits,
                    proof.gpu_index_gate_count_dispatches,
                    proof.gpu_index_gated_emit_dispatches,
                    proof.gpu_index_ungated_emit_dispatches,
                    proof.gpu_index_retained_minimizer_count,
                    proof.gpu_index_distinct_minimizer_count,
                    proof.post_seed_owned_gpu_cache_evictions,
                    proof.post_seed_owned_gpu_resident_bytes_released,
                    proof.post_seed_owned_cpu_index_releases,
                    proof.post_seed_owned_cpu_position_bytes_released,
                    proof.saturated));
            }
        }
        if (strict_xxl_gate
            && (algo == "bioseq_block_vulkan_dp_vulkan_minimap_int"
                || algo == "bioseq_block_vulkan_phase_c_frontend")) {
            const auto& proof = results.partial_vulkan_execution_proof;
            if (!seed_radix_sort_proof_accepted(proof)) {
                throw std::runtime_error(fmt::format(
                    "strict XXL Vulkan row '{}' lacks scalable GPU seed-sort proof: "
                    "seed_dispatches={} bitonic={} radix_ops={} radix_anchors={} "
                    "radix_passes={} histogram={} scan={} scatter={} host_sort={}",
                    algo,
                    proof.seed_lookup_dispatches,
                    proof.seed_bitonic_sort_dispatches,
                    proof.seed_radix_sort_operations,
                    proof.seed_radix_anchor_count,
                    proof.seed_radix_passes,
                    proof.seed_radix_histogram_dispatches,
                    proof.seed_radix_scan_dispatches,
                    proof.seed_radix_scatter_dispatches,
                    proof.seed_host_sort_fallbacks));
            }
        }
    } else if (results.partial_vulkan_execution_proof.has_any_dispatch()) {
        throw std::runtime_error(fmt::format(
            "non-Vulkan row '{}' emitted partial Vulkan execution proof", algo));
    }
}

/// Hand the (query, target) pair off to either the C reference or the
/// bioseq port, then aggregate the per-pair primary-correctness flags
/// into the AlignerOutput.
[[nodiscard]] AlignerOutput run_aligner_impl(
                                          std::string_view algo,
                                          const ParamMap& /*params*/,
                                          const AlignerInput& input,
                                          bool strict_xxl_gate,
                                          bool capture_full_route_proof) {
    const AlgorithmDispatch dispatch = dispatch_algorithm(algo, input.kmer_size_override);

    AlignerOutput output;
    output.total_pairs = input.pairs.size();
    output.historical_partial_gpu_probe =
        dispatch.historical_partial_gpu_probe;
    output.all_gpu_acceptance_eligible = false;
    if (dispatch.explicit_vulkan != dispatch.historical_partial_gpu_probe) {
        throw std::logic_error(fmt::format(
            "Vulkan row '{}' lacks explicit historical partial-GPU classification",
            algo));
    }
    if (dispatch.explicit_vulkan && !is_explicit_vulkan_hardware_available(algo)) {
        if (strict_xxl_gate) {
            throw std::runtime_error(
                "strict XXL gate requires every selected Vulkan row to execute");
        }
        output.vulkan_hardware_unavailable = true;
        return output;
    }
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
                auto bench_lazy_target = bioseq::storage::make_lazy_in_memory(*pair.target);
                results = bioseq::simd::HWY_NAMESPACE::minimap_port(
                    bench_lazy_query, bench_lazy_target, dispatch.params);
            }
        } catch (const bioseq::gpu::vulkan::VulkanRuntimeError& exception) {
            bioseq_log<ERR>("[bench_v10] selected Vulkan aligner failed (algo='{}', pair_index={}, "
                 "query_len={}, target_len={}, k={}): {}",
                 algo, pair_index,
                 input.queries_acgt[pair_index].size(),
                 input.targets_acgt[pair_index].size(),
                 dispatch.params.kmer_size,
                 exception.what());
            if (dispatch.explicit_vulkan || strict_xxl_gate) {
                throw;
            }
            continue;
        } catch (const std::exception& exception) {
            bioseq_log<ERR>("[bench_v10] aligner threw exception (algo='{}', pair_index={}, "
                 "query_len={}, target_len={}, k={}): {}",
                 algo, pair_index,
                 input.queries_acgt[pair_index].size(),
                 input.targets_acgt[pair_index].size(),
                 dispatch.params.kmer_size,
                 exception.what());
            if (strict_xxl_gate) throw;
            continue;
        } catch (...) {
            bioseq_log<ERR>("[bench_v10] aligner threw non-std exception (algo='{}', pair_index={})",
                 algo, pair_index);
            if (strict_xxl_gate) throw;
            continue;
        }
        require_selected_route_execution(
            algo, dispatch, results, strict_xxl_gate);
        if (capture_full_route_proof) {
            output.selected_route_execution_proof_accepted = true;
            if (results.exact_simd_query_execution_proof_present) {
                output.exact_simd_query_execution_proof.combine(
                    results.exact_simd_query_execution_proof);
                output.exact_simd_query_execution_proof_present = true;
            }
            if (results.exact_simd_target_execution_proof_present) {
                output.exact_simd_target_execution_proof.combine(
                    results.exact_simd_target_execution_proof);
                output.exact_simd_target_execution_proof_present = true;
            }
            if (results.simd_chaining_execution_proof_present) {
                output.simd_chaining_execution_proof.combine(
                    results.simd_chaining_execution_proof);
                output.simd_chaining_execution_proof_present = true;
            }
            if (results.block_aligner_execution_proof_present) {
                output.block_aligner_execution_proof.combine(
                    results.block_aligner_execution_proof);
                output.block_aligner_execution_proof_present = true;
            }
        }
        output.partial_vulkan_execution_proof.combine(
            results.partial_vulkan_execution_proof);
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
    output.phase_c_gpu_index_ownership_proof_accepted =
        algo == "bioseq_block_vulkan_phase_c_frontend"
        && phase_c_gpu_index_ownership_proof_accepted(
            output.partial_vulkan_execution_proof);
    if (strict_xxl_gate
        && (output.total_pairs != 1
            || output.total_results != 1
            || output.correct_pairs != 1)) {
        throw std::runtime_error(fmt::format(
            "strict XXL correctness gate rejected algo='{}': pairs={} "
            "match_count={} correct_pairs={} (required 1/1/1)",
            algo, output.total_pairs, output.total_results,
            output.correct_pairs));
    }
    return output;
}

[[nodiscard]] AlignerOutput run_aligner(
        std::string_view algo,
        const ParamMap& params,
        const AlignerInput& input) {
    return run_aligner_impl(algo, params, input, false, false);
}

[[nodiscard]] AlignerOutput run_aligner_strict_xxl(
        std::string_view algo,
        const ParamMap& params,
        const AlignerInput& input) {
    return run_aligner_impl(algo, params, input, true, false);
}

[[nodiscard]] AlignerOutput run_aligner_strict_xxl_paired(
        std::string_view algo,
        const ParamMap& params,
        const AlignerInput& input) {
    return run_aligner_impl(algo, params, input, true, true);
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
    metrics.emplace("historical_partial_gpu_probe",
                    output.historical_partial_gpu_probe ? 1.0 : 0.0);
    metrics.emplace("all_gpu_acceptance_eligible",
                    output.all_gpu_acceptance_eligible ? 1.0 : 0.0);
    metrics.emplace("phase_c_gpu_index_ownership_proof_accepted",
        output.phase_c_gpu_index_ownership_proof_accepted ? 1.0 : 0.0);
    metrics.emplace("vulkan_query_sketch_dispatches",
                    static_cast<double>(output.partial_vulkan_execution_proof
                                            .query_sketch_dispatches));
    metrics.emplace("vulkan_target_sketch_dispatches",
                    static_cast<double>(output.partial_vulkan_execution_proof
                                            .target_sketch_dispatches));
    metrics.emplace("vulkan_target_index_build_dispatches",
                    static_cast<double>(output.partial_vulkan_execution_proof
                                            .target_index_build_dispatches));
    metrics.emplace("vulkan_seed_lookup_dispatches",
                    static_cast<double>(output.partial_vulkan_execution_proof
                                            .seed_lookup_dispatches));
    metrics.emplace("vulkan_chain_score_dispatches",
                    static_cast<double>(output.partial_vulkan_execution_proof
                                            .chain_score_dispatches));
    metrics.emplace("vulkan_seed_bitonic_sort_dispatches",
                    static_cast<double>(output.partial_vulkan_execution_proof
                                            .seed_bitonic_sort_dispatches));
    metrics.emplace("vulkan_seed_radix_sort_operations",
                    static_cast<double>(output.partial_vulkan_execution_proof
                                            .seed_radix_sort_operations));
    metrics.emplace("vulkan_seed_radix_anchor_count",
                    static_cast<double>(output.partial_vulkan_execution_proof
                                            .seed_radix_anchor_count));
    metrics.emplace("vulkan_seed_radix_passes",
                    static_cast<double>(output.partial_vulkan_execution_proof
                                            .seed_radix_passes));
    metrics.emplace("vulkan_seed_radix_histogram_dispatches",
                    static_cast<double>(output.partial_vulkan_execution_proof
                                            .seed_radix_histogram_dispatches));
    metrics.emplace("vulkan_seed_radix_scan_dispatches",
                    static_cast<double>(output.partial_vulkan_execution_proof
                                            .seed_radix_scan_dispatches));
    metrics.emplace("vulkan_seed_radix_scatter_dispatches",
                    static_cast<double>(output.partial_vulkan_execution_proof
                                            .seed_radix_scatter_dispatches));
    metrics.emplace("vulkan_seed_host_sort_fallbacks",
                    static_cast<double>(output.partial_vulkan_execution_proof
                                            .seed_host_sort_fallbacks));
    const auto& gpu_index = output.partial_vulkan_execution_proof;
    metrics.emplace("gpu_index_materializations",
                    static_cast<double>(gpu_index.gpu_index_materializations));
    metrics.emplace("gpu_index_open_addressed_materializations",
                    static_cast<double>(
                        gpu_index.gpu_index_open_addressed_materializations));
    metrics.emplace("gpu_index_sorted_dense_materializations",
                    static_cast<double>(
                        gpu_index.gpu_index_sorted_dense_materializations));
    metrics.emplace("gpu_index_unique_keys",
                    static_cast<double>(gpu_index.gpu_index_unique_keys));
    metrics.emplace("gpu_index_slot_capacity",
                    static_cast<double>(gpu_index.gpu_index_slot_capacity));
    metrics.emplace("gpu_index_logical_resident_bytes",
                    static_cast<double>(
                        gpu_index.gpu_index_logical_resident_bytes));
    metrics.emplace("gpu_index_actual_allocation_bytes",
                    static_cast<double>(
                        gpu_index.gpu_index_actual_allocation_bytes));
    metrics.emplace("gpu_index_fully_touched_bytes",
                    static_cast<double>(
                        gpu_index.gpu_index_fully_touched_bytes));
    metrics.emplace("gpu_index_staging_bytes",
                    static_cast<double>(gpu_index.gpu_index_staging_bytes));
    metrics.emplace("gpu_index_permit_releases",
                    static_cast<double>(gpu_index.gpu_index_permit_releases));
    metrics.emplace("gpu_index_process_footprint_reflections",
                    static_cast<double>(
                        gpu_index.gpu_index_process_footprint_reflections));
    metrics.emplace("gpu_index_reflected_footprint_bytes",
                    static_cast<double>(
                        gpu_index.gpu_index_reflected_footprint_bytes));
    metrics.emplace("gpu_index_max_load_ppm",
                    static_cast<double>(gpu_index.gpu_index_max_load_ppm));
    metrics.emplace("gpu_index_max_probe",
                    static_cast<double>(gpu_index.gpu_index_max_probe));
    metrics.emplace("gpu_index_count_phase_admissions",
        static_cast<double>(gpu_index.gpu_index_count_phase_admissions));
    metrics.emplace("gpu_index_main_peak_admissions",
        static_cast<double>(gpu_index.gpu_index_main_peak_admissions));
    metrics.emplace("gpu_index_resident_transfers",
        static_cast<double>(gpu_index.gpu_index_resident_transfers));
    metrics.emplace("gpu_index_adoption_rereserve_attempts",
        static_cast<double>(gpu_index.gpu_index_adoption_rereserve_attempts));
    metrics.emplace("gpu_index_adoption_successes",
        static_cast<double>(gpu_index.gpu_index_adoption_successes));
    metrics.emplace("gpu_index_owned_resident_bytes",
        static_cast<double>(gpu_index.gpu_index_owned_resident_bytes));
    metrics.emplace("gpu_index_gate_enabled",
        static_cast<double>(gpu_index.gpu_index_gate_enabled));
    metrics.emplace("gpu_index_gate_word_count",
        static_cast<double>(gpu_index.gpu_index_gate_word_count));
    metrics.emplace("gpu_index_gate_mask",
        static_cast<double>(gpu_index.gpu_index_gate_mask));
    metrics.emplace("gpu_index_gate_set_bits",
        static_cast<double>(gpu_index.gpu_index_gate_set_bits));
    metrics.emplace("gpu_index_gate_count_dispatches",
        static_cast<double>(gpu_index.gpu_index_gate_count_dispatches));
    metrics.emplace("gpu_index_gated_emit_dispatches",
        static_cast<double>(gpu_index.gpu_index_gated_emit_dispatches));
    metrics.emplace("gpu_index_ungated_emit_dispatches",
        static_cast<double>(gpu_index.gpu_index_ungated_emit_dispatches));
    metrics.emplace("gpu_index_retained_minimizer_count",
        static_cast<double>(gpu_index.gpu_index_retained_minimizer_count));
    metrics.emplace("gpu_index_distinct_minimizer_count",
        static_cast<double>(gpu_index.gpu_index_distinct_minimizer_count));
    metrics.emplace("post_seed_owned_gpu_cache_evictions",
        static_cast<double>(
            gpu_index.post_seed_owned_gpu_cache_evictions));
    metrics.emplace("post_seed_owned_gpu_resident_bytes_released",
        static_cast<double>(
            gpu_index.post_seed_owned_gpu_resident_bytes_released));
    metrics.emplace("post_seed_owned_cpu_index_releases",
        static_cast<double>(
            gpu_index.post_seed_owned_cpu_index_releases));
    metrics.emplace("post_seed_owned_cpu_position_bytes_released",
        static_cast<double>(
            gpu_index.post_seed_owned_cpu_position_bytes_released));
    if (output.vulkan_hardware_unavailable) {
        const double not_measured = std::numeric_limits<double>::quiet_NaN();
        metrics.emplace("match_count", not_measured);
        metrics.emplace("primary_correct_fraction", not_measured);
        metrics.emplace("mean_position_offset_bp", not_measured);
        return metrics;
    }
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

[[nodiscard]] nlohmann::json two_stack_proof_json(
    const TwoStackExecutionProof& proof, bool present)
{
#define BIOSEQ_PROOF_FIELD(name) {#name, proof.name}
    return nlohmann::json{
        {"present", present},
        BIOSEQ_PROOF_FIELD(bp_consumed),
        BIOSEQ_PROOF_FIELD(minimizers_emitted),
        BIOSEQ_PROOF_FIELD(vector_batches),
        BIOSEQ_PROOF_FIELD(vector_lanes_visited),
        BIOSEQ_PROOF_FIELD(masked_tail_lanes),
        BIOSEQ_PROOF_FIELD(bridge_symbols_staged),
        BIOSEQ_PROOF_FIELD(bridge_vector_runs),
        BIOSEQ_PROOF_FIELD(consecutive_kmer_batches),
        BIOSEQ_PROOF_FIELD(initial_kmer_vector_batches),
        BIOSEQ_PROOF_FIELD(rolling_kmer_vector_batches),
        BIOSEQ_PROOF_FIELD(canonical_scramble_batches),
        BIOSEQ_PROOF_FIELD(window_reduction_batches),
        BIOSEQ_PROOF_FIELD(event_generation_batches),
        BIOSEQ_PROOF_FIELD(stable_compress_batches),
        BIOSEQ_PROOF_FIELD(direct_event_compress_batches),
        BIOSEQ_PROOF_FIELD(tie_matrix_event_batches),
        BIOSEQ_PROOF_FIELD(row_transpose_batches),
        BIOSEQ_PROOF_FIELD(row_vector_stores),
        BIOSEQ_PROOF_FIELD(palindrome_lanes),
        BIOSEQ_PROOF_FIELD(first_window_tie_events),
        BIOSEQ_PROOF_FIELD(expiry_tie_events),
        BIOSEQ_PROOF_FIELD(entering_min_transitions),
        BIOSEQ_PROOF_FIELD(expiry_rescan_transitions),
        BIOSEQ_PROOF_FIELD(final_flush_records),
        BIOSEQ_PROOF_FIELD(fragment_symbols_staged),
        BIOSEQ_PROOF_FIELD(packed_decode_batches),
        BIOSEQ_PROOF_FIELD(packed_decode_lanes),
        BIOSEQ_PROOF_FIELD(ambiguous_symbols),
        BIOSEQ_PROOF_FIELD(ambiguous_reset_batches),
        BIOSEQ_PROOF_FIELD(packed_word_boundary_merges),
        BIOSEQ_PROOF_FIELD(scalar_orchestration_steps),
        BIOSEQ_PROOF_FIELD(fail_closed_rejections),
        BIOSEQ_PROOF_FIELD(scalar_backend_branches),
        BIOSEQ_PROOF_FIELD(scalar_tail_symbols),
        BIOSEQ_PROOF_FIELD(scalar_hash_symbols),
        BIOSEQ_PROOF_FIELD(scalar_window_transitions),
        BIOSEQ_PROOF_FIELD(scalar_event_records),
        BIOSEQ_PROOF_FIELD(scalar_sort_records),
        BIOSEQ_PROOF_FIELD(scalar_dedup_records),
        BIOSEQ_PROOF_FIELD(saturated),
    };
#undef BIOSEQ_PROOF_FIELD
}

[[nodiscard]] nlohmann::json simd_chaining_proof_json(
    const SimdChainingExecutionProof& proof, bool present)
{
#define BIOSEQ_PROOF_FIELD(name) {#name, proof.name}
    return nlohmann::json{
        {"present", present},
        BIOSEQ_PROOF_FIELD(anchors_consumed),
        BIOSEQ_PROOF_FIELD(score_vector_batches),
        BIOSEQ_PROOF_FIELD(score_vector_lanes),
        BIOSEQ_PROOF_FIELD(score_masked_lanes),
        BIOSEQ_PROOF_FIELD(descending_candidate_steps),
        BIOSEQ_PROOF_FIELD(visited_marks),
        BIOSEQ_PROOF_FIELD(max_chain_skip_stops),
        BIOSEQ_PROOF_FIELD(global_best_vector_batches),
        BIOSEQ_PROOF_FIELD(global_best_vector_lanes),
        BIOSEQ_PROOF_FIELD(backtrack_rank_batches),
        BIOSEQ_PROOF_FIELD(backtrack_path_vector_batches),
        BIOSEQ_PROOF_FIELD(backtrack_commit_vector_batches),
        BIOSEQ_PROOF_FIELD(backtrack_vector_lanes),
        BIOSEQ_PROOF_FIELD(backtrack_overlap_stops),
        BIOSEQ_PROOF_FIELD(scalar_orchestration_steps),
        BIOSEQ_PROOF_FIELD(fail_closed_rejections),
        BIOSEQ_PROOF_FIELD(scalar_score_branches),
        BIOSEQ_PROOF_FIELD(scalar_backtrack_branches),
        BIOSEQ_PROOF_FIELD(shared_scalar_backtrack_calls),
        BIOSEQ_PROOF_FIELD(saturated),
    };
#undef BIOSEQ_PROOF_FIELD
}

[[nodiscard]] nlohmann::json block_aligner_proof_json(
    const BlockAlignerExecutionProof& proof, bool present)
{
#define BIOSEQ_PROOF_FIELD(name) {#name, proof.name}
    return nlohmann::json{
        {"present", present},
        BIOSEQ_PROOF_FIELD(align_region_invocations),
        BIOSEQ_PROOF_FIELD(serial_align_region_dispatches),
        BIOSEQ_PROOF_FIELD(parallel_align_region_dispatches),
        BIOSEQ_PROOF_FIELD(routed_region_scopes),
        BIOSEQ_PROOF_FIELD(left_route_candidates),
        BIOSEQ_PROOF_FIELD(gap_route_candidates),
        BIOSEQ_PROOF_FIELD(right_route_candidates),
        BIOSEQ_PROOF_FIELD(left_eligible),
        BIOSEQ_PROOF_FIELD(left_executed),
        BIOSEQ_PROOF_FIELD(gap_eligible),
        BIOSEQ_PROOF_FIELD(gap_executed),
        BIOSEQ_PROOF_FIELD(right_eligible),
        BIOSEQ_PROOF_FIELD(right_executed),
        BIOSEQ_PROOF_FIELD(global_calls),
        BIOSEQ_PROOF_FIELD(native_xdrop_calls),
        BIOSEQ_PROOF_FIELD(native_xdrop_terminations),
        BIOSEQ_PROOF_FIELD(zero_work_calls),
        BIOSEQ_PROOF_FIELD(nonempty_calls),
        BIOSEQ_PROOF_FIELD(nonempty_calls_with_simd),
        BIOSEQ_PROOF_FIELD(trace_calls),
        BIOSEQ_PROOF_FIELD(executed_trace_calls),
        BIOSEQ_PROOF_FIELD(forward_input_calls),
        BIOSEQ_PROOF_FIELD(reversed_input_calls),
        BIOSEQ_PROOF_FIELD(executed_global_calls),
        BIOSEQ_PROOF_FIELD(executed_native_xdrop_calls),
        BIOSEQ_PROOF_FIELD(executed_forward_input_calls),
        BIOSEQ_PROOF_FIELD(executed_reversed_input_calls),
        BIOSEQ_PROOF_FIELD(simd_lane8_calls),
        BIOSEQ_PROOF_FIELD(simd_lane16_calls),
        BIOSEQ_PROOF_FIELD(simd_dp_vector_batches),
        BIOSEQ_PROOF_FIELD(simd_dp_cells),
        BIOSEQ_PROOF_FIELD(adaptive_grow_events),
        BIOSEQ_PROOF_FIELD(adaptive_shrink_events),
        BIOSEQ_PROOF_FIELD(expected_reversed_compact_symbols),
        BIOSEQ_PROOF_FIELD(reversed_compact_symbols),
        BIOSEQ_PROOF_FIELD(config_identity_calls),
        BIOSEQ_PROOF_FIELD(config_identity_xor),
        BIOSEQ_PROOF_FIELD(config_identity_sum),
        BIOSEQ_PROOF_FIELD(executed_config_identity_calls),
        BIOSEQ_PROOF_FIELD(executed_config_identity_xor),
        BIOSEQ_PROOF_FIELD(executed_config_identity_sum),
        BIOSEQ_PROOF_FIELD(unsupported_contract_rejections),
        BIOSEQ_PROOF_FIELD(forbidden_ksw2_calls),
        BIOSEQ_PROOF_FIELD(forbidden_scalar_biological_fallbacks),
        BIOSEQ_PROOF_FIELD(saturated),
    };
#undef BIOSEQ_PROOF_FIELD
}

/// Lossless per-repeat evidence for the statistically paired XXL companion.
/// The framework adds algorithm/test-case/repeat/schedule/wall-clock fields
/// and durably flushes the resulting object. Keeping the route proof beside
/// every duration prevents a fast sample from being accepted after a silent
/// backend change; repeat zero is also summarized separately as first-repeat
/// evidence by the framework raw manifest (not claimed globally cold).
[[nodiscard]] std::string serialize_xxl_paired_sample(
    std::string_view /*algo*/,
    const ParamMap& /*params*/,
    const AlignerInput& /*input*/,
    const AlignerOutput& output)
{
    const auto& proof = output.partial_vulkan_execution_proof;
    return nlohmann::json{
        {"payload_schema", "bioseq.xxl_hifi_paired.route_proof"},
        {"payload_schema_version", 2},
        {"total_pairs", output.total_pairs},
        {"total_results", output.total_results},
        {"correct_pairs", output.correct_pairs},
        {"selected_route_execution_proof_accepted",
            output.selected_route_execution_proof_accepted},
        {"historical_partial_gpu_probe",
            output.historical_partial_gpu_probe},
        {"all_gpu_acceptance_eligible",
            output.all_gpu_acceptance_eligible},
        {"phase_c_gpu_index_ownership_proof_accepted",
            output.phase_c_gpu_index_ownership_proof_accepted},
        {"exact_simd_query_execution_proof", two_stack_proof_json(
            output.exact_simd_query_execution_proof,
            output.exact_simd_query_execution_proof_present)},
        {"exact_simd_target_execution_proof", two_stack_proof_json(
            output.exact_simd_target_execution_proof,
            output.exact_simd_target_execution_proof_present)},
        {"simd_chaining_execution_proof", simd_chaining_proof_json(
            output.simd_chaining_execution_proof,
            output.simd_chaining_execution_proof_present)},
        {"block_aligner_execution_proof", block_aligner_proof_json(
            output.block_aligner_execution_proof,
            output.block_aligner_execution_proof_present)},
        {"partial_vulkan_execution_proof", {
            {"query_sketch_dispatches", proof.query_sketch_dispatches},
            {"target_sketch_dispatches", proof.target_sketch_dispatches},
            {"target_index_build_dispatches",
                proof.target_index_build_dispatches},
            {"seed_lookup_dispatches", proof.seed_lookup_dispatches},
            {"chain_score_dispatches", proof.chain_score_dispatches},
            {"seed_bitonic_sort_dispatches",
                proof.seed_bitonic_sort_dispatches},
            {"seed_radix_sort_operations", proof.seed_radix_sort_operations},
            {"seed_radix_anchor_count", proof.seed_radix_anchor_count},
            {"seed_radix_passes", proof.seed_radix_passes},
            {"seed_radix_histogram_dispatches",
                proof.seed_radix_histogram_dispatches},
            {"seed_radix_scan_dispatches",
                proof.seed_radix_scan_dispatches},
            {"seed_radix_scatter_dispatches",
                proof.seed_radix_scatter_dispatches},
            {"seed_host_sort_fallbacks", proof.seed_host_sort_fallbacks},
            {"gpu_index_materializations", proof.gpu_index_materializations},
            {"gpu_index_open_addressed_materializations",
                proof.gpu_index_open_addressed_materializations},
            {"gpu_index_sorted_dense_materializations",
                proof.gpu_index_sorted_dense_materializations},
            {"gpu_index_unique_keys", proof.gpu_index_unique_keys},
            {"gpu_index_slot_capacity", proof.gpu_index_slot_capacity},
            {"gpu_index_logical_resident_bytes",
                proof.gpu_index_logical_resident_bytes},
            {"gpu_index_actual_allocation_bytes",
                proof.gpu_index_actual_allocation_bytes},
            {"gpu_index_fully_touched_bytes",
                proof.gpu_index_fully_touched_bytes},
            {"gpu_index_staging_bytes", proof.gpu_index_staging_bytes},
            {"gpu_index_permit_releases", proof.gpu_index_permit_releases},
            {"gpu_index_process_footprint_reflections",
                proof.gpu_index_process_footprint_reflections},
            {"gpu_index_reflected_footprint_bytes",
                proof.gpu_index_reflected_footprint_bytes},
            {"gpu_index_max_load_ppm", proof.gpu_index_max_load_ppm},
            {"gpu_index_max_probe", proof.gpu_index_max_probe},
            {"gpu_index_count_phase_admissions",
                proof.gpu_index_count_phase_admissions},
            {"gpu_index_main_peak_admissions",
                proof.gpu_index_main_peak_admissions},
            {"gpu_index_resident_transfers",
                proof.gpu_index_resident_transfers},
            {"gpu_index_adoption_rereserve_attempts",
                proof.gpu_index_adoption_rereserve_attempts},
            {"gpu_index_adoption_successes",
                proof.gpu_index_adoption_successes},
            {"gpu_index_owned_resident_bytes",
                proof.gpu_index_owned_resident_bytes},
            {"gpu_index_gate_enabled", proof.gpu_index_gate_enabled},
            {"gpu_index_gate_word_count", proof.gpu_index_gate_word_count},
            {"gpu_index_gate_mask", proof.gpu_index_gate_mask},
            {"gpu_index_gate_set_bits", proof.gpu_index_gate_set_bits},
            {"gpu_index_gate_digest_lo", proof.gpu_index_gate_digest_lo},
            {"gpu_index_gate_digest_hi", proof.gpu_index_gate_digest_hi},
            {"gpu_index_gate_count_dispatches",
                proof.gpu_index_gate_count_dispatches},
            {"gpu_index_gated_emit_dispatches",
                proof.gpu_index_gated_emit_dispatches},
            {"gpu_index_ungated_emit_dispatches",
                proof.gpu_index_ungated_emit_dispatches},
            {"gpu_index_retained_minimizer_count",
                proof.gpu_index_retained_minimizer_count},
            {"gpu_index_distinct_minimizer_count",
                proof.gpu_index_distinct_minimizer_count},
            {"post_seed_owned_gpu_cache_evictions",
                proof.post_seed_owned_gpu_cache_evictions},
            {"post_seed_owned_gpu_resident_bytes_released",
                proof.post_seed_owned_gpu_resident_bytes_released},
            {"post_seed_owned_cpu_index_releases",
                proof.post_seed_owned_cpu_index_releases},
            {"post_seed_owned_cpu_position_bytes_released",
                proof.post_seed_owned_cpu_position_bytes_released},
            {"saturated", proof.saturated},
        }},
    }.dump();
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
    // `std::function` parameter without help. Reused across the six blocks
    // below since all six share the same I/O types and lambdas.
    auto estimate = std::function<RunConstraints(
        std::string_view, std::string_view, simdjson::dom::element)>{
            estimate_aligner_resources};
    auto build = std::function<AlignerInput(
        std::string_view, std::string_view, simdjson::dom::element,
        const RunConstraints&)>{build_input};
    auto run = std::function<AlignerOutput(
        std::string_view, const ParamMap&, const AlignerInput&)>{run_aligner};
    auto run_strict_xxl = std::function<AlignerOutput(
        std::string_view, const ParamMap&, const AlignerInput&)>{
            run_aligner_strict_xxl};
    auto run_strict_xxl_paired = std::function<AlignerOutput(
        std::string_view, const ParamMap&, const AlignerInput&)>{
            run_aligner_strict_xxl_paired};
    auto eval = std::function<std::map<std::string, double>(
        std::string_view, const ParamMap&,
        const AlignerInput&, const AlignerOutput&)>{evaluate_aligner};
    auto serialize_paired = std::function<std::string(
        std::string_view, const ParamMap&,
        const AlignerInput&, const AlignerOutput&)>{
            serialize_xxl_paired_sample};

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
            "bench_aligners_curated", config_path, estimate, build, run, eval),
        create_benchmark<AlignerInput, AlignerOutput>(
            "XXL_HIFI report_76 relative-performance gate",
            "bench_aligners_xxl_hifi_relative",
            config_path, estimate, build, run_strict_xxl, eval),
        create_benchmark<AlignerInput, AlignerOutput>(
            "XXL_HIFI paired relative-performance companion",
            "bench_aligners_xxl_hifi_paired",
            config_path, estimate, build, run_strict_xxl_paired, eval,
            {}, serialize_paired));
}
