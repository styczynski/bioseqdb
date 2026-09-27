/**
 * @file
 * @brief Keeps or rebuilds join paths where a custom scan owns each mmsearch evaluation.
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

export module bioseq.postgres.table:custom_scan_join_ownership_transformed_verification;

import std;
import bioseq.postgres.support;
import bioseq.postgres.column.table_access;
import bioseq.indexing.type;
import :custom_scan_oid_expression_support_oid_catalog;
import :custom_scan_oid_expression_support_expression_analysis;
import :custom_scan_oid_expression_support_restriction_ownership;
import :custom_scan_planner_method_contracts;
import :custom_scan_planner_path_contracts;
import :custom_scan_join_ownership_carrier_lineage;
import :custom_scan_join_ownership_sequence_typmod;
import :custom_scan_join_ownership_semantic_admission;
import :custom_scan_join_ownership_descendant_neutralization;
import :custom_scan_join_ownership_parameterized_owner_analysis;
export import :custom_scan_join_ownership_transformed_verification_contracts;

using namespace bioseq::postgres::support;

export {
    /**
     * @brief Rebuilds the parameterized BioseqScan needed as the inner side of a join.
     *
     * PostgreSQL may discard that CustomPath before building the higher join.
     * Use add_bioseq_column_scan_path() to rebuild and cost the exact target
     * search. Keep the result private to the new NestPath, outside the base
     * relation's pathlist.
     */
    [[nodiscard]] Path*
    reconstruct_exact_parameterized_bmo_inner(PlannerInfo* root,
                                              RelOptInfo* target_input,
                                              RelOptInfo* provider_input,
                                              RestrictInfo* restriction,
                                              FuncExpr* mmsearch,
                                              exact_parameterized_bmo_reconstruction_audit& audit) {
        audit = {};
        // Records one reconstruction failure and returns no candidate path.
        auto reject =
            [&audit](
                const exact_parameterized_bmo_reconstruction_rejection reason) noexcept -> Path* {
            audit.rejection_mask |= static_cast<std::uint32_t>(reason);
            return nullptr;
        };
        if (root == nullptr || root->parse == nullptr || target_input == nullptr ||
            provider_input == nullptr || restriction == nullptr || mmsearch == nullptr ||
            list_length(mmsearch->args) < 2) {
            return reject(k_exact_bmo_reconstruct_invalid_input);
        }
        postgres_list<Node> operands(mmsearch->args);
        Var* target_var = try_cast_pg_node<Var>(operands.get_first());
        Var* query_var = try_cast_pg_node<Var>(operands.get_second());
        const bioseq_stub_oids& oids = get_cached_stub_oids();
        if (target_var == nullptr || query_var == nullptr ||
            !bioseq_is_concrete_rte_varno(target_var->varno) ||
            !bioseq_is_concrete_rte_varno(query_var->varno) || target_var->varlevelsup != 0 ||
            query_var->varlevelsup != 0 || target_var->varattno <= 0 || query_var->varattno <= 0 ||
            !bms_is_empty(target_var->varnullingrels) || !bms_is_empty(query_var->varnullingrels) ||
            target_var->vartype != oids.bioseq_seq_type_oid ||
            query_var->vartype != oids.bioseq_seq_type_oid ||
            !mmsearch_typmods_share_alphabet(target_var->vartypmod, query_var->vartypmod) ||
            target_var->varcollid != query_var->varcollid) {
            return reject(k_exact_bmo_reconstruct_operand_shape);
        }
        if (target_input->reloptkind != RELOPT_BASEREL ||
            target_input->relid != target_var->varno ||
            bms_num_members(target_input->relids) != 1 ||
            !bms_is_member(static_cast<int>(target_var->varno), target_input->relids) ||
            !bms_is_member(static_cast<int>(query_var->varno), provider_input->relids) ||
            bms_overlap(target_input->relids, provider_input->relids)) {
            return reject(k_exact_bmo_reconstruct_target_rel);
        }
        if (target_input->relid <= 0 || target_input->relid >= root->simple_rel_array_size ||
            root->simple_rel_array[target_input->relid] != target_input ||
            target_input->relid > static_cast<Index>(list_length(root->parse->rtable))) {
            return reject(k_exact_bmo_reconstruct_target_rel);
        }
        RangeTblEntry* target_rte = rt_fetch(target_input->relid, root->parse->rtable);
        if (target_rte == nullptr || target_rte->rtekind != RTE_RELATION ||
            target_rte->securityQuals != NIL || !OidIsValid(target_rte->relid) ||
            !relation_oid_uses_column_table_access_method(target_rte->relid)) {
            return reject(k_exact_bmo_reconstruct_target_rte);
        }

        postgres_list<RestrictInfo> base_mmsearch_restrictions;
        FuncExpr* base_mmsearch = nullptr;
        for (RestrictInfo* base : postgres_list<RestrictInfo>(target_input->baserestrictinfo)) {
            const std::size_t calls =
                base == nullptr
                    ? 0u
                    : count_mmsearch_occurrences(reinterpret_cast<Node*>(base->clause), oids);
            if (calls == 0u)
                continue;
            FuncExpr* expression =
                calls == 1u ? try_cast_pg_node<FuncExpr>(reinterpret_cast<Node*>(base->clause))
                            : nullptr;
            if (expression == nullptr || !is_mmsearch_funcid(expression->funcid, oids) ||
                (expression->funcid == oids.mmsearch_oid && list_length(expression->args) != 2) ||
                (expression->funcid == oids.mmsearch_with_opts_oid &&
                 list_length(expression->args) != 3) ||
                base_mmsearch != nullptr) {
                return reject(k_exact_bmo_reconstruct_base_mmsearch);
            }
            postgres_list<Node> base_operands(expression->args);
            Const* constant_query = try_cast_pg_node<Const>(base_operands.get_second());
            if (!equal(base_operands.get_first(), target_var) || constant_query == nullptr ||
                constant_query->constisnull ||
                constant_query->consttype != oids.bioseq_seq_type_oid ||
                !mmsearch_typmods_share_alphabet(target_var->vartypmod,
                                                 constant_query->consttypmod)) {
                return reject(k_exact_bmo_reconstruct_base_mmsearch);
            }
            base_mmsearch = expression;
            base_mmsearch_restrictions.append(base);
        }
        if (base_mmsearch != nullptr) {
            if (query_var->varno > static_cast<Index>(list_length(root->parse->rtable))) {
                return reject(k_exact_bmo_reconstruct_base_mmsearch);
            }
            RangeTblEntry* query_rte = rt_fetch(query_var->varno, root->parse->rtable);
            FuncExpr* normalized_primary = copyObject(mmsearch);
            if (list_length(normalized_primary->args) == 3) {
                Node* option_expression =
                    reinterpret_cast<Node*>(list_nth(normalized_primary->args, 2));
                Node* folded = eval_const_expressions(
                    root, reinterpret_cast<Node*>(copyObject(option_expression)));
                lfirst(list_nth_cell(normalized_primary->args, 2)) = folded;
            }
            if (query_rte == nullptr || query_rte->rtekind != RTE_RELATION ||
                query_rte->securityQuals != NIL || !OidIsValid(query_rte->relid) ||
                relation_oid_uses_column_table_access_method(query_rte->relid) ||
                !bounded_batch_fallback_options_are_supported(
                    normalized_primary, base_mmsearch, oids)) {
                return reject(k_exact_bmo_reconstruct_base_mmsearch);
            }
        }

        Relids required_outer = bms_add_member(bms_copy(target_input->lateral_relids),
                                               static_cast<int>(query_var->varno));
        if (required_outer == nullptr || bms_overlap(required_outer, target_input->relids) ||
            !bms_is_subset(required_outer, provider_input->relids)) {
            bms_free(required_outer);
            return reject(k_exact_bmo_reconstruct_required_outer);
        }
        Path* candidate =
            add_bioseq_column_scan_path(root, target_input, target_rte, required_outer);
        /* get_baserel_parampathinfo may store required_outer in its parameter
         * information. Leave the bitmap owned by the planner after this call. */
        auto* custom = candidate != nullptr && IsA(candidate, CustomPath)
                           ? reinterpret_cast<CustomPath*>(candidate)
                           : nullptr;
        if (custom == nullptr || custom->methods != bioseq_column_scan_path_methods() ||
            candidate->parent != target_input || candidate->pathtype != T_CustomScan ||
            list_length(custom->custom_private) != 3) {
            return reject(k_exact_bmo_reconstruct_path_shape);
        }
        exact_parameterized_bmo_ownership owner_analysis{};
        count_exact_parameterized_bmo_owners(root, candidate, restriction, owner_analysis);
        if (owner_analysis.invalid) {
            audit.rejection_mask |= owner_analysis.rejection_mask << 16;
            return reject(k_exact_bmo_reconstruct_owner_analysis);
        }
        if (owner_analysis.matches != 1u) {
            return reject(k_exact_bmo_reconstruct_owner_multiplicity);
        }
        const std::size_t expected_owner_count =
            1u + static_cast<std::size_t>(base_mmsearch_restrictions.size());
        audit.expected_owner_count = expected_owner_count;
        if (expected_owner_count > 2u ||
            list_length(custom->custom_restrictinfo) != static_cast<int>(expected_owner_count)) {
            return reject(k_exact_bmo_reconstruct_owner_multiplicity);
        }
        std::size_t dynamic_owner_count = 0u;
        std::size_t base_owner_count = 0u;
        for (RestrictInfo* owned : postgres_list<RestrictInfo>(custom->custom_restrictinfo)) {
            if (same_planner_restriction_identity(owned, restriction)) {
                ++dynamic_owner_count;
            } else if (list_member_ptr(base_mmsearch_restrictions.get(), owned)) {
                ++base_owner_count;
            } else {
                return reject(k_exact_bmo_reconstruct_owner_multiplicity);
            }
        }
        if (dynamic_owner_count != 1u ||
            base_owner_count != static_cast<std::size_t>(base_mmsearch_restrictions.size())) {
            return reject(k_exact_bmo_reconstruct_owner_multiplicity);
        }
        if (candidate->param_info == nullptr ||
            !bms_equal(PATH_REQ_OUTER(candidate), required_outer) ||
            !bms_is_member(static_cast<int>(query_var->varno), PATH_REQ_OUTER(candidate)) ||
            !bms_is_subset(PATH_REQ_OUTER(candidate), provider_input->relids)) {
            return reject(k_exact_bmo_reconstruct_ppi);
        }
        std::size_t ppi_dynamic_count = 0u;
        for (RestrictInfo* ppi_clause :
             postgres_list<RestrictInfo>(candidate->param_info->ppi_clauses)) {
            const std::size_t calls =
                ppi_clause == nullptr
                    ? 0u
                    : count_mmsearch_occurrences(reinterpret_cast<Node*>(ppi_clause->clause), oids);
            if (same_planner_restriction_identity(ppi_clause, restriction)) {
                ++ppi_dynamic_count;
                if (calls != 1u)
                    return reject(k_exact_bmo_reconstruct_ppi);
            } else if (calls != 0u) {
                return reject(k_exact_bmo_reconstruct_ppi);
            }
        }
        if (ppi_dynamic_count != 1u) {
            return reject(k_exact_bmo_reconstruct_ppi);
        }

        bool invalid_owner_abi = false;
        Node* owner_recipe = per_row_base_scan_owner_recipe(custom, invalid_owner_abi);
        List* owner_expressions = NIL;
        std::size_t owner_tag_count = 0u;
        for (Node* private_node : postgres_list<Node>(custom->custom_private)) {
            if (private_node == nullptr || !IsA(private_node, List))
                continue;
            List* tag = reinterpret_cast<List*>(private_node);
            Node* magic = list_length(tag) >= 1 ? reinterpret_cast<Node*>(linitial(tag)) : nullptr;
            if (magic == nullptr || !IsA(magic, Integer) ||
                intVal(magic) != k_bioseq_mmsearch_owner_magic)
                continue;
            ++owner_tag_count;
            Node* expressions =
                list_length(tag) == 3 ? reinterpret_cast<Node*>(list_nth(tag, 2)) : nullptr;
            if (list_length(tag) != 3 || !IsA(reinterpret_cast<Node*>(list_nth(tag, 1)), Integer) ||
                intVal(list_nth(tag, 1)) != k_bioseq_mmsearch_owner_version ||
                expressions == nullptr || !IsA(expressions, List)) {
                invalid_owner_abi = true;
            } else {
                owner_expressions = reinterpret_cast<List*>(expressions);
            }
        }
        audit.owner_tag_count = owner_tag_count;
        audit.owner_expression_count = static_cast<std::size_t>(list_length(owner_expressions));
        if (invalid_owner_abi) {
            audit.owner_abi_mismatch_mask |= k_exact_bmo_reconstruct_abi_invalid_tag;
        }
        if (owner_recipe == nullptr) {
            audit.owner_abi_mismatch_mask |= k_exact_bmo_reconstruct_abi_missing_recipe;
        } else if (!equal(owner_recipe, target_var)) {
            audit.owner_abi_mismatch_mask |= k_exact_bmo_reconstruct_abi_recipe_target;
        }
        if (owner_tag_count != 1u) {
            audit.owner_abi_mismatch_mask |= k_exact_bmo_reconstruct_abi_tag_multiplicity;
        }
        if (list_length(owner_expressions) != static_cast<int>(expected_owner_count)) {
            audit.owner_abi_mismatch_mask |= k_exact_bmo_reconstruct_abi_expression_multiplicity;
        }
        if (audit.owner_abi_mismatch_mask != 0u) {
            return reject(k_exact_bmo_reconstruct_owner_abi);
        }
        FuncExpr* dynamic_expression = nullptr;
        FuncExpr* constant_expression = nullptr;
        for (int index = 0; index < static_cast<int>(expected_owner_count); ++index) {
            RestrictInfo* owned =
                reinterpret_cast<RestrictInfo*>(list_nth(custom->custom_restrictinfo, index));
            FuncExpr* expression = try_cast_pg_node<FuncExpr>(
                reinterpret_cast<Node*>(list_nth(owner_expressions, index)));
            if (expression == nullptr || list_length(expression->args) < 2) {
                audit.owner_abi_mismatch_mask |= k_exact_bmo_reconstruct_abi_expression_shape;
                return reject(k_exact_bmo_reconstruct_owner_abi);
            }
            if (!equal(linitial(expression->args), target_var)) {
                audit.owner_abi_mismatch_mask |= k_exact_bmo_reconstruct_abi_expression_target;
                return reject(k_exact_bmo_reconstruct_owner_abi);
            }
            if (same_planner_restriction_identity(owned, restriction)) {
                if (!equal(lsecond(expression->args), query_var)) {
                    audit.owner_abi_mismatch_mask |= k_exact_bmo_reconstruct_abi_dynamic_query;
                    return reject(k_exact_bmo_reconstruct_owner_abi);
                }
                if (dynamic_expression != nullptr) {
                    audit.owner_abi_mismatch_mask |=
                        k_exact_bmo_reconstruct_abi_dynamic_multiplicity;
                    return reject(k_exact_bmo_reconstruct_owner_abi);
                }
                dynamic_expression = expression;
            } else {
                Const* constant =
                    try_cast_pg_node<Const>(reinterpret_cast<Node*>(lsecond(expression->args)));
                if (constant == nullptr || constant->constisnull) {
                    audit.owner_abi_mismatch_mask |= k_exact_bmo_reconstruct_abi_constant_shape;
                    return reject(k_exact_bmo_reconstruct_owner_abi);
                }
                if (constant_expression != nullptr) {
                    audit.owner_abi_mismatch_mask |=
                        k_exact_bmo_reconstruct_abi_constant_multiplicity;
                    return reject(k_exact_bmo_reconstruct_owner_abi);
                }
                constant_expression = expression;
            }
        }
        if (dynamic_expression == nullptr) {
            audit.owner_abi_mismatch_mask |= k_exact_bmo_reconstruct_abi_missing_dynamic;
        }
        if (base_mmsearch != nullptr && constant_expression == nullptr) {
            audit.owner_abi_mismatch_mask |= k_exact_bmo_reconstruct_abi_missing_constant;
        }
        if (base_mmsearch == nullptr && constant_expression != nullptr) {
            audit.owner_abi_mismatch_mask |= k_exact_bmo_reconstruct_abi_unexpected_constant;
        }
        if (base_mmsearch != nullptr && dynamic_expression != nullptr &&
            constant_expression != nullptr &&
            !bounded_batch_fallback_options_are_supported(
                dynamic_expression, constant_expression, oids)) {
            audit.owner_abi_mismatch_mask |= k_exact_bmo_reconstruct_abi_incompatible_options;
        }
        if (audit.owner_abi_mismatch_mask != 0u) {
            return reject(k_exact_bmo_reconstruct_owner_abi);
        }
        for (int index = 0; index < 2; ++index) {
            Node* residual_node = reinterpret_cast<Node*>(list_nth(custom->custom_private, index));
            if (residual_node != nullptr && !IsA(residual_node, List)) {
                return reject(k_exact_bmo_reconstruct_owner_abi);
            }
            for (RestrictInfo* residual : postgres_list<RestrictInfo>(
                     residual_node == nullptr ? NIL : reinterpret_cast<List*>(residual_node))) {
                if (residual != nullptr &&
                    count_mmsearch_occurrences(reinterpret_cast<Node*>(residual->clause), oids) !=
                        0u) {
                    return reject(k_exact_bmo_reconstruct_residual_mmsearch);
                }
            }
        }
        if (candidate->parallel_aware || candidate->parallel_safe ||
            candidate->parallel_workers != 0) {
            return reject(k_exact_bmo_reconstruct_parallel);
        }
        if (!std::isfinite(candidate->rows) || candidate->rows < 0 ||
            !std::isfinite(candidate->startup_cost) || candidate->startup_cost < 0 ||
            !std::isfinite(candidate->total_cost) ||
            candidate->total_cost < candidate->startup_cost) {
            return reject(k_exact_bmo_reconstruct_cost);
        }
        return candidate;
    }

    /** @brief Problems with the stored mmsearch owner or its restriction identity. */
    enum exact_parameterized_bmo_retainer_abi_mismatch : std::uint32_t {
        k_exact_bmo_retainer_invalid_owner_tag = 1u << 0,
        k_exact_bmo_retainer_missing_custom_restriction = 1u << 1,
        k_exact_bmo_retainer_missing_custom_serial = 1u << 2,
        k_exact_bmo_retainer_missing_custom_clause = 1u << 3,
        k_exact_bmo_retainer_missing_custom_identity = 1u << 4,
        k_exact_bmo_retainer_multiple_custom_identities = 1u << 5,
    };

    /** @brief Problems matching the mmsearch restriction to the path's parameter clauses. */
    enum exact_parameterized_bmo_retainer_ppi_mismatch : std::uint32_t {
        k_exact_bmo_retainer_missing_param_info = 1u << 0,
        k_exact_bmo_retainer_empty_ppi_clauses = 1u << 1,
        k_exact_bmo_retainer_missing_ppi_serial = 1u << 2,
        k_exact_bmo_retainer_missing_ppi_clause = 1u << 3,
        k_exact_bmo_retainer_missing_ppi_identity = 1u << 4,
        k_exact_bmo_retainer_multiple_ppi_identities = 1u << 5,
    };

    /** @brief Reasons a candidate scan cannot supply the inner side of the join. */
    enum exact_parameterized_bmo_retainer_validation_mismatch : std::uint32_t {
        k_exact_bmo_retainer_candidate_parent = 1u << 0,
        k_exact_bmo_retainer_candidate_owner_analysis = 1u << 1,
        k_exact_bmo_retainer_candidate_owner_multiplicity = 1u << 2,
        k_exact_bmo_retainer_candidate_unparameterized = 1u << 3,
        k_exact_bmo_retainer_candidate_missing_query_relid = 1u << 4,
        k_exact_bmo_retainer_candidate_foreign_required_outer = 1u << 5,
        k_exact_bmo_retainer_candidate_parallel = 1u << 6,
        k_exact_bmo_retainer_candidate_nonfinite_cost = 1u << 7,
        k_exact_bmo_retainer_candidate_negative_cost = 1u << 8,
    };

    /**
     * @brief Keeps valid join paths or builds one with a custom scan owning mmsearch.
     * @return True after installing at least one checked path in the join relation.
     */
    [[nodiscard]] bool retain_join_paths_with_custom_restriction_owner(
        PlannerInfo* root,
        RelOptInfo* joinrel,
        RelOptInfo* left_input,
        RelOptInfo* right_input,
        JoinPathExtraData* extra,
        RestrictInfo* restriction,
        FuncExpr* mmsearch,
        exact_parameterized_bmo_retainer_audit* audit) {
        if (root == nullptr || joinrel == nullptr || left_input == nullptr ||
            right_input == nullptr || extra == nullptr || restriction == nullptr ||
            mmsearch == nullptr || list_length(mmsearch->args) < 2)
            return false;
        exact_parameterized_bmo_retainer_audit local_audit{};
        local_audit.joinrel_relids_low = exact_bmo_audit_relids_low(joinrel->relids);
        local_audit.left_relids_low = exact_bmo_audit_relids_low(left_input->relids);
        local_audit.right_relids_low = exact_bmo_audit_relids_low(right_input->relids);
        local_audit.left_relid_count =
            static_cast<std::size_t>(bms_num_members(left_input->relids));
        local_audit.right_relid_count =
            static_cast<std::size_t>(bms_num_members(right_input->relids));
        postgres_list<Path> retained;
        for (Path* path : postgres_list<Path>(joinrel->pathlist)) {
            ++local_audit.candidates;
            if (path == nullptr || !IsA(path, NestPath))
                continue;
            ++local_audit.nest_paths;
            auto* nest = reinterpret_cast<NestPath*>(path);
            if (nest->jpath.jointype != JOIN_INNER)
                continue;
            ++local_audit.inner_nest_paths;
            RelOptInfo* outer_parent =
                nest->jpath.outerjoinpath == nullptr ? nullptr : nest->jpath.outerjoinpath->parent;
            RelOptInfo* inner_parent =
                nest->jpath.innerjoinpath == nullptr ? nullptr : nest->jpath.innerjoinpath->parent;
            const bool current_direct = outer_parent != nullptr && inner_parent != nullptr &&
                                        bms_equal(outer_parent->relids, left_input->relids) &&
                                        bms_equal(inner_parent->relids, right_input->relids);
            const bool current_swapped = outer_parent != nullptr && inner_parent != nullptr &&
                                         bms_equal(outer_parent->relids, right_input->relids) &&
                                         bms_equal(inner_parent->relids, left_input->relids);
            const bool current_pair = current_direct || current_swapped;
            if (current_pair) {
                ++local_audit.current_pair_paths;
            } else {
                ++local_audit.foreign_decomposition_paths;
            }

            std::size_t exact_root_copies = 0u;
            bool foreign_root_mmsearch = false;
            for (RestrictInfo* root_clause :
                 postgres_list<RestrictInfo>(nest->jpath.joinrestrictinfo)) {
                const std::size_t calls =
                    root_clause == nullptr
                        ? 0u
                        : count_mmsearch_occurrences(reinterpret_cast<Node*>(root_clause->clause),
                                                     get_cached_stub_oids());
                if (same_planner_restriction_identity(root_clause, restriction)) {
                    ++exact_root_copies;
                    if (calls != 1u)
                        foreign_root_mmsearch = true;
                } else if (calls != 0u) {
                    /* This mmsearch belongs to a different planner restriction. Removing
                     * the selected restriction would leave this SQL stub in the join
                     * filter, where it cannot execute. */
                    foreign_root_mmsearch = true;
                }
            }
            if (exact_root_copies > 1u || foreign_root_mmsearch) {
                ++local_audit.root_foreign_mmsearch_paths;
                continue;
            }
            if (exact_root_copies == 1u) {
                ++local_audit.root_owned_clause_paths;
            }
            if (!current_pair) {
                bool exact_foreign_partition =
                    path->parent == joinrel && outer_parent != nullptr && inner_parent != nullptr &&
                    !bms_overlap(outer_parent->relids, inner_parent->relids);
                Relids foreign_union = exact_foreign_partition
                                           ? bms_union(outer_parent->relids, inner_parent->relids)
                                           : nullptr;
                exact_foreign_partition =
                    exact_foreign_partition && bms_equal(foreign_union, joinrel->relids);
                bms_free(foreign_union);
                if (!exact_foreign_partition) {
                    local_audit.foreign_rejection_mask |= 1u << 0;
                    continue;
                }
                if (exact_root_copies != 0u || foreign_root_mmsearch) {
                    local_audit.foreign_rejection_mask |= 1u << 1;
                    continue;
                }
                if (batch_join_path_contains_parallel_execution(path) ||
                    !std::isfinite(path->startup_cost) || !std::isfinite(path->total_cost) ||
                    path->startup_cost < 0 || path->total_cost < path->startup_cost) {
                    local_audit.foreign_rejection_mask |= 1u << 2;
                    continue;
                }
                exact_parameterized_bmo_ownership foreign_owner{};
                count_exact_parameterized_bmo_owners(root,
                                                     path,
                                                     restriction,
                                                     foreign_owner,
                                                     /*admit_known_wrapper_owner=*/true);
                local_audit.foreign_owner_matches += foreign_owner.matches;
                local_audit.foreign_owner_rejection_mask |= foreign_owner.rejection_mask;
                if (foreign_owner.invalid) {
                    local_audit.foreign_rejection_mask |= 1u << 3;
                    continue;
                }
                if (foreign_owner.matches != 1u) {
                    local_audit.foreign_rejection_mask |= 1u << 4;
                    continue;
                }
                retained.append(path);
                ++local_audit.safe_foreign_paths;
                continue;
            }

            exact_parameterized_bmo_ownership outer{};
            exact_parameterized_bmo_ownership inner{};
            count_exact_parameterized_bmo_owners(
                root, nest->jpath.outerjoinpath, restriction, outer);
            count_exact_parameterized_bmo_owners(
                root, nest->jpath.innerjoinpath, restriction, inner);
            local_audit.outer_matches += outer.matches;
            local_audit.inner_matches += inner.matches;
            local_audit.outer_rejection_mask |= outer.rejection_mask;
            local_audit.inner_rejection_mask |= inner.rejection_mask;
            // Copies wrapper diagnostics from one exact-owner analysis into the result audit.
            auto capture_wrapper_audit = [&local_audit](
                                             exact_parameterized_bmo_ownership const& ownership,
                                             const std::uint32_t side) {
                if (ownership.wrapper_owns_expected == 0u)
                    return;
                local_audit.wrapper_side_mask |= side;
                local_audit.wrapper_owns_expected += ownership.wrapper_owns_expected;
                local_audit.wrapper_method_mask |= ownership.wrapper_method_mask;
                if (local_audit.wrapper_expected_serial == 0u) {
                    local_audit.wrapper_version = ownership.wrapper_version;
                    local_audit.wrapper_restriction_count = ownership.wrapper_restriction_count;
                    local_audit.wrapper_child_count = ownership.wrapper_child_count;
                    local_audit.wrapper_pointer_matches = ownership.wrapper_pointer_matches;
                    local_audit.wrapper_serial_matches = ownership.wrapper_serial_matches;
                    local_audit.wrapper_clause_matches = ownership.wrapper_clause_matches;
                    local_audit.wrapper_expected_serial = ownership.expected_rinfo_serial;
                    local_audit.wrapper_owned_serial = ownership.wrapper_rinfo_serial;
                    local_audit.wrapper_expected_target_varno = ownership.expected_target_varno;
                    local_audit.wrapper_expected_query_varno = ownership.expected_query_varno;
                    local_audit.wrapper_private_target_varno =
                        ownership.wrapper_private_target_varno;
                    local_audit.wrapper_private_query_varno = ownership.wrapper_private_query_varno;
                    local_audit.wrapper_private_target_equal =
                        ownership.wrapper_private_target_equal;
                    local_audit.wrapper_private_query_equal = ownership.wrapper_private_query_equal;
                    local_audit.wrapper_owner0_target_varno = ownership.wrapper_owner0_target_varno;
                    local_audit.wrapper_owner0_query_varno = ownership.wrapper_owner0_query_varno;
                    local_audit.wrapper_owner1_target_varno = ownership.wrapper_owner1_target_varno;
                    local_audit.wrapper_owner1_query_varno = ownership.wrapper_owner1_query_varno;
                    local_audit.wrapper_parent_relids_low = ownership.wrapper_parent_relids_low;
                    local_audit.wrapper_child0_relids_low = ownership.wrapper_child0_relids_low;
                    local_audit.wrapper_child1_relids_low = ownership.wrapper_child1_relids_low;
                }
            };
            capture_wrapper_audit(outer, 1u);
            capture_wrapper_audit(inner, 2u);
            if (outer.invalid || inner.invalid || outer.matches != 0u || inner.matches != 1u)
                continue;

            if (exact_root_copies == 0u) {
                retained.append(path);
            } else {
                /* PostgreSQL can keep the join filter even when the inner BioseqScan
                 * already owns it. Copy the root NestPath and remove that restriction so
                 * create_nestloop_plan cannot emit an mmsearch SQL stub as a Join Filter.
                 * Keep the original cost estimate. */
                auto* clone = makeNode(NestPath);
                *clone = *nest;
                postgres_list<RestrictInfo> residual;
                for (RestrictInfo* root_clause :
                     postgres_list<RestrictInfo>(nest->jpath.joinrestrictinfo)) {
                    if (!same_planner_restriction_identity(root_clause, restriction))
                        residual.append(root_clause);
                }
                clone->jpath.joinrestrictinfo = residual.release();
                retained.append(reinterpret_cast<Path*>(clone));
            }
        }
        /* PostgreSQL may leave a parameterized BioseqScan without a NestPath
         * using it. If no existing join is valid, combine the target scan
         * with the cheapest compatible outer path and use normal join costing. */
        if (retained.is_empty()) {
            postgres_list<Node> operands(mmsearch->args);
            Var* target_var = try_cast_pg_node<Var>(operands.get_first());
            Var* query_var = try_cast_pg_node<Var>(operands.get_second());
            local_audit.target_var_resolved = target_var == nullptr ? 0u : 1u;
            local_audit.query_var_resolved = query_var == nullptr ? 0u : 1u;
            // Identifies which current join input contains a Var's relation.
            auto relid_side_mask = [left_input, right_input](Var* var) noexcept {
                std::uint32_t mask = 0u;
                if (var == nullptr || !bioseq_is_concrete_rte_varno(var->varno)) {
                    return mask;
                }
                if (bms_is_member(static_cast<int>(var->varno), left_input->relids)) {
                    mask |= 1u;
                }
                if (bms_is_member(static_cast<int>(var->varno), right_input->relids)) {
                    mask |= 2u;
                }
                return mask;
            };
            local_audit.target_relid_side_mask = relid_side_mask(target_var);
            local_audit.query_relid_side_mask = relid_side_mask(query_var);
            RelOptInfo* target_input = nullptr;
            RelOptInfo* provider_input = nullptr;
            // Checks whether an input is exactly the target Var's base relation.
            auto input_is_target_base = [target_var](RelOptInfo* input) noexcept {
                return input != nullptr && target_var != nullptr &&
                       (input->reloptkind == RELOPT_BASEREL ||
                        input->reloptkind == RELOPT_OTHER_MEMBER_REL) &&
                       input->relid == target_var->varno;
            };
            if (input_is_target_base(left_input)) {
                local_audit.target_base_side_mask |= 1u;
                target_input = left_input;
                provider_input = right_input;
            } else if (input_is_target_base(right_input)) {
                local_audit.target_base_side_mask |= 2u;
                target_input = right_input;
                provider_input = left_input;
            }
            const bool provider_has_query =
                provider_input != nullptr && query_var != nullptr &&
                bioseq_is_concrete_rte_varno(query_var->varno) &&
                bms_is_member(static_cast<int>(query_var->varno), provider_input->relids);
            local_audit.provider_has_query = provider_has_query ? 1u : 0u;
            Path* best_bmo = nullptr;
            if (target_input != nullptr) {
                for (Path* candidate : postgres_list<Path>(target_input->pathlist)) {
                    ++local_audit.target_paths;
                    auto* custom = candidate != nullptr && IsA(candidate, CustomPath)
                                       ? reinterpret_cast<CustomPath*>(candidate)
                                       : nullptr;
                    if (custom != nullptr)
                        ++local_audit.target_custom_paths;
                    const bool bioseq_path =
                        custom != nullptr && custom->methods == bioseq_column_scan_path_methods();
                    if (bioseq_path)
                        ++local_audit.target_bioseq_paths;
                    std::size_t candidate_bmo_tags = 0u;
                    if (bioseq_path) {
                        for (Node* private_node : postgres_list<Node>(custom->custom_private)) {
                            if (private_node == nullptr || !IsA(private_node, List)) {
                                continue;
                            }
                            List* tag = reinterpret_cast<List*>(private_node);
                            Node* magic = list_length(tag) >= 1
                                              ? reinterpret_cast<Node*>(linitial(tag))
                                              : nullptr;
                            if (magic != nullptr && IsA(magic, Integer) &&
                                intVal(magic) == k_bioseq_mmsearch_owner_magic) {
                                ++candidate_bmo_tags;
                                ++local_audit.target_bmo_tags;
                            }
                        }
                    }
                    if (candidate_bmo_tags != 0u) {
                        bool invalid_owner_abi = false;
                        Node* recipe = per_row_base_scan_owner_recipe(custom, invalid_owner_abi);
                        if (candidate_bmo_tags != 1u || invalid_owner_abi || recipe == nullptr) {
                            local_audit.target_abi_mismatch_mask |=
                                k_exact_bmo_retainer_invalid_owner_tag;
                        }
                        std::size_t candidate_pointer_matches = 0u;
                        std::size_t candidate_serial_matches = 0u;
                        std::size_t candidate_clause_matches = 0u;
                        std::size_t candidate_exact_matches = 0u;
                        for (RestrictInfo* owned :
                             postgres_list<RestrictInfo>(custom->custom_restrictinfo)) {
                            ++local_audit.target_custom_restrictions;
                            if (owned == restriction) {
                                ++candidate_pointer_matches;
                                ++local_audit.target_custom_pointer_matches;
                            }
                            if (owned != nullptr && restriction != nullptr &&
                                owned->rinfo_serial > 0 &&
                                owned->rinfo_serial == restriction->rinfo_serial) {
                                ++candidate_serial_matches;
                                ++local_audit.target_custom_serial_matches;
                            }
                            if (owned != nullptr && restriction != nullptr &&
                                equal(owned->clause, restriction->clause)) {
                                ++candidate_clause_matches;
                                ++local_audit.target_custom_clause_matches;
                            }
                            if (same_planner_restriction_identity(owned, restriction)) {
                                ++candidate_exact_matches;
                                ++local_audit.target_custom_exact_matches;
                            }
                        }
                        if (custom->custom_restrictinfo == NIL) {
                            local_audit.target_abi_mismatch_mask |=
                                k_exact_bmo_retainer_missing_custom_restriction;
                        }
                        if (candidate_serial_matches == 0u) {
                            local_audit.target_abi_mismatch_mask |=
                                k_exact_bmo_retainer_missing_custom_serial;
                        }
                        if (candidate_clause_matches == 0u) {
                            local_audit.target_abi_mismatch_mask |=
                                k_exact_bmo_retainer_missing_custom_clause;
                        }
                        if (candidate_exact_matches == 0u) {
                            local_audit.target_abi_mismatch_mask |=
                                k_exact_bmo_retainer_missing_custom_identity;
                        } else if (candidate_exact_matches > 1u) {
                            local_audit.target_abi_mismatch_mask |=
                                k_exact_bmo_retainer_multiple_custom_identities;
                        }
                        if (candidate->param_info == nullptr) {
                            local_audit.target_ppi_mismatch_mask |=
                                k_exact_bmo_retainer_missing_param_info;
                        } else {
                            ++local_audit.target_ppi_paths;
                            std::size_t candidate_ppi_serial_matches = 0u;
                            std::size_t candidate_ppi_clause_matches = 0u;
                            std::size_t candidate_ppi_exact_matches = 0u;
                            for (RestrictInfo* parameter_clause :
                                 postgres_list<RestrictInfo>(candidate->param_info->ppi_clauses)) {
                                ++local_audit.target_ppi_clauses;
                                if (parameter_clause != nullptr && restriction != nullptr &&
                                    parameter_clause->rinfo_serial > 0 &&
                                    parameter_clause->rinfo_serial == restriction->rinfo_serial) {
                                    ++candidate_ppi_serial_matches;
                                    ++local_audit.target_ppi_serial_matches;
                                }
                                if (parameter_clause != nullptr && restriction != nullptr &&
                                    equal(parameter_clause->clause, restriction->clause)) {
                                    ++candidate_ppi_clause_matches;
                                    ++local_audit.target_ppi_clause_matches;
                                }
                                if (same_planner_restriction_identity(parameter_clause,
                                                                      restriction)) {
                                    ++candidate_ppi_exact_matches;
                                    ++local_audit.target_ppi_exact_matches;
                                }
                            }
                            if (candidate->param_info->ppi_clauses == NIL) {
                                local_audit.target_ppi_mismatch_mask |=
                                    k_exact_bmo_retainer_empty_ppi_clauses;
                            }
                            if (candidate_ppi_serial_matches == 0u) {
                                local_audit.target_ppi_mismatch_mask |=
                                    k_exact_bmo_retainer_missing_ppi_serial;
                            }
                            if (candidate_ppi_clause_matches == 0u) {
                                local_audit.target_ppi_mismatch_mask |=
                                    k_exact_bmo_retainer_missing_ppi_clause;
                            }
                            if (candidate_ppi_exact_matches == 0u) {
                                local_audit.target_ppi_mismatch_mask |=
                                    k_exact_bmo_retainer_missing_ppi_identity;
                            } else if (candidate_ppi_exact_matches > 1u) {
                                local_audit.target_ppi_mismatch_mask |=
                                    k_exact_bmo_retainer_multiple_ppi_identities;
                            }
                        }
                        (void)candidate_pointer_matches;
                    }
                    exact_parameterized_bmo_ownership candidate_analysis{};
                    count_exact_parameterized_bmo_owners(
                        root, candidate, restriction, candidate_analysis);
                    local_audit.target_owner_rejection_mask |= candidate_analysis.rejection_mask;
                    if (provider_has_query && candidate_analysis.matches != 0u) {
                        ++local_audit.available_bmo_paths;
                    }
                    Relids required = candidate == nullptr ? nullptr : PATH_REQ_OUTER(candidate);
                    if (candidate_bmo_tags != 0u || candidate_analysis.matches != 0u) {
                        if (candidate == nullptr || candidate->parent != target_input) {
                            local_audit.target_validation_mismatch_mask |=
                                k_exact_bmo_retainer_candidate_parent;
                        }
                        if (candidate_analysis.invalid) {
                            local_audit.target_validation_mismatch_mask |=
                                k_exact_bmo_retainer_candidate_owner_analysis;
                        }
                        if (candidate_analysis.matches != 1u) {
                            local_audit.target_validation_mismatch_mask |=
                                k_exact_bmo_retainer_candidate_owner_multiplicity;
                        }
                        if (required == nullptr) {
                            local_audit.target_validation_mismatch_mask |=
                                k_exact_bmo_retainer_candidate_unparameterized;
                        } else if (provider_has_query) {
                            if (!bms_is_member(static_cast<int>(query_var->varno), required)) {
                                local_audit.target_validation_mismatch_mask |=
                                    k_exact_bmo_retainer_candidate_missing_query_relid;
                            }
                            if (!bms_is_subset(required, provider_input->relids)) {
                                local_audit.target_validation_mismatch_mask |=
                                    k_exact_bmo_retainer_candidate_foreign_required_outer;
                            }
                        }
                        if (candidate != nullptr &&
                            (candidate->parallel_aware || candidate->parallel_workers != 0)) {
                            local_audit.target_validation_mismatch_mask |=
                                k_exact_bmo_retainer_candidate_parallel;
                        }
                        if (candidate != nullptr && !std::isfinite(candidate->total_cost)) {
                            local_audit.target_validation_mismatch_mask |=
                                k_exact_bmo_retainer_candidate_nonfinite_cost;
                        }
                        if (candidate != nullptr && candidate->total_cost < 0) {
                            local_audit.target_validation_mismatch_mask |=
                                k_exact_bmo_retainer_candidate_negative_cost;
                        }
                    }
                    if (!provider_has_query || candidate == nullptr ||
                        candidate->parent != target_input || candidate_analysis.invalid ||
                        candidate_analysis.matches != 1u || required == nullptr ||
                        !bms_is_member(static_cast<int>(query_var->varno), required) ||
                        !bms_is_subset(required, provider_input->relids) ||
                        candidate->parallel_aware || candidate->parallel_workers != 0 ||
                        !std::isfinite(candidate->total_cost) || candidate->total_cost < 0)
                        continue;
                    ++local_audit.compatible_bmo_paths;
                    if (best_bmo == nullptr || candidate->total_cost < best_bmo->total_cost) {
                        best_bmo = candidate;
                    }
                }
                if (best_bmo == nullptr && provider_has_query) {
                    ++local_audit.reconstructed_bmo_attempts;
                    exact_parameterized_bmo_reconstruction_audit reconstruction_audit{};
                    best_bmo = reconstruct_exact_parameterized_bmo_inner(root,
                                                                         target_input,
                                                                         provider_input,
                                                                         restriction,
                                                                         mmsearch,
                                                                         reconstruction_audit);
                    local_audit.reconstructed_bmo_rejection_mask |=
                        reconstruction_audit.rejection_mask;
                    local_audit.reconstructed_owner_abi_mismatch_mask |=
                        reconstruction_audit.owner_abi_mismatch_mask;
                    local_audit.reconstructed_expected_owner_count =
                        reconstruction_audit.expected_owner_count;
                    local_audit.reconstructed_owner_tag_count =
                        reconstruction_audit.owner_tag_count;
                    local_audit.reconstructed_owner_expression_count =
                        reconstruction_audit.owner_expression_count;
                    if (best_bmo != nullptr) {
                        ++local_audit.reconstructed_bmo_paths;
                        ++local_audit.available_bmo_paths;
                        ++local_audit.compatible_bmo_paths;
                    }
                }
            }

            Path* best_outer = nullptr;
            if (provider_input != nullptr && best_bmo != nullptr) {
                for (Path* candidate : postgres_list<Path>(provider_input->pathlist)) {
                    if (candidate == nullptr || candidate->parent != provider_input ||
                        candidate->parallel_aware || candidate->parallel_workers != 0 ||
                        bms_overlap(PATH_REQ_OUTER(candidate), target_input->relids) ||
                        !std::isfinite(candidate->total_cost) || candidate->total_cost < 0)
                        continue;
                    ++local_audit.available_outer_paths;
                    if (best_outer == nullptr || candidate->total_cost < best_outer->total_cost) {
                        best_outer = candidate;
                    }
                }
            }
            if (best_outer != nullptr && best_bmo != nullptr) {
                Relids available_to_inner =
                    bms_union(provider_input->relids, PATH_REQ_OUTER(best_outer));
                const bool inner_requirements_available =
                    bms_is_subset(PATH_REQ_OUTER(best_bmo), available_to_inner);
                bms_free(available_to_inner);
                postgres_list<RestrictInfo> residual_join_clauses;
                bool residual_mmsearch = false;
                for (RestrictInfo* clause : postgres_list<RestrictInfo>(extra->restrictlist)) {
                    if (same_planner_restriction_identity(clause, restriction))
                        continue;
                    if (clause != nullptr &&
                        count_mmsearch_occurrences(reinterpret_cast<Node*>(clause->clause),
                                                   get_cached_stub_oids()) != 0u) {
                        residual_mmsearch = true;
                        break;
                    }
                    residual_join_clauses.append(clause);
                }
                if (inner_requirements_available && !residual_mmsearch) {
                    Relids required_outer = calc_nestloop_required_outer(provider_input->relids,
                                                                         PATH_REQ_OUTER(best_outer),
                                                                         target_input->relids,
                                                                         PATH_REQ_OUTER(best_bmo));
                    if (bms_overlap(required_outer, joinrel->relids)) {
                        bms_free(required_outer);
                    } else {
                        JoinCostWorkspace workspace = initial_cost_serial_nested_loop(
                            root, JOIN_INNER, best_outer, best_bmo, extra);
                        NestPath* constructed = create_nestloop_path(
                            root,
                            joinrel,
                            JOIN_INNER,
                            &workspace,
                            extra,
                            best_outer,
                            best_bmo,
                            residual_join_clauses.get(),
                            build_join_pathkeys(root, joinrel, JOIN_INNER, best_outer->pathkeys),
                            required_outer);
                        bool constructed_has_mmsearch = false;
                        for (RestrictInfo* clause : postgres_list<RestrictInfo>(
                                 constructed == nullptr ? NIL
                                                        : constructed->jpath.joinrestrictinfo)) {
                            if (clause != nullptr &&
                                count_mmsearch_occurrences(reinterpret_cast<Node*>(clause->clause),
                                                           get_cached_stub_oids()) != 0u) {
                                constructed_has_mmsearch = true;
                                break;
                            }
                        }
                        if (constructed != nullptr && !constructed_has_mmsearch) {
                            retained.append(reinterpret_cast<Path*>(constructed));
                            ++local_audit.constructed_paths;
                        } else if (constructed == nullptr) {
                            bms_free(required_outer);
                        }
                    }
                }
            }
        }
        local_audit.retained_paths = static_cast<std::size_t>(retained.size());
        if (audit != nullptr)
            *audit = local_audit;
        if (retained.is_empty())
            return false;
        joinrel->pathlist = retained.release();
        joinrel->partial_pathlist = NIL;
        return true;
    }

} // export custom-scan partition declarations
