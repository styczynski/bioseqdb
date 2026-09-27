/**
 * @file
 * @brief Declares child-path output adjustment and test fixtures for path wrappers.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_per_row_join_paths_target_normalization_contracts;

/** @brief Returns a test-only custom-path method table that the wrapper checks must reject. */
export [[nodiscard]] CustomPathMethods const&
match_helper_wrapper_probe_unknown_path_methods() noexcept;

/** @brief Initializes a serial, one-row path for the wrapper tests. */
export void match_helper_wrapper_probe_init_path(Path* path,
                                                 NodeTag plan_tag,
                                                 RelOptInfo* parent,
                                                 PathTarget* target) noexcept;

/** @brief Creates a minimal planner relation with an empty target for the wrapper tests. */
export [[nodiscard]] RelOptInfo*
match_helper_wrapper_probe_rel(RelOptKind kind, Index relid, Relids relids);

/** @brief Adjusts a private path tree to the helper results available at each node. */
export [[nodiscard]] bool per_row_normalize_private_path_spine(
    PlannerInfo* root,
    Path* path,
    List* candidate_helpers,
    List* validated_inherited_helpers,
    List* required_vars);
