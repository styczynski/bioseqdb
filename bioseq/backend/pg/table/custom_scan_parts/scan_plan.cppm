/**
 * @file
 * @brief Builds base BioseqScan plans and serialized match-owner descriptors.
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

export module bioseq.postgres.table:custom_scan_scan_plan;

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
import :custom_scan_scan_paths;

using namespace bioseq::postgres::support;

export {

    struct base_match_helper_registration {
        const bioseq_stub_oids* oids = nullptr;
        Node* target_recipe = nullptr;
        Index target_relid = 0;
        AttrNumber primary_target_attno = InvalidAttrNumber;
        AttrNumber current_resno = InvalidAttrNumber;
        List* sites = NIL;
        bool invalid = false;
    };

    [[nodiscard]] bool base_match_helper_site_already_registered(List* sites,
                                                                 FuncExpr* helper,
                                                                 const AttrNumber resno) noexcept {
        if (helper == nullptr)
            return false;
        Node* argument = batch_join_match_helper_argument(helper);
        for (List* encoded : postgres_list<List>(sites)) {
            if (encoded == nullptr || !IsA(encoded, List) ||
                list_length(encoded) != k_bioseq_match_helper_site_fields)
                continue;
            Node* funcid_node = reinterpret_cast<Node*>(list_nth(encoded, k_match_site_funcid));
            Node* resno_node = reinterpret_cast<Node*>(list_nth(encoded, k_match_site_resno));
            Node* encoded_argument =
                reinterpret_cast<Node*>(list_nth(encoded, k_match_site_canonical_argument));
            if (funcid_node != nullptr && IsA(funcid_node, Integer) && resno_node != nullptr &&
                IsA(resno_node, Integer) && intVal(resno_node) == resno &&
                static_cast<Oid>(intVal(funcid_node)) == helper->funcid &&
                encoded_argument != nullptr && argument != nullptr &&
                equal(encoded_argument, argument))
                return true;
        }
        return false;
    }

    struct base_match_helper_registration_visitor {
        [[nodiscard]] pg_walk_step operator()(pg_expression_walk_cursor,
                                              pg_node_view node,
                                              base_match_helper_registration& registration) const {
            FuncExpr* function = node.try_cast<FuncExpr>();
            if (function == nullptr || registration.oids == nullptr ||
                !is_bioseq_projection_funcid(function->funcid, *registration.oids)) {
                return pg_walk_step::descend;
            }
            FuncExpr* helper = batch_join_well_formed_match_helper(
                reinterpret_cast<Node*>(function), *registration.oids);
            if (helper == nullptr || registration.target_recipe == nullptr) {
                registration.invalid = true;
                return pg_walk_step::stop;
            }
            const bool names_recipe =
                batch_join_match_helper_names_recipe(helper, registration.target_recipe);
            const bool names_lineage = batch_join_match_helper_names_base_lineage(
                helper, registration.target_recipe, *registration.oids);
            /* Exact transformed recipes and their unique canonical base lineage are
             * both valid spellings for one base owner.  Resolution across multiple
             * owners remains query-scope fail-closed in the common finalizer. */
            if (!names_recipe && !names_lineage) {
                return pg_walk_step::skip_subtree;
            }
            if (!edit_sequence_argument_matches_recipe(helper,
                                                       registration.target_recipe,
                                                       registration.target_relid,
                                                       registration.primary_target_attno,
                                                       *registration.oids)) {
                registration.invalid = true;
                return pg_walk_step::stop;
            }
            if (base_match_helper_site_already_registered(
                    registration.sites, helper, registration.current_resno)) {
                return pg_walk_step::skip_subtree;
            }
            const Oid result_type = exprType(reinterpret_cast<Node*>(helper));
            if (!OidIsValid(result_type) || result_type != helper->funcresulttype ||
                result_type != get_func_rettype(helper->funcid) ||
                registration.current_resno <= 0) {
                registration.invalid = true;
                return pg_walk_step::stop;
            }
            List* encoded = list_make5(
                makeInteger(registration.current_resno),
                makeInteger(static_cast<int>(helper->funcid)),
                makeInteger(exprTypmod(reinterpret_cast<Node*>(helper))),
                makeInteger(static_cast<int>(exprCollation(reinterpret_cast<Node*>(helper)))),
                makeInteger(static_cast<int>(match_helper_binding_kind::unresolved)));
            encoded = lappend(encoded, copyObject(batch_join_match_helper_argument(helper)));
            encoded = lappend(encoded, makeInteger(0));
            encoded = lappend(encoded, makeInteger(0));
            registration.sites = lappend(registration.sites, encoded);
            return pg_walk_step::skip_subtree;
        }
    };

    [[nodiscard]] List* collect_base_match_helper_sites(List* targetlist,
                                                        Node* target_recipe,
                                                        const Index target_relid,
                                                        const AttrNumber primary_target_attno,
                                                        const bioseq_stub_oids& oids) {
        base_match_helper_registration registration{&oids,
                                                    target_recipe,
                                                    target_relid,
                                                    primary_target_attno,
                                                    InvalidAttrNumber,
                                                    NIL,
                                                    false};
        base_match_helper_registration_visitor visitor;
        for (TargetEntry* entry : postgres_list<TargetEntry>(targetlist)) {
            if (entry == nullptr || entry->expr == nullptr)
                continue;
            registration.current_resno = entry->resno;
            walk_pg_expression_including_root(
                reinterpret_cast<Node*>(entry->expr), registration, visitor);
            if (registration.invalid) {
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
                         errmsg("BioseqScan match helper identity is invalid"),
                         errhint("Use the exact mmsearch target recipe or its unique base source "
                                 "lineage.")));
            }
        }
        return registration.sites;
    }

    Plan* bioseq_column_scan_path_plan_custom_path(PlannerInfo* root,
                                                   RelOptInfo* rel,
                                                   CustomPath* best_path,
                                                   List* tlist,
                                                   List* clauses,
                                                   List* custom_plans) {
        bioseq_log<logging_level::debug>(
            "[bioseq] pg: BioseqColumnScanPath_PlanCustomPath() entered");
        // PAPIESZ: ADD HERE
        /*
         * Must return a CustomScan, not a larger structure containing a
         * CustomScan as the first field. Otherwise, copyObject() will fail to
         * copy the additional fields.
         */
        CustomScan* cscan = makeNode(CustomScan);

        cscan->methods = bioseq_column_scan_scan_methods();
        cscan->custom_private = NIL;
        bool locator_passthrough_requested = false;
        bool lazy_source_projection_requested = false;
        List* owned_mmsearch_exprs = NIL;
        for (Node* possible_tag : postgres_list<Node>(best_path->custom_private)) {
            if (possible_tag == nullptr || !IsA(possible_tag, List))
                continue;
            List* tag = reinterpret_cast<List*>(possible_tag);
            if (tag == NIL || !IsA(reinterpret_cast<Node*>(linitial(tag)), Integer))
                continue;
            if (list_length(tag) == 4 &&
                intVal(list_nth(tag, 0)) == k_bioseq_locator_passthrough_magic) {
                locator_passthrough_requested = true;
            }
            if (list_length(tag) == 3 && intVal(list_nth(tag, 0)) == k_bioseq_slice_source_magic) {
                lazy_source_projection_requested = true;
            }
            if (list_length(tag) == 3 &&
                intVal(list_nth(tag, 0)) == k_bioseq_mmsearch_owner_magic) {
                Node* expressions_node = reinterpret_cast<Node*>(list_nth(tag, 2));
                if (intVal(list_nth(tag, 1)) != k_bioseq_mmsearch_owner_version ||
                    owned_mmsearch_exprs != NIL || expressions_node == nullptr ||
                    !IsA(expressions_node, List) ||
                    (owned_mmsearch_exprs = reinterpret_cast<List*>(expressions_node)) == NIL) {
                    ereport(ERROR,
                            (errcode(ERRCODE_DATA_CORRUPTED),
                             errmsg("Bioseq mmsearch owner path tag is invalid")));
                }
                if (list_length(owned_mmsearch_exprs) > 2) {
                    ereport(ERROR,
                            (errcode(ERRCODE_DATA_CORRUPTED),
                             errmsg("Bioseq mmsearch owner path has too many expressions")));
                }
                for (Node* expression : postgres_list<Node>(owned_mmsearch_exprs)) {
                    FuncExpr* mmsearch = try_cast_pg_node<FuncExpr>(expression);
                    if (mmsearch == nullptr ||
                        !is_mmsearch_funcid(mmsearch->funcid, get_cached_stub_oids())) {
                        ereport(ERROR,
                                (errcode(ERRCODE_DATA_CORRUPTED),
                                 errmsg("Bioseq mmsearch owner path expression is invalid")));
                    }
                }
            }
        }
        List* owned_mmsearch_restrictions = best_path->custom_restrictinfo;
        if ((owned_mmsearch_exprs == NIL) != (owned_mmsearch_restrictions == NIL) ||
            list_length(owned_mmsearch_exprs) != list_length(owned_mmsearch_restrictions)) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("Bioseq mmsearch ownership metadata is inconsistent")));
        }
        Node* planner_owned_target_recipe =
            owned_mmsearch_exprs == NIL
                ? nullptr
                : reinterpret_cast<Node*>(
                      linitial(reinterpret_cast<FuncExpr*>(linitial(owned_mmsearch_exprs))->args));
        const bool planner_owned_target_evaluated =
            planner_owned_target_recipe != nullptr &&
            try_cast_pg_node<Var>(planner_owned_target_recipe) == nullptr;

        /* XXX: also need to store projected column list for EXPLAIN */

        if (bioseq::settings::planner::qual_pushdown::enabled) {
            /*
             * Lists of pushed-down clauses. The Vars in custom_exprs referencing
             * other relations will be changed into exec Params by
             * create_customscan_plan().
             *
             * Like CustomPath->custom_private, keep a list of plain clauses
             * separate from the list of all clauses by making them sublists of a
             * 2-element list.
             *
             * XXX: custom_exprs are the quals that will be pushed into the
             * columnar reader code; some of these may not be usable. We should
             * fix this by processing the quals more completely and using
             * ScanKeys.
             */
            postgres_list<List> private_lists(best_path->custom_private);
            List* plain_clauses =
                extract_actual_clauses(private_lists.get_first(), false /* no pseudoconstants */);
            List* all_clauses =
                extract_actual_clauses(private_lists.get_second(), false /* no pseudoconstants */);
            cscan->custom_exprs = copyObject(list_make2(plain_clauses, all_clauses));
        } else {
            cscan->custom_exprs = list_make2(NIL, NIL);
        }

        List* executable_clauses =
            restrictions_without_owned_mmsearch(clauses,
                                                owned_mmsearch_restrictions,
                                                /*restrictions_are_private_copies=*/false);
        cscan->scan.plan.qual =
            extract_actual_clauses(executable_clauses, false /* no pseudoconstants */);
        if (owned_mmsearch_exprs != NIL) {
            for (Node* qual : postgres_list<Node>(cscan->scan.plan.qual)) {
                if (contains_mmsearch_qualifier(qual, get_cached_stub_oids())) {
                    ereport(ERROR,
                            (errcode(ERRCODE_DATA_CORRUPTED),
                             errmsg("owned mmsearch remained in executable scan quals")));
                }
            }
        }

        /* Split the exact edit predicate out before ExecInitQual sees it.  Its
         * placeholder has no value until match_results are drained, whereas every
         * remaining scan qual is valid in the ordinary pre-alignment stage. */
        std::optional<strict_edit_predicate> strict_edit_predicate;
        {
            const bioseq_stub_oids& edit_oids = get_cached_stub_oids();
            postgres_list<Node> pre_alignment_quals;
            for (Node* qual : postgres_list<Node>(cscan->scan.plan.qual)) {
                if (!contains_bioseq_edit(qual, edit_oids)) {
                    pre_alignment_quals.append(qual);
                    continue;
                }
                if (planner_owned_target_evaluated) {
                    ereport(ERROR,
                            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                             errmsg("bioseq.edit scan predicates do not support a transformed "
                                    "mmsearch target"),
                             errhint("Project bioseq.edit with the exact transformed recipe, or "
                                     "apply the edit predicate in a separate query level.")));
                }
                const auto decoded =
                    decode_strict_edit_upper_bound(qual, best_path->path.parent->relid, edit_oids);
                if (!decoded.has_value() || strict_edit_predicate.has_value()) {
                    ereport(ERROR,
                            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                             errmsg("unsupported bioseq.edit predicate"),
                             errdetail("Use exactly one top-level bioseq.edit(sequence_column) < "
                                       "integer constant predicate.")));
                }
                strict_edit_predicate = *decoded;
            }
            cscan->scan.plan.qual = pre_alignment_quals.release();

            if (strict_edit_predicate.has_value()) {
                postgres_list<List> edit_expr_lists(cscan->custom_exprs);
                cscan->custom_exprs =
                    list_make2(quals_without_bioseq_edit(edit_expr_lists.get_first(), edit_oids),
                               quals_without_bioseq_edit(edit_expr_lists.get_second(), edit_oids));
                Const* threshold =
                    makeConst(INT8OID,
                              -1,
                              InvalidOid,
                              8,
                              Int64GetDatum(strict_edit_predicate->exclusive_upper_bound),
                              false,
                              true);
                List* tag = list_make4(makeInteger(k_bioseq_edit_predicate_magic),
                                       makeInteger(k_bioseq_edit_predicate_version),
                                       threshold,
                                       makeInteger(strict_edit_predicate->sequence_attno));
                cscan->custom_private = lappend(cscan->custom_private, tag);
            }
        }

        const bioseq_stub_oids& plan_stub_oids = get_cached_stub_oids();
        const Index scan_relid = best_path->path.parent->relid;
        if (!locator_passthrough_requested)
            for (auto n : postgres_list<TargetEntry>(root->processed_tlist)) {
                if (FuncExpr* fe = try_cast_pg_node<FuncExpr>(to_pg_node(n->expr))) {
                    bool belongs_to_scan = false;
                    if (is_bioseq_projection_funcid(fe->funcid, plan_stub_oids)) {
                        FuncExpr* helper = batch_join_well_formed_match_helper(
                            reinterpret_cast<Node*>(fe), plan_stub_oids);
                        Var* argument =
                            helper == nullptr
                                ? nullptr
                                : try_cast_pg_node<Var>(batch_join_match_helper_argument(helper));
                        const bool direct_scan_argument =
                            argument != nullptr && argument->varno == scan_relid &&
                            argument->varattno > 0 && argument->varlevelsup == 0 &&
                            bms_is_empty(argument->varnullingrels);
                        /* Keep a direct scan-local mismatch inside this owner so its
                         * finalized injector emits the established target-column
                         * diagnostic.  Wrapped mismatches remain unclaimed and reach
                         * the scalar fail-closed boundary. */
                        if (helper != nullptr && planner_owned_target_recipe != nullptr) {
                            belongs_to_scan =
                                batch_join_match_helper_names_recipe(helper,
                                                                     planner_owned_target_recipe) ||
                                batch_join_match_helper_names_base_lineage(
                                    helper, planner_owned_target_recipe, plan_stub_oids) ||
                                direct_scan_argument;
                        } else {
                            belongs_to_scan = direct_scan_argument;
                        }
                    } else if (fe->funcid == plan_stub_oids.symbol_count_oid) {
                        constexpr int pull_flags = PVC_RECURSE_AGGREGATES |
                                                   PVC_RECURSE_WINDOWFUNCS |
                                                   PVC_RECURSE_PLACEHOLDERS;
                        List* variables = pull_var_clause(reinterpret_cast<Node*>(fe), pull_flags);
                        belongs_to_scan = variables != NIL;
                        for (Var* variable : postgres_list<Var>(variables)) {
                            if (variable == nullptr || variable->varno != scan_relid ||
                                variable->varlevelsup != 0) {
                                belongs_to_scan = false;
                                break;
                            }
                        }
                    }
                    if (belongs_to_scan) {
                        /* Descendant-owner propagation may already have widened this
                         * private BioseqScan PathTarget with the exact whole helper.
                         * Retain one authoritative carrier/site instead of appending a
                         * duplicate from processed_tlist. */
                        bool already_projected = false;
                        for (TargetEntry* existing : postgres_list<TargetEntry>(tlist)) {
                            if (existing != nullptr && equal(existing->expr, n->expr)) {
                                already_projected = true;
                                break;
                            }
                        }
                        if (!already_projected) {
                            TargetEntry* projected = copyObject(n);
                            projected->resno = static_cast<AttrNumber>(list_length(tlist) + 1);
                            tlist = lappend(tlist, projected);
                        }
                    }
                }
            }
        bioseq_log<logging_level::debug>(
            "[bioseq] pg: BioseqColumnScanPath_PlanCustomPath() tlist_is_null={}",
            tlist == nullptr);

        cscan->scan.plan.targetlist = list_copy(tlist);
        cscan->scan.scanrelid = best_path->path.parent->relid;
        if (owned_mmsearch_exprs != NIL) {
            FuncExpr* owned_mmsearch_expr =
                reinterpret_cast<FuncExpr*>(linitial(owned_mmsearch_exprs));
            Node* target_node = list_length(owned_mmsearch_expr->args) >= 1
                                    ? reinterpret_cast<Node*>(linitial(owned_mmsearch_expr->args))
                                    : nullptr;
            Node* target_recipe = target_node;
            RangeTblEntry* owner_rte =
                root != nullptr && root->parse != nullptr &&
                        bioseq_is_concrete_rte_varno(cscan->scan.scanrelid) &&
                        cscan->scan.scanrelid <=
                            static_cast<Index>(list_length(root->parse->rtable))
                    ? rt_fetch(cscan->scan.scanrelid, root->parse->rtable)
                    : nullptr;
            const bioseq_stub_oids& owner_oids = get_cached_stub_oids();
            List* target_lineage = canonical_match_base_lineage(target_recipe, owner_oids);
            Var* primary_source =
                target_lineage == NIL ? nullptr : reinterpret_cast<Var*>(linitial(target_lineage));
            bool valid_lineage = primary_source != nullptr;
            for (Var* source : postgres_list<Var>(target_lineage)) {
                if (source == nullptr || source->varno != cscan->scan.scanrelid ||
                    source->varattno <= 0 || source->varlevelsup != 0 ||
                    !bms_is_empty(source->varnullingrels) ||
                    source->vartype != owner_oids.bioseq_seq_type_oid) {
                    valid_lineage = false;
                    break;
                }
            }
            if (target_recipe == nullptr ||
                exprType(target_recipe) != owner_oids.bioseq_seq_type_oid || !valid_lineage ||
                owner_rte == nullptr || owner_rte->rtekind != RTE_RELATION ||
                !OidIsValid(owner_rte->relid)) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqScan match owner target identity is invalid")));
            }
            List* descriptor =
                make_match_owner_descriptor(target_recipe, cscan->scan.scanrelid, owner_oids);
            List* sites = collect_base_match_helper_sites(cscan->scan.plan.targetlist,
                                                          target_recipe,
                                                          cscan->scan.scanrelid,
                                                          primary_source->varattno,
                                                          owner_oids);
            List* owner_tag = list_make5(makeInteger(k_bioseq_base_match_owner_plan_magic),
                                         makeInteger(k_bioseq_base_match_owner_plan_version),
                                         descriptor,
                                         sites,
                                         makeInteger(static_cast<int>(owner_rte->relid)));
            /* The planner-scope descriptor/sites above deliberately retain their
             * query-local pre-setrefs Vars for ambiguity resolution.  The finalizer
             * fills these two executor-only fields from post-setrefs custom_exprs and
             * scan targetlist identities once Plan.plan_node_id is stable. */
            owner_tag = lappend(owner_tag, NIL);
            owner_tag = lappend(owner_tag, NIL);
            Assert(list_length(owner_tag) == k_bioseq_base_match_owner_plan_fields);
            cscan->custom_private = lappend(cscan->custom_private, owner_tag);
        }
        for (Node* tag_node : postgres_list<Node>(best_path->custom_private)) {
            if (tag_node != nullptr && IsA(tag_node, List)) {
                List* source_tag = reinterpret_cast<List*>(tag_node);
                if (source_tag == NIL ||
                    !IsA(reinterpret_cast<Node*>(linitial(source_tag)), Integer)) {
                    continue;
                }
                List* tag = reinterpret_cast<List*>(copyObject(source_tag));
                if (list_length(tag) == 4 &&
                    intVal(list_nth(tag, 0)) == k_bioseq_locator_passthrough_magic &&
                    intVal(list_nth(tag, 1)) == k_bioseq_batch_join_plan_version) {
                    TargetEntry* rowno_entry =
                        reinterpret_cast<TargetEntry*>(llast(cscan->scan.plan.targetlist));
                    if (rowno_entry == nullptr ||
                        exprType(reinterpret_cast<Node*>(rowno_entry->expr)) != INT8OID) {
                        ereport(
                            ERROR,
                            (errcode(ERRCODE_INTERNAL_ERROR),
                             errmsg("Bioseq locator passthrough row-number column is missing")));
                    }
                    rowno_entry->resjunk = true;
                    lfirst(list_nth_cell(tag, 3)) = makeInteger(rowno_entry->resno);
                    cscan->custom_private = lappend(cscan->custom_private, tag);
                } else if (list_length(tag) == 3 &&
                           IsA(reinterpret_cast<Node*>(list_nth(tag, 1)), Integer) &&
                           intVal(list_nth(tag, 0)) == k_bioseq_slice_source_magic &&
                           intVal(list_nth(tag, 1)) == k_bioseq_slice_source_version &&
                           (list_nth(tag, 2) == nullptr ||
                            IsA(reinterpret_cast<Node*>(list_nth(tag, 2)), List))) {
                    cscan->custom_private = lappend(cscan->custom_private, tag);
                }
            }
        }

        /*
         * Vectorized QUAL execution. Split QUAL list into vectorized list
         * stored in third position of custom_exprs list and rest of
         * qual list (which can't be vectorized) in scan.plan.qual.
         */
        /* Locator/source scans deliberately run the scalar executor so every row
         * can be promoted through its exact provider.  Do not remove vectorizable
         * quals from scan.plan.qual when BeginCustomScan will disable the vector
         * executor; doing so turns alias-local restrictions into a cross product. */
        if (bioseq::settings::vectorization::enabled && !locator_passthrough_requested &&
            !lazy_source_projection_requested) {
            List* candidate_qual_list = rewrite_vectorized_expression_list(cscan->scan.plan.qual);
            List* listDifference = list_difference(candidate_qual_list, cscan->scan.plan.qual);

            bioseq_log<logging_level::debug>("[bioseq] pg: BioseqColumnScanPath_PlanCustomPath() "
                                             "splitting qual list for vectorization");

            cscan->custom_exprs = lappend(cscan->custom_exprs, listDifference);

            if (listDifference != nullptr)
                cscan->scan.plan.qual =
                    list_intersection(cscan->scan.plan.qual, candidate_qual_list);
        } else {
            bioseq_log<logging_level::debug>("[bioseq] pg: BioseqColumnScanPath_PlanCustomPath() "
                                             "vectorization disabled, qual not split");
            /* We still need to create third entry in custom_exprs */
            cscan->custom_exprs = lappend(cscan->custom_exprs, NIL);
        }
        /* The fourth expression list is executor-only ownership state.  Keeping it
         * in custom_exprs lets PostgreSQL replace outer Vars with PARAM_EXEC while
         * ensuring ExecInitQual never sees the rendezvous function. */
        cscan->custom_exprs =
            lappend(cscan->custom_exprs,
                    owned_mmsearch_exprs != NIL ? copyObject(owned_mmsearch_exprs) : NIL);

        /*
         * Freeze every result-affecting BioseqScan setting only after applying
         * the primary mmsearch aligner_options override and the evaluated-target
         * index rule.  Executor, EXPLAIN, and cache-provider consumers decode this
         * payload; none of them reconstruct these values from live semantic GUCs.
         */
        frozen_scan_semantics frozen_semantics{};
        frozen_semantics.params = build_params_from_gucs();
        bioseq::indexing::index_read_requirement planned_requirement =
            bioseq::indexing::index_read_requirement::allow;
        FuncExpr* primary_mmsearch = owned_mmsearch_exprs == NIL
                                         ? nullptr
                                         : try_cast_pg_node<FuncExpr>(reinterpret_cast<Node*>(
                                               linitial(owned_mmsearch_exprs)));
        (void)apply_aligner_options_override(
            primary_mmsearch, frozen_semantics.params, planned_requirement, get_cached_stub_oids());
        finalize_simd_chaining_policy(frozen_semantics.params);
        if (planner_owned_target_evaluated &&
            planned_requirement ==
                bioseq::indexing::index_read_requirement::require_compatible_persisted) {
            ereport(
                ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("a transformed mmsearch operand cannot require a persisted target index"),
                 errhint("Use index_read_requirement=allow or require_absent; evaluated base-local "
                         "operands run against their exact logical views.")));
        }
        frozen_semantics.index_read_requirement =
            planner_owned_target_evaluated
                ? bioseq::indexing::index_read_requirement::require_absent
                : planned_requirement;
        frozen_semantics.read_sequence_index = bioseq::settings::index::sequence_read::enabled;
        cscan->custom_private =
            lappend(cscan->custom_private, make_bioseq_semantic_payload(frozen_semantics));

        bioseq_log<logging_level::debug>(
            "[bioseq] pg: BioseqColumnScanPath_PlanCustomPath() returning");
        return reinterpret_cast<Plan*>(cscan);
    }

    [[nodiscard]] int canonical_match_lineage_var_compare(const ListCell* left_cell,
                                                          const ListCell* right_cell) noexcept {
        const Var* left = reinterpret_cast<const Var*>(lfirst(left_cell));
        const Var* right = reinterpret_cast<const Var*>(lfirst(right_cell));
        if (left->varno != right->varno)
            return left->varno < right->varno ? -1 : 1;
        if (left->varattno != right->varattno) {
            return left->varattno < right->varattno ? -1 : 1;
        }
        if (left->varlevelsup != right->varlevelsup) {
            return left->varlevelsup < right->varlevelsup ? -1 : 1;
        }
        if (left->vartypmod != right->vartypmod) {
            return left->vartypmod < right->vartypmod ? -1 : 1;
        }
        if (left->varcollid != right->varcollid) {
            return left->varcollid < right->varcollid ? -1 : 1;
        }
        return 0;
    }

    [[nodiscard]] List* canonical_match_base_lineage(Node* target_recipe,
                                                     const bioseq_stub_oids& oids) {
        if (target_recipe == nullptr)
            return NIL;
        constexpr int pull_flags =
            PVC_RECURSE_AGGREGATES | PVC_RECURSE_WINDOWFUNCS | PVC_RECURSE_PLACEHOLDERS;
        List* variables = pull_var_clause(target_recipe, pull_flags);
        List* lineage = NIL;
        for (Var* variable : postgres_list<Var>(variables)) {
            if (variable == nullptr || variable->vartype != oids.bioseq_seq_type_oid ||
                !bioseq_is_concrete_rte_varno(variable->varno) || variable->varattno <= 0 ||
                variable->varlevelsup != 0 || !bms_is_empty(variable->varnullingrels))
                continue;
            bool duplicate = false;
            for (Var* existing : postgres_list<Var>(lineage)) {
                if (equal(existing, variable)) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate)
                lineage = lappend(lineage, variable);
        }
        if (list_length(lineage) > 1) {
            list_sort(lineage, canonical_match_lineage_var_compare);
        }
        List* result = NIL;
        for (Var* entry : postgres_list<Var>(lineage)) {
            result = lappend(result, copyObject(entry));
        }
        list_free(lineage);
        list_free(variables);
        return result;
    }

    [[nodiscard]] List* make_match_owner_descriptor(Node* target_recipe,
                                                    const Index base_relid,
                                                    const bioseq_stub_oids& oids) {
        if (target_recipe == nullptr || !bioseq_is_concrete_rte_varno(base_relid)) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("Bioseq match owner descriptor input is invalid")));
        }
        List* descriptor = list_make5(makeInteger(k_bioseq_match_owner_descriptor_magic),
                                      makeInteger(k_bioseq_match_owner_descriptor_version),
                                      makeInteger(0),
                                      copyObject(target_recipe),
                                      canonical_match_base_lineage(target_recipe, oids));
        return lappend(descriptor, makeInteger(static_cast<int>(base_relid)));
    }

} // export custom-scan partition declarations
