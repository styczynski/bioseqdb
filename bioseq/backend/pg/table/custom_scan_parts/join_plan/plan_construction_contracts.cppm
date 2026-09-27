/**
 * @file
 * @brief Declares the BatchJoin plan builder and shared planner callbacks.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_join_plan_plan_construction_contracts;

export import :custom_scan_planner_path_contracts;

/** @brief Builds the BatchJoin plan with target and query children and saved executor settings. */
export Plan* bioseq_batch_join_plan_custom_path(
    PlannerInfo* root,
    RelOptInfo* rel,
    CustomPath* best_path,
    List* tlist,
    List* clauses,
    List* custom_plans);
