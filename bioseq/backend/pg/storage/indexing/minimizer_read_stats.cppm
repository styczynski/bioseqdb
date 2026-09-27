/**
 * @file minimizer_read_stats.cppm
 * @brief Count reads, validation  work and memory use when loading stored minimizer indexes.
 */
module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

export module bioseq.postgres.storage.indexing.minimizer_read_stats;

import bioseq.indexing.storage;

export namespace bioseq::storage::indexing {

using bioseq::indexing::index_chunk_read_stats;

/**
 * @brief Count PostgreSQL work needed to prepare an index for its first read
 *
 * add and merge stop counters at INT64_MAX and mark saturation so the values
 * remain safe to return as PostgreSQL bigint values.
 */
struct cold_index_preparation_stats {
    std::uint64_t target_relation_reuses = 0;
    std::uint64_t target_relation_extra_opens = 0;
    std::uint64_t parent_random_reads = 0;
    std::uint64_t policy_heap_opens = 0;
    std::uint64_t policy_index_opens = 0;
    std::uint64_t source_heap_opens = 0;
    std::uint64_t source_index_opens = 0;
    std::uint64_t fragment_heap_opens = 0;
    std::uint64_t sidecar_heap_opens = 0;
    std::uint64_t sidecar_index_opens = 0;
    std::uint64_t policy_schema_validations = 0;
    std::uint64_t source_schema_validations = 0;
    std::uint64_t sidecar_schema_validations = 0;
    std::uint64_t head_scans = 0;
    std::uint64_t directory_scans = 0;
    std::uint64_t payload_range_scans = 0;
    std::uint64_t lock_acquires = 0;
    std::uint64_t lock_releases = 0;
    std::uint64_t snapshot_registers = 0;
    std::uint64_t route_empty = 0;
    std::uint64_t route_selective = 0;
    std::uint64_t route_full = 0;
    std::uint64_t route_candidate_rows = 0;
    std::uint64_t route_full_rows = 0;
    std::uint64_t route_candidate_bytes = 0;
    std::uint64_t route_full_bytes = 0;
    std::uint64_t route_selective_cost = 0;
    std::uint64_t route_full_cost = 0;
    std::uint64_t memory_reroutes = 0;
    std::uint64_t payload_bytes_requested = 0;
    std::uint64_t payload_bytes_delivered = 0;
    std::uint64_t payload_rows_requested = 0;
    std::uint64_t payload_rows_delivered = 0;
    std::uint64_t payload_rows_scanned = 0;
    std::uint64_t payload_bytes_scanned = 0;
    std::uint64_t payload_rows_skipped = 0;
    std::uint64_t heap_tid_adjacent = 0;
    std::uint64_t heap_tid_forward_gap = 0;
    std::uint64_t heap_tid_backward_jump = 0;
    std::uint64_t extent_crc_bytes = 0;
    std::uint64_t extent_crc_raw_bytes_hashed = 0;
    std::uint64_t extent_crc_combined_bytes = 0;
    std::uint64_t extent_crc_combine_rows = 0;
    std::uint64_t row_crc_bytes = 0;
    std::uint64_t tuple_form_bytes = 0;
    std::uint64_t toast_values = 0;
    std::uint64_t detoast_bytes = 0;
    std::uint64_t policy_ns = 0;
    std::uint64_t source_ns = 0;
    std::uint64_t bind_ns = 0;
    std::uint64_t head_ns = 0;
    std::uint64_t directory_ns = 0;
    std::uint64_t payload_ns = 0;
    std::uint64_t decode_ns = 0;
    std::uint64_t transfer_ns = 0;
    std::uint64_t close_ns = 0;
    std::uint64_t shared_blks_hit = 0;
    std::uint64_t shared_blks_read = 0;
    bool are_stats_saturated = false;

    static constexpr std::uint64_t k_sql_bigint_max =
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());

    /// Add to a counter, stopping at INT64_MAX and recording overflow.
    void add(std::uint64_t& destination, std::uint64_t increment) noexcept {
        if (destination > k_sql_bigint_max ||
            increment > k_sql_bigint_max - std::min(destination, k_sql_bigint_max)) {
            destination = k_sql_bigint_max;
            are_stats_saturated = true;
            return;
        }
        destination += increment;
    }

    /// Add another preparation record to these counters.
    void merge(const cold_index_preparation_stats& rhs) noexcept {
        are_stats_saturated = are_stats_saturated || rhs.are_stats_saturated;
        add(target_relation_reuses, rhs.target_relation_reuses);
        add(target_relation_extra_opens, rhs.target_relation_extra_opens);
        add(parent_random_reads, rhs.parent_random_reads);
        add(policy_heap_opens, rhs.policy_heap_opens);
        add(policy_index_opens, rhs.policy_index_opens);
        add(source_heap_opens, rhs.source_heap_opens);
        add(source_index_opens, rhs.source_index_opens);
        add(fragment_heap_opens, rhs.fragment_heap_opens);
        add(sidecar_heap_opens, rhs.sidecar_heap_opens);
        add(sidecar_index_opens, rhs.sidecar_index_opens);
        add(policy_schema_validations, rhs.policy_schema_validations);
        add(source_schema_validations, rhs.source_schema_validations);
        add(sidecar_schema_validations, rhs.sidecar_schema_validations);
        add(head_scans, rhs.head_scans);
        add(directory_scans, rhs.directory_scans);
        add(payload_range_scans, rhs.payload_range_scans);
        add(lock_acquires, rhs.lock_acquires);
        add(lock_releases, rhs.lock_releases);
        add(snapshot_registers, rhs.snapshot_registers);
        add(route_empty, rhs.route_empty);
        add(route_selective, rhs.route_selective);
        add(route_full, rhs.route_full);
        add(route_candidate_rows, rhs.route_candidate_rows);
        add(route_full_rows, rhs.route_full_rows);
        add(route_candidate_bytes, rhs.route_candidate_bytes);
        add(route_full_bytes, rhs.route_full_bytes);
        add(route_selective_cost, rhs.route_selective_cost);
        add(route_full_cost, rhs.route_full_cost);
        add(memory_reroutes, rhs.memory_reroutes);
        add(payload_bytes_requested, rhs.payload_bytes_requested);
        add(payload_bytes_delivered, rhs.payload_bytes_delivered);
        add(payload_rows_requested, rhs.payload_rows_requested);
        add(payload_rows_delivered, rhs.payload_rows_delivered);
        add(payload_rows_scanned, rhs.payload_rows_scanned);
        add(payload_bytes_scanned, rhs.payload_bytes_scanned);
        add(payload_rows_skipped, rhs.payload_rows_skipped);
        add(heap_tid_adjacent, rhs.heap_tid_adjacent);
        add(heap_tid_forward_gap, rhs.heap_tid_forward_gap);
        add(heap_tid_backward_jump, rhs.heap_tid_backward_jump);
        add(extent_crc_bytes, rhs.extent_crc_bytes);
        add(extent_crc_raw_bytes_hashed, rhs.extent_crc_raw_bytes_hashed);
        add(extent_crc_combined_bytes, rhs.extent_crc_combined_bytes);
        add(extent_crc_combine_rows, rhs.extent_crc_combine_rows);
        add(row_crc_bytes, rhs.row_crc_bytes);
        add(tuple_form_bytes, rhs.tuple_form_bytes);
        add(toast_values, rhs.toast_values);
        add(detoast_bytes, rhs.detoast_bytes);
        add(policy_ns, rhs.policy_ns);
        add(source_ns, rhs.source_ns);
        add(bind_ns, rhs.bind_ns);
        add(head_ns, rhs.head_ns);
        add(directory_ns, rhs.directory_ns);
        add(payload_ns, rhs.payload_ns);
        add(decode_ns, rhs.decode_ns);
        add(transfer_ns, rhs.transfer_ns);
        add(close_ns, rhs.close_ns);
        add(shared_blks_hit, rhs.shared_blks_hit);
        add(shared_blks_read, rhs.shared_blks_read);
    }

    [[nodiscard]] constexpr bool
    operator==(const cold_index_preparation_stats&) const noexcept = default;
};

/// Count planned requests, actual reads and validation for one payload stage.
struct minimizer_generation_payload_stage_read_stats {
    std::uint64_t plan_calls = 0;
    std::uint64_t planned_request_groups = 0;
    std::uint64_t executed_request_groups = 0;
    std::uint64_t range_scan_calls = 0;
    std::uint64_t physical_heap_rows_read = 0;
    std::uint64_t physical_bytes_read = 0;
    std::uint64_t control_authorization_checks = 0;
    index_chunk_read_stats read{};
};

/// Counters for the ordered full-generation relation scan.
struct minimizer_generation_full_payload_read_stats {
    std::uint64_t scan_calls = 0;
    std::uint64_t planned_extents = 0;
    std::uint64_t completed_extents = 0;
    std::uint64_t control_chunks = 0;
    std::uint64_t position_chunks = 0;
    std::uint64_t control_bytes = 0;
    std::uint64_t position_bytes = 0;
    std::uint64_t payload_row_checksum = 0;
};

/// Record identity, validation, memory use and results when reading a hash-major  index.
struct hash_major_minimizer_cold_read_stats {
    std::uint64_t layout_revision = 0;
    std::uint64_t sessions = 0;
    std::uint64_t generation_id = 0;
    std::array<std::byte, 16> source_identity{};
    std::uint64_t source_content_crc64 = 0;
    std::array<std::byte, 32> storage_contract_digest{};
    std::uint64_t directory_rows = 0;
    std::uint64_t directory_bytes = 0;
    std::uint64_t control_rows = 0;
    std::uint64_t control_bytes = 0;
    std::uint64_t posting_rows = 0;
    std::uint64_t posting_bytes = 0;
    std::uint64_t directory_row_crc_receipts = 0;
    std::uint64_t directory_stream_crc_receipts = 0;
    std::uint64_t control_row_crc_receipts = 0;
    std::uint64_t control_block_crc_receipts = 0;
    std::uint64_t control_semantic_receipts = 0;
    std::uint64_t posting_row_crc_receipts = 0;
    std::uint64_t posting_ranges_delivered = 0;
    std::uint64_t posting_range_crc_receipts = 0;
    std::uint64_t posting_semantic_receipts = 0;
    std::uint64_t authorized_posting_bytes = 0;
    std::uint64_t generic_source_fallback_calls = 0;
    std::uint64_t query_count = 0;
    std::uint64_t selected_block_count = 0;
    std::uint64_t accepted_hash_count = 0;
    std::uint64_t retained_position_count = 0;
    std::uint64_t directory_stage_peak_bytes = 0;
    std::uint64_t control_stage_peak_bytes = 0;
    std::uint64_t posting_stage_peak_bytes = 0;
    std::uint64_t peak_admitted_bytes = 0;
    std::uint64_t retained_owner_bytes = 0;
    std::uint64_t admitted_bytes = 0;
    std::uint64_t failures = 0;
    bool saturated = false;
};

/// Collect statistics for one index generation read session.
struct minimizer_generation_read_stats {
    cold_index_preparation_stats cold{};
    std::uint64_t relation_opens = 0;
    std::uint64_t index_scans = 0;
    std::uint64_t head_scans = 0;
    std::uint64_t head_rows_read = 0;
    std::uint64_t head_bytes_read = 0;
    std::uint64_t manifest_directory_scans = 0;
    std::uint64_t manifest_rows_read = 0;
    std::uint64_t manifest_bytes_read = 0;
    std::uint64_t directory_chunks_read = 0;
    std::uint64_t directory_bytes_read = 0;
    std::uint64_t metadata_repeated_reads = 0;
    std::uint64_t metadata_heap_rows_read = 0;
    std::uint64_t metadata_bytes_read = 0;
    std::uint64_t payload_heap_rows_read = 0;
    std::uint64_t payload_bytes_read = 0;
    std::uint64_t total_heap_rows_read = 0;
    std::uint64_t total_bytes_read = 0;
    std::uint64_t toast_values_read = 0;
    std::uint64_t detoast_bytes = 0;
    std::uint64_t explicit_copy_bytes = 0;
    std::uint64_t bulk_read_calls = 0;
    std::uint64_t planned_request_groups = 0;
    std::uint64_t executed_request_groups = 0;
    std::uint64_t range_scan_calls = 0;
    std::uint64_t cache_lookups = 0;
    std::uint64_t cache_populations = 0;
    std::uint64_t source_parent_rows_read = 0;
    std::uint64_t source_catalog_rows_read = 0;
    std::uint64_t source_catalog_index_scans = 0;
    std::uint64_t source_fragment_rows_read = 0;
    std::uint64_t source_decoded_bp = 0;
    std::uint64_t source_storage_incarnations_checked = 0;
    index_chunk_read_stats payload{};
    minimizer_generation_payload_stage_read_stats controls{};
    minimizer_generation_payload_stage_read_stats positions{};
    minimizer_generation_full_payload_read_stats full_generation{};
    hash_major_minimizer_cold_read_stats hash_major{};
};

} // namespace bioseq::storage::indexing
