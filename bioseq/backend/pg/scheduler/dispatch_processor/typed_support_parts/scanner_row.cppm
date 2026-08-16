/**
 * @file typed_support_parts/scanner_row.cppm
 * @brief Defines the complete PostgreSQL-backed typed scanner-row algorithm.
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

export module bioseq.postgres.scheduler.dispatch_processor.typed_support:scanner_row;

import bioseq.postgres.scheduler.dispatch_processor;
import :policy_stats;
import :pg_alignment_errors;
import :transient_storage;
import :resident_targets;
import :row_adapters;
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

// Keep the PG-backed scanner row path as a separate function so GCC does not
// compile the whole typed scheduler as one very large control-flow graph.
template <class p_sequence>
[[nodiscard]] match_result_collection process_scanner_pg_row_typed(
    bioseq::storage::lazy_sequence_view<
        typename compact_sequence_range<p_sequence>::traits::alphabet_type> query_view,
    std::optional<bioseq::storage::lazy_sequence_view<
        typename compact_sequence_range<p_sequence>::traits::alphabet_type>> target_recipe_view,
    const bioseq::pg_types::seq* target_hdr,
    const std::size_t row_index,
    const sequence_size_t target_bp_size,
    const std::size_t min_bp,
    const minimap2_parameters& dispatch_params,
    const minimap2_parameters& params,
    const bool per_row_query_mode,
    std::optional<std::vector<minimap_sketch_data>>& cached_query_minimizers,
    std::optional<pipe::minimizer_range_execution_stats>& cached_query_minimizer_stats,
    bool& consumed_shared_cached_query_stats,
    const uint8_t alphabet_code,
    const uint8_t storage_unit_byte_width,
    batch_processor::scanner_pg_context* scanner_pg_ctx,
    bioseq::parallelism::global_pool& pool,
    pg_sequence_read_session* active_read_session) {
    match_result_collection collection;
    // Recompute-owned and per-row sketches are independent operations. A
    // broadcast persisted-query sketch is shared across rows: every consuming
    // job receives its stats for route acceptance, while the caller accounts
    // the shared operation only once on the first stats-valid consumer.
    consumed_shared_cached_query_stats = false;
    bool row_consumed_persisted_query_sketch = false;
    std::uint64_t query_sketch_host_wall_ns = 0;
    auto* cpu_prepared_target = scanner_pg_ctx->cpu_prepared_target;
    if (cpu_prepared_target != nullptr) {
        if (selected_vulkan_execution(params)) {
            raise_typed_dispatch_pg_error_as_cpp(
                ERRCODE_FEATURE_NOT_SUPPORTED,
                "Vulkan execution cannot consume a CPU prepared target");
        }
        if (!cpu_prepared_target->unions_ready ||
            cpu_prepared_target->target_rowno != scanner_pg_ctx->target_rownos[row_index] ||
            cpu_prepared_target->target_bp_size != static_cast<std::uint64_t>(target_bp_size)) {
            raise_typed_dispatch_pg_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "bioseq batch join CPU prepared-target identity is invalid");
        }
    }
    scanner_pg_ctx->target_index_sources[row_index] =
        ::bioseq::pipeline_stats::target_index_source::recomputed;
    std::optional<std::vector<minimap_sketch_data>> row_query_minimizers;
    std::optional<pipe::minimizer_range_execution_stats> row_query_minimizer_stats;
    const batch_processor::cpu_prepared_query_sketch* retained_prepared_query_sketch = nullptr;
    const sequence_size_t query_bp_size = query_view.bp_size();
    const auto unobserved_query_view = query_view;
    const auto lazy_query_view = unobserved_query_view.with_payload_allocation_observer(
        std::addressof(scanner_pg_ctx->mmsearch_query_payload_observer));
    const auto fq_pg_view = lazy_query_view;
    const std::size_t target_letters_per_unit = bioseq::storage::packed_letters_per_unit(
        storage_unit_byte_width, static_cast<std::uint8_t>(p_sequence::letter_bit_width));
    if (target_letters_per_unit == 0u) {
        raise_typed_dispatch_pg_error_as_cpp(ERRCODE_INTERNAL_ERROR,
                                             "PG lazy facade has invalid packed storage geometry");
    }
    const auto& loc_opt = scanner_pg_ctx->target_locators[row_index];
    const bool persisted_required =
        scanner_pg_ctx->index_read_requirement ==
        bioseq::indexing::index_read_requirement::require_compatible_persisted;
    if (persisted_required &&
        (cpu_prepared_target == nullptr || selected_vulkan_execution(params))) {
        raise_required_persisted_index_error(
            scanner_pg_ctx,
            scanner_pg_ctx->target_rownos[row_index],
            "the live persisted reader is available only through the retained "
            "CPU prepared-target lifecycle; GPU and ad-hoc row fallback are forbidden");
    }
    if (persisted_required && !loc_opt.has_value()) {
        raise_required_persisted_index_error(
            scanner_pg_ctx,
            scanner_pg_ctx->target_rownos[row_index],
            "target payload is inline; persisted minimizer storage requires "
            "a locator-backed target");
    }
    if (persisted_required && scanner_pg_ctx->target_rownos[row_index] < 0) {
        raise_required_persisted_index_error(scanner_pg_ctx,
                                             scanner_pg_ctx->target_rownos[row_index],
                                             "logical target row identity is unavailable");
    }
    if (target_recipe_view.has_value()) {
        if (loc_opt.has_value() || target_hdr != nullptr) {
            raise_typed_dispatch_pg_error_as_cpp(
                ERRCODE_DATA_CORRUPTED, "target recipe is not exclusive with flat/locator storage");
        }
        if (persisted_required) {
            raise_required_persisted_index_error(
                scanner_pg_ctx,
                scanner_pg_ctx->target_rownos[row_index],
                "a transformed target has no compatible persisted base-row index");
        }
        const auto observed_target = target_recipe_view->with_payload_allocation_observer(
            std::addressof(scanner_pg_ctx->mmsearch_target_payload_observer));
        collection =
            target_bp_size > postgres_segmentation_threshold_bp
                ? minimap_port_segmented_pg_checked(lazy_query_view, observed_target, params)
                : minimap_port_raw_pg_checked(lazy_query_view, observed_target, params);
    } else if (loc_opt.has_value()) {
        // Compute last-fragment padding from the canonical facade
        // geometry. For
        // total_bp = N, padding = ceil(N/lpu)*lpu - N.
        const std::uint64_t _remainder = loc_opt->logical_length % target_letters_per_unit;
        const std::size_t _padding =
            static_cast<std::size_t>(_remainder == 0u ? 0u : target_letters_per_unit - _remainder);
        with_source_provider_pg_target<p_sequence>(
            *scanner_pg_ctx,
            *loc_opt,
            alphabet_code,
            storage_unit_byte_width,
            static_cast<std::uint16_t>(_padding),
            active_read_session,
            [scanner_pg_ctx,
             cpu_prepared_target,
             &dispatch_params,
             &params,
             min_bp,
             per_row_query_mode,
             persisted_required,
             &row_query_minimizers,
             &cached_query_minimizers,
             &row_query_minimizer_stats,
             &cached_query_minimizer_stats,
             query_bp_size,
             lazy_query_view,
             fq_pg_view,
             target_bp_size,
             alphabet_code,
             storage_unit_byte_width,
             row_index,
             &loc_opt,
             _padding,
             &pool,
             target_letters_per_unit,
             &collection,
             &row_consumed_persisted_query_sketch,
             &retained_prepared_query_sketch,
             &query_sketch_host_wall_ns](auto ft_pg_view, pg_sequence_read_session*) {
                ft_pg_view = ft_pg_view.with_payload_allocation_observer(
                    std::addressof(scanner_pg_ctx->mmsearch_target_payload_observer));
                // Assemble from the stored
                // per-fragment minimizer index if present
                // (deserialise + globalise, no re-collection);
                // otherwise fall back to recompute.
                bool assembled = false;
                /*
                 * Persisted-payload reads and opportunistic recompute persistence are
                 * independent policies.  In particular, require_absent deliberately
                 * clears read_index_active, but index.recompute_async_persistence.enabled must
                 * still be able to build and enqueue the default minimizer-fragment index from the
                 * source sequence. Keep the minimizer-fragment RAM-first path reachable when both
                 * async write gates are captured for this scan; every prepared,
                 * sidecar, and assembled-cache read below remains guarded by
                 * read_index_active.
                 */
                const auto requested_target_backend =
                    target_backend_from_params_for_recompute(dispatch_params);
                const bool stored_minimizer_scheme_compatible =
                    requested_target_backend !=
                    bioseq::minimizers::minimizer_sketch_backend::bounded_nthash_simd;
                if (scanner_pg_ctx->read_index_active && !stored_minimizer_scheme_compatible) {
                    bioseq_log<logging_level::debug>(
                        "[bioseq] index: persisted read skipped for inapplicable storage profile "
                        "row={} "
                        "backend=bounded_nthash_simd reason=payload_has_no_scheme_identity",
                        static_cast<unsigned long long>(scanner_pg_ctx->target_rownos[row_index]));
                }
                const bool opportunistic_minimizer_fragment_recompute_requested =
                    !scanner_pg_ctx->read_index_active &&
                    scanner_pg_ctx->recompute_async_persist_active &&
                    scanner_pg_ctx->index_async_persist_active &&
                    stored_minimizer_scheme_compatible &&
                    scanner_pg_ctx->active_index == minimizers_per_fragment_index_name;
                if ((persisted_required ||
                     (scanner_pg_ctx->read_index_active && stored_minimizer_scheme_compatible) ||
                     opportunistic_minimizer_fragment_recompute_requested) &&
                    OidIsValid(scanner_pg_ctx->user_relid)) {
                    using index_storage_type = bioseq::storage::indexing::fragment_index_storage;
                    const bool want_seq_recompute =
                        scanner_pg_ctx->active_index == minimizers_per_sequence_index_name;
                    const bool want_seq = want_seq_recompute;
                    // The stored index is ALWAYS w=10,k=15 (pinned at build); the query
                    // minimizers + the seed pipeline MUST use the same w,k (NOT the size
                    // heuristic's reduced k), exactly as the prebuilt storage-profile branch does.
                    minimap2_parameters idx_params = dispatch_params;
                    idx_params.kmer_size = 15;
                    idx_params.window_size = 10;
                    if (min_bp < 50) {
                        idx_params.min_chain_score = 10;
                        idx_params.min_seed_count = 1;
                    } else if (min_bp < 200) {
                        idx_params.min_chain_score = 20;
                        idx_params.min_seed_count = 2;
                    }
                    std::optional<bioseq::indexing::gpu_minimizer_index_cache> pg_gpu_index_cache;
                    if (pg_vulkan_seed_lookup_selected(idx_params)) {
                        pg_gpu_index_cache.emplace(pg_gpu_index_resident_limit_bytes());
                    }
                    auto* pg_gpu_index_cache_ptr =
                        pg_gpu_index_cache ? &*pg_gpu_index_cache : nullptr;
                    const std::uint64_t target_rowno =
                        static_cast<std::uint64_t>(scanner_pg_ctx->target_rownos[row_index]);
                    auto& query_minimizers =
                        per_row_query_mode ? row_query_minimizers : cached_query_minimizers;
                    auto& query_minimizer_stats = per_row_query_mode ? row_query_minimizer_stats
                                                                     : cached_query_minimizer_stats;
                    auto ensure_q = [&query_minimizers,
                                     &query_minimizer_stats,
                                     query_bp_size,
                                     lazy_query_view,
                                     &idx_params,
                                     &row_consumed_persisted_query_sketch,
                                     &query_sketch_host_wall_ns] {
                        if (!query_minimizers.has_value()) {
                            if (!is_minimizer_query_length_representable<minimap_sketch_data>(
                                    static_cast<sequence_size_t>(query_bp_size))) {
                                raise_typed_dispatch_pg_error_as_cpp(
                                    ERRCODE_PROGRAM_LIMIT_EXCEEDED,
                                    "query exceeds the 28-bit minimizer position domain");
                            }
                            const std::uint64_t query_sketch_started_ns = dispatch_host_now_ns();
                            if (selected_vulkan_execution(idx_params)) {
                                bioseq::gpu::vulkan::vulkan_runtime_error err{
                                    "persisted Vulkan query sketch cannot use the CPU "
                                    "prepared-index path"};
                                raise_vulkan_pg_error("persisted query minimizer sketch",
                                                      err.what());
                            }
                            sketch_collector qc;
                            const minimizer_sketch_backend backend =
                                pipe::target_index_sketch_backend_from_params(idx_params);
                            auto stats = pipe::minimizer_dispatch_range<minimap_sketch_data>(
                                qc,
                                lazy_query_view,
                                0,
                                query_bp_size,
                                static_cast<size_t>(idx_params.window_size),
                                static_cast<size_t>(idx_params.kmer_size),
                                backend,
                                pipe::exact_simd_range_route::require_continuation,
                                idx_params.highway_kernels);
                            if (!pipe::minimizer_range_stats_matches_backend(
                                    backend,
                                    stats,
                                    /*require_nonempty_vector_work=*/false,
                                    pipe::exact_simd_range_route::require_continuation)) {
                                raise_typed_dispatch_pg_error_as_cpp(
                                    ERRCODE_INTERNAL_ERROR,
                                    "persisted query sketch stats do not match the selected "
                                    "minimizer backend");
                            }
                            query_minimizers.emplace(std::move(qc).extract_sketches_data());
                            query_minimizer_stats.emplace(std::move(stats));
                            query_sketch_host_wall_ns =
                                elapsed_dispatch_host_ns(query_sketch_started_ns);
                        } else if (!query_minimizer_stats.has_value()) {
                            raise_typed_dispatch_pg_error_as_cpp(
                                ERRCODE_DATA_CORRUPTED,
                                "cached persisted query minimizers have no operation stats");
                        }
                        row_consumed_persisted_query_sketch = true;
                    };
                    auto* prepared_group =
                        cpu_prepared_target == nullptr
                            ? nullptr
                            : cpu_prepared_target->find_group(
                                  idx_params.kmer_size,
                                  idx_params.window_size,
                                  idx_params.max_occ,
                                  pipe::target_index_sketch_backend_from_params(idx_params));
                    if (cpu_prepared_target != nullptr && prepared_group == nullptr) {
                        raise_typed_dispatch_pg_error_as_cpp(
                            ERRCODE_DATA_CORRUPTED,
                            "bioseq batch join prepared query group is missing");
                    }
                    if (persisted_required) {
                        const bool has_fragment_reader =
                            prepared_group != nullptr &&
                            prepared_group->persisted_fragment_reader.has_value();
                        const bool has_fragment_mapping_guard =
                            prepared_group != nullptr &&
                            prepared_group->persisted_fragment_mapping_guard.has_value() &&
                            static_cast<bool>(*prepared_group->persisted_fragment_mapping_guard);
                        const bool has_grouped_soa_reader =
                            prepared_group != nullptr &&
                            prepared_group->persisted_grouped_soa_reader.has_value();
                        const bool has_hash_major_reader =
                            prepared_group != nullptr &&
                            prepared_group->persisted_hash_major_reader.has_value();
                        const bool has_full_hash_major_source_identity =
                            prepared_group != nullptr &&
                            prepared_group->persisted_hash_major_source_identity_present;
                        const bool hash_major_identity_valid =
                            prepared_group == nullptr ||
                            (!has_hash_major_reader
                                 ? !has_full_hash_major_source_identity &&
                                       prepared_group->persisted_hash_major_source_identity ==
                                           std::array<std::byte, 16>{}
                                 : has_full_hash_major_source_identity &&
                                       prepared_group->persisted_source_identity ==
                                           hash_major_source_diagnostic_token(
                                               prepared_group
                                                   ->persisted_hash_major_source_identity));
                        const bool canonical_owner_exclusive =
                            prepared_group == nullptr ||
                            prepared_group->query_hash_union.capacity() == 0u;
                        bool hash_major_read_receipt_valid = !has_hash_major_reader;
                        if (has_hash_major_reader) {
                            const auto& receipt = prepared_group->persisted_read_stats.hash_major;
                            const auto& memory =
                                prepared_group->persisted_hash_major_reader->memory_stats();
                            const std::uint64_t stage_peak = std::max({
                                receipt.directory_stage_peak_bytes,
                                receipt.control_stage_peak_bytes,
                                receipt.posting_stage_peak_bytes,
                            });
                            hash_major_read_receipt_valid =
                                receipt.layout_revision ==
                                    bioseq::indexing::k_hash_major_minimizer_layout_revision &&
                                receipt.sessions == 1u &&
                                receipt.generation_id == prepared_group->persisted_generation_id &&
                                receipt.source_identity ==
                                    prepared_group->persisted_hash_major_source_identity &&
                                receipt.directory_row_crc_receipts == receipt.directory_rows &&
                                receipt.control_row_crc_receipts == receipt.control_rows &&
                                receipt.posting_row_crc_receipts == receipt.posting_rows &&
                                receipt.control_block_crc_receipts ==
                                    receipt.selected_block_count &&
                                receipt.control_semantic_receipts == receipt.selected_block_count &&
                                receipt.posting_ranges_delivered ==
                                    receipt.posting_semantic_receipts &&
                                receipt.posting_range_crc_receipts <=
                                    receipt.posting_ranges_delivered &&
                                receipt.query_count == memory.query_count &&
                                receipt.accepted_hash_count == memory.accepted_hash_count &&
                                receipt.retained_position_count == memory.retained_position_count &&
                                receipt.peak_admitted_bytes == stage_peak &&
                                receipt.retained_owner_bytes == receipt.admitted_bytes &&
                                receipt.admitted_bytes ==
                                    prepared_group->persisted_hash_major_reader->admitted_bytes() &&
                                receipt.generic_source_fallback_calls == 0u &&
                                receipt.failures == 0u && !receipt.saturated;
                        }
                        if (prepared_group == nullptr ||
                            !batch_processor::exactly_one_native_persisted_reader(
                                has_fragment_reader,
                                has_grouped_soa_reader,
                                has_hash_major_reader) ||
                            has_fragment_reader != has_fragment_mapping_guard ||
                            (has_fragment_reader &&
                             !prepared_group->persisted_fragment_reader->ready()) ||
                            (has_grouped_soa_reader &&
                             !prepared_group->persisted_grouped_soa_reader->ready()) ||
                            prepared_group->target_index ||
                            prepared_group->target_index_source !=
                                ::bioseq::pipeline_stats::target_index_source::persisted ||
                            prepared_group->persisted_generation_id == 0 ||
                            prepared_group->persisted_source_identity == 0 ||
                            !hash_major_identity_valid || !canonical_owner_exclusive ||
                            !hash_major_read_receipt_valid) {
                            raise_required_persisted_index_error(
                                scanner_pg_ctx,
                                scanner_pg_ctx->target_rownos[row_index],
                                "the exact READY native persisted reader is absent, incomplete, "
                                "duplicated, or "
                                "mixed with a storage-profile-transcoded assembled index");
                        }
                        if (scanner_pg_ctx->explicit_query_ordinals) {
                            retained_prepared_query_sketch =
                                prepared_group->find_retained_query_sketch(
                                    scanner_pg_ctx->query_ordinals[row_index]);
                        }
                        if (retained_prepared_query_sketch != nullptr) {
                            const auto backend =
                                pipe::target_index_sketch_backend_from_params(idx_params);
                            if (retained_prepared_query_sketch->query_bp_size !=
                                    static_cast<std::uint64_t>(query_bp_size) ||
                                !pipe::minimizer_range_stats_matches_backend(
                                    backend,
                                    retained_prepared_query_sketch->execution_stats,
                                    /*require_nonempty_vector_work=*/false,
                                    pipe::exact_simd_range_route::require_continuation)) {
                                raise_typed_dispatch_pg_error_as_cpp(
                                    ERRCODE_DATA_CORRUPTED,
                                    "retained native query sketch stats do not "
                                    "match the selected minimizer backend");
                            }
                            row_consumed_persisted_query_sketch = true;
                        } else {
                            ensure_q();
                        }
                        const std::vector<minimap_sketch_data>& alignment_query_minimizers =
                            retained_prepared_query_sketch != nullptr
                                ? retained_prepared_query_sketch->minimizers
                                : *query_minimizers;
                        if (has_hash_major_reader) {
                            collection = minimap_port_hash_major_pg_checked(
                                fq_pg_view,
                                ft_pg_view,
                                alignment_query_minimizers,
                                *prepared_group->persisted_hash_major_reader,
                                idx_params,
                                /*gpu_index_cache=*/nullptr,
                                bioseq::indexing::gpu_index_cache_identity{});
                        } else if (has_fragment_reader) {
                            collection = run_aligner_pg_checked(
                                "fragment-native single-query anchor preparation",
                                [&alignment_query_minimizers,
                                 query_bp_size,
                                 &idx_params,
                                 prepared_group,
                                 fq_pg_view,
                                 ft_pg_view]() -> match_result_collection {
                                    const std::uint64_t prepare_started_ns = dispatch_host_now_ns();
                                    auto& admission = bioseq::parallelism::
                                        memory_admission_controller::instance();
                                    auto scratch_result =
                                        bioseq::indexing::query_wave_scratch::create(admission);
                                    if (!scratch_result) {
                                        throw bioseq::indexing::persisted_minimizer_lookup_error(
                                            "fragment-native query scratch was not "
                                            "admitted; wire_error=" +
                                            std::to_string(
                                                static_cast<unsigned>(scratch_result.error())));
                                    }
                                    auto scratch = std::move(*scratch_result);
                                    const bioseq::indexing::fragment_seed_query fragment_query{
                                        .minimizers = alignment_query_minimizers,
                                        .query_length = static_cast<sequence_size_t>(query_bp_size),
                                        .kmer_size = idx_params.kmer_size,
                                    };
                                    fragment_anchor_wave_completion wave_completion{scratch};
                                    auto prepared_seed_batch =
                                        prepared_group->persisted_fragment_reader
                                            ->prepare_fragment_anchor_batch(
                                                admission,
                                                std::span<
                                                    const bioseq::indexing::fragment_seed_query>{
                                                    &fragment_query, 1u},
                                                idx_params.max_occ,
                                                scratch,
                                                /*baseline_query_plan_matcher_for_testing=*/false,
                                                /*verify_compositional_order_for_testing=*/false,
                                                prepared_group->persisted_fragment_lookup_route);
                                    const auto prepared_query_index =
                                        prepared_seed_batch.query_view(0u);
                                    const std::uint64_t prepare_host_wall_ns =
                                        elapsed_dispatch_host_ns(prepare_started_ns);
                                    match_result_collection match_collection =
                                        minimap_port_fragment_query_owned(
                                            fq_pg_view,
                                            ft_pg_view,
                                            alignment_query_minimizers,
                                            prepared_query_index,
                                            idx_params,
                                            /*gpu_index_cache=*/nullptr,
                                            bioseq::indexing::gpu_index_cache_identity{});
                                    match_collection.record_seed_lookup_host_time(
                                        prepare_host_wall_ns);
                                    match_collection.record_fragment_native_query_stats(
                                        prepared_seed_batch.stats());
                                    return match_collection;
                                });
                        } else {
                            collection = minimap_port_grouped_soa_pg_checked(
                                fq_pg_view,
                                ft_pg_view,
                                alignment_query_minimizers,
                                *prepared_group->persisted_grouped_soa_reader,
                                idx_params,
                                /*gpu_index_cache=*/nullptr,
                                bioseq::indexing::gpu_index_cache_identity{});
                        }
                        if (prepared_group->persisted_reader_queries == 0u &&
                            prepared_group->persisted_reader_load_host_wall_ns != 0u) {
                            collection.record_persisted_index_decode_host_time(
                                prepared_group->persisted_reader_load_host_wall_ns);
                        }
                        ++prepared_group->persisted_reader_queries;
                        assembled = true;
                        scanner_pg_ctx->target_index_sources[row_index] =
                            ::bioseq::pipeline_stats::target_index_source::persisted;
                        scanner_pg_ctx->persisted_generation_ids[row_index] =
                            prepared_group->persisted_generation_id;
                        scanner_pg_ctx->persisted_source_identities[row_index] =
                            prepared_group->persisted_source_identity;
                    }
                    if (scanner_pg_ctx->read_index_active && !persisted_required &&
                        prepared_group != nullptr && prepared_group->target_index) {
                        ensure_q();
                        collection = minimap_port_fragment_index_vector_pg_checked(
                            fq_pg_view,
                            ft_pg_view,
                            *query_minimizers,
                            *prepared_group->target_index,
                            idx_params,
                            /*gpu_index_cache=*/nullptr,
                            bioseq::indexing::gpu_index_cache_identity{
                                .assembly_identity = prepared_group->key_set_checksum});
                        ++prepared_group->target_index_reuses;
                        assembled = true;
                        scanner_pg_ctx->target_index_sources[row_index] =
                            prepared_group->target_index_source;
                    }
                    auto make_cache_key = [scanner_pg_ctx,
                                           loc_opt,
                                           &query_minimizers,
                                           &idx_params,
                                           row_index](std::string_view index_name,
                                                      sequence_size_t clip_lo_bp,
                                                      sequence_size_t clip_hi_bp) {
                        const auto query_span = std::span<const minimap_sketch_data>(
                            query_minimizers->data(), query_minimizers->size());
                        const auto sketch_backend =
                            pipe::target_index_sketch_backend_from_params(idx_params);
                        return bioseq::storage::indexing::assembled_index_key{
                            .db_oid = scanner_pg_ctx->db_oid,
                            .user_relid = scanner_pg_ctx->user_relid,
                            .tablespace_oid = scanner_pg_ctx->tablespace_oid,
                            .main_relfilenode = scanner_pg_ctx->main_relfilenode,
                            .column_attno = scanner_pg_ctx->mmsearch_seq_attno,
                            .index_name_hash = bioseq::storage::indexing::index_name_hash(
                                scanner_pg_ctx->db_oid,
                                scanner_pg_ctx->tablespace_oid,
                                scanner_pg_ctx->main_relfilenode,
                                scanner_pg_ctx->mmsearch_seq_attno,
                                index_name),
                            .sketch_backend = sketch_backend,
                            .minimizer_scheme =
                                bioseq::indexing::minimizer_scheme_identity_for_backend(
                                    sketch_backend),
                            .query_key = static_cast<std::uint64_t>(
                                scanner_pg_ctx->target_rownos[row_index]),
                            .query_signature = bioseq::storage::indexing::query_minimizer_signature(
                                query_span,
                                static_cast<std::uint32_t>(idx_params.kmer_size),
                                static_cast<std::uint32_t>(idx_params.window_size),
                                static_cast<std::uint32_t>(idx_params.max_occ)),
                            .clip_lo_bp = clip_lo_bp,
                            .clip_hi_bp = clip_hi_bp,
                        };
                    };
                    auto run_fragment_backed_index = [&ensure_q,
                                                      &loc_opt,
                                                      &idx_params,
                                                      query_bp_size,
                                                      &query_minimizers,
                                                      &collection,
                                                      &assembled,
                                                      &make_cache_key,
                                                      fq_pg_view,
                                                      ft_pg_view,
                                                      target_letters_per_unit,
                                                      pg_gpu_index_cache_ptr,
                                                      prepared_group,
                                                      cpu_prepared_target](
                                                         index_storage_type& fragment_store,
                                                         std::string_view cache_index_name) {
                        ensure_q();
                        const sequence_size_t total_bp = loc_opt->logical_length;
                        const std::size_t a_lpu = target_letters_per_unit;
                        constexpr sequence_size_t frag_bp = static_cast<sequence_size_t>(
                            bioseq::storage::postgres_lazy_sequence_binding<
                                p_sequence>::fragment_bp_capacity());
                        static_assert(frag_bp != 0u);
                        const std::size_t size_u =
                            static_cast<std::size_t>((total_bp + a_lpu - 1) / a_lpu);
                        bioseq::indexing::minimizer_assembly_scratch scratch;
                        const auto& assembly_minimizers = prepared_group == nullptr
                                                              ? *query_minimizers
                                                              : prepared_group->query_hash_union;
                        auto assemble_one = [a_lpu,
                                             total_bp,
                                             frag_bp,
                                             &fragment_store,
                                             &scratch,
                                             &assembly_minimizers](std::size_t cs_u,
                                                                   std::size_t ce_u) {
                            const sequence_size_t cs_bp =
                                static_cast<sequence_size_t>(cs_u) * a_lpu;
                            const sequence_size_t ce_bp =
                                std::min(static_cast<sequence_size_t>(ce_u) * a_lpu, total_bp);
                            const std::size_t frag_lo = static_cast<std::size_t>(cs_bp / frag_bp);
                            const std::size_t frag_hi =
                                static_cast<std::size_t>(bioseq::common::ceil_div(ce_bp, frag_bp));
                            const auto query_span = std::span<const minimap_sketch_data>(
                                assembly_minimizers.data(), assembly_minimizers.size());
                            return bioseq::indexing::minimizers_per_fragment_index<
                                index_storage_type>::assemble_segment_for_query(fragment_store,
                                                                                query_span,
                                                                                frag_lo,
                                                                                frag_hi,
                                                                                frag_bp,
                                                                                total_bp,
                                                                                cs_bp,
                                                                                ce_bp,
                                                                                scratch);
                        };
                        auto run_with_segment_index = [&make_cache_key,
                                                       &assemble_one,
                                                       fq_pg_view,
                                                       ft_pg_view,
                                                       &query_minimizers,
                                                       &idx_params,
                                                       cache_index_name,
                                                       pg_gpu_index_cache_ptr,
                                                       prepared_group,
                                                       cpu_prepared_target,
                                                       total_bp](sequence_size_t clip_lo_bp,
                                                                 sequence_size_t clip_hi_bp,
                                                                 std::size_t cs_u,
                                                                 std::size_t ce_u) {
                            const auto cache_key =
                                make_cache_key(cache_index_name, clip_lo_bp, clip_hi_bp);
                            const auto gpu_identity = pg_gpu_index_identity(cache_key);
                            if (prepared_group == nullptr)
                                if (auto hit = bioseq::storage::indexing::lookup_assembled_index(
                                        cache_key)) {
                                    return minimap_port_fragment_index_vector_pg_checked(
                                        fq_pg_view,
                                        ft_pg_view,
                                        *query_minimizers,
                                        *hit->index,
                                        idx_params,
                                        pg_gpu_index_cache_ptr,
                                        gpu_identity);
                                }
                            if (prepared_group != nullptr &&
                                !prepared_group->target_index_permit.has_value()) {
                                const std::uint64_t admission_bytes =
                                    bioseq::parallelism::minimizer_index_peak_bytes_for_bp(
                                        static_cast<std::uint64_t>(total_bp));
                                auto permit =
                                    bioseq::parallelism::memory_admission_controller::instance()
                                        .try_reserve(admission_bytes);
                                if (!permit) {
                                    raise_typed_dispatch_pg_error_as_cpp(
                                        ERRCODE_OUT_OF_MEMORY,
                                        "bioseq batch join prepared persisted target "
                                        "index cannot be admitted");
                                }
                                prepared_group->target_index_permit.emplace(std::move(*permit));
                            }
                            auto idx = assemble_one(cs_u, ce_u);
                            if (prepared_group != nullptr) {
                                if (prepared_group->target_index) {
                                    ++prepared_group->duplicate_build_attempts;
                                } else {
                                    prepared_group->target_index = std::make_shared<
                                        bioseq::indexing::minimizer_hashmap_fragment_index>(
                                        std::move(idx));
                                    prepared_group->target_index_source =
                                        ::bioseq::pipeline_stats::target_index_source::persisted;
                                    ++prepared_group->target_index_builds;
                                    prepared_group->build_set_checksum_present = true;
                                    prepared_group->build_set_checksum =
                                        prepared_group->key_set_checksum;
                                    ++cpu_prepared_target->physical_target_index_reads;
                                }
                                return minimap_port_fragment_index_vector_pg_checked(
                                    fq_pg_view,
                                    ft_pg_view,
                                    *query_minimizers,
                                    *prepared_group->target_index,
                                    idx_params,
                                    /*gpu_index_cache=*/nullptr,
                                    bioseq::indexing::gpu_index_cache_identity{
                                        .assembly_identity = prepared_group->key_set_checksum});
                            }
                            match_result_collection match_collection =
                                minimap_port_fragment_index_vector_pg_checked(
                                    fq_pg_view,
                                    ft_pg_view,
                                    *query_minimizers,
                                    idx,
                                    idx_params,
                                    pg_gpu_index_cache_ptr,
                                    gpu_identity);
                            bioseq::storage::indexing::store_assembled_index(cache_key,
                                                                             std::move(idx));
                            return match_collection;
                        };
                        if (prepared_group != nullptr) {
                            /* One full-target union-filtered index is the
                             * scan-retained object reused by every wave. */
                            collection = run_with_segment_index(0, total_bp, 0, size_u);
                        } else if (total_bp <= postgres_segmentation_threshold_bp) {
                            collection = run_with_segment_index(0, total_bp, 0, size_u);
                        } else {
                            match_result_collection merged_collection;
                            std::vector<match_result>& merged = merged_collection.results;
                            const sequence_size_t query_len = query_bp_size;
                            for (auto sr :
                                 pg_segment_unit_ranges(total_bp, query_len, a_lpu, idx_params)) {
                                const sequence_size_t clip_lo_bp =
                                    static_cast<sequence_size_t>(sr.cs_u) * a_lpu;
                                const sequence_size_t clip_hi_bp = std::min(
                                    static_cast<sequence_size_t>(sr.ce_u) * a_lpu, total_bp);
                                match_result_collection seg_res = run_with_segment_index(
                                    clip_lo_bp, clip_hi_bp, sr.cs_u, sr.ce_u);
                                merged_collection.combine_execution_stats(seg_res);
                                for (auto& r : seg_res.results) {
                                    if (r.target_start >= sr.core_lo_bp &&
                                        r.target_start < sr.core_hi_bp) {
                                        merged.push_back(std::move(r));
                                    }
                                }
                            }
                            collection = std::move(merged_collection);
                        }
                        assembled = true;
                    };
                    // Route by the table's ACTIVE read index.
                    // (set_active_read_index / resolve_active_read_index, default
                    // minimizers_per_fragment). Per-row presence still gates: an
                    // active index not materialised for THIS row recomputes.
                    if (scanner_pg_ctx->read_index_active && !persisted_required && !assembled) {
                        if (want_seq) {
                            auto seq_store = bioseq::storage::indexing::open_fragment_storage(
                                scanner_pg_ctx->user_relid,
                                scanner_pg_ctx->db_oid,
                                scanner_pg_ctx->tablespace_oid,
                                scanner_pg_ctx->main_relfilenode,
                                scanner_pg_ctx->mmsearch_seq_attno,
                                minimizers_per_sequence_index_name,
                                *loc_opt,
                                static_cast<std::uint64_t>(
                                    scanner_pg_ctx->target_rownos[row_index]),
                                scanner_pg_ctx->caches,
                                scanner_pg_ctx->parent_mctx);
                            with_checked_minimizer_fragment_reader(
                                seq_store,
                                [&assembled,
                                 &collection,
                                 &cpu_prepared_target,
                                 &ensure_q,
                                 &fq_pg_view,
                                 &ft_pg_view,
                                 &row_index,
                                 &idx_params,
                                 &loc_opt,
                                 &make_cache_key,
                                 &pg_gpu_index_cache_ptr,
                                 &prepared_group,
                                 &query_bp_size,
                                 &query_minimizers,
                                 &run_fragment_backed_index,
                                 &scanner_pg_ctx,
                                 &seq_store,
                                 &target_letters_per_unit] {
                                    if (seq_store.number_of_chunks(
                                            bioseq::indexing::k_directory_sub_key) >= 1) {
                                        const std::size_t dir_bytes = seq_store.chunk_byte_size(
                                            bioseq::indexing::k_directory_sub_key, 0);
                                        if (dir_bytes == sizeof(std::int64_t)) {
                                            run_fragment_backed_index(
                                                seq_store, minimizers_per_sequence_index_name);
                                            bioseq_log<logging_level::debug>(
                                                "[bioseq] index: logical fragment-backed read "
                                                "index=minimizers_per_sequence row={} target_bp={}",
                                                static_cast<unsigned long long>(
                                                    scanner_pg_ctx->target_rownos[row_index]),
                                                static_cast<unsigned long long>(
                                                    loc_opt->logical_length));
                                        } else {
                                            ensure_q();
                                            const sequence_size_t total_bp =
                                                loc_opt->logical_length;
                                            const std::size_t a_lpu = target_letters_per_unit;
                                            const std::vector<minimap_sketch_data>& query_vec =
                                                *query_minimizers;
                                            const std::vector<minimap_sketch_data>&
                                                assembly_query_vec =
                                                    prepared_group == nullptr
                                                        ? query_vec
                                                        : prepared_group->query_hash_union;
                                            const auto query_span =
                                                std::span<const minimap_sketch_data>(
                                                    assembly_query_vec.data(),
                                                    assembly_query_vec.size());
                                            bioseq::indexing::minimizer_assembly_scratch sq_scratch;
                                            auto run_minimizer_sequence_index = [&make_cache_key,
                                                                                 &seq_store,
                                                                                 fq_pg_view,
                                                                                 ft_pg_view,
                                                                                 &idx_params,
                                                                                 &query_vec,
                                                                                 query_span,
                                                                                 total_bp,
                                                                                 &sq_scratch,
                                                                                 pg_gpu_index_cache_ptr,
                                                                                 prepared_group,
                                                                                 cpu_prepared_target](
                                                                                    sequence_size_t
                                                                                        clip_lo_bp,
                                                                                    sequence_size_t
                                                                                        clip_hi_bp) {
                                                const auto cache_key = make_cache_key(
                                                    minimizers_per_sequence_index_name,
                                                    clip_lo_bp,
                                                    clip_hi_bp);
                                                const auto gpu_identity =
                                                    pg_gpu_index_identity(cache_key);
                                                if (prepared_group == nullptr)
                                                    if (auto hit = bioseq::storage::indexing::
                                                            lookup_assembled_index(cache_key)) {
                                                        return minimap_port_fragment_index_vector_pg_checked(
                                                            fq_pg_view,
                                                            ft_pg_view,
                                                            query_vec,
                                                            *hit->index,
                                                            idx_params,
                                                            pg_gpu_index_cache_ptr,
                                                            gpu_identity);
                                                    }
                                                if (prepared_group != nullptr &&
                                                    !prepared_group->target_index_permit
                                                         .has_value()) {
                                                    const std::uint64_t admission_bytes =
                                                        bioseq::parallelism::
                                                            minimizer_index_peak_bytes_for_bp(
                                                                static_cast<std::uint64_t>(
                                                                    total_bp));
                                                    auto permit =
                                                        bioseq::parallelism::
                                                            memory_admission_controller::instance()
                                                                .try_reserve(admission_bytes);
                                                    if (!permit) {
                                                        raise_typed_dispatch_pg_error_as_cpp(
                                                            ERRCODE_OUT_OF_MEMORY,
                                                            "bioseq batch join prepared persisted "
                                                            "target "
                                                            "index cannot be admitted");
                                                    }
                                                    prepared_group->target_index_permit.emplace(
                                                        std::move(*permit));
                                                }
                                                auto idx =
                                                    bioseq::indexing::direct_minimizer_bucket_index<
                                                        index_storage_type>::
                                                        assemble_for_query_segment(
                                                            seq_store,
                                                            query_span,
                                                            total_bp,
                                                            idx_params.max_occ,
                                                            clip_lo_bp,
                                                            clip_hi_bp,
                                                            sq_scratch);
                                                if (prepared_group != nullptr) {
                                                    if (prepared_group->target_index) {
                                                        ++prepared_group->duplicate_build_attempts;
                                                    } else {
                                                        prepared_group
                                                            ->target_index = std::make_shared<
                                                            bioseq::indexing::
                                                                minimizer_hashmap_fragment_index>(
                                                            std::move(idx));
                                                        prepared_group->target_index_source =
                                                            ::bioseq::pipeline_stats::
                                                                target_index_source::persisted;
                                                        ++prepared_group->target_index_builds;
                                                        prepared_group->build_set_checksum_present =
                                                            true;
                                                        prepared_group->build_set_checksum =
                                                            prepared_group->key_set_checksum;
                                                        ++cpu_prepared_target
                                                              ->physical_target_index_reads;
                                                    }
                                                    return minimap_port_fragment_index_vector_pg_checked(
                                                        fq_pg_view,
                                                        ft_pg_view,
                                                        query_vec,
                                                        *prepared_group->target_index,
                                                        idx_params,
                                                        /*gpu_index_cache=*/nullptr,
                                                        bioseq::indexing::gpu_index_cache_identity{
                                                            .assembly_identity =
                                                                prepared_group->key_set_checksum});
                                                }
                                                match_result_collection match_collection =
                                                    minimap_port_fragment_index_vector_pg_checked(
                                                        fq_pg_view,
                                                        ft_pg_view,
                                                        query_vec,
                                                        idx,
                                                        idx_params,
                                                        pg_gpu_index_cache_ptr,
                                                        gpu_identity);
                                                bioseq::storage::indexing::store_assembled_index(
                                                    cache_key, std::move(idx));
                                                return match_collection;
                                            };
                                            if (prepared_group != nullptr) {
                                                collection =
                                                    run_minimizer_sequence_index(0, total_bp);
                                            } else if (total_bp <=
                                                       postgres_segmentation_threshold_bp) {
                                                collection =
                                                    run_minimizer_sequence_index(0, total_bp);
                                            } else {
                                                match_result_collection merged_collection;
                                                std::vector<match_result>& merged =
                                                    merged_collection.results;
                                                const sequence_size_t query_len = query_bp_size;
                                                for (auto sr : pg_segment_unit_ranges(
                                                         total_bp, query_len, a_lpu, idx_params)) {
                                                    const sequence_size_t clip_lo_bp =
                                                        static_cast<sequence_size_t>(sr.cs_u) *
                                                        a_lpu;
                                                    const sequence_size_t clip_hi_bp = std::min(
                                                        static_cast<sequence_size_t>(sr.ce_u) *
                                                            a_lpu,
                                                        total_bp);
                                                    match_result_collection seg_res =
                                                        run_minimizer_sequence_index(clip_lo_bp,
                                                                                     clip_hi_bp);
                                                    merged_collection.combine_execution_stats(
                                                        seg_res);
                                                    for (auto& r : seg_res.results) {
                                                        if (r.target_start >= sr.core_lo_bp &&
                                                            r.target_start < sr.core_hi_bp) {
                                                            merged.push_back(std::move(r));
                                                        }
                                                    }
                                                }
                                                collection = std::move(merged_collection);
                                            }
                                            assembled = true;
                                        }
                                    }
                                });
                        } else {
                            auto frag_store = bioseq::storage::indexing::open_fragment_storage(
                                scanner_pg_ctx->user_relid,
                                scanner_pg_ctx->db_oid,
                                scanner_pg_ctx->tablespace_oid,
                                scanner_pg_ctx->main_relfilenode,
                                scanner_pg_ctx->mmsearch_seq_attno,
                                minimizers_per_fragment_index_name,
                                *loc_opt,
                                static_cast<std::uint64_t>(
                                    scanner_pg_ctx->target_rownos[row_index]),
                                scanner_pg_ctx->caches,
                                scanner_pg_ctx->parent_mctx);
                            with_checked_minimizer_fragment_reader(
                                frag_store, [&frag_store, &run_fragment_backed_index] {
                                    if (frag_store.number_of_chunks(
                                            bioseq::indexing::k_directory_sub_key) >= 1) {
                                        run_fragment_backed_index(
                                            frag_store, minimizers_per_fragment_index_name);
                                    }
                                });
                        }
                    }
                    if (assembled) {
                        scanner_pg_ctx->target_index_sources[row_index] =
                            ::bioseq::pipeline_stats::target_index_source::persisted;
                    }
                    if (persisted_required && !assembled) {
                        raise_required_persisted_index_error(
                            scanner_pg_ctx,
                            scanner_pg_ctx->target_rownos[row_index],
                            "the active persisted minimizer index has no compatible "
                            "directory/chunks for this target row");
                    }
                    // A recompute under an
                    // An active per-sequence read index with no persisted minimizer-sequence row
                    // must build a real transient minimizer-sequence index via the streaming GPU
                    // sorted-pair seam, not relabel an opportunistic minimizer-fragment index.
                    // The same invariant applies to both the
                    // asynchronous/deferred route and active-read recomputation.
                    if (!assembled && want_seq) {
                        collection = build_and_align_transient_minimizer_sequence(
                            fq_pg_view,
                            ft_pg_view,
                            params,
                            static_cast<sequence_size_t>(loc_opt->logical_length),
                            target_letters_per_unit);
                        assembled = true;
                        bioseq_log<logging_level::debug>(
                            "[bioseq] index: recompute real-minimizer-sequence-read-index row={} "
                            "target_bp={} backend={}",
                            static_cast<unsigned long long>(
                                scanner_pg_ctx->target_rownos[row_index]),
                            static_cast<unsigned long long>(loc_opt->logical_length),
                            bioseq::minimizers::minimizer_sketch_backend_name(
                                target_backend_from_params_for_recompute(params)));
                    }
                    if (!assembled && loc_opt->logical_length >= opportunistic_index_minimum_bp) {
                        const std::uint64_t peak_bytes =
                            bioseq::parallelism::minimizer_index_peak_bytes_for_bp(
                                static_cast<std::uint64_t>(loc_opt->logical_length));
                        auto build_permit =
                            bioseq::parallelism::memory_admission_controller::instance()
                                .try_reserve(peak_bytes);
                        if (build_permit) {
                            if (params.highway_kernels == nullptr) {
                                raise_typed_dispatch_pg_error_as_cpp(
                                    ERRCODE_INTERNAL_ERROR,
                                    "opportunistic minimizer-fragment build has no selected "
                                    "Highway minimizer table");
                            }
                            ensure_q();
                            opportunistic_fragment_index_storage ram_store;
                            bioseq::indexing::minimizers_per_fragment_index<
                                opportunistic_fragment_index_storage>::
                                build(ram_store,
                                      ft_pg_view,
                                      target_backend_from_params_for_recompute(params),
                                      &params.highway_kernels->minimizers,
                                      params.alphabet_code);
                            const std::string_view opportunistic_index_name =
                                want_seq ? minimizers_per_sequence_index_name
                                         : minimizers_per_fragment_index_name;

                            bioseq_log<logging_level::debug>(
                                "[bioseq] index: opportunistic RAM-first index row={} index={} "
                                "target_bp={} chunks={} peak_bytes={}",
                                static_cast<unsigned long long>(
                                    scanner_pg_ctx->target_rownos[row_index]),
                                opportunistic_index_name,
                                static_cast<unsigned long long>(loc_opt->logical_length),
                                ram_store.chunks.size(),
                                static_cast<unsigned long long>(peak_bytes));

                            const sequence_size_t total_bp = loc_opt->logical_length;
                            const std::size_t a_lpu = target_letters_per_unit;
                            constexpr sequence_size_t frag_bp = static_cast<sequence_size_t>(
                                bioseq::storage::postgres_lazy_sequence_binding<
                                    p_sequence>::fragment_bp_capacity());
                            static_assert(frag_bp != 0u);
                            const std::size_t size_u =
                                static_cast<std::size_t>((total_bp + a_lpu - 1) / a_lpu);
                            bioseq::indexing::minimizer_assembly_scratch scratch;
                            auto assemble_ram_one =
                                [a_lpu, total_bp, frag_bp, &ram_store, &scratch, &query_minimizers](
                                    std::size_t cs_u, std::size_t ce_u) {
                                    const sequence_size_t cs_bp =
                                        static_cast<sequence_size_t>(cs_u) * a_lpu;
                                    const sequence_size_t ce_bp = std::min(
                                        static_cast<sequence_size_t>(ce_u) * a_lpu, total_bp);
                                    const std::size_t frag_lo =
                                        static_cast<std::size_t>(cs_bp / frag_bp);
                                    const std::size_t frag_hi = static_cast<std::size_t>(
                                        bioseq::common::ceil_div(ce_bp, frag_bp));
                                    const auto query_span = std::span<const minimap_sketch_data>(
                                        query_minimizers->data(), query_minimizers->size());
                                    return bioseq::indexing::minimizers_per_fragment_index<
                                        opportunistic_fragment_index_storage>::
                                        assemble_segment_for_query(ram_store,
                                                                   query_span,
                                                                   frag_lo,
                                                                   frag_hi,
                                                                   frag_bp,
                                                                   total_bp,
                                                                   cs_bp,
                                                                   ce_bp,
                                                                   scratch);
                                };
                            const bool assembled_cache_allowed =
                                scanner_pg_ctx->read_index_active &&
                                scanner_pg_ctx->index_read_requirement !=
                                    bioseq::indexing::index_read_requirement::require_absent;
                            auto run_with_ram_index =
                                [&make_cache_key,
                                 &assemble_ram_one,
                                 fq_pg_view,
                                 ft_pg_view,
                                 &query_minimizers,
                                 &idx_params,
                                 opportunistic_index_name,
                                 pg_gpu_index_cache_ptr,
                                 assembled_cache_allowed](sequence_size_t clip_lo_bp,
                                                          sequence_size_t clip_hi_bp,
                                                          std::size_t cs_u,
                                                          std::size_t ce_u) {
                                    const auto cache_key = make_cache_key(
                                        opportunistic_index_name, clip_lo_bp, clip_hi_bp);
                                    const auto gpu_identity = pg_gpu_index_identity(cache_key);
                                    if (assembled_cache_allowed) {
                                        if (auto hit =
                                                bioseq::storage::indexing::lookup_assembled_index(
                                                    cache_key)) {
                                            return minimap_port_fragment_index_vector_pg_checked(
                                                fq_pg_view,
                                                ft_pg_view,
                                                *query_minimizers,
                                                *hit->index,
                                                idx_params,
                                                pg_gpu_index_cache_ptr,
                                                gpu_identity);
                                        }
                                    }
                                    auto idx = assemble_ram_one(cs_u, ce_u);
                                    match_result_collection match_collection =
                                        minimap_port_fragment_index_vector_pg_checked(
                                            fq_pg_view,
                                            ft_pg_view,
                                            *query_minimizers,
                                            idx,
                                            idx_params,
                                            pg_gpu_index_cache_ptr,
                                            gpu_identity);
                                    if (assembled_cache_allowed) {
                                        bioseq::storage::indexing::store_assembled_index(
                                            cache_key, std::move(idx));
                                    }
                                    return match_collection;
                                };
                            if (total_bp <= postgres_segmentation_threshold_bp) {
                                collection = run_with_ram_index(0, total_bp, 0, size_u);
                            } else {
                                match_result_collection merged_collection;
                                std::vector<match_result>& merged = merged_collection.results;
                                const sequence_size_t query_len = query_bp_size;
                                for (auto sr : pg_segment_unit_ranges(
                                         total_bp, query_len, a_lpu, idx_params)) {
                                    const sequence_size_t clip_lo_bp =
                                        static_cast<sequence_size_t>(sr.cs_u) * a_lpu;
                                    const sequence_size_t clip_hi_bp = std::min(
                                        static_cast<sequence_size_t>(sr.ce_u) * a_lpu, total_bp);
                                    match_result_collection seg_res = run_with_ram_index(
                                        clip_lo_bp, clip_hi_bp, sr.cs_u, sr.ce_u);
                                    merged_collection.combine_execution_stats(seg_res);
                                    for (auto& r : seg_res.results) {
                                        if (r.target_start >= sr.core_lo_bp &&
                                            r.target_start < sr.core_hi_bp) {
                                            merged.push_back(std::move(r));
                                        }
                                    }
                                }
                                collection = std::move(merged_collection);
                            }
                            assembled = true;
                        } else {
                            bioseq_log<logging_level::debug>(
                                "[bioseq] index: opportunistic RAM-first index skipped row={} "
                                "target_bp={} peak_bytes={} (not admitted)",
                                static_cast<unsigned long long>(
                                    scanner_pg_ctx->target_rownos[row_index]),
                                static_cast<unsigned long long>(loc_opt->logical_length),
                                static_cast<unsigned long long>(peak_bytes));
                        }
                    }
                }
                // Normal allow/no-index mode may recompute without manufacturing or consuming
                // a scheme-less minimizer-sequence representation. The transient builder is
                // available only for a supported stored minimizer scheme.
                const bool want_seq_noindex =
                    scanner_pg_ctx->active_index == minimizers_per_sequence_index_name &&
                    stored_minimizer_scheme_compatible;
                if (!assembled && want_seq_noindex) {
                    const auto rowno =
                        static_cast<std::uint64_t>(scanner_pg_ctx->target_rownos[row_index]);
                    // No-index minimizer-sequence must use a real transient
                    // minimizers_per_sequence index even when
                    // index.recompute_async_persistence.enabled is off. Reads are
                    // disabled here, but the active policy still
                    // selects the algorithm shape.
                    collection = build_and_align_transient_minimizer_sequence(
                        fq_pg_view,
                        ft_pg_view,
                        params,
                        static_cast<sequence_size_t>(loc_opt->logical_length),
                        target_letters_per_unit);
                    // build_and_align_transient_minimizer_sequence always builds and queries the
                    // persisted minimizer-sequence scheme. Pass that concrete scheme to staging
                    // even when the short-query heuristic changed the outer alignment pair;
                    // ordinary streaming callers retain their requested k/w and still
                    // fail closed in can_defer_recompute_minimizer_persist().
                    minimap2_parameters minimizer_sequence_stage_params = params;
                    minimizer_sequence_stage_params.kmer_size = 15;
                    minimizer_sequence_stage_params.window_size = 10;
                    const bool async_staged = stage_recompute_minimizer_sequence_build_if_selected(
                        *scanner_pg_ctx,
                        *loc_opt,
                        rowno,
                        alphabet_code,
                        storage_unit_byte_width,
                        static_cast<std::uint16_t>(_padding),
                        minimizer_sequence_stage_params,
                        "real_minimizer_sequence_active_policy_noindex");
                    bioseq_log<logging_level::debug>(
                        "[bioseq] index: recompute real-minimizer-sequence-noindex-policy row={} "
                        "target_bp={} async_staged={} persisted=false backend={}",
                        static_cast<unsigned long long>(rowno),
                        static_cast<unsigned long long>(loc_opt->logical_length),
                        async_staged,
                        bioseq::minimizers::minimizer_sketch_backend_name(
                            target_backend_from_params_for_recompute(params)));
                    assembled = true;
                }
                if (!assembled) {
                    const bool can_defer_fast_minimizer_sequence_recompute =
                        can_defer_recompute_minimizer_persist(*scanner_pg_ctx, params);
                    const auto fast_minimizer_sequence_choice =
                        can_defer_fast_minimizer_sequence_recompute
                            ? choose_deferred_minimizer_persist_index(*scanner_pg_ctx)
                            : std::optional<deferred_minimizer_persist_choice>{};
                    if (fast_minimizer_sequence_choice.has_value() &&
                        fast_minimizer_sequence_choice->kind ==
                            deferred_minimizer_index_kind::per_sequence) {
                        const auto rowno =
                            static_cast<std::uint64_t>(scanner_pg_ctx->target_rownos[row_index]);
                        // Build a real transient minimizer-sequence index for the
                        // no-index/per-sequence cell. The GPU sorted-pair seam (GPU emit + host
                        // sort) streams from lazy PostgreSQL (no whole-target
                        // resident materialization, so 2/4 Gbp targets do not OOM).
                        collection = build_and_align_transient_minimizer_sequence(
                            fq_pg_view,
                            ft_pg_view,
                            params,
                            static_cast<sequence_size_t>(loc_opt->logical_length),
                            target_letters_per_unit);
                        const bool async_staged =
                            stage_recompute_minimizer_sequence_build_if_selected(
                                *scanner_pg_ctx,
                                *loc_opt,
                                rowno,
                                alphabet_code,
                                storage_unit_byte_width,
                                static_cast<std::uint16_t>(_padding),
                                params,
                                "real_minimizer_sequence_fast_noindex");
                        bioseq_log<logging_level::debug>(
                            "[bioseq] index: recompute real-minimizer-sequence-fast-noindex row={} "
                            "index={} target_bp={} async_staged={} persisted=false backend={}",
                            static_cast<unsigned long long>(rowno),
                            fast_minimizer_sequence_choice->index_name,
                            static_cast<unsigned long long>(loc_opt->logical_length),
                            async_staged,
                            bioseq::minimizers::minimizer_sketch_backend_name(
                                target_backend_from_params_for_recompute(params)));
                        assembled = true;
                    }
                }
                if (!assembled) {
                    const bool try_resident_segment_workers =
                        target_bp_size > postgres_segmentation_threshold_bp &&
                        !dispatch_params.enable_profiling &&
                        bioseq::parallelism::has_parallel_tasks(pool.get_mode()) &&
                        pool.maximum_concurrency() > 1;
                    if (try_resident_segment_workers) {
                        auto resident = try_materialize_target_for_segment_workers(
                            ft_pg_view,
                            storage_unit_byte_width,
                            static_cast<std::uint64_t>(scanner_pg_ctx->target_rownos[row_index]));
                        if (resident.has_value()) {
                            const auto resident_target_view = resident->owner.view();
                            const bool can_defer_recompute_persist =
                                can_defer_recompute_minimizer_persist(*scanner_pg_ctx, params);
                            const auto deferred_persist_choice =
                                can_defer_recompute_persist
                                    ? choose_deferred_minimizer_persist_index(*scanner_pg_ctx)
                                    : std::optional<deferred_minimizer_persist_choice>{};
                            if (deferred_persist_choice.has_value()) {
                                const auto rowno = static_cast<std::uint64_t>(
                                    scanner_pg_ctx->target_rownos[row_index]);
                                collection = run_recompute_with_deferred_minimizer_persist(
                                    lazy_query_view,
                                    resident_target_view,
                                    params,
                                    *scanner_pg_ctx,
                                    *loc_opt,
                                    rowno,
                                    alphabet_code,
                                    storage_unit_byte_width,
                                    static_cast<std::uint16_t>(_padding),
                                    *deferred_persist_choice);
                            } else {
                                collection = minimap_port_segmented_pg_checked(
                                    lazy_query_view, resident_target_view, params);
                                if (scanner_pg_ctx->recompute_async_persist_active &&
                                    (params.kmer_size != 15 || params.window_size != 10)) {
                                    bioseq_log<logging_level::debug>(
                                        "[bioseq] index: recompute async-persist skipped row={} "
                                        "target_bp={} because active params are k={} w={} but "
                                        "persisted minimizer indexes are pinned to k=15 w=10",
                                        static_cast<unsigned long long>(
                                            scanner_pg_ctx->target_rownos[row_index]),
                                        static_cast<unsigned long long>(loc_opt->logical_length),
                                        params.kmer_size,
                                        params.window_size);
                                } else if (can_defer_recompute_persist) {
                                    bioseq_log<logging_level::debug>(
                                        "[bioseq] index: recompute async-persist skipped row={} "
                                        "target_bp={} because no minimizer index is enabled for "
                                        "UPDATE_ON_INSERT",
                                        static_cast<unsigned long long>(
                                            scanner_pg_ctx->target_rownos[row_index]),
                                        static_cast<unsigned long long>(loc_opt->logical_length));
                                }
                            }
                        } else {
                            collection =
                                minimap_port_segmented_pg_checked(fq_pg_view, ft_pg_view, params);
                            stage_recompute_minimizer_sequence_build_if_selected(
                                *scanner_pg_ctx,
                                *loc_opt,
                                static_cast<std::uint64_t>(
                                    scanner_pg_ctx->target_rownos[row_index]),
                                alphabet_code,
                                storage_unit_byte_width,
                                static_cast<std::uint16_t>(_padding),
                                params,
                                "resident_materialize_failed");
                        }
                    } else {
                        collection =
                            (target_bp_size > postgres_segmentation_threshold_bp)
                                ? minimap_port_segmented_pg_checked(fq_pg_view, ft_pg_view, params)
                                : minimap_port_raw_pg_checked(fq_pg_view, ft_pg_view, params);
                        stage_recompute_minimizer_sequence_build_if_selected(
                            *scanner_pg_ctx,
                            *loc_opt,
                            static_cast<std::uint64_t>(scanner_pg_ctx->target_rownos[row_index]),
                            alphabet_code,
                            storage_unit_byte_width,
                            static_cast<std::uint16_t>(_padding),
                            params,
                            "streaming_recompute");
                    }
                }
            });
    } else {
        compact_sequence_range<p_sequence> target_seq{*target_hdr};
        const auto target_view = bioseq::storage::make_lazy_sequence_view(target_seq);
        collection = (target_bp_size > postgres_segmentation_threshold_bp)
                         ? minimap_port_segmented_pg_checked(lazy_query_view, target_view, params)
                         : minimap_port_raw_pg_checked(lazy_query_view, target_view, params);
    }

    if (query_sketch_host_wall_ns != 0u) {
        collection.record_query_sketch_host_time(query_sketch_host_wall_ns);
    }
    if (row_consumed_persisted_query_sketch) {
        const pipe::minimizer_range_execution_stats* completed_query_stats =
            retained_prepared_query_sketch != nullptr
                ? &retained_prepared_query_sketch->execution_stats
            : per_row_query_mode ? row_query_minimizer_stats ? &*row_query_minimizer_stats : nullptr
            : cached_query_minimizer_stats ? &*cached_query_minimizer_stats
                                           : nullptr;
        if (completed_query_stats == nullptr) {
            raise_typed_dispatch_pg_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "consumed persisted query minimizers have no operation stats");
        }
        const bool exact_query_stats_absent =
            !collection.exact_simd_query_execution_stats_available &&
            collection.exact_simd_query_execution_stats ==
                decltype(collection.exact_simd_query_execution_stats){};
        const bool scalar_query_stats_absent =
            collection.scalar_pipeline_stats.query_minimizers ==
            decltype(collection.scalar_pipeline_stats.query_minimizers){};
        const bool bounded_query_stats_absent =
            !collection.bounded_nthash_query_execution_stats_available &&
            collection.bounded_nthash_query_execution_stats ==
                decltype(collection.bounded_nthash_query_execution_stats){};
        if (!scalar_query_stats_absent || !exact_query_stats_absent ||
            !bounded_query_stats_absent) {
            raise_typed_dispatch_pg_error_as_cpp(
                ERRCODE_INTERNAL_ERROR,
                "persisted query sketch stats were recorded more than once");
        }
        if (completed_query_stats->exact_simd_available) {
            collection.record_exact_simd_query_execution_stats(completed_query_stats->exact_simd);
        }
        if (completed_query_stats->bounded_nthash_available) {
            collection.record_bounded_nthash_query_execution_stats(
                completed_query_stats->bounded_nthash);
        }
        if (completed_query_stats->scalar_available) {
            collection.record_scalar_query_minimizer_execution_stats(completed_query_stats->scalar);
        }
        consumed_shared_cached_query_stats = !per_row_query_mode;
    }
    return collection;
}

} // namespace bioseq::simd::typed_dispatch::typed_support_detail
