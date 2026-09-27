/**
 * @file
 * @brief Declares slice path cloning and ownership tagging.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_slice_rewrite_path_contracts;

/** @brief Clone every mutable node in a supported planner path spine. */
export [[nodiscard]] Path* slice_project_clone_mutable_path_spine(Path* path);

/** @brief Clone every path in a list through the mutable path-spine cloner. */
export [[nodiscard]] List* slice_project_clone_path_list(List* paths);

/** @brief Report whether a path tree contains a Bioseq scan source. */
export [[nodiscard]] bool slice_project_path_tree_has_bioseq_scan(Path* path) noexcept;
