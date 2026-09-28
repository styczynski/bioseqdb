/**
 * @file
 * @brief Public declarations for Bioseq table scans and planner hooks.
 */
module;

#include <bioseq/pg_compat.h>
#include <string_view>

export module bioseq.postgres.table:api;

export import :table_access_method_contracts;

/* Other modules import bioseq.postgres.table, which re-exports these
 * declarations. Keeping implementation partitions out of  that interface
 * avoids importing their dependencies into every caller. */
/** @brief Register Bioseq's table access method with PostgreSQL. */
export void bioseq_tableam_init(void);

/** @brief Return custom-scan methods for Bioseq sequence search. */
export const CustomScanMethods* bioseq_customscan_methods(void);
/** Check whether a sequence-search plan has valid saved execution settings. */
export bool bioseq_customscan_has_valid_semantic_payload(
    const CustomScan* scan) noexcept;
/** Copy sequence-search plan data for caching, removing statement-lpcal tokens. */
export List* bioseq_customscan_cache_private_payload(
    const CustomScan* scan);
/** @brief Return custom-scan methods for a Bioseq batch join. */
export const CustomScanMethods* bioseq_batch_join_customscan_methods(void);
/** Check whether a batch-join plan contains the fields required for execution. */
export bool bioseq_batch_join_customscan_has_valid_semantic_payload(
    const CustomScan* scan) noexcept;
/** Copy batch-join plan data for caching, removing statement-local tokens and costs. */
export List* bioseq_batch_join_cache_plan_payload(
    const CustomScan* scan);
/** Rebuild the batch join's saved alignment settings for its cache entry */
export List* bioseq_batch_join_cache_semantic_snapshot(
    const CustomScan* scan);
/** Check for a match-result helper, mmsearch, or symbol_count in an  expression. */
export bool bioseq_expr_uses_bioseq_projection(Node* node);
/** Install Bioseq planner hooks and register its custom scans. */
export void bioseq_customscan_init(void);
/** Reject volatile SQL functions in mmsearch options before planner simplification. */
export void bioseq_validate_raw_mmsearch_options(Query* query);
/** Connect match-result helpers to the scan that computes them in the completed plan. */
export void bioseq_finalize_match_helper_bindings(PlannedStmt* statement);
/** Select an incomplete or malformed shared-memory test fixture for SliceProject. */
export void bioseq_arm_slice_dsm_fixture_for_test(std::string_view mode);

/** Create an empty Bioseq index-scan node for the planner to, fill. */
export CustomScan* bioseq_create_index_scan_node(void);
/** @brief Register Bioseq's index-scan CustomScan node. */
export void bioseq_register_index_scan_node(void);
