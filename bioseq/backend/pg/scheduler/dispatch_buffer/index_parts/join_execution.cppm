/**
 * @file index_parts/join_execution.cppm
 * @brief Defines join-wave execution, normalization, cleanup, and rescan recovery.
 */
module;

#include <bioseq/pg_compat.h>
extern "C" {
#include "executor/instrument.h"
#include "utils/snapmgr.h"
}

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// The chunked-scanner dispatch path is always compiled as a permanent part of
// the fragmented pipeline tree.
// The `bioseq.testing.chunked_scanner.enabled` GUC routes row sequences through a
// synthetic chunked scanner end-to-end. The
// `bioseq.testing.in_memory_scanner.enabled` GUC forces the materialise-then-
// in_memory baseline. Without either GUC the production path promotes locator
// values through the statement-owned source provider and runs from its
// canonical lazy facade.
module bioseq.postgres.scheduler.dispatch_buffer:join_execution;

import bioseq.postgres.scheduler.dispatch_buffer;

import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.postgres.support;
import bioseq.postgres.support.datums;
import bioseq.postgres.configuration.core;
import bioseq.indexing.type;
import bioseq.postgres.storage.indexing.handles;
import bioseq.postgres.storage.indexing.chunked_storage;
import bioseq.storage.lazy_sequence; // allocation observer for route-aware sequence payload tracing
import bioseq.postgres.scheduler.dispatch_processor; // fixed-shape batch_processor, heavy pipeline in its own module
import bioseq.sequences;
import bioseq.sequences.simd;
import bioseq.pipeline.minimap_port;
import bioseq.sequences.cigar;
import bioseq.pipeline.pipe;
import bioseq.postgres.types.compact_sequence;
import bioseq.postgres.storage.manager;
import bioseq.storage.scanner;
import bioseq.storage.layout;
import bioseq.postgres.storage.scanner; // statement-owned source provider for locator targets
import bioseq.parallelism.memory_admission;
import bioseq.postgres.scheduler.supervisor;
import bioseq.postgres.column.table_access;

namespace bioseq::scheduler {

/** @brief Implements the run join query batch and build cursors contract declared by the stable
 * buffer facade. */
[[nodiscard]] bool tuple_dispatch_buffer::run_join_query_batch_and_build_cursors() {
    if (!join_mode || !join_target_prepared || !join_target_active) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq batch join query batch ran before target preparation")));
    }
    if (join_queries.empty()) {
        last_stats = ::bioseq::pipeline_stats::pipeline_execution_stats{};
        join_output_cursors.clear();
        join_output_position = 0;
        return false;
    }
    if (join_queries.size() > dispatch_batch_capacity || buffered_entries.size() != 1) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq batch join dispatch shape is invalid")));
    }

    bioseq::postgres::scheduler::backend_sync();
    last_stats = ::bioseq::pipeline_stats::pipeline_execution_stats{};
    const std::size_t batch_size = join_queries.size();
    join_target_hdrs.fill(nullptr);
    join_query_hdrs.fill(nullptr);
    join_query_ids.fill(0);
    join_query_detoast_copies.fill(nullptr);
    join_query_rewidth_varlenas.clear();
    join_query_rewidth_varlenas.resize(batch_size);
    join_result_scratch.clear();
    if (!join_scanner_pg_ctx) {
        join_scanner_pg_ctx =
            std::make_unique<::bioseq::simd::batch_processor::scanner_pg_context>();
    }
    *join_scanner_pg_ctx = ::bioseq::simd::batch_processor::scanner_pg_context{};
    auto& scanner_pg_ctx = *join_scanner_pg_ctx;
    const auto ticket =
        bioseq::postgres::scheduler::begin_batch(static_cast<std::int64_t>(batch_size));
    auto prepared_counter_totals = [this] {
        std::array<std::uint64_t, 5> totals{};
        for (const auto& group : join_cpu_prepared_target.groups) {
            totals[0] += group.target_index_builds;
            totals[1] += group.target_index_reuses;
            totals[2] += group.duplicate_build_attempts;
            totals[3] += group.persisted_reader_loads;
            totals[4] += group.persisted_reader_queries;
        }
        return totals;
    };
    const auto prepared_before = prepared_counter_totals();
    join_scanner_pg_cache = nullptr;
    MemoryContext saved_context = CurrentMemoryContext;
    ErrorData* volatile original_error = nullptr;
    std::exception_ptr cpp_error;
    PG_TRY();
    {
        try {
            buffered_scan_entry& target = buffered_entries.front();
            for (std::size_t query_index = 0; query_index < batch_size; ++query_index) {
                const join_query_ref& query = join_queries[query_index];
                if (query.query_slot == nullptr ||
                    query.ordinal >
                        static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)())) {
                    ereport(ERROR,
                            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                             errmsg("bioseq batch join query reference is invalid")));
                }
                bool query_is_null = true;
                const Datum query_datum =
                    slot_getattr(query.query_slot, join_query_slot_seq_resno, &query_is_null);
                if (query_is_null) {
                    ereport(ERROR,
                            (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                             errmsg("bioseq batch join query sequence is NULL")));
                }
                if (VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(query_datum))) {
                    const pg_sequence_descriptor descriptor =
                        classify_and_describe_sequence(query_datum);
                    require_recipe_descriptor_(descriptor,
                                               mmsearch_state.query_alphabet_code,
                                               mmsearch_state.query_storage_unit_byte_width,
                                               "query");
                    join_query_hdrs[query_index] = nullptr;
                    scanner_pg_ctx.query_recipe_datums[query_index] = query_datum;
                    scanner_pg_ctx.query_recipe_descriptors[query_index] = descriptor;
                    join_target_hdrs[query_index] = target.seq_hdr();
                    scanner_pg_ctx.target_recipe_datums[query_index] = target.seq_recipe_datum;
                    scanner_pg_ctx.target_recipe_descriptors[query_index] =
                        target.seq_recipe_descriptor;
                    join_query_ids[query_index] = static_cast<std::int32_t>(query.ordinal);
                    continue;
                }
                bioseq::pg_types::seq* query_hdr =
                    reinterpret_cast<bioseq::pg_types::seq*>(PG_DETOAST_DATUM(query_datum));
                if (bioseq::storage::is_locator_stub(query_hdr)) {
                    ereport(ERROR,
                            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                             errmsg("bioseq batch join query child returned a locator stub"),
                             errhint("The bounded query child must materialize its compact "
                                     "sequence before join dispatch.")));
                }
                if (query_hdr->alphabet_code != mmsearch_state.query_alphabet_code ||
                    query_hdr->storage_unit_byte_width !=
                        mmsearch_state.query_storage_unit_byte_width) {
                    ereport(ERROR,
                            (errcode(ERRCODE_DATATYPE_MISMATCH),
                             errmsg("bioseq batch join query typmod changed after planning")));
                }
                if (reinterpret_cast<void*>(query_hdr) !=
                    reinterpret_cast<void*>(DatumGetPointer(query_datum))) {
                    join_query_detoast_copies[query_index] = query_hdr;
                }
                const bioseq::pg_types::seq* dispatch_query_hdr = query_hdr;
                if (query_hdr->storage_unit_byte_width != mmsearch_state.storage_unit_byte_width) {
                    bioseq::pg_types::seq* rewidth = ::bioseq::simd::stdc_rewidth_to_type_params(
                        *query_hdr,
                        compact_sequence_stdc_type_params{mmsearch_state.alphabet_code,
                                                          mmsearch_state.storage_unit_byte_width},
                        pg_alloc);
                    if (rewidth == nullptr) {
                        ereport(ERROR,
                                (errcode(ERRCODE_DATATYPE_MISMATCH),
                                 errmsg("bioseq batch join query rewidth rejected an incompatible "
                                        "alphabet")));
                    }
                    const std::size_t rewidth_bytes = static_cast<std::size_t>(VARSIZE(rewidth));
                    join_query_rewidth_varlenas[query_index].assign(
                        reinterpret_cast<const char*>(rewidth),
                        reinterpret_cast<const char*>(rewidth) + rewidth_bytes);
                    pfree(rewidth);
                    dispatch_query_hdr = reinterpret_cast<const bioseq::pg_types::seq*>(
                        join_query_rewidth_varlenas[query_index].data());
                }
                join_query_hdrs[query_index] = dispatch_query_hdr;
                join_target_hdrs[query_index] = target.seq_hdr();
                scanner_pg_ctx.target_recipe_datums[query_index] = target.seq_recipe_datum;
                scanner_pg_ctx.target_recipe_descriptors[query_index] =
                    target.seq_recipe_descriptor;
                join_query_ids[query_index] = static_cast<std::int32_t>(query.ordinal);
            }

            if (batch_payload_ctx == nullptr) {
                batch_payload_ctx =
                    AllocSetContextCreate(CurrentMemoryContext,
                                          "tuple_dispatch_buffer join payload arena",
                                          ALLOCSET_DEFAULT_SIZES);
            }
            join_scanner_pg_cache = bioseq::storage::alloc_seq_caches(batch_payload_ctx);
            join_scanner_pg_cache->side = bioseq::storage::seq_caches::access_side::read;
            join_scanner_pg_cache->lock_mode = AccessShareLock;
            join_scanner_pg_cache->read_state_mctx = batch_payload_ctx;
            join_scanner_pg_cache->main_database_oid = user_main_database_oid;
            join_scanner_pg_cache->main_tablespace_oid = user_main_tablespace_oid;
            join_scanner_pg_cache->main_relfilenode = user_main_relfilenode;

            scanner_pg_ctx.async_job_observer =
                make_slice_async_pool_observer_noexcept(slice_async_owner);
            scanner_pg_ctx.mmsearch_target_payload_observer = mmsearch_target_payload_observer;
            scanner_pg_ctx.mmsearch_query_payload_observer = mmsearch_query_payload_observer;
            scanner_pg_ctx.db_oid = user_main_database_oid;
            scanner_pg_ctx.tablespace_oid = user_main_tablespace_oid;
            scanner_pg_ctx.main_relfilenode = user_main_relfilenode;
            scanner_pg_ctx.mmsearch_seq_attno =
                static_cast<std::int16_t>(join_target_storage_seq_attno);
            scanner_pg_ctx.mmsearch_query_attno =
                static_cast<std::int16_t>(join_query_slot_seq_resno);
            scanner_pg_ctx.query_alphabet_code = mmsearch_state.query_alphabet_code;
            scanner_pg_ctx.query_storage_unit_byte_width =
                mmsearch_state.query_storage_unit_byte_width;
            scanner_pg_ctx.caches = join_scanner_pg_cache;
            scanner_pg_ctx.parent_mctx = batch_payload_ctx;
            scanner_pg_ctx.snapshot = scan_snapshot;
            scanner_pg_ctx.source_provider = source_provider;
            scanner_pg_ctx.persisted_gpu_operation_stats_sink = persisted_gpu_operation_stats_sink;
            scanner_pg_ctx.user_relid = user_relid;
            scanner_pg_ctx.index_read_requirement = index_read_requirement;
            scanner_pg_ctx.read_index_active = read_sequence_index_active;
            scanner_pg_ctx.index_async_persist_active =
                bioseq::settings::index::async_persistence::enabled;
            scanner_pg_ctx.recompute_async_persist_active =
                bioseq::settings::index::recompute_async_persistence::enabled;
            if (!join_active_read_policy.has_value()) {
                ereport(ERROR,
                        (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                         errmsg("bioseq batch join active-read policy stats are missing")));
            }
            apply_active_read_policy_resolution(
                scanner_pg_ctx,
                *join_active_read_policy,
                user_relid,
                user_main_relfilenode,
                static_cast<std::int16_t>(join_target_storage_seq_attno));
            const bool record_physical_target = !join_target_outcomes_recorded;
            scanner_pg_ctx.explicit_query_ordinals = true;
            /* Ask the generic processor for its native raw stats on every
             * wave. CPU reports one target/source observation per pair; GPU
             * reports its already-deduplicated logical target identity. The
             * batch join owner validates that backend-specific shape above and
             * alone decides whether this wave publishes the physical target. */
            scanner_pg_ctx.shared_target_batch = false;
            scanner_pg_ctx.record_target_operational_outcomes = true;
            if (join_cpu_preparation_pending) {
                if (join_pending_cpu_preparation_queries.empty() ||
                    join_cpu_prepared_target.unions_ready) {
                    ereport(
                        ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg(
                             "bioseq batch join deferred CPU preparation state is inconsistent")));
                }
                scanner_pg_ctx.highway_preparation = {
                    .state = this,
                    .function =
                        &tuple_dispatch_buffer::prepare_join_cpu_after_highway_selection_thunk,
                };
            } else {
                scanner_pg_ctx.cpu_prepared_target =
                    join_cpu_prepared_target.unions_ready ? &join_cpu_prepared_target : nullptr;
            }
            scanner_pg_ctx.gpu_prepared_grouped_soa_target =
                join_gpu_prepared_grouped_soa_target.ready() ? &join_gpu_prepared_grouped_soa_target
                                                             : nullptr;
            for (std::size_t query_index = 0; query_index < batch_size; ++query_index) {
                scanner_pg_ctx.target_locators[query_index] = target.seq_loc;
                scanner_pg_ctx.target_rownos[query_index] = target.sequence_rowno;
                scanner_pg_ctx.query_ordinals[query_index] = join_queries[query_index].ordinal;
            }
            const bool wave_has_recipe_or_locator =
                join_cpu_preparation_pending || target.seq_loc.has_value() ||
                target.has_seq_recipe() ||
                std::ranges::any_of(scanner_pg_ctx.query_recipe_datums.begin(),
                                    scanner_pg_ctx.query_recipe_datums.begin() + batch_size,
                                    [](Datum datum) noexcept { return datum != Datum{0}; });
            if (wave_has_recipe_or_locator) {
                if (active_wave_read_session != nullptr) {
                    ereport(ERROR,
                            (errcode(ERRCODE_INTERNAL_ERROR),
                             errmsg("bioseq batch join retained a stale dispatch session")));
                }
                active_wave_read_session = ::begin_pg_sequence_read_session(batch_payload_ctx);
                scanner_pg_ctx.read_session = active_wave_read_session;
            }

            join_result_scratch =
                processor.process_sequences(batch_size,
                                            mmsearch_state.alphabet_code,
                                            mmsearch_state.storage_unit_byte_width,
                                            nullptr,
                                            &join_query_hdrs,
                                            join_target_hdrs,
                                            join_query_ids,
                                            base_params,
                                            ::bioseq::simd::batch_invocation_role::primary,
                                            0,
                                            last_stats,
                                            &scanner_pg_ctx);
            if (active_wave_read_session != nullptr) {
                ::finish_pg_sequence_read_session(active_wave_read_session, true);
                active_wave_read_session = nullptr;
                scanner_pg_ctx.read_session = nullptr;
            }
            if (join_cpu_prepared_target.unions_ready &&
                !join_cpu_prepared_target.query_set_validation_ready()) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("bioseq batch join compact-direct query-set stats remained pending "
                                "after dispatch")));
            }
            normalize_shared_join_target_outcomes(
                last_stats,
                record_physical_target,
                batch_size,
                classify_shared_join_target_outcome_topology(
                    ::bioseq::simd::pipe::vulkan_components_requested(base_params),
                    ::bioseq::simd::pipe::complete_all_gpu_tuple_selected(base_params)));
            const auto prepared_after = prepared_counter_totals();
            if (prepared_after[0] < prepared_before[0] || prepared_after[1] < prepared_before[1] ||
                prepared_after[2] < prepared_before[2] || prepared_after[3] < prepared_before[3] ||
                prepared_after[4] < prepared_before[4] ||
                join_cpu_prepared_target.physical_target_index_reads !=
                    prepared_after[0] + prepared_after[3]) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("bioseq batch join prepared-target counters are inconsistent")));
            }
            auto& operational = last_stats.operational;
            if (!join_cpu_prepared_target.query_union_prepare_host_wall_published &&
                join_cpu_prepared_target.query_union_prepare_host_wall_ns != 0u) {
                const std::size_t query_sketch_stage = ::bioseq::pipeline_stats::stage_index(
                    ::bioseq::pipeline_stats::pipeline_stage::query_sketch);
                operational.are_stats_saturated =
                    ::bioseq::pipeline_stats::merge_counter(
                        operational.timing[query_sketch_stage].host_wall_ns,
                        join_cpu_prepared_target.query_union_prepare_host_wall_ns) ||
                    operational.are_stats_saturated;
                join_cpu_prepared_target.query_union_prepare_host_wall_published = true;
            }
            ::bioseq::pipeline_stats::pipeline_operational_stats prepared_delta{};
            prepared_delta.cpu_prepared_index_builds = prepared_after[0] - prepared_before[0];
            prepared_delta.cpu_prepared_index_reuses = prepared_after[1] - prepared_before[1];
            prepared_delta.cpu_prepared_duplicate_build_attempts =
                prepared_after[2] - prepared_before[2];
            operational.merge(prepared_delta);
            if (!join_cpu_prepared_target.persisted_read_stats_published &&
                !join_gpu_prepared_grouped_soa_target.ready()) {
                merge_cold_index_preparation_stats(operational, join_cold_policy_stats);
                for (const auto& group : join_cpu_prepared_target.groups) {
                    if (!group.persisted_fragment_reader && !group.persisted_grouped_soa_reader &&
                        !group.persisted_hash_major_reader)
                        continue;
                    const auto& read = group.persisted_read_stats;
                    ::bioseq::pipeline_stats::pipeline_operational_stats read_delta{};
                    read_delta.persisted_generation_read_sessions = 1;
                    read_delta.persisted_relation_opens = read.relation_opens;
                    read_delta.persisted_index_scans = read.index_scans;
                    read_delta.persisted_head_scans = read.head_scans;
                    read_delta.persisted_head_rows_read = read.head_rows_read;
                    read_delta.persisted_head_bytes_read = read.head_bytes_read;
                    read_delta.persisted_manifest_directory_scans = read.manifest_directory_scans;
                    read_delta.persisted_manifest_rows_read = read.manifest_rows_read;
                    read_delta.persisted_manifest_bytes_read = read.manifest_bytes_read;
                    read_delta.persisted_directory_chunks_read = read.directory_chunks_read;
                    read_delta.persisted_directory_bytes_read = read.directory_bytes_read;
                    read_delta.persisted_metadata_repeated_reads = read.metadata_repeated_reads;
                    read_delta.persisted_metadata_heap_rows_read = read.metadata_heap_rows_read;
                    read_delta.persisted_metadata_bytes_read = read.metadata_bytes_read;
                    read_delta.persisted_payload_heap_rows_read = read.payload_heap_rows_read;
                    read_delta.persisted_payload_bytes_read = read.payload_bytes_read;
                    read_delta.persisted_heap_rows_read = read.total_heap_rows_read;
                    read_delta.persisted_toast_values_read = read.toast_values_read;
                    read_delta.persisted_detoast_bytes = read.detoast_bytes;
                    read_delta.persisted_explicit_copy_bytes = read.explicit_copy_bytes;
                    read_delta.persisted_wanted_subkeys = read.payload.wanted_subkeys;
                    read_delta.persisted_read_subkeys = read.payload.read_subkeys;
                    read_delta.persisted_wanted_extents = read.payload.wanted_extents;
                    read_delta.persisted_wanted_chunks = read.payload.wanted_chunks;
                    read_delta.persisted_skipped_rows = read.payload.skipped_rows;
                    read_delta.persisted_repeated_chunk_reads = read.payload.repeated_chunk_reads;
                    read_delta.persisted_extents_read = read.payload.read_extents;
                    read_delta.persisted_chunks_read = read.payload.delivered_chunks;
                    read_delta.persisted_bytes_read = read.payload.delivered_bytes;
                    read_delta.persisted_bulk_read_calls = read.bulk_read_calls;
                    read_delta.persisted_planned_request_groups = read.planned_request_groups;
                    read_delta.persisted_executed_request_groups = read.executed_request_groups;
                    read_delta.persisted_range_scan_calls = read.range_scan_calls;
                    read_delta.persisted_skip_budget_rows = read.payload.skip_budget_rows;
                    read_delta.persisted_source_parent_rows_read = read.source_parent_rows_read;
                    read_delta.persisted_source_catalog_rows_read = read.source_catalog_rows_read;
                    read_delta.persisted_source_catalog_index_scans =
                        read.source_catalog_index_scans;
                    read_delta.persisted_source_fragment_rows_read = read.source_fragment_rows_read;
                    read_delta.persisted_source_decoded_bp = read.source_decoded_bp;
                    map_hash_major_index_reader_stats(read_delta, read);
                    const auto record_grouped_soa_reader_memory_stats = [&read_delta](
                                                                            const auto& reader) {
                        const auto memory_stats = reader.memory_stats();
                        const std::uint64_t exact_position_bytes = bioseq::common::saturating_mul(
                            memory_stats.retained_position_count, sizeof(std::uint64_t));
                        const bool exact_position_image =
                            exact_position_bytes != (std::numeric_limits<std::uint64_t>::max)() &&
                            memory_stats.retained_position_capacity ==
                                memory_stats.retained_position_count &&
                            memory_stats.global_position_capacity_bytes == exact_position_bytes;
                        const bool retained_layout =
                            memory_stats.retained_layout_equation_holds() &&
                            memory_stats.owned_capacity_bytes == reader.admitted_bytes();
                        read_delta.grouped_soa_reader_capacity_accounting_passes =
                            memory_stats.capacity_accounting_passes;
                        read_delta.grouped_soa_reader_global_position_storage_preparations =
                            memory_stats.global_position_storage_preparations;
                        read_delta.grouped_soa_reader_global_position_storage_allocations =
                            memory_stats.global_position_storage_allocations;
                        read_delta.grouped_soa_reader_position_append_growth_reservations =
                            memory_stats.position_append_growth_reservations;
                        read_delta.grouped_soa_reader_retained_position_count =
                            memory_stats.retained_position_count;
                        read_delta.grouped_soa_reader_retained_position_capacity =
                            memory_stats.retained_position_capacity;
                        read_delta.grouped_soa_reader_directory_capacity_bytes =
                            memory_stats.directory_capacity_bytes;
                        read_delta.grouped_soa_reader_query_hash_capacity_bytes =
                            memory_stats.query_hash_capacity_bytes;
                        read_delta.grouped_soa_reader_query_record_capacity_bytes =
                            memory_stats.query_record_capacity_bytes;
                        read_delta.grouped_soa_reader_ordinal_route_capacity_bytes =
                            memory_stats.ordinal_route_capacity_bytes;
                        read_delta.grouped_soa_reader_selected_extent_capacity_bytes =
                            memory_stats.selected_extent_capacity_bytes;
                        read_delta.grouped_soa_reader_current_selected_control_capacity_bytes =
                            memory_stats.current_selected_control_capacity_bytes;
                        read_delta.grouped_soa_reader_global_position_capacity_bytes =
                            memory_stats.global_position_capacity_bytes;
                        read_delta.grouped_soa_reader_control_payload_capacity_bytes =
                            memory_stats.control_payload_capacity_bytes;
                        read_delta.grouped_soa_reader_owned_capacity_bytes =
                            memory_stats.owned_capacity_bytes;
                        read_delta.grouped_soa_reader_admitted_bytes = reader.admitted_bytes();
                        read_delta.grouped_soa_reader_exact_position_image_checks = 1;
                        read_delta.grouped_soa_reader_exact_position_image_failures =
                            exact_position_image ? 0 : 1;
                        read_delta.grouped_soa_reader_retained_layout_checks = 1;
                        read_delta.grouped_soa_reader_retained_layout_failures =
                            retained_layout ? 0 : 1;
                        read_delta.are_grouped_soa_reader_memory_stats_saturated =
                            memory_stats.saturated ||
                            exact_position_bytes == (std::numeric_limits<std::uint64_t>::max)();
                    };
                    if (group.persisted_fragment_reader) {
                        record_grouped_soa_reader_memory_stats(*group.persisted_fragment_reader);
                    } else if (group.persisted_grouped_soa_reader) {
                        record_grouped_soa_reader_memory_stats(*group.persisted_grouped_soa_reader);
                    }
                    operational.merge(read_delta);
                    merge_cold_index_preparation_stats(operational, read.cold);
                }
                join_cpu_prepared_target.persisted_read_stats_published = true;
            }
            if (join_gpu_prepared_grouped_soa_target.ready() &&
                !join_gpu_prepared_grouped_soa_target.persisted_read_stats_published) {
                merge_cold_index_preparation_stats(operational, join_cold_policy_stats);
                merge_generation_read_stats(
                    operational, join_gpu_prepared_grouped_soa_target.persisted_read_stats);
                ::bioseq::pipeline_stats::pipeline_operational_stats gpu_load{};
                gpu_load.gpu_persisted_resident_uploads = 1;
                gpu_load.gpu_buffer_creates = 3;
                gpu_load.gpu_memory_allocations = 3;
                operational.merge(gpu_load);
                join_gpu_prepared_grouped_soa_target.persisted_read_stats_published = true;
            }
            if (join_cpu_prepared_target.unions_ready && !join_cpu_prepared_target.groups.empty() &&
                !join_prepared_union_outcomes_recorded) {
                operational.cpu_prepared_unique_keys = join_cpu_prepared_target.total_unique_keys;
                operational.cpu_prepared_key_set_checksum =
                    join_cpu_prepared_target.key_set_checksum;
                operational.cpu_prepared_key_set_checksum_available = true;
                join_prepared_union_outcomes_recorded = true;
            }
            const bool every_group_built =
                !join_cpu_prepared_target.groups.empty() &&
                std::all_of(join_cpu_prepared_target.groups.begin(),
                            join_cpu_prepared_target.groups.end(),
                            [](const auto& group) noexcept {
                                return (group.target_index != nullptr ||
                                        group.persisted_fragment_reader.has_value() ||
                                        group.persisted_grouped_soa_reader.has_value() ||
                                        group.persisted_hash_major_reader.has_value()) &&
                                       group.build_set_checksum_present &&
                                       group.build_set_checksum == group.key_set_checksum;
                            });
            if (every_group_built && !join_prepared_build_checksum_recorded) {
                operational.cpu_prepared_build_set_checksum =
                    join_cpu_prepared_target.key_set_checksum;
                operational.cpu_prepared_build_set_checksum_available = true;
                join_prepared_build_checksum_recorded = true;
            }
            join_scanner_pg_cache->close_all(/*commit=*/false);
            bioseq::storage::destroy_seq_caches(join_scanner_pg_cache);
            join_scanner_pg_cache = nullptr;
            for (std::size_t query_index = 0; query_index < batch_size; ++query_index) {
                if (join_query_detoast_copies[query_index] != nullptr) {
                    pfree(join_query_detoast_copies[query_index]);
                    join_query_detoast_copies[query_index] = nullptr;
                }
            }

            join_match_map.clear();
            join_output_cursors.clear();
            for (const auto& [result_key, matches] : join_result_scratch) {
                (void)matches;
                const bool known =
                    std::any_of(join_queries.begin(),
                                join_queries.end(),
                                [result_key](const join_query_ref& query) noexcept {
                                    return query.ordinal == static_cast<std::uint64_t>(result_key);
                                });
                if (result_key < 0 || !known) {
                    ereport(ERROR,
                            (errcode(ERRCODE_DATA_CORRUPTED),
                             errmsg("bioseq batch join processor returned an unknown query id"),
                             errdetail("query_id=%d", result_key)));
                }
            }
            for (std::size_t query_index = 0; query_index < join_queries.size(); ++query_index) {
                const std::uint64_t ordinal = join_queries[query_index].ordinal;
                auto found = join_result_scratch.find(static_cast<std::int32_t>(ordinal));
                if (found == join_result_scratch.end()) {
                    continue;
                }
                auto [inserted, did_insert] =
                    join_match_map.emplace(ordinal, std::move(found->second));
                if (!did_insert) {
                    ereport(ERROR,
                            (errcode(ERRCODE_DATA_CORRUPTED),
                             errmsg("bioseq batch join produced duplicate query ordinals")));
                }
                for (std::size_t match_index = 0; match_index < inserted->second.size();
                     ++match_index) {
                    const auto& match = inserted->second[match_index];
                    if (base_params.do_cigar && (!match.has_cigar || match.cigar.empty())) {
                        continue;
                    }
                    join_output_cursors.push_back(join_output_cursor{query_index, match_index});
                }
            }
            join_output_position = 0;
            join_result_scratch.clear();
            join_target_outcomes_recorded = true;
        } catch (...) {
            cpp_error = std::current_exception();
        }
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(saved_context);
        original_error = CopyErrorData();
        FlushErrorState();
    }
    PG_END_TRY();
    if (original_error != nullptr) {
        abandon_join_runtime_after_pg_error_noexcept();
        bioseq::postgres::scheduler::end_batch(ticket, false);
        ErrorData* exact_error = original_error;
        throw bioseq::postgres::support::postgres_error{exact_error};
    }
    if (cpp_error != nullptr) {
        abandon_join_runtime_after_pg_error_noexcept();
        bioseq::postgres::scheduler::end_batch(ticket, false);
        std::rethrow_exception(cpp_error);
    }
    bioseq::postgres::scheduler::end_batch(ticket, true);
    return true;
}

/** @brief Implements the finish join target contract declared by the stable buffer facade. */
void tuple_dispatch_buffer::finish_join_target() {
    if (!join_mode) {
        return;
    }
    if (join_gpu_prepared_grouped_soa_target.resident != nullptr) {
        const bool gpu_equation_valid =
            join_gpu_prepared_grouped_soa_target.ready() &&
            join_gpu_prepared_grouped_soa_target.query_count != 0 &&
            join_gpu_prepared_grouped_soa_target.dispatched_query_count <=
                join_gpu_prepared_grouped_soa_target.query_count &&
            (join_gpu_prepared_grouped_soa_target.dispatched_query_count == 0
                 ? join_gpu_prepared_grouped_soa_target.dispatch_waves == 0
                 : join_gpu_prepared_grouped_soa_target.dispatch_waves != 0);
        if (!gpu_equation_valid) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq batch join GPU grouped-SoA target lifecycle equation failed"),
                     errdetail("prepared_queries=%llu dispatched_queries=%llu waves=%llu ready=%s",
                               static_cast<unsigned long long>(
                                   join_gpu_prepared_grouped_soa_target.query_count),
                               static_cast<unsigned long long>(
                                   join_gpu_prepared_grouped_soa_target.dispatched_query_count),
                               static_cast<unsigned long long>(
                                   join_gpu_prepared_grouped_soa_target.dispatch_waves),
                               join_gpu_prepared_grouped_soa_target.ready() ? "true" : "false")));
        }
    }
    std::uint64_t builds = 0;
    std::uint64_t reuses = 0;
    std::uint64_t duplicate_build_attempts = 0;
    std::uint64_t built_groups = 0;
    std::uint64_t reader_loads = 0;
    std::uint64_t reader_queries = 0;
    for (const auto& group : join_cpu_prepared_target.groups) {
        builds += group.target_index_builds;
        reuses += group.target_index_reuses;
        duplicate_build_attempts += group.duplicate_build_attempts;
        built_groups += group.target_index != nullptr ? 1u : 0u;
        reader_loads += group.persisted_reader_loads;
        reader_queries += group.persisted_reader_queries;
    }
    const bool reader_mode = reader_loads != 0;
    const std::uint64_t processed_queries = reader_mode ? reader_queries : builds + reuses;
    const bool fully_processed = processed_queries == join_cpu_prepared_target.query_count;
    if ((built_groups != 0 || reader_loads != 0) &&
        (builds != built_groups ||
         builds + reader_loads != join_cpu_prepared_target.physical_target_index_reads ||
         processed_queries > join_cpu_prepared_target.query_count ||
         (reader_mode && (reader_loads != 1 || builds != 0 || reuses != 0)) ||
         duplicate_build_attempts != 0 ||
         (fully_processed && join_cpu_prepared_target.query_count != 0 &&
          !join_prepared_build_checksum_recorded))) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq batch join target-wide CPU preparation equation failed"),
                 errdetail("builds=%llu built_groups=%llu reads=%llu reuses=%llu queries=%llu "
                           "duplicate_build_attempts=%llu checksum_published=%s",
                           static_cast<unsigned long long>(builds),
                           static_cast<unsigned long long>(built_groups),
                           static_cast<unsigned long long>(
                               join_cpu_prepared_target.physical_target_index_reads),
                           static_cast<unsigned long long>(reuses),
                           static_cast<unsigned long long>(join_cpu_prepared_target.query_count),
                           static_cast<unsigned long long>(duplicate_build_attempts),
                           join_prepared_build_checksum_recorded ? "true" : "false")));
    }
    if (!fully_processed) {
        bioseq_log<logging_level::debug>(
            "[bioseq] batch join prepared target finalized partial: "
            "processed_queries={} prepared_queries={} reader_mode={}",
            static_cast<unsigned long long>(processed_queries),
            static_cast<unsigned long long>(join_cpu_prepared_target.query_count),
            reader_mode);
    }
    join_queries.clear();
    join_match_map.clear();
    join_output_cursors.clear();
    join_output_position = 0;
    join_target_active = false;
    join_target_prepared = false;
    join_pending_cpu_preparation_queries.clear();
    join_cpu_preparation_pending = false;
    join_cpu_preparation_scratch.clear_storage_noexcept();
    join_target_outcomes_recorded = false;
    join_prepared_union_outcomes_recorded = false;
    join_prepared_build_checksum_recorded = false;
    join_active_read_policy.reset();
    join_cold_policy_stats = {};
    join_cpu_prepared_target = ::bioseq::simd::batch_processor::cpu_prepared_target{};
    join_gpu_prepared_grouped_soa_target =
        ::bioseq::simd::batch_processor::gpu_prepared_grouped_soa_target{};
    clear_slots();
}

/** @brief Implements the abandon join runtime after pg error noexcept contract declared by the
 * stable buffer facade. */
void tuple_dispatch_buffer::abandon_join_runtime_after_pg_error_noexcept() noexcept {
    if (join_scanner_pg_cache != nullptr) {
        auto* abandoned = std::exchange(join_scanner_pg_cache, nullptr);
        for (auto& [key, entry] : abandoned->entries) {
            (void)key;
            entry.rel = nullptr;
            entry.write_state = nullptr;
            entry.read_state = nullptr;
            entry.read_snapshot_source = nullptr;
            entry.pending_bytes_delta = 0;
            entry.pending_total_payload_delta = 0;
        }
        abandoned->entries.clear();
        abandoned->~seq_caches();
    }
    join_query_detoast_copies.fill(nullptr);
    join_query_rewidth_varlenas.clear();
    join_scanner_pg_ctx.reset();
    join_result_scratch.clear();
    join_queries.clear();
    join_match_map.clear();
    join_output_cursors.clear();
    join_output_position = 0;
    join_target_hdrs.fill(nullptr);
    join_query_hdrs.fill(nullptr);
    join_query_ids.fill(0);
    join_target_active = false;
    join_target_prepared = false;
    join_pending_cpu_preparation_queries.clear();
    join_cpu_preparation_pending = false;
    join_cpu_preparation_scratch.clear_storage_noexcept();
    join_target_outcomes_recorded = false;
    join_prepared_union_outcomes_recorded = false;
    join_prepared_build_checksum_recorded = false;
    join_active_read_policy.reset();
    join_cold_policy_stats = {};
    join_cpu_prepared_target = ::bioseq::simd::batch_processor::cpu_prepared_target{};
    join_gpu_prepared_grouped_soa_target =
        ::bioseq::simd::batch_processor::gpu_prepared_grouped_soa_target{};
    for (auto& entry : buffered_entries) {
        entry.scan_slot_copy = nullptr;
    }
    buffered_entries.clear();
    match_map.clear();
    output_cursors.clear();
    output_position = -1;
    for (auto& requests : per_bucket_target_requests)
        requests.clear();
    for (auto& requests : per_bucket_query_requests)
        requests.clear();
    fetch_bulk_sort_scratch.clear();
    hydrated_target_payloads.clear();
    hydrated_query_payloads.clear();
    ::abort_pg_sequence_read_session_noexcept(std::exchange(active_wave_read_session, nullptr));
    active_wave_permit.reset();
    slice_async_owner = nullptr;
    mmsearch_target_payload_observer = {};
    mmsearch_query_payload_observer = {};
    copy_staging_slot = nullptr;
    batch_payload_ctx = nullptr;
}

/** @brief Implements the reset join runtime for rescan contract declared by the stable buffer
 * facade. */
void tuple_dispatch_buffer::reset_join_runtime_for_rescan() {
    join_queries.clear();
    join_match_map.clear();
    join_output_cursors.clear();
    join_output_position = 0;
    join_target_active = false;
    join_target_prepared = false;
    join_pending_cpu_preparation_queries.clear();
    join_cpu_preparation_pending = false;
    join_cpu_preparation_scratch.clear_storage_noexcept();
    join_target_outcomes_recorded = false;
    join_prepared_union_outcomes_recorded = false;
    join_prepared_build_checksum_recorded = false;
    join_active_read_policy.reset();
    join_cold_policy_stats = {};
    join_cpu_prepared_target = ::bioseq::simd::batch_processor::cpu_prepared_target{};
    join_gpu_prepared_grouped_soa_target =
        ::bioseq::simd::batch_processor::gpu_prepared_grouped_soa_target{};
    clear_slots();
    last_stats = ::bioseq::pipeline_stats::pipeline_execution_stats{};
}

} // namespace bioseq::scheduler
