/**
 * @file
 * @brief Bioseq custom executor node for PostgreSQL index scans.
 *
 * The planner hook rewrites a vanilla
 * IndexScan plan into a CustomScan whose state is a index_scan_state
 * (CustomScanState first, IndexScanState* second). The execution methods
 * delegate to PostgreSQL's IndexScan executor while reusing bioseq's
 * tuple-fetch path so projected columns can be pushed into the columnar
 * reader.
 *
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

/*
 * When an ordering operator is used, tuples fetched from the index that
 * need to be reordered are queued in a pairing heap as ReorderTuples.
 */
struct reorder_tuple
{
    pairingheap_node ph_node;
    HeapTuple htup;
    Datum* orderbyvals;
    bool* orderbynulls;
};

/* Table identity, attribute discovery, and index-fetch construction are
 * imported from their leaf modules to keep this executor adapter acyclic. */



/*
 * relation_checks / check_rel_procedure mirror PostgreSQL's internal
 * RELATION_CHECKS / CHECK_REL_PROCEDURE macros. relation_checks runs the
 * three relcache sanity Asserts; check_rel_procedure raises the same
 * ereport(ERROR) (via bioseq_log<logging_level::error, logging_target::postgres>, which longjmps) when an index AM
 * routine pointer is missing. The member-pointer template preserves the
 * macro's token-paste intent (the procedure name is a real argument).
 */
namespace {

[[gnu::always_inline]] inline void relation_checks(Relation index_relation) noexcept
{
    Assert(RelationIsValid(index_relation));
    Assert(PointerIsValid(index_relation->rd_indam));
    Assert(!ReindexIsProcessingIndex(RelationGetRelid(index_relation)));
}

template <class member>
[[gnu::always_inline]] inline void
check_rel_procedure(Relation index_relation, member IndexAmRoutine::* proc, const char* pname)
{
    if (index_relation->rd_indam->*proc == nullptr)
        bioseq_log<logging_level::error, logging_target::postgres>("function \"{}\" is not defined for index \"{}\"",
                            pname, RelationGetRelationName(index_relation));
}

}


static IndexScanDesc
begin_internal_index_scan(Relation index_relation,
                         int nkeys, int norderbys, Snapshot snapshot,
                         ParallelIndexScanDesc pscan, bool temp_snap)
{
    relation_checks(index_relation);
    check_rel_procedure(index_relation, &IndexAmRoutine::ambeginscan, "ambeginscan");

    if (!(index_relation->rd_indam->ampredlocks))
        PredicateLockRelation(index_relation, snapshot);

    /* We hold a reference count to the relcache entry throughout the scan. */
    RelationIncrementReferenceCount(index_relation);

    /* Tell the AM to open a scan. */
    IndexScanDesc scan = index_relation->rd_indam->ambeginscan(index_relation, nkeys, norderbys);
    /* Initialize information for parallel scan. */
    scan->parallel_scan = pscan;
    scan->xs_temp_snap = temp_snap;

    return scan;
}

/*
 * Bioseq variant of index_beginscan. Caller must already hold suitable
 * locks on the heap and the index.
 */
static IndexScanDesc
begin_bioseq_index_scan_descriptor(Relation heap_relation,
                       Relation index_relation,
                       Snapshot snapshot,
                       int nkeys, int norderbys,
                       Bitmapset* attr_needed)
{
    IndexScanDesc scan = begin_internal_index_scan(index_relation, nkeys, norderbys, snapshot, nullptr, false);

    /*
     * Save additional parameters into the scandesc.  Everything else was set
     * up by RelationGetIndexScan.
     */
    scan->heapRelation = heap_relation;
    scan->xs_snapshot = snapshot;

    /* prepare to fetch index matches from table */
    scan->xs_heapfetch = bioseq_index_fetch_begin_extended(heap_relation, attr_needed);

    return scan;
}

/*
 * fetch_next_index_tuple retrieves the next tuple from the IndexScanState's relation
 * using the bioseq-specific xs_heapfetch path so projected columns are
 * read through the columnar reader.
 */
static TupleTableSlot*
fetch_next_index_tuple(IndexScanState* node)
{
    EState* estate = node->ss.ps.state;
    ScanDirection direction = estate->es_direction;
    /* Flip direction if this is an overall backward scan */
    if (ScanDirectionIsBackward((reinterpret_cast<IndexScan*>(node->ss.ps.plan))->indexorderdir))
    {
        if (ScanDirectionIsForward(direction))
            direction = BackwardScanDirection;
        else if (ScanDirectionIsBackward(direction))
            direction = ForwardScanDirection;
    }
    IndexScanDesc scandesc = node->iss_ScanDesc;
    ExprContext* econtext = node->ss.ps.ps_ExprContext;
    TupleTableSlot* slot = node->ss.ss_ScanTupleSlot;

    if (scandesc == nullptr)
    {
        /*
         * We reach here if the index scan is not parallel, or if we're
         * serially executing an index scan that was planned to be parallel.
         */
        bioseq::postgres::support::postgres_bitmapset attr_needed = collect_needed_column_attributes(
            &node->ss,
            (reinterpret_cast<IndexScan*>(node->ss.ps.plan))->indexqualorig);

        scandesc = begin_bioseq_index_scan_descriptor(node->ss.ss_currentRelation,
                                          node->iss_RelationDesc,
                                          estate->es_snapshot,
                                          node->iss_NumScanKeys,
                                          node->iss_NumOrderByKeys,
                                          attr_needed);

        attr_needed.free();

        node->iss_ScanDesc = scandesc;

        /*
         * If no run-time keys to calculate or they are ready, go ahead and
         * pass the scankeys to the index AM.
         */
        if (node->iss_NumRuntimeKeys == 0 || node->iss_RuntimeKeysReady)
            index_rescan(scandesc,
                         node->iss_ScanKeys, node->iss_NumScanKeys,
                         node->iss_OrderByKeys, node->iss_NumOrderByKeys);
    }

    /* fetch the next tuple */
    while (index_getnext_slot(scandesc, direction, slot))
    {
        CHECK_FOR_INTERRUPTS();

        /*
         * If the index was lossy, we have to recheck the index quals using
         * the fetched tuple.
         */
        if (scandesc->xs_recheck)
        {
            econtext->ecxt_scantuple = slot;
            if (!ExecQualAndReset(node->indexqualorig, econtext))
            {
                /* Fails recheck, so drop it and loop back for another */
                InstrCountFiltered2(node, 1);
                continue;
            }
        }

        return slot;
    }

    /*
     * If we get here it means the index scan failed so we are at the end of
     * the scan.
     */
    node->iss_ReachedEnd = true;
    return ExecClearTuple(slot);
}

/*
 * Calculate the expressions in the ORDER BY clause, based on the heap tuple.
 */
static void
evaluate_index_order_by(IndexScanState* node, ExprContext* econtext)
{
    bioseq::postgres::support::memory_context_scope ctx_guard{
        econtext->ecxt_per_tuple_memory};

    int orderby_index = 0;
    for (auto orderby_state : bioseq::postgres::support::postgres_list<ExprState>(node->indexorderbyorig))
    {
        node->iss_OrderByValues[orderby_index] = ExecEvalExpr(orderby_state,
                                                              econtext,
                                                              &node->iss_OrderByNulls[orderby_index]);
        orderby_index++;
    }
}

/* recheck_index_tuple -- access method routine to recheck a tuple in EvalPlanQual */
static bool
recheck_index_tuple(IndexScanState* node, TupleTableSlot* slot)
{
    ExprContext* econtext = node->ss.ps.ps_ExprContext;

    /* Does the tuple meet the indexqual condition? */
    econtext->ecxt_scantuple = slot;
    return ExecQualAndReset(node->indexqualorig, econtext);
}


/*
 * Compare ORDER BY expression values.
 */
static int
compare_order_by_values(const Datum* adist, const bool* anulls,
                const Datum* bdist, const bool* bnulls,
                IndexScanState* node)
{
    for (int orderby_index = 0; orderby_index < node->iss_NumOrderByKeys; orderby_index++)
    {
        SortSupport ssup = &node->iss_SortSupport[orderby_index];

        /*
         * Handle nulls.  We only need to support NULLS LAST ordering, because
         * match_pathkeys_to_index() doesn't consider indexorderby
         * implementation otherwise.
         */
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


/* Helper function to push a tuple to the reorder queue. */
static void
push_reorder_queue(IndexScanState* node, TupleTableSlot* slot,
                  Datum* orderbyvals, bool* orderbynulls)
{
    IndexScanDesc scandesc = node->iss_ScanDesc;
    EState* estate = node->ss.ps.state;
    bioseq::postgres::support::memory_context_scope ctx_guard{estate->es_query_cxt};

    reorder_tuple* rt = bioseq::postgres::support::allocate_zeroed_object<reorder_tuple>();
    rt->htup = ExecCopySlotHeapTuple(slot);
    rt->orderbyvals = bioseq::postgres::support::allocate_zeroed_array<Datum>(scandesc->numberOfOrderBys);
    rt->orderbynulls = bioseq::postgres::support::allocate_zeroed_array<bool>(scandesc->numberOfOrderBys);
    for (int orderby_index = 0; orderby_index < node->iss_NumOrderByKeys; orderby_index++)
    {
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

/* Helper function to pop the next tuple from the reorder queue. */
static HeapTuple
pop_reorder_queue(IndexScanState* node)
{
    reorder_tuple* topmost = reinterpret_cast<reorder_tuple*>(pairingheap_remove_first(node->iss_ReorderQueue));

    HeapTuple result = topmost->htup;
    for (int orderby_index = 0; orderby_index < node->iss_NumOrderByKeys; orderby_index++)
    {
        if (!node->iss_OrderByTypByVals[orderby_index] && !topmost->orderbynulls[orderby_index])
            pfree(DatumGetPointer(topmost->orderbyvals[orderby_index]));
    }
    pfree(topmost->orderbyvals);
    pfree(topmost->orderbynulls);
    pfree(topmost);

    return result;
}


/*
 * fetch_next_reordered_index_tuple is like fetch_next_index_tuple but additionally re-checks
 * ORDER BY expressions and reorders tuples through a pairing heap.
 */
static TupleTableSlot*
fetch_next_reordered_index_tuple(IndexScanState* node)
{
    reorder_tuple* topmost = nullptr;
    bool was_exact;
    Datum* lastfetched_vals;
    bool* lastfetched_nulls;

    EState* estate = node->ss.ps.state;

    /*
     * Only forward scan is supported with reordering.  Note: we can get away
     * with just Asserting here because the system will not try to run the
     * plan backwards if ExecSupportsBackwardScan() says it won't work.
     */
    Assert(!ScanDirectionIsBackward((reinterpret_cast<IndexScan*>(node->ss.ps.plan))->indexorderdir));
    Assert(ScanDirectionIsForward(estate->es_direction));

    IndexScanDesc scandesc = node->iss_ScanDesc;
    ExprContext* econtext = node->ss.ps.ps_ExprContext;
    TupleTableSlot* slot = node->ss.ss_ScanTupleSlot;

    if (scandesc == nullptr)
    {
        bioseq::postgres::support::postgres_bitmapset attr_needed = collect_needed_column_attributes(
            &node->ss,
            (reinterpret_cast<IndexScan*>(node->ss.ps.plan))->indexqualorig);

        scandesc = begin_bioseq_index_scan_descriptor(node->ss.ss_currentRelation,
                                          node->iss_RelationDesc,
                                          estate->es_snapshot,
                                          node->iss_NumScanKeys,
                                          node->iss_NumOrderByKeys,
                                          attr_needed);

        attr_needed.free();

        node->iss_ScanDesc = scandesc;

        if (node->iss_NumRuntimeKeys == 0 || node->iss_RuntimeKeysReady)
            index_rescan(scandesc,
                         node->iss_ScanKeys, node->iss_NumScanKeys,
                         node->iss_OrderByKeys, node->iss_NumOrderByKeys);
    }

    for (;;)
    {
        CHECK_FOR_INTERRUPTS();

        /*
         * Check the reorder queue first.  If the topmost tuple in the queue
         * has an ORDER BY value smaller than (or equal to) the value last
         * returned by the index, we can return it now.
         */
        if (!pairingheap_is_empty(node->iss_ReorderQueue))
        {
            topmost = reinterpret_cast<reorder_tuple*>(pairingheap_first(node->iss_ReorderQueue));

            if (node->iss_ReachedEnd ||
                compare_order_by_values(topmost->orderbyvals,
                                topmost->orderbynulls,
                                scandesc->xs_orderbyvals,
                                scandesc->xs_orderbynulls,
                                node) <= 0)
            {
                HeapTuple tuple = pop_reorder_queue(node);

                /* Pass 'true', as the tuple in the queue is a palloc'd copy */
                ExecForceStoreHeapTuple(tuple, slot, true);
                return slot;
            }
        }
        else if (node->iss_ReachedEnd)
        {
            /* Queue is empty, and no more tuples from index.  We're done. */
            return ExecClearTuple(slot);
        }

        /* Fetch next tuple from the index. */
    next_indextuple:
        if (!index_getnext_slot(scandesc, ForwardScanDirection, slot))
        {
            /*
             * No more tuples from the index.  But we still need to drain any
             * remaining tuples from the queue before we're done.
             */
            node->iss_ReachedEnd = true;
            continue;
        }

        /*
         * If the index was lossy, we have to recheck the index quals and
         * ORDER BY expressions using the fetched tuple.
         */
        if (scandesc->xs_recheck)
        {
            econtext->ecxt_scantuple = slot;
            if (!ExecQualAndReset(node->indexqualorig, econtext))
            {
                /* Fails recheck, so drop it and loop back for another */
                InstrCountFiltered2(node, 1);
                CHECK_FOR_INTERRUPTS();
                goto next_indextuple;
            }
        }

        if (scandesc->xs_recheckorderby)
        {
            econtext->ecxt_scantuple = slot;
            ResetExprContext(econtext);
            evaluate_index_order_by(node, econtext);

            int cmp = compare_order_by_values(node->iss_OrderByValues,
                                      node->iss_OrderByNulls,
                                      scandesc->xs_orderbyvals,
                                      scandesc->xs_orderbynulls,
                                      node);
            if (cmp < 0)
                bioseq_log<logging_level::error, logging_target::postgres>( "index returned tuples in wrong order");
            else if (cmp == 0)
                was_exact = true;
            else
                was_exact = false;
            lastfetched_vals = node->iss_OrderByValues;
            lastfetched_nulls = node->iss_OrderByNulls;
        }
        else
        {
            was_exact = true;
            lastfetched_vals = scandesc->xs_orderbyvals;
            lastfetched_nulls = scandesc->xs_orderbynulls;
        }

        /*
         * Can we return this tuple immediately, or does it need to be pushed
         * to the reorder queue?
         */
        if (!was_exact || (topmost && compare_order_by_values(lastfetched_vals,
                                                      lastfetched_nulls,
                                                      topmost->orderbyvals,
                                                      topmost->orderbynulls,
                                                      node) > 0))
        {
            push_reorder_queue(node, slot, lastfetched_vals, lastfetched_nulls);
            continue;
        }
        return slot;
    }
}

/* execute_index_scan(node) - executes the index scan, dispatching to the
 * reordering or non-reordering variant based on whether ORDER BY keys are
 * present.
 */
static TupleTableSlot*
execute_index_scan(PlanState* pstate)
{
    index_scan_state* bioseq_index_state = index_scan_state::from_plan_state(pstate);
    IndexScanState* node = castNode(IndexScanState, bioseq_index_state->index_scan_executor_state);

    /* If we have runtime keys and they've not already been set up, do it now. */
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

/* ExecIndexScanInitializeDSM - set up a parallel index scan descriptor. */
static void
initialize_parallel_index_scan(IndexScanState* node,
                                             ParallelContext* pcxt,
                                             void* coordinate)
{
    EState* estate = node->ss.ps.state;
    ParallelIndexScanDesc piscan = static_cast<ParallelIndexScanDesc>(coordinate);

    bioseq_initialize_uninstrumented_parallel_index_scan(
        node->ss.ss_currentRelation,
        node->iss_RelationDesc,
        estate->es_snapshot,
        piscan);

    node->iss_ScanDesc = bioseq_begin_uninstrumented_parallel_index_scan(
        node->ss.ss_currentRelation,
        node->iss_RelationDesc,
        node->iss_NumScanKeys,
        node->iss_NumOrderByKeys,
        piscan);

    if (node->iss_NumRuntimeKeys == 0 || node->iss_RuntimeKeysReady)
        index_rescan(node->iss_ScanDesc,
                     node->iss_ScanKeys, node->iss_NumScanKeys,
                     node->iss_OrderByKeys, node->iss_NumOrderByKeys);
}

/* ExecIndexScanInitializeWorker - copy relevant info from TOC into planstate. */
static void
initialize_parallel_index_scan_worker(IndexScanState* node, void* coordinate)
{
    ParallelIndexScanDesc piscan = static_cast<ParallelIndexScanDesc>(coordinate);

    node->iss_ScanDesc = bioseq_begin_uninstrumented_parallel_index_scan(
        node->ss.ss_currentRelation,
        node->iss_RelationDesc,
        node->iss_NumScanKeys,
        node->iss_NumOrderByKeys,
        piscan);

    if (node->iss_NumRuntimeKeys == 0 || node->iss_RuntimeKeysReady)
        index_rescan(node->iss_ScanDesc,
                     node->iss_ScanKeys, node->iss_NumScanKeys,
                     node->iss_OrderByKeys, node->iss_NumOrderByKeys);
}


/* CustomScan factory + lifecycle hooks for the bioseq IndexScan node. */

static void
begin_bioseq_index_scan(CustomScanState* css, EState* estate, int eflags)
{
    index_scan_state* index_state = index_scan_state::from_custom_scan_state(css);

    /* Clean out the tuple table */
    ExecClearTuple(css->ss.ps.ps_ResultTupleSlot);
    ExecClearTuple(css->ss.ss_ScanTupleSlot);

    index_state->index_scan_executor_state = ExecInitIndexScan(index_state->inner_index_scan_plan(), estate, eflags);

    /* Initialize result type and projection. */
    ExecInitResultTypeTL(&index_state->custom_scan_state.ss.ps);
}


static TupleTableSlot*
execute_bioseq_index_scan(CustomScanState* node)
{
    return execute_index_scan(reinterpret_cast<PlanState*>(node));
}


static void
end_bioseq_index_scan(CustomScanState* node)
{
    ExecEndIndexScan(index_scan_state::from_custom_scan_state(node)->index_scan_executor_state);
}

/* Show a generic expression. */
static void
show_expression(Node* node, const char* qlabel,
                PlanState* planstate, List* ancestors,
                bool useprefix, ExplainState* es)
{
    List* context = set_deparse_context_plan(es->deparse_cxt, planstate->plan, ancestors);
    char* exprstr = deparse_expression(node, context, useprefix, false);
    ExplainPropertyText(qlabel, exprstr, es);
}

/* Show a qualifier expression (which is a List with implicit AND semantics). */
static void
show_qual(List* qual, const char* qlabel,
          PlanState* planstate, List* ancestors,
          bool useprefix, ExplainState* es)
{
    if (qual == NIL)
        return;

    Node* and_node = reinterpret_cast<Node*>(make_ands_explicit(qual));
    show_expression(and_node, qlabel, planstate, ancestors, useprefix, es);
}

/* Show a qualifier expression for a scan plan node. */
static void
show_scan_qual(List* qual, const char* qlabel,
               PlanState* planstate, List* ancestors,
               ExplainState* es)
{
    bool useprefix = (IsA(planstate->plan, SubqueryScan) || es->verbose);
    show_qual(qual, qlabel, planstate, ancestors, useprefix, es);
}


static void
show_instrumentation_count(const char* qlabel, int which,
                           PlanState* planstate, ExplainState* es)
{
    if (!es->analyze || !planstate->instrument)
        return;

    double nfiltered = (which == 2)
        ? planstate->instrument->nfiltered2
        : planstate->instrument->nfiltered1;
    double nloops = planstate->instrument->nloops;

    /* In text mode, suppress zero counts; they're not interesting enough */
    if (nfiltered > 0 || es->format != EXPLAIN_FORMAT_TEXT)
    {
        if (nloops > 0)
            ExplainPropertyFloat(qlabel, nullptr, nfiltered / nloops, 0, es);
        else
            ExplainPropertyFloat(qlabel, nullptr, 0.0, 0, es);
    }
}

static void
explain_bioseq_index_scan(CustomScanState* node, List* ancestors, ExplainState* es)
{
    index_scan_state* index_state = index_scan_state::from_custom_scan_state(node);
    IndexScan* inner_index_scan = index_state->inner_index_scan_plan();

    const char* indexname = get_rel_name(inner_index_scan->indexid);

    ExplainPropertyText("BioseqIndexScan using ", indexname, es);

    show_scan_qual(inner_index_scan->indexqualorig,
                    "Index Cond", &index_state->custom_scan_state.ss.ps, ancestors, es);

    if (inner_index_scan->indexqualorig)
        show_instrumentation_count("Rows Removed by Index Recheck", 2,
                                    &index_state->custom_scan_state.ss.ps, es);

    show_scan_qual(inner_index_scan->indexorderbyorig,
                    "Order By", &index_state->custom_scan_state.ss.ps, ancestors, es);

    show_scan_qual(node->ss.ps.plan->qual, "Filter",
                   &index_state->custom_scan_state.ss.ps, ancestors, es);

    if (node->ss.ps.plan->qual)
        show_instrumentation_count("Rows Removed by Filter", 1,
                                    &index_state->custom_scan_state.ss.ps, es);
}

/* Parallel Execution */

static Size
estimate_parallel_bioseq_index_scan(CustomScanState* node, ParallelContext* pcxt)
{
    index_scan_state* index_state = index_scan_state::from_custom_scan_state(node);
    ExecIndexScanEstimate(index_state->index_scan_executor_state, pcxt);
    return index_state->index_scan_executor_state->iss_PscanLen;
}


static void
initialize_parallel_bioseq_index_scan(CustomScanState* node,
                                        ParallelContext* pcxt,
                                        void* coordinate)
{
    index_scan_state* index_state = index_scan_state::from_custom_scan_state(node);
    initialize_parallel_index_scan(index_state->index_scan_executor_state, pcxt, coordinate);
}


static void
reinitialize_parallel_bioseq_index_scan(CustomScanState* node,
                                          ParallelContext* pcxt,
                                          void* coordinate)
{
    index_scan_state* index_state = index_scan_state::from_custom_scan_state(node);
    ExecIndexScanReInitializeDSM(index_state->index_scan_executor_state, pcxt);
}


static void
initialize_parallel_bioseq_index_scan_worker(CustomScanState* node,
                                           shm_toc* toc,
                                           void* coordinate)
{
    index_scan_state* index_state = index_scan_state::from_custom_scan_state(node);
    initialize_parallel_index_scan_worker(index_state->index_scan_executor_state, coordinate);
}


[[nodiscard]] static const CustomExecMethods&
index_scan_execution_methods() noexcept
{
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


static Node*
create_index_scan_custom_state(CustomScan* custom_plan)
{
    index_scan_state* index_state = index_scan_state::allocate();
    index_state->as_custom_scan_state()->methods = &index_scan_execution_methods();
    return index_state->as_node();
}


[[nodiscard]] static const CustomScanMethods&
index_scan_plan_methods() noexcept
{
    static const CustomScanMethods methods = {
        .CustomName = "BioseqIndexScan",
        .CreateCustomScanState = create_index_scan_custom_state,
    };
    return methods;
}


export CustomScan*
bioseq_create_index_scan_node(void)
{
    CustomScan* cscan = reinterpret_cast<CustomScan*>(makeNode(CustomScan));
    cscan->methods = &index_scan_plan_methods();
    return cscan;
}


export void
bioseq_register_index_scan_node(void)
{
    RegisterCustomScanMethods(&index_scan_plan_methods());
}
