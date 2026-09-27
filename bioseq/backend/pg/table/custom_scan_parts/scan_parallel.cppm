/**
 * @file
 * @brief Shares parallel scan state  and defines column-scan executor callbacks.
 */
module;
extern "C" {
#include "postgres.h"
#include "access/tupdesc.h"
#include "catalog/namespace.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_language_d.h"
#include "catalog/pg_proc.h"
#include "commands/extension.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/pg_list.h"
#include "optimizer/appendinfo.h"
#include "optimizer/optimizer.h"
#include "optimizer/planmain.h"
#include "port/atomics.h"
#include "storage/dsm.h"
#include "storage/shmem.h"
#include "utils/errcodes.h"
#include "utils/inval.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#include "rewrite/rewriteManip.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

export module bioseq.postgres.table:custom_scan_scan_parallel;

import :utils;
import bioseq.common.logging;
import bioseq.postgres.scheduler.dispatch_buffer.runtime_contract;
import bioseq.postgres.storage.parallel_scan_contract;
import bioseq.postgres.storage.scanner.snapshot_rebind;
import bioseq.postgres.table.scan_state;
import bioseq.postgres.support;

import :custom_scan_execution_method_contracts;
import :custom_scan_scan_runtime_state_contracts;
import :custom_scan_semantic_payload_explain_callback_contracts;
import :custom_scan_slice_execution_dsm_protocol;

using namespace bioseq::postgres::support;

export {

    /** @brief Shared-memory layout revision for parallel base scans. */

    inline constexpr std::uint32_t k_parallel_column_scan_revision = 3u;

    /** @brief Returns a worker counter slot, or null if its shared-memory layout is invalid. */
    [[nodiscard]] slice_dsm_worker_header*
    bioseq_column_parallel_counter_slot(parallel_column_scan_handle pscan,
                                        std::uint32_t slot_index,
                                        std::uint32_t expected_entry_count,
                                        std::uint32_t expected_slot_count) noexcept {
        if (pscan == nullptr || pscan->version != k_parallel_column_scan_revision ||
            pscan->entry_count != expected_entry_count ||
            pscan->slot_count != expected_slot_count || slot_index >= pscan->slot_count ||
            pscan->counters_offset <
                MAXALIGN(offsetof(parallel_column_scan_state, snapshot_data))) {
            return nullptr;
        }
        slice_project_dsm_layout layout{};
        if (!slice_project_dsm_layout_noexcept(pscan->entry_count, pscan->slot_count, &layout) ||
            pscan->slot_stride != layout.slot_stride)
            return nullptr;
        return reinterpret_cast<slice_dsm_worker_header*>(
            reinterpret_cast<char*>(pscan) + pscan->counters_offset +
            static_cast<Size>(slot_index) * pscan->slot_stride);
    }

    /** @brief Estimates shared memory needed for the scan snapshot and counters for the leader and workers. */
    Size bioseq_column_estimate_dsm_custom_scan(CustomScanState* node, ParallelContext* pcxt) {
        scan_state* bioseq_scan_state = scan_state::from_custom_scan_state(node);
        if (pcxt == nullptr || pcxt->nworkers < 0) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqScan parallel context is invalid")));
        }
        const Size snapshot_end = add_size(offsetof(parallel_column_scan_state, snapshot_data),
                                           EstimateSnapshotSpace(bioseq_scan_state->snapshot));
        const Size counters_offset = MAXALIGN(snapshot_end);
        const Size slot_count = add_size(static_cast<Size>(pcxt->nworkers), 1u);
        constexpr std::uint32_t entry_count = 0u;
        const Size slot_stride = MAXALIGN(add_size(
            sizeof(slice_dsm_worker_header), mul_size(entry_count, sizeof(slice_dsm_counter))));
        return add_size(counters_offset, mul_size(slot_count, slot_stride));
    }

    /** @brief Saves the leader's snapshot and prepares shared stripe allocation and worker counters. */
    void bioseq_column_initialize_dsm_custom_scan(CustomScanState* node,
                                                  ParallelContext* pcxt,
                                                  void* coordinate) {
        parallel_column_scan_handle pscan =
            reinterpret_cast<parallel_column_scan_handle>(coordinate);
        scan_state* bioseq_scan_state = scan_state::from_custom_scan_state(node);
        if (pcxt == nullptr || pcxt->nworkers < 0 || pscan == nullptr) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqScan DSM initialization is invalid")));
        }
        const Size snapshot_end = add_size(offsetof(parallel_column_scan_state, snapshot_data),
                                           EstimateSnapshotSpace(bioseq_scan_state->snapshot));
        const Size counters_offset = MAXALIGN(snapshot_end);
        const Size slot_count = add_size(static_cast<Size>(pcxt->nworkers), 1u);
        constexpr std::uint32_t entry_count = 0u;
        const Size slot_stride = MAXALIGN(add_size(
            sizeof(slice_dsm_worker_header), mul_size(entry_count, sizeof(slice_dsm_counter))));
        pscan->version = k_parallel_column_scan_revision;
        pscan->counters_offset = counters_offset;
        pscan->entry_count = entry_count;
        pscan->slot_count = static_cast<std::uint32_t>(slot_count);
        pscan->slot_stride = slot_stride;
        std::memset(
            reinterpret_cast<char*>(pscan) + counters_offset, 0, mul_size(slot_count, slot_stride));

        /* Give workers the snapshot held by this scan. */
        SerializeSnapshot(bioseq_scan_state->snapshot, pscan->snapshot_data);

        /* Stripe IDs start at 1. */
        pg_atomic_init_u64(&pscan->next_stripe_id, 1);

        if (parallel_leader_participation)
            bioseq_scan_state->parallel_scan = pscan;
        else
            bioseq_scan_state->parallel_scan = nullptr;
    }

    /** @brief Resets the shared stripe cursor and worker counters for a parallel rescan. */
    void bioseq_column_reinitialize_dsm_custom_scan(CustomScanState* node,
                                                    ParallelContext* pcxt,
                                                    void* coordinate) {
        parallel_column_scan_handle pscan =
            reinterpret_cast<parallel_column_scan_handle>(coordinate);
        scan_state* bioseq_scan_state = scan_state::from_custom_scan_state(node);
        if (pcxt == nullptr || pcxt->nworkers < 0 || pscan == nullptr ||
            pscan->slot_count != static_cast<std::uint32_t>(pcxt->nworkers + 1) ||
            bioseq_column_parallel_counter_slot(pscan, 0u, pscan->entry_count, pscan->slot_count) ==
                nullptr) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqScan DSM reinitialization is invalid")));
        }

        /* Reset the atomic stripe cursor to its initial value. */
        pg_atomic_init_u64(&pscan->next_stripe_id, 1);
        std::memset(reinterpret_cast<char*>(pscan) + pscan->counters_offset,
                    0,
                    mul_size(pscan->slot_count, pscan->slot_stride));

        if (parallel_leader_participation)
            bioseq_scan_state->parallel_scan = pscan;
        else
            bioseq_scan_state->parallel_scan = nullptr;
    }

    /** @brief Restores the leader's snapshot in a worker and attaches its source provider and buffer to it. */
    void bioseq_column_initialize_worker_custom_scan(CustomScanState* node,
                                                     shm_toc* toc,
                                                     void* coordinate) {
        scan_state* bioseq_scan_state = scan_state::from_custom_scan_state(node);
        parallel_column_scan_handle pscan =
            reinterpret_cast<parallel_column_scan_handle>(coordinate);
        if (pscan == nullptr || pscan->slot_count == 0u || ParallelWorkerNumber < 0 ||
            static_cast<std::uint32_t>(ParallelWorkerNumber) >= pscan->slot_count - 1u ||
            bioseq_column_parallel_counter_slot(
                pscan,
                1u + static_cast<std::uint32_t>(ParallelWorkerNumber),
                pscan->entry_count,
                pscan->slot_count) == nullptr) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqScan worker counter coordinate is invalid")));
        }
        bioseq_scan_state->parallel_scan = pscan;
        if (bioseq_scan_state->owns_snapshot_registration) {
            UnregisterSnapshot(bioseq_scan_state->snapshot);
            bioseq_scan_state->owns_snapshot_registration = false;
        }
        bioseq_scan_state->snapshot = RegisterSnapshot(RestoreSnapshot(pscan->snapshot_data));
        bioseq_scan_state->owns_snapshot_registration = true;
        bioseq_scan_runtime& runtime = bioseq_runtime(bioseq_scan_state);
        if (runtime.lazy_source_provider != nullptr) {
            bioseq::storage::rebind_pg_sequence_source_provider_snapshot(
                runtime.lazy_source_provider, bioseq_scan_state->snapshot);
        } else if (runtime.lazy_source_projection || runtime.locator_passthrough) {
            ereport(ERROR,
                    (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                     errmsg("BioseqScan worker source provider is unavailable")));
        }
        runtime.buffer.replace_executor_snapshot(bioseq_scan_state->snapshot);
    }

    /** @brief Formats pushed-down filters as an SQL expresson for EXPLAIN. */
    const char* bioseq_column_pushdown_clauses_str(List* context, List* clauses) {
        Expr* conjunction;

        postgres_list<Expr> clause_list{clauses};
        Assert(clause_list.size() > 0);

        if (clause_list.size() == 1) {
            conjunction = clause_list.get_first();
        } else {
            conjunction = make_andclause(clauses);
        }

        bool use_table_name_prefix = false;
        bool show_implicit_cast = false;
        return deparse_expression(reinterpret_cast<Node*>(conjunction),
                                  context,
                                  use_table_name_prefix,
                                  show_implicit_cast);
    }

    /** @brief Formats projected columns for EXPLAIN, with a message when no columns are read. */
    const char* bioseq_column_projected_columns_str(List* context, List* projected_columns) {
        if (list_length(projected_columns) == 0) {
            return "<columnar optimized out all columns>";
        }

        bool use_table_name_prefix = false;
        bool show_implicit_cast = false;
        return deparse_expression(reinterpret_cast<Node*>(projected_columns),
                                  context,
                                  use_table_name_prefix,
                                  show_implicit_cast);
    }

    /**
     * @brief Builds EXPLAIN Vars for every attribute required by a columnar scan.
     * @throws PostgreSQL ERROR when a required attribute is dropped or unsupported.
     */
    List* bioseq_column_var_needed(scan_state* bioseq_scan_state) {
        ScanState* scan_state = &bioseq_scan_state->custom_scan_state.ss;

        postgres_list<Var> var_list;

        bioseq::postgres::support::postgres_bitmapset needed_attr_set{
            collect_needed_column_attributes(scan_state,
                                             bioseq_scan_state->vectorization.qualifiers.get())};

        for (int bms_member : needed_attr_set) {
            Relation bioseq_column_relation = scan_state->ss_currentRelation;

            /* The attribute bitmap uses zero-based  column numbers. */
            Form_pg_attribute column_form =
                bioseq::postgres::support::tuple_descriptor_attribute(RelationGetDescr(bioseq_column_relation), bms_member);
            if (column_form->attisdropped) {
                bioseq_report<logging_level::error, logging_target::postgres>()
                    .with_sql_error_code(ERRCODE_UNDEFINED_COLUMN)
                    .with_message("cannot explain column with attrNum={} "
                                  "of columnar table {} since it is dropped",
                                  bms_member + 1,
                                  RelationGetRelationName(bioseq_column_relation));
            } else if (column_form->attnum <= 0) {
                /* The collector expands whole-row references and omits ctid/tableoid.
                 * It rejects unsupported system columns before this, point. */
                bioseq_report<logging_level::error, logging_target::postgres>()
                    .with_sql_error_code(ERRCODE_FEATURE_NOT_SUPPORTED)
                    .with_message("cannot explain column with attrNum={} "
                                  "of columnar table {} since it is either "
                                  "a system column or a whole-row "
                                  "reference",
                                  column_form->attnum,
                                  RelationGetRelationName(bioseq_column_relation));
            }

            /* These Vars refer to the current query's scan relation. */
            Index varlevelsup = 0;

            CustomScan* custom_scan =
                reinterpret_cast<CustomScan*>(bioseq_scan_state->executor_plan_state()->plan);
            Index scanrelid = custom_scan->scan.scanrelid;
            Var* var = makeVar(scanrelid,
                               column_form->attnum,
                               column_form->atttypid,
                               column_form->atttypmod,
                               column_form->attcollation,
                               varlevelsup);
            var_list.append(var);
        }

        return var_list.release();
    }

    /** @brief Builds the expression-printing context from the plan inside a PlanState. */
    List* set_deparse_context_planstate(List* dpcontext, Node* node, List* ancestors) {
        PlanState* ps = reinterpret_cast<PlanState*>(node);
        return set_deparse_context_plan(dpcontext, ps->plan, ancestors);
    }

    /** Returns the stable PostgreSQL executor callbacks for base Bioseq scans. */
    [[nodiscard]] const CustomExecMethods* bioseq_column_scan_execute_methods() noexcept {
        static const CustomExecMethods methods{
            .CustomName = "BioseqScan",
            .BeginCustomScan = bioseq_column_scan_begin_custom_scan,
            .ExecCustomScan = bioseq_column_scan_exec_custom_scan,
            .EndCustomScan = bioseq_column_scan_end_custom_scan,
            .ReScanCustomScan = bioseq_column_scan_rescan_custom_scan,
            .EstimateDSMCustomScan = bioseq_column_estimate_dsm_custom_scan,
            .InitializeDSMCustomScan = bioseq_column_initialize_dsm_custom_scan,
            .ReInitializeDSMCustomScan = bioseq_column_reinitialize_dsm_custom_scan,
            .InitializeWorkerCustomScan = bioseq_column_initialize_worker_custom_scan,
            .ExplainCustomScan = bioseq_column_scan_explain_custom_scan,
        };
        return &methods;
    }

} // export custom-scan partition declarations
