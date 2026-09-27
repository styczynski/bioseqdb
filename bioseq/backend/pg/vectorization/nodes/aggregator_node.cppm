/**
 * @file
 * @brief Register the PostgreSQL custom scan for vector aggregation.
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
/** Initialize the aggregate executor and connect its input and result slots to the custom scan. */
static void begin_vector_aggregate(CustomScanState* custom_scan_state,
                                   EState* executor_state,
                                   int executor_flags) {
    vector_aggregate_state* vector_state =
        reinterpret_cast<vector_aggregate_state*>(custom_scan_state);
    CustomScan* custom_scan = reinterpret_cast<CustomScan*>(custom_scan_state->ss.ps.plan);
    Agg* aggregate_node = reinterpret_cast<Agg*>(linitial(custom_scan->custom_plans));

    /* Clear the custom scan's result and scan slots before initialization. */
    ExecClearTuple(custom_scan_state->ss.ps.ps_ResultTupleSlot);
    ExecClearTuple(custom_scan_state->ss.ss_ScanTupleSlot);

    vector_state->aggregate_state =
        initialize_vector_aggregate_executor(aggregate_node, executor_state, executor_flags);
    outerPlanState(vector_state) = outerPlanState(vector_state->aggregate_state);

    /* Set the result type and reuse the aggregate executor's output slot. */
    ExecInitResultTypeTL(&vector_state->custom_scan_state.ss.ps);

    vector_state->custom_scan_state.ss.ps.ps_ResultTupleSlot =
        vector_state->aggregate_state->ss.ps.ps_ResultTupleSlot;
}

/** Produces the next vector aggregate result tuple. */
static TupleTableSlot* execute_vector_aggregate(CustomScanState* custom_scan_state) {
    return execute_aggregate_plan(reinterpret_cast<PlanState*>(custom_scan_state));
}

/** Release the aggregate executor and its resources. */
static void end_vector_aggregate(CustomScanState* custom_scan_state) {
    ExecEndAgg(reinterpret_cast<vector_aggregate_state*>(custom_scan_state)->aggregate_state);
}

/** Return the callbacks used to run the vector aggregate custom scan. */
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

/** Allocates the PostgreSQL custom-scan state for vector aggregation. */
static Node* create_vector_aggregate_state(CustomScan* custom_plan) {
    (void)custom_plan;
    vector_aggregate_state* vector_state = allocate_zeroed_object<vector_aggregate_state>();
    NodeSetTag(&vector_state->custom_scan_state, T_CustomScanState);

    CustomScanState* custom_scan_state = &vector_state->custom_scan_state;
    custom_scan_state->methods = vector_aggregate_exec_methods();

    return reinterpret_cast<Node*>(custom_scan_state);
}

/** Return the callback that creates executor state for a vector aggregate plan. */
static const CustomScanMethods* vector_aggregate_scan_methods() noexcept {
    static const CustomScanMethods methods{
        .CustomName = "VectorAggNode",
        .CreateCustomScanState = create_vector_aggregate_state,
    };
    return &methods;
}

} /* close outer extern "C" */

/** Create an empty vector aggregate plan node with its custom-scan methods. */
export CustomScan* create_vector_aggregate_scan(void) {
    CustomScan* custom_scan = makeNode(CustomScan);
    custom_scan->methods = vector_aggregate_scan_methods();
    return custom_scan;
}

/** Registers the vector aggregate scan callbacks with PostgreSQL. */
export void register_vector_aggregate_scan(void) {
    RegisterCustomScanMethods(vector_aggregate_scan_methods());
}
