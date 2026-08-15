/**
 * @file typed_support_parts/vulkan_persisted_route.cppm
 * @brief Owns transactional persisted grouped-SoA Vulkan publication.
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

export module bioseq.postgres.scheduler.dispatch_processor.typed_support:vulkan_persisted_route;

import bioseq.postgres.scheduler.dispatch_processor;
import :policy_stats;
import :pg_alignment_errors;
import :transient_storage;
import :resident_targets;
import :row_adapters;
import :scanner_row;
import :vulkan_query_route;
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

/**
 * Publishes one authenticated persisted grouped-SoA operation atomically.
 *
 * All result, pipeline-stat, source-identity, lifecycle, and SQL-ledger
 * mutations remain staged until the final no-throw publication boundary.
 */
template <class p_sequence, class p_runtime, class p_prepared_target>
void process_vulkan_persisted_route(const size_t batch_size,
                                    const std::array<int32_t, dispatch_batch_capacity>& query_ids,
                                    const minimap2_parameters& dispatch_params,
                                    const std::uint64_t broadcast_query_ordinal,
                                    ::bioseq::pipeline_stats::pipeline_execution_stats& stats,
                                    batch_processor::scanner_pg_context* scanner_pg_ctx,
                                    const bool per_row_query_mode,
                                    const bool record_target_operational_outcomes,
                                    const bool gpu_persisted_prepared,
                                    p_runtime& runtime,
                                    p_prepared_target* const prepared_gpu_target,
                                    vulkan_query_batch_state<p_sequence>& state,
                                    std::map<int32_t, std::vector<match_result>>& results) {
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
    auto cp = dispatch_params.make_chaining_parameters();
    const std::int32_t max_dist_x = std::max(cp.max_gap, cp.bandwidth);
    const std::int32_t max_dist_y = std::max(cp.max_gap, cp.bandwidth);
    const auto chain_key = bioseq::chaining::choose_vulkan_chaining_pipeline(runtime.features());
    constexpr auto coordinate_algorithm =
        bioseq::gpu::vulkan::gpu_seed_coordinate_algorithm::vulkan_tiled_dp;
    const std::int32_t max_iter = std::min<std::int32_t>(cp.max_chain_iter, 3000);
    constexpr std::uint32_t probe_limit = 16u;
    const auto coordinate_configuration =
        bioseq::gpu::vulkan::make_persisted_grouped_soa_coordinate_config_identity(
            static_cast<std::uint32_t>(dispatch_params.kmer_size),
            dispatch_params.max_occ,
            coordinate_algorithm,
            chain_key,
            max_dist_x,
            max_dist_y,
            cp.bandwidth,
            max_iter,
            cp.gap_penalty,
            cp.skip_penalty,
            cp.max_chain_skip,
            cp.min_chain_score,
            cp.min_seed_count,
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
        cp.bandwidth,
        max_iter,
        cp.gap_penalty,
        cp.skip_penalty,
        cp.max_chain_skip,
        cp.min_chain_score,
        cp.min_seed_count,
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
    decltype(results) staged_results;
    std::array<bool, dispatch_batch_capacity> staged_rows{};

    for (std::size_t problem_index = 0; problem_index < direct_jobs.size(); ++problem_index) {
        const std::size_t row = direct_job_rows[problem_index];
        if (row >= batch_size || row >= dispatch_batch_capacity || staged_rows[row] ||
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

} // namespace bioseq::simd::typed_dispatch::typed_support_detail
