/**
 * @file
 * @brief Clones supported PostgreSQL path containers and finds BioseqScan leaves.
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

export module bioseq.postgres.table:custom_scan_slice_rewrite_path_tree;

import std;
import bioseq.postgres.support;
export import :custom_scan_slice_rewrite_path_contracts;

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

    /* PostgreSQL copyObject does not support Path nodes. Copy the containers
     * that source tagging changes, while sharing expression metadata and leaf
     * paths that the tagger leaves alone. */
    [[nodiscard]] Path* slice_project_clone_mutable_path_spine(Path* path);

    /** @brief Copy the list and pass each child through the path-container copier. */
    [[nodiscard]] List* slice_project_clone_path_list(List* paths) {
        List* clones = NIL;
        for (Path* child : postgres_list<Path>(paths)) {
            clones = lappend(clones, slice_project_clone_mutable_path_spine(child));
        }
        return clones;
    }

    /** @brief Copy supported path containers, sharing expression metadata and other leaves. */
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
        return path;
    }

    /** @brief Search supported path children for a BioseqScan. */
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
        if (unary != nullptr)
            return slice_project_path_tree_has_bioseq_scan(unary);

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

} // export
