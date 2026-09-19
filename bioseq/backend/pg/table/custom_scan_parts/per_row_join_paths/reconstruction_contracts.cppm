/**
 * @file
 * @brief Declares per-row path reconstruction diagnostics without importing implementations.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_per_row_join_paths_reconstruction_contracts;

import std;

/** @brief Accumulates per-stage rejection and reconstruction counts for path admission. */
export struct per_row_reconstruction_rejections {
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

/** @brief Rebuild an inner join after transferring one owned predicate. */
export [[nodiscard]] Path* per_row_reconstruct_inner_join_path(
    PlannerInfo* root,
    RelOptInfo* joinrel,
    Path* incumbent,
    RestrictInfo* owned,
    const JoinPathExtraData& original_extra,
    PathTarget* required_target,
    List* required_vars,
    Index target_relid,
    per_row_reconstruction_rejections& rejects);

/** @brief Record a deterministic transformed-path admission decline. */
export [[nodiscard]] bool per_row_decline_transformed_path(const char* stage);
