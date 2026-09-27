/**
 * @file index_parts/join_preparation.cppm
 * @brief Keep a join target and prepare it for reuse across query batches.
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

// A batch join keeps one locator-backed  target while processing its queries.
module bioseq.postgres.scheduler.dispatch_buffer:join_preparation;

import bioseq.postgres.scheduler.dispatch_buffer;

import bioseq.common.logging;
import bioseq.postgres.support;
import bioseq.postgres.support.datums;
import bioseq.indexing.type;
import bioseq.indexing.gpu_minimizer_index;
import bioseq.postgres.storage.indexing.active_read_policy_resolver;
import bioseq.postgres.storage.indexing.cold_index_timing;
import bioseq.postgres.scheduler.dispatch_processor; // Keep the alignment pipeline in a separate module.
import bioseq.sequences.compact_sequence_typmod;
import bioseq.sequences.simd;
import bioseq.pipeline.minimap_port;
import bioseq.pipeline.pipe;
import bioseq.postgres.types.compact_sequence;
import bioseq.postgres.storage.manager;

namespace bioseq::scheduler {

/**
 * @brief Save one locator-backed target row for the next join pass.
 * Require its physical row number and clear preparation left by the previous
 * target. The copied row remains available while query batches run.
 */
void tuple_dispatch_buffer::begin_join_target(TupleTableSlot* target_slot) {
    if (!join_mode || target_slot == nullptr || !join_queries.empty()) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq batch join target lifecycle is invalid")));
    }
    join_active_read_policy.reset();
    join_cold_policy_stats = {};
    bool sequence_is_null = true;
    const Datum sequence_datum =
        slot_getattr(target_slot, join_target_slot_seq_resno, &sequence_is_null);
    if (sequence_is_null) {
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bioseq batch join locator target sequence is NULL")));
    }
    auto target_sequence = bioseq::postgres::support::detoast_as<bioseq::pg_types::seq>(
        bioseq::postgres::support::pg_datum::from_raw(sequence_datum));
    const bool is_locator = bioseq::storage::is_locator_stub(target_sequence.ptr);
    target_sequence.free_if_copy();
    if (!is_locator) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq batch join target provider did not return a locator stub"),
                 errdetail("locator passthrough is mandatory; materialized target values are not "
                           "accepted by this executor")));
    }
    clear_slots();
    submit_scan(target_slot);
    if (buffered_entries.size() != 1) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq batch join failed to retain exactly one target")));
    }
    bool rowno_is_null = true;
    const Datum rowno_datum =
        slot_getattr(target_slot, join_target_slot_rowno_resno, &rowno_is_null);
    if (rowno_is_null) {
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bioseq batch join hidden target row number is NULL")));
    }
    const std::int64_t rowno = DatumGetInt64(rowno_datum);
    if (rowno < 0) {
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bioseq batch join hidden target row number is negative")));
    }
    buffered_entries.front().sequence_rowno = rowno;
    join_target_active = true;
    join_target_prepared = false;
    join_pending_cpu_preparation_queries.clear();
    join_cpu_preparation_pending = false;
    join_cpu_preparation_scratch.clear_storage_noexcept();
    join_target_outcomes_recorded = false;
    join_prepared_union_outcomes_recorded = false;
    join_prepared_build_checksum_recorded = false;
    join_cpu_prepared_target =
        std::make_shared<::bioseq::simd::batch_processor::cpu_prepared_target>();
    join_cpu_prepared_target->target_rowno = rowno;
    join_gpu_prepared_mpf_target =
        ::bioseq::simd::batch_processor::gpu_prepared_mpf_target{};
    join_gpu_prepared_mpf_target.target_rowno = rowno;
    join_gpu_prepared_grouped_soa_target =
        ::bioseq::simd::batch_processor::gpu_prepared_grouped_soa_target{};
    join_gpu_prepared_grouped_soa_target.target_rowno = rowno;
}

/** @brief Forward the post-selection Highway callback to its owning oin buffer */
void tuple_dispatch_buffer::prepare_join_cpu_after_highway_selection_thunk(
    void* state,
    ::bioseq::simd::batch_processor::scanner_pg_context& scanner_pg_ctx,
    ::bioseq::simd::pipe::statement_execution_plan_owner dispatched_plan,
    const ::bioseq::simd::batch_processor::cpu_preparation_typmod_operations& typmod_ops) {
    auto* self = static_cast<tuple_dispatch_buffer*>(state);
    if (self == nullptr) {
        throw std::logic_error{"bioseq batch join Highway preparation owner is null"};
    }
    self->prepare_join_cpu_after_highway_selection(scanner_pg_ctx, dispatched_plan, typmod_ops);
}

/**
 * @brief Prepare the shared CPU target  after Highway has selected its kernels.
 * Read the saved query slots and combine their minimizers. Open the required
 * stored index when requested, and keep the result for later query batches.
 * PostgreSQL and C++ failures leave through the same caller cleanup path.
 */
void tuple_dispatch_buffer::prepare_join_cpu_after_highway_selection(
    ::bioseq::simd::batch_processor::scanner_pg_context& scanner_pg_ctx,
    ::bioseq::simd::pipe::statement_execution_plan_owner dispatched_plan,
    const ::bioseq::simd::batch_processor::cpu_preparation_typmod_operations& typmod_ops) {
    if (!execution_plan || !dispatched_plan ||
        execution_plan.get() != dispatched_plan.get() ||
        execution_plan.owner_before(dispatched_plan) ||
        dispatched_plan.owner_before(execution_plan)) {
        throw std::logic_error{"bioseq batch join preparation changed its statement owner"};
    }
    if (!join_cpu_preparation_pending || join_pending_cpu_preparation_queries.empty() ||
        !join_target_active || !join_target_prepared ||
        (!read_sequence_index_active &&
         index_read_requirement !=
             bioseq::indexing::index_read_requirement::require_absent) ||
        !join_active_read_policy.has_value() || execution_plan->uses_vulkan_seed()) {
        throw std::logic_error{"bioseq batch join deferred CPU preparation state is invalid"};
    }
    if (scanner_pg_ctx.cpu_prepared_target != nullptr || scanner_pg_ctx.read_session == nullptr ||
        scanner_pg_ctx.read_session != active_wave_read_session) {
        throw std::logic_error{"bioseq batch join deferred CPU preparation has no wave session"};
    }
    if (!join_cpu_preparation_scratch.empty()) {
        throw std::logic_error{"bioseq batch join retained stale CPU preparation scratch"};
    }

    join_cpu_preparation_scratch.rewidthed_query_varlenas.resize(
        join_pending_cpu_preparation_queries.size());
    join_cpu_preparation_scratch.query_headers.reserve(join_pending_cpu_preparation_queries.size());
    join_cpu_preparation_scratch.query_recipe_datums.reserve(
        join_pending_cpu_preparation_queries.size());
    join_cpu_preparation_scratch.query_recipe_descriptors.reserve(
        join_pending_cpu_preparation_queries.size());
    join_cpu_preparation_scratch.query_ordinals.reserve(
        join_pending_cpu_preparation_queries.size());
    join_cpu_preparation_scratch.detoast_copies.reserve(
        join_pending_cpu_preparation_queries.size());

    MemoryContext saved_context = CurrentMemoryContext;
    ErrorData* volatile pg_error = nullptr;
    PG_TRY();
    {
        try {
            std::uint64_t previous_ordinal = 0;
            bool first = true;
            for (const join_query_ref& query : join_pending_cpu_preparation_queries) {
                if (query.query_slot == nullptr ||
                    query.ordinal >
                        static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)()) ||
                    (!first && query.ordinal <= previous_ordinal)) {
                    ereport(
                        ERROR,
                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                         errmsg("bioseq batch join query references are invalid or unordered")));
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
                    join_cpu_preparation_scratch.query_headers.push_back(nullptr);
                    join_cpu_preparation_scratch.query_recipe_datums.push_back(query_datum);
                    join_cpu_preparation_scratch.query_recipe_descriptors.push_back(descriptor);
                    join_cpu_preparation_scratch.query_ordinals.push_back(query.ordinal);
                    previous_ordinal = query.ordinal;
                    first = false;
                    continue;
                }
                bioseq::pg_types::seq* query_header =
                    reinterpret_cast<bioseq::pg_types::seq*>(PG_DETOAST_DATUM(query_datum));
                if (bioseq::storage::is_locator_stub(query_header)) {
                    if (reinterpret_cast<void*>(query_header) !=
                        reinterpret_cast<void*>(DatumGetPointer(query_datum))) {
                        pfree(query_header);
                    }
                    ereport(
                        ERROR,
                        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                         errmsg("bioseq batch join query preparation received a locator stub")));
                }
                if (query_header->alphabet_code != mmsearch_state.query_alphabet_code ||
                    query_header->storage_unit_byte_width !=
                        mmsearch_state.query_storage_unit_byte_width) {
                    if (reinterpret_cast<void*>(query_header) !=
                        reinterpret_cast<void*>(DatumGetPointer(query_datum))) {
                        pfree(query_header);
                    }
                    ereport(ERROR,
                            (errcode(ERRCODE_DATATYPE_MISMATCH),
                             errmsg("bioseq batch join query typmod changed during preparation")));
                }
                const bioseq::pg_types::seq* preparation_query_header = query_header;
                if (query_header->storage_unit_byte_width !=
                    mmsearch_state.storage_unit_byte_width) {
                    bioseq::pg_types::seq* rewidthed = ::bioseq::simd::stdc_rewidth_to_type_params(
                        *query_header,
                        compact_sequence_stdc_type_params{mmsearch_state.alphabet_code,
                                                          mmsearch_state.storage_unit_byte_width},
                        pg_alloc);
                    if (rewidthed == nullptr) {
                        if (reinterpret_cast<void*>(query_header) !=
                            reinterpret_cast<void*>(DatumGetPointer(query_datum))) {
                            pfree(query_header);
                        }
                        ereport(ERROR,
                                (errcode(ERRCODE_DATATYPE_MISMATCH),
                                 errmsg("bioseq batch join query preparation rewidth rejected an "
                                        "incompatible alphabet")));
                    }
                    const std::size_t rewidthed_bytes =
                        static_cast<std::size_t>(bioseq::postgres::support::varlena_4byte_size(rewidthed));
                    auto& owned = join_cpu_preparation_scratch.rewidthed_query_varlenas
                                      [join_cpu_preparation_scratch.query_headers.size()];
                    owned.assign(reinterpret_cast<const char*>(rewidthed),
                                 reinterpret_cast<const char*>(rewidthed) + rewidthed_bytes);
                    pfree(rewidthed);
                    preparation_query_header =
                        reinterpret_cast<const bioseq::pg_types::seq*>(owned.data());
                }
                join_cpu_preparation_scratch.query_headers.push_back(preparation_query_header);
                join_cpu_preparation_scratch.query_recipe_datums.push_back(Datum{0});
                join_cpu_preparation_scratch.query_recipe_descriptors.push_back(
                    pg_sequence_descriptor{});
                join_cpu_preparation_scratch.query_ordinals.push_back(query.ordinal);
                if (reinterpret_cast<void*>(query_header) !=
                    reinterpret_cast<void*>(DatumGetPointer(query_datum))) {
                    join_cpu_preparation_scratch.detoast_copies.push_back(query_header);
                }
                previous_ordinal = query.ordinal;
                first = false;
            }

            if (!join_cpu_prepared_target->bind_highway_dispatch_once(dispatched_plan)) {
                throw std::logic_error{"bioseq batch join rejected its selected Highway binding"};
            }
            if (index_read_requirement ==
                bioseq::indexing::index_read_requirement::require_compatible_persisted) {
                processor.prepare_cpu_persisted_native_target(
                    user_main_database_oid,
                    user_main_tablespace_oid,
                    user_main_relfilenode,
                    user_relid,
                    static_cast<std::int16_t>(join_target_storage_seq_attno),
                    join_active_read_policy->index_name,
                    static_cast<std::uint64_t>(join_cpu_prepared_target->target_rowno),
                    join_cpu_prepared_target->target_bp_size,
                    mmsearch_state.alphabet_code,
                    mmsearch_state.storage_unit_byte_width,
                    join_cpu_preparation_scratch.query_headers,
                    join_cpu_preparation_scratch.query_recipe_datums,
                    join_cpu_preparation_scratch.query_recipe_descriptors,
                    scanner_pg_ctx.read_session,
                    std::addressof(mmsearch_query_payload_observer),
                    join_cpu_preparation_scratch.query_ordinals,
                    dispatched_plan,
                    *executor_read_context,
                    typmod_ops,
                    *join_cpu_prepared_target);
            } else {
                const auto union_storage =
                    index_read_requirement ==
                            bioseq::indexing::index_read_requirement::require_absent
                        ? ::bioseq::simd::batch_processor::cpu_query_union_storage::
                              canonical_hashes
                        : ::bioseq::simd::batch_processor::cpu_query_union_storage::
                              minimizer_sketch;
                processor.prepare_cpu_shared_query_unions(
                    mmsearch_state.alphabet_code,
                    mmsearch_state.storage_unit_byte_width,
                    join_cpu_prepared_target->target_bp_size,
                    join_cpu_preparation_scratch.query_headers,
                    join_cpu_preparation_scratch.query_recipe_datums,
                    join_cpu_preparation_scratch.query_recipe_descriptors,
                    scanner_pg_ctx.read_session,
                    std::addressof(mmsearch_query_payload_observer),
                    join_cpu_preparation_scratch.query_ordinals,
                    dispatched_plan,
                    /*include_persisted_parameter_group=*/true,
                    union_storage,
                    typmod_ops,
                    *join_cpu_prepared_target);
            }
        } catch (...) {
            join_cpu_preparation_scratch.cpp_error = std::current_exception();
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
        join_cpu_preparation_scratch.clear_storage_noexcept();
        ErrorData* exact_error = pg_error;
        throw bioseq::postgres::support::postgres_error{exact_error};
    }
    if (join_cpu_preparation_scratch.cpp_error != nullptr) {
        for (bioseq::pg_types::seq* copy : join_cpu_preparation_scratch.detoast_copies) {
            bioseq::postgres::support::invoke_pg_as_cpp([copy] { pfree(copy); });
        }
        std::exception_ptr exact_error =
            std::exchange(join_cpu_preparation_scratch.cpp_error, std::exception_ptr{});
        join_cpu_preparation_scratch.clear_storage_noexcept();
        std::rethrow_exception(exact_error);
    }
    for (bioseq::pg_types::seq* copy : join_cpu_preparation_scratch.detoast_copies) {
        bioseq::postgres::support::invoke_pg_as_cpp([copy] { pfree(copy); });
    }
    join_cpu_preparation_scratch.clear_storage_noexcept();
    if (!join_cpu_prepared_target->unions_ready ||
        !join_cpu_prepared_target->highway_binding_matches(dispatched_plan)) {
        throw std::logic_error{
            "bioseq batch join CPU preparation did not retain its Highway binding"};
    }

    join_pending_cpu_preparation_queries.clear();
    join_cpu_preparation_pending = false;
    scanner_pg_ctx.cpu_prepared_target = join_cpu_prepared_target;
}

/**
 * @brief Resolve the target's index policy and prepare it for all queries.
 * CPU sketching waits for Highway selection. Vulkan prepares its shared
 * target source here and sketches queries later in the selected GPU route.
 */
void tuple_dispatch_buffer::prepare_join_target_queries(
    std::span<const join_query_ref> all_queries) {
    if (!join_target_active || join_target_prepared) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq batch join target preparation is out of order")));
    }
    if (join_cpu_preparation_pending || !join_pending_cpu_preparation_queries.empty()) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq batch join retained stale CPU preparation inputs")));
    }
    if (join_active_read_policy.has_value()) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq batch join active-read policy was already resolved")));
    }
    const std::uint64_t policy_begin_ns = bioseq::storage::indexing::cold_index_now_ns();
    const BufferUsage policy_buffer_begin = pgBufferUsage;
    if (!executor_read_context.has_value() || !executor_read_context->valid()) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq batch join borrowed executor context is unavailable")));
    }
    join_active_read_policy.emplace(bioseq::storage::indexing::resolve_active_read_index_outcome(
        *executor_read_context,
        static_cast<std::int16_t>(join_target_storage_seq_attno),
        &join_cold_policy_stats));
    bioseq::storage::indexing::add_cold_index_elapsed(
        join_cold_policy_stats, join_cold_policy_stats.policy_ns, policy_begin_ns);
    bioseq::storage::indexing::add_cold_index_buffer_usage(join_cold_policy_stats,
                                                           policy_buffer_begin);
    if (!execution_plan) {
        throw std::logic_error{"join target preparation has no statement plan"};
    }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    const auto attest_join_preparation = [this] {
        auto* const plan_tracker = execution_plan->frozen_parameters.downstream_active_wall;
        if (plan_tracker == nullptr || plan_tracker != downstream_active_wall_tracker ||
            !plan_tracker->attest_benchmark_e2e_statement_plan_checkpoint(
                execution_plan,
                ::bioseq::simd::benchmark_e2e_statement_plan_checkpoint::join_preparation)) {
            throw std::logic_error{
                "bioseq batch join preparation changed its guarded statement-plan owner"};
        }
    };
#endif
    const bool vulkan_seed_requested = execution_plan->uses_vulkan_seed();
    const bool active_mpf =
        join_active_read_policy->index_name ==
        bioseq::indexing::k_minimizers_per_fragment_index_name;
    const bool active_mps =
        join_active_read_policy->index_name ==
        bioseq::indexing::k_minimizers_per_sequence_index_name;
    if (!active_mpf && !active_mps) {
        throw std::logic_error{"bioseq batch join selected an unknown target index layout"};
    }
    const bool prepared_host_target_requested =
        !vulkan_seed_requested &&
        (read_sequence_index_active ||
         index_read_requirement ==
             bioseq::indexing::index_read_requirement::require_absent);
    if (prepared_host_target_requested) {
        std::uint64_t previous_ordinal = 0;
        bool first = true;
        for (const join_query_ref& query : all_queries) {
            if (query.query_slot == nullptr ||
                query.ordinal >
                    static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)()) ||
                (!first && query.ordinal <= previous_ordinal)) {
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                         errmsg("bioseq batch join query references are invalid or unordered")));
            }
            previous_ordinal = query.ordinal;
            first = false;
        }
        const std::uint64_t target_bp_size = buffered_entries.front().seq_bp_size();
        join_cpu_prepared_target->target_bp_size = target_bp_size;
        join_cpu_prepared_target->query_count = all_queries.size();
        join_cpu_prepared_target->unions_ready = false;
        join_pending_cpu_preparation_queries.assign(all_queries.begin(), all_queries.end());
        join_cpu_preparation_pending = !join_pending_cpu_preparation_queries.empty();
        join_target_prepared = true;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        attest_join_preparation();
#endif
        return;
    }
    /* Vulkan will sketch these queries after selecting the generated kernel.
     * Check references now  and leave sequence reads and, conversion to that route. */
    if (vulkan_seed_requested) {
        if (mmsearch_state.storage_unit_byte_width != 4u &&
            mmsearch_state.storage_unit_byte_width != 8u) {
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("bioseq Vulkan seed route has no generated physical record for "
                            "the selected packed storage width")));
        }
        std::uint64_t previous_ordinal = 0u;
        bool first = true;
        for (const join_query_ref& query : all_queries) {
            if (query.query_slot == nullptr ||
                query.ordinal >
                    static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)()) ||
                (!first && query.ordinal <= previous_ordinal)) {
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                         errmsg("bioseq batch join Vulkan query references are invalid or unordered")));
            }
            previous_ordinal = query.ordinal;
            first = false;
        }
        const buffered_scan_entry& target = buffered_entries.front();
        const std::uint64_t target_bp_size = target.seq_bp_size();
        join_cpu_prepared_target->target_bp_size = target_bp_size;
        join_cpu_prepared_target->query_count = all_queries.size();
        join_cpu_prepared_target->unions_ready = false;

        if (read_sequence_index_active) {
            MemoryContext saved_context = CurrentMemoryContext;
            ErrorData* volatile pg_error = nullptr;
            std::exception_ptr cpp_error;
            PG_TRY();
            {
                try {
                    if (active_mpf) {
                        processor.prepare_gpu_persisted_mpf_target(
                            user_main_database_oid,
                            user_main_tablespace_oid,
                            user_main_relfilenode,
                            user_relid,
                            static_cast<std::int16_t>(join_target_storage_seq_attno),
                            join_active_read_policy->index_name,
                            static_cast<std::uint64_t>(join_gpu_prepared_mpf_target.target_rowno),
                            target_bp_size,
                            all_queries.size(),
                            mmsearch_state.alphabet_code,
                            mmsearch_state.storage_unit_byte_width,
                            execution_plan,
                            *executor_read_context,
                            join_gpu_prepared_mpf_target);
                    } else if (active_mps) {
                        processor.prepare_gpu_persisted_grouped_soa_target(
                            user_main_database_oid,
                            user_main_tablespace_oid,
                            user_main_relfilenode,
                            user_relid,
                            static_cast<std::int16_t>(join_target_storage_seq_attno),
                            join_active_read_policy->index_name,
                            static_cast<std::uint64_t>(
                                join_gpu_prepared_grouped_soa_target.target_rowno),
                            target_bp_size,
                            all_queries.size(),
                            mmsearch_state.alphabet_code,
                            mmsearch_state.storage_unit_byte_width,
                            execution_plan,
                            *executor_read_context,
                            join_gpu_prepared_grouped_soa_target);
                    } else {
                        throw std::logic_error{
                            "bioseq batch join Vulkan preparation escaped MPF/MPS selection"};
                    }
                } catch (...) {
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
                ErrorData* exact_error = pg_error;
                throw bioseq::postgres::support::postgres_error{exact_error};
            }
            if (cpp_error != nullptr) {
                std::rethrow_exception(cpp_error);
            }
            if ((active_mpf && !join_gpu_prepared_mpf_target.source_ready()) ||
                (active_mps && !join_gpu_prepared_grouped_soa_target.ready())) {
                throw std::logic_error{
                    "bioseq batch join persisted Vulkan target did not retain its owner"};
            }
        } else if (active_mpf) {
            join_gpu_prepared_mpf_target.execution_plan = execution_plan;
            join_gpu_prepared_mpf_target.target_bp_size = target_bp_size;
            join_gpu_prepared_mpf_target.query_count = all_queries.size();
            join_gpu_prepared_mpf_target.operation_identity =
                bioseq::indexing::next_gpu_source_direct_operation_identity();
            join_gpu_prepared_mpf_target.resident_cache =
                std::make_shared<bioseq::indexing::gpu_minimizer_index_cache>(0u);
            if (!join_gpu_prepared_mpf_target.source_ready()) {
                throw std::logic_error{
                    "bioseq batch join no-index MPF source owner is invalid"};
            }
        } else {
            join_gpu_prepared_grouped_soa_target.execution_plan = execution_plan;
            join_gpu_prepared_grouped_soa_target.target_bp_size = target_bp_size;
            join_gpu_prepared_grouped_soa_target.query_count = all_queries.size();
            join_gpu_prepared_grouped_soa_target.operation_identity =
                bioseq::indexing::next_gpu_source_direct_operation_identity();
            join_gpu_prepared_grouped_soa_target.persisted = false;
            if (!join_gpu_prepared_grouped_soa_target.source_ready()) {
                throw std::logic_error{
                    "bioseq batch join no-index MPS source owner is invalid"};
            }
        }
        join_target_prepared = true;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        attest_join_preparation();
#endif
        return;
    }
    if (read_sequence_index_active) {
        throw std::logic_error{
            "bioseq batch join persisted CPU preparation escaped its early owner path"};
    }
    std::vector<const bioseq::pg_types::seq*> query_headers;
    std::vector<Datum> query_recipe_datums;
    std::vector<pg_sequence_descriptor> query_recipe_descriptors;
    std::vector<std::uint64_t> query_ordinals;
    std::vector<bioseq::pg_types::seq*> detoast_copies;
    std::vector<std::vector<char>> rewidthed_query_varlenas(all_queries.size());
    query_headers.reserve(all_queries.size());
    query_recipe_datums.reserve(all_queries.size());
    query_recipe_descriptors.reserve(all_queries.size());
    query_ordinals.reserve(all_queries.size());
    detoast_copies.reserve(all_queries.size());
    std::uint64_t previous_ordinal = 0;
    bool first = true;
    for (const join_query_ref& query : all_queries) {
        if (query.query_slot == nullptr ||
            query.ordinal >
                static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)()) ||
            (!first && query.ordinal <= previous_ordinal)) {
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("bioseq batch join query references are invalid or unordered")));
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
            const pg_sequence_descriptor descriptor = classify_and_describe_sequence(query_datum);
            require_recipe_descriptor_(descriptor,
                                       mmsearch_state.query_alphabet_code,
                                       mmsearch_state.query_storage_unit_byte_width,
                                       "query");
            query_headers.push_back(nullptr);
            query_recipe_datums.push_back(query_datum);
            query_recipe_descriptors.push_back(descriptor);
            query_ordinals.push_back(query.ordinal);
            previous_ordinal = query.ordinal;
            first = false;
            continue;
        }
        bioseq::pg_types::seq* query_header =
            reinterpret_cast<bioseq::pg_types::seq*>(PG_DETOAST_DATUM(query_datum));
        if (bioseq::storage::is_locator_stub(query_header)) {
            if (reinterpret_cast<void*>(query_header) !=
                reinterpret_cast<void*>(DatumGetPointer(query_datum))) {
                pfree(query_header);
            }
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("bioseq batch join query preparation received a locator stub")));
        }
        if (query_header->alphabet_code != mmsearch_state.query_alphabet_code ||
            query_header->storage_unit_byte_width != mmsearch_state.query_storage_unit_byte_width) {
            if (reinterpret_cast<void*>(query_header) !=
                reinterpret_cast<void*>(DatumGetPointer(query_datum))) {
                pfree(query_header);
            }
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("bioseq batch join query typmod changed during preparation")));
        }
        const bioseq::pg_types::seq* preparation_query_header = query_header;
        if (query_header->storage_unit_byte_width != mmsearch_state.storage_unit_byte_width) {
            bioseq::pg_types::seq* rewidthed = ::bioseq::simd::stdc_rewidth_to_type_params(
                *query_header,
                compact_sequence_stdc_type_params{mmsearch_state.alphabet_code,
                                                  mmsearch_state.storage_unit_byte_width},
                pg_alloc);
            if (rewidthed == nullptr) {
                if (reinterpret_cast<void*>(query_header) !=
                    reinterpret_cast<void*>(DatumGetPointer(query_datum))) {
                    pfree(query_header);
                }
                ereport(ERROR,
                        (errcode(ERRCODE_DATATYPE_MISMATCH),
                         errmsg("bioseq batch join query preparation rewidth rejected an "
                                "incompatible alphabet")));
            }
            const std::size_t rewidthed_bytes = static_cast<std::size_t>(bioseq::postgres::support::varlena_4byte_size(rewidthed));
            auto& owned = rewidthed_query_varlenas[query_headers.size()];
            owned.assign(reinterpret_cast<const char*>(rewidthed),
                         reinterpret_cast<const char*>(rewidthed) + rewidthed_bytes);
            pfree(rewidthed);
            preparation_query_header = reinterpret_cast<const bioseq::pg_types::seq*>(owned.data());
        }
        query_headers.push_back(preparation_query_header);
        query_recipe_datums.push_back(Datum{0});
        query_recipe_descriptors.push_back(pg_sequence_descriptor{});
        query_ordinals.push_back(query.ordinal);
        if (reinterpret_cast<void*>(query_header) !=
            reinterpret_cast<void*>(DatumGetPointer(query_datum))) {
            detoast_copies.push_back(query_header);
        }
        previous_ordinal = query.ordinal;
        first = false;
    }
    const buffered_scan_entry& target = buffered_entries.front();
    const std::uint64_t target_bp_size = target.seq_bp_size();
    /* Build the temporary CPU target during dispatch so index construction
     * is included in execution timing. Keep only its size and query count here. */
    join_cpu_prepared_target->target_bp_size = target_bp_size;
    join_cpu_prepared_target->query_count = query_headers.size();
    join_cpu_prepared_target->unions_ready = false;
    bioseq_log<logging_level::debug>(
        "[bioseq] pg: batch join no_index retains no prepared target; "
        "the selected backend must use its authentic transient index route");
    for (bioseq::pg_types::seq* copy : detoast_copies) {
        bioseq::postgres::support::invoke_pg_as_cpp([copy] { pfree(copy); });
    }
    join_target_prepared = true;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    attest_join_preparation();
#endif
}

/**
 * @brief Select the next query batch after the previous output is drained.
 * Reject batches larger than dispatch_batch_capacity.
 */
void tuple_dispatch_buffer::set_join_query_batch(std::span<const join_query_ref> queries) {
    if (!join_target_prepared || join_output_position != join_output_cursors.size()) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq batch join query batch lifecycle is invalid")));
    }
    join_output_cursors.clear();
    join_match_map.clear();
    if (queries.size() > dispatch_batch_capacity) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq batch join query batch exceeds dispatch capacity")));
    }
    join_queries.assign(queries.begin(), queries.end());
    join_output_position = 0;
}

/** @brief Use already-converted qery bytes while keeping the original value for EXPLAIN. */
void tuple_dispatch_buffer::apply_typmod_coalesce(std::vector<char> coalesced_bytes) {
    mmsearch_state.coalesced_query_varlena = std::move(coalesced_bytes);
    mmsearch_state.coalesce_active = true;
    const bioseq::pg_types::seq* raw_query =
        mmsearch_state.query_varlena.empty()
            ? nullptr
            : reinterpret_cast<const bioseq::pg_types::seq*>(mmsearch_state.query_varlena.data());
    bioseq_log<logging_level::debug>(
        "[bioseq] pg: tuple_dispatch_buffer::apply_typmod_coalesce() raw=({},{}) coalesced=({},{}) "
        "raw_bytes={} coalesced_bytes={}",
        static_cast<unsigned>(raw_query == nullptr ? mmsearch_state.alphabet_code
                                                   : raw_query->alphabet_code),
        static_cast<unsigned>(raw_query == nullptr ? mmsearch_state.storage_unit_byte_width
                                                   : raw_query->storage_unit_byte_width),
        static_cast<unsigned>(mmsearch_state.alphabet_code),
        static_cast<unsigned>(mmsearch_state.storage_unit_byte_width),
        mmsearch_state.query_varlena.size(),
        mmsearch_state.coalesced_query_varlena.size());
}

} // namespace bioseq::scheduler
