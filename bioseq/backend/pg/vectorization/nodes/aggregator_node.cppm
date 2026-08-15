/**
 * @file
 * @brief Exposes the stable vector aggregate custom-scan callbacks.
 */
module;
#include <bioseq/pg_compat.h>
extern "C" {
#include "nodes/extensible.h"
}

export module bioseq.postgres.vectorization.aggregator_node;

import bioseq.postgres.support;
import :support;
import :transitions;
import :hash_runtime;
import :execution;
import :initialization;

using bioseq::postgres::support::allocate_zeroed_object;

extern "C" {
/* The state constructor and executor table form the method-table exception. */
static const CustomExecMethods* vector_aggregate_exec_methods() noexcept;

/** Allocates the PostgreSQL custom-scan state for vector aggregation. */
static Node* create_vector_aggregate_state(CustomScan* custom_plan) {
    (void)custom_plan;
    auto* vector_state = allocate_zeroed_object<vector_aggregate_state>();
    NodeSetTag(&vector_state->custom_scan_state, T_CustomScanState);

    CustomScanState* custom_scan_state = &vector_state->custom_scan_state;
    custom_scan_state->methods = vector_aggregate_exec_methods();

    return reinterpret_cast<Node*>(custom_scan_state);
}

/** Initializes the copied AggState and connects it to the custom scan. */
static void begin_vector_aggregate(CustomScanState* custom_scan_state,
                                   EState* executor_state,
                                   int executor_flags) {
    vector_aggregate_state* vector_state =
        reinterpret_cast<vector_aggregate_state*>(custom_scan_state);
    CustomScan* custom_scan = reinterpret_cast<CustomScan*>(custom_scan_state->ss.ps.plan);
    Agg* aggregate_node = reinterpret_cast<Agg*>(linitial(custom_scan->custom_plans));

    /* Clean out the tuple table */
    ExecClearTuple(custom_scan_state->ss.ps.ps_ResultTupleSlot);
    ExecClearTuple(custom_scan_state->ss.ss_ScanTupleSlot);

    vector_state->aggregate_state =
        initialize_vector_aggregate_executor(aggregate_node, executor_state, executor_flags);
    outerPlanState(vector_state) = outerPlanState(vector_state->aggregate_state);

    /* Initialize result type and projection. */
    ExecInitResultTypeTL(&vector_state->custom_scan_state.ss.ps);

    vector_state->custom_scan_state.ss.ps.ps_ResultTupleSlot =
        vector_state->aggregate_state->ss.ps.ps_ResultTupleSlot;
}

/** Produces the next vector aggregate result tuple. */
static TupleTableSlot* execute_vector_aggregate(CustomScanState* node) {
    return execute_aggregate_plan(reinterpret_cast<PlanState*>(node));
}

/** Releases the copied aggregate executor. */
static void end_vector_aggregate(CustomScanState* node) {
    ExecEndAgg(reinterpret_cast<vector_aggregate_state*>(node)->aggregate_state);
}

/** Returns the stable executor callback table for vector aggregation. */
static const CustomExecMethods* vector_aggregate_exec_methods() noexcept {
    static const CustomExecMethods methods{
        .CustomName = "VectorAggNode",
        .BeginCustomScan = begin_vector_aggregate,
        .ExecCustomScan = execute_vector_aggregate,
        .EndCustomScan = end_vector_aggregate,
        .EstimateDSMCustomScan = nullptr,
        .InitializeDSMCustomScan = nullptr,
        .ReInitializeDSMCustomScan = nullptr,
        .InitializeWorkerCustomScan = nullptr,
        .ExplainCustomScan = nullptr,
    };
    return &methods;
}

/** Returns the stable planner callback table for vector aggregation. */
static const CustomScanMethods* vector_aggregate_scan_methods() noexcept {
    static const CustomScanMethods methods{
        .CustomName = "VectorAggNode",
        .CreateCustomScanState = create_vector_aggregate_state,
    };
    return &methods;
}

} /* close outer extern "C" */

/** Creates an unregistered vector aggregate custom-scan plan node. */
export CustomScan* create_vector_aggregate_scan(void) {
    CustomScan* custom_scan = makeNode(CustomScan);
    custom_scan->methods = vector_aggregate_scan_methods();
    return custom_scan;
}

/** Registers the vector aggregate scan callbacks with PostgreSQL. */
export void register_vector_aggregate_scan(void) {
    RegisterCustomScanMethods(vector_aggregate_scan_methods());
}
