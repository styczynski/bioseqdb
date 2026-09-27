/**
 * @file index_parts/scan_execution.cppm
 * @brief Align buffered scan rows and apply extra mmsearch filters.
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

// Production scans read locators and slices through the statement source
// provider. Test settings can fetch full payloads first, then use an in-memory
// sequence or a synthetic fragmented scanner.
module bioseq.postgres.scheduler.dispatch_buffer:scan_execution;

import bioseq.postgres.scheduler.dispatch_buffer;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
import :accounting;
#endif
import :active_policy_accounting;

import bioseq.common.logging;
import bioseq.postgres.support;
import bioseq.postgres.configuration.core;
import bioseq.postgres.storage.indexing.active_read_policy_resolver;
import bioseq.postgres.scheduler.dispatch_processor; // Keep the alignment pipeline in a separate module.
import bioseq.indexing.gpu_minimizer_index;
import bioseq.alignment.contracts;
import bioseq.sequences;
import bioseq.pipeline.pipe;
import bioseq.postgres.types.compact_sequence;
import bioseq.postgres.storage.manager;
import bioseq.parallelism.memory_admission;
import bioseq.postgres.scheduler.supervisor;

namespace bioseq::scheduler {

namespace {

using scanner_context = ::bioseq::simd::batch_processor::scanner_pg_context;

/** @return Whether both pointers share the same statement plan and its ownership. */
[[nodiscard]] bool scanner_plan_matches(
    const scanner_context& scanner,
    const ::bioseq::simd::pipe::statement_execution_plan_owner& execution_plan) noexcept {
    return scanner.execution_plan && execution_plan &&
           scanner.execution_plan.get() == execution_plan.get() &&
           !scanner.execution_plan.owner_before(execution_plan) &&
           !execution_plan.owner_before(scanner.execution_plan);
}

/** Prepare one row's MPF target for Vulkan, using a stored index or a new source owner. */
void prepare_ordinary_scan_mpf_target(
    void* opaque_processor,
    scanner_context& scanner,
    const std::size_t row,
    const std::uint64_t target_bp_size,
    const std::uint8_t target_alphabet_code,
    const std::uint8_t target_storage_unit_byte_width,
    ::bioseq::simd::pipe::statement_execution_plan_owner execution_plan,
    scanner_context::gpu_mpf_target_owner& prepared_target) {
    auto* const processor = static_cast<::bioseq::simd::batch_processor*>(opaque_processor);
    if (processor == nullptr || row >= ::bioseq::simd::dispatch_batch_capacity ||
        target_bp_size == 0u || scanner.target_rownos[row] < 0 ||
        !scanner_plan_matches(scanner, execution_plan)) {
        throw std::logic_error{
            "ordinary MPF Vulkan preparation changed row or statement ownership"};
    }
    const auto target_rowno = static_cast<std::uint64_t>(scanner.target_rownos[row]);
    if (scanner.read_index_active) {
        if (scanner.executor_read_context == nullptr) {
            throw std::logic_error{
                "ordinary persisted MPF Vulkan preparation has no executor read owner"};
        }
        processor->prepare_gpu_persisted_mpf_target(
            scanner.db_oid,
            scanner.tablespace_oid,
            scanner.main_relfilenode,
            scanner.user_relid,
            scanner.mmsearch_seq_attno,
            scanner.active_index,
            target_rowno,
            target_bp_size,
            /*query_count=*/1u,
            target_alphabet_code,
            target_storage_unit_byte_width,
            std::move(execution_plan),
            *scanner.executor_read_context,
            prepared_target);
    } else {
        prepared_target = scanner_context::gpu_mpf_target_owner{};
        prepared_target.execution_plan = std::move(execution_plan);
        prepared_target.target_rowno = static_cast<std::int64_t>(target_rowno);
        prepared_target.target_bp_size = target_bp_size;
        prepared_target.query_count = 1u;
        prepared_target.operation_identity =
            bioseq::indexing::next_gpu_source_direct_operation_identity();
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
        prepared_target.resident_cache =
            std::allocate_shared<bioseq::indexing::gpu_minimizer_index_cache>(
                ::bioseq::simd::admitted_alignment_node_allocator<
                    bioseq::indexing::gpu_minimizer_index_cache>{},
                0u);
#else
        prepared_target.resident_cache =
            std::make_shared<bioseq::indexing::gpu_minimizer_index_cache>(0u);
#endif
        prepared_target.persisted = false;
    }
    if (!prepared_target.source_ready() || prepared_target.target_rowno !=
                                               scanner.target_rownos[row] ||
        prepared_target.target_bp_size != target_bp_size ||
        prepared_target.query_count != 1u) {
        throw std::logic_error{
            "ordinary MPF Vulkan preparation did not publish one exact row owner"};
    }
    scanner.persisted_generation_ids[row] =
        prepared_target.persisted ? prepared_target.persisted_generation_id : 0u;
    scanner.persisted_source_identities[row] =
        prepared_target.persisted ? prepared_target.persisted_source_identity : 0u;
}

/** Prepare one row's MPS target for Vulkan, using a stored index or a new source owner. */
void prepare_ordinary_scan_mps_target(
    void* opaque_processor,
    scanner_context& scanner,
    const std::size_t row,
    const std::uint64_t target_bp_size,
    const std::uint8_t target_alphabet_code,
    const std::uint8_t target_storage_unit_byte_width,
    ::bioseq::simd::pipe::statement_execution_plan_owner execution_plan,
    scanner_context::gpu_mps_target_owner& prepared_target) {
    auto* const processor = static_cast<::bioseq::simd::batch_processor*>(opaque_processor);
    if (processor == nullptr || row >= ::bioseq::simd::dispatch_batch_capacity ||
        target_bp_size == 0u || scanner.target_rownos[row] < 0 ||
        !scanner_plan_matches(scanner, execution_plan)) {
        throw std::logic_error{
            "ordinary MPS Vulkan preparation changed row or statement ownership"};
    }
    const auto target_rowno = static_cast<std::uint64_t>(scanner.target_rownos[row]);
    if (scanner.read_index_active) {
        if (scanner.executor_read_context == nullptr) {
            throw std::logic_error{
                "ordinary persisted MPS Vulkan preparation has no executor read owner"};
        }
        processor->prepare_gpu_persisted_grouped_soa_target(
            scanner.db_oid,
            scanner.tablespace_oid,
            scanner.main_relfilenode,
            scanner.user_relid,
            scanner.mmsearch_seq_attno,
            scanner.active_index,
            target_rowno,
            target_bp_size,
            /*query_count=*/1u,
            target_alphabet_code,
            target_storage_unit_byte_width,
            std::move(execution_plan),
            *scanner.executor_read_context,
            prepared_target);
    } else {
        prepared_target = scanner_context::gpu_mps_target_owner{};
        prepared_target.execution_plan = std::move(execution_plan);
        prepared_target.target_rowno = static_cast<std::int64_t>(target_rowno);
        prepared_target.target_bp_size = target_bp_size;
        prepared_target.query_count = 1u;
        prepared_target.operation_identity =
            bioseq::indexing::next_gpu_source_direct_operation_identity();
        prepared_target.persisted = false;
    }
    const bool exact_source = prepared_target.source_ready() &&
                              prepared_target.target_rowno == scanner.target_rownos[row] &&
                              prepared_target.target_bp_size == target_bp_size &&
                              prepared_target.query_count == 1u;
    if (!exact_source || (scanner.read_index_active && !prepared_target.ready())) {
        throw std::logic_error{
            "ordinary MPS Vulkan preparation did not publish one exact row owner"};
    }
    scanner.persisted_generation_ids[row] =
        prepared_target.persisted ? prepared_target.resident_view.generation_id : 0u;
    scanner.persisted_source_identities[row] =
        prepared_target.persisted ? prepared_target.resident_view.source_identity : 0u;
}

} // namespace

/**
 * @brief Run the scan batch and report completion to the scheduler.
 * Preserve PostgreSQL and C++ errors for the executor to handle.
 * @return Whether mmsearch ran on a nonempty buffer, regardless of matches.
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
    /* Report only this batch. Its waves and extra queries add to these
     * counters before the executor merges them into the statement totals. */
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

/**
 * @brief Align each group of rows and build cursors for surviving matches.
 * Extra constant queries act as filters. Matches keep the primary query
 * results used by SQL projection.
 */
void tuple_dispatch_buffer::run_batch_and_build_cursors_impl() {
    /* Group flat-payload test rows by the available memory budget. If a group
     * cannot reserve its estimated memory, keep its locators for deferred reads.
     * Production scans read through their source provider. */
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
     * Use each row's buffer position as its result key. A primary key may be
     * absent from the scan projection, so it cannot identify every input row.
     * Buffer positions keep those rows separate even when no primary key
     * column was captured.
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
    if (!execution_plan) {
        throw std::logic_error{"scan execution has no immutable statement plan"};
    }
    // Later dispatch uses this same statement plan to select the seed algorithm.
    /* Vulkan reads deferred inputs through lazy sequence views and prepares
     * its own packed uploads. Only the test baseline needs flat payloads here. */
    const bool requires_flat_sequence_payloads = forced_flat_baseline;

    std::size_t lpu_for_hydration = 0;
    std::size_t bytes_per_unit_for_hydration = 0;
    bioseq::pg_types::seq::dispatch_instance(
        mmsearch_state.alphabet_code,
        mmsearch_state.storage_unit_byte_width,
        /** @brief Get the target packing sizes used to estimate full-payload memory. */
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
            /** @brief Get the query packing sizes, which may differ from the target width. */
            [&query_lpu_for_hydration,
             &query_bytes_per_unit_for_hydration]<class p_sequence>() noexcept {
                using traits = typename compact_sequence_range<p_sequence>::traits;
                query_lpu_for_hydration = traits::letters_per_unit;
                query_bytes_per_unit_for_hydration = sizeof(typename traits::storage_unit);
            });
    }
    /**
     * @brief Estimate the bytes needed for one complete PostgreSQL sequence value.
     * Return zero for invalid packing sizes, overflow, or a value beyond MaxAllocSize.
     */
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
    /**
     * @brief Estimate a row's payload memory for the flat-sequence test route.
     * Include a second query copy when its storage width needs conversion.
     * Return zero when this route does not need full payloads.
     */
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
    /** @brief Fill dispatch arrays with the active rows' sequence pointers and buffer positions. */
    auto refresh_row_arrays =
        [this, &target_hdrs, &per_row_query_hdrs, &query_ids](std::span<const size_t> rows) {
            for (const size_t row_index : rows) {
                if (row_index >= buffered_entries.size() || row_index >= dispatch_batch_capacity) {
                    continue;
                }
                const buffered_scan_entry& entry = buffered_entries[row_index];
                target_hdrs[row_index] = entry.seq_hdr(); // Null if the sequence is NULL or still deferred.
                per_row_query_hdrs[row_index] = entry.per_row_query_hdr();
                query_ids[row_index] = static_cast<int32_t>(row_index);
            }
        };

    std::vector<std::pair<size_t, size_t>> waves;
    const std::uint64_t budget = bioseq::parallelism::memory_admission_controller::instance()
                                     .allocatable_after_reservations();
    std::uint64_t wave_bytes = 0;
    size_t wave_begin = 0;
    for (size_t row_index = 0; row_index < batch_size; ++row_index) {
        const std::uint64_t row_bytes = estimate_hydration_bytes(row_index);
        if (requires_flat_sequence_payloads &&
            row_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
            ereport(
                ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("selected dispatch cannot materialize sequence recipe"),
                 errdetail("row=%zu requires an invalid or oversized flat payload", row_index)));
        }
        const bool split = budget != (std::numeric_limits<std::uint64_t>::max)() &&
                           wave_bytes != 0 && row_bytes > 0 &&
                           row_bytes > budget - std::min(wave_bytes, budget);
        if (split) {
            waves.emplace_back(wave_begin, row_index);
            wave_begin = row_index;
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

    // Keep PostgreSQL storage-read state in the batch context while alignment runs.
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
    scanner_pg_ctx.executor_read_context =
        executor_read_context.has_value() ? std::addressof(*executor_read_context) : nullptr;
    scanner_pg_ctx.source_provider = source_provider;
    scanner_pg_ctx.persisted_gpu_operation_stats_sink = persisted_gpu_operation_stats_sink;
    scanner_pg_ctx.downstream_active_wall = downstream_active_wall_tracker;
    if (execution_plan->uses_vulkan_seed()) {
        scanner_pg_ctx.execution_plan = execution_plan;
        scanner_pg_ctx.ordinary_vulkan_target_preparation = {
            .state = std::addressof(processor),
            .prepare_mpf = &prepare_ordinary_scan_mpf_target,
            .prepare_mps = &prepare_ordinary_scan_mps_target,
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            .map_persisted_read_stats = &merge_generation_read_stats,
#endif
        };
    }
    // Pass locators and saved slice wrappers to the scanner so it can read each
    // operand through its source provider.
    for (size_t row_index = 0; row_index < batch_size; ++row_index) {
        const auto& seq_loc_opt = buffered_entries[row_index].seq_loc;
        if (seq_loc_opt.has_value()) {
            scanner_pg_ctx.target_locators[row_index] = *seq_loc_opt;
        }
        const auto& query_loc_opt = buffered_entries[row_index].per_row_query_loc;
        if (query_loc_opt.has_value()) {
            scanner_pg_ctx.query_locators[row_index] = *query_loc_opt;
        }
        if (buffered_entries[row_index].has_seq_recipe()) {
            scanner_pg_ctx.target_recipe_datums[row_index] =
                buffered_entries[row_index].seq_recipe_datum;
            scanner_pg_ctx.target_recipe_descriptors[row_index] =
                buffered_entries[row_index].seq_recipe_descriptor;
        }
        if (buffered_entries[row_index].has_per_row_query_recipe()) {
            scanner_pg_ctx.query_recipe_datums[row_index] =
                buffered_entries[row_index].per_row_query_recipe_datum;
            scanner_pg_ctx.query_recipe_descriptors[row_index] =
                buffered_entries[row_index].per_row_query_recipe_descriptor;
        }
        // Identify the stored index belonging to this physical sequence row.
        scanner_pg_ctx.target_rownos[row_index] = buffered_entries[row_index].sequence_rowno;
    }
    scanner_pg_ctx.user_relid = user_relid;
    scanner_pg_ctx.index_read_requirement = index_read_requirement;
    scanner_pg_ctx.read_index_active = read_sequence_index_active;
    scanner_pg_ctx.index_async_persist_active = bioseq::settings::index::async_persistence::enabled;
    scanner_pg_ctx.recompute_async_persist_active =
        bioseq::settings::index::recompute_async_persistence::enabled;
    // Resolve the column's index layout once for this batch. The selected MPF
    // or MPS layout also controls temporary indexes when stored payload reads
    // are disabled. Reading this policy only reads catalog metadata.
    apply_active_read_policy_resolution(
        scanner_pg_ctx,
        bioseq::storage::indexing::resolve_active_read_index_outcome(
            *executor_read_context, static_cast<std::int16_t>(mmsearch_state.seq_attno)),
        user_relid,
        user_main_relfilenode,
        static_cast<std::int16_t>(mmsearch_state.seq_attno));
    /** @brief Clear dispatch pointers and IDs so inactive rows cannot enter the next group. */
    auto reset_dispatch_arrays = [&target_hdrs, &per_row_query_hdrs, &query_ids] {
        target_hdrs.fill(nullptr);
        per_row_query_hdrs.fill(nullptr);
        query_ids.fill(-1);
    };
    /** @brief Clear source references and index identities before installing the next group. */
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
        scanner_pg_ctx.persisted_generation_ids.fill(0u);
        scanner_pg_ctx.persisted_source_identities.fill(0u);
    };
    /** @brief Give the scanner source references for the rows in this group. */
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
    /** @brief Total the group's payload estimate, returning UINT64_MAX on overflow. */
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
    /** @brief Check whether this group needs a sequence read session for locators or slices. */
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
    /** @brief Add completed alignment counters to this batch's totals. */
    auto merge_stats =
        [this](const ::bioseq::pipeline_stats::pipeline_execution_stats& wave_stats) {
            last_stats.merge(wave_stats);
        };

    match_map.clear();
    // Pass this storage context to each alignment call.
    auto* scanner_pg_arg = &scanner_pg_ctx;
    ::abort_pg_sequence_read_session_noexcept(std::exchange(active_wave_read_session, nullptr));
    active_wave_permit.reset();
    if (copy_staging_slot != nullptr) {
        ExecDropSingleTupleTableSlot(copy_staging_slot);
        copy_staging_slot = nullptr;
    }
    /** @brief Forget full-payload pointers before releasing this group's memory permit. */
    const auto clear_active_wave_member_handles_noexcept = [this]() noexcept {
        for (auto& entry : buffered_entries) {
            entry.hydrated_seq_hdr = nullptr;
            entry.hydrated_per_row_query_hdr = nullptr;
        }
        hydrated_target_payloads.clear();
        hydrated_query_payloads.clear();
    };
    /** @brief Abort reads and release this group's memory permit after an error. */
    const auto release_active_wave_after_error_noexcept =
        [this,
         &clear_active_wave_member_handles_noexcept,
         &scanner_pg_ctx,
         &target_hdrs,
         &per_row_query_hdrs]() noexcept {
            scanner_pg_ctx.read_session = nullptr;
            ::abort_pg_sequence_read_session_noexcept(
                std::exchange(active_wave_read_session, nullptr));
            clear_active_wave_member_handles_noexcept();
            target_hdrs.fill(nullptr);
            per_row_query_hdrs.fill(nullptr);
            active_wave_permit.reset();
    };
    /** @brief Require reads to be finished before clearing pointers and releasing the permit. */
    const auto release_active_wave_checked =
        [this,
         &clear_active_wave_member_handles_noexcept,
         &target_hdrs,
         &per_row_query_hdrs]() {
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
    /**
     * @brief Abort active sequence reads and discard scanner cache references after an error.
     * PostgreSQL error cleanup reclaims the detached cache handles.
     */
    const auto abandon_scanner_cache_noexcept =
        [this, &release_active_wave_after_error_noexcept, &scanner_pg_cache]() noexcept {
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
                ::bioseq::simd::match_result_map wave_match_map;
                if (per_row_mode()) {
                    wave_match_map =
                        processor.process_sequences(batch_size,
                                                    mmsearch_state.alphabet_code,
                                                    mmsearch_state.storage_unit_byte_width,
                                                    nullptr,
                                                    &per_row_query_hdrs,
                                                    target_hdrs,
                                                    query_ids,
                                                    execution_plan,
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
                                                    execution_plan,
                                                    ::bioseq::simd::batch_invocation_role::primary,
                                                    0,
                                                    wave_stats,
                                                    scanner_pg_arg);
                }
                merge_stats(wave_stats);

                /* Run each extra mmsearch query over these rows and keep only rows
                 * that also match it. Preserve the primary query's match data. */
                for (size_t extra_query_index = 0; extra_query_index < extra_query_varlenas.size();
                     ++extra_query_index) {
                    const std::vector<char>& extra_bytes = extra_query_varlenas[extra_query_index];
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
                        execution_plan,
                        ::bioseq::simd::batch_invocation_role::extra_query,
                        extra_broadcast_query_ordinal(
                            per_row_mode(), batch_size, extra_query_index),
                        extra_stats,
                        scanner_pg_arg);
                    merge_stats(extra_stats);
                    bioseq_log<logging_level::debug>(
                        "[bioseq] pg: tuple_dispatch_buffer extra-query #{} wave [{}, {}) produced "
                        "matches for {} entries",
                        extra_query_index,
                        wave.first,
                        wave.second,
                        extra_match_map.size());
                    for (auto match_entry = wave_match_map.begin();
                         match_entry != wave_match_map.end();) {
                        if (extra_match_map.find(match_entry->first) == extra_match_map.end()) {
                            match_entry = wave_match_map.erase(match_entry);
                        } else {
                            ++match_entry;
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

            // All alignment calls have finished. Close the storage relations and
            // read states, then free this batch's cache.
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
    for (size_t entry_index = 0; entry_index < buffered_entries.size(); ++entry_index) {
        const buffered_scan_entry& entry = buffered_entries[entry_index];
        /* A row absent from the match map produced no accepted match, so it
         * contributes no output for WHERE mmsearch(...). */
        auto matches_for_entry = match_map.find(static_cast<int32_t>(entry_index));
        if (matches_for_entry == match_map.end()) {
            bioseq_log<logging_level::debug>(
                "[bioseq] pg: batch miss for entry index={}; WHERE mmsearch evaluates to FALSE",
                entry_index);
            continue;
        }
        /*
         * When base alignment was requested, require a nonempty CIGAR before
         * returning the match to SQL. Otherwise a chain with no usable base
         * alignment could produce a row with empty match fields. With do_cigar
         * disabled, keep the matches produced by chaining.
         */
        for (size_t match_index = 0; match_index < matches_for_entry->second.size();
             ++match_index) {
            const ::bioseq::simd::match_result& match_candidate =
                matches_for_entry->second[match_index];
            if (base_aligner_params().do_cigar &&
                (!match_candidate.has_cigar || match_candidate.cigar.empty())) {
                bioseq_log<logging_level::debug>(
                    "[bioseq] pg: batch skip: degenerate match for entry index={} cursor m={} "
                    "(do_cigar=true but cigar empty)",
                    entry_index,
                    match_index);
                continue;
            }
            output_cursors.push_back({entry_index, match_index});
        }
    }
    output_position = 0;
    bioseq_log<logging_level::debug>(
        "[bioseq] pg: batch produced {} output tuples from {} scan tuples",
        output_cursors.size(),
        buffered_entries.size());
}

} // namespace bioseq::scheduler
