/**
 * @file
 * @brief Declares the SliceProject upper-path callback without importing its implementation.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_slice_rewrite_upper_path_callback_contracts;

/** @brief Install SliceProject ownership at the relevant upper-planner stage. */
export void bioseq_slice_project_create_upper_paths(
    PlannerInfo* root,
    UpperRelationKind stage,
    RelOptInfo* input_rel,
    RelOptInfo* output_rel,
    void* extra);
