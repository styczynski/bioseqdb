/**
 * @file typed_support_parts/row_adapters.cppm
 * @brief Check shared target readers and attach saved query statistics.
 */
module;

extern "C" {
#include "postgres.h"
#include "utils/errcodes.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <new>

export module bioseq.postgres.scheduler.dispatch_processor.typed_support.row_adapters;

import std;
export import bioseq.postgres.scheduler.dispatch_processor.typed_support.source_provider_adapters;
import bioseq.postgres.scheduler.dispatch_processor.hash_major_diagnostic_identity;
import bioseq.postgres.scheduler.dispatch_processor.checked_alignment_calls;
import bioseq.indexing.type;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.hash_major_minimizer_index;
import bioseq.pipeline.stats;
import bioseq.pipeline.minimap_port;
import bioseq.pipeline.minimizer_execution_contract;
import bioseq.storage.layout;

export namespace bioseq::simd::typed_dispatch::typed_support_detail {

/// Checks that two locators identify the same stored sequence before workers
/// share a target copy. Compares all fields, including reserved fields.
[[nodiscard]] constexpr bool
same_dispatch_target_locator(const bioseq::storage::sequence_locator& left,
                             const bioseq::storage::sequence_locator& right) noexcept {
    return left.payload_version == right.payload_version &&
           left.bucket_index == right.bucket_index && left.reserved_16 == right.reserved_16 &&
           left.shard_index == right.shard_index && left.fragment_count == right.fragment_count &&
           left.reserved_32 == right.reserved_32 && left.logical_length == right.logical_length &&
           left.first_fragment_row == right.first_fragment_row &&
           left.source_epoch == right.source_epoch;
}

/// Checks that the hash-major reader belongs to this target and stored index
/// generation. Its read checks and  memory counts must agree before query
/// workers can share it.
template <class p_prepared_index_group>
[[nodiscard]] inline bool
hash_major_parallel_reader_identity_valid(const p_prepared_index_group& group,
                                          const std::uint64_t target_bp_size) noexcept {
    if (!group.persisted_hash_major_reader.has_value() ||
        group.persisted_fragment_mapping_guard.has_value() ||
        group.persisted_fragment_reader.has_value() ||
        group.persisted_grouped_soa_reader.has_value() || group.target_index ||
        group.target_index_source != ::bioseq::pipeline_stats::target_index_source::persisted ||
        group.persisted_generation_id == 0u || group.persisted_source_identity == 0u ||
        !group.persisted_hash_major_source_identity_present ||
        group.persisted_hash_major_source_identity == std::array<std::byte, 16>{} ||
        group.persisted_source_identity !=
            hash_major_source_diagnostic_token(group.persisted_hash_major_source_identity) ||
        !group.query_hash_union.empty() || group.query_hash_union.capacity() != 0u ||
        group.persisted_hash_major_reader->target_length() != target_bp_size) {
        return false;
    }

    const auto& receipt = group.persisted_read_stats.hash_major;
    const auto& memory = group.persisted_hash_major_reader->memory_stats();
    const std::uint64_t stage_peak = std::max({
        receipt.directory_stage_peak_bytes,
        receipt.control_stage_peak_bytes,
        receipt.posting_stage_peak_bytes,
    });
    return receipt.layout_revision == bioseq::indexing::k_hash_major_minimizer_layout_revision &&
           receipt.sessions == 1u && receipt.generation_id == group.persisted_generation_id &&
           receipt.source_identity == group.persisted_hash_major_source_identity &&
           receipt.directory_row_crc_receipts == receipt.directory_rows &&
           receipt.control_row_crc_receipts == receipt.control_rows &&
           receipt.posting_row_crc_receipts == receipt.posting_rows &&
           receipt.control_block_crc_receipts == receipt.selected_block_count &&
           receipt.control_semantic_receipts == receipt.selected_block_count &&
           receipt.posting_ranges_delivered == receipt.posting_semantic_receipts &&
           receipt.posting_range_crc_receipts <= receipt.posting_ranges_delivered &&
           receipt.query_count == memory.query_count &&
           memory.query_count == group.canonical_query_hash_union_u32.size() &&
           receipt.accepted_hash_count == memory.accepted_hash_count &&
           receipt.retained_position_count == memory.retained_position_count &&
           receipt.peak_admitted_bytes == stage_peak &&
           receipt.retained_owner_bytes == receipt.admitted_bytes &&
           receipt.admitted_bytes == group.persisted_hash_major_reader->admitted_bytes() &&
           receipt.generic_source_fallback_calls == 0u && receipt.failures == 0u &&
           !receipt.saturated;
}

/// Checks a mapped fragment reader before query workers share it. The mapping
/// must stay alive, and its lookup format must match the prepared group. Query
/// routing and selected anchors are allocated later for each wave.
template <class p_prepared_index_group>
[[nodiscard]] inline bool
fragment_parallel_reader_identity_valid(const p_prepared_index_group& group,
                                        const std::uint64_t target_bp_size) noexcept {
    if (!group.persisted_fragment_reader.has_value() ||
        !group.persisted_fragment_mapping_guard.has_value() ||
        !*group.persisted_fragment_mapping_guard ||
        group.persisted_grouped_soa_reader.has_value() ||
        group.persisted_hash_major_reader.has_value() || group.target_index ||
        group.target_index_source != ::bioseq::pipeline_stats::target_index_source::persisted ||
        group.persisted_generation_id == 0u || group.persisted_source_identity == 0u ||
        group.persisted_hash_major_source_identity_present ||
        group.persisted_hash_major_source_identity != std::array<std::byte, 16>{} ||
        !group.query_hash_union.empty() || group.query_hash_union.capacity() != 0u ||
        group.persisted_fragment_reader->target_length() != target_bp_size) {
        return false;
    }
    const auto& read = group.persisted_read_stats;
    const auto& reader = *group.persisted_fragment_reader;
    const auto memory = reader.memory_stats();
    const bool exact_lookup_backing_identity = [&group, &memory, &read, &reader]() noexcept {
        switch (group.persisted_fragment_lookup_route) {
        case bioseq::indexing::minimizer_fragment_lookup_route::direct_compact_hash_lookup:
            return reader.compact_direct_ready() && reader.compact_direct_only() &&
                   reader.mapped_grouped_soa_payload_bytes() == 0u &&
                   reader.grouped_soa_directory_entry_count() == 0u &&
                   memory.directory_capacity_bytes == 0u &&
                   memory.ordinal_route_capacity_bytes == 0u && read.directory_chunks_read == 0u &&
                   read.directory_bytes_read == 0u;
        case bioseq::indexing::minimizer_fragment_lookup_route::grouped_soa_control_scans:
            return !reader.compact_direct_ready() && !reader.compact_direct_only() &&
                   reader.mapped_grouped_soa_payload_bytes() > 0u &&
                   reader.grouped_soa_directory_entry_count() > 0u &&
                   memory.directory_capacity_bytes > 0u &&
                   memory.ordinal_route_capacity_bytes > 0u && read.directory_chunks_read > 0u &&
                   read.directory_bytes_read > 0u;
        }
        return false;
    }();
    const bool deferred_query_set_identity =
        group.compact_direct_query_set_validation_pending
            ? group.persisted_fragment_lookup_route ==
                      bioseq::indexing::minimizer_fragment_lookup_route::
                          direct_compact_hash_lookup &&
                  group.canonical_query_hash_union_u32.empty() &&
                  group.canonical_query_hash_union_u32.capacity() == 0u &&
                  group.key_set_checksum == 0u && !group.build_set_checksum_present &&
                  group.build_set_checksum == 0u
            : group.build_set_checksum_present &&
                  group.build_set_checksum == group.key_set_checksum;
    return reader.ready() && exact_lookup_backing_identity && deferred_query_set_identity &&
           memory.query_record_count == 0u && memory.query_record_capacity_bytes == 0u &&
           memory.query_hash_capacity_bytes == 0u && memory.capacity_accounting_passes == 1u &&
           memory.global_position_storage_preparations == 0u &&
           memory.global_position_storage_allocations == 0u &&
           memory.position_append_growth_reservations == 0u &&
           memory.retained_position_count == 0u && memory.retained_position_capacity == 0u &&
           memory.global_position_capacity_bytes == 0u &&
           memory.query_record_position_capacity_bytes == 0u &&
           memory.retained_selected_control_capacity_bytes == 0u &&
           memory.current_selected_control_capacity_bytes == 0u &&
           memory.selected_extent_capacity_bytes == 0u &&
           memory.control_payload_capacity_bytes == 0u && memory.retained_layout_equation_holds() &&
           memory.owned_capacity_bytes == reader.admitted_bytes() && read.relation_opens == 1u &&
           read.head_scans == 1u && read.manifest_directory_scans == 1u && read.index_scans == 2u &&
           read.metadata_repeated_reads == 0u && read.full_generation.scan_calls == 0u &&
           read.bulk_read_calls == 0u && read.range_scan_calls == 0u &&
           read.payload_heap_rows_read == 0u && read.payload_bytes_read == 0u &&
           read.source_parent_rows_read == 1u && read.source_catalog_rows_read == 1u &&
           read.source_catalog_index_scans == 1u && read.source_fragment_rows_read == 0u &&
           read.source_decoded_bp == 0u;
}

/// Add the saved query-sketch statistics after its worker finishes.
/// Reject a result that already contains query-sketch counts, preventing
/// the same preparation work from being recorded twice
inline void
attach_retained_hash_major_query_stats(match_result_collection& collection,
                                       const pipe::minimizer_range_execution_stats& stats) {
    const bool exact_query_stats_absent =
        !collection.exact_simd_query_execution_stats_available &&
        collection.exact_simd_query_execution_stats ==
            decltype(collection.exact_simd_query_execution_stats){};
    const bool scalar_query_stats_absent =
        collection.scalar_pipeline_stats.query_minimizers ==
        decltype(collection.scalar_pipeline_stats.query_minimizers){};
    const bool bounded_query_stats_absent =
        !collection.bounded_nthash_query_execution_stats_available &&
        collection.bounded_nthash_query_execution_stats ==
            decltype(collection.bounded_nthash_query_execution_stats){};
    if (!scalar_query_stats_absent || !exact_query_stats_absent || !bounded_query_stats_absent) {
        raise_typed_dispatch_pg_error_as_cpp(
            ERRCODE_INTERNAL_ERROR, "persisted query sketch stats were recorded more than once");
    }
    if (stats.exact_simd_available) {
        collection.record_exact_simd_query_execution_stats(stats.exact_simd);
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        collection.record_completed_minimizer_algorithm_identity(
            minimizer_algorithm::two_stack_nthash);
#endif
    }
    if (stats.bounded_nthash_available) {
        collection.record_bounded_nthash_query_execution_stats(stats.bounded_nthash);
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        collection.record_completed_minimizer_algorithm_identity(
            minimizer_algorithm::bounded_nthash_simd);
#endif
    }
    if (stats.scalar_available) {
        collection.record_scalar_query_minimizer_execution_stats(stats.scalar);
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        collection.record_completed_minimizer_algorithm_identity(minimizer_algorithm::scalar);
#endif
    }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    // Record the reference algorithm  after its checked executio has completed.
    if (stats.checked_reference_simd_bridge_completed) {
        collection.record_completed_minimizer_algorithm_identity(
            minimizer_algorithm::simd_minimap2_reference);
    }
#endif
}

} // namespace bioseq::simd::typed_dispatch::typed_support_detail
