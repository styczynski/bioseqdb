/**
 * @file
 * @brief Manage aggregate hash tables and spill excess groups to  temporary files.
 */
module;
#include <bioseq/pg_compat.h>
extern "C" {
#include "nodes/extensible.h"
#include "port/pg_bitutils.h"
}
#include <algorithm>
#include <cmath>
#include <cstdint>

export module bioseq.postgres.vectorization.aggregator_node:hash_runtime;

import bioseq.common.logging;
import bioseq.postgres.support;
import bioseq.postgres.vectorization.types;
import :support;
import :transitions;

using bioseq::postgres::support::allocate_zeroed_array;
using bioseq::postgres::support::allocate_zeroed_object;
using bioseq::postgres::support::allocate_zeroed_storage;
using bioseq::postgres::support::pg_expression_walk_cursor;
using bioseq::postgres::support::pg_node_view;
using bioseq::postgres::support::pg_walk_step;
using bioseq::postgres::support::walk_pg_expression_including_root;

/** Separate input columns used inside aggregate calls from other input references. */
struct aggregate_column_classifier {
    [[nodiscard]] pg_walk_step operator()(pg_expression_walk_cursor cursor,
                                          pg_node_view node,
                                          find_columns_context& context) const {
        if (Var* var = node.try_cast<Var>()) {
            /* setrefs.c should have set the varno to OUTER_VAR */
            Assert(var->varno == OUTER_VAR);
            Assert(var->varlevelsup == 0);
            if (context.is_aggref)
                context.aggregated = bms_add_member(context.aggregated, var->varattno);
            else
                context.unaggregated = bms_add_member(context.unaggregated, var->varattno);
            return pg_walk_step::skip_subtree;
        }
        if (node.is_type<Aggref>()) {
            Assert(!context.is_aggref);
            context.is_aggref = true;
            cursor.descend(node.get());
            context.is_aggref = false;
            return pg_walk_step::skip_subtree;
        }
        return pg_walk_step::descend;
    }
};

/** Find input columns needed by aggregate arguments, grouping, HAVING, and output expressions. */
static void find_aggregate_columns(AggState* aggregate_state,
                                   Bitmapset** aggregated_columns_out,
                                   Bitmapset** unaggregated_columns_out) {
    Agg* aggregate_plan = reinterpret_cast<Agg*>(aggregate_state->ss.ps.plan);
    find_columns_context context;

    context.is_aggref = false;
    context.aggregated = nullptr;
    context.unaggregated = nullptr;

    /* Find column references in output expressions and HAVING. */
    aggregate_column_classifier column_classifier;
    (void)walk_pg_expression_including_root(
        reinterpret_cast<Node*>(aggregate_plan->plan.targetlist), context, column_classifier);
    (void)walk_pg_expression_including_root(
        reinterpret_cast<Node*>(aggregate_plan->plan.qual), context, column_classifier);

    /* Keep grouping columns even when the, query does not rekurn them. */
    for (int group_column_index = 0; group_column_index < aggregate_plan->numCols;
         ++group_column_index)
        context.unaggregated =
            bms_add_member(context.unaggregated, aggregate_plan->grpColIdx[group_column_index]);

    *aggregated_columns_out = context.aggregated;
    *unaggregated_columns_out = context.unaggregated;
}

/** Size the initial bucket array from the expected groups and available memory */
static long choose_hash_bucket_count(double hash_entry_size, long group_count, Size memory_budget) {
    long maximum_bucket_count;
    long bucket_count = group_count;

    maximum_bucket_count = memory_budget / hash_entry_size;

    /* Leave room for group keys and transition values as well as buckets. */
    maximum_bucket_count >>= 1;

    if (bucket_count > maximum_bucket_count)
        bucket_count = maximum_bucket_count;

    return std::max<long>(bucket_count, 1);
}

/** Create the hash table and per-group state storage for one grouping set. */
static void build_hash_table(AggState* aggregate_state, int set_index, long bucket_count) {
    AggStatePerHash hash_state = &aggregate_state->perhash[set_index];
    MemoryContext metadata_context = aggregate_state->hash_metacxt;
    MemoryContext hash_context = aggregate_state->hashcontext->ecxt_per_tuple_memory;
    MemoryContext temporary_context = aggregate_state->tmpcontext->ecxt_per_tuple_memory;
    Size per_entry_state_size;

    Assert(aggregate_state->aggstrategy == AGG_HASHED || aggregate_state->aggstrategy == AGG_MIXED);

    /* Reserve  the fixed transition-state records stored with each group.
     * Separately allocated transition values and tuples need additional memory. */
    per_entry_state_size = aggregate_state->numtrans * sizeof(AggStatePerGroupData);

    hash_state->hashtable =
        bioseq_build_tuple_hash_table(&aggregate_state->ss.ps,
                                      hash_state->hashslot->tts_tupleDescriptor,
                                      hash_state->numCols,
                                      hash_state->hashGrpColIdxHash,
                                      hash_state->eqfuncoids,
                                      hash_state->hashfunctions,
                                      hash_state->aggnode->grpCollations,
                                      bucket_count,
                                      per_entry_state_size,
                                      metadata_context,
                                      hash_context,
                                      temporary_context,
                                      DO_AGGSPLIT_SKIPFINAL(aggregate_state->aggsplit));
}

/** Clear existing hash tables or create one for each hashed grouping set. */
export void build_hash_tables(AggState* aggregate_state) {
    int set_index;

    for (set_index = 0; set_index < aggregate_state->num_hashes; ++set_index) {
        AggStatePerHash hash_state = &aggregate_state->perhash[set_index];
        long bucket_count;
        Size memory_budget;

        if (hash_state->hashtable != nullptr) {
            ResetTupleHashTable(hash_state->hashtable);
            continue;
        }

        Assert(hash_state->aggnode->numGroups > 0);

        memory_budget = aggregate_state->hash_mem_limit / aggregate_state->num_hashes;

        /* Fit this table's initial buckets within its share of the memory budget. */
        bucket_count = choose_hash_bucket_count(
            aggregate_state->hashentrysize, hash_state->aggnode->numGroups, memory_budget);

        build_hash_table(aggregate_state, set_index, bucket_count);
    }

    aggregate_state->hash_ngroups_current = 0;
}

/**
 * Build compact hash tuples and map their columns back to the input.
 * Keep grouping keys and the input values needed by HAVING or output
 * expressions. Aggregate-only inputs are read while updating the  group.
 */
export void find_hash_columns(AggState* aggregate_state) {
    Bitmapset* unaggregated_columns;
    Bitmapset* aggregated_columns;
    TupleDesc scan_descriptor = aggregate_state->ss.ss_ScanTupleSlot->tts_tupleDescriptor;
    List* outer_target_list = outerPlanState(aggregate_state)->plan->targetlist;
    const int hash_table_count = aggregate_state->num_hashes;
    EState* executor_state = aggregate_state->ss.ps.state;

    /* Find every input column needed to update or return a group. */
    find_aggregate_columns(aggregate_state, &aggregated_columns, &unaggregated_columns);
    aggregate_state->colnos_needed = bms_union(unaggregated_columns, aggregated_columns);
    aggregate_state->max_colno_needed = 0;
    aggregate_state->all_cols_needed = true;

    for (int attribute_index = 0; attribute_index < scan_descriptor->natts; ++attribute_index) {
        const int column_number = attribute_index + 1;

        if (bms_is_member(column_number, aggregate_state->colnos_needed))
            aggregate_state->max_colno_needed = column_number;
        else
            aggregate_state->all_cols_needed = false;
    }

    for (int hash_table_index = 0; hash_table_index < hash_table_count; ++hash_table_index) {
        AggStatePerHash hash_state = &aggregate_state->perhash[hash_table_index];
        Bitmapset* column_numbers = bms_copy(unaggregated_columns);
        AttrNumber* group_column_indexes = hash_state->aggnode->grpColIdx;
        List* hash_target_entries = NIL;
        TupleDesc hash_descriptor;
        int maximum_column_count;

        hash_state->largestGrpColIdx = 0;

        /* Omit grouping columns that this set will return as NULL. */
        if (aggregate_state->phases[0].grouped_cols) {
            Bitmapset* grouped_columns = aggregate_state->phases[0].grouped_cols[hash_table_index];
            ListCell* cell;

            foreach (cell, aggregate_state->all_grouped_cols) {
                const int attribute_number = lfirst_int(cell);

                if (!bms_is_member(attribute_number, grouped_columns))
                    column_numbers = bms_del_member(column_numbers, attribute_number);
            }
        }

        /* Leave room for grouping indexes that name the same input column. */
        maximum_column_count = bms_num_members(column_numbers) + hash_state->numCols;

        hash_state->hashGrpColIdxInput = allocate_zeroed_array<AttrNumber>(maximum_column_count);
        hash_state->hashGrpColIdxHash = allocate_zeroed_array<AttrNumber>(hash_state->numCols);

        /* Add all the grouping columns to column_numbers. */
        for (int group_column_index = 0; group_column_index < hash_state->numCols;
             ++group_column_index)
            column_numbers =
                bms_add_member(column_numbers, group_column_indexes[group_column_index]);

        /* Put hash keys first and record where each comes from in the input. */
        for (int group_column_index = 0; group_column_index < hash_state->numCols;
             ++group_column_index) {
            hash_state->hashGrpColIdxInput[group_column_index] =
                group_column_indexes[group_column_index];
            hash_state->hashGrpColIdxHash[group_column_index] = group_column_index + 1;
            hash_state->numhashGrpCols++;
            /* delete already mapped columns */
            bms_del_member(column_numbers, group_column_indexes[group_column_index]);
        }

        /* Append the remaining input columns needed for output. */
        int column_number = -1;
        while ((column_number = bms_next_member(column_numbers, column_number)) >= 0) {
            hash_state->hashGrpColIdxInput[hash_state->numhashGrpCols] = column_number;
            hash_state->numhashGrpCols++;
        }

        /* Build the tuple type for those stored columns. */
        for (int hash_column_index = 0; hash_column_index < hash_state->numhashGrpCols;
             ++hash_column_index) {
            const int variable_index = hash_state->hashGrpColIdxInput[hash_column_index] - 1;

            hash_target_entries =
                lappend(hash_target_entries, list_nth(outer_target_list, variable_index));
            hash_state->largestGrpColIdx =
                std::max(variable_index + 1, hash_state->largestGrpColIdx);
        }

        hash_descriptor = ExecTypeFromTL(hash_target_entries);

        execTuplesHashPrepare(hash_state->numCols,
                              hash_state->aggnode->grpOperators,
                              &hash_state->eqfuncoids,
                              &hash_state->hashfunctions);
        hash_state->hashslot = ExecAllocTableSlot(
            &executor_state->es_tupleTable, hash_descriptor, &TTSOpsMinimalTuple
#if PG_VERSION_NUM >= 190000
            , 0  // Use the default xlot flags for these derived aggregate tuples.
#endif
        );

        list_free(hash_target_entries);
        bms_free(column_numbers);
    }

    bms_free(unaggregated_columns);
}

/** Estimate memory per hash group, including its tuple and transition  state allocations. */
extern "C" Size hash_agg_entry_size(int transition_count, Size tuple_width, Size transition_space) {
    Size tuple_chunk_size;
    Size per_group_chunk_size;
    Size transition_chunk_size;
    Size tuple_size = (MAXALIGN(SizeofMinimalTupleHeader) + tuple_width);
    Size per_group_size = transition_count * sizeof(AggStatePerGroupData);

    tuple_chunk_size = aggregate_chunk_header_bytes + tuple_size;

    if (per_group_size > 0)
        per_group_chunk_size = aggregate_chunk_header_bytes + per_group_size;
    else
        per_group_chunk_size = 0;

    if (transition_space > 0)
        transition_chunk_size = aggregate_chunk_header_bytes + transition_space;
    else
        transition_chunk_size = 0;

    return sizeof(TupleHashEntryData) + tuple_chunk_size + per_group_chunk_size +
           transition_chunk_size;
}

/**
 * Select a compiled transition expression for the current input slot and spill mode
 * Cache separate versions for MinimalTuple input and for checks that skip
 * grouping sets whose rows were spilled.
 */
export void recompile_hash_aggregate_expressions(AggState* aggregate_state,
                                                 bool use_minimal_slot,
                                                 bool check_null) {
    AggStatePerPhase phase;
    const int slot_cache_index = use_minimal_slot ? 1 : 0;
    const int null_check_cache_index = check_null ? 1 : 0;

    Assert(aggregate_state->aggstrategy == AGG_HASHED || aggregate_state->aggstrategy == AGG_MIXED);

    if (aggregate_state->aggstrategy == AGG_HASHED)
        phase = &aggregate_state->phases[0];
    else /* AGG_MIXED */
        phase = &aggregate_state->phases[1];

    if (phase->evaltrans_cache[slot_cache_index][null_check_cache_index] == nullptr) {
        const TupleTableSlotOps* outer_slot_ops = aggregate_state->ss.ps.outerops;
        bool outer_ops_fixed = aggregate_state->ss.ps.outeropsfixed;
        bool process_hash = true;
        bool process_sorted = false;

        /* Mixed-mode, input updates sorted groups too. Replayed spill batches
         * update only the selected hash grouping set. */
        if (aggregate_state->aggstrategy == AGG_MIXED && !use_minimal_slot)
            process_sorted = true;

        /* Compile against the slot type used by spilled tuples. */
        if (use_minimal_slot) {
            aggregate_state->ss.ps.outerops = &TTSOpsMinimalTuple;
            aggregate_state->ss.ps.outeropsfixed = true;
        }

        phase->evaltrans_cache[slot_cache_index][null_check_cache_index] =
            ExecBuildAggTrans(aggregate_state, phase, process_sorted, process_hash, check_null);

        /* Restore the child plan's slot settings. */
        aggregate_state->ss.ps.outerops = outer_slot_ops;
        aggregate_state->ss.ps.outeropsfixed = outer_ops_fixed;
    }

    phase->evaltrans = phase->evaltrans_cache[slot_cache_index][null_check_cache_index];
}

/** Choose a power-of-two spill partition count and optionally return its hash-bit count. */
static int choose_hash_partition_count(double input_groups,
                                       double hash_entry_size,
                                       int used_bits,
                                       int* partition_bits_out) {
    const Size hash_memory_limit = get_hash_memory_limit();
    double partition_limit;
    double desired_memory;
    double estimated_partition_count;
    int partition_count;
    int partition_bits;

    /* Limit spill buffers to about a quarter of the  hash memory budget. */
    partition_limit = (hash_memory_limit * 0.25 - hash_aggregate_read_buffer_bytes) /
                      hash_aggregate_write_buffer_bytes;

    desired_memory = hash_aggregate_partition_factor * input_groups * hash_entry_size;

    /* make enough partitions so that each one is likely to fit in memory */
    estimated_partition_count = 1 + (desired_memory / hash_memory_limit);

    if (estimated_partition_count > partition_limit)
        estimated_partition_count = partition_limit;

    if (estimated_partition_count < hash_aggregate_min_partitions)
        estimated_partition_count = hash_aggregate_min_partitions;
    if (estimated_partition_count > hash_aggregate_max_partitions)
        estimated_partition_count = hash_aggregate_max_partitions;

    /* The partition cap makes this cast safe. */
    partition_count = static_cast<int>(estimated_partition_count);

    /* Round the partition count up to a power of two. */
#if PG_VERSION_NUM >= 190000
    // PG19 uses pg_ceil_log2_32 for the partition count. The same rounding
    // applies at power-of-two boundaries.
    partition_bits = pg_ceil_log2_32(static_cast<std::uint32_t>(partition_count));
#else
    partition_bits = my_log2(partition_count);
#endif

    /* Use only hash bits that earlier spill passes have not consumed. */
    if (partition_bits + used_bits >= 32)
        partition_bits = 32 - used_bits;

    if (partition_bits_out != nullptr)
        *partition_bits_out = partition_bits;

    /* number of partitions will be a power of two */
    partition_count = 1 << partition_bits;

    return partition_count;
}

/** Set the memoy and group limits that trigger spilling, allowing space for spill buffers. */
extern "C" void hash_agg_set_limits(double hash_entry_size,
                                    double input_groups,
                                    int used_bits,
                                    Size* mem_limit,
                                    uint64* ngroups_limit,
                                    int* num_partitions) {
    int partition_count;
    Size partition_buffer_memory;
    const Size hash_memory_limit = get_hash_memory_limit();

    /* Use the full budget when the estimated groups fit in memory. */
    if (input_groups * hash_entry_size <= hash_memory_limit) {
        if (num_partitions != nullptr)
            *num_partitions = 0;
        *mem_limit = hash_memory_limit;
        *ngroups_limit = hash_memory_limit / hash_entry_size;
        return;
    }

    /* Budget for the spill tapes before assigning memory to hash groups. */
    partition_count =
        choose_hash_partition_count(input_groups, hash_entry_size, used_bits, nullptr);
    if (num_partitions != nullptr)
        *num_partitions = partition_count;

    partition_buffer_memory =
        hash_aggregate_read_buffer_bytes + hash_aggregate_write_buffer_bytes * partition_count;

    /* Keep at least three quarters of the budget available for hash groups. */
    if (hash_memory_limit > 4 * partition_buffer_memory)
        *mem_limit = hash_memory_limit - partition_buffer_memory;
    else
        *mem_limit = hash_memory_limit * 0.75;

    if (*mem_limit > hash_entry_size)
        *ngroups_limit = *mem_limit / hash_entry_size;
    else
        *ngroups_limit = 1;
}

/** Create  spill partitions and their group-count estimators after spilling becomes necessary. */
export void initialize_hash_aggregate_spill(hash_aggregate_spill* spill,
                                            LogicalTapeSet* tape_set,
                                            int used_bits,
                                            double input_groups,
                                            double hash_entry_size) {
    int partition_count;
    int partition_bits;

    partition_count =
        choose_hash_partition_count(input_groups, hash_entry_size, used_bits, &partition_bits);

    spill->partitions = allocate_zeroed_array<LogicalTape*>(partition_count);
    spill->ntuples = allocate_zeroed_array<int64_t>(partition_count);
    spill->hll_card = allocate_zeroed_array<hyperLogLogState>(partition_count);

    for (int partition_index = 0; partition_index < partition_count; ++partition_index)
        spill->partitions[partition_index] = LogicalTapeCreate(tape_set);

    spill->shift = 32 - used_bits - partition_bits;
    spill->mask = (partition_count - 1) << spill->shift;
    spill->npartitions = partition_count;

    for (int partition_index = 0; partition_index < partition_count; ++partition_index)
        initHyperLogLog(&spill->hll_card[partition_index], hash_aggregate_hll_bit_width);
}

/**
 * Stop creating hash groups and spill rows for new groups to process later
 * Existing in-memory groups can still accept rows. Initialize the spill tapes
 * on the first transition to this mode.
 */
static void enter_hash_aggregate_spill_mode(AggState* aggregate_state) {
    aggregate_state->hash_spill_mode = true;
    recompile_hash_aggregate_expressions(aggregate_state, aggregate_state->table_filled, true);

    if (!aggregate_state->hash_ever_spilled) {
        Assert(aggregate_state->hash_tapeset == nullptr);
        Assert(aggregate_state->hash_spills == nullptr);

        aggregate_state->hash_ever_spilled = true;

        aggregate_state->hash_tapeset = LogicalTapeSetCreate(true, nullptr, -1);

        aggregate_state->hash_spills =
            allocate_zeroed_array<hash_aggregate_spill>(aggregate_state->num_hashes);

        for (int set_index = 0; set_index < aggregate_state->num_hashes; set_index++) {
            AggStatePerHash hash_state = &aggregate_state->perhash[set_index];
            hash_aggregate_spill* spill = &aggregate_state->hash_spills[set_index];

            initialize_hash_aggregate_spill(spill,
                                            aggregate_state->hash_tapeset,
                                            0,
                                            hash_state->aggnode->numGroups,
                                            aggregate_state->hashentrysize);
        }
    }
}

/**
 * Enters spill mode when a newly added group crosses a hash limit.
 */
static void check_hash_aggregate_limits(AggState* aggregate_state) {
    const uint64_t group_count = aggregate_state->hash_ngroups_current;
    const Size metadata_memory = MemoryContextMemAllocated(aggregate_state->hash_metacxt, true);
    const Size hash_key_memory =
        MemoryContextMemAllocated(aggregate_state->hashcontext->ecxt_per_tuple_memory, true);

    /* Keep at least one group in memory so every batch makes progress. */
    if (aggregate_state->hash_ngroups_current > 0 &&
        (metadata_memory + hash_key_memory > aggregate_state->hash_mem_limit ||
         group_count > aggregate_state->hash_ngroups_limit)) {
        enter_hash_aggregate_spill_mode(aggregate_state);
    }
}

/** Record peak memory and disk use after a hash batch, then update the estimated group size.
 * Set reading_from_tape when  this batch came from a previous spill. */
export void update_hash_aggregate_metrics(AggState* aggregate_state,
                                          bool reading_from_tape,
                                          int partition_count) {
    Size buffer_memory;
    Size total_memory;

    if (aggregate_state->aggstrategy != AGG_MIXED && aggregate_state->aggstrategy != AGG_HASHED)
        return;

    /* memory for the hash table itself */
    const Size metadata_memory = MemoryContextMemAllocated(aggregate_state->hash_metacxt, true);

    /* memory for the group keys and transition states */
    const Size hash_key_memory =
        MemoryContextMemAllocated(aggregate_state->hashcontext->ecxt_per_tuple_memory, true);

    /* memory for read/write tape buffers, if spilled */
    buffer_memory = partition_count * hash_aggregate_write_buffer_bytes;
    if (reading_from_tape)
        buffer_memory += hash_aggregate_read_buffer_bytes;

    /* Record the largest memory use observed so far. */
    total_memory = metadata_memory + hash_key_memory + buffer_memory;
    if (total_memory > aggregate_state->hash_mem_peak)
        aggregate_state->hash_mem_peak = total_memory;

    /* Record peak, temporary-file use in kilobytes. */
    if (aggregate_state->hash_tapeset != nullptr) {
        uint64_t disk_used = LogicalTapeSetBlocks(aggregate_state->hash_tapeset) * (BLCKSZ / 1024);

        if (aggregate_state->hash_disk_used < disk_used)
            aggregate_state->hash_disk_used = disk_used;
    }

    /* Estimate later batch sizes from the memory used by these groups. */
    if (aggregate_state->hash_ngroups_current > 0) {
        aggregate_state->hashentrysize =
            sizeof(TupleHashEntryData) +
            (hash_key_memory / static_cast<double>(aggregate_state->hash_ngroups_current));
    }
}

/** Count a new hash group, check spill limits, and initialize its transition states. */
export void
initialize_hash_entry(AggState* aggregate_state, TupleHashTable hash_table, TupleHashEntry entry) {
    AggStatePerGroup group_state;
    int transition_index;

    aggregate_state->hash_ngroups_current++;
    check_hash_aggregate_limits(aggregate_state);

    /* A group with no transitions needs no addiional state. */
    if (aggregate_state->numtrans == 0)
        return;

    group_state = static_cast<AggStatePerGroup>(bioseq_prepare_tuple_hash_entry_additional(
        hash_table, entry, sizeof(AggStatePerGroupData) * aggregate_state->numtrans));

    /* Initialize each transition using the grouping set selected by the caller. */
    for (transition_index = 0; transition_index < aggregate_state->numtrans; transition_index++) {
        AggStatePerTrans transition_state = &aggregate_state->pertrans[transition_index];
        AggStatePerGroup per_group_state = &group_state[transition_index];

        initialize_aggregate(aggregate_state, transition_state, per_group_state);
    }
}

/** Write an input tuple and its hash value to the partition that will  process its group later. */
export Size spill_hash_aggregate_tuple(AggState* aggregate_state,
                                       hash_aggregate_spill* spill,
                                       TupleTableSlot* input_slot,
                                       uint32_t hash_value) {
    TupleTableSlot* spill_slot;
    int partition_index;
    MinimalTuple tuple;
    LogicalTape* tape;
    int written_byte_count = 0;
    bool should_free;

    Assert(spill->partitions != nullptr);

    /* Replace unused attributes with NULL before writing the spill tuple. */
    if (!aggregate_state->all_cols_needed) {
        spill_slot = aggregate_state->hash_spill_wslot;
        slot_getsomeattrs(input_slot, aggregate_state->max_colno_needed);
        ExecClearTuple(spill_slot);
        for (int attribute_index = 0; attribute_index < spill_slot->tts_tupleDescriptor->natts;
             ++attribute_index) {
            if (bms_is_member(attribute_index + 1, aggregate_state->colnos_needed)) {
                spill_slot->tts_values[attribute_index] = input_slot->tts_values[attribute_index];
                spill_slot->tts_isnull[attribute_index] = input_slot->tts_isnull[attribute_index];
            } else
                spill_slot->tts_isnull[attribute_index] = true;
        }
        ExecStoreVirtualTuple(spill_slot);
    } else
        spill_slot = input_slot;

    tuple = ExecFetchSlotMinimalTuple(spill_slot, &should_free);

    partition_index = (hash_value & spill->mask) >> spill->shift;
    spill->ntuples[partition_index]++;

    /* Mix the hash again before estimating distinct groups. Partition
     * selection has already fixed some bits of the original hash. */
    addHyperLogLog(&spill->hll_card[partition_index], hash_bytes_uint32(hash_value));

    tape = spill->partitions[partition_index];

    LogicalTapeWrite(tape, static_cast<const void*>(&hash_value), sizeof(uint32_t));
    written_byte_count += sizeof(uint32_t);

    LogicalTapeWrite(tape, static_cast<const void*>(tuple), tuple->t_len);
    written_byte_count += tuple->t_len;

    if (should_free)
        pfree(tuple);

    return written_byte_count;
}

/**
 * Find or create the current row's group in each hash table
 * Spill rows for groups that cannot be added, leaving their transition-state
 * pointer null. Hash lookup can reset the temporary expression context.
 */
export void lookup_hash_entries(AggState* aggregate_state) {
    AggStatePerGroup* group_state = aggregate_state->hash_pergroup;
    TupleTableSlot* outer_slot = aggregate_state->tmpcontext->ecxt_outertuple;

    for (int set_index = 0; set_index < aggregate_state->num_hashes; ++set_index) {
        AggStatePerHash hash_state = &aggregate_state->perhash[set_index];
        TupleHashTable hash_table = hash_state->hashtable;
        TupleTableSlot* hash_slot = hash_state->hashslot;
        TupleHashEntry entry;
        uint32_t hash_value;
        bool entry_is_new = false;

        /* Once spilling starts, lookup may  find existing groups but cannot add new ones. */
        bool* const entry_is_new_out = aggregate_state->hash_spill_mode ? nullptr : &entry_is_new;

        select_current_set(aggregate_state, set_index, true);
        prepare_hash_slot(hash_state, outer_slot, hash_slot);

        entry = LookupTupleHashEntry(hash_table, hash_slot, entry_is_new_out, &hash_value);

        if (entry != nullptr) {
            if (entry_is_new)
                initialize_hash_entry(aggregate_state, hash_table, entry);
            group_state[set_index] = static_cast<AggStatePerGroupData*>(
                bioseq_tuple_hash_entry_additional(hash_table, entry));
        } else {
            hash_aggregate_spill* spill = &aggregate_state->hash_spills[set_index];
            TupleTableSlot* spill_input_slot = aggregate_state->tmpcontext->ecxt_outertuple;

            if (spill->partitions == nullptr)
                initialize_hash_aggregate_spill(spill,
                                                aggregate_state->hash_tapeset,
                                                0,
                                                hash_state->aggnode->numGroups,
                                                aggregate_state->hashentrysize);

            spill_hash_aggregate_tuple(aggregate_state, spill, spill_input_slot, hash_value);
            group_state[set_index] = nullptr;
        }
    }
}

/** Describe one spilled partition to read during a later hash-aggregation pass. */
static hash_aggregate_batch* create_hash_aggregate_batch(LogicalTape* input_tape,
                                                         int set_index,
                                                         int64_t input_tuples,
                                                         double input_card,
                                                         int used_bits) {
    hash_aggregate_batch* batch = allocate_zeroed_object<hash_aggregate_batch>();

    batch->set_index = set_index;
    batch->used_bits = used_bits;
    batch->input_tape = input_tape;
    batch->input_tuples = input_tuples;
    batch->input_card = input_card;

    return batch;
}

/** Read the next spilled tuple and its saved hash value, or return nullptr at end of tape. */
export MinimalTuple read_hash_aggregate_batch(hash_aggregate_batch* batch,
                                              uint32_t* hash_value_out) {
    LogicalTape* tape = batch->input_tape;
    MinimalTuple tuple;
    uint32_t tuple_length;
    size_t read_byte_count;
    uint32_t hash_value;

    read_byte_count = LogicalTapeRead(tape, &hash_value, sizeof(uint32_t));
    if (read_byte_count == 0)
        return nullptr;
    if (read_byte_count != sizeof(uint32_t))
        bioseq_report<logging_level::error, logging_target::postgres>()
            .with_sql_error_code(errcode_for_file_access())
            .with_message("unexpected EOF for tape {}: requested {} bytes, read {} bytes",
                          static_cast<const void*>(tape),
                          sizeof(uint32_t),
                          read_byte_count);
    if (hash_value_out != nullptr)
        *hash_value_out = hash_value;

    read_byte_count = LogicalTapeRead(tape, &tuple_length, sizeof(tuple_length));
    if (read_byte_count != sizeof(uint32_t))
        bioseq_report<logging_level::error, logging_target::postgres>()
            .with_sql_error_code(errcode_for_file_access())
            .with_message("unexpected EOF for tape {}: requested {} bytes, read {} bytes",
                          static_cast<const void*>(tape),
                          sizeof(uint32_t),
                          read_byte_count);

    tuple = allocate_zeroed_storage<MinimalTupleData>(tuple_length);
    tuple->t_len = tuple_length;

    read_byte_count =
        LogicalTapeRead(tape,
                        static_cast<void*>(reinterpret_cast<char*>(tuple) + sizeof(uint32_t)),
                        tuple_length - sizeof(uint32_t));
    if (read_byte_count != tuple_length - sizeof(uint32_t))
        bioseq_report<logging_level::error, logging_target::postgres>()
            .with_sql_error_code(errcode_for_file_access())
            .with_message("unexpected EOF for tape {}: requested {} bytes, read {} bytes",
                          static_cast<const void*>(tape),
                          tuple_length - sizeof(uint32_t),
                          read_byte_count);

    return tuple;
}

/** Queue nonempty spill partitions as future hash batches. */
export void
finish_hash_aggregate_spill(AggState* aggregate_state, hash_aggregate_spill* spill, int set_index) {
    const int used_bits = 32 - spill->shift;

    if (spill->npartitions == 0)
        return; /* didn't spill */

    for (int partition_index = 0; partition_index < spill->npartitions; ++partition_index) {
        LogicalTape* tape = spill->partitions[partition_index];
        hash_aggregate_batch* queued_batch;
        double cardinality;

        /* if the partition is empty, don't create a new batch of work */
        if (spill->ntuples[partition_index] == 0)
            continue;

        cardinality = estimateHyperLogLog(&spill->hll_card[partition_index]);
        freeHyperLogLog(&spill->hll_card[partition_index]);

        /* Prepare this partition's tape for the later read pass. */
        LogicalTapeRewindForRead(tape, hash_aggregate_read_buffer_bytes);

        queued_batch = create_hash_aggregate_batch(
            tape, set_index, spill->ntuples[partition_index], cardinality, used_bits);
        aggregate_state->hash_batches = lappend(aggregate_state->hash_batches, queued_batch);
        aggregate_state->hash_batches_used++;
    }

    pfree(spill->ntuples);
    pfree(spill->hll_card);
    pfree(spill->partitions);
}

/** Queue the initil spill partitions and record their resource use. */
export void finish_initial_hash_aggregate_spills(AggState* aggregate_state) {
    int total_partition_count = 0;

    if (aggregate_state->hash_spills != nullptr) {
        for (int set_index = 0; set_index < aggregate_state->num_hashes; ++set_index) {
            hash_aggregate_spill* spill = &aggregate_state->hash_spills[set_index];

            total_partition_count += spill->npartitions;
            finish_hash_aggregate_spill(aggregate_state, spill, set_index);
        }

        /* Queued batches now hold the information needed for later passes. */
        pfree(aggregate_state->hash_spills);
        aggregate_state->hash_spills = nullptr;
    }

    update_hash_aggregate_metrics(aggregate_state, false, total_partition_count);
    aggregate_state->hash_spill_mode = false;
}

/** Clear queued spill work and close its temporary tapes. */
export void reset_hash_aggregate_spill_state(AggState* aggregate_state) {
    /* Release any remaining records from the initial spill  pass. */
    if (aggregate_state->hash_spills != nullptr) {
        for (int set_index = 0; set_index < aggregate_state->num_hashes; ++set_index) {
            hash_aggregate_spill* spill = &aggregate_state->hash_spills[set_index];

            pfree(spill->ntuples);
            pfree(spill->partitions);
        }
        pfree(aggregate_state->hash_spills);
        aggregate_state->hash_spills = nullptr;
    }

    /* Free queued batch descriptions. */
    list_free_deep(aggregate_state->hash_batches);
    aggregate_state->hash_batches = NIL;

    /* close tape set */
    if (aggregate_state->hash_tapeset != nullptr) {
        LogicalTapeSetClose(aggregate_state->hash_tapeset);
        aggregate_state->hash_tapeset = nullptr;
    }
}
