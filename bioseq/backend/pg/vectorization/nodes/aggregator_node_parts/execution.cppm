/**
 * @file
 * @brief Executes sorted, plain, and hash vector aggregates.
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

/** Consumes the child plan into the initial aggregate hash tables. */
static void fill_aggregate_hash_table(AggState* aggregate_state) {
    TupleTableSlot* outer_slot;
    ExprContext* temporary_context = aggregate_state->tmpcontext;

    /**
     * Process each outer-plan tuple, and then fetch the next one, until we
     * exhaust the outer plan.
     */
    for (;;) {
        outer_slot = fetch_input_tuple(aggregate_state);
        if (TupIsNull(outer_slot))
            break;

        /* set up for lookup_hash_entries and advance_aggregates */
        temporary_context->ecxt_outertuple = outer_slot;

        /* Find or build hash_table entries */
        lookup_hash_entries(aggregate_state);

        /* Advance the aggregates (or combine functions) */
        advance_aggregates(aggregate_state);

        /**
         * Reset per-input-tuple context after each tuple, but note that the
         * hash lookups do this too
         */
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

/**
 * Rebuilds the hash tables from the next spilled batch.
 *
 * Returns false after both in-memory and spilled work are exhausted.
 */
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

    /**
     * Enables only the grouping set represented by the current spilled batch.
     */
    MemSet(
        aggregate_state->hash_pergroup, 0, sizeof(AggStatePerGroup) * aggregate_state->num_hashes);

    /* free memory and reset hash tables */
    ReScanExprContext(aggregate_state->hashcontext);
    for (int set_index = 0; set_index < aggregate_state->num_hashes; set_index++)
        ResetTupleHashTable(aggregate_state->perhash[set_index].hashtable);

    aggregate_state->hash_ngroups_current = 0;

    /**
     * Runs mixed-mode batch input in phase one and restores hash output phase zero.
     */
    Assert(aggregate_state->current_phase == 0);
    if (aggregate_state->phase->aggstrategy == AGG_MIXED) {
        aggregate_state->current_phase = 1;
        aggregate_state->phase = &aggregate_state->phases[aggregate_state->current_phase];
    }

    select_current_set(aggregate_state, batch->set_index, true);

    hash_state = &aggregate_state->perhash[aggregate_state->current_set];

    /**
     * Recompiles transition expressions for MinimalTuple input and nullable
     * inactive grouping sets.
     */
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
                /**
                 * Avoid initializing the spill until we actually need it so
                 * that we don't assign tapes that will never be used.
                 */
                spill_initialized = true;
                initialize_hash_aggregate_spill(&spill,
                                                tape_set,
                                                batch->used_bits,
                                                batch->input_card,
                                                aggregate_state->hashentrysize);
            }
            /* no memory for a new group, spill */
            spill_hash_aggregate_tuple(aggregate_state, &spill, spill_slot, hash_value);

            aggregate_state->hash_pergroup[batch->set_index] = nullptr;
        }

        /**
         * Reset per-input-tuple context after each tuple, but note that the
         * hash lookups do this too
         */
        ResetExprContext(aggregate_state->tmpcontext);
    }

    LogicalTapeClose(batch->input_tape);

    /* change back to phase 0 */
    aggregate_state->current_phase = 0;
    aggregate_state->phase = &aggregate_state->phases[aggregate_state->current_phase];

    if (spill_initialized) {
        finish_hash_aggregate_spill(aggregate_state, &spill, batch->set_index);
        update_hash_aggregate_metrics(aggregate_state, true, spill.npartitions);
    } else
        update_hash_aggregate_metrics(aggregate_state, true, 0);

    aggregate_state->hash_spill_mode = false;

    /* prepare to walk the first hash table */
    select_current_set(aggregate_state, batch->set_index, true);
    ResetTupleHashIterator(aggregate_state->perhash[batch->set_index].hashtable,
                           &aggregate_state->perhash[batch->set_index].hashiter);

    pfree(batch);

    return true;
}

/**
 * Retrieve the groups from the in-memory hash tables without considering any
 * spilled tuples.
 */
static TupleTableSlot* retrieve_aggregate_hash_table_in_memory(AggState* aggregate_state) {
    ExprContext* expression_context;
    AggStatePerAgg aggregate_descriptor;
    AggStatePerGroup group_state;
    TupleHashEntryData* entry;
    TupleTableSlot* first_slot;
    TupleTableSlot* result;
    AggStatePerHash hash_state;

    /**
     * Selects the output expression context for hash-group projection.
     */
    expression_context = aggregate_state->ss.ps.ps_ExprContext;
    aggregate_descriptor = aggregate_state->peragg;
    first_slot = aggregate_state->ss.ss_ScanTupleSlot;

    /**
     * Note that hash_state (and therefore anything accessed through it) can
     * change inside the loop, as we change between grouping sets.
     */
    hash_state = &aggregate_state->perhash[aggregate_state->current_set];

    /**
     * We loop retrieving groups until we find one satisfying
     * aggregate_state->ss.ps.qual
     */
    for (;;) {
        TupleTableSlot* hash_slot = hash_state->hashslot;

        CHECK_FOR_INTERRUPTS();

        /**
         * Find the next entry in the hash table
         */
        entry = ScanTupleHashTable(hash_state->hashtable, &hash_state->hashiter);
        if (entry == nullptr) {
            const int next_set = aggregate_state->current_set + 1;

            if (next_set < aggregate_state->num_hashes) {
                /**
                 * Switch to next grouping set, reinitialize, and restart the
                 * loop.
                 */
                select_current_set(aggregate_state, next_set, true);

                hash_state = &aggregate_state->perhash[aggregate_state->current_set];

                ResetTupleHashIterator(hash_state->hashtable, &hash_state->hashiter);

                continue;
            } else {
                return nullptr;
            }
        }

        /**
         * Resets output storage without firing aggregate shutdown callbacks early.
         */
        ResetExprContext(expression_context);

        /**
         * Transform representative tuple back into one with the right
         * columns.
         */
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

        /**
         * Use the representative input tuple for any references to
         * non-aggregated input columns in the qual and tlist.
         */
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
/**
 * Projects qualifying hash groups, refilling from spilled batches as needed.
 */
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

/** Retrieves the next result from sorted or plain aggregation. */
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

    /**
     * Selects the output and input expression contexts for sorted aggregation.
     */
    expression_context = aggregate_state->ss.ps.ps_ExprContext;
    temporary_context = aggregate_state->tmpcontext;

    aggregate_descriptor = aggregate_state->peragg;
    group_state_sets = aggregate_state->pergroups;
    first_slot = aggregate_state->ss.ss_ScanTupleSlot;

    /**
     * Advances until a group satisfies the aggregate qualification.
     */
    while (!aggregate_state->agg_done) {
        bool first_input_is_vector = false;

        /**
         * Rescans group contexts so transition shutdown callbacks run at the
         * boundary.
         */
        ReScanExprContext(expression_context);

        /**
         * Determine how many grouping sets need to be reset at this boundary.
         */
        if (aggregate_state->projected_set >= 0 &&
            aggregate_state->projected_set < grouping_set_count)
            reset_count = aggregate_state->projected_set + 1;
        else
            reset_count = grouping_set_count;

        /**
         * Resets contexts from the old phase before initializing the new phase.
         */

        for (int grouping_context_index = 0; grouping_context_index < reset_count;
             ++grouping_context_index) {
            ReScanExprContext(aggregate_state->aggcontexts[grouping_context_index]);
        }

        /**
         * Check if input is complete and there are no more groups to project
         * in this phase. Move to next phase or mark as done.
         */
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
                /**
                 * Mixed mode. We've output all the grouped stuff and have
                 * full hashtables, so switch to outputting those.
                 */
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

        /**
         * Selects the grouping prefix used for the next boundary comparison.
         */
        if (aggregate_state->projected_set >= 0 &&
            aggregate_state->projected_set < (grouping_set_count - 1))
            next_set_size =
                aggregate_state->phase->gset_lengths[aggregate_state->projected_set + 1];
        else
            next_set_size = 0;

        /**
         * Projects the next changed grouping set or an empty grouping set after
         * input exhaustion.
         */
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
            /**
             * Restarts projection at the first grouping set for the next group.
             */
            aggregate_state->projected_set = 0;

            /**
             * If we don't already have the first tuple of the new group,
             * fetch it from the outer plan.
             */
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
                                /**
                                 * Leaves the executor active so later phases can run.
                                 */
                                break;
                            }
                        }

                        if (aggregate_state->projected_set >= grouping_set_count)
                            continue;
                    } else {
                        aggregate_state->agg_done = true;
                        /* If we are grouping, we should produce no tuples too */
                        if (aggregate_plan->aggstrategy != AGG_PLAIN)
                            return nullptr;
                    }
                }
            }

            /**
             * Initialize working state for a new input tuple group.
             */
            initialize_aggregates(aggregate_state, group_state_sets, reset_count);

            if (aggregate_state->grp_firstTuple != nullptr || first_input_is_vector) {
                /**
                 * Retains the representative row in its tuple-table slot.
                 */
                if (aggregate_state->grp_firstTuple != nullptr) {
                    ExecForceStoreHeapTuple(aggregate_state->grp_firstTuple, first_slot, true);
                    aggregate_state->grp_firstTuple = nullptr;
                }

                /* set up for first advance_aggregates call */
                temporary_context->ecxt_outertuple = outer_slot;

                /**
                 * Process each outer-plan tuple, and then fetch the next one,
                 * until we exhaust the outer plan or cross a group boundary.
                 */
                for (;;) {
                    if (!is_vector_tuple_table_slot(outer_slot)) {
                        ereport(ERROR,
                                (errcode(ERRCODE_INTERNAL_ERROR),
                                 errmsg("VectorAgg input is not a vector tuple slot")));
                    }

                    /**
                     * During phase 1 only of a mixed agg, we need to update
                     * hashtables as well in advance_aggregates.
                     */
                    if (aggregate_state->aggstrategy == AGG_MIXED &&
                        aggregate_state->current_phase == 1) {
                        lookup_hash_entries(aggregate_state);
                    }

                    /* Advance the aggregates (or combine functions) */
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

                    /* Reset per-input-tuple context after each tuple */
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
                    /* set up for next advance_aggregates call */
                    temporary_context->ecxt_outertuple = outer_slot;

                    /**
                     * If we are grouping, check whether we've crossed a group
                     * boundary.
                     */
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

            /**
             * Exposes the representative row to direct arguments, qualification,
             * and projection.
             */
            expression_context->ecxt_outertuple = first_slot;
        }

        Assert(aggregate_state->projected_set >= 0);

        current_set = aggregate_state->projected_set;

        prepare_projection_slot(aggregate_state, expression_context->ecxt_outertuple, current_set);

        select_current_set(aggregate_state, current_set, false);

        finalize_aggregates(aggregate_state, aggregate_descriptor, group_state_sets[current_set]);

        /**
         * If there's no row to project right now, we must continue rather
         * than returning a null since there might be more groups.
         */
        result = project_aggregates(aggregate_state);
        if (result)
            return result;
    }

    /* No more groups */
    return nullptr;
}

/**
 * Executes grouped or plain aggregation and projects each qualifying result.
 */
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
