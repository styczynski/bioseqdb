/**
 * @file
 * @brief Selects the exact owned mmsearch restriction and residual quals.
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

export module bioseq.postgres.table:custom_scan_oid_expression_support_restriction_ownership;

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

import :custom_scan_runtime_support;

import :custom_scan_oid_expression_support_oid_catalog;
import :custom_scan_oid_expression_support_expression_analysis;

using namespace bioseq::postgres::support;

export {
    [[nodiscard]] bool bounded_batch_fallback_options_are_supported(FuncExpr* primary,
                                                                    FuncExpr* extra,
                                                                    const bioseq_stub_oids& oids);

    /*
     * Choose the bounded RestrictInfo set the custom scan is allowed to execute:
     * either one semantic call, or the narrow BatchJoin-fallback shape consisting
     * of an ordinary-relation outer Var primary plus one same-target Const base
     * filter.  The decision is deliberately made against the original planner
     * nodes: their pointer identities later select exactly which restrictions are
     * removed from executable quals and which competing paths
     * would execute them.
     */
    [[nodiscard]] owned_mmsearch_restriction
    choose_owned_mmsearch_restriction(PlannerInfo* root,
                                      RelOptInfo* rel,
                                      RangeTblEntry* rte,
                                      List* restrictions,
                                      List* ppi_restrictions,
                                      Relids param_relids,
                                      const bioseq_stub_oids& oids) {
        std::size_t semantic_calls = 0;
        List* semantic_restrictions = NIL;
        for (RestrictInfo* rinfo : postgres_list<RestrictInfo>(restrictions)) {
            const std::size_t in_clause =
                count_mmsearch_occurrences(reinterpret_cast<Node*>(rinfo->clause), oids);
            semantic_calls += in_clause;
            if (in_clause != 0)
                semantic_restrictions = lappend(semantic_restrictions, rinfo);
        }
        if (semantic_calls == 0 || semantic_restrictions == NIL) {
            return {};
        }
        if (semantic_calls != static_cast<std::size_t>(list_length(semantic_restrictions))) {
            /* More than one occurrence inside one Boolean expression is never the
             * narrow Param-primary/Const-extra ownership shape below. */
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("stacked mmsearch qualifiers are unsupported"),
                     errhint("Split the alignment predicates into separate query levels.")));
        }

        /* Fail closed under row-level security, security-barrier quals, leaky
         * ordering, pseudoconstants, or volatile siblings. */
        if (root == nullptr || root->parse == nullptr || root->parse->hasRowSecurity ||
            rte == nullptr || rte->securityQuals != NIL) {
            return {};
        }
        for (RestrictInfo* sibling : postgres_list<RestrictInfo>(restrictions)) {
            if (!list_member_ptr(semantic_restrictions, sibling) &&
                contain_volatile_functions(reinterpret_cast<Node*>(sibling->clause))) {
                return {};
            }
        }

        /* A parameterized baserel candidate can rendezvous restrictions from any
         * number of higher owners.  Provenance, not operand syntax, decides whether
         * this candidate may classify them together. */
        if (semantic_calls > 1u) {
            /* Walk PostgreSQL Lists directly.  Do not retain a std::vector (or any
             * other nontrivial C++ owner) across ereport(ERROR): PostgreSQL's longjmp
             * would skip its destructor in the backend. */
            for (ListCell* current_cell = list_head(semantic_restrictions); current_cell != nullptr;
                 current_cell = lnext(semantic_restrictions, current_cell)) {
                RestrictInfo* current = reinterpret_cast<RestrictInfo*>(lfirst(current_cell));
                FuncExpr* current_expression =
                    current == nullptr
                        ? nullptr
                        : try_cast_pg_node<FuncExpr>(reinterpret_cast<Node*>(current->clause));
                const bool exact_expression =
                    current_expression != nullptr &&
                    is_mmsearch_funcid(current_expression->funcid, oids) &&
                    ((current_expression->funcid == oids.mmsearch_oid &&
                      list_length(current_expression->args) == 2) ||
                     (current_expression->funcid == oids.mmsearch_with_opts_oid &&
                      list_length(current_expression->args) == 3));
                const bool current_base =
                    current != nullptr && list_member_ptr(rel->baserestrictinfo, current);
                const bool current_ppi =
                    current != nullptr && list_member_ptr(ppi_restrictions, current);
                if (!exact_expression || current_base == current_ppi ||
                    current->required_relids == nullptr) {
                    ereport(
                        ERROR,
                        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                         errmsg("stacked mmsearch qualifiers are unsupported"),
                         errhint("Split the alignment predicates into separate query levels.")));
                }

                for (ListCell* prior_cell = list_head(semantic_restrictions);
                     prior_cell != current_cell;
                     prior_cell = lnext(semantic_restrictions, prior_cell)) {
                    RestrictInfo* prior = reinterpret_cast<RestrictInfo*>(lfirst(prior_cell));
                    const bool prior_base = list_member_ptr(rel->baserestrictinfo, prior);
                    if (current_base == prior_base &&
                        bms_equal(current->required_relids, prior->required_relids)) {
                        ereport(
                            ERROR,
                            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                             errmsg("stacked mmsearch qualifiers are unsupported"),
                             errdetail("Two semantic mmsearch calls share one ownership frontier."),
                             errhint(
                                 "Split the alignment predicates into separate query levels.")));
                    }
                }
            }

            bool bounded_fallback_candidate = false;
            if (semantic_calls == 2u) {
                RestrictInfo* first =
                    reinterpret_cast<RestrictInfo*>(linitial(semantic_restrictions));
                RestrictInfo* second =
                    reinterpret_cast<RestrictInfo*>(lsecond(semantic_restrictions));
                const bool first_base = list_member_ptr(rel->baserestrictinfo, first);
                const bool second_base = list_member_ptr(rel->baserestrictinfo, second);
                if (first_base != second_base) {
                    RestrictInfo* base_restriction = first_base ? first : second;
                    RestrictInfo* ppi_restriction = first_base ? second : first;
                    FuncExpr* base_expression = try_cast_pg_node<FuncExpr>(
                        reinterpret_cast<Node*>(base_restriction->clause));
                    FuncExpr* ppi_expression = try_cast_pg_node<FuncExpr>(
                        reinterpret_cast<Node*>(ppi_restriction->clause));
                    Node* base_target = reinterpret_cast<Node*>(linitial(base_expression->args));
                    Node* base_query = reinterpret_cast<Node*>(lsecond(base_expression->args));
                    Node* ppi_target = reinterpret_cast<Node*>(linitial(ppi_expression->args));
                    Node* ppi_query = reinterpret_cast<Node*>(lsecond(ppi_expression->args));
                    Var* ppi_query_var = try_cast_pg_node<Var>(ppi_query);
                    Const* base_query_const = try_cast_pg_node<Const>(base_query);
                    bounded_fallback_candidate =
                        try_cast_pg_node<Var>(base_target) != nullptr &&
                        try_cast_pg_node<Var>(ppi_target) != nullptr &&
                        equal(base_target, ppi_target) && base_query_const != nullptr &&
                        base_query_const->consttype == oids.bioseq_seq_type_oid &&
                        ppi_query_var != nullptr && ppi_query_var->varlevelsup == 0 &&
                        ppi_query_var->varattno > 0 &&
                        ppi_query_var->vartype == oids.bioseq_seq_type_oid &&
                        bms_is_empty(ppi_query_var->varnullingrels) &&
                        bms_is_member(static_cast<int>(ppi_query_var->varno), param_relids);
                }
            }
            if (!bounded_fallback_candidate)
                return {};
        }

        RestrictInfo* primary_restriction = nullptr;
        FuncExpr* primary_expr = nullptr;
        RestrictInfo* constant_restriction = nullptr;
        FuncExpr* constant_expr = nullptr;
        for (RestrictInfo* semantic_restriction :
             postgres_list<RestrictInfo>(semantic_restrictions)) {
            /* Nested Boolean/CASE/NOT shapes are not equivalent to a consumed,
             * strict top-level predicate and therefore remain executable SQL quals. */
            FuncExpr* original =
                try_cast_pg_node<FuncExpr>(reinterpret_cast<Node*>(semantic_restriction->clause));
            if (original == nullptr || !is_mmsearch_funcid(original->funcid, oids) ||
                semantic_restriction->pseudoconstant || semantic_restriction->security_level != 0 ||
                !restriction_is_securely_promotable(semantic_restriction, rel))
                return {};
            const int arity = list_length(original->args);
            if ((original->funcid == oids.mmsearch_oid && arity != 2) ||
                (original->funcid == oids.mmsearch_with_opts_oid && arity != 3))
                return {};
            pg_first_two_node_arguments args =
                get_pg_first_two_arguments_if_arity_between(*original, 2, 3);
            Node* target_node = args.is_present() ? args.first_argument : nullptr;
            Var* target = try_cast_pg_node<Var>(target_node);
            const bool raw_target =
                target != nullptr && target->varlevelsup == 0 && target->varno == rel->relid &&
                target->varattno > 0 && bms_is_empty(target->varnullingrels) &&
                target->vartype == oids.bioseq_seq_type_oid && target->vartypmod >= 0;
            if (!raw_target &&
                !coarse_base_mmsearch_operand_candidate(target_node,
                                                        rel,
                                                        oids,
                                                        /*require_sequence_var=*/true))
                return {};

            Node* query = args.second_argument;
            Const* query_constant = try_cast_pg_node<Const>(query);
            const bool constant_query = query_constant != nullptr &&
                                        query_constant->consttype == oids.bioseq_seq_type_oid &&
                                        query_constant->consttypmod >= 0;
            bool parameterized_query = false;
            if (!constant_query) {
                if (Param* param = try_cast_pg_node<Param>(query)) {
                    parameterized_query =
                        param->paramtype == oids.bioseq_seq_type_oid &&
                        (param->paramkind == PARAM_EXEC || param->paramkind == PARAM_EXTERN) &&
                        (param->paramtypmod >= 0 || param->paramkind == PARAM_EXTERN);
                } else if (Var* query_var = try_cast_pg_node<Var>(query)) {
                    const bool concrete_varno = bioseq_is_concrete_rte_varno(query_var->varno);
                    const bool same_relation = concrete_varno && query_var->varno == rel->relid;
                    const bool available_outer =
                        concrete_varno &&
                        bms_is_member(static_cast<int>(query_var->varno), param_relids);
                    parameterized_query = query_var->varlevelsup == 0 && query_var->varattno > 0 &&
                                          query_var->vartype == oids.bioseq_seq_type_oid &&
                                          query_var->vartypmod >= 0 &&
                                          (same_relation || available_outer);
                    /* A same-relation per-row query remains a valid sole owner, but
                     * cannot be combined with a constant extra: the dispatcher has one
                     * dynamic primary slot and the bounded stacked contract is specifically
                     * the parameterized join case. */
                    if (same_relation && semantic_calls == 2) {
                        parameterized_query = false;
                    }
                } else {
                    /* A complete base-local query recipe (slice/concat/reverse,
                     * CASE/COALESCE, dynamic bounds, or an opaque effectful arm) is
                     * evaluated once per row after the target.  The later validation
                     * checks every Var/nulling frontier and derives its bound type. */
                    parameterized_query =
                        semantic_calls == 1 &&
                        coarse_base_mmsearch_operand_candidate(query,
                                                               rel,
                                                               oids,
                                                               /*require_sequence_var=*/false);
                }
            }
            if (!constant_query && !parameterized_query) {
                if (semantic_calls == 2) {
                    ereport(ERROR,
                            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                             errmsg("stacked mmsearch qualifiers are unsupported"),
                             errdetail("Only one parameterized mmsearch plus one constant filter "
                                       "on the same target column can share a Bioseq scan.")));
                }
                return {};
            }

            FuncExpr* normalized = copyObject(original);
            if (arity == 3) {
                Node* option = reinterpret_cast<Node*>(list_nth(normalized->args, 2));
                if (contain_volatile_functions(option))
                    return {};
                Node* folded = eval_const_expressions(root, option);
                Const* option_const = try_cast_pg_node<Const>(folded);
                if (option_const == nullptr ||
                    option_const->consttype != oids.bioseq_aligner_options_type_oid)
                    return {};
                lfirst(list_nth_cell(normalized->args, 2)) = folded;
            }
            if (constant_query) {
                if (constant_expr != nullptr) {
                    ereport(ERROR,
                            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                             errmsg("stacked mmsearch qualifiers are unsupported"),
                             errdetail("Two constant mmsearch filters cannot share one "
                                       "match_result owner.")));
                }
                constant_restriction = semantic_restriction;
                constant_expr = normalized;
            } else {
                if (primary_expr != nullptr) {
                    ereport(
                        ERROR,
                        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                         errmsg("stacked mmsearch qualifiers are unsupported"),
                         errdetail(
                             "Two dynamic mmsearch filters cannot share one match_result owner.")));
                }
                primary_restriction = semantic_restriction;
                primary_expr = normalized;
            }
        }

        if (semantic_calls == 1) {
            return primary_expr != nullptr
                       ? owned_mmsearch_restriction{primary_restriction, primary_expr, NIL, NIL}
                       : owned_mmsearch_restriction{constant_restriction, constant_expr, NIL, NIL};
        }
        Node* primary_target = primary_expr == nullptr
                                   ? nullptr
                                   : reinterpret_cast<Node*>(linitial(primary_expr->args));
        Node* constant_target = constant_expr == nullptr
                                    ? nullptr
                                    : reinterpret_cast<Node*>(linitial(constant_expr->args));
        Node* primary_query = primary_expr == nullptr || list_length(primary_expr->args) < 2
                                  ? nullptr
                                  : reinterpret_cast<Node*>(lsecond(primary_expr->args));
        Node* constant_query = constant_expr == nullptr || list_length(constant_expr->args) < 2
                                   ? nullptr
                                   : reinterpret_cast<Node*>(lsecond(constant_expr->args));
        Var* primary_query_var = try_cast_pg_node<Var>(primary_query);
        Const* constant_query_value = try_cast_pg_node<Const>(constant_query);
        RangeTblEntry* primary_query_rte =
            primary_query_var == nullptr ||
                    !bioseq_is_concrete_rte_varno(primary_query_var->varno) ||
                    primary_query_var->varno > static_cast<Index>(list_length(root->parse->rtable))
                ? nullptr
                : rt_fetch(primary_query_var->varno, root->parse->rtable);
        /* Preserve the established general stacked-qualifier rejection.  The sole
         * admitted exception is the BatchJoin fallback contract: an explicitly
         * require-absent primary over an outer Var from an ordinary relation becomes
         * PARAM_EXEC for a Bioseq target scan whose same target column also has one
         * non-NULL, default-options Const filter.  Both calls therefore share the
         * exact biological parameters, while require-absent is a valid execution of
         * the extra's default "allow" index policy.  A second Bioseq relation,
         * PARAM_EXTERN, same-row Var, view/subquery, security-qualified provider,
         * NULL extra, or independent option set remains unsupported; accepting those
         * would silently broaden the generic multi-owner surface beyond the one
         * executor shape that has an AND-stack runtime and integration contract. */
        const bool bounded_batch_fallback =
            primary_query_var != nullptr &&
            bms_is_member(static_cast<int>(primary_query_var->varno), param_relids) &&
            constant_query_value != nullptr && !constant_query_value->constisnull &&
            primary_query_rte != nullptr && primary_query_rte->rtekind == RTE_RELATION &&
            primary_query_rte->securityQuals == NIL && OidIsValid(primary_query_rte->relid) &&
            !relation_oid_uses_column_table_access_method(primary_query_rte->relid) &&
            bounded_batch_fallback_options_are_supported(primary_expr, constant_expr, oids);
        if (primary_restriction == nullptr || primary_expr == nullptr ||
            constant_restriction == nullptr || constant_expr == nullptr ||
            !equal(primary_target, constant_target) || !bounded_batch_fallback) {
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("stacked mmsearch qualifiers are unsupported"),
                     errdetail("Only one parameterized mmsearch plus one constant filter on the "
                               "same target column can share a Bioseq scan."),
                     errhint("Split the alignment predicates into separate query levels.")));
        }
        return owned_mmsearch_restriction{primary_restriction,
                                          primary_expr,
                                          list_make1(constant_restriction),
                                          list_make1(constant_expr)};
    }

    [[nodiscard]] List* restrictions_without_owned_mmsearch(List* restrictions,
                                                            List* owned,
                                                            bool restrictions_are_private_copies) {
        postgres_list<RestrictInfo> result;
        for (RestrictInfo* rinfo : postgres_list<RestrictInfo>(restrictions)) {
            bool is_owned = false;
            for (RestrictInfo* owner : postgres_list<RestrictInfo>(owned)) {
                if (rinfo == owner || (restrictions_are_private_copies && owner != nullptr &&
                                       equal(rinfo->clause, owner->clause))) {
                    is_owned = true;
                    break;
                }
            }
            if (is_owned)
                continue;
            result.append(rinfo);
        }
        return result.release();
    }

    /*
     * The owned mmsearch is removed before these storage quals are built. Keep a
     * defensive filter at the reader boundary so an unowned/nested rendezvous
     * expression can never be mistaken for a min/max pruning predicate.
     */
    [[nodiscard]] List* storage_prunable_quals_without_mmsearch(List* quals,
                                                                const bioseq_stub_oids& oids) {
        postgres_list<Node> filtered;
        for (auto qual : postgres_list<Node>(quals)) {
            if (qual == nullptr) {
                continue;
            }
            if (contains_mmsearch_qualifier(qual, oids)) {
                continue;
            }
            filtered.append(qual);
        }
        return filtered.release();
    }

} // export custom-scan partition declarations
