/**
 * @file
 * @brief Collects declarations for base-scan execution and source-value lifetimes.
 */
export module bioseq.postgres.table:custom_scan_scan_runtime;

export import :custom_scan_scan_runtime_symbol_count_materialization_contracts;
export import :custom_scan_scan_runtime_source_lifecycle_contracts;
export import :custom_scan_scan_runtime_tuple_production_contracts;
export import :custom_scan_scan_runtime_execution_callbacks_contracts;
