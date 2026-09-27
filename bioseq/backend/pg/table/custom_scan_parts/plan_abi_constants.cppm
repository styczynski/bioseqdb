/**
 * @file
 * @brief Defines stable field counts and version tags for custom-scan plan payloads.
 */
export module bioseq.postgres.table:custom_scan_plan_abi_constants;

/** @brief Version of the two-child batched-join planner payload. */
export inline constexpr int k_bioseq_batch_join_plan_version = 3;
/** @brief Magic tag for locator passthrough metadata. */
export inline constexpr int k_bioseq_locator_passthrough_magic = 0x424c5031;
/** @brief Magic tag for a serialized slice source. */
export inline constexpr int k_bioseq_slice_source_magic = 0x42535331;
/** @brief Version of the serialized slice-source payload. */
export inline constexpr int k_bioseq_slice_source_version = 3;
/** @brief Magic tag for a serialized mmsearch owner. */
export inline constexpr int k_bioseq_mmsearch_owner_magic = 0x424d4f31;
/** @brief Version of the serialized mmsearch-owner payload. */
export inline constexpr int k_bioseq_mmsearch_owner_version = 2;
/** @brief Magic tag for a base match-owner plan. */
export inline constexpr int k_bioseq_base_match_owner_plan_magic = 0x424d5031;
/** @brief Version of a base match-owner plan. */
export inline constexpr int k_bioseq_base_match_owner_plan_version = 2;
/** @brief Number of fields in a base match-owner plan. */
export inline constexpr int k_bioseq_base_match_owner_plan_fields = 7;
/** @brief Number of private fields in a batched-join path. */
export inline constexpr int k_bioseq_batch_join_path_private_fields = 13;
/** @brief Number of private fields in a batched-join plan. */
export inline constexpr int k_bioseq_batch_join_private_fields = 18;
/** @brief Version of the one-child per-row join planner payload. */
export inline constexpr int k_bioseq_per_row_join_plan_version = 5;
/** @brief Number of private fields in a per-row join path. */
export inline constexpr int k_bioseq_per_row_join_path_private_fields = 13;
/** @brief Number of private fields in a per-row join plan. */
export inline constexpr int k_bioseq_per_row_join_private_fields = 21;
/** @brief Magic tag for a match-owner descriptor. */
export inline constexpr int k_bioseq_match_owner_descriptor_magic = 0x424d4231;
/** @brief Version of a match-owner descriptor. */
export inline constexpr int k_bioseq_match_owner_descriptor_version = 1;
/** @brief Number of fields in a match-owner descriptor. */
export inline constexpr int k_bioseq_match_owner_descriptor_fields = 6;
