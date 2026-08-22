/**
 * @file generation_session_lifecycle.cppm
 * @brief Defines grouped-generation session binding, counters, and PostgreSQL resource cleanup.
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
export module bioseq.postgres.storage.indexing.chunked_storage:generation_session_lifecycle;
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
export namespace bioseq::storage::indexing {

/**
 * @brief Returns the single manifest currently bound to the read session.
 * @return Borrowed manifest pointer, or null unless exactly one metadata form is active.
 */
[[nodiscard]] inline const minimizer_generation_manifest*
index_storage_fragment_generation_read_session::active_manifest_() const noexcept {
    if (metadata_.has_value() == manifest_only_.has_value()) {
        return nullptr;
    }
    return metadata_ ? &metadata_->manifest : &*manifest_only_;
}

/**
 * @brief Marks the session failed and raises a storage error.
 * @param[in] message Diagnostic describing the violated generation contract.
 */
[[noreturn]] inline void
index_storage_fragment_generation_read_session::fail_(std::string_view message) {
    state_ = operation_state::failed;
    minimizer_generation_storage_error_as_cpp(message);
}

/**
 * @brief Adds to a session counter while rejecting unsigned overflow.
 * @param[in,out] value Counter to update.
 * @param[in] increment Amount to add.
 * @param[in] message Diagnostic used when the addition would overflow.
 */
inline void index_storage_fragment_generation_read_session::checked_add_(std::uint64_t& value,
                                                                         std::uint64_t increment,
                                                                         std::string_view message) {
    if (increment > (std::numeric_limits<std::uint64_t>::max)() - value) {
        fail_(message);
    }
    value += increment;
}

/**
 * @brief Classifies one payload tuple's heap-block movement for cold-read statistics.
 * @param[in] tid PostgreSQL tuple identifier whose block number is sampled.
 */
inline void
index_storage_fragment_generation_read_session::record_payload_tid_(ItemPointerData tid) noexcept {
    const std::uint64_t block = static_cast<std::uint64_t>(ItemPointerGetBlockNumber(&tid));
    if (previous_payload_heap_block_) {
        const std::uint64_t previous = *previous_payload_heap_block_;
        if (block == previous || block == previous + 1u) {
            stats_.cold.add(stats_.cold.heap_tid_adjacent, 1);
        } else if (block > previous) {
            stats_.cold.add(stats_.cold.heap_tid_forward_gap, 1);
        } else {
            stats_.cold.add(stats_.cold.heap_tid_backward_jump, 1);
        }
    }
    previous_payload_heap_block_ = block;
}

/**
 * @brief Records heap-block movement for one payload tuple.
 * @param[in] tuple Borrowed heap tuple read by the active scan.
 */
inline void index_storage_fragment_generation_read_session::record_payload_tid_(
    const HeapTuple tuple) noexcept {
    record_payload_tid_(tuple->t_self);
}

/// @brief Starts the payload-stage timer once for the current read stage.
inline void index_storage_fragment_generation_read_session::start_payload_timing_() noexcept {
    if (!payload_stage_begin_ns_) {
        payload_stage_begin_ns_ = cold_index_now_ns();
    }
}

/// @brief Stops the payload-stage timer and adds saturated nanoseconds to statistics.
inline void index_storage_fragment_generation_read_session::finalize_payload_timing_() noexcept {
    if (!payload_stage_begin_ns_)
        return;
    const std::uint64_t end_ns = cold_index_now_ns();
    const std::uint64_t raw_total_ns =
        end_ns >= *payload_stage_begin_ns_ ? end_ns - *payload_stage_begin_ns_ : 0;
    const std::uint64_t total_ns =
        std::min(raw_total_ns, cold_index_preparation_stats::k_sql_bigint_max);
    if (raw_total_ns != total_ns) {
        stats_.cold.are_stats_saturated = true;
    }
    stats_.cold.add(stats_.cold.payload_ns, total_ns);
    payload_stage_begin_ns_.reset();
}

/**
 * @brief Releases admitted vectors, metadata, and permits owned by the session.
 *
 * Container capacity is returned before its corresponding admission permit.
 */
inline void index_storage_fragment_generation_read_session::release_owned_state_() noexcept {
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

/// @brief Releases the current extent plan and its admitted capacity.
inline void index_storage_fragment_generation_read_session::release_section_plan_() noexcept {
    std::vector<planned_group>{}.swap(planned_groups_);
    next_group_ = 0;
    plan_permit_.release();
}

/// @brief Releases authorized position ordinals and their admitted capacity.
inline void
index_storage_fragment_generation_read_session::release_control_authorization_() noexcept {
    std::vector<std::uint32_t>{}.swap(authorized_position_ordinals_);
    control_authorization_permit_.release();
}

/**
 * @brief Adds a payload-read statistics block with overflow checks.
 * @param[in,out] destination Accumulated session statistics.
 * @param[in] increment Statistics produced by one payload operation.
 */
inline void index_storage_fragment_generation_read_session::checked_add_read_stats_(
    index_chunk_read_stats& destination, const index_chunk_read_stats& increment) {
    checked_add_(destination.relation_opens,
                 increment.relation_opens,
                 "payload relation-open counter overflow");
    checked_add_(
        destination.index_scans, increment.index_scans, "payload index-scan counter overflow");
    checked_add_(destination.requested_ranges,
                 increment.requested_ranges,
                 "payload requested-range counter overflow");
    checked_add_(
        destination.heap_rows_read, increment.heap_rows_read, "payload heap-row counter overflow");
    checked_add_(destination.toast_values_read,
                 increment.toast_values_read,
                 "payload toast-value counter overflow");
    checked_add_(destination.detoast_bytes,
                 increment.detoast_bytes,
                 "payload detoast-byte counter overflow");
    checked_add_(destination.explicit_copy_bytes,
                 increment.explicit_copy_bytes,
                 "payload explicit-copy counter overflow");
    checked_add_(destination.wanted_subkeys,
                 increment.wanted_subkeys,
                 "payload wanted-sub-key counter overflow");
    checked_add_(
        destination.read_subkeys, increment.read_subkeys, "payload read-sub-key counter overflow");
    checked_add_(destination.wanted_extents,
                 increment.wanted_extents,
                 "payload wanted-extent counter overflow");
    checked_add_(
        destination.read_extents, increment.read_extents, "payload read-extent counter overflow");
    checked_add_(destination.wanted_chunks,
                 increment.wanted_chunks,
                 "payload wanted-chunk counter overflow");
    checked_add_(
        destination.skipped_rows, increment.skipped_rows, "payload skipped-row counter overflow");
    checked_add_(destination.skip_budget_rows,
                 increment.skip_budget_rows,
                 "payload skip-budget counter overflow");
    checked_add_(destination.repeated_chunk_reads,
                 increment.repeated_chunk_reads,
                 "payload repeated-read counter overflow");
    checked_add_(destination.delivered_chunks,
                 increment.delivered_chunks,
                 "payload delivered-chunk counter overflow");
    checked_add_(destination.delivered_bytes,
                 increment.delivered_bytes,
                 "payload delivered-byte counter overflow");
}

/**
 * @brief Creates the fixed batch of buffer-backed tuple slots for full scans.
 *
 * Slots are owned by the session and remain valid until checked drop or
 * PostgreSQL resource-owner cleanup after abandonment.
 */
inline void
index_storage_fragment_generation_read_session::create_full_generation_batch_slots_checked_() {
    bioseq::postgres::support::invoke_pg_as_cpp([this] {
        while (full_generation_batch_slot_count_ < full_generation_batch_slots_.size()) {
            TupleTableSlot* slot = table_slot_create(heap_relation_, nullptr);
            Assert(TTS_IS_BUFFERTUPLE(slot));
            full_generation_batch_slots_[full_generation_batch_slot_count_] = slot;
            ++full_generation_batch_slot_count_;
        }
    });
}

/// @brief Drops every session-owned full-generation tuple slot through PostgreSQL.
inline void
index_storage_fragment_generation_read_session::drop_full_generation_batch_slots_checked_() {
    if (full_generation_batch_slot_count_ == 0)
        return;
    bioseq::postgres::support::invoke_pg_as_cpp([this] {
        while (full_generation_batch_slot_count_ != 0) {
            const std::size_t ordinal = full_generation_batch_slot_count_ - 1u;
            TupleTableSlot* slot = full_generation_batch_slots_[ordinal];
            full_generation_batch_slots_[ordinal] = nullptr;
            full_generation_batch_slot_count_ = ordinal;
            if (slot != nullptr) {
                ExecDropSingleTupleTableSlot(slot);
            }
        }
    });
}

/**
 * @brief Forgets all PostgreSQL resources without calling cleanup functions.
 *
 * This is the destructor and PostgreSQL-error path. The active ResourceOwner
 * retains responsibility for pins, scans, relations, and a registered snapshot.
 */
inline void index_storage_fragment_generation_read_session::abandon_relations_noexcept_() noexcept {
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

/**
 * @brief Closes scans, slots, relations, and any session-registered snapshot.
 *
 * Relations and indexes are released with their matching `AccessShareLock`.
 * A PostgreSQL cleanup error changes the session to failed and abandons the
 * remaining aliases to the active ResourceOwner.
 */
inline void index_storage_fragment_generation_read_session::close_relations_checked_() {
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
            bioseq::postgres::support::invoke_pg_as_cpp(
                [this] { index_close(storage_descriptor_live_index_relation_, AccessShareLock); });
            storage_descriptor_live_index_relation_ = nullptr;
            stats_.cold.add(stats_.cold.lock_releases, 1);
        }
        if (storage_descriptor_relation_ != nullptr) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [this] { relation_close(storage_descriptor_relation_, AccessShareLock); });
            storage_descriptor_relation_ = nullptr;
            stats_.cold.add(stats_.cold.lock_releases, 1);
        }
        if (primary_index_relation_ != nullptr) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [this] { index_close(primary_index_relation_, AccessShareLock); });
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
            bioseq::postgres::support::invoke_pg_as_cpp([this] { UnregisterSnapshot(snapshot_); });
        }
        snapshot_ = nullptr;
        snapshot_registered_by_session_ = false;
        release_owned_state_();
        add_cold_index_elapsed(stats_.cold, stats_.cold.close_ns, close_begin_ns);
        add_cold_index_buffer_usage(stats_.cold, close_buffer_begin);
    } catch (const bioseq::postgres::support::postgres_error&) {
        state_ = operation_state::failed;
        abandon_relations_noexcept_();
        throw;
    }
}

/**
 * @brief Cleans up a failed session and rethrows the initiating exception.
 *
 * A secondary cleanup failure is suppressed so it cannot replace the original
 * corruption or sink error.
 */
[[noreturn]] inline void
index_storage_fragment_generation_read_session::cleanup_failed_session_and_rethrow_() {
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

/**
 * @brief Projects a stored sub-key range into one logical extent namespace.
 * @param[in] range Half-open stored sub-key range.
 * @return Logical half-open range and extent kind, or no value for an invalid or mixed range.
 */
[[nodiscard]] inline std::optional<
    index_storage_fragment_generation_read_session::projected_extent_range>
index_storage_fragment_generation_read_session::project_extent_range_(
    const index_sub_key_range& range) noexcept {
    if (range.begin < 0 || range.begin >= range.end || range.end > k_temporary_run_sub_key_base) {
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

/**
 * @brief Tests whether a request exactly names one directory extent.
 * @param[in] request Requested sub-key, chunks, byte count, and checksum.
 * @param[in] entry Directory entry being authorized.
 * @param[in] position_extent Whether to compare the position rather than control extent.
 * @return `true` when every persisted extent field matches.
 */
[[nodiscard]] inline bool index_storage_fragment_generation_read_session::request_matches_extent_(
    const index_sub_key_request& request,
    const grouped_soa_bucket_directory_entry& entry,
    bool position_extent) noexcept {
    const auto position_sub_key = derive_minimizer_position_extent_sub_key(entry.sub_key);
    if (!position_sub_key) {
        return false;
    }
    const bool control = !position_extent && request.sub_key == entry.sub_key &&
                         request.first_chunk_no == entry.control_first_chunk &&
                         request.expected_chunk_count == entry.control_chunk_count &&
                         request.expected_total_bytes == entry.control_encoded_bytes &&
                         request.expected_checksum == entry.control_checksum;
    const bool positions = position_extent && request.sub_key == *position_sub_key &&
                           request.first_chunk_no == entry.position_first_chunk &&
                           request.expected_chunk_count == entry.position_chunk_count &&
                           request.expected_total_bytes == entry.position_encoded_bytes &&
                           request.expected_checksum == entry.position_checksum;
    return control || positions;
}

/**
 * @brief Binds a grouped-generation read session to one validated sidecar.
 *
 * The constructor validates expected layout identity, opens the heap and
 * primary index under `AccessShareLock`, and optionally registers a snapshot
 * copy owned by the session. Classification mode reports missing or malformed
 * relations with `minimizer_generation_relation_open_failure`.
 *
 * @param[in] table_name Canonical extension-internal sidecar name.
 * @param[in] query_key Generation query key stored as SQL `BIGINT`.
 * @param[in] executor_snapshot Registered MVCC snapshot used for all reads.
 * @param[in] expected Expected immutable generation and encoding identity.
 * @param[in] expected_tablespace_oid Tablespace required for the sidecar.
 * @param[in] snapshot_registration Whether to borrow or register a copy of the snapshot.
 * @param[in] classification_index_name Nonempty when absence and malformed schema are classified.
 */
inline index_storage_fragment_generation_read_session::
    index_storage_fragment_generation_read_session(
        std::string_view table_name,
        std::uint64_t query_key,
        Snapshot executor_snapshot,
        expected_minimizer_generation_identity expected,
        Oid expected_tablespace_oid,
        minimizer_generation_snapshot_registration snapshot_registration,
        std::string_view classification_index_name)
    : table_name_(table_name), snapshot_(executor_snapshot), query_key_(query_key),
      expected_tablespace_oid_(expected_tablespace_oid), expected_(expected) {
    const std::uint64_t bind_begin_ns = cold_index_now_ns();
    const BufferUsage bind_buffer_begin = pgBufferUsage;
    try {
        if (snapshot_ == nullptr || !IsMVCCSnapshot(snapshot_) || snapshot_->regd_count == 0) {
            minimizer_generation_storage_error(
                "generation read requires a registered executor MVCC snapshot");
        }
        if (!classification_index_name.empty() &&
            snapshot_registration !=
                minimizer_generation_snapshot_registration::borrow_registered) {
            minimizer_generation_storage_error(
                "bound generation classification must borrow the executor snapshot");
        }
        if (!bioseq::indexing::sql_bigint_can_represent(query_key_)) {
            minimizer_generation_storage_error(
                "generation read query key exceeds signed SQL range");
        }
        if (!OidIsValid(expected_tablespace_oid_)) {
            minimizer_generation_storage_error("generation read expected tablespace is invalid");
        }
        const bioseq::indexing::minimizer_scheme_identity expected_scheme{
            .id = expected_.minimizer_scheme_id,
            .revision = expected_.minimizer_scheme_revision,
        };
        const auto expected_profile =
            expected_.max_occ_cap
                ? bioseq::indexing::derive_grouped_soa_minimizer_profile(
                      expected_scheme,
                      expected_.alphabet_code,
                      expected_.kmer_size,
                      /*position_segment_span_bp=*/1u,
                      *expected_.max_occ_cap)
                : std::expected<grouped_soa_minimizer_profile, minimizer_generation_encoding_error>{
                      std::unexpected(
                          minimizer_generation_encoding_error::storage_contract_mismatch)};
        const bool supported_storage_width =
            expected_.storage_unit_byte_width == 1u || expected_.storage_unit_byte_width == 2u ||
            expected_.storage_unit_byte_width == 4u || expected_.storage_unit_byte_width == 8u;
        if (!bioseq::indexing::is_supported_minimizer_generation_layout(expected_.layout) ||
            !expected_profile || expected_profile->layout != expected_.layout ||
            expected_profile->minimizer_scheme != expected_scheme ||
            expected_profile->kmer_size != expected_.kmer_size || !supported_storage_width ||
            expected_.window_size == 0 ||
            expected_.gpu_abi_revision != expected_profile->gpu_abi_revision) {
            minimizer_generation_storage_error(
                "generation read expected minimizer storage contract is invalid");
        }
        expected_profile_ = *expected_profile;
        if (snapshot_registration == minimizer_generation_snapshot_registration::register_copy) {
            snapshot_ = bioseq::postgres::support::invoke_pg_as_cpp(
                [this] { return RegisterSnapshot(snapshot_); });
            snapshot_registered_by_session_ = true;
            stats_.cold.snapshot_registers = 1;
        }
        const Oid relation_oid = bioseq::postgres::support::invoke_pg_as_cpp(
            [this] { return get_relname_relid(table_name_.c_str(), bioseq_internal_ns()); });
        if (!OidIsValid(relation_oid) && !classification_index_name.empty()) {
            throw minimizer_generation_relation_open_failure{
                .inspection =
                    {
                        .kind = minimizer_generation_relation_kind::missing,
                        .schema_error = minimizer_generation_schema_error::none,
                    },
            };
        }
        heap_relation_ =
            OidIsValid(relation_oid) ? bioseq::postgres::support::invoke_pg_as_cpp([relation_oid] {
                return bioseq::postgres::support::try_open_postgres_relation(relation_oid,
                                                                             AccessShareLock);
            })
                                     : nullptr;
        if (heap_relation_ != nullptr) {
            stats_.cold.add(stats_.cold.sidecar_heap_opens, 1);
            stats_.cold.add(stats_.cold.lock_acquires, 1);
        }
        if (heap_relation_ == nullptr && !classification_index_name.empty()) {
            throw minimizer_generation_relation_open_failure{
                .inspection =
                    {
                        .kind = minimizer_generation_relation_kind::missing,
                        .schema_error = minimizer_generation_schema_error::none,
                    },
            };
        }
        if (heap_relation_ == nullptr) {
            minimizer_generation_storage_error("generation relation is missing or renamed");
        }
        const bool relation_identity_exact =
            RelationGetNamespace(heap_relation_) == bioseq_internal_ns() &&
            std::string_view{RelationGetRelationName(heap_relation_)} == table_name_;
        const Oid primary_index_oid = bioseq::postgres::support::invoke_pg_as_cpp(
            [this] { return RelationGetPrimaryKeyIndex(heap_relation_); });
        if (OidIsValid(primary_index_oid)) {
            primary_index_relation_ = bioseq::postgres::support::invoke_pg_as_cpp(
                [primary_index_oid] { return index_open(primary_index_oid, AccessShareLock); });
            stats_.cold.add(stats_.cold.sidecar_index_opens, 1);
            stats_.cold.add(stats_.cold.lock_acquires, 1);
        }
        const minimizer_generation_schema_inspection schema =
            relation_identity_exact ? bioseq::postgres::support::invoke_pg_as_cpp([this] {
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
        stats_.cold.add(stats_.cold.sidecar_schema_validations, 1);
        if (!schema) {
            if (!classification_index_name.empty()) {
                if (primary_index_relation_ != nullptr) {
                    Relation finished_index = std::exchange(primary_index_relation_, nullptr);
                    bioseq::postgres::support::invoke_pg_as_cpp(
                        [finished_index] { index_close(finished_index, AccessShareLock); });
                    stats_.cold.add(stats_.cold.lock_releases, 1);
                }
                Relation finished_heap = std::exchange(heap_relation_, nullptr);
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [finished_heap] { relation_close(finished_heap, AccessShareLock); });
                stats_.cold.add(stats_.cold.lock_releases, 1);
                if (snapshot_registered_by_session_ && snapshot_ != nullptr) {
                    Snapshot finished_snapshot = snapshot_;
                    snapshot_ = nullptr;
                    snapshot_registered_by_session_ = false;
                    bioseq::postgres::support::invoke_pg_as_cpp(
                        [finished_snapshot] { UnregisterSnapshot(finished_snapshot); });
                }
                throw minimizer_generation_relation_open_failure{
                    .inspection =
                        {
                            .kind = minimizer_generation_relation_kind::malformed,
                            .schema_error = schema.error,
                        },
                };
            }
            minimizer_generation_storage_error(
                fmt::format("generation relation schema mismatch: {}",
                            minimizer_generation_schema_error_name(schema.error)));
        }
        (void)verify_pg_inline_payload_layout(heap_relation_);
        stats_.relation_opens = 1;
        add_cold_index_elapsed(stats_.cold, stats_.cold.bind_ns, bind_begin_ns);
        add_cold_index_buffer_usage(stats_.cold, bind_buffer_begin);
    } catch (const bioseq::postgres::support::postgres_error&) {
        state_ = operation_state::failed;
        abandon_relations_noexcept_();
        throw;
    }
}

/**
 * @brief Transfers all PostgreSQL and admitted state from another session.
 * @param[in,out] other Session consumed and left in the moved state.
 */
inline index_storage_fragment_generation_read_session::
    index_storage_fragment_generation_read_session(
        index_storage_fragment_generation_read_session&& other) noexcept
    : table_name_(std::move(other.table_name_)),
      heap_relation_(std::exchange(other.heap_relation_, nullptr)),
      primary_index_relation_(std::exchange(other.primary_index_relation_, nullptr)),
      storage_descriptor_relation_(std::exchange(other.storage_descriptor_relation_, nullptr)),
      storage_descriptor_live_index_relation_(
          std::exchange(other.storage_descriptor_live_index_relation_, nullptr)),
      active_scan_(std::exchange(other.active_scan_, nullptr)),
      full_generation_batch_slots_(other.full_generation_batch_slots_),
      full_generation_batch_slot_count_(std::exchange(other.full_generation_batch_slot_count_, 0)),
      snapshot_(std::exchange(other.snapshot_, nullptr)),
      snapshot_registered_by_session_(std::exchange(other.snapshot_registered_by_session_, false)),
      query_key_(other.query_key_), expected_tablespace_oid_(other.expected_tablespace_oid_),
      expected_(other.expected_), expected_profile_(other.expected_profile_), state_(other.state_),
      catalog_source_authorized_(other.catalog_source_authorized_),
      metadata_permit_(std::move(other.metadata_permit_)),
      plan_permit_(std::move(other.plan_permit_)),
      runtime_permit_(std::move(other.runtime_permit_)),
      control_authorization_permit_(std::move(other.control_authorization_permit_)),
      head_(std::move(other.head_)), metadata_(std::move(other.metadata_)),
      manifest_only_(std::move(other.manifest_only_)),
      directory_bytes_scratch_(std::move(other.directory_bytes_scratch_)),
      planned_groups_(std::move(other.planned_groups_)),
      runtime_requests_(std::move(other.runtime_requests_)),
      authorized_position_ordinals_(std::move(other.authorized_position_ordinals_)),
      next_group_(other.next_group_),
      previous_payload_heap_block_(other.previous_payload_heap_block_),
      payload_stage_begin_ns_(other.payload_stage_begin_ns_), stats_(other.stats_) {
    other.full_generation_batch_slots_.fill(nullptr);
    other.state_ = operation_state::moved;
}

/**
 * @brief Abandons any still-open PostgreSQL aliases to their ResourceOwner.
 *
 * Normal callers close the session explicitly. Destruction performs no
 * PostgreSQL cleanup and is safe during error unwinding.
 */
inline index_storage_fragment_generation_read_session::
    ~index_storage_fragment_generation_read_session() {
    abandon_relations_noexcept_();
}

} // namespace bioseq::storage::indexing
