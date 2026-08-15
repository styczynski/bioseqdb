/**
 * @file
 * @brief Owns scan costing, predicate pushdown, selectivity, and path admission.
 */
module;
// Anchor Highway in this global module fragment so target-specific definitions
// imported through multiple module graphs share one translation-unit context.
#include <bioseq/pg_compat.h>
#include <hwy/highway.h>

extern "C" {
#include "catalog/namespace.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_language_d.h"
#include "catalog/pg_proc.h"
#include "commands/extension.h"
#include "optimizer/appendinfo.h"
#include "optimizer/planmain.h"
#include "storage/dsm.h"
#include "storage/shmem.h"
#include "utils/inval.h"
#include "utils/syscache.h"

#include "rewrite/rewriteManip.h"
}

export module bioseq.postgres.table:custom_scan_scan_paths_admission;

import std;
import bioseq.postgres.table.io;
import :access_method;
import :access_common;
import :utils;
import bioseq.postgres.column.table_access;
import bioseq.postgres.configuration.aligner;
import bioseq.postgres.configuration.core;
import bioseq.postgres.vectorization.types;
import bioseq.postgres.vectorization.execution;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.highway.dispatch_contract;
import bioseq.postgres.external.path_builder_slot;
import bioseq.postgres.storage.metadata;
import bioseq.postgres.table.types;
import bioseq.postgres.table.scan_state;
import bioseq.postgres.table.sql_row_codecs;
import bioseq.postgres.table.sql_rows;
import bioseq.postgres.support;
import bioseq.postgres.support.datums;
import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.scheduler.dispatch_buffer.api;
import bioseq.postgres.scheduler.supervisor;
import bioseq.postgres.scheduler.supervisor.vulkan;
import bioseq.parallelism.mode;
import bioseq.parallelism.memory_admission;
import bioseq.gpu.vulkan.runtime;
import bioseq.sequences;
import bioseq.sequences.simd;
import bioseq.pipeline.stats;
import bioseq.pipeline.minimap_port;
import bioseq.pipeline.pipe;
import bioseq.sequences.cigar;
import bioseq.postgres.types.compact_sequence;
import bioseq.postgres.storage.manager;
import bioseq.storage.layout;
import bioseq.storage.size;
import bioseq.storage.coordinates;
import bioseq.storage.lazy_sequence;
import bioseq.postgres.storage.scanner;
import bioseq.indexing.type;
import bioseq.indexing.symbol_counts;
import bioseq.postgres.storage.indexing.chunked_storage;
import bioseq.postgres.storage.indexing.admin;
import bioseq.instrumentation.summary_writer;
import bioseq.common.seqan_adapter;
import bioseq.postgres.table.batch_sort_scan;

import :custom_scan_support;
import :custom_scan_slice_projection;
import :custom_scan_join_paths;

using namespace bioseq::postgres::support;

export {

    /*
     * RemovePathsByPredicate removes the paths that removePathPredicate
     * evaluates to true from pathlist of given rel.
     */
    void remove_paths_by_predicate(RelOptInfo* rel, path_predicate remove_path_predicate) {
        bioseq_log<logging_level::debug>("[bioseq] pg: RemovePathsByPredicate() entered");
        postgres_list<Path> filtered_path_list;
        postgres_list<Path> filtered_partial_path_list;

        for (auto path : postgres_list<Path>(rel->pathlist)) {
            if (!remove_path_predicate(path)) {
                filtered_path_list.append(path);
            }
        }

        rel->pathlist = filtered_path_list.release();

        for (auto path : postgres_list<Path>(rel->partial_pathlist)) {
            if (!remove_path_predicate(path)) {
                filtered_partial_path_list.append(path);
            }
        }

        rel->partial_pathlist = filtered_partial_path_list.release();
    }

    /*
     * IsNotIndexPath returns true if given path is not an IndexPath.
     */
    bool is_not_index_path(Path* path) {
        return !pg_node_view{to_pg_node(path)}.is_type<IndexPath>();
    }

    /*
     * CreateBioseqColumnSeqScanPath returns Path for sequential scan on columnar
     * table with relationId.
     */
    Path* create_bioseq_column_seq_scan_path(PlannerInfo* root, RelOptInfo* rel, Oid relation_id) {
        bioseq_log<logging_level::debug>("[bioseq] pg: CreateBioseqColumnSeqScanPath() entered");
        /* columnar doesn't support parallel scan */
        int parallel_workers = 0;

        Relids required_outer = rel->lateral_relids;
        Path* path = create_seqscan_path(root, rel, required_outer, parallel_workers);
        cost_bioseq_column_seq_path(rel, relation_id, path);
        return path;
    }

    /*
     * CostBioseqColumnPaths re-costs paths of given RelOptInfo for
     * columnar table with relationId.
     */
    void cost_bioseq_column_paths(PlannerInfo* root, RelOptInfo* rel, Oid relation_id) {
        bioseq_log<logging_level::debug>("[bioseq] pg: CostBioseqColumnPaths() entered");
        for (auto path : postgres_list<Path>(rel->pathlist)) {
            if (pg_node_view{to_pg_node(path)}.is_type<IndexPath>()) {
                /*
                 * Since we don't provide implementations for scan_bitmap_next_block
                 * & scan_bitmap_next_tuple, postgres doesn't generate bitmap index
                 * scan paths for columnar tables already (see related comments in
                 * TableAmRoutine). For this reason, we only consider IndexPath's
                 * here.
                 */
                cost_bioseq_column_index_path(
                    root, rel, relation_id, reinterpret_cast<IndexPath*>(path));
            } else if (path->pathtype == T_SeqScan) {
                cost_bioseq_column_seq_path(rel, relation_id, path);
            }
        }
    }

    /*
     * CostBioseqColumnIndexPath re-costs given index path for columnar table with
     * relationId.
     */
    void cost_bioseq_column_index_path(PlannerInfo* root,
                                       RelOptInfo* rel,
                                       Oid relation_id,
                                       IndexPath* index_path) {
        if (!enable_indexscan) {
            bioseq_log<logging_level::debug>(
                "[bioseq] pg: CostBioseqColumnIndexPath() skipped (enable_indexscan=false)");
            /* costs are already set to disable_cost, don't adjust them */
            return;
        }

        bioseq_log_pg(DEBUG4,
                      "columnar table index scan costs estimated by "
                      "indexAM: startup cost = {:.10f}, total cost = "
                      "{:.10f}",
                      index_path->path.startup_cost,
                      index_path->path.total_cost);

        /*
         * We estimate the cost for columnar table read during index scan. Also,
         * instead of overwriting total cost, we "add" ours to the cost estimated
         * by indexAM since we should consider index traversal related costs too.
         */

        if (!bioseq::settings::planner::index_scan::enabled) {
            bioseq_log<logging_level::debug>(
                "[bioseq] pg: CostBioseqColumnIndexPath() "
                "bioseq::settings::planner::index_scan::enabled disabled, adding columnar cost");
            Cost bioseq_column_index_scan_cost =
                bioseq_column_index_scan_additional_cost(root, rel, relation_id, index_path);
            index_path->path.total_cost += bioseq_column_index_scan_cost;
        }

        bioseq_log<logging_level::debug>(
            "[bioseq] pg: CostBioseqColumnIndexPath() cost computation complete");

        bioseq_log_pg(DEBUG4,
                      "columnar table index scan costs re-estimated "
                      "by columnarAM (including indexAM costs): "
                      "startup cost = {:.10f}, total cost = {:.10f}",
                      index_path->path.startup_cost,
                      index_path->path.total_cost);
    }

    /*
     * BioseqColumnIndexScanAdditionalCost returns additional cost estimated for
     * index scan described by IndexPath for columnar table with relationId.
     */
    Cost bioseq_column_index_scan_additional_cost(PlannerInfo* root,
                                                  RelOptInfo* rel,
                                                  Oid relation_id,
                                                  IndexPath* index_path) {
        bioseq_log<logging_level::debug>(
            "[bioseq] pg: BioseqColumnIndexScanAdditionalCost() entered");
        int number_of_columns_read = relation_id_get_number_of_attributes(relation_id);
        Cost per_stripe_cost =
            bioseq_column_per_stripe_scan_cost(rel, relation_id, number_of_columns_read);

        /*
         * We don't need to pass correct loop count to amcostestimate since we
         * will only use index correlation & index selectivity, and loop count
         * doesn't have any effect on those two.
         */
        double fake_loop_count = 1;
        Cost fake_index_startup_cost;
        Cost fake_index_total_cost;
        double fake_index_pages;
        Selectivity index_selectivity;
        double index_correlation;
        amcostestimate_function amcostestimate = index_path->indexinfo->amcostestimate;
        amcostestimate(root,
                       index_path,
                       fake_loop_count,
                       &fake_index_startup_cost,
                       &fake_index_total_cost,
                       &index_selectivity,
                       &index_correlation,
                       &fake_index_pages);

        Relation relation = RelationIdGetRelation(relation_id);
        uint64_t row_count = column_table_row_count(relation);
        RelationClose(relation);
        double estimated_rows = row_count * index_selectivity;

        /*
         * In the worst case (i.e no correlation between the column & the index),
         * we need to read a different stripe for each row.
         */
        double max_stripe_read_count = estimated_rows;

        /*
         * In the best case (i.e the column is fully correlated with the index),
         * we wouldn't read the same stripe again and again thanks
         * to locality.
         */
        double avg_stripe_row_count =
            row_count / static_cast<double>(bioseq_column_table_stripe_count(relation_id));
        double min_stripe_read_count = estimated_rows / avg_stripe_row_count;

        /*
         * While being close to 0 means low correlation, being close to -1 or +1
         * means high correlation. For index scans on columnar tables, it doesn't
         * matter if the column and the index are "correlated" (+1) or
         * "anti-correlated" (-1) since both help us avoiding from reading the
         * same stripe again and again.
         */
        double abs_index_correlation = std::abs(index_correlation);

        /*
         * To estimate the number of stripes that we need to read, we do linear
         * interpolation between minStripeReadCount & maxStripeReadCount. To do
         * that, we use complement to 1 of absolute correlation, where being
         * close to 0 means high correlation and being close to 1 means low
         * correlation.
         * In practice, we only want to do an index scan when absIndexCorrelation
         * is 1 (or extremely close to it), or when the absolute number of tuples
         * returned is very small. Other cases will have a prohibitive cost.
         */
        double complement_index_correlation = 1 - abs_index_correlation;
        double estimated_stripe_read_count =
            min_stripe_read_count +
            complement_index_correlation * (max_stripe_read_count - min_stripe_read_count);

        /* even in the best case, we will read a single stripe */
        estimated_stripe_read_count = std::max(estimated_stripe_read_count, 1.0);

        Cost scan_cost = per_stripe_cost * estimated_stripe_read_count;

        bioseq_log_pg(DEBUG4,
                      "re-costing index scan for columnar table: "
                      "selectivity = {:.10f}, complement abs "
                      "correlation = {:.10f}, per stripe cost = {:.10f}, "
                      "estimated stripe read count = {:.10f}, "
                      "total additional cost = {:.10f}",
                      index_selectivity,
                      complement_index_correlation,
                      per_stripe_cost,
                      estimated_stripe_read_count,
                      scan_cost);

        return scan_cost;
    }

    /*
     * CostBioseqColumnSeqPath sets costs given seq path for columnar table with
     * relationId.
     */
    void cost_bioseq_column_seq_path(RelOptInfo* rel, Oid relation_id, Path* path) {
        if (!enable_seqscan) {
            /* costs are already set to disable_cost, don't adjust them */
            return;
        }

        /*
         * Seq scan doesn't support projection or qual pushdown, so we will read
         * all the stripes and all the columns.
         */
        double stripes_to_read = bioseq_column_table_stripe_count(relation_id);
        int number_of_columns_read = relation_id_get_number_of_attributes(relation_id);

        path->rows = rel->tuples;
        path->startup_cost = 0;
        path->total_cost = stripes_to_read * bioseq_column_per_stripe_scan_cost(
                                                 rel, relation_id, number_of_columns_read);
    }

    /*
     * RelationIdGetNumberOfAttributes returns number of attributes that relation
     * with relationId has.
     */
    int relation_id_get_number_of_attributes(Oid relation_id) {
        Relation relation = RelationIdGetRelation(relation_id);
        int nattrs = relation->rd_att->natts;
        RelationClose(relation);
        return nattrs;
    }

    /*
     * CheckVarStats() checks whether a qual involving this Var is likely to be
     * useful based on the correlation stats. If so, or if stats are unavailable,
     * return true; otherwise return false and sets absVarCorrelation in case
     * caller wants to use for logging purposes.
     */
    bool check_var_stats(PlannerInfo* root, Var* var, Oid sortop, float* abs_var_correlation) {
        bioseq_log<logging_level::debug>("[bioseq] pg: CheckVarStats() entered");
        /*
         * Collect isunique, ndistinct, and varCorrelation.
         */
        VariableStatData var_stat_data;
        examine_variable(root, reinterpret_cast<Node*>(var), var->varno, &var_stat_data);
        if (var_stat_data.rel == nullptr || !HeapTupleIsValid(var_stat_data.statsTuple)) {
            ReleaseVariableStats(var_stat_data);
            return true;
        }

        AttStatsSlot sslot;
        if (!get_attstatsslot(&sslot,
                              var_stat_data.statsTuple,
                              STATISTIC_KIND_CORRELATION,
                              sortop,
                              ATTSTATSSLOT_NUMBERS)) {
            ReleaseVariableStats(var_stat_data);
            return true;
        }

        Assert(sslot.nnumbers == 1);

        float var_correlation = sslot.numbers[0];

        free_attstatsslot(&sslot);
        ReleaseVariableStats(var_stat_data);

        /*
         * If the Var is not highly correlated, then the chunk's min/max bounds
         * will be nearly useless.
         */
        if (std::abs(var_correlation) <
            bioseq::settings::planner::qual_pushdown::correlation_threshold) {
            if (abs_var_correlation) {
                /*
                 * Report absVarCorrelation if caller wants to know why given
                 * var is rejected.
                 */
                *abs_var_correlation = std::abs(var_correlation);
            }
            return false;
        }

        return true;
    }

    /*
     * ExprReferencesRelid returns true if any of the Expr's Vars refer to the
     * given relid; false otherwise.
     */
    bool expr_references_relid(Expr* expr, Index relid) {
        List* expr_vars = pull_var_clause(reinterpret_cast<Node*>(expr),
                                          PVC_RECURSE_AGGREGATES | PVC_RECURSE_WINDOWFUNCS |
                                              PVC_RECURSE_PLACEHOLDERS);
        for (auto var : postgres_list<Var>(expr_vars)) {
            if (var->varno == relid) {
                return true;
            }
        }
        return false;
    }

    /*
     * ExtractPushdownClause extracts an Expr node from given clause for pushing down
     * into the given rel (including join clauses). This test may not be exact in
     * all cases; it's used to reduce the search space for parameterization.
     *
     * Note that we don't try to handle cases like "Var + ExtParam = 3". That
     * would require going through eval_const_expression after parameter binding,
     * and that doesn't seem worth the effort. Here we just look for "Var op Expr"
     * or "Expr op Var", where Var references rel and Expr references other rels
     * (or no rels at all).
     *
     * Moreover, this function also looks into BoolExpr's to recursively extract
     * pushdownable OpExpr's of them:
     * i)   AND_EXPR:
     *      Take pushdownable args of AND expressions by ignoring the other args.
     * ii)  OR_EXPR:
     *      Ignore the whole OR expression if we cannot extract a pushdownable Expr
     *      from one of its args.
     * iii) NOT_EXPR:
     *      Simply ignore NOT expressions since we don't expect to see them before
     *      an expression that we can pushdown, see the comment in function.
     *
     * The reasoning for those three rules could also be summarized as such;
     * for any expression that we cannot push-down, we must assume that it
     * evaluates to true.
     *
     * For example, given following WHERE clause:
     * (
     *     (a > random() OR a < 30)
     *     AND
     *     a < 200
     * ) OR
     * (
     *     a = 300
     *     OR
     *     a > 400
     * );
     * Even if we can pushdown (a < 30), we cannot pushdown (a > random() OR a < 30)
     * due to (a > random()). However, we can pushdown (a < 200), so we extract
     * (a < 200) from the lhs of the top level OR expression.
     *
     * For the rhs of the top level OR expression, since we can pushdown both (a = 300)
     * and (a > 400), we take this part as is.
     *
     * Finally, since both sides of the top level OR expression yielded pushdownable
     * expressions, we will pushdown the following:
     *  (a < 200) OR ((a = 300) OR (a > 400))
     */
    /*
     * mmsearch() FuncExprs are recognised here as pushdownable scan
     * quals even though they have no btree opclass and no scan-key
     * shape: the actual filtering happens at execution time inside
     * batch_processor::process_sequences. The reason we still register
     * them as "pushdownable" is to make the planner consider
     * parameterised paths for cross-table mmsearches like
     *   WHERE mmsearch(t1.seq, t2.seq2)
     * Without this recognition `FilterPushdownClauses` drops the
     * FuncExpr from `joinClauses`, `FindCandidateRelids` returns no
     * outer rels for it, and `AddBioseqColumnScanPathsRec` never asks
     * `get_baserel_parampathinfo` to bring the qual into the inner
     * scan's `ppi_clauses`. The result: mmsearch ends up as a Nested
     * Loop join filter (the C stub), which always returns TRUE, and
     * the inner scan never runs the aligner — every cross-product row
     * comes back with NULL bioseq.* projections.
     *
     * Returning the unchanged FuncExpr from this helper tells
     * FilterPushdownClauses "this clause references this rel and is
     * safe to attach to its quals." Postgres (via
     * `replace_nestloop_params`) will rewrite the cross-rel Var (e.g.
     * t2.seq2) into a PARAM_EXEC Param at plan-finalization time, and
     * BeginCustomScan / ReScanCustomScan already handle Params for the
     * mmsearch second argument.
     */
    [[nodiscard]] bool
    is_mmsearch_pushdownable(RelOptInfo* rel, FuncExpr* fe, const bioseq_stub_oids& stub_oids) {
        if (!is_mmsearch_funcid(fe->funcid, stub_oids)) {
            return false;
        }
        pg_first_two_node_arguments two_args =
            get_pg_first_two_arguments_if_arity_between(*fe, 2, 3);
        if (!two_args.is_present()) {
            return false;
        }
        Node* arg1 = two_args.second_argument;
        auto target_resolution = resolve_base_local_mmsearch_operand(two_args.first_argument,
                                                                     rel,
                                                                     stub_oids,
                                                                     /*require_sequence_var=*/true);
        if (!target_resolution.has_value() || target_resolution->primary_sequence_var == nullptr) {
            return false;
        }
        Var* col_var = target_resolution->primary_sequence_var;
        /*
         * The first argument is the column to align — it MUST belong to
         * `rel`. Cross-table mmsearch with the column on the OTHER side
         * (e.g. mmsearch(t1.seq, t2.seq) when we are processing t2)
         * doesn't push to this rel; the mirror call from t1's side
         * (mmsearch(t1.seq, t2.seq) processing t1) does.
         */
        if (col_var->varno != rel->relid) {
            return false;
        }
        if (col_var->varattno <= 0) {
            return false;
        }
        /*
         * Constrain the second argument to shapes BioseqColumnScan_BeginCustomScan
         * actually handles at execution time (Const literal, Param-driven NL
         * inner-side rewrite, or another base-relation Var with a varno that
         * Postgres can later turn into a PARAM_EXEC on the parameterised path).
         * Without this filter the planner would generate a parameterised path
         * carrying a complex-expression mmsearch which BeginCustomScan then
         * silently rejects, leaving the buffer inactive and bioseq.* projections
         * NULL.
         */
        const bool local_query_recipe =
            !pg_node_view{arg1}.is_type<Const>() && !pg_node_view{arg1}.is_type<Var>() &&
            !pg_node_view{arg1}.is_type<Param>() &&
            resolve_base_local_mmsearch_operand(arg1,
                                                rel,
                                                stub_oids,
                                                /*require_sequence_var=*/false)
                .has_value();
        if (!pg_node_view{arg1}.is_type<Const>() && !pg_node_view{arg1}.is_type<Var>() &&
            !pg_node_view{arg1}.is_type<Param>() && !local_query_recipe)
            return false;
        /* A volatile function inside a validated base-local query recipe stays at
         * this scan frontier and is evaluated by one retained ExprState exactly
         * once per admitted row.  Only an unvalidated volatile shape is ineligible. */
        if (contain_volatile_functions(arg1) && !local_query_recipe) {
            return false;
        }
        /*
         * Typmod compatibility check. We accept a supported bioseq.seq arg1
         * here and let BeginCustomScan enforce the binding contract:
         *   - same (alphabet, width)     → direct dispatch
         *   - same alphabet, diff width → rewidth arg1 to the column width
         *   - different bound alphabet  → SQLSTATE 42804 before dispatch
         *
         * Generate the parameterized path for supported sequence types and perform
         * coalescing in BeginCustomScan, where the executor has the complete binding
         * information needed to produce a value or a precise type error.
         */
        if (col_var->vartype != stub_oids.bioseq_seq_type_oid || target_resolution->typmod < 0) {
            return false;
        }
        int32 arg1_typmod = -1;
        Oid arg1_typoid = InvalidOid;
        if (Const* c = try_cast_pg_node<Const>(arg1)) {
            arg1_typmod = c->consttypmod;
            arg1_typoid = c->consttype;
        } else if (Var* v = try_cast_pg_node<Var>(arg1)) {
            arg1_typmod = v->vartypmod;
            arg1_typoid = v->vartype;
        } else if (Param* p = try_cast_pg_node<Param>(arg1)) {
            arg1_typmod = p->paramtypmod;
            arg1_typoid = p->paramtype;
        } else if (local_query_recipe) {
            auto query_resolution =
                resolve_base_local_mmsearch_operand(arg1,
                                                    rel,
                                                    stub_oids,
                                                    /*require_sequence_var=*/false);
            arg1_typmod = query_resolution.has_value() ? query_resolution->typmod : -1;
            arg1_typoid = stub_oids.bioseq_seq_type_oid;
        }
        if (arg1_typoid != stub_oids.bioseq_seq_type_oid || arg1_typmod < 0) {
            return false;
        }
        /*
         * Width must always be in the supported set (8 / 16 / 32 / 64).
         * Alphabet must be one we know how to dispatch on (dna4 / dna5).
         * The pack/unpack helper will raise on truly invalid typmods, so
         * we keep this gate loose: anything BeginCustomScan can decode
         * is allowed through. Cross-typmod is a runtime concern, not a
         * planner-time rejection.
         */
        return true;
    }

    Expr* extract_pushdown_clause(PlannerInfo* root, RelOptInfo* rel, Node* node) {
        CHECK_FOR_INTERRUPTS();
        check_stack_depth();

        if (node == nullptr) {
            return nullptr;
        }

        /*
         * mmsearch FuncExpr accepted by IsMmsearchPushdownable above.
         * When the qual is a parameter-driven cross-table join
         * mmsearch(t1.seq, t2.seq2), letting it through here is what
         * makes Postgres generate the parameterised inner-side path
         * that drives mmsearch_parameter_rebind.
         */
        if (FuncExpr* fe = try_cast_pg_node<FuncExpr>(node)) {
            const bioseq_stub_oids& stub_oids = get_cached_stub_oids();
            if (is_mmsearch_pushdownable(rel, fe, stub_oids)) {
                bioseq_log<logging_level::debug>("[bioseq] pg: ExtractPushdownClause() accepting "
                                                 "mmsearch funcexpr funcid={} for rel relid={}",
                                                 fe->funcid,
                                                 static_cast<int>(rel->relid));
                return reinterpret_cast<Expr*>(node);
            }
        }

        if (BoolExpr* boolean_expression = try_cast_pg_node<BoolExpr>(node)) {
            if (boolean_expression->boolop == NOT_EXPR) {
                /*
                 * Standard planner should have already applied de-morgan rule to
                 * simple NOT expressions. If we encounter with such an expression
                 * here, then it can't be a pushdownable one, such as:
                 *   WHERE id NOT IN (SELECT id FROM something).
                 */
                bioseq_log_pg(planner_debug_level(),
                              "columnar planner: cannot push down clause: "
                              "must not contain a subplan");
                return nullptr;
            }

            postgres_list<Expr> pushdownable_args;

            for (auto bool_expr_arg : postgres_list<Node>(boolean_expression->args)) {
                Expr* pushdownable_arg =
                    extract_pushdown_clause(root, rel, reinterpret_cast<Node*>(bool_expr_arg));
                if (pushdownable_arg) {
                    pushdownable_args.append(pushdownable_arg);
                } else if (boolean_expression->boolop == OR_EXPR) {
                    bioseq_log_pg(planner_debug_level(),
                                  "columnar planner: cannot push down clause: "
                                  "all arguments of an OR expression must be "
                                  "pushdownable but one of them was not, due "
                                  "to the reason given above");
                    return nullptr;
                }

                /* simply skip AND args that we cannot pushdown */
            }

            int npushdownable_args = pushdownable_args.size();
            if (npushdownable_args == 0) {
                bioseq_log_pg(planner_debug_level(),
                              "columnar planner: cannot push down clause: "
                              "none of the arguments were pushdownable, "
                              "due to the reason(s) given above");
                return nullptr;
            } else if (npushdownable_args == 1) {
                return pushdownable_args.get_first();
            }

            if (boolean_expression->boolop == AND_EXPR) {
                return make_andclause(pushdownable_args.release());
            } else if (boolean_expression->boolop == OR_EXPR) {
                return make_orclause(pushdownable_args.release());
            } else {
                /* already discarded NOT expr, so should not be reachable */
                return nullptr;
            }
        }

        OpExpr* op_expr = try_cast_pg_node<OpExpr>(node);
        pg_binary_node_arguments op_args =
            op_expr != nullptr ? get_pg_binary_arguments(*op_expr) : pg_binary_node_arguments{};
        if (!op_args.is_present()) {
            bioseq_log_pg(planner_debug_level(),
                          "columnar planner: cannot push down clause: "
                          "must be binary operator expression");
            return nullptr;
        }

        Expr* lhs = reinterpret_cast<Expr*>(op_args.first_argument);
        Expr* rhs = reinterpret_cast<Expr*>(op_args.second_argument);

        Var* var_side;
        Expr* expr_side;

        Var* lhs_var = try_cast_pg_node<Var>(to_pg_node(lhs));
        Var* rhs_var = try_cast_pg_node<Var>(to_pg_node(rhs));

        if (lhs_var != nullptr && lhs_var->varno == rel->relid &&
            !expr_references_relid(rhs, rel->relid)) {
            var_side = lhs_var;
            expr_side = rhs;
        } else if (rhs_var != nullptr && rhs_var->varno == rel->relid &&
                   !expr_references_relid(lhs, rel->relid)) {
            var_side = rhs_var;
            expr_side = lhs;
        } else {
            bioseq_report_pg(planner_debug_level())
                .with_message("columnar planner: cannot push down clause: "
                              "must match 'Var <op> Expr' or 'Expr <op> Var'")
                .with_hint("Var must only reference this rel, "
                           "and Expr must not reference this rel");
            return nullptr;
        }

        if (var_side->varattno <= 0) {
            bioseq_log_pg(planner_debug_level(),
                          "columnar planner: cannot push down clause: "
                          "var is whole-row reference or system column");
            return nullptr;
        }

        if (contain_volatile_functions(reinterpret_cast<Node*>(expr_side))) {
            bioseq_log_pg(planner_debug_level(),
                          "columnar planner: cannot push down clause: "
                          "expr contains volatile functions");
            return nullptr;
        }

        /* only the default opclass is used for qual pushdown. */
        Oid var_op_class = GetDefaultOpClass(var_side->vartype, BTREE_AM_OID);
        Oid var_op_family;
        Oid var_opc_in_type;

        if (!OidIsValid(var_op_class) ||
            !get_opclass_opfamily_and_input_type(var_op_class, &var_op_family, &var_opc_in_type)) {
            bioseq_log_pg(planner_debug_level(),
                          "columnar planner: cannot push down clause: "
                          "cannot find default btree opclass and opfamily for type: {}",
                          format_type_be(var_side->vartype));
            return nullptr;
        }

        if (!op_in_opfamily(op_expr->opno, var_op_family)) {
            bioseq_log_pg(planner_debug_level(),
                          "columnar planner: cannot push down clause: "
                          "operator {} not a member of opfamily {}",
                          op_expr->opno,
                          var_op_family);
            return nullptr;
        }

        Oid sortop = get_opfamily_member(
            var_op_family, var_opc_in_type, var_opc_in_type, BTLessStrategyNumber);
        Assert(OidIsValid(sortop));

        /*
         * Check that statistics on the Var support the utility of this
         * clause.
         */
        float abs_var_correlation = 0;
        if (!check_var_stats(root, var_side, sortop, &abs_var_correlation)) {
            bioseq_log_pg(planner_debug_level(),
                          "columnar planner: cannot push down clause: "
                          "absolute correlation ({:.3f}) of var attribute {} is "
                          "smaller than the value configured in "
                          "\"bioseq.planner.qual_pushdown.correlation_threshold\" "
                          "({:.3f})",
                          abs_var_correlation,
                          var_side->varattno,
                          bioseq::settings::planner::qual_pushdown::correlation_threshold);
            return nullptr;
        }

        return reinterpret_cast<Expr*>(node);
    }

    /*
     * FilterPushdownClauses filters for clauses that are candidates for pushing
     * down into rel.
     */
    List* filter_pushdown_clauses(PlannerInfo* root, RelOptInfo* rel, List* input_clauses) {
        postgres_list<RestrictInfo> filtered_clauses;
        for (auto rinfo : postgres_list<RestrictInfo>(input_clauses)) {
            /*
             * Ignore clauses that don't refer to this rel, and pseudoconstants.
             *
             * Pseudoconstants contain no Vars and are not pushed into this rel.
             */
            if (rinfo->pseudoconstant || !bms_is_member(rel->relid, rinfo->required_relids)) {
                continue;
            }

            Expr* pushdownable_expr =
                extract_pushdown_clause(root, rel, reinterpret_cast<Node*>(rinfo->clause));
            if (!pushdownable_expr) {
                continue;
            }

            rinfo = copyObject(rinfo);
            rinfo->clause = pushdownable_expr;
            filtered_clauses.append(rinfo);
        }

        return filtered_clauses.release();
    }

    /*
     * PushdownJoinClauseMatches is a callback that returns true, indicating that
     * we want all of the clauses from generate_implied_equalities_for_column().
     */
    bool pushdown_join_clause_matches(PlannerInfo* root,
                                      RelOptInfo* rel,
                                      EquivalenceClass* ec,
                                      EquivalenceMember* em,
                                      void* arg) {
        return true;
    }

    /*
     * FindPushdownJoinClauses finds join clauses, including those implied by ECs,
     * that may be pushed down.
     */
    List* find_pushdown_join_clauses(PlannerInfo* root, RelOptInfo* rel) {
        postgres_list<RestrictInfo> join_clauses{copyObject(rel->joininfo)};

        /*
         * Here we are generating the clauses just so we can later extract the
         * interesting relids. This is somewhat wasteful, but it allows us to
         * filter out joinclauses, reducing the number of relids we need to
         * consider.
         *
         * This pass does not derive further EC implications from joininfo itself.
         */
        List* ec_clauses = generate_implied_equalities_for_column(
            root, rel, pushdown_join_clause_matches, nullptr, rel->lateral_referencers);
        join_clauses.concatenate(ec_clauses);

        return filter_pushdown_clauses(root, rel, join_clauses.release());
    }

    /*
     * FindCandidateRelids identifies candidate rels for parameterization from the
     * list of join clauses.
     *
     * Some rels cannot be considered for parameterization, such as a partitioned
     * parent of the given rel. Other rels are just not useful because they don't
     * appear in a join clause that could be pushed down.
     */
    Relids find_candidate_relids(PlannerInfo* root, RelOptInfo* rel, List* join_clauses) {
        Relids candidate_relids = nullptr;
        for (auto rinfo : postgres_list<RestrictInfo>(join_clauses)) {
            candidate_relids = bms_add_members(candidate_relids, rinfo->required_relids);
        }

        candidate_relids = bms_del_members(candidate_relids, rel->relids);
        candidate_relids = bms_del_members(candidate_relids, rel->lateral_relids);
        candidate_relids = bms_del_members(candidate_relids, root->outer_join_rels);
        return candidate_relids;
    }

    /*
     * Combinations() calculates the number of combinations of n things taken k at
     * a time. When the correct result is large, the calculation may produce a
     * non-integer result, or overflow to inf, which caller should handle
     * appropriately.
     *
     * Use the following two formulae from Knuth TAoCP, 1.2.6:
     *    (2) Combinations(n, k) = (n*(n-1)..(n-k+1)) / (k*(k-1)..1)
     *    (5) Combinations(n, k) = Combinations(n, n-k)
     */
    double combinations(int n, int k) {
        double v = 1;

        /*
         * If k is close to n, then both the numerator and the denominator are
         * close to n!, and we may overflow even if the input is reasonable
         * (e.g. Combinations(500, 500)). Use formula (5) to choose the smaller,
         * but equivalent, k.
         */
        k = std::min(k, n - k);

        /* calculate numerator of formula (2) first */
        for (int i = n; i >= n - k + 1; i--) {
            v *= i;
        }

        /*
         * Divide by each factor in the denominator of formula (2), skipping
         * division by 1.
         */
        for (int i = k; i >= 2; i--) {
            v /= i;
        }

        return v;
    }

    /*
     * ChooseDepthLimit() calculates the depth limit for the parameterization
     * search, given the number of candidate relations.
     *
     * The maximum number of paths generated for a given depthLimit is:
     *
     *    Combinations(nCandidates, 0) + Combinations(nCandidates, 1) + ... +
     *    Combinations(nCandidates, depthLimit)
     *
     * There's no closed formula for a partial sum of combinations, so just keep
     * increasing the depth until the number of combinations exceeds the limit.
     */
    int choose_depth_limit(int n_candidates) {
        if (!bioseq::settings::planner::qual_pushdown::enabled) {
            return 0;
        }

        int depth = 0;
        double num_paths = 1;

        while (depth < n_candidates) {
            num_paths += combinations(n_candidates, depth + 1);

            if (num_paths >
                static_cast<double>(bioseq::settings::planner::custom_scan::max_paths)) {
                break;
            }

            depth++;
        }

        return depth;
    }

} // export custom-scan partition declarations
