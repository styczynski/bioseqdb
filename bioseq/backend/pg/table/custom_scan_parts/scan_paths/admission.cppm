/**
 * @file
 * @brief Estimates scan costs and selects filters for columnar scan paths.
 */
module;
#include <bioseq/pg_compat.h>

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

export import :custom_scan_scan_paths_admission_contracts;
import std;
import bioseq.common.logging;
import bioseq.postgres.configuration.core;
import bioseq.postgres.storage.metadata;
import bioseq.postgres.support;
import bioseq.postgres.table.types;

import :custom_scan_join_ownership;
import :custom_scan_oid_expression_support;
import :custom_scan_scan_paths_cost_finalization;
import :custom_scan_semantic_payload;

using namespace bioseq::postgres::support;

export {

    /** @brief Removes matching paths from both complete and partial path lists. */
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

    /** @brief Reports whether a planner path is not an IndexPath. */
    bool is_not_index_path(Path* path) {
        return !pg_node_view{to_pg_node(path)}.is_type<IndexPath>();
    }

    /** @brief Returns the number of physical attributes in a relation descriptor. */
    int relation_id_get_number_of_attributes(Oid relation_id) {
        Relation relation = RelationIdGetRelation(relation_id);
        int nattrs = relation->rd_att->natts;
        RelationClose(relation);
        return nattrs;
    }

    /** @brief Estimates sequential-scan cost for reading every stored column and stripe. */
    void cost_bioseq_column_seq_path(RelOptInfo* rel, Oid relation_id, Path* path) {
        if (!enable_seqscan) {
            /* Keep PostgreSQL's cost for a disabled sequential scan. */
            return;
        }

        /* Ordinary SeqScan reads all columns and stripes. Column selection
         * and stripe pruning belong to the Bioseq custom scan. */
        double stripes_to_read = bioseq_column_table_stripe_count(relation_id);
        int number_of_columns_read = relation_id_get_number_of_attributes(relation_id);

        path->rows = rel->tuples;
        path->startup_cost = 0;
        path->total_cost = stripes_to_read * bioseq_column_per_stripe_scan_cost(
                                                 rel, relation_id, number_of_columns_read);
    }

    /** @brief Creates and costs a nonparallel sequential path for a columnar relation. */
    Path* create_bioseq_column_seq_scan_path(PlannerInfo* root, RelOptInfo* rel, Oid relation_id) {
        bioseq_log<logging_level::debug>("[bioseq] pg: CreateBioseqColumnSeqScanPath() entered");
        /* Keep this ordinary SeqScan serial. Bioseq custom scans handle parallel reads. */
        int parallel_workers = 0;

        Relids required_outer = rel->lateral_relids;
        Path* path = create_seqscan_path(root, rel, required_outer, parallel_workers);
        cost_bioseq_column_seq_path(rel, relation_id, path);
        return path;
    }

    /** @brief Estimates the stripe reads needed to fetch rows found through an index. */
    Cost bioseq_column_index_scan_additional_cost(PlannerInfo* root,
                                                  RelOptInfo* rel,
                                                  Oid relation_id,
                                                  IndexPath* index_path) {
        bioseq_log<logging_level::debug>(
            "[bioseq] pg: BioseqColumnIndexScanAdditionalCost() entered");
        int number_of_columns_read = relation_id_get_number_of_attributes(relation_id);
        Cost per_stripe_cost =
            bioseq_column_per_stripe_scan_cost(rel, relation_id, number_of_columns_read);

        /* Request index selectivity and correlation from the index cost estimator.
         * Its cost outputs are unused here, so estimate a single pass. */
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

        /* With scattered matches, fetching each row may require another stripe read. */
        double max_stripe_read_count = estimated_rows;

        /* With matches clustered in storage order, each stripe read supplies
         * roughly one stripe's worth of matching rows. */
        double avg_stripe_row_count =
            row_count / static_cast<double>(bioseq_column_table_stripe_count(relation_id));
        double min_stripe_read_count = estimated_rows / avg_stripe_row_count;

        /* Both ascending and descending index order can keep nearby rows
         * together, so only the size of the correlation matters. */
        double abs_index_correlation = std::abs(index_correlation);

        /* Blend the clustered and scattered read estimates using correlation.
         * Poor correlation makes index access costly unless it selects very few rows. */
        double complement_index_correlation = 1 - abs_index_correlation;
        double estimated_stripe_read_count =
            min_stripe_read_count +
            complement_index_correlation * (max_stripe_read_count - min_stripe_read_count);

        /* Charge for at least one stripe read. */
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

    /** @brief Adds columnar tuple-fetch cost to an index path when configured. */
    void cost_bioseq_column_index_path(PlannerInfo* root,
                                       RelOptInfo* rel,
                                       Oid relation_id,
                                       IndexPath* index_path) {
        if (!enable_indexscan) {
            bioseq_log<logging_level::debug>(
                "[bioseq] pg: CostBioseqColumnIndexPath() skipped (enable_indexscan=false)");
            /* Keep PostgreSQL's cost for a disabled index scan. */
            return;
        }

        bioseq_log_pg(DEBUG4,
                      "columnar table index scan costs estimated by "
                      "indexAM: startup cost = {:.10f}, total cost = "
                      "{:.10f}",
                      index_path->path.startup_cost,
                      index_path->path.total_cost);

        /* Add stripe reads to the existing cost, which already accounts for
         * traversing the index. */

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

    /** @brief Re-costs sequential and index paths admitted for a columnar relation. */
    void cost_bioseq_column_paths(PlannerInfo* root, RelOptInfo* rel, Oid relation_id) {
        bioseq_log<logging_level::debug>("[bioseq] pg: CostBioseqColumnPaths() entered");
        for (auto path : postgres_list<Path>(rel->pathlist)) {
            if (pg_node_view{to_pg_node(path)}.is_type<IndexPath>()) {
                /* Bioseq has no bitmap-scan callbacks, so this branch only needs
                 * ordinary index paths. */
                cost_bioseq_column_index_path(
                    root, rel, relation_id, reinterpret_cast<IndexPath*>(path));
            } else if (path->pathtype == T_SeqScan) {
                cost_bioseq_column_seq_path(rel, relation_id, path);
            }
        }
    }

    /**
     * @brief Checks whether a column's storage order makes min/max pruning useful.
     *
     * Accepts the filter when correlation meets the configured threshold or statistics
     * are missing. On rejection, writes the absolute correlation to
     * @p abs_var_correlation if the caller provided it.
     */
    bool check_var_stats(PlannerInfo* root, Var* var, Oid sortop, float* abs_var_correlation) {
        bioseq_log<logging_level::debug>("[bioseq] pg: CheckVarStats() entered");
        /* Look up the column statistics, then its correlation with storage order. */
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

        /* Poor correlation spreads values across chunks and weakens min/max pruning. */
        if (std::abs(var_correlation) <
            bioseq::settings::planner::qual_pushdown::correlation_threshold) {
            if (abs_var_correlation) {
                /* Return the rejected correlation for the caller's diagnostic message. */
                *abs_var_correlation = std::abs(var_correlation);
            }
            return false;
        }

        return true;
    }

    /** @brief Reports whether an expression contains a Var belonging to a relation. */
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

    /* Pushdown selection narrows the search for parameterized scan paths.
     * It keeps useful parts of AND, requires a useful part from every OR arm,
     * and rejects NOT. For ordinary comparisons, one side must be a column
     * of this relation and the other must be independent of that relation.
     * Replacing an unusable condition with true explains which partial
     * filters remain safe. */
    /* Recognize mmsearch here so a join query can produce a parameterized
     * inner scan. For mmsearch(t1.seq, t2.seq), the t1 scan can receive t2.seq
     * as PARAM_EXEC and align it on each rescan. Keeping the call in the
     * candidate clauses lets the planner discover that outer dependency. */
    /** @brief Checks whether an mmsearch call is a candidate for this base scan. */
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
        /* The aligned target must come from this relation. In mmsearch(t1.seq,
         * t2.seq), t1 owns the target and t2 supplies the query. */
        if (col_var->varno != rel->relid) {
            return false;
        }
        if (col_var->varattno <= 0) {
            return false;
        }
        /* The executor accepts a literal, parameter, column or expression
         * evaluated from this scan's row as the query sequence. An outer column
         * becomes a nested-loop parameter during planning. */
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
        /* Keep a validated volatile query expression at this scan, where it
         * is evaluated once per admitted row. Reject other volatile query shapes. */
        if (contain_volatile_functions(arg1) && !local_query_recipe) {
            return false;
        }
        /* Require sequence types with known typmods here. The executor checks
         * alphabet compatibility and repacks a query with a different storage width
         * when it binds the operands. */
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
        /* Detailed alphabet and width checks happen when the executor binds
         * these operands. This test only requires their sequence type and typmod. */
        return true;
    }

    /** @brief Extracts filters worth considering inside this base scan, including mmsearch. */
    Expr* extract_pushdown_clause(PlannerInfo* root, RelOptInfo* rel, Node* node) {
        CHECK_FOR_INTERRUPTS();
        check_stack_depth();

        if (node == nullptr) {
            return nullptr;
        }

        /* Keep eligible mmsearch calls so an inner scan can receive its query
         * from an outer row through a nested-loop parameter. */
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
                /* Simple NOT conditions should already be simplified by PostgreSQL.
                 * Leave any remaining NOT expression outside the pushdown filter. */
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

                /* AND can still use its other filters when one argument is rejected. */
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
                /* NOT was handled before visiting the arguments. */
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

        /* Use the column type's default B-tree operator class for comparisons. */
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

        /* Reject the comparison if available statistics suggest poor stripe pruning. */
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

    /** @brief Copies restrictions and keeps the parts that can be used inside this base scan. */
    List* filter_pushdown_clauses(PlannerInfo* root, RelOptInfo* rel, List* input_clauses) {
        postgres_list<RestrictInfo> filtered_clauses;
        for (auto rinfo : postgres_list<RestrictInfo>(input_clauses)) {
            /* Skip pseudoconstants and clauses that do not depend on this relation. */
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

    /** @brief Accepts every equivalence member considered for implied join clauses. */
    bool pushdown_join_clause_matches(PlannerInfo* root,
                                      RelOptInfo* rel,
                                      EquivalenceClass* ec,
                                      EquivalenceMember* em,
                                      void* arg) {
        return true;
    }

    /** @brief Finds explicit and implied join restrictions eligible for pushdown. */
    List* find_pushdown_join_clauses(PlannerInfo* root, RelOptInfo* rel) {
        postgres_list<RestrictInfo> join_clauses{copyObject(rel->joininfo)};

        /* Generate implied equality clauses so pushdown checks can narrow
         * the outer relations considered for parameterized paths. */
        List* ec_clauses = generate_implied_equalities_for_column(
            root, rel, pushdown_join_clause_matches, nullptr, rel->lateral_referencers);
        join_clauses.concatenate(ec_clauses);

        return filter_pushdown_clauses(root, rel, join_clauses.release());
    }

    /**
     * @brief Finds additional outer relations that could supply scan parameters.
     *
     * Collects dependencies from admitted join clauses. Excludes this scan's relations,
     * already-required lateral dependencies and outer-join relation IDs.
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

    /**
     * @brief Estimates how many sets of @p k relations can be chosen from @p n.
     *
     * Used to limit planner path enumeration. Computes n choose k in floating point,
     * so large results can lose precision or become infinity. Uses the product and
     * symmetry identities from Knuth, TAoCP 1.2.6, equations (2) and (5).
     */
    double combinations(int n, int k) {
        double v = 1;

        /* Choosing k or n-k gives the same count. Use the smaller value to
         * reduce work and avoid needless overflow near n, such as choose(500, 500). */
        k = std::min(k, n - k);

        /* Multiply the descending numerator factors of n choose k. */
        for (int i = n; i >= n - k + 1; i--) {
            v *= i;
        }

        /* Divide by k factorial. The factor 1 needs no operation. */
        for (int i = k; i >= 2; i--) {
            v /= i;
        }

        return v;
    }

    /**
     * @brief Limits how many outer relations each parameterized path may depend on.
     *
     * Counts subsets from size zero upward and stops before their total exceeds
     * the configured path limit. Returns zero when qual pushdown is disabled,
     * leaving only the path with no additional outer dependencies.
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
