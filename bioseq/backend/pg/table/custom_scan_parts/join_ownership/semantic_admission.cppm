/**
 * @file
 * @brief Checks whether sequence expressions and match outputs can run in a BioseqScan or PerRow join.
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

export module bioseq.postgres.table:custom_scan_join_ownership_semantic_admission;

import std;
import bioseq.sequences.compact_sequence_typmod;
import bioseq.postgres.support;
import :custom_scan_oid_expression_support_oid_catalog;
import :custom_scan_oid_expression_support_expression_analysis;
import :custom_scan_slice_path_expression_contract;
import :custom_scan_join_ownership_helper_contracts;
import :custom_scan_join_ownership_alphabet_mismatch_contracts;
import :custom_scan_join_ownership_sequence_typmod_contract;
import :custom_scan_join_ownership_expression_signature_contract;
export import :custom_scan_join_ownership_target_contracts;
export import :custom_scan_join_ownership_semantic_admission_contracts;

using namespace bioseq::postgres::support;

export {
    /** @brief Stores a finite planner cost in a float8 Const for the plan. */
    [[nodiscard]] Const* make_batch_join_cost_const(const Cost value) {
        if (!std::isfinite(value)) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqBatchJoin planner cost is not finite")));
        }
        return makeConst(FLOAT8OID,
                         -1,
                         InvalidOid,
                         static_cast<int16>(sizeof(float8)),
                         Float8GetDatum(value),
                         false,
                         FLOAT8PASSBYVAL);
    }

    /** @brief Reads a finite BatchJoin cost from a float8 Const, reporting malformed values as errors. */
    [[nodiscard]] Cost batch_join_cost_from_node(Node* node, const char* field) {
        Const* value = try_cast_pg_node<Const>(node);
        if (value == nullptr || value->consttype != FLOAT8OID || value->constisnull ||
            value->constlen != static_cast<int16>(sizeof(float8)) ||
            value->constbyval != FLOAT8PASSBYVAL) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqBatchJoin cost ABI is invalid"),
                     errdetail("field=%s", field)));
        }
        const Cost decoded = DatumGetFloat8(value->constvalue);
        if (!std::isfinite(decoded)) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqBatchJoin cost ABI is not finite"),
                     errdetail("field=%s", field)));
        }
        return decoded;
    }

    /** @brief Checks that an operand uses columns from this join and collects its sequence columns. */
    struct per_row_operand_source_visitor {
        /** @brief Records a sequence column or rejects a node that cannot move into this join. */
        [[nodiscard]] pg_walk_step operator()(pg_expression_walk_cursor,
                                              pg_node_view node,
                                              per_row_operand_source_resolution& resolution) const {
            if (node.try_cast<PlaceHolderVar>() != nullptr) {
                /* Moving a PlaceHolderVar past the join could change when it is
                 * evaluated or becomes NULL. Reject it because this operand path
                 * cannot preserve those rules. */
                resolution.invalid = true;
                return pg_walk_step::stop;
            }
            Var* var = node.try_cast<Var>();
            if (var == nullptr)
                return pg_walk_step::descend;
            if (resolution.joinrel == nullptr || !bioseq_is_concrete_rte_varno(var->varno) ||
                var->varattno <= 0 || var->varlevelsup != 0 || !bms_is_empty(var->varnullingrels) ||
                !bms_is_member(static_cast<int>(var->varno), resolution.joinrel->relids)) {
                resolution.invalid = true;
                return pg_walk_step::stop;
            }
            if (var->vartype == get_cached_stub_oids().bioseq_seq_type_oid) {
                bool seen = false;
                for (Node* existing : postgres_list<Node>(resolution.sequence_vars)) {
                    if (equal(existing, var)) {
                        seen = true;
                        break;
                    }
                }
                if (!seen)
                    resolution.sequence_vars = lappend(resolution.sequence_vars, var);
            }
            return pg_walk_step::skip_subtree;
        }
    };

    /** @brief The target expression and relation sets used to check PerRow output. */
    struct per_row_output_semantic_validation {
        Index target_relid = 0;
        Relids join_relids = nullptr;
        Node* target_recipe = nullptr;
        bool invalid = false;
    };

    /** @brief Checks that output Vars and match helpers keep their meaning at the PerRow join. */
    struct per_row_output_semantic_visitor {
        /** @brief Checks one output node against the join and its search target. */
        [[nodiscard]] pg_walk_step
        operator()(pg_expression_walk_cursor,
                   pg_node_view node,
                   per_row_output_semantic_validation& validation) const {
            if (PlaceHolderVar* placeholder = node.try_cast<PlaceHolderVar>()) {
                /* Match helpers run after their owner produces a match_result. A
                 * helper for a sequence expression may belong to a higher join, so
                 * allow it here until that owner removes it from child outputs before
                 * PlanCustomPath. Other PlaceHolderVars must keep the evaluation
                 * level PostgreSQL chose for them. */
                FuncExpr* helper =
                    batch_join_match_helper_phv_function(placeholder, get_cached_stub_oids());
                Node* argument = batch_join_match_helper_argument(helper);
                const bool possible_higher_owner =
                    helper != nullptr && argument != nullptr &&
                    exprType(argument) == get_cached_stub_oids().bioseq_seq_type_oid;
                if (helper == nullptr ||
                    (!batch_join_match_helper_names_recipe(helper, validation.target_recipe) &&
                     !batch_join_match_helper_names_base_lineage(
                         helper, validation.target_recipe, get_cached_stub_oids()) &&
                     !possible_higher_owner)) {
                    validation.invalid = true;
                }
                return validation.invalid ? pg_walk_step::stop : pg_walk_step::skip_subtree;
            }
            if (Var* var = node.try_cast<Var>()) {
                /* UPDATE ... FROM needs ctid and tableoid from target and joined
                 * rows for EvalPlanQual rechecks. Pass these validated row markers
                 * through the inner join. */
                if (per_row_is_exact_row_identity_var(var) &&
                    bms_is_member(static_cast<int>(var->varno), validation.join_relids)) {
                    return pg_walk_step::skip_subtree;
                }
                if (!bioseq_is_concrete_rte_varno(var->varno) || var->varattno <= 0 ||
                    var->varlevelsup != 0 || !bms_is_empty(var->varnullingrels)) {
                    validation.invalid = true;
                    return pg_walk_step::stop;
                }
                return pg_walk_step::skip_subtree;
            }
            return pg_walk_step::descend;
        }
    };

    /** @brief Checks that the PerRow join can produce this output with the same Var and helper bindings. */
    [[nodiscard]] bool per_row_output_semantics_are_supported(PathTarget* target,
                                                              const Index target_relid,
                                                              Relids join_relids,
                                                              Node* target_recipe) {
        if (target == nullptr || !bioseq_is_concrete_rte_varno(target_relid) ||
            join_relids == nullptr || target_recipe == nullptr) {
            return false;
        }
        /* PostgreSQL evaluates volatile output expressions once for each
         * emitted match through ProjectionInfo. Check only the nodes whose
         * evaluation level or NULL behavior could change. */
        per_row_output_semantic_validation validation{
            target_relid, join_relids, target_recipe, false};
        per_row_output_semantic_visitor visitor;
        for (Node* expression : postgres_list<Node>(target->exprs)) {
            walk_pg_expression_including_root(expression, validation, visitor);
            if (validation.invalid)
                return false;
        }
        return true;
    }

    /** @brief Appends a copied expression unless an equal expression is already present. */
    void per_row_append_unique_expression(List*& expressions, Node* node) {
        if (node == nullptr)
            return;
        for (Node* existing : postgres_list<Node>(expressions)) {
            if (equal(existing, node))
                return;
        }
        expressions = lappend(expressions, copyObject(node));
    }

    /**
     * @brief Copies the join's output with PostgreSQL's matching sort and group references.
     *
     * Keep the expression list at its original size. PlanCustomPath adds helper
     * and hidden input columns later, after PostgreSQL has finished replacing
     * scan and join targets. Adding them now could leave a longer expression
     * list paired with a shorter sortgroupref array.
     */
    [[nodiscard]] PathTarget* per_row_build_owner_target(PlannerInfo* root, PathTarget* original) {
        if (root == nullptr || original == nullptr)
            return nullptr;
        std::vector<Index> sortgrouprefs;
        if (!per_row_authoritative_sortgrouprefs(root, original->exprs, sortgrouprefs))
            return nullptr;
        if (sortgrouprefs.size() != static_cast<std::size_t>(list_length(original->exprs)))
            return nullptr;
        PathTarget* rebuilt = create_empty_pathtarget();
        rebuilt->exprs = static_cast<List*>(copyObject(original->exprs));
        const bool has_sortgroupref = std::any_of(sortgrouprefs.begin(),
                                                  sortgrouprefs.end(),
                                                  [](Index value) noexcept { return value != 0; });
        if (has_sortgroupref) {
            rebuilt->sortgrouprefs =
                static_cast<Index*>(palloc0(sortgrouprefs.size() * sizeof(Index)));
            for (std::size_t index = 0u; index < sortgrouprefs.size(); ++index) {
                rebuilt->sortgrouprefs[index] = sortgrouprefs[index];
            }
        }
        Assert(list_length(rebuilt->exprs) == list_length(original->exprs));
        return rebuilt;
    }

    /**
     * @brief Collects hidden input columns needed when PostgreSQL fixes plan references.
     *
     * setrefs can reach a helper's argument before it is bound to match_result.
     * Carry the target expression's Vars and validated helper arguments as
     * resjunk columns in the Plan. Keep them out of the public PathTarget.
     */
    [[nodiscard]] bool per_row_collect_setrefs_carrier_vars(Node* target_recipe,
                                                            List* helper_argument_vars,
                                                            Relids join_relids,
                                                            List*& carrier_vars) {
        carrier_vars = NIL;
        if (target_recipe == nullptr || join_relids == nullptr)
            return false;
        constexpr int pull_flags =
            PVC_RECURSE_AGGREGATES | PVC_RECURSE_WINDOWFUNCS | PVC_RECURSE_PLACEHOLDERS;
        List* recipe_vars = pull_var_clause(target_recipe, pull_flags);
        // Adds a validated local join column or row marker to the hidden inputs.
        auto append_valid = [join_relids, &carrier_vars](Node* node) -> bool {
            Var* var = try_cast_pg_node<Var>(node);
            const bool row_identity = per_row_is_exact_row_identity_var(var);
            if (var == nullptr || !bioseq_is_concrete_rte_varno(var->varno) ||
                (var->varattno <= 0 && !row_identity) || var->varlevelsup != 0 ||
                !bms_is_empty(var->varnullingrels) ||
                !bms_is_member(static_cast<int>(var->varno), join_relids))
                return false;
            per_row_append_unique_expression(carrier_vars, node);
            return true;
        };
        for (Node* recipe_var : postgres_list<Node>(recipe_vars)) {
            if (!append_valid(recipe_var))
                return false;
        }
        for (Node* helper_argument : postgres_list<Node>(helper_argument_vars)) {
            if (!append_valid(helper_argument))
                return false;
        }
        return carrier_vars != NIL;
    }

    /**
     * @brief Collects columns and whole match helpers needed by the final output.
     *
     * PostgreSQL can place SELECT or RETURNING above the join and replace its
     * targetlist after PlanCustomPath returns. Collect those dependencies now.
     * Keep each helper intact, and record its source argument separately where
     * setrefs needs an input column below the owner.
     */
    struct per_row_owner_support_visitor {
        /** @brief Records a required column or match helper, rejecting unsupported helper bindings. */
        [[nodiscard]] pg_walk_step operator()(pg_expression_walk_cursor,
                                              pg_node_view node,
                                              per_row_owner_support_validation& validation) const {
            if (PlaceHolderVar* placeholder = node.try_cast<PlaceHolderVar>()) {
                FuncExpr* helper =
                    validation.oids == nullptr
                        ? nullptr
                        : batch_join_match_helper_phv_function(placeholder, *validation.oids);
                if (helper != nullptr) {
                    const bool names_recipe =
                        batch_join_match_helper_names_recipe(helper, validation.target_recipe);
                    const bool names_lineage = batch_join_match_helper_names_base_lineage(
                        helper, validation.target_recipe, *validation.oids);
                    Node* argument = batch_join_match_helper_argument(helper);
                    const bool possible_foreign_exact =
                        !names_recipe && !names_lineage && argument != nullptr &&
                        exprType(argument) == validation.oids->bioseq_seq_type_oid;
                    if (!names_recipe && !names_lineage && !possible_foreign_exact) {
                        validation.invalid = true;
                        return pg_walk_step::stop;
                    }
                    /* The join can expose the whole PlaceHolderVar while an upper
                     * Sort or Result contains its FuncExpr. Record both forms so
                     * setrefs can bind either to the same match result. Treat the
                     * helper as a whole here. */
                    per_row_append_unique_expression(validation.helper_identities,
                                                     reinterpret_cast<Node*>(placeholder));
                    per_row_append_unique_expression(validation.helper_identities,
                                                     reinterpret_cast<Node*>(helper));
                    if (possible_foreign_exact) {
                        per_row_append_unique_expression(validation.foreign_exact_helper_identities,
                                                         reinterpret_cast<Node*>(placeholder));
                        per_row_append_unique_expression(validation.foreign_exact_helper_identities,
                                                         reinterpret_cast<Node*>(helper));
                    }
                    /* A Result above BioseqScan may still need the helper's source
                     * column while the whole helper travels to its owner. Preserve
                     * that argument for setrefs without evaluating the helper here. */
                    if (names_lineage) {
                        per_row_append_unique_expression(validation.helper_argument_vars,
                                                         batch_join_match_helper_argument(helper));
                    }
                    return pg_walk_step::skip_subtree;
                }
                if (validation.oids != nullptr &&
                    batch_join_expression_has_match_helper(
                        reinterpret_cast<Node*>(placeholder->phexpr), *validation.oids)) {
                    validation.invalid = true;
                    return pg_walk_step::stop;
                }
                /* Keep an ordinary PlaceHolderVar at PostgreSQL's chosen evaluation level. */
                return pg_walk_step::skip_subtree;
            }
            if (FuncExpr* function = node.try_cast<FuncExpr>();
                validation.oids != nullptr && function != nullptr &&
                is_bioseq_projection_funcid(function->funcid, *validation.oids)) {
                const bool names_recipe =
                    batch_join_match_helper_names_recipe(function, validation.target_recipe);
                const bool base_lineage = batch_join_match_helper_names_base_lineage(
                    function, validation.target_recipe, *validation.oids);
                Node* argument = batch_join_match_helper_argument(function);
                const bool possible_foreign_exact =
                    !names_recipe && !base_lineage && argument != nullptr &&
                    exprType(argument) == validation.oids->bioseq_seq_type_oid;
                if (!names_recipe && !base_lineage && !possible_foreign_exact) {
                    validation.invalid = true;
                    return pg_walk_step::stop;
                }
                per_row_append_unique_expression(validation.helper_identities,
                                                 reinterpret_cast<Node*>(function));
                if (possible_foreign_exact) {
                    per_row_append_unique_expression(validation.foreign_exact_helper_identities,
                                                     reinterpret_cast<Node*>(function));
                }
                if (base_lineage) {
                    per_row_append_unique_expression(validation.helper_argument_vars,
                                                     batch_join_match_helper_argument(function));
                }
                return pg_walk_step::skip_subtree;
            }
            if (Var* var = node.try_cast<Var>()) {
                if (var->varlevelsup != 0) {
                    return pg_walk_step::skip_subtree;
                }
                if (!bioseq_is_concrete_rte_varno(var->varno)) {
                    validation.invalid = true;
                    return pg_walk_step::stop;
                }
                if (!bms_is_member(static_cast<int>(var->varno), validation.join_relids)) {
                    return pg_walk_step::skip_subtree;
                }
                const bool row_identity = per_row_is_exact_row_identity_var(var);
                if ((var->varattno <= 0 && !row_identity) || !bms_is_empty(var->varnullingrels)) {
                    validation.invalid = true;
                    return pg_walk_step::stop;
                }
                per_row_append_unique_expression(validation.passthrough_vars,
                                                 reinterpret_cast<Node*>(var));
                return pg_walk_step::skip_subtree;
            }
            return pg_walk_step::descend;
        }
    };

    /** @brief Collects the final output's input columns and match helpers for this PerRow join. */
    [[nodiscard]] bool per_row_collect_owner_support(PlannerInfo* root,
                                                     RelOptInfo* joinrel,
                                                     const Index target_relid,
                                                     Node* target_recipe,
                                                     per_row_owner_support_validation& validation) {
        if (root == nullptr || joinrel == nullptr || !bioseq_is_concrete_rte_varno(target_relid) ||
            joinrel->relids == nullptr || target_recipe == nullptr) {
            return false;
        }
        validation.target_relid = target_relid;
        validation.join_relids = joinrel->relids;
        validation.oids = &get_cached_stub_oids();
        validation.target_recipe = target_recipe;
        per_row_owner_support_visitor visitor;
        for (TargetEntry* output : postgres_list<TargetEntry>(root->processed_tlist)) {
            if (output == nullptr || output->expr == nullptr)
                continue;
            walk_pg_expression_including_root(
                reinterpret_cast<Node*>(output->expr), validation, visitor);
            if (validation.invalid)
                return false;
        }
        return true;
    }

    /** @brief Rejects volatile join operands and collects the sequence columns they use. */
    [[nodiscard]] bool per_row_collect_operand_sources(
        Node* expression, RelOptInfo* joinrel, per_row_operand_source_resolution& resolution) {
        if (expression == nullptr || joinrel == nullptr || contain_volatile_functions(expression))
            return false;
        resolution.joinrel = joinrel;
        per_row_operand_source_visitor visitor;
        walk_pg_expression_including_root(expression, resolution, visitor);
        return !resolution.invalid;
    }

    /** @brief The scan relation, registered functions, and sequence columns found in an operand. */
    struct base_local_mmsearch_operand_walker {
        RelOptInfo* rel = nullptr;
        const bioseq_stub_oids* oids = nullptr;
        List* sequence_vars = NIL;
        bool invalid = false;
    };

    /** @brief Rejects operands that need evaluation outside this base scan. */
    struct base_local_mmsearch_operand_visitor {
        /** @brief Validates one operand node and records distinct sequence Vars. */
        [[nodiscard]] pg_walk_step
        operator()(pg_expression_walk_cursor,
                   pg_node_view node,
                   base_local_mmsearch_operand_walker& validation_state) const {
            Node* raw = node.get();
            if (raw == nullptr || validation_state.rel == nullptr ||
                validation_state.oids == nullptr) {
                validation_state.invalid = true;
                return pg_walk_step::stop;
            }
            if (IsA(raw, PlaceHolderVar) || IsA(raw, Aggref) || IsA(raw, GroupingFunc) ||
                IsA(raw, WindowFunc) || IsA(raw, SubPlan) || IsA(raw, AlternativeSubPlan) ||
                IsA(raw, SubLink)) {
                validation_state.invalid = true;
                return pg_walk_step::stop;
            }
            if (FuncExpr* function = node.try_cast<FuncExpr>()) {
                if (function->funcretset ||
                    is_mmsearch_funcid(function->funcid, *validation_state.oids)) {
                    validation_state.invalid = true;
                    return pg_walk_step::stop;
                }
            }
            if (OpExpr* operation = node.try_cast<OpExpr>();
                operation != nullptr && operation->opretset) {
                validation_state.invalid = true;
                return pg_walk_step::stop;
            }
            Var* variable = node.try_cast<Var>();
            if (variable == nullptr)
                return pg_walk_step::descend;
            if (!bioseq_is_concrete_rte_varno(variable->varno) ||
                variable->varno != validation_state.rel->relid || variable->varattno <= 0 ||
                variable->varlevelsup != 0 || !bms_is_empty(variable->varnullingrels)) {
                validation_state.invalid = true;
                return pg_walk_step::stop;
            }
            if (variable->vartype == validation_state.oids->bioseq_seq_type_oid) {
                bool duplicate = false;
                for (Var* existing : postgres_list<Var>(validation_state.sequence_vars)) {
                    if (equal(existing, variable)) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate)
                    validation_state.sequence_vars =
                        lappend(validation_state.sequence_vars, variable);
            }
            return pg_walk_step::skip_subtree;
        }
    };

    /** @brief Finds the sequence alphabet and storage width through casts, branches, and supported sequence operations. */
    [[nodiscard]] base_local_sequence_typmod_resolution
    resolve_base_local_sequence_typmod(Node* expression, const bioseq_stub_oids& oids) {
        check_stack_depth();
        if (expression == nullptr || exprType(expression) != oids.bioseq_seq_type_oid) {
            return {.invalid = true};
        }
        // Recognizes concrete typmods supported by the alignment runtime.
        auto supported_descriptor = [](const int32 typmod) noexcept {
            return typmod >= 0 && batch_join_typmod_is_supported(typmod);
        };
        // Checks the expression's declared sequence type against the type found in its children.
        auto finish = [expression,
                       &supported_descriptor](base_local_sequence_typmod_resolution derived) {
            if (derived.invalid)
                return derived;
            const int32 descriptor = exprTypmod(expression);
            if (descriptor < -1 || (descriptor >= 0 && !supported_descriptor(descriptor))) {
                return base_local_sequence_typmod_resolution{.invalid = true};
            }
            if (descriptor >= 0) {
                if (derived.typmod >= 0) {
                    const auto declared = compact_sequence_stdc_type_params::decode(descriptor);
                    const auto child = compact_sequence_stdc_type_params::decode(derived.typmod);
                    if (declared.alphabet_code != child.alphabet_code) {
                        return base_local_sequence_typmod_resolution{.invalid = true};
                    }
                }
                derived.typmod = descriptor;
            }
            return derived;
        };
        // Requires known CASE or COALESCE result types to agree.
        auto merge_exact = [](base_local_sequence_typmod_resolution& destination,
                              const base_local_sequence_typmod_resolution source) {
            if (source.invalid) {
                destination.invalid = true;
                return;
            }
            /* An arm with no known typmod contributes no type information. If
             * selected, its returned Datum is checked before dispatch. Type
             * analysis must not evaluate an unused arm that could throw. */
            if (source.typmod < 0)
                return;
            if (destination.typmod < 0) {
                destination.typmod = source.typmod;
                return;
            }
            if (destination.typmod != source.typmod)
                destination.invalid = true;
        };

        if (Node* source = slice_project_typmod_cast_source(expression, oids)) {
            base_local_sequence_typmod_resolution child =
                resolve_base_local_sequence_typmod(source, oids);
            Const* target_typmod = try_cast_pg_node<Const>(
                reinterpret_cast<Node*>(lsecond(reinterpret_cast<FuncExpr*>(expression)->args)));
            if (child.invalid || target_typmod == nullptr || target_typmod->constisnull)
                return {.invalid = true};
            const int32 cast_typmod = DatumGetInt32(target_typmod->constvalue);
            if (!supported_descriptor(cast_typmod))
                return {.invalid = true};
            if (child.typmod >= 0 &&
                compact_sequence_stdc_type_params::decode(child.typmod).alphabet_code !=
                    compact_sequence_stdc_type_params::decode(cast_typmod).alphabet_code)
                return {.invalid = true};
            return base_local_sequence_typmod_resolution{cast_typmod, false};
        }
        if (IsA(expression, RelabelType)) {
            auto* relabel = reinterpret_cast<RelabelType*>(expression);
            return finish(
                resolve_base_local_sequence_typmod(reinterpret_cast<Node*>(relabel->arg), oids));
        }
        if (nodeTag(expression) == T_CaseExpr) {
            CaseExpr* choice = castNode(CaseExpr, expression);
            base_local_sequence_typmod_resolution result{};
            for (CaseWhen* arm : postgres_list<CaseWhen>(choice->args)) {
                if (arm == nullptr || arm->result == nullptr ||
                    exprType(reinterpret_cast<Node*>(arm->result)) != oids.bioseq_seq_type_oid) {
                    result.invalid = true;
                    break;
                }
                merge_exact(
                    result,
                    resolve_base_local_sequence_typmod(reinterpret_cast<Node*>(arm->result), oids));
                if (result.invalid)
                    break;
            }
            if (!result.invalid && choice->defresult != nullptr) {
                merge_exact(result,
                            resolve_base_local_sequence_typmod(
                                reinterpret_cast<Node*>(choice->defresult), oids));
            }
            return finish(result);
        }
        if (nodeTag(expression) == T_CoalesceExpr) {
            CoalesceExpr* choice = castNode(CoalesceExpr, expression);
            base_local_sequence_typmod_resolution result{};
            for (Node* arm : postgres_list<Node>(choice->args)) {
                merge_exact(result, resolve_base_local_sequence_typmod(arm, oids));
                if (result.invalid)
                    break;
            }
            return finish(result);
        }

        const auto opcode = slice_project_node_opcode(expression, oids);
        if (opcode.has_value() && *opcode == slice_project_opcode::slice) {
            auto* subscript = reinterpret_cast<SubscriptingRef*>(expression);
            if (subscript->refexpr == nullptr || subscript->refassgnexpr != nullptr ||
                subscript->refcontainertype != oids.bioseq_seq_type_oid ||
                subscript->refrestype != oids.bioseq_seq_type_oid) {
                return {.invalid = true};
            }
            return finish(resolve_base_local_sequence_typmod(
                reinterpret_cast<Node*>(subscript->refexpr), oids));
        }
        if (opcode.has_value() && (*opcode == slice_project_opcode::reverse ||
                                   *opcode == slice_project_opcode::reverse_complement)) {
            FuncExpr* function = try_cast_pg_node<FuncExpr>(expression);
            if (function == nullptr || list_length(function->args) != 1 ||
                !per_row_function_signature_is_exact(function)) {
                return {.invalid = true};
            }
            return finish(resolve_base_local_sequence_typmod(
                reinterpret_cast<Node*>(linitial(function->args)), oids));
        }
        if (opcode.has_value() && *opcode == slice_project_opcode::concat) {
            List* arguments = NIL;
            if (FuncExpr* function = try_cast_pg_node<FuncExpr>(expression)) {
                if (!slice_project_concat_oid(function->funcid, oids) ||
                    !per_row_function_signature_is_exact(function)) {
                    return {.invalid = true};
                }
                arguments = function->args;
            } else if (OpExpr* operation = try_cast_pg_node<OpExpr>(expression)) {
                if (!slice_project_concat_oid(operation->opfuncid, oids) ||
                    !per_row_operator_signature_is_exact(operation)) {
                    return {.invalid = true};
                }
                arguments = operation->args;
            } else {
                return {.invalid = true};
            }
            base_local_sequence_typmod_resolution result{};
            for (Node* argument : postgres_list<Node>(arguments)) {
                if (exprType(argument) != oids.bioseq_seq_type_oid)
                    continue;
                const auto child = resolve_base_local_sequence_typmod(argument, oids);
                if (child.invalid)
                    return child;
                if (child.typmod < 0)
                    continue;
                if (result.typmod < 0) {
                    /* Concat takes its storage width from the first sequence operand
                     * with a known typmod. Later known types must use the same alphabet. */
                    result.typmod = child.typmod;
                    continue;
                }
                if (compact_sequence_stdc_type_params::decode(result.typmod).alphabet_code !=
                    compact_sequence_stdc_type_params::decode(child.typmod).alphabet_code)
                    return {.invalid = true};
            }
            return finish(result);
        }

        /* Vars, constants, parameters, and other sequence-producing
         * expressions can supply a known typmod. An expression without one
         * needs type information from its surrounding expression before
         * the scan can accept the complete operand. */
        const int32 descriptor = exprTypmod(expression);
        return descriptor >= 0 && supported_descriptor(descriptor)
                   ? base_local_sequence_typmod_resolution{descriptor, false}
                   : base_local_sequence_typmod_resolution{};
    }

    /** @brief Checks that this sequence expression can run inside the scan and determines its input columns and type. */
    [[nodiscard]] std::optional<base_local_mmsearch_operand_resolution>
    resolve_base_local_mmsearch_operand(Node* expression,
                                        RelOptInfo* rel,
                                        const bioseq_stub_oids& oids,
                                        const bool require_sequence_var) {
        if (expression == nullptr || rel == nullptr ||
            exprType(expression) != oids.bioseq_seq_type_oid ||
            expression_returns_set(expression) || contain_subplans(expression))
            return std::nullopt;
        base_local_mmsearch_operand_walker walker{rel, &oids, NIL, false};
        base_local_mmsearch_operand_visitor visitor;
        walk_pg_expression_including_root(expression, walker, visitor);
        if (walker.invalid || (require_sequence_var && walker.sequence_vars == NIL)) {
            return std::nullopt;
        }
        const base_local_sequence_typmod_resolution type =
            resolve_base_local_sequence_typmod(expression, oids);
        if (type.invalid || type.typmod < 0 || !batch_join_typmod_is_supported(type.typmod))
            return std::nullopt;
        Var* raw = try_cast_pg_node<Var>(expression);
        return base_local_mmsearch_operand_resolution{
            .expression = expression,
            .sequence_vars = walker.sequence_vars,
            .primary_sequence_var = walker.sequence_vars == NIL
                                        ? nullptr
                                        : reinterpret_cast<Var*>(linitial(walker.sequence_vars)),
            .typmod = type.typmod,
            .evaluated = raw == nullptr,
        };
    }

    /**
     * @brief Checks whether a BioseqScan can own this mmsearch call at its current evaluation level.
     * @throws PostgreSQL ERROR when a supported non-NULL query constant has a different alphabet from the target.
     */
    [[nodiscard]] std::optional<base_local_mmsearch_ownership>
    resolve_base_local_mmsearch_ownership(FuncExpr* function,
                                          RelOptInfo* rel,
                                          Relids param_relids,
                                          const bioseq_stub_oids& oids) {
        if (function == nullptr || rel == nullptr || !is_mmsearch_funcid(function->funcid, oids))
            return std::nullopt;
        pg_first_two_node_arguments arguments =
            get_pg_first_two_arguments_if_arity_between(*function, 2, 3);
        if (!arguments.is_present())
            return std::nullopt;
        auto target = resolve_base_local_mmsearch_operand(arguments.first_argument,
                                                          rel,
                                                          oids,
                                                          /*require_sequence_var=*/true);
        if (!target.has_value())
            return std::nullopt;
        base_local_mmsearch_ownership result{};
        result.target = *target;
        result.any_evaluated_operand = target->evaluated;
        Node* query = arguments.second_argument;
        if (Const* constant = try_cast_pg_node<Const>(query)) {
            if (constant->consttype != oids.bioseq_seq_type_oid || constant->consttypmod < 0 ||
                !batch_join_typmod_is_supported(constant->consttypmod)) {
                return std::nullopt;
            }
            if (!constant->constisnull &&
                !mmsearch_typmods_share_alphabet(target->typmod, constant->consttypmod)) {
                raise_mmsearch_alphabet_mismatch(
                    compact_sequence_stdc_type_params::decode(target->typmod),
                    compact_sequence_stdc_type_params::decode(constant->consttypmod),
                    "column scan planning");
            }
            result.query_is_const = true;
            return result;
        }
        if (Param* parameter = try_cast_pg_node<Param>(query)) {
            const bool runtime_typed_external =
                parameter->paramkind == PARAM_EXTERN && parameter->paramtypmod < 0;
            if (parameter->paramtype != oids.bioseq_seq_type_oid ||
                (parameter->paramkind != PARAM_EXEC && parameter->paramkind != PARAM_EXTERN) ||
                (!runtime_typed_external &&
                 (parameter->paramtypmod < 0 ||
                  !batch_join_typmod_is_supported(parameter->paramtypmod) ||
                  !mmsearch_typmods_share_alphabet(target->typmod, parameter->paramtypmod)))) {
                return std::nullopt;
            }
            result.query_is_param = true;
            return result;
        }
        if (Var* variable = try_cast_pg_node<Var>(query);
            variable != nullptr && variable->varno != rel->relid) {
            if (!bioseq_is_concrete_rte_varno(variable->varno) ||
                !bms_is_member(static_cast<int>(variable->varno), param_relids) ||
                variable->varattno <= 0 || variable->varlevelsup != 0 ||
                !bms_is_empty(variable->varnullingrels) ||
                variable->vartype != oids.bioseq_seq_type_oid || variable->vartypmod < 0 ||
                !batch_join_typmod_is_supported(variable->vartypmod) ||
                !mmsearch_typmods_share_alphabet(target->typmod, variable->vartypmod))
                return std::nullopt;
            result.query_is_outer_var = true;
            return result;
        }
        auto local_query =
            resolve_base_local_mmsearch_operand(query, rel, oids, /*require_sequence_var=*/false);
        if (!local_query.has_value() ||
            !mmsearch_typmods_share_alphabet(target->typmod, local_query->typmod))
            return std::nullopt;
        result.any_evaluated_operand = result.any_evaluated_operand || local_query->evaluated;
        result.local_query = *local_query;
        return result;
    }

} // export custom-scan partition declarations
