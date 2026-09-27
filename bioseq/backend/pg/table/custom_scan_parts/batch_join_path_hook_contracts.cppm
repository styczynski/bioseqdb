/**
 * @file
 * @brief Planner hook for joins containing mmsearch.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_batch_join_path_hook_contracts;

/** Add or keep Bioseq plans that execute an eligible mmsearch inner join. */
export void bioseq_batch_join_set_join_pathlist_hook(
    PlannerInfo* root,
    RelOptInfo* joinrel,
    RelOptInfo* outerrel,
    RelOptInfo* innerrel,
    JoinType jointype,
    JoinPathExtraData* extra);
