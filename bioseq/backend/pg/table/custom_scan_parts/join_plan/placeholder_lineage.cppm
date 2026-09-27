/**
 * @file
 * @brief Assigns output columns to match helpers and recognizes results supplied by child scans.
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

export module bioseq.postgres.table:custom_scan_join_plan_placeholder_lineage;

import std;
import bioseq.postgres.support;
import :custom_scan_support;
import :custom_scan_join_ownership_helper_contracts;
export import :custom_scan_join_plan_placeholder_registration_contracts;

using namespace bioseq::postgres::support;

export {

    /** @brief Reports whether an expression contains any match-result helper. */
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

    /**
     * @brief Finds the existing output column for this helper expression and PlaceHolderVar id.
     *
     * The same helper can appear in several outputs or hidden resjunk columns.
     * Reuse its match_result slot when both the expression and phid agree. Keep
     * a bare FuncExpr distinct from its PlaceHolderVar wrapper.
     */
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
            // Decodes one required Integer field from the serialized site record.
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

    /** @brief Registers an output column for this match helper, reusing an existing matching site. */
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
        /* PostgreSQL fixes parent references before visiting this child.
         * Keep the original helper or PlaceHolderVar so the parent can match
         * it to a child output. Later setrefs maps it to INDEX_VAR, and
         * BioseqBatchJoin fills that column from match_result. Each distinct
         * expression or phid gets its own column, even when result types match. */
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

    /**
     * @brief Uses an existing scan output column for this match helper.
     *
     * The target child may already carry the whole helper as a planning
     * placeholder, while an upper Result will consume it. Bind that column to
     * match_result so setrefs has one result to reference and the placeholder
     * has an executor owner.
     */
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

    /** @brief Checks a list for a structurally equal helper identity. */
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

    /** @brief Checks whether a child plan already supplies this match helper. */
    [[nodiscard]] bool
    batch_join_identity_is_inherited(batch_join_placeholder_registration const& registration,
                                     Node* identity) noexcept {
        return batch_join_identity_list_contains(registration.inherited_helper_identities,
                                                 identity);
    }

    /** @brief Finds the BioseqScan match-owner tag, rejecting malformed or duplicate tags. */
    [[nodiscard]] List* find_base_match_owner_plan_tag(CustomScan* scan);

    /**
     * @brief Checks whether a custom scan has registered an output for this helper expression.
     *
     * This runs during PlanCustomPath, before the planner hook binds final
     * plan_node_id values. Use the saved helper sites and output expressions
     * to check ownership at this stage.
     */
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

    /** @brief Checks this plan and its children for a scan that supplies the helper. */
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
        // Searches one PostgreSQL plan-child list for the same helper owner.
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
            /* A SubqueryScan has its own helper bindings. An equal expression
             * inside that subquery cannot identify an owner in this scope. */
            return false;
        }
        return batch_join_descendant_owns_helper_identity(outerPlan(plan), identity) ||
               (innerPlan(plan) != outerPlan(plan) &&
                batch_join_descendant_owns_helper_identity(innerPlan(plan), identity));
    }

    /** @brief Collects helpers in the child output whose producing scan can be found and checked. */
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

} // export custom-scan partition declarations
