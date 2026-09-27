/**
 * @file generation_session_lifecycle.cppm
 * @brief Open generation read  sessions, record statistics and release their resources.
 */
module;

extern "C" {
#include "postgres.h"

#include "access/genam.h"
#include "access/htup.h"
#include "access/relation.h"
#include "access/tableam.h"
#include "catalog/namespace.h" // RelnameGetRelid
#include "executor/instrument.h"
#include "executor/tuptable.h"
#include "storage/itemptr.h"
#include "storage/lockdefs.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/snapmgr.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <new>

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
import bioseq.indexing.storage;           // Storage adapter requirements.
import bioseq.indexing.minimizer_hashmap; // Minimizer index header and bucket format.
import bioseq.indexing.minimizer_fragment_image; // Minimizer fragment format with a fixed byte order.
import bioseq.indexing.compact_hash32_direct_index;
import bioseq.indexing.minimizer_fragment_compact_hash32_adapter;
import bioseq.postgres.table.io; // begin_column_read/read_next_column_row/end_column_read/column_read_state
import bioseq.storage.lazy_sequence;           // lazy_sequence
import bioseq.postgres.types.compact_sequence; // bioseq::pg_types::seq, dispatch_instance
import bioseq.sequences;                       // compact_sequence_range
import bioseq.sequences.simd;                  // Decode sequence bases with SIMD.
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
 * @brief Return the manifest from the active metadata form.
 * @return Borrowed pointer, or null if both forms are present or both are absent
 */
[[nodiscard]] inline const minimizer_generation_manifest*
index_storage_fragment_generation_read_session::active_manifest_() const noexcept {
    if (metadata_.has_value() == manifest_only_.has_value()) {
        return nullptr;
    }
    return metadata_ ? &metadata_->manifest : &*manifest_only_;
}

/**
 * @brief Mark the session failed and report a storage rror.
 * @param[in] message Error description.
 */
[[noreturn]] inline void
index_storage_fragment_generation_read_session::fail_(std::string_view message) {
    state_ = operation_state::failed;
    minimizer_generation_storage_error_as_cpp(message);
}

/**
 * @brief Add to a counter, failing if  the value would overflow.
 * @param[in,out] value Counter.
 * @param[in] increment Amount to add.
 * @param[in] message Overflow error message.
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
 * @brief Count adjacent heap blocks, forward gaps and backward jumps during payload reads.
 * @param[in] tid Tuple location to compare with the previous row.
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
 * @brief Record the heap-block change for this payload row.
 * @param[in] tuple Borrowed tuple from the scan.
 */
inline void index_storage_fragment_generation_read_session::record_payload_tid_(
    const HeapTuple tuple) noexcept {
    record_payload_tid_(tuple->t_self);
}

/// Start the payload timer if it has not started yet.
inline void index_storage_fragment_generation_read_session::start_payload_timing_() noexcept {
    if (!payload_stage_begin_ns_) {
        payload_stage_begin_ns_ = cold_index_now_ns();
    }
}

/// Add elapsed payload time to statistics, capping it at the counter limit, and clear the timer.
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
 * @brief Free session-owned metadata and buffers before releasing their memory reservations.
 */
inline void index_storage_fragment_generation_read_session::release_owned_state_() noexcept {
    // Free the  covered allocations before releasing their memory reservations.
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

/// Free the current read plan and its memory reservation.
inline void index_storage_fragment_generation_read_session::release_section_plan_() noexcept {
    std::vector<planned_group>{}.swap(planned_groups_);
    next_group_ = 0;
    plan_permit_.release();
}

/// Free the list of position extents permitted by completed control reads.
inline void
index_storage_fragment_generation_read_session::release_control_authorization_() noexcept {
    std::vector<std::uint32_t>{}.swap(authorized_position_ordinals_);
    control_authorization_permit_.release();
}

/**
 * @brief Add read counters with overflow checks.
 * @param[in,out] destination Accumulated counters.
 * @param[in] increment Counters from one read.
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
 * @brief Create the tuple slots that pin batches of payload rows.
 *
 * Keep each successful slot allocation in the session so, partial setup can be
 * cleaned up after a PostgreSQL error.
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

/// Release the full-scan tuple slots and their buffer pins.
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
 * @brief Clear PostgreSQL handles and leave their cleanup to ResourceOwner.
 *
 * Destruction and error paths use this wthout  calling PostgreSQL. Also free
 * the C++ metadata and buffers owned by the session
 */
inline void index_storage_fragment_generation_read_session::abandon_relations_noexcept_() noexcept {
    // Leave remaining pins and handles to ResourceOwner during destruction
    // or error cleanup. Clear the pointers to prevent reuse or a second close.
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
 * @brief Close the session's slots, scans and relations and release its snapshot reference.
 *
 * Borrowed snapshots remain registered by their caller. If PostgreSQL cleanup
 * fails, mark the session failed and leave remaining handles to ResourceOwner.
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
 * @brief Try to close a failed session and rethrow its original exception.
 *
 * Ignore a second cleanup  failure so it cannot hide the first error.
 */
[[noreturn]] inline void
index_storage_fragment_generation_read_session::cleanup_failed_session_and_rethrow_() {
    std::exception_ptr original_error = std::current_exception();
    state_ = operation_state::failed;
    try {
        close_relations_checked_();
    } catch (...) {
        // Keep the original error. ResourceOwner will clean up anything that
        // could not be closed here.
        abandon_relations_noexcept_();
    }
    std::rethrow_exception(original_error);
}

/**
 * @brief Convert a control or position sub-key range to directory keys.
 * @param[in] range Stored half-open range.
 * @return Converted range and section, or no value for invalid or mixed ranges.
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
 * @brief Check that a request matches one complete directory extent.
 * @param[in] request Requested chunks, byte count and checksum.
 * @param[in] entry Directory entry.
 * @param[in] position_extent True for positions, false for controls.
 * @return True when the extent fields match.
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
 * @brief Open and check the index table and primary key for a generation read.
 *
 * Keep  both AccessShareLock references for the session.
 *
 * @param[in] table_name Internal index tible name.
 * @param[in] query_key Sequence row key.
 * @param[in] executor_snapshot Registered MVCC snapshot.
 * @param[in] expected Required generation format.
 * @param[in] expected_tablespace_oid Required tablespace.
 * @param[in] snapshot_registration Borrow the registration or register a, copy
 * @param[in] classification_index_name If nonempty, report missing or malformed tables through minimizer_generation_relation_open_failure.
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
            [this] {
                return bioseq::postgres::support::relation_immediate_primary_key_index(heap_relation_);
            });
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
 * @brief Move the reader's PostgreSQL handles, metadata and memory reservations.
 * @param[in,out] other Reader whose PostgreSQL handles are cleared and state is marked as moved.
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
 * @brief Leave any remaining PostgreSQL handles to ResourceOwner cleanup.
 *
 * Callers close successful sessions explicitly. The destructor frees C++ state
 * and makes no PostgreSQL calls while an exception may be  unwinding.
 */
inline index_storage_fragment_generation_read_session::
    ~index_storage_fragment_generation_read_session() {
    abandon_relations_noexcept_();
}

} // namespace bioseq::storage::indexing
