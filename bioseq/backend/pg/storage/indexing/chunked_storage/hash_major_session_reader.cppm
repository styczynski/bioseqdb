/**
 * @file hash_major_session_reader.cppm
 * @brief Read hash-major index metadata and payloads using one PostgreSQL snapshot.
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

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <cstdint>
#include <new>
export module bioseq.postgres.storage.indexing.hash_major_session_reader;
import std;
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
import bioseq.indexing.storage;           // Storage adapter requirements.
import bioseq.indexing.minimizer_hashmap; // Minimizer index header and bucket format.
import bioseq.indexing.minimizer_fragment_image; // Minimizer fragment format with a fixed byte order.
import bioseq.indexing.compact_hash32_direct_index;
import bioseq.indexing.minimizer_fragment_compact_hash32_adapter;
import bioseq.postgres.table.io; // begin_column_read/read_next_column_row/end_column_read/column_read_state
import bioseq.storage.lazy_sequence;           // lazy_sequence
import bioseq.postgres.types.compact_sequence; // bioseq::pg_types::seq, dispatch_instance
import bioseq.sequences;                       // compact_sequence_range
import bioseq.sequences.simd;                  // Decode seqence bases with SIMD.
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
import bioseq.postgres.storage.indexing.chunked_storage;
export import bioseq.postgres.storage.indexing.hash_major_parallel_probe_contract;
export namespace bioseq::storage::indexing {
/// Read one published hash-major index through the caller's registered  snapshot.
/// Check metadata and source identity before reading directory, control or position bytes.
class index_storage_hash_major_minimizer_read_session {
  private:
    enum class operation_state : std::uint8_t {
        opened,
        head_absent,
        metadata_ready,
        source_authorized,
        finished,
        failed,
        moved,
    };

    std::string table_name_;
    std::string index_name_;
    Relation heap_relation_ = nullptr;
    Relation primary_index_relation_ = nullptr;
    SysScanDesc active_scan_ = nullptr;
    std::array<TupleTableSlot*, k_minimizer_generation_full_scan_pinned_batch_rows>
        row_batch_slots_{};
    std::size_t row_batch_slot_count_ = 0u;
    bool cached_boundary_row_valid_ = false;
    hash_major_minimizer_stream_kind cached_boundary_row_kind_ =
        hash_major_minimizer_stream_kind::directory;
    std::uint64_t cached_boundary_row_ordinal_ = 0u;
    std::size_t cached_boundary_slot_ordinal_ = k_minimizer_generation_full_scan_pinned_batch_rows;
    minimizer_generation_projected_row cached_boundary_row_{};
    Snapshot snapshot_ = nullptr;
    std::uint64_t query_key_ = 0u;
    Oid expected_tablespace_oid_ = InvalidOid;
    operation_state state_ = operation_state::opened;
    encoded_hash_major_minimizer_head encoded_head_{};
    encoded_hash_major_minimizer_manifest encoded_manifest_{};
    std::optional<hash_major_minimizer_head> head_{};
    std::optional<hash_major_minimizer_manifest> manifest_{};
    std::optional<expected_hash_major_minimizer_generation> expected_generation_{};
    std::array<std::byte, k_hash_major_minimizer_maximum_control_block_bytes>
        control_wave_scratch_{};
    minimizer_generation_read_stats stats_{};

    [[noreturn]] void fail_(std::string_view message) {
        state_ = operation_state::failed;
        minimizer_generation_storage_error_as_cpp(message);
    }

    void create_row_batch_slots_checked_() {
        bioseq::postgres::support::invoke_pg_as_cpp([this] {
            while (row_batch_slot_count_ < row_batch_slots_.size()) {
                TupleTableSlot* slot = table_slot_create(heap_relation_, nullptr);
                Assert(TTS_IS_BUFFERTUPLE(slot));
                row_batch_slots_[row_batch_slot_count_] = slot;
                ++row_batch_slot_count_;
            }
        });
    }

    void invalidate_cached_boundary_row_() noexcept {
        cached_boundary_row_valid_ = false;
        cached_boundary_row_kind_ = hash_major_minimizer_stream_kind::directory;
        cached_boundary_row_ordinal_ = 0u;
        cached_boundary_slot_ordinal_ = row_batch_slots_.size();
        cached_boundary_row_ = {};
    }

    void clear_row_batch_slots_checked_() {
        if (row_batch_slot_count_ == 0u)
            return;
        invalidate_cached_boundary_row_();
        bioseq::postgres::support::invoke_pg_as_cpp([this] {
            for (std::size_t ordinal = 0u; ordinal < row_batch_slot_count_; ++ordinal) {
                TupleTableSlot* slot = row_batch_slots_[ordinal];
                Assert(slot != nullptr);
                ExecClearTuple(slot);
            }
        });
    }

    /// Keep the final row available for the next request and release the other row buffers.
    /// Adjacent requests can split inside a stored row, so keeping its buffer
    /// pin avoids fetching and checking the same bytes again.
    void clear_row_batch_slots_except_checked_(std::size_t retained_ordinal) {
        if (!cached_boundary_row_valid_ || retained_ordinal >= row_batch_slot_count_) {
            fail_("hash-major boundary-row cache slot is invalid");
        }
        bioseq::postgres::support::invoke_pg_as_cpp([this, retained_ordinal] {
            for (std::size_t ordinal = 0u; ordinal < row_batch_slot_count_; ++ordinal) {
                if (ordinal == retained_ordinal)
                    continue;
                TupleTableSlot* slot = row_batch_slots_[ordinal];
                Assert(slot != nullptr);
                ExecClearTuple(slot);
            }
        });
    }

    void drop_row_batch_slots_checked_() {
        if (row_batch_slot_count_ == 0u)
            return;
        invalidate_cached_boundary_row_();
        bioseq::postgres::support::invoke_pg_as_cpp([this] {
            while (row_batch_slot_count_ != 0u) {
                const std::size_t ordinal = row_batch_slot_count_ - 1u;
                TupleTableSlot* slot = row_batch_slots_[ordinal];
                row_batch_slots_[ordinal] = nullptr;
                row_batch_slot_count_ = ordinal;
                if (slot != nullptr) {
                    ExecDropSingleTupleTableSlot(slot);
                }
            }
        });
    }

    void abandon_noexcept_() noexcept {
        invalidate_cached_boundary_row_();
        row_batch_slots_.fill(nullptr);
        row_batch_slot_count_ = 0u;
        active_scan_ = nullptr;
        primary_index_relation_ = nullptr;
        heap_relation_ = nullptr;
        snapshot_ = nullptr;
        expected_generation_.reset();
        manifest_.reset();
        head_.reset();
    }

    void close_relations_() {
        drop_row_batch_slots_checked_();
        if (active_scan_ != nullptr) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [this] { systable_endscan_ordered(active_scan_); });
            active_scan_ = nullptr;
        }
        if (primary_index_relation_ != nullptr) {
            Relation finished = std::exchange(primary_index_relation_, nullptr);
            bioseq::postgres::support::invoke_pg_as_cpp(
                [finished] { index_close(finished, AccessShareLock); });
            stats_.cold.add(stats_.cold.lock_releases, 1u);
        }
        if (heap_relation_ != nullptr) {
            Relation finished = std::exchange(heap_relation_, nullptr);
            bioseq::postgres::support::invoke_pg_as_cpp(
                [finished] { relation_close(finished, AccessShareLock); });
            stats_.cold.add(stats_.cold.lock_releases, 1u);
        }
        snapshot_ = nullptr;
    }

    [[nodiscard]] const bioseq::indexing::hash_major_minimizer_stream_descriptor&
    descriptor_(hash_major_minimizer_stream_kind kind) const {
        if (!manifest_ || !hash_major_minimizer_pg_detail::valid_stream_kind(kind)) {
            minimizer_generation_storage_error_as_cpp(
                "hash-major stream descriptor requested before metadata");
        }
        const std::size_t index = hash_major_minimizer_pg_detail::stream_index(kind);
        const auto& descriptor = manifest_->streams[index];
        if (descriptor.kind != kind) {
            minimizer_generation_storage_error_as_cpp(
                "hash-major stream descriptor ordering changed");
        }
        return descriptor;
    }

    void record_stream_row_(hash_major_minimizer_stream_kind kind, std::size_t bytes) {
        ++stats_.payload_heap_rows_read;
        stats_.payload_bytes_read += bytes;
        stats_.payload.heap_rows_read++;
        stats_.payload.delivered_chunks++;
        stats_.payload.delivered_bytes += bytes;
        stats_.cold.add(stats_.cold.payload_rows_scanned, 1u);
        stats_.cold.add(stats_.cold.payload_rows_delivered, 1u);
        stats_.cold.add(stats_.cold.payload_bytes_scanned, bytes);
        stats_.cold.add(stats_.cold.payload_bytes_delivered, bytes);
        stats_.cold.add(stats_.cold.row_crc_bytes, bytes);
        if (kind == hash_major_minimizer_stream_kind::directory) {
            ++stats_.directory_chunks_read;
            stats_.directory_bytes_read += bytes;
            ++stats_.hash_major.directory_row_crc_receipts;
        } else if (kind == hash_major_minimizer_stream_kind::controls) {
            ++stats_.controls.physical_heap_rows_read;
            stats_.controls.physical_bytes_read += bytes;
            ++stats_.controls.read.heap_rows_read;
            ++stats_.controls.read.delivered_chunks;
            stats_.controls.read.delivered_bytes += bytes;
            ++stats_.hash_major.control_row_crc_receipts;
        } else {
            ++stats_.positions.physical_heap_rows_read;
            stats_.positions.physical_bytes_read += bytes;
            ++stats_.positions.read.heap_rows_read;
            ++stats_.positions.read.delivered_chunks;
            stats_.positions.read.delivered_bytes += bytes;
            ++stats_.hash_major.posting_row_crc_receipts;
        }
    }

    template <class p_consumer>
    [[nodiscard]] bool scan_stream_rows_(hash_major_minimizer_stream_kind kind,
                                         std::uint64_t first_row,
                                         std::uint64_t last_row,
                                         p_consumer&& consume) {
        const auto& descriptor = descriptor_(kind);
        if (state_ != operation_state::source_authorized || active_scan_ != nullptr ||
            first_row > last_row || last_row >= descriptor.row_count ||
            last_row > static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)())) {
            return false;
        }
        std::uint64_t expected_row = first_row;
        bool accepted = true;

        // Reuse the previous request's final row when this request starts there.
        // Clear the saved row and other slots before scanning any further rows.
        if (cached_boundary_row_valid_) {
            if (cached_boundary_row_kind_ == kind && cached_boundary_row_ordinal_ == expected_row) {
                // The cached row already passed its address  and CRC checks.
                // Keeping its buffer pinned lets this request reuse the bytes directly.
                accepted = consume(expected_row, cached_boundary_row_.bytes);
                ++expected_row;
                if (!accepted || expected_row > last_row) {
                    return accepted && expected_row == last_row + 1u;
                }
            }
            clear_row_batch_slots_checked_();
        }

        ScanKeyData keys[6];
        ScanKeyInit(&keys[0],
                    k_minimizer_generation_index_generation_id,
                    BTEqualStrategyNumber,
                    F_INT8EQ,
                    Int64GetDatum(static_cast<std::int64_t>(manifest_->generation_id)));
        ScanKeyInit(&keys[1],
                    k_minimizer_generation_index_query_key,
                    BTEqualStrategyNumber,
                    F_INT8EQ,
                    Int64GetDatum(static_cast<std::int64_t>(query_key_)));
        ScanKeyInit(&keys[2],
                    k_minimizer_generation_index_sub_key,
                    BTEqualStrategyNumber,
                    F_INT8EQ,
                    Int64GetDatum(static_cast<std::int64_t>(
                        hash_major_minimizer_pg_detail::stream_sub_key(kind))));
        ScanKeyInit(&keys[3],
                    k_minimizer_generation_index_position_segment,
                    BTEqualStrategyNumber,
                    F_INT4EQ,
                    Int32GetDatum(0));
        ScanKeyInit(&keys[4],
                    k_minimizer_generation_index_chunk_number,
                    BTGreaterEqualStrategyNumber,
                    F_INT4GE,
                    Int32GetDatum(static_cast<std::int32_t>(expected_row)));
        ScanKeyInit(&keys[5],
                    k_minimizer_generation_index_chunk_number,
                    BTLessEqualStrategyNumber,
                    F_INT4LE,
                    Int32GetDatum(static_cast<std::int32_t>(last_row)));
        create_row_batch_slots_checked_();
        active_scan_ = bioseq::postgres::support::invoke_pg_as_cpp([this, &keys] {
            return systable_beginscan_ordered(
                heap_relation_, primary_index_relation_, snapshot_, 6, keys);
        });
        ++stats_.index_scans;
        ++stats_.range_scan_calls;
        stats_.cold.add(stats_.cold.payload_range_scans, 1u);
        pinned_minimizer_generation_projected_row_batch batch{};
        validated_minimizer_generation_row_receipt_batch validated_receipts{};
        std::size_t batch_ordinal = 0u;
        while (accepted && expected_row <= last_row) {
            if (batch_ordinal == batch.size) {
                if (batch.eof)
                    break;
                clear_validated_minimizer_generation_row_receipt_batch(validated_receipts);
                batch = next_pinned_minimizer_generation_projected_row_batch_or_error(
                    heap_relation_, active_scan_, row_batch_slots_);
                batch_ordinal = 0u;
                if (batch.size == 0u) {
                    if (batch.eof)
                        break;
                    fail_("hash-major batched stream scan made no progress");
                }
                authenticate_minimizer_generation_projected_row_batch(batch, validated_receipts);
            }
            const pinned_minimizer_generation_projected_row& pinned = batch.rows[batch_ordinal];
            minimizer_generation_loaded_row row =
                load_authenticated_minimizer_generation_projected_row_or_error(
                    pinned.projected, validated_receipts[batch_ordinal]);
            ++batch_ordinal;
            if (row.query_key != query_key_ ||
                row.address != index_chunk_address{
                                   .generation_id = manifest_->generation_id,
                                   .position_segment = 0,
                                   .sub_key = hash_major_minimizer_pg_detail::stream_sub_key(kind),
                                   .chunk_no = static_cast<std::uint32_t>(expected_row),
                               }) {
                row.release();
                fail_("hash-major stream row identity/order disagrees");
            }
            const std::uint64_t row_offset =
                expected_row * k_hash_major_minimizer_row_payload_bytes;
            const std::uint64_t remaining = descriptor.logical_bytes - row_offset;
            const std::size_t expected_bytes = static_cast<std::size_t>(
                std::min<std::uint64_t>(remaining, k_hash_major_minimizer_row_payload_bytes));
            if (row.bytes.size() != expected_bytes ||
                (expected_row + 1u < descriptor.row_count &&
                 row.bytes.size() != k_hash_major_minimizer_row_payload_bytes)) {
                row.release();
                fail_("hash-major stream physical row length disagrees");
            }
            record_stream_row_(kind, row.bytes.size());
            accepted = consume(expected_row, row.bytes);
            const bool retain_boundary_row = accepted && expected_row == last_row;
            row.release();
            if (retain_boundary_row) {
                cached_boundary_row_valid_ = true;
                cached_boundary_row_kind_ = kind;
                cached_boundary_row_ordinal_ = expected_row;
                cached_boundary_slot_ordinal_ = batch_ordinal - 1u;
                cached_boundary_row_ = pinned.projected;
            }
            ++expected_row;
        }
        clear_validated_minimizer_generation_row_receipt_batch(validated_receipts);
        if (cached_boundary_row_valid_) {
            clear_row_batch_slots_except_checked_(cached_boundary_slot_ordinal_);
        } else {
            clear_row_batch_slots_checked_();
        }
        bioseq::postgres::support::invoke_pg_as_cpp(
            [this] { systable_endscan_ordered(active_scan_); });
        active_scan_ = nullptr;
        return accepted && expected_row == last_row + 1u;
    }

    [[nodiscard]] bool read_logical_bytes_(hash_major_minimizer_stream_kind kind,
                                           std::uint64_t offset,
                                           std::span<std::byte> output) {
        if (state_ != operation_state::source_authorized ||
            !hash_major_minimizer_pg_detail::valid_stream_kind(kind)) {
            return false;
        }
        const auto& descriptor = descriptor_(kind);
        if (offset > descriptor.logical_bytes ||
            output.size() > descriptor.logical_bytes - offset) {
            return false;
        }
        if (output.empty())
            return true;
        const std::uint64_t end = offset + output.size();
        const std::uint64_t first_row = offset / k_hash_major_minimizer_row_payload_bytes;
        const std::uint64_t last_row = (end - 1u) / k_hash_major_minimizer_row_payload_bytes;
        std::size_t copied = 0u;
        const bool read = scan_stream_rows_(
            kind,
            first_row,
            last_row,
            [offset, end, output, &copied](std::uint64_t row_ordinal,
                                           std::span<const std::byte> row) {
                const std::uint64_t row_begin =
                    row_ordinal * k_hash_major_minimizer_row_payload_bytes;
                const std::uint64_t row_end = row_begin + row.size();
                const std::uint64_t copy_begin = std::max(offset, row_begin);
                const std::uint64_t copy_end = std::min(end, row_end);
                if (copy_begin >= copy_end)
                    return true;
                const std::size_t bytes = static_cast<std::size_t>(copy_end - copy_begin);
                std::copy_n(row.begin() + static_cast<std::ptrdiff_t>(copy_begin - row_begin),
                            bytes,
                            output.begin() + static_cast<std::ptrdiff_t>(copied));
                copied += bytes;
                return true;
            });
        return read && copied == output.size();
    }

    [[nodiscard]] bool
    read_control_wave_(std::span<const hash_major_minimizer_authorized_control_range> ranges,
                       void* consumer_context,
                       hash_major_minimizer_consume_control_block_fn consume) {
        if (state_ != operation_state::source_authorized || consume == nullptr || ranges.empty() ||
            ranges.size() > bioseq::indexing::k_hash_major_minimizer_control_read_wave_blocks ||
            ranges.size() > static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())) {
            return false;
        }
        const auto& descriptor = descriptor_(hash_major_minimizer_stream_kind::controls);
        std::uint64_t previous_end = 0u;
        std::uint64_t previous_block = 0u;
        bool have_previous = false;
        for (const auto& range : ranges) {
            const std::uint64_t end = range.control_byte_offset + range.byte_count;
            if (range.byte_count == 0u ||
                range.byte_count > k_hash_major_minimizer_maximum_control_block_bytes ||
                range.reserved_zero != 0u || end < range.control_byte_offset ||
                end > descriptor.logical_bytes ||
                (have_previous && (range.control_byte_offset < previous_end ||
                                   range.control_block_ordinal <= previous_block))) {
                return false;
            }
            previous_end = end;
            previous_block = range.control_block_ordinal;
            have_previous = true;
        }

        std::size_t group_begin = 0u;
        while (group_begin < ranges.size()) {
            const auto first_interval_end =
                ranges[group_begin].control_byte_offset + ranges[group_begin].byte_count;
            std::uint64_t group_last_row =
                (first_interval_end - 1u) / k_hash_major_minimizer_row_payload_bytes;
            std::size_t group_end = group_begin + 1u;
            while (group_end < ranges.size()) {
                const std::uint64_t next_first_row = ranges[group_end].control_byte_offset /
                                                     k_hash_major_minimizer_row_payload_bytes;
                if (next_first_row > group_last_row + 1u)
                    break;
                const std::uint64_t next_end =
                    ranges[group_end].control_byte_offset + ranges[group_end].byte_count;
                group_last_row = std::max(
                    group_last_row, (next_end - 1u) / k_hash_major_minimizer_row_payload_bytes);
                ++group_end;
            }
            const std::uint64_t group_first_row =
                ranges[group_begin].control_byte_offset / k_hash_major_minimizer_row_payload_bytes;
            std::size_t cursor = group_begin;
            const bool delivered = scan_stream_rows_(
                hash_major_minimizer_stream_kind::controls,
                group_first_row,
                group_last_row,
                [this, ranges, consumer_context, consume, group_end, &cursor](
                    std::uint64_t row_ordinal, std::span<const std::byte> row) {
                    const std::uint64_t row_begin =
                        row_ordinal * k_hash_major_minimizer_row_payload_bytes;
                    const std::uint64_t row_end = row_begin + row.size();
                    while (cursor < group_end) {
                        const auto& range = ranges[cursor];
                        const std::uint64_t range_begin = range.control_byte_offset;
                        const std::uint64_t range_end = range_begin + range.byte_count;
                        if (range_begin >= row_end)
                            break;
                        const std::uint64_t copy_begin = std::max(row_begin, range_begin);
                        const std::uint64_t copy_end = std::min(row_end, range_end);
                        if (copy_begin < copy_end) {
                            std::copy_n(row.begin() +
                                            static_cast<std::ptrdiff_t>(copy_begin - row_begin),
                                        static_cast<std::size_t>(copy_end - copy_begin),
                                        control_wave_scratch_.begin() +
                                            static_cast<std::ptrdiff_t>(copy_begin - range_begin));
                        }
                        if (row_end < range_end)
                            break;
                        if (!consume(consumer_context,
                                     static_cast<std::uint32_t>(cursor),
                                     std::span<const std::byte>{control_wave_scratch_}.first(
                                         range.byte_count))) {
                            return false;
                        }
                        ++cursor;
                    }
                    return true;
                });
            if (!delivered || cursor != group_end)
                return false;
            group_begin = group_end;
        }
        return true;
    }

    [[nodiscard]] bool
    read_posting_wave_(std::span<const hash_major_minimizer_authorized_posting_range> ranges,
                       void* consumer_context,
                       hash_major_minimizer_consume_posting_bytes_fn consume) {
        if (state_ != operation_state::source_authorized || consume == nullptr || ranges.empty() ||
            ranges.size() > static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())) {
            return false;
        }
        const auto& descriptor = descriptor_(hash_major_minimizer_stream_kind::postings);
        std::uint64_t previous_end = 0u;
        bool have_previous = false;
        for (const auto& range : ranges) {
            const std::uint64_t end = range.posting_byte_offset + range.byte_count;
            if (range.byte_count == 0u || range.reserved_zero != 0u || range.request_count == 0u ||
                end < range.posting_byte_offset || end > descriptor.logical_bytes ||
                (have_previous && range.posting_byte_offset < previous_end)) {
                return false;
            }
            previous_end = end;
            have_previous = true;
        }

        std::size_t group_begin = 0u;
        while (group_begin < ranges.size()) {
            const std::uint64_t first_end =
                ranges[group_begin].posting_byte_offset + ranges[group_begin].byte_count;
            std::uint64_t group_last_row =
                (first_end - 1u) / k_hash_major_minimizer_row_payload_bytes;
            std::size_t group_end = group_begin + 1u;
            while (group_end < ranges.size()) {
                const std::uint64_t next_first_row = ranges[group_end].posting_byte_offset /
                                                     k_hash_major_minimizer_row_payload_bytes;
                if (next_first_row > group_last_row + 1u)
                    break;
                const std::uint64_t next_end =
                    ranges[group_end].posting_byte_offset + ranges[group_end].byte_count;
                group_last_row = std::max(
                    group_last_row, (next_end - 1u) / k_hash_major_minimizer_row_payload_bytes);
                ++group_end;
            }
            const std::uint64_t group_first_row =
                ranges[group_begin].posting_byte_offset / k_hash_major_minimizer_row_payload_bytes;
            std::size_t cursor = group_begin;
            const bool delivered = scan_stream_rows_(
                hash_major_minimizer_stream_kind::postings,
                group_first_row,
                group_last_row,
                [this, ranges, consumer_context, consume, group_end, &cursor](
                    std::uint64_t row_ordinal, std::span<const std::byte> row) {
                    const std::uint64_t row_begin =
                        row_ordinal * k_hash_major_minimizer_row_payload_bytes;
                    const std::uint64_t row_end = row_begin + row.size();
                    while (cursor < group_end) {
                        const auto& range = ranges[cursor];
                        const std::uint64_t range_begin = range.posting_byte_offset;
                        const std::uint64_t range_end = range_begin + range.byte_count;
                        if (range_begin >= row_end)
                            break;
                        const std::uint64_t copy_begin = std::max(row_begin, range_begin);
                        const std::uint64_t copy_end = std::min(row_end, range_end);
                        if (copy_begin < copy_end &&
                            !consume(
                                consumer_context,
                                static_cast<std::uint32_t>(cursor),
                                copy_begin - range_begin,
                                row.subspan(static_cast<std::size_t>(copy_begin - row_begin),
                                            static_cast<std::size_t>(copy_end - copy_begin)))) {
                            return false;
                        }
                        if (row_end < range_end)
                            break;
                        if (stats_.hash_major.posting_ranges_delivered ==
                                (std::numeric_limits<std::uint64_t>::max)() ||
                            range.byte_count > (std::numeric_limits<std::uint64_t>::max)() -
                                                   stats_.hash_major.authorized_posting_bytes) {
                            return false;
                        }
                        ++stats_.hash_major.posting_ranges_delivered;
                        stats_.hash_major.authorized_posting_bytes += range.byte_count;
                        ++cursor;
                    }
                    return true;
                });
            if (!delivered || cursor != group_end)
                return false;
            group_begin = group_end;
        }
        return true;
    }

    [[nodiscard]] static bool read_bytes_callback_(void* context,
                                                   hash_major_minimizer_stream_kind kind,
                                                   std::uint64_t offset,
                                                   std::span<std::byte> output) {
        return static_cast<index_storage_hash_major_minimizer_read_session*>(context)
            ->read_logical_bytes_(kind, offset, output);
    }

    [[nodiscard]] static std::uint64_t
    logical_size_callback_(void* context, hash_major_minimizer_stream_kind kind) {
        auto& self = *static_cast<index_storage_hash_major_minimizer_read_session*>(context);
        if (self.state_ != operation_state::source_authorized ||
            !hash_major_minimizer_pg_detail::valid_stream_kind(kind)) {
            return 0u;
        }
        return self.descriptor_(kind).logical_bytes;
    }

    [[nodiscard]] static bool
    control_wave_callback_(void* context,
                           std::span<const hash_major_minimizer_authorized_control_range> ranges,
                           void* consumer_context,
                           hash_major_minimizer_consume_control_block_fn consume) {
        return static_cast<index_storage_hash_major_minimizer_read_session*>(context)
            ->read_control_wave_(ranges, consumer_context, consume);
    }

    [[nodiscard]] static bool
    posting_wave_callback_(void* context,
                           std::span<const hash_major_minimizer_authorized_posting_range> ranges,
                           void* consumer_context,
                           hash_major_minimizer_consume_posting_bytes_fn consume) {
        return static_cast<index_storage_hash_major_minimizer_read_session*>(context)
            ->read_posting_wave_(ranges, consumer_context, consume);
    }

  public:
    index_storage_hash_major_minimizer_read_session() = delete;
    /// Open the index table and check its schema and primary key
    /// The caller must keep the registered MVCC snapshot alive until this reader closes.
    index_storage_hash_major_minimizer_read_session(std::string_view table_name,
                                                    std::string_view classification_index_name,
                                                    std::uint64_t query_key,
                                                    Snapshot registered_snapshot,
                                                    Oid expected_tablespace_oid)
        : table_name_(table_name), index_name_(classification_index_name),
          snapshot_(registered_snapshot), query_key_(query_key),
          expected_tablespace_oid_(expected_tablespace_oid) {
        try {
            if (table_name_.empty() || index_name_.empty() || snapshot_ == nullptr ||
                !IsMVCCSnapshot(snapshot_) || snapshot_->regd_count == 0 ||
                !OidIsValid(expected_tablespace_oid_) ||
                !bioseq::indexing::sql_bigint_can_represent(query_key_)) {
                fail_("hash-major read binding is invalid");
            }
            const Oid relation_oid = bioseq::postgres::support::invoke_pg_as_cpp(
                [this] { return get_relname_relid(table_name_.c_str(), bioseq_internal_ns()); });
            if (!OidIsValid(relation_oid)) {
                throw minimizer_generation_relation_open_failure{
                    .inspection =
                        {
                            .kind = minimizer_generation_relation_kind::missing,
                            .schema_error = minimizer_generation_schema_error::none,
                        },
                };
            }
            heap_relation_ = bioseq::postgres::support::invoke_pg_as_cpp([relation_oid] {
                return bioseq::postgres::support::try_open_postgres_relation(relation_oid,
                                                                             AccessShareLock);
            });
            if (heap_relation_ == nullptr) {
                throw minimizer_generation_relation_open_failure{
                    .inspection =
                        {
                            .kind = minimizer_generation_relation_kind::missing,
                            .schema_error = minimizer_generation_schema_error::none,
                        },
                };
            }
            stats_.cold.add(stats_.cold.sidecar_heap_opens, 1u);
            stats_.cold.add(stats_.cold.lock_acquires, 1u);
            const bool exact_identity =
                RelationGetNamespace(heap_relation_) == bioseq_internal_ns() &&
                std::string_view{RelationGetRelationName(heap_relation_)} == table_name_;
            const Oid primary_oid = bioseq::postgres::support::invoke_pg_as_cpp(
                [this] {
                    return bioseq::postgres::support::relation_immediate_primary_key_index(heap_relation_);
                });
            if (OidIsValid(primary_oid)) {
                primary_index_relation_ = bioseq::postgres::support::invoke_pg_as_cpp(
                    [primary_oid] { return index_open(primary_oid, AccessShareLock); });
                stats_.cold.add(stats_.cold.sidecar_index_opens, 1u);
                stats_.cold.add(stats_.cold.lock_acquires, 1u);
            }
            const minimizer_generation_schema_inspection schema =
                exact_identity ? bioseq::postgres::support::invoke_pg_as_cpp([this] {
                    return inspect_bound_minimizer_generation_schema(
                        heap_relation_,
                        primary_index_relation_,
                        minimizer_generation_schema_expectation{
                            .namespace_oid = bioseq_internal_ns(),
                            .relation_name = table_name_,
                            .tablespace_oid = expected_tablespace_oid_,
                        });
                })
                               : minimizer_generation_schema_inspection{
                                     .error = minimizer_generation_schema_error::not_heap,
                                 };
            stats_.cold.add(stats_.cold.sidecar_schema_validations, 1u);
            if (!schema) {
                if (primary_index_relation_ != nullptr) {
                    Relation finished = std::exchange(primary_index_relation_, nullptr);
                    bioseq::postgres::support::invoke_pg_as_cpp(
                        [finished] { index_close(finished, AccessShareLock); });
                }
                Relation finished = std::exchange(heap_relation_, nullptr);
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [finished] { relation_close(finished, AccessShareLock); });
                snapshot_ = nullptr;
                throw minimizer_generation_relation_open_failure{
                    .inspection =
                        {
                            .kind = minimizer_generation_relation_kind::malformed,
                            .schema_error = schema.error,
                        },
                };
            }
            (void)verify_pg_inline_payload_layout(heap_relation_);
            stats_.relation_opens = 1u;
        } catch (const minimizer_generation_relation_open_failure&) {
            state_ = operation_state::failed;
            abandon_noexcept_();
            throw;
        } catch (...) {
            state_ = operation_state::failed;
            abandon_noexcept_();
            throw;
        }
    }
    index_storage_hash_major_minimizer_read_session(
        const index_storage_hash_major_minimizer_read_session&) = delete;
    index_storage_hash_major_minimizer_read_session&
    operator=(const index_storage_hash_major_minimizer_read_session&) = delete;
    index_storage_hash_major_minimizer_read_session(
        index_storage_hash_major_minimizer_read_session&& other) noexcept
        : table_name_(std::move(other.table_name_)), index_name_(std::move(other.index_name_)),
          heap_relation_(std::exchange(other.heap_relation_, nullptr)),
          primary_index_relation_(std::exchange(other.primary_index_relation_, nullptr)),
          active_scan_(std::exchange(other.active_scan_, nullptr)),
          row_batch_slots_(other.row_batch_slots_),
          row_batch_slot_count_(other.row_batch_slot_count_),
          cached_boundary_row_valid_(other.cached_boundary_row_valid_),
          cached_boundary_row_kind_(other.cached_boundary_row_kind_),
          cached_boundary_row_ordinal_(other.cached_boundary_row_ordinal_),
          cached_boundary_slot_ordinal_(other.cached_boundary_slot_ordinal_),
          cached_boundary_row_(other.cached_boundary_row_),
          snapshot_(std::exchange(other.snapshot_, nullptr)), query_key_(other.query_key_),
          expected_tablespace_oid_(other.expected_tablespace_oid_), state_(other.state_),
          encoded_head_(other.encoded_head_), encoded_manifest_(other.encoded_manifest_),
          head_(std::move(other.head_)), manifest_(std::move(other.manifest_)),
          expected_generation_(std::move(other.expected_generation_)),
          control_wave_scratch_(other.control_wave_scratch_), stats_(other.stats_) {
        other.invalidate_cached_boundary_row_();
        other.row_batch_slots_.fill(nullptr);
        other.row_batch_slot_count_ = 0u;
        other.state_ = operation_state::moved;
    }
    index_storage_hash_major_minimizer_read_session&
    operator=(index_storage_hash_major_minimizer_read_session&&) = delete;
    ~index_storage_hash_major_minimizer_read_session() {
        abandon_noexcept_();
    }

    /// Read and check the active head and its manifest once.
    /// Return false if the head is absent. A malformed head or manifest is an error.
    [[nodiscard]] bool read_active_head_manifest_once() {
        if (state_ != operation_state::opened || active_scan_ != nullptr) {
            fail_("hash-major metadata was read more than once");
        }
        ScanKeyData head_keys[5];
        ScanKeyInit(&head_keys[0],
                    k_minimizer_generation_index_generation_id,
                    BTEqualStrategyNumber,
                    F_INT8EQ,
                    Int64GetDatum(0));
        ScanKeyInit(&head_keys[1],
                    k_minimizer_generation_index_query_key,
                    BTEqualStrategyNumber,
                    F_INT8EQ,
                    Int64GetDatum(static_cast<std::int64_t>(query_key_)));
        ScanKeyInit(&head_keys[2],
                    k_minimizer_generation_index_sub_key,
                    BTEqualStrategyNumber,
                    F_INT8EQ,
                    Int64GetDatum(static_cast<std::int64_t>(k_hash_major_generation_head_sub_key)));
        ScanKeyInit(&head_keys[3],
                    k_minimizer_generation_index_position_segment,
                    BTEqualStrategyNumber,
                    F_INT4EQ,
                    Int32GetDatum(-1));
        ScanKeyInit(&head_keys[4],
                    k_minimizer_generation_index_chunk_number,
                    BTEqualStrategyNumber,
                    F_INT4EQ,
                    Int32GetDatum(0));
        active_scan_ = bioseq::postgres::support::invoke_pg_as_cpp([this, &head_keys] {
            return systable_beginscan_ordered(
                heap_relation_, primary_index_relation_, snapshot_, 5, head_keys);
        });
        ++stats_.index_scans;
        ++stats_.head_scans;
        HeapTuple tuple = bioseq::postgres::support::invoke_pg_as_cpp(
            [this] { return systable_getnext_ordered(active_scan_, ForwardScanDirection); });
        if (!HeapTupleIsValid(tuple)) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [this] { systable_endscan_ordered(active_scan_); });
            active_scan_ = nullptr;
            state_ = operation_state::head_absent;
            return false;
        }
        minimizer_generation_loaded_row head_row =
            load_minimizer_generation_row_or_error(heap_relation_, tuple);
        if (head_row.query_key != query_key_ ||
            head_row.address !=
                index_chunk_address{
                    .generation_id = 0u,
                    .position_segment = -1,
                    .sub_key = k_hash_major_generation_head_sub_key,
                    .chunk_no = 0u,
                } ||
            head_row.bytes.size() != k_hash_major_minimizer_head_bytes) {
            head_row.release();
            fail_("hash-major head row shape disagrees");
        }
        std::copy(head_row.bytes.begin(), head_row.bytes.end(), encoded_head_.begin());
        head_row.release();
        HeapTuple extra = bioseq::postgres::support::invoke_pg_as_cpp(
            [this] { return systable_getnext_ordered(active_scan_, ForwardScanDirection); });
        bioseq::postgres::support::invoke_pg_as_cpp(
            [this] { systable_endscan_ordered(active_scan_); });
        active_scan_ = nullptr;
        if (HeapTupleIsValid(extra)) {
            fail_("hash-major head primary key returned duplicates");
        }
        auto decoded_head = bioseq::indexing::decode_hash_major_minimizer_head(encoded_head_);
        if (!decoded_head || decoded_head->query_key != query_key_ ||
            decoded_head->generation_id == 0u ||
            !bioseq::indexing::sql_bigint_can_represent(decoded_head->generation_id)) {
            fail_("hash-major head fixed wire is invalid");
        }
        head_.emplace(*decoded_head);
        if (head_->query_key != query_key_ || head_->generation_id == 0u ||
            !bioseq::indexing::sql_bigint_can_represent(head_->generation_id)) {
            fail_("hash-major head fixed wire is invalid");
        }
        stats_.head_rows_read = 1u;
        stats_.head_bytes_read = k_hash_major_minimizer_head_bytes;
        stats_.cold.add(stats_.cold.row_crc_bytes, k_hash_major_minimizer_head_bytes);

        ScanKeyData manifest_keys[5];
        ScanKeyInit(&manifest_keys[0],
                    k_minimizer_generation_index_generation_id,
                    BTEqualStrategyNumber,
                    F_INT8EQ,
                    Int64GetDatum(static_cast<std::int64_t>(head_->generation_id)));
        ScanKeyInit(&manifest_keys[1],
                    k_minimizer_generation_index_query_key,
                    BTEqualStrategyNumber,
                    F_INT8EQ,
                    Int64GetDatum(static_cast<std::int64_t>(query_key_)));
        ScanKeyInit(&manifest_keys[2],
                    k_minimizer_generation_index_sub_key,
                    BTEqualStrategyNumber,
                    F_INT8EQ,
                    Int64GetDatum(0));
        ScanKeyInit(&manifest_keys[3],
                    k_minimizer_generation_index_position_segment,
                    BTEqualStrategyNumber,
                    F_INT4EQ,
                    Int32GetDatum(0));
        ScanKeyInit(&manifest_keys[4],
                    k_minimizer_generation_index_chunk_number,
                    BTEqualStrategyNumber,
                    F_INT4EQ,
                    Int32GetDatum(0));
        active_scan_ = bioseq::postgres::support::invoke_pg_as_cpp([this, &manifest_keys] {
            return systable_beginscan_ordered(
                heap_relation_, primary_index_relation_, snapshot_, 5, manifest_keys);
        });
        ++stats_.index_scans;
        ++stats_.manifest_directory_scans;
        tuple = bioseq::postgres::support::invoke_pg_as_cpp(
            [this] { return systable_getnext_ordered(active_scan_, ForwardScanDirection); });
        if (!HeapTupleIsValid(tuple)) {
            fail_("hash-major manifest row is missing");
        }
        minimizer_generation_loaded_row manifest_row =
            load_minimizer_generation_row_or_error(heap_relation_, tuple);
        if (manifest_row.query_key != query_key_ ||
            manifest_row.address !=
                index_chunk_address{
                    .generation_id = head_->generation_id,
                    .position_segment = 0,
                    .sub_key = 0,
                    .chunk_no = 0u,
                } ||
            manifest_row.bytes.size() != k_hash_major_minimizer_manifest_bytes) {
            manifest_row.release();
            fail_("hash-major manifest row shape disagrees");
        }
        std::copy(manifest_row.bytes.begin(), manifest_row.bytes.end(), encoded_manifest_.begin());
        manifest_row.release();
        extra = bioseq::postgres::support::invoke_pg_as_cpp(
            [this] { return systable_getnext_ordered(active_scan_, ForwardScanDirection); });
        bioseq::postgres::support::invoke_pg_as_cpp(
            [this] { systable_endscan_ordered(active_scan_); });
        active_scan_ = nullptr;
        if (HeapTupleIsValid(extra)) {
            fail_("hash-major manifest primary key returned duplicates");
        }
        auto decoded_manifest =
            bioseq::indexing::decode_hash_major_minimizer_manifest(encoded_manifest_);
        if (!decoded_manifest || decoded_manifest->generation_id != head_->generation_id ||
            decoded_manifest->source_epoch != head_->source_epoch ||
            decoded_manifest->source_identity != head_->source_identity ||
            decoded_manifest->source_content_crc64 != head_->source_content_crc64 ||
            decoded_manifest->storage_profile_digest != head_->storage_profile_digest) {
            fail_("hash-major head/manifest authentication disagrees");
        }
        manifest_.emplace(*decoded_manifest);
        if (manifest_->generation_id != head_->generation_id ||
            manifest_->source_epoch != head_->source_epoch ||
            manifest_->source_identity != head_->source_identity ||
            manifest_->source_content_crc64 != head_->source_content_crc64 ||
            manifest_->storage_profile_digest != head_->storage_profile_digest ||
            head_->manifest_payload_crc64 != bioseq::indexing::crc64_ecma182(encoded_manifest_) ||
            head_->manifest_address_bound_row_crc64 !=
                bioseq::indexing::crc64_index_chunk_row(
                    head_->generation_id,
                    0,
                    0,
                    0u,
                    static_cast<std::uint32_t>(encoded_manifest_.size()),
                    encoded_manifest_) ||
            !bioseq::indexing::is_valid_hash_major_minimizer_storage_profile(
                manifest_->storage_profile, manifest_->target_length) ||
            manifest_->streams[0].kind != hash_major_minimizer_stream_kind::directory ||
            manifest_->streams[1].kind != hash_major_minimizer_stream_kind::controls ||
            manifest_->streams[2].kind != hash_major_minimizer_stream_kind::postings) {
            fail_("hash-major head/manifest authentication disagrees");
        }
        std::uint64_t first_row = 0u;
        for (std::size_t index = 0u; index < 3u; ++index) {
            const auto& stream = manifest_->streams[index];
            if (stream.row_count !=
                    hash_major_minimizer_pg_detail::ceil_rows(stream.logical_bytes) ||
                stream.first_row_ordinal != first_row ||
                stream.row_count >
                    static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)()) ||
                stream.row_count > (std::numeric_limits<std::uint64_t>::max)() - first_row) {
                fail_("hash-major stream row descriptor disagrees");
            }
            first_row += stream.row_count;
        }
        stats_.manifest_rows_read = 1u;
        stats_.manifest_bytes_read = k_hash_major_minimizer_manifest_bytes;
        stats_.metadata_heap_rows_read = 2u;
        stats_.metadata_bytes_read =
            k_hash_major_minimizer_head_bytes + k_hash_major_minimizer_manifest_bytes;
        stats_.cold.add(stats_.cold.row_crc_bytes, k_hash_major_minimizer_manifest_bytes);
        state_ = operation_state::metadata_ready;
        return true;
    }

    [[nodiscard]] std::span<const std::byte> encoded_head() const noexcept {
        return state_ == operation_state::metadata_ready ||
                       state_ == operation_state::source_authorized
                   ? std::span<const std::byte>{encoded_head_}
                   : std::span<const std::byte>{};
    }

    [[nodiscard]] std::span<const std::byte> encoded_manifest() const noexcept {
        return state_ == operation_state::metadata_ready ||
                       state_ == operation_state::source_authorized
                   ? std::span<const std::byte>{encoded_manifest_}
                   : std::span<const std::byte>{};
    }

    [[nodiscard]] const hash_major_minimizer_head& head() const {
        if (!head_) {
            minimizer_generation_storage_error_as_cpp("hash-major head requested before metadata");
        }
        return *head_;
    }

    [[nodiscard]] const hash_major_minimizer_manifest& manifest() const {
        if (!manifest_) {
            minimizer_generation_storage_error_as_cpp(
                "hash-major manifest requested before metadata");
        }
        return *manifest_;
    }

    [[nodiscard]] const expected_hash_major_minimizer_generation& consume_source_authorization(
        minimizer_generation_catalog_source_authorization&& authorization) {
        try {
            if (state_ != operation_state::metadata_ready || !head_ || !manifest_ ||
                !authorization.matches_hash_major_reader(snapshot_,
                                                          MyDatabaseId,
                                                          expected_tablespace_oid_,
                                                          index_name_,
                                                          query_key_,
                                                          table_name_)) {
                authorization.invalidate_after_consumption();
                fail_("hash-major source authorization binding/work disagrees");
            }
            const auto& authorized_source = authorization.source_for_diagnostics();
            const std::uint64_t authorized_checksum =
                authorization.source_content_checksum_for_diagnostics();
            const auto source_identity = bioseq::indexing::hash_major_minimizer_source_identity(
                query_key_,
                authorized_source.locator.source_epoch,
                authorized_checksum,
                authorized_source.locator.logical_length);
            if (authorized_source.locator.source_epoch != manifest_->source_epoch ||
                authorized_checksum != manifest_->source_content_crc64 ||
                authorized_source.locator.logical_length != manifest_->target_length ||
                authorized_source.alphabet_code != manifest_->storage_profile.alphabet_code ||
                source_identity != manifest_->source_identity ||
                source_identity != head_->source_identity) {
                authorization.invalidate_after_consumption();
                fail_("hash-major source identity/content disagrees");
            }
            authorization.record_hash_major_reader_stats(stats_);
            expected_generation_.emplace(expected_hash_major_minimizer_generation{
                .generation_id = head_->generation_id,
                .query_key = query_key_,
                .source_epoch = manifest_->source_epoch,
                .source_content_crc64 = manifest_->source_content_crc64,
                .target_length = manifest_->target_length,
                .storage_profile = manifest_->storage_profile,
            });
            authorization.invalidate_after_consumption();
            state_ = operation_state::source_authorized;
            return *expected_generation_;
        } catch (...) {
            authorization.invalidate_after_consumption();
            state_ = operation_state::failed;
            throw;
        }
    }

    [[nodiscard]] hash_major_minimizer_logical_stream_source_ref source_ref() noexcept {
        if (state_ != operation_state::source_authorized)
            return {};
        return {
            .context = this,
            .read_bytes = &read_bytes_callback_,
            .logical_size = &logical_size_callback_,
            .read_control_wave = &control_wave_callback_,
            .read_posting_wave = &posting_wave_callback_,
        };
    }

    /// Measure a serial or parallel scan of  this generation for diagnostcs.
    [[nodiscard]] hash_major_minimizer_parallel_scan_stats
    run_parallel_scan_probe(std::uint32_t requested_workers);

    /// Check every stream's stored rows and checksum for an administrative audit.
    /// Read in primary-key order using bounded batches, then compare row counts,
    /// byte lengths and the CRC of each complete stream with the manifest.
    [[nodiscard]] std::array<hash_major_minimizer_catalog_stream_receipt, 3>
    audit_complete_streams() {
        if (state_ != operation_state::source_authorized || active_scan_ != nullptr || !manifest_) {
            fail_("hash-major complete stream audit is out of order");
        }
        std::array<hash_major_minimizer_catalog_stream_receipt, 3> receipts{};
        for (std::size_t index = 0u; index < receipts.size(); ++index) {
            const hash_major_minimizer_stream_kind kind =
                hash_major_minimizer_pg_detail::stream_kind(index);
            const auto& descriptor = descriptor_(kind);
            crc64_ecma182_accumulator crc{};
            std::uint64_t rows = 0u;
            std::uint64_t bytes = 0u;
            bool read = true;
            if (descriptor.row_count != 0u) {
                read = scan_stream_rows_(
                    kind,
                    0u,
                    descriptor.row_count - 1u,
                    [&rows, &bytes, &crc](std::uint64_t, std::span<const std::byte> row) {
                        ++rows;
                        bytes += row.size();
                        crc.update(row);
                        return true;
                    });
            }
            receipts[index] = {
                .kind = kind,
                .exact_catalog_rows = rows,
                .exact_catalog_bytes = bytes,
                .exact_catalog_stream_crc64 = crc.checksum(),
            };
            if (!read || rows != descriptor.row_count || bytes != descriptor.logical_bytes ||
                crc.checksum() != descriptor.stream_crc64) {
                fail_("hash-major complete stream audit disagrees");
            }
        }
        return receipts;
    }

    /// Close after reading metadata when this index's storage profile cannot, serve the query.
    /// The caller may then try the separately stored grouped-SoA index.
    void close_metadata_read() {
        if (state_ != operation_state::metadata_ready || active_scan_ != nullptr) {
            fail_("hash-major metadata close is out of order");
        }
        state_ = operation_state::finished;
        close_relations_();
    }

    /// Close after finding no active hash-major  head.
    void close_head_absent() {
        if (state_ != operation_state::head_absent || active_scan_ != nullptr) {
            fail_("hash-major absent-head close is out of order");
        }
        state_ = operation_state::finished;
        close_relations_();
    }

    /// Finish the read statistics and close the reader's PostgreSQL resources.
    [[nodiscard]] minimizer_generation_read_stats finish() {
        if (state_ != operation_state::source_authorized || active_scan_ != nullptr ||
            heap_relation_ == nullptr || primary_index_relation_ == nullptr) {
            fail_("hash-major read finished out of order");
        }
        stats_.total_heap_rows_read =
            stats_.metadata_heap_rows_read + stats_.payload_heap_rows_read;
        stats_.total_bytes_read = stats_.metadata_bytes_read + stats_.payload_bytes_read;
        stats_.cold.add(stats_.cold.payload_rows_requested, stats_.payload_heap_rows_read);
        stats_.cold.add(stats_.cold.payload_bytes_requested, stats_.payload_bytes_read);
        stats_.hash_major.layout_revision =
            bioseq::indexing::k_hash_major_minimizer_layout_revision;
        stats_.hash_major.sessions = 1u;
        stats_.hash_major.generation_id = head_->generation_id;
        stats_.hash_major.source_identity = head_->source_identity;
        stats_.hash_major.source_content_crc64 = head_->source_content_crc64;
        stats_.hash_major.storage_contract_digest = manifest_->storage_profile_digest;
        stats_.hash_major.directory_rows = stats_.directory_chunks_read;
        stats_.hash_major.directory_bytes = stats_.directory_bytes_read;
        stats_.hash_major.control_rows = stats_.controls.physical_heap_rows_read;
        stats_.hash_major.control_bytes = stats_.controls.physical_bytes_read;
        stats_.hash_major.posting_rows = stats_.positions.physical_heap_rows_read;
        stats_.hash_major.posting_bytes = stats_.positions.physical_bytes_read;
        state_ = operation_state::finished;
        close_relations_();
        return stats_;
    }
};

static_assert(!std::is_default_constructible_v<index_storage_hash_major_minimizer_read_session>);
static_assert(!std::is_copy_constructible_v<index_storage_hash_major_minimizer_read_session>);
static_assert(
    std::is_nothrow_move_constructible_v<index_storage_hash_major_minimizer_read_session>);

/// An opened reader, or details of a missing head or unusable index table.
struct hash_major_minimizer_read_session_open_result {
    std::optional<index_storage_hash_major_minimizer_read_session> session{};
    minimizer_generation_relation_inspection relation_failure{
        .kind = minimizer_generation_relation_kind::malformed,
        .schema_error = minimizer_generation_schema_error::none,
    };
    bool head_absent = false;

    [[nodiscard]] explicit operator bool() const noexcept {
        return session.has_value();
    }
};

/// Open the index reader and check its metadata.
/// Report a missing or malformed table, or an absent head, in the result.
/// Invalid head or manifest contents raise an error.
[[nodiscard]] inline hash_major_minimizer_read_session_open_result
open_exact_hash_major_minimizer_read_session_pg(
    std::string_view table_name,
    std::string_view index_name,
    std::uint64_t query_key,
    const borrowed_executor_read_context& executor_read) {
    if (!executor_read.valid()) {
        minimizer_generation_storage_error_as_cpp(
            "hash-major read requires a valid executor context");
    }
    hash_major_minimizer_read_session_open_result result{};
    try {
        result.session.emplace(table_name,
                               index_name,
                               query_key,
                               executor_read.snapshot(),
                               executor_read.tablespace_oid());
        if (!result.session->read_active_head_manifest_once()) {
            result.session->close_head_absent();
            result.session.reset();
            result.head_absent = true;
            result.relation_failure = {
                .kind = minimizer_generation_relation_kind::current_exact,
                .schema_error = minimizer_generation_schema_error::none,
            };
        } else {
            result.relation_failure = {
                .kind = minimizer_generation_relation_kind::current_exact,
                .schema_error = minimizer_generation_schema_error::none,
            };
        }
    } catch (const minimizer_generation_relation_open_failure& failure) {
        result.relation_failure = failure.inspection;
    }
    return result;
}

} // namespace bioseq::storage::indexing
