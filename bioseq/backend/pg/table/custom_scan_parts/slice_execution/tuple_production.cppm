/**
 * @file
 * @brief Owns SliceProject begin, tuple production, end, and rescan callbacks.
 */
module;
#include <bioseq/pg_compat.h>

extern "C" {
#include "catalog/namespace.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_language_d.h"
#include "catalog/pg_proc.h"
#include "commands/extension.h"
#include "optimizer/appendinfo.h"
#include "optimizer/planmain.h"
#include "storage/dsm.h"
#include "storage/shmem.h"
#include "utils/inval.h"
#include "utils/syscache.h"

#include "rewrite/rewriteManip.h"
}

export module bioseq.postgres.table:custom_scan_slice_execution_tuple_production;

import bioseq.postgres.support;

import std;
import bioseq.common.checked_arithmetic;
import bioseq.postgres.storage.scanner;
import bioseq.postgres.types.compact_sequence;

import :custom_scan_oid_expression_support_oid_catalog;
import :custom_scan_slice_path_codec_contract;
import :custom_scan_slice_path_runtime_contract;
import :custom_scan_slice_path_codec_validation;

export {
    [[nodiscard]] TupleTableSlot* slice_project_access(ScanState* scan) {
        return slice_project_publish_child(reinterpret_cast<CustomScanState*>(scan), true);
    }

    [[nodiscard]] bool slice_project_recheck(ScanState* scan, TupleTableSlot*) {
        CustomScanState* custom = reinterpret_cast<CustomScanState*>(scan);
        if (custom->ss.ps.state == nullptr || custom->ss.ps.state->es_epq_active == nullptr)
            return true;
        return !TupIsNull(slice_project_publish_child(custom, false));
    }

    void slice_project_begin(CustomScanState* custom, EState* estate, int eflags) {
        auto* state = reinterpret_cast<slice_project_state*>(custom);
#ifdef BIOSEQ_TEST_INDEXES
        /* Claim the backend-local one-shot before any plan validation or child
         * initialization can ERROR.  The claimed mode stays on this executor node
         * for post-execution EXPLAIN diagnostics; an aborted statement destroys it
         * instead of leaking the injection to a later query. */
        state->runtime.dsm_test_fixture_mode =
            std::exchange(pending_slice_dsm_test_fixture, k_slice_dsm_test_none);
#endif
        auto* scan = reinterpret_cast<CustomScan*>(custom->ss.ps.plan);
        scan->methods = &bioseq_slice_project_scan_methods();
        slice_project_validate_codec(scan);
        if (list_length(scan->custom_plans) != 1) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqSliceProject requires exactly one child plan")));
        }
        List* header = slice_project_plan_header(scan);
        state->runtime.planned_symbols = slice_project_int8_value(
            reinterpret_cast<Node*>(list_nth(header, k_slice_header_planned_symbols)));
        state->runtime.planned_bytes = slice_project_int8_value(
            reinterpret_cast<Node*>(list_nth(header, k_slice_header_planned_bytes)));
        /* Register the upper-lease reset callback before ExecInitNode registers
         * any child/provider callbacks.  Their reverse-order teardown then closes
         * providers before this callback drops the independently retained routes. */
        state->runtime.source_routes_callback.func = slice_project_source_routes_reset;
        state->runtime.source_routes_callback.arg = state;
        bioseq::storage::initialize_pg_sequence_source_routes(state->runtime.source_routes,
                                                              estate->es_query_cxt);
        MemoryContextRegisterResetCallback(estate->es_query_cxt,
                                           &state->runtime.source_routes_callback);
        state->runtime.source_routes_callback_registered = true;
        Plan* child_plan = reinterpret_cast<Plan*>(linitial(scan->custom_plans));
        custom->custom_ps = list_make1(ExecInitNode(child_plan, estate, eflags));
    }

    [[nodiscard]] TupleTableSlot* slice_project_exec(CustomScanState* custom) {
        slice_project_ensure_trace_initialized(custom);
        TupleTableSlot* result = ExecScan(&custom->ss, slice_project_access, slice_project_recheck);
        auto* state = reinterpret_cast<slice_project_state*>(custom);
        auto& runtime = state->runtime;
        if (TupIsNull(result) || !slice_trace_attachment_live(runtime.attachment) ||
            runtime.output_binding_count == 0u)
            return result;
        slot_getallattrs(result);
        const Oid seq_oid = get_cached_stub_oids().bioseq_seq_type_oid;
        for (std::uint32_t index = 0u; index < runtime.output_binding_count; ++index) {
            auto const& binding = runtime.output_bindings[index];
            if (binding.resno <= 0 || binding.resno > result->tts_tupleDescriptor->natts ||
                bioseq::postgres::support::tuple_descriptor_attribute(result->tts_tupleDescriptor, binding.resno - 1)->atttypid !=
                    seq_oid) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqSliceProject result binding changed")));
            }
            if (result->tts_isnull[binding.resno - 1])
                continue;
            /* CASE/COALESCE may select a flat or literal arm even though another
             * arm caused this symbolic recipe to be planned.  Only an expanded
             * result can carry provenance; a skipped lazy arm is not an error and
             * must retain zero actual boundary calls. */
            if (!VARATT_IS_EXTERNAL_EXPANDED(
                    DatumGetPointer(result->tts_values[binding.resno - 1])))
                continue;
            if (!attach_slice_trace_provenance(result->tts_values[binding.resno - 1],
                                               runtime.attachment,
                                               binding.producer_value_id)) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqSliceProject result lost expanded provenance")));
            }
        }
        return result;
    }

    void slice_project_end(CustomScanState* custom) {
        auto* state = reinterpret_cast<slice_project_state*>(custom);
        /* No source-backed task may outlive the owning upper executor. */
        if (state->runtime.owner_initialized) {
            finish_slice_async_owner_noexcept(&state->runtime.owner_guard,
                                              slice_shutdown_disposition::complete);
            state->runtime.attachment = nullptr;
        }
        /* Core frees this node's ExprContext only after EndCustomScan returns.
         * Drain the last projected EOH while both the child provider and our upper
         * route are still live; otherwise provider_end and upper_lease_end can both
         * sweep before that EOH drops its source-domain reference, leaving only the
         * registry token after a cursor CLOSE with no later statement boundary. */
        if (custom->ss.ps.ps_ResultTupleSlot != nullptr) {
            ExecClearTuple(custom->ss.ps.ps_ResultTupleSlot);
        }
        if (custom->ss.ps.ps_ExprContext != nullptr) {
            ReScanExprContext(custom->ss.ps.ps_ExprContext);
        }
        if (custom->custom_ps != NIL) {
            ExecEndNode(reinterpret_cast<PlanState*>(linitial(custom->custom_ps)));
            custom->custom_ps = NIL;
        }
        ExecClearTuple(custom->ss.ss_ScanTupleSlot);
        /* Child End closes every provider first.  The independent upper leases
         * keep registries alive until this final release/sweep. */
        if (state->runtime.source_routes_acquired) {
            bioseq::storage::release_pg_sequence_source_routes(state->runtime.source_routes,
                                                               /*is_commit=*/true);
            state->runtime.source_routes_acquired = false;
        }
        if (state->runtime.detach_state != nullptr) {
            slice_project_release_detach_state(state->runtime.detach_state);
            state->runtime.detach_state = nullptr;
        }
        state->runtime.mapped_header = nullptr;
        state->runtime.mapped_worker = nullptr;
    }

    void slice_project_rescan(CustomScanState* custom) {
        auto* state = reinterpret_cast<slice_project_state*>(custom);
        slice_project_ensure_trace_initialized(custom);
        if (state->runtime.owner_initialized) {
            quiesce_slice_async_owner_noexcept(&state->runtime.owner_guard,
                                               slice_shutdown_disposition::complete);
        }
        /* Reset the ScanState projection/result machinery before the parameterized
         * child is rescanned.  Clearing only ss_ScanTupleSlot leaves ExecScan's
         * prior projected datum live, so a NULL source on the next PARAM_EXEC pass
         * can inherit the preceding non-NULL slice. */
        ExecScanReScan(&custom->ss);
        /* ExecScanReScan resets scan input and expression state but does not
         * guarantee that a previously projected virtual result is invalidated.
         * Nested-loop PARAM_EXEC rescans must not return that stale expanded slice
         * when the next child row projects NULL. */
        if (custom->ss.ps.ps_ResultTupleSlot != nullptr) {
            ExecClearTuple(custom->ss.ps.ps_ResultTupleSlot);
        }
        if (custom->custom_ps != NIL) {
            auto* child = reinterpret_cast<PlanState*>(linitial(custom->custom_ps));
            /* CustomScan children live in custom_ps rather than lefttree/righttree,
             * so core ExecReScan cannot propagate the parent's changed PARAM_EXEC
             * set for us.  Preserve that invalidation before rescanning the
             * parameterized join/Append subtree. */
            if (custom->ss.ps.chgParam != nullptr) {
                UpdateChangedParamSet(child, custom->ss.ps.chgParam);
            }
            ExecReScan(child);
        }
        if (state->runtime.source_routes_acquired) {
            for (std::size_t index = 0u; index < state->runtime.source_routes.count; ++index) {
                bioseq::storage::sweep_reclaimable_pg_sequence_source_domains(
                    bioseq::storage::pg_sequence_source_route_lease(state->runtime.source_routes,
                                                                    index),
                    bioseq::storage::pg_sequence_source_sweep_boundary::rescan,
                    /*is_commit=*/true);
            }
        }
        state->runtime.rescans = bioseq::common::saturating_add(state->runtime.rescans, 1u);
        const bool worker_rearmed = slice_project_rearm_worker_for_rescan_noexcept(state);
        if (state->runtime.owner_initialized &&
            state->runtime.owner_guard.phase == slice_async_owner_phase::quiesced &&
            worker_rearmed) {
            (void)rearm_slice_async_owner_for_rescan_noexcept(&state->runtime.owner_guard);
        }
    }

    void slice_project_mark_pos(CustomScanState* custom) {
        if (custom->custom_ps != NIL) {
            ExecMarkPos(reinterpret_cast<PlanState*>(linitial(custom->custom_ps)));
        }
    }

    void slice_project_restr_pos(CustomScanState* custom) {
        if (custom->custom_ps != NIL) {
            ExecRestrPos(reinterpret_cast<PlanState*>(linitial(custom->custom_ps)));
        }
    }

} // export custom-scan partition declarations
