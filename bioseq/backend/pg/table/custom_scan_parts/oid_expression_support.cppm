/**
 * @file
 * @brief Aggregates Bioseq OID catalog, expression, restriction, and semantic analysis.
 */
export module bioseq.postgres.table:custom_scan_oid_expression_support;

import :custom_scan_runtime_support;

export import :custom_scan_oid_expression_support_oid_catalog;
export import :custom_scan_oid_expression_support_expression_analysis;
export import :custom_scan_oid_expression_support_restriction_ownership;
export import :custom_scan_oid_expression_support_semantic_analysis;
