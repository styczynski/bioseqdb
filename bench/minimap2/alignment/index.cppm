/**
 * @file bench/minimap2/alignment/index.cppm
 * @brief Aligner benchmark entry point on `bioseq.bench.framework`.
 *
 * The driver binds input construction, measured execution, evaluation, and
 * serialization callbacks to `bioseq::bench::framework` benchmark records.
 *
 * Benchmark blocks declared in `alignment_benchmark_configuration.json` include:
 *   * bench_aligners_grid          — full length × error × algorithm matrix
 *   * bench_aligners_repr_storage  — 32-bit / 128-bit minimizer storage
 *   * bench_aligners_rmq_repro     — pinned LONG/CCS RMQ-chaining seed
 *   * bench_aligners_curated       — curated correctness probes
 *   * bench_aligners_xxl_hifi_relative — report_76 relative-performance gate
 *   * bench_aligners_xxl_hifi_paired — serial paired-ratio companion gate
 *
 * Sample input reference: the bioseq benchmark framework example driver.
 */
module;

#include <hwy/highway.h>

#include <simdjson.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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

export module bioseq.bench.minimap2.alignment;

import bioseq.common.logging;
import bioseq.common.context;
import bioseq.common.checked_arithmetic;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.sequences.edit_distance_threshold;
import bioseq.sequences.kmers.two_stack_minimizers;
import bioseq.chaining.simd;
import bioseq.chaining.core;
import bioseq.alignment.block_aligner;
import bioseq.pipeline.minimap_port;
import bioseq.pipeline.pipe;
import bioseq.highway.test_support;
import bioseq.storage.lazy_sequence;
import bioseq.instrumentation;
import bioseq.parallelism.memory_admission;
import bioseq.bench.framework;
import bioseq.bench.prefilter_pipeline_metrics;
import bioseq.bench.minimap2;
import bioseq.sequences.generation;
import bioseq.gpu.vulkan.runtime;
import bioseq.bench.prefilters;

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

using bioseq::bench::framework::parameter_map;
using bioseq::bench::framework::run_constraints;
using bioseq::simd::base_alignment_algorithm;
using bioseq::simd::chaining_algorithm;
using bioseq::simd::match_result;
using bioseq::simd::match_result_set_identity;
using bioseq::simd::match_result_collection;
using bioseq::simd::minimap2_parameters;
using bioseq::simd::minimizer_algorithm;
using bioseq::prefilter::prefilter_backend;
using bioseq::prefilter::prefilter_execution_stats;
using bioseq::simd::partial_vulkan_execution_stats;
using bioseq::simd::two_stack_execution_stats;
using bioseq::simd::bounded_nthash_execution_stats;
using bioseq::chaining::simd_chaining_execution_stats;
using bioseq::simd::block_aligner_execution_stats;
using bioseq::simd::scalar_pipeline_execution_stats;
using bioseq::simd::ksw2_batched_execution_stats;
using bioseq::chaining::simd_chaining_policy;
namespace gen = bioseq::sequences::generation;
using bench_generator_t = gen::alphabet_sequence_generator<seq::dna4, std::uint32_t>;

// Comparable mappings may differ from the reference coordinate, but the
// contract is strict: a delta of 15 bp is already a rejection.
constexpr std::int32_t k_position_tolerance = 15;

/** @brief Cross-row ledger for prefilter-on versus prefilter-off parity. */
struct prefilter_pipeline_match_count_parity_state {
    struct threshold_state {
        bool prefilter_off_scheduled = false;
        bool vulkan_scheduled = false;
        std::optional<std::size_t> prefilter_off_match_count;
        std::optional<std::size_t> vulkan_match_count;
        std::optional<bool> vulkan_hardware_available;
        bool comparison_performed = false;
        bool comparison_passed = false;
    };

    std::mutex mutex;
    std::map<std::uint32_t, threshold_state> by_threshold;
};

/** @brief Generated sequence pairs and untimed representations for a run. */
struct aligner_input {
    /// Generated pairs are read-only after `on_input` returns, but
    /// `tool_minimap2` / `minimap_port` take non-const `compact_sequence_range&`
    /// refs even though they don't mutate. `mutable` lets concurrent
    /// `on_run` calls access them through their declared signatures
    /// without forcing a `const_cast` at every call site.
    mutable std::vector<typename bench_generator_t::query_target_pair> pairs;
    /// Pre-decoded ACGT strings in lock-step with `pairs`. Populated
    /// once in `build_input` so the per-iteration timing window of
    /// `run_aligner` does not include the O(bp_size) compact-to-ACGT
    /// conversion that `tool_minimap2` would otherwise do inline.
    std::vector<std::string> queries_acgt;
    std::vector<std::string> targets_acgt;
    /// Optional kmer-size override pulled from the test case `data`
    /// blob. 0 means "use the algorithm default (15)". Set on the
    /// REPR_STORAGE_* test cases to force a specific minimizer representation
    /// storage instantiation (K=7 → 32-bit, K=27 → 128-bit).
    std::int32_t kmer_size_override = 0;
    /// Shared because the framework may move/copy an input while every timed
    /// execution tuple for one batch must contribute to the same zero-work
    /// parity ledger. The evaluator is the only writer.
    std::shared_ptr<prefilter_pipeline_match_count_parity_state>
        prefilter_pipeline_match_count_parity =
            std::make_shared<prefilter_pipeline_match_count_parity_state>();
};

/** @brief Correctness, routing, and backend execution stats for a run. */
struct aligner_output {
    std::size_t total_results          = 0;
    std::size_t correct_pairs          = 0;
    std::size_t total_pairs            = 0;
    std::vector<std::uint8_t> correct_pair_flags;
    double      sum_abs_position_offset = 0.0;
    bool        vulkan_hardware_unavailable = false;
    /// The Vulkan-labelled rows are deliberately mixed CPU/GPU pipelines and
    /// therefore do not satisfy the all-on-GPU acceptance condition.
    bool        mixed_gpu_pipeline_marker = false;
    bool        all_gpu_acceptance_eligible = false;
    bool        edit_threshold_enforced = false;
    std::uint32_t edit_threshold = 0u;
    bool        prefilter_enabled = false;
    prefilter_backend selected_prefilter_backend = prefilter_backend::automatic;
    prefilter_execution_stats
        prefilter_stats{};
    bool prefilter_execution_stats_available = false;
    bool prefilter_execution_stats_consistent = false;
    bool        gpu_index_ownership_stats_consistent = false;
    /// True only when every completed pair in a source-direct row passed the
    /// single-operation validator before its stats was folded into the row
    /// aggregate.  The aggregate counters are additive and therefore cannot
    /// be reinterpreted as one operation after a multi-pair run.
    bool        is_source_direct_gpu_index_materialization_consistent = false;
    partial_vulkan_execution_stats partial_vulkan_stats{};
    match_result_set_identity biological_fingerprint{};
    two_stack_execution_stats exact_simd_query_execution_stats{};
    two_stack_execution_stats exact_simd_target_execution_stats{};
    bounded_nthash_execution_stats bounded_nthash_query_execution_stats{};
    bounded_nthash_execution_stats bounded_nthash_target_execution_stats{};
    simd_chaining_execution_stats simd_chaining_stats{};
    block_aligner_execution_stats block_aligner_stats{};
    scalar_pipeline_execution_stats scalar_pipeline_stats{};
    ksw2_batched_execution_stats ksw2_batched_stats{};
    bool exact_simd_query_execution_stats_available = false;
    bool exact_simd_target_execution_stats_available = false;
    bool bounded_nthash_query_execution_stats_available = false;
    bool bounded_nthash_target_execution_stats_available = false;
    bool simd_chaining_execution_stats_available = false;
    bool block_aligner_execution_stats_available = false;
    bool scalar_pipeline_execution_stats_available = false;
    bool ksw2_batched_execution_stats_available = false;
    /// Requested policy is recorded beside the operation-originated stats.
    /// It is never inferred from elapsed time. Non-SIMD rows leave
    /// `simd_chaining_policy_selected` false and are forbidden from carrying
    /// SIMD chaining stats.
    simd_chaining_policy selected_simd_chaining_policy =
        simd_chaining_policy::exact_visited;
    bool simd_chaining_policy_selected = false;
    /// Set only after `require_selected_route_execution` accepts the
    /// operation-originated stats for every processed pair. The strict XXL
    /// raw companion persists this beside each duration.
    bool        selected_route_execution_stats_consistent = false;
};

using bench_pair_t = typename bench_generator_t::query_target_pair;
using bench_sequence_t = typename bench_generator_t::sequence_type;

/// One fixed endpoint-corpus item. Every backend receives this exact query and
/// this exact single target; `target_ordinal` is compared before coordinates so
/// a future multi-target adapter cannot accidentally compare different loci.
/** @brief Generated endpoint corpus and its independent expected mapping. */
struct endpoint_qualification_fixture {
    std::string name;
    std::uint64_t target_ordinal = 0;
    mutable bench_pair_t pair{};
    std::string query_acgt;
    std::string target_acgt;
    float chain_skip_scale = 0.0f;
    std::int32_t max_chain_skip = 25;
};

/** @brief Fixed collection of endpoint qualification fixtures. */
struct endpoint_qualification_input {
    mutable std::vector<endpoint_qualification_fixture> fixtures;
};

/** @brief Comparable mapping coordinates observed from one backend. */
struct endpoint_coordinates_stats {
    bool present = false;
    std::uint64_t query_len = 0;
    std::uint64_t query_start = 0;
    std::uint64_t query_end = 0;
    bool is_reverse = false;
    std::uint64_t target_len = 0;
    std::uint64_t target_start = 0;
    std::uint64_t target_end = 0;
};

/** @brief Backend mapping and execution stats for one fixture. */
struct endpoint_backend_outcome {
    match_result_collection matches;
    // match_result has no reference-id field. The single-target adapter binds
    // this source-corpus ordinal at invocation and carries it beside results.
    std::uint64_t target_ordinal = 0;
};

/** @brief Cross-backend comparison result for one endpoint fixture. */
struct endpoint_fixture_comparison {
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
    endpoint_coordinates_stats bounded_endpoint{};
    endpoint_coordinates_stats exact_endpoint{};
    endpoint_coordinates_stats minimap2_endpoint{};
    std::uint64_t bounded_cutoff_stops = 0;
    std::uint64_t bounded_candidates_skipped = 0;
    std::uint64_t bounded_rescue_probes = 0;
    std::uint64_t bounded_rescue_wins = 0;
    bool bounded_fixture_stats_consistent = false;
    bool deterministic_rescue_fixture = false;
    bool bounded_route_stats_consistent = false;
    bool exact_route_stats_consistent = false;
    bool minimap2_route_stats_consistent = false;
};

/// A delta of exactly 15 bp is a failure; no mean or percentile can hide one
/// endpoint-qualification outlier.
/** @brief Aggregate acceptance result for the endpoint fixture suite. */
struct simd_chaining_endpoint_qualification {
    std::uint64_t fixtures = 0;
    std::uint64_t mappings_compared = 0;
    std::uint64_t missing_primary = 0;
    std::uint64_t ambiguous_primary = 0;
    std::uint64_t reference_or_strand_mismatches = 0;
    std::uint64_t invalid_normalized_coordinates = 0;
    std::uint64_t max_delta_vs_exact_bp = 0;
    std::uint64_t max_delta_vs_minimap2_bp = 0;
    bool saturated = false;

    simd_chaining_execution_stats exact_policy_stats{};
    simd_chaining_execution_stats bounded_policy_stats{};
    bool exact_policy_stats_available = false;
    bool bounded_policy_stats_available = false;
    bool exact_route_stats_consistent = false;
    bool bounded_route_stats_consistent = false;
    bool minimap2_route_stats_consistent = false;
    std::uint64_t deterministic_rescue_fixture_runs = 0;
    std::uint64_t deterministic_rescue_fixture_mappings_compared = 0;
    std::vector<endpoint_fixture_comparison> comparisons;

    [[nodiscard]] constexpr bool accepted() const noexcept {
        return !saturated && fixtures > 0 && mappings_compared == fixtures
            && missing_primary == 0 && ambiguous_primary == 0
            && reference_or_strand_mismatches == 0
            && invalid_normalized_coordinates == 0
            && max_delta_vs_exact_bp < 15
            && max_delta_vs_minimap2_bp < 15;
    }

    [[nodiscard]] bool has_consistent_execution_stats() const noexcept {
        return exact_route_stats_consistent && bounded_route_stats_consistent
            && minimap2_route_stats_consistent
            && exact_policy_stats_available && bounded_policy_stats_available
            && exact_policy_stats.is_internally_consistent(
                simd_chaining_policy::exact_visited, true)
            && bounded_policy_stats.is_internally_consistent(
                simd_chaining_policy::bounded_batch_approximate, true)
            && bounded_policy_stats.policy.bounded_cutoff_stops > 0
            && bounded_policy_stats.policy.bounded_candidates_skipped > 0
            && rescue_stats_accepted()
            && bounded_policy_stats.scalar_score_branches == 0
            && bounded_policy_stats.scalar_backtrack_branches == 0
            && bounded_policy_stats.shared_scalar_backtrack_calls == 0;
    }

    [[nodiscard]] constexpr bool rescue_stats_accepted() const noexcept {
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
    const parameter_map& params,
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

/** @brief Placement of a near-copy relative to the true target locus. */
enum class endpoint_decoy_placement : std::uint8_t {
    none = 0,
    before_true_locus,
    after_true_locus,
};

/** @brief Deterministic generation parameters for one endpoint fixture. */
struct endpoint_fixture_spec {
    std::string_view name;
    std::size_t target_bp;
    std::size_t query_bp;
    double substitution_rate;
    double insertion_rate;
    double deletion_rate;
    double reverse_rate;
    std::uint64_t seed;
    float chain_skip_scale;
    endpoint_decoy_placement decoy;
    std::int32_t max_chain_skip = 25;
};

// Fixed before any result is observed. The four nearby short exact-read sizes
// are empirical tail-size probes; they do not claim a source-level anchor
// count. The two constructed decoys put the true locus on opposite sides of a
// near-copy, the rescue item has its own structural construction below, and
// the remaining fixed-seed items span strand, penalty, error, and size buckets
// through the report-76 XXL seed. Every target is <= INT32_MAX because bundled
// minimap2 narrows its native ABI.
inline constexpr std::array<endpoint_fixture_spec, 16>
    k_endpoint_qualification_specs{{
        {"short_exact_1021bp", 12'000u, 1'021u,
            0.0, 0.0, 0.0, 0.0, 3'720'001u, 0.0f,
            endpoint_decoy_placement::none},
        {"short_exact_1030bp", 12'000u, 1'030u,
            0.0, 0.0, 0.0, 0.0, 3'720'002u, 0.0f,
            endpoint_decoy_placement::none},
        {"short_exact_1039bp", 12'000u, 1'039u,
            0.0, 0.0, 0.0, 0.0, 3'720'003u, 0.0f,
            endpoint_decoy_placement::none},
        {"short_exact_1067bp", 12'000u, 1'067u,
            0.0, 0.0, 0.0, 0.0, 3'720'004u, 0.0f,
            endpoint_decoy_placement::none},
        {"repetitive_decoy_true_after", 120'000u, 12'000u,
            0.0, 0.0, 0.0, 0.0, 3'720'005u, 0.0f,
            endpoint_decoy_placement::before_true_locus},
        {"repetitive_decoy_true_before", 120'000u, 12'000u,
            0.0, 0.0, 0.0, 0.0, 3'720'006u, 0.0f,
            endpoint_decoy_placement::after_true_locus},
        {"reverse_strand_fixed_seed", 80'000u, 8'000u,
            0.001, 0.0005, 0.0005, 1.0, 3'720'007u, 0.0f,
            endpoint_decoy_placement::none},
        {"max_skip_invalid_runs_and_rescue", 300'000u, 50'000u,
            0.005, 0.0125, 0.0125, 0.0, 3'720'008u, 0.0f,
            endpoint_decoy_placement::none},
        {"nonzero_skip_penalty_fixed_seed", 400'000u, 70'000u,
            0.005, 0.005, 0.005, 0.0, 3'720'009u, 0.75f,
            endpoint_decoy_placement::none},
        {"short_hifi", 3'000u, 300u,
            0.001, 0.0005, 0.0005, 0.0, 1'234'567u, 0.0f,
            endpoint_decoy_placement::none},
        {"medium_ccs", 30'000u, 5'000u,
            0.005, 0.005, 0.005, 0.0, 8'901'234u, 0.0f,
            endpoint_decoy_placement::none},
        {"long_modern_ont", 250'000u, 50'000u,
            0.005, 0.025, 0.020, 0.0, 6'543'210u, 0.0f,
            endpoint_decoy_placement::none},
        {"long_substitution_heavy", 250'000u, 50'000u,
            0.05, 0.0005, 0.0005, 0.0, 7'654'321u, 0.0f,
            endpoint_decoy_placement::none},
        {"long_indel_heavy", 250'000u, 30'000u,
            0.001, 0.02, 0.02, 0.0, 2'103'456u, 0.0f,
            endpoint_decoy_placement::none},
        {"very_long_hifi", 1'500'000u, 1'000'000u,
            0.001, 0.0005, 0.0005, 0.0, 8'765'432u, 0.0f,
            endpoint_decoy_placement::none},
        {"xxl_hifi_report76", 500'000'000u, 5'000'000u,
            0.001, 0.0005, 0.0005, 0.5, 3'219'876u, 0.0f,
            endpoint_decoy_placement::none},
    }};

static_assert([] {
    for (const auto& spec : k_endpoint_qualification_specs) {
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

/// Estimate the memory required by one `(benchmark, test_case, group)`
/// batch so the framework can throttle parallel batches against the
/// global memory budget.
///
/// We size off the data already in the JSON blob, before any sequence
/// generation. The cached `aligner_input` keeps every generated pair in compact
/// dna4/u32 storage and also keeps pre-decoded ACGT strings for the minimap2
/// reference path. Generation shares the compact target across pairs and
/// fills packed storage directly, so it carries no per-pair reference copy.
[[nodiscard]] run_constraints estimate_aligner_resources(
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
    // resident GPU-index frontend GPU builds hold a target index resident for the duration of the
    // timed run; omitting it let XXL_HIFI co-schedule two 500 Mbp GPU rows and
    // trip the fail-closed Vulkan admission gate before dispatch.
    const std::uint64_t generation_peak_extra = reference_length;
    const std::uint64_t aligner_scratch_extra =
        fragment_count * (fragment_length_mean / 2 + 1024);
    const std::uint64_t target_index_extra =
        bioseq::parallelism::minimizer_index_peak_bytes_for_bp(reference_length);
    const std::uint64_t extra_memory =
        bioseq::common::saturating_add(
            bioseq::common::saturating_add(
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
[[nodiscard]] aligner_input build_input(std::string_view /*data_dir*/,
                                        std::string_view test_case_name,
                                        simdjson::dom::element data,
                                        const run_constraints& constraints) {
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

    aligner_input input;
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

    bioseq_log<logging_level::debug>("[bioseq_benchmark] build_input '{}' produced {} pairs (k_override={})",
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

[[nodiscard]] endpoint_qualification_fixture make_endpoint_fixture(
        const endpoint_fixture_spec& spec,
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

    endpoint_qualification_fixture fixture;
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
            wall_bp * static_cast<std::size_t>(minimap2_parameters{}.mismatch_pen)
            < static_cast<std::size_t>(minimap2_parameters{}.zdrop));
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

    if (spec.decoy == endpoint_decoy_placement::none) {
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
        spec.decoy == endpoint_decoy_placement::before_true_locus
            ? spec.target_bp - quarter - spec.query_bp
            : quarter;
    const std::size_t decoy_start =
        spec.decoy == endpoint_decoy_placement::before_true_locus
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

[[nodiscard]] run_constraints estimate_endpoint_qualification_resources(
    std::string_view /*data_dir*/,
    std::string_view /*test_case_name*/,
    simdjson::dom::element /*data*/) noexcept
{
    constexpr std::uint64_t packed_bytes_per_bp_numerator = 1u;
    constexpr std::uint64_t packed_bytes_per_bp_denominator = 4u;
    std::uint64_t cached_bytes = 0;
    std::uint64_t largest_target_bp = 0;
    for (const auto& spec : k_endpoint_qualification_specs) {
        const std::uint64_t target_packed =
            (spec.target_bp * packed_bytes_per_bp_numerator
             + packed_bytes_per_bp_denominator - 1u)
            / packed_bytes_per_bp_denominator;
        const std::uint64_t query_packed =
            (spec.query_bp * packed_bytes_per_bp_numerator
             + packed_bytes_per_bp_denominator - 1u)
            / packed_bytes_per_bp_denominator;
        cached_bytes = bioseq::common::saturating_add(
            cached_bytes,
            target_packed + query_packed + spec.target_bp + spec.query_bp);
        largest_target_bp = std::max<std::uint64_t>(
            largest_target_bp, spec.target_bp);
    }
    const std::uint64_t peak_index_bytes =
        bioseq::parallelism::minimizer_index_peak_bytes_for_bp(
            largest_target_bp);
    const std::uint64_t query_scratch_bytes =
        bioseq::common::saturating_mul(
            static_cast<std::uint64_t>(5'000'000u), 2u);
    const std::uint64_t extra_bytes = bioseq::common::saturating_add(
        peak_index_bytes, query_scratch_bytes);
    return {
        .base_memory = static_cast<std::size_t>(cached_bytes),
        .extra_memory = static_cast<std::size_t>(extra_bytes),
    };
}

[[nodiscard]] endpoint_qualification_input build_endpoint_qualification_input(
    std::string_view /*data_dir*/,
    std::string_view /*test_case_name*/,
    simdjson::dom::element /*data*/,
    const run_constraints& /*constraints*/)
{
    endpoint_qualification_input input;
    input.fixtures.reserve(k_endpoint_qualification_specs.size());
    for (std::size_t index = 0;
         index < k_endpoint_qualification_specs.size(); ++index) {
        input.fixtures.push_back(make_endpoint_fixture(
            k_endpoint_qualification_specs[index],
            static_cast<std::uint64_t>(index)));
    }
    return input;
}

/// Map an algorithm key from the JSON config to a fully-resolved
/// `minimap2_parameters` plus a flag selecting the C reference vs the bioseq
/// port. A fixed kmer_size variant is selected for the special K=7 / K=27
/// REPR_STORAGE test cases via the test-case name passed via the input.
/** @brief Fully resolved algorithm choices for one benchmark row. */
struct algorithm_dispatch {
    bool is_minimap2_reference = false;
    bool explicit_vulkan = false;
    bool explicit_prefilter_vulkan = false;
    bool mixed_gpu_pipeline_marker = false;
    bool enforce_edit_threshold = false;
    minimap2_parameters params{};
};

[[nodiscard]] bool is_explicit_vulkan_hardware_available(std::string_view algo) {
    auto& runtime = bioseq::gpu::vulkan::runtime::instance();
    if (bioseq::gpu::vulkan::hardware_device_available(runtime)) {
        return true;
    }

    static std::atomic_bool logged_unavailable{false};
    if (!logged_unavailable.exchange(true, std::memory_order_relaxed)) {
        if (runtime.is_usable()) {
            bioseq_log<logging_level::warning>(
                "[bioseq_benchmark] explicit Vulkan benchmark rows are not measured on this host "
                "(first algo='{}'): active Vulkan device '{}' is a CPU/software device "
                "(VkPhysicalDeviceType={}); hardware Vulkan is required for GPU timing/parity",
                algo,
                runtime.features().device_name.data(),
                runtime.features().device_type);
        } else {
            bioseq_log<logging_level::warning>(
                "[bioseq_benchmark] explicit Vulkan benchmark rows are not measured on this host "
                "(first algo='{}'): Vulkan runtime is unavailable: {}",
                algo,
                runtime.initialization_error());
        }
    }
    return false;
}

/// Bind the generated fixed-target provider used by standalone benchmarks.
/// Production owns runtime target selection; this harness instead records the
/// single generated test root as every dispatch-mask dimension so route stats
/// remains complete without performing another Highway dispatch in a leaf.
void bind_fixed_benchmark_highway_target(minimap2_parameters& params) {
    const auto& binding =
        ::bioseq::simd_dispatch::highway_test_target_binding();
    if (binding.target == 0) {
        throw std::logic_error{
            "generated fixed-target benchmark binding has no target"};
    }
    params.highway_kernels = binding.kernels;
    params.highway_dispatch = {
        .compiled_targets = binding.target,
        .cpu_supported_targets = binding.target,
        .runnable_targets = binding.target,
        .planned_target = binding.target,
    };
    params.highway_executed_target = binding.target;
    // Frozen scheduler-registry code: DNA4 is alignment alphabet entry zero.
    params.alphabet_code = 0u;
}

[[nodiscard]] algorithm_dispatch dispatch_algorithm(std::string_view algo,
                                                    std::int32_t kmer_override,
                                                    std::uint32_t max_edits = 40u) {
    algorithm_dispatch dispatch;
    dispatch.params.kmer_size   = kmer_override > 0 ? kmer_override : 15;
    dispatch.params.window_size = 10;

    if (algo == "minimap2_dp") {
        dispatch.is_minimap2_reference = true;
        dispatch.params.chaining_algorithm       = chaining_algorithm::scalar_minimap2;
        dispatch.params.base_alignment_algorithm = base_alignment_algorithm::ksw2;
        dispatch.params.minimizer_algorithm      = minimizer_algorithm::scalar;
    } else if (algo == "minimap2_rmq") {
        dispatch.is_minimap2_reference = true;
        dispatch.params.chaining_algorithm       = chaining_algorithm::range_minimum_query;
        dispatch.params.base_alignment_algorithm = base_alignment_algorithm::ksw2;
        dispatch.params.minimizer_algorithm      = minimizer_algorithm::scalar;
    } else if (algo == "bioseq_ksw2_scalar_dp_scalar_mm") {
        dispatch.params.chaining_algorithm       = chaining_algorithm::scalar_minimap2;
        dispatch.params.base_alignment_algorithm = base_alignment_algorithm::ksw2;
        dispatch.params.minimizer_algorithm      = minimizer_algorithm::scalar;
    } else if (algo == "bioseq_ksw2_simd_dp_scalar_mm") {
        dispatch.params.chaining_algorithm       = chaining_algorithm::simd_minimap2;
        dispatch.params.base_alignment_algorithm = base_alignment_algorithm::ksw2;
        dispatch.params.minimizer_algorithm      = minimizer_algorithm::scalar;
    } else if (algo == "bioseq_prefilter_off"
               || algo == "bioseq_prefilter_scalar"
               || algo == "bioseq_prefilter_simd"
               || algo == "bioseq_prefilter_vulkan") {
        // Controlled full-pipeline comparison: all rows use identical SIMD
        // sketch/chaining and KSW2 extension, compute exact edit distance, and
        // expose only matches satisfying the same E. The sole varied dimension
        // is whether/where the lower-bound prefilter executes.
        dispatch.params.chaining_algorithm       = chaining_algorithm::simd_minimap2;
        dispatch.params.base_alignment_algorithm = base_alignment_algorithm::ksw2;
        dispatch.params.minimizer_algorithm      = minimizer_algorithm::two_stack_nthash;
        dispatch.params.compute_edit_distance    = true;
        dispatch.params.prefilter_max_edits      = max_edits;
        dispatch.params.exact_edit_filter_enabled = true;
        dispatch.params.exact_edit_filter_exclusive_upper_bound =
            static_cast<std::int64_t>(max_edits) + 1;
        dispatch.enforce_edit_threshold          = true;
        if (algo != "bioseq_prefilter_off") {
            dispatch.params.prefilter_enabled = true;
            if (algo == "bioseq_prefilter_scalar") {
                dispatch.params.prefilter_backend = prefilter_backend::scalar;
            } else if (algo == "bioseq_prefilter_simd") {
                dispatch.params.prefilter_backend = prefilter_backend::simd;
            } else {
                dispatch.params.prefilter_backend = prefilter_backend::vulkan;
                dispatch.explicit_prefilter_vulkan = true;
            }
        }
    } else if (algo == "bioseq_ksw2_simd_dp_two_stack_nthash"
               || algo == "bioseq_ksw2_simd_dp_two_stack_nthash_exact_visited"
               || algo == "bioseq_ksw2_simd_dp_two_stack_nthash_bounded_batch_approximate") {
        dispatch.params.chaining_algorithm       = chaining_algorithm::simd_minimap2;
        dispatch.params.base_alignment_algorithm = base_alignment_algorithm::ksw2;
        dispatch.params.minimizer_algorithm      = minimizer_algorithm::two_stack_nthash;
    } else if (algo == "bioseq_ksw2_simd_dp_bounded_nthash") {
        dispatch.params.chaining_algorithm       = chaining_algorithm::simd_minimap2;
        dispatch.params.base_alignment_algorithm = base_alignment_algorithm::ksw2;
        dispatch.params.minimizer_algorithm      = minimizer_algorithm::bounded_nthash_simd;
    } else if (algo == "bioseq_ksw2_vulkan_tiled_dp_two_stack_nthash") {
        // Mixed GPU/CPU route: CPU exact SIMD
        // minimizers, Vulkan score generation, CPU backtracking and CPU KSW2.
        // Explicit Vulkan selection fails loudly when no hardware device exists.
        // Identical minimizer (two-stack nthash) + base aligner (KSW2) to
        // bioseq_ksw2_simd_dp_two_stack_nthash, so the only varied dimension is
        // the chaining backend: SIMD_MM2 vs VULKAN_TILED_DP.
        dispatch.params.chaining_algorithm       = chaining_algorithm::vulkan_tiled_dynamic_programming;
        dispatch.params.base_alignment_algorithm = base_alignment_algorithm::ksw2;
        dispatch.params.minimizer_algorithm      = minimizer_algorithm::two_stack_nthash;
        dispatch.explicit_vulkan = true;
        dispatch.mixed_gpu_pipeline_marker = true;
    } else if (algo == "bioseq_ksw2_batched_simd_dp_two_stack_nthash") {
        // Batched KSW2 is selected through runtime parameters.
        dispatch.params.chaining_algorithm       = chaining_algorithm::simd_minimap2;
        dispatch.params.base_alignment_algorithm = base_alignment_algorithm::ksw2_batched;
        dispatch.params.minimizer_algorithm      = minimizer_algorithm::two_stack_nthash;
    } else if (algo == "bioseq_ksw2_simd_dp_simd_minimap2_slow") {
        dispatch.params.chaining_algorithm       = chaining_algorithm::simd_minimap2;
        dispatch.params.base_alignment_algorithm = base_alignment_algorithm::ksw2;
        dispatch.params.minimizer_algorithm      = minimizer_algorithm::simd_minimap2_reference;
    } else if (algo == "bioseq_block_simd_dp_scalar_mm") {
        dispatch.params.chaining_algorithm       = chaining_algorithm::simd_minimap2;
        dispatch.params.base_alignment_algorithm = base_alignment_algorithm::block_aligner;
        dispatch.params.minimizer_algorithm      = minimizer_algorithm::scalar;
        dispatch.params.do_cigar                 = true;
    } else if (algo == "bioseq_block_simd_dp_two_stack_nthash") {
        dispatch.params.chaining_algorithm       = chaining_algorithm::simd_minimap2;
        dispatch.params.base_alignment_algorithm = base_alignment_algorithm::block_aligner;
        dispatch.params.minimizer_algorithm      = minimizer_algorithm::two_stack_nthash;
        dispatch.params.do_cigar                 = true;
    } else if (algo == "bioseq_block_simd_dp_bounded_nthash") {
        dispatch.params.chaining_algorithm       = chaining_algorithm::simd_minimap2;
        dispatch.params.base_alignment_algorithm = base_alignment_algorithm::block_aligner;
        dispatch.params.minimizer_algorithm      = minimizer_algorithm::bounded_nthash_simd;
        dispatch.params.do_cigar                 = true;
    } else if (algo == "bioseq_block_vulkan_dp_vulkan_minimap_int") {
        // Mixed CPU/GPU path: Vulkan minimap-int minimizers, a
        // source-direct mapped sorted-dense resident target index, Vulkan seed
        // lookup, and Vulkan tiled-DP score generation, followed by CPU mapped
        // prefix authority reads, backtracking, and CPU Block Aligner. The
        // minimizer sketch is always the fused tiled path; this row keeps the
        // non-fused direct-resident seed/index
        // frontend for comparison. The only varied
        // dimensions versus bioseq_block_simd_dp_two_stack_nthash (SIMD
        // minimizers + SIMD chaining + block aligner) are the seeding (GPU
        // sketch + GPU seed-hit lookup) and the chaining backend; the base
        // aligner stays block because the G3SA extension kernel is not used.
        // All GPU stages fail loudly when no hardware Vulkan device exists.
        dispatch.params.chaining_algorithm        = chaining_algorithm::vulkan_tiled_dynamic_programming;
        dispatch.params.base_alignment_algorithm  = base_alignment_algorithm::block_aligner;
        dispatch.params.minimizer_algorithm       = minimizer_algorithm::vulkan_minimap_integer;
        dispatch.params.vulkan_enabled            = true;
        dispatch.params.vulkan_seed_hits_enabled  = true;
        dispatch.params.vulkan_index_cache_enabled = true;
        dispatch.params.do_cigar                  = true;
        // Keep the resident two-submit seed/index frontend off for this
        // direct-resident comparison row.
        dispatch.params.vulkan_fused_frontend_enabled  = false;
        dispatch.params.vulkan_gpu_index_build_enabled = false;
        dispatch.explicit_vulkan = true;
        dispatch.mixed_gpu_pipeline_marker = true;
    } else if (algo == "bioseq_block_vulkan_resident_gpu_index_frontend") {
        // Resident two-submit Vulkan frontend: parallel
        // ordered tile sketch + GPU target-index build (device radix sort +
        // race-free slot table) + GPU seed count/emit + device anchor sort +
        // Vulkan tiled-DP score generation, followed by CPU backtracking and CPU
        // Block Aligner (`vulkan_extend_enabled=false`). Same aligner as the
        // fastest CPU row; the varied dimensions are the GPU frontend and GPU
        // index build. It fails loudly when no hardware Vulkan device
        // exists. This row is compared against
        // `bioseq_block_simd_dp_two_stack_nthash` (SIMD minimizers + SIMD
        // chaining + block aligner) and `minimap2_dp`.
        dispatch.params.chaining_algorithm        = chaining_algorithm::vulkan_tiled_dynamic_programming;
        dispatch.params.base_alignment_algorithm  = base_alignment_algorithm::block_aligner;
        dispatch.params.minimizer_algorithm       = minimizer_algorithm::vulkan_minimap_integer;
        dispatch.params.vulkan_enabled            = true;
        dispatch.params.vulkan_seed_hits_enabled  = true;
        dispatch.params.vulkan_index_cache_enabled = true;
        dispatch.params.vulkan_fused_frontend_enabled  = true;
        dispatch.params.vulkan_gpu_index_build_enabled = true;
        dispatch.params.vulkan_extend_enabled          = false;
        dispatch.params.do_cigar                       = true;
        dispatch.explicit_vulkan = true;
        dispatch.mixed_gpu_pipeline_marker = true;
    } else if (algo == "bioseq_ksw2_rmq_scalar_mm") {
        dispatch.params.chaining_algorithm       = chaining_algorithm::range_minimum_query;
        dispatch.params.base_alignment_algorithm = base_alignment_algorithm::ksw2;
        dispatch.params.minimizer_algorithm      = minimizer_algorithm::scalar;
    } else {
        bioseq_log<logging_level::error>("[bioseq_benchmark] unknown algorithm '{}'", algo);
        throw std::invalid_argument(fmt::format(
            "bioseq_benchmark has no dispatch route for algorithm key '{}'", algo));
    }

    // C++ remains fail-safe by default (`exact_visited`). Benchmark SIMD rows
    // are explicit promotion candidates and therefore name the bounded policy;
    // the focused oracle row is the only deliberate exact SIMD exception.
    // A policy/stats mismatch is rejected later by
    // `require_selected_route_execution` rather than inferred from timing.
    if (dispatch.params.chaining_algorithm == chaining_algorithm::simd_minimap2) {
        dispatch.params.simd_chaining_policy =
            algo == "bioseq_ksw2_simd_dp_two_stack_nthash_exact_visited"
                ? simd_chaining_policy::exact_visited
                : simd_chaining_policy::bounded_batch_approximate;
    }
    if (!dispatch.is_minimap2_reference) {
        bind_fixed_benchmark_highway_target(dispatch.params);
    }
    return dispatch;
}

[[nodiscard]] constexpr bool
is_host_owned_gpu_index_materialization_consistent(
        const partial_vulkan_execution_stats& stats) noexcept {
    const bool open_addressed =
        stats.gpu_index_open_addressed_materializations == 1u
        && stats.gpu_index_sorted_dense_materializations == 0u
        && stats.seed_lookup_dispatches != 0u
        && stats.seed_open_addressed_lookup_dispatches
            == stats.seed_lookup_dispatches
        && stats.seed_sorted_dense_lookup_dispatches == 0u
        && stats.gpu_index_slot_capacity >= stats.gpu_index_unique_keys
        && stats.gpu_index_max_load_ppm != 0u
        && stats.gpu_index_max_load_ppm <= 700'000u
        && stats.gpu_index_max_probe != 0u;
    const bool sorted_dense =
        stats.gpu_index_open_addressed_materializations == 0u
        && stats.gpu_index_sorted_dense_materializations == 1u
        && stats.seed_lookup_dispatches != 0u
        && stats.seed_open_addressed_lookup_dispatches == 0u
        && stats.seed_sorted_dense_lookup_dispatches
            == stats.seed_lookup_dispatches
        && stats.gpu_index_slot_capacity == stats.gpu_index_unique_keys
        && stats.gpu_index_max_load_ppm == 1'000'000u
        && stats.gpu_index_max_probe == 0u;
    return !stats.is_saturated
        && stats.gpu_index_materializations == 1u
        && (open_addressed || sorted_dense)
        && stats.gpu_index_unique_keys != 0u
        && stats.gpu_index_slot_capacity != 0u
        && stats.gpu_index_logical_resident_bytes != 0u
        && stats.gpu_index_actual_allocation_bytes
            >= stats.gpu_index_logical_resident_bytes
        && stats.gpu_index_fully_touched_bytes
            == stats.gpu_index_actual_allocation_bytes
        && stats.gpu_index_staging_bytes == 0u
        && stats.gpu_index_permit_releases == 1u
        && stats.gpu_index_process_footprint_reflections == 1u
        && stats.gpu_index_reflected_footprint_bytes
            >= stats.gpu_index_actual_allocation_bytes
        && stats.post_seed_owned_gpu_cache_evictions == 1u
        && stats.post_seed_owned_gpu_resident_bytes_released
            == stats.gpu_index_actual_allocation_bytes
        && stats.post_seed_owned_cpu_index_releases == 1u
        && stats.post_seed_owned_cpu_position_bytes_released != 0u;
}

static_assert([] {
    partial_vulkan_execution_stats stats{};
    stats.gpu_index_materializations = 1u;
    stats.gpu_index_open_addressed_materializations = 1u;
    stats.seed_lookup_dispatches = 1u;
    stats.seed_open_addressed_lookup_dispatches = 1u;
    stats.gpu_index_unique_keys = 3u;
    stats.gpu_index_slot_capacity = 8u;
    stats.gpu_index_logical_resident_bytes = 128u;
    stats.gpu_index_actual_allocation_bytes = 256u;
    stats.gpu_index_fully_touched_bytes = 256u;
    stats.gpu_index_permit_releases = 1u;
    stats.gpu_index_process_footprint_reflections = 1u;
    stats.gpu_index_reflected_footprint_bytes = 256u;
    stats.gpu_index_max_load_ppm = 375'000u;
    stats.gpu_index_max_probe = 3u;
    stats.post_seed_owned_gpu_cache_evictions = 1u;
    stats.post_seed_owned_gpu_resident_bytes_released = 256u;
    stats.post_seed_owned_cpu_index_releases = 1u;
    stats.post_seed_owned_cpu_position_bytes_released = 128u;
    if (!is_host_owned_gpu_index_materialization_consistent(stats)) return false;
    stats.gpu_index_open_addressed_materializations = 0u;
    stats.gpu_index_sorted_dense_materializations = 1u;
    stats.seed_open_addressed_lookup_dispatches = 0u;
    stats.seed_sorted_dense_lookup_dispatches = 1u;
    stats.gpu_index_slot_capacity = 3u;
    stats.gpu_index_max_load_ppm = 1'000'000u;
    stats.gpu_index_max_probe = 0u;
    if (!is_host_owned_gpu_index_materialization_consistent(stats)) return false;
    stats.gpu_index_open_addressed_materializations = 1u;
    if (is_host_owned_gpu_index_materialization_consistent(stats)) return false;
    stats.gpu_index_open_addressed_materializations = 0u;
    stats.seed_open_addressed_lookup_dispatches = 1u;
    if (is_host_owned_gpu_index_materialization_consistent(stats)) return false;
    stats.seed_open_addressed_lookup_dispatches = 0u;
    stats.gpu_index_max_probe = 1u;
    if (is_host_owned_gpu_index_materialization_consistent(stats)) return false;
    stats.gpu_index_max_probe = 0u;
    stats.gpu_index_permit_releases = 0u;
    if (is_host_owned_gpu_index_materialization_consistent(stats)) return false;
    stats.gpu_index_permit_releases = 1u;
    stats.gpu_index_process_footprint_reflections = 0u;
    if (is_host_owned_gpu_index_materialization_consistent(stats)) return false;
    stats.gpu_index_process_footprint_reflections = 1u;
    stats.gpu_index_reflected_footprint_bytes = 255u;
    return !is_host_owned_gpu_index_materialization_consistent(stats);
}());

[[nodiscard]] constexpr bool
vulkan_frontend_target_index_stats_absent(
        const partial_vulkan_execution_stats& stats) noexcept {
    return stats.target_index_build_dispatches == 0u
        && stats.gpu_index_count_admissions == 0u
        && stats.gpu_index_main_peak_admissions == 0u
        && stats.gpu_index_resident_transfers == 0u
        && stats.gpu_index_adoption_rereserve_attempts == 0u
        && stats.gpu_index_adoption_successes == 0u
        && stats.gpu_index_owned_resident_bytes == 0u
        && stats.gpu_index_gate_enabled == 0u
        && stats.gpu_index_gate_word_count == 0u
        && stats.gpu_index_gate_mask == 0u
        && stats.gpu_index_gate_set_bits == 0u
        && stats.gpu_index_gate_digest_lo == 0u
        && stats.gpu_index_gate_digest_hi == 0u
        && stats.gpu_index_gate_count_dispatches == 0u
        && stats.gpu_index_gated_emit_dispatches == 0u
        && stats.gpu_index_ungated_emit_dispatches == 0u
        && stats.gpu_index_exact_count_dispatches == 0u
        && stats.gpu_index_exact_count_prefix_scan_dispatches == 0u
        && stats.gpu_index_exact_count_control_readbacks == 0u
        && stats.gpu_index_deterministic_tile_emit_dispatches == 0u
        && stats.gpu_index_atomic_append_dispatches == 0u
        && stats.gpu_index_emit_validation_control_readbacks == 0u
        && stats.gpu_index_counted_minimizer_count == 0u
        && stats.gpu_index_verified_emit_count == 0u
        && stats.gpu_index_count_emit_parity_verified == 0u
        && stats.gpu_index_target_minimizer_capacity_estimate == 0u
        && stats.gpu_index_capacity_estimate_underestimated == 0u
        && stats.gpu_index_arena_trimmed_before_permit_shrink == 0u;
}

[[nodiscard]] constexpr bool seed_sort_stats_absent(
        const partial_vulkan_execution_stats& stats) noexcept {
    return stats.seed_bitonic_sort_dispatches == 0u
        && stats.seed_radix_sort_operations == 0u
        && stats.seed_radix_anchor_count == 0u
        && stats.seed_radix_passes == 0u
        && stats.seed_radix_histogram_dispatches == 0u
        && stats.seed_radix_scan_dispatches == 0u
        && stats.seed_radix_scatter_dispatches == 0u
        && stats.seed_host_sort_fallbacks == 0u;
}

/// Accept source-direct sorted-dense execution stats without applying the
/// CPU-index-release contract of the host-owned materialization path.
/// The operation ledger publishes these source-direct counters only after its
/// terminal predicate succeeds. Generic layout/shape/allocation fields remain
/// cross-checks for N, D, layout identity, and the direct split-byte totals.
[[nodiscard]] constexpr bool
is_source_direct_gpu_index_materialization_consistent(
        const partial_vulkan_execution_stats& stats) noexcept {
    if (stats.is_saturated
        || stats.gpu_index_source_direct_operations != 1u
        || stats.gpu_index_source_direct_planning_receipts != 1u
        || stats.gpu_index_source_direct_terminal_receipts != 1u
        || stats.gpu_index_source_direct_terminal_successes != 1u
        || stats.gpu_index_source_direct_required_misses != 0u
        || stats.gpu_index_source_direct_fallback_attempts != 0u
        || stats.gpu_index_source_direct_seed_slab_initial_capacity != 0u
        || !vulkan_frontend_target_index_stats_absent(stats)
        || stats.seed_host_sort_fallbacks != 0u
        || stats.post_seed_owned_cpu_index_releases != 0u
        || stats.post_seed_owned_cpu_position_bytes_released != 0u) {
        return false;
    }

    if (stats.gpu_index_source_direct_empty_bypasses == 1u) {
        return stats.gpu_index_source_direct_materializations == 0u
            && stats.gpu_index_source_direct_zero_query_bypasses == 0u
            && stats.gpu_index_source_direct_resident_publications == 0u
            && stats.gpu_index_source_direct_logical_control_bytes == 0u
            && stats.gpu_index_source_direct_logical_position_bytes == 0u
            && stats.gpu_index_source_direct_control_requirement_bytes == 0u
            && stats.gpu_index_source_direct_position_requirement_bytes == 0u
            && stats.gpu_index_source_direct_allocation_request_bytes == 0u
            && stats.gpu_index_source_direct_actual_control_bytes == 0u
            && stats.gpu_index_source_direct_actual_position_bytes == 0u
            && stats.gpu_index_source_direct_source_releases == 0u
            && stats.gpu_index_source_direct_source_released_bytes == 0u
            && stats.gpu_index_source_direct_checksum_matches == 0u
            && stats.gpu_index_source_direct_source_traversals == 0u
            && stats.gpu_index_source_direct_mapped_traversals == 0u
            && stats.gpu_index_source_direct_reflected_authorities == 0u
            && stats.gpu_index_source_direct_retained_authorities == 0u
            && stats.gpu_index_source_direct_retained_resident_permit_bytes
                == 0u
            && stats.gpu_index_source_direct_cache_preinsertions == 0u
            && stats.gpu_index_source_direct_expected_seed_consumers == 0u
            && stats.gpu_index_source_direct_completed_seed_consumers == 0u
            && stats.gpu_index_source_direct_required_lookups == 0u
            && stats.gpu_index_source_direct_required_hits == 0u
            && stats.gpu_index_source_direct_mapped_prefix_passes == 0u
            && stats.gpu_index_source_direct_mapped_prefix_lookups == 0u
            && stats.gpu_index_source_direct_mapped_prefix_comparisons == 0u
            && stats.gpu_index_source_direct_seed_slab_requested_bytes == 0u
            && stats.gpu_index_source_direct_seed_slab_actual_bytes == 0u
            && stats.gpu_index_source_direct_seed_slab_retained_bytes == 0u
            && stats.gpu_index_source_direct_seed_slab_permit_releases == 0u
            && stats.gpu_index_source_direct_seed_reserved_bytes == 0u
            && stats.gpu_index_source_direct_seed_planned_vulkan_bytes == 0u
            && stats.gpu_index_source_direct_seed_actual_vulkan_bytes == 0u
            && stats.gpu_index_source_direct_seed_roundtrips == 0u
            && stats.gpu_index_source_direct_zero_anchor_bypasses == 0u
            && stats.gpu_index_source_direct_seed_fence_completions == 0u
            && stats.gpu_index_source_direct_cache_cleanup_releases == 0u
            && stats.gpu_index_source_direct_resident_cleanup_bytes == 0u
            && stats.gpu_index_source_direct_pipeline_completions == 0u
            && stats.chain_score_dispatches == 0u
            && seed_sort_stats_absent(stats)
            && stats.seed_lookup_dispatches == 0u
            && stats.seed_open_addressed_lookup_dispatches == 0u
            && stats.seed_sorted_dense_lookup_dispatches == 0u
            && stats.seed_anchor_stats_operations == 0u
            && stats.seed_canonical_anchor_count == 0u
            && stats.seed_canonical_anchor_order_checks == 0u
            && stats.seed_canonical_anchor_order_accepts == 0u
            && stats.seed_canonical_tie_normalization_runs == 0u
            && stats.seed_canonical_tie_normalization_anchors == 0u
            && stats.seed_anchor_digest_lane0 == 0u
            && stats.seed_anchor_digest_lane1 == 0u
            && stats.gpu_index_materializations == 0u
            && stats.gpu_index_open_addressed_materializations == 0u
            && stats.gpu_index_sorted_dense_materializations == 0u
            && stats.gpu_index_unique_keys == 0u
            && stats.gpu_index_slot_capacity == 0u
            && stats.gpu_index_logical_resident_bytes == 0u
            && stats.gpu_index_actual_allocation_bytes == 0u
            && stats.gpu_index_fully_touched_bytes == 0u
            && stats.gpu_index_staging_bytes == 0u
            && stats.gpu_index_permit_releases == 0u
            && stats.gpu_index_process_footprint_reflections == 0u
            && stats.gpu_index_reflected_footprint_bytes == 0u
            && stats.gpu_index_max_load_ppm == 0u
            && stats.gpu_index_max_probe == 0u
            && stats.gpu_index_retained_minimizer_count == 0u
            && stats.gpu_index_distinct_minimizer_count == 0u
            && stats.post_seed_owned_gpu_cache_evictions == 0u
            && stats.post_seed_owned_gpu_resident_bytes_released == 0u;
    }
    if (stats.gpu_index_source_direct_empty_bypasses != 0u) {
        return false;
    }

    const auto max_u64 = (std::numeric_limits<std::uint64_t>::max)();
    const bool exact_direct_allocation =
        stats.gpu_index_distinct_minimizer_count != 0u
        && stats.gpu_index_distinct_minimizer_count <= max_u64 / 16u
        && stats.gpu_index_source_direct_logical_control_bytes
            == 16u * stats.gpu_index_distinct_minimizer_count
        && stats.gpu_index_retained_minimizer_count != 0u
        && stats.gpu_index_retained_minimizer_count <= max_u64 / 8u
        && stats.gpu_index_source_direct_logical_position_bytes
            == 8u * stats.gpu_index_retained_minimizer_count
        && stats.gpu_index_source_direct_logical_control_bytes
            <= max_u64
                - stats.gpu_index_source_direct_logical_position_bytes
        && stats.gpu_index_logical_resident_bytes
            == stats.gpu_index_source_direct_logical_control_bytes
                + stats.gpu_index_source_direct_logical_position_bytes
        && stats.gpu_index_source_direct_control_requirement_bytes
            == stats.gpu_index_source_direct_actual_control_bytes
        && stats.gpu_index_source_direct_actual_control_bytes
            >= stats.gpu_index_source_direct_logical_control_bytes
        && stats.gpu_index_source_direct_position_requirement_bytes
            == stats.gpu_index_source_direct_actual_position_bytes
        && stats.gpu_index_source_direct_actual_position_bytes
            >= stats.gpu_index_source_direct_logical_position_bytes
        && stats.gpu_index_source_direct_control_requirement_bytes
            <= max_u64
                - stats.gpu_index_source_direct_position_requirement_bytes
        && stats.gpu_index_source_direct_allocation_request_bytes
            == stats.gpu_index_source_direct_control_requirement_bytes
                + stats.gpu_index_source_direct_position_requirement_bytes
        && stats.gpu_index_source_direct_actual_control_bytes
            <= max_u64 - stats.gpu_index_source_direct_actual_position_bytes
        && stats.gpu_index_source_direct_allocation_request_bytes
            == stats.gpu_index_source_direct_actual_control_bytes
                + stats.gpu_index_source_direct_actual_position_bytes
        && stats.gpu_index_actual_allocation_bytes
            == stats.gpu_index_source_direct_allocation_request_bytes;
    const bool exact_sorted_dense_shape =
        stats.gpu_index_source_direct_materializations == 1u
        && stats.gpu_index_materializations == 1u
        && stats.gpu_index_open_addressed_materializations == 0u
        && stats.gpu_index_sorted_dense_materializations == 1u
        && stats.gpu_index_retained_minimizer_count != 0u
        && stats.gpu_index_distinct_minimizer_count != 0u
        && stats.gpu_index_distinct_minimizer_count
            <= stats.gpu_index_retained_minimizer_count
        && stats.gpu_index_unique_keys
            == stats.gpu_index_distinct_minimizer_count
        && stats.gpu_index_slot_capacity == stats.gpu_index_unique_keys
        && stats.gpu_index_logical_resident_bytes != 0u
        && stats.gpu_index_actual_allocation_bytes
            >= stats.gpu_index_logical_resident_bytes
        && stats.gpu_index_fully_touched_bytes
            == stats.gpu_index_actual_allocation_bytes
        && stats.gpu_index_staging_bytes == 0u
        && stats.gpu_index_max_load_ppm == 1'000'000u
        && stats.gpu_index_max_probe == 0u;
    const bool reflected_authority =
        stats.gpu_index_source_direct_reflected_authorities == 1u
        && stats.gpu_index_source_direct_retained_authorities == 0u
        && stats.gpu_index_source_direct_retained_resident_permit_bytes == 0u
        && stats.gpu_index_permit_releases == 1u
        && stats.gpu_index_process_footprint_reflections == 1u
        && stats.gpu_index_reflected_footprint_bytes
            >= stats.gpu_index_actual_allocation_bytes;
    const bool retained_authority =
        stats.gpu_index_source_direct_reflected_authorities == 0u
        && stats.gpu_index_source_direct_retained_authorities == 1u
        && stats.gpu_index_source_direct_retained_resident_permit_bytes
            == stats.gpu_index_actual_allocation_bytes
        && stats.gpu_index_permit_releases == 0u
        && stats.gpu_index_process_footprint_reflections == 0u
        && stats.gpu_index_reflected_footprint_bytes == 0u;
    const bool exact_required_cache_use =
        stats.gpu_index_source_direct_zero_query_bypasses == 0u
        && stats.gpu_index_source_direct_expected_seed_consumers != 0u
        && stats.gpu_index_source_direct_completed_seed_consumers
            == stats.gpu_index_source_direct_expected_seed_consumers
        && stats.gpu_index_source_direct_required_lookups
            == stats.gpu_index_source_direct_completed_seed_consumers
        && stats.gpu_index_source_direct_required_hits
            == stats.gpu_index_source_direct_required_lookups
        && stats.gpu_index_source_direct_required_lookups
            <= (std::numeric_limits<std::uint64_t>::max)() / 2u
        && stats.gpu_index_source_direct_mapped_prefix_passes
            == 2u * stats.gpu_index_source_direct_required_lookups
        && (stats.gpu_index_source_direct_mapped_prefix_lookups == 0u
            ? stats.gpu_index_source_direct_mapped_prefix_comparisons == 0u
            : stats.gpu_index_source_direct_mapped_prefix_comparisons
                >= stats.gpu_index_source_direct_mapped_prefix_lookups);
    const bool exact_anchor_stats =
        stats.seed_anchor_stats_operations
            == stats.gpu_index_source_direct_seed_roundtrips
        && stats.seed_canonical_anchor_order_checks
            == stats.seed_anchor_stats_operations
        && stats.seed_canonical_anchor_order_accepts
            == stats.seed_canonical_anchor_order_checks
        && (stats.seed_anchor_stats_operations == 0u
            ? stats.seed_anchor_digest_lane0 == 0u
                && stats.seed_anchor_digest_lane1 == 0u
            : stats.seed_anchor_digest_lane0 != 0u
                && stats.seed_anchor_digest_lane1 != 0u);
    const bool positive_seed_slab =
        stats.gpu_index_source_direct_mapped_prefix_lookups != 0u
        && stats.gpu_index_source_direct_seed_slab_requested_bytes != 0u
        && stats.gpu_index_source_direct_seed_slab_requested_bytes
            == stats.gpu_index_source_direct_seed_slab_actual_bytes
        && stats.gpu_index_source_direct_seed_slab_actual_bytes
            == stats.gpu_index_source_direct_seed_slab_retained_bytes
        && stats.gpu_index_source_direct_seed_slab_permit_releases == 1u
        && stats.gpu_index_source_direct_seed_planned_vulkan_bytes != 0u
        && stats.gpu_index_source_direct_seed_planned_vulkan_bytes
            == stats.gpu_index_source_direct_seed_actual_vulkan_bytes
        && stats.gpu_index_source_direct_seed_planned_vulkan_bytes
            <= max_u64
                - stats.gpu_index_source_direct_seed_slab_actual_bytes
        && stats.gpu_index_source_direct_seed_reserved_bytes
            >= stats.gpu_index_source_direct_seed_planned_vulkan_bytes
                + stats.gpu_index_source_direct_seed_slab_actual_bytes
        && stats.gpu_index_source_direct_seed_roundtrips
            == stats.gpu_index_source_direct_required_lookups
        && stats.gpu_index_source_direct_zero_anchor_bypasses == 0u
        && stats.seed_canonical_anchor_count != 0u
        && stats.seed_lookup_dispatches
            == stats.gpu_index_source_direct_seed_roundtrips
        && stats.seed_open_addressed_lookup_dispatches == 0u
        && stats.seed_sorted_dense_lookup_dispatches
            == stats.seed_lookup_dispatches
        && stats.gpu_index_source_direct_seed_fence_completions
            == stats.gpu_index_source_direct_seed_roundtrips;
    const bool zero_anchor_seed =
        stats.gpu_index_source_direct_seed_slab_requested_bytes == 0u
        && stats.gpu_index_source_direct_seed_slab_actual_bytes == 0u
        && stats.gpu_index_source_direct_seed_slab_retained_bytes == 0u
        && stats.gpu_index_source_direct_seed_slab_permit_releases == 0u
        && stats.gpu_index_source_direct_seed_reserved_bytes == 0u
        && stats.gpu_index_source_direct_seed_planned_vulkan_bytes == 0u
        && stats.gpu_index_source_direct_seed_actual_vulkan_bytes == 0u
        && stats.gpu_index_source_direct_seed_roundtrips
            == stats.gpu_index_source_direct_required_lookups
        && stats.gpu_index_source_direct_zero_anchor_bypasses
            == stats.gpu_index_source_direct_seed_roundtrips
        && stats.seed_canonical_anchor_count == 0u
        && stats.seed_lookup_dispatches == 0u
        && stats.seed_open_addressed_lookup_dispatches == 0u
        && stats.seed_sorted_dense_lookup_dispatches == 0u
        && seed_sort_stats_absent(stats)
        && stats.gpu_index_source_direct_seed_fence_completions == 0u;

    const bool zero_query_seed =
        stats.gpu_index_source_direct_zero_query_bypasses == 1u
        && stats.gpu_index_source_direct_expected_seed_consumers == 0u
        && stats.gpu_index_source_direct_completed_seed_consumers == 0u
        && stats.gpu_index_source_direct_required_lookups == 0u
        && stats.gpu_index_source_direct_required_hits == 0u
        && stats.gpu_index_source_direct_required_misses == 0u
        && stats.gpu_index_source_direct_mapped_prefix_passes == 0u
        && stats.gpu_index_source_direct_mapped_prefix_lookups == 0u
        && stats.gpu_index_source_direct_mapped_prefix_comparisons == 0u
        && stats.gpu_index_source_direct_seed_slab_requested_bytes == 0u
        && stats.gpu_index_source_direct_seed_slab_actual_bytes == 0u
        && stats.gpu_index_source_direct_seed_slab_retained_bytes == 0u
        && stats.gpu_index_source_direct_seed_slab_permit_releases == 0u
        && stats.gpu_index_source_direct_seed_reserved_bytes == 0u
        && stats.gpu_index_source_direct_seed_planned_vulkan_bytes == 0u
        && stats.gpu_index_source_direct_seed_actual_vulkan_bytes == 0u
        && stats.gpu_index_source_direct_seed_roundtrips == 0u
        && stats.gpu_index_source_direct_zero_anchor_bypasses == 0u
        && stats.gpu_index_source_direct_seed_fence_completions == 0u
        && stats.seed_lookup_dispatches == 0u
        && stats.seed_open_addressed_lookup_dispatches == 0u
        && stats.seed_sorted_dense_lookup_dispatches == 0u
        && seed_sort_stats_absent(stats)
        && stats.chain_score_dispatches == 0u
        && stats.seed_anchor_stats_operations == 0u
        && stats.seed_canonical_anchor_count == 0u
        && stats.seed_canonical_anchor_order_checks == 0u
        && stats.seed_canonical_anchor_order_accepts == 0u
        && stats.seed_canonical_tie_normalization_runs == 0u
        && stats.seed_canonical_tie_normalization_anchors == 0u
        && stats.seed_anchor_digest_lane0 == 0u
        && stats.seed_anchor_digest_lane1 == 0u
        && stats.gpu_index_source_direct_pipeline_completions == 0u;
    const bool seeded_pipeline =
        stats.gpu_index_source_direct_pipeline_completions == 1u
        && exact_required_cache_use
        && exact_anchor_stats
        && (positive_seed_slab || zero_anchor_seed);

    return exact_direct_allocation
        && exact_sorted_dense_shape
        && stats.gpu_index_source_direct_resident_publications == 1u
        && stats.gpu_index_source_direct_source_releases == 1u
        && stats.gpu_index_source_direct_source_released_bytes != 0u
        && stats.gpu_index_source_direct_checksum_matches == 1u
        && stats.gpu_index_source_direct_source_traversals == 1u
        && stats.gpu_index_source_direct_mapped_traversals == 1u
        && (reflected_authority != retained_authority)
        && stats.gpu_index_source_direct_cache_preinsertions == 1u
        && (seeded_pipeline || zero_query_seed)
        && stats.gpu_index_source_direct_cache_cleanup_releases == 1u
        && stats.gpu_index_source_direct_resident_cleanup_bytes
            == stats.gpu_index_actual_allocation_bytes;
}

static_assert([] {
    const auto valid_reflected = [] {
        partial_vulkan_execution_stats stats{};
        stats.seed_lookup_dispatches = 1u;
        stats.seed_sorted_dense_lookup_dispatches = 1u;
        stats.seed_anchor_stats_operations = 1u;
        stats.seed_canonical_anchor_count = 1'337u;
        stats.seed_canonical_anchor_order_checks = 1u;
        stats.seed_canonical_anchor_order_accepts = 1u;
        stats.seed_anchor_digest_lane0 = 0x0123456789abcdefull;
        stats.seed_anchor_digest_lane1 = 0xfedcba9876543210ull;
        stats.gpu_index_materializations = 1u;
        stats.gpu_index_sorted_dense_materializations = 1u;
        stats.gpu_index_unique_keys = 17u;
        stats.gpu_index_slot_capacity = 17u;
        stats.gpu_index_logical_resident_bytes = 696u;
        stats.gpu_index_actual_allocation_bytes = 4'096u;
        stats.gpu_index_fully_touched_bytes = 4'096u;
        stats.gpu_index_permit_releases = 1u;
        stats.gpu_index_process_footprint_reflections = 1u;
        stats.gpu_index_reflected_footprint_bytes = 4'096u;
        stats.gpu_index_max_load_ppm = 1'000'000u;
        stats.gpu_index_retained_minimizer_count = 53u;
        stats.gpu_index_distinct_minimizer_count = 17u;
        stats.gpu_index_source_direct_operations = 1u;
        stats.gpu_index_source_direct_planning_receipts = 1u;
        stats.gpu_index_source_direct_terminal_receipts = 1u;
        stats.gpu_index_source_direct_terminal_successes = 1u;
        stats.gpu_index_source_direct_materializations = 1u;
        stats.gpu_index_source_direct_resident_publications = 1u;
        stats.gpu_index_source_direct_logical_control_bytes = 272u;
        stats.gpu_index_source_direct_logical_position_bytes = 424u;
        stats.gpu_index_source_direct_control_requirement_bytes = 1'024u;
        stats.gpu_index_source_direct_position_requirement_bytes = 3'072u;
        stats.gpu_index_source_direct_allocation_request_bytes = 4'096u;
        stats.gpu_index_source_direct_actual_control_bytes = 1'024u;
        stats.gpu_index_source_direct_actual_position_bytes = 3'072u;
        stats.gpu_index_source_direct_source_releases = 1u;
        stats.gpu_index_source_direct_source_released_bytes = 512u;
        stats.gpu_index_source_direct_checksum_matches = 1u;
        stats.gpu_index_source_direct_source_traversals = 1u;
        stats.gpu_index_source_direct_mapped_traversals = 1u;
        stats.gpu_index_source_direct_reflected_authorities = 1u;
        stats.gpu_index_source_direct_cache_preinsertions = 1u;
        stats.gpu_index_source_direct_expected_seed_consumers = 1u;
        stats.gpu_index_source_direct_completed_seed_consumers = 1u;
        stats.gpu_index_source_direct_required_lookups = 1u;
        stats.gpu_index_source_direct_required_hits = 1u;
        stats.gpu_index_source_direct_mapped_prefix_passes = 2u;
        stats.gpu_index_source_direct_mapped_prefix_lookups = 20u;
        stats.gpu_index_source_direct_mapped_prefix_comparisons = 80u;
        stats.gpu_index_source_direct_seed_slab_requested_bytes = 1'024u;
        stats.gpu_index_source_direct_seed_slab_actual_bytes = 1'024u;
        stats.gpu_index_source_direct_seed_slab_retained_bytes = 1'024u;
        stats.gpu_index_source_direct_seed_slab_permit_releases = 1u;
        stats.gpu_index_source_direct_seed_reserved_bytes = 4'096u;
        stats.gpu_index_source_direct_seed_planned_vulkan_bytes = 2'048u;
        stats.gpu_index_source_direct_seed_actual_vulkan_bytes = 2'048u;
        stats.gpu_index_source_direct_seed_roundtrips = 1u;
        stats.gpu_index_source_direct_seed_fence_completions = 1u;
        stats.gpu_index_source_direct_cache_cleanup_releases = 1u;
        stats.gpu_index_source_direct_resident_cleanup_bytes = 4'096u;
        stats.gpu_index_source_direct_pipeline_completions = 1u;
        return stats;
    };
    const auto reflected = valid_reflected();
    if (!is_source_direct_gpu_index_materialization_consistent(reflected)) {
        return false;
    }
    auto strict_reflected = reflected;
    strict_reflected.query_sketch_dispatches = 1u;
    strict_reflected.target_sketch_dispatches = 1u;
    strict_reflected.chain_score_dispatches = 1u;
    if (!strict_reflected.is_exact_source_direct_stage_mask_consistent(
            true, true, true, true)
        || strict_reflected.is_exact_stage_mask_consistent(
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
    if (!is_source_direct_gpu_index_materialization_consistent(retained)) {
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
    if (!is_source_direct_gpu_index_materialization_consistent(zero_anchor)) {
        return false;
    }
    auto empty_minimizer_zero_anchor = zero_anchor;
    empty_minimizer_zero_anchor.
        gpu_index_source_direct_mapped_prefix_lookups = 0u;
    empty_minimizer_zero_anchor.
        gpu_index_source_direct_mapped_prefix_comparisons = 0u;
    if (!is_source_direct_gpu_index_materialization_consistent(
            empty_minimizer_zero_anchor)) {
        return false;
    }
    empty_minimizer_zero_anchor.
        gpu_index_source_direct_mapped_prefix_passes = 1u;
    if (is_source_direct_gpu_index_materialization_consistent(
            empty_minimizer_zero_anchor)) {
        return false;
    }
    auto invalid_zero_anchor_sort = zero_anchor;
    invalid_zero_anchor_sort.seed_bitonic_sort_dispatches = 1u;
    if (is_source_direct_gpu_index_materialization_consistent(
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
    if (is_source_direct_gpu_index_materialization_consistent(
            invalid_zero_anchor_allocation)) {
        return false;
    }

    auto zero_query = reflected;
    zero_query.seed_lookup_dispatches = 0u;
    zero_query.seed_sorted_dense_lookup_dispatches = 0u;
    zero_query.seed_anchor_stats_operations = 0u;
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
    if (!is_source_direct_gpu_index_materialization_consistent(zero_query)) {
        return false;
    }
    auto invalid_zero_query_sort = zero_query;
    invalid_zero_query_sort.seed_radix_sort_operations = 1u;
    if (is_source_direct_gpu_index_materialization_consistent(
            invalid_zero_query_sort)) {
        return false;
    }
    auto invalid_zero_query_chain = zero_query;
    invalid_zero_query_chain.chain_score_dispatches = 1u;
    if (is_source_direct_gpu_index_materialization_consistent(
            invalid_zero_query_chain)) {
        return false;
    }
    zero_query.gpu_index_source_direct_required_hits = 1u;
    if (is_source_direct_gpu_index_materialization_consistent(zero_query)) {
        return false;
    }

    auto invalid = reflected;
    invalid.post_seed_owned_cpu_index_releases = 1u;
    if (is_source_direct_gpu_index_materialization_consistent(invalid)) {
        return false;
    }
    invalid = reflected;
    invalid.gpu_index_source_direct_required_hits = 0u;
    if (is_source_direct_gpu_index_materialization_consistent(invalid)) {
        return false;
    }
    invalid = reflected;
    invalid.gpu_index_source_direct_required_misses = 1u;
    if (is_source_direct_gpu_index_materialization_consistent(invalid)) {
        return false;
    }
    invalid = reflected;
    invalid.gpu_index_source_direct_completed_seed_consumers = 0u;
    if (is_source_direct_gpu_index_materialization_consistent(invalid)) {
        return false;
    }
    invalid = reflected;
    invalid.gpu_index_source_direct_mapped_prefix_passes = 1u;
    if (is_source_direct_gpu_index_materialization_consistent(invalid)) {
        return false;
    }
    invalid = reflected;
    invalid.gpu_index_source_direct_retained_authorities = 1u;
    if (is_source_direct_gpu_index_materialization_consistent(invalid)) {
        return false;
    }
    invalid = reflected;
    invalid.gpu_index_permit_releases = 0u;
    if (is_source_direct_gpu_index_materialization_consistent(invalid)) {
        return false;
    }
    invalid = retained;
    invalid.gpu_index_permit_releases = 1u;
    if (is_source_direct_gpu_index_materialization_consistent(invalid)) {
        return false;
    }
    invalid = reflected;
    invalid.gpu_index_gate_enabled = 1u;
    if (is_source_direct_gpu_index_materialization_consistent(invalid)) {
        return false;
    }
    invalid = reflected;
    invalid.gpu_index_source_direct_terminal_receipts = 0u;
    if (is_source_direct_gpu_index_materialization_consistent(invalid)) {
        return false;
    }
    invalid = reflected;
    invalid.gpu_index_source_direct_seed_slab_actual_bytes = 1'023u;
    if (is_source_direct_gpu_index_materialization_consistent(invalid)) {
        return false;
    }
    invalid = reflected;
    invalid.gpu_index_source_direct_actual_position_bytes = 3'071u;
    if (is_source_direct_gpu_index_materialization_consistent(invalid)) {
        return false;
    }
    invalid = reflected;
    invalid.gpu_index_source_direct_seed_slab_initial_capacity = 1u;
    if (is_source_direct_gpu_index_materialization_consistent(invalid)) {
        return false;
    }

    partial_vulkan_execution_stats empty{};
    empty.gpu_index_source_direct_operations = 1u;
    empty.gpu_index_source_direct_empty_bypasses = 1u;
    empty.gpu_index_source_direct_planning_receipts = 1u;
    empty.gpu_index_source_direct_terminal_receipts = 1u;
    empty.gpu_index_source_direct_terminal_successes = 1u;
    if (!is_source_direct_gpu_index_materialization_consistent(empty)) {
        return false;
    }
    constexpr std::array empty_zero_fields{
        &partial_vulkan_execution_stats::chain_score_dispatches,
        &partial_vulkan_execution_stats::seed_bitonic_sort_dispatches,
        &partial_vulkan_execution_stats::seed_radix_sort_operations,
        &partial_vulkan_execution_stats::seed_radix_anchor_count,
        &partial_vulkan_execution_stats::seed_radix_passes,
        &partial_vulkan_execution_stats::seed_radix_histogram_dispatches,
        &partial_vulkan_execution_stats::seed_radix_scan_dispatches,
        &partial_vulkan_execution_stats::seed_radix_scatter_dispatches,
        &partial_vulkan_execution_stats::gpu_index_permit_releases,
        &partial_vulkan_execution_stats::gpu_index_max_load_ppm,
        &partial_vulkan_execution_stats::gpu_index_max_probe,
        &partial_vulkan_execution_stats::gpu_index_count_admissions,
        &partial_vulkan_execution_stats::gpu_index_main_peak_admissions,
        &partial_vulkan_execution_stats::gpu_index_resident_transfers,
        &partial_vulkan_execution_stats::gpu_index_adoption_rereserve_attempts,
        &partial_vulkan_execution_stats::gpu_index_adoption_successes,
        &partial_vulkan_execution_stats::gpu_index_owned_resident_bytes,
        &partial_vulkan_execution_stats::gpu_index_gate_enabled,
        &partial_vulkan_execution_stats::gpu_index_gate_word_count,
        &partial_vulkan_execution_stats::gpu_index_gate_mask,
        &partial_vulkan_execution_stats::gpu_index_gate_set_bits,
        &partial_vulkan_execution_stats::gpu_index_gate_digest_lo,
        &partial_vulkan_execution_stats::gpu_index_gate_digest_hi,
        &partial_vulkan_execution_stats::gpu_index_gate_count_dispatches,
        &partial_vulkan_execution_stats::gpu_index_gated_emit_dispatches,
        &partial_vulkan_execution_stats::gpu_index_ungated_emit_dispatches,
        &partial_vulkan_execution_stats::gpu_index_exact_count_dispatches,
        &partial_vulkan_execution_stats::gpu_index_exact_count_prefix_scan_dispatches,
        &partial_vulkan_execution_stats::gpu_index_exact_count_control_readbacks,
        &partial_vulkan_execution_stats::gpu_index_deterministic_tile_emit_dispatches,
        &partial_vulkan_execution_stats::gpu_index_atomic_append_dispatches,
        &partial_vulkan_execution_stats::gpu_index_emit_validation_control_readbacks,
        &partial_vulkan_execution_stats::gpu_index_counted_minimizer_count,
        &partial_vulkan_execution_stats::gpu_index_verified_emit_count,
        &partial_vulkan_execution_stats::gpu_index_count_emit_parity_verified,
        &partial_vulkan_execution_stats::gpu_index_target_minimizer_capacity_estimate,
        &partial_vulkan_execution_stats::gpu_index_capacity_estimate_underestimated,
        &partial_vulkan_execution_stats::gpu_index_arena_trimmed_before_permit_shrink,
    };
    for (const auto field : empty_zero_fields) {
        auto contradictory_empty = empty;
        contradictory_empty.*field = 1u;
        if (is_source_direct_gpu_index_materialization_consistent(
                contradictory_empty)) {
            return false;
        }
    }
    empty.gpu_index_source_direct_resident_publications = 1u;
    return !is_source_direct_gpu_index_materialization_consistent(empty);
}());

static_assert(bioseq::gpu::vulkan::k_gpu_query_gate_word_count == 2'097'152u);
static_assert(bioseq::gpu::vulkan::k_gpu_query_gate_mask == 67'108'863u);

[[nodiscard]] constexpr bool
gpu_index_ownership_stats_consistent(
        const partial_vulkan_execution_stats& stats) noexcept {
    return !stats.is_saturated
        && stats.is_exact_target_index_route_consistent()
        && stats.target_sketch_dispatches == 1u
        && stats.target_index_build_dispatches == 1u
        && stats.seed_lookup_dispatches != 0u
        && stats.seed_open_addressed_lookup_dispatches == 0u
        && stats.seed_sorted_dense_lookup_dispatches
            == stats.seed_lookup_dispatches
        && stats.seed_anchor_stats_operations
            == stats.seed_lookup_dispatches
        && stats.seed_canonical_anchor_count != 0u
        && stats.seed_canonical_anchor_order_checks
            == stats.seed_anchor_stats_operations
        && stats.seed_canonical_anchor_order_accepts
            == stats.seed_canonical_anchor_order_checks
        && stats.seed_anchor_digest_lane0 != 0u
        && stats.seed_anchor_digest_lane1 != 0u
        && stats.gpu_index_count_admissions == 1u
        && stats.gpu_index_main_peak_admissions == 1u
        && stats.gpu_index_resident_transfers == 1u
        && stats.gpu_index_adoption_rereserve_attempts == 0u
        && stats.gpu_index_adoption_successes == 1u
        && stats.gpu_index_owned_resident_bytes != 0u
        && stats.gpu_index_gate_enabled == 0u
        && stats.gpu_index_gate_word_count == 0u
        && stats.gpu_index_gate_mask == 0u
        && stats.gpu_index_gate_set_bits == 0u
        && stats.gpu_index_gate_count_dispatches == 0u
        && stats.gpu_index_gated_emit_dispatches == 0u
        && stats.gpu_index_ungated_emit_dispatches == 1u
        && stats.gpu_index_retained_minimizer_count != 0u
        && stats.gpu_index_distinct_minimizer_count != 0u
        && stats.gpu_index_distinct_minimizer_count
            <= stats.gpu_index_retained_minimizer_count
        && stats.post_seed_owned_gpu_cache_evictions == 1u
        && stats.post_seed_owned_gpu_resident_bytes_released
            == stats.gpu_index_owned_resident_bytes
        && stats.post_seed_owned_cpu_index_releases == 0u
        && stats.post_seed_owned_cpu_position_bytes_released == 0u;
}

static_assert([] {
    partial_vulkan_execution_stats stats{};
    stats.target_sketch_dispatches = 1u;
    stats.target_index_build_dispatches = 1u;
    stats.seed_lookup_dispatches = 1u;
    stats.seed_sorted_dense_lookup_dispatches = 1u;
    stats.seed_anchor_stats_operations = 1u;
    stats.seed_canonical_anchor_count = 53u;
    stats.seed_canonical_anchor_order_checks = 1u;
    stats.seed_canonical_anchor_order_accepts = 1u;
    stats.seed_anchor_digest_lane0 = 0x0123456789abcdefull;
    stats.seed_anchor_digest_lane1 = 0xfedcba9876543210ull;
    stats.gpu_index_count_admissions = 1u;
    stats.gpu_index_main_peak_admissions = 1u;
    stats.gpu_index_resident_transfers = 1u;
    stats.gpu_index_adoption_successes = 1u;
    stats.gpu_index_owned_resident_bytes = 4096u;
    stats.gpu_index_ungated_emit_dispatches = 1u;
    stats.gpu_index_exact_count_dispatches = 1u;
    stats.gpu_index_exact_count_prefix_scan_dispatches = 3u;
    stats.gpu_index_exact_count_control_readbacks = 1u;
    stats.gpu_index_deterministic_tile_emit_dispatches = 1u;
    stats.gpu_index_atomic_append_dispatches = 0u;
    stats.gpu_index_emit_validation_control_readbacks = 1u;
    stats.gpu_index_counted_minimizer_count = 53u;
    stats.gpu_index_verified_emit_count = 53u;
    stats.gpu_index_count_emit_parity_verified = 1u;
    stats.gpu_index_target_minimizer_capacity_estimate = 100u;
    stats.gpu_index_capacity_estimate_underestimated = 0u;
    stats.gpu_index_arena_trimmed_before_permit_shrink = 1u;
    stats.gpu_index_retained_minimizer_count = 53u;
    stats.gpu_index_distinct_minimizer_count = 17u;
    stats.post_seed_owned_gpu_cache_evictions = 1u;
    stats.post_seed_owned_gpu_resident_bytes_released = 4096u;
    if (!gpu_index_ownership_stats_consistent(stats)) return false;

    auto invalid = stats;
    invalid.target_sketch_dispatches = 2u;
    if (gpu_index_ownership_stats_consistent(invalid)) return false;
    invalid = stats;
    invalid.target_index_build_dispatches = 2u;
    if (gpu_index_ownership_stats_consistent(invalid)) return false;
    invalid = stats;
    invalid.gpu_index_adoption_rereserve_attempts = 1u;
    if (gpu_index_ownership_stats_consistent(invalid)) return false;
    invalid = stats;
    invalid.gpu_index_gated_emit_dispatches = 1u;
    if (gpu_index_ownership_stats_consistent(invalid)) return false;
    invalid = stats;
    invalid.gpu_index_atomic_append_dispatches = 1u;
    if (gpu_index_ownership_stats_consistent(invalid)) return false;
    invalid = stats;
    invalid.gpu_index_count_emit_parity_verified = 0u;
    if (gpu_index_ownership_stats_consistent(invalid)) return false;
    invalid = stats;
    invalid.seed_open_addressed_lookup_dispatches = 1u;
    if (gpu_index_ownership_stats_consistent(invalid)) return false;
    invalid = stats;
    invalid.gpu_index_gate_word_count = 1u;
    if (gpu_index_ownership_stats_consistent(invalid)) return false;
    invalid = stats;
    invalid.gpu_index_gate_mask = 1u;
    if (gpu_index_ownership_stats_consistent(invalid)) return false;
    invalid = stats;
    invalid.gpu_index_gate_set_bits = 1u;
    if (gpu_index_ownership_stats_consistent(invalid)) return false;
    invalid = stats;
    invalid.gpu_index_distinct_minimizer_count = 54u;
    return !gpu_index_ownership_stats_consistent(invalid);
}());

[[nodiscard]] constexpr bool is_seed_radix_sort_stats_consistent(
        const partial_vulkan_execution_stats& stats) noexcept {
    return !stats.is_saturated
        && stats.seed_lookup_dispatches != 0u
        && stats.seed_bitonic_sort_dispatches == 0u
        && stats.seed_radix_sort_operations == stats.seed_lookup_dispatches
        && stats.seed_radix_anchor_count > 1024u
        && stats.seed_radix_passes != 0u
        && stats.seed_radix_histogram_dispatches == stats.seed_radix_passes
        && stats.seed_radix_scan_dispatches == stats.seed_radix_passes
        && stats.seed_radix_scatter_dispatches == stats.seed_radix_passes
        && stats.seed_host_sort_fallbacks == 0u;
}

static_assert([] {
    partial_vulkan_execution_stats stats{};
    stats.seed_lookup_dispatches = 1u;
    stats.seed_radix_sort_operations = 1u;
    stats.seed_radix_anchor_count = 1'520'944u;
    stats.seed_radix_passes = 4u;
    stats.seed_radix_histogram_dispatches = 4u;
    stats.seed_radix_scan_dispatches = 4u;
    stats.seed_radix_scatter_dispatches = 4u;
    if (!is_seed_radix_sort_stats_consistent(stats)) return false;
    stats.seed_radix_sort_operations = 2u;
    if (is_seed_radix_sort_stats_consistent(stats)) return false;
    stats.seed_radix_sort_operations = 1u;
    stats.seed_bitonic_sort_dispatches = 1u;
    if (is_seed_radix_sort_stats_consistent(stats)) return false;
    stats.seed_bitonic_sort_dispatches = 0u;
    stats.seed_host_sort_fallbacks = 1u;
    return !is_seed_radix_sort_stats_consistent(stats);
}());

[[nodiscard]] constexpr bool exact_b8_execution_accepted(
        const two_stack_execution_stats& stats) noexcept {
    return stats.is_internally_consistent(
               /*require_nonempty_vector_work=*/true)
        && stats.generic_u64_kernel_calls == 0
        && stats.generic_u64_candidate_blocks == 0
        && stats.generic_u64_lane_visits == 0
        && stats.kmer_15_window_10_byte_8_uint32_hash_kernel_calls > 0
        && stats.kmer_15_window_10_byte_8_uint32_hash_candidate_blocks
            == stats.vector_batches
        && stats.kmer_15_window_10_byte_8_uint32_hash_lane_visits
            == stats.vector_lanes_visited
        && stats.kmer_15_window_10_byte_8_packed_22_groups
            == stats.kmer_15_window_10_byte_8_uint32_hash_candidate_blocks
        && stats.kmer_15_window_10_byte_8_packed_22_lane_visits
            == stats.kmer_15_window_10_byte_8_uint32_hash_lane_visits;
}

[[nodiscard]] constexpr bool exact_resident_dual_vector_stream_accepted(
        const two_stack_execution_stats& stats) noexcept {
    return stats.is_internally_consistent(
               /*require_nonempty_vector_work=*/true)
        && stats.resident_vector_stream_kernel_calls > 0
        && stats.resident_vector_stream_single_calls == 0
        && stats.resident_vector_stream_dual_calls
            == stats.resident_vector_stream_kernel_calls
        && stats.resident_vector_stream_nonempty_work_calls
            == stats.resident_vector_stream_kernel_calls
        && stats.resident_vector_stream_input_bp == stats.bp_consumed
        && stats.resident_vector_stream_minimizers_emitted
            == stats.minimizers_emitted
        && stats.generic_u64_kernel_calls == 0
        && stats.kmer_15_window_10_byte_8_uint32_hash_kernel_calls == 0;
}

[[nodiscard]] constexpr bool exact_fast_execution_accepted(
        const two_stack_execution_stats& stats) noexcept {
    return exact_b8_execution_accepted(stats)
        || exact_resident_dual_vector_stream_accepted(stats);
}

static_assert([] {
    two_stack_execution_stats stats{};
    stats.bp_consumed = 4096u;
    stats.minimizers_emitted = 613u;
    stats.resident_vector_stream_kernel_calls = 1u;
    stats.resident_vector_stream_dual_calls = 1u;
    stats.resident_vector_stream_nonempty_work_calls = 1u;
    stats.resident_vector_stream_input_bp = stats.bp_consumed;
    stats.resident_vector_stream_minimizers_emitted =
        stats.minimizers_emitted;
    if (!exact_fast_execution_accepted(stats)) return false;
    auto zero_output = stats;
    zero_output.minimizers_emitted = 0u;
    zero_output.resident_vector_stream_minimizers_emitted = 0u;
    if (exact_fast_execution_accepted(zero_output)) return false;
    stats.generic_u64_kernel_calls = 1u;
    return !exact_fast_execution_accepted(stats);
}());

[[nodiscard]] constexpr bool scalar_minimizer_dimensions_absent(
        const scalar_pipeline_execution_stats& stats) noexcept {
    using scalar_minimizer_execution_stats =
        bioseq::simd::scalar_minimizer_execution_stats;
    return stats.query_minimizers == scalar_minimizer_execution_stats{}
        && stats.target_minimizers == scalar_minimizer_execution_stats{};
}

[[nodiscard]] constexpr bool scalar_seed_dimension_absent(
        const scalar_pipeline_execution_stats& stats) noexcept {
    return stats.seed_lookup_calls == 0u
        && stats.seed_query_minimizers == 0u;
}

[[nodiscard]] constexpr bool scalar_chain_dimension_absent(
        const scalar_pipeline_execution_stats& stats) noexcept {
    return stats.chain_score_calls == 0u
        && stats.chain_backtrack_calls == 0u
        && stats.chain_anchor_inputs == 0u;
}

[[nodiscard]] constexpr bool scalar_ksw2_dimension_absent(
        const scalar_pipeline_execution_stats& stats) noexcept {
    return stats.ksw2_left_calls == 0u
        && stats.ksw2_gap_calls == 0u
        && stats.ksw2_right_calls == 0u
        && stats.ksw2_input_cell_area == 0u;
}

[[nodiscard]] constexpr bool scalar_seed_dimension_accepted(
        const scalar_pipeline_execution_stats& stats,
        const bool require_nonempty_work) noexcept {
    return !stats.is_saturated
        && stats.seed_lookup_calls > 0u
        && (!require_nonempty_work || stats.seed_query_minimizers > 0u);
}

/// The generic pipeline executes CPU `collect_seed_hits` unless the statement
/// explicitly selects the strict Vulkan seed path. A Vulkan chaining-only
/// hybrid still uses CPU seed lookup and therefore must carry scalar seed stats.
[[nodiscard]] constexpr bool cpu_seed_lookup_selected(
        const minimap2_parameters& params) noexcept {
    return !(params.vulkan_enabled && params.vulkan_seed_hits_enabled);
}

[[nodiscard]] constexpr bool scalar_chain_dimension_accepted(
        const scalar_pipeline_execution_stats& stats,
        const bool require_nonempty_work) noexcept {
    const bool calls_are_paired =
        stats.chain_score_calls == stats.chain_backtrack_calls;
    const bool has_calls = stats.chain_score_calls != 0u;
    const bool work_matches_calls = has_calls
        == (stats.chain_anchor_inputs != 0u);
    return !stats.is_saturated
        && calls_are_paired
        && work_matches_calls
        && (has_calls || !require_nonempty_work);
}

[[nodiscard]] constexpr bool scalar_ksw2_dimension_accepted(
        const scalar_pipeline_execution_stats& stats,
        const bool require_nonempty_work) noexcept {
    const bool has_calls = stats.ksw2_left_calls != 0u
        || stats.ksw2_gap_calls != 0u
        || stats.ksw2_right_calls != 0u;
    return !stats.is_saturated
        && (has_calls || !require_nonempty_work)
        && (!require_nonempty_work || stats.ksw2_input_cell_area > 0u)
        && (has_calls || stats.ksw2_input_cell_area == 0u);
}

static_assert([] {
    scalar_pipeline_execution_stats stats{};
    if (!scalar_minimizer_dimensions_absent(stats)
        || !scalar_seed_dimension_absent(stats)
        || !scalar_chain_dimension_absent(stats)
        || !scalar_ksw2_dimension_absent(stats)) {
        return false;
    }
    stats.record_seed_lookup(7u);
    stats.record_scalar_chain(11u);
    stats.record_ksw2_call(
        bioseq::simd::scalar_ksw2_stage::gap, 3u, 5u);
    if (!scalar_seed_dimension_accepted(stats, true)
        || !scalar_chain_dimension_accepted(stats, true)
        || !scalar_ksw2_dimension_accepted(stats, true)
        || scalar_seed_dimension_absent(stats)
        || scalar_chain_dimension_absent(stats)
        || scalar_ksw2_dimension_absent(stats)) {
        return false;
    }
    auto missing_seed = stats;
    missing_seed.seed_lookup_calls = 0u;
    if (scalar_seed_dimension_accepted(missing_seed, true)) return false;
    auto empty_seed_work = stats;
    empty_seed_work.seed_query_minimizers = 0u;
    if (scalar_seed_dimension_accepted(empty_seed_work, true)
        || !scalar_seed_dimension_accepted(empty_seed_work, false)) {
        return false;
    }
    auto saturated_seed = stats;
    saturated_seed.is_saturated = true;
    return !scalar_seed_dimension_accepted(saturated_seed, false);
}());

static_assert([] {
    minimap2_parameters params{};
    if (!cpu_seed_lookup_selected(params)) return false;
    params.vulkan_enabled = false;
    params.vulkan_seed_hits_enabled = true;
    if (!cpu_seed_lookup_selected(params)) return false;
    params.vulkan_enabled = true;
    params.vulkan_seed_hits_enabled = true;
    return !cpu_seed_lookup_selected(params);
}());

/// Fail-closed stats policy for one completed pair. The minimap2 reference is
/// intentionally outside Bioseq's stats ABI and therefore must emit no Bioseq
/// algorithm stats. Every Bioseq row is validated against the selectors that
/// fixed its template leaf. The strict XXL gate additionally requires positive
/// non-empty vector work; ordinary benchmark rows still reject missing,
/// malformed, forbidden, or unexpected stats without requiring work from an
/// empty input. Routes that did not select an algorithm must not emit its stats.
[[nodiscard]] bool require_selected_route_execution(
        std::string_view algo,
        const algorithm_dispatch& dispatch,
        const match_result_collection& results,
        bool strict_xxl_gate) {
    if (dispatch.is_minimap2_reference) {
        if (dispatch.params.minimizer_algorithm != minimizer_algorithm::scalar
            || dispatch.params.base_alignment_algorithm
                != base_alignment_algorithm::ksw2
            || results.has_any_algorithm_execution_stats()) {
            throw std::runtime_error(fmt::format(
                "minimap2 reference row '{}' violated scalar/KSW2/no-Bioseq-stats policy",
                algo));
        }
        return true;
    }

    bool all_dimensions_validated = true;

    const auto& scalar_stats = results.scalar_pipeline_stats;
    if (scalar_stats.is_saturated) {
        throw std::runtime_error(fmt::format(
            "Bioseq row '{}' emitted saturated scalar execution stats",
            algo));
    }

    const bool scalar_sketch_selected =
        dispatch.params.minimizer_algorithm
            == minimizer_algorithm::scalar;
    if (scalar_sketch_selected) {
        if (!results.is_selected_scalar_sketch_execution_consistent(
                /*require_target_sketch=*/true,
                /*require_nonempty_work=*/strict_xxl_gate)) {
            throw std::runtime_error(fmt::format(
                "selected scalar sketch route '{}' lacks exact positive "
                "query/target execution stats or emitted contradictory sketch stats",
                algo));
        }
    } else if (!scalar_minimizer_dimensions_absent(scalar_stats)) {
        throw std::runtime_error(fmt::format(
            "non-scalar sketch route '{}' emitted scalar minimizer stats",
            algo));
    }

    const bool exact_simd_sketch_selected =
        dispatch.params.minimizer_algorithm
            == minimizer_algorithm::two_stack_nthash;
    if (exact_simd_sketch_selected) {
        if (!results.is_exact_simd_query_execution_consistent(
                /*require_nonempty_vector_work=*/strict_xxl_gate)
            || !results.is_exact_simd_target_execution_consistent(
                /*require_nonempty_vector_work=*/strict_xxl_gate)) {
            throw std::runtime_error(
                "selected ExactSimd sketch route lacks accepted query/target execution stats");
        }
        if (strict_xxl_gate
            && (!exact_resident_dual_vector_stream_accepted(
                    results.exact_simd_query_execution_stats)
                || !exact_fast_execution_accepted(
                    results.exact_simd_target_execution_stats))) {
            const auto& query = results.exact_simd_query_execution_stats;
            const auto& target = results.exact_simd_target_execution_stats;
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
                query.kmer_15_window_10_byte_8_uint32_hash_kernel_calls,
                query.kmer_15_window_10_byte_8_uint32_hash_candidate_blocks,
                query.kmer_15_window_10_byte_8_uint32_hash_lane_visits,
                query.kmer_15_window_10_byte_8_packed_22_groups,
                query.kmer_15_window_10_byte_8_packed_22_lane_visits,
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
                target.kmer_15_window_10_byte_8_uint32_hash_kernel_calls,
                target.kmer_15_window_10_byte_8_uint32_hash_candidate_blocks,
                target.kmer_15_window_10_byte_8_uint32_hash_lane_visits,
                target.kmer_15_window_10_byte_8_packed_22_groups,
                target.kmer_15_window_10_byte_8_packed_22_lane_visits,
                target.vector_batches,
                target.vector_lanes_visited));
        }
    } else if (results.exact_simd_query_execution_stats_available
               || results.exact_simd_target_execution_stats_available) {
        throw std::runtime_error(
            "non-ExactSimd sketch route emitted ExactSimd stats");
    }

    const bool bounded_nthash_sketch_selected =
        dispatch.params.minimizer_algorithm
            == minimizer_algorithm::bounded_nthash_simd;
    if (bounded_nthash_sketch_selected) {
        if (!results.is_bounded_nthash_query_execution_consistent(
                /*require_nonempty_vector_work=*/strict_xxl_gate)
            || !results.is_bounded_nthash_target_execution_consistent(
                /*require_nonempty_vector_work=*/strict_xxl_gate)) {
            throw std::runtime_error(
                "selected BoundedNtHashSimd route lacks accepted query/target execution stats");
        }
        if (strict_xxl_gate
            && results.bounded_nthash_target_execution_stats.segments_combined < 2) {
            throw std::runtime_error(
                "strict XXL BoundedNtHashSimd route did not execute a segmented target sketch");
        }
    } else if (results.bounded_nthash_query_execution_stats_available
               || results.bounded_nthash_target_execution_stats_available) {
        throw std::runtime_error(
            "non-BoundedNtHashSimd route emitted bounded ntHash stats");
    }

    if (dispatch.params.minimizer_algorithm
            == minimizer_algorithm::simd_minimap2_reference) {
        all_dimensions_validated = false;
        static std::atomic_bool logged_unvalidated_slow_simd{false};
        if (!logged_unvalidated_slow_simd.exchange(
                true, std::memory_order_relaxed)) {
            bioseq_log<logging_level::warning>(
                "[bioseq_benchmark] route '{}' executed SIMD_MINIMAP2_SLOW, which "
                "has no operation-originated sketch stats ABI; timing and "
                "correctness remain available but "
                "selected_route_execution_stats_consistent is false",
                algo);
        }
    }

    const bool simd_chaining_selected =
        dispatch.params.chaining_algorithm == chaining_algorithm::simd_minimap2;
    if (simd_chaining_selected) {
        if (!results.is_simd_chaining_execution_consistent(
                dispatch.params.simd_chaining_policy,
                /*require_nonempty_vector_work=*/strict_xxl_gate)) {
            throw std::runtime_error(fmt::format(
                "selected SIMD chaining policy '{}' lacks accepted, mutually "
                "exclusive score/backtrack stats",
                bioseq::chaining::simd_chaining_policy_name(
                    dispatch.params.simd_chaining_policy)));
        }
    } else if (results.simd_chaining_execution_stats_available) {
        throw std::runtime_error(
            "non-SIMD chaining route emitted SIMD chaining stats");
    }

    if (dispatch.params.chaining_algorithm == chaining_algorithm::range_minimum_query) {
        all_dimensions_validated = false;
        static std::atomic_bool logged_unvalidated_rmq{false};
        if (!logged_unvalidated_rmq.exchange(
                true, std::memory_order_relaxed)) {
            bioseq_log<logging_level::warning>(
                "[bioseq_benchmark] route '{}' executed RMQ chaining, which has no "
                "operation-originated chaining stats ABI; timing and "
                "correctness remain available but "
                "selected_route_execution_stats_consistent is false",
                algo);
        }
    }

    const bool cpu_seed_selected =
        cpu_seed_lookup_selected(dispatch.params);
    if (cpu_seed_selected) {
        if (!results.scalar_pipeline_execution_stats_available
            || !scalar_seed_dimension_accepted(
                scalar_stats, strict_xxl_gate)) {
            throw std::runtime_error(fmt::format(
                "selected CPU seed route '{}' lacks positive scalar "
                "seed-lookup operation stats",
                algo));
        }
    } else if (!scalar_seed_dimension_absent(scalar_stats)) {
        throw std::runtime_error(fmt::format(
            "selected Vulkan seed route '{}' emitted scalar CPU seed-lookup stats",
            algo));
    }

    const bool scalar_chaining_selected =
        dispatch.params.chaining_algorithm
            == chaining_algorithm::scalar_minimap2;
    if (scalar_chaining_selected) {
        if (!results.scalar_pipeline_execution_stats_available
            || !scalar_chain_dimension_accepted(
                scalar_stats, strict_xxl_gate)) {
            throw std::runtime_error(fmt::format(
                "selected scalar chaining route '{}' lacks paired "
                "score/backtrack stats",
                algo));
        }
    } else if (!scalar_chain_dimension_absent(scalar_stats)) {
        throw std::runtime_error(fmt::format(
            "non-scalar chaining route '{}' emitted scalar chaining stats",
            algo));
    }

    const bool ksw2_selected =
        dispatch.params.base_alignment_algorithm
            == base_alignment_algorithm::ksw2;
    if (ksw2_selected) {
        if (!results.scalar_pipeline_execution_stats_available
            || !scalar_ksw2_dimension_accepted(
                scalar_stats, strict_xxl_gate)) {
            throw std::runtime_error(fmt::format(
                "selected KSW2 route '{}' lacks positive KSW2 call/input stats",
                algo));
        }
    } else if (!scalar_ksw2_dimension_absent(scalar_stats)) {
        throw std::runtime_error(fmt::format(
            "non-KSW2 route '{}' emitted KSW2 execution stats",
            algo));
    }

    const bool ksw2_batched_selected =
        dispatch.params.base_alignment_algorithm
            == base_alignment_algorithm::ksw2_batched;
    if (ksw2_batched_selected) {
        if (!results.is_ksw2_batched_execution_consistent(
                /*require_nonempty_work=*/strict_xxl_gate)) {
            const auto& stats = results.ksw2_batched_stats;
            throw std::runtime_error(fmt::format(
                "selected KSW2_BATCHED route '{}' lacks operation stats: "
                "available={} invocations={} regions={} fills={} calls={} "
                "short_dispatches={} short_lanes={} long_calls={} "
                "input_area={} saturated={}",
                algo,
                results.ksw2_batched_execution_stats_available,
                stats.alignment_region_invocations,
                stats.regions_seen,
                stats.fills_collected,
                stats.nonempty_fill_calls,
                stats.short_bucket_dispatches,
                stats.short_bucket_lanes,
                stats.long_fill_calls,
                stats.input_cell_area,
                stats.saturated));
        }
    } else if (results.ksw2_batched_execution_stats_available
               || results.ksw2_batched_stats
                    != ksw2_batched_execution_stats{}) {
        throw std::runtime_error(fmt::format(
            "non-KSW2_BATCHED route '{}' emitted batched execution stats",
            algo));
    }

    const bool block_aligner_selected =
        dispatch.params.base_alignment_algorithm
            == base_alignment_algorithm::block_aligner;
    if (block_aligner_selected) {
        if (!dispatch.params.do_cigar) {
            if (strict_xxl_gate) {
                throw std::runtime_error(fmt::format(
                    "strict XXL Block Aligner row '{}' must request CIGAR execution",
                    algo));
            }
            if (results.block_aligner_execution_stats_available) {
                throw std::runtime_error(fmt::format(
                    "Block Aligner row '{}' emitted stats although CIGAR was disabled",
                    algo));
            }
        } else if (!results.is_block_aligner_execution_consistent(
                       /*require_nonempty_simd_work=*/strict_xxl_gate)) {
            const auto& stats = results.block_aligner_stats;
            throw std::runtime_error(fmt::format(
                "selected Block Aligner lacks accepted SIMD stats: "
                "algo='{}' cigar={} available={} consistent={} executed={} "
                "nonempty={} vector_batches={} cells={} ksw2={} scalar_fallbacks={}",
                algo, dispatch.params.do_cigar,
                results.block_aligner_execution_stats_available,
                stats.is_internally_consistent(), stats.executed_calls(),
                stats.nonempty_calls,
                stats.simd_dp_vector_batches, stats.simd_dp_cells,
                stats.forbidden_ksw2_calls,
                stats.forbidden_scalar_biological_fallbacks));
        }
    } else if (results.block_aligner_execution_stats_available) {
        throw std::runtime_error(fmt::format(
            "non-Block row '{}' emitted Block Aligner execution stats", algo));
    }

    const bool any_scalar_dimension_selected = scalar_sketch_selected
        || cpu_seed_selected || scalar_chaining_selected || ksw2_selected;
    if (!any_scalar_dimension_selected
        && (results.scalar_pipeline_execution_stats_available
            || scalar_stats != scalar_pipeline_execution_stats{})) {
        throw std::runtime_error(fmt::format(
            "route '{}' selected no scalar biological stage but emitted "
            "scalar pipeline stats",
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
        } else if (algo == "bioseq_block_vulkan_resident_gpu_index_frontend") {
            expect_query_sketch = true;
            expect_target_sketch = true;
            expect_target_index_build = true;
            expect_seed_lookup = true;
        } else {
            throw std::logic_error(fmt::format(
                "explicit Vulkan row '{}' has no execution-stats stage contract", algo));
        }
        const auto& partial_vulkan_stats =
            results.partial_vulkan_stats;
        const bool is_exact_stage_mask_consistent =
            expect_source_direct_target_index
            ? partial_vulkan_stats.is_exact_source_direct_stage_mask_consistent(
                expect_query_sketch,
                expect_target_sketch,
                expect_seed_lookup,
                expect_chain_score)
            : partial_vulkan_stats.is_exact_stage_mask_consistent(
                expect_query_sketch,
                expect_target_sketch,
                expect_target_index_build,
                expect_seed_lookup,
                expect_chain_score);
        if (strict_xxl_gate && !is_exact_stage_mask_consistent) {
            const auto& stats = results.partial_vulkan_stats;
            throw std::runtime_error(fmt::format(
                "strict XXL Vulkan row '{}' lacks exact positive dispatch validation: "
                "query_sketch={} target_sketch={} target_index_build={} "
                "source_direct_target_index={} seed_lookup={} chain_score={} "
                "saturated={}",
                algo,
                stats.query_sketch_dispatches,
                stats.target_sketch_dispatches,
                stats.target_index_build_dispatches,
                expect_source_direct_target_index,
                stats.seed_lookup_dispatches,
                stats.chain_score_dispatches,
                stats.is_saturated));
        }
        if (strict_xxl_gate
            && algo == "bioseq_block_vulkan_dp_vulkan_minimap_int") {
            const auto& stats = results.partial_vulkan_stats;
            const bool materialization_stats_consistent =
                is_source_direct_gpu_index_materialization_consistent(stats);
            if (!materialization_stats_consistent) {
                throw std::runtime_error(fmt::format(
                    "strict XXL direct-resident Vulkan row lacks exact "
                    "source-direct GPU-index validation: operations={} empty={} "
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
                    stats.gpu_index_source_direct_operations,
                    stats.gpu_index_source_direct_empty_bypasses,
                    stats.gpu_index_source_direct_planning_receipts,
                    stats.gpu_index_source_direct_terminal_receipts,
                    stats.gpu_index_source_direct_terminal_successes,
                    stats.gpu_index_source_direct_zero_query_bypasses,
                    stats.gpu_index_source_direct_materializations,
                    stats.gpu_index_source_direct_resident_publications,
                    stats.gpu_index_source_direct_source_releases,
                    stats.gpu_index_source_direct_source_released_bytes,
                    stats.gpu_index_source_direct_checksum_matches,
                    stats.gpu_index_source_direct_source_traversals,
                    stats.gpu_index_source_direct_mapped_traversals,
                    stats.gpu_index_source_direct_reflected_authorities,
                    stats.gpu_index_source_direct_retained_authorities,
                    stats.gpu_index_source_direct_retained_resident_permit_bytes,
                    stats.gpu_index_source_direct_cache_preinsertions,
                    stats.gpu_index_source_direct_expected_seed_consumers,
                    stats.gpu_index_source_direct_completed_seed_consumers,
                    stats.gpu_index_source_direct_required_lookups,
                    stats.gpu_index_source_direct_required_hits,
                    stats.gpu_index_source_direct_required_misses,
                    stats.gpu_index_source_direct_fallback_attempts,
                    stats.gpu_index_source_direct_mapped_prefix_passes,
                    stats.gpu_index_source_direct_mapped_prefix_lookups,
                    stats.gpu_index_source_direct_mapped_prefix_comparisons,
                    stats.gpu_index_source_direct_seed_slab_requested_bytes,
                    stats.gpu_index_source_direct_seed_slab_initial_capacity,
                    stats.gpu_index_source_direct_seed_slab_actual_bytes,
                    stats.gpu_index_source_direct_seed_slab_retained_bytes,
                    stats.gpu_index_source_direct_seed_slab_permit_releases,
                    stats.gpu_index_source_direct_seed_roundtrips,
                    stats.gpu_index_source_direct_zero_anchor_bypasses,
                    stats.gpu_index_source_direct_seed_fence_completions,
                    stats.gpu_index_source_direct_cache_cleanup_releases,
                    stats.gpu_index_source_direct_resident_cleanup_bytes,
                    stats.gpu_index_source_direct_pipeline_completions,
                    stats.gpu_index_retained_minimizer_count,
                    stats.gpu_index_distinct_minimizer_count,
                    stats.gpu_index_actual_allocation_bytes,
                    stats.seed_lookup_dispatches,
                    stats.seed_sorted_dense_lookup_dispatches,
                    stats.seed_host_sort_fallbacks,
                    stats.post_seed_owned_cpu_index_releases,
                    stats.post_seed_owned_cpu_position_bytes_released,
                    stats.is_saturated));
            }
        }
        if (strict_xxl_gate
            && algo == "bioseq_block_vulkan_resident_gpu_index_frontend") {
            const auto& stats = results.partial_vulkan_stats;
            if (!gpu_index_ownership_stats_consistent(stats)) {
                throw std::runtime_error(fmt::format(
                    "strict XXL resident GPU-index frontend row lacks exact GPU target-index "
                    "ownership/gated-build validation: target_sketches={} target_builds={} "
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
                    stats.target_sketch_dispatches,
                    stats.target_index_build_dispatches,
                    stats.seed_lookup_dispatches,
                    stats.seed_open_addressed_lookup_dispatches,
                    stats.seed_sorted_dense_lookup_dispatches,
                    stats.gpu_index_count_admissions,
                    stats.gpu_index_main_peak_admissions,
                    stats.gpu_index_resident_transfers,
                    stats.gpu_index_adoption_rereserve_attempts,
                    stats.gpu_index_adoption_successes,
                    stats.gpu_index_owned_resident_bytes,
                    stats.gpu_index_gate_enabled,
                    stats.gpu_index_gate_word_count,
                    stats.gpu_index_gate_mask,
                    stats.gpu_index_gate_set_bits,
                    stats.gpu_index_gate_count_dispatches,
                    stats.gpu_index_gated_emit_dispatches,
                    stats.gpu_index_ungated_emit_dispatches,
                    stats.gpu_index_exact_count_dispatches,
                    stats.gpu_index_exact_count_prefix_scan_dispatches,
                    stats.gpu_index_exact_count_control_readbacks,
                    stats.gpu_index_deterministic_tile_emit_dispatches,
                    stats.gpu_index_atomic_append_dispatches,
                    stats.gpu_index_emit_validation_control_readbacks,
                    stats.gpu_index_counted_minimizer_count,
                    stats.gpu_index_verified_emit_count,
                    stats.gpu_index_count_emit_parity_verified,
                    stats.gpu_index_target_minimizer_capacity_estimate,
                    stats.gpu_index_capacity_estimate_underestimated,
                    stats.gpu_index_arena_trimmed_before_permit_shrink,
                    stats.gpu_index_retained_minimizer_count,
                    stats.gpu_index_distinct_minimizer_count,
                    stats.post_seed_owned_gpu_cache_evictions,
                    stats.post_seed_owned_gpu_resident_bytes_released,
                    stats.post_seed_owned_cpu_index_releases,
                    stats.post_seed_owned_cpu_position_bytes_released,
                    stats.is_saturated));
            }
        }
        if (strict_xxl_gate
            && (algo == "bioseq_block_vulkan_dp_vulkan_minimap_int"
                || algo == "bioseq_block_vulkan_resident_gpu_index_frontend")) {
            const auto& stats = results.partial_vulkan_stats;
            if (!is_seed_radix_sort_stats_consistent(stats)) {
                throw std::runtime_error(fmt::format(
                    "strict XXL Vulkan row '{}' lacks scalable GPU seed-sort validation: "
                    "seed_dispatches={} bitonic={} radix_ops={} radix_anchors={} "
                    "radix_passes={} histogram={} scan={} scatter={} host_sort={}",
                    algo,
                    stats.seed_lookup_dispatches,
                    stats.seed_bitonic_sort_dispatches,
                    stats.seed_radix_sort_operations,
                    stats.seed_radix_anchor_count,
                    stats.seed_radix_passes,
                    stats.seed_radix_histogram_dispatches,
                    stats.seed_radix_scan_dispatches,
                    stats.seed_radix_scatter_dispatches,
                    stats.seed_host_sort_fallbacks));
            }
        }
    } else if (results.partial_vulkan_stats.has_any_dispatch()) {
        throw std::runtime_error(fmt::format(
            "non-Vulkan row '{}' emitted partial Vulkan execution stats", algo));
    }
    if (strict_xxl_gate && !all_dimensions_validated) {
        throw std::runtime_error(fmt::format(
            "strict XXL route '{}' selected an algorithm dimension without "
            "operation-originated execution stats",
            algo));
    }
    return all_dimensions_validated;
}

/// Hand the (query, target) pair off to either the C reference or the
/// bioseq port, then aggregate the per-pair primary-correctness flags
/// into the aligner_output.
[[nodiscard]] aligner_output run_aligner_impl(
                                          std::string_view algo,
                                          const parameter_map& params,
                                          const aligner_input& input,
                                          bool strict_xxl_gate,
                                          bool capture_full_route_stats) {
    const std::uint32_t edit_threshold = read_max_edits_param(params);
    const algorithm_dispatch dispatch = dispatch_algorithm(
        algo, input.kmer_size_override, edit_threshold);

    aligner_output output;
    output.total_pairs = input.pairs.size();
    output.correct_pair_flags.assign(input.pairs.size(), 0u);
    output.mixed_gpu_pipeline_marker =
        dispatch.mixed_gpu_pipeline_marker;
    output.all_gpu_acceptance_eligible = false;
    output.edit_threshold_enforced = dispatch.enforce_edit_threshold;
    output.edit_threshold = dispatch.params.prefilter_max_edits;
    output.prefilter_enabled = dispatch.params.prefilter_enabled;
    output.selected_prefilter_backend = dispatch.params.prefilter_backend;
    output.simd_chaining_policy_selected =
        dispatch.params.chaining_algorithm == chaining_algorithm::simd_minimap2;
    output.selected_simd_chaining_policy =
        dispatch.params.simd_chaining_policy;
    if (dispatch.explicit_vulkan != dispatch.mixed_gpu_pipeline_marker) {
        throw std::logic_error(fmt::format(
            "Vulkan row '{}' lacks explicit mixed CPU/GPU classification",
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
    bool every_pair_route_validated = !input.pairs.empty();
    const bool source_direct_selected =
        algo == "bioseq_block_vulkan_dp_vulkan_minimap_int";
    bool every_source_direct_pair_validated =
        source_direct_selected && !input.pairs.empty();
    for (std::size_t pair_index = 0; pair_index < input.pairs.size(); ++pair_index) {
        auto& pair = input.pairs[pair_index];     // pairs are `mutable` in aligner_input; see field comment
        match_result_collection results;
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
                // Keep the in-memory owners alive while the canonical lazy facade
                // views are consumed by the minimap entry points.
                auto bench_lazy_query =
                    bioseq::storage::make_resident_lazy_sequence(pair.query);
                auto bench_lazy_target =
                    bioseq::storage::make_resident_lazy_sequence(*pair.target);
                const auto bench_query_view =
                    bioseq::storage::make_lazy_sequence_view(bench_lazy_query);
                const auto bench_target_view =
                    bioseq::storage::make_lazy_sequence_view(bench_lazy_target);
                if (dispatch.params.minimizer_algorithm
                    == minimizer_algorithm::bounded_nthash_simd) {
                    // bounded ntHash uses a 32-bit hash plus segment-local position
                    // representation.  XXL targets must therefore traverse the
                    // production segmented entry point; the unsegmented port can
                    // select a query-sized position layout and correctly rejects
                    // the 500-Mbp target rather than truncating coordinates.
                    results = bioseq::simd::minimap_port_segmented(
                        bench_query_view, bench_target_view, dispatch.params);
                } else {
                    results = bioseq::simd::minimap_port(
                        bench_query_view, bench_target_view, dispatch.params);
                }
            }
        } catch (const bioseq::gpu::vulkan::vulkan_runtime_error& exception) {
            bioseq_log<logging_level::error>("[bioseq_benchmark] selected Vulkan aligner failed (algo='{}', pair_index={}, "
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
            every_pair_route_validated = false;
            continue;
        } catch (const std::exception& exception) {
            bioseq_log<logging_level::error>("[bioseq_benchmark] aligner threw exception (algo='{}', pair_index={}, "
                 "query_len={}, target_len={}, k={}): {}",
                 algo, pair_index,
                 input.queries_acgt[pair_index].size(),
                 input.targets_acgt[pair_index].size(),
                 dispatch.params.kmer_size,
                 exception.what());
            if (dispatch.enforce_edit_threshold
                || dispatch.explicit_prefilter_vulkan
                || strict_xxl_gate) throw;
            every_pair_route_validated = false;
            continue;
        } catch (...) {
            bioseq_log<logging_level::error>("[bioseq_benchmark] aligner threw non-std exception (algo='{}', pair_index={})",
                 algo, pair_index);
            if (dispatch.enforce_edit_threshold
                || dispatch.explicit_prefilter_vulkan
                || strict_xxl_gate) throw;
            every_pair_route_validated = false;
            continue;
        }
        const bool pair_route_validated = require_selected_route_execution(
            algo, dispatch, results, strict_xxl_gate);
        if (!pair_route_validated) {
            every_pair_route_validated = false;
        }
        if (source_direct_selected
            && !is_source_direct_gpu_index_materialization_consistent(
                results.partial_vulkan_stats)) {
            every_source_direct_pair_validated = false;
        }
        if (results.bounded_nthash_query_execution_stats_available) {
            output.bounded_nthash_query_execution_stats.merge(
                results.bounded_nthash_query_execution_stats);
            output.bounded_nthash_query_execution_stats_available = true;
        }
        if (results.bounded_nthash_target_execution_stats_available) {
            output.bounded_nthash_target_execution_stats.merge(
                results.bounded_nthash_target_execution_stats);
            output.bounded_nthash_target_execution_stats_available = true;
        }
        if (results.exact_simd_query_execution_stats_available) {
            output.exact_simd_query_execution_stats.merge(
                results.exact_simd_query_execution_stats);
            output.exact_simd_query_execution_stats_available = true;
        }
        if (results.exact_simd_target_execution_stats_available) {
            output.exact_simd_target_execution_stats.merge(
                results.exact_simd_target_execution_stats);
            output.exact_simd_target_execution_stats_available = true;
        }
        if (results.scalar_pipeline_execution_stats_available) {
            output.scalar_pipeline_stats.merge(
                results.scalar_pipeline_stats);
            output.scalar_pipeline_execution_stats_available = true;
        }
        if (results.ksw2_batched_execution_stats_available) {
            output.ksw2_batched_stats.merge(
                results.ksw2_batched_stats);
            output.ksw2_batched_execution_stats_available = true;
        }
        if (results.prefilter_execution_stats_available) {
            output.prefilter_stats.merge(
                results.prefilter_stats);
            output.prefilter_execution_stats_available = true;
        }
        if (capture_full_route_stats) {
            if (results.simd_chaining_execution_stats_available) {
                output.simd_chaining_stats.merge(
                    results.simd_chaining_stats);
                output.simd_chaining_execution_stats_available = true;
            }
            if (results.block_aligner_execution_stats_available) {
                output.block_aligner_stats.merge(
                    results.block_aligner_stats);
                output.block_aligner_execution_stats_available = true;
            }
        }
        output.partial_vulkan_stats.merge(
            results.partial_vulkan_stats);
        output.biological_fingerprint.merge(
            results.biological_identity());
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
            if (abs_offset < k_position_tolerance &&
                match.is_reverse == pair.is_reverse) {
                output.correct_pair_flags[pair_index] = 1u;
                output.correct_pairs += 1;
            }
        }
    }
    if (strict_xxl_gate
        && (!output.biological_fingerprint.is_computed
            || output.biological_fingerprint.is_saturated
            || output.biological_fingerprint.collection_count
                != output.total_pairs)) {
        throw std::runtime_error(fmt::format(
            "strict XXL biological fingerprint rejected algo='{}': "
            "computed={} saturated={} collections={} pairs={}",
            algo,
            output.biological_fingerprint.is_computed,
            output.biological_fingerprint.is_saturated,
            output.biological_fingerprint.collection_count,
            output.total_pairs));
    }
    output.gpu_index_ownership_stats_consistent =
        algo == "bioseq_block_vulkan_resident_gpu_index_frontend"
        && gpu_index_ownership_stats_consistent(
            output.partial_vulkan_stats);
    output.is_source_direct_gpu_index_materialization_consistent =
        source_direct_selected
        && every_pair_route_validated
        && every_source_direct_pair_validated;
    output.selected_route_execution_stats_consistent =
        every_pair_route_validated;
    if (dispatch.enforce_edit_threshold && !every_pair_route_validated) {
        throw std::runtime_error(fmt::format(
            "exact-threshold benchmark row '{}' did not complete and validate "
            "every input pair",
            algo));
    }
    const bool prefilter_stats_are_zero =
        output.prefilter_stats
            == prefilter_execution_stats{};
    if (!dispatch.params.prefilter_enabled) {
        if (output.prefilter_execution_stats_available
            || !prefilter_stats_are_zero) {
            throw std::runtime_error(fmt::format(
                "prefilter-off row '{}' emitted SneakySnake execution stats",
                algo));
        }
    } else {
        const bool require_evaluated_vulkan_jobs =
            dispatch.params.prefilter_backend == prefilter_backend::vulkan
            && dispatch.params.prefilter_max_edits <= 31u;
        output.prefilter_execution_stats_consistent =
            output.prefilter_execution_stats_available
            && output.prefilter_stats.is_backend_consistent(
                dispatch.params.prefilter_backend,
                require_evaluated_vulkan_jobs,
                require_evaluated_vulkan_jobs);
        if (!output.prefilter_execution_stats_consistent) {
            const auto& stats =
                output.prefilter_stats;
            throw std::runtime_error(fmt::format(
                "prefilter row '{}' lacks accepted operation stats: "
                "present={} backend={} dispatches={} scalar={} simd={} "
                "vulkan={} jobs={} evaluated={} length_only={} "
                "unsupported={} rejected_jobs={} rejected_regions={} "
                "survivors={} saturated={}",
                algo,
                output.prefilter_execution_stats_available,
                static_cast<unsigned>(dispatch.params.prefilter_backend),
                stats.dispatches,
                stats.scalar_dispatches,
                stats.simd_dispatches,
                stats.vulkan_dispatches,
                stats.jobs,
                stats.evaluated_jobs,
                stats.length_difference_jobs,
                stats.unsupported_jobs,
                stats.rejected_jobs,
                stats.rejected_regions,
                stats.surviving_regions,
                stats.is_saturated));
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

[[nodiscard]] aligner_output run_aligner(
        std::string_view algo,
        const parameter_map& params,
        const aligner_input& input) {
    return run_aligner_impl(algo, params, input, false, false);
}

[[nodiscard]] aligner_output run_aligner_strict_xxl(
        std::string_view algo,
        const parameter_map& params,
        const aligner_input& input) {
    // The strict relative gate reports operation-originated route stats,
    // not only the high-level dispatch decision.  Keep the full stats so its
    // evaluator can fail closed on scalar chaining/backtracking fallbacks.
    return run_aligner_impl(algo, params, input, true, true);
}

[[nodiscard]] aligner_output run_aligner_strict_xxl_paired(
        std::string_view algo,
        const parameter_map& params,
        const aligner_input& input) {
    return run_aligner_impl(algo, params, input, true, true);
}

[[nodiscard]] constexpr std::uint64_t unsigned_abs_diff(
        std::uint64_t lhs, std::uint64_t rhs) noexcept {
    return lhs >= rhs ? lhs - rhs : rhs - lhs;
}

[[nodiscard]] constexpr std::uint64_t endpoint_delta_bp(
        const match_result& lhs, const match_result& rhs) noexcept {
    return std::max({
        unsigned_abs_diff(lhs.query_start, rhs.query_start),
        unsigned_abs_diff(lhs.query_end, rhs.query_end),
        unsigned_abs_diff(lhs.target_start, rhs.target_start),
        unsigned_abs_diff(lhs.target_end, rhs.target_end),
    });
}

[[nodiscard]] constexpr bool normalized_endpoint_coordinates(
        const match_result& match,
        std::uint64_t query_length,
        std::uint64_t target_length) noexcept {
    return match.query_len == query_length
        && match.target_len == target_length
        && match.query_start <= match.query_end
        && match.query_end <= query_length
        && match.target_start <= match.target_end
        && match.target_end <= target_length;
}

/** @brief Unique primary mapping extracted from a result collection. */
struct unique_primary_view {
    const match_result* primary = nullptr;
    std::uint64_t count = 0;
};

[[nodiscard]] unique_primary_view unique_primary(
        const match_result_collection& results) noexcept {
    unique_primary_view selected;
    for (const auto& match : results.view()) {
        if (!match.is_primary) continue;
        ++selected.count;
        if (selected.count == 1) selected.primary = &match;
    }
    return selected;
}

[[nodiscard]] endpoint_coordinates_stats endpoint_stats(
        const unique_primary_view& primary) noexcept {
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

[[nodiscard]] endpoint_backend_outcome run_endpoint_bioseq_policy(
        endpoint_qualification_fixture& fixture,
        simd_chaining_policy policy,
        bool& route_stats_consistent) {
    const std::string_view algorithm =
        policy == simd_chaining_policy::exact_visited
            ? "bioseq_ksw2_simd_dp_two_stack_nthash_exact_visited"
            : "bioseq_ksw2_simd_dp_two_stack_nthash_bounded_batch_approximate";
    algorithm_dispatch dispatch = dispatch_algorithm(algorithm, 0);
    dispatch.params.chain_skip_scale = fixture.chain_skip_scale;
    dispatch.params.max_chain_skip = fixture.max_chain_skip;
    auto lazy_query =
        bioseq::storage::make_resident_lazy_sequence(fixture.pair.query);
    auto lazy_target =
        bioseq::storage::make_resident_lazy_sequence(*fixture.pair.target);
    const auto query_view = bioseq::storage::make_lazy_sequence_view(lazy_query);
    const auto target_view = bioseq::storage::make_lazy_sequence_view(lazy_target);
    match_result_collection results =
        bioseq::simd::minimap_port(
            query_view, target_view, dispatch.params);
    try {
        route_stats_consistent = require_selected_route_execution(
            algorithm, dispatch, results, /*strict_xxl_gate=*/false);
    } catch (const std::exception&) {
        // Preserve the completed operation and its stats in endpoint raw
        // stats. Ordinary qualification rules reject this false value.
        route_stats_consistent = false;
    }
    return {
        .matches = std::move(results),
        .target_ordinal = fixture.target_ordinal,
    };
}

/// One callback deliberately owns all three executions for every fixture.
/// Independent matrix rows cannot safely exchange four endpoints, and globals
/// would make pairing depend on scheduler order.
[[nodiscard]] simd_chaining_endpoint_qualification
run_simd_chaining_endpoint_qualification(
    std::string_view algorithm,
    const parameter_map& /*params*/,
    const endpoint_qualification_input& input)
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

    simd_chaining_endpoint_qualification qualification;
    qualification.comparisons.reserve(input.fixtures.size());
    qualification.exact_route_stats_consistent = !input.fixtures.empty();
    qualification.bounded_route_stats_consistent = !input.fixtures.empty();
    qualification.minimap2_route_stats_consistent = !input.fixtures.empty();

    for (auto& fixture : input.fixtures) {
        // Fixed order is part of the qualification contract.
        bool bounded_fixture_route_accepted = false;
        endpoint_backend_outcome bounded = run_endpoint_bioseq_policy(
            fixture, simd_chaining_policy::bounded_batch_approximate,
            bounded_fixture_route_accepted);
        bool exact_fixture_route_accepted = false;
        endpoint_backend_outcome exact = run_endpoint_bioseq_policy(
            fixture, simd_chaining_policy::exact_visited,
            exact_fixture_route_accepted);

        qualification.bounded_route_stats_consistent =
            qualification.bounded_route_stats_consistent
            && bounded_fixture_route_accepted
            && bounded.matches.is_exact_simd_query_execution_consistent(true)
            && bounded.matches.is_exact_simd_target_execution_consistent(true)
            && bounded.matches.is_simd_chaining_execution_consistent(
                simd_chaining_policy::bounded_batch_approximate, true);
        qualification.exact_route_stats_consistent =
            qualification.exact_route_stats_consistent
            && exact_fixture_route_accepted
            && exact.matches.is_exact_simd_query_execution_consistent(true)
            && exact.matches.is_exact_simd_target_execution_consistent(true)
            && exact.matches.is_simd_chaining_execution_consistent(
                simd_chaining_policy::exact_visited, true);

        algorithm_dispatch minimap_dispatch = dispatch_algorithm("minimap2_dp", 0);
        minimap_dispatch.params.chain_skip_scale = fixture.chain_skip_scale;
        minimap_dispatch.params.max_chain_skip = fixture.max_chain_skip;
        endpoint_backend_outcome minimap{
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
        qualification.minimap2_route_stats_consistent =
            qualification.minimap2_route_stats_consistent
            && minimap_fixture_route_accepted;

        qualification.exact_policy_stats.merge(
            exact.matches.simd_chaining_stats);
        qualification.bounded_policy_stats.merge(
            bounded.matches.simd_chaining_stats);
        qualification.exact_policy_stats_available =
            qualification.exact_policy_stats_available
            || exact.matches.simd_chaining_execution_stats_available;
        qualification.bounded_policy_stats_available =
            qualification.bounded_policy_stats_available
            || bounded.matches.simd_chaining_execution_stats_available;

        endpoint_fixture_comparison comparison;
        comparison.fixture_name = fixture.name;
        comparison.target_ordinal = fixture.target_ordinal;
        const unique_primary_view bounded_primary = unique_primary(bounded.matches);
        const unique_primary_view exact_primary = unique_primary(exact.matches);
        const unique_primary_view minimap_primary = unique_primary(minimap.matches);
        comparison.bounded_primary_count = bounded_primary.count;
        comparison.exact_primary_count = exact_primary.count;
        comparison.minimap2_primary_count = minimap_primary.count;
        comparison.bounded_target_ordinal = bounded.target_ordinal;
        comparison.exact_target_ordinal = exact.target_ordinal;
        comparison.minimap2_target_ordinal = minimap.target_ordinal;
        comparison.bounded_endpoint = endpoint_stats(bounded_primary);
        comparison.exact_endpoint = endpoint_stats(exact_primary);
        comparison.minimap2_endpoint = endpoint_stats(minimap_primary);
        const auto& fixture_stats =
            bounded.matches.simd_chaining_stats;
        comparison.bounded_cutoff_stops =
            fixture_stats.policy.bounded_cutoff_stops;
        comparison.bounded_candidates_skipped =
            fixture_stats.policy.bounded_candidates_skipped;
        comparison.bounded_rescue_probes =
            fixture_stats.policy.global_best_rescue_probes;
        comparison.bounded_rescue_wins =
            fixture_stats.policy.global_best_rescue_wins;
        comparison.bounded_fixture_stats_consistent =
            bounded.matches.simd_chaining_execution_stats_available
            && fixture_stats.is_internally_consistent(
                simd_chaining_policy::bounded_batch_approximate, true);
        comparison.deterministic_rescue_fixture =
            fixture.name == "max_skip_invalid_runs_and_rescue";
        comparison.bounded_route_stats_consistent =
            bounded_fixture_route_accepted;
        comparison.exact_route_stats_consistent = exact_fixture_route_accepted;
        comparison.minimap2_route_stats_consistent =
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
        // match_result has no reference-id field.  Each adapter is invoked with
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
            if (comparison.bounded_fixture_stats_consistent
                && comparison.bounded_cutoff_stops > 0
                && comparison.bounded_candidates_skipped > 0
                && comparison.bounded_rescue_probes > 0
                && comparison.bounded_rescue_wins > 0) {
                ++qualification.deterministic_rescue_fixture_runs;
            }
        }
        qualification.comparisons.push_back(std::move(comparison));
    }

    qualification.exact_route_stats_consistent =
        qualification.exact_route_stats_consistent
        && qualification.exact_policy_stats_available
        && qualification.exact_policy_stats.is_internally_consistent(
            simd_chaining_policy::exact_visited, true);
    qualification.bounded_route_stats_consistent =
        qualification.bounded_route_stats_consistent
        && qualification.bounded_policy_stats_available
        && qualification.bounded_policy_stats.is_internally_consistent(
            simd_chaining_policy::bounded_batch_approximate, true);
    qualification.saturated = qualification.exact_policy_stats.saturated
        || qualification.bounded_policy_stats.saturated
        || qualification.exact_policy_stats.policy.saturated
        || qualification.bounded_policy_stats.policy.saturated;

    return qualification;
}

[[nodiscard]] std::map<std::string, double>
evaluate_simd_chaining_endpoint_qualification(
    std::string_view /*algorithm*/,
    const parameter_map& /*params*/,
    const endpoint_qualification_input& /*input*/,
    const simd_chaining_endpoint_qualification& output)
{
    // Return every metric normally.  Blocking comparison rules use the
    // no-work zero-reference row, so a rejected qualification still produces
    // a complete manifest, ordinary report metrics, and its raw fixture data.
    return {
        // Coverage for the config-global regression rule. This is deliberately
        // finite and identical for both endpoint
        // rows; biological correctness is exclusively gated by endpoint_*.
        {"primary_correct_fraction", 1.0},
        {"endpoint_qualification_accepted", output.accepted() ? 1.0 : 0.0},
        {"endpoint_policy_validation_accepted", output.has_consistent_execution_stats() ? 1.0 : 0.0},
        {"endpoint_rescue_stats_accepted",
            output.rescue_stats_accepted() ? 1.0 : 0.0},
        {"endpoint_exact_route_validation_accepted",
            output.exact_route_stats_consistent ? 1.0 : 0.0},
        {"endpoint_bounded_route_validation_accepted",
            output.bounded_route_stats_consistent ? 1.0 : 0.0},
        {"endpoint_minimap2_route_validation_accepted",
            output.minimap2_route_stats_consistent ? 1.0 : 0.0},
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
            output.exact_policy_stats.policy.exact_visited_calls)},
        {"endpoint_bounded_policy_calls", static_cast<double>(
            output.bounded_policy_stats.policy
                .bounded_batch_approximate_calls)},
        {"endpoint_bounded_score_batches", static_cast<double>(
            output.bounded_policy_stats.policy.bounded_score_batches)},
        {"endpoint_bounded_cutoff_stops", static_cast<double>(
            output.bounded_policy_stats.policy.bounded_cutoff_stops)},
        {"endpoint_bounded_candidates_skipped", static_cast<double>(
            output.bounded_policy_stats.policy
                .bounded_candidates_skipped)},
        {"endpoint_global_best_rescue_probes", static_cast<double>(
            output.bounded_policy_stats.policy
                .global_best_rescue_probes)},
        {"endpoint_global_best_rescue_wins", static_cast<double>(
            output.bounded_policy_stats.policy.global_best_rescue_wins)},
        {"endpoint_exact_policy_rejections", static_cast<double>(
            output.exact_policy_stats.policy.policy_rejections)},
        {"endpoint_bounded_policy_rejections", static_cast<double>(
            output.bounded_policy_stats.policy.policy_rejections)},
        {"endpoint_exact_policy_stats_saturated",
            output.exact_policy_stats.policy.saturated ? 1.0 : 0.0},
        {"endpoint_bounded_policy_stats_saturated",
            output.bounded_policy_stats.policy.saturated ? 1.0 : 0.0},
        {"endpoint_scalar_score_branches", static_cast<double>(
            output.bounded_policy_stats.scalar_score_branches)},
        {"endpoint_scalar_backtrack_branches", static_cast<double>(
            output.bounded_policy_stats.scalar_backtrack_branches)},
        {"endpoint_shared_scalar_backtrack_calls", static_cast<double>(
            output.bounded_policy_stats.shared_scalar_backtrack_calls)},
    };
}

/** @brief Published parity state for one prefilter threshold. */
struct prefilter_pipeline_match_count_parity_stats {
    bool required = false;
    bool performed = false;
    bool passed = false;
};

/// Record only results produced by the existing timed rows. No reference
/// pipeline is executed here. The seeded repetition-major schedule includes
/// both off and Vulkan in repeat zero, so the second observation compares
/// immediately; all later observations also validate per-row determinism.
[[nodiscard]] prefilter_pipeline_match_count_parity_stats
record_prefilter_pipeline_match_count_parity(
    std::string_view algorithm,
    const parameter_map& params,
    const aligner_input& input,
    const aligner_output& output)
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
    const parameter_map& params,
    const aligner_input& input,
    const aligner_output& output)
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
            && !output.prefilter_execution_stats_available
            && output.prefilter_stats
                == prefilter_execution_stats{}
        ? 1.0 : 0.0);
    metrics.emplace("prefilter_backend_scalar",
        output.prefilter_execution_stats_available
            && output.prefilter_stats.is_backend_consistent(
                prefilter_backend::scalar)
        ? 1.0 : 0.0);
    metrics.emplace("prefilter_backend_simd",
        output.prefilter_execution_stats_available
            && output.prefilter_stats.is_backend_consistent(
                prefilter_backend::simd)
        ? 1.0 : 0.0);
    metrics.emplace("prefilter_backend_vulkan",
        output.prefilter_execution_stats_available
            && output.prefilter_stats.is_backend_consistent(
                prefilter_backend::vulkan,
                /*require_evaluated_jobs=*/true,
                /*forbid_unsupported_jobs=*/true)
        ? 1.0 : 0.0);
    metrics.emplace("prefilter_execution_stats_available",
        output.prefilter_execution_stats_available ? 1.0 : 0.0);
    metrics.emplace("prefilter_execution_stats_consistent",
        output.prefilter_execution_stats_consistent ? 1.0 : 0.0);
    metrics.emplace("prefilter_dispatches", static_cast<double>(
        output.prefilter_stats.dispatches));
    metrics.emplace("prefilter_sneaky_snake_dispatches", static_cast<double>(
        output.prefilter_stats.sneaky_snake_dispatches));
    metrics.emplace("prefilter_gatekeeper_dispatches", static_cast<double>(
        output.prefilter_stats.gatekeeper_dispatches));
    metrics.emplace("prefilter_scalar_dispatches", static_cast<double>(
        output.prefilter_stats.scalar_dispatches));
    metrics.emplace("prefilter_simd_dispatches", static_cast<double>(
        output.prefilter_stats.simd_dispatches));
    metrics.emplace("prefilter_vulkan_dispatches", static_cast<double>(
        output.prefilter_stats.vulkan_dispatches));
    metrics.emplace("prefilter_jobs", static_cast<double>(
        output.prefilter_stats.jobs));
    metrics.emplace("prefilter_evaluated_jobs", static_cast<double>(
        output.prefilter_stats.evaluated_jobs));
    metrics.emplace("prefilter_length_difference_jobs", static_cast<double>(
        output.prefilter_stats
            .length_difference_jobs));
    metrics.emplace("prefilter_unsupported_jobs", static_cast<double>(
        output.prefilter_stats.unsupported_jobs));
    metrics.emplace("prefilter_rejected_jobs", static_cast<double>(
        output.prefilter_stats.rejected_jobs));
    metrics.emplace("prefilter_rejected_regions", static_cast<double>(
        output.prefilter_stats.rejected_regions));
    metrics.emplace("prefilter_surviving_regions", static_cast<double>(
        output.prefilter_stats.surviving_regions));
    metrics.emplace("prefilter_mask_rejection_candidates", static_cast<double>(
        output.prefilter_stats.mask_rejection_candidates));
    metrics.emplace("prefilter_exact_guard_activations", static_cast<double>(
        output.prefilter_stats.exact_guard_activations));
    metrics.emplace("prefilter_guard_confirmed_rejections", static_cast<double>(
        output.prefilter_stats.guard_confirmed_rejections));
    metrics.emplace("prefilter_hamming_exact_rejections", static_cast<double>(
        output.prefilter_stats.hamming_exact_rejections));
    metrics.emplace("prefilter_execution_stats_saturated",
        output.prefilter_stats.is_saturated ? 1.0 : 0.0);
    metrics.emplace("ksw2_left_calls", static_cast<double>(
        output.scalar_pipeline_stats.ksw2_left_calls));
    metrics.emplace("ksw2_gap_calls", static_cast<double>(
        output.scalar_pipeline_stats.ksw2_gap_calls));
    metrics.emplace("ksw2_right_calls", static_cast<double>(
        output.scalar_pipeline_stats.ksw2_right_calls));
    metrics.emplace("ksw2_total_calls", static_cast<double>(
        output.scalar_pipeline_stats.ksw2_left_calls
        + output.scalar_pipeline_stats.ksw2_gap_calls
        + output.scalar_pipeline_stats.ksw2_right_calls));
    metrics.emplace("ksw2_input_cell_area", static_cast<double>(
        output.scalar_pipeline_stats.ksw2_input_cell_area));
    metrics.emplace("selected_route_execution_stats_consistent",
        output.selected_route_execution_stats_consistent ? 1.0 : 0.0);
    const auto& biological = output.biological_fingerprint;
    metrics.emplace("biological_fingerprint_computed",
        biological.is_computed ? 1.0 : 0.0);
    metrics.emplace("biological_fingerprint_saturated",
        biological.is_saturated ? 1.0 : 0.0);
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
    metrics.emplace("ksw2_batched_execution_stats_consistent",
        output.ksw2_batched_execution_stats_available
            && output.ksw2_batched_stats.is_coherent(true)
            ? 1.0 : 0.0);
    metrics.emplace("bounded_nthash_query_execution_stats_consistent",
        output.bounded_nthash_query_execution_stats_available
            && output.bounded_nthash_query_execution_stats
                .is_internally_consistent(true)
            ? 1.0 : 0.0);
    metrics.emplace("bounded_nthash_target_execution_stats_consistent",
        output.bounded_nthash_target_execution_stats_available
            && output.bounded_nthash_target_execution_stats
                .is_internally_consistent(true)
            ? 1.0 : 0.0);
    metrics.emplace("bounded_nthash_query_segments_combined",
        static_cast<double>(
            output.bounded_nthash_query_execution_stats.segments_combined));
    metrics.emplace("bounded_nthash_target_segments_combined",
        static_cast<double>(
            output.bounded_nthash_target_execution_stats.segments_combined));
    metrics.emplace("bounded_nthash_fallback_calls",
        static_cast<double>(
            output.bounded_nthash_query_execution_stats.fallback_calls
            + output.bounded_nthash_target_execution_stats.fallback_calls));
    metrics.emplace("exact_simd_query_execution_stats_consistent",
        output.exact_simd_query_execution_stats_available
            && output.exact_simd_query_execution_stats
                .is_internally_consistent(true)
            ? 1.0 : 0.0);
    metrics.emplace("exact_simd_target_execution_stats_consistent",
        output.exact_simd_target_execution_stats_available
            && output.exact_simd_target_execution_stats
                .is_internally_consistent(true)
            ? 1.0 : 0.0);
    metrics.emplace("exact_simd_query_b8_route_accepted",
        output.exact_simd_query_execution_stats_available
            && exact_b8_execution_accepted(
                output.exact_simd_query_execution_stats)
            ? 1.0 : 0.0);
    metrics.emplace("exact_simd_target_b8_route_accepted",
        output.exact_simd_target_execution_stats_available
            && exact_b8_execution_accepted(
                output.exact_simd_target_execution_stats)
            ? 1.0 : 0.0);
    metrics.emplace("exact_simd_query_resident_dual_route_accepted",
        output.exact_simd_query_execution_stats_available
            && exact_resident_dual_vector_stream_accepted(
                output.exact_simd_query_execution_stats)
            ? 1.0 : 0.0);
    metrics.emplace("exact_simd_target_resident_dual_route_accepted",
        output.exact_simd_target_execution_stats_available
            && exact_resident_dual_vector_stream_accepted(
                output.exact_simd_target_execution_stats)
            ? 1.0 : 0.0);
    metrics.emplace("exact_simd_query_fast_route_accepted",
        output.exact_simd_query_execution_stats_available
            && exact_fast_execution_accepted(
                output.exact_simd_query_execution_stats)
            ? 1.0 : 0.0);
    metrics.emplace("exact_simd_target_fast_route_accepted",
        output.exact_simd_target_execution_stats_available
            && exact_fast_execution_accepted(
                output.exact_simd_target_execution_stats)
            ? 1.0 : 0.0);
    metrics.emplace("exact_simd_query_resident_vector_stream_calls",
        static_cast<double>(output.exact_simd_query_execution_stats
            .resident_vector_stream_kernel_calls));
    metrics.emplace("exact_simd_target_resident_vector_stream_calls",
        static_cast<double>(output.exact_simd_target_execution_stats
            .resident_vector_stream_kernel_calls));
    metrics.emplace("exact_simd_query_resident_vector_stream_dual_calls",
        static_cast<double>(output.exact_simd_query_execution_stats
            .resident_vector_stream_dual_calls));
    metrics.emplace("exact_simd_target_resident_vector_stream_dual_calls",
        static_cast<double>(output.exact_simd_target_execution_stats
            .resident_vector_stream_dual_calls));
    metrics.emplace("exact_simd_query_generic_kernel_calls",
        static_cast<double>(output.exact_simd_query_execution_stats
            .generic_u64_kernel_calls));
    metrics.emplace("exact_simd_target_generic_kernel_calls",
        static_cast<double>(output.exact_simd_target_execution_stats
            .generic_u64_kernel_calls));
    metrics.emplace("exact_simd_query_b8_kernel_calls",
        static_cast<double>(output.exact_simd_query_execution_stats
            .kmer_15_window_10_byte_8_uint32_hash_kernel_calls));
    metrics.emplace("exact_simd_target_b8_kernel_calls",
        static_cast<double>(output.exact_simd_target_execution_stats
            .kmer_15_window_10_byte_8_uint32_hash_kernel_calls));
    metrics.emplace("simd_chaining_no_improvement_fast_batches",
        static_cast<double>(output.simd_chaining_stats
            .no_improvement_fast_batches));
    metrics.emplace("simd_chaining_improvement_replay_batches",
        static_cast<double>(output.simd_chaining_stats
            .improvement_replay_batches));
    metrics.emplace("simd_chaining_improvement_replay_lanes",
        static_cast<double>(output.simd_chaining_stats
            .improvement_replay_lanes));
    metrics.emplace("simd_chaining_scalar_orchestration_steps",
        static_cast<double>(output.simd_chaining_stats
            .scalar_orchestration_steps));
    metrics.emplace("simd_chaining_hybrid_stats_coherent",
        output.simd_chaining_execution_stats_available
            && output.simd_chaining_stats
                .has_consistent_exact_execution_stats()
            ? 1.0 : 0.0);
    const bool selected_policy_stats_coherent =
        output.simd_chaining_policy_selected
        && output.simd_chaining_execution_stats_available
        && output.simd_chaining_stats.has_consistent_policy_stats(
            output.selected_simd_chaining_policy,
            /*require_accelerated_work=*/
                output.selected_simd_chaining_policy
                    == simd_chaining_policy::bounded_batch_approximate);
    metrics.emplace("simd_chaining_selected_policy_exact_visited",
        output.simd_chaining_policy_selected
            && output.selected_simd_chaining_policy
                == simd_chaining_policy::exact_visited
            ? 1.0 : 0.0);
    metrics.emplace(
        "simd_chaining_selected_policy_bounded_batch_approximate",
        output.simd_chaining_policy_selected
            && output.selected_simd_chaining_policy
                == simd_chaining_policy::bounded_batch_approximate
            ? 1.0 : 0.0);
    metrics.emplace("simd_chaining_policy_stats_coherent",
        selected_policy_stats_coherent ? 1.0 : 0.0);
    metrics.emplace("simd_chaining_selected_policy_matches_stats",
        selected_policy_stats_coherent ? 1.0 : 0.0);
    metrics.emplace("simd_chaining_exact_visited_calls",
        static_cast<double>(
            output.simd_chaining_stats.policy.exact_visited_calls));
    metrics.emplace("simd_chaining_bounded_batch_approximate_calls",
        static_cast<double>(output.simd_chaining_stats.policy
            .bounded_batch_approximate_calls));
    metrics.emplace("simd_chaining_bounded_score_batches",
        static_cast<double>(output.simd_chaining_stats.policy
            .bounded_score_batches));
    metrics.emplace("simd_chaining_bounded_improving_batches",
        static_cast<double>(output.simd_chaining_stats.policy
            .bounded_improving_batches));
    metrics.emplace("simd_chaining_bounded_nonimproving_batches",
        static_cast<double>(output.simd_chaining_stats.policy
            .bounded_nonimproving_batches));
    metrics.emplace("simd_chaining_bounded_cutoff_stops",
        static_cast<double>(output.simd_chaining_stats.policy
            .bounded_cutoff_stops));
    metrics.emplace("simd_chaining_bounded_candidates_skipped",
        static_cast<double>(output.simd_chaining_stats.policy
            .bounded_candidates_skipped));
    metrics.emplace("simd_chaining_global_best_rescue_probes",
        static_cast<double>(output.simd_chaining_stats.policy
            .global_best_rescue_probes));
    metrics.emplace("simd_chaining_global_best_rescue_wins",
        static_cast<double>(output.simd_chaining_stats.policy
            .global_best_rescue_wins));
    metrics.emplace("simd_chaining_policy_rejections",
        static_cast<double>(output.simd_chaining_stats.policy
            .policy_rejections));
    metrics.emplace("simd_chaining_policy_stats_saturated",
        output.simd_chaining_stats.policy.saturated ? 1.0 : 0.0);
    metrics.emplace("simd_chaining_contiguous_predecessor_store_batches",
        static_cast<double>(output.simd_chaining_stats
            .contiguous_predecessor_store_batches));
    metrics.emplace(
        "simd_chaining_backtrack_primary_contiguous_batches",
        static_cast<double>(output.simd_chaining_stats
            .backtrack_primary_contiguous_batches));
    metrics.emplace(
        "simd_chaining_backtrack_all_starts_visited_fast_batches",
        static_cast<double>(output.simd_chaining_stats
            .backtrack_all_starts_visited_fast_batches));
    metrics.emplace("simd_chaining_execution_stats_consistent",
        output.simd_chaining_policy_selected
            && output.simd_chaining_execution_stats_available
            && output.simd_chaining_stats
                .is_internally_consistent(
                    output.selected_simd_chaining_policy,
                    /*require_nonempty_vector_work=*/true)
            ? 1.0 : 0.0);
    metrics.emplace("simd_chaining_scalar_score_branches",
        static_cast<double>(output.simd_chaining_stats
            .scalar_score_branches));
    metrics.emplace("simd_chaining_scalar_backtrack_branches",
        static_cast<double>(output.simd_chaining_stats
            .scalar_backtrack_branches));
    metrics.emplace("simd_chaining_shared_scalar_backtrack_calls",
        static_cast<double>(output.simd_chaining_stats
            .shared_scalar_backtrack_calls));
    metrics.emplace("mixed_gpu_pipeline_selected",
                    output.mixed_gpu_pipeline_marker ? 1.0 : 0.0);
    metrics.emplace("all_gpu_acceptance_eligible",
                    output.all_gpu_acceptance_eligible ? 1.0 : 0.0);
    metrics.emplace("gpu_index_ownership_validation_consistent",
        output.gpu_index_ownership_stats_consistent ? 1.0 : 0.0);
    metrics.emplace("vulkan_query_sketch_dispatches",
                    static_cast<double>(output.partial_vulkan_stats
                                            .query_sketch_dispatches));
    metrics.emplace("vulkan_target_sketch_dispatches",
                    static_cast<double>(output.partial_vulkan_stats
                                            .target_sketch_dispatches));
    metrics.emplace("vulkan_target_index_build_dispatches",
                    static_cast<double>(output.partial_vulkan_stats
                                            .target_index_build_dispatches));
    metrics.emplace("vulkan_seed_lookup_dispatches",
                    static_cast<double>(output.partial_vulkan_stats
                                            .seed_lookup_dispatches));
    metrics.emplace("vulkan_seed_open_addressed_lookup_dispatches",
                    static_cast<double>(output.partial_vulkan_stats
                                            .seed_open_addressed_lookup_dispatches));
    metrics.emplace("vulkan_seed_sorted_dense_lookup_dispatches",
                    static_cast<double>(output.partial_vulkan_stats
                                            .seed_sorted_dense_lookup_dispatches));
    metrics.emplace("vulkan_chain_score_dispatches",
                    static_cast<double>(output.partial_vulkan_stats
                                            .chain_score_dispatches));
    metrics.emplace("vulkan_seed_bitonic_sort_dispatches",
                    static_cast<double>(output.partial_vulkan_stats
                                            .seed_bitonic_sort_dispatches));
    metrics.emplace("vulkan_seed_radix_sort_operations",
                    static_cast<double>(output.partial_vulkan_stats
                                            .seed_radix_sort_operations));
    metrics.emplace("vulkan_seed_radix_anchor_count",
                    static_cast<double>(output.partial_vulkan_stats
                                            .seed_radix_anchor_count));
    metrics.emplace("vulkan_seed_radix_passes",
                    static_cast<double>(output.partial_vulkan_stats
                                            .seed_radix_passes));
    metrics.emplace("vulkan_seed_radix_histogram_dispatches",
                    static_cast<double>(output.partial_vulkan_stats
                                            .seed_radix_histogram_dispatches));
    metrics.emplace("vulkan_seed_radix_scan_dispatches",
                    static_cast<double>(output.partial_vulkan_stats
                                            .seed_radix_scan_dispatches));
    metrics.emplace("vulkan_seed_radix_scatter_dispatches",
                    static_cast<double>(output.partial_vulkan_stats
                                            .seed_radix_scatter_dispatches));
    metrics.emplace("vulkan_seed_host_sort_fallbacks",
                    static_cast<double>(output.partial_vulkan_stats
                                            .seed_host_sort_fallbacks));
    const auto& gpu_index = output.partial_vulkan_stats;
    metrics.emplace("vulkan_seed_anchor_validation_operations",
        static_cast<double>(gpu_index.seed_anchor_stats_operations));
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
    metrics.emplace("gpu_index_count_stage_admissions",
        static_cast<double>(gpu_index.gpu_index_count_admissions));
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
    metrics.emplace("source_direct_gpu_index_materialization_validation_accepted",
        output.is_source_direct_gpu_index_materialization_consistent
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
    const parameter_map& params,
    const aligner_input& input,
    const aligner_output& output)
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
            bioseq::bench::prefilter_metrics::oriented_truth_sequence(
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
        bioseq::bench::prefilter_metrics::summarize_primary_correctness(
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

[[nodiscard]] nlohmann::json scalar_pipeline_stats_json(
    const scalar_pipeline_execution_stats& stats, bool available)
{
    return nlohmann::json{
        {"available", available},
        {"query_minimizers", {
            {"calls", stats.query_minimizers.calls},
            {"input_symbols", stats.query_minimizers.input_symbols},
            {"saturated", stats.query_minimizers.saturated},
        }},
        {"target_minimizers", {
            {"calls", stats.target_minimizers.calls},
            {"input_symbols", stats.target_minimizers.input_symbols},
            {"saturated", stats.target_minimizers.saturated},
        }},
        {"seed_lookup_calls", stats.seed_lookup_calls},
        {"seed_query_minimizers", stats.seed_query_minimizers},
        {"chain_score_calls", stats.chain_score_calls},
        {"chain_backtrack_calls", stats.chain_backtrack_calls},
        {"chain_anchor_inputs", stats.chain_anchor_inputs},
        {"ksw2_left_calls", stats.ksw2_left_calls},
        {"ksw2_gap_calls", stats.ksw2_gap_calls},
        {"ksw2_right_calls", stats.ksw2_right_calls},
        {"ksw2_input_cell_area", stats.ksw2_input_cell_area},
        {"saturated", stats.is_saturated},
    };
}

[[nodiscard]] nlohmann::json ksw2_batched_stats_json(
    const ksw2_batched_execution_stats& stats, bool available)
{
    return nlohmann::json{
        {"available", available},
        {"consistent_nonempty",
            available && stats.is_coherent(true)},
        {"align_region_invocations", stats.alignment_region_invocations},
        {"regions_seen", stats.regions_seen},
        {"fills_collected", stats.fills_collected},
        {"nonempty_fill_calls", stats.nonempty_fill_calls},
        {"short_bucket_dispatches", stats.short_bucket_dispatches},
        {"short_bucket_lanes", stats.short_bucket_lanes},
        {"long_fill_calls", stats.long_fill_calls},
        {"input_cell_area", stats.input_cell_area},
        {"saturated", stats.saturated},
    };
}

[[nodiscard]] nlohmann::json two_stack_stats_json(
    const two_stack_execution_stats& stats, bool available)
{
#define BIOSEQ_STATS_FIELD(name) {#name, stats.name}
    return nlohmann::json{
        {"available", available},
        BIOSEQ_STATS_FIELD(bp_consumed),
        BIOSEQ_STATS_FIELD(minimizers_emitted),
        BIOSEQ_STATS_FIELD(vector_batches),
        BIOSEQ_STATS_FIELD(vector_lanes_visited),
        BIOSEQ_STATS_FIELD(masked_tail_lanes),
        BIOSEQ_STATS_FIELD(bridge_symbols_staged),
        BIOSEQ_STATS_FIELD(bridge_vector_runs),
        BIOSEQ_STATS_FIELD(consecutive_kmer_batches),
        BIOSEQ_STATS_FIELD(canonical_scramble_batches),
        BIOSEQ_STATS_FIELD(window_reduction_batches),
        BIOSEQ_STATS_FIELD(event_generation_batches),
        BIOSEQ_STATS_FIELD(stable_compress_batches),
        BIOSEQ_STATS_FIELD(direct_event_compress_batches),
        BIOSEQ_STATS_FIELD(tie_matrix_event_batches),
        BIOSEQ_STATS_FIELD(row_transpose_batches),
        BIOSEQ_STATS_FIELD(row_vector_stores),
        BIOSEQ_STATS_FIELD(palindrome_lanes),
        BIOSEQ_STATS_FIELD(first_window_tie_events),
        BIOSEQ_STATS_FIELD(expiry_tie_events),
        BIOSEQ_STATS_FIELD(entering_min_transitions),
        BIOSEQ_STATS_FIELD(expiry_rescan_transitions),
        BIOSEQ_STATS_FIELD(final_flush_records),
        BIOSEQ_STATS_FIELD(fragment_symbols_staged),
        BIOSEQ_STATS_FIELD(packed_decode_batches),
        BIOSEQ_STATS_FIELD(packed_decode_lanes),
        BIOSEQ_STATS_FIELD(ambiguous_symbols),
        BIOSEQ_STATS_FIELD(ambiguous_reset_batches),
        BIOSEQ_STATS_FIELD(packed_word_boundary_merges),
        BIOSEQ_STATS_FIELD(scalar_orchestration_steps),
        BIOSEQ_STATS_FIELD(fail_closed_rejections),
        BIOSEQ_STATS_FIELD(scalar_backend_branches),
        BIOSEQ_STATS_FIELD(scalar_tail_symbols),
        BIOSEQ_STATS_FIELD(scalar_hash_symbols),
        BIOSEQ_STATS_FIELD(scalar_window_transitions),
        BIOSEQ_STATS_FIELD(scalar_event_records),
        BIOSEQ_STATS_FIELD(scalar_sort_records),
        BIOSEQ_STATS_FIELD(scalar_dedup_records),
        BIOSEQ_STATS_FIELD(resident_vector_stream_kernel_calls),
        BIOSEQ_STATS_FIELD(resident_vector_stream_single_calls),
        BIOSEQ_STATS_FIELD(resident_vector_stream_dual_calls),
        BIOSEQ_STATS_FIELD(resident_vector_stream_nonempty_work_calls),
        BIOSEQ_STATS_FIELD(resident_vector_stream_input_bp),
        BIOSEQ_STATS_FIELD(resident_vector_stream_minimizers_emitted),
        BIOSEQ_STATS_FIELD(generic_u64_kernel_calls),
        BIOSEQ_STATS_FIELD(generic_u64_candidate_blocks),
        BIOSEQ_STATS_FIELD(generic_u64_lane_visits),
        BIOSEQ_STATS_FIELD(kmer_15_window_10_byte_8_uint32_hash_kernel_calls),
        BIOSEQ_STATS_FIELD(kmer_15_window_10_byte_8_uint32_hash_candidate_blocks),
        BIOSEQ_STATS_FIELD(kmer_15_window_10_byte_8_uint32_hash_lane_visits),
        BIOSEQ_STATS_FIELD(kmer_15_window_10_byte_8_packed_22_groups),
        BIOSEQ_STATS_FIELD(kmer_15_window_10_byte_8_packed_22_lane_visits),
        BIOSEQ_STATS_FIELD(tie_hash_prefilter_batches),
        BIOSEQ_STATS_FIELD(tie_hash_prefilter_rejected_lanes),
        BIOSEQ_STATS_FIELD(tie_raw_verification_batches),
        BIOSEQ_STATS_FIELD(saturated),
    };
#undef BIOSEQ_STATS_FIELD
}

[[nodiscard]] nlohmann::json bounded_nthash_stats_json(
    const bounded_nthash_execution_stats& stats, bool available)
{
#define BIOSEQ_STATS_FIELD(name) {#name, stats.name}
    return nlohmann::json{
        {"available", available},
        {"consistent_nonempty",
            available && stats.is_internally_consistent(true)},
        BIOSEQ_STATS_FIELD(scheme_id),
        BIOSEQ_STATS_FIELD(kmer_size),
        BIOSEQ_STATS_FIELD(window_size),
        BIOSEQ_STATS_FIELD(nthash_rotation),
        BIOSEQ_STATS_FIELD(source_storage_unit_bytes),
        BIOSEQ_STATS_FIELD(input_symbols),
        BIOSEQ_STATS_FIELD(input_storage_units),
        BIOSEQ_STATS_FIELD(segments_combined),
        BIOSEQ_STATS_FIELD(logical_streams),
        BIOSEQ_STATS_FIELD(physical_vector_lanes),
        BIOSEQ_STATS_FIELD(vector_registers_per_step),
        BIOSEQ_STATS_FIELD(packed_decode_batches),
        BIOSEQ_STATS_FIELD(packed_decode_lane_visits),
        BIOSEQ_STATS_FIELD(hash_initialization_batches),
        BIOSEQ_STATS_FIELD(rolling_hash_batches),
        BIOSEQ_STATS_FIELD(hash_table_lookup_batches),
        BIOSEQ_STATS_FIELD(canonical_sum_batches),
        BIOSEQ_STATS_FIELD(window_update_batches),
        BIOSEQ_STATS_FIELD(suffix_rebuild_batches),
        BIOSEQ_STATS_FIELD(emission_decision_batches),
        BIOSEQ_STATS_FIELD(emission_mask_store_batches),
        BIOSEQ_STATS_FIELD(minimizers_emitted),
        BIOSEQ_STATS_FIELD(tied_endpoint_records_emitted),
        BIOSEQ_STATS_FIELD(stream_boundary_records_buffered),
        BIOSEQ_STATS_FIELD(stream_boundary_records_committed),
        BIOSEQ_STATS_FIELD(stream_boundary_duplicates_suppressed),
        BIOSEQ_STATS_FIELD(scalar_collector_appends),
        BIOSEQ_STATS_FIELD(partial_chunk_callback_batches),
        BIOSEQ_STATS_FIELD(masked_tail_lane_visits),
        BIOSEQ_STATS_FIELD(fail_closed_rejections),
        BIOSEQ_STATS_FIELD(scalar_hash_symbols),
        BIOSEQ_STATS_FIELD(scalar_window_transitions),
        BIOSEQ_STATS_FIELD(scalar_minimum_decisions),
        BIOSEQ_STATS_FIELD(fallback_calls),
        BIOSEQ_STATS_FIELD(positions_are_segment_local),
        BIOSEQ_STATS_FIELD(aggregation_mismatch),
        BIOSEQ_STATS_FIELD(saturated),
    };
#undef BIOSEQ_STATS_FIELD
}

[[nodiscard]] nlohmann::json simd_chaining_stats_json(
    const simd_chaining_execution_stats& stats,
    bool available,
    simd_chaining_policy selected_policy,
    bool policy_selected)
{
#define BIOSEQ_STATS_FIELD(name) {#name, stats.name}
    return nlohmann::json{
        {"available", available},
        {"selected_policy_available", policy_selected},
        {"selected_policy", policy_selected
            ? std::string{bioseq::chaining::simd_chaining_policy_name(
                selected_policy)}
            : std::string{"not_selected"}},
        {"selected_policy_matches_stats",
            policy_selected && available
                && stats.has_consistent_policy_stats(
                    selected_policy,
                    selected_policy
                        == simd_chaining_policy::bounded_batch_approximate)},
        {"selected_policy_stats_consistent",
            policy_selected && available
                && stats.is_internally_consistent(selected_policy, true)},
        {"exact_hybrid_stats_coherent",
            stats.has_consistent_exact_execution_stats()},
        BIOSEQ_STATS_FIELD(anchors_consumed),
        BIOSEQ_STATS_FIELD(score_vector_batches),
        BIOSEQ_STATS_FIELD(score_vector_lanes),
        BIOSEQ_STATS_FIELD(score_masked_lanes),
        BIOSEQ_STATS_FIELD(contiguous_predecessor_store_batches),
        BIOSEQ_STATS_FIELD(descending_candidate_steps),
        BIOSEQ_STATS_FIELD(visited_marks),
        BIOSEQ_STATS_FIELD(max_chain_skip_stops),
        BIOSEQ_STATS_FIELD(no_improvement_fast_batches),
        BIOSEQ_STATS_FIELD(improvement_replay_batches),
        BIOSEQ_STATS_FIELD(improvement_replay_lanes),
        BIOSEQ_STATS_FIELD(global_best_vector_batches),
        BIOSEQ_STATS_FIELD(global_best_vector_lanes),
        BIOSEQ_STATS_FIELD(backtrack_rank_batches),
        BIOSEQ_STATS_FIELD(backtrack_path_vector_batches),
        BIOSEQ_STATS_FIELD(backtrack_commit_vector_batches),
        BIOSEQ_STATS_FIELD(backtrack_vector_lanes),
        BIOSEQ_STATS_FIELD(backtrack_overlap_stops),
        BIOSEQ_STATS_FIELD(backtrack_primary_contiguous_batches),
        BIOSEQ_STATS_FIELD(backtrack_all_starts_visited_fast_batches),
        BIOSEQ_STATS_FIELD(scalar_orchestration_steps),
        BIOSEQ_STATS_FIELD(fail_closed_rejections),
        BIOSEQ_STATS_FIELD(scalar_score_branches),
        BIOSEQ_STATS_FIELD(scalar_backtrack_branches),
        BIOSEQ_STATS_FIELD(shared_scalar_backtrack_calls),
        {"policy", {
            {"exact_visited_calls", stats.policy.exact_visited_calls},
            {"bounded_batch_approximate_calls",
                stats.policy.bounded_batch_approximate_calls},
            {"bounded_score_batches", stats.policy.bounded_score_batches},
            {"bounded_improving_batches",
                stats.policy.bounded_improving_batches},
            {"bounded_nonimproving_batches",
                stats.policy.bounded_nonimproving_batches},
            {"bounded_cutoff_stops", stats.policy.bounded_cutoff_stops},
            {"bounded_candidates_skipped",
                stats.policy.bounded_candidates_skipped},
            {"global_best_rescue_probes",
                stats.policy.global_best_rescue_probes},
            {"global_best_rescue_wins",
                stats.policy.global_best_rescue_wins},
            {"policy_rejections", stats.policy.policy_rejections},
            {"saturated", stats.policy.saturated},
        }},
        BIOSEQ_STATS_FIELD(saturated),
    };
#undef BIOSEQ_STATS_FIELD
}

[[nodiscard]] nlohmann::json block_aligner_stats_json(
    const block_aligner_execution_stats& stats, bool available)
{
#define BIOSEQ_STATS_FIELD(name) {#name, stats.name}
    return nlohmann::json{
        {"available", available},
        {"align_region_invocations", stats.align_region_invocations},
        BIOSEQ_STATS_FIELD(serial_align_region_dispatches),
        BIOSEQ_STATS_FIELD(parallel_align_region_dispatches),
        BIOSEQ_STATS_FIELD(routed_region_scopes),
        BIOSEQ_STATS_FIELD(left_route_candidates),
        BIOSEQ_STATS_FIELD(gap_route_candidates),
        BIOSEQ_STATS_FIELD(right_route_candidates),
        BIOSEQ_STATS_FIELD(left_eligible),
        BIOSEQ_STATS_FIELD(left_executed),
        BIOSEQ_STATS_FIELD(gap_eligible),
        BIOSEQ_STATS_FIELD(gap_executed),
        BIOSEQ_STATS_FIELD(right_eligible),
        BIOSEQ_STATS_FIELD(right_executed),
        BIOSEQ_STATS_FIELD(global_calls),
        {"native_xdrop_calls", stats.native_x_drop_calls},
        {"native_xdrop_terminations", stats.native_x_drop_terminations},
        BIOSEQ_STATS_FIELD(zero_work_calls),
        BIOSEQ_STATS_FIELD(nonempty_calls),
        BIOSEQ_STATS_FIELD(nonempty_calls_with_simd),
        BIOSEQ_STATS_FIELD(trace_calls),
        BIOSEQ_STATS_FIELD(executed_trace_calls),
        BIOSEQ_STATS_FIELD(forward_input_calls),
        BIOSEQ_STATS_FIELD(reversed_input_calls),
        BIOSEQ_STATS_FIELD(executed_global_calls),
        {"executed_native_xdrop_calls", stats.executed_native_x_drop_calls},
        BIOSEQ_STATS_FIELD(executed_forward_input_calls),
        BIOSEQ_STATS_FIELD(executed_reversed_input_calls),
        {"simd_lane8_calls", stats.simd_eight_lane_calls},
        {"simd_lane16_calls", stats.simd_sixteen_lane_calls},
        BIOSEQ_STATS_FIELD(simd_dp_vector_batches),
        BIOSEQ_STATS_FIELD(simd_dp_cells),
        BIOSEQ_STATS_FIELD(adaptive_grow_events),
        BIOSEQ_STATS_FIELD(adaptive_shrink_events),
        BIOSEQ_STATS_FIELD(expected_reversed_compact_symbols),
        BIOSEQ_STATS_FIELD(reversed_compact_symbols),
        {"config_identity_calls", stats.configuration_identity_calls},
        {"config_identity_xor", stats.configuration_identity_xor},
        {"config_identity_sum", stats.configuration_identity_sum},
        {"executed_config_identity_calls",
            stats.executed_configuration_identity_calls},
        {"executed_config_identity_xor",
            stats.executed_configuration_identity_xor},
        {"executed_config_identity_sum",
            stats.executed_configuration_identity_sum},
        BIOSEQ_STATS_FIELD(unsupported_contract_rejections),
        BIOSEQ_STATS_FIELD(forbidden_ksw2_calls),
        BIOSEQ_STATS_FIELD(forbidden_scalar_biological_fallbacks),
        {"saturated", stats.is_saturated},
    };
#undef BIOSEQ_STATS_FIELD
}

/// Lossless per-repeat stats for the statistically paired XXL companion.
/// The framework adds algorithm/test-case/repeat/schedule/wall-clock fields
/// and durably flushes the resulting object. Keeping the route stats beside
/// every duration prevents a fast sample from being accepted after a silent
/// backend change; repeat zero is also summarized separately as first-repeat
/// stats by the framework raw manifest (not claimed globally cold).
[[nodiscard]] std::string serialize_xxl_paired_sample(
    std::string_view algorithm,
    const parameter_map& /*params*/,
    const aligner_input& /*input*/,
    const aligner_output& output)
{
    const auto& stats = output.partial_vulkan_stats;
    const auto& biological = output.biological_fingerprint;
    return nlohmann::json{
        {"payload_schema", "bioseq.xxl_hifi.route_validation"},
        {"payload_schema_version", 7},
        {"total_pairs", output.total_pairs},
        {"total_results", output.total_results},
        {"correct_pairs", output.correct_pairs},
        {"selected_route_execution_stats_consistent",
            output.selected_route_execution_stats_consistent},
        {"mixed_gpu_pipeline_selected",
            output.mixed_gpu_pipeline_marker},
        {"all_gpu_acceptance_eligible",
            output.all_gpu_acceptance_eligible},
        {"gpu_index_ownership_validation_consistent",
            output.gpu_index_ownership_stats_consistent},
        {"source_direct_gpu_index_materialization_validation_accepted",
            algorithm == "bioseq_block_vulkan_dp_vulkan_minimap_int"
                && output
                    .is_source_direct_gpu_index_materialization_consistent},
        {"biological_fingerprint", {
            {"computed", biological.is_computed},
            {"saturated", biological.is_saturated},
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
        {"exact_simd_query_execution_stats", two_stack_stats_json(
            output.exact_simd_query_execution_stats,
            output.exact_simd_query_execution_stats_available)},
        {"exact_simd_target_execution_stats", two_stack_stats_json(
            output.exact_simd_target_execution_stats,
            output.exact_simd_target_execution_stats_available)},
        {"bounded_nthash_query_execution_stats", bounded_nthash_stats_json(
            output.bounded_nthash_query_execution_stats,
            output.bounded_nthash_query_execution_stats_available)},
        {"bounded_nthash_target_execution_stats", bounded_nthash_stats_json(
            output.bounded_nthash_target_execution_stats,
            output.bounded_nthash_target_execution_stats_available)},
        {"scalar_pipeline_execution_stats", scalar_pipeline_stats_json(
            output.scalar_pipeline_stats,
            output.scalar_pipeline_execution_stats_available)},
        {"ksw2_batched_execution_stats", ksw2_batched_stats_json(
            output.ksw2_batched_stats,
            output.ksw2_batched_execution_stats_available)},
        {"simd_chaining_execution_stats", simd_chaining_stats_json(
            output.simd_chaining_stats,
            output.simd_chaining_execution_stats_available,
            output.selected_simd_chaining_policy,
            output.simd_chaining_policy_selected)},
        {"block_aligner_execution_stats", block_aligner_stats_json(
            output.block_aligner_stats,
            output.block_aligner_execution_stats_available)},
        {"partial_vulkan_execution_stats", {
            {"query_sketch_dispatches", stats.query_sketch_dispatches},
            {"target_sketch_dispatches", stats.target_sketch_dispatches},
            {"target_index_build_dispatches",
                stats.target_index_build_dispatches},
            {"seed_lookup_dispatches", stats.seed_lookup_dispatches},
            {"seed_open_addressed_lookup_dispatches",
                stats.seed_open_addressed_lookup_dispatches},
            {"seed_sorted_dense_lookup_dispatches",
                stats.seed_sorted_dense_lookup_dispatches},
            {"chain_score_dispatches", stats.chain_score_dispatches},
            {"seed_bitonic_sort_dispatches",
                stats.seed_bitonic_sort_dispatches},
            {"seed_radix_sort_operations", stats.seed_radix_sort_operations},
            {"seed_radix_anchor_count", stats.seed_radix_anchor_count},
            {"seed_radix_passes", stats.seed_radix_passes},
            {"seed_radix_histogram_dispatches",
                stats.seed_radix_histogram_dispatches},
            {"seed_radix_scan_dispatches",
                stats.seed_radix_scan_dispatches},
            {"seed_radix_scatter_dispatches",
                stats.seed_radix_scatter_dispatches},
            {"seed_host_sort_fallbacks", stats.seed_host_sort_fallbacks},
            {"seed_anchor_validation_operations",
                stats.seed_anchor_stats_operations},
            {"seed_canonical_anchor_count",
                stats.seed_canonical_anchor_count},
            {"seed_canonical_anchor_order_checks",
                stats.seed_canonical_anchor_order_checks},
            {"seed_canonical_anchor_order_accepts",
                stats.seed_canonical_anchor_order_accepts},
            {"seed_canonical_tie_normalization_runs",
                stats.seed_canonical_tie_normalization_runs},
            {"seed_canonical_tie_normalization_anchors",
                stats.seed_canonical_tie_normalization_anchors},
            {"seed_anchor_digest_lane0_hex",
                uint64_hex(stats.seed_anchor_digest_lane0)},
            {"seed_anchor_digest_lane1_hex",
                uint64_hex(stats.seed_anchor_digest_lane1)},
            {"gpu_index_materializations", stats.gpu_index_materializations},
            {"gpu_index_open_addressed_materializations",
                stats.gpu_index_open_addressed_materializations},
            {"gpu_index_sorted_dense_materializations",
                stats.gpu_index_sorted_dense_materializations},
            {"gpu_index_unique_keys", stats.gpu_index_unique_keys},
            {"gpu_index_slot_capacity", stats.gpu_index_slot_capacity},
            {"gpu_index_logical_resident_bytes",
                stats.gpu_index_logical_resident_bytes},
            {"gpu_index_actual_allocation_bytes",
                stats.gpu_index_actual_allocation_bytes},
            {"gpu_index_fully_touched_bytes",
                stats.gpu_index_fully_touched_bytes},
            {"gpu_index_staging_bytes", stats.gpu_index_staging_bytes},
            {"gpu_index_permit_releases", stats.gpu_index_permit_releases},
            {"gpu_index_process_footprint_reflections",
                stats.gpu_index_process_footprint_reflections},
            {"gpu_index_reflected_footprint_bytes",
                stats.gpu_index_reflected_footprint_bytes},
            {"gpu_index_max_load_ppm", stats.gpu_index_max_load_ppm},
            {"gpu_index_max_probe", stats.gpu_index_max_probe},
            {"gpu_index_count_stage_admissions",
                stats.gpu_index_count_admissions},
            {"gpu_index_main_peak_admissions",
                stats.gpu_index_main_peak_admissions},
            {"gpu_index_resident_transfers",
                stats.gpu_index_resident_transfers},
            {"gpu_index_adoption_rereserve_attempts",
                stats.gpu_index_adoption_rereserve_attempts},
            {"gpu_index_adoption_successes",
                stats.gpu_index_adoption_successes},
            {"gpu_index_owned_resident_bytes",
                stats.gpu_index_owned_resident_bytes},
            {"gpu_index_gate_enabled", stats.gpu_index_gate_enabled},
            {"gpu_index_gate_word_count", stats.gpu_index_gate_word_count},
            {"gpu_index_gate_mask", stats.gpu_index_gate_mask},
            {"gpu_index_gate_set_bits", stats.gpu_index_gate_set_bits},
            {"gpu_index_gate_digest_lo", stats.gpu_index_gate_digest_lo},
            {"gpu_index_gate_digest_hi", stats.gpu_index_gate_digest_hi},
            {"gpu_index_gate_count_dispatches",
                stats.gpu_index_gate_count_dispatches},
            {"gpu_index_gated_emit_dispatches",
                stats.gpu_index_gated_emit_dispatches},
            {"gpu_index_ungated_emit_dispatches",
                stats.gpu_index_ungated_emit_dispatches},
            {"gpu_index_exact_count_dispatches",
                stats.gpu_index_exact_count_dispatches},
            {"gpu_index_exact_count_prefix_scan_dispatches",
                stats.gpu_index_exact_count_prefix_scan_dispatches},
            {"gpu_index_exact_count_control_readbacks",
                stats.gpu_index_exact_count_control_readbacks},
            {"gpu_index_deterministic_tile_emit_dispatches",
                stats.gpu_index_deterministic_tile_emit_dispatches},
            {"gpu_index_atomic_append_dispatches",
                stats.gpu_index_atomic_append_dispatches},
            {"gpu_index_emit_validation_control_readbacks",
                stats.gpu_index_emit_validation_control_readbacks},
            {"gpu_index_counted_minimizer_count",
                stats.gpu_index_counted_minimizer_count},
            {"gpu_index_verified_emit_count",
                stats.gpu_index_verified_emit_count},
            {"gpu_index_count_emit_parity_verified",
                stats.gpu_index_count_emit_parity_verified},
            {"gpu_index_target_minimizer_capacity_estimate",
                stats.gpu_index_target_minimizer_capacity_estimate},
            {"gpu_index_capacity_estimate_underestimated",
                stats.gpu_index_capacity_estimate_underestimated},
            {"gpu_index_arena_trimmed_before_permit_shrink",
                stats.gpu_index_arena_trimmed_before_permit_shrink},
            {"gpu_index_retained_minimizer_count",
                stats.gpu_index_retained_minimizer_count},
            {"gpu_index_distinct_minimizer_count",
                stats.gpu_index_distinct_minimizer_count},
            {"gpu_index_source_direct_operations",
                stats.gpu_index_source_direct_operations},
            {"gpu_index_source_direct_terminal_successes",
                stats.gpu_index_source_direct_terminal_successes},
            {"gpu_index_source_direct_empty_bypasses",
                stats.gpu_index_source_direct_empty_bypasses},
            {"gpu_index_source_direct_zero_query_bypasses",
                stats.gpu_index_source_direct_zero_query_bypasses},
            {"gpu_index_source_direct_planning_receipts",
                stats.gpu_index_source_direct_planning_receipts},
            {"gpu_index_source_direct_terminal_receipts",
                stats.gpu_index_source_direct_terminal_receipts},
            {"gpu_index_source_direct_materializations",
                stats.gpu_index_source_direct_materializations},
            {"gpu_index_source_direct_resident_publications",
                stats.gpu_index_source_direct_resident_publications},
            {"gpu_index_source_direct_logical_control_bytes",
                stats.gpu_index_source_direct_logical_control_bytes},
            {"gpu_index_source_direct_logical_position_bytes",
                stats.gpu_index_source_direct_logical_position_bytes},
            {"gpu_index_source_direct_control_requirement_bytes",
                stats.gpu_index_source_direct_control_requirement_bytes},
            {"gpu_index_source_direct_position_requirement_bytes",
                stats.gpu_index_source_direct_position_requirement_bytes},
            {"gpu_index_source_direct_allocation_request_bytes",
                stats.gpu_index_source_direct_allocation_request_bytes},
            {"gpu_index_source_direct_actual_control_bytes",
                stats.gpu_index_source_direct_actual_control_bytes},
            {"gpu_index_source_direct_actual_position_bytes",
                stats.gpu_index_source_direct_actual_position_bytes},
            {"gpu_index_source_direct_source_releases",
                stats.gpu_index_source_direct_source_releases},
            {"gpu_index_source_direct_source_released_bytes",
                stats.gpu_index_source_direct_source_released_bytes},
            {"gpu_index_source_direct_checksum_matches",
                stats.gpu_index_source_direct_checksum_matches},
            {"gpu_index_source_direct_source_traversals",
                stats.gpu_index_source_direct_source_traversals},
            {"gpu_index_source_direct_mapped_traversals",
                stats.gpu_index_source_direct_mapped_traversals},
            {"gpu_index_source_direct_reflected_authorities",
                stats.gpu_index_source_direct_reflected_authorities},
            {"gpu_index_source_direct_retained_authorities",
                stats.gpu_index_source_direct_retained_authorities},
            {"gpu_index_source_direct_retained_resident_permit_bytes",
                stats
                    .gpu_index_source_direct_retained_resident_permit_bytes},
            {"gpu_index_source_direct_cache_preinsertions",
                stats.gpu_index_source_direct_cache_preinsertions},
            {"gpu_index_source_direct_expected_seed_consumers",
                stats.gpu_index_source_direct_expected_seed_consumers},
            {"gpu_index_source_direct_completed_seed_consumers",
                stats.gpu_index_source_direct_completed_seed_consumers},
            {"gpu_index_source_direct_required_lookups",
                stats.gpu_index_source_direct_required_lookups},
            {"gpu_index_source_direct_required_hits",
                stats.gpu_index_source_direct_required_hits},
            {"gpu_index_source_direct_required_misses",
                stats.gpu_index_source_direct_required_misses},
            {"gpu_index_source_direct_fallback_attempts",
                stats.gpu_index_source_direct_fallback_attempts},
            {"gpu_index_source_direct_mapped_prefix_passes",
                stats.gpu_index_source_direct_mapped_prefix_passes},
            {"gpu_index_source_direct_mapped_prefix_lookups",
                stats.gpu_index_source_direct_mapped_prefix_lookups},
            {"gpu_index_source_direct_mapped_prefix_comparisons",
                stats.gpu_index_source_direct_mapped_prefix_comparisons},
            {"gpu_index_source_direct_seed_slab_requested_bytes",
                stats.gpu_index_source_direct_seed_slab_requested_bytes},
            {"gpu_index_source_direct_seed_slab_initial_capacity",
                stats.gpu_index_source_direct_seed_slab_initial_capacity},
            {"gpu_index_source_direct_seed_slab_actual_bytes",
                stats.gpu_index_source_direct_seed_slab_actual_bytes},
            {"gpu_index_source_direct_seed_slab_retained_bytes",
                stats.gpu_index_source_direct_seed_slab_retained_bytes},
            {"gpu_index_source_direct_seed_slab_permit_releases",
                stats.gpu_index_source_direct_seed_slab_permit_releases},
            {"gpu_index_source_direct_seed_reserved_bytes",
                stats.gpu_index_source_direct_seed_reserved_bytes},
            {"gpu_index_source_direct_seed_planned_vulkan_bytes",
                stats.gpu_index_source_direct_seed_planned_vulkan_bytes},
            {"gpu_index_source_direct_seed_actual_vulkan_bytes",
                stats.gpu_index_source_direct_seed_actual_vulkan_bytes},
            {"gpu_index_source_direct_seed_roundtrips",
                stats.gpu_index_source_direct_seed_roundtrips},
            {"gpu_index_source_direct_zero_anchor_bypasses",
                stats.gpu_index_source_direct_zero_anchor_bypasses},
            {"gpu_index_source_direct_seed_fence_completions",
                stats.gpu_index_source_direct_seed_fence_completions},
            {"gpu_index_source_direct_cache_cleanup_releases",
                stats.gpu_index_source_direct_cache_cleanup_releases},
            {"gpu_index_source_direct_resident_cleanup_bytes",
                stats.gpu_index_source_direct_resident_cleanup_bytes},
            {"gpu_index_source_direct_pipeline_completions",
                stats.gpu_index_source_direct_pipeline_completions},
            {"post_seed_owned_gpu_cache_evictions",
                stats.post_seed_owned_gpu_cache_evictions},
            {"post_seed_owned_gpu_resident_bytes_released",
                stats.post_seed_owned_gpu_resident_bytes_released},
            {"post_seed_owned_cpu_index_releases",
                stats.post_seed_owned_cpu_index_releases},
            {"post_seed_owned_cpu_position_bytes_released",
                stats.post_seed_owned_cpu_position_bytes_released},
            {"saturated", stats.is_saturated},
        }},
    }.dump();
}

[[nodiscard]] std::string serialize_endpoint_qualification_sample(
    std::string_view algorithm,
    const parameter_map& /*params*/,
    const endpoint_qualification_input& /*input*/,
    const simd_chaining_endpoint_qualification& output)
{
    const auto endpoint_json = [](const endpoint_coordinates_stats& endpoint) {
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
            {"bounded_fixture_validation_accepted",
                comparison.bounded_fixture_stats_consistent},
            {"deterministic_rescue_fixture",
                comparison.deterministic_rescue_fixture},
            {"bounded_route_validation_accepted",
                comparison.bounded_route_stats_consistent},
            {"exact_route_validation_accepted",
                comparison.exact_route_stats_consistent},
            {"minimap2_route_validation_accepted",
                comparison.minimap2_route_stats_consistent},
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
        {"validation_accepted", output.has_consistent_execution_stats()},
        {"rescue_stats_accepted", output.rescue_stats_accepted()},
        {"exact_route_validation_accepted",
            output.exact_route_stats_consistent},
        {"bounded_route_validation_accepted",
            output.bounded_route_stats_consistent},
        {"minimap2_route_validation_accepted",
            output.minimap2_route_stats_consistent},
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
        {"exact_policy_validation", simd_chaining_stats_json(
            output.exact_policy_stats,
            output.exact_policy_stats_available,
            simd_chaining_policy::exact_visited,
            output.exact_policy_stats_available)},
        {"bounded_policy_validation", simd_chaining_stats_json(
            output.bounded_policy_stats,
            output.bounded_policy_stats_available,
            simd_chaining_policy::bounded_batch_approximate,
            output.bounded_policy_stats_available)},
        {"fixture_comparisons", std::move(comparisons)},
    }.dump();
}

}  // namespace

HWY_AFTER_NAMESPACE();

export namespace bioseq::bench::alignment {

int run(int argc, char** argv) {
    configure_logging(true);

    const std::string config_path{BIOSEQ_BENCH_CONFIG_PATH};
    const std::string report_dir{BIOSEQ_BENCH_REPORT_DIR};
    const std::string data_dir{BIOSEQ_BENCH_DATA_DIR};

    using bioseq::bench::framework::parameter_map;
    using bioseq::bench::framework::run_constraints;
    using bioseq::bench::framework::create_benchmark;

    // Mandatory `std::function<...>` casts (README §3e) — `create_benchmark`'s
    // template-arg deduction won't bind a lambda or function pointer to a
    // `std::function` parameter without help. Reused across the six blocks
    // below since all six share the same I/O types and lambdas.
    auto estimate = std::function<run_constraints(
        std::string_view, std::string_view, simdjson::dom::element)>{
            estimate_aligner_resources};
    auto build = std::function<aligner_input(
        std::string_view, std::string_view, simdjson::dom::element,
        const run_constraints&)>{build_input};
    auto run = std::function<aligner_output(
        std::string_view, const parameter_map&, const aligner_input&)>{run_aligner};
    auto run_strict_xxl = std::function<aligner_output(
        std::string_view, const parameter_map&, const aligner_input&)>{
            run_aligner_strict_xxl};
    auto run_strict_xxl_paired = std::function<aligner_output(
        std::string_view, const parameter_map&, const aligner_input&)>{
            run_aligner_strict_xxl_paired};
    auto eval = std::function<std::map<std::string, double>(
        std::string_view, const parameter_map&,
        const aligner_input&, const aligner_output&)>{evaluate_aligner};
    auto eval_prefilter_pipeline =
        std::function<std::map<std::string, double>(
            std::string_view, const parameter_map&,
            const aligner_input&, const aligner_output&)>{
                evaluate_prefilter_pipeline};
    auto serialize_paired = std::function<std::string(
        std::string_view, const parameter_map&,
        const aligner_input&, const aligner_output&)>{
            serialize_xxl_paired_sample};
    auto estimate_endpoint = std::function<run_constraints(
        std::string_view, std::string_view, simdjson::dom::element)>{
            estimate_endpoint_qualification_resources};
    auto build_endpoint = std::function<endpoint_qualification_input(
        std::string_view, std::string_view, simdjson::dom::element,
        const run_constraints&)>{build_endpoint_qualification_input};
    auto run_endpoint = std::function<simd_chaining_endpoint_qualification(
        std::string_view, const parameter_map&,
        const endpoint_qualification_input&)>{
            run_simd_chaining_endpoint_qualification};
    auto evaluate_endpoint = std::function<std::map<std::string, double>(
        std::string_view, const parameter_map&,
        const endpoint_qualification_input&,
        const simd_chaining_endpoint_qualification&)>{
            evaluate_simd_chaining_endpoint_qualification};
    auto serialize_endpoint = std::function<std::string(
        std::string_view, const parameter_map&,
        const endpoint_qualification_input&,
        const simd_chaining_endpoint_qualification&)>{
            serialize_endpoint_qualification_sample};

    return bioseq::bench::framework::run(
        argc, argv, report_dir, data_dir,
        bioseq::bench::prefilters::make_prefilter_kernel_benchmark(
            config_path),
        bioseq::bench::prefilters::make_prefilter_small_seed_benchmark(
            config_path),
        create_benchmark<aligner_input, aligner_output>(
            "SneakySnake full-pipeline regional impact",
            "bench_prefilter_pipeline",
            config_path, estimate, build, run, eval_prefilter_pipeline),
        create_benchmark<aligner_input, aligner_output>(
            "Aligner full grid (length × error × algorithm)",
            "bench_aligners_grid", config_path, estimate, build, run, eval),
        create_benchmark<aligner_input, aligner_output>(
            "MinimizerRepr storage exercise (K=7 / K=27)",
            "bench_aligners_repr_storage", config_path, estimate, build, run, eval),
        create_benchmark<aligner_input, aligner_output>(
            "RMQ chaining LONG/CCS regression repro",
            "bench_aligners_rmq_repro", config_path, estimate, build, run, eval),
        create_benchmark<aligner_input, aligner_output>(
            "Curated correctness probes",
            "bench_aligners_curated", config_path, estimate, build, run, eval),
        create_benchmark<aligner_input, aligner_output>(
            "XXL_HIFI SIMD chaining iteration diagnostic",
            "bench_aligners_xxl_hifi_chain_focus",
            config_path, estimate, build, run_strict_xxl, eval,
            {}, serialize_paired),
        create_benchmark<
            endpoint_qualification_input,
            simd_chaining_endpoint_qualification>(
                "SIMD chaining bounded endpoint qualification",
                "bench_aligners_simd_chaining_endpoint_quality",
                config_path,
                estimate_endpoint,
                build_endpoint,
                run_endpoint,
                evaluate_endpoint,
                {},
                serialize_endpoint),
        create_benchmark<aligner_input, aligner_output>(
            "XXL_HIFI report_76 relative-performance gate",
            "bench_aligners_xxl_hifi_relative",
            config_path, estimate, build, run_strict_xxl, eval,
            {}, serialize_paired),
        create_benchmark<aligner_input, aligner_output>(
            "XXL_HIFI bounded ntHash SIMD validation and speed gate",
            "bench_bounded_nthash_xxl_hifi_validation",
            config_path, estimate, build, run_strict_xxl_paired, eval,
            {}, serialize_paired),
        create_benchmark<aligner_input, aligner_output>(
            "XXL_HIFI paired relative-performance companion",
            "bench_aligners_xxl_hifi_paired",
            config_path, estimate, build, run_strict_xxl_paired, eval,
            {}, serialize_paired));
}

}  // namespace bioseq::bench::alignment
