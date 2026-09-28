/**
 * @file index_parts/accounting.cppm
 * @brief Set batch limits and check alignment counters before reporting them.
 */
module;

#include <bioseq/pg_compat.h>
extern "C" {
#include "executor/instrument.h"
#include "utils/snapmgr.h"
}

#include <cstddef>
#include <cstdint>

// Shared limits and accounting rules for scan and join batches.
export module bioseq.postgres.scheduler.dispatch_buffer:accounting;

import bioseq.indexing.type;
import bioseq.pipeline.stats;
import bioseq.postgres.storage.indexing.minimizer_read_stats;

export namespace bioseq::scheduler {

constexpr size_t dispatch_batch_capacity = 8192;

/// Add PostgreSQL index-preparation counters to the pipeline statistics.
/// The destination merge records overflow. Backend choice and index-source
/// outcomes are left unchanged.
inline void merge_cold_index_preparation_stats(
    ::bioseq::pipeline_stats::pipeline_operational_stats& destination,
    const bioseq::storage::indexing::cold_index_preparation_stats& source) noexcept {
    ::bioseq::pipeline_stats::pipeline_operational_stats delta{};
    delta.cold_index_target_relation_reuses = source.target_relation_reuses;
    delta.cold_index_target_relation_extra_opens = source.target_relation_extra_opens;
    delta.cold_index_parent_random_reads = source.parent_random_reads;
    delta.cold_index_policy_heap_opens = source.policy_heap_opens;
    delta.cold_index_policy_index_opens = source.policy_index_opens;
    delta.cold_index_source_heap_opens = source.source_heap_opens;
    delta.cold_index_source_index_opens = source.source_index_opens;
    delta.cold_index_fragment_heap_opens = source.fragment_heap_opens;
    delta.cold_index_sidecar_heap_opens = source.sidecar_heap_opens;
    delta.cold_index_sidecar_index_opens = source.sidecar_index_opens;
    delta.cold_index_policy_schema_validations = source.policy_schema_validations;
    delta.cold_index_source_schema_validations = source.source_schema_validations;
    delta.cold_index_sidecar_schema_validations = source.sidecar_schema_validations;
    delta.cold_index_head_scans = source.head_scans;
    delta.cold_index_directory_scans = source.directory_scans;
    delta.cold_index_payload_range_scans = source.payload_range_scans;
    delta.cold_index_lock_acquires = source.lock_acquires;
    delta.cold_index_lock_releases = source.lock_releases;
    delta.cold_index_snapshot_registers = source.snapshot_registers;
    delta.cold_index_route_empty = source.route_empty;
    delta.cold_index_route_selective = source.route_selective;
    delta.cold_index_route_full = source.route_full;
    delta.cold_index_route_candidate_rows = source.route_candidate_rows;
    delta.cold_index_route_full_rows = source.route_full_rows;
    delta.cold_index_route_candidate_bytes = source.route_candidate_bytes;
    delta.cold_index_route_full_bytes = source.route_full_bytes;
    delta.cold_index_route_selective_cost = source.route_selective_cost;
    delta.cold_index_route_full_cost = source.route_full_cost;
    delta.cold_index_memory_reroutes = source.memory_reroutes;
    delta.cold_index_payload_bytes_requested = source.payload_bytes_requested;
    delta.cold_index_payload_bytes_delivered = source.payload_bytes_delivered;
    delta.cold_index_payload_rows_requested = source.payload_rows_requested;
    delta.cold_index_payload_rows_delivered = source.payload_rows_delivered;
    delta.cold_index_payload_rows_scanned = source.payload_rows_scanned;
    delta.cold_index_payload_bytes_scanned = source.payload_bytes_scanned;
    delta.cold_index_payload_rows_skipped = source.payload_rows_skipped;
    delta.cold_index_heap_tid_adjacent = source.heap_tid_adjacent;
    delta.cold_index_heap_tid_forward_gap = source.heap_tid_forward_gap;
    delta.cold_index_heap_tid_backward_jump = source.heap_tid_backward_jump;
    delta.cold_index_extent_crc_bytes = source.extent_crc_bytes;
    delta.cold_index_extent_crc_raw_bytes_hashed = source.extent_crc_raw_bytes_hashed;
    delta.cold_index_extent_crc_combined_bytes = source.extent_crc_combined_bytes;
    delta.cold_index_extent_crc_combine_rows = source.extent_crc_combine_rows;
    delta.cold_index_row_crc_bytes = source.row_crc_bytes;
    delta.cold_index_tuple_form_bytes = source.tuple_form_bytes;
    delta.cold_index_toast_values = source.toast_values;
    delta.cold_index_detoast_bytes = source.detoast_bytes;
    delta.cold_index_policy_ns = source.policy_ns;
    delta.cold_index_source_ns = source.source_ns;
    delta.cold_index_bind_ns = source.bind_ns;
    delta.cold_index_head_ns = source.head_ns;
    delta.cold_index_directory_ns = source.directory_ns;
    delta.cold_index_payload_ns = source.payload_ns;
    delta.cold_index_decode_ns = source.decode_ns;
    delta.cold_index_transfer_ns = source.transfer_ns;
    delta.cold_index_close_ns = source.close_ns;
    delta.cold_index_shared_blks_hit = source.shared_blks_hit;
    delta.cold_index_shared_blks_read = source.shared_blks_read;
    delta.are_stats_saturated = source.are_stats_saturated;
    destination.merge(delta);
}

inline void map_hash_major_index_reader_stats(
    ::bioseq::pipeline_stats::pipeline_operational_stats& destination,
    const bioseq::storage::indexing::minimizer_generation_read_stats& source) noexcept {
    const auto& input = source.hash_major;
    auto& output = destination.hash_major_index_reader;
    output.layout_revision = input.layout_revision;
    output.sessions = input.sessions;
    output.identity_available = input.sessions != 0u;
    output.generation_id = input.generation_id;
    output.source_identity = input.source_identity;
    output.source_content_crc64 = input.source_content_crc64;
    output.storage_contract_digest = input.storage_contract_digest;
    output.directory_rows = input.directory_rows;
    output.directory_bytes = input.directory_bytes;
    output.control_rows = input.control_rows;
    output.control_bytes = input.control_bytes;
    output.posting_rows = input.posting_rows;
    output.posting_bytes = input.posting_bytes;
    output.directory_row_crc_receipts = input.directory_row_crc_receipts;
    output.directory_stream_crc_receipts = input.directory_stream_crc_receipts;
    output.control_row_crc_receipts = input.control_row_crc_receipts;
    output.control_block_crc_receipts = input.control_block_crc_receipts;
    output.control_semantic_receipts = input.control_semantic_receipts;
    output.posting_row_crc_receipts = input.posting_row_crc_receipts;
    output.posting_ranges_delivered = input.posting_ranges_delivered;
    output.posting_range_crc_receipts = input.posting_range_crc_receipts;
    output.posting_semantic_receipts = input.posting_semantic_receipts;
    output.authorized_posting_bytes = input.authorized_posting_bytes;
    output.query_count = input.query_count;
    output.selected_block_count = input.selected_block_count;
    output.accepted_hash_count = input.accepted_hash_count;
    output.retained_position_count = input.retained_position_count;
    output.directory_peak_bytes = input.directory_stage_peak_bytes;
    output.control_peak_bytes = input.control_stage_peak_bytes;
    output.posting_peak_bytes = input.posting_stage_peak_bytes;
    output.peak_admitted_bytes = input.peak_admitted_bytes;
    output.owner_bytes_available = input.sessions != 0u;
    output.retained_owner_bytes = input.retained_owner_bytes;
    output.admitted_bytes = input.admitted_bytes;
    output.generic_source_fallback_calls = input.generic_source_fallback_calls;
    output.failures = input.failures;
    output.is_saturated = input.saturated;
}

/// Add one stored-index read session to the  pipeline statistics.
/// CPU and GPU targets use the same PostgreSQL reader counters. The caller
/// must add each session only once
inline void merge_generation_read_stats(
    ::bioseq::pipeline_stats::pipeline_operational_stats& destination,
    const bioseq::storage::indexing::minimizer_generation_read_stats& source) noexcept {
    ::bioseq::pipeline_stats::pipeline_operational_stats delta{};
    delta.persisted_generation_read_sessions = 1;
    delta.persisted_relation_opens = source.relation_opens;
    delta.persisted_index_scans = source.index_scans;
    delta.persisted_head_scans = source.head_scans;
    delta.persisted_head_rows_read = source.head_rows_read;
    delta.persisted_head_bytes_read = source.head_bytes_read;
    delta.persisted_manifest_directory_scans = source.manifest_directory_scans;
    delta.persisted_manifest_rows_read = source.manifest_rows_read;
    delta.persisted_manifest_bytes_read = source.manifest_bytes_read;
    delta.persisted_directory_chunks_read = source.directory_chunks_read;
    delta.persisted_directory_bytes_read = source.directory_bytes_read;
    delta.persisted_metadata_repeated_reads = source.metadata_repeated_reads;
    delta.persisted_metadata_heap_rows_read = source.metadata_heap_rows_read;
    delta.persisted_metadata_bytes_read = source.metadata_bytes_read;
    delta.persisted_payload_heap_rows_read = source.payload_heap_rows_read;
    delta.persisted_payload_bytes_read = source.payload_bytes_read;
    delta.persisted_heap_rows_read = source.total_heap_rows_read;
    delta.persisted_toast_values_read = source.toast_values_read;
    delta.persisted_detoast_bytes = source.detoast_bytes;
    delta.persisted_explicit_copy_bytes = source.explicit_copy_bytes;
    delta.persisted_wanted_subkeys = source.payload.wanted_subkeys;
    delta.persisted_read_subkeys = source.payload.read_subkeys;
    delta.persisted_wanted_extents = source.payload.wanted_extents;
    delta.persisted_wanted_chunks = source.payload.wanted_chunks;
    delta.persisted_skipped_rows = source.payload.skipped_rows;
    delta.persisted_repeated_chunk_reads = source.payload.repeated_chunk_reads;
    delta.persisted_extents_read = source.payload.read_extents;
    delta.persisted_chunks_read = source.payload.delivered_chunks;
    delta.persisted_bytes_read = source.payload.delivered_bytes;
    delta.persisted_bulk_read_calls = source.bulk_read_calls;
    delta.persisted_planned_request_groups = source.planned_request_groups;
    delta.persisted_executed_request_groups = source.executed_request_groups;
    delta.persisted_range_scan_calls = source.range_scan_calls;
    delta.persisted_skip_budget_rows = source.payload.skip_budget_rows;
    delta.persisted_source_parent_rows_read = source.source_parent_rows_read;
    delta.persisted_source_catalog_rows_read = source.source_catalog_rows_read;
    delta.persisted_source_catalog_index_scans = source.source_catalog_index_scans;
    delta.persisted_source_fragment_rows_read = source.source_fragment_rows_read;
    delta.persisted_source_decoded_bp = source.source_decoded_bp;
    map_hash_major_index_reader_stats(delta, source);
    destination.merge(delta);
    merge_cold_index_preparation_stats(destination, source.cold);
}

[[nodiscard]] constexpr std::uint64_t
extra_broadcast_query_ordinal(const bool per_row_query_mode,
                              const std::size_t batch_size,
                              const std::size_t extra_query_index) noexcept {
    return per_row_query_mode ? static_cast<std::uint64_t>(batch_size) +
                                    static_cast<std::uint64_t>(extra_query_index)
                              : static_cast<std::uint64_t>(extra_query_index) + 1;
}
static_assert(extra_broadcast_query_ordinal(false, 2, 0) == 1);
static_assert(extra_broadcast_query_ordinal(true, 2, 0) == 2);
static_assert(std::uint64_t{1} != extra_broadcast_query_ordinal(true, 2, 0),
              "per-row primary row 1 and the first extra broadcast query must differ");

[[nodiscard]] constexpr bool dispatch_uses_per_row_query(const bool active,
                                                         const AttrNumber raw_query_attno,
                                                         const bool evaluated_query) noexcept {
    return active && (raw_query_attno != InvalidAttrNumber || evaluated_query);
}

static_assert(!dispatch_uses_per_row_query(false, InvalidAttrNumber, true));
static_assert(dispatch_uses_per_row_query(true, AttrNumber{1}, false));
static_assert(dispatch_uses_per_row_query(true, InvalidAttrNumber, true));

[[nodiscard]] constexpr bool
evaluated_mmsearch_index_policy_valid(const bioseq::indexing::index_read_requirement requirement,
                                      const bool read_sequence_index) noexcept {
    return requirement == bioseq::indexing::index_read_requirement::require_absent &&
           !read_sequence_index;
}

static_assert(evaluated_mmsearch_index_policy_valid(
    bioseq::indexing::index_read_requirement::require_absent, false));
static_assert(
    !evaluated_mmsearch_index_policy_valid(bioseq::indexing::index_read_requirement::allow, false));

enum class cursor_boundary : std::uint8_t {
    value,
    exact_eof,
    corrupt,
};

[[nodiscard]] constexpr cursor_boundary classify_output_cursor(const std::int64_t position,
                                                               const std::size_t size) noexcept {
    if (position < 0)
        return cursor_boundary::corrupt;
    const auto unsigned_position = static_cast<std::uint64_t>(position);
    if (unsigned_position == size)
        return cursor_boundary::exact_eof;
    return unsigned_position < size ? cursor_boundary::value : cursor_boundary::corrupt;
}

[[nodiscard]] constexpr cursor_boundary classify_output_cursor(const std::size_t position,
                                                               const std::size_t size) noexcept {
    if (position == size)
        return cursor_boundary::exact_eof;
    return position < size ? cursor_boundary::value : cursor_boundary::corrupt;
}

static_assert(classify_output_cursor(std::int64_t{0}, 1) == cursor_boundary::value);
static_assert(classify_output_cursor(std::int64_t{1}, 1) == cursor_boundary::exact_eof);
static_assert(classify_output_cursor(std::int64_t{-1}, 1) == cursor_boundary::corrupt);
static_assert(classify_output_cursor(std::int64_t{2}, 1) == cursor_boundary::corrupt);
static_assert(classify_output_cursor(std::size_t{0}, 0) == cursor_boundary::exact_eof);
static_assert(classify_output_cursor(std::size_t{2}, 1) == cursor_boundary::corrupt);

enum class shared_join_target_outcome_topology : std::uint8_t {
    invalid,
    cpu_per_pair,
    gpu_shared_target,
};

/*
 * Determine whether the request uses CPU or a complete Vulkan pipeline from
 * its selected options. A mixed set of CPU algorithms can have a
 * mixed_invalid backend label and still report valid per-query counts.
 * Both routes report one raw target outcome for each query.
 */
[[nodiscard]] constexpr shared_join_target_outcome_topology
classify_shared_join_target_outcome_topology(const bool any_vulkan_component_requested,
                                             const bool complete_vulkan_tuple_requested) noexcept {
    if (complete_vulkan_tuple_requested) {
        return any_vulkan_component_requested
                   ? shared_join_target_outcome_topology::gpu_shared_target
                   : shared_join_target_outcome_topology::invalid;
    }
    return any_vulkan_component_requested ? shared_join_target_outcome_topology::invalid
                                          : shared_join_target_outcome_topology::cpu_per_pair;
}
static_assert(classify_shared_join_target_outcome_topology(false, false) ==
              shared_join_target_outcome_topology::cpu_per_pair);
static_assert(classify_shared_join_target_outcome_topology(true, true) ==
              shared_join_target_outcome_topology::gpu_shared_target);
static_assert(classify_shared_join_target_outcome_topology(true, false) ==
              shared_join_target_outcome_topology::invalid);
static_assert(classify_shared_join_target_outcome_topology(false, true) ==
              shared_join_target_outcome_topology::invalid);

[[nodiscard]] constexpr bool
shared_join_raw_target_shape_valid(const shared_join_target_outcome_topology topology,
                                   const ::bioseq::pipeline_stats::executed_backend backend,
                                   const std::uint64_t query_count,
                                   const std::uint64_t attempted_jobs,
                                   const std::uint64_t raw_target_rows) noexcept {
    if (query_count == 0 || attempted_jobs != query_count) {
        return false;
    }
    std::uint64_t expected = 0;
    switch (topology) {
    case shared_join_target_outcome_topology::cpu_per_pair:
        if (backend == ::bioseq::pipeline_stats::executed_backend::none ||
            backend == ::bioseq::pipeline_stats::executed_backend::vulkan_gpu) {
            return false;
        }
        /* CPU algorithms from different implementations can produce
         * mixed_invalid while still reporting one target outcoe per query. */
        expected = query_count;
        break;
    case shared_join_target_outcome_topology::gpu_shared_target:
        if (backend != ::bioseq::pipeline_stats::executed_backend::vulkan_gpu
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
            && backend != ::bioseq::pipeline_stats::executed_backend::hybrid_cpu_vulkan
#elif defined(BIOSEQ_TEST_INDEXES)
            && backend != ::bioseq::pipeline_stats::executed_backend::mixed_invalid
#endif
        ) {
            return false;
        }
        expected = query_count;
        break;
    case shared_join_target_outcome_topology::invalid:
        return false;
    }
    return raw_target_rows == expected;
}
static_assert(
    shared_join_raw_target_shape_valid(shared_join_target_outcome_topology::cpu_per_pair,
                                       ::bioseq::pipeline_stats::executed_backend::simd_cpu,
                                       16,
                                       16,
                                       16));
static_assert(
    shared_join_raw_target_shape_valid(shared_join_target_outcome_topology::cpu_per_pair,
                                       ::bioseq::pipeline_stats::executed_backend::mixed_invalid,
                                       16,
                                       16,
                                       16));
static_assert(
    shared_join_raw_target_shape_valid(shared_join_target_outcome_topology::gpu_shared_target,
                                       ::bioseq::pipeline_stats::executed_backend::vulkan_gpu,
                                       16,
                                       16,
                                       16));
/* Every wave reports its full target counts. The join owner later counts
 * the shared physical  target once across all waves. */
static_assert(
    shared_join_raw_target_shape_valid(shared_join_target_outcome_topology::cpu_per_pair,
                                       ::bioseq::pipeline_stats::executed_backend::simd_cpu,
                                       8192,
                                       8192,
                                       8192));
static_assert(
    !shared_join_raw_target_shape_valid(shared_join_target_outcome_topology::cpu_per_pair,
                                        ::bioseq::pipeline_stats::executed_backend::simd_cpu,
                                        1,
                                        1,
                                        0));
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
static_assert(
    shared_join_raw_target_shape_valid(
        shared_join_target_outcome_topology::gpu_shared_target,
        ::bioseq::pipeline_stats::executed_backend::hybrid_cpu_vulkan,
        16,
        16,
        16));
static_assert(
    !shared_join_raw_target_shape_valid(shared_join_target_outcome_topology::gpu_shared_target,
                                        ::bioseq::pipeline_stats::executed_backend::mixed_invalid,
                                        16,
                                        16,
                                        1));
#elif defined(BIOSEQ_TEST_INDEXES)
static_assert(
    shared_join_raw_target_shape_valid(shared_join_target_outcome_topology::gpu_shared_target,
                                       ::bioseq::pipeline_stats::executed_backend::mixed_invalid,
                                       16,
                                       16,
                                       16));
static_assert(
    !shared_join_raw_target_shape_valid(
        shared_join_target_outcome_topology::gpu_shared_target,
        ::bioseq::pipeline_stats::executed_backend::hybrid_cpu_vulkan,
        16,
        16,
        1));
#else
static_assert(
    !shared_join_raw_target_shape_valid(shared_join_target_outcome_topology::gpu_shared_target,
                                        ::bioseq::pipeline_stats::executed_backend::mixed_invalid,
                                        16,
                                        16,
                                        1));
#endif
static_assert(
    !shared_join_raw_target_shape_valid(shared_join_target_outcome_topology::cpu_per_pair,
                                        ::bioseq::pipeline_stats::executed_backend::vulkan_gpu,
                                        16,
                                        16,
                                        16));
static_assert(
    !shared_join_raw_target_shape_valid(shared_join_target_outcome_topology::cpu_per_pair,
                                        ::bioseq::pipeline_stats::executed_backend::simd_cpu,
                                        16,
                                        15,
                                        16));
static_assert(
    !shared_join_raw_target_shape_valid(shared_join_target_outcome_topology::cpu_per_pair,
                                        ::bioseq::pipeline_stats::executed_backend::simd_cpu,
                                        16,
                                        16,
                                        15));

[[nodiscard]] constexpr bool
shared_join_raw_source_shape_valid(const ::bioseq::pipeline_stats::target_index_source requested,
                                   const ::bioseq::pipeline_stats::target_index_source actual,
                                   const std::uint64_t raw_target_rows,
                                   const std::uint64_t persisted_rows,
                                   const std::uint64_t recomputed_rows,
                                   const std::uint64_t missing_rows) noexcept {
    if (raw_target_rows == 0 || requested == ::bioseq::pipeline_stats::target_index_source::none ||
        requested == ::bioseq::pipeline_stats::target_index_source::mixed_invalid ||
        actual == ::bioseq::pipeline_stats::target_index_source::none ||
        actual == ::bioseq::pipeline_stats::target_index_source::mixed_invalid) {
        return false;
    }
    if (actual == ::bioseq::pipeline_stats::target_index_source::persisted) {
        /* A request to recompute must not read a stored index. */
        return requested == ::bioseq::pipeline_stats::target_index_source::persisted &&
               persisted_rows == raw_target_rows && recomputed_rows == 0 && missing_rows == 0;
    }
    /* The remaining valid source is an index computed for, this request. */
    if (persisted_rows != 0 || recomputed_rows != raw_target_rows) {
        return false;
    }
    if (requested == ::bioseq::pipeline_stats::target_index_source::recomputed) {
        return missing_rows == 0;
    }
    /* If a requested stored index was missing, every recomputed target must
     * also have a missing-index count. */
    return requested == ::bioseq::pipeline_stats::target_index_source::persisted &&
           missing_rows == raw_target_rows;
}
static_assert(
    shared_join_raw_source_shape_valid(::bioseq::pipeline_stats::target_index_source::recomputed,
                                       ::bioseq::pipeline_stats::target_index_source::recomputed,
                                       16,
                                       0,
                                       16,
                                       0));
static_assert(
    shared_join_raw_source_shape_valid(::bioseq::pipeline_stats::target_index_source::persisted,
                                       ::bioseq::pipeline_stats::target_index_source::persisted,
                                       16,
                                       16,
                                       0,
                                       0));
static_assert(
    shared_join_raw_source_shape_valid(::bioseq::pipeline_stats::target_index_source::persisted,
                                       ::bioseq::pipeline_stats::target_index_source::recomputed,
                                       16,
                                       0,
                                       16,
                                       16));
static_assert(
    !shared_join_raw_source_shape_valid(::bioseq::pipeline_stats::target_index_source::persisted,
                                        ::bioseq::pipeline_stats::target_index_source::recomputed,
                                        16,
                                        0,
                                        16,
                                        0));
static_assert(
    !shared_join_raw_source_shape_valid(::bioseq::pipeline_stats::target_index_source::recomputed,
                                        ::bioseq::pipeline_stats::target_index_source::persisted,
                                        16,
                                        16,
                                        0,
                                        0));
static_assert(!shared_join_raw_source_shape_valid(
    ::bioseq::pipeline_stats::target_index_source::mixed_invalid,
    ::bioseq::pipeline_stats::target_index_source::recomputed,
    16,
    0,
    16,
    0));
static_assert(
    !shared_join_raw_source_shape_valid(::bioseq::pipeline_stats::target_index_source::persisted,
                                        ::bioseq::pipeline_stats::target_index_source::persisted,
                                        16,
                                        15,
                                        1,
                                        0));

/*
 * A join reuses one target for many queries, but the processor reports one
 * target outcome per pair. Check those counts and index sources first.
 * Then count the physical target once, or omit it  if an earlier wave already
 * counted it. Inconsistent input raises a PostgreSQL error.
 */
inline void
normalize_shared_join_target_outcomes(::bioseq::pipeline_stats::pipeline_execution_stats& stats,
                                      const bool record_physical_target,
                                      const std::size_t query_count,
                                      const shared_join_target_outcome_topology topology) {
    auto& operational = stats.operational;
    const std::uint64_t target_rows = operational.target_rows;
    const std::uint64_t persisted_rows = operational.persisted_index_rows;
    const std::uint64_t recomputed_rows = operational.recomputed_target_index_rows;
    const std::uint64_t missing_rows = operational.missing_persisted_index_rows;
    const std::uint64_t query_count_u64 = static_cast<std::uint64_t>(query_count);
    std::uint64_t expected_raw_target_rows = 0;
    switch (topology) {
    case shared_join_target_outcome_topology::cpu_per_pair:
        expected_raw_target_rows = query_count_u64;
        break;
    case shared_join_target_outcome_topology::gpu_shared_target:
        expected_raw_target_rows = query_count_u64;
        break;
    case shared_join_target_outcome_topology::invalid:
        ereport(
            ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq batch join target outcomes have an invalid dispatch topology"),
             errdetail(
                 "requested_backend=%.*s query_count=%zu",
                 static_cast<int>(
                     ::bioseq::pipeline_stats::executed_backend_name(stats.requested_backend)
                         .size()),
                 ::bioseq::pipeline_stats::executed_backend_name(stats.requested_backend).data(),
                 query_count)));
    }
    const bool source_mixed = persisted_rows != 0 && recomputed_rows != 0;
    const bool source_count_invalid = persisted_rows > query_count_u64 ||
                                      recomputed_rows > query_count_u64 ||
                                      persisted_rows + recomputed_rows != target_rows;
    const bool raw_target_shape_invalid =
        !shared_join_raw_target_shape_valid(topology,
                                            stats.requested_backend,
                                            query_count_u64,
                                            stats.attempted_alignment_jobs,
                                            target_rows);
    const bool raw_source_shape_invalid =
        !shared_join_raw_source_shape_valid(stats.requested_target_index_source,
                                            stats.actual_target_index_source,
                                            target_rows,
                                            persisted_rows,
                                            recomputed_rows,
                                            missing_rows);
    const bool shape_invalid = query_count == 0 || raw_target_shape_invalid ||
                               raw_source_shape_invalid || missing_rows > recomputed_rows ||
                               (record_physical_target && target_rows == 0);
    if (source_mixed || source_count_invalid || shape_invalid) {
        ereport(
            ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq batch join target outcome accounting is inconsistent"),
             errdetail(
                 "record_physical_target=%s query_count=%zu attempted_jobs=%llu "
                 "requested_backend=%.*s dispatch_topology=%s "
                 "expected_raw_target_rows=%llu "
                 "requested_source=%.*s actual_source=%.*s "
                 "target_rows=%llu persisted_rows=%llu recomputed_rows=%llu "
                 "missing_rows=%llu",
                 record_physical_target ? "true" : "false",
                 query_count,
                 static_cast<unsigned long long>(stats.attempted_alignment_jobs),
                 static_cast<int>(
                     ::bioseq::pipeline_stats::executed_backend_name(stats.requested_backend)
                         .size()),
                 ::bioseq::pipeline_stats::executed_backend_name(stats.requested_backend).data(),
                 topology == shared_join_target_outcome_topology::cpu_per_pair
                     ? "cpu_per_pair"
                     : "gpu_shared_target",
                 static_cast<unsigned long long>(expected_raw_target_rows),
                 static_cast<int>(::bioseq::pipeline_stats::target_index_source_name(
                                      stats.requested_target_index_source)
                                      .size()),
                 ::bioseq::pipeline_stats::target_index_source_name(
                     stats.requested_target_index_source)
                     .data(),
                 static_cast<int>(::bioseq::pipeline_stats::target_index_source_name(
                                      stats.actual_target_index_source)
                                      .size()),
                 ::bioseq::pipeline_stats::target_index_source_name(
                     stats.actual_target_index_source)
                     .data(),
                 static_cast<unsigned long long>(target_rows),
                 static_cast<unsigned long long>(persisted_rows),
                 static_cast<unsigned long long>(recomputed_rows),
                 static_cast<unsigned long long>(missing_rows))));
    }

    operational.target_rows = record_physical_target ? 1 : 0;
    operational.persisted_index_rows = record_physical_target && persisted_rows != 0 ? 1 : 0;
    operational.recomputed_target_index_rows =
        record_physical_target && recomputed_rows != 0 ? 1 : 0;
    operational.missing_persisted_index_rows = record_physical_target && missing_rows != 0 ? 1 : 0;
}

} // namespace bioseq::scheduler
