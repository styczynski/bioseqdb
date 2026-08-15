/**
 * @file
 * @brief Costing and path construction for Bioseq column scans.
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

export module bioseq.postgres.table:custom_scan_scan_paths;

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
             * XXX: A pseudoconstant may be of use, but it doesn't make sense to
             * push it down because it doesn't contain any Vars. Look into if
             * there's something we should do with pseudoconstants here.
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
         * XXX: also find additional clauses for joininfo that are implied by ECs?
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

    /*
     * AddBioseqColumnScanPaths is the entry point for recursively generating
     * parameterized paths. See AddBioseqColumnScanPathsRec() for discussion.
     */
    void add_bioseq_column_scan_paths(PlannerInfo* root, RelOptInfo* rel, RangeTblEntry* rte) {
        bioseq_log<logging_level::debug>("[bioseq] pg: AddBioseqColumnScanPaths() entered");
        List* join_clauses = find_pushdown_join_clauses(root, rel);
        Relids candidate_relids = find_candidate_relids(root, rel, join_clauses);

        int depth_limit = choose_depth_limit(bms_num_members(candidate_relids));

        /* must always parameterize by lateral refs */
        Relids param_relids = bms_copy(rel->lateral_relids);

        add_bioseq_column_scan_paths_rec(
            root, rel, rte, param_relids, candidate_relids, depth_limit);

        /* Once a CustomPath claims a restriction, remove only alternatives that
         * would execute that same RestrictInfo through the SQL function.  This is
         * structural selection; costs remain honest and nonnegative. */
        postgres_list<RestrictInfo> owners;
        for (Path* candidate : postgres_list<Path>(rel->pathlist)) {
            if (!IsA(candidate, CustomPath))
                continue;
            CustomPath* custom = reinterpret_cast<CustomPath*>(candidate);
            bool is_mmsearch_owner = false;
            for (Node* private_node : postgres_list<Node>(custom->custom_private)) {
                if (private_node == nullptr || !IsA(private_node, List))
                    continue;
                List* tag = reinterpret_cast<List*>(private_node);
                if (list_length(tag) == 3 && IsA(reinterpret_cast<Node*>(linitial(tag)), Integer) &&
                    intVal(linitial(tag)) == k_bioseq_mmsearch_owner_magic) {
                    is_mmsearch_owner = true;
                    break;
                }
            }
            if (!is_mmsearch_owner)
                continue;
            for (RestrictInfo* owner : postgres_list<RestrictInfo>(custom->custom_restrictinfo)) {
                if (!list_member_ptr(owners.get(), owner))
                    owners.append(owner);
            }
        }
        auto prune_owned = [rel, &owners](List* paths) {
            postgres_list<Path> survivors;
            for (Path* candidate : postgres_list<Path>(paths)) {
                bool remove = false;
                for (RestrictInfo* owner : owners) {
                    const bool executes_owner =
                        list_member_ptr(rel->baserestrictinfo, owner) ||
                        (candidate->param_info != nullptr &&
                         list_member_ptr(candidate->param_info->ppi_clauses, owner));
                    const bool owns_restriction =
                        IsA(candidate, CustomPath) &&
                        list_member_ptr(
                            reinterpret_cast<CustomPath*>(candidate)->custom_restrictinfo, owner);
                    if (executes_owner && !owns_restriction) {
                        remove = true;
                        break;
                    }
                }
                if (!remove)
                    survivors.append(candidate);
            }
            return survivors.release();
        };
        if (!owners.is_empty()) {
            rel->pathlist = prune_owned(rel->pathlist);
            rel->partial_pathlist = prune_owned(rel->partial_pathlist);
        }

        bioseq_log<logging_level::debug>("[bioseq] pg: AddBioseqColumnScanPaths() returning");
    }

    /*
     * AddBioseqColumnScanPathsRec is a recursive function to search the
     * parameterization space and add CustomPaths for columnar scans.
     *
     * The set paramRelids is the parameterization at the current level, and
     * candidateRelids is the set from which we draw to generate paths with
     * greater parameterization.
     *
     * BioseqColumn tables resemble indexes because of the ability to push down
     * quals. Ordinary quals, such as x = 7, can be pushed down easily. But join
     * quals of the form "x = y" (where "y" comes from another rel) require the
     * proper parameterization.
     *
     * Paths that require more outer rels can push down more join clauses that
     * depend on those outer rels. But requiring more outer rels gives the planner
     * fewer options for the shape of the plan. That means there is a trade-off,
     * and we should generate plans of various parameterizations, then let the
     * planner choose. We always need to generate one minimally-parameterized path
     * (parameterized only by lateral refs, if present) to make sure that at least
     * one path can be chosen. Then, we generate as many parameterized paths as we
     * reasonably can.
     *
     * The set of all possible parameterizations is the power set of
     * candidateRelids. The power set has cardinality 2^N, where N is the
     * cardinality of candidateRelids. To avoid creating a huge number of paths,
     * limit the depth of the search; the depthLimit is equivalent to the maximum
     * number of required outer rels (beyond the minimal parameterization) for the
     * path. A depthLimit of zero means that only the minimally-parameterized path
     * will be generated.
     */
    void add_bioseq_column_scan_paths_rec(PlannerInfo* root,
                                          RelOptInfo* rel,
                                          RangeTblEntry* rte,
                                          Relids param_relids,
                                          Relids candidate_relids,
                                          int depth_limit) {
        CHECK_FOR_INTERRUPTS();
        check_stack_depth();

        Assert(!bms_overlap(param_relids, candidate_relids));

        Path* bioseq_column_scan_path = add_bioseq_column_scan_path(root, rel, rte, param_relids);
        CustomPath* owner_path = IsA(bioseq_column_scan_path, CustomPath)
                                     ? reinterpret_cast<CustomPath*>(bioseq_column_scan_path)
                                     : nullptr;
        if (owner_path != nullptr)
            for (RestrictInfo* owner :
                 postgres_list<RestrictInfo>(owner_path->custom_restrictinfo)) {
                auto prune_competing_owner_execution = [rel, owner](List* paths) {
                    postgres_list<Path> survivors;
                    for (Path* candidate : postgres_list<Path>(paths)) {
                        const bool executes_owner =
                            list_member_ptr(rel->baserestrictinfo, owner) ||
                            (candidate->param_info != nullptr &&
                             list_member_ptr(candidate->param_info->ppi_clauses, owner));
                        const bool owns_restriction =
                            IsA(candidate, CustomPath) &&
                            list_member_ptr(
                                reinterpret_cast<CustomPath*>(candidate)->custom_restrictinfo,
                                owner);
                        if (!executes_owner || owns_restriction)
                            survivors.append(candidate);
                    }
                    return survivors.release();
                };
                rel->pathlist = prune_competing_owner_execution(rel->pathlist);
                rel->partial_pathlist = prune_competing_owner_execution(rel->partial_pathlist);
            }
        if (bioseq::settings::execution::parallel::enabled)
            bioseq_column_scan_path->total_cost += bioseq_column_scan_path->rows * 0.1;
        add_path(rel, bioseq_column_scan_path);

        /* For columnar custom scan we should also do planning for parallel exeuction
         * if it is enabled with GUC.
         */
        if (bioseq::settings::execution::parallel::enabled) {
            int bioseq_min_parallel_process_running =
                bioseq::settings::execution::parallel::minimum_processes;

            if (bioseq::settings::execution::parallel::minimum_processes > max_parallel_workers) {
                bioseq_log_pg(
                    DEBUG1,
                    "bioseq.min_parallel_proceses is set higher than max_parallel_workers.");
                bioseq_log_pg(DEBUG1,
                              "Using max_parallel_workers instead for parallel columnar scan.");
                bioseq_min_parallel_process_running = std::min(
                    max_parallel_workers, bioseq::settings::execution::parallel::minimum_processes);
            }

            if (parallel_leader_participation)
                bioseq_min_parallel_process_running--;

            /*
             * We know how many workers are we going to run. Since workers are using single stripe
             * probably it doesn't have much sense to spawn more workers than stripes in table.
             */
            bioseq_min_parallel_process_running = std::min<int64_t>(
                bioseq_min_parallel_process_running,
                parallel_leader_participation ? bioseq_column_table_stripe_count(rte->relid) - 1
                                              : bioseq_column_table_stripe_count(rte->relid));

            // FIXME: Fix this!!!
            const bool force_parallel = true;
            if (force_parallel) {
                bioseq_log<logging_level::debug>(
                    "[bioseq] pg: AddBioseqColumnScanPathsRec() force_parallel override active");
                bioseq_min_parallel_process_running =
                    bioseq::settings::execution::parallel::minimum_processes;
            }

            if (force_parallel || (rel->consider_parallel && rel->lateral_relids == nullptr &&
                                   bioseq_min_parallel_process_running > 0)) {
                bioseq_log<logging_level::debug>("[bioseq] pg: AddBioseqColumnScanPathsRec() "
                                                 "add_partial_path for parallel scan");
                /*
                 * Passing NULL for JOIN quals in parallel execution.
                 */
                Path* parallel_bioseq_column_scan_path =
                    add_bioseq_column_scan_path(root, rel, rte, nullptr);

                if (parallel_bioseq_column_scan_path->parallel_safe) {
                    parallel_bioseq_column_scan_path->parallel_workers =
                        bioseq_min_parallel_process_running;
                    parallel_bioseq_column_scan_path->parallel_aware = true;
                    adjust_bioseq_column_parallel_scan_cost(parallel_bioseq_column_scan_path);
                    add_partial_path(rel, parallel_bioseq_column_scan_path);
                } else {
                    bioseq_log<logging_level::debug>("[bioseq] pg: suppressing parallel partial "
                                                     "path for alignment-owning custom scan");
                }
            }
        }

        bioseq_log<logging_level::debug>(
            "[bioseq] pg: AddBioseqColumnScanPathsRec() recursing into candidate relids");

        /* recurse for all candidateRelids, unless we hit the depth limit */
        Assert(depth_limit >= 0);
        if (depth_limit-- == 0) {
            return;
        }

        /*
         * Iterate through parameter combinations depth-first. Deeper levels
         * generate paths of greater parameterization (and hopefully lower
         * cost).
         */
        bioseq_log<logging_level::debug>("[bioseq] pg: AddBioseqColumnScanPathsRec() iterating "
                                         "parameter combinations depth-first");
        Relids tmp_candidate_relids = bms_copy(candidate_relids);
        int relid = -1;
        while ((relid = bms_next_member(candidate_relids, relid)) >= 0) {
            Relids tmp_param_relids = bms_add_member(bms_copy(param_relids), relid);

            /*
             * Because we are generating combinations (not permutations), remove
             * the relid from the set of candidates at this level as we descend to
             * the next.
             */
            tmp_candidate_relids = bms_del_member(tmp_candidate_relids, relid);

            add_bioseq_column_scan_paths_rec(
                root, rel, rte, tmp_param_relids, tmp_candidate_relids, depth_limit);
        }

        bms_free(tmp_candidate_relids);
    }

    /*
     * ParameterizationAsString returns the string representation of the set of
     * rels given in paramRelids.
     *
     * Takes a StringInfo so that it doesn't return palloc'd memory. This makes it
     * easy to call this function as an argument to ereport(), such that it won't
     * be evaluated unless the message is going to be output somewhere.
     */
    char* parameterization_as_string(PlannerInfo* root, Relids param_relids, StringInfo buf) {
        bioseq::postgres::support::postgres_bitmapset param_relids_view{param_relids};

        if (param_relids_view.is_empty()) {
            return "unparameterized";
        }

        bool first_time = true;
        appendStringInfoString(buf, "parameterized by rels {");
        for (int relid : param_relids_view) {
            RangeTblEntry* rte = root->simple_rte_array[relid];
            const char* relname = quote_identifier(rte->eref->aliasname);

            appendStringInfo(buf, "%s%s", first_time ? "" : ", ", relname);

            if (relname != rte->eref->aliasname) {
                pfree(const_cast<char*>(relname));
            }

            first_time = false;
        }
        appendStringInfoString(buf, "}");
        return buf->data;
    }

    /*
     * ContainsExecParams tests whether the node contains any exec params. The
     * signature accepts an extra argument for use with expression_tree_walker.
     */
    struct exec_params_probe_state {};

    struct exec_params_visitor {
        [[nodiscard]] pg_walk_step
        operator()(pg_expression_walk_cursor, pg_node_view node, exec_params_probe_state&) const {
            if (Param* param = node.try_cast<Param>()) {
                if (param->paramkind == PARAM_EXEC) {
                    return pg_walk_step::stop;
                }
            }
            return pg_walk_step::descend;
        }
    };

    [[nodiscard]] bool contains_exec_params(Node* node) {
        exec_params_probe_state state;
        exec_params_visitor visitor;
        return walk_pg_expression_including_root(node, state, visitor);
    }

    /*
     * fixup_inherited_columns
     *
     * Exact function Copied from PG16 as it's static.
     *
     * When user is querying on a table with children, it implicitly accesses
     * child tables also. So, we also need to check security label of child
     * tables and columns, but there is no guarantee attribute numbers are
     * same between the parent and children.
     * It returns a bitmapset which contains attribute number of the child
     * table based on the given bitmapset of the parent.
     */
    Bitmapset* fixup_inherited_columns(Oid parent_id, Oid child_id, Bitmapset* columns) {
        bioseq::postgres::support::postgres_bitmapset result;

        /*
         * obviously, no need to do anything here
         */
        if (parent_id == child_id) {
            return columns;
        }

        for (int index : bioseq::postgres::support::postgres_bitmapset(columns)) {
            /* bit numbers are offset by FirstLowInvalidHeapAttributeNumber */
            AttrNumber attno = index + FirstLowInvalidHeapAttributeNumber;

            /*
             * whole-row-reference shall be fixed-up later
             */
            if (attno == InvalidAttrNumber) {
                result.add(index);
                continue;
            }

            char* attname = get_attname(parent_id, attno, false);
            attno = get_attnum(child_id, attname);
            if (attno == InvalidAttrNumber) {
                bioseq_log<logging_level::error, logging_target::postgres>(
                    "cache lookup failed for attribute {} of relation {}", attname, child_id);
            }

            result.add(attno - FirstLowInvalidHeapAttributeNumber);

            pfree(attname);
        }

        return result.release();
    }

    struct cost_test_visitor {
        [[nodiscard]] pg_walk_step
        operator()(pg_expression_walk_cursor, pg_node_view node, Query& parse) const {
            bioseq_log<logging_level::debug>("[bioseq] pg: cost_test_visitor() entered");
            if (Var* var_node = node.try_cast<Var>()) {
                const auto var_no = var_node->varno;
                List* range_table = parse.rtable;
                if (range_table == nullptr) {
                    bioseq_log<logging_level::debug>(
                        "[bioseq] pg: cost_test_visitor() var rtable is null");
                } else if (!bioseq_is_concrete_rte_varno(var_no) ||
                           var_no > static_cast<Index>(list_length(range_table))) {
                    bioseq_log<logging_level::debug>("[bioseq] pg: cost_test_visitor() var varno "
                                                     "out of bounds varno={} rtable_len={}",
                                                     var_no,
                                                     list_length(range_table));
                } else {
                    RangeTblEntry* rte = rt_fetch(var_no, range_table);
                    bioseq_log<logging_level::debug>(
                        "[bioseq] pg: cost_test_visitor() rte_kind={} relid={}",
                        static_cast<int>(rte->rtekind),
                        static_cast<int>(rte->relid));
                }
            }

            return pg_walk_step::descend;
        }
    };

    /*
     * Create and add a path with the given parameterization paramRelids.
     *
     * XXX: Consider refactoring to be more like postgresGetForeignPaths(). The
     * only differences are param_info and custom_private.
     */
    Path* add_bioseq_column_scan_path(PlannerInfo* root,
                                      RelOptInfo* rel,
                                      RangeTblEntry* rte,
                                      Relids param_relids) {
        bioseq_log<logging_level::debug>("[bioseq] pg: AddBioseqColumnScanPath() entered");
        /*
         * Must return a CustomPath, not a larger structure containing a
         * CustomPath as the first field. Otherwise, nodeToString() will fail to
         * output the additional fields.
         */
        CustomPath* cpath = makeNode(CustomPath);

        cpath->methods = bioseq_column_scan_path_methods();

        /*
         * populate generic path information
         */
        Path* path = &cpath->path;
        path->pathtype = T_CustomScan;
        path->parent = rel;
        path->pathtarget = copy_pathtarget(rel->reltarget);

        const bioseq_stub_oids& stub_oids = get_cached_stub_oids();

        bioseq_log<logging_level::debug>(
            "[bioseq] pg: AddBioseqColumnScanPath() mmsearch_oid={} score_oid={} cigar_oid={} "
            "query_start_oid={} symbol_count_oid={}",
            stub_oids.mmsearch_oid,
            stub_oids.score_oid,
            stub_oids.cigar_oid,
            stub_oids.query_start_oid,
            stub_oids.symbol_count_oid);

        bioseq_log<logging_level::debug>(
            "[bioseq] pg: AddBioseqColumnScanPath() walking pathtarget expressions");
        cost_test_visitor cost_visitor;
        for (auto expr : postgres_list<Node>(path->pathtarget->exprs)) {
            bioseq_log<logging_level::debug>(
                "[bioseq] pg: AddBioseqColumnScanPath() pathtarget expression");
            walk_pg_expression_including_root(expr, *root->parse, cost_visitor);
        }
        bioseq_log<logging_level::debug>(
            "[bioseq] pg: AddBioseqColumnScanPath() pathtarget walk complete");

        /* columnar scans are not parallel-aware, but they are parallel-safe */
        path->parallel_safe = rel->consider_parallel;

        path->param_info = get_baserel_parampathinfo(root, rel, param_relids);

        /*
         * Usable clauses for this parameterization exist in baserestrictinfo and
         * ppi_clauses.
         */
        postgres_list<RestrictInfo> clause_references;
        for (RestrictInfo* rinfo : postgres_list<RestrictInfo>(rel->baserestrictinfo)) {
            if (!list_member_ptr(clause_references.get(), rinfo)) {
                clause_references.append(rinfo);
            }
        }
        if (path->param_info != nullptr) {
            for (RestrictInfo* rinfo : postgres_list<RestrictInfo>(path->param_info->ppi_clauses)) {
                if (!list_member_ptr(clause_references.get(), rinfo)) {
                    clause_references.append(rinfo);
                }
            }
        }
        owned_mmsearch_restriction owned_mmsearch = choose_owned_mmsearch_restriction(
            root,
            rel,
            rte,
            clause_references.get(),
            path->param_info == nullptr ? NIL : path->param_info->ppi_clauses,
            param_relids,
            stub_oids);
        std::optional<base_local_mmsearch_ownership> owned_mmsearch_resolution;
        if (owned_mmsearch.normalized_expr != nullptr) {
            List* candidate_expressions =
                list_concat(list_make1(owned_mmsearch.normalized_expr),
                            list_copy(owned_mmsearch.extra_normalized_exprs));
            bool exact = true;
            Node* canonical_target = nullptr;
            for (FuncExpr* candidate : postgres_list<FuncExpr>(candidate_expressions)) {
                auto resolution =
                    resolve_base_local_mmsearch_ownership(candidate, rel, param_relids, stub_oids);
                Node* target = candidate == nullptr || list_length(candidate->args) < 1
                                   ? nullptr
                                   : reinterpret_cast<Node*>(linitial(candidate->args));
                if (!resolution.has_value() ||
                    (canonical_target != nullptr && !equal(canonical_target, target))) {
                    exact = false;
                    break;
                }
                bioseq::simd::minimap2_parameters option_probe = build_params_from_gucs();
                bioseq::indexing::index_read_requirement requirement_probe =
                    bioseq::indexing::index_read_requirement::allow;
                (void)apply_aligner_options_override(
                    candidate, option_probe, requirement_probe, stub_oids);
                if (resolution->any_evaluated_operand &&
                    requirement_probe ==
                        bioseq::indexing::index_read_requirement::require_compatible_persisted) {
                    exact = false;
                    break;
                }
                if (canonical_target == nullptr) {
                    canonical_target = target;
                    owned_mmsearch_resolution = *resolution;
                }
            }
            /* The bounded two-call fallback has one target capture and one query
             * configuration.  Effectful/evaluated operands would either be shared
             * across SQL occurrences or duplicated, so keep that contract raw-only. */
            if (exact && list_length(candidate_expressions) > 1 &&
                owned_mmsearch_resolution.has_value() &&
                owned_mmsearch_resolution->any_evaluated_operand) {
                exact = false;
            }
            list_free(candidate_expressions);
            if (!exact) {
                owned_mmsearch = {};
                owned_mmsearch_resolution.reset();
            }
        }

        bioseq_log<logging_level::debug>(
            "[bioseq] pg: AddBioseqColumnScanPath() iterating allClauses (no-op pass for tracing)");
        for (auto restrict_info : clause_references) {
            (void)restrict_info;
        }

        postgres_list<RestrictInfo> all_clauses(
            filter_pushdown_clauses(root, rel, list_copy(clause_references.get())));
        List* owned_mmsearch_restrictions =
            owned_mmsearch.restriction == nullptr
                ? NIL
                : list_concat(list_make1(owned_mmsearch.restriction),
                              list_copy(owned_mmsearch.extra_restrictions));
        List* owned_mmsearch_exprs =
            owned_mmsearch.normalized_expr == nullptr
                ? NIL
                : list_concat(list_make1(owned_mmsearch.normalized_expr),
                              list_copy(owned_mmsearch.extra_normalized_exprs));
        if (owned_mmsearch.restriction != nullptr) {
            all_clauses = postgres_list<RestrictInfo>{
                restrictions_without_owned_mmsearch(all_clauses.release(),
                                                    owned_mmsearch_restrictions,
                                                    /*restrictions_are_private_copies=*/true)};
            /* Alignment statistics are backend-local and have no DSM aggregation.
             * A worker-local snapshot must not be exposed as statement statistics,
             * so every alignment-owning path is parallel unsafe. */
            path->parallel_safe = false;
        }

        slice_project_ir_builder base_operand_sources{
            .oids = &stub_oids,
            .command_type = CMD_SELECT,
            .planner_root = root,
            .target_leaf_support_relid = rel->relid,
            .source_bindings_only = true,
        };
        const bool needs_evaluated_operands = owned_mmsearch_resolution.has_value() &&
                                              owned_mmsearch_resolution->any_evaluated_operand;
        auto append_support_var = [&base_operand_sources](Var* variable) {
            if (variable == nullptr)
                return;
            for (Var* existing :
                 postgres_list<Var>(base_operand_sources.target_leaf_support_vars)) {
                if (equal(existing, variable))
                    return;
            }
            base_operand_sources.target_leaf_support_vars =
                lappend(base_operand_sources.target_leaf_support_vars, variable);
        };
        if (needs_evaluated_operands) {
            auto register_operand = [&base_operand_sources, &append_support_var](
                                        const base_local_mmsearch_operand_resolution& operand) {
                const std::uint32_t producer =
                    slice_project_build_ir_node(base_operand_sources, operand.expression);
                slice_project_note_source_binding(
                    base_operand_sources, operand.expression, producer);
                for (Var* source : postgres_list<Var>(operand.sequence_vars)) {
                    append_support_var(source);
                }
            };
            register_operand(owned_mmsearch_resolution->target);
            if (owned_mmsearch_resolution->local_query.has_value()) {
                register_operand(*owned_mmsearch_resolution->local_query);
            }
            /* Residual quals execute before operand ExprStates.  Discover them from
             * the full baserestrictinfo + parameterized-clause set, not allClauses:
             * FilterPushdownClauses deliberately omits executor-only quals, while
             * PlanCustomPath installs those full residuals in scan.plan.qual. */
            List* executable_residual_restrictions =
                restrictions_without_owned_mmsearch(clause_references.get(),
                                                    owned_mmsearch_restrictions,
                                                    /*restrictions_are_private_copies=*/false);
            constexpr int pull_flags =
                PVC_RECURSE_AGGREGATES | PVC_RECURSE_WINDOWFUNCS | PVC_RECURSE_PLACEHOLDERS;
            for (RestrictInfo* residual :
                 postgres_list<RestrictInfo>(executable_residual_restrictions)) {
                if (residual == nullptr || residual->clause == nullptr)
                    continue;
                List* variables =
                    pull_var_clause(reinterpret_cast<Node*>(residual->clause), pull_flags);
                for (Var* variable : postgres_list<Var>(variables)) {
                    if (variable == nullptr || variable->varno != rel->relid ||
                        variable->varattno <= 0 || variable->varlevelsup != 0 ||
                        !bms_is_empty(variable->varnullingrels) ||
                        variable->vartype != stub_oids.bioseq_seq_type_oid)
                        continue;
                    append_support_var(variable);
                    bool already_bound = false;
                    for (auto const& binding : base_operand_sources.source_bindings) {
                        if (equal(binding.first, variable)) {
                            already_bound = true;
                            break;
                        }
                    }
                    if (!already_bound) {
                        base_operand_sources.source_bindings.emplace_back(
                            reinterpret_cast<Node*>(copyObject(variable)), 0u);
                    }
                }
                list_free(variables);
            }
            list_free(executable_residual_restrictions);
        }

        /*
         * Plain clauses may contain extern params, but not exec params, and can
         * be evaluated at init time or rescan time. Track them in another list
         * that is a subset of allClauses.
         *
         * Note: although typically baserestrictinfo contains plain clauses,
         * that's not always true. It can also contain a qual referencing a Var at
         * a higher query level, which can be turned into an exec param, and
         * therefore it won't be a plain clause.
         */
        postgres_list<RestrictInfo> plain_clauses;
        for (auto rinfo : all_clauses) {
            if (bms_is_subset(rinfo->required_relids, rel->relids) &&
                !contains_exec_params(reinterpret_cast<Node*>(rinfo->clause))) {
                plain_clauses.append(rinfo);
            }
        }

        /*
         * We can't make our own CustomPath structure, so we need to put
         * everything in the custom_private list. To keep the two lists separate,
         * we make them sublists in a 2-element list.
         */
        if (bioseq::settings::planner::qual_pushdown::enabled) {
            cpath->custom_private =
                list_make2(copyObject(plain_clauses.get()), copyObject(all_clauses.get()));
        } else {
            cpath->custom_private = list_make2(NIL, NIL);
        }
        if (owned_mmsearch.restriction != nullptr) {
            cpath->custom_restrictinfo = list_copy(owned_mmsearch_restrictions);
            cpath->custom_private = lappend(cpath->custom_private,
                                            list_make3(makeInteger(k_bioseq_mmsearch_owner_magic),
                                                       makeInteger(k_bioseq_mmsearch_owner_version),
                                                       copyObject(owned_mmsearch_exprs)));
        } else {
            cpath->custom_restrictinfo = NIL;
        }

        int number_of_columns_read = 0;
        if (rte->perminfoindex > 0) {
            /*
             * If perminfoindex > 0, that means that this relation's permission info
             * is directly found in the list of rteperminfos of the Query(root->parse)
             * So, all we have to do here is retrieve that info.
             */
            RTEPermissionInfo* perminfo = getRTEPermissionInfo(root->parse->rteperminfos, rte);
            number_of_columns_read = bms_num_members(perminfo->selectedCols);
        } else {
            /*
             * If perminfoindex = 0, that means we are skipping the check for permission info
             * for this relation, which means that it's either a partition or an inheritance child.
             * In these cases, we need to access the permission info of the top parent of this
             * relation. After thorough checking, we found that the index of the top parent pointing
             * to the correct range table entry in Query's range tables (root->parse->rtable) is
             * found under RelOptInfo rel->top_parent->relid. For reference, check
             * expand_partitioned_rtentry and expand_inherited_rtentry PG functions
             */
            Assert(rel->top_parent);
            RangeTblEntry* parent_rte = rt_fetch(rel->top_parent->relid, root->parse->rtable);
            RTEPermissionInfo* perminfo =
                getRTEPermissionInfo(root->parse->rteperminfos, parent_rte);
            number_of_columns_read = bms_num_members(
                fixup_inherited_columns(perminfo->relid, rte->relid, perminfo->selectedCols));
        }
        int number_of_clauses_pushed = all_clauses.size();

        /* Queries that contain only aggregate with STAR doesn't have any
         * selectedCols so we should consider at least one column that needs
         * read (for better cost calculation).
         */
        if (number_of_columns_read == 0)
            number_of_columns_read = 1;

        cost_bioseq_column_scan(
            root, rel, rte->relid, cpath, number_of_columns_read, number_of_clauses_pushed);

        for (FuncExpr* owned_expr : postgres_list<FuncExpr>(owned_mmsearch_exprs)) {
            QualCost operand_cost{};
            List* operand_expressions = mmsearch_operand_cost_exprs(owned_expr, stub_oids);
            cost_qual_eval(&operand_cost, operand_expressions, root);
            postgres_list<Node> operands(owned_expr->args);
            const std::uint64_t target_symbols =
                estimate_mmsearch_operand_symbols(operands.get_first(), stub_oids);
            const std::uint64_t query_symbols =
                estimate_mmsearch_operand_symbols(operands.get_second(), stub_oids);
            const std::uint64_t admitted_bytes =
                bioseq::common::saturating_add(target_symbols, query_symbols);
            bioseq::simd::minimap2_parameters planned_params = build_params_from_gucs();
            bioseq::indexing::index_read_requirement planned_requirement =
                bioseq::indexing::index_read_requirement::allow;
            (void)apply_aligner_options_override(
                owned_expr, planned_params, planned_requirement, stub_oids);
            if (owned_mmsearch_resolution.has_value() &&
                owned_mmsearch_resolution->any_evaluated_operand) {
                planned_requirement = bioseq::indexing::index_read_requirement::require_absent;
            }
            const owned_mmsearch_cost alignment = estimate_owned_mmsearch_cost(
                target_symbols,
                query_symbols,
                path->rows,
                planned_requirement != bioseq::indexing::index_read_requirement::require_absent,
                static_cast<std::uint32_t>(std::min<std::size_t>(
                    k_bioseq_runtime_buffer_reserve, (std::numeric_limits<std::uint32_t>::max)())),
                admitted_bytes);
            const Cost owner_startup = std::max<Cost>(0, operand_cost.startup + alignment.startup);
            const Cost owner_per_row =
                std::max<Cost>(0, operand_cost.per_tuple + alignment.per_input_tuple);
            path->startup_cost += owner_startup;
            path->total_cost += owner_startup + owner_per_row * std::max(0.0, path->rows);
        }
        if (!std::isfinite(path->startup_cost) || path->startup_cost < 0) {
            path->startup_cost = 0;
        }
        if (!std::isfinite(path->total_cost)) {
            path->total_cost = std::numeric_limits<Cost>::max() / 4.0;
        }
        path->total_cost = std::max(path->startup_cost, path->total_cost);

        bioseq_report_pg_lazy(
            planner_debug_level(), [rte, root, param_relids, number_of_clauses_pushed] {
                StringInfoData buf;
                initStringInfo(&buf);
                bioseq_report_pg(planner_debug_level())
                    .with_message("columnar planner: adding CustomScan path for {}",
                                  rte->eref->aliasname)
                    .with_detail("{}; {} clauses pushed down",
                                 parameterization_as_string(root, param_relids, &buf),
                                 number_of_clauses_pushed);
            });

        if (needs_evaluated_operands) {
            Path* private_path = slice_project_clone_mutable_path_spine(path);
            bool tagged_source = false;
            private_path = slice_project_tag_owned_path_tree(
                private_path, base_operand_sources, tagged_source);
            if (!tagged_source) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("BioseqScan evaluated mmsearch operand has no source provider")));
            }
            return private_path;
        }
        return path;
    }

    /*
     * AdjustBioseqColumnParallelScanCost calculates path cost based on
     * number of parallel workers (based on postgres code).
     */
    void adjust_bioseq_column_parallel_scan_cost(Path* path) {
        /* Adjust costing for parallelism, if used. */
        if (path->parallel_workers > 0) {
            double parallel_divisor = path->parallel_workers;

            if (parallel_leader_participation) {
                double leader_contribution;

                leader_contribution = 1.0 - (0.3 * path->parallel_workers);
                if (leader_contribution > 0)
                    parallel_divisor += leader_contribution;
            }

            /* The CPU cost is divided among all the workers. */
            path->total_cost /= parallel_divisor;

            path->rows = clamp_row_est(path->rows / parallel_divisor);
        }
    }

    /*
     * CostBioseqColumnScan calculates the cost of scanning the columnar table. The
     * cost is estimated by using all stripe metadata to estimate based on the
     * columns to read how many pages need to be read.
     */
    void cost_bioseq_column_scan(PlannerInfo* root,
                                 RelOptInfo* rel,
                                 Oid relation_id,
                                 CustomPath* cpath,
                                 int number_of_columns_read,
                                 int n_clauses) {
        Path* path = &cpath->path;

        List* all_clauses = postgres_list<List>(cpath->custom_private).get_second();
        Selectivity clause_sel =
            clauselist_selectivity(root, all_clauses, rel->relid, JOIN_INNER, nullptr);

        /*
         * We already filtered out clauses where the overall selectivity would be
         * misleading, such as inequalities involving an uncorrelated column. So
         * we can apply the selectivity directly to the number of stripes.
         */
        double stripes_to_read = clause_sel * bioseq_column_table_stripe_count(relation_id);
        stripes_to_read = std::max(stripes_to_read, 1.0);

        path->rows = rel->tuples;
        path->startup_cost = enable_seqscan ? 0 : disable_cost;
        path->total_cost =
            path->startup_cost + stripes_to_read * bioseq_column_per_stripe_scan_cost(
                                                       rel, relation_id, number_of_columns_read);
    }

    /*
     * BioseqColumnPerStripeScanCost calculates the cost to scan a single stripe
     * of given columnar table based on number of columns that needs to be
     * read during scan operation.
     */
    Cost bioseq_column_per_stripe_scan_cost(RelOptInfo* rel,
                                            Oid relation_id,
                                            int number_of_columns_read) {
        Relation relation = RelationIdGetRelation(relation_id);
        List* stripe_list = list_stripes_for_relation(relation->rd_locator, ForwardScanDirection);
        RelationClose(relation);

        uint32_t max_column_count = 0;
        uint64_t total_stripe_size = 0;
        for (auto stripe_metadata : postgres_list<stripe_metadata>(stripe_list)) {
            total_stripe_size += stripe_metadata->data_length;
            max_column_count = std::max(max_column_count, stripe_metadata->column_count);
        }

        /*
         * When no stripes are in the table we don't have a count in maxColumnCount. To
         * prevent a division by zero turning into a NaN we keep the ratio on zero.
         * This will result in a cost of 0 for scanning the table which is a reasonable
         * cost on an empty table.
         */
        if (max_column_count == 0) {
            return 0;
        }

        double column_selection_ratio =
            number_of_columns_read / static_cast<double>(max_column_count);
        Cost table_scan_cost =
            static_cast<double>(total_stripe_size) / BLCKSZ * column_selection_ratio;
        Cost per_stripe_scan_cost = table_scan_cost / list_length(stripe_list);

        /*
         * Finally, multiply the cost of reading a single stripe by seq page read
         * cost to make our estimation scale compatible with postgres.
         * Since we are calculating the cost for a single stripe here, we use seq
         * page cost instead of random page cost. This is because, random page
         * access only happens when switching between columns, which is pretty
         * much neglactable.
         */
        double rel_space_seq_page_cost;
        get_tablespace_page_costs(rel->reltablespace, nullptr, &rel_space_seq_page_cost);
        per_stripe_scan_cost = per_stripe_scan_cost * rel_space_seq_page_cost;

        return per_stripe_scan_cost;
    }

    /*
     * BioseqColumnTableStripeCount returns the number of stripes that columnar
     * table with relationId has by using stripe metadata.
     */
    uint64_t bioseq_column_table_stripe_count(Oid relation_id) {
        Relation relation = RelationIdGetRelation(relation_id);
        List* stripe_list = list_stripes_for_relation(relation->rd_locator, ForwardScanDirection);
        int stripe_count = list_length(stripe_list);
        RelationClose(relation);

        return stripe_count;
    }

} // export custom-scan partition declarations
