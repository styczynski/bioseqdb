/**
 * @file alignment_sql_rows.cppm
 * @brief SQL columns returned by the alignment statistics function.
 */
module;

#include <bioseq/pg_compat.h>

#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>

export module bioseq.postgres.extension.alignment.sql_rows;

import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.support.sql_boundary;

export namespace bioseq::postgres::sql_rows {


/** @brief One  pipeline stage's statistics, with query totals repeated on each row. */
struct alignment_execution_stats_row final {
    std::int64_t execution_id;
    std::int32_t backend_pid;
    Oid database_oid;
    std::int32_t effective_cpu_workers;
    std::optional<std::string> gpu_device_uuid;
    std::optional<std::string> gpu_driver;
    std::optional<std::string> gpu_pipeline_cache_policy;
    std::optional<std::string> gpu_pipeline_cache_seed_sha256;
    std::optional<std::int64_t> gpu_pipeline_cache_seed_size;
    std::optional<std::string> gpu_pipeline_cache_manifest_checksum;
    std::optional<bool> gpu_pipeline_cache_hit;
    std::int32_t scan_node_count;
    std::int32_t nested_alignment_executor_count;
    bool is_completed;
    bool is_partial;
    bool is_fully_drained;
    std::string requested_backend;
    std::string actual_backend;
    std::string requested_target_index_source;
    std::string actual_target_index_source;
    std::int64_t persisted_generation_id;
    std::string persisted_source_identity;
    bool has_mixed_persisted_identity;
    std::string stage;
    std::int64_t eligible_jobs;
    std::int64_t executed_host_io_jobs;
    std::int64_t executed_scalar_cpu_jobs;
    std::int64_t executed_simd_cpu_jobs;
    std::int64_t executed_vulkan_gpu_jobs;
    std::int64_t fallback_jobs;
    std::int64_t stage_host_wall_ns;
    std::int64_t stage_gpu_device_ns;
    std::int64_t target_rows;
    std::int64_t attempted_alignment_jobs;
    std::int64_t fully_attested_alignment_jobs;
    std::int64_t unattested_alignment_jobs;
    std::optional<std::int64_t> first_unattested_target_row;
    std::optional<std::int64_t> first_unattested_query_ordinal;
    std::int64_t cpu_prepared_index_builds;
    std::int64_t cpu_prepared_index_reuses;
    std::int64_t cpu_prepared_unique_keys;
    std::int64_t cpu_prepared_duplicate_build_attempts;
    std::optional<std::string> cpu_prepared_key_set_checksum;
    std::optional<std::string> cpu_prepared_build_set_checksum;
    bool has_operational_identity_conflict;
    std::int64_t persisted_index_rows;
    std::int64_t missing_persisted_index_rows;
    std::int64_t recomputed_target_index_rows;
    std::int64_t persisted_generation_read_sessions;
    std::int64_t persisted_relation_opens;
    std::int64_t persisted_index_scans;
    std::int64_t persisted_head_scans;
    std::int64_t persisted_head_rows_read;
    std::int64_t persisted_head_bytes_read;
    std::int64_t persisted_manifest_directory_scans;
    std::int64_t persisted_manifest_rows_read;
    std::int64_t persisted_manifest_bytes_read;
    std::int64_t persisted_directory_chunks_read;
    std::int64_t persisted_directory_bytes_read;
    std::int64_t persisted_metadata_repeated_reads;
    std::int64_t persisted_metadata_heap_rows_read;
    std::int64_t persisted_metadata_bytes_read;
    std::int64_t persisted_payload_heap_rows_read;
    std::int64_t persisted_payload_bytes_read;
    std::int64_t persisted_heap_rows_read;
    std::int64_t persisted_toast_values_read;
    std::int64_t persisted_detoast_bytes;
    std::int64_t persisted_explicit_copy_bytes;
    std::int64_t persisted_wanted_subkeys;
    std::int64_t persisted_read_subkeys;
    std::int64_t persisted_wanted_extents;
    std::int64_t persisted_wanted_chunks;
    std::int64_t persisted_skipped_rows;
    std::int64_t persisted_repeated_chunk_reads;
    std::int64_t persisted_extents_read;
    std::int64_t persisted_chunks_read;
    std::int64_t persisted_bytes_read;
    std::int64_t persisted_bulk_read_calls;
    std::int64_t persisted_planned_request_groups;
    std::int64_t persisted_executed_request_groups;
    std::int64_t persisted_range_scan_calls;
    std::int64_t persisted_skip_budget_rows;
    std::int64_t persisted_hash_build_calls;
    std::int64_t persisted_assembly_ns;
    std::int64_t pair_sort_calls;
    std::int64_t host_pair_materialization_calls;
    std::int64_t host_biological_hash_filter_calls;
    std::int64_t host_query_hash_readbacks;
    std::int64_t host_query_symbol_packing_calls;
    std::int64_t host_target_symbol_packing_calls;
    std::int64_t host_alignment_job_classification_calls;
    std::int64_t host_anchor_concatenation_calls;
    std::int64_t host_cigar_construction_calls;
    std::int64_t full_segment_hydrations;
    std::int64_t nthash_vector_batches;
    std::int64_t nthash_scalar_backend_branches;
    std::int64_t nthash_scalar_tail_symbols;
    std::int64_t nthash_masked_tail_lanes;
    std::int64_t simd_chain_vector_batches;
    std::int64_t simd_chain_backtrack_vector_batches;
    std::int64_t simd_chain_scalar_backend_branches;
    std::int64_t simd_chain_backtrack_scalar_branches;
    std::int64_t cpu_seed_positions_rejected_outside_collection;
    std::int64_t gpu_partition_select_dispatches;
    std::int64_t gpu_partition_anchor_merge_dispatches;
    std::int64_t gpu_index_build_candidates;
    std::int64_t gpu_index_build_canonical_emitted;
    std::int64_t gpu_index_build_overlap_duplicates_rejected;
    std::int64_t gpu_index_build_noncanonical_extras_rejected;
    std::int64_t gpu_index_build_surviving_extras;
    std::int64_t gpu_persisted_resident_uploads;
    std::int64_t gpu_persisted_resident_cache_hits;
    std::int64_t gpu_overlap_anchor_duplicates;
    std::int64_t gpu_seed_dispatches;
    std::int64_t gpu_chain_dispatches;
    std::int64_t gpu_chain_backtrack_dispatches;
    std::int64_t gpu_extend_dispatches;
    std::int64_t gpu_target_window_request_dispatches;
    std::int64_t gpu_target_window_logical_units_requested;
    std::int64_t host_target_window_requests_consumed;
    std::int64_t host_target_window_physical_units_read;
    std::int64_t host_target_window_remap_units_filled;
    std::int64_t host_target_window_packed_bytes_read;
    std::int64_t gpu_runtime_init_ns;
    std::int64_t gpu_pipeline_cache_loads;
    std::int64_t gpu_pipeline_cache_seed_bytes;
    std::int64_t gpu_pipeline_cache_load_ns;
    std::int64_t gpu_pipeline_creation_ns;
    std::int64_t gpu_submit_cpu_ns;
    std::int64_t gpu_wait_cpu_ns;
    std::int64_t gpu_ordinal_readback_cpu_ns;
    std::int64_t gpu_result_readback_cpu_ns;
    std::int64_t gpu_device_critical_path_ns;
    std::int64_t gpu_timestamp_query_failures;
    std::int64_t gpu_queue_submits;
    std::int64_t gpu_host_waits;
    std::int64_t gpu_buffer_creates;
    std::int64_t gpu_memory_allocations;
    std::int64_t gpu_pipeline_creates;
    std::int64_t backend_declines;
    std::int64_t backend_fallbacks;
    bool are_stats_saturated;
    std::int64_t persisted_source_parent_rows_read;
    std::int64_t persisted_source_catalog_rows_read;
    std::int64_t persisted_source_catalog_index_scans;
    std::int64_t persisted_source_fragment_rows_read;
    std::int64_t persisted_source_decoded_bp;
    std::int64_t persisted_pair_index_conversion_calls;
    std::int64_t persisted_segment_rebase_calls;
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::jsonb,
    })]]
    bioseq::postgres::support::jsonb_value cold_index_preparation;
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::jsonb,
    })]]
    bioseq::postgres::support::jsonb_value simd_execution_stats;
};

static_assert(std::is_aggregate_v<alignment_execution_stats_row>);

}  // namespace bioseq::postgres::sql_rows
