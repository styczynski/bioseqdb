/**
 * @file sql_rows.cppm
 * @brief Declares typed SQL rows returned by extension diagnostics and fixtures.
 */
module;

extern "C" {
#include "postgres.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>

export module bioseq.postgres.extension.sql_rows;

import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.support.sql_boundary;

export namespace bioseq::postgres::sql_rows {

/** @brief Reports Vulkan runtime availability and device identity. */
struct vulkan_runtime_status_row final {
    bool is_available;
    std::string device_name;
    std::int32_t max_workgroup_count_x;
    bool has_scalar_block_layout;
    std::string initialization_error;
    std::int64_t device_local_bytes;
    bool is_software_device;
};

/** @brief Reports one sequence relation metadata entry. */
struct list_metadata_row final {
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::regclass,
    })]]
    Oid table_name;
    std::int16_t column_attno;
    std::string column_name;
    std::string index_name;
    std::string index_type;
    std::string update_policy;
    std::string storage_size;
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::interval,
    })]]
    std::optional<bioseq::postgres::support::interval_value> time_since_update;
};

/** @brief Describes the resolved active read policy for one indexed column. */
struct active_read_policy_debug_row final {
    std::int64_t resolver_configured_rows;
    std::int64_t resolver_authoritative_defaults;
    std::int64_t resolver_non_relation;
    std::int64_t dispatch_configured_batches;
    std::int64_t dispatch_authoritative_default_batches;
    std::int64_t dispatch_non_relation_batches;
    std::string last_dispatch_source;
    std::int64_t prohibited_file_constructor_entries;
    std::int64_t prohibited_file_writer_entries;
};

/** @brief Reports the last minimizer-sequence build in this backend. */
struct last_minimizer_sequence_build_stats_row final {
    std::int64_t backend_pid;
    std::int64_t db_oid;
    std::int64_t relfilenode;
    std::int64_t sequence_rowno;
    std::int64_t total_run_count;
    std::int64_t resident_runs;
    std::int64_t file_runs;
    std::int64_t resident_run_bytes;
    std::int64_t resident_peak_bytes;
    std::int64_t spilled_run_bytes;
    std::int64_t postgres_temp_file_bytes;
    std::int64_t postgres_temp_file_count;
    std::int64_t merge_reader_buffer_bytes;
    std::int64_t merge_pass_count;
    std::int64_t spill_count_from_pressure;
    std::int64_t resident_demotions;
    std::int64_t position_spill_bytes;
    std::int64_t position_spill_count;
};

/** @brief Reports scheduler-wide minimizer-sequence build counters. */
struct scheduler_minimizer_sequence_build_stats_row final {
    std::int64_t backend_pid;
    std::int64_t db_oid;
    std::int64_t relfilenode;
    std::int64_t sequence_rowno;
    std::int64_t total_run_count;
    std::int64_t resident_runs;
    std::int64_t file_runs;
    std::int64_t resident_run_bytes;
    std::int64_t resident_peak_bytes;
    std::int64_t spilled_run_bytes;
    std::int64_t postgres_temp_file_bytes;
    std::int64_t postgres_temp_file_count;
    std::int64_t merge_reader_buffer_bytes;
    std::int64_t merge_pass_count;
    std::int64_t spill_count_from_pressure;
    std::int64_t resident_demotions;
    std::int64_t position_spill_bytes;
    std::int64_t position_spill_count;
    std::int64_t generation;
    std::int64_t worker_build_ns;
    std::int64_t pipeline_mode;
    std::int64_t max_concurrency;
    std::int64_t index_memory_cap_mb;
    std::int64_t shareable_lazy_workers;
};

/** @brief Reports worker-boundary success and failure counters. */
struct async_minimizer_sequence_worker_boundary_stats_row final {
    std::int64_t relation_leaf_calls;
    std::int64_t spi_leaf_calls;
    std::int64_t materialize_interrupt_leaf_calls;
    std::int64_t materialize_scanner_read_leaf_calls;
    std::int64_t pg_leaf_failures;
    std::int64_t injected_pg_leaf_failures;
    std::int64_t materialize_admissions;
    std::int64_t materialize_failure_unwinds;
    std::int64_t materialize_admission_restored;
    std::int64_t materialize_admission_mismatches;
    std::string last_failure_point;
    std::string armed_failure_point;
    std::string last_failure_sqlstate;
    std::string last_failure_message;
};

/** @brief Reports queue, worker, and persistence scheduler counters. */
struct scheduler_stats_row final {
    bool is_supervisor;
    std::int32_t config_generation;
    std::string pipeline_concurrency_mode;
    std::int32_t min_threads;
    std::int32_t max_threads;
    std::int32_t current_threads;
    std::int64_t batches_scheduled;
    std::int64_t batches_awaiting;
    std::int64_t batches_processed;
    std::int64_t queries_waiting;
    std::int64_t queries_processed;
    double oldest_waiting_batch_age_secs;
    double queries_per_sec;
    double uptime_secs;
    std::int64_t async_persist_jobs_waiting;
    std::int64_t index_memory_reserved_bytes;
};

/** @brief Reports the cold-read absence state for one sequence index. */
struct inspect_cold_no_index_absence_row final {
    std::int64_t target_rows;
    bool is_target_cardinality_metadata_only;
    std::int64_t source_parent_rows_read;
    std::int64_t source_fragment_rows_read;
    std::int64_t source_decoded_bp;
    std::int64_t physical_index_relations;
    std::int64_t persisted_index_rows;
    std::int64_t generation_objects;
};

/** @brief Reports the ready minimizer generation selected for inspection. */
struct inspect_ready_minimizer_generation_row final {
    std::int64_t generation_id;
    std::string source_identity;
    std::string compatibility_checksum;
    std::string source_content_checksum;
    Oid database_oid;
    Oid main_relfilenode;
    std::int16_t column_attno;
    std::int64_t sequence_rowno;
    std::int16_t locator_revision;
    std::int16_t bucket_idx;
    std::int32_t shard_idx;
    std::int64_t num_fragments;
    std::int64_t total_bp_size;
    std::int64_t first_sub_row_no;
    std::int32_t padding_letters;
    std::int16_t alphabet_code;
    std::int16_t storage_unit_byte_width;
    std::int16_t kmer_size;
    std::int16_t window_size;
    std::int32_t gpu_abi_revision;
    std::int16_t segment_geometry_revision;
    std::int64_t position_segment_span_bp;
    std::int32_t position_segment_count;
};

/** @brief Reports one generated minimizer fixture and its identity. */
struct build_minimizer_fixture_row final {
    std::string sidecar_table;
    std::int16_t layout;
    std::int64_t generation_id;
    std::string source_identity;
    std::string source_content_checksum;
    std::string compatibility_checksum;
    std::int64_t source_num_fragments;
    std::int64_t source_total_bp;
    std::int64_t source_padding_letters;
    std::int64_t writer_rows;
    std::int64_t writer_batches;
    std::int64_t published_head_bytes;
    std::int64_t control_count;
    std::int64_t position_count;
    std::int64_t directory_entries;
    std::int64_t position_segment_count;
    std::int64_t control_encoded_bytes;
    std::int64_t position_encoded_bytes;
    std::int64_t control_chunks;
    std::int64_t position_chunks;
};

/** @brief Reports one minimizer fixture read and validation result. */
struct read_minimizer_fixture_row final {
    std::string sidecar_table;
    std::int16_t layout;
    std::int64_t generation_id;
    std::string source_identity;
    std::string source_content_checksum;
    std::string compatibility_checksum;
    std::int64_t source_num_fragments;
    std::int64_t source_total_bp;
    std::int64_t source_padding_letters;
    std::int64_t validated_control_bytes;
    std::int64_t validated_position_bytes;
    std::string validated_control_checksum;
    std::string validated_position_checksum;
    std::int64_t lookup_count;
    std::int64_t lookup_bp;
    bool is_lookup_reverse;
    bool are_lookup_positions_complete;
    std::int64_t relation_opens;
    std::int64_t index_scans;
    std::int64_t head_scans;
    std::int64_t manifest_directory_scans;
    std::int64_t planned_request_groups;
    std::int64_t executed_request_groups;
    std::int64_t bulk_read_calls;
    std::int64_t range_scan_calls;
    std::int64_t payload_wanted_extents;
    std::int64_t payload_read_extents;
    std::int64_t payload_wanted_chunks;
    std::int64_t payload_delivered_chunks;
    std::int64_t payload_delivered_bytes;
    std::int64_t payload_heap_rows_read;
    std::int64_t payload_skipped_rows;
    std::int64_t toast_values_read;
    std::int64_t detoast_bytes;
    std::int64_t cache_lookups;
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::jsonb,
    })]]
    bioseq::postgres::support::jsonb_value selective_cold_index_preparation;
};

/** @brief Reports one grouped-SoA generation scan performed by a test routine. */
struct scan_grouped_soa_minimizer_generation_test_row final {
    std::string sidecar_table;
    std::string selected_backend;
    std::int64_t generation_id;
    std::int64_t control_bytes;
    std::int64_t position_bytes;
    std::string control_checksum;
    std::string position_checksum;
    std::int64_t relation_opens;
    std::int64_t index_scans;
    std::int64_t range_scan_calls;
    std::int64_t payload_heap_rows;
    std::int64_t delivered_chunks;
    std::int64_t delivered_bytes;
    std::int64_t planned_extents;
    std::int64_t completed_extents;
    std::int64_t scan_calls;
};

/** @brief Reports storage state for one persisted index generation. */
struct generation_storage_state_row final {
    std::optional<Oid> before_oid;
    std::optional<Oid> operation_oid;
    std::optional<Oid> after_oid;
    std::optional<bool> is_same_handle_rebound;
    std::optional<bool> has_generation_schema_after;
    std::optional<bool> is_chunk_cache_hit;
    std::optional<bool> is_assembled_cache_hit;
};

/** @brief Reports a ready minimizer-fragment generation built by a fixture. */
struct build_ready_minimizer_fragment_generation_row final {
    std::string sidecar_table;
    std::int16_t column_attno;
    std::int64_t source_sequence_rowno;
    std::string source_epoch;
    std::int64_t source_num_fragments;
    std::int64_t source_total_bp;
    std::string selected_backend;
    std::int16_t layout;
    bool is_published;
    bool is_head_manifest_verified;
    std::int64_t generation_id;
    std::string source_identity;
    std::string source_content_checksum;
    std::string compatibility_checksum;
    std::int64_t manifest_target_length;
    std::int16_t kmer_size;
    std::int16_t window_size;
    std::int32_t minimizer_scheme_id;
    std::int32_t minimizer_scheme_revision;
    std::int32_t gpu_abi_revision;
    std::int64_t position_segment_span_bp;
    std::int32_t position_segment_count;
    std::int64_t writer_rows;
    std::int64_t writer_batches;
    std::int64_t control_count;
    std::int64_t position_count;
    std::int64_t producer_scan_count;
    std::int64_t geometry_retry_count;
    std::int64_t short_core_runs;
    std::int64_t scalar_core_runs;
    std::int64_t exact_simd_core_runs;
    std::int64_t bounded_nthash_core_runs;
    std::int64_t vulkan_core_runs;
    bool is_execution_accepted;
    bool are_exact_simd_stats_available;
    bool are_exact_simd_stats_consistent;
    std::int64_t exact_simd_vector_batches;
    std::int64_t exact_simd_scalar_backend_branches;
    bool are_bounded_nthash_stats_available;
    std::string bounded_nthash_status;
    bool are_bounded_nthash_stats_consistent;
    std::int64_t bounded_nthash_packed_decode_batches;
    std::int64_t bounded_nthash_fallback_calls;
    std::int64_t vulkan_completed_roundtrips;
    std::int64_t vulkan_downloaded_minimizers;
};

/** @brief Reports a ready minimizer-sequence generation built by a fixture. */
struct build_ready_minimizer_sequence_generation_row final {
    std::string sidecar_table;
    std::int16_t column_attno;
    std::int64_t source_sequence_rowno;
    std::string source_epoch;
    std::int64_t source_num_fragments;
    std::int64_t source_total_bp;
    std::string selected_backend;
    std::int16_t layout;
    bool is_published;
    bool is_head_manifest_verified;
    std::int64_t generation_id;
    std::string source_identity;
    std::string source_content_checksum;
    std::string compatibility_checksum;
    std::int64_t manifest_target_length;
    std::int16_t kmer_size;
    std::int16_t window_size;
    std::int32_t minimizer_scheme_id;
    std::int32_t minimizer_scheme_revision;
    std::int32_t gpu_abi_revision;
    std::int64_t position_segment_span_bp;
    std::int32_t position_segment_count;
    std::int64_t writer_rows;
    std::int64_t writer_batches;
    std::int64_t control_count;
    std::int64_t position_count;
    std::int64_t spill_run_count;
    std::int64_t spilled_event_count;
    std::int64_t spill_operation_epoch;
    std::int64_t spill_generation_token;
    std::int64_t spill_mutation_revision;
    std::int64_t spill_qualification_scan_count;
    std::int64_t spill_qualification_merge_count;
    std::int64_t configured_core_span_bp;
    std::int64_t peak_core_span_bp;
    std::int64_t peak_core_events;
    std::int64_t short_core_runs;
    std::int64_t scalar_core_runs;
    std::int64_t exact_simd_core_runs;
    std::int64_t bounded_nthash_core_runs;
    std::int64_t vulkan_core_runs;
    bool is_execution_accepted;
    bool are_exact_simd_stats_available;
    bool are_exact_simd_stats_consistent;
    std::int64_t exact_simd_vector_batches;
    std::int64_t exact_simd_scalar_backend_branches;
    bool are_bounded_nthash_stats_available;
    std::string bounded_nthash_status;
    bool are_bounded_nthash_stats_consistent;
    std::int64_t bounded_nthash_packed_decode_batches;
    std::int64_t bounded_nthash_fallback_calls;
    std::int64_t vulkan_completed_roundtrips;
    std::int64_t vulkan_downloaded_minimizers;
    std::int64_t replay_scan_count;
    std::int64_t replay_merge_pass_count;
    std::int64_t replay_reroute_pass_count;
    std::int64_t replay_completed_scan_count;
    std::int64_t replay_failed_scan_count;
    std::int64_t replay_binding_checked_failed_scan_count;
    std::int64_t replay_illegal_span_transition_count;
    std::int64_t replay_first_completed_event_count;
    std::int64_t replay_last_completed_event_count;
    std::int64_t replay_final_span_bp;
    std::int64_t replay_final_generation_token;
    std::int64_t replay_final_mutation_revision;
    std::int64_t replay_visited_event_count;
    std::int64_t manifest_position_count;
    std::int64_t manifest_position_segment_span_bp;
    std::int64_t release_generation_token;
    std::int64_t release_mutation_before;
    std::int64_t release_mutation_after;
    bool are_spill_resources_released;
};

static_assert(std::is_aggregate_v<vulkan_runtime_status_row>);
static_assert(std::is_aggregate_v<list_metadata_row>);
static_assert(std::is_aggregate_v<active_read_policy_debug_row>);
static_assert(std::is_aggregate_v<last_minimizer_sequence_build_stats_row>);
static_assert(std::is_aggregate_v<scheduler_minimizer_sequence_build_stats_row>);
static_assert(std::is_aggregate_v<async_minimizer_sequence_worker_boundary_stats_row>);
static_assert(std::is_aggregate_v<scheduler_stats_row>);
static_assert(std::is_aggregate_v<inspect_cold_no_index_absence_row>);
static_assert(std::is_aggregate_v<inspect_ready_minimizer_generation_row>);
static_assert(std::is_aggregate_v<build_minimizer_fixture_row>);
static_assert(std::is_aggregate_v<read_minimizer_fixture_row>);
static_assert(
    std::is_aggregate_v<scan_grouped_soa_minimizer_generation_test_row>);
static_assert(std::is_aggregate_v<generation_storage_state_row>);
static_assert(std::is_aggregate_v<build_ready_minimizer_fragment_generation_row>);
static_assert(std::is_aggregate_v<build_ready_minimizer_sequence_generation_row>);

}  // namespace bioseq::postgres::sql_rows
