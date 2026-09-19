/**
 * @file
 * @brief Declares base SliceProject owner admission without importing its implementation.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_slice_rewrite_owner_admission_contracts;

/** @brief Install a complete base-relation SliceProject owner when admissible. */
export void slice_project_try_install_base_owner(
    PlannerInfo* root,
    RelOptInfo* rel,
    Relation relation);
