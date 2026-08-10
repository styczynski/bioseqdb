/**
 * @file
 * @brief Lightweight public declarations for the Bioseq table module.
 */
module;

#include <bioseq/pg_compat.h>
#include <string_view>

export module bioseq.postgres.table:api;

/*
 * Public table-module surface used outside bioseq.postgres.table.
 *
 * C++ module partitions are only importable by units of the same named module,
 * so external modules import bioseq.postgres.table and see this small API partition
 * re-exported by the primary interface. The heavy implementation partitions stay
 * out of the primary module's re-export graph for GCC 16.1.
 */
export const TableAmRoutine* column_table_access_method(void);
export void bioseq_tableam_init(void);

export const CustomScanMethods* bioseq_customscan_methods(void);
export bool bioseq_customscan_has_valid_semantic_payload(
    const CustomScan* scan) noexcept;
export List* bioseq_customscan_cache_private_payload(
    const CustomScan* scan);
export const CustomScanMethods* bioseq_batch_join_customscan_methods(void);
export bool bioseq_batch_join_customscan_has_valid_semantic_payload(
    const CustomScan* scan) noexcept;
export List* bioseq_batch_join_cache_plan_payload(
    const CustomScan* scan);
export List* bioseq_batch_join_cache_semantic_snapshot(
    const CustomScan* scan);
export bool bioseq_expr_uses_bioseq_projection(Node* node);
export void bioseq_customscan_init(void);
export void bioseq_validate_raw_mmsearch_options(Query* query);
export void bioseq_finalize_match_helper_bindings(PlannedStmt* statement);
export void bioseq_arm_slice_dsm_fixture_for_test(std::string_view mode);

export CustomScan* bioseq_create_index_scan_node(void);
export void bioseq_register_index_scan_node(void);
