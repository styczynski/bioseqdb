/**
 * @file
 * @brief Builds SliceProject plans and provides their planner and executor callbacks.
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

export module bioseq.postgres.table:custom_scan_slice_path_plan_construction;

import std;
import bioseq.postgres.support;

import :custom_scan_slice_path_runtime_contract;

using namespace bioseq::postgres::support;

export {
    [[nodiscard]] Plan* bioseq_slice_project_plan_custom_path(PlannerInfo*,
                                                              RelOptInfo* rel,
                                                              CustomPath* best_path,
                                                              List* tlist,
                                                              List* clauses,
                                                              List* custom_plans) {
        if (rel == nullptr || best_path == nullptr || list_length(custom_plans) != 1 ||
            list_length(best_path->custom_private) != 5) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqSliceProject path shape is invalid")));
        }
        List* header = reinterpret_cast<List*>(list_nth(best_path->custom_private, 0));
        List* expressions = reinterpret_cast<List*>(list_nth(best_path->custom_private, 1));
        List* nodes = reinterpret_cast<List*>(list_nth(best_path->custom_private, 2));
        List* records = reinterpret_cast<List*>(list_nth(best_path->custom_private, 3));
        List* output_bindings = reinterpret_cast<List*>(list_nth(best_path->custom_private, 4));
        if (header == nullptr || !IsA(header, List) ||
            (expressions != NIL && !IsA(expressions, List)) ||
            (nodes != NIL && !IsA(nodes, List)) || (records != NIL && !IsA(records, List)) ||
            (output_bindings != NIL && !IsA(output_bindings, List))) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqSliceProject path codec is invalid")));
        }
        auto* scan = makeNode(CustomScan);
        scan->methods = &bioseq_slice_project_scan_methods();
        scan->scan.scanrelid = 0;
        scan->scan.plan.targetlist = tlist;
        /* The child already has the base filters. This wrapper only projects values.
         * Copying those filters here would run them twice and maue setrefs resolve
         * their base-table Vars against the child projection. */
        (void)clauses;
        scan->scan.plan.qual = NIL;
        scan->custom_plans = custom_plans;
        auto* child_plan = reinterpret_cast<Plan*>(linitial(custom_plans));
        scan->custom_scan_tlist = static_cast<List*>(copyObject(child_plan->targetlist));
        scan->custom_exprs = static_cast<List*>(copyObject(expressions));
        scan->custom_private = list_make4(copyObject(header),
                                          copyObject(nodes),
                                          copyObject(records),
                                          copyObject(output_bindings));
        scan->custom_relids = bms_copy(rel->relids);
        scan->flags = best_path->flags;
        return reinterpret_cast<Plan*>(scan);
    }

    [[nodiscard]] List* bioseq_slice_project_reparameterize_custom_path_by_child(
        PlannerInfo*, List* custom_private, RelOptInfo*) {
        return static_cast<List*>(copyObject(custom_private));
    }

    [[nodiscard]] Node* bioseq_slice_project_create_custom_scan_state(CustomScan*) {
        auto* state = static_cast<slice_project_state*>(palloc0(sizeof(slice_project_state)));
        NodeSetTag(&state->custom_scan_state, T_CustomScanState);
        state->custom_scan_state.methods = &bioseq_slice_project_exec_methods();
        return reinterpret_cast<Node*>(state);
    }

    [[nodiscard]] CustomScanMethods const& bioseq_slice_project_scan_methods() noexcept {
        static const CustomScanMethods methods{
            .CustomName = "BioseqSliceProject",
            .CreateCustomScanState = bioseq_slice_project_create_custom_scan_state,
        };
        return methods;
    }

    [[nodiscard]] CustomPathMethods const& bioseq_slice_project_path_methods() noexcept {
        static const CustomPathMethods methods{
            .CustomName = "BioseqSliceProject",
            .PlanCustomPath = bioseq_slice_project_plan_custom_path,
            .ReparameterizeCustomPathByChild =
                bioseq_slice_project_reparameterize_custom_path_by_child,
        };
        return methods;
    }

} // export
