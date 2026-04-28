/*-------------------------------------------------------------------------
 *
 * table/columnar_indexscan.cppm
 *
 * Bioseq IndexScan custom node. The planner_hook rewrites a vanilla
 * IndexScan plan into a CustomScan whose state is a BioseqIndexScanState
 * (CustomScanState first, IndexScanState* second). The execution methods
 * delegate to PostgreSQL's IndexScan executor while reusing bioseq's
 * tuple-fetch path so projected columns can be pushed into the columnar
 * reader.
 *
 *-------------------------------------------------------------------------
 */
module;
#include "common.h"

#include "columnar/columnar_tableam.h"

export module bioseq_table:index_scan;

import pg_utils;
import bioseq_table_types;
import bioseq_scan_state;
import pg_utils.lists;
import pg_utils.bitmapset;

/*
 * When an ordering operator is used, tuples fetched from the index that
 * need to be reordered are queued in a pairing heap as ReorderTuples.
 */
struct ReorderTuple
{
    pairingheap_node ph_node;
    HeapTuple htup;
    Datum* orderbyvals;
    bool* orderbynulls;
};

bool IsColumnarTableAmTable(Oid relationId);
Bitmapset* ColumnarAttrNeeded(ScanState* ss, List* customList);
IndexFetchTableData* bioseq_index_fetch_begin_extended(Relation rel, Bitmapset* attr_needed);


static TupleTableSlot* IndexNext(IndexScanState* node);
static TupleTableSlot* IndexNextWithReorder(IndexScanState* node);
static void EvalOrderByExpressions(IndexScanState* node, ExprContext* econtext);
static bool IndexRecheck(IndexScanState* node, TupleTableSlot* slot);
static int cmp_orderbyvals(const Datum* adist, const bool* anulls,
                           const Datum* bdist, const bool* bnulls,
                           IndexScanState* node);
static void reorderqueue_push(IndexScanState* node, TupleTableSlot* slot,
                              Datum* orderbyvals, bool* orderbynulls);
static HeapTuple reorderqueue_pop(IndexScanState* node);


/*
 * RELATION_CHECKS / CHECK_REL_PROCEDURE mirror PostgreSQL's internal
 * macros (originally token-pasting macros). Kept as macros here because
 * AssertMacro / elog / CppAsString are themselves macros and a function
 * could not preserve the same compile-time semantics.
 */
#define RELATION_CHECKS \
    ( \
        AssertMacro(RelationIsValid(indexRelation)), \
        AssertMacro(PointerIsValid(indexRelation->rd_indam)), \
        AssertMacro(!ReindexIsProcessingIndex(RelationGetRelid(indexRelation))) \
    )

#define CHECK_REL_PROCEDURE(pname) \
    do { \
        if (indexRelation->rd_indam->pname == NULL) \
            pg_log(ERROR, "function \"{}\" is not defined for index \"{}\"", \
                 CppAsString(pname), RelationGetRelationName(indexRelation)); \
    } while(0)


static IndexScanDesc
index_beginscan_internal(Relation indexRelation,
                         int nkeys, int norderbys, Snapshot snapshot,
                         ParallelIndexScanDesc pscan, bool temp_snap)
{
    RELATION_CHECKS;
    CHECK_REL_PROCEDURE(ambeginscan);

    if (!(indexRelation->rd_indam->ampredlocks))
        PredicateLockRelation(indexRelation, snapshot);

    /* We hold a reference count to the relcache entry throughout the scan. */
    RelationIncrementReferenceCount(indexRelation);

    /* Tell the AM to open a scan. */
    IndexScanDesc scan = indexRelation->rd_indam->ambeginscan(indexRelation, nkeys, norderbys);
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
bioseq_index_beginscan(Relation heapRelation,
                       Relation indexRelation,
                       Snapshot snapshot,
                       int nkeys, int norderbys,
                       Bitmapset* attr_needed)
{
    IndexScanDesc scan = index_beginscan_internal(indexRelation, nkeys, norderbys, snapshot, nullptr, false);

    /*
     * Save additional parameters into the scandesc.  Everything else was set
     * up by RelationGetIndexScan.
     */
    scan->heapRelation = heapRelation;
    scan->xs_snapshot = snapshot;

    /* prepare to fetch index matches from table */
    scan->xs_heapfetch = bioseq_index_fetch_begin_extended(heapRelation, attr_needed);

    return scan;
}

/*
 * IndexNext retrieves the next tuple from the IndexScanState's relation
 * using the bioseq-specific xs_heapfetch path so projected columns are
 * read through the columnar reader.
 */
static TupleTableSlot*
IndexNext(IndexScanState* node)
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
        PBitmapset attr_needed =
            ColumnarAttrNeeded(&node->ss, (reinterpret_cast<IndexScan*>(node->ss.ps.plan))->indexqualorig);

        scandesc = bioseq_index_beginscan(node->ss.ss_currentRelation,
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
 * IndexNextWithReorder is like IndexNext but additionally re-checks
 * ORDER BY expressions and reorders tuples through a pairing heap.
 */
static TupleTableSlot*
IndexNextWithReorder(IndexScanState* node)
{
    ReorderTuple* topmost = nullptr;
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
        PBitmapset attr_needed =
            ColumnarAttrNeeded(&node->ss, (reinterpret_cast<IndexScan*>(node->ss.ps.plan))->indexqualorig);

        scandesc = bioseq_index_beginscan(node->ss.ss_currentRelation,
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
            topmost = reinterpret_cast<ReorderTuple*>(pairingheap_first(node->iss_ReorderQueue));

            if (node->iss_ReachedEnd ||
                cmp_orderbyvals(topmost->orderbyvals,
                                topmost->orderbynulls,
                                scandesc->xs_orderbyvals,
                                scandesc->xs_orderbynulls,
                                node) <= 0)
            {
                HeapTuple tuple = reorderqueue_pop(node);

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
            EvalOrderByExpressions(node, econtext);

            int cmp = cmp_orderbyvals(node->iss_OrderByValues,
                                      node->iss_OrderByNulls,
                                      scandesc->xs_orderbyvals,
                                      scandesc->xs_orderbynulls,
                                      node);
            if (cmp < 0)
                pg_log(ERROR, "index returned tuples in wrong order");
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
        if (!was_exact || (topmost && cmp_orderbyvals(lastfetched_vals,
                                                      lastfetched_nulls,
                                                      topmost->orderbyvals,
                                                      topmost->orderbynulls,
                                                      node) > 0))
        {
            reorderqueue_push(node, slot, lastfetched_vals, lastfetched_nulls);
            continue;
        }
        return slot;
    }
}

/*
 * Calculate the expressions in the ORDER BY clause, based on the heap tuple.
 */
static void
EvalOrderByExpressions(IndexScanState* node, ExprContext* econtext)
{
    MemoryContextScope ctx_guard{econtext->ecxt_per_tuple_memory};

    int orderby_index = 0;
    for (auto orderby_state : PList<ExprState>(node->indexorderbyorig))
    {
        node->iss_OrderByValues[orderby_index] = ExecEvalExpr(orderby_state,
                                                              econtext,
                                                              &node->iss_OrderByNulls[orderby_index]);
        orderby_index++;
    }
}

/* IndexRecheck -- access method routine to recheck a tuple in EvalPlanQual */
static bool
IndexRecheck(IndexScanState* node, TupleTableSlot* slot)
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
cmp_orderbyvals(const Datum* adist, const bool* anulls,
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
reorderqueue_push(IndexScanState* node, TupleTableSlot* slot,
                  Datum* orderbyvals, bool* orderbynulls)
{
    IndexScanDesc scandesc = node->iss_ScanDesc;
    EState* estate = node->ss.ps.state;
    MemoryContextScope ctx_guard{estate->es_query_cxt};

    ReorderTuple* rt = make0<ReorderTuple>();
    rt->htup = ExecCopySlotHeapTuple(slot);
    rt->orderbyvals = make_array<Datum>(scandesc->numberOfOrderBys);
    rt->orderbynulls = make_array<bool>(scandesc->numberOfOrderBys);
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
reorderqueue_pop(IndexScanState* node)
{
    ReorderTuple* topmost = reinterpret_cast<ReorderTuple*>(pairingheap_remove_first(node->iss_ReorderQueue));

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


/* ExecIndexScan(node) - executes the index scan, dispatching to the
 * reordering or non-reordering variant based on whether ORDER BY keys are
 * present.
 */
static TupleTableSlot*
ExecIndexScan(PlanState* pstate)
{
    BioseqIndexScanState* bioseq_index_state = BioseqIndexScanState::from_plan_state(pstate);
    IndexScanState* node = castNode(IndexScanState, bioseq_index_state->indexscan_state);

    /* If we have runtime keys and they've not already been set up, do it now. */
    if (node->iss_NumRuntimeKeys != 0 && !node->iss_RuntimeKeysReady)
        ExecReScan(reinterpret_cast<PlanState*>(node));

    if (node->iss_NumOrderByKeys > 0)
        return ExecScan(&node->ss,
                        reinterpret_cast<ExecScanAccessMtd>(IndexNextWithReorder),
                        reinterpret_cast<ExecScanRecheckMtd>(IndexRecheck));
    return ExecScan(&node->ss,
                    reinterpret_cast<ExecScanAccessMtd>(IndexNext),
                    reinterpret_cast<ExecScanRecheckMtd>(IndexRecheck));
}

/* ExecIndexScanInitializeDSM - set up a parallel index scan descriptor. */
static void
ColumnarIndexScan_ExecIndexScanInitializeDSM(IndexScanState* node,
                                             ParallelContext* pcxt,
                                             void* coordinate)
{
    EState* estate = node->ss.ps.state;
    ParallelIndexScanDesc piscan = static_cast<ParallelIndexScanDesc>(coordinate);

    index_parallelscan_initialize_compat(node->ss.ss_currentRelation,
                                         node->iss_RelationDesc,
                                         estate->es_snapshot,
                                         piscan);

    node->iss_ScanDesc = index_beginscan_parallel_compat(node->ss.ss_currentRelation,
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
ColumnarIndexScan_ExecIndexScanInitializeWorker(IndexScanState* node, void* coordinate)
{
    ParallelIndexScanDesc piscan = static_cast<ParallelIndexScanDesc>(coordinate);

    node->iss_ScanDesc = index_beginscan_parallel_compat(node->ss.ss_currentRelation,
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
static Node* CreateBioseqIndexScanState(CustomScan* custom_plan);

static void BioseqIndexScan_BeginCustomScan(CustomScanState* node,
                                            EState* estate, int eflags);
static TupleTableSlot* BioseqIndexScan_ExecCustomScan(CustomScanState* node);
static void BioseqIndexScan_EndCustomScan(CustomScanState* node);
static void BioseqIndexScan_ExplainCustomScan(CustomScanState* node,
                                              List* ancestors,
                                              ExplainState* es);
static Size BioseqIndexScan_EstimateDSMCustomScan(CustomScanState* node,
                                                  ParallelContext* pcxt);
static void BioseqIndexScan_InitializeDSMCustomScan(CustomScanState* node,
                                                    ParallelContext* pcxt,
                                                    void* coordinate);
static void BioseqIndexScan_ReinitializeDSMCustomScan(CustomScanState* node,
                                                      ParallelContext* pcxt,
                                                      void* coordinate);
static void BioseqIndexScan_InitializeWorkerCustomScan(CustomScanState* node,
                                                       shm_toc* toc,
                                                       void* coordinate);

static CustomScanMethods BioseqIndexCustomScanMethods = {
    .CustomName = "BioseqIndexScan",
    .CreateCustomScanState = CreateBioseqIndexScanState,
};

static CustomExecMethods BioseqIndexScanExecMethods = {
    .CustomName = "BioseqIndexScan",

    .BeginCustomScan = BioseqIndexScan_BeginCustomScan,
    .ExecCustomScan = BioseqIndexScan_ExecCustomScan,
    .EndCustomScan = BioseqIndexScan_EndCustomScan,

    .ExplainCustomScan = BioseqIndexScan_ExplainCustomScan,

    .EstimateDSMCustomScan = BioseqIndexScan_EstimateDSMCustomScan,
    .InitializeDSMCustomScan = BioseqIndexScan_InitializeDSMCustomScan,
    .ReInitializeDSMCustomScan = BioseqIndexScan_ReinitializeDSMCustomScan,
    .InitializeWorkerCustomScan = BioseqIndexScan_InitializeWorkerCustomScan
};


static Node*
CreateBioseqIndexScanState(CustomScan* custom_plan)
{
    BioseqIndexScanState* index_state = BioseqIndexScanState::allocate();
    index_state->as_custom_scan_state()->methods = &BioseqIndexScanExecMethods;
    return index_state->as_node();
}


static void
BioseqIndexScan_BeginCustomScan(CustomScanState* css, EState* estate, int eflags)
{
    BioseqIndexScanState* index_state = BioseqIndexScanState::from_custom_scan_state(css);

    /* Clean out the tuple table */
    ExecClearTuple(css->ss.ps.ps_ResultTupleSlot);
    ExecClearTuple(css->ss.ss_ScanTupleSlot);

    index_state->indexscan_state = ExecInitIndexScan(index_state->inner_index_scan_plan(), estate, eflags);

    /* Initialize result type and projection. */
    ExecInitResultTypeTL(&index_state->css.ss.ps);
}


static TupleTableSlot*
BioseqIndexScan_ExecCustomScan(CustomScanState* node)
{
    return ExecIndexScan(reinterpret_cast<PlanState*>(node));
}


static void
BioseqIndexScan_EndCustomScan(CustomScanState* node)
{
    ExecEndIndexScan(BioseqIndexScanState::from_custom_scan_state(node)->indexscan_state);
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
BioseqIndexScan_ExplainCustomScan(CustomScanState* node, List* ancestors, ExplainState* es)
{
    BioseqIndexScanState* index_state = BioseqIndexScanState::from_custom_scan_state(node);
    IndexScan* inner_index_scan = index_state->inner_index_scan_plan();

    const char* indexname = get_rel_name(inner_index_scan->indexid);

    ExplainPropertyText("BioseqIndexScan using ", indexname, es);

    show_scan_qual(inner_index_scan->indexqualorig,
                    "Index Cond", &index_state->css.ss.ps, ancestors, es);

    if (inner_index_scan->indexqualorig)
        show_instrumentation_count("Rows Removed by Index Recheck", 2,
                                    &index_state->css.ss.ps, es);

    show_scan_qual(inner_index_scan->indexorderbyorig,
                    "Order By", &index_state->css.ss.ps, ancestors, es);

    show_scan_qual(node->ss.ps.plan->qual, "Filter", &index_state->css.ss.ps, ancestors, es);

    if (node->ss.ps.plan->qual)
        show_instrumentation_count("Rows Removed by Filter", 1,
                                    &index_state->css.ss.ps, es);
}

/* Parallel Execution */

static Size
BioseqIndexScan_EstimateDSMCustomScan(CustomScanState* node, ParallelContext* pcxt)
{
    BioseqIndexScanState* index_state = BioseqIndexScanState::from_custom_scan_state(node);
    ExecIndexScanEstimate(index_state->indexscan_state, pcxt);
    return index_state->indexscan_state->iss_PscanLen;
}


static void
BioseqIndexScan_InitializeDSMCustomScan(CustomScanState* node,
                                        ParallelContext* pcxt,
                                        void* coordinate)
{
    BioseqIndexScanState* index_state = BioseqIndexScanState::from_custom_scan_state(node);
    ColumnarIndexScan_ExecIndexScanInitializeDSM(index_state->indexscan_state, pcxt, coordinate);
}


static void
BioseqIndexScan_ReinitializeDSMCustomScan(CustomScanState* node,
                                          ParallelContext* pcxt,
                                          void* coordinate)
{
    BioseqIndexScanState* index_state = BioseqIndexScanState::from_custom_scan_state(node);
    ExecIndexScanReInitializeDSM(index_state->indexscan_state, pcxt);
}


static void
BioseqIndexScan_InitializeWorkerCustomScan(CustomScanState* node,
                                           shm_toc* toc,
                                           void* coordinate)
{
    BioseqIndexScanState* index_state = BioseqIndexScanState::from_custom_scan_state(node);
    ColumnarIndexScan_ExecIndexScanInitializeWorker(index_state->indexscan_state, coordinate);
}


export CustomScan*
bioseq_create_indexscan_node(void)
{
    CustomScan* cscan = reinterpret_cast<CustomScan*>(makeNode(CustomScan));
    cscan->methods = &BioseqIndexCustomScanMethods;
    return cscan;
}


export void
bioseq_register_indexscan_node(void)
{
    RegisterCustomScanMethods(&BioseqIndexCustomScanMethods);
}
