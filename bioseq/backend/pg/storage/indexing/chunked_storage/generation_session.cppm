/**
 * @file generation_session.cppm
 * @brief Snapshot-bound grouped-SoA generation read sessions and audits.
 *
 * Index chunks are raw BYTEA tuples in heap relations (heap_form_tuple /
 * CatalogTupleInsert + systable_beginscan), bypassing columnarisation
 * (Invariant C). The per-sequence layouts co-locate with the sequence's
 * `(bucket, shard)`; the table layout shards via `index_table_shard`.
 */
module;
#include <libassert/assert.hpp>
#include <bioseq/pg_compat.h>

extern "C" {
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/heapam.h"
#include "access/nbtree.h"
#include "access/parallel.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "common/relpath.h"
#include "commands/sequence.h"
#include "miscadmin.h"
#include "catalog/namespace.h"   // RelnameGetRelid
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "catalog/pg_opclass_d.h"
#include "catalog/pg_sequence.h"
#include "catalog/pg_type_d.h"
#include "commands/tablespace.h"
#include "executor/instrument.h"
#include "executor/spi.h"
#include "executor/tuptable.h"
#include "port/atomics.h"
#include "storage/buffile.h"
#include "storage/dsm.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/procarray.h"
#include "storage/relfilelocator.h"
#include "storage/shm_toc.h"
#include "utils/lsyscache.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/xid8.h"
#include "port.h"
}

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <expected>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>
export module bioseq.postgres.storage.indexing.chunked_storage:generation_session;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.common.memory_budget;
import bioseq.postgres.support;                       // bioseq::postgres::support::try_open_postgres_relation, bioseq::postgres::support::find_primary_key_oid
import bioseq.postgres.support.datums;
import bioseq.postgres.sql_boundary;
import bioseq.postgres.storage.manager;                 // seq_caches, sequence_locator, format helpers
import bioseq.storage.layout;             // sequence_locator
import bioseq.storage.size;            // sequence_size_t
import bioseq.indexing.type;           // sequence_index_layout, index_sub_key_t, k_max_index_chunk_size_bytes, sentinels
import bioseq.indexing.storage;        // the concepts
import bioseq.indexing.minimizer_hashmap;   // encoded header/bucket validation
import bioseq.indexing.minimizer_fragment_image;       // explicit fixed-endian minimizer fragment image image ABI
import bioseq.indexing.compact_hash32_direct_index;
import bioseq.indexing.minimizer_fragment_compact_hash32_adapter;
import bioseq.postgres.table.io;                // begin_column_read/read_next_column_row/end_column_read/column_read_state
import bioseq.postgres.storage.scanner;      // PostgreSQL-backed lazy sequence access
import bioseq.storage.lazy_sequence;            // lazy_sequence
import bioseq.postgres.types.compact_sequence;  // bioseq::pg_types::seq, dispatch_instance
import bioseq.sequences;               // compact_sequence_range
import bioseq.sequences.simd;          // canonical bounded base decoding
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
export namespace bioseq::storage::indexing {

/// Authenticated head, manifest, and directory returned by metadata discovery.
struct minimizer_generation_read_metadata {
    minimizer_generation_head head{};
    minimizer_generation_manifest manifest{};
    grouped_soa_directory_view directory{};
};

/// One caller-declared admission group. A group is one exact ordered PG range
/// scan; every request must match one complete directory control/position
/// extent, and every skipped row must be predeclared by that directory.
struct minimizer_generation_payload_read_group {
    index_sub_key_range range{};
    std::vector<index_sub_key_request> requests;
};

/// Physical and logical counters for one control or position read stage.
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

/// Authorization counters for the full-generation relation-scan route.  Unlike the
/// selected-range reader, this route consumes every payload row through one
/// ordered primary-index scan after metadata/source authorization has completed.
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

/// Compact hash-major-specific stats nested under the existing generation
/// read receipt. It carries only correctness/performance gates that cannot be
/// reconstructed from the control/position extent fields.
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

/// Aggregate receipt for one current-generation relation read session.
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

namespace minimizer_generation_full_crc_detail {

/// Verify that incremental CRC concatenation matches a single byte stream.
[[nodiscard]] inline bool concatenation_self_test() noexcept {
    return bioseq::indexing::crc64_ecma182_concatenate(
        bioseq::indexing::crc64_ecma182(std::string_view{"control"}),
        bioseq::indexing::crc64_ecma182(std::string_view{"position"}),
        std::string_view{"position"}.size())
        == bioseq::indexing::crc64_ecma182(
            std::string_view{"controlposition"});
}

} // namespace minimizer_generation_full_crc_detail

/// Select whether a read session owns a registered snapshot reference.
enum class minimizer_generation_snapshot_registration : std::uint8_t {
    register_copy,
    borrow_registered,
};

/// Current-generation target session. It binds one fresh heap OID, its exact
/// five-key primary index, one executor-owned data snapshot, and one query key
/// for the complete head -> metadata -> controls -> positions -> finish
/// lifecycle. Both payload sections retain the same open relations and snapshot.
class index_storage_fragment_generation_read_session {
private:
    enum class operation_state : std::uint8_t {
        opened,
        head_read,
        metadata_pending_source,
        metadata_ready,
        controls_planned,
        controls_complete,
        positions_planned,
        payload_complete,
        full_generation_payload_complete,
        finished,
        failed,
        moved,
    };

    enum class payload_section : std::uint8_t {
        controls,
        positions,
    };

    static constexpr std::uint64_t k_ordered_range_scan_penalty_rows = 8u;

    using loaded_row_type = minimizer_generation_loaded_row;

    struct planned_group {
        index_sub_key_range range{};
        std::vector<index_sub_key_request> requests;
        std::size_t first_directory_ordinal = 0;
        std::size_t end_directory_ordinal = 0;
        std::uint64_t skip_budget_rows = 0;
        bool position_extent = false;
        bool executed = false;
    };

    struct runtime_request {
        index_sub_key_request request{};
        std::uint64_t end_chunk_no = 0;
        std::uint64_t next_chunk_no = 0;
        std::uint64_t total_bytes = 0;
        crc64_ecma182_accumulator checksum{};
    };

    std::string table_name_;
    Relation heap_relation_ = nullptr;
    Relation primary_index_relation_ = nullptr;
    Relation storage_descriptor_relation_ = nullptr;
    Relation storage_descriptor_live_index_relation_ = nullptr;
    SysScanDesc active_scan_ = nullptr;
    std::array<TupleTableSlot*, k_minimizer_generation_full_scan_pinned_batch_rows>
        full_generation_batch_slots_{};
    std::size_t full_generation_batch_slot_count_ = 0;
    Snapshot snapshot_ = nullptr;
    bool snapshot_registered_by_session_ = false;
    std::uint64_t query_key_ = 0;
    Oid expected_tablespace_oid_ = InvalidOid;
    expected_minimizer_generation_identity expected_{};
    grouped_soa_minimizer_profile expected_profile_{};
    operation_state state_ = operation_state::opened;
    bool catalog_source_authorized_ = false;
    // Permits precede every allocation they cover, so reverse member
    // destruction cannot return admitted bytes while an owned view/buffer is
    // still alive.
    bioseq::parallelism::memory_permit metadata_permit_;
    bioseq::parallelism::memory_permit plan_permit_;
    bioseq::parallelism::memory_permit runtime_permit_;
    bioseq::parallelism::memory_permit control_authorization_permit_;
    std::optional<minimizer_generation_head> head_;
    std::optional<minimizer_generation_read_metadata> metadata_;
    std::optional<minimizer_generation_manifest> manifest_only_;
    std::vector<std::byte> directory_bytes_scratch_;
    std::vector<planned_group> planned_groups_;
    std::vector<runtime_request> runtime_requests_;
    std::vector<std::uint32_t> authorized_position_ordinals_;
    std::size_t next_group_ = 0;
    std::optional<std::uint64_t> previous_payload_heap_block_;
    std::optional<std::uint64_t> payload_stage_begin_ns_;
    minimizer_generation_read_stats stats_{};

    /// Return the sole authenticated manifest authority. Full metadata and
    /// manifest-only state are mutually exclusive; observing both or neither
    /// fails closed at every transition that consumes a manifest.
    [[nodiscard]] const minimizer_generation_manifest*
    active_manifest_() const noexcept {
        if (metadata_.has_value() == manifest_only_.has_value()) {
            return nullptr;
        }
        return metadata_
            ? &metadata_->manifest : &*manifest_only_;
    }

    [[noreturn]] void fail_(std::string_view message) {
        state_ = operation_state::failed;
        minimizer_generation_storage_error_as_cpp(message);
    }

    void checked_add_(
            std::uint64_t& value,
            std::uint64_t increment,
            std::string_view message) {
        if (increment
            > (std::numeric_limits<std::uint64_t>::max)() - value) {
            fail_(message);
        }
        value += increment;
    }

    /// Classify one physical payload TID transition. Same-page and immediately
    /// next-page tuples are adjacent; larger forward jumps and all backward
    /// jumps remain separately visible.
    void record_payload_tid_(ItemPointerData tid) noexcept {
        const std::uint64_t block = static_cast<std::uint64_t>(
            ItemPointerGetBlockNumber(&tid));
        if (previous_payload_heap_block_) {
            const std::uint64_t previous = *previous_payload_heap_block_;
            if (block == previous || block == previous + 1u) {
                stats_.cold.add(
                    stats_.cold.heap_tid_adjacent, 1);
            } else if (block > previous) {
                stats_.cold.add(
                    stats_.cold.heap_tid_forward_gap, 1);
            } else {
                stats_.cold.add(
                    stats_.cold.heap_tid_backward_jump, 1);
            }
        }
        previous_payload_heap_block_ = block;
    }

    void record_payload_tid_(const HeapTuple tuple) noexcept {
        record_payload_tid_(tuple->t_self);
    }

    void start_payload_timing_() noexcept {
        if (!payload_stage_begin_ns_) {
            payload_stage_begin_ns_ = cold_index_now_ns();
        }
    }

    /// Record one truthful coarse wall interval for the complete payload
    /// scan-and-callback stage. Decode and transfer remain explicitly
    /// unavailable (zero) until their owning sections expose coarse boundaries;
    /// no tuple/chunk-local clock reads are permitted on the cold path.
    void finalize_payload_timing_() noexcept {
        if (!payload_stage_begin_ns_) return;
        const std::uint64_t end_ns = cold_index_now_ns();
        const std::uint64_t raw_total_ns =
            end_ns >= *payload_stage_begin_ns_
                ? end_ns - *payload_stage_begin_ns_ : 0;
        const std::uint64_t total_ns = std::min(
            raw_total_ns, cold_index_preparation_stats::k_sql_bigint_max);
        if (raw_total_ns != total_ns) {
            stats_.cold.are_stats_saturated = true;
        }
        stats_.cold.add(stats_.cold.payload_ns, total_ns);
        payload_stage_begin_ns_.reset();
    }

    void release_owned_state_() noexcept {
        // Free/reset every covered allocation before returning admission.
        std::vector<std::byte>{}.swap(directory_bytes_scratch_);
        std::vector<planned_group>{}.swap(planned_groups_);
        std::vector<runtime_request>{}.swap(runtime_requests_);
        std::vector<std::uint32_t>{}.swap(authorized_position_ordinals_);
        metadata_.reset();
        manifest_only_.reset();
        head_.reset();
        runtime_permit_.release();
        plan_permit_.release();
        control_authorization_permit_.release();
        metadata_permit_.release();
    }

    void release_section_plan_() noexcept {
        std::vector<planned_group>{}.swap(planned_groups_);
        next_group_ = 0;
        plan_permit_.release();
    }

    void release_control_authorization_() noexcept {
        std::vector<std::uint32_t>{}.swap(authorized_position_ordinals_);
        control_authorization_permit_.release();
    }

    void checked_add_read_stats_(
            index_chunk_read_stats& destination,
            const index_chunk_read_stats& increment) {
        checked_add_(destination.relation_opens, increment.relation_opens,
            "payload relation-open counter overflow");
        checked_add_(destination.index_scans, increment.index_scans,
            "payload index-scan counter overflow");
        checked_add_(destination.requested_ranges, increment.requested_ranges,
            "payload requested-range counter overflow");
        checked_add_(destination.heap_rows_read, increment.heap_rows_read,
            "payload heap-row counter overflow");
        checked_add_(destination.toast_values_read, increment.toast_values_read,
            "payload toast-value counter overflow");
        checked_add_(destination.detoast_bytes, increment.detoast_bytes,
            "payload detoast-byte counter overflow");
        checked_add_(destination.explicit_copy_bytes,
            increment.explicit_copy_bytes,
            "payload explicit-copy counter overflow");
        checked_add_(destination.wanted_subkeys, increment.wanted_subkeys,
            "payload wanted-sub-key counter overflow");
        checked_add_(destination.read_subkeys, increment.read_subkeys,
            "payload read-sub-key counter overflow");
        checked_add_(destination.wanted_extents, increment.wanted_extents,
            "payload wanted-extent counter overflow");
        checked_add_(destination.read_extents, increment.read_extents,
            "payload read-extent counter overflow");
        checked_add_(destination.wanted_chunks, increment.wanted_chunks,
            "payload wanted-chunk counter overflow");
        checked_add_(destination.skipped_rows, increment.skipped_rows,
            "payload skipped-row counter overflow");
        checked_add_(destination.skip_budget_rows,
            increment.skip_budget_rows,
            "payload skip-budget counter overflow");
        checked_add_(destination.repeated_chunk_reads,
            increment.repeated_chunk_reads,
            "payload repeated-read counter overflow");
        checked_add_(destination.delivered_chunks,
            increment.delivered_chunks,
            "payload delivered-chunk counter overflow");
        checked_add_(destination.delivered_bytes, increment.delivered_bytes,
            "payload delivered-byte counter overflow");
    }

    [[nodiscard]] static constexpr bool counter_is_sum_(
            std::uint64_t total,
            std::uint64_t left,
            std::uint64_t right) noexcept {
        return total >= left && total - left == right;
    }

    [[nodiscard]] static constexpr bool read_stats_are_sum_(
            const index_chunk_read_stats& total,
            const index_chunk_read_stats& left,
            const index_chunk_read_stats& right) noexcept {
        return counter_is_sum_(total.relation_opens,
                   left.relation_opens, right.relation_opens)
            && counter_is_sum_(total.index_scans,
                   left.index_scans, right.index_scans)
            && counter_is_sum_(total.requested_ranges,
                   left.requested_ranges, right.requested_ranges)
            && counter_is_sum_(total.heap_rows_read,
                   left.heap_rows_read, right.heap_rows_read)
            && counter_is_sum_(total.toast_values_read,
                   left.toast_values_read, right.toast_values_read)
            && counter_is_sum_(total.detoast_bytes,
                   left.detoast_bytes, right.detoast_bytes)
            && counter_is_sum_(total.explicit_copy_bytes,
                   left.explicit_copy_bytes, right.explicit_copy_bytes)
            && counter_is_sum_(total.wanted_subkeys,
                   left.wanted_subkeys, right.wanted_subkeys)
            && counter_is_sum_(total.read_subkeys,
                   left.read_subkeys, right.read_subkeys)
            && counter_is_sum_(total.wanted_extents,
                   left.wanted_extents, right.wanted_extents)
            && counter_is_sum_(total.read_extents,
                   left.read_extents, right.read_extents)
            && counter_is_sum_(total.wanted_chunks,
                   left.wanted_chunks, right.wanted_chunks)
            && counter_is_sum_(total.skipped_rows,
                   left.skipped_rows, right.skipped_rows)
            && counter_is_sum_(total.skip_budget_rows,
                   left.skip_budget_rows, right.skip_budget_rows)
            && counter_is_sum_(total.repeated_chunk_reads,
                   left.repeated_chunk_reads, right.repeated_chunk_reads)
            && counter_is_sum_(total.delivered_chunks,
                   left.delivered_chunks, right.delivered_chunks)
            && counter_is_sum_(total.delivered_bytes,
                   left.delivered_bytes, right.delivered_bytes);
    }

    [[nodiscard]] static constexpr bool read_stats_are_zero_(
            const index_chunk_read_stats& stats) noexcept {
        return read_stats_are_sum_(
            stats, index_chunk_read_stats{}, index_chunk_read_stats{});
    }

    [[nodiscard]] static constexpr bool section_stats_are_zero_(
            const minimizer_generation_payload_stage_read_stats& section) noexcept {
        return section.plan_calls == 0u
            && section.planned_request_groups == 0u
            && section.executed_request_groups == 0u
            && section.range_scan_calls == 0u
            && section.physical_heap_rows_read == 0u
            && section.physical_bytes_read == 0u
            && section.control_authorization_checks == 0u
            && read_stats_are_zero_(section.read);
    }

    [[nodiscard]] static constexpr bool full_payload_stats_are_zero_(
            const minimizer_generation_full_payload_read_stats& full) noexcept {
        return full.scan_calls == 0u
            && full.planned_extents == 0u
            && full.completed_extents == 0u
            && full.control_chunks == 0u
            && full.position_chunks == 0u
            && full.control_bytes == 0u
            && full.position_bytes == 0u
            && full.payload_row_checksum == 0u;
    }

    [[nodiscard]] static constexpr bool section_equations_hold_(
            const minimizer_generation_payload_stage_read_stats& section) noexcept {
        return section.plan_calls == 1
            && section.planned_request_groups
                == section.executed_request_groups
            && section.executed_request_groups == section.range_scan_calls
            && section.read.relation_opens == 0
            && section.read.index_scans == section.range_scan_calls
            && section.read.requested_ranges == section.range_scan_calls
            && section.read.heap_rows_read == section.physical_heap_rows_read
            && section.read.heap_rows_read
                == section.read.delivered_chunks + section.read.skipped_rows
            && section.read.toast_values_read == 0
            && section.read.detoast_bytes == 0
            && section.read.explicit_copy_bytes == 0
            && section.read.wanted_subkeys == section.read.read_subkeys
            && section.read.wanted_extents == section.read.read_extents
            && section.read.wanted_chunks == section.read.delivered_chunks
            && section.read.skipped_rows == section.read.skip_budget_rows
            && section.read.repeated_chunk_reads == 0
            && section.physical_bytes_read >= section.read.delivered_bytes;
    }

    /// Allocate every auxiliary BufferHeapTuple slot before the batch reader
    /// starts. The count advances after each successful allocation so an ERROR
    /// part-way through setup leaves an exact cleanup prefix.
    void create_full_generation_batch_slots_checked_() {
        bioseq::postgres::support::invoke_pg_as_cpp(
            [this] {
                while (full_generation_batch_slot_count_
                       < full_generation_batch_slots_.size()) {
                    TupleTableSlot* slot =
                        table_slot_create(heap_relation_, nullptr);
                    Assert(TTS_IS_BUFFERTUPLE(slot));
                    full_generation_batch_slots_[
                        full_generation_batch_slot_count_] = slot;
                    ++full_generation_batch_slot_count_;
                }
            });
    }

    /// Drop pins and executor slots explicitly while PostgreSQL cleanup is
    /// still legal. Retire each pointer before its PG call so ResourceOwner is
    /// the single fallback if that call raises ERROR, with no later double-drop.
    void drop_full_generation_batch_slots_checked_() {
        if (full_generation_batch_slot_count_ == 0) return;
        bioseq::postgres::support::invoke_pg_as_cpp(
            [this] {
                while (full_generation_batch_slot_count_ != 0) {
                    const std::size_t ordinal =
                        full_generation_batch_slot_count_ - 1u;
                    TupleTableSlot* slot =
                        full_generation_batch_slots_[ordinal];
                    full_generation_batch_slots_[ordinal] = nullptr;
                    full_generation_batch_slot_count_ = ordinal;
                    if (slot != nullptr) {
                        ExecDropSingleTupleTableSlot(slot);
                    }
                }
            });
    }

    void abandon_relations_noexcept_() noexcept {
        // PostgreSQL cleanup is illegal from the noexcept destructor path.
        // On an exceptional PG cleanup failure, ResourceOwner owns any pins;
        // forget the C++ aliases so they cannot be used or dropped twice.
        full_generation_batch_slots_.fill(nullptr);
        full_generation_batch_slot_count_ = 0;
        active_scan_ = nullptr;
        storage_descriptor_live_index_relation_ = nullptr;
        storage_descriptor_relation_ = nullptr;
        primary_index_relation_ = nullptr;
        heap_relation_ = nullptr;
        snapshot_ = nullptr;
        snapshot_registered_by_session_ = false;
        release_owned_state_();
    }

    void close_relations_checked_() {
        const std::uint64_t close_begin_ns = cold_index_now_ns();
        const BufferUsage close_buffer_begin = pgBufferUsage;
        try {
            drop_full_generation_batch_slots_checked_();
            if (active_scan_ != nullptr) {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [this] { systable_endscan_ordered(active_scan_); });
                active_scan_ = nullptr;
            }
            if (storage_descriptor_live_index_relation_ != nullptr) {
                bioseq::postgres::support::invoke_pg_as_cpp([this] {
                    index_close(
                        storage_descriptor_live_index_relation_,
                        AccessShareLock);
                });
                storage_descriptor_live_index_relation_ = nullptr;
                stats_.cold.add(stats_.cold.lock_releases, 1);
            }
            if (storage_descriptor_relation_ != nullptr) {
                bioseq::postgres::support::invoke_pg_as_cpp([this] {
                    relation_close(
                        storage_descriptor_relation_, AccessShareLock);
                });
                storage_descriptor_relation_ = nullptr;
                stats_.cold.add(stats_.cold.lock_releases, 1);
            }
            if (primary_index_relation_ != nullptr) {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [this] {
                        index_close(primary_index_relation_, AccessShareLock);
                    });
                primary_index_relation_ = nullptr;
                stats_.cold.add(stats_.cold.lock_releases, 1);
            }
            if (heap_relation_ != nullptr) {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [this] { relation_close(heap_relation_, AccessShareLock); });
                heap_relation_ = nullptr;
                stats_.cold.add(stats_.cold.lock_releases, 1);
            }
            if (snapshot_registered_by_session_ && snapshot_ != nullptr) {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [this] { UnregisterSnapshot(snapshot_); });
            }
            snapshot_ = nullptr;
            snapshot_registered_by_session_ = false;
            release_owned_state_();
            add_cold_index_elapsed(
                stats_.cold, stats_.cold.close_ns, close_begin_ns);
            add_cold_index_buffer_usage(stats_.cold, close_buffer_begin);
        } catch (const bioseq::postgres::support::postgres_error&) {
            state_ = operation_state::failed;
            abandon_relations_noexcept_();
            throw;
        }
    }

    /// Preserve the original PG/C++ failure while deterministically ending an
    /// active scan and releasing both relation locks plus snapshot ownership.
    /// Cleanup runs after invoke_pg_as_cpp restored PostgreSQL's exception
    /// stack, never from a destructor during C++ unwinding.
    [[noreturn]] void cleanup_failed_session_and_rethrow_() {
        std::exception_ptr original_error = std::current_exception();
        state_ = operation_state::failed;
        try {
            close_relations_checked_();
        } catch (...) {
            // A cleanup ERROR must not replace the initiating corruption or
            // sink exception. The failed transaction's ResourceOwner remains
            // the final backstop for a PG object that could not be closed.
            abandon_relations_noexcept_();
        }
        std::rethrow_exception(original_error);
    }

    struct projected_extent_range {
        index_sub_key_t logical_begin = 0;
        index_sub_key_t logical_end = 0;
        bool position_extent = false;
    };

    /// Project one physical range onto the logical/control directory key
    /// namespace. A range may cover controls or positions, never cross the
    /// namespace boundary.
    [[nodiscard]] static std::optional<projected_extent_range>
    project_extent_range_(const index_sub_key_range& range) noexcept {
        if (range.begin < 0 || range.begin >= range.end
            || range.end > k_temporary_run_sub_key_base) {
            return std::nullopt;
        }
        if (range.end <= k_position_extent_sub_key_base) {
            return projected_extent_range{
                .logical_begin = range.begin,
                .logical_end = range.end,
                .position_extent = false,
            };
        }
        if (range.begin >= k_position_extent_sub_key_base) {
            return projected_extent_range{
                .logical_begin = range.begin - k_position_extent_sub_key_base,
                .logical_end = range.end - k_position_extent_sub_key_base,
                .position_extent = true,
            };
        }
        return std::nullopt;
    }

    [[nodiscard]] static bool request_matches_extent_(
            const index_sub_key_request& request,
            const grouped_soa_bucket_directory_entry& entry,
            bool position_extent) noexcept {
        const auto position_sub_key =
            derive_minimizer_position_extent_sub_key(entry.sub_key);
        if (!position_sub_key) {
            return false;
        }
        const bool control = !position_extent
            && request.sub_key == entry.sub_key
            && request.first_chunk_no
                == entry.control_first_chunk
            && request.expected_chunk_count == entry.control_chunk_count
            && request.expected_total_bytes == entry.control_encoded_bytes
            && request.expected_checksum == entry.control_checksum;
        const bool positions = position_extent
            && request.sub_key == *position_sub_key
            && request.first_chunk_no
                == entry.position_first_chunk
            && request.expected_chunk_count == entry.position_chunk_count
            && request.expected_total_bytes == entry.position_encoded_bytes
            && request.expected_checksum == entry.position_checksum;
        return control || positions;
    }

public:
    index_storage_fragment_generation_read_session() = delete;
    index_storage_fragment_generation_read_session(
            std::string_view table_name,
            std::uint64_t query_key,
            Snapshot executor_snapshot,
            expected_minimizer_generation_identity expected,
            Oid expected_tablespace_oid,
            minimizer_generation_snapshot_registration snapshot_registration =
                minimizer_generation_snapshot_registration::register_copy,
            std::string_view classification_index_name = {})
        : table_name_(table_name),
          snapshot_(executor_snapshot),
          query_key_(query_key),
          expected_tablespace_oid_(expected_tablespace_oid),
          expected_(expected) {
        const std::uint64_t bind_begin_ns = cold_index_now_ns();
        const BufferUsage bind_buffer_begin = pgBufferUsage;
        try {
            if (snapshot_ == nullptr
                || !IsMVCCSnapshot(snapshot_)
                || snapshot_->regd_count == 0) {
                minimizer_generation_storage_error(
                    "generation read requires a registered executor MVCC snapshot");
            }
            if (!classification_index_name.empty()
                && snapshot_registration
                    != minimizer_generation_snapshot_registration::borrow_registered) {
                minimizer_generation_storage_error(
                    "bound generation classification must borrow the executor snapshot");
            }
            if (!bioseq::indexing::sql_bigint_can_represent(query_key_)) {
                minimizer_generation_storage_error(
                    "generation read query key exceeds signed SQL range");
            }
            if (!OidIsValid(expected_tablespace_oid_)) {
                minimizer_generation_storage_error(
                    "generation read expected tablespace is invalid");
            }
            const bioseq::indexing::minimizer_scheme_identity expected_scheme{
                .id = expected_.minimizer_scheme_id,
                .revision = expected_.minimizer_scheme_revision,
            };
            const auto expected_profile = expected_.max_occ_cap
                ? bioseq::indexing::derive_grouped_soa_minimizer_profile(
                    expected_scheme,
                    expected_.alphabet_code,
                    expected_.kmer_size,
                    /*position_segment_span_bp=*/1u,
                    *expected_.max_occ_cap)
                : std::expected<grouped_soa_minimizer_profile,
                    minimizer_generation_encoding_error>{
                        std::unexpected(
                            minimizer_generation_encoding_error::
                                storage_contract_mismatch)};
            const bool supported_storage_width =
                expected_.storage_unit_byte_width == 1u
                || expected_.storage_unit_byte_width == 2u
                || expected_.storage_unit_byte_width == 4u
                || expected_.storage_unit_byte_width == 8u;
            if (!bioseq::indexing::
                    is_supported_minimizer_generation_layout(
                        expected_.layout)
                || !expected_profile
                || expected_profile->layout != expected_.layout
                || expected_profile->minimizer_scheme != expected_scheme
                || expected_profile->kmer_size != expected_.kmer_size
                || !supported_storage_width
                || expected_.window_size == 0
                || expected_.gpu_abi_revision
                    != expected_profile->gpu_abi_revision) {
                minimizer_generation_storage_error(
                    "generation read expected minimizer storage contract is invalid");
            }
            expected_profile_ = *expected_profile;
            if (snapshot_registration
                    == minimizer_generation_snapshot_registration::register_copy) {
                snapshot_ = bioseq::postgres::support::invoke_pg_as_cpp(
                    [this] { return RegisterSnapshot(snapshot_); });
                snapshot_registered_by_session_ = true;
                stats_.cold.snapshot_registers = 1;
            }
            const Oid relation_oid = bioseq::postgres::support::invoke_pg_as_cpp([this] {
                return get_relname_relid(
                    table_name_.c_str(), bioseq_internal_ns());
            });
            if (!OidIsValid(relation_oid)
                && !classification_index_name.empty()) {
                throw minimizer_generation_relation_open_failure{
                    .inspection = {
                        .kind = minimizer_generation_relation_kind::missing,
                        .schema_error = minimizer_generation_schema_error::none,
                    },
                };
            }
            heap_relation_ = OidIsValid(relation_oid)
                ? bioseq::postgres::support::invoke_pg_as_cpp([relation_oid] {
                    return bioseq::postgres::support::try_open_postgres_relation(
                        relation_oid, AccessShareLock);
                })
                : nullptr;
            if (heap_relation_ != nullptr) {
                stats_.cold.add(stats_.cold.sidecar_heap_opens, 1);
                stats_.cold.add(stats_.cold.lock_acquires, 1);
            }
            if (heap_relation_ == nullptr && !classification_index_name.empty()) {
                throw minimizer_generation_relation_open_failure{
                    .inspection = {
                        .kind = minimizer_generation_relation_kind::missing,
                        .schema_error = minimizer_generation_schema_error::none,
                    },
                };
            }
            if (heap_relation_ == nullptr) {
                minimizer_generation_storage_error(
                    "generation relation is missing or renamed");
            }
            const bool relation_identity_exact =
                RelationGetNamespace(heap_relation_) == bioseq_internal_ns()
                && std::string_view{RelationGetRelationName(heap_relation_)}
                    == table_name_;
            const Oid primary_index_oid = bioseq::postgres::support::invoke_pg_as_cpp(
                [this] {
                    return RelationGetPrimaryKeyIndex(heap_relation_);
                });
            if (OidIsValid(primary_index_oid)) {
                primary_index_relation_ = bioseq::postgres::support::invoke_pg_as_cpp(
                    [primary_index_oid] {
                        return index_open(
                            primary_index_oid, AccessShareLock);
                    });
                stats_.cold.add(stats_.cold.sidecar_index_opens, 1);
                stats_.cold.add(stats_.cold.lock_acquires, 1);
            }
            const minimizer_generation_schema_inspection schema = relation_identity_exact
                ? bioseq::postgres::support::invoke_pg_as_cpp([this] {
                    return inspect_bound_minimizer_generation_schema(
                        heap_relation_, primary_index_relation_,
                        minimizer_generation_schema_expectation{
                            .namespace_oid = bioseq_internal_ns(),
                            .relation_name = table_name_,
                            .tablespace_oid = expected_tablespace_oid_,
                        });
                })
                : minimizer_generation_schema_inspection{
                    .error = minimizer_generation_schema_error::not_heap,
                };
            stats_.cold.add(stats_.cold.sidecar_schema_validations, 1);
            if (!schema) {
                if (!classification_index_name.empty()) {
                    if (primary_index_relation_ != nullptr) {
                        Relation finished_index = std::exchange(
                            primary_index_relation_, nullptr);
                        bioseq::postgres::support::invoke_pg_as_cpp([finished_index] {
                            index_close(finished_index, AccessShareLock);
                        });
                        stats_.cold.add(stats_.cold.lock_releases, 1);
                    }
                    Relation finished_heap = std::exchange(
                        heap_relation_, nullptr);
                    bioseq::postgres::support::invoke_pg_as_cpp([finished_heap] {
                        relation_close(finished_heap, AccessShareLock);
                    });
                    stats_.cold.add(stats_.cold.lock_releases, 1);
                    if (snapshot_registered_by_session_ && snapshot_ != nullptr) {
                        Snapshot finished_snapshot = snapshot_;
                        snapshot_ = nullptr;
                        snapshot_registered_by_session_ = false;
                        bioseq::postgres::support::invoke_pg_as_cpp([finished_snapshot] {
                            UnregisterSnapshot(finished_snapshot);
                        });
                    }
                    throw minimizer_generation_relation_open_failure{
                        .inspection = {
                            .kind = minimizer_generation_relation_kind::malformed,
                            .schema_error = schema.error,
                        },
                    };
                }
                minimizer_generation_storage_error(fmt::format(
                    "generation relation schema mismatch: {}",
                    minimizer_generation_schema_error_name(schema.error)));
            }
            (void)verify_pg_inline_payload_layout(heap_relation_);
            stats_.relation_opens = 1;
            add_cold_index_elapsed(
                stats_.cold, stats_.cold.bind_ns, bind_begin_ns);
            add_cold_index_buffer_usage(stats_.cold, bind_buffer_begin);
        } catch (const bioseq::postgres::support::postgres_error&) {
            state_ = operation_state::failed;
            abandon_relations_noexcept_();
            throw;
        }
    }
    index_storage_fragment_generation_read_session(
        const index_storage_fragment_generation_read_session&) = delete;
    index_storage_fragment_generation_read_session& operator=(
        const index_storage_fragment_generation_read_session&) = delete;
    index_storage_fragment_generation_read_session(
            index_storage_fragment_generation_read_session&& other) noexcept
        : table_name_(std::move(other.table_name_)),
          heap_relation_(std::exchange(other.heap_relation_, nullptr)),
          primary_index_relation_(std::exchange(
              other.primary_index_relation_, nullptr)),
          storage_descriptor_relation_(std::exchange(
              other.storage_descriptor_relation_, nullptr)),
          storage_descriptor_live_index_relation_(std::exchange(
              other.storage_descriptor_live_index_relation_, nullptr)),
          active_scan_(std::exchange(other.active_scan_, nullptr)),
          full_generation_batch_slots_(
              other.full_generation_batch_slots_),
          full_generation_batch_slot_count_(std::exchange(
              other.full_generation_batch_slot_count_, 0)),
          snapshot_(std::exchange(other.snapshot_, nullptr)),
          snapshot_registered_by_session_(std::exchange(
              other.snapshot_registered_by_session_, false)),
          query_key_(other.query_key_),
          expected_tablespace_oid_(other.expected_tablespace_oid_),
          expected_(other.expected_),
          expected_profile_(other.expected_profile_),
          state_(other.state_),
          catalog_source_authorized_(other.catalog_source_authorized_),
          metadata_permit_(std::move(other.metadata_permit_)),
          plan_permit_(std::move(other.plan_permit_)),
          runtime_permit_(std::move(other.runtime_permit_)),
          control_authorization_permit_(std::move(
              other.control_authorization_permit_)),
          head_(std::move(other.head_)),
          metadata_(std::move(other.metadata_)),
          manifest_only_(std::move(other.manifest_only_)),
          directory_bytes_scratch_(std::move(
              other.directory_bytes_scratch_)),
          planned_groups_(std::move(other.planned_groups_)),
          runtime_requests_(std::move(other.runtime_requests_)),
          authorized_position_ordinals_(std::move(
              other.authorized_position_ordinals_)),
          next_group_(other.next_group_),
          previous_payload_heap_block_(other.previous_payload_heap_block_),
          payload_stage_begin_ns_(other.payload_stage_begin_ns_),
          stats_(other.stats_) {
        other.full_generation_batch_slots_.fill(nullptr);
        other.state_ = operation_state::moved;
    }
    index_storage_fragment_generation_read_session& operator=(
        index_storage_fragment_generation_read_session&&) = delete;
    ~index_storage_fragment_generation_read_session() {
        abandon_relations_noexcept_();
    }

private:
    [[nodiscard]] minimizer_generation_head read_active_head_impl_(
            bool bind_expected_layout) {
        minimizer_generation_head result{};
        const std::uint64_t head_begin_ns = cold_index_now_ns();
        const BufferUsage head_buffer_begin = pgBufferUsage;
        try {
            if (state_ != operation_state::opened) {
                checked_add_(stats_.metadata_repeated_reads, 1,
                    "repeated metadata counter overflow");
                fail_("active generation head was read more than once or out of order");
            }
            ScanKeyData keys[5];
            ScanKeyInit(&keys[0], k_minimizer_generation_index_generation_id,
                BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum(0));
            ScanKeyInit(&keys[1], k_minimizer_generation_index_query_key,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(query_key_)));
            ScanKeyInit(&keys[2], k_minimizer_generation_index_sub_key,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(
                    k_generation_head_sub_key)));
            ScanKeyInit(&keys[3], k_minimizer_generation_index_position_segment,
                BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(-1));
            ScanKeyInit(&keys[4], k_minimizer_generation_index_chunk_number,
                BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(0));
            active_scan_ = bioseq::postgres::support::invoke_pg_as_cpp(
                [this, &keys] {
                return systable_beginscan_ordered(
                    heap_relation_, primary_index_relation_, snapshot_, 5,
                    keys);
            });
            const auto next_tuple = [this] {
                return bioseq::postgres::support::invoke_pg_as_cpp([this] {
                    return systable_getnext_ordered(
                        active_scan_, ForwardScanDirection);
                });
            };
            HeapTuple tuple = next_tuple();
            if (!HeapTupleIsValid(tuple)) {
                fail_("active generation head is missing");
            }
            loaded_row_type row =
                load_minimizer_generation_row_or_error(heap_relation_, tuple);
            std::array<std::byte, minimizer_generation_head::encoded_bytes> bytes{};
            if (row.query_key != query_key_
                || row.address != index_chunk_address{
                    .generation_id = 0,
                    .position_segment = -1,
                    .sub_key = k_generation_head_sub_key,
                    .chunk_no = 0,
                }
                || row.bytes.size() != bytes.size()) {
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
            auto decoded =
                bioseq::indexing::decode_minimizer_generation_head(bytes);
            if (!decoded || !bioseq::indexing::sql_bigint_can_represent(
                    decoded->generation_id)) {
                fail_("active generation head fixed wire is invalid");
            }
            if (bind_expected_layout
                && decoded->layout != expected_profile_.layout) {
                fail_(
                    "active generation head wire format disagrees with reader");
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
            add_cold_index_elapsed(
                stats_.cold, stats_.cold.head_ns, head_begin_ns);
            add_cold_index_buffer_usage(stats_.cold, head_buffer_begin);
        } catch (const bioseq::postgres::support::postgres_error&) {
            state_ = operation_state::failed;
            abandon_relations_noexcept_();
            throw;
        }
        return result;
    }

public:
    /// Read and authenticate an active head bound to the caller's independently
    /// selected expected format.
    [[nodiscard]] minimizer_generation_head read_active_head_once() {
        return read_active_head_impl_(/*bind_expected_layout=*/true);
    }

    /// Administrative wire-format discovery. This authenticates the head,
    /// defers only its expected-format comparison, and closes the head-only
    /// session before returning. The caller must reopen a normally bound
    /// session before reading manifest or payload bytes.
    [[nodiscard]] minimizer_generation_head
    discover_active_head_layout_and_close() {
        minimizer_generation_head result = read_active_head_impl_(
            /*bind_expected_layout=*/false);
        close_head_read();
        return result;
    }

    /// Timed compact-direct metadata path. Read the exact manifest tuple under
    /// the same snapshot as the head, using chunk_no=0 in the primary-index
    /// qualification so no directory tuple can be visited. The fixed manifest
    /// and its head checksum remain fully authenticated; only the O(directory)
    /// materialization/CRC/grouped_soa_directory_view construction is intentionally omitted.
    [[nodiscard]] const minimizer_generation_manifest&
    read_manifest_once_without_directory(
            const minimizer_generation_head& expected_head) {
        const std::uint64_t manifest_begin_ns = cold_index_now_ns();
        const BufferUsage manifest_buffer_begin = pgBufferUsage;
        try {
            if (state_ != operation_state::head_read || !head_
                || expected_head != *head_ || metadata_ || manifest_only_
                || expected_head.layout
                    != bioseq::indexing::minimizer_generation_layout::
                        grouped_soa_segment_local) {
                checked_add_(stats_.metadata_repeated_reads, 1,
                    "repeated metadata counter overflow");
                fail_(
                    "manifest-only read repeated, early, changed, or not grouped-SoA");
            }

            ScanKeyData keys[5];
            ScanKeyInit(&keys[0], k_minimizer_generation_index_generation_id,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(
                    expected_head.generation_id)));
            ScanKeyInit(&keys[1], k_minimizer_generation_index_query_key,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(query_key_)));
            ScanKeyInit(&keys[2], k_minimizer_generation_index_sub_key,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(
                    k_directory_sub_key)));
            ScanKeyInit(&keys[3], k_minimizer_generation_index_position_segment,
                BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(-1));
            ScanKeyInit(&keys[4], k_minimizer_generation_index_chunk_number,
                BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(0));
            active_scan_ = bioseq::postgres::support::invoke_pg_as_cpp(
                [this, &keys] {
                return systable_beginscan_ordered(
                    heap_relation_, primary_index_relation_, snapshot_, 5,
                    keys);
            });
            const auto next_tuple = [this] {
                return bioseq::postgres::support::invoke_pg_as_cpp([this] {
                    return systable_getnext_ordered(
                        active_scan_, ForwardScanDirection);
                });
            };
            HeapTuple tuple = next_tuple();
            if (!HeapTupleIsValid(tuple)) {
                fail_("READY generation manifest is missing");
            }
            loaded_row_type manifest_row =
                load_minimizer_generation_row_or_error(heap_relation_, tuple);
            std::array<std::byte,
                minimizer_generation_manifest::encoded_bytes>
                manifest_bytes{};
            constexpr std::size_t k_expected_manifest_bytes =
                minimizer_generation_manifest::encoded_bytes;
            if (manifest_row.query_key != query_key_
                || manifest_row.address != index_chunk_address{
                    .generation_id = expected_head.generation_id,
                    .position_segment = -1,
                    .sub_key = k_directory_sub_key,
                    .chunk_no = 0,
                }
                || manifest_row.bytes.size() != k_expected_manifest_bytes) {
                manifest_row.release();
                fail_("READY generation manifest row shape disagrees");
            }
            std::copy(
                manifest_row.bytes.begin(), manifest_row.bytes.end(),
                manifest_bytes.begin());
            manifest_row.release();
            if (HeapTupleIsValid(next_tuple())) {
                fail_("READY generation manifest scan returned duplicate rows");
            }
            bioseq::postgres::support::invoke_pg_as_cpp(
                [this] { systable_endscan_ordered(active_scan_); });
            active_scan_ = nullptr;

            const std::span<const std::byte> encoded_manifest{
                manifest_bytes};
            auto manifest =
                bioseq::indexing::decode_minimizer_generation_manifest(
                    encoded_manifest);
            if (!manifest
                || manifest->layout != expected_head.layout
                || manifest->generation_id != expected_head.generation_id
                || manifest->source_identity
                    != expected_head.source_identity
                || manifest->source_content_checksum
                    != expected_head.source_content_checksum
                || manifest->directory_bytes
                    > (std::numeric_limits<std::size_t>::max)()
                || bioseq::indexing::crc64_ecma182(encoded_manifest)
                    != expected_head.manifest_checksum
                || bioseq::indexing::
                    compute_minimizer_generation_storage_contract_checksum(*manifest)
                    != expected_head.storage_contract_checksum
                || !bioseq::indexing::
                    validate_minimizer_generation_storage_contract(
                        *manifest, expected_)) {
                fail_("READY generation manifest/head binding disagrees");
            }
            const auto manifest_profile =
                bioseq::indexing::derive_grouped_soa_minimizer_profile(
                    *manifest);
            if (!manifest_profile
                || manifest_profile->layout != expected_.layout) {
                fail_(
                    "READY generation manifest payload profile is invalid");
            }
            const std::uint32_t extent_payload_bytes =
                bioseq::indexing::minimizer_extent_payload_bytes(
                    manifest->layout);
            if (extent_payload_bytes == 0u) {
                fail_("READY manifest wire quantum is invalid");
            }
            const std::uint64_t expected_directory_chunks =
                manifest->directory_bytes / extent_payload_bytes
                + (manifest->directory_bytes % extent_payload_bytes != 0u
                    ? 1u : 0u);
            const std::size_t directory_entry_bytes =
                bioseq::indexing::
                    minimizer_directory_entry_encoded_bytes(
                        *manifest_profile);
            if (expected_directory_chunks
                    != manifest->directory_chunk_count
                || directory_entry_bytes == 0u
                || manifest->directory_bytes % directory_entry_bytes
                    != 0u) {
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
                minimizer_generation_head::encoded_bytes
                + k_expected_manifest_bytes;
            checked_add_(stats_.index_scans, 1u,
                "metadata index scan count overflow");
            checked_add_(stats_.explicit_copy_bytes,
                k_expected_manifest_bytes,
                "metadata copy byte count overflow");
            add_cold_index_elapsed(
                stats_.cold, stats_.cold.directory_ns,
                manifest_begin_ns);
            add_cold_index_buffer_usage(
                stats_.cold, manifest_buffer_begin);
        } catch (const bioseq::postgres::support::postgres_error&) {
            state_ = operation_state::failed;
            abandon_relations_noexcept_();
            throw;
        }
        return *manifest_only_;
    }

    [[nodiscard]] const minimizer_generation_read_metadata&
    read_manifest_directory_once(const minimizer_generation_head& expected_head) {
        const std::uint64_t directory_begin_ns = cold_index_now_ns();
        const BufferUsage directory_buffer_begin = pgBufferUsage;
        try {
        if (state_ != operation_state::head_read || !head_
            || expected_head != *head_ || metadata_ || manifest_only_) {
            checked_add_(stats_.metadata_repeated_reads, 1,
                "repeated metadata counter overflow");
            fail_("manifest/directory read repeated, early, or changed head identity");
        }
        ScanKeyData keys[4];
        ScanKeyInit(&keys[0], k_minimizer_generation_index_generation_id,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(
                expected_head.generation_id)));
        ScanKeyInit(&keys[1], k_minimizer_generation_index_query_key,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(query_key_)));
        ScanKeyInit(&keys[2], k_minimizer_generation_index_sub_key,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(k_directory_sub_key)));
        ScanKeyInit(&keys[3], k_minimizer_generation_index_position_segment,
            BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(-1));
        active_scan_ = bioseq::postgres::support::invoke_pg_as_cpp(
            [this, &keys] {
            return systable_beginscan_ordered(
                heap_relation_, primary_index_relation_, snapshot_, 4,
                keys);
        });
        const auto next_tuple = [this] {
            return bioseq::postgres::support::invoke_pg_as_cpp([this] {
                return systable_getnext_ordered(
                    active_scan_, ForwardScanDirection);
            });
        };
        HeapTuple tuple = next_tuple();
        if (!HeapTupleIsValid(tuple)) {
            fail_("READY generation manifest is missing");
        }
        loaded_row_type manifest_row =
            load_minimizer_generation_row_or_error(heap_relation_, tuple);
        std::array<std::byte,
            minimizer_generation_manifest::encoded_bytes>
            manifest_bytes{};
        const std::size_t expected_manifest_bytes =
            bioseq::indexing::minimizer_manifest_encoded_bytes(
                expected_head.layout);
        if (manifest_row.query_key != query_key_
            || manifest_row.address != index_chunk_address{
                .generation_id = expected_head.generation_id,
                .position_segment = -1,
                .sub_key = k_directory_sub_key,
                .chunk_no = 0,
            }
            || expected_manifest_bytes == 0
            || manifest_row.bytes.size() != expected_manifest_bytes) {
            manifest_row.release();
            fail_("READY generation manifest row shape disagrees");
        }
        std::copy_n(manifest_row.bytes.begin(), expected_manifest_bytes,
            manifest_bytes.begin());
        manifest_row.release();
        auto manifest = bioseq::indexing::decode_minimizer_generation_manifest(
            std::span<const std::byte>{manifest_bytes}.first(
                expected_manifest_bytes));
        if (!manifest
            || manifest->generation_id != expected_head.generation_id
            || manifest->source_identity != expected_head.source_identity
            || manifest->directory_bytes
                > (std::numeric_limits<std::size_t>::max)()) {
            fail_("READY generation manifest fixed wire disagrees");
        }
        if (!bioseq::indexing::validate_minimizer_generation_storage_contract(
                *manifest, expected_)) {
            fail_("READY generation minimizer storage contract disagrees");
        }
        const auto manifest_profile =
            bioseq::indexing::derive_grouped_soa_minimizer_profile(
                *manifest);
        if (!manifest_profile
            || manifest_profile->layout != expected_.layout) {
            fail_("READY generation manifest payload profile is invalid");
        }
        expected_profile_ = *manifest_profile;
        const std::uint32_t extent_payload_bytes =
            bioseq::indexing::minimizer_extent_payload_bytes(
                manifest->layout);
        if (extent_payload_bytes == 0) {
            fail_("READY manifest wire quantum is invalid");
        }
        const std::uint64_t expected_directory_chunks =
            manifest->directory_bytes
                / extent_payload_bytes
            + (manifest->directory_bytes
                    % extent_payload_bytes
                != 0 ? 1u : 0u);
        if (expected_directory_chunks != manifest->directory_chunk_count) {
            fail_("READY manifest directory chunk equation disagrees");
        }
        const std::size_t directory_entry_bytes =
            bioseq::indexing::minimizer_directory_entry_encoded_bytes(
                expected_profile_);
        if (directory_entry_bytes == 0
            || manifest->directory_bytes % directory_entry_bytes != 0) {
            fail_("READY manifest directory entry equation disagrees");
        }
        const std::uint64_t entry_count = manifest->directory_bytes
            / directory_entry_bytes;
        if (entry_count
                > (std::numeric_limits<std::uint64_t>::max)()
                    / sizeof(grouped_soa_bucket_directory_entry)) {
            fail_("READY metadata admission size overflow");
        }
        const std::uint64_t entry_storage =
            entry_count * sizeof(grouped_soa_bucket_directory_entry);
        const std::uint64_t directory_owned_logical =
            bioseq::common::saturating_add(
                manifest->directory_bytes, entry_storage);
        const std::uint64_t scratch_headroom =
            bioseq::common::saturating_mul(
                manifest->directory_bytes,
                bioseq::parallelism::k_vector_preallocation_headroom_factor);
        const std::uint64_t admitted_bytes =
            bioseq::common::saturating_add(
                scratch_headroom, directory_owned_logical);
        if (directory_owned_logical
                == (std::numeric_limits<std::uint64_t>::max)()
            || scratch_headroom
                == (std::numeric_limits<std::uint64_t>::max)()
            || admitted_bytes
                == (std::numeric_limits<std::uint64_t>::max)()) {
            fail_("READY metadata admission size overflow");
        }
        auto metadata_permit =
            bioseq::parallelism::memory_admission_controller::instance()
                .try_reserve(admitted_bytes);
        if (!metadata_permit) {
            fail_("READY metadata memory admission was denied");
        }
        metadata_permit_ = std::move(*metadata_permit);
        directory_bytes_scratch_.clear();
        directory_bytes_scratch_.reserve(
            static_cast<std::size_t>(manifest->directory_bytes));
        const std::uint64_t measured_scratch_bytes =
            bioseq::common::saturating_mul(
                directory_bytes_scratch_.capacity(), sizeof(std::byte));
        const std::uint64_t measured_metadata_bytes =
            bioseq::common::saturating_add(
                measured_scratch_bytes, directory_owned_logical);
        if (measured_scratch_bytes
                == (std::numeric_limits<std::uint64_t>::max)()
            || measured_metadata_bytes
                == (std::numeric_limits<std::uint64_t>::max)()
            || measured_metadata_bytes > admitted_bytes) {
            fail_("READY metadata scratch capacity exceeded admission");
        }
        metadata_permit_.shrink_to(measured_metadata_bytes);
        for (std::uint32_t chunk_no = 1;
             chunk_no <= manifest->directory_chunk_count; ++chunk_no) {
            tuple = next_tuple();
            if (!HeapTupleIsValid(tuple)) {
                fail_("READY generation directory has missing chunks");
            }
            loaded_row_type directory_row =
                load_minimizer_generation_row_or_error(heap_relation_, tuple);
            const std::uint64_t offset =
                static_cast<std::uint64_t>(chunk_no - 1u)
                * extent_payload_bytes;
            const std::uint64_t expected_bytes = std::min<std::uint64_t>(
                extent_payload_bytes,
                manifest->directory_bytes - offset);
            if (directory_row.query_key != query_key_
                || directory_row.address != index_chunk_address{
                    .generation_id = expected_head.generation_id,
                    .position_segment = -1,
                    .sub_key = k_directory_sub_key,
                    .chunk_no = chunk_no,
                }
                || directory_row.bytes.size() != expected_bytes) {
                directory_row.release();
                fail_("READY generation directory row shape disagrees");
            }
            directory_bytes_scratch_.insert(directory_bytes_scratch_.end(),
                directory_row.bytes.begin(), directory_row.bytes.end());
            directory_row.release();
        }
        if (HeapTupleIsValid(next_tuple())) {
            fail_("READY generation directory scan returned extra rows");
        }
        bioseq::postgres::support::invoke_pg_as_cpp(
            [this] { systable_endscan_ordered(active_scan_); });
        active_scan_ = nullptr;
        if (directory_bytes_scratch_.size() != manifest->directory_bytes
            || bioseq::indexing::crc64_ecma182(
                std::span<const std::byte>{directory_bytes_scratch_})
                != manifest->directory_checksum) {
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
        std::optional<std::pair<index_sub_key_t, std::int32_t>>
            previous_storage_key;
        std::uint64_t full_payload_rows = 0;
        for (const grouped_soa_bucket_directory_entry& entry
             : metadata_->directory.entries()) {
            const std::pair storage_key{
                entry.sub_key, entry.position_segment};
            if (previous_storage_key
                && !(*previous_storage_key < storage_key)) {
                fail_("READY directory is not in physical storage order");
            }
            previous_storage_key = storage_key;
            stats_.cold.add(
                full_payload_rows, entry.control_chunk_count);
            stats_.cold.add(
                full_payload_rows, entry.position_chunk_count);
        }
        stats_.cold.route_full_rows = full_payload_rows;
        stats_.cold.add(
            stats_.cold.route_full_bytes,
            metadata_->manifest.payload_bytes);
        stats_.cold.add(
            stats_.cold.route_full_cost, full_payload_rows);
        stats_.cold.add(
            stats_.cold.route_full_cost,
            k_ordered_range_scan_penalty_rows);
        metadata_permit_.shrink_to(
            metadata_->directory.owned_capacity_bytes());
        state_ = operation_state::metadata_pending_source;
        stats_.manifest_directory_scans = 1;
        stats_.cold.directory_scans = 1;
        stats_.manifest_rows_read = 1;
        stats_.manifest_bytes_read = expected_manifest_bytes;
        stats_.directory_chunks_read = manifest->directory_chunk_count;
        stats_.directory_bytes_read = manifest->directory_bytes;
        stats_.metadata_heap_rows_read = 2u
            + manifest->directory_chunk_count;
        stats_.metadata_bytes_read = minimizer_generation_head::encoded_bytes
            + expected_manifest_bytes
            + manifest->directory_bytes;
        checked_add_(stats_.index_scans, 1,
            "metadata index scan count overflow");
        checked_add_(stats_.explicit_copy_bytes,
            expected_manifest_bytes,
            "metadata copy byte count overflow");
        checked_add_(stats_.explicit_copy_bytes,
            manifest->directory_bytes,
            "directory copy byte count overflow");
        add_cold_index_elapsed(
            stats_.cold, stats_.cold.directory_ns,
            directory_begin_ns);
        add_cold_index_buffer_usage(stats_.cold, directory_buffer_begin);
        } catch (const bioseq::postgres::support::postgres_error&) {
            state_ = operation_state::failed;
            abandon_relations_noexcept_();
            throw;
        }
        return *metadata_;
    }

    /// Bind metadata to an independently reopened source row. This is the only
    /// transition to metadata_ready: source identity alone is insufficient.
    void consume_full_source_authorization(
            minimizer_generation_source_authorization&& authorization) {
        try {
            const minimizer_generation_manifest* const manifest =
                active_manifest_();
            if (state_ != operation_state::metadata_pending_source
                || !head_ || manifest == nullptr) {
                authorization.invalidate_();
                fail_("READY source authorization was consumed out of order");
            }
            if (!authorization.valid_
                || authorization.snapshot_ != snapshot_
                || authorization.database_oid_ != MyDatabaseId
                || authorization.source_.sequence_rowno != query_key_
                || authorization.table_name_ != table_name_) {
                authorization.invalidate_();
                fail_("READY source authorization relation/query/snapshot binding disagrees");
            }
            const std::uint64_t storage_contract_checksum =
                bioseq::indexing::compute_minimizer_generation_storage_contract_checksum(
                    *manifest);
            authorization.source_.storage_contract_checksum =
                storage_contract_checksum;
            const std::uint64_t source_identity = minimizer_generation_source_identity(
                authorization.main_relfilenode_, authorization.source_);
            const std::uint64_t source_content_checksum =
                authorization.source_content_checksum_;
            if (source_identity == 0
                || manifest->alphabet_code
                    != authorization.source_.alphabet_code
                || manifest->storage_unit_byte_width
                    != authorization.source_.storage_unit_byte_width
                || manifest->target_length
                    != authorization.source_.locator.logical_length
                || head_->source_identity != source_identity
                || manifest->source_identity != source_identity
                || head_->source_content_checksum
                    != source_content_checksum
                || manifest->source_content_checksum
                    != source_content_checksum
                || head_->storage_contract_checksum
                    != storage_contract_checksum
                || bioseq::indexing::compute_minimizer_generation_storage_contract_checksum(
                    *manifest) != storage_contract_checksum) {
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

    /// Timed cold-reader transition. Only the constant-work catalog authorization is
    /// accepted here; the full decoded authorization has a separate, explicitly named
    /// untimed writer/admin transition above.
    void consume_source_authorization(minimizer_generation_catalog_source_authorization&& authorization) {
        try {
            const minimizer_generation_manifest* const manifest =
                active_manifest_();
            if (state_ != operation_state::metadata_pending_source
                || !head_ || manifest == nullptr) {
                authorization.invalidate_();
                fail_("READY catalog source authorization was consumed out of order");
            }
            if (!authorization.valid_
                || !authorization.stats_.timed_equation_holds()
                || authorization.snapshot_ != snapshot_
                || authorization.database_oid_ != MyDatabaseId
                || authorization.source_.sequence_rowno != query_key_
                || authorization.table_name_ != table_name_) {
                authorization.invalidate_();
                fail_("READY catalog source authorization binding/work equation disagrees");
            }
            const std::uint64_t storage_contract_checksum =
                bioseq::indexing::compute_minimizer_generation_storage_contract_checksum(
                    *manifest);
            authorization.source_.storage_contract_checksum =
                storage_contract_checksum;
            const std::uint64_t source_identity = minimizer_generation_source_identity(
                authorization.main_relfilenode_, authorization.source_);
            const std::uint64_t source_content_checksum =
                authorization.source_content_checksum_;
            if (source_identity == 0
                || manifest->alphabet_code
                    != authorization.source_.alphabet_code
                || manifest->storage_unit_byte_width
                    != authorization.source_.storage_unit_byte_width
                || manifest->target_length
                    != authorization.source_.locator.logical_length
                || head_->source_identity != source_identity
                || manifest->source_identity != source_identity
                || head_->source_content_checksum
                    != source_content_checksum
                || manifest->source_content_checksum
                    != source_content_checksum
                || head_->storage_contract_checksum
                    != storage_contract_checksum) {
                authorization.invalidate_();
                fail_("READY metadata/catalog source authorization disagrees");
            }
            stats_.source_parent_rows_read = authorization.stats_.parent_rows_read;
            stats_.source_catalog_rows_read = authorization.stats_.catalog_rows_read;
            stats_.source_catalog_index_scans =
                authorization.stats_.catalog_index_scans;
            stats_.source_fragment_rows_read =
                authorization.stats_.fragment_rows_read;
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

    /// Bind the exact LIVE image descriptor under the same registered MVCC
    /// snapshot that supplied the head and manifest.  Both catalog locks stay
    /// owned by this session until the mapped image has been registered and
    /// the selected mapped grouped-SoA finish transition performs the checked
    /// close.
    [[nodiscard]] minimizer_generation_storage_descriptor
    read_live_storage_descriptor_once(
            const minimizer_generation_administration_identity& identity) {
        try {
            const minimizer_generation_manifest* const manifest =
                active_manifest_();
            if (state_ != operation_state::metadata_ready
                || !catalog_source_authorized_ || !head_
                || manifest == nullptr
                || manifest->layout
                    != bioseq::indexing::minimizer_generation_layout::
                        grouped_soa_segment_local
                || storage_descriptor_relation_ != nullptr
                || storage_descriptor_live_index_relation_ != nullptr
                || identity.table_name != table_name_
                || identity.database_oid != MyDatabaseId
                || identity.tablespace_oid != expected_tablespace_oid_
                || identity.index_name
                    != k_minimizers_per_fragment_index_name) {
                fail_(
                    "LIVE generation descriptor was requested out of order");
            }
            const Oid descriptor_oid =
                bioseq::postgres::support::invoke_pg_as_cpp([] {
                    return get_relname_relid(
                        "minimizer_generation_storage",
                        bioseq_internal_ns());
                });
            const Oid live_index_oid =
                bioseq::postgres::support::invoke_pg_as_cpp([] {
                    return get_relname_relid(
                        "minimizer_generation_storage_one_live",
                        bioseq_internal_ns());
                });
            if (!OidIsValid(descriptor_oid) || !OidIsValid(live_index_oid)) {
                fail_("generation storage descriptor authority is missing");
            }
            storage_descriptor_relation_ =
                bioseq::postgres::support::invoke_pg_as_cpp([descriptor_oid] {
                    return bioseq::postgres::support::try_open_postgres_relation(
                        descriptor_oid, AccessShareLock);
                });
            if (storage_descriptor_relation_ == nullptr) {
                fail_("generation storage descriptor relation disappeared");
            }
            stats_.cold.add(stats_.cold.lock_acquires, 1);
            storage_descriptor_live_index_relation_ =
                bioseq::postgres::support::invoke_pg_as_cpp([live_index_oid] {
                    return index_open(live_index_oid, AccessShareLock);
                });
            stats_.cold.add(stats_.cold.lock_acquires, 1);
            if (RelationGetNamespace(storage_descriptor_relation_)
                    != bioseq_internal_ns()
                || std::string_view{RelationGetRelationName(
                    storage_descriptor_relation_)}
                    != "minimizer_generation_storage"
                || RelationGetNamespace(
                    storage_descriptor_live_index_relation_)
                    != bioseq_internal_ns()) {
                fail_("generation storage descriptor identity changed");
            }
            bioseq::storage::
                validate_minimizer_generation_storage_catalogs_pg();
            auto descriptor =
                read_live_minimizer_generation_storage_descriptor_pg(
                    storage_descriptor_relation_,
                    storage_descriptor_live_index_relation_, snapshot_,
                    identity, query_key_, head_->generation_id);
            if (descriptor.payload_relation_oid
                        != std::optional<Oid>{RelationGetRelid(heap_relation_)}
                    || descriptor.payload_database_oid
                        != std::optional<Oid>{heap_relation_->rd_locator.dbOid}
                    || descriptor.payload_tablespace_oid
                        != std::optional<Oid>{heap_relation_->rd_locator.spcOid}
                    || descriptor.payload_relfilenode
                        != std::optional<Oid>{
                            static_cast<Oid>(
                                heap_relation_->rd_locator.relNumber)}
                    || descriptor.payload_relation_name
                        != std::optional<std::string>{table_name_}) {
                fail_("LIVE relation-backed descriptor locator disagrees");
            }
            return descriptor;
        } catch (...) {
            cleanup_failed_session_and_rethrow_();
        }
    }

private:
    [[nodiscard]] minimizer_generation_read_stats
    finish_full_generation_() {
        if (state_ != operation_state::full_generation_payload_complete
            || !metadata_ || next_group_ != 0 || !planned_groups_.empty()
            || !runtime_requests_.empty() || plan_permit_ || runtime_permit_
            || control_authorization_permit_
            || !authorized_position_ordinals_.empty()
            || active_scan_ != nullptr || heap_relation_ == nullptr
            || primary_index_relation_ == nullptr || snapshot_ == nullptr) {
            fail_("full-generation read finished with incomplete or leaked state");
        }
        std::uint64_t expected_metadata_rows = 0;
        checked_add_(expected_metadata_rows, stats_.head_rows_read,
            "full-generation metadata row equation overflow");
        checked_add_(expected_metadata_rows, stats_.manifest_rows_read,
            "full-generation metadata row equation overflow");
        checked_add_(expected_metadata_rows, stats_.directory_chunks_read,
            "full-generation metadata row equation overflow");
        std::uint64_t expected_metadata_bytes = 0;
        checked_add_(expected_metadata_bytes, stats_.head_bytes_read,
            "full-generation metadata byte equation overflow");
        checked_add_(expected_metadata_bytes, stats_.manifest_bytes_read,
            "full-generation metadata byte equation overflow");
        checked_add_(expected_metadata_bytes, stats_.directory_bytes_read,
            "full-generation metadata byte equation overflow");
        std::uint64_t expected_index_scans = 0;
        checked_add_(expected_index_scans, stats_.head_scans,
            "full-generation index scan equation overflow");
        checked_add_(expected_index_scans,
            stats_.manifest_directory_scans,
            "full-generation index scan equation overflow");
        checked_add_(expected_index_scans,
            stats_.full_generation.scan_calls,
            "full-generation index scan equation overflow");
        std::uint64_t streamed_chunks = 0;
        checked_add_(streamed_chunks, stats_.full_generation.control_chunks,
            "full-generation chunk equation overflow");
        checked_add_(streamed_chunks, stats_.full_generation.position_chunks,
            "full-generation chunk equation overflow");
        std::uint64_t streamed_bytes = 0;
        checked_add_(streamed_bytes, stats_.full_generation.control_bytes,
            "full-generation byte equation overflow");
        checked_add_(streamed_bytes, stats_.full_generation.position_bytes,
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
        const std::uint64_t manifest_extents =
            metadata_->manifest.extent_count;
        if (stats_.relation_opens != 1
            || stats_.head_scans != 1 || stats_.head_rows_read != 1
            || stats_.head_bytes_read
                != minimizer_generation_head::encoded_bytes
            || stats_.manifest_directory_scans != 1
            || stats_.manifest_rows_read != 1
            || stats_.manifest_bytes_read
                != bioseq::indexing::minimizer_manifest_encoded_bytes(
                    metadata_->manifest.layout)
            || stats_.metadata_heap_rows_read != expected_metadata_rows
            || stats_.metadata_bytes_read != expected_metadata_bytes
            || stats_.full_generation.scan_calls != 1
            || stats_.full_generation.planned_extents != manifest_extents
            || stats_.full_generation.completed_extents != manifest_extents
            || streamed_chunks != stats_.payload.delivered_chunks
            || streamed_bytes != metadata_->manifest.payload_bytes
            || stats_.full_generation.payload_row_checksum
                != metadata_->manifest.payload_checksum
            || stats_.planned_request_groups != 1
            || stats_.executed_request_groups != 1
            || stats_.bulk_read_calls != 1 || stats_.range_scan_calls != 1
            || stats_.payload.index_scans != 1
            || stats_.payload.requested_ranges != 1
            || stats_.payload.heap_rows_read != streamed_chunks
            || stats_.payload_heap_rows_read != streamed_chunks
            || stats_.payload_bytes_read != streamed_bytes
            || stats_.payload.wanted_subkeys != manifest_extents
            || stats_.payload.read_subkeys != manifest_extents
            || stats_.payload.wanted_extents != manifest_extents
            || stats_.payload.read_extents != manifest_extents
            || stats_.payload.wanted_chunks != streamed_chunks
            || stats_.payload.delivered_chunks != streamed_chunks
            || stats_.payload.delivered_bytes != streamed_bytes
            || stats_.payload.skipped_rows != 0
            || stats_.payload.skip_budget_rows != 0
            || stats_.cold.route_empty != 0
            || stats_.cold.route_selective != 0
            || stats_.cold.route_full != 1
            || stats_.cold.payload_rows_requested != streamed_chunks
            || stats_.cold.payload_rows_delivered != streamed_chunks
            || stats_.cold.payload_rows_scanned != streamed_chunks
            || stats_.cold.payload_rows_skipped != 0
            || stats_.cold.payload_bytes_requested != streamed_bytes
            || stats_.cold.payload_bytes_delivered != streamed_bytes
            || stats_.cold.payload_bytes_scanned != streamed_bytes
            || stats_.cold.row_crc_bytes != streamed_bytes
            || stats_.cold.extent_crc_bytes != streamed_bytes
            || stats_.cold.extent_crc_raw_bytes_hashed != 0
            || stats_.cold.extent_crc_combined_bytes != streamed_bytes
            || stats_.cold.extent_crc_combine_rows != streamed_chunks
            || (streamed_chunks == 0
                ? cold_tid_transitions != 0
                : cold_tid_transitions != streamed_chunks - 1)
            || stats_.payload.repeated_chunk_reads != 0
            || stats_.index_scans != expected_index_scans
            || stats_.cache_lookups != 0 || stats_.cache_populations != 0
            || stats_.toast_values_read != 0 || stats_.detoast_bytes != 0
            || !catalog_source_authorized_
            || stats_.source_parent_rows_read != 1
            || stats_.source_catalog_rows_read != 1
            || stats_.source_catalog_index_scans != 1
            || stats_.source_fragment_rows_read != 0
            || stats_.source_decoded_bp != 0
            || stats_.source_storage_incarnations_checked < 1
            || stats_.source_storage_incarnations_checked > 2) {
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
        checked_add_(stats_.total_bytes_read, stats_.metadata_bytes_read,
            "full-generation total byte equation overflow");
        checked_add_(stats_.total_bytes_read, stats_.payload_bytes_read,
            "full-generation total byte equation overflow");
        stats_.cold.add(
            stats_.cold.toast_values, stats_.toast_values_read);
        stats_.cold.add(
            stats_.cold.detoast_bytes, stats_.detoast_bytes);
        finalize_payload_timing_();
        state_ = operation_state::finished;
        close_relations_checked_();
        return stats_;
    }

    void plan_section_groups_(
            payload_section section,
            std::span<const minimizer_generation_payload_read_group> groups) {
        try {
            const bool position_section = section == payload_section::positions;
            const operation_state required_state = position_section
                ? operation_state::controls_complete : operation_state::metadata_ready;
            if (state_ != required_state || !metadata_) {
                fail_(position_section
                    ? "position payload requests were planned before control payload completed or more than once"
                    : "control payload requests were planned before metadata or more than once");
            }
            if (!planned_groups_.empty() || next_group_ != 0 || plan_permit_
                || !runtime_requests_.empty() || runtime_permit_
                || active_scan_ != nullptr) {
                fail_("payload section planning found leaked prior-section state");
            }
            if (!position_section
                && (!authorized_position_ordinals_.empty()
                    || control_authorization_permit_)) {
                fail_("control payload authorization state was already populated");
            }
            std::uint64_t request_count = 0;
            std::uint64_t requested_rows = 0;
            std::uint64_t requested_bytes = 0;
            for (const minimizer_generation_payload_read_group& group : groups) {
                checked_add_(request_count, group.requests.size(),
                    "payload plan request count overflow");
                for (const index_sub_key_request& request : group.requests) {
                    checked_add_(requested_rows,
                        request.expected_chunk_count,
                        "payload requested-row telemetry overflow");
                    checked_add_(requested_bytes,
                        request.expected_total_bytes,
                        "payload requested-byte telemetry overflow");
                }
            }
            std::uint64_t plan_bytes = 0;
            if (groups.size()
                    > (std::numeric_limits<std::uint64_t>::max)()
                        / sizeof(planned_group)
                || request_count
                    > (std::numeric_limits<std::uint64_t>::max)()
                        / sizeof(index_sub_key_request)
                || (!position_section
                    && request_count
                        > (std::numeric_limits<std::uint64_t>::max)()
                            / sizeof(std::uint32_t))) {
                fail_("payload plan admission size overflow");
            }
            plan_bytes = groups.size() * sizeof(planned_group);
            checked_add_(plan_bytes,
                request_count * sizeof(index_sub_key_request),
                "payload plan admission size overflow");
            if (!position_section && request_count != 0) {
                const std::uint64_t authorization_bytes =
                    request_count * sizeof(std::uint32_t);
                auto authorization_permit =
                    bioseq::parallelism::memory_admission_controller::instance()
                        .try_reserve(authorization_bytes);
                if (!authorization_permit) {
                    fail_("control payload authorization memory admission was denied");
                }
                control_authorization_permit_ =
                    std::move(*authorization_permit);
                authorized_position_ordinals_.clear();
                authorized_position_ordinals_.reserve(
                    static_cast<std::size_t>(request_count));
            }
            if (plan_bytes != 0) {
                auto plan_permit =
                    bioseq::parallelism::memory_admission_controller::instance()
                        .try_reserve(plan_bytes);
                if (!plan_permit) {
                    fail_("payload plan memory admission was denied");
                }
                plan_permit_ = std::move(*plan_permit);
            }
            planned_groups_.clear();
            planned_groups_.reserve(groups.size());
            std::optional<index_sub_key_range> previous_range;
            const auto entries = metadata_->directory.entries();
            const std::uint32_t extent_payload_bytes =
                bioseq::indexing::minimizer_extent_payload_bytes(
                    metadata_->manifest.layout);
            if (extent_payload_bytes == 0) {
                fail_("full-generation wire quantum is invalid");
            }
            std::uint64_t authorization_checks = 0;
            for (const minimizer_generation_payload_read_group& input : groups) {
                const index_sub_key_range& range = input.range;
                const auto projected_range = project_extent_range_(range);
                if (range.generation_id != metadata_->manifest.generation_id
                    || range.position_segment < 0
                    || !projected_range
                    || projected_range->position_extent != position_section
                    || input.requests.empty()) {
                    fail_(position_section
                        ? "position payload group range is empty, unqualified, or outside the position namespace"
                        : "control payload group range is empty, unqualified, or outside the control namespace");
                }
                if (previous_range) {
                    const auto previous_key = std::tuple{
                        previous_range->generation_id,
                        previous_range->position_segment,
                        previous_range->begin};
                    const auto current_key = std::tuple{
                        range.generation_id,
                        range.position_segment,
                        range.begin};
                    if (!(previous_key < current_key)
                        || (previous_range->generation_id
                                == range.generation_id
                            && previous_range->position_segment
                                == range.position_segment
                            && range.begin < previous_range->end)) {
                        fail_("payload group ranges are not sorted and disjoint");
                    }
                }
                previous_range = range;
                using directory_key_type =
                    std::pair<std::int32_t, index_sub_key_t>;
                const auto entry_less_than_key = [](
                        const grouped_soa_bucket_directory_entry& entry,
                        const directory_key_type& key) noexcept {
                    return directory_key_type{
                        entry.position_segment, entry.sub_key} < key;
                };
                const auto first_entry = std::lower_bound(
                    entries.begin(), entries.end(),
                    directory_key_type{range.position_segment,
                        projected_range->logical_begin},
                    entry_less_than_key);
                const auto end_entry = std::lower_bound(
                    first_entry, entries.end(),
                    directory_key_type{range.position_segment,
                        projected_range->logical_end},
                    entry_less_than_key);
                const std::size_t first_ordinal = static_cast<std::size_t>(
                    first_entry - entries.begin());
                const std::size_t end_ordinal = static_cast<std::size_t>(
                    end_entry - entries.begin());
                if (first_ordinal == end_ordinal) {
                    fail_("payload group range covers no directory entry");
                }
                std::size_t request_entry_cursor = first_ordinal;
                std::uint64_t wanted_chunks = 0;
                std::optional<std::tuple<index_sub_key_t, std::uint32_t>>
                    previous_request;
                std::uint64_t previous_request_end = 0;
                for (const index_sub_key_request& request : input.requests) {
                    const auto request_key = std::tuple{
                        request.sub_key, request.first_chunk_no};
                    if (previous_request && !(*previous_request < request_key)) {
                        fail_("payload requests are not strictly sorted");
                    }
                    if (previous_request
                        && std::get<0>(*previous_request) == request.sub_key
                        && request.first_chunk_no < previous_request_end) {
                        fail_("payload request intervals overlap");
                    }
                    previous_request = request_key;
                    previous_request_end =
                        static_cast<std::uint64_t>(request.first_chunk_no)
                        + request.expected_chunk_count;
                    const auto logical_request_sub_key =
                        project_minimizer_extent_sub_key(
                            request.sub_key,
                            projected_range->position_extent);
                    if (request.generation_id != range.generation_id
                        || request.position_segment != range.position_segment
                        || request.sub_key < range.begin
                        || request.sub_key >= range.end
                        || !logical_request_sub_key
                        || request.expected_chunk_count == 0
                        || previous_request_end == 0
                        || previous_request_end - 1u
                            > static_cast<std::uint64_t>(
                                (std::numeric_limits<std::int32_t>::max)())) {
                        fail_("payload request is outside its qualified range");
                    }
                    while (request_entry_cursor < end_ordinal
                           && entries[request_entry_cursor].sub_key
                                < *logical_request_sub_key) {
                        ++request_entry_cursor;
                    }
                    if (request_entry_cursor == end_ordinal
                        || entries[request_entry_cursor].sub_key
                            != *logical_request_sub_key
                        || !request_matches_extent_(
                            request, entries[request_entry_cursor],
                            projected_range->position_extent)) {
                        fail_("payload request does not equal one directory extent");
                    }
                    const std::uint32_t directory_ordinal =
                        entries[request_entry_cursor].partition_id;
                    if (!position_section) {
                        if (!authorized_position_ordinals_.empty()
                            && authorized_position_ordinals_.back()
                                >= directory_ordinal) {
                            fail_("control payload directory authorizations are not strictly sorted");
                        }
                        authorized_position_ordinals_.push_back(
                            directory_ordinal);
                    } else {
                        if (!std::binary_search(
                                authorized_position_ordinals_.begin(),
                                authorized_position_ordinals_.end(),
                                directory_ordinal)) {
                            fail_("position payload request has no delivered control extent");
                        }
                        checked_add_(authorization_checks, 1,
                            "position control-authorization counter overflow");
                    }
                    checked_add_(wanted_chunks,
                        request.expected_chunk_count,
                        "payload wanted chunk count overflow");
                }
                std::uint64_t declared_chunks = 0;
                for (std::size_t ordinal = first_ordinal;
                     ordinal < end_ordinal; ++ordinal) {
                    checked_add_(declared_chunks,
                        projected_range->position_extent
                            ? entries[ordinal].position_chunk_count
                            : entries[ordinal].control_chunk_count,
                        "payload declared chunk count overflow");
                }
                if (declared_chunks < wanted_chunks) {
                    fail_("payload skip budget is smaller than requested chunks");
                }
                planned_groups_.push_back(planned_group{
                    .range = range,
                    .requests = input.requests,
                    .first_directory_ordinal = first_ordinal,
                    .end_directory_ordinal = end_ordinal,
                    .skip_budget_rows = declared_chunks - wanted_chunks,
                    .position_extent = projected_range->position_extent,
                });
            }
            auto& section_stats = position_section
                ? stats_.positions : stats_.controls;
            checked_add_(section_stats.plan_calls, 1,
                "payload section plan-call counter overflow");
            checked_add_(section_stats.planned_request_groups,
                planned_groups_.size(),
                "payload section planned-group counter overflow");
            checked_add_(section_stats.control_authorization_checks,
                authorization_checks,
                "position control-authorization counter overflow");
            checked_add_(stats_.bulk_read_calls, 1,
                "payload plan-call counter overflow");
            checked_add_(stats_.planned_request_groups,
                planned_groups_.size(),
                "payload planned-group counter overflow");
            if (!position_section) {
                if (planned_groups_.empty()) {
                    stats_.cold.route_empty = 1;
                } else {
                    stats_.cold.route_selective = 1;
                }
            }
            stats_.cold.add(
                stats_.cold.route_candidate_rows, requested_rows);
            stats_.cold.add(
                stats_.cold.route_candidate_bytes, requested_bytes);
            stats_.cold.add(
                stats_.cold.payload_rows_requested, requested_rows);
            stats_.cold.add(
                stats_.cold.payload_bytes_requested, requested_bytes);
            stats_.cold.add(
                stats_.cold.route_selective_cost, requested_rows);
            const std::uint64_t group_penalty =
                planned_groups_.size()
                    > cold_index_preparation_stats::k_sql_bigint_max
                        / k_ordered_range_scan_penalty_rows
                ? cold_index_preparation_stats::k_sql_bigint_max
                : static_cast<std::uint64_t>(planned_groups_.size())
                    * k_ordered_range_scan_penalty_rows;
            stats_.cold.add(
                stats_.cold.route_selective_cost, group_penalty);
            next_group_ = 0;
            if (position_section) {
                release_control_authorization_();
                state_ = planned_groups_.empty()
                    ? operation_state::payload_complete : operation_state::positions_planned;
            } else {
                state_ = planned_groups_.empty()
                    ? operation_state::controls_complete : operation_state::controls_planned;
            }
        } catch (...) {
            state_ = operation_state::failed;
            throw;
        }
    }

public:
    /// parallel_scan_stream the complete authorized generation through one ordered physical
    /// scan.  Payload bytes remain opaque and callback-scoped: this layer does
    /// not decode controls, globalize/rebase local positions, or retain a
    /// payload image.  A callback may copy into already-admitted host or mapped
    /// device storage, but command submission/fence waits must occur only after
    /// this method returns and `active_scan_` has been closed.
    template <class p_control_sink, class p_position_sink>
    requires (
        bioseq::indexing::index_chunk_reader_fn<p_control_sink>
        || std::invocable<p_control_sink&,
            const validated_minimizer_generation_row_receipt&>)
          && (
        bioseq::indexing::index_chunk_reader_fn<p_position_sink>
        || std::invocable<p_position_sink&,
            const validated_minimizer_generation_row_receipt&>)
    [[nodiscard]] index_chunk_read_stats read_full_generation_payload_once(
            p_control_sink&& control_sink,
            p_position_sink&& position_sink) {
        index_chunk_read_stats local{};
        const BufferUsage payload_buffer_begin = pgBufferUsage;
        start_payload_timing_();
        try {
            if (!minimizer_generation_full_crc_detail::concatenation_self_test()) {
                fail_("full-generation CRC concatenation self-test failed");
            }
            if (state_ != operation_state::metadata_ready || !metadata_
                || !catalog_source_authorized_
                || active_scan_ != nullptr
                || full_generation_batch_slot_count_ != 0
                || !planned_groups_.empty()
                || next_group_ != 0 || !runtime_requests_.empty()
                || plan_permit_ || runtime_permit_
                || control_authorization_permit_
                || !authorized_position_ordinals_.empty()) {
                checked_add_(stats_.payload.repeated_chunk_reads, 1,
                    "full-generation repeated-read counter overflow");
                fail_("full-generation payload scan was repeated, early, lacked catalog authorization, or found selected-range state");
            }
            const auto entries = metadata_->directory.entries();
            const std::uint32_t extent_payload_bytes =
                bioseq::indexing::minimizer_extent_payload_bytes(
                    metadata_->manifest.layout);
            if (extent_payload_bytes == 0) {
                fail_("full-generation wire quantum is invalid");
            }
            if (entries.size()
                    > (std::numeric_limits<std::uint64_t>::max)() / 2u) {
                fail_("full-generation extent count overflow");
            }
            const std::uint64_t planned_extents =
                static_cast<std::uint64_t>(entries.size()) * 2u;
            if (planned_extents != metadata_->manifest.extent_count) {
                fail_("full-generation directory extent equation disagrees");
            }

            // The primary key yields every global control sub-key followed by
            // every derived position sub-key. Grouped-SoA hashes that same
            // physical order, so the payload checksum can be streamed directly.
            std::uint64_t total_chunks = 0;
            std::uint64_t planned_control_chunks = 0;
            std::uint64_t planned_position_chunks = 0;
            std::uint64_t planned_control_bytes = 0;
            std::uint64_t planned_position_bytes = 0;
            for (const grouped_soa_bucket_directory_entry& entry : entries) {
                checked_add_(planned_control_chunks,
                    entry.control_chunk_count,
                    "full-generation control chunk count overflow");
                checked_add_(planned_position_chunks,
                    entry.position_chunk_count,
                    "full-generation position chunk count overflow");
                checked_add_(planned_control_bytes,
                    entry.control_encoded_bytes,
                    "full-generation control byte count overflow");
                checked_add_(planned_position_bytes,
                    entry.position_encoded_bytes,
                    "full-generation position byte count overflow");
            }
            checked_add_(total_chunks, planned_control_chunks,
                "full-generation total chunk count overflow");
            checked_add_(total_chunks, planned_position_chunks,
                "full-generation total chunk count overflow");
            local.index_scans = 1;
            local.requested_ranges = 1;
            local.wanted_subkeys = planned_extents;
            local.wanted_extents = planned_extents;
            local.wanted_chunks = total_chunks;
            stats_.cold.route_full = 1;
            stats_.cold.add(
                stats_.cold.payload_rows_requested, total_chunks);
            stats_.cold.add(
                stats_.cold.payload_bytes_requested,
                metadata_->manifest.payload_bytes);

            ScanKeyData keys[3];
            ScanKeyInit(&keys[0], k_minimizer_generation_index_generation_id,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(
                    metadata_->manifest.generation_id)));
            ScanKeyInit(&keys[1], k_minimizer_generation_index_query_key,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(query_key_)));
            ScanKeyInit(&keys[2], k_minimizer_generation_index_position_segment,
                BTGreaterEqualStrategyNumber, F_INT4GE, Int32GetDatum(0));
            active_scan_ = bioseq::postgres::support::invoke_pg_as_cpp(
                [this, &keys] {
                return systable_beginscan_ordered(
                    heap_relation_, primary_index_relation_, snapshot_, 3,
                    keys);
            });
            create_full_generation_batch_slots_checked_();
            stats_.cold.add(stats_.cold.payload_range_scans, 1);

            std::size_t entry_index = 0;
            std::uint32_t extent_chunk = 0;
            bool position_extent = false;
            bool payload_complete = entries.empty();
            crc64_ecma182_accumulator extent_checksum{};
            crc64_ecma182_accumulator payload_row_checksum{};

            // One bridge covers up to K fetch/deform/projection operations.
            // Each projection borrows its own pinned slot; row authority, CRC
            // and arbitrary sinks execute only after PG_END_TRY, so neither a
            // receipt nor sink-owned C++ state crosses a siglongjmp boundary.
            pinned_minimizer_generation_projected_row_batch batch{};
            std::size_t batch_ordinal = 0;
            for (;;) {
                if (batch_ordinal == batch.size) {
                    if (batch.eof) break;
                    batch = next_pinned_minimizer_generation_projected_row_batch_or_error(
                        heap_relation_, active_scan_,
                        full_generation_batch_slots_);
                    batch_ordinal = 0;
                    if (batch.size == 0) {
                        if (batch.eof) break;
                        fail_("full-generation batched payload scan made no progress");
                    }
                }
                const pinned_minimizer_generation_projected_row& pinned =
                    batch.rows[batch_ordinal++];
                record_payload_tid_(pinned.tid);
                loaded_row_type row =
                    load_minimizer_generation_projected_row_or_error(pinned.projected);
                stats_.cold.add(
                    stats_.cold.row_crc_bytes, row.bytes.size());
                stats_.cold.add(
                    stats_.cold.extent_crc_bytes, row.bytes.size());
                stats_.cold.add(stats_.cold.payload_rows_scanned, 1);
                stats_.cold.add(
                    stats_.cold.payload_bytes_scanned, row.bytes.size());
                checked_add_(local.heap_rows_read, 1,
                    "full-generation heap row count overflow");
                if (payload_complete || entry_index >= entries.size()) {
                    row.release();
                    fail_("full-generation payload scan returned extra rows");
                }
                const grouped_soa_bucket_directory_entry& entry = entries[entry_index];
                const auto position_sub_key =
                    derive_minimizer_position_extent_sub_key(entry.sub_key);
                if (!position_sub_key) {
                    row.release();
                    fail_("full-generation position sub-key is invalid");
                }
                const std::uint32_t expected_chunk_count = position_extent
                    ? entry.position_chunk_count : entry.control_chunk_count;
                const std::uint32_t first_chunk = position_extent
                    ? entry.position_first_chunk : entry.control_first_chunk;
                const std::uint64_t expected_extent_bytes = position_extent
                    ? entry.position_encoded_bytes
                    : entry.control_encoded_bytes;
                if (extent_chunk >= expected_chunk_count
                    || extent_chunk
                        > (std::numeric_limits<std::uint64_t>::max)()
                            / extent_payload_bytes) {
                    row.release();
                    fail_("full-generation extent chunk cursor overflow");
                }
                const std::uint64_t extent_offset =
                    static_cast<std::uint64_t>(extent_chunk)
                    * extent_payload_bytes;
                if (extent_offset >= expected_extent_bytes) {
                    row.release();
                    fail_("full-generation extent byte cursor overflow");
                }
                const std::uint64_t expected_bytes =
                    std::min<std::uint64_t>(
                        extent_payload_bytes,
                        expected_extent_bytes - extent_offset);
                const index_chunk_address expected_address{
                    .generation_id = metadata_->manifest.generation_id,
                    .position_segment = entry.position_segment,
                    .sub_key = position_extent
                        ? *position_sub_key : entry.sub_key,
                    .chunk_no = static_cast<std::uint32_t>(
                        first_chunk + extent_chunk),
                };
                if (row.query_key != query_key_
                    || row.address != expected_address
                    || row.bytes.size() != expected_bytes) {
                    row.release();
                    fail_("full-generation payload rows are missing, extra, duplicate, or reordered");
                }

                std::uint64_t validated_row_checksum = 0u;
                bool receipt_matches_row = false;
                const bool receipt_visited = row.validated_receipt.has_value()
                    && bioseq::indexing::
                        visit_validated_minimizer_generation_row(
                            *row.validated_receipt,
                            [&row, &receipt_matches_row,
                             &validated_row_checksum](
                                const index_chunk_address& receipt_address,
                                std::span<const std::byte> receipt_bytes,
                                std::uint64_t receipt_checksum) noexcept {
                                receipt_matches_row =
                                    receipt_address == row.address
                                    && receipt_bytes.data()
                                        == row.bytes.data()
                                    && receipt_bytes.size()
                                        == row.bytes.size();
                                validated_row_checksum = receipt_checksum;
                            });
                if (!receipt_visited || !receipt_matches_row) {
                    row.release();
                    fail_("full-generation row receipt lost validated authority");
                }

                extent_checksum.value = bioseq::indexing::
                    crc64_append_validated_index_chunk_payload(
                        extent_checksum.checksum(),
                        row.address,
                        row.chunk_bytes,
                        validated_row_checksum);
                stats_.cold.add(
                    stats_.cold.extent_crc_combined_bytes,
                    row.bytes.size());
                stats_.cold.add(stats_.cold.extent_crc_combine_rows, 1);
                payload_row_checksum.update_little_endian(
                    validated_row_checksum);
                if (position_extent) {
                    if constexpr (std::invocable<p_position_sink&,
                            const validated_minimizer_generation_row_receipt&>) {
                        std::invoke(
                            position_sink, *row.validated_receipt);
                    } else {
                        std::invoke(position_sink, row.address,
                            reinterpret_cast<const char*>(row.bytes.data()),
                            row.bytes.size());
                    }
                    checked_add_(stats_.full_generation.position_chunks, 1,
                        "full-generation position chunk counter overflow");
                    checked_add_(stats_.full_generation.position_bytes,
                        row.bytes.size(),
                        "full-generation position byte counter overflow");
                } else {
                    if constexpr (std::invocable<p_control_sink&,
                            const validated_minimizer_generation_row_receipt&>) {
                        std::invoke(
                            control_sink, *row.validated_receipt);
                    } else {
                        std::invoke(control_sink, row.address,
                            reinterpret_cast<const char*>(row.bytes.data()),
                            row.bytes.size());
                    }
                    checked_add_(stats_.full_generation.control_chunks, 1,
                        "full-generation control chunk counter overflow");
                    checked_add_(stats_.full_generation.control_bytes,
                        row.bytes.size(),
                        "full-generation control byte counter overflow");
                }
                checked_add_(local.delivered_chunks, 1,
                    "full-generation delivered chunk counter overflow");
                checked_add_(local.delivered_bytes, row.bytes.size(),
                    "full-generation delivered byte counter overflow");
                stats_.cold.add(
                    stats_.cold.payload_rows_delivered, 1);
                stats_.cold.add(
                    stats_.cold.payload_bytes_delivered,
                    row.bytes.size());
                row.release();

                ++extent_chunk;
                if (extent_chunk == expected_chunk_count) {
                    const std::uint64_t expected_extent_checksum =
                        position_extent
                        ? entry.position_checksum : entry.control_checksum;
                    if (extent_checksum.checksum()
                            != expected_extent_checksum) {
                        fail_("full-generation extent checksum disagrees");
                    }
                    checked_add_(stats_.full_generation.completed_extents, 1,
                        "full-generation completed extent counter overflow");
                    extent_chunk = 0;
                    extent_checksum = {};
                    ++entry_index;
                    if (entry_index == entries.size()) {
                        if (!position_extent) {
                            position_extent = true;
                            entry_index = 0;
                        } else {
                            payload_complete = true;
                        }
                    }
                }
            }
            drop_full_generation_batch_slots_checked_();
            bioseq::postgres::support::invoke_pg_as_cpp(
                [this] { systable_endscan_ordered(active_scan_); });
            active_scan_ = nullptr;
            if (!payload_complete || extent_chunk != 0
                || stats_.full_generation.completed_extents
                    != planned_extents
                || stats_.full_generation.control_chunks
                    != planned_control_chunks
                || stats_.full_generation.position_chunks
                    != planned_position_chunks
                || stats_.full_generation.control_bytes
                    != planned_control_bytes
                || stats_.full_generation.position_bytes
                    != planned_position_bytes) {
                fail_("full-generation payload scan ended before its complete directory plan");
            }
            stats_.full_generation.payload_row_checksum =
                payload_row_checksum.checksum();
            if (stats_.full_generation.payload_row_checksum
                    != metadata_->manifest.payload_checksum) {
                fail_("full-generation manifest payload checksum disagrees");
            }
            local.read_subkeys = local.wanted_subkeys;
            local.read_extents = local.wanted_extents;
            if (local.heap_rows_read != local.delivered_chunks
                || local.delivered_chunks != local.wanted_chunks
                || local.delivered_bytes
                    != metadata_->manifest.payload_bytes) {
                fail_("full-generation payload authorization equations disagree");
            }
            stats_.full_generation.scan_calls = 1;
            stats_.full_generation.planned_extents = planned_extents;
            stats_.planned_request_groups = 1;
            stats_.executed_request_groups = 1;
            stats_.bulk_read_calls = 1;
            stats_.range_scan_calls = 1;
            checked_add_(stats_.index_scans, 1,
                "full-generation index scan counter overflow");
            checked_add_read_stats_(stats_.payload, local);
            stats_.payload_heap_rows_read = local.heap_rows_read;
            stats_.payload_bytes_read = local.delivered_bytes;
            add_cold_index_buffer_usage(stats_.cold, payload_buffer_begin);
            state_ = operation_state::full_generation_payload_complete;
        } catch (...) {
            cleanup_failed_session_and_rethrow_();
        }
        return local;
    }

    void plan_control_groups(
            std::span<const minimizer_generation_payload_read_group> groups) {
        plan_section_groups_(payload_section::controls, groups);
    }

    void plan_position_groups(
            std::span<const minimizer_generation_payload_read_group> groups) {
        plan_section_groups_(payload_section::positions, groups);
    }

    template <class p_sink>
    requires bioseq::indexing::index_chunk_reader_fn<p_sink>
        || std::invocable<p_sink&,
            const validated_minimizer_generation_row_receipt&>
    [[nodiscard]] index_chunk_read_stats read_chunks_in_range(
            std::size_t group_index,
            index_chunk_cache_policy cache_policy,
            p_sink&& sink) {
        index_chunk_read_stats local{};
        const BufferUsage payload_buffer_begin = pgBufferUsage;
        start_payload_timing_();
        try {
            const bool position_section = state_ == operation_state::positions_planned;
            if ((state_ != operation_state::controls_planned && !position_section)
                || group_index != next_group_
                || group_index >= planned_groups_.size()) {
                checked_add_(stats_.payload.repeated_chunk_reads, 1,
                    "payload repeated-read counter overflow");
                fail_("payload group was repeated, reordered, or not planned");
            }
            if (cache_policy != index_chunk_cache_policy::bypass_one_shot) {
                fail_("authorized generation session requires bypass_one_shot");
            }
            planned_group& group = planned_groups_[group_index];
            if (group.position_extent != position_section
                || group.executed || active_scan_ != nullptr
                || !runtime_requests_.empty() || runtime_permit_) {
                checked_add_(stats_.payload.repeated_chunk_reads, 1,
                    "payload repeated-read counter overflow");
                fail_("payload group was executed more than once or session state leaked");
            }
            if (group.requests.size()
                > (std::numeric_limits<std::uint64_t>::max)()
                    / sizeof(runtime_request)) {
                fail_("payload runtime admission size overflow");
            }
            const std::uint64_t runtime_bytes =
                group.requests.size() * sizeof(runtime_request);
            auto runtime_permit =
                bioseq::parallelism::memory_admission_controller::instance()
                    .try_reserve(runtime_bytes);
            if (!runtime_permit) {
                fail_("payload runtime memory admission was denied");
            }
            runtime_permit_ = std::move(*runtime_permit);
            runtime_requests_.clear();
            runtime_requests_.reserve(group.requests.size());
            local.index_scans = 1;
            local.requested_ranges = 1;
            local.wanted_extents = group.requests.size();
            local.skip_budget_rows = group.skip_budget_rows;
            std::optional<index_sub_key_t> previous_subkey;
            for (const index_sub_key_request& request : group.requests) {
                runtime_requests_.push_back(runtime_request{
                    .request = request,
                    .end_chunk_no = static_cast<std::uint64_t>(
                        request.first_chunk_no) + request.expected_chunk_count,
                    .next_chunk_no = request.first_chunk_no,
                });
                if (!previous_subkey || *previous_subkey != request.sub_key) {
                    checked_add_(local.wanted_subkeys, 1,
                        "payload wanted sub-key count overflow");
                    previous_subkey = request.sub_key;
                }
                checked_add_(local.wanted_chunks,
                    request.expected_chunk_count,
                    "payload local wanted chunk count overflow");
            }

            ScanKeyData keys[5];
            ScanKeyInit(&keys[0], k_minimizer_generation_index_generation_id,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(
                    group.range.generation_id)));
            ScanKeyInit(&keys[1], k_minimizer_generation_index_query_key,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(query_key_)));
            ScanKeyInit(&keys[2], k_minimizer_generation_index_sub_key,
                BTGreaterEqualStrategyNumber, F_INT8GE,
                Int64GetDatum(static_cast<std::int64_t>(group.range.begin)));
            ScanKeyInit(&keys[3], k_minimizer_generation_index_sub_key,
                BTLessStrategyNumber, F_INT8LT,
                Int64GetDatum(static_cast<std::int64_t>(group.range.end)));
            ScanKeyInit(&keys[4], k_minimizer_generation_index_position_segment,
                BTEqualStrategyNumber, F_INT4EQ,
                Int32GetDatum(group.range.position_segment));
            active_scan_ = bioseq::postgres::support::invoke_pg_as_cpp(
                [this, &keys] {
                return systable_beginscan_ordered(
                    heap_relation_, primary_index_relation_, snapshot_, 5,
                    keys);
            });
            stats_.cold.add(stats_.cold.payload_range_scans, 1);

            const auto entries = metadata_->directory.entries();
            std::size_t directory_ordinal = group.first_directory_ordinal;
            std::uint64_t expected_chunk_no = 0;
            std::size_t request_cursor = 0;
            std::uint64_t physical_payload_bytes = 0;
            for (HeapTuple tuple;
                 (tuple = bioseq::postgres::support::invoke_pg_as_cpp([this] {
                      return systable_getnext_ordered(
                          active_scan_, ForwardScanDirection);
                  })) != nullptr;) {
                record_payload_tid_(tuple);
                loaded_row_type row =
                    load_minimizer_generation_row_or_error(heap_relation_, tuple);
                stats_.cold.add(
                    stats_.cold.row_crc_bytes, row.bytes.size());
                stats_.cold.add(stats_.cold.payload_rows_scanned, 1);
                stats_.cold.add(
                    stats_.cold.payload_bytes_scanned, row.bytes.size());
                checked_add_(local.heap_rows_read, 1,
                    "payload heap row count overflow");
                checked_add_(physical_payload_bytes, row.bytes.size(),
                    "payload physical byte count overflow");
                if (row.query_key != query_key_
                    || row.address.generation_id
                        != group.range.generation_id
                    || row.address.position_segment
                        != group.range.position_segment
                    || row.address.sub_key < group.range.begin
                    || row.address.sub_key >= group.range.end) {
                    row.release();
                    fail_("payload scan returned a row outside its qualified range");
                }

                if (directory_ordinal >= group.end_directory_ordinal) {
                    row.release();
                    fail_("payload scan returned more rows than its directory range");
                }
                const grouped_soa_bucket_directory_entry& entry =
                    entries[directory_ordinal];
                const auto position_sub_key =
                    derive_minimizer_position_extent_sub_key(entry.sub_key);
                if (!position_sub_key) {
                    row.release();
                    fail_("payload directory position sub-key is invalid");
                }
                const index_sub_key_t expected_sub_key =
                    group.position_extent
                    ? *position_sub_key : entry.sub_key;
                const std::uint64_t entry_chunk_count = group.position_extent
                    ? entry.position_chunk_count : entry.control_chunk_count;
                if (row.address.sub_key != expected_sub_key
                    || row.address.chunk_no != expected_chunk_no
                    || expected_chunk_no >= entry_chunk_count) {
                    row.release();
                    fail_("payload physical rows do not exactly equal directory order");
                }

                while (request_cursor < runtime_requests_.size()) {
                    runtime_request& request =
                        runtime_requests_[request_cursor];
                    const bool row_after_request =
                        row.address.sub_key > request.request.sub_key
                        || (row.address.sub_key == request.request.sub_key
                            && static_cast<std::uint64_t>(
                                row.address.chunk_no) >= request.end_chunk_no);
                    if (!row_after_request) {
                        break;
                    }
                    if (request.next_chunk_no != request.end_chunk_no
                        || request.total_bytes
                            != request.request.expected_total_bytes
                        || request.checksum.checksum()
                            != request.request.expected_checksum) {
                        row.release();
                        fail_("payload extent aggregate count/bytes/checksum disagrees");
                    }
                    ++request_cursor;
                }

                runtime_request* matched = nullptr;
                if (request_cursor < runtime_requests_.size()) {
                    runtime_request& request =
                        runtime_requests_[request_cursor];
                    if (row.address.sub_key == request.request.sub_key
                        && row.address.chunk_no
                            >= request.request.first_chunk_no
                        && static_cast<std::uint64_t>(row.address.chunk_no)
                            < request.end_chunk_no) {
                        matched = &request;
                    }
                }
                if (matched == nullptr) {
                    checked_add_(local.skipped_rows, 1,
                        "payload skipped row count overflow");
                    stats_.cold.add(stats_.cold.payload_rows_skipped, 1);
                } else {
                    if (matched->next_chunk_no != row.address.chunk_no) {
                        row.release();
                        fail_("payload extent is missing, duplicate, or non-contiguous");
                    }
                    checked_add_(matched->total_bytes, row.bytes.size(),
                        "payload extent byte count overflow");
                    std::uint64_t validated_row_checksum = 0u;
                    bool receipt_matches_row = false;
                    const bool receipt_visited = row.validated_receipt.has_value()
                        && bioseq::indexing::
                            visit_validated_minimizer_generation_row(
                                *row.validated_receipt,
                                [&row, &receipt_matches_row,
                                 &validated_row_checksum](
                                    const index_chunk_address& receipt_address,
                                    std::span<const std::byte> receipt_bytes,
                                    std::uint64_t receipt_checksum) noexcept {
                                    receipt_matches_row =
                                        receipt_address == row.address
                                        && receipt_bytes.data()
                                            == row.bytes.data()
                                        && receipt_bytes.size()
                                            == row.bytes.size();
                                    validated_row_checksum = receipt_checksum;
                                });
                    if (!receipt_visited || !receipt_matches_row) {
                        row.release();
                        fail_("payload row receipt lost validated authority");
                    }
                    matched->checksum.value = bioseq::indexing::
                        crc64_append_validated_index_chunk_payload(
                            matched->checksum.checksum(),
                            row.address,
                            row.chunk_bytes,
                            validated_row_checksum);
                    stats_.cold.add(
                        stats_.cold.extent_crc_bytes,
                        row.bytes.size());
                    stats_.cold.add(
                        stats_.cold.extent_crc_combined_bytes,
                        row.bytes.size());
                    stats_.cold.add(
                        stats_.cold.extent_crc_combine_rows, 1);
                    if constexpr (std::invocable<p_sink&,
                            const validated_minimizer_generation_row_receipt&>) {
                        std::invoke(sink, *row.validated_receipt);
                    } else {
                        std::invoke(sink, row.address,
                            reinterpret_cast<const char*>(row.bytes.data()),
                            row.bytes.size());
                    }
                    ++matched->next_chunk_no;
                    checked_add_(local.delivered_chunks, 1,
                        "payload delivered chunk count overflow");
                    checked_add_(local.delivered_bytes, row.bytes.size(),
                        "payload delivered byte count overflow");
                    stats_.cold.add(
                        stats_.cold.payload_rows_delivered, 1);
                    stats_.cold.add(
                        stats_.cold.payload_bytes_delivered,
                        row.bytes.size());
                }
                row.release();
                ++expected_chunk_no;
                if (expected_chunk_no == entry_chunk_count) {
                    ++directory_ordinal;
                    expected_chunk_no = 0;
                }
            }
            bioseq::postgres::support::invoke_pg_as_cpp(
                [this] { systable_endscan_ordered(active_scan_); });
            active_scan_ = nullptr;

            if (directory_ordinal != group.end_directory_ordinal
                || expected_chunk_no != 0) {
                fail_("payload scan ended before its complete directory range");
            }
            for (; request_cursor < runtime_requests_.size(); ++request_cursor) {
                const runtime_request& request =
                    runtime_requests_[request_cursor];
                if (request.next_chunk_no != request.end_chunk_no
                    || request.total_bytes
                        != request.request.expected_total_bytes
                    || request.checksum.checksum()
                        != request.request.expected_checksum) {
                    fail_("payload extent aggregate count/bytes/checksum disagrees");
                }
            }
            local.read_subkeys = local.wanted_subkeys;
            local.read_extents = local.wanted_extents;
            if (local.delivered_chunks != local.wanted_chunks
                || local.heap_rows_read
                    != local.delivered_chunks + local.skipped_rows
                || local.skipped_rows != local.skip_budget_rows) {
                fail_("payload authorization-counter equation disagrees");
            }

            group.executed = true;
            ++next_group_;
            const bool section_complete = next_group_ == planned_groups_.size();
            auto& section_stats = position_section
                ? stats_.positions : stats_.controls;
            checked_add_(section_stats.executed_request_groups, 1,
                "payload section executed-group counter overflow");
            checked_add_(section_stats.range_scan_calls, 1,
                "payload section range-scan counter overflow");
            checked_add_(section_stats.physical_heap_rows_read,
                local.heap_rows_read,
                "payload section physical-row counter overflow");
            checked_add_(section_stats.physical_bytes_read,
                physical_payload_bytes,
                "payload section physical-byte counter overflow");
            checked_add_read_stats_(section_stats.read, local);
            checked_add_(stats_.executed_request_groups, 1,
                "executed request group count overflow");
            checked_add_(stats_.range_scan_calls, 1,
                "payload range scan count overflow");
            checked_add_(stats_.index_scans, 1,
                "total index scan count overflow");
            checked_add_read_stats_(stats_.payload, local);
            stats_.payload_heap_rows_read = stats_.payload.heap_rows_read;
            checked_add_(stats_.payload_bytes_read, physical_payload_bytes,
                "payload physical byte count overflow");
            std::vector<runtime_request>{}.swap(runtime_requests_);
            runtime_permit_.release();
            if (section_complete) {
                release_section_plan_();
                state_ = position_section
                    ? operation_state::payload_complete : operation_state::controls_complete;
            }
            add_cold_index_buffer_usage(stats_.cold, payload_buffer_begin);
        } catch (const bioseq::postgres::support::postgres_error&) {
            state_ = operation_state::failed;
            abandon_relations_noexcept_();
            throw;
        } catch (...) {
            state_ = operation_state::failed;
            throw;
        }
        return local;
    }

    /// Checked success close for a publication verifier that intentionally
    /// rereads only the active head. Borrowed snapshots remain caller-owned.
    void close_head_read() {
        try {
            if (state_ != operation_state::head_read || active_scan_ != nullptr
                || heap_relation_ == nullptr
                || primary_index_relation_ == nullptr
                || snapshot_ == nullptr) {
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

    /// Finish a grouped-SoA read whose immutable payload was consumed from the
    /// exact generation-bound mmap image. PostgreSQL remains the authority for
    /// the active head, manifest, directory and source authorization; the image
    /// itself was created only by the complete authenticated READY reread and
    /// was fsynced and durably renamed before publication.
    [[nodiscard]] minimizer_generation_read_stats
    finish_mapped_minimizer_fragment_payload() {
        try {
            if (state_ != operation_state::metadata_ready
                || !catalog_source_authorized_
                || !head_ || !metadata_
                || metadata_->manifest.layout
                    != bioseq::indexing::minimizer_generation_layout::
                        grouped_soa_segment_local
                || active_scan_ != nullptr || heap_relation_ == nullptr
                || primary_index_relation_ == nullptr || snapshot_ == nullptr
                || storage_descriptor_relation_ == nullptr
                || storage_descriptor_live_index_relation_ == nullptr
                || next_group_ != 0 || !planned_groups_.empty()
                || !runtime_requests_.empty() || plan_permit_
                || runtime_permit_ || control_authorization_permit_
                || !authorized_position_ordinals_.empty()
                || stats_.payload_heap_rows_read != 0
                || stats_.payload_bytes_read != 0
                || stats_.bulk_read_calls != 0
                || stats_.range_scan_calls != 0
                || stats_.full_generation.scan_calls != 0) {
                fail_(
                    "mapped grouped-SoA generation read closed in an invalid state");
            }
            const std::uint64_t expected_metadata_rows =
                stats_.head_rows_read + stats_.manifest_rows_read
                + stats_.directory_chunks_read;
            const std::uint64_t expected_metadata_bytes =
                stats_.head_bytes_read + stats_.manifest_bytes_read
                + stats_.directory_bytes_read;
            if (stats_.relation_opens != 1
                || stats_.head_scans != 1
                || stats_.manifest_directory_scans != 1
                || stats_.index_scans != 2
                || stats_.metadata_heap_rows_read != expected_metadata_rows
                || stats_.metadata_bytes_read != expected_metadata_bytes
                || stats_.source_parent_rows_read != 1
                || stats_.source_catalog_rows_read != 1
                || stats_.source_catalog_index_scans != 1
                || stats_.source_fragment_rows_read != 0
                || stats_.source_decoded_bp != 0
                || stats_.source_storage_incarnations_checked < 1
                || stats_.source_storage_incarnations_checked > 2) {
                fail_("mapped grouped-SoA metadata/source authorization disagrees");
            }
            stats_.total_heap_rows_read = stats_.metadata_heap_rows_read;
            stats_.total_bytes_read = stats_.metadata_bytes_read;
            stats_.cold.add(
                stats_.cold.toast_values, stats_.toast_values_read);
            stats_.cold.add(
                stats_.cold.detoast_bytes, stats_.detoast_bytes);
            state_ = operation_state::finished;
            close_relations_checked_();
            return stats_;
        } catch (...) {
            state_ = operation_state::failed;
            throw;
        }
    }

    /// Checked success close for the timed compact-H32 mmap route. PostgreSQL
    /// authenticates exactly one head row and one manifest row; the exact
    /// chunk_no=0 scan qualification ensures that no directory tuple was read.
    /// Any control/payload planning or scan stats make this transition
    /// invalid rather than silently falling back to the relation reader.
    [[nodiscard]] minimizer_generation_read_stats
    finish_mapped_compact_direct() {
        try {
            const minimizer_generation_manifest* const manifest =
                active_manifest_();
            const bool cold_payload_stats_are_zero =
                stats_.cold.payload_range_scans == 0u
                && stats_.cold.route_empty == 0u
                && stats_.cold.route_selective == 0u
                && stats_.cold.route_full == 0u
                && stats_.cold.route_candidate_rows == 0u
                && stats_.cold.route_full_rows == 0u
                && stats_.cold.route_candidate_bytes == 0u
                && stats_.cold.route_full_bytes == 0u
                && stats_.cold.route_selective_cost == 0u
                && stats_.cold.route_full_cost == 0u
                && stats_.cold.memory_reroutes == 0u
                && stats_.cold.payload_bytes_requested == 0u
                && stats_.cold.payload_bytes_delivered == 0u
                && stats_.cold.payload_rows_requested == 0u
                && stats_.cold.payload_rows_delivered == 0u
                && stats_.cold.payload_rows_scanned == 0u
                && stats_.cold.payload_bytes_scanned == 0u
                && stats_.cold.payload_rows_skipped == 0u
                && stats_.cold.heap_tid_adjacent == 0u
                && stats_.cold.heap_tid_forward_gap == 0u
                && stats_.cold.heap_tid_backward_jump == 0u
                && stats_.cold.extent_crc_bytes == 0u
                && stats_.cold.extent_crc_raw_bytes_hashed == 0u
                && stats_.cold.extent_crc_combined_bytes == 0u
                && stats_.cold.extent_crc_combine_rows == 0u
                && stats_.cold.payload_ns == 0u
                && stats_.cold.decode_ns == 0u
                && stats_.cold.transfer_ns == 0u;
            if (state_ != operation_state::metadata_ready
                || !catalog_source_authorized_ || !head_
                || manifest == nullptr || !manifest_only_ || metadata_
                || manifest != &*manifest_only_
                || manifest->layout
                    != bioseq::indexing::minimizer_generation_layout::
                        grouped_soa_segment_local
                || active_scan_ != nullptr || heap_relation_ == nullptr
                || primary_index_relation_ == nullptr || snapshot_ == nullptr
                || storage_descriptor_relation_ == nullptr
                || storage_descriptor_live_index_relation_ == nullptr
                || next_group_ != 0u || !planned_groups_.empty()
                || !runtime_requests_.empty() || plan_permit_
                || runtime_permit_ || control_authorization_permit_
                || metadata_permit_
                || !authorized_position_ordinals_.empty()
                || !directory_bytes_scratch_.empty()
                || previous_payload_heap_block_ || payload_stage_begin_ns_
                || stats_.payload_heap_rows_read != 0u
                || stats_.payload_bytes_read != 0u
                || stats_.bulk_read_calls != 0u
                || stats_.planned_request_groups != 0u
                || stats_.executed_request_groups != 0u
                || stats_.range_scan_calls != 0u
                || stats_.cache_lookups != 0u
                || stats_.cache_populations != 0u
                || !read_stats_are_zero_(stats_.payload)
                || !section_stats_are_zero_(stats_.controls)
                || !section_stats_are_zero_(stats_.positions)
                || !full_payload_stats_are_zero_(stats_.full_generation)
                || !cold_payload_stats_are_zero) {
                fail_(
                    "mapped compact-H32 generation read closed in an invalid state");
            }

            constexpr std::uint64_t k_expected_manifest_bytes =
                minimizer_generation_manifest::encoded_bytes;
            constexpr std::uint64_t k_expected_metadata_bytes =
                minimizer_generation_head::encoded_bytes
                + k_expected_manifest_bytes;
            if (stats_.relation_opens != 1u
                || stats_.head_scans != 1u
                || stats_.cold.head_scans != 1u
                || stats_.head_rows_read != 1u
                || stats_.head_bytes_read
                    != minimizer_generation_head::encoded_bytes
                || stats_.manifest_directory_scans != 1u
                || stats_.cold.directory_scans != 1u
                || stats_.manifest_rows_read != 1u
                || stats_.manifest_bytes_read != k_expected_manifest_bytes
                || stats_.directory_chunks_read != 0u
                || stats_.directory_bytes_read != 0u
                || stats_.metadata_repeated_reads != 0u
                || stats_.metadata_heap_rows_read != 2u
                || stats_.metadata_bytes_read != k_expected_metadata_bytes
                || stats_.index_scans != 2u
                || stats_.explicit_copy_bytes != k_expected_metadata_bytes
                || stats_.toast_values_read != 0u
                || stats_.detoast_bytes != 0u
                || stats_.total_heap_rows_read != 0u
                || stats_.total_bytes_read != 0u
                || stats_.source_parent_rows_read != 1u
                || stats_.source_catalog_rows_read != 1u
                || stats_.source_catalog_index_scans != 1u
                || stats_.source_fragment_rows_read != 0u
                || stats_.source_decoded_bp != 0u
                || stats_.source_storage_incarnations_checked < 1u
                || stats_.source_storage_incarnations_checked > 2u) {
                fail_(
                    "mapped compact-H32 metadata/source validation disagrees");
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

    /// Checked success close for callers that intentionally validate only the
    /// immutable head/manifest/directory contract and do not enter payload
    /// sections. Borrowed snapshots remain caller-owned.
    void close_metadata_read() {
        try {
            if ((state_ != operation_state::metadata_pending_source
                    && state_ != operation_state::metadata_ready)
                || active_manifest_() == nullptr
                || active_scan_ != nullptr || heap_relation_ == nullptr
                || primary_index_relation_ == nullptr
                || snapshot_ == nullptr) {
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

    [[nodiscard]] minimizer_generation_read_stats finish() {
        minimizer_generation_read_stats result{};
        try {
            if (state_ == operation_state::full_generation_payload_complete) {
                return finish_full_generation_();
            }
            if (state_ != operation_state::payload_complete
                || next_group_ != 0 || !planned_groups_.empty()
                || !runtime_requests_.empty() || plan_permit_
                || runtime_permit_ || control_authorization_permit_
                || !authorized_position_ordinals_.empty()
                || active_scan_ != nullptr || heap_relation_ == nullptr
                || primary_index_relation_ == nullptr || snapshot_ == nullptr) {
                fail_("generation read finished before position payload completed or with leaked section state");
            }
            std::uint64_t expected_metadata_rows = 0;
            checked_add_(expected_metadata_rows, stats_.head_rows_read,
                "metadata authorization row equation overflow");
            checked_add_(expected_metadata_rows, stats_.manifest_rows_read,
                "metadata authorization row equation overflow");
            checked_add_(expected_metadata_rows,
                stats_.directory_chunks_read,
                "metadata authorization row equation overflow");
            std::uint64_t expected_metadata_bytes = 0;
            checked_add_(expected_metadata_bytes, stats_.head_bytes_read,
                "metadata authorization byte equation overflow");
            checked_add_(expected_metadata_bytes, stats_.manifest_bytes_read,
                "metadata authorization byte equation overflow");
            checked_add_(expected_metadata_bytes, stats_.directory_bytes_read,
                "metadata authorization byte equation overflow");
            std::uint64_t expected_index_scans = 0;
            checked_add_(expected_index_scans, stats_.head_scans,
                "index scan authorization equation overflow");
            checked_add_(expected_index_scans,
                stats_.manifest_directory_scans,
                "index scan authorization equation overflow");
            checked_add_(expected_index_scans, stats_.range_scan_calls,
                "index scan authorization equation overflow");
            std::uint64_t expected_payload_rows = 0;
            checked_add_(expected_payload_rows,
                stats_.payload.delivered_chunks,
                "payload authorization row equation overflow");
            checked_add_(expected_payload_rows, stats_.payload.skipped_rows,
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
            if (stats_.relation_opens != 1
                || stats_.head_scans != 1
                || stats_.head_rows_read != 1
                || stats_.head_bytes_read
                    != minimizer_generation_head::encoded_bytes
                || stats_.manifest_directory_scans != 1
                || stats_.manifest_rows_read != 1
                || stats_.manifest_bytes_read
                    != bioseq::indexing::minimizer_manifest_encoded_bytes(
                        metadata_->manifest.layout)
                || stats_.metadata_heap_rows_read != expected_metadata_rows
                || stats_.metadata_bytes_read != expected_metadata_bytes
                || !section_equations_hold_(stats_.controls)
                || !section_equations_hold_(stats_.positions)
                || stats_.controls.control_authorization_checks != 0
                || stats_.positions.control_authorization_checks
                    != stats_.positions.read.wanted_extents
                || stats_.positions.read.wanted_extents
                    > stats_.controls.read.wanted_extents
                || !counter_is_sum_(stats_.planned_request_groups,
                    stats_.controls.planned_request_groups,
                    stats_.positions.planned_request_groups)
                || !counter_is_sum_(stats_.executed_request_groups,
                    stats_.controls.executed_request_groups,
                    stats_.positions.executed_request_groups)
                || !counter_is_sum_(stats_.range_scan_calls,
                    stats_.controls.range_scan_calls,
                    stats_.positions.range_scan_calls)
                || !counter_is_sum_(stats_.payload_heap_rows_read,
                    stats_.controls.physical_heap_rows_read,
                    stats_.positions.physical_heap_rows_read)
                || !counter_is_sum_(stats_.payload_bytes_read,
                    stats_.controls.physical_bytes_read,
                    stats_.positions.physical_bytes_read)
                || !read_stats_are_sum_(stats_.payload,
                    stats_.controls.read, stats_.positions.read)
                || stats_.planned_request_groups
                    != stats_.executed_request_groups
                || stats_.bulk_read_calls != 2
                || stats_.range_scan_calls
                    != stats_.executed_request_groups
                || stats_.payload.index_scans
                    != stats_.executed_request_groups
                || stats_.payload.requested_ranges
                    != stats_.executed_request_groups
                || stats_.index_scans != expected_index_scans
                || stats_.payload.heap_rows_read != expected_payload_rows
                || stats_.payload_heap_rows_read
                    != stats_.payload.heap_rows_read
                || stats_.payload_bytes_read
                    < stats_.payload.delivered_bytes
                || stats_.payload.skipped_rows
                    != stats_.payload.skip_budget_rows
                || stats_.cold.route_full != 0
                || !((stats_.cold.route_empty == 1
                          && stats_.cold.route_selective == 0)
                     || (stats_.cold.route_empty == 0
                         && stats_.cold.route_selective == 1))
                || stats_.cold.payload_rows_requested
                    != stats_.payload.delivered_chunks
                || stats_.cold.payload_rows_delivered
                    != stats_.payload.delivered_chunks
                || stats_.cold.payload_rows_scanned
                    != stats_.payload.heap_rows_read
                || stats_.cold.payload_rows_skipped
                    != stats_.payload.skipped_rows
                || stats_.cold.payload_bytes_requested
                    != stats_.payload.delivered_bytes
                || stats_.cold.payload_bytes_delivered
                    != stats_.payload.delivered_bytes
                || stats_.cold.payload_bytes_scanned
                    != stats_.payload_bytes_read
                || stats_.cold.row_crc_bytes != stats_.payload_bytes_read
                || stats_.cold.extent_crc_bytes
                    != stats_.payload.delivered_bytes
                || stats_.cold.extent_crc_raw_bytes_hashed != 0
                || stats_.cold.extent_crc_combined_bytes
                    != stats_.payload.delivered_bytes
                || stats_.cold.extent_crc_combine_rows
                    != stats_.payload.delivered_chunks
                || (stats_.payload.heap_rows_read == 0
                    ? cold_tid_transitions != 0
                    : cold_tid_transitions
                        != stats_.payload.heap_rows_read - 1)
                || stats_.payload.wanted_subkeys
                    != stats_.payload.read_subkeys
                || stats_.payload.wanted_extents
                    != stats_.payload.read_extents
                || stats_.payload.wanted_chunks
                    != stats_.payload.delivered_chunks
                || stats_.cache_lookups != 0
                || stats_.cache_populations != 0
                || stats_.toast_values_read != 0
                || stats_.detoast_bytes != 0
                || (stats_.source_catalog_rows_read != 0
                    && (stats_.source_parent_rows_read != 1
                        || stats_.source_catalog_rows_read != 1
                        || stats_.source_catalog_index_scans != 1
                        || stats_.source_fragment_rows_read != 0
                        || stats_.source_decoded_bp != 0
                        || stats_.source_storage_incarnations_checked < 1
                        || stats_.source_storage_incarnations_checked > 2))) {
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
            stats_.cold.add(
                stats_.cold.toast_values, stats_.toast_values_read);
            stats_.cold.add(
                stats_.cold.detoast_bytes, stats_.detoast_bytes);
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
};

static_assert(!std::is_default_constructible_v<
    index_storage_fragment_generation_read_session>);
static_assert(!std::is_copy_constructible_v<
    index_storage_fragment_generation_read_session>);
static_assert(std::is_nothrow_move_constructible_v<
    index_storage_fragment_generation_read_session>);
static_assert(!std::is_move_assignable_v<
    index_storage_fragment_generation_read_session>);

/// Normal open outcome with either an owned session or a classified relation failure.
struct minimizer_generation_read_session_open_result {
    std::optional<index_storage_fragment_generation_read_session> session{};
    minimizer_generation_relation_inspection failure{
        .kind = minimizer_generation_relation_kind::malformed,
        .schema_error = minimizer_generation_schema_error::none,
    };

    [[nodiscard]] explicit operator bool() const noexcept {
        return session.has_value();
    }
};

/// Bind classification and reading to one heap descriptor and its primary
/// index. Unrelated secondary indexes are deliberately neither opened nor
/// counted.
/// A successful result owns both AccessShareLock references until finish(); an
/// unsuccessful normal result owns neither.  PostgreSQL ERROR remains an
/// exception so the transaction ResourceOwner retains its ordinary authority.
[[nodiscard]] inline minimizer_generation_read_session_open_result
open_exact_minimizer_generation_read_session_pg(
        std::string_view table_name,
        std::string_view index_name,
        std::uint64_t query_key,
        const borrowed_executor_read_context& executor_read,
        expected_minimizer_generation_identity expected) {
    if (!executor_read.valid()) {
        minimizer_generation_storage_error_as_cpp(
            "generation read requires a valid borrowed executor context");
    }
    minimizer_generation_read_session_open_result result{};
    try {
        result.session.emplace(
            table_name, query_key, executor_read.snapshot(), expected,
            executor_read.tablespace_oid(),
            minimizer_generation_snapshot_registration::borrow_registered, index_name);
    } catch (const minimizer_generation_relation_open_failure& failure) {
        result.failure = failure.inspection;
    }
    return result;
}

namespace minimizer_generation_compile_contract_detail {
/// Minimal sink used to instantiate and validate the exported range-read contract.
struct qualified_read_sink {
    void operator()(
        const index_chunk_address&, const char*, std::size_t) const noexcept {}
};
}  // namespace minimizer_generation_compile_contract_detail

template index_chunk_read_stats
index_storage_fragment_generation_read_session::read_chunks_in_range<
    minimizer_generation_compile_contract_detail::qualified_read_sink>(
        std::size_t,
        index_chunk_cache_policy,
        minimizer_generation_compile_contract_detail::qualified_read_sink&&);

/// Integration-only self-consistent metadata corruption. Both manifest and
/// head source-content fields, the head's manifest checksum, and both physical
/// row CRCs are rewritten together. Only an independent source-row decode can
/// detect the corruption.
inline void rewrite_source_checksum_pg(
        Oid table_oid,
        std::uint64_t query_key,
        std::uint64_t xor_mask) {
    if (!bioseq::postgres::support::invoke_pg_as_cpp([] { return superuser(); })
        || xor_mask == 0
        || !bioseq::indexing::sql_bigint_can_represent(query_key)) {
        minimizer_generation_storage_error_as_cpp(
            "current generation checksum corruption input is invalid");
    }
    Relation relation = bioseq::postgres::support::invoke_pg_as_cpp(
        [table_oid] {
            return bioseq::postgres::support::try_open_postgres_relation(table_oid, AccessShareLock);
        });
    if (relation == nullptr
        || RelationGetNamespace(relation) != bioseq_internal_ns()) {
        if (relation != nullptr) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [relation] { relation_close(relation, AccessShareLock); });
        }
        minimizer_generation_storage_error_as_cpp(
            "current generation checksum corruption relation is invalid");
    }
    const minimizer_generation_schema_inspection schema =
        bioseq::postgres::support::invoke_pg_as_cpp(
            [relation] { return inspect_minimizer_generation_schema(relation); });
    const std::string table_name{RelationGetRelationName(relation)};
    const Oid table_tablespace_oid = relation->rd_locator.spcOid;
    bioseq::postgres::support::invoke_pg_as_cpp(
        [relation] { relation_close(relation, AccessShareLock); });
    if (!schema) {
        minimizer_generation_storage_error_as_cpp(
            "current generation checksum corruption schema is invalid");
    }

    Snapshot active_snapshot = bioseq::postgres::support::invoke_pg_as_cpp(
        [] { return GetActiveSnapshot(); });
    if (active_snapshot == nullptr || !IsMVCCSnapshot(active_snapshot)) {
        minimizer_generation_storage_error_as_cpp(
            "current generation checksum corruption requires an active MVCC snapshot");
    }
    Snapshot registered_snapshot = bioseq::postgres::support::invoke_pg_as_cpp(
        [active_snapshot] { return RegisterSnapshot(active_snapshot); });
    minimizer_generation_head head{};
    minimizer_generation_manifest manifest{};
    {
        constexpr auto k_fixture_scheme =
            bioseq::indexing::minimizer_scheme_identity_for_backend(
                bioseq::indexing::minimizer_sketch_backend::scalar);
        constexpr expected_minimizer_generation_identity k_fixture_expected{
            .layout = bioseq::indexing::minimizer_generation_layout::
                grouped_soa_segment_local,
            .alphabet_code =
                bioseq::indexing::k_minimizer_persisted_dna4_alphabet_code,
            .storage_unit_byte_width = 8,
            .kmer_size = 15,
            .window_size = 10,
            .gpu_abi_revision =
                bioseq::indexing::k_grouped_soa_gpu_abi_revision,
            .minimizer_scheme_id = k_fixture_scheme.id,
            .minimizer_scheme_revision = k_fixture_scheme.revision,
            .producer_backend_id = bioseq::indexing::
                minimizer_producer_backend::scalar,
            .producer_identity_revision = bioseq::indexing::
                k_minimizer_producer_identity_revision,
            .max_occ_cap = bioseq::indexing::
                k_grouped_soa_default_max_occurrence_cap,
        };
        index_storage_fragment_generation_read_session session{
            table_name, query_key, registered_snapshot,
            k_fixture_expected,
            table_tablespace_oid,
            minimizer_generation_snapshot_registration::borrow_registered};
        head = session.read_active_head_once();
        manifest = session.read_manifest_directory_once(head).manifest;
        session.close_metadata_read();
    }
    bioseq::postgres::support::invoke_pg_as_cpp(
        [registered_snapshot] { UnregisterSnapshot(registered_snapshot); });
    manifest.source_content_checksum ^= xor_mask;
    head.source_content_checksum = manifest.source_content_checksum;
    const auto manifest_bytes =
        bioseq::indexing::encode_minimizer_generation_manifest(manifest);
    if (!manifest_bytes) {
        minimizer_generation_storage_error_as_cpp(
            "current generation checksum corruption could not encode manifest");
    }
    head.manifest_checksum = bioseq::indexing::crc64_ecma182(
        std::span<const std::byte>{*manifest_bytes});
    const auto head_bytes =
        bioseq::indexing::encode_minimizer_generation_head(head);
    if (!head_bytes) {
        minimizer_generation_storage_error_as_cpp(
            "current generation checksum corruption could not encode head");
    }
    const std::uint64_t manifest_row_checksum =
        bioseq::indexing::crc64_index_chunk_row(
            head.generation_id, -1, k_directory_sub_key, 0,
            static_cast<std::uint32_t>(manifest_bytes->size()),
            std::span<const std::byte>{*manifest_bytes});
    const std::uint64_t head_row_checksum =
        bioseq::indexing::crc64_index_chunk_row(
            0, -1, k_generation_head_sub_key, 0,
            static_cast<std::uint32_t>(head_bytes->size()),
            std::span<const std::byte>{*head_bytes});

    const auto make_bytea = [](std::span<const std::byte> bytes) {
        bytea* value = bioseq::postgres::support::invoke_pg_as_cpp([bytes] {
            return reinterpret_cast<bytea*>(
                palloc(VARHDRSZ + bytes.size()));
        });
        SET_VARSIZE(value, VARHDRSZ + bytes.size());
        std::memcpy(VARDATA(value), bytes.data(), bytes.size());
        return value;
    };
    const char* quoted_table = bioseq::postgres::support::invoke_pg_as_cpp(
        [&table_name] { return quote_identifier(table_name.c_str()); });
    const std::string sql = fmt::format(
        "UPDATE bioseq_internal.{} SET blob=$1,chunk_bytes=$2,checksum=$3 "
        "WHERE query_key=$4 AND generation_id=$5 AND position_segment=-1 "
        "AND sub_key=$6 AND chunk_no=0",
        quoted_table);
    std::array<Oid, 6> argument_types{
        BYTEAOID, INT4OID, INT8OID, INT8OID, INT8OID, INT8OID};
    bytea* manifest_blob = make_bytea(
        std::span<const std::byte>{*manifest_bytes});
    bytea* head_blob = make_bytea(
        std::span<const std::byte>{*head_bytes});
    std::array<Datum, 6> manifest_arguments{
        PointerGetDatum(manifest_blob),
        Int32GetDatum(static_cast<std::int32_t>(manifest_bytes->size())),
        Int64GetDatum(checksum_to_pg_bigint(manifest_row_checksum)),
        Int64GetDatum(static_cast<std::int64_t>(query_key)),
        Int64GetDatum(static_cast<std::int64_t>(head.generation_id)),
        Int64GetDatum(static_cast<std::int64_t>(k_directory_sub_key)),
    };
    std::array<Datum, 6> head_arguments{
        PointerGetDatum(head_blob),
        Int32GetDatum(static_cast<std::int32_t>(head_bytes->size())),
        Int64GetDatum(checksum_to_pg_bigint(head_row_checksum)),
        Int64GetDatum(static_cast<std::int64_t>(query_key)),
        Int64GetDatum(0),
        Int64GetDatum(static_cast<std::int64_t>(k_generation_head_sub_key)),
    };
    struct rewrite_spi_state {
        int connect_status = 0;
        int manifest_status = 0;
        int head_status = 0;
        int finish_status = 0;
        std::uint64_t manifest_rows = 0;
        std::uint64_t head_rows = 0;
    } spi{};
    const char* sql_data = sql.c_str();
    bioseq::postgres::support::invoke_pg_as_cpp(
        [&spi, sql_data, &argument_types, &manifest_arguments,
         &head_arguments] noexcept {
            spi.connect_status = SPI_connect();
            if (spi.connect_status != SPI_OK_CONNECT) return;
            spi.manifest_status = SPI_execute_with_args(
                sql_data, static_cast<int>(argument_types.size()),
                argument_types.data(), manifest_arguments.data(),
                nullptr, false, 1);
            spi.manifest_rows = SPI_processed;
            if (spi.manifest_status == SPI_OK_UPDATE
                && spi.manifest_rows == 1) {
                spi.head_status = SPI_execute_with_args(
                    sql_data, static_cast<int>(argument_types.size()),
                    argument_types.data(), head_arguments.data(),
                    nullptr, false, 1);
                spi.head_rows = SPI_processed;
            }
            spi.finish_status = SPI_finish();
        });
    bioseq::postgres::support::invoke_pg_as_cpp(
        [manifest_blob, head_blob] {
            pfree(manifest_blob);
            pfree(head_blob);
        });
    if (spi.connect_status != SPI_OK_CONNECT
        || spi.manifest_status != SPI_OK_UPDATE
        || spi.manifest_rows != 1
        || spi.head_status != SPI_OK_UPDATE
        || spi.head_rows != 1
        || spi.finish_status != SPI_OK_FINISH) {
        minimizer_generation_storage_error_as_cpp(fmt::format(
            "current generation checksum corruption SPI session disagrees: connect={} "
            "manifest_status={} manifest_rows={} head_status={} head_rows={} "
            "finish={}",
            spi.connect_status, spi.manifest_status, spi.manifest_rows,
            spi.head_status, spi.head_rows, spi.finish_status));
    }
    bioseq::postgres::support::invoke_pg_as_cpp(
        [] { CommandCounterIncrement(); });
}

/// Setup-only identity carried across the mandatory E2E restart. The timed
/// backend must obtain its own identity from statement authorization instead of
/// invoking this pre-restart inspection seam.
struct ready_minimizer_generation_identity {
    std::uint64_t generation_id = 0;
    std::uint64_t source_identity = 0;
    std::uint64_t source_content_checksum = 0;
    std::uint64_t storage_contract_checksum = 0;

    [[nodiscard]] bool operator==(
        const ready_minimizer_generation_identity&) const = default;
};

/// Read-only, PostgreSQL-owned pre-restart audit for one exact relation row,
/// column and logical minimizer index. It validates the source relfilenode and
/// attribute, the exact eight-column sidecar/PK, the address-bound row CRCs,
/// fixed head/manifest wire, every directory chunk, and all cross-object
/// metadata equations under one registered MVCC snapshot. It intentionally
/// does not activate ordinal 2 or expose a standalone SQL symbol.
[[nodiscard]] inline ready_minimizer_generation_identity
inspect_ready_minimizer_generation_identity_pg(
        const minimizer_generation_administration_identity& identity,
        std::uint64_t sequence_rowno,
        Snapshot executor_snapshot,
        expected_minimizer_generation_identity expected) {
    const auto minimizer_generation_storage_error = [](std::string_view message) {
        minimizer_generation_storage_error_as_cpp(message);
    };
    struct source_inspection_state {
        Snapshot registered_snapshot = nullptr;
        MemoryContext work_context = nullptr;
        void* session_storage = nullptr;
        index_storage_fragment_generation_read_session* session = nullptr;
        bool session_constructed = false;
        std::optional<minimizer_generation_catalog_source_authorization> source_authorization;
    };
    auto* source = new source_inspection_state{};
    ready_minimizer_generation_identity result{};
    try {
        if (identity.database_oid != MyDatabaseId
            || !OidIsValid(identity.user_relid)
            || identity.column_attno <= 0
            || identity.index_name.empty()
            || identity.table_name.empty()
            || !bioseq::indexing::sql_bigint_can_represent(
                sequence_rowno)
            || executor_snapshot == nullptr
            || !IsMVCCSnapshot(executor_snapshot)
            || executor_snapshot->regd_count == 0) {
            minimizer_generation_storage_error(
                "generation identity inspection input is invalid");
        }
        source->registered_snapshot = executor_snapshot;
        source->source_authorization.emplace(
            authorize_minimizer_generation_catalog_source_pg(
                identity, sequence_rowno,
                source->registered_snapshot));
        const minimizer_generation_source_locator& validated_source =
            source->source_authorization->source_for_diagnostics();
        source->work_context = bioseq::postgres::support::invoke_pg_as_cpp([] {
            return AllocSetContextCreate(
                CurrentMemoryContext, "bioseq current generation source inspection",
                ALLOCSET_DEFAULT_SIZES);
        });

        source->session_storage = bioseq::postgres::support::invoke_pg_as_cpp([source] {
            return MemoryContextAlloc(
                source->work_context,
                sizeof(index_storage_fragment_generation_read_session));
        });
        source->session = ::new (source->session_storage)
            index_storage_fragment_generation_read_session{
                identity.table_name,
                sequence_rowno,
                source->registered_snapshot,
                expected,
                identity.tablespace_oid,
                minimizer_generation_snapshot_registration::borrow_registered};
        source->session_constructed = true;
        const minimizer_generation_head head =
            source->session->read_active_head_once();
        const minimizer_generation_read_metadata& metadata =
            source->session->read_manifest_directory_once(head);
        if (metadata.manifest.alphabet_code
                != validated_source.alphabet_code
            || metadata.manifest.storage_unit_byte_width
                != validated_source.storage_unit_byte_width
            || metadata.manifest.target_length
                != validated_source.locator.logical_length) {
            minimizer_generation_storage_error(
                "generation identity manifest/source row disagrees");
        }
        const std::uint64_t validated_source_content_checksum =
            source->source_authorization
                ->source_content_checksum_for_diagnostics();
        const std::uint64_t validated_storage_contract_checksum =
            bioseq::indexing::compute_minimizer_generation_storage_contract_checksum(
                metadata.manifest);
        source->session->consume_source_authorization(
            std::move(*source->source_authorization));
        source->source_authorization.reset();
        result = ready_minimizer_generation_identity{
            .generation_id = metadata.head.generation_id,
            .source_identity = metadata.head.source_identity,
            .source_content_checksum = validated_source_content_checksum,
            .storage_contract_checksum = validated_storage_contract_checksum,
        };
        source->session->plan_control_groups(
            std::span<const minimizer_generation_payload_read_group>{});
        source->session->plan_position_groups(
            std::span<const minimizer_generation_payload_read_group>{});
        const minimizer_generation_read_stats read_stats =
            source->session->finish();
        if (read_stats.source_parent_rows_read != 1
            || read_stats.source_catalog_rows_read != 1
            || read_stats.source_catalog_index_scans != 1
            || read_stats.source_fragment_rows_read != 0
            || read_stats.source_decoded_bp != 0) {
            minimizer_generation_storage_error(
                "generation identity timed source work equation disagrees");
        }
        source->session->~index_storage_fragment_generation_read_session();
        source->session = nullptr;
        source->session_constructed = false;
        source->session_storage = nullptr;

        source->registered_snapshot = nullptr;
        MemoryContext finished_context = std::exchange(
            source->work_context, nullptr);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [finished_context] { MemoryContextDelete(finished_context); });
    } catch (...) {
        if (source->session_constructed && source->session != nullptr) {
            auto* cleanup_session = source->session;
            source->session = nullptr;
            source->session_constructed = false;
            cleanup_session->~index_storage_fragment_generation_read_session();
        }
        source->session_storage = nullptr;
        source->registered_snapshot = nullptr;
        // The failed statement owns the MemoryContext. Do not invoke PG
        // cleanup while propagating either a bridged PG error or a C++ error.
        source->work_context = nullptr;
        delete source;
        throw;
    }
    delete source;
    return result;
}

namespace minimizer_generation_pg_compile_contract_detail {
using publisher_type = ready_minimizer_generation_published_head_receipt (*)(
    ready_minimizer_generation_token&&);
using inspector_type = ready_minimizer_generation_identity (*)(
    const minimizer_generation_administration_identity&,
    std::uint64_t,
    Snapshot,
    expected_minimizer_generation_identity);

static_assert(std::same_as<
    decltype(&publish_ready_minimizer_generation_head_pg), publisher_type>);
static_assert(std::same_as<
    decltype(&inspect_ready_minimizer_generation_identity_pg), inspector_type>);
static_assert(requires(
    mapped_minimizer_fragment_image& image,
    const mapped_minimizer_fragment_lease_guard& guard,
    const bioseq::indexing::compact_hash32_direct_index_view& direct) {
    { image.borrow_mapping_token() }
        -> std::same_as<std::shared_ptr<const void>>;
    { image.advise_compact_h32_direct_metadata_readahead(direct) }
        -> std::same_as<void>;
    { image.borrow_compact_h32_selected_page_adviser(direct) }
        -> std::same_as<
            bioseq::indexing::compact_direct_selected_page_adviser>;
    { std::move(image).release_mapping_lease_guard() }
        -> std::same_as<mapped_minimizer_fragment_lease_guard>;
    { guard.borrow_mapping_token() }
        -> std::same_as<std::shared_ptr<const void>>;
});
static_assert(noexcept(std::declval<const mapped_minimizer_fragment_image&>()
    .advise_compact_h32_direct_metadata_readahead(
        std::declval<const bioseq::indexing::compact_hash32_direct_index_view&>())));
static_assert(noexcept(std::declval<const mapped_minimizer_fragment_image&>()
    .borrow_compact_h32_selected_page_adviser(
        std::declval<const bioseq::indexing::compact_hash32_direct_index_view&>())));
static_assert(requires(
    index_storage_fragment_generation_row_sink& writer,
    const encoded_grouped_soa_generation& generation,
    const minimizer_generation_row_view& row) {
    { writer.write(row) }
        -> std::same_as<std::expected<void, minimizer_generation_encoding_error>>;
    { writer.validate_ready_generation(generation) }
        -> std::same_as<ready_minimizer_generation_token>;
});
static_assert(requires(
    index_storage_fragment_generation_read_session& session,
    const minimizer_generation_head& head,
    std::span<const minimizer_generation_payload_read_group> groups) {
    { session.read_active_head_once() }
        -> std::same_as<minimizer_generation_head>;
    { session.read_manifest_directory_once(head) }
        -> std::same_as<const minimizer_generation_read_metadata&>;
    { session.read_manifest_once_without_directory(head) }
        -> std::same_as<const minimizer_generation_manifest&>;
    { session.plan_control_groups(groups) };
    { session.plan_position_groups(groups) };
    { session.read_full_generation_payload_once(
        minimizer_generation_compile_contract_detail::qualified_read_sink{},
        minimizer_generation_compile_contract_detail::qualified_read_sink{}) }
        -> std::same_as<index_chunk_read_stats>;
    { session.finish() } -> std::same_as<minimizer_generation_read_stats>;
    { session.finish_mapped_compact_direct() }
        -> std::same_as<minimizer_generation_read_stats>;
});

constexpr minimizer_generation_source_locator k_source_identity_golden{
    .sequence_rowno = 17,
    .locator = sequence_locator{
        .payload_version = sequence_locator_payload_version,
        .bucket_index = 9,
        .reserved_16 = 0,
        .shard_index = 4,
        .fragment_count = 3,
        .reserved_32 = 0,
        .logical_length = 12'345,
        .first_fragment_row = 77,
        .source_epoch = 101,
    },
    .alphabet_code = 1,
    .storage_unit_byte_width = 8,
    .padding_letters = 3,
    .storage_contract_checksum = 0x0102030405060708ULL,
};
constexpr auto k_source_identity_different_epoch = [] {
    auto source = k_source_identity_golden;
    ++source.locator.source_epoch;
    return source;
}();
static_assert(minimizer_generation_source_identity(42, k_source_identity_golden) == 101);
static_assert(minimizer_generation_source_identity(42, k_source_identity_golden)
    != minimizer_generation_source_identity(42, k_source_identity_different_epoch));
static_assert(minimizer_generation_source_identity(42, k_source_identity_golden)
    == minimizer_generation_source_identity(43, k_source_identity_golden));
}  // namespace minimizer_generation_pg_compile_contract_detail


}  // namespace bioseq::storage::indexing
