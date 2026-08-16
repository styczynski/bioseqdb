/**
 * @file
 * @brief Constructs per-row transformed plans and the stable path methods.
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

export module bioseq.postgres.table:custom_scan_join_plan_transformed_plan;

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

import :custom_scan_scan_plan;

import :custom_scan_join_plan_placeholder_lineage;
import :custom_scan_join_plan_setrefs_rewrite;
import :custom_scan_join_plan_plan_construction;

using namespace bioseq::postgres::support;

export {
    Plan* bioseq_per_row_join_plan_custom_path(PlannerInfo* root,
                                               RelOptInfo* rel,
                                               CustomPath* best_path,
                                               List* tlist,
                                               List* clauses,
                                               List* custom_plans) {
        if (root == nullptr || rel == nullptr || best_path == nullptr ||
            list_length(custom_plans) != 1 ||
            list_length(best_path->custom_private) != k_bioseq_per_row_join_path_private_fields) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqPerRowMmsearch planner state is incomplete")));
        }
        auto private_integer = [best_path](const int index) -> int {
            Node* node = reinterpret_cast<Node*>(list_nth(best_path->custom_private, index));
            if (node == nullptr || !IsA(node, Integer)) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("BioseqPerRowMmsearch private integer is invalid"),
                         errdetail("field=%d", index)));
            }
            return intVal(node);
        };
        const int version = private_integer(0);
        const Oid target_relation_oid = static_cast<Oid>(private_integer(1));
        const Index target_varno = static_cast<Index>(private_integer(2));
        const AttrNumber target_storage_attno = static_cast<AttrNumber>(private_integer(3));
        Node* target_expression = reinterpret_cast<Node*>(list_nth(best_path->custom_private, 4));
        Node* query_expression = reinterpret_cast<Node*>(list_nth(best_path->custom_private, 5));
        FuncExpr* mmsearch = try_cast_pg_node<FuncExpr>(
            reinterpret_cast<Node*>(list_nth(best_path->custom_private, 6)));
        const int alphabet_code = private_integer(7);
        const int storage_unit_byte_width = private_integer(8);
        const int index_read_requirement = private_integer(9);
        const Cost honest_startup_cost = batch_join_cost_from_node(
            reinterpret_cast<Node*>(list_nth(best_path->custom_private, 10)),
            "honest_startup_cost");
        const Cost honest_total_cost = batch_join_cost_from_node(
            reinterpret_cast<Node*>(list_nth(best_path->custom_private, 11)), "honest_total_cost");
        const Cost incumbent_cost_floor = batch_join_cost_from_node(
            reinterpret_cast<Node*>(list_nth(best_path->custom_private, 12)),
            "incumbent_cost_floor");
        const bioseq_stub_oids& oids = get_cached_stub_oids();
        const auto target_typmod_validation_result =
            per_row_validate_sequence_typmod(target_expression, oids);
        const auto query_typmod_validation_result =
            per_row_validate_sequence_typmod(query_expression, oids);
        const int32 expected_typmod =
            compact_sequence_stdc_type_params{static_cast<std::uint8_t>(alphabet_code),
                                              static_cast<std::uint8_t>(storage_unit_byte_width)}
                .encode();
        if (version != k_bioseq_per_row_join_plan_version || !OidIsValid(target_relation_oid) ||
            !bioseq_is_concrete_rte_varno(target_varno) || target_storage_attno <= 0 ||
            target_expression == nullptr || query_expression == nullptr || mmsearch == nullptr ||
            !is_mmsearch_funcid(mmsearch->funcid, oids) ||
            count_mmsearch_occurrences(reinterpret_cast<Node*>(mmsearch), oids) != 1u ||
            exprType(target_expression) != oids.bioseq_seq_type_oid ||
            exprType(query_expression) != oids.bioseq_seq_type_oid ||
            !target_typmod_validation_result.has_value() ||
            !query_typmod_validation_result.has_value() ||
            target_typmod_validation_result->typmod != expected_typmod ||
            !batch_join_typmod_is_supported(query_typmod_validation_result->typmod) ||
            !mmsearch_typmods_share_alphabet(target_typmod_validation_result->typmod,
                                             query_typmod_validation_result->typmod) ||
            alphabet_code < 0 ||
            alphabet_code > static_cast<int>(std::numeric_limits<std::uint8_t>::max()) ||
            (storage_unit_byte_width != 1 && storage_unit_byte_width != 2 &&
             storage_unit_byte_width != 4 && storage_unit_byte_width != 8) ||
            index_read_requirement !=
                static_cast<int>(bioseq::indexing::index_read_requirement::require_absent)) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqPerRowMmsearch private path ABI is invalid")));
        }
        Plan* child_plan = reinterpret_cast<Plan*>(linitial(custom_plans));
        if (child_plan == nullptr) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqPerRowMmsearch child plan is missing")));
        }
        per_row_mmsearch_plan_audit child_audit{};
        per_row_audit_mmsearch_plan(child_plan, child_audit, get_cached_stub_oids(), mmsearch);
        if (!child_audit.clean()) {
            ereport(
                ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("BioseqPerRowMmsearch reconstructed child retained owned mmsearch state"),
                 errdetail("executable=%zu vector=%zu storage=%zu descendant_owner=%zu",
                           child_audit.executable_qual_copies,
                           child_audit.vector_qual_copies,
                           child_audit.storage_qual_copies,
                           child_audit.descendant_owner_copies)));
        }
        per_row_owner_support_validation plan_owner_support{};
        if (!per_row_collect_owner_support(
                root, rel, target_varno, target_expression, plan_owner_support)) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqPerRowMmsearch plan owner support is invalid")));
        }
        List* setrefs_carrier_vars = NIL;
        if (!per_row_collect_setrefs_carrier_vars(target_expression,
                                                  plan_owner_support.helper_argument_vars,
                                                  rel->relids,
                                                  setrefs_carrier_vars)) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqPerRowMmsearch setrefs carriers are invalid")));
        }
        const AttrNumber target_resno =
            find_plan_tlist_expression_resno(child_plan->targetlist, target_expression);
        const AttrNumber query_resno =
            find_plan_tlist_expression_resno(child_plan->targetlist, query_expression);
        if (target_resno <= 0 || query_resno <= 0) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqPerRowMmsearch operand projection is missing"),
                     errdetail("target_resno=%d query_resno=%d",
                               static_cast<int>(target_resno),
                               static_cast<int>(query_resno))));
        }
        /* required_target promised each raw carrier below SliceProject.  Validate
         * that contract before taking the child scan-tuple snapshot, then add only
         * missing owner outputs as resjunk.  The public owner target remains at its
         * construction-time join shape: helper identities and deferred passthrough
         * Vars are plan-only support, added after core can no longer replace the
         * path's sortgroupref storage. */
        /* build_path_tlist replaces lateral Vars with PARAM_EXEC nodes.  The exact
         * no-widening validation belongs to path construction; here validate only the
         * callback target's ordinal and SQL type shape. */
        bool callback_target_consistent =
            best_path->path.pathtarget != nullptr &&
            list_length(tlist) == list_length(best_path->path.pathtarget->exprs);
        int callback_target_index = 0;
        for (TargetEntry* entry : postgres_list<TargetEntry>(tlist)) {
            Node* actual = entry == nullptr ? nullptr : reinterpret_cast<Node*>(entry->expr);
            Node* expected = !callback_target_consistent
                                 ? nullptr
                                 : reinterpret_cast<Node*>(list_nth(
                                       best_path->path.pathtarget->exprs, callback_target_index));
            if (actual == nullptr || expected == nullptr ||
                entry->resno != callback_target_index + 1 ||
                exprType(actual) != exprType(expected) ||
                exprTypmod(actual) != exprTypmod(expected) ||
                exprCollation(actual) != exprCollation(expected)) {
                callback_target_consistent = false;
                break;
            }
            ++callback_target_index;
        }
        if (!callback_target_consistent) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqPerRowMmsearch callback target is inconsistent")));
        }
        List* requested_tlist = list_copy(tlist);
        List* plan_only_passthrough_support = NIL;
        List* plan_only_helper_support = NIL;
        struct per_row_plan_only_helper_binding {
            AttrNumber output_resno = InvalidAttrNumber;
            AttrNumber site_resno = InvalidAttrNumber;
            Node* identity = nullptr;
        };
        std::vector<per_row_plan_only_helper_binding> plan_only_helper_bindings;
        List* inherited_helper_identities =
            batch_join_validated_inherited_helper_identities(child_plan, oids);
        List* current_owner_helper_identities = NIL;
        for (Node* identity : postgres_list<Node>(plan_owner_support.helper_identities)) {
            FuncExpr* helper = per_row_helper_identity_function(identity, oids);
            if (helper != nullptr &&
                (batch_join_match_helper_names_recipe(helper, target_expression) ||
                 batch_join_match_helper_names_base_lineage(helper, target_expression, oids))) {
                per_row_append_unique_expression(current_owner_helper_identities, identity);
            }
        }
        for (Node* identity : postgres_list<Node>(plan_owner_support.helper_identities)) {
            if (!batch_join_descendant_owns_helper_identity(child_plan, identity))
                continue;
            const bool exposed_by_child =
                find_plan_tlist_expression_resno(child_plan->targetlist, identity) > 0;
            const bool classified_inherited =
                batch_join_identity_list_contains(inherited_helper_identities, identity);
            FuncExpr* helper = per_row_helper_identity_function(identity, oids);
            const bool names_recipe = helper != nullptr && batch_join_match_helper_names_recipe(
                                                               helper, target_expression);
            const bool base_lineage =
                helper != nullptr &&
                batch_join_match_helper_names_base_lineage(helper, target_expression, oids);
            /* A helper that names only the shared base lineage deliberately belongs
             * to both this transformed owner and the validated descendant base owner.
             * Register the current site and let finalized semantic binding reject the
             * two candidates with 42702.  Exact-recipe carrier loss is corruption. */
            const bool deliberate_lineage_ambiguity = base_lineage && !names_recipe;
            if ((!exposed_by_child || !classified_inherited) && !deliberate_lineage_ambiguity) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqPerRowMmsearch descendant helper carrier was dropped"),
                         errdetail(
                             "node_type=%d exposed=%s inherited=%s exact_recipe=%s base_lineage=%s",
                             static_cast<int>(nodeTag(identity)),
                             exposed_by_child ? "true" : "false",
                             classified_inherited ? "true" : "false",
                             names_recipe ? "true" : "false",
                             base_lineage ? "true" : "false")));
            }
        }
        for (Node* support : postgres_list<Node>(plan_owner_support.passthrough_vars)) {
            if (find_plan_tlist_expression_resno(requested_tlist, support) > 0)
                continue;
            requested_tlist =
                lappend(requested_tlist,
                        makeTargetEntry(reinterpret_cast<Expr*>(copyObject(support)),
                                        static_cast<AttrNumber>(list_length(requested_tlist) + 1),
                                        nullptr,
                                        /*resjunk=*/true));
            per_row_append_unique_expression(plan_only_passthrough_support, support);
        }
        for (Node* support : postgres_list<Node>(current_owner_helper_identities)) {
            if (batch_join_identity_list_contains(inherited_helper_identities, support))
                continue;
            if (find_plan_tlist_expression_resno(requested_tlist, support) > 0)
                continue;
            requested_tlist =
                lappend(requested_tlist,
                        makeTargetEntry(reinterpret_cast<Expr*>(copyObject(support)),
                                        static_cast<AttrNumber>(list_length(requested_tlist) + 1),
                                        nullptr,
                                        /*resjunk=*/true));
            per_row_append_unique_expression(plan_only_helper_support, support);
        }
        for (Node* identity : postgres_list<Node>(current_owner_helper_identities)) {
            if (batch_join_identity_list_contains(inherited_helper_identities, identity))
                continue;
            if (find_plan_tlist_expression_resno(requested_tlist, identity) <= 0) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqPerRowMmsearch plan-only helper was lost")));
            }
        }
        List* hidden_setrefs_carriers = NIL;
        for (Node* carrier : postgres_list<Node>(setrefs_carrier_vars)) {
            if (find_plan_tlist_expression_resno(child_plan->targetlist, carrier) <= 0) {
                Var* var = try_cast_pg_node<Var>(carrier);
                if (var == nullptr) {
                    ereport(ERROR,
                            (errcode(ERRCODE_INTERNAL_ERROR),
                             errmsg("BioseqPerRowMmsearch setrefs carrier is not a Var")));
                }
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("BioseqPerRowMmsearch child setrefs carrier is missing"),
                         errdetail("varno=%u attno=%d", var->varno, var->varattno)));
            }
            if (find_plan_tlist_expression_resno(requested_tlist, carrier) > 0) {
                continue;
            }
            requested_tlist =
                lappend(requested_tlist,
                        makeTargetEntry(reinterpret_cast<Expr*>(copyObject(carrier)),
                                        static_cast<AttrNumber>(list_length(requested_tlist) + 1),
                                        nullptr,
                                        /*resjunk=*/true));
            per_row_append_unique_expression(hidden_setrefs_carriers, carrier);
        }
        List* combined_tlist = NIL;
        for (TargetEntry* source : postgres_list<TargetEntry>(child_plan->targetlist)) {
            TargetEntry* copied = static_cast<TargetEntry*>(copyObject(source));
            copied->resno = static_cast<AttrNumber>(list_length(combined_tlist) + 1);
            combined_tlist = lappend(combined_tlist, copied);
        }
        const int child_column_count = list_length(combined_tlist);
        const AttrNumber current_site_first_resno = static_cast<AttrNumber>(child_column_count + 1);
        List* placeholder_sites = NIL;
        List* output_tlist = NIL;
        for (TargetEntry* output : postgres_list<TargetEntry>(requested_tlist)) {
            if (output == nullptr)
                continue;
            TargetEntry* copied = static_cast<TargetEntry*>(copyObject(output));
            Node* plan_only_helper_identity = nullptr;
            for (Node* identity : postgres_list<Node>(plan_only_helper_support)) {
                if (equal(copied->expr, identity)) {
                    plan_only_helper_identity = identity;
                    break;
                }
            }
            const int site_count_before = list_length(placeholder_sites);
            batch_join_placeholder_registration registration{&oids,
                                                             target_varno,
                                                             target_storage_attno,
                                                             target_expression,
                                                             &combined_tlist,
                                                             &placeholder_sites,
                                                             false,
                                                             false,
                                                             inherited_helper_identities};
            batch_join_placeholder_registration_visitor visitor;
            walk_pg_expression_including_root(
                reinterpret_cast<Node*>(copied->expr), registration, visitor);
            if (registration.edit_sequence_mismatch) {
                ereport(
                    ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("bioseq.edit projection must reference the mmsearch target source")));
            }
            if (registration.invalid) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("BioseqPerRowMmsearch match helper identity is invalid")));
            }
            if (plan_only_helper_identity != nullptr) {
                FuncExpr* helper =
                    per_row_helper_identity_function(plan_only_helper_identity, oids);
                PlaceHolderVar* placeholder =
                    try_cast_pg_node<PlaceHolderVar>(plan_only_helper_identity);
                const Index phid = placeholder == nullptr ? 0 : placeholder->phid;
                const int site_count_after = list_length(placeholder_sites);
                const AttrNumber site_resno =
                    helper == nullptr ? InvalidAttrNumber
                                      : batch_join_find_same_owner_placeholder_site(
                                            plan_only_helper_identity, helper, phid, registration);
                TargetEntry* site_entry =
                    site_resno < current_site_first_resno ||
                            site_resno > list_length(combined_tlist)
                        ? nullptr
                        : reinterpret_cast<TargetEntry*>(list_nth(combined_tlist, site_resno - 1));
                if (registration.invalid || site_count_after < site_count_before ||
                    site_count_after > site_count_before + 1 || site_resno == InvalidAttrNumber ||
                    site_entry == nullptr || site_entry->resno != site_resno ||
                    !equal(site_entry->expr, plan_only_helper_identity)) {
                    ereport(
                        ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqPerRowMmsearch plan-only helper site identity is invalid"),
                         errdetail("sites_before=%d sites_after=%d",
                                   site_count_before,
                                   site_count_after)));
                }
                plan_only_helper_bindings.push_back(
                    {copied->resno, site_resno, plan_only_helper_identity});
            }
            copied->expr = batch_join_rewrite_output_for_setrefs(copied->expr,
                                                                 combined_tlist,
                                                                 oids,
                                                                 current_site_first_resno,
                                                                 inherited_helper_identities);
            output_tlist = lappend(output_tlist, copied);
        }
        for (Node* carrier : postgres_list<Node>(hidden_setrefs_carriers)) {
            bool found_resjunk = false;
            for (TargetEntry* output : postgres_list<TargetEntry>(output_tlist)) {
                if (output != nullptr && output->resjunk && equal(output->expr, carrier)) {
                    found_resjunk = true;
                    break;
                }
            }
            if (!found_resjunk) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("BioseqPerRowMmsearch hidden setrefs carrier was lost")));
            }
        }
        for (Node* support : postgres_list<Node>(plan_only_passthrough_support)) {
            bool found_resjunk = false;
            for (TargetEntry* output : postgres_list<TargetEntry>(output_tlist)) {
                if (output != nullptr && output->resjunk && equal(output->expr, support)) {
                    found_resjunk = true;
                    break;
                }
            }
            if (!found_resjunk) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqPerRowMmsearch plan-only passthrough was lost")));
            }
        }
        for (const per_row_plan_only_helper_binding& binding : plan_only_helper_bindings) {
            TargetEntry* output_entry = nullptr;
            for (TargetEntry* candidate : postgres_list<TargetEntry>(output_tlist)) {
                if (candidate != nullptr && candidate->resno == binding.output_resno) {
                    output_entry = candidate;
                    break;
                }
            }
            TargetEntry* site_entry =
                binding.site_resno <= 0 || binding.site_resno > list_length(combined_tlist)
                    ? nullptr
                    : reinterpret_cast<TargetEntry*>(
                          list_nth(combined_tlist, binding.site_resno - 1));
            Var* output_slot =
                output_entry == nullptr
                    ? nullptr
                    : try_cast_pg_node<Var>(reinterpret_cast<Node*>(output_entry->expr));
            const bool output_binds_site =
                output_entry != nullptr &&
                (equal(output_entry->expr, binding.identity) ||
                 (output_slot != nullptr && output_slot->varno == INDEX_VAR &&
                  output_slot->varattno == binding.site_resno));
            if (output_entry == nullptr || !output_entry->resjunk || site_entry == nullptr ||
                site_entry->resno != binding.site_resno ||
                !equal(site_entry->expr, binding.identity) || !output_binds_site) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqPerRowMmsearch plan-only helper binding was lost")));
            }
        }
        if (plan_only_helper_bindings.size() !=
            static_cast<std::size_t>(list_length(plan_only_helper_support))) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqPerRowMmsearch plan-only helper binding count is invalid")));
        }
        validate_batch_join_setrefs_coverage(output_tlist, combined_tlist);
        /* custom_restrictinfo is planner-lifetime join metadata, primarily used by
         * core for pseudoconstant gating; PostgreSQL may legitimately pass NIL in
         * `clauses` here.  Executable ownership is instead established by the
         * validated versioned mmsearch recipe above plus the recursive child-plan audit, which
         * rejects every surviving duplicate below this sole owner. */
        (void)clauses;
        auto* cscan = makeNode(CustomScan);
        /* Post-join-recipe mode is a versioned BioseqBatchJoin shape.  Reuse the
         * registered scan/executor method set so copied, cached, and deserialized
         * plans retain the same public owner identity as the raw-Var mode. */
        cscan->methods = bioseq_batch_join_scan_methods();
        cscan->flags = best_path->flags;
        cscan->scan.scanrelid = 0;
        cscan->scan.plan.targetlist = output_tlist;
        cscan->scan.plan.qual = NIL;
        cscan->custom_exprs = NIL;
        cscan->custom_plans = custom_plans;
        cscan->custom_scan_tlist = combined_tlist;
        cscan->custom_relids = bms_copy(rel->relids);
        cscan->custom_private = list_make5(makeInteger(k_bioseq_per_row_join_plan_version),
                                           makeInteger(static_cast<int>(target_relation_oid)),
                                           makeInteger(static_cast<int>(target_storage_attno)),
                                           makeInteger(static_cast<int>(target_resno)),
                                           makeInteger(static_cast<int>(query_resno)));
        cscan->custom_private = lappend(cscan->custom_private, makeInteger(child_column_count));
        cscan->custom_private = lappend(cscan->custom_private, makeInteger(alphabet_code));
        cscan->custom_private =
            lappend(cscan->custom_private, makeInteger(storage_unit_byte_width));
        const auto query_type_params =
            compact_sequence_stdc_type_params::decode(query_typmod_validation_result->typmod);
        cscan->custom_private =
            lappend(cscan->custom_private, makeInteger(query_type_params.alphabet_code));
        cscan->custom_private =
            lappend(cscan->custom_private, makeInteger(query_type_params.storage_unit_byte_width));
        cscan->custom_private =
            lappend(cscan->custom_private,
                    makeInteger(target_typmod_validation_result->descriptor_erased ? 1 : 0));
        cscan->custom_private =
            lappend(cscan->custom_private,
                    makeInteger(query_typmod_validation_result->descriptor_erased ? 1 : 0));
        cscan->custom_private = lappend(cscan->custom_private, placeholder_sites);
        cscan->custom_private = lappend(cscan->custom_private, copyObject(mmsearch));
        cscan->custom_private = lappend(cscan->custom_private, makeInteger(index_read_requirement));
        cscan->custom_private =
            lappend(cscan->custom_private, make_batch_join_cost_const(honest_startup_cost));
        cscan->custom_private =
            lappend(cscan->custom_private, make_batch_join_cost_const(honest_total_cost));
        cscan->custom_private =
            lappend(cscan->custom_private, make_batch_join_cost_const(incumbent_cost_floor));
        cscan->custom_private =
            lappend(cscan->custom_private,
                    make_match_owner_descriptor(target_expression, target_varno, oids));
        Assert(list_length(cscan->custom_private) == k_bioseq_per_row_join_private_fields);
        return reinterpret_cast<Plan*>(cscan);
    }

    /** Returns the stable PostgreSQL path callbacks for batched joins. */
    [[nodiscard]] const CustomPathMethods* bioseq_batch_join_path_methods() noexcept {
        static const CustomPathMethods methods{
            .CustomName = "BioseqBatchJoin",
            .PlanCustomPath = bioseq_batch_join_plan_custom_path,
        };
        return &methods;
    }

} // export custom-scan partition declarations
