/**
 * @file
 * @brief Clones and tags the mutable path spine and source routes.
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

export module bioseq.postgres.table:custom_scan_slice_rewrite_path_cloning;

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

    /** Returns true when @p path is a CustomPath whose CustomName is BioseqScan. */
    [[nodiscard]] bool slice_project_is_bioseq_scan_path(Path* path) {
        if (path == nullptr || !IsA(path, CustomPath))
            return false;
        auto* custom = reinterpret_cast<CustomPath*>(path);
        return custom->methods != nullptr && custom->methods->CustomName != nullptr &&
               strcmp(custom->methods->CustomName, "BioseqScan") == 0;
    }

    /** Returns true when @p path is a CustomPath using slice-project owner methods. */
    [[nodiscard]] bool slice_project_is_owner_path(Path* path) {
        if (path == nullptr || !IsA(path, CustomPath))
            return false;
        auto* custom = reinterpret_cast<CustomPath*>(path);
        return custom->methods == &bioseq_slice_project_path_methods();
    }

    /** Returns true when @p actual and @p expected share the same expression list and sortgrouprefs. */
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
    /* Genuine recursion cycle: the path clone delegates Append/MergeAppend lists,
     * whose elements recurse back through the path clone. */
    [[nodiscard]] Path* slice_project_clone_mutable_path_spine(Path* path);

    /** Clones every Path in @p paths through slice_project_clone_mutable_path_spine. */
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

    /** Copies a BioseqScan @p path, rebuilds BSS3 source tags from @p target, and returns the tagged leaf. */
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
        /** Recursively tags every path stored in @p subpaths. */
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

    /** Walks @p plan for lazy BioseqScan providers and adds each source route to @p routes. */
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

    /** Collects BioseqScan source routes from @p child into @p routes and returns true when any provider is found. */
    [[nodiscard]] bool
    batch_sort_attach_source_routes(PlanState* child,
                                    bioseq::storage::pg_sequence_source_route_set& routes) {
        std::uint32_t provider_count = 0u;
        batch_sort_collect_source_routes(child, routes, provider_count);
        return provider_count != 0u;
    }

} // export custom-scan partition declarations
