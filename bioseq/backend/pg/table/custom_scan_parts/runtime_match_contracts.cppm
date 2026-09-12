/**
 * @file
 * @brief Defines stable match-owner and helper payload fields without importing runtimes.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_runtime_match_contracts;

export {
    /** @brief Defines stable fields in a serialized match-owner descriptor. */
    enum match_owner_descriptor_index : int {
        k_match_owner_magic = 0,
        k_match_owner_version = 1,
        k_match_owner_stable_id = 2,
        k_match_owner_target_recipe = 3,
        k_match_owner_base_lineage = 4,
        k_match_owner_base_relid = 5,
    };

    /** @brief Classifies how a match helper resolves to its finalized execution owner. */
    enum class match_helper_binding_kind : int {
        unresolved = 0,
        exact_recipe = 1,
        unique_lineage = 2,
    };

    inline constexpr int k_bioseq_match_helper_site_fields = 8;
    /** @brief Defines stable fields in one serialized match-helper call site. */
    enum match_helper_site_index : int {
        k_match_site_resno = 0,
        k_match_site_funcid = 1,
        k_match_site_typmod = 2,
        k_match_site_collation = 3,
        k_match_site_binding_kind = 4,
        k_match_site_canonical_argument = 5,
        k_match_site_phid = 6,
        k_match_site_stable_id = 7,
    };

    /** @brief Defines stable fields in a base-scan match-owner plan payload. */
    enum base_match_owner_plan_index : int {
        k_base_match_plan_magic = 0,
        k_base_match_plan_version = 1,
        k_base_match_plan_descriptor = 2,
        k_base_match_plan_sites = 3,
        k_base_match_plan_relation_oid = 4,
        k_base_match_plan_executor_descriptor = 5,
        k_base_match_plan_executor_sites = 6,
    };

    /** @brief Defines stable fields in a per-row join planner-to-executor payload. */
    enum per_row_join_private_index : int {
        k_per_row_version = 0,
        k_per_row_target_relation_oid = 1,
        k_per_row_target_storage_attno = 2,
        k_per_row_target_slot_resno = 3,
        k_per_row_query_slot_resno = 4,
        k_per_row_child_column_count = 5,
        k_per_row_alphabet_code = 6,
        k_per_row_storage_unit_byte_width = 7,
        k_per_row_query_alphabet_code = 8,
        k_per_row_query_storage_unit_byte_width = 9,
        k_per_row_target_typmod_erased = 10,
        k_per_row_query_typmod_erased = 11,
        k_per_row_placeholder_sites = 12,
        k_per_row_mmsearch_expr = 13,
        k_per_row_index_read_requirement = 14,
        k_per_row_honest_startup_cost = 15,
        k_per_row_honest_total_cost = 16,
        k_per_row_incumbent_cost_floor = 17,
        k_per_row_match_owner_descriptor = 18,
        k_per_row_target_source_tid_resno = 19,
        k_per_row_target_source_tableoid_resno = 20,
    };

    /** @brief Defines stable fields in a batched-join planner-to-executor payload. */
    enum batch_join_private_index : int {
        k_batch_join_version = 0,
        k_batch_join_token = 1,
        k_batch_join_target_relation_oid = 2,
        k_batch_join_target_storage_seq_attno = 3,
        k_batch_join_target_slot_seq_resno = 4,
        k_batch_join_target_slot_rowno_resno = 5,
        k_batch_join_query_slot_seq_resno = 6,
        k_batch_join_target_column_count = 7,
        k_batch_join_query_column_count = 8,
        k_batch_join_alphabet_code = 9,
        k_batch_join_storage_unit_byte_width = 10,
        k_batch_join_placeholder_sites = 11,
        k_batch_join_mmsearch_expr = 12,
        k_batch_join_index_read_requirement = 13,
        k_batch_join_honest_startup_cost = 14,
        k_batch_join_honest_total_cost = 15,
        k_batch_join_incumbent_cost_floor = 16,
        k_batch_join_match_owner_descriptor = 17,
    };

    /** @brief Identifies one match-result placeholder populated for a batched join row. */
    struct batch_join_placeholder_site {
        AttrNumber resno = InvalidAttrNumber;
        Oid funcid = InvalidOid;
        int32 typmod = -1;
        Oid collation = InvalidOid;
        match_helper_binding_kind binding_kind = match_helper_binding_kind::unresolved;
        int stable_match_id = 0;
    };
}
