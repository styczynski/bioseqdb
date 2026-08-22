/**
 * @file
 * @brief Aggregates frozen semantics, callback contracts, and cache validation.
 */
export module bioseq.postgres.table:custom_scan_semantic_payload;

import :custom_scan_runtime_support;
import :custom_scan_oid_expression_support;

export import :custom_scan_semantic_payload_payload_codec;
export import :custom_scan_semantic_payload_aligner_semantics;
export import :custom_scan_semantic_payload_callback_contracts;
export import :custom_scan_semantic_payload_cache_validation;
