/**
 * @file
 * @brief Declares the Bioseq join-path hook without importing path construction.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_batch_join_path_hook_contracts;

/** @brief Extend one eligible inner join with Bioseq-owned join paths. */
export void bioseq_batch_join_set_join_pathlist_hook(
    PlannerInfo* root,
    RelOptInfo* joinrel,
    RelOptInfo* outerrel,
    RelOptInfo* innerrel,
    JoinType jointype,
    JoinPathExtraData* extra);
