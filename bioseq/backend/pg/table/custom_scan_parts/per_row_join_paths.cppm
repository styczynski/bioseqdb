/**
 * @file
 * @brief Builds transformed per-row mmsearch owners over reconstructed joins.
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

export module bioseq.postgres.table:custom_scan_per_row_join_paths;

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

import :custom_scan_join_ownership;

using namespace bioseq::postgres::support;

export {

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

    struct per_row_path_helper_audit {
        bioseq_stub_oids const* oids = nullptr;
        List* available = NIL;
        bool saw_helper = false;
        bool unavailable = false;
    };

    struct per_row_path_helper_audit_visitor {
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

    [[nodiscard]] bool
    per_row_expression_uses_available_helpers(Node* expression, List* available, bool& saw_helper) {
        per_row_path_helper_audit audit{&get_cached_stub_oids(), available, false, false};
        per_row_path_helper_audit_visitor visitor;
        walk_pg_expression_including_root(expression, audit, visitor);
        saw_helper = audit.saw_helper;
        return !audit.unavailable;
    }

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

    [[nodiscard]] bool per_row_path_metadata_has_no_helper(Node* metadata);

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

    [[nodiscard]] bool per_row_path_metadata_node_is_scalar(const NodeTag tag) noexcept {
        return tag == T_Integer || tag == T_Float || tag == T_Boolean || tag == T_String ||
               tag == T_BitString || tag == T_IntList || tag == T_OidList || tag == T_XidList;
    }

    struct per_row_path_metadata_audit {
        const bioseq_stub_oids* oids = nullptr;
        bool invalid = false;
    };

    struct per_row_path_metadata_audit_visitor {
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

    /* PathTarget does not record the allocated length of sortgrouprefs.  The
     * processed targetlist is the authoritative representation whose TLEs carry
     * their refs inline, so never index a possibly extended/shared PathTarget
     * array while rebuilding descendant targets.  Support-only expressions that
     * are absent from the processed targetlist correctly receive ref zero. */
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
        /* If this PathTarget names an expression group, it must contain enough
         * stable occurrences to carry every authoritative nonzero ref in that
         * group.  Otherwise choosing which duplicate owns the ref is ambiguous. */
        for (TargetEntry* entry : postgres_list<TargetEntry>(root->processed_tlist)) {
            if (entry == nullptr || entry->expr == nullptr || entry->ressortgroupref == 0 ||
                list_member_ptr(consumed, entry))
                continue;
            if (per_row_expression_list_contains(expressions, reinterpret_cast<Node*>(entry->expr)))
                return false;
        }
        return results.size() == static_cast<std::size_t>(list_length(expressions));
    }

    [[nodiscard]] PathTarget* per_row_normalized_path_target(PlannerInfo* root,
                                                             Path* path,
                                                             List* available,
                                                             List* candidate_helpers,
                                                             List* required_vars,
                                                             bool& changed,
                                                             bool& invalid) {
        changed = false;
        if (root == nullptr || path == nullptr || path->parent == nullptr ||
            path->parent->relids == nullptr || path->pathtarget == nullptr) {
            invalid = true;
            return nullptr;
        }
        List* expressions = NIL;
        std::vector<Index> sortgrouprefs;
        sortgrouprefs.reserve(static_cast<std::size_t>(list_length(path->pathtarget->exprs) +
                                                       list_length(required_vars)));
        std::vector<Index> source_sortgrouprefs;
        if (!per_row_authoritative_sortgrouprefs(
                root, path->pathtarget->exprs, source_sortgrouprefs)) {
            invalid = true;
            return nullptr;
        }
        std::size_t source_index = 0u;
        for (Node* expression : postgres_list<Node>(path->pathtarget->exprs)) {
            bool saw_helper = false;
            const bool supported =
                per_row_expression_uses_available_helpers(expression, available, saw_helper);
            const Index sortgroupref = source_sortgrouprefs[source_index++];
            if (!supported) {
                /* Pathkeys were audited against the actually available helpers
                 * before target normalization.  A current-owner helper may therefore
                 * be removed from this private child even when its public TLE carries
                 * a sortgroupref; the exact outer owner target retains that ref. */
                bool saw_candidate_helper = false;
                const bool covered_by_current_owner = per_row_expression_uses_available_helpers(
                    expression, candidate_helpers, saw_candidate_helper);
                if (!saw_helper || !saw_candidate_helper || !covered_by_current_owner) {
                    invalid = true;
                    return nullptr;
                }
                changed = true;
                continue;
            }
            expressions = lappend(expressions, copyObject(expression));
            sortgrouprefs.push_back(sortgroupref);
        }
        for (Node* expression : postgres_list<Node>(required_vars)) {
            Var* var = try_cast_pg_node<Var>(expression);
            const bool row_identity = per_row_is_exact_row_identity_var(var);
            if (var == nullptr || !bioseq_is_concrete_rte_varno(var->varno) ||
                (var->varattno <= 0 && !row_identity) || var->varlevelsup != 0 ||
                !bms_is_empty(var->varnullingrels)) {
                invalid = true;
                return nullptr;
            }
            if (!bms_is_member(static_cast<int>(var->varno), path->parent->relids) ||
                per_row_expression_list_contains(expressions, expression)) {
                continue;
            }
            expressions = lappend(expressions, copyObject(expression));
            sortgrouprefs.push_back(0);
            changed = true;
        }
        if (sortgrouprefs.size() != static_cast<std::size_t>(list_length(expressions))) {
            invalid = true;
            return nullptr;
        }
        PathTarget* target = create_empty_pathtarget();
        target->exprs = expressions;
        const bool has_sortgroupref = std::any_of(sortgrouprefs.begin(),
                                                  sortgrouprefs.end(),
                                                  [](Index value) noexcept { return value != 0; });
        if (has_sortgroupref) {
            target->sortgrouprefs =
                static_cast<Index*>(palloc0(sortgrouprefs.size() * sizeof(Index)));
            for (std::size_t index = 0u; index < sortgrouprefs.size(); ++index) {
                target->sortgrouprefs[index] = sortgrouprefs[index];
            }
        }
        set_pathtarget_cost_width(root, target);
        for (Node* expression : postgres_list<Node>(target->exprs)) {
            bool saw_helper = false;
            if (!per_row_expression_uses_available_helpers(expression, available, saw_helper)) {
                invalid = true;
                return nullptr;
            }
        }
        return target;
    }

    [[nodiscard]] bool per_row_append_targets_are_ordinally_compatible(PathTarget* parent,
                                                                       List* children) {
        if (parent == nullptr || children == NIL)
            return false;
        const int columns = list_length(parent->exprs);
        for (Path* child : postgres_list<Path>(children)) {
            if (child == nullptr || child->pathtarget == nullptr ||
                list_length(child->pathtarget->exprs) != columns)
                return false;
            for (int index = 0; index < columns; ++index) {
                Node* parent_expression = reinterpret_cast<Node*>(list_nth(parent->exprs, index));
                Node* child_expression =
                    reinterpret_cast<Node*>(list_nth(child->pathtarget->exprs, index));
                if (parent_expression == nullptr || child_expression == nullptr ||
                    exprType(parent_expression) != exprType(child_expression) ||
                    exprTypmod(parent_expression) != exprTypmod(child_expression) ||
                    exprCollation(parent_expression) != exprCollation(child_expression))
                    return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool
    per_row_authoritative_targets_match(PlannerInfo* root, PathTarget* left, PathTarget* right) {
        if (root == nullptr || left == nullptr || right == nullptr ||
            list_length(left->exprs) != list_length(right->exprs) ||
            !equal(left->exprs, right->exprs))
            return false;
        std::vector<Index> left_sortgrouprefs;
        std::vector<Index> right_sortgrouprefs;
        return per_row_authoritative_sortgrouprefs(root, left->exprs, left_sortgrouprefs) &&
               per_row_authoritative_sortgrouprefs(root, right->exprs, right_sortgrouprefs) &&
               left_sortgrouprefs == right_sortgrouprefs;
    }

    [[nodiscard]] bool per_row_normalize_private_path_spine(PlannerInfo* root,
                                                            Path* path,
                                                            List* candidate_helpers,
                                                            List* validated_inherited_helpers,
                                                            List* required_vars) {
        if (root == nullptr || path == nullptr)
            return false;
        check_stack_depth();
        List* available = NIL;
        if (!per_row_collect_available_path_helpers(
                path, candidate_helpers, validated_inherited_helpers, available) ||
            !per_row_pathkeys_use_available_helpers(path, path->pathkeys, available) ||
            (path->param_info != nullptr &&
             !per_row_restrictinfo_list_has_no_helper(path->param_info->ppi_clauses))) {
            return false;
        }
        bool target_changed = false;
        bool target_invalid = false;
        PathTarget* normalized_target = per_row_normalized_path_target(root,
                                                                       path,
                                                                       available,
                                                                       candidate_helpers,
                                                                       required_vars,
                                                                       target_changed,
                                                                       target_invalid);
        if (target_invalid || normalized_target == nullptr)
            return false;

        if (IsA(path, CustomPath)) {
            auto* custom = reinterpret_cast<CustomPath*>(path);
            if (custom->methods != bioseq_column_scan_path_methods() &&
                custom->methods != bioseq_batch_join_path_methods() &&
                custom->methods != bioseq_per_row_join_path_methods() &&
                custom->methods != &bioseq_slice_project_path_methods()) {
                return false;
            }
            if (!per_row_restrictinfo_list_has_no_helper(custom->custom_restrictinfo) ||
                !per_row_path_metadata_has_no_helper(
                    reinterpret_cast<Node*>(custom->custom_private)))
                return false;
            path->pathtarget = normalized_target;
            for (Path* child : postgres_list<Path>(custom->custom_paths)) {
                if (!per_row_normalize_private_path_spine(
                        root, child, candidate_helpers, validated_inherited_helpers, required_vars))
                    return false;
            }
            return true;
        }

        JoinPath* join = nullptr;
        if (IsA(path, NestPath)) {
            join = &reinterpret_cast<NestPath*>(path)->jpath;
        } else if (IsA(path, MergePath)) {
            join = &reinterpret_cast<MergePath*>(path)->jpath;
        } else if (IsA(path, HashPath)) {
            join = &reinterpret_cast<HashPath*>(path)->jpath;
        }
        if (join != nullptr) {
            if (!per_row_restrictinfo_list_has_no_helper(join->joinrestrictinfo))
                return false;
            if (IsA(path, MergePath)) {
                auto* merge = reinterpret_cast<MergePath*>(path);
                List* outer_available = NIL;
                List* inner_available = NIL;
                if (!per_row_restrictinfo_list_has_no_helper(merge->path_mergeclauses) ||
                    !per_row_collect_available_path_helpers(join->outerjoinpath,
                                                            candidate_helpers,
                                                            validated_inherited_helpers,
                                                            outer_available) ||
                    !per_row_collect_available_path_helpers(join->innerjoinpath,
                                                            candidate_helpers,
                                                            validated_inherited_helpers,
                                                            inner_available) ||
                    !per_row_pathkeys_use_available_helpers(
                        join->outerjoinpath, merge->outersortkeys, outer_available) ||
                    !per_row_pathkeys_use_available_helpers(
                        join->innerjoinpath, merge->innersortkeys, inner_available))
                    return false;
            } else if (IsA(path, HashPath) &&
                       !per_row_restrictinfo_list_has_no_helper(
                           reinterpret_cast<HashPath*>(path)->path_hashclauses)) {
                return false;
            }
            path->pathtarget = normalized_target;
            return per_row_normalize_private_path_spine(root,
                                                        join->outerjoinpath,
                                                        candidate_helpers,
                                                        validated_inherited_helpers,
                                                        required_vars) &&
                   per_row_normalize_private_path_spine(root,
                                                        join->innerjoinpath,
                                                        candidate_helpers,
                                                        validated_inherited_helpers,
                                                        required_vars);
        }

        Path* unary = nullptr;
        bool target_preserving = false;
        if (IsA(path, ProjectionPath)) {
            auto* projection = reinterpret_cast<ProjectionPath*>(path);
            unary = projection->subpath;
            if (target_changed)
                projection->dummypp = false;
        } else if (IsA(path, ProjectSetPath)) {
            unary = reinterpret_cast<ProjectSetPath*>(path)->subpath;
        } else if (IsA(path, SortPath)) {
            unary = reinterpret_cast<SortPath*>(path)->subpath;
            target_preserving = true;
        } else if (IsA(path, IncrementalSortPath)) {
            unary = reinterpret_cast<IncrementalSortPath*>(path)->spath.subpath;
            target_preserving = true;
        } else if (IsA(path, MaterialPath)) {
            unary = reinterpret_cast<MaterialPath*>(path)->subpath;
            target_preserving = true;
        } else if (IsA(path, MemoizePath)) {
            auto* memoize = reinterpret_cast<MemoizePath*>(path);
            if (!per_row_path_metadata_has_no_helper(reinterpret_cast<Node*>(memoize->param_exprs)))
                return false;
            unary = memoize->subpath;
            target_preserving = true;
        } else if (IsA(path, UniquePath)) {
            auto* unique = reinterpret_cast<UniquePath*>(path);
            if (!per_row_path_metadata_has_no_helper(reinterpret_cast<Node*>(unique->uniq_exprs)))
                return false;
            unary = unique->subpath;
            target_preserving = true;
        } else if (IsA(path, GatherPath)) {
            unary = reinterpret_cast<GatherPath*>(path)->subpath;
            target_preserving = true;
        } else if (IsA(path, GatherMergePath)) {
            unary = reinterpret_cast<GatherMergePath*>(path)->subpath;
            target_preserving = true;
        }
        if (unary != nullptr) {
            path->pathtarget = normalized_target;
            if (!per_row_normalize_private_path_spine(
                    root, unary, candidate_helpers, validated_inherited_helpers, required_vars))
                return false;
            return !target_preserving ||
                   per_row_authoritative_targets_match(root, path->pathtarget, unary->pathtarget);
        }

        List* children = NIL;
        if (IsA(path, AppendPath)) {
            children = reinterpret_cast<AppendPath*>(path)->subpaths;
        } else if (IsA(path, MergeAppendPath)) {
            children = reinterpret_cast<MergeAppendPath*>(path)->subpaths;
        }
        if (children != NIL) {
            path->pathtarget = normalized_target;
            for (Path* child : postgres_list<Path>(children)) {
                if (!per_row_normalize_private_path_spine(
                        root, child, candidate_helpers, validated_inherited_helpers, required_vars))
                    return false;
            }
            return per_row_append_targets_are_ordinally_compatible(path->pathtarget, children);
        }

        /* Ordinary base scan paths are immutable leaves in the existing COW
         * cloner.  They may be shared only when normalization is a no-op; direct
         * BioseqScan leaves were handled above and are private CustomPaths. */
        if (path->parent != nullptr && (path->parent->reloptkind == RELOPT_BASEREL ||
                                        path->parent->reloptkind == RELOPT_OTHER_MEMBER_REL)) {
            return !target_changed;
        }
        return false;
    }

    /* SQL cannot deterministically retain each of these core wrapper Path types:
     * PostgreSQL commonly folds ProjectionPath, chooses Material/Memoize by cost,
     * and represents SELECT DISTINCT with an upper path rather than UniquePath.
     * This integration fixture constructs their real node types and invokes
     * the same exact-owner analysis and copy-on-write propagation used by transformed
     * mmsearch planning.  A genuine nested sliced-mmsearch execution in the
     * acceptance suite separately validates final setrefs/executor helper binding. */
    [[nodiscard]] CustomPathMethods const&
    match_helper_wrapper_probe_unknown_path_methods() noexcept {
        static const CustomPathMethods methods{
            .CustomName = "BioseqMatchHelperWrapperProbeUnknown",
            .PlanCustomPath = nullptr,
            .ReparameterizeCustomPathByChild = nullptr,
        };
        return methods;
    }

    void match_helper_wrapper_probe_init_path(Path* path,
                                              NodeTag plan_tag,
                                              RelOptInfo* parent,
                                              PathTarget* target) noexcept {
        Assert(path != nullptr);
        path->pathtype = plan_tag;
        path->parent = parent;
        path->pathtarget = target;
        path->param_info = nullptr;
        path->parallel_aware = false;
        path->parallel_safe = false;
        path->parallel_workers = 0;
        path->rows = 1.0;
        path->startup_cost = 1.0;
        path->total_cost = 2.0;
        path->pathkeys = NIL;
    }

    [[nodiscard]] RelOptInfo*
    match_helper_wrapper_probe_rel(RelOptKind kind, Index relid, Relids relids) {
        auto* rel = makeNode(RelOptInfo);
        rel->reloptkind = kind;
        rel->relid = relid;
        rel->relids = relids;
        rel->rows = 1.0;
        rel->reltarget = create_empty_pathtarget();
        return rel;
    }

    struct per_row_reconstruction_rejections {
        std::size_t incumbents = 0u;
        std::size_t unsupported_path = 0u;
        std::size_t invalid_join = 0u;
        std::size_t missing_owned_identity = 0u;
        std::size_t replacement_missing = 0u;
        std::size_t projection_or_leak = 0u;
        std::size_t new_external_parameter = 0u;
        std::size_t unusable_algorithm_clause = 0u;
        std::size_t reconstructed = 0u;
        std::size_t cloned = 0u;
        std::size_t tag_missing = 0u;
        std::size_t source_projection_missing = 0u;
        std::size_t nonfinite_cost = 0u;
        std::size_t owners_created = 0u;
        int last_incumbent_tag = -1;
        int last_outer_tag = -1;
        int last_inner_tag = -1;
        const char* last_incumbent_custom_name = "none";
        const char* last_outer_custom_name = "none";
        const char* last_inner_custom_name = "none";
    };

    void per_row_record_path_identity(Path* path, int& tag, const char*& custom_name) noexcept {
        tag = path == nullptr ? -1 : static_cast<int>(nodeTag(path));
        custom_name = "none";
        if (path == nullptr || !IsA(path, CustomPath))
            return;
        auto* custom = reinterpret_cast<CustomPath*>(path);
        if (custom->methods != nullptr && custom->methods->CustomName != nullptr) {
            custom_name = custom->methods->CustomName;
        }
    }

    [[nodiscard]] Path*
    per_row_reconstruct_inner_join_path(PlannerInfo* root,
                                        RelOptInfo* joinrel,
                                        Path* incumbent,
                                        RestrictInfo* owned,
                                        JoinPathExtraData const& original_extra,
                                        PathTarget* required_target,
                                        List* required_vars,
                                        const Index target_relid,
                                        per_row_reconstruction_rejections& rejects) {
        ++rejects.incumbents;
        if (root == nullptr || joinrel == nullptr || incumbent == nullptr || owned == nullptr ||
            required_target == nullptr || required_vars == nullptr ||
            !bioseq_is_concrete_rte_varno(target_relid)) {
            ++rejects.invalid_join;
            return nullptr;
        }
        per_row_record_path_identity(
            incumbent, rejects.last_incumbent_tag, rejects.last_incumbent_custom_name);
        JoinPath* join = nullptr;
        if (IsA(incumbent, NestPath)) {
            join = &reinterpret_cast<NestPath*>(incumbent)->jpath;
        } else if (IsA(incumbent, MergePath)) {
            join = &reinterpret_cast<MergePath*>(incumbent)->jpath;
        } else if (IsA(incumbent, HashPath)) {
            join = &reinterpret_cast<HashPath*>(incumbent)->jpath;
        } else {
            ++rejects.unsupported_path;
            return nullptr;
        }
        if (join->jointype != JOIN_INNER || join->outerjoinpath == nullptr ||
            join->innerjoinpath == nullptr ||
            batch_join_path_contains_parallel_execution(incumbent)) {
            ++rejects.invalid_join;
            return nullptr;
        }
        per_row_record_path_identity(
            join->outerjoinpath, rejects.last_outer_tag, rejects.last_outer_custom_name);
        per_row_record_path_identity(
            join->innerjoinpath, rejects.last_inner_tag, rejects.last_inner_custom_name);
        Path* outer_path = join->outerjoinpath;
        Path* inner_path = join->innerjoinpath;
        const bool join_owns = list_member_ptr(join->joinrestrictinfo, owned);
        const bool outer_owns = per_row_descendant_owns_restriction(outer_path, owned);
        const bool inner_owns = per_row_descendant_owns_restriction(inner_path, owned);
        if (IsA(incumbent, NestPath)) {
            if (!join_owns && !outer_owns && !inner_owns) {
                ++rejects.missing_owned_identity;
                return nullptr;
            }
        } else if (!join_owns) {
            /* Merge/Hash cannot legally push the join predicate into one child. */
            ++rejects.missing_owned_identity;
            return nullptr;
        }
        const bool outer_is_target =
            outer_path->parent != nullptr &&
            bms_is_member(static_cast<int>(target_relid), outer_path->parent->relids);
        const bool inner_is_target =
            inner_path->parent != nullptr &&
            bms_is_member(static_cast<int>(target_relid), inner_path->parent->relids);
        const bool outer_needs_source =
            outer_is_target && !slice_project_path_tree_has_bioseq_scan(outer_path);
        const bool inner_needs_source =
            inner_is_target && !slice_project_path_tree_has_bioseq_scan(inner_path);
        /* Only a nested loop can push the exact join RestrictInfo into a
         * parameterized child.  Provider-only replacement is also valid for
         * Merge/Hash: their reconstruction below retains the required sort/hash
         * clauses while swapping an IndexPath for a source-owning BioseqScan. */
        if ((outer_owns || inner_owns) && !IsA(incumbent, NestPath)) {
            ++rejects.invalid_join;
            return nullptr;
        }
        if (outer_owns || inner_owns || outer_needs_source || inner_needs_source) {
            /* Rebuild each affected side from a legal path whose complete descendant
             * tree does not own the predicate; unrelated LATERAL parameters remain
             * valid and are checked against the incumbent frontier below. */
            if (outer_owns || outer_needs_source)
                outer_path = per_row_path_without_owned_qual(
                    join->outerjoinpath->parent, owned, outer_is_target);
            if (inner_owns || inner_needs_source)
                inner_path = per_row_path_without_owned_qual(
                    join->innerjoinpath->parent, owned, inner_is_target);
            if (outer_path == nullptr || inner_path == nullptr) {
                ++rejects.replacement_missing;
                return nullptr;
            }
        }
        outer_path = per_row_project_required_child_vars(root, outer_path, required_vars);
        inner_path = per_row_project_required_child_vars(root, inner_path, required_vars);
        if (outer_path == nullptr || inner_path == nullptr ||
            per_row_descendant_owns_restriction(outer_path, owned) ||
            per_row_descendant_owns_restriction(inner_path, owned)) {
            ++rejects.projection_or_leak;
            return nullptr;
        }
        List* filtered_restrictions = per_row_without_restriction(join->joinrestrictinfo, owned);
        JoinPathExtraData filtered_extra = original_extra;
        filtered_extra.inner_unique = join->inner_unique;
        filtered_extra.restrictlist =
            per_row_without_restriction(original_extra.restrictlist, owned);
        filtered_extra.mergeclause_list =
            per_row_without_restriction(original_extra.mergeclause_list, owned);
        JoinCostWorkspace workspace{};
        if (IsA(incumbent, NestPath)) {
            Relids calculated_required_outer =
                calc_nestloop_required_outer(outer_path->parent->relids,
                                             PATH_REQ_OUTER(outer_path),
                                             inner_path->parent->relids,
                                             PATH_REQ_OUTER(inner_path));
            Relids required_outer = bms_copy(PATH_REQ_OUTER(&join->path));
            /* Removing the owned predicate can remove an internal nestloop
             * dependency, but it must never introduce a new dependency outside
             * the join.  Preserve the incumbent's exact external parameter ABI so
             * LATERAL/partition rescans remain attached to the same frontier. */
            if (!bms_is_subset(calculated_required_outer, required_outer)) {
                ++rejects.new_external_parameter;
                bms_free(calculated_required_outer);
                bms_free(required_outer);
                return nullptr;
            }
            bms_free(calculated_required_outer);
            List* reconstructed_pathkeys =
                pathkeys_contained_in(join->path.pathkeys, outer_path->pathkeys)
                    ? join->path.pathkeys
                    : outer_path->pathkeys;
            initial_cost_nestloop(
                root, &workspace, JOIN_INNER, outer_path, inner_path, &filtered_extra);
            Path* result = reinterpret_cast<Path*>(create_nestloop_path(root,
                                                                        joinrel,
                                                                        JOIN_INNER,
                                                                        &workspace,
                                                                        &filtered_extra,
                                                                        outer_path,
                                                                        inner_path,
                                                                        filtered_restrictions,
                                                                        reconstructed_pathkeys,
                                                                        required_outer));
            if (result != nullptr)
                ++rejects.reconstructed;
            return result;
        }
        Relids required_outer = calc_non_nestloop_required_outer(outer_path, inner_path);
        if (!bms_equal(required_outer, PATH_REQ_OUTER(&join->path))) {
            ++rejects.new_external_parameter;
            return nullptr;
        }
        if (IsA(incumbent, MergePath)) {
            auto* merge = reinterpret_cast<MergePath*>(incumbent);
            List* mergeclauses = per_row_without_restriction(merge->path_mergeclauses, owned);
            if (mergeclauses == NIL) {
                ++rejects.unusable_algorithm_clause;
                return nullptr;
            }
            /* A provider-only replacement can swap a presorted IndexPath for an
             * unordered BioseqScan.  NIL in the incumbent's sort-key fields means its
             * original child already supplies the effective order; it does not mean
             * an arbitrary replacement is ordered.  Capture that validated contract,
             * validate it still covers every retained merge clause, and request an
             * explicit sort exactly where a replacement no longer satisfies it. */
            List* effective_outer_pathkeys =
                merge->outersortkeys != NIL ? merge->outersortkeys : join->outerjoinpath->pathkeys;
            List* effective_inner_pathkeys =
                merge->innersortkeys != NIL ? merge->innersortkeys : join->innerjoinpath->pathkeys;
            if (effective_outer_pathkeys == NIL || effective_inner_pathkeys == NIL) {
                ++rejects.unusable_algorithm_clause;
                return nullptr;
            }
            List* ordered_mergeclauses =
                find_mergeclauses_for_outer_pathkeys(root, effective_outer_pathkeys, mergeclauses);
            List* required_inner_pathkeys =
                ordered_mergeclauses == NIL
                    ? NIL
                    : make_inner_pathkeys_for_merge(
                          root, ordered_mergeclauses, effective_outer_pathkeys);
            if (ordered_mergeclauses == NIL ||
                list_length(ordered_mergeclauses) != list_length(mergeclauses) ||
                required_inner_pathkeys == NIL ||
                !pathkeys_contained_in(required_inner_pathkeys, effective_inner_pathkeys)) {
                ++rejects.unusable_algorithm_clause;
                return nullptr;
            }
            List* outer_sortkeys =
                pathkeys_contained_in(effective_outer_pathkeys, outer_path->pathkeys)
                    ? NIL
                    : effective_outer_pathkeys;
            List* inner_sortkeys =
                pathkeys_contained_in(effective_inner_pathkeys, inner_path->pathkeys)
                    ? NIL
                    : effective_inner_pathkeys;
            filtered_extra.mergeclause_list = ordered_mergeclauses;
            initial_cost_mergejoin(root,
                                   &workspace,
                                   JOIN_INNER,
                                   ordered_mergeclauses,
                                   outer_path,
                                   inner_path,
                                   outer_sortkeys,
                                   inner_sortkeys,
                                   &filtered_extra);
            Path* result = reinterpret_cast<Path*>(create_mergejoin_path(root,
                                                                         joinrel,
                                                                         JOIN_INNER,
                                                                         &workspace,
                                                                         &filtered_extra,
                                                                         outer_path,
                                                                         inner_path,
                                                                         filtered_restrictions,
                                                                         join->path.pathkeys,
                                                                         required_outer,
                                                                         ordered_mergeclauses,
                                                                         outer_sortkeys,
                                                                         inner_sortkeys));
            if (result != nullptr)
                ++rejects.reconstructed;
            return result;
        }
        auto* hash = reinterpret_cast<HashPath*>(incumbent);
        List* hashclauses = per_row_without_restriction(hash->path_hashclauses, owned);
        if (hashclauses == NIL) {
            ++rejects.unusable_algorithm_clause;
            return nullptr;
        }
        const bool parallel_hash = inner_path->parallel_aware;
        initial_cost_hashjoin(root,
                              &workspace,
                              JOIN_INNER,
                              hashclauses,
                              outer_path,
                              inner_path,
                              &filtered_extra,
                              parallel_hash);
        Path* result = reinterpret_cast<Path*>(create_hashjoin_path(root,
                                                                    joinrel,
                                                                    JOIN_INNER,
                                                                    &workspace,
                                                                    &filtered_extra,
                                                                    outer_path,
                                                                    inner_path,
                                                                    parallel_hash,
                                                                    filtered_restrictions,
                                                                    required_outer,
                                                                    hashclauses));
        if (result != nullptr)
            ++rejects.reconstructed;
        return result;
    }

    [[nodiscard]] bool per_row_decline_transformed_path(const char* stage) {
        (void)stage;
        return false;
    }

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
