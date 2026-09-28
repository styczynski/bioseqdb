/**
 * @file
 * @brief Finds which finalized scans supply match-helper results.
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

export module bioseq.postgres.table:custom_scan_match_analysis_semantic_analysis;

import std;
import bioseq.postgres.support;

import :custom_scan_plan_abi_constants;
import :custom_scan_runtime_match_contracts;
import :custom_scan_oid_expression_support_oid_catalog_contracts;
import :custom_scan_planner_method_contracts;
import :custom_scan_match_finalization_contracts;
import :custom_scan_join_ownership_helper_contracts;
import :custom_scan_match_analysis_carrier_validation_contracts;
export import :custom_scan_match_analysis_semantic_analysis_contracts;

using namespace bioseq::postgres::support;

export {
    /** @brief Holds a base scan's checked target expression, source columns, and saved helper bindings. */
    struct finalized_base_owner_site_context {
        List* sites = NIL;
        Node* target_recipe = nullptr;
        List* target_lineage = NIL;
        Index target_relid = 0;
        int stable_match_id = 0;
    };

    /** @brief Loads the saved helper bindings of a base scan with a valid match target. */
    [[nodiscard]] bool
    try_resolve_finalized_base_owner_site_context(CustomScan* scan,
                                                  const bioseq_stub_oids& oids,
                                                  finalized_base_owner_site_context& context) {
        context = {};
        if (scan == nullptr || scan->methods != bioseq_column_scan_scan_methods()) {
            return false;
        }
        List* owner_tag = find_base_match_owner_plan_tag(scan);
        Node* descriptor_node =
            owner_tag == NIL ? nullptr
                             : reinterpret_cast<Node*>(
                                   list_nth(owner_tag, k_base_match_plan_executor_descriptor));
        Node* sites_node =
            owner_tag == NIL
                ? nullptr
                : reinterpret_cast<Node*>(list_nth(owner_tag, k_base_match_plan_executor_sites));
        if (descriptor_node == nullptr || !IsA(descriptor_node, List) || sites_node == nullptr ||
            !IsA(sites_node, List)) {
            return false;
        }
        List* descriptor = reinterpret_cast<List*>(descriptor_node);
        const int stable_match_id =
            validate_finalized_match_owner_descriptor_value(scan, descriptor);
        Node* target_recipe =
            reinterpret_cast<Node*>(list_nth(descriptor, k_match_owner_target_recipe));
        List* target_lineage = canonical_match_base_lineage(target_recipe, oids);
        bool valid_lineage = target_lineage != NIL;
        for (Var* source : postgres_list<Var>(target_lineage)) {
            if (source == nullptr || source->varno != scan->scan.scanrelid ||
                source->varattno <= 0 || source->varlevelsup != 0 ||
                !bms_is_empty(source->varnullingrels) ||
                source->vartype != oids.bioseq_seq_type_oid) {
                valid_lineage = false;
                break;
            }
        }
        if (target_recipe == nullptr || exprType(target_recipe) != oids.bioseq_seq_type_oid ||
            !valid_lineage) {
            return false;
        }
        context = finalized_base_owner_site_context{reinterpret_cast<List*>(sites_node),
                                                    target_recipe,
                                                    target_lineage,
                                                    scan->scan.scanrelid,
                                                    stable_match_id};
        return true;
    }

    /**
     * @brief Checks that a saved binding matches this helper and output column.
     *
     * Require the same function and argument, compatible result metadata, and this
     * scan's match ID. The argument must identify the target expression or a
     * permitted  source column. Set malformed when the saved record has the wrong
     * shape or field types.
     */
    [[nodiscard]] bool
    finalized_base_owner_site_matches(List* site,
                                      FuncExpr* helper,
                                      AttrNumber resno,
                                      const finalized_base_owner_site_context& context,
                                      const bioseq_stub_oids& oids,
                                      bool& malformed) noexcept {
        if (site == nullptr || helper == nullptr ||
            list_length(site) != k_bioseq_match_helper_site_fields) {
            malformed = true;
            return false;
        }
        for (const int index : {k_match_site_resno,
                                k_match_site_funcid,
                                k_match_site_typmod,
                                k_match_site_collation,
                                k_match_site_binding_kind,
                                k_match_site_phid,
                                k_match_site_stable_id}) {
            Node* value = reinterpret_cast<Node*>(list_nth(site, index));
            if (value == nullptr || !IsA(value, Integer)) {
                malformed = true;
                return false;
            }
        }
        Node* canonical_argument =
            reinterpret_cast<Node*>(list_nth(site, k_match_site_canonical_argument));
        Node* helper_argument = batch_join_match_helper_argument(helper);
        Var* argument = try_cast_pg_node<Var>(canonical_argument);
        const auto binding_kind = static_cast<match_helper_binding_kind>(
            intVal(list_nth(site, k_match_site_binding_kind)));
        const bool exact_sequence_argument =
            canonical_argument != nullptr &&
            exprType(canonical_argument) == oids.bioseq_seq_type_oid &&
            equal(canonical_argument, context.target_recipe) &&
            binding_kind == match_helper_binding_kind::exact_recipe;
        bool base_lineage_argument = argument != nullptr &&
                                     argument->varno == context.target_relid &&
                                     argument->varattno > 0 && argument->varlevelsup == 0 &&
                                     bms_is_empty(argument->varnullingrels) &&
                                     binding_kind == match_helper_binding_kind::unique_lineage;
        if (base_lineage_argument && argument->vartype == oids.bioseq_seq_type_oid) {
            base_lineage_argument = false;
            for (Var* source : postgres_list<Var>(context.target_lineage)) {
                if (equal(source, argument)) {
                    base_lineage_argument = true;
                    break;
                }
            }
        }
        return helper_argument != nullptr && canonical_argument != nullptr &&
               equal(canonical_argument, helper_argument) &&
               intVal(list_nth(site, k_match_site_resno)) == resno &&
               static_cast<Oid>(intVal(list_nth(site, k_match_site_funcid))) == helper->funcid &&
               intVal(list_nth(site, k_match_site_typmod)) ==
                   exprTypmod(reinterpret_cast<Node*>(helper)) &&
               static_cast<Oid>(intVal(list_nth(site, k_match_site_collation))) ==
                   exprCollation(reinterpret_cast<Node*>(helper)) &&
               intVal(list_nth(site, k_match_site_phid)) == 0 &&
               intVal(list_nth(site, k_match_site_stable_id)) == context.stable_match_id &&
               (exact_sequence_argument || base_lineage_argument);
    }

    /** @brief Tracks the saved bindings found while checking the base scan's output expressions. */
    struct finalized_base_targetlist_audit {
        finalized_base_owner_site_context context{};
        const bioseq_stub_oids* oids = nullptr;
        AttrNumber resno = InvalidAttrNumber;
        Bitmapset** matched_sites = nullptr;
        bool invalid = false;
    };

    /** @brief Checks that each helper call has exactly one saved binding for its outout column. */
    struct finalized_base_targetlist_audit_visitor {
        /** @brief Finds a helper's saved binding and records it, stopping on invalid or ambiguous data */
        [[nodiscard]] pg_walk_step operator()(pg_expression_walk_cursor,
                                              pg_node_view node,
                                              finalized_base_targetlist_audit& audit) const {
            FuncExpr* function = node.try_cast<FuncExpr>();
            if (function == nullptr || audit.oids == nullptr ||
                !is_bioseq_projection_funcid(function->funcid, *audit.oids)) {
                return pg_walk_step::descend;
            }
            FuncExpr* helper =
                batch_join_well_formed_match_helper(reinterpret_cast<Node*>(function), *audit.oids);
            if (helper == nullptr || audit.matched_sites == nullptr) {
                audit.invalid = true;
                return pg_walk_step::stop;
            }
            std::size_t matches = 0u;
            std::size_t matched_index = 0u;
            for (int index = 0; index < list_length(audit.context.sites); ++index) {
                Node* raw_site = reinterpret_cast<Node*>(list_nth(audit.context.sites, index));
                List* site = raw_site != nullptr && IsA(raw_site, List)
                                 ? reinterpret_cast<List*>(raw_site)
                                 : NIL;
                bool malformed = false;
                if (finalized_base_owner_site_matches(
                        site, helper, audit.resno, audit.context, *audit.oids, malformed)) {
                    ++matches;
                    matched_index = static_cast<std::size_t>(index);
                }
                if (malformed) {
                    audit.invalid = true;
                    return pg_walk_step::stop;
                }
            }
            if (matches != 1u) {
                audit.invalid = true;
                return pg_walk_step::stop;
            }
            *audit.matched_sites =
                bms_add_member(*audit.matched_sites, static_cast<int>(matched_index));
            return pg_walk_step::skip_subtree;
        }
    };

    /**
     * @brief Checks every base-scan helper against its saved binding.
     *
     * Each helper call must match one record for its output column. Also require
     * every  saved, record to appear in the target list, so stale bindings cannot
     * survive unnoticed.
     */
    [[nodiscard]] bool
    base_scan_targetlist_has_only_finalized_match_helpers(CustomScan* scan,
                                                          const bioseq_stub_oids& oids) {
        finalized_base_owner_site_context context{};
        if (!try_resolve_finalized_base_owner_site_context(scan, oids, context) ||
            context.sites == NIL)
            return false;
        Bitmapset* matched_sites = nullptr;
        finalized_base_targetlist_audit audit{
            context, &oids, InvalidAttrNumber, &matched_sites, false};
        finalized_base_targetlist_audit_visitor visitor;
        for (TargetEntry* entry : postgres_list<TargetEntry>(scan->scan.plan.targetlist)) {
            if (entry == nullptr || entry->expr == nullptr)
                continue;
            audit.resno = entry->resno;
            walk_pg_expression_including_root(reinterpret_cast<Node*>(entry->expr), audit, visitor);
            if (audit.invalid) {
                bms_free(matched_sites);
                return false;
            }
        }
        const bool all_sites_matched = bms_num_members(matched_sites) == list_length(context.sites);
        bms_free(matched_sites);
        return all_sites_matched;
    }

    /**
     * @brief Checks whether this base scan supplies a helper result.
     *
     * Return 1 when the scan owns a matching helper, even if several output columns
     * use it. Return 0 for no match and 2 for malformed saved site data.
     */
    [[nodiscard]] std::size_t finalized_base_owner_matching_site_count(
        CustomScan* scan, Node* identity, const bioseq_stub_oids& oids) {
        finalized_base_owner_site_context context{};
        if (!try_resolve_finalized_base_owner_site_context(scan, oids, context) ||
            identity == nullptr)
            return 0u;
        PlaceHolderVar* placeholder = try_cast_pg_node<PlaceHolderVar>(identity);
        FuncExpr* helper = placeholder == nullptr
                               ? batch_join_well_formed_match_helper(identity, oids)
                               : batch_join_match_helper_phv_function(placeholder, oids);
        if (helper == nullptr)
            return 0u;
        std::size_t matches = 0u;
        for (Node* raw_site : postgres_list<Node>(context.sites)) {
            List* site = raw_site != nullptr && IsA(raw_site, List)
                             ? reinterpret_cast<List*>(raw_site)
                             : NIL;
            if (list_length(site) != k_bioseq_match_helper_site_fields)
                return 2u;
            const int resno =
                site != NIL &&
                        IsA(reinterpret_cast<Node*>(list_nth(site, k_match_site_resno)), Integer)
                    ? intVal(list_nth(site, k_match_site_resno))
                    : -1;
            Node* raw_supplied =
                resno <= 0 || resno > list_length(scan->scan.plan.targetlist)
                    ? nullptr
                    : reinterpret_cast<Node*>(list_nth(scan->scan.plan.targetlist, resno - 1));
            TargetEntry* supplied = raw_supplied != nullptr && IsA(raw_supplied, TargetEntry)
                                        ? reinterpret_cast<TargetEntry*>(raw_supplied)
                                        : nullptr;
            bool malformed = false;
            if (supplied != nullptr && supplied->resno == resno &&
                equal(supplied->expr, identity) &&
                finalized_base_owner_site_matches(
                    site, helper, static_cast<AttrNumber>(resno), context, oids, malformed)) {
                /* setrefs can make a PlaceHolderVar and a bare helper equal in two
                 * output columns. Both still belong to this one base scan. The
                 * target-list audit checks their separate  output positions. */
                matches = 1u;
            }
            if (malformed)
                return 2u;
        }
        return matches;
    }

    /**
     * @brief Counts matching helper owners in this plan subtree.
     *
     * Stop at subquery boundaries. Counts stop at 2 because callers only need to
     * know whether a helper has one unambiguous source.
     */
    [[nodiscard]] std::size_t finalized_descendant_match_owner_site_count(
        Plan* plan, Node* identity, const bioseq_stub_oids& oids) {
        if (plan == nullptr || identity == nullptr)
            return 0u;
        check_stack_depth();
        // Keep 2 as the result oce ownership is ambiguous.
        auto add = [](std::size_t left, std::size_t right) noexcept {
            return left > 1u || right > 1u || left + right > 1u ? std::size_t{2u} : left + right;
        };
        if (IsA(plan, CustomScan)) {
            auto* custom = reinterpret_cast<CustomScan*>(plan);
            std::size_t count = finalized_batch_owner_matching_site_count(custom, identity, oids);
            count = add(count, finalized_base_owner_matching_site_count(custom, identity, oids));
            for (Plan* child : postgres_list<Plan>(custom->custom_plans)) {
                count =
                    add(count, finalized_descendant_match_owner_site_count(child, identity, oids));
            }
            return count;
        }
        // Counts matching owners across one PostgreSQL plan-child list.
        auto search_plan_list = [identity, &oids, &add](List* plans) {
            std::size_t count = 0u;
            for (Plan* child : postgres_list<Plan>(plans)) {
                count =
                    add(count, finalized_descendant_match_owner_site_count(child, identity, oids));
            }
            return count;
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
            /* Helpers inside this subquery have separate bindings. */
            return 0u;
        }
        std::size_t count =
            finalized_descendant_match_owner_site_count(outerPlan(plan), identity, oids);
        if (innerPlan(plan) != outerPlan(plan)) {
            count =
                add(count,
                    finalized_descendant_match_owner_site_count(innerPlan(plan), identity, oids));
        }
        return count;
    }

    /**
     * @brief Counts helper owners of one kind for failure diagnostics.
     *
     * batch_owner selects join owners. Otherwise, count base scans. Keep the same
     * subquery boundaries as the ownership check and stop each count at 2 so  the
     * diagnostic distinguishes absent, unique, and ambiguous ownership
     */
    [[nodiscard]] std::size_t finalized_descendant_match_owner_site_count_by_kind(
        Plan* plan, Node* identity, const bioseq_stub_oids& oids, bool batch_owner) {
        if (plan == nullptr || identity == nullptr)
            return 0u;
        check_stack_depth();
        // Keep 2 as the result once ownership is ambiguous.
        auto add = [](std::size_t left, std::size_t right) noexcept {
            return left > 1u || right > 1u || left + right > 1u ? std::size_t{2u} : left + right;
        };
        if (IsA(plan, CustomScan)) {
            auto* custom = reinterpret_cast<CustomScan*>(plan);
            std::size_t count =
                batch_owner ? finalized_batch_owner_matching_site_count(custom, identity, oids)
                            : finalized_base_owner_matching_site_count(custom, identity, oids);
            for (Plan* child : postgres_list<Plan>(custom->custom_plans)) {
                count = add(count,
                            finalized_descendant_match_owner_site_count_by_kind(
                                child, identity, oids, batch_owner));
            }
            return count;
        }
        // Counts one owner class across a PostgreSQL plan-child list.
        auto search_plan_list = [identity, &oids, batch_owner, &add](List* plans) {
            std::size_t count = 0u;
            for (Plan* child : postgres_list<Plan>(plans)) {
                count = add(count,
                            finalized_descendant_match_owner_site_count_by_kind(
                                child, identity, oids, batch_owner));
            }
            return count;
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
            /* Use the same subquery boundary as the ownership check. */
            return 0u;
        }
        std::size_t count = finalized_descendant_match_owner_site_count_by_kind(
            outerPlan(plan), identity, oids, batch_owner);
        if (innerPlan(plan) != outerPlan(plan)) {
            count = add(count,
                        finalized_descendant_match_owner_site_count_by_kind(
                            innerPlan(plan), identity, oids, batch_owner));
        }
        return count;
    }

    /** @brief Counts join and base-scan helper owners in the child subtrees for diagnostics. */
    [[nodiscard]] finalized_match_owner_site_counts finalized_child_match_owner_site_counts(
        Plan* plan, Node* identity, const bioseq_stub_oids& oids) {
        finalized_match_owner_site_counts counts{};
        if (plan == nullptr || identity == nullptr)
            return counts;
        // Keep 2 as the result once ownership is ambiguous.
        auto add = [](std::size_t left, std::size_t right) noexcept {
            return left > 1u || right > 1u || left + right > 1u ? std::size_t{2u} : left + right;
        };
        // Merges both owner classes from one child subtree.
        auto count_child = [&counts, &add, identity, &oids](Plan* child) {
            counts.batch = add(
                counts.batch,
                finalized_descendant_match_owner_site_count_by_kind(child, identity, oids, true));
            counts.base = add(
                counts.base,
                finalized_descendant_match_owner_site_count_by_kind(child, identity, oids, false));
        };
        if (IsA(plan, Append)) {
            for (Plan* child : postgres_list<Plan>(reinterpret_cast<Append*>(plan)->appendplans)) {
                count_child(child);
            }
        } else if (IsA(plan, MergeAppend)) {
            for (Plan* child :
                 postgres_list<Plan>(reinterpret_cast<MergeAppend*>(plan)->mergeplans)) {
                count_child(child);
            }
        } else if (IsA(plan, CustomScan)) {
            for (Plan* child :
                 postgres_list<Plan>(reinterpret_cast<CustomScan*>(plan)->custom_plans)) {
                count_child(child);
            }
        } else if (IsA(plan, BitmapAnd)) {
            for (Plan* child :
                 postgres_list<Plan>(reinterpret_cast<BitmapAnd*>(plan)->bitmapplans)) {
                count_child(child);
            }
        } else if (IsA(plan, BitmapOr)) {
            for (Plan* child :
                 postgres_list<Plan>(reinterpret_cast<BitmapOr*>(plan)->bitmapplans)) {
                count_child(child);
            }
        } else if (!IsA(plan, SubqueryScan)) {
            count_child(outerPlan(plan));
            if (innerPlan(plan) != outerPlan(plan))
                count_child(innerPlan(plan));
        }
        return counts;
    }

    /**
     * @brief Finds the saved match-owner record for an ancestor join.
     *
     * Return null when the scan has no join-owner record. Reject duplicates or a
     * record that disagrees with  the scan's private, data, plan-node ID, or query
     * scope. Both batch and per-row joins use this check.
     */
    [[nodiscard]] match_owner_finalization*
    finalized_batch_owner_for_ancestor_stack(CustomScan* scan,
                                             std::span<match_owner_finalization> owners) {
        match_owner_finalization* result = nullptr;
        for (match_owner_finalization& owner : owners) {
            if (owner.scan != scan || owner.base_owner_tag != NIL)
                continue;
            if (result != nullptr) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("Bioseq finalized ancestor match owner is duplicated"),
                         errdetail("plan_node_id=%d", scan->scan.plan.plan_node_id)));
            }
            result = &owner;
        }
        if (result == nullptr)
            return nullptr;

        Node* version_node = scan->custom_private == NIL
                                 ? nullptr
                                 : reinterpret_cast<Node*>(linitial(scan->custom_private));
        const int version =
            version_node != nullptr && IsA(version_node, Integer) ? intVal(version_node) : -1;
        const int expected_fields = version == k_bioseq_batch_join_plan_version
                                        ? k_bioseq_batch_join_private_fields
                                        : (version == k_bioseq_per_row_join_plan_version
                                               ? k_bioseq_per_row_join_private_fields
                                               : -1);
        const int descriptor_index =
            version == k_bioseq_batch_join_plan_version
                ? k_batch_join_match_owner_descriptor
                : (version == k_bioseq_per_row_join_plan_version ? k_per_row_match_owner_descriptor
                                                                 : -1);
        const int sites_index =
            version == k_bioseq_batch_join_plan_version
                ? k_batch_join_placeholder_sites
                : (version == k_bioseq_per_row_join_plan_version ? k_per_row_placeholder_sites
                                                                 : -1);
        Node* descriptor_node =
            descriptor_index >= 0 && descriptor_index < list_length(scan->custom_private)
                ? reinterpret_cast<Node*>(list_nth(scan->custom_private, descriptor_index))
                : nullptr;
        Node* sites_node =
            sites_index >= 0 && sites_index < list_length(scan->custom_private)
                ? reinterpret_cast<Node*>(list_nth(scan->custom_private, sites_index))
                : nullptr;
        const bool sites_agree = result->sites == NIL
                                     ? sites_node == nullptr
                                     : sites_node == reinterpret_cast<Node*>(result->sites);
        if (scan->methods != bioseq_batch_join_scan_methods() || expected_fields < 0 ||
            list_length(scan->custom_private) != expected_fields || descriptor_node == nullptr ||
            !IsA(descriptor_node, List) ||
            descriptor_node != reinterpret_cast<Node*>(result->descriptor) || !sites_agree ||
            result->stable_match_id < 0 ||
            result->stable_match_id != scan->scan.plan.plan_node_id ||
            result->query_scope_id == 0u) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("Bioseq finalized ancestor match owner is invalid"),
                     errdetail("plan_node_id=%d version=%d query_scope_id=" UINT64_FORMAT,
                               scan->scan.plan.plan_node_id,
                               version,
                               static_cast<uint64>(result->query_scope_id))));
        }
        const int descriptor_stable_id = validate_finalized_match_owner_descriptor_value(
            scan, reinterpret_cast<List*>(descriptor_node));
        if (descriptor_stable_id != result->stable_match_id) {
            ereport(
                ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("Bioseq finalized ancestor match identity disagrees with its descriptor"),
                 errdetail("plan_node_id=%d descriptor_stable_id=%d owner_stable_id=%d",
                           scan->scan.plan.plan_node_id,
                           descriptor_stable_id,
                           result->stable_match_id)));
        }
        return result;
    }

} // export custom-scan partition declarations
