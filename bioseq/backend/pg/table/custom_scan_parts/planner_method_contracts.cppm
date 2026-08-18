/**
 * @file
 * @brief Declares stable planner method tables without importing their implementations.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_planner_method_contracts;

/** @brief Returns the path method table for base Bioseq scans. */
export [[nodiscard]] const CustomPathMethods* bioseq_column_scan_path_methods() noexcept;
/** @brief Returns the scan method table for base Bioseq scans. */
export [[nodiscard]] const CustomScanMethods* bioseq_column_scan_scan_methods() noexcept;
/** @brief Returns the path method table for batched joins. */
export [[nodiscard]] const CustomPathMethods* bioseq_batch_join_path_methods() noexcept;
/** @brief Returns the scan method table for batched joins. */
export [[nodiscard]] const CustomScanMethods* bioseq_batch_join_scan_methods() noexcept;
/** @brief Returns the path method table for per-row joins. */
export [[nodiscard]] const CustomPathMethods* bioseq_per_row_join_path_methods() noexcept;
