/**
 * @file
 * @brief Finds helper results available below a join and checks where paths may use them.
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

export module bioseq.postgres.table:custom_scan_per_row_join_paths_carrier_analysis;

import std;
import bioseq.postgres.support;

import :custom_scan_oid_expression_support_oid_catalog;
import :custom_scan_planner_method_contracts;
import :custom_scan_join_ownership_helper_contracts;
import :custom_scan_join_ownership_carrier_lineage_contracts;
import :custom_scan_join_ownership_descendant_path_contracts;
import :custom_scan_join_ownership_path_owner_contracts;
import :custom_scan_join_ownership_target_contracts;
export import :custom_scan_per_row_join_paths_carrier_contracts;

using namespace bioseq::postgres::support;

export {

    /**
     * @brief Counts base scans in this path tree that can supply a match helper.
     *
     * Match the helper against each scan's target expression or source columns.
     * Record invalid owner data and unsupported path wrappers so callers can
     * reject an uncertain match.
     */
    void per_row_analyze_descendant_base_helper_owner(Path* path,
                                                      FuncExpr* helper,
                                                      per_row_base_helper_ownership& ownership) {
        if (path == nullptr || helper == nullptr || ownership.invalid_owner_abi ||
            ownership.unsupported_wrapper)
            return;
        check_stack_depth();
        if (IsA(path, CustomPath)) {
            auto* custom = reinterpret_cast<CustomPath*>(path);
            if (custom->methods == bioseq_column_scan_path_methods()) {
                Node* recipe = per_row_base_scan_owner_recipe(custom, ownership.invalid_owner_abi);
                if (ownership.invalid_owner_abi)
                    return;
                if (recipe != nullptr && (batch_join_match_helper_names_recipe(helper, recipe) ||
                                          batch_join_match_helper_names_base_lineage(
                                              helper, recipe, get_cached_stub_oids()))) {
                    ++ownership.matches;
                    ownership.owner = custom;
                }
            } else if (custom->methods != bioseq_batch_join_path_methods() &&
                       custom->methods != bioseq_per_row_join_path_methods() &&
                       custom->methods != &bioseq_slice_project_path_methods()) {
                ownership.unsupported_wrapper = true;
                return;
            }
            for (Path* child : postgres_list<Path>(custom->custom_paths)) {
                per_row_analyze_descendant_base_helper_owner(child, helper, ownership);
            }
            return;
        }
        // Visit both inputs of a supported binary join wrapper.
        auto descend_join = [helper, &ownership](JoinPath* join) {
            if (join == nullptr)
                return;
            per_row_analyze_descendant_base_helper_owner(join->outerjoinpath, helper, ownership);
            per_row_analyze_descendant_base_helper_owner(join->innerjoinpath, helper, ownership);
        };
        if (IsA(path, NestPath)) {
            descend_join(&reinterpret_cast<NestPath*>(path)->jpath);
            return;
        }
        if (IsA(path, MergePath)) {
            descend_join(&reinterpret_cast<MergePath*>(path)->jpath);
            return;
        }
        if (IsA(path, HashPath)) {
            descend_join(&reinterpret_cast<HashPath*>(path)->jpath);
            return;
        }
        Path* unary = nullptr;
        if (IsA(path, ProjectionPath)) {
            unary = reinterpret_cast<ProjectionPath*>(path)->subpath;
        } else if (IsA(path, ProjectSetPath)) {
            unary = reinterpret_cast<ProjectSetPath*>(path)->subpath;
        } else if (IsA(path, SortPath)) {
            unary = reinterpret_cast<SortPath*>(path)->subpath;
        } else if (IsA(path, IncrementalSortPath)) {
            unary = reinterpret_cast<IncrementalSortPath*>(path)->spath.subpath;
        } else if (IsA(path, MaterialPath)) {
            unary = reinterpret_cast<MaterialPath*>(path)->subpath;
        } else if (IsA(path, MemoizePath)) {
            unary = reinterpret_cast<MemoizePath*>(path)->subpath;
        } else if (IsA(path, UniquePath)) {
            unary = reinterpret_cast<UniquePath*>(path)->subpath;
        } else if (IsA(path, GatherPath)) {
            unary = reinterpret_cast<GatherPath*>(path)->subpath;
        } else if (IsA(path, GatherMergePath)) {
            unary = reinterpret_cast<GatherMergePath*>(path)->subpath;
        }
        if (unary != nullptr) {
            per_row_analyze_descendant_base_helper_owner(unary, helper, ownership);
            return;
        }
        List* children = NIL;
        if (IsA(path, AppendPath)) {
            children = reinterpret_cast<AppendPath*>(path)->subpaths;
        } else if (IsA(path, MergeAppendPath)) {
            children = reinterpret_cast<MergeAppendPath*>(path)->subpaths;
        }
        if (children != NIL) {
            for (Path* child : postgres_list<Path>(children)) {
                per_row_analyze_descendant_base_helper_owner(child, helper, ownership);
            }
            return;
        }
        if (path->parent == nullptr || (path->parent->reloptkind != RELOPT_BASEREL &&
                                        path->parent->reloptkind != RELOPT_OTHER_MEMBER_REL)) {
            ownership.unsupported_wrapper = true;
        }
    }

    /**
     * @brief Collects candidate helpers that this path can obtain from one descendant scan.
     *
     * Check both inherited join owners and base scans. An owner must expose the
     * complete helper expression in its output. Skip candidates with no supplier
     * and reject ambiguous ownership or unsupported path wrappers.
     */
    [[nodiscard]] bool per_row_collect_available_path_helpers(Path* path,
                                                              List* candidate_helpers,
                                                              List* validated_inherited_helpers,
                                                              List*& available) {
        available = NIL;
        if (path == nullptr)
            return false;
        bioseq_stub_oids const& oids = get_cached_stub_oids();
        for (Node* identity : postgres_list<Node>(candidate_helpers)) {
            FuncExpr* helper = per_row_helper_identity_function(identity, oids);
            Node* argument = batch_join_match_helper_argument(helper);
            if (helper == nullptr || argument == nullptr)
                return false;
            bool supplied = false;
            std::size_t inherited_owner_matches = 0u;
            if (per_row_expression_list_contains(validated_inherited_helpers, identity)) {
                per_row_descendant_ownership ownership{};
                per_row_analyze_descendant_exact_owner(path, argument, ownership);
                if (ownership.invalid_owner_abi || ownership.unsupported_wrapper ||
                    ownership.matches > 1u)
                    return false;
                if (ownership.matches == 1u) {
                    if (ownership.owner == nullptr || ownership.owner->path.pathtarget == nullptr ||
                        !path_target_contains_expression(ownership.owner->path.pathtarget,
                                                         reinterpret_cast<Expr*>(identity)))
                        return false;
                    supplied = true;
                }
                inherited_owner_matches = ownership.matches;
            }
            per_row_base_helper_ownership base_ownership{};
            per_row_analyze_descendant_base_helper_owner(path, helper, base_ownership);
            if (base_ownership.invalid_owner_abi || base_ownership.unsupported_wrapper ||
                base_ownership.matches > 1u ||
                inherited_owner_matches + base_ownership.matches > 1u) {
                return false;
            }
            if (base_ownership.matches == 1u) {
                if (base_ownership.owner == nullptr ||
                    base_ownership.owner->path.pathtarget == nullptr ||
                    !path_target_contains_expression(base_ownership.owner->path.pathtarget,
                                                     reinterpret_cast<Expr*>(identity)))
                    return false;
                supplied = true;
            }
            if (supplied)
                available = lappend(available, identity);
        }
        return true;
    }

    /** @brief Tracks helper calls found in an expression and whether their results are available. */
    struct per_row_path_helper_audit {
        bioseq_stub_oids const* oids = nullptr;
        List* available = NIL;
        bool saw_helper = false;
        bool unavailable = false;
    };

    /** @brief Checks complete helper calls and PlaceHolderVars against the available expressions. */
    struct per_row_path_helper_audit_visitor {
        /** @brief Stops when a helper or its whole PlaceHolderVar is unavailable. */
        [[nodiscard]] pg_walk_step operator()(pg_expression_walk_cursor,
                                              pg_node_view node,
                                              per_row_path_helper_audit& audit) const {
            if (audit.oids == nullptr) {
                audit.unavailable = true;
                return pg_walk_step::stop;
            }
            if (PlaceHolderVar* placeholder = node.try_cast<PlaceHolderVar>()) {
                if (!batch_join_expression_has_match_helper(
                        reinterpret_cast<Node*>(placeholder->phexpr), *audit.oids))
                    return pg_walk_step::descend;
                audit.saw_helper = true;
                if (!per_row_expression_list_contains(audit.available,
                                                      reinterpret_cast<Node*>(placeholder))) {
                    audit.unavailable = true;
                    return pg_walk_step::stop;
                }
                return pg_walk_step::skip_subtree;
            }
            FuncExpr* function = node.try_cast<FuncExpr>();
            if (function == nullptr ||
                !is_bioseq_projection_funcid(function->funcid, *audit.oids)) {
                return pg_walk_step::descend;
            }
            audit.saw_helper = true;
            if (!per_row_expression_list_contains(audit.available,
                                                  reinterpret_cast<Node*>(function))) {
                audit.unavailable = true;
                return pg_walk_step::stop;
            }
            return pg_walk_step::skip_subtree;
        }
    };

    /** @brief Checks that an expression uses only available helper results and reports whether it uses any. */
    [[nodiscard]] bool
    per_row_expression_uses_available_helpers(Node* expression, List* available, bool& saw_helper) {
        per_row_path_helper_audit audit{&get_cached_stub_oids(), available, false, false};
        per_row_path_helper_audit_visitor visitor;
        walk_pg_expression_including_root(expression, audit, visitor);
        saw_helper = audit.saw_helper;
        return !audit.unavailable;
    }

    /**
     * @brief Checks that sorting expressions use only helpers available in this path.
     *
     * Inspect equivalent expressions whose relations fit within the path's
     * relation set. Skip members that require relations outside it.
     */
    [[nodiscard]] bool
    per_row_pathkeys_use_available_helpers(Path* scope, List* pathkeys, List* available) {
        if (pathkeys == NIL)
            return true;
        if (scope == nullptr || scope->parent == nullptr)
            return false;
        for (PathKey* pathkey : postgres_list<PathKey>(pathkeys)) {
            if (pathkey == nullptr || pathkey->pk_eclass == nullptr)
                return false;
            for (EquivalenceMember* member :
                 postgres_list<EquivalenceMember>(pathkey->pk_eclass->ec_members)) {
                if (member == nullptr || member->em_expr == nullptr)
                    return false;
                if (!bms_is_empty(member->em_relids) &&
                    !bms_is_subset(member->em_relids, scope->parent->relids))
                    continue;
                bool saw_helper = false;
                if (!per_row_expression_uses_available_helpers(
                        reinterpret_cast<Node*>(member->em_expr), available, saw_helper))
                    return false;
            }
        }
        return true;
    }

    /** @brief Checks that path metadata has supported node shapes and contains no match helpers. */
    [[nodiscard]] bool per_row_path_metadata_has_no_helper(Node* metadata);

    /** @brief Checks whether the expression walker supports this metadata node type. */
    [[nodiscard]] bool per_row_path_metadata_node_is_walkable(const NodeTag tag) noexcept {
        switch (tag) {
        case T_Var:
        case T_Const:
        case T_Param:
        case T_CaseTestExpr:
        case T_SQLValueFunction:
        case T_CoerceToDomainValue:
        case T_SetToDefault:
        case T_CurrentOfExpr:
        case T_NextValueExpr:
        case T_RangeTblRef:
        case T_SortGroupClause:
        case T_CTESearchClause:
        case T_MergeSupportFunc:
        case T_WithCheckOption:
        case T_Aggref:
        case T_GroupingFunc:
        case T_WindowFunc:
        case T_WindowFuncRunCondition:
        case T_SubscriptingRef:
        case T_FuncExpr:
        case T_NamedArgExpr:
        case T_OpExpr:
        case T_DistinctExpr:
        case T_NullIfExpr:
        case T_ScalarArrayOpExpr:
        case T_BoolExpr:
        case T_SubLink:
        case T_SubPlan:
        case T_AlternativeSubPlan:
        case T_FieldSelect:
        case T_FieldStore:
        case T_RelabelType:
        case T_CoerceViaIO:
        case T_ArrayCoerceExpr:
        case T_ConvertRowtypeExpr:
        case T_CollateExpr:
        case T_CaseExpr:
        case T_ArrayExpr:
        case T_RowExpr:
        case T_RowCompareExpr:
        case T_CoalesceExpr:
        case T_MinMaxExpr:
        case T_XmlExpr:
        case T_JsonValueExpr:
        case T_JsonConstructorExpr:
        case T_JsonIsPredicate:
        case T_JsonExpr:
        case T_JsonBehavior:
        case T_NullTest:
        case T_BooleanTest:
        case T_CoerceToDomain:
        case T_TargetEntry:
        case T_Query:
        case T_WindowClause:
        case T_CTECycleClause:
        case T_CommonTableExpr:
        case T_JsonKeyValue:
        case T_JsonObjectConstructor:
        case T_JsonArrayConstructor:
        case T_JsonArrayQueryConstructor:
        case T_JsonAggConstructor:
        case T_JsonObjectAgg:
        case T_JsonArrayAgg:
        case T_PartitionBoundSpec:
        case T_PartitionRangeDatum:
        case T_FromExpr:
        case T_OnConflictExpr:
        case T_MergeAction:
        case T_PartitionPruneStepOp:
        case T_PartitionPruneStepCombine:
        case T_JoinExpr:
        case T_SetOperationStmt:
        case T_IndexClause:
        case T_PlaceHolderVar:
        case T_InferenceElem:
        case T_AppendRelInfo:
        case T_PlaceHolderInfo:
        case T_RangeTblFunction:
        case T_TableSampleClause:
        case T_TableFunc:
            return true;
        default:
            return false;
        }
    }

    /** @brief Recognizes scalar metadata and scalar lists that cannot contain helper expressions. */
    [[nodiscard]] bool per_row_path_metadata_node_is_scalar(const NodeTag tag) noexcept {
        return tag == T_Integer || tag == T_Float || tag == T_Boolean || tag == T_String ||
               tag == T_BitString || tag == T_IntList || tag == T_OidList || tag == T_XidList;
    }

    /** @brief Records an unsupported node shape or match helper found in path metadata. */
    struct per_row_path_metadata_audit {
        const bioseq_stub_oids* oids = nullptr;
        bool invalid = false;
    };

    /** @brief Checks nested path metadata for match helpers and unsupported node types. */
    struct per_row_path_metadata_audit_visitor {
        /** @brief Visits supported metadata nodes and stops on a helper or unknown node type. */
        [[nodiscard]] pg_walk_step operator()(pg_expression_walk_cursor,
                                              pg_node_view node,
                                              per_row_path_metadata_audit& audit) const {
            if (node.get_tag() == T_List) {
                if (!per_row_path_metadata_has_no_helper(node.get())) {
                    audit.invalid = true;
                    return pg_walk_step::stop;
                }
                return pg_walk_step::skip_subtree;
            }
            if (RestrictInfo* restriction = node.try_cast<RestrictInfo>()) {
                if (restriction->clause == nullptr ||
                    !per_row_path_metadata_has_no_helper(
                        reinterpret_cast<Node*>(restriction->clause)) ||
                    !per_row_path_metadata_has_no_helper(
                        reinterpret_cast<Node*>(restriction->orclause))) {
                    audit.invalid = true;
                    return pg_walk_step::stop;
                }
                return pg_walk_step::skip_subtree;
            }
            if (per_row_path_metadata_node_is_scalar(node.get_tag())) {
                return pg_walk_step::skip_subtree;
            }
            if (!per_row_path_metadata_node_is_walkable(node.get_tag())) {
                audit.invalid = true;
                return pg_walk_step::stop;
            }
            if (FuncExpr* function = node.try_cast<FuncExpr>();
                function != nullptr && is_bioseq_projection_funcid(function->funcid, *audit.oids)) {
                audit.invalid = true;
                return pg_walk_step::stop;
            }
            return pg_walk_step::descend;
        }
    };

    /**
     * @brief Checks that path metadata contains no match helpers.
     *
     * Inspect lists and both expressions in RestrictInfo records. Scalar values
     * are safe. Reject unknown node types because the walker cannot prove that
     * they contain no helper calls.
     */
    [[nodiscard]] bool per_row_path_metadata_has_no_helper(Node* metadata) {
        if (metadata == nullptr)
            return true;
        check_stack_depth();
        if (nodeTag(metadata) == T_List) {
            for (Node* item : postgres_list<Node>(reinterpret_cast<List*>(metadata))) {
                if (!per_row_path_metadata_has_no_helper(item))
                    return false;
            }
            return true;
        }
        if (RestrictInfo* restriction = try_cast_pg_node<RestrictInfo>(metadata)) {
            return restriction->clause != nullptr &&
                   per_row_path_metadata_has_no_helper(
                       reinterpret_cast<Node*>(restriction->clause)) &&
                   per_row_path_metadata_has_no_helper(
                       reinterpret_cast<Node*>(restriction->orclause));
        }
        if (per_row_path_metadata_node_is_scalar(nodeTag(metadata)))
            return true;
        if (!per_row_path_metadata_node_is_walkable(nodeTag(metadata)))
            return false;
        per_row_path_metadata_audit audit{&get_cached_stub_oids(), false};
        per_row_path_metadata_audit_visitor visitor;
        walk_pg_expression_including_root(metadata, audit, visitor);
        return !audit.invalid;
    }

    /** @brief Checks that restriction clauses and their OR forms contain no helpers or unsupported nodes. */
    [[nodiscard]] bool per_row_restrictinfo_list_has_no_helper(List* restrictions) {
        for (Node* item : postgres_list<Node>(restrictions)) {
            RestrictInfo* restriction = try_cast_pg_node<RestrictInfo>(item);
            if (restriction == nullptr || restriction->clause == nullptr ||
                !per_row_path_metadata_has_no_helper(
                    reinterpret_cast<Node*>(restriction->clause)) ||
                !per_row_path_metadata_has_no_helper(
                    reinterpret_cast<Node*>(restriction->orclause)))
                return false;
        }
        return true;
    }

    /**
     * @brief Recovers sort/group references from PostgreSQL's processed target list.
     *
     * Match equal expressions in occurrence order, using each target entry once.
     * Reject a path that omits a duplicate needed to carry a nonzero sort/group
     * reference.
     */
    [[nodiscard]] bool per_row_authoritative_sortgrouprefs(PlannerInfo* root,
                                                           List* expressions,
                                                           std::vector<Index>& results) {
        results.clear();
        if (root == nullptr)
            return false;
        results.reserve(static_cast<std::size_t>(list_length(expressions)));
        List* consumed = NIL;
        for (Node* expression : postgres_list<Node>(expressions)) {
            if (expression == nullptr)
                return false;
            TargetEntry* selected = nullptr;
            for (TargetEntry* entry : postgres_list<TargetEntry>(root->processed_tlist)) {
                if (entry == nullptr || entry->expr == nullptr ||
                    list_member_ptr(consumed, entry) || !equal(entry->expr, expression))
                    continue;
                selected = entry;
                break;
            }
            results.push_back(selected == nullptr ? 0 : selected->ressortgroupref);
            if (selected != nullptr)
                consumed = lappend(consumed, selected);
        }
        /* Equal expressions can have separate sort/group references. Reject
         * this target if it lacks an occurrence needed for one of those refs. */
        for (TargetEntry* entry : postgres_list<TargetEntry>(root->processed_tlist)) {
            if (entry == nullptr || entry->expr == nullptr || entry->ressortgroupref == 0 ||
                list_member_ptr(consumed, entry))
                continue;
            if (per_row_expression_list_contains(expressions, reinterpret_cast<Node*>(entry->expr)))
                return false;
        }
        return results.size() == static_cast<std::size_t>(list_length(expressions));
    }

} // export custom-scan partition declarations
