/**
 * @file
 * @brief Registers join placeholders and inherited helper identities.
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

export module bioseq.postgres.table:custom_scan_join_plan_placeholder_lineage;

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

} // export custom-scan partition declarations
