/**
 * @file
 * @brief Declares base-scan path construction and conversion of selected paths to plans.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_planner_path_contracts;

/** @brief Builds and costs a Bioseq scan path with the requested outer-relation parameters. */
export Path* add_bioseq_column_scan_path(PlannerInfo* root,
                                         RelOptInfo* rel,
                                         RangeTblEntry* rte,
                                         Relids required_relids);

/** @brief Converts the selected Bioseq scan path into an executable CustomScan plan. */
export Plan* bioseq_column_scan_path_plan_custom_path(PlannerInfo* root,
                                                       RelOptInfo* rel,
                                                       CustomPath* best_path,
                                                       List* tlist,
                                                       List* clauses,
                                                       List* custom_plans);

/** @brief Converts the selected per-row join path into its executable CustomScan plan. */
export Plan* bioseq_per_row_join_plan_custom_path(PlannerInfo* root,
                                                   RelOptInfo* rel,
                                                   CustomPath* best_path,
                                                   List* tlist,
                                                   List* clauses,
                                                   List* custom_plans);
