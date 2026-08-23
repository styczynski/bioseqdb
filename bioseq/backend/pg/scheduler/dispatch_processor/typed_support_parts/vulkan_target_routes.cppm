/**
 * @file typed_support_parts/vulkan_target_routes.cppm
 * @brief Owns resident and persisted Vulkan target execution routes.
 */
module;

extern "C" {
#include "postgres.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#ifdef NDEBUG
#include <libassert/assert-macros.hpp>
#else
#include <libassert/assert.hpp>
#endif
#include <new>

export module bioseq.postgres.scheduler.dispatch_processor.typed_support.vulkan_target_routes;

import std;
import bioseq.common.logging;
import bioseq.postgres.scheduler.dispatch_processor.typed_support.policy_stats;
import bioseq.postgres.scheduler.dispatch_processor.typed_support.pg_alignment_errors;
import bioseq.postgres.scheduler.dispatch_processor.typed_support.vulkan_query_route;
import bioseq.highway.dispatch_contract;
import bioseq.common.hashing;
import bioseq.indexing.type;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.gpu_minimizer_index;
import bioseq.sequences;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.chaining.vulkan;
import bioseq.sequences.cigar;
import bioseq.pipeline.minimap_port;
import bioseq.pipeline.pipe;
import bioseq.gpu.vulkan.runtime;
import bioseq.instrumentation.clock;
import bioseq.storage.lazy_sequence;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;

export namespace bioseq::simd::typed_dispatch::typed_support_detail {

/**
 * Publishes one authenticated persisted grouped-SoA operation atomically.
 *
 * All result, pipeline-stat, source-identity, lifecycle, and SQL-ledger
 * mutations remain staged until the final no-throw publication boundary.
 *
 * @tparam p_sequence Query sequence type carried by @p state.
 * @tparam p_runtime Vulkan runtime that executes seed-to-coordinate and finalizes
 *         the persisted grouped-SoA operation receipt.
 * @tparam p_prepared_target Prepared persisted grouped-SoA resident target handle.
 * @throws bioseq::gpu::vulkan::vulkan_runtime_error when admission, identity,
 *         receipt, or stats contracts fail before the publication boundary.
 */
template <class p_sequence,
          std::size_t p_batch_capacity,
          class p_scanner_context,
          class p_runtime,
          class p_prepared_target>
void process_vulkan_persisted_route(const size_t batch_size,
                                    const std::array<int32_t, p_batch_capacity>& query_ids,
                                    const minimap2_parameters& dispatch_params,
                                    const std::uint64_t broadcast_query_ordinal,
                                    ::bioseq::pipeline_stats::pipeline_execution_stats& stats,
                                    p_scanner_context* scanner_pg_ctx,
                                    const bool per_row_query_mode,
                                    const bool record_target_operational_outcomes,
                                    const bool gpu_persisted_prepared,
                                    p_runtime& runtime,
                                    p_prepared_target* const prepared_gpu_target,
                                    vulkan_query_batch_state<p_sequence, p_batch_capacity>& state,
                                    std::map<int32_t, std::vector<match_result>>& results) {
    using minimizer_type =
        typename vulkan_query_batch_state<p_sequence, p_batch_capacity>::minimizer_type;
    auto& query_records = state.query_records;
    auto& query_minimizer_device_span = state.query_minimizer_device_span;
    auto& query_sketch_authority = state.query_sketch_authority;
    auto& query_sidecars = state.query_sidecars;
    auto& row_to_query_index = state.row_to_query_index;
    const std::uint64_t gpu_query_sketch_host_ns = state.gpu_query_sketch_host_ns;
    if (!gpu_persisted_prepared || !prepared_gpu_target->ready() ||
        prepared_gpu_target->resident == nullptr || query_records.size() != query_sidecars.size() ||
        prepared_gpu_target->target_bp_size >
            static_cast<std::uint64_t>((std::numeric_limits<sequence_size_t>::max)())) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "persisted GPU grouped-SoA prepared-target identity is invalid");
    }
    const auto requested_source = requested_target_index_source(scanner_pg_ctx);
    if (requested_source != ::bioseq::pipeline_stats::target_index_source::persisted ||
        dispatch_params.max_occ < 0 ||
        static_cast<std::uint64_t>(dispatch_params.max_occ) >
            prepared_gpu_target->resident_view.max_occ_cap) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "persisted GPU grouped-SoA source or max_occ contract is invalid");
    }

    /* Atomic publication keeps the direct inputs, runtime results
     * and staged SQL values alive together. Admit that transient overlap
     * before the first covered N-job reserve. */
    const std::uint64_t publication_staging_bytes = persisted_grouped_soa_publication_staging_bytes(
        static_cast<std::uint64_t>(query_sidecars.size()));
    if (publication_staging_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "persisted GPU grouped-SoA publication staging admission overflow");
    }
    auto publication_staging_permit =
        bioseq::parallelism::memory_admission_controller::instance().try_reserve(
            publication_staging_bytes);
    if (!publication_staging_permit.has_value()) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "persisted GPU grouped-SoA publication staging admission was rejected");
    }

    /** Per-job pipeline stats row staged before the publication boundary merges them. */
    using staged_job_type = ::bioseq::pipeline_stats::pipeline_job_execution_stats;
    std::vector<bioseq::gpu::vulkan::gpu_seed_coordinate_job> direct_jobs;
    std::vector<std::size_t> direct_job_rows;
    direct_jobs.reserve(query_sidecars.size());
    direct_job_rows.reserve(query_sidecars.size());
    for (const gpu_batch_query_sidecar& query : query_sidecars) {
        const std::size_t row = query.row_index;
        const std::uint32_t query_index = row_to_query_index[row];
        if (query_index == std::numeric_limits<std::uint32_t>::max() ||
            query_index >= query_records.size() ||
            scanner_pg_ctx->target_rownos[row] != prepared_gpu_target->target_rowno) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "persisted GPU grouped-SoA query/target row identity is inconsistent");
        }
        direct_jobs.push_back(bioseq::gpu::vulkan::gpu_seed_coordinate_job{
            .slots = nullptr,
            .positions = nullptr,
            .target_len = prepared_gpu_target->target_bp_size,
            .slot_mask = 0u,
            .position_count = 0u,
            .bucket_count = 0u,
            .target_position_offset = 0u,
            .query_index = query_index,
            .logical_target_row = static_cast<std::uint64_t>(scanner_pg_ctx->target_rownos[row]),
            .logical_query_ordinal = scanner_pg_ctx->explicit_query_ordinals
                                         ? scanner_pg_ctx->query_ordinals[row]
                                     : per_row_query_mode ? static_cast<std::uint64_t>(row)
                                                          : broadcast_query_ordinal,
            .grouped_soa = &prepared_gpu_target->resident_view,
        });
        direct_job_rows.push_back(row);
    }
    if (direct_jobs.empty()) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "persisted GPU grouped-SoA batch has no admissible GPU query job");
    }
    if (query_minimizer_device_span.count() >
        static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)())) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "persisted GPU grouped-SoA query minimizer count exceeds ABI");
    }

    bioseq::gpu::vulkan::gpu_seed_query_inputs query_inputs{
        .host_minimizer_bytes = {},
        .device_minimizer_words = query_minimizer_device_span.words(),
        .minimizer_count = static_cast<std::uint32_t>(query_minimizer_device_span.count()),
        .minimizer_word_kind = static_cast<std::uint32_t>(
            bioseq::gpu::vulkan::minimizer_wire_traits<minimizer_type>::k_word_kind),
        .minimizer_words32_per_record =
            bioseq::gpu::vulkan::minimizer_wire_traits<minimizer_type>::k_words32_per_minimizer,
        .hash_shift = bioseq::gpu::vulkan::minimizer_wire_traits<minimizer_type>::k_hash_shift,
        .pos_bits = bioseq::gpu::vulkan::minimizer_wire_traits<minimizer_type>::k_position_bits,
        .window_size = static_cast<std::uint32_t>(dispatch_params.window_size),
        .host_query_records =
            std::span<const bioseq::gpu::vulkan::gpu_seed_query_record_wire>{query_records.data(),
                                                                             query_records.size()},
        .device_query_records = nullptr,
    };
    auto chaining_parameters = dispatch_params.make_chaining_parameters();
    const std::int32_t max_dist_x =
        std::max(chaining_parameters.max_gap, chaining_parameters.bandwidth);
    const std::int32_t max_dist_y =
        std::max(chaining_parameters.max_gap, chaining_parameters.bandwidth);
    const auto chain_key = bioseq::chaining::choose_vulkan_chaining_pipeline(runtime.features());
    constexpr auto coordinate_algorithm =
        bioseq::gpu::vulkan::gpu_seed_coordinate_algorithm::vulkan_tiled_dp;
    const std::int32_t max_iter = std::min<std::int32_t>(chaining_parameters.max_chain_iter, 3000);
    constexpr std::uint32_t probe_limit = 16u;
    const auto coordinate_configuration =
        bioseq::gpu::vulkan::make_persisted_grouped_soa_coordinate_config_identity(
            static_cast<std::uint32_t>(dispatch_params.kmer_size),
            dispatch_params.max_occ,
            coordinate_algorithm,
            chain_key,
            max_dist_x,
            max_dist_y,
            chaining_parameters.bandwidth,
            max_iter,
            chaining_parameters.gap_penalty,
            chaining_parameters.skip_penalty,
            chaining_parameters.max_chain_skip,
            chaining_parameters.min_chain_score,
            chaining_parameters.min_seed_count,
            probe_limit);
    const std::uint64_t query_authority_identity = query_sketch_authority.identity();
    const auto* const query_minimizer_buffer_identity = query_inputs.device_minimizer_words;
    auto coord_run = runtime.run_seed_to_coordinate_batch_roundtrip(
        query_inputs,
        std::span<const bioseq::gpu::vulkan::gpu_seed_coordinate_job>{direct_jobs.data(),
                                                                      direct_jobs.size()},
        static_cast<std::uint32_t>(dispatch_params.kmer_size),
        dispatch_params.max_occ,
        coordinate_algorithm,
        chain_key,
        max_dist_x,
        max_dist_y,
        chaining_parameters.bandwidth,
        max_iter,
        chaining_parameters.gap_penalty,
        chaining_parameters.skip_penalty,
        chaining_parameters.max_chain_skip,
        chaining_parameters.min_chain_score,
        chaining_parameters.min_seed_count,
        probe_limit,
        std::move(query_sketch_authority));

    const auto& direct = coord_run.grouped_soa_direct;
    const bool direct_validation_valid =
        direct.selected && direct.direct_count_scan_emit_completed &&
        direct_job_rows.size() == direct_jobs.size() &&
        direct.problem_count == direct_jobs.size() &&
        coord_run.stats.problem_count == direct_jobs.size() &&
        coord_run.results.size() == direct_jobs.size() &&
        direct.generation_id == prepared_gpu_target->resident_view.generation_id &&
        direct.source_identity == prepared_gpu_target->resident_view.source_identity &&
        direct.cold_load_identity == prepared_gpu_target->resident_view.cold_load_identity &&
        direct.count_dispatches == coord_run.stats.seed_count_dispatches &&
        direct.emit_dispatches == coord_run.stats.seed_emit_dispatches &&
        direct.count_status_checks == direct.count_dispatches &&
        direct.emit_status_checks == direct.emit_dispatches && direct.status_or == 0u &&
        direct.cpu_or_simd_fallbacks == 0u && coord_run.stats.coordinate_vote_dispatches == 0u &&
        coord_run.operation_receipt.is_sealed() &&
        coord_run.operation_receipt.job_count() == direct_jobs.size();
    if (!direct_validation_valid) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "persisted GPU grouped-SoA direct-route stats are incomplete or inconsistent");
    }

    ::bioseq::pipeline_stats::pipeline_execution_stats staged_stats{};
    add_stage_host_time(staged_stats,
                        ::bioseq::pipeline_stats::pipeline_stage::query_sketch,
                        gpu_query_sketch_host_ns);
    add_operational_counter(staged_stats,
                            staged_stats.operational.gpu_seed_dispatches,
                            coord_run.stats.seed_count_dispatches +
                                coord_run.stats.seed_emit_dispatches);
    add_operational_counter(staged_stats,
                            staged_stats.operational.gpu_chain_dispatches,
                            coord_run.stats.chain_dispatches);
    add_operational_counter(staged_stats,
                            staged_stats.operational.gpu_chain_backtrack_dispatches,
                            coord_run.stats.chain_backtrack_dispatches);
    const std::uint64_t resident_cache_hits = static_cast<std::uint64_t>(direct_jobs.size()) -
                                              (prepared_gpu_target->dispatch_waves == 0u ? 1u : 0u);
    add_operational_counter(staged_stats,
                            staged_stats.operational.gpu_persisted_resident_cache_hits,
                            resident_cache_hits);

    std::vector<staged_job_type> staged_jobs;
    staged_jobs.reserve(direct_jobs.size());
    std::remove_reference_t<decltype(results)> staged_results;
    std::array<bool, p_batch_capacity> staged_rows{};

    for (std::size_t problem_index = 0; problem_index < direct_jobs.size(); ++problem_index) {
        const std::size_t row = direct_job_rows[problem_index];
        if (row >= batch_size || row >= p_batch_capacity || staged_rows[row] ||
            query_ids[row] < 0 ||
            direct_jobs[problem_index].logical_target_row >
                ::bioseq::pipeline_stats::k_sql_bigint_value_max ||
            direct_jobs[problem_index].logical_query_ordinal >
                ::bioseq::pipeline_stats::k_sql_bigint_value_max ||
            prepared_gpu_target->resident_view.generation_id >
                ::bioseq::pipeline_stats::k_sql_bigint_value_max ||
            scanner_pg_ctx->target_index_sources[row] !=
                ::bioseq::pipeline_stats::target_index_source::none ||
            scanner_pg_ctx->persisted_generation_ids[row] != 0u ||
            scanner_pg_ctx->persisted_source_identities[row] != 0u) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "persisted GPU grouped-SoA staged row publication is invalid");
        }
        staged_rows[row] = true;
        const auto& coord = coord_run.results[problem_index];
        const auto& query_record = query_records[direct_jobs[problem_index].query_index];
        auto staged_job = stage_persisted_grouped_soa_coordinate_job(
            dispatch_params,
            requested_source,
            coord_run.operation_receipt,
            bioseq::gpu::vulkan::persisted_grouped_soa_coordinate_job_expectation{
                .problem_index = problem_index,
                .query_authority_identity = query_authority_identity,
                .resident_descriptors = bioseq::gpu::vulkan::runtime_buffer_identity(
                    prepared_gpu_target->resident_view.descriptors),
                .resident_controls = bioseq::gpu::vulkan::runtime_buffer_identity(
                    prepared_gpu_target->resident_view.controls),
                .resident_positions = bioseq::gpu::vulkan::runtime_buffer_identity(
                    prepared_gpu_target->resident_view.positions),
                .query_minimizer_buffer =
                    bioseq::gpu::vulkan::runtime_buffer_identity(query_minimizer_buffer_identity),
                .device_uuid = prepared_gpu_target->resident_view.device_uuid,
                .descriptor_bytes = prepared_gpu_target->resident_view.descriptor_bytes,
                .control_bytes = prepared_gpu_target->resident_view.control_bytes,
                .position_bytes = prepared_gpu_target->resident_view.position_bytes,
                .position_count = prepared_gpu_target->resident_view.position_count,
                .max_occ_cap = prepared_gpu_target->resident_view.max_occ_cap,
                .partition_count = prepared_gpu_target->resident_view.partition_count,
                .abi_revision = prepared_gpu_target->resident_view.abi_revision,
                .generation_id = prepared_gpu_target->resident_view.generation_id,
                .source_identity = prepared_gpu_target->resident_view.source_identity,
                .cold_load_identity = prepared_gpu_target->resident_view.cold_load_identity,
                .target_len = prepared_gpu_target->target_bp_size,
                .query_index = direct_jobs[problem_index].query_index,
                .logical_target_row = direct_jobs[problem_index].logical_target_row,
                .logical_query_ordinal = direct_jobs[problem_index].logical_query_ordinal,
                .query_record = query_record,
                .result = coord,
                .configuration = coordinate_configuration,
            });
        if (!staged_job.has_value()) {
            continue;
        }
        staged_jobs.push_back(std::move(*staged_job));
        if (coord.is_valid != 0u) {
            const std::uint32_t query_index = direct_jobs[problem_index].query_index;
            staged_results[query_ids[row]].push_back(pipe::gpu_coordinate_to_match_result(
                coord,
                query_sidecars[query_index].query_len,
                static_cast<sequence_size_t>(prepared_gpu_target->target_bp_size)));
        }
    }
    const std::uint64_t expected_jobs = static_cast<std::uint64_t>(direct_jobs.size());
    const std::uint64_t validated_jobs =
        static_cast<std::uint64_t>(coord_run.operation_receipt.job_count());
    const std::uint64_t consumed_jobs =
        static_cast<std::uint64_t>(coord_run.operation_receipt.consumed_count());
    const std::uint64_t pending_jobs = validated_jobs >= consumed_jobs
                                           ? validated_jobs - consumed_jobs
                                           : (std::numeric_limits<std::uint64_t>::max)();
    const ::bioseq::pipeline_stats::persisted_grouped_soa_coordinate_publication_stats
        publication_counts{
            .expected_jobs = expected_jobs,
            .validated_jobs = validated_jobs,
            .consumed_jobs = consumed_jobs,
            .accepted_jobs =
                static_cast<std::uint64_t>(coord_run.operation_receipt.accepted_count()),
            .burned_jobs = static_cast<std::uint64_t>(coord_run.operation_receipt.burned_count()),
            .pending_jobs = pending_jobs,
            .staged_jobs = static_cast<std::uint64_t>(staged_jobs.size()),
        };
    if (!::bioseq::pipeline_stats::is_persisted_grouped_soa_coordinate_publication_consistent(
            publication_counts) ||
        !coord_run.operation_receipt.are_all_jobs_accepted()) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "persisted GPU grouped-SoA operation stats were not accepted exactly once");
    }
    const bool lifecycle_prestate_valid =
        prepared_gpu_target->query_count != 0u &&
        prepared_gpu_target->query_count <= ::bioseq::pipeline_stats::k_sql_bigint_value_max &&
        prepared_gpu_target->dispatched_query_count <= prepared_gpu_target->query_count &&
        (prepared_gpu_target->dispatched_query_count == 0u
             ? prepared_gpu_target->dispatch_waves == 0u
             : prepared_gpu_target->dispatch_waves != 0u) &&
        expected_jobs <=
            prepared_gpu_target->query_count - prepared_gpu_target->dispatched_query_count;
    if (!lifecycle_prestate_valid ||
        prepared_gpu_target->dispatched_query_count >
            ::bioseq::pipeline_stats::k_sql_bigint_value_max - expected_jobs ||
        prepared_gpu_target->dispatch_waves >= ::bioseq::pipeline_stats::k_sql_bigint_value_max) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "persisted GPU grouped-SoA lifecycle counters overflow");
    }

    if (!results.empty()) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "persisted GPU grouped-SoA result staging did not start empty");
    }
    const std::size_t effective_workers =
        bioseq::parallelism::global_pool::instance().maximum_concurrency();
    if (effective_workers == 0u ||
        effective_workers > ::bioseq::pipeline_stats::k_sql_integer_value_max) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "persisted GPU grouped-SoA CPU worker identity is invalid");
    }
    const auto cache_metrics_preview = coord_run.operation_receipt.cache_metrics_preview();
    const auto environment_preview = coord_run.operation_receipt.environment_preview();
    const auto staged_environment =
        ::bioseq::pipeline_stats::make_persisted_grouped_soa_gpu_environment_snapshot(
            environment_preview, effective_workers);
    const auto maximum_cache_metrics =
        ::bioseq::pipeline_stats::make_persisted_grouped_soa_gpu_cache_metrics_delta(
            cache_metrics_preview);
    /* These are immutable/bounded maxima, not counters to publish. They
     * let every environment, SQL range and prospective-merge rejection
     * happen before the finalizer claims report-once cache bits. */
    if (!staged_environment.has_value() || !maximum_cache_metrics.has_value() ||
        environment_preview.gpu_device_uuid != prepared_gpu_target->resident_view.device_uuid ||
        (cache_metrics_preview.gpu_pipeline_cache_loads != 0u &&
         cache_metrics_preview.gpu_pipeline_cache_seed_bytes != environment_preview.seed_size)) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "persisted GPU grouped-SoA environment/cache preview is invalid");
    }
    auto complete_environment = *staged_environment;
    complete_environment.record_highway_target(dispatch_params.highway_dispatch,
                                               dispatch_params.highway_executed_target);
    if (!::bioseq::pipeline_stats::is_environment_valid_for_backend(
            complete_environment, ::bioseq::pipeline_stats::executed_backend::vulkan_gpu)) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "persisted GPU grouped-SoA Highway environment stats are invalid");
    }
    for (staged_job_type& job : staged_jobs) {
        job.environment = complete_environment;
        staged_stats.merge_job(job);
    }
    if (record_target_operational_outcomes) {
        record_target_row(staged_stats);
        record_target_index_outcome(staged_stats,
                                    requested_source,
                                    ::bioseq::pipeline_stats::target_index_source::persisted);
    }
    auto maximum_staged_stats = staged_stats;
    maximum_staged_stats.merge_operational(*maximum_cache_metrics);
    auto maximum_prospective_stats = stats;
    maximum_prospective_stats.merge(maximum_staged_stats);
    if (!::bioseq::pipeline_stats::is_persisted_grouped_soa_coordinate_prospective_stats_consistent(
            stats, maximum_staged_stats, maximum_prospective_stats, expected_jobs)) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "persisted GPU grouped-SoA prospective stats are invalid");
    }

    /* SQL stats use a separate admitted statement-root ledger. Build,
     * validate and preflight its complete operation image now: nothing may
     * allocate, search for duplicates or decide capacity after Runtime's
     * final immutable gate claims report-once cache authority. */
    const auto operation_receipt_preview = coord_run.operation_receipt.operation_receipt_preview();
    auto operation_stats_chunk =
        ::bioseq::pipeline_stats::persisted_gpu_operation_chunk::prepare(direct_jobs.size());
    if (!operation_receipt_preview.is_internally_consistent() || operation_stats_chunk == nullptr ||
        scanner_pg_ctx->persisted_gpu_operation_stats_sink == nullptr ||
        operation_receipt_preview.device_uuid != prepared_gpu_target->resident_view.device_uuid ||
        operation_receipt_preview.acceptance.operation_identity !=
            cache_metrics_preview.token_identity) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "persisted GPU grouped-SoA SQL operation stats are unavailable");
    }
    operation_stats_chunk->operation() =
        make_persisted_gpu_operation_stats(operation_receipt_preview,
                                           publication_counts,
                                           prepared_gpu_target->resident_view.generation_id,
                                           prepared_gpu_target->resident_view.source_identity,
                                           prepared_gpu_target->resident_view.cold_load_identity);
    for (std::size_t problem_index = 0u; problem_index < direct_jobs.size(); ++problem_index) {
        const auto job_stats_preview =
            coord_run.operation_receipt.job_evidence_preview(problem_index);
        if (!job_stats_preview.has_value() ||
            job_stats_preview->audit.operation_identity !=
                operation_receipt_preview.acceptance.operation_identity ||
            job_stats_preview->audit.generation_id !=
                prepared_gpu_target->resident_view.generation_id ||
            job_stats_preview->audit.source_identity !=
                prepared_gpu_target->resident_view.source_identity ||
            job_stats_preview->audit.cold_load_identity !=
                prepared_gpu_target->resident_view.cold_load_identity ||
            job_stats_preview->audit.logical_target_row !=
                direct_jobs[problem_index].logical_target_row ||
            job_stats_preview->audit.logical_query_ordinal !=
                direct_jobs[problem_index].logical_query_ordinal ||
            job_stats_preview->audit.query_index != direct_jobs[problem_index].query_index) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "persisted GPU grouped-SoA SQL job stats are inconsistent");
        }
        operation_stats_chunk->jobs()[problem_index] =
            make_persisted_gpu_job_stats(*job_stats_preview);
    }
    if (!operation_stats_chunk->seal()) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "persisted GPU grouped-SoA SQL operation stats did not seal");
    }
    auto operation_stats_append =
        scanner_pg_ctx->persisted_gpu_operation_stats_sink->prepare_append(operation_stats_chunk);
    if (!operation_stats_append.is_valid()) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "persisted GPU grouped-SoA SQL stats ledger rejected the operation");
    }

    /* Every allocation, row check and overflow check is complete.
     * The runtime finalizer validates the causal receipt union and
     * performs the final immutable-policy live gate before minting
     * this move-only authority. */
    auto accepted_operation = runtime.finalize_persisted_grouped_soa_coordinate_operation(
        std::move(coord_run.operation_receipt), operation_receipt_preview);
    const auto& acceptance = accepted_operation.audit();
    const auto& authoritative_environment = accepted_operation.environment_binding();
    const auto authoritative_cache_metrics = accepted_operation.cache_metrics_audit();
    ::bioseq::pipeline_stats::pipeline_operational_stats authoritative_cache_delta{};
    authoritative_cache_delta.gpu_pipeline_cache_loads =
        authoritative_cache_metrics.gpu_pipeline_cache_loads;
    authoritative_cache_delta.gpu_pipeline_cache_seed_bytes =
        authoritative_cache_metrics.gpu_pipeline_cache_seed_bytes;
    authoritative_cache_delta.gpu_pipeline_cache_load_ns =
        authoritative_cache_metrics.gpu_pipeline_cache_load_ns;
    authoritative_cache_delta.gpu_pipeline_creates =
        authoritative_cache_metrics.gpu_pipeline_creates;
    authoritative_cache_delta.gpu_pipeline_creation_ns =
        authoritative_cache_metrics.gpu_pipeline_creation_ns;
    auto committed_staged_stats = staged_stats;
    committed_staged_stats.merge_operational(authoritative_cache_delta);
    auto prospective_stats = stats;
    prospective_stats.merge(committed_staged_stats);

    /* The report-once token must be the final authority staged into the
     * prospective aggregate. Consume it only when publication is now
     * inevitable; a copied environment receipt cannot replay these
     * counters. */
    auto cache_metrics_delta = accepted_operation.take_cache_metrics_delta();
    static_assert(
        std::is_nothrow_copy_constructible_v<::bioseq::pipeline_stats::pipeline_execution_stats>);
    static_assert(
        std::is_nothrow_copy_assignable_v<::bioseq::pipeline_stats::pipeline_execution_stats>);
    static_assert(noexcept(committed_staged_stats.merge_operational(authoritative_cache_delta)));
    static_assert(noexcept(prospective_stats.merge(committed_staged_stats)));
    static_assert(std::is_nothrow_move_constructible_v<decltype(results)>);
    static_assert(noexcept(results.swap(staged_results)));
    static_assert(
        noexcept(::bioseq::pipeline_stats::is_persisted_grouped_soa_gpu_environment_consistent(
            authoritative_environment, environment_preview)));
    static_assert(
        noexcept(::bioseq::pipeline_stats::is_persisted_grouped_soa_gpu_cache_metrics_bounded_by(
            authoritative_cache_metrics, cache_metrics_preview)));
    static_assert(noexcept(cache_metrics_delta.consume()));
    static_assert(noexcept(operation_stats_chunk->publish_cache_metrics(
        make_persisted_gpu_cache_metrics_stats(authoritative_cache_metrics))));
    static_assert(noexcept(scanner_pg_ctx->persisted_gpu_operation_stats_sink->commit_append(
        std::move(operation_stats_append), std::move(operation_stats_chunk))));
    /* The finalizer compared this same preview before its live gate
     * and report-once claim. These are construction assertions, not a
     * recoverable post-finalizer rejection path. */
    DEBUG_ASSERT(accepted_operation.is_valid(),
                 "persisted GPU accepted operation authority vanished");
    DEBUG_ASSERT(acceptance == operation_receipt_preview.acceptance,
                 "persisted GPU operation changed after finalizer");
    DEBUG_ASSERT(acceptance.expected_job_count == expected_jobs,
                 "persisted GPU job count changed after finalizer");
    DEBUG_ASSERT(::bioseq::pipeline_stats::is_persisted_grouped_soa_gpu_environment_consistent(
                     authoritative_environment, environment_preview),
                 "persisted GPU environment changed after finalizer");
    DEBUG_ASSERT(::bioseq::pipeline_stats::is_persisted_grouped_soa_gpu_cache_metrics_bounded_by(
                     authoritative_cache_metrics, cache_metrics_preview),
                 "persisted GPU cache metrics exceeded preview");
    DEBUG_ASSERT(cache_metrics_delta.is_valid() &&
                     cache_metrics_delta.audit() == authoritative_cache_metrics,
                 "persisted GPU cache token changed after finalizer");
    /**
     * Applies the finalized persisted grouped-SoA publication: publishes cache metrics,
     * burns the one-shot cache token, swaps staged match results and stats, stamps
     * scanner row identities, advances target lifecycle counters, and commits the
     * sealed SQL operation chunk. Runs noexcept after all validation passes.
     */
    const auto commit_publication = [&authoritative_cache_metrics,
                                     &cache_metrics_delta,
                                     &results,
                                     &staged_results,
                                     &stats,
                                     &prospective_stats,
                                     &direct_job_rows,
                                     &scanner_pg_ctx,
                                     &prepared_gpu_target,
                                     &expected_jobs,
                                     &operation_stats_chunk,
                                     &operation_stats_append]() noexcept {
        operation_stats_chunk->publish_cache_metrics(
            make_persisted_gpu_cache_metrics_stats(authoritative_cache_metrics));
        /* accepted() plus the exact audit equality above are consume()'s
         * complete precondition. Burn the one-shot authority now; no
         * rejecting edge may follow before SQL-visible publication. */
        cache_metrics_delta.consume();
        results.swap(staged_results);
        stats = prospective_stats;
        for (const std::size_t row : direct_job_rows) {
            scanner_pg_ctx->target_index_sources[row] =
                ::bioseq::pipeline_stats::target_index_source::persisted;
            scanner_pg_ctx->persisted_generation_ids[row] =
                prepared_gpu_target->resident_view.generation_id;
            scanner_pg_ctx->persisted_source_identities[row] =
                prepared_gpu_target->resident_view.source_identity;
        }
        prepared_gpu_target->dispatched_query_count += expected_jobs;
        ++prepared_gpu_target->dispatch_waves;
        scanner_pg_ctx->persisted_gpu_operation_stats_sink->commit_append(
            std::move(operation_stats_append), std::move(operation_stats_chunk));
    };
    static_assert(noexcept(commit_publication()));
    commit_publication();
    return;
}

/**
 * Describes one logical target and every admitted resident segment.
 */
template <class p_gpu_row_view, class p_sequence_header> struct gpu_batch_target_sidecar {
    const p_sequence_header* hdr = nullptr;
    std::optional<p_gpu_row_view> recipe_view{};
    Datum recipe_datum = Datum{0};
    std::size_t source_row_index = 0u;
    std::uint64_t fingerprint = 0;
    sequence_size_t target_len = 0;
    sequence_size_t max_query_len = 0;
    std::size_t row_count = 0;
    std::vector<::bioseq::indexing::segment_unit_range> ranges{};
    std::vector<std::shared_ptr<bioseq::indexing::gpu_resident_minimizer_index>> residents{};
};

/**
 * Maps one submitted coordinate problem back to its logical row and segment.
 */
struct gpu_batch_problem_sidecar {
    std::size_t row_index = 0;
    std::size_t target_index = 0;
    std::size_t segment_index = 0;
    sequence_size_t core_lo_bp = 0;
    sequence_size_t core_hi_bp = 0;
};

/**
 * Executes transient resident target preparation, batched coordinates, and
 * optional bounded GPU CIGAR projection for one admitted Vulkan batch.
 */
template <class p_sequence,
          std::size_t p_batch_capacity,
          class p_sequence_header,
          class p_scanner_context,
          class p_same_target_locator,
          class p_runtime>
void process_vulkan_resident_route(
    const size_t batch_size,
    const uint8_t alphabet_code,
    const uint8_t storage_unit_byte_width,
    const std::size_t facade_letters_per_unit,
    const p_sequence_header* query_hdr,
    const std::array<const p_sequence_header*, p_batch_capacity>* per_row_query_hdrs,
    const std::array<const p_sequence_header*, p_batch_capacity>& target_hdrs,
    const std::array<int32_t, p_batch_capacity>& query_ids,
    const minimap2_parameters& dispatch_params,
    const std::uint64_t broadcast_query_ordinal,
    ::bioseq::pipeline_stats::pipeline_execution_stats& stats,
    p_scanner_context* scanner_pg_ctx,
    p_same_target_locator& same_target_locator,
    const bool per_row_query_mode,
    const bool record_target_operational_outcomes,
    bool& cached_query_minimizer_stats_accounted,
    p_runtime& runtime,
    vulkan_query_batch_state<p_sequence, p_batch_capacity>& state,
    std::map<int32_t, std::vector<match_result>>& results) {
    using state_type = vulkan_query_batch_state<p_sequence, p_batch_capacity>;
    using minimizer_type = typename state_type::minimizer_type;
    using gpu_row_view = typename state_type::gpu_row_view;
    using target_sidecar_type = gpu_batch_target_sidecar<gpu_row_view, p_sequence_header>;
    auto& gpu_query_recipe_views = state.gpu_query_recipe_views;
    auto& gpu_target_recipe_views = state.gpu_target_recipe_views;
    const std::size_t window_size = state.window_size;
    const std::size_t kmer_size = state.kmer_size;
    auto& query_records = state.query_records;
    auto& query_minimizer_device_span = state.query_minimizer_device_span;
    auto& query_sketch_authority = state.query_sketch_authority;
    auto& query_sidecars = state.query_sidecars;
    auto& row_to_query_index = state.row_to_query_index;
    // The ordered gate belongs only to the resident target route. Persisted
    // grouped-SoA execution consumes the authenticated device query span
    // directly and must not pull the full pipeline graph into query admission.
    pipe::ordered_sketch_query_gate_storage query_gate;
    const std::uint64_t gpu_query_sketch_host_ns = state.gpu_query_sketch_host_ns;
    /** Returns monotonic time in nanoseconds for GPU batch stage accounting. */
    const auto gpu_batch_now_ns = []() -> std::uint64_t {
        return static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns());
    };
    add_stage_host_time(
        stats, ::bioseq::pipeline_stats::pipeline_stage::query_sketch, gpu_query_sketch_host_ns);

    std::vector<target_sidecar_type> targets;
    std::vector<std::uint32_t> row_to_target_index(batch_size,
                                                   std::numeric_limits<std::uint32_t>::max());
    targets.reserve(batch_size);
    /** Exposes packed target storage bytes for equality checks and fingerprinting. */
    const auto target_storage_bytes = [](const p_sequence_header* hdr) -> std::string_view {
        compact_sequence_range<p_sequence> seq{*hdr};
        using storage_unit_type = typename compact_sequence_range<p_sequence>::traits::storage_unit;
        const auto* bytes = reinterpret_cast<const char*>(seq.cbegin());
        return std::string_view{bytes, seq.size() * sizeof(storage_unit_type)};
    };
    /** Hashes alphabet, width, length, and payload bytes to identify duplicate flat targets. */
    const auto target_fingerprint =
        [target_storage_bytes](const p_sequence_header* hdr) -> std::uint64_t {
        std::uint64_t hash = 14695981039346656037ull;
        hash =
            bioseq::common::hash_combine_64(hash, static_cast<std::uint64_t>(hdr->alphabet_code));
        hash = bioseq::common::hash_combine_64(
            hash, static_cast<std::uint64_t>(hdr->storage_unit_byte_width));
        hash = bioseq::common::hash_combine_64(hash, static_cast<std::uint64_t>(hdr->bp_size));
        return bioseq::common::hash_combine_64(hash, target_storage_bytes(hdr));
    };
    for (std::size_t row_index = 0; row_index < batch_size; ++row_index) {
        if (row_to_query_index[row_index] == std::numeric_limits<std::uint32_t>::max()) {
            continue;
        }
        const p_sequence_header* target_hdr = target_hdrs[row_index];
        const bool recipe_target = gpu_target_recipe_views[row_index].has_value();
        const Datum target_recipe_datum =
            recipe_target ? scanner_pg_ctx->target_recipe_datums[row_index] : Datum{0};
        if (target_hdr != nullptr && recipe_target) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "Vulkan target recipe overlaps a flat target");
        }
        if (target_hdr == nullptr && !recipe_target) {
            if (scanner_pg_ctx != nullptr &&
                scanner_pg_ctx->target_locators[row_index].has_value()) {
                const auto& loc = *scanner_pg_ctx->target_locators[row_index];
                throw bioseq::gpu::vulkan::vulkan_runtime_error(
                    fmt::format("selected Vulkan batch target row {} "
                                "is only available as a PG locator "
                                "({} bp); selected Vulkan requires a "
                                "materialized target payload and "
                                "cannot use scanner_pg/MaxAllocSize "
                                "streaming",
                                static_cast<int>(query_ids[row_index]),
                                static_cast<unsigned long long>(loc.logical_length)));
            }
            continue;
        }
        if (target_hdr != nullptr &&
            (target_hdr->alphabet_code != alphabet_code ||
             target_hdr->storage_unit_byte_width != storage_unit_byte_width)) {
            continue;
        }
        std::uint32_t target_index = std::numeric_limits<std::uint32_t>::max();
        for (std::size_t existing = 0; existing < targets.size(); ++existing) {
            const bool same_source_recipe = recipe_target && target_recipe_datum != Datum{0} &&
                                            targets[existing].recipe_datum == target_recipe_datum;
            const bool same_source_locator =
                recipe_target && target_recipe_datum == Datum{0} &&
                targets[existing].recipe_view.has_value() &&
                scanner_pg_ctx->target_locators[row_index].has_value() &&
                scanner_pg_ctx->target_locators[targets[existing].source_row_index].has_value() &&
                same_target_locator(
                    *scanner_pg_ctx->target_locators[row_index],
                    *scanner_pg_ctx->target_locators[targets[existing].source_row_index]);
            if ((!recipe_target && targets[existing].hdr == target_hdr) || same_source_recipe ||
                same_source_locator) {
                target_index = static_cast<std::uint32_t>(existing);
                break;
            }
        }
        std::uint64_t fingerprint = 0u;
        if (target_index == std::numeric_limits<std::uint32_t>::max() && !recipe_target) {
            fingerprint = target_fingerprint(target_hdr);
        }
        for (std::size_t existing = 0; target_index == std::numeric_limits<std::uint32_t>::max() &&
                                       !recipe_target && existing < targets.size();
             ++existing) {
            if (targets[existing].fingerprint == fingerprint &&
                targets[existing].target_len == static_cast<sequence_size_t>(target_hdr->bp_size) &&
                targets[existing].hdr != nullptr &&
                target_storage_bytes(targets[existing].hdr) == target_storage_bytes(target_hdr)) {
                target_index = static_cast<std::uint32_t>(existing);
                break;
            }
        }
        const sequence_size_t row_query_len =
            query_sidecars[row_to_query_index[row_index]].query_len;
        const sequence_size_t target_length =
            recipe_target ? gpu_target_recipe_views[row_index]->bp_size()
                          : static_cast<sequence_size_t>(target_hdr->bp_size);
        if (target_index == std::numeric_limits<std::uint32_t>::max()) {
            target_index = static_cast<std::uint32_t>(targets.size());
            targets.push_back(target_sidecar_type{
                .hdr = target_hdr,
                .recipe_view = recipe_target ? gpu_target_recipe_views[row_index] : std::nullopt,
                .recipe_datum = target_recipe_datum,
                .source_row_index = row_index,
                .fingerprint = fingerprint,
                .target_len = target_length,
                .max_query_len = row_query_len,
                .row_count = 1,
                .ranges = {},
                .residents = {},
            });
        } else {
            ++targets[target_index].row_count;
            targets[target_index].max_query_len =
                std::max(targets[target_index].max_query_len, row_query_len);
        }
        row_to_target_index[row_index] = target_index;
    }
    const auto requested_source = requested_target_index_source(scanner_pg_ctx);
    /** Records unattested jobs and target-index outcomes for admitted batch rows. */
    const auto record_gpu_batch_outcomes = [batch_size,
                                            broadcast_query_ordinal,
                                            per_row_query_mode,
                                            record_target_operational_outcomes,
                                            requested_source,
                                            scanner_pg_ctx,
                                            &dispatch_params,
                                            &row_to_query_index,
                                            &row_to_target_index,
                                            &cached_query_minimizer_stats_accounted,
                                            &stats] {
        std::vector<std::uint64_t> logical_target_rows;
        if (record_target_operational_outcomes) {
            logical_target_rows.reserve(batch_size);
        }
        for (std::size_t row = 0; row < batch_size; ++row) {
            if (row_to_query_index[row] == std::numeric_limits<std::uint32_t>::max() ||
                row_to_target_index[row] == std::numeric_limits<std::uint32_t>::max()) {
                continue;
            }
            if (record_target_operational_outcomes) {
                const std::uint64_t logical_target_row =
                    scanner_pg_ctx != nullptr && scanner_pg_ctx->target_rownos[row] >= 0
                        ? static_cast<std::uint64_t>(scanner_pg_ctx->target_rownos[row])
                        : static_cast<std::uint64_t>(row);
                logical_target_rows.push_back(logical_target_row);
            }
            record_unattested_job(stats,
                                  dispatch_params,
                                  requested_source,
                                  ::bioseq::pipeline_stats::target_index_source::recomputed,
                                  scanner_pg_ctx,
                                  row,
                                  per_row_query_mode,
                                  broadcast_query_ordinal,
                                  /*execution_stats=*/nullptr,
                                  /*consumed_shared_cached_query_stats=*/false,
                                  cached_query_minimizer_stats_accounted);
        }
        if (record_target_operational_outcomes) {
            const auto unique_target_count =
                ::bioseq::pipeline_stats::count_unique_logical_target_rows(
                    std::span<std::uint64_t>{logical_target_rows});
            record_target_row(stats, unique_target_count);
            record_target_index_outcome(stats,
                                        requested_source,
                                        ::bioseq::pipeline_stats::target_index_source::recomputed,
                                        unique_target_count);
        }
    };
    if (query_records.empty() || targets.empty()) {
        record_gpu_batch_outcomes();
        return;
    }

    const auto frozen_query_gate = pipe::freeze_gpu_query_gate(query_gate.vulkan_gate());
    const auto frozen_gate_identity = frozen_query_gate.canonical_identity();

    const std::size_t lpu = facade_letters_per_unit;
    for (std::size_t target_index = 0; target_index < targets.size(); ++target_index) {
        target_sidecar_type& target = targets[target_index];
        std::optional<compact_sequence_range<p_sequence>> flat_target_sequence;
        /** Resolves a GPU row view from a recipe or a materialized flat header. */
        const gpu_row_view target_view = [&target, &flat_target_sequence] {
            if (target.recipe_view.has_value()) {
                return *target.recipe_view;
            }
            if (target.hdr == nullptr) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error(
                    "Vulkan target sidecar has no backing view");
            }
            flat_target_sequence.emplace(*target.hdr);
            return bioseq::storage::make_lazy_sequence_view(*flat_target_sequence);
        }();
        const std::uint64_t gpu_target_plan_begin_ns = gpu_batch_now_ns();
        constexpr sequence_size_t gpu_batch_huge_target_bp = 100'000'000ull;
        constexpr sequence_size_t gpu_batch_huge_target_core_bp = 8'000'000ull;
        constexpr sequence_size_t gpu_batch_huge_target_overlap_max_bp = 30'000'000ull;
        if (target.row_count > 1 && target.target_len >= gpu_batch_huge_target_bp) {
            target.ranges.clear();
            const sequence_size_t total_units = (target.target_len + lpu - 1u) / lpu;
            sequence_size_t overlap_bp =
                target.max_query_len + target.max_query_len / 4u + 100'000ull;
            overlap_bp = std::min(overlap_bp, gpu_batch_huge_target_overlap_max_bp);
            const sequence_size_t overlap_units = (overlap_bp + lpu - 1u) / lpu;
            const sequence_size_t segment_units =
                std::max<sequence_size_t>(1u, gpu_batch_huge_target_core_bp / lpu);
            for (sequence_size_t lo_u = 0; lo_u < total_units; lo_u += segment_units) {
                const sequence_size_t hi_u = std::min(lo_u + segment_units, total_units);
                const sequence_size_t cs_u = lo_u > overlap_units ? lo_u - overlap_units : 0u;
                const sequence_size_t ce_u = std::min(hi_u + overlap_units, total_units);
                target.ranges.push_back(::bioseq::indexing::segment_unit_range{
                    .cs_u = cs_u,
                    .ce_u = ce_u,
                    .core_lo_bp = lo_u * lpu,
                    .core_hi_bp = std::min(hi_u * lpu, target.target_len),
                });
                if (hi_u == total_units) {
                    break;
                }
            }
        } else {
            target.ranges =
                pipe::segment_unit_ranges_for_backend(target.target_len,
                                                      target.max_query_len,
                                                      lpu,
                                                      bioseq::minimizers::minimizer_sketch_backend::
                                                          vulkan_segment);
        }
        add_stage_host_time(stats,
                            ::bioseq::pipeline_stats::pipeline_stage::target_index_build,
                            gpu_batch_now_ns() - gpu_target_plan_begin_ns);
        target.residents.reserve(target.ranges.size());
        for (std::size_t segment_index = 0; segment_index < target.ranges.size(); ++segment_index) {
            const ::bioseq::indexing::segment_unit_range& range = target.ranges[segment_index];
            const auto [segment_lo_bp, segment_hi_bp] =
                pipe::validated_segment_bp_bounds(range, lpu, target.target_len);
            const std::uint64_t gpu_index_build_begin_ns = gpu_batch_now_ns();
            const bool full_target_range =
                segment_lo_bp == 0u && segment_hi_bp == target.target_len;
            bioseq::gpu::vulkan::vulkan_sketch_decline decline{};
            auto built =
                full_target_range
                    ? bioseq::gpu::vulkan::build_gpu_index_on_device<minimizer_type>(
                          target_view, window_size, kmer_size, frozen_query_gate, &decline)
                    : bioseq::gpu::vulkan::build_gpu_segment_index_on_device<minimizer_type>(
                          target_view,
                          segment_lo_bp,
                          segment_hi_bp,
                          window_size,
                          kmer_size,
                          frozen_query_gate,
                          &decline);
            if (!built) {
                pipe::throw_vulkan_sketch_declined("Vulkan batched segment index build", decline);
            }
            if (!built->valid || built->position_count == 0u || built->occupied_slots == 0u) {
                add_stage_host_time(stats,
                                    ::bioseq::pipeline_stats::pipeline_stage::target_index_build,
                                    gpu_batch_now_ns() - gpu_index_build_begin_ns);
                target.residents.push_back(nullptr);
                continue;
            }
            bioseq::indexing::gpu_minimizer_index_cache_key key{
                .source_id = static_cast<std::uint64_t>(target_index + 1u),
                .caller_identity =
                    0xd1b54a32d192ed03ULL ^
                    (static_cast<std::uint64_t>(segment_index) + 0x9e3779b97f4a7c15ULL),
                .target_len = built->target_len,
                .table_size = built->occupied_slots,
                .position_count = built->position_count,
            };
            pipe::bind_verified_gpu_segment_gate_identity(
                key, frozen_gate_identity, built->gate_identity);
            auto adopted = bioseq::indexing::adopt_gpu_built_index(runtime, std::move(*built), key);
            if (!adopted) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error(fmt::format(
                    "Vulkan batched segment index adoption "
                    "rejected ({})",
                    bioseq::indexing::gpu_built_index_adoption_error_name(adopted.error())));
            }
            auto resident = std::move(*adopted);
            resident->target_position_offset = segment_lo_bp;
            target.residents.push_back(resident);
            add_stage_host_time(stats,
                                ::bioseq::pipeline_stats::pipeline_stage::target_index_build,
                                gpu_batch_now_ns() - gpu_index_build_begin_ns);
        }
    }

    std::vector<bioseq::gpu::vulkan::gpu_seed_coordinate_job> gpu_jobs;
    std::vector<gpu_batch_problem_sidecar> problem_sidecars;
    const std::uint64_t gpu_problem_build_begin_ns = gpu_batch_now_ns();
    gpu_jobs.reserve(batch_size);
    problem_sidecars.reserve(batch_size);
    for (std::size_t target_index = 0; target_index < targets.size(); ++target_index) {
        target_sidecar_type& target = targets[target_index];
        for (std::size_t segment_index = 0; segment_index < target.residents.size();
             ++segment_index) {
            const auto& resident = target.residents[segment_index];
            if (!resident) {
                continue;
            }
            const ::bioseq::indexing::segment_unit_range& range = target.ranges[segment_index];
            for (std::size_t row = 0; row < batch_size; ++row) {
                const std::uint32_t query_index = row_to_query_index[row];
                if (query_index == std::numeric_limits<std::uint32_t>::max() ||
                    row_to_target_index[row] != target_index) {
                    continue;
                }
                gpu_jobs.push_back(bioseq::gpu::vulkan::gpu_seed_coordinate_job{
                    .slots = &resident->slots,
                    .positions = &resident->positions,
                    .target_len = resident->target_len,
                    .slot_mask = resident->slot_mask,
                    .position_count = resident->position_count,
                    .bucket_count = resident->bucket_count,
                    .target_position_offset = resident->target_position_offset,
                    .query_index = query_index,
                });
                problem_sidecars.push_back(gpu_batch_problem_sidecar{
                    .row_index = row,
                    .target_index = target_index,
                    .segment_index = segment_index,
                    .core_lo_bp = range.core_lo_bp,
                    .core_hi_bp = range.core_hi_bp,
                });
            }
        }
    }
    add_stage_host_time(stats,
                        ::bioseq::pipeline_stats::pipeline_stage::seed_lookup,
                        gpu_batch_now_ns() - gpu_problem_build_begin_ns);

    if (gpu_jobs.empty()) {
        record_gpu_batch_outcomes();
        return;
    }

    auto chaining_parameters = dispatch_params.make_chaining_parameters();
    std::int32_t max_dist_x = chaining_parameters.max_gap;
    std::int32_t max_dist_y = chaining_parameters.max_gap;
    if (max_dist_x < chaining_parameters.bandwidth) {
        max_dist_x = chaining_parameters.bandwidth;
    }
    if (max_dist_y < chaining_parameters.bandwidth) {
        max_dist_y = chaining_parameters.bandwidth;
    }
    const auto chain_key = bioseq::chaining::choose_vulkan_chaining_pipeline(runtime.features());
    if (query_minimizer_device_span.count() >
        static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)())) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error{
            "batched device query minimizer count exceeds ABI"};
    }
    bioseq::gpu::vulkan::gpu_seed_query_inputs query_inputs{
        .host_minimizer_bytes = {},
        .device_minimizer_words = query_minimizer_device_span.words(),
        .minimizer_count = static_cast<std::uint32_t>(query_minimizer_device_span.count()),
        .minimizer_word_kind = static_cast<std::uint32_t>(
            bioseq::gpu::vulkan::minimizer_wire_traits<minimizer_type>::k_word_kind),
        .minimizer_words32_per_record =
            bioseq::gpu::vulkan::minimizer_wire_traits<minimizer_type>::k_words32_per_minimizer,
        .hash_shift = bioseq::gpu::vulkan::minimizer_wire_traits<minimizer_type>::k_hash_shift,
        .pos_bits = bioseq::gpu::vulkan::minimizer_wire_traits<minimizer_type>::k_position_bits,
        .host_query_records =
            std::span<const bioseq::gpu::vulkan::gpu_seed_query_record_wire>{query_records.data(),
                                                                             query_records.size()},
        .device_query_records = nullptr,
    };
    const auto coord_run = runtime.run_seed_to_coordinate_batch_roundtrip(
        query_inputs,
        std::span<const bioseq::gpu::vulkan::gpu_seed_coordinate_job>{gpu_jobs.data(),
                                                                      gpu_jobs.size()},
        static_cast<std::uint32_t>(dispatch_params.kmer_size),
        dispatch_params.max_occ,
        bioseq::gpu::vulkan::gpu_seed_coordinate_algorithm::vulkan_tiled_dp,
        chain_key,
        max_dist_x,
        max_dist_y,
        chaining_parameters.bandwidth,
        std::min<std::int32_t>(chaining_parameters.max_chain_iter, 3000),
        chaining_parameters.gap_penalty,
        chaining_parameters.skip_penalty,
        chaining_parameters.max_chain_skip,
        chaining_parameters.min_chain_score,
        chaining_parameters.min_seed_count,
        16u);
    add_operational_counter(stats,
                            stats.operational.gpu_seed_dispatches,
                            coord_run.stats.seed_count_dispatches +
                                coord_run.stats.seed_emit_dispatches);
    if (coord_run.stats.coordinate_vote_dispatches != 0u) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "selected Vulkan tiled-DP route executed the "
            "coordinate-vote algorithm");
    }
    add_operational_counter(
        stats, stats.operational.gpu_chain_dispatches, coord_run.stats.chain_dispatches);
    add_operational_counter(stats,
                            stats.operational.gpu_chain_backtrack_dispatches,
                            coord_run.stats.chain_backtrack_dispatches);
    add_operational_counter(stats,
                            stats.operational.gpu_coordinate_vote_dispatches,
                            coord_run.stats.coordinate_vote_dispatches);

    /** Pairs a dispatch query id with a coordinate match awaiting GPU CIGAR merge. */
    struct gpu_cigar_pending_match {
        int32_t query_id = 0;
        match_result match{};
    };
    std::vector<gpu_cigar_pending_match> pending_cigar_matches;
    std::vector<bioseq::gpu::vulkan::gpu_alignment_cigar_job_wire> cigar_jobs;
    std::vector<std::uint32_t> cigar_query_symbols;
    std::vector<std::uint32_t> cigar_target_symbols;
    std::vector<std::uint8_t> cigar_symbol_scratch;
    std::uint32_t packed_cigar_capacity = 0;
    std::uint32_t traceback_word_capacity = 0;
    pending_cigar_matches.reserve(coord_run.results.size());
    cigar_jobs.reserve(coord_run.results.size());
    /** Returns monotonic time in nanoseconds for CIGAR job preparation timing. */
    const auto gpu_cigar_now_ns = []() -> std::uint64_t {
        return static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns());
    };
    const std::uint64_t cigar_prepare_begin_ns = gpu_cigar_now_ns();

    for (std::size_t problem_index = 0; problem_index < coord_run.results.size(); ++problem_index) {
        const auto& coord = coord_run.results[problem_index];
        if (coord.is_valid == 0u) {
            continue;
        }
        const gpu_batch_problem_sidecar& owner = problem_sidecars[problem_index];
        if (coord.target_start < owner.core_lo_bp || coord.target_start >= owner.core_hi_bp) {
            continue;
        }
        const std::uint32_t query_index = row_to_query_index[owner.row_index];
        const target_sidecar_type& target = targets[owner.target_index];
        match_result coordinate_match = pipe::gpu_coordinate_to_match_result(
            coord, query_sidecars[query_index].query_len, target.target_len);
        if (!dispatch_params.do_cigar) {
            results[query_ids[owner.row_index]].push_back(std::move(coordinate_match));
            continue;
        }

        const p_sequence_header* row_query_hdr =
            per_row_query_mode ? (*per_row_query_hdrs)[owner.row_index] : query_hdr;
        const bool recipe_query = gpu_query_recipe_views[owner.row_index].has_value();
        const bool recipe_target = target.recipe_view.has_value();
        if ((row_query_hdr == nullptr && !recipe_query) ||
            (target.hdr == nullptr && !recipe_target)) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "GPU CIGAR extension received null query/target sequence");
        }
        std::optional<compact_sequence_range<p_sequence>> flat_query_sequence;
        std::optional<compact_sequence_range<p_sequence>> flat_target_sequence;
        /** Resolves the query sequence view used to pack GPU alignment symbols. */
        const gpu_row_view row_query_seq = [recipe_query,
                                            &gpu_query_recipe_views,
                                            owner_row_index = owner.row_index,
                                            &flat_query_sequence,
                                            row_query_hdr] {
            if (recipe_query) {
                return *gpu_query_recipe_views[owner_row_index];
            }
            flat_query_sequence.emplace(*row_query_hdr);
            return bioseq::storage::make_lazy_sequence_view(*flat_query_sequence);
        }();
        /** Resolves the target sequence view used to pack GPU alignment symbols. */
        const gpu_row_view target_seq = [recipe_target, &target, &flat_target_sequence] {
            if (recipe_target) {
                return *target.recipe_view;
            }
            flat_target_sequence.emplace(*target.hdr);
            return bioseq::storage::make_lazy_sequence_view(*flat_target_sequence);
        }();
        const sequence_size_t query_len_bp = row_query_seq.bp_size();
        const sequence_size_t target_len_bp = target_seq.bp_size();
        if (coordinate_match.query_start > query_len_bp ||
            coordinate_match.target_start > target_len_bp) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "GPU CIGAR extension coordinate start exceeds sequence length");
        }
        coordinate_match.query_end = std::min(coordinate_match.query_end, query_len_bp);
        coordinate_match.target_end = std::min(coordinate_match.target_end, target_len_bp);
        const sequence_size_t query_span =
            coordinate_match.query_end - coordinate_match.query_start;
        const sequence_size_t target_span =
            coordinate_match.target_end - coordinate_match.target_start;
        if (query_span == 0 || target_span == 0) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "GPU CIGAR extension received empty coordinate span");
        }
        /** Converts a value to uint32 or throws when it exceeds the wire limit. */
        const auto checked_u32 = [](std::uint64_t value,
                                    std::string_view field_name) -> std::uint32_t {
            if (value > std::numeric_limits<std::uint32_t>::max()) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error(
                    fmt::format("{} exceeds uint32", field_name));
            }
            return static_cast<std::uint32_t>(value);
        };
        if (query_span > bioseq::gpu::vulkan::k_gpu_alignment_simple_cigar_max_len ||
            target_span > bioseq::gpu::vulkan::k_gpu_alignment_simple_cigar_max_len) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                fmt::format("GPU CIGAR extension simple CIGAR supports spans up to {}, "
                            "got query_span={} target_span={}",
                            bioseq::gpu::vulkan::k_gpu_alignment_simple_cigar_max_len,
                            query_span,
                            target_span));
        }

        const std::uint32_t query_symbol_offset =
            checked_u32(cigar_query_symbols.size(), "GPU CIGAR query symbol offset");
        std::span<const std::uint32_t> query_symbols =
            append_gpu_alignment_symbols(cigar_query_symbols,
                                         cigar_symbol_scratch,
                                         row_query_seq,
                                         coordinate_match.query_start,
                                         coordinate_match.query_end,
                                         false);
        const std::uint32_t target_symbol_offset =
            checked_u32(cigar_target_symbols.size(), "GPU CIGAR target symbol offset");
        std::span<const std::uint32_t> target_symbols =
            append_gpu_alignment_symbols(cigar_target_symbols,
                                         cigar_symbol_scratch,
                                         target_seq,
                                         coordinate_match.target_start,
                                         coordinate_match.target_end,
                                         coordinate_match.is_reverse);
        add_operational_counter(stats, stats.operational.host_query_symbol_packing_calls);
        add_operational_counter(stats, stats.operational.host_target_symbol_packing_calls);

        const std::uint32_t query_len = checked_u32(query_span, "GPU CIGAR query span");
        const std::uint32_t target_len = checked_u32(target_span, "GPU CIGAR target span");
        const std::uint32_t cigar_capacity = std::max<std::uint32_t>(
            1u, checked_u32(query_span + target_span, "GPU CIGAR packed capacity"));
        const bool bounded_dp_candidate =
            query_len <= bioseq::gpu::vulkan::k_gpu_alignment_extension_max_len &&
            target_len <= bioseq::gpu::vulkan::k_gpu_alignment_extension_max_len;
        const bool exact_long_cigar_candidate =
            query_len == target_len &&
            std::equal(query_symbols.begin(), query_symbols.end(), target_symbols.begin());
        const bool unit_gap_walk_cigar_candidate =
            gpu_alignment_unit_gap_walk_cigar_candidate(query_symbols, target_symbols);
        const std::uint64_t traceback_cells =
            bounded_dp_candidate ? (static_cast<std::uint64_t>(query_len) + 1u) *
                                       (static_cast<std::uint64_t>(target_len) + 1u)
            : (exact_long_cigar_candidate || unit_gap_walk_cigar_candidate)
                ? 0u
                : bioseq::gpu::vulkan::gpu_alignment_cigar_gridded_traceback_words(
                      query_len,
                      target_len,
                      bioseq::gpu::vulkan::k_gpu_alignment_cigar_traceback_tile_len,
                      bioseq::gpu::vulkan::k_gpu_alignment_cigar_traceback_tile_len);
        add_operational_counter(stats, stats.operational.host_alignment_job_classification_calls);
        cigar_jobs.push_back(bioseq::gpu::vulkan::gpu_alignment_cigar_job_wire{
            .query_symbol_offset = query_symbol_offset,
            .target_symbol_offset = target_symbol_offset,
            .query_len = query_len,
            .target_len = target_len,
            .query_global_start =
                checked_u32(coordinate_match.query_start, "GPU CIGAR query global start"),
            .target_global_start =
                checked_u32(coordinate_match.target_start, "GPU CIGAR target global start"),
            .band_width = std::min<std::uint32_t>(
                bioseq::gpu::vulkan::k_gpu_alignment_extension_max_band_width,
                std::max(query_len, target_len)),
            .flags = 0,
            .cigar_offset = packed_cigar_capacity,
            .cigar_capacity = cigar_capacity,
            .traceback_tile_offset = traceback_word_capacity,
            .traceback_tile_count = checked_u32(traceback_cells, "GPU CIGAR traceback cell count"),
        });
        packed_cigar_capacity =
            checked_u32(static_cast<std::uint64_t>(packed_cigar_capacity) + cigar_capacity,
                        "GPU CIGAR packed capacity total");
        traceback_word_capacity =
            checked_u32(static_cast<std::uint64_t>(traceback_word_capacity) + traceback_cells,
                        "GPU CIGAR traceback capacity total");
        pending_cigar_matches.push_back(gpu_cigar_pending_match{
            .query_id = query_ids[owner.row_index],
            .match = std::move(coordinate_match),
        });
    }
    add_stage_host_time(stats,
                        ::bioseq::pipeline_stats::pipeline_stage::cigar_job_generation,
                        gpu_cigar_now_ns() - cigar_prepare_begin_ns);

    if (dispatch_params.do_cigar && !cigar_jobs.empty()) {
        const bioseq::gpu::vulkan::gpu_alignment_cigar_params cigar_params{
            .alphabet_size = static_cast<std::uint32_t>(
                compact_sequence_range<p_sequence>::traits::alphabet_size_n),
            .max_band_width = bioseq::gpu::vulkan::k_gpu_alignment_extension_max_band_width,
            .tile_query = bioseq::gpu::vulkan::k_gpu_alignment_cigar_traceback_tile_len,
            .tile_target = bioseq::gpu::vulkan::k_gpu_alignment_cigar_traceback_tile_len,
            .match_score = dispatch_params.match_score,
            .mismatch_penalty = dispatch_params.mismatch_pen,
            .gap_open_short = dispatch_params.gap_open1,
            .gap_extend_short = dispatch_params.gap_ext1,
            .gap_open_long = dispatch_params.gap_open2,
            .gap_extend_long = dispatch_params.gap_ext2,
            .z_drop = dispatch_params.zdrop,
            .end_bonus = std::max<std::int32_t>(0, dispatch_params.end_bonus),
            .flags = 0,
            .reserved = 0,
        };
        const auto cigar_run = runtime.run_alignment_simple_cigar_roundtrip(
            std::span<const std::uint32_t>{cigar_query_symbols.data(), cigar_query_symbols.size()},
            std::span<const std::uint32_t>{cigar_target_symbols.data(),
                                           cigar_target_symbols.size()},
            std::span<const bioseq::gpu::vulkan::gpu_alignment_cigar_job_wire>{cigar_jobs.data(),
                                                                               cigar_jobs.size()},
            cigar_params,
            packed_cigar_capacity);
        add_operational_counter(stats, stats.operational.gpu_extend_dispatches);
        if (cigar_run.results.size() != pending_cigar_matches.size()) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "GPU CIGAR extension result count mismatch");
        }
        for (std::size_t match_index = 0; match_index < pending_cigar_matches.size();
             ++match_index) {
            const auto& cigar_result = cigar_run.results[match_index];
            const auto& cigar_job = cigar_jobs[match_index];
            if (cigar_result.cigar_length == 0u) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error(
                    "GPU CIGAR extension returned empty CIGAR");
            }
            const std::uint64_t cigar_end = static_cast<std::uint64_t>(cigar_result.cigar_offset) +
                                            static_cast<std::uint64_t>(cigar_result.cigar_length);
            if (cigar_end > cigar_run.packed_cigar.size()) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error(
                    "GPU CIGAR extension returned invalid CIGAR slice");
            }

            gpu_cigar_pending_match& pending = pending_cigar_matches[match_index];
            match_result& match = pending.match;
            match.dp_score = cigar_result.score;
            match.dp_max_score = cigar_result.max_score;
            match.query_start = cigar_result.query_start;
            match.query_end = cigar_result.query_end;
            match.target_start = cigar_result.target_start;
            match.target_end = cigar_result.target_end;
            match.has_cigar = true;
            match.cigar.clear();

            const std::span<const std::uint32_t> packed{cigar_run.packed_cigar.data() +
                                                            cigar_result.cigar_offset,
                                                        cigar_result.cigar_length};
            sequence_size_t block_length = 0;
            sequence_size_t num_matches = 0;
            std::uint32_t query_cursor = 0;
            std::uint32_t target_cursor = 0;
            for (std::uint32_t entry : packed) {
                const std::uint32_t opcode = entry & 0xfU;
                const std::uint32_t operation_length = entry >> 4;
                if (opcode == cigar_match_opcode || opcode == cigar_equal_opcode ||
                    opcode == cigar_mismatch_opcode) {
                    block_length += operation_length;
                    const std::uint32_t query_base = cigar_job.query_symbol_offset + query_cursor;
                    const std::uint32_t target_base =
                        cigar_job.target_symbol_offset + target_cursor;
                    for (std::uint32_t symbol_offset = 0; symbol_offset < operation_length;
                         ++symbol_offset) {
                        if (cigar_query_symbols[query_base + symbol_offset] ==
                            cigar_target_symbols[target_base + symbol_offset]) {
                            ++num_matches;
                        }
                    }
                    query_cursor += operation_length;
                    target_cursor += operation_length;
                } else if (opcode == cigar_insertion_opcode) {
                    block_length += operation_length;
                    query_cursor += operation_length;
                } else if (opcode == cigar_deletion_opcode) {
                    block_length += operation_length;
                    target_cursor += operation_length;
                }
            }
            match.block_length = block_length;
            match.num_matches = num_matches;
            append_packed_cigar_slice(match.cigar, packed);
            results[pending.query_id].push_back(std::move(match));
        }
        add_operational_counter(stats,
                                stats.operational.host_cigar_construction_calls,
                                static_cast<std::uint64_t>(pending_cigar_matches.size()));
    }
    record_gpu_batch_outcomes();
    return;
}

} // namespace bioseq::simd::typed_dispatch::typed_support_detail
