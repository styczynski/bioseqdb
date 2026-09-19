/**
 * @file
 * @brief Audits exact parameterized mmsearch owners and their planner ABI.
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

export module bioseq.postgres.table:custom_scan_join_ownership_parameterized_owner_analysis;

import std;
import bioseq.postgres.support;
import :custom_scan_oid_expression_support_oid_catalog;
import :custom_scan_oid_expression_support_expression_analysis;
import :custom_scan_planner_method_contracts;
import :custom_scan_join_ownership_path_owner_contracts;
export import :custom_scan_join_ownership_parameterized_owner_analysis_contracts;

using namespace bioseq::postgres::support;

export {
    /** @brief Encodes relation identifiers below 64 into a compact diagnostic mask. */
    [[nodiscard]] std::uint64_t exact_bmo_audit_relids_low(Relids relids) noexcept {
        std::uint64_t mask = 0u;
        int relid = -1;
        while ((relid = bms_next_member(relids, relid)) >= 0) {
            if (relid < 64)
                mask |= std::uint64_t{1} << relid;
        }
        return mask;
    }

    /**
     * @brief Checks whether two RestrictInfo nodes retain one PostgreSQL planner identity.
     *
     * PostgreSQL copies RestrictInfo nodes between baserel parameterization and
     * join-path construction while preserving rinfo_serial specifically so the
     * copies retain one planner identity.  Pointer equality alone therefore
     * misses the BMO path that owns the same qual.  Admit only that documented
     * clone relation plus an exact executable/security shape; equal SQL text with
     * a different serial remains a distinct restriction.
     */
    [[nodiscard]] bool same_planner_restriction_identity(RestrictInfo* candidate,
                                                         RestrictInfo* expected) noexcept {
        if (candidate == expected) {
            return candidate != nullptr && candidate->rinfo_serial > 0;
        }
        return candidate != nullptr && expected != nullptr && candidate->rinfo_serial > 0 &&
               expected->rinfo_serial > 0 && candidate->rinfo_serial == expected->rinfo_serial &&
               candidate->pseudoconstant == expected->pseudoconstant &&
               candidate->security_level == expected->security_level &&
               candidate->is_pushed_down == expected->is_pushed_down &&
               candidate->can_join == expected->can_join &&
               candidate->has_clone == expected->has_clone &&
               candidate->is_clone == expected->is_clone &&
               candidate->leakproof == expected->leakproof &&
               candidate->has_volatile == expected->has_volatile &&
               candidate->num_base_rels == expected->num_base_rels &&
               bms_equal(candidate->clause_relids, expected->clause_relids) &&
               bms_equal(candidate->required_relids, expected->required_relids) &&
               bms_equal(candidate->incompatible_relids, expected->incompatible_relids) &&
               bms_equal(candidate->outer_relids, expected->outer_relids) &&
               bms_equal(candidate->left_relids, expected->left_relids) &&
               bms_equal(candidate->right_relids, expected->right_relids) &&
               equal(candidate->orclause, expected->orclause) &&
               equal(candidate->clause, expected->clause);
    }

    /** @brief Marks owner analysis invalid and records one rejection reason. */
    inline void reject_exact_parameterized_bmo_owner(
        exact_parameterized_bmo_ownership& ownership,
        const exact_parameterized_bmo_owner_rejection reason) noexcept {
        ownership.invalid = true;
        ownership.rejection_mask |= static_cast<std::uint32_t>(reason);
    }

    /** @brief Validates a known BatchJoin owner that is foreign to the expected restriction. */
    [[nodiscard]] bool foreign_known_join_owner_is_valid(PlannerInfo* root, CustomPath* custom) {
        if (root == nullptr || custom == nullptr)
            return false;
        const bool raw_owner = custom->methods == bioseq_batch_join_path_methods();
        const bool transformed_owner = custom->methods == bioseq_per_row_join_path_methods();
        if (!raw_owner && !transformed_owner)
            return false;
        bool invalid_owner_abi = false;
        if (per_row_known_path_owner_recipe(custom, invalid_owner_abi) == nullptr ||
            invalid_owner_abi || custom->path.parent == nullptr)
            return false;

        FuncExpr* owned_expression = nullptr;
        std::size_t semantic_calls = 0u;
        for (RestrictInfo* owned : postgres_list<RestrictInfo>(custom->custom_restrictinfo)) {
            const std::size_t calls =
                owned == nullptr
                    ? 0u
                    : count_mmsearch_occurrences(reinterpret_cast<Node*>(owned->clause),
                                                 get_cached_stub_oids());
            semantic_calls += calls;
            if (calls != 0u) {
                if (calls != 1u || owned_expression != nullptr)
                    return false;
                owned_expression =
                    try_cast_pg_node<FuncExpr>(reinterpret_cast<Node*>(owned->clause));
                if (owned_expression == nullptr)
                    return false;
            }
        }
        if (semantic_calls != 1u || owned_expression == nullptr)
            return false;
        const int executable_index = raw_owner ? 8 : 6;
        Node* private_target = reinterpret_cast<Node*>(list_nth(custom->custom_private, 4));
        Node* private_query = reinterpret_cast<Node*>(list_nth(custom->custom_private, 5));
        FuncExpr* executable_expression =
            list_length(custom->custom_private) > executable_index
                ? try_cast_pg_node<FuncExpr>(
                      reinterpret_cast<Node*>(list_nth(custom->custom_private, executable_index)))
                : nullptr;
        if (executable_expression == nullptr ||
            !is_mmsearch_funcid(executable_expression->funcid, get_cached_stub_oids()) ||
            count_mmsearch_occurrences(private_target, get_cached_stub_oids()) != 0u ||
            count_mmsearch_occurrences(private_query, get_cached_stub_oids()) != 0u ||
            count_mmsearch_occurrences(reinterpret_cast<Node*>(executable_expression),
                                       get_cached_stub_oids()) != 1u)
            return false;
        FuncExpr* normalized_owned = copyObject(owned_expression);
        if (list_length(normalized_owned->args) == 3) {
            Node* option = reinterpret_cast<Node*>(list_nth(normalized_owned->args, 2));
            Node* folded =
                eval_const_expressions(root, reinterpret_cast<Node*>(copyObject(option)));
            Const* folded_const = try_cast_pg_node<Const>(folded);
            if (folded_const == nullptr ||
                folded_const->consttype != get_cached_stub_oids().bioseq_aligner_options_type_oid) {
                return false;
            }
            lfirst(list_nth_cell(normalized_owned->args, 2)) = folded;
        }
        if (!equal(normalized_owned, executable_expression))
            return false;

        List* variables = pull_var_clause(reinterpret_cast<Node*>(owned_expression),
                                          PVC_RECURSE_AGGREGATES | PVC_RECURSE_WINDOWFUNCS |
                                              PVC_RECURSE_PLACEHOLDERS);
        for (Node* variable : postgres_list<Node>(variables)) {
            Var* var = try_cast_pg_node<Var>(variable);
            if (var == nullptr || !bioseq_is_concrete_rte_varno(var->varno) ||
                var->varlevelsup != 0 || var->varattno <= 0 || !bms_is_empty(var->varnullingrels) ||
                !bms_is_member(static_cast<int>(var->varno), custom->path.parent->relids))
                return false;
        }

        bool children_cover_parent = raw_owner ? list_length(custom->custom_paths) == 2
                                               : list_length(custom->custom_paths) == 1;
        if (children_cover_parent && raw_owner) {
            Path* first = reinterpret_cast<Path*>(linitial(custom->custom_paths));
            Path* second = reinterpret_cast<Path*>(lsecond(custom->custom_paths));
            children_cover_parent = first != nullptr && second != nullptr &&
                                    first->parent != nullptr && second->parent != nullptr &&
                                    !bms_overlap(first->parent->relids, second->parent->relids);
            Relids child_union = children_cover_parent
                                     ? bms_union(first->parent->relids, second->parent->relids)
                                     : nullptr;
            children_cover_parent =
                children_cover_parent && bms_equal(child_union, custom->path.parent->relids);
            bms_free(child_union);
        } else if (children_cover_parent) {
            Path* child = reinterpret_cast<Path*>(linitial(custom->custom_paths));
            children_cover_parent = child != nullptr && child->parent != nullptr &&
                                    bms_equal(child->parent->relids, custom->path.parent->relids);
        }
        return children_cover_parent && !custom->path.parallel_aware &&
               !custom->path.parallel_safe && custom->path.parallel_workers == 0 &&
               std::isfinite(custom->path.rows) && custom->path.rows >= 0 &&
               std::isfinite(custom->path.startup_cost) && custom->path.startup_cost >= 0 &&
               std::isfinite(custom->path.total_cost) &&
               custom->path.total_cost >= custom->path.startup_cost;
    }

    /** @brief Recursively counts exact owners while rejecting unmodeled executable path state. */
    void count_exact_parameterized_bmo_owners(PlannerInfo* root,
                                              Path* path,
                                              RestrictInfo* restriction,
                                              exact_parameterized_bmo_ownership& ownership,
                                              const bool admit_known_wrapper_owner) {
        if (root == nullptr || path == nullptr || restriction == nullptr || ownership.invalid)
            return;
        check_stack_depth();
        if (admit_known_wrapper_owner) {
            for (Node* expression :
                 postgres_list<Node>(path->pathtarget == nullptr ? NIL : path->pathtarget->exprs)) {
                if (count_mmsearch_occurrences(expression, get_cached_stub_oids()) != 0u) {
                    reject_exact_parameterized_bmo_owner(ownership, k_exact_bmo_nested_join_clause);
                    return;
                }
            }
            for (RestrictInfo* parameter_clause : postgres_list<RestrictInfo>(
                     path->param_info == nullptr ? NIL : path->param_info->ppi_clauses)) {
                if (parameter_clause == nullptr ||
                    count_mmsearch_occurrences(reinterpret_cast<Node*>(parameter_clause->clause),
                                               get_cached_stub_oids()) == 0u)
                    continue;
                bool owned_here = false;
                if (IsA(path, CustomPath)) {
                    for (RestrictInfo* owned : postgres_list<RestrictInfo>(
                             reinterpret_cast<CustomPath*>(path)->custom_restrictinfo)) {
                        owned_here = owned_here ||
                                     same_planner_restriction_identity(owned, parameter_clause);
                    }
                }
                if (!owned_here) {
                    reject_exact_parameterized_bmo_owner(ownership, k_exact_bmo_nested_join_clause);
                    return;
                }
            }
        }
        if (IsA(path, CustomPath)) {
            auto* custom = reinterpret_cast<CustomPath*>(path);
            if (custom->methods == bioseq_column_scan_path_methods() &&
                custom->custom_paths != NIL) {
                /* A BioseqColumnScan is a leaf.  Children would make the owner count
                 * incomplete and therefore invalidate this internal path ABI. */
                reject_exact_parameterized_bmo_owner(ownership, k_exact_bmo_base_scan_has_children);
                return;
            }
            bool owns_expected_restriction = false;
            for (RestrictInfo* owned : postgres_list<RestrictInfo>(custom->custom_restrictinfo)) {
                owns_expected_restriction = owns_expected_restriction ||
                                            same_planner_restriction_identity(owned, restriction);
            }
            if (admit_known_wrapper_owner && custom->methods == bioseq_column_scan_path_methods()) {
                bool invalid_owner_abi = false;
                Node* recipe = per_row_base_scan_owner_recipe(custom, invalid_owner_abi);
                if (custom->custom_restrictinfo == NIL) {
                    if (recipe != nullptr || invalid_owner_abi) {
                        reject_exact_parameterized_bmo_owner(ownership, k_exact_bmo_owner_abi);
                        return;
                    }
                } else {
                    if (recipe == nullptr || invalid_owner_abi) {
                        reject_exact_parameterized_bmo_owner(ownership, k_exact_bmo_owner_abi);
                        return;
                    }
                    for (RestrictInfo* owned :
                         postgres_list<RestrictInfo>(custom->custom_restrictinfo)) {
                        if (owned == nullptr ||
                            count_mmsearch_occurrences(reinterpret_cast<Node*>(owned->clause),
                                                       get_cached_stub_oids()) != 1u ||
                            try_cast_pg_node<FuncExpr>(reinterpret_cast<Node*>(owned->clause)) ==
                                nullptr) {
                            reject_exact_parameterized_bmo_owner(ownership, k_exact_bmo_owner_abi);
                            return;
                        }
                    }
                    if (custom->path.parallel_aware || custom->path.parallel_safe ||
                        custom->path.parallel_workers != 0 || !std::isfinite(custom->path.rows) ||
                        custom->path.rows < 0 || !std::isfinite(custom->path.startup_cost) ||
                        custom->path.startup_cost < 0 || !std::isfinite(custom->path.total_cost) ||
                        custom->path.total_cost < custom->path.startup_cost) {
                        reject_exact_parameterized_bmo_owner(ownership, k_exact_bmo_owner_abi);
                        return;
                    }
                }
                if (list_length(custom->custom_private) < 2) {
                    reject_exact_parameterized_bmo_owner(ownership, k_exact_bmo_owner_abi);
                    return;
                }
                for (int index = 0; index < 2; ++index) {
                    Node* residual_node =
                        reinterpret_cast<Node*>(list_nth(custom->custom_private, index));
                    if (residual_node != nullptr && !IsA(residual_node, List)) {
                        reject_exact_parameterized_bmo_owner(ownership, k_exact_bmo_owner_abi);
                        return;
                    }
                    for (RestrictInfo* residual : postgres_list<RestrictInfo>(
                             residual_node == nullptr ? NIL
                                                      : reinterpret_cast<List*>(residual_node))) {
                        if (residual != nullptr &&
                            count_mmsearch_occurrences(reinterpret_cast<Node*>(residual->clause),
                                                       get_cached_stub_oids()) != 0u) {
                            reject_exact_parameterized_bmo_owner(ownership,
                                                                 k_exact_bmo_nested_join_clause);
                            return;
                        }
                    }
                }
            }
            if (custom->methods == bioseq_column_scan_path_methods() && owns_expected_restriction) {
                std::size_t exact_restriction_count = 0u;
                for (RestrictInfo* owned :
                     postgres_list<RestrictInfo>(custom->custom_restrictinfo)) {
                    if (same_planner_restriction_identity(owned, restriction))
                        ++exact_restriction_count;
                }
                bool invalid_owner_abi = false;
                Node* recipe = per_row_base_scan_owner_recipe(custom, invalid_owner_abi);
                if (invalid_owner_abi || recipe == nullptr) {
                    reject_exact_parameterized_bmo_owner(ownership, k_exact_bmo_owner_abi);
                    return;
                }
                if (exact_restriction_count != 1u) {
                    reject_exact_parameterized_bmo_owner(ownership,
                                                         k_exact_bmo_restriction_multiplicity);
                    return;
                }
                if (custom->path.param_info == nullptr) {
                    reject_exact_parameterized_bmo_owner(ownership, k_exact_bmo_missing_param_info);
                    return;
                }
                std::size_t ppi_exact_count = 0u;
                for (RestrictInfo* parameter_clause :
                     postgres_list<RestrictInfo>(custom->path.param_info->ppi_clauses)) {
                    if (same_planner_restriction_identity(parameter_clause, restriction))
                        ++ppi_exact_count;
                }
                if (ppi_exact_count != 1u) {
                    reject_exact_parameterized_bmo_owner(
                        ownership,
                        ppi_exact_count == 0u ? k_exact_bmo_missing_ppi_clause
                                              : k_exact_bmo_restriction_multiplicity);
                    return;
                }
                ++ownership.matches;
                return;
            }
            if (custom->methods == bioseq_column_scan_path_methods())
                return;

            const bool known_join_owner = custom->methods == bioseq_batch_join_path_methods() ||
                                          custom->methods == bioseq_per_row_join_path_methods();
            const bool known_slice_wrapper =
                custom->methods == &bioseq_slice_project_path_methods();
            if (!known_join_owner && !known_slice_wrapper) {
                /* Unknown CustomPaths can hide restrictions or private children. */
                reject_exact_parameterized_bmo_owner(ownership, k_exact_bmo_unknown_custom_path);
                return;
            }
            if (admit_known_wrapper_owner && known_join_owner &&
                !foreign_known_join_owner_is_valid(root, custom)) {
                reject_exact_parameterized_bmo_owner(ownership, k_exact_bmo_known_wrapper_abi);
                return;
            }
            if (owns_expected_restriction) {
                ++ownership.wrapper_owns_expected;
                ownership.wrapper_method_mask |=
                    custom->methods == bioseq_batch_join_path_methods()
                        ? 1u
                        : (custom->methods == bioseq_per_row_join_path_methods()
                               ? 2u
                               : (known_slice_wrapper ? 4u : 0u));
                ownership.wrapper_restriction_count +=
                    static_cast<std::size_t>(list_length(custom->custom_restrictinfo));
                ownership.wrapper_child_count +=
                    static_cast<std::size_t>(list_length(custom->custom_paths));
                Node* private_version =
                    custom->custom_private == NIL
                        ? nullptr
                        : reinterpret_cast<Node*>(linitial(custom->custom_private));
                ownership.wrapper_version =
                    private_version != nullptr && IsA(private_version, Integer)
                        ? static_cast<std::size_t>(intVal(private_version))
                        : 0u;
                ownership.expected_rinfo_serial =
                    static_cast<std::size_t>(restriction->rinfo_serial);
                ownership.wrapper_parent_relids_low = exact_bmo_audit_relids_low(
                    custom->path.parent == nullptr ? nullptr : custom->path.parent->relids);
                if (list_length(custom->custom_paths) >= 1) {
                    Path* child = reinterpret_cast<Path*>(linitial(custom->custom_paths));
                    ownership.wrapper_child0_relids_low = exact_bmo_audit_relids_low(
                        child == nullptr || child->parent == nullptr ? nullptr
                                                                     : child->parent->relids);
                }
                if (list_length(custom->custom_paths) >= 2) {
                    Path* child = reinterpret_cast<Path*>(lsecond(custom->custom_paths));
                    ownership.wrapper_child1_relids_low = exact_bmo_audit_relids_low(
                        child == nullptr || child->parent == nullptr ? nullptr
                                                                     : child->parent->relids);
                }
                FuncExpr* expected_mmsearch =
                    try_cast_pg_node<FuncExpr>(reinterpret_cast<Node*>(restriction->clause));
                Node* expected_target =
                    expected_mmsearch != nullptr && list_length(expected_mmsearch->args) >= 2
                        ? reinterpret_cast<Node*>(linitial(expected_mmsearch->args))
                        : nullptr;
                Node* expected_query =
                    expected_mmsearch != nullptr && list_length(expected_mmsearch->args) >= 2
                        ? reinterpret_cast<Node*>(lsecond(expected_mmsearch->args))
                        : nullptr;
                Var* expected_target_var = try_cast_pg_node<Var>(expected_target);
                Var* expected_query_var = try_cast_pg_node<Var>(expected_query);
                ownership.expected_target_varno =
                    expected_target_var == nullptr
                        ? 0u
                        : static_cast<std::size_t>(expected_target_var->varno);
                ownership.expected_query_varno =
                    expected_query_var == nullptr
                        ? 0u
                        : static_cast<std::size_t>(expected_query_var->varno);
                Node* private_target = nullptr;
                Node* private_query = nullptr;
                if (known_join_owner && list_length(custom->custom_private) >= 6) {
                    private_target = reinterpret_cast<Node*>(list_nth(custom->custom_private, 4));
                    private_query = reinterpret_cast<Node*>(list_nth(custom->custom_private, 5));
                }
                Var* private_target_var = try_cast_pg_node<Var>(private_target);
                Var* private_query_var = try_cast_pg_node<Var>(private_query);
                ownership.wrapper_private_target_varno =
                    private_target_var == nullptr
                        ? 0u
                        : static_cast<std::size_t>(private_target_var->varno);
                ownership.wrapper_private_query_varno =
                    private_query_var == nullptr
                        ? 0u
                        : static_cast<std::size_t>(private_query_var->varno);
                ownership.wrapper_private_target_equal =
                    private_target != nullptr && equal(private_target, expected_target) ? 1u : 0u;
                ownership.wrapper_private_query_equal =
                    private_query != nullptr && equal(private_query, expected_query) ? 1u : 0u;
                std::size_t owner_index = 0u;
                for (RestrictInfo* owned :
                     postgres_list<RestrictInfo>(custom->custom_restrictinfo)) {
                    if (owned == restriction)
                        ++ownership.wrapper_pointer_matches;
                    if (owned != nullptr && owned->rinfo_serial > 0 &&
                        owned->rinfo_serial == restriction->rinfo_serial) {
                        ++ownership.wrapper_serial_matches;
                    }
                    if (owned != nullptr && equal(owned->clause, restriction->clause)) {
                        ++ownership.wrapper_clause_matches;
                    }
                    if (same_planner_restriction_identity(owned, restriction)) {
                        ownership.wrapper_rinfo_serial =
                            static_cast<std::size_t>(owned->rinfo_serial);
                    }
                    FuncExpr* owned_mmsearch =
                        owned == nullptr
                            ? nullptr
                            : try_cast_pg_node<FuncExpr>(reinterpret_cast<Node*>(owned->clause));
                    Var* owned_target =
                        owned_mmsearch != nullptr && list_length(owned_mmsearch->args) >= 2
                            ? try_cast_pg_node<Var>(
                                  reinterpret_cast<Node*>(linitial(owned_mmsearch->args)))
                            : nullptr;
                    Var* owned_query =
                        owned_mmsearch != nullptr && list_length(owned_mmsearch->args) >= 2
                            ? try_cast_pg_node<Var>(
                                  reinterpret_cast<Node*>(lsecond(owned_mmsearch->args)))
                            : nullptr;
                    if (owner_index == 0u) {
                        ownership.wrapper_owner0_target_varno =
                            owned_target == nullptr ? 0u
                                                    : static_cast<std::size_t>(owned_target->varno);
                        ownership.wrapper_owner0_query_varno =
                            owned_query == nullptr ? 0u
                                                   : static_cast<std::size_t>(owned_query->varno);
                    } else if (owner_index == 1u) {
                        ownership.wrapper_owner1_target_varno =
                            owned_target == nullptr ? 0u
                                                    : static_cast<std::size_t>(owned_target->varno);
                        ownership.wrapper_owner1_query_varno =
                            owned_query == nullptr ? 0u
                                                   : static_cast<std::size_t>(owned_query->varno);
                    }
                    ++owner_index;
                }
                if (!admit_known_wrapper_owner || !known_join_owner) {
                    reject_exact_parameterized_bmo_owner(ownership,
                                                         k_exact_bmo_known_wrapper_owns_clause);
                    return;
                }
                bool invalid_wrapper_abi = false;
                Node* owner_recipe = per_row_known_path_owner_recipe(custom, invalid_wrapper_abi);
                std::size_t exact_owner_count = 0u;
                std::size_t wrapper_mmsearch_count = 0u;
                FuncExpr* owned_mmsearch = nullptr;
                for (RestrictInfo* owned :
                     postgres_list<RestrictInfo>(custom->custom_restrictinfo)) {
                    const std::size_t calls =
                        owned == nullptr
                            ? 0u
                            : count_mmsearch_occurrences(reinterpret_cast<Node*>(owned->clause),
                                                         get_cached_stub_oids());
                    if (same_planner_restriction_identity(owned, restriction)) {
                        ++exact_owner_count;
                        owned_mmsearch =
                            calls == 1u
                                ? try_cast_pg_node<FuncExpr>(reinterpret_cast<Node*>(owned->clause))
                                : nullptr;
                    }
                    wrapper_mmsearch_count += calls;
                }
                const int executable_index =
                    custom->methods == bioseq_batch_join_path_methods() ? 8 : 6;
                FuncExpr* executable_mmsearch =
                    list_length(custom->custom_private) > executable_index
                        ? try_cast_pg_node<FuncExpr>(reinterpret_cast<Node*>(
                              list_nth(custom->custom_private, executable_index)))
                        : nullptr;
                postgres_list<Node> expected_operands(
                    expected_mmsearch == nullptr ? NIL : expected_mmsearch->args);
                postgres_list<Node> executable_operands(
                    executable_mmsearch == nullptr ? NIL : executable_mmsearch->args);
                const bool executable_shape =
                    executable_mmsearch != nullptr &&
                    is_mmsearch_funcid(executable_mmsearch->funcid, get_cached_stub_oids()) &&
                    (list_length(executable_mmsearch->args) == 2 ||
                     list_length(executable_mmsearch->args) == 3) &&
                    expected_operands.size() >= 2 && executable_operands.size() >= 2 &&
                    equal(executable_operands.get_first(), expected_operands.get_first()) &&
                    equal(executable_operands.get_second(), expected_operands.get_second());
                Var* owner_target_var = expected_operands.size() >= 2
                                            ? try_cast_pg_node<Var>(expected_operands.get_first())
                                            : nullptr;
                Var* owner_query_var = expected_operands.size() >= 2
                                           ? try_cast_pg_node<Var>(expected_operands.get_second())
                                           : nullptr;
                const bool owner_parent_contains_operands =
                    custom->path.parent != nullptr && owner_target_var != nullptr &&
                    owner_query_var != nullptr &&
                    bms_is_member(static_cast<int>(owner_target_var->varno),
                                  custom->path.parent->relids) &&
                    bms_is_member(static_cast<int>(owner_query_var->varno),
                                  custom->path.parent->relids);
                bool owner_children_cover_parent =
                    custom->path.parent != nullptr &&
                    ((custom->methods == bioseq_batch_join_path_methods() &&
                      list_length(custom->custom_paths) == 2) ||
                     (custom->methods == bioseq_per_row_join_path_methods() &&
                      list_length(custom->custom_paths) == 1));
                if (owner_children_cover_parent &&
                    custom->methods == bioseq_batch_join_path_methods()) {
                    Path* first_child = reinterpret_cast<Path*>(linitial(custom->custom_paths));
                    Path* second_child = reinterpret_cast<Path*>(lsecond(custom->custom_paths));
                    owner_children_cover_parent =
                        first_child != nullptr && second_child != nullptr &&
                        first_child->parent != nullptr && second_child->parent != nullptr &&
                        !bms_overlap(first_child->parent->relids, second_child->parent->relids);
                    Relids child_union =
                        owner_children_cover_parent
                            ? bms_union(first_child->parent->relids, second_child->parent->relids)
                            : nullptr;
                    owner_children_cover_parent =
                        owner_children_cover_parent &&
                        bms_equal(child_union, custom->path.parent->relids);
                    bms_free(child_union);
                } else if (owner_children_cover_parent) {
                    Path* child = reinterpret_cast<Path*>(linitial(custom->custom_paths));
                    owner_children_cover_parent =
                        child != nullptr && child->parent != nullptr &&
                        bms_equal(child->parent->relids, custom->path.parent->relids);
                }
                if (invalid_wrapper_abi || owner_recipe == nullptr || exact_owner_count != 1u ||
                    wrapper_mmsearch_count != 1u || owned_mmsearch == nullptr ||
                    !executable_shape || !owner_parent_contains_operands ||
                    !owner_children_cover_parent || custom->path.parallel_aware ||
                    custom->path.parallel_safe || custom->path.parallel_workers != 0 ||
                    !std::isfinite(custom->path.rows) || custom->path.rows < 0 ||
                    !std::isfinite(custom->path.startup_cost) || custom->path.startup_cost < 0 ||
                    !std::isfinite(custom->path.total_cost) ||
                    custom->path.total_cost < custom->path.startup_cost) {
                    reject_exact_parameterized_bmo_owner(ownership, k_exact_bmo_known_wrapper_abi);
                    return;
                }
                ++ownership.matches;
                for (Path* child : postgres_list<Path>(custom->custom_paths)) {
                    count_exact_parameterized_bmo_owners(
                        root, child, restriction, ownership, admit_known_wrapper_owner);
                }
                return;
            }
            bool invalid_wrapper_abi = false;
            if (known_join_owner &&
                per_row_known_path_owner_recipe(custom, invalid_wrapper_abi) == nullptr) {
                reject_exact_parameterized_bmo_owner(ownership, k_exact_bmo_known_wrapper_abi);
                return;
            }
            if (known_slice_wrapper && list_length(custom->custom_paths) != 1) {
                reject_exact_parameterized_bmo_owner(ownership, k_exact_bmo_known_wrapper_abi);
                return;
            }
            for (Path* child : postgres_list<Path>(custom->custom_paths)) {
                count_exact_parameterized_bmo_owners(
                    root, child, restriction, ownership, admit_known_wrapper_owner);
            }
            return;
        }
        if (IsA(path, NestPath) || IsA(path, MergePath) || IsA(path, HashPath)) {
            auto* join = reinterpret_cast<JoinPath*>(path);
            bool nested_owns_expected = false;
            for (RestrictInfo* nested : postgres_list<RestrictInfo>(join->joinrestrictinfo)) {
                if (admit_known_wrapper_owner && nested != nullptr &&
                    count_mmsearch_occurrences(reinterpret_cast<Node*>(nested->clause),
                                               get_cached_stub_oids()) != 0u) {
                    reject_exact_parameterized_bmo_owner(ownership, k_exact_bmo_nested_join_clause);
                    return;
                }
                nested_owns_expected =
                    nested_owns_expected || same_planner_restriction_identity(nested, restriction);
            }
            List* algorithm_clauses =
                IsA(path, MergePath)
                    ? reinterpret_cast<MergePath*>(path)->path_mergeclauses
                    : (IsA(path, HashPath) ? reinterpret_cast<HashPath*>(path)->path_hashclauses
                                           : NIL);
            if (admit_known_wrapper_owner) {
                for (RestrictInfo* algorithm_clause :
                     postgres_list<RestrictInfo>(algorithm_clauses)) {
                    if (algorithm_clause != nullptr &&
                        count_mmsearch_occurrences(
                            reinterpret_cast<Node*>(algorithm_clause->clause),
                            get_cached_stub_oids()) != 0u) {
                        reject_exact_parameterized_bmo_owner(ownership,
                                                             k_exact_bmo_nested_join_clause);
                        return;
                    }
                }
            }
            if (nested_owns_expected) {
                reject_exact_parameterized_bmo_owner(ownership, k_exact_bmo_nested_join_clause);
                return;
            }
            count_exact_parameterized_bmo_owners(
                root, join->outerjoinpath, restriction, ownership, admit_known_wrapper_owner);
            count_exact_parameterized_bmo_owners(
                root, join->innerjoinpath, restriction, ownership, admit_known_wrapper_owner);
            return;
        }
        Path* unary = nullptr;
        if (IsA(path, ProjectionPath)) {
            unary = reinterpret_cast<ProjectionPath*>(path)->subpath;
        } else if (IsA(path, ProjectSetPath)) {
            unary = reinterpret_cast<ProjectSetPath*>(path)->subpath;
        } else if (IsA(path, SortPath)) {
            unary = reinterpret_cast<SortPath*>(path)->subpath;
        } else if (IsA(path, IncrementalSortPath)) {
            unary = reinterpret_cast<IncrementalSortPath*>(path)->spath.subpath;
        } else if (IsA(path, MaterialPath)) {
            unary = reinterpret_cast<MaterialPath*>(path)->subpath;
        } else if (IsA(path, MemoizePath)) {
            unary = reinterpret_cast<MemoizePath*>(path)->subpath;
        } else if (IsA(path, UniquePath)) {
            unary = reinterpret_cast<UniquePath*>(path)->subpath;
        } else if (IsA(path, GatherPath)) {
            unary = reinterpret_cast<GatherPath*>(path)->subpath;
        } else if (IsA(path, GatherMergePath)) {
            unary = reinterpret_cast<GatherMergePath*>(path)->subpath;
        }
        if (unary != nullptr) {
            count_exact_parameterized_bmo_owners(
                root, unary, restriction, ownership, admit_known_wrapper_owner);
            return;
        }
        if (IsA(path, AppendPath)) {
            for (Path* child : postgres_list<Path>(reinterpret_cast<AppendPath*>(path)->subpaths)) {
                count_exact_parameterized_bmo_owners(
                    root, child, restriction, ownership, admit_known_wrapper_owner);
            }
            return;
        }
        if (IsA(path, MergeAppendPath)) {
            for (Path* child :
                 postgres_list<Path>(reinterpret_cast<MergeAppendPath*>(path)->subpaths)) {
                count_exact_parameterized_bmo_owners(
                    root, child, restriction, ownership, admit_known_wrapper_owner);
            }
            return;
        }
        if (IsA(path, GroupResultPath)) {
            /* GroupResultPath is childless, but its bare HAVING quals are executable
             * and are not represented by pathtarget or ParamPathInfo.  It can never
             * own a BMO restriction, so any surviving mmsearch here invalidates the
             * closed-world owner count instead of disappearing behind a leaf tag. */
            for (Node* qual :
                 postgres_list<Node>(reinterpret_cast<GroupResultPath*>(path)->quals)) {
                if (count_mmsearch_occurrences(qual, get_cached_stub_oids()) != 0u) {
                    reject_exact_parameterized_bmo_owner(ownership, k_exact_bmo_nested_join_clause);
                    return;
                }
            }
            return;
        }
        /* Closed-world ownership: these are the only childless core Path variants
         * admitted as leaves.  Any other tag may hide a child/owner layout this
         * narrow fallback does not understand. */
        if (IsA(path, Path) || IsA(path, IndexPath) || IsA(path, TidPath) ||
            IsA(path, TidRangePath))
            return;
        reject_exact_parameterized_bmo_owner(ownership, k_exact_bmo_unknown_core_path);
    }

    /** @brief Bit flags describing failures while reconstructing a parameterized base owner. */
    enum exact_parameterized_bmo_reconstruction_rejection : std::uint32_t {
        k_exact_bmo_reconstruct_invalid_input = 1u << 0,
        k_exact_bmo_reconstruct_operand_shape = 1u << 1,
        k_exact_bmo_reconstruct_target_rel = 1u << 2,
        k_exact_bmo_reconstruct_target_rte = 1u << 3,
        k_exact_bmo_reconstruct_required_outer = 1u << 4,
        k_exact_bmo_reconstruct_base_mmsearch = 1u << 5,
        k_exact_bmo_reconstruct_path_shape = 1u << 6,
        k_exact_bmo_reconstruct_owner_analysis = 1u << 7,
        k_exact_bmo_reconstruct_owner_multiplicity = 1u << 8,
        k_exact_bmo_reconstruct_ppi = 1u << 9,
        k_exact_bmo_reconstruct_owner_abi = 1u << 10,
        k_exact_bmo_reconstruct_residual_mmsearch = 1u << 11,
        k_exact_bmo_reconstruct_parallel = 1u << 12,
        k_exact_bmo_reconstruct_cost = 1u << 13,
    };

    /** @brief Bit flags describing mismatches in a reconstructed base-owner private ABI. */
    enum exact_parameterized_bmo_reconstruction_abi_mismatch : std::uint32_t {
        k_exact_bmo_reconstruct_abi_invalid_tag = 1u << 0,
        k_exact_bmo_reconstruct_abi_missing_recipe = 1u << 1,
        k_exact_bmo_reconstruct_abi_recipe_target = 1u << 2,
        k_exact_bmo_reconstruct_abi_tag_multiplicity = 1u << 3,
        k_exact_bmo_reconstruct_abi_expression_multiplicity = 1u << 4,
        k_exact_bmo_reconstruct_abi_expression_shape = 1u << 5,
        k_exact_bmo_reconstruct_abi_expression_target = 1u << 6,
        k_exact_bmo_reconstruct_abi_dynamic_query = 1u << 7,
        k_exact_bmo_reconstruct_abi_dynamic_multiplicity = 1u << 8,
        k_exact_bmo_reconstruct_abi_constant_shape = 1u << 9,
        k_exact_bmo_reconstruct_abi_constant_multiplicity = 1u << 10,
        k_exact_bmo_reconstruct_abi_missing_dynamic = 1u << 11,
        k_exact_bmo_reconstruct_abi_missing_constant = 1u << 12,
        k_exact_bmo_reconstruct_abi_unexpected_constant = 1u << 13,
        k_exact_bmo_reconstruct_abi_incompatible_options = 1u << 14,
    };

    /** @brief Aggregates reconstruction rejection masks and owner multiplicities. */
    struct exact_parameterized_bmo_reconstruction_audit {
        std::uint32_t rejection_mask = 0u;
        std::uint32_t owner_abi_mismatch_mask = 0u;
        std::size_t expected_owner_count = 0u;
        std::size_t owner_tag_count = 0u;
        std::size_t owner_expression_count = 0u;
    };

} // export custom-scan partition declarations
