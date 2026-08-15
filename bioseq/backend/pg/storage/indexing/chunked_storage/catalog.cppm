/**
 * @file catalog.cppm
 * @brief PostgreSQL catalog, schema, row-codec, and source-authorization
 *        support for chunked index storage.
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
export module bioseq.postgres.storage.indexing.chunked_storage:catalog;
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
static_assert(__cplusplus >= 202302L, "C++23-or-later");

namespace indexing_entity_handles =
    bioseq::sql::generated::core::
        storage_indexing_pg_chunked_storage::entity_handles;
using minimizer_generation_storage_catalog_row =
    bioseq::storage::catalog_rows::minimizer_generation_storage;
using minimizer_generation_storage_codec =
    bioseq::postgres::storage::manager::sql_row_codecs::
        minimizer_generation_storage_codec;
using metadata_age_log_catalog_row =
    bioseq::storage::indexing::catalog_rows::metadata_age_log;
using metadata_age_log_codec =
    bioseq::postgres::storage::indexing::sql_row_codecs::
        metadata_age_log_codec;

static_assert(
    indexing_entity_handles::minimizer_generation_id_seq
        .schema_binding()
    == bioseq::postgres::support::sql_schema_binding::fixed);

export namespace bioseq::storage::indexing {

using bioseq::indexing::sequence_index_layout;
using bioseq::indexing::index_sub_key_t;
using bioseq::indexing::k_max_index_chunk_size_bytes;
using bioseq::indexing::k_temporary_run_sub_key_base;
using bioseq::indexing::k_position_extent_sub_key_base;
using bioseq::indexing::k_directory_sub_key;
using bioseq::indexing::k_hash_major_dense_seal_sub_key;
using bioseq::indexing::k_hash_major_generation_head_sub_key;
using bioseq::indexing::k_generation_head_sub_key;
using bioseq::indexing::minimizer_hashmap_fragment_index;
using bioseq::indexing::minimizer_generation_head;
using bioseq::indexing::minimizer_generation_manifest;
using bioseq::indexing::minimizer_generation_row_view;
using bioseq::indexing::validated_minimizer_generation_row_receipt;
using bioseq::indexing::unchecked_minimizer_generation_row_view;
using bioseq::indexing::minimizer_generation_row_write_receipt;
using bioseq::indexing::minimizer_generation_encoding_error;
using bioseq::indexing::grouped_soa_minimizer_profile;
using bioseq::indexing::expected_minimizer_generation_identity;
using bioseq::indexing::encoded_grouped_soa_generation;
using bioseq::indexing::grouped_soa_bucket_directory_entry;
using bioseq::indexing::grouped_soa_directory_view;
using bioseq::indexing::index_chunk_address;
using bioseq::indexing::index_chunk_cache_policy;
using bioseq::indexing::index_chunk_read_stats;
using bioseq::indexing::index_sub_key_range;
using bioseq::indexing::direct_minimizer_spill_storage_binding;
using bioseq::indexing::derive_minimizer_position_extent_sub_key;
using bioseq::indexing::project_minimizer_extent_sub_key;
using bioseq::indexing::index_sub_key_request;
using bioseq::indexing::crc64_ecma182_accumulator;
using bioseq::indexing::k_minimizers_per_fragment_index_name;
using bioseq::indexing::k_minimizers_per_sequence_index_name;
using bioseq::indexing::k_symbol_counts_index_name;
using bioseq::indexing::encoded_hash_major_minimizer_head;
using bioseq::indexing::encoded_hash_major_minimizer_manifest;
using bioseq::indexing::hash_major_minimizer_authorized_control_range;
using bioseq::indexing::hash_major_minimizer_authorized_posting_range;
using bioseq::indexing::hash_major_minimizer_storage_profile;
using bioseq::indexing::hash_major_minimizer_consume_control_block_fn;
using bioseq::indexing::hash_major_minimizer_consume_posting_bytes_fn;
using bioseq::indexing::hash_major_minimizer_encoded_metadata;
using bioseq::indexing::hash_major_minimizer_error;
using bioseq::indexing::expected_hash_major_minimizer_generation;
using bioseq::indexing::hash_major_minimizer_head;
using bioseq::indexing::hash_major_minimizer_logical_stream_sink_ref;
using bioseq::indexing::hash_major_minimizer_logical_stream_source_ref;
using bioseq::indexing::hash_major_minimizer_manifest;
using bioseq::indexing::hash_major_minimizer_stream_kind;
using bioseq::indexing::k_hash_major_minimizer_head_bytes;
using bioseq::indexing::k_hash_major_minimizer_manifest_bytes;
using bioseq::indexing::k_hash_major_minimizer_maximum_control_block_bytes;
using bioseq::indexing::k_hash_major_minimizer_read_slab_bytes;
using bioseq::indexing::k_hash_major_minimizer_row_payload_bytes;

static_assert(k_hash_major_generation_head_sub_key > k_temporary_run_sub_key_base);
static_assert(k_hash_major_generation_head_sub_key
    < k_hash_major_dense_seal_sub_key);
static_assert(k_hash_major_dense_seal_sub_key < k_generation_head_sub_key);

// PostgreSQL-side mirror checks: the macros are in scope here.
static_assert(bioseq::indexing::k_varlena_header_bytes == VARHDRSZ, "VARHDRSZ mirror drift");
static_assert(bioseq::indexing::k_postgres_max_allocation_bytes == MaxAllocSize, "MaxAllocSize mirror drift");

/// Backend-local counters for the most recent per-sequence index build.
/// Storage updates the counters, administration resets them, and SQL reads
/// them from the same backend process.
struct last_minimizer_sequence_build_stats {
    std::uint64_t backend_pid = 0;
    std::uint64_t db_oid = 0;
    std::uint64_t relfilenode = 0;
    std::uint64_t sequence_rowno = 0;
    std::uint64_t total_run_count = 0;
    std::uint64_t resident_runs = 0;
    std::uint64_t file_runs = 0;
    std::uint64_t resident_run_bytes = 0;
    std::uint64_t resident_peak_bytes = 0;
    std::uint64_t spilled_run_bytes = 0;
    std::uint64_t postgres_temp_file_bytes = 0;
    std::uint64_t postgres_temp_file_count = 0;
    std::uint64_t merge_reader_buffer_bytes = 0;
    std::uint64_t merge_pass_count = 0;
    std::uint64_t spill_count_from_pressure = 0;
    std::uint64_t resident_demotions = 0;
    std::uint64_t position_spill_bytes = 0;
    std::uint64_t position_spill_count = 0;
};

/// PostgreSQL-local stats for one cold persisted-index preparation. This
/// is a transport delta, not a second execution/fallback authority: the
/// dispatch owner merges it into pipeline_operational_stats exactly once.
/// Every SQL-bound value clamps at INT64_MAX so the PostgreSQL bigint row
/// shape can never wrap a large counter into a plausible small value.
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
    // extent_crc_bytes is the frozen logical byte-coverage counter. The three
    // additive counters distinguish raw payload traversal from authenticated
    // algebraic combination without changing that public meaning.
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
        static_cast<std::uint64_t>(
            (std::numeric_limits<std::int64_t>::max)());

    /// Add one SQL-bound value and fail closed on overflow.
    void add(
            std::uint64_t& destination,
            std::uint64_t increment) noexcept {
        if (destination > k_sql_bigint_max
            || increment > k_sql_bigint_max -
                std::min(destination, k_sql_bigint_max)) {
            destination = k_sql_bigint_max;
            are_stats_saturated = true;
            return;
        }
        destination += increment;
    }

    /// Saturating merge used for policy, source and sidecar stage deltas.
    void merge(const cold_index_preparation_stats& rhs) noexcept {
        are_stats_saturated =
            are_stats_saturated || rhs.are_stats_saturated;
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
        add(extent_crc_raw_bytes_hashed,
            rhs.extent_crc_raw_bytes_hashed);
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

    [[nodiscard]] constexpr bool operator==(
        const cold_index_preparation_stats&) const noexcept = default;
};

/// Forward declarations required by the mutual friend-based authorization
/// cycle between executor context and catalog source authority.
struct minimizer_generation_administration_identity;
class minimizer_generation_catalog_source_authorization;
/// Authorize one catalog source against an already registered snapshot.
[[nodiscard]] inline minimizer_generation_catalog_source_authorization authorize_minimizer_generation_catalog_source_pg(
    const minimizer_generation_administration_identity&, std::uint64_t, Snapshot);

/// Move-only, non-owning executor read capability.  The CustomScan owns the
/// Relation lock and the registered MVCC snapshot; cold-index preparation may
/// borrow both only while this token remains valid.  Moving the token transfers
/// the right to borrow, but neither move nor destruction performs PostgreSQL
/// work (in particular, it never closes the Relation or unregisters the
/// snapshot).
class borrowed_executor_read_context {
private:
    friend minimizer_generation_catalog_source_authorization authorize_minimizer_generation_catalog_source_pg(
        const minimizer_generation_administration_identity&, std::uint64_t, Snapshot);

    Relation relation_ = nullptr;
    Snapshot snapshot_ = nullptr;
    Oid relation_oid_ = InvalidOid;
    Oid database_oid_ = InvalidOid;
    Oid tablespace_oid_ = InvalidOid;
    Oid relfilenode_ = InvalidOid;

    void invalidate_() noexcept {
        relation_ = nullptr;
        snapshot_ = nullptr;
        relation_oid_ = InvalidOid;
        database_oid_ = InvalidOid;
        tablespace_oid_ = InvalidOid;
        relfilenode_ = InvalidOid;
    }

    borrowed_executor_read_context(
            Relation relation,
            Snapshot registered_snapshot) noexcept
        : relation_(relation),
          snapshot_(registered_snapshot),
          relation_oid_(relation == nullptr
              ? InvalidOid : RelationGetRelid(relation)),
          database_oid_(relation == nullptr
              ? InvalidOid : relation->rd_locator.dbOid),
          tablespace_oid_(relation == nullptr
              ? InvalidOid : relation->rd_locator.spcOid),
          relfilenode_(relation == nullptr
              ? InvalidOid
              : static_cast<Oid>(relation->rd_locator.relNumber)) {}

public:
    borrowed_executor_read_context() = delete;

    [[nodiscard]] static std::optional<borrowed_executor_read_context>
    bind_checked(
            Relation relation,
            Snapshot registered_snapshot) noexcept {
        borrowed_executor_read_context context{
            relation, registered_snapshot};
        if (!context.valid()) return std::nullopt;
        return std::optional<borrowed_executor_read_context>{
            std::move(context)};
    }

    borrowed_executor_read_context(
        const borrowed_executor_read_context&) = delete;
    borrowed_executor_read_context& operator=(
        const borrowed_executor_read_context&) = delete;
    borrowed_executor_read_context(
            borrowed_executor_read_context&& other) noexcept
        : relation_(std::exchange(other.relation_, nullptr)),
          snapshot_(std::exchange(other.snapshot_, nullptr)),
          relation_oid_(std::exchange(other.relation_oid_, InvalidOid)),
          database_oid_(std::exchange(other.database_oid_, InvalidOid)),
          tablespace_oid_(std::exchange(other.tablespace_oid_, InvalidOid)),
          relfilenode_(std::exchange(other.relfilenode_, InvalidOid)) {}
    borrowed_executor_read_context& operator=(
            borrowed_executor_read_context&& other) noexcept {
        if (this != &other) {
            invalidate_();
            relation_ = std::exchange(other.relation_, nullptr);
            snapshot_ = std::exchange(other.snapshot_, nullptr);
            relation_oid_ = std::exchange(other.relation_oid_, InvalidOid);
            database_oid_ = std::exchange(other.database_oid_, InvalidOid);
            tablespace_oid_ = std::exchange(other.tablespace_oid_, InvalidOid);
            relfilenode_ = std::exchange(other.relfilenode_, InvalidOid);
        }
        return *this;
    }
    ~borrowed_executor_read_context() = default;

    [[nodiscard]] bool valid() const noexcept {
        return relation_ != nullptr
            && snapshot_ != nullptr
            && IsMVCCSnapshot(snapshot_)
            && snapshot_->regd_count > 0
            && OidIsValid(relation_oid_)
            && OidIsValid(database_oid_)
            && OidIsValid(tablespace_oid_)
            && OidIsValid(relfilenode_)
            && RelationGetRelid(relation_) == relation_oid_
            && relation_->rd_locator.dbOid == database_oid_
            && relation_->rd_locator.spcOid == tablespace_oid_
            && static_cast<Oid>(relation_->rd_locator.relNumber)
                == relfilenode_;
    }

    [[nodiscard]] bool matches(
            Oid database_oid,
            Oid tablespace_oid,
            Oid relfilenode,
            Oid relation_oid) const noexcept {
        return valid()
            && database_oid_ == database_oid
            && tablespace_oid_ == tablespace_oid
            && relfilenode_ == relfilenode
            && relation_oid_ == relation_oid;
    }

    [[nodiscard]] Relation relation() const noexcept { return relation_; }
    [[nodiscard]] Snapshot snapshot() const noexcept { return snapshot_; }
    [[nodiscard]] Oid relation_oid() const noexcept { return relation_oid_; }
    [[nodiscard]] Oid database_oid() const noexcept { return database_oid_; }
    [[nodiscard]] Oid tablespace_oid() const noexcept { return tablespace_oid_; }
    [[nodiscard]] Oid relfilenode() const noexcept { return relfilenode_; }

};

static_assert(!std::is_copy_constructible_v<borrowed_executor_read_context>);
static_assert(std::is_nothrow_move_constructible_v<
    borrowed_executor_read_context>);

/// Monotonic project clock converted to the unsigned SQL-bound domain.
[[nodiscard]] inline std::uint64_t cold_index_now_ns() noexcept {
    return bioseq::instrumentation::clock::now_ns();
}

/// Add one elapsed interval while defending against a broken clock sample.
inline void add_cold_index_elapsed(
        cold_index_preparation_stats& stats,
        std::uint64_t& destination,
        std::uint64_t begin_ns) noexcept {
    const std::uint64_t end_ns = cold_index_now_ns();
    stats.add(destination, end_ns >= begin_ns ? end_ns - begin_ns : 0);
}

/// Attribute PostgreSQL shared-buffer traffic since `begin` to this delta.
inline void add_cold_index_buffer_usage(
        cold_index_preparation_stats& stats,
        const BufferUsage& begin) noexcept {
    BufferUsage delta{};
    BufferUsageAccumDiff(&delta, &pgBufferUsage, &begin);
    if (delta.shared_blks_hit < 0 || delta.shared_blks_read < 0) {
        stats.are_stats_saturated = true;
        return;
    }
    stats.add(stats.shared_blks_hit,
        static_cast<std::uint64_t>(delta.shared_blks_hit));
    stats.add(stats.shared_blks_read,
        static_cast<std::uint64_t>(delta.shared_blks_read));
}

inline std::uint64_t g_last_index_merge_run_count = 0;
inline last_minimizer_sequence_build_stats
    g_last_minimizer_sequence_build_stats{};

/// Reset backend-local build counters before a new administrative build.
inline void reset_last_minimizer_sequence_build_stats() noexcept {
    g_last_index_merge_run_count = 0;
    g_last_minimizer_sequence_build_stats = {};
}

/// Test-only failure points spanning generation write and read lifecycles.
enum class minimizer_generation_failure_injection : std::uint8_t {
    none,
    minimizer_sequence_spill,
    minimizer_sequence_merge,
    generation_tts_create,
    generation_tuple_form,
    generation_tts_store,
    generation_tts_drop,
    generation_multi_insert,
    generation_reread,
    generation_spi_publish,
    generation_writer_close,
    generation_storage_close,
    generation_post_publish_reread,
    generation_reader_close,
};

#ifdef BIOSEQ_TEST_INDEXES
inline minimizer_generation_failure_injection g_minimizer_generation_failure_injection =
    minimizer_generation_failure_injection::none;

/// Arm one generation failure point for the next matching operation.
inline void arm_minimizer_generation_failure_injection(
        minimizer_generation_failure_injection point) noexcept {
    g_minimizer_generation_failure_injection = point;
}

/// Clear any armed generation failure point.
inline void disarm_minimizer_generation_failure_injection() noexcept {
    g_minimizer_generation_failure_injection = minimizer_generation_failure_injection::none;
}

/// Raise an armed failure from inside an existing PostgreSQL error boundary.
/// Composite PostgreSQL operations use this leaf to avoid nesting a second
/// context-restoring boundary.
inline void inject_minimizer_generation_failure_leaf(
        minimizer_generation_failure_injection point,
        const char* point_name) {
    if (g_minimizer_generation_failure_injection != point) return;
    g_minimizer_generation_failure_injection = minimizer_generation_failure_injection::none;
    ereport(ERROR,
        (errcode(ERRCODE_IO_ERROR),
         errmsg("injected PostgreSQL generation failure"),
         errdetail("failure_point=%s", point_name),
         errhint("retry the failed index build in a fresh transaction")));
}

/// Translate an armed PostgreSQL failure into the C++ boundary representation.
inline void inject_minimizer_generation_failure_as_cpp(
        minimizer_generation_failure_injection point,
        const char* point_name) {
    bioseq::postgres::support::invoke_pg_as_cpp([point, point_name] {
        inject_minimizer_generation_failure_leaf(point, point_name);
    });
}
#else
inline void arm_minimizer_generation_failure_injection(
        minimizer_generation_failure_injection) noexcept {}
inline void disarm_minimizer_generation_failure_injection() noexcept {}
inline void inject_minimizer_generation_failure_leaf(
        minimizer_generation_failure_injection,
        const char*) {}
inline void inject_minimizer_generation_failure_as_cpp(
        minimizer_generation_failure_injection,
        const char*) {}
#endif

/// Test-only PostgreSQL leaf failures for inline sequence reads.
enum class inline_sequence_pg_leaf_failure : std::uint8_t {
	none,
	relation_lookup,
	tuple_read,
	detoast,
	close,
};

[[nodiscard]] constexpr const char* inline_sequence_pg_leaf_failure_name(
		const inline_sequence_pg_leaf_failure point) noexcept {
	switch (point) {
		case inline_sequence_pg_leaf_failure::none:
			return "none";
		case inline_sequence_pg_leaf_failure::relation_lookup:
			return "relation_lookup";
		case inline_sequence_pg_leaf_failure::tuple_read:
			return "tuple_read";
		case inline_sequence_pg_leaf_failure::detoast:
			return "detoast";
		case inline_sequence_pg_leaf_failure::close:
			return "close";
	}
	return "invalid";
}

#ifdef BIOSEQ_TEST_INDEXES
inline inline_sequence_pg_leaf_failure g_inline_sequence_pg_leaf_failure =
	inline_sequence_pg_leaf_failure::none;

/// Arm one inline sequence leaf failure.
inline void arm_inline_sequence_pg_leaf_failure(
		const inline_sequence_pg_leaf_failure point) noexcept {
	g_inline_sequence_pg_leaf_failure = point;
}

/// Clear the armed inline sequence leaf failure.
inline void disarm_inline_sequence_pg_leaf_failure() noexcept {
	g_inline_sequence_pg_leaf_failure = inline_sequence_pg_leaf_failure::none;
}

/// Raise and consume an inline sequence failure inside a PostgreSQL leaf.
/// Consumption makes the next statement a real recovery probe.
inline void inject_inline_sequence_pg_leaf_failure(
		const inline_sequence_pg_leaf_failure point) {
	if (g_inline_sequence_pg_leaf_failure != point) {
		return;
	}
	g_inline_sequence_pg_leaf_failure = inline_sequence_pg_leaf_failure::none;
	ereport(ERROR,
		(errcode(ERRCODE_IO_ERROR),
		 errmsg("injected inline sequence storage PostgreSQL leaf failure"),
		 errdetail("failure_point=%s",
			inline_sequence_pg_leaf_failure_name(point)),
		 errhint("retry the symbol-count read in a fresh statement")));
}
#else
inline void arm_inline_sequence_pg_leaf_failure(
		inline_sequence_pg_leaf_failure) noexcept {}
inline void disarm_inline_sequence_pg_leaf_failure() noexcept {}
inline void inject_inline_sequence_pg_leaf_failure(
		inline_sequence_pg_leaf_failure) noexcept {}
#endif

/// Query-key attribute in a fragment-chunk tuple.
inline constexpr AttrNumber k_attribute_query_key   = 1;
/// Sub-key attribute in a fragment-chunk tuple.
inline constexpr AttrNumber k_attribute_sub_key     = 2;
/// Chunk-number attribute in a fragment-chunk tuple.
inline constexpr AttrNumber k_attribute_chunk_number    = 3;
/// Logical byte-count attribute in a fragment-chunk tuple.
inline constexpr AttrNumber k_attribute_chunk_bytes = 4;
/// Payload attribute in a fragment-chunk tuple.
inline constexpr AttrNumber k_attribute_blob       = 5;

/// Query-key attribute in the frozen generation-row layout.
inline constexpr AttrNumber k_minimizer_generation_attribute_query_key       = 1;
/// Generation identifier attribute in the frozen generation-row layout.
inline constexpr AttrNumber k_minimizer_generation_attribute_generation_id   = 2;
/// Position-segment attribute in the frozen generation-row layout.
inline constexpr AttrNumber k_minimizer_generation_attribute_position_segment = 3;
/// Sub-key attribute in the frozen generation-row layout.
inline constexpr AttrNumber k_minimizer_generation_attribute_sub_key         = 4;
/// Chunk-number attribute in the frozen generation-row layout.
inline constexpr AttrNumber k_minimizer_generation_attribute_chunk_number        = 5;
/// Logical byte-count attribute in the frozen generation-row layout.
inline constexpr AttrNumber k_minimizer_generation_attribute_chunk_bytes     = 6;
/// Address-bound checksum attribute in the frozen generation-row layout.
inline constexpr AttrNumber k_minimizer_generation_attribute_checksum       = 7;
/// PLAIN payload attribute in the frozen generation-row layout.
inline constexpr AttrNumber k_minimizer_generation_attribute_blob           = 8;

// systable_beginscan_ordered() accepts HEAP attribute numbers, then remaps
// each key through pg_index.indkey before handing the scan to the index AM.
// Keep the call sites in primary-key order while retaining the unchanged
// eight-column heap tuple's physical attribute numbers. Passing index
// ordinals here happens to look ordered to C++, but PostgreSQL remaps them a
// second time and gives btree an out-of-order key sequence.
inline constexpr AttrNumber k_minimizer_generation_index_generation_id =
    k_minimizer_generation_attribute_generation_id;
inline constexpr AttrNumber k_minimizer_generation_index_query_key =
    k_minimizer_generation_attribute_query_key;
inline constexpr AttrNumber k_minimizer_generation_index_sub_key =
    k_minimizer_generation_attribute_sub_key;
inline constexpr AttrNumber k_minimizer_generation_index_position_segment =
    k_minimizer_generation_attribute_position_segment;
inline constexpr AttrNumber k_minimizer_generation_index_chunk_number =
    k_minimizer_generation_attribute_chunk_number;

/// Expected name, type, and heap attribute number for one generation column.
struct minimizer_generation_index_attribute_descriptor {
    AttrNumber attno = InvalidAttrNumber;
    std::string_view name;
    Oid type_oid = InvalidOid;
};

/// Complete expected descriptor for the frozen eight-column relation.
inline constexpr std::array<minimizer_generation_index_attribute_descriptor, 8>
k_minimizer_generation_index_attribute_descriptors{{
    {k_minimizer_generation_attribute_query_key, "query_key", INT8OID},
    {k_minimizer_generation_attribute_generation_id, "generation_id", INT8OID},
    {k_minimizer_generation_attribute_position_segment, "position_segment", INT4OID},
    {k_minimizer_generation_attribute_sub_key, "sub_key", INT8OID},
    {k_minimizer_generation_attribute_chunk_number, "chunk_no", INT4OID},
    {k_minimizer_generation_attribute_chunk_bytes, "chunk_bytes", INT4OID},
    {k_minimizer_generation_attribute_checksum, "checksum", INT8OID},
    {k_minimizer_generation_attribute_blob, "blob", BYTEAOID},
}};

/// Heap attribute numbers in the exact primary-key scan order.
inline constexpr std::array<AttrNumber, 5> k_minimizer_generation_primary_key_attributes{
    k_minimizer_generation_attribute_generation_id,
    k_minimizer_generation_attribute_query_key,
    k_minimizer_generation_attribute_sub_key,
    k_minimizer_generation_attribute_position_segment,
    k_minimizer_generation_attribute_chunk_number,
};

static_assert(k_minimizer_generation_index_attribute_descriptors.front().attno == 1);
static_assert(k_minimizer_generation_index_attribute_descriptors.back().attno == 8);
static_assert(k_minimizer_generation_primary_key_attributes.size() == 5);
static_assert(k_minimizer_generation_index_generation_id == k_minimizer_generation_primary_key_attributes[0]);
static_assert(k_minimizer_generation_index_query_key == k_minimizer_generation_primary_key_attributes[1]);
static_assert(k_minimizer_generation_index_sub_key == k_minimizer_generation_primary_key_attributes[2]);
static_assert(k_minimizer_generation_index_position_segment ==
    k_minimizer_generation_primary_key_attributes[3]);
static_assert(k_minimizer_generation_index_chunk_number == k_minimizer_generation_primary_key_attributes[4]);

/// Preserve all CRC-64 bits in PostgreSQL's signed BIGINT container.
[[nodiscard]] constexpr std::int64_t checksum_to_pg_bigint(
        std::uint64_t checksum) noexcept {
    return std::bit_cast<std::int64_t>(checksum);
}

/// Recover the exact CRC-64 word from PostgreSQL's signed BIGINT container.
[[nodiscard]] constexpr std::uint64_t checksum_from_pg_bigint(
        std::int64_t checksum) noexcept {
    return std::bit_cast<std::uint64_t>(checksum);
}

static_assert(checksum_from_pg_bigint(checksum_to_pg_bigint(0)) == 0);
static_assert(checksum_from_pg_bigint(checksum_to_pg_bigint(
    (std::numeric_limits<std::uint64_t>::max)()))
    == (std::numeric_limits<std::uint64_t>::max)());
static_assert(checksum_to_pg_bigint(std::uint64_t{1} << 63u)
    == (std::numeric_limits<std::int64_t>::min)());

/// Exact schema mismatch observed while binding a generation relation.
enum class minimizer_generation_schema_error : std::uint8_t {
    none,
    not_heap,
    attribute_count,
    attribute_number,
    attribute_dropped,
    attribute_name,
    attribute_type,
    attribute_options,
    attribute_nullable,
    blob_not_plain,
    wrong_namespace,
    wrong_relation_name,
    wrong_heap_persistence,
    wrong_index_persistence,
    wrong_heap_tablespace,
    wrong_index_tablespace,
    wrong_heap_fillfactor,
    wrong_index_fillfactor,
    primary_index_count,
    primary_index_not_unique,
    primary_index_not_immediate,
    primary_index_not_ready,
    primary_index_key_count,
    primary_index_key_order,
    primary_index_not_btree,
    primary_index_partial,
    primary_index_expression,
    primary_index_opclass,
    primary_index_collation,
    primary_index_options,
};

/// Schema inspection result and the validated primary-index OID.
struct minimizer_generation_schema_inspection {
    minimizer_generation_schema_error error = minimizer_generation_schema_error::none;
    Oid primary_index_oid = InvalidOid;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == minimizer_generation_schema_error::none
            && OidIsValid(primary_index_oid);
    }
};

/// Raise a generation-storage PostgreSQL error through the C++ boundary.
[[noreturn]] inline void minimizer_generation_storage_error(std::string_view message);

[[nodiscard]] constexpr std::string_view minimizer_generation_schema_error_name(
        minimizer_generation_schema_error error) noexcept {
    switch (error) {
        case minimizer_generation_schema_error::none: return "none";
        case minimizer_generation_schema_error::not_heap: return "relation is not heap AM";
        case minimizer_generation_schema_error::attribute_count: return "attribute count";
        case minimizer_generation_schema_error::attribute_number: return "attribute number";
        case minimizer_generation_schema_error::attribute_dropped: return "dropped attribute";
        case minimizer_generation_schema_error::attribute_name: return "attribute name";
        case minimizer_generation_schema_error::attribute_type: return "attribute type";
        case minimizer_generation_schema_error::attribute_options: return "attribute options";
        case minimizer_generation_schema_error::attribute_nullable: return "nullable attribute";
        case minimizer_generation_schema_error::blob_not_plain: return "blob storage is not PLAIN";
        case minimizer_generation_schema_error::wrong_namespace: return "relation namespace";
        case minimizer_generation_schema_error::wrong_relation_name: return "relation name";
        case minimizer_generation_schema_error::wrong_heap_persistence: return "heap is not UNLOGGED";
        case minimizer_generation_schema_error::wrong_index_persistence: return "primary index is not UNLOGGED";
        case minimizer_generation_schema_error::wrong_heap_tablespace: return "heap tablespace";
        case minimizer_generation_schema_error::wrong_index_tablespace: return "primary index tablespace";
        case minimizer_generation_schema_error::wrong_heap_fillfactor: return "heap fillfactor";
        case minimizer_generation_schema_error::wrong_index_fillfactor: return "primary index fillfactor";
        case minimizer_generation_schema_error::primary_index_count: return "primary index count";
        case minimizer_generation_schema_error::primary_index_not_unique: return "primary index is not unique";
        case minimizer_generation_schema_error::primary_index_not_immediate: return "primary index is deferred";
        case minimizer_generation_schema_error::primary_index_not_ready: return "primary index is not ready/valid/live";
        case minimizer_generation_schema_error::primary_index_key_count: return "primary index key count";
        case minimizer_generation_schema_error::primary_index_key_order: return "primary index key order";
        case minimizer_generation_schema_error::primary_index_not_btree: return "primary index is not btree";
        case minimizer_generation_schema_error::primary_index_partial: return "primary index is partial";
        case minimizer_generation_schema_error::primary_index_expression: return "primary index has expressions";
        case minimizer_generation_schema_error::primary_index_opclass: return "primary index opclass";
        case minimizer_generation_schema_error::primary_index_collation: return "primary index collation";
        case minimizer_generation_schema_error::primary_index_options: return "primary index ordering/opclass options";
    }
    return "unknown";
}

/// Optional identity constraints applied during relation-shape validation.
struct minimizer_generation_schema_expectation {
    Oid namespace_oid = InvalidOid;
    std::string_view relation_name{};
    Oid tablespace_oid = InvalidOid;
};

/// Inspect the complete current-generation relation shape without accepting attno
/// shims, included PK columns, nullable fields, or a toastable blob.
[[nodiscard]] inline minimizer_generation_schema_inspection inspect_minimizer_generation_schema(
        Relation relation,
        cold_index_preparation_stats* cold_stats = nullptr,
        minimizer_generation_schema_expectation expected = {},
        Oid permitted_auxiliary_index_oid = InvalidOid) {
    if (relation == nullptr || relation->rd_rel->relam != HEAP_TABLE_AM_OID
        || relation->rd_rel->relkind != RELKIND_RELATION) {
        return {.error = minimizer_generation_schema_error::not_heap};
    }
    if (OidIsValid(expected.namespace_oid)
        && RelationGetNamespace(relation) != expected.namespace_oid) {
        return {.error = minimizer_generation_schema_error::wrong_namespace};
    }
    if (!expected.relation_name.empty()
        && std::string_view{RelationGetRelationName(relation)}
            != expected.relation_name) {
        return {.error = minimizer_generation_schema_error::wrong_relation_name};
    }
    if (relation->rd_rel->relpersistence != RELPERSISTENCE_UNLOGGED) {
        return {.error = minimizer_generation_schema_error::wrong_heap_persistence};
    }
    if (RelationGetFillFactor(relation, HEAP_DEFAULT_FILLFACTOR) != 100) {
        return {.error = minimizer_generation_schema_error::wrong_heap_fillfactor};
    }
    if (OidIsValid(expected.tablespace_oid)
        && relation->rd_locator.spcOid != expected.tablespace_oid) {
        return {.error = minimizer_generation_schema_error::wrong_heap_tablespace};
    }
    TupleDesc tuple_desc = RelationGetDescr(relation);
    if (tuple_desc == nullptr
        || tuple_desc->natts
            != static_cast<int>(k_minimizer_generation_index_attribute_descriptors.size())) {
        return {.error = minimizer_generation_schema_error::attribute_count};
    }
    for (std::size_t index = 0;
         index < k_minimizer_generation_index_attribute_descriptors.size(); ++index) {
        const minimizer_generation_index_attribute_descriptor& expected =
            k_minimizer_generation_index_attribute_descriptors[index];
        Form_pg_attribute attribute = TupleDescAttr(
            tuple_desc, static_cast<int>(index));
        if (attribute->attnum != expected.attno) {
            return {.error = minimizer_generation_schema_error::attribute_number};
        }
        if (attribute->attisdropped) {
            return {.error = minimizer_generation_schema_error::attribute_dropped};
        }
        if (std::string_view{NameStr(attribute->attname)} != expected.name) {
            return {.error = minimizer_generation_schema_error::attribute_name};
        }
        if (attribute->atttypid != expected.type_oid) {
            return {.error = minimizer_generation_schema_error::attribute_type};
        }
        if (attribute->atttypmod != -1
            || OidIsValid(attribute->attcollation)
            || attribute->atthasdef
            || attribute->atthasmissing
            || attribute->attidentity != '\0'
            || attribute->attgenerated != '\0'
            || attribute->attcompression != '\0') {
            return {.error = minimizer_generation_schema_error::attribute_options};
        }
        if (!attribute->attnotnull) {
            return {.error = minimizer_generation_schema_error::attribute_nullable};
        }
        if (attribute->attstorage != TYPSTORAGE_PLAIN) {
            return {.error = minimizer_generation_schema_error::blob_not_plain};
        }
    }

    Oid primary_index_oid = InvalidOid;
    std::uint32_t index_count = 0;
    std::uint32_t primary_index_count = 0;
    minimizer_generation_schema_error primary_error = minimizer_generation_schema_error::none;
    List* index_list = RelationGetIndexList(relation);
    ListCell* cell = nullptr;
    foreach (cell, index_list) {
        const Oid index_oid = lfirst_oid(cell);
        // Production callers leave this invalid and retain the exact-one-index
        // contract.  The current-generation catalog-corruption fixture alone names its
        // real donor index, which must coexist long enough for its parsed
        // predicate/expression tree to be copied onto the primary index.
        if (index_oid == permitted_auxiliary_index_oid) continue;
        ++index_count;
        Relation index_relation = index_open(index_oid, AccessShareLock);
        if (cold_stats != nullptr) {
            cold_stats->add(cold_stats->sidecar_index_opens, 1);
            cold_stats->add(cold_stats->lock_acquires, 1);
        }
        if (index_relation->rd_index != nullptr
            && index_relation->rd_index->indisprimary) {
            ++primary_index_count;
            primary_index_oid = index_oid;
            const Form_pg_index index_form = index_relation->rd_index;
            if (!index_form->indisunique) {
                primary_error = minimizer_generation_schema_error::primary_index_not_unique;
            } else if (index_form->indnullsnotdistinct) {
                primary_error = minimizer_generation_schema_error::primary_index_options;
            } else if (!index_form->indimmediate) {
                primary_error = minimizer_generation_schema_error::primary_index_not_immediate;
            } else if (!index_form->indisvalid || !index_form->indisready
                       || !index_form->indislive) {
                primary_error = minimizer_generation_schema_error::primary_index_not_ready;
            } else if (index_form->indnatts
                           != static_cast<std::int16_t>(
                               k_minimizer_generation_primary_key_attributes.size())
                       || index_form->indnkeyatts
                           != static_cast<std::int16_t>(
                               k_minimizer_generation_primary_key_attributes.size())) {
                primary_error = minimizer_generation_schema_error::primary_index_key_count;
            } else if (index_relation->rd_rel->relam != BTREE_AM_OID) {
                primary_error = minimizer_generation_schema_error::primary_index_not_btree;
            } else if (index_relation->rd_rel->relpersistence
                       != RELPERSISTENCE_UNLOGGED) {
                primary_error = minimizer_generation_schema_error::wrong_index_persistence;
            } else if (index_relation->rd_locator.spcOid
                       != relation->rd_locator.spcOid
                       || (OidIsValid(expected.tablespace_oid)
                           && index_relation->rd_locator.spcOid
                               != expected.tablespace_oid)) {
                primary_error = minimizer_generation_schema_error::wrong_index_tablespace;
            } else if (RelationGetFillFactor(index_relation,
                           BTREE_DEFAULT_FILLFACTOR) != 90) {
                primary_error = minimizer_generation_schema_error::wrong_index_fillfactor;
            } else if (RelationGetIndexPredicate(index_relation) != NIL) {
                primary_error = minimizer_generation_schema_error::primary_index_partial;
            } else if (RelationGetIndexExpressions(index_relation) != NIL) {
                primary_error = minimizer_generation_schema_error::primary_index_expression;
            } else {
                constexpr std::array<Oid, 5> k_expected_opclasses{
                    INT8_BTREE_OPS_OID,
                    INT8_BTREE_OPS_OID,
                    INT8_BTREE_OPS_OID,
                    INT4_BTREE_OPS_OID,
                    INT4_BTREE_OPS_OID,
                };
                for (std::size_t key = 0;
                     key < k_minimizer_generation_primary_key_attributes.size(); ++key) {
                    if (index_form->indkey.values[key]
                        != k_minimizer_generation_primary_key_attributes[key]) {
                        primary_error = minimizer_generation_schema_error::primary_index_key_order;
                        break;
                    }
                    if (get_index_column_opclass(
                            index_oid, static_cast<int>(key + 1u))
                        != k_expected_opclasses[key]) {
                        primary_error = minimizer_generation_schema_error::primary_index_opclass;
                        break;
                    }
                    if (index_relation->rd_indcollation == nullptr
                        || OidIsValid(index_relation->rd_indcollation[key])) {
                        primary_error = minimizer_generation_schema_error::primary_index_collation;
                        break;
                    }
                    if (index_relation->rd_indoption == nullptr
                        || index_relation->rd_indoption[key] != 0
                        || (index_relation->rd_opcoptions != nullptr
                            && index_relation->rd_opcoptions[key] != nullptr)) {
                        primary_error = minimizer_generation_schema_error::primary_index_options;
                        break;
                    }
                }
            }
        }
        index_close(index_relation, AccessShareLock);
        if (cold_stats != nullptr) {
            cold_stats->add(cold_stats->lock_releases, 1);
        }
    }
    list_free(index_list);
    if (index_count != 1 || primary_index_count != 1) {
        return {.error = minimizer_generation_schema_error::primary_index_count};
    }
    if (primary_error != minimizer_generation_schema_error::none) {
        return {.error = primary_error};
    }
    return {
        .error = minimizer_generation_schema_error::none,
        .primary_index_oid = primary_index_oid,
    };
}

/// Validate a current-generation heap and its already-open primary-index descriptor.
/// Unlike inspect_minimizer_generation_schema(), this routine performs no Relation open
/// or close.  It is the production cold-read validator: the exact descriptor
/// validated here is the descriptor subsequently used by every ordered scan.
[[nodiscard]] inline minimizer_generation_schema_inspection
inspect_bound_minimizer_generation_schema(
        Relation relation,
        Relation primary_index_relation,
        minimizer_generation_schema_expectation expected = {}) {
    if (relation == nullptr || relation->rd_rel->relam != HEAP_TABLE_AM_OID
        || relation->rd_rel->relkind != RELKIND_RELATION) {
        return {.error = minimizer_generation_schema_error::not_heap};
    }
    if (OidIsValid(expected.namespace_oid)
        && RelationGetNamespace(relation) != expected.namespace_oid) {
        return {.error = minimizer_generation_schema_error::wrong_namespace};
    }
    if (!expected.relation_name.empty()
        && std::string_view{RelationGetRelationName(relation)}
            != expected.relation_name) {
        return {.error = minimizer_generation_schema_error::wrong_relation_name};
    }
    if (relation->rd_rel->relpersistence != RELPERSISTENCE_UNLOGGED) {
        return {.error = minimizer_generation_schema_error::wrong_heap_persistence};
    }
    if (RelationGetFillFactor(relation, HEAP_DEFAULT_FILLFACTOR) != 100) {
        return {.error = minimizer_generation_schema_error::wrong_heap_fillfactor};
    }
    if (OidIsValid(expected.tablespace_oid)
        && relation->rd_locator.spcOid != expected.tablespace_oid) {
        return {.error = minimizer_generation_schema_error::wrong_heap_tablespace};
    }
    TupleDesc tuple_desc = RelationGetDescr(relation);
    if (tuple_desc == nullptr
        || tuple_desc->natts
            != static_cast<int>(k_minimizer_generation_index_attribute_descriptors.size())) {
        return {.error = minimizer_generation_schema_error::attribute_count};
    }
    for (std::size_t index = 0;
         index < k_minimizer_generation_index_attribute_descriptors.size(); ++index) {
        const minimizer_generation_index_attribute_descriptor& expected =
            k_minimizer_generation_index_attribute_descriptors[index];
        Form_pg_attribute attribute = TupleDescAttr(
            tuple_desc, static_cast<int>(index));
        if (attribute->attnum != expected.attno) {
            return {.error = minimizer_generation_schema_error::attribute_number};
        }
        if (attribute->attisdropped) {
            return {.error = minimizer_generation_schema_error::attribute_dropped};
        }
        if (std::string_view{NameStr(attribute->attname)} != expected.name) {
            return {.error = minimizer_generation_schema_error::attribute_name};
        }
        if (attribute->atttypid != expected.type_oid) {
            return {.error = minimizer_generation_schema_error::attribute_type};
        }
        if (attribute->atttypmod != -1
            || OidIsValid(attribute->attcollation)
            || attribute->atthasdef
            || attribute->atthasmissing
            || attribute->attidentity != '\0'
            || attribute->attgenerated != '\0'
            || attribute->attcompression != '\0') {
            return {.error = minimizer_generation_schema_error::attribute_options};
        }
        if (!attribute->attnotnull) {
            return {.error = minimizer_generation_schema_error::attribute_nullable};
        }
        if (attribute->attstorage != TYPSTORAGE_PLAIN) {
            return {.error = minimizer_generation_schema_error::blob_not_plain};
        }
    }

    List* index_list = RelationGetIndexList(relation);
    const bool exactly_one_index = list_length(index_list) == 1;
    list_free(index_list);
    if (!exactly_one_index || primary_index_relation == nullptr
        || primary_index_relation->rd_index == nullptr) {
        return {.error = minimizer_generation_schema_error::primary_index_count};
    }
    const bool exact_primary = RelationGetPrimaryKeyIndex(relation)
        == RelationGetRelid(primary_index_relation);
    if (!exact_primary
        || primary_index_relation->rd_index->indrelid
            != RelationGetRelid(relation)
        || !primary_index_relation->rd_index->indisprimary) {
        return {.error = minimizer_generation_schema_error::primary_index_count};
    }

    const Form_pg_index index_form = primary_index_relation->rd_index;
    if (!index_form->indisunique) {
        return {.error = minimizer_generation_schema_error::primary_index_not_unique};
    }
    if (index_form->indnullsnotdistinct) {
        return {.error = minimizer_generation_schema_error::primary_index_options};
    }
    if (!index_form->indimmediate) {
        return {.error = minimizer_generation_schema_error::primary_index_not_immediate};
    }
    if (!index_form->indisvalid || !index_form->indisready
        || !index_form->indislive) {
        return {.error = minimizer_generation_schema_error::primary_index_not_ready};
    }
    if (index_form->indnatts
            != static_cast<std::int16_t>(
                k_minimizer_generation_primary_key_attributes.size())
        || index_form->indnkeyatts
            != static_cast<std::int16_t>(
                k_minimizer_generation_primary_key_attributes.size())) {
        return {.error = minimizer_generation_schema_error::primary_index_key_count};
    }
    if (primary_index_relation->rd_rel->relam != BTREE_AM_OID) {
        return {.error = minimizer_generation_schema_error::primary_index_not_btree};
    }
    if (primary_index_relation->rd_rel->relpersistence
        != RELPERSISTENCE_UNLOGGED) {
        return {.error = minimizer_generation_schema_error::wrong_index_persistence};
    }
    if (primary_index_relation->rd_locator.spcOid
            != relation->rd_locator.spcOid
        || (OidIsValid(expected.tablespace_oid)
            && primary_index_relation->rd_locator.spcOid
                != expected.tablespace_oid)) {
        return {.error = minimizer_generation_schema_error::wrong_index_tablespace};
    }
    if (RelationGetFillFactor(primary_index_relation,
            BTREE_DEFAULT_FILLFACTOR) != 90) {
        return {.error = minimizer_generation_schema_error::wrong_index_fillfactor};
    }
    if (RelationGetIndexPredicate(primary_index_relation) != NIL) {
        return {.error = minimizer_generation_schema_error::primary_index_partial};
    }
    if (RelationGetIndexExpressions(primary_index_relation) != NIL) {
        return {.error = minimizer_generation_schema_error::primary_index_expression};
    }
    constexpr std::array<Oid, 5> k_expected_opclasses{
        INT8_BTREE_OPS_OID,
        INT8_BTREE_OPS_OID,
        INT8_BTREE_OPS_OID,
        INT4_BTREE_OPS_OID,
        INT4_BTREE_OPS_OID,
    };
    const Oid primary_index_oid = RelationGetRelid(primary_index_relation);
    for (std::size_t key = 0;
         key < k_minimizer_generation_primary_key_attributes.size(); ++key) {
        if (index_form->indkey.values[key]
            != k_minimizer_generation_primary_key_attributes[key]) {
            return {.error = minimizer_generation_schema_error::primary_index_key_order};
        }
        if (get_index_column_opclass(
                primary_index_oid, static_cast<int>(key + 1u))
            != k_expected_opclasses[key]) {
            return {.error = minimizer_generation_schema_error::primary_index_opclass};
        }
        if (primary_index_relation->rd_indcollation == nullptr
            || OidIsValid(primary_index_relation->rd_indcollation[key])) {
            return {.error = minimizer_generation_schema_error::primary_index_collation};
        }
        if (primary_index_relation->rd_indoption == nullptr
            || primary_index_relation->rd_indoption[key] != 0
            || (primary_index_relation->rd_opcoptions != nullptr
                && primary_index_relation->rd_opcoptions[key] != nullptr)) {
            return {.error = minimizer_generation_schema_error::primary_index_options};
        }
    }
    return {
        .error = minimizer_generation_schema_error::none,
        .primary_index_oid = primary_index_oid,
    };
}

/// Classification of a discovered generation relation.
enum class minimizer_generation_relation_kind : std::uint8_t {
    missing,
    current_exact,
    malformed,
};

/// Relation classification paired with its exact schema failure.
struct minimizer_generation_relation_inspection {
    minimizer_generation_relation_kind kind =
        minimizer_generation_relation_kind::malformed;
    minimizer_generation_schema_error schema_error =
        minimizer_generation_schema_error::none;
};

/// Non-PostgreSQL control-flow result used only while binding a production
/// read session.  The binder closes every normally-opened descriptor before
/// throwing it; PostgreSQL errors remain postgres_error and are left to the
/// ResourceOwner/transaction abort path.
struct minimizer_generation_relation_open_failure {
    minimizer_generation_relation_inspection inspection{};
};

/// Integration-only catalog corruption seam. It copies a real parsed
/// predicate or expression tree from a donor index onto the otherwise-valid
/// primary index row. Tests execute it inside a transaction and roll back,
/// proving PG17's varlena `indpred`/`indexprs` path rather than fabricating a
/// C++ Relation object.
inline void copy_primary_index_shape_pg(
        Oid table_oid,
        Oid donor_index_oid,
        bool copy_predicate) {
    if (!superuser()) {
        minimizer_generation_storage_error("current generation index-shape corruption requires superuser");
    }
    Relation target = bioseq::postgres::support::try_open_postgres_relation(table_oid, AccessShareLock);
    if (target == nullptr) {
        minimizer_generation_storage_error("current generation index-shape target relation is missing");
    }
    const minimizer_generation_schema_inspection schema = inspect_minimizer_generation_schema(
        target, nullptr, {}, donor_index_oid);
    relation_close(target, AccessShareLock);
    if (!schema) {
        minimizer_generation_storage_error("current generation index-shape target schema is not initially valid");
    }
    Relation donor = bioseq::postgres::support::try_open_postgres_relation(donor_index_oid, AccessShareLock);
    if (donor == nullptr || donor->rd_index == nullptr
        || donor->rd_index->indrelid != table_oid
        || (copy_predicate
            ? RelationGetIndexPredicate(donor) == NIL
            : RelationGetIndexExpressions(donor) == NIL)) {
        if (donor != nullptr) relation_close(donor, AccessShareLock);
        minimizer_generation_storage_error("current generation index-shape donor does not carry the requested shape");
    }
    relation_close(donor, AccessShareLock);

    Relation catalog = table_open(IndexRelationId, RowExclusiveLock);
    HeapTuple primary_tuple = SearchSysCacheCopy1(
        INDEXRELID, ObjectIdGetDatum(schema.primary_index_oid));
    HeapTuple donor_tuple = SearchSysCache1(
        INDEXRELID, ObjectIdGetDatum(donor_index_oid));
    if (!HeapTupleIsValid(primary_tuple) || !HeapTupleIsValid(donor_tuple)) {
        if (HeapTupleIsValid(primary_tuple)) heap_freetuple(primary_tuple);
        if (HeapTupleIsValid(donor_tuple)) ReleaseSysCache(donor_tuple);
        table_close(catalog, RowExclusiveLock);
        minimizer_generation_storage_error("current generation index-shape catalog tuple is missing");
    }
    const AttrNumber shape_attno = copy_predicate
        ? Anum_pg_index_indpred : Anum_pg_index_indexprs;
    bool donor_is_null = false;
    const Datum donor_shape = heap_getattr(
        donor_tuple, shape_attno, RelationGetDescr(catalog), &donor_is_null);
    if (donor_is_null) {
        heap_freetuple(primary_tuple);
        ReleaseSysCache(donor_tuple);
        table_close(catalog, RowExclusiveLock);
        minimizer_generation_storage_error("current generation index-shape donor catalog value is NULL");
    }
    std::array<Datum, Natts_pg_index> values{};
    std::array<bool, Natts_pg_index> nulls{};
    std::array<bool, Natts_pg_index> replaces{};
    values[shape_attno - 1] = datumCopy(donor_shape, false, -1);
    replaces[shape_attno - 1] = true;
    HeapTuple modified = heap_modify_tuple(
        primary_tuple, RelationGetDescr(catalog),
        values.data(), nulls.data(), replaces.data());
    CatalogTupleUpdate(catalog, &primary_tuple->t_self, modified);
    heap_freetuple(modified);
    pfree(DatumGetPointer(values[shape_attno - 1]));
    heap_freetuple(primary_tuple);
    ReleaseSysCache(donor_tuple);
    table_close(catalog, RowExclusiveLock);
    CommandCounterIncrement();
    CacheInvalidateRelcacheByRelid(table_oid);
    CacheInvalidateRelcacheByRelid(schema.primary_index_oid);
}

// ---- naming helpers ------------------------------------------------------

/// Compile-time-ish abbreviation for an index name (keeps the physical name
/// inside NAMEDATALEN). Unknown names fall back to a stable short hash.
[[nodiscard]] inline std::string short_index_for(std::string_view name) {
    if (name == k_minimizers_per_fragment_index_name) return "mpf";
    if (name == k_minimizers_per_sequence_index_name) return "mps";
    if (name == k_symbol_counts_index_name)           return "sc";
    if (name == "test_table_rowcount")     return "ttr";
    // generic fallback: first 4 chars + length, still deterministic
    std::string out = "ix";
    for (char c : name) { if (out.size() >= 6) break; if ((c>='a'&&c<='z')||(c>='0'&&c<='9')) out.push_back(c); }
    return out;
}

/// Stable 64-bit FNV-1a over the logical identity tuple, hex-rendered.
[[nodiscard]] inline std::uint64_t identity_hash64(Oid db, Oid spc, Oid relfn,
                                                   std::int16_t attno, std::string_view name) {
    std::uint64_t hash_value = 1469598103934665603ull;
    const auto mix_word = [&hash_value](std::uint64_t word) {
        for (int byte_index = 0; byte_index < 8; ++byte_index) {
            hash_value ^= word & 0xffu;
            hash_value *= 1099511628211ull;
            word >>= 8u;
        }
    };
    mix_word(db);
    mix_word(spc);
    mix_word(relfn);
    mix_word(static_cast<std::uint64_t>(
        static_cast<std::uint16_t>(attno)));
    for (const char character : name) {
        hash_value ^= static_cast<std::uint8_t>(character);
        hash_value *= 1099511628211ull;
    }
    return hash_value;
}

[[noreturn]] inline void derived_index_name_too_long(
        std::string_view name) {
    const char* name_data = name.empty() ? "" : name.data();
    const int name_size = static_cast<int>(std::min(
        name.size(),
        static_cast<std::size_t>((std::numeric_limits<int>::max)())));
    const unsigned long long full_size =
        static_cast<unsigned long long>(name.size());
    bioseq::postgres::support::invoke_pg_as_cpp(
        [name_data, name_size, full_size] {
            ereport(ERROR,
                (errcode(ERRCODE_NAME_TOO_LONG),
                 errmsg("bioseq index: derived name is too long"),
                 errdetail("bytes=%llu name=%.*s",
                    full_size, name_size, name_data)));
        });
    pg_unreachable();
}

/// Format one canonical per-sequence index relation name.
[[nodiscard]] inline std::string format_seqidx_name(std::string_view name, Oid db, Oid spc, Oid relfn,
        std::int16_t attno, std::uint8_t bucket, std::uint32_t shard) {
    std::string out = fmt::format(
        "seqidx_{}_{:016x}_{}_{}", short_index_for(name),
        identity_hash64(db, spc, relfn, attno, name),
        static_cast<unsigned>(bucket), static_cast<unsigned>(shard));
    if (out.size() >= NAMEDATALEN) derived_index_name_too_long(out);
    return out;
}
/// Format one canonical table-wide index relation name.
[[nodiscard]] inline std::string format_seqidxtab_name(std::string_view name, Oid db, Oid spc, Oid relfn,
        std::int16_t attno, std::int32_t shard) {
    std::string out = fmt::format("seqidxtab_{}_{:016x}_{}", short_index_for(name),
                                  identity_hash64(db, spc, relfn, attno, name), shard);
    if (out.size() >= NAMEDATALEN) derived_index_name_too_long(out);
    return out;
}

/// Resolve the internal schema from within an existing PostgreSQL boundary.
[[nodiscard]] inline Oid bioseq_internal_ns_pg_leaf() {
    return get_namespace_oid("bioseq_internal", false);
}

/// Resolve the internal schema through the C++ PostgreSQL boundary.
[[nodiscard]] inline Oid bioseq_internal_ns() {
    return bioseq::postgres::support::invoke_pg_as_cpp(
        [] { return bioseq_internal_ns_pg_leaf(); });
}

/// Runs one DDL statement inside a rollback-safe SQL subtransaction.
inline void run_internal_ddl(const char* ddl) {
    auto subtransaction =
        bioseq::postgres::support::subtransaction::begin();
    bioseq::postgres::support::spi_session spi;
    const int connect_status = spi.connect();
    if (connect_status != SPI_OK_CONNECT) {
        minimizer_generation_storage_error(
            "SPI_connect failed while running index DDL");
    }
    const int execute_status =
        bioseq::postgres::support::invoke_pg_as_cpp(
            [ddl] {
                return SPI_execute(
                    ddl, /*read_only=*/false, /*tcount=*/0);
            });
    if (execute_status != SPI_OK_UTILITY
        && execute_status != SPI_OK_SELECT) {
        minimizer_generation_storage_error(
            "index DDL returned an unexpected SPI status");
    }
    if (spi.finish() != SPI_OK_FINISH) {
        minimizer_generation_storage_error(
            "SPI_finish failed while running index DDL");
    }
    subtransaction.commit();
}

/// Persistence keyword inherited from the user relation (UNLOGGED if parent is).
[[nodiscard]] inline const char* persistence_kw_for(Oid user_relid) {
    char persist = 'p';
    Relation r = bioseq::postgres::support::try_open_postgres_relation(user_relid, AccessShareLock);
    if (r != nullptr) { persist = r->rd_rel->relpersistence; relation_close(r, AccessShareLock); }
    return persist == 'u' ? "UNLOGGED" : "";
}

/// Raise a generation-storage error from within a PostgreSQL leaf.
[[noreturn]] inline void minimizer_generation_storage_error_pg_leaf(
        std::string_view message) {
    ereport(ERROR,
        (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
         errmsg("bioseq current-generation index: %.*s",
            static_cast<int>(message.size()), message.data())));
    pg_unreachable();
}

/// Translate a generation-storage PostgreSQL error for C++ unwinding.
[[noreturn]] inline void minimizer_generation_storage_error_as_cpp(
        std::string_view message) {
    bioseq::postgres::support::invoke_pg_as_cpp(
        [message] { minimizer_generation_storage_error_pg_leaf(message); });
    pg_unreachable();
}

/// Re-enter PostgreSQL only after all owning C++ values have unwound.
[[noreturn]] inline void minimizer_generation_storage_error(std::string_view message) {
    minimizer_generation_storage_error_as_cpp(message);
    pg_unreachable();
}

/// Validate the exact metadata-age-log primary-key descriptor.
inline void validate_metadata_age_log_primary_key(Relation relation) {
    const Oid primary_oid = bioseq::postgres::support::invoke_pg_as_cpp(
        [relation] { return bioseq::postgres::support::find_primary_key_oid(relation); });
    if (!OidIsValid(primary_oid)) {
        minimizer_generation_storage_error(
            "metadata_age_log primary key is missing");
    }
    Relation primary = bioseq::postgres::support::invoke_pg_as_cpp(
        [primary_oid] {
            return index_open(primary_oid, AccessShareLock);
        });
    const Form_pg_index form = primary->rd_index;
    constexpr std::array<AttrNumber, 4> k_expected_columns{
        1, 2, 3, 4};
    bool valid = form != nullptr
        && form->indisvalid
        && form->indisready
        && form->indislive
        && form->indisunique
        && form->indisprimary
        && form->indnkeyatts
            == static_cast<std::int16_t>(
                k_expected_columns.size());
    for (std::size_t index = 0;
         valid && index < k_expected_columns.size();
         ++index) {
        valid =
            form->indkey.values[index]
            == k_expected_columns[index];
    }
    bioseq::postgres::support::invoke_pg_as_cpp(
        [primary] {
            index_close(primary, AccessShareLock);
        });
    if (!valid) {
        minimizer_generation_storage_error(
            "metadata_age_log primary key shape changed");
    }
}

/**
 * Insert or refresh one metadata-age row using the generated row codec as the
 * sole C++/Datum boundary.  SPI owns only the fixed SQL statement; no value is
 * string-formatted into SQL.
 */
[[nodiscard]] inline std::uint64_t
upsert_metadata_age_log_pg(
        Oid user_relid,
        Oid main_relfilenode,
        std::int16_t column_attno,
        std::string_view index_name) {
    const Oid namespace_oid = bioseq_internal_ns();
    const Oid relation_oid = bioseq::postgres::support::invoke_pg_as_cpp(
        [namespace_oid] {
            return get_relname_relid(
                "metadata_age_log", namespace_oid);
        });
    if (!OidIsValid(relation_oid)) {
        minimizer_generation_storage_error(
            "metadata_age_log catalog relation is missing");
    }

    Relation relation = bioseq::postgres::support::invoke_pg_as_cpp(
        [relation_oid] {
            return table_open(relation_oid, RowExclusiveLock);
        });
    MemoryContext operation_context = nullptr;
    try {
        operation_context =
            bioseq::postgres::support::invoke_pg_as_cpp([] {
                return AllocSetContextCreate(
                    CurrentMemoryContext,
                    "metadata_age_log upsert operation",
                    ALLOCSET_SMALL_SIZES);
            });
        if (RelationGetNamespace(relation) != namespace_oid
            || relation->rd_rel->relkind != RELKIND_RELATION
            || std::string_view{RelationGetRelationName(relation)}
                != "metadata_age_log") {
            minimizer_generation_storage_error(
                "metadata_age_log catalog identity changed");
        }
        metadata_age_log_codec::validate_descriptor(
            RelationGetDescr(relation));
        validate_metadata_age_log_primary_key(relation);

        text* const name_value = bioseq::postgres::support::invoke_pg_as_cpp(
            [index_name, operation_context] {
                MemoryContext previous =
                    MemoryContextSwitchTo(operation_context);
                text* const result = cstring_to_text_with_len(
                    index_name.data(),
                    static_cast<int>(index_name.size()));
                MemoryContextSwitchTo(previous);
                return result;
            });
        auto encoded = metadata_age_log_codec::encode(
            metadata_age_log_catalog_row{
                .user_relid = user_relid,
                .main_relfilenode = main_relfilenode,
                .column_attno = column_attno,
                .index_name = name_value,
                .last_updated = GetCurrentTimestamp(),
            },
            operation_context);
        const auto tuple_data =
            encoded.tuple_data(RelationGetDescr(relation));
        std::array<Oid, metadata_age_log_codec::field_count>
            argument_types{
                OIDOID,
                OIDOID,
                INT2OID,
                TEXTOID,
                TIMESTAMPTZOID,
            };
        std::array<Datum, metadata_age_log_codec::field_count>
            argument_values{};
        std::copy_n(
            tuple_data.values,
            argument_values.size(),
            argument_values.begin());

        bioseq::postgres::support::spi_session spi;
        const int connect_status = spi.connect();
        if (connect_status != SPI_OK_CONNECT) {
            minimizer_generation_storage_error(
                "metadata_age_log SPI_connect failed");
        }
        static constexpr char k_statement[] =
            "INSERT INTO bioseq_internal.metadata_age_log "
            "(user_relid, main_relfilenode, column_attno, "
            " index_name, last_updated) "
            "VALUES ($1, $2, $3, $4, $5) "
            "ON CONFLICT "
            "(user_relid, main_relfilenode, column_attno, index_name) "
            "DO UPDATE SET last_updated = EXCLUDED.last_updated";
        const int execute_status =
            bioseq::postgres::support::invoke_pg_as_cpp(
                [&argument_types, &argument_values] {
                    return SPI_execute_with_args(
                        k_statement,
                        static_cast<int>(argument_types.size()),
                        argument_types.data(),
                        argument_values.data(),
                        /*Nulls=*/nullptr,
                        /*read_only=*/false,
                        /*tcount=*/0);
                });
        const std::uint64_t affected_rows = SPI_processed;
        if (execute_status != SPI_OK_INSERT
            || affected_rows != 1) {
            minimizer_generation_storage_error(fmt::format(
                "metadata_age_log upsert failed "
                "(SPI status={}, affected rows={})",
                execute_status,
                affected_rows));
        }
        if (spi.finish() != SPI_OK_FINISH) {
            minimizer_generation_storage_error(
                "metadata_age_log SPI_finish failed");
        }
        MemoryContext finished_context =
            std::exchange(operation_context, nullptr);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [finished_context] {
                MemoryContextDelete(finished_context);
            });
        Relation relation_to_close =
            std::exchange(relation, nullptr);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [relation_to_close] {
                table_close(
                    relation_to_close, RowExclusiveLock);
            });
        return affected_rows;
    } catch (...) {
        const std::exception_ptr original =
            std::current_exception();
        if (operation_context != nullptr) {
            MemoryContext doomed_context =
                std::exchange(operation_context, nullptr);
            try {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [doomed_context] {
                        MemoryContextDelete(doomed_context);
                    });
            } catch (...) {
                std::terminate();
            }
        }
        if (relation != nullptr) {
            try {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [relation] {
                        table_close(
                            relation, RowExclusiveLock);
                    });
            } catch (...) {
                std::terminate();
            }
        }
        std::rethrow_exception(original);
    }
}

/**
 * Delete one metadata-age row through generated codec-encoded key Datums.
 */
[[nodiscard]] inline std::uint64_t
delete_metadata_age_log_pg(
        Oid user_relid,
        Oid main_relfilenode,
        std::int16_t column_attno,
        std::string_view index_name) {
    const Oid namespace_oid = bioseq_internal_ns();
    const Oid relation_oid = bioseq::postgres::support::invoke_pg_as_cpp(
        [namespace_oid] {
            return get_relname_relid(
                "metadata_age_log", namespace_oid);
        });
    if (!OidIsValid(relation_oid)) {
        minimizer_generation_storage_error(
            "metadata_age_log catalog relation is missing");
    }

    Relation relation = bioseq::postgres::support::invoke_pg_as_cpp(
        [relation_oid] {
            return table_open(relation_oid, RowExclusiveLock);
        });
    MemoryContext operation_context = nullptr;
    try {
        operation_context =
            bioseq::postgres::support::invoke_pg_as_cpp([] {
                return AllocSetContextCreate(
                    CurrentMemoryContext,
                    "metadata_age_log delete operation",
                    ALLOCSET_SMALL_SIZES);
            });
        if (RelationGetNamespace(relation) != namespace_oid
            || relation->rd_rel->relkind != RELKIND_RELATION
            || std::string_view{RelationGetRelationName(relation)}
                != "metadata_age_log") {
            minimizer_generation_storage_error(
                "metadata_age_log catalog identity changed");
        }
        metadata_age_log_codec::validate_descriptor(
            RelationGetDescr(relation));
        validate_metadata_age_log_primary_key(relation);

        text* const name_value = bioseq::postgres::support::invoke_pg_as_cpp(
            [index_name, operation_context] {
                MemoryContext previous =
                    MemoryContextSwitchTo(operation_context);
                text* const result = cstring_to_text_with_len(
                    index_name.data(),
                    static_cast<int>(index_name.size()));
                MemoryContextSwitchTo(previous);
                return result;
            });
        auto encoded = metadata_age_log_codec::encode(
            metadata_age_log_catalog_row{
                .user_relid = user_relid,
                .main_relfilenode = main_relfilenode,
                .column_attno = column_attno,
                .index_name = name_value,
                .last_updated = GetCurrentTimestamp(),
            },
            operation_context);
        const auto tuple_data =
            encoded.tuple_data(RelationGetDescr(relation));
        std::array<Oid, 4> argument_types{
            OIDOID,
            OIDOID,
            INT2OID,
            TEXTOID,
        };
        std::array<Datum, 4> argument_values{};
        std::copy_n(
            tuple_data.values,
            argument_values.size(),
            argument_values.begin());

        bioseq::postgres::support::spi_session spi;
        const int connect_status = spi.connect();
        if (connect_status != SPI_OK_CONNECT) {
            minimizer_generation_storage_error(
                "metadata_age_log SPI_connect failed");
        }
        static constexpr char k_statement[] =
            "DELETE FROM bioseq_internal.metadata_age_log "
            "WHERE user_relid = $1 "
            "  AND main_relfilenode = $2 "
            "  AND column_attno = $3 "
            "  AND index_name = $4";
        const int execute_status =
            bioseq::postgres::support::invoke_pg_as_cpp(
                [&argument_types, &argument_values] {
                    return SPI_execute_with_args(
                        k_statement,
                        static_cast<int>(argument_types.size()),
                        argument_types.data(),
                        argument_values.data(),
                        /*Nulls=*/nullptr,
                        /*read_only=*/false,
                        /*tcount=*/0);
                });
        const std::uint64_t affected_rows = SPI_processed;
        if (execute_status != SPI_OK_DELETE
            || affected_rows > 1) {
            minimizer_generation_storage_error(fmt::format(
                "metadata_age_log delete failed "
                "(SPI status={}, affected rows={})",
                execute_status,
                affected_rows));
        }
        if (spi.finish() != SPI_OK_FINISH) {
            minimizer_generation_storage_error(
                "metadata_age_log SPI_finish failed");
        }
        MemoryContext finished_context =
            std::exchange(operation_context, nullptr);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [finished_context] {
                MemoryContextDelete(finished_context);
            });
        Relation relation_to_close =
            std::exchange(relation, nullptr);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [relation_to_close] {
                table_close(
                    relation_to_close, RowExclusiveLock);
            });
        return affected_rows;
    } catch (...) {
        const std::exception_ptr original =
            std::current_exception();
        if (operation_context != nullptr) {
            MemoryContext doomed_context =
                std::exchange(operation_context, nullptr);
            try {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [doomed_context] {
                        MemoryContextDelete(doomed_context);
                    });
            } catch (...) {
                std::terminate();
            }
        }
        if (relation != nullptr) {
            try {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [relation] {
                        table_close(
                            relation, RowExclusiveLock);
                    });
            } catch (...) {
                std::terminate();
            }
        }
        std::rethrow_exception(original);
    }
}

/// Stable administration-lock key shared by reset and publication. The hash
/// remains byte-for-byte identical across both operations.
[[nodiscard]] inline std::uint64_t index_advisory_xact_lock_key(
        Oid db,
        Oid tablespace,
        Oid relfilenode,
        std::int16_t attno,
        std::string_view index_name) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    hash ^= 0xB10D2790u;
    const auto mix_word = [&hash](std::uint64_t value) noexcept {
        for (std::uint32_t byte = 0; byte < 8; ++byte) {
            hash ^= value & 0xffu;
            hash *= 1099511628211ULL;
            value >>= 8u;
        }
    };
    mix_word(db);
    mix_word(tablespace);
    mix_word(relfilenode);
    mix_word(static_cast<std::uint16_t>(attno));
    for (char byte : index_name) {
        hash ^= static_cast<std::uint8_t>(byte);
        hash *= 1099511628211ULL;
    }
    return hash;
}

/// Acquire the one existing transaction-scoped logical-index administration
/// lock. Read sessions deliberately never call this helper.
inline void acquire_index_advisory_xact_lock(
        Oid db,
        Oid tablespace,
        Oid relfilenode,
        std::int16_t attno,
        std::string_view index_name) {
    const std::int64_t signed_key = std::bit_cast<std::int64_t>(
        index_advisory_xact_lock_key(
            db, tablespace, relfilenode, attno, index_name));
    bioseq::postgres::support::invoke_pg_as_cpp(
        [signed_key] {
            DirectFunctionCall1(
                pg_advisory_xact_lock_int8,
                Int64GetDatum(signed_key));
        });
}

/// Sequence-generator relation mismatch observed before ID allocation.
enum class minimizer_generation_sequence_error : std::uint8_t {
    none,
    missing,
    wrong_namespace,
    wrong_relkind,
    wrong_persistence,
    missing_catalog_row,
    wrong_type,
    wrong_bounds,
    cycling,
};

[[nodiscard]] constexpr std::string_view
minimizer_generation_sequence_error_name(
        minimizer_generation_sequence_error error) noexcept {
    switch (error) {
        case minimizer_generation_sequence_error::none: return "none";
        case minimizer_generation_sequence_error::missing: return "missing";
        case minimizer_generation_sequence_error::wrong_namespace: return "wrong namespace";
        case minimizer_generation_sequence_error::wrong_relkind: return "not a sequence";
        case minimizer_generation_sequence_error::wrong_persistence: return "not permanent";
        case minimizer_generation_sequence_error::missing_catalog_row: return "missing pg_sequence row";
        case minimizer_generation_sequence_error::wrong_type: return "not BIGINT";
        case minimizer_generation_sequence_error::wrong_bounds: return "wrong positive non-wrapping bounds";
        case minimizer_generation_sequence_error::cycling: return "cycles";
    }
    return "unknown";
}

/// Inspection result for the generation identifier sequence.
struct minimizer_generation_sequence_inspection {
    minimizer_generation_sequence_error error =
        minimizer_generation_sequence_error::none;
    Oid sequence_oid = InvalidOid;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == minimizer_generation_sequence_error::none
            && OidIsValid(sequence_oid);
    }
};

/// Validate the SQL installation object's exact non-wrapping BIGINT contract.
[[nodiscard]] inline minimizer_generation_sequence_inspection
inspect_minimizer_generation_sequence_contract() {
    const Oid internal_namespace = bioseq::postgres::support::invoke_pg_as_cpp(
        [] { return bioseq_internal_ns(); });
    const Oid sequence_oid =
        indexing_entity_handles::minimizer_generation_id_seq.resolve_oid(
            bioseq::postgres::support::catalog_missing_policy::return_invalid);
    if (!OidIsValid(sequence_oid)) {
        return {.error = minimizer_generation_sequence_error::missing};
    }
    Relation sequence_relation = bioseq::postgres::support::invoke_pg_as_cpp(
        [sequence_oid] {
            return bioseq::postgres::support::try_open_postgres_relation(
                sequence_oid, AccessShareLock);
        });
    if (sequence_relation == nullptr) {
        return {.error = minimizer_generation_sequence_error::missing};
    }
    minimizer_generation_sequence_error error =
        minimizer_generation_sequence_error::none;
    if (RelationGetNamespace(sequence_relation) != internal_namespace) {
        error = minimizer_generation_sequence_error::wrong_namespace;
    } else if (sequence_relation->rd_rel->relkind != RELKIND_SEQUENCE) {
        error = minimizer_generation_sequence_error::wrong_relkind;
    } else if (sequence_relation->rd_rel->relpersistence
               != RELPERSISTENCE_PERMANENT) {
        error = minimizer_generation_sequence_error::wrong_persistence;
    }
    bioseq::postgres::support::invoke_pg_as_cpp(
        [sequence_relation] {
            relation_close(sequence_relation, AccessShareLock);
        });
    if (error != minimizer_generation_sequence_error::none) {
        return {.error = error};
    }
    HeapTuple sequence_tuple = bioseq::postgres::support::invoke_pg_as_cpp(
        [sequence_oid] {
            return SearchSysCache1(
                SEQRELID, ObjectIdGetDatum(sequence_oid));
        });
    if (!HeapTupleIsValid(sequence_tuple)) {
        return {.error =
            minimizer_generation_sequence_error::missing_catalog_row};
    }
    const Form_pg_sequence sequence =
        reinterpret_cast<Form_pg_sequence>(GETSTRUCT(sequence_tuple));
    if (sequence->seqtypid != INT8OID) {
        error = minimizer_generation_sequence_error::wrong_type;
    } else if (sequence->seqincrement != 1
               || sequence->seqmin != 1
               || sequence->seqmax
                    != (std::numeric_limits<std::int64_t>::max)()
               || sequence->seqstart != 1
               || sequence->seqcache != 1) {
        error = minimizer_generation_sequence_error::wrong_bounds;
    } else if (sequence->seqcycle) {
        error = minimizer_generation_sequence_error::cycling;
    }
    bioseq::postgres::support::invoke_pg_as_cpp(
        [sequence_tuple] { ReleaseSysCache(sequence_tuple); });
    return {.error = error, .sequence_oid =
        error == minimizer_generation_sequence_error::none
            ? sequence_oid : InvalidOid};
}

/// Allocate a positive signed-SQL generation ID. This helper never creates the
/// sequence; installation owns that atomic DDL step.
[[nodiscard]] inline std::uint64_t allocate_minimizer_generation_id_pg() {
    const minimizer_generation_sequence_inspection sequence =
        inspect_minimizer_generation_sequence_contract();
    if (!sequence) {
        minimizer_generation_storage_error_as_cpp(fmt::format(
            "owned generation sequence contract mismatch: {}",
            minimizer_generation_sequence_error_name(sequence.error)));
    }
    const std::int64_t generation_id = bioseq::postgres::support::invoke_pg_as_cpp(
        [&sequence] {
            return nextval_internal(sequence.sequence_oid, false);
        });
    if (generation_id <= 0) {
        minimizer_generation_storage_error_as_cpp(
            "generation sequence exhausted its positive BIGINT range");
    }
    return static_cast<std::uint64_t>(generation_id);
}

/// Validate one current-generation address and its row checksum before a PostgreSQL
/// tuple is formed. The active-head row is never accepted by the immutable
/// generation sink and has a separate atomic publication helper below.
struct minimizer_generation_row_validator;

/// Move-only authority for a row validated against the frozen wire contract.
class validated_minimizer_generation_row {
private:
    minimizer_generation_row_view row_{};

    explicit validated_minimizer_generation_row(
            const minimizer_generation_row_view& row) noexcept
        : row_(row) {}

    friend struct minimizer_generation_row_validator;

public:
    validated_minimizer_generation_row() = delete;

    [[nodiscard]] const minimizer_generation_row_view& row() const noexcept {
        return row_;
    }
};

/// Sole validation and construction authority. Defining this after the token is
/// complete avoids instantiating std::expected<validated_minimizer_generation_row, ...>
/// while preserving the invariant that no caller can construct an unchecked
/// token.
struct minimizer_generation_row_validator {
private:
    [[nodiscard]] static std::expected<
        void, minimizer_generation_encoding_error>
    validate_shape(
            std::uint64_t expected_generation_id,
            const index_chunk_address& address,
            std::span<const std::byte> bytes) noexcept {
        if (expected_generation_id == 0
            || !bioseq::indexing::sql_bigint_can_represent(
                expected_generation_id)
            || address.generation_id != expected_generation_id) {
            return std::unexpected(
                minimizer_generation_encoding_error::generation_id);
        }
        if (bytes.empty()
            || bytes.size()
                > static_cast<std::size_t>(
                    (std::numeric_limits<std::int32_t>::max)())
            || !bioseq::indexing::sql_integer_can_represent(
                address.chunk_no)) {
            return std::unexpected(
                minimizer_generation_encoding_error::byte_count);
        }
        if (address.position_segment == -1) {
            if (address.sub_key != k_directory_sub_key) {
                return std::unexpected(
                    minimizer_generation_encoding_error::sub_key);
            }
            if ((address.chunk_no == 0
                    && bytes.size()
                        != minimizer_generation_manifest::encoded_bytes
                    && bytes.size()
                        != minimizer_generation_manifest::encoded_bytes)
                || (address.chunk_no != 0
                    && bytes.size()
                        > bioseq::indexing::
                            k_grouped_soa_extent_payload_bytes)) {
                return std::unexpected(
                    minimizer_generation_encoding_error::byte_count);
            }
        } else if (address.position_segment < 0) {
            return std::unexpected(
                minimizer_generation_encoding_error::position_segment);
        } else if (address.sub_key < 0
                   || address.sub_key >= k_temporary_run_sub_key_base
                   || bytes.size()
                        > bioseq::indexing::
                            k_grouped_soa_extent_payload_bytes) {
            return std::unexpected(minimizer_generation_encoding_error::sub_key);
        }
        return {};
    }

public:
    [[nodiscard]] static std::expected<
        validated_minimizer_generation_row, minimizer_generation_encoding_error>
    validate(
            std::uint64_t expected_generation_id,
            const minimizer_generation_row_view& row) noexcept {
        if (auto shape = validate_shape(
                expected_generation_id, row.address, row.bytes); !shape) {
            return std::unexpected(shape.error());
        }
        const std::uint64_t expected_checksum =
            bioseq::indexing::crc64_index_chunk_row(
                row.address.generation_id,
                row.address.position_segment,
                row.address.sub_key,
                row.address.chunk_no,
                static_cast<std::uint32_t>(row.bytes.size()),
                row.bytes);
        if (row.row_checksum != expected_checksum) {
            return std::unexpected(minimizer_generation_encoding_error::checksum);
        }
        return validated_minimizer_generation_row{row};
    }

    /// Compute and bind the sole grouped-SoA payload-row CRC. The caller does
    /// not provide a checksum that would require a second verification pass.
    [[nodiscard]] static std::expected<
        validated_minimizer_generation_row, minimizer_generation_encoding_error>
    validate_and_checksum(
            std::uint64_t expected_generation_id,
            const unchecked_minimizer_generation_row_view& row) noexcept {
        if (row.address.position_segment < 0) {
            return std::unexpected(minimizer_generation_encoding_error::row_order);
        }
        if (auto shape = validate_shape(
                expected_generation_id, row.address, row.bytes); !shape) {
            return std::unexpected(shape.error());
        }
        const std::uint64_t checksum =
            bioseq::indexing::crc64_index_chunk_row(
                row.address.generation_id,
                row.address.position_segment,
                row.address.sub_key,
                row.address.chunk_no,
                static_cast<std::uint32_t>(row.bytes.size()),
                row.bytes);
        return validated_minimizer_generation_row{
            minimizer_generation_row_view{
                .address = row.address,
                .bytes = row.bytes,
                .row_checksum = checksum,
            }};
    }
};

/// Validate one row before forming a PostgreSQL tuple.
[[nodiscard]] inline std::expected<
    validated_minimizer_generation_row, minimizer_generation_encoding_error>
validate_minimizer_generation_row_for_write(
        std::uint64_t expected_generation_id,
        const minimizer_generation_row_view& row) noexcept {
    return minimizer_generation_row_validator::validate(
        expected_generation_id, row);
}

/// Catalog identity shared by generation writers, readers, and administrators.
struct minimizer_generation_administration_identity {
    Oid database_oid = InvalidOid;
    Oid tablespace_oid = InvalidOid;
    Oid main_relfilenode = InvalidOid;
    Oid user_relid = InvalidOid;
    std::int16_t column_attno = 0;
    std::string index_name;
    std::string table_name;

    void release_owned_strings() noexcept {
        std::string{}.swap(index_name);
        std::string{}.swap(table_name);
    }
};

/// Immutable identity of the exact visible source row used by the current generation.
/// `storage_contract_checksum` is filled only after the independently decoded
/// manifest has been checked against the source row and active storage-contract
/// axes; no head/manifest checksum is copied into this object.
struct minimizer_generation_source_locator {
    std::uint64_t sequence_rowno = 0;
    sequence_locator locator{};
    std::uint8_t alphabet_code = 0;
    std::uint8_t storage_unit_byte_width = 0;
    std::uint16_t padding_letters = 0;
    std::uint64_t storage_contract_checksum = 0;

    [[nodiscard]] bool operator==(
            const minimizer_generation_source_locator& other) const noexcept {
        return sequence_rowno == other.sequence_rowno
            && locator.payload_version == other.locator.payload_version
            && locator.bucket_index == other.locator.bucket_index
            && locator.reserved_16 == other.locator.reserved_16
            && locator.shard_index == other.locator.shard_index
            && locator.fragment_count == other.locator.fragment_count
            && locator.reserved_32 == other.locator.reserved_32
            && locator.logical_length == other.locator.logical_length
            && locator.first_fragment_row == other.locator.first_fragment_row
            && locator.source_epoch == other.locator.source_epoch
            && alphabet_code == other.alphabet_code
            && storage_unit_byte_width == other.storage_unit_byte_width
            && padding_letters == other.padding_letters
            && storage_contract_checksum == other.storage_contract_checksum;
    }
};

[[nodiscard]] constexpr bool same_minimizer_generation_sequence_locator(
        const sequence_locator& left,
        const sequence_locator& right) noexcept {
    return left.payload_version == right.payload_version
        && left.bucket_index == right.bucket_index
        && left.reserved_16 == right.reserved_16
        && left.shard_index == right.shard_index
        && left.fragment_count == right.fragment_count
        && left.reserved_32 == right.reserved_32
        && left.logical_length == right.logical_length
        && left.first_fragment_row == right.first_fragment_row
        && left.source_epoch == right.source_epoch;
}

/// Canonical locator-derived identity. The independently streamed content
/// checksum prevents a same-length mutation from being hidden by a
/// self-consistent rewrite of the head and manifest.
[[nodiscard]] constexpr std::uint64_t minimizer_generation_source_identity(
        Oid /*main_relfilenode*/,
        const minimizer_generation_source_locator& source) noexcept {
    return source.locator.source_epoch;
}

class index_storage_fragment_generation_row_sink;
class index_storage_fragment_generation_read_session;
class index_storage_hash_major_minimizer_writer;
class index_storage_hash_major_minimizer_read_session;

/// Unforgeable PostgreSQL-owned authorization derived from the exact visible main-row
/// datum and, for a locator stub, every referenced fragment row under one
/// registered MVCC snapshot. Only the authorization helper can construct it. The
/// relation/query/snapshot binding prevents replay into another read session;
/// consumption is move-only and invalidates the token.
class minimizer_generation_source_authorization {
private:
    minimizer_generation_source_locator source_{};
    std::uint64_t source_content_checksum_ = 0;
    Oid database_oid_ = InvalidOid;
    Oid tablespace_oid_ = InvalidOid;
    Oid main_relfilenode_ = InvalidOid;
    Oid user_relid_ = InvalidOid;
    std::int16_t column_attno_ = 0;
    std::string index_name_{};
    std::string table_name_{};
    Snapshot snapshot_ = nullptr;
    TransactionId transaction_id_ = InvalidTransactionId;
    bool valid_ = false;

    minimizer_generation_source_authorization() = default;

    void invalidate_() noexcept {
        source_ = {};
        source_content_checksum_ = 0;
        database_oid_ = InvalidOid;
        tablespace_oid_ = InvalidOid;
        main_relfilenode_ = InvalidOid;
        user_relid_ = InvalidOid;
        column_attno_ = 0;
        std::string{}.swap(index_name_);
        std::string{}.swap(table_name_);
        snapshot_ = nullptr;
        transaction_id_ = InvalidTransactionId;
        valid_ = false;
    }

    friend minimizer_generation_source_authorization
    authorize_minimizer_generation_source_pg(
        const minimizer_generation_administration_identity&,
        std::uint64_t,
        Snapshot);
    friend class index_storage_fragment_generation_row_sink;
    friend class index_storage_fragment_generation_read_session;
    friend class index_storage_hash_major_minimizer_writer;
    friend class index_storage_hash_major_minimizer_read_session;

public:
    minimizer_generation_source_authorization(
        const minimizer_generation_source_authorization&) = delete;
    minimizer_generation_source_authorization& operator=(
        const minimizer_generation_source_authorization&) = delete;
    minimizer_generation_source_authorization(
            minimizer_generation_source_authorization&& other) noexcept
        : source_(other.source_),
          source_content_checksum_(other.source_content_checksum_),
          database_oid_(other.database_oid_),
          tablespace_oid_(other.tablespace_oid_),
          main_relfilenode_(other.main_relfilenode_),
          user_relid_(other.user_relid_),
          column_attno_(other.column_attno_),
          index_name_(std::move(other.index_name_)),
          table_name_(std::move(other.table_name_)),
          snapshot_(other.snapshot_),
          transaction_id_(other.transaction_id_),
          valid_(other.valid_) {
        other.invalidate_();
    }
    minimizer_generation_source_authorization& operator=(
            minimizer_generation_source_authorization&& other) noexcept {
        if (this != &other) {
            invalidate_();
            source_ = other.source_;
            source_content_checksum_ = other.source_content_checksum_;
            database_oid_ = other.database_oid_;
            tablespace_oid_ = other.tablespace_oid_;
            main_relfilenode_ = other.main_relfilenode_;
            user_relid_ = other.user_relid_;
            column_attno_ = other.column_attno_;
            index_name_ = std::move(other.index_name_);
            table_name_ = std::move(other.table_name_);
            snapshot_ = other.snapshot_;
            transaction_id_ = other.transaction_id_;
            valid_ = other.valid_;
            other.invalidate_();
        }
        return *this;
    }
    ~minimizer_generation_source_authorization() = default;

    [[nodiscard]] const minimizer_generation_source_locator&
    source_for_diagnostics() const noexcept {
        return source_;
    }

    [[nodiscard]] std::uint64_t
    source_content_checksum_for_diagnostics() const noexcept {
        return source_content_checksum_;
    }
};

static_assert(!std::is_default_constructible_v<
    minimizer_generation_source_authorization>);
static_assert(!std::is_copy_constructible_v<
    minimizer_generation_source_authorization>);
static_assert(std::is_nothrow_move_constructible_v<
    minimizer_generation_source_authorization>);

[[nodiscard]] inline minimizer_generation_source_authorization
authorize_minimizer_generation_source_pg(
        const minimizer_generation_administration_identity& identity,
        std::uint64_t sequence_rowno,
        Snapshot registered_snapshot) {
    const auto minimizer_generation_storage_error = [](std::string_view message) {
        minimizer_generation_storage_error_as_cpp(message);
    };
    struct source_read_state {
        Relation source_relation = nullptr;
        column_read_state* source_read = nullptr;
        column_read_state* fragment_read = nullptr;
        seq_caches* fragment_caches = nullptr;
        MemoryContext work_context = nullptr;
        MemoryContext fragment_context = nullptr;
    };
    auto* state = new source_read_state{};
    minimizer_generation_source_authorization authorization{};
    try {
        if (identity.database_oid != MyDatabaseId
            || !OidIsValid(identity.user_relid)
            || !OidIsValid(identity.tablespace_oid)
            || !OidIsValid(identity.main_relfilenode)
            || identity.column_attno <= 0
            || identity.index_name.empty()
            || !bioseq::indexing::sql_bigint_can_represent(sequence_rowno)
            || registered_snapshot == nullptr
            || !IsMVCCSnapshot(registered_snapshot)
            || registered_snapshot->regd_count == 0) {
            minimizer_generation_storage_error("authoritative source authorization input is invalid");
        }
        state->work_context = bioseq::postgres::support::invoke_pg_as_cpp([] {
            return AllocSetContextCreate(
                CurrentMemoryContext,
                "bioseq current generation authoritative source authorization",
                ALLOCSET_DEFAULT_SIZES);
        });
        state->fragment_context =
            bioseq::postgres::support::invoke_pg_as_cpp([state] {
            return AllocSetContextCreate(
                state->work_context, "bioseq current generation source authorization fragment",
                ALLOCSET_DEFAULT_SIZES);
        });
        state->source_relation =
            bioseq::postgres::support::invoke_pg_as_cpp(
                [user_relid = identity.user_relid] {
            return bioseq::postgres::support::try_open_postgres_relation(
                user_relid, AccessShareLock);
        });
        if (state->source_relation == nullptr
            || RelationGetRelid(state->source_relation) != identity.user_relid
            || state->source_relation->rd_locator.dbOid != identity.database_oid
            || static_cast<Oid>(state->source_relation->rd_locator.relNumber)
                != identity.main_relfilenode) {
            minimizer_generation_storage_error("authoritative source relation is missing or changed");
        }
        const Oid source_tablespace =
            OidIsValid(state->source_relation->rd_rel->reltablespace)
            ? state->source_relation->rd_rel->reltablespace
            : MyDatabaseTableSpace;
        TupleDesc source_desc = RelationGetDescr(state->source_relation);
        if (source_tablespace != identity.tablespace_oid
            || identity.column_attno > source_desc->natts
            || TupleDescAttr(source_desc, identity.column_attno - 1)->attisdropped) {
            minimizer_generation_storage_error("authoritative source relation/column identity changed");
        }

        MemoryContext previous_context = bioseq::postgres::support::invoke_pg_as_cpp(
            [state] { return MemoryContextSwitchTo(state->work_context); });
        const auto switch_context = [](MemoryContext context) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [context] { MemoryContextSwitchTo(context); });
        };
        List* projected = bioseq::postgres::support::invoke_pg_as_cpp(
            [column_attno = identity.column_attno] {
                return list_make1_int(column_attno);
            });
        Datum* values = bioseq::postgres::support::invoke_pg_as_cpp(
            [source_desc] {
            return static_cast<Datum*>(palloc0(
                sizeof(Datum)
                * static_cast<std::size_t>(source_desc->natts)));
        });
        bool* nulls = bioseq::postgres::support::invoke_pg_as_cpp(
            [source_desc] {
            return static_cast<bool*>(palloc0(
                sizeof(bool)
                * static_cast<std::size_t>(source_desc->natts)));
        });
        state->source_read = bioseq::postgres::support::invoke_pg_as_cpp(
            [state, source_desc, projected, registered_snapshot] {
            return begin_column_read(
                state->source_relation, source_desc, projected, NIL,
                state->work_context, registered_snapshot,
                /*randomAccess=*/true, nullptr);
        });
        const bool row_visible = bioseq::postgres::support::invoke_pg_as_cpp(
            [state, sequence_rowno, values, nulls] {
            return read_column_row_by_number(
                state->source_read, sequence_rowno, values, nulls);
        });
        if (!row_visible || nulls[identity.column_attno - 1]) {
            switch_context(previous_context);
            minimizer_generation_storage_error("authoritative source row is not visible");
        }
        bioseq::pg_types::seq* source_header =
            bioseq::postgres::support::invoke_pg_as_cpp(
                [values, column_attno = identity.column_attno] {
                    return reinterpret_cast<bioseq::pg_types::seq*>(
                        PG_DETOAST_DATUM(values[column_attno - 1]));
                });
        const std::uint8_t alphabet_code = static_cast<std::uint8_t>(
            source_header->alphabet_code);
        const std::uint8_t storage_width =
            logical_storage_unit_byte_width(source_header);
        const std::uint16_t padding_letters = source_header->padding_letters;
        if (storage_width == 0) {
            switch_context(previous_context);
            minimizer_generation_storage_error("authoritative source storage width is zero");
        }

        if (!is_locator_stub(source_header)) {
            switch_context(previous_context);
            minimizer_generation_storage_error(
                "authoritative current-generation source is not an epoch-bearing locator stub");
        }
        const sequence_locator locator = decode_locator_from_stub(source_header);
        if (locator.payload_version != sequence_locator_payload_version
            || locator.reserved_16 != 0 || locator.reserved_32 != 0
            || locator.source_epoch == 0) {
            switch_context(previous_context);
            minimizer_generation_storage_error("authoritative source locator is invalid");
        }
        authorization.source_ = minimizer_generation_source_locator{
            .sequence_rowno = sequence_rowno,
            .locator = locator,
            .alphabet_code = alphabet_code,
            .storage_unit_byte_width = storage_width,
            .padding_letters = padding_letters,
            .storage_contract_checksum = 0,
        };

        const auto source_catalog =
            bioseq::postgres::support::invoke_pg_as_cpp(
                [source_epoch = locator.source_epoch,
                 registered_snapshot] {
            return sequence_source_version_manager::lookup(
                source_epoch, registered_snapshot);
        });
        const std::uint64_t main_storage_id =
            bioseq::postgres::support::invoke_pg_as_cpp([state] {
                return column_storage_id(
                    state->source_relation, false);
            });
        if (!source_catalog
            || source_catalog->source_epoch != locator.source_epoch
            || source_catalog->user_relid != identity.user_relid
            || source_catalog->database_oid != identity.database_oid
            || source_catalog->tablespace_oid != identity.tablespace_oid
            || source_catalog->main_relfilenode != identity.main_relfilenode
            || source_catalog->main_storage_id != main_storage_id
            || source_catalog->column_attno != identity.column_attno
            || !same_minimizer_generation_sequence_locator(source_catalog->locator, locator)
            || source_catalog->alphabet_code != alphabet_code
            || source_catalog->storage_unit_byte_width != storage_width
            || source_catalog->padding_letters != padding_letters) {
            switch_context(previous_context);
            minimizer_generation_storage_error(
                "authoritative source epoch/catalog binding disagrees");
        }

        crc64_ecma182_accumulator content_checksum{};
        std::uint64_t decoded_bp = 0;
        const auto consume_visible_fragment = [&content_checksum, &decoded_bp,
                &minimizer_generation_storage_error, alphabet_code,
                storage_width](const bioseq::pg_types::seq* fragment) {
            if (fragment == nullptr || is_locator_stub(fragment)
                || static_cast<std::uint8_t>(fragment->alphabet_code)
                    != alphabet_code
                || logical_storage_unit_byte_width(fragment) != storage_width) {
                minimizer_generation_storage_error("authoritative source fragment header disagrees");
            }
            fragment->convert_to([&content_checksum, &decoded_bp,
                    &minimizer_generation_storage_error]<class p_sequence_type>(
                    const compact_sequence_range<p_sequence_type>& range) {
                const std::string decoded =
                    ::bioseq::simd::active_target::decode_sequence_simd<p_sequence_type>(range);
                if (decoded.size() != range.bp_size()
                    || decoded.size()
                        > (std::numeric_limits<std::uint64_t>::max)()
                            - decoded_bp) {
                    minimizer_generation_storage_error("authoritative source decoded length is invalid");
                }
                content_checksum.update(std::as_bytes(
                    std::span<const char>{decoded.data(), decoded.size()}));
                decoded_bp += decoded.size();
            });
        };

        if (!is_locator_stub(source_header)) {
            consume_visible_fragment(source_header);
        } else if (locator.fragment_count != 0) {
            state->fragment_caches = bioseq::postgres::support::invoke_pg_as_cpp(
                [state] { return alloc_seq_caches(state->work_context); });
            state->fragment_caches->side = seq_caches::access_side::read;
            state->fragment_caches->lock_mode = AccessShareLock;
            state->fragment_caches->read_state_mctx = state->work_context;
            state->fragment_caches->main_database_oid = identity.database_oid;
            state->fragment_caches->main_tablespace_oid = identity.tablespace_oid;
            state->fragment_caches->main_relfilenode = identity.main_relfilenode;
            Relation fragment_relation = bioseq::postgres::support::invoke_pg_as_cpp(
                [state, &identity, locator] {
                    return open_sub_table_for_locator(
                        identity.database_oid, identity.tablespace_oid,
                        identity.main_relfilenode, identity.column_attno,
                        locator, AccessShareLock, state->fragment_caches);
                });
            if (RelationGetRelid(fragment_relation)
                    != source_catalog->fragment_relid
                || bioseq::postgres::support::invoke_pg_as_cpp(
                    [fragment_relation] {
                    return column_storage_id(
                        fragment_relation, false);
                })
                    != source_catalog->fragment_storage_id) {
                switch_context(previous_context);
                minimizer_generation_storage_error(
                    "authoritative source fragment storage incarnation changed");
            }
            TupleDesc fragment_desc = RelationGetDescr(fragment_relation);
            if (fragment_desc->natts != 1
                || TupleDescAttr(fragment_desc, 0)->attisdropped) {
                switch_context(previous_context);
                minimizer_generation_storage_error("authoritative source fragment schema disagrees");
            }
            List* fragment_projected = bioseq::postgres::support::invoke_pg_as_cpp(
                [] { return list_make1_int(1); });
            state->fragment_read =
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [state, fragment_relation, fragment_desc,
                     fragment_projected, registered_snapshot] {
                return begin_column_read(
                    fragment_relation, fragment_desc, fragment_projected,
                    NIL, state->work_context, registered_snapshot,
                    /*randomAccess=*/true, nullptr);
            });
            for (std::uint32_t fragment_no = 0;
                 fragment_no < locator.fragment_count; ++fragment_no) {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [] { CHECK_FOR_INTERRUPTS(); });
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [state] { MemoryContextReset(state->fragment_context); });
                Datum fragment_values[1]{0};
                bool fragment_nulls[1]{true};
                const std::uint64_t fragment_rowno =
                    locator.first_fragment_row + fragment_no;
                if (fragment_rowno < locator.first_fragment_row
                    || !bioseq::postgres::support::invoke_pg_as_cpp(
                        [state, fragment_rowno, &fragment_values,
                         &fragment_nulls] {
                        return read_column_row_by_number(
                            state->fragment_read, fragment_rowno,
                            fragment_values, fragment_nulls);
                    })
                    || fragment_nulls[0]) {
                    switch_context(previous_context);
                    minimizer_generation_storage_error("authoritative source fragment row is missing");
                }
                switch_context(state->fragment_context);
                bioseq::pg_types::seq* fragment =
                    bioseq::postgres::support::invoke_pg_as_cpp(
                        [fragment_datum = fragment_values[0]] {
                            return reinterpret_cast<bioseq::pg_types::seq*>(
                                PG_DETOAST_DATUM(fragment_datum));
                        });
                if ((fragment_no + 1u != locator.fragment_count
                        && fragment->padding_letters != 0)
                    || (fragment_no + 1u == locator.fragment_count
                        && fragment->padding_letters != padding_letters)) {
                    switch_context(previous_context);
                    minimizer_generation_storage_error("authoritative source fragment padding disagrees");
                }
                consume_visible_fragment(fragment);
                switch_context(state->work_context);
            }
        }
        if (decoded_bp != locator.logical_length) {
            switch_context(previous_context);
            minimizer_generation_storage_error("authoritative source decoded length disagrees with locator");
        }
        if (content_checksum.checksum()
                != source_catalog->source_content_checksum) {
            switch_context(previous_context);
            minimizer_generation_storage_error(
                "authoritative source decoded checksum disagrees with its epoch catalog");
        }
        authorization.source_content_checksum_ =
            source_catalog->source_content_checksum;
        authorization.database_oid_ = identity.database_oid;
        authorization.tablespace_oid_ = identity.tablespace_oid;
        authorization.main_relfilenode_ = identity.main_relfilenode;
        authorization.user_relid_ = identity.user_relid;
        authorization.column_attno_ = identity.column_attno;
        authorization.index_name_ = identity.index_name;
        authorization.table_name_ = format_seqidx_name(
            identity.index_name, identity.database_oid,
            identity.tablespace_oid, identity.main_relfilenode,
            identity.column_attno, authorization.source_.locator.bucket_index,
            authorization.source_.locator.shard_index);
        authorization.snapshot_ = registered_snapshot;
        authorization.transaction_id_ = bioseq::postgres::support::invoke_pg_as_cpp(
            [] { return GetCurrentTransactionId(); });
        authorization.valid_ = true;

        if (state->fragment_read != nullptr) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [state] { end_column_read(state->fragment_read); });
            state->fragment_read = nullptr;
        }
        if (state->fragment_caches != nullptr) {
            bioseq::postgres::support::invoke_pg_as_cpp([state] {
                close_and_destroy_seq_caches(
                    state->fragment_caches, /*commit=*/false);
            });
            state->fragment_caches = nullptr;
        }
        bioseq::postgres::support::invoke_pg_as_cpp(
            [state] { end_column_read(state->source_read); });
        state->source_read = nullptr;
        switch_context(previous_context);
        Relation finished_source = std::exchange(
            state->source_relation, nullptr);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [finished_source] {
                relation_close(finished_source, AccessShareLock);
            });
        MemoryContext finished_context = std::exchange(
            state->work_context, nullptr);
        state->fragment_context = nullptr;
        bioseq::postgres::support::invoke_pg_as_cpp(
            [finished_context] { MemoryContextDelete(finished_context); });
    } catch (...) {
        state->fragment_read = nullptr;
        state->fragment_caches = nullptr;
        state->source_read = nullptr;
        state->source_relation = nullptr;
        state->work_context = nullptr;
        state->fragment_context = nullptr;
        authorization.invalidate_();
        delete state;
        throw;
    }
    delete state;
    return authorization;
}

/// Fixed work equation for cold, timed source admission. These counters travel
/// inside the move-only authorization and are rechecked by the read session, so a later
/// accidental source scan cannot be benchmark-accepted as an O(1) load.
struct minimizer_generation_catalog_source_stats {
    std::uint64_t parent_rows_read = 0;
    std::uint64_t catalog_rows_read = 0;
    std::uint64_t catalog_index_scans = 0;
    std::uint64_t fragment_rows_read = 0;
    std::uint64_t decoded_bp = 0;
    std::uint64_t storage_incarnations_checked = 0;

    [[nodiscard]] bool timed_equation_holds() const noexcept {
        return parent_rows_read == 1
            && catalog_rows_read == 1
            && catalog_index_scans == 1
            && fragment_rows_read == 0
            && decoded_bp == 0
            && storage_incarnations_checked >= 1
            && storage_incarnations_checked <= 2;
    }
};

/// O(1), PostgreSQL-owned source authorization for a cold reader. It validates one
/// visible parent stub and one source-epoch catalog tuple, plus constant-size
/// relation storage-incarnation metadata. It never opens a source fragment
/// read state and never decodes bases.
class minimizer_generation_catalog_source_authorization {
private:
    minimizer_generation_source_locator source_{};
    std::uint64_t source_content_checksum_ = 0;
    minimizer_generation_catalog_source_stats stats_{};
    cold_index_preparation_stats cold_stats_{};
    Oid database_oid_ = InvalidOid;
    Oid tablespace_oid_ = InvalidOid;
    Oid main_relfilenode_ = InvalidOid;
    Oid user_relid_ = InvalidOid;
    std::int16_t column_attno_ = 0;
    std::string index_name_{};
    std::string table_name_{};
    Snapshot snapshot_ = nullptr;
    bool valid_ = false;

    minimizer_generation_catalog_source_authorization() = default;

    void invalidate_() noexcept {
        source_ = {};
        source_content_checksum_ = 0;
        stats_ = {};
        cold_stats_ = {};
        database_oid_ = InvalidOid;
        tablespace_oid_ = InvalidOid;
        main_relfilenode_ = InvalidOid;
        user_relid_ = InvalidOid;
        column_attno_ = 0;
        std::string{}.swap(index_name_);
        std::string{}.swap(table_name_);
        snapshot_ = nullptr;
        valid_ = false;
    }

    friend minimizer_generation_catalog_source_authorization authorize_minimizer_generation_catalog_source_pg(
        const minimizer_generation_administration_identity&, std::uint64_t,
        const borrowed_executor_read_context&);
    friend minimizer_generation_catalog_source_authorization authorize_minimizer_generation_catalog_source_pg(
        const minimizer_generation_administration_identity&, std::uint64_t, Snapshot);
    friend class index_storage_fragment_generation_read_session;
    friend class index_storage_hash_major_minimizer_read_session;

public:
    minimizer_generation_catalog_source_authorization(const minimizer_generation_catalog_source_authorization&) = delete;
    minimizer_generation_catalog_source_authorization& operator=(const minimizer_generation_catalog_source_authorization&) = delete;
    minimizer_generation_catalog_source_authorization(minimizer_generation_catalog_source_authorization&& other) noexcept
        : source_(other.source_),
          source_content_checksum_(other.source_content_checksum_),
          stats_(other.stats_),
          cold_stats_(other.cold_stats_),
          database_oid_(other.database_oid_),
          tablespace_oid_(other.tablespace_oid_),
          main_relfilenode_(other.main_relfilenode_),
          user_relid_(other.user_relid_),
          column_attno_(other.column_attno_),
          index_name_(std::move(other.index_name_)),
          table_name_(std::move(other.table_name_)),
          snapshot_(other.snapshot_), valid_(other.valid_) {
        other.invalidate_();
    }
    minimizer_generation_catalog_source_authorization& operator=(
            minimizer_generation_catalog_source_authorization&& other) noexcept {
        if (this != &other) {
            invalidate_();
            source_ = other.source_;
            source_content_checksum_ = other.source_content_checksum_;
            stats_ = other.stats_;
            cold_stats_ = other.cold_stats_;
            database_oid_ = other.database_oid_;
            tablespace_oid_ = other.tablespace_oid_;
            main_relfilenode_ = other.main_relfilenode_;
            user_relid_ = other.user_relid_;
            column_attno_ = other.column_attno_;
            index_name_ = std::move(other.index_name_);
            table_name_ = std::move(other.table_name_);
            snapshot_ = other.snapshot_;
            valid_ = other.valid_;
            other.invalidate_();
        }
        return *this;
    }
    ~minimizer_generation_catalog_source_authorization() = default;

    [[nodiscard]] const minimizer_generation_source_locator&
    source_for_diagnostics() const noexcept { return source_; }
    [[nodiscard]] std::uint64_t
    source_content_checksum_for_diagnostics() const noexcept {
        return source_content_checksum_;
    }
    [[nodiscard]] const minimizer_generation_catalog_source_stats&
    stats_for_diagnostics() const noexcept { return stats_; }
    [[nodiscard]] std::string_view
    table_name_for_diagnostics() const noexcept { return table_name_; }
};

static_assert(!std::is_default_constructible_v<minimizer_generation_catalog_source_authorization>);
static_assert(!std::is_copy_constructible_v<minimizer_generation_catalog_source_authorization>);
static_assert(std::is_nothrow_move_constructible_v<minimizer_generation_catalog_source_authorization>);

[[nodiscard]] inline minimizer_generation_catalog_source_authorization
authorize_minimizer_generation_catalog_source_pg(
        const minimizer_generation_administration_identity& identity,
        std::uint64_t sequence_rowno,
        const borrowed_executor_read_context& executor_read) {
    const std::uint64_t source_begin_ns = cold_index_now_ns();
    const BufferUsage source_buffer_begin = pgBufferUsage;
    const auto minimizer_generation_storage_error = [](std::string_view message) {
        minimizer_generation_storage_error_as_cpp(message);
    };
    struct source_read_state {
        Relation source_relation = nullptr;
        Relation fragment_relation = nullptr;
        column_read_state* source_read = nullptr;
        MemoryContext work_context = nullptr;
    };
    auto* state = new source_read_state{};
    minimizer_generation_catalog_source_authorization authorization{};
    try {
        Snapshot registered_snapshot = executor_read.snapshot();
        if (identity.database_oid != MyDatabaseId
            || !OidIsValid(identity.user_relid)
            || !OidIsValid(identity.tablespace_oid)
            || !OidIsValid(identity.main_relfilenode)
            || identity.column_attno <= 0
            || identity.index_name.empty()
            || !bioseq::indexing::sql_bigint_can_represent(sequence_rowno)
            || !executor_read.matches(
                identity.database_oid, identity.tablespace_oid,
                identity.main_relfilenode, identity.user_relid)) {
            minimizer_generation_storage_error("catalog source authorization input is invalid");
        }
        state->work_context = bioseq::postgres::support::invoke_pg_as_cpp([] {
            return AllocSetContextCreate(
                CurrentMemoryContext, "bioseq current generation catalog source authorization",
                ALLOCSET_DEFAULT_SIZES);
        });
        state->source_relation = executor_read.relation();
        if (state->source_relation == nullptr
            || RelationGetRelid(state->source_relation) != identity.user_relid
            || state->source_relation->rd_locator.dbOid != identity.database_oid
            || static_cast<Oid>(state->source_relation->rd_locator.relNumber)
                != identity.main_relfilenode) {
            minimizer_generation_storage_error("catalog source relation is missing or changed");
        }
        const Oid source_tablespace =
            OidIsValid(state->source_relation->rd_rel->reltablespace)
            ? state->source_relation->rd_rel->reltablespace
            : MyDatabaseTableSpace;
        TupleDesc source_desc = RelationGetDescr(state->source_relation);
        if (source_tablespace != identity.tablespace_oid
            || identity.column_attno > source_desc->natts
            || TupleDescAttr(source_desc, identity.column_attno - 1)->attisdropped) {
            minimizer_generation_storage_error("catalog source relation/column identity changed");
        }

        MemoryContext previous_context = bioseq::postgres::support::invoke_pg_as_cpp(
            [state] { return MemoryContextSwitchTo(state->work_context); });
        const auto switch_context = [](MemoryContext context) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [context] { MemoryContextSwitchTo(context); });
        };
        List* projected = bioseq::postgres::support::invoke_pg_as_cpp(
            [column_attno = identity.column_attno] {
                return list_make1_int(column_attno);
            });
        Datum* values = bioseq::postgres::support::invoke_pg_as_cpp(
            [source_desc] {
            return static_cast<Datum*>(palloc0(
                sizeof(Datum)
                * static_cast<std::size_t>(source_desc->natts)));
        });
        bool* nulls = bioseq::postgres::support::invoke_pg_as_cpp(
            [source_desc] {
            return static_cast<bool*>(palloc0(
                sizeof(bool)
                * static_cast<std::size_t>(source_desc->natts)));
        });
        state->source_read = bioseq::postgres::support::invoke_pg_as_cpp(
            [state, source_desc, projected, registered_snapshot] {
            return begin_column_read(
                state->source_relation, source_desc, projected, NIL,
                state->work_context, registered_snapshot,
                /*randomAccess=*/true, nullptr);
        });
        const bool row_visible = bioseq::postgres::support::invoke_pg_as_cpp(
            [state, sequence_rowno, values, nulls] {
            return read_column_row_by_number(
                state->source_read, sequence_rowno, values, nulls);
        });
        if (!row_visible || nulls[identity.column_attno - 1]) {
            switch_context(previous_context);
            minimizer_generation_storage_error("catalog source row is not visible");
        }
        ++authorization.stats_.parent_rows_read;
        authorization.cold_stats_.parent_random_reads = 1;
        bioseq::pg_types::seq* source_header =
            bioseq::postgres::support::invoke_pg_as_cpp(
                [values, column_attno = identity.column_attno] {
                    return reinterpret_cast<bioseq::pg_types::seq*>(
                        PG_DETOAST_DATUM(values[column_attno - 1]));
                });
        if (!is_locator_stub(source_header)) {
            switch_context(previous_context);
            minimizer_generation_storage_error(
                "catalog source is not an epoch-bearing locator stub");
        }
        const sequence_locator locator = decode_locator_from_stub(source_header);
        const std::uint8_t alphabet_code = static_cast<std::uint8_t>(
            source_header->alphabet_code);
        const std::uint8_t storage_width =
            logical_storage_unit_byte_width(source_header);
        const std::uint16_t padding_letters = source_header->padding_letters;
        if (locator.payload_version != sequence_locator_payload_version || locator.source_epoch == 0
            || locator.reserved_16 != 0 || locator.reserved_32 != 0
            || storage_width == 0) {
            switch_context(previous_context);
            minimizer_generation_storage_error("catalog source locator is invalid");
        }

        bool exact_primary_index_scan = false;
        sequence_source_lookup_telemetry source_lookup_telemetry{};
        const auto catalog = bioseq::postgres::support::invoke_pg_as_cpp(
            [source_epoch = locator.source_epoch, registered_snapshot,
             &exact_primary_index_scan, &source_lookup_telemetry] {
            return sequence_source_version_manager::lookup(
                source_epoch, registered_snapshot,
                /*require_ready=*/true, &exact_primary_index_scan,
                &source_lookup_telemetry);
        });
        if (exact_primary_index_scan) {
            ++authorization.stats_.catalog_index_scans;
            authorization.cold_stats_.add(
                authorization.cold_stats_.source_heap_opens,
                source_lookup_telemetry.heap_opens);
            authorization.cold_stats_.add(
                authorization.cold_stats_.source_index_opens,
                source_lookup_telemetry.index_opens);
            authorization.cold_stats_.add(
                authorization.cold_stats_.source_schema_validations,
                source_lookup_telemetry.schema_validations);
            authorization.cold_stats_.add(
                authorization.cold_stats_.lock_acquires,
                source_lookup_telemetry.lock_acquires);
            authorization.cold_stats_.add(
                authorization.cold_stats_.lock_releases,
                source_lookup_telemetry.lock_releases);
        }
        if (catalog) ++authorization.stats_.catalog_rows_read;
        const std::uint64_t main_storage_id =
            bioseq::postgres::support::invoke_pg_as_cpp([state] {
                return column_storage_id(
                    state->source_relation, false);
            });
        ++authorization.stats_.storage_incarnations_checked;
        if (!catalog
            || catalog->source_epoch != locator.source_epoch
            || catalog->user_relid != identity.user_relid
            || catalog->database_oid != identity.database_oid
            || catalog->tablespace_oid != identity.tablespace_oid
            || catalog->main_relfilenode != identity.main_relfilenode
            || catalog->main_storage_id != main_storage_id
            || catalog->column_attno != identity.column_attno
            || !same_minimizer_generation_sequence_locator(catalog->locator, locator)
            || catalog->alphabet_code != alphabet_code
            || catalog->storage_unit_byte_width != storage_width
            || catalog->padding_letters != padding_letters) {
            switch_context(previous_context);
            minimizer_generation_storage_error("catalog source epoch binding disagrees");
        }
        if (locator.fragment_count == 0) {
            if (OidIsValid(catalog->fragment_relid)
                || catalog->fragment_storage_id != 0
                || locator.first_fragment_row != 0) {
                switch_context(previous_context);
                minimizer_generation_storage_error("empty catalog source sentinel disagrees");
            }
        } else {
            state->fragment_relation =
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [fragment_relid = catalog->fragment_relid] {
                return bioseq::postgres::support::try_open_postgres_relation(
                    fragment_relid, AccessShareLock);
            });
            authorization.cold_stats_.fragment_heap_opens = 1;
            authorization.cold_stats_.add(authorization.cold_stats_.lock_acquires, 1);
            const std::string expected_fragment_name = format_sub_table_name(
                identity.database_oid, identity.tablespace_oid,
                identity.main_relfilenode, identity.column_attno,
                static_cast<std::int16_t>(locator.bucket_index),
                static_cast<std::int32_t>(locator.shard_index));
            if (state->fragment_relation == nullptr
                || RelationGetNamespace(state->fragment_relation)
                    != bioseq_internal_ns()
                || expected_fragment_name
                    != RelationGetRelationName(state->fragment_relation)
                || bioseq::postgres::support::invoke_pg_as_cpp([state] {
                    return column_storage_id(
                        state->fragment_relation, false);
                })
                    != catalog->fragment_storage_id) {
                switch_context(previous_context);
                minimizer_generation_storage_error(
                    "catalog source fragment storage incarnation changed");
            }
            ++authorization.stats_.storage_incarnations_checked;
        }

        authorization.source_ = minimizer_generation_source_locator{
            .sequence_rowno = sequence_rowno,
            .locator = locator,
            .alphabet_code = alphabet_code,
            .storage_unit_byte_width = storage_width,
            .padding_letters = padding_letters,
            .storage_contract_checksum = 0,
        };
        authorization.source_content_checksum_ = catalog->source_content_checksum;
        authorization.database_oid_ = identity.database_oid;
        authorization.tablespace_oid_ = identity.tablespace_oid;
        authorization.main_relfilenode_ = identity.main_relfilenode;
        authorization.user_relid_ = identity.user_relid;
        authorization.column_attno_ = identity.column_attno;
        authorization.index_name_ = identity.index_name;
        authorization.table_name_ = format_seqidx_name(
            identity.index_name, identity.database_oid,
            identity.tablespace_oid, identity.main_relfilenode,
            identity.column_attno, locator.bucket_index, locator.shard_index);
        authorization.snapshot_ = registered_snapshot;
        authorization.valid_ = authorization.stats_.timed_equation_holds();
        if (!authorization.valid_) {
            switch_context(previous_context);
            minimizer_generation_storage_error("catalog source authorization work equation disagrees");
        }

        if (state->fragment_relation != nullptr) {
            Relation finished = std::exchange(
                state->fragment_relation, nullptr);
            bioseq::postgres::support::invoke_pg_as_cpp(
                [finished] { relation_close(finished, AccessShareLock); });
            authorization.cold_stats_.add(authorization.cold_stats_.lock_releases, 1);
        }
        bioseq::postgres::support::invoke_pg_as_cpp(
            [state] { end_column_read(state->source_read); });
        state->source_read = nullptr;
        switch_context(previous_context);
        // The executor owns this Relation and its AccessShareLock. The authorization
        // closes only its column read state and never closes the borrowed
        // descriptor.
        state->source_relation = nullptr;
        MemoryContext finished_context = std::exchange(
            state->work_context, nullptr);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [finished_context] { MemoryContextDelete(finished_context); });
        add_cold_index_elapsed(
            authorization.cold_stats_, authorization.cold_stats_.source_ns,
            source_begin_ns);
        add_cold_index_buffer_usage(authorization.cold_stats_, source_buffer_begin);
    } catch (...) {
        state->source_read = nullptr;
        state->fragment_relation = nullptr;
        state->source_relation = nullptr;
        state->work_context = nullptr;
        authorization.invalidate_();
        delete state;
        throw;
    }
    delete state;
    return authorization;
}

/// Administrative overload. Admin callers do not own an
/// executor Relation descriptor, so they retain the explicit open; the timed
/// BatchJoin production path above never enters this overload.
[[nodiscard]] inline minimizer_generation_catalog_source_authorization
authorize_minimizer_generation_catalog_source_pg(
        const minimizer_generation_administration_identity& identity,
        std::uint64_t sequence_rowno,
        Snapshot registered_snapshot) {
    Relation relation = nullptr;
    try {
        relation = bioseq::postgres::support::invoke_pg_as_cpp([&identity] {
            return bioseq::postgres::support::try_open_postgres_relation(identity.user_relid, AccessShareLock);
        });
        if (relation == nullptr) {
            minimizer_generation_storage_error_as_cpp(
                "catalog source relation is missing or changed");
        }
        borrowed_executor_read_context borrowed{
            relation, registered_snapshot};
        auto authorization = authorize_minimizer_generation_catalog_source_pg(
            identity, sequence_rowno, borrowed);
        Relation finished = std::exchange(relation, nullptr);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [finished] { relation_close(finished, AccessShareLock); });
        authorization.cold_stats_.target_relation_extra_opens = 1;
        authorization.cold_stats_.add(authorization.cold_stats_.lock_acquires, 1);
        authorization.cold_stats_.add(authorization.cold_stats_.lock_releases, 1);
        return authorization;
    } catch (const bioseq::postgres::support::postgres_error&) {
        relation = nullptr;
        throw;
    } catch (...) {
        if (relation != nullptr) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [relation] { relation_close(relation, AccessShareLock); });
        }
        throw;
    }
}

class index_storage_fragment_generation_row_sink;
class ready_minimizer_generation_token;

/// Exact receipt returned by the immutable minimizer fragment image writer. Device/inode
/// are transaction-local sealing receipts; the descriptor persists the byte
/// length and authenticated header checksum.
struct minimizer_fragment_image_file_receipt {
    std::uint64_t file_bytes = 0u;
    std::uint64_t payload_crc64 = 0u;
    std::uint64_t header_crc64 = 0u;
    dev_t device{};
    ino_t inode{};
};

/// Publication lifecycle stored in the generation descriptor catalog.
enum class minimizer_generation_storage_state : std::int16_t {
    live = 1,
    retired = 2,
};

/// Production row model for
/// bioseq_internal.minimizer_generation_storage. The SQL catalog requires the
/// complete relation locator; defensive in-memory optionals prevent a malformed
/// raw row from acquiring relation authority through a default value.
struct minimizer_generation_storage_descriptor {
    Oid database_oid = InvalidOid;
    Oid tablespace_oid = InvalidOid;
    Oid main_relfilenode = InvalidOid;
    Oid user_relid = InvalidOid;
    std::int16_t column_attno = 0;
    std::string index_name{};
    std::uint64_t query_key = 0u;
    std::uint64_t generation_id = 0u;
    std::optional<Oid> payload_relation_oid{};
    std::optional<Oid> payload_database_oid{};
    std::optional<Oid> payload_tablespace_oid{};
    std::optional<Oid> payload_relfilenode{};
    std::optional<std::string> payload_relation_name{};
    std::uint64_t image_bytes = 0u;
    std::uint64_t header_checksum = 0u;
    std::optional<std::uint64_t> retire_full_xid{};
    minimizer_generation_storage_state state =
        minimizer_generation_storage_state::live;
};

/// Field count of the generated descriptor-row codec.
inline constexpr std::size_t k_storage_descriptor_attribute_count =
    minimizer_generation_storage_codec::field_count;

/// Report whether a descriptor belongs to the requested catalog identity.
[[nodiscard]] inline bool exact_storage_descriptor_identity(
        const minimizer_generation_storage_descriptor& descriptor,
        const minimizer_generation_administration_identity& identity,
        const std::uint64_t query_key) noexcept {
    return descriptor.database_oid == identity.database_oid
        && descriptor.tablespace_oid == identity.tablespace_oid
        && descriptor.main_relfilenode == identity.main_relfilenode
        && descriptor.user_relid == identity.user_relid
        && descriptor.column_attno == identity.column_attno
        && descriptor.index_name == identity.index_name
        && descriptor.query_key == query_key;
}

/// Report whether every relation-locator field carries valid authority.
[[nodiscard]] inline bool has_exact_storage_descriptor_relation_shape(
        const minimizer_generation_storage_descriptor& descriptor) noexcept {
    return descriptor.payload_relation_oid
        && OidIsValid(*descriptor.payload_relation_oid)
        && descriptor.payload_database_oid
        && *descriptor.payload_database_oid == descriptor.database_oid
        && descriptor.payload_tablespace_oid
        && OidIsValid(*descriptor.payload_tablespace_oid)
        && descriptor.payload_relfilenode
        && OidIsValid(*descriptor.payload_relfilenode)
        && descriptor.payload_relation_name
        && !descriptor.payload_relation_name->empty();
}

/// Immutable receipt for one atomically published grouped-SoA head.
struct ready_minimizer_generation_published_head_receipt {
    std::string table_name;
    std::uint64_t query_key = 0;
    minimizer_generation_head head{};
    std::array<std::byte, minimizer_generation_head::encoded_bytes>
        head_bytes{};
    std::optional<std::uint64_t> replaced_generation_id{};
};

template <class p_receipt>
concept reports_ready_minimizer_generation_acceptance =
    requires(const p_receipt& receipt) { receipt.published(); }
    || requires(const p_receipt& receipt) { receipt.execution_accepted(); }
    || requires(const p_receipt& receipt) { receipt.accepted(); };

static_assert(!reports_ready_minimizer_generation_acceptance<
    ready_minimizer_generation_published_head_receipt>);

/// Consume a validated token and atomically replace the active head.
[[nodiscard]] inline ready_minimizer_generation_published_head_receipt
publish_ready_minimizer_generation_head_pg(
    ready_minimizer_generation_token&& token);

/// Move-only authorization that one exact relation/source/generation was completely
/// reread under an own-write-visible registered snapshot. Construction is
/// private to the generation sink's full validation pass.
class ready_minimizer_generation_token {
private:
    minimizer_generation_administration_identity identity_{};
    Oid sidecar_relation_oid_ = InvalidOid;
    RelFileLocator sidecar_locator_{};
    std::uint64_t advisory_lock_key_ = 0;
    std::uint64_t query_key_ = 0;
    std::uint64_t generation_id_ = 0;
    std::uint64_t source_identity_ = 0;
    std::uint64_t source_content_checksum_ = 0;
    std::uint64_t storage_contract_checksum_ = 0;
    TransactionId transaction_id_ = InvalidTransactionId;
    std::array<std::byte, minimizer_generation_head::encoded_bytes>
        head_bytes_{};
    std::uint64_t head_row_checksum_ = 0;
    std::optional<minimizer_fragment_image_file_receipt> file_receipt_{};
    bool valid_ = false;

    ready_minimizer_generation_token(
            minimizer_generation_administration_identity identity,
            Oid sidecar_relation_oid,
            RelFileLocator sidecar_locator,
            std::uint64_t advisory_lock_key,
            std::uint64_t query_key,
            std::uint64_t generation_id,
            std::uint64_t source_identity,
            std::uint64_t source_content_checksum,
            std::uint64_t storage_contract_checksum,
            TransactionId transaction_id,
            std::span<const std::byte> head_bytes,
            std::uint64_t head_row_checksum,
            std::optional<minimizer_fragment_image_file_receipt> file_receipt)
        : identity_(std::move(identity)),
          sidecar_relation_oid_(sidecar_relation_oid),
          sidecar_locator_(sidecar_locator),
          advisory_lock_key_(advisory_lock_key),
          query_key_(query_key),
          generation_id_(generation_id),
          source_identity_(source_identity),
          source_content_checksum_(source_content_checksum),
          storage_contract_checksum_(storage_contract_checksum),
          transaction_id_(transaction_id),
          head_row_checksum_(head_row_checksum),
          file_receipt_(std::move(file_receipt)),
          valid_(true) {
        std::copy(head_bytes.begin(), head_bytes.end(), head_bytes_.begin());
    }

    void invalidate() noexcept {
        identity_.release_owned_strings();
        sidecar_relation_oid_ = InvalidOid;
        sidecar_locator_ = {};
        advisory_lock_key_ = 0;
        query_key_ = 0;
        generation_id_ = 0;
        source_identity_ = 0;
        source_content_checksum_ = 0;
        storage_contract_checksum_ = 0;
        transaction_id_ = InvalidTransactionId;
        head_bytes_.fill(std::byte{0});
        head_row_checksum_ = 0;
        file_receipt_.reset();
        valid_ = false;
    }

    friend class index_storage_fragment_generation_row_sink;
    friend ready_minimizer_generation_published_head_receipt
    publish_ready_minimizer_generation_head_pg(
        ready_minimizer_generation_token&& token);

public:
    ready_minimizer_generation_token() = delete;
    ready_minimizer_generation_token(const ready_minimizer_generation_token&) = delete;
    ready_minimizer_generation_token& operator=(const ready_minimizer_generation_token&) = delete;
    ready_minimizer_generation_token(ready_minimizer_generation_token&& other) noexcept
        : identity_(std::move(other.identity_)),
          sidecar_relation_oid_(other.sidecar_relation_oid_),
          sidecar_locator_(other.sidecar_locator_),
          advisory_lock_key_(other.advisory_lock_key_),
          query_key_(other.query_key_),
          generation_id_(other.generation_id_),
          source_identity_(other.source_identity_),
          source_content_checksum_(other.source_content_checksum_),
          storage_contract_checksum_(other.storage_contract_checksum_),
          transaction_id_(other.transaction_id_),
          head_bytes_(other.head_bytes_),
          head_row_checksum_(other.head_row_checksum_),
          file_receipt_(std::move(other.file_receipt_)),
          valid_(other.valid_) {
        other.invalidate();
    }
    ready_minimizer_generation_token& operator=(ready_minimizer_generation_token&& other) noexcept {
        if (this != &other) {
            invalidate();
            identity_ = std::move(other.identity_);
            sidecar_relation_oid_ = other.sidecar_relation_oid_;
            sidecar_locator_ = other.sidecar_locator_;
            advisory_lock_key_ = other.advisory_lock_key_;
            query_key_ = other.query_key_;
            generation_id_ = other.generation_id_;
            source_identity_ = other.source_identity_;
            source_content_checksum_ = other.source_content_checksum_;
            storage_contract_checksum_ = other.storage_contract_checksum_;
            transaction_id_ = other.transaction_id_;
            head_bytes_ = other.head_bytes_;
            head_row_checksum_ = other.head_row_checksum_;
            file_receipt_ = std::move(other.file_receipt_);
            valid_ = other.valid_;
            other.invalidate();
        }
        return *this;
    }
    ~ready_minimizer_generation_token() = default;
};

static_assert(!std::is_default_constructible_v<ready_minimizer_generation_token>);
static_assert(!std::is_copy_constructible_v<ready_minimizer_generation_token>);
static_assert(!std::is_copy_assignable_v<ready_minimizer_generation_token>);
static_assert(std::is_nothrow_move_constructible_v<ready_minimizer_generation_token>);
static_assert(std::is_nothrow_move_assignable_v<ready_minimizer_generation_token>);

/// Read the sole LIVE descriptor for one exact storage identity through the
/// catalog's partial unique index.  The caller owns both relation locks and
/// the registered MVCC snapshot; this helper neither substitutes SnapshotSelf
/// nor consults mutable payload-routing state.
[[nodiscard]] inline minimizer_generation_storage_descriptor
read_live_minimizer_generation_storage_descriptor_pg(
        Relation storage_relation,
        Relation live_index_relation,
        Snapshot registered_snapshot,
        const minimizer_generation_administration_identity& identity,
        const std::uint64_t query_key,
        const std::uint64_t expected_generation_id) {
    using namespace bioseq::indexing;
    if (storage_relation == nullptr || live_index_relation == nullptr
        || registered_snapshot == nullptr
        || !IsMVCCSnapshot(registered_snapshot)
        || registered_snapshot->regd_count == 0
        || live_index_relation->rd_index == nullptr
        || live_index_relation->rd_index->indrelid
            != RelationGetRelid(storage_relation)
        || std::string_view{RelationGetRelationName(live_index_relation)}
            != "minimizer_generation_storage_one_live"
        || !OidIsValid(identity.database_oid)
        || !OidIsValid(identity.tablespace_oid)
        || !OidIsValid(identity.main_relfilenode)
        || !OidIsValid(identity.user_relid)
        || identity.column_attno <= 0
        || identity.index_name != k_minimizers_per_fragment_index_name
        || !sql_bigint_can_represent(query_key)
        || expected_generation_id == 0u
        || !sql_bigint_can_represent(expected_generation_id)) {
        minimizer_generation_storage_error_as_cpp(
            "LIVE generation descriptor lookup binding is invalid");
    }

    NameData index_name{};
    bioseq::postgres::support::invoke_pg_as_cpp([&index_name, &identity] {
        namestrcpy(&index_name, identity.index_name.c_str());
    });
    std::array<ScanKeyData, 7> keys{
        minimizer_generation_storage_codec::scan_key<
            &minimizer_generation_storage_catalog_row::database_oid>(
                identity.database_oid),
        minimizer_generation_storage_codec::scan_key<
            &minimizer_generation_storage_catalog_row::tablespace_oid>(
                identity.tablespace_oid),
        minimizer_generation_storage_codec::scan_key<
            &minimizer_generation_storage_catalog_row::main_relfilenode>(
                identity.main_relfilenode),
        minimizer_generation_storage_codec::scan_key<
            &minimizer_generation_storage_catalog_row::user_relid>(
                identity.user_relid),
        minimizer_generation_storage_codec::scan_key<
            &minimizer_generation_storage_catalog_row::column_attno>(
                identity.column_attno),
        {},
        minimizer_generation_storage_codec::scan_key<
            &minimizer_generation_storage_catalog_row::query_key>(
                static_cast<std::int64_t>(query_key)),
    };
    ScanKeyInit(
        &keys[5],
        minimizer_generation_storage_codec::attribute_number<
            &minimizer_generation_storage_catalog_row::index_name>(),
        BTEqualStrategyNumber,
        minimizer_generation_storage_codec::field<
            &minimizer_generation_storage_catalog_row::index_name>::
                comparison_procedure(
                    bioseq::postgres::support::scan_comparison::equal),
        NameGetDatum(&index_name));

    SysScanDesc scan = nullptr;
    try {
        scan = bioseq::postgres::support::invoke_pg_as_cpp(
            [storage_relation, live_index_relation, registered_snapshot, &keys] {
            return systable_beginscan_ordered(
                storage_relation, live_index_relation,
                registered_snapshot,
                static_cast<int>(keys.size()), keys.data());
        });
        HeapTuple tuple = bioseq::postgres::support::invoke_pg_as_cpp([scan] {
            return systable_getnext_ordered(scan, ForwardScanDirection);
        });
        if (!HeapTupleIsValid(tuple)) {
            minimizer_generation_storage_error_as_cpp(
                "LIVE generation storage descriptor is missing");
        }

        std::optional<minimizer_generation_storage_catalog_row>
            decoded_catalog;
        try {
            decoded_catalog.emplace(
                minimizer_generation_storage_codec::deform(
                    tuple,
                    RelationGetDescr(storage_relation),
                    CurrentMemoryContext));
        } catch (const bioseq::postgres::support::row_codec_error& error) {
            minimizer_generation_storage_error_as_cpp(error.what());
        }
        const minimizer_generation_storage_catalog_row& catalog =
            *decoded_catalog;

        const std::int64_t query_key_sql = catalog.query_key;
        const std::int64_t generation_id_sql =
            catalog.generation_id;
        const std::int16_t storage_kind_sql = catalog.provider;
        const std::int64_t image_bytes_sql = catalog.image_bytes;
        const std::int16_t state_sql = catalog.state;
        if (query_key_sql < 0 || generation_id_sql <= 0
            || image_bytes_sql
                < static_cast<std::int64_t>(k_minimizer_fragment_image_header_bytes)
            || storage_kind_sql
                != static_cast<std::int16_t>(
                    bioseq::indexing::
                        k_minimizer_fragment_image_relation_backed_storage_kind)) {
            minimizer_generation_storage_error_as_cpp(
                "LIVE generation descriptor scalar shape is invalid");
        }

        minimizer_generation_storage_descriptor descriptor{
            .database_oid = catalog.database_oid,
            .tablespace_oid = catalog.tablespace_oid,
            .main_relfilenode = catalog.main_relfilenode,
            .user_relid = catalog.user_relid,
            .column_attno = catalog.column_attno,
            .index_name = NameStr(catalog.index_name),
            .query_key = static_cast<std::uint64_t>(query_key_sql),
            .generation_id = static_cast<std::uint64_t>(generation_id_sql),
            .payload_relation_oid =
                std::optional<Oid>{catalog.payload_relation_oid},
            .payload_database_oid =
                std::optional<Oid>{catalog.payload_database_oid},
            .payload_tablespace_oid =
                std::optional<Oid>{catalog.payload_tablespace_oid},
            .payload_relfilenode =
                std::optional<Oid>{catalog.payload_relfilenode},
            .payload_relation_name = std::optional<std::string>{
                NameStr(catalog.payload_relation_name)},
            .image_bytes = static_cast<std::uint64_t>(image_bytes_sql),
            .header_checksum = checksum_from_pg_bigint(
                catalog.header_checksum),
            .retire_full_xid = catalog.retire_full_xid
                ? std::optional<std::uint64_t>{
                    U64FromFullTransactionId(
                        *catalog.retire_full_xid)}
                : std::nullopt,
            .state = static_cast<minimizer_generation_storage_state>(
                state_sql),
        };
        if (!exact_storage_descriptor_identity(
                descriptor, identity, query_key)
            || descriptor.generation_id != expected_generation_id
            || descriptor.state
                != minimizer_generation_storage_state::live
            || descriptor.retire_full_xid
            || !has_exact_storage_descriptor_relation_shape(descriptor)) {
            minimizer_generation_storage_error_as_cpp(
                "LIVE generation descriptor disagrees with the active head");
        }

        HeapTuple duplicate = bioseq::postgres::support::invoke_pg_as_cpp([scan] {
            return systable_getnext_ordered(scan, ForwardScanDirection);
        });
        if (HeapTupleIsValid(duplicate)) {
            minimizer_generation_storage_error_as_cpp(
                "LIVE generation descriptor lookup returned duplicates");
        }
        bioseq::postgres::support::invoke_pg_as_cpp(
            [scan] { systable_endscan_ordered(scan); });
        scan = nullptr;
        return descriptor;
    } catch (const bioseq::postgres::support::postgres_error&) {
        scan = nullptr;
        throw;
    } catch (...) {
        if (scan != nullptr) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [scan] { systable_endscan_ordered(scan); });
            scan = nullptr;
        }
        throw;
    }
}

/// Form one immutable current-generation payload/directory/manifest tuple. The caller
/// transfers the tuple into a TTSOpsHeapTuple slot and flushes a bounded group
/// with CatalogTuplesMultiInsertWithInfo, PostgreSQL 17's supported catalog
/// heap-multi-insert + index-maintenance API. This helper never inserts, probes
/// for, or replaces a row by itself.
[[nodiscard]] inline std::expected<HeapTuple, minimizer_generation_encoding_error>
form_minimizer_generation_row_pg(
        Relation relation,
        std::uint64_t query_key,
        const validated_minimizer_generation_row& validated_row,
        bool inject_failure = true) {
    const minimizer_generation_row_view& row = validated_row.row();
    if (relation == nullptr
        || !bioseq::indexing::sql_bigint_can_represent(query_key)) {
        return std::unexpected(minimizer_generation_encoding_error::generation_id);
    }
    bytea* blob = bioseq::postgres::support::invoke_pg_as_cpp([&row] {
        return reinterpret_cast<bytea*>(
            palloc(VARHDRSZ + row.bytes.size()));
    });
    SET_VARSIZE(blob, VARHDRSZ + row.bytes.size());
    std::memcpy(VARDATA(blob), row.bytes.data(), row.bytes.size());
    bioseq::postgres::support::pg_datum_list<8> values{};
    values.set(k_minimizer_generation_attribute_query_key, static_cast<std::int64_t>(query_key));
    values.set(k_minimizer_generation_attribute_generation_id,
        static_cast<std::int64_t>(row.address.generation_id));
    values.set(k_minimizer_generation_attribute_position_segment, row.address.position_segment);
    values.set(k_minimizer_generation_attribute_sub_key,
        static_cast<std::int64_t>(row.address.sub_key));
    values.set(k_minimizer_generation_attribute_chunk_number,
        static_cast<std::int32_t>(row.address.chunk_no));
    values.set(k_minimizer_generation_attribute_chunk_bytes,
        static_cast<std::int32_t>(row.bytes.size()));
    values.set(k_minimizer_generation_attribute_checksum,
        checksum_to_pg_bigint(row.row_checksum));
    values.set_raw(k_minimizer_generation_attribute_blob,
        bioseq::postgres::support::pg_datum::from_pointer(blob));
    HeapTuple tuple = bioseq::postgres::support::invoke_pg_as_cpp(
        [&values, relation] {
            return values.form_tuple(RelationGetDescr(relation));
        });
    if (inject_failure) {
        inject_minimizer_generation_failure_as_cpp(
            minimizer_generation_failure_injection::generation_tuple_form,
            "generation_tuple_form");
    }
    bioseq::postgres::support::invoke_pg_as_cpp([blob] { pfree(blob); });
    return tuple;
}

/// Exact PostgreSQL ABI validation result for the S1 one-page PLAIN payload contract.
struct pg_inline_payload_layout {
    std::uint32_t block_bytes = 0;
    std::uint32_t max_heap_tuple_bytes = 0;
    std::uint32_t fixed_tuple_bytes = 0;
    std::uint32_t payload_bytes = 0;
    std::uint32_t alignment_multiple = 0;
    std::uint32_t full_tuple_bytes = 0;
};

[[nodiscard]] inline pg_inline_payload_layout
verify_pg_inline_payload_layout(Relation relation) {
    static std::optional<pg_inline_payload_layout> cached;
    if (cached) return *cached;
    if (relation == nullptr || BLCKSZ != 8192
        || RelationGetDescr(relation) == nullptr
        || RelationGetDescr(relation)->natts
            != static_cast<int>(k_minimizer_generation_index_attribute_descriptors.size())) {
        minimizer_generation_storage_error_as_cpp(
            "grouped-SoA storage requires BLCKSZ=8192 and the exact tuple descriptor");
    }
    constexpr std::uint32_t k_payload_bytes =
        bioseq::indexing::k_grouped_soa_extent_payload_bytes;
    constexpr std::uint32_t k_alignment_multiple = 96u;
    static_assert(k_payload_bytes % k_alignment_multiple == 0u);
    static_assert(k_payload_bytes % 16u == 0u);
    static_assert(k_payload_bytes % 12u == 0u);
    static_assert(k_payload_bytes % 8u == 0u);
    static_assert(k_payload_bytes % 4u == 0u);

    std::array<std::byte, k_payload_bytes> payload{};
    const index_chunk_address address{
        .generation_id = 1u,
        .position_segment = 0,
        .sub_key = 0,
        .chunk_no = 0u,
    };
    const minimizer_generation_row_view row{
        .address = address,
        .bytes = payload,
        .row_checksum = bioseq::indexing::crc64_index_chunk_row(
            address.generation_id, address.position_segment,
            address.sub_key, address.chunk_no, k_payload_bytes, payload),
    };
    auto validated = validate_minimizer_generation_row_for_write(1u, row);
    if (!validated) {
        minimizer_generation_storage_error_as_cpp(
            "grouped-SoA representative row validation failed");
    }
    auto tuple = form_minimizer_generation_row_pg(
        relation, 1u, *validated, /*inject_failure=*/false);
    if (!tuple) {
        minimizer_generation_storage_error_as_cpp(
            "grouped-SoA representative tuple formation failed");
    }
    const std::uint32_t tuple_bytes = (*tuple)->t_len;
    heap_freetuple(*tuple);
    if (tuple_bytes < k_payload_bytes) {
        minimizer_generation_storage_error_as_cpp(
            "grouped-SoA representative tuple length underflow");
    }
    const std::uint32_t fixed_tuple_bytes = tuple_bytes - k_payload_bytes;
    const std::uint32_t max_payload_by_tuple =
        static_cast<std::uint32_t>(MaxHeapTupleSize) - fixed_tuple_bytes;
    const std::uint32_t largest_aligned_payload =
        max_payload_by_tuple / k_alignment_multiple * k_alignment_multiple;
    const std::uint32_t page_payload_capacity =
        BLCKSZ - SizeOfPageHeaderData;
    if (tuple_bytes != 8140u
        || tuple_bytes > static_cast<std::uint32_t>(MaxHeapTupleSize)
        || MAXALIGN(tuple_bytes) + sizeof(ItemIdData) > page_payload_capacity
        || largest_aligned_payload != k_payload_bytes) {
        minimizer_generation_storage_error_as_cpp(
            "grouped-SoA 8064-byte payload does not satisfy the one-page tuple equation");
    }
    cached = pg_inline_payload_layout{
        .block_bytes = BLCKSZ,
        .max_heap_tuple_bytes = MaxHeapTupleSize,
        .fixed_tuple_bytes = fixed_tuple_bytes,
        .payload_bytes = k_payload_bytes,
        .alignment_multiple = k_alignment_multiple,
        .full_tuple_bytes = tuple_bytes,
    };
    return *cached;
}

/// Callback-scoped payload row with address-bound checksum authority.
struct minimizer_generation_loaded_row {
    std::uint64_t query_key = 0;
    index_chunk_address address{};
    std::uint32_t chunk_bytes = 0;
    std::uint64_t checksum = 0;
    // The schema is frozen to PLAIN and the loader rejects every external or
    // compressed datum. Payload bytes either borrow the current scan slot or
    // one admitted batch; neither form needs detoast ownership or pfree.
    std::span<const std::byte> bytes;
    std::optional<validated_minimizer_generation_row_receipt>
        validated_receipt;

    void release() noexcept {
        // The receipt seals a callback-scoped span into blob storage. Destroy
        // that authority before the scan slot may be advanced.
        validated_receipt.reset();
        bytes = {};
    }
};

/// One deformation of the frozen eight-column heap tuple.  This object is
/// deliberately trivial: the hot full-generation scan can form it inside one
/// outer PostgreSQL error boundary without a longjmp crossing a C++ owner.
struct minimizer_generation_deformed_row {
    HeapTuple tuple = nullptr;
    std::array<Datum, k_minimizer_generation_index_attribute_descriptors.size()> values{};
    std::array<bool, k_minimizer_generation_index_attribute_descriptors.size()> nulls{};
};

static_assert(std::is_trivially_copyable_v<minimizer_generation_deformed_row>);
static_assert(std::is_trivially_destructible_v<minimizer_generation_deformed_row>);

/// Trivial optional row returned from one protected ordered-scan step.
struct minimizer_generation_next_deformed_row {
    minimizer_generation_deformed_row row{};
    bool found = false;
};

static_assert(std::is_trivially_copyable_v<minimizer_generation_next_deformed_row>);
static_assert(std::is_trivially_destructible_v<minimizer_generation_next_deformed_row>);

/// PostgreSQL leaf: deform all fixed attributes once instead of entering one
/// PG_TRY/sigsetjmp bridge per heap_getattr call.  Call either from an existing
/// invoke_pg_as_cpp boundary or through deform_minimizer_generation_row_or_error().
[[nodiscard]] inline minimizer_generation_deformed_row deform_minimizer_generation_row_pg_leaf(
        Relation relation,
        HeapTuple tuple) {
    minimizer_generation_deformed_row deformed{.tuple = tuple};
    heap_deform_tuple(tuple, RelationGetDescr(relation),
        deformed.values.data(), deformed.nulls.data());
    return deformed;
}

/// Standalone deformation bridge used by metadata, publication validation,
/// and selective reads. The full-generation route instead combines its fetch
/// and deformation through next_minimizer_generation_row_or_error().
[[nodiscard]] inline minimizer_generation_deformed_row deform_minimizer_generation_row_or_error(
        Relation relation,
        HeapTuple tuple) {
    return bioseq::postgres::support::invoke_pg_as_cpp(
        [relation, tuple] {
            return deform_minimizer_generation_row_pg_leaf(relation, tuple);
        });
}

/// Fetch and deform one ordered-scan row under one PostgreSQL boundary. The
/// returned object is trivial; authentication and arbitrary sinks run only
/// after PG_END_TRY has restored PostgreSQL's exception stack.
[[nodiscard]] inline minimizer_generation_next_deformed_row next_minimizer_generation_row_or_error(
        Relation relation,
        SysScanDesc scan) {
    return bioseq::postgres::support::invoke_pg_as_cpp(
        [relation, scan] {
            HeapTuple tuple = systable_getnext_ordered(
                scan, ForwardScanDirection);
            if (!HeapTupleIsValid(tuple)) return minimizer_generation_next_deformed_row{};
            return minimizer_generation_next_deformed_row{
                .row = deform_minimizer_generation_row_pg_leaf(relation, tuple),
                .found = true,
            };
        });
}

/// Trivial borrowed projection used while a PostgreSQL sigsetjmp frame is
/// active.  It performs no allocation, owns no receipt, and reports semantic
/// failure as a static message for the C++ caller to raise after leaving the
/// PostgreSQL boundary.
struct minimizer_generation_projected_row {
    std::uint64_t query_key = 0;
    index_chunk_address address{};
    std::uint32_t chunk_bytes = 0;
    std::uint64_t checksum = 0;
    std::span<const std::byte> bytes{};
    const char* error = nullptr;
};

static_assert(std::is_trivially_copyable_v<minimizer_generation_projected_row>);
static_assert(std::is_trivially_destructible_v<minimizer_generation_projected_row>);

/// Decode the frozen eight-column projection and validate inline PLAIN/length
/// shape without constructing any C++ authority object.  Address-bound CRC
/// validation remains outside PostgreSQL error frames.
[[nodiscard]] inline minimizer_generation_projected_row project_minimizer_generation_deformed_row(
        const minimizer_generation_deformed_row& deformed) noexcept {
    const auto read_attribute = [&deformed](AttrNumber attribute_number) {
        return deformed.values[static_cast<std::size_t>(
            attribute_number - 1)];
    };
    const auto attribute_is_null = [&deformed](AttrNumber attribute_number) {
        return deformed.nulls[static_cast<std::size_t>(
            attribute_number - 1)];
    };
    const auto failed = [](const char* message) noexcept {
        return minimizer_generation_projected_row{.error = message};
    };
    const Datum query_key_datum = read_attribute(k_minimizer_generation_attribute_query_key);
    if (attribute_is_null(k_minimizer_generation_attribute_query_key)) {
        return failed("row query key is NULL");
    }
    const std::int64_t signed_query_key = DatumGetInt64(query_key_datum);
    if (signed_query_key < 0) return failed("row query key is negative");
    const Datum generation_datum = read_attribute(k_minimizer_generation_attribute_generation_id);
    if (attribute_is_null(k_minimizer_generation_attribute_generation_id)) {
        return failed("row generation ID is NULL");
    }
    const std::int64_t signed_generation_id = DatumGetInt64(generation_datum);
    if (signed_generation_id < 0) {
        return failed("row generation ID is negative");
    }
    const Datum position_datum = read_attribute(k_minimizer_generation_attribute_position_segment);
    if (attribute_is_null(k_minimizer_generation_attribute_position_segment)) {
        return failed("row position segment is NULL");
    }
    const std::int32_t position_segment = DatumGetInt32(position_datum);
    const Datum sub_key_datum = read_attribute(k_minimizer_generation_attribute_sub_key);
    if (attribute_is_null(k_minimizer_generation_attribute_sub_key)) {
        return failed("row sub-key is NULL");
    }
    const std::int64_t sub_key = DatumGetInt64(sub_key_datum);
    const Datum chunk_no_datum = read_attribute(k_minimizer_generation_attribute_chunk_number);
    if (attribute_is_null(k_minimizer_generation_attribute_chunk_number)) {
        return failed("row chunk number is NULL");
    }
    const std::int32_t signed_chunk_no = DatumGetInt32(chunk_no_datum);
    if (signed_chunk_no < 0) return failed("row chunk number is negative");
    const Datum chunk_bytes_datum = read_attribute(k_minimizer_generation_attribute_chunk_bytes);
    if (attribute_is_null(k_minimizer_generation_attribute_chunk_bytes)) {
        return failed("row byte count is NULL");
    }
    const std::int32_t signed_chunk_bytes = DatumGetInt32(chunk_bytes_datum);
    if (signed_chunk_bytes <= 0) {
        return failed("row byte count is non-positive");
    }
    const Datum checksum_datum = read_attribute(k_minimizer_generation_attribute_checksum);
    if (attribute_is_null(k_minimizer_generation_attribute_checksum)) {
        return failed("row checksum is NULL");
    }
    const std::int64_t signed_checksum = DatumGetInt64(checksum_datum);
    const Datum blob_datum = read_attribute(k_minimizer_generation_attribute_blob);
    if (attribute_is_null(k_minimizer_generation_attribute_blob)) return failed("row blob is NULL");
    auto* original_blob = reinterpret_cast<varlena*>(
        DatumGetPointer(blob_datum));
    if (original_blob == nullptr) return failed("row blob pointer is NULL");
    if (VARATT_IS_EXTERNAL(original_blob)
        || VARATT_IS_COMPRESSED(original_blob)) {
        return failed("PLAIN row contains toasted/compressed data");
    }
    auto* inline_blob = reinterpret_cast<bytea*>(original_blob);
    const std::size_t payload_bytes = VARSIZE_ANY_EXHDR(inline_blob);
    if (payload_bytes != static_cast<std::size_t>(signed_chunk_bytes)) {
        return failed("row byte count disagrees with BYTEA length");
    }
    const index_chunk_address address{
        .generation_id = static_cast<std::uint64_t>(signed_generation_id),
        .position_segment = position_segment,
        .sub_key = sub_key,
        .chunk_no = static_cast<std::uint32_t>(signed_chunk_no),
    };
    const std::span<const std::byte> bytes{
        reinterpret_cast<const std::byte*>(VARDATA_ANY(inline_blob)),
        payload_bytes};
    const std::uint64_t checksum = checksum_from_pg_bigint(signed_checksum);
    return minimizer_generation_projected_row{
        .query_key = static_cast<std::uint64_t>(signed_query_key),
        .address = address,
        .chunk_bytes = static_cast<std::uint32_t>(signed_chunk_bytes),
        .checksum = checksum,
        .bytes = bytes,
    };
}

/// Bound simultaneous heap-buffer pins while amortizing PostgreSQL's
/// PG_TRY/sigsetjmp bridge. At the measured 562,075-row production geometry,
/// K=128 reduces the hot-path boundary count to at most 4,392 without copying
/// or materializing the 4.46-GB payload image.
inline constexpr std::size_t k_minimizer_generation_full_scan_pinned_batch_rows = 128;

/// Projected row paired with the TID of its independently pinned tuple.
struct pinned_minimizer_generation_projected_row {
    minimizer_generation_projected_row projected{};
    ItemPointerData tid{};
};

static_assert(std::is_trivially_copyable_v<pinned_minimizer_generation_projected_row>);
static_assert(std::is_trivially_destructible_v<pinned_minimizer_generation_projected_row>);

/// Trivial output from one PostgreSQL boundary. Each projection borrows the
/// independently pinned BufferHeapTuple slot at the same ordinal. Receipts,
/// CRC work and arbitrary sinks are deliberately constructed only after the
/// boundary has been left.
struct pinned_minimizer_generation_projected_row_batch {
    std::array<pinned_minimizer_generation_projected_row,
        k_minimizer_generation_full_scan_pinned_batch_rows> rows{};
    std::size_t size = 0;
    bool eof = false;
};

static_assert(std::is_trivially_copyable_v<pinned_minimizer_generation_projected_row_batch>);
static_assert(std::is_trivially_destructible_v<
    pinned_minimizer_generation_projected_row_batch>);

/// Keep systable_getnext_ordered's concurrent-abort contract while using
/// caller-owned slots. PostgreSQL keeps this helper private to genam.c, so the
/// extension mirrors its PG17 implementation inside the same protected leaf.
inline void handle_minimizer_generation_concurrent_abort_pg_leaf() {
    if (TransactionIdIsValid(CheckXidAlive)
        && !TransactionIdIsInProgress(CheckXidAlive)
        && !TransactionIdDidCommit(CheckXidAlive)) {
        ereport(ERROR,
            (errcode(ERRCODE_TRANSACTION_ROLLBACK),
             errmsg("transaction aborted during system catalog scan")));
    }
}

/// Fetch, deform and project up to K rows under one PostgreSQL error boundary.
/// Every slot owns its buffer pin until the caller has consumed the complete
/// batch. A semantic projection error stops the fill at that row so its error
/// identity remains the first observable failure, matching single-row reads.
[[nodiscard]] inline pinned_minimizer_generation_projected_row_batch
next_pinned_minimizer_generation_projected_row_batch_or_error(
        Relation relation,
        SysScanDesc scan,
        const std::array<TupleTableSlot*,
            k_minimizer_generation_full_scan_pinned_batch_rows>& slots) {
    pinned_minimizer_generation_projected_row_batch batch{};
    bioseq::postgres::support::invoke_pg_as_cpp(
        [relation, scan, &slots, &batch] {
            Assert(scan != nullptr);
            Assert(scan->irel != nullptr);

            // All receipts and callbacks from the preceding batch are gone
            // before this function is entered, so its buffer pins may now be
            // released in bulk before the slots are reused.
            for (TupleTableSlot* slot : slots) {
                Assert(slot != nullptr);
                ExecClearTuple(slot);
            }

            for (std::size_t ordinal = 0;
                 ordinal < slots.size(); ++ordinal) {
                TupleTableSlot* slot = slots[ordinal];
                HeapTuple tuple = nullptr;
                if (index_getnext_slot(
                        scan->iscan, ForwardScanDirection, slot)) {
                    tuple = ExecFetchSlotHeapTuple(slot, false, nullptr);
                }
                if (HeapTupleIsValid(tuple) && scan->iscan->xs_recheck) {
                    elog(ERROR,
                        "system catalog scans with lossy index conditions are not implemented");
                }
                handle_minimizer_generation_concurrent_abort_pg_leaf();
                if (!HeapTupleIsValid(tuple)) {
                    batch.eof = true;
                    return;
                }

                pinned_minimizer_generation_projected_row& output =
                    batch.rows[batch.size];
                output.tid = tuple->t_self;
                output.projected = project_minimizer_generation_deformed_row(
                    deform_minimizer_generation_row_pg_leaf(relation, tuple));
                ++batch.size;
                if (output.projected.error != nullptr) return;
            }
        });
    return batch;
}

/// Validate the address-bound row checksum only after PostgreSQL deformation
/// has completed.  Callers outside a PG boundary may safely own the receipt
/// and invoke arbitrary C++ sinks.
[[nodiscard]] inline minimizer_generation_loaded_row load_minimizer_generation_projected_row_or_error(
        const minimizer_generation_projected_row& projected) {
    if (projected.error != nullptr) {
        minimizer_generation_storage_error_as_cpp(projected.error);
    }
    auto validated_receipt =
        bioseq::indexing::validate_minimizer_generation_row_receipt(
            minimizer_generation_row_view{
                .address = projected.address,
                .bytes = projected.bytes,
                .row_checksum = projected.checksum,
            });
    if (!validated_receipt) {
        minimizer_generation_storage_error_as_cpp("row checksum disagrees");
    }
    return minimizer_generation_loaded_row{
        .query_key = projected.query_key,
        .address = projected.address,
        .chunk_bytes = projected.chunk_bytes,
        .checksum = projected.checksum,
        .bytes = projected.bytes,
        .validated_receipt = std::move(*validated_receipt),
    };
}

using validated_minimizer_generation_row_receipt_batch = std::array<
    std::optional<validated_minimizer_generation_row_receipt>,
    k_minimizer_generation_full_scan_pinned_batch_rows>;

/// Minimum batch size eligible for storage-neutral parallel authentication.
inline constexpr std::size_t k_minimizer_generation_parallel_row_authentication_min_rows = 32u;
/// Row grain used by parallel checksum authentication.
inline constexpr std::size_t k_minimizer_generation_parallel_row_authentication_grain_rows = 8u;

/// Destroy every row authority before its pinned slot can be advanced.
inline void clear_validated_minimizer_generation_row_receipt_batch(
        validated_minimizer_generation_row_receipt_batch& receipts) noexcept {
    for (auto& receipt : receipts) {
        receipt.reset();
    }
}

/// Authenticate one already-projected pinned batch. Only immutable payload
/// spans and storage-neutral CRC code enter TBB; no PostgreSQL API or callback
/// is invoked off the backend thread. The call joins before returning, so all
/// borrowed spans remain inside the lifetime of their pinned tuple slots.
inline void authenticate_minimizer_generation_projected_row_batch(
        const pinned_minimizer_generation_projected_row_batch& batch,
        validated_minimizer_generation_row_receipt_batch& receipts) {
    clear_validated_minimizer_generation_row_receipt_batch(receipts);
    const auto authenticate_range =
        [&batch, &receipts](
                std::size_t begin,
                std::size_t end) noexcept {
            for (std::size_t ordinal = begin;
                 ordinal < end; ++ordinal) {
                const minimizer_generation_projected_row& projected =
                    batch.rows[ordinal].projected;
                if (projected.error != nullptr) continue;
                auto validated =
                    bioseq::indexing::
                        validate_minimizer_generation_row_receipt(
                            minimizer_generation_row_view{
                                .address = projected.address,
                                .bytes = projected.bytes,
                                .row_checksum = projected.checksum,
                            });
                if (validated) {
                    receipts[ordinal].emplace(
                        std::move(*validated));
                }
            }
        };

    auto& pool = bioseq::parallelism::global_pool::instance();
    const bool use_parallel_authentication =
        batch.size >= k_minimizer_generation_parallel_row_authentication_min_rows
        && pool.is_active()
        && bioseq::parallelism::has_parallel_tasks(pool.get_mode())
        && pool.maximum_concurrency() > 1u;
    if (use_parallel_authentication) {
        pool.parallel_for(
            0u, batch.size,
            k_minimizer_generation_parallel_row_authentication_grain_rows,
            bioseq::parallelism::range_body{
                authenticate_range});
    } else {
        authenticate_range(0u, batch.size);
    }
}

/// Transfer one pre-authenticated row authority into the callback-scoped
/// loaded-row form without repeating its address-bound CRC pass.
[[nodiscard]] inline minimizer_generation_loaded_row
load_authenticated_minimizer_generation_projected_row_or_error(
        const minimizer_generation_projected_row& projected,
        std::optional<validated_minimizer_generation_row_receipt>&
            validated_receipt) {
    if (projected.error != nullptr) {
        minimizer_generation_storage_error_as_cpp(projected.error);
    }
    if (!validated_receipt) {
        minimizer_generation_storage_error_as_cpp("row checksum disagrees");
    }
    bool exact_authority = false;
    const bool visited =
        bioseq::indexing::visit_validated_minimizer_generation_row(
            *validated_receipt,
            [&projected, &exact_authority](
                    const index_chunk_address& address,
                    std::span<const std::byte> bytes,
                    std::uint64_t row_checksum) noexcept {
                exact_authority =
                    address == projected.address
                    && bytes.data() == projected.bytes.data()
                    && bytes.size() == projected.bytes.size()
                    && row_checksum == projected.checksum;
            });
    if (!visited || !exact_authority) {
        minimizer_generation_storage_error_as_cpp(
            "row checksum authority disagrees");
    }
    return minimizer_generation_loaded_row{
        .query_key = projected.query_key,
        .address = projected.address,
        .chunk_bytes = projected.chunk_bytes,
        .checksum = projected.checksum,
        .bytes = projected.bytes,
        .validated_receipt = std::move(*validated_receipt),
    };
}

/// Decode all eight non-NULL fields and validate PLAIN storage, byte count and
/// the address-bound row checksum before any caller observes the payload.
[[nodiscard]] inline minimizer_generation_loaded_row load_minimizer_generation_deformed_row_or_error(
        const minimizer_generation_deformed_row& deformed) {
    return load_minimizer_generation_projected_row_or_error(
        project_minimizer_generation_deformed_row(deformed));
}

/// General single-row loader.  Its hot-path PostgreSQL work is one deformation
/// bridge regardless of the eight-column schema width.
[[nodiscard]] inline minimizer_generation_loaded_row load_minimizer_generation_row_or_error(
        Relation relation,
        HeapTuple tuple) {
    const minimizer_generation_deformed_row deformed =
        deform_minimizer_generation_row_or_error(relation, tuple);
    return load_minimizer_generation_deformed_row_or_error(deformed);
}


}  // namespace bioseq::storage::indexing
