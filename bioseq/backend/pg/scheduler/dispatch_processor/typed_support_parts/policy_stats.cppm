/**
 * @file typed_support_parts/policy_stats.cppm
 * @brief Selects dispatch policies and collects checked execution statistics.
 */
module;

extern "C" {
#include "postgres_ext.h"

char* GetConfigOptionByName(const char* name, const char** varname, bool missing_ok);
void pfree(void* pointer);
}

#include <new>

export module bioseq.postgres.scheduler.dispatch_processor.typed_support.policy_stats;

import std;
export import bioseq.postgres.scheduler.dispatch_processor.operational_stats;
import bioseq.postgres.scheduler.dispatch_processor.effective_minimap_parameters;
import bioseq.common.checked_arithmetic;
import bioseq.common.hashing;
import bioseq.postgres.support;
import bioseq.indexing.type;
import bioseq.indexing.traits;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_sequence;
import bioseq.indexing.gpu_minimizer_index;
import bioseq.postgres.storage.indexing.minimizer_persistence_policy;
import bioseq.sequences.kmers;
import bioseq.pipeline.minimap_port;
import bioseq.pipeline.pipe;
import bioseq.gpu.vulkan.runtime;
import bioseq.postgres.types.compact_sequence;
import bioseq.instrumentation.clock;
import bioseq.storage.size;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;
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

// Read and free the PostgreSQL string through invoke_pg_as_cpp so PostgreSQL
// errors become C++ exceptions. Copy it between those calls so std::string
// cleanup can run normally.
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

/**
 * @brief Hashes an assembled-index key for lookup in the GPU cache.
 * @param[in] key Relation, minimizer scheme, query, and sequence-window identity.
 * @return A stable nonzero cache key.
 */
template <class p_assembled_index_key>
[[nodiscard]] inline bioseq::indexing::gpu_index_cache_identity
pg_gpu_index_identity(const p_assembled_index_key& key) noexcept {
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
choose_deferred_minimizer_persist_index(const auto& ctx) {
    if (ctx.user_relid == InvalidOid || ctx.mmsearch_seq_attno <= 0) {
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
 * Adjust the default minimizer and chaining settings for short sequence pairs.
 * The shared helper changes them only when k, w, the minimum chain score, and
 * the minimum seed count still have their default values.
 */
[[nodiscard]] inline minimap2_parameters apply_size_heuristic(const minimap2_parameters& base,
                                                              size_t min_bp) {
    return effective_minimap_parameters_for_pair(base, min_bp);
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
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    return ::bioseq::pipeline_stats::executed_backend::mixed_cpu;
#else
    return ::bioseq::pipeline_stats::executed_backend::mixed_invalid;
#endif
}

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
/// Gets the requested backend label from the statement plan. Execution
/// statistics must still confirm which stages ran and on which device.
[[nodiscard]] inline ::bioseq::pipeline_stats::executed_backend
backend_from_statement_plan(const pipe::statement_execution_plan& execution_plan) noexcept {
    using backend = ::bioseq::pipeline_stats::executed_backend;
    switch (execution_plan.route) {
    case pipe::seed_chain_route::host_seed_host_chain:
        return backend_from_selected_params(execution_plan.frozen_parameters);
    case pipe::seed_chain_route::host_seed_vulkan_chain:
    case pipe::seed_chain_route::vulkan_seed_host_chain:
    case pipe::seed_chain_route::vulkan_seed_vulkan_chain:
        return backend::hybrid_cpu_vulkan;
    }
    return backend::mixed_invalid;
}

[[nodiscard]] inline ::bioseq::pipeline_stats::minimizer_algorithm_identity
pipeline_minimizer_algorithm_identity(const minimizer_algorithm algorithm) noexcept {
    using identity_type = ::bioseq::pipeline_stats::minimizer_algorithm_identity;
    switch (algorithm) {
    case minimizer_algorithm::scalar:
        return identity_type::scalar;
    case minimizer_algorithm::simd_minimap2_reference:
        return identity_type::simd_minimap2_reference;
    case minimizer_algorithm::two_stack_nthash:
        return identity_type::two_stack_nthash;
    case minimizer_algorithm::vulkan_minimap_integer:
        return identity_type::vulkan_minimap_integer;
    case minimizer_algorithm::bounded_nthash_simd:
        return identity_type::bounded_nthash_simd;
    }
    return identity_type::mixed_invalid;
}

[[nodiscard]] inline ::bioseq::pipeline_stats::chaining_algorithm_identity
pipeline_chaining_algorithm_identity(const chaining_algorithm algorithm) noexcept {
    using identity_type = ::bioseq::pipeline_stats::chaining_algorithm_identity;
    switch (algorithm) {
    case chaining_algorithm::scalar_minimap2:
        return identity_type::scalar_minimap2;
    case chaining_algorithm::simd_minimap2:
        return identity_type::simd_minimap2;
    case chaining_algorithm::range_minimum_query:
        return identity_type::range_minimum_query;
    case chaining_algorithm::vulkan_tiled_dynamic_programming:
        return identity_type::vulkan_tiled_dynamic_programming;
    }
    return identity_type::mixed_invalid;
}

[[nodiscard]] inline ::bioseq::pipeline_stats::base_alignment_algorithm_identity
pipeline_base_alignment_algorithm_identity(const base_alignment_algorithm algorithm) noexcept {
    using identity_type = ::bioseq::pipeline_stats::base_alignment_algorithm_identity;
    switch (algorithm) {
    case base_alignment_algorithm::ksw2:
        return identity_type::ksw2;
    case base_alignment_algorithm::block_aligner:
        return identity_type::block_aligner;
    case base_alignment_algorithm::ksw2_batched:
        return identity_type::mixed_invalid;
    }
    return identity_type::mixed_invalid;
}

[[nodiscard]] inline ::bioseq::pipeline_stats::minimizer_algorithm_identity
observed_minimizer_algorithm_identity(const match_result_collection* execution_stats) noexcept {
    using identity_type = ::bioseq::pipeline_stats::minimizer_algorithm_identity;
    if (execution_stats == nullptr || !execution_stats->observed_minimizer_algorithm_available)
        return identity_type::none;
    if (execution_stats->observed_minimizer_algorithm_conflict)
        return identity_type::mixed_invalid;
    return pipeline_minimizer_algorithm_identity(execution_stats->observed_minimizer_algorithm);
}

[[nodiscard]] inline ::bioseq::pipeline_stats::chaining_algorithm_identity
observed_chaining_algorithm_identity(const match_result_collection* execution_stats) noexcept {
    using identity_type = ::bioseq::pipeline_stats::chaining_algorithm_identity;
    if (execution_stats == nullptr || !execution_stats->observed_chaining_algorithm_available)
        return identity_type::none;
    if (execution_stats->observed_chaining_algorithm_conflict)
        return identity_type::mixed_invalid;
    return pipeline_chaining_algorithm_identity(execution_stats->observed_chaining_algorithm);
}

[[nodiscard]] inline ::bioseq::pipeline_stats::base_alignment_algorithm_identity
observed_base_alignment_algorithm_identity(
    const match_result_collection* execution_stats) noexcept {
    using identity_type = ::bioseq::pipeline_stats::base_alignment_algorithm_identity;
    if (execution_stats == nullptr || !execution_stats->observed_base_alignment_algorithm_available)
        return identity_type::none;
    if (execution_stats->observed_base_alignment_algorithm_conflict)
        return identity_type::mixed_invalid;
    return pipeline_base_alignment_algorithm_identity(
        execution_stats->observed_base_alignment_algorithm);
}
#endif
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
    // Identify the chaining policy from its recorded calls. Keep rejection
    // reasons and invalid work in their separate counters so a failed policy
    // can still be identified correctly.
    const bool exact = policy.exact_visited_calls != 0;
    const bool bounded = policy.bounded_batch_approximate_calls != 0;
    if (exact == bounded) {
        // Available statistics must identify exactly one policy.
        return identity_type::mixed_invalid;
    }
    return exact ? identity_type::exact_visited : identity_type::bounded_batch_approximate;
}

[[nodiscard]] inline ::bioseq::pipeline_stats::simd_alignment_algorithm_identity
requested_simd_alignment_algorithm_identity(const minimap2_parameters& params) noexcept {
    using identity_type = ::bioseq::pipeline_stats::simd_alignment_algorithm_identity;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    return params.base_alignment_algorithm == base_alignment_algorithm::block_aligner
               ? identity_type::block_aligner
               : identity_type::none;
#else
    return backend_from_selected_params(params) ==
                   ::bioseq::pipeline_stats::executed_backend::simd_cpu
               ? identity_type::block_aligner
               : identity_type::none;
#endif
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

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
/** Finds the target row and query number used to identify this alignment job. */
template <class p_scanner_pg_context>
[[nodiscard]] inline benchmark_e2e_operation_origin
benchmark_e2e_operation_origin_for_row(const p_scanner_pg_context* scanner_pg_ctx,
                                       const std::size_t row_index,
                                       const bool per_row_query_mode,
                                       const std::uint64_t broadcast_query_ordinal) noexcept {
    benchmark_e2e_operation_origin origin{};
    origin.logical_target_row =
        scanner_pg_ctx != nullptr && scanner_pg_ctx->target_rownos[row_index] >= 0
            ? static_cast<std::uint64_t>(scanner_pg_ctx->target_rownos[row_index])
            : static_cast<std::uint64_t>(row_index);
    origin.query_ordinal = scanner_pg_ctx != nullptr && scanner_pg_ctx->explicit_query_ordinals
                               ? scanner_pg_ctx->query_ordinals[row_index]
                           : per_row_query_mode ? static_cast<std::uint64_t>(row_index)
                                                : broadcast_query_ordinal;
    return origin;
}

/** Binds subsequent benchmark events to this job and its checked statement plan. */
template <class p_scanner_pg_context>
[[nodiscard]] inline benchmark_e2e_operation_scope
bind_benchmark_e2e_operation_origin(const pipe::statement_execution_plan_owner& execution_plan,
                                    const p_scanner_pg_context* scanner_pg_ctx,
                                    const std::size_t row_index,
                                    const bool per_row_query_mode,
                                    const std::uint64_t broadcast_query_ordinal) {
    if (!execution_plan ||
        (execution_plan->frozen_parameters.downstream_active_wall != nullptr &&
         !execution_plan->frozen_parameters.downstream_active_wall
              ->matches_benchmark_e2e_statement_owner(execution_plan))) {
        throw std::logic_error{"E2E job binding changed the statement owner"};
    }
    return benchmark_e2e_operation_scope{benchmark_e2e_operation_binding{
        .statement_owner = execution_plan,
        .origin = benchmark_e2e_operation_origin_for_row(
            scanner_pg_ctx, row_index, per_row_query_mode, broadcast_query_ordinal),
        .is_available = execution_plan->frozen_parameters.downstream_active_wall != nullptr,
    }};
}
#endif
template <class p_scanner_pg_context>
inline void
record_unattested_job(::bioseq::pipeline_stats::pipeline_execution_stats& stats,
                      const pipe::statement_execution_plan_owner& execution_plan,
                      const ::bioseq::pipeline_stats::target_index_source requested_source,
                      const ::bioseq::pipeline_stats::target_index_source actual_source,
                      p_scanner_pg_context* scanner_pg_ctx,
                      const std::size_t row_index,
                      const bool per_row_query_mode,
                      const std::uint64_t broadcast_query_ordinal,
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
                      match_result_collection* execution_stats,
#else
                      const match_result_collection* execution_stats,
#endif
                      const bool consumed_shared_cached_query_stats,
                      bool& cached_query_minimizer_stats_accounted
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                      , const vulkan_seed_frontend_job_execution* shared_vulkan_seed = nullptr
                      , const vulkan_seed_target_execution* shared_vulkan_target = nullptr
                      , const ::bioseq::pipeline_stats::execution_environment_snapshot*
                            completed_gpu_environment = nullptr
#endif
                      ) {
    if (!execution_plan) {
        throw std::logic_error{"E2E observation has no statement owner"};
    }
    const minimap2_parameters& params = execution_plan->frozen_parameters;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    if (params.downstream_active_wall != nullptr &&
        !params.downstream_active_wall->attest_benchmark_e2e_statement_plan_checkpoint(
            execution_plan,
            benchmark_e2e_statement_plan_checkpoint::statistics)) {
        throw std::logic_error{"E2E observation changed the statement control block"};
    }
#endif
    if (execution_stats != nullptr) {
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        stats.operational.record_algorithm_identities(
            pipeline_minimizer_algorithm_identity(params.minimizer_algorithm),
            observed_minimizer_algorithm_identity(execution_stats),
            pipeline_chaining_algorithm_identity(params.chaining_algorithm),
            observed_chaining_algorithm_identity(execution_stats),
            params.do_cigar
                ? pipeline_base_alignment_algorithm_identity(params.base_alignment_algorithm)
                : ::bioseq::pipeline_stats::base_alignment_algorithm_identity::none,
            params.do_cigar ? observed_base_alignment_algorithm_identity(execution_stats)
                            : ::bioseq::pipeline_stats::base_alignment_algorithm_identity::none);
        if (execution_stats->mapq_execution_available) {
            stats.operational.record_mapq_execution(execution_stats->mapq_executed);
        }
        if (execution_stats->mapq_execution_conflict ||
            (!execution_stats->mapq_execution_available && execution_stats->mapq_executed)) {
            stats.operational.mapq_execution_conflict = true;
            stats.operational.has_operational_identity_conflict = true;
        }
#endif
        if (execution_stats->prefilter_execution_stats_available) {
            stats.operational.record_prefilter_execution_stats(execution_stats->prefilter_stats);
        } else if (execution_stats->prefilter_stats !=
                   decltype(execution_stats->prefilter_stats){}) {
            auto malformed_prefilter_stats = execution_stats->prefilter_stats;
            malformed_prefilter_stats.is_saturated = true;
            stats.operational.record_prefilter_execution_stats(malformed_prefilter_stats);
        }
        // Add each completed job's measured stage times once, after its workers join.
        // Chaining includes backtracking. The left_extension slot holds the full
        // alignment-to-result time, leaving gap/right timing slots empty. Sums
        // across parallel jobs measure total work.
        // Query-union preparation is accounted separately because several jobs
        // can share it.
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
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    job.requested_backend = backend_from_statement_plan(*execution_plan);
#else
    job.requested_backend = backend_from_selected_params(params);
#endif
    job.requested_simd_chaining_policy = requested_simd_chaining_policy_identity(params);
    job.observed_simd_chaining_policy = observed_simd_chaining_policy_identity(execution_stats);
    job.requested_simd_alignment_algorithm = requested_simd_alignment_algorithm_identity(params);
    job.observed_simd_alignment_algorithm =
        observed_simd_alignment_algorithm_identity(execution_stats);
    const bool selected_all_simd =
        job.requested_backend == ::bioseq::pipeline_stats::executed_backend::simd_cpu;
    const bool selected_all_scalar =
        job.requested_backend == ::bioseq::pipeline_stats::executed_backend::scalar_cpu;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    const bool selected_mixed_cpu =
        job.requested_backend == ::bioseq::pipeline_stats::executed_backend::mixed_cpu;
#endif
    const bool require_target_sketch =
        actual_source == ::bioseq::pipeline_stats::target_index_source::recomputed;
    const std::uint64_t expected_query_ordinal =
        scanner_pg_ctx != nullptr && scanner_pg_ctx->explicit_query_ordinals
            ? scanner_pg_ctx->query_ordinals[row_index]
        : per_row_query_mode ? static_cast<std::uint64_t>(row_index)
                             : broadcast_query_ordinal;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
    using transient_receipt =
        bioseq::indexing::minimizer_sequence_generation_execution_receipt;
    transient_receipt* shared_cpu_target_receipt = nullptr;
    bool* shared_cpu_target_stats_published = nullptr;
    if (require_target_sketch && scanner_pg_ctx != nullptr &&
        scanner_pg_ctx->cpu_prepared_target != nullptr &&
        !execution_plan->uses_vulkan_seed()) {
        auto& prepared = *scanner_pg_ctx->cpu_prepared_target;
        for (auto& group : prepared.groups) {
            if (group.kmer_size != params.kmer_size ||
                group.window_size != params.window_size || group.max_occ != params.max_occ ||
                group.target_index_source !=
                    ::bioseq::pipeline_stats::target_index_source::recomputed ||
                !group.transient_target_generation_receipt.has_value()) {
                continue;
            }
            auto& receipt = *group.transient_target_generation_receipt;
            const bool grouped = group.transient_grouped_soa_reader.has_value();
            const bool mpf = group.target_index != nullptr;
            const bool exact_owner = grouped != mpf &&
                group.sketch_backend == receipt.selected_backend &&
                group.target_index_builds == 1u &&
                group.build_set_checksum_present &&
                group.build_set_checksum == group.key_set_checksum &&
                receipt.target_length == prepared.target_bp_size &&
                receipt.kmer_size == static_cast<std::uint16_t>(params.kmer_size) &&
                receipt.window_size == static_cast<std::uint16_t>(params.window_size) &&
                receipt.algorithm_identity.requested_algorithm == params.minimizer_algorithm &&
                (grouped ? receipt.is_internally_consistent() : receipt.is_spill_valid());
            if (!exact_owner || shared_cpu_target_receipt != nullptr) {
                shared_cpu_target_receipt = nullptr;
                shared_cpu_target_stats_published = nullptr;
                break;
            }
            shared_cpu_target_receipt = std::addressof(receipt);
            shared_cpu_target_stats_published =
                std::addressof(group.transient_target_generation_stats_published);
        }
    }
    const bool shared_cpu_target_proved = shared_cpu_target_receipt != nullptr;
    transient_receipt* operation_local_cpu_target_receipt = nullptr;
    bool* operation_local_cpu_target_stats_published = nullptr;
    const bool operation_local_cpu_target_receipt_present =
        execution_stats != nullptr &&
        (execution_stats->transient_cpu_target_generation_receipt.has_value() ||
         execution_stats->transient_cpu_target_generation_representation_kind !=
             transient_cpu_target_generation_representation::none ||
         execution_stats->transient_cpu_target_generation_receipt_conflict ||
         execution_stats->transient_cpu_target_generation_stats_published);
    if (operation_local_cpu_target_receipt_present && execution_stats != nullptr &&
        execution_stats->transient_cpu_target_generation_receipt.has_value() &&
        !execution_stats->transient_cpu_target_generation_receipt_conflict &&
        !execution_stats->transient_cpu_target_generation_stats_published &&
        scanner_pg_ctx != nullptr &&
        scanner_pg_ctx->target_recipe_datums[row_index] == 0 &&
        scanner_pg_ctx->target_locators[row_index].has_value()) {
        auto& receipt = *execution_stats->transient_cpu_target_generation_receipt;
        const auto representation =
            execution_stats->transient_cpu_target_generation_representation_kind;
        const bool terminal_receipt_valid =
            representation ==
                    transient_cpu_target_generation_representation::minimizer_fragment_spill
                ? receipt.is_spill_valid()
            : representation ==
                    transient_cpu_target_generation_representation::minimizer_sequence_publication
                ? receipt.is_internally_consistent()
                : false;
        const auto effective_backend =
            pipe::minimizer_sketch_backend_for_effective_algorithm(
                receipt.algorithm_identity.effective_algorithm);
        const bool scalar_or_exact_target =
            receipt.selected_backend ==
                bioseq::minimizers::minimizer_sketch_backend::scalar ||
            receipt.selected_backend ==
                bioseq::minimizers::minimizer_sketch_backend::exact_simd;
        const bool target_stats_absent =
            execution_stats->scalar_pipeline_stats.target_minimizers ==
                scalar_minimizer_execution_stats{} &&
            !execution_stats->exact_simd_target_execution_stats_available &&
            execution_stats->exact_simd_target_execution_stats == two_stack_execution_stats{} &&
            !execution_stats->bounded_nthash_target_execution_stats_available &&
            execution_stats->bounded_nthash_target_execution_stats ==
                bounded_nthash_execution_stats{};
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        const bool effective_identity_matches_query =
            execution_stats->observed_minimizer_algorithm_available &&
            !execution_stats->observed_minimizer_algorithm_conflict &&
            execution_stats->observed_minimizer_algorithm ==
                receipt.algorithm_identity.effective_algorithm;
#else
        const bool effective_identity_matches_query = true;
#endif
        const bool exact_owner =
            require_target_sketch && !execution_plan->uses_vulkan_seed() &&
            terminal_receipt_valid && scalar_or_exact_target && target_stats_absent &&
            effective_backend.has_value() && *effective_backend == receipt.selected_backend &&
            receipt.algorithm_identity.requested_algorithm == params.minimizer_algorithm &&
            effective_identity_matches_query && params.kmer_size > 0 && params.window_size > 0 &&
            static_cast<std::uint64_t>(params.kmer_size) <=
                (std::numeric_limits<std::uint16_t>::max)() &&
            static_cast<std::uint64_t>(params.window_size) <=
                (std::numeric_limits<std::uint16_t>::max)() &&
            receipt.kmer_size == static_cast<std::uint16_t>(params.kmer_size) &&
            receipt.window_size == static_cast<std::uint16_t>(params.window_size) &&
            receipt.target_length ==
                scanner_pg_ctx->target_locators[row_index]->logical_length;
        if (exact_owner) {
            operation_local_cpu_target_receipt = std::addressof(receipt);
            operation_local_cpu_target_stats_published = std::addressof(
                execution_stats->transient_cpu_target_generation_stats_published);
        }
    }
    const bool operation_local_cpu_target_proved =
        operation_local_cpu_target_receipt != nullptr;
    const bool cpu_target_authority_conflict =
        shared_cpu_target_proved && operation_local_cpu_target_receipt_present;
    const bool cpu_target_authority_consistent =
        !cpu_target_authority_conflict &&
        (!operation_local_cpu_target_receipt_present || operation_local_cpu_target_proved);
    transient_receipt* completed_cpu_target_receipt = nullptr;
    bool* completed_cpu_target_stats_published = nullptr;
    if (cpu_target_authority_consistent) {
        if (shared_cpu_target_proved) {
            completed_cpu_target_receipt = shared_cpu_target_receipt;
            completed_cpu_target_stats_published = shared_cpu_target_stats_published;
        } else if (operation_local_cpu_target_proved) {
            completed_cpu_target_receipt = operation_local_cpu_target_receipt;
            completed_cpu_target_stats_published = operation_local_cpu_target_stats_published;
        }
    }
    const bool completed_cpu_target_proved = completed_cpu_target_receipt != nullptr;
#else
    const bool cpu_target_authority_consistent = true;
#endif
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    const bool shared_gpu_target_proved =
        shared_vulkan_seed != nullptr && shared_vulkan_target != nullptr &&
        completed_gpu_environment != nullptr &&
        shared_vulkan_seed->is_internally_consistent() &&
        shared_vulkan_target->is_internally_consistent() &&
        shared_vulkan_seed->target_source() == actual_source &&
        shared_vulkan_target->target_source() == actual_source &&
        shared_vulkan_seed->query_ordinal == expected_query_ordinal &&
        shared_vulkan_seed->target_completion == shared_vulkan_target->completion &&
        shared_vulkan_seed->target_operation_identity ==
            shared_vulkan_target->target_operation_identity &&
        shared_vulkan_seed->target_len == shared_vulkan_target->target_len &&
        shared_vulkan_seed->persisted_generation_id ==
            shared_vulkan_target->persisted_generation_id &&
        shared_vulkan_seed->persisted_source_identity ==
            shared_vulkan_target->persisted_source_identity &&
        (!shared_vulkan_target->query_empty_bypass ||
         (shared_vulkan_target->completion ==
              vulkan_seed_target_completion_kind::mpf_runtime_generated &&
          shared_vulkan_seed->completion ==
              vulkan_seed_frontend_completion_kind::mpf_zero_query &&
          shared_vulkan_seed->minimizer_count == 0u &&
          shared_vulkan_seed->operation_identity == 0u)) &&
        completed_gpu_environment->gpu_device_uuid == shared_vulkan_target->device_uuid;
    const bool require_collection_target_sketch =
        require_target_sketch && !completed_cpu_target_proved && !shared_gpu_target_proved;
#elif defined(BIOSEQ_TEST_INDEXES)
    const bool require_collection_target_sketch =
        require_target_sketch && !completed_cpu_target_proved;
#else
    const bool require_collection_target_sketch = require_target_sketch;
#endif
    const bool accepted_all_simd =
        cpu_target_authority_consistent && selected_all_simd && execution_stats != nullptr &&
        execution_stats->is_all_simd_execution_consistent(params.minimizer_algorithm,
                                                          params.simd_chaining_policy,
                                                          require_collection_target_sketch,
                                                          /*require_nonempty_vector_work=*/true);
    // A scalar chain completed with zero anchors has no regions to align.
    // Accept that completed empty result without requiring a KSW2 call, after
    // checking that its counters contain no chaining or alignment work.
    const bool completed_zero_anchor_scalar_chain =
        selected_all_scalar && params.do_cigar && execution_stats != nullptr &&
        execution_stats->scalar_pipeline_execution_stats_available &&
        execution_stats->size() == 0u &&
        !execution_stats->scalar_pipeline_stats.is_saturated &&
        execution_stats->scalar_pipeline_stats.chain_stage_completion_calls > 0u &&
        execution_stats->scalar_pipeline_stats.chain_stage_completed_anchor_inputs == 0u &&
        execution_stats->scalar_pipeline_stats.chain_score_calls == 0u &&
        execution_stats->scalar_pipeline_stats.chain_backtrack_calls == 0u &&
        execution_stats->scalar_pipeline_stats.chain_anchor_inputs == 0u &&
        execution_stats->scalar_pipeline_stats.ksw2_left_calls == 0u &&
        execution_stats->scalar_pipeline_stats.ksw2_gap_calls == 0u &&
        execution_stats->scalar_pipeline_stats.ksw2_right_calls == 0u &&
        execution_stats->scalar_pipeline_stats.ksw2_input_cell_area == 0u;
    const bool require_scalar_alignment =
        params.do_cigar && !completed_zero_anchor_scalar_chain;
    const bool accepted_all_scalar =
        cpu_target_authority_consistent && selected_all_scalar && execution_stats != nullptr &&
        execution_stats->is_all_scalar_execution_consistent(require_collection_target_sketch,
                                                            require_scalar_alignment);
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    const bool accepted_mixed_cpu =
        cpu_target_authority_consistent && selected_mixed_cpu && execution_stats != nullptr &&
        execution_stats->is_selected_components_execution_consistent(params.minimizer_algorithm,
                                                           params.chaining_algorithm,
                                                           params.base_alignment_algorithm,
                                                           params.simd_chaining_policy,
                                                           require_collection_target_sketch,
                                                           /*require_alignment=*/params.do_cigar);
    const bool accepted_hybrid_host_seed =
        cpu_target_authority_consistent &&
        execution_plan->route == pipe::seed_chain_route::host_seed_vulkan_chain &&
        execution_stats != nullptr &&
        execution_stats->has_valid_completed_vulkan_chaining(
            execution_stats->partial_vulkan_stats.chain_score_dispatches) &&
        execution_stats->is_selected_components_execution_consistent(
            params.minimizer_algorithm, params.chaining_algorithm,
            params.base_alignment_algorithm, params.simd_chaining_policy,
            require_collection_target_sketch, /*require_alignment=*/params.do_cigar);
    const bool accepted_hybrid_gpu_seed =
        cpu_target_authority_consistent &&
        (execution_plan->route == pipe::seed_chain_route::vulkan_seed_host_chain ||
         execution_plan->route == pipe::seed_chain_route::vulkan_seed_vulkan_chain) &&
        completed_gpu_environment != nullptr && shared_gpu_target_proved &&
        execution_stats != nullptr &&
        execution_stats->is_selected_components_execution_consistent(
            params.minimizer_algorithm,
            params.chaining_algorithm,
            params.base_alignment_algorithm,
            params.simd_chaining_policy,
            /*require_target_sketch=*/false,
            /*require_alignment=*/params.do_cigar,
            shared_vulkan_seed);
    const bool accepted_selected = accepted_all_simd || accepted_all_scalar ||
                                   accepted_mixed_cpu || accepted_hybrid_host_seed ||
                                   accepted_hybrid_gpu_seed;
    const bool authenticated_query_empty_recomputed_bypass =
        accepted_hybrid_gpu_seed && shared_vulkan_target->query_empty_bypass &&
        shared_vulkan_target->completion ==
            vulkan_seed_target_completion_kind::mpf_runtime_generated &&
        shared_vulkan_seed->completion ==
            vulkan_seed_frontend_completion_kind::mpf_zero_query;
#endif
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
        // Count continuation batches and whole-sequence vector calls separately.
        // Both perform vector work, with different units of measurement.
        add_operational_counter(
            stats, stats.operational.nthash_vector_batches, range_stats.vector_batches);
        add_operational_counter(stats,
                                stats.operational.nthash_resident_vector_stream_kernel_calls,
                                range_stats.resident_vector_stream_kernel_calls);
    };
    // A broadcast query sketch can serve several jobs. Count its work once,
    // at the first consumer whose execution checks pass. Recomputed query
    // sketches are counted with their own jobs below.
    const bool accepted_shared_query_sketch =
        consumed_shared_cached_query_stats && !cached_query_minimizer_stats_accounted &&
        execution_stats != nullptr &&
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        accepted_selected &&
        (params.minimizer_algorithm == minimizer_algorithm::scalar
             ? execution_stats->is_selected_scalar_sketch_component_consistent(
                   /*require_target_sketch=*/false,
                   /*require_nonempty_work=*/false)
         : params.minimizer_algorithm == minimizer_algorithm::simd_minimap2_reference
             ? true
             : execution_stats->is_selected_simd_sketch_component_consistent(
                   params.minimizer_algorithm,
                   /*require_target_sketch=*/false,
                   /*require_nonempty_vector_work=*/false));
#else
        (selected_all_simd ? execution_stats->is_selected_simd_sketch_execution_consistent(
                                 params.minimizer_algorithm,
                                 /*require_target_sketch=*/false,
                                 /*require_nonempty_vector_work=*/false)
                           : selected_all_scalar &&
                                 execution_stats->is_selected_scalar_sketch_execution_consistent(
                                     /*require_target_sketch=*/false,
                                     /*require_nonempty_work=*/false));
#endif
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
    // Report the backend only after the completed stage records pass validation.
    // Missing or inconsistent records leave the job marked mixed_invalid.
    job.actual_backend =
        accepted_all_simd     ? ::bioseq::pipeline_stats::executed_backend::simd_cpu
        : accepted_all_scalar ? ::bioseq::pipeline_stats::executed_backend::scalar_cpu
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        : accepted_mixed_cpu ? ::bioseq::pipeline_stats::executed_backend::mixed_cpu
        : accepted_hybrid_host_seed || accepted_hybrid_gpu_seed
            ? ::bioseq::pipeline_stats::executed_backend::hybrid_cpu_vulkan
#endif
                             : ::bioseq::pipeline_stats::executed_backend::mixed_invalid;
    job.requested_target_index_source = requested_source;
    job.actual_target_index_source = actual_source;
    if (actual_source == ::bioseq::pipeline_stats::target_index_source::persisted &&
        scanner_pg_ctx != nullptr) {
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        if (accepted_hybrid_gpu_seed) {
            job.persisted_generation_id = shared_vulkan_seed->persisted_generation_id;
            job.persisted_source_identity = shared_vulkan_seed->persisted_source_identity;
        } else
#endif
        {
            job.persisted_generation_id = scanner_pg_ctx->persisted_generation_ids[row_index];
            job.persisted_source_identity = scanner_pg_ctx->persisted_source_identities[row_index];
        }
    }
    const std::size_t effective_workers =
        bioseq::parallelism::global_pool::instance().maximum_concurrency();
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    if (accepted_hybrid_gpu_seed) {
        job.environment = *completed_gpu_environment;
    } else
#endif
    {
        job.environment.effective_cpu_workers =
            effective_workers >
                    static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())
                ? (std::numeric_limits<std::uint32_t>::max)()
                : static_cast<std::uint32_t>(effective_workers);
        job.environment.record_highway_target(params.highway_dispatch,
                                              params.highway_executed_target);
    }
    if (scanner_pg_ctx != nullptr && scanner_pg_ctx->target_rownos[row_index] >= 0) {
        job.logical_target_row =
            static_cast<std::uint64_t>(scanner_pg_ctx->target_rownos[row_index]);
    } else {
        job.logical_target_row = static_cast<std::uint64_t>(row_index);
    }
    job.query_ordinal = expected_query_ordinal;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    if (accepted_selected) {
#else
    if (accepted_all_simd || accepted_all_scalar) {
#endif
        const auto mark = [&job](::bioseq::pipeline_stats::pipeline_stage stage,
                                 ::bioseq::pipeline_stats::execution_engine engine) noexcept {
            auto& coverage = job.stages.coverage[::bioseq::pipeline_stats::stage_index(stage)];
            coverage.eligible_jobs = 1;
            coverage.executed_jobs[::bioseq::pipeline_stats::engine_index(engine)] = 1;
        };
        using stage_type = ::bioseq::pipeline_stats::pipeline_stage;
        using engine_type = ::bioseq::pipeline_stats::execution_engine;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        const engine_type minimizer_engine =
            accepted_hybrid_gpu_seed
                ? engine_type::vulkan_gpu
            : execution_stats->observed_minimizer_algorithm == minimizer_algorithm::scalar
                ? engine_type::scalar_cpu
                : engine_type::simd_cpu;
        const engine_type chaining_engine =
            params.chaining_algorithm == chaining_algorithm::simd_minimap2
                ? engine_type::simd_cpu
            : params.chaining_algorithm == chaining_algorithm::vulkan_tiled_dynamic_programming
                ? engine_type::vulkan_gpu
                : engine_type::scalar_cpu;
        const engine_type alignment_engine =
            params.base_alignment_algorithm == base_alignment_algorithm::block_aligner
                ? engine_type::simd_cpu
                : engine_type::scalar_cpu;
        mark(stage_type::query_sketch, minimizer_engine);
#else
        const engine_type biological_engine =
            accepted_all_scalar ? engine_type::scalar_cpu : engine_type::simd_cpu;
        mark(stage_type::query_sketch, biological_engine);
#endif
        if (require_target_sketch) {
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            if (!accepted_hybrid_gpu_seed ||
                shared_vulkan_target->target_sketch_completed()) {
                mark(stage_type::target_sketch, minimizer_engine);
            }
#else
            mark(stage_type::target_sketch, biological_engine);
#endif
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            if (!accepted_hybrid_gpu_seed ||
                shared_vulkan_target->target_index_build_executed()) {
                // Vulkan supplies target minimizers. The host builds their index by
                // writing and replaying bounded runs.
                mark(stage_type::target_index_build, engine_type::scalar_cpu);
            }
#else
            mark(stage_type::target_index_build, engine_type::scalar_cpu);
#endif
        } else {
            mark(stage_type::persisted_index_decode, engine_type::host_io);
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            const bool grouped_soa_target = accepted_hybrid_gpu_seed &&
                (shared_vulkan_target->completion ==
                     vulkan_seed_target_completion_kind::mps_persisted ||
                 shared_vulkan_target->completion ==
                     vulkan_seed_target_completion_kind::mps_runtime_generated);
            mark(stage_type::partition_selection,
                 grouped_soa_target ? engine_type::vulkan_gpu
                                    : engine_type::scalar_cpu);
#else
            mark(stage_type::partition_selection, engine_type::scalar_cpu);
#endif
        }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        if (accepted_hybrid_gpu_seed) {
            if (shared_vulkan_target->device_index_upload_executed()) {
                mark(stage_type::device_index_upload, engine_type::vulkan_gpu);
            }
            if (shared_vulkan_seed->seed_stats.seed_lookup_dispatches != 0u) {
                mark(stage_type::seed_lookup, engine_type::vulkan_gpu);
            }
            const bool chain_executed =
                chaining_engine == engine_type::vulkan_gpu
                    ? execution_stats->partial_vulkan_stats.chain_score_dispatches != 0u
                : chaining_engine == engine_type::scalar_cpu
                    ? execution_stats->scalar_pipeline_stats.chain_score_calls != 0u
                    : execution_stats->simd_chaining_execution_stats_available;
            if (chain_executed) {
                mark(stage_type::chain_score, chaining_engine);
                mark(stage_type::chain_backtrack,
                     chaining_engine == engine_type::vulkan_gpu
                         ? engine_type::scalar_cpu : chaining_engine);
            }
        } else {
            mark(stage_type::seed_lookup, engine_type::scalar_cpu);
            mark(stage_type::chain_score, chaining_engine);
            mark(stage_type::chain_backtrack,
                 accepted_hybrid_host_seed ? engine_type::scalar_cpu : chaining_engine);
        }
        if (params.base_alignment_algorithm == base_alignment_algorithm::block_aligner) {
#else
        mark(stage_type::seed_lookup, engine_type::scalar_cpu);
        mark(stage_type::chain_score, biological_engine);
        mark(stage_type::chain_backtrack, biological_engine);
        if (accepted_all_simd) {
#endif
            const auto& block = execution_stats->block_aligner_stats;
            if (block.left_executed != 0) {
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                mark(stage_type::left_extension, alignment_engine);
#else
                mark(stage_type::left_extension, engine_type::simd_cpu);
#endif
            }
            if (block.gap_executed != 0) {
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                mark(stage_type::gap_extension, alignment_engine);
#else
                mark(stage_type::gap_extension, engine_type::simd_cpu);
#endif
            }
            if (block.right_executed != 0) {
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                mark(stage_type::right_extension, alignment_engine);
#else
                mark(stage_type::right_extension, engine_type::simd_cpu);
#endif
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

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        if (accepted_selected &&
#else
        if (accepted_all_simd &&
#endif
            params.minimizer_algorithm == minimizer_algorithm::two_stack_nthash) {
            if (!consumed_shared_cached_query_stats) {
                record_exact_nthash_vector_activity(
                    execution_stats->exact_simd_query_execution_stats);
            }
            if (execution_stats->exact_simd_target_execution_stats_available) {
                record_exact_nthash_vector_activity(
                    execution_stats->exact_simd_target_execution_stats);
            }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        } else if (accepted_selected &&
#else
        } else if (accepted_all_simd &&
#endif
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
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        if (accepted_selected && params.chaining_algorithm == chaining_algorithm::simd_minimap2) {
#else
        if (accepted_all_simd) {
#endif
            add_operational_counter(stats,
                                    stats.operational.simd_chain_vector_batches,
                                    execution_stats->simd_chaining_stats.score_vector_batches);
            add_operational_counter(
                stats,
                stats.operational.simd_chain_backtrack_vector_batches,
                execution_stats->simd_chaining_stats.backtrack_path_vector_batches);
        }
    }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    if (accepted_hybrid_gpu_seed) {
        add_operational_counter(stats,
                                stats.operational.gpu_seed_dispatches,
                                shared_vulkan_seed->seed_stats.seed_lookup_dispatches);
        add_operational_counter(stats,
                                stats.operational.gpu_chain_dispatches,
                                execution_stats->partial_vulkan_stats.chain_score_dispatches);
        // Tiled DP returns predecessor links for CPU backtracking.
    }
    if (accepted_hybrid_host_seed) {
        // The host-seed job has finished and kept the GPU chaining completion
        // record. Validate its device and cache statistics before publishing
        // them. Jobs with GPU seeding are finalized with their full query wave.
        struct publication_validation {
            ::bioseq::pipeline_stats::pipeline_job_execution_stats* job;
            const minimap2_parameters* params;
            const match_result_collection* execution_stats;
            std::size_t effective_workers;
            ::bioseq::pipeline_stats::pipeline_operational_stats cache_delta{};
        } publication{&job, &params, execution_stats, effective_workers};
        const auto validate_publication = +[](
            void* context,
            const bioseq::gpu::vulkan::runtime_gpu_execution_environment_snapshot& binding,
            const bioseq::gpu::vulkan::runtime_gpu_cache_metrics_audit& cache_audit) {
            auto& pending = *static_cast<publication_validation*>(context);
            auto environment =
                ::bioseq::pipeline_stats::make_persisted_grouped_soa_gpu_environment_snapshot(
                    binding, pending.effective_workers);
            const auto cache_delta =
                ::bioseq::pipeline_stats::make_persisted_grouped_soa_gpu_cache_metrics_delta(
                    cache_audit);
            if (!environment || !cache_delta) {
                throw std::logic_error{
                    "completed hybrid chaining has no publishable GPU authority"};
            }
            environment->record_highway_target(pending.params->highway_dispatch,
                                               pending.params->highway_executed_target);
            auto candidate = *pending.job;
            candidate.environment = *environment;
            if (!::bioseq::pipeline_stats::is_job_execution_stats_valid(
                    candidate, observed_minimizer_algorithm_identity(pending.execution_stats),
                    observed_chaining_algorithm_identity(pending.execution_stats),
                    observed_base_alignment_algorithm_identity(pending.execution_stats))) {
                throw std::logic_error{
                    "completed hybrid chaining job differs from selected stages"};
            }
            *pending.job = candidate;
            pending.cache_delta = *cache_delta;
        };
        auto completed = bioseq::gpu::vulkan::runtime::instance()
                             .finalize_selected_pipeline_operation(
                                 std::move(execution_stats->completed_vulkan_chaining),
                                 validate_publication, &publication,
                                 std::span<bioseq::gpu::vulkan::runtime_pipeline_operation_token>{
                                     execution_stats->additional_completed_vulkan_chaining.data(),
                                     execution_stats->additional_completed_vulkan_chaining.size()});
        // Validation finished before the one-time publication. The remaining
        // statistics merge and record consumption cannot throw.
        stats.merge_operational(publication.cache_delta);
        add_operational_counter(
            stats,
            stats.operational.gpu_chain_dispatches,
            execution_stats->partial_vulkan_stats.chain_score_dispatches);
        completed.second.consume();
    }
#endif
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
    if (
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        accepted_selected &&
#else
        (accepted_all_simd || accepted_all_scalar) &&
#endif
        completed_cpu_target_proved &&
        completed_cpu_target_stats_published != nullptr &&
        !*completed_cpu_target_stats_published) {
        const auto& receipt = *completed_cpu_target_receipt;
        if (receipt.selected_backend ==
                bioseq::minimizers::minimizer_sketch_backend::exact_simd &&
            receipt.exact_simd_stats_available) {
            add_operational_counter(
                stats,
                stats.operational.nthash_vector_batches,
                receipt.exact_simd_stats.vector_batches);
            add_operational_counter(
                stats,
                stats.operational.nthash_scalar_backend_branches,
                receipt.exact_simd_stats.scalar_backend_branches);
            add_operational_counter(
                stats,
                stats.operational.nthash_scalar_tail_symbols,
                receipt.exact_simd_stats.scalar_tail_symbols);
            add_operational_counter(
                stats,
                stats.operational.nthash_masked_tail_lanes,
                receipt.exact_simd_stats.masked_tail_lanes);
        } else if (receipt.selected_backend ==
                       bioseq::minimizers::minimizer_sketch_backend::bounded_nthash_simd &&
                   receipt.packed_bounded_stats_available) {
            add_operational_counter(
                stats,
                stats.operational.nthash_vector_batches,
                receipt.packed_bounded_stats.packed_decode_batches);
        }
        *completed_cpu_target_stats_published = true;
    }
#endif
    // Keep incomplete execution records in the statistics so validation can
    // reject them. Stored-index jobs also need a checked generation identity.
    stats.merge_job(job
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                    , authenticated_query_empty_recomputed_bypass
#endif
                    );
}

/// @brief Checked GPU environment and cache metrics awaiting one-time reporting.
struct source_direct_vulkan_execution_acceptance {
    ::bioseq::pipeline_stats::execution_environment_snapshot environment{};
    bioseq::gpu::vulkan::runtime_gpu_cache_metrics_delta cache_metrics{};

    /// @brief Checks the GPU environment and whether the cache metrics can still be reported.
    [[nodiscard]] bool is_valid() const noexcept {
        return ::bioseq::pipeline_stats::is_environment_valid_for_backend(
                   environment, ::bioseq::pipeline_stats::executed_backend::vulkan_gpu) &&
               cache_metrics.is_valid();
    }
};

/// @brief Checks completed GPU batch results and consumes their execution record.
/// @param[in] params Selected GPU pipeline settings.
/// @param[in,out] receipt One-use record tied to these counters and results.
/// @param[in] batch_stats Completed batch counters.
/// @param[in] results Completed coordinate results.
/// @return The checked device information and cache metrics, or no value on mismatch.
[[nodiscard]] inline std::optional<source_direct_vulkan_execution_acceptance>
consume_source_direct_vulkan_environment(
    const minimap2_parameters& params,
    bioseq::gpu::vulkan::source_direct_coordinate_execution_receipt& receipt,
    const bioseq::gpu::vulkan::gpu_seed_coordinate_batch_stats& batch_stats,
    const std::span<const bioseq::gpu::vulkan::gpu_coordinate_result_wire> results) noexcept {
    auto runtime_execution = receipt.consume(batch_stats, results);
    const bool counters_consistent =
        batch_stats.problem_count == static_cast<std::uint64_t>(results.size()) &&
        batch_stats.problem_count != 0u && batch_stats.wave_count != 0u &&
        batch_stats.wave_count <= batch_stats.problem_count &&
        batch_stats.seed_count_dispatches != 0u &&
        batch_stats.seed_count_dispatches <= batch_stats.problem_count &&
        batch_stats.seed_emit_dispatches <= batch_stats.seed_count_dispatches &&
        batch_stats.sort_dispatches == batch_stats.range_build_dispatches &&
        batch_stats.chain_dispatches <= batch_stats.range_build_dispatches &&
        batch_stats.chain_dispatches == batch_stats.chain_backtrack_dispatches &&
        batch_stats.coordinate_vote_dispatches == 0u;
    bool results_consistent = counters_consistent;
    bool has_valid_result = false;
    for (const auto& result : results) {
        results_consistent = results_consistent && result.is_valid <= 1u;
        has_valid_result = has_valid_result || result.is_valid == 1u;
    }
    if (!results_consistent || (has_valid_result && batch_stats.chain_dispatches == 0u) ||
        backend_from_selected_params(params) !=
            ::bioseq::pipeline_stats::executed_backend::vulkan_gpu ||
        !selected_vulkan_execution(params) || !selected_vulkan_coordinate_prereqs_active(params) ||
        !pipe::is_gpu_target_index_build_active(params) || !runtime_execution.has_value() ||
        !runtime_execution->is_valid()) {
        return std::nullopt;
    }
    const std::size_t effective_workers =
        bioseq::parallelism::global_pool::instance().maximum_concurrency();
    auto environment =
        ::bioseq::pipeline_stats::make_persisted_grouped_soa_gpu_environment_snapshot(
            runtime_execution->environment_binding(), effective_workers);
    if (!environment.has_value()) {
        return std::nullopt;
    }
    environment->record_highway_target(params.highway_dispatch, params.highway_executed_target);
    if (!::bioseq::pipeline_stats::is_environment_valid_for_backend(
            *environment, ::bioseq::pipeline_stats::executed_backend::vulkan_gpu)) {
        return std::nullopt;
    }
    source_direct_vulkan_execution_acceptance accepted{
        .environment = *environment,
        .cache_metrics = runtime_execution->take_cache_metrics_delta(),
    };
    return accepted.is_valid()
               ? std::optional<source_direct_vulkan_execution_acceptance>{std::move(accepted)}
               : std::nullopt;
}

/// @brief Adds this GPU operation's cache metrics to the statement once.
/// @param[in,out] stats Statement statistics receiving the counts.
/// @param[in,out] execution Checked execution record whose metrics are consumed.
/// @return True if valid metrics were added. Reusing the record returns false.
[[nodiscard]] inline bool commit_source_direct_vulkan_cache_metrics(
    ::bioseq::pipeline_stats::pipeline_execution_stats& stats,
    source_direct_vulkan_execution_acceptance& execution) noexcept {
    if (!execution.is_valid()) {
        return false;
    }
    const auto authoritative = execution.cache_metrics.audit();
    ::bioseq::pipeline_stats::pipeline_operational_stats delta{};
    delta.gpu_pipeline_cache_loads = authoritative.gpu_pipeline_cache_loads;
    delta.gpu_pipeline_cache_seed_bytes = authoritative.gpu_pipeline_cache_seed_bytes;
    delta.gpu_pipeline_cache_load_ns = authoritative.gpu_pipeline_cache_load_ns;
    delta.gpu_pipeline_creates = authoritative.gpu_pipeline_creates;
    delta.gpu_pipeline_creation_ns = authoritative.gpu_pipeline_creation_ns;
    auto cache_metrics = std::move(execution.cache_metrics);
    if (!cache_metrics.is_valid() || cache_metrics.audit() != authoritative) {
        return false;
    }
    stats.merge_operational(delta);
    cache_metrics.consume();
    return true;
}

/// @brief Records one job from a completed GPU batch that built its target index.
///
/// Uses the supplied device information and stage flags to describe the work
/// performed for this target row and query number. Checks that the selected
/// pipeline and requested index source agree, then adds the job to stats.
/// @return True when the job passes validation and its statistics are added.
template <class p_scanner_pg_context>
[[nodiscard]] inline bool record_source_direct_vulkan_job(
    ::bioseq::pipeline_stats::pipeline_execution_stats& stats,
    const minimap2_parameters& params,
    const ::bioseq::pipeline_stats::target_index_source requested_source,
    const ::bioseq::pipeline_stats::execution_environment_snapshot& environment,
    const p_scanner_pg_context* scanner_pg_ctx,
    const std::size_t row_index,
    const bool per_row_query_mode,
    const std::uint64_t broadcast_query_ordinal,
    const bool seed_lookup_executed,
    const bool chaining_executed,
    const bool cigar_executed) noexcept {
    using backend_type = ::bioseq::pipeline_stats::executed_backend;
    using source_type = ::bioseq::pipeline_stats::target_index_source;
    const bool coordinate_projection =
        !params.do_cigar && pipe::is_gpu_coordinate_only_execution_active(params);
    const bool cigar_projection = params.do_cigar && params.vulkan_extend_enabled &&
                                  selected_vulkan_coordinate_prereqs_active(params);
    if (backend_from_selected_params(params) != backend_type::vulkan_gpu ||
        !selected_vulkan_execution(params) || (!coordinate_projection && !cigar_projection) ||
        !pipe::is_gpu_target_index_build_active(params) ||
        requested_source != source_type::recomputed ||
        !::bioseq::pipeline_stats::is_environment_valid_for_backend(environment,
                                                                    backend_type::vulkan_gpu) ||
        (cigar_executed && (!params.do_cigar || !chaining_executed))) {
        return false;
    }

    ::bioseq::pipeline_stats::pipeline_job_execution_stats job{};
    job.requested_backend = backend_type::vulkan_gpu;
    job.actual_backend = backend_type::vulkan_gpu;
    job.requested_target_index_source = source_type::recomputed;
    job.actual_target_index_source = source_type::recomputed;
    job.logical_target_row =
        scanner_pg_ctx != nullptr && scanner_pg_ctx->target_rownos[row_index] >= 0
            ? static_cast<std::uint64_t>(scanner_pg_ctx->target_rownos[row_index])
            : static_cast<std::uint64_t>(row_index);
    job.query_ordinal = scanner_pg_ctx != nullptr && scanner_pg_ctx->explicit_query_ordinals
                            ? scanner_pg_ctx->query_ordinals[row_index]
                        : per_row_query_mode ? static_cast<std::uint64_t>(row_index)
                                             : broadcast_query_ordinal;
    job.environment = environment;
    const auto mark = [&job](const ::bioseq::pipeline_stats::pipeline_stage stage,
                             const ::bioseq::pipeline_stats::execution_engine engine) noexcept {
        auto& coverage = job.stages.coverage[::bioseq::pipeline_stats::stage_index(stage)];
        coverage.eligible_jobs = 1u;
        coverage.executed_jobs[::bioseq::pipeline_stats::engine_index(engine)] = 1u;
    };
    using stage_type = ::bioseq::pipeline_stats::pipeline_stage;
    using engine_type = ::bioseq::pipeline_stats::execution_engine;
    mark(stage_type::query_sketch, engine_type::vulkan_gpu);
    mark(stage_type::target_sketch, engine_type::vulkan_gpu);
    mark(stage_type::target_index_build, engine_type::vulkan_gpu);
    if (seed_lookup_executed) {
        mark(stage_type::seed_lookup, engine_type::vulkan_gpu);
    }
    if (chaining_executed) {
        mark(stage_type::chain_score, engine_type::vulkan_gpu);
        mark(stage_type::chain_backtrack, engine_type::vulkan_gpu);
        mark(stage_type::region_generation, engine_type::vulkan_gpu);
    }
    if (cigar_executed) {
        mark(stage_type::target_window_plan, engine_type::vulkan_gpu);
        mark(stage_type::target_window_io, engine_type::host_io);
        mark(stage_type::target_window_upload, engine_type::vulkan_gpu);
        mark(stage_type::cigar_job_generation, engine_type::vulkan_gpu);
        mark(stage_type::left_extension, engine_type::vulkan_gpu);
        mark(stage_type::gap_extension, engine_type::vulkan_gpu);
        mark(stage_type::right_extension, engine_type::vulkan_gpu);
    }
    if (!::bioseq::pipeline_stats::is_job_execution_stats_valid(job)) {
        return false;
    }
    stats.merge_job(job);
    return true;
}

/// Checks and consumes a stored-index GPU coordinate record to prepare job
/// statistics. Returns no value if the record or requested settings disagree.
/// The caller still needs to validate the complete operation and attach its
/// GPU environment before publishing these statistics.
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
    const bool coordinate_projection =
        !params.do_cigar && pipe::is_gpu_coordinate_only_execution_active(params);
    const bool bounded_cigar_projection = params.do_cigar && params.vulkan_extend_enabled &&
                                          selected_vulkan_coordinate_prereqs_active(params);
    const bool accepted =
        job.requested_backend == ::bioseq::pipeline_stats::executed_backend::vulkan_gpu &&
        selected_vulkan_execution(params) && (coordinate_projection || bounded_cigar_projection) &&
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
        if (bounded_cigar_projection && consumed_validation->audit().result.is_valid == 1u) {
            mark(stage_type::target_window_plan, engine_type::vulkan_gpu);
            mark(stage_type::target_window_io, engine_type::host_io);
            mark(stage_type::target_window_upload, engine_type::vulkan_gpu);
            mark(stage_type::cigar_job_generation, engine_type::vulkan_gpu);
            mark(stage_type::left_extension, engine_type::vulkan_gpu);
            mark(stage_type::gap_extension, engine_type::vulkan_gpu);
            mark(stage_type::right_extension, engine_type::vulkan_gpu);
        }
    }
    return job;
}

/// Copies GPU cache counters into the statistics record used by SQL.
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

/// Copies the shader-cache validation results into SQL statistics. Helper
/// identity strings that exceed the fixed storage get length zero, which
/// makes the record invalid.
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

/// Copies one completed GPU job's work counts and coordinates into SQL statistics.
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
        .binding_window_count = audit.binding_window_count,
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

/// Builds operation statistics from the GPU runtime preview and publication details.
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
        .alignment_pipeline_receipt_count = acceptance.alignment_pipeline_receipt_count,
        .alignment_submission_count = acceptance.alignment_submission_count,
        .mapq_pipeline_receipt_count = acceptance.mapq_pipeline_receipt_count,
        .mapq_submission_count = acceptance.mapq_submission_count,
        .mapq_result_count = acceptance.mapq_result_count,
        .mapq_published_count = acceptance.mapq_published_count,
        .mapq_result_digest = acceptance.mapq_result_digest,
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

/// Estimates memory for the job records held while a stored-index GPU batch
/// prepares its SQL results. CIGAR input storage is reserved separately once
/// its sequence windows are known.
[[nodiscard]] inline std::uint64_t
persisted_grouped_soa_publication_staging_bytes(const std::uint64_t job_count) noexcept {
    using staged_job_type = ::bioseq::pipeline_stats::pipeline_job_execution_stats;

    std::uint64_t bytes_per_job = 0u;
    const auto add = [&bytes_per_job](const std::uint64_t bytes) noexcept {
        bytes_per_job = bioseq::common::saturating_add(bytes_per_job, bytes);
    };
    add(sizeof(bioseq::gpu::vulkan::gpu_seed_coordinate_job));
    add(sizeof(std::size_t));
    add(sizeof(bioseq::gpu::vulkan::gpu_coordinate_result_wire));
    // Reserve the SQL-side staged-job vector here. The GPU runtime reserves
    // memory for its own statistics and execution records separately.
    add(sizeof(staged_job_type));
#if !defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    using result_map_value_type = typename match_result_map::value_type;
    // Ordinary builds estimate result-map nodes and vectors together.
    // Benchmark builds reserve their actual allocations separately.
    add(sizeof(result_map_value_type));
    add(4u * sizeof(void*));
    add(bioseq::common::saturating_mul(2u, sizeof(match_result)));
#endif
    return bioseq::common::saturating_mul(
        bioseq::common::saturating_mul(job_count, bytes_per_job),
        bioseq::parallelism::k_vector_preallocation_headroom_factor);
}

/// Estimates CPU memory held for one GPU CIGAR job from its sequence windows.
[[nodiscard]] inline std::uint64_t
persisted_grouped_soa_cigar_staging_bytes(const std::uint64_t query_span,
                                          const std::uint64_t target_span) noexcept {
    // Each packed window needs at most one byte per symbol and seven padding bytes.
    const std::uint64_t window_bytes = bioseq::common::saturating_add(
        bioseq::common::saturating_add(query_span, target_span), 14u);
    // Reserve packed inputs and job metadata here. The runtime separately
    // reserves memory for downloaded CIGAR operations.
    std::uint64_t bytes = window_bytes;
    bytes = bioseq::common::saturating_add(
        bytes, sizeof(bioseq::gpu::vulkan::gpu_alignment_cigar_job_wire));
    bytes = bioseq::common::saturating_add(
        bytes, sizeof(bioseq::gpu::vulkan::gpu_alignment_candidate_binding_wire));
    bytes = bioseq::common::saturating_add(bytes, sizeof(match_result) + sizeof(std::int32_t));
    return bioseq::common::saturating_mul(
        bytes, bioseq::parallelism::k_vector_preallocation_headroom_factor);
}

inline void record_target_row(::bioseq::pipeline_stats::pipeline_execution_stats& stats,
                              const std::uint64_t rows = 1) noexcept {
    add_operational_counter(stats, stats.operational.target_rows, rows);
}

[[nodiscard]] inline ::bioseq::pipeline_stats::target_index_source
requested_target_index_source(const auto* scanner_pg_ctx) noexcept {
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
