/**
 * @file
 * @brief Prepare aggregate execution state and clean it up or reset it for another scan.
 */
module;
#include <bioseq/pg_compat.h>
extern "C" {
#include "nodes/extensible.h"
}
#include <algorithm>
#include <array>

export module bioseq.postgres.vectorization.aggregator_node:initialization;

import bioseq.common.logging;
import bioseq.postgres.support;
import bioseq.postgres.vectorization.types;
import :support;
import :transitions;
import :hash_runtime;
import :execution;

using bioseq::postgres::support::allocate_zeroed_array;
using bioseq::postgres::support::allocate_zeroed_storage;
using bioseq::postgres::support::pg_node_view;
using bioseq::postgres::support::postgres_list;

/** Converts a catalog text initializer to the aggregate transition type. */
static Datum get_aggregate_initial_value(Datum text_initial_value, Oid transition_type) {
    Oid type_input_function;
    Oid type_io_parameter;
    char* initial_value_text;
    Datum initial_value;

    getTypeInputInfo(transition_type, &type_input_function, &type_io_parameter);
    initial_value_text = TextDatumGetCString(text_initial_value);
    initial_value =
        OidInputFunctionCall(type_input_function, initial_value_text, type_io_parameter, -1);
    pfree(initial_value_text);
    return initial_value;
}

/** Prepare the functions, initial value, and optional input sorting for one aggregate transition. */
static void build_aggregate_transition_state(AggStatePerTrans transition_state,
                                             AggState* aggregate_state,
                                             EState* executor_state,
                                             Aggref* aggregate_reference,
                                             Oid transfn_oid,
                                             Oid aggregate_transition_type,
                                             Oid aggregate_serialize_function_oid,
                                             Oid aggregate_deserialize_function_oid,
                                             Datum initial_value,
                                             bool initial_value_is_null,
                                             Oid* input_types,
                                             int argument_count) {
    const int grouping_set_count = std::max(aggregate_state->maxsets, 1);
    Expr* transition_function_expression;
    int transition_argument_count;
    Expr* serialize_function_expression = nullptr;
    Expr* deserialize_function_expression = nullptr;
    int input_count;
    int direct_argument_count;
    List* sort_list;
    int sort_column_count;
    int distinct_column_count;

    /* Store the aggregate definition and initial transition value. */
    transition_state->aggref = aggregate_reference;
    transition_state->aggshared = false;
    transition_state->aggCollation = aggregate_reference->inputcollid;
    transition_state->transfn_oid = transfn_oid;
    transition_state->serialfn_oid = aggregate_serialize_function_oid;
    transition_state->deserialfn_oid = aggregate_deserialize_function_oid;
    transition_state->initValue = initial_value;
    transition_state->initValueIsNull = initial_value_is_null;

    /* Direct arguments are evaluated once per group. */
    direct_argument_count = list_length(aggregate_reference->aggdirectargs);

    /* Count the expressions evaluated for aggregate input rows. */
    transition_state->numInputs = input_count = list_length(aggregate_reference->args);

    transition_state->aggtranstype = aggregate_transition_type;

    /* The transition function also receives its current state. */
    transition_argument_count = transition_state->numTransInputs + 1;

    /* Prepare calls to the transition function. This executor does not
     * use an inverse transition function. */
    build_aggregate_transfn_expr(input_types,
                                 argument_count,
                                 direct_argument_count,
                                 aggregate_reference->aggvariadic,
                                 aggregate_transition_type,
                                 aggregate_reference->inputcollid,
                                 transfn_oid,
                                 InvalidOid,
                                 &transition_function_expression,
                                 nullptr);

    fmgr_info(transfn_oid, &transition_state->transfn);
    fmgr_info_set_expr(reinterpret_cast<Node*>(transition_function_expression),
                       &transition_state->transfn);

    transition_state->transfn_fcinfo = allocate_zeroed_storage<FunctionCallInfoBaseData>(
        SizeForFunctionCallInfo(transition_argument_count));
    InitFunctionCallInfoData(*transition_state->transfn_fcinfo,
                             &transition_state->transfn,
                             transition_argument_count,
                             transition_state->aggCollation,
                             reinterpret_cast<Node*>(aggregate_state),
                             nullptr);

    /* Record how transition values must be stored and copied. */
    get_typlenbyval(aggregate_transition_type,
                    &transition_state->transtypeLen,
                    &transition_state->transtypeByVal);

    if (OidIsValid(aggregate_serialize_function_oid)) {
        build_aggregate_serialfn_expr(aggregate_serialize_function_oid,
                                      &serialize_function_expression);
        fmgr_info(aggregate_serialize_function_oid, &transition_state->serialfn);
        fmgr_info_set_expr(reinterpret_cast<Node*>(serialize_function_expression),
                           &transition_state->serialfn);

        transition_state->serialfn_fcinfo =
            allocate_zeroed_storage<FunctionCallInfoBaseData>(SizeForFunctionCallInfo(1));
        InitFunctionCallInfoData(*transition_state->serialfn_fcinfo,
                                 &transition_state->serialfn,
                                 1,
                                 InvalidOid,
                                 reinterpret_cast<Node*>(aggregate_state),
                                 nullptr);
    }

    if (OidIsValid(aggregate_deserialize_function_oid)) {
        build_aggregate_deserialfn_expr(aggregate_deserialize_function_oid,
                                        &deserialize_function_expression);
        fmgr_info(aggregate_deserialize_function_oid, &transition_state->deserialfn);
        fmgr_info_set_expr(reinterpret_cast<Node*>(deserialize_function_expression),
                           &transition_state->deserialfn);

        transition_state->deserialfn_fcinfo =
            allocate_zeroed_storage<FunctionCallInfoBaseData>(SizeForFunctionCallInfo(2));
        InitFunctionCallInfoData(*transition_state->deserialfn_fcinfo,
                                 &transition_state->deserialfn,
                                 2,
                                 InvalidOid,
                                 reinterpret_cast<Node*>(aggregate_state),
                                 nullptr);
    }

    /* Prepare input sorting for DISTINCT or ORDER BY. Ordered-set aggregates
     * perform their own ordering inside their support functions. */
    if (AGGKIND_IS_ORDERED_SET(aggregate_reference->aggkind)) {
        sort_list = NIL;
        sort_column_count = distinct_column_count = 0;
    } else if (aggregate_reference->aggdistinct) {
        sort_list = aggregate_reference->aggdistinct;
        sort_column_count = distinct_column_count = list_length(sort_list);
        Assert(sort_column_count >= list_length(aggregate_reference->aggorder));
    } else {
        sort_list = aggregate_reference->aggorder;
        sort_column_count = list_length(sort_list);
        distinct_column_count = 0;
    }

    transition_state->numSortCols = sort_column_count;
    transition_state->numDistinctCols = distinct_column_count;

    /* Allocate an input slot when sorting or FILTER needs evaluated arguments. */
    if (sort_column_count > 0 || aggregate_reference->aggfilter) {
        transition_state->sortdesc = ExecTypeFromTL(aggregate_reference->args);
        transition_state->sortslot =
            ExecInitExtraTupleSlot(executor_state, transition_state->sortdesc, &TTSOpsMinimalTuple);
    }

    if (sort_column_count > 0) {
        /* Hash aggregation does not support DISTINCT or ORDER BY inputs here. */
        Assert(aggregate_state->aggstrategy != AGG_HASHED &&
               aggregate_state->aggstrategy != AGG_MIXED);

        /* Sorted aggregate inputs cannot be used while combining partial states. */
        Assert(!DO_AGGSPLIT_COMBINE(aggregate_state->aggsplit));

        /* A single sorted value needs its type's copy and storage rules. */
        if (input_count == 1) {
            get_typlenbyval(input_types[direct_argument_count],
                            &transition_state->inputtypeLen,
                            &transition_state->inputtypeByVal);
        } else if (distinct_column_count > 0) {
            /* Keep the previous row when DISTINCT compares several columns. */
            transition_state->uniqslot = ExecInitExtraTupleSlot(
                executor_state, transition_state->sortdesc, &TTSOpsMinimalTuple);
        }

        /* Save the columns, operators, and null ordering used by the sort. */
        transition_state->sortColIdx = allocate_zeroed_array<AttrNumber>(sort_column_count);
        transition_state->sortOperators = allocate_zeroed_array<Oid>(sort_column_count);
        transition_state->sortCollations = allocate_zeroed_array<Oid>(sort_column_count);
        transition_state->sortNullsFirst = allocate_zeroed_array<bool>(sort_column_count);

        int sort_column_index = 0;
        for (SortGroupClause* sort_clause : postgres_list<SortGroupClause>{sort_list}) {
            TargetEntry* target_entry =
                get_sortgroupclause_tle(sort_clause, aggregate_reference->args);

            /* The parser must have supplied a sort operator. */
            Assert(OidIsValid(sort_clause->sortop));

            transition_state->sortColIdx[sort_column_index] = target_entry->resno;
            transition_state->sortOperators[sort_column_index] = sort_clause->sortop;
            transition_state->sortCollations[sort_column_index] =
                exprCollation(reinterpret_cast<Node*>(target_entry->expr));
            transition_state->sortNullsFirst[sort_column_index] = sort_clause->nulls_first;
            ++sort_column_index;
        }
        Assert(sort_column_index == sort_column_count);
    }

    if (aggregate_reference->aggdistinct) {
        Assert(argument_count > 0);
        Assert(list_length(aggregate_reference->aggdistinct) == distinct_column_count);

        Oid* equality_operators = allocate_zeroed_array<Oid>(distinct_column_count);

        int distinct_column_index = 0;
        for (SortGroupClause* group_clause :
             postgres_list<SortGroupClause>{aggregate_reference->aggdistinct}) {
            equality_operators[distinct_column_index++] = group_clause->eqop;
        }

        /* Prepare equality checks used to remove duplicate aggregate inputs. */
        if (distinct_column_count == 1)
            fmgr_info(get_opcode(equality_operators[0]), &transition_state->equalfnOne);
        else
            transition_state->equalfnMulti =
                execTuplesMatchPrepare(transition_state->sortdesc,
                                       distinct_column_count,
                                       transition_state->sortColIdx,
                                       equality_operators,
                                       transition_state->sortCollations,
                                       &aggregate_state->ss.ps);
        pfree(equality_operators);
    }

    transition_state->sortstates = allocate_zeroed_array<Tuplesortstate*>(grouping_set_count);
}

/**
 * Builds the PostgreSQL AggState used by the vector custom scan.
 */
export AggState*
initialize_vector_aggregate_executor(Agg* node, EState* executor_state, int executor_flags) {
    AggState* aggregate_state;
    AggStatePerAgg aggregate_descriptors;
    AggStatePerTrans transition_state_storage;
    AggStatePerGroup* group_state_sets;
    Plan* child_plan;
    ExprContext* expression_context;
    TupleDesc scan_descriptor;
    int max_aggno;
    int max_transno;
    int aggregate_reference_count;
    int aggregate_count;
    int transition_state_count;
    int phase;
    Bitmapset* all_grouped_cols = nullptr;
    int grouping_set_count = 1;
    int phase_count;
    int hash_table_count;
    const bool use_hashing = node->aggstrategy == AGG_HASHED || node->aggstrategy == AGG_MIXED;

    /* Backward scans and mark/restore are unsupported. */
    Assert(!(executor_flags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

    /* Create the executor state and start with no active groups. */
    aggregate_state = makeNode(AggState);
    aggregate_state->ss.ps.plan = reinterpret_cast<Plan*>(node);
    aggregate_state->ss.ps.state = executor_state;
    aggregate_state->ss.ps.ExecProcNode = execute_aggregate_plan;

    aggregate_state->aggs = NIL;
    aggregate_state->numaggs = 0;
    aggregate_state->numtrans = 0;
    aggregate_state->aggstrategy = node->aggstrategy;
    aggregate_state->aggsplit = node->aggsplit;
    aggregate_state->maxsets = 0;
    aggregate_state->projected_set = -1;
    aggregate_state->current_set = 0;
    aggregate_state->peragg = nullptr;
    aggregate_state->pertrans = nullptr;
    aggregate_state->curperagg = nullptr;
    aggregate_state->curpertrans = nullptr;
    aggregate_state->input_done = false;
    aggregate_state->agg_done = false;
    aggregate_state->pergroups = nullptr;
    aggregate_state->grp_firstTuple = nullptr;
    aggregate_state->sort_in = nullptr;
    aggregate_state->sort_out = nullptr;

    /* Reserve phase zero for hashing. Plain and sorted plans start at phase one. */
    phase_count = (use_hashing ? 1 : 2);
    hash_table_count = (use_hashing ? 1 : 0);

    /* Size shared grouping-set storage for the largest phase and count execution phases. */
    if (node->groupingSets) {
        grouping_set_count = list_length(node->groupingSets);

        for (Agg* chained_aggregate : postgres_list<Agg>{node->chain}) {
            grouping_set_count =
                std::max(grouping_set_count, list_length(chained_aggregate->groupingSets));

            /* Hash aggregates share phase zero. Each other chained aggregate
             * needs another input phase. */
            if (chained_aggregate->aggstrategy != AGG_HASHED)
                ++phase_count;
            else
                ++hash_table_count;
        }
    }

    aggregate_state->maxsets = grouping_set_count;
    aggregate_state->numphases = phase_count;

    aggregate_state->aggcontexts =
        reinterpret_cast<ExprContext**>(allocate_zeroed_array<ExprContext*>(grouping_set_count));

    /* Keep temporary input work separate from group state and output expressions.
     * Each group context runs its cleanup callbacks when that group finishes.
     * Hash aggregation has its own context for groups kept in memory. */
    ExecAssignExprContext(executor_state, &aggregate_state->ss.ps);
    aggregate_state->tmpcontext = aggregate_state->ss.ps.ps_ExprContext;

    for (int grouping_context_index = 0; grouping_context_index < grouping_set_count;
         ++grouping_context_index) {
        ExecAssignExprContext(executor_state, &aggregate_state->ss.ps);
        aggregate_state->aggcontexts[grouping_context_index] =
            aggregate_state->ss.ps.ps_ExprContext;
    }

    if (use_hashing)
        aggregate_state->hashcontext = CreateWorkExprContext(executor_state);

    ExecAssignExprContext(executor_state, &aggregate_state->ss.ps);

    /* A hash-only scan can reuse its groups, so the child needs no rewind support. */
    if (node->aggstrategy == AGG_HASHED)
        executor_flags &= ~EXEC_FLAG_REWIND;
    child_plan = outerPlan(node);
    outerPlanState(aggregate_state) = ExecInitNode(child_plan, executor_state, executor_flags);

    /* Match the scan slot to the child plan's tuple representation. */
    aggregate_state->ss.ps.outerops = ExecGetResultSlotOps(outerPlanState(&aggregate_state->ss),
                                                           &aggregate_state->ss.ps.outeropsfixed);
    aggregate_state->ss.ps.outeropsset = true;

    ExecCreateScanSlotFromOuterPlan(
        executor_state, &aggregate_state->ss, aggregate_state->ss.ps.outerops);
    scan_descriptor = aggregate_state->ss.ss_ScanTupleSlot->tts_tupleDescriptor;

    /* Extra input phases read rows from tuplesort and need a MinimalTuple slot. */
    if (phase_count > 2) {
        aggregate_state->sort_slot =
            ExecInitExtraTupleSlot(executor_state, scan_descriptor, &TTSOpsMinimalTuple);

        /* Input may switch from the child's slot type to MinimalTuple after sorting. */
        if (aggregate_state->ss.ps.outeropsfixed &&
            aggregate_state->ss.ps.outerops != &TTSOpsMinimalTuple)
            aggregate_state->ss.ps.outeropsfixed = false;
    }

    /**
     * Initialize result type, slot and projection.
     */
    ExecInitResultTupleSlotTL(&aggregate_state->ss.ps, &TTSOpsVirtual);
    ExecAssignProjectionInfo(&aggregate_state->ss.ps, nullptr);

    /* Compile HAVING expressions and collect their aggregate references. */
    aggregate_state->ss.ps.qual =
        ExecInitQual(node->plan.qual, reinterpret_cast<PlanState*>(aggregate_state));

    /* Projection and HAVING initialization have now collected the aggregate calls. */
    aggregate_reference_count = list_length(aggregate_state->aggs);
    max_aggno = -1;
    max_transno = -1;
    for (auto aggregate_reference : postgres_list<Aggref>{aggregate_state->aggs}) {
        max_aggno = std::max(max_aggno, aggregate_reference->aggno);
        max_transno = std::max(max_transno, aggregate_reference->aggtransno);
    }
    aggregate_count = max_aggno + 1;
    transition_state_count = max_transno + 1;

    /* Prepare each phase's grouping columns and equality checks. */
    aggregate_state->phases = allocate_zeroed_array<AggStatePerPhaseData>(phase_count);

    aggregate_state->num_hashes = hash_table_count;
    if (hash_table_count) {
        aggregate_state->perhash = allocate_zeroed_array<AggStatePerHashData>(hash_table_count);
        aggregate_state->phases[0].numsets = 0;
        aggregate_state->phases[0].gset_lengths = allocate_zeroed_array<int>(hash_table_count);
        aggregate_state->phases[0].grouped_cols =
            allocate_zeroed_array<Bitmapset*>(hash_table_count);
    }

    phase = 0;
    for (int phase_index = 0; phase_index <= list_length(node->chain); ++phase_index) {
        Agg* aggregate_node;
        Sort* sort_node;

        if (phase_index > 0) {
            aggregate_node = list_nth_node(Agg, node->chain, phase_index - 1);
            sort_node = castNode(Sort, aggregate_node->plan.lefttree);
        } else {
            aggregate_node = node;
            sort_node = nullptr;
        }

        Assert(phase <= 1 || sort_node);

        if (aggregate_node->aggstrategy == AGG_HASHED || aggregate_node->aggstrategy == AGG_MIXED) {
            AggStatePerPhase phase_data = &aggregate_state->phases[0];
            AggStatePerHash hash_state;
            Bitmapset* grouped_columns = nullptr;

            Assert(phase == 0);
            const int hash_set_index = phase_data->numsets++;
            hash_state = &aggregate_state->perhash[hash_set_index];

            /* Hash phase zero uses the top-level aggregate plan. */
            phase_data->aggnode = node;
            phase_data->aggstrategy = node->aggstrategy;

            /* Keep this grouping set's aggregate plan with its hash table. */
            hash_state->aggnode = aggregate_node;

            phase_data->gset_lengths[hash_set_index] = hash_state->numCols =
                aggregate_node->numCols;

            for (int group_column_index = 0; group_column_index < aggregate_node->numCols;
                 ++group_column_index)
                grouped_columns =
                    bms_add_member(grouped_columns, aggregate_node->grpColIdx[group_column_index]);

            phase_data->grouped_cols[hash_set_index] = grouped_columns;

            all_grouped_cols = bms_add_members(all_grouped_cols, grouped_columns);
            continue;
        } else {
            AggStatePerPhase phase_data = &aggregate_state->phases[++phase];
            const int phase_grouping_set_count = list_length(aggregate_node->groupingSets);

            phase_data->numsets = phase_grouping_set_count;

            if (phase_grouping_set_count > 0) {
                phase_data->gset_lengths = allocate_zeroed_array<int>(phase_grouping_set_count);
                phase_data->grouped_cols =
                    allocate_zeroed_array<Bitmapset*>(phase_grouping_set_count);

                int grouping_set_index = 0;
                for (List* grouping_set : postgres_list<List>{aggregate_node->groupingSets}) {
                    const int grouping_column_count = list_length(grouping_set);
                    Bitmapset* grouped_columns = nullptr;

                    /* The planner orders each grouping set as a prefix of grpColIdx. */
                    for (int group_column_index = 0; group_column_index < grouping_column_count;
                         ++group_column_index)
                        grouped_columns = bms_add_member(
                            grouped_columns, aggregate_node->grpColIdx[group_column_index]);

                    phase_data->grouped_cols[grouping_set_index] = grouped_columns;
                    phase_data->gset_lengths[grouping_set_index] = grouping_column_count;

                    ++grouping_set_index;
                }

                all_grouped_cols = bms_add_members(all_grouped_cols, phase_data->grouped_cols[0]);
            } else {
                Assert(phase_index == 0);

                phase_data->gset_lengths = nullptr;
                phase_data->grouped_cols = nullptr;
            }

            /* Prepare the equality checks used to find sorted group boundaries. */
            if (aggregate_node->aggstrategy == AGG_SORTED) {
                Assert(aggregate_node->numCols > 0);

                /* Each grouping prefix may need a different row comparison. */
                phase_data->eqfunctions =
                    allocate_zeroed_array<ExprState*>(aggregate_node->numCols);

                /* for each grouping set */
                for (int grouping_set_index = 0; grouping_set_index < phase_data->numsets;
                     ++grouping_set_index) {
                    const int grouping_column_count = phase_data->gset_lengths[grouping_set_index];

                    /* An empty grouping set has no columns to compare. */
                    if (grouping_column_count == 0)
                        continue;

                    /* Grouping sets with the same prefix length can share this comparison. */
                    if (phase_data->eqfunctions[grouping_column_count - 1] != nullptr)
                        continue;

                    phase_data->eqfunctions[grouping_column_count - 1] =
                        execTuplesMatchPrepare(scan_descriptor,
                                               grouping_column_count,
                                               aggregate_node->grpColIdx,
                                               aggregate_node->grpOperators,
                                               aggregate_node->grpCollations,
                                               reinterpret_cast<PlanState*>(aggregate_state));
                }

                /* Also prepare the comparison across all grouping columns. */
                if (phase_data->eqfunctions[aggregate_node->numCols - 1] == nullptr) {
                    phase_data->eqfunctions[aggregate_node->numCols - 1] =
                        execTuplesMatchPrepare(scan_descriptor,
                                               aggregate_node->numCols,
                                               aggregate_node->grpColIdx,
                                               aggregate_node->grpOperators,
                                               aggregate_node->grpCollations,
                                               reinterpret_cast<PlanState*>(aggregate_state));
                }
            }

            phase_data->aggnode = aggregate_node;
            phase_data->aggstrategy = aggregate_node->aggstrategy;
            phase_data->sortnode = sort_node;
        }
    }

    /* Put the largest grouping column first so projection can fetch all needed attributes at once. */
    int grouped_column_number = -1;
    while ((grouped_column_number = bms_next_member(all_grouped_cols, grouped_column_number)) >= 0)
        aggregate_state->all_grouped_cols =
            lcons_int(grouped_column_number, aggregate_state->all_grouped_cols);

    /* Allocate result values and the reusable descriptors for aggregates and transitions. */
    expression_context = aggregate_state->ss.ps.ps_ExprContext;
    expression_context->ecxt_aggvalues = allocate_zeroed_array<Datum>(aggregate_count);
    expression_context->ecxt_aggnulls = allocate_zeroed_array<bool>(aggregate_count);

    aggregate_descriptors = allocate_zeroed_array<AggStatePerAggData>(aggregate_count);
    transition_state_storage = allocate_zeroed_array<AggStatePerTransData>(transition_state_count);

    aggregate_state->peragg = aggregate_descriptors;
    aggregate_state->pertrans = transition_state_storage;

    aggregate_state->all_pergroups =
        allocate_zeroed_array<AggStatePerGroup>(grouping_set_count + hash_table_count);
    group_state_sets = aggregate_state->all_pergroups;

    if (node->aggstrategy != AGG_HASHED) {
        for (int grouping_set_index = 0; grouping_set_index < grouping_set_count;
             ++grouping_set_index) {
            group_state_sets[grouping_set_index] =
                allocate_zeroed_array<AggStatePerGroupData>(aggregate_count);
        }

        aggregate_state->pergroups = group_state_sets;
        group_state_sets += grouping_set_count;
    }

    /* Set up hash tables and spill state for the hashing phase. */
    if (use_hashing) {
        uint64_t total_groups = 0;

        aggregate_state->hash_metacxt =
            AllocSetContextCreate(aggregate_state->ss.ps.state->es_query_cxt,
                                  "HashAgg meta context",
                                  ALLOCSET_DEFAULT_SIZES);
        aggregate_state->hash_spill_rslot =
            ExecInitExtraTupleSlot(executor_state, scan_descriptor, &TTSOpsMinimalTuple);
        aggregate_state->hash_spill_wslot =
            ExecInitExtraTupleSlot(executor_state, scan_descriptor, &TTSOpsVirtual);

        /* Store one pointer to the current transition states for each hash grouping set. */
        aggregate_state->hash_pergroup = group_state_sets;

        aggregate_state->hashentrysize = hash_agg_entry_size(
            aggregate_state->numtrans, child_plan->plan_width, node->transitionSpace);

        /* Share one spill budget across the estimated groups in all hash tables. */
        for (int hash_table_index = 0; hash_table_index < aggregate_state->num_hashes;
             ++hash_table_index)
            total_groups += aggregate_state->perhash[hash_table_index].aggnode->numGroups;

        hash_agg_set_limits(aggregate_state->hashentrysize,
                            total_groups,
                            0,
                            &aggregate_state->hash_mem_limit,
                            &aggregate_state->hash_ngroups_limit,
                            &aggregate_state->hash_planned_partitions);
        find_hash_columns(aggregate_state);

        /* EXPLAIN without execution needs metadata, so defer hash-table allocation. */
        if (!(executor_flags & EXEC_FLAG_EXPLAIN_ONLY))
            build_hash_tables(aggregate_state);

        aggregate_state->table_filled = false;

        /* Count the initial in-memory batch before any spill batches exist. */
        aggregate_state->hash_batches_used = 1;
    }

    /* Start hash-only plans in phase zero and other strategies in phase one. */
    if (node->aggstrategy == AGG_HASHED) {
        aggregate_state->current_phase = 0;
        initialize_phase(aggregate_state, 0);
        select_current_set(aggregate_state, 0, true);
    } else {
        aggregate_state->current_phase = 1;
        initialize_phase(aggregate_state, 1);
        select_current_set(aggregate_state, 0, false);
    }

    /* Resolve aggregate functions and fill the descriptors shared by all groups. */
    for (Aggref* aggregate_reference : postgres_list<Aggref>{aggregate_state->aggs}) {
        AggStatePerAgg aggregate_descriptor;
        AggStatePerTrans transition_state;
        std::array<Oid, FUNC_MAX_ARGS> aggregate_transition_input_types;
        int aggregate_transition_argument_count;
        int direct_argument_count;
        HeapTuple aggregate_tuple;
        Form_pg_aggregate aggregate_form;
        AclResult access_result;
        Oid finalfn_oid;
        Oid serialfn_oid, deserialfn_oid;
        Oid aggregate_owner;
        Expr* final_function_expression;
        Oid aggregate_transition_type;

        /* Planner should have assigned aggregate to correct level */
        Assert(aggregate_reference->agglevelsup == 0);
        /* The aggregate call must use the plan's partial-aggregation mode. */
        Assert(aggregate_reference->aggsplit == aggregate_state->aggsplit);

        aggregate_descriptor = &aggregate_descriptors[aggregate_reference->aggno];

        /* Check if we initialized the state for this aggregate already. */
        if (aggregate_descriptor->aggref != nullptr)
            continue;

        aggregate_descriptor->aggref = aggregate_reference;
        aggregate_descriptor->transno = aggregate_reference->aggtransno;

        /* Fetch the pg_aggregate row */
        aggregate_tuple =
            SearchSysCache1(AGGFNOID, ObjectIdGetDatum(aggregate_reference->aggfnoid));
        if (!HeapTupleIsValid(aggregate_tuple))
            bioseq_log<logging_level::error, logging_target::postgres>(
                "cache lookup failed for aggregate {}", aggregate_reference->aggfnoid);
        aggregate_form = reinterpret_cast<Form_pg_aggregate>(GETSTRUCT(aggregate_tuple));

        /* Check permission to call aggregate function */
        access_result = object_aclcheck(
            ProcedureRelationId, aggregate_reference->aggfnoid, GetUserId(), ACL_EXECUTE);
        if (access_result != ACLCHECK_OK)
            aclcheck_error(
                access_result, OBJECT_AGGREGATE, get_func_name(aggregate_reference->aggfnoid));
        InvokeFunctionExecuteHook(aggregate_reference->aggfnoid);

        /* Use the transition-state type resolved by the planner. */
        aggregate_transition_type = aggregate_reference->aggtranstype;
        Assert(OidIsValid(aggregate_transition_type));

        /* Final function only required if we're finalizing the aggregates */
        if (DO_AGGSPLIT_SKIPFINAL(aggregate_state->aggsplit))
            aggregate_descriptor->finalfn_oid = finalfn_oid = InvalidOid;
        else
            aggregate_descriptor->finalfn_oid = finalfn_oid = aggregate_form->aggfinalfn;

        serialfn_oid = InvalidOid;
        deserialfn_oid = InvalidOid;

        /* INTERNAL states need support functions when partial aggregation
         * requests serialization or deserialization. */
        if (aggregate_transition_type == INTERNALOID) {
            /* Find the serializer when this plan returns a serialized partial state. */
            if (DO_AGGSPLIT_SERIALIZE(aggregate_state->aggsplit)) {
                /* Serialization returns transition state, so no final function runs. */
                Assert(DO_AGGSPLIT_SKIPFINAL(aggregate_state->aggsplit));

                if (!OidIsValid(aggregate_form->aggserialfn))
                    bioseq_log<logging_level::error, logging_target::postgres>(
                        "serialfunc not provided for serialization aggregation");
                serialfn_oid = aggregate_form->aggserialfn;
            }

            /* Find the deserializer when incoming partial states need decoding. */
            if (DO_AGGSPLIT_DESERIALIZE(aggregate_state->aggsplit)) {
                /* deserialization only valid when combining states */
                Assert(DO_AGGSPLIT_COMBINE(aggregate_state->aggsplit));

                if (!OidIsValid(aggregate_form->aggdeserialfn))
                    bioseq_log<logging_level::error, logging_target::postgres>(
                        "deserialfunc not provided for deserialization aggregation");
                deserialfn_oid = aggregate_form->aggdeserialfn;
            }
        }

        /* Check the aggregate owner's permission to call its support functions. */
        {
            HeapTuple procedure_tuple;

            procedure_tuple =
                SearchSysCache1(PROCOID, ObjectIdGetDatum(aggregate_reference->aggfnoid));
            if (!HeapTupleIsValid(procedure_tuple))
                bioseq_log<logging_level::error, logging_target::postgres>(
                    "cache lookup failed for function {}", aggregate_reference->aggfnoid);
            aggregate_owner =
                (reinterpret_cast<Form_pg_proc>(GETSTRUCT(procedure_tuple)))->proowner;
            ReleaseSysCache(procedure_tuple);

            if (OidIsValid(finalfn_oid)) {
                access_result =
                    object_aclcheck(ProcedureRelationId, finalfn_oid, aggregate_owner, ACL_EXECUTE);
                if (access_result != ACLCHECK_OK)
                    aclcheck_error(access_result, OBJECT_FUNCTION, get_func_name(finalfn_oid));
                InvokeFunctionExecuteHook(finalfn_oid);
            }
            if (OidIsValid(serialfn_oid)) {
                access_result = object_aclcheck(
                    ProcedureRelationId, serialfn_oid, aggregate_owner, ACL_EXECUTE);
                if (access_result != ACLCHECK_OK)
                    aclcheck_error(access_result, OBJECT_FUNCTION, get_func_name(serialfn_oid));
                InvokeFunctionExecuteHook(serialfn_oid);
            }
            if (OidIsValid(deserialfn_oid)) {
                access_result = object_aclcheck(
                    ProcedureRelationId, deserialfn_oid, aggregate_owner, ACL_EXECUTE);
                if (access_result != ACLCHECK_OK)
                    aclcheck_error(access_result, OBJECT_FUNCTION, get_func_name(deserialfn_oid));
                InvokeFunctionExecuteHook(deserialfn_oid);
            }
        }

        /* Read the actual SQL argument types, including resolved polymorphic types. */
        aggregate_transition_argument_count =
            get_aggregate_argtypes(aggregate_reference, aggregate_transition_input_types.data());

        /* Count the arguments evaluated once per group. */
        direct_argument_count = list_length(aggregate_reference->aggdirectargs);

        /* Include the transition state and any extra arguments required by the final function. */
        if (aggregate_form->aggfinalextra)
            aggregate_descriptor->numFinalArgs = aggregate_transition_argument_count + 1;
        else
            aggregate_descriptor->numFinalArgs = direct_argument_count + 1;

        /* Initialize any direct-argument expressions */
        aggregate_descriptor->aggdirectargs = ExecInitExprList(
            aggregate_reference->aggdirectargs, reinterpret_cast<PlanState*>(aggregate_state));

        /* Prepare the final-function call with its resolved argument and result types. */
        if (OidIsValid(finalfn_oid)) {
            build_aggregate_finalfn_expr(aggregate_transition_input_types.data(),
                                         aggregate_descriptor->numFinalArgs,
                                         aggregate_transition_type,
                                         aggregate_reference->aggtype,
                                         aggregate_reference->inputcollid,
                                         finalfn_oid,
                                         &final_function_expression);
            fmgr_info(finalfn_oid, &aggregate_descriptor->finalfn);
            fmgr_info_set_expr(reinterpret_cast<Node*>(final_function_expression),
                               &aggregate_descriptor->finalfn);
        }

        /* Record how final results must be stored and copied. */
        get_typlenbyval(aggregate_reference->aggtype,
                        &aggregate_descriptor->resulttypeLen,
                        &aggregate_descriptor->resulttypeByVal);

        /* Several aggregates can share a transition. Initialize it once. */
        transition_state = &transition_state_storage[aggregate_reference->aggtransno];
        if (transition_state->aggref == nullptr) {
            Datum text_initial_value;
            Datum initial_value;
            bool initial_value_is_null;
            Oid transfn_oid;

            /* Partial-state aggregation calls the combine function to merge states. */
            if (DO_AGGSPLIT_COMBINE(aggregate_state->aggsplit)) {
                transfn_oid = aggregate_form->aggcombinefn;

                /* A combine plan requires a combine function in the aggregate definition. */
                if (!OidIsValid(transfn_oid))
                    bioseq_log<logging_level::error, logging_target::postgres>(
                        "combinefn not set for aggregate function");
            } else
                transfn_oid = aggregate_form->aggtransfn;

            access_result =
                object_aclcheck(ProcedureRelationId, transfn_oid, aggregate_owner, ACL_EXECUTE);
            if (access_result != ACLCHECK_OK)
                aclcheck_error(access_result, OBJECT_FUNCTION, get_func_name(transfn_oid));
            InvokeFunctionExecuteHook(transfn_oid);

            /* Read the initial value through SysCacheGetAttr because the catalog
             * column can be NULL. */
            text_initial_value = SysCacheGetAttr(
                AGGFNOID, aggregate_tuple, Anum_pg_aggregate_agginitval, &initial_value_is_null);
            if (initial_value_is_null)
                initial_value = Datum{0};
            else
                initial_value =
                    get_aggregate_initial_value(text_initial_value, aggregate_transition_type);

            if (DO_AGGSPLIT_COMBINE(aggregate_state->aggsplit)) {
                std::array<Oid, 2u> combine_input_types{aggregate_transition_type,
                                                        aggregate_transition_type};

                /* Combining adds one incoming state to the current state. */
                transition_state->numTransInputs = 1;

                /* Both combine-function arguments have the transition-state type. */
                build_aggregate_transition_state(transition_state,
                                                 aggregate_state,
                                                 executor_state,
                                                 aggregate_reference,
                                                 transfn_oid,
                                                 aggregate_transition_type,
                                                 serialfn_oid,
                                                 deserialfn_oid,
                                                 initial_value,
                                                 initial_value_is_null,
                                                 combine_input_types.data(),
                                                 2);

                /* An INTERNAL combine function must accept NULL states and cannot be strict. */
                if (transition_state->transfn.fn_strict && aggregate_transition_type == INTERNALOID)
                    bioseq_report<logging_level::error, logging_target::postgres>()
                        .with_sql_error_code(ERRCODE_INVALID_FUNCTION_DEFINITION)
                        .with_message(
                            "combine function with transition type {} must not be declared STRICT",
                            format_type_be(aggregate_transition_type));
            } else {
                /* Count row inputs passed to the transition function. */
                if (AGGKIND_IS_ORDERED_SET(aggregate_reference->aggkind))
                    transition_state->numTransInputs = list_length(aggregate_reference->args);
                else
                    transition_state->numTransInputs = aggregate_transition_argument_count;

                build_aggregate_transition_state(transition_state,
                                                 aggregate_state,
                                                 executor_state,
                                                 aggregate_reference,
                                                 transfn_oid,
                                                 aggregate_transition_type,
                                                 serialfn_oid,
                                                 deserialfn_oid,
                                                 initial_value,
                                                 initial_value_is_null,
                                                 aggregate_transition_input_types.data(),
                                                 aggregate_transition_argument_count);

                /* With a strict function and NULL initial state, the first non-NULL
                 * input must be usable directly as the transition value. */
                if (transition_state->transfn.fn_strict && transition_state->initValueIsNull) {
                    if (aggregate_transition_argument_count <= direct_argument_count ||
                        !IsBinaryCoercible(aggregate_transition_input_types[direct_argument_count],
                                           aggregate_transition_type))
                        bioseq_report<logging_level::error, logging_target::postgres>()
                            .with_sql_error_code(ERRCODE_INVALID_FUNCTION_DEFINITION)
                            .with_message("aggregate {} needs to have compatible input type and "
                                          "transition type",
                                          aggregate_reference->aggfnoid);
                }
            }
        } else
            transition_state->aggshared = true;
        ReleaseSysCache(aggregate_tuple);
    }

    /* Store the counts after repeated aggregate and transition references have been merged. */
    aggregate_state->numaggs = aggregate_count;
    aggregate_state->numtrans = transition_state_count;

    /**
     * Rejects nested aggregate references discovered while building arguments.
     */
    if (aggregate_reference_count != list_length(aggregate_state->aggs))
        bioseq_report<logging_level::error, logging_target::postgres>()
            .with_sql_error_code(ERRCODE_GROUPING_ERROR)
            .with_message("aggregate function calls cannot be nested");

    /* Compile the transition or combine expressions needed by each input phase. */
    for (int phase_index = 0; phase_index < aggregate_state->numphases; ++phase_index) {
        AggStatePerPhase phase_state = &aggregate_state->phases[phase_index];
        bool process_hash = false;
        bool process_sorted = false;

        /* Phase zero has no aggregate plan in plain or sorted execution. */
        if (!phase_state->aggnode)
            continue;

        if (aggregate_state->aggstrategy == AGG_MIXED && phase_index == 1) {
            /* The first mixed input phase updates both sorted and hash groups. */
            process_hash = true;
            process_sorted = true;
        } else if (aggregate_state->aggstrategy == AGG_MIXED && phase_index == 0) {
            /**
             * Mixed phase zero reads hash tables already populated by phase one.
             */
            continue;
        } else if (phase_state->aggstrategy == AGG_PLAIN ||
                   phase_state->aggstrategy == AGG_SORTED) {
            process_hash = false;
            process_sorted = true;
        } else if (phase_state->aggstrategy == AGG_HASHED) {
            process_hash = true;
            process_sorted = false;
        } else
            Assert(false);

        phase_state->evaltrans =
            ExecBuildAggTrans(aggregate_state, phase_state, process_sorted, process_hash, false);

        /* Cache the child-slot version with every grouping set active. */
        phase_state->evaltrans_cache[0][0] = phase_state->evaltrans;
    }

    return aggregate_state;
}

/** Releases the executor resources owned by a vector aggregate state. */
// NOLINTNEXTLINE(readability-identifier-naming) -- exact PostgreSQL executor ABI name
extern "C" void ExecEndAgg(AggState* node) {
    const int grouping_set_count = std::max(node->maxsets, 1);

    /* Copy worker statistics into shared storage for EXPLAIN ANALYZE. */
    if (node->shared_info && IsParallelWorker()) {
        AggregateInstrumentation* worker_instrumentation;

        Assert(ParallelWorkerNumber <= node->shared_info->num_workers);
        worker_instrumentation = &node->shared_info->sinstrument[ParallelWorkerNumber];
        worker_instrumentation->hash_batches_used = node->hash_batches_used;
        worker_instrumentation->hash_disk_used = node->hash_disk_used;
        worker_instrumentation->hash_mem_peak = node->hash_mem_peak;
    }

    /* Close sorting work still owned by this aggregate. */

    if (node->sort_in)
        tuplesort_end(node->sort_in);
    if (node->sort_out)
        tuplesort_end(node->sort_out);

    reset_hash_aggregate_spill_state(node);

    if (node->hash_metacxt != nullptr) {
        MemoryContextDelete(node->hash_metacxt);
        node->hash_metacxt = nullptr;
    }

    for (int transition_index = 0; transition_index < node->numtrans; ++transition_index) {
        AggStatePerTrans transition_state = &node->pertrans[transition_index];

        for (int set_index = 0; set_index < grouping_set_count; ++set_index) {
            if (transition_state->sortstates[set_index])
                tuplesort_end(transition_state->sortstates[set_index]);
        }
    }

    /* Run pending aggregate cleanup callbacks before ending the child plan. */
    for (int set_index = 0; set_index < grouping_set_count; ++set_index)
        ReScanExprContext(node->aggcontexts[set_index]);
    if (node->hashcontext)
        ReScanExprContext(node->hashcontext);

    /* Clear the representative input slot. */
    ExecClearTuple(node->ss.ss_ScanTupleSlot);

    PlanState* child_plan_state = outerPlanState(node);
    ExecEndNode(child_plan_state);
}

/** Prepare aggregation for a rescan, reusing an unchanged hash table when possible. */
// NOLINTNEXTLINE(readability-identifier-naming) -- exact PostgreSQL executor ABI name
extern "C" void ExecReScanAgg(AggState* node) {
    ExprContext* expression_context = node->ss.ps.ps_ExprContext;
    PlanState* child_plan_state = outerPlanState(node);
    Agg* aggregate_node = reinterpret_cast<Agg*>(node->ss.ps.plan);
    const int grouping_set_count = std::max(node->maxsets, 1);

    node->agg_done = false;

    if (node->aggstrategy == AGG_HASHED) {
        /* Input has not filled the hash table yet, so there is nothing to restart. */
        if (!node->table_filled)
            return;

        /**
         * Reuses an unspilled hash table when neither child nor aggregate inputs
         * have parameter changes.
         */
        if (child_plan_state->chgParam == nullptr && !node->hash_ever_spilled &&
            !bms_overlap(node->ss.ps.chgParam, aggregate_node->aggParams)) {
            ResetTupleHashIterator(node->perhash[0].hashtable, &node->perhash[0].hashiter);
            select_current_set(node, 0, true);
            return;
        }
    }

    /* Close each aggregate's pending input sort. */
    for (int transition_index = 0; transition_index < node->numtrans; ++transition_index) {
        for (int set_index = 0; set_index < grouping_set_count; ++set_index) {
            AggStatePerTrans transition_state = &node->pertrans[transition_index];

            if (transition_state->sortstates[set_index]) {
                tuplesort_end(transition_state->sortstates[set_index]);
                transition_state->sortstates[set_index] = nullptr;
            }
        }
    }

    /* Run grouping-state cleanup callbacks before rebuilding the groups. */

    for (int set_index = 0; set_index < grouping_set_count; ++set_index) {
        ReScanExprContext(node->aggcontexts[set_index]);
    }

    /* Release first tuple of group, if we have made a copy */
    if (node->grp_firstTuple != nullptr) {
        heap_freetuple(node->grp_firstTuple);
        node->grp_firstTuple = nullptr;
    }
    ExecClearTuple(node->ss.ss_ScanTupleSlot);

    /* Clear the previous aggregate results. */
    MemSet(expression_context->ecxt_aggvalues, 0, sizeof(Datum) * node->numaggs);
    MemSet(expression_context->ecxt_aggnulls, 0, sizeof(bool) * node->numaggs);

    /* Hash groups live below hashcontext. Reset it before rebuilding the
     * empty tables and restarting spill tracking. */
    if (node->aggstrategy == AGG_HASHED || node->aggstrategy == AGG_MIXED) {
        reset_hash_aggregate_spill_state(node);

        node->hash_ever_spilled = false;
        node->hash_spill_mode = false;
        node->hash_ngroups_current = 0;

        ReScanExprContext(node->hashcontext);
        /* Rebuild an empty hash table */
        build_hash_tables(node);
        node->table_filled = false;
        /* iterator will be reset when the table is filled */

        recompile_hash_aggregate_expressions(node, false, false);
    }

    if (node->aggstrategy != AGG_HASHED) {
        /* Clear the per-group records. Group initialization will restore the initial transition values. */
        for (int set_index = 0; set_index < grouping_set_count; ++set_index) {
            MemSet(node->pergroups[set_index], 0, sizeof(AggStatePerGroupData) * node->numaggs);
        }

        /* reset to phase 1 */
        initialize_phase(node, 1);

        node->input_done = false;
        node->projected_set = -1;
    }

    if (child_plan_state->chgParam == nullptr)
        ExecReScan(child_plan_state);
}

/* Helpers called by aggregate support functions. */

/** Identify an aggregate or window-aggregate call and optionally return its state memory context.
 * Return zero and a null context for other calls. */
// NOLINTNEXTLINE(readability-identifier-naming) -- exact PostgreSQL fmgr ABI name
extern "C" int AggCheckCallContext(FunctionCallInfo call_info, MemoryContext* aggregate_context) {
    if (AggState* aggregate_state = pg_node_view{call_info->context}.try_cast<AggState>()) {
        if (aggregate_context) {
            ExprContext* aggregate_expression_context = aggregate_state->curaggcontext;

            *aggregate_context = aggregate_expression_context->ecxt_per_tuple_memory;
        }
        return AGG_CONTEXT_AGGREGATE;
    }
    if (WindowAggState* window_agg_state =
            pg_node_view{call_info->context}.try_cast<WindowAggState>()) {
        if (aggregate_context)
            *aggregate_context = window_agg_state->curaggcontext;
        return AGG_CONTEXT_WINDOW;
    }

    /* Always initialize the output when the call has no aggregate context. */
    if (aggregate_context)
        *aggregate_context = nullptr;
    return 0;
}

/** Return the active aggregate call, or nullptr outside aggregate support.
 * A shared transition returns the aggregate reference stored with that transition. */
// NOLINTNEXTLINE(readability-identifier-naming) -- exact PostgreSQL fmgr ABI name
extern "C" Aggref* AggGetAggref(FunctionCallInfo call_info) {
    if (AggState* aggregate_state = pg_node_view{call_info->context}.try_cast<AggState>()) {
        AggStatePerAgg current_aggregate;
        AggStatePerTrans current_transition;

        /* Final functions identify the aggregate being finalized. */
        current_aggregate = aggregate_state->curperagg;

        if (current_aggregate)
            return current_aggregate->aggref;

        /* Transition functions identify their current transition descriptor. */
        current_transition = aggregate_state->curpertrans;

        if (current_transition)
            return current_transition->aggref;
    }
    return nullptr;
}

/** Return whether another aggregate may share the current transition state.
 * Unknown call contexts return true so callers avoid modifying possibly shared data. */
// NOLINTNEXTLINE(readability-identifier-naming) -- exact PostgreSQL fmgr ABI name
extern "C" bool AggStateIsShared(FunctionCallInfo call_info) {
    if (AggState* aggregate_state = pg_node_view{call_info->context}.try_cast<AggState>()) {
        AggStatePerAgg current_aggregate;
        AggStatePerTrans current_transition;

        /* A final function uses the sharing flag of its aggregate's transition. */
        current_aggregate = aggregate_state->curperagg;

        if (current_aggregate)
            return aggregate_state->pertrans[current_aggregate->transno].aggshared;

        /* A transition function can read its own sharing flag. */
        current_transition = aggregate_state->curpertrans;

        if (current_transition)
            return current_transition->aggshared;
    }
    return true;
}
