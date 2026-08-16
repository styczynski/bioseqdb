/**
 * @file typed_support_parts/policy_stats.cppm
 * @brief Defines shared dispatch policy, execution statistics, and persisted GPU ledger helpers.
 */
module;

#include <algorithm>
#include <array>
#include <bioseq/pg_compat.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <libassert/assert.hpp>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

export module bioseq.postgres.scheduler.dispatch_processor.typed_support:policy_stats;

import bioseq.postgres.scheduler.dispatch_processor;
import bioseq.highway.dispatch_contract;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.common.hashing;
import bioseq.common.seqan_adapter;
import bioseq.postgres.support;
import bioseq.postgres.configuration.core;
import bioseq.postgres.configuration.scheduler;
import bioseq.indexing.type;
import bioseq.indexing.traits;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_sequence;
import bioseq.indexing.gpu_minimizer_index;
import bioseq.postgres.storage.indexing.chunked_storage;
import bioseq.postgres.storage.indexing.handles;
import bioseq.postgres.storage.indexing.residency;
import bioseq.postgres.storage.indexing.sorted_runs;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.chaining.vulkan;
import bioseq.sequences.cigar;
import bioseq.pipeline.minimap_port;
import bioseq.pipeline.pipe;
import bioseq.gpu.vulkan.runtime;
import bioseq.postgres.types.compact_sequence;
import bioseq.instrumentation.clock;
import bioseq.instrumentation.configuration;
import bioseq.instrumentation.descriptor;
import bioseq.instrumentation.profiler_state;
import bioseq.instrumentation.region;
import bioseq.instrumentation.runtime;
import bioseq.postgres.storage.manager;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.storage.layout;
import bioseq.postgres.storage.scanner;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.pipeline_ops;
import bioseq.postgres.scheduler.supervisor;
import bioseq.postgres.scheduler.supervisor.vulkan;

export namespace bioseq::simd::typed_dispatch::typed_support_detail {

class fragment_anchor_wave_completion final {
  private:
    bioseq::indexing::query_wave_scratch* scratch_ = nullptr;

  public:
    explicit fragment_anchor_wave_completion(bioseq::indexing::query_wave_scratch& scratch) noexcept
        : scratch_(&scratch) {}

    ~fragment_anchor_wave_completion() noexcept {
        scratch_->complete_wave_after_join();
    }

    fragment_anchor_wave_completion(const fragment_anchor_wave_completion&) = delete;
    fragment_anchor_wave_completion& operator=(const fragment_anchor_wave_completion&) = delete;
};

inline constexpr std::string_view minimizers_per_fragment_index_name =
    bioseq::indexing::minimizer_hashmap_fragment_index::index_name;
inline constexpr std::string_view minimizers_per_sequence_index_name =
    bioseq::indexing::index_descriptor<bioseq::indexing::direct_minimizer_bucket_index>::index_name;
inline constexpr sequence_size_t postgres_segmentation_threshold_bp = 256'000'000;
inline constexpr sequence_size_t opportunistic_index_minimum_bp = 4096;

// GetConfigOptionByName returns a palloc-owned string and can ereport.  Keep the
// PostgreSQL call and release in separate PG leaves; copy only after the getter
// leaf has restored PG state so the owning std::string always unwinds normally.
[[nodiscard]] inline std::optional<std::string> pg_guc_value_as_cpp(const char* name) {
    char* raw_value = bioseq::postgres::support::invoke_pg_as_cpp(
        [name] { return GetConfigOptionByName(name, nullptr, true); });
    if (raw_value == nullptr) {
        return std::nullopt;
    }

    std::string copied_value;
    try {
        copied_value.assign(raw_value);
    } catch (...) {
        bioseq::postgres::support::invoke_pg_as_cpp([raw_value] { pfree(raw_value); });
        throw;
    }
    bioseq::postgres::support::invoke_pg_as_cpp([raw_value] { pfree(raw_value); });
    return copied_value;
}

[[nodiscard]] inline bool
pg_vulkan_seed_lookup_selected(const minimap2_parameters& params) noexcept {
    return params.vulkan_enabled && params.vulkan_seed_hits_enabled;
}

[[nodiscard]] inline std::uint64_t pg_gpu_index_resident_limit_bytes() {
    bioseq::gpu::vulkan::runtime& runtime =
        bioseq::postgres::scheduler::ensure_pg_vulkan_runtime_ready();
    std::uint64_t limit = runtime.is_usable() ? runtime.features().device_local_bytes / 2u
                                              : (std::uint64_t{512} << 20);
    if (limit < (std::uint64_t{512} << 20)) {
        limit = std::uint64_t{512} << 20;
    }
    if (limit > (std::uint64_t{4} << 30)) {
        limit = std::uint64_t{4} << 30;
    }
    return limit;
}

[[nodiscard]] inline bioseq::indexing::gpu_index_cache_identity
pg_gpu_index_identity(std::uint64_t rowno,
                      std::string_view index_name,
                      sequence_size_t clip_lo_bp,
                      sequence_size_t clip_hi_bp,
                      bioseq::minimizers::minimizer_sketch_backend sketch_backend) noexcept {
    const auto scheme = bioseq::indexing::minimizer_scheme_identity_for_backend(sketch_backend);
    std::uint64_t identity_hash = bioseq::indexing::splitmix64(rowno);
    identity_hash ^=
        bioseq::indexing::splitmix64(static_cast<std::uint64_t>(clip_lo_bp) + (identity_hash << 1));
    identity_hash ^=
        bioseq::indexing::splitmix64(static_cast<std::uint64_t>(clip_hi_bp) + (identity_hash << 1));
    for (const unsigned char index_character : index_name) {
        identity_hash ^= bioseq::indexing::splitmix64(static_cast<std::uint64_t>(index_character) +
                                                      (identity_hash << 1));
    }
    identity_hash ^= bioseq::indexing::splitmix64(static_cast<std::uint64_t>(sketch_backend) +
                                                  (identity_hash << 1));
    identity_hash ^=
        bioseq::indexing::splitmix64(static_cast<std::uint64_t>(scheme.id) + (identity_hash << 1));
    identity_hash ^= bioseq::indexing::splitmix64(static_cast<std::uint64_t>(scheme.revision) +
                                                  (identity_hash << 1));
    if (identity_hash == 0) {
        identity_hash = 0x9e3779b97f4a7c15ULL;
    }
    return bioseq::indexing::gpu_index_cache_identity{.assembly_identity = identity_hash};
}

[[nodiscard]] inline bioseq::indexing::gpu_index_cache_identity
pg_gpu_index_identity(const bioseq::storage::indexing::assembled_index_key& key) noexcept {
    std::uint64_t identity_hash =
        bioseq::indexing::splitmix64(static_cast<std::uint64_t>(key.db_oid));
    identity_hash ^= bioseq::indexing::splitmix64(static_cast<std::uint64_t>(key.user_relid) +
                                                  (identity_hash << 1));
    identity_hash ^= bioseq::indexing::splitmix64(static_cast<std::uint64_t>(key.tablespace_oid) +
                                                  (identity_hash << 1));
    identity_hash ^= bioseq::indexing::splitmix64(static_cast<std::uint64_t>(key.main_relfilenode) +
                                                  (identity_hash << 1));
    identity_hash ^= bioseq::indexing::splitmix64(
        static_cast<std::uint64_t>(static_cast<std::uint16_t>(key.column_attno)) +
        (identity_hash << 1));
    identity_hash ^= bioseq::indexing::splitmix64(key.index_name_hash + (identity_hash << 1));
    identity_hash ^= bioseq::indexing::splitmix64(static_cast<std::uint64_t>(key.sketch_backend) +
                                                  (identity_hash << 1));
    identity_hash ^= bioseq::indexing::splitmix64(
        static_cast<std::uint64_t>(key.minimizer_scheme.id) + (identity_hash << 1));
    identity_hash ^= bioseq::indexing::splitmix64(
        static_cast<std::uint64_t>(key.minimizer_scheme.revision) + (identity_hash << 1));
    identity_hash ^= bioseq::indexing::splitmix64(key.query_key + (identity_hash << 1));
    identity_hash ^= bioseq::indexing::splitmix64(key.query_signature + (identity_hash << 1));
    identity_hash ^= bioseq::indexing::splitmix64(static_cast<std::uint64_t>(key.clip_lo_bp) +
                                                  (identity_hash << 1));
    identity_hash ^= bioseq::indexing::splitmix64(static_cast<std::uint64_t>(key.clip_hi_bp) +
                                                  (identity_hash << 1));
    if (identity_hash == 0) {
        identity_hash = 0x9e3779b97f4a7c15ULL;
    }
    return bioseq::indexing::gpu_index_cache_identity{.assembly_identity = identity_hash};
}

enum class deferred_minimizer_index_kind : std::uint8_t {
    per_fragment,
    per_sequence,
};

struct deferred_minimizer_persist_choice {
    deferred_minimizer_index_kind kind = deferred_minimizer_index_kind::per_fragment;
    std::string_view index_name = minimizers_per_fragment_index_name;
};

[[nodiscard]] deferred_minimizer_index_kind
deferred_kind_for_index(std::string_view index_name) noexcept {
    return index_name == minimizers_per_sequence_index_name
               ? deferred_minimizer_index_kind::per_sequence
               : deferred_minimizer_index_kind::per_fragment;
}

[[nodiscard]] std::optional<deferred_minimizer_persist_choice>
choose_deferred_minimizer_persist_index(const batch_processor::scanner_pg_context& ctx) {
    if (!OidIsValid(ctx.user_relid) || ctx.mmsearch_seq_attno <= 0) {
        return std::nullopt;
    }

    constexpr auto minimizers = bioseq::indexing::k_minimizer_index_names;
    const auto policy = bioseq::storage::indexing::resolve_minimizer_persistence_policy(
        ctx.user_relid, ctx.main_relfilenode, ctx.mmsearch_seq_attno, ctx.snapshot);

    for (std::size_t policy_index = 0; policy_index < minimizers.size(); ++policy_index) {
        if (policy.enabled[policy_index] && policy.update_on_insert[policy_index]) {
            return deferred_minimizer_persist_choice{
                .kind = deferred_kind_for_index(minimizers[policy_index]),
                .index_name = minimizers[policy_index],
            };
        }
    }
    return std::nullopt;
}

/*
 * Scale k/w for the small-query regime typical inside Postgres — short
 * motifs vs per-row sequences under 1 kb. Runs once per row before the
 * core pipeline. Factored out of the hot lambda so all code paths
 * (recompute vs prebuilt index) share a single size-heuristic knob and
 * so EXPLAIN can replay the same derivation for its param printout.
 *
 * The heuristic only fires when the user left the fields at their
 * minimap2 defaults. Any aligner_options() override or non-default GUC
 * value short-circuits the adjustment so we never silently overwrite a
 * user-supplied tuning value.
 */
[[nodiscard]] inline minimap2_parameters apply_size_heuristic(const minimap2_parameters& base,
                                                              size_t min_bp) {
    return batch_processor::effective_params_for_pair(base, min_bp);
}

[[nodiscard]] inline bool selected_vulkan_execution(const minimap2_parameters& params) noexcept {
    return pipe::vulkan_components_requested(params);
}

[[nodiscard]] inline bool selected_vulkan_accelerator(const minimap2_parameters& params) noexcept {
    return pipe::vulkan_accelerator_requested(params);
}

[[nodiscard]] inline bool
selected_vulkan_coordinate_prereqs_active(const minimap2_parameters& params) noexcept {
    return pipe::complete_all_gpu_tuple_selected(params);
}

[[nodiscard]] inline ::bioseq::pipeline_stats::executed_backend
backend_from_selected_params(const minimap2_parameters& params) noexcept {
    const bool full_vulkan_pipeline = pipe::complete_all_gpu_tuple_selected(params);
    if (full_vulkan_pipeline) {
        return ::bioseq::pipeline_stats::executed_backend::vulkan_gpu;
    }
    if (selected_vulkan_execution(params)) {
        return ::bioseq::pipeline_stats::executed_backend::mixed_invalid;
    }
    if (params.minimizer_algorithm == minimizer_algorithm::two_stack_nthash &&
        params.chaining_algorithm == chaining_algorithm::simd_minimap2 &&
        params.base_alignment_algorithm == base_alignment_algorithm::block_aligner) {
        return ::bioseq::pipeline_stats::executed_backend::simd_cpu;
    }
    if (params.minimizer_algorithm == minimizer_algorithm::bounded_nthash_simd &&
        params.chaining_algorithm == chaining_algorithm::simd_minimap2 &&
        params.base_alignment_algorithm == base_alignment_algorithm::block_aligner) {
        return ::bioseq::pipeline_stats::executed_backend::simd_cpu;
    }
    if (params.minimizer_algorithm == minimizer_algorithm::scalar &&
        params.chaining_algorithm == chaining_algorithm::scalar_minimap2 &&
        params.base_alignment_algorithm == base_alignment_algorithm::ksw2) {
        return ::bioseq::pipeline_stats::executed_backend::scalar_cpu;
    }
    return ::bioseq::pipeline_stats::executed_backend::mixed_invalid;
}

[[nodiscard]] inline ::bioseq::pipeline_stats::simd_chaining_policy_identity
requested_simd_chaining_policy_identity(const minimap2_parameters& params) noexcept {
    using identity_type = ::bioseq::pipeline_stats::simd_chaining_policy_identity;
    if (params.chaining_algorithm != chaining_algorithm::simd_minimap2) {
        return identity_type::none;
    }
    switch (params.simd_chaining_policy) {
    case simd_chaining_policy::exact_visited:
        return identity_type::exact_visited;
    case simd_chaining_policy::bounded_batch_approximate:
        return identity_type::bounded_batch_approximate;
    }
    return identity_type::mixed_invalid;
}

[[nodiscard]] inline ::bioseq::pipeline_stats::simd_chaining_policy_identity
observed_simd_chaining_policy_identity(const match_result_collection* execution_stats) noexcept {
    using identity_type = ::bioseq::pipeline_stats::simd_chaining_policy_identity;
    if (execution_stats == nullptr || !execution_stats->simd_chaining_execution_stats_available) {
        return identity_type::none;
    }
    const auto& policy = execution_stats->simd_chaining_stats.policy;
    // identity_type answers only which named policy emitted operation calls.
    // Rejections, saturation, forbidden scalar work, and missing vector work
    // are retained as separate fail-closed validation stats below; folding them into
    // identity would mislabel a rejected exact (or bounded) execution as a
    // mixed-policy execution and hide the actual reason for rejection.
    const bool exact = policy.exact_visited_calls != 0;
    const bool bounded = policy.bounded_batch_approximate_calls != 0;
    if (exact == bounded) {
        // Neither identity, or an impossible validation accepting both identities,
        // is not a policy absence. Preserve it as mixed-invalid state.
        return identity_type::mixed_invalid;
    }
    return exact ? identity_type::exact_visited : identity_type::bounded_batch_approximate;
}

[[nodiscard]] inline ::bioseq::pipeline_stats::simd_alignment_algorithm_identity
requested_simd_alignment_algorithm_identity(const minimap2_parameters& params) noexcept {
    using identity_type = ::bioseq::pipeline_stats::simd_alignment_algorithm_identity;
    return backend_from_selected_params(params) ==
                   ::bioseq::pipeline_stats::executed_backend::simd_cpu
               ? identity_type::block_aligner
               : identity_type::none;
}

[[nodiscard]] inline ::bioseq::pipeline_stats::simd_alignment_algorithm_identity
observed_simd_alignment_algorithm_identity(
    const match_result_collection* execution_stats) noexcept {
    using identity_type = ::bioseq::pipeline_stats::simd_alignment_algorithm_identity;
    if (execution_stats == nullptr)
        return identity_type::none;

    const bool block_present = execution_stats->block_aligner_execution_stats_available;
    const auto& block = execution_stats->block_aligner_stats;
    const auto& scalar = execution_stats->scalar_pipeline_stats;
    const bool scalar_alignment_observed =
        execution_stats->scalar_pipeline_execution_stats_available &&
        (scalar.ksw2_left_calls != 0 || scalar.ksw2_gap_calls != 0 || scalar.ksw2_right_calls != 0);
    const bool forbidden_alignment_observed =
        scalar_alignment_observed || execution_stats->ksw2_batched_execution_stats_available ||
        (block_present &&
         (block.forbidden_ksw2_calls != 0 || block.forbidden_scalar_biological_fallbacks != 0));
    if (block_present && forbidden_alignment_observed) {
        return identity_type::mixed_invalid;
    }
    return block_present ? identity_type::block_aligner : identity_type::none;
}

inline void add_operational_counter(::bioseq::pipeline_stats::pipeline_execution_stats& stats,
                                    std::uint64_t& destination,
                                    const std::uint64_t increment = 1) noexcept {
    stats.operational.are_stats_saturated =
        ::bioseq::pipeline_stats::merge_counter(destination, increment) ||
        stats.operational.are_stats_saturated;
}

inline void add_stage_host_time(::bioseq::pipeline_stats::pipeline_execution_stats& stats,
                                const ::bioseq::pipeline_stats::pipeline_stage stage,
                                const std::uint64_t elapsed_ns) noexcept {
    auto& destination =
        stats.operational.timing[::bioseq::pipeline_stats::stage_index(stage)].host_wall_ns;
    add_operational_counter(stats, destination, elapsed_ns);
}

[[nodiscard]] inline std::uint64_t dispatch_host_now_ns() noexcept {
    const auto now = bioseq::instrumentation::clock::now_ns();
    return now > 0 ? static_cast<std::uint64_t>(now) : 1u;
}

[[nodiscard]] inline std::uint64_t
elapsed_dispatch_host_ns(const std::uint64_t started_ns) noexcept {
    const std::uint64_t finished_ns = dispatch_host_now_ns();
    const std::uint64_t elapsed_ns = finished_ns >= started_ns ? finished_ns - started_ns : 0u;
    return elapsed_ns > 0 ? elapsed_ns : 1u;
}

inline void
record_block_aligner_execution_stats(::bioseq::pipeline_stats::pipeline_execution_stats& stats,
                                     const match_result_collection* execution_stats) noexcept {
    if (execution_stats == nullptr || !execution_stats->block_aligner_execution_stats_available) {
        return;
    }
    const auto& block_stats = execution_stats->block_aligner_stats;
    ::bioseq::pipeline_stats::block_aligner_stage_stats stage_stats{};
    stage_stats.is_available = true;
    stage_stats.is_accepted = block_stats.is_internally_consistent();
    stage_stats.executed_calls = block_stats.executed_calls();
    stage_stats.nonempty_calls = block_stats.nonempty_calls;
    stage_stats.nonempty_calls_with_simd = block_stats.nonempty_calls_with_simd;
    stage_stats.simd_dp_vector_batches = block_stats.simd_dp_vector_batches;
    stage_stats.simd_dp_cells = block_stats.simd_dp_cells;
    stage_stats.forbidden_ksw2_calls = block_stats.forbidden_ksw2_calls;
    stage_stats.forbidden_scalar_biological_fallbacks =
        block_stats.forbidden_scalar_biological_fallbacks;
    stage_stats.is_saturated = block_stats.is_saturated;
    stats.operational.are_stats_saturated =
        stats.operational.block_aligner_stats.merge(stage_stats) ||
        stats.operational.are_stats_saturated;
}

inline void
record_unattested_job(::bioseq::pipeline_stats::pipeline_execution_stats& stats,
                      const minimap2_parameters& params,
                      const ::bioseq::pipeline_stats::target_index_source requested_source,
                      const ::bioseq::pipeline_stats::target_index_source actual_source,
                      const batch_processor::scanner_pg_context* scanner_pg_ctx,
                      const std::size_t row_index,
                      const bool per_row_query_mode,
                      const std::uint64_t broadcast_query_ordinal,
                      const match_result_collection* execution_stats,
                      const bool consumed_shared_cached_query_stats,
                      bool& cached_query_minimizer_stats_accounted) noexcept {
    if (execution_stats != nullptr) {
        if (execution_stats->prefilter_execution_stats_available) {
            stats.operational.record_prefilter_execution_stats(execution_stats->prefilter_stats);
        } else if (execution_stats->prefilter_stats !=
                   decltype(execution_stats->prefilter_stats){}) {
            auto malformed_prefilter_stats = execution_stats->prefilter_stats;
            malformed_prefilter_stats.is_saturated = true;
            stats.operational.record_prefilter_execution_stats(malformed_prefilter_stats);
        }
        // CPU/SIMD stages write into the operation-local result collection;
        // consume that delta once here, after any parallel row has joined.
        // chain_stage is intentionally inclusive, so chain_backtrack remains
        // zero. The left_extension timing slot likewise owns the coarse
        // alignment-through-result interval; gap/right remain available for a
        // later measured split. These owners never overlap.
        // query_sketch is only the per-operation sketch recorded below or by
        // minimizer_stage; the batch join query-union preparation that precedes
        // persisted payload delivery has a different lifetime owner and must
        // be published separately rather than charged here twice.
        using stage_type = ::bioseq::pipeline_stats::pipeline_stage;
        add_stage_host_time(
            stats, stage_type::query_sketch, execution_stats->query_sketch_host_wall_ns);
        add_stage_host_time(stats,
                            stage_type::persisted_index_decode,
                            execution_stats->persisted_index_decode_host_wall_ns);
        add_stage_host_time(
            stats, stage_type::seed_lookup, execution_stats->seed_lookup_host_wall_ns);
        add_stage_host_time(
            stats, stage_type::chain_score, execution_stats->chain_score_host_wall_ns);
        add_stage_host_time(
            stats, stage_type::region_generation, execution_stats->region_generation_host_wall_ns);
        add_stage_host_time(
            stats, stage_type::left_extension, execution_stats->alignment_result_host_wall_ns);
        stats.operational.are_stats_saturated = stats.operational.are_stats_saturated ||
                                                execution_stats->is_host_stage_timing_saturated;
        if (execution_stats->fragment_native_query_stats_available) {
            stats.operational.fragment_native_query_stats.merge(
                execution_stats->fragment_native_query_stats);
            stats.operational.are_stats_saturated =
                stats.operational.are_stats_saturated ||
                stats.operational.fragment_native_query_stats.saturated;
        }
    }
    ::bioseq::pipeline_stats::pipeline_job_execution_stats job{};
    job.requested_backend = backend_from_selected_params(params);
    job.requested_simd_chaining_policy = requested_simd_chaining_policy_identity(params);
    job.observed_simd_chaining_policy = observed_simd_chaining_policy_identity(execution_stats);
    job.requested_simd_alignment_algorithm = requested_simd_alignment_algorithm_identity(params);
    job.observed_simd_alignment_algorithm =
        observed_simd_alignment_algorithm_identity(execution_stats);
    const bool selected_all_simd =
        job.requested_backend == ::bioseq::pipeline_stats::executed_backend::simd_cpu;
    const bool selected_all_scalar =
        job.requested_backend == ::bioseq::pipeline_stats::executed_backend::scalar_cpu;
    const bool require_target_sketch =
        actual_source == ::bioseq::pipeline_stats::target_index_source::recomputed;
    const bool accepted_all_simd =
        selected_all_simd && execution_stats != nullptr &&
        execution_stats->is_all_simd_execution_consistent(params.minimizer_algorithm,
                                                          params.simd_chaining_policy,
                                                          require_target_sketch,
                                                          /*require_nonempty_vector_work=*/true);
    const bool accepted_all_scalar =
        selected_all_scalar && execution_stats != nullptr &&
        execution_stats->is_all_scalar_execution_consistent(require_target_sketch,
                                                            /*require_alignment=*/params.do_cigar);
    if (execution_stats != nullptr && execution_stats->simd_chaining_execution_stats_available) {
        const auto& policy = execution_stats->simd_chaining_stats.policy;
        add_operational_counter(
            stats, stats.operational.simd_chaining_exact_policy_calls, policy.exact_visited_calls);
        add_operational_counter(stats,
                                stats.operational.simd_chaining_bounded_policy_calls,
                                policy.bounded_batch_approximate_calls);
        add_operational_counter(stats,
                                stats.operational.simd_chaining_bounded_score_batches,
                                policy.bounded_score_batches);
        add_operational_counter(stats,
                                stats.operational.simd_chaining_bounded_cutoff_stops,
                                policy.bounded_cutoff_stops);
        add_operational_counter(stats,
                                stats.operational.simd_chaining_bounded_candidates_skipped,
                                policy.bounded_candidates_skipped);
        add_operational_counter(
            stats, stats.operational.simd_chaining_policy_rejections, policy.policy_rejections);
        stats.operational.are_simd_chaining_policy_stats_saturated =
            stats.operational.are_simd_chaining_policy_stats_saturated || policy.saturated;
    }
    record_block_aligner_execution_stats(stats, execution_stats);
    const auto record_exact_nthash_vector_activity = [&stats](const auto& range_stats) noexcept {
        // Continuation batches and whole-range resident vector_stream calls
        // remain separate stats. A resident call is authentic vector work,
        // but it is not one continuation batch.
        add_operational_counter(
            stats, stats.operational.nthash_vector_batches, range_stats.vector_batches);
        add_operational_counter(stats,
                                stats.operational.nthash_resident_vector_stream_kernel_calls,
                                range_stats.resident_vector_stream_kernel_calls);
    };
    // A broadcast persisted-query sketch is one physical operation shared by
    // logical jobs. Account it at its first validation-approved consumer even when
    // that job later has no chain/alignment; downstream route attestation is
    // a separate question. Recompute rows cannot enter this block because
    // their consumed_shared_cached_query_stats flag is false.
    const bool accepted_shared_query_sketch =
        consumed_shared_cached_query_stats && !cached_query_minimizer_stats_accounted &&
        execution_stats != nullptr &&
        (selected_all_simd ? execution_stats->is_selected_simd_sketch_execution_consistent(
                                 params.minimizer_algorithm,
                                 /*require_target_sketch=*/false,
                                 /*require_nonempty_vector_work=*/false)
                           : selected_all_scalar &&
                                 execution_stats->is_selected_scalar_sketch_execution_consistent(
                                     /*require_target_sketch=*/false,
                                     /*require_nonempty_work=*/false));
    if (accepted_shared_query_sketch) {
        if (params.minimizer_algorithm == minimizer_algorithm::two_stack_nthash) {
            record_exact_nthash_vector_activity(execution_stats->exact_simd_query_execution_stats);
        } else if (params.minimizer_algorithm == minimizer_algorithm::bounded_nthash_simd) {
            add_operational_counter(
                stats,
                stats.operational.nthash_vector_batches,
                execution_stats->bounded_nthash_query_execution_stats.packed_decode_batches);
        }
        cached_query_minimizer_stats_accounted = true;
    }
    // A SIMD identity is published only after the per-job collection proves
    // two_stack_nthash sketching, local SIMD score/backtracking, and Block alignment.
    // Every partial, missing, or inconsistent validation remains mixed-invalid.
    job.actual_backend = accepted_all_simd ? ::bioseq::pipeline_stats::executed_backend::simd_cpu
                         : accepted_all_scalar
                             ? ::bioseq::pipeline_stats::executed_backend::scalar_cpu
                             : ::bioseq::pipeline_stats::executed_backend::mixed_invalid;
    job.requested_target_index_source = requested_source;
    job.actual_target_index_source = actual_source;
    if (actual_source == ::bioseq::pipeline_stats::target_index_source::persisted &&
        scanner_pg_ctx != nullptr) {
        job.persisted_generation_id = scanner_pg_ctx->persisted_generation_ids[row_index];
        job.persisted_source_identity = scanner_pg_ctx->persisted_source_identities[row_index];
    }
    const std::size_t effective_workers =
        bioseq::parallelism::global_pool::instance().maximum_concurrency();
    job.environment.effective_cpu_workers =
        effective_workers > static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())
            ? (std::numeric_limits<std::uint32_t>::max)()
            : static_cast<std::uint32_t>(effective_workers);
    job.environment.record_highway_target(params.highway_dispatch, params.highway_executed_target);
    if (scanner_pg_ctx != nullptr && scanner_pg_ctx->target_rownos[row_index] >= 0) {
        job.logical_target_row =
            static_cast<std::uint64_t>(scanner_pg_ctx->target_rownos[row_index]);
    } else {
        job.logical_target_row = static_cast<std::uint64_t>(row_index);
    }
    job.query_ordinal = scanner_pg_ctx != nullptr && scanner_pg_ctx->explicit_query_ordinals
                            ? scanner_pg_ctx->query_ordinals[row_index]
                        : per_row_query_mode ? static_cast<std::uint64_t>(row_index)
                                             : broadcast_query_ordinal;
    if (accepted_all_simd || accepted_all_scalar) {
        const auto mark = [&job](::bioseq::pipeline_stats::pipeline_stage stage,
                                 ::bioseq::pipeline_stats::execution_engine engine) noexcept {
            auto& coverage = job.stages.coverage[::bioseq::pipeline_stats::stage_index(stage)];
            coverage.eligible_jobs = 1;
            coverage.executed_jobs[::bioseq::pipeline_stats::engine_index(engine)] = 1;
        };
        using stage_type = ::bioseq::pipeline_stats::pipeline_stage;
        using engine_type = ::bioseq::pipeline_stats::execution_engine;
        const engine_type biological_engine =
            accepted_all_scalar ? engine_type::scalar_cpu : engine_type::simd_cpu;
        mark(stage_type::query_sketch, biological_engine);
        if (require_target_sketch) {
            mark(stage_type::target_sketch, biological_engine);
            mark(stage_type::target_index_build, engine_type::scalar_cpu);
        } else {
            mark(stage_type::persisted_index_decode, engine_type::host_io);
            mark(stage_type::partition_selection, engine_type::scalar_cpu);
        }
        mark(stage_type::seed_lookup, engine_type::scalar_cpu);
        mark(stage_type::chain_score, biological_engine);
        mark(stage_type::chain_backtrack, biological_engine);
        if (accepted_all_simd) {
            const auto& block = execution_stats->block_aligner_stats;
            if (block.left_executed != 0) {
                mark(stage_type::left_extension, engine_type::simd_cpu);
            }
            if (block.gap_executed != 0) {
                mark(stage_type::gap_extension, engine_type::simd_cpu);
            }
            if (block.right_executed != 0) {
                mark(stage_type::right_extension, engine_type::simd_cpu);
            }
        } else {
            const auto& scalar = execution_stats->scalar_pipeline_stats;
            if (scalar.ksw2_left_calls != 0) {
                mark(stage_type::left_extension, engine_type::scalar_cpu);
            }
            if (scalar.ksw2_gap_calls != 0) {
                mark(stage_type::gap_extension, engine_type::scalar_cpu);
            }
            if (scalar.ksw2_right_calls != 0) {
                mark(stage_type::right_extension, engine_type::scalar_cpu);
            }
        }

        if (accepted_all_simd &&
            params.minimizer_algorithm == minimizer_algorithm::two_stack_nthash) {
            if (!consumed_shared_cached_query_stats) {
                record_exact_nthash_vector_activity(
                    execution_stats->exact_simd_query_execution_stats);
            }
            if (execution_stats->exact_simd_target_execution_stats_available) {
                record_exact_nthash_vector_activity(
                    execution_stats->exact_simd_target_execution_stats);
            }
        } else if (accepted_all_simd &&
                   params.minimizer_algorithm == minimizer_algorithm::bounded_nthash_simd) {
            if (!consumed_shared_cached_query_stats) {
                add_operational_counter(
                    stats,
                    stats.operational.nthash_vector_batches,
                    execution_stats->bounded_nthash_query_execution_stats.packed_decode_batches);
            }
            if (execution_stats->bounded_nthash_target_execution_stats_available) {
                add_operational_counter(
                    stats,
                    stats.operational.nthash_vector_batches,
                    execution_stats->bounded_nthash_target_execution_stats.packed_decode_batches);
            }
        }
        if (accepted_all_simd) {
            add_operational_counter(stats,
                                    stats.operational.simd_chain_vector_batches,
                                    execution_stats->simd_chaining_stats.score_vector_batches);
            add_operational_counter(
                stats,
                stats.operational.simd_chain_backtrack_vector_batches,
                execution_stats->simd_chaining_stats.backtrack_path_vector_batches);
        }
    }
    // Unsupported tuples and incomplete scalar/SIMD paths deliberately remain
    // unattested. Persisted identity remains fail-closed until its reader
    // publishes the generation validation.
    stats.merge_job(job);
}

/// Consume one runtime-sealed persisted grouped-SoA coordinate receipt into a local
/// job value.  Nothing caller-visible is mutated here: a missing, tampered or
/// repeated consume burns only the runtime record and returns no staged job.
/// The accepted operation authority supplies the immutable GPU environment
/// after every job and the complete causal receipt union have been validated.
[[nodiscard]] inline std::optional<::bioseq::pipeline_stats::pipeline_job_execution_stats>
stage_persisted_grouped_soa_coordinate_job(
    const minimap2_parameters& params,
    const ::bioseq::pipeline_stats::target_index_source requested_source,
    bioseq::gpu::vulkan::persisted_grouped_soa_coordinate_operation_receipt& receipt,
    const bioseq::gpu::vulkan::persisted_grouped_soa_coordinate_job_expectation&
        expectation) noexcept {
    ::bioseq::pipeline_stats::pipeline_job_execution_stats job{};
    job.requested_backend = backend_from_selected_params(params);
    job.requested_target_index_source = requested_source;
    const auto consumed_validation = receipt.consume(expectation);
    const bool accepted =
        job.requested_backend == ::bioseq::pipeline_stats::executed_backend::vulkan_gpu &&
        selected_vulkan_execution(params) && !params.do_cigar &&
        pipe::is_gpu_coordinate_only_execution_active(params) &&
        pipe::is_gpu_target_index_build_active(params) &&
        requested_source == ::bioseq::pipeline_stats::target_index_source::persisted &&
        consumed_validation.has_value() && consumed_validation->is_valid();
    if (!accepted)
        return std::nullopt;
    job.actual_backend = ::bioseq::pipeline_stats::executed_backend::vulkan_gpu;
    job.actual_target_index_source = ::bioseq::pipeline_stats::target_index_source::persisted;
    job.persisted_generation_id = consumed_validation->audit().generation_id;
    job.persisted_source_identity = consumed_validation->audit().source_identity;
    job.logical_target_row = expectation.logical_target_row;
    job.query_ordinal = expectation.logical_query_ordinal;
    const auto mark = [&job](const ::bioseq::pipeline_stats::pipeline_stage stage,
                             const ::bioseq::pipeline_stats::execution_engine engine) {
        auto& coverage = job.stages.coverage[::bioseq::pipeline_stats::stage_index(stage)];
        coverage.eligible_jobs = 1u;
        coverage.executed_jobs[::bioseq::pipeline_stats::engine_index(engine)] = 1u;
    };
    using stage_type = ::bioseq::pipeline_stats::pipeline_stage;
    using engine_type = ::bioseq::pipeline_stats::execution_engine;
    mark(stage_type::query_sketch, engine_type::vulkan_gpu);
    mark(stage_type::persisted_index_decode, engine_type::host_io);
    if (consumed_validation->audit().query_record.minimizer_count != 0u) {
        mark(stage_type::partition_selection, engine_type::vulkan_gpu);
        mark(stage_type::seed_lookup, engine_type::vulkan_gpu);
    }
    if (consumed_validation->audit().range_count != 0u) {
        mark(stage_type::chain_score, engine_type::vulkan_gpu);
        mark(stage_type::chain_backtrack, engine_type::vulkan_gpu);
        mark(stage_type::region_generation, engine_type::vulkan_gpu);
    }
    return job;
}

/// Copy one runtime cache audit into the pointer-free SQL ledger wire.
[[nodiscard]] inline ::bioseq::pipeline_stats::persisted_gpu_cache_metrics
make_persisted_gpu_cache_metrics_stats(
    const bioseq::gpu::vulkan::runtime_gpu_cache_metrics_audit& source) noexcept {
    return {
        .token_identity = source.token_identity,
        .pipeline_cache_loads = source.gpu_pipeline_cache_loads,
        .pipeline_cache_seed_bytes = source.gpu_pipeline_cache_seed_bytes,
        .pipeline_cache_load_ns = source.gpu_pipeline_cache_load_ns,
        .pipeline_creates = source.gpu_pipeline_creates,
        .pipeline_creation_ns = source.gpu_pipeline_creation_ns,
    };
}

template <std::size_t p_capacity>
[[nodiscard]] inline std::uint32_t
copy_persisted_gpu_stats_text(std::array<char, p_capacity>& destination,
                              const std::string_view source) noexcept {
    if (source.empty() || source.size() > destination.size() ||
        source.size() > (std::numeric_limits<std::uint32_t>::max)()) {
        return 0u;
    }
    std::copy(source.begin(), source.end(), destination.begin());
    return static_cast<std::uint32_t>(source.size());
}

/// Copy the pointer/path/handle-free process qualification into the admitted
/// SQL ledger. An oversized helper identity becomes an invalid zero-length
/// field instead of allocating or truncating provenance.
[[nodiscard]] inline ::bioseq::pipeline_stats::persisted_gpu_cache_validation_result
make_persisted_gpu_cache_validation_stats(
    const bioseq::gpu::vulkan::runtime_metal_archive_process_qualification& source) noexcept {
    ::bioseq::pipeline_stats::persisted_gpu_cache_validation_result validation_stats{
        .qualification_wire_revision = source.wire_revision,
        .identity = source.identity,
        .archive_count = source.archive_set.archive_count,
        .archive_bytes = source.archive_set.total_size,
        .archive_checksum = source.archive_set.ordered_checksum,
        .receipt_count = source.receipt_count,
        .hash_passes = source.hash_passes,
        .hashed_bytes = source.hashed_bytes,
        .private_files_created = source.private_files_created,
        .private_files_unlinked = source.private_files_unlinked,
        .live_private_paths = source.live_private_paths,
        .pipeline_filesystem_hash_passes = source.pipeline_filesystem_hash_passes,
        .pipeline_filesystem_hashed_bytes = source.pipeline_filesystem_hashed_bytes,
        .helper_wire_revision = source.helper_wire_revision,
        .helper_executable_size = source.helper_executable_size,
        .helper_executable_sha256 = source.helper_executable_sha256,
        .helper_metal_registry_id = source.helper_metal_registry_id,
        .device_uuid = source.vulkan_device_uuid,
        .api_version = source.api_version,
        .driver_version = source.driver_version,
        .vendor_id = source.vendor_id,
        .device_id = source.device_id,
        .pipeline_cache_uuid = source.pipeline_cache_uuid,
        .seed_receipt_identity = source.seed_receipt_identity,
        .seed_sha256 = source.seed_sha256,
        .seed_size = source.seed_size,
        .manifest_checksum = source.manifest_checksum,
        .is_saturated = source.saturated,
    };
    validation_stats.helper_build_identity_size = copy_persisted_gpu_stats_text(
        validation_stats.helper_build_identity, source.helper_build_identity);
    validation_stats.helper_source_size =
        copy_persisted_gpu_stats_text(validation_stats.helper_source, source.helper_source);
    return validation_stats;
}

/// Copy one full-stage runtime job preview into the admitted ledger slot.
[[nodiscard]] inline ::bioseq::pipeline_stats::persisted_gpu_job_stats make_persisted_gpu_job_stats(
    const bioseq::gpu::vulkan::persisted_grouped_soa_coordinate_job_receipt_preview&
        source) noexcept {
    const auto& audit = source.audit;
    const auto& coordinate = audit.result;
    const std::array<std::uint64_t, 4> submit_wait_identities{
        audit.count_submit_wait_identity,
        audit.emit_submit_wait_identity,
        audit.sort_range_submit_wait_identity,
        audit.chain_coordinate_submit_wait_identity,
    };
    ::bioseq::pipeline_stats::persisted_gpu_job_stats job_stats{
        .logical_target_row = audit.logical_target_row,
        .logical_query_ordinal = audit.logical_query_ordinal,
        .query_index = audit.query_index,
        .query_length = audit.query_record.query_len,
        .target_length = audit.target_len,
        .query_minimizer_count = audit.query_record.minimizer_count,
        .query_tile_count = audit.query_tile_count,
        .query_count_dispatches = audit.query_count_dispatches,
        .query_scan_dispatches = audit.query_scan_dispatches,
        .query_emit_dispatches = audit.query_emit_dispatches,
        .query_completed_roundtrips = audit.query_completed_roundtrips,
        .direct_count_dispatches = audit.direct_count_dispatches,
        .direct_scan_dispatches = audit.direct_scan_dispatches,
        .anchor_count = audit.anchor_count,
        .direct_emit_dispatches = audit.direct_emit_dispatches,
        .sort_operations = audit.sort_operations,
        .range_build_dispatches = audit.range_build_dispatches,
        .range_count = audit.range_count,
        .chain_dispatches = audit.chain_dispatches,
        .chain_backtrack_dispatches = audit.chain_backtrack_dispatches,
        .coordinate_dispatches = audit.coordinate_dispatches,
        .count_status_checks = audit.count_status_checks,
        .emit_status_checks = audit.emit_status_checks,
        .status_or = audit.status_or,
        .result_readbacks = audit.result_readbacks,
        .coordinate_vote_dispatches = audit.coordinate_vote_dispatches,
        .cpu_or_simd_biological_calls = audit.cpu_or_simd_biological_calls,
        .recompute_calls = audit.recompute_calls,
        .fallback_calls = audit.fallback_calls,
        .coordinate =
            {
                .query_start = coordinate.query_start,
                .query_end = coordinate.query_end,
                .target_start = coordinate.target_start,
                .target_end = coordinate.target_end,
                .match_length = coordinate.match_length,
                .block_length = coordinate.block_length,
                .chain_score = coordinate.chain_score,
                .seed_count = coordinate.seed_count,
                .is_reverse = coordinate.is_reverse != 0u,
                .is_valid = coordinate.is_valid != 0u,
            },
    };
    for (std::size_t submission_index = 0u; submission_index < job_stats.submissions.size();
         ++submission_index) {
        job_stats.submissions[submission_index] = {
            .operation_identity = source.shape_pipeline_operation_identity,
            .ordinal = source.shape_submission_ordinals[submission_index],
            .submit_wait_identity = submit_wait_identities[submission_index],
        };
    }
    return job_stats;
}

/// Populate the fixed operation envelope from the pre-finalizer runtime view.
[[nodiscard]] inline ::bioseq::pipeline_stats::persisted_gpu_operation_stats
make_persisted_gpu_operation_stats(
    const bioseq::gpu::vulkan::persisted_grouped_soa_coordinate_operation_receipt_preview& source,
    const ::bioseq::pipeline_stats::persisted_grouped_soa_coordinate_publication_stats& publication,
    const std::uint64_t generation_id,
    const std::uint64_t source_identity,
    const std::uint64_t cold_load_identity) noexcept {
    const auto& acceptance = source.acceptance;
    return {
        .operation_identity = acceptance.operation_identity,
        .publication = publication,
        .published_jobs = publication.expected_jobs,
        .publication_commits = 1u,
        .query_pipeline_receipt_count = acceptance.query_pipeline_receipt_count,
        .shape_pipeline_receipt_count = acceptance.shape_pipeline_receipt_count,
        .submission_count = acceptance.committed_submission_count,
        .query_pipeline_operation_identity = source.query_pipeline_operation_identity,
        .shape_pipeline_operation_identity = source.shape_pipeline_operation_identity,
        .query_submission_ordinals = source.query_submission_ordinals,
        .seed_receipt_identity = acceptance.seed_receipt_identity,
        .generation_id = generation_id,
        .source_identity = source_identity,
        .cold_load_identity = cold_load_identity,
        .environment =
            {
                .device_uuid = source.device_uuid,
                .api_version = source.api_version,
                .driver_version = source.driver_version,
                .vendor_id = source.vendor_id,
                .device_id = source.device_id,
                .pipeline_cache_uuid = source.pipeline_cache_uuid,
                .seed_sha256 = source.seed_sha256,
                .seed_size = source.seed_size,
                .manifest_checksum = source.manifest_checksum,
            },
        .cache_qualification =
            make_persisted_gpu_cache_validation_stats(source.cache_qualification),
        .cache_metrics = make_persisted_gpu_cache_metrics_stats(source.maximum_cache_metrics),
    };
}

/// Conservative admission envelope for the N-job capacities that coexist
/// between persisted grouped-SoA execution and atomic SQL publication.  The
/// returned reservation covers transient stats/staging overlap. The result map
/// that leaves process_sequences_typed() remains part of the
/// caller-owned output contract.
[[nodiscard]] inline std::uint64_t
persisted_grouped_soa_publication_staging_bytes(const std::uint64_t job_count) noexcept {
    using staged_job_type = ::bioseq::pipeline_stats::pipeline_job_execution_stats;
    using result_map_type = std::map<int32_t, std::vector<match_result>>;
    using result_map_value_type = typename result_map_type::value_type;

    std::uint64_t bytes_per_job = 0u;
    const auto add = [&bytes_per_job](const std::uint64_t bytes) noexcept {
        bytes_per_job = bioseq::common::saturating_add(bytes_per_job, bytes);
    };
    add(sizeof(bioseq::gpu::vulkan::gpu_seed_coordinate_job));
    add(sizeof(std::size_t));
    add(sizeof(bioseq::gpu::vulkan::gpu_coordinate_result_wire));
    // Runtime owns separate retained permits for stats records and both
    // causal audit-job vectors; counting them here would double-reserve them.
    add(sizeof(staged_job_type));
    // At most one ordered-map node per job. Four pointers cover
    // parent/left/right plus allocator/alignment bookkeeping.
    add(sizeof(result_map_value_type));
    add(4u * sizeof(void*));
    // Repeated query ids can geometrically grow result vectors, but total
    // capacity remains below two elements per appended coordinate result.
    add(bioseq::common::saturating_mul(2u, sizeof(match_result)));

    return bioseq::common::saturating_mul(
        bioseq::common::saturating_mul(job_count, bytes_per_job),
        bioseq::parallelism::k_vector_preallocation_headroom_factor);
}

inline void record_target_row(::bioseq::pipeline_stats::pipeline_execution_stats& stats,
                              const std::uint64_t rows = 1) noexcept {
    add_operational_counter(stats, stats.operational.target_rows, rows);
}

[[nodiscard]] inline ::bioseq::pipeline_stats::target_index_source
requested_target_index_source(const batch_processor::scanner_pg_context* scanner_pg_ctx) noexcept {
    if (scanner_pg_ctx == nullptr) {
        return ::bioseq::pipeline_stats::target_index_source::recomputed;
    }
    switch (scanner_pg_ctx->index_read_requirement) {
    case bioseq::indexing::index_read_requirement::require_absent:
        return ::bioseq::pipeline_stats::target_index_source::recomputed;
    case bioseq::indexing::index_read_requirement::require_compatible_persisted:
        return ::bioseq::pipeline_stats::target_index_source::persisted;
    case bioseq::indexing::index_read_requirement::allow:
        return scanner_pg_ctx->read_index_active
                   ? ::bioseq::pipeline_stats::target_index_source::persisted
                   : ::bioseq::pipeline_stats::target_index_source::recomputed;
    }
    return ::bioseq::pipeline_stats::target_index_source::recomputed;
}

} // namespace bioseq::simd::typed_dispatch::typed_support_detail
