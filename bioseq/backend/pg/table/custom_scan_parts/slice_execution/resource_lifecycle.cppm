/**
 * @file
 * @brief Owns SliceProject DSM attachment, publication, and rescan resources.
 */
module;
extern "C" {
#include "postgres.h"

#include "access/parallel.h"
#include "access/tupdesc.h"
#include "catalog/namespace.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_language_d.h"
#include "catalog/pg_proc.h"
#include "commands/extension.h"
#include "executor/executor.h"
#include "executor/tuptable.h"
#include "nodes/execnodes.h"
#include "nodes/pg_list.h"
#include "nodes/value.h"
#include "optimizer/appendinfo.h"
#include "optimizer/planmain.h"
#include "port/atomics.h"
#include "storage/dsm.h"
#include "storage/shm_toc.h"
#include "storage/shmem.h"
#include "utils/errcodes.h"
#include "utils/expandeddatum.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

#include "rewrite/rewriteManip.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

export module bioseq.postgres.table:custom_scan_slice_execution_resource_lifecycle;

import bioseq.common.checked_arithmetic;
import bioseq.postgres.storage.scanner;
import bioseq.postgres.support;
import bioseq.postgres.table.scan_state;
import bioseq.postgres.types.compact_sequence;

import :custom_scan_execution_method_contracts;
import :custom_scan_scan_runtime_state_contracts;
import :custom_scan_slice_path_codec_contract;
import :custom_scan_slice_path_runtime_contract;

import :custom_scan_slice_execution_dsm_protocol;

using namespace bioseq::postgres::support;

export {
    /** @brief Frees detach state when its refcount drops to zero after a balanced release. */
    void slice_project_release_detach_state(slice_dsm_detach_state* state) noexcept {
        if (state != nullptr && state->refcount != 0u &&
            state->refcount != std::numeric_limits<std::uint32_t>::max() &&
            --state->refcount == 0u)
            pfree(state);
    }

    /** @brief Memory-context reset callback that quiesces the async owner and drops DSM detach bookkeeping. */
    void slice_project_owner_reset(void* argument) noexcept {
        auto* state = static_cast<slice_project_state*>(argument);
        if (state == nullptr)
            return;
        auto& runtime = state->runtime;
        if (runtime.owner_initialized) {
            finish_slice_async_owner_noexcept(&runtime.owner_guard,
                                              slice_shutdown_disposition::incomplete);
            runtime.attachment = nullptr;
        }
        /* End may be skipped by ERROR.  Drop exactly the query-owner token here;
         * the independently retained DSM callback token remains until detach. */
        if (runtime.detach_state != nullptr) {
            slice_project_release_detach_state(runtime.detach_state);
            runtime.detach_state = nullptr;
        }
        runtime.mapped_header = nullptr;
        runtime.mapped_worker = nullptr;
    }

    /* Registered before child initialization.  PostgreSQL invokes reset callbacks
     * in reverse registration order, so child scan/provider callbacks run after
     * the async-owner callback but before this upper-lease callback. */
    void slice_project_source_routes_reset(void* argument) noexcept {
        auto* state = static_cast<slice_project_state*>(argument);
        if (state == nullptr)
            return;
        auto& runtime = state->runtime;
        /* Attachment can ERROR after retaining an early route but before publishing
         * source_routes_acquired.  The container count is the unwind authority; an
         * empty or already-released set is intentionally a no-op. */
        bioseq::storage::release_pg_sequence_source_routes_noexcept(runtime.source_routes);
        runtime.source_routes_acquired = false;
    }

    /** @brief DSM detach callback that merges the final parallel cycle and releases trace attachment. */
    void slice_project_dsm_on_detach(dsm_segment*, Datum argument) noexcept {
        auto* state = reinterpret_cast<slice_dsm_detach_state*>(DatumGetPointer(argument));
        if (state == nullptr || !state->callback_armed)
            return;
        state->callback_armed = false;
        slice_project_merge_dsm_cycle_noexcept(
            state->mapped_header, state->pcxt, state->attachment, state->expected_cycle);
        auto* attachment = state->attachment;
        state->attachment = nullptr;
        state->pcxt = nullptr;
        state->mapped_header = nullptr;
        release_slice_trace_attachment(attachment);
        slice_project_release_detach_state(state);
    }

    /** @brief Merges backend-local counter deltas into the shared worker slot and publishes final_state. */
    void slice_project_publish_worker_noexcept(slice_project_state* state,
                                               slice_dsm_publish_state final_state) noexcept {
        if (state == nullptr)
            return;
        auto& runtime = state->runtime;
        auto* worker = runtime.mapped_worker;
        if (worker == nullptr || runtime.mapped_cycle == 0u)
            return;
        if (worker->publish_state != slice_dsm_publish_state::attached_running ||
            worker->cycle != runtime.mapped_cycle) {
            slice_project_mark_trace_invalid_noexcept(runtime.attachment);
            return;
        }
        auto* destination = slice_project_worker_counters(worker);
        bool overflowed = runtime.trace_runtime.invalid || runtime.trace_runtime.overflowed ||
                          worker->overflowed != 0u;
        for (std::uint32_t index = 0u; index < runtime.boundary_count; ++index) {
            if (!slice_dsm_counter_reserved_valid(destination[index]) ||
                !slice_dsm_counter_reserved_valid(runtime.counters[index])) {
                overflowed = true;
            } else {
                /* The shared slot is cumulative for one parallel cycle, whereas the
                 * backend-local sink is a delta since the preceding publish/rescan. */
                merge_slice_dsm_counter_noexcept(destination[index], runtime.counters[index]);
            }
            overflowed = overflowed || destination[index].overflowed != 0u ||
                         !slice_dsm_counter_reserved_valid(destination[index]);
        }
        worker->overflowed = overflowed ? 1u : 0u;
        pg_write_barrier();
        worker->publish_state =
            overflowed ? slice_dsm_publish_state::published_incomplete : final_state;
        /* A worker publishes one cycle delta.  Only after the shared slot is fully
         * visible may its backend-local sink be cleared for a subsequent rescan;
         * otherwise ReInitializeDSM would merge cumulative prefixes twice. */
        for (std::uint32_t index = 0u; index < runtime.boundary_count; ++index) {
            reset_slice_dsm_counter_noexcept(runtime.counters[index]);
        }
    }

    /** @brief Discards incomplete worker deltas and reattaches the slot for another parallel rescan. */
    [[nodiscard]] bool
    slice_project_rearm_worker_for_rescan_noexcept(slice_project_state* state) noexcept {
        if (state == nullptr)
            return false;
        auto& runtime = state->runtime;
        auto* worker = runtime.mapped_worker;
        if (worker == nullptr)
            return true;
        if (runtime.mapped_cycle == 0u || worker->cycle != runtime.mapped_cycle ||
            (worker->publish_state != slice_dsm_publish_state::published_complete &&
             worker->publish_state != slice_dsm_publish_state::published_incomplete)) {
            slice_project_mark_trace_invalid_noexcept(runtime.attachment);
            return false;
        }
        /* Completed slots are cumulative across rescans.  An incomplete slot is a
         * tentative delta and must be discarded before a later rescan can publish
         * the same stable worker slot as complete. */
        if (worker->publish_state == slice_dsm_publish_state::published_incomplete) {
            auto* counters = slice_project_worker_counters(worker);
            for (std::uint32_t index = 0u; index < runtime.boundary_count; ++index) {
                reset_slice_dsm_counter_noexcept(counters[index]);
            }
            worker->overflowed = 0u;
        }
        pg_write_barrier();
        worker->publish_state = slice_dsm_publish_state::attached_running;
        return true;
    }

    /** @brief Async-owner publish-complete callback that marks the worker slot published_complete. */
    void slice_project_publish_complete_noexcept(void* argument) noexcept {
        slice_project_publish_worker_noexcept(static_cast<slice_project_state*>(argument),
                                              slice_dsm_publish_state::published_complete);
    }

    /** @brief Async-owner publish-incomplete callback that marks the worker slot published_incomplete. */
    void slice_project_publish_incomplete_noexcept(void* argument) noexcept {
        slice_project_publish_worker_noexcept(static_cast<slice_project_state*>(argument),
                                              slice_dsm_publish_state::published_incomplete);
    }

    /** @brief Walks a plan subtree to retain BioseqScan and nested projector sequence source routes. */
    void slice_project_attach_sources_from_plan_state(PlanState* child,
                                                      slice_project_state* state,
                                                      std::uint32_t& tagged_sources) {
        if (child == nullptr || state == nullptr)
            return;
        if (IsA(child, CustomScanState)) {
            auto* child_custom = reinterpret_cast<CustomScanState*>(child);
            if (child_custom->methods == &bioseq_slice_project_exec_methods()) {
                /* Nested projectors form an async-owner boundary.  Initialize the
                 * lower projector first, then retain its exact source routes instead
                 * of descending through it and rebinding the same leaf buffers to a
                 * second owner.  The lower projector owns work below the boundary;
                 * this projector owns only provenance it publishes above it. */
                slice_project_ensure_trace_initialized(child_custom);
                auto* child_state = reinterpret_cast<slice_project_state*>(child_custom);
                if (!child_state->runtime.source_routes_acquired ||
                    child_state->runtime.source_routes.count == 0u ||
                    !bioseq::storage::merge_pg_sequence_source_routes(
                        state->runtime.source_routes, child_state->runtime.source_routes)) {
                    ereport(ERROR,
                            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                             errmsg("BioseqSliceProject nested source route is unavailable")));
                }
                state->runtime.source_routes_acquired = true;
                ++tagged_sources;
                return;
            }
            if (child_custom->methods == bioseq_column_scan_execute_methods()) {
                auto* bioseq_state = scan_state::from_custom_scan_state(child_custom);
                bioseq_scan_runtime& child_runtime = bioseq_runtime(bioseq_state);
                if (!child_runtime.lazy_source_projection)
                    return;
                ++tagged_sources;
                /* The upper set retains child lifetimes only.  Locator stubs are
                 * promoted inside each BioseqScan through that leaf's exact provider
                 * before its tuple reaches this projection, so self-join aliases never
                 * require an ambiguous physical-relation lookup here. */
                if (child_runtime.lazy_source_provider == nullptr ||
                    !bioseq::storage::add_pg_sequence_source_route(
                        state->runtime.source_routes, child_runtime.lazy_source_provider)) {
                    ereport(ERROR,
                            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                             errmsg("BioseqSliceProject source route is unavailable")));
                }
                state->runtime.source_routes_acquired = true;
                if (child_runtime.lazy_trace_attachment != nullptr) {
                    release_slice_trace_attachment(child_runtime.lazy_trace_attachment);
                    child_runtime.lazy_trace_attachment = nullptr;
                }
                if (slice_trace_attachment_live(state->runtime.attachment)) {
                    child_runtime.lazy_trace_attachment = state->runtime.attachment;
                    retain_slice_trace_attachment(child_runtime.lazy_trace_attachment);
                    child_runtime.buffer.bind_slice_async_owner(&state->runtime.owner_guard);
                }
                return;
            }
            for (PlanState* custom_child : postgres_list<PlanState>(child_custom->custom_ps)) {
                slice_project_attach_sources_from_plan_state(custom_child, state, tagged_sources);
            }
            return;
        }
        if (IsA(child, AppendState)) {
            auto* append = reinterpret_cast<AppendState*>(child);
            for (int index = 0; index < append->as_nplans; ++index) {
                slice_project_attach_sources_from_plan_state(
                    append->appendplans[index], state, tagged_sources);
            }
            return;
        }
        if (IsA(child, MergeAppendState)) {
            auto* append = reinterpret_cast<MergeAppendState*>(child);
            for (int index = 0; index < append->ms_nplans; ++index) {
                slice_project_attach_sources_from_plan_state(
                    append->mergeplans[index], state, tagged_sources);
            }
            return;
        }
        PlanState* outer = outerPlanState(child);
        PlanState* inner = innerPlanState(child);
        if (outer != nullptr) {
            slice_project_attach_sources_from_plan_state(outer, state, tagged_sources);
        }
        if (inner != nullptr && inner != outer) {
            slice_project_attach_sources_from_plan_state(inner, state, tagged_sources);
        }
    }

    /** @brief Retains child sequence provenance and binds trace ownership across the projector subtree. */
    void slice_project_attach_trace_to_child(CustomScanState* custom, slice_project_state* state) {
        if (custom == nullptr || custom->custom_ps == NIL || state == nullptr)
            return;
        std::uint32_t tagged_sources = 0u;
        for (PlanState* child : postgres_list<PlanState>(custom->custom_ps)) {
            slice_project_attach_sources_from_plan_state(child, state, tagged_sources);
        }
        if (tagged_sources == 0u) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqSliceProject child did not accept source ownership")));
        }
    }

    /** @brief Allocates trace runtime, async owner, materialization boundaries, and output bindings from plan metadata. */
    void
    slice_project_initialize_trace(slice_project_state* state, CustomScan* scan, EState* estate) {
        auto& runtime = state->runtime;
        List* records = slice_project_plan_records(scan);
        List* output_bindings = slice_project_plan_output_bindings(scan);
        runtime.boundary_count = static_cast<std::uint32_t>(list_length(records));
        runtime.output_binding_count = static_cast<std::uint32_t>(list_length(output_bindings));
        runtime.collect_actual =
            runtime.boundary_count != 0u && state->custom_scan_state.ss.ps.instrument != nullptr;
        runtime.timing =
            runtime.collect_actual && state->custom_scan_state.ss.ps.instrument->need_timer;
        if (runtime.boundary_count != 0u) {
            runtime.boundaries =
                static_cast<slice_materialization_boundary*>(MemoryContextAllocZero(
                    estate->es_query_cxt,
                    mul_size(runtime.boundary_count, sizeof(slice_materialization_boundary))));
            runtime.counters = static_cast<slice_dsm_counter*>(MemoryContextAllocZero(
                estate->es_query_cxt, mul_size(runtime.boundary_count, sizeof(slice_dsm_counter))));
        }
        std::uint32_t index = 0u;
        for (Node* raw_record : postgres_list<Node>(records)) {
            List* record = reinterpret_cast<List*>(raw_record);
            runtime.boundaries[index] = {
                .boundary_id = static_cast<std::uint32_t>(
                    intVal(list_nth(record, k_slice_record_boundary_id))),
                .producer_value_id = static_cast<std::uint32_t>(
                    intVal(list_nth(record, k_slice_record_producer_id))),
                .counter_index = index,
                .mechanism = static_cast<materialization_mechanism>(
                    intVal(list_nth(record, k_slice_record_mechanism))),
                .reason = static_cast<materialization_reason>(
                    intVal(list_nth(record, k_slice_record_reason))),
                .reserved = {},
            };
            ++index;
        }
        if (!initialize_slice_materialization_runtime_noexcept(&runtime.trace_runtime,
                                                               runtime.boundaries,
                                                               runtime.boundary_count,
                                                               runtime.counters,
                                                               runtime.boundary_count,
                                                               runtime.collect_actual,
                                                               runtime.timing)) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqSliceProject trace runtime is invalid")));
        }
        runtime.attachment = make_slice_trace_attachment(&runtime.trace_runtime);
        runtime.async_jobs = static_cast<slice_async_job_slot*>(MemoryContextAllocZero(
            estate->es_query_cxt,
            mul_size(slice_async_max_live_jobs, sizeof(slice_async_job_slot))));
        if (runtime.boundary_count != 0u) {
            const Size async_counter_count = mul_size(static_cast<Size>(slice_async_max_live_jobs),
                                                      static_cast<Size>(runtime.boundary_count));
            runtime.async_job_counters = static_cast<slice_dsm_counter*>(MemoryContextAllocZero(
                estate->es_query_cxt, mul_size(async_counter_count, sizeof(slice_dsm_counter))));
        }
        if (!initialize_slice_async_owner_noexcept(&runtime.owner_guard,
                                                   &runtime.trace_runtime,
                                                   runtime.attachment,
                                                   runtime.async_jobs,
                                                   slice_async_max_live_jobs,
                                                   runtime.async_job_counters,
                                                   runtime.boundary_count)) {
            invalidate_slice_trace_attachment_noexcept(runtime.attachment);
            release_slice_trace_attachment(runtime.attachment);
            runtime.attachment = nullptr;
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqSliceProject owner initialization failed")));
        }
        runtime.owner_guard.publish_complete = {.argument = state,
                                                .invoke = slice_project_publish_complete_noexcept};
        runtime.owner_guard.publish_incomplete = {
            .argument = state, .invoke = slice_project_publish_incomplete_noexcept};
        runtime.owner_initialized = true;
        runtime.owner_guard.callback.func = slice_project_owner_reset;
        runtime.owner_guard.callback.arg = state;
        MemoryContextRegisterResetCallback(estate->es_query_cxt, &runtime.owner_guard.callback);
        if (runtime.output_binding_count != 0u) {
            runtime.output_bindings =
                static_cast<slice_project_output_binding*>(MemoryContextAllocZero(
                    estate->es_query_cxt,
                    mul_size(runtime.output_binding_count, sizeof(slice_project_output_binding))));
            index = 0u;
            for (Node* raw_binding : postgres_list<Node>(output_bindings)) {
                List* binding = reinterpret_cast<List*>(raw_binding);
                runtime.output_bindings[index++] = {
                    .resno = static_cast<AttrNumber>(intVal(linitial(binding))),
                    .producer_value_id = static_cast<std::uint32_t>(intVal(lsecond(binding))),
                };
            }
        }
    }

    /** @brief Lazily initializes trace state and child source attachment on first use. */
    void slice_project_ensure_trace_initialized(CustomScanState* custom) {
        if (custom == nullptr)
            return;
        auto* state = reinterpret_cast<slice_project_state*>(custom);
        auto& runtime = state->runtime;
        if (runtime.trace_initialization_attempted)
            return;
        runtime.trace_initialization_attempted = true;
        EState* estate = custom->ss.ps.state;
        auto* scan = reinterpret_cast<CustomScan*>(custom->ss.ps.plan);
        if (estate == nullptr || scan == nullptr) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqSliceProject trace initialization state is invalid")));
        }
        slice_project_initialize_trace(state, scan, estate);
        slice_project_attach_trace_to_child(custom, state);
    }

    /** @brief Returns shared DSM bytes required when instrumentation collects actual materialization counters. */
    Size slice_project_estimate_dsm(CustomScanState* custom, ParallelContext* pcxt) {
        auto* state = reinterpret_cast<slice_project_state*>(custom);
        slice_project_ensure_trace_initialized(custom);
        if (!state->runtime.collect_actual)
            return 0u;
        if (pcxt == nullptr || pcxt->nworkers < 0) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqSliceProject parallel context is invalid")));
        }
        return make_slice_project_dsm_layout(state->runtime.boundary_count,
                                             static_cast<std::uint32_t>(pcxt->nworkers))
            .total_bytes;
    }

    /** @brief Publishes the initial DSM header, arms detach merge, and retains trace attachment for workers. */
    void
    slice_project_initialize_dsm(CustomScanState* custom, ParallelContext* pcxt, void* coordinate) {
        auto* state = reinterpret_cast<slice_project_state*>(custom);
        slice_project_ensure_trace_initialized(custom);
        auto& runtime = state->runtime;
        if (!runtime.collect_actual)
            return;
        if (coordinate == nullptr || pcxt == nullptr || pcxt->seg == nullptr ||
            pcxt->nworkers < 0 || runtime.detach_state != nullptr) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqSliceProject DSM initialization is invalid")));
        }
        const auto slot_count = static_cast<std::uint32_t>(pcxt->nworkers);
        const slice_project_dsm_layout layout =
            make_slice_project_dsm_layout(runtime.boundary_count, slot_count);
        std::memset(coordinate, 0, layout.total_bytes);
        auto* header = static_cast<slice_dsm_header*>(coordinate);
        header->magic = k_bioseq_slice_dsm_magic;
        header->codec_version = k_bioseq_slice_dsm_version;
        header->header_bytes = static_cast<std::uint16_t>(layout.header_bytes);
        header->recipe_count = runtime.boundary_count;
        header->slot_count = slot_count;
        header->cycle = 1u;
        header->slot_stride = layout.slot_stride;
        runtime.mapped_header = header;
        runtime.mapped_cycle = 1u;
        auto* detach = static_cast<slice_dsm_detach_state*>(
            MemoryContextAllocZero(TopMemoryContext, sizeof(slice_dsm_detach_state)));
        /* The query-owner token exists before registration so an ERROR from
         * on_dsm_detach is reclaimed by the already-installed query callback.
         * Acquire the independent callback token/attachment retain only after
         * registration succeeds; otherwise neither can become a phantom leak. */
        detach->refcount = 1u;
        detach->callback_armed = false;
        detach->pcxt = pcxt;
        detach->mapped_header = header;
        detach->attachment = runtime.attachment;
        detach->expected_cycle = 1u;
        runtime.detach_state = detach;
        on_dsm_detach(pcxt->seg, slice_project_dsm_on_detach, PointerGetDatum(detach));
        retain_slice_trace_attachment(detach->attachment);
        detach->refcount = 2u;
        detach->callback_armed = true;
    }

    /** @brief Merges the prior parallel cycle into trace state, bumps the DSM cycle, and clears worker slots. */
    void slice_project_reinitialize_dsm(CustomScanState* custom,
                                        ParallelContext* pcxt,
                                        void* coordinate) {
        auto* state = reinterpret_cast<slice_project_state*>(custom);
        slice_project_ensure_trace_initialized(custom);
        auto& runtime = state->runtime;
        if (!runtime.collect_actual)
            return;
        if (coordinate == nullptr || pcxt == nullptr || pcxt->seg == nullptr ||
            pcxt->nworkers < 0) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqSliceProject DSM reinitialization is invalid")));
        }
        auto* header = static_cast<slice_dsm_header*>(coordinate);
        const std::uint64_t expected_cycle = runtime.mapped_cycle;
        if (runtime.mapped_header != header || expected_cycle == 0u ||
            header->cycle != expected_cycle ||
            (runtime.detach_state != nullptr &&
             (runtime.detach_state->mapped_header != header ||
              runtime.detach_state->expected_cycle != expected_cycle))) {
            slice_project_mark_trace_invalid_noexcept(runtime.attachment);
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqSliceProject DSM cycle identity changed"),
                     errdetail("expected_cycle=%llu shared_cycle=%llu",
                               static_cast<unsigned long long>(expected_cycle),
                               static_cast<unsigned long long>(header->cycle))));
        }
        slice_project_dsm_merge_audit* merge_audit = nullptr;
#ifdef BIOSEQ_TEST_INDEXES
        merge_audit = runtime.dsm_test_reinitialize_audit;
        runtime.dsm_test_reinitialize_audit = nullptr;
#endif
        auto* trace_runtime = runtime.attachment == nullptr ? nullptr : runtime.attachment->runtime;
        slice_project_merge_dsm_cycle_noexcept(
            header, pcxt, runtime.attachment, expected_cycle, merge_audit);
        if (trace_runtime == nullptr || trace_runtime->invalid ||
            trace_runtime->last_merged_cycle != expected_cycle) {
            ereport(
                ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("BioseqSliceProject DSM cycle merge was rejected"),
                 errdetail("expected_cycle=%llu merged_cycle=%llu",
                           static_cast<unsigned long long>(expected_cycle),
                           static_cast<unsigned long long>(
                               trace_runtime == nullptr ? 0u : trace_runtime->last_merged_cycle))));
        }
        if (expected_cycle == std::numeric_limits<std::uint64_t>::max()) {
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("BioseqSliceProject parallel cycle exhausted")));
        }
        const auto slot_count = static_cast<std::uint32_t>(pcxt->nworkers);
        const slice_project_dsm_layout layout =
            make_slice_project_dsm_layout(runtime.boundary_count, slot_count);
        std::memset(reinterpret_cast<char*>(header) + layout.header_bytes,
                    0,
                    layout.total_bytes - layout.header_bytes);
        header->magic = k_bioseq_slice_dsm_magic;
        header->codec_version = k_bioseq_slice_dsm_version;
        header->header_bytes = static_cast<std::uint16_t>(layout.header_bytes);
        header->recipe_count = runtime.boundary_count;
        header->slot_count = slot_count;
        header->slot_stride = layout.slot_stride;
        pg_write_barrier();
        header->cycle = expected_cycle + 1u;
        runtime.mapped_header = header;
        runtime.mapped_cycle = header->cycle;
        if (runtime.detach_state != nullptr) {
            runtime.detach_state->mapped_header = header;
            pg_write_barrier();
            runtime.detach_state->expected_cycle = header->cycle;
        }
        if (runtime.owner_initialized &&
            runtime.owner_guard.phase == slice_async_owner_phase::quiesced) {
            (void)rearm_slice_async_owner_for_rescan_noexcept(&runtime.owner_guard);
        }
    }

    /** @brief Maps one parallel worker to its DSM slot and marks it attached_running for the current cycle. */
    void slice_project_initialize_worker(CustomScanState* custom, shm_toc*, void* coordinate) {
        auto* state = reinterpret_cast<slice_project_state*>(custom);
        slice_project_ensure_trace_initialized(custom);
        auto& runtime = state->runtime;
        if (!runtime.collect_actual)
            return;
        if (coordinate == nullptr || ParallelWorkerNumber < 0) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqSliceProject worker coordinate is invalid")));
        }
        auto* header = static_cast<slice_dsm_header*>(coordinate);
        if (header->slot_count >
                static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
            ParallelWorkerNumber >= static_cast<int>(header->slot_count)) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqSliceProject worker slot is out of range")));
        }
        slice_project_dsm_layout layout{};
        if (!slice_project_validate_dsm_header_noexcept(
                header, runtime.boundary_count, header->slot_count, header->cycle, &layout)) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqSliceProject worker DSM header is invalid")));
        }
        auto* worker =
            slice_project_worker_slot(header, static_cast<std::uint32_t>(ParallelWorkerNumber));
        if (worker->publish_state != slice_dsm_publish_state::never_launched ||
            worker->cycle != 0u ||
            std::any_of(worker->reserved.begin(), worker->reserved.end(), [](std::byte byte) {
                return byte != std::byte{0};
            })) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqSliceProject worker DSM slot is invalid")));
        }
        runtime.mapped_header = header;
        runtime.mapped_worker = worker;
        runtime.mapped_cycle = header->cycle;
        worker->cycle = header->cycle;
        pg_write_barrier();
        worker->publish_state = slice_dsm_publish_state::attached_running;
    }

    /** @brief Executes the sole child plan and publishes its tuple into the scan slot with read-only expanded datums. */
    [[nodiscard]] TupleTableSlot* slice_project_publish_child(CustomScanState* custom,
                                                              bool count_row) {
        if (custom->custom_ps == NIL || list_length(custom->custom_ps) != 1) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqSliceProject executor child is missing")));
        }
        auto* child = reinterpret_cast<PlanState*>(linitial(custom->custom_ps));
        TupleTableSlot* child_slot = ExecProcNode(child);
        TupleTableSlot* scan_slot = custom->ss.ss_ScanTupleSlot;
        if (TupIsNull(child_slot))
            return ExecClearTuple(scan_slot);
#ifdef BIOSEQ_TEST_INDEXES
        const auto publication_before = capture_slice_payload_allocation_test_snapshot_noexcept();
#endif
        slot_getallattrs(child_slot);
        const int natts = child_slot->tts_tupleDescriptor->natts;
        if (natts != scan_slot->tts_tupleDescriptor->natts) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("BioseqSliceProject child descriptor changed")));
        }
        ExecClearTuple(scan_slot);
        std::memcpy(scan_slot->tts_values,
                    child_slot->tts_values,
                    static_cast<std::size_t>(natts) * sizeof(Datum));
        std::memcpy(scan_slot->tts_isnull,
                    child_slot->tts_isnull,
                    static_cast<std::size_t>(natts) * sizeof(bool));
        scan_slot->tts_nvalid = natts;
        TupleDesc descriptor = scan_slot->tts_tupleDescriptor;
        for (int index = 0; index < natts; ++index) {
            if (!scan_slot->tts_isnull[index]) {
                scan_slot->tts_values[index] = MakeExpandedObjectReadOnly(
                    scan_slot->tts_values[index], false, TupleDescAttr(descriptor, index)->attlen);
            }
        }
        ExecStoreVirtualTuple(scan_slot);
#ifdef BIOSEQ_TEST_INDEXES
        record_slice_project_publication_allocation_delta_noexcept(
            publication_before, capture_slice_payload_allocation_test_snapshot_noexcept());
#endif
        if (count_row) {
            auto* state = reinterpret_cast<slice_project_state*>(custom);
            state->runtime.rows_published =
                bioseq::common::saturating_add(state->runtime.rows_published, 1u);
        }
        return scan_slot;
    }

} // export custom-scan partition declarations
