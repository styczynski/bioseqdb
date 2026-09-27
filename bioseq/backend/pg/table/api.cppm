/**
 * @file
 * @brief Lightweight public declarations for the Bioseq table module.
 */
module;

#include <bioseq/pg_compat.h>
#include <string_view>

export module bioseq.postgres.table:api;

export import :table_access_method_contracts;

/*
 * Public table-module surface used outside bioseq.postgres.table.
 *
 * C++ module partitions are only importable by units of the same named module,
 * so external modules import bioseq.postgres.table and see this small API partition
 * re-exported by the primary interface. The heavy implementation partitions stay
 * out of the primary module's re-export graph for GCC 16.1.
 */
/** @brief Register Bioseq's table access method with PostgreSQL. */
export void bioseq_tableam_init(void);

/** @brief Return custom-scan methods for Bioseq sequence search. */
export const CustomScanMethods* bioseq_customscan_methods(void);
/** @brief Check whether a sequence-search plan carries a valid semantic payload. */
export bool bioseq_customscan_has_valid_semantic_payload(
    const CustomScan* scan) noexcept;
/** @brief Return the cached private payload for a sequence-search plan. */
export List* bioseq_customscan_cache_private_payload(
    const CustomScan* scan);
/** @brief Return custom-scan methods for a Bioseq batch join. */
export const CustomScanMethods* bioseq_batch_join_customscan_methods(void);
/** @brief Check whether a batch-join plan carries a valid semantic payload. */
export bool bioseq_batch_join_customscan_has_valid_semantic_payload(
    const CustomScan* scan) noexcept;
/** @brief Return the cached plan payload for a batch join. */
export List* bioseq_batch_join_cache_plan_payload(
    const CustomScan* scan);
/** @brief Return the semantic cache snapshot for a batch-join plan. */
export List* bioseq_batch_join_cache_semantic_snapshot(
    const CustomScan* scan);
/** @brief Report whether an expression uses a Bioseq projection. */
export bool bioseq_expr_uses_bioseq_projection(Node* node);
/** @brief Register Bioseq custom-scan methods with PostgreSQL. */
export void bioseq_customscan_init(void);
/** @brief Reject unsupported raw mmsearch options in a parsed query. */
export void bioseq_validate_raw_mmsearch_options(Query* query);
/** @brief Bind match-helper references after PostgreSQL planning. */
export void bioseq_finalize_match_helper_bindings(PlannedStmt* statement);
/** @brief Arm the named DSM fixture mode for a test execution. */
export void bioseq_arm_slice_dsm_fixture_for_test(std::string_view mode);

/** @brief Create a CustomScan node for a Bioseq index scan. */
export CustomScan* bioseq_create_index_scan_node(void);
/** @brief Register Bioseq's index-scan CustomScan node. */
export void bioseq_register_index_scan_node(void);
