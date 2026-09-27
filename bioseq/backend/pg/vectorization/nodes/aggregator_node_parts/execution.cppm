/**
 * @file
 * @brief Run aggregate plans for the vector custom scan.
 */
module;
#include <bioseq/pg_compat.h>
extern "C" {
#include "nodes/extensible.h"
}
#include <algorithm>

export module bioseq.postgres.vectorization.aggregator_node:execution;

import bioseq.common.logging;
import bioseq.postgres.support;
import bioseq.postgres.vectorization.types;

import :support;
import :transitions;
import :hash_runtime;

/** Read the child plan and accumulate groups in the initial hash tables. */
static void fill_aggregate_hash_table(AggState* aggregate_state) {
    TupleTableSlot* outer_slot;
    ExprContext* temporary_context = aggregate_state->tmpcontext;

    /* Consume input until the child plan is exhausted. */
    for (;;) {
        outer_slot = fetch_input_tuple(aggregate_state);
        if (TupIsNull(outer_slot))
            break;

        /* Make this input available to hash  lookup and transition expressions. */
        temporary_context->ecxt_outertuple = outer_slot;

        /* Find each grouping set's state, creating a hash entry when needed. */
        lookup_hash_entries(aggregate_state);

        /* Update the aggregate states from this input. */
        advance_aggregates(aggregate_state);

        /* Release temporary storage used by this input. */
        ResetExprContext(aggregate_state->tmpcontext);
    }

    /* finalize spills, if any */
    finish_initial_hash_aggregate_spills(aggregate_state);

    aggregate_state->table_filled = true;
    /* Initialize to walk the first hash table */
    select_current_set(aggregate_state, 0, true);
    ResetTupleHashIterator(aggregate_state->perhash[0].hashtable,
                           &aggregate_state->perhash[0].hashiter);
}

/** Rebuild the hash table for the next spilled batch. Return false when no batches remain. */
static bool refill_aggregate_hash_table(AggState* aggregate_state) {
    hash_aggregate_batch* batch;
    AggStatePerHash hash_state;
    hash_aggregate_spill spill;
    LogicalTapeSet* tape_set = aggregate_state->hash_tapeset;
    bool spill_initialized = false;

    if (aggregate_state->hash_batches == NIL)
        return false;

    /* hash_batches is a stack, with the top item at the end of the list */
    batch = static_cast<hash_aggregate_batch*>(llast(aggregate_state->hash_batches));
    aggregate_state->hash_batches = list_delete_last(aggregate_state->hash_batches);

    hash_agg_set_limits(aggregate_state->hashentrysize,
                        batch->input_card,
                        batch->used_bits,
                        &aggregate_state->hash_mem_limit,
                        &aggregate_state->hash_ngroups_limit,
                        nullptr);

    /* Only the grouping set in this batch will receive transition updates. */
    MemSet(
        aggregate_state->hash_pergroup, 0, sizeof(AggStatePerGroup) * aggregate_state->num_hashes);

    /* Free the previous groups and clear the hash tables for this batch. */
    ReScanExprContext(aggregate_state->hashcontext);
    for (int set_index = 0; set_index < aggregate_state->num_hashes; set_index++)
        ResetTupleHashTable(aggregate_state->perhash[set_index].hashtable);

    aggregate_state->hash_ngroups_current = 0;

    /* Mixed, aggregation uses phase one to process input and phase zero
     * to return  hash groups. */
    Assert(aggregate_state->current_phase == 0);
    if (aggregate_state->phase->aggstrategy == AGG_MIXED) {
        aggregate_state->current_phase = 1;
        aggregate_state->phase = &aggregate_state->phases[aggregate_state->current_phase];
    }

    select_current_set(aggregate_state, batch->set_index, true);

    hash_state = &aggregate_state->perhash[aggregate_state->current_set];

    /* Read spilled MinimalTuples and skip grouping sets absent from this batch */
    recompile_hash_aggregate_expressions(aggregate_state, true, true);

    for (;;) {
        TupleTableSlot* spill_slot = aggregate_state->hash_spill_rslot;
        TupleTableSlot* hash_slot = hash_state->hashslot;
        TupleHashEntry entry;
        MinimalTuple spilled_tuple;
        uint32_t hash_value;
        bool entry_is_new = false;
        bool* const entry_is_new_out = aggregate_state->hash_spill_mode ? nullptr : &entry_is_new;

        CHECK_FOR_INTERRUPTS();

        spilled_tuple = read_hash_aggregate_batch(batch, &hash_value);
        if (spilled_tuple == nullptr)
            break;

        ExecStoreMinimalTuple(spilled_tuple, spill_slot, true);
        aggregate_state->tmpcontext->ecxt_outertuple = spill_slot;

        prepare_hash_slot(hash_state, aggregate_state->tmpcontext->ecxt_outertuple, hash_slot);
        entry = LookupTupleHashEntryHash(
            hash_state->hashtable, hash_slot, entry_is_new_out, hash_value);

        if (entry != nullptr) {
            if (entry_is_new)
                initialize_hash_entry(aggregate_state, hash_state->hashtable, entry);
            aggregate_state->hash_pergroup[batch->set_index] = static_cast<AggStatePerGroupData*>(
                bioseq_tuple_hash_entry_additional(hash_state->hashtable, entry));
            advance_aggregates(aggregate_state);
        } else {
            if (!spill_initialized) {
                /* Allocate spill tapes only when another group no longer fits in memory. */
                spill_initialized = true;
                initialize_hash_aggregate_spill(&spill,
                                                tape_set,
                                                batch->used_bits,
                                                batch->input_card,
                                                aggregate_state->hashentrysize);
            }
            /* Save the row for a later batch because its group does not fit. */
            spill_hash_aggregate_tuple(aggregate_state, &spill, spill_slot, hash_value);

            aggregate_state->hash_pergroup[batch->set_index] = nullptr;
        }

        /* Release temporary storage used while remding this row. */
        ResetExprContext(aggregate_state->tmpcontext);
    }

    LogicalTapeClose(batch->input_tape);

    /* Return to the hash-output phase. */
    aggregate_state->current_phase = 0;
    aggregate_state->phase = &aggregate_state->phases[aggregate_state->current_phase];

    if (spill_initialized) {
        finish_hash_aggregate_spill(aggregate_state, &spill, batch->set_index);
        update_hash_aggregate_metrics(aggregate_state, true, spill.npartitions);
    } else
        update_hash_aggregate_metrics(aggregate_state, true, 0);

    aggregate_state->hash_spill_mode = false;

    /* Prepare to return groups from the table rebuilt for this batch. */
    select_current_set(aggregate_state, batch->set_index, true);
    ResetTupleHashIterator(aggregate_state->perhash[batch->set_index].hashtable,
                           &aggregate_state->perhash[batch->set_index].hashiter);

    pfree(batch);

    return true;
}

/** Return the next in-memory hash group that passes HAVING. */
static TupleTableSlot* retrieve_aggregate_hash_table_in_memory(AggState* aggregate_state) {
    ExprContext* expression_context;
    AggStatePerAgg aggregate_descriptor;
    AggStatePerGroup group_state;
    TupleHashEntryData* entry;
    TupleTableSlot* first_slot;
    TupleTableSlot* result;
    AggStatePerHash hash_state;

    /* Use the output context  for HAVING and result expressions. */
    expression_context = aggregate_state->ss.ps.ps_ExprContext;
    aggregate_descriptor = aggregate_state->peragg;
    first_slot = aggregate_state->ss.ss_ScanTupleSlot;

    /* This pointer changes when the loop moves to another grouping set. */
    hash_state = &aggregate_state->perhash[aggregate_state->current_set];

    /* Skip groups rejected by HAVING. */
    for (;;) {
        TupleTableSlot* hash_slot = hash_state->hashslot;

        CHECK_FOR_INTERRUPTS();

        /* Read the next group from the current hash table. */
        entry = ScanTupleHashTable(hash_state->hashtable, &hash_state->hashiter);
        if (entry == nullptr) {
            const int next_set = aggregate_state->current_set + 1;

            if (next_set < aggregate_state->num_hashes) {
                /* Start the next grouping set's hash-table iterator. */
                select_current_set(aggregate_state, next_set, true);

                hash_state = &aggregate_state->perhash[aggregate_state->current_set];

                ResetTupleHashIterator(hash_state->hashtable, &hash_state->hashiter);

                continue;
            } else {
                return nullptr;
            }
        }

        /* Clear output storage while keeping aggregate shutdown callbacks pending. */
        ResetExprContext(expression_context);

        /* Put the saved group columns back in their original input positions. */
        ExecStoreMinimalTuple(entry->firstTuple, hash_slot, false);
        slot_getallattrs(hash_slot);

        ExecClearTuple(first_slot);
        memset(first_slot->tts_isnull, true, first_slot->tts_tupleDescriptor->natts * sizeof(bool));

        for (int hash_column_index = 0; hash_column_index < hash_state->numhashGrpCols;
             ++hash_column_index) {
            const int variable_index = hash_state->hashGrpColIdxInput[hash_column_index] - 1;

            first_slot->tts_values[variable_index] = hash_slot->tts_values[hash_column_index];
            first_slot->tts_isnull[variable_index] = hash_slot->tts_isnull[hash_column_index];
        }
        ExecStoreVirtualTuple(first_slot);

        group_state = static_cast<AggStatePerGroup>(
            bioseq_tuple_hash_entry_additional(hash_state->hashtable, entry));

        /* Use the group's saved input values for non-aggregate references
         * in HAVING and the result expressions. */
        expression_context->ecxt_outertuple = first_slot;

        prepare_projection_slot(
            aggregate_state, expression_context->ecxt_outertuple, aggregate_state->current_set);

        finalize_aggregates(aggregate_state, aggregate_descriptor, group_state);

        result = project_aggregates(aggregate_state);
        if (result)
            return result;
    }

    /* No more groups */
    return nullptr;
}
/** Return hash groups that pass HAVING, loading spilled batches when needed. */
static TupleTableSlot* retrieve_aggregate_hash_table(AggState* aggregate_state) {
    TupleTableSlot* result = nullptr;

    while (result == nullptr) {
        result = retrieve_aggregate_hash_table_in_memory(aggregate_state);
        if (result == nullptr) {
            if (!refill_aggregate_hash_table(aggregate_state)) {
                aggregate_state->agg_done = true;
                break;
            }
        }
    }

    return result;
}

/** Return  the next result from direct aggregation. Vector input currently requires AGG_PLAIN. */
static TupleTableSlot* retrieve_direct_aggregate(vector_aggregate_state* vector_state) {
    AggState* aggregate_state = vector_state->aggregate_state;
    Agg* aggregate_plan = aggregate_state->phase->aggnode;
    ExprContext* expression_context;
    ExprContext* temporary_context;
    AggStatePerAgg aggregate_descriptor;
    AggStatePerGroup* group_state_sets;
    TupleTableSlot* outer_slot = nullptr;
    TupleTableSlot* first_slot;
    TupleTableSlot* result;
    const bool has_grouping_sets = aggregate_state->phase->numsets > 0;
    int grouping_set_count = std::max(aggregate_state->phase->numsets, 1);
    int current_set;
    int next_set_size;
    int reset_count;

    /* Keep output expressions separate from temporary input work */
    expression_context = aggregate_state->ss.ps.ps_ExprContext;
    temporary_context = aggregate_state->tmpcontext;

    aggregate_descriptor = aggregate_state->peragg;
    group_state_sets = aggregate_state->pergroups;
    first_slot = aggregate_state->ss.ss_ScanTupleSlot;

    /* Continue until a group passes HAVING or aggregation finishes. */
    while (!aggregate_state->agg_done) {
        bool first_input_is_vector = false;

        /* Reset output storage and run its pending shutdown callbacks. */
        ReScanExprContext(expression_context);

        /* Reset only the grouping sets whoee previous result has been returned. */
        if (aggregate_state->projected_set >= 0 &&
            aggregate_state->projected_set < grouping_set_count)
            reset_count = aggregate_state->projected_set + 1;
        else
            reset_count = grouping_set_count;

        /* Release state for those completed groups before processing the next ones. */

        for (int grouping_context_index = 0; grouping_context_index < reset_count;
             ++grouping_context_index) {
            ReScanExprContext(aggregate_state->aggcontexts[grouping_context_index]);
        }

        /* After returning every group in this phase, advance to the next phase
         * or finish  aggregation. */
        if (aggregate_state->input_done &&
            aggregate_state->projected_set >= (grouping_set_count - 1)) {
            if (aggregate_state->current_phase < aggregate_state->numphases - 1) {
                initialize_phase(aggregate_state, aggregate_state->current_phase + 1);
                aggregate_state->input_done = false;
                aggregate_state->projected_set = -1;
                grouping_set_count = std::max(aggregate_state->phase->numsets, 1);
                aggregate_plan = aggregate_state->phase->aggnode;
                reset_count = grouping_set_count;
            } else if (aggregate_state->aggstrategy == AGG_MIXED) {
                /* Sorted results are complete. Return the hash groups accumulated
                 * during mixed aggregation. */
                initialize_phase(aggregate_state, 0);
                aggregate_state->table_filled = true;
                ResetTupleHashIterator(aggregate_state->perhash[0].hashtable,
                                       &aggregate_state->perhash[0].hashiter);
                select_current_set(aggregate_state, 0, true);
                return retrieve_aggregate_hash_table(aggregate_state);
            } else {
                aggregate_state->agg_done = true;
                break;
            }
        }

        /* Choose the next grouping prefix to check for a group boundary. */
        if (aggregate_state->projected_set >= 0 &&
            aggregate_state->projected_set < (grouping_set_count - 1))
            next_set_size =
                aggregate_state->phase->gset_lengths[aggregate_state->projected_set + 1];
        else
            next_set_size = 0;

        /* Return a completed grouping set when its prefix changes. At end
         * of input, finish the remaining sets, including empty grouping sets. */
        temporary_context->ecxt_innertuple = expression_context->ecxt_outertuple;
        if (aggregate_state->input_done ||
            (aggregate_plan->aggstrategy != AGG_PLAIN && aggregate_state->projected_set != -1 &&
             aggregate_state->projected_set < (grouping_set_count - 1) && next_set_size > 0 &&
             !ExecQualAndReset(aggregate_state->phase->eqfunctions[next_set_size - 1],
                               temporary_context))) {
            aggregate_state->projected_set += 1;

            Assert(aggregate_state->projected_set < grouping_set_count);
            Assert(next_set_size > 0 || aggregate_state->input_done);
        } else {
            /* Start the next group at the first grouping set. */
            aggregate_state->projected_set = 0;

            /* Fetch the first input if a previous, group did not already save it. */
            if (aggregate_state->grp_firstTuple == nullptr) {
                outer_slot = fetch_input_tuple(aggregate_state);
                if (!TupIsNull(outer_slot)) {
                    first_input_is_vector = is_vector_tuple_table_slot(outer_slot);
                    if (first_input_is_vector) {
                        if (aggregate_plan->aggstrategy != AGG_PLAIN) {
                            ereport(ERROR,
                                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                                     errmsg("VectorAgg supports only plain aggregation")));
                        }
                    } else {
                        ereport(ERROR,
                                (errcode(ERRCODE_INTERNAL_ERROR),
                                 errmsg("VectorAgg input is not a vector tuple slot")));
                    }
                } else {
                    /* outer plan produced no tuples at all */
                    if (has_grouping_sets) {
                        /**
                         * Empty input produces rows only for empty grouping sets.
                         */
                        aggregate_state->input_done = true;

                        while (
                            aggregate_state->phase->gset_lengths[aggregate_state->projected_set] >
                            0) {
                            aggregate_state->projected_set += 1;
                            if (aggregate_state->projected_set >= grouping_set_count) {
                                /* Later phases may still have an empty grouping set to  return. */
                                break;
                            }
                        }

                        if (aggregate_state->projected_set >= grouping_set_count)
                            continue;
                    } else {
                        aggregate_state->agg_done = true;
                        /* Grouped aggregation has no result for empty input. */
                        if (aggregate_plan->aggstrategy != AGG_PLAIN)
                            return nullptr;
                    }
                }
            }

            /* Initialize transition states for the new group. */
            initialize_aggregates(aggregate_state, group_state_sets, reset_count);

            if (aggregate_state->grp_firstTuple != nullptr || first_input_is_vector) {
                /* Move the saved first row into the representative input slot */
                if (aggregate_state->grp_firstTuple != nullptr) {
                    ExecForceStoreHeapTuple(aggregate_state->grp_firstTuple, first_slot, true);
                    aggregate_state->grp_firstTuple = nullptr;
                }

                /* Make the first input available to transition expressions. */
                temporary_context->ecxt_outertuple = outer_slot;

                /* Consume inpxt batches until the plan ends or a group boundary is reached. */
                for (;;) {
                    if (!is_vector_tuple_table_slot(outer_slot)) {
                        ereport(ERROR,
                                (errcode(ERRCODE_INTERNAL_ERROR),
                                 errmsg("VectorAgg input is not a vector tuple slot")));
                    }

                    /* In the first mixed phase, update the hash groups alongside the
                     * directly accumulated groups. */
                    if (aggregate_state->aggstrategy == AGG_MIXED &&
                        aggregate_state->current_phase == 1) {
                        lookup_hash_entries(aggregate_state);
                    }

                    /* Update aggregate states from this  batch. */
                    advance_aggregates(aggregate_state);

                    current_set = aggregate_state->projected_set;

                    prepare_projection_slot(
                        aggregate_state, expression_context->ecxt_outertuple, current_set);

                    select_current_set(aggregate_state, current_set, false);

                    for (int transition_index = 0; transition_index < aggregate_state->numtrans;
                         ++transition_index) {
                        AggStatePerTrans transition_state =
                            &aggregate_state->pertrans[transition_index];
                        AggStatePerGroup per_group_state;

                        per_group_state = &group_state_sets[current_set][transition_index];

                        if (transition_state->aggref->aggstar) {
                            const auto* vector_slot =
                                reinterpret_cast<vector_tuple_table_slot_storage*>(outer_slot);
                            per_group_state->transValue =
                                Int64GetDatum(DatumGetInt64(per_group_state->transValue) +
                                              static_cast<std::int64_t>(vector_slot->row_count));
                        }
                    }

                    /* Release temporary storage used by this input batch. */
                    ResetExprContext(temporary_context);

                    outer_slot = fetch_input_tuple(aggregate_state);
                    if (TupIsNull(outer_slot)) {
                        /* no more outer-plan tuples available */

                        /* if we built hash tables, finalize any spills */
                        if (aggregate_state->aggstrategy == AGG_MIXED &&
                            aggregate_state->current_phase == 1)
                            finish_initial_hash_aggregate_spills(aggregate_state);

                        if (has_grouping_sets) {
                            aggregate_state->input_done = true;
                            break;
                        } else {
                            aggregate_state->agg_done = true;
                            break;
                        }
                    }
                    /* Make the next input available to transition expressions. */
                    temporary_context->ecxt_outertuple = outer_slot;

                    /* Compare grouping columns and save the first row of a new group. */
                    if (aggregate_plan->aggstrategy != AGG_PLAIN) {
                        temporary_context->ecxt_innertuple = first_slot;
                        if (!ExecQual(
                                aggregate_state->phase->eqfunctions[aggregate_plan->numCols - 1],
                                temporary_context)) {
                            aggregate_state->grp_firstTuple = ExecCopySlotHeapTuple(outer_slot);
                            break;
                        }
                    }
                }
            }

            /* Use the representative row for direct aggregate arguments, HAVING,
             * and the result expressions. */
            expression_context->ecxt_outertuple = first_slot;
        }

        Assert(aggregate_state->projected_set >= 0);

        current_set = aggregate_state->projected_set;

        prepare_projection_slot(aggregate_state, expression_context->ecxt_outertuple, current_set);

        select_current_set(aggregate_state, current_set, false);

        finalize_aggregates(aggregate_state, aggregate_descriptor, group_state_sets[current_set]);

        /* A group rejected by HAVING produces no row. Keep looking because
         * later groups may still produce results. */
        result = project_aggregates(aggregate_state);
        if (result)
            return result;
    }

    /* No more groups */
    return nullptr;
}

/** Return the next qualifying aggregate result using the current execution strategy. */
export TupleTableSlot* execute_aggregate_plan(PlanState* plan_state) {
    vector_aggregate_state* vector_state = reinterpret_cast<vector_aggregate_state*>(plan_state);
    AggState* aggregate_state = vector_state->aggregate_state;
    TupleTableSlot* result = nullptr;

    CHECK_FOR_INTERRUPTS();

    if (!aggregate_state->agg_done) {
        /* Dispatch based on strategy */
        switch (aggregate_state->phase->aggstrategy) {
        case AGG_HASHED:
            if (!aggregate_state->table_filled)
                fill_aggregate_hash_table(aggregate_state);
            /* FALLTHROUGH */
        case AGG_MIXED:
            result = retrieve_aggregate_hash_table(aggregate_state);
            break;
        case AGG_PLAIN:
        case AGG_SORTED:
            result = retrieve_direct_aggregate(vector_state);
            break;
        }

        if (!TupIsNull(result))
            return result;
    }

    return nullptr;
}
