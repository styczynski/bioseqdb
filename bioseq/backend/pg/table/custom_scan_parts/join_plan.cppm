/**
 * @file
 * @brief Aggregates join placeholder, setrefs, and plan-construction contracts.
 */
export module bioseq.postgres.table:custom_scan_join_plan;

export import :custom_scan_join_plan_placeholder_registration_contracts;
export import :custom_scan_join_plan_setrefs_contracts;
export import :custom_scan_join_plan_plan_construction_contracts;
export import :custom_scan_planner_method_contracts;
export import :custom_scan_match_finalization_contracts;
