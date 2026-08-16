/**
 * @file typed_support_parts/persisted_native_batch.cppm
 * @brief Owns retained native persisted-index query-batch execution.
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

export module bioseq.postgres.scheduler.dispatch_processor.typed_support:persisted_native_batch;

import bioseq.postgres.scheduler.dispatch_processor;
import :policy_stats;
import :pg_alignment_errors;
import :transient_storage;
import :resident_targets;
import :row_adapters;
import :scanner_row;
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
 * Executes the authenticated retained native query batch when its complete
 * preflight succeeds, finalizes deferred compact-direct validation, and runs
 * the compile-time generic-row continuation only when the native batch did
 * not produce the complete result.
 */
template <class p_sequence, class p_pool, class p_generic_rows>
void process_persisted_native_batch(
    const size_t batch_size,
    const uint8_t alphabet_code,
    const uint8_t storage_unit_byte_width,
    const std::array<const bioseq::pg_types::seq*, dispatch_batch_capacity>* per_row_query_hdrs,
    const std::array<const bioseq::pg_types::seq*, dispatch_batch_capacity>& target_hdrs,
    const std::array<int32_t, dispatch_batch_capacity>& query_ids,
    const minimap2_parameters& dispatch_params,
    const std::uint64_t broadcast_query_ordinal,
    ::bioseq::pipeline_stats::pipeline_execution_stats& stats,
    batch_processor::scanner_pg_context* scanner_pg_ctx,
    const std::size_t facade_letters_per_unit,
    const bool per_row_query_mode,
    const bool record_target_operational_outcomes,
    const bool use_scanner_pg,
    const bool persisted_required,
    bool& cached_query_minimizer_stats_accounted,
    p_pool& pool,
    p_generic_rows& generic_rows,
    std::map<int32_t, std::vector<match_result>>& results) {
    const bool may_parallel_persisted_native =
        persisted_required && use_scanner_pg && scanner_pg_ctx != nullptr &&
        scanner_pg_ctx->explicit_query_ordinals && per_row_query_mode &&
        per_row_query_hdrs != nullptr && !dispatch_params.enable_profiling &&
        !selected_vulkan_accelerator(dispatch_params) &&
        bioseq::parallelism::has_parallel_tasks(pool.get_mode()) &&
        pool.maximum_concurrency() > 1 &&
        batch_size >= bioseq::parallelism::task_parallel_minimums::k_query_count &&
        scanner_pg_ctx->cpu_prepared_target != nullptr;
    if (may_parallel_persisted_native) {
        auto* const prepared_target = scanner_pg_ctx->cpu_prepared_target;
        bool batch_identity_valid =
            prepared_target->unions_ready && prepared_target->target_rowno >= 0 &&
            prepared_target->query_count >= static_cast<std::uint64_t>(batch_size) &&
            (prepared_target->query_set_validation_ready() ||
             prepared_target->query_count == static_cast<std::uint64_t>(batch_size)) &&
            prepared_target->target_bp_size >
                static_cast<std::uint64_t>(postgres_segmentation_threshold_bp);
        const std::int64_t shared_target_rowno = prepared_target->target_rowno;
        const std::uint64_t shared_target_bp_size = prepared_target->target_bp_size;
        const bioseq::storage::sequence_locator* shared_locator = nullptr;
        batch_processor::cpu_prepared_index_group* native_group = nullptr;
        std::vector<const batch_processor::cpu_prepared_query_sketch*> retained_query_sketches(
            batch_size, nullptr);
        std::vector<minimap2_parameters> native_query_params(batch_size);

        for (std::size_t row = 0; batch_identity_valid && row < batch_size; ++row) {
            const bioseq::pg_types::seq* const row_query_hdr = (*per_row_query_hdrs)[row];
            const bioseq::pg_types::seq* const target_hdr = target_hdrs[row];
            const auto& locator = scanner_pg_ctx->target_locators[row];
            if (row_query_hdr == nullptr || row_query_hdr->alphabet_code != alphabet_code ||
                row_query_hdr->storage_unit_byte_width != storage_unit_byte_width ||
                !locator.has_value() || scanner_pg_ctx->target_rownos[row] != shared_target_rowno ||
                (row != 0u &&
                 scanner_pg_ctx->query_ordinals[row - 1u] >= scanner_pg_ctx->query_ordinals[row]) ||
                locator->logical_length != shared_target_bp_size ||
                (target_hdr != nullptr &&
                 (target_hdr->alphabet_code != alphabet_code ||
                  target_hdr->storage_unit_byte_width != storage_unit_byte_width ||
                  target_hdr->bp_size != shared_target_bp_size))) {
                batch_identity_valid = false;
                break;
            }
            if (shared_locator == nullptr) {
                shared_locator = &*locator;
            } else if (!same_dispatch_target_locator(*shared_locator, *locator)) {
                batch_identity_valid = false;
                break;
            }

            const std::size_t min_bp =
                std::min<std::size_t>(static_cast<std::size_t>(row_query_hdr->bp_size),
                                      static_cast<std::size_t>(shared_target_bp_size));
            minimap2_parameters params = dispatch_params;
            params.kmer_size = 15;
            params.window_size = 10;
            if (min_bp < 50u) {
                params.min_chain_score = 10;
                params.min_seed_count = 1;
            } else if (min_bp < 200u) {
                params.min_chain_score = 20;
                params.min_seed_count = 2;
            }
            const auto backend = pipe::target_index_sketch_backend_from_params(params);
            auto* const group = prepared_target->find_group(
                params.kmer_size, params.window_size, params.max_occ, backend);
            if (group == nullptr || (native_group != nullptr && native_group != group)) {
                batch_identity_valid = false;
                break;
            }
            native_group = group;
            const auto* const retained =
                group->find_retained_query_sketch(scanner_pg_ctx->query_ordinals[row]);
            if (retained == nullptr ||
                retained->query_bp_size != static_cast<std::uint64_t>(row_query_hdr->bp_size) ||
                !pipe::minimizer_range_stats_matches_backend(
                    backend,
                    retained->execution_stats,
                    /*require_nonempty_vector_work=*/false,
                    pipe::exact_simd_range_route::require_continuation)) {
                batch_identity_valid = false;
                break;
            }
            retained_query_sketches[row] = retained;
            native_query_params[row] = params;
        }

        batch_identity_valid =
            batch_identity_valid && shared_locator != nullptr && native_group != nullptr &&
            (hash_major_parallel_reader_identity_valid(*native_group, shared_target_bp_size) ||
             fragment_parallel_reader_identity_valid(*native_group, shared_target_bp_size));
        if (batch_identity_valid) {
            const std::size_t letters_per_unit = facade_letters_per_unit;
            const std::uint64_t target_remainder = shared_target_bp_size % letters_per_unit;
            const std::size_t padding = static_cast<std::size_t>(
                target_remainder == 0u ? 0u : letters_per_unit - target_remainder);
            bool native_batch_completed = false;
            with_source_provider_pg_target<p_sequence>(
                *scanner_pg_ctx,
                *shared_locator,
                alphabet_code,
                storage_unit_byte_width,
                static_cast<std::uint16_t>(padding),
                nullptr,
                [shared_target_rowno,
                 shared_target_bp_size,
                 native_group,
                 batch_size,
                 &pool,
                 &native_query_params,
                 &retained_query_sketches,
                 per_row_query_hdrs,
                 scanner_pg_ctx,
                 prepared_target,
                 record_target_operational_outcomes,
                 &stats,
                 broadcast_query_ordinal,
                 &cached_query_minimizer_stats_accounted,
                 &query_ids,
                 &results,
                 &native_batch_completed,
                 storage_unit_byte_width](auto fragmented_target_view, pg_sequence_read_session*) {
                    auto resident = try_materialize_target_for_segment_workers(
                        fragmented_target_view,
                        storage_unit_byte_width,
                        static_cast<std::uint64_t>(shared_target_rowno));
                    if (resident.has_value()) {
                        const auto resident_target_view = resident->owner.view();
                        const auto* const hash_major_reader =
                            native_group->persisted_hash_major_reader
                                ? &*native_group->persisted_hash_major_reader
                                : nullptr;
                        const auto* const fragment_reader =
                            native_group->persisted_fragment_reader
                                ? &*native_group->persisted_fragment_reader
                                : nullptr;
                        const auto fragment_lookup_route =
                            native_group->persisted_fragment_lookup_route;
                        // Compact-direct preparation is itself the exact memory
                        // authority. Attempt the complete canonical batch once,
                        // without a duplicate occurrence-estimation pass. A typed
                        // preparation-time admission failure may retry from wave 0
                        // at the configured worker bound below.
                        const std::size_t worker_bounded_fragment_wave_size = std::max(
                            std::size_t{1u},
                            std::min(batch_size,
                                     static_cast<std::size_t>(pool.maximum_concurrency())));
                        const std::size_t fragment_wave_size =
                            fragment_reader != nullptr &&
                                    fragment_lookup_route ==
                                        bioseq::indexing::minimizer_fragment_lookup_route::
                                            direct_compact_hash_lookup
                                ? batch_size
                                : worker_bounded_fragment_wave_size;
                        std::vector<match_result_collection> native_collections(batch_size);
                        (void)run_aligner_pg_checked(
                            "persisted fragment-native parallel seed wave",
                            [&native_collections,
                             &native_query_params,
                             resident_target_view,
                             &retained_query_sketches,
                             per_row_query_hdrs,
                             hash_major_reader,
                             fragment_reader,
                             fragment_lookup_route,
                             prepared_target,
                             native_group,
                             batch_size,
                             worker_bounded_fragment_wave_size,
                             fragment_wave_size]() -> match_result_collection {
                                /**
                                 * Runs minimap hash-major alignment for one batch row into
                                 * native_collections at the given row index.
                                 */
                                const auto align_hash_major_query =
                                    [&native_collections,
                                     &native_query_params,
                                     resident_target_view,
                                     &retained_query_sketches,
                                     per_row_query_hdrs](
                                        const std::size_t row,
                                        const bioseq::indexing::
                                            hash_major_minimizer_selective_reader& target_index) {
                                        compact_sequence_range<p_sequence> query_seq{
                                            *(*per_row_query_hdrs)[row]};
                                        const auto lazy_query_view =
                                            bioseq::storage::make_lazy_sequence_view(query_seq);
                                        native_collections[row] = minimap_port_hash_major_owned(
                                            lazy_query_view,
                                            resident_target_view,
                                            retained_query_sketches[row]->minimizers,
                                            target_index,
                                            native_query_params[row],
                                            /*gpu_index_cache=*/nullptr,
                                            bioseq::indexing::gpu_index_cache_identity{});
                                    };
                                /**
                                 * Runs minimap fragment-query alignment for one batch row using
                                 * the prepared fragment anchor handle.
                                 */
                                const auto align_fragment_query =
                                    [&native_collections,
                                     &native_query_params,
                                     resident_target_view,
                                     &retained_query_sketches,
                                     per_row_query_hdrs](
                                        const std::size_t row,
                                        const bioseq::indexing::prepared_fragment_anchor_batch::
                                            query_handle& target_index) {
                                        compact_sequence_range<p_sequence> query_seq{
                                            *(*per_row_query_hdrs)[row]};
                                        const auto lazy_query_view =
                                            bioseq::storage::make_lazy_sequence_view(query_seq);
                                        native_collections[row] = minimap_port_fragment_query_owned(
                                            lazy_query_view,
                                            resident_target_view,
                                            retained_query_sketches[row]->minimizers,
                                            target_index,
                                            native_query_params[row],
                                            /*gpu_index_cache=*/nullptr,
                                            bioseq::indexing::gpu_index_cache_identity{});
                                    };
                                if (hash_major_reader != nullptr) {
                                    bioseq::parallelism::parallel_for_pipeline(
                                        0,
                                        batch_size,
                                        1,
                                        bioseq::parallelism::task_parallel_minimums::k_query_count,
                                        bioseq::parallelism::task_parallel_region::query_batch,
                                        [&align_hash_major_query,
                                         hash_major_reader](const std::size_t range_begin,
                                                            const std::size_t range_end) {
                                            for (std::size_t row = range_begin; row < range_end;
                                                 ++row) {
                                                align_hash_major_query(row, *hash_major_reader);
                                            }
                                        });
                                    return {};
                                }
                                if (fragment_reader == nullptr) {
                                    throw bioseq::indexing::persisted_minimizer_lookup_error(
                                        "fragment-native grouped-SoA reader disappeared");
                                }

                                auto& memory_admission =
                                    bioseq::parallelism::memory_admission_controller::instance();
                                const std::uint64_t first_wave_prepare_started_ns =
                                    dispatch_host_now_ns();
                                auto scratch_result =
                                    bioseq::indexing::query_wave_scratch::create(memory_admission);
                                if (!scratch_result) {
                                    throw bioseq::indexing::persisted_minimizer_lookup_error(
                                        "fragment-native query scratch was "
                                        "not admitted; wire_error=" +
                                        std::to_string(
                                            static_cast<unsigned>(scratch_result.error())));
                                }
                                // Reuse one admitted scratch owner across the
                                // serial waves. Each prepared batch owns its
                                // final anchors independently and is destroyed
                                // after its synchronous query wave completes.
                                auto query_wave_scratch = std::move(*scratch_result);

                                const std::uint64_t descriptor_bytes =
                                    bioseq::common::saturating_mul(
                                        fragment_wave_size,
                                        sizeof(bioseq::indexing::fragment_seed_query));
                                const std::uint64_t descriptor_headroom =
                                    bioseq::common::saturating_mul(
                                        descriptor_bytes,
                                        bioseq::parallelism::
                                            k_vector_preallocation_headroom_factor);
                                if (descriptor_bytes ==
                                        (std::numeric_limits<std::uint64_t>::max)() ||
                                    descriptor_headroom ==
                                        (std::numeric_limits<std::uint64_t>::max)()) {
                                    throw bioseq::indexing::persisted_minimizer_lookup_error(
                                        "fragment-native query descriptor "
                                        "capacity overflow");
                                }
                                auto descriptor_permit_result =
                                    memory_admission.try_reserve(descriptor_headroom);
                                if (!descriptor_permit_result) {
                                    throw bioseq::indexing::persisted_minimizer_lookup_error(
                                        "fragment-native query descriptors "
                                        "were not admitted");
                                }
                                // Declared before the vector so reverse
                                // destruction releases storage before permit.
                                auto descriptor_permit = std::move(*descriptor_permit_result);
                                std::vector<bioseq::indexing::fragment_seed_query> wave_queries;
                                try {
                                    wave_queries.reserve(fragment_wave_size);
                                } catch (const std::bad_alloc&) {
                                    throw bioseq::indexing::persisted_minimizer_lookup_error(
                                        "fragment-native query descriptor "
                                        "allocation failed");
                                }
                                const std::uint64_t actual_descriptor_bytes =
                                    bioseq::common::saturating_mul(
                                        wave_queries.capacity(),
                                        sizeof(bioseq::indexing::fragment_seed_query));
                                if (actual_descriptor_bytes ==
                                        (std::numeric_limits<std::uint64_t>::max)() ||
                                    (actual_descriptor_bytes > descriptor_permit.bytes() &&
                                     !descriptor_permit.grow_to(actual_descriptor_bytes))) {
                                    throw bioseq::indexing::persisted_minimizer_lookup_error(
                                        "fragment-native query descriptor "
                                        "reconciliation failed");
                                }
                                descriptor_permit.shrink_to(actual_descriptor_bytes);

                                std::size_t active_fragment_wave_size = fragment_wave_size;
                                bool full_wave_admission_retry_available =
                                    fragment_lookup_route ==
                                        bioseq::indexing::minimizer_fragment_lookup_route::
                                            direct_compact_hash_lookup &&
                                    fragment_wave_size == batch_size &&
                                    worker_bounded_fragment_wave_size < batch_size;
                                for (std::size_t wave_begin = 0u; wave_begin < batch_size;) {
                                    const std::uint64_t seed_wave_prepare_started_ns =
                                        wave_begin == 0u ? first_wave_prepare_started_ns
                                                         : dispatch_host_now_ns();
                                    const std::size_t wave_end = std::min(
                                        batch_size, wave_begin + active_fragment_wave_size);
                                    wave_queries.clear();
                                    for (std::size_t row = wave_begin; row < wave_end; ++row) {
                                        wave_queries.push_back(
                                            bioseq::indexing::fragment_seed_query{
                                                .minimizers =
                                                    retained_query_sketches[row]->minimizers,
                                                .query_length = static_cast<sequence_size_t>(
                                                    (*per_row_query_hdrs)[row]->bp_size),
                                                .kmer_size = native_query_params[row].kmer_size,
                                            });
                                    }
                                    bool retry_at_worker_bound = false;
                                    {
                                        fragment_anchor_wave_completion wave_completion{
                                            query_wave_scratch};
                                        std::optional<
                                            bioseq::indexing::prepared_fragment_anchor_batch>
                                            prepared_seed_batch;
                                        try {
                                            prepared_seed_batch.emplace(
                                                fragment_reader->prepare_fragment_anchor_batch(
                                                    memory_admission,
                                                    std::span<const bioseq::indexing::
                                                                  fragment_seed_query>{
                                                        wave_queries},
                                                    native_query_params[wave_begin].max_occ,
                                                    query_wave_scratch,
                                                    /*baseline_query_plan_matcher_for_testing=*/
                                                    false,
                                                    /*verify_compositional_order_for_testing=*/
                                                    false,
                                                    fragment_lookup_route));
                                        } catch (const bioseq::indexing::
                                                     persisted_minimizer_lookup_error& error) {
                                            retry_at_worker_bound =
                                                wave_begin == 0u &&
                                                full_wave_admission_retry_available &&
                                                error.failure_reason() ==
                                                    bioseq::indexing::
                                                        persisted_minimizer_lookup_error::reason::
                                                            admission_or_allocation;
                                            if (!retry_at_worker_bound)
                                                throw;
                                        }

                                        if (!retry_at_worker_bound) {
                                            if (native_group
                                                    ->compact_direct_query_set_validation_pending) {
                                                const bool complete_query_set =
                                                    wave_begin == 0u && wave_end == batch_size &&
                                                    wave_queries.size() == batch_size;
                                                if (!complete_query_set ||
                                                    !batch_processor::
                                                        publish_compact_direct_query_set_stats(
                                                            *prepared_target,
                                                            *native_group,
                                                            prepared_seed_batch
                                                                ->compact_direct_query_stats())) {
                                                    throw bioseq::indexing::
                                                        persisted_minimizer_lookup_error(
                                                            "compact-direct full-batch query-set "
                                                            "stats were rejected");
                                                }
                                            }
                                            const std::uint64_t seed_wave_prepare_host_wall_ns =
                                                elapsed_dispatch_host_ns(
                                                    seed_wave_prepare_started_ns);
                                            bioseq::parallelism::parallel_for_pipeline(
                                                wave_begin,
                                                wave_end,
                                                1,
                                                bioseq::parallelism::task_parallel_minimums::
                                                    k_query_count,
                                                bioseq::parallelism::task_parallel_region::
                                                    query_batch,
                                                [&align_fragment_query,
                                                 &prepared_seed_batch,
                                                 wave_begin](const std::size_t range_begin,
                                                             const std::size_t range_end) {
                                                    for (std::size_t row = range_begin;
                                                         row < range_end;
                                                         ++row) {
                                                        const auto query_index =
                                                            prepared_seed_batch->query_view(
                                                                row - wave_begin);
                                                        align_fragment_query(row, query_index);
                                                    }
                                                });
                                            native_collections[wave_begin]
                                                .record_fragment_native_query_stats(
                                                    prepared_seed_batch->stats());
                                            native_collections[wave_begin]
                                                .record_seed_lookup_host_time(
                                                    seed_wave_prepare_host_wall_ns);
                                        }
                                    }
                                    if (retry_at_worker_bound) {
                                        full_wave_admission_retry_available = false;
                                        if (query_wave_scratch.wave_outstanding()) {
                                            throw bioseq::indexing::
                                                persisted_minimizer_lookup_error(
                                                    "fragment-native failed wave retained "
                                                    "active scratch state");
                                        }
                                        query_wave_scratch.release_retained_capacity();
                                        batch_processor::
                                            finalize_pending_compact_direct_query_set_validation_fallback(
                                                *prepared_target);
                                        active_fragment_wave_size =
                                            worker_bounded_fragment_wave_size;
                                        wave_queries.clear();
                                        continue;
                                    }
                                    wave_begin = wave_end;
                                }
                                return {};
                            });

                        if (!native_collections.empty() &&
                            native_group->persisted_reader_queries == 0u &&
                            native_group->persisted_reader_load_host_wall_ns != 0u) {
                            native_collections.front().record_persisted_index_decode_host_time(
                                native_group->persisted_reader_load_host_wall_ns);
                        }
                        for (std::size_t row = 0; row < batch_size; ++row) {
                            attach_retained_hash_major_query_stats(
                                native_collections[row],
                                retained_query_sketches[row]->execution_stats);
                            scanner_pg_ctx->target_index_sources[row] =
                                ::bioseq::pipeline_stats::target_index_source::persisted;
                            scanner_pg_ctx->persisted_generation_ids[row] =
                                native_group->persisted_generation_id;
                            scanner_pg_ctx->persisted_source_identities[row] =
                                native_group->persisted_source_identity;
                        }
                        native_group->persisted_reader_queries +=
                            static_cast<std::uint64_t>(batch_size);

                        const auto requested_source = requested_target_index_source(scanner_pg_ctx);
                        for (std::size_t row = 0; row < batch_size; ++row) {
                            bioseq::postgres::support::invoke_pg_as_cpp(
                                [] { CHECK_FOR_INTERRUPTS(); });
                            if (record_target_operational_outcomes) {
                                record_target_row(stats);
                                record_target_index_outcome(
                                    stats,
                                    requested_source,
                                    ::bioseq::pipeline_stats::target_index_source::persisted);
                            }
                            record_unattested_job(
                                stats,
                                native_query_params[row],
                                requested_source,
                                ::bioseq::pipeline_stats::target_index_source::persisted,
                                scanner_pg_ctx,
                                row,
                                /*per_row_query_mode=*/true,
                                broadcast_query_ordinal,
                                &native_collections[row],
                                /*consumed_shared_cached_query_stats=*/false,
                                cached_query_minimizer_stats_accounted);
                            bioseq_log<logging_level::debug>(
                                "[bioseq] pg: batch_processor row query_id={} "
                                "target_bp_size={} k={} w={} min_chain_score={} "
                                "min_seed_count={} prebuilt_index=true produced "
                                "{} match(es) [fragment-native-query-parallel]",
                                static_cast<int>(query_ids[row]),
                                shared_target_bp_size,
                                native_query_params[row].kmer_size,
                                native_query_params[row].window_size,
                                native_query_params[row].min_chain_score,
                                native_query_params[row].min_seed_count,
                                native_collections[row].size());
                            if (!native_collections[row].is_empty()) {
                                results[query_ids[row]] =
                                    std::move(native_collections[row].results);
                            }
                        }
                        native_batch_completed = true;
                    }
                });
            if (native_batch_completed)
                return;
        }
    }
    if (scanner_pg_ctx != nullptr && scanner_pg_ctx->cpu_prepared_target != nullptr &&
        !scanner_pg_ctx->cpu_prepared_target->query_set_validation_ready()) {
        (void)run_aligner_pg_checked(
            "compact-direct query-set slow fallback",
            [prepared_target = scanner_pg_ctx->cpu_prepared_target]() -> match_result_collection {
                batch_processor::finalize_pending_compact_direct_query_set_validation_fallback(
                    *prepared_target);
                return {};
            });
    }
    generic_rows();
}

} // namespace bioseq::simd::typed_dispatch::typed_support_detail
