/**
 * @file
 * @brief Update aggregate states, process sorted inputs, and return group results.
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
import :support;

using bioseq::postgres::support::memory_context_scope;
using bioseq::postgres::support::postgres_list;

/** Select the grouping set and the memory context that owns its transition values. */
export void select_current_set(AggState* aggregate_state, int set_index, bool is_hash) {
    /* Keep this context selection consistent with PostgreSQL's
     * ExecAggPlainTransByVal() and ExecAggPlainTransByRef(). */
    if (is_hash)
        aggregate_state->curaggcontext = aggregate_state->hashcontext;
    else
        aggregate_state->curaggcontext = aggregate_state->aggcontexts[set_index];

    aggregate_state->current_set = set_index;
}

/** Start an aggregation phase and prepare sorting for its input and the next phase. */
export void initialize_phase(AggState* aggregate_state, int new_phase) {
    Assert(new_phase <= 1 || new_phase == aggregate_state->current_phase + 1);

    /* The previous phase has finished reading its sorted input. */
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
        /* Sort the rows saved by the previous phase and use them as this phase's input. */
        aggregate_state->sort_in = aggregate_state->sort_out;
        aggregate_state->sort_out = nullptr;
        Assert(aggregate_state->sort_in);
        tuplesort_performsort(aggregate_state->sort_in);
    }

    /* Save input in the order required by the following phase. */
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

/** Read the next input from the child plan or a previous phase's sort.
 * The slot is reused by later fetches. Save its contents before keeping a row. */
export TupleTableSlot* fetch_input_tuple(AggState* aggregate_state) {
    TupleTableSlot* input_slot;

    if (aggregate_state->sort_in) {
        /* Check for cancellation while reading from the sorter too. */
        CHECK_FOR_INTERRUPTS();
        if (!tuplesort_gettupleslot(
                aggregate_state->sort_in, true, false, aggregate_state->sort_slot, nullptr))
            return nullptr;
        input_slot = aggregate_state->sort_slot;
    } else
        input_slot = ExecProcNode(outerPlanState(aggregate_state));

    if (!TupIsNull(input_slot) && aggregate_state->sort_out)
        tuplesort_puttupleslot(aggregate_state->sort_out, input_slot);

    return input_slot;
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
        /* A rescan can leave an unfinished sort to close. */
        if (transition_state->sortstates[aggregate_state->current_set])
            tuplesort_end(transition_state->sortstates[aggregate_state->current_set]);

        /* Sort a single input as Datums and several inputs as tuples. */
        if (transition_state->numInputs == 1) {
            Form_pg_attribute sort_attribute = bioseq::postgres::support::tuple_descriptor_attribute(transition_state->sortdesc, 0);

            transition_state->sortstates[aggregate_state->current_set] =
                tuplesort_begin_datum(sort_attribute->atttypid,
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

    /* Copy the initial transition value into the context that owns this group. */
    if (transition_state->initValueIsNull)
        per_group_state->transValue = transition_state->initValue;
    else {
        memory_context_scope aggregate_context_scope{
            aggregate_state->curaggcontext->ecxt_per_tuple_memory};
        per_group_state->transValue = datumCopy(transition_state->initValue,
                                                transition_state->transtypeByVal,
                                                transition_state->transtypeLen);
    }
    per_group_state->transValueIsNull = transition_state->initValueIsNull;

    /* A strict transition with no initial value can start from its first non-NULL input. */
    per_group_state->noTransValue = transition_state->initValueIsNull;
}

/** Initialize the first reset_count grouping sets, or all sets when reset_count is zero. */
export void initialize_aggregates(AggState* aggregate_state,
                                  AggStatePerGroup* group_state_sets,
                                  int reset_count) {
    const int grouping_set_count = std::max(aggregate_state->phase->numsets, 1);
    const int transition_count = aggregate_state->numtrans;
    AggStatePerTrans transition_states = aggregate_state->pertrans;

    if (reset_count == 0)
        reset_count = grouping_set_count;

    for (int set_index = 0; set_index < reset_count; ++set_index) {
        AggStatePerGroup group_state = group_state_sets[set_index];

        select_current_set(aggregate_state, set_index, false);

        for (int transition_index = 0; transition_index < transition_count; ++transition_index) {
            AggStatePerTrans transition_state = &transition_states[transition_index];
            AggStatePerGroup per_group_state = &group_state[transition_index];

            initialize_aggregate(aggregate_state, transition_state, per_group_state);
        }
    }
}

/**
 * Apply one transition call whose input arguments are already loaded.
 * Honor SQL NULL rules and keep any referenced result in the group's memory
 * context so temporary input storage can be reset safely.
 */
static void advance_transition_function(AggState* aggregate_state,
                                        AggStatePerTrans transition_state,
                                        AggStatePerGroup per_group_state) {
    FunctionCallInfo call_info = transition_state->transfn_fcinfo;
    Datum new_value;

    if (transition_state->transfn.fn_strict) {
        /* A strict transition skips rows with a SQL NULL input. Keep the current state. */
        const int num_transition_inputs = transition_state->numTransInputs;

        for (int argument_index = 1; argument_index <= num_transition_inputs; ++argument_index) {
            if (call_info->args[argument_index].isnull)
                return;
        }
        if (per_group_state->noTransValue) {
            /**
             * Copies the first non-null input into the aggregate context as the
             * initial transition state.
             */
            {
                memory_context_scope aggregate_context_scope{
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
            /* Once a strict transition returns NULL, later rows keep that NULL state. */
            return;
        }
    }

    /* Temporary transition work belongs to the input context. */
    memory_context_scope input_context_scope{aggregate_state->tmpcontext->ecxt_per_tuple_memory};

    /* Let aggregate support functions identify the active transition. */
    aggregate_state->curpertrans = transition_state;

    /* Pass the current state before the already loaded row arguments. */
    call_info->args[0].value = per_group_state->transValue;
    call_info->args[0].isnull = per_group_state->transValueIsNull;
    call_info->isnull = false; /* Default to a non-NULL result unless the function says otherwise. */

    new_value = FunctionCallInvoke(call_info);

    aggregate_state->curpertrans = nullptr;

    /* Keep a newly returned reference alive in the group context. A result
     * that reuses the previous state already has the required lifetime. */
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

/** Evaluate this phase's transition expression for the current input slot. */
export void advance_aggregates(AggState* aggregate_state) {
#if PG_VERSION_NUM >= 190000
    // PG19 transition programs store results in group state and end with
    // EEOP_DONE_NO_RETURN. Use the evaluator for expressions with no result.
    ExecEvalExprNoReturnSwitchContext(
        aggregate_state->phase->evaltrans, aggregate_state->tmpcontext);
#else
    bool transition_result_is_null;

    ExecEvalExprSwitchContext(
        aggregate_state->phase->evaltrans, aggregate_state->tmpcontext, &transition_result_is_null);
#endif
}

/** Sort single-column aggregate inputs, skip duplicates for DISTINCT, and update the state. */
static void process_ordered_aggregate_single(AggState* aggregate_state,
                                             AggStatePerTrans transition_state,
                                             AggStatePerGroup per_group_state) {
    Datum old_value = Datum{0};
    bool old_is_null = true;
    bool has_old_value = false;
    MemoryContext work_context = aggregate_state->tmpcontext->ecxt_per_tuple_memory;
    const bool is_distinct = transition_state->numDistinctCols > 0;
    Datum new_abbreviated_value = Datum{0};
    Datum old_abbreviated_value = Datum{0};
    FunctionCallInfo call_info = transition_state->transfn_fcinfo;
    Datum* current_value_out;
    bool* current_is_null_out;

    Assert(transition_state->numDistinctCols < 2);

    tuplesort_performsort(transition_state->sortstates[aggregate_state->current_set]);

    /* Read the column into argument one. Argument zero holds the transition state. */
    current_value_out = &call_info->args[1].value;
    current_is_null_out = &call_info->args[1].isnull;

    /* Keep one previous value for DISTINCT checks and free sorter-owned copies after use. */

    while (tuplesort_getdatum(transition_state->sortstates[aggregate_state->current_set],
                              true,
                              false,
                              current_value_out,
                              current_is_null_out,
                              &new_abbreviated_value)) {
        /* Reset scratch memory before equality checks and the transition call. */
        MemoryContextReset(work_context);
        memory_context_scope evaluation_context_scope{work_context};

        /* DISTINCT accepts only the first occurrence of each sorted value. */
        if (is_distinct && has_old_value &&
            ((old_is_null && *current_is_null_out) ||
             (!old_is_null && !*current_is_null_out &&
              old_abbreviated_value == new_abbreviated_value &&
              DatumGetBool(FunctionCall2Coll(&transition_state->equalfnOne,
                                             transition_state->aggCollation,
                                             old_value,
                                             *current_value_out))))) {
            /* Free this duplicate value if it owns storage. */
            if (!transition_state->inputtypeByVal && !*current_is_null_out)
                pfree(DatumGetPointer(*current_value_out));
        } else {
            advance_transition_function(aggregate_state, transition_state, per_group_state);
            /* forget the old value, if any */
            if (!old_is_null && !transition_state->inputtypeByVal)
                pfree(DatumGetPointer(old_value));
            /* Keep this value for the next duplicate check. */
            old_value = *current_value_out;
            old_abbreviated_value = new_abbreviated_value;
            old_is_null = *current_is_null_out;
            has_old_value = true;
        }
    }

    if (!old_is_null && !transition_state->inputtypeByVal)
        pfree(DatumGetPointer(old_value));

    tuplesort_end(transition_state->sortstates[aggregate_state->current_set]);
    transition_state->sortstates[aggregate_state->current_set] = nullptr;
}

/** Sort multi-column aggregate inputs, remove DISTINCT duplicates, and update the state. */
static void process_ordered_aggregate_multi(AggState* aggregate_state,
                                            AggStatePerTrans transition_state,
                                            AggStatePerGroup per_group_state) {
    ExprContext* temporary_context = aggregate_state->tmpcontext;
    FunctionCallInfo call_info = transition_state->transfn_fcinfo;
    TupleTableSlot* current_slot = transition_state->sortslot;
    TupleTableSlot* previous_slot = transition_state->uniqslot;
    const int num_transition_inputs = transition_state->numTransInputs;
    const int distinct_column_count = transition_state->numDistinctCols;
    Datum new_abbreviated_value = Datum{0};
    Datum old_abbreviated_value = Datum{0};
    bool has_old_value = false;
    TupleTableSlot* saved_outer_slot = aggregate_state->tmpcontext->ecxt_outertuple;

    tuplesort_performsort(transition_state->sortstates[aggregate_state->current_set]);

    ExecClearTuple(current_slot);
    if (previous_slot)
        ExecClearTuple(previous_slot);

    while (tuplesort_gettupleslot(transition_state->sortstates[aggregate_state->current_set],
                                  true,
                                  true,
                                  current_slot,
                                  &new_abbreviated_value)) {
        CHECK_FOR_INTERRUPTS();

        temporary_context->ecxt_outertuple = current_slot;
        temporary_context->ecxt_innertuple = previous_slot;

        if (distinct_column_count == 0 || !has_old_value ||
            new_abbreviated_value != old_abbreviated_value ||
            !ExecQual(transition_state->equalfnMulti, temporary_context)) {
            /* Fetch the columns passed as transition-function inputs. */
            slot_getsomeattrs(current_slot, num_transition_inputs);


            /* Argument zero holds the state. Row inputs start at argument one. */
            for (int input_index = 0; input_index < num_transition_inputs; ++input_index) {
                call_info->args[input_index + 1].value = current_slot->tts_values[input_index];
                call_info->args[input_index + 1].isnull = current_slot->tts_isnull[input_index];
            }

            advance_transition_function(aggregate_state, transition_state, per_group_state);

            if (distinct_column_count > 0) {
                /* Keep the accepted row in the previous slot for the next DISTINCT check. */
                TupleTableSlot* temporary_slot = previous_slot;

                previous_slot = current_slot;
                current_slot = temporary_slot;
                /* Different abbreviated keys let the next comparison skip full equality checks. */
                old_abbreviated_value = new_abbreviated_value;
                has_old_value = true;
            }
        }

        /* Release temporary expression data from this row. */
        ResetExprContext(temporary_context);

        ExecClearTuple(current_slot);
    }

    if (previous_slot)
        ExecClearTuple(previous_slot);

    tuplesort_end(transition_state->sortstates[aggregate_state->current_set]);
    transition_state->sortstates[aggregate_state->current_set] = nullptr;

    /* Restore the input slot used by the surrounding grouping-set execution. */
    temporary_context->ecxt_outertuple = saved_outer_slot;
}

/** Produce one aggregate result, exposing expanded transition values as read-only. */
static void finalize_aggregate(AggState* aggregate_state,
                               AggStatePerAgg aggregate_descriptor,
                               AggStatePerGroup per_group_state,
                               Datum* result_value,
                               bool* result_is_null) {
    LOCAL_FCINFO(call_info, FUNC_MAX_ARGS);
    bool contains_null = false;
    int argument_index = 1;
    AggStatePerTrans transition_state = &aggregate_state->pertrans[aggregate_descriptor->transno];

    memory_context_scope output_context_scope{
        aggregate_state->ss.ps.ps_ExprContext->ecxt_per_tuple_memory};

    /* Evaluate direct arguments once for this group, including their side effects.
     * Their values follow the transition state in the final-function call. */
    for (ExprState* direct_argument :
         postgres_list<ExprState>{aggregate_descriptor->aggdirectargs}) {
        call_info->args[argument_index].value =
            ExecEvalExpr(direct_argument,
                         aggregate_state->ss.ps.ps_ExprContext,
                         &call_info->args[argument_index].isnull);
        contains_null |= call_info->args[argument_index].isnull;
        ++argument_index;
    }

    /* Call the final function when present, otherwise return the transition value. */
    if (OidIsValid(aggregate_descriptor->finalfn_oid)) {
        const int final_argument_count = aggregate_descriptor->numFinalArgs;

        /* Let support functions identify the aggregate being finalized. */
        aggregate_state->curperagg = aggregate_descriptor;

        InitFunctionCallInfoData(*call_info,
                                 &aggregate_descriptor->finalfn,
                                 final_argument_count,
                                 transition_state->aggCollation,
                                 reinterpret_cast<Node*>(aggregate_state),
                                 nullptr);

        /* Pass the transition state with expanded objects marked read-only. */
        call_info->args[0].value = MakeExpandedObjectReadOnly(per_group_state->transValue,
                                                              per_group_state->transValueIsNull,
                                                              transition_state->transtypeLen);
        call_info->args[0].isnull = per_group_state->transValueIsNull;
        contains_null |= per_group_state->transValueIsNull;

        /* Extra final-function arguments carry type information and are passed as SQL NULL. */
        for (; argument_index < final_argument_count; ++argument_index) {
            call_info->args[argument_index].value = Datum{0};
            call_info->args[argument_index].isnull = true;
            contains_null = true;
        }

        if (call_info->flinfo->fn_strict && contains_null) {
            /* A strict final function returns NULL when any argument is SQL NULL. */
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

/** Return a partial aggregate state, serializing it when required.
 * The serializer runs in the output-tuple memory context. */
static void finalize_partial_aggregate(AggState* aggregate_state,
                                       AggStatePerAgg aggregate_descriptor,
                                       AggStatePerGroup per_group_state,
                                       Datum* result_value,
                                       bool* result_is_null) {
    AggStatePerTrans transition_state = &aggregate_state->pertrans[aggregate_descriptor->transno];

    memory_context_scope output_context_scope{
        aggregate_state->ss.ps.ps_ExprContext->ecxt_per_tuple_memory};

    /* Serialize the state when this partial-aggregation mode requires it. */
    if (OidIsValid(transition_state->serialfn_oid)) {
        /* A strict serializer returns NULL for a SQL NULL state. */
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

/** Copy the group keys and required output columns into the compact hash tuple. */
export inline void prepare_hash_slot(AggStatePerHash hash_state,
                                     TupleTableSlot* input_slot,
                                     TupleTableSlot* hash_slot) {
    /* Copy only columns recorded in the hash tuple's input mapping. */
    slot_getsomeattrs(input_slot, hash_state->largestGrpColIdx);
    ExecClearTuple(hash_slot);

    for (int hash_column_index = 0; hash_column_index < hash_state->numhashGrpCols;
         ++hash_column_index) {
        const int variable_index = hash_state->hashGrpColIdxInput[hash_column_index] - 1;

        hash_slot->tts_values[hash_column_index] = input_slot->tts_values[variable_index];
        hash_slot->tts_isnull[hash_column_index] = input_slot->tts_isnull[variable_index];
    }
    ExecStoreVirtualTuple(hash_slot);
}

/** Prepare a group's representative row, setting excluded grouping columns to NULL.
 * Record the active grouping columns for grouping expressions. */
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
            ListCell* grouped_column_cell;

            /* The first list entry is the largest attribute number needed. */
            slot_getsomeattrs(slot, linitial_int(aggregate_state->all_grouped_cols));

            foreach (grouped_column_cell, aggregate_state->all_grouped_cols) {
                const int attribute_number = lfirst_int(grouped_column_cell);

                if (!bms_is_member(attribute_number, grouped_cols))
                    slot->tts_isnull[attribute_number - 1] = true;
            }
        }
    }
}

/** Finish all aggregate results for the selected group in the output context. */
export void finalize_aggregates(AggState* aggregate_state,
                                AggStatePerAgg aggregate_descriptors,
                                AggStatePerGroup group_state) {
    ExprContext* expression_context = aggregate_state->ss.ps.ps_ExprContext;
    Datum* aggregate_values = expression_context->ecxt_aggvalues;
    bool* aggregate_nulls = expression_context->ecxt_aggnulls;

    /* Feed sorted DISTINCT and ORDER BY inputs through their transition functions. */
    for (int transition_index = 0; transition_index < aggregate_state->numtrans;
         ++transition_index) {
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

    /* Produce final values or partial states according to the plan's aggregation mode. */
    for (int aggregate_index = 0; aggregate_index < aggregate_state->numaggs; ++aggregate_index) {
        AggStatePerAgg aggregate_descriptor = &aggregate_descriptors[aggregate_index];
        const int transition_index = aggregate_descriptor->transno;
        AggStatePerGroup per_group_state = &group_state[transition_index];

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

/** Project a group after its aggregate values have been finalized.
 * Return nullptr when HAVING rejects the group. */
export TupleTableSlot* project_aggregates(AggState* aggregate_state) {
    ExprContext* expression_context = aggregate_state->ss.ps.ps_ExprContext;

    /* Return only groups that pass HAVING. */
    if (ExecQual(aggregate_state->ss.ps.qual, expression_context)) {
        /* Build the output from aggregate results and the representative input row. */
        return ExecProject(aggregate_state->ss.ps.ps_ProjInfo);
    } else
        InstrCountFiltered1(aggregate_state, 1);

    return nullptr;
}
