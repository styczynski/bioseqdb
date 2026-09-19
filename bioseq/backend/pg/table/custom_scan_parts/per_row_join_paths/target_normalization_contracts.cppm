/**
 * @file
 * @brief Declares private path-spine target normalization.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_per_row_join_paths_target_normalization_contracts;

/** @brief Return methods for the unsupported-wrapper integration probe path. */
export [[nodiscard]] CustomPathMethods const&
match_helper_wrapper_probe_unknown_path_methods() noexcept;

/** @brief Initialize common Path fields for a wrapper integration probe. */
export void match_helper_wrapper_probe_init_path(Path* path,
                                                 NodeTag plan_tag,
                                                 RelOptInfo* parent,
                                                 PathTarget* target) noexcept;

/** @brief Allocate a minimal planner relation for wrapper integration probes. */
export [[nodiscard]] RelOptInfo*
match_helper_wrapper_probe_rel(RelOptKind kind, Index relid, Relids relids);

/** @brief Normalize a private path spine to its available helper carriers. */
export [[nodiscard]] bool per_row_normalize_private_path_spine(
    PlannerInfo* root,
    Path* path,
    List* candidate_helpers,
    List* validated_inherited_helpers,
    List* required_vars);
