/**
 * @file
 * @brief Runs PostgreSQL index scans over Bioseq tables.
 *
 * The planner wraps an IndexScan in BioseqIndexScan. PostgreSQL initializes and
 * closes the inner scan, while these fetch callbacks let serial scans request
 * only the table columns needed by the query. Lossy index conditions are checked
 * against fetched rows, and a queue preserves ORDER BY results when the index
 * returns approximate distances.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:index_scan;

import bioseq.common.logging;
import bioseq.postgres.support;
import bioseq.postgres.table.types;
import bioseq.postgres.table.scan_state;
import :access_method;
import :utils;
import bioseq.postgres.column.table_access;

/**
 * A copied tuple and its ORDER BY values, held until the scan can return it
 * in the correct order.
 */
struct reorder_tuple {
    pairingheap_node ph_node;
    HeapTuple htup;
    Datum* orderbyvals;
    bool* orderbynulls;
};

/* Imports supply table checks, column discovery, and index fetch setup. */

/* Match PostgreSQL index-access checks before calling an AM callback. Missing
 * callbacks raise a PostgreSQL error through bioseq_log. */
namespace {

/** @brief Assert that the index relation is ready for access-method calls. */
[[gnu::always_inline]] inline void relation_checks(Relation index_relation) noexcept {
    Assert(RelationIsValid(index_relation));
    Assert(index_relation->rd_indam != nullptr);
    Assert(!ReindexIsProcessingIndex(RelationGetRelid(index_relation)));
}

/** Require one callback in the relation's index access-method routine. */
template <class p_member>
[[gnu::always_inline]] inline void
check_rel_procedure(Relation index_relation, p_member IndexAmRoutine::* proc, const char* pname) {
    if (index_relation->rd_indam->*proc == nullptr)
        bioseq_log<logging_level::error, logging_target::postgres>(
            "function \"{}\" is not defined for index \"{}\"",
            pname,
            RelationGetRelationName(index_relation));
}

} // namespace

/** @brief Open an index scan and keep its relation-cache entry alive until scan end. */
static IndexScanDesc begin_internal_index_scan(Relation index_relation,
                                               int nkeys,
                                               int norderbys,
                                               Snapshot snapshot,
                                               ParallelIndexScanDesc pscan,
                                               bool temp_snap) {
    relation_checks(index_relation);
    check_rel_procedure(index_relation, &IndexAmRoutine::ambeginscan, "ambeginscan");

    if (!(index_relation->rd_indam->ampredlocks))
        PredicateLockRelation(index_relation, snapshot);

    /* Keep the index relation cached for the lifetime of this scan. */
    RelationIncrementReferenceCount(index_relation);

    /* Ask the index access method to allocate its scan descriptor. */
    IndexScanDesc scan = index_relation->rd_indam->ambeginscan(index_relation, nkeys, norderbys);
    /* Save the parallel descriptor and temporary-snapshot ownership flag. */
    scan->parallel_scan = pscan;
    scan->xs_temp_snap = temp_snap;

    return scan;
}

/**
 * @brief Open a Bioseq index scan with the requested table columns.
 *
 * The caller must already hold the required table and index locks.
 */
static IndexScanDesc begin_bioseq_index_scan_descriptor(Relation heap_relation,
                                                        Relation index_relation,
                                                        Snapshot snapshot,
                                                        int nkeys,
                                                        int norderbys,
                                                        Bitmapset* attr_needed) {
    IndexScanDesc scan =
        begin_internal_index_scan(index_relation, nkeys, norderbys, snapshot, nullptr, false);

    /* Attach the table and snapshot to the index scan descriptor. */
    scan->heapRelation = heap_relation;
    scan->xs_snapshot = snapshot;

    /* Read matching table rows through the projected column reader. */
#if PG_VERSION_NUM >= 200000
    bioseq_index_scan_begin_projected(scan, SO_NONE, attr_needed);
#else
    scan->xs_heapfetch = bioseq_index_fetch_begin_extended(heap_relation, attr_needed);
#endif

    return scan;
}

/**
 * @brief Fetch the next row through Bioseq and recheck lossy index matches.
 *
 * When opening a serial scan, collect the columns used by the query and index
 * recheck so the column reader can skip the remaining table columns.
 */
static TupleTableSlot* fetch_next_index_tuple(IndexScanState* node) {
    EState* estate = node->ss.ps.state;
    ScanDirection direction = estate->es_direction;
    /* Combine the executor direction with the index order chosen by the planner. */
    if (ScanDirectionIsBackward((reinterpret_cast<IndexScan*>(node->ss.ps.plan))->indexorderdir)) {
        if (ScanDirectionIsForward(direction))
            direction = BackwardScanDirection;
        else if (ScanDirectionIsBackward(direction))
            direction = ForwardScanDirection;
    }
    IndexScanDesc scandesc = node->iss_ScanDesc;
    ExprContext* econtext = node->ss.ps.ps_ExprContext;
    TupleTableSlot* slot = node->ss.ss_ScanTupleSlot;

    if (scandesc == nullptr) {
        /* Open here for serial execution, including a parallel plan run without
         * workers. Parallel initialization otherwise supplies the descriptor. */
        bioseq::postgres::support::postgres_bitmapset attr_needed{collect_needed_column_attributes(
            &node->ss, (reinterpret_cast<IndexScan*>(node->ss.ps.plan))->indexqualorig)};

        scandesc = begin_bioseq_index_scan_descriptor(node->ss.ss_currentRelation,
                                                      node->iss_RelationDesc,
                                                      estate->es_snapshot,
                                                      node->iss_NumScanKeys,
                                                      node->iss_NumOrderByKeys,
                                                      attr_needed.get());

        attr_needed.clear();

        node->iss_ScanDesc = scandesc;

        /* Runtime keys must be evaluated before the index receives them. */
        if (node->iss_NumRuntimeKeys == 0 || node->iss_RuntimeKeysReady)
            index_rescan(scandesc,
                         node->iss_ScanKeys,
                         node->iss_NumScanKeys,
                         node->iss_OrderByKeys,
                         node->iss_NumOrderByKeys);
    }

    /* Read rows in the requested index direction. */
#if PG_VERSION_NUM >= 200000
    while (table_index_getnext_slot(scandesc, direction, slot))
#else
    while (index_getnext_slot(scandesc, direction, slot))
#endif
    {
        CHECK_FOR_INTERRUPTS();

        /* A lossy index match still needs its original conditions checked on the row. */
        if (scandesc->xs_recheck) {
            econtext->ecxt_scantuple = slot;
            if (!ExecQualAndReset(node->indexqualorig, econtext)) {
                /* Count the rejected match and fetch another row. */
                InstrCountFiltered2(node, 1);
                continue;
            }
        }

        return slot;
    }

    /* The index has no more matching rows. */
    node->iss_ReachedEnd = true;
    return ExecClearTuple(slot);
}

/** @brief Evaluate the current row's exact index ORDER BY values. */
static void evaluate_index_order_by(IndexScanState* node, ExprContext* econtext) {
    bioseq::postgres::support::memory_context_scope ctx_guard{econtext->ecxt_per_tuple_memory};

    int orderby_index = 0;
    for (auto orderby_state :
         bioseq::postgres::support::postgres_list<ExprState>(node->indexorderbyorig)) {
        node->iss_OrderByValues[orderby_index] =
            ExecEvalExpr(orderby_state, econtext, &node->iss_OrderByNulls[orderby_index]);
        orderby_index++;
    }
}

/** Recheck one tuple against the original index qualifier during EvalPlanQual. */
static bool recheck_index_tuple(IndexScanState* node, TupleTableSlot* slot) {
    ExprContext* econtext = node->ss.ps.ps_ExprContext;

    /* Check the replacement row against the original index conditions. */
    econtext->ecxt_scantuple = slot;
    return ExecQualAndReset(node->indexqualorig, econtext);
}

/** Compare two index ORDER BY value vectors using PostgreSQL sort support. */
static int compare_order_by_values(const Datum* adist,
                                   const bool* anulls,
                                   const Datum* bdist,
                                   const bool* bnulls,
                                   IndexScanState* node) {
    for (int orderby_index = 0; orderby_index < node->iss_NumOrderByKeys; orderby_index++) {
        SortSupport ssup = &node->iss_SortSupport[orderby_index];

        /* Index ORDER BY plans use NULLS LAST. Two NULL values compare equal here. */
        if (anulls[orderby_index] && !bnulls[orderby_index])
            return 1;
        if (!anulls[orderby_index] && bnulls[orderby_index])
            return -1;
        if (anulls[orderby_index] && bnulls[orderby_index])
            continue;

        int result = ssup->comparator(adist[orderby_index], bdist[orderby_index], ssup);
        if (result != 0)
            return result;
    }
    return 0;
}

/** Copy one tuple and its order values into the reorder queue. */
static void push_reorder_queue(IndexScanState* node,
                               TupleTableSlot* slot,
                               Datum* orderbyvals,
                               bool* orderbynulls) {
    IndexScanDesc scandesc = node->iss_ScanDesc;
    EState* estate = node->ss.ps.state;
    bioseq::postgres::support::memory_context_scope ctx_guard{estate->es_query_cxt};

    reorder_tuple* rt = bioseq::postgres::support::allocate_zeroed_object<reorder_tuple>();
    rt->htup = ExecCopySlotHeapTuple(slot);
    rt->orderbyvals =
        bioseq::postgres::support::allocate_zeroed_array<Datum>(scandesc->numberOfOrderBys);
    rt->orderbynulls =
        bioseq::postgres::support::allocate_zeroed_array<bool>(scandesc->numberOfOrderBys);
    for (int orderby_index = 0; orderby_index < node->iss_NumOrderByKeys; orderby_index++) {
        if (!orderbynulls[orderby_index])
            rt->orderbyvals[orderby_index] = datumCopy(orderbyvals[orderby_index],
                                                       node->iss_OrderByTypByVals[orderby_index],
                                                       node->iss_OrderByTypLens[orderby_index]);
        else
            rt->orderbyvals[orderby_index] = static_cast<Datum>(0);
        rt->orderbynulls[orderby_index] = orderbynulls[orderby_index];
    }
    pairingheap_add(node->iss_ReorderQueue, &rt->ph_node);
}

/**
 * @brief Remove the first queued tuple and free its copied ordering values.
 *
 * The caller takes ownership of the returned heap tuple.
 */
static HeapTuple pop_reorder_queue(IndexScanState* node) {
    reorder_tuple* topmost =
        reinterpret_cast<reorder_tuple*>(pairingheap_remove_first(node->iss_ReorderQueue));

    HeapTuple result = topmost->htup;
    for (int orderby_index = 0; orderby_index < node->iss_NumOrderByKeys; orderby_index++) {
        if (!node->iss_OrderByTypByVals[orderby_index] && !topmost->orderbynulls[orderby_index])
            pfree(DatumGetPointer(topmost->orderbyvals[orderby_index]));
    }
    pfree(topmost->orderbyvals);
    pfree(topmost->orderbynulls);
    pfree(topmost);

    return result;
}

/**
 * @brief Fetch rows in exact ORDER BY order when index distances can be approximate.
 *
 * Recheck each approximate distance against the row. Hold a row in the queue
 * until the index lower bound proves it can be returned before unread rows.
 */
static TupleTableSlot* fetch_next_reordered_index_tuple(IndexScanState* node) {
    reorder_tuple* topmost = nullptr;
    bool was_exact;
    Datum* lastfetched_vals;
    bool* lastfetched_nulls;

    EState* estate = node->ss.ps.state;

    /* Reordering supports forward scans. PostgreSQL checks backward-scan
     * support before choosing to run this plan in reverse. */
    Assert(
        !ScanDirectionIsBackward((reinterpret_cast<IndexScan*>(node->ss.ps.plan))->indexorderdir));
    Assert(ScanDirectionIsForward(estate->es_direction));

    IndexScanDesc scandesc = node->iss_ScanDesc;
    ExprContext* econtext = node->ss.ps.ps_ExprContext;
    TupleTableSlot* slot = node->ss.ss_ScanTupleSlot;

    if (scandesc == nullptr) {
        bioseq::postgres::support::postgres_bitmapset attr_needed{collect_needed_column_attributes(
            &node->ss, (reinterpret_cast<IndexScan*>(node->ss.ps.plan))->indexqualorig)};

        scandesc = begin_bioseq_index_scan_descriptor(node->ss.ss_currentRelation,
                                                      node->iss_RelationDesc,
                                                      estate->es_snapshot,
                                                      node->iss_NumScanKeys,
                                                      node->iss_NumOrderByKeys,
                                                      attr_needed.get());

        attr_needed.clear();

        node->iss_ScanDesc = scandesc;

        if (node->iss_NumRuntimeKeys == 0 || node->iss_RuntimeKeysReady)
            index_rescan(scandesc,
                         node->iss_ScanKeys,
                         node->iss_NumScanKeys,
                         node->iss_OrderByKeys,
                         node->iss_NumOrderByKeys);
    }

    for (;;) {
        CHECK_FOR_INTERRUPTS();

        /* The index value is a lower bound for unread rows. A queued row at or
         * below it can be returned now. At index end, drain the queue in order. */
        if (!pairingheap_is_empty(node->iss_ReorderQueue)) {
            topmost = reinterpret_cast<reorder_tuple*>(pairingheap_first(node->iss_ReorderQueue));

            if (node->iss_ReachedEnd || compare_order_by_values(topmost->orderbyvals,
                                                                topmost->orderbynulls,
                                                                scandesc->xs_orderbyvals,
                                                                scandesc->xs_orderbynulls,
                                                                node) <= 0) {
                HeapTuple tuple = pop_reorder_queue(node);

                /* The slot takes ownership of this copied heap tuple. */
                ExecForceStoreHeapTuple(tuple, slot, true);
                return slot;
            }
        } else if (node->iss_ReachedEnd) {
            /* Both the index and the reorder queue are exhausted. */
            return ExecClearTuple(slot);
        }

        /* Fetch next tuple from the index. */
    next_indextuple:
#if PG_VERSION_NUM >= 200000
        if (!table_index_getnext_slot(scandesc, ForwardScanDirection, slot))
#else
        if (!index_getnext_slot(scandesc, ForwardScanDirection, slot))
#endif
        {
            /* Finish returning queued rows after the index is exhausted. */
            node->iss_ReachedEnd = true;
            continue;
        }

        /* Recheck lossy index conditions and ORDER BY values on the fetched row. */
        if (scandesc->xs_recheck) {
            econtext->ecxt_scantuple = slot;
            if (!ExecQualAndReset(node->indexqualorig, econtext)) {
                /* Count the rejected match and continue the index scan. */
                InstrCountFiltered2(node, 1);
                CHECK_FOR_INTERRUPTS();
                goto next_indextuple;
            }
        }

        if (scandesc->xs_recheckorderby) {
            econtext->ecxt_scantuple = slot;
            ResetExprContext(econtext);
            evaluate_index_order_by(node, econtext);

            int cmp = compare_order_by_values(node->iss_OrderByValues,
                                              node->iss_OrderByNulls,
                                              scandesc->xs_orderbyvals,
                                              scandesc->xs_orderbynulls,
                                              node);
            if (cmp < 0)
                bioseq_log<logging_level::error, logging_target::postgres>(
                    "index returned tuples in wrong order");
            else if (cmp == 0)
                was_exact = true;
            else
                was_exact = false;
            lastfetched_vals = node->iss_OrderByValues;
            lastfetched_nulls = node->iss_OrderByNulls;
        } else {
            was_exact = true;
            lastfetched_vals = scandesc->xs_orderbyvals;
            lastfetched_nulls = scandesc->xs_orderbynulls;
        }

        /* Queue rows whose order is still uncertain, or which belong after a
         * queued row. An exact row that sorts first can be returned immediately. */
        if (!was_exact || (topmost && compare_order_by_values(lastfetched_vals,
                                                              lastfetched_nulls,
                                                              topmost->orderbyvals,
                                                              topmost->orderbynulls,
                                                              node) > 0)) {
            push_reorder_queue(node, slot, lastfetched_vals, lastfetched_nulls);
            continue;
        }
        return slot;
    }
}

/** @brief Run the scan with ORDER BY rechecking when ordering keys are present. */
static TupleTableSlot* execute_index_scan(PlanState* pstate) {
    index_scan_state* bioseq_index_state = index_scan_state::from_plan_state(pstate);
    IndexScanState* node = castNode(IndexScanState, bioseq_index_state->index_scan_executor_state);

    /* Evaluate runtime keys before fetching the first row. */
    if (node->iss_NumRuntimeKeys != 0 && !node->iss_RuntimeKeysReady)
        ExecReScan(reinterpret_cast<PlanState*>(node));

    if (node->iss_NumOrderByKeys > 0)
        return ExecScan(&node->ss,
                        reinterpret_cast<ExecScanAccessMtd>(fetch_next_reordered_index_tuple),
                        reinterpret_cast<ExecScanRecheckMtd>(recheck_index_tuple));
    return ExecScan(&node->ss,
                    reinterpret_cast<ExecScanAccessMtd>(fetch_next_index_tuple),
                    reinterpret_cast<ExecScanRecheckMtd>(recheck_index_tuple));
}

/** @brief Initialize the shared index scan and open the leader's scan descriptor. */
static void
initialize_parallel_index_scan(IndexScanState* node, ParallelContext* pcxt, void* coordinate) {
    EState* estate = node->ss.ps.state;
    ParallelIndexScanDesc piscan = static_cast<ParallelIndexScanDesc>(coordinate);

    bioseq_initialize_uninstrumented_parallel_index_scan(
        node->ss.ss_currentRelation, node->iss_RelationDesc, estate->es_snapshot, piscan);

    node->iss_ScanDesc =
        bioseq_begin_uninstrumented_parallel_index_scan(node->ss.ss_currentRelation,
                                                        node->iss_RelationDesc,
                                                        node->iss_NumScanKeys,
                                                        node->iss_NumOrderByKeys,
                                                        piscan);

    if (node->iss_NumRuntimeKeys == 0 || node->iss_RuntimeKeysReady)
        index_rescan(node->iss_ScanDesc,
                     node->iss_ScanKeys,
                     node->iss_NumScanKeys,
                     node->iss_OrderByKeys,
                     node->iss_NumOrderByKeys);
}

/** @brief Open a worker's descriptor for the shared index scan. */
static void initialize_parallel_index_scan_worker(IndexScanState* node, void* coordinate) {
    ParallelIndexScanDesc piscan = static_cast<ParallelIndexScanDesc>(coordinate);

    node->iss_ScanDesc =
        bioseq_begin_uninstrumented_parallel_index_scan(node->ss.ss_currentRelation,
                                                        node->iss_RelationDesc,
                                                        node->iss_NumScanKeys,
                                                        node->iss_NumOrderByKeys,
                                                        piscan);

    if (node->iss_NumRuntimeKeys == 0 || node->iss_RuntimeKeysReady)
        index_rescan(node->iss_ScanDesc,
                     node->iss_ScanKeys,
                     node->iss_NumScanKeys,
                     node->iss_OrderByKeys,
                     node->iss_NumOrderByKeys);
}

/** Initialize the wrapped PostgreSQL IndexScan executor state. */
static void begin_bioseq_index_scan(CustomScanState* css, EState* estate, int eflags) {
    index_scan_state* index_state = index_scan_state::from_custom_scan_state(css);

    /* Clear the wrapper's result and scan slots before initializing the inner scan. */
    ExecClearTuple(css->ss.ps.ps_ResultTupleSlot);
    ExecClearTuple(css->ss.ss_ScanTupleSlot);

    index_state->index_scan_executor_state =
        ExecInitIndexScan(index_state->inner_index_scan_plan(), estate, eflags);

    /* Set the wrapper result type from its target list. */
    ExecInitResultTypeTL(&index_state->custom_scan_state.ss.ps);
}

/** @brief Fetch one result from the wrapped index scan. */
static TupleTableSlot* execute_bioseq_index_scan(CustomScanState* node) {
    return execute_index_scan(reinterpret_cast<PlanState*>(node));
}

/** End the wrapped PostgreSQL IndexScan. */
static void end_bioseq_index_scan(CustomScanState* node) {
    ExecEndIndexScan(index_scan_state::from_custom_scan_state(node)->index_scan_executor_state);
}

/** Render one expression through PostgreSQL's EXPLAIN deparser. */
static void show_expression(Node* node,
                            const char* qlabel,
                            PlanState* planstate,
                            List* ancestors,
                            bool useprefix,
                            ExplainState* es) {
    List* context = set_deparse_context_plan(es->deparse_cxt, planstate->plan, ancestors);
    char* exprstr = deparse_expression(node, context, useprefix, false);
    ExplainPropertyText(qlabel, exprstr, es);
}

/** Render a qualifier list with PostgreSQL's implicit AND semantics. */
static void show_qual(List* qual,
                      const char* qlabel,
                      PlanState* planstate,
                      List* ancestors,
                      bool useprefix,
                      ExplainState* es) {
    if (qual == NIL)
        return;

    Node* and_node = reinterpret_cast<Node*>(make_ands_explicit(qual));
    show_expression(and_node, qlabel, planstate, ancestors, useprefix, es);
}

/** @brief Print scan conditions, adding relation names for verbose or subquery output. */
static void show_scan_qual(
    List* qual, const char* qlabel, PlanState* planstate, List* ancestors, ExplainState* es) {
    bool useprefix = (IsA(planstate->plan, SubqueryScan) || es->verbose);
    show_qual(qual, qlabel, planstate, ancestors, useprefix, es);
}

/** @brief Print the average filtered-row count per loop for EXPLAIN ANALYZE. */
static void
show_instrumentation_count(const char* qlabel, int which, PlanState* planstate, ExplainState* es) {
    if (!es->analyze || !planstate->instrument)
        return;

    double nfiltered =
        (which == 2) ? planstate->instrument->nfiltered2 : planstate->instrument->nfiltered1;
    double nloops = planstate->instrument->nloops;

    /* Text EXPLAIN omits zero counts. Structured output keeps the field. */
    if (nfiltered > 0 || es->format != EXPLAIN_FORMAT_TEXT) {
        if (nloops > 0)
            ExplainPropertyFloat(qlabel, nullptr, nfiltered / nloops, 0, es);
        else
            ExplainPropertyFloat(qlabel, nullptr, 0.0, 0, es);
    }
}

/** Explain the wrapped index conditions, ordering, filters, and counters. */
static void explain_bioseq_index_scan(CustomScanState* node, List* ancestors, ExplainState* es) {
    index_scan_state* index_state = index_scan_state::from_custom_scan_state(node);
    IndexScan* inner_index_scan = index_state->inner_index_scan_plan();

    const char* indexname = get_rel_name(inner_index_scan->indexid);

    ExplainPropertyText("BioseqIndexScan using ", indexname, es);

    show_scan_qual(inner_index_scan->indexqualorig,
                   "Index Cond",
                   &index_state->custom_scan_state.ss.ps,
                   ancestors,
                   es);

    if (inner_index_scan->indexqualorig)
        show_instrumentation_count(
            "Rows Removed by Index Recheck", 2, &index_state->custom_scan_state.ss.ps, es);

    show_scan_qual(inner_index_scan->indexorderbyorig,
                   "Order By",
                   &index_state->custom_scan_state.ss.ps,
                   ancestors,
                   es);

    show_scan_qual(
        node->ss.ps.plan->qual, "Filter", &index_state->custom_scan_state.ss.ps, ancestors, es);

    if (node->ss.ps.plan->qual)
        show_instrumentation_count(
            "Rows Removed by Filter", 1, &index_state->custom_scan_state.ss.ps, es);
}

/** @brief Ask PostgreSQL for the parallel index scan's shared-memory size. */
static Size estimate_parallel_bioseq_index_scan(CustomScanState* node, ParallelContext* pcxt) {
    index_scan_state* index_state = index_scan_state::from_custom_scan_state(node);
    ExecIndexScanEstimate(index_state->index_scan_executor_state, pcxt);
    return index_state->index_scan_executor_state->iss_PscanLen;
}

/** Initialize the wrapped parallel index scan in the leader. */
static void initialize_parallel_bioseq_index_scan(CustomScanState* node,
                                                  ParallelContext* pcxt,
                                                  void* coordinate) {
    index_scan_state* index_state = index_scan_state::from_custom_scan_state(node);
    initialize_parallel_index_scan(index_state->index_scan_executor_state, pcxt, coordinate);
}

/** @brief Reset shared index scan state before a parallel rescan. */
static void reinitialize_parallel_bioseq_index_scan(CustomScanState* node,
                                                    ParallelContext* pcxt,
                                                    void* coordinate) {
    index_scan_state* index_state = index_scan_state::from_custom_scan_state(node);
    ExecIndexScanReInitializeDSM(index_state->index_scan_executor_state, pcxt);
}

/** Attach a worker to the wrapped parallel index scan. */
static void initialize_parallel_bioseq_index_scan_worker(CustomScanState* node,
                                                         shm_toc* toc,
                                                         void* coordinate) {
    index_scan_state* index_state = index_scan_state::from_custom_scan_state(node);
    initialize_parallel_index_scan_worker(index_state->index_scan_executor_state, coordinate);
}

/** @brief Return the BioseqIndexScan executor callbacks. */
[[nodiscard]] static const CustomExecMethods& index_scan_execution_methods() noexcept {
    static const CustomExecMethods methods = {
        .CustomName = "BioseqIndexScan",
        .BeginCustomScan = begin_bioseq_index_scan,
        .ExecCustomScan = execute_bioseq_index_scan,
        .EndCustomScan = end_bioseq_index_scan,
        .EstimateDSMCustomScan = estimate_parallel_bioseq_index_scan,
        .InitializeDSMCustomScan = initialize_parallel_bioseq_index_scan,
        .ReInitializeDSMCustomScan = reinitialize_parallel_bioseq_index_scan,
        .InitializeWorkerCustomScan = initialize_parallel_bioseq_index_scan_worker,
        .ExplainCustomScan = explain_bioseq_index_scan,
    };
    return methods;
}

/** Allocate executor state for a planned Bioseq index scan. */
static Node* create_index_scan_custom_state(CustomScan* custom_plan) {
    index_scan_state* index_state = index_scan_state::allocate();
    index_state->as_custom_scan_state()->methods = &index_scan_execution_methods();
    return index_state->as_node();
}

/** @brief Return the callback that creates BioseqIndexScan executor state. */
[[nodiscard]] static const CustomScanMethods& index_scan_plan_methods() noexcept {
    static const CustomScanMethods methods = {
        .CustomName = "BioseqIndexScan",
        .CreateCustomScanState = create_index_scan_custom_state,
    };
    return methods;
}

/** Create an empty Bioseq index-scan wrapper node for planner rewrites. */
export CustomScan* bioseq_create_index_scan_node(void) {
    CustomScan* cscan = reinterpret_cast<CustomScan*>(makeNode(CustomScan));
    cscan->methods = &index_scan_plan_methods();
    return cscan;
}

/** Register the Bioseq index-scan methods with PostgreSQL. */
export void bioseq_register_index_scan_node(void) {
    RegisterCustomScanMethods(&index_scan_plan_methods());
}
