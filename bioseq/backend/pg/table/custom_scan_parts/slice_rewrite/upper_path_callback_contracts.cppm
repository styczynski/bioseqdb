/**
 * @file
 * @brief Declares the planner callback that places SliceProject above scan and join paths.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_slice_rewrite_upper_path_callback_contracts;

/** @brief Add SliceProject paths when  this planner stage has supported sequence work. */
export void bioseq_slice_project_create_upper_paths(
    PlannerInfo* root,
    UpperRelationKind stage,
    RelOptInfo* input_rel,
    RelOptInfo* output_rel,
    void* extra);
