/**
 * @file stripe.cppm
 * @brief Storage-id, stripe, chunk-group, and skip-list metadata operations.
 */
export module bioseq.postgres.storage.metadata:stripe;

import :core;

export using ::lookup_storage_id;
export using ::delete_metadata_rows;
export using ::delete_metadata_rows_for_stripe;
export using ::allocate_column_storage_id;
export using ::find_highest_used_address;
export using ::reserve_empty_stripe;
export using ::complete_stripe_reservation;
export using ::save_stripe_skip_list;
export using ::save_chunk_groups;
export using ::update_chunk_group_deleted_rows;
export using ::read_stripe_skip_list;
export using ::find_next_stripe_by_row_number;
export using ::find_stripe_by_row_number;
export using ::find_stripe_by_first_row_number;
export using ::find_next_stripe_for_parallel_worker;
export using ::stripe_write_state_for;
export using ::stripe_highest_row_number;
export using ::find_highest_row_number_stripe;
export using ::column_stripe_relation_oid;
export using ::column_stripe_primary_key_index_oid;
export using ::build_stripe_metadata;
export using ::list_stripes_for_relation;
export using ::column_table_row_count;
export using ::deleted_rows_for_stripe;
export using ::decompressed_length_for_stripe;
export using ::ensure_current_column_storage_format;
export using ::rewrite_stripe_metadata;
