/**
 * @file
 * @brief Declares path copying and searches for BioseqScan sources.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_slice_rewrite_path_contracts;

/** @brief Copy the supported path containers that source tagging may change. */
export [[nodiscard]] Path* slice_project_clone_mutable_path_spine(Path* path);

/** @brief Copy each path in a list using the supported path-container copier. */
export [[nodiscard]] List* slice_project_clone_path_list(List* paths);

/** @brief Search supported path children for a BioseqScan. */
export [[nodiscard]] bool slice_project_path_tree_has_bioseq_scan(Path* path) noexcept;
