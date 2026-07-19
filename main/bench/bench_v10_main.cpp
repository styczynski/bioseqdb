/**
 * @file bench_v10_main.cpp
 * @brief Aligner bench entry point on top of bench_framework (V10).
 *
 * Replaces the legacy Catch2-based harness in `bench/bench_main.cpp` and
 * its surrounding `bench/aligners/` + `bench/bench_scheduler/` trees.
 * One TU per binary, three lambdas (`build_input` / `run_aligner` /
 * `evaluate_aligner`), one `bioseq::bench_framework::run(...)` call.
 *
 * Benchmark blocks declared in `bench_v10_configuration.json` include:
 *   * bench_grouped32_local32_validation — benchmark-only contiguous u32 validation POC
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
#include "prefilter_pipeline_metrics.hpp"

#include <simdjson.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

import bioseq.common.mlogging;
import bioseq.common.context;
import bioseq.common.sequan_compat;
import bioseq.simd.utils;
import bioseq.simd.vector_stream;
import bioseq.sequences;
import bioseq.sequences.edit_distance_threshold;
import bioseq.sequences.kmers.two_stacks;
import bioseq.sequences.chaining.simd;
import bioseq.sequences.chaining.utils;
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

namespace bioseq::bench_prefilters {
[[nodiscard]] bioseq::bench_framework::ErasedBenchmark
make_prefilter_kernel_benchmark(std::string config_path);
[[nodiscard]] bioseq::bench_framework::ErasedBenchmark
make_prefilter_small_seed_benchmark(std::string config_path);
}

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
using bioseq::simd::HWY_NAMESPACE::MatchResult;
using bioseq::simd::HWY_NAMESPACE::MatchResultBiologicalFingerprint;
using bioseq::simd::HWY_NAMESPACE::MatchResultCollection;
using bioseq::simd::HWY_NAMESPACE::Minimap2Params;
using bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType;
using bioseq::prefilter_stats::PrefilterBackendType;
using bioseq::prefilter_stats::PrefilterExecutionProof;
using bioseq::simd::HWY_NAMESPACE::PartialVulkanExecutionProof;
using bioseq::simd::HWY_NAMESPACE::TwoStackExecutionProof;
using bioseq::simd::HWY_NAMESPACE::PackedPaperNthashExecutionProof;
using bioseq::simd::HWY_NAMESPACE::SimdChainingExecutionProof;
using bioseq::simd::HWY_NAMESPACE::BlockAlignerExecutionProof;
using bioseq::simd::HWY_NAMESPACE::ScalarPipelineExecutionProof;
using bioseq::simd::HWY_NAMESPACE::Ksw2BatchedExecutionProof;
using bioseq::chaining::SimdChainingPolicy;
namespace gen = bioseq::sequences::generation;
using bench_generator_t = gen::alphabet_sequence_generator<seq::dna4, std::uint32_t>;

// Comparable mappings may differ from the reference coordinate, but the
// contract is strict: a delta of 15 bp is already a rejection.
constexpr std::int32_t kPositionTolerance = 15;

struct PrefilterPipelineMatchCountParityState {
    struct ThresholdState {
        bool prefilter_off_scheduled = false;
        bool vulkan_scheduled = false;
        std::optional<std::size_t> prefilter_off_match_count;
        std::optional<std::size_t> vulkan_match_count;
        std::optional<bool> vulkan_hardware_available;
        bool comparison_performed = false;
        bool comparison_passed = false;
    };

    std::mutex mutex;
    std::map<std::uint32_t, ThresholdState> by_threshold;
};

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
    /// Shared because the framework may move/copy an input while every timed
    /// execution tuple for one batch must contribute to the same zero-work
    /// parity ledger. The evaluator is the only writer.
    std::shared_ptr<PrefilterPipelineMatchCountParityState>
        prefilter_pipeline_match_count_parity =
            std::make_shared<PrefilterPipelineMatchCountParityState>();
};

struct AlignerOutput {
    std::size_t total_results          = 0;
    std::size_t correct_pairs          = 0;
    std::size_t total_pairs            = 0;
    std::vector<std::uint8_t> correct_pair_flags;
    double      sum_abs_position_offset = 0.0;
    bool        vulkan_hardware_unavailable = false;
    /// The three Vulkan-labelled rows in this historical report_76 matrix are
    /// deliberately mixed pipelines. None is evidence for the user's
    /// all-on-GPU acceptance condition.
    bool        historical_partial_gpu_probe = false;
    bool        all_gpu_acceptance_eligible = false;
    bool        edit_threshold_enforced = false;
    std::uint32_t edit_threshold = 0u;
    bool        prefilter_enabled = false;
    PrefilterBackendType prefilter_backend = PrefilterBackendType::AUTO;
    PrefilterExecutionProof
        prefilter_execution_proof{};
    bool prefilter_execution_proof_present = false;
    bool prefilter_execution_proof_accepted = false;
    bool        phase_c_gpu_index_ownership_proof_accepted = false;
    /// True only when every completed pair in a source-direct row passed the
    /// single-operation validator before its proof was folded into the row
    /// aggregate.  The aggregate counters are additive and therefore cannot
    /// be reinterpreted as one operation after a multi-pair run.
    bool        source_direct_gpu_index_materialization_proof_accepted = false;
    PartialVulkanExecutionProof partial_vulkan_execution_proof{};
    MatchResultBiologicalFingerprint biological_fingerprint{};
    TwoStackExecutionProof exact_simd_query_execution_proof{};
    TwoStackExecutionProof exact_simd_target_execution_proof{};
    PackedPaperNthashExecutionProof paper_nthash_query_execution_proof{};
    PackedPaperNthashExecutionProof paper_nthash_target_execution_proof{};
    SimdChainingExecutionProof simd_chaining_execution_proof{};
    BlockAlignerExecutionProof block_aligner_execution_proof{};
    ScalarPipelineExecutionProof scalar_pipeline_execution_proof{};
    Ksw2BatchedExecutionProof ksw2_batched_execution_proof{};
    bool exact_simd_query_execution_proof_present = false;
    bool exact_simd_target_execution_proof_present = false;
    bool paper_nthash_query_execution_proof_present = false;
    bool paper_nthash_target_execution_proof_present = false;
    bool simd_chaining_execution_proof_present = false;
    bool block_aligner_execution_proof_present = false;
    bool scalar_pipeline_execution_proof_present = false;
    bool ksw2_batched_execution_proof_present = false;
    /// Requested policy is recorded beside the operation-originated proof.
    /// It is never inferred from elapsed time. Non-SIMD rows leave
    /// `simd_chaining_policy_selected` false and are forbidden from carrying
    /// SIMD chaining proof.
    SimdChainingPolicy selected_simd_chaining_policy =
        SimdChainingPolicy::exact_visited;
    bool simd_chaining_policy_selected = false;
    /// Set only after `require_selected_route_execution` accepts the
    /// operation-originated proof for every processed pair. The strict XXL
    /// raw companion persists this beside each duration.
    bool        selected_route_execution_proof_accepted = false;
};

using bench_pair_t = typename bench_generator_t::query_target_pair;
using bench_sequence_t = typename bench_generator_t::seq_t;

/// One fixed endpoint-corpus item. Every backend receives this exact query and
/// this exact single target; `target_ordinal` is compared before coordinates so
/// a future multi-target adapter cannot accidentally compare different loci.
struct EndpointQualificationFixture {
    std::string name;
    std::uint64_t target_ordinal = 0;
    mutable bench_pair_t pair{};
    std::string query_acgt;
    std::string target_acgt;
    float chain_skip_scale = 0.0f;
    std::int32_t max_chain_skip = 25;
};

struct EndpointQualificationInput {
    mutable std::vector<EndpointQualificationFixture> fixtures;
};

struct EndpointCoordinatesEvidence {
    bool present = false;
    std::uint64_t query_len = 0;
    std::uint64_t query_start = 0;
    std::uint64_t query_end = 0;
    bool is_reverse = false;
    std::uint64_t target_len = 0;
    std::uint64_t target_start = 0;
    std::uint64_t target_end = 0;
};

struct EndpointBackendOutcome {
    MatchResultCollection matches;
    // MatchResult has no reference-id field. The single-target adapter binds
    // this source-corpus ordinal at invocation and carries it beside results.
    std::uint64_t target_ordinal = 0;
};

struct EndpointFixtureComparison {
    std::string fixture_name;
    std::uint64_t target_ordinal = 0;
    std::uint64_t bounded_primary_count = 0;
    std::uint64_t exact_primary_count = 0;
    std::uint64_t minimap2_primary_count = 0;
    std::uint64_t bounded_target_ordinal = 0;
    std::uint64_t exact_target_ordinal = 0;
    std::uint64_t minimap2_target_ordinal = 0;
    bool normalized_coordinates = false;
    bool identity_and_strand_match = false;
    std::uint64_t delta_vs_exact_bp = 0;
    std::uint64_t delta_vs_minimap2_bp = 0;
    EndpointCoordinatesEvidence bounded_endpoint{};
    EndpointCoordinatesEvidence exact_endpoint{};
    EndpointCoordinatesEvidence minimap2_endpoint{};
    std::uint64_t bounded_cutoff_stops = 0;
    std::uint64_t bounded_candidates_skipped = 0;
    std::uint64_t bounded_rescue_probes = 0;
    std::uint64_t bounded_rescue_wins = 0;
    bool bounded_fixture_proof_accepted = false;
    bool deterministic_rescue_fixture = false;
    bool bounded_route_proof_accepted = false;
    bool exact_route_proof_accepted = false;
    bool minimap2_route_proof_accepted = false;
};

/// Blocking endpoint qualification from DOC0372. A delta of exactly 15 bp is
/// a failure; no mean or percentile can hide one outlier.
struct SimdChainingEndpointQualification {
    std::uint64_t fixtures = 0;
    std::uint64_t mappings_compared = 0;
    std::uint64_t missing_primary = 0;
    std::uint64_t ambiguous_primary = 0;
    std::uint64_t reference_or_strand_mismatches = 0;
    std::uint64_t invalid_normalized_coordinates = 0;
    std::uint64_t max_delta_vs_exact_bp = 0;
    std::uint64_t max_delta_vs_minimap2_bp = 0;
    bool saturated = false;

    SimdChainingExecutionProof exact_policy_proof{};
    SimdChainingExecutionProof bounded_policy_proof{};
    bool exact_policy_proof_present = false;
    bool bounded_policy_proof_present = false;
    bool exact_route_proof_accepted = false;
    bool bounded_route_proof_accepted = false;
    bool minimap2_route_proof_accepted = false;
    std::uint64_t deterministic_rescue_fixture_runs = 0;
    std::uint64_t deterministic_rescue_fixture_mappings_compared = 0;
    std::vector<EndpointFixtureComparison> comparisons;

    [[nodiscard]] constexpr bool accepted() const noexcept {
        return !saturated && fixtures > 0 && mappings_compared == fixtures
            && missing_primary == 0 && ambiguous_primary == 0
            && reference_or_strand_mismatches == 0
            && invalid_normalized_coordinates == 0
            && max_delta_vs_exact_bp < 15
            && max_delta_vs_minimap2_bp < 15;
    }

    [[nodiscard]] bool proof_accepted() const noexcept {
        return exact_route_proof_accepted && bounded_route_proof_accepted
            && minimap2_route_proof_accepted
            && exact_policy_proof_present && bounded_policy_proof_present
            && exact_policy_proof.accepted(
                SimdChainingPolicy::exact_visited, true)
            && bounded_policy_proof.accepted(
                SimdChainingPolicy::bounded_batch_approximate, true)
            && bounded_policy_proof.policy.bounded_cutoff_stops > 0
            && bounded_policy_proof.policy.bounded_candidates_skipped > 0
            && rescue_evidence_accepted()
            && bounded_policy_proof.scalar_score_branches == 0
            && bounded_policy_proof.scalar_backtrack_branches == 0
            && bounded_policy_proof.shared_scalar_backtrack_calls == 0;
    }

    [[nodiscard]] constexpr bool rescue_evidence_accepted() const noexcept {
        return deterministic_rescue_fixture_runs == 1
            && deterministic_rescue_fixture_mappings_compared == 1;
    }
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

[[nodiscard]] std::uint32_t read_max_edits_param(
    const ParamMap& params,
    std::uint32_t fallback = 40u) noexcept
{
    const auto found = params.find("max_edits");
    if (found == params.end() || !found->second.is_integer()) {
        return fallback;
    }
    const std::int64_t raw = found->second.as_integer();
    if (raw < 0) {
        return fallback;
    }
    return static_cast<std::uint32_t>(std::min<std::int64_t>(
        raw,
        static_cast<std::int64_t>(
            (std::numeric_limits<std::uint32_t>::max)())));
}

enum class EndpointDecoyPlacement : std::uint8_t {
    none = 0,
    before_true_locus,
    after_true_locus,
};

struct EndpointFixtureSpec {
    std::string_view name;
    std::size_t target_bp;
    std::size_t query_bp;
    double substitution_rate;
    double insertion_rate;
    double deletion_rate;
    double reverse_rate;
    std::uint64_t seed;
    float chain_skip_scale;
    EndpointDecoyPlacement decoy;
    std::int32_t max_chain_skip = 25;
};

// Fixed before any result is observed. The four nearby short exact-read sizes
// are empirical tail-size probes; they do not claim a source-level anchor
// count. The two constructed decoys put the true locus on opposite sides of a
// near-copy, the rescue item has its own structural construction below, and
// the remaining fixed-seed items span strand, penalty, error, and size buckets
// through the report-76 XXL seed. Every target is <= INT32_MAX because bundled
// minimap2 narrows its native ABI.
inline constexpr std::array<EndpointFixtureSpec, 16>
    kEndpointQualificationSpecs{{
        {"short_exact_1021bp", 12'000u, 1'021u,
            0.0, 0.0, 0.0, 0.0, 3'720'001u, 0.0f,
            EndpointDecoyPlacement::none},
        {"short_exact_1030bp", 12'000u, 1'030u,
            0.0, 0.0, 0.0, 0.0, 3'720'002u, 0.0f,
            EndpointDecoyPlacement::none},
        {"short_exact_1039bp", 12'000u, 1'039u,
            0.0, 0.0, 0.0, 0.0, 3'720'003u, 0.0f,
            EndpointDecoyPlacement::none},
        {"short_exact_1067bp", 12'000u, 1'067u,
            0.0, 0.0, 0.0, 0.0, 3'720'004u, 0.0f,
            EndpointDecoyPlacement::none},
        {"repetitive_decoy_true_after", 120'000u, 12'000u,
            0.0, 0.0, 0.0, 0.0, 3'720'005u, 0.0f,
            EndpointDecoyPlacement::before_true_locus},
        {"repetitive_decoy_true_before", 120'000u, 12'000u,
            0.0, 0.0, 0.0, 0.0, 3'720'006u, 0.0f,
            EndpointDecoyPlacement::after_true_locus},
        {"reverse_strand_fixed_seed", 80'000u, 8'000u,
            0.001, 0.0005, 0.0005, 1.0, 3'720'007u, 0.0f,
            EndpointDecoyPlacement::none},
        {"max_skip_invalid_runs_and_rescue", 300'000u, 50'000u,
            0.005, 0.0125, 0.0125, 0.0, 3'720'008u, 0.0f,
            EndpointDecoyPlacement::none},
        {"nonzero_skip_penalty_fixed_seed", 400'000u, 70'000u,
            0.005, 0.005, 0.005, 0.0, 3'720'009u, 0.75f,
            EndpointDecoyPlacement::none},
        {"short_hifi", 3'000u, 300u,
            0.001, 0.0005, 0.0005, 0.0, 1'234'567u, 0.0f,
            EndpointDecoyPlacement::none},
        {"medium_ccs", 30'000u, 5'000u,
            0.005, 0.005, 0.005, 0.0, 8'901'234u, 0.0f,
            EndpointDecoyPlacement::none},
        {"long_modern_ont", 250'000u, 50'000u,
            0.005, 0.025, 0.020, 0.0, 6'543'210u, 0.0f,
            EndpointDecoyPlacement::none},
        {"long_substitution_heavy", 250'000u, 50'000u,
            0.05, 0.0005, 0.0005, 0.0, 7'654'321u, 0.0f,
            EndpointDecoyPlacement::none},
        {"long_indel_heavy", 250'000u, 30'000u,
            0.001, 0.02, 0.02, 0.0, 2'103'456u, 0.0f,
            EndpointDecoyPlacement::none},
        {"very_long_hifi", 1'500'000u, 1'000'000u,
            0.001, 0.0005, 0.0005, 0.0, 8'765'432u, 0.0f,
            EndpointDecoyPlacement::none},
        {"xxl_hifi_report76", 500'000'000u, 5'000'000u,
            0.001, 0.0005, 0.0005, 0.5, 3'219'876u, 0.0f,
            EndpointDecoyPlacement::none},
    }};

static_assert([] {
    for (const auto& spec : kEndpointQualificationSpecs) {
        constexpr auto max_minimap_coordinate =
            static_cast<std::size_t>(
                (std::numeric_limits<std::int32_t>::max)());
        if (spec.target_bp > max_minimap_coordinate
            || spec.query_bp > max_minimap_coordinate) {
            return false;
        }
    }
    return true;
}(), "bundled minimap2 endpoint fixtures must fit its signed-32-bit ABI");

// --------------------------------------------------------------------------
// Grouped32 local-position validation experiment
// --------------------------------------------------------------------------
//
// This is deliberately benchmark-only. It models the contiguous, non-scatter
// portion of PreparedGrouped32QueryReader::finish_load without changing the
// reader or exposing a benchmark selector to production. In particular, none
// of these variants performs a gather or constructs global u64 positions.

enum class Grouped32ValidationError : std::uint8_t {
    none = 0,
    byte_count,
    position_range,
    arithmetic_overflow,
};

enum class Grouped32ValidationRoute : std::uint8_t {
    scalar_exact = 1,
    scalar_optimized,
    scalar_unrolled,
    vector_stream_1,
    vector_stream_2,
    vector_stream_4,
};

struct Grouped32ValidationSpan {
    std::size_t byte_offset = 0;
    std::size_t position_count = 0;
    std::uint64_t segment_base_bp = 0;
    std::uint64_t segment_end_bp = 0;
};

struct Grouped32ValidationResult {
    Grouped32ValidationError error = Grouped32ValidationError::none;
    std::uint64_t first_error_word =
        (std::numeric_limits<std::uint64_t>::max)();
    std::uint64_t words_examined = 0;
    std::uint32_t terminal_word_witness = 0;
    Grouped32ValidationRoute selected_route =
        Grouped32ValidationRoute::scalar_exact;
    std::uint8_t selected_vector_stream_width = 0;
    std::uint64_t vectorized_words = 0;
    std::uint64_t scalar_boundary_words = 0;
    std::uint64_t scalar_tail_words = 0;
    std::uint64_t failure_replay_count = 0;

    [[nodiscard]] bool valid() const noexcept {
        return error == Grouped32ValidationError::none;
    }
};

struct Grouped32ValidationInput {
    std::vector<std::byte> encoded_bytes;
    std::vector<Grouped32ValidationSpan> spans;
    std::uint64_t total_words = 0;
    Grouped32ValidationResult exact_reference{};
};

struct Grouped32FastValidationResult {
    bool valid = true;
    std::uint64_t words_examined = 0;
    std::uint32_t terminal_word_witness = 0;
    Grouped32ValidationRoute selected_route =
        Grouped32ValidationRoute::scalar_optimized;
    std::uint8_t selected_vector_stream_width = 0;
    std::uint64_t vectorized_words = 0;
    std::uint64_t scalar_boundary_words = 0;
    std::uint64_t scalar_tail_words = 0;
};

inline constexpr std::uint64_t kGrouped32MaximumLocalBp = 0x7fffffffu;
inline constexpr std::size_t kGrouped32EncodedWordBytes = 4u;

/// Compare only the biological/validation semantics. Route proof is expected
/// to differ across implementations and is checked independently.
[[nodiscard]] constexpr bool same_grouped32_validation_semantics(
    const Grouped32ValidationResult& lhs,
    const Grouped32ValidationResult& rhs) noexcept
{
    return lhs.error == rhs.error
        && lhs.first_error_word == rhs.first_error_word
        && lhs.words_examined == rhs.words_examined
        && lhs.terminal_word_witness == rhs.terminal_word_witness;
}

template <std::size_t StreamWidth>
[[nodiscard]] consteval Grouped32ValidationRoute
grouped32_vector_route() noexcept {
    static_assert(StreamWidth == 1u
        || StreamWidth == 2u
        || StreamWidth == 4u);
    if constexpr (StreamWidth == 1u) {
        return Grouped32ValidationRoute::vector_stream_1;
    } else if constexpr (StreamWidth == 2u) {
        return Grouped32ValidationRoute::vector_stream_2;
    } else {
        return Grouped32ValidationRoute::vector_stream_4;
    }
}

/// Decode one persisted little-endian u32 without alignment or host-endian
/// assumptions. This is the exact scalar reference used for failure replay.
[[nodiscard, gnu::always_inline]] inline std::uint32_t
read_grouped32_word_le(
    std::span<const std::byte> bytes,
    std::size_t byte_offset) noexcept
{
    return std::to_integer<std::uint32_t>(bytes[byte_offset])
        | (std::to_integer<std::uint32_t>(bytes[byte_offset + 1u]) << 8u)
        | (std::to_integer<std::uint32_t>(bytes[byte_offset + 2u]) << 16u)
        | (std::to_integer<std::uint32_t>(bytes[byte_offset + 3u]) << 24u);
}

/// Fast scalar load after a caller has admitted the complete byte range.
/// memcpy is both alias-safe and optimized to one native load on little-endian
/// NEON/x86 hosts.
[[nodiscard, gnu::always_inline]] inline std::uint32_t
read_grouped32_word_native(
    std::span<const std::byte> bytes,
    std::size_t byte_offset) noexcept
{
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data() + byte_offset, sizeof(value));
    if constexpr (std::endian::native == std::endian::big) {
        return std::byteswap(value);
    } else if constexpr (std::endian::native == std::endian::little) {
        return value;
    } else {
        return read_grouped32_word_le(bytes, byte_offset);
    }
}

/// Exact current-reader semantics: byte bounds precede encoded ordering,
/// which precedes segment globalization. Optimized variants replay this whole
/// function after any fast-path failure so the first error remains exact.
[[nodiscard, gnu::noinline]] Grouped32ValidationResult
validate_grouped32_local32_scalar_exact_bench(
    const Grouped32ValidationInput& input) noexcept
{
    const std::span<const std::byte> bytes{input.encoded_bytes};
    Grouped32ValidationResult result;
    std::uint64_t logical_word = 0;

    for (const Grouped32ValidationSpan& span : input.spans) {
        bool has_previous = false;
        std::uint32_t previous = 0;
        for (std::size_t position_index = 0;
             position_index < span.position_count;
             ++position_index) {
            if (position_index
                > ((std::numeric_limits<std::size_t>::max)()
                    - span.byte_offset) / kGrouped32EncodedWordBytes) {
                result.error = Grouped32ValidationError::byte_count;
                result.first_error_word = logical_word + position_index;
                return result;
            }
            const std::size_t byte_offset = span.byte_offset
                + position_index * kGrouped32EncodedWordBytes;
            if (byte_offset > bytes.size()
                || kGrouped32EncodedWordBytes > bytes.size() - byte_offset) {
                result.error = Grouped32ValidationError::byte_count;
                result.first_error_word = logical_word + position_index;
                return result;
            }

            const std::uint32_t encoded =
                read_grouped32_word_le(bytes, byte_offset);
            result.words_examined = logical_word + position_index + 1u;
            result.terminal_word_witness = encoded;
            if (has_previous && encoded <= previous) {
                result.error = Grouped32ValidationError::position_range;
                result.first_error_word = logical_word + position_index;
                return result;
            }
            previous = encoded;
            has_previous = true;

            if (span.segment_base_bp >= span.segment_end_bp
                || span.segment_end_bp - span.segment_base_bp
                    > kGrouped32MaximumLocalBp + 1u) {
                result.error = Grouped32ValidationError::position_range;
                result.first_error_word = logical_word + position_index;
                return result;
            }
            const std::uint64_t local_bp = encoded >> 1u;
            if (local_bp
                > (std::numeric_limits<std::uint64_t>::max)()
                    - span.segment_base_bp) {
                result.error = Grouped32ValidationError::arithmetic_overflow;
                result.first_error_word = logical_word + position_index;
                return result;
            }
            if (span.segment_base_bp + local_bp >= span.segment_end_bp) {
                result.error = Grouped32ValidationError::position_range;
                result.first_error_word = logical_word + position_index;
                return result;
            }
        }
        if (span.position_count != 0u) {
            ++result.scalar_boundary_words;
            result.scalar_tail_words += span.position_count - 1u;
        }
        logical_word += span.position_count;
    }
    result.words_examined = logical_word;
    return result;
}

/// Admit the conditions that make the valid fast path equivalent to the exact
/// per-word globalization checks. A false result always triggers exact replay.
[[nodiscard, gnu::always_inline]] inline bool
grouped32_span_fast_path_admitted(
    std::span<const std::byte> bytes,
    const Grouped32ValidationSpan& span) noexcept
{
    if (span.segment_base_bp >= span.segment_end_bp
        || span.segment_end_bp - span.segment_base_bp
            > kGrouped32MaximumLocalBp + 1u
        || span.byte_offset > bytes.size()) {
        return false;
    }
    return span.position_count
        <= (bytes.size() - span.byte_offset) / kGrouped32EncodedWordBytes;
}

/// Hoisted scalar candidate. It is intentionally left auto-vectorizable: the
/// explicit Highway candidates must beat the compiler's best ordinary loop,
/// not a deliberately weakened scalar baseline.
[[nodiscard, gnu::always_inline]] inline Grouped32FastValidationResult
validate_grouped32_local32_scalar_optimized_fast(
    const Grouped32ValidationInput& input) noexcept
{
    const std::span<const std::byte> bytes{input.encoded_bytes};
    Grouped32FastValidationResult result;
    result.selected_route = Grouped32ValidationRoute::scalar_optimized;
    for (const Grouped32ValidationSpan& span : input.spans) {
        if (!grouped32_span_fast_path_admitted(bytes, span)) {
            result.valid = false;
            return result;
        }
        const auto segment_width = static_cast<std::uint32_t>(
            span.segment_end_bp - span.segment_base_bp);
        if (span.position_count == 0u) {
            continue;
        }
        std::uint32_t previous = read_grouped32_word_native(
            bytes, span.byte_offset);
        ++result.scalar_boundary_words;
        if ((previous >> 1u) >= segment_width) {
            result.valid = false;
            return result;
        }
        result.terminal_word_witness = previous;
        for (std::size_t position_index = 1;
             position_index < span.position_count;
             ++position_index) {
            const std::uint32_t encoded = read_grouped32_word_native(
                bytes,
                span.byte_offset
                    + position_index * kGrouped32EncodedWordBytes);
            if (encoded <= previous
                || (encoded >> 1u) >= segment_width) {
                result.scalar_tail_words += position_index;
                result.valid = false;
                return result;
            }
            previous = encoded;
            result.terminal_word_witness = encoded;
        }
        result.scalar_tail_words += span.position_count - 1u;
        result.words_examined += span.position_count;
    }
    return result;
}

[[nodiscard, gnu::noinline]] Grouped32ValidationResult
validate_grouped32_local32_scalar_optimized_bench(
    const Grouped32ValidationInput& input) noexcept
{
    const Grouped32FastValidationResult fast =
        validate_grouped32_local32_scalar_optimized_fast(input);
    if (!fast.valid) {
        Grouped32ValidationResult replay =
            validate_grouped32_local32_scalar_exact_bench(input);
        replay.selected_route = fast.selected_route;
        replay.selected_vector_stream_width =
            fast.selected_vector_stream_width;
        replay.vectorized_words = fast.vectorized_words;
        replay.scalar_boundary_words = fast.scalar_boundary_words;
        replay.scalar_tail_words = fast.scalar_tail_words;
        replay.failure_replay_count = 1u;
        return replay;
    }
    return Grouped32ValidationResult{
        .words_examined = fast.words_examined,
        .terminal_word_witness = fast.terminal_word_witness,
        .selected_route = fast.selected_route,
        .selected_vector_stream_width =
            fast.selected_vector_stream_width,
        .vectorized_words = fast.vectorized_words,
        .scalar_boundary_words = fast.scalar_boundary_words,
        .scalar_tail_words = fast.scalar_tail_words,
    };
}

/// Four-word scalar candidate. Cross-group and intra-group predecessor checks
/// are explicit, while native loads remain alias-safe.
[[nodiscard, gnu::always_inline]] inline Grouped32FastValidationResult
validate_grouped32_local32_scalar_unrolled_fast(
    const Grouped32ValidationInput& input) noexcept
{
    const std::span<const std::byte> bytes{input.encoded_bytes};
    Grouped32FastValidationResult result;
    result.selected_route = Grouped32ValidationRoute::scalar_unrolled;
    for (const Grouped32ValidationSpan& span : input.spans) {
        if (!grouped32_span_fast_path_admitted(bytes, span)) {
            result.valid = false;
            return result;
        }
        if (span.position_count == 0u) {
            continue;
        }
        const auto segment_width = static_cast<std::uint32_t>(
            span.segment_end_bp - span.segment_base_bp);
        std::size_t position_index = 0;
        std::uint32_t previous = read_grouped32_word_native(
            bytes, span.byte_offset);
        ++result.scalar_boundary_words;
        if ((previous >> 1u) >= segment_width) {
            result.valid = false;
            return result;
        }
        ++position_index;
        result.terminal_word_witness = previous;

        while (span.position_count - position_index >= 4u) {
            const std::size_t byte_offset = span.byte_offset
                + position_index * kGrouped32EncodedWordBytes;
            const std::array<std::uint32_t, 4> values{
                read_grouped32_word_native(bytes, byte_offset),
                read_grouped32_word_native(bytes, byte_offset + 4u),
                read_grouped32_word_native(bytes, byte_offset + 8u),
                read_grouped32_word_native(bytes, byte_offset + 12u),
            };
            if (values[0] <= previous
                || values[1] <= values[0]
                || values[2] <= values[1]
                || values[3] <= values[2]
                || (values[0] >> 1u) >= segment_width
                || (values[1] >> 1u) >= segment_width
                || (values[2] >> 1u) >= segment_width
                || (values[3] >> 1u) >= segment_width) {
                result.scalar_tail_words += position_index + 3u;
                result.valid = false;
                return result;
            }
            previous = values[3];
            result.terminal_word_witness = previous;
            position_index += values.size();
        }
        while (position_index < span.position_count) {
            const std::uint32_t encoded = read_grouped32_word_native(
                bytes,
                span.byte_offset
                    + position_index * kGrouped32EncodedWordBytes);
            if (encoded <= previous
                || (encoded >> 1u) >= segment_width) {
                result.scalar_tail_words += position_index;
                result.valid = false;
                return result;
            }
            previous = encoded;
            result.terminal_word_witness = encoded;
            ++position_index;
        }
        result.scalar_tail_words += span.position_count - 1u;
        result.words_examined += span.position_count;
    }
    return result;
}

[[nodiscard, gnu::noinline]] Grouped32ValidationResult
validate_grouped32_local32_scalar_unrolled_bench(
    const Grouped32ValidationInput& input) noexcept
{
    const Grouped32FastValidationResult fast =
        validate_grouped32_local32_scalar_unrolled_fast(input);
    if (!fast.valid) {
        Grouped32ValidationResult replay =
            validate_grouped32_local32_scalar_exact_bench(input);
        replay.selected_route = fast.selected_route;
        replay.selected_vector_stream_width =
            fast.selected_vector_stream_width;
        replay.vectorized_words = fast.vectorized_words;
        replay.scalar_boundary_words = fast.scalar_boundary_words;
        replay.scalar_tail_words = fast.scalar_tail_words;
        replay.failure_replay_count = 1u;
        return replay;
    }
    return Grouped32ValidationResult{
        .words_examined = fast.words_examined,
        .terminal_word_witness = fast.terminal_word_witness,
        .selected_route = fast.selected_route,
        .selected_vector_stream_width =
            fast.selected_vector_stream_width,
        .vectorized_words = fast.vectorized_words,
        .scalar_boundary_words = fast.scalar_boundary_words,
        .scalar_tail_words = fast.scalar_tail_words,
    };
}

#if HWY_TARGET != HWY_SCALAR

namespace grouped32_hn = hwy::HWY_NAMESPACE;
namespace grouped32_vs = bioseq::simd::HWY_NAMESPACE;

/// Validate W adjacent vector blocks from one contiguous span per iteration.
/// W=2 is the expected NEON issue-width candidate; W=1 and W=4 diagnose
/// wrapper overhead and register pressure respectively.
template <std::size_t StreamWidth>
[[nodiscard, gnu::always_inline]] inline Grouped32FastValidationResult
validate_grouped32_local32_vec_stream_fast(
    const Grouped32ValidationInput& input) noexcept
{
    static_assert(StreamWidth == 1u
        || StreamWidth == 2u
        || StreamWidth == 4u);
    using D8 = grouped32_hn::ScalableTag<std::uint8_t>;
    using D32 = grouped32_hn::Repartition<std::uint32_t, D8>;
    D8 d8;
    D32 d32;
    const std::size_t lanes = grouped32_hn::Lanes(d32);
    const std::span<const std::byte> bytes{input.encoded_bytes};
    Grouped32FastValidationResult result;
    result.selected_route = grouped32_vector_route<StreamWidth>();
    result.selected_vector_stream_width =
        static_cast<std::uint8_t>(StreamWidth);

    for (const Grouped32ValidationSpan& span : input.spans) {
        if (!grouped32_span_fast_path_admitted(bytes, span)) {
            result.valid = false;
            return result;
        }
        if (span.position_count == 0u) {
            continue;
        }

        const auto segment_width = static_cast<std::uint32_t>(
            span.segment_end_bp - span.segment_base_bp);
        const auto* span_bytes = reinterpret_cast<const std::uint8_t*>(
            bytes.data() + span.byte_offset);
        std::size_t position_index = 0;
        const std::uint32_t first = read_grouped32_word_native(
            bytes, span.byte_offset);
        ++result.scalar_boundary_words;
        if ((first >> 1u) >= segment_width) {
            result.valid = false;
            return result;
        }
        ++position_index;
        std::uint32_t previous_scalar = first;
        result.terminal_word_witness = first;

        const std::size_t words_per_stream_iteration = StreamWidth * lanes;
        if (span.position_count - position_index
                >= words_per_stream_iteration) {
            auto previous_vector = grouped32_hn::Set(d32, first);
            auto invalid_masks = grouped32_vs::mask_stream<
                D32, StreamWidth>::splat_false(d32);
            const auto widths = grouped32_vs::vec_stream<
                D32, std::uint32_t, StreamWidth>::splat(
                    d32, segment_width);
            while (span.position_count - position_index
                    >= words_per_stream_iteration) {
                const auto loaded_bytes = grouped32_vs::make_vec_stream<
                    D8, std::uint8_t, StreamWidth>(
                    [d8, span_bytes, position_index, lanes](auto slot_constant)
                        [[gnu::always_inline]] {
                        constexpr std::size_t slot =
                            decltype(slot_constant)::value;
                        const std::size_t word_offset = position_index
                            + slot * lanes;
                        return grouped32_hn::LoadU(
                            d8,
                            span_bytes
                                + word_offset * kGrouped32EncodedWordBytes);
                });
                const auto current = grouped32_vs::BitCast(
                    d32, loaded_bytes);
                const auto predecessors = grouped32_vs::make_vec_stream<
                    D32, std::uint32_t, StreamWidth>(
                    [d32, &current, previous_vector](auto slot_constant)
                        [[gnu::always_inline]] {
                        constexpr std::size_t slot =
                            decltype(slot_constant)::value;
                        if constexpr (slot == 0u) {
                            return grouped32_vs::lanes_shift_left_1_borrow(
                                d32, current[slot], previous_vector);
                        } else {
                            return grouped32_vs::lanes_shift_left_1_borrow(
                                d32, current[slot], current[slot - 1u]);
                        }
                    });
                const auto invalid_order = grouped32_vs::Le(
                    current, predecessors);
                const auto invalid_range = grouped32_vs::Ge(
                    grouped32_vs::ShiftRight<1>(current), widths);
                invalid_masks = grouped32_vs::Or(
                    invalid_masks,
                    grouped32_vs::Or(invalid_order, invalid_range));
                previous_vector = current[StreamWidth - 1u];
                position_index += words_per_stream_iteration;
            }
            const std::uint64_t vectorized_words_this_span =
                position_index - 1u;
            const auto all_clear = grouped32_vs::AllFalsePerStream(
                d32, invalid_masks);
            for (std::size_t slot = 0; slot < StreamWidth; ++slot) {
                if (!all_clear[slot]) {
                    result.vectorized_words += vectorized_words_this_span;
                    result.valid = false;
                    return result;
                }
            }
            result.vectorized_words += vectorized_words_this_span;
            previous_scalar = grouped32_vs::extract_last(
                d32, previous_vector);
            result.terminal_word_witness = previous_scalar;
        }

        const std::size_t scalar_tail_begin = position_index;
        while (position_index < span.position_count) {
            const std::uint32_t encoded = read_grouped32_word_native(
                bytes,
                span.byte_offset
                    + position_index * kGrouped32EncodedWordBytes);
            if (encoded <= previous_scalar
                || (encoded >> 1u) >= segment_width) {
                result.scalar_tail_words +=
                    position_index - scalar_tail_begin + 1u;
                result.valid = false;
                return result;
            }
            previous_scalar = encoded;
            result.terminal_word_witness = encoded;
            ++position_index;
        }
        result.scalar_tail_words += position_index - scalar_tail_begin;
        result.words_examined += span.position_count;
    }
    return result;
}

#endif

template <std::size_t StreamWidth>
[[nodiscard, gnu::noinline]] Grouped32ValidationResult
validate_grouped32_local32_vec_stream_bench(
    const Grouped32ValidationInput& input)
{
#if HWY_TARGET == HWY_SCALAR
    throw std::runtime_error(fmt::format(
        "grouped32 vector_stream_{} was selected on the Highway scalar target",
        StreamWidth));
#else
    if constexpr (std::endian::native != std::endian::little) {
        throw std::runtime_error(fmt::format(
            "grouped32 vector_stream_{} requires a little-endian host; "
            "silent scalar fallback is forbidden",
            StreamWidth));
    } else {
        const Grouped32FastValidationResult fast =
            validate_grouped32_local32_vec_stream_fast<StreamWidth>(input);
        if (!fast.valid) {
            Grouped32ValidationResult replay =
                validate_grouped32_local32_scalar_exact_bench(input);
            replay.selected_route = fast.selected_route;
            replay.selected_vector_stream_width =
                fast.selected_vector_stream_width;
            replay.vectorized_words = fast.vectorized_words;
            replay.scalar_boundary_words = fast.scalar_boundary_words;
            replay.scalar_tail_words = fast.scalar_tail_words;
            replay.failure_replay_count = 1u;
            return replay;
        }
        return Grouped32ValidationResult{
            .words_examined = fast.words_examined,
            .terminal_word_witness = fast.terminal_word_witness,
            .selected_route = fast.selected_route,
            .selected_vector_stream_width =
                fast.selected_vector_stream_width,
            .vectorized_words = fast.vectorized_words,
            .scalar_boundary_words = fast.scalar_boundary_words,
            .scalar_tail_words = fast.scalar_tail_words,
        };
    }
#endif
}

/// Return one exact four-byte little-endian benchmark word.
[[nodiscard]] constexpr std::array<std::byte, 4>
encode_grouped32_benchmark_word(std::uint32_t value) noexcept {
    return {
        static_cast<std::byte>(value & 0xffu),
        static_cast<std::byte>((value >> 8u) & 0xffu),
        static_cast<std::byte>((value >> 16u) & 0xffu),
        static_cast<std::byte>((value >> 24u) & 0xffu),
    };
}

struct Grouped32AdversarialProbe {
    std::string_view name;
    Grouped32ValidationInput input;
    Grouped32ValidationError expected_error =
        Grouped32ValidationError::none;
    std::uint64_t required_vectorized_words = 0;
};

/// Small owned input used only by the untimed evaluator probes.
[[nodiscard]] Grouped32ValidationInput make_grouped32_adversarial_input(
    std::size_t position_count,
    std::uint64_t segment_base_bp,
    std::uint64_t segment_end_bp,
    std::uint32_t encoded_bias = 0u)
{
    Grouped32ValidationInput input;
    input.encoded_bytes.resize(4u, std::byte{0});
    input.encoded_bytes.reserve(
        4u + position_count * kGrouped32EncodedWordBytes);
    input.spans.push_back(Grouped32ValidationSpan{
        .byte_offset = input.encoded_bytes.size(),
        .position_count = position_count,
        .segment_base_bp = segment_base_bp,
        .segment_end_bp = segment_end_bp,
    });
    input.total_words = position_count;
    for (std::size_t position_index = 0;
         position_index < position_count;
         ++position_index) {
        const auto encoded = encode_grouped32_benchmark_word(
            static_cast<std::uint32_t>(position_index) + encoded_bias);
        input.encoded_bytes.insert(
            input.encoded_bytes.end(), encoded.begin(), encoded.end());
    }
    return input;
}

[[nodiscard]] Grouped32ValidationInput with_grouped32_probe_word(
    Grouped32ValidationInput input,
    std::size_t position_index,
    std::uint32_t encoded_word)
{
    const std::size_t byte_offset = input.spans.front().byte_offset
        + position_index * kGrouped32EncodedWordBytes;
    const auto encoded = encode_grouped32_benchmark_word(encoded_word);
    std::copy(
        encoded.begin(), encoded.end(),
        input.encoded_bytes.begin()
            + static_cast<std::ptrdiff_t>(byte_offset));
    return input;
}

[[nodiscard]] Grouped32AdversarialProbe finalize_grouped32_probe(
    std::string_view name,
    Grouped32ValidationInput input,
    Grouped32ValidationError expected_error,
    std::uint64_t required_vectorized_words = 0)
{
    input.exact_reference =
        validate_grouped32_local32_scalar_exact_bench(input);
    return Grouped32AdversarialProbe{
        .name = name,
        .input = std::move(input),
        .expected_error = expected_error,
        .required_vectorized_words = required_vectorized_words,
    };
}

[[nodiscard]] std::size_t grouped32_probe_u32_lanes() noexcept {
#if HWY_TARGET == HWY_SCALAR
    return 4u;
#else
    using D8 = hwy::HWY_NAMESPACE::ScalableTag<std::uint8_t>;
    using D32 = hwy::HWY_NAMESPACE::Repartition<std::uint32_t, D8>;
    D32 d32;
    return hwy::HWY_NAMESPACE::Lanes(d32);
#endif
}

/// Build corruption at one strict-order boundary. Equal and decreasing probes
/// are kept separate so both comparisons are exercised by every route.
[[nodiscard]] Grouped32AdversarialProbe make_grouped32_order_probe(
    std::string_view name,
    std::size_t corrupt_index,
    std::size_t minimum_count,
    std::uint64_t required_vectorized_words,
    bool decreasing)
{
    const std::size_t count = std::max(
        corrupt_index + 2u, minimum_count);
    Grouped32ValidationInput input = make_grouped32_adversarial_input(
        count,
        0u,
        static_cast<std::uint64_t>(count + 32u),
        4u);
    const auto previous = static_cast<std::uint32_t>(corrupt_index) + 3u;
    input = with_grouped32_probe_word(
        std::move(input),
        corrupt_index,
        decreasing ? previous - 1u : previous);
    return finalize_grouped32_probe(
        name,
        std::move(input),
        Grouped32ValidationError::position_range,
        required_vectorized_words);
}

[[nodiscard]] std::vector<Grouped32AdversarialProbe>
build_grouped32_adversarial_probes(std::size_t stream_width)
{
    const std::size_t lanes = grouped32_probe_u32_lanes();
    const std::size_t admitted_stream_width =
        stream_width == 1u || stream_width == 2u || stream_width == 4u
        ? stream_width : 2u;
    const std::size_t vector_words = admitted_stream_width * lanes;
    const std::size_t one_iteration_count = 1u + vector_words;
    const std::size_t scalar_to_vector_boundary = 1u;
    const std::size_t lane_boundary = lanes;
    const std::size_t stream_boundary = 1u + lanes;
    const std::size_t cross_iteration_boundary = 1u + vector_words;
    const std::size_t two_iteration_count = 1u + 2u * vector_words;
    const std::size_t scalar_tail_boundary = two_iteration_count;
    const std::size_t scalar_tail_probe_count = scalar_tail_boundary + 2u;

    std::vector<Grouped32AdversarialProbe> probes;
    probes.reserve(14u);

    Grouped32ValidationInput truncated = make_grouped32_adversarial_input(
        two_iteration_count + 1u, 0u,
        static_cast<std::uint64_t>(two_iteration_count + 64u));
    truncated.encoded_bytes.pop_back();
    probes.push_back(finalize_grouped32_probe(
        "byte_truncation",
        std::move(truncated),
        Grouped32ValidationError::byte_count));

    probes.push_back(finalize_grouped32_probe(
        "invalid_segment_geometry",
        make_grouped32_adversarial_input(2u, 5u, 5u),
        Grouped32ValidationError::position_range));

    probes.push_back(finalize_grouped32_probe(
        "order_valid_local_range_inside_vector_block",
        make_grouped32_adversarial_input(
            one_iteration_count, 0u, 1u),
        Grouped32ValidationError::position_range,
        vector_words));

    probes.push_back(finalize_grouped32_probe(
        "global_addition_overflow",
        with_grouped32_probe_word(
            make_grouped32_adversarial_input(
                1u,
                (std::numeric_limits<std::uint64_t>::max)() - 2u,
                (std::numeric_limits<std::uint64_t>::max)()),
            0u,
            6u),
        Grouped32ValidationError::arithmetic_overflow));

    probes.push_back(make_grouped32_order_probe(
        "equal_at_scalar_to_vector_borrow",
        scalar_to_vector_boundary,
        one_iteration_count,
        vector_words,
        false));
    probes.push_back(make_grouped32_order_probe(
        "decreasing_at_scalar_to_vector_borrow",
        scalar_to_vector_boundary,
        one_iteration_count,
        vector_words,
        true));
    probes.push_back(make_grouped32_order_probe(
        "equal_at_vector_lane_boundary",
        lane_boundary,
        one_iteration_count,
        vector_words,
        false));
    probes.push_back(make_grouped32_order_probe(
        "decreasing_at_vector_lane_boundary",
        lane_boundary,
        one_iteration_count,
        vector_words,
        true));
    if (admitted_stream_width > 1u) {
        probes.push_back(make_grouped32_order_probe(
            "equal_at_intra_iteration_stream_boundary",
            stream_boundary,
            one_iteration_count,
            vector_words,
            false));
        probes.push_back(make_grouped32_order_probe(
            "decreasing_at_intra_iteration_stream_boundary",
            stream_boundary,
            one_iteration_count,
            vector_words,
            true));
    }
    probes.push_back(make_grouped32_order_probe(
        "equal_at_cross_iteration_slot_zero_borrow",
        cross_iteration_boundary,
        two_iteration_count,
        2u * vector_words,
        false));
    probes.push_back(make_grouped32_order_probe(
        "decreasing_at_cross_iteration_slot_zero_borrow",
        cross_iteration_boundary,
        two_iteration_count,
        2u * vector_words,
        true));
    probes.push_back(make_grouped32_order_probe(
        "equal_at_scalar_tail_after_two_vector_iterations",
        scalar_tail_boundary,
        scalar_tail_probe_count,
        2u * vector_words,
        false));
    probes.push_back(make_grouped32_order_probe(
        "decreasing_at_scalar_tail_after_two_vector_iterations",
        scalar_tail_boundary,
        scalar_tail_probe_count,
        2u * vector_words,
        true));
    return probes;
}

[[nodiscard]] const std::vector<Grouped32AdversarialProbe>&
grouped32_adversarial_probes(std::size_t stream_width)
{
    static const std::vector<Grouped32AdversarialProbe> probes_w1 =
        build_grouped32_adversarial_probes(1u);
    static const std::vector<Grouped32AdversarialProbe> probes_w2 =
        build_grouped32_adversarial_probes(2u);
    static const std::vector<Grouped32AdversarialProbe> probes_w4 =
        build_grouped32_adversarial_probes(4u);
    if (stream_width == 1u) {
        return probes_w1;
    }
    if (stream_width == 4u) {
        return probes_w4;
    }
    return probes_w2;
}

[[nodiscard]] RunConstraints estimate_grouped32_validation_resources(
    std::string_view /*data_dir*/,
    std::string_view /*test_case_name*/,
    simdjson::dom::element data) noexcept
{
    const std::uint64_t total_words = static_cast<std::uint64_t>(
        std::max<std::int64_t>(0,
            read_int_field(data, "total_words", 65'536)));
    const std::uint64_t span_count = static_cast<std::uint64_t>(
        std::max<std::int64_t>(0,
            read_int_field(data, "span_count", 1)));
    const std::uint64_t encoded_bytes = total_words
        > (std::numeric_limits<std::uint64_t>::max)()
            / kGrouped32EncodedWordBytes
        ? (std::numeric_limits<std::uint64_t>::max)()
        : total_words * kGrouped32EncodedWordBytes;
    const std::uint64_t span_bytes = span_count
        > (std::numeric_limits<std::uint64_t>::max)()
            / (sizeof(Grouped32ValidationSpan) + 15u)
        ? (std::numeric_limits<std::uint64_t>::max)()
        : span_count * (sizeof(Grouped32ValidationSpan) + 15u);
    const std::uint64_t total_bytes = encoded_bytes
        > (std::numeric_limits<std::uint64_t>::max)() - span_bytes
        ? (std::numeric_limits<std::uint64_t>::max)()
        : encoded_bytes + span_bytes;
    return {
        .base_memory = static_cast<std::size_t>(
            std::min<std::uint64_t>(
                total_bytes,
                (std::numeric_limits<std::size_t>::max)())),
        .extra_memory = 0u,
    };
}

/// Build deterministic valid spans. Every span starts on a production-real
/// four-byte boundary while rotating through offsets 0/4/8/12 modulo 16.
/// Consecutive encoded values exercise same-bp forward/reverse ordering.
[[nodiscard]] Grouped32ValidationInput build_grouped32_validation_input(
    std::string_view /*data_dir*/,
    std::string_view test_case_name,
    simdjson::dom::element data,
    const RunConstraints& /*constraints*/)
{
    const auto configured_total_words = static_cast<std::size_t>(
        std::max<std::int64_t>(0,
            read_int_field(data, "total_words", 65'536)));
    const auto configured_span_count = static_cast<std::size_t>(
        std::max<std::int64_t>(1,
            read_int_field(data, "span_count", 1)));
    const auto configured_words_per_span = static_cast<std::size_t>(
        std::max<std::int64_t>(1,
            read_int_field(data, "words_per_span", 1)));

    std::vector<std::size_t> counts;
    counts.reserve(configured_span_count);
    if (test_case_name == "GROUPED32_LOCAL32_ZIPF_64K") {
        std::size_t assigned = 0;
        for (std::size_t rank = 1; rank <= configured_span_count; ++rank) {
            const std::size_t count = std::max<std::size_t>(
                1u, 8192u / rank);
            counts.push_back(count);
            assigned += count;
        }
        if (assigned <= configured_total_words) {
            counts.front() += configured_total_words - assigned;
        } else {
            std::size_t excess = assigned - configured_total_words;
            for (std::size_t index = 0;
                 index < counts.size() && excess != 0u;
                 ++index) {
                const std::size_t removable = counts[index] - 1u;
                const std::size_t removed = std::min(removable, excess);
                counts[index] -= removed;
                excess -= removed;
            }
        }
    } else {
        counts.assign(configured_span_count, configured_words_per_span);
    }

    std::uint64_t total_words = 0;
    for (const std::size_t count : counts) {
        total_words += count;
    }

    Grouped32ValidationInput input;
    input.total_words = total_words;
    input.spans.reserve(counts.size());
    const std::uint64_t reserve_bytes = total_words
        * kGrouped32EncodedWordBytes
        + static_cast<std::uint64_t>(counts.size()) * 15u;
    input.encoded_bytes.reserve(static_cast<std::size_t>(reserve_bytes));

    for (std::size_t span_index = 0;
         span_index < counts.size();
         ++span_index) {
        const std::size_t desired_modulo = (span_index % 4u) * 4u;
        const std::size_t current_modulo = input.encoded_bytes.size() % 16u;
        const std::size_t padding =
            (desired_modulo + 16u - current_modulo) % 16u;
        input.encoded_bytes.resize(
            input.encoded_bytes.size() + padding, std::byte{0});

        const std::size_t count = counts[span_index];
        const std::uint64_t segment_base_bp =
            static_cast<std::uint64_t>(span_index) * 1'000'000u;
        const std::uint64_t segment_width =
            static_cast<std::uint64_t>((count + 1u) / 2u) + 32u;
        input.spans.push_back(Grouped32ValidationSpan{
            .byte_offset = input.encoded_bytes.size(),
            .position_count = count,
            .segment_base_bp = segment_base_bp,
            .segment_end_bp = segment_base_bp + segment_width,
        });
        for (std::size_t position_index = 0;
             position_index < count;
             ++position_index) {
            const auto encoded = encode_grouped32_benchmark_word(
                static_cast<std::uint32_t>(position_index));
            input.encoded_bytes.insert(
                input.encoded_bytes.end(), encoded.begin(), encoded.end());
        }
    }
    input.exact_reference =
        validate_grouped32_local32_scalar_exact_bench(input);
    return input;
}

struct Grouped32ExpectedRoute {
    Grouped32ValidationRoute route =
        Grouped32ValidationRoute::scalar_exact;
    std::uint8_t vector_stream_width = 0;
};

[[nodiscard]] Grouped32ExpectedRoute grouped32_expected_route(
    std::string_view algorithm)
{
    if (algorithm == "grouped32_local32_scalar_exact") {
        return {Grouped32ValidationRoute::scalar_exact, 0u};
    }
    if (algorithm == "grouped32_local32_scalar_optimized") {
        return {Grouped32ValidationRoute::scalar_optimized, 0u};
    }
    if (algorithm == "grouped32_local32_scalar_unrolled") {
        return {Grouped32ValidationRoute::scalar_unrolled, 0u};
    }
    if (algorithm == "grouped32_local32_vec_stream_1") {
        return {Grouped32ValidationRoute::vector_stream_1, 1u};
    }
    if (algorithm == "grouped32_local32_vec_stream_2") {
        return {Grouped32ValidationRoute::vector_stream_2, 2u};
    }
    if (algorithm == "grouped32_local32_vec_stream_4") {
        return {Grouped32ValidationRoute::vector_stream_4, 4u};
    }
    throw std::invalid_argument(fmt::format(
        "unknown grouped32 validation algorithm '{}'", algorithm));
}

/// Dispatch without instrumentation. The timed callback wraps exactly one
/// call; the evaluator reuses this function for untimed adversarial probes.
[[nodiscard]] Grouped32ValidationResult run_grouped32_validation_selected(
    std::string_view algorithm,
    const Grouped32ValidationInput& input)
{
    if (algorithm == "grouped32_local32_scalar_exact") {
        return validate_grouped32_local32_scalar_exact_bench(input);
    }
    if (algorithm == "grouped32_local32_scalar_optimized") {
        return validate_grouped32_local32_scalar_optimized_bench(input);
    }
    if (algorithm == "grouped32_local32_scalar_unrolled") {
        return validate_grouped32_local32_scalar_unrolled_bench(input);
    }
    if (algorithm == "grouped32_local32_vec_stream_1") {
        return validate_grouped32_local32_vec_stream_bench<1u>(input);
    }
    if (algorithm == "grouped32_local32_vec_stream_2") {
        return validate_grouped32_local32_vec_stream_bench<2u>(input);
    }
    if (algorithm == "grouped32_local32_vec_stream_4") {
        return validate_grouped32_local32_vec_stream_bench<4u>(input);
    }
    throw std::invalid_argument(fmt::format(
        "unknown grouped32 validation algorithm '{}'", algorithm));
}

[[nodiscard]] bool grouped32_route_identity_matches(
    const Grouped32ValidationResult& result,
    const Grouped32ExpectedRoute& expected) noexcept
{
    return result.selected_route == expected.route
        && result.selected_vector_stream_width
            == expected.vector_stream_width
        && (expected.vector_stream_width != 0u
            || result.vectorized_words == 0u);
}

[[nodiscard]] bool grouped32_complete_route_proof_matches(
    const Grouped32ValidationResult& result,
    const Grouped32ExpectedRoute& expected) noexcept
{
    return grouped32_route_identity_matches(result, expected)
        && result.failure_replay_count == 0u
        && result.vectorized_words
            + result.scalar_boundary_words
            + result.scalar_tail_words
            == result.words_examined;
}

[[nodiscard]] Grouped32ValidationResult run_grouped32_validation(
    std::string_view algorithm,
    const ParamMap& /*params*/,
    const Grouped32ValidationInput& input)
{
    static const auto* descriptor =
        bioseq::instr::register_region_static(
            "grouped32_local32_validation");
    auto scope = bioseq::instr::region(descriptor);
    return run_grouped32_validation_selected(algorithm, input);
}

[[nodiscard]] std::map<std::string, double>
evaluate_grouped32_validation(
    std::string_view algorithm,
    const ParamMap& /*params*/,
    const Grouped32ValidationInput& input,
    const Grouped32ValidationResult& output)
{
    const Grouped32ValidationResult reference =
        validate_grouped32_local32_scalar_exact_bench(input);
    const Grouped32ExpectedRoute expected =
        grouped32_expected_route(algorithm);
    bool input_requires_vector_words = false;
    if (expected.vector_stream_width != 0u) {
        const std::size_t vector_words =
            static_cast<std::size_t>(expected.vector_stream_width)
            * grouped32_probe_u32_lanes();
        for (const Grouped32ValidationSpan& span : input.spans) {
            if (span.position_count > vector_words) {
                input_requires_vector_words = true;
                break;
            }
        }
    }
    if (!same_grouped32_validation_semantics(output, reference)
        || !same_grouped32_validation_semantics(
            reference, input.exact_reference)
        || (reference.valid()
            && reference.words_examined != input.total_words)
        || !grouped32_complete_route_proof_matches(output, expected)
        || (input_requires_vector_words
            && output.vectorized_words == 0u)) {
        throw std::runtime_error(fmt::format(
            "grouped32 validation/parity route proof failed for '{}' "
            "(error={}, first={}, words={}, witness={}; "
            "reference error={}, first={}, words={}, witness={}; "
            "route={}, width={}, vector={}, boundary={}, tail={}, replay={})",
            algorithm,
            static_cast<unsigned>(output.error),
            output.first_error_word,
            output.words_examined,
            output.terminal_word_witness,
            static_cast<unsigned>(reference.error),
            reference.first_error_word,
            reference.words_examined,
            reference.terminal_word_witness,
            static_cast<unsigned>(output.selected_route),
            static_cast<unsigned>(output.selected_vector_stream_width),
            output.vectorized_words,
            output.scalar_boundary_words,
            output.scalar_tail_words,
            output.failure_replay_count));
    }

    const std::size_t probe_width = expected.vector_stream_width == 0u
        ? 2u : expected.vector_stream_width;
    const auto& probes = grouped32_adversarial_probes(probe_width);
    std::uint64_t adversarial_vectorized_words = 0;
    std::uint64_t adversarial_replay_count = 0;
    for (const Grouped32AdversarialProbe& probe : probes) {
        const Grouped32ValidationResult probe_reference =
            validate_grouped32_local32_scalar_exact_bench(probe.input);
        const Grouped32ValidationResult candidate =
            run_grouped32_validation_selected(algorithm, probe.input);
        const std::uint64_t expected_replay_count =
            expected.route == Grouped32ValidationRoute::scalar_exact
            ? 0u : 1u;
        if (probe_reference.error != probe.expected_error
            || !same_grouped32_validation_semantics(
                probe_reference, probe.input.exact_reference)
            || !same_grouped32_validation_semantics(
                candidate, probe_reference)
            || !grouped32_route_identity_matches(candidate, expected)
            || candidate.failure_replay_count != expected_replay_count
            || (expected.vector_stream_width != 0u
                && candidate.vectorized_words
                    < probe.required_vectorized_words)) {
            throw std::runtime_error(fmt::format(
                "grouped32 adversarial parity failed for '{}' probe '{}' "
                "(expected_error={}, reference_error={}, candidate_error={}, "
                "first={}, route={}, width={}, vector={}, required_vector={}, "
                "boundary={}, tail={}, replay={})",
                algorithm,
                probe.name,
                static_cast<unsigned>(probe.expected_error),
                static_cast<unsigned>(probe_reference.error),
                static_cast<unsigned>(candidate.error),
                candidate.first_error_word,
                static_cast<unsigned>(candidate.selected_route),
                static_cast<unsigned>(
                    candidate.selected_vector_stream_width),
                candidate.vectorized_words,
                probe.required_vectorized_words,
                candidate.scalar_boundary_words,
                candidate.scalar_tail_words,
                candidate.failure_replay_count));
        }
        adversarial_vectorized_words += candidate.vectorized_words;
        adversarial_replay_count += candidate.failure_replay_count;
    }
    if (expected.vector_stream_width != 0u
        && adversarial_vectorized_words == 0u) {
        throw std::runtime_error(fmt::format(
            "grouped32 vector_stream_{} adversarial probes executed no "
            "vector words",
            expected.vector_stream_width));
    }
    return {
        {"validation_parity", 1.0},
        {"adversarial_parity", 1.0},
        {"primary_correct_fraction", 1.0},
        {"route_proof_accepted", 1.0},
        {"selected_route",
            static_cast<double>(static_cast<unsigned>(
                output.selected_route))},
        {"selected_vector_stream_width",
            static_cast<double>(output.selected_vector_stream_width)},
        {"vectorized_words",
            static_cast<double>(output.vectorized_words)},
        {"scalar_boundary_words",
            static_cast<double>(output.scalar_boundary_words)},
        {"scalar_tail_words",
            static_cast<double>(output.scalar_tail_words)},
        {"failure_replay_count",
            static_cast<double>(output.failure_replay_count)},
        {"adversarial_probe_count",
            static_cast<double>(probes.size())},
        {"adversarial_vectorized_words",
            static_cast<double>(adversarial_vectorized_words)},
        {"adversarial_replay_count",
            static_cast<double>(adversarial_replay_count)},
        {"valid_result", output.valid() ? 1.0 : 0.0},
        {"validated_words", static_cast<double>(output.words_examined)},
        {"terminal_word_witness",
            static_cast<double>(output.terminal_word_witness)},
    };
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
/// batch. The generator dimensions come from the JSON blob; the framework-
/// resolved tuples in `constraints` seed the zero-work off/Vulkan parity
/// ledger with the rows and thresholds that are actually scheduled.
[[nodiscard]] AlignerInput build_input(std::string_view /*data_dir*/,
                                        std::string_view test_case_name,
                                        simdjson::dom::element data,
                                        const RunConstraints& constraints) {
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

    // Record only the framework-resolved tuples that will actually consume
    // this cached input. This is schedule provenance, not reference work: an
    // available Vulkan result is accepted only when the same threshold also
    // has a scheduled prefilter-off row in this batch.
    for (const auto& tuple : constraints.execution_tuples) {
        const bool is_prefilter_off =
            tuple.algorithm_name == "bioseq_prefilter_off";
        const bool is_prefilter_vulkan =
            tuple.algorithm_name == "bioseq_prefilter_vulkan";
        if (!is_prefilter_off && !is_prefilter_vulkan) continue;
        const std::uint32_t threshold = read_max_edits_param(tuple.params);
        auto& scheduled = input.prefilter_pipeline_match_count_parity
            ->by_threshold[threshold];
        scheduled.prefilter_off_scheduled |= is_prefilter_off;
        scheduled.vulkan_scheduled |= is_prefilter_vulkan;
    }

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

/// Encode a fixed ACGT fixture outside every timed callback.
[[nodiscard]] bench_sequence_t compact_dna4_from_acgt(
        std::string_view acgt) {
    std::vector<seq::dna4> letters(acgt.size());
    for (std::size_t index = 0; index < acgt.size(); ++index) {
        letters[index].assign_char(acgt[index]);
    }
    return bench_sequence_t{std::move(letters)};
}

[[nodiscard]] EndpointQualificationFixture make_endpoint_fixture(
        const EndpointFixtureSpec& spec,
        std::uint64_t target_ordinal) {
    gen::generator_params params{};
    params.reference_length = spec.target_bp;
    params.fragment_count = 1;
    params.fragment_length_mean = spec.query_bp;
    params.fragment_length_stddev = 0.0;
    params.substitution_rate = spec.substitution_rate;
    params.insertion_rate = spec.insertion_rate;
    params.deletion_rate = spec.deletion_rate;
    params.insertion_length_mean = 1.5;
    params.deletion_length_mean = 1.5;
    params.reverse_complement_rate = spec.reverse_rate;
    params.seed = spec.seed;

    EndpointQualificationFixture fixture;
    fixture.name = spec.name;
    fixture.target_ordinal = target_ordinal;
    fixture.chain_skip_scale = spec.chain_skip_scale;
    fixture.max_chain_skip = spec.max_chain_skip;

    if (spec.name == "max_skip_invalid_runs_and_rescue") {
        // Construct the older-winner geometry in sequence space instead of
        // hoping a random error profile happens to produce it.  The query is
        // an exact 50-kbp target slice.  Inside that slice the target then
        // receives a 96-bp wall made from a 32-bp motif near the beginning
        // of the query.  A sixteen-copy bank of that motif is installed in both
        // query and target at the early locus, so the shorter wall still emits
        // enough later-reference/early-query distractors to exceed the default
        // max_skip=25 bounded scan.  Chaining therefore sees a strong older
        // diagonal, the distractor wall, and an exact suffix anchor.  The
        // repaired one-lane global-best rescue must reconnect the diagonal.
        // Keeping the target-only disruption to 96 bp bounds even an all-
        // mismatch score drop below minimap2's default z-drop=400, avoiding the
        // two-primary split observed with the original 512-bp wall while
        // retaining the same deterministic rescue geometry and strict oracle
        // contract.
        auto references = bench_generator_t(params).generate_sequences(1);
        fixture.target_acgt =
            bioseq::bench::HWY_NAMESPACE::compact_sequence_to_acgt(
                references.front());
        const std::size_t true_start =
            (spec.target_bp - spec.query_bp) / 2u;
        fixture.query_acgt = fixture.target_acgt.substr(
            true_start, spec.query_bp);
        constexpr std::size_t motif_offset = 1'000u;
        constexpr std::size_t motif_bp = 32u;
        constexpr std::size_t early_repeat_bp = 512u;
        constexpr std::size_t wall_offset = 9'900u;
        constexpr std::size_t wall_bp = 96u;
        static_assert(early_repeat_bp % motif_bp == 0u);
        static_assert(wall_bp % motif_bp == 0u);
        static_assert(
            wall_bp * static_cast<std::size_t>(Minimap2Params{}.mismatch_pen)
            < static_cast<std::size_t>(Minimap2Params{}.zdrop));
        const std::string motif = fixture.query_acgt.substr(
            motif_offset, motif_bp);
        for (std::size_t offset = 0; offset < early_repeat_bp; ++offset) {
            const char base = motif[offset % motif_bp];
            fixture.query_acgt[motif_offset + offset] = base;
            fixture.target_acgt[
                true_start + motif_offset + offset] = base;
        }
        for (std::size_t offset = 0; offset < wall_bp; ++offset) {
            fixture.target_acgt[true_start + wall_offset + offset] =
                motif[offset % motif_bp];
        }

        fixture.pair.query = compact_dna4_from_acgt(fixture.query_acgt);
        fixture.pair.target = std::make_shared<const bench_sequence_t>(
            compact_dna4_from_acgt(fixture.target_acgt));
        fixture.pair.truth_target_start = true_start;
        fixture.pair.truth_target_end = true_start + spec.query_bp;
        fixture.pair.is_reverse = false;
        fixture.pair.effective_error_rate =
            static_cast<double>(wall_bp) /
            static_cast<double>(spec.query_bp);
        fixture.pair.original_query_length = spec.query_bp;
        return fixture;
    }

    if (spec.decoy == EndpointDecoyPlacement::none) {
        auto pairs = bench_generator_t(params).generate_sequences_pairs(1);
        fixture.pair = std::move(pairs.front());
        fixture.query_acgt =
            bioseq::bench::HWY_NAMESPACE::compact_sequence_to_acgt(
                fixture.pair.query);
        fixture.target_acgt =
            bioseq::bench::HWY_NAMESPACE::compact_sequence_to_acgt(
                *fixture.pair.target);
        return fixture;
    }

    // Fixed-locus repetitive decoy. The near-copy differs every 97 bp so all
    // three backends should prefer the exact true locus while still seeing a
    // long equal-score/invalid predecessor run. Placement is source-defined,
    // never selected after inspecting an alignment result.
    auto references = bench_generator_t(params).generate_sequences(1);
    fixture.target_acgt =
        bioseq::bench::HWY_NAMESPACE::compact_sequence_to_acgt(
            references.front());
    const std::size_t quarter = spec.target_bp / 4u;
    const std::size_t true_start =
        spec.decoy == EndpointDecoyPlacement::before_true_locus
            ? spec.target_bp - quarter - spec.query_bp
            : quarter;
    const std::size_t decoy_start =
        spec.decoy == EndpointDecoyPlacement::before_true_locus
            ? quarter
            : spec.target_bp - quarter - spec.query_bp;
    fixture.query_acgt = fixture.target_acgt.substr(true_start, spec.query_bp);
    fixture.target_acgt.replace(
        decoy_start, spec.query_bp, fixture.query_acgt);
    for (std::size_t offset = 48u; offset < spec.query_bp; offset += 97u) {
        char& base = fixture.target_acgt[decoy_start + offset];
        base = base == 'A' ? 'C' : 'A';
    }

    fixture.pair.query = compact_dna4_from_acgt(fixture.query_acgt);
    fixture.pair.target = std::make_shared<const bench_sequence_t>(
        compact_dna4_from_acgt(fixture.target_acgt));
    fixture.pair.truth_target_start = true_start;
    fixture.pair.truth_target_end = true_start + spec.query_bp;
    fixture.pair.is_reverse = false;
    fixture.pair.effective_error_rate = 0.0;
    fixture.pair.original_query_length = spec.query_bp;
    return fixture;
}

[[nodiscard]] RunConstraints estimate_endpoint_qualification_resources(
    std::string_view /*data_dir*/,
    std::string_view /*test_case_name*/,
    simdjson::dom::element /*data*/) noexcept
{
    constexpr std::uint64_t packed_bytes_per_bp_numerator = 1u;
    constexpr std::uint64_t packed_bytes_per_bp_denominator = 4u;
    std::uint64_t cached_bytes = 0;
    std::uint64_t largest_target_bp = 0;
    for (const auto& spec : kEndpointQualificationSpecs) {
        const std::uint64_t target_packed =
            (spec.target_bp * packed_bytes_per_bp_numerator
             + packed_bytes_per_bp_denominator - 1u)
            / packed_bytes_per_bp_denominator;
        const std::uint64_t query_packed =
            (spec.query_bp * packed_bytes_per_bp_numerator
             + packed_bytes_per_bp_denominator - 1u)
            / packed_bytes_per_bp_denominator;
        cached_bytes = bioseq::parallelism::saturating_add(
            cached_bytes,
            target_packed + query_packed + spec.target_bp + spec.query_bp);
        largest_target_bp = std::max<std::uint64_t>(
            largest_target_bp, spec.target_bp);
    }
    const std::uint64_t peak_index_bytes =
        bioseq::parallelism::minimizer_index_peak_bytes_for_bp(
            largest_target_bp);
    const std::uint64_t query_scratch_bytes =
        bioseq::parallelism::saturating_mul(
            static_cast<std::uint64_t>(5'000'000u), 2u);
    const std::uint64_t extra_bytes = bioseq::parallelism::saturating_add(
        peak_index_bytes, query_scratch_bytes);
    return {
        .base_memory = static_cast<std::size_t>(cached_bytes),
        .extra_memory = static_cast<std::size_t>(extra_bytes),
    };
}

[[nodiscard]] EndpointQualificationInput build_endpoint_qualification_input(
    std::string_view /*data_dir*/,
    std::string_view /*test_case_name*/,
    simdjson::dom::element /*data*/,
    const RunConstraints& /*constraints*/)
{
    EndpointQualificationInput input;
    input.fixtures.reserve(kEndpointQualificationSpecs.size());
    for (std::size_t index = 0;
         index < kEndpointQualificationSpecs.size(); ++index) {
        input.fixtures.push_back(make_endpoint_fixture(
            kEndpointQualificationSpecs[index],
            static_cast<std::uint64_t>(index)));
    }
    return input;
}

/// Map an algorithm key from the JSON config to a fully-resolved
/// `Minimap2Params` plus a flag selecting the C reference vs the bioseq
/// port. A fixed kmer_size variant is selected for the special K=7 / K=27
/// REPR_STORAGE test cases via the test-case name passed via the input.
struct AlgorithmDispatch {
    bool is_minimap2_reference = false;
    bool explicit_vulkan = false;
    bool explicit_prefilter_vulkan = false;
    bool historical_partial_gpu_probe = false;
    bool enforce_edit_threshold = false;
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
                                                    std::int32_t kmer_override,
                                                    std::uint32_t max_edits = 40u) {
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
    } else if (algo == "bioseq_prefilter_off"
               || algo == "bioseq_prefilter_scalar"
               || algo == "bioseq_prefilter_simd"
               || algo == "bioseq_prefilter_vulkan") {
        // Controlled full-pipeline comparison: all rows use identical SIMD
        // sketch/chaining and KSW2 extension, compute exact edit distance, and
        // expose only matches satisfying the same E. The sole varied dimension
        // is whether/where the lower-bound prefilter executes.
        dispatch.params.chaining_algorithm       = ChainingAlgorithmType::SIMD_MM2;
        dispatch.params.base_alignment_algorithm = BaseAlignmentAlgorithmType::KSW2;
        dispatch.params.minimizer_algorithm      = MinimizerAlgorithmType::TWO_STACK_NTHASH;
        dispatch.params.compute_edit_distance    = true;
        dispatch.params.prefilter_max_edits      = max_edits;
        dispatch.params.exact_edit_filter_enabled = true;
        dispatch.params.exact_edit_filter_exclusive_upper_bound =
            static_cast<std::int64_t>(max_edits) + 1;
        dispatch.enforce_edit_threshold          = true;
        if (algo != "bioseq_prefilter_off") {
            dispatch.params.prefilter_enabled = true;
            if (algo == "bioseq_prefilter_scalar") {
                dispatch.params.prefilter_backend = PrefilterBackendType::SCALAR;
            } else if (algo == "bioseq_prefilter_simd") {
                dispatch.params.prefilter_backend = PrefilterBackendType::SIMD;
            } else {
                dispatch.params.prefilter_backend = PrefilterBackendType::VULKAN;
                dispatch.explicit_prefilter_vulkan = true;
            }
        }
    } else if (algo == "bioseq_ksw2_simd_dp_two_stack_nthash"
               || algo == "bioseq_ksw2_simd_dp_two_stack_nthash_exact_visited"
               || algo == "bioseq_ksw2_simd_dp_two_stack_nthash_bounded_batch_approximate") {
        dispatch.params.chaining_algorithm       = ChainingAlgorithmType::SIMD_MM2;
        dispatch.params.base_alignment_algorithm = BaseAlignmentAlgorithmType::KSW2;
        dispatch.params.minimizer_algorithm      = MinimizerAlgorithmType::TWO_STACK_NTHASH;
    } else if (algo == "bioseq_ksw2_simd_dp_paper_nthash") {
        dispatch.params.chaining_algorithm       = ChainingAlgorithmType::SIMD_MM2;
        dispatch.params.base_alignment_algorithm = BaseAlignmentAlgorithmType::KSW2;
        dispatch.params.minimizer_algorithm      = MinimizerAlgorithmType::PAPER_NTHASH_SIMD;
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
    } else if (algo == "bioseq_block_simd_dp_paper_nthash") {
        dispatch.params.chaining_algorithm       = ChainingAlgorithmType::SIMD_MM2;
        dispatch.params.base_alignment_algorithm = BaseAlignmentAlgorithmType::BLOCK_ALIGNER;
        dispatch.params.minimizer_algorithm      = MinimizerAlgorithmType::PAPER_NTHASH_SIMD;
        dispatch.params.do_cigar                 = true;
    } else if (algo == "bioseq_block_vulkan_dp_vulkan_minimap_int") {
        // Historical PARTIAL-GPU probe: Vulkan minimap-int minimizers, a
        // source-direct mapped sorted-dense resident target index, Vulkan seed
        // lookup, and Vulkan tiled-DP score generation, followed by CPU mapped
        // prefix authority reads, backtracking, and CPU Block Aligner. The
        // minimizer sketch is always the fused tiled Phase-C path after
        // DOC0351; this row keeps the non-fused direct-resident seed/index
        // frontend for comparison. The only varied
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

    // C++ remains fail-safe by default (`exact_visited`). Benchmark SIMD rows
    // are explicit promotion candidates and therefore name the bounded policy;
    // the focused oracle row is the only deliberate exact SIMD exception.
    // A policy/proof mismatch is rejected later by
    // `require_selected_route_execution` rather than inferred from timing.
    if (dispatch.params.chaining_algorithm == ChainingAlgorithmType::SIMD_MM2) {
        dispatch.params.simd_chaining_policy =
            algo == "bioseq_ksw2_simd_dp_two_stack_nthash_exact_visited"
                ? SimdChainingPolicy::exact_visited
                : SimdChainingPolicy::bounded_batch_approximate;
    }
    return dispatch;
}

[[nodiscard]] constexpr bool
legacy_gpu_index_materialization_proof_accepted(
        const PartialVulkanExecutionProof& proof) noexcept {
    const bool open_addressed =
        proof.gpu_index_open_addressed_materializations == 1u
        && proof.gpu_index_sorted_dense_materializations == 0u
        && proof.seed_lookup_dispatches != 0u
        && proof.seed_open_addressed_lookup_dispatches
            == proof.seed_lookup_dispatches
        && proof.seed_sorted_dense_lookup_dispatches == 0u
        && proof.gpu_index_slot_capacity >= proof.gpu_index_unique_keys
        && proof.gpu_index_max_load_ppm != 0u
        && proof.gpu_index_max_load_ppm <= 700'000u
        && proof.gpu_index_max_probe != 0u;
    const bool sorted_dense =
        proof.gpu_index_open_addressed_materializations == 0u
        && proof.gpu_index_sorted_dense_materializations == 1u
        && proof.seed_lookup_dispatches != 0u
        && proof.seed_open_addressed_lookup_dispatches == 0u
        && proof.seed_sorted_dense_lookup_dispatches
            == proof.seed_lookup_dispatches
        && proof.gpu_index_slot_capacity == proof.gpu_index_unique_keys
        && proof.gpu_index_max_load_ppm == 1'000'000u
        && proof.gpu_index_max_probe == 0u;
    return !proof.saturated
        && proof.gpu_index_materializations == 1u
        && (open_addressed || sorted_dense)
        && proof.gpu_index_unique_keys != 0u
        && proof.gpu_index_slot_capacity != 0u
        && proof.gpu_index_logical_resident_bytes != 0u
        && proof.gpu_index_actual_allocation_bytes
            >= proof.gpu_index_logical_resident_bytes
        && proof.gpu_index_fully_touched_bytes
            == proof.gpu_index_actual_allocation_bytes
        && proof.gpu_index_staging_bytes == 0u
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
    proof.seed_lookup_dispatches = 1u;
    proof.seed_open_addressed_lookup_dispatches = 1u;
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
    proof.gpu_index_open_addressed_materializations = 0u;
    proof.gpu_index_sorted_dense_materializations = 1u;
    proof.seed_open_addressed_lookup_dispatches = 0u;
    proof.seed_sorted_dense_lookup_dispatches = 1u;
    proof.gpu_index_slot_capacity = 3u;
    proof.gpu_index_max_load_ppm = 1'000'000u;
    proof.gpu_index_max_probe = 0u;
    if (!legacy_gpu_index_materialization_proof_accepted(proof)) return false;
    proof.gpu_index_open_addressed_materializations = 1u;
    if (legacy_gpu_index_materialization_proof_accepted(proof)) return false;
    proof.gpu_index_open_addressed_materializations = 0u;
    proof.seed_open_addressed_lookup_dispatches = 1u;
    if (legacy_gpu_index_materialization_proof_accepted(proof)) return false;
    proof.seed_open_addressed_lookup_dispatches = 0u;
    proof.gpu_index_max_probe = 1u;
    if (legacy_gpu_index_materialization_proof_accepted(proof)) return false;
    proof.gpu_index_max_probe = 0u;
    proof.gpu_index_permit_releases = 0u;
    if (legacy_gpu_index_materialization_proof_accepted(proof)) return false;
    proof.gpu_index_permit_releases = 1u;
    proof.gpu_index_process_footprint_reflections = 0u;
    if (legacy_gpu_index_materialization_proof_accepted(proof)) return false;
    proof.gpu_index_process_footprint_reflections = 1u;
    proof.gpu_index_reflected_footprint_bytes = 255u;
    return !legacy_gpu_index_materialization_proof_accepted(proof);
}());

[[nodiscard]] constexpr bool
phase_c_target_index_evidence_absent(
        const PartialVulkanExecutionProof& proof) noexcept {
    return proof.target_index_build_dispatches == 0u
        && proof.gpu_index_count_phase_admissions == 0u
        && proof.gpu_index_main_peak_admissions == 0u
        && proof.gpu_index_resident_transfers == 0u
        && proof.gpu_index_adoption_rereserve_attempts == 0u
        && proof.gpu_index_adoption_successes == 0u
        && proof.gpu_index_owned_resident_bytes == 0u
        && proof.gpu_index_gate_enabled == 0u
        && proof.gpu_index_gate_word_count == 0u
        && proof.gpu_index_gate_mask == 0u
        && proof.gpu_index_gate_set_bits == 0u
        && proof.gpu_index_gate_digest_lo == 0u
        && proof.gpu_index_gate_digest_hi == 0u
        && proof.gpu_index_gate_count_dispatches == 0u
        && proof.gpu_index_gated_emit_dispatches == 0u
        && proof.gpu_index_ungated_emit_dispatches == 0u
        && proof.gpu_index_exact_count_dispatches == 0u
        && proof.gpu_index_exact_count_prefix_scan_dispatches == 0u
        && proof.gpu_index_exact_count_control_readbacks == 0u
        && proof.gpu_index_deterministic_tile_emit_dispatches == 0u
        && proof.gpu_index_atomic_append_dispatches == 0u
        && proof.gpu_index_emit_validation_control_readbacks == 0u
        && proof.gpu_index_counted_minimizer_count == 0u
        && proof.gpu_index_verified_emit_count == 0u
        && proof.gpu_index_count_emit_parity_verified == 0u
        && proof.gpu_index_target_minimizer_capacity_estimate == 0u
        && proof.gpu_index_capacity_estimate_underestimated == 0u
        && proof.gpu_index_arena_trimmed_before_permit_shrink == 0u;
}

[[nodiscard]] constexpr bool seed_sort_evidence_absent(
        const PartialVulkanExecutionProof& proof) noexcept {
    return proof.seed_bitonic_sort_dispatches == 0u
        && proof.seed_radix_sort_operations == 0u
        && proof.seed_radix_anchor_count == 0u
        && proof.seed_radix_passes == 0u
        && proof.seed_radix_histogram_dispatches == 0u
        && proof.seed_radix_scan_dispatches == 0u
        && proof.seed_radix_scatter_dispatches == 0u
        && proof.seed_host_sort_fallbacks == 0u;
}

/// Accept the DOC0384 source-direct sorted-dense operation proof without
/// applying the contradictory legacy CPU-index-release ownership contract.
/// The operation ledger publishes these source-direct counters only after its
/// terminal predicate succeeds. Generic layout/shape/allocation fields remain
/// cross-checks for N, D, layout identity, and the direct split-byte totals.
[[nodiscard]] constexpr bool
source_direct_gpu_index_materialization_proof_accepted(
        const PartialVulkanExecutionProof& proof) noexcept {
    if (proof.saturated
        || proof.gpu_index_source_direct_operations != 1u
        || proof.gpu_index_source_direct_planning_receipts != 1u
        || proof.gpu_index_source_direct_terminal_receipts != 1u
        || proof.gpu_index_source_direct_terminal_successes != 1u
        || proof.gpu_index_source_direct_required_misses != 0u
        || proof.gpu_index_source_direct_fallback_attempts != 0u
        || proof.gpu_index_source_direct_seed_slab_initial_capacity != 0u
        || !phase_c_target_index_evidence_absent(proof)
        || proof.seed_host_sort_fallbacks != 0u
        || proof.post_seed_owned_cpu_index_releases != 0u
        || proof.post_seed_owned_cpu_position_bytes_released != 0u) {
        return false;
    }

    if (proof.gpu_index_source_direct_empty_bypasses == 1u) {
        return proof.gpu_index_source_direct_materializations == 0u
            && proof.gpu_index_source_direct_zero_query_bypasses == 0u
            && proof.gpu_index_source_direct_resident_publications == 0u
            && proof.gpu_index_source_direct_logical_control_bytes == 0u
            && proof.gpu_index_source_direct_logical_position_bytes == 0u
            && proof.gpu_index_source_direct_control_requirement_bytes == 0u
            && proof.gpu_index_source_direct_position_requirement_bytes == 0u
            && proof.gpu_index_source_direct_allocation_request_bytes == 0u
            && proof.gpu_index_source_direct_actual_control_bytes == 0u
            && proof.gpu_index_source_direct_actual_position_bytes == 0u
            && proof.gpu_index_source_direct_source_releases == 0u
            && proof.gpu_index_source_direct_source_released_bytes == 0u
            && proof.gpu_index_source_direct_checksum_matches == 0u
            && proof.gpu_index_source_direct_source_traversals == 0u
            && proof.gpu_index_source_direct_mapped_traversals == 0u
            && proof.gpu_index_source_direct_reflected_authorities == 0u
            && proof.gpu_index_source_direct_retained_authorities == 0u
            && proof.gpu_index_source_direct_retained_resident_permit_bytes
                == 0u
            && proof.gpu_index_source_direct_cache_preinsertions == 0u
            && proof.gpu_index_source_direct_expected_seed_consumers == 0u
            && proof.gpu_index_source_direct_completed_seed_consumers == 0u
            && proof.gpu_index_source_direct_required_lookups == 0u
            && proof.gpu_index_source_direct_required_hits == 0u
            && proof.gpu_index_source_direct_mapped_prefix_passes == 0u
            && proof.gpu_index_source_direct_mapped_prefix_lookups == 0u
            && proof.gpu_index_source_direct_mapped_prefix_comparisons == 0u
            && proof.gpu_index_source_direct_seed_slab_requested_bytes == 0u
            && proof.gpu_index_source_direct_seed_slab_actual_bytes == 0u
            && proof.gpu_index_source_direct_seed_slab_retained_bytes == 0u
            && proof.gpu_index_source_direct_seed_slab_permit_releases == 0u
            && proof.gpu_index_source_direct_seed_reserved_bytes == 0u
            && proof.gpu_index_source_direct_seed_planned_vulkan_bytes == 0u
            && proof.gpu_index_source_direct_seed_actual_vulkan_bytes == 0u
            && proof.gpu_index_source_direct_seed_roundtrips == 0u
            && proof.gpu_index_source_direct_zero_anchor_bypasses == 0u
            && proof.gpu_index_source_direct_seed_fence_completions == 0u
            && proof.gpu_index_source_direct_cache_cleanup_releases == 0u
            && proof.gpu_index_source_direct_resident_cleanup_bytes == 0u
            && proof.gpu_index_source_direct_pipeline_completions == 0u
            && proof.chain_score_dispatches == 0u
            && seed_sort_evidence_absent(proof)
            && proof.seed_lookup_dispatches == 0u
            && proof.seed_open_addressed_lookup_dispatches == 0u
            && proof.seed_sorted_dense_lookup_dispatches == 0u
            && proof.seed_anchor_proof_operations == 0u
            && proof.seed_canonical_anchor_count == 0u
            && proof.seed_canonical_anchor_order_checks == 0u
            && proof.seed_canonical_anchor_order_accepts == 0u
            && proof.seed_canonical_tie_normalization_runs == 0u
            && proof.seed_canonical_tie_normalization_anchors == 0u
            && proof.seed_anchor_digest_lane0 == 0u
            && proof.seed_anchor_digest_lane1 == 0u
            && proof.gpu_index_materializations == 0u
            && proof.gpu_index_open_addressed_materializations == 0u
            && proof.gpu_index_sorted_dense_materializations == 0u
            && proof.gpu_index_unique_keys == 0u
            && proof.gpu_index_slot_capacity == 0u
            && proof.gpu_index_logical_resident_bytes == 0u
            && proof.gpu_index_actual_allocation_bytes == 0u
            && proof.gpu_index_fully_touched_bytes == 0u
            && proof.gpu_index_staging_bytes == 0u
            && proof.gpu_index_permit_releases == 0u
            && proof.gpu_index_process_footprint_reflections == 0u
            && proof.gpu_index_reflected_footprint_bytes == 0u
            && proof.gpu_index_max_load_ppm == 0u
            && proof.gpu_index_max_probe == 0u
            && proof.gpu_index_retained_minimizer_count == 0u
            && proof.gpu_index_distinct_minimizer_count == 0u
            && proof.post_seed_owned_gpu_cache_evictions == 0u
            && proof.post_seed_owned_gpu_resident_bytes_released == 0u;
    }
    if (proof.gpu_index_source_direct_empty_bypasses != 0u) {
        return false;
    }

    const auto max_u64 = (std::numeric_limits<std::uint64_t>::max)();
    const bool exact_direct_allocation =
        proof.gpu_index_distinct_minimizer_count != 0u
        && proof.gpu_index_distinct_minimizer_count <= max_u64 / 16u
        && proof.gpu_index_source_direct_logical_control_bytes
            == 16u * proof.gpu_index_distinct_minimizer_count
        && proof.gpu_index_retained_minimizer_count != 0u
        && proof.gpu_index_retained_minimizer_count <= max_u64 / 8u
        && proof.gpu_index_source_direct_logical_position_bytes
            == 8u * proof.gpu_index_retained_minimizer_count
        && proof.gpu_index_source_direct_logical_control_bytes
            <= max_u64
                - proof.gpu_index_source_direct_logical_position_bytes
        && proof.gpu_index_logical_resident_bytes
            == proof.gpu_index_source_direct_logical_control_bytes
                + proof.gpu_index_source_direct_logical_position_bytes
        && proof.gpu_index_source_direct_control_requirement_bytes
            == proof.gpu_index_source_direct_actual_control_bytes
        && proof.gpu_index_source_direct_actual_control_bytes
            >= proof.gpu_index_source_direct_logical_control_bytes
        && proof.gpu_index_source_direct_position_requirement_bytes
            == proof.gpu_index_source_direct_actual_position_bytes
        && proof.gpu_index_source_direct_actual_position_bytes
            >= proof.gpu_index_source_direct_logical_position_bytes
        && proof.gpu_index_source_direct_control_requirement_bytes
            <= max_u64
                - proof.gpu_index_source_direct_position_requirement_bytes
        && proof.gpu_index_source_direct_allocation_request_bytes
            == proof.gpu_index_source_direct_control_requirement_bytes
                + proof.gpu_index_source_direct_position_requirement_bytes
        && proof.gpu_index_source_direct_actual_control_bytes
            <= max_u64 - proof.gpu_index_source_direct_actual_position_bytes
        && proof.gpu_index_source_direct_allocation_request_bytes
            == proof.gpu_index_source_direct_actual_control_bytes
                + proof.gpu_index_source_direct_actual_position_bytes
        && proof.gpu_index_actual_allocation_bytes
            == proof.gpu_index_source_direct_allocation_request_bytes;
    const bool exact_sorted_dense_shape =
        proof.gpu_index_source_direct_materializations == 1u
        && proof.gpu_index_materializations == 1u
        && proof.gpu_index_open_addressed_materializations == 0u
        && proof.gpu_index_sorted_dense_materializations == 1u
        && proof.gpu_index_retained_minimizer_count != 0u
        && proof.gpu_index_distinct_minimizer_count != 0u
        && proof.gpu_index_distinct_minimizer_count
            <= proof.gpu_index_retained_minimizer_count
        && proof.gpu_index_unique_keys
            == proof.gpu_index_distinct_minimizer_count
        && proof.gpu_index_slot_capacity == proof.gpu_index_unique_keys
        && proof.gpu_index_logical_resident_bytes != 0u
        && proof.gpu_index_actual_allocation_bytes
            >= proof.gpu_index_logical_resident_bytes
        && proof.gpu_index_fully_touched_bytes
            == proof.gpu_index_actual_allocation_bytes
        && proof.gpu_index_staging_bytes == 0u
        && proof.gpu_index_max_load_ppm == 1'000'000u
        && proof.gpu_index_max_probe == 0u;
    const bool reflected_authority =
        proof.gpu_index_source_direct_reflected_authorities == 1u
        && proof.gpu_index_source_direct_retained_authorities == 0u
        && proof.gpu_index_source_direct_retained_resident_permit_bytes == 0u
        && proof.gpu_index_permit_releases == 1u
        && proof.gpu_index_process_footprint_reflections == 1u
        && proof.gpu_index_reflected_footprint_bytes
            >= proof.gpu_index_actual_allocation_bytes;
    const bool retained_authority =
        proof.gpu_index_source_direct_reflected_authorities == 0u
        && proof.gpu_index_source_direct_retained_authorities == 1u
        && proof.gpu_index_source_direct_retained_resident_permit_bytes
            == proof.gpu_index_actual_allocation_bytes
        && proof.gpu_index_permit_releases == 0u
        && proof.gpu_index_process_footprint_reflections == 0u
        && proof.gpu_index_reflected_footprint_bytes == 0u;
    const bool exact_required_cache_use =
        proof.gpu_index_source_direct_zero_query_bypasses == 0u
        && proof.gpu_index_source_direct_expected_seed_consumers != 0u
        && proof.gpu_index_source_direct_completed_seed_consumers
            == proof.gpu_index_source_direct_expected_seed_consumers
        && proof.gpu_index_source_direct_required_lookups
            == proof.gpu_index_source_direct_completed_seed_consumers
        && proof.gpu_index_source_direct_required_hits
            == proof.gpu_index_source_direct_required_lookups
        && proof.gpu_index_source_direct_required_lookups
            <= (std::numeric_limits<std::uint64_t>::max)() / 2u
        && proof.gpu_index_source_direct_mapped_prefix_passes
            == 2u * proof.gpu_index_source_direct_required_lookups
        && (proof.gpu_index_source_direct_mapped_prefix_lookups == 0u
            ? proof.gpu_index_source_direct_mapped_prefix_comparisons == 0u
            : proof.gpu_index_source_direct_mapped_prefix_comparisons
                >= proof.gpu_index_source_direct_mapped_prefix_lookups);
    const bool exact_anchor_proof =
        proof.seed_anchor_proof_operations
            == proof.gpu_index_source_direct_seed_roundtrips
        && proof.seed_canonical_anchor_order_checks
            == proof.seed_anchor_proof_operations
        && proof.seed_canonical_anchor_order_accepts
            == proof.seed_canonical_anchor_order_checks
        && (proof.seed_anchor_proof_operations == 0u
            ? proof.seed_anchor_digest_lane0 == 0u
                && proof.seed_anchor_digest_lane1 == 0u
            : proof.seed_anchor_digest_lane0 != 0u
                && proof.seed_anchor_digest_lane1 != 0u);
    const bool positive_seed_slab =
        proof.gpu_index_source_direct_mapped_prefix_lookups != 0u
        && proof.gpu_index_source_direct_seed_slab_requested_bytes != 0u
        && proof.gpu_index_source_direct_seed_slab_requested_bytes
            == proof.gpu_index_source_direct_seed_slab_actual_bytes
        && proof.gpu_index_source_direct_seed_slab_actual_bytes
            == proof.gpu_index_source_direct_seed_slab_retained_bytes
        && proof.gpu_index_source_direct_seed_slab_permit_releases == 1u
        && proof.gpu_index_source_direct_seed_planned_vulkan_bytes != 0u
        && proof.gpu_index_source_direct_seed_planned_vulkan_bytes
            == proof.gpu_index_source_direct_seed_actual_vulkan_bytes
        && proof.gpu_index_source_direct_seed_planned_vulkan_bytes
            <= max_u64
                - proof.gpu_index_source_direct_seed_slab_actual_bytes
        && proof.gpu_index_source_direct_seed_reserved_bytes
            >= proof.gpu_index_source_direct_seed_planned_vulkan_bytes
                + proof.gpu_index_source_direct_seed_slab_actual_bytes
        && proof.gpu_index_source_direct_seed_roundtrips
            == proof.gpu_index_source_direct_required_lookups
        && proof.gpu_index_source_direct_zero_anchor_bypasses == 0u
        && proof.seed_canonical_anchor_count != 0u
        && proof.seed_lookup_dispatches
            == proof.gpu_index_source_direct_seed_roundtrips
        && proof.seed_open_addressed_lookup_dispatches == 0u
        && proof.seed_sorted_dense_lookup_dispatches
            == proof.seed_lookup_dispatches
        && proof.gpu_index_source_direct_seed_fence_completions
            == proof.gpu_index_source_direct_seed_roundtrips;
    const bool zero_anchor_seed =
        proof.gpu_index_source_direct_seed_slab_requested_bytes == 0u
        && proof.gpu_index_source_direct_seed_slab_actual_bytes == 0u
        && proof.gpu_index_source_direct_seed_slab_retained_bytes == 0u
        && proof.gpu_index_source_direct_seed_slab_permit_releases == 0u
        && proof.gpu_index_source_direct_seed_reserved_bytes == 0u
        && proof.gpu_index_source_direct_seed_planned_vulkan_bytes == 0u
        && proof.gpu_index_source_direct_seed_actual_vulkan_bytes == 0u
        && proof.gpu_index_source_direct_seed_roundtrips
            == proof.gpu_index_source_direct_required_lookups
        && proof.gpu_index_source_direct_zero_anchor_bypasses
            == proof.gpu_index_source_direct_seed_roundtrips
        && proof.seed_canonical_anchor_count == 0u
        && proof.seed_lookup_dispatches == 0u
        && proof.seed_open_addressed_lookup_dispatches == 0u
        && proof.seed_sorted_dense_lookup_dispatches == 0u
        && seed_sort_evidence_absent(proof)
        && proof.gpu_index_source_direct_seed_fence_completions == 0u;

    const bool zero_query_seed =
        proof.gpu_index_source_direct_zero_query_bypasses == 1u
        && proof.gpu_index_source_direct_expected_seed_consumers == 0u
        && proof.gpu_index_source_direct_completed_seed_consumers == 0u
        && proof.gpu_index_source_direct_required_lookups == 0u
        && proof.gpu_index_source_direct_required_hits == 0u
        && proof.gpu_index_source_direct_required_misses == 0u
        && proof.gpu_index_source_direct_mapped_prefix_passes == 0u
        && proof.gpu_index_source_direct_mapped_prefix_lookups == 0u
        && proof.gpu_index_source_direct_mapped_prefix_comparisons == 0u
        && proof.gpu_index_source_direct_seed_slab_requested_bytes == 0u
        && proof.gpu_index_source_direct_seed_slab_actual_bytes == 0u
        && proof.gpu_index_source_direct_seed_slab_retained_bytes == 0u
        && proof.gpu_index_source_direct_seed_slab_permit_releases == 0u
        && proof.gpu_index_source_direct_seed_reserved_bytes == 0u
        && proof.gpu_index_source_direct_seed_planned_vulkan_bytes == 0u
        && proof.gpu_index_source_direct_seed_actual_vulkan_bytes == 0u
        && proof.gpu_index_source_direct_seed_roundtrips == 0u
        && proof.gpu_index_source_direct_zero_anchor_bypasses == 0u
        && proof.gpu_index_source_direct_seed_fence_completions == 0u
        && proof.seed_lookup_dispatches == 0u
        && proof.seed_open_addressed_lookup_dispatches == 0u
        && proof.seed_sorted_dense_lookup_dispatches == 0u
        && seed_sort_evidence_absent(proof)
        && proof.chain_score_dispatches == 0u
        && proof.seed_anchor_proof_operations == 0u
        && proof.seed_canonical_anchor_count == 0u
        && proof.seed_canonical_anchor_order_checks == 0u
        && proof.seed_canonical_anchor_order_accepts == 0u
        && proof.seed_canonical_tie_normalization_runs == 0u
        && proof.seed_canonical_tie_normalization_anchors == 0u
        && proof.seed_anchor_digest_lane0 == 0u
        && proof.seed_anchor_digest_lane1 == 0u
        && proof.gpu_index_source_direct_pipeline_completions == 0u;
    const bool seeded_pipeline =
        proof.gpu_index_source_direct_pipeline_completions == 1u
        && exact_required_cache_use
        && exact_anchor_proof
        && (positive_seed_slab || zero_anchor_seed);

    return exact_direct_allocation
        && exact_sorted_dense_shape
        && proof.gpu_index_source_direct_resident_publications == 1u
        && proof.gpu_index_source_direct_source_releases == 1u
        && proof.gpu_index_source_direct_source_released_bytes != 0u
        && proof.gpu_index_source_direct_checksum_matches == 1u
        && proof.gpu_index_source_direct_source_traversals == 1u
        && proof.gpu_index_source_direct_mapped_traversals == 1u
        && (reflected_authority != retained_authority)
        && proof.gpu_index_source_direct_cache_preinsertions == 1u
        && (seeded_pipeline || zero_query_seed)
        && proof.gpu_index_source_direct_cache_cleanup_releases == 1u
        && proof.gpu_index_source_direct_resident_cleanup_bytes
            == proof.gpu_index_actual_allocation_bytes;
}

static_assert([] {
    const auto valid_reflected = [] {
        PartialVulkanExecutionProof proof{};
        proof.seed_lookup_dispatches = 1u;
        proof.seed_sorted_dense_lookup_dispatches = 1u;
        proof.seed_anchor_proof_operations = 1u;
        proof.seed_canonical_anchor_count = 1'337u;
        proof.seed_canonical_anchor_order_checks = 1u;
        proof.seed_canonical_anchor_order_accepts = 1u;
        proof.seed_anchor_digest_lane0 = 0x0123456789abcdefull;
        proof.seed_anchor_digest_lane1 = 0xfedcba9876543210ull;
        proof.gpu_index_materializations = 1u;
        proof.gpu_index_sorted_dense_materializations = 1u;
        proof.gpu_index_unique_keys = 17u;
        proof.gpu_index_slot_capacity = 17u;
        proof.gpu_index_logical_resident_bytes = 696u;
        proof.gpu_index_actual_allocation_bytes = 4'096u;
        proof.gpu_index_fully_touched_bytes = 4'096u;
        proof.gpu_index_permit_releases = 1u;
        proof.gpu_index_process_footprint_reflections = 1u;
        proof.gpu_index_reflected_footprint_bytes = 4'096u;
        proof.gpu_index_max_load_ppm = 1'000'000u;
        proof.gpu_index_retained_minimizer_count = 53u;
        proof.gpu_index_distinct_minimizer_count = 17u;
        proof.gpu_index_source_direct_operations = 1u;
        proof.gpu_index_source_direct_planning_receipts = 1u;
        proof.gpu_index_source_direct_terminal_receipts = 1u;
        proof.gpu_index_source_direct_terminal_successes = 1u;
        proof.gpu_index_source_direct_materializations = 1u;
        proof.gpu_index_source_direct_resident_publications = 1u;
        proof.gpu_index_source_direct_logical_control_bytes = 272u;
        proof.gpu_index_source_direct_logical_position_bytes = 424u;
        proof.gpu_index_source_direct_control_requirement_bytes = 1'024u;
        proof.gpu_index_source_direct_position_requirement_bytes = 3'072u;
        proof.gpu_index_source_direct_allocation_request_bytes = 4'096u;
        proof.gpu_index_source_direct_actual_control_bytes = 1'024u;
        proof.gpu_index_source_direct_actual_position_bytes = 3'072u;
        proof.gpu_index_source_direct_source_releases = 1u;
        proof.gpu_index_source_direct_source_released_bytes = 512u;
        proof.gpu_index_source_direct_checksum_matches = 1u;
        proof.gpu_index_source_direct_source_traversals = 1u;
        proof.gpu_index_source_direct_mapped_traversals = 1u;
        proof.gpu_index_source_direct_reflected_authorities = 1u;
        proof.gpu_index_source_direct_cache_preinsertions = 1u;
        proof.gpu_index_source_direct_expected_seed_consumers = 1u;
        proof.gpu_index_source_direct_completed_seed_consumers = 1u;
        proof.gpu_index_source_direct_required_lookups = 1u;
        proof.gpu_index_source_direct_required_hits = 1u;
        proof.gpu_index_source_direct_mapped_prefix_passes = 2u;
        proof.gpu_index_source_direct_mapped_prefix_lookups = 20u;
        proof.gpu_index_source_direct_mapped_prefix_comparisons = 80u;
        proof.gpu_index_source_direct_seed_slab_requested_bytes = 1'024u;
        proof.gpu_index_source_direct_seed_slab_actual_bytes = 1'024u;
        proof.gpu_index_source_direct_seed_slab_retained_bytes = 1'024u;
        proof.gpu_index_source_direct_seed_slab_permit_releases = 1u;
        proof.gpu_index_source_direct_seed_reserved_bytes = 4'096u;
        proof.gpu_index_source_direct_seed_planned_vulkan_bytes = 2'048u;
        proof.gpu_index_source_direct_seed_actual_vulkan_bytes = 2'048u;
        proof.gpu_index_source_direct_seed_roundtrips = 1u;
        proof.gpu_index_source_direct_seed_fence_completions = 1u;
        proof.gpu_index_source_direct_cache_cleanup_releases = 1u;
        proof.gpu_index_source_direct_resident_cleanup_bytes = 4'096u;
        proof.gpu_index_source_direct_pipeline_completions = 1u;
        return proof;
    };
    const auto reflected = valid_reflected();
    if (!source_direct_gpu_index_materialization_proof_accepted(reflected)) {
        return false;
    }
    auto strict_reflected = reflected;
    strict_reflected.query_sketch_dispatches = 1u;
    strict_reflected.target_sketch_dispatches = 1u;
    strict_reflected.chain_score_dispatches = 1u;
    if (!strict_reflected.exact_source_direct_stage_mask_accepted(
            true, true, true, true)
        || strict_reflected.exact_stage_mask_accepted(
            true, true, false, true, true)) {
        return false;
    }

    auto retained = reflected;
    retained.gpu_index_source_direct_reflected_authorities = 0u;
    retained.gpu_index_source_direct_retained_authorities = 1u;
    retained.gpu_index_source_direct_retained_resident_permit_bytes = 4'096u;
    retained.gpu_index_permit_releases = 0u;
    retained.gpu_index_process_footprint_reflections = 0u;
    retained.gpu_index_reflected_footprint_bytes = 0u;
    if (!source_direct_gpu_index_materialization_proof_accepted(retained)) {
        return false;
    }

    auto zero_anchor = reflected;
    zero_anchor.seed_lookup_dispatches = 0u;
    zero_anchor.seed_sorted_dense_lookup_dispatches = 0u;
    zero_anchor.gpu_index_source_direct_seed_slab_requested_bytes = 0u;
    zero_anchor.gpu_index_source_direct_seed_slab_actual_bytes = 0u;
    zero_anchor.gpu_index_source_direct_seed_slab_retained_bytes = 0u;
    zero_anchor.gpu_index_source_direct_seed_slab_permit_releases = 0u;
    zero_anchor.gpu_index_source_direct_seed_reserved_bytes = 0u;
    zero_anchor.gpu_index_source_direct_seed_planned_vulkan_bytes = 0u;
    zero_anchor.gpu_index_source_direct_seed_actual_vulkan_bytes = 0u;
    zero_anchor.gpu_index_source_direct_zero_anchor_bypasses = 1u;
    zero_anchor.gpu_index_source_direct_seed_fence_completions = 0u;
    zero_anchor.seed_canonical_anchor_count = 0u;
    if (!source_direct_gpu_index_materialization_proof_accepted(zero_anchor)) {
        return false;
    }
    auto empty_minimizer_zero_anchor = zero_anchor;
    empty_minimizer_zero_anchor.
        gpu_index_source_direct_mapped_prefix_lookups = 0u;
    empty_minimizer_zero_anchor.
        gpu_index_source_direct_mapped_prefix_comparisons = 0u;
    if (!source_direct_gpu_index_materialization_proof_accepted(
            empty_minimizer_zero_anchor)) {
        return false;
    }
    empty_minimizer_zero_anchor.
        gpu_index_source_direct_mapped_prefix_passes = 1u;
    if (source_direct_gpu_index_materialization_proof_accepted(
            empty_minimizer_zero_anchor)) {
        return false;
    }
    auto invalid_zero_anchor_sort = zero_anchor;
    invalid_zero_anchor_sort.seed_bitonic_sort_dispatches = 1u;
    if (source_direct_gpu_index_materialization_proof_accepted(
            invalid_zero_anchor_sort)) {
        return false;
    }
    auto invalid_zero_anchor_allocation = zero_anchor;
    invalid_zero_anchor_allocation.
        gpu_index_source_direct_seed_reserved_bytes = 1u;
    invalid_zero_anchor_allocation.
        gpu_index_source_direct_seed_planned_vulkan_bytes = 1u;
    invalid_zero_anchor_allocation.
        gpu_index_source_direct_seed_actual_vulkan_bytes = 1u;
    if (source_direct_gpu_index_materialization_proof_accepted(
            invalid_zero_anchor_allocation)) {
        return false;
    }

    auto zero_query = reflected;
    zero_query.seed_lookup_dispatches = 0u;
    zero_query.seed_sorted_dense_lookup_dispatches = 0u;
    zero_query.seed_anchor_proof_operations = 0u;
    zero_query.seed_canonical_anchor_count = 0u;
    zero_query.seed_canonical_anchor_order_checks = 0u;
    zero_query.seed_canonical_anchor_order_accepts = 0u;
    zero_query.seed_anchor_digest_lane0 = 0u;
    zero_query.seed_anchor_digest_lane1 = 0u;
    zero_query.gpu_index_source_direct_zero_query_bypasses = 1u;
    zero_query.gpu_index_source_direct_expected_seed_consumers = 0u;
    zero_query.gpu_index_source_direct_completed_seed_consumers = 0u;
    zero_query.gpu_index_source_direct_required_lookups = 0u;
    zero_query.gpu_index_source_direct_required_hits = 0u;
    zero_query.gpu_index_source_direct_mapped_prefix_passes = 0u;
    zero_query.gpu_index_source_direct_mapped_prefix_lookups = 0u;
    zero_query.gpu_index_source_direct_mapped_prefix_comparisons = 0u;
    zero_query.gpu_index_source_direct_seed_slab_requested_bytes = 0u;
    zero_query.gpu_index_source_direct_seed_slab_actual_bytes = 0u;
    zero_query.gpu_index_source_direct_seed_slab_retained_bytes = 0u;
    zero_query.gpu_index_source_direct_seed_slab_permit_releases = 0u;
    zero_query.gpu_index_source_direct_seed_reserved_bytes = 0u;
    zero_query.gpu_index_source_direct_seed_planned_vulkan_bytes = 0u;
    zero_query.gpu_index_source_direct_seed_actual_vulkan_bytes = 0u;
    zero_query.gpu_index_source_direct_seed_roundtrips = 0u;
    zero_query.gpu_index_source_direct_seed_fence_completions = 0u;
    zero_query.gpu_index_source_direct_pipeline_completions = 0u;
    if (!source_direct_gpu_index_materialization_proof_accepted(zero_query)) {
        return false;
    }
    auto invalid_zero_query_sort = zero_query;
    invalid_zero_query_sort.seed_radix_sort_operations = 1u;
    if (source_direct_gpu_index_materialization_proof_accepted(
            invalid_zero_query_sort)) {
        return false;
    }
    auto invalid_zero_query_chain = zero_query;
    invalid_zero_query_chain.chain_score_dispatches = 1u;
    if (source_direct_gpu_index_materialization_proof_accepted(
            invalid_zero_query_chain)) {
        return false;
    }
    zero_query.gpu_index_source_direct_required_hits = 1u;
    if (source_direct_gpu_index_materialization_proof_accepted(zero_query)) {
        return false;
    }

    auto invalid = reflected;
    invalid.post_seed_owned_cpu_index_releases = 1u;
    if (source_direct_gpu_index_materialization_proof_accepted(invalid)) {
        return false;
    }
    invalid = reflected;
    invalid.gpu_index_source_direct_required_hits = 0u;
    if (source_direct_gpu_index_materialization_proof_accepted(invalid)) {
        return false;
    }
    invalid = reflected;
    invalid.gpu_index_source_direct_required_misses = 1u;
    if (source_direct_gpu_index_materialization_proof_accepted(invalid)) {
        return false;
    }
    invalid = reflected;
    invalid.gpu_index_source_direct_completed_seed_consumers = 0u;
    if (source_direct_gpu_index_materialization_proof_accepted(invalid)) {
        return false;
    }
    invalid = reflected;
    invalid.gpu_index_source_direct_mapped_prefix_passes = 1u;
    if (source_direct_gpu_index_materialization_proof_accepted(invalid)) {
        return false;
    }
    invalid = reflected;
    invalid.gpu_index_source_direct_retained_authorities = 1u;
    if (source_direct_gpu_index_materialization_proof_accepted(invalid)) {
        return false;
    }
    invalid = reflected;
    invalid.gpu_index_permit_releases = 0u;
    if (source_direct_gpu_index_materialization_proof_accepted(invalid)) {
        return false;
    }
    invalid = retained;
    invalid.gpu_index_permit_releases = 1u;
    if (source_direct_gpu_index_materialization_proof_accepted(invalid)) {
        return false;
    }
    invalid = reflected;
    invalid.gpu_index_gate_enabled = 1u;
    if (source_direct_gpu_index_materialization_proof_accepted(invalid)) {
        return false;
    }
    invalid = reflected;
    invalid.gpu_index_source_direct_terminal_receipts = 0u;
    if (source_direct_gpu_index_materialization_proof_accepted(invalid)) {
        return false;
    }
    invalid = reflected;
    invalid.gpu_index_source_direct_seed_slab_actual_bytes = 1'023u;
    if (source_direct_gpu_index_materialization_proof_accepted(invalid)) {
        return false;
    }
    invalid = reflected;
    invalid.gpu_index_source_direct_actual_position_bytes = 3'071u;
    if (source_direct_gpu_index_materialization_proof_accepted(invalid)) {
        return false;
    }
    invalid = reflected;
    invalid.gpu_index_source_direct_seed_slab_initial_capacity = 1u;
    if (source_direct_gpu_index_materialization_proof_accepted(invalid)) {
        return false;
    }

    PartialVulkanExecutionProof empty{};
    empty.gpu_index_source_direct_operations = 1u;
    empty.gpu_index_source_direct_empty_bypasses = 1u;
    empty.gpu_index_source_direct_planning_receipts = 1u;
    empty.gpu_index_source_direct_terminal_receipts = 1u;
    empty.gpu_index_source_direct_terminal_successes = 1u;
    if (!source_direct_gpu_index_materialization_proof_accepted(empty)) {
        return false;
    }
    constexpr std::array empty_zero_fields{
        &PartialVulkanExecutionProof::chain_score_dispatches,
        &PartialVulkanExecutionProof::seed_bitonic_sort_dispatches,
        &PartialVulkanExecutionProof::seed_radix_sort_operations,
        &PartialVulkanExecutionProof::seed_radix_anchor_count,
        &PartialVulkanExecutionProof::seed_radix_passes,
        &PartialVulkanExecutionProof::seed_radix_histogram_dispatches,
        &PartialVulkanExecutionProof::seed_radix_scan_dispatches,
        &PartialVulkanExecutionProof::seed_radix_scatter_dispatches,
        &PartialVulkanExecutionProof::gpu_index_permit_releases,
        &PartialVulkanExecutionProof::gpu_index_max_load_ppm,
        &PartialVulkanExecutionProof::gpu_index_max_probe,
        &PartialVulkanExecutionProof::gpu_index_count_phase_admissions,
        &PartialVulkanExecutionProof::gpu_index_main_peak_admissions,
        &PartialVulkanExecutionProof::gpu_index_resident_transfers,
        &PartialVulkanExecutionProof::gpu_index_adoption_rereserve_attempts,
        &PartialVulkanExecutionProof::gpu_index_adoption_successes,
        &PartialVulkanExecutionProof::gpu_index_owned_resident_bytes,
        &PartialVulkanExecutionProof::gpu_index_gate_enabled,
        &PartialVulkanExecutionProof::gpu_index_gate_word_count,
        &PartialVulkanExecutionProof::gpu_index_gate_mask,
        &PartialVulkanExecutionProof::gpu_index_gate_set_bits,
        &PartialVulkanExecutionProof::gpu_index_gate_digest_lo,
        &PartialVulkanExecutionProof::gpu_index_gate_digest_hi,
        &PartialVulkanExecutionProof::gpu_index_gate_count_dispatches,
        &PartialVulkanExecutionProof::gpu_index_gated_emit_dispatches,
        &PartialVulkanExecutionProof::gpu_index_ungated_emit_dispatches,
        &PartialVulkanExecutionProof::gpu_index_exact_count_dispatches,
        &PartialVulkanExecutionProof::gpu_index_exact_count_prefix_scan_dispatches,
        &PartialVulkanExecutionProof::gpu_index_exact_count_control_readbacks,
        &PartialVulkanExecutionProof::gpu_index_deterministic_tile_emit_dispatches,
        &PartialVulkanExecutionProof::gpu_index_atomic_append_dispatches,
        &PartialVulkanExecutionProof::gpu_index_emit_validation_control_readbacks,
        &PartialVulkanExecutionProof::gpu_index_counted_minimizer_count,
        &PartialVulkanExecutionProof::gpu_index_verified_emit_count,
        &PartialVulkanExecutionProof::gpu_index_count_emit_parity_verified,
        &PartialVulkanExecutionProof::gpu_index_target_minimizer_capacity_estimate,
        &PartialVulkanExecutionProof::gpu_index_capacity_estimate_underestimated,
        &PartialVulkanExecutionProof::gpu_index_arena_trimmed_before_permit_shrink,
    };
    for (const auto field : empty_zero_fields) {
        auto contradictory_empty = empty;
        contradictory_empty.*field = 1u;
        if (source_direct_gpu_index_materialization_proof_accepted(
                contradictory_empty)) {
            return false;
        }
    }
    empty.gpu_index_source_direct_resident_publications = 1u;
    return !source_direct_gpu_index_materialization_proof_accepted(empty);
}());

static_assert(bioseq::gpu::vulkan::kPhaseCQueryGateWordCount == 2'097'152u);
static_assert(bioseq::gpu::vulkan::kPhaseCQueryGateMask == 67'108'863u);

[[nodiscard]] constexpr bool
phase_c_gpu_index_ownership_proof_accepted(
        const PartialVulkanExecutionProof& proof) noexcept {
    return !proof.saturated
        && proof.exact_target_index_route_accepted()
        && proof.target_sketch_dispatches == 1u
        && proof.target_index_build_dispatches == 1u
        && proof.seed_lookup_dispatches != 0u
        && proof.seed_open_addressed_lookup_dispatches == 0u
        && proof.seed_sorted_dense_lookup_dispatches
            == proof.seed_lookup_dispatches
        && proof.seed_anchor_proof_operations
            == proof.seed_lookup_dispatches
        && proof.seed_canonical_anchor_count != 0u
        && proof.seed_canonical_anchor_order_checks
            == proof.seed_anchor_proof_operations
        && proof.seed_canonical_anchor_order_accepts
            == proof.seed_canonical_anchor_order_checks
        && proof.seed_anchor_digest_lane0 != 0u
        && proof.seed_anchor_digest_lane1 != 0u
        && proof.gpu_index_count_phase_admissions == 1u
        && proof.gpu_index_main_peak_admissions == 1u
        && proof.gpu_index_resident_transfers == 1u
        && proof.gpu_index_adoption_rereserve_attempts == 0u
        && proof.gpu_index_adoption_successes == 1u
        && proof.gpu_index_owned_resident_bytes != 0u
        && proof.gpu_index_gate_enabled == 0u
        && proof.gpu_index_gate_word_count == 0u
        && proof.gpu_index_gate_mask == 0u
        && proof.gpu_index_gate_set_bits == 0u
        && proof.gpu_index_gate_count_dispatches == 0u
        && proof.gpu_index_gated_emit_dispatches == 0u
        && proof.gpu_index_ungated_emit_dispatches == 1u
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
    proof.seed_lookup_dispatches = 1u;
    proof.seed_sorted_dense_lookup_dispatches = 1u;
    proof.seed_anchor_proof_operations = 1u;
    proof.seed_canonical_anchor_count = 53u;
    proof.seed_canonical_anchor_order_checks = 1u;
    proof.seed_canonical_anchor_order_accepts = 1u;
    proof.seed_anchor_digest_lane0 = 0x0123456789abcdefull;
    proof.seed_anchor_digest_lane1 = 0xfedcba9876543210ull;
    proof.gpu_index_count_phase_admissions = 1u;
    proof.gpu_index_main_peak_admissions = 1u;
    proof.gpu_index_resident_transfers = 1u;
    proof.gpu_index_adoption_successes = 1u;
    proof.gpu_index_owned_resident_bytes = 4096u;
    proof.gpu_index_ungated_emit_dispatches = 1u;
    proof.gpu_index_exact_count_dispatches = 1u;
    proof.gpu_index_exact_count_prefix_scan_dispatches = 3u;
    proof.gpu_index_exact_count_control_readbacks = 1u;
    proof.gpu_index_deterministic_tile_emit_dispatches = 1u;
    proof.gpu_index_atomic_append_dispatches = 0u;
    proof.gpu_index_emit_validation_control_readbacks = 1u;
    proof.gpu_index_counted_minimizer_count = 53u;
    proof.gpu_index_verified_emit_count = 53u;
    proof.gpu_index_count_emit_parity_verified = 1u;
    proof.gpu_index_target_minimizer_capacity_estimate = 100u;
    proof.gpu_index_capacity_estimate_underestimated = 0u;
    proof.gpu_index_arena_trimmed_before_permit_shrink = 1u;
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
    invalid.gpu_index_gated_emit_dispatches = 1u;
    if (phase_c_gpu_index_ownership_proof_accepted(invalid)) return false;
    invalid = proof;
    invalid.gpu_index_atomic_append_dispatches = 1u;
    if (phase_c_gpu_index_ownership_proof_accepted(invalid)) return false;
    invalid = proof;
    invalid.gpu_index_count_emit_parity_verified = 0u;
    if (phase_c_gpu_index_ownership_proof_accepted(invalid)) return false;
    invalid = proof;
    invalid.seed_open_addressed_lookup_dispatches = 1u;
    if (phase_c_gpu_index_ownership_proof_accepted(invalid)) return false;
    invalid = proof;
    invalid.gpu_index_gate_word_count = 1u;
    if (phase_c_gpu_index_ownership_proof_accepted(invalid)) return false;
    invalid = proof;
    invalid.gpu_index_gate_mask = 1u;
    if (phase_c_gpu_index_ownership_proof_accepted(invalid)) return false;
    invalid = proof;
    invalid.gpu_index_gate_set_bits = 1u;
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

[[nodiscard]] constexpr bool exact_b8_execution_accepted(
        const TwoStackExecutionProof& proof) noexcept {
    return proof.accepted(/*require_nonempty_vector_work=*/true)
        && proof.generic_u64_kernel_calls == 0
        && proof.generic_u64_candidate_blocks == 0
        && proof.generic_u64_lane_visits == 0
        && proof.k15_w10_b8_u32_hash_kernel_calls > 0
        && proof.k15_w10_b8_u32_hash_candidate_blocks
            == proof.vector_batches
        && proof.k15_w10_b8_u32_hash_lane_visits
            == proof.vector_lanes_visited
        && proof.k15_w10_b8_packed22_groups
            == proof.k15_w10_b8_u32_hash_candidate_blocks
        && proof.k15_w10_b8_packed22_lane_visits
            == proof.k15_w10_b8_u32_hash_lane_visits;
}

[[nodiscard]] constexpr bool exact_resident_dual_vector_stream_accepted(
        const TwoStackExecutionProof& proof) noexcept {
    return proof.accepted(/*require_nonempty_vector_work=*/true)
        && proof.resident_vector_stream_kernel_calls > 0
        && proof.resident_vector_stream_single_calls == 0
        && proof.resident_vector_stream_dual_calls
            == proof.resident_vector_stream_kernel_calls
        && proof.resident_vector_stream_nonempty_work_calls
            == proof.resident_vector_stream_kernel_calls
        && proof.resident_vector_stream_input_bp == proof.bp_consumed
        && proof.resident_vector_stream_minimizers_emitted
            == proof.minimizers_emitted
        && proof.generic_u64_kernel_calls == 0
        && proof.k15_w10_b8_u32_hash_kernel_calls == 0;
}

[[nodiscard]] constexpr bool exact_fast_execution_accepted(
        const TwoStackExecutionProof& proof) noexcept {
    return exact_b8_execution_accepted(proof)
        || exact_resident_dual_vector_stream_accepted(proof);
}

static_assert([] {
    TwoStackExecutionProof proof{};
    proof.bp_consumed = 4096u;
    proof.minimizers_emitted = 613u;
    proof.resident_vector_stream_kernel_calls = 1u;
    proof.resident_vector_stream_dual_calls = 1u;
    proof.resident_vector_stream_nonempty_work_calls = 1u;
    proof.resident_vector_stream_input_bp = proof.bp_consumed;
    proof.resident_vector_stream_minimizers_emitted =
        proof.minimizers_emitted;
    if (!exact_fast_execution_accepted(proof)) return false;
    auto zero_output = proof;
    zero_output.minimizers_emitted = 0u;
    zero_output.resident_vector_stream_minimizers_emitted = 0u;
    if (exact_fast_execution_accepted(zero_output)) return false;
    proof.generic_u64_kernel_calls = 1u;
    return !exact_fast_execution_accepted(proof);
}());

[[nodiscard]] constexpr bool scalar_minimizer_dimensions_absent(
        const ScalarPipelineExecutionProof& proof) noexcept {
    using ScalarMinimizerExecutionProof =
        bioseq::simd::HWY_NAMESPACE::ScalarMinimizerExecutionProof;
    return proof.query_minimizers == ScalarMinimizerExecutionProof{}
        && proof.target_minimizers == ScalarMinimizerExecutionProof{};
}

[[nodiscard]] constexpr bool scalar_seed_dimension_absent(
        const ScalarPipelineExecutionProof& proof) noexcept {
    return proof.seed_lookup_calls == 0u
        && proof.seed_query_minimizers == 0u;
}

[[nodiscard]] constexpr bool scalar_chain_dimension_absent(
        const ScalarPipelineExecutionProof& proof) noexcept {
    return proof.chain_score_calls == 0u
        && proof.chain_backtrack_calls == 0u
        && proof.chain_anchor_inputs == 0u;
}

[[nodiscard]] constexpr bool scalar_ksw2_dimension_absent(
        const ScalarPipelineExecutionProof& proof) noexcept {
    return proof.ksw2_left_calls == 0u
        && proof.ksw2_gap_calls == 0u
        && proof.ksw2_right_calls == 0u
        && proof.ksw2_input_cell_area == 0u;
}

[[nodiscard]] constexpr bool scalar_seed_dimension_accepted(
        const ScalarPipelineExecutionProof& proof,
        const bool require_nonempty_work) noexcept {
    return !proof.saturated
        && proof.seed_lookup_calls > 0u
        && (!require_nonempty_work || proof.seed_query_minimizers > 0u);
}

/// The generic pipeline executes CPU `collect_seed_hits` unless the statement
/// explicitly selects the strict Vulkan seed path. A Vulkan chaining-only
/// hybrid still uses CPU seed lookup and therefore must carry scalar seed proof.
[[nodiscard]] constexpr bool cpu_seed_lookup_selected(
        const Minimap2Params& params) noexcept {
    return !(params.vulkan_enabled && params.vulkan_seed_hits_enabled);
}

[[nodiscard]] constexpr bool scalar_chain_dimension_accepted(
        const ScalarPipelineExecutionProof& proof,
        const bool require_nonempty_work) noexcept {
    const bool calls_are_paired =
        proof.chain_score_calls == proof.chain_backtrack_calls;
    const bool has_calls = proof.chain_score_calls != 0u;
    const bool work_matches_calls = has_calls
        == (proof.chain_anchor_inputs != 0u);
    return !proof.saturated
        && calls_are_paired
        && work_matches_calls
        && (has_calls || !require_nonempty_work);
}

[[nodiscard]] constexpr bool scalar_ksw2_dimension_accepted(
        const ScalarPipelineExecutionProof& proof,
        const bool require_nonempty_work) noexcept {
    const bool has_calls = proof.ksw2_left_calls != 0u
        || proof.ksw2_gap_calls != 0u
        || proof.ksw2_right_calls != 0u;
    return !proof.saturated
        && (has_calls || !require_nonempty_work)
        && (!require_nonempty_work || proof.ksw2_input_cell_area > 0u)
        && (has_calls || proof.ksw2_input_cell_area == 0u);
}

static_assert([] {
    ScalarPipelineExecutionProof proof{};
    if (!scalar_minimizer_dimensions_absent(proof)
        || !scalar_seed_dimension_absent(proof)
        || !scalar_chain_dimension_absent(proof)
        || !scalar_ksw2_dimension_absent(proof)) {
        return false;
    }
    proof.record_seed_lookup(7u);
    proof.record_scalar_chain(11u);
    proof.record_ksw2_call(
        bioseq::simd::HWY_NAMESPACE::ScalarKsw2Phase::gap, 3u, 5u);
    if (!scalar_seed_dimension_accepted(proof, true)
        || !scalar_chain_dimension_accepted(proof, true)
        || !scalar_ksw2_dimension_accepted(proof, true)
        || scalar_seed_dimension_absent(proof)
        || scalar_chain_dimension_absent(proof)
        || scalar_ksw2_dimension_absent(proof)) {
        return false;
    }
    auto missing_seed = proof;
    missing_seed.seed_lookup_calls = 0u;
    if (scalar_seed_dimension_accepted(missing_seed, true)) return false;
    auto empty_seed_work = proof;
    empty_seed_work.seed_query_minimizers = 0u;
    if (scalar_seed_dimension_accepted(empty_seed_work, true)
        || !scalar_seed_dimension_accepted(empty_seed_work, false)) {
        return false;
    }
    auto saturated_seed = proof;
    saturated_seed.saturated = true;
    return !scalar_seed_dimension_accepted(saturated_seed, false);
}());

static_assert([] {
    Minimap2Params params{};
    if (!cpu_seed_lookup_selected(params)) return false;
    params.vulkan_enabled = false;
    params.vulkan_seed_hits_enabled = true;
    if (!cpu_seed_lookup_selected(params)) return false;
    params.vulkan_enabled = true;
    params.vulkan_seed_hits_enabled = true;
    return !cpu_seed_lookup_selected(params);
}());

/// Fail-closed proof policy for one completed pair. The minimap2 reference is
/// intentionally outside Bioseq's proof ABI and therefore must emit no Bioseq
/// algorithm proof. Every Bioseq row is validated against the selectors that
/// fixed its template leaf. The strict XXL gate additionally requires positive
/// non-empty vector work; ordinary benchmark rows still reject missing,
/// malformed, forbidden, or unexpected evidence without requiring work from an
/// empty input. Routes that did not select an algorithm must not emit its proof.
[[nodiscard]] bool require_selected_route_execution(
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
        return true;
    }

    bool all_dimensions_proven = true;

    const auto& scalar_proof = results.scalar_pipeline_execution_proof;
    if (scalar_proof.saturated) {
        throw std::runtime_error(fmt::format(
            "Bioseq row '{}' emitted saturated scalar execution proof",
            algo));
    }

    const bool scalar_sketch_selected =
        dispatch.params.minimizer_algorithm
            == MinimizerAlgorithmType::SCALAR;
    if (scalar_sketch_selected) {
        if (!results.selected_scalar_sketch_execution_accepted(
                /*require_target_sketch=*/true,
                /*require_nonempty_work=*/strict_xxl_gate)) {
            throw std::runtime_error(fmt::format(
                "selected scalar sketch route '{}' lacks exact positive "
                "query/target proof or emitted contradictory sketch evidence",
                algo));
        }
    } else if (!scalar_minimizer_dimensions_absent(scalar_proof)) {
        throw std::runtime_error(fmt::format(
            "non-scalar sketch route '{}' emitted scalar minimizer proof",
            algo));
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
        if (strict_xxl_gate
            && (!exact_resident_dual_vector_stream_accepted(
                    results.exact_simd_query_execution_proof)
                || !exact_fast_execution_accepted(
                    results.exact_simd_target_execution_proof))) {
            const auto& query = results.exact_simd_query_execution_proof;
            const auto& target = results.exact_simd_target_execution_proof;
            throw std::runtime_error(fmt::format(
                "strict XXL ExactSimd route did not execute resident dual-"
                "vector_stream for the query and an approved fast target "
                "kernel (resident dual-vector_stream or B8/u32 continuation): "
                "query(resident_calls={},resident_single={},"
                "resident_dual={},resident_work={},resident_bp={},"
                "generic_calls={},generic_blocks={},generic_lanes={},"
                "b8_calls={},b8_blocks={},b8_lanes={},packed22_groups={},"
                "packed22_lanes={},vector_blocks={},vector_lanes={}); "
                "target(resident_calls={},resident_single={},resident_dual={},"
                "resident_work={},resident_bp={},generic_calls={},"
                "generic_blocks={},generic_lanes={},"
                "b8_calls={},b8_blocks={},b8_lanes={},packed22_groups={},"
                "packed22_lanes={},vector_blocks={},vector_lanes={})",
                query.resident_vector_stream_kernel_calls,
                query.resident_vector_stream_single_calls,
                query.resident_vector_stream_dual_calls,
                query.resident_vector_stream_nonempty_work_calls,
                query.resident_vector_stream_input_bp,
                query.generic_u64_kernel_calls,
                query.generic_u64_candidate_blocks,
                query.generic_u64_lane_visits,
                query.k15_w10_b8_u32_hash_kernel_calls,
                query.k15_w10_b8_u32_hash_candidate_blocks,
                query.k15_w10_b8_u32_hash_lane_visits,
                query.k15_w10_b8_packed22_groups,
                query.k15_w10_b8_packed22_lane_visits,
                query.vector_batches,
                query.vector_lanes_visited,
                target.resident_vector_stream_kernel_calls,
                target.resident_vector_stream_single_calls,
                target.resident_vector_stream_dual_calls,
                target.resident_vector_stream_nonempty_work_calls,
                target.resident_vector_stream_input_bp,
                target.generic_u64_kernel_calls,
                target.generic_u64_candidate_blocks,
                target.generic_u64_lane_visits,
                target.k15_w10_b8_u32_hash_kernel_calls,
                target.k15_w10_b8_u32_hash_candidate_blocks,
                target.k15_w10_b8_u32_hash_lane_visits,
                target.k15_w10_b8_packed22_groups,
                target.k15_w10_b8_packed22_lane_visits,
                target.vector_batches,
                target.vector_lanes_visited));
        }
    } else if (results.exact_simd_query_execution_proof_present
               || results.exact_simd_target_execution_proof_present) {
        throw std::runtime_error(
            "non-ExactSimd sketch route emitted ExactSimd proof");
    }

    const bool paper_nthash_sketch_selected =
        dispatch.params.minimizer_algorithm
            == MinimizerAlgorithmType::PAPER_NTHASH_SIMD;
    if (paper_nthash_sketch_selected) {
        if (!results.paper_nthash_query_execution_accepted(
                /*require_nonempty_vector_work=*/strict_xxl_gate)
            || !results.paper_nthash_target_execution_accepted(
                /*require_nonempty_vector_work=*/strict_xxl_gate)) {
            throw std::runtime_error(
                "selected PaperNtHashSimd route lacks accepted query/target proof");
        }
        if (strict_xxl_gate
            && results.paper_nthash_target_execution_proof.segments_combined < 2) {
            throw std::runtime_error(
                "strict XXL PaperNtHashSimd route did not execute a segmented target sketch");
        }
    } else if (results.paper_nthash_query_execution_proof_present
               || results.paper_nthash_target_execution_proof_present) {
        throw std::runtime_error(
            "non-PaperNtHashSimd route emitted Paper ntHash proof");
    }

    if (dispatch.params.minimizer_algorithm
            == MinimizerAlgorithmType::SIMD_MINIMAP2_SLOW) {
        all_dimensions_proven = false;
        static std::atomic_bool logged_unproved_slow_simd{false};
        if (!logged_unproved_slow_simd.exchange(
                true, std::memory_order_relaxed)) {
            bioseq_log<WRN>(
                "[bench_v10] route '{}' executed SIMD_MINIMAP2_SLOW, which "
                "has no operation-originated sketch proof ABI; timing and "
                "correctness remain available but "
                "selected_route_execution_proof_accepted is false",
                algo);
        }
    }

    const bool simd_chaining_selected =
        dispatch.params.chaining_algorithm == ChainingAlgorithmType::SIMD_MM2;
    if (simd_chaining_selected) {
        if (!results.simd_chaining_execution_accepted(
                dispatch.params.simd_chaining_policy,
                /*require_nonempty_vector_work=*/strict_xxl_gate)) {
            throw std::runtime_error(fmt::format(
                "selected SIMD chaining policy '{}' lacks accepted, mutually "
                "exclusive score/backtrack proof",
                bioseq::chaining::simd_chaining_policy_name(
                    dispatch.params.simd_chaining_policy)));
        }
    } else if (results.simd_chaining_execution_proof_present) {
        throw std::runtime_error(
            "non-SIMD chaining route emitted SIMD chaining proof");
    }

    if (dispatch.params.chaining_algorithm == ChainingAlgorithmType::RMQ) {
        all_dimensions_proven = false;
        static std::atomic_bool logged_unproved_rmq{false};
        if (!logged_unproved_rmq.exchange(
                true, std::memory_order_relaxed)) {
            bioseq_log<WRN>(
                "[bench_v10] route '{}' executed RMQ chaining, which has no "
                "operation-originated chaining proof ABI; timing and "
                "correctness remain available but "
                "selected_route_execution_proof_accepted is false",
                algo);
        }
    }

    const bool cpu_seed_selected =
        cpu_seed_lookup_selected(dispatch.params);
    if (cpu_seed_selected) {
        if (!results.scalar_pipeline_execution_proof_present
            || !scalar_seed_dimension_accepted(
                scalar_proof, strict_xxl_gate)) {
            throw std::runtime_error(fmt::format(
                "selected CPU seed route '{}' lacks positive scalar "
                "seed-lookup operation proof",
                algo));
        }
    } else if (!scalar_seed_dimension_absent(scalar_proof)) {
        throw std::runtime_error(fmt::format(
            "selected Vulkan seed route '{}' emitted scalar CPU seed-lookup proof",
            algo));
    }

    const bool scalar_chaining_selected =
        dispatch.params.chaining_algorithm
            == ChainingAlgorithmType::SCALAR_MM2;
    if (scalar_chaining_selected) {
        if (!results.scalar_pipeline_execution_proof_present
            || !scalar_chain_dimension_accepted(
                scalar_proof, strict_xxl_gate)) {
            throw std::runtime_error(fmt::format(
                "selected scalar chaining route '{}' lacks paired "
                "score/backtrack proof",
                algo));
        }
    } else if (!scalar_chain_dimension_absent(scalar_proof)) {
        throw std::runtime_error(fmt::format(
            "non-scalar chaining route '{}' emitted scalar chaining proof",
            algo));
    }

    const bool ksw2_selected =
        dispatch.params.base_alignment_algorithm
            == BaseAlignmentAlgorithmType::KSW2;
    if (ksw2_selected) {
        if (!results.scalar_pipeline_execution_proof_present
            || !scalar_ksw2_dimension_accepted(
                scalar_proof, strict_xxl_gate)) {
            throw std::runtime_error(fmt::format(
                "selected KSW2 route '{}' lacks positive KSW2 call/input proof",
                algo));
        }
    } else if (!scalar_ksw2_dimension_absent(scalar_proof)) {
        throw std::runtime_error(fmt::format(
            "non-KSW2 route '{}' emitted KSW2 execution proof",
            algo));
    }

    const bool ksw2_batched_selected =
        dispatch.params.base_alignment_algorithm
            == BaseAlignmentAlgorithmType::KSW2_BATCHED;
    if (ksw2_batched_selected) {
        if (!results.ksw2_batched_execution_accepted(
                /*require_nonempty_work=*/strict_xxl_gate)) {
            const auto& proof = results.ksw2_batched_execution_proof;
            throw std::runtime_error(fmt::format(
                "selected KSW2_BATCHED route '{}' lacks operation proof: "
                "present={} invocations={} regions={} fills={} calls={} "
                "short_dispatches={} short_lanes={} long_calls={} "
                "input_area={} saturated={}",
                algo,
                results.ksw2_batched_execution_proof_present,
                proof.align_region_invocations,
                proof.regions_seen,
                proof.fills_collected,
                proof.nonempty_fill_calls,
                proof.short_bucket_dispatches,
                proof.short_bucket_lanes,
                proof.long_fill_calls,
                proof.input_cell_area,
                proof.saturated));
        }
    } else if (results.ksw2_batched_execution_proof_present
               || results.ksw2_batched_execution_proof
                    != Ksw2BatchedExecutionProof{}) {
        throw std::runtime_error(fmt::format(
            "non-KSW2_BATCHED route '{}' emitted batched execution proof",
            algo));
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

    const bool any_scalar_dimension_selected = scalar_sketch_selected
        || cpu_seed_selected || scalar_chaining_selected || ksw2_selected;
    if (!any_scalar_dimension_selected
        && (results.scalar_pipeline_execution_proof_present
            || scalar_proof != ScalarPipelineExecutionProof{})) {
        throw std::runtime_error(fmt::format(
            "route '{}' selected no scalar biological stage but emitted "
            "scalar pipeline proof",
            algo));
    }

    if (dispatch.explicit_vulkan) {
        bool expect_query_sketch = false;
        bool expect_target_sketch = false;
        bool expect_target_index_build = false;
        bool expect_source_direct_target_index = false;
        bool expect_seed_lookup = false;
        bool expect_chain_score = true;
        if (algo == "bioseq_ksw2_vulkan_tiled_dp_two_stack_nthash") {
            // CPU ExactSimd sketch/index/seed, Vulkan chain score only.
        } else if (algo
                   == "bioseq_block_vulkan_dp_vulkan_minimap_int") {
            expect_query_sketch = true;
            expect_target_sketch = true;
            expect_source_direct_target_index = true;
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
        const auto& partial_vulkan_proof =
            results.partial_vulkan_execution_proof;
        const bool exact_stage_mask_accepted =
            expect_source_direct_target_index
            ? partial_vulkan_proof.exact_source_direct_stage_mask_accepted(
                expect_query_sketch,
                expect_target_sketch,
                expect_seed_lookup,
                expect_chain_score)
            : partial_vulkan_proof.exact_stage_mask_accepted(
                expect_query_sketch,
                expect_target_sketch,
                expect_target_index_build,
                expect_seed_lookup,
                expect_chain_score);
        if (strict_xxl_gate && !exact_stage_mask_accepted) {
            const auto& proof = results.partial_vulkan_execution_proof;
            throw std::runtime_error(fmt::format(
                "strict XXL Vulkan row '{}' lacks exact positive dispatch proof: "
                "query_sketch={} target_sketch={} target_index_build={} "
                "source_direct_target_index={} seed_lookup={} chain_score={} "
                "saturated={}",
                algo,
                proof.query_sketch_dispatches,
                proof.target_sketch_dispatches,
                proof.target_index_build_dispatches,
                expect_source_direct_target_index,
                proof.seed_lookup_dispatches,
                proof.chain_score_dispatches,
                proof.saturated));
        }
        if (strict_xxl_gate
            && algo == "bioseq_block_vulkan_dp_vulkan_minimap_int") {
            const auto& proof = results.partial_vulkan_execution_proof;
            const bool materialization_proof_accepted =
                source_direct_gpu_index_materialization_proof_accepted(proof);
            if (!materialization_proof_accepted) {
                throw std::runtime_error(fmt::format(
                    "strict XXL direct-resident Vulkan row lacks exact "
                    "source-direct GPU-index proof: operations={} empty={} "
                    "planning_receipts={} terminal_receipts={} "
                    "terminal_successes={} zero_query_bypasses={} "
                    "direct_materializations={} publications={} "
                    "source_releases={} source_bytes={} checksum_matches={} "
                    "source_traversals={} mapped_traversals={} reflected={} "
                    "retained={} retained_bytes={} cache_preinsertions={} "
                    "expected_consumers={} completed_consumers={} "
                    "required_lookups={} required_hits={} required_misses={} "
                    "fallback_attempts={} prefix_passes={} "
                    "prefix_lookups={} prefix_comparisons={} slab_requested={} "
                    "slab_initial_capacity={} slab_actual={} slab_retained={} "
                    "slab_releases={} "
                    "roundtrips={} zero_anchor_bypasses={} fences={} "
                    "cache_cleanup={} resident_cleanup_bytes={} "
                    "pipeline_completions={} N={} D={} actual_bytes={} "
                    "seed_lookup={} seed_sorted={} host_sort_fallbacks={} "
                    "post_seed_cpu_releases={} post_seed_cpu_bytes={} "
                    "saturated={}",
                    proof.gpu_index_source_direct_operations,
                    proof.gpu_index_source_direct_empty_bypasses,
                    proof.gpu_index_source_direct_planning_receipts,
                    proof.gpu_index_source_direct_terminal_receipts,
                    proof.gpu_index_source_direct_terminal_successes,
                    proof.gpu_index_source_direct_zero_query_bypasses,
                    proof.gpu_index_source_direct_materializations,
                    proof.gpu_index_source_direct_resident_publications,
                    proof.gpu_index_source_direct_source_releases,
                    proof.gpu_index_source_direct_source_released_bytes,
                    proof.gpu_index_source_direct_checksum_matches,
                    proof.gpu_index_source_direct_source_traversals,
                    proof.gpu_index_source_direct_mapped_traversals,
                    proof.gpu_index_source_direct_reflected_authorities,
                    proof.gpu_index_source_direct_retained_authorities,
                    proof.gpu_index_source_direct_retained_resident_permit_bytes,
                    proof.gpu_index_source_direct_cache_preinsertions,
                    proof.gpu_index_source_direct_expected_seed_consumers,
                    proof.gpu_index_source_direct_completed_seed_consumers,
                    proof.gpu_index_source_direct_required_lookups,
                    proof.gpu_index_source_direct_required_hits,
                    proof.gpu_index_source_direct_required_misses,
                    proof.gpu_index_source_direct_fallback_attempts,
                    proof.gpu_index_source_direct_mapped_prefix_passes,
                    proof.gpu_index_source_direct_mapped_prefix_lookups,
                    proof.gpu_index_source_direct_mapped_prefix_comparisons,
                    proof.gpu_index_source_direct_seed_slab_requested_bytes,
                    proof.gpu_index_source_direct_seed_slab_initial_capacity,
                    proof.gpu_index_source_direct_seed_slab_actual_bytes,
                    proof.gpu_index_source_direct_seed_slab_retained_bytes,
                    proof.gpu_index_source_direct_seed_slab_permit_releases,
                    proof.gpu_index_source_direct_seed_roundtrips,
                    proof.gpu_index_source_direct_zero_anchor_bypasses,
                    proof.gpu_index_source_direct_seed_fence_completions,
                    proof.gpu_index_source_direct_cache_cleanup_releases,
                    proof.gpu_index_source_direct_resident_cleanup_bytes,
                    proof.gpu_index_source_direct_pipeline_completions,
                    proof.gpu_index_retained_minimizer_count,
                    proof.gpu_index_distinct_minimizer_count,
                    proof.gpu_index_actual_allocation_bytes,
                    proof.seed_lookup_dispatches,
                    proof.seed_sorted_dense_lookup_dispatches,
                    proof.seed_host_sort_fallbacks,
                    proof.post_seed_owned_cpu_index_releases,
                    proof.post_seed_owned_cpu_position_bytes_released,
                    proof.saturated));
            }
        }
        if (strict_xxl_gate
            && algo == "bioseq_block_vulkan_phase_c_frontend") {
            const auto& proof = results.partial_vulkan_execution_proof;
            if (!phase_c_gpu_index_ownership_proof_accepted(proof)) {
                throw std::runtime_error(fmt::format(
                    "strict XXL Phase-C row lacks exact GPU target-index "
                    "ownership/gated-build proof: target_sketches={} target_builds={} "
                    "seed_lookup={} seed_open={} seed_sorted={} "
                    "count_admissions={} main_admissions={} transfers={} "
                    "rereserve={} adoptions={} resident_bytes={} "
                    "gate_enabled={} gate_words={} gate_mask={} gate_set_bits={} "
                    "gate_count={} gated_emit={} ungated_emit={} "
                    "exact_count={} prefix_scan={} count_readback={} "
                    "deterministic_emit={} append_atomics={} emit_readback={} "
                    "counted={} verified={} parity={} estimate={} "
                    "estimate_low={} arena_trim={} T={} D={} "
                    "post_seed_gpu_evictions={} post_seed_gpu_bytes={} "
                    "post_seed_cpu_releases={} post_seed_cpu_position_bytes={} "
                    "saturated={}",
                    proof.target_sketch_dispatches,
                    proof.target_index_build_dispatches,
                    proof.seed_lookup_dispatches,
                    proof.seed_open_addressed_lookup_dispatches,
                    proof.seed_sorted_dense_lookup_dispatches,
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
                    proof.gpu_index_exact_count_dispatches,
                    proof.gpu_index_exact_count_prefix_scan_dispatches,
                    proof.gpu_index_exact_count_control_readbacks,
                    proof.gpu_index_deterministic_tile_emit_dispatches,
                    proof.gpu_index_atomic_append_dispatches,
                    proof.gpu_index_emit_validation_control_readbacks,
                    proof.gpu_index_counted_minimizer_count,
                    proof.gpu_index_verified_emit_count,
                    proof.gpu_index_count_emit_parity_verified,
                    proof.gpu_index_target_minimizer_capacity_estimate,
                    proof.gpu_index_capacity_estimate_underestimated,
                    proof.gpu_index_arena_trimmed_before_permit_shrink,
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
    if (strict_xxl_gate && !all_dimensions_proven) {
        throw std::runtime_error(fmt::format(
            "strict XXL route '{}' selected an algorithm dimension without "
            "operation-originated execution proof",
            algo));
    }
    return all_dimensions_proven;
}

/// Hand the (query, target) pair off to either the C reference or the
/// bioseq port, then aggregate the per-pair primary-correctness flags
/// into the AlignerOutput.
[[nodiscard]] AlignerOutput run_aligner_impl(
                                          std::string_view algo,
                                          const ParamMap& params,
                                          const AlignerInput& input,
                                          bool strict_xxl_gate,
                                          bool capture_full_route_proof) {
    const std::uint32_t edit_threshold = read_max_edits_param(params);
    const AlgorithmDispatch dispatch = dispatch_algorithm(
        algo, input.kmer_size_override, edit_threshold);

    AlignerOutput output;
    output.total_pairs = input.pairs.size();
    output.correct_pair_flags.assign(input.pairs.size(), 0u);
    output.historical_partial_gpu_probe =
        dispatch.historical_partial_gpu_probe;
    output.all_gpu_acceptance_eligible = false;
    output.edit_threshold_enforced = dispatch.enforce_edit_threshold;
    output.edit_threshold = dispatch.params.prefilter_max_edits;
    output.prefilter_enabled = dispatch.params.prefilter_enabled;
    output.prefilter_backend = dispatch.params.prefilter_backend;
    output.simd_chaining_policy_selected =
        dispatch.params.chaining_algorithm == ChainingAlgorithmType::SIMD_MM2;
    output.selected_simd_chaining_policy =
        dispatch.params.simd_chaining_policy;
    if (dispatch.explicit_vulkan != dispatch.historical_partial_gpu_probe) {
        throw std::logic_error(fmt::format(
            "Vulkan row '{}' lacks explicit historical partial-GPU classification",
            algo));
    }
    if ((dispatch.explicit_vulkan || dispatch.explicit_prefilter_vulkan)
        && !is_explicit_vulkan_hardware_available(algo)) {
        if (strict_xxl_gate) {
            throw std::runtime_error(
                "strict XXL gate requires every selected Vulkan row to execute");
        }
        output.vulkan_hardware_unavailable = true;
        return output;
    }
    bool every_pair_route_proven = !input.pairs.empty();
    const bool source_direct_selected =
        algo == "bioseq_block_vulkan_dp_vulkan_minimap_int";
    bool every_source_direct_pair_proven =
        source_direct_selected && !input.pairs.empty();
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
                if (dispatch.params.minimizer_algorithm
                    == MinimizerAlgorithmType::PAPER_NTHASH_SIMD) {
                    // Paper ntHash uses a 32-bit hash plus segment-local position
                    // representation.  XXL targets must therefore traverse the
                    // production segmented entry point; the unsegmented port can
                    // select a query-sized position layout and correctly rejects
                    // the 500-Mbp target rather than truncating coordinates.
                    results = bioseq::simd::HWY_NAMESPACE::minimap_port_segmented(
                        bench_lazy_query, bench_lazy_target, dispatch.params);
                } else {
                    results = bioseq::simd::HWY_NAMESPACE::minimap_port(
                        bench_lazy_query, bench_lazy_target, dispatch.params);
                }
            }
        } catch (const bioseq::gpu::vulkan::VulkanRuntimeError& exception) {
            bioseq_log<ERR>("[bench_v10] selected Vulkan aligner failed (algo='{}', pair_index={}, "
                 "query_len={}, target_len={}, k={}): {}",
                 algo, pair_index,
                 input.queries_acgt[pair_index].size(),
                 input.targets_acgt[pair_index].size(),
                 dispatch.params.kmer_size,
                 exception.what());
            if (dispatch.enforce_edit_threshold
                || dispatch.explicit_vulkan
                || dispatch.explicit_prefilter_vulkan
                || strict_xxl_gate) {
                throw;
            }
            every_pair_route_proven = false;
            continue;
        } catch (const std::exception& exception) {
            bioseq_log<ERR>("[bench_v10] aligner threw exception (algo='{}', pair_index={}, "
                 "query_len={}, target_len={}, k={}): {}",
                 algo, pair_index,
                 input.queries_acgt[pair_index].size(),
                 input.targets_acgt[pair_index].size(),
                 dispatch.params.kmer_size,
                 exception.what());
            if (dispatch.enforce_edit_threshold
                || dispatch.explicit_prefilter_vulkan
                || strict_xxl_gate) throw;
            every_pair_route_proven = false;
            continue;
        } catch (...) {
            bioseq_log<ERR>("[bench_v10] aligner threw non-std exception (algo='{}', pair_index={})",
                 algo, pair_index);
            if (dispatch.enforce_edit_threshold
                || dispatch.explicit_prefilter_vulkan
                || strict_xxl_gate) throw;
            every_pair_route_proven = false;
            continue;
        }
        const bool pair_route_proven = require_selected_route_execution(
            algo, dispatch, results, strict_xxl_gate);
        if (!pair_route_proven) {
            every_pair_route_proven = false;
        }
        if (source_direct_selected
            && !source_direct_gpu_index_materialization_proof_accepted(
                results.partial_vulkan_execution_proof)) {
            every_source_direct_pair_proven = false;
        }
        if (results.paper_nthash_query_execution_proof_present) {
            output.paper_nthash_query_execution_proof.combine(
                results.paper_nthash_query_execution_proof);
            output.paper_nthash_query_execution_proof_present = true;
        }
        if (results.paper_nthash_target_execution_proof_present) {
            output.paper_nthash_target_execution_proof.combine(
                results.paper_nthash_target_execution_proof);
            output.paper_nthash_target_execution_proof_present = true;
        }
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
        if (results.scalar_pipeline_execution_proof_present) {
            output.scalar_pipeline_execution_proof.combine(
                results.scalar_pipeline_execution_proof);
            output.scalar_pipeline_execution_proof_present = true;
        }
        if (results.ksw2_batched_execution_proof_present) {
            output.ksw2_batched_execution_proof.combine(
                results.ksw2_batched_execution_proof);
            output.ksw2_batched_execution_proof_present = true;
        }
        if (results.prefilter_execution_proof_present) {
            output.prefilter_execution_proof.combine(
                results.prefilter_execution_proof);
            output.prefilter_execution_proof_present = true;
        }
        if (capture_full_route_proof) {
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
        output.biological_fingerprint.combine(
            results.biological_fingerprint());
        bool found_primary = false;
        for (const auto& match : results.view()) {
            if (dispatch.enforce_edit_threshold
                && match.edit_distance > dispatch.params.prefilter_max_edits) {
                continue;
            }
            output.total_results += 1;
            if (!match.is_primary || found_primary) continue;
            found_primary = true;
            const std::int32_t offset =
                match.target_start - static_cast<std::int32_t>(pair.truth_target_start);
            const std::int32_t abs_offset = std::abs(offset);
            output.sum_abs_position_offset += static_cast<double>(abs_offset);
            if (abs_offset < kPositionTolerance &&
                match.is_reverse == pair.is_reverse) {
                output.correct_pair_flags[pair_index] = 1u;
                output.correct_pairs += 1;
            }
        }
    }
    if (strict_xxl_gate
        && (!output.biological_fingerprint.computed
            || output.biological_fingerprint.saturated
            || output.biological_fingerprint.collection_count
                != output.total_pairs)) {
        throw std::runtime_error(fmt::format(
            "strict XXL biological fingerprint rejected algo='{}': "
            "computed={} saturated={} collections={} pairs={}",
            algo,
            output.biological_fingerprint.computed,
            output.biological_fingerprint.saturated,
            output.biological_fingerprint.collection_count,
            output.total_pairs));
    }
    output.phase_c_gpu_index_ownership_proof_accepted =
        algo == "bioseq_block_vulkan_phase_c_frontend"
        && phase_c_gpu_index_ownership_proof_accepted(
            output.partial_vulkan_execution_proof);
    output.source_direct_gpu_index_materialization_proof_accepted =
        source_direct_selected
        && every_pair_route_proven
        && every_source_direct_pair_proven;
    output.selected_route_execution_proof_accepted =
        every_pair_route_proven;
    if (dispatch.enforce_edit_threshold && !every_pair_route_proven) {
        throw std::runtime_error(fmt::format(
            "exact-threshold benchmark row '{}' did not complete and prove "
            "every input pair",
            algo));
    }
    const bool prefilter_proof_is_zero =
        output.prefilter_execution_proof
            == PrefilterExecutionProof{};
    if (!dispatch.params.prefilter_enabled) {
        if (output.prefilter_execution_proof_present
            || !prefilter_proof_is_zero) {
            throw std::runtime_error(fmt::format(
                "prefilter-off row '{}' emitted SneakySnake execution proof",
                algo));
        }
    } else {
        const bool require_evaluated_vulkan_jobs =
            dispatch.params.prefilter_backend == PrefilterBackendType::VULKAN
            && dispatch.params.prefilter_max_edits <= 31u;
        output.prefilter_execution_proof_accepted =
            output.prefilter_execution_proof_present
            && output.prefilter_execution_proof.accepted(
                dispatch.params.prefilter_backend,
                require_evaluated_vulkan_jobs,
                require_evaluated_vulkan_jobs);
        if (!output.prefilter_execution_proof_accepted) {
            const auto& proof =
                output.prefilter_execution_proof;
            throw std::runtime_error(fmt::format(
                "prefilter row '{}' lacks accepted operation proof: "
                "present={} backend={} dispatches={} scalar={} simd={} "
                "vulkan={} jobs={} evaluated={} length_only={} "
                "unsupported={} rejected_jobs={} rejected_regions={} "
                "survivors={} saturated={}",
                algo,
                output.prefilter_execution_proof_present,
                static_cast<unsigned>(dispatch.params.prefilter_backend),
                proof.dispatches,
                proof.scalar_dispatches,
                proof.simd_dispatches,
                proof.vulkan_dispatches,
                proof.jobs,
                proof.evaluated_jobs,
                proof.length_difference_jobs,
                proof.unsupported_jobs,
                proof.rejected_jobs,
                proof.rejected_regions,
                proof.surviving_regions,
                proof.saturated));
        }
    }
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
    // The strict relative gate reports operation-originated route evidence,
    // not only the high-level dispatch decision.  Keep the full proof so its
    // evaluator can fail closed on scalar chaining/backtracking fallbacks.
    return run_aligner_impl(algo, params, input, true, true);
}

[[nodiscard]] AlignerOutput run_aligner_strict_xxl_paired(
        std::string_view algo,
        const ParamMap& params,
        const AlignerInput& input) {
    return run_aligner_impl(algo, params, input, true, true);
}

[[nodiscard]] constexpr std::uint64_t unsigned_abs_diff(
        std::uint64_t lhs, std::uint64_t rhs) noexcept {
    return lhs >= rhs ? lhs - rhs : rhs - lhs;
}

[[nodiscard]] constexpr std::uint64_t endpoint_delta_bp(
        const MatchResult& lhs, const MatchResult& rhs) noexcept {
    return std::max({
        unsigned_abs_diff(lhs.query_start, rhs.query_start),
        unsigned_abs_diff(lhs.query_end, rhs.query_end),
        unsigned_abs_diff(lhs.target_start, rhs.target_start),
        unsigned_abs_diff(lhs.target_end, rhs.target_end),
    });
}

[[nodiscard]] constexpr bool normalized_endpoint_coordinates(
        const MatchResult& match,
        std::uint64_t query_length,
        std::uint64_t target_length) noexcept {
    return match.query_len == query_length
        && match.target_len == target_length
        && match.query_start <= match.query_end
        && match.query_end <= query_length
        && match.target_start <= match.target_end
        && match.target_end <= target_length;
}

struct UniquePrimaryView {
    const MatchResult* primary = nullptr;
    std::uint64_t count = 0;
};

[[nodiscard]] UniquePrimaryView unique_primary(
        const MatchResultCollection& results) noexcept {
    UniquePrimaryView selected;
    for (const auto& match : results.view()) {
        if (!match.is_primary) continue;
        ++selected.count;
        if (selected.count == 1) selected.primary = &match;
    }
    return selected;
}

[[nodiscard]] EndpointCoordinatesEvidence endpoint_evidence(
        const UniquePrimaryView& primary) noexcept {
    if (primary.count != 1 || primary.primary == nullptr) return {};
    return {
        .present = true,
        .query_len = primary.primary->query_len,
        .query_start = primary.primary->query_start,
        .query_end = primary.primary->query_end,
        .is_reverse = primary.primary->is_reverse,
        .target_len = primary.primary->target_len,
        .target_start = primary.primary->target_start,
        .target_end = primary.primary->target_end,
    };
}

[[nodiscard]] EndpointBackendOutcome run_endpoint_bioseq_policy(
        EndpointQualificationFixture& fixture,
        SimdChainingPolicy policy,
        bool& route_proof_accepted) {
    const std::string_view algorithm =
        policy == SimdChainingPolicy::exact_visited
            ? "bioseq_ksw2_simd_dp_two_stack_nthash_exact_visited"
            : "bioseq_ksw2_simd_dp_two_stack_nthash_bounded_batch_approximate";
    AlgorithmDispatch dispatch = dispatch_algorithm(algorithm, 0);
    dispatch.params.chain_skip_scale = fixture.chain_skip_scale;
    dispatch.params.max_chain_skip = fixture.max_chain_skip;
    auto lazy_query = bioseq::storage::make_lazy_in_memory(fixture.pair.query);
    auto lazy_target = bioseq::storage::make_lazy_in_memory(*fixture.pair.target);
    MatchResultCollection results =
        bioseq::simd::HWY_NAMESPACE::minimap_port(
            lazy_query, lazy_target, dispatch.params);
    try {
        route_proof_accepted = require_selected_route_execution(
            algorithm, dispatch, results, /*strict_xxl_gate=*/false);
    } catch (const std::exception&) {
        // Preserve the completed operation and its proof in endpoint raw
        // evidence. Ordinary qualification rules reject this false value.
        route_proof_accepted = false;
    }
    return {
        .matches = std::move(results),
        .target_ordinal = fixture.target_ordinal,
    };
}

/// One callback deliberately owns all three executions for every fixture.
/// Independent matrix rows cannot safely exchange four endpoints, and globals
/// would make pairing depend on scheduler order.
[[nodiscard]] SimdChainingEndpointQualification
run_simd_chaining_endpoint_qualification(
    std::string_view algorithm,
    const ParamMap& /*params*/,
    const EndpointQualificationInput& input)
{
    if (algorithm == "simd_chaining_endpoint_zero_reference") {
        // Framework-literal substitute used only as the zero side of blocking
        // comparison rules.  It intentionally performs no biological work.
        return {};
    }
    if (algorithm != "simd_chaining_endpoint_qualification") {
        throw std::invalid_argument(fmt::format(
            "unknown endpoint qualification row '{}'", algorithm));
    }

    SimdChainingEndpointQualification qualification;
    qualification.comparisons.reserve(input.fixtures.size());
    qualification.exact_route_proof_accepted = !input.fixtures.empty();
    qualification.bounded_route_proof_accepted = !input.fixtures.empty();
    qualification.minimap2_route_proof_accepted = !input.fixtures.empty();

    for (auto& fixture : input.fixtures) {
        // Fixed order is part of the qualification contract.
        bool bounded_fixture_route_accepted = false;
        EndpointBackendOutcome bounded = run_endpoint_bioseq_policy(
            fixture, SimdChainingPolicy::bounded_batch_approximate,
            bounded_fixture_route_accepted);
        bool exact_fixture_route_accepted = false;
        EndpointBackendOutcome exact = run_endpoint_bioseq_policy(
            fixture, SimdChainingPolicy::exact_visited,
            exact_fixture_route_accepted);

        qualification.bounded_route_proof_accepted =
            qualification.bounded_route_proof_accepted
            && bounded_fixture_route_accepted
            && bounded.matches.exact_simd_query_execution_accepted(true)
            && bounded.matches.exact_simd_target_execution_accepted(true)
            && bounded.matches.simd_chaining_execution_accepted(
                SimdChainingPolicy::bounded_batch_approximate, true);
        qualification.exact_route_proof_accepted =
            qualification.exact_route_proof_accepted
            && exact_fixture_route_accepted
            && exact.matches.exact_simd_query_execution_accepted(true)
            && exact.matches.exact_simd_target_execution_accepted(true)
            && exact.matches.simd_chaining_execution_accepted(
                SimdChainingPolicy::exact_visited, true);

        AlgorithmDispatch minimap_dispatch = dispatch_algorithm("minimap2_dp", 0);
        minimap_dispatch.params.chain_skip_scale = fixture.chain_skip_scale;
        minimap_dispatch.params.max_chain_skip = fixture.max_chain_skip;
        EndpointBackendOutcome minimap{
            .matches = bioseq::bench::HWY_NAMESPACE::tool_minimap2(
                fixture.query_acgt,
                fixture.target_acgt,
                minimap_dispatch.params),
            .target_ordinal = fixture.target_ordinal,
        };
        bool minimap_fixture_route_accepted = false;
        try {
            minimap_fixture_route_accepted = require_selected_route_execution(
                "minimap2_dp", minimap_dispatch, minimap.matches,
                /*strict_xxl_gate=*/false);
        } catch (const std::exception&) {
            minimap_fixture_route_accepted = false;
        }
        qualification.minimap2_route_proof_accepted =
            qualification.minimap2_route_proof_accepted
            && minimap_fixture_route_accepted;

        qualification.exact_policy_proof.combine(
            exact.matches.simd_chaining_execution_proof);
        qualification.bounded_policy_proof.combine(
            bounded.matches.simd_chaining_execution_proof);
        qualification.exact_policy_proof_present =
            qualification.exact_policy_proof_present
            || exact.matches.simd_chaining_execution_proof_present;
        qualification.bounded_policy_proof_present =
            qualification.bounded_policy_proof_present
            || bounded.matches.simd_chaining_execution_proof_present;

        EndpointFixtureComparison comparison;
        comparison.fixture_name = fixture.name;
        comparison.target_ordinal = fixture.target_ordinal;
        const UniquePrimaryView bounded_primary = unique_primary(bounded.matches);
        const UniquePrimaryView exact_primary = unique_primary(exact.matches);
        const UniquePrimaryView minimap_primary = unique_primary(minimap.matches);
        comparison.bounded_primary_count = bounded_primary.count;
        comparison.exact_primary_count = exact_primary.count;
        comparison.minimap2_primary_count = minimap_primary.count;
        comparison.bounded_target_ordinal = bounded.target_ordinal;
        comparison.exact_target_ordinal = exact.target_ordinal;
        comparison.minimap2_target_ordinal = minimap.target_ordinal;
        comparison.bounded_endpoint = endpoint_evidence(bounded_primary);
        comparison.exact_endpoint = endpoint_evidence(exact_primary);
        comparison.minimap2_endpoint = endpoint_evidence(minimap_primary);
        const auto& fixture_proof =
            bounded.matches.simd_chaining_execution_proof;
        comparison.bounded_cutoff_stops =
            fixture_proof.policy.bounded_cutoff_stops;
        comparison.bounded_candidates_skipped =
            fixture_proof.policy.bounded_candidates_skipped;
        comparison.bounded_rescue_probes =
            fixture_proof.policy.global_best_rescue_probes;
        comparison.bounded_rescue_wins =
            fixture_proof.policy.global_best_rescue_wins;
        comparison.bounded_fixture_proof_accepted =
            bounded.matches.simd_chaining_execution_proof_present
            && fixture_proof.accepted(
                SimdChainingPolicy::bounded_batch_approximate, true);
        comparison.deterministic_rescue_fixture =
            fixture.name == "max_skip_invalid_runs_and_rescue";
        comparison.bounded_route_proof_accepted =
            bounded_fixture_route_accepted;
        comparison.exact_route_proof_accepted = exact_fixture_route_accepted;
        comparison.minimap2_route_proof_accepted =
            minimap_fixture_route_accepted;

        ++qualification.fixtures;
        const bool any_missing = bounded_primary.count == 0
            || exact_primary.count == 0 || minimap_primary.count == 0;
        const bool any_ambiguous = bounded_primary.count > 1
            || exact_primary.count > 1 || minimap_primary.count > 1;
        qualification.missing_primary += any_missing ? 1u : 0u;
        qualification.ambiguous_primary += any_ambiguous ? 1u : 0u;
        if (any_missing || any_ambiguous) {
            qualification.comparisons.push_back(std::move(comparison));
            continue;
        }

        const std::uint64_t query_length = fixture.query_acgt.size();
        const std::uint64_t target_length = fixture.target_acgt.size();
        comparison.normalized_coordinates =
            normalized_endpoint_coordinates(
                *bounded_primary.primary, query_length, target_length)
            && normalized_endpoint_coordinates(
                *exact_primary.primary, query_length, target_length)
            && normalized_endpoint_coordinates(
                *minimap_primary.primary, query_length, target_length);
        if (!comparison.normalized_coordinates) {
            ++qualification.invalid_normalized_coordinates;
            qualification.comparisons.push_back(std::move(comparison));
            continue;
        }

        // All adapters carry the same source fixture ordinal because every run
        // received one target. Keep this explicit before strand/coordinate
        // comparison so a future multi-target wrapper must preserve identity.
        // MatchResult has no reference-id field.  Each adapter is invoked with
        // exactly one target, so caller-bound `target_ordinal` identifies the
        // source fixture and the observed target length is the strongest
        // backend-returned identity check available at this API boundary.
        const bool identity_matches =
            bounded.target_ordinal == fixture.target_ordinal
            && exact.target_ordinal == fixture.target_ordinal
            && minimap.target_ordinal == fixture.target_ordinal
            && bounded.target_ordinal == exact.target_ordinal
            && bounded.target_ordinal == minimap.target_ordinal
            && bounded_primary.primary->target_len == target_length
            && exact_primary.primary->target_len == target_length
            && minimap_primary.primary->target_len == target_length;
        const bool strand_matches =
            bounded_primary.primary->is_reverse
                == exact_primary.primary->is_reverse
            && bounded_primary.primary->is_reverse
                == minimap_primary.primary->is_reverse;
        comparison.identity_and_strand_match =
            identity_matches && strand_matches;
        if (!comparison.identity_and_strand_match) {
            ++qualification.reference_or_strand_mismatches;
            qualification.comparisons.push_back(std::move(comparison));
            continue;
        }

        comparison.delta_vs_exact_bp = endpoint_delta_bp(
            *bounded_primary.primary, *exact_primary.primary);
        comparison.delta_vs_minimap2_bp = endpoint_delta_bp(
            *bounded_primary.primary, *minimap_primary.primary);
        qualification.max_delta_vs_exact_bp = std::max(
            qualification.max_delta_vs_exact_bp,
            comparison.delta_vs_exact_bp);
        qualification.max_delta_vs_minimap2_bp = std::max(
            qualification.max_delta_vs_minimap2_bp,
            comparison.delta_vs_minimap2_bp);
        ++qualification.mappings_compared;
        if (comparison.deterministic_rescue_fixture) {
            ++qualification.deterministic_rescue_fixture_mappings_compared;
            if (comparison.bounded_fixture_proof_accepted
                && comparison.bounded_cutoff_stops > 0
                && comparison.bounded_candidates_skipped > 0
                && comparison.bounded_rescue_probes > 0
                && comparison.bounded_rescue_wins > 0) {
                ++qualification.deterministic_rescue_fixture_runs;
            }
        }
        qualification.comparisons.push_back(std::move(comparison));
    }

    qualification.exact_route_proof_accepted =
        qualification.exact_route_proof_accepted
        && qualification.exact_policy_proof_present
        && qualification.exact_policy_proof.accepted(
            SimdChainingPolicy::exact_visited, true);
    qualification.bounded_route_proof_accepted =
        qualification.bounded_route_proof_accepted
        && qualification.bounded_policy_proof_present
        && qualification.bounded_policy_proof.accepted(
            SimdChainingPolicy::bounded_batch_approximate, true);
    qualification.saturated = qualification.exact_policy_proof.saturated
        || qualification.bounded_policy_proof.saturated
        || qualification.exact_policy_proof.policy.saturated
        || qualification.bounded_policy_proof.policy.saturated;

    return qualification;
}

[[nodiscard]] std::map<std::string, double>
evaluate_simd_chaining_endpoint_qualification(
    std::string_view /*algorithm*/,
    const ParamMap& /*params*/,
    const EndpointQualificationInput& /*input*/,
    const SimdChainingEndpointQualification& output)
{
    // Return every metric normally.  Blocking comparison rules use the
    // no-work zero-reference row, so a rejected qualification still produces
    // a complete manifest, ordinary report metrics, and its raw fixture data.
    return {
        // Compatibility coverage for the config-global historical-regression
        // rule. This is deliberately finite and identical for both endpoint
        // rows; biological correctness is exclusively gated by endpoint_*.
        {"primary_correct_fraction", 1.0},
        {"endpoint_qualification_accepted", output.accepted() ? 1.0 : 0.0},
        {"endpoint_policy_proof_accepted", output.proof_accepted() ? 1.0 : 0.0},
        {"endpoint_rescue_evidence_accepted",
            output.rescue_evidence_accepted() ? 1.0 : 0.0},
        {"endpoint_exact_route_proof_accepted",
            output.exact_route_proof_accepted ? 1.0 : 0.0},
        {"endpoint_bounded_route_proof_accepted",
            output.bounded_route_proof_accepted ? 1.0 : 0.0},
        {"endpoint_minimap2_route_proof_accepted",
            output.minimap2_route_proof_accepted ? 1.0 : 0.0},
        {"endpoint_qualification_saturated", output.saturated ? 1.0 : 0.0},
        {"endpoint_deterministic_rescue_fixture_runs",
            static_cast<double>(output.deterministic_rescue_fixture_runs)},
        {"endpoint_deterministic_rescue_fixture_mappings_compared",
            static_cast<double>(
                output.deterministic_rescue_fixture_mappings_compared)},
        {"endpoint_fixtures", static_cast<double>(output.fixtures)},
        {"endpoint_mappings_compared",
            static_cast<double>(output.mappings_compared)},
        {"endpoint_missing_primary",
            static_cast<double>(output.missing_primary)},
        {"endpoint_ambiguous_primary",
            static_cast<double>(output.ambiguous_primary)},
        {"endpoint_reference_or_strand_mismatches",
            static_cast<double>(output.reference_or_strand_mismatches)},
        {"endpoint_invalid_normalized_coordinates",
            static_cast<double>(output.invalid_normalized_coordinates)},
        {"endpoint_max_delta_vs_exact_bp",
            static_cast<double>(output.max_delta_vs_exact_bp)},
        {"endpoint_max_delta_vs_minimap2_bp",
            static_cast<double>(output.max_delta_vs_minimap2_bp)},
        {"endpoint_exact_policy_calls", static_cast<double>(
            output.exact_policy_proof.policy.exact_visited_calls)},
        {"endpoint_bounded_policy_calls", static_cast<double>(
            output.bounded_policy_proof.policy
                .bounded_batch_approximate_calls)},
        {"endpoint_bounded_score_batches", static_cast<double>(
            output.bounded_policy_proof.policy.bounded_score_batches)},
        {"endpoint_bounded_cutoff_stops", static_cast<double>(
            output.bounded_policy_proof.policy.bounded_cutoff_stops)},
        {"endpoint_bounded_candidates_skipped", static_cast<double>(
            output.bounded_policy_proof.policy
                .bounded_candidates_skipped)},
        {"endpoint_global_best_rescue_probes", static_cast<double>(
            output.bounded_policy_proof.policy
                .global_best_rescue_probes)},
        {"endpoint_global_best_rescue_wins", static_cast<double>(
            output.bounded_policy_proof.policy.global_best_rescue_wins)},
        {"endpoint_exact_policy_rejections", static_cast<double>(
            output.exact_policy_proof.policy.policy_rejections)},
        {"endpoint_bounded_policy_rejections", static_cast<double>(
            output.bounded_policy_proof.policy.policy_rejections)},
        {"endpoint_exact_policy_evidence_saturated",
            output.exact_policy_proof.policy.saturated ? 1.0 : 0.0},
        {"endpoint_bounded_policy_evidence_saturated",
            output.bounded_policy_proof.policy.saturated ? 1.0 : 0.0},
        {"endpoint_scalar_score_branches", static_cast<double>(
            output.bounded_policy_proof.scalar_score_branches)},
        {"endpoint_scalar_backtrack_branches", static_cast<double>(
            output.bounded_policy_proof.scalar_backtrack_branches)},
        {"endpoint_shared_scalar_backtrack_calls", static_cast<double>(
            output.bounded_policy_proof.shared_scalar_backtrack_calls)},
    };
}

struct PrefilterPipelineMatchCountParityEvidence {
    bool required = false;
    bool performed = false;
    bool passed = false;
};

/// Record only results produced by the existing timed rows. No reference
/// pipeline is executed here. The seeded repetition-major schedule includes
/// both off and Vulkan in repeat zero, so the second observation compares
/// immediately; all later observations also prove per-row determinism.
[[nodiscard]] PrefilterPipelineMatchCountParityEvidence
record_prefilter_pipeline_match_count_parity(
    std::string_view algorithm,
    const ParamMap& params,
    const AlignerInput& input,
    const AlignerOutput& output)
{
    const bool is_prefilter_off = algorithm == "bioseq_prefilter_off";
    const bool is_prefilter_vulkan =
        algorithm == "bioseq_prefilter_vulkan";
    if (!is_prefilter_off && !is_prefilter_vulkan) return {};
    if (!input.prefilter_pipeline_match_count_parity) {
        throw std::logic_error(
            "prefilter pipeline parity state is unexpectedly absent");
    }

    const std::uint32_t threshold = read_max_edits_param(params);
    auto& shared = *input.prefilter_pipeline_match_count_parity;
    std::scoped_lock lock{shared.mutex};
    auto& state = shared.by_threshold[threshold];
    const auto record_stable_count =
        [threshold](std::optional<std::size_t>& destination,
                    const std::size_t observed,
                    std::string_view row_name) {
            if (destination.has_value() && *destination != observed) {
                throw std::runtime_error(fmt::format(
                    "nondeterministic {} match count for max_edits={}: "
                    "first={} repeated={}",
                    row_name, threshold, *destination, observed));
            }
            destination = observed;
        };

    if (is_prefilter_off) {
        if (!state.prefilter_off_scheduled) {
            throw std::logic_error(fmt::format(
                "observed unscheduled prefilter-off row for max_edits={}",
                threshold));
        }
        record_stable_count(
            state.prefilter_off_match_count,
            output.total_results,
            "prefilter-off");
    } else {
        if (!state.vulkan_scheduled) {
            throw std::logic_error(fmt::format(
                "observed unscheduled Vulkan prefilter row for max_edits={}",
                threshold));
        }
        const bool hardware_available =
            !output.vulkan_hardware_unavailable;
        if (state.vulkan_hardware_available.has_value()
            && *state.vulkan_hardware_available != hardware_available) {
            throw std::runtime_error(fmt::format(
                "nondeterministic Vulkan hardware availability for "
                "max_edits={}: first={} repeated={}",
                threshold,
                *state.vulkan_hardware_available,
                hardware_available));
        }
        state.vulkan_hardware_available = hardware_available;
        if (hardware_available) {
            if (!state.prefilter_off_scheduled) {
                throw std::runtime_error(fmt::format(
                    "available Vulkan prefilter row has no scheduled "
                    "same-threshold prefilter-off reference for "
                    "max_edits={}",
                    threshold));
            }
            record_stable_count(
                state.vulkan_match_count,
                output.total_results,
                "Vulkan prefilter");
        }
    }

    if (state.vulkan_hardware_available.value_or(false)
        && state.prefilter_off_match_count.has_value()
        && state.vulkan_match_count.has_value()) {
        state.comparison_performed = true;
        state.comparison_passed = *state.vulkan_match_count
            == *state.prefilter_off_match_count;
        if (!state.comparison_passed) {
            throw std::runtime_error(fmt::format(
                "available Vulkan prefilter pipeline changed exact-threshold "
                "match count: vulkan={} prefilter_off={} max_edits={}",
                *state.vulkan_match_count,
                *state.prefilter_off_match_count,
                threshold));
        }
    }

    return {
        .required = state.vulkan_hardware_available.value_or(false),
        .performed = state.comparison_performed,
        .passed = state.comparison_passed,
    };
}

/// Translate `Output` into the evaluation metrics declared in the JSON
/// config. The framework drops keys that aren't declared on the active
/// benchmark, so we always emit the full set; only the keys the running
/// benchmark consumes land in the report.
[[nodiscard]] constexpr double uint64_hi32_metric(
        std::uint64_t value) noexcept {
    return static_cast<double>(static_cast<std::uint32_t>(value >> 32u));
}

[[nodiscard]] constexpr double uint64_lo32_metric(
        std::uint64_t value) noexcept {
    return static_cast<double>(static_cast<std::uint32_t>(value));
}

[[nodiscard]] std::string uint64_hex(std::uint64_t value) {
    return fmt::format("{:016x}", value);
}

[[nodiscard]] std::map<std::string, double> evaluate_aligner(
    std::string_view algorithm,
    const ParamMap& params,
    const AlignerInput& input,
    const AlignerOutput& output)
{
    const auto prefilter_match_count_parity =
        record_prefilter_pipeline_match_count_parity(
            algorithm, params, input, output);
    std::map<std::string, double> metrics;
    metrics.emplace("pipeline_backend_available",
        output.vulkan_hardware_unavailable ? 0.0 : 1.0);
    metrics.emplace("vulkan_match_count_parity_required",
        prefilter_match_count_parity.required ? 1.0 : 0.0);
    metrics.emplace("vulkan_match_count_parity_performed",
        prefilter_match_count_parity.performed ? 1.0 : 0.0);
    metrics.emplace("vulkan_match_count_parity_passed",
        prefilter_match_count_parity.passed ? 1.0 : 0.0);
    metrics.emplace("edit_threshold_enforced",
        output.edit_threshold_enforced ? 1.0 : 0.0);
    metrics.emplace("edit_threshold",
        static_cast<double>(output.edit_threshold));
    metrics.emplace("prefilter_enabled",
        output.prefilter_enabled ? 1.0 : 0.0);
    metrics.emplace("prefilter_backend_off",
        !output.prefilter_enabled
            && !output.prefilter_execution_proof_present
            && output.prefilter_execution_proof
                == PrefilterExecutionProof{}
        ? 1.0 : 0.0);
    metrics.emplace("prefilter_backend_scalar",
        output.prefilter_execution_proof_present
            && output.prefilter_execution_proof.accepted(
                PrefilterBackendType::SCALAR)
        ? 1.0 : 0.0);
    metrics.emplace("prefilter_backend_simd",
        output.prefilter_execution_proof_present
            && output.prefilter_execution_proof.accepted(
                PrefilterBackendType::SIMD)
        ? 1.0 : 0.0);
    metrics.emplace("prefilter_backend_vulkan",
        output.prefilter_execution_proof_present
            && output.prefilter_execution_proof.accepted(
                PrefilterBackendType::VULKAN,
                /*require_evaluated_jobs=*/true,
                /*forbid_unsupported_jobs=*/true)
        ? 1.0 : 0.0);
    metrics.emplace("prefilter_execution_proof_present",
        output.prefilter_execution_proof_present ? 1.0 : 0.0);
    metrics.emplace("prefilter_execution_proof_accepted",
        output.prefilter_execution_proof_accepted ? 1.0 : 0.0);
    metrics.emplace("prefilter_dispatches", static_cast<double>(
        output.prefilter_execution_proof.dispatches));
    metrics.emplace("prefilter_sneaky_snake_dispatches", static_cast<double>(
        output.prefilter_execution_proof.sneaky_snake_dispatches));
    metrics.emplace("prefilter_gatekeeper_dispatches", static_cast<double>(
        output.prefilter_execution_proof.gatekeeper_dispatches));
    metrics.emplace("prefilter_scalar_dispatches", static_cast<double>(
        output.prefilter_execution_proof.scalar_dispatches));
    metrics.emplace("prefilter_simd_dispatches", static_cast<double>(
        output.prefilter_execution_proof.simd_dispatches));
    metrics.emplace("prefilter_vulkan_dispatches", static_cast<double>(
        output.prefilter_execution_proof.vulkan_dispatches));
    metrics.emplace("prefilter_jobs", static_cast<double>(
        output.prefilter_execution_proof.jobs));
    metrics.emplace("prefilter_evaluated_jobs", static_cast<double>(
        output.prefilter_execution_proof.evaluated_jobs));
    metrics.emplace("prefilter_length_difference_jobs", static_cast<double>(
        output.prefilter_execution_proof
            .length_difference_jobs));
    metrics.emplace("prefilter_unsupported_jobs", static_cast<double>(
        output.prefilter_execution_proof.unsupported_jobs));
    metrics.emplace("prefilter_rejected_jobs", static_cast<double>(
        output.prefilter_execution_proof.rejected_jobs));
    metrics.emplace("prefilter_rejected_regions", static_cast<double>(
        output.prefilter_execution_proof.rejected_regions));
    metrics.emplace("prefilter_surviving_regions", static_cast<double>(
        output.prefilter_execution_proof.surviving_regions));
    metrics.emplace("prefilter_mask_rejection_candidates", static_cast<double>(
        output.prefilter_execution_proof.mask_rejection_candidates));
    metrics.emplace("prefilter_exact_guard_activations", static_cast<double>(
        output.prefilter_execution_proof.exact_guard_activations));
    metrics.emplace("prefilter_guard_confirmed_rejections", static_cast<double>(
        output.prefilter_execution_proof.guard_confirmed_rejections));
    metrics.emplace("prefilter_hamming_exact_rejections", static_cast<double>(
        output.prefilter_execution_proof.hamming_exact_rejections));
    metrics.emplace("prefilter_execution_proof_saturated",
        output.prefilter_execution_proof.saturated ? 1.0 : 0.0);
    metrics.emplace("ksw2_left_calls", static_cast<double>(
        output.scalar_pipeline_execution_proof.ksw2_left_calls));
    metrics.emplace("ksw2_gap_calls", static_cast<double>(
        output.scalar_pipeline_execution_proof.ksw2_gap_calls));
    metrics.emplace("ksw2_right_calls", static_cast<double>(
        output.scalar_pipeline_execution_proof.ksw2_right_calls));
    metrics.emplace("ksw2_total_calls", static_cast<double>(
        output.scalar_pipeline_execution_proof.ksw2_left_calls
        + output.scalar_pipeline_execution_proof.ksw2_gap_calls
        + output.scalar_pipeline_execution_proof.ksw2_right_calls));
    metrics.emplace("ksw2_input_cell_area", static_cast<double>(
        output.scalar_pipeline_execution_proof.ksw2_input_cell_area));
    metrics.emplace("selected_route_execution_proof_accepted",
        output.selected_route_execution_proof_accepted ? 1.0 : 0.0);
    const auto& biological = output.biological_fingerprint;
    metrics.emplace("biological_fingerprint_computed",
        biological.computed ? 1.0 : 0.0);
    metrics.emplace("biological_fingerprint_saturated",
        biological.saturated ? 1.0 : 0.0);
    metrics.emplace("biological_collection_count",
        static_cast<double>(biological.collection_count));
    metrics.emplace("biological_match_result_count",
        static_cast<double>(biological.match_result_count));
    metrics.emplace("biological_cigar_result_count",
        static_cast<double>(biological.cigar_result_count));
    metrics.emplace("biological_cigar_operation_count",
        static_cast<double>(biological.cigar_operation_count));
    metrics.emplace("biological_match_digest_lane0_hi32",
        uint64_hi32_metric(biological.match_digest_lane0));
    metrics.emplace("biological_match_digest_lane0_lo32",
        uint64_lo32_metric(biological.match_digest_lane0));
    metrics.emplace("biological_match_digest_lane1_hi32",
        uint64_hi32_metric(biological.match_digest_lane1));
    metrics.emplace("biological_match_digest_lane1_lo32",
        uint64_lo32_metric(biological.match_digest_lane1));
    metrics.emplace("biological_cigar_digest_lane0_hi32",
        uint64_hi32_metric(biological.cigar_digest_lane0));
    metrics.emplace("biological_cigar_digest_lane0_lo32",
        uint64_lo32_metric(biological.cigar_digest_lane0));
    metrics.emplace("biological_cigar_digest_lane1_hi32",
        uint64_hi32_metric(biological.cigar_digest_lane1));
    metrics.emplace("biological_cigar_digest_lane1_lo32",
        uint64_lo32_metric(biological.cigar_digest_lane1));
    metrics.emplace("ksw2_batched_execution_proof_accepted",
        output.ksw2_batched_execution_proof_present
            && output.ksw2_batched_execution_proof.accepted(true)
            ? 1.0 : 0.0);
    metrics.emplace("paper_nthash_query_execution_proof_accepted",
        output.paper_nthash_query_execution_proof_present
            && output.paper_nthash_query_execution_proof.accepted(true)
            ? 1.0 : 0.0);
    metrics.emplace("paper_nthash_target_execution_proof_accepted",
        output.paper_nthash_target_execution_proof_present
            && output.paper_nthash_target_execution_proof.accepted(true)
            ? 1.0 : 0.0);
    metrics.emplace("paper_nthash_query_segments_combined",
        static_cast<double>(
            output.paper_nthash_query_execution_proof.segments_combined));
    metrics.emplace("paper_nthash_target_segments_combined",
        static_cast<double>(
            output.paper_nthash_target_execution_proof.segments_combined));
    metrics.emplace("paper_nthash_fallback_calls",
        static_cast<double>(
            output.paper_nthash_query_execution_proof.fallback_calls
            + output.paper_nthash_target_execution_proof.fallback_calls));
    metrics.emplace("exact_simd_query_execution_proof_accepted",
        output.exact_simd_query_execution_proof_present
            && output.exact_simd_query_execution_proof.accepted(true)
            ? 1.0 : 0.0);
    metrics.emplace("exact_simd_target_execution_proof_accepted",
        output.exact_simd_target_execution_proof_present
            && output.exact_simd_target_execution_proof.accepted(true)
            ? 1.0 : 0.0);
    metrics.emplace("exact_simd_query_b8_route_accepted",
        output.exact_simd_query_execution_proof_present
            && exact_b8_execution_accepted(
                output.exact_simd_query_execution_proof)
            ? 1.0 : 0.0);
    metrics.emplace("exact_simd_target_b8_route_accepted",
        output.exact_simd_target_execution_proof_present
            && exact_b8_execution_accepted(
                output.exact_simd_target_execution_proof)
            ? 1.0 : 0.0);
    metrics.emplace("exact_simd_query_resident_dual_route_accepted",
        output.exact_simd_query_execution_proof_present
            && exact_resident_dual_vector_stream_accepted(
                output.exact_simd_query_execution_proof)
            ? 1.0 : 0.0);
    metrics.emplace("exact_simd_target_resident_dual_route_accepted",
        output.exact_simd_target_execution_proof_present
            && exact_resident_dual_vector_stream_accepted(
                output.exact_simd_target_execution_proof)
            ? 1.0 : 0.0);
    metrics.emplace("exact_simd_query_fast_route_accepted",
        output.exact_simd_query_execution_proof_present
            && exact_fast_execution_accepted(
                output.exact_simd_query_execution_proof)
            ? 1.0 : 0.0);
    metrics.emplace("exact_simd_target_fast_route_accepted",
        output.exact_simd_target_execution_proof_present
            && exact_fast_execution_accepted(
                output.exact_simd_target_execution_proof)
            ? 1.0 : 0.0);
    metrics.emplace("exact_simd_query_resident_vector_stream_calls",
        static_cast<double>(output.exact_simd_query_execution_proof
            .resident_vector_stream_kernel_calls));
    metrics.emplace("exact_simd_target_resident_vector_stream_calls",
        static_cast<double>(output.exact_simd_target_execution_proof
            .resident_vector_stream_kernel_calls));
    metrics.emplace("exact_simd_query_resident_vector_stream_dual_calls",
        static_cast<double>(output.exact_simd_query_execution_proof
            .resident_vector_stream_dual_calls));
    metrics.emplace("exact_simd_target_resident_vector_stream_dual_calls",
        static_cast<double>(output.exact_simd_target_execution_proof
            .resident_vector_stream_dual_calls));
    metrics.emplace("exact_simd_query_generic_kernel_calls",
        static_cast<double>(output.exact_simd_query_execution_proof
            .generic_u64_kernel_calls));
    metrics.emplace("exact_simd_target_generic_kernel_calls",
        static_cast<double>(output.exact_simd_target_execution_proof
            .generic_u64_kernel_calls));
    metrics.emplace("exact_simd_query_b8_kernel_calls",
        static_cast<double>(output.exact_simd_query_execution_proof
            .k15_w10_b8_u32_hash_kernel_calls));
    metrics.emplace("exact_simd_target_b8_kernel_calls",
        static_cast<double>(output.exact_simd_target_execution_proof
            .k15_w10_b8_u32_hash_kernel_calls));
    metrics.emplace("simd_chaining_no_improvement_fast_batches",
        static_cast<double>(output.simd_chaining_execution_proof
            .no_improvement_fast_batches));
    metrics.emplace("simd_chaining_improvement_replay_batches",
        static_cast<double>(output.simd_chaining_execution_proof
            .improvement_replay_batches));
    metrics.emplace("simd_chaining_improvement_replay_lanes",
        static_cast<double>(output.simd_chaining_execution_proof
            .improvement_replay_lanes));
    metrics.emplace("simd_chaining_scalar_orchestration_steps",
        static_cast<double>(output.simd_chaining_execution_proof
            .scalar_orchestration_steps));
    metrics.emplace("simd_chaining_hybrid_evidence_coherent",
        output.simd_chaining_execution_proof_present
            && output.simd_chaining_execution_proof
                .exact_hybrid_evidence_coherent()
            ? 1.0 : 0.0);
    const bool selected_policy_evidence_coherent =
        output.simd_chaining_policy_selected
        && output.simd_chaining_execution_proof_present
        && output.simd_chaining_execution_proof.policy_evidence_coherent(
            output.selected_simd_chaining_policy,
            /*require_accelerated_work=*/
                output.selected_simd_chaining_policy
                    == SimdChainingPolicy::bounded_batch_approximate);
    metrics.emplace("simd_chaining_selected_policy_exact_visited",
        output.simd_chaining_policy_selected
            && output.selected_simd_chaining_policy
                == SimdChainingPolicy::exact_visited
            ? 1.0 : 0.0);
    metrics.emplace(
        "simd_chaining_selected_policy_bounded_batch_approximate",
        output.simd_chaining_policy_selected
            && output.selected_simd_chaining_policy
                == SimdChainingPolicy::bounded_batch_approximate
            ? 1.0 : 0.0);
    metrics.emplace("simd_chaining_policy_evidence_coherent",
        selected_policy_evidence_coherent ? 1.0 : 0.0);
    metrics.emplace("simd_chaining_selected_policy_matches_proof",
        selected_policy_evidence_coherent ? 1.0 : 0.0);
    metrics.emplace("simd_chaining_exact_visited_calls",
        static_cast<double>(
            output.simd_chaining_execution_proof.policy.exact_visited_calls));
    metrics.emplace("simd_chaining_bounded_batch_approximate_calls",
        static_cast<double>(output.simd_chaining_execution_proof.policy
            .bounded_batch_approximate_calls));
    metrics.emplace("simd_chaining_bounded_score_batches",
        static_cast<double>(output.simd_chaining_execution_proof.policy
            .bounded_score_batches));
    metrics.emplace("simd_chaining_bounded_improving_batches",
        static_cast<double>(output.simd_chaining_execution_proof.policy
            .bounded_improving_batches));
    metrics.emplace("simd_chaining_bounded_nonimproving_batches",
        static_cast<double>(output.simd_chaining_execution_proof.policy
            .bounded_nonimproving_batches));
    metrics.emplace("simd_chaining_bounded_cutoff_stops",
        static_cast<double>(output.simd_chaining_execution_proof.policy
            .bounded_cutoff_stops));
    metrics.emplace("simd_chaining_bounded_candidates_skipped",
        static_cast<double>(output.simd_chaining_execution_proof.policy
            .bounded_candidates_skipped));
    metrics.emplace("simd_chaining_global_best_rescue_probes",
        static_cast<double>(output.simd_chaining_execution_proof.policy
            .global_best_rescue_probes));
    metrics.emplace("simd_chaining_global_best_rescue_wins",
        static_cast<double>(output.simd_chaining_execution_proof.policy
            .global_best_rescue_wins));
    metrics.emplace("simd_chaining_policy_rejections",
        static_cast<double>(output.simd_chaining_execution_proof.policy
            .policy_rejections));
    metrics.emplace("simd_chaining_policy_evidence_saturated",
        output.simd_chaining_execution_proof.policy.saturated ? 1.0 : 0.0);
    metrics.emplace("simd_chaining_contiguous_predecessor_store_batches",
        static_cast<double>(output.simd_chaining_execution_proof
            .contiguous_predecessor_store_batches));
    metrics.emplace(
        "simd_chaining_backtrack_primary_contiguous_batches",
        static_cast<double>(output.simd_chaining_execution_proof
            .backtrack_primary_contiguous_batches));
    metrics.emplace(
        "simd_chaining_backtrack_all_starts_visited_fast_batches",
        static_cast<double>(output.simd_chaining_execution_proof
            .backtrack_all_starts_visited_fast_batches));
    metrics.emplace("simd_chaining_execution_proof_accepted",
        output.simd_chaining_policy_selected
            && output.simd_chaining_execution_proof_present
            && output.simd_chaining_execution_proof
                .accepted(output.selected_simd_chaining_policy,
                    /*require_nonempty_vector_work=*/true)
            ? 1.0 : 0.0);
    metrics.emplace("simd_chaining_scalar_score_branches",
        static_cast<double>(output.simd_chaining_execution_proof
            .scalar_score_branches));
    metrics.emplace("simd_chaining_scalar_backtrack_branches",
        static_cast<double>(output.simd_chaining_execution_proof
            .scalar_backtrack_branches));
    metrics.emplace("simd_chaining_shared_scalar_backtrack_calls",
        static_cast<double>(output.simd_chaining_execution_proof
            .shared_scalar_backtrack_calls));
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
    metrics.emplace("vulkan_seed_open_addressed_lookup_dispatches",
                    static_cast<double>(output.partial_vulkan_execution_proof
                                            .seed_open_addressed_lookup_dispatches));
    metrics.emplace("vulkan_seed_sorted_dense_lookup_dispatches",
                    static_cast<double>(output.partial_vulkan_execution_proof
                                            .seed_sorted_dense_lookup_dispatches));
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
    metrics.emplace("vulkan_seed_anchor_proof_operations",
        static_cast<double>(gpu_index.seed_anchor_proof_operations));
    metrics.emplace("vulkan_seed_canonical_anchor_count",
        static_cast<double>(gpu_index.seed_canonical_anchor_count));
    metrics.emplace("vulkan_seed_canonical_anchor_order_checks",
        static_cast<double>(gpu_index.seed_canonical_anchor_order_checks));
    metrics.emplace("vulkan_seed_canonical_anchor_order_accepts",
        static_cast<double>(gpu_index.seed_canonical_anchor_order_accepts));
    metrics.emplace("vulkan_seed_canonical_tie_normalization_runs",
        static_cast<double>(
            gpu_index.seed_canonical_tie_normalization_runs));
    metrics.emplace("vulkan_seed_canonical_tie_normalization_anchors",
        static_cast<double>(
            gpu_index.seed_canonical_tie_normalization_anchors));
    metrics.emplace("vulkan_seed_anchor_digest_lane0_hi32",
        uint64_hi32_metric(gpu_index.seed_anchor_digest_lane0));
    metrics.emplace("vulkan_seed_anchor_digest_lane0_lo32",
        uint64_lo32_metric(gpu_index.seed_anchor_digest_lane0));
    metrics.emplace("vulkan_seed_anchor_digest_lane1_hi32",
        uint64_hi32_metric(gpu_index.seed_anchor_digest_lane1));
    metrics.emplace("vulkan_seed_anchor_digest_lane1_lo32",
        uint64_lo32_metric(gpu_index.seed_anchor_digest_lane1));
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
    metrics.emplace("gpu_index_exact_count_dispatches",
        static_cast<double>(gpu_index.gpu_index_exact_count_dispatches));
    metrics.emplace("gpu_index_exact_count_prefix_scan_dispatches",
        static_cast<double>(
            gpu_index.gpu_index_exact_count_prefix_scan_dispatches));
    metrics.emplace("gpu_index_exact_count_control_readbacks",
        static_cast<double>(
            gpu_index.gpu_index_exact_count_control_readbacks));
    metrics.emplace("gpu_index_deterministic_tile_emit_dispatches",
        static_cast<double>(
            gpu_index.gpu_index_deterministic_tile_emit_dispatches));
    metrics.emplace("gpu_index_atomic_append_dispatches",
        static_cast<double>(gpu_index.gpu_index_atomic_append_dispatches));
    metrics.emplace("gpu_index_emit_validation_control_readbacks",
        static_cast<double>(
            gpu_index.gpu_index_emit_validation_control_readbacks));
    metrics.emplace("gpu_index_counted_minimizer_count",
        static_cast<double>(gpu_index.gpu_index_counted_minimizer_count));
    metrics.emplace("gpu_index_verified_emit_count",
        static_cast<double>(gpu_index.gpu_index_verified_emit_count));
    metrics.emplace("gpu_index_count_emit_parity_verified",
        static_cast<double>(gpu_index.gpu_index_count_emit_parity_verified));
    metrics.emplace("gpu_index_target_minimizer_capacity_estimate",
        static_cast<double>(
            gpu_index.gpu_index_target_minimizer_capacity_estimate));
    metrics.emplace("gpu_index_capacity_estimate_underestimated",
        static_cast<double>(
            gpu_index.gpu_index_capacity_estimate_underestimated));
    metrics.emplace("gpu_index_arena_trimmed_before_permit_shrink",
        static_cast<double>(
            gpu_index.gpu_index_arena_trimmed_before_permit_shrink));
    metrics.emplace("gpu_index_retained_minimizer_count",
        static_cast<double>(gpu_index.gpu_index_retained_minimizer_count));
    metrics.emplace("gpu_index_distinct_minimizer_count",
        static_cast<double>(gpu_index.gpu_index_distinct_minimizer_count));
    metrics.emplace("source_direct_gpu_index_materialization_proof_accepted",
        output.source_direct_gpu_index_materialization_proof_accepted
            ? 1.0 : 0.0);
    metrics.emplace("gpu_index_source_direct_operations",
        static_cast<double>(gpu_index.gpu_index_source_direct_operations));
    metrics.emplace("gpu_index_source_direct_terminal_successes",
        static_cast<double>(
            gpu_index.gpu_index_source_direct_terminal_successes));
    metrics.emplace("gpu_index_source_direct_empty_bypasses",
        static_cast<double>(
            gpu_index.gpu_index_source_direct_empty_bypasses));
    metrics.emplace("gpu_index_source_direct_zero_query_bypasses",
        static_cast<double>(
            gpu_index.gpu_index_source_direct_zero_query_bypasses));
    metrics.emplace("gpu_index_source_direct_planning_receipts",
        static_cast<double>(
            gpu_index.gpu_index_source_direct_planning_receipts));
    metrics.emplace("gpu_index_source_direct_terminal_receipts",
        static_cast<double>(
            gpu_index.gpu_index_source_direct_terminal_receipts));
    metrics.emplace("gpu_index_source_direct_materializations",
        static_cast<double>(
            gpu_index.gpu_index_source_direct_materializations));
    metrics.emplace("gpu_index_source_direct_resident_publications",
        static_cast<double>(
            gpu_index.gpu_index_source_direct_resident_publications));
    metrics.emplace("gpu_index_source_direct_logical_control_bytes",
        static_cast<double>(
            gpu_index.gpu_index_source_direct_logical_control_bytes));
    metrics.emplace("gpu_index_source_direct_logical_position_bytes",
        static_cast<double>(
            gpu_index.gpu_index_source_direct_logical_position_bytes));
    metrics.emplace("gpu_index_source_direct_control_requirement_bytes",
        static_cast<double>(
            gpu_index.gpu_index_source_direct_control_requirement_bytes));
    metrics.emplace("gpu_index_source_direct_position_requirement_bytes",
        static_cast<double>(
            gpu_index.gpu_index_source_direct_position_requirement_bytes));
    metrics.emplace("gpu_index_source_direct_allocation_request_bytes",
        static_cast<double>(
            gpu_index.gpu_index_source_direct_allocation_request_bytes));
    metrics.emplace("gpu_index_source_direct_actual_control_bytes",
        static_cast<double>(
            gpu_index.gpu_index_source_direct_actual_control_bytes));
    metrics.emplace("gpu_index_source_direct_actual_position_bytes",
        static_cast<double>(
            gpu_index.gpu_index_source_direct_actual_position_bytes));
    metrics.emplace("gpu_index_source_direct_source_releases",
        static_cast<double>(
            gpu_index.gpu_index_source_direct_source_releases));
    metrics.emplace("gpu_index_source_direct_source_released_bytes",
        static_cast<double>(
            gpu_index.gpu_index_source_direct_source_released_bytes));
    metrics.emplace("gpu_index_source_direct_checksum_matches",
        static_cast<double>(
            gpu_index.gpu_index_source_direct_checksum_matches));
    metrics.emplace("gpu_index_source_direct_source_traversals",
        static_cast<double>(
            gpu_index.gpu_index_source_direct_source_traversals));
    metrics.emplace("gpu_index_source_direct_mapped_traversals",
        static_cast<double>(
            gpu_index.gpu_index_source_direct_mapped_traversals));
    metrics.emplace("gpu_index_source_direct_reflected_authorities",
        static_cast<double>(
            gpu_index.gpu_index_source_direct_reflected_authorities));
    metrics.emplace("gpu_index_source_direct_retained_authorities",
        static_cast<double>(
            gpu_index.gpu_index_source_direct_retained_authorities));
    metrics.emplace(
        "gpu_index_source_direct_retained_resident_permit_bytes",
        static_cast<double>(gpu_index
            .gpu_index_source_direct_retained_resident_permit_bytes));
    metrics.emplace("gpu_index_source_direct_cache_preinsertions",
        static_cast<double>(
            gpu_index.gpu_index_source_direct_cache_preinsertions));
    metrics.emplace("gpu_index_source_direct_expected_seed_consumers",
        static_cast<double>(
            gpu_index.gpu_index_source_direct_expected_seed_consumers));
    metrics.emplace("gpu_index_source_direct_completed_seed_consumers",
        static_cast<double>(
            gpu_index.gpu_index_source_direct_completed_seed_consumers));
    metrics.emplace("gpu_index_source_direct_required_lookups",
        static_cast<double>(
            gpu_index.gpu_index_source_direct_required_lookups));
    metrics.emplace("gpu_index_source_direct_required_hits",
        static_cast<double>(
            gpu_index.gpu_index_source_direct_required_hits));
    metrics.emplace("gpu_index_source_direct_required_misses",
        static_cast<double>(
            gpu_index.gpu_index_source_direct_required_misses));
    metrics.emplace("gpu_index_source_direct_fallback_attempts",
        static_cast<double>(
            gpu_index.gpu_index_source_direct_fallback_attempts));
    metrics.emplace("gpu_index_source_direct_mapped_prefix_passes",
        static_cast<double>(
            gpu_index.gpu_index_source_direct_mapped_prefix_passes));
    metrics.emplace("gpu_index_source_direct_mapped_prefix_lookups",
        static_cast<double>(
            gpu_index.gpu_index_source_direct_mapped_prefix_lookups));
    metrics.emplace("gpu_index_source_direct_mapped_prefix_comparisons",
        static_cast<double>(
            gpu_index.gpu_index_source_direct_mapped_prefix_comparisons));
    metrics.emplace("gpu_index_source_direct_seed_slab_requested_bytes",
        static_cast<double>(
            gpu_index.gpu_index_source_direct_seed_slab_requested_bytes));
    metrics.emplace("gpu_index_source_direct_seed_slab_initial_capacity",
        static_cast<double>(
            gpu_index.gpu_index_source_direct_seed_slab_initial_capacity));
    metrics.emplace("gpu_index_source_direct_seed_slab_actual_bytes",
        static_cast<double>(
            gpu_index.gpu_index_source_direct_seed_slab_actual_bytes));
    metrics.emplace("gpu_index_source_direct_seed_slab_retained_bytes",
        static_cast<double>(
            gpu_index.gpu_index_source_direct_seed_slab_retained_bytes));
    metrics.emplace("gpu_index_source_direct_seed_slab_permit_releases",
        static_cast<double>(
            gpu_index.gpu_index_source_direct_seed_slab_permit_releases));
    metrics.emplace("gpu_index_source_direct_seed_reserved_bytes",
        static_cast<double>(
            gpu_index.gpu_index_source_direct_seed_reserved_bytes));
    metrics.emplace("gpu_index_source_direct_seed_planned_vulkan_bytes",
        static_cast<double>(
            gpu_index.gpu_index_source_direct_seed_planned_vulkan_bytes));
    metrics.emplace("gpu_index_source_direct_seed_actual_vulkan_bytes",
        static_cast<double>(
            gpu_index.gpu_index_source_direct_seed_actual_vulkan_bytes));
    metrics.emplace("gpu_index_source_direct_seed_roundtrips",
        static_cast<double>(
            gpu_index.gpu_index_source_direct_seed_roundtrips));
    metrics.emplace("gpu_index_source_direct_zero_anchor_bypasses",
        static_cast<double>(
            gpu_index.gpu_index_source_direct_zero_anchor_bypasses));
    metrics.emplace("gpu_index_source_direct_seed_fence_completions",
        static_cast<double>(
            gpu_index.gpu_index_source_direct_seed_fence_completions));
    metrics.emplace("gpu_index_source_direct_cache_cleanup_releases",
        static_cast<double>(
            gpu_index.gpu_index_source_direct_cache_cleanup_releases));
    metrics.emplace("gpu_index_source_direct_resident_cleanup_bytes",
        static_cast<double>(
            gpu_index.gpu_index_source_direct_resident_cleanup_bytes));
    metrics.emplace("gpu_index_source_direct_pipeline_completions",
        static_cast<double>(
            gpu_index.gpu_index_source_direct_pipeline_completions));
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

/// Evaluate primary correctness against an independent exact-threshold truth
/// set. This work is outside the timed callback. In particular, a generated
/// dataset may contain input pairs but no pair whose ground-truth interval has
/// ED <= E; that empty truth set has complete recall, while a nonempty truth
/// set with no returned correct pairs remains a real zero and will regress.
[[nodiscard]] std::map<std::string, double> evaluate_prefilter_pipeline(
    std::string_view algorithm,
    const ParamMap& params,
    const AlignerInput& input,
    const AlignerOutput& output)
{
    auto metrics = evaluate_aligner(algorithm, params, input, output);
    if (output.vulkan_hardware_unavailable) {
        return metrics;
    }

    if (input.pairs.size() != input.queries_acgt.size()
        || input.pairs.size() != input.targets_acgt.size()) {
        throw std::logic_error(
            "prefilter truth oracle input vectors are not in lock-step");
    }

    if (output.correct_pair_flags.size() != input.pairs.size()) {
        throw std::logic_error(
            "prefilter correctness flags are not in lock-step with inputs");
    }
    std::vector<std::uint8_t> threshold_truth_flags(
        input.pairs.size(), 0u);
    for (std::size_t index = 0u; index < input.pairs.size(); ++index) {
        const auto& pair = input.pairs[index];
        const auto& target = input.targets_acgt[index];
        if (pair.truth_target_start > pair.truth_target_end
            || pair.truth_target_end > target.size()) {
            throw std::logic_error(
                "prefilter truth oracle interval is outside the target");
        }
        const std::string truth =
            bioseq::bench_prefilter_metrics::oriented_truth_sequence(
                std::string_view{target}.substr(
                    pair.truth_target_start,
                    pair.truth_target_end - pair.truth_target_start),
                pair.is_reverse);
        threshold_truth_flags[index] =
            bioseq::sequences::edit_distance_within_threshold(
                std::span<const char>{
                    input.queries_acgt[index].data(),
                    input.queries_acgt[index].size()},
                std::span<const char>{truth.data(), truth.size()},
                output.edit_threshold)
            ? 1u : 0u;
    }
    const auto correctness =
        bioseq::bench_prefilter_metrics::summarize_primary_correctness(
            threshold_truth_flags, output.correct_pair_flags);
    if (correctness.observed_correct_pairs != output.correct_pairs) {
        throw std::logic_error(
            "prefilter per-pair correctness flags disagree with aggregate");
    }
    metrics.insert_or_assign("threshold_truth_pair_count",
        static_cast<double>(correctness.truth_pairs));
    metrics.insert_or_assign("threshold_correct_pair_count",
        static_cast<double>(correctness.correct_truth_pairs));
    metrics.insert_or_assign("primary_correct_fraction",
        correctness.fraction);
    return metrics;
}

[[nodiscard]] nlohmann::json scalar_pipeline_proof_json(
    const ScalarPipelineExecutionProof& proof, bool present)
{
    return nlohmann::json{
        {"present", present},
        {"query_minimizers", {
            {"calls", proof.query_minimizers.calls},
            {"input_symbols", proof.query_minimizers.input_symbols},
            {"saturated", proof.query_minimizers.saturated},
        }},
        {"target_minimizers", {
            {"calls", proof.target_minimizers.calls},
            {"input_symbols", proof.target_minimizers.input_symbols},
            {"saturated", proof.target_minimizers.saturated},
        }},
        {"seed_lookup_calls", proof.seed_lookup_calls},
        {"seed_query_minimizers", proof.seed_query_minimizers},
        {"chain_score_calls", proof.chain_score_calls},
        {"chain_backtrack_calls", proof.chain_backtrack_calls},
        {"chain_anchor_inputs", proof.chain_anchor_inputs},
        {"ksw2_left_calls", proof.ksw2_left_calls},
        {"ksw2_gap_calls", proof.ksw2_gap_calls},
        {"ksw2_right_calls", proof.ksw2_right_calls},
        {"ksw2_input_cell_area", proof.ksw2_input_cell_area},
        {"saturated", proof.saturated},
    };
}

[[nodiscard]] nlohmann::json ksw2_batched_proof_json(
    const Ksw2BatchedExecutionProof& proof, bool present)
{
    return nlohmann::json{
        {"present", present},
        {"accepted_nonempty", present && proof.accepted(true)},
        {"align_region_invocations", proof.align_region_invocations},
        {"regions_seen", proof.regions_seen},
        {"fills_collected", proof.fills_collected},
        {"nonempty_fill_calls", proof.nonempty_fill_calls},
        {"short_bucket_dispatches", proof.short_bucket_dispatches},
        {"short_bucket_lanes", proof.short_bucket_lanes},
        {"long_fill_calls", proof.long_fill_calls},
        {"input_cell_area", proof.input_cell_area},
        {"saturated", proof.saturated},
    };
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
        BIOSEQ_PROOF_FIELD(resident_vector_stream_kernel_calls),
        BIOSEQ_PROOF_FIELD(resident_vector_stream_single_calls),
        BIOSEQ_PROOF_FIELD(resident_vector_stream_dual_calls),
        BIOSEQ_PROOF_FIELD(resident_vector_stream_nonempty_work_calls),
        BIOSEQ_PROOF_FIELD(resident_vector_stream_input_bp),
        BIOSEQ_PROOF_FIELD(resident_vector_stream_minimizers_emitted),
        BIOSEQ_PROOF_FIELD(generic_u64_kernel_calls),
        BIOSEQ_PROOF_FIELD(generic_u64_candidate_blocks),
        BIOSEQ_PROOF_FIELD(generic_u64_lane_visits),
        BIOSEQ_PROOF_FIELD(k15_w10_b8_u32_hash_kernel_calls),
        BIOSEQ_PROOF_FIELD(k15_w10_b8_u32_hash_candidate_blocks),
        BIOSEQ_PROOF_FIELD(k15_w10_b8_u32_hash_lane_visits),
        BIOSEQ_PROOF_FIELD(k15_w10_b8_packed22_groups),
        BIOSEQ_PROOF_FIELD(k15_w10_b8_packed22_lane_visits),
        BIOSEQ_PROOF_FIELD(tie_hash_prefilter_batches),
        BIOSEQ_PROOF_FIELD(tie_hash_prefilter_rejected_lanes),
        BIOSEQ_PROOF_FIELD(tie_raw_verification_batches),
        BIOSEQ_PROOF_FIELD(saturated),
    };
#undef BIOSEQ_PROOF_FIELD
}

[[nodiscard]] nlohmann::json paper_nthash_proof_json(
    const PackedPaperNthashExecutionProof& proof, bool present)
{
#define BIOSEQ_PROOF_FIELD(name) {#name, proof.name}
    return nlohmann::json{
        {"present", present},
        {"accepted_nonempty", present && proof.accepted(true)},
        BIOSEQ_PROOF_FIELD(scheme_id),
        BIOSEQ_PROOF_FIELD(kmer_size),
        BIOSEQ_PROOF_FIELD(window_size),
        BIOSEQ_PROOF_FIELD(nthash_rotation),
        BIOSEQ_PROOF_FIELD(source_storage_unit_bytes),
        BIOSEQ_PROOF_FIELD(input_symbols),
        BIOSEQ_PROOF_FIELD(input_storage_units),
        BIOSEQ_PROOF_FIELD(segments_combined),
        BIOSEQ_PROOF_FIELD(logical_streams),
        BIOSEQ_PROOF_FIELD(physical_vector_lanes),
        BIOSEQ_PROOF_FIELD(vector_registers_per_step),
        BIOSEQ_PROOF_FIELD(packed_decode_batches),
        BIOSEQ_PROOF_FIELD(packed_decode_lane_visits),
        BIOSEQ_PROOF_FIELD(hash_initialization_batches),
        BIOSEQ_PROOF_FIELD(rolling_hash_batches),
        BIOSEQ_PROOF_FIELD(hash_table_lookup_batches),
        BIOSEQ_PROOF_FIELD(canonical_sum_batches),
        BIOSEQ_PROOF_FIELD(window_update_batches),
        BIOSEQ_PROOF_FIELD(suffix_rebuild_batches),
        BIOSEQ_PROOF_FIELD(emission_decision_batches),
        BIOSEQ_PROOF_FIELD(emission_mask_store_batches),
        BIOSEQ_PROOF_FIELD(minimizers_emitted),
        BIOSEQ_PROOF_FIELD(tied_endpoint_records_emitted),
        BIOSEQ_PROOF_FIELD(stream_boundary_records_buffered),
        BIOSEQ_PROOF_FIELD(stream_boundary_records_committed),
        BIOSEQ_PROOF_FIELD(stream_boundary_duplicates_suppressed),
        BIOSEQ_PROOF_FIELD(scalar_collector_appends),
        BIOSEQ_PROOF_FIELD(partial_chunk_callback_batches),
        BIOSEQ_PROOF_FIELD(masked_tail_lane_visits),
        BIOSEQ_PROOF_FIELD(fail_closed_rejections),
        BIOSEQ_PROOF_FIELD(scalar_hash_symbols),
        BIOSEQ_PROOF_FIELD(scalar_window_transitions),
        BIOSEQ_PROOF_FIELD(scalar_minimum_decisions),
        BIOSEQ_PROOF_FIELD(fallback_calls),
        BIOSEQ_PROOF_FIELD(positions_are_segment_local),
        BIOSEQ_PROOF_FIELD(aggregation_mismatch),
        BIOSEQ_PROOF_FIELD(saturated),
    };
#undef BIOSEQ_PROOF_FIELD
}

[[nodiscard]] nlohmann::json simd_chaining_proof_json(
    const SimdChainingExecutionProof& proof,
    bool present,
    SimdChainingPolicy selected_policy,
    bool policy_selected)
{
#define BIOSEQ_PROOF_FIELD(name) {#name, proof.name}
    return nlohmann::json{
        {"present", present},
        {"selected_policy_present", policy_selected},
        {"selected_policy", policy_selected
            ? std::string{bioseq::chaining::simd_chaining_policy_name(
                selected_policy)}
            : std::string{"not_selected"}},
        {"selected_policy_matches_proof",
            policy_selected && present
                && proof.policy_evidence_coherent(
                    selected_policy,
                    selected_policy
                        == SimdChainingPolicy::bounded_batch_approximate)},
        {"selected_policy_proof_accepted",
            policy_selected && present
                && proof.accepted(selected_policy, true)},
        {"exact_hybrid_evidence_coherent",
            proof.exact_hybrid_evidence_coherent()},
        BIOSEQ_PROOF_FIELD(anchors_consumed),
        BIOSEQ_PROOF_FIELD(score_vector_batches),
        BIOSEQ_PROOF_FIELD(score_vector_lanes),
        BIOSEQ_PROOF_FIELD(score_masked_lanes),
        BIOSEQ_PROOF_FIELD(contiguous_predecessor_store_batches),
        BIOSEQ_PROOF_FIELD(descending_candidate_steps),
        BIOSEQ_PROOF_FIELD(visited_marks),
        BIOSEQ_PROOF_FIELD(max_chain_skip_stops),
        BIOSEQ_PROOF_FIELD(no_improvement_fast_batches),
        BIOSEQ_PROOF_FIELD(improvement_replay_batches),
        BIOSEQ_PROOF_FIELD(improvement_replay_lanes),
        BIOSEQ_PROOF_FIELD(global_best_vector_batches),
        BIOSEQ_PROOF_FIELD(global_best_vector_lanes),
        BIOSEQ_PROOF_FIELD(backtrack_rank_batches),
        BIOSEQ_PROOF_FIELD(backtrack_path_vector_batches),
        BIOSEQ_PROOF_FIELD(backtrack_commit_vector_batches),
        BIOSEQ_PROOF_FIELD(backtrack_vector_lanes),
        BIOSEQ_PROOF_FIELD(backtrack_overlap_stops),
        BIOSEQ_PROOF_FIELD(backtrack_primary_contiguous_batches),
        BIOSEQ_PROOF_FIELD(backtrack_all_starts_visited_fast_batches),
        BIOSEQ_PROOF_FIELD(scalar_orchestration_steps),
        BIOSEQ_PROOF_FIELD(fail_closed_rejections),
        BIOSEQ_PROOF_FIELD(scalar_score_branches),
        BIOSEQ_PROOF_FIELD(scalar_backtrack_branches),
        BIOSEQ_PROOF_FIELD(shared_scalar_backtrack_calls),
        {"policy", {
            {"exact_visited_calls", proof.policy.exact_visited_calls},
            {"bounded_batch_approximate_calls",
                proof.policy.bounded_batch_approximate_calls},
            {"bounded_score_batches", proof.policy.bounded_score_batches},
            {"bounded_improving_batches",
                proof.policy.bounded_improving_batches},
            {"bounded_nonimproving_batches",
                proof.policy.bounded_nonimproving_batches},
            {"bounded_cutoff_stops", proof.policy.bounded_cutoff_stops},
            {"bounded_candidates_skipped",
                proof.policy.bounded_candidates_skipped},
            {"global_best_rescue_probes",
                proof.policy.global_best_rescue_probes},
            {"global_best_rescue_wins",
                proof.policy.global_best_rescue_wins},
            {"policy_rejections", proof.policy.policy_rejections},
            {"saturated", proof.policy.saturated},
        }},
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
    std::string_view algorithm,
    const ParamMap& /*params*/,
    const AlignerInput& /*input*/,
    const AlignerOutput& output)
{
    const auto& proof = output.partial_vulkan_execution_proof;
    const auto& biological = output.biological_fingerprint;
    return nlohmann::json{
        {"payload_schema", "bioseq.xxl_hifi.route_proof"},
        {"payload_schema_version", 7},
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
        {"source_direct_gpu_index_materialization_proof_accepted",
            algorithm == "bioseq_block_vulkan_dp_vulkan_minimap_int"
                && output
                    .source_direct_gpu_index_materialization_proof_accepted},
        {"biological_fingerprint", {
            {"computed", biological.computed},
            {"saturated", biological.saturated},
            {"collection_count", biological.collection_count},
            {"match_result_count", biological.match_result_count},
            {"cigar_result_count", biological.cigar_result_count},
            {"cigar_operation_count", biological.cigar_operation_count},
            {"match_digest_lane0_hex",
                uint64_hex(biological.match_digest_lane0)},
            {"match_digest_lane1_hex",
                uint64_hex(biological.match_digest_lane1)},
            {"cigar_digest_lane0_hex",
                uint64_hex(biological.cigar_digest_lane0)},
            {"cigar_digest_lane1_hex",
                uint64_hex(biological.cigar_digest_lane1)},
        }},
        {"exact_simd_query_execution_proof", two_stack_proof_json(
            output.exact_simd_query_execution_proof,
            output.exact_simd_query_execution_proof_present)},
        {"exact_simd_target_execution_proof", two_stack_proof_json(
            output.exact_simd_target_execution_proof,
            output.exact_simd_target_execution_proof_present)},
        {"paper_nthash_query_execution_proof", paper_nthash_proof_json(
            output.paper_nthash_query_execution_proof,
            output.paper_nthash_query_execution_proof_present)},
        {"paper_nthash_target_execution_proof", paper_nthash_proof_json(
            output.paper_nthash_target_execution_proof,
            output.paper_nthash_target_execution_proof_present)},
        {"scalar_pipeline_execution_proof", scalar_pipeline_proof_json(
            output.scalar_pipeline_execution_proof,
            output.scalar_pipeline_execution_proof_present)},
        {"ksw2_batched_execution_proof", ksw2_batched_proof_json(
            output.ksw2_batched_execution_proof,
            output.ksw2_batched_execution_proof_present)},
        {"simd_chaining_execution_proof", simd_chaining_proof_json(
            output.simd_chaining_execution_proof,
            output.simd_chaining_execution_proof_present,
            output.selected_simd_chaining_policy,
            output.simd_chaining_policy_selected)},
        {"block_aligner_execution_proof", block_aligner_proof_json(
            output.block_aligner_execution_proof,
            output.block_aligner_execution_proof_present)},
        {"partial_vulkan_execution_proof", {
            {"query_sketch_dispatches", proof.query_sketch_dispatches},
            {"target_sketch_dispatches", proof.target_sketch_dispatches},
            {"target_index_build_dispatches",
                proof.target_index_build_dispatches},
            {"seed_lookup_dispatches", proof.seed_lookup_dispatches},
            {"seed_open_addressed_lookup_dispatches",
                proof.seed_open_addressed_lookup_dispatches},
            {"seed_sorted_dense_lookup_dispatches",
                proof.seed_sorted_dense_lookup_dispatches},
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
            {"seed_anchor_proof_operations",
                proof.seed_anchor_proof_operations},
            {"seed_canonical_anchor_count",
                proof.seed_canonical_anchor_count},
            {"seed_canonical_anchor_order_checks",
                proof.seed_canonical_anchor_order_checks},
            {"seed_canonical_anchor_order_accepts",
                proof.seed_canonical_anchor_order_accepts},
            {"seed_canonical_tie_normalization_runs",
                proof.seed_canonical_tie_normalization_runs},
            {"seed_canonical_tie_normalization_anchors",
                proof.seed_canonical_tie_normalization_anchors},
            {"seed_anchor_digest_lane0_hex",
                uint64_hex(proof.seed_anchor_digest_lane0)},
            {"seed_anchor_digest_lane1_hex",
                uint64_hex(proof.seed_anchor_digest_lane1)},
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
            {"gpu_index_exact_count_dispatches",
                proof.gpu_index_exact_count_dispatches},
            {"gpu_index_exact_count_prefix_scan_dispatches",
                proof.gpu_index_exact_count_prefix_scan_dispatches},
            {"gpu_index_exact_count_control_readbacks",
                proof.gpu_index_exact_count_control_readbacks},
            {"gpu_index_deterministic_tile_emit_dispatches",
                proof.gpu_index_deterministic_tile_emit_dispatches},
            {"gpu_index_atomic_append_dispatches",
                proof.gpu_index_atomic_append_dispatches},
            {"gpu_index_emit_validation_control_readbacks",
                proof.gpu_index_emit_validation_control_readbacks},
            {"gpu_index_counted_minimizer_count",
                proof.gpu_index_counted_minimizer_count},
            {"gpu_index_verified_emit_count",
                proof.gpu_index_verified_emit_count},
            {"gpu_index_count_emit_parity_verified",
                proof.gpu_index_count_emit_parity_verified},
            {"gpu_index_target_minimizer_capacity_estimate",
                proof.gpu_index_target_minimizer_capacity_estimate},
            {"gpu_index_capacity_estimate_underestimated",
                proof.gpu_index_capacity_estimate_underestimated},
            {"gpu_index_arena_trimmed_before_permit_shrink",
                proof.gpu_index_arena_trimmed_before_permit_shrink},
            {"gpu_index_retained_minimizer_count",
                proof.gpu_index_retained_minimizer_count},
            {"gpu_index_distinct_minimizer_count",
                proof.gpu_index_distinct_minimizer_count},
            {"gpu_index_source_direct_operations",
                proof.gpu_index_source_direct_operations},
            {"gpu_index_source_direct_terminal_successes",
                proof.gpu_index_source_direct_terminal_successes},
            {"gpu_index_source_direct_empty_bypasses",
                proof.gpu_index_source_direct_empty_bypasses},
            {"gpu_index_source_direct_zero_query_bypasses",
                proof.gpu_index_source_direct_zero_query_bypasses},
            {"gpu_index_source_direct_planning_receipts",
                proof.gpu_index_source_direct_planning_receipts},
            {"gpu_index_source_direct_terminal_receipts",
                proof.gpu_index_source_direct_terminal_receipts},
            {"gpu_index_source_direct_materializations",
                proof.gpu_index_source_direct_materializations},
            {"gpu_index_source_direct_resident_publications",
                proof.gpu_index_source_direct_resident_publications},
            {"gpu_index_source_direct_logical_control_bytes",
                proof.gpu_index_source_direct_logical_control_bytes},
            {"gpu_index_source_direct_logical_position_bytes",
                proof.gpu_index_source_direct_logical_position_bytes},
            {"gpu_index_source_direct_control_requirement_bytes",
                proof.gpu_index_source_direct_control_requirement_bytes},
            {"gpu_index_source_direct_position_requirement_bytes",
                proof.gpu_index_source_direct_position_requirement_bytes},
            {"gpu_index_source_direct_allocation_request_bytes",
                proof.gpu_index_source_direct_allocation_request_bytes},
            {"gpu_index_source_direct_actual_control_bytes",
                proof.gpu_index_source_direct_actual_control_bytes},
            {"gpu_index_source_direct_actual_position_bytes",
                proof.gpu_index_source_direct_actual_position_bytes},
            {"gpu_index_source_direct_source_releases",
                proof.gpu_index_source_direct_source_releases},
            {"gpu_index_source_direct_source_released_bytes",
                proof.gpu_index_source_direct_source_released_bytes},
            {"gpu_index_source_direct_checksum_matches",
                proof.gpu_index_source_direct_checksum_matches},
            {"gpu_index_source_direct_source_traversals",
                proof.gpu_index_source_direct_source_traversals},
            {"gpu_index_source_direct_mapped_traversals",
                proof.gpu_index_source_direct_mapped_traversals},
            {"gpu_index_source_direct_reflected_authorities",
                proof.gpu_index_source_direct_reflected_authorities},
            {"gpu_index_source_direct_retained_authorities",
                proof.gpu_index_source_direct_retained_authorities},
            {"gpu_index_source_direct_retained_resident_permit_bytes",
                proof
                    .gpu_index_source_direct_retained_resident_permit_bytes},
            {"gpu_index_source_direct_cache_preinsertions",
                proof.gpu_index_source_direct_cache_preinsertions},
            {"gpu_index_source_direct_expected_seed_consumers",
                proof.gpu_index_source_direct_expected_seed_consumers},
            {"gpu_index_source_direct_completed_seed_consumers",
                proof.gpu_index_source_direct_completed_seed_consumers},
            {"gpu_index_source_direct_required_lookups",
                proof.gpu_index_source_direct_required_lookups},
            {"gpu_index_source_direct_required_hits",
                proof.gpu_index_source_direct_required_hits},
            {"gpu_index_source_direct_required_misses",
                proof.gpu_index_source_direct_required_misses},
            {"gpu_index_source_direct_fallback_attempts",
                proof.gpu_index_source_direct_fallback_attempts},
            {"gpu_index_source_direct_mapped_prefix_passes",
                proof.gpu_index_source_direct_mapped_prefix_passes},
            {"gpu_index_source_direct_mapped_prefix_lookups",
                proof.gpu_index_source_direct_mapped_prefix_lookups},
            {"gpu_index_source_direct_mapped_prefix_comparisons",
                proof.gpu_index_source_direct_mapped_prefix_comparisons},
            {"gpu_index_source_direct_seed_slab_requested_bytes",
                proof.gpu_index_source_direct_seed_slab_requested_bytes},
            {"gpu_index_source_direct_seed_slab_initial_capacity",
                proof.gpu_index_source_direct_seed_slab_initial_capacity},
            {"gpu_index_source_direct_seed_slab_actual_bytes",
                proof.gpu_index_source_direct_seed_slab_actual_bytes},
            {"gpu_index_source_direct_seed_slab_retained_bytes",
                proof.gpu_index_source_direct_seed_slab_retained_bytes},
            {"gpu_index_source_direct_seed_slab_permit_releases",
                proof.gpu_index_source_direct_seed_slab_permit_releases},
            {"gpu_index_source_direct_seed_reserved_bytes",
                proof.gpu_index_source_direct_seed_reserved_bytes},
            {"gpu_index_source_direct_seed_planned_vulkan_bytes",
                proof.gpu_index_source_direct_seed_planned_vulkan_bytes},
            {"gpu_index_source_direct_seed_actual_vulkan_bytes",
                proof.gpu_index_source_direct_seed_actual_vulkan_bytes},
            {"gpu_index_source_direct_seed_roundtrips",
                proof.gpu_index_source_direct_seed_roundtrips},
            {"gpu_index_source_direct_zero_anchor_bypasses",
                proof.gpu_index_source_direct_zero_anchor_bypasses},
            {"gpu_index_source_direct_seed_fence_completions",
                proof.gpu_index_source_direct_seed_fence_completions},
            {"gpu_index_source_direct_cache_cleanup_releases",
                proof.gpu_index_source_direct_cache_cleanup_releases},
            {"gpu_index_source_direct_resident_cleanup_bytes",
                proof.gpu_index_source_direct_resident_cleanup_bytes},
            {"gpu_index_source_direct_pipeline_completions",
                proof.gpu_index_source_direct_pipeline_completions},
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

[[nodiscard]] std::string serialize_endpoint_qualification_sample(
    std::string_view algorithm,
    const ParamMap& /*params*/,
    const EndpointQualificationInput& /*input*/,
    const SimdChainingEndpointQualification& output)
{
    const auto endpoint_json = [](const EndpointCoordinatesEvidence& endpoint) {
        return nlohmann::json{
            {"present", endpoint.present},
            {"query_len", endpoint.query_len},
            {"query_start", endpoint.query_start},
            {"query_end", endpoint.query_end},
            {"is_reverse", endpoint.is_reverse},
            {"target_len", endpoint.target_len},
            {"target_start", endpoint.target_start},
            {"target_end", endpoint.target_end},
        };
    };
    nlohmann::json comparisons = nlohmann::json::array();
    for (const auto& comparison : output.comparisons) {
        comparisons.push_back({
            {"fixture_name", comparison.fixture_name},
            {"target_ordinal", comparison.target_ordinal},
            {"bounded_primary_count", comparison.bounded_primary_count},
            {"exact_primary_count", comparison.exact_primary_count},
            {"minimap2_primary_count", comparison.minimap2_primary_count},
            {"bounded_target_ordinal",
                comparison.bounded_target_ordinal},
            {"exact_target_ordinal", comparison.exact_target_ordinal},
            {"minimap2_target_ordinal",
                comparison.minimap2_target_ordinal},
            {"normalized_coordinates", comparison.normalized_coordinates},
            {"identity_and_strand_match",
                comparison.identity_and_strand_match},
            {"delta_vs_exact_bp", comparison.delta_vs_exact_bp},
            {"delta_vs_minimap2_bp", comparison.delta_vs_minimap2_bp},
            {"bounded_endpoint", endpoint_json(comparison.bounded_endpoint)},
            {"exact_endpoint", endpoint_json(comparison.exact_endpoint)},
            {"minimap2_endpoint", endpoint_json(comparison.minimap2_endpoint)},
            {"bounded_cutoff_stops", comparison.bounded_cutoff_stops},
            {"bounded_candidates_skipped",
                comparison.bounded_candidates_skipped},
            {"bounded_rescue_probes", comparison.bounded_rescue_probes},
            {"bounded_rescue_wins", comparison.bounded_rescue_wins},
            {"bounded_fixture_proof_accepted",
                comparison.bounded_fixture_proof_accepted},
            {"deterministic_rescue_fixture",
                comparison.deterministic_rescue_fixture},
            {"bounded_route_proof_accepted",
                comparison.bounded_route_proof_accepted},
            {"exact_route_proof_accepted",
                comparison.exact_route_proof_accepted},
            {"minimap2_route_proof_accepted",
                comparison.minimap2_route_proof_accepted},
        });
    }
    return nlohmann::json{
        {"payload_schema", "bioseq.simd_chaining.endpoint_qualification"},
        {"payload_schema_version", 2},
        {"endpoint_row", std::string{algorithm}},
        {"zero_reference_row",
            algorithm == "simd_chaining_endpoint_zero_reference"},
        {"identity_scope",
            "single-target caller ordinal plus backend-observed target length; MatchResult has no reference-id field"},
        {"accepted", output.accepted()},
        {"proof_accepted", output.proof_accepted()},
        {"rescue_evidence_accepted", output.rescue_evidence_accepted()},
        {"exact_route_proof_accepted",
            output.exact_route_proof_accepted},
        {"bounded_route_proof_accepted",
            output.bounded_route_proof_accepted},
        {"minimap2_route_proof_accepted",
            output.minimap2_route_proof_accepted},
        {"deterministic_rescue_fixture_runs",
            output.deterministic_rescue_fixture_runs},
        {"deterministic_rescue_fixture_mappings_compared",
            output.deterministic_rescue_fixture_mappings_compared},
        {"saturated", output.saturated},
        {"fixtures", output.fixtures},
        {"mappings_compared", output.mappings_compared},
        {"missing_primary", output.missing_primary},
        {"ambiguous_primary", output.ambiguous_primary},
        {"reference_or_strand_mismatches",
            output.reference_or_strand_mismatches},
        {"invalid_normalized_coordinates",
            output.invalid_normalized_coordinates},
        {"max_delta_vs_exact_bp", output.max_delta_vs_exact_bp},
        {"max_delta_vs_minimap2_bp", output.max_delta_vs_minimap2_bp},
        {"exact_policy_proof", simd_chaining_proof_json(
            output.exact_policy_proof,
            output.exact_policy_proof_present,
            SimdChainingPolicy::exact_visited,
            output.exact_policy_proof_present)},
        {"bounded_policy_proof", simd_chaining_proof_json(
            output.bounded_policy_proof,
            output.bounded_policy_proof_present,
            SimdChainingPolicy::bounded_batch_approximate,
            output.bounded_policy_proof_present)},
        {"fixture_comparisons", std::move(comparisons)},
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
    auto eval_prefilter_pipeline =
        std::function<std::map<std::string, double>(
            std::string_view, const ParamMap&,
            const AlignerInput&, const AlignerOutput&)>{
                evaluate_prefilter_pipeline};
    auto serialize_paired = std::function<std::string(
        std::string_view, const ParamMap&,
        const AlignerInput&, const AlignerOutput&)>{
            serialize_xxl_paired_sample};
    auto estimate_grouped32 = std::function<RunConstraints(
        std::string_view, std::string_view, simdjson::dom::element)>{
            estimate_grouped32_validation_resources};
    auto build_grouped32 = std::function<Grouped32ValidationInput(
        std::string_view, std::string_view, simdjson::dom::element,
        const RunConstraints&)>{build_grouped32_validation_input};
    auto run_grouped32 = std::function<Grouped32ValidationResult(
        std::string_view, const ParamMap&,
        const Grouped32ValidationInput&)>{run_grouped32_validation};
    auto evaluate_grouped32 = std::function<std::map<std::string, double>(
        std::string_view, const ParamMap&,
        const Grouped32ValidationInput&,
        const Grouped32ValidationResult&)>{evaluate_grouped32_validation};
    auto estimate_endpoint = std::function<RunConstraints(
        std::string_view, std::string_view, simdjson::dom::element)>{
            estimate_endpoint_qualification_resources};
    auto build_endpoint = std::function<EndpointQualificationInput(
        std::string_view, std::string_view, simdjson::dom::element,
        const RunConstraints&)>{build_endpoint_qualification_input};
    auto run_endpoint = std::function<SimdChainingEndpointQualification(
        std::string_view, const ParamMap&,
        const EndpointQualificationInput&)>{
            run_simd_chaining_endpoint_qualification};
    auto evaluate_endpoint = std::function<std::map<std::string, double>(
        std::string_view, const ParamMap&,
        const EndpointQualificationInput&,
        const SimdChainingEndpointQualification&)>{
            evaluate_simd_chaining_endpoint_qualification};
    auto serialize_endpoint = std::function<std::string(
        std::string_view, const ParamMap&,
        const EndpointQualificationInput&,
        const SimdChainingEndpointQualification&)>{
            serialize_endpoint_qualification_sample};

    return bioseq::bench_framework::run(
        argc, argv, report_dir, data_dir,
        create_benchmark<
            Grouped32ValidationInput,
            Grouped32ValidationResult>(
                "Grouped32 contiguous local32 validation",
                "bench_grouped32_local32_validation",
                config_path,
                estimate_grouped32,
                build_grouped32,
                run_grouped32,
                evaluate_grouped32),
        bioseq::bench_prefilters::make_prefilter_kernel_benchmark(
            config_path),
        bioseq::bench_prefilters::make_prefilter_small_seed_benchmark(
            config_path),
        create_benchmark<AlignerInput, AlignerOutput>(
            "SneakySnake full-pipeline regional impact",
            "bench_prefilter_pipeline",
            config_path, estimate, build, run, eval_prefilter_pipeline),
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
            "XXL_HIFI SIMD chaining iteration diagnostic",
            "bench_aligners_xxl_hifi_chain_focus",
            config_path, estimate, build, run_strict_xxl, eval,
            {}, serialize_paired),
        create_benchmark<
            EndpointQualificationInput,
            SimdChainingEndpointQualification>(
                "SIMD chaining bounded endpoint qualification",
                "bench_aligners_simd_chaining_endpoint_quality",
                config_path,
                estimate_endpoint,
                build_endpoint,
                run_endpoint,
                evaluate_endpoint,
                {},
                serialize_endpoint),
        create_benchmark<AlignerInput, AlignerOutput>(
            "XXL_HIFI report_76 relative-performance gate",
            "bench_aligners_xxl_hifi_relative",
            config_path, estimate, build, run_strict_xxl, eval,
            {}, serialize_paired),
        create_benchmark<AlignerInput, AlignerOutput>(
            "XXL_HIFI bounded paper-ntHash SIMD proof and speed gate",
            "bench_paper_nthash_xxl_hifi_probe",
            config_path, estimate, build, run_strict_xxl_paired, eval,
            {}, serialize_paired),
        create_benchmark<AlignerInput, AlignerOutput>(
            "XXL_HIFI paired relative-performance companion",
            "bench_aligners_xxl_hifi_paired",
            config_path, estimate, build, run_strict_xxl_paired, eval,
            {}, serialize_paired));
}
