/**
 * @file generation_session_completion.cppm
 * @brief Defines grouped-generation close paths and final authorization equations.
 */
module;
#include <bioseq/pg_compat.h>
#include <libassert/assert.hpp>

extern "C" {
#include "access/heapam.h"
#include "access/nbtree.h"
#include "access/parallel.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h" // RelnameGetRelid
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "catalog/pg_opclass_d.h"
#include "catalog/pg_sequence.h"
#include "catalog/pg_type_d.h"
#include "commands/sequence.h"
#include "commands/tablespace.h"
#include "executor/instrument.h"
#include "executor/spi.h"
#include "executor/tuptable.h"
#include "storage/buffile.h"
#include "storage/dsm.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/procarray.h"
#include "storage/relfilelocator.h"
#include "storage/shm_toc.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/xid8.h"

#include "common/relpath.h"
#include "miscadmin.h"
#include "port.h"
#include "port/atomics.h"
}

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <expected>
#include <fcntl.h>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <queue>
#include <span>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <tuple>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>
export module bioseq.postgres.storage.indexing.chunked_storage:generation_session_completion;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.common.memory_budget;
import bioseq.postgres.support; // bioseq::postgres::support::try_open_postgres_relation, bioseq::postgres::support::find_primary_key_oid
import bioseq.postgres.support.datums;
import bioseq.postgres.sql_boundary;
import bioseq.postgres.storage.manager; // seq_caches, sequence_locator, format helpers
import bioseq.storage.layout;           // sequence_locator
import bioseq.storage.size;             // sequence_size_t
import bioseq.indexing.type; // sequence_index_layout, index_sub_key_t, k_max_index_chunk_size_bytes, sentinels
import bioseq.indexing.storage;           // the concepts
import bioseq.indexing.minimizer_hashmap; // encoded header/bucket validation
import bioseq.indexing.minimizer_fragment_image; // explicit fixed-endian minimizer fragment image image ABI
import bioseq.indexing.compact_hash32_direct_index;
import bioseq.indexing.minimizer_fragment_compact_hash32_adapter;
import bioseq.postgres.table.io; // begin_column_read/read_next_column_row/end_column_read/column_read_state
import bioseq.postgres.storage.scanner;        // PostgreSQL-backed lazy sequence access
import bioseq.storage.lazy_sequence;           // lazy_sequence
import bioseq.postgres.types.compact_sequence; // bioseq::pg_types::seq, dispatch_instance
import bioseq.sequences;                       // compact_sequence_range
import bioseq.sequences.simd;                  // canonical bounded base decoding
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;
import bioseq.postgres.storage.indexing.residency;
import bioseq.postgres.storage.indexing.sorted_runs;
import bioseq.instrumentation.clock;
import bioseq.postgres.column.table_access;
import bioseq.postgres.storage.metadata;
import bioseq.postgres.storage.physical;
import bioseq.postgres.storage.indexing.sql_row_codecs;
import bioseq.postgres.storage.manager.sql_row_codecs;
import bioseq.postgres.sql.generated.core.storage_indexing_pg_chunked_storage.entity_handles;
import :catalog;
import :fragment_image;
export import :generation_session_contract;
export import :generation_session_lifecycle;
export import :generation_session_metadata;
export import :generation_session_payload;
export namespace bioseq::storage::indexing {

inline void index_storage_fragment_generation_read_session::close_head_read() {
    try {
        if (state_ != operation_state::head_read || active_scan_ != nullptr ||
            heap_relation_ == nullptr || primary_index_relation_ == nullptr ||
            snapshot_ == nullptr) {
            fail_("head-only generation read closed in an invalid state");
        }
        state_ = operation_state::finished;
        close_relations_checked_();
    } catch (const bioseq::postgres::support::postgres_error&) {
        state_ = operation_state::failed;
        abandon_relations_noexcept_();
        throw;
    } catch (...) {
        state_ = operation_state::failed;
        throw;
    }
}

[[nodiscard]] inline minimizer_generation_read_stats
index_storage_fragment_generation_read_session::finish_mapped_minimizer_fragment_payload() {
    try {
        if (state_ != operation_state::metadata_ready || !catalog_source_authorized_ || !head_ ||
            !metadata_ ||
            metadata_->manifest.layout !=
                bioseq::indexing::minimizer_generation_layout::grouped_soa_segment_local ||
            active_scan_ != nullptr || heap_relation_ == nullptr ||
            primary_index_relation_ == nullptr || snapshot_ == nullptr ||
            storage_descriptor_relation_ == nullptr ||
            storage_descriptor_live_index_relation_ == nullptr || next_group_ != 0 ||
            !planned_groups_.empty() || !runtime_requests_.empty() || plan_permit_ ||
            runtime_permit_ || control_authorization_permit_ ||
            !authorized_position_ordinals_.empty() || stats_.payload_heap_rows_read != 0 ||
            stats_.payload_bytes_read != 0 || stats_.bulk_read_calls != 0 ||
            stats_.range_scan_calls != 0 || stats_.full_generation.scan_calls != 0) {
            fail_("mapped grouped-SoA generation read closed in an invalid state");
        }
        const std::uint64_t expected_metadata_rows =
            stats_.head_rows_read + stats_.manifest_rows_read + stats_.directory_chunks_read;
        const std::uint64_t expected_metadata_bytes =
            stats_.head_bytes_read + stats_.manifest_bytes_read + stats_.directory_bytes_read;
        if (stats_.relation_opens != 1 || stats_.head_scans != 1 ||
            stats_.manifest_directory_scans != 1 || stats_.index_scans != 2 ||
            stats_.metadata_heap_rows_read != expected_metadata_rows ||
            stats_.metadata_bytes_read != expected_metadata_bytes ||
            stats_.source_parent_rows_read != 1 || stats_.source_catalog_rows_read != 1 ||
            stats_.source_catalog_index_scans != 1 || stats_.source_fragment_rows_read != 0 ||
            stats_.source_decoded_bp != 0 || stats_.source_storage_incarnations_checked < 1 ||
            stats_.source_storage_incarnations_checked > 2) {
            fail_("mapped grouped-SoA metadata/source authorization disagrees");
        }
        stats_.total_heap_rows_read = stats_.metadata_heap_rows_read;
        stats_.total_bytes_read = stats_.metadata_bytes_read;
        stats_.cold.add(stats_.cold.toast_values, stats_.toast_values_read);
        stats_.cold.add(stats_.cold.detoast_bytes, stats_.detoast_bytes);
        state_ = operation_state::finished;
        close_relations_checked_();
        return stats_;
    } catch (...) {
        state_ = operation_state::failed;
        throw;
    }
}

[[nodiscard]] inline minimizer_generation_read_stats
index_storage_fragment_generation_read_session::finish_mapped_compact_direct() {
    try {
        const minimizer_generation_manifest* const manifest = active_manifest_();
        const bool cold_payload_stats_are_zero =
            stats_.cold.payload_range_scans == 0u && stats_.cold.route_empty == 0u &&
            stats_.cold.route_selective == 0u && stats_.cold.route_full == 0u &&
            stats_.cold.route_candidate_rows == 0u && stats_.cold.route_full_rows == 0u &&
            stats_.cold.route_candidate_bytes == 0u && stats_.cold.route_full_bytes == 0u &&
            stats_.cold.route_selective_cost == 0u && stats_.cold.route_full_cost == 0u &&
            stats_.cold.memory_reroutes == 0u && stats_.cold.payload_bytes_requested == 0u &&
            stats_.cold.payload_bytes_delivered == 0u && stats_.cold.payload_rows_requested == 0u &&
            stats_.cold.payload_rows_delivered == 0u && stats_.cold.payload_rows_scanned == 0u &&
            stats_.cold.payload_bytes_scanned == 0u && stats_.cold.payload_rows_skipped == 0u &&
            stats_.cold.heap_tid_adjacent == 0u && stats_.cold.heap_tid_forward_gap == 0u &&
            stats_.cold.heap_tid_backward_jump == 0u && stats_.cold.extent_crc_bytes == 0u &&
            stats_.cold.extent_crc_raw_bytes_hashed == 0u &&
            stats_.cold.extent_crc_combined_bytes == 0u &&
            stats_.cold.extent_crc_combine_rows == 0u && stats_.cold.payload_ns == 0u &&
            stats_.cold.decode_ns == 0u && stats_.cold.transfer_ns == 0u;
        if (state_ != operation_state::metadata_ready || !catalog_source_authorized_ || !head_ ||
            manifest == nullptr || !manifest_only_ || metadata_ || manifest != &*manifest_only_ ||
            manifest->layout !=
                bioseq::indexing::minimizer_generation_layout::grouped_soa_segment_local ||
            active_scan_ != nullptr || heap_relation_ == nullptr ||
            primary_index_relation_ == nullptr || snapshot_ == nullptr ||
            storage_descriptor_relation_ == nullptr ||
            storage_descriptor_live_index_relation_ == nullptr || next_group_ != 0u ||
            !planned_groups_.empty() || !runtime_requests_.empty() || plan_permit_ ||
            runtime_permit_ || control_authorization_permit_ || metadata_permit_ ||
            !authorized_position_ordinals_.empty() || !directory_bytes_scratch_.empty() ||
            previous_payload_heap_block_ || payload_stage_begin_ns_ ||
            stats_.payload_heap_rows_read != 0u || stats_.payload_bytes_read != 0u ||
            stats_.bulk_read_calls != 0u || stats_.planned_request_groups != 0u ||
            stats_.executed_request_groups != 0u || stats_.range_scan_calls != 0u ||
            stats_.cache_lookups != 0u || stats_.cache_populations != 0u ||
            !read_stats_are_zero_(stats_.payload) || !section_stats_are_zero_(stats_.controls) ||
            !section_stats_are_zero_(stats_.positions) ||
            !full_payload_stats_are_zero_(stats_.full_generation) || !cold_payload_stats_are_zero) {
            fail_("mapped compact-H32 generation read closed in an invalid state");
        }

        constexpr std::uint64_t k_expected_manifest_bytes =
            minimizer_generation_manifest::encoded_bytes;
        constexpr std::uint64_t k_expected_metadata_bytes =
            minimizer_generation_head::encoded_bytes + k_expected_manifest_bytes;
        if (stats_.relation_opens != 1u || stats_.head_scans != 1u ||
            stats_.cold.head_scans != 1u || stats_.head_rows_read != 1u ||
            stats_.head_bytes_read != minimizer_generation_head::encoded_bytes ||
            stats_.manifest_directory_scans != 1u || stats_.cold.directory_scans != 1u ||
            stats_.manifest_rows_read != 1u ||
            stats_.manifest_bytes_read != k_expected_manifest_bytes ||
            stats_.directory_chunks_read != 0u || stats_.directory_bytes_read != 0u ||
            stats_.metadata_repeated_reads != 0u || stats_.metadata_heap_rows_read != 2u ||
            stats_.metadata_bytes_read != k_expected_metadata_bytes || stats_.index_scans != 2u ||
            stats_.explicit_copy_bytes != k_expected_metadata_bytes ||
            stats_.toast_values_read != 0u || stats_.detoast_bytes != 0u ||
            stats_.total_heap_rows_read != 0u || stats_.total_bytes_read != 0u ||
            stats_.source_parent_rows_read != 1u || stats_.source_catalog_rows_read != 1u ||
            stats_.source_catalog_index_scans != 1u || stats_.source_fragment_rows_read != 0u ||
            stats_.source_decoded_bp != 0u || stats_.source_storage_incarnations_checked < 1u ||
            stats_.source_storage_incarnations_checked > 2u) {
            fail_("mapped compact-H32 metadata/source validation disagrees");
        }

        stats_.total_heap_rows_read = 2u;
        stats_.total_bytes_read = k_expected_metadata_bytes;
        state_ = operation_state::finished;
        close_relations_checked_();
        return stats_;
    } catch (...) {
        state_ = operation_state::failed;
        throw;
    }
}

inline void index_storage_fragment_generation_read_session::close_metadata_read() {
    try {
        if ((state_ != operation_state::metadata_pending_source &&
             state_ != operation_state::metadata_ready) ||
            active_manifest_() == nullptr || active_scan_ != nullptr || heap_relation_ == nullptr ||
            primary_index_relation_ == nullptr || snapshot_ == nullptr) {
            fail_("metadata-only generation read closed in an invalid state");
        }
        state_ = operation_state::finished;
        close_relations_checked_();
    } catch (const bioseq::postgres::support::postgres_error&) {
        state_ = operation_state::failed;
        abandon_relations_noexcept_();
        throw;
    } catch (...) {
        state_ = operation_state::failed;
        throw;
    }
}

[[nodiscard]] inline minimizer_generation_read_stats
index_storage_fragment_generation_read_session::finish() {
    minimizer_generation_read_stats result{};
    try {
        if (state_ == operation_state::full_generation_payload_complete) {
            return finish_full_generation_();
        }
        if (state_ != operation_state::payload_complete || next_group_ != 0 ||
            !planned_groups_.empty() || !runtime_requests_.empty() || plan_permit_ ||
            runtime_permit_ || control_authorization_permit_ ||
            !authorized_position_ordinals_.empty() || active_scan_ != nullptr ||
            heap_relation_ == nullptr || primary_index_relation_ == nullptr ||
            snapshot_ == nullptr) {
            fail_("generation read finished before position payload completed or with leaked "
                  "section state");
        }
        std::uint64_t expected_metadata_rows = 0;
        checked_add_(expected_metadata_rows,
                     stats_.head_rows_read,
                     "metadata authorization row equation overflow");
        checked_add_(expected_metadata_rows,
                     stats_.manifest_rows_read,
                     "metadata authorization row equation overflow");
        checked_add_(expected_metadata_rows,
                     stats_.directory_chunks_read,
                     "metadata authorization row equation overflow");
        std::uint64_t expected_metadata_bytes = 0;
        checked_add_(expected_metadata_bytes,
                     stats_.head_bytes_read,
                     "metadata authorization byte equation overflow");
        checked_add_(expected_metadata_bytes,
                     stats_.manifest_bytes_read,
                     "metadata authorization byte equation overflow");
        checked_add_(expected_metadata_bytes,
                     stats_.directory_bytes_read,
                     "metadata authorization byte equation overflow");
        std::uint64_t expected_index_scans = 0;
        checked_add_(
            expected_index_scans, stats_.head_scans, "index scan authorization equation overflow");
        checked_add_(expected_index_scans,
                     stats_.manifest_directory_scans,
                     "index scan authorization equation overflow");
        checked_add_(expected_index_scans,
                     stats_.range_scan_calls,
                     "index scan authorization equation overflow");
        std::uint64_t expected_payload_rows = 0;
        checked_add_(expected_payload_rows,
                     stats_.payload.delivered_chunks,
                     "payload authorization row equation overflow");
        checked_add_(expected_payload_rows,
                     stats_.payload.skipped_rows,
                     "payload authorization row equation overflow");
        std::uint64_t cold_tid_transitions = 0;
        checked_add_(cold_tid_transitions,
                     stats_.cold.heap_tid_adjacent,
                     "payload TID transition equation overflow");
        checked_add_(cold_tid_transitions,
                     stats_.cold.heap_tid_forward_gap,
                     "payload TID transition equation overflow");
        checked_add_(cold_tid_transitions,
                     stats_.cold.heap_tid_backward_jump,
                     "payload TID transition equation overflow");
        if (stats_.relation_opens != 1 || stats_.head_scans != 1 || stats_.head_rows_read != 1 ||
            stats_.head_bytes_read != minimizer_generation_head::encoded_bytes ||
            stats_.manifest_directory_scans != 1 || stats_.manifest_rows_read != 1 ||
            stats_.manifest_bytes_read !=
                bioseq::indexing::minimizer_manifest_encoded_bytes(metadata_->manifest.layout) ||
            stats_.metadata_heap_rows_read != expected_metadata_rows ||
            stats_.metadata_bytes_read != expected_metadata_bytes ||
            !section_equations_hold_(stats_.controls) ||
            !section_equations_hold_(stats_.positions) ||
            stats_.controls.control_authorization_checks != 0 ||
            stats_.positions.control_authorization_checks != stats_.positions.read.wanted_extents ||
            stats_.positions.read.wanted_extents > stats_.controls.read.wanted_extents ||
            !counter_is_sum_(stats_.planned_request_groups,
                             stats_.controls.planned_request_groups,
                             stats_.positions.planned_request_groups) ||
            !counter_is_sum_(stats_.executed_request_groups,
                             stats_.controls.executed_request_groups,
                             stats_.positions.executed_request_groups) ||
            !counter_is_sum_(stats_.range_scan_calls,
                             stats_.controls.range_scan_calls,
                             stats_.positions.range_scan_calls) ||
            !counter_is_sum_(stats_.payload_heap_rows_read,
                             stats_.controls.physical_heap_rows_read,
                             stats_.positions.physical_heap_rows_read) ||
            !counter_is_sum_(stats_.payload_bytes_read,
                             stats_.controls.physical_bytes_read,
                             stats_.positions.physical_bytes_read) ||
            !read_stats_are_sum_(stats_.payload, stats_.controls.read, stats_.positions.read) ||
            stats_.planned_request_groups != stats_.executed_request_groups ||
            stats_.bulk_read_calls != 2 ||
            stats_.range_scan_calls != stats_.executed_request_groups ||
            stats_.payload.index_scans != stats_.executed_request_groups ||
            stats_.payload.requested_ranges != stats_.executed_request_groups ||
            stats_.index_scans != expected_index_scans ||
            stats_.payload.heap_rows_read != expected_payload_rows ||
            stats_.payload_heap_rows_read != stats_.payload.heap_rows_read ||
            stats_.payload_bytes_read < stats_.payload.delivered_bytes ||
            stats_.payload.skipped_rows != stats_.payload.skip_budget_rows ||
            stats_.cold.route_full != 0 ||
            !((stats_.cold.route_empty == 1 && stats_.cold.route_selective == 0) ||
              (stats_.cold.route_empty == 0 && stats_.cold.route_selective == 1)) ||
            stats_.cold.payload_rows_requested != stats_.payload.delivered_chunks ||
            stats_.cold.payload_rows_delivered != stats_.payload.delivered_chunks ||
            stats_.cold.payload_rows_scanned != stats_.payload.heap_rows_read ||
            stats_.cold.payload_rows_skipped != stats_.payload.skipped_rows ||
            stats_.cold.payload_bytes_requested != stats_.payload.delivered_bytes ||
            stats_.cold.payload_bytes_delivered != stats_.payload.delivered_bytes ||
            stats_.cold.payload_bytes_scanned != stats_.payload_bytes_read ||
            stats_.cold.row_crc_bytes != stats_.payload_bytes_read ||
            stats_.cold.extent_crc_bytes != stats_.payload.delivered_bytes ||
            stats_.cold.extent_crc_raw_bytes_hashed != 0 ||
            stats_.cold.extent_crc_combined_bytes != stats_.payload.delivered_bytes ||
            stats_.cold.extent_crc_combine_rows != stats_.payload.delivered_chunks ||
            (stats_.payload.heap_rows_read == 0
                 ? cold_tid_transitions != 0
                 : cold_tid_transitions != stats_.payload.heap_rows_read - 1) ||
            stats_.payload.wanted_subkeys != stats_.payload.read_subkeys ||
            stats_.payload.wanted_extents != stats_.payload.read_extents ||
            stats_.payload.wanted_chunks != stats_.payload.delivered_chunks ||
            stats_.cache_lookups != 0 || stats_.cache_populations != 0 ||
            stats_.toast_values_read != 0 || stats_.detoast_bytes != 0 ||
            (stats_.source_catalog_rows_read != 0 &&
             (stats_.source_parent_rows_read != 1 || stats_.source_catalog_rows_read != 1 ||
              stats_.source_catalog_index_scans != 1 || stats_.source_fragment_rows_read != 0 ||
              stats_.source_decoded_bp != 0 || stats_.source_storage_incarnations_checked < 1 ||
              stats_.source_storage_incarnations_checked > 2))) {
            fail_("generation read final authorization equations disagree");
        }
        stats_.total_heap_rows_read = 0;
        checked_add_(stats_.total_heap_rows_read,
                     stats_.metadata_heap_rows_read,
                     "total heap row authorization equation overflow");
        checked_add_(stats_.total_heap_rows_read,
                     stats_.payload_heap_rows_read,
                     "total heap row authorization equation overflow");
        stats_.total_bytes_read = 0;
        checked_add_(stats_.total_bytes_read,
                     stats_.metadata_bytes_read,
                     "total byte authorization equation overflow");
        checked_add_(stats_.total_bytes_read,
                     stats_.payload_bytes_read,
                     "total byte authorization equation overflow");
        stats_.cold.add(stats_.cold.toast_values, stats_.toast_values_read);
        stats_.cold.add(stats_.cold.detoast_bytes, stats_.detoast_bytes);
        finalize_payload_timing_();
        state_ = operation_state::finished;
        close_relations_checked_();
        result = stats_;
    } catch (...) {
        state_ = operation_state::failed;
        throw;
    }
    return result;
}

} // namespace bioseq::storage::indexing
