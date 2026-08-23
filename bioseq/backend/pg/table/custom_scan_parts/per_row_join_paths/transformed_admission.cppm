/**
 * @file
 * @brief Admits verified transformed mmsearch join paths.
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

export module bioseq.postgres.table:custom_scan_per_row_join_paths_transformed_admission;

import std;
import bioseq.common.checked_arithmetic;
import bioseq.highway.dispatch_contract;
import bioseq.indexing.type;
import bioseq.postgres.column.table_access;
import bioseq.postgres.support;
import bioseq.sequences.compact_sequence_typmod;

import :custom_scan_plan_abi_constants;
import :custom_scan_runtime_buffer_contracts;
import :custom_scan_oid_expression_support_oid_catalog;
import :custom_scan_planner_aligner_contracts;
import :custom_scan_planner_method_contracts;
import :custom_scan_join_ownership_helper_contracts;
import :custom_scan_join_ownership_carrier_lineage_contracts;
import :custom_scan_join_ownership_sequence_typmod_validation_contracts;
import :custom_scan_join_ownership_descendant_path_contracts;
import :custom_scan_join_ownership_semantic_admission_contracts;
import :custom_scan_join_ownership_target_contracts;
import :custom_scan_slice_path_codec_contract;
import :custom_scan_slice_path_carrier_contracts;
import :custom_scan_slice_rewrite_path_contracts;
import :custom_scan_slice_rewrite_path_tagging_contracts;
import :custom_scan_per_row_join_paths_carrier_contracts;
import :custom_scan_per_row_join_paths_target_normalization_contracts;
import :custom_scan_per_row_join_paths_reconstruction_contracts;
export import :custom_scan_per_row_join_paths_transformed_admission_contracts;

using namespace bioseq::postgres::support;

export {
    /**
     * @brief Builds per-row BatchJoin owners for an admissible transformed mmsearch restriction.
     *
     * Successful admission replaces the join relation's competing paths with planner-context
     * CustomPaths that preserve the reconstructed child metadata and helper carriers.
     * @param root Planner state used for path reconstruction, costing, and pathtarget edits.
     * @param joinrel Inner join relation whose pathlist may be replaced on success.
     * @param extra Join-path extras holding residual restrictions evaluated alongside the owned mmsearch clause.
     * @param restriction RestrictInfo for the transformed mmsearch predicate transferred into the owner CustomPath.
     * @param mmsearch Planner FuncExpr whose first and second arguments are the target and query sequence operands.
     * @param executable_mmsearch Executable mmsearch FuncExpr used for aligner overrides and operand costing.
     * @param requirement Index read policy, admission declines when persisted-compatible reads are required.
     * @param oids Bioseq type OIDs used for typmod, helper, and alphabet validation.
     * @return True when at least one owner path was added to the join relation.
     * @throws PostgreSQL ERROR when reconstruction leaks the transferred RestrictInfo.
     */
    [[nodiscard]] bool
    per_row_add_transformed_mmsearch_paths(PlannerInfo* root,
                                           RelOptInfo* joinrel,
                                           JoinPathExtraData* extra,
                                           RestrictInfo* restriction,
                                           FuncExpr* mmsearch,
                                           FuncExpr* executable_mmsearch,
                                           bioseq::indexing::index_read_requirement requirement,
                                           bioseq_stub_oids const& oids) {
        if (root == nullptr || joinrel == nullptr || extra == nullptr || restriction == nullptr ||
            mmsearch == nullptr || executable_mmsearch == nullptr)
            return false;
        postgres_list<Node> operands(mmsearch->args);
        Node* target_expression = operands.get_first();
        Node* query_expression = operands.get_second();
        const auto target_typmod_validation_result =
            per_row_validate_sequence_typmod(target_expression, oids);
        const auto query_typmod_validation_result =
            per_row_validate_sequence_typmod(query_expression, oids);
        if (target_typmod_validation_result.has_value() &&
            query_typmod_validation_result.has_value() &&
            batch_join_typmod_is_supported(target_typmod_validation_result->typmod) &&
            batch_join_typmod_is_supported(query_typmod_validation_result->typmod) &&
            !mmsearch_typmods_share_alphabet(target_typmod_validation_result->typmod,
                                             query_typmod_validation_result->typmod)) {
            raise_mmsearch_alphabet_mismatch(
                compact_sequence_stdc_type_params::decode(target_typmod_validation_result->typmod),
                compact_sequence_stdc_type_params::decode(query_typmod_validation_result->typmod),
                "join planning");
        }
        if (exprType(target_expression) != oids.bioseq_seq_type_oid ||
            exprType(query_expression) != oids.bioseq_seq_type_oid ||
            !target_typmod_validation_result.has_value() ||
            !query_typmod_validation_result.has_value() ||
            !batch_join_typmod_is_supported(target_typmod_validation_result->typmod) ||
            !batch_join_typmod_is_supported(query_typmod_validation_result->typmod) ||
            !mmsearch_typmods_share_alphabet(target_typmod_validation_result->typmod,
                                             query_typmod_validation_result->typmod) ||
            exprCollation(target_expression) != exprCollation(query_expression) ||
            requirement == bioseq::indexing::index_read_requirement::require_compatible_persisted) {
            return per_row_decline_transformed_path("typmod_or_requirement");
        }
        per_row_operand_source_resolution target_sources{};
        per_row_operand_source_resolution query_sources{};
        if (!per_row_collect_operand_sources(target_expression, joinrel, target_sources) ||
            !per_row_collect_operand_sources(query_expression, joinrel, query_sources)) {
            return per_row_decline_transformed_path("operand_sources");
        }
        Var* target_source = nullptr;
        RangeTblEntry* target_rte = nullptr;
        for (Var* candidate : postgres_list<Var>(target_sources.sequence_vars)) {
            if (!bioseq_is_concrete_rte_varno(candidate->varno) ||
                candidate->varno > static_cast<Index>(list_length(root->parse->rtable)))
                continue;
            RangeTblEntry* rte = rt_fetch(candidate->varno, root->parse->rtable);
            if (rte != nullptr && rte->rtekind == RTE_RELATION && rte->securityQuals == NIL &&
                OidIsValid(rte->relid) &&
                relation_oid_uses_column_table_access_method(rte->relid)) {
                target_source = candidate;
                target_rte = rte;
                break;
            }
        }
        if (target_source == nullptr || target_rte == nullptr) {
            return per_row_decline_transformed_path("target_source");
        }
        if (!per_row_output_semantics_are_supported(
                joinrel->reltarget, target_source->varno, joinrel->relids, target_expression)) {
            return per_row_decline_transformed_path("output_semantics");
        }
        for (Var* source : postgres_list<Var>(query_sources.sequence_vars)) {
            if (!bioseq_is_concrete_rte_varno(source->varno) ||
                source->varno > static_cast<Index>(list_length(root->parse->rtable))) {
                return per_row_decline_transformed_path("query_source_rti");
            }
            RangeTblEntry* rte = rt_fetch(source->varno, root->parse->rtable);
            if (rte == nullptr || rte->rtekind != RTE_RELATION || rte->securityQuals != NIL) {
                return per_row_decline_transformed_path("query_source_security");
            }
        }
        per_row_owner_support_validation owner_support{};
        if (!per_row_collect_owner_support(
                root, joinrel, target_source->varno, target_expression, owner_support)) {
            return per_row_decline_transformed_path("owner_support");
        }
        PathTarget* required_target = create_empty_pathtarget();
        List* required_vars = NIL;
        constexpr int pull_flags =
            PVC_RECURSE_AGGREGATES | PVC_RECURSE_WINDOWFUNCS | PVC_RECURSE_PLACEHOLDERS;
        List* owner_setrefs_carrier_vars = NIL;
        if (!per_row_collect_setrefs_carrier_vars(target_expression,
                                                  owner_support.helper_argument_vars,
                                                  joinrel->relids,
                                                  owner_setrefs_carrier_vars)) {
            return per_row_decline_transformed_path("setrefs_carrier_vars");
        }
        for (Node* expression : postgres_list<Node>(joinrel->reltarget->exprs)) {
            required_vars = list_concat(required_vars, pull_var_clause(expression, pull_flags));
        }
        for (RestrictInfo* residual : postgres_list<RestrictInfo>(extra->restrictlist)) {
            if (residual == nullptr || residual == restriction)
                continue;
            required_vars =
                list_concat(required_vars,
                            pull_var_clause(reinterpret_cast<Node*>(residual->clause), pull_flags));
        }
        required_vars = list_concat(required_vars, list_copy(owner_setrefs_carrier_vars));
        required_vars = list_concat(required_vars, pull_var_clause(query_expression, pull_flags));
        required_vars = list_concat(required_vars, list_copy(owner_support.passthrough_vars));
        for (Node* required : postgres_list<Node>(required_vars)) {
            Var* var = try_cast_pg_node<Var>(required);
            const bool row_identity = per_row_is_exact_row_identity_var(var);
            if (var == nullptr || !bioseq_is_concrete_rte_varno(var->varno) ||
                (var->varattno <= 0 && !row_identity) || var->varlevelsup != 0 ||
                !bms_is_empty(var->varnullingrels) ||
                !bms_is_member(static_cast<int>(var->varno), joinrel->relids)) {
                return per_row_decline_transformed_path("required_var");
            }
        }
        add_new_columns_to_pathtarget(required_target, required_vars);
        set_pathtarget_cost_width(root, required_target);
        PathTarget* owner_target = per_row_build_owner_target(root, joinrel->reltarget);
        if (owner_target == nullptr) {
            return per_row_decline_transformed_path("owner_target_authority");
        }
        if (!equal(owner_target->exprs, joinrel->reltarget->exprs) ||
            list_length(owner_target->exprs) != list_length(joinrel->reltarget->exprs)) {
            return per_row_decline_transformed_path("owner_target_widened");
        }
        set_pathtarget_cost_width(root, owner_target);

        PathTarget* recipe_target = copy_pathtarget(required_target);
        if (!path_target_contains_expression(recipe_target,
                                             reinterpret_cast<Expr*>(target_expression))) {
            add_column_to_pathtarget(
                recipe_target, reinterpret_cast<Expr*>(copyObject(target_expression)), 0);
        }
        if (!path_target_contains_expression(recipe_target,
                                             reinterpret_cast<Expr*>(query_expression))) {
            add_column_to_pathtarget(
                recipe_target, reinterpret_cast<Expr*>(copyObject(query_expression)), 0);
        }
        set_pathtarget_cost_width(root, recipe_target);
        slice_project_ir_builder builder{
            .oids = &oids,
            .command_type = CMD_UNKNOWN,
            .planner_root = root,
            .target_leaf_support_relid = target_source->varno,
            .target_leaf_support_vars = owner_support.helper_argument_vars,
        };
        if (!slice_project_discover_target(recipe_target, builder)) {
            return per_row_decline_transformed_path("recipe_discovery");
        }
        const std::uint32_t target_producer =
            slice_project_add_producer(builder, target_expression);
        const std::uint32_t query_producer = slice_project_add_producer(builder, query_expression);
        bioseq::simd::minimap2_parameters planned_params = build_params_from_gucs();
        bioseq::indexing::index_read_requirement planned_requirement =
            bioseq::indexing::index_read_requirement::allow;
        (void)apply_aligner_options_override(
            executable_mmsearch, planned_params, planned_requirement, oids);
        if (mmsearch_plans_admitted_packed_regions(planned_params)) {
            slice_project_add_boundary(builder,
                                       target_expression,
                                       target_producer,
                                       materialization_mechanism::packed_region,
                                       materialization_reason::mmsearch_target,
                                       true);
            slice_project_add_boundary(builder,
                                       query_expression,
                                       query_producer,
                                       materialization_mechanism::packed_region,
                                       materialization_reason::mmsearch_query,
                                       true);
        }

        postgres_list<Path> owners;
        per_row_reconstruction_rejections reconstruction_rejects{};
        List* incumbent_paths = list_copy(joinrel->pathlist);
        Cost incumbent_floor = std::numeric_limits<Cost>::max();
        for (Path* incumbent : postgres_list<Path>(incumbent_paths)) {
            if (incumbent != nullptr && std::isfinite(incumbent->total_cost) &&
                incumbent->total_cost >= 0) {
                incumbent_floor = std::min(incumbent_floor, incumbent->total_cost);
            }
        }
        for (Path* incumbent : postgres_list<Path>(incumbent_paths)) {
            Path* helper_ready_incumbent = incumbent;
            PathTarget* incumbent_required_target = copy_pathtarget(required_target);
            List* validated_inherited_helpers = NIL;
            bool helper_provenance_invalid = false;
            for (Node* identity : postgres_list<Node>(owner_support.helper_identities)) {
                FuncExpr* helper = nullptr;
                if (PlaceHolderVar* placeholder = try_cast_pg_node<PlaceHolderVar>(identity)) {
                    helper = batch_join_match_helper_phv_function(placeholder, oids);
                } else {
                    helper = batch_join_well_formed_match_helper(identity, oids);
                }
                Node* argument = batch_join_match_helper_argument(helper);
                if (helper == nullptr || argument == nullptr) {
                    helper_provenance_invalid = true;
                    break;
                }
                per_row_descendant_ownership ownership{};
                per_row_analyze_descendant_exact_owner(helper_ready_incumbent, argument, ownership);
                per_row_base_helper_ownership base_ownership{};
                per_row_analyze_descendant_base_helper_owner(
                    helper_ready_incumbent, helper, base_ownership);
                if (ownership.invalid_owner_abi || ownership.unsupported_wrapper ||
                    base_ownership.invalid_owner_abi || base_ownership.unsupported_wrapper ||
                    ownership.matches > 1u || base_ownership.matches > 1u ||
                    ownership.matches + base_ownership.matches > 1u) {
                    helper_provenance_invalid = true;
                    break;
                }
                /* root->processed_tlist is query-global.  No descendant match means
                 * either that this exact helper belongs to the owner being constructed,
                 * or that its owner will be constructed at a higher join frontier.  In
                 * both cases it is not inherited here.  A helper with one exact
                 * descendant remains inherited, while multiplicity still fails above. */
                if (ownership.matches + base_ownership.matches == 0u)
                    continue;
                CustomPath* inherited_owner =
                    ownership.matches == 1u ? ownership.owner : base_ownership.owner;
                bool applied = false;
                bool propagation_invalid = false;
                helper_ready_incumbent = per_row_clone_path_to_helper_owner(root,
                                                                            helper_ready_incumbent,
                                                                            inherited_owner,
                                                                            identity,
                                                                            applied,
                                                                            propagation_invalid);
                if (!applied || propagation_invalid) {
                    helper_provenance_invalid = true;
                    break;
                }
                if (!path_target_contains_expression(incumbent_required_target,
                                                     reinterpret_cast<Expr*>(identity))) {
                    add_column_to_pathtarget(incumbent_required_target,
                                             reinterpret_cast<Expr*>(copyObject(identity)),
                                             0);
                }
                per_row_append_unique_expression(validated_inherited_helpers, identity);
            }
            if (helper_provenance_invalid) {
                ++reconstruction_rejects.projection_or_leak;
                continue;
            }
            set_pathtarget_cost_width(root, incumbent_required_target);
            Path* reconstructed = per_row_reconstruct_inner_join_path(root,
                                                                      joinrel,
                                                                      helper_ready_incumbent,
                                                                      restriction,
                                                                      *extra,
                                                                      incumbent_required_target,
                                                                      required_vars,
                                                                      target_source->varno,
                                                                      reconstruction_rejects);
            if (reconstructed == nullptr)
                continue;
            /* Re-run the exact pointer audit on the finished reconstructed path,
             * before copyObject can turn a leaked RestrictInfo into a merely-equal
             * private copy and before any Plan node exists. */
            if (per_row_descendant_owns_restriction(reconstructed, restriction)) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqPerRowMmsearch reconstructed path retained its owned "
                                "RestrictInfo")));
            }
            for (Node* identity : postgres_list<Node>(validated_inherited_helpers)) {
                FuncExpr* helper = try_cast_pg_node<PlaceHolderVar>(identity) != nullptr
                                       ? batch_join_match_helper_phv_function(
                                             try_cast_pg_node<PlaceHolderVar>(identity), oids)
                                       : batch_join_well_formed_match_helper(identity, oids);
                per_row_descendant_ownership ownership{};
                per_row_analyze_descendant_exact_owner(
                    reconstructed, batch_join_match_helper_argument(helper), ownership);
                per_row_base_helper_ownership base_ownership{};
                per_row_analyze_descendant_base_helper_owner(reconstructed, helper, base_ownership);
                const std::size_t owner_matches = ownership.matches + base_ownership.matches;
                CustomPath* inherited_owner =
                    ownership.matches == 1u ? ownership.owner : base_ownership.owner;
                if (ownership.invalid_owner_abi || ownership.unsupported_wrapper ||
                    base_ownership.invalid_owner_abi || base_ownership.unsupported_wrapper ||
                    owner_matches != 1u || inherited_owner == nullptr ||
                    inherited_owner->path.pathtarget == nullptr ||
                    !path_target_contains_expression(inherited_owner->path.pathtarget,
                                                     reinterpret_cast<Expr*>(identity))) {
                    helper_provenance_invalid = true;
                    break;
                }
            }
            if (helper_provenance_invalid) {
                ++reconstruction_rejects.projection_or_leak;
                continue;
            }
            Path* private_join = slice_project_clone_mutable_path_spine(reconstructed);
            ++reconstruction_rejects.cloned;
            if (!per_row_normalize_private_path_spine(root,
                                                      private_join,
                                                      owner_support.helper_identities,
                                                      validated_inherited_helpers,
                                                      required_vars)) {
                ++reconstruction_rejects.projection_or_leak;
                continue;
            }
            private_join->pathtarget = copy_pathtarget(incumbent_required_target);
            bool tagged_source = false;
            private_join = slice_project_tag_owned_path_tree(private_join, builder, tagged_source);
            if (!tagged_source) {
                ++reconstruction_rejects.tag_missing;
                continue;
            }
            Path* source_path = reinterpret_cast<Path*>(
                create_projection_path(root, joinrel, private_join, incumbent_required_target));
            if (source_path == nullptr) {
                ++reconstruction_rejects.source_projection_missing;
                continue;
            }

            PathTarget* incumbent_recipe_target = copy_pathtarget(recipe_target);
            for (Node* identity : postgres_list<Node>(validated_inherited_helpers)) {
                if (!path_target_contains_expression(incumbent_recipe_target,
                                                     reinterpret_cast<Expr*>(identity))) {
                    add_column_to_pathtarget(
                        incumbent_recipe_target, reinterpret_cast<Expr*>(copyObject(identity)), 0);
                }
            }
            set_pathtarget_cost_width(root, incumbent_recipe_target);
            auto* recipe_path = makeNode(CustomPath);
            recipe_path->path.pathtype = T_CustomScan;
            recipe_path->path.parent = joinrel;
            recipe_path->path.pathtarget = incumbent_recipe_target;
            recipe_path->path.param_info = source_path->param_info;
            recipe_path->path.parallel_aware = false;
            recipe_path->path.parallel_safe = false;
            recipe_path->path.parallel_workers = 0;
            recipe_path->path.rows = source_path->rows;
            QualCost recipe_cost{};
            cost_qual_eval(&recipe_cost, incumbent_recipe_target->exprs, root);
            recipe_path->path.startup_cost = source_path->startup_cost + recipe_cost.startup;
            recipe_path->path.total_cost = source_path->total_cost + recipe_cost.startup +
                                           std::max(0.0, source_path->rows) * recipe_cost.per_tuple;
            recipe_path->path.pathkeys = source_path->pathkeys;
            recipe_path->flags = CUSTOMPATH_SUPPORT_PROJECTION;
            recipe_path->custom_paths = list_make1(source_path);
            recipe_path->custom_private = list_make5(slice_project_header(builder),
                                                     copyObject(builder.expressions),
                                                     copyObject(builder.nodes),
                                                     copyObject(builder.records),
                                                     copyObject(builder.output_bindings));
            recipe_path->custom_restrictinfo = NIL;
            recipe_path->methods = &bioseq_slice_project_path_methods();

            QualCost operand_cost{};
            cost_qual_eval(
                &operand_cost, mmsearch_operand_cost_exprs(executable_mmsearch, oids), root);
            const std::uint64_t target_symbols =
                estimate_mmsearch_operand_symbols(target_expression, oids);
            const std::uint64_t query_symbols =
                estimate_mmsearch_operand_symbols(query_expression, oids);
            const owned_mmsearch_cost alignment_cost = estimate_owned_mmsearch_cost(
                target_symbols,
                query_symbols,
                recipe_path->path.rows,
                /*index_read_requirement=*/false,
                static_cast<std::uint32_t>(k_bioseq_runtime_buffer_reserve),
                bioseq::common::saturating_add(target_symbols, query_symbols));
            const Cost startup = std::max<Cost>(
                0, recipe_path->path.startup_cost + operand_cost.startup + alignment_cost.startup);
            const Cost total = std::max(
                startup,
                recipe_path->path.total_cost + operand_cost.startup + alignment_cost.startup +
                    std::max(0.0, recipe_path->path.rows) *
                        (operand_cost.per_tuple + alignment_cost.per_input_tuple + cpu_tuple_cost));
            if (!std::isfinite(startup) || !std::isfinite(total)) {
                ++reconstruction_rejects.nonfinite_cost;
                continue;
            }

            auto* owner = makeNode(CustomPath);
            owner->path.pathtype = T_CustomScan;
            owner->path.parent = joinrel;
            owner->path.pathtarget = copy_pathtarget(owner_target);
            owner->path.param_info = recipe_path->path.param_info;
            owner->path.parallel_aware = false;
            owner->path.parallel_safe = false;
            owner->path.parallel_workers = 0;
            owner->path.rows = recipe_path->path.rows;
            owner->path.startup_cost = startup;
            owner->path.total_cost = total;
            owner->path.pathkeys = recipe_path->path.pathkeys;
            /* Keep core from folding a later SELECT/RETURNING projection into this
             * CustomScan after PlanCustomPath has registered descriptor identities.
             * A normal Result above the owner can bind whole helpers through the
             * owner-support target without ever executing their SQL stubs. */
            owner->flags = 0;
            owner->custom_paths = list_make1(recipe_path);
            owner->custom_restrictinfo = list_make1(restriction);
            const auto typmod =
                compact_sequence_stdc_type_params::decode(target_typmod_validation_result->typmod);
            owner->custom_private =
                list_make5(makeInteger(k_bioseq_per_row_join_plan_version),
                           makeInteger(static_cast<int>(target_rte->relid)),
                           makeInteger(static_cast<int>(target_source->varno)),
                           makeInteger(static_cast<int>(target_source->varattno)),
                           copyObject(target_expression));
            owner->custom_private = lappend(owner->custom_private, copyObject(query_expression));
            owner->custom_private = lappend(owner->custom_private, copyObject(executable_mmsearch));
            owner->custom_private =
                lappend(owner->custom_private, makeInteger(typmod.alphabet_code));
            owner->custom_private =
                lappend(owner->custom_private, makeInteger(typmod.storage_unit_byte_width));
            owner->custom_private =
                lappend(owner->custom_private,
                        makeInteger(static_cast<int>(
                            bioseq::indexing::index_read_requirement::require_absent)));
            owner->custom_private =
                lappend(owner->custom_private, make_batch_join_cost_const(startup));
            owner->custom_private =
                lappend(owner->custom_private, make_batch_join_cost_const(total));
            owner->custom_private =
                lappend(owner->custom_private,
                        make_batch_join_cost_const(std::isfinite(incumbent_floor) ? incumbent_floor
                                                                                  : total));
            Assert(list_length(owner->custom_private) == k_bioseq_per_row_join_path_private_fields);
            owner->methods = bioseq_per_row_join_path_methods();
            owners.append(reinterpret_cast<Path*>(owner));
            ++reconstruction_rejects.owners_created;
        }
        if (owners.is_empty()) {
            return false;
        }
        postgres_list<Path> retained;
        for (Path* incumbent : postgres_list<Path>(joinrel->pathlist)) {
            if (incumbent != nullptr && IsA(incumbent, CustomPath) &&
                list_member_ptr(reinterpret_cast<CustomPath*>(incumbent)->custom_restrictinfo,
                                restriction))
                retained.append(incumbent);
        }
        joinrel->pathlist = retained.release();
        joinrel->partial_pathlist = NIL;
        for (Path* owner : owners)
            add_path(joinrel, owner);
        return true;
    }

} // export custom-scan partition declarations
