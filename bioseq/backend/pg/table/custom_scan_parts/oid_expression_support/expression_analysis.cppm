/**
 * @file
 * @brief Analyzes volatility, mmsearch calls, and coarse operand shape.
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

export module bioseq.postgres.table:custom_scan_oid_expression_support_expression_analysis;

import std;
import bioseq.postgres.support;

import :custom_scan_oid_expression_support_oid_catalog;
export import :custom_scan_oid_expression_support_expression_analysis_contracts;

using namespace bioseq::postgres::support;

export {
    /** @brief Carries cached stub OIDs through the raw-query validation walk. */
    struct raw_mmsearch_options_validation {
        const bioseq_stub_oids* oids = nullptr;
    };

    /** @brief Tests whether a procedure is a volatile SQL-language function. */
    [[nodiscard]] inline bool is_volatile_sql_function(const Oid function_oid) {
        HeapTuple procedure_tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(function_oid));
        if (!HeapTupleIsValid(procedure_tuple))
            return false;
        const Form_pg_proc procedure = reinterpret_cast<Form_pg_proc>(GETSTRUCT(procedure_tuple));
        const bool result =
            procedure->provolatile == PROVOLATILE_VOLATILE && procedure->prolang == SQLlanguageId;
        ReleaseSysCache(procedure_tuple);
        return result;
    }

    /** @brief Visits expression nodes while delegating nested queries to the query walker. */
    inline bool contains_volatile_sql_function_node(Node* node, void* context);

    /** @brief Searches a query tree and its range-table expressions for volatile SQL calls. */
    [[nodiscard]] inline bool contains_volatile_sql_function_query(Query* query, void* context) {
        return query != nullptr &&
               query_tree_walker(
                   query, contains_volatile_sql_function_node, context, QTW_EXAMINE_RTES_BEFORE);
    }

    /** @brief Searches one expression subtree for a volatile SQL-language call. */
    [[nodiscard]] inline bool contains_volatile_sql_function_node(Node* node, void* context) {
        if (node == nullptr)
            return false;
        if (FuncExpr* expression = try_cast_pg_node<FuncExpr>(node);
            expression != nullptr && is_volatile_sql_function(expression->funcid))
            return true;
        if (IsA(node, RangeTblEntry)) {
            /* query_tree_walker owns traversal of RTE expressions and subqueries. */
            return false;
        }
        if (Query* query = try_cast_pg_node<Query>(node)) {
            return contains_volatile_sql_function_query(query, context);
        }
        return expression_tree_walker(node, contains_volatile_sql_function_node, context);
    }

    /** @brief Detects volatile SQL calls without evaluating the supplied expression. */
    [[nodiscard]] inline bool contains_volatile_sql_function(Node* expression) {
        return contains_volatile_sql_function_node(expression, nullptr);
    }

    /** @brief Validates mmsearch option expressions while walking non-query nodes. */
    inline bool validate_raw_mmsearch_options_node(Node* node,
                                                   raw_mmsearch_options_validation* validation);

    /** @brief Validates raw mmsearch options throughout one query and its nested queries. */
    inline void validate_raw_mmsearch_options_query(Query* query,
                                                    raw_mmsearch_options_validation* validation) {
        if (query == nullptr) {
            return;
        }
        (void)query_tree_walker(
            query, validate_raw_mmsearch_options_node, validation, QTW_EXAMINE_RTES_BEFORE);
    }

    /**
     * @brief Rejects volatile options on a three-argument mmsearch expression.
     * @throws PostgreSQL ERROR when a volatile SQL wrapper occurs in the options argument.
     */
    inline bool validate_raw_mmsearch_options_node(Node* node,
                                                   raw_mmsearch_options_validation* validation) {
        if (node == nullptr) {
            return false;
        }
        if (FuncExpr* expression = try_cast_pg_node<FuncExpr>(node);
            expression != nullptr && is_mmsearch_funcid(expression->funcid, *validation->oids) &&
            expression->funcid == validation->oids->mmsearch_with_opts_oid &&
            list_length(expression->args) == 3) {
            Node* options = reinterpret_cast<Node*>(list_nth(expression->args, 2));
            if (contains_volatile_sql_function(options)) {
                ereport(
                    ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("mmsearch qualifier is not owned by a Bioseq executor"),
                     errdetail("Volatile aligner options cannot be reduced to a statement-stable "
                               "executor-owned value."),
                     errhint(
                         "Pass a constant bioseq_aligner_options value as the third argument.")));
            }
        }
        if (IsA(node, RangeTblEntry)) {
            /* query_tree_walker owns traversal of RTE expressions and subqueries. */
            return false;
        }
        if (IsA(node, Query)) {
            validate_raw_mmsearch_options_query(castNode(Query, node), validation);
            return false;
        }
        return expression_tree_walker(node, validate_raw_mmsearch_options_node, validation);
    }

    /** @brief Reject volatile raw options before planner simplification removes their provenance.
     *
     * Called by the top-level planner hook while the analyzed Query still retains
     * SQL-function volatility.  standard_planner simplification may erase that
     * provenance by replacing the wrapper with its constant body. */
    void bioseq_validate_raw_mmsearch_options(Query* query) {
        const bioseq_stub_oids* oids = try_get_cached_stub_oids();
        if (query == nullptr || oids == nullptr) {
            return;
        }
        raw_mmsearch_options_validation validation{oids};
        validate_raw_mmsearch_options_query(query, &validation);
    }

    /** @brief Carries stub OIDs through the mmsearch-presence expression walk. */
    struct mmsearch_qualifier_walker {
        const bioseq_stub_oids* oids = nullptr;
    };

    /** @brief Stops an expression walk at the first mmsearch call. */
    struct mmsearch_qualifier_visitor {
        /** @brief Classifies the current node for the mmsearch-presence walk. */
        [[nodiscard]] pg_walk_step operator()(pg_expression_walk_cursor,
                                              pg_node_view node,
                                              mmsearch_qualifier_walker& data) const {
            if (FuncExpr* fe = node.try_cast<FuncExpr>()) {
                if (is_mmsearch_funcid(fe->funcid, *data.oids)) {
                    return pg_walk_step::stop;
                }
            }
            return pg_walk_step::descend;
        }
    };

    /** @brief Tests whether an expression tree contains an mmsearch call. */
    [[nodiscard]] bool contains_mmsearch_qualifier(Node* node, const bioseq_stub_oids& oids) {
        mmsearch_qualifier_walker data{&oids};
        mmsearch_qualifier_visitor visitor;
        return walk_pg_expression_including_root(node, data, visitor);
    }

    /** @brief Accumulates the number of mmsearch calls in an expression tree. */
    struct mmsearch_occurrence_counter {
        const bioseq_stub_oids* oids = nullptr;
        std::size_t count = 0;
    };

    /** @brief Counts every mmsearch node while descending through an expression tree. */
    struct mmsearch_occurrence_visitor {
        /** @brief Records a call at the current node and continues traversal. */
        [[nodiscard]] pg_walk_step operator()(pg_expression_walk_cursor,
                                              pg_node_view node,
                                              mmsearch_occurrence_counter& data) const {
            if (FuncExpr* fe = node.try_cast<FuncExpr>();
                fe != nullptr && is_mmsearch_funcid(fe->funcid, *data.oids)) {
                ++data.count;
            }
            return pg_walk_step::descend;
        }
    };

    /** @brief Counts mmsearch calls, including nested occurrences and the root node. */
    [[nodiscard]] std::size_t count_mmsearch_occurrences(Node* node, const bioseq_stub_oids& oids) {
        mmsearch_occurrence_counter data{&oids, 0};
        mmsearch_occurrence_visitor visitor;
        walk_pg_expression_including_root(node, data, visitor);
        return data.count;
    }

    /**
     * @brief Applies the inexpensive base-local shape filter used before full operand validation.
     *
     * Volatile subexpressions remain admissible because the authoritative expression state
     * evaluates them at the original base-relation frontier.
     */
    [[nodiscard]] bool coarse_base_mmsearch_operand_candidate(Node* expression,
                                                              RelOptInfo* rel,
                                                              const bioseq_stub_oids& oids,
                                                              const bool require_sequence_var) {
        if (expression == nullptr || rel == nullptr ||
            exprType(expression) != oids.bioseq_seq_type_oid ||
            expression_returns_set(expression) || contain_subplans(expression))
            return false;
        constexpr int pull_flags =
            PVC_RECURSE_AGGREGATES | PVC_RECURSE_WINDOWFUNCS | PVC_RECURSE_PLACEHOLDERS;
        List* variables = pull_var_clause(expression, pull_flags);
        bool saw_sequence_var = false;
        bool valid = true;
        for (Var* variable : postgres_list<Var>(variables)) {
            if (variable == nullptr || !bioseq_is_concrete_rte_varno(variable->varno) ||
                variable->varno != rel->relid || variable->varattno <= 0 ||
                variable->varlevelsup != 0 || !bms_is_empty(variable->varnullingrels)) {
                valid = false;
                break;
            }
            if (variable->vartype == oids.bioseq_seq_type_oid) {
                saw_sequence_var = true;
            }
        }
        list_free(variables);
        return valid && (!require_sequence_var || saw_sequence_var);
    }

} // export custom-scan partition declarations
