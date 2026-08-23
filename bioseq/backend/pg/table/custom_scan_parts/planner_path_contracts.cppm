/**
 * @file
 * @brief Declares planner path construction callbacks without importing implementations.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_planner_path_contracts;

/** @brief Construct a Bioseq base-scan path for one required-outer relation set. */
export Path* add_bioseq_column_scan_path(PlannerInfo* root,
                                         RelOptInfo* rel,
                                         RangeTblEntry* rte,
                                         Relids required_relids);

/** @brief Convert the selected Bioseq scan path into its executable CustomScan plan. */
export Plan* bioseq_column_scan_path_plan_custom_path(PlannerInfo* root,
                                                       RelOptInfo* rel,
                                                       CustomPath* best_path,
                                                       List* tlist,
                                                       List* clauses,
                                                       List* custom_plans);

/** @brief Convert a selected per-row join path into its executable plan. */
export Plan* bioseq_per_row_join_plan_custom_path(PlannerInfo* root,
                                                   RelOptInfo* rel,
                                                   CustomPath* best_path,
                                                   List* tlist,
                                                   List* clauses,
                                                   List* custom_plans);
