/**
 * @file
 * @brief Installs planner hooks and constructs raw two-child BatchJoin paths.
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

export module bioseq.postgres.table:custom_scan_batch_join_paths;

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
import bioseq.instrumentation.summary_writer;
import bioseq.common.seqan_adapter;
import bioseq.postgres.table.batch_sort_scan;

import :custom_scan_support;
import :custom_scan_runtime_support;
import :custom_scan_slice_path_codec_contract;
import :custom_scan_slice_rewrite_path_tree;

import :custom_scan_join_ownership;
import :custom_scan_per_row_join_paths;

using namespace bioseq::postgres::support;

export {

    /**
     * @brief Extends join path generation for one eligible mmsearch inner join.
     *
     * Invokes the chained hook first. For one securely promotable mmsearch restriction
     * on supported inputs, retains existing custom-owner paths, admits transformed
     * per-row paths, or appends a raw two-child BatchJoin CustomPath to @p joinrel.
     */
    void bioseq_batch_join_set_join_pathlist_hook(PlannerInfo* root,
                                                  RelOptInfo* joinrel,
                                                  RelOptInfo* outerrel,
                                                  RelOptInfo* innerrel,
                                                  JoinType jointype,
                                                  JoinPathExtraData* extra) {
        if (previous_set_join_pathlist_hook != nullptr) {
            previous_set_join_pathlist_hook(root, joinrel, outerrel, innerrel, jointype, extra);
        }
        if (root == nullptr || joinrel == nullptr || outerrel == nullptr || innerrel == nullptr ||
            extra == nullptr || jointype != JOIN_INNER) {
            return;
        }
        // shared_preload_libraries installs this hook cluster-wide. Databases that
        // have not installed Bioseq must retain ordinary PostgreSQL join planning.
        if (!OidIsValid(get_extension_oid("bioseq", /*missing_ok=*/true))) {
            return;
        }

        const bioseq_stub_oids* stub_oids = try_get_cached_stub_oids();
        if (stub_oids == nullptr) {
            return;
        }
        const bioseq_stub_oids& oids = *stub_oids;
        std::size_t semantic_calls = 0;
        RestrictInfo* restriction = nullptr;
        for (RestrictInfo* candidate : postgres_list<RestrictInfo>(extra->restrictlist)) {
            const std::size_t calls =
                count_mmsearch_occurrences(reinterpret_cast<Node*>(candidate->clause), oids);
            semantic_calls += calls;
            if (calls != 0)
                restriction = candidate;
        }
        if (semantic_calls > 1) {
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("stacked mmsearch join qualifiers are unsupported"),
                     errhint("Split the alignment predicates into separate query levels.")));
        }
        if (semantic_calls != 1 || restriction == nullptr) {
            return;
        }
        FuncExpr* mmsearch =
            restriction == nullptr
                ? nullptr
                : try_cast_pg_node<FuncExpr>(reinterpret_cast<Node*>(restriction->clause));
        if (mmsearch == nullptr || !is_mmsearch_funcid(mmsearch->funcid, oids) ||
            (mmsearch->funcid == oids.mmsearch_oid && list_length(mmsearch->args) != 2) ||
            (mmsearch->funcid == oids.mmsearch_with_opts_oid && list_length(mmsearch->args) != 3)) {
            return;
        }
        if (root->parse == nullptr || root->parse->hasRowSecurity || restriction->pseudoconstant ||
            restriction->security_level != 0 ||
            !restriction_is_securely_promotable(restriction, joinrel)) {
            return;
        }
        for (RestrictInfo* sibling : postgres_list<RestrictInfo>(extra->restrictlist)) {
            if (sibling != restriction &&
                contain_volatile_functions(reinterpret_cast<Node*>(sibling->clause))) {
                /* The first release does not claim an evaluation-order-preserving
                 * split around an effectful sibling. */
                return;
            }
        }
        List* residual_join_restrictions = NIL;
        for (RestrictInfo* candidate : postgres_list<RestrictInfo>(extra->restrictlist)) {
            if (candidate != restriction) {
                residual_join_restrictions = lappend(residual_join_restrictions, candidate);
            }
        }
        postgres_list<Node> arguments(mmsearch->args);
        Var* target_var = try_cast_pg_node<Var>(arguments.get_first());
        Var* query_var = try_cast_pg_node<Var>(arguments.get_second());
        /* Eligibility is non-throwing: fold only the option expression on a
         * private copy. A typed NULL Const means default options. */
        auto* batch_mmsearch = static_cast<FuncExpr*>(copyObject(mmsearch));
        if (list_length(mmsearch->args) == 3) {
            Node* folded_options = eval_const_expressions(
                root, reinterpret_cast<Node*>(copyObject(arguments.get_third())));
            Const* options = try_cast_pg_node<Const>(folded_options);
            if (options == nullptr || options->consttype != oids.bioseq_aligner_options_type_oid) {
                return;
            }
            lfirst(list_nth_cell(batch_mmsearch->args, 2)) = folded_options;
        }
        /* Decode options before choosing the raw two-child or transformed
         * one-child ownership shape. */
        bioseq::simd::minimap2_parameters planned_params = build_params_from_gucs();
        bioseq::indexing::index_read_requirement planned_requirement =
            bioseq::indexing::index_read_requirement::allow;
        (void)apply_aligner_options_override(
            batch_mmsearch, planned_params, planned_requirement, oids);
        finalize_simd_chaining_policy(planned_params);
        const bool transformed_operands =
            bioseq_expr_uses_lazy_sequence_projection(arguments.get_first()) ||
            bioseq_expr_uses_lazy_sequence_projection(arguments.get_second());
        /** @brief Reports whether @p input is a base or other-member planner relation. */
        auto is_base_input = [](RelOptInfo const* input) noexcept {
            return input != nullptr && (input->reloptkind == RELOPT_BASEREL ||
                                        input->reloptkind == RELOPT_OTHER_MEMBER_REL);
        };
        /** @brief Reports whether @p input is a join planner relation. */
        auto is_join_input = [](RelOptInfo const* input) noexcept {
            return input != nullptr && input->reloptkind == RELOPT_JOINREL;
        };
        const bool two_base_inputs = is_base_input(outerrel) && is_base_input(innerrel);
        const bool one_join_one_base = (is_join_input(outerrel) && is_base_input(innerrel)) ||
                                       (is_base_input(outerrel) && is_join_input(innerrel));
        /* Raw nested joins retain the established parameterized BioseqScan owner.
         * Core also offers join paths that leave the same rendezvous RestrictInfo as
         * an executable Join Filter; costs cannot choose between semantic ownership
         * and the forbidden SQL stub, so discard those paths structurally. */
        if (one_join_one_base && !transformed_operands) {
            exact_parameterized_bmo_retainer_audit owner_audit{};
            if (retain_join_paths_with_custom_restriction_owner(
                    root, joinrel, outerrel, innerrel, extra, restriction, mmsearch, &owner_audit))
                return;
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("mmsearch join qualifier is not owned by a parameterized BioseqScan"),
                     errdetail(
                         "The raw nested join cannot retain exactly one inner BMO owner: "
                         "candidates=%zu nest=%zu inner_nest=%zu root_owned_clause=%zu "
                         "root_foreign_mmsearch=%zu outer_matches=%zu inner_matches=%zu "
                         "available_bmo=%zu compatible_bmo=%zu available_outer=%zu constructed=%zu "
                         "reconstruct_attempts=%zu reconstructed_bmo=%zu "
                         "reconstruct_reject_mask=0x%x "
                         "reconstruct_abi_mask=0x%x reconstruct_expected=%zu reconstruct_tags=%zu "
                         "reconstruct_exprs=%zu outer_reject_mask=0x%x inner_reject_mask=0x%x "
                         "target_var=%zu query_var=%zu target_relid_sides=0x%x "
                         "target_base_side=0x%x "
                         "query_relid_sides=0x%x provider_has_query=%zu target_paths=%zu "
                         "target_custom=%zu target_bioseq=%zu target_bmo_tags=%zu "
                         "custom_restrictions=%zu custom_pointer=%zu custom_serial=%zu "
                         "custom_clause=%zu custom_exact=%zu ppi_paths=%zu ppi_clauses=%zu "
                         "ppi_serial=%zu ppi_clause=%zu ppi_exact=%zu "
                         "target_owner_reject_mask=0x%x "
                         "target_abi_mismatch_mask=0x%x target_ppi_mismatch_mask=0x%x "
                         "target_compat_mismatch_mask=0x%x join_relids=0x%llx "
                         "left_relids=0x%llx/%zu "
                         "right_relids=0x%llx/%zu wrapper_side=0x%x wrapper_method=0x%x "
                         "wrapper_version=%zu wrapper_owns=%zu wrapper_restrictions=%zu "
                         "wrapper_children=%zu wrapper_pointer=%zu wrapper_serial=%zu "
                         "wrapper_clause=%zu current_serial=%zu owned_serial=%zu "
                         "current_vars=%zu/%zu "
                         "private_vars=%zu/%zu private_equal=%zu/%zu owner0_vars=%zu/%zu "
                         "owner1_vars=%zu/%zu wrapper_parent=0x%llx wrapper_child0=0x%llx "
                         "wrapper_child1=0x%llx current_pair=%zu foreign=%zu safe_foreign=%zu "
                         "foreign_matches=%zu foreign_reject=0x%x foreign_owner_reject=0x%x.",
                         owner_audit.candidates,
                         owner_audit.nest_paths,
                         owner_audit.inner_nest_paths,
                         owner_audit.root_owned_clause_paths,
                         owner_audit.root_foreign_mmsearch_paths,
                         owner_audit.outer_matches,
                         owner_audit.inner_matches,
                         owner_audit.available_bmo_paths,
                         owner_audit.compatible_bmo_paths,
                         owner_audit.available_outer_paths,
                         owner_audit.constructed_paths,
                         owner_audit.reconstructed_bmo_attempts,
                         owner_audit.reconstructed_bmo_paths,
                         owner_audit.reconstructed_bmo_rejection_mask,
                         owner_audit.reconstructed_owner_abi_mismatch_mask,
                         owner_audit.reconstructed_expected_owner_count,
                         owner_audit.reconstructed_owner_tag_count,
                         owner_audit.reconstructed_owner_expression_count,
                         owner_audit.outer_rejection_mask,
                         owner_audit.inner_rejection_mask,
                         owner_audit.target_var_resolved,
                         owner_audit.query_var_resolved,
                         owner_audit.target_relid_side_mask,
                         owner_audit.target_base_side_mask,
                         owner_audit.query_relid_side_mask,
                         owner_audit.provider_has_query,
                         owner_audit.target_paths,
                         owner_audit.target_custom_paths,
                         owner_audit.target_bioseq_paths,
                         owner_audit.target_bmo_tags,
                         owner_audit.target_custom_restrictions,
                         owner_audit.target_custom_pointer_matches,
                         owner_audit.target_custom_serial_matches,
                         owner_audit.target_custom_clause_matches,
                         owner_audit.target_custom_exact_matches,
                         owner_audit.target_ppi_paths,
                         owner_audit.target_ppi_clauses,
                         owner_audit.target_ppi_serial_matches,
                         owner_audit.target_ppi_clause_matches,
                         owner_audit.target_ppi_exact_matches,
                         owner_audit.target_owner_rejection_mask,
                         owner_audit.target_abi_mismatch_mask,
                         owner_audit.target_ppi_mismatch_mask,
                         owner_audit.target_validation_mismatch_mask,
                         static_cast<unsigned long long>(owner_audit.joinrel_relids_low),
                         static_cast<unsigned long long>(owner_audit.left_relids_low),
                         owner_audit.left_relid_count,
                         static_cast<unsigned long long>(owner_audit.right_relids_low),
                         owner_audit.right_relid_count,
                         owner_audit.wrapper_side_mask,
                         owner_audit.wrapper_method_mask,
                         owner_audit.wrapper_version,
                         owner_audit.wrapper_owns_expected,
                         owner_audit.wrapper_restriction_count,
                         owner_audit.wrapper_child_count,
                         owner_audit.wrapper_pointer_matches,
                         owner_audit.wrapper_serial_matches,
                         owner_audit.wrapper_clause_matches,
                         owner_audit.wrapper_expected_serial,
                         owner_audit.wrapper_owned_serial,
                         owner_audit.wrapper_expected_target_varno,
                         owner_audit.wrapper_expected_query_varno,
                         owner_audit.wrapper_private_target_varno,
                         owner_audit.wrapper_private_query_varno,
                         owner_audit.wrapper_private_target_equal,
                         owner_audit.wrapper_private_query_equal,
                         owner_audit.wrapper_owner0_target_varno,
                         owner_audit.wrapper_owner0_query_varno,
                         owner_audit.wrapper_owner1_target_varno,
                         owner_audit.wrapper_owner1_query_varno,
                         static_cast<unsigned long long>(owner_audit.wrapper_parent_relids_low),
                         static_cast<unsigned long long>(owner_audit.wrapper_child0_relids_low),
                         static_cast<unsigned long long>(owner_audit.wrapper_child1_relids_low),
                         owner_audit.current_pair_paths,
                         owner_audit.foreign_decomposition_paths,
                         owner_audit.safe_foreign_paths,
                         owner_audit.foreign_owner_matches,
                         owner_audit.foreign_rejection_mask,
                         owner_audit.foreign_owner_rejection_mask)));
        }
        if (transformed_operands) {
            /* A transformed owner may sit above one already-owned join frontier.
             * This is the bounded same-PlannerInfo form used when two matches share
             * one SQL-visible base.  Raw BatchJoin remains strictly two-baserel
             * below.  Reject wider/partition-derived join kinds and validate that this
             * hook invocation describes exactly the output relids before reconstructing
             * an incumbent path. */
            if (!two_base_inputs && !one_join_one_base)
                return;
            Relids input_relids = bms_union(outerrel->relids, innerrel->relids);
            const bool exact_frontier = bms_equal(input_relids, joinrel->relids);
            bms_free(input_relids);
            if (!exact_frontier)
                return;
            (void)per_row_add_transformed_mmsearch_paths(root,
                                                         joinrel,
                                                         extra,
                                                         restriction,
                                                         mmsearch,
                                                         batch_mmsearch,
                                                         planned_requirement,
                                                         oids);
            return;
        }
        if (!two_base_inputs)
            return;
        if (target_var == nullptr || query_var == nullptr ||
            !bioseq_is_concrete_rte_varno(target_var->varno) ||
            !bioseq_is_concrete_rte_varno(query_var->varno) || target_var->varattno <= 0 ||
            query_var->varattno <= 0 || target_var->varlevelsup != 0 ||
            query_var->varlevelsup != 0 || !bms_is_empty(target_var->varnullingrels) ||
            !bms_is_empty(query_var->varnullingrels) ||
            target_var->vartype != oids.bioseq_seq_type_oid ||
            query_var->vartype != oids.bioseq_seq_type_oid ||
            !batch_join_typmod_is_supported(target_var->vartypmod) ||
            !batch_join_typmod_is_supported(query_var->vartypmod) ||
            target_var->varcollid != query_var->varcollid) {
            return;
        }
        const auto target_type_params =
            compact_sequence_stdc_type_params::decode(target_var->vartypmod);
        const auto query_type_params =
            compact_sequence_stdc_type_params::decode(query_var->vartypmod);
        if (target_type_params.alphabet_code != query_type_params.alphabet_code) {
            raise_mmsearch_alphabet_mismatch(
                target_type_params, query_type_params, "join planning");
        }
        RelOptInfo* target_rel = nullptr;
        RelOptInfo* query_rel = nullptr;
        if (target_var->varno == outerrel->relid && query_var->varno == innerrel->relid) {
            target_rel = outerrel;
            query_rel = innerrel;
        } else if (target_var->varno == innerrel->relid && query_var->varno == outerrel->relid) {
            target_rel = innerrel;
            query_rel = outerrel;
        } else {
            return;
        }
        List* target_residual_vars = NIL;
        List* query_residual_vars = NIL;
        bool unsupported_residual_var = false;
        constexpr int residual_pull_flags =
            PVC_RECURSE_AGGREGATES | PVC_RECURSE_WINDOWFUNCS | PVC_RECURSE_PLACEHOLDERS;
        for (RestrictInfo* residual : postgres_list<RestrictInfo>(residual_join_restrictions)) {
            List* variables =
                pull_var_clause(reinterpret_cast<Node*>(residual->clause), residual_pull_flags);
            for (Node* variable : postgres_list<Node>(variables)) {
                Var* var = try_cast_pg_node<Var>(variable);
                if (var == nullptr || !bioseq_is_concrete_rte_varno(var->varno) ||
                    var->varlevelsup != 0 || var->varattno <= 0 ||
                    !bms_is_empty(var->varnullingrels)) {
                    unsupported_residual_var = true;
                    break;
                }
                List** destination =
                    var->varno == target_rel->relid
                        ? &target_residual_vars
                        : (var->varno == query_rel->relid ? &query_residual_vars : nullptr);
                if (destination == nullptr) {
                    unsupported_residual_var = true;
                    break;
                }
                bool seen = false;
                for (Node* existing : postgres_list<Node>(*destination)) {
                    if (equal(existing, variable)) {
                        seen = true;
                        break;
                    }
                }
                if (!seen)
                    *destination = lappend(*destination, copyObject(variable));
            }
            if (unsupported_residual_var)
                break;
        }
        if (unsupported_residual_var)
            return;
        RangeTblEntry* target_rte = rt_fetch(target_var->varno, root->parse->rtable);
        RangeTblEntry* query_rte = rt_fetch(query_var->varno, root->parse->rtable);
        const bool plain_query_relation =
            query_rte != nullptr && query_rte->rtekind == RTE_RELATION;
        const bool plain_query_subquery =
            query_rte != nullptr && query_rte->rtekind == RTE_SUBQUERY &&
            query_rte->subquery != nullptr && !query_rte->security_barrier &&
            !query_rte->subquery->hasRowSecurity;
        if (target_rte == nullptr || target_rte->rtekind != RTE_RELATION || query_rte == nullptr ||
            (!plain_query_relation && !plain_query_subquery) || target_rte->securityQuals != NIL ||
            query_rte->securityQuals != NIL || !OidIsValid(target_rte->relid) ||
            !relation_oid_uses_column_table_access_method(target_rte->relid) ||
            batch_join_target_expression_is_forbidden(
                reinterpret_cast<Node*>(root->processed_tlist),
                target_var,
                /*allow_mmsearch_qualifier=*/false) ||
            batch_join_target_restrictions_are_forbidden(target_rel->baserestrictinfo,
                                                         target_var,
                                                         /*allow_mmsearch_qualifier=*/true)) {
            return;
        }
        const auto target_qual_features =
            detect_restrictinfo_feature_flags(target_rel->baserestrictinfo, oids);
        const auto query_qual_features =
            detect_restrictinfo_feature_flags(query_rel->baserestrictinfo, oids);
        /* The BatchJoin locator child deliberately does not execute its target
         * BioseqScan's alignment runtime.  A target-side base mmsearch therefore
         * has no safe owner after locator conversion; leave this shape to the
         * ordinary parameterized scan, which can consume both the join Param and
         * its base qual.  Query-side base mmsearch remains child-owned. */
        if (!target_qual_features.has_value() || !query_qual_features.has_value() ||
            target_qual_features->any_match_projection ||
            query_qual_features->any_match_projection) {
            return;
        }
        if (target_qual_features->has_mmsearch) {
            /* The locator path is ineligible, but a narrowly admitted ordinary
             * relation/require-absent fallback has a parameterized BioseqScan that
             * owns both calls.  Retain only join paths reaching that real owner;
             * leaving an ordinary Join Filter would execute the SQL rendezvous stub. */
            exact_parameterized_bmo_retainer_audit owner_audit{};
            if (retain_join_paths_with_custom_restriction_owner(
                    root, joinrel, outerrel, innerrel, extra, restriction, mmsearch, &owner_audit))
                return;
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("stacked mmsearch qualifiers are unsupported"),
                     errdetail(
                         "No parameterized BioseqScan owns the bounded BatchJoin fallback: "
                         "candidates=%zu nest=%zu inner_nest=%zu root_owned_clause=%zu "
                         "root_foreign_mmsearch=%zu outer_matches=%zu inner_matches=%zu "
                         "available_bmo=%zu compatible_bmo=%zu available_outer=%zu constructed=%zu "
                         "reconstruct_attempts=%zu reconstructed_bmo=%zu "
                         "reconstruct_reject_mask=0x%x "
                         "reconstruct_abi_mask=0x%x reconstruct_expected=%zu reconstruct_tags=%zu "
                         "reconstruct_exprs=%zu outer_reject_mask=0x%x inner_reject_mask=0x%x "
                         "target_var=%zu query_var=%zu target_relid_sides=0x%x "
                         "target_base_side=0x%x "
                         "query_relid_sides=0x%x provider_has_query=%zu target_paths=%zu "
                         "target_custom=%zu target_bioseq=%zu target_bmo_tags=%zu "
                         "custom_restrictions=%zu custom_pointer=%zu custom_serial=%zu "
                         "custom_clause=%zu custom_exact=%zu ppi_paths=%zu ppi_clauses=%zu "
                         "ppi_serial=%zu ppi_clause=%zu ppi_exact=%zu "
                         "target_owner_reject_mask=0x%x "
                         "target_abi_mismatch_mask=0x%x target_ppi_mismatch_mask=0x%x "
                         "target_compat_mismatch_mask=0x%x join_relids=0x%llx "
                         "left_relids=0x%llx/%zu "
                         "right_relids=0x%llx/%zu wrapper_side=0x%x wrapper_method=0x%x "
                         "wrapper_version=%zu wrapper_owns=%zu wrapper_restrictions=%zu "
                         "wrapper_children=%zu wrapper_pointer=%zu wrapper_serial=%zu "
                         "wrapper_clause=%zu current_serial=%zu owned_serial=%zu "
                         "current_vars=%zu/%zu "
                         "private_vars=%zu/%zu private_equal=%zu/%zu owner0_vars=%zu/%zu "
                         "owner1_vars=%zu/%zu wrapper_parent=0x%llx wrapper_child0=0x%llx "
                         "wrapper_child1=0x%llx current_pair=%zu foreign=%zu safe_foreign=%zu "
                         "foreign_matches=%zu foreign_reject=0x%x foreign_owner_reject=0x%x.",
                         owner_audit.candidates,
                         owner_audit.nest_paths,
                         owner_audit.inner_nest_paths,
                         owner_audit.root_owned_clause_paths,
                         owner_audit.root_foreign_mmsearch_paths,
                         owner_audit.outer_matches,
                         owner_audit.inner_matches,
                         owner_audit.available_bmo_paths,
                         owner_audit.compatible_bmo_paths,
                         owner_audit.available_outer_paths,
                         owner_audit.constructed_paths,
                         owner_audit.reconstructed_bmo_attempts,
                         owner_audit.reconstructed_bmo_paths,
                         owner_audit.reconstructed_bmo_rejection_mask,
                         owner_audit.reconstructed_owner_abi_mismatch_mask,
                         owner_audit.reconstructed_expected_owner_count,
                         owner_audit.reconstructed_owner_tag_count,
                         owner_audit.reconstructed_owner_expression_count,
                         owner_audit.outer_rejection_mask,
                         owner_audit.inner_rejection_mask,
                         owner_audit.target_var_resolved,
                         owner_audit.query_var_resolved,
                         owner_audit.target_relid_side_mask,
                         owner_audit.target_base_side_mask,
                         owner_audit.query_relid_side_mask,
                         owner_audit.provider_has_query,
                         owner_audit.target_paths,
                         owner_audit.target_custom_paths,
                         owner_audit.target_bioseq_paths,
                         owner_audit.target_bmo_tags,
                         owner_audit.target_custom_restrictions,
                         owner_audit.target_custom_pointer_matches,
                         owner_audit.target_custom_serial_matches,
                         owner_audit.target_custom_clause_matches,
                         owner_audit.target_custom_exact_matches,
                         owner_audit.target_ppi_paths,
                         owner_audit.target_ppi_clauses,
                         owner_audit.target_ppi_serial_matches,
                         owner_audit.target_ppi_clause_matches,
                         owner_audit.target_ppi_exact_matches,
                         owner_audit.target_owner_rejection_mask,
                         owner_audit.target_abi_mismatch_mask,
                         owner_audit.target_ppi_mismatch_mask,
                         owner_audit.target_validation_mismatch_mask,
                         static_cast<unsigned long long>(owner_audit.joinrel_relids_low),
                         static_cast<unsigned long long>(owner_audit.left_relids_low),
                         owner_audit.left_relid_count,
                         static_cast<unsigned long long>(owner_audit.right_relids_low),
                         owner_audit.right_relid_count,
                         owner_audit.wrapper_side_mask,
                         owner_audit.wrapper_method_mask,
                         owner_audit.wrapper_version,
                         owner_audit.wrapper_owns_expected,
                         owner_audit.wrapper_restriction_count,
                         owner_audit.wrapper_child_count,
                         owner_audit.wrapper_pointer_matches,
                         owner_audit.wrapper_serial_matches,
                         owner_audit.wrapper_clause_matches,
                         owner_audit.wrapper_expected_serial,
                         owner_audit.wrapper_owned_serial,
                         owner_audit.wrapper_expected_target_varno,
                         owner_audit.wrapper_expected_query_varno,
                         owner_audit.wrapper_private_target_varno,
                         owner_audit.wrapper_private_query_varno,
                         owner_audit.wrapper_private_target_equal,
                         owner_audit.wrapper_private_query_equal,
                         owner_audit.wrapper_owner0_target_varno,
                         owner_audit.wrapper_owner0_query_varno,
                         owner_audit.wrapper_owner1_target_varno,
                         owner_audit.wrapper_owner1_query_varno,
                         static_cast<unsigned long long>(owner_audit.wrapper_parent_relids_low),
                         static_cast<unsigned long long>(owner_audit.wrapper_child0_relids_low),
                         static_cast<unsigned long long>(owner_audit.wrapper_child1_relids_low),
                         owner_audit.current_pair_paths,
                         owner_audit.foreign_decomposition_paths,
                         owner_audit.safe_foreign_paths,
                         owner_audit.foreign_owner_matches,
                         owner_audit.foreign_rejection_mask,
                         owner_audit.foreign_owner_rejection_mask)));
        }
        const batch_join_bioseq_path_candidate target_candidate =
            find_unparameterized_bioseq_custom_path(target_rel, target_var->varattno);
        CustomPath* target_path = target_candidate.scan;
        Path* query_path = query_rel->cheapest_total_path;
        if (target_path == nullptr || target_candidate.locator_target == nullptr ||
            query_path == nullptr || query_path->param_info != nullptr ||
            batch_join_path_contains_parallel_execution(query_path)) {
            return;
        }
        if (query_residual_vars != NIL) {
            PathTarget* query_target = copy_pathtarget(query_path->pathtarget);
            add_new_columns_to_pathtarget(query_target, query_residual_vars);
            set_pathtarget_cost_width(root, query_target);
            query_path = reinterpret_cast<Path*>(
                create_projection_path(root, query_path->parent, query_path, query_target));
        }

        auto* locator_path = makeNode(CustomPath);
        *locator_path = *target_path;
        /* Preserve the wrapper's conservative projection cost when a helper-only
         * ProjectionPath was unwrapped. The locator child does not execute those
         * helpers, but underpricing the custom join is worse than retaining their
         * planner estimate. */
        locator_path->path.startup_cost = target_candidate.cost_owner->startup_cost;
        locator_path->path.total_cost = target_candidate.cost_owner->total_cost;
        locator_path->path.rows = target_candidate.cost_owner->rows;
        locator_path->path.pathtarget = create_empty_pathtarget();
        /** @brief Copies eligible expressions from @p source into the locator pathtarget. */
        auto append_locator_target = [locator_path](const PathTarget* source) {
            if (source == nullptr)
                return;
            for (Node* expression : postgres_list<Node>(source->exprs)) {
                if (bioseq_expr_uses_bioseq_projection(expression) ||
                    path_target_contains_expression(locator_path->path.pathtarget,
                                                    reinterpret_cast<Expr*>(expression))) {
                    continue;
                }
                add_column_to_pathtarget(locator_path->path.pathtarget,
                                         reinterpret_cast<Expr*>(copyObject(expression)),
                                         0);
            }
        };
        /* Preserve the scan's ordinary expressions and safe plain expressions
         * introduced by the wrapper projection. Helper-bearing expressions are
         * parent-owned and handled by the recursive Var pass below. */
        append_locator_target(target_path->path.pathtarget);
        append_locator_target(target_candidate.locator_target);
        add_new_columns_to_pathtarget(locator_path->path.pathtarget, target_residual_vars);
        /* A helper-bearing expression is intentionally not evaluated by the
         * locator child, but PostgreSQL setrefs still requires its underlying base
         * Vars to exist in the child/combined tlist.  The wrapper PathTarget is the
         * usual owner; joinrel->reltarget is the authoritative fallback for an
         * upper compound projection. */
        if (!batch_join_append_underlying_target_vars(locator_path->path.pathtarget,
                                                      target_candidate.locator_target,
                                                      target_rel->relid,
                                                      target_var->varattno) ||
            !batch_join_append_underlying_target_vars(locator_path->path.pathtarget,
                                                      joinrel->reltarget,
                                                      target_rel->relid,
                                                      target_var->varattno)) {
            return;
        }
        locator_path->custom_private = static_cast<List*>(copyObject(target_path->custom_private));
        locator_path->custom_paths = slice_project_clone_path_list(target_path->custom_paths);
        locator_path->custom_restrictinfo = NIL;
        if (!path_target_contains_expression(locator_path->path.pathtarget,
                                             reinterpret_cast<Expr*>(target_var))) {
            add_column_to_pathtarget(
                locator_path->path.pathtarget, reinterpret_cast<Expr*>(copyObject(target_var)), 0);
        }
        Const* hidden_rowno = makeConst(INT8OID, -1, InvalidOid, 8, Int64GetDatum(0), false, true);
        add_column_to_pathtarget(
            locator_path->path.pathtarget, reinterpret_cast<Expr*>(hidden_rowno), 0);
        set_pathtarget_cost_width(root, locator_path->path.pathtarget);
        /* A lower BatchJoin may publish an otherwise-unused target sequence as the
         * PARAM_EXEC query of an upper BatchJoin.  The locator child must therefore
         * carry BSS3's exact projected-resno -> physical-varattno route for every
         * sequence it can publish.  Do not infer a physical column for an expression
         * recipe (or silently keep an older, differently-shaped source tag). */
        for (Node* private_node : postgres_list<Node>(locator_path->custom_private)) {
            if (private_node == nullptr || !IsA(private_node, List))
                continue;
            List* tag = reinterpret_cast<List*>(private_node);
            if (tag != NIL && IsA(reinterpret_cast<Node*>(linitial(tag)), Integer) &&
                intVal(linitial(tag)) == k_bioseq_slice_source_magic) {
                return;
            }
        }
        List* locator_source_bindings = NIL;
        Bitmapset* locator_source_attnos = nullptr;
        bool locator_target_source_bound = false;
        bool locator_source_bindings_valid = true;
        int locator_resno = 1;
        for (Node* expression : postgres_list<Node>(locator_path->path.pathtarget->exprs)) {
            if (exprType(expression) == oids.bioseq_seq_type_oid) {
                Var* source = try_cast_pg_node<Var>(expression);
                if (source == nullptr || source->varno != target_var->varno ||
                    source->varattno <= 0 || source->varlevelsup != 0 ||
                    !bms_is_empty(source->varnullingrels) ||
                    bms_is_member(static_cast<int>(source->varattno), locator_source_attnos)) {
                    locator_source_bindings_valid = false;
                    break;
                }
                locator_source_bindings =
                    lappend(locator_source_bindings,
                            list_make3(makeInteger(locator_resno),
                                       makeInteger(0),
                                       makeInteger(static_cast<int>(source->varattno))));
                locator_source_attnos =
                    bms_add_member(locator_source_attnos, static_cast<int>(source->varattno));
                locator_target_source_bound =
                    locator_target_source_bound || equal(source, target_var);
            }
            ++locator_resno;
        }
        if (locator_source_attnos != nullptr)
            bms_free(locator_source_attnos);
        if (!locator_source_bindings_valid || !locator_target_source_bound)
            return;
        locator_path->custom_private =
            lappend(locator_path->custom_private,
                    list_make3(makeInteger(k_bioseq_slice_source_magic),
                               makeInteger(k_bioseq_slice_source_version),
                               locator_source_bindings));
        if (next_batch_join_plan_token <= 0 ||
            next_batch_join_plan_token == (std::numeric_limits<int>::max)()) {
            next_batch_join_plan_token = 1;
        }
        const int token = next_batch_join_plan_token++;
        List* locator_tag = list_make4(makeInteger(k_bioseq_locator_passthrough_magic),
                                       makeInteger(k_bioseq_batch_join_plan_version),
                                       makeInteger(token),
                                       makeInteger(0));
        locator_path->custom_private = lappend(locator_path->custom_private, locator_tag);
        locator_path->path.parallel_safe = false;
        locator_path->path.parallel_aware = false;
        locator_path->path.parallel_workers = 0;

        const auto typmod = compact_sequence_stdc_type_params::decode(target_var->vartypmod);
        auto* join_path = makeNode(CustomPath);
        join_path->path.pathtype = T_CustomScan;
        join_path->path.parent = joinrel;
        join_path->path.pathtarget = copy_pathtarget(joinrel->reltarget);
        join_path->path.param_info = nullptr;
        join_path->path.parallel_aware = false;
        join_path->path.parallel_safe = false;
        join_path->path.parallel_workers = 0;
        join_path->path.pathkeys = NIL;
        join_path->path.rows = std::max(1.0, joinrel->rows);
        /* The query child is fully materialized before the first output, so its
         * complete cost is startup.  The target child is read exactly once; never
         * underprice this as startup-only and force an unadmittable custom join. */
        const double planned_query_rows = std::max(1.0, query_path->rows);
        const double planned_query_width =
            std::max(1, query_path->pathtarget == nullptr ? 1 : query_path->pathtarget->width);
        const Cost query_materialization_cost =
            planned_query_rows *
            (cpu_tuple_cost + cpu_operator_cost * (1.0 + planned_query_width / 64.0));
        const Cost output_cost = join_path->path.rows * cpu_tuple_cost;
        QualCost residual_join_cost{};
        if (residual_join_restrictions != NIL) {
            List* residual_expressions = NIL;
            for (RestrictInfo* residual : postgres_list<RestrictInfo>(residual_join_restrictions)) {
                residual_expressions = lappend(residual_expressions, residual->clause);
            }
            cost_qual_eval(&residual_join_cost, residual_expressions, root);
        }
        QualCost operand_cost{};
        cost_qual_eval(&operand_cost, mmsearch_operand_cost_exprs(batch_mmsearch, oids), root);
        postgres_list<Node> owned_operands(batch_mmsearch->args);
        const std::uint64_t target_symbols =
            estimate_mmsearch_operand_symbols(owned_operands.get_first(), oids);
        const std::uint64_t query_symbols =
            estimate_mmsearch_operand_symbols(owned_operands.get_second(), oids);
        const std::uint64_t admitted_bytes =
            bioseq::common::saturating_add(target_symbols, query_symbols);
        const owned_mmsearch_cost alignment_cost = estimate_owned_mmsearch_cost(
            target_symbols,
            query_symbols,
            locator_path->path.rows,
            planned_requirement != bioseq::indexing::index_read_requirement::require_absent,
            static_cast<std::uint32_t>(std::min<std::size_t>(
                k_bioseq_runtime_buffer_reserve, (std::numeric_limits<std::uint32_t>::max)())),
            admitted_bytes);
        const Cost honest_startup_cost = std::max<Cost>(
            0,
            query_path->total_cost + locator_path->path.startup_cost + query_materialization_cost +
                operand_cost.startup + alignment_cost.startup + residual_join_cost.startup);
        const Cost honest_total_cost = std::max(
            honest_startup_cost,
            std::max<Cost>(0,
                           query_path->total_cost + locator_path->path.total_cost +
                               query_materialization_cost + operand_cost.startup +
                               alignment_cost.startup + residual_join_cost.startup + output_cost +
                               std::max(0.0, locator_path->path.rows) *
                                   (operand_cost.per_tuple + alignment_cost.per_input_tuple) +
                               std::max(0.0, join_path->path.rows) * residual_join_cost.per_tuple));
        if (!std::isfinite(honest_startup_cost) || !std::isfinite(honest_total_cost)) {
            return;
        }

        /* Retain the best incumbent as EXPLAIN diagnostic context only.  Path selection is
         * structural once ownership is established, never a negative sentinel. */
        Cost incumbent_cost_floor = honest_total_cost;
        for (Path* incumbent : postgres_list<Path>(joinrel->pathlist)) {
            if (incumbent == nullptr)
                continue;
            if (std::isfinite(incumbent->total_cost) && incumbent->total_cost >= 0) {
                incumbent_cost_floor = std::min(incumbent_cost_floor, incumbent->total_cost);
            }
        }
        join_path->path.startup_cost = std::max<Cost>(0, honest_startup_cost);
        join_path->path.total_cost =
            std::max(join_path->path.startup_cost, std::max<Cost>(0, honest_total_cost));
        join_path->flags = 0;
        join_path->custom_restrictinfo = list_copy(extra->restrictlist);
        join_path->custom_paths = list_make2(locator_path, query_path);
        join_path->custom_private = list_make5(makeInteger(k_bioseq_batch_join_plan_version),
                                               makeInteger(token),
                                               makeInteger(static_cast<int>(target_rte->relid)),
                                               makeInteger(target_var->varattno),
                                               copyObject(target_var));
        join_path->custom_private = lappend(join_path->custom_private, copyObject(query_var));
        join_path->custom_private =
            lappend(join_path->custom_private, makeInteger(typmod.alphabet_code));
        join_path->custom_private =
            lappend(join_path->custom_private, makeInteger(typmod.storage_unit_byte_width));
        join_path->custom_private = lappend(join_path->custom_private, batch_mmsearch);
        join_path->custom_private =
            lappend(join_path->custom_private, makeInteger(static_cast<int>(planned_requirement)));
        join_path->custom_private =
            lappend(join_path->custom_private, make_batch_join_cost_const(honest_startup_cost));
        join_path->custom_private =
            lappend(join_path->custom_private, make_batch_join_cost_const(honest_total_cost));
        join_path->custom_private =
            lappend(join_path->custom_private, make_batch_join_cost_const(incumbent_cost_floor));
        Assert(list_length(join_path->custom_private) == k_bioseq_batch_join_path_private_fields);
        join_path->methods = bioseq_batch_join_path_methods();
        /** @brief Keeps CustomPath entries that already own the mmsearch restriction. */
        auto retain_existing_owner = [restriction](List* paths) {
            postgres_list<Path> survivors;
            for (Path* incumbent : postgres_list<Path>(paths)) {
                if (incumbent != nullptr && IsA(incumbent, CustomPath) &&
                    list_member_ptr(reinterpret_cast<CustomPath*>(incumbent)->custom_restrictinfo,
                                    restriction)) {
                    survivors.append(incumbent);
                }
            }
            return survivors.release();
        };
        joinrel->pathlist = retain_existing_owner(joinrel->pathlist);
        joinrel->partial_pathlist = retain_existing_owner(joinrel->partial_pathlist);
        add_path(joinrel, &join_path->path);
    }

} // export custom-scan partition declarations
