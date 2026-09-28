/**
 * @file
 * @brief Declares SliceProject placement above a base-table scan.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_slice_rewrite_owner_admission_contracts;

/** @brief Wrap eligible base-table scans with SliceProject after checking expression placement. */
export void slice_project_try_install_base_owner(
    PlannerInfo* root,
    RelOptInfo* rel,
    Relation relation);
