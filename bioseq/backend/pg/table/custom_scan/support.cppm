/**
 * @file
 * @brief Aggregates shared runtime, catalog, expression, and semantic custom-scan support.
 */
export module bioseq.postgres.table:custom_scan_support;

export import :custom_scan_runtime_support;
export import :custom_scan_oid_expression_support;
export import :custom_scan_semantic_payload;
