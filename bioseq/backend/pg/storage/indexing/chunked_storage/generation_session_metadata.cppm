/**
 * @file generation_session_metadata.cppm
 * @brief Defines grouped-generation head, manifest, source, and descriptor authentication.
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
export module bioseq.postgres.storage.indexing.chunked_storage:generation_session_metadata;
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
export namespace bioseq::storage::indexing {

[[nodiscard]] inline minimizer_generation_head
index_storage_fragment_generation_read_session::read_active_head_impl_(bool bind_expected_layout) {
    minimizer_generation_head result{};
    const std::uint64_t head_begin_ns = cold_index_now_ns();
    const BufferUsage head_buffer_begin = pgBufferUsage;
    try {
        if (state_ != operation_state::opened) {
            checked_add_(stats_.metadata_repeated_reads, 1, "repeated metadata counter overflow");
            fail_("active generation head was read more than once or out of order");
        }
        ScanKeyData keys[5];
        ScanKeyInit(&keys[0],
                    k_minimizer_generation_index_generation_id,
                    BTEqualStrategyNumber,
                    F_INT8EQ,
                    Int64GetDatum(0));
        ScanKeyInit(&keys[1],
                    k_minimizer_generation_index_query_key,
                    BTEqualStrategyNumber,
                    F_INT8EQ,
                    Int64GetDatum(static_cast<std::int64_t>(query_key_)));
        ScanKeyInit(&keys[2],
                    k_minimizer_generation_index_sub_key,
                    BTEqualStrategyNumber,
                    F_INT8EQ,
                    Int64GetDatum(static_cast<std::int64_t>(k_generation_head_sub_key)));
        ScanKeyInit(&keys[3],
                    k_minimizer_generation_index_position_segment,
                    BTEqualStrategyNumber,
                    F_INT4EQ,
                    Int32GetDatum(-1));
        ScanKeyInit(&keys[4],
                    k_minimizer_generation_index_chunk_number,
                    BTEqualStrategyNumber,
                    F_INT4EQ,
                    Int32GetDatum(0));
        active_scan_ = bioseq::postgres::support::invoke_pg_as_cpp([this, &keys] {
            return systable_beginscan_ordered(
                heap_relation_, primary_index_relation_, snapshot_, 5, keys);
        });
        const auto next_tuple = [this] {
            return bioseq::postgres::support::invoke_pg_as_cpp(
                [this] { return systable_getnext_ordered(active_scan_, ForwardScanDirection); });
        };
        HeapTuple tuple = next_tuple();
        if (!HeapTupleIsValid(tuple)) {
            fail_("active generation head is missing");
        }
        loaded_row_type row = load_minimizer_generation_row_or_error(heap_relation_, tuple);
        std::array<std::byte, minimizer_generation_head::encoded_bytes> bytes{};
        if (row.query_key != query_key_ ||
            row.address !=
                index_chunk_address{
                    .generation_id = 0,
                    .position_segment = -1,
                    .sub_key = k_generation_head_sub_key,
                    .chunk_no = 0,
                } ||
            row.bytes.size() != bytes.size()) {
            row.release();
            fail_("active generation head row shape disagrees");
        }
        std::copy(row.bytes.begin(), row.bytes.end(), bytes.begin());
        row.release();
        if (HeapTupleIsValid(next_tuple())) {
            fail_("active generation head scan returned duplicate rows");
        }
        bioseq::postgres::support::invoke_pg_as_cpp(
            [this] { systable_endscan_ordered(active_scan_); });
        active_scan_ = nullptr;
        auto decoded = bioseq::indexing::decode_minimizer_generation_head(bytes);
        if (!decoded || !bioseq::indexing::sql_bigint_can_represent(decoded->generation_id)) {
            fail_("active generation head fixed wire is invalid");
        }
        if (bind_expected_layout && decoded->layout != expected_profile_.layout) {
            fail_("active generation head wire format disagrees with reader");
        }
        result = *decoded;
        head_ = result;
        state_ = operation_state::head_read;
        stats_.head_scans = 1;
        stats_.cold.head_scans = 1;
        stats_.head_rows_read = 1;
        stats_.head_bytes_read = minimizer_generation_head::encoded_bytes;
        stats_.index_scans = 1;
        stats_.explicit_copy_bytes = minimizer_generation_head::encoded_bytes;
        add_cold_index_elapsed(stats_.cold, stats_.cold.head_ns, head_begin_ns);
        add_cold_index_buffer_usage(stats_.cold, head_buffer_begin);
    } catch (const bioseq::postgres::support::postgres_error&) {
        state_ = operation_state::failed;
        abandon_relations_noexcept_();
        throw;
    }
    return result;
}

[[nodiscard]] inline minimizer_generation_head
index_storage_fragment_generation_read_session::read_active_head_once() {
    return read_active_head_impl_(/*bind_expected_layout=*/true);
}

[[nodiscard]] inline minimizer_generation_head
index_storage_fragment_generation_read_session::discover_active_head_layout_and_close() {
    minimizer_generation_head result = read_active_head_impl_(
        /*bind_expected_layout=*/false);
    close_head_read();
    return result;
}

[[nodiscard]] inline const minimizer_generation_manifest&
index_storage_fragment_generation_read_session::read_manifest_once_without_directory(
    const minimizer_generation_head& expected_head) {
    const std::uint64_t manifest_begin_ns = cold_index_now_ns();
    const BufferUsage manifest_buffer_begin = pgBufferUsage;
    try {
        if (state_ != operation_state::head_read || !head_ || expected_head != *head_ ||
            metadata_ || manifest_only_ ||
            expected_head.layout !=
                bioseq::indexing::minimizer_generation_layout::grouped_soa_segment_local) {
            checked_add_(stats_.metadata_repeated_reads, 1, "repeated metadata counter overflow");
            fail_("manifest-only read repeated, early, changed, or not grouped-SoA");
        }

        ScanKeyData keys[5];
        ScanKeyInit(&keys[0],
                    k_minimizer_generation_index_generation_id,
                    BTEqualStrategyNumber,
                    F_INT8EQ,
                    Int64GetDatum(static_cast<std::int64_t>(expected_head.generation_id)));
        ScanKeyInit(&keys[1],
                    k_minimizer_generation_index_query_key,
                    BTEqualStrategyNumber,
                    F_INT8EQ,
                    Int64GetDatum(static_cast<std::int64_t>(query_key_)));
        ScanKeyInit(&keys[2],
                    k_minimizer_generation_index_sub_key,
                    BTEqualStrategyNumber,
                    F_INT8EQ,
                    Int64GetDatum(static_cast<std::int64_t>(k_directory_sub_key)));
        ScanKeyInit(&keys[3],
                    k_minimizer_generation_index_position_segment,
                    BTEqualStrategyNumber,
                    F_INT4EQ,
                    Int32GetDatum(-1));
        ScanKeyInit(&keys[4],
                    k_minimizer_generation_index_chunk_number,
                    BTEqualStrategyNumber,
                    F_INT4EQ,
                    Int32GetDatum(0));
        active_scan_ = bioseq::postgres::support::invoke_pg_as_cpp([this, &keys] {
            return systable_beginscan_ordered(
                heap_relation_, primary_index_relation_, snapshot_, 5, keys);
        });
        const auto next_tuple = [this] {
            return bioseq::postgres::support::invoke_pg_as_cpp(
                [this] { return systable_getnext_ordered(active_scan_, ForwardScanDirection); });
        };
        HeapTuple tuple = next_tuple();
        if (!HeapTupleIsValid(tuple)) {
            fail_("READY generation manifest is missing");
        }
        loaded_row_type manifest_row =
            load_minimizer_generation_row_or_error(heap_relation_, tuple);
        std::array<std::byte, minimizer_generation_manifest::encoded_bytes> manifest_bytes{};
        constexpr std::size_t k_expected_manifest_bytes =
            minimizer_generation_manifest::encoded_bytes;
        if (manifest_row.query_key != query_key_ ||
            manifest_row.address !=
                index_chunk_address{
                    .generation_id = expected_head.generation_id,
                    .position_segment = -1,
                    .sub_key = k_directory_sub_key,
                    .chunk_no = 0,
                } ||
            manifest_row.bytes.size() != k_expected_manifest_bytes) {
            manifest_row.release();
            fail_("READY generation manifest row shape disagrees");
        }
        std::copy(manifest_row.bytes.begin(), manifest_row.bytes.end(), manifest_bytes.begin());
        manifest_row.release();
        if (HeapTupleIsValid(next_tuple())) {
            fail_("READY generation manifest scan returned duplicate rows");
        }
        bioseq::postgres::support::invoke_pg_as_cpp(
            [this] { systable_endscan_ordered(active_scan_); });
        active_scan_ = nullptr;

        const std::span<const std::byte> encoded_manifest{manifest_bytes};
        auto manifest = bioseq::indexing::decode_minimizer_generation_manifest(encoded_manifest);
        if (!manifest || manifest->layout != expected_head.layout ||
            manifest->generation_id != expected_head.generation_id ||
            manifest->source_identity != expected_head.source_identity ||
            manifest->source_content_checksum != expected_head.source_content_checksum ||
            manifest->directory_bytes > (std::numeric_limits<std::size_t>::max)() ||
            bioseq::indexing::crc64_ecma182(encoded_manifest) != expected_head.manifest_checksum ||
            bioseq::indexing::compute_minimizer_generation_storage_contract_checksum(*manifest) !=
                expected_head.storage_contract_checksum ||
            !bioseq::indexing::validate_minimizer_generation_storage_contract(*manifest,
                                                                              expected_)) {
            fail_("READY generation manifest/head binding disagrees");
        }
        const auto manifest_profile =
            bioseq::indexing::derive_grouped_soa_minimizer_profile(*manifest);
        if (!manifest_profile || manifest_profile->layout != expected_.layout) {
            fail_("READY generation manifest payload profile is invalid");
        }
        const std::uint32_t extent_payload_bytes =
            bioseq::indexing::minimizer_extent_payload_bytes(manifest->layout);
        if (extent_payload_bytes == 0u) {
            fail_("READY manifest wire quantum is invalid");
        }
        const std::uint64_t expected_directory_chunks =
            manifest->directory_bytes / extent_payload_bytes +
            (manifest->directory_bytes % extent_payload_bytes != 0u ? 1u : 0u);
        const std::size_t directory_entry_bytes =
            bioseq::indexing::minimizer_directory_entry_encoded_bytes(*manifest_profile);
        if (expected_directory_chunks != manifest->directory_chunk_count ||
            directory_entry_bytes == 0u ||
            manifest->directory_bytes % directory_entry_bytes != 0u) {
            fail_("READY manifest fixed directory equations disagree");
        }

        expected_profile_ = *manifest_profile;
        manifest_only_.emplace(*manifest);
        state_ = operation_state::metadata_pending_source;
        stats_.manifest_directory_scans = 1u;
        stats_.cold.directory_scans = 1u;
        stats_.manifest_rows_read = 1u;
        stats_.manifest_bytes_read = k_expected_manifest_bytes;
        stats_.directory_chunks_read = 0u;
        stats_.directory_bytes_read = 0u;
        stats_.metadata_heap_rows_read = 2u;
        stats_.metadata_bytes_read =
            minimizer_generation_head::encoded_bytes + k_expected_manifest_bytes;
        checked_add_(stats_.index_scans, 1u, "metadata index scan count overflow");
        checked_add_(stats_.explicit_copy_bytes,
                     k_expected_manifest_bytes,
                     "metadata copy byte count overflow");
        add_cold_index_elapsed(stats_.cold, stats_.cold.directory_ns, manifest_begin_ns);
        add_cold_index_buffer_usage(stats_.cold, manifest_buffer_begin);
    } catch (const bioseq::postgres::support::postgres_error&) {
        state_ = operation_state::failed;
        abandon_relations_noexcept_();
        throw;
    }
    return *manifest_only_;
}

[[nodiscard]] inline const minimizer_generation_read_metadata&
index_storage_fragment_generation_read_session::read_manifest_directory_once(
    const minimizer_generation_head& expected_head) {
    const std::uint64_t directory_begin_ns = cold_index_now_ns();
    const BufferUsage directory_buffer_begin = pgBufferUsage;
    try {
        if (state_ != operation_state::head_read || !head_ || expected_head != *head_ ||
            metadata_ || manifest_only_) {
            checked_add_(stats_.metadata_repeated_reads, 1, "repeated metadata counter overflow");
            fail_("manifest/directory read repeated, early, or changed head identity");
        }
        ScanKeyData keys[4];
        ScanKeyInit(&keys[0],
                    k_minimizer_generation_index_generation_id,
                    BTEqualStrategyNumber,
                    F_INT8EQ,
                    Int64GetDatum(static_cast<std::int64_t>(expected_head.generation_id)));
        ScanKeyInit(&keys[1],
                    k_minimizer_generation_index_query_key,
                    BTEqualStrategyNumber,
                    F_INT8EQ,
                    Int64GetDatum(static_cast<std::int64_t>(query_key_)));
        ScanKeyInit(&keys[2],
                    k_minimizer_generation_index_sub_key,
                    BTEqualStrategyNumber,
                    F_INT8EQ,
                    Int64GetDatum(static_cast<std::int64_t>(k_directory_sub_key)));
        ScanKeyInit(&keys[3],
                    k_minimizer_generation_index_position_segment,
                    BTEqualStrategyNumber,
                    F_INT4EQ,
                    Int32GetDatum(-1));
        active_scan_ = bioseq::postgres::support::invoke_pg_as_cpp([this, &keys] {
            return systable_beginscan_ordered(
                heap_relation_, primary_index_relation_, snapshot_, 4, keys);
        });
        const auto next_tuple = [this] {
            return bioseq::postgres::support::invoke_pg_as_cpp(
                [this] { return systable_getnext_ordered(active_scan_, ForwardScanDirection); });
        };
        HeapTuple tuple = next_tuple();
        if (!HeapTupleIsValid(tuple)) {
            fail_("READY generation manifest is missing");
        }
        loaded_row_type manifest_row =
            load_minimizer_generation_row_or_error(heap_relation_, tuple);
        std::array<std::byte, minimizer_generation_manifest::encoded_bytes> manifest_bytes{};
        const std::size_t expected_manifest_bytes =
            bioseq::indexing::minimizer_manifest_encoded_bytes(expected_head.layout);
        if (manifest_row.query_key != query_key_ ||
            manifest_row.address !=
                index_chunk_address{
                    .generation_id = expected_head.generation_id,
                    .position_segment = -1,
                    .sub_key = k_directory_sub_key,
                    .chunk_no = 0,
                } ||
            expected_manifest_bytes == 0 || manifest_row.bytes.size() != expected_manifest_bytes) {
            manifest_row.release();
            fail_("READY generation manifest row shape disagrees");
        }
        std::copy_n(manifest_row.bytes.begin(), expected_manifest_bytes, manifest_bytes.begin());
        manifest_row.release();
        auto manifest = bioseq::indexing::decode_minimizer_generation_manifest(
            std::span<const std::byte>{manifest_bytes}.first(expected_manifest_bytes));
        if (!manifest || manifest->generation_id != expected_head.generation_id ||
            manifest->source_identity != expected_head.source_identity ||
            manifest->directory_bytes > (std::numeric_limits<std::size_t>::max)()) {
            fail_("READY generation manifest fixed wire disagrees");
        }
        if (!bioseq::indexing::validate_minimizer_generation_storage_contract(*manifest,
                                                                              expected_)) {
            fail_("READY generation minimizer storage contract disagrees");
        }
        const auto manifest_profile =
            bioseq::indexing::derive_grouped_soa_minimizer_profile(*manifest);
        if (!manifest_profile || manifest_profile->layout != expected_.layout) {
            fail_("READY generation manifest payload profile is invalid");
        }
        expected_profile_ = *manifest_profile;
        const std::uint32_t extent_payload_bytes =
            bioseq::indexing::minimizer_extent_payload_bytes(manifest->layout);
        if (extent_payload_bytes == 0) {
            fail_("READY manifest wire quantum is invalid");
        }
        const std::uint64_t expected_directory_chunks =
            manifest->directory_bytes / extent_payload_bytes +
            (manifest->directory_bytes % extent_payload_bytes != 0 ? 1u : 0u);
        if (expected_directory_chunks != manifest->directory_chunk_count) {
            fail_("READY manifest directory chunk equation disagrees");
        }
        const std::size_t directory_entry_bytes =
            bioseq::indexing::minimizer_directory_entry_encoded_bytes(expected_profile_);
        if (directory_entry_bytes == 0 || manifest->directory_bytes % directory_entry_bytes != 0) {
            fail_("READY manifest directory entry equation disagrees");
        }
        const std::uint64_t entry_count = manifest->directory_bytes / directory_entry_bytes;
        if (entry_count > (std::numeric_limits<std::uint64_t>::max)() /
                              sizeof(grouped_soa_bucket_directory_entry)) {
            fail_("READY metadata admission size overflow");
        }
        const std::uint64_t entry_storage =
            entry_count * sizeof(grouped_soa_bucket_directory_entry);
        const std::uint64_t directory_owned_logical =
            bioseq::common::saturating_add(manifest->directory_bytes, entry_storage);
        const std::uint64_t scratch_headroom = bioseq::common::saturating_mul(
            manifest->directory_bytes, bioseq::parallelism::k_vector_preallocation_headroom_factor);
        const std::uint64_t admitted_bytes =
            bioseq::common::saturating_add(scratch_headroom, directory_owned_logical);
        if (directory_owned_logical == (std::numeric_limits<std::uint64_t>::max)() ||
            scratch_headroom == (std::numeric_limits<std::uint64_t>::max)() ||
            admitted_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
            fail_("READY metadata admission size overflow");
        }
        auto metadata_permit =
            bioseq::parallelism::memory_admission_controller::instance().try_reserve(
                admitted_bytes);
        if (!metadata_permit) {
            fail_("READY metadata memory admission was denied");
        }
        metadata_permit_ = std::move(*metadata_permit);
        directory_bytes_scratch_.clear();
        directory_bytes_scratch_.reserve(static_cast<std::size_t>(manifest->directory_bytes));
        const std::uint64_t measured_scratch_bytes =
            bioseq::common::saturating_mul(directory_bytes_scratch_.capacity(), sizeof(std::byte));
        const std::uint64_t measured_metadata_bytes =
            bioseq::common::saturating_add(measured_scratch_bytes, directory_owned_logical);
        if (measured_scratch_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            measured_metadata_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            measured_metadata_bytes > admitted_bytes) {
            fail_("READY metadata scratch capacity exceeded admission");
        }
        metadata_permit_.shrink_to(measured_metadata_bytes);
        for (std::uint32_t chunk_no = 1; chunk_no <= manifest->directory_chunk_count; ++chunk_no) {
            tuple = next_tuple();
            if (!HeapTupleIsValid(tuple)) {
                fail_("READY generation directory has missing chunks");
            }
            loaded_row_type directory_row =
                load_minimizer_generation_row_or_error(heap_relation_, tuple);
            const std::uint64_t offset =
                static_cast<std::uint64_t>(chunk_no - 1u) * extent_payload_bytes;
            const std::uint64_t expected_bytes =
                std::min<std::uint64_t>(extent_payload_bytes, manifest->directory_bytes - offset);
            if (directory_row.query_key != query_key_ ||
                directory_row.address !=
                    index_chunk_address{
                        .generation_id = expected_head.generation_id,
                        .position_segment = -1,
                        .sub_key = k_directory_sub_key,
                        .chunk_no = chunk_no,
                    } ||
                directory_row.bytes.size() != expected_bytes) {
                directory_row.release();
                fail_("READY generation directory row shape disagrees");
            }
            directory_bytes_scratch_.insert(directory_bytes_scratch_.end(),
                                            directory_row.bytes.begin(),
                                            directory_row.bytes.end());
            directory_row.release();
        }
        if (HeapTupleIsValid(next_tuple())) {
            fail_("READY generation directory scan returned extra rows");
        }
        bioseq::postgres::support::invoke_pg_as_cpp(
            [this] { systable_endscan_ordered(active_scan_); });
        active_scan_ = nullptr;
        if (directory_bytes_scratch_.size() != manifest->directory_bytes ||
            bioseq::indexing::crc64_ecma182(std::span<const std::byte>{directory_bytes_scratch_}) !=
                manifest->directory_checksum) {
            fail_("READY generation directory aggregate checksum disagrees");
        }
        auto directory = grouped_soa_directory_view::decode_admitted(
            directory_bytes_scratch_, expected_profile_, metadata_permit_);
        if (!directory) {
            fail_("READY generation directory fixed wire is invalid");
        }
        metadata_.emplace(minimizer_generation_read_metadata{
            .head = expected_head,
            .manifest = *manifest,
            .directory = std::move(*directory),
        });
        std::vector<std::byte>{}.swap(directory_bytes_scratch_);
        if (auto metadata = bioseq::indexing::validate_minimizer_generation_metadata(
                expected_head, metadata_->manifest, metadata_->directory);
            !metadata) {
            fail_("READY generation metadata equations disagree");
        }
        std::optional<std::pair<index_sub_key_t, std::int32_t>> previous_storage_key;
        std::uint64_t full_payload_rows = 0;
        for (const grouped_soa_bucket_directory_entry& entry : metadata_->directory.entries()) {
            const std::pair storage_key{entry.sub_key, entry.position_segment};
            if (previous_storage_key && !(*previous_storage_key < storage_key)) {
                fail_("READY directory is not in physical storage order");
            }
            previous_storage_key = storage_key;
            stats_.cold.add(full_payload_rows, entry.control_chunk_count);
            stats_.cold.add(full_payload_rows, entry.position_chunk_count);
        }
        stats_.cold.route_full_rows = full_payload_rows;
        stats_.cold.add(stats_.cold.route_full_bytes, metadata_->manifest.payload_bytes);
        stats_.cold.add(stats_.cold.route_full_cost, full_payload_rows);
        stats_.cold.add(stats_.cold.route_full_cost, k_ordered_range_scan_penalty_rows);
        metadata_permit_.shrink_to(metadata_->directory.owned_capacity_bytes());
        state_ = operation_state::metadata_pending_source;
        stats_.manifest_directory_scans = 1;
        stats_.cold.directory_scans = 1;
        stats_.manifest_rows_read = 1;
        stats_.manifest_bytes_read = expected_manifest_bytes;
        stats_.directory_chunks_read = manifest->directory_chunk_count;
        stats_.directory_bytes_read = manifest->directory_bytes;
        stats_.metadata_heap_rows_read = 2u + manifest->directory_chunk_count;
        stats_.metadata_bytes_read = minimizer_generation_head::encoded_bytes +
                                     expected_manifest_bytes + manifest->directory_bytes;
        checked_add_(stats_.index_scans, 1, "metadata index scan count overflow");
        checked_add_(stats_.explicit_copy_bytes,
                     expected_manifest_bytes,
                     "metadata copy byte count overflow");
        checked_add_(stats_.explicit_copy_bytes,
                     manifest->directory_bytes,
                     "directory copy byte count overflow");
        add_cold_index_elapsed(stats_.cold, stats_.cold.directory_ns, directory_begin_ns);
        add_cold_index_buffer_usage(stats_.cold, directory_buffer_begin);
    } catch (const bioseq::postgres::support::postgres_error&) {
        state_ = operation_state::failed;
        abandon_relations_noexcept_();
        throw;
    }
    return *metadata_;
}

inline void index_storage_fragment_generation_read_session::consume_full_source_authorization(
    minimizer_generation_source_authorization&& authorization) {
    try {
        const minimizer_generation_manifest* const manifest = active_manifest_();
        if (state_ != operation_state::metadata_pending_source || !head_ || manifest == nullptr) {
            authorization.invalidate_();
            fail_("READY source authorization was consumed out of order");
        }
        if (!authorization.valid_ || authorization.snapshot_ != snapshot_ ||
            authorization.database_oid_ != MyDatabaseId ||
            authorization.source_.sequence_rowno != query_key_ ||
            authorization.table_name_ != table_name_) {
            authorization.invalidate_();
            fail_("READY source authorization relation/query/snapshot binding disagrees");
        }
        const std::uint64_t storage_contract_checksum =
            bioseq::indexing::compute_minimizer_generation_storage_contract_checksum(*manifest);
        authorization.source_.storage_contract_checksum = storage_contract_checksum;
        const std::uint64_t source_identity = minimizer_generation_source_identity(
            authorization.main_relfilenode_, authorization.source_);
        const std::uint64_t source_content_checksum = authorization.source_content_checksum_;
        if (source_identity == 0 ||
            manifest->alphabet_code != authorization.source_.alphabet_code ||
            manifest->storage_unit_byte_width != authorization.source_.storage_unit_byte_width ||
            manifest->target_length != authorization.source_.locator.logical_length ||
            head_->source_identity != source_identity ||
            manifest->source_identity != source_identity ||
            head_->source_content_checksum != source_content_checksum ||
            manifest->source_content_checksum != source_content_checksum ||
            head_->storage_contract_checksum != storage_contract_checksum ||
            bioseq::indexing::compute_minimizer_generation_storage_contract_checksum(*manifest) !=
                storage_contract_checksum) {
            authorization.invalidate_();
            fail_("READY metadata/source authorization disagrees");
        }
        authorization.invalidate_();
        catalog_source_authorized_ = false;
        state_ = operation_state::metadata_ready;
    } catch (...) {
        authorization.invalidate_();
        state_ = operation_state::failed;
        throw;
    }
}

inline void index_storage_fragment_generation_read_session::consume_source_authorization(
    minimizer_generation_catalog_source_authorization&& authorization) {
    try {
        const minimizer_generation_manifest* const manifest = active_manifest_();
        if (state_ != operation_state::metadata_pending_source || !head_ || manifest == nullptr) {
            authorization.invalidate_();
            fail_("READY catalog source authorization was consumed out of order");
        }
        if (!authorization.valid_ || !authorization.stats_.timed_equation_holds() ||
            authorization.snapshot_ != snapshot_ || authorization.database_oid_ != MyDatabaseId ||
            authorization.source_.sequence_rowno != query_key_ ||
            authorization.table_name_ != table_name_) {
            authorization.invalidate_();
            fail_("READY catalog source authorization binding/work equation disagrees");
        }
        const std::uint64_t storage_contract_checksum =
            bioseq::indexing::compute_minimizer_generation_storage_contract_checksum(*manifest);
        authorization.source_.storage_contract_checksum = storage_contract_checksum;
        const std::uint64_t source_identity = minimizer_generation_source_identity(
            authorization.main_relfilenode_, authorization.source_);
        const std::uint64_t source_content_checksum = authorization.source_content_checksum_;
        if (source_identity == 0 ||
            manifest->alphabet_code != authorization.source_.alphabet_code ||
            manifest->storage_unit_byte_width != authorization.source_.storage_unit_byte_width ||
            manifest->target_length != authorization.source_.locator.logical_length ||
            head_->source_identity != source_identity ||
            manifest->source_identity != source_identity ||
            head_->source_content_checksum != source_content_checksum ||
            manifest->source_content_checksum != source_content_checksum ||
            head_->storage_contract_checksum != storage_contract_checksum) {
            authorization.invalidate_();
            fail_("READY metadata/catalog source authorization disagrees");
        }
        stats_.source_parent_rows_read = authorization.stats_.parent_rows_read;
        stats_.source_catalog_rows_read = authorization.stats_.catalog_rows_read;
        stats_.source_catalog_index_scans = authorization.stats_.catalog_index_scans;
        stats_.source_fragment_rows_read = authorization.stats_.fragment_rows_read;
        stats_.source_decoded_bp = authorization.stats_.decoded_bp;
        stats_.source_storage_incarnations_checked =
            authorization.stats_.storage_incarnations_checked;
        stats_.cold.merge(authorization.cold_stats_);
        authorization.invalidate_();
        catalog_source_authorized_ = true;
        state_ = operation_state::metadata_ready;
    } catch (...) {
        authorization.invalidate_();
        state_ = operation_state::failed;
        throw;
    }
}

[[nodiscard]] inline minimizer_generation_storage_descriptor
index_storage_fragment_generation_read_session::read_live_storage_descriptor_once(
    const minimizer_generation_administration_identity& identity) {
    try {
        const minimizer_generation_manifest* const manifest = active_manifest_();
        if (state_ != operation_state::metadata_ready || !catalog_source_authorized_ || !head_ ||
            manifest == nullptr ||
            manifest->layout !=
                bioseq::indexing::minimizer_generation_layout::grouped_soa_segment_local ||
            storage_descriptor_relation_ != nullptr ||
            storage_descriptor_live_index_relation_ != nullptr ||
            identity.table_name != table_name_ || identity.database_oid != MyDatabaseId ||
            identity.tablespace_oid != expected_tablespace_oid_ ||
            identity.index_name != k_minimizers_per_fragment_index_name) {
            fail_("LIVE generation descriptor was requested out of order");
        }
        const Oid descriptor_oid = bioseq::postgres::support::invoke_pg_as_cpp(
            [] { return get_relname_relid("minimizer_generation_storage", bioseq_internal_ns()); });
        const Oid live_index_oid = bioseq::postgres::support::invoke_pg_as_cpp([] {
            return get_relname_relid("minimizer_generation_storage_one_live", bioseq_internal_ns());
        });
        if (!OidIsValid(descriptor_oid) || !OidIsValid(live_index_oid)) {
            fail_("generation storage descriptor authority is missing");
        }
        storage_descriptor_relation_ =
            bioseq::postgres::support::invoke_pg_as_cpp([descriptor_oid] {
                return bioseq::postgres::support::try_open_postgres_relation(descriptor_oid,
                                                                             AccessShareLock);
            });
        if (storage_descriptor_relation_ == nullptr) {
            fail_("generation storage descriptor relation disappeared");
        }
        stats_.cold.add(stats_.cold.lock_acquires, 1);
        storage_descriptor_live_index_relation_ = bioseq::postgres::support::invoke_pg_as_cpp(
            [live_index_oid] { return index_open(live_index_oid, AccessShareLock); });
        stats_.cold.add(stats_.cold.lock_acquires, 1);
        if (RelationGetNamespace(storage_descriptor_relation_) != bioseq_internal_ns() ||
            std::string_view{RelationGetRelationName(storage_descriptor_relation_)} !=
                "minimizer_generation_storage" ||
            RelationGetNamespace(storage_descriptor_live_index_relation_) != bioseq_internal_ns()) {
            fail_("generation storage descriptor identity changed");
        }
        bioseq::storage::validate_minimizer_generation_storage_catalogs_pg();
        auto descriptor = read_live_minimizer_generation_storage_descriptor_pg(
            storage_descriptor_relation_,
            storage_descriptor_live_index_relation_,
            snapshot_,
            identity,
            query_key_,
            head_->generation_id);
        if (descriptor.payload_relation_oid !=
                std::optional<Oid>{RelationGetRelid(heap_relation_)} ||
            descriptor.payload_database_oid !=
                std::optional<Oid>{heap_relation_->rd_locator.dbOid} ||
            descriptor.payload_tablespace_oid !=
                std::optional<Oid>{heap_relation_->rd_locator.spcOid} ||
            descriptor.payload_relfilenode !=
                std::optional<Oid>{static_cast<Oid>(heap_relation_->rd_locator.relNumber)} ||
            descriptor.payload_relation_name != std::optional<std::string>{table_name_}) {
            fail_("LIVE relation-backed descriptor locator disagrees");
        }
        return descriptor;
    } catch (...) {
        cleanup_failed_session_and_rethrow_();
    }
}

[[nodiscard]] inline minimizer_generation_read_stats
index_storage_fragment_generation_read_session::finish_full_generation_() {
    if (state_ != operation_state::full_generation_payload_complete || !metadata_ ||
        next_group_ != 0 || !planned_groups_.empty() || !runtime_requests_.empty() ||
        plan_permit_ || runtime_permit_ || control_authorization_permit_ ||
        !authorized_position_ordinals_.empty() || active_scan_ != nullptr ||
        heap_relation_ == nullptr || primary_index_relation_ == nullptr || snapshot_ == nullptr) {
        fail_("full-generation read finished with incomplete or leaked state");
    }
    std::uint64_t expected_metadata_rows = 0;
    checked_add_(expected_metadata_rows,
                 stats_.head_rows_read,
                 "full-generation metadata row equation overflow");
    checked_add_(expected_metadata_rows,
                 stats_.manifest_rows_read,
                 "full-generation metadata row equation overflow");
    checked_add_(expected_metadata_rows,
                 stats_.directory_chunks_read,
                 "full-generation metadata row equation overflow");
    std::uint64_t expected_metadata_bytes = 0;
    checked_add_(expected_metadata_bytes,
                 stats_.head_bytes_read,
                 "full-generation metadata byte equation overflow");
    checked_add_(expected_metadata_bytes,
                 stats_.manifest_bytes_read,
                 "full-generation metadata byte equation overflow");
    checked_add_(expected_metadata_bytes,
                 stats_.directory_bytes_read,
                 "full-generation metadata byte equation overflow");
    std::uint64_t expected_index_scans = 0;
    checked_add_(
        expected_index_scans, stats_.head_scans, "full-generation index scan equation overflow");
    checked_add_(expected_index_scans,
                 stats_.manifest_directory_scans,
                 "full-generation index scan equation overflow");
    checked_add_(expected_index_scans,
                 stats_.full_generation.scan_calls,
                 "full-generation index scan equation overflow");
    std::uint64_t streamed_chunks = 0;
    checked_add_(streamed_chunks,
                 stats_.full_generation.control_chunks,
                 "full-generation chunk equation overflow");
    checked_add_(streamed_chunks,
                 stats_.full_generation.position_chunks,
                 "full-generation chunk equation overflow");
    std::uint64_t streamed_bytes = 0;
    checked_add_(streamed_bytes,
                 stats_.full_generation.control_bytes,
                 "full-generation byte equation overflow");
    checked_add_(streamed_bytes,
                 stats_.full_generation.position_bytes,
                 "full-generation byte equation overflow");
    std::uint64_t cold_tid_transitions = 0;
    checked_add_(cold_tid_transitions,
                 stats_.cold.heap_tid_adjacent,
                 "full-generation TID transition equation overflow");
    checked_add_(cold_tid_transitions,
                 stats_.cold.heap_tid_forward_gap,
                 "full-generation TID transition equation overflow");
    checked_add_(cold_tid_transitions,
                 stats_.cold.heap_tid_backward_jump,
                 "full-generation TID transition equation overflow");
    const std::uint64_t manifest_extents = metadata_->manifest.extent_count;
    if (stats_.relation_opens != 1 || stats_.head_scans != 1 || stats_.head_rows_read != 1 ||
        stats_.head_bytes_read != minimizer_generation_head::encoded_bytes ||
        stats_.manifest_directory_scans != 1 || stats_.manifest_rows_read != 1 ||
        stats_.manifest_bytes_read !=
            bioseq::indexing::minimizer_manifest_encoded_bytes(metadata_->manifest.layout) ||
        stats_.metadata_heap_rows_read != expected_metadata_rows ||
        stats_.metadata_bytes_read != expected_metadata_bytes ||
        stats_.full_generation.scan_calls != 1 ||
        stats_.full_generation.planned_extents != manifest_extents ||
        stats_.full_generation.completed_extents != manifest_extents ||
        streamed_chunks != stats_.payload.delivered_chunks ||
        streamed_bytes != metadata_->manifest.payload_bytes ||
        stats_.full_generation.payload_row_checksum != metadata_->manifest.payload_checksum ||
        stats_.planned_request_groups != 1 || stats_.executed_request_groups != 1 ||
        stats_.bulk_read_calls != 1 || stats_.range_scan_calls != 1 ||
        stats_.payload.index_scans != 1 || stats_.payload.requested_ranges != 1 ||
        stats_.payload.heap_rows_read != streamed_chunks ||
        stats_.payload_heap_rows_read != streamed_chunks ||
        stats_.payload_bytes_read != streamed_bytes ||
        stats_.payload.wanted_subkeys != manifest_extents ||
        stats_.payload.read_subkeys != manifest_extents ||
        stats_.payload.wanted_extents != manifest_extents ||
        stats_.payload.read_extents != manifest_extents ||
        stats_.payload.wanted_chunks != streamed_chunks ||
        stats_.payload.delivered_chunks != streamed_chunks ||
        stats_.payload.delivered_bytes != streamed_bytes || stats_.payload.skipped_rows != 0 ||
        stats_.payload.skip_budget_rows != 0 || stats_.cold.route_empty != 0 ||
        stats_.cold.route_selective != 0 || stats_.cold.route_full != 1 ||
        stats_.cold.payload_rows_requested != streamed_chunks ||
        stats_.cold.payload_rows_delivered != streamed_chunks ||
        stats_.cold.payload_rows_scanned != streamed_chunks ||
        stats_.cold.payload_rows_skipped != 0 ||
        stats_.cold.payload_bytes_requested != streamed_bytes ||
        stats_.cold.payload_bytes_delivered != streamed_bytes ||
        stats_.cold.payload_bytes_scanned != streamed_bytes ||
        stats_.cold.row_crc_bytes != streamed_bytes ||
        stats_.cold.extent_crc_bytes != streamed_bytes ||
        stats_.cold.extent_crc_raw_bytes_hashed != 0 ||
        stats_.cold.extent_crc_combined_bytes != streamed_bytes ||
        stats_.cold.extent_crc_combine_rows != streamed_chunks ||
        (streamed_chunks == 0 ? cold_tid_transitions != 0
                              : cold_tid_transitions != streamed_chunks - 1) ||
        stats_.payload.repeated_chunk_reads != 0 || stats_.index_scans != expected_index_scans ||
        stats_.cache_lookups != 0 || stats_.cache_populations != 0 ||
        stats_.toast_values_read != 0 || stats_.detoast_bytes != 0 || !catalog_source_authorized_ ||
        stats_.source_parent_rows_read != 1 || stats_.source_catalog_rows_read != 1 ||
        stats_.source_catalog_index_scans != 1 || stats_.source_fragment_rows_read != 0 ||
        stats_.source_decoded_bp != 0 || stats_.source_storage_incarnations_checked < 1 ||
        stats_.source_storage_incarnations_checked > 2) {
        fail_("full-generation read authorization equations disagree");
    }
    stats_.total_heap_rows_read = 0;
    checked_add_(stats_.total_heap_rows_read,
                 stats_.metadata_heap_rows_read,
                 "full-generation total row equation overflow");
    checked_add_(stats_.total_heap_rows_read,
                 stats_.payload_heap_rows_read,
                 "full-generation total row equation overflow");
    stats_.total_bytes_read = 0;
    checked_add_(stats_.total_bytes_read,
                 stats_.metadata_bytes_read,
                 "full-generation total byte equation overflow");
    checked_add_(stats_.total_bytes_read,
                 stats_.payload_bytes_read,
                 "full-generation total byte equation overflow");
    stats_.cold.add(stats_.cold.toast_values, stats_.toast_values_read);
    stats_.cold.add(stats_.cold.detoast_bytes, stats_.detoast_bytes);
    finalize_payload_timing_();
    state_ = operation_state::finished;
    close_relations_checked_();
    return stats_;
}

} // namespace bioseq::storage::indexing
