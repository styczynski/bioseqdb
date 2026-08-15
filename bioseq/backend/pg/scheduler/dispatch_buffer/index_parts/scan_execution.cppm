/**
 * @file index_parts/scan_execution.cppm
 * @brief Defines ordinary scan dispatch, admitted waves, and extra-query intersection.
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
module bioseq.postgres.scheduler.dispatch_buffer:scan_execution;

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

/** @brief Implements the run batch and build cursors contract declared by the stable buffer facade.
 */
[[nodiscard]] bool tuple_dispatch_buffer::run_batch_and_build_cursors() {
    bioseq::postgres::scheduler::backend_sync();
    if (buffered_entries.size() > dispatch_batch_capacity) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq attempted to dispatch an oversized tuple buffer"),
                 errdetail("buffered=%zu capacity=%zu",
                           buffered_entries.size(),
                           dispatch_batch_capacity)));
    }
    const size_t batch_size = buffered_entries.size();
    /*
     * This value is one completed dispatch-buffer delta.
     * Waves and stacked extra queries below merge into it, but a later
     * ReScan/batch must never replay an earlier delta into the statement
     * collector.
     */
    last_stats = ::bioseq::pipeline_stats::pipeline_execution_stats{};
    const bool executed_nonempty_buffer = mmsearch_state.active && batch_size != 0;
    const std::int64_t scheduled_queries =
        static_cast<std::int64_t>(batch_size) *
        static_cast<std::int64_t>(1 + extra_query_varlenas.size());
    const auto ticket = bioseq::postgres::scheduler::begin_batch(scheduled_queries);
    MemoryContext saved_context = CurrentMemoryContext;
    ErrorData* volatile pg_error = nullptr;
    std::exception_ptr cpp_error;
    PG_TRY();
    {
        try {
            run_batch_and_build_cursors_impl();
        } catch (...) {
            MemoryContextSwitchTo(saved_context);
            cpp_error = std::current_exception();
        }
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(saved_context);
        pg_error = CopyErrorData();
        FlushErrorState();
    }
    PG_END_TRY();
    if (pg_error != nullptr) {
        bioseq::postgres::scheduler::end_batch(ticket, false);
        ErrorData* exact_error = pg_error;
        throw bioseq::postgres::support::postgres_error{exact_error};
    }
    if (cpp_error != nullptr) {
        bioseq::postgres::scheduler::end_batch(ticket, false);
        std::rethrow_exception(cpp_error);
    }
    bioseq::postgres::scheduler::end_batch(ticket, true);
    return executed_nonempty_buffer;
}

/** @brief Implements the run batch and build cursors impl contract declared by the stable buffer
 * facade. */
void tuple_dispatch_buffer::run_batch_and_build_cursors_impl() {
    /*
     * Hydrate in row-span waves. A small batch naturally becomes
     * one wave; larger batches split at the current index-memory cap so
     * the dispatch stage does not force materialisation of rows that are
     * not active in the current wave.
     */
    std::array<const bioseq::pg_types::seq*, dispatch_batch_capacity> target_hdrs{};
    std::array<const bioseq::pg_types::seq*, dispatch_batch_capacity> per_row_query_hdrs{};
    std::array<int32_t, dispatch_batch_capacity> query_ids{};

    const size_t batch_size = buffered_entries.size();
    if (!mmsearch_state.active || batch_size == 0) {
        match_map.clear();
        output_cursors.clear();
        output_position = 0;
        return;
    }

    /*
     * We use the buffer entry index `i` as the batch_processor's
     * key rather than the row's primary_key column. The PK from
     * the user's table is only available when the targetlist
     * includes a bioseq.<projection>(pk) FuncExpr (which is what
     * BeginCustomScan walks to discover `primary_key_attno`).
     * For queries without such a projection — e.g. plain
     * `SELECT t.pk FROM t WHERE mmsearch(t.seq, ...)` or
     * cross-table mmsearch joins where the qual is the only
     * bioseq surface — primary_key_attno stays
     * InvalidAttrNumber and every row would map to query_id=-1,
     * letting the std::map key collapse all matches into one
     * bucket. Using the entry index as the key keeps each row
     * uniquely identifiable inside the batch regardless of
     * targetlist shape.
     */
    std::vector<size_t> all_rows(batch_size);
    std::iota(all_rows.begin(), all_rows.end(), size_t{0});
    const bool batch_has_recipe =
        std::ranges::any_of(buffered_entries, [](const buffered_scan_entry& entry) noexcept {
            return entry.has_seq_recipe() || entry.has_per_row_query_recipe();
        });
#if defined(BIOSEQ_TEST_INDEXES)
    const bool forced_flat_baseline = (bioseq::settings::testing::in_memory_scanner::enabled ||
                                       bioseq::settings::testing::chunked_scanner::enabled) &&
                                      !batch_has_recipe;
#else
    constexpr bool forced_flat_baseline = false;
#endif
    const bool vulkan_requested = ::bioseq::simd::pipe::vulkan_components_requested(base_params);
    if (vulkan_requested && !::bioseq::simd::pipe::complete_all_gpu_tuple_selected(base_params)) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("Vulkan aligner execution failed"),
                 errdetail("Vulkan complete-tuple admission: requested Vulkan components do not "
                           "form the complete supported all-GPU tuple; generic CPU/SIMD biological "
                           "fallback is forbidden"),
                 errhint("Selected Vulkan execution has no CPU fallback; fix the Vulkan runtime or "
                         "select a non-Vulkan algorithm before dispatch.")));
    }
    /* Selected Vulkan consumes source-backed inputs through typed lazy
     * views and owner-bearing admitted_packed_sequence uploads. It must not
     * ask the tuple buffer to EOH-flatten locators or recipes. */
    const bool requires_flat_sequence_payloads = forced_flat_baseline;

    std::size_t lpu_for_hydration = 0;
    std::size_t bytes_per_unit_for_hydration = 0;
    bioseq::pg_types::seq::dispatch_instance(
        mmsearch_state.alphabet_code,
        mmsearch_state.storage_unit_byte_width,
        [&lpu_for_hydration, &bytes_per_unit_for_hydration]<class p_sequence>() noexcept {
            using traits = typename compact_sequence_range<p_sequence>::traits;
            lpu_for_hydration = traits::letters_per_unit;
            bytes_per_unit_for_hydration = sizeof(typename traits::storage_unit);
        });
    std::size_t query_lpu_for_hydration = lpu_for_hydration;
    std::size_t query_bytes_per_unit_for_hydration = bytes_per_unit_for_hydration;
    if (per_row_mode()) {
        bioseq::pg_types::seq::dispatch_instance(
            mmsearch_state.query_alphabet_code,
            mmsearch_state.query_storage_unit_byte_width,
            [&query_lpu_for_hydration,
             &query_bytes_per_unit_for_hydration]<class p_sequence>() noexcept {
                using traits = typename compact_sequence_range<p_sequence>::traits;
                query_lpu_for_hydration = traits::letters_per_unit;
                query_bytes_per_unit_for_hydration = sizeof(typename traits::storage_unit);
            });
    }
    auto estimate_flat_sequence_bytes = [](const std::uint64_t bp_length,
                                           const std::size_t letters_per_unit,
                                           const std::size_t bytes_per_unit) -> std::uint64_t {
        if (letters_per_unit == 0 || bytes_per_unit == 0) {
            return 0;
        }
        const std::uint64_t total_units =
            bp_length / letters_per_unit +
            static_cast<std::uint64_t>(bp_length % letters_per_unit != 0u);
        if (total_units > (std::numeric_limits<std::uint64_t>::max)() /
                              static_cast<std::uint64_t>(bytes_per_unit)) {
            return 0;
        }
        const std::uint64_t payload_bytes =
            total_units * static_cast<std::uint64_t>(bytes_per_unit);
        if (payload_bytes > (std::numeric_limits<std::uint64_t>::max)() -
                                static_cast<std::uint64_t>(bioseq::pg_types::seq::header_bytes())) {
            return 0;
        }
        const std::uint64_t varlena_bytes =
            static_cast<std::uint64_t>(bioseq::pg_types::seq::header_bytes()) + payload_bytes;
        if (varlena_bytes > static_cast<std::uint64_t>(MaxAllocSize)) {
            return 0;
        }
        return varlena_bytes;
    };
    auto estimate_hydration_bytes = [this,
                                     requires_flat_sequence_payloads,
                                     lpu_for_hydration,
                                     bytes_per_unit_for_hydration,
                                     query_lpu_for_hydration,
                                     query_bytes_per_unit_for_hydration,
                                     &estimate_flat_sequence_bytes](size_t row) -> std::uint64_t {
        if (!requires_flat_sequence_payloads) {
            return 0u;
        }
        if (row >= buffered_entries.size()) {
            return 0;
        }
        const buffered_scan_entry& entry = buffered_entries[row];
        std::uint64_t bytes = 0;
        if (entry.seq_loc.has_value()) {
            const std::uint64_t target_bytes = estimate_flat_sequence_bytes(
                entry.seq_loc->logical_length, lpu_for_hydration, bytes_per_unit_for_hydration);
            bytes = target_bytes;
        }
        if (entry.per_row_query_loc.has_value()) {
            const std::uint64_t query_bytes =
                estimate_flat_sequence_bytes(entry.per_row_query_loc->logical_length,
                                             query_lpu_for_hydration,
                                             query_bytes_per_unit_for_hydration);
            if (query_bytes > (std::numeric_limits<std::uint64_t>::max)() - bytes) {
                return (std::numeric_limits<std::uint64_t>::max)();
            }
            bytes += query_bytes;
            if (mmsearch_state.query_storage_unit_byte_width !=
                mmsearch_state.storage_unit_byte_width) {
                const std::uint64_t rewidthed_query_bytes =
                    estimate_flat_sequence_bytes(entry.per_row_query_loc->logical_length,
                                                 lpu_for_hydration,
                                                 bytes_per_unit_for_hydration);
                if (rewidthed_query_bytes > (std::numeric_limits<std::uint64_t>::max)() - bytes) {
                    return (std::numeric_limits<std::uint64_t>::max)();
                }
                bytes += rewidthed_query_bytes;
            }
        }
        return bytes;
    };
    auto refresh_row_arrays =
        [this, &target_hdrs, &per_row_query_hdrs, &query_ids](std::span<const size_t> rows) {
            for (const size_t i : rows) {
                if (i >= buffered_entries.size() || i >= dispatch_batch_capacity) {
                    continue;
                }
                const buffered_scan_entry& entry = buffered_entries[i];
                target_hdrs[i] = entry.seq_hdr(); // nullptr for NULL seq
                per_row_query_hdrs[i] = entry.per_row_query_hdr();
                query_ids[i] = static_cast<int32_t>(i);
            }
        };

    std::vector<std::pair<size_t, size_t>> waves;
    const std::uint64_t budget = bioseq::parallelism::memory_admission_controller::instance()
                                     .allocatable_after_reservations();
    std::uint64_t wave_bytes = 0;
    size_t wave_begin = 0;
    for (size_t i = 0; i < batch_size; ++i) {
        const std::uint64_t row_bytes = estimate_hydration_bytes(i);
        if (requires_flat_sequence_payloads &&
            row_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("selected dispatch cannot materialize sequence recipe"),
                     errdetail("row=%zu requires an invalid or oversized flat payload", i)));
        }
        const bool split = budget != (std::numeric_limits<std::uint64_t>::max)() &&
                           wave_bytes != 0 && row_bytes > 0 &&
                           row_bytes > budget - std::min(wave_bytes, budget);
        if (split) {
            waves.emplace_back(wave_begin, i);
            wave_begin = i;
            wave_bytes = 0;
        }
        if (row_bytes > (std::numeric_limits<std::uint64_t>::max)() - wave_bytes) {
            wave_bytes = (std::numeric_limits<std::uint64_t>::max)();
        } else {
            wave_bytes += row_bytes;
        }
    }
    if (batch_size > 0) {
        waves.emplace_back(wave_begin, batch_size);
    }

    const bioseq::pg_types::seq* query_hdr = mmsearch_state.query_hdr();
    const uint32_t query_bp_size = query_hdr != nullptr ? query_hdr->bp_size : 0;
    bioseq_log<logging_level::debug>(
        "[bioseq] pg: tuple_dispatch_buffer calling batch_processor::process_sequences "
        "batch_size={} typmod=({},{}) query_bp_size={} read_index={} per_row_mode={}",
        batch_size,
        static_cast<unsigned>(mmsearch_state.alphabet_code),
        static_cast<unsigned>(mmsearch_state.storage_unit_byte_width),
        query_bp_size,
        read_sequence_index_active,
        per_row_mode());

    // Wire scanner_pg into the dispatch loop.
    // Allocates a fresh seq_caches for scanner_pg's PG-backed reads.
    // Lives in batch_payload_ctx so palloc'd state survives the call.
    if (batch_payload_ctx == nullptr) {
        batch_payload_ctx = AllocSetContextCreate(
            executor_query_mctx, "tuple_dispatch_buffer payload arena", ALLOCSET_DEFAULT_SIZES);
    }
    bioseq::storage::seq_caches* scanner_pg_cache =
        bioseq::storage::alloc_seq_caches(batch_payload_ctx);
    scanner_pg_cache->side = bioseq::storage::seq_caches::access_side::read;
    scanner_pg_cache->lock_mode = AccessShareLock;
    scanner_pg_cache->read_state_mctx = batch_payload_ctx;
    scanner_pg_cache->main_database_oid = user_main_database_oid;
    scanner_pg_cache->main_tablespace_oid = user_main_tablespace_oid;
    scanner_pg_cache->main_relfilenode = user_main_relfilenode;

    typename ::bioseq::simd::batch_processor::scanner_pg_context scanner_pg_ctx{};
    scanner_pg_ctx.async_job_observer = make_slice_async_pool_observer_noexcept(slice_async_owner);
    scanner_pg_ctx.mmsearch_target_payload_observer = mmsearch_target_payload_observer;
    scanner_pg_ctx.mmsearch_query_payload_observer = mmsearch_query_payload_observer;
    scanner_pg_ctx.db_oid = user_main_database_oid;
    scanner_pg_ctx.tablespace_oid = user_main_tablespace_oid;
    scanner_pg_ctx.main_relfilenode = user_main_relfilenode;
    scanner_pg_ctx.mmsearch_seq_attno = static_cast<std::int16_t>(mmsearch_state.seq_attno);
    scanner_pg_ctx.mmsearch_query_attno = static_cast<std::int16_t>(query_source_attno());
    scanner_pg_ctx.query_alphabet_code = mmsearch_state.query_alphabet_code;
    scanner_pg_ctx.query_storage_unit_byte_width = mmsearch_state.query_storage_unit_byte_width;
    scanner_pg_ctx.caches = scanner_pg_cache;
    scanner_pg_ctx.parent_mctx = batch_payload_ctx;
    scanner_pg_ctx.snapshot = scan_snapshot;
    scanner_pg_ctx.source_provider = source_provider;
    scanner_pg_ctx.persisted_gpu_operation_stats_sink = persisted_gpu_operation_stats_sink;
    // Populate per-row locators for the target column. The production
    // scanner_pg path uses these. Last-fragment padding is recomputed
    // inside process_sequences where the template parameter p_sequence (and thus
    // its letters_per_unit) is available.
    for (size_t i = 0; i < batch_size; ++i) {
        const auto& seq_loc_opt = buffered_entries[i].seq_loc;
        if (seq_loc_opt.has_value()) {
            scanner_pg_ctx.target_locators[i] = *seq_loc_opt;
        }
        const auto& query_loc_opt = buffered_entries[i].per_row_query_loc;
        if (query_loc_opt.has_value()) {
            scanner_pg_ctx.query_locators[i] = *query_loc_opt;
        }
        if (buffered_entries[i].has_seq_recipe()) {
            scanner_pg_ctx.target_recipe_datums[i] = buffered_entries[i].seq_recipe_datum;
            scanner_pg_ctx.target_recipe_descriptors[i] = buffered_entries[i].seq_recipe_descriptor;
        }
        if (buffered_entries[i].has_per_row_query_recipe()) {
            scanner_pg_ctx.query_recipe_datums[i] = buffered_entries[i].per_row_query_recipe_datum;
            scanner_pg_ctx.query_recipe_descriptors[i] =
                buffered_entries[i].per_row_query_recipe_descriptor;
        }
        // Surface the per-row columnar row number (the index
        // query_key) so the stored per-fragment index can be assembled.
        scanner_pg_ctx.target_rownos[i] = buffered_entries[i].sequence_rowno;
    }
    scanner_pg_ctx.user_relid = user_relid;
    scanner_pg_ctx.index_read_requirement = index_read_requirement;
    scanner_pg_ctx.read_index_active = read_sequence_index_active;
    scanner_pg_ctx.index_async_persist_active = bioseq::settings::index::async_persistence::enabled;
    scanner_pg_ctx.recompute_async_persist_active =
        bioseq::settings::index::recompute_async_persistence::enabled;
    // Resolve the target column's active minimizer
    // policy ONCE per batch. Stored-index reads use it when
    // read_index_active is true; no-index recompute also uses it to select
    // the transient minimizer-fragment/minimizer-sequence index shape without
    // reading stored chunks. The active-policy catalog row selects that shape
    // even when payload reads are forbidden. Resolving policy metadata is
    // not a persisted target-index payload probe and must not mutate the
    // existing no-index algorithm contract.
    apply_active_read_policy_resolution(
        scanner_pg_ctx,
        bioseq::storage::indexing::resolve_active_read_index_outcome(
            *executor_read_context, static_cast<std::int16_t>(mmsearch_state.seq_attno)),
        user_relid,
        user_main_relfilenode,
        static_cast<std::int16_t>(mmsearch_state.seq_attno));
    auto reset_dispatch_arrays = [&target_hdrs, &per_row_query_hdrs, &query_ids] {
        target_hdrs.fill(nullptr);
        per_row_query_hdrs.fill(nullptr);
        query_ids.fill(-1);
    };
    auto reset_scanner_wave = [&scanner_pg_ctx] {
        scanner_pg_ctx.target_locators.fill(std::nullopt);
        scanner_pg_ctx.query_locators.fill(std::nullopt);
        scanner_pg_ctx.target_recipe_datums.fill(Datum{0});
        scanner_pg_ctx.query_recipe_datums.fill(Datum{0});
        scanner_pg_ctx.target_recipe_descriptors.fill(pg_sequence_descriptor{});
        scanner_pg_ctx.query_recipe_descriptors.fill(pg_sequence_descriptor{});
        scanner_pg_ctx.target_rownos.fill(-1);
        scanner_pg_ctx.target_index_sources.fill(
            ::bioseq::pipeline_stats::target_index_source::none);
    };
    auto install_scanner_wave = [this, &scanner_pg_ctx](std::span<const size_t> rows) {
        for (const size_t row : rows) {
            if (row >= buffered_entries.size() || row >= dispatch_batch_capacity) {
                continue;
            }
            const auto& seq_loc_opt = buffered_entries[row].seq_loc;
            if (seq_loc_opt.has_value()) {
                scanner_pg_ctx.target_locators[row] = *seq_loc_opt;
            }
            const auto& query_loc_opt = buffered_entries[row].per_row_query_loc;
            if (query_loc_opt.has_value()) {
                scanner_pg_ctx.query_locators[row] = *query_loc_opt;
            }
            if (buffered_entries[row].has_seq_recipe()) {
                scanner_pg_ctx.target_recipe_datums[row] = buffered_entries[row].seq_recipe_datum;
                scanner_pg_ctx.target_recipe_descriptors[row] =
                    buffered_entries[row].seq_recipe_descriptor;
            }
            if (buffered_entries[row].has_per_row_query_recipe()) {
                scanner_pg_ctx.query_recipe_datums[row] =
                    buffered_entries[row].per_row_query_recipe_datum;
                scanner_pg_ctx.query_recipe_descriptors[row] =
                    buffered_entries[row].per_row_query_recipe_descriptor;
            }
            scanner_pg_ctx.target_rownos[row] = buffered_entries[row].sequence_rowno;
        }
    };
    auto wave_estimated_bytes =
        [&estimate_hydration_bytes](std::span<const size_t> rows) -> std::uint64_t {
        std::uint64_t bytes = 0;
        for (const size_t row : rows) {
            const std::uint64_t row_bytes = estimate_hydration_bytes(row);
            if (row_bytes > (std::numeric_limits<std::uint64_t>::max)() - bytes) {
                return (std::numeric_limits<std::uint64_t>::max)();
            }
            bytes += row_bytes;
        }
        return bytes;
    };
    auto wave_has_source_facade = [this](std::span<const size_t> rows) noexcept {
        for (const size_t row : rows) {
            if (row >= buffered_entries.size())
                continue;
            const auto& entry = buffered_entries[row];
            if (entry.has_seq_recipe() || entry.has_per_row_query_recipe() ||
                entry.seq_loc.has_value() || entry.per_row_query_loc.has_value()) {
                return true;
            }
        }
        return false;
    };
    auto merge_stats =
        [this](const ::bioseq::pipeline_stats::pipeline_execution_stats& wave_stats) {
            last_stats.merge(wave_stats);
        };

    match_map.clear();
    // Pointer to the context.
    auto* scanner_pg_arg = &scanner_pg_ctx;
    ::abort_pg_sequence_read_session_noexcept(std::exchange(active_wave_read_session, nullptr));
    active_wave_permit.reset();
    if (copy_staging_slot != nullptr) {
        ExecDropSingleTupleTableSlot(copy_staging_slot);
        copy_staging_slot = nullptr;
    }
    const auto clear_active_wave_member_handles_noexcept = [this]() noexcept {
        for (auto& entry : buffered_entries) {
            entry.hydrated_seq_hdr = nullptr;
            entry.hydrated_per_row_query_hdr = nullptr;
        }
        hydrated_target_payloads.clear();
        hydrated_query_payloads.clear();
    };
    const auto release_active_wave_after_error_noexcept =
        [this, &clear_active_wave_member_handles_noexcept]() noexcept {
            scanner_pg_ctx.read_session = nullptr;
            ::abort_pg_sequence_read_session_noexcept(
                std::exchange(active_wave_read_session, nullptr));
            clear_active_wave_member_handles_noexcept();
            target_hdrs.fill(nullptr);
            per_row_query_hdrs.fill(nullptr);
            active_wave_permit.reset();
        };
    const auto release_active_wave_checked = [this, &clear_active_wave_member_handles_noexcept]() {
        if (active_wave_read_session != nullptr) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("bioseq dispatch wave session was not finished")));
        }
        clear_active_wave_member_handles_noexcept();
        target_hdrs.fill(nullptr);
        per_row_query_hdrs.fill(nullptr);
        active_wave_permit.reset();
    };
    const auto abandon_scanner_cache_noexcept =
        [this, &release_active_wave_after_error_noexcept]() noexcept {
            release_active_wave_after_error_noexcept();
            if (scanner_pg_cache == nullptr) {
                return;
            }
            for (auto& [key, entry] : scanner_pg_cache->entries) {
                (void)key;
                entry.rel = nullptr;
                entry.write_state = nullptr;
                entry.read_state = nullptr;
                entry.read_snapshot_source = nullptr;
                entry.pending_bytes_delta = 0;
                entry.pending_total_payload_delta = 0;
            }
            scanner_pg_cache->entries.clear();
            scanner_pg_cache->~seq_caches();
            scanner_pg_cache = nullptr;
        };
    MemoryContext saved_context = CurrentMemoryContext;
    ErrorData* volatile pg_error = nullptr;
    std::exception_ptr cpp_error;

    PG_TRY();
    {
        try {
            for (const auto& wave : waves) {
                const std::span<const size_t> rows{all_rows.data() + wave.first,
                                                   wave.second - wave.first};
                const std::uint64_t estimated_wave_bytes = wave_estimated_bytes(rows);
                active_wave_permit =
                    bioseq::parallelism::memory_admission_controller::instance().try_reserve(
                        estimated_wave_bytes);
                const bool hydrate_wave =
                    estimated_wave_bytes == 0 || active_wave_permit.has_value();
                if (!hydrate_wave) {
                    bioseq_log<logging_level::debug>(
                        "[bioseq] pg: tuple_dispatch_buffer wave [{}, {}) was not admitted for {} "
                        "bytes; target locators stream through scanner_pg",
                        wave.first,
                        wave.second,
                        static_cast<unsigned long long>(estimated_wave_bytes));
                }
                reset_dispatch_arrays();
                reset_scanner_wave();
                if (hydrate_wave) {
                    if (forced_flat_baseline) {
                        hydrate_locators_by_bucket(rows);
                    }
                }
                refresh_row_arrays(rows);
                install_scanner_wave(rows);
                if (wave_has_source_facade(rows)) {
                    if (active_wave_read_session != nullptr) {
                        ereport(ERROR,
                                (errcode(ERRCODE_INTERNAL_ERROR),
                                 errmsg("bioseq dispatch retained a stale read session")));
                    }
                    active_wave_read_session = ::begin_pg_sequence_read_session(batch_payload_ctx);
                    scanner_pg_ctx.read_session = active_wave_read_session;
                }

                ::bioseq::pipeline_stats::pipeline_execution_stats wave_stats{};
                std::map<int32_t, std::vector<::bioseq::simd::match_result>> wave_match_map;
                if (per_row_mode()) {
                    wave_match_map =
                        processor.process_sequences(batch_size,
                                                    mmsearch_state.alphabet_code,
                                                    mmsearch_state.storage_unit_byte_width,
                                                    nullptr,
                                                    &per_row_query_hdrs,
                                                    target_hdrs,
                                                    query_ids,
                                                    base_params,
                                                    ::bioseq::simd::batch_invocation_role::primary,
                                                    0,
                                                    wave_stats,
                                                    scanner_pg_arg);
                } else {
                    wave_match_map =
                        processor.process_sequences(batch_size,
                                                    mmsearch_state.alphabet_code,
                                                    mmsearch_state.storage_unit_byte_width,
                                                    query_hdr,
                                                    nullptr,
                                                    target_hdrs,
                                                    query_ids,
                                                    base_params,
                                                    ::bioseq::simd::batch_invocation_role::primary,
                                                    0,
                                                    wave_stats,
                                                    scanner_pg_arg);
                }
                merge_stats(wave_stats);

                /*
                 * For every extra mmsearch qual the caller registered we run
                 * an independent process_sequences pass over the same active
                 * wave, then logically AND the matches into the wave map.
                 */
                for (size_t extra_idx = 0; extra_idx < extra_query_varlenas.size(); ++extra_idx) {
                    const std::vector<char>& extra_bytes = extra_query_varlenas[extra_idx];
                    if (extra_bytes.empty()) {
                        continue;
                    }
                    const bioseq::pg_types::seq* extra_hdr =
                        reinterpret_cast<const bioseq::pg_types::seq*>(extra_bytes.data());
                    ::bioseq::pipeline_stats::pipeline_execution_stats extra_stats{};
                    auto extra_match_map = processor.process_sequences(
                        batch_size,
                        mmsearch_state.alphabet_code,
                        mmsearch_state.storage_unit_byte_width,
                        extra_hdr,
                        nullptr,
                        target_hdrs,
                        query_ids,
                        base_params,
                        ::bioseq::simd::batch_invocation_role::extra_query,
                        extra_broadcast_query_ordinal(per_row_mode(), batch_size, extra_idx),
                        extra_stats,
                        scanner_pg_arg);
                    merge_stats(extra_stats);
                    bioseq_log<logging_level::debug>(
                        "[bioseq] pg: tuple_dispatch_buffer extra-query #{} wave [{}, {}) produced "
                        "matches for {} entries",
                        extra_idx,
                        wave.first,
                        wave.second,
                        extra_match_map.size());
                    for (auto it = wave_match_map.begin(); it != wave_match_map.end();) {
                        if (extra_match_map.find(it->first) == extra_match_map.end()) {
                            it = wave_match_map.erase(it);
                        } else {
                            ++it;
                        }
                    }
                }

                for (auto& [entry_id, matches] : wave_match_map) {
                    match_map[entry_id] = std::move(matches);
                }
                if (active_wave_read_session != nullptr) {
                    ::finish_pg_sequence_read_session(active_wave_read_session, true);
                    active_wave_read_session = nullptr;
                    scanner_pg_ctx.read_session = nullptr;
                }
                release_active_wave_checked();
            }

            // Release scanner_pg's per-batch sequence caches after all
            // process_sequences calls have returned. close_all releases the
            // sub-table relations and their column-read states;
            // destroy_seq_caches returns the slab to the parent context.
            scanner_pg_cache->close_all(/*commit=*/false);
            bioseq::storage::destroy_seq_caches(scanner_pg_cache);
            scanner_pg_cache = nullptr;
        } catch (...) {
            MemoryContextSwitchTo(saved_context);
            cpp_error = std::current_exception();
        }
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(saved_context);
        pg_error = CopyErrorData();
        FlushErrorState();
    }
    PG_END_TRY();
    if (pg_error != nullptr) {
        abandon_scanner_cache_noexcept();
        ErrorData* exact_error = pg_error;
        throw bioseq::postgres::support::postgres_error{exact_error};
    }
    if (cpp_error != nullptr) {
        abandon_scanner_cache_noexcept();
        std::rethrow_exception(cpp_error);
    }

    output_cursors.clear();
    for (size_t i = 0; i < buffered_entries.size(); ++i) {
        const buffered_scan_entry& entry = buffered_entries[i];
        /*
         * Match map is keyed on the entry index (see comment in
         * the loop above that built `query_ids[i] = i`). Skip
         * entries whose index is not in the map — those rows had
         * no usable target sequence (NULL seq column) or the
         * aligner produced zero matches; either way `WHERE
         * mmsearch(...)` evaluates to FALSE for them.
         */
        auto hit = match_map.find(static_cast<int32_t>(i));
        if (hit == match_map.end()) {
            bioseq_log<logging_level::debug>(
                "[bioseq] pg: batch miss for entry index={}; WHERE mmsearch evaluates to FALSE", i);
            continue;
        }
        /*
         * Filter degenerate matches. The aligner can return a
         * match result that has a chain pinned in the seed pass
         * but no usable base alignment (`has_cigar == false` or
         * `cigar` empty). When the user requested a CIGAR via
         * `bioseq.cigar(...)`, an empty/no-cigar match is the
         * fuzzy-search equivalent of "no real hit": surfacing it
         * as a row would mean WHERE mmsearch(...) acts more like
         * a tautology than a filter, which contradicts the
         * substring-match semantics users expect of mmsearch.
         *
         * Skipping these here filters them out at output-cursor
         * build time; the row never reaches the projection
         * pipeline, so its bioseq.cigar / bioseq.target_start /
         * etc. would have come back as the placeholder Const's
         * default-NULL state — exactly the kind of "row but
         * empty fields" output the user reported as undesired.
         *
         * When `do_cigar` is false (the user only projects e.g.
         * bioseq.target_start / bioseq.score), we don't have a
         * cigar to inspect; in that case we keep every match
         * the chainer surfaced because the chain itself is
         * meaningful (passed `min_seed_count` / `min_chain_score`).
         */
        for (size_t m = 0; m < hit->second.size(); ++m) {
            const ::bioseq::simd::match_result& match_candidate = hit->second[m];
            if (base_params.do_cigar &&
                (!match_candidate.has_cigar || match_candidate.cigar.empty())) {
                bioseq_log<logging_level::debug>(
                    "[bioseq] pg: batch skip: degenerate match for entry index={} cursor m={} "
                    "(do_cigar=true but cigar empty)",
                    i,
                    m);
                continue;
            }
            output_cursors.push_back({i, m});
        }
    }
    output_position = 0;
    bioseq_log<logging_level::debug>(
        "[bioseq] pg: batch produced {} output tuples from {} scan tuples",
        output_cursors.size(),
        buffered_entries.size());
}

} // namespace bioseq::scheduler
