/**
 * @file
 * @brief Declares BatchJoin and per-row plan construction without implementation fanout.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_join_plan_plan_construction_contracts;

export import :custom_scan_planner_path_contracts;

/** @brief Build the two-child BatchJoin plan and its serialized executor contract. */
export Plan* bioseq_batch_join_plan_custom_path(
    PlannerInfo* root,
    RelOptInfo* rel,
    CustomPath* best_path,
    List* tlist,
    List* clauses,
    List* custom_plans);
