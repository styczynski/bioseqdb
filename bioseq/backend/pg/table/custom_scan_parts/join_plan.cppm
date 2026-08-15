/**
 * @file
 * @brief Builds BatchJoin and per-row join plans with finalized placeholder layouts.
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

export module bioseq.postgres.table:custom_scan_join_plan;

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

using namespace bioseq::postgres::support;

export {

    struct batch_join_placeholder_registration {
        const bioseq_stub_oids* oids = nullptr;
        Index target_relid = 0;
        AttrNumber target_sequence_attno = InvalidAttrNumber;
        Node* target_recipe = nullptr;
        List** combined_tlist = nullptr;
        List** encoded_sites = nullptr;
        bool invalid = false;
        bool edit_sequence_mismatch = false;
        /* Helper identities already supplied as match_result columns by a validated
         * descendant BioseqBatchJoin.  An outer owner may pass these through, but
         * must neither execute their SQL stubs nor relabel them as its own sites. */
        List* inherited_helper_identities = NIL;
    };

    [[nodiscard]] bool batch_join_expression_has_match_helper(Node* expression,
                                                              const bioseq_stub_oids& oids) {
        if (expression == nullptr) {
            return false;
        }
        bioseq_feature_detector detector{&oids, scan_feature_flags{}};
        feature_flag_visitor visitor;
        walk_pg_expression_including_root(expression, detector, visitor);
        return detector.flags.any_match_projection;
    }

    /* Multiple output expressions can contain the same current-owner helper, and
     * PlanCustomPath may append the same whole identity as a resjunk carrier.
     * One exact identity/phid pair is one match_result slot.  Reuse that slot while
     * keeping PHV and bare FuncExpr identities distinct. */
    [[nodiscard]] AttrNumber
    batch_join_find_same_owner_placeholder_site(Node* identity,
                                                FuncExpr* function,
                                                const Index phid,
                                                batch_join_placeholder_registration& registration) {
        if (identity == nullptr || function == nullptr || registration.combined_tlist == nullptr ||
            registration.encoded_sites == nullptr) {
            registration.invalid = true;
            return InvalidAttrNumber;
        }
        AttrNumber result = InvalidAttrNumber;
        for (Node* encoded_node : postgres_list<Node>(*registration.encoded_sites)) {
            if (encoded_node == nullptr || !IsA(encoded_node, List)) {
                registration.invalid = true;
                return InvalidAttrNumber;
            }
            List* encoded = reinterpret_cast<List*>(encoded_node);
            if (list_length(encoded) != k_bioseq_match_helper_site_fields) {
                registration.invalid = true;
                return InvalidAttrNumber;
            }
            auto integer_field = [encoded](const int index) -> Integer* {
                Node* node = reinterpret_cast<Node*>(list_nth(encoded, index));
                return node != nullptr && IsA(node, Integer) ? reinterpret_cast<Integer*>(node)
                                                             : nullptr;
            };
            Integer* resno_node = integer_field(k_match_site_resno);
            Integer* funcid_node = integer_field(k_match_site_funcid);
            Integer* typmod_node = integer_field(k_match_site_typmod);
            Integer* collation_node = integer_field(k_match_site_collation);
            Integer* binding_node = integer_field(k_match_site_binding_kind);
            Integer* phid_node = integer_field(k_match_site_phid);
            Integer* stable_id_node = integer_field(k_match_site_stable_id);
            Node* argument =
                reinterpret_cast<Node*>(list_nth(encoded, k_match_site_canonical_argument));
            if (resno_node == nullptr || funcid_node == nullptr || typmod_node == nullptr ||
                collation_node == nullptr || binding_node == nullptr || phid_node == nullptr ||
                stable_id_node == nullptr || argument == nullptr) {
                registration.invalid = true;
                return InvalidAttrNumber;
            }
            const int encoded_resno = intVal(resno_node);
            TargetEntry* supplied =
                encoded_resno <= 0 || encoded_resno > list_length(*registration.combined_tlist)
                    ? nullptr
                    : reinterpret_cast<TargetEntry*>(
                          list_nth(*registration.combined_tlist, encoded_resno - 1));
            if (supplied == nullptr || supplied->resno != encoded_resno) {
                registration.invalid = true;
                return InvalidAttrNumber;
            }
            if (!equal(supplied->expr, identity) || static_cast<Index>(intVal(phid_node)) != phid)
                continue;
            if (result != InvalidAttrNumber ||
                static_cast<Oid>(intVal(funcid_node)) != function->funcid ||
                intVal(typmod_node) != exprTypmod(identity) ||
                static_cast<Oid>(intVal(collation_node)) != exprCollation(identity) ||
                intVal(binding_node) != static_cast<int>(match_helper_binding_kind::unresolved) ||
                !equal(argument, batch_join_match_helper_argument(function)) ||
                intVal(stable_id_node) != 0) {
                registration.invalid = true;
                return InvalidAttrNumber;
            }
            result = static_cast<AttrNumber>(encoded_resno);
        }
        return result;
    }

    void batch_join_register_placeholder_site(Node* identity,
                                              FuncExpr* function,
                                              const Index phid,
                                              batch_join_placeholder_registration& registration) {
        if (identity == nullptr || function == nullptr || registration.combined_tlist == nullptr ||
            registration.encoded_sites == nullptr) {
            registration.invalid = true;
            return;
        }
        const Oid type_oid = exprType(identity);
        const int32 typmod = exprTypmod(identity);
        const Oid collation = exprCollation(identity);
        if (!OidIsValid(type_oid) || type_oid != function->funcresulttype ||
            type_oid != get_func_rettype(function->funcid)) {
            registration.invalid = true;
            return;
        }
        const AttrNumber existing =
            batch_join_find_same_owner_placeholder_site(identity, function, phid, registration);
        if (registration.invalid || existing != InvalidAttrNumber)
            return;
        const AttrNumber resno =
            static_cast<AttrNumber>(list_length(*registration.combined_tlist) + 1);
        /* PostgreSQL fixes a parent Result/Projection before recursing into this
         * child.  Therefore plan.targetlist must retain the original helper/PHV
         * tree long enough for the parent to match the child's output identity.
         * The same exact helper owner is a descriptor-only custom_scan_tlist
         * column: this CustomScan's later setrefs pass maps it to INDEX_VAR, and
         * BioseqBatchJoin fills the slot from match_result.  No executor evaluates
         * the function stub.  Distinct PHV ids or FuncExpr identities receive
         * distinct resnos even when their SQL result types are the same. */
        TargetEntry* scan_entry =
            makeTargetEntry(reinterpret_cast<Expr*>(copyObject(identity)), resno, nullptr, false);
        *registration.combined_tlist = lappend(*registration.combined_tlist, scan_entry);
        List* encoded =
            list_make5(makeInteger(resno),
                       makeInteger(static_cast<int>(function->funcid)),
                       makeInteger(typmod),
                       makeInteger(static_cast<int>(collation)),
                       makeInteger(static_cast<int>(match_helper_binding_kind::unresolved)));
        encoded = lappend(encoded, copyObject(batch_join_match_helper_argument(function)));
        encoded = lappend(encoded, makeInteger(static_cast<int>(phid)));
        encoded = lappend(encoded, makeInteger(0));
        *registration.encoded_sites = lappend(*registration.encoded_sites, encoded);
    }

    /* A raw BatchJoin's locator child can already expose a whole helper as a
     * plan-only dummy target even when the callback tlist defers that helper to an
     * upper Result.  Make the existing target-child ordinal the match_result site;
     * adding a second scan column would leave the dummy leaf carrier unowned and
     * would give setrefs two spellings for one semantic result. */
    void batch_join_register_existing_placeholder_site(
        Node* identity,
        FuncExpr* function,
        const Index phid,
        const AttrNumber existing_resno,
        batch_join_placeholder_registration& registration) {
        if (identity == nullptr || function == nullptr || existing_resno <= 0 ||
            registration.combined_tlist == nullptr || registration.encoded_sites == nullptr ||
            existing_resno > list_length(*registration.combined_tlist)) {
            registration.invalid = true;
            return;
        }
        TargetEntry* supplied = reinterpret_cast<TargetEntry*>(
            list_nth(*registration.combined_tlist, existing_resno - 1));
        const Oid type_oid = exprType(identity);
        const int32 typmod = exprTypmod(identity);
        const Oid collation = exprCollation(identity);
        if (supplied == nullptr || supplied->resno != existing_resno ||
            !equal(supplied->expr, identity) || !OidIsValid(type_oid) ||
            type_oid != function->funcresulttype ||
            type_oid != get_func_rettype(function->funcid)) {
            registration.invalid = true;
            return;
        }
        const AttrNumber same_identity =
            batch_join_find_same_owner_placeholder_site(identity, function, phid, registration);
        if (registration.invalid || same_identity != InvalidAttrNumber)
            return;
        for (List* encoded : postgres_list<List>(*registration.encoded_sites)) {
            Node* resno_node = encoded == nullptr || !IsA(encoded, List) ||
                                       list_length(encoded) != k_bioseq_match_helper_site_fields
                                   ? nullptr
                                   : reinterpret_cast<Node*>(list_nth(encoded, k_match_site_resno));
            if (resno_node == nullptr || !IsA(resno_node, Integer)) {
                registration.invalid = true;
                return;
            }
            if (intVal(resno_node) == existing_resno) {
                registration.invalid = true;
                return;
            }
        }
        List* encoded =
            list_make5(makeInteger(existing_resno),
                       makeInteger(static_cast<int>(function->funcid)),
                       makeInteger(typmod),
                       makeInteger(static_cast<int>(collation)),
                       makeInteger(static_cast<int>(match_helper_binding_kind::unresolved)));
        encoded = lappend(encoded, copyObject(batch_join_match_helper_argument(function)));
        encoded = lappend(encoded, makeInteger(static_cast<int>(phid)));
        encoded = lappend(encoded, makeInteger(0));
        *registration.encoded_sites = lappend(*registration.encoded_sites, encoded);
    }

    [[nodiscard]] bool batch_join_identity_list_contains(List* identities,
                                                         Node* identity) noexcept {
        if (identity == nullptr)
            return false;
        for (Node* inherited : postgres_list<Node>(identities)) {
            if (inherited != nullptr && equal(inherited, identity))
                return true;
        }
        return false;
    }

    [[nodiscard]] bool
    batch_join_identity_is_inherited(batch_join_placeholder_registration const& registration,
                                     Node* identity) noexcept {
        return batch_join_identity_list_contains(registration.inherited_helper_identities,
                                                 identity);
    }

    [[nodiscard]] List* find_base_match_owner_plan_tag(CustomScan* scan);

    /* Decode only finalized-plan-independent placeholder ownership.  The stable
     * id is deliberately still zero here: this validation runs in PlanCustomPath,
     * before the top-level planner hook assigns final plan_node_id bindings. */
    [[nodiscard]] bool batch_join_scan_owns_helper_identity(CustomScan* scan,
                                                            Node* identity) noexcept {
        if (scan == nullptr || identity == nullptr || scan->methods == nullptr) {
            return false;
        }
        Node* sites_node = nullptr;
        List* supplied_tlist = NIL;
        if (scan->methods->CustomName != nullptr &&
            strcmp(scan->methods->CustomName, "BioseqBatchJoin") == 0 &&
            scan->custom_private != NIL) {
            Node* version_node = reinterpret_cast<Node*>(linitial(scan->custom_private));
            if (version_node == nullptr || !IsA(version_node, Integer))
                return false;
            const int version = intVal(version_node);
            const int sites_index =
                version == k_bioseq_batch_join_plan_version
                    ? k_batch_join_placeholder_sites
                    : (version == k_bioseq_per_row_join_plan_version ? k_per_row_placeholder_sites
                                                                     : -1);
            if (sites_index < 0 || sites_index >= list_length(scan->custom_private))
                return false;
            sites_node = reinterpret_cast<Node*>(list_nth(scan->custom_private, sites_index));
            supplied_tlist = scan->custom_scan_tlist;
        } else if (scan->methods == bioseq_column_scan_scan_methods()) {
            List* owner_tag = find_base_match_owner_plan_tag(scan);
            if (owner_tag == NIL)
                return false;
            sites_node = reinterpret_cast<Node*>(list_nth(owner_tag, k_base_match_plan_sites));
            supplied_tlist = scan->scan.plan.targetlist;
        } else {
            return false;
        }
        if (sites_node == nullptr)
            return false;
        if (!IsA(sites_node, List))
            return false;
        for (List* encoded : postgres_list<List>(reinterpret_cast<List*>(sites_node))) {
            if (encoded == nullptr || !IsA(encoded, List) ||
                list_length(encoded) != k_bioseq_match_helper_site_fields)
                continue;
            Node* resno_node = reinterpret_cast<Node*>(list_nth(encoded, k_match_site_resno));
            if (resno_node == nullptr || !IsA(resno_node, Integer))
                continue;
            const int resno = intVal(resno_node);
            TargetEntry* supplied =
                resno <= 0 || resno > list_length(supplied_tlist)
                    ? nullptr
                    : reinterpret_cast<TargetEntry*>(list_nth(supplied_tlist, resno - 1));
            if (supplied != nullptr && supplied->resno == resno && equal(supplied->expr, identity))
                return true;
        }
        return false;
    }

    [[nodiscard]] bool batch_join_descendant_owns_helper_identity(Plan* plan, Node* identity) {
        if (plan == nullptr || identity == nullptr)
            return false;
        check_stack_depth();
        if (IsA(plan, CustomScan)) {
            auto* custom = reinterpret_cast<CustomScan*>(plan);
            if (batch_join_scan_owns_helper_identity(custom, identity))
                return true;
            for (Plan* child : postgres_list<Plan>(custom->custom_plans)) {
                if (batch_join_descendant_owns_helper_identity(child, identity)) {
                    return true;
                }
            }
            return false;
        }
        auto search_plan_list = [identity](List* plans) {
            for (Plan* child : postgres_list<Plan>(plans)) {
                if (batch_join_descendant_owns_helper_identity(child, identity)) {
                    return true;
                }
            }
            return false;
        };
        if (IsA(plan, Append)) {
            return search_plan_list(reinterpret_cast<Append*>(plan)->appendplans);
        }
        if (IsA(plan, MergeAppend)) {
            return search_plan_list(reinterpret_cast<MergeAppend*>(plan)->mergeplans);
        }
        if (IsA(plan, BitmapAnd)) {
            return search_plan_list(reinterpret_cast<BitmapAnd*>(plan)->bitmapplans);
        }
        if (IsA(plan, BitmapOr)) {
            return search_plan_list(reinterpret_cast<BitmapOr*>(plan)->bitmapplans);
        }
        if (IsA(plan, SubqueryScan)) {
            /* A SubqueryScan starts a distinct helper-binding scope.  Equal local
             * expression spelling below it is not provenance for this owner. */
            return false;
        }
        return batch_join_descendant_owns_helper_identity(outerPlan(plan), identity) ||
               (innerPlan(plan) != outerPlan(plan) &&
                batch_join_descendant_owns_helper_identity(innerPlan(plan), identity));
    }

    [[nodiscard]] List*
    batch_join_validated_inherited_helper_identities(Plan* child_plan,
                                                     const bioseq_stub_oids& oids) {
        List* result = NIL;
        if (child_plan == nullptr)
            return result;
        for (TargetEntry* entry : postgres_list<TargetEntry>(child_plan->targetlist)) {
            Node* identity = entry == nullptr ? nullptr : reinterpret_cast<Node*>(entry->expr);
            if (identity == nullptr || !batch_join_expression_has_match_helper(identity, oids) ||
                !batch_join_descendant_owns_helper_identity(child_plan, identity))
                continue;
            bool duplicate = false;
            for (Node* existing : postgres_list<Node>(result)) {
                if (equal(existing, identity)) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate)
                result = lappend(result, copyObject(identity));
        }
        return result;
    }

    struct batch_join_placeholder_registration_visitor {
        [[nodiscard]] pg_walk_step
        operator()(pg_expression_walk_cursor,
                   pg_node_view node,
                   batch_join_placeholder_registration& registration) const {
            if (PlaceHolderVar* placeholder = node.try_cast<PlaceHolderVar>()) {
                FuncExpr* helper =
                    registration.oids == nullptr
                        ? nullptr
                        : batch_join_match_helper_phv_function(placeholder, *registration.oids);
                if (helper != nullptr) {
                    if (batch_join_identity_is_inherited(registration,
                                                         reinterpret_cast<Node*>(placeholder))) {
                        return pg_walk_step::skip_subtree;
                    }
                    if (!edit_sequence_argument_matches_recipe(helper,
                                                               registration.target_recipe,
                                                               registration.target_relid,
                                                               registration.target_sequence_attno,
                                                               *registration.oids)) {
                        registration.invalid = true;
                        registration.edit_sequence_mismatch = true;
                        return pg_walk_step::stop;
                    }
                    const bool names_recipe =
                        batch_join_match_helper_names_recipe(helper, registration.target_recipe);
                    const bool base_lineage = batch_join_match_helper_names_base_lineage(
                        helper, registration.target_recipe, *registration.oids);
                    if (!names_recipe && !base_lineage) {
                        registration.invalid = true;
                        return pg_walk_step::stop;
                    }
                    batch_join_register_placeholder_site(reinterpret_cast<Node*>(placeholder),
                                                         helper,
                                                         placeholder->phid,
                                                         registration);
                    return registration.invalid ? pg_walk_step::stop : pg_walk_step::skip_subtree;
                }
                /* A generic PHV containing a helper has evaluation/nulling semantics
                 * that this join cannot reproduce.  Do not descend and accidentally
                 * register only the inner FuncExpr. */
                if (registration.oids != nullptr &&
                    batch_join_expression_has_match_helper(
                        reinterpret_cast<Node*>(placeholder->phexpr), *registration.oids)) {
                    registration.invalid = true;
                    return pg_walk_step::stop;
                }
                return pg_walk_step::descend;
            }
            if (FuncExpr* function = node.try_cast<FuncExpr>()) {
                if (registration.oids != nullptr &&
                    is_bioseq_projection_funcid(function->funcid, *registration.oids)) {
                    if (batch_join_identity_is_inherited(registration,
                                                         reinterpret_cast<Node*>(function))) {
                        return pg_walk_step::skip_subtree;
                    }
                    if (!edit_sequence_argument_matches_recipe(function,
                                                               registration.target_recipe,
                                                               registration.target_relid,
                                                               registration.target_sequence_attno,
                                                               *registration.oids)) {
                        registration.invalid = true;
                        registration.edit_sequence_mismatch = true;
                        return pg_walk_step::stop;
                    }
                    const bool names_recipe =
                        batch_join_match_helper_names_recipe(function, registration.target_recipe);
                    const bool base_lineage = batch_join_match_helper_names_base_lineage(
                        function, registration.target_recipe, *registration.oids);
                    if (!names_recipe && !base_lineage) {
                        registration.invalid = true;
                        return pg_walk_step::stop;
                    }
                    batch_join_register_placeholder_site(
                        reinterpret_cast<Node*>(function), function, 0, registration);
                    return registration.invalid ? pg_walk_step::stop : pg_walk_step::skip_subtree;
                }
            }
            return pg_walk_step::descend;
        }
    };

    struct batch_join_output_setrefs_rewrite {
        List* scan_tlist = NIL;
        const bioseq_stub_oids* oids = nullptr;
        AttrNumber current_site_first_resno = 1;
        List* inherited_helper_identities = NIL;
        bool invalid = false;
    };

    [[nodiscard]] TargetEntry*
    batch_join_find_exact_scan_entry(List* scan_tlist,
                                     const Node* expression,
                                     const AttrNumber minimum_resno = 1,
                                     const AttrNumber maximum_resno = MaxAttrNumber) noexcept {
        if (expression == nullptr)
            return nullptr;
        /* Sites are appended after inherited child columns.  Search backwards
         * within the caller's validated range so an outer owner binds the site it just
         * registered, never the first equal identity inherited from a descendant. */
        for (int index = list_length(scan_tlist) - 1; index >= 0; --index) {
            TargetEntry* entry = reinterpret_cast<TargetEntry*>(list_nth(scan_tlist, index));
            if (entry != nullptr && entry->resno >= minimum_resno &&
                entry->resno <= maximum_resno && equal(entry->expr, expression))
                return entry;
        }
        return nullptr;
    }

    [[nodiscard]] TargetEntry*
    batch_join_find_unique_semantic_helper_entry(List* scan_tlist,
                                                 Node* identity,
                                                 const bioseq_stub_oids& oids,
                                                 const AttrNumber minimum_resno,
                                                 const AttrNumber maximum_resno,
                                                 bool& ambiguous) noexcept {
        ambiguous = false;
        FuncExpr* expected = per_row_helper_identity_function(identity, oids);
        if (expected == nullptr)
            return nullptr;
        TargetEntry* result = nullptr;
        for (TargetEntry* entry : postgres_list<TargetEntry>(scan_tlist)) {
            if (entry == nullptr || entry->expr == nullptr || entry->resno < minimum_resno ||
                entry->resno > maximum_resno) {
                continue;
            }
            Node* supplied_identity = reinterpret_cast<Node*>(entry->expr);
            FuncExpr* supplied = per_row_helper_identity_function(supplied_identity, oids);
            if (supplied == nullptr || !equal(supplied, expected) ||
                exprType(supplied_identity) != exprType(identity) ||
                exprTypmod(supplied_identity) != exprTypmod(identity) ||
                exprCollation(supplied_identity) != exprCollation(identity)) {
                continue;
            }
            if (result != nullptr) {
                ambiguous = true;
                return nullptr;
            }
            result = entry;
        }
        return result;
    }

    [[nodiscard]] TargetEntry* batch_join_find_compatible_var_entry(List* scan_tlist,
                                                                    const Var* var) noexcept {
        if (var == nullptr)
            return nullptr;
        for (TargetEntry* entry : postgres_list<TargetEntry>(scan_tlist)) {
            Var* supplied = entry == nullptr
                                ? nullptr
                                : try_cast_pg_node<Var>(reinterpret_cast<Node*>(entry->expr));
            if (supplied != nullptr && supplied->varno == var->varno &&
                supplied->varattno == var->varattno && supplied->varlevelsup == var->varlevelsup &&
                supplied->vartype == var->vartype && supplied->vartypmod == var->vartypmod &&
                supplied->varcollid == var->varcollid &&
                bms_equal(supplied->varnullingrels, var->varnullingrels)) {
                return entry;
            }
        }
        return nullptr;
    }

    /* CustomScan setrefs resolves output Vars against the unfixed scan tlist.
     * Normalize harmless base-Var spelling deltas, but preserve the exact raw
     * helper/PHV identity in both lists.  Upper setrefs fixes a parent before it
     * recurses into this child, so hiding that identity here makes Sort/Result
     * fail to bind the whole helper expression.  PostgreSQL's normal CustomScan
     * setrefs pass then maps those identities to INDEX_VAR against
     * custom_scan_tlist before executor initialization. */
    struct batch_join_output_setrefs_rewrite_mutator {
        [[nodiscard]] pg_node_mutation
        operator()(pg_expression_mutation_cursor,
                   pg_node_view node,
                   batch_join_output_setrefs_rewrite& rewrite) const {
            if (Var* var = node.try_cast<Var>()) {
                if (var->varno == INDEX_VAR || var->varno == OUTER_VAR || var->varno == INNER_VAR) {
                    return pg_node_mutation::replace_with(reinterpret_cast<Node*>(copyObject(var)));
                }
                TargetEntry* entry = batch_join_find_exact_scan_entry(rewrite.scan_tlist,
                                                                      reinterpret_cast<Node*>(var));
                if (entry == nullptr)
                    entry = batch_join_find_compatible_var_entry(rewrite.scan_tlist, var);
                if (entry == nullptr) {
                    rewrite.invalid = true;
                    return pg_node_mutation::replace_with(reinterpret_cast<Node*>(copyObject(var)));
                }
                return pg_node_mutation::replace_with(
                    reinterpret_cast<Node*>(copyObject(entry->expr)));
            }

            bool helper_identity = false;
            if (PlaceHolderVar* placeholder = node.try_cast<PlaceHolderVar>()) {
                helper_identity = rewrite.oids != nullptr &&
                                  batch_join_expression_has_match_helper(
                                      reinterpret_cast<Node*>(placeholder->phexpr), *rewrite.oids);
            } else if (FuncExpr* function = node.try_cast<FuncExpr>()) {
                helper_identity = rewrite.oids != nullptr &&
                                  is_bioseq_projection_funcid(function->funcid, *rewrite.oids);
            }
            if (!helper_identity)
                return pg_node_mutation::descend();

            const bool inherited =
                batch_join_identity_list_contains(rewrite.inherited_helper_identities, node.get());
            TargetEntry* entry =
                inherited ? batch_join_find_exact_scan_entry(
                                rewrite.scan_tlist,
                                node.get(),
                                1,
                                static_cast<AttrNumber>(rewrite.current_site_first_resno - 1))
                          : batch_join_find_exact_scan_entry(
                                rewrite.scan_tlist, node.get(), rewrite.current_site_first_resno);
            if (entry == nullptr && inherited && rewrite.oids != nullptr) {
                bool ambiguous = false;
                entry = batch_join_find_unique_semantic_helper_entry(
                    rewrite.scan_tlist,
                    node.get(),
                    *rewrite.oids,
                    1,
                    static_cast<AttrNumber>(rewrite.current_site_first_resno - 1),
                    ambiguous);
                if (ambiguous)
                    rewrite.invalid = true;
            }
            if (entry == nullptr || entry->resno <= 0) {
                rewrite.invalid = true;
                return pg_node_mutation::replace_with(
                    reinterpret_cast<Node*>(copyObject(node.get())));
            }
            return pg_node_mutation::replace_with(reinterpret_cast<Node*>(copyObject(entry->expr)));
        }
    };

    [[nodiscard]] Expr*
    batch_join_rewrite_output_for_setrefs(Expr* expression,
                                          List* scan_tlist,
                                          const bioseq_stub_oids& oids,
                                          const AttrNumber current_site_first_resno,
                                          List* inherited_helper_identities) {
        batch_join_output_setrefs_rewrite rewrite{
            scan_tlist, &oids, current_site_first_resno, inherited_helper_identities, false};
        batch_join_output_setrefs_rewrite_mutator mutator;
        Expr* result = reinterpret_cast<Expr*>(
            mutate_pg_expression(reinterpret_cast<Node*>(expression), rewrite, mutator));
        if (rewrite.invalid) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqBatchJoin output cannot be bound to its scan tuple")));
        }
        return result;
    }

    [[nodiscard]] AttrNumber find_plan_tlist_expression_resno(List* targetlist,
                                                              const void* expression) noexcept {
        for (TargetEntry* entry : postgres_list<TargetEntry>(targetlist)) {
            if (entry != nullptr && equal(entry->expr, expression)) {
                return entry->resno;
            }
        }
        return InvalidAttrNumber;
    }

    struct batch_join_setrefs_coverage {
        List* scan_tlist = NIL;
        const bioseq_stub_oids* oids = nullptr;
        Var* missing_var = nullptr;
        Node* missing_helper_identity = nullptr;
    };

    /* Mirror the relevant fix_upper_expr() lookup order before PostgreSQL's final
     * setrefs pass.  A CustomScan with scanrelid == 0 must make every output Var
     * discoverable in custom_scan_tlist; diagnosing that invariant here preserves
     * the exact offending varno/attno instead of reducing it to PostgreSQL's
     * generic "variable not found in subplan target list" error. */
    struct batch_join_setrefs_coverage_visitor {
        [[nodiscard]] pg_walk_step operator()(pg_expression_walk_cursor,
                                              pg_node_view node,
                                              batch_join_setrefs_coverage& coverage) const {
            if (Var* var = node.try_cast<Var>()) {
                for (TargetEntry* entry : postgres_list<TargetEntry>(coverage.scan_tlist)) {
                    Var* supplied =
                        entry == nullptr
                            ? nullptr
                            : try_cast_pg_node<Var>(reinterpret_cast<Node*>(entry->expr));
                    if (supplied != nullptr && supplied->varno == var->varno &&
                        supplied->varattno == var->varattno &&
                        bms_equal(supplied->varnullingrels, var->varnullingrels)) {
                        return pg_walk_step::skip_subtree;
                    }
                }
                coverage.missing_var = var;
                return pg_walk_step::stop;
            }
            if (PlaceHolderVar* placeholder = node.try_cast<PlaceHolderVar>()) {
                for (TargetEntry* entry : postgres_list<TargetEntry>(coverage.scan_tlist)) {
                    if (entry != nullptr && IsA(entry->expr, PlaceHolderVar) &&
                        equal(entry->expr, placeholder)) {
                        return pg_walk_step::skip_subtree;
                    }
                }
                if (coverage.oids != nullptr &&
                    batch_join_expression_has_match_helper(
                        reinterpret_cast<Node*>(placeholder->phexpr), *coverage.oids)) {
                    coverage.missing_helper_identity = reinterpret_cast<Node*>(placeholder);
                    return pg_walk_step::stop;
                }
                return pg_walk_step::descend;
            }
            for (TargetEntry* entry : postgres_list<TargetEntry>(coverage.scan_tlist)) {
                if (entry != nullptr && entry->expr != nullptr && !IsA(entry->expr, Var) &&
                    !IsA(entry->expr, PlaceHolderVar) && equal(entry->expr, node.get())) {
                    return pg_walk_step::skip_subtree;
                }
            }
            if (FuncExpr* function = node.try_cast<FuncExpr>();
                coverage.oids != nullptr && function != nullptr &&
                is_bioseq_projection_funcid(function->funcid, *coverage.oids)) {
                coverage.missing_helper_identity = reinterpret_cast<Node*>(function);
                return pg_walk_step::stop;
            }
            return pg_walk_step::descend;
        }
    };

    void validate_batch_join_setrefs_coverage(List* output_tlist, List* scan_tlist) {
        const bioseq_stub_oids& oids = get_cached_stub_oids();
        batch_join_setrefs_coverage coverage{scan_tlist, &oids, nullptr, nullptr};
        batch_join_setrefs_coverage_visitor visitor;
        for (TargetEntry* output : postgres_list<TargetEntry>(output_tlist)) {
            if (output == nullptr || output->expr == nullptr) {
                continue;
            }
            walk_pg_expression_including_root(
                reinterpret_cast<Node*>(output->expr), coverage, visitor);
            if (coverage.missing_helper_identity != nullptr) {
                ereport(
                    ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqBatchJoin helper identity is absent from scan tuple"),
                     errdetail(
                         "node_type=%d scan_columns=%d; executing the SQL helper stub is forbidden",
                         static_cast<int>(nodeTag(coverage.missing_helper_identity)),
                         list_length(scan_tlist))));
            }
            if (coverage.missing_var != nullptr) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("BioseqBatchJoin output variable is absent from scan tuple"),
                         errdetail("varno=%u attno=%d type=%u typmod=%d scan_columns=%d",
                                   coverage.missing_var->varno,
                                   coverage.missing_var->varattno,
                                   coverage.missing_var->vartype,
                                   coverage.missing_var->vartypmod,
                                   list_length(scan_tlist))));
            }
        }
    }

    /* The same helper is commonly represented by a positive-id PHV in the
     * processed target and by its stripped FuncExpr in a non-projecting locator
     * child.  Preserve the authoritative PHV id in the site codec when present;
     * finalized binding then accepts only that exact PHV-to-bare transition. */
    [[nodiscard]] bool batch_join_plan_only_helper_phid(List* authoritative_identities,
                                                        Node* carrier_identity,
                                                        FuncExpr* carrier_helper,
                                                        const bioseq_stub_oids& oids,
                                                        Index& phid) noexcept {
        if (carrier_identity == nullptr || carrier_helper == nullptr)
            return false;
        bool matched = false;
        Index positive_phid = 0;
        for (Node* identity : postgres_list<Node>(authoritative_identities)) {
            FuncExpr* helper = per_row_helper_identity_function(identity, oids);
            if (helper == nullptr || !equal(helper, carrier_helper))
                continue;
            matched = true;
            PlaceHolderVar* placeholder = try_cast_pg_node<PlaceHolderVar>(identity);
            if (placeholder == nullptr)
                continue;
            if (placeholder->phid == 0 ||
                (positive_phid != 0 && positive_phid != placeholder->phid)) {
                return false;
            }
            positive_phid = placeholder->phid;
        }
        PlaceHolderVar* carrier = try_cast_pg_node<PlaceHolderVar>(carrier_identity);
        if (positive_phid != 0) {
            phid = positive_phid;
            return true;
        }
        if (carrier != nullptr) {
            if (carrier->phid == 0)
                return false;
            phid = carrier->phid;
            return true;
        }
        phid = 0;
        return matched || equal(carrier_identity, carrier_helper);
    }

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
            alphabet_code < 0 || alphabet_code > UINT8_MAX ||
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
