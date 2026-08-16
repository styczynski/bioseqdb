/**
 * @file
 * @brief Implements aggregate transitions, ordering, and projection.
 */
module;
#include <bioseq/pg_compat.h>
extern "C" {
#include "nodes/extensible.h"
}
#include <algorithm>

export module bioseq.postgres.vectorization.aggregator_node:transitions;

import bioseq.common.logging;
import bioseq.postgres.support;
import bioseq.postgres.vectorization.types;

using bioseq::postgres::support::allocate_zeroed_array;
using bioseq::postgres::support::allocate_zeroed_object;
using bioseq::postgres::support::allocate_zeroed_storage;
using bioseq::postgres::support::memory_context_scope;
using bioseq::postgres::support::pg_expression_walk_cursor;
using bioseq::postgres::support::pg_node_view;
using bioseq::postgres::support::pg_walk_step;
using bioseq::postgres::support::postgres_list;
using bioseq::postgres::support::walk_pg_expression_including_root;

import :support;

/**
 * Select the current grouping set. Affects current_set and
 * curaggcontext.
 */
export void select_current_set(AggState* aggregate_state, int set_index, bool is_hash) {
    /**
     * When changing this, also adapt ExecAggPlainTransByVal() and
     * ExecAggPlainTransByRef().
     */
    if (is_hash)
        aggregate_state->curaggcontext = aggregate_state->hashcontext;
    else
        aggregate_state->curaggcontext = aggregate_state->aggcontexts[set_index];

    aggregate_state->current_set = set_index;
}

/**
 * Switches aggregation phases and rotates or releases their tuplesorts.
 */
export void initialize_phase(AggState* aggregate_state, int new_phase) {
    Assert(new_phase <= 1 || new_phase == aggregate_state->current_phase + 1);

    /**
     * Whatever the previous state, we're now done with whatever input
     * tuplesort was in use.
     */
    if (aggregate_state->sort_in) {
        tuplesort_end(aggregate_state->sort_in);
        aggregate_state->sort_in = nullptr;
    }

    if (new_phase <= 1) {
        /**
         * Discard any existing output tuplesort.
         */
        if (aggregate_state->sort_out) {
            tuplesort_end(aggregate_state->sort_out);
            aggregate_state->sort_out = nullptr;
        }
    } else {
        /**
         * The old output tuplesort becomes the new input one, and this is the
         * right time to actually sort it.
         */
        aggregate_state->sort_in = aggregate_state->sort_out;
        aggregate_state->sort_out = nullptr;
        Assert(aggregate_state->sort_in);
        tuplesort_performsort(aggregate_state->sort_in);
    }

    /**
     * If this isn't the last phase, we need to sort appropriately for the
     * next phase in sequence.
     */
    if (new_phase > 0 && new_phase < aggregate_state->numphases - 1) {
        Sort* sort_node = aggregate_state->phases[new_phase + 1].sortnode;
        PlanState* outer_node = outerPlanState(aggregate_state);
        TupleDesc tuple_descriptor = ExecGetResultType(outer_node);

        aggregate_state->sort_out = tuplesort_begin_heap(tuple_descriptor,
                                                         sort_node->numCols,
                                                         sort_node->sortColIdx,
                                                         sort_node->sortOperators,
                                                         sort_node->collations,
                                                         sort_node->nullsFirst,
                                                         work_mem,
                                                         nullptr,
                                                         TUPLESORT_NONE);
    }

    aggregate_state->current_phase = new_phase;
    aggregate_state->phase = &aggregate_state->phases[new_phase];
}

/**
 * Fetches one input tuple from the outer plan or the preceding phase sorter.
 *
 * The returned slot is valid only until the next fetch.
 */
export TupleTableSlot* fetch_input_tuple(AggState* aggregate_state) {
    TupleTableSlot* slot;

    if (aggregate_state->sort_in) {
        /* make sure we check for interrupts in either path through here */
        CHECK_FOR_INTERRUPTS();
        if (!tuplesort_gettupleslot(
                aggregate_state->sort_in, true, false, aggregate_state->sort_slot, nullptr))
            return nullptr;
        slot = aggregate_state->sort_slot;
    } else
        slot = ExecProcNode(outerPlanState(aggregate_state));

    if (!TupIsNull(slot) && aggregate_state->sort_out)
        tuplesort_puttupleslot(aggregate_state->sort_out, slot);

    return slot;
}

/**
 * Initializes one aggregate transition state for the selected grouping set.
 */
export void initialize_aggregate(AggState* aggregate_state,
                                 AggStatePerTrans transition_state,
                                 AggStatePerGroup per_group_state) {
    /**
     * Start a fresh sort operation for each DISTINCT/ORDER BY aggregate.
     */
    if (transition_state->numSortCols > 0) {
        /**
         * In case of rescan, maybe there could be an uncompleted sort
         * operation?  Clean it up if so.
         */
        if (transition_state->sortstates[aggregate_state->current_set])
            tuplesort_end(transition_state->sortstates[aggregate_state->current_set]);

        /**
         * Uses the Datum sorter for one input and the tuple sorter otherwise.
         */
        if (transition_state->numInputs == 1) {
            Form_pg_attribute attr = TupleDescAttr(transition_state->sortdesc, 0);

            transition_state->sortstates[aggregate_state->current_set] =
                tuplesort_begin_datum(attr->atttypid,
                                      transition_state->sortOperators[0],
                                      transition_state->sortCollations[0],
                                      transition_state->sortNullsFirst[0],
                                      work_mem,
                                      nullptr,
                                      TUPLESORT_NONE);
        } else
            transition_state->sortstates[aggregate_state->current_set] =
                tuplesort_begin_heap(transition_state->sortdesc,
                                     transition_state->numSortCols,
                                     transition_state->sortColIdx,
                                     transition_state->sortOperators,
                                     transition_state->sortCollations,
                                     transition_state->sortNullsFirst,
                                     work_mem,
                                     nullptr,
                                     TUPLESORT_NONE);
    }

    /**
     * Restores the initial transition value, copying referenced state into the
     * aggregate context.
     */
    if (transition_state->initValueIsNull)
        per_group_state->transValue = transition_state->initValue;
    else {
        memory_context_scope ctx_guard{aggregate_state->curaggcontext->ecxt_per_tuple_memory};
        per_group_state->transValue = datumCopy(transition_state->initValue,
                                                transition_state->transtypeByVal,
                                                transition_state->transtypeLen);
    }
    per_group_state->transValueIsNull = transition_state->initValueIsNull;

    /**
     * Lets the first non-null input initialize a transition with no catalog
     * initial value.
     */
    per_group_state->noTransValue = transition_state->initValueIsNull;
}

/**
 * Initializes the requested leading grouping sets, or every set when the
 * reset count is zero.
 */
export void initialize_aggregates(AggState* aggregate_state,
                                  AggStatePerGroup* group_state_sets,
                                  int reset_count) {
    int transition_index;
    int grouping_set_count = std::max(aggregate_state->phase->numsets, 1);
    int set_index = 0;
    int transition_count = aggregate_state->numtrans;
    AggStatePerTrans transition_states = aggregate_state->pertrans;

    if (reset_count == 0)
        reset_count = grouping_set_count;

    for (set_index = 0; set_index < reset_count; set_index++) {
        AggStatePerGroup group_state = group_state_sets[set_index];

        select_current_set(aggregate_state, set_index, false);

        for (transition_index = 0; transition_index < transition_count; transition_index++) {
            AggStatePerTrans transition_state = &transition_states[transition_index];
            AggStatePerGroup per_group_state = &group_state[transition_index];

            initialize_aggregate(aggregate_state, transition_state, per_group_state);
        }
    }
}

/**
 * Advances one preloaded transition call for the selected grouping set.
 *
 * Strictness and pass-by-reference state ownership follow PostgreSQL fmgr rules.
 */
static void advance_transition_function(AggState* aggregate_state,
                                        AggStatePerTrans transition_state,
                                        AggStatePerGroup per_group_state) {
    FunctionCallInfo call_info = transition_state->transfn_fcinfo;
    Datum new_value;

    if (transition_state->transfn.fn_strict) {
        /**
         * For a strict transfn, nothing happens when there's a nullptr input. We
         * just keep the prior transValue.
         */
        int num_transition_inputs = transition_state->numTransInputs;
        int i;

        for (i = 1; i <= num_transition_inputs; i++) {
            if (call_info->args[i].isnull)
                return;
        }
        if (per_group_state->noTransValue) {
            /**
             * Copies the first non-null input into the aggregate context as the
             * initial transition state.
             */
            {
                memory_context_scope ctx_guard{
                    aggregate_state->curaggcontext->ecxt_per_tuple_memory};
                per_group_state->transValue = datumCopy(call_info->args[1].value,
                                                        transition_state->transtypeByVal,
                                                        transition_state->transtypeLen);
                per_group_state->transValueIsNull = false;
                per_group_state->noTransValue = false;
            }
            return;
        }
        if (per_group_state->transValueIsNull) {
            /**
             * Propagates a null state previously returned by a strict transition.
             */
            return;
        }
    }

    /* We run the transition functions in per-input-tuple memory context */
    memory_context_scope ctx_guard{aggregate_state->tmpcontext->ecxt_per_tuple_memory};

    /* set up aggregate_state->curpertrans for AggGetAggref() */
    aggregate_state->curpertrans = transition_state;

    /**
     * OK to call the transition function
     */
    call_info->args[0].value = per_group_state->transValue;
    call_info->args[0].isnull = per_group_state->transValueIsNull;
    call_info->isnull = false; /* just in case transfn doesn't set it */

    new_value = FunctionCallInvoke(call_info);

    aggregate_state->curpertrans = nullptr;

    /**
     * Reparents changed referenced state while preserving in-place returns and
     * expanded objects already owned by the aggregate context.
     */
    if (!transition_state->transtypeByVal &&
        DatumGetPointer(new_value) != DatumGetPointer(per_group_state->transValue))

        new_value = ExecAggCopyTransValue(aggregate_state,
                                          transition_state,
                                          new_value,
                                          call_info->isnull,
                                          per_group_state->transValue,
                                          per_group_state->transValueIsNull);

    per_group_state->transValue = new_value;
    per_group_state->transValueIsNull = call_info->isnull;
}

/**
 * Evaluates the compiled transition expression for one input tuple.
 */
export void advance_aggregates(AggState* aggregate_state) {
    bool dummy_null;

    ExecEvalExprSwitchContext(
        aggregate_state->phase->evaltrans, aggregate_state->tmpcontext, &dummy_null);
}

/**
 * Sorts one-input DISTINCT or ORDER BY values and advances each accepted value.
 *
 * The dedicated Datum path preserves the common single-column fast path.
 */
static void process_ordered_aggregate_single(AggState* aggregate_state,
                                             AggStatePerTrans transition_state,
                                             AggStatePerGroup per_group_state) {
    Datum old_value = Datum{0};
    bool old_is_null = true;
    bool has_old_value = false;
    MemoryContext work_context = aggregate_state->tmpcontext->ecxt_per_tuple_memory;
    bool is_distinct = (transition_state->numDistinctCols > 0);
    Datum new_abbreviated_value = Datum{0};
    Datum old_abbreviated_value = Datum{0};
    FunctionCallInfo call_info = transition_state->transfn_fcinfo;
    Datum* new_value;
    bool* is_null;

    Assert(transition_state->numDistinctCols < 2);

    tuplesort_performsort(transition_state->sortstates[aggregate_state->current_set]);

    /* Load the column into argument 1 (arg 0 will be transition value) */
    new_value = &call_info->args[1].value;
    is_null = &call_info->args[1].isnull;

    /**
     * Releases referenced values allocated by the Datum sorter after use.
     */

    while (tuplesort_getdatum(transition_state->sortstates[aggregate_state->current_set],
                              true,
                              false,
                              new_value,
                              is_null,
                              &new_abbreviated_value)) {
        /**
         * Clear and select the working context for evaluation of the equality
         * function and transition function.
         */
        MemoryContextReset(work_context);
        memory_context_scope ctx_guard{work_context};

        /**
         * If DISTINCT mode, and not distinct from prior, skip it.
         */
        if (is_distinct && has_old_value &&
            ((old_is_null && *is_null) ||
             (!old_is_null && !*is_null && old_abbreviated_value == new_abbreviated_value &&
              DatumGetBool(FunctionCall2Coll(&transition_state->equalfnOne,
                                             transition_state->aggCollation,
                                             old_value,
                                             *new_value))))) {
            /* equal to prior, so forget this one */
            if (!transition_state->inputtypeByVal && !*is_null)
                pfree(DatumGetPointer(*new_value));
        } else {
            advance_transition_function(aggregate_state, transition_state, per_group_state);
            /* forget the old value, if any */
            if (!old_is_null && !transition_state->inputtypeByVal)
                pfree(DatumGetPointer(old_value));
            /* and remember the new one for subsequent equality checks */
            old_value = *new_value;
            old_abbreviated_value = new_abbreviated_value;
            old_is_null = *is_null;
            has_old_value = true;
        }
    }

    if (!old_is_null && !transition_state->inputtypeByVal)
        pfree(DatumGetPointer(old_value));

    tuplesort_end(transition_state->sortstates[aggregate_state->current_set]);
    transition_state->sortstates[aggregate_state->current_set] = nullptr;
}

/**
 * Sorts multi-input DISTINCT or ORDER BY tuples and advances accepted rows.
 */
static void process_ordered_aggregate_multi(AggState* aggregate_state,
                                            AggStatePerTrans transition_state,
                                            AggStatePerGroup per_group_state) {
    ExprContext* temporary_context = aggregate_state->tmpcontext;
    FunctionCallInfo call_info = transition_state->transfn_fcinfo;
    TupleTableSlot* slot1 = transition_state->sortslot;
    TupleTableSlot* slot2 = transition_state->uniqslot;
    int num_transition_inputs = transition_state->numTransInputs;
    int distinct_column_count = transition_state->numDistinctCols;
    Datum new_abbreviated_value = Datum{0};
    Datum old_abbreviated_value = Datum{0};
    bool has_old_value = false;
    TupleTableSlot* save = aggregate_state->tmpcontext->ecxt_outertuple;
    int i;

    tuplesort_performsort(transition_state->sortstates[aggregate_state->current_set]);

    ExecClearTuple(slot1);
    if (slot2)
        ExecClearTuple(slot2);

    while (tuplesort_gettupleslot(transition_state->sortstates[aggregate_state->current_set],
                                  true,
                                  true,
                                  slot1,
                                  &new_abbreviated_value)) {
        CHECK_FOR_INTERRUPTS();

        temporary_context->ecxt_outertuple = slot1;
        temporary_context->ecxt_innertuple = slot2;

        if (distinct_column_count == 0 || !has_old_value ||
            new_abbreviated_value != old_abbreviated_value ||
            !ExecQual(transition_state->equalfnMulti, temporary_context)) {
            /**
             * Extract the first num_transition_inputs columns as datums to pass to
             * the transfn.
             */
            slot_getsomeattrs(slot1, num_transition_inputs);

            /* Load values into call_info */
            /* Start from 1, since the 0th arg will be the transition value */
            for (i = 0; i < num_transition_inputs; i++) {
                call_info->args[i + 1].value = slot1->tts_values[i];
                call_info->args[i + 1].isnull = slot1->tts_isnull[i];
            }

            advance_transition_function(aggregate_state, transition_state, per_group_state);

            if (distinct_column_count > 0) {
                /* swap the slot pointers to retain the current tuple */
                TupleTableSlot* temporary_slot = slot2;

                slot2 = slot1;
                slot1 = temporary_slot;
                /* avoid ExecQual() calls by reusing abbreviated keys */
                old_abbreviated_value = new_abbreviated_value;
                has_old_value = true;
            }
        }

        /* Reset context each time */
        ResetExprContext(temporary_context);

        ExecClearTuple(slot1);
    }

    if (slot2)
        ExecClearTuple(slot2);

    tuplesort_end(transition_state->sortstates[aggregate_state->current_set]);
    transition_state->sortstates[aggregate_state->current_set] = nullptr;

    /* restore previous slot, potentially in use for grouping sets */
    temporary_context->ecxt_outertuple = save;
}

/**
 * Finalizes one aggregate without mutating transition state shared by peers.
 */
static void finalize_aggregate(AggState* aggregate_state,
                               AggStatePerAgg aggregate_descriptor,
                               AggStatePerGroup per_group_state,
                               Datum* result_value,
                               bool* result_is_null) {
    LOCAL_FCINFO(call_info, FUNC_MAX_ARGS);
    bool contains_null = false;
    int i;
    AggStatePerTrans transition_state = &aggregate_state->pertrans[aggregate_descriptor->transno];

    memory_context_scope ctx_guard{aggregate_state->ss.ps.ps_ExprContext->ecxt_per_tuple_memory};

    /**
     * Evaluates direct arguments for side effects and places them after the
     * transition state.
     */
    i = 1;
    for (auto expr : postgres_list<ExprState>{aggregate_descriptor->aggdirectargs}) {
        call_info->args[i].value =
            ExecEvalExpr(expr, aggregate_state->ss.ps.ps_ExprContext, &call_info->args[i].isnull);
        contains_null |= call_info->args[i].isnull;
        i++;
    }

    /**
     * Apply the agg's finalfn if one is provided, else return transValue.
     */
    if (OidIsValid(aggregate_descriptor->finalfn_oid)) {
        int final_argument_count = aggregate_descriptor->numFinalArgs;

        /* set up aggregate_state->curperagg for AggGetAggref() */
        aggregate_state->curperagg = aggregate_descriptor;

        InitFunctionCallInfoData(*call_info,
                                 &aggregate_descriptor->finalfn,
                                 final_argument_count,
                                 transition_state->aggCollation,
                                 reinterpret_cast<Node*>(aggregate_state),
                                 nullptr);

        /* Fill in the transition state value */
        call_info->args[0].value = MakeExpandedObjectReadOnly(per_group_state->transValue,
                                                              per_group_state->transValueIsNull,
                                                              transition_state->transtypeLen);
        call_info->args[0].isnull = per_group_state->transValueIsNull;
        contains_null |= per_group_state->transValueIsNull;

        /* Fill any remaining argument positions with nulls */
        for (; i < final_argument_count; i++) {
            call_info->args[i].value = Datum{0};
            call_info->args[i].isnull = true;
            contains_null = true;
        }

        if (call_info->flinfo->fn_strict && contains_null) {
            /* don't call a strict function with nullptr inputs */
            *result_value = Datum{0};
            *result_is_null = true;
        } else {
            *result_value = FunctionCallInvoke(call_info);
            *result_is_null = call_info->isnull;
        }
        aggregate_state->curperagg = nullptr;
    } else {
        *result_value = MakeExpandedObjectReadOnly(per_group_state->transValue,
                                                   per_group_state->transValueIsNull,
                                                   transition_state->transtypeLen);
        *result_is_null = per_group_state->transValueIsNull;
    }
}

/**
 * Compute the output value of one partial aggregate.
 *
 * The serialization function will be run, and the result delivered, in the
 * output-tuple context. Caller's CurrentMemoryContext does not matter.
 */
static void finalize_partial_aggregate(AggState* aggregate_state,
                                       AggStatePerAgg aggregate_descriptor,
                                       AggStatePerGroup per_group_state,
                                       Datum* result_value,
                                       bool* result_is_null) {
    AggStatePerTrans transition_state = &aggregate_state->pertrans[aggregate_descriptor->transno];

    memory_context_scope ctx_guard{aggregate_state->ss.ps.ps_ExprContext->ecxt_per_tuple_memory};

    /**
     * serialfn_oid will be set if we must serialize the transvalue before
     * returning it
     */
    if (OidIsValid(transition_state->serialfn_oid)) {
        /* Don't call a strict serialization function with nullptr input. */
        if (transition_state->serialfn.fn_strict && per_group_state->transValueIsNull) {
            *result_value = Datum{0};
            *result_is_null = true;
        } else {
            FunctionCallInfo call_info = transition_state->serialfn_fcinfo;

            call_info->args[0].value = MakeExpandedObjectReadOnly(per_group_state->transValue,
                                                                  per_group_state->transValueIsNull,
                                                                  transition_state->transtypeLen);
            call_info->args[0].isnull = per_group_state->transValueIsNull;
            call_info->isnull = false;

            *result_value = FunctionCallInvoke(call_info);
            *result_is_null = call_info->isnull;
        }
    } else {
        *result_value = MakeExpandedObjectReadOnly(per_group_state->transValue,
                                                   per_group_state->transValueIsNull,
                                                   transition_state->transtypeLen);
        *result_is_null = per_group_state->transValueIsNull;
    }
}

/**
 * Extract the attributes that make up the grouping key into the
 * hash_slot. This is necessary to compute the hash or perform a lookup.
 */
export inline void prepare_hash_slot(AggStatePerHash hash_state,
                                     TupleTableSlot* input_slot,
                                     TupleTableSlot* hash_slot) {
    int i;

    /* transfer just the needed columns into hash_slot */
    slot_getsomeattrs(input_slot, hash_state->largestGrpColIdx);
    ExecClearTuple(hash_slot);

    for (i = 0; i < hash_state->numhashGrpCols; i++) {
        int variable_index = hash_state->hashGrpColIdxInput[i] - 1;

        hash_slot->tts_values[i] = input_slot->tts_values[variable_index];
        hash_slot->tts_isnull[i] = input_slot->tts_isnull[variable_index];
    }
    ExecStoreVirtualTuple(hash_slot);
}

/**
 * Prepares a representative slot for one grouping set by nulling excluded
 * attributes in place and publishing the current grouping bitmap.
 */
export void
prepare_projection_slot(AggState* aggregate_state, TupleTableSlot* slot, int current_set) {
    if (aggregate_state->phase->grouped_cols) {
        Bitmapset* grouped_cols = aggregate_state->phase->grouped_cols[current_set];

        aggregate_state->grouped_cols = grouped_cols;

        if (TTS_EMPTY(slot)) {
            /**
             * Supplies null values when an empty grouping set has no representative row.
             */
            ExecStoreAllNullTuple(slot);
        } else if (aggregate_state->all_grouped_cols) {
            ListCell* cell;

            /* all_grouped_cols is arranged in desc order */
            slot_getsomeattrs(slot, linitial_int(aggregate_state->all_grouped_cols));

            foreach (cell, aggregate_state->all_grouped_cols) {
                int attnum = lfirst_int(cell);

                if (!bms_is_member(attnum, grouped_cols))
                    slot->tts_isnull[attnum - 1] = true;
            }
        }
    }
}

/**
 * Finalizes every aggregate for the selected group into the output context.
 */
export void finalize_aggregates(AggState* aggregate_state,
                                AggStatePerAgg aggregate_descriptors,
                                AggStatePerGroup group_state) {
    ExprContext* expression_context = aggregate_state->ss.ps.ps_ExprContext;
    Datum* aggregate_values = expression_context->ecxt_aggvalues;
    bool* aggregate_nulls = expression_context->ecxt_aggnulls;
    int aggregate_index;
    int transition_index;

    /**
     * If there were any DISTINCT and/or ORDER BY aggregates, sort their
     * inputs and run the transition functions.
     */
    for (transition_index = 0; transition_index < aggregate_state->numtrans; transition_index++) {
        AggStatePerTrans transition_state = &aggregate_state->pertrans[transition_index];
        AggStatePerGroup per_group_state;

        per_group_state = &group_state[transition_index];

        if (transition_state->numSortCols > 0) {
            Assert(aggregate_state->aggstrategy != AGG_HASHED &&
                   aggregate_state->aggstrategy != AGG_MIXED);

            if (transition_state->numInputs == 1)
                process_ordered_aggregate_single(
                    aggregate_state, transition_state, per_group_state);
            else
                process_ordered_aggregate_multi(aggregate_state, transition_state, per_group_state);
        }
    }

    /**
     * Run the final functions.
     */
    for (aggregate_index = 0; aggregate_index < aggregate_state->numaggs; aggregate_index++) {
        AggStatePerAgg aggregate_descriptor = &aggregate_descriptors[aggregate_index];
        transition_index = aggregate_descriptor->transno;
        AggStatePerGroup per_group_state;

        per_group_state = &group_state[transition_index];

        if (DO_AGGSPLIT_SKIPFINAL(aggregate_state->aggsplit))
            finalize_partial_aggregate(aggregate_state,
                                       aggregate_descriptor,
                                       per_group_state,
                                       &aggregate_values[aggregate_index],
                                       &aggregate_nulls[aggregate_index]);
        else
            finalize_aggregate(aggregate_state,
                               aggregate_descriptor,
                               per_group_state,
                               &aggregate_values[aggregate_index],
                               &aggregate_nulls[aggregate_index]);
    }
}

/**
 * Project the result of a group (whose aggs have already been calculated by
 * finalize_aggregates). Returns the result slot, or nullptr if no row is
 * projected (suppressed by qual).
 */
export TupleTableSlot* project_aggregates(AggState* aggregate_state) {
    ExprContext* expression_context = aggregate_state->ss.ps.ps_ExprContext;

    /**
     * Check the qual (HAVING clause). If the group does not match, ignore it.
     */
    if (ExecQual(aggregate_state->ss.ps.qual, expression_context)) {
        /**
         * Form and return projection tuple using the aggregate results and
         * the representative input tuple.
         */
        return ExecProject(aggregate_state->ss.ps.ps_ProjInfo);
    } else
        InstrCountFiltered1(aggregate_state, 1);

    return nullptr;
}
