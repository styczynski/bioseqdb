/**
 * @file
 * @brief Declares stable CustomScan executor method tables without importing implementations.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_execution_method_contracts;

/** @brief Returns the stable executor method table for base Bioseq scans. */
export [[nodiscard]] const CustomExecMethods* bioseq_column_scan_execute_methods() noexcept;
