/**
 * @file
 * @brief Constructs the batched custom join plan and private payload.
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

export module bioseq.postgres.table:custom_scan_join_plan_plan_construction;

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

using namespace bioseq::postgres::support;

export {
    /**
     * @brief Builds the two-child BatchJoin plan and its serialized executor contract.
     *
     * The plan preserves child target identities required by setrefs while assigning each
     * current-owner match helper to one descriptor-only scan column.
     */
    Plan* bioseq_batch_join_plan_custom_path(PlannerInfo* root,
                                             RelOptInfo* rel,
                                             CustomPath* best_path,
                                             List* tlist,
                                             List* clauses,
                                             List* custom_plans) {
        if (root == nullptr || rel == nullptr || best_path == nullptr ||
            list_length(custom_plans) != 2 ||
            list_length(best_path->custom_private) != k_bioseq_batch_join_path_private_fields) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqBatchJoin planner state is incomplete")));
        }
        const int version = intVal(list_nth(best_path->custom_private, 0));
        const int token = intVal(list_nth(best_path->custom_private, 1));
        const Oid target_relation_oid =
            static_cast<Oid>(intVal(list_nth(best_path->custom_private, 2)));
        const AttrNumber target_storage_seq_attno =
            static_cast<AttrNumber>(intVal(list_nth(best_path->custom_private, 3)));
        Var* target_var =
            try_cast_pg_node<Var>(reinterpret_cast<Node*>(list_nth(best_path->custom_private, 4)));
        Var* query_var =
            try_cast_pg_node<Var>(reinterpret_cast<Node*>(list_nth(best_path->custom_private, 5)));
        const int alphabet_code = intVal(list_nth(best_path->custom_private, 6));
        const int storage_unit_byte_width = intVal(list_nth(best_path->custom_private, 7));
        Node* mmsearch_node = reinterpret_cast<Node*>(list_nth(best_path->custom_private, 8));
        FuncExpr* mmsearch = try_cast_pg_node<FuncExpr>(mmsearch_node);
        const bioseq_stub_oids& oids = get_cached_stub_oids();
        const int mmsearch_arity = mmsearch == nullptr ? 0 : list_length(mmsearch->args);
        Node* private_target_operand =
            mmsearch_arity >= 1 ? reinterpret_cast<Node*>(linitial(mmsearch->args)) : nullptr;
        Node* private_query_operand =
            mmsearch_arity >= 2 ? reinterpret_cast<Node*>(lsecond(mmsearch->args)) : nullptr;
        Const* private_options =
            mmsearch_arity == 3
                ? try_cast_pg_node<Const>(reinterpret_cast<Node*>(list_nth(mmsearch->args, 2)))
                : nullptr;
        const bool private_mmsearch_recipe_valid =
            version == k_bioseq_batch_join_plan_version && mmsearch != nullptr &&
            count_mmsearch_occurrences(mmsearch_node, oids) == 1u &&
            ((mmsearch->funcid == oids.mmsearch_oid && mmsearch_arity == 2) ||
             (mmsearch->funcid == oids.mmsearch_with_opts_oid && mmsearch_arity == 3 &&
              private_options != nullptr &&
              private_options->consttype == oids.bioseq_aligner_options_type_oid)) &&
            private_target_operand != nullptr && private_query_operand != nullptr &&
            equal(private_target_operand, target_var) && equal(private_query_operand, query_var);
        const int index_read_requirement = intVal(list_nth(best_path->custom_private, 9));
        const Cost honest_startup_cost = batch_join_cost_from_node(
            reinterpret_cast<Node*>(list_nth(best_path->custom_private, 10)),
            "honest_startup_cost");
        const Cost honest_total_cost = batch_join_cost_from_node(
            reinterpret_cast<Node*>(list_nth(best_path->custom_private, 11)), "honest_total_cost");
        const Cost incumbent_cost_floor = batch_join_cost_from_node(
            reinterpret_cast<Node*>(list_nth(best_path->custom_private, 12)),
            "incumbent_cost_floor");
        if (version != k_bioseq_batch_join_plan_version || token <= 0 || target_var == nullptr ||
            query_var == nullptr || !private_mmsearch_recipe_valid ||
            !bioseq::indexing::index_read_requirement_ordinal_valid(index_read_requirement) ||
            !batch_join_typmod_is_supported(target_var->vartypmod) ||
            !batch_join_typmod_is_supported(query_var->vartypmod) ||
            !mmsearch_typmods_share_alphabet(target_var->vartypmod, query_var->vartypmod) ||
            target_var->varcollid != query_var->varcollid) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqBatchJoin private plan ABI is invalid")));
        }
        Plan* target_plan = reinterpret_cast<Plan*>(linitial(custom_plans));
        Plan* query_plan = reinterpret_cast<Plan*>(lsecond(custom_plans));
        if (target_plan == nullptr || query_plan == nullptr || !IsA(target_plan, CustomScan)) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqBatchJoin target child is not a custom scan")));
        }
        auto* target_custom = reinterpret_cast<CustomScan*>(target_plan);
        if (target_custom->methods != bioseq_column_scan_scan_methods() ||
            target_custom->scan.scanrelid != target_var->varno) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqBatchJoin target child lacks locator ownership")));
        }
        List* locator_tag = nullptr;
        for (Node* raw_tag : postgres_list<Node>(target_custom->custom_private)) {
            if (raw_tag == nullptr || !IsA(raw_tag, List))
                continue;
            List* candidate = reinterpret_cast<List*>(raw_tag);
            if (list_length(candidate) != 4 ||
                !IsA(reinterpret_cast<Node*>(linitial(candidate)), Integer) ||
                intVal(linitial(candidate)) != k_bioseq_locator_passthrough_magic)
                continue;
            if (locator_tag != nullptr) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("BioseqBatchJoin target has duplicate locator ownership")));
            }
            locator_tag = candidate;
        }
        if (locator_tag == nullptr || !IsA(locator_tag, List) || list_length(locator_tag) != 4 ||
            intVal(list_nth(locator_tag, 0)) != k_bioseq_locator_passthrough_magic ||
            intVal(list_nth(locator_tag, 1)) != k_bioseq_batch_join_plan_version ||
            intVal(list_nth(locator_tag, 2)) != token) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqBatchJoin locator tag/token validation failed")));
        }

        const AttrNumber target_slot_seq_resno =
            find_plan_tlist_expression_resno(target_plan->targetlist, target_var);
        const AttrNumber query_slot_seq_resno =
            find_plan_tlist_expression_resno(query_plan->targetlist, query_var);
        const AttrNumber target_slot_rowno_resno =
            static_cast<AttrNumber>(intVal(list_nth(locator_tag, 3)));
        if (target_slot_seq_resno <= 0 || query_slot_seq_resno <= 0 ||
            target_slot_rowno_resno <= 0 ||
            target_slot_rowno_resno > list_length(target_plan->targetlist)) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqBatchJoin child projection resnos are invalid")));
        }
        TargetEntry* target_seq_entry = reinterpret_cast<TargetEntry*>(
            list_nth(target_plan->targetlist, target_slot_seq_resno - 1));
        TargetEntry* query_seq_entry = reinterpret_cast<TargetEntry*>(
            list_nth(query_plan->targetlist, query_slot_seq_resno - 1));
        TargetEntry* rowno_entry = reinterpret_cast<TargetEntry*>(
            list_nth(target_plan->targetlist, target_slot_rowno_resno - 1));
        if (target_seq_entry == nullptr || query_seq_entry == nullptr || rowno_entry == nullptr ||
            !rowno_entry->resjunk || target_seq_entry->resno != target_slot_seq_resno ||
            query_seq_entry->resno != query_slot_seq_resno ||
            rowno_entry->resno != target_slot_rowno_resno ||
            exprType(reinterpret_cast<Node*>(rowno_entry->expr)) != INT8OID ||
            exprTypmod(reinterpret_cast<Node*>(rowno_entry->expr)) != -1 ||
            OidIsValid(exprCollation(reinterpret_cast<Node*>(rowno_entry->expr))) ||
            exprType(reinterpret_cast<Node*>(target_seq_entry->expr)) !=
                get_cached_stub_oids().bioseq_seq_type_oid ||
            exprType(reinterpret_cast<Node*>(query_seq_entry->expr)) !=
                get_cached_stub_oids().bioseq_seq_type_oid ||
            exprTypmod(reinterpret_cast<Node*>(target_seq_entry->expr)) != target_var->vartypmod ||
            exprTypmod(reinterpret_cast<Node*>(query_seq_entry->expr)) != query_var->vartypmod ||
            exprCollation(reinterpret_cast<Node*>(target_seq_entry->expr)) !=
                target_var->varcollid ||
            exprCollation(reinterpret_cast<Node*>(query_seq_entry->expr)) != query_var->varcollid) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("BioseqBatchJoin child sequence/row-number ABI is invalid")));
        }

        List* combined_tlist = NIL;
        for (TargetEntry* source : postgres_list<TargetEntry>(target_plan->targetlist)) {
            TargetEntry* copied = static_cast<TargetEntry*>(copyObject(source));
            copied->resno = static_cast<AttrNumber>(list_length(combined_tlist) + 1);
            combined_tlist = lappend(combined_tlist, copied);
        }
        const int target_column_count = list_length(combined_tlist);
        for (TargetEntry* source : postgres_list<TargetEntry>(query_plan->targetlist)) {
            TargetEntry* copied = static_cast<TargetEntry*>(copyObject(source));
            copied->resno = static_cast<AttrNumber>(list_length(combined_tlist) + 1);
            combined_tlist = lappend(combined_tlist, copied);
        }
        const int query_column_count = list_length(combined_tlist) - target_column_count;
        const AttrNumber current_site_first_resno =
            static_cast<AttrNumber>(list_length(combined_tlist) + 1);

        List* placeholder_sites = NIL;
        List* existing_helper_identities = NIL;
        List* existing_helper_functions = NIL;
        per_row_owner_support_validation plan_only_owner_support{};
        if (!per_row_collect_owner_support(root,
                                           rel,
                                           target_var->varno,
                                           reinterpret_cast<Node*>(target_var),
                                           plan_only_owner_support)) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqBatchJoin plan-only helper support is invalid")));
        }
        for (int index = 0; index < target_column_count; ++index) {
            TargetEntry* carrier_entry =
                reinterpret_cast<TargetEntry*>(list_nth(combined_tlist, index));
            Node* carrier_identity =
                carrier_entry == nullptr ? nullptr : reinterpret_cast<Node*>(carrier_entry->expr);
            PlaceHolderVar* carrier_placeholder =
                try_cast_pg_node<PlaceHolderVar>(carrier_identity);
            FuncExpr* carrier_helper =
                carrier_placeholder == nullptr
                    ? batch_join_well_formed_match_helper(carrier_identity, oids)
                    : batch_join_match_helper_phv_function(carrier_placeholder, oids);
            if (carrier_helper == nullptr) {
                if (batch_join_expression_has_match_helper(carrier_identity, oids)) {
                    ereport(
                        ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqBatchJoin locator helper carrier is not a whole identity"),
                         errdetail("target_resno=%d", index + 1)));
                }
                continue;
            }
            const bool names_recipe = batch_join_match_helper_names_recipe(
                carrier_helper, reinterpret_cast<Node*>(target_var));
            const bool names_lineage = batch_join_match_helper_names_base_lineage(
                carrier_helper, reinterpret_cast<Node*>(target_var), oids);
            if (!names_recipe && !names_lineage)
                continue;
            if (carrier_entry->resno == target_slot_seq_resno ||
                carrier_entry->resno == target_slot_rowno_resno) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqBatchJoin helper reuses an operand carrier"),
                         errdetail("target_resno=%d", carrier_entry->resno)));
            }
            for (FuncExpr* existing_helper : postgres_list<FuncExpr>(existing_helper_functions)) {
                if (existing_helper != nullptr && equal(existing_helper, carrier_helper)) {
                    ereport(ERROR,
                            (errcode(ERRCODE_DATA_CORRUPTED),
                             errmsg("BioseqBatchJoin locator has duplicate helper carriers"),
                             errdetail("target_resno=%d", carrier_entry->resno)));
                }
            }
            existing_helper_functions =
                lappend(existing_helper_functions, copyObject(carrier_helper));
            if (!edit_sequence_argument_matches_recipe(carrier_helper,
                                                       reinterpret_cast<Node*>(target_var),
                                                       target_var->varno,
                                                       target_storage_seq_attno,
                                                       oids)) {
                ereport(
                    ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("bioseq.edit projection must reference the mmsearch target column")));
            }
            if (find_base_match_owner_plan_tag(target_custom) != NIL ||
                batch_join_scan_owns_helper_identity(target_custom, carrier_identity)) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqBatchJoin locator retained leaf helper ownership"),
                         errdetail("target_resno=%d", carrier_entry->resno)));
            }
            Index phid = 0;
            if (!batch_join_plan_only_helper_phid(plan_only_owner_support.helper_identities,
                                                  carrier_identity,
                                                  carrier_helper,
                                                  oids,
                                                  phid)) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqBatchJoin plan-only helper identity is ambiguous"),
                         errdetail("target_resno=%d", carrier_entry->resno)));
            }
            per_row_append_unique_expression(existing_helper_identities, carrier_identity);
            for (Node* authoritative :
                 postgres_list<Node>(plan_only_owner_support.helper_identities)) {
                FuncExpr* authoritative_helper =
                    per_row_helper_identity_function(authoritative, oids);
                if (authoritative_helper != nullptr &&
                    equal(authoritative_helper, carrier_helper)) {
                    per_row_append_unique_expression(existing_helper_identities, authoritative);
                }
            }
            batch_join_placeholder_registration registration{&oids,
                                                             target_var->varno,
                                                             target_storage_seq_attno,
                                                             reinterpret_cast<Node*>(target_var),
                                                             &combined_tlist,
                                                             &placeholder_sites,
                                                             false,
                                                             false,
                                                             NIL};
            const int site_count_before = list_length(placeholder_sites);
            batch_join_register_existing_placeholder_site(
                carrier_identity, carrier_helper, phid, carrier_entry->resno, registration);
            const AttrNumber registered = batch_join_find_same_owner_placeholder_site(
                carrier_identity, carrier_helper, phid, registration);
            if (registration.invalid || registered != carrier_entry->resno ||
                list_length(placeholder_sites) != site_count_before + 1) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqBatchJoin existing helper site registration failed"),
                         errdetail("target_resno=%d registered_resno=%d",
                                   carrier_entry->resno,
                                   registered)));
            }
        }
        List* requested_tlist = list_copy(tlist);
        for (Node* identity : postgres_list<Node>(plan_only_owner_support.helper_identities)) {
            FuncExpr* helper = per_row_helper_identity_function(identity, oids);
            if (helper == nullptr)
                continue;
            const bool names_recipe =
                batch_join_match_helper_names_recipe(helper, reinterpret_cast<Node*>(target_var));
            const bool names_lineage = batch_join_match_helper_names_base_lineage(
                helper, reinterpret_cast<Node*>(target_var), oids);
            if (!names_recipe && !names_lineage)
                continue;
            if (!edit_sequence_argument_matches_recipe(helper,
                                                       reinterpret_cast<Node*>(target_var),
                                                       target_var->varno,
                                                       target_storage_seq_attno,
                                                       oids)) {
                ereport(
                    ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("bioseq.edit projection must reference the mmsearch target column")));
            }
            if (find_plan_tlist_expression_resno(requested_tlist, identity) > 0) {
                continue;
            }
            /* The callback tlist already carries whole helper dependencies for an
             * upper compound Result, while processed_tlist may retain the same helper
             * inside its authoritative PHV.  Wrapper inequality alone must not append
             * a second executor site: unwrap both identities and reuse only one unique,
             * exact function/argument/type match whose authoritative PHV group has one
             * non-conflicting provenance id.  No match, an ambiguous match, or conflicting
             * positive phids keeps the authoritative identity as its own resjunk carrier. */
            bool semantic_match_ambiguous = false;
            TargetEntry* semantic_match =
                batch_join_find_unique_semantic_helper_entry(requested_tlist,
                                                             identity,
                                                             oids,
                                                             /*minimum_resno=*/1,
                                                             /*maximum_resno=*/MaxAttrNumber,
                                                             semantic_match_ambiguous);
            FuncExpr* semantic_match_helper =
                semantic_match == nullptr
                    ? nullptr
                    : per_row_helper_identity_function(
                          reinterpret_cast<Node*>(semantic_match->expr), oids);
            Index authoritative_phid = 0;
            const bool authoritative_group_unambiguous =
                semantic_match_helper != nullptr &&
                batch_join_plan_only_helper_phid(plan_only_owner_support.helper_identities,
                                                 reinterpret_cast<Node*>(semantic_match->expr),
                                                 semantic_match_helper,
                                                 oids,
                                                 authoritative_phid);
            PlaceHolderVar* semantic_match_phv =
                semantic_match == nullptr ? nullptr
                                          : try_cast_pg_node<PlaceHolderVar>(
                                                reinterpret_cast<Node*>(semantic_match->expr));
            const bool carrier_phid_agrees =
                semantic_match_phv == nullptr || semantic_match_phv->phid == authoritative_phid;
            if (semantic_match != nullptr && !semantic_match_ambiguous &&
                authoritative_group_unambiguous && carrier_phid_agrees) {
                continue;
            }
            requested_tlist =
                lappend(requested_tlist,
                        makeTargetEntry(reinterpret_cast<Expr*>(copyObject(identity)),
                                        static_cast<AttrNumber>(list_length(requested_tlist) + 1),
                                        nullptr,
                                        /*resjunk=*/true));
        }
        List* output_tlist = NIL;
        for (TargetEntry* output : postgres_list<TargetEntry>(requested_tlist)) {
            if (output == nullptr) {
                continue;
            }
            TargetEntry* copied = static_cast<TargetEntry*>(copyObject(output));
            batch_join_placeholder_registration registration{&oids,
                                                             target_var->varno,
                                                             target_storage_seq_attno,
                                                             reinterpret_cast<Node*>(target_var),
                                                             &combined_tlist,
                                                             &placeholder_sites,
                                                             false,
                                                             false,
                                                             existing_helper_identities};
            batch_join_placeholder_registration_visitor visitor;
            walk_pg_expression_including_root(
                reinterpret_cast<Node*>(copied->expr), registration, visitor);
            if (registration.edit_sequence_mismatch) {
                ereport(
                    ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("bioseq.edit projection must reference the mmsearch target column")));
            }
            if (registration.invalid) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("BioseqBatchJoin match helper identity is invalid"),
                         errdetail("only exact target-owned FuncExpr or PlaceHolderVar helper "
                                   "sites are supported")));
            }
            copied->expr = batch_join_rewrite_output_for_setrefs(copied->expr,
                                                                 combined_tlist,
                                                                 oids,
                                                                 current_site_first_resno,
                                                                 existing_helper_identities);
            output_tlist = lappend(output_tlist, copied);
        }
        validate_batch_join_setrefs_coverage(output_tlist, combined_tlist);

        auto* cscan = makeNode(CustomScan);
        cscan->methods = bioseq_batch_join_scan_methods();
        cscan->flags = best_path->flags;
        cscan->scan.scanrelid = 0;
        /* Keep the pre-setrefs output expression identities intact.  PostgreSQL
         * fixes any upper Result/Projection against this child targetlist before
         * it recurses and maps this CustomScan against custom_scan_tlist. */
        cscan->scan.plan.targetlist = output_tlist;
        postgres_list<RestrictInfo> executable_join_restrictions;
        std::size_t removed_mmsearch_occurrences = 0u;
        for (RestrictInfo* candidate : postgres_list<RestrictInfo>(clauses)) {
            const std::size_t occurrences =
                candidate == nullptr
                    ? 0u
                    : count_mmsearch_occurrences(reinterpret_cast<Node*>(candidate->clause), oids);
            if (occurrences != 0u) {
                removed_mmsearch_occurrences =
                    occurrences >
                            std::numeric_limits<std::size_t>::max() - removed_mmsearch_occurrences
                        ? std::numeric_limits<std::size_t>::max()
                        : removed_mmsearch_occurrences + occurrences;
                continue;
            }
            executable_join_restrictions.append(candidate);
        }
        cscan->scan.plan.qual = extract_actual_clauses(executable_join_restrictions.get(), false);
        for (Node* executable : postgres_list<Node>(cscan->scan.plan.qual)) {
            if (contains_mmsearch_qualifier(executable, oids)) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("owned mmsearch remained in BioseqBatchJoin quals")));
            }
        }
        per_row_mmsearch_plan_audit ownership_audit{};
        per_row_audit_mmsearch_expression(reinterpret_cast<Node*>(cscan->scan.plan.qual),
                                          ownership_audit.executable_qual_copies,
                                          oids);
        for (Plan* child : postgres_list<Plan>(custom_plans)) {
            per_row_audit_mmsearch_plan(child, ownership_audit, oids);
        }
        /* PostgreSQL may pass NIL clauses when a fenced upper UPDATE has already
         * consumed the join RestrictInfo while constructing this exact CustomPath.
         * In that one case the versioned private recipe is the ownership identity.
         * Never accept a nonempty clause list without the owned predicate, stacked
         * copies, or a copy retained anywhere in either executable child tree. */
        const bool callback_ownership_valid =
            removed_mmsearch_occurrences == 1u ||
            (removed_mmsearch_occurrences == 0u && clauses == NIL && private_mmsearch_recipe_valid);
        if (!callback_ownership_valid || removed_mmsearch_occurrences > 1u ||
            !ownership_audit.clean()) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqBatchJoin owned restriction identity is invalid"),
                     errdetail(
                         "private_recipe_valid=%s clauses_nil=%s removed_mmsearch_occurrences=%zu "
                         "executable_qual_copies=%zu vector_qual_copies=%zu "
                         "storage_qual_copies=%zu descendant_owner_copies=%zu",
                         private_mmsearch_recipe_valid ? "true" : "false",
                         clauses == NIL ? "true" : "false",
                         removed_mmsearch_occurrences,
                         ownership_audit.executable_qual_copies,
                         ownership_audit.vector_qual_copies,
                         ownership_audit.storage_qual_copies,
                         ownership_audit.descendant_owner_copies)));
        }
        cscan->custom_exprs = NIL;
        cscan->custom_plans = custom_plans;
        cscan->custom_scan_tlist = combined_tlist;
        cscan->custom_relids = bms_copy(rel->relids);
        cscan->custom_private = list_make5(makeInteger(k_bioseq_batch_join_plan_version),
                                           makeInteger(token),
                                           makeInteger(static_cast<int>(target_relation_oid)),
                                           makeInteger(target_storage_seq_attno),
                                           makeInteger(target_slot_seq_resno));
        cscan->custom_private =
            lappend(cscan->custom_private, makeInteger(target_slot_rowno_resno));
        cscan->custom_private = lappend(cscan->custom_private, makeInteger(query_slot_seq_resno));
        cscan->custom_private = lappend(cscan->custom_private, makeInteger(target_column_count));
        cscan->custom_private = lappend(cscan->custom_private, makeInteger(query_column_count));
        cscan->custom_private = lappend(cscan->custom_private, makeInteger(alphabet_code));
        cscan->custom_private =
            lappend(cscan->custom_private, makeInteger(storage_unit_byte_width));
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
                    make_match_owner_descriptor(
                        reinterpret_cast<Node*>(target_var), target_var->varno, oids));
        Assert(list_length(cscan->custom_private) == k_bioseq_batch_join_private_fields);
        return reinterpret_cast<Plan*>(cscan);
    }

} // export custom-scan partition declarations
