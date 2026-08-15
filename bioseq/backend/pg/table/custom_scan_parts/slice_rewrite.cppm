/**
 * @file
 * @brief Installs source-aware SliceProject path rewrites at planner frontiers.
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

export module bioseq.postgres.table:custom_scan_slice_rewrite;

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

import :custom_scan_slice_path;
import :custom_scan_slice_execution;

using namespace bioseq::postgres::support;

export {

    [[nodiscard]] bool slice_project_is_bioseq_scan_path(Path* path) {
        if (path == nullptr || !IsA(path, CustomPath))
            return false;
        auto* custom = reinterpret_cast<CustomPath*>(path);
        return custom->methods != nullptr && custom->methods->CustomName != nullptr &&
               strcmp(custom->methods->CustomName, "BioseqScan") == 0;
    }

    [[nodiscard]] bool slice_project_is_owner_path(Path* path) {
        if (path == nullptr || !IsA(path, CustomPath))
            return false;
        auto* custom = reinterpret_cast<CustomPath*>(path);
        return custom->methods == &bioseq_slice_project_path_methods();
    }

    [[nodiscard]] bool slice_project_targets_match(PathTarget* actual, PathTarget* expected) {
        const int expression_count = actual == nullptr ? 0 : list_length(actual->exprs);
        if (actual == nullptr || expected == nullptr ||
            expression_count != list_length(expected->exprs) ||
            !equal(actual->exprs, expected->exprs)) {
            return false;
        }
        for (int index = 0; index < expression_count; ++index) {
            if (get_pathtarget_sortgroupref(actual, index) !=
                get_pathtarget_sortgroupref(expected, index)) {
                return false;
            }
        }
        return true;
    }

    /* An ordered upper target may omit resjunk entries retained by its child's
     * full target.  Validate every requested top-level lazy expression, including
     * its sortgroupref and duplicate multiplicity, is already published by that
     * full target.  Non-lazy outputs remain core's ordinary projection concern. */
    [[nodiscard]] bool slice_project_requested_lazy_target_is_subset(PathTarget* full_target,
                                                                     PathTarget* requested_target) {
        if (full_target == nullptr || requested_target == nullptr)
            return false;
        const int full_count = list_length(full_target->exprs);
        const int requested_count = list_length(requested_target->exprs);
        bool saw_lazy_expression = false;
        for (int requested_index = 0; requested_index < requested_count; ++requested_index) {
            auto* requested_expression =
                reinterpret_cast<Node*>(list_nth(requested_target->exprs, requested_index));
            if (!bioseq_expr_uses_lazy_sequence_projection(requested_expression))
                continue;
            saw_lazy_expression = true;
            const Index requested_sortgroupref =
                get_pathtarget_sortgroupref(requested_target, requested_index);

            int required_occurrences = 1;
            for (int prior_index = 0; prior_index < requested_index; ++prior_index) {
                auto* prior_expression =
                    reinterpret_cast<Node*>(list_nth(requested_target->exprs, prior_index));
                if (bioseq_expr_uses_lazy_sequence_projection(prior_expression) &&
                    get_pathtarget_sortgroupref(requested_target, prior_index) ==
                        requested_sortgroupref &&
                    equal(prior_expression, requested_expression)) {
                    ++required_occurrences;
                }
            }

            int available_occurrences = 0;
            for (int full_index = 0; full_index < full_count; ++full_index) {
                auto* full_expression =
                    reinterpret_cast<Node*>(list_nth(full_target->exprs, full_index));
                if (get_pathtarget_sortgroupref(full_target, full_index) ==
                        requested_sortgroupref &&
                    equal(full_expression, requested_expression)) {
                    ++available_occurrences;
                }
            }
            if (available_occurrences < required_occurrences)
                return false;
        }
        return saw_lazy_expression;
    }

    /* Recognition only: after the subset validation, pin the child's complete target
     * and cross exactly the ordering/parallel containers which preserve every
     * expression and sortgroupref.  Never cross projection, materialization,
     * joins, or any other evaluation frontier. */
    [[nodiscard]] bool
    slice_project_is_owner_below_target_preserving_ordering(Path* path,
                                                            PathTarget* requested_target) {
        if (path == nullptr ||
            !slice_project_requested_lazy_target_is_subset(path->pathtarget, requested_target))
            return false;
        PathTarget* const full_target = path->pathtarget;
        while (path != nullptr && slice_project_targets_match(path->pathtarget, full_target)) {
            if (slice_project_is_owner_path(path))
                return true;
            if (IsA(path, SortPath)) {
                path = reinterpret_cast<SortPath*>(path)->subpath;
                continue;
            }
            if (IsA(path, IncrementalSortPath)) {
                path = reinterpret_cast<IncrementalSortPath*>(path)->spath.subpath;
                continue;
            }
            if (IsA(path, GatherPath)) {
                path = reinterpret_cast<GatherPath*>(path)->subpath;
                continue;
            }
            if (IsA(path, GatherMergePath)) {
                path = reinterpret_cast<GatherMergePath*>(path)->subpath;
                continue;
            }
            return false;
        }
        return false;
    }

    /* Planner Path nodes are deliberately marked `no_copy_equal` by PostgreSQL.
     * `copyObject(path)` therefore reaches copyObjectImpl's "unrecognized node
     * type" branch for every Path-family tag.  Clone exactly the mutable path
     * spine that slice_project_tag_owned_path_tree() descends, while sharing
     * immutable leaf paths and expression metadata.  This keeps Path traversal
     * structural and reserves PostgreSQL expression walkers/copying for actual
     * Expr/List nodes. */
    [[nodiscard]] Path* slice_project_clone_mutable_path_spine(Path* path);

    [[nodiscard]] List* slice_project_clone_path_list(List* paths) {
        List* clones = NIL;
        for (Path* child : postgres_list<Path>(paths)) {
            clones = lappend(clones, slice_project_clone_mutable_path_spine(child));
        }
        return clones;
    }

    [[nodiscard]] Path* slice_project_clone_mutable_path_spine(Path* path) {
        if (path == nullptr)
            return nullptr;
        if (IsA(path, NestPath)) {
            auto* clone = makeNode(NestPath);
            *clone = *reinterpret_cast<NestPath*>(path);
            clone->jpath.outerjoinpath =
                slice_project_clone_mutable_path_spine(clone->jpath.outerjoinpath);
            clone->jpath.innerjoinpath =
                slice_project_clone_mutable_path_spine(clone->jpath.innerjoinpath);
            return reinterpret_cast<Path*>(clone);
        }
        if (IsA(path, MergePath)) {
            auto* clone = makeNode(MergePath);
            *clone = *reinterpret_cast<MergePath*>(path);
            clone->jpath.outerjoinpath =
                slice_project_clone_mutable_path_spine(clone->jpath.outerjoinpath);
            clone->jpath.innerjoinpath =
                slice_project_clone_mutable_path_spine(clone->jpath.innerjoinpath);
            return reinterpret_cast<Path*>(clone);
        }
        if (IsA(path, HashPath)) {
            auto* clone = makeNode(HashPath);
            *clone = *reinterpret_cast<HashPath*>(path);
            clone->jpath.outerjoinpath =
                slice_project_clone_mutable_path_spine(clone->jpath.outerjoinpath);
            clone->jpath.innerjoinpath =
                slice_project_clone_mutable_path_spine(clone->jpath.innerjoinpath);
            return reinterpret_cast<Path*>(clone);
        }
        if (IsA(path, CustomPath)) {
            auto* clone = makeNode(CustomPath);
            *clone = *reinterpret_cast<CustomPath*>(path);
            clone->custom_paths = slice_project_clone_path_list(clone->custom_paths);
            clone->custom_restrictinfo = list_copy(clone->custom_restrictinfo);
            clone->custom_private = list_copy(clone->custom_private);
            return reinterpret_cast<Path*>(clone);
        }
        if (IsA(path, ProjectionPath)) {
            auto* clone = makeNode(ProjectionPath);
            *clone = *reinterpret_cast<ProjectionPath*>(path);
            clone->subpath = slice_project_clone_mutable_path_spine(clone->subpath);
            return reinterpret_cast<Path*>(clone);
        }
        if (IsA(path, ProjectSetPath)) {
            auto* clone = makeNode(ProjectSetPath);
            *clone = *reinterpret_cast<ProjectSetPath*>(path);
            clone->subpath = slice_project_clone_mutable_path_spine(clone->subpath);
            return reinterpret_cast<Path*>(clone);
        }
        if (IsA(path, IncrementalSortPath)) {
            auto* clone = makeNode(IncrementalSortPath);
            *clone = *reinterpret_cast<IncrementalSortPath*>(path);
            clone->spath.subpath = slice_project_clone_mutable_path_spine(clone->spath.subpath);
            return reinterpret_cast<Path*>(clone);
        }
        if (IsA(path, SortPath)) {
            auto* clone = makeNode(SortPath);
            *clone = *reinterpret_cast<SortPath*>(path);
            clone->subpath = slice_project_clone_mutable_path_spine(clone->subpath);
            return reinterpret_cast<Path*>(clone);
        }
        if (IsA(path, MaterialPath)) {
            auto* clone = makeNode(MaterialPath);
            *clone = *reinterpret_cast<MaterialPath*>(path);
            clone->subpath = slice_project_clone_mutable_path_spine(clone->subpath);
            return reinterpret_cast<Path*>(clone);
        }
        if (IsA(path, MemoizePath)) {
            auto* clone = makeNode(MemoizePath);
            *clone = *reinterpret_cast<MemoizePath*>(path);
            clone->subpath = slice_project_clone_mutable_path_spine(clone->subpath);
            return reinterpret_cast<Path*>(clone);
        }
        if (IsA(path, UniquePath)) {
            auto* clone = makeNode(UniquePath);
            *clone = *reinterpret_cast<UniquePath*>(path);
            clone->subpath = slice_project_clone_mutable_path_spine(clone->subpath);
            return reinterpret_cast<Path*>(clone);
        }
        if (IsA(path, GatherPath)) {
            auto* clone = makeNode(GatherPath);
            *clone = *reinterpret_cast<GatherPath*>(path);
            clone->subpath = slice_project_clone_mutable_path_spine(clone->subpath);
            return reinterpret_cast<Path*>(clone);
        }
        if (IsA(path, GatherMergePath)) {
            auto* clone = makeNode(GatherMergePath);
            *clone = *reinterpret_cast<GatherMergePath*>(path);
            clone->subpath = slice_project_clone_mutable_path_spine(clone->subpath);
            return reinterpret_cast<Path*>(clone);
        }
        if (IsA(path, AppendPath)) {
            auto* clone = makeNode(AppendPath);
            *clone = *reinterpret_cast<AppendPath*>(path);
            clone->subpaths = slice_project_clone_path_list(clone->subpaths);
            return reinterpret_cast<Path*>(clone);
        }
        if (IsA(path, MergeAppendPath)) {
            auto* clone = makeNode(MergeAppendPath);
            *clone = *reinterpret_cast<MergeAppendPath*>(path);
            clone->subpaths = slice_project_clone_path_list(clone->subpaths);
            return reinterpret_cast<Path*>(clone);
        }
        /* The tagger never mutates an unrecognised leaf/container. */
        return path;
    }

    [[nodiscard]] Path* slice_project_tag_source_child(Path* path,
                                                       PathTarget* target,
                                                       slice_project_ir_builder const& builder) {
        if (!slice_project_is_bioseq_scan_path(path))
            return path;
        auto* original = reinterpret_cast<CustomPath*>(path);
        auto* copy = makeNode(CustomPath);
        *copy = *original;
        copy->custom_private = NIL;
        for (Node* node : postgres_list<Node>(original->custom_private)) {
            if (node != nullptr && IsA(node, List)) {
                List* tag = reinterpret_cast<List*>(node);
                if (list_length(tag) >= 1 && IsA(reinterpret_cast<Node*>(linitial(tag)), Integer) &&
                    intVal(linitial(tag)) == k_bioseq_slice_source_magic) {
                    continue;
                }
            }
            copy->custom_private = lappend(copy->custom_private, copyObject(node));
        }
        List* bindings = NIL;
        Bitmapset* seen_column_attnos = nullptr;
        int resno = 1;
        /* BSS3 bindings are [child resno, producer id, physical varattno].  The
         * child resno validates plan/setrefs identity; promotion indexes the raw
         * relation scan slot by physical varattno. */
        for (Node* expression : postgres_list<Node>(target->exprs)) {
            if (exprType(expression) == builder.oids->bioseq_seq_type_oid) {
                Var* var = try_cast_pg_node<Var>(expression);
                if (var == nullptr || var->varattno <= 0 ||
                    bms_is_member(var->varattno, seen_column_attnos)) {
                    ++resno;
                    continue;
                }
                std::uint32_t producer_id = 0u;
                bool binding_found = false;
                for (auto const& binding : builder.source_bindings) {
                    if (equal(binding.first, expression)) {
                        producer_id = binding.second;
                        binding_found = true;
                        break;
                    }
                }
                if (builder.source_bindings_only && !binding_found) {
                    ++resno;
                    continue;
                }
                bindings = lappend(bindings,
                                   list_make3(makeInteger(resno),
                                              makeInteger(static_cast<int>(producer_id)),
                                              makeInteger(static_cast<int>(var->varattno))));
                seen_column_attnos = bms_add_member(seen_column_attnos, var->varattno);
            }
            ++resno;
        }
        if (seen_column_attnos != nullptr)
            bms_free(seen_column_attnos);
        copy->custom_private = lappend(copy->custom_private,
                                       list_make3(makeInteger(k_bioseq_slice_source_magic),
                                                  makeInteger(k_bioseq_slice_source_version),
                                                  bindings));
        copy->path.pathtarget = target;
        return reinterpret_cast<Path*>(copy);
    }

    /* `path` is already a private copy.  Descend only core path shapes whose
     * child ownership is part of PostgreSQL's public Path ABI; unknown custom and
     * foreign providers remain unmodified unless they expose their own contract. */
    [[nodiscard]] Path* slice_project_tag_owned_path_tree(Path* path,
                                                          slice_project_ir_builder const& builder,
                                                          bool& tagged_source) {
        if (path == nullptr)
            return nullptr;
        if (slice_project_is_bioseq_scan_path(path)) {
            tagged_source = true;
            PathTarget* leaf_target = copy_pathtarget(path->pathtarget);
            if (builder.planner_root != nullptr && path->parent != nullptr &&
                path->parent->relid == builder.target_leaf_support_relid) {
                List* exact_support = NIL;
                for (Var* support : postgres_list<Var>(builder.target_leaf_support_vars)) {
                    if (support != nullptr && bioseq_is_concrete_rte_varno(support->varno) &&
                        support->varno == builder.target_leaf_support_relid &&
                        support->varattno > 0 && support->varlevelsup == 0 &&
                        bms_is_empty(support->varnullingrels)) {
                        exact_support = lappend(exact_support, support);
                    }
                }
                add_new_columns_to_pathtarget(leaf_target, exact_support);
                set_pathtarget_cost_width(builder.planner_root, leaf_target);
            }
            return slice_project_tag_source_child(path, leaf_target, builder);
        }
        if (IsA(path, NestPath)) {
            auto* join = &reinterpret_cast<NestPath*>(path)->jpath;
            join->outerjoinpath =
                slice_project_tag_owned_path_tree(join->outerjoinpath, builder, tagged_source);
            join->innerjoinpath =
                slice_project_tag_owned_path_tree(join->innerjoinpath, builder, tagged_source);
            return path;
        }
        if (IsA(path, MergePath)) {
            auto* join = &reinterpret_cast<MergePath*>(path)->jpath;
            join->outerjoinpath =
                slice_project_tag_owned_path_tree(join->outerjoinpath, builder, tagged_source);
            join->innerjoinpath =
                slice_project_tag_owned_path_tree(join->innerjoinpath, builder, tagged_source);
            return path;
        }
        if (IsA(path, HashPath)) {
            auto* join = &reinterpret_cast<HashPath*>(path)->jpath;
            join->outerjoinpath =
                slice_project_tag_owned_path_tree(join->outerjoinpath, builder, tagged_source);
            join->innerjoinpath =
                slice_project_tag_owned_path_tree(join->innerjoinpath, builder, tagged_source);
            return path;
        }
        if (IsA(path, CustomPath)) {
            auto* custom = reinterpret_cast<CustomPath*>(path);
            for (ListCell* cell = list_head(custom->custom_paths); cell != nullptr;
                 cell = lnext(custom->custom_paths, cell)) {
                lfirst(cell) = slice_project_tag_owned_path_tree(
                    reinterpret_cast<Path*>(lfirst(cell)), builder, tagged_source);
            }
            return path;
        }
        if (IsA(path, ProjectionPath)) {
            auto* projection = reinterpret_cast<ProjectionPath*>(path);
            projection->subpath =
                slice_project_tag_owned_path_tree(projection->subpath, builder, tagged_source);
            return path;
        }
        if (IsA(path, ProjectSetPath)) {
            auto* projection = reinterpret_cast<ProjectSetPath*>(path);
            projection->subpath =
                slice_project_tag_owned_path_tree(projection->subpath, builder, tagged_source);
            return path;
        }
        if (IsA(path, SortPath)) {
            auto* sort = reinterpret_cast<SortPath*>(path);
            sort->subpath =
                slice_project_tag_owned_path_tree(sort->subpath, builder, tagged_source);
            return path;
        }
        if (IsA(path, IncrementalSortPath)) {
            auto* sort = reinterpret_cast<IncrementalSortPath*>(path);
            sort->spath.subpath =
                slice_project_tag_owned_path_tree(sort->spath.subpath, builder, tagged_source);
            return path;
        }
        if (IsA(path, MaterialPath)) {
            auto* material = reinterpret_cast<MaterialPath*>(path);
            material->subpath =
                slice_project_tag_owned_path_tree(material->subpath, builder, tagged_source);
            return path;
        }
        if (IsA(path, MemoizePath)) {
            auto* memoize = reinterpret_cast<MemoizePath*>(path);
            memoize->subpath =
                slice_project_tag_owned_path_tree(memoize->subpath, builder, tagged_source);
            return path;
        }
        if (IsA(path, UniquePath)) {
            auto* unique = reinterpret_cast<UniquePath*>(path);
            unique->subpath =
                slice_project_tag_owned_path_tree(unique->subpath, builder, tagged_source);
            return path;
        }
        if (IsA(path, GatherPath)) {
            auto* gather = reinterpret_cast<GatherPath*>(path);
            gather->subpath =
                slice_project_tag_owned_path_tree(gather->subpath, builder, tagged_source);
            return path;
        }
        if (IsA(path, GatherMergePath)) {
            auto* gather = reinterpret_cast<GatherMergePath*>(path);
            gather->subpath =
                slice_project_tag_owned_path_tree(gather->subpath, builder, tagged_source);
            return path;
        }
        auto tag_path_list = [&builder, &tagged_source](List* subpaths) {
            for (ListCell* cell = list_head(subpaths); cell != nullptr;
                 cell = lnext(subpaths, cell)) {
                lfirst(cell) = slice_project_tag_owned_path_tree(
                    reinterpret_cast<Path*>(lfirst(cell)), builder, tagged_source);
            }
        };
        if (IsA(path, AppendPath)) {
            tag_path_list(reinterpret_cast<AppendPath*>(path)->subpaths);
        } else if (IsA(path, MergeAppendPath)) {
            tag_path_list(reinterpret_cast<MergeAppendPath*>(path)->subpaths);
        }
        return path;
    }

    /* Match the exact public Path containers understood by the tagger above.
     * Transformed per-row owners may not choose an IndexPath merely because it is
     * the cheapest mmsearch-free replacement: at least the target side must keep
     * a BioseqScan provider from which BSS3 can borrow the lazy source. */
    [[nodiscard]] bool slice_project_path_tree_has_bioseq_scan(Path* path) noexcept {
        if (path == nullptr)
            return false;
        if (slice_project_is_bioseq_scan_path(path))
            return true;
        if (IsA(path, NestPath)) {
            auto* join = &reinterpret_cast<NestPath*>(path)->jpath;
            return slice_project_path_tree_has_bioseq_scan(join->outerjoinpath) ||
                   slice_project_path_tree_has_bioseq_scan(join->innerjoinpath);
        }
        if (IsA(path, MergePath)) {
            auto* join = &reinterpret_cast<MergePath*>(path)->jpath;
            return slice_project_path_tree_has_bioseq_scan(join->outerjoinpath) ||
                   slice_project_path_tree_has_bioseq_scan(join->innerjoinpath);
        }
        if (IsA(path, HashPath)) {
            auto* join = &reinterpret_cast<HashPath*>(path)->jpath;
            return slice_project_path_tree_has_bioseq_scan(join->outerjoinpath) ||
                   slice_project_path_tree_has_bioseq_scan(join->innerjoinpath);
        }
        if (IsA(path, CustomPath)) {
            for (Path* child :
                 postgres_list<Path>(reinterpret_cast<CustomPath*>(path)->custom_paths)) {
                if (slice_project_path_tree_has_bioseq_scan(child))
                    return true;
            }
            return false;
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
            return slice_project_path_tree_has_bioseq_scan(unary);
        }
        List* children = NIL;
        if (IsA(path, AppendPath)) {
            children = reinterpret_cast<AppendPath*>(path)->subpaths;
        } else if (IsA(path, MergeAppendPath)) {
            children = reinterpret_cast<MergeAppendPath*>(path)->subpaths;
        }
        for (Path* child : postgres_list<Path>(children)) {
            if (slice_project_path_tree_has_bioseq_scan(child))
                return true;
        }
        return false;
    }

    /* BatchSort is an upper owner rather than a SliceProject node, but it needs
     * the same exact base-scan source contract: copy the owned input tree, attach
     * BSS3 only to BioseqScan leaves, and leave unknown providers untouched. */
    [[nodiscard]] Path*
    batch_sort_tag_source_path(Path* path, Node* sort_expression, bool* tagged_out) {
        if (tagged_out == nullptr)
            return nullptr;
        *tagged_out = false;
        if (path == nullptr || sort_expression == nullptr)
            return path;
        const bioseq_stub_oids* oids = try_get_cached_stub_oids();
        if (oids == nullptr || exprType(sort_expression) != oids->bioseq_seq_type_oid)
            return path;
        slice_project_ir_builder builder{
            .oids = oids,
            .command_type = CMD_SELECT,
            .source_bindings_only = true,
        };
        /* BatchSort has no SliceProject producer graph.  Producer zero therefore
         * truthfully means source ownership without boundary attribution. */
        builder.source_bindings.emplace_back(reinterpret_cast<Node*>(copyObject(sort_expression)),
                                             0u);
        Path* private_path = slice_project_clone_mutable_path_spine(path);
        bool tagged = false;
        private_path = slice_project_tag_owned_path_tree(private_path, builder, tagged);
        *tagged_out = tagged;
        return tagged ? private_path : path;
    }

    void batch_sort_collect_source_routes(PlanState* plan,
                                          bioseq::storage::pg_sequence_source_route_set& routes,
                                          std::uint32_t& provider_count) {
        if (plan == nullptr)
            return;
        if (IsA(plan, CustomScanState)) {
            auto* custom = reinterpret_cast<CustomScanState*>(plan);
            if (custom->methods == bioseq_column_scan_execute_methods()) {
                auto* scan = scan_state::from_custom_scan_state(custom);
                bioseq_scan_runtime& runtime = bioseq_runtime(scan);
                if (!runtime.lazy_source_projection)
                    return;
                if (runtime.lazy_source_provider == nullptr ||
                    !bioseq::storage::add_pg_sequence_source_route(routes,
                                                                   runtime.lazy_source_provider)) {
                    ereport(ERROR,
                            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                             errmsg("BioseqBatchSort source route is unavailable")));
                }
                provider_count = bioseq::common::saturating_add(provider_count, 1u);
                return;
            }
            for (PlanState* child : postgres_list<PlanState>(custom->custom_ps)) {
                batch_sort_collect_source_routes(child, routes, provider_count);
            }
            return;
        }
        if (IsA(plan, AppendState)) {
            auto* append = reinterpret_cast<AppendState*>(plan);
            for (int index = 0; index < append->as_nplans; ++index) {
                batch_sort_collect_source_routes(
                    append->appendplans[index], routes, provider_count);
            }
            return;
        }
        if (IsA(plan, MergeAppendState)) {
            auto* append = reinterpret_cast<MergeAppendState*>(plan);
            for (int index = 0; index < append->ms_nplans; ++index) {
                batch_sort_collect_source_routes(append->mergeplans[index], routes, provider_count);
            }
            return;
        }
        PlanState* outer = outerPlanState(plan);
        PlanState* inner = innerPlanState(plan);
        batch_sort_collect_source_routes(outer, routes, provider_count);
        if (inner != outer) {
            batch_sort_collect_source_routes(inner, routes, provider_count);
        }
    }

    [[nodiscard]] bool
    batch_sort_attach_source_routes(PlanState* child,
                                    bioseq::storage::pg_sequence_source_route_set& routes) {
        std::uint32_t provider_count = 0u;
        batch_sort_collect_source_routes(child, routes, provider_count);
        return provider_count != 0u;
    }

    struct slice_project_base_target_validation {
        Index relid = 0;
        bool allow_row_identity = false;
        bool saw_base_var = false;
        bool invalid = false;
    };

    [[nodiscard]] bool slice_project_is_target_row_identity_var(const Var* var,
                                                                const Index relid) noexcept {
        if (var == nullptr || !bioseq_is_concrete_rte_varno(relid) || var->varno != relid ||
            var->varlevelsup != 0 || !bms_is_empty(var->varnullingrels))
            return false;
        return (var->varattno == SelfItemPointerAttributeNumber && var->vartype == TIDOID &&
                var->vartypmod == -1 && !OidIsValid(var->varcollid)) ||
               (var->varattno == TableOidAttributeNumber && var->vartype == OIDOID &&
                var->vartypmod == -1 && !OidIsValid(var->varcollid));
    }

    struct slice_project_base_target_visitor {
        [[nodiscard]] pg_walk_step
        operator()(pg_expression_walk_cursor,
                   pg_node_view node,
                   slice_project_base_target_validation& validation) const noexcept {
            if (node.try_cast<PlaceHolderVar>() != nullptr) {
                /* A PHV belongs to a planner-selected evaluation/nulling frontier.
                 * A base SliceProject must not move it below that frontier. */
                validation.invalid = true;
                return pg_walk_step::stop;
            }
            if (Var* var = node.try_cast<Var>()) {
                if (validation.allow_row_identity &&
                    slice_project_is_target_row_identity_var(var, validation.relid)) {
                    return pg_walk_step::skip_subtree;
                }
                if (var->varno != validation.relid || var->varattno <= 0 || var->varlevelsup != 0 ||
                    !bms_is_empty(var->varnullingrels)) {
                    validation.invalid = true;
                    return pg_walk_step::stop;
                }
                validation.saw_base_var = true;
                return pg_walk_step::skip_subtree;
            }
            return pg_walk_step::descend;
        }
    };

    [[nodiscard]] bool slice_project_target_has_semantic_owner(PlannerInfo* root,
                                                               PathTarget* target) {
        if (root == nullptr || root->parse == nullptr || target == nullptr) {
            return true;
        }
        Node* expressions = reinterpret_cast<Node*>(target->exprs);
        return root->parse->hasAggs || root->parse->hasWindowFuncs || root->parse->hasTargetSRFs ||
               root->parse->groupClause != NIL || root->parse->groupingSets != NIL ||
               root->parse->havingQual != nullptr || root->parse->setOperations != nullptr ||
               contain_agg_clause(expressions) || contain_window_function(expressions) ||
               contain_volatile_functions(expressions) || contain_subplans(expressions) ||
               expression_returns_set(expressions);
    }

    /* UPDATE's RETURNING target belongs to ModifyTable, not to its scan/join
     * subplan.  We may describe a nonvolatile lazy subtree below a volatile final
     * consumer, but we must never move or execute that consumer here.  This
     * validation checks only the base-local Var/semantic frontier and deliberately
     * does not reject volatile ancestors; slice_project_discover_target() rejects
     * every recognized lazy subtree which itself contains volatility. */
    [[nodiscard]] bool slice_project_update_recipe_target_is_safe(PlannerInfo* root,
                                                                  RelOptInfo* rel,
                                                                  PathTarget* target) {
        if (root == nullptr || rel == nullptr || root->parse == nullptr || target == nullptr ||
            target->exprs == NIL || contain_agg_clause(reinterpret_cast<Node*>(target->exprs)) ||
            contain_window_function(reinterpret_cast<Node*>(target->exprs)) ||
            contain_subplans(reinterpret_cast<Node*>(target->exprs)) ||
            expression_returns_set(reinterpret_cast<Node*>(target->exprs))) {
            return false;
        }
        slice_project_base_target_validation validation{.relid = rel->relid};
        slice_project_base_target_visitor visitor;
        walk_pg_expression_including_root(
            reinterpret_cast<Node*>(target->exprs), validation, visitor);
        return validation.saw_base_var && !validation.invalid;
    }

    /* Validate that the final processed projection can be evaluated immediately
     * above this one base relation.  The single-baserel restriction is intentionally
     * strict: a wider query needs a join/upper owner which preserves its own
     * nulling and evaluation frontier. */
    [[nodiscard]] bool
    slice_project_base_target_is_safe(PlannerInfo* root, RelOptInfo* rel, PathTarget* target) {
        if (root == nullptr || rel == nullptr || root->parse == nullptr || target == nullptr ||
            target->exprs == NIL || root->parse->hasRowSecurity ||
            !(root->parse->commandType == CMD_SELECT || root->parse->commandType == CMD_INSERT ||
              root->parse->commandType == CMD_UPDATE) ||
            rel->relid == 0 || rel->reloptkind != RELOPT_BASEREL ||
            bms_num_members(root->all_baserels) != 1 ||
            !bms_is_member(static_cast<int>(rel->relid), root->all_baserels) ||
            !bms_is_empty(rel->lateral_relids) ||
            slice_project_target_has_semantic_owner(root, target)) {
            return false;
        }
        slice_project_base_target_validation validation{
            .relid = rel->relid,
            .allow_row_identity = root->parse->commandType == CMD_UPDATE,
        };
        slice_project_base_target_visitor visitor;
        walk_pg_expression_including_root(
            reinterpret_cast<Node*>(target->exprs), validation, visitor);
        return (validation.saw_base_var || root->parse->commandType == CMD_UPDATE) &&
               !validation.invalid;
    }

    [[nodiscard]] bool slice_project_update_sources_are_stable(
        PlannerInfo* root, RelOptInfo* rel, slice_project_ir_builder const& builder) {
        if (root == nullptr || rel == nullptr || root->update_colnos == NIL) {
            return false;
        }
        constexpr int pull_flags =
            PVC_RECURSE_AGGREGATES | PVC_RECURSE_WINDOWFUNCS | PVC_RECURSE_PLACEHOLDERS;
        bool saw_sequence_source = false;
        for (Node* expression : postgres_list<Node>(builder.expressions)) {
            List* vars = pull_var_clause(expression, pull_flags);
            for (Var* var : postgres_list<Var>(vars)) {
                if (var->vartype != builder.oids->bioseq_seq_type_oid)
                    continue;
                if (var->varno != rel->relid || var->varattno <= 0 || var->varlevelsup != 0 ||
                    !bms_is_empty(var->varnullingrels) ||
                    list_member_int(root->update_colnos, var->varattno)) {
                    return false;
                }
                saw_sequence_source = true;
            }
        }
        return saw_sequence_source;
    }

    /* Install the lowest valid owner for a base-local recipe.  Every surviving
     * alternative is a real SliceProject -> BioseqScan pair; incompatible index,
     * heap, and untagged paths are removed only after at least one complete owner
     * has been constructed.  Costs remain ordinary nonnegative PostgreSQL costs. */
    void
    slice_project_try_install_base_owner(PlannerInfo* root, RelOptInfo* rel, Relation relation) {
        if (!bioseq::settings::planner::custom_scan::enabled || root == nullptr || rel == nullptr ||
            relation == nullptr || root->processed_tlist == NIL || root->parse == nullptr)
            return;
        const bool update_returning = root->parse->commandType == CMD_UPDATE;
        if (update_returning &&
            (root->parse->resultRelation != rel->relid || root->parse->returningList == NIL ||
             relation->trigdesc != nullptr))
            return;
        PathTarget* projection_target = make_pathtarget_from_tlist(root->processed_tlist);
        set_pathtarget_cost_width(root, projection_target);
        if (!slice_project_base_target_is_safe(root, rel, projection_target))
            return;
        if (!is_parallel_safe(root, reinterpret_cast<Node*>(projection_target->exprs)))
            return;
        const bioseq_stub_oids* oids = try_get_cached_stub_oids();
        if (oids == nullptr)
            return;
        slice_project_ir_builder builder{.oids = oids, .command_type = root->parse->commandType};
        PathTarget* recipe_target = projection_target;
        if (update_returning) {
            recipe_target = make_pathtarget_from_tlist(root->parse->returningList);
            set_pathtarget_cost_width(root, recipe_target);
            if (!slice_project_update_recipe_target_is_safe(root, rel, recipe_target))
                return;
        }
        if (!slice_project_discover_target(recipe_target, builder))
            return;
        if (!is_parallel_safe(root, reinterpret_cast<Node*>(builder.expressions)))
            return;
        if (update_returning) {
            /* RETURNING is evaluated by ModifyTable against the actual updated
             * tuple.  These bindings use RETURNING resnos and must never be applied
             * to the distinct UPDATE subplan output descriptor. */
            builder.output_bindings = NIL;
            if (!slice_project_update_sources_are_stable(root, rel, builder))
                return;
        }

        List* required = NIL;
        constexpr int pull_flags =
            PVC_RECURSE_AGGREGATES | PVC_RECURSE_WINDOWFUNCS | PVC_RECURSE_PLACEHOLDERS;
        for (Node* expression : postgres_list<Node>(projection_target->exprs)) {
            required = list_concat(required, pull_var_clause(expression, pull_flags));
        }
        if (update_returning) {
            for (Node* expression : postgres_list<Node>(builder.expressions)) {
                required = list_concat(required, pull_var_clause(expression, pull_flags));
            }
        }
        QualCost expression_cost{};
        cost_qual_eval(&expression_cost, projection_target->exprs, root);
        const double metadata_per_row =
            cpu_operator_cost *
            static_cast<double>(std::min<std::uint64_t>(builder.piece_boundaries, UINT32_MAX));
        const double boundary_per_row =
            cpu_operator_cost *
            static_cast<double>(std::min<std::uint64_t>(builder.planned_symbols, UINT32_MAX));

        postgres_list<Path> owners;
        postgres_list<Path> partial_owners;
        auto build_owner = [root,
                            rel,
                            projection_target,
                            required,
                            &builder,
                            &expression_cost,
                            metadata_per_row,
                            boundary_per_row](Path* source) -> Path* {
            PathTarget* child_target = copy_pathtarget(source->pathtarget);
            add_new_columns_to_pathtarget(child_target, required);
            set_pathtarget_cost_width(root, child_target);
            Path* child = slice_project_tag_source_child(source, child_target, builder);

            auto* owner = makeNode(CustomPath);
            owner->path.pathtype = T_CustomScan;
            owner->path.parent = rel;
            owner->path.pathtarget = copy_pathtarget(projection_target);
            owner->path.param_info = child->param_info;
            owner->path.parallel_aware = child->parallel_aware;
            owner->path.parallel_safe = child->parallel_safe;
            owner->path.parallel_workers = child->parallel_workers;
            owner->path.rows = child->rows;
            owner->path.startup_cost = child->startup_cost + expression_cost.startup;
            const double rows = std::max(0.0, child->rows);
            owner->path.total_cost =
                child->total_cost + expression_cost.startup +
                rows * (expression_cost.per_tuple + metadata_per_row + boundary_per_row);
            owner->path.pathkeys = child->pathkeys;
            owner->flags = CUSTOMPATH_SUPPORT_PROJECTION;
            owner->custom_paths = list_make1(child);
            owner->custom_private = list_make5(slice_project_header(builder),
                                               copyObject(builder.expressions),
                                               copyObject(builder.nodes),
                                               copyObject(builder.records),
                                               copyObject(builder.output_bindings));
            owner->custom_restrictinfo = NIL;
            owner->methods = &bioseq_slice_project_path_methods();
            return reinterpret_cast<Path*>(owner);
        };
        for (Path* source : postgres_list<Path>(rel->pathlist)) {
            if (!slice_project_is_bioseq_scan_path(source))
                continue;
            owners.append(build_owner(source));
        }
        for (Path* source : postgres_list<Path>(rel->partial_pathlist)) {
            if (!slice_project_is_bioseq_scan_path(source))
                continue;
            partial_owners.append(build_owner(source));
        }
        if (owners.is_empty())
            return;

        /* Gather paths are derived after this hook from reltarget.  Since every
         * surviving ordinary and partial path publishes the processed projection,
         * make that target canonical before core constructs Gather; otherwise it
         * asks a final-expression SliceProject child to publish the raw Var. */
        rel->reltarget = copy_pathtarget(projection_target);
        rel->pathlist = NIL;
        rel->partial_pathlist = NIL;
        rel->cheapest_startup_path = nullptr;
        rel->cheapest_total_path = nullptr;
        rel->cheapest_unique_path = nullptr;
        rel->cheapest_parameterized_paths = NIL;
        for (Path* owner : owners)
            add_path(rel, owner);
        for (Path* owner : partial_owners)
            add_partial_path(rel, owner);
    }

    struct slice_project_aggregate_recipe_presence {
        const bioseq_stub_oids* oids = nullptr;
        bool found = false;
    };

    struct slice_project_aggregate_recipe_presence_visitor {
        /* Stop at the first planner expression represented by the projection IR. */
        [[nodiscard]] pg_walk_step
        operator()(pg_expression_walk_cursor,
                   pg_node_view node,
                   slice_project_aggregate_recipe_presence& analysis) const noexcept {
            if (slice_project_node_opcode(node.get(), *analysis.oids).has_value()) {
                analysis.found = true;
                return pg_walk_step::stop;
            }
            return pg_walk_step::descend;
        }
    };

    /* Report whether an expression tree contains a recognized projection recipe. */
    [[nodiscard]] inline bool
    slice_project_aggregate_expression_has_recipe(Node* expression, const bioseq_stub_oids* oids) {
        if (expression == nullptr || oids == nullptr)
            return false;
        slice_project_aggregate_recipe_presence analysis{.oids = oids};
        slice_project_aggregate_recipe_presence_visitor visitor;
        walk_pg_expression_including_root(expression, analysis, visitor);
        return analysis.found;
    }

    struct slice_project_aggregate_argument_validation {
        const bioseq_stub_oids* oids = nullptr;
        Relids available_relids = nullptr;
        bool saw_sequence_var = false;
        bool invalid = false;
    };

    struct slice_project_aggregate_argument_visitor {
        /* Validate that every Var belongs to the aggregate input relation. */
        [[nodiscard]] pg_walk_step
        operator()(pg_expression_walk_cursor,
                   pg_node_view node,
                   slice_project_aggregate_argument_validation& validation) const noexcept {
            if (node.try_cast<PlaceHolderVar>() != nullptr || node.try_cast<Param>() != nullptr) {
                validation.invalid = true;
                return pg_walk_step::stop;
            }
            if (Var* var = node.try_cast<Var>()) {
                if (!bioseq_is_concrete_rte_varno(var->varno) ||
                    !bms_is_member(static_cast<int>(var->varno), validation.available_relids) ||
                    var->varattno <= 0 || var->varlevelsup != 0 ||
                    !bms_is_empty(var->varnullingrels)) {
                    validation.invalid = true;
                    return pg_walk_step::stop;
                }
                if (var->vartype == validation.oids->bioseq_seq_type_oid) {
                    validation.saw_sequence_var = true;
                }
                return pg_walk_step::skip_subtree;
            }
            return pg_walk_step::descend;
        }
    };

    /* Accept only parallel-safe aggregate inputs rooted in a sequence Var. */
    [[nodiscard]] inline bool
    slice_project_aggregate_argument_is_safe(PlannerInfo* root,
                                             Relids available_relids,
                                             Node* expression,
                                             const bioseq_stub_oids* oids,
                                             bool require_sequence_var = true) {
        if (root == nullptr || available_relids == nullptr || expression == nullptr ||
            oids == nullptr || contain_agg_clause(expression) ||
            contain_window_function(expression) || contain_subplans(expression) ||
            expression_returns_set(expression) || contain_volatile_functions(expression) ||
            !is_parallel_safe(root, expression))
            return false;
        slice_project_aggregate_argument_validation validation{
            .oids = oids, .available_relids = available_relids};
        slice_project_aggregate_argument_visitor visitor;
        walk_pg_expression_including_root(expression, validation, visitor);
        return !validation.invalid && (!require_sequence_var || validation.saw_sequence_var);
    }

    struct slice_project_aggregate_input_validation {
        PlannerInfo* root = nullptr;
        const bioseq_stub_oids* oids = nullptr;
        Relids available_relids = nullptr;
        List* candidates = NIL;
        List* input_expressions = NIL;
        List* visited_aggregates = NIL;
        bool saw_recipe = false;
        bool invalid = false;
    };

    /* Append one structurally distinct whole aggregate-argument expression. */
    inline void slice_project_add_unique_aggregate_candidate(
        slice_project_aggregate_input_validation& validation, Node* expression) {
        for (Node* candidate : postgres_list<Node>(validation.candidates)) {
            if (equal(candidate, expression))
                return;
        }
        validation.candidates = lappend(validation.candidates, expression);
    }

    /* Retain every ordinary argument expression so raw Vars needed by resjunk
     * ORDER BY carriers are published through the lower owner as well. */
    inline void
    slice_project_add_unique_aggregate_input(slice_project_aggregate_input_validation& validation,
                                             Node* expression) {
        for (Node* input : postgres_list<Node>(validation.input_expressions)) {
            if (equal(input, expression))
                return;
        }
        validation.input_expressions = lappend(validation.input_expressions, expression);
    }

    struct slice_project_aggregate_input_visitor {
        /* Collect movable whole arguments while enforcing aggregate barriers. */
        [[nodiscard]] pg_walk_step
        operator()(pg_expression_walk_cursor,
                   pg_node_view node,
                   slice_project_aggregate_input_validation& validation) const {
            if (node.get_tag() == T_GroupingFunc || node.get_tag() == T_WindowFunc ||
                node.get_tag() == T_SubPlan) {
                return pg_walk_step::skip_subtree;
            }
            Aggref* aggregate = node.try_cast<Aggref>();
            if (aggregate == nullptr)
                return pg_walk_step::descend;
            if (aggregate->agglevelsup != 0 ||
                list_member_ptr(validation.visited_aggregates, aggregate)) {
                return pg_walk_step::skip_subtree;
            }
            validation.visited_aggregates = lappend(validation.visited_aggregates, aggregate);

            List* aggregate_candidates = NIL;
            bool malformed_arguments = false;
            bool other_argument_is_unsafe = false;
            for (Node* raw_argument : postgres_list<Node>(aggregate->args)) {
                TargetEntry* argument = try_cast_pg_node<TargetEntry>(raw_argument);
                if (argument == nullptr || argument->expr == nullptr) {
                    malformed_arguments = true;
                    continue;
                }
                Node* expression = reinterpret_cast<Node*>(argument->expr);
                other_argument_is_unsafe =
                    other_argument_is_unsafe || contain_agg_clause(expression) ||
                    contain_window_function(expression) || contain_subplans(expression) ||
                    expression_returns_set(expression) || contain_volatile_functions(expression);
                if (slice_project_aggregate_expression_has_recipe(expression, validation.oids)) {
                    aggregate_candidates = lappend(aggregate_candidates, expression);
                }
            }
            bool barrier_has_recipe = false;
            for (Node* direct : postgres_list<Node>(aggregate->aggdirectargs)) {
                barrier_has_recipe =
                    barrier_has_recipe ||
                    slice_project_aggregate_expression_has_recipe(direct, validation.oids);
            }
            if (aggregate->aggfilter != nullptr) {
                barrier_has_recipe =
                    barrier_has_recipe ||
                    slice_project_aggregate_expression_has_recipe(
                        reinterpret_cast<Node*>(aggregate->aggfilter), validation.oids);
            }
            if (aggregate_candidates == NIL && !barrier_has_recipe) {
                return pg_walk_step::skip_subtree;
            }
            validation.saw_recipe = true;
            /* FILTER controls whether ordinary arguments are evaluated.  Direct
             * arguments and ordered-set/hypothetical aggregates have group-level
             * rather than per-input-row semantics.  None may cross this Agg node. */
            if (malformed_arguments || aggregate->aggkind != AGGKIND_NORMAL ||
                aggregate->aggdirectargs != NIL || aggregate->aggfilter != nullptr ||
                barrier_has_recipe || other_argument_is_unsafe) {
                validation.invalid = true;
                return pg_walk_step::stop;
            }
            /* Validate every ordinary input, including resjunk ORDER BY carriers,
             * against the same sole source relation.  The owner copies the complete
             * input PathTarget, so these expressions retain their original carrier
             * positions and sort-group wiring. */
            for (Node* raw_argument : postgres_list<Node>(aggregate->args)) {
                TargetEntry* argument = try_cast_pg_node<TargetEntry>(raw_argument);
                Node* expression = reinterpret_cast<Node*>(argument->expr);
                if (!slice_project_aggregate_argument_is_safe(validation.root,
                                                              validation.available_relids,
                                                              expression,
                                                              validation.oids,
                                                              false)) {
                    validation.invalid = true;
                    return pg_walk_step::stop;
                }
                slice_project_add_unique_aggregate_input(validation, expression);
            }
            for (Node* expression : postgres_list<Node>(aggregate_candidates)) {
                if (!slice_project_aggregate_argument_is_safe(validation.root,
                                                              validation.available_relids,
                                                              expression,
                                                              validation.oids)) {
                    validation.invalid = true;
                    return pg_walk_step::stop;
                }
                /* Publish the exact argument expression, never its TargetEntry.  The
                 * Aggref therefore retains its resno/ressortgroupref and aggorder
                 * wiring while setrefs maps only the argument to the owner output. */
                slice_project_add_unique_aggregate_candidate(validation, expression);
            }
            return pg_walk_step::skip_subtree;
        }
    };

    /* Resolve relation identity from the sole source-owning scan rather than an
     * upper SortPath's parent RelOptInfo (upper relations intentionally have no
     * base relids).  Only target-preserving unary containers may be crossed: this
     * keeps every ordered-aggregate carrier and its PathTarget sortgroupref intact. */
    [[nodiscard]] inline Relids slice_project_aggregate_source_relids(Path* path) noexcept {
        while (path != nullptr) {
            if (slice_project_is_bioseq_scan_path(path)) {
                return path->parent != nullptr && path->parent->relids != nullptr &&
                               bms_num_members(path->parent->relids) == 1
                           ? path->parent->relids
                           : nullptr;
            }
            Path* child = nullptr;
            if (IsA(path, ProjectionPath)) {
                child = reinterpret_cast<ProjectionPath*>(path)->subpath;
            } else if (IsA(path, SortPath)) {
                child = reinterpret_cast<SortPath*>(path)->subpath;
            } else if (IsA(path, IncrementalSortPath)) {
                child = reinterpret_cast<IncrementalSortPath*>(path)->spath.subpath;
            } else if (IsA(path, MaterialPath)) {
                child = reinterpret_cast<MaterialPath*>(path)->subpath;
            } else {
                return nullptr;
            }
            if (child == nullptr ||
                !slice_project_targets_match(path->pathtarget, child->pathtarget))
                return nullptr;
            path = child;
        }
        return nullptr;
    }

    /* Discover every supported projection argument consumed by one AggPath. */
    [[nodiscard]] inline slice_project_aggregate_input_validation
    slice_project_collect_aggregate_inputs(PlannerInfo* root,
                                           AggPath* aggregate,
                                           const bioseq_stub_oids* oids) {
        slice_project_aggregate_input_validation validation{
            .root = root,
            .oids = oids,
            .available_relids = aggregate != nullptr
                                    ? slice_project_aggregate_source_relids(aggregate->subpath)
                                    : nullptr,
        };
        if (aggregate == nullptr || aggregate->path.pathtarget == nullptr ||
            aggregate->path.pathtarget->exprs == NIL || validation.available_relids == nullptr) {
            validation.invalid = true;
            return validation;
        }
        slice_project_aggregate_input_visitor visitor;
        walk_pg_expression_including_root(
            reinterpret_cast<Node*>(aggregate->path.pathtarget->exprs), validation, visitor);
        if (!validation.invalid && aggregate->qual != NIL) {
            walk_pg_expression_including_root(
                reinterpret_cast<Node*>(aggregate->qual), validation, visitor);
        }
        return validation;
    }

    /* Report whether a path spine crosses a Gather or GatherMerge boundary. */
    [[nodiscard]] inline bool slice_project_path_tree_has_exchange(Path* path) noexcept {
        if (path == nullptr)
            return false;
        if (IsA(path, GatherPath) || IsA(path, GatherMergePath))
            return true;
        if (IsA(path, NestPath)) {
            auto* join = &reinterpret_cast<NestPath*>(path)->jpath;
            return slice_project_path_tree_has_exchange(join->outerjoinpath) ||
                   slice_project_path_tree_has_exchange(join->innerjoinpath);
        }
        if (IsA(path, MergePath)) {
            auto* join = &reinterpret_cast<MergePath*>(path)->jpath;
            return slice_project_path_tree_has_exchange(join->outerjoinpath) ||
                   slice_project_path_tree_has_exchange(join->innerjoinpath);
        }
        if (IsA(path, HashPath)) {
            auto* join = &reinterpret_cast<HashPath*>(path)->jpath;
            return slice_project_path_tree_has_exchange(join->outerjoinpath) ||
                   slice_project_path_tree_has_exchange(join->innerjoinpath);
        }
        if (IsA(path, CustomPath)) {
            for (Path* child :
                 postgres_list<Path>(reinterpret_cast<CustomPath*>(path)->custom_paths)) {
                if (slice_project_path_tree_has_exchange(child))
                    return true;
            }
            return false;
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
        }
        if (unary != nullptr)
            return slice_project_path_tree_has_exchange(unary);
        List* children = NIL;
        if (IsA(path, AppendPath)) {
            children = reinterpret_cast<AppendPath*>(path)->subpaths;
        } else if (IsA(path, MergeAppendPath)) {
            children = reinterpret_cast<MergeAppendPath*>(path)->subpaths;
        }
        for (Path* child : postgres_list<Path>(children)) {
            if (slice_project_path_tree_has_exchange(child))
                return true;
        }
        return false;
    }

    /* Build the lower SliceProject that materializes aggregate arguments lazily. */
    [[nodiscard]] inline Path* slice_project_build_aggregate_input_owner(
        PlannerInfo* root,
        AggPath* aggregate,
        slice_project_aggregate_input_validation const& validation) {
        Path* source = aggregate == nullptr ? nullptr : aggregate->subpath;
        Relids source_relids = slice_project_aggregate_source_relids(source);
        if (root == nullptr || source == nullptr || source->parent == nullptr ||
            source->param_info != nullptr || validation.candidates == NIL ||
            validation.oids == nullptr || source_relids == nullptr ||
            !bms_equal(source_relids, validation.available_relids) ||
            !slice_project_path_tree_has_bioseq_scan(source))
            return nullptr;

        PathTarget* lower_target = copy_pathtarget(source->pathtarget);
        /* Aggref arguments remain owned by the parent aggregate PathTarget.  Keep
         * only structural equality across the setrefs boundary: neither later
         * child-target processing nor the SliceProject codec may share those exact
         * mutable Node objects with the parent Aggref. */
        List* owned_candidates = static_cast<List*>(copyObject(validation.candidates));
        List* owned_inputs = static_cast<List*>(copyObject(validation.input_expressions));
        List* required = NIL;
        constexpr int pull_flags =
            PVC_RECURSE_AGGREGATES | PVC_RECURSE_WINDOWFUNCS | PVC_RECURSE_PLACEHOLDERS;
        for (Node* input : postgres_list<Node>(owned_inputs)) {
            required = list_concat(required, pull_var_clause(input, pull_flags));
        }
        /* The copied source target retains ordered carriers (notably the resjunk
         * id Var) and all existing sortgrouprefs.  Append only any missing raw Vars
         * on private copies before adding the exact lazy argument expressions. */
        add_new_columns_to_pathtarget(lower_target, static_cast<List*>(copyObject(required)));
        add_new_columns_to_pathtarget(lower_target, owned_candidates);
        set_pathtarget_cost_width(root, lower_target);
        slice_project_ir_builder builder{
            .oids = validation.oids,
            /* Aggregate inputs are internal executor values, not client output. */
            .command_type = CMD_UNKNOWN,
        };
        if (!slice_project_discover_target(lower_target, builder) ||
            !is_parallel_safe(root, reinterpret_cast<Node*>(lower_target->exprs)))
            return nullptr;

        PathTarget* support_target = copy_pathtarget(source->pathtarget);
        add_new_columns_to_pathtarget(support_target, static_cast<List*>(copyObject(required)));
        set_pathtarget_cost_width(root, support_target);

        Path* child = nullptr;
        if (slice_project_is_bioseq_scan_path(source)) {
            child = slice_project_tag_source_child(source, support_target, builder);
        } else {
            bool tagged_source = false;
            Path* private_child = slice_project_clone_mutable_path_spine(source);
            private_child =
                slice_project_tag_owned_path_tree(private_child, builder, tagged_source);
            if (!tagged_source)
                return nullptr;
            child = reinterpret_cast<Path*>(
                create_projection_path(root, private_child->parent, private_child, support_target));
        }
        if (child == nullptr || child->param_info != nullptr ||
            !pathkeys_contained_in(source->pathkeys, child->pathkeys) ||
            !pathkeys_contained_in(child->pathkeys, source->pathkeys)) {
            return nullptr;
        }

        const double rows = std::max(0.0, child->rows);
        const double metadata_per_row =
            cpu_operator_cost *
            static_cast<double>(std::min<std::uint64_t>(builder.piece_boundaries, UINT32_MAX));
        const double boundary_per_row =
            cpu_operator_cost *
            static_cast<double>(std::min<std::uint64_t>(builder.planned_symbols, UINT32_MAX));
        auto* owner = makeNode(CustomPath);
        owner->path.pathtype = T_CustomScan;
        owner->path.parent = source->parent;
        owner->path.pathtarget = lower_target;
        owner->path.param_info = child->param_info;
        owner->path.parallel_aware = child->parallel_aware;
        owner->path.parallel_safe = child->parallel_safe;
        owner->path.parallel_workers = child->parallel_workers;
        owner->path.rows = child->rows;
        /* AggPath already charges evaluation of its arguments.  This wrapper only
         * adds source-owner metadata overhead; charging the argument twice would
         * distort serial/partial aggregate selection. */
        owner->path.startup_cost = child->startup_cost;
        owner->path.total_cost = child->total_cost + rows * (metadata_per_row + boundary_per_row);
        owner->path.pathkeys = child->pathkeys;
        owner->flags = CUSTOMPATH_SUPPORT_PROJECTION;
        owner->custom_paths = list_make1(child);
        owner->custom_private = list_make5(slice_project_header(builder),
                                           copyObject(builder.expressions),
                                           copyObject(builder.nodes),
                                           copyObject(builder.records),
                                           copyObject(builder.output_bindings));
        owner->custom_restrictinfo = NIL;
        owner->methods = &bioseq_slice_project_path_methods();

        return reinterpret_cast<Path*>(owner);
    }

    struct slice_project_aggregate_path_rewrite {
        Path* path = nullptr;
        bool changed = false;
        bool unsafe = false;
    };

    /* Detect an eager semantic alternative that this AggPath rewrite cannot own. */
    [[nodiscard]] inline bool
    slice_project_non_aggregate_path_has_recipe(Path* path, const bioseq_stub_oids* oids) {
        if (path == nullptr || oids == nullptr)
            return false;
        if (path->pathtarget != nullptr && path->pathtarget->exprs != NIL &&
            slice_project_aggregate_expression_has_recipe(
                reinterpret_cast<Node*>(path->pathtarget->exprs), oids)) {
            return true;
        }
        List* quals = NIL;
        if (IsA(path, GroupPath)) {
            quals = reinterpret_cast<GroupPath*>(path)->qual;
        } else if (IsA(path, GroupingSetsPath)) {
            quals = reinterpret_cast<GroupingSetsPath*>(path)->qual;
        } else if (IsA(path, MinMaxAggPath)) {
            quals = reinterpret_cast<MinMaxAggPath*>(path)->quals;
        }
        return quals != NIL &&
               slice_project_aggregate_expression_has_recipe(reinterpret_cast<Node*>(quals), oids);
    }

    /* Clone one supported raw-input AggPath over its new SliceProject owner. */
    [[nodiscard]] inline slice_project_aggregate_path_rewrite slice_project_rewrite_aggregate_path(
        PlannerInfo* root, UpperRelationKind stage, Path* path, const bioseq_stub_oids* oids) {
        if (path == nullptr)
            return {.path = path};
        if (!IsA(path, AggPath)) {
            return {
                .path = path,
                .unsafe = slice_project_non_aggregate_path_has_recipe(path, oids),
            };
        }
        auto* aggregate = reinterpret_cast<AggPath*>(path);
        if (aggregate->path.pathtarget == nullptr) {
            return {.path = path, .unsafe = true};
        }
        if (DO_AGGSPLIT_COMBINE(aggregate->aggsplit))
            return {.path = path};
        const bool supported_split =
            (stage == UPPERREL_PARTIAL_GROUP_AGG &&
             aggregate->aggsplit == AGGSPLIT_INITIAL_SERIAL) ||
            (stage == UPPERREL_GROUP_AGG && aggregate->aggsplit == AGGSPLIT_SIMPLE);
        slice_project_aggregate_input_validation validation =
            slice_project_collect_aggregate_inputs(root, aggregate, oids);
        if (!validation.saw_recipe)
            return {.path = path};
        if (!supported_split || validation.invalid || validation.candidates == NIL ||
            (stage == UPPERREL_GROUP_AGG &&
             slice_project_path_tree_has_exchange(aggregate->subpath))) {
            return {.path = path, .unsafe = true};
        }
        Path* owner = slice_project_build_aggregate_input_owner(root, aggregate, validation);
        if (owner == nullptr)
            return {.path = path, .unsafe = true};

        auto* clone = makeNode(AggPath);
        *clone = *aggregate;
        clone->subpath = owner;
        const Cost startup_delta =
            (aggregate->aggstrategy == AGG_SORTED || aggregate->aggstrategy == AGG_MIXED)
                ? owner->startup_cost - aggregate->subpath->startup_cost
                : owner->total_cost - aggregate->subpath->total_cost;
        clone->path.startup_cost += startup_delta;
        clone->path.total_cost += owner->total_cost - aggregate->subpath->total_cost;
        return {
            .path = reinterpret_cast<Path*>(clone),
            .changed = true,
        };
    }

    struct slice_project_aggregate_path_list_rewrite {
        List* paths = NIL;
        bool changed = false;
        bool unsafe = false;
    };

    /* Copy-on-write rewrite one complete planner path list or reject it whole. */
    [[nodiscard]] inline slice_project_aggregate_path_list_rewrite
    slice_project_rewrite_aggregate_path_list(PlannerInfo* root,
                                              UpperRelationKind stage,
                                              List* paths,
                                              const bioseq_stub_oids* oids) {
        slice_project_aggregate_path_list_rewrite result;
        for (Path* path : postgres_list<Path>(paths)) {
            slice_project_aggregate_path_rewrite rewritten =
                slice_project_rewrite_aggregate_path(root, stage, path, oids);
            if (rewritten.unsafe) {
                result.unsafe = true;
                return result;
            }
            result.paths = lappend(result.paths, rewritten.path);
            result.changed = result.changed || rewritten.changed;
        }
        return result;
    }

    /* Atomically install safe aggregate-input owners and refresh cheapest paths. */
    inline void slice_project_try_install_aggregate_input_owners(PlannerInfo* root,
                                                                 UpperRelationKind stage,
                                                                 RelOptInfo* output_rel) {
        const bioseq_stub_oids* oids = try_get_cached_stub_oids();
        if (oids == nullptr)
            return;
        slice_project_aggregate_path_list_rewrite ordinary =
            slice_project_rewrite_aggregate_path_list(root, stage, output_rel->pathlist, oids);
        slice_project_aggregate_path_list_rewrite partial;
        if (stage == UPPERREL_PARTIAL_GROUP_AGG) {
            partial = slice_project_rewrite_aggregate_path_list(
                root, stage, output_rel->partial_pathlist, oids);
        }
        if (ordinary.unsafe || partial.unsafe || (!ordinary.changed && !partial.changed))
            return;
        if (ordinary.changed)
            output_rel->pathlist = ordinary.paths;
        if (partial.changed)
            output_rel->partial_pathlist = partial.paths;
        if (ordinary.changed) {
            output_rel->cheapest_startup_path = nullptr;
            output_rel->cheapest_total_path = nullptr;
            output_rel->cheapest_unique_path = nullptr;
            output_rel->cheapest_parameterized_paths = NIL;
            set_cheapest(output_rel);
        }
    }

    /* Install SliceProject ownership at the relevant upper-planner stages. */
    inline void bioseq_slice_project_create_upper_paths(PlannerInfo* root,
                                                        UpperRelationKind stage,
                                                        RelOptInfo* input_rel,
                                                        RelOptInfo* output_rel,
                                                        void*) {
        if (!bioseq::settings::planner::custom_scan::enabled || root == nullptr ||
            input_rel == nullptr || output_rel == nullptr || output_rel->reltarget == nullptr ||
            root->parse == nullptr || root->parse->hasRowSecurity ||
            root->parse->commandType != CMD_SELECT)
            return;
        if (stage == UPPERREL_PARTIAL_GROUP_AGG || stage == UPPERREL_GROUP_AGG) {
            slice_project_try_install_aggregate_input_owners(root, stage, output_rel);
            return;
        }
        if (!(stage == UPPERREL_WINDOW || stage == UPPERREL_DISTINCT || stage == UPPERREL_ORDERED ||
              stage == UPPERREL_FINAL))
            return;
        /*
         * PostgreSQL can defer the final SELECT projection until after the
         * UPPERREL_FINAL hook has run.  In that shape output_rel->reltarget still
         * describes the pass-through input (and a later core ProjectionPath becomes
         * the visible Result), while grouping_planner has already published the
         * stage's authoritative projection in root->upper_targets.  Discovering
         * recipes only in output_rel->reltarget therefore misses the common
         *
         *     Result (slice expression)
         *       -> BioseqScan (raw sequence Var)
         *
         * plan.  Build the owner against the same stage target core will apply
         * after this hook.  Falling back to the relation target preserves stages
         * for which core has no separate upper target.
         */
        PathTarget* projection_target = root->upper_targets[stage];
        if (projection_target == nullptr) {
            projection_target = output_rel->reltarget;
        }
        if (projection_target == nullptr ||
            slice_project_target_has_semantic_owner(root, projection_target))
            return;
        const bioseq_stub_oids* oids = try_get_cached_stub_oids();
        if (oids == nullptr)
            return;
        if (!is_parallel_safe(root, reinterpret_cast<Node*>(projection_target->exprs)))
            return;
        slice_project_ir_builder builder{.oids = oids, .command_type = root->parse->commandType};
        if (!slice_project_discover_target(projection_target, builder))
            return;
        for (Node* expression : postgres_list<Node>(projection_target->exprs)) {
            if (bioseq_expr_uses_lazy_sequence_projection(expression) &&
                contain_volatile_functions(expression))
                return;
        }
        Path* child = input_rel->cheapest_total_path;
        if (child == nullptr && input_rel->pathlist != NIL) {
            child = reinterpret_cast<Path*>(linitial(input_rel->pathlist));
        }
        if (child == nullptr)
            return;
        while (IsA(child, ProjectionPath) &&
               bioseq_expr_uses_lazy_sequence_projection(
                   reinterpret_cast<Node*>(child->pathtarget->exprs))) {
            child = reinterpret_cast<ProjectionPath*>(child)->subpath;
        }
        if (slice_project_is_owner_below_target_preserving_ordering(child, projection_target))
            return;

        List* required = NIL;
        constexpr int pull_flags =
            PVC_RECURSE_AGGREGATES | PVC_RECURSE_WINDOWFUNCS | PVC_RECURSE_PLACEHOLDERS;
        /* Only the owned recipe graph needs raw child inputs.  Ordinary projection
         * expressions are already represented by the chosen child's PathTarget and
         * must remain atomic here.  In particular, a raw BatchJoin output contract
         * publishes each match_result helper as one exact output identity;
         * descending through bioseq.query_start(t.pk) would synthesize a new t.pk
         * requirement above the owner even though that raw Var is intentionally
         * absent from its public target.  Core setrefs would then fail while binding
         * the intermediate ProjectionPath.  The IR expression list contains every
         * lazy producer,
         * source, concat operand, and bound expression, so pulling from it retains
         * the complete SliceProject dependency closure without opening non-lazy
         * helper identities. */
        for (Node* expression : postgres_list<Node>(builder.expressions)) {
            required = list_concat(required, pull_var_clause(expression, pull_flags));
        }
        PathTarget* child_target = copy_pathtarget(child->pathtarget);
        add_new_columns_to_pathtarget(child_target, required);
        set_pathtarget_cost_width(root, child_target);
        if (slice_project_is_bioseq_scan_path(child)) {
            child = slice_project_tag_source_child(child, child_target, builder);
        } else {
            /* A join/upper child is immutable and may be shared by many candidates.
             * Deep-copy supported core path shapes and tag every BioseqScan source
             * below them before installing hidden leaf outputs through core's
             * projection constructor.  The final target remains wrapper-only. */
            bool tagged_source = false;
            Path* private_child = slice_project_clone_mutable_path_spine(child);
            private_child =
                slice_project_tag_owned_path_tree(private_child, builder, tagged_source);
            if (!tagged_source)
                return;
            child = reinterpret_cast<Path*>(
                create_projection_path(root, private_child->parent, private_child, child_target));
        }

        QualCost expression_cost{};
        cost_qual_eval(&expression_cost, projection_target->exprs, root);
        const double rows = std::max(0.0, child->rows);
        const double metadata_per_row =
            cpu_operator_cost *
            static_cast<double>(std::min<std::uint64_t>(builder.piece_boundaries, UINT32_MAX));
        const double boundary_per_row =
            cpu_operator_cost *
            static_cast<double>(std::min<std::uint64_t>(builder.planned_symbols, UINT32_MAX));

        auto* path = makeNode(CustomPath);
        path->path.pathtype = T_CustomScan;
        path->path.parent = output_rel;
        path->path.pathtarget = copy_pathtarget(projection_target);
        path->path.param_info = child->param_info;
        path->path.parallel_aware = child->parallel_aware;
        path->path.parallel_safe = child->parallel_safe;
        path->path.parallel_workers = child->parallel_workers;
        path->path.rows = child->rows;
        path->path.startup_cost = child->startup_cost + expression_cost.startup;
        path->path.total_cost =
            child->total_cost + expression_cost.startup +
            rows * (expression_cost.per_tuple + metadata_per_row + boundary_per_row);
        path->path.pathkeys = child->pathkeys;
        path->flags = CUSTOMPATH_SUPPORT_PROJECTION;
        path->custom_paths = list_make1(child);
        path->custom_private = list_make5(slice_project_header(builder),
                                          builder.expressions,
                                          builder.nodes,
                                          builder.records,
                                          builder.output_bindings);
        path->custom_restrictinfo = NIL;
        path->methods = &bioseq_slice_project_path_methods();

        /* The core ProjectionPath remains fractionally cheaper because PostgreSQL's
         * generic cost model cannot represent avoided full-sequence hydration.  Do
         * not falsify either cost.  Once a complete source-tagged wrapper exists,
         * remove the incompatible unowned alternatives so selection is structural.
         * Every early/fallback return above leaves the original lists untouched. */
        output_rel->pathlist = NIL;
        output_rel->partial_pathlist = NIL;
        output_rel->cheapest_startup_path = nullptr;
        output_rel->cheapest_total_path = nullptr;
        output_rel->cheapest_unique_path = nullptr;
        output_rel->cheapest_parameterized_paths = NIL;
        add_path(output_rel, reinterpret_cast<Path*>(path));
        set_cheapest(output_rel);
    }

} // export custom-scan partition declarations
