/**
 * @file
 * @brief Analyzes descendant helper ownership and path metadata.
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

export module bioseq.postgres.table:custom_scan_per_row_join_paths_carrier_analysis;

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

    /** @brief Counts descendant base scans that can supply one match-helper identity. */
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

    /** @brief Collects helper identities supplied uniquely by descendant custom paths. */
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

    /** @brief Tracks whether an expression references unavailable match helpers. */
    struct per_row_path_helper_audit {
        bioseq_stub_oids const* oids = nullptr;
        List* available = NIL;
        bool saw_helper = false;
        bool unavailable = false;
    };

    /** @brief Validates helper and placeholder references against a supplied identity list. */
    struct per_row_path_helper_audit_visitor {
        /** @brief Audits the current expression node and stops at an unavailable helper. */
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

    /** @brief Checks one expression against available helper identities and reports their use. */
    [[nodiscard]] bool
    per_row_expression_uses_available_helpers(Node* expression, List* available, bool& saw_helper) {
        per_row_path_helper_audit audit{&get_cached_stub_oids(), available, false, false};
        per_row_path_helper_audit_visitor visitor;
        walk_pg_expression_including_root(expression, audit, visitor);
        saw_helper = audit.saw_helper;
        return !audit.unavailable;
    }

    /** @brief Checks that relevant pathkey equivalence members use only available helpers. */
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

    /** @brief Checks recursively that opaque path metadata contains no match helper. */
    [[nodiscard]] bool per_row_path_metadata_has_no_helper(Node* metadata);

    /** @brief Tests whether a node tag is supported by the metadata expression walker. */
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

    /** @brief Tests whether a metadata node tag can be accepted without recursion. */
    [[nodiscard]] bool per_row_path_metadata_node_is_scalar(const NodeTag tag) noexcept {
        return tag == T_Integer || tag == T_Float || tag == T_Boolean || tag == T_String ||
               tag == T_BitString || tag == T_IntList || tag == T_OidList || tag == T_XidList;
    }

    /** @brief Records an unsupported node shape or match helper found in path metadata. */
    struct per_row_path_metadata_audit {
        const bioseq_stub_oids* oids = nullptr;
        bool invalid = false;
    };

    /** @brief Recursively audits expression-like path metadata for helper references. */
    struct per_row_path_metadata_audit_visitor {
        /** @brief Classifies the current metadata node and controls recursive traversal. */
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

    /** @brief Checks recursively that opaque path metadata contains no match helper. */
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

    /** @brief Checks that every RestrictInfo clause and or-clause contains no helper. */
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

    /** @brief Reconstructs sort-group references from the processed target list by occurrence. */
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

} // export custom-scan partition declarations
