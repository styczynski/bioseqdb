/**
 * @file chunked_storage.cppm
 * @brief Concrete Postgres storage implementations that satisfy
 *        the `is_index_chunked_storage_for<...>` concepts.
 *
 * Index chunks are raw BYTEA tuples in heap relations (heap_form_tuple /
 * CatalogTupleInsert + systable_beginscan), bypassing columnarisation
 * (Invariant C). The per-sequence layouts co-locate with the sequence's
 * `(bucket, shard)`; the table layout shards via `index_table_shard`.
 */
module;
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
export module bioseq.postgres.storage.indexing.chunked_storage;
import bioseq.common.logging;
import bioseq.postgres.support;                       // bioseq::postgres::support::try_open_postgres_relation, bioseq::postgres::support::find_primary_key_oid
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
    bioseq::storage::manager::sql_row_codecs::
        minimizer_generation_storage_codec;
using metadata_age_log_catalog_row =
    bioseq::storage::indexing::catalog_rows::metadata_age_log;
using metadata_age_log_codec =
    bioseq::storage::indexing_pg::sql_row_codecs::
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

// Backend-local per-sequence build observability. Defined here
// (storage) so merge/release can set it; admin zeroes it at build start and
// mod.cppm reads it through SQL in the same backend.
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

struct minimizer_generation_administration_identity;
class minimizer_generation_catalog_source_authorization;
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

inline void reset_last_minimizer_sequence_build_stats() noexcept {
    g_last_index_merge_run_count = 0;
    g_last_minimizer_sequence_build_stats = {};
}

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

inline void arm_minimizer_generation_failure_injection(
        minimizer_generation_failure_injection point) noexcept {
    g_minimizer_generation_failure_injection = point;
}

inline void disarm_minimizer_generation_failure_injection() noexcept {
    g_minimizer_generation_failure_injection = minimizer_generation_failure_injection::none;
}

// PostgreSQL leaf form: call only from inside an existing invoke_pg_as_cpp
// callback.  Composite PG operations such as one complete SPI session use it
// so an injected ERROR does not introduce a nested context-restoring boundary.
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

inline void arm_inline_sequence_pg_leaf_failure(
		const inline_sequence_pg_leaf_failure point) noexcept {
	g_inline_sequence_pg_leaf_failure = point;
}

inline void disarm_inline_sequence_pg_leaf_failure() noexcept {
	g_inline_sequence_pg_leaf_failure = inline_sequence_pg_leaf_failure::none;
}

// This raw ereport helper is called only from inside one invoke_pg_as_cpp
// operation leaf. Consume first so the next statement in the same backend is
// a real recovery probe.
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

// Pinned attribute numbers for the fragment-chunk table.
inline constexpr AttrNumber k_attribute_query_key   = 1;
inline constexpr AttrNumber k_attribute_sub_key     = 2;
inline constexpr AttrNumber k_attribute_chunk_number    = 3;
inline constexpr AttrNumber k_attribute_chunk_bytes = 4;
inline constexpr AttrNumber k_attribute_blob       = 5;

// Current-generation storage uses the exact eight-column relation surface.
// The direct fragment-row constants above define the separate row-chunk API.
inline constexpr AttrNumber k_minimizer_generation_attribute_query_key       = 1;
inline constexpr AttrNumber k_minimizer_generation_attribute_generation_id   = 2;
inline constexpr AttrNumber k_minimizer_generation_attribute_position_segment = 3;
inline constexpr AttrNumber k_minimizer_generation_attribute_sub_key         = 4;
inline constexpr AttrNumber k_minimizer_generation_attribute_chunk_number        = 5;
inline constexpr AttrNumber k_minimizer_generation_attribute_chunk_bytes     = 6;
inline constexpr AttrNumber k_minimizer_generation_attribute_checksum       = 7;
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

struct minimizer_generation_index_attribute_descriptor {
    AttrNumber attno = InvalidAttrNumber;
    std::string_view name;
    Oid type_oid = InvalidOid;
};

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

struct minimizer_generation_schema_inspection {
    minimizer_generation_schema_error error = minimizer_generation_schema_error::none;
    Oid primary_index_oid = InvalidOid;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == minimizer_generation_schema_error::none
            && OidIsValid(primary_index_oid);
    }
};

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

enum class minimizer_generation_relation_kind : std::uint8_t {
    missing,
    current_exact,
    malformed,
};

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
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&](std::uint64_t v){ for (int i=0;i<8;++i){ h ^= (v & 0xff); h *= 1099511628211ull; v >>= 8; } };
    mix(db); mix(spc); mix(relfn); mix((std::uint64_t)(std::uint16_t)attno);
    for (char c : name) { h ^= (std::uint8_t)c; h *= 1099511628211ull; }
    return h;
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

[[nodiscard]] inline std::string format_seqidx_name(std::string_view name, Oid db, Oid spc, Oid relfn,
        std::int16_t attno, std::uint8_t bucket, std::uint32_t shard) {
    std::string out = fmt::format("seqidx_{}_{:016x}_{}_{}", short_index_for(name),
                                  identity_hash64(db, spc, relfn, attno, name),
                                  (unsigned)bucket, (unsigned)shard);
    if (out.size() >= NAMEDATALEN) derived_index_name_too_long(out);
    return out;
}
[[nodiscard]] inline std::string format_seqidxtab_name(std::string_view name, Oid db, Oid spc, Oid relfn,
        std::int16_t attno, std::int32_t shard) {
    std::string out = fmt::format("seqidxtab_{}_{:016x}_{}", short_index_for(name),
                                  identity_hash64(db, spc, relfn, attno, name), shard);
    if (out.size() >= NAMEDATALEN) derived_index_name_too_long(out);
    return out;
}

[[nodiscard]] inline Oid bioseq_internal_ns_pg_leaf() {
    return get_namespace_oid("bioseq_internal", false);
}

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

[[noreturn]] inline void minimizer_generation_storage_error_pg_leaf(
        std::string_view message) {
    ereport(ERROR,
        (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
         errmsg("bioseq current-generation index: %.*s",
            static_cast<int>(message.size()), message.data())));
    pg_unreachable();
}

[[noreturn]] inline void minimizer_generation_storage_error_as_cpp(
        std::string_view message) {
    bioseq::postgres::support::invoke_pg_as_cpp(
        [message] { minimizer_generation_storage_error_pg_leaf(message); });
    pg_unreachable();
}

[[noreturn]] inline void minimizer_generation_storage_error(std::string_view message) {
    minimizer_generation_storage_error_as_cpp(message);
    pg_unreachable();
}

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
        std::array<Oid, metadata_age_log_codec::fields_count>
            argument_types{
                OIDOID,
                OIDOID,
                INT2OID,
                TEXTOID,
                TIMESTAMPTZOID,
            };
        std::array<Datum, metadata_age_log_codec::fields_count>
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

[[nodiscard]] inline std::expected<
    validated_minimizer_generation_row, minimizer_generation_encoding_error>
validate_minimizer_generation_row_for_write(
        std::uint64_t expected_generation_id,
        const minimizer_generation_row_view& row) noexcept {
    return minimizer_generation_row_validator::validate(
        expected_generation_id, row);
}

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
        state->work_context = bioseq::postgres::support::invoke_pg_as_cpp([&] {
            return AllocSetContextCreate(
                CurrentMemoryContext,
                "bioseq current generation authoritative source authorization",
                ALLOCSET_DEFAULT_SIZES);
        });
        state->fragment_context = bioseq::postgres::support::invoke_pg_as_cpp([&] {
            return AllocSetContextCreate(
                state->work_context, "bioseq current generation source authorization fragment",
                ALLOCSET_DEFAULT_SIZES);
        });
        state->source_relation = bioseq::postgres::support::invoke_pg_as_cpp([&] {
            return bioseq::postgres::support::try_open_postgres_relation(
                identity.user_relid, AccessShareLock);
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
            [&] { return MemoryContextSwitchTo(state->work_context); });
        const auto switch_context = [](MemoryContext context) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [context] { MemoryContextSwitchTo(context); });
        };
        List* projected = bioseq::postgres::support::invoke_pg_as_cpp(
            [&] { return list_make1_int(identity.column_attno); });
        Datum* values = bioseq::postgres::support::invoke_pg_as_cpp([&] {
            return static_cast<Datum*>(palloc0(
                sizeof(Datum)
                * static_cast<std::size_t>(source_desc->natts)));
        });
        bool* nulls = bioseq::postgres::support::invoke_pg_as_cpp([&] {
            return static_cast<bool*>(palloc0(
                sizeof(bool)
                * static_cast<std::size_t>(source_desc->natts)));
        });
        state->source_read = bioseq::postgres::support::invoke_pg_as_cpp([&] {
            return begin_column_read(
                state->source_relation, source_desc, projected, NIL,
                state->work_context, registered_snapshot,
                /*randomAccess=*/true, nullptr);
        });
        const bool row_visible = bioseq::postgres::support::invoke_pg_as_cpp([&] {
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

        const auto source_catalog = bioseq::postgres::support::invoke_pg_as_cpp([&] {
            return sequence_source_version_manager::lookup(
                locator.source_epoch, registered_snapshot);
        });
        const std::uint64_t main_storage_id =
            bioseq::postgres::support::invoke_pg_as_cpp([&] {
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
                [&] { return alloc_seq_caches(state->work_context); });
            state->fragment_caches->side = seq_caches::access_side::read;
            state->fragment_caches->lock_mode = AccessShareLock;
            state->fragment_caches->read_state_mctx = state->work_context;
            state->fragment_caches->main_database_oid = identity.database_oid;
            state->fragment_caches->main_tablespace_oid = identity.tablespace_oid;
            state->fragment_caches->main_relfilenode = identity.main_relfilenode;
            Relation fragment_relation = bioseq::postgres::support::invoke_pg_as_cpp(
                [&] {
                    return open_sub_table_for_locator(
                        identity.database_oid, identity.tablespace_oid,
                        identity.main_relfilenode, identity.column_attno,
                        locator, AccessShareLock, state->fragment_caches);
                });
            if (RelationGetRelid(fragment_relation)
                    != source_catalog->fragment_relid
                || bioseq::postgres::support::invoke_pg_as_cpp([&] {
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
            state->fragment_read = bioseq::postgres::support::invoke_pg_as_cpp([&] {
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
                    [&] { MemoryContextReset(state->fragment_context); });
                Datum fragment_values[1]{0};
                bool fragment_nulls[1]{true};
                const std::uint64_t fragment_rowno =
                    locator.first_fragment_row + fragment_no;
                if (fragment_rowno < locator.first_fragment_row
                    || !bioseq::postgres::support::invoke_pg_as_cpp([&] {
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
                [&] { end_column_read(state->fragment_read); });
            state->fragment_read = nullptr;
        }
        if (state->fragment_caches != nullptr) {
            bioseq::postgres::support::invoke_pg_as_cpp([&] {
                close_and_destroy_seq_caches(
                    state->fragment_caches, /*commit=*/false);
            });
            state->fragment_caches = nullptr;
        }
        bioseq::postgres::support::invoke_pg_as_cpp(
            [&] { end_column_read(state->source_read); });
        state->source_read = nullptr;
        switch_context(previous_context);
        Relation finished_source = std::exchange(
            state->source_relation, nullptr);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [&] { relation_close(finished_source, AccessShareLock); });
        MemoryContext finished_context = std::exchange(
            state->work_context, nullptr);
        state->fragment_context = nullptr;
        bioseq::postgres::support::invoke_pg_as_cpp(
            [&] { MemoryContextDelete(finished_context); });
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
        state->work_context = bioseq::postgres::support::invoke_pg_as_cpp([&] {
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
            [&] { return MemoryContextSwitchTo(state->work_context); });
        const auto switch_context = [](MemoryContext context) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [context] { MemoryContextSwitchTo(context); });
        };
        List* projected = bioseq::postgres::support::invoke_pg_as_cpp(
            [&] { return list_make1_int(identity.column_attno); });
        Datum* values = bioseq::postgres::support::invoke_pg_as_cpp([&] {
            return static_cast<Datum*>(palloc0(
                sizeof(Datum)
                * static_cast<std::size_t>(source_desc->natts)));
        });
        bool* nulls = bioseq::postgres::support::invoke_pg_as_cpp([&] {
            return static_cast<bool*>(palloc0(
                sizeof(bool)
                * static_cast<std::size_t>(source_desc->natts)));
        });
        state->source_read = bioseq::postgres::support::invoke_pg_as_cpp([&] {
            return begin_column_read(
                state->source_relation, source_desc, projected, NIL,
                state->work_context, registered_snapshot,
                /*randomAccess=*/true, nullptr);
        });
        const bool row_visible = bioseq::postgres::support::invoke_pg_as_cpp([&] {
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
        const auto catalog = bioseq::postgres::support::invoke_pg_as_cpp([&] {
            return sequence_source_version_manager::lookup(
                locator.source_epoch, registered_snapshot,
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
            bioseq::postgres::support::invoke_pg_as_cpp([&] {
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
            state->fragment_relation = bioseq::postgres::support::invoke_pg_as_cpp([&] {
                return bioseq::postgres::support::try_open_postgres_relation(
                    catalog->fragment_relid, AccessShareLock);
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
                || bioseq::postgres::support::invoke_pg_as_cpp([&] {
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
                [&] { relation_close(finished, AccessShareLock); });
            authorization.cold_stats_.add(authorization.cold_stats_.lock_releases, 1);
        }
        bioseq::postgres::support::invoke_pg_as_cpp(
            [&] { end_column_read(state->source_read); });
        state->source_read = nullptr;
        switch_context(previous_context);
        // The executor owns this Relation and its AccessShareLock. The authorization
        // closes only its column read state and never closes the borrowed
        // descriptor.
        state->source_relation = nullptr;
        MemoryContext finished_context = std::exchange(
            state->work_context, nullptr);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [&] { MemoryContextDelete(finished_context); });
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
        relation = bioseq::postgres::support::invoke_pg_as_cpp([&] {
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

inline constexpr std::size_t k_storage_descriptor_attribute_count =
    minimizer_generation_storage_codec::fields_count;

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
    bioseq::postgres::support::invoke_pg_as_cpp([&] {
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
        scan = bioseq::postgres::support::invoke_pg_as_cpp([&] {
            return systable_beginscan_ordered(
                storage_relation, live_index_relation,
                registered_snapshot,
                static_cast<int>(keys.size()), keys.data());
        });
        HeapTuple tuple = bioseq::postgres::support::invoke_pg_as_cpp([&] {
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

        HeapTuple duplicate = bioseq::postgres::support::invoke_pg_as_cpp([&] {
            return systable_getnext_ordered(scan, ForwardScanDirection);
        });
        if (HeapTupleIsValid(duplicate)) {
            minimizer_generation_storage_error_as_cpp(
                "LIVE generation descriptor lookup returned duplicates");
        }
        bioseq::postgres::support::invoke_pg_as_cpp(
            [&] { systable_endscan_ordered(scan); });
        scan = nullptr;
        return descriptor;
    } catch (const bioseq::postgres::support::postgres_error&) {
        scan = nullptr;
        throw;
    } catch (...) {
        if (scan != nullptr) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [&] { systable_endscan_ordered(scan); });
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
    bytea* blob = bioseq::postgres::support::invoke_pg_as_cpp([&] {
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
        [&] {
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

inline constexpr std::size_t k_minimizer_generation_parallel_row_authentication_min_rows = 32u;
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

// -------------------------------------------------------------------------
// Bounded transaction-owned generation file actions.
//
// The file-action queue is separate from publication so transaction callbacks
// never inherit writer object lifetime.
// -------------------------------------------------------------------------

inline constexpr std::size_t k_pending_file_action_capacity = 4096u;
inline constexpr std::size_t k_file_action_diagnostic_capacity = 256u;
inline constexpr std::size_t k_maximum_reaper_file_actions = 64u;
inline constexpr std::uint16_t k_no_paired_file_action = UINT16_MAX;
inline constexpr std::uint8_t k_mapped_minimizer_fragment_index_identity = 1u;
inline constexpr std::string_view k_minimizer_generation_file_directory_name =
    "bioseq_minimizer_fragment_images";

enum class file_action : std::uint8_t {
    unlink_on_abort,
    unlink_on_commit,
};

/// Callback-safe file authority.  The queue stores no owning C++ object and
/// no pointer into a PostgreSQL memory context.
struct pending_file_action {
    file_action action{};
    SubTransactionId owner_subxid{};
    dev_t expected_device{};
    ino_t expected_inode{};
    std::uint16_t paired_slot = k_no_paired_file_action;
    bool inode_valid = false;
    bool armed = false;
    char path[MAXPGPATH]{};
};

static_assert(std::is_trivially_copyable_v<pending_file_action>);
static_assert(std::is_standard_layout_v<pending_file_action>);

enum class file_action_diagnostic_code : std::uint8_t {
    none,
    already_missing,
    unlinked,
    missing_inode_authority,
    stat_failed,
    not_regular,
    identity_mismatch,
    link_count_mismatch,
    paired_slot_invalid,
    paired_identity_mismatch,
    unlink_failed,
    prepare_rejected,
};

struct file_action_diagnostic {
    std::uint64_t sequence = 0u;
    std::uint16_t slot = k_no_paired_file_action;
    file_action_diagnostic_code code = file_action_diagnostic_code::none;
    int saved_errno = 0;
};

struct file_action_diagnostics_snapshot {
    std::array<file_action_diagnostic, k_file_action_diagnostic_capacity> entries{};
    std::uint64_t total = 0u;
};

/// Opaque all-or-nothing reservation.  Slots are found by reservation id
/// rather than assumed contiguous because savepoint completion can leave
/// holes in the fixed queue.
struct file_action_reservation {
    std::uint32_t id = 0u;
    std::uint16_t count = 0u;
};

struct minimizer_generation_file_identity {
    Oid database_oid = InvalidOid;
    Oid tablespace_oid = InvalidOid;
    Oid main_relfilenode = InvalidOid;
    Oid user_relid = InvalidOid;
    std::int16_t column_attno = 0;
    std::uint8_t index_identity =
        k_mapped_minimizer_fragment_index_identity;
    std::uint64_t query_key = 0u;
    std::uint64_t generation_id = 0u;
};

/// Fixed owned buffers let callbacks consume the exact backend-resolved path
/// without catalog access, formatting, allocation or a memory-context pointer.
struct resolved_minimizer_generation_file_path {
    std::array<char, MAXPGPATH> database_directory{};
    std::array<char, MAXPGPATH> bioseq_directory{};
    std::array<char, MAXPGPATH> shard_directory{};
    std::array<char, MAXPGPATH> final_path{};
};

struct pending_minimizer_generation_file_publication {
    file_action_reservation reservation{};
    std::uint16_t temporary_slot = k_no_paired_file_action;
    std::uint16_t final_slot = k_no_paired_file_action;
    int fd = -1;
    dev_t expected_device{};
    ino_t expected_inode{};
    std::array<char, MAXPGPATH> shard_directory{};
    std::array<char, MAXPGPATH> temporary_path{};
    std::array<char, MAXPGPATH> final_path{};
    bool published = false;

    pending_minimizer_generation_file_publication() = default;
    pending_minimizer_generation_file_publication(
            const pending_minimizer_generation_file_publication&) = delete;
    pending_minimizer_generation_file_publication& operator=(
            const pending_minimizer_generation_file_publication&) = delete;
    pending_minimizer_generation_file_publication(
            pending_minimizer_generation_file_publication&& other) noexcept
        : reservation(std::exchange(
              other.reservation, file_action_reservation{})),
          temporary_slot(std::exchange(
              other.temporary_slot, k_no_paired_file_action)),
          final_slot(std::exchange(
              other.final_slot, k_no_paired_file_action)),
          fd(std::exchange(other.fd, -1)),
          expected_device(other.expected_device),
          expected_inode(other.expected_inode),
          shard_directory(other.shard_directory),
          temporary_path(other.temporary_path),
          final_path(other.final_path),
          published(std::exchange(other.published, true)) {
        other.temporary_path.fill('\0');
        other.final_path.fill('\0');
    }
    pending_minimizer_generation_file_publication& operator=(
            pending_minimizer_generation_file_publication&&) = delete;
    ~pending_minimizer_generation_file_publication();
};

namespace generation_file_action_detail {

enum class slot_state : std::uint8_t {
    free,
    reserved,
    armed,
};

struct queue_state {
    std::array<pending_file_action, k_pending_file_action_capacity> actions{};
    std::array<slot_state, k_pending_file_action_capacity> slot_states{};
    std::array<std::uint32_t, k_pending_file_action_capacity> reservation_ids{};
    std::array<file_action_diagnostic,
        k_file_action_diagnostic_capacity> diagnostics{};
    std::uint64_t diagnostic_total = 0u;
    std::uint32_t next_reservation_id = 1u;
    bool xact_callback_installed = false;
    bool subxact_callback_installed = false;
};

// The extension image is private to one PostgreSQL backend process.
inline queue_state g_queue_state{};

static_assert(std::tuple_size_v<decltype(queue_state::actions)>
    == k_pending_file_action_capacity);
static_assert(std::tuple_size_v<decltype(queue_state::diagnostics)>
    == k_file_action_diagnostic_capacity);

[[nodiscard]] inline bool slot_is_occupied(
        const std::size_t slot) noexcept {
    return g_queue_state.slot_states[slot] != slot_state::free;
}

inline void record_diagnostic(
        const std::uint16_t slot,
        const file_action_diagnostic_code code,
        const int saved_errno = 0) noexcept {
    const std::uint64_t sequence = g_queue_state.diagnostic_total++;
    file_action_diagnostic& diagnostic =
        g_queue_state.diagnostics[
            sequence % k_file_action_diagnostic_capacity];
    diagnostic = file_action_diagnostic{
        .sequence = sequence + 1u,
        .slot = slot,
        .code = code,
        .saved_errno = saved_errno,
    };
}

inline void release_slot(const std::size_t slot) noexcept {
    g_queue_state.actions[slot] = pending_file_action{};
    g_queue_state.slot_states[slot] = slot_state::free;
    g_queue_state.reservation_ids[slot] = 0u;
}

[[nodiscard]] inline bool reservation_id_is_live(
        const std::uint32_t reservation_id) noexcept {
    for (std::size_t slot = 0u; slot < k_pending_file_action_capacity;
         ++slot) {
        if (slot_is_occupied(slot)
            && g_queue_state.reservation_ids[slot] == reservation_id) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline std::uint32_t next_unused_reservation_id() {
    for (std::uint64_t attempts = 0u;
         attempts < static_cast<std::uint64_t>(UINT32_MAX);
         ++attempts) {
        std::uint32_t candidate = g_queue_state.next_reservation_id++;
        if (candidate == 0u) {
            candidate = g_queue_state.next_reservation_id++;
        }
        if (candidate != 0u && !reservation_id_is_live(candidate)) {
            return candidate;
        }
    }
    minimizer_generation_storage_error_as_cpp(
        "generation file-action reservation id space is exhausted");
}

[[nodiscard]] inline std::optional<std::uint16_t> reservation_slot(
        const file_action_reservation reservation,
        const std::size_t ordinal) noexcept {
    if (reservation.id == 0u || ordinal >= reservation.count) {
        return std::nullopt;
    }
    std::size_t seen = 0u;
    for (std::size_t slot = 0u; slot < k_pending_file_action_capacity;
         ++slot) {
        if (slot_is_occupied(slot)
            && g_queue_state.reservation_ids[slot] == reservation.id) {
            if (seen == ordinal) {
                return static_cast<std::uint16_t>(slot);
            }
            ++seen;
        }
    }
    return std::nullopt;
}

[[nodiscard]] inline bool same_inode(
        const struct stat& observed,
        const pending_file_action& action) noexcept {
    return action.inode_valid
        && observed.st_dev == action.expected_device
        && observed.st_ino == action.expected_inode;
}

#ifdef BIOSEQ_TEST_INDEXES
enum class callback_failure_injection : std::uint8_t {
    none,
    stat_failed,
    identity_mismatch,
    unlink_failed,
};
inline callback_failure_injection g_callback_failure_injection =
    callback_failure_injection::none;
#endif

/// The sole callback filesystem mutation.  It cannot allocate, log or raise;
/// failures enter the fixed ring for later backend-thread reporting.
inline void unlink_identity_bound_noexcept(
        const std::uint16_t slot) noexcept {
    pending_file_action& action = g_queue_state.actions[slot];
    if (!action.armed || !action.inode_valid || action.path[0] == '\0') {
        record_diagnostic(
            slot, file_action_diagnostic_code::missing_inode_authority);
        return;
    }
#ifdef BIOSEQ_TEST_INDEXES
    if (g_callback_failure_injection
        == callback_failure_injection::stat_failed) {
        g_callback_failure_injection = callback_failure_injection::none;
        record_diagnostic(
            slot, file_action_diagnostic_code::stat_failed, EIO);
        return;
    }
#endif
    struct stat observed{};
    if (fstatat(
            AT_FDCWD, action.path, &observed,
            AT_SYMLINK_NOFOLLOW) != 0) {
        const int saved_errno = errno;
        record_diagnostic(
            slot,
            saved_errno == ENOENT
                ? file_action_diagnostic_code::already_missing
                : file_action_diagnostic_code::stat_failed,
            saved_errno == ENOENT ? 0 : saved_errno);
        return;
    }
    if (!S_ISREG(observed.st_mode)) {
        record_diagnostic(slot, file_action_diagnostic_code::not_regular);
        return;
    }
#ifdef BIOSEQ_TEST_INDEXES
    if (g_callback_failure_injection
        == callback_failure_injection::identity_mismatch) {
        g_callback_failure_injection = callback_failure_injection::none;
        record_diagnostic(
            slot, file_action_diagnostic_code::identity_mismatch);
        return;
    }
#endif
    if (!same_inode(observed, action)) {
        record_diagnostic(
            slot, file_action_diagnostic_code::identity_mismatch);
        return;
    }
    if (observed.st_nlink == 2) {
        if (action.paired_slot >= k_pending_file_action_capacity
            || action.paired_slot == slot) {
            record_diagnostic(
                slot, file_action_diagnostic_code::paired_slot_invalid);
            return;
        }
        const pending_file_action& paired =
            g_queue_state.actions[action.paired_slot];
        if (g_queue_state.slot_states[action.paired_slot]
                != slot_state::armed
            || !paired.armed || !paired.inode_valid
            || paired.path[0] == '\0') {
            record_diagnostic(
                slot, file_action_diagnostic_code::paired_slot_invalid);
            return;
        }
        struct stat paired_observed{};
        if (fstatat(
                AT_FDCWD, paired.path, &paired_observed,
                AT_SYMLINK_NOFOLLOW) != 0
            || !S_ISREG(paired_observed.st_mode)
            || paired_observed.st_dev != observed.st_dev
            || paired_observed.st_ino != observed.st_ino
            || !same_inode(paired_observed, paired)) {
            record_diagnostic(
                slot,
                file_action_diagnostic_code::paired_identity_mismatch,
                errno);
            return;
        }
    } else if (observed.st_nlink != 1) {
        record_diagnostic(
            slot, file_action_diagnostic_code::link_count_mismatch);
        return;
    }
#ifdef BIOSEQ_TEST_INDEXES
    if (g_callback_failure_injection
        == callback_failure_injection::unlink_failed) {
        g_callback_failure_injection = callback_failure_injection::none;
        record_diagnostic(
            slot, file_action_diagnostic_code::unlink_failed, EIO);
        return;
    }
#endif
    if (unlink(action.path) != 0) {
        const int saved_errno = errno;
        record_diagnostic(
            slot,
            saved_errno == ENOENT
                ? file_action_diagnostic_code::already_missing
                : file_action_diagnostic_code::unlink_failed,
            saved_errno == ENOENT ? 0 : saved_errno);
        return;
    }
    record_diagnostic(slot, file_action_diagnostic_code::unlinked);
}

inline void finish_top_transaction(const bool committed) noexcept {
    for (std::size_t slot = 0u; slot < k_pending_file_action_capacity;
         ++slot) {
        if (!slot_is_occupied(slot)) continue;
        const pending_file_action& action = g_queue_state.actions[slot];
        if (g_queue_state.slot_states[slot] == slot_state::armed
            && action.armed
            && ((committed
                    && action.action == file_action::unlink_on_commit)
                || (!committed
                    && action.action == file_action::unlink_on_abort))) {
            unlink_identity_bound_noexcept(
                static_cast<std::uint16_t>(slot));
        }
        release_slot(slot);
    }
}

inline void finish_subtransaction(
        const SubTransactionId subxid,
        const bool committed,
        const SubTransactionId parent_subxid) noexcept {
    for (std::size_t slot = 0u; slot < k_pending_file_action_capacity;
         ++slot) {
        if (!slot_is_occupied(slot)
            || g_queue_state.actions[slot].owner_subxid != subxid) {
            continue;
        }
        pending_file_action& action = g_queue_state.actions[slot];
        if (committed) {
            action.owner_subxid = parent_subxid;
            continue;
        }
        if (g_queue_state.slot_states[slot] == slot_state::armed
            && action.armed
            && action.action == file_action::unlink_on_abort) {
            unlink_identity_bound_noexcept(
                static_cast<std::uint16_t>(slot));
        }
        release_slot(slot);
    }
}

[[nodiscard]] inline bool has_occupied_slot() noexcept {
    for (std::size_t slot = 0u; slot < k_pending_file_action_capacity;
         ++slot) {
        if (slot_is_occupied(slot)) return true;
    }
    return false;
}

inline void xact_callback(const XactEvent event, void*) {
    switch (event) {
        case XACT_EVENT_COMMIT:
        case XACT_EVENT_PARALLEL_COMMIT:
            finish_top_transaction(true);
            break;
        case XACT_EVENT_ABORT:
        case XACT_EVENT_PARALLEL_ABORT:
            finish_top_transaction(false);
            break;
        case XACT_EVENT_PRE_PREPARE:
            if (has_occupied_slot()) {
                record_diagnostic(
                    k_no_paired_file_action,
                    file_action_diagnostic_code::prepare_rejected);
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("bioseq generation file actions do not support prepared transactions"),
                     errhint("commit or roll back without PREPARE TRANSACTION")));
            }
            break;
        case XACT_EVENT_PREPARE:
        case XACT_EVENT_PRE_COMMIT:
        case XACT_EVENT_PARALLEL_PRE_COMMIT:
            break;
    }
}

inline void subxact_callback(
        const SubXactEvent event,
        const SubTransactionId subxid,
        const SubTransactionId parent_subxid,
        void*) noexcept {
    switch (event) {
        case SUBXACT_EVENT_COMMIT_SUB:
            finish_subtransaction(subxid, true, parent_subxid);
            break;
        case SUBXACT_EVENT_ABORT_SUB:
            finish_subtransaction(subxid, false, parent_subxid);
            break;
        case SUBXACT_EVENT_START_SUB:
        case SUBXACT_EVENT_PRE_COMMIT_SUB:
            break;
    }
}

inline void install_callbacks() {
    if (!g_queue_state.xact_callback_installed) {
        bioseq::postgres::support::invoke_pg_as_cpp([] {
            RegisterXactCallback(xact_callback, nullptr);
        });
        g_queue_state.xact_callback_installed = true;
    }
    if (!g_queue_state.subxact_callback_installed) {
        bioseq::postgres::support::invoke_pg_as_cpp([] {
            RegisterSubXactCallback(subxact_callback, nullptr);
        });
        g_queue_state.subxact_callback_installed = true;
    }
}

template <std::size_t p_buffer_bytes, typename... p_argument_types>
[[nodiscard]] inline bool format_fixed(
        std::array<char, p_buffer_bytes>& destination,
        fmt::format_string<p_argument_types...> format,
        p_argument_types&&... args) {
    static_assert(p_buffer_bytes > 1u);
    destination.fill('\0');
    const auto formatted = fmt::format_to_n(
        destination.data(), p_buffer_bytes - 1u, format,
        std::forward<p_argument_types>(args)...);
    if (formatted.size >= p_buffer_bytes) {
        destination[0] = '\0';
        return false;
    }
    destination[formatted.size] = '\0';
    return true;
}

[[nodiscard]] inline bool path_has_only_canonical_components(
        const std::string_view path) noexcept {
    if (path.empty() || path.back() == '/') return false;
    std::size_t component_begin = path.front() == '/' ? 1u : 0u;
    if (component_begin == path.size()) return false;
    while (component_begin < path.size()) {
        const std::size_t separator = path.find('/', component_begin);
        const std::size_t component_end = separator == std::string_view::npos
            ? path.size() : separator;
        if (component_end == component_begin) return false;
        const std::string_view component =
            path.substr(component_begin, component_end - component_begin);
        if (component == "." || component == "..") return false;
        if (separator == std::string_view::npos) break;
        component_begin = separator + 1u;
    }
    return true;
}

[[nodiscard]] inline bool exact_child_path(
        const std::string_view parent,
        const std::string_view child) noexcept {
    if (child.size() <= parent.size() + 1u
        || !child.starts_with(parent)
        || child[parent.size()] != '/') {
        return false;
    }
    return child.find('/', parent.size() + 1u)
        == std::string_view::npos;
}

[[nodiscard]] inline bool ensure_private_directory(const char* path) {
    struct stat observed{};
    bool created = false;
    if (lstat(path, &observed) != 0) {
        if (errno != ENOENT) {
            minimizer_generation_storage_error_as_cpp(fmt::format(
                "generation file directory preflight failed: errno={}",
                errno));
        }
        if (mkdir(path, 0700) == 0) {
            created = true;
        } else if (errno != EEXIST) {
            minimizer_generation_storage_error_as_cpp(fmt::format(
                "generation file directory creation failed: errno={}",
                errno));
        }
        if (lstat(path, &observed) != 0) {
            minimizer_generation_storage_error_as_cpp(fmt::format(
                "generation file directory stat failed: errno={}",
                errno));
        }
    }
    if (!S_ISDIR(observed.st_mode)
        || observed.st_uid != geteuid()
        || (observed.st_mode & 07777) != 0700) {
        minimizer_generation_storage_error_as_cpp(
            "generation file directory is not server-owned mode-0700");
    }
    return created;
}

inline void arm_reserved_slot(
        const file_action_reservation reservation,
        const std::size_t ordinal,
        const file_action action,
        const char* path,
        const dev_t expected_device,
        const ino_t expected_inode,
        const std::uint16_t paired_slot) {
    const std::optional<std::uint16_t> maybe_slot =
        reservation_slot(reservation, ordinal);
    if (!maybe_slot) {
        minimizer_generation_storage_error_as_cpp(
            "generation file-action reservation slot is missing");
    }
    const std::uint16_t slot = *maybe_slot;
    if (g_queue_state.slot_states[slot] != slot_state::reserved
        || path == nullptr) {
        minimizer_generation_storage_error_as_cpp(
            "generation file-action reservation cannot be armed");
    }
    const std::size_t path_bytes = strnlen(path, MAXPGPATH);
    if (path_bytes == 0u || path_bytes >= MAXPGPATH) {
        minimizer_generation_storage_error_as_cpp(
            "generation file-action path is empty or truncated");
    }
    pending_file_action& pending = g_queue_state.actions[slot];
    pending = pending_file_action{
        .action = action,
        .owner_subxid = GetCurrentSubTransactionId(),
        .expected_device = expected_device,
        .expected_inode = expected_inode,
        .paired_slot = paired_slot,
        .inode_valid = true,
        .armed = true,
    };
    std::memcpy(pending.path, path, path_bytes + 1u);
    g_queue_state.slot_states[slot] = slot_state::armed;
}

inline void fsync_directory(const char* path) {
    const int fd = open(
        path, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0) {
        minimizer_generation_storage_error_as_cpp(fmt::format(
            "generation file directory open failed: errno={}", errno));
    }
    const int sync_result = pg_fsync(fd);
    const int sync_errno = errno;
    const int close_result = close(fd);
    if (sync_result != 0 || close_result != 0) {
        minimizer_generation_storage_error_as_cpp(fmt::format(
            "generation file directory fsync/close failed: errno={}",
            sync_result != 0 ? sync_errno : errno));
    }
}

} // namespace generation_file_action_detail

[[nodiscard]] inline file_action_reservation reserve_file_actions(
        const std::size_t requested_count) {
    using namespace generation_file_action_detail;
    if (requested_count == 0u
        || requested_count > k_pending_file_action_capacity) {
        minimizer_generation_storage_error_as_cpp(
            "generation file-action reservation size is invalid");
    }
    install_callbacks();
    std::size_t available = 0u;
    for (std::size_t slot = 0u; slot < k_pending_file_action_capacity;
         ++slot) {
        available += slot_is_occupied(slot) ? 0u : 1u;
    }
    if (available < requested_count) {
        minimizer_generation_storage_error_as_cpp(
            "generation file-action queue capacity exhausted before mutation");
    }

    // After the complete preflight this pass cannot fail: it marks exactly
    // the requested number without allocation or partial-return behavior.
    const std::uint32_t reservation_id = next_unused_reservation_id();
    std::size_t reserved = 0u;
    for (std::size_t slot = 0u;
         slot < k_pending_file_action_capacity && reserved < requested_count;
         ++slot) {
        if (slot_is_occupied(slot)) continue;
        g_queue_state.actions[slot] = pending_file_action{
            .owner_subxid = GetCurrentSubTransactionId(),
        };
        g_queue_state.slot_states[slot] = slot_state::reserved;
        g_queue_state.reservation_ids[slot] = reservation_id;
        ++reserved;
    }
    return file_action_reservation{
        .id = reservation_id,
        .count = static_cast<std::uint16_t>(requested_count),
    };
}

[[nodiscard]] inline file_action_reservation reserve_reaper_file_actions(
        const std::size_t requested_count) {
    if (requested_count == 0u
        || requested_count > k_maximum_reaper_file_actions) {
        minimizer_generation_storage_error_as_cpp(
            "generation reaper file-action batch exceeds hard cap 64");
    }
    return reserve_file_actions(requested_count);
}

inline void release_unarmed_file_action_reservation(
        const file_action_reservation reservation) noexcept {
    using namespace generation_file_action_detail;
    for (std::size_t slot = 0u; slot < k_pending_file_action_capacity;
         ++slot) {
        if (g_queue_state.slot_states[slot] == slot_state::reserved
            && g_queue_state.reservation_ids[slot] == reservation.id) {
            release_slot(slot);
        }
    }
}

inline void cancel_generation_file_publication_noexcept(
        pending_minimizer_generation_file_publication& publication) noexcept {
    using namespace generation_file_action_detail;
    if (publication.published) return;

    if (publication.reservation.id == 0u) {
        if (publication.fd >= 0) {
            (void)CloseTransientFile(publication.fd);
            publication.fd = -1;
        }
        if (publication.temporary_path[0] != '\0') {
            (void)unlink(publication.temporary_path.data());
        }
        publication.published = true;
        return;
    }

    bool has_armed_action = false;
    for (std::size_t slot = 0u; slot < k_pending_file_action_capacity;
         ++slot) {
        if (g_queue_state.reservation_ids[slot]
                == publication.reservation.id
            && g_queue_state.slot_states[slot] == slot_state::armed) {
            has_armed_action = true;
            break;
        }
    }
    if (publication.fd >= 0) {
        (void)CloseTransientFile(publication.fd);
        publication.fd = -1;
    }
    // Before fstat supplies inode authority the O_EXCL name is still owned by
    // this backend inside its private 0700 shard.  Remove that exact temp
    // directly; after arming, the queue's inode-bound path is the sole
    // deletion authority.
    if (!has_armed_action && publication.temporary_path[0] != '\0') {
        (void)unlink(publication.temporary_path.data());
    }
    for (std::size_t slot = 0u; slot < k_pending_file_action_capacity;
         ++slot) {
        if (g_queue_state.reservation_ids[slot]
            != publication.reservation.id) {
            continue;
        }
        if (g_queue_state.slot_states[slot] == slot_state::armed
            && g_queue_state.actions[slot].armed
            && g_queue_state.actions[slot].action
                == file_action::unlink_on_abort) {
            unlink_identity_bound_noexcept(
                static_cast<std::uint16_t>(slot));
        }
        release_slot(slot);
    }
    publication.reservation = {};
    publication.temporary_slot = k_no_paired_file_action;
    publication.final_slot = k_no_paired_file_action;
    publication.temporary_path.fill('\0');
    publication.final_path.fill('\0');
    publication.published = true;
}

inline pending_minimizer_generation_file_publication::~pending_minimizer_generation_file_publication() {
    cancel_generation_file_publication_noexcept(*this);
}

[[nodiscard]] inline resolved_minimizer_generation_file_path
resolve_generation_file_path(
        const minimizer_generation_file_identity& identity) {
    using namespace generation_file_action_detail;
    if (!OidIsValid(identity.database_oid)
        || !OidIsValid(identity.tablespace_oid)
        || !OidIsValid(identity.main_relfilenode)
        || !OidIsValid(identity.user_relid)
        || identity.column_attno <= 0
        || identity.index_identity
            != k_mapped_minimizer_fragment_index_identity
        || identity.generation_id == 0u) {
        minimizer_generation_storage_error_as_cpp(
            "generation file identity is invalid or is not a mapped minimizer-fragment index");
    }

    resolved_minimizer_generation_file_path resolved{};
    bioseq::postgres::support::invoke_pg_as_cpp(
        [&resolved, database_oid = identity.database_oid,
         tablespace_oid = identity.tablespace_oid] {
            char* const database_path =
                GetDatabasePath(database_oid, tablespace_oid);
            if (database_path == nullptr) {
                ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("could not resolve bioseq generation database path")));
            }
            const std::size_t path_bytes =
                strnlen(database_path, MAXPGPATH);
            if (path_bytes == 0u || path_bytes >= MAXPGPATH) {
                pfree(database_path);
                ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("bioseq generation database path is truncated")));
            }
            std::memcpy(
                resolved.database_directory.data(),
                database_path, path_bytes + 1u);
            pfree(database_path);
        });
    const std::string_view database_directory{
        resolved.database_directory.data()};
    if (!path_has_only_canonical_components(database_directory)) {
        minimizer_generation_storage_error_as_cpp(
            "generation database path is not canonical");
    }
    if (!format_fixed(
            resolved.bioseq_directory, "{}/{}",
            database_directory, k_minimizer_generation_file_directory_name)) {
        minimizer_generation_storage_error_as_cpp(
            "generation Bioseq directory path is truncated");
    }
    constexpr std::array<char, 16u> k_lower_hex{
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    const std::uint8_t shard =
        static_cast<std::uint8_t>(identity.generation_id & 0xffu);
    const std::array<char, 3u> shard_name{
        k_lower_hex[shard >> 4u], k_lower_hex[shard & 0x0fu], '\0'};
    if (!format_fixed(
            resolved.shard_directory, "{}/{}",
            resolved.bioseq_directory.data(), shard_name.data())) {
        minimizer_generation_storage_error_as_cpp(
            "generation shard directory path is truncated");
    }
    if (!format_fixed(
            resolved.final_path,
            "{}/{}.{}.{}.1.{}.{}.minimizer_fragment_index",
            resolved.shard_directory.data(),
            static_cast<unsigned>(identity.main_relfilenode),
            static_cast<unsigned>(identity.user_relid),
            static_cast<int>(identity.column_attno),
            static_cast<unsigned long long>(identity.query_key),
            static_cast<unsigned long long>(identity.generation_id))) {
        minimizer_generation_storage_error_as_cpp(
            "generation final path is truncated");
    }
    if (!path_has_only_canonical_components(
            resolved.final_path.data())
        || !exact_child_path(
            resolved.database_directory.data(),
            resolved.bioseq_directory.data())
        || !exact_child_path(
            resolved.bioseq_directory.data(),
            resolved.shard_directory.data())
        || !exact_child_path(
            resolved.shard_directory.data(),
            resolved.final_path.data())) {
        minimizer_generation_storage_error_as_cpp(
            "generation file path escapes its canonical database shard");
    }
    return resolved;
}

/// Reserve temp/final before the first filesystem mutation and then create a
/// cryptographically unique strict mode-0600 O_EXCL temp.
[[nodiscard]] inline pending_minimizer_generation_file_publication
begin_generation_file_publication(
        const minimizer_generation_file_identity& identity) {
    using namespace generation_file_action_detail;
    const resolved_minimizer_generation_file_path resolved =
        resolve_generation_file_path(identity);
    const file_action_reservation reservation = reserve_file_actions(2u);
    const auto temporary_slot = reservation_slot(reservation, 0u);
    const auto final_slot = reservation_slot(reservation, 1u);
    if (!temporary_slot || !final_slot) {
        minimizer_generation_storage_error_as_cpp(
            "generation publication pair reservation is incomplete");
    }

    pending_minimizer_generation_file_publication publication;
    publication.reservation = reservation;
    publication.temporary_slot = *temporary_slot;
    publication.final_slot = *final_slot;
    publication.shard_directory = resolved.shard_directory;
    publication.final_path = resolved.final_path;

    const bool bioseq_directory_created = ensure_private_directory(
        resolved.bioseq_directory.data());
    if (bioseq_directory_created) {
        fsync_directory(resolved.database_directory.data());
    }
    const bool shard_directory_created = ensure_private_directory(
        resolved.shard_directory.data());
    if (shard_directory_created) {
        fsync_directory(resolved.bioseq_directory.data());
    }
    constexpr std::size_t k_nonce_bytes = 16u;
    constexpr std::size_t k_maximum_collision_retries = 128u;
    constexpr std::array<char, 16u> k_lower_hex{
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    const std::uint64_t top_full_xid =
        bioseq::postgres::support::invoke_pg_as_cpp([] {
            return U64FromFullTransactionId(
                GetTopFullTransactionId());
        });
    for (std::size_t attempt = 0u;
         attempt < k_maximum_collision_retries; ++attempt) {
        std::array<unsigned char, k_nonce_bytes> nonce{};
        if (!pg_strong_random(nonce.data(), nonce.size())) {
            minimizer_generation_storage_error_as_cpp(
                "generation temp nonce source failed");
        }
        std::array<char, k_nonce_bytes * 2u + 1u> nonce_hex{};
        for (std::size_t byte_index = 0u; byte_index < nonce.size();
             ++byte_index) {
            nonce_hex[byte_index * 2u] =
                k_lower_hex[nonce[byte_index] >> 4u];
            nonce_hex[byte_index * 2u + 1u] =
                k_lower_hex[nonce[byte_index] & 0x0fu];
        }
        if (!format_fixed(
                publication.temporary_path,
                "{}.tmp.{}.{}.{}",
                publication.final_path.data(),
                static_cast<unsigned long long>(top_full_xid),
                static_cast<int>(MyProcPid), nonce_hex.data())) {
            minimizer_generation_storage_error_as_cpp(
                "generation temporary path is truncated");
        }
        publication.fd = bioseq::postgres::support::invoke_pg_as_cpp([&] {
            return OpenTransientFilePerm(
                publication.temporary_path.data(),
                O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW
                    | PG_BINARY,
                S_IRUSR | S_IWUSR);
        });
        if (publication.fd >= 0) break;
        if (errno != EEXIST) {
            minimizer_generation_storage_error_as_cpp(fmt::format(
                "generation O_EXCL temp creation failed: errno={}",
                errno));
        }
    }
    if (publication.fd < 0) {
        minimizer_generation_storage_error_as_cpp(
            "generation O_EXCL temp nonce collisions exceeded retry bound");
    }
    struct stat observed{};
    if (fstat(publication.fd, &observed) != 0
        || !S_ISREG(observed.st_mode)
        || observed.st_uid != geteuid()
        || (observed.st_mode & 07777) != 0600
        || observed.st_nlink != 1) {
        minimizer_generation_storage_error_as_cpp(
            "generation temp inode contract is invalid");
    }
    publication.expected_device = observed.st_dev;
    publication.expected_inode = observed.st_ino;
    arm_reserved_slot(
        reservation, 0u, file_action::unlink_on_abort,
        publication.temporary_path.data(), observed.st_dev,
        observed.st_ino, publication.final_slot);
    return publication;
}

/// Freeze, link without replacement, validate and directory-fsync a completed
/// header-last image.  The caller finishes payload/header writes and image
/// validation before entering this function.
inline void publish_generation_file(
        pending_minimizer_generation_file_publication& publication) {
    using namespace generation_file_action_detail;
    if (publication.fd < 0
        || publication.temporary_slot == k_no_paired_file_action
        || publication.final_slot == k_no_paired_file_action) {
        minimizer_generation_storage_error_as_cpp(
            "generation publication is not open");
    }
    if (fchmod(publication.fd, S_IRUSR) != 0
        || pg_fsync(publication.fd) != 0) {
        minimizer_generation_storage_error_as_cpp(fmt::format(
            "generation image freeze/fsync failed: errno={}", errno));
    }
    struct stat frozen{};
    if (fstat(publication.fd, &frozen) != 0
        || !S_ISREG(frozen.st_mode)
        || frozen.st_uid != geteuid()
        || (frozen.st_mode & 07777) != 0400
        || frozen.st_nlink != 1
        || frozen.st_dev != publication.expected_device
        || frozen.st_ino != publication.expected_inode) {
        minimizer_generation_storage_error_as_cpp(
            "generation frozen temp inode contract is invalid");
    }
    arm_reserved_slot(
        publication.reservation, 1u,
        file_action::unlink_on_abort,
        publication.final_path.data(),
        publication.expected_device, publication.expected_inode,
        publication.temporary_slot);
    if (linkat(
            AT_FDCWD, publication.temporary_path.data(),
            AT_FDCWD, publication.final_path.data(), 0) != 0) {
        // EEXIST is intentionally not special: no existing final is ever
        // overwritten, renamed aside or pre-unlinked.
        minimizer_generation_storage_error_as_cpp(fmt::format(
            "generation immutable final link failed without replacement: errno={}",
            errno));
    }
    if (unlink(publication.temporary_path.data()) != 0) {
        // Both abort actions stay armed and paired.  Callback cleanup accepts
        // precisely this two-link window and refuses every unrelated link.
        minimizer_generation_storage_error_as_cpp(fmt::format(
            "generation strict temp unlink failed: errno={}", errno));
    }
    release_slot(publication.temporary_slot);
    pending_file_action& final_action =
        g_queue_state.actions[publication.final_slot];
    final_action.paired_slot = k_no_paired_file_action;

    struct stat published{};
    if (fstatat(
            AT_FDCWD, publication.final_path.data(), &published,
            AT_SYMLINK_NOFOLLOW) != 0
        || !S_ISREG(published.st_mode)
        || published.st_uid != geteuid()
        || (published.st_mode & 07777) != 0400
        || published.st_nlink != 1
        || published.st_dev != publication.expected_device
        || published.st_ino != publication.expected_inode) {
        minimizer_generation_storage_error_as_cpp(
            "generation immutable final inode contract is invalid");
    }
    fsync_directory(publication.shard_directory.data());
    if (CloseTransientFile(publication.fd) != 0) {
        publication.fd = -1;
        minimizer_generation_storage_error_as_cpp(fmt::format(
            "generation immutable final close failed: errno={}", errno));
    }
    publication.fd = -1;
    publication.temporary_path.fill('\0');
    // The final abort action remains transaction-owned until commit/abort;
    // the C++ publication guard must no longer cancel it on ordinary scope
    // exit after the durable no-replace publication succeeds.
    publication.published = true;
}

/// Bind one pre-reserved commit action to an existing exact final inode.  A
/// reaper first reserves its whole <=64-file batch, then calls this only after
/// its descriptor/head horizon checks and before descriptor deletion.
inline void arm_generation_file_unlink_on_commit(
        const file_action_reservation reservation,
        const std::size_t ordinal,
        const minimizer_generation_file_identity& identity) {
    using namespace generation_file_action_detail;
    // Never accept caller-assembled path authority.  Recompute the canonical
    // database/shard/final path from the persisted physical identity.
    const resolved_minimizer_generation_file_path resolved =
        resolve_generation_file_path(identity);
    if (!path_has_only_canonical_components(resolved.final_path.data())
        || !exact_child_path(
            resolved.database_directory.data(),
            resolved.bioseq_directory.data())
        || !exact_child_path(
            resolved.bioseq_directory.data(),
            resolved.shard_directory.data())
        || !exact_child_path(
            resolved.shard_directory.data(),
            resolved.final_path.data())) {
        minimizer_generation_storage_error_as_cpp(
            "generation commit-unlink path is not trusted");
    }
    struct stat observed{};
    if (fstatat(
            AT_FDCWD, resolved.final_path.data(), &observed,
            AT_SYMLINK_NOFOLLOW) != 0
        || !S_ISREG(observed.st_mode)
        || observed.st_uid != geteuid()
        || (observed.st_mode & 07777) != 0400
        || observed.st_nlink != 1) {
        minimizer_generation_storage_error_as_cpp(
            "generation commit-unlink inode contract is invalid");
    }
    arm_reserved_slot(
        reservation, ordinal, file_action::unlink_on_commit,
        resolved.final_path.data(), observed.st_dev, observed.st_ino,
        k_no_paired_file_action);
}

[[nodiscard]] inline file_action_diagnostics_snapshot
generation_file_action_diagnostics() noexcept {
    return file_action_diagnostics_snapshot{
        .entries = generation_file_action_detail::g_queue_state.diagnostics,
        .total =
            generation_file_action_detail::g_queue_state.diagnostic_total,
    };
}

#ifdef BIOSEQ_TEST_INDEXES
struct file_action_queue_test_snapshot {
    std::size_t reserved = 0u;
    std::size_t armed_abort = 0u;
    std::size_t armed_commit = 0u;
};

[[nodiscard]] inline file_action_queue_test_snapshot
generation_file_action_queue_test_snapshot() noexcept {
    using namespace generation_file_action_detail;
    file_action_queue_test_snapshot snapshot{};
    for (std::size_t slot = 0u; slot < k_pending_file_action_capacity;
         ++slot) {
        if (g_queue_state.slot_states[slot] == slot_state::reserved) {
            ++snapshot.reserved;
        } else if (g_queue_state.slot_states[slot] == slot_state::armed) {
            if (g_queue_state.actions[slot].action
                == file_action::unlink_on_abort) {
                ++snapshot.armed_abort;
            } else {
                ++snapshot.armed_commit;
            }
        }
    }
    return snapshot;
}

inline void arm_generation_file_callback_failure(
        const generation_file_action_detail::callback_failure_injection
            injection) noexcept {
    generation_file_action_detail::g_callback_failure_injection = injection;
}

inline void reset_generation_file_action_queue_for_test() noexcept {
    using namespace generation_file_action_detail;
    const bool xact_callback_installed =
        g_queue_state.xact_callback_installed;
    const bool subxact_callback_installed =
        g_queue_state.subxact_callback_installed;
    g_queue_state = queue_state{};
    g_queue_state.xact_callback_installed = xact_callback_installed;
    g_queue_state.subxact_callback_installed =
        subxact_callback_installed;
}
#endif

namespace minimizer_fragment_image_pg_detail {

[[nodiscard]] inline minimizer_generation_file_identity generation_file_identity(
        const minimizer_generation_administration_identity& identity,
        const std::uint64_t query_key,
        const std::uint64_t generation_id) {
    if (identity.index_name != k_minimizers_per_fragment_index_name
        || identity.database_oid != MyDatabaseId
        || identity.column_attno <= 0) {
        minimizer_generation_storage_error_as_cpp(
            "minimizer fragment image file identity is not the current minimizer-fragment relation");
    }
    return minimizer_generation_file_identity{
        .database_oid = identity.database_oid,
        .tablespace_oid = identity.tablespace_oid,
        .main_relfilenode = identity.main_relfilenode,
        .user_relid = identity.user_relid,
        .column_attno = identity.column_attno,
        .index_identity = k_mapped_minimizer_fragment_index_identity,
        .query_key = query_key,
        .generation_id = generation_id,
    };
}

inline void pwrite_all(
        const int fd,
        const std::span<const std::byte> bytes,
        const std::uint64_t offset) {
    std::size_t written = 0u;
    while (written < bytes.size()) {
        if (written > (std::numeric_limits<std::uint64_t>::max)() - offset) {
            minimizer_generation_storage_error_as_cpp("minimizer fragment image write offset overflows");
        }
        const std::uint64_t current = offset + written;
        if (current > static_cast<std::uint64_t>(
                (std::numeric_limits<off_t>::max)())) {
            minimizer_generation_storage_error_as_cpp(
                "minimizer fragment image write offset exceeds off_t");
        }
        const ssize_t result = pg_pwrite(
            fd,
            bytes.data() + static_cast<std::ptrdiff_t>(written),
            bytes.size() - written,
            static_cast<off_t>(current));
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) {
            minimizer_generation_storage_error_as_cpp(fmt::format(
                "minimizer fragment image write failed: errno={}", errno));
        }
        written += static_cast<std::size_t>(result);
    }
}

inline void write_zero_padding(
        const int fd,
        const std::uint64_t begin,
        const std::uint64_t end) {
    static constexpr std::array<std::byte, 64u> k_zeros{};
    if (end < begin || end - begin > k_zeros.size()) {
        minimizer_generation_storage_error_as_cpp("minimizer fragment image padding geometry is invalid");
    }
    pwrite_all(
        fd,
        std::span<const std::byte>{k_zeros}.first(
            static_cast<std::size_t>(end - begin)),
        begin);
}

/// Extend the private temporary inode to one checked, mmap-representable
/// image length. No final name is visible while this operation can fail.
inline void resize_temporary_image(
        const int fd,
        const std::uint64_t image_bytes) {
    if (fd < 0
        || image_bytes > static_cast<std::uint64_t>(
            (std::numeric_limits<off_t>::max)())
        || image_bytes > static_cast<std::uint64_t>(
            (std::numeric_limits<std::size_t>::max)())) {
        minimizer_generation_storage_error_as_cpp(
            "minimizer fragment image image length is not mmap/off_t representable");
    }
    while (::ftruncate(fd, static_cast<off_t>(image_bytes)) != 0) {
        if (errno == EINTR) continue;
        minimizer_generation_storage_error_as_cpp(fmt::format(
            "minimizer fragment image image resize failed: errno={}", errno));
    }
}

/// Scope-bound offset-zero shared mapping. Offset zero is page-aligned on
/// every supported PostgreSQL host, while callers expose only exact sections.
class scoped_mapped_region {
private:
    void* mapping_ = MAP_FAILED;
    std::size_t bytes_ = 0u;

public:
    scoped_mapped_region(
            const int fd,
            const std::uint64_t bytes,
            const int protection)
        : bytes_(static_cast<std::size_t>(bytes)) {
        if (fd < 0 || bytes == 0u
            || bytes > static_cast<std::uint64_t>(
                (std::numeric_limits<std::size_t>::max)())) {
            minimizer_generation_storage_error_as_cpp(
                "minimizer fragment image mapping geometry is invalid");
        }
        mapping_ = mmap(
            nullptr, bytes_, protection, MAP_SHARED, fd, 0);
        if (mapping_ == MAP_FAILED) {
            minimizer_generation_storage_error_as_cpp(fmt::format(
                "minimizer fragment image mmap failed: errno={}", errno));
        }
    }

    scoped_mapped_region(const scoped_mapped_region&) = delete;
    scoped_mapped_region& operator=(const scoped_mapped_region&) = delete;
    scoped_mapped_region(scoped_mapped_region&&) = delete;
    scoped_mapped_region& operator=(scoped_mapped_region&&) = delete;

    ~scoped_mapped_region() {
        if (mapping_ != MAP_FAILED) {
            (void)munmap(mapping_, bytes_);
        }
    }

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return {
            static_cast<const std::byte*>(mapping_), bytes_};
    }

    [[nodiscard]] std::span<std::byte> writable_bytes() noexcept {
        return {static_cast<std::byte*>(mapping_), bytes_};
    }

    /// Persist every dirty page before the outer header can authorize it.
    void sync() const {
        while (msync(mapping_, bytes_, MS_SYNC) != 0) {
            if (errno == EINTR) continue;
            minimizer_generation_storage_error_as_cpp(fmt::format(
                "minimizer fragment image mapped image sync failed: errno={}", errno));
        }
    }
};

[[nodiscard]] inline bioseq::indexing::expected_minimizer_fragment_image_identity
expected_identity(
        const minimizer_generation_file_identity& identity,
        const minimizer_generation_head& head,
        const minimizer_generation_manifest& manifest,
        const bioseq::indexing::minimizer_fragment_image_layout& layout,
        const std::uint64_t header_crc64) noexcept {
    return bioseq::indexing::expected_minimizer_fragment_image_identity{
        .database_oid = identity.database_oid,
        .tablespace_oid = identity.tablespace_oid,
        .main_relfilenode = identity.main_relfilenode,
        .user_relid = identity.user_relid,
        .column_attno = static_cast<std::uint16_t>(identity.column_attno),
        .index_identity =
            bioseq::indexing::k_minimizer_fragment_image_index_identity,
        .layout_identity =
            bioseq::indexing::k_minimizer_fragment_image_layout_identity,
        .query_key = identity.query_key,
        .generation_id = identity.generation_id,
        .source_identity = head.source_identity,
        .source_content_crc = head.source_content_checksum,
        .storage_contract_checksum = head.storage_contract_checksum,
        .target_length = manifest.target_length,
        .file_bytes = layout.file_bytes,
        .position_segment_count = manifest.position_segment_count,
        .position_segment_span_bp = manifest.position_segment_span_bp,
        .payload = layout.payload,
        .directory = layout.directory,
        .manifest = layout.manifest,
        .header_crc64 = header_crc64,
        .feature_bitmap = layout.feature_bitmap,
        .compact_hash32_direct = layout.compact_hash32_direct,
    };
}

inline void validate_generation_metadata(
        const minimizer_generation_file_identity& identity,
        const minimizer_generation_head& head,
        const minimizer_generation_manifest& manifest,
        const std::span<const std::byte> directory,
        const std::span<const std::byte> encoded_manifest) {
    if (manifest.layout
            != bioseq::indexing::minimizer_generation_layout::
                grouped_soa_segment_local
        || static_cast<std::uint16_t>(manifest.layout)
            != bioseq::indexing::k_minimizer_fragment_image_layout_identity
        || identity.generation_id == 0u
        || identity.generation_id != head.generation_id
        || identity.generation_id != manifest.generation_id
        || head.source_identity != manifest.source_identity
        || head.source_content_checksum
            != manifest.source_content_checksum
        || head.storage_contract_checksum
            != bioseq::indexing::compute_minimizer_generation_storage_contract_checksum(
                manifest)
        || directory.size() != manifest.directory_bytes
        || bioseq::indexing::crc64_ecma182(directory)
            != manifest.directory_checksum
        || encoded_manifest.size()
            != minimizer_generation_manifest::encoded_bytes
        || bioseq::indexing::crc64_ecma182(encoded_manifest)
            != head.manifest_checksum) {
        minimizer_generation_storage_error_as_cpp(
            "minimizer fragment image generation metadata receipt is invalid");
    }
}

struct mapping_lease {
    ResourceOwner owner = nullptr;
    int fd = -1;
    void* mapping = MAP_FAILED;
    std::size_t mapping_bytes = 0u;
    dev_t device{};
    ino_t inode{};
    bool remembered = false;
};

inline void release_mapping_lease_raw(mapping_lease& lease) noexcept {
    if (lease.mapping != MAP_FAILED) {
        (void)munmap(lease.mapping, lease.mapping_bytes);
        lease.mapping = MAP_FAILED;
        lease.mapping_bytes = 0u;
    }
    if (lease.fd >= 0) {
        (void)::close(lease.fd);
        lease.fd = -1;
    }
}

inline void release_mapping_lease_resource(const Datum value) noexcept {
    auto* const lease = reinterpret_cast<mapping_lease*>(DatumGetPointer(value));
    if (lease == nullptr) return;
    lease->remembered = false;
    release_mapping_lease_raw(*lease);
}

inline constexpr ResourceReleasePriority k_mapping_lease_release_priority = 10u;
static_assert(k_mapping_lease_release_priority >= RELEASE_PRIO_FIRST);
inline const ResourceOwnerDesc k_mapping_lease_resource_owner_descriptor{
    .name = "bioseq minimizer fragment image mapping lease",
    .release_phase = RESOURCE_RELEASE_AFTER_LOCKS,
    .release_priority = k_mapping_lease_release_priority,
    .ReleaseResource = release_mapping_lease_resource,
    .DebugPrint = nullptr,
};

[[nodiscard]] inline mapping_lease* allocate_mapping_lease() {
    if (CurrentResourceOwner == nullptr || TopTransactionContext == nullptr) {
        minimizer_generation_storage_error_as_cpp(
            "minimizer fragment image mapping requires a transaction ResourceOwner");
    }
    void* const storage = MemoryContextAlloc(
        TopTransactionContext, sizeof(mapping_lease));
    return ::new (storage) mapping_lease{
        .owner = CurrentResourceOwner,
    };
}

inline void destroy_mapping_lease(mapping_lease*& lease) noexcept {
    if (lease == nullptr) return;
    if (lease->remembered) {
        ResourceOwnerForget(
            lease->owner, PointerGetDatum(lease),
            &k_mapping_lease_resource_owner_descriptor);
        lease->remembered = false;
    }
    release_mapping_lease_raw(*lease);
    lease->~mapping_lease();
    pfree(lease);
    lease = nullptr;
}

/// Form the lifetime token expected by PostgreSQL-free mapped-index
/// readers without transferring or extending the lease lifetime.  The token
/// deliberately has no control block: callers must retain the backend-thread
/// lease guard which supplied it until every borrowed span has been destroyed.
[[nodiscard]] inline std::shared_ptr<const void>
borrow_mapping_token(mapping_lease* const lease) noexcept {
    if (lease == nullptr || lease->owner == nullptr || !lease->remembered
        || lease->fd < 0 || lease->mapping == MAP_FAILED
        || lease->mapping == nullptr || lease->mapping_bytes == 0u) {
        return {};
    }
    std::shared_ptr<const void> no_owner;
    return std::shared_ptr<const void>{no_owner, lease->mapping};
}

struct page_aligned_mapping_range {
    std::size_t begin = 0u;
    std::size_t end = 0u;

    [[nodiscard]] constexpr bool operator==(
        const page_aligned_mapping_range&) const noexcept = default;
};

/// Translate one byte-exact authenticated section into the page interval
/// accepted by madvise-style APIs. The final interval never exceeds the
/// mapping; its first/last page may also contain adjacent wire bytes because
/// the VM can only advise complete pages.
[[nodiscard]] constexpr std::optional<page_aligned_mapping_range>
page_aligned_advice_range(
        const std::size_t mapping_bytes,
        const std::size_t raw_begin,
        const std::size_t raw_bytes,
        const std::size_t page_bytes) noexcept {
    if (mapping_bytes == 0u || raw_bytes == 0u || page_bytes == 0u
        || raw_begin >= mapping_bytes) {
        return std::nullopt;
    }
    const std::size_t available = mapping_bytes - raw_begin;
    const std::size_t raw_end = raw_begin
        + std::min(raw_bytes, available);
    const std::size_t aligned_begin = raw_begin
        - raw_begin % page_bytes;
    std::size_t aligned_end = raw_end;
    const std::size_t remainder = aligned_end % page_bytes;
    if (remainder != 0u) {
        const std::size_t growth = page_bytes - remainder;
        aligned_end = growth > mapping_bytes - aligned_end
            ? mapping_bytes
            : aligned_end + growth;
    }
    if (aligned_end <= aligned_begin) return std::nullopt;
    return page_aligned_mapping_range{
        .begin = aligned_begin,
        .end = aligned_end,
    };
}

static_assert(page_aligned_advice_range(
    16'384u, 4'352u, 4'096u, 4'096u)
    == page_aligned_mapping_range{4'096u, 12'288u});
static_assert(page_aligned_advice_range(
    10'000u, 9'000u, 1'000u, 4'096u)
    == page_aligned_mapping_range{8'192u, 10'000u});
static_assert(!page_aligned_advice_range(
    10'000u, 10'000u, 1u, 4'096u));

[[nodiscard]] constexpr std::size_t file_advice_chunk_bytes(
        const std::size_t remaining) noexcept {
    return std::min<std::size_t>(
        remaining,
        static_cast<std::size_t>((std::numeric_limits<int>::max)()));
}

static_assert(file_advice_chunk_bytes(17u) == 17u);
static_assert(file_advice_chunk_bytes(
    static_cast<std::size_t>((std::numeric_limits<int>::max)()) + 1u)
    == static_cast<std::size_t>((std::numeric_limits<int>::max)()));

/// Ask the host to begin fetching one already-validated file range without
/// making success part of correctness. No error is logged or propagated: a
/// filesystem/kernel which rejects the preferred async file hint receives a
/// best-effort mapped-page hint, and rejection of that hint is also harmless.
inline void advise_mapping_range_async_best_effort(
        const mapping_lease& lease,
        const page_aligned_mapping_range range) noexcept {
    if (lease.fd < 0 || lease.mapping == MAP_FAILED
        || lease.mapping == nullptr || range.end <= range.begin
        || range.end > lease.mapping_bytes) {
        return;
    }

    bool file_advice_accepted = false;
#if defined(__APPLE__)
    std::size_t cursor = range.begin;
    file_advice_accepted = true;
    while (cursor < range.end) {
        const std::size_t remaining = range.end - cursor;
        const int count = static_cast<int>(
            file_advice_chunk_bytes(remaining));
        struct radvisory advice {
            .ra_offset = static_cast<off_t>(cursor),
            .ra_count = count,
        };
        int advised = -1;
        do {
            advised = ::fcntl(lease.fd, F_RDADVISE, &advice);
        } while (advised == -1 && errno == EINTR);
        if (advised == -1) {
            file_advice_accepted = false;
            break;
        }
        cursor += static_cast<std::size_t>(count);
    }
#elif defined(__linux__)
    int advised = 0;
    do {
        advised = ::posix_fadvise(
            lease.fd,
            static_cast<off_t>(range.begin),
            static_cast<off_t>(range.end - range.begin),
            POSIX_FADV_WILLNEED);
    } while (advised == EINTR);
    file_advice_accepted = advised == 0;
#endif

#if defined(MADV_WILLNEED)
    if (!file_advice_accepted) {
        auto* const address = static_cast<std::byte*>(lease.mapping)
            + range.begin;
        int advised = -1;
        do {
            advised = ::madvise(
                address, range.end - range.begin, MADV_WILLNEED);
        } while (advised == -1 && errno == EINTR);
        (void)advised;
    }
#else
    (void)file_advice_accepted;
#endif
}

/// Borrowed callback used by the PostgreSQL-free selected-page coalescer.
/// Advice rejection is intentionally invisible to query correctness.  The
/// absolute interval is translated only after proving it still belongs to the
/// stable mapping_lease which supplied the callback token.
inline void advise_borrowed_mapping_page_range(
        void* const context,
        const std::uintptr_t page_begin,
        const std::uintptr_t page_end) noexcept {
    auto* const lease = static_cast<mapping_lease*>(context);
    if (lease == nullptr || lease->owner == nullptr || !lease->remembered
        || lease->fd < 0 || lease->mapping == MAP_FAILED
        || lease->mapping == nullptr || lease->mapping_bytes == 0u
        || page_begin >= page_end) {
        return;
    }
    const std::uintptr_t mapping_begin =
        reinterpret_cast<std::uintptr_t>(lease->mapping);
    if (lease->mapping_bytes
        > (std::numeric_limits<std::uintptr_t>::max)() - mapping_begin) {
        return;
    }
    const std::uintptr_t mapping_end = mapping_begin
        + lease->mapping_bytes;
    if (page_begin < mapping_begin || page_begin >= mapping_end) return;
    const std::uintptr_t clipped_end = std::min(page_end, mapping_end);
    if (clipped_end <= page_begin) return;
    advise_mapping_range_async_best_effort(
        *lease,
        page_aligned_mapping_range{
            .begin = static_cast<std::size_t>(page_begin - mapping_begin),
            .end = static_cast<std::size_t>(clipped_end - mapping_begin),
        });
}

} // namespace minimizer_fragment_image_pg_detail

/// Shared header-last writer. Relation generations feed this from their
/// authenticated heap reread through the common grouped-SoA encoder callbacks.
class minimizer_fragment_image_writer {
private:
    minimizer_generation_file_identity identity_{};
    pending_minimizer_generation_file_publication publication_{};
    std::uint64_t payload_bytes_ = 0u;
    crc64_ecma182_accumulator payload_crc_{};
    bool sealed_ = false;

public:
    minimizer_fragment_image_writer(
            const minimizer_generation_administration_identity& identity,
            const std::uint64_t query_key,
            const std::uint64_t generation_id)
        : identity_(minimizer_fragment_image_pg_detail::generation_file_identity(
              identity, query_key, generation_id)),
          publication_(begin_generation_file_publication(identity_)) {}

    minimizer_fragment_image_writer(const minimizer_fragment_image_writer&) = delete;
    minimizer_fragment_image_writer& operator=(const minimizer_fragment_image_writer&) = delete;
    minimizer_fragment_image_writer(minimizer_fragment_image_writer&&) = delete;
    minimizer_fragment_image_writer& operator=(minimizer_fragment_image_writer&&) = delete;

    void append(const std::span<const std::byte> bytes) {
        if (sealed_ || publication_.fd < 0
            || bytes.size()
                > (std::numeric_limits<std::uint64_t>::max)()
                    - payload_bytes_) {
            minimizer_generation_storage_error_as_cpp(
                "minimizer fragment image payload append state/size is invalid");
        }
        minimizer_fragment_image_pg_detail::pwrite_all(
            publication_.fd, bytes,
            bioseq::indexing::k_minimizer_fragment_image_header_bytes + payload_bytes_);
        payload_crc_.update(bytes);
        payload_bytes_ += bytes.size();
    }

    [[nodiscard]] minimizer_fragment_image_file_receipt publish(
            const minimizer_generation_head& head,
            const minimizer_generation_manifest& manifest,
            const grouped_soa_directory_view& authenticated_directory,
            const std::span<const std::byte> encoded_manifest) {
        using namespace bioseq::indexing;
        const std::span<const std::byte> directory =
            authenticated_directory.encoded_bytes();
        minimizer_fragment_image_pg_detail::validate_generation_metadata(
            identity_, head, manifest, directory, encoded_manifest);
        if (sealed_ || publication_.fd < 0
            || payload_bytes_ != manifest.payload_bytes) {
            minimizer_generation_storage_error_as_cpp(
                "minimizer fragment image publication payload receipt is invalid");
        }
        const std::uint64_t payload_crc64 = payload_crc_.checksum();
        if (payload_bytes_
                > (std::numeric_limits<std::uint64_t>::max)()
                    - k_minimizer_fragment_image_header_bytes) {
            minimizer_generation_storage_error_as_cpp(
                "minimizer fragment image payload mapping length overflows");
        }
        const std::uint64_t payload_mapping_bytes =
            k_minimizer_fragment_image_header_bytes + payload_bytes_;
        minimizer_fragment_image_pg_detail::resize_temporary_image(
            publication_.fd, payload_mapping_bytes);
        minimizer_fragment_image_pg_detail::scoped_mapped_region payload_mapping{
            publication_.fd, payload_mapping_bytes, PROT_READ};
        const std::span<const std::byte> mapped_payload =
            payload_mapping.bytes().subspan(
                static_cast<std::size_t>(k_minimizer_fragment_image_header_bytes),
                static_cast<std::size_t>(payload_bytes_));
        auto opened_adapter = minimizer_fragment_compact_hash32_adapter::open(
            opened_minimizer_fragment_generation_view{
                .head = &head,
                .manifest = &manifest,
                .directory = &authenticated_directory,
                .mapped_payload = mapped_payload,
            });
        if (!opened_adapter) {
            minimizer_generation_storage_error_as_cpp(
                "minimizer fragment image compact-H32 source adapter rejected grouped-SoA");
        }
        minimizer_fragment_compact_hash32_adapter compact_adapter =
            std::move(*opened_adapter);
        const auto profile = derive_grouped_soa_minimizer_profile(manifest);
        if (!profile || profile->max_occ_cap == 0u
            || profile->max_occ_cap
                > (std::numeric_limits<std::uint32_t>::max)()) {
            minimizer_generation_storage_error_as_cpp(
                "minimizer fragment image persisted grouped-SoA cap is invalid");
        }
        const std::uint32_t persisted_max_occ_cap =
            static_cast<std::uint32_t>(
                profile->max_occ_cap);
        const auto compact_options = compact_adapter.compact_build_options(
            persisted_max_occ_cap);
        if (!compact_options) {
            minimizer_generation_storage_error_as_cpp(
                "minimizer fragment image compact-H32 build options are incompatible");
        }
        const compact_hash32_sorted_run_source compact_source =
            compact_adapter.sorted_run_source();
        const auto compact_plan = plan_compact_h32_direct_index(
            compact_source, *compact_options);
        if (!compact_plan) {
            minimizer_generation_storage_error_as_cpp(
                "minimizer fragment image compact-H32 image planning failed");
        }

        const auto provisional_layout = plan_minimizer_fragment_image_layout(
            payload_bytes_, directory.size(), encoded_manifest.size(),
            payload_crc64, manifest.directory_checksum,
            head.manifest_checksum,
            minimizer_fragment_compact_hash32_section_spec{
                .bytes = compact_plan->image_bytes,
                .direct_header_crc64 = 0u,
            });
        if (!provisional_layout
            || provisional_layout->feature_bitmap
                != k_minimizer_fragment_compact_hash32_feature
            || provisional_layout->compact_hash32_direct.bytes
                != compact_plan->image_bytes) {
            minimizer_generation_storage_error_as_cpp(
                "minimizer fragment image direct-section layout planning failed");
        }
        const std::uint64_t payload_end =
            provisional_layout->payload.offset
                + provisional_layout->payload.bytes;
        minimizer_fragment_image_pg_detail::write_zero_padding(
            publication_.fd, payload_end,
            provisional_layout->directory.offset);
        minimizer_fragment_image_pg_detail::pwrite_all(
            publication_.fd, directory,
            provisional_layout->directory.offset);
        const std::uint64_t directory_end =
            provisional_layout->directory.offset
                + provisional_layout->directory.bytes;
        minimizer_fragment_image_pg_detail::write_zero_padding(
            publication_.fd, directory_end,
            provisional_layout->manifest.offset);
        minimizer_fragment_image_pg_detail::pwrite_all(
            publication_.fd, encoded_manifest,
            provisional_layout->manifest.offset);
        const std::uint64_t manifest_end =
            provisional_layout->manifest.offset
                + provisional_layout->manifest.bytes;
        minimizer_fragment_image_pg_detail::write_zero_padding(
            publication_.fd, manifest_end,
            provisional_layout->compact_hash32_direct.offset);
        minimizer_fragment_image_pg_detail::resize_temporary_image(
            publication_.fd, provisional_layout->file_bytes);

        std::uint64_t direct_header_crc64 = 0u;
        {
            minimizer_fragment_image_pg_detail::scoped_mapped_region destination_mapping{
                publication_.fd, provisional_layout->file_bytes,
                PROT_READ | PROT_WRITE};
            const auto direct_offset = static_cast<std::size_t>(
                provisional_layout->compact_hash32_direct.offset);
            const auto direct_bytes = static_cast<std::size_t>(
                provisional_layout->compact_hash32_direct.bytes);
            std::span<std::byte> direct_image =
                destination_mapping.writable_bytes().subspan(
                    direct_offset, direct_bytes);
            if (auto built = build_compact_h32_direct_index_into(
                    compact_source, *compact_options,
                    *compact_plan, direct_image);
                !built) {
                minimizer_generation_storage_error_as_cpp(
                    "minimizer fragment image compact-H32 image replay/build failed");
            }
            const auto validated_direct =
                validate_compact_h32_direct_index(
                    std::span<const std::byte>{direct_image},
                    compact_options->identity,
                    compact_options->max_occ_cap,
                    compact_options->geometry);
            if (!validated_direct) {
                minimizer_generation_storage_error_as_cpp(
                    "minimizer fragment image compact-H32 nested image validation failed");
            }
            const compact_hash32_image_plan& validated_direct_plan =
                validated_direct->header;
            if (validated_direct_plan != *compact_plan) {
                minimizer_generation_storage_error_as_cpp(
                    "minimizer fragment image compact-H32 validated plan receipt changed");
            }
            direct_header_crc64 = crc64_ecma182(
                std::span<const std::byte>{direct_image}.first(
                    k_compact_hash32_direct_index_header_bytes));
            destination_mapping.sync();
        }

        const auto layout = plan_minimizer_fragment_image_layout(
            payload_bytes_, directory.size(), encoded_manifest.size(),
            payload_crc64, manifest.directory_checksum,
            head.manifest_checksum,
            minimizer_fragment_compact_hash32_section_spec{
                .bytes = compact_plan->image_bytes,
                .direct_header_crc64 = direct_header_crc64,
            });
        if (!layout
            || layout->payload != provisional_layout->payload
            || layout->directory != provisional_layout->directory
            || layout->manifest != provisional_layout->manifest
            || layout->feature_bitmap
                != provisional_layout->feature_bitmap
            || layout->compact_hash32_direct.offset
                != provisional_layout->compact_hash32_direct.offset
            || layout->compact_hash32_direct.bytes
                != provisional_layout->compact_hash32_direct.bytes
            || layout->file_bytes != provisional_layout->file_bytes) {
            minimizer_generation_storage_error_as_cpp(
                "minimizer fragment image authenticated direct layout changed after build");
        }

        minimizer_fragment_image_header header{
            .magic = k_minimizer_fragment_image_magic,
            .layout_revision = k_minimizer_fragment_image_layout_revision,
            .header_bytes = k_minimizer_fragment_image_header_bytes,
            .database_oid = identity_.database_oid,
            .tablespace_oid = identity_.tablespace_oid,
            .main_relfilenode = identity_.main_relfilenode,
            .user_relid = identity_.user_relid,
            .column_attno = static_cast<std::uint16_t>(
                identity_.column_attno),
            .index_identity = k_minimizer_fragment_image_index_identity,
            .storage_kind = bioseq::indexing::
                k_minimizer_fragment_image_relation_backed_storage_kind,
            .layout_identity = k_minimizer_fragment_image_layout_identity,
            .query_key = identity_.query_key,
            .generation_id = identity_.generation_id,
            .source_identity = head.source_identity,
            .source_content_crc = head.source_content_checksum,
            .storage_contract_checksum = head.storage_contract_checksum,
            .target_length = manifest.target_length,
            .file_bytes = layout->file_bytes,
            .position_segment_count = manifest.position_segment_count,
            .position_segment_span_bp =
                manifest.position_segment_span_bp,
            .payload = layout->payload,
            .directory = layout->directory,
            .manifest = layout->manifest,
            .feature_bitmap = layout->feature_bitmap,
            .compact_hash32_direct = layout->compact_hash32_direct,
        };
        const auto encoded_header =
            encode_minimizer_fragment_image_header(header);
        if (!encoded_header) {
            minimizer_generation_storage_error_as_cpp("minimizer fragment image header encoding failed");
        }
        const auto decoded_header =
            decode_minimizer_fragment_image_header(*encoded_header);
        if (!decoded_header) {
            minimizer_generation_storage_error_as_cpp("minimizer fragment image header self-decode failed");
        }
        minimizer_fragment_image_pg_detail::pwrite_all(
            publication_.fd, *encoded_header, 0u);

        struct stat observed{};
        if (fstat(publication_.fd, &observed) != 0
            || observed.st_size < 0
            || static_cast<std::uint64_t>(observed.st_size)
                != layout->file_bytes
            || observed.st_dev != publication_.expected_device
            || observed.st_ino != publication_.expected_inode) {
            minimizer_generation_storage_error_as_cpp(
                "minimizer fragment image completed temp image size/inode is invalid");
        }
        const minimizer_fragment_image_validation_options validation_options{
            .expected_identity = minimizer_fragment_image_pg_detail::expected_identity(
                identity_, head, manifest, *layout,
                decoded_header->header_crc64),
            .payload_crc_policy = minimizer_fragment_payload_crc_policy::deferred,
        };
        {
            minimizer_fragment_image_pg_detail::scoped_mapped_region validation_mapping{
                publication_.fd, layout->file_bytes, PROT_READ};
            const auto validated = validate_minimizer_fragment_image(
                validation_mapping.bytes(), validation_options);
            if (!validated || validated->payload_crc_bytes != 0u) {
                minimizer_generation_storage_error_as_cpp(
                    "minimizer fragment image bounded publication validation failed");
            }
        }

        const minimizer_fragment_image_file_receipt receipt{
            .file_bytes = layout->file_bytes,
            .payload_crc64 = payload_crc64,
            .header_crc64 = decoded_header->header_crc64,
            .device = publication_.expected_device,
            .inode = publication_.expected_inode,
        };
        publish_generation_file(publication_);
        sealed_ = true;
        return receipt;
    }
};

enum class minimizer_fragment_image_mapped_open_validation : std::uint8_t {
    /// Timed restart path: authenticate only the fixed outer header, layout,
    /// descriptor identity and section receipts. No section payload is read.
    bounded_header_only = 0u,
    /// Explicit publication/scrub validation. Payload CRC remains
    /// deferred, while directory/manifest metadata receive complete
    /// validation.
    full_metadata = 1u,
};

/// Backend-thread RAII authority for one registered minimizer fragment image mmap/fd lease.
///
/// Borrowed reader tokens do not own or prolong this guard.  On the normal
/// executor path the guard must therefore be declared/stored before every
/// reader which borrows it, so reverse destruction releases the reader first
/// and then promptly forgets, unmaps, closes and frees the lease.  PostgreSQL's
/// ResourceOwner registration remains armed while the guard is live and is the
/// ERROR/longjmp fallback when ordinary C++ destruction cannot run.
class mapped_minimizer_fragment_lease_guard {
private:
    minimizer_fragment_image_pg_detail::mapping_lease* lease_ = nullptr;

    explicit mapped_minimizer_fragment_lease_guard(
            minimizer_fragment_image_pg_detail::mapping_lease* lease) noexcept
        : lease_(lease) {}

    friend class mapped_minimizer_fragment_image;

public:
    mapped_minimizer_fragment_lease_guard() = default;
    mapped_minimizer_fragment_lease_guard(
            const mapped_minimizer_fragment_lease_guard&) = delete;
    mapped_minimizer_fragment_lease_guard& operator=(
            const mapped_minimizer_fragment_lease_guard&) = delete;

    mapped_minimizer_fragment_lease_guard(
            mapped_minimizer_fragment_lease_guard&& other) noexcept
        : lease_(std::exchange(other.lease_, nullptr)) {}

    mapped_minimizer_fragment_lease_guard& operator=(
            mapped_minimizer_fragment_lease_guard&& other) noexcept {
        if (this != &other) {
            minimizer_fragment_image_pg_detail::destroy_mapping_lease(lease_);
            lease_ = std::exchange(other.lease_, nullptr);
        }
        return *this;
    }

    ~mapped_minimizer_fragment_lease_guard() {
        minimizer_fragment_image_pg_detail::destroy_mapping_lease(lease_);
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(borrow_mapping_token());
    }

    /// Non-owning lifetime token. Every copy becomes invalid as soon as
    /// this guard is destroyed or replaced.
    [[nodiscard]] std::shared_ptr<const void>
    borrow_mapping_token() const noexcept {
        return minimizer_fragment_image_pg_detail::borrow_mapping_token(lease_);
    }
};

static_assert(!std::is_copy_constructible_v<
    mapped_minimizer_fragment_lease_guard>);
static_assert(!std::is_copy_assignable_v<
    mapped_minimizer_fragment_lease_guard>);
static_assert(std::is_nothrow_move_constructible_v<
    mapped_minimizer_fragment_lease_guard>);
static_assert(std::is_nothrow_move_assignable_v<
    mapped_minimizer_fragment_lease_guard>);

class mapped_minimizer_fragment_image {
private:
    minimizer_fragment_image_pg_detail::mapping_lease* lease_ = nullptr;
    std::span<const std::byte> payload_{};
    std::span<const std::byte> compact_h32_direct_{};
    std::uint64_t payload_raw_checksum_ = 0u;
    std::optional<std::uint64_t>
        compact_h32_direct_header_receipt_{};

    void clear_views_() noexcept {
        payload_ = {};
        compact_h32_direct_ = {};
        payload_raw_checksum_ = 0u;
        compact_h32_direct_header_receipt_.reset();
    }

    void release_() noexcept {
        clear_views_();
        minimizer_fragment_image_pg_detail::destroy_mapping_lease(lease_);
    }

public:
    mapped_minimizer_fragment_image() = default;
    mapped_minimizer_fragment_image(
            const mapped_minimizer_fragment_image&) = delete;
    mapped_minimizer_fragment_image& operator=(
            const mapped_minimizer_fragment_image&) = delete;
    mapped_minimizer_fragment_image(
            mapped_minimizer_fragment_image&& other) noexcept
        : lease_(std::exchange(other.lease_, nullptr)),
          payload_(std::exchange(other.payload_, {})),
          compact_h32_direct_(std::exchange(
              other.compact_h32_direct_, {})),
          payload_raw_checksum_(std::exchange(
              other.payload_raw_checksum_, 0u)),
          compact_h32_direct_header_receipt_(std::exchange(
              other.compact_h32_direct_header_receipt_, std::nullopt)) {}
    mapped_minimizer_fragment_image& operator=(
            mapped_minimizer_fragment_image&&) = delete;
    ~mapped_minimizer_fragment_image() {
        release_();
    }

    [[nodiscard]] std::span<const std::byte> payload() const noexcept {
        return payload_;
    }

    [[nodiscard]] std::uint64_t payload_raw_checksum() const noexcept {
        return payload_raw_checksum_;
    }

    /// Authenticated lazy span for the optional nested compact-H32 direct
    /// image. The bounded outer open formed this span arithmetically without
    /// reading any nested payload byte.
    [[nodiscard]] std::span<const std::byte>
    compact_h32_direct() const noexcept {
        return compact_h32_direct_;
    }

    /// Raw CRC64/ECMA receipt over the finalized fixed 256-byte nested header,
    /// persisted in and authenticated by the outer minimizer fragment image header.
    [[nodiscard]] std::optional<std::uint64_t>
    compact_h32_direct_header_receipt() const noexcept {
        return compact_h32_direct_header_receipt_;
    }

    /// Issue fail-open asynchronous readahead for the authenticated compact-
    /// H32 metadata only. Selected posting pages are advised later, while the
    /// existing descriptor layout traversal already knows their exact spans.
    /// The unrelated outer minimizer-fragment payload and unselected posting pages are never
    /// named as byte ranges; page granularity may include adjacent bytes on a
    /// boundary page.
    void advise_compact_h32_direct_metadata_readahead(
            const bioseq::indexing::compact_hash32_direct_index_view&
                authenticated_direct) const noexcept {
        using minimizer_fragment_image_pg_detail::page_aligned_mapping_range;
        if (lease_ == nullptr || lease_->mapping == MAP_FAILED
            || lease_->mapping == nullptr || lease_->mapping_bytes == 0u
            || compact_h32_direct_.empty()
            || authenticated_direct.image.data()
                != compact_h32_direct_.data()
            || authenticated_direct.image.size()
                != compact_h32_direct_.size()) {
            return;
        }

        const std::uintptr_t mapping_address = reinterpret_cast<std::uintptr_t>(
            lease_->mapping);
        const std::uintptr_t direct_address = reinterpret_cast<std::uintptr_t>(
            compact_h32_direct_.data());
        if (direct_address < mapping_address) return;
        const std::uint64_t direct_file_offset =
            static_cast<std::uint64_t>(direct_address - mapping_address);
        if (direct_file_offset > lease_->mapping_bytes
            || compact_h32_direct_.size()
                > lease_->mapping_bytes
                    - static_cast<std::size_t>(direct_file_offset)) {
            return;
        }

        const auto& header = authenticated_direct.header;
        if (header.image_bytes != compact_h32_direct_.size()) return;
        const std::array<std::pair<std::uint64_t, std::uint64_t>, 3u>
            direct_sections{
                std::pair{header.membership_offset,
                    header.membership_bytes},
                std::pair{header.rank_offset, header.rank_bytes},
                std::pair{header.descriptors_offset,
                    header.descriptors_bytes},
            };
        std::array<page_aligned_mapping_range, direct_sections.size()>
            merged_ranges{};
        std::size_t merged_count = 0u;
        const long observed_page_bytes = ::sysconf(_SC_PAGESIZE);
        if (observed_page_bytes <= 0) return;
        const std::size_t page_bytes = static_cast<std::size_t>(
            observed_page_bytes);

        std::uint64_t previous_section_end = 0u;
        for (const auto [section_offset, section_bytes]
                : direct_sections) {
            if (section_offset < previous_section_end
                || section_offset > compact_h32_direct_.size()
                || section_bytes
                    > compact_h32_direct_.size()
                        - static_cast<std::size_t>(section_offset)) {
                return;
            }
            const std::uint64_t section_end = section_offset
                + section_bytes;
            previous_section_end = section_end;
            if (section_bytes == 0u) continue;

            if (section_offset
                    > (std::numeric_limits<std::uint64_t>::max)()
                        - direct_file_offset) {
                return;
            }
            const std::uint64_t file_offset = direct_file_offset
                + section_offset;
            if (file_offset > lease_->mapping_bytes
                || section_bytes > lease_->mapping_bytes
                    - static_cast<std::size_t>(file_offset)) {
                return;
            }
            const auto aligned = minimizer_fragment_image_pg_detail::
                page_aligned_advice_range(
                    lease_->mapping_bytes,
                    static_cast<std::size_t>(file_offset),
                    static_cast<std::size_t>(section_bytes),
                    page_bytes);
            if (!aligned) return;
            if (merged_count != 0u
                && aligned->begin <= merged_ranges[merged_count - 1u].end) {
                merged_ranges[merged_count - 1u].end = std::max(
                    merged_ranges[merged_count - 1u].end,
                    aligned->end);
            } else {
                merged_ranges[merged_count++] = *aligned;
            }
        }
        if (previous_section_end > header.postings_offset
            || header.postings_offset > header.image_bytes
            || header.postings_bytes
                > header.image_bytes - header.postings_offset
            || header.postings_offset + header.postings_bytes
                != header.image_bytes) {
            return;
        }
        for (std::size_t ordinal = 0u;
             ordinal < merged_count; ++ordinal) {
            minimizer_fragment_image_pg_detail::
                advise_mapping_range_async_best_effort(
                    *lease_, merged_ranges[ordinal]);
        }
    }

    /// Form a non-owning adviser before this image transfers its lease into
    /// the longer-lived guard. The context is the mapping_lease allocation
    /// itself, whose address remains stable across that transfer.
    [[nodiscard]] bioseq::indexing::compact_direct_selected_page_adviser
    borrow_compact_h32_selected_page_adviser(
            const bioseq::indexing::compact_hash32_direct_index_view&
                authenticated_direct) const noexcept {
        if (lease_ == nullptr || lease_->owner == nullptr
            || !lease_->remembered || lease_->fd < 0
            || lease_->mapping == MAP_FAILED || lease_->mapping == nullptr
            || lease_->mapping_bytes == 0u || compact_h32_direct_.empty()
            || authenticated_direct.image.data()
                != compact_h32_direct_.data()
            || authenticated_direct.image.size()
                != compact_h32_direct_.size()
            || authenticated_direct.header.image_bytes
                != compact_h32_direct_.size()) {
            return {};
        }
        const std::uintptr_t mapping_begin =
            reinterpret_cast<std::uintptr_t>(lease_->mapping);
        const std::uintptr_t direct_begin = reinterpret_cast<std::uintptr_t>(
            compact_h32_direct_.data());
        if (lease_->mapping_bytes
                > (std::numeric_limits<std::uintptr_t>::max)()
                    - mapping_begin
            || direct_begin < mapping_begin) {
            return {};
        }
        const std::uintptr_t mapping_end = mapping_begin
            + lease_->mapping_bytes;
        if (compact_h32_direct_.size()
                > (std::numeric_limits<std::uintptr_t>::max)()
                    - direct_begin
            || direct_begin + compact_h32_direct_.size() > mapping_end) {
            return {};
        }
        const long observed_page_bytes = ::sysconf(_SC_PAGESIZE);
        if (observed_page_bytes <= 0) return {};
        return bioseq::indexing::compact_direct_selected_page_adviser{
            .context = lease_,
            .page_bytes = static_cast<std::size_t>(observed_page_bytes),
            .advise_fn =
                &minimizer_fragment_image_pg_detail::advise_borrowed_mapping_page_range,
        };
    }

    /// Borrow the PG-free reader's shared_ptr-shaped lifetime token while
    /// retaining this image as the prompt normal-path lease authority.
    [[nodiscard]] std::shared_ptr<const void>
    borrow_mapping_token() const noexcept {
        return minimizer_fragment_image_pg_detail::borrow_mapping_token(lease_);
    }

    /// Transfer only the mmap/fd lifetime authority into a compact guard.  Any
    /// spans copied before this call remain valid until the returned guard is
    /// destroyed; this moved-from image no longer exposes mapped spans.
    [[nodiscard]] mapped_minimizer_fragment_lease_guard
    release_mapping_lease_guard() && noexcept {
        auto* const lease = std::exchange(lease_, nullptr);
        clear_views_();
        return mapped_minimizer_fragment_lease_guard{lease};
    }

    friend mapped_minimizer_fragment_image
    open_exact_minimizer_fragment_image_pg(
        const minimizer_generation_administration_identity&,
        std::uint64_t,
        const minimizer_generation_head&,
        const minimizer_generation_manifest&,
        const minimizer_generation_storage_descriptor&,
        minimizer_fragment_image_mapped_open_validation);
};

[[nodiscard]] inline mapped_minimizer_fragment_image
open_exact_minimizer_fragment_image_pg(
        const minimizer_generation_administration_identity& identity,
        const std::uint64_t query_key,
        const minimizer_generation_head& head,
        const minimizer_generation_manifest& manifest,
        const minimizer_generation_storage_descriptor& descriptor,
        const minimizer_fragment_image_mapped_open_validation validation) {
    using namespace bioseq::indexing;
    const minimizer_generation_file_identity file_identity =
        minimizer_fragment_image_pg_detail::generation_file_identity(
            identity, query_key, manifest.generation_id);
    if (manifest.layout
            != minimizer_generation_layout::
                grouped_soa_segment_local
        || head.generation_id == 0u
        || head.generation_id != manifest.generation_id
        || head.source_identity != manifest.source_identity
        || head.source_content_checksum
            != manifest.source_content_checksum
        || !exact_storage_descriptor_identity(
            descriptor, identity, query_key)
        || descriptor.generation_id != manifest.generation_id
        || descriptor.state != minimizer_generation_storage_state::live
        || descriptor.retire_full_xid
        || !has_exact_storage_descriptor_relation_shape(descriptor)) {
        minimizer_generation_storage_error_as_cpp("minimizer fragment image open binding is invalid");
    }
    const resolved_minimizer_generation_file_path resolved =
        resolve_generation_file_path(file_identity);
    auto* lease = minimizer_fragment_image_pg_detail::allocate_mapping_lease();
    ResourceOwnerEnlarge(lease->owner);
    lease->fd = ::open(
        resolved.final_path.data(),
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | PG_BINARY);
    if (lease->fd < 0) {
        const int saved_errno = errno;
        minimizer_fragment_image_pg_detail::destroy_mapping_lease(lease);
        minimizer_generation_storage_error_as_cpp(fmt::format(
            "minimizer fragment image mapped image open failed: errno={}", saved_errno));
    }
    struct stat observed{};
    const bool inode_valid = fstat(lease->fd, &observed) == 0
        && S_ISREG(observed.st_mode)
        && observed.st_uid == geteuid()
        && (observed.st_mode & 07777) == 0400
        && observed.st_nlink == 1
        && observed.st_size >= static_cast<off_t>(k_minimizer_fragment_image_header_bytes)
        && static_cast<std::uint64_t>(observed.st_size)
            <= (std::numeric_limits<std::size_t>::max)()
        && static_cast<std::uint64_t>(observed.st_size)
            == descriptor.image_bytes;
    if (!inode_valid) {
        minimizer_fragment_image_pg_detail::destroy_mapping_lease(lease);
        minimizer_generation_storage_error_as_cpp(
            "minimizer fragment image mapped image inode contract is invalid");
    }
    lease->device = observed.st_dev;
    lease->inode = observed.st_ino;
    // No PostgreSQL call is permitted between the raw fd/inode acquisition
    // above and this pre-enlarged ResourceOwner registration.
    ResourceOwnerRemember(
        lease->owner, PointerGetDatum(lease),
        &minimizer_fragment_image_pg_detail::k_mapping_lease_resource_owner_descriptor);
    lease->remembered = true;
    lease->mapping_bytes = static_cast<std::size_t>(observed.st_size);
    lease->mapping = mmap(
        nullptr, lease->mapping_bytes,
        PROT_READ, MAP_PRIVATE, lease->fd, 0);
    if (lease->mapping == MAP_FAILED) {
        const int saved_errno = errno;
        minimizer_fragment_image_pg_detail::destroy_mapping_lease(lease);
        minimizer_generation_storage_error_as_cpp(fmt::format(
            "minimizer fragment image image mmap failed: errno={}", saved_errno));
    }
    const std::span<const std::byte> image{
        static_cast<const std::byte*>(lease->mapping),
        lease->mapping_bytes};
    encoded_minimizer_fragment_image_header encoded_header{};
    std::memcpy(
        encoded_header.data(), image.data(), encoded_header.size());
    const auto decoded_header =
        decode_minimizer_fragment_image_header(encoded_header);
    if (!decoded_header
        || decoded_header->header_crc64 != descriptor.header_checksum) {
        minimizer_fragment_image_pg_detail::destroy_mapping_lease(lease);
        minimizer_generation_storage_error_as_cpp(
            "minimizer fragment image mapped header disagrees with its LIVE descriptor");
    }
    std::optional<minimizer_fragment_compact_hash32_section_spec>
        compact_h32_direct;
    if ((decoded_header->feature_bitmap
            & k_minimizer_fragment_compact_hash32_feature) != 0u) {
        compact_h32_direct = minimizer_fragment_compact_hash32_section_spec{
            .bytes = decoded_header->compact_hash32_direct.bytes,
            .direct_header_crc64 =
                decoded_header->compact_hash32_direct.direct_header_crc64,
        };
    }
    const auto layout = plan_minimizer_fragment_image_layout(
        manifest.payload_bytes,
        manifest.directory_bytes,
        minimizer_generation_manifest::encoded_bytes,
        decoded_header->payload.crc64,
        manifest.directory_checksum,
        head.manifest_checksum,
        compact_h32_direct);
    if (!layout || descriptor.image_bytes != layout->file_bytes) {
        minimizer_fragment_image_pg_detail::destroy_mapping_lease(lease);
        minimizer_generation_storage_error_as_cpp(
            "minimizer fragment image descriptor layout is invalid");
    }
    const minimizer_fragment_image_validation_options options{
        .expected_identity = minimizer_fragment_image_pg_detail::expected_identity(
            file_identity, head, manifest, *layout,
            descriptor.header_checksum),
        .payload_crc_policy = minimizer_fragment_payload_crc_policy::deferred,
    };
    const auto validated = validation
            == minimizer_fragment_image_mapped_open_validation::bounded_header_only
        ? open_minimizer_fragment_image_bounded(image, options)
        : validation == minimizer_fragment_image_mapped_open_validation::full_metadata
            ? validate_minimizer_fragment_image(image, options)
            : std::expected<validated_minimizer_fragment_image_view,
                minimizer_fragment_image_error>{std::unexpected(
                    minimizer_fragment_image_error::identity_mismatch)};
    const bool bounded_validation_exact = validated
        && validated->header_crc_bytes == k_minimizer_fragment_image_header_bytes
        && validated->payload_crc_bytes == 0u
        && (validation
                == minimizer_fragment_image_mapped_open_validation::bounded_header_only
            ? validated->directory_crc_bytes == 0u
                && validated->manifest_crc_bytes == 0u
            : validation == minimizer_fragment_image_mapped_open_validation::full_metadata
                && validated->directory_crc_bytes
                    == manifest.directory_bytes
                && validated->manifest_crc_bytes
                    == minimizer_generation_manifest::encoded_bytes);
    if (!bounded_validation_exact) {
        minimizer_fragment_image_pg_detail::destroy_mapping_lease(lease);
        minimizer_generation_storage_error_as_cpp(
            "minimizer fragment image mapped image validation failed");
    }
    mapped_minimizer_fragment_image result;
    result.lease_ = lease;
    result.payload_ = validated->payload;
    result.compact_h32_direct_ = validated->compact_hash32_direct;
    result.payload_raw_checksum_ = validated->header.payload.crc64;
    if (!validated->compact_hash32_direct.empty()) {
        result.compact_h32_direct_header_receipt_ =
            validated->header.compact_hash32_direct.direct_header_crc64;
    }
    return result;
}

/// Relation-owning sink factory target used directly by the existing common
/// two-pass generation encoder. It cannot publish the head and therefore cannot
/// make an incompletely reread generation visible.
class index_storage_fragment_generation_row_sink {
private:
    enum class write_stage : std::uint8_t {
        payload,
        directory,
        manifest,
        tokenized,
        failed,
        moved,
    };

    static constexpr std::uint64_t k_write_batch_rows = 4096;
    static constexpr std::uint64_t k_write_batch_bytes =
        MAX_CATALOG_MULTI_INSERT_BYTES;

    minimizer_generation_administration_identity identity_{};
    Relation relation_ = nullptr;
    CatalogIndexState catalog_indexes_ = nullptr;
    Relation validation_primary_index_ = nullptr;
    Snapshot validation_snapshot_ = nullptr;
    SysScanDesc validation_scan_ = nullptr;
    std::uint64_t advisory_lock_key_ = 0;
    std::uint64_t query_key_ = 0;
    std::uint64_t generation_id_ = 0;
    std::uint64_t row_count_ = 0;
    std::uint64_t write_batch_row_limit_ = k_write_batch_rows;
    std::uint64_t catalog_multi_insert_batches_ = 0;
    std::vector<TupleTableSlot*> pending_slots_;
    std::uint64_t rows_in_batch_ = 0;
    std::uint64_t bytes_in_batch_ = 0;
    std::uint64_t payload_rows_written_ = 0;
    std::uint64_t payload_bytes_written_ = 0;
    crc64_ecma182_accumulator payload_row_checksums_{};
    std::uint32_t next_directory_chunk_ = 1;
    std::uint64_t directory_bytes_written_ = 0;
    crc64_ecma182_accumulator directory_checksum_{};
    std::optional<index_chunk_address> previous_payload_address_;
    bool global_payload_order_possible_ = true;
    std::array<std::byte, minimizer_generation_manifest::encoded_bytes>
        written_manifest_bytes_{};
    std::uint32_t written_manifest_size_ = 0;
    std::optional<minimizer_generation_source_authorization> validation_source_authorization_;
    write_stage stage_ = write_stage::payload;
    bool pg_poisoned_ = false;

    [[nodiscard]] static bool checked_add_(
            std::uint64_t& value,
            std::uint64_t increment) noexcept {
        if (increment
            > (std::numeric_limits<std::uint64_t>::max)() - value) {
            return false;
        }
        value += increment;
        return true;
    }

    void drop_pending_slots_as_cpp_() {
        for (TupleTableSlot* slot : pending_slots_) {
            if (slot != nullptr) {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [slot] { ExecDropSingleTupleTableSlot(slot); });
                inject_minimizer_generation_failure_as_cpp(
                    minimizer_generation_failure_injection::generation_tts_drop,
                    "generation_tts_drop");
            }
        }
        std::vector<TupleTableSlot*>{}.swap(pending_slots_);
        rows_in_batch_ = 0;
        bytes_in_batch_ = 0;
    }

    void abandon_resources_noexcept_() noexcept {
        validation_source_authorization_.reset();
        pending_slots_.clear();
        std::vector<TupleTableSlot*>{}.swap(pending_slots_);
        rows_in_batch_ = 0;
        bytes_in_batch_ = 0;
        validation_scan_ = nullptr;
        validation_primary_index_ = nullptr;
        validation_snapshot_ = nullptr;
        catalog_indexes_ = nullptr;
        relation_ = nullptr;
    }

    void fail_cleanup_() noexcept {
        stage_ = write_stage::failed;
        pg_poisoned_ = true;
        abandon_resources_noexcept_();
        identity_.release_owned_strings();
    }

    void abandon_after_pg_error_() noexcept {
        stage_ = write_stage::failed;
        pg_poisoned_ = true;
        validation_source_authorization_.reset();
        // Do not call any PostgreSQL cleanup routine while propagating an
        // ERROR converted at a leaf. Slots, scans, snapshots, catalog state
        // and relation locks remain registered with PostgreSQL's current
        // ResourceOwner and are reclaimed by statement/transaction abort.
        pending_slots_.clear();
        std::vector<TupleTableSlot*>{}.swap(pending_slots_);
        rows_in_batch_ = 0;
        bytes_in_batch_ = 0;
        validation_scan_ = nullptr;
        validation_primary_index_ = nullptr;
        validation_snapshot_ = nullptr;
        catalog_indexes_ = nullptr;
        relation_ = nullptr;
        identity_.release_owned_strings();
    }

    void flush_bounded_batch_as_cpp_() {
        if (pending_slots_.empty()) {
            if (rows_in_batch_ != 0 || bytes_in_batch_ != 0) {
                minimizer_generation_storage_error_as_cpp(
                    "generation write batch counters disagree");
            }
            return;
        }
        if (relation_ == nullptr || catalog_indexes_ == nullptr
            || pending_slots_.size() != rows_in_batch_
            || pending_slots_.size()
                > static_cast<std::size_t>(
                    (std::numeric_limits<int>::max)())) {
            minimizer_generation_storage_error_as_cpp(
                "generation write batch state is invalid");
        }
        bioseq::postgres::support::invoke_pg_as_cpp([&] {
            CatalogTuplesMultiInsertWithInfo(
                relation_, pending_slots_.data(),
                static_cast<int>(pending_slots_.size()), catalog_indexes_);
        });
        inject_minimizer_generation_failure_as_cpp(
            minimizer_generation_failure_injection::generation_multi_insert,
            "generation_multi_insert");
        if (!checked_add_(catalog_multi_insert_batches_, 1)) {
            minimizer_generation_storage_error_as_cpp(
                "generation write batch counter overflow");
        }
        drop_pending_slots_as_cpp_();
    }

    [[nodiscard]] std::optional<minimizer_generation_encoding_error>
    validate_next_row_(const minimizer_generation_row_view& row) noexcept {
        if (stage_ == write_stage::manifest
            || stage_ == write_stage::tokenized
            || stage_ == write_stage::failed
            || stage_ == write_stage::moved) {
            return minimizer_generation_encoding_error::row_order;
        }
        const bool manifest = row.address.position_segment == -1
            && row.address.sub_key == k_directory_sub_key
            && row.address.chunk_no == 0;
        const bool directory = row.address.position_segment == -1
            && row.address.sub_key == k_directory_sub_key
            && row.address.chunk_no > 0;
        if (manifest) {
            if (row.address.chunk_no != 0
                || (row.bytes.size()
                        != minimizer_generation_manifest::encoded_bytes
                    && row.bytes.size()
                        != minimizer_generation_manifest::
                            encoded_bytes)) {
                return minimizer_generation_encoding_error::row_order;
            }
            return std::nullopt;
        }
        if (directory) {
            if (stage_ != write_stage::payload
                && stage_ != write_stage::directory) {
                return minimizer_generation_encoding_error::row_order;
            }
            if (row.address.chunk_no != next_directory_chunk_) {
                return minimizer_generation_encoding_error::chunk_range;
            }
            return std::nullopt;
        }
        if (stage_ != write_stage::payload
            || row.address.position_segment < 0
            || row.address.sub_key < 0
            || row.address.sub_key >= k_temporary_run_sub_key_base) {
            return minimizer_generation_encoding_error::row_order;
        }
        const bool position_extent =
            row.address.sub_key >= k_position_extent_sub_key_base;
        const index_sub_key_t logical_sub_key = position_extent
            ? row.address.sub_key - k_position_extent_sub_key_base
            : row.address.sub_key;
        if (logical_sub_key < 0
            || logical_sub_key >= k_position_extent_sub_key_base) {
            return minimizer_generation_encoding_error::sub_key;
        }
        if (previous_payload_address_) {
            const auto previous_global = std::tuple{
                previous_payload_address_->sub_key,
                previous_payload_address_->position_segment,
                previous_payload_address_->chunk_no};
            const auto current_global = std::tuple{
                row.address.sub_key,
                row.address.position_segment,
                row.address.chunk_no};
            global_payload_order_possible_ =
                global_payload_order_possible_
                && previous_global < current_global;
            if (!global_payload_order_possible_) {
                return minimizer_generation_encoding_error::row_order;
            }
        }
        return std::nullopt;
    }

    void record_written_row_(
            const minimizer_generation_row_view& row,
            std::uint64_t admitted_batch_bytes) {
        const bool manifest = row.address.position_segment == -1
            && row.address.sub_key == k_directory_sub_key
            && row.address.chunk_no == 0;
        const bool directory = row.address.position_segment == -1
            && row.address.sub_key == k_directory_sub_key
            && row.address.chunk_no > 0;
        if (manifest) {
            written_manifest_bytes_.fill(std::byte{0});
            std::copy(row.bytes.begin(), row.bytes.end(),
                written_manifest_bytes_.begin());
            written_manifest_size_ = static_cast<std::uint32_t>(
                row.bytes.size());
            stage_ = write_stage::manifest;
        } else if (directory) {
            stage_ = write_stage::directory;
            ++next_directory_chunk_;
            directory_checksum_.update(row.bytes);
            if (!checked_add_(directory_bytes_written_, row.bytes.size())) {
                minimizer_generation_storage_error_as_cpp(
                    "directory byte counter overflow");
            }
        } else {
            previous_payload_address_ = row.address;
            payload_row_checksums_.update_little_endian(row.row_checksum);
            if (!checked_add_(payload_rows_written_, 1)
                || !checked_add_(payload_bytes_written_, row.bytes.size())) {
                minimizer_generation_storage_error_as_cpp(
                    "payload write counter overflow");
            }
        }
        if (!checked_add_(row_count_, 1)
            || !checked_add_(rows_in_batch_, 1)
            || !checked_add_(bytes_in_batch_, admitted_batch_bytes)) {
            minimizer_generation_storage_error_as_cpp(
                "generation write counter overflow");
        }
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    write_validated_row_(
            const validated_minimizer_generation_row& validated_row) {
        const minimizer_generation_row_view& row = validated_row.row();
        try {
            auto tuple = form_minimizer_generation_row_pg(
                relation_, query_key_, validated_row);
            if (!tuple) {
                fail_cleanup_();
                return std::unexpected(tuple.error());
            }
            const std::uint64_t actual_batch_bytes =
                static_cast<std::uint64_t>((*tuple)->t_len)
                + sizeof(TupleTableSlot*) + sizeof(HeapTupleData);
            const auto batch_decision =
                bioseq::indexing::plan_index_tuple_batch_append(
                    pending_slots_.size(), bytes_in_batch_,
                    actual_batch_bytes,
                    write_batch_row_limit_, k_write_batch_bytes);
            if (!batch_decision.valid) {
                heap_freetuple(*tuple);
                minimizer_generation_storage_error_as_cpp(
                    "generation write row exceeds bounded catalog batch");
            }
            if (batch_decision.flush_before) {
                flush_bounded_batch_as_cpp_();
            }

            TupleTableSlot* slot = bioseq::postgres::support::invoke_pg_as_cpp([&] {
                return MakeSingleTupleTableSlot(
                    RelationGetDescr(relation_), &TTSOpsHeapTuple);
            });
            inject_minimizer_generation_failure_as_cpp(
                minimizer_generation_failure_injection::generation_tts_create,
                "generation_tts_create");
            try {
                pending_slots_.push_back(slot);
            } catch (...) {
                heap_freetuple(*tuple);
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [&] { ExecDropSingleTupleTableSlot(slot); });
                throw;
            }
            bioseq::postgres::support::invoke_pg_as_cpp(
                [&] { ExecStoreHeapTuple(*tuple, slot, true); });
            inject_minimizer_generation_failure_as_cpp(
                minimizer_generation_failure_injection::generation_tts_store,
                "generation_tts_store");
            record_written_row_(row, actual_batch_bytes);
            if (batch_decision.flush_after) {
                flush_bounded_batch_as_cpp_();
            }
        } catch (const bioseq::postgres::support::postgres_error&) {
            abandon_after_pg_error_();
            throw;
        }
        return {};
    }

public:
    index_storage_fragment_generation_row_sink() = delete;
    index_storage_fragment_generation_row_sink(
            minimizer_generation_administration_identity identity,
            std::uint64_t query_key,
            std::uint64_t generation_id,
            std::uint64_t write_batch_row_limit = k_write_batch_rows)
        : identity_(std::move(identity)),
          query_key_(query_key), generation_id_(generation_id),
          write_batch_row_limit_(write_batch_row_limit) {
        try {
            if (!bioseq::indexing::sql_bigint_can_represent(query_key)
                || generation_id == 0
                || !bioseq::indexing::sql_bigint_can_represent(generation_id)
                || identity_.database_oid != MyDatabaseId
                || !OidIsValid(identity_.user_relid)
                || identity_.column_attno <= 0
                || identity_.index_name.empty()
                || identity_.table_name.empty()
                || write_batch_row_limit_ == 0
                || write_batch_row_limit_ > k_write_batch_rows) {
                minimizer_generation_storage_error_as_cpp(
                    "generation sink identity/key is invalid");
            }
            advisory_lock_key_ = index_advisory_xact_lock_key(
                identity_.database_oid,
                identity_.tablespace_oid,
                identity_.main_relfilenode,
                identity_.column_attno,
                identity_.index_name);
            bioseq::postgres::support::invoke_pg_as_cpp([&] {
                acquire_index_advisory_xact_lock(
                    identity_.database_oid,
                    identity_.tablespace_oid,
                    identity_.main_relfilenode,
                    identity_.column_attno,
                    identity_.index_name);
            });
            const Oid relation_oid = bioseq::postgres::support::invoke_pg_as_cpp([&] {
                return get_relname_relid(
                    identity_.table_name.c_str(), bioseq_internal_ns());
            });
            relation_ = OidIsValid(relation_oid)
                ? bioseq::postgres::support::invoke_pg_as_cpp([&] {
                    return bioseq::postgres::support::try_open_postgres_relation(
                        relation_oid, RowExclusiveLock);
                })
                : nullptr;
            if (relation_ == nullptr
                || RelationGetNamespace(relation_) != bioseq_internal_ns()
                || std::string_view{RelationGetRelationName(relation_)}
                    != identity_.table_name) {
                minimizer_generation_storage_error_as_cpp(
                    "generation relation is missing or renamed");
            }
            const minimizer_generation_schema_inspection schema =
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [&] { return inspect_minimizer_generation_schema(
                        relation_, nullptr,
                        minimizer_generation_schema_expectation{
                            .namespace_oid = bioseq_internal_ns(),
                            .relation_name = identity_.table_name,
                            .tablespace_oid = identity_.tablespace_oid,
                        }); });
            if (!schema) {
                minimizer_generation_storage_error_as_cpp(fmt::format(
                    "generation relation schema mismatch: {}",
                    minimizer_generation_schema_error_name(schema.error)));
            }
            (void)verify_pg_inline_payload_layout(relation_);
            catalog_indexes_ = bioseq::postgres::support::invoke_pg_as_cpp(
                [&] { return CatalogOpenIndexes(relation_); });
            if (catalog_indexes_ == nullptr) {
                minimizer_generation_storage_error_as_cpp(
                    "CatalogOpenIndexes returned NULL");
            }
        } catch (const bioseq::postgres::support::postgres_error&) {
            abandon_after_pg_error_();
            throw;
        }
    }
    index_storage_fragment_generation_row_sink(
        const index_storage_fragment_generation_row_sink&) = delete;
    index_storage_fragment_generation_row_sink& operator=(
        const index_storage_fragment_generation_row_sink&) = delete;
    index_storage_fragment_generation_row_sink(
            index_storage_fragment_generation_row_sink&& other) noexcept
        : identity_(std::move(other.identity_)),
          relation_(std::exchange(other.relation_, nullptr)),
          catalog_indexes_(std::exchange(other.catalog_indexes_, nullptr)),
          validation_primary_index_(std::exchange(
              other.validation_primary_index_, nullptr)),
          validation_snapshot_(std::exchange(
              other.validation_snapshot_, nullptr)),
          validation_scan_(std::exchange(other.validation_scan_, nullptr)),
          advisory_lock_key_(other.advisory_lock_key_),
          query_key_(other.query_key_),
          generation_id_(other.generation_id_),
          row_count_(other.row_count_),
          write_batch_row_limit_(other.write_batch_row_limit_),
          catalog_multi_insert_batches_(
              other.catalog_multi_insert_batches_),
          pending_slots_(std::move(other.pending_slots_)),
          rows_in_batch_(other.rows_in_batch_),
          bytes_in_batch_(other.bytes_in_batch_),
          payload_rows_written_(other.payload_rows_written_),
          payload_bytes_written_(other.payload_bytes_written_),
          payload_row_checksums_(other.payload_row_checksums_),
          next_directory_chunk_(other.next_directory_chunk_),
          directory_bytes_written_(other.directory_bytes_written_),
          directory_checksum_(other.directory_checksum_),
          previous_payload_address_(other.previous_payload_address_),
          global_payload_order_possible_(
              other.global_payload_order_possible_),
          written_manifest_bytes_(other.written_manifest_bytes_),
          written_manifest_size_(other.written_manifest_size_),
          validation_source_authorization_(std::move(
              other.validation_source_authorization_)),
          stage_(other.stage_),
          pg_poisoned_(other.pg_poisoned_) {
        other.stage_ = write_stage::moved;
        other.pg_poisoned_ = true;
    }
    index_storage_fragment_generation_row_sink& operator=(
        index_storage_fragment_generation_row_sink&&) = delete;
    ~index_storage_fragment_generation_row_sink() {
        abandon_resources_noexcept_();
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    write(const minimizer_generation_row_view& row) {
        if (pg_poisoned_) {
            minimizer_generation_storage_error_as_cpp(
                "poisoned generation row sink reused after PostgreSQL error");
        }
        if (auto order_error = validate_next_row_(row)) {
            fail_cleanup_();
            return std::unexpected(*order_error);
        }
        auto validated = validate_minimizer_generation_row_for_write(
            generation_id_, row);
        if (!validated) {
            fail_cleanup_();
            return std::unexpected(validated.error());
        }
        return write_validated_row_(*validated);
    }

    /// grouped-SoA payload owner: compute the address-bound CRC once, bind it
    /// into an unforgeable validated token, form the tuple from that token, and
    /// return the exact checksum for manifest folding.
    [[nodiscard]] std::expected<minimizer_generation_row_write_receipt,
        minimizer_generation_encoding_error>
    write_grouped_soa_payload(
            const unchecked_minimizer_generation_row_view& row) {
        if (pg_poisoned_) {
            minimizer_generation_storage_error_as_cpp(
                "poisoned generation row sink reused after PostgreSQL error");
        }
        const minimizer_generation_row_view order_view{
            .address = row.address,
            .bytes = row.bytes,
            .row_checksum = 0,
        };
        if (auto order_error = validate_next_row_(order_view)) {
            fail_cleanup_();
            return std::unexpected(*order_error);
        }
        auto validated = minimizer_generation_row_validator::validate_and_checksum(
            generation_id_, row);
        if (!validated) {
            fail_cleanup_();
            return std::unexpected(validated.error());
        }
        const std::uint64_t checksum = validated->row().row_checksum;
        if (auto written = write_validated_row_(*validated); !written) {
            return std::unexpected(written.error());
        }
        return minimizer_generation_row_write_receipt{
            .row_checksum = checksum,
        };
    }

    [[nodiscard]] std::uint64_t row_count() const noexcept {
        return row_count_;
    }
    [[nodiscard]] std::uint64_t catalog_multi_insert_batches() const noexcept {
        return catalog_multi_insert_batches_;
    }
    [[nodiscard]] ready_minimizer_generation_token validate_ready_generation(
            const encoded_grouped_soa_generation& generation) {
        const auto minimizer_generation_storage_error = [](std::string_view message) {
            minimizer_generation_storage_error_as_cpp(message);
        };
        Oid validated_relation_oid = InvalidOid;
        RelFileLocator validated_locator{};
        std::array<std::byte, minimizer_generation_head::encoded_bytes>
            validated_head_bytes{};
        std::uint64_t validated_head_checksum = 0;
        std::uint64_t validated_source_identity = 0;
        std::uint64_t validated_source_content_checksum = 0;
        std::uint64_t validated_storage_contract_checksum = 0;
        std::optional<minimizer_fragment_image_file_receipt> validated_file_receipt{};

        try {
            if (stage_ != write_stage::manifest
                || relation_ == nullptr || catalog_indexes_ == nullptr
                || generation.head.generation_id != generation_id_
                || generation.manifest.generation_id != generation_id_) {
                minimizer_generation_storage_error("READY validation identity/stage disagrees");
            }
            if (auto metadata =
                    bioseq::indexing::validate_minimizer_generation_metadata(
                        generation.head,
                        generation.manifest,
                        generation.directory);
                !metadata) {
                minimizer_generation_storage_error("READY validation metadata disagrees");
            }
            const bool mapped_minimizer_fragment_image =
                identity_.index_name == k_minimizers_per_fragment_index_name;
            std::unique_ptr<minimizer_fragment_image_writer>
                fragment_image_writer;
			/* The sidecar path/header intentionally encode the minimizer-fragment
			 * source identity, not a logical-index discriminator. The minimizer-sequence
			 * grouped-SoA route keeps the authoritative PostgreSQL payload and therefore
			 * must not create or overwrite this minimizer-fragment-only image. */
            if (mapped_minimizer_fragment_image) {
                fragment_image_writer =
                    std::make_unique<minimizer_fragment_image_writer>(
                        identity_, query_key_, generation_id_);
            }
            if (!global_payload_order_possible_) {
                minimizer_generation_storage_error(
                    "sink payload order disagrees with grouped-SoA storage");
            }
            const std::uint32_t extent_payload_bytes =
                bioseq::indexing::minimizer_extent_payload_bytes(
                    generation.manifest.layout);
            if (extent_payload_bytes == 0) {
                minimizer_generation_storage_error("READY validation wire quantum is invalid");
            }
            const minimizer_generation_row_view head_row{
                .address = generation.unpublished_head_row.address,
                .bytes = generation.unpublished_head_row.bytes(),
                .row_checksum = generation.unpublished_head_row.row_checksum,
            };
            if (head_row.address != index_chunk_address{
                    .generation_id = 0,
                    .position_segment = -1,
                    .sub_key = k_generation_head_sub_key,
                    .chunk_no = 0,
                }
                || head_row.bytes.size() != validated_head_bytes.size()
                || head_row.row_checksum
                    != bioseq::indexing::crc64_index_chunk_row(
                        0, -1, k_generation_head_sub_key, 0,
                        static_cast<std::uint32_t>(head_row.bytes.size()),
                        head_row.bytes)) {
                minimizer_generation_storage_error("READY validation head row is malformed");
            }
            std::copy(head_row.bytes.begin(), head_row.bytes.end(),
                validated_head_bytes.begin());
            validated_head_checksum = head_row.row_checksum;

            std::uint64_t expected_payload_rows = 0;
            std::uint64_t expected_payload_bytes = 0;
            std::optional<std::pair<index_sub_key_t, std::int32_t>>
                previous_storage_key;
            for (const grouped_soa_bucket_directory_entry& entry
                 : generation.directory.entries()) {
                const std::pair storage_key{
                    entry.sub_key, entry.position_segment};
                if (previous_storage_key
                    && !(*previous_storage_key < storage_key)) {
                    minimizer_generation_storage_error(
                        "directory is not in physical PK storage order");
                }
                previous_storage_key = storage_key;
                if (!checked_add_(expected_payload_rows,
                        static_cast<std::uint64_t>(entry.control_chunk_count)
                            + entry.position_chunk_count)
                    || !checked_add_(expected_payload_bytes,
                        entry.control_encoded_bytes)
                    || !checked_add_(expected_payload_bytes,
                        entry.position_encoded_bytes)) {
                    minimizer_generation_storage_error("READY expected payload counters overflow");
                }
            }
            std::uint64_t expected_total_rows = expected_payload_rows;
            if (!checked_add_(expected_total_rows,
                    generation.manifest.directory_chunk_count)
                || !checked_add_(expected_total_rows, 1)) {
                minimizer_generation_storage_error("READY expected row counter overflow");
            }
            if (row_count_ != expected_total_rows
                || payload_rows_written_ != expected_payload_rows
                || payload_bytes_written_ != expected_payload_bytes
                || payload_row_checksums_.checksum()
                    != generation.manifest.payload_checksum
                || next_directory_chunk_
                    != generation.manifest.directory_chunk_count + 1u
                || directory_bytes_written_
                    != generation.manifest.directory_bytes
                || directory_checksum_.checksum()
                    != generation.manifest.directory_checksum
                || written_manifest_size_
                    != generation.manifest_row.bytes().size()
                || !std::ranges::equal(
                    std::span<const std::byte>{written_manifest_bytes_}
                        .first(written_manifest_size_),
                    generation.manifest_row.bytes())) {
                minimizer_generation_storage_error("sink write order/count/checksum disagrees");
            }

            flush_bounded_batch_as_cpp_();
            bioseq::postgres::support::invoke_pg_as_cpp(
                [&] { CatalogCloseIndexes(catalog_indexes_); });
            catalog_indexes_ = nullptr;
            bioseq::postgres::support::invoke_pg_as_cpp(
                [] { CommandCounterIncrement(); });
            if (!bioseq::postgres::support::invoke_pg_as_cpp(
                    [] { return ActiveSnapshotSet(); })) {
                minimizer_generation_storage_error(
                    "READY reread requires an active MVCC snapshot");
            }
            Snapshot active_snapshot = bioseq::postgres::support::invoke_pg_as_cpp(
                [] { return GetActiveSnapshot(); });
            if (!IsMVCCSnapshot(active_snapshot)) {
                minimizer_generation_storage_error(
                    "READY reread active snapshot is not MVCC");
            }
            bioseq::postgres::support::invoke_pg_as_cpp(
                [] { UpdateActiveSnapshotCommandId(); });
            if (active_snapshot->curcid
                != bioseq::postgres::support::invoke_pg_as_cpp(
                    [] { return GetCurrentCommandId(false); })) {
                minimizer_generation_storage_error(
                    "READY reread active command ID was not refreshed");
            }
            validation_snapshot_ = bioseq::postgres::support::invoke_pg_as_cpp(
                [&] { return RegisterSnapshot(active_snapshot); });
            if (validation_snapshot_ == nullptr
                || validation_snapshot_->regd_count == 0) {
                minimizer_generation_storage_error("READY reread snapshot was not registered");
            }

            validation_source_authorization_.emplace(
                authorize_minimizer_generation_source_pg(
                    identity_, query_key_, validation_snapshot_));
            minimizer_generation_source_authorization& source_authorization =
                *validation_source_authorization_;
            if (!source_authorization.valid_
                || source_authorization.snapshot_ != validation_snapshot_
                || source_authorization.database_oid_ != identity_.database_oid
                || source_authorization.tablespace_oid_ != identity_.tablespace_oid
                || source_authorization.main_relfilenode_
                    != identity_.main_relfilenode
                || source_authorization.user_relid_ != identity_.user_relid
                || source_authorization.column_attno_ != identity_.column_attno
                || source_authorization.index_name_ != identity_.index_name
                || source_authorization.table_name_ != identity_.table_name
                || source_authorization.source_.sequence_rowno != query_key_) {
                minimizer_generation_storage_error(
                    "READY authoritative source authorization binding disagrees");
            }
            const std::uint64_t storage_contract_checksum =
                bioseq::indexing::compute_minimizer_generation_storage_contract_checksum(
                    generation.manifest);
            if (generation.manifest.alphabet_code
                    != source_authorization.source_.alphabet_code
                || generation.manifest.storage_unit_byte_width
                    != source_authorization.source_.storage_unit_byte_width
                || generation.manifest.target_length
                    != source_authorization.source_.locator.logical_length
                || generation.head.storage_contract_checksum
                    != storage_contract_checksum
                || generation.head.source_content_checksum
                    != source_authorization.source_content_checksum_
                || generation.manifest.source_content_checksum
                    != source_authorization.source_content_checksum_) {
                minimizer_generation_storage_error(
                    "READY authoritative source content/storage contract disagrees");
            }
            source_authorization.source_.storage_contract_checksum =
                storage_contract_checksum;
            const std::uint64_t required_source_identity =
                minimizer_generation_source_identity(
                    identity_.main_relfilenode, source_authorization.source_);
            if (required_source_identity == 0
                || generation.head.source_identity
                    != required_source_identity
                || generation.manifest.source_identity
                    != required_source_identity) {
                minimizer_generation_storage_error(
                    "READY authoritative source identity disagrees");
            }
            validated_source_identity = required_source_identity;
            validated_source_content_checksum =
                source_authorization.source_content_checksum_;
            validated_storage_contract_checksum = storage_contract_checksum;
            source_authorization.invalidate_();
            validation_source_authorization_.reset();

            const minimizer_generation_schema_inspection schema =
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [&] { return inspect_minimizer_generation_schema(
                        relation_, nullptr,
                        minimizer_generation_schema_expectation{
                            .namespace_oid = bioseq_internal_ns(),
                            .relation_name = identity_.table_name,
                            .tablespace_oid = identity_.tablespace_oid,
                        }); });
            if (!schema) {
                minimizer_generation_storage_error("READY relation schema changed");
            }
            validated_relation_oid = RelationGetRelid(relation_);
            validated_locator = relation_->rd_locator;
            validation_primary_index_ = bioseq::postgres::support::invoke_pg_as_cpp(
                [&] {
                    return index_open(
                        schema.primary_index_oid, AccessShareLock);
                });

            ScanKeyData metadata_keys[4];
            ScanKeyInit(&metadata_keys[0], k_minimizer_generation_index_generation_id,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(generation_id_)));
            ScanKeyInit(&metadata_keys[1], k_minimizer_generation_index_query_key,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(query_key_)));
            ScanKeyInit(&metadata_keys[2], k_minimizer_generation_index_sub_key,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(k_directory_sub_key)));
            ScanKeyInit(&metadata_keys[3], k_minimizer_generation_index_position_segment,
                BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(-1));
            validation_scan_ = bioseq::postgres::support::invoke_pg_as_cpp([&] {
                return systable_beginscan_ordered(
                    relation_, validation_primary_index_,
                    validation_snapshot_, 4, metadata_keys);
            });
            const auto next_validation_tuple = [&] {
                return bioseq::postgres::support::invoke_pg_as_cpp([&] {
                    return systable_getnext_ordered(
                        validation_scan_, ForwardScanDirection);
                });
            };
            HeapTuple tuple = next_validation_tuple();
            if (!HeapTupleIsValid(tuple)) {
                minimizer_generation_storage_error("READY manifest row is missing");
            }
            minimizer_generation_loaded_row manifest_row =
                load_minimizer_generation_row_or_error(relation_, tuple);
            inject_minimizer_generation_failure_as_cpp(
                minimizer_generation_failure_injection::generation_reread,
                "generation_reread");
            if (manifest_row.query_key != query_key_
                || manifest_row.address != generation.manifest_row.address
                || manifest_row.checksum
                    != generation.manifest_row.row_checksum
                || !std::ranges::equal(
                    manifest_row.bytes, generation.manifest_row.bytes())) {
                manifest_row.release();
                minimizer_generation_storage_error("READY manifest reread disagrees");
            }
            manifest_row.release();

            crc64_ecma182_accumulator reread_directory_checksum;
            std::uint64_t reread_directory_bytes = 0;
            const std::span<const std::byte> expected_directory =
                generation.directory.encoded_bytes();
            for (std::uint32_t chunk_no = 1;
                 chunk_no <= generation.manifest.directory_chunk_count;
                 ++chunk_no) {
                tuple = next_validation_tuple();
                if (!HeapTupleIsValid(tuple)) {
                    minimizer_generation_storage_error("READY directory chunk is missing");
                }
                minimizer_generation_loaded_row directory_row =
                    load_minimizer_generation_row_or_error(relation_, tuple);
                const std::uint64_t offset64 =
                    static_cast<std::uint64_t>(chunk_no - 1u)
                    * extent_payload_bytes;
                const std::size_t offset = static_cast<std::size_t>(offset64);
                const std::size_t count = std::min<std::size_t>(
                    extent_payload_bytes,
                    expected_directory.size() - offset);
                const auto expected_bytes = expected_directory.subspan(
                    offset, count);
                if (directory_row.query_key != query_key_
                    || directory_row.address != index_chunk_address{
                        .generation_id = generation_id_,
                        .position_segment = -1,
                        .sub_key = k_directory_sub_key,
                        .chunk_no = chunk_no,
                    }
                    || !std::ranges::equal(
                        directory_row.bytes, expected_bytes)) {
                    directory_row.release();
                    minimizer_generation_storage_error("READY directory reread disagrees");
                }
                reread_directory_checksum.update(directory_row.bytes);
                if (!checked_add_(reread_directory_bytes,
                        directory_row.bytes.size())) {
                    directory_row.release();
                    minimizer_generation_storage_error("READY directory reread bytes overflow");
                }
                directory_row.release();
            }
            if (HeapTupleIsValid(next_validation_tuple())) {
                minimizer_generation_storage_error("READY directory reread has extra rows");
            }
            bioseq::postgres::support::invoke_pg_as_cpp(
                [&] { systable_endscan_ordered(validation_scan_); });
            validation_scan_ = nullptr;
            if (reread_directory_bytes
                    != generation.manifest.directory_bytes
                || reread_directory_checksum.checksum()
                    != generation.manifest.directory_checksum) {
                minimizer_generation_storage_error(
                    "READY directory aggregate reread disagrees");
            }

            ScanKeyData payload_keys[3];
            ScanKeyInit(&payload_keys[0], k_minimizer_generation_index_generation_id,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(generation_id_)));
            ScanKeyInit(&payload_keys[1], k_minimizer_generation_index_query_key,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(query_key_)));
            ScanKeyInit(&payload_keys[2], k_minimizer_generation_index_position_segment,
                BTGreaterEqualStrategyNumber, F_INT4GE, Int32GetDatum(0));
            validation_scan_ = bioseq::postgres::support::invoke_pg_as_cpp([&] {
                return systable_beginscan_ordered(
                    relation_, validation_primary_index_,
                    validation_snapshot_, 3, payload_keys);
            });
            const auto directory_entries = generation.directory.entries();
            std::size_t entry_ordinal = 0;
            std::uint32_t entry_chunk = 0;
            bool position_extent = false;
            bool payload_complete = directory_entries.empty();
            std::uint64_t reread_payload_rows = 0;
            std::uint64_t reread_payload_bytes = 0;
            crc64_ecma182_accumulator extent_checksum;
            for (tuple = next_validation_tuple();
                 HeapTupleIsValid(tuple);
                 tuple = next_validation_tuple()) {
                if (payload_complete
                    || entry_ordinal >= directory_entries.size()) {
                    minimizer_generation_storage_error("READY payload reread has extra rows");
                }
                const grouped_soa_bucket_directory_entry& entry =
                    directory_entries[entry_ordinal];
                const auto position_sub_key =
                    derive_minimizer_position_extent_sub_key(entry.sub_key);
                if (!position_sub_key) {
                    minimizer_generation_storage_error(
                        "READY directory position sub-key is invalid");
                }
                const std::uint32_t extent_chunk_count = position_extent
                    ? entry.position_chunk_count : entry.control_chunk_count;
                const std::uint64_t extent_bytes = position_extent
                    ? entry.position_encoded_bytes : entry.control_encoded_bytes;
                const std::uint64_t offset =
                    static_cast<std::uint64_t>(entry_chunk)
                    * extent_payload_bytes;
                const std::uint64_t expected_bytes =
                    std::min<std::uint64_t>(
                        extent_payload_bytes,
                        extent_bytes - offset);
                minimizer_generation_loaded_row payload_row =
                    load_minimizer_generation_row_or_error(relation_, tuple);
                if (payload_row.query_key != query_key_
                    || payload_row.address != index_chunk_address{
                        .generation_id = generation_id_,
                        .position_segment = entry.position_segment,
                        .sub_key = position_extent
                            ? *position_sub_key : entry.sub_key,
                        .chunk_no = entry_chunk,
                    }
                    || payload_row.bytes.size() != expected_bytes) {
                    payload_row.release();
                    minimizer_generation_storage_error("READY payload row/address/size disagrees");
                }
                extent_checksum.update(payload_row.bytes);
                if (!checked_add_(reread_payload_rows, 1)
                    || !checked_add_(reread_payload_bytes,
                        payload_row.bytes.size())) {
                    payload_row.release();
                    minimizer_generation_storage_error("READY payload reread counters overflow");
                }
                if (fragment_image_writer) {
                    fragment_image_writer->append(payload_row.bytes);
                }
                payload_row.release();
                ++entry_chunk;
                if (entry_chunk == extent_chunk_count) {
                    const std::uint64_t expected_extent_checksum =
                        position_extent
                        ? entry.position_checksum : entry.control_checksum;
                    if (extent_checksum.checksum()
                        != expected_extent_checksum) {
                        minimizer_generation_storage_error(position_extent
                            ? "READY position extent checksum disagrees"
                            : "READY control extent checksum disagrees");
                    }
                    extent_checksum = {};
                    entry_chunk = 0;
                    ++entry_ordinal;
                    if (entry_ordinal == directory_entries.size()) {
                        if (!position_extent) {
                            position_extent = true;
                            entry_ordinal = 0;
                        } else {
                            payload_complete = true;
                        }
                    }
                }
            }
            bioseq::postgres::support::invoke_pg_as_cpp(
                [&] { systable_endscan_ordered(validation_scan_); });
            validation_scan_ = nullptr;
            // The S1 PK is the physical authority: every global control
            // sub-key precedes every derived position sub-key. Wire payload
            // checksums use emitter order, so the sink validates that aggregate
            // before this physical reread.
            if (!payload_complete
                || entry_chunk != 0
                || reread_payload_rows != expected_payload_rows
                || reread_payload_bytes != expected_payload_bytes
                || reread_payload_bytes != generation.manifest.payload_bytes) {
                minimizer_generation_storage_error(
                    "READY complete payload aggregate reread disagrees");
            }
            if (fragment_image_writer) {
                validated_file_receipt = fragment_image_writer->publish(
                    generation.head,
                    generation.manifest,
                    generation.directory,
                    generation.manifest_row.bytes());
            }

            bioseq::postgres::support::invoke_pg_as_cpp(
                [&] { index_close(validation_primary_index_, AccessShareLock); });
            validation_primary_index_ = nullptr;
            bioseq::postgres::support::invoke_pg_as_cpp(
                [&] { UnregisterSnapshot(validation_snapshot_); });
            validation_snapshot_ = nullptr;
            bioseq::postgres::support::invoke_pg_as_cpp(
                [&] { relation_close(relation_, RowExclusiveLock); });
            relation_ = nullptr;
            inject_minimizer_generation_failure_as_cpp(
                minimizer_generation_failure_injection::generation_writer_close,
                "generation_writer_close");
        } catch (const bioseq::postgres::support::postgres_error&) {
            abandon_after_pg_error_();
            throw;
        }

        stage_ = write_stage::tokenized;
        const TransactionId validated_transaction_id =
            bioseq::postgres::support::invoke_pg_as_cpp(
                [] { return GetCurrentTransactionId(); });
        minimizer_generation_administration_identity validated_identity =
            std::move(identity_);
        return ready_minimizer_generation_token{
            std::move(validated_identity),
            validated_relation_oid,
            validated_locator,
            advisory_lock_key_,
            query_key_,
            generation_id_,
            validated_source_identity,
            validated_source_content_checksum,
            validated_storage_contract_checksum,
            validated_transaction_id,
            validated_head_bytes,
            validated_head_checksum,
            std::move(validated_file_receipt)};
    }
};

static_assert(bioseq::indexing::minimizer_generation_row_sink<
    index_storage_fragment_generation_row_sink>);
static_assert(bioseq::indexing::grouped_soa_minimizer_payload_sink<
    index_storage_fragment_generation_row_sink>);
static_assert(!std::is_default_constructible_v<
    index_storage_fragment_generation_row_sink>);
static_assert(!std::is_copy_constructible_v<
    index_storage_fragment_generation_row_sink>);
static_assert(std::is_nothrow_move_constructible_v<
    index_storage_fragment_generation_row_sink>);
static_assert(!std::is_move_assignable_v<
    index_storage_fragment_generation_row_sink>);

[[nodiscard]] inline std::optional<std::array<
    std::byte, minimizer_generation_head::encoded_bytes>>
read_locked_generation_head_for_descriptor_publication_pg(
        const std::string_view table_name,
        const std::uint64_t query_key) {
    std::array<std::byte, minimizer_generation_head::encoded_bytes> bytes{};
    std::array<Oid, 5u> types{
        INT8OID, INT8OID, INT8OID, INT4OID, INT4OID};
    std::array<Datum, 5u> arguments{
        Int64GetDatum(static_cast<std::int64_t>(query_key)),
        Int64GetDatum(0),
        Int64GetDatum(static_cast<std::int64_t>(k_generation_head_sub_key)),
        Int32GetDatum(-1),
        Int32GetDatum(0),
    };
    const std::string owned_table_name{table_name};
    const char* quoted_table = bioseq::postgres::support::invoke_pg_as_cpp([&] {
        return quote_identifier(owned_table_name.c_str());
    });
    const std::string sql = fmt::format(
        "SELECT blob FROM bioseq_internal.{} "
        "WHERE query_key=$1 AND generation_id=$2 AND sub_key=$3 "
        "AND position_segment=$4 AND chunk_no=$5 FOR UPDATE",
        quoted_table);
    struct operation_state {
        int connect = 0;
        int execute = 0;
        int finish = 0;
        std::uint64_t rows = 0u;
        bool table = false;
        bool is_null = true;
        bool exact_size = false;
    } state{};
    bioseq::postgres::support::invoke_pg_as_cpp(
        [&state, &sql, &types, &arguments, &bytes] noexcept {
            state.connect = SPI_connect();
            if (state.connect != SPI_OK_CONNECT) return;
            state.execute = SPI_execute_with_args(
                sql.c_str(), static_cast<int>(types.size()), types.data(),
                arguments.data(), nullptr, false, 1);
            state.rows = SPI_processed;
            state.table = SPI_tuptable != nullptr;
            if (state.execute == SPI_OK_SELECT && state.rows == 1u
                && state.table) {
                const Datum datum = SPI_getbinval(
                    SPI_tuptable->vals[0], SPI_tuptable->tupdesc,
                    1, &state.is_null);
                if (!state.is_null) {
                    auto* detoasted = reinterpret_cast<bytea*>(
                        PG_DETOAST_DATUM_PACKED(datum));
                    const std::size_t size = VARSIZE_ANY_EXHDR(detoasted);
                    if (size == bytes.size()) {
                        std::memcpy(
                            bytes.data(), VARDATA_ANY(detoasted), size);
                        state.exact_size = true;
                    }
                    if (static_cast<void*>(detoasted)
                        != DatumGetPointer(datum)) {
                        pfree(detoasted);
                    }
                }
            }
            state.finish = SPI_finish();
        });
    if (state.connect != SPI_OK_CONNECT
        || state.execute != SPI_OK_SELECT || state.rows > 1u
        || (state.rows == 1u
            && (!state.table || state.is_null || !state.exact_size))
        || state.finish != SPI_OK_FINISH) {
        minimizer_generation_storage_error_as_cpp(fmt::format(
            "descriptor publication head lock disagrees: connect={} "
            "execute={} rows={} table={} null={} size={} finish={}",
            state.connect, state.execute, state.rows, state.table,
            state.is_null, state.exact_size, state.finish));
    }
    if (state.rows == 0u) return std::nullopt;
    return bytes;
}

[[nodiscard]] inline std::optional<minimizer_generation_storage_descriptor>
read_locked_live_descriptor_for_publication_pg(
        const minimizer_generation_administration_identity& identity,
        const std::uint64_t query_key) {
    NameData index_name{};
    bioseq::postgres::support::invoke_pg_as_cpp([&] {
        namestrcpy(&index_name, identity.index_name.c_str());
    });
    std::array<Oid, 7u> types{
        OIDOID, OIDOID, OIDOID, OIDOID, INT2OID, NAMEOID, INT8OID};
    std::array<Datum, 7u> arguments{
        ObjectIdGetDatum(identity.database_oid),
        ObjectIdGetDatum(identity.tablespace_oid),
        ObjectIdGetDatum(identity.main_relfilenode),
        ObjectIdGetDatum(identity.user_relid),
        Int16GetDatum(identity.column_attno),
        NameGetDatum(&index_name),
        Int64GetDatum(static_cast<std::int64_t>(query_key)),
    };
    static constexpr std::string_view k_select_sql{
        "SELECT * FROM bioseq_internal.minimizer_generation_storage "
        "WHERE database_oid=$1 AND tablespace_oid=$2 "
        "AND main_relfilenode=$3 AND user_relid=$4 "
        "AND column_attno=$5 AND index_name=$6 AND query_key=$7 "
        "AND state=smallint '1' FOR UPDATE"};
    struct raw_row {
        Oid database_oid = InvalidOid;
        Oid tablespace_oid = InvalidOid;
        Oid main_relfilenode = InvalidOid;
        Oid user_relid = InvalidOid;
        std::int16_t column_attno = 0;
        NameData index_name{};
        std::int64_t query_key = -1;
        std::int64_t generation_id = 0;
        std::int16_t storage_kind = -1;
        Oid payload_relation_oid = InvalidOid;
        Oid payload_database_oid = InvalidOid;
        Oid payload_tablespace_oid = InvalidOid;
        Oid payload_relfilenode = InvalidOid;
        NameData payload_relation_name{};
        std::int64_t image_bytes = 0;
        std::int64_t header_checksum = 0;
        FullTransactionId retire_full_xid = InvalidFullTransactionId;
        std::int16_t state = 0;
        bool payload_relation_oid_null = true;
        bool payload_database_oid_null = true;
        bool payload_tablespace_oid_null = true;
        bool payload_relfilenode_null = true;
        bool payload_relation_name_null = true;
        bool retire_full_xid_null = true;
        bool required_null = false;
    } row{};
    struct operation_state {
        int connect = 0;
        int execute = 0;
        int finish = 0;
        std::uint64_t rows = 0u;
        bool table = false;
    } state{};
    bioseq::postgres::support::invoke_pg_as_cpp(
        [&state, &row, &types, &arguments] noexcept {
            state.connect = SPI_connect();
            if (state.connect != SPI_OK_CONNECT) return;
            state.execute = SPI_execute_with_args(
                k_select_sql.data(), static_cast<int>(types.size()), types.data(),
                arguments.data(), nullptr, false, 1);
            state.rows = SPI_processed;
            state.table = SPI_tuptable != nullptr;
            if (state.execute == SPI_OK_SELECT && state.rows == 1u
                && state.table) {
                HeapTuple tuple = SPI_tuptable->vals[0];
                TupleDesc descriptor = SPI_tuptable->tupdesc;
                bool is_null = false;
                const auto required = [&](const int attribute) {
                    const Datum value = SPI_getbinval(
                        tuple, descriptor, attribute, &is_null);
                    row.required_null = row.required_null || is_null;
                    return value;
                };
                row.database_oid = DatumGetObjectId(required(1));
                row.tablespace_oid = DatumGetObjectId(required(2));
                row.main_relfilenode = DatumGetObjectId(required(3));
                row.user_relid = DatumGetObjectId(required(4));
                row.column_attno = DatumGetInt16(required(5));
                row.index_name = *DatumGetName(required(6));
                row.query_key = DatumGetInt64(required(7));
                row.generation_id = DatumGetInt64(required(8));
                row.storage_kind = DatumGetInt16(required(9));
                Datum value = SPI_getbinval(
                    tuple, descriptor, 10, &row.payload_relation_oid_null);
                if (!row.payload_relation_oid_null) {
                    row.payload_relation_oid = DatumGetObjectId(value);
                }
                value = SPI_getbinval(
                    tuple, descriptor, 11, &row.payload_database_oid_null);
                if (!row.payload_database_oid_null) {
                    row.payload_database_oid = DatumGetObjectId(value);
                }
                value = SPI_getbinval(
                    tuple, descriptor, 12, &row.payload_tablespace_oid_null);
                if (!row.payload_tablespace_oid_null) {
                    row.payload_tablespace_oid = DatumGetObjectId(value);
                }
                value = SPI_getbinval(
                    tuple, descriptor, 13, &row.payload_relfilenode_null);
                if (!row.payload_relfilenode_null) {
                    row.payload_relfilenode = DatumGetObjectId(value);
                }
                value = SPI_getbinval(
                    tuple, descriptor, 14, &row.payload_relation_name_null);
                if (!row.payload_relation_name_null) {
                    row.payload_relation_name = *DatumGetName(value);
                }
                row.image_bytes = DatumGetInt64(required(15));
                row.header_checksum = DatumGetInt64(required(16));
                value = SPI_getbinval(
                    tuple, descriptor, 17, &row.retire_full_xid_null);
                if (!row.retire_full_xid_null) {
                    row.retire_full_xid = DatumGetFullTransactionId(value);
                }
                row.state = DatumGetInt16(required(18));
            }
            state.finish = SPI_finish();
        });
    if (state.connect != SPI_OK_CONNECT
        || state.execute != SPI_OK_SELECT || state.rows > 1u
        || (state.rows == 1u && (!state.table || row.required_null))
        || state.finish != SPI_OK_FINISH) {
        minimizer_generation_storage_error_as_cpp(fmt::format(
            "descriptor publication LIVE lookup disagrees: connect={} "
            "execute={} rows={} table={} required_null={} finish={}",
            state.connect, state.execute, state.rows, state.table,
            row.required_null, state.finish));
    }
    if (state.rows == 0u) return std::nullopt;
    if (row.query_key < 0 || row.generation_id <= 0
        || row.image_bytes
            < static_cast<std::int64_t>(
                bioseq::indexing::k_minimizer_fragment_image_header_bytes)
        || row.storage_kind
            != static_cast<std::int16_t>(
                bioseq::indexing::
                    k_minimizer_fragment_image_relation_backed_storage_kind)) {
        minimizer_generation_storage_error_as_cpp(
            "descriptor publication LIVE row scalar shape is invalid");
    }
    minimizer_generation_storage_descriptor result{
        .database_oid = row.database_oid,
        .tablespace_oid = row.tablespace_oid,
        .main_relfilenode = row.main_relfilenode,
        .user_relid = row.user_relid,
        .column_attno = row.column_attno,
        .index_name = NameStr(row.index_name),
        .query_key = static_cast<std::uint64_t>(row.query_key),
        .generation_id = static_cast<std::uint64_t>(row.generation_id),
        .payload_relation_oid = row.payload_relation_oid_null
            ? std::nullopt
            : std::optional<Oid>{row.payload_relation_oid},
        .payload_database_oid = row.payload_database_oid_null
            ? std::nullopt
            : std::optional<Oid>{row.payload_database_oid},
        .payload_tablespace_oid = row.payload_tablespace_oid_null
            ? std::nullopt
            : std::optional<Oid>{row.payload_tablespace_oid},
        .payload_relfilenode = row.payload_relfilenode_null
            ? std::nullopt
            : std::optional<Oid>{row.payload_relfilenode},
        .payload_relation_name = row.payload_relation_name_null
            ? std::nullopt
            : std::optional<std::string>{NameStr(
                row.payload_relation_name)},
        .image_bytes = static_cast<std::uint64_t>(row.image_bytes),
        .header_checksum = checksum_from_pg_bigint(row.header_checksum),
        .retire_full_xid = row.retire_full_xid_null
            ? std::nullopt
            : std::optional<std::uint64_t>{U64FromFullTransactionId(
                row.retire_full_xid)},
        .state = static_cast<minimizer_generation_storage_state>(row.state),
    };
    if (!exact_storage_descriptor_identity(result, identity, query_key)
        || !has_exact_storage_descriptor_relation_shape(result)
        || result.state != minimizer_generation_storage_state::live
        || result.retire_full_xid) {
        minimizer_generation_storage_error_as_cpp(
            "descriptor publication LIVE row shape disagrees");
    }
    return result;
}

inline void apply_minimizer_generation_storage_transition_pg(
        const minimizer_generation_administration_identity& identity,
        const std::uint64_t query_key,
        const std::optional<minimizer_generation_storage_descriptor>& old_live,
        const std::optional<minimizer_generation_storage_descriptor>& new_live) {
    if (!old_live && !new_live) return;
    if ((old_live && (!exact_storage_descriptor_identity(
                *old_live, identity, query_key)
            || old_live->state != minimizer_generation_storage_state::live
            || old_live->retire_full_xid))
        || (new_live && (!exact_storage_descriptor_identity(
                *new_live, identity, query_key)
            || new_live->state != minimizer_generation_storage_state::live
            || new_live->retire_full_xid
            || !has_exact_storage_descriptor_relation_shape(*new_live)
            || !bioseq::indexing::sql_bigint_can_represent(
                new_live->generation_id)
            || !bioseq::indexing::sql_bigint_can_represent(
                new_live->image_bytes)))) {
        minimizer_generation_storage_error_as_cpp(
            "generation storage transition binding is invalid");
    }

    NameData index_name{};
    NameData payload_relation_name{};
    bioseq::postgres::support::invoke_pg_as_cpp([&] {
        namestrcpy(&index_name, identity.index_name.c_str());
        if (new_live && new_live->payload_relation_name) {
            namestrcpy(&payload_relation_name,
                new_live->payload_relation_name->c_str());
        }
    });
    std::array<Oid, 9u> update_types{
        OIDOID, OIDOID, OIDOID, OIDOID, INT2OID, NAMEOID,
        INT8OID, INT8OID, XID8OID};
    std::array<Datum, 9u> update_arguments{
        ObjectIdGetDatum(identity.database_oid),
        ObjectIdGetDatum(identity.tablespace_oid),
        ObjectIdGetDatum(identity.main_relfilenode),
        ObjectIdGetDatum(identity.user_relid),
        Int16GetDatum(identity.column_attno),
        NameGetDatum(&index_name),
        Int64GetDatum(static_cast<std::int64_t>(query_key)),
        Int64GetDatum(old_live
            ? static_cast<std::int64_t>(old_live->generation_id) : 0),
        old_live
            ? FullTransactionIdGetDatum(
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [] { return GetTopFullTransactionId(); }))
            : FullTransactionIdGetDatum(InvalidFullTransactionId),
    };
    static constexpr std::string_view k_update_sql{
        "UPDATE bioseq_internal.minimizer_generation_storage "
        "SET retire_full_xid=$9, state=smallint '2' "
        "WHERE database_oid=$1 AND tablespace_oid=$2 "
        "AND main_relfilenode=$3 AND user_relid=$4 "
        "AND column_attno=$5 AND index_name=$6 AND query_key=$7 "
        "AND generation_id=$8 AND state=smallint '1' "
        "AND retire_full_xid IS NULL"};

    std::array<Oid, k_storage_descriptor_attribute_count> insert_types{
        OIDOID, OIDOID, OIDOID, OIDOID, INT2OID, NAMEOID,
        INT8OID, INT8OID, INT2OID,
        OIDOID, OIDOID, OIDOID, OIDOID, NAMEOID,
        INT8OID, INT8OID, XID8OID, INT2OID};
    std::array<Datum, k_storage_descriptor_attribute_count> insert_arguments{};
    std::array<char, k_storage_descriptor_attribute_count> insert_nulls{};
    if (new_live) {
        insert_arguments = {
            ObjectIdGetDatum(new_live->database_oid),
            ObjectIdGetDatum(new_live->tablespace_oid),
            ObjectIdGetDatum(new_live->main_relfilenode),
            ObjectIdGetDatum(new_live->user_relid),
            Int16GetDatum(new_live->column_attno),
            NameGetDatum(&index_name),
            Int64GetDatum(static_cast<std::int64_t>(new_live->query_key)),
            Int64GetDatum(static_cast<std::int64_t>(
                new_live->generation_id)),
            Int16GetDatum(static_cast<std::int16_t>(
                bioseq::indexing::
                    k_minimizer_fragment_image_relation_backed_storage_kind)),
            new_live->payload_relation_oid
                ? ObjectIdGetDatum(*new_live->payload_relation_oid) : 0,
            new_live->payload_database_oid
                ? ObjectIdGetDatum(*new_live->payload_database_oid) : 0,
            new_live->payload_tablespace_oid
                ? ObjectIdGetDatum(*new_live->payload_tablespace_oid) : 0,
            new_live->payload_relfilenode
                ? ObjectIdGetDatum(*new_live->payload_relfilenode) : 0,
            new_live->payload_relation_name
                ? NameGetDatum(&payload_relation_name) : 0,
            Int64GetDatum(static_cast<std::int64_t>(new_live->image_bytes)),
            Int64GetDatum(checksum_to_pg_bigint(
                new_live->header_checksum)),
            0,
            Int16GetDatum(static_cast<std::int16_t>(
                minimizer_generation_storage_state::live)),
        };
        insert_nulls.fill(' ');
        for (const std::size_t attribute : {9u, 10u, 11u, 12u, 13u}) {
            const bool present = attribute == 9u
                ? new_live->payload_relation_oid.has_value()
                : attribute == 10u
                    ? new_live->payload_database_oid.has_value()
                    : attribute == 11u
                        ? new_live->payload_tablespace_oid.has_value()
                        : attribute == 12u
                            ? new_live->payload_relfilenode.has_value()
                            : new_live->payload_relation_name.has_value();
            if (!present) insert_nulls[attribute] = 'n';
        }
        insert_nulls[16] = 'n';
    }
    static constexpr std::string_view k_insert_sql{
        "INSERT INTO bioseq_internal.minimizer_generation_storage "
        "(database_oid,tablespace_oid,main_relfilenode,user_relid,"
        "column_attno,index_name,query_key,generation_id,provider,"
        "payload_relation_oid,payload_database_oid,payload_tablespace_oid,"
        "payload_relfilenode,payload_relation_name,image_bytes,"
        "header_checksum,retire_full_xid,state) VALUES "
        "($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16,"
        "$17,$18)"};
    struct operation_state {
        int connect = 0;
        int update = 0;
        int insert = 0;
        int finish = 0;
        std::uint64_t updated = 0u;
        std::uint64_t inserted = 0u;
    } state{};
    bioseq::postgres::support::invoke_pg_as_cpp(
        [&state, &update_types, &update_arguments, &insert_types,
         &insert_arguments, &insert_nulls, &old_live,
         &new_live] noexcept {
            state.connect = SPI_connect();
            if (state.connect != SPI_OK_CONNECT) return;
            if (old_live) {
                state.update = SPI_execute_with_args(
                    k_update_sql.data(), static_cast<int>(update_types.size()),
                    update_types.data(), update_arguments.data(), nullptr,
                    false, 0);
                state.updated = SPI_processed;
                if (state.update != SPI_OK_UPDATE || state.updated != 1u) {
                    state.finish = SPI_finish();
                    return;
                }
            }
            if (new_live) {
                state.insert = SPI_execute_with_args(
                    k_insert_sql.data(), static_cast<int>(insert_types.size()),
                    insert_types.data(), insert_arguments.data(),
                    insert_nulls.data(), false, 0);
                state.inserted = SPI_processed;
            }
            state.finish = SPI_finish();
        });
    if (state.connect != SPI_OK_CONNECT
        || (old_live
            && (state.update != SPI_OK_UPDATE || state.updated != 1u))
        || (new_live
            && (state.insert != SPI_OK_INSERT || state.inserted != 1u))
        || state.finish != SPI_OK_FINISH) {
        minimizer_generation_storage_error_as_cpp(fmt::format(
            "generation storage transition disagrees: connect={} "
            "update={} updated={} insert={} inserted={} finish={}",
            state.connect, state.update, state.updated, state.insert,
            state.inserted, state.finish));
    }
}

/// Consume the private complete-reread authorization and publish its head atomically.
/// There is no public overload that accepts an arbitrary relation/generation.
[[nodiscard]] inline ready_minimizer_generation_published_head_receipt
publish_ready_minimizer_generation_head_pg(
        ready_minimizer_generation_token&& token) {
    const auto minimizer_generation_storage_error = [](std::string_view message) {
        minimizer_generation_storage_error_as_cpp(message);
    };
    struct publication_state {
        Relation relation = nullptr;
        Relation descriptor_relation = nullptr;
        bytea* blob = nullptr;
        std::string sql;
    };
    auto* publication = new publication_state{};
    std::array<std::byte, minimizer_generation_head::encoded_bytes>
        returned_head_bytes{};
    std::array<std::byte, minimizer_generation_head::encoded_bytes>
        previous_head_bytes{};
    bool returned_valid = false;
    bool previous_present = false;
    std::optional<minimizer_generation_head> decoded_previous_head{};

    try {
        if (!token.valid_
            || !bioseq::indexing::sql_bigint_can_represent(token.query_key_)
            || token.generation_id_ == 0
            || token.identity_.database_oid != MyDatabaseId
            || !TransactionIdIsValid(token.transaction_id_)
            || token.transaction_id_
                != bioseq::postgres::support::invoke_pg_as_cpp(
                    [] { return GetCurrentTransactionIdIfAny(); })
            || token.advisory_lock_key_
                != index_advisory_xact_lock_key(
                    token.identity_.database_oid,
                    token.identity_.tablespace_oid,
                    token.identity_.main_relfilenode,
                    token.identity_.column_attno,
                    token.identity_.index_name)) {
            minimizer_generation_storage_error("READY publication token is invalid");
        }
        bioseq::postgres::support::invoke_pg_as_cpp([&] {
            acquire_index_advisory_xact_lock(
                token.identity_.database_oid,
                token.identity_.tablespace_oid,
                token.identity_.main_relfilenode,
                token.identity_.column_attno,
                token.identity_.index_name);
        });
        publication->relation = bioseq::postgres::support::invoke_pg_as_cpp([&] {
            return bioseq::postgres::support::try_open_postgres_relation(
                token.sidecar_relation_oid_, RowExclusiveLock);
        });
        if (publication->relation == nullptr
            || RelationGetNamespace(publication->relation)
                != bioseq_internal_ns()
            || std::string_view{
                RelationGetRelationName(publication->relation)}
                != token.identity_.table_name
            || publication->relation->rd_locator.spcOid
                != token.sidecar_locator_.spcOid
            || publication->relation->rd_locator.dbOid
                != token.sidecar_locator_.dbOid
            || publication->relation->rd_locator.relNumber
                != token.sidecar_locator_.relNumber) {
            minimizer_generation_storage_error(
                "READY publication relation identity changed after reread");
        }
        const minimizer_generation_schema_inspection schema =
            bioseq::postgres::support::invoke_pg_as_cpp([&] {
                return inspect_minimizer_generation_schema(
                    publication->relation, nullptr,
                    minimizer_generation_schema_expectation{
                        .namespace_oid = bioseq_internal_ns(),
                        .relation_name = token.identity_.table_name,
                        .tablespace_oid = token.identity_.tablespace_oid,
                    });
            });
        if (!schema) {
            minimizer_generation_storage_error("READY publication schema changed after reread");
        }
        auto decoded_token_head =
            bioseq::indexing::decode_minimizer_generation_head(
                token.head_bytes_);
        if (!decoded_token_head
            || decoded_token_head->generation_id != token.generation_id_
            || decoded_token_head->source_identity != token.source_identity_
            || decoded_token_head->source_content_checksum
                != token.source_content_checksum_
            || decoded_token_head->storage_contract_checksum
                != token.storage_contract_checksum_
            || token.head_row_checksum_
                != bioseq::indexing::crc64_index_chunk_row(
                    0, -1, k_generation_head_sub_key, 0,
                    static_cast<std::uint32_t>(token.head_bytes_.size()),
                    token.head_bytes_)) {
            minimizer_generation_storage_error("READY publication token head disagrees");
        }

        const auto locked_previous_head =
            read_locked_generation_head_for_descriptor_publication_pg(
                token.identity_.table_name, token.query_key_);
        previous_present = locked_previous_head.has_value();
        if (locked_previous_head) {
            previous_head_bytes = *locked_previous_head;
            auto decoded_previous_head_result =
                bioseq::indexing::decode_minimizer_generation_head(
                    previous_head_bytes);
            if (!decoded_previous_head_result
                || decoded_previous_head_result->generation_id == 0u
                || decoded_previous_head_result->generation_id
                    == token.generation_id_) {
                minimizer_generation_storage_error(
                    "READY publication previous head is malformed or names the new generation");
            }
            decoded_previous_head = *decoded_previous_head_result;
        }

        const bool fragment_index = token.identity_.index_name
            == k_minimizers_per_fragment_index_name;
        const bool new_grouped_soa = decoded_token_head->layout
            == bioseq::indexing::minimizer_generation_layout::
                grouped_soa_segment_local;
        if (!fragment_index && token.file_receipt_) {
            minimizer_generation_storage_error(
                "non-fragment generation carried a minimizer fragment image file receipt");
        }
        if (fragment_index) {
            const Oid descriptor_oid =
                bioseq::postgres::support::invoke_pg_as_cpp([&] {
                    return get_relname_relid(
                        "minimizer_generation_storage",
                        bioseq_internal_ns());
                });
            if (!OidIsValid(descriptor_oid)) {
                minimizer_generation_storage_error(
                    "generation storage descriptor catalog is missing");
            }
            publication->descriptor_relation =
                bioseq::postgres::support::invoke_pg_as_cpp([&] {
                    return bioseq::postgres::support::try_open_postgres_relation(
                        descriptor_oid, RowExclusiveLock);
                });
            if (publication->descriptor_relation == nullptr
                || RelationGetNamespace(publication->descriptor_relation)
                    != bioseq_internal_ns()
                || std::string_view{RelationGetRelationName(
                    publication->descriptor_relation)}
                    != "minimizer_generation_storage") {
                minimizer_generation_storage_error(
                    "generation storage descriptor catalog identity changed");
            }
            bioseq::storage::
                validate_minimizer_generation_storage_catalogs_pg();

            auto old_live = read_locked_live_descriptor_for_publication_pg(
                token.identity_, token.query_key_);
            const bool old_grouped_soa = decoded_previous_head
                && decoded_previous_head->layout
                    == bioseq::indexing::minimizer_generation_layout::
                        grouped_soa_segment_local;
            if (old_grouped_soa != old_live.has_value()
                || (old_live
                    && old_live->generation_id
                        != decoded_previous_head->generation_id)) {
                minimizer_generation_storage_error(
                    "active head and LIVE generation descriptor disagree");
            }
            if (old_live
                && (old_live->payload_relation_oid
                        != std::optional<Oid>{token.sidecar_relation_oid_}
                    || old_live->payload_database_oid
                        != std::optional<Oid>{token.sidecar_locator_.dbOid}
                    || old_live->payload_tablespace_oid
                        != std::optional<Oid>{token.sidecar_locator_.spcOid}
                    || old_live->payload_relfilenode
                        != std::optional<Oid>{
                            static_cast<Oid>(
                                token.sidecar_locator_.relNumber)}
                    || old_live->payload_relation_name
                        != std::optional<std::string>{
                            token.identity_.table_name})) {
                minimizer_generation_storage_error(
                    "previous relation-backed descriptor locator changed");
            }
            if (new_grouped_soa != token.file_receipt_.has_value()) {
                minimizer_generation_storage_error(
                    "new minimizer-fragment grouped-SoA head and file receipt disagree");
            }
            std::optional<minimizer_generation_storage_descriptor> new_live;
            if (token.file_receipt_) {
                const minimizer_fragment_image_file_receipt& file = *token.file_receipt_;
                if (file.file_bytes
                        < bioseq::indexing::k_minimizer_fragment_image_header_bytes
                    || !bioseq::indexing::sql_bigint_can_represent(
                        file.file_bytes)) {
                    minimizer_generation_storage_error(
                        "new minimizer fragment image file receipt cannot be cataloged");
                }
                new_live = minimizer_generation_storage_descriptor{
                    .database_oid = token.identity_.database_oid,
                    .tablespace_oid = token.identity_.tablespace_oid,
                    .main_relfilenode = token.identity_.main_relfilenode,
                    .user_relid = token.identity_.user_relid,
                    .column_attno = token.identity_.column_attno,
                    .index_name = token.identity_.index_name,
                    .query_key = token.query_key_,
                    .generation_id = token.generation_id_,
                    .payload_relation_oid =
                        std::optional<Oid>{token.sidecar_relation_oid_},
                    .payload_database_oid =
                        std::optional<Oid>{token.sidecar_locator_.dbOid},
                    .payload_tablespace_oid =
                        std::optional<Oid>{token.sidecar_locator_.spcOid},
                    .payload_relfilenode = std::optional<Oid>{
                        static_cast<Oid>(token.sidecar_locator_.relNumber)},
                    .payload_relation_name = std::optional<std::string>{
                        token.identity_.table_name},
                    .image_bytes = file.file_bytes,
                    .header_checksum = file.header_crc64,
                    .retire_full_xid = std::nullopt,
                    .state = minimizer_generation_storage_state::live,
                };
            }
            apply_minimizer_generation_storage_transition_pg(
                token.identity_, token.query_key_, old_live, new_live);
        }

        publication->blob = bioseq::postgres::support::invoke_pg_as_cpp([&] {
            return reinterpret_cast<bytea*>(
                palloc(VARHDRSZ + token.head_bytes_.size()));
        });
        SET_VARSIZE(publication->blob, VARHDRSZ + token.head_bytes_.size());
        std::memcpy(VARDATA(publication->blob), token.head_bytes_.data(),
            token.head_bytes_.size());
    std::array<Oid, 8> argument_types{
        INT8OID, INT8OID, INT4OID, INT8OID,
        INT4OID, INT4OID, INT8OID, BYTEAOID,
    };
    std::array<Datum, 8> arguments{
        Int64GetDatum(static_cast<std::int64_t>(token.query_key_)),
        Int64GetDatum(0),
        Int32GetDatum(-1),
        Int64GetDatum(static_cast<std::int64_t>(k_generation_head_sub_key)),
        Int32GetDatum(0),
        Int32GetDatum(static_cast<std::int32_t>(token.head_bytes_.size())),
        Int64GetDatum(checksum_to_pg_bigint(token.head_row_checksum_)),
        PointerGetDatum(publication->blob),
    };
    const char* quoted_table = bioseq::postgres::support::invoke_pg_as_cpp([&] {
        return quote_identifier(token.identity_.table_name.c_str());
    });
    publication->sql = fmt::format(
        "INSERT INTO bioseq_internal.{} "
        "(query_key,generation_id,position_segment,sub_key,chunk_no,"
        "chunk_bytes,checksum,blob) VALUES "
        "($1,$2,$3,$4,$5,$6,$7,$8) "
        "ON CONFLICT "
        "(generation_id,query_key,sub_key,position_segment,chunk_no) "
        "DO UPDATE SET blob=EXCLUDED.blob, "
        "chunk_bytes=EXCLUDED.chunk_bytes, checksum=EXCLUDED.checksum "
        "RETURNING blob",
        quoted_table);
        struct publish_spi_state {
            int connect_status = 0;
            int execute_status = 0;
            int finish_status = 0;
            std::uint64_t processed = 0;
            bool table_present = false;
            bool returned_is_null = true;
            bool returned_size_valid = false;
        } spi{};
        const char* publication_sql = publication->sql.c_str();
        bioseq::postgres::support::invoke_pg_as_cpp(
            [&spi, publication_sql, &argument_types, &arguments,
             &returned_head_bytes] noexcept {
                spi.connect_status = SPI_connect();
                if (spi.connect_status != SPI_OK_CONNECT) return;
                spi.execute_status = SPI_execute_with_args(
                    publication_sql,
                    static_cast<int>(argument_types.size()),
                    argument_types.data(), arguments.data(),
                    nullptr, false, 1);
                inject_minimizer_generation_failure_leaf(
                    minimizer_generation_failure_injection::generation_spi_publish,
                    "generation_spi_publish");
                spi.processed = SPI_processed;
                spi.table_present = SPI_tuptable != nullptr;
                if (spi.execute_status == SPI_OK_INSERT_RETURNING
                    && spi.processed == 1 && spi.table_present) {
                    const Datum returned_blob = SPI_getbinval(
                        SPI_tuptable->vals[0], SPI_tuptable->tupdesc,
                        1, &spi.returned_is_null);
                    if (!spi.returned_is_null) {
                        auto* detoasted = reinterpret_cast<bytea*>(
                            PG_DETOAST_DATUM_PACKED(returned_blob));
                        const std::size_t bytes =
                            VARSIZE_ANY_EXHDR(detoasted);
                        if (bytes == returned_head_bytes.size()) {
                            std::memcpy(
                                returned_head_bytes.data(),
                                VARDATA_ANY(detoasted), bytes);
                            spi.returned_size_valid = true;
                        }
                        if (static_cast<void*>(detoasted)
                            != DatumGetPointer(returned_blob)) {
                            pfree(detoasted);
                        }
                    }
                }
                spi.finish_status = SPI_finish();
            });
        returned_valid = spi.returned_size_valid;
        if (spi.connect_status != SPI_OK_CONNECT
            || spi.execute_status != SPI_OK_INSERT_RETURNING
            || spi.processed != 1 || !spi.table_present
            || spi.returned_is_null || !spi.returned_size_valid
            || spi.finish_status != SPI_OK_FINISH) {
            minimizer_generation_storage_error(fmt::format(
                "READY publication SPI session disagrees: connect={} "
                "execute={} rows={} table={} null={} size={} finish={}",
                spi.connect_status, spi.execute_status, spi.processed,
                spi.table_present, spi.returned_is_null,
                spi.returned_size_valid, spi.finish_status));
        }
        bytea* finished_blob = std::exchange(publication->blob, nullptr);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [&] { pfree(finished_blob); });
        if (publication->descriptor_relation != nullptr) {
            Relation finished_descriptor = std::exchange(
                publication->descriptor_relation, nullptr);
            bioseq::postgres::support::invoke_pg_as_cpp([&] {
                relation_close(finished_descriptor, RowExclusiveLock);
            });
        }
        Relation finished_relation = std::exchange(
            publication->relation, nullptr);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [&] { relation_close(finished_relation, RowExclusiveLock); });
        auto returned_head =
            bioseq::indexing::decode_minimizer_generation_head(
                returned_head_bytes);
        if (!returned_valid || !returned_head
            || !bioseq::indexing::is_fixed_wire_returned_exactly(
                token.head_bytes_, returned_head_bytes)
            || *returned_head != *decoded_token_head) {
            minimizer_generation_storage_error(
                "READY publication did not return the byte-exact validated head");
        }
        std::optional<std::uint64_t> replaced_generation_id;
        if (previous_present) {
            const auto previous_head =
                bioseq::indexing::decode_minimizer_generation_head(
                    previous_head_bytes);
            if (!previous_head || previous_head->generation_id == 0
                || previous_head->generation_id
                    == returned_head->generation_id) {
                minimizer_generation_storage_error(
                    "READY publication previous head is malformed or names the new generation");
            }
            replaced_generation_id = previous_head->generation_id;
        }
        ready_minimizer_generation_published_head_receipt receipt{
            .table_name = token.identity_.table_name,
            .query_key = token.query_key_,
            .head = *returned_head,
            .head_bytes = returned_head_bytes,
            .replaced_generation_id = replaced_generation_id,
        };
        token.invalidate();
        std::string{}.swap(publication->sql);
        delete publication;
        return receipt;
    } catch (const bioseq::postgres::support::postgres_error&) {
        // The current transaction/ResourceOwner owns SPI, relation and palloc
        // state. Do not issue PostgreSQL cleanup calls while propagating the
        // exact original error.
        publication->blob = nullptr;
        publication->descriptor_relation = nullptr;
        publication->relation = nullptr;
        token.invalidate();
        delete publication;
        throw;
    } catch (...) {
        std::exception_ptr failure = std::current_exception();
        publication->blob = nullptr;
        publication->descriptor_relation = nullptr;
        publication->relation = nullptr;
        token.invalidate();
        delete publication;
        std::rethrow_exception(failure);
    }
    delete publication;
    pg_unreachable();
}

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

[[nodiscard]] inline bool concatenation_self_test() noexcept {
    return bioseq::indexing::crc64_ecma182_concatenate(
        bioseq::indexing::crc64_ecma182(std::string_view{"control"}),
        bioseq::indexing::crc64_ecma182(std::string_view{"position"}),
        std::string_view{"position"}.size())
        == bioseq::indexing::crc64_ecma182(
            std::string_view{"controlposition"});
}

} // namespace minimizer_generation_full_crc_detail

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
                    [&] { systable_endscan_ordered(active_scan_); });
                active_scan_ = nullptr;
            }
            if (storage_descriptor_live_index_relation_ != nullptr) {
                bioseq::postgres::support::invoke_pg_as_cpp([&] {
                    index_close(
                        storage_descriptor_live_index_relation_,
                        AccessShareLock);
                });
                storage_descriptor_live_index_relation_ = nullptr;
                stats_.cold.add(stats_.cold.lock_releases, 1);
            }
            if (storage_descriptor_relation_ != nullptr) {
                bioseq::postgres::support::invoke_pg_as_cpp([&] {
                    relation_close(
                        storage_descriptor_relation_, AccessShareLock);
                });
                storage_descriptor_relation_ = nullptr;
                stats_.cold.add(stats_.cold.lock_releases, 1);
            }
            if (primary_index_relation_ != nullptr) {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [&] { index_close(primary_index_relation_, AccessShareLock); });
                primary_index_relation_ = nullptr;
                stats_.cold.add(stats_.cold.lock_releases, 1);
            }
            if (heap_relation_ != nullptr) {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [&] { relation_close(heap_relation_, AccessShareLock); });
                heap_relation_ = nullptr;
                stats_.cold.add(stats_.cold.lock_releases, 1);
            }
            if (snapshot_registered_by_session_ && snapshot_ != nullptr) {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [&] { UnregisterSnapshot(snapshot_); });
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
                    [&] { return RegisterSnapshot(snapshot_); });
                snapshot_registered_by_session_ = true;
                stats_.cold.snapshot_registers = 1;
            }
            const Oid relation_oid = bioseq::postgres::support::invoke_pg_as_cpp([&] {
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
                ? bioseq::postgres::support::invoke_pg_as_cpp([&] {
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
                [&] {
                    return RelationGetPrimaryKeyIndex(heap_relation_);
                });
            if (OidIsValid(primary_index_oid)) {
                primary_index_relation_ = bioseq::postgres::support::invoke_pg_as_cpp(
                    [&] {
                        return index_open(
                            primary_index_oid, AccessShareLock);
                    });
                stats_.cold.add(stats_.cold.sidecar_index_opens, 1);
                stats_.cold.add(stats_.cold.lock_acquires, 1);
            }
            const minimizer_generation_schema_inspection schema = relation_identity_exact
                ? bioseq::postgres::support::invoke_pg_as_cpp([&] {
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
                        bioseq::postgres::support::invoke_pg_as_cpp([&] {
                            index_close(finished_index, AccessShareLock);
                        });
                        stats_.cold.add(stats_.cold.lock_releases, 1);
                    }
                    Relation finished_heap = std::exchange(
                        heap_relation_, nullptr);
                    bioseq::postgres::support::invoke_pg_as_cpp([&] {
                        relation_close(finished_heap, AccessShareLock);
                    });
                    stats_.cold.add(stats_.cold.lock_releases, 1);
                    if (snapshot_registered_by_session_ && snapshot_ != nullptr) {
                        Snapshot finished_snapshot = snapshot_;
                        snapshot_ = nullptr;
                        snapshot_registered_by_session_ = false;
                        bioseq::postgres::support::invoke_pg_as_cpp([&] {
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
            active_scan_ = bioseq::postgres::support::invoke_pg_as_cpp([&] {
                return systable_beginscan_ordered(
                    heap_relation_, primary_index_relation_, snapshot_, 5,
                    keys);
            });
            const auto next_tuple = [&] {
                return bioseq::postgres::support::invoke_pg_as_cpp([&] {
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
                [&] { systable_endscan_ordered(active_scan_); });
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
            active_scan_ = bioseq::postgres::support::invoke_pg_as_cpp([&] {
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
                [&] { systable_endscan_ordered(active_scan_); });
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
        active_scan_ = bioseq::postgres::support::invoke_pg_as_cpp([&] {
            return systable_beginscan_ordered(
                heap_relation_, primary_index_relation_, snapshot_, 4,
                keys);
        });
        const auto next_tuple = [&] {
            return bioseq::postgres::support::invoke_pg_as_cpp([&] {
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
            bioseq::parallelism::saturating_add(
                manifest->directory_bytes, entry_storage);
        const std::uint64_t scratch_headroom =
            bioseq::parallelism::saturating_mul(
                manifest->directory_bytes,
                bioseq::parallelism::k_vector_preallocation_headroom_factor);
        const std::uint64_t admitted_bytes =
            bioseq::parallelism::saturating_add(
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
            bioseq::parallelism::saturating_mul(
                directory_bytes_scratch_.capacity(), sizeof(std::byte));
        const std::uint64_t measured_metadata_bytes =
            bioseq::parallelism::saturating_add(
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
            [&] { systable_endscan_ordered(active_scan_); });
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
                bioseq::postgres::support::invoke_pg_as_cpp([&] {
                    return get_relname_relid(
                        "minimizer_generation_storage",
                        bioseq_internal_ns());
                });
            const Oid live_index_oid =
                bioseq::postgres::support::invoke_pg_as_cpp([&] {
                    return get_relname_relid(
                        "minimizer_generation_storage_one_live",
                        bioseq_internal_ns());
                });
            if (!OidIsValid(descriptor_oid) || !OidIsValid(live_index_oid)) {
                fail_("generation storage descriptor authority is missing");
            }
            storage_descriptor_relation_ =
                bioseq::postgres::support::invoke_pg_as_cpp([&] {
                    return bioseq::postgres::support::try_open_postgres_relation(
                        descriptor_oid, AccessShareLock);
                });
            if (storage_descriptor_relation_ == nullptr) {
                fail_("generation storage descriptor relation disappeared");
            }
            stats_.cold.add(stats_.cold.lock_acquires, 1);
            storage_descriptor_live_index_relation_ =
                bioseq::postgres::support::invoke_pg_as_cpp([&] {
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
            active_scan_ = bioseq::postgres::support::invoke_pg_as_cpp([&] {
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
                            [&](const index_chunk_address& receipt_address,
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
            active_scan_ = bioseq::postgres::support::invoke_pg_as_cpp([&] {
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
                 (tuple = bioseq::postgres::support::invoke_pg_as_cpp([&] {
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
                                [&](const index_chunk_address& receipt_address,
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
                [&] { systable_endscan_ordered(active_scan_); });
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
        source->work_context = bioseq::postgres::support::invoke_pg_as_cpp([&] {
            return AllocSetContextCreate(
                CurrentMemoryContext, "bioseq current generation source inspection",
                ALLOCSET_DEFAULT_SIZES);
        });

        source->session_storage = bioseq::postgres::support::invoke_pg_as_cpp([&] {
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
            [&] { MemoryContextDelete(finished_context); });
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

// =========================================================================
//  hash-major PostgreSQL storage adapter
// =========================================================================

namespace hash_major_minimizer_pg_detail {

[[nodiscard]] constexpr std::size_t stream_index(
        hash_major_minimizer_stream_kind kind) noexcept {
    return static_cast<std::size_t>(kind) - 1u;
}

[[nodiscard]] constexpr hash_major_minimizer_stream_kind stream_kind(
        std::size_t index) noexcept {
    return static_cast<hash_major_minimizer_stream_kind>(index + 1u);
}

[[nodiscard]] constexpr bool valid_stream_kind(
        hash_major_minimizer_stream_kind kind) noexcept {
    return kind == hash_major_minimizer_stream_kind::directory
        || kind == hash_major_minimizer_stream_kind::controls
        || kind == hash_major_minimizer_stream_kind::postings;
}

[[nodiscard]] constexpr index_sub_key_t stream_sub_key(
        hash_major_minimizer_stream_kind kind) noexcept {
    return static_cast<index_sub_key_t>(
        static_cast<std::uint16_t>(kind));
}

[[nodiscard]] constexpr std::uint64_t ceil_rows(
        std::uint64_t bytes) noexcept {
    return bytes / k_hash_major_minimizer_row_payload_bytes
        + (bytes % k_hash_major_minimizer_row_payload_bytes != 0u ? 1u : 0u);
}

[[nodiscard]] inline BufFile* create_spool() {
    BufFile* file = bioseq::postgres::support::invoke_pg_as_cpp(
        [] { return BufFileCreateTemp(false); });
    if (file == nullptr) {
        minimizer_generation_storage_error_as_cpp(
            "hash-major temporary logical stream could not be created");
    }
    return file;
}

inline void append_spool(
        BufFile* file,
        std::span<const std::byte> bytes) {
    if (file == nullptr || bytes.empty()) {
        minimizer_generation_storage_error_as_cpp(
            "hash-major logical stream append is invalid");
    }
    bioseq::postgres::support::invoke_pg_as_cpp(
        [file, bytes] {
            BufFileWrite(file, bytes.data(), bytes.size());
        });
}

inline void rewind_spool(BufFile* file) {
    if (file == nullptr
        || bioseq::postgres::support::invoke_pg_as_cpp(
            [file] { return BufFileSeek(file, 0, 0, SEEK_SET); }) != 0) {
        minimizer_generation_storage_error_as_cpp(
            "hash-major logical stream rewind failed");
    }
}

inline void read_spool_exact(
        BufFile* file,
        std::span<std::byte> bytes) {
    if (file == nullptr || bytes.empty()) {
        minimizer_generation_storage_error_as_cpp(
            "hash-major logical stream read is invalid");
    }
    bioseq::postgres::support::invoke_pg_as_cpp(
        [file, bytes] {
            BufFileReadExact(file, bytes.data(), bytes.size());
        });
}

inline void close_spool(BufFile*& file) {
    BufFile* closing = std::exchange(file, nullptr);
    if (closing != nullptr) {
        bioseq::postgres::support::invoke_pg_as_cpp(
            [closing] { BufFileClose(closing); });
    }
}

inline void abandon_spool(BufFile*& file) noexcept {
    // PostgreSQL ResourceOwner owns a file whose leaf raised ERROR.
    file = nullptr;
}

[[nodiscard]] inline HeapTuple form_row(
        Relation relation,
        std::uint64_t query_key,
        const index_chunk_address& address,
        std::span<const std::byte> bytes,
        std::uint64_t checksum) {
    if (relation == nullptr || bytes.empty()
        || bytes.size() > k_hash_major_minimizer_row_payload_bytes
        || bytes.size()
            > static_cast<std::size_t>(
                (std::numeric_limits<std::int32_t>::max)())
        || !bioseq::indexing::sql_bigint_can_represent(query_key)
        || !bioseq::indexing::sql_bigint_can_represent(
            address.generation_id)
        || !bioseq::indexing::sql_integer_can_represent(address.chunk_no)
        || checksum != bioseq::indexing::crc64_index_chunk_row(
            address.generation_id,
            address.position_segment,
            address.sub_key,
            address.chunk_no,
            static_cast<std::uint32_t>(bytes.size()),
            bytes)) {
        minimizer_generation_storage_error_as_cpp(
            "hash-major row envelope is invalid");
    }
    bytea* blob = bioseq::postgres::support::invoke_pg_as_cpp(
        [bytes] {
            return reinterpret_cast<bytea*>(
                palloc(VARHDRSZ + bytes.size()));
        });
    SET_VARSIZE(blob, VARHDRSZ + bytes.size());
    std::memcpy(VARDATA(blob), bytes.data(), bytes.size());
    bioseq::postgres::support::pg_datum_list<8> values{};
    values.set(k_minimizer_generation_attribute_query_key, static_cast<std::int64_t>(query_key));
    values.set(k_minimizer_generation_attribute_generation_id,
        static_cast<std::int64_t>(address.generation_id));
    values.set(k_minimizer_generation_attribute_position_segment, address.position_segment);
    values.set(k_minimizer_generation_attribute_sub_key,
        static_cast<std::int64_t>(address.sub_key));
    values.set(k_minimizer_generation_attribute_chunk_number,
        static_cast<std::int32_t>(address.chunk_no));
    values.set(k_minimizer_generation_attribute_chunk_bytes,
        static_cast<std::int32_t>(bytes.size()));
    values.set(k_minimizer_generation_attribute_checksum, checksum_to_pg_bigint(checksum));
    values.set_raw(k_minimizer_generation_attribute_blob,
        bioseq::postgres::support::pg_datum::from_pointer(blob));
    HeapTuple tuple = bioseq::postgres::support::invoke_pg_as_cpp(
        [&] { return values.form_tuple(RelationGetDescr(relation)); });
    bioseq::postgres::support::invoke_pg_as_cpp([blob] { pfree(blob); });
    return tuple;
}

[[nodiscard]] inline HeapTuple form_row(
        Relation relation,
        std::uint64_t query_key,
        const index_chunk_address& address,
        std::span<const std::byte> bytes) {
    return form_row(
        relation,
        query_key,
        address,
        bytes,
        bioseq::indexing::crc64_index_chunk_row(
            address.generation_id,
            address.position_segment,
            address.sub_key,
            address.chunk_no,
            static_cast<std::uint32_t>(bytes.size()),
            bytes));
}

} // namespace hash_major_minimizer_pg_detail

class ready_hash_major_minimizer_token;

struct ready_hash_major_minimizer_published_head_receipt {
    std::string table_name;
    std::uint64_t query_key = 0u;
    hash_major_minimizer_head head{};
    encoded_hash_major_minimizer_head head_bytes{};
    std::optional<std::uint64_t> replaced_generation_id{};
};

[[nodiscard]] inline ready_hash_major_minimizer_published_head_receipt
publish_ready_hash_major_minimizer_head_pg(
    ready_hash_major_minimizer_token&& token);

/// Move-only authorization that the exact unpublished hash-major generation was
/// authenticated from PostgreSQL rows and bound to the authoritative source.
class ready_hash_major_minimizer_token {
private:
    minimizer_generation_administration_identity identity_{};
    Oid sidecar_relation_oid_ = InvalidOid;
    RelFileLocator sidecar_locator_{};
    std::uint64_t advisory_lock_key_ = 0u;
    std::uint64_t query_key_ = 0u;
    std::uint64_t generation_id_ = 0u;
    TransactionId transaction_id_ = InvalidTransactionId;
    hash_major_minimizer_head head_{};
    encoded_hash_major_minimizer_head head_bytes_{};
    std::uint64_t head_row_checksum_ = 0u;
    bool valid_ = false;

    ready_hash_major_minimizer_token(
            minimizer_generation_administration_identity identity,
            Oid sidecar_relation_oid,
            RelFileLocator sidecar_locator,
            std::uint64_t advisory_lock_key,
            std::uint64_t query_key,
            std::uint64_t generation_id,
            TransactionId transaction_id,
            const hash_major_minimizer_head& head,
            const encoded_hash_major_minimizer_head& head_bytes,
            std::uint64_t head_row_checksum)
        : identity_(std::move(identity)),
          sidecar_relation_oid_(sidecar_relation_oid),
          sidecar_locator_(sidecar_locator),
          advisory_lock_key_(advisory_lock_key),
          query_key_(query_key),
          generation_id_(generation_id),
          transaction_id_(transaction_id),
          head_(head),
          head_bytes_(head_bytes),
          head_row_checksum_(head_row_checksum),
          valid_(true) {}

    void invalidate_() noexcept {
        identity_.release_owned_strings();
        sidecar_relation_oid_ = InvalidOid;
        sidecar_locator_ = {};
        advisory_lock_key_ = 0u;
        query_key_ = 0u;
        generation_id_ = 0u;
        transaction_id_ = InvalidTransactionId;
        head_ = {};
        head_bytes_.fill(std::byte{0});
        head_row_checksum_ = 0u;
        valid_ = false;
    }

    friend class index_storage_hash_major_minimizer_writer;
    friend ready_hash_major_minimizer_published_head_receipt
    publish_ready_hash_major_minimizer_head_pg(
        ready_hash_major_minimizer_token&& token);

public:
    ready_hash_major_minimizer_token() = delete;
    ready_hash_major_minimizer_token(
        const ready_hash_major_minimizer_token&) = delete;
    ready_hash_major_minimizer_token& operator=(
        const ready_hash_major_minimizer_token&) = delete;
    ready_hash_major_minimizer_token(
            ready_hash_major_minimizer_token&& other) noexcept
        : identity_(std::move(other.identity_)),
          sidecar_relation_oid_(other.sidecar_relation_oid_),
          sidecar_locator_(other.sidecar_locator_),
          advisory_lock_key_(other.advisory_lock_key_),
          query_key_(other.query_key_),
          generation_id_(other.generation_id_),
          transaction_id_(other.transaction_id_),
          head_(other.head_),
          head_bytes_(other.head_bytes_),
          head_row_checksum_(other.head_row_checksum_),
          valid_(other.valid_) {
        other.invalidate_();
    }
    ready_hash_major_minimizer_token& operator=(
        ready_hash_major_minimizer_token&&) = delete;
    ~ready_hash_major_minimizer_token() = default;
};

static_assert(!std::is_default_constructible_v<
    ready_hash_major_minimizer_token>);
static_assert(!std::is_copy_constructible_v<
    ready_hash_major_minimizer_token>);
static_assert(std::is_nothrow_move_constructible_v<
    ready_hash_major_minimizer_token>);

inline constexpr std::uint64_t k_hash_major_minimizer_publication_slice_bytes =
    std::uint64_t{8} << 20u;

/// PostgreSQL-owned bounded sink for the storage-neutral hash-major encoder.
/// The callback path only appends to three ResourceOwner-managed BufFiles.
/// Sealing replays exact 8,064-byte rows in postings -> controls -> directory
/// order, writes the manifest, and rereads the complete unpublished generation
/// before a private publication token can be produced.
class index_storage_hash_major_minimizer_writer {
private:
    enum class operation_state : std::uint8_t {
        accepting,
        sealed,
        failed,
        moved,
    };

    static constexpr std::uint64_t k_batch_rows = 1'024u;

    minimizer_generation_administration_identity identity_{};
    Relation relation_ = nullptr;
    CatalogIndexState catalog_indexes_ = nullptr;
    Snapshot validation_snapshot_ = nullptr;
    Relation validation_primary_index_ = nullptr;
    SysScanDesc validation_scan_ = nullptr;
    std::array<BufFile*, 3> stream_spools_{};
    std::uint64_t advisory_lock_key_ = 0u;
    std::uint64_t query_key_ = 0u;
    std::uint64_t generation_id_ = 0u;
    std::array<std::uint64_t, 3> logical_bytes_{};
    std::array<crc64_ecma182_accumulator, 3> logical_crc_{};
    bioseq::parallelism::memory_permit publication_permit_{};
    std::vector<TupleTableSlot*> pending_slots_{};
    std::uint64_t pending_tuple_bytes_ = 0u;
    std::array<std::byte, k_hash_major_minimizer_row_payload_bytes> row_buffer_{};
    operation_state state_ = operation_state::accepting;

    [[noreturn]] void fail_(std::string_view message) {
        state_ = operation_state::failed;
        minimizer_generation_storage_error_as_cpp(message);
    }

    void abandon_noexcept_() noexcept {
        relation_ = nullptr;
        catalog_indexes_ = nullptr;
        validation_snapshot_ = nullptr;
        validation_primary_index_ = nullptr;
        validation_scan_ = nullptr;
        for (BufFile*& file : stream_spools_) {
            hash_major_minimizer_pg_detail::abandon_spool(file);
        }
        pending_slots_.clear();
        std::vector<TupleTableSlot*>{}.swap(pending_slots_);
        pending_tuple_bytes_ = 0u;
        publication_permit_.release();
    }

    void drop_pending_slots_() {
        for (TupleTableSlot* slot : pending_slots_) {
            if (slot != nullptr) {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [slot] { ExecDropSingleTupleTableSlot(slot); });
            }
        }
        pending_slots_.clear();
        pending_tuple_bytes_ = 0u;
    }

    void flush_batch_() {
        if (pending_slots_.empty()) {
            if (pending_tuple_bytes_ != 0u) {
                fail_("hash-major tuple batch accounting disagrees");
            }
            return;
        }
        if (relation_ == nullptr || catalog_indexes_ == nullptr
            || pending_slots_.size()
                > static_cast<std::size_t>(
                    (std::numeric_limits<int>::max)())) {
            fail_("hash-major tuple batch state is invalid");
        }
        bioseq::postgres::support::invoke_pg_as_cpp([&] {
            CatalogTuplesMultiInsertWithInfo(
                relation_,
                pending_slots_.data(),
                static_cast<int>(pending_slots_.size()),
                catalog_indexes_);
        });
        drop_pending_slots_();
    }

    void queue_row_(
            const index_chunk_address& address,
            std::span<const std::byte> bytes) {
        HeapTuple tuple = hash_major_minimizer_pg_detail::form_row(
            relation_, query_key_, address, bytes);
        const std::uint64_t tuple_bytes =
            static_cast<std::uint64_t>(tuple->t_len)
            + sizeof(TupleTableSlot*) + sizeof(HeapTupleData);
        const auto decision =
            bioseq::indexing::plan_index_tuple_batch_append(
                pending_slots_.size(),
                pending_tuple_bytes_,
                tuple_bytes,
                k_batch_rows,
                k_hash_major_minimizer_publication_slice_bytes);
        if (!decision.valid) {
            heap_freetuple(tuple);
            fail_("hash-major row exceeds the bounded publication batch");
        }
        if (decision.flush_before) flush_batch_();
        TupleTableSlot* slot = bioseq::postgres::support::invoke_pg_as_cpp([&] {
            return MakeSingleTupleTableSlot(
                RelationGetDescr(relation_), &TTSOpsHeapTuple);
        });
        try {
            pending_slots_.push_back(slot);
        } catch (...) {
            heap_freetuple(tuple);
            bioseq::postgres::support::invoke_pg_as_cpp(
                [slot] { ExecDropSingleTupleTableSlot(slot); });
            throw;
        }
        bioseq::postgres::support::invoke_pg_as_cpp(
            [slot, tuple] { ExecStoreHeapTuple(tuple, slot, true); });
        if (tuple_bytes
            > (std::numeric_limits<std::uint64_t>::max)()
                - pending_tuple_bytes_) {
            fail_("hash-major publication byte counter overflow");
        }
        pending_tuple_bytes_ += tuple_bytes;
        if (decision.flush_after) flush_batch_();
    }

    void replay_stream_(
            hash_major_minimizer_stream_kind kind,
            const bioseq::indexing::hash_major_minimizer_stream_descriptor&
                descriptor) {
        using hash_major_minimizer_pg_detail::ceil_rows;
        using hash_major_minimizer_pg_detail::stream_index;
        const std::size_t index = stream_index(kind);
        if (descriptor.kind != kind
            || descriptor.logical_bytes != logical_bytes_[index]
            || descriptor.stream_crc64 != logical_crc_[index].checksum()
            || descriptor.row_count != ceil_rows(descriptor.logical_bytes)
            || descriptor.row_count
                > static_cast<std::uint64_t>(
                    (std::numeric_limits<std::int32_t>::max)())) {
            fail_("hash-major stream descriptor disagrees with encoder output");
        }
        hash_major_minimizer_pg_detail::rewind_spool(stream_spools_[index]);
        std::uint64_t remaining = descriptor.logical_bytes;
        for (std::uint64_t row = 0u; row < descriptor.row_count; ++row) {
            const std::size_t bytes = static_cast<std::size_t>(
                std::min<std::uint64_t>(
                    remaining, k_hash_major_minimizer_row_payload_bytes));
            if (bytes == 0u
                || (row + 1u < descriptor.row_count
                    && bytes != k_hash_major_minimizer_row_payload_bytes)) {
                fail_("hash-major physical stream row equation disagrees");
            }
            auto payload = std::span<std::byte>{row_buffer_}.first(bytes);
            hash_major_minimizer_pg_detail::read_spool_exact(
                stream_spools_[index], payload);
            queue_row_(
                index_chunk_address{
                    .generation_id = generation_id_,
                    .position_segment = 0,
                    .sub_key = hash_major_minimizer_pg_detail::stream_sub_key(kind),
                    .chunk_no = static_cast<std::uint32_t>(row),
                },
                payload);
            remaining -= bytes;
        }
        if (remaining != 0u) {
            fail_("hash-major physical stream replay was incomplete");
        }
    }

    [[nodiscard]] minimizer_generation_loaded_row read_exact_row_(
            const index_chunk_address& expected_address) {
        ScanKeyData keys[5];
        ScanKeyInit(&keys[0], k_minimizer_generation_index_generation_id,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(
                expected_address.generation_id)));
        ScanKeyInit(&keys[1], k_minimizer_generation_index_query_key,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(query_key_)));
        ScanKeyInit(&keys[2], k_minimizer_generation_index_sub_key,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(
                expected_address.sub_key)));
        ScanKeyInit(&keys[3], k_minimizer_generation_index_position_segment,
            BTEqualStrategyNumber, F_INT4EQ,
            Int32GetDatum(expected_address.position_segment));
        ScanKeyInit(&keys[4], k_minimizer_generation_index_chunk_number,
            BTEqualStrategyNumber, F_INT4EQ,
            Int32GetDatum(static_cast<std::int32_t>(
                expected_address.chunk_no)));
        validation_scan_ = bioseq::postgres::support::invoke_pg_as_cpp([&] {
            return systable_beginscan_ordered(
                relation_, validation_primary_index_,
                validation_snapshot_, 5, keys);
        });
        HeapTuple tuple = bioseq::postgres::support::invoke_pg_as_cpp([&] {
            return systable_getnext_ordered(
                validation_scan_, ForwardScanDirection);
        });
        if (!HeapTupleIsValid(tuple)) {
            fail_("hash-major validation row is missing");
        }
        minimizer_generation_loaded_row row =
            load_minimizer_generation_row_or_error(relation_, tuple);
        HeapTuple extra = bioseq::postgres::support::invoke_pg_as_cpp([&] {
            return systable_getnext_ordered(
                validation_scan_, ForwardScanDirection);
        });
        bioseq::postgres::support::invoke_pg_as_cpp(
            [&] { systable_endscan_ordered(validation_scan_); });
        validation_scan_ = nullptr;
        if (HeapTupleIsValid(extra)
            || row.query_key != query_key_
            || row.address != expected_address) {
            row.release();
            fail_("hash-major validation row identity is ambiguous");
        }
        return row;
    }

    void reread_stream_(
            hash_major_minimizer_stream_kind kind,
            const bioseq::indexing::hash_major_minimizer_stream_descriptor&
                descriptor) {
        ScanKeyData keys[4];
        ScanKeyInit(&keys[0], k_minimizer_generation_index_generation_id,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(generation_id_)));
        ScanKeyInit(&keys[1], k_minimizer_generation_index_query_key,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(query_key_)));
        ScanKeyInit(&keys[2], k_minimizer_generation_index_sub_key,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(
                hash_major_minimizer_pg_detail::stream_sub_key(kind))));
        ScanKeyInit(&keys[3], k_minimizer_generation_index_position_segment,
            BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(0));
        validation_scan_ = bioseq::postgres::support::invoke_pg_as_cpp([&] {
            return systable_beginscan_ordered(
                relation_, validation_primary_index_,
                validation_snapshot_, 4, keys);
        });
        crc64_ecma182_accumulator crc;
        std::uint64_t bytes_read = 0u;
        std::uint64_t ordinal = 0u;
        for (HeapTuple tuple;
             HeapTupleIsValid(tuple =
                 bioseq::postgres::support::invoke_pg_as_cpp([&] {
                     return systable_getnext_ordered(
                         validation_scan_, ForwardScanDirection);
                 }));) {
            minimizer_generation_loaded_row row =
                load_minimizer_generation_row_or_error(relation_, tuple);
            const index_chunk_address expected_address{
                    .generation_id = generation_id_,
                    .position_segment = 0,
                    .sub_key = hash_major_minimizer_pg_detail::stream_sub_key(kind),
                    .chunk_no = static_cast<std::uint32_t>(ordinal),
                };
            const std::uint64_t remaining =
                descriptor.logical_bytes - bytes_read;
            const std::size_t expected_bytes =
                static_cast<std::size_t>(std::min<std::uint64_t>(
                    remaining, k_hash_major_minimizer_row_payload_bytes));
            if (ordinal >= descriptor.row_count
                || row.query_key != query_key_
                || row.address != expected_address
                || row.bytes.size() != expected_bytes) {
                row.release();
                fail_("hash-major validation stream row length disagrees");
            }
            crc.update(row.bytes);
            bytes_read += row.bytes.size();
            ++ordinal;
            row.release();
        }
        bioseq::postgres::support::invoke_pg_as_cpp(
            [&] { systable_endscan_ordered(validation_scan_); });
        validation_scan_ = nullptr;
        if (ordinal != descriptor.row_count
            || bytes_read != descriptor.logical_bytes
            || crc.checksum() != descriptor.stream_crc64) {
            fail_("hash-major validation stream aggregate disagrees");
        }
    }

    [[nodiscard]] static bool append_callback_(
            void* context,
            hash_major_minimizer_stream_kind kind,
            std::span<const std::byte> bytes) {
        return static_cast<index_storage_hash_major_minimizer_writer*>(
            context)->append_logical_bytes(kind, bytes);
    }

public:
    index_storage_hash_major_minimizer_writer() = delete;
    index_storage_hash_major_minimizer_writer(
            minimizer_generation_administration_identity identity,
            std::uint64_t query_key,
            std::uint64_t generation_id)
        : identity_(std::move(identity)),
          query_key_(query_key),
          generation_id_(generation_id) {
        try {
            if (identity_.database_oid != MyDatabaseId
                || !OidIsValid(identity_.tablespace_oid)
                || !OidIsValid(identity_.main_relfilenode)
                || !OidIsValid(identity_.user_relid)
                || identity_.column_attno <= 0
                || identity_.index_name.empty()
                || identity_.table_name.empty()
                || generation_id_ == 0u
                || !bioseq::indexing::sql_bigint_can_represent(query_key_)
                || !bioseq::indexing::sql_bigint_can_represent(
                    generation_id_)) {
                fail_("hash-major writer identity is invalid");
            }
            auto permit =
                bioseq::parallelism::memory_admission_controller::instance()
                    .try_reserve(k_hash_major_minimizer_publication_slice_bytes);
            if (!permit) {
                fail_("hash-major publication memory admission was denied");
            }
            publication_permit_ = std::move(*permit);
            pending_slots_.reserve(k_batch_rows);
            advisory_lock_key_ = index_advisory_xact_lock_key(
                identity_.database_oid,
                identity_.tablespace_oid,
                identity_.main_relfilenode,
                identity_.column_attno,
                identity_.index_name);
            bioseq::postgres::support::invoke_pg_as_cpp([&] {
                acquire_index_advisory_xact_lock(
                    identity_.database_oid,
                    identity_.tablespace_oid,
                    identity_.main_relfilenode,
                    identity_.column_attno,
                    identity_.index_name);
            });
            const Oid relation_oid =
                bioseq::postgres::support::invoke_pg_as_cpp([&] {
                    return get_relname_relid(
                        identity_.table_name.c_str(),
                        bioseq_internal_ns());
                });
            relation_ = OidIsValid(relation_oid)
                ? bioseq::postgres::support::invoke_pg_as_cpp([&] {
                    return bioseq::postgres::support::try_open_postgres_relation(
                        relation_oid, RowExclusiveLock);
                })
                : nullptr;
            if (relation_ == nullptr) {
                fail_("hash-major generation relation is missing");
            }
            const minimizer_generation_schema_inspection schema =
                bioseq::postgres::support::invoke_pg_as_cpp([&] {
                    return inspect_minimizer_generation_schema(
                        relation_, nullptr,
                        minimizer_generation_schema_expectation{
                            .namespace_oid = bioseq_internal_ns(),
                            .relation_name = identity_.table_name,
                            .tablespace_oid = identity_.tablespace_oid,
                        });
                });
            if (!schema) {
                fail_(fmt::format(
                    "hash-major generation relation schema mismatch: {}",
                    minimizer_generation_schema_error_name(schema.error)));
            }
            (void)verify_pg_inline_payload_layout(relation_);
            catalog_indexes_ = bioseq::postgres::support::invoke_pg_as_cpp(
                [&] { return CatalogOpenIndexes(relation_); });
            if (catalog_indexes_ == nullptr) {
                fail_("hash-major CatalogOpenIndexes returned NULL");
            }
            for (BufFile*& file : stream_spools_) {
                file = hash_major_minimizer_pg_detail::create_spool();
            }
        } catch (...) {
            state_ = operation_state::failed;
            abandon_noexcept_();
            throw;
        }
    }
    index_storage_hash_major_minimizer_writer(
        const index_storage_hash_major_minimizer_writer&) = delete;
    index_storage_hash_major_minimizer_writer& operator=(
        const index_storage_hash_major_minimizer_writer&) = delete;
    index_storage_hash_major_minimizer_writer(
            index_storage_hash_major_minimizer_writer&& other) noexcept
        : identity_(std::move(other.identity_)),
          relation_(std::exchange(other.relation_, nullptr)),
          catalog_indexes_(std::exchange(
              other.catalog_indexes_, nullptr)),
          validation_snapshot_(std::exchange(
              other.validation_snapshot_, nullptr)),
          validation_primary_index_(std::exchange(
              other.validation_primary_index_, nullptr)),
          validation_scan_(std::exchange(
              other.validation_scan_, nullptr)),
          stream_spools_(other.stream_spools_),
          advisory_lock_key_(other.advisory_lock_key_),
          query_key_(other.query_key_),
          generation_id_(other.generation_id_),
          logical_bytes_(other.logical_bytes_),
          logical_crc_(other.logical_crc_),
          publication_permit_(std::move(other.publication_permit_)),
          pending_slots_(std::move(other.pending_slots_)),
          pending_tuple_bytes_(other.pending_tuple_bytes_),
          row_buffer_(other.row_buffer_),
          state_(other.state_) {
        other.stream_spools_.fill(nullptr);
        other.pending_tuple_bytes_ = 0u;
        other.state_ = operation_state::moved;
    }
    index_storage_hash_major_minimizer_writer& operator=(
        index_storage_hash_major_minimizer_writer&&) = delete;
    ~index_storage_hash_major_minimizer_writer() {
        abandon_noexcept_();
    }

    [[nodiscard]] bool append_logical_bytes(
            hash_major_minimizer_stream_kind kind,
            std::span<const std::byte> bytes) {
        if (state_ != operation_state::accepting
            || !hash_major_minimizer_pg_detail::valid_stream_kind(kind)
            || bytes.empty()) {
            return false;
        }
        const std::size_t index =
            hash_major_minimizer_pg_detail::stream_index(kind);
        if (bytes.size()
            > (std::numeric_limits<std::uint64_t>::max)()
                - logical_bytes_[index]) {
            return false;
        }
        hash_major_minimizer_pg_detail::append_spool(
            stream_spools_[index], bytes);
        logical_crc_[index].update(bytes);
        logical_bytes_[index] += bytes.size();
        return true;
    }

    [[nodiscard]] hash_major_minimizer_logical_stream_sink_ref
    logical_stream_sink() noexcept {
        return {
            .context = this,
            .append_bytes = &append_callback_,
        };
    }

    [[nodiscard]] ready_hash_major_minimizer_token
    seal_and_validate_ready(
            const hash_major_minimizer_encoded_metadata& metadata,
            minimizer_generation_source_authorization source) {
        try {
            if (state_ != operation_state::accepting) {
                fail_("hash-major writer was sealed more than once");
            }
            auto decoded_head =
                bioseq::indexing::decode_hash_major_minimizer_head(
                    metadata.encoded_head);
            auto decoded_manifest =
                bioseq::indexing::decode_hash_major_minimizer_manifest(
                    metadata.encoded_manifest);
            if (!decoded_head || !decoded_manifest
                || *decoded_head != metadata.head
                || *decoded_manifest != metadata.manifest
                || metadata.head.generation_id != generation_id_
                || metadata.head.query_key != query_key_
                || metadata.manifest.generation_id != generation_id_
                || metadata.head.dense_seal.has_value()
                || metadata.manifest.streams[0].kind
                    != hash_major_minimizer_stream_kind::directory
                || metadata.manifest.streams[1].kind
                    != hash_major_minimizer_stream_kind::controls
                || metadata.manifest.streams[2].kind
                    != hash_major_minimizer_stream_kind::postings) {
                fail_("hash-major encoded metadata disagrees");
            }
            const TransactionId transaction_id =
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [] { return GetCurrentTransactionIdIfAny(); });
            if (!source.valid_
                || source.snapshot_ == nullptr
                || !IsMVCCSnapshot(source.snapshot_)
                || source.snapshot_->regd_count == 0
                || !TransactionIdIsValid(transaction_id)
                || source.transaction_id_ != transaction_id
                || source.database_oid_ != identity_.database_oid
                || source.tablespace_oid_ != identity_.tablespace_oid
                || source.main_relfilenode_ != identity_.main_relfilenode
                || source.user_relid_ != identity_.user_relid
                || source.column_attno_ != identity_.column_attno
                || source.index_name_ != identity_.index_name
                || source.table_name_ != identity_.table_name
                || source.source_.sequence_rowno != query_key_) {
                source.invalidate_();
                fail_(
                    "hash-major source authorization is not bound to this writer transaction");
            }
            for (std::size_t index = 0u; index < 3u; ++index) {
                const auto& stream = metadata.manifest.streams[index];
                if (stream.logical_bytes != logical_bytes_[index]
                    || stream.stream_crc64
                        != logical_crc_[index].checksum()
                    || stream.row_count
                        != hash_major_minimizer_pg_detail::ceil_rows(
                            stream.logical_bytes)) {
                    fail_("hash-major logical stream receipt disagrees");
                }
            }

            // Authority is persisted only after all postings, then controls,
            // then the directory.  The primary-key layout remains unchanged;
            // the head is still absent, so none of these rows is visible as a
            // READY generation during replay.
            replay_stream_(
                hash_major_minimizer_stream_kind::postings,
                metadata.manifest.streams[2]);
            replay_stream_(
                hash_major_minimizer_stream_kind::controls,
                metadata.manifest.streams[1]);
            replay_stream_(
                hash_major_minimizer_stream_kind::directory,
                metadata.manifest.streams[0]);
            queue_row_(
                index_chunk_address{
                    .generation_id = generation_id_,
                    .position_segment = 0,
                    .sub_key = 0,
                    .chunk_no = 0u,
                },
                metadata.encoded_manifest);
            flush_batch_();
            bioseq::postgres::support::invoke_pg_as_cpp(
                [&] { CatalogCloseIndexes(catalog_indexes_); });
            catalog_indexes_ = nullptr;
            bioseq::postgres::support::invoke_pg_as_cpp(
                [] { CommandCounterIncrement(); });
            if (!bioseq::postgres::support::invoke_pg_as_cpp(
                    [] { return ActiveSnapshotSet(); })) {
                fail_("hash-major validation requires an active snapshot");
            }
            Snapshot active_snapshot =
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [] { return GetActiveSnapshot(); });
            if (!IsMVCCSnapshot(active_snapshot)) {
                fail_("hash-major validation snapshot is not MVCC");
            }
            bioseq::postgres::support::invoke_pg_as_cpp(
                [] { UpdateActiveSnapshotCommandId(); });
            validation_snapshot_ =
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [active_snapshot] {
                        return RegisterSnapshot(active_snapshot);
                    });
            const Oid primary_index_oid =
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [&] { return RelationGetPrimaryKeyIndex(relation_); });
            validation_primary_index_ = OidIsValid(primary_index_oid)
                ? bioseq::postgres::support::invoke_pg_as_cpp(
                    [primary_index_oid] {
                        return index_open(
                            primary_index_oid, AccessShareLock);
                    })
                : nullptr;
            if (validation_primary_index_ == nullptr) {
                fail_("hash-major validation primary index is missing");
            }

            minimizer_generation_loaded_row manifest_row = read_exact_row_(
                index_chunk_address{
                    .generation_id = generation_id_,
                    .position_segment = 0,
                    .sub_key = 0,
                    .chunk_no = 0u,
                });
            if (manifest_row.bytes.size()
                    != k_hash_major_minimizer_manifest_bytes
                || !std::ranges::equal(
                    manifest_row.bytes,
                    metadata.encoded_manifest)) {
                manifest_row.release();
                fail_("hash-major validation manifest disagrees");
            }
            manifest_row.release();
            reread_stream_(
                hash_major_minimizer_stream_kind::directory,
                metadata.manifest.streams[0]);
            reread_stream_(
                hash_major_minimizer_stream_kind::controls,
                metadata.manifest.streams[1]);
            reread_stream_(
                hash_major_minimizer_stream_kind::postings,
                metadata.manifest.streams[2]);

            const auto expected_source =
                bioseq::indexing::hash_major_minimizer_source_identity(
                    query_key_,
                    source.source_.locator.source_epoch,
                    source.source_content_checksum_,
                    source.source_.locator.logical_length);
            if (!source.valid_
                || source.source_.sequence_rowno != query_key_
                || source.table_name_ != identity_.table_name
                || source.source_.locator.source_epoch
                    != metadata.manifest.source_epoch
                || source.source_content_checksum_
                    != metadata.manifest.source_content_crc64
                || source.source_.locator.logical_length
                    != metadata.manifest.target_length
                || source.source_.alphabet_code
                    != metadata.manifest.storage_profile.alphabet_code
                || expected_source != metadata.manifest.source_identity
                || expected_source != metadata.head.source_identity) {
                source.invalidate_();
                fail_("hash-major validation source authorization disagrees");
            }
            source.invalidate_();

            Relation finished_index = std::exchange(
                validation_primary_index_, nullptr);
            bioseq::postgres::support::invoke_pg_as_cpp(
                [finished_index] {
                    index_close(finished_index, AccessShareLock);
                });
            Snapshot finished_snapshot = std::exchange(
                validation_snapshot_, nullptr);
            bioseq::postgres::support::invoke_pg_as_cpp(
                [finished_snapshot] {
                    UnregisterSnapshot(finished_snapshot);
                });
            for (BufFile*& file : stream_spools_) {
                hash_major_minimizer_pg_detail::close_spool(file);
            }
            publication_permit_.release();

            const Oid validated_relation_oid = RelationGetRelid(relation_);
            const RelFileLocator validated_locator = relation_->rd_locator;
            Relation finished_relation =
                std::exchange(relation_, nullptr);
            bioseq::postgres::support::invoke_pg_as_cpp(
                [finished_relation] {
                    relation_close(finished_relation, RowExclusiveLock);
                });
            const std::uint64_t head_row_checksum =
                bioseq::indexing::crc64_index_chunk_row(
                    0u,
                    -1,
                    k_hash_major_generation_head_sub_key,
                    0u,
                    static_cast<std::uint32_t>(
                        metadata.encoded_head.size()),
                    metadata.encoded_head);
            state_ = operation_state::sealed;
            minimizer_generation_administration_identity validated_identity =
                std::move(identity_);
            return ready_hash_major_minimizer_token{
                std::move(validated_identity),
                validated_relation_oid,
                validated_locator,
                advisory_lock_key_,
                query_key_,
                generation_id_,
                transaction_id,
                metadata.head,
                metadata.encoded_head,
                head_row_checksum,
            };
        } catch (...) {
            state_ = operation_state::failed;
            abandon_noexcept_();
            throw;
        }
    }
};

static_assert(!std::is_default_constructible_v<
    index_storage_hash_major_minimizer_writer>);
static_assert(!std::is_copy_constructible_v<
    index_storage_hash_major_minimizer_writer>);
static_assert(std::is_nothrow_move_constructible_v<
    index_storage_hash_major_minimizer_writer>);

[[nodiscard]] inline ready_hash_major_minimizer_published_head_receipt
publish_ready_hash_major_minimizer_head_pg(
        ready_hash_major_minimizer_token&& token) {
    Relation relation = nullptr;
    Relation primary_index = nullptr;
    SysScanDesc scan = nullptr;
    HeapTuple replacement = nullptr;
    try {
        if (!token.valid_
            || token.identity_.database_oid != MyDatabaseId
            || token.generation_id_ == 0u
            || token.head_.generation_id != token.generation_id_
            || token.head_.query_key != token.query_key_
            || token.transaction_id_
                != bioseq::postgres::support::invoke_pg_as_cpp(
                    [] { return GetCurrentTransactionIdIfAny(); })
            || token.advisory_lock_key_
                != index_advisory_xact_lock_key(
                    token.identity_.database_oid,
                    token.identity_.tablespace_oid,
                    token.identity_.main_relfilenode,
                    token.identity_.column_attno,
                    token.identity_.index_name)
            || token.head_row_checksum_
                != bioseq::indexing::crc64_index_chunk_row(
                    0u, -1, k_hash_major_generation_head_sub_key, 0u,
                    static_cast<std::uint32_t>(
                        token.head_bytes_.size()),
                    token.head_bytes_)) {
            minimizer_generation_storage_error_as_cpp(
                "hash-major publication token is invalid");
        }
        auto decoded_head =
            bioseq::indexing::decode_hash_major_minimizer_head(
                token.head_bytes_);
        if (!decoded_head || *decoded_head != token.head_) {
            minimizer_generation_storage_error_as_cpp(
                "hash-major publication token head disagrees");
        }
        bioseq::postgres::support::invoke_pg_as_cpp([&] {
            acquire_index_advisory_xact_lock(
                token.identity_.database_oid,
                token.identity_.tablespace_oid,
                token.identity_.main_relfilenode,
                token.identity_.column_attno,
                token.identity_.index_name);
        });
        relation = bioseq::postgres::support::invoke_pg_as_cpp([&] {
            return bioseq::postgres::support::try_open_postgres_relation(
                token.sidecar_relation_oid_, RowExclusiveLock);
        });
        if (relation == nullptr
            || RelationGetNamespace(relation) != bioseq_internal_ns()
            || std::string_view{RelationGetRelationName(relation)}
                != token.identity_.table_name
            || relation->rd_locator.spcOid
                != token.sidecar_locator_.spcOid
            || relation->rd_locator.dbOid
                != token.sidecar_locator_.dbOid
            || relation->rd_locator.relNumber
                != token.sidecar_locator_.relNumber) {
            minimizer_generation_storage_error_as_cpp(
                "hash-major publication relation identity changed");
        }
        const minimizer_generation_schema_inspection schema =
            bioseq::postgres::support::invoke_pg_as_cpp([&] {
                return inspect_minimizer_generation_schema(
                    relation, nullptr,
                    minimizer_generation_schema_expectation{
                        .namespace_oid = bioseq_internal_ns(),
                        .relation_name = token.identity_.table_name,
                        .tablespace_oid = token.identity_.tablespace_oid,
                    });
            });
        if (!schema) {
            minimizer_generation_storage_error_as_cpp(
                "hash-major publication relation schema changed");
        }
        primary_index = bioseq::postgres::support::invoke_pg_as_cpp(
            [&] { return index_open(
                schema.primary_index_oid, AccessShareLock); });
        replacement = hash_major_minimizer_pg_detail::form_row(
            relation,
            token.query_key_,
            index_chunk_address{
                .generation_id = 0u,
                .position_segment = -1,
                .sub_key = k_hash_major_generation_head_sub_key,
                .chunk_no = 0u,
            },
            token.head_bytes_,
            token.head_row_checksum_);

        ScanKeyData keys[5];
        ScanKeyInit(&keys[0], k_minimizer_generation_index_generation_id,
            BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum(0));
        ScanKeyInit(&keys[1], k_minimizer_generation_index_query_key,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(
                token.query_key_)));
        ScanKeyInit(&keys[2], k_minimizer_generation_index_sub_key,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(
                k_hash_major_generation_head_sub_key)));
        ScanKeyInit(&keys[3], k_minimizer_generation_index_position_segment,
            BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(-1));
        ScanKeyInit(&keys[4], k_minimizer_generation_index_chunk_number,
            BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(0));
        scan = bioseq::postgres::support::invoke_pg_as_cpp([&] {
            return systable_beginscan_ordered(
                relation, primary_index, SnapshotSelf, 5, keys);
        });
        HeapTuple previous_tuple =
            bioseq::postgres::support::invoke_pg_as_cpp([&] {
                return systable_getnext_ordered(
                    scan, ForwardScanDirection);
            });
        std::optional<std::uint64_t> replaced_generation_id;
        ItemPointerData previous_tid{};
        bool replace = HeapTupleIsValid(previous_tuple);
        if (replace) {
            previous_tid = previous_tuple->t_self;
            minimizer_generation_loaded_row previous =
                load_minimizer_generation_row_or_error(relation, previous_tuple);
            if (previous.query_key != token.query_key_
                || previous.address != index_chunk_address{
                    .generation_id = 0u,
                    .position_segment = -1,
                    .sub_key = k_hash_major_generation_head_sub_key,
                    .chunk_no = 0u,
                }
                || previous.bytes.size()
                    != k_hash_major_minimizer_head_bytes) {
                previous.release();
                minimizer_generation_storage_error_as_cpp(
                    "previous hash-major head row is malformed");
            }
            auto previous_head =
                bioseq::indexing::decode_hash_major_minimizer_head(
                    previous.bytes);
            previous.release();
            if (!previous_head
                || previous_head->generation_id == 0u
                || previous_head->generation_id
                    == token.generation_id_) {
                minimizer_generation_storage_error_as_cpp(
                    "previous hash-major head generation is invalid");
            }
            replaced_generation_id =
                previous_head->generation_id;
        }
        HeapTuple extra = bioseq::postgres::support::invoke_pg_as_cpp([&] {
            return systable_getnext_ordered(
                scan, ForwardScanDirection);
        });
        bioseq::postgres::support::invoke_pg_as_cpp(
            [&] { systable_endscan_ordered(scan); });
        scan = nullptr;
        if (HeapTupleIsValid(extra)) {
            minimizer_generation_storage_error_as_cpp(
                "hash-major head primary key returned duplicates");
        }
        if (replace) {
            bioseq::postgres::support::invoke_pg_as_cpp([&] {
                CatalogTupleUpdate(
                    relation, &previous_tid, replacement);
            });
        } else {
            bioseq::postgres::support::invoke_pg_as_cpp([&] {
                CatalogTupleInsert(relation, replacement);
            });
        }
        heap_freetuple(replacement);
        replacement = nullptr;
        bioseq::postgres::support::invoke_pg_as_cpp(
            [] { CommandCounterIncrement(); });

        // systable_beginscan_ordered() rewrites heap attribute numbers in
        // place to index ordinals. Reinitialize before reusing this array;
        // passing the already-remapped keys through a second time produces
        // the non-monotone ordinal sequence 2,1,4,3,5 for this primary key.
        ScanKeyInit(&keys[0], k_minimizer_generation_index_generation_id,
            BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum(0));
        ScanKeyInit(&keys[1], k_minimizer_generation_index_query_key,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(
                token.query_key_)));
        ScanKeyInit(&keys[2], k_minimizer_generation_index_sub_key,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(
                k_hash_major_generation_head_sub_key)));
        ScanKeyInit(&keys[3], k_minimizer_generation_index_position_segment,
            BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(-1));
        ScanKeyInit(&keys[4], k_minimizer_generation_index_chunk_number,
            BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(0));
        scan = bioseq::postgres::support::invoke_pg_as_cpp([&] {
            return systable_beginscan_ordered(
                relation, primary_index, SnapshotSelf, 5, keys);
        });
        HeapTuple published_tuple =
            bioseq::postgres::support::invoke_pg_as_cpp([&] {
                return systable_getnext_ordered(
                    scan, ForwardScanDirection);
            });
        if (!HeapTupleIsValid(published_tuple)) {
            minimizer_generation_storage_error_as_cpp(
                "published hash-major head is missing");
        }
        minimizer_generation_loaded_row published =
            load_minimizer_generation_row_or_error(relation, published_tuple);
        if (published.query_key != token.query_key_
            || published.address != index_chunk_address{
                .generation_id = 0u,
                .position_segment = -1,
                .sub_key = k_hash_major_generation_head_sub_key,
                .chunk_no = 0u,
            }
            || !std::ranges::equal(
                published.bytes, token.head_bytes_)) {
            published.release();
            minimizer_generation_storage_error_as_cpp(
                "published hash-major head differs from its token");
        }
        published.release();
        HeapTuple published_extra =
            bioseq::postgres::support::invoke_pg_as_cpp([&] {
                return systable_getnext_ordered(
                    scan, ForwardScanDirection);
            });
        bioseq::postgres::support::invoke_pg_as_cpp(
            [&] { systable_endscan_ordered(scan); });
        scan = nullptr;
        if (HeapTupleIsValid(published_extra)) {
            minimizer_generation_storage_error_as_cpp(
                "published hash-major head is not unique");
        }
        Relation finished_index =
            std::exchange(primary_index, nullptr);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [finished_index] {
                index_close(finished_index, AccessShareLock);
            });
        Relation finished_relation =
            std::exchange(relation, nullptr);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [finished_relation] {
                relation_close(finished_relation, RowExclusiveLock);
            });
        ready_hash_major_minimizer_published_head_receipt receipt{
            .table_name = token.identity_.table_name,
            .query_key = token.query_key_,
            .head = token.head_,
            .head_bytes = token.head_bytes_,
            .replaced_generation_id = replaced_generation_id,
        };
        token.invalidate_();
        return receipt;
    } catch (...) {
        // Do not invoke PostgreSQL cleanup while propagating a translated
        // ERROR. The current memory context and ResourceOwner retain every
        // outstanding tuple/scan/relation object until statement abort.
        replacement = nullptr;
        scan = nullptr;
        primary_index = nullptr;
        relation = nullptr;
        token.invalidate_();
        throw;
    }
}

/// One hash-major read session owns the exact current-generation heap/primary-index
/// descriptors and borrows one already-registered executor MVCC snapshot.
/// Metadata discovery, source authorization, and every logical stream callback
/// therefore observe one relation incarnation and one snapshot.
struct hash_major_minimizer_catalog_stream_receipt {
    hash_major_minimizer_stream_kind kind =
        hash_major_minimizer_stream_kind::directory;
    std::uint64_t exact_catalog_rows = 0;
    std::uint64_t exact_catalog_bytes = 0;
    std::uint64_t exact_catalog_stream_crc64 = 0;
};

/// Test-only authorization returned by the hash-major parallel heap-scan experiment.
/// The production reader does not consult this object or switch to this path.
struct hash_major_minimizer_parallel_scan_stats {
    std::uint32_t requested_workers = 0;
    std::uint32_t launched_workers = 0;
    std::uint64_t generation_id = 0;
    std::uint64_t query_key = 0;
    std::uint64_t expected_rows = 0;
    std::uint64_t scanned_rows = 0;
    std::uint64_t expected_bytes = 0;
    std::uint64_t scanned_bytes = 0;
    std::uint64_t duplicate_rows = 0;
    std::uint64_t missing_rows = 0;
    std::uint64_t corrupt_rows = 0;
    std::uint64_t elapsed_ns = 0;
    std::uint64_t checksum_digest = 0;
    std::uint64_t receipt_digest = 0;
    std::uint64_t image_bytes = 0;
    std::uint64_t image_build_ns = 0;
    std::uint64_t mmap_elapsed_ns = 0;
    std::uint64_t mmap_scanned_rows = 0;
    std::uint64_t mmap_scanned_bytes = 0;
    std::uint64_t mmap_checksum_digest = 0;
    std::uint64_t mmap_receipt_digest = 0;
    bool has_leader_participation = false;
    bool mmap_advice_applied = false;
};

namespace hash_major_minimizer_parallel_probe_detail {

inline constexpr std::uint64_t k_toc_magic =
    UINT64_C(0x4236534350524f42); // "B6SCPROB"
inline constexpr std::uint64_t k_header_toc_key =
    UINT64_C(0x4236534300000001);
inline constexpr std::uint64_t k_parallel_scan_toc_key =
    UINT64_C(0x4236534300000002);
inline constexpr std::uint64_t k_seen_bitset_toc_key =
    UINT64_C(0x4236534300000003);
inline constexpr std::uint32_t k_parallel_header_format_revision = 1u;

struct parallel_scan_stream {
    std::int64_t sub_key = 0;
    std::uint64_t logical_bytes = 0;
    std::uint64_t row_count = 0;
    std::uint64_t global_row_base = 0;
};

/// Pointer-free DSM state. Workers authenticate rows and publish only
/// commutative counters plus one exact expected-row bit per accepted tuple.
struct parallel_scan_header {
    std::uint64_t magic = 0;
    std::uint32_t format_revision = 0;
    std::uint32_t requested_workers = 0;
    Oid relation_oid = InvalidOid;
    RelFileLocator relation_locator{};
    std::uint64_t generation_id = 0;
    std::uint64_t query_key = 0;
    std::uint64_t expected_rows = 0;
    std::uint64_t expected_bytes = 0;
    std::uint64_t bitset_word_count = 0;
    std::array<parallel_scan_stream, 3> streams{};
    pg_atomic_uint64 scanned_rows{};
    pg_atomic_uint64 scanned_bytes{};
    pg_atomic_uint64 duplicate_rows{};
    pg_atomic_uint64 corrupt_rows{};
    pg_atomic_uint64 checksum_digest{};
    pg_atomic_uint64 receipt_digest{};
};

static_assert(std::is_trivially_destructible_v<parallel_scan_header>);
static_assert(std::is_standard_layout_v<parallel_scan_header>);

inline constexpr std::uint64_t k_image_magic =
    UINT64_C(0x42364636494d4147); // "B6F6IMAG"
inline constexpr std::uint32_t k_image_format_revision = 1u;
inline constexpr std::uint64_t k_image_section_alignment = 4096u;

struct parallel_image_stream {
    std::int64_t sub_key = 0;
    std::uint64_t logical_bytes = 0;
    std::uint64_t row_count = 0;
    std::uint64_t global_row_base = 0;
    std::uint64_t payload_offset = 0;
};

/// Offset-only test image written to a PostgreSQL-owned temporary file.
struct parallel_image_header {
    std::uint64_t magic = 0;
    std::uint32_t format_revision = 0;
    std::uint32_t header_bytes = 0;
    std::uint64_t generation_id = 0;
    std::uint64_t query_key = 0;
    std::uint64_t expected_rows = 0;
    std::uint64_t expected_bytes = 0;
    std::uint64_t receipts_offset = 0;
    std::uint64_t payload_offset = 0;
    std::uint64_t image_bytes = 0;
    std::array<parallel_image_stream, 3> streams{};
    std::uint64_t header_crc64 = 0;
};

static_assert(std::is_trivially_copyable_v<parallel_image_header>);
static_assert(std::is_standard_layout_v<parallel_image_header>);

struct parallel_image_write_target {
    File file = -1;
    const parallel_image_header* header = nullptr;
};

struct parallel_image_validation {
    std::uint64_t elapsed_ns = 0;
    std::uint64_t rows = 0;
    std::uint64_t bytes = 0;
    std::uint64_t checksum_digest = 0;
    std::uint64_t receipt_digest = 0;
    bool advice_applied = false;
};

[[nodiscard]] inline std::uint64_t mix64(
        std::uint64_t value) noexcept {
    value += UINT64_C(0x9e3779b97f4a7c15);
    value = (value ^ (value >> 30u))
        * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27u))
        * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31u);
}

[[nodiscard]] inline std::uint64_t align_image_offset(
        std::uint64_t value) {
    if (value > (std::numeric_limits<std::uint64_t>::max)()
            - (k_image_section_alignment - 1u)) {
        minimizer_generation_storage_error_as_cpp(
            "hash-major mmap image offset overflows");
    }
    return (value + k_image_section_alignment - 1u)
        / k_image_section_alignment * k_image_section_alignment;
}

[[nodiscard]] inline std::uint64_t image_header_crc(
        const parallel_image_header& header) noexcept {
    parallel_image_header copy = header;
    copy.header_crc64 = 0;
    crc64_ecma182_accumulator crc{};
    crc.update(std::as_bytes(
        std::span{&copy, std::size_t{1}}));
    return crc.checksum();
}

[[noreturn]] inline void corrupt_row_pg_leaf(
        parallel_scan_header* header,
        const char* detail) {
    if (header != nullptr) {
        (void)pg_atomic_fetch_add_u64(
            &header->corrupt_rows, 1);
    }
    ereport(ERROR,
        (errcode(ERRCODE_DATA_CORRUPTED),
         errmsg("bioseq hash-major parallel scan probe rejected a row"),
         errdetail_internal("%s", detail)));
    pg_unreachable();
}

[[noreturn]] inline void duplicate_row_pg_leaf(
        parallel_scan_header* header,
        std::uint64_t global_row) {
    (void)pg_atomic_fetch_add_u64(
        &header->duplicate_rows, 1);
    ereport(ERROR,
        (errcode(ERRCODE_DATA_CORRUPTED),
         errmsg("bioseq hash-major parallel scan probe found a duplicate row"),
         errdetail_internal(
            "global_row=%llu",
            static_cast<unsigned long long>(global_row))));
    pg_unreachable();
}

[[nodiscard]] inline int stream_index_for_sub_key(
        const parallel_scan_header& header,
        std::int64_t sub_key) noexcept {
    for (std::size_t index = 0; index < header.streams.size();
         ++index) {
        if (header.streams[index].sub_key == sub_key) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

inline void scan_table_desc_pg_leaf(
        Relation relation,
        TableScanDesc scan,
        parallel_scan_header* header,
        pg_atomic_uint64* seen_words) {
    if (relation == nullptr || scan == nullptr || header == nullptr
        || seen_words == nullptr
        || header->magic != k_toc_magic
        || header->format_revision != k_parallel_header_format_revision
        || RelationGetRelid(relation) != header->relation_oid
        || !RelFileLocatorEquals(
            relation->rd_locator, header->relation_locator)
        || RelationGetDescr(relation) == nullptr
        || RelationGetDescr(relation)->natts
            != static_cast<int>(
                k_minimizer_generation_index_attribute_descriptors.size())) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq hash-major parallel scan probe binding changed")));
    }

    TupleTableSlot* slot = table_slot_create(relation, nullptr);
    std::uint64_t local_rows = 0;
    std::uint64_t local_bytes = 0;
    std::uint64_t local_checksum_digest = 0;
    std::uint64_t local_receipt_digest = 0;
    std::uint64_t visited_rows = 0;

    while (table_scan_getnextslot(
            scan, ForwardScanDirection, slot)) {
        ++visited_rows;
        slot_getallattrs(slot);
        Datum* values = slot->tts_values;
        bool* nulls = slot->tts_isnull;

        // Filter on the four fixed identity columns before inspecting BYTEA.
        // Non-active generations and metadata rows are deliberately outside this
        // probe's corruption authority.
        if (!nulls[k_minimizer_generation_attribute_generation_id - 1]
            && !nulls[k_minimizer_generation_attribute_query_key - 1]
            && !nulls[k_minimizer_generation_attribute_position_segment - 1]
            && !nulls[k_minimizer_generation_attribute_sub_key - 1]
            && DatumGetInt64(values[k_minimizer_generation_attribute_generation_id - 1])
                == static_cast<std::int64_t>(
                    header->generation_id)
            && DatumGetInt64(values[k_minimizer_generation_attribute_query_key - 1])
                == static_cast<std::int64_t>(header->query_key)
            && DatumGetInt32(
                    values[k_minimizer_generation_attribute_position_segment - 1]) == 0) {
            const std::int64_t sub_key =
                DatumGetInt64(values[k_minimizer_generation_attribute_sub_key - 1]);
            const int stream_index =
                stream_index_for_sub_key(*header, sub_key);
            if (stream_index >= 0) {
                minimizer_generation_deformed_row deformed{};
                for (std::size_t index = 0;
                     index < deformed.values.size(); ++index) {
                    deformed.values[index] = values[index];
                    deformed.nulls[index] = nulls[index];
                }
                const minimizer_generation_projected_row projected =
                    project_minimizer_generation_deformed_row(deformed);
                if (projected.error != nullptr) {
                    corrupt_row_pg_leaf(header, projected.error);
                }
                const parallel_scan_stream& stream =
                    header->streams[
                        static_cast<std::size_t>(stream_index)];
                const std::uint64_t row_ordinal =
                    projected.address.chunk_no;
                if (projected.query_key != header->query_key
                    || projected.address.generation_id
                        != header->generation_id
                    || projected.address.position_segment != 0
                    || projected.address.sub_key != stream.sub_key
                    || row_ordinal >= stream.row_count
                    || row_ordinal
                        > (std::numeric_limits<std::uint64_t>::max)()
                            / k_hash_major_minimizer_row_payload_bytes) {
                    corrupt_row_pg_leaf(
                        header, "row identity or ordinal disagrees");
                }
                const std::uint64_t row_offset =
                    row_ordinal * k_hash_major_minimizer_row_payload_bytes;
                if (row_offset >= stream.logical_bytes) {
                    corrupt_row_pg_leaf(
                        header, "row offset exceeds logical stream");
                }
                const std::uint64_t remaining =
                    stream.logical_bytes - row_offset;
                const std::size_t expected_bytes =
                    static_cast<std::size_t>(
                        std::min<std::uint64_t>(
                            remaining,
                            k_hash_major_minimizer_row_payload_bytes));
                if (projected.bytes.size() != expected_bytes
                    || projected.chunk_bytes != expected_bytes) {
                    corrupt_row_pg_leaf(
                        header, "PLAIN row length disagrees");
                }
                const std::uint64_t expected_checksum =
                    bioseq::indexing::crc64_index_chunk_row(
                        projected.address.generation_id,
                        projected.address.position_segment,
                        projected.address.sub_key,
                        projected.address.chunk_no,
                        projected.chunk_bytes,
                        projected.bytes);
                if (projected.checksum != expected_checksum) {
                    corrupt_row_pg_leaf(
                        header,
                        "address-bound row CRC disagrees");
                }

                const std::uint64_t global_row =
                    stream.global_row_base + row_ordinal;
                if (global_row >= header->expected_rows) {
                    corrupt_row_pg_leaf(
                        header, "global row ordinal overflows");
                }
                const std::uint64_t word_ordinal = global_row / 64u;
                const std::uint64_t bit =
                    std::uint64_t{1} << (global_row % 64u);
                if (word_ordinal >= header->bitset_word_count) {
                    corrupt_row_pg_leaf(
                        header, "seen-row bitset is undersized");
                }
                const std::uint64_t previous =
                    pg_atomic_fetch_or_u64(
                        &seen_words[word_ordinal], bit);
                if ((previous & bit) != 0u) {
                    duplicate_row_pg_leaf(header, global_row);
                }

                ++local_rows;
                local_bytes += projected.bytes.size();
                local_checksum_digest +=
                    mix64(projected.checksum);
                local_receipt_digest += mix64(
                    projected.checksum
                    ^ mix64(projected.address.generation_id)
                    ^ mix64(projected.query_key)
                    ^ mix64(static_cast<std::uint64_t>(
                        projected.address.sub_key))
                    ^ mix64(row_ordinal)
                    ^ mix64(projected.chunk_bytes));
            }
        }

        ExecClearTuple(slot);
        if ((visited_rows & 1023u) == 0u) {
            if (!IsParallelWorker() && ParallelMessagePending) {
                HandleParallelMessages();
            }
            CHECK_FOR_INTERRUPTS();
        }
    }

    ExecDropSingleTupleTableSlot(slot);
    (void)pg_atomic_fetch_add_u64(
        &header->scanned_rows,
        static_cast<std::int64_t>(local_rows));
    (void)pg_atomic_fetch_add_u64(
        &header->scanned_bytes,
        static_cast<std::int64_t>(local_bytes));
    (void)pg_atomic_fetch_add_u64(
        &header->checksum_digest,
        std::bit_cast<std::int64_t>(local_checksum_digest));
    (void)pg_atomic_fetch_add_u64(
        &header->receipt_digest,
        std::bit_cast<std::int64_t>(local_receipt_digest));
}

inline void serial_scan_pg_leaf(
        Relation relation,
        Snapshot snapshot,
        parallel_scan_header* header,
        pg_atomic_uint64* seen_words) {
    TableScanDesc scan =
        table_beginscan(relation, snapshot, 0, nullptr);
    scan_table_desc_pg_leaf(
        relation, scan, header, seen_words);
    table_endscan(scan);
}

inline void parallel_scan_pg_leaf(
        Relation relation,
        ParallelTableScanDesc parallel_scan,
        parallel_scan_header* header,
        pg_atomic_uint64* seen_words) {
    TableScanDesc scan =
        table_beginscan_parallel(relation, parallel_scan);
    scan_table_desc_pg_leaf(
        relation, scan, header, seen_words);
    table_endscan(scan);
}

inline void initialize_header(
        parallel_scan_header* header,
        Relation relation,
        const hash_major_minimizer_manifest& manifest,
        std::uint64_t query_key,
        std::uint32_t requested_workers) {
    if (header == nullptr || relation == nullptr
        || manifest.generation_id == 0u
        || !bioseq::indexing::sql_bigint_can_represent(
            manifest.generation_id)
        || !bioseq::indexing::sql_bigint_can_represent(query_key)) {
        minimizer_generation_storage_error_as_cpp(
            "hash-major parallel scan probe header is invalid");
    }
    std::memset(header, 0, sizeof(*header));
    header->magic = k_toc_magic;
    header->format_revision = k_parallel_header_format_revision;
    header->requested_workers = requested_workers;
    header->relation_oid = RelationGetRelid(relation);
    header->relation_locator = relation->rd_locator;
    header->generation_id = manifest.generation_id;
    header->query_key = query_key;

    std::uint64_t row_base = 0;
    std::uint64_t total_bytes = 0;
    for (std::size_t index = 0;
         index < header->streams.size(); ++index) {
        const hash_major_minimizer_stream_kind expected_kind =
            hash_major_minimizer_pg_detail::stream_kind(index);
        const auto& descriptor = manifest.streams[index];
        const std::uint64_t expected_rows =
            hash_major_minimizer_pg_detail::ceil_rows(
                descriptor.logical_bytes);
        if (descriptor.kind != expected_kind
            || descriptor.row_count != expected_rows
            || descriptor.row_count
                > static_cast<std::uint64_t>(
                    (std::numeric_limits<std::int32_t>::max)())
            || descriptor.row_count
                > (std::numeric_limits<std::uint64_t>::max)()
                    - row_base
            || descriptor.logical_bytes
                > (std::numeric_limits<std::uint64_t>::max)()
                    - total_bytes) {
            minimizer_generation_storage_error_as_cpp(
                "hash-major parallel scan probe stream geometry is invalid");
        }
        header->streams[index] = parallel_scan_stream{
            .sub_key =
                hash_major_minimizer_pg_detail::stream_sub_key(expected_kind),
            .logical_bytes = descriptor.logical_bytes,
            .row_count = descriptor.row_count,
            .global_row_base = row_base,
        };
        row_base += descriptor.row_count;
        total_bytes += descriptor.logical_bytes;
    }
    header->expected_rows = row_base;
    header->expected_bytes = total_bytes;
    header->bitset_word_count =
        row_base / 64u + (row_base % 64u != 0u ? 1u : 0u);
    if (header->bitset_word_count == 0u
        || header->bitset_word_count
            > static_cast<std::uint64_t>(
                MaxAllocSize / sizeof(pg_atomic_uint64))) {
        minimizer_generation_storage_error_as_cpp(
            "hash-major parallel scan probe bitset geometry is invalid");
    }
    pg_atomic_init_u64(&header->scanned_rows, 0);
    pg_atomic_init_u64(&header->scanned_bytes, 0);
    pg_atomic_init_u64(&header->duplicate_rows, 0);
    pg_atomic_init_u64(&header->corrupt_rows, 0);
    pg_atomic_init_u64(&header->checksum_digest, 0);
    pg_atomic_init_u64(&header->receipt_digest, 0);
}

inline void initialize_seen_words(
        pg_atomic_uint64* words,
        std::uint64_t word_count) {
    if (words == nullptr || word_count == 0u) {
        minimizer_generation_storage_error_as_cpp(
            "hash-major parallel scan probe bitset is invalid");
    }
    for (std::uint64_t index = 0; index < word_count; ++index) {
        pg_atomic_init_u64(&words[index], 0);
    }
}

[[nodiscard]] inline hash_major_minimizer_parallel_scan_stats
finalize_result(
        parallel_scan_header& header,
        pg_atomic_uint64* seen_words,
        std::uint32_t launched_workers,
        std::uint64_t elapsed_ns) {
    std::uint64_t seen_rows = 0;
    for (std::uint64_t index = 0;
         index < header.bitset_word_count; ++index) {
        std::uint64_t word =
            pg_atomic_read_u64(&seen_words[index]);
        if (index + 1u == header.bitset_word_count
            && header.expected_rows % 64u != 0u) {
            word &= (std::uint64_t{1}
                    << (header.expected_rows % 64u))
                - 1u;
        }
        seen_rows += std::popcount(word);
    }
    const std::uint64_t scanned_rows =
        pg_atomic_read_u64(&header.scanned_rows);
    const std::uint64_t scanned_bytes =
        pg_atomic_read_u64(&header.scanned_bytes);
    const std::uint64_t duplicates =
        pg_atomic_read_u64(&header.duplicate_rows);
    const std::uint64_t corrupt =
        pg_atomic_read_u64(&header.corrupt_rows);
    const std::uint64_t missing =
        seen_rows <= header.expected_rows
            ? header.expected_rows - seen_rows
            : header.expected_rows;
    if (duplicates != 0u || corrupt != 0u || missing != 0u
        || seen_rows != header.expected_rows
        || scanned_rows != header.expected_rows
        || scanned_bytes != header.expected_bytes) {
        minimizer_generation_storage_error_as_cpp(fmt::format(
            "hash-major parallel scan probe receipt equation failed: "
            "expected_rows={} seen_rows={} scanned_rows={} "
            "expected_bytes={} scanned_bytes={} duplicates={} "
            "missing={} corrupt={}",
            header.expected_rows, seen_rows, scanned_rows,
            header.expected_bytes, scanned_bytes, duplicates,
            missing, corrupt));
    }
    return hash_major_minimizer_parallel_scan_stats{
        .requested_workers = header.requested_workers,
        .launched_workers = launched_workers,
        .generation_id = header.generation_id,
        .query_key = header.query_key,
        .expected_rows = header.expected_rows,
        .scanned_rows = scanned_rows,
        .expected_bytes = header.expected_bytes,
        .scanned_bytes = scanned_bytes,
        .duplicate_rows = duplicates,
        .missing_rows = missing,
        .corrupt_rows = corrupt,
        .elapsed_ns = elapsed_ns,
        .checksum_digest =
            pg_atomic_read_u64(&header.checksum_digest),
        .receipt_digest =
            pg_atomic_read_u64(&header.receipt_digest),
        .has_leader_participation = true,
    };
}

[[nodiscard]] inline hash_major_minimizer_parallel_scan_stats
run_probe(
        Relation relation,
        Snapshot snapshot,
        const hash_major_minimizer_manifest& manifest,
        std::uint64_t query_key,
        std::uint32_t requested_workers) {
    if (relation == nullptr || snapshot == nullptr
        || !IsMVCCSnapshot(snapshot)
        || snapshot->regd_count == 0) {
        minimizer_generation_storage_error_as_cpp(
            "hash-major parallel scan probe requires a registered MVCC snapshot");
    }

    if (requested_workers == 0u) {
        parallel_scan_header header{};
        initialize_header(
            &header, relation, manifest, query_key, 0u);
        const Size bitset_bytes = static_cast<Size>(
            header.bitset_word_count
            * sizeof(pg_atomic_uint64));
        auto* seen_words =
            static_cast<pg_atomic_uint64*>(
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [bitset_bytes] {
                        return palloc0(bitset_bytes);
                    }));
        initialize_seen_words(
            seen_words, header.bitset_word_count);
        instr_time begin{};
        instr_time end{};
        INSTR_TIME_SET_CURRENT(begin);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [&] {
                serial_scan_pg_leaf(
                    relation, snapshot, &header, seen_words);
            });
        INSTR_TIME_SET_CURRENT(end);
        INSTR_TIME_SUBTRACT(end, begin);
        hash_major_minimizer_parallel_scan_stats result =
            finalize_result(
                header, seen_words, 0u,
                INSTR_TIME_GET_NANOSEC(end));
        bioseq::postgres::support::invoke_pg_as_cpp(
            [seen_words] { pfree(seen_words); });
        return result;
    }

    if (IsParallelWorker() || IsInParallelMode()
        || ParallelContextActive()) {
        minimizer_generation_storage_error_as_cpp(
            "hash-major parallel scan probe cannot nest a parallel context");
    }
    const Size parallel_scan_bytes =
        bioseq::postgres::support::invoke_pg_as_cpp(
            [&] {
                return table_parallelscan_estimate(
                    relation, snapshot);
            });
    if (parallel_scan_bytes == 0u) {
        minimizer_generation_storage_error_as_cpp(
            "hash-major parallel scan probe received an empty scan descriptor");
    }

    ParallelContext* context = nullptr;
    bool entered_parallel_mode = false;
    instr_time begin{};
    instr_time end{};
    INSTR_TIME_SET_CURRENT(begin);
    try {
        EnterParallelMode();
        entered_parallel_mode = true;
        context = bioseq::postgres::support::invoke_pg_as_cpp(
            [&] {
                return CreateParallelContext(
                    "bioseq",
                    "bioseq_hash_major_parallel_scan_probe_worker_main",
                    static_cast<int>(requested_workers));
            });
        if (context == nullptr) {
            minimizer_generation_storage_error_as_cpp(
                "hash-major parallel scan probe could not create a parallel context");
        }
        bioseq::postgres::support::invoke_pg_as_cpp(
            [&] {
                shm_toc_estimate_chunk(
                    &context->estimator, sizeof(parallel_scan_header));
                shm_toc_estimate_chunk(
                    &context->estimator, parallel_scan_bytes);
                // The exact size becomes known only after the manifest-backed
                // header is initialized. Its upper bound is one atomic word
                // per 64 logical rows.
                std::uint64_t expected_rows = 0;
                for (const auto& descriptor : manifest.streams) {
                    if (descriptor.row_count
                        > (std::numeric_limits<std::uint64_t>::max)()
                            - expected_rows) {
                        ereport(ERROR,
                            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                             errmsg("bioseq hash-major parallel scan row count overflows")));
                    }
                    expected_rows += descriptor.row_count;
                }
                const std::uint64_t word_count =
                    expected_rows / 64u
                    + (expected_rows % 64u != 0u ? 1u : 0u);
                if (word_count == 0u
                    || word_count
                        > static_cast<std::uint64_t>(
                            MaxAllocSize
                            / sizeof(pg_atomic_uint64))) {
                    ereport(ERROR,
                        (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                         errmsg("bioseq hash-major parallel scan bitset is too large")));
                }
                shm_toc_estimate_chunk(
                    &context->estimator,
                    static_cast<Size>(
                        word_count
                        * sizeof(pg_atomic_uint64)));
                shm_toc_estimate_keys(&context->estimator, 3);
                InitializeParallelDSM(context);
            });
        if (context->toc == nullptr) {
            minimizer_generation_storage_error_as_cpp(
                "hash-major parallel scan probe has no DSM table of contents");
        }

        auto* header = static_cast<parallel_scan_header*>(
            bioseq::postgres::support::invoke_pg_as_cpp(
                [&] {
                    return shm_toc_allocate(
                        context->toc, sizeof(parallel_scan_header));
                }));
        initialize_header(
            header, relation, manifest, query_key,
            requested_workers);
        const Size bitset_bytes = static_cast<Size>(
            header->bitset_word_count
            * sizeof(pg_atomic_uint64));
        auto* parallel_scan =
            static_cast<ParallelTableScanDesc>(
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [&] {
                        return shm_toc_allocate(
                            context->toc,
                            parallel_scan_bytes);
                    }));
        auto* seen_words =
            static_cast<pg_atomic_uint64*>(
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [&] {
                        return shm_toc_allocate(
                            context->toc, bitset_bytes);
                    }));
        initialize_seen_words(
            seen_words, header->bitset_word_count);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [&] {
                table_parallelscan_initialize(
                    relation, parallel_scan, snapshot);
                shm_toc_insert(
                    context->toc, k_header_toc_key, header);
                shm_toc_insert(
                    context->toc, k_parallel_scan_toc_key,
                    parallel_scan);
                shm_toc_insert(
                    context->toc, k_seen_bitset_toc_key,
                    seen_words);
                LaunchParallelWorkers(context);
                WaitForParallelWorkersToAttach(context);
            });
        const std::uint32_t launched_workers =
            static_cast<std::uint32_t>(
                std::max(context->nworkers_launched, 0));

        // The leader is an equal consumer of the shared block allocator.
        bioseq::postgres::support::invoke_pg_as_cpp(
            [&] {
                parallel_scan_pg_leaf(
                    relation, parallel_scan,
                    header, seen_words);
                WaitForParallelWorkersToFinish(context);
            });
        INSTR_TIME_SET_CURRENT(end);
        INSTR_TIME_SUBTRACT(end, begin);
        hash_major_minimizer_parallel_scan_stats result =
            finalize_result(
                *header, seen_words, launched_workers,
                INSTR_TIME_GET_NANOSEC(end));
        bioseq::postgres::support::invoke_pg_as_cpp(
            [&] { DestroyParallelContext(context); });
        context = nullptr;
        ExitParallelMode();
        entered_parallel_mode = false;
        return result;
    } catch (...) {
        if (context != nullptr) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [&] { DestroyParallelContext(context); });
            context = nullptr;
        }
        if (entered_parallel_mode) {
            ExitParallelMode();
            entered_parallel_mode = false;
        }
        throw;
    }
}

} // namespace hash_major_minimizer_parallel_probe_detail

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
    std::size_t cached_boundary_slot_ordinal_ =
        k_minimizer_generation_full_scan_pinned_batch_rows;
    minimizer_generation_projected_row cached_boundary_row_{};
    Snapshot snapshot_ = nullptr;
    std::uint64_t query_key_ = 0u;
    Oid expected_tablespace_oid_ = InvalidOid;
    operation_state state_ = operation_state::opened;
    encoded_hash_major_minimizer_head encoded_head_{};
    encoded_hash_major_minimizer_manifest encoded_manifest_{};
    std::optional<hash_major_minimizer_head> head_{};
    std::optional<hash_major_minimizer_manifest> manifest_{};
    std::optional<expected_hash_major_minimizer_generation>
        expected_generation_{};
    std::array<std::byte, k_hash_major_minimizer_maximum_control_block_bytes>
        control_wave_scratch_{};
    minimizer_generation_read_stats stats_{};

    [[noreturn]] void fail_(std::string_view message) {
        state_ = operation_state::failed;
        minimizer_generation_storage_error_as_cpp(message);
    }

    void create_row_batch_slots_checked_() {
        bioseq::postgres::support::invoke_pg_as_cpp(
            [this] {
                while (row_batch_slot_count_
                       < row_batch_slots_.size()) {
                    TupleTableSlot* slot =
                        table_slot_create(heap_relation_, nullptr);
                    Assert(TTS_IS_BUFFERTUPLE(slot));
                    row_batch_slots_[row_batch_slot_count_] = slot;
                    ++row_batch_slot_count_;
                }
            });
    }

    void invalidate_cached_boundary_row_() noexcept {
        cached_boundary_row_valid_ = false;
        cached_boundary_row_kind_ =
            hash_major_minimizer_stream_kind::directory;
        cached_boundary_row_ordinal_ = 0u;
        cached_boundary_slot_ordinal_ =
            row_batch_slots_.size();
        cached_boundary_row_ = {};
    }

    void clear_row_batch_slots_checked_() {
        if (row_batch_slot_count_ == 0u) return;
        invalidate_cached_boundary_row_();
        bioseq::postgres::support::invoke_pg_as_cpp(
            [this] {
                for (std::size_t ordinal = 0u;
                     ordinal < row_batch_slot_count_; ++ordinal) {
                    TupleTableSlot* slot = row_batch_slots_[ordinal];
                    Assert(slot != nullptr);
                    ExecClearTuple(slot);
                }
            });
    }

    /// Retain only the final validated row of a selective scan. Consecutive
    /// scheduler waves commonly split inside one 8,064-byte catalog row; its
    /// pinned projection can serve the next wave before any slot is advanced.
    /// All other batch pins are released immediately.
    void clear_row_batch_slots_except_checked_(
            std::size_t retained_ordinal) {
        if (!cached_boundary_row_valid_
            || retained_ordinal >= row_batch_slot_count_) {
            fail_("hash-major boundary-row cache slot is invalid");
        }
        bioseq::postgres::support::invoke_pg_as_cpp(
            [this, retained_ordinal] {
                for (std::size_t ordinal = 0u;
                     ordinal < row_batch_slot_count_; ++ordinal) {
                    if (ordinal == retained_ordinal) continue;
                    TupleTableSlot* slot = row_batch_slots_[ordinal];
                    Assert(slot != nullptr);
                    ExecClearTuple(slot);
                }
            });
    }

    void drop_row_batch_slots_checked_() {
        if (row_batch_slot_count_ == 0u) return;
        invalidate_cached_boundary_row_();
        bioseq::postgres::support::invoke_pg_as_cpp(
            [this] {
                while (row_batch_slot_count_ != 0u) {
                    const std::size_t ordinal =
                        row_batch_slot_count_ - 1u;
                    TupleTableSlot* slot =
                        row_batch_slots_[ordinal];
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
                [&] { systable_endscan_ordered(active_scan_); });
            active_scan_ = nullptr;
        }
        if (primary_index_relation_ != nullptr) {
            Relation finished =
                std::exchange(primary_index_relation_, nullptr);
            bioseq::postgres::support::invoke_pg_as_cpp(
                [finished] {
                    index_close(finished, AccessShareLock);
                });
            stats_.cold.add(stats_.cold.lock_releases, 1u);
        }
        if (heap_relation_ != nullptr) {
            Relation finished =
                std::exchange(heap_relation_, nullptr);
            bioseq::postgres::support::invoke_pg_as_cpp(
                [finished] {
                    relation_close(finished, AccessShareLock);
                });
            stats_.cold.add(stats_.cold.lock_releases, 1u);
        }
        snapshot_ = nullptr;
    }

    [[nodiscard]] const
    bioseq::indexing::hash_major_minimizer_stream_descriptor&
    descriptor_(hash_major_minimizer_stream_kind kind) const {
        if (!manifest_ || !hash_major_minimizer_pg_detail::valid_stream_kind(kind)) {
            minimizer_generation_storage_error_as_cpp(
                "hash-major stream descriptor requested before metadata");
        }
        const std::size_t index =
            hash_major_minimizer_pg_detail::stream_index(kind);
        const auto& descriptor = manifest_->streams[index];
        if (descriptor.kind != kind) {
            minimizer_generation_storage_error_as_cpp(
                "hash-major stream descriptor ordering changed");
        }
        return descriptor;
    }

    void record_stream_row_(
            hash_major_minimizer_stream_kind kind,
            std::size_t bytes) {
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
    [[nodiscard]] bool scan_stream_rows_(
            hash_major_minimizer_stream_kind kind,
            std::uint64_t first_row,
            std::uint64_t last_row,
            p_consumer&& consume) {
        const auto& descriptor = descriptor_(kind);
        if (state_ != operation_state::source_authorized
            || active_scan_ != nullptr
            || first_row > last_row
            || last_row >= descriptor.row_count
            || last_row
                > static_cast<std::uint64_t>(
                    (std::numeric_limits<std::int32_t>::max)())) {
            return false;
        }
        std::uint64_t expected_row = first_row;
        bool accepted = true;

        // A bounded control/posting wave often starts in the same physical
        // row where the preceding wave ended. Reuse that already authenticated
        // pinned projection before opening another btree scan. If more rows
        // remain, releasing the cache also clears every prior batch slot before
        // the next scan advances them.
        if (cached_boundary_row_valid_) {
            if (cached_boundary_row_kind_ == kind
                && cached_boundary_row_ordinal_ == expected_row) {
                // The projection was address-bound CRC authenticated before
                // being retained, and its heap buffer has remained pinned.
                // Replaying the immutable span needs neither another tuple
                // deformation nor another 8-KiB row-CRC pass.
                accepted = consume(
                    expected_row, cached_boundary_row_.bytes);
                ++expected_row;
                if (!accepted || expected_row > last_row) {
                    return accepted
                        && expected_row == last_row + 1u;
                }
            }
            clear_row_batch_slots_checked_();
        }

        ScanKeyData keys[6];
        ScanKeyInit(&keys[0], k_minimizer_generation_index_generation_id,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(
                manifest_->generation_id)));
        ScanKeyInit(&keys[1], k_minimizer_generation_index_query_key,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(query_key_)));
        ScanKeyInit(&keys[2], k_minimizer_generation_index_sub_key,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(
                hash_major_minimizer_pg_detail::stream_sub_key(kind))));
        ScanKeyInit(&keys[3], k_minimizer_generation_index_position_segment,
            BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(0));
        ScanKeyInit(&keys[4], k_minimizer_generation_index_chunk_number,
            BTGreaterEqualStrategyNumber, F_INT4GE,
            Int32GetDatum(static_cast<std::int32_t>(expected_row)));
        ScanKeyInit(&keys[5], k_minimizer_generation_index_chunk_number,
            BTLessEqualStrategyNumber, F_INT4LE,
            Int32GetDatum(static_cast<std::int32_t>(last_row)));
        create_row_batch_slots_checked_();
        active_scan_ = bioseq::postgres::support::invoke_pg_as_cpp([&] {
            return systable_beginscan_ordered(
                heap_relation_, primary_index_relation_,
                snapshot_, 6, keys);
        });
        ++stats_.index_scans;
        ++stats_.range_scan_calls;
        stats_.cold.add(stats_.cold.payload_range_scans, 1u);
        pinned_minimizer_generation_projected_row_batch batch{};
        validated_minimizer_generation_row_receipt_batch validated_receipts{};
        std::size_t batch_ordinal = 0u;
        while (accepted && expected_row <= last_row) {
            if (batch_ordinal == batch.size) {
                if (batch.eof) break;
                clear_validated_minimizer_generation_row_receipt_batch(
                    validated_receipts);
                batch =
                    next_pinned_minimizer_generation_projected_row_batch_or_error(
                        heap_relation_, active_scan_,
                        row_batch_slots_);
                batch_ordinal = 0u;
                if (batch.size == 0u) {
                    if (batch.eof) break;
                    fail_("hash-major batched stream scan made no progress");
                }
                authenticate_minimizer_generation_projected_row_batch(
                    batch, validated_receipts);
            }
            const pinned_minimizer_generation_projected_row& pinned =
                batch.rows[batch_ordinal];
            minimizer_generation_loaded_row row =
                load_authenticated_minimizer_generation_projected_row_or_error(
                    pinned.projected,
                    validated_receipts[batch_ordinal]);
            ++batch_ordinal;
            if (row.query_key != query_key_
                || row.address != index_chunk_address{
                    .generation_id = manifest_->generation_id,
                    .position_segment = 0,
                    .sub_key =
                        hash_major_minimizer_pg_detail::stream_sub_key(kind),
                    .chunk_no =
                        static_cast<std::uint32_t>(expected_row),
                }) {
                row.release();
                fail_("hash-major stream row identity/order disagrees");
            }
            const std::uint64_t row_offset =
                expected_row * k_hash_major_minimizer_row_payload_bytes;
            const std::uint64_t remaining =
                descriptor.logical_bytes - row_offset;
            const std::size_t expected_bytes =
                static_cast<std::size_t>(std::min<std::uint64_t>(
                    remaining, k_hash_major_minimizer_row_payload_bytes));
            if (row.bytes.size() != expected_bytes
                || (expected_row + 1u < descriptor.row_count
                    && row.bytes.size()
                        != k_hash_major_minimizer_row_payload_bytes)) {
                row.release();
                fail_("hash-major stream physical row length disagrees");
            }
            record_stream_row_(kind, row.bytes.size());
            accepted = consume(expected_row, row.bytes);
            const bool retain_boundary_row =
                accepted && expected_row == last_row;
            row.release();
            if (retain_boundary_row) {
                cached_boundary_row_valid_ = true;
                cached_boundary_row_kind_ = kind;
                cached_boundary_row_ordinal_ = expected_row;
                cached_boundary_slot_ordinal_ =
                    batch_ordinal - 1u;
                cached_boundary_row_ = pinned.projected;
            }
            ++expected_row;
        }
        clear_validated_minimizer_generation_row_receipt_batch(
            validated_receipts);
        if (cached_boundary_row_valid_) {
            clear_row_batch_slots_except_checked_(
                cached_boundary_slot_ordinal_);
        } else {
            clear_row_batch_slots_checked_();
        }
        bioseq::postgres::support::invoke_pg_as_cpp(
            [&] { systable_endscan_ordered(active_scan_); });
        active_scan_ = nullptr;
        return accepted && expected_row == last_row + 1u;
    }

    [[nodiscard]] bool read_logical_bytes_(
            hash_major_minimizer_stream_kind kind,
            std::uint64_t offset,
            std::span<std::byte> output) {
        if (state_ != operation_state::source_authorized
            || !hash_major_minimizer_pg_detail::valid_stream_kind(kind)) {
            return false;
        }
        const auto& descriptor = descriptor_(kind);
        if (offset > descriptor.logical_bytes
            || output.size() > descriptor.logical_bytes - offset) {
            return false;
        }
        if (output.empty()) return true;
        const std::uint64_t end = offset + output.size();
        const std::uint64_t first_row =
            offset / k_hash_major_minimizer_row_payload_bytes;
        const std::uint64_t last_row =
            (end - 1u) / k_hash_major_minimizer_row_payload_bytes;
        std::size_t copied = 0u;
        const bool read = scan_stream_rows_(
            kind, first_row, last_row,
            [&](std::uint64_t row_ordinal,
                    std::span<const std::byte> row) {
                const std::uint64_t row_begin =
                    row_ordinal * k_hash_major_minimizer_row_payload_bytes;
                const std::uint64_t row_end = row_begin + row.size();
                const std::uint64_t copy_begin =
                    std::max(offset, row_begin);
                const std::uint64_t copy_end =
                    std::min(end, row_end);
                if (copy_begin >= copy_end) return true;
                const std::size_t bytes =
                    static_cast<std::size_t>(copy_end - copy_begin);
                std::copy_n(
                    row.begin()
                        + static_cast<std::ptrdiff_t>(
                            copy_begin - row_begin),
                    bytes,
                    output.begin()
                        + static_cast<std::ptrdiff_t>(copied));
                copied += bytes;
                return true;
            });
        return read && copied == output.size();
    }

    [[nodiscard]] bool read_control_wave_(
            std::span<const hash_major_minimizer_authorized_control_range>
                ranges,
            void* consumer_context,
            hash_major_minimizer_consume_control_block_fn consume) {
        if (state_ != operation_state::source_authorized
            || consume == nullptr
            || ranges.empty()
            || ranges.size()
                > bioseq::indexing::
                    k_hash_major_minimizer_control_read_wave_blocks
            || ranges.size()
                > static_cast<std::size_t>(
                    (std::numeric_limits<std::uint32_t>::max)())) {
            return false;
        }
        const auto& descriptor =
            descriptor_(hash_major_minimizer_stream_kind::controls);
        std::uint64_t previous_end = 0u;
        std::uint64_t previous_block = 0u;
        bool have_previous = false;
        for (const auto& range : ranges) {
            const std::uint64_t end =
                range.control_byte_offset + range.byte_count;
            if (range.byte_count == 0u
                || range.byte_count
                    > k_hash_major_minimizer_maximum_control_block_bytes
                || range.reserved_zero != 0u
                || end < range.control_byte_offset
                || end > descriptor.logical_bytes
                || (have_previous
                    && (range.control_byte_offset < previous_end
                        || range.control_block_ordinal
                            <= previous_block))) {
                return false;
            }
            previous_end = end;
            previous_block = range.control_block_ordinal;
            have_previous = true;
        }

        std::size_t group_begin = 0u;
        while (group_begin < ranges.size()) {
            const auto first_interval_end =
                ranges[group_begin].control_byte_offset
                + ranges[group_begin].byte_count;
            std::uint64_t group_last_row =
                (first_interval_end - 1u)
                / k_hash_major_minimizer_row_payload_bytes;
            std::size_t group_end = group_begin + 1u;
            while (group_end < ranges.size()) {
                const std::uint64_t next_first_row =
                    ranges[group_end].control_byte_offset
                    / k_hash_major_minimizer_row_payload_bytes;
                if (next_first_row > group_last_row + 1u) break;
                const std::uint64_t next_end =
                    ranges[group_end].control_byte_offset
                    + ranges[group_end].byte_count;
                group_last_row = std::max(
                    group_last_row,
                    (next_end - 1u)
                        / k_hash_major_minimizer_row_payload_bytes);
                ++group_end;
            }
            const std::uint64_t group_first_row =
                ranges[group_begin].control_byte_offset
                / k_hash_major_minimizer_row_payload_bytes;
            std::size_t cursor = group_begin;
            const bool delivered = scan_stream_rows_(
                hash_major_minimizer_stream_kind::controls,
                group_first_row,
                group_last_row,
                [&](std::uint64_t row_ordinal,
                        std::span<const std::byte> row) {
                    const std::uint64_t row_begin =
                        row_ordinal
                        * k_hash_major_minimizer_row_payload_bytes;
                    const std::uint64_t row_end =
                        row_begin + row.size();
                    while (cursor < group_end) {
                        const auto& range = ranges[cursor];
                        const std::uint64_t range_begin =
                            range.control_byte_offset;
                        const std::uint64_t range_end =
                            range_begin + range.byte_count;
                        if (range_begin >= row_end) break;
                        const std::uint64_t copy_begin =
                            std::max(row_begin, range_begin);
                        const std::uint64_t copy_end =
                            std::min(row_end, range_end);
                        if (copy_begin < copy_end) {
                            std::copy_n(
                                row.begin()
                                    + static_cast<std::ptrdiff_t>(
                                        copy_begin - row_begin),
                                static_cast<std::size_t>(
                                    copy_end - copy_begin),
                                control_wave_scratch_.begin()
                                    + static_cast<std::ptrdiff_t>(
                                        copy_begin - range_begin));
                        }
                        if (row_end < range_end) break;
                        if (!consume(
                                consumer_context,
                                static_cast<std::uint32_t>(cursor),
                                std::span<const std::byte>{
                                    control_wave_scratch_}.first(
                                        range.byte_count))) {
                            return false;
                        }
                        ++cursor;
                    }
                    return true;
                });
            if (!delivered || cursor != group_end) return false;
            group_begin = group_end;
        }
        return true;
    }

    [[nodiscard]] bool read_posting_wave_(
            std::span<const hash_major_minimizer_authorized_posting_range>
                ranges,
            void* consumer_context,
            hash_major_minimizer_consume_posting_bytes_fn consume) {
        if (state_ != operation_state::source_authorized
            || consume == nullptr || ranges.empty()
            || ranges.size()
                > static_cast<std::size_t>(
                    (std::numeric_limits<std::uint32_t>::max)())) {
            return false;
        }
        const auto& descriptor =
            descriptor_(hash_major_minimizer_stream_kind::postings);
        std::uint64_t previous_end = 0u;
        bool have_previous = false;
        for (const auto& range : ranges) {
            const std::uint64_t end =
                range.posting_byte_offset + range.byte_count;
            if (range.byte_count == 0u
                || range.reserved_zero != 0u
                || range.request_count == 0u
                || end < range.posting_byte_offset
                || end > descriptor.logical_bytes
                || (have_previous
                    && range.posting_byte_offset < previous_end)) {
                return false;
            }
            previous_end = end;
            have_previous = true;
        }

        std::size_t group_begin = 0u;
        while (group_begin < ranges.size()) {
            const std::uint64_t first_end =
                ranges[group_begin].posting_byte_offset
                + ranges[group_begin].byte_count;
            std::uint64_t group_last_row =
                (first_end - 1u)
                / k_hash_major_minimizer_row_payload_bytes;
            std::size_t group_end = group_begin + 1u;
            while (group_end < ranges.size()) {
                const std::uint64_t next_first_row =
                    ranges[group_end].posting_byte_offset
                    / k_hash_major_minimizer_row_payload_bytes;
                if (next_first_row > group_last_row + 1u) break;
                const std::uint64_t next_end =
                    ranges[group_end].posting_byte_offset
                    + ranges[group_end].byte_count;
                group_last_row = std::max(
                    group_last_row,
                    (next_end - 1u)
                        / k_hash_major_minimizer_row_payload_bytes);
                ++group_end;
            }
            const std::uint64_t group_first_row =
                ranges[group_begin].posting_byte_offset
                / k_hash_major_minimizer_row_payload_bytes;
            std::size_t cursor = group_begin;
            const bool delivered = scan_stream_rows_(
                hash_major_minimizer_stream_kind::postings,
                group_first_row,
                group_last_row,
                [&](std::uint64_t row_ordinal,
                        std::span<const std::byte> row) {
                    const std::uint64_t row_begin =
                        row_ordinal
                        * k_hash_major_minimizer_row_payload_bytes;
                    const std::uint64_t row_end =
                        row_begin + row.size();
                    while (cursor < group_end) {
                        const auto& range = ranges[cursor];
                        const std::uint64_t range_begin =
                            range.posting_byte_offset;
                        const std::uint64_t range_end =
                            range_begin + range.byte_count;
                        if (range_begin >= row_end) break;
                        const std::uint64_t copy_begin =
                            std::max(row_begin, range_begin);
                        const std::uint64_t copy_end =
                            std::min(row_end, range_end);
                        if (copy_begin < copy_end
                            && !consume(
                                consumer_context,
                                static_cast<std::uint32_t>(cursor),
                                copy_begin - range_begin,
                                row.subspan(
                                    static_cast<std::size_t>(
                                        copy_begin - row_begin),
                                    static_cast<std::size_t>(
                                        copy_end - copy_begin)))) {
                            return false;
                        }
                        if (row_end < range_end) break;
                        if (stats_.hash_major.posting_ranges_delivered
                                == (std::numeric_limits<
                                    std::uint64_t>::max)()
                            || range.byte_count
                                > (std::numeric_limits<
                                    std::uint64_t>::max)()
                                    - stats_.hash_major
                                        .authorized_posting_bytes) {
                            return false;
                        }
                        ++stats_.hash_major.posting_ranges_delivered;
                        stats_.hash_major.authorized_posting_bytes +=
                            range.byte_count;
                        ++cursor;
                    }
                    return true;
                });
            if (!delivered || cursor != group_end) return false;
            group_begin = group_end;
        }
        return true;
    }

    [[nodiscard]] static bool read_bytes_callback_(
            void* context,
            hash_major_minimizer_stream_kind kind,
            std::uint64_t offset,
            std::span<std::byte> output) {
        return static_cast<index_storage_hash_major_minimizer_read_session*>(
            context)->read_logical_bytes_(kind, offset, output);
    }

    [[nodiscard]] static std::uint64_t logical_size_callback_(
            void* context,
            hash_major_minimizer_stream_kind kind) {
        auto& self =
            *static_cast<index_storage_hash_major_minimizer_read_session*>(
                context);
        if (self.state_ != operation_state::source_authorized
            || !hash_major_minimizer_pg_detail::valid_stream_kind(kind)) {
            return 0u;
        }
        return self.descriptor_(kind).logical_bytes;
    }

    [[nodiscard]] static bool control_wave_callback_(
            void* context,
            std::span<const hash_major_minimizer_authorized_control_range>
                ranges,
            void* consumer_context,
            hash_major_minimizer_consume_control_block_fn consume) {
        return static_cast<index_storage_hash_major_minimizer_read_session*>(
            context)->read_control_wave_(
                ranges, consumer_context, consume);
    }

    [[nodiscard]] static bool posting_wave_callback_(
            void* context,
            std::span<const hash_major_minimizer_authorized_posting_range>
                ranges,
            void* consumer_context,
            hash_major_minimizer_consume_posting_bytes_fn consume) {
        return static_cast<index_storage_hash_major_minimizer_read_session*>(
            context)->read_posting_wave_(
                ranges, consumer_context, consume);
    }

public:
    index_storage_hash_major_minimizer_read_session() = delete;
    index_storage_hash_major_minimizer_read_session(
            std::string_view table_name,
            std::string_view classification_index_name,
            std::uint64_t query_key,
            Snapshot registered_snapshot,
            Oid expected_tablespace_oid)
        : table_name_(table_name),
          index_name_(classification_index_name),
          snapshot_(registered_snapshot),
          query_key_(query_key),
          expected_tablespace_oid_(expected_tablespace_oid) {
        try {
            if (table_name_.empty()
                || index_name_.empty()
                || snapshot_ == nullptr
                || !IsMVCCSnapshot(snapshot_)
                || snapshot_->regd_count == 0
                || !OidIsValid(expected_tablespace_oid_)
                || !bioseq::indexing::sql_bigint_can_represent(
                    query_key_)) {
                fail_("hash-major read binding is invalid");
            }
            const Oid relation_oid =
                bioseq::postgres::support::invoke_pg_as_cpp([&] {
                    return get_relname_relid(
                        table_name_.c_str(), bioseq_internal_ns());
                });
            if (!OidIsValid(relation_oid)) {
                throw minimizer_generation_relation_open_failure{
                    .inspection = {
                        .kind = minimizer_generation_relation_kind::missing,
                        .schema_error = minimizer_generation_schema_error::none,
                    },
                };
            }
            heap_relation_ = bioseq::postgres::support::invoke_pg_as_cpp([&] {
                return bioseq::postgres::support::try_open_postgres_relation(
                    relation_oid, AccessShareLock);
            });
            if (heap_relation_ == nullptr) {
                throw minimizer_generation_relation_open_failure{
                    .inspection = {
                        .kind = minimizer_generation_relation_kind::missing,
                        .schema_error = minimizer_generation_schema_error::none,
                    },
                };
            }
            stats_.cold.add(stats_.cold.sidecar_heap_opens, 1u);
            stats_.cold.add(stats_.cold.lock_acquires, 1u);
            const bool exact_identity =
                RelationGetNamespace(heap_relation_)
                    == bioseq_internal_ns()
                && std::string_view{
                    RelationGetRelationName(heap_relation_)}
                    == table_name_;
            const Oid primary_oid =
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [&] {
                        return RelationGetPrimaryKeyIndex(
                            heap_relation_);
                    });
            if (OidIsValid(primary_oid)) {
                primary_index_relation_ =
                    bioseq::postgres::support::invoke_pg_as_cpp(
                        [primary_oid] {
                            return index_open(
                                primary_oid, AccessShareLock);
                        });
                stats_.cold.add(
                    stats_.cold.sidecar_index_opens, 1u);
                stats_.cold.add(stats_.cold.lock_acquires, 1u);
            }
            const minimizer_generation_schema_inspection schema = exact_identity
                ? bioseq::postgres::support::invoke_pg_as_cpp([&] {
                    return inspect_bound_minimizer_generation_schema(
                        heap_relation_,
                        primary_index_relation_,
                        minimizer_generation_schema_expectation{
                            .namespace_oid = bioseq_internal_ns(),
                            .relation_name = table_name_,
                            .tablespace_oid =
                                expected_tablespace_oid_,
                        });
                })
                : minimizer_generation_schema_inspection{
                    .error = minimizer_generation_schema_error::not_heap,
                };
            stats_.cold.add(
                stats_.cold.sidecar_schema_validations, 1u);
            if (!schema) {
                if (primary_index_relation_ != nullptr) {
                    Relation finished = std::exchange(
                        primary_index_relation_, nullptr);
                    bioseq::postgres::support::invoke_pg_as_cpp(
                        [finished] {
                            index_close(
                                finished, AccessShareLock);
                        });
                }
                Relation finished =
                    std::exchange(heap_relation_, nullptr);
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [finished] {
                        relation_close(
                            finished, AccessShareLock);
                    });
                snapshot_ = nullptr;
                throw minimizer_generation_relation_open_failure{
                    .inspection = {
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
    index_storage_hash_major_minimizer_read_session& operator=(
        const index_storage_hash_major_minimizer_read_session&) = delete;
    index_storage_hash_major_minimizer_read_session(
            index_storage_hash_major_minimizer_read_session&& other) noexcept
        : table_name_(std::move(other.table_name_)),
          index_name_(std::move(other.index_name_)),
          heap_relation_(std::exchange(
              other.heap_relation_, nullptr)),
          primary_index_relation_(std::exchange(
              other.primary_index_relation_, nullptr)),
          active_scan_(std::exchange(other.active_scan_, nullptr)),
          row_batch_slots_(other.row_batch_slots_),
          row_batch_slot_count_(other.row_batch_slot_count_),
          cached_boundary_row_valid_(
              other.cached_boundary_row_valid_),
          cached_boundary_row_kind_(
              other.cached_boundary_row_kind_),
          cached_boundary_row_ordinal_(
              other.cached_boundary_row_ordinal_),
          cached_boundary_slot_ordinal_(
              other.cached_boundary_slot_ordinal_),
          cached_boundary_row_(other.cached_boundary_row_),
          snapshot_(std::exchange(other.snapshot_, nullptr)),
          query_key_(other.query_key_),
          expected_tablespace_oid_(
              other.expected_tablespace_oid_),
          state_(other.state_),
          encoded_head_(other.encoded_head_),
          encoded_manifest_(other.encoded_manifest_),
          head_(std::move(other.head_)),
          manifest_(std::move(other.manifest_)),
          expected_generation_(
              std::move(other.expected_generation_)),
          control_wave_scratch_(other.control_wave_scratch_),
          stats_(other.stats_) {
        other.invalidate_cached_boundary_row_();
        other.row_batch_slots_.fill(nullptr);
        other.row_batch_slot_count_ = 0u;
        other.state_ = operation_state::moved;
    }
    index_storage_hash_major_minimizer_read_session& operator=(
        index_storage_hash_major_minimizer_read_session&&) = delete;
    ~index_storage_hash_major_minimizer_read_session() {
        abandon_noexcept_();
    }

    /// Returns false only for an authenticated absence of the distinct
    /// hash-major head sentinel.  A present malformed head or manifest is a
    /// hard error and never enters the grouped-SoA fallback.
    [[nodiscard]] bool read_active_head_manifest_once() {
        if (state_ != operation_state::opened || active_scan_ != nullptr) {
            fail_("hash-major metadata was read more than once");
        }
        ScanKeyData head_keys[5];
        ScanKeyInit(&head_keys[0], k_minimizer_generation_index_generation_id,
            BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum(0));
        ScanKeyInit(&head_keys[1], k_minimizer_generation_index_query_key,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(query_key_)));
        ScanKeyInit(&head_keys[2], k_minimizer_generation_index_sub_key,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(
                k_hash_major_generation_head_sub_key)));
        ScanKeyInit(&head_keys[3], k_minimizer_generation_index_position_segment,
            BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(-1));
        ScanKeyInit(&head_keys[4], k_minimizer_generation_index_chunk_number,
            BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(0));
        active_scan_ = bioseq::postgres::support::invoke_pg_as_cpp([&] {
            return systable_beginscan_ordered(
                heap_relation_, primary_index_relation_,
                snapshot_, 5, head_keys);
        });
        ++stats_.index_scans;
        ++stats_.head_scans;
        HeapTuple tuple = bioseq::postgres::support::invoke_pg_as_cpp([&] {
            return systable_getnext_ordered(
                active_scan_, ForwardScanDirection);
        });
        if (!HeapTupleIsValid(tuple)) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [&] { systable_endscan_ordered(active_scan_); });
            active_scan_ = nullptr;
            state_ = operation_state::head_absent;
            return false;
        }
        minimizer_generation_loaded_row head_row =
            load_minimizer_generation_row_or_error(heap_relation_, tuple);
        if (head_row.query_key != query_key_
            || head_row.address != index_chunk_address{
                .generation_id = 0u,
                .position_segment = -1,
                .sub_key = k_hash_major_generation_head_sub_key,
                .chunk_no = 0u,
            }
            || head_row.bytes.size()
                != k_hash_major_minimizer_head_bytes) {
            head_row.release();
            fail_("hash-major head row shape disagrees");
        }
        std::copy(
            head_row.bytes.begin(), head_row.bytes.end(),
            encoded_head_.begin());
        head_row.release();
        HeapTuple extra = bioseq::postgres::support::invoke_pg_as_cpp([&] {
            return systable_getnext_ordered(
                active_scan_, ForwardScanDirection);
        });
        bioseq::postgres::support::invoke_pg_as_cpp(
            [&] { systable_endscan_ordered(active_scan_); });
        active_scan_ = nullptr;
        if (HeapTupleIsValid(extra)) {
            fail_("hash-major head primary key returned duplicates");
        }
        auto decoded_head =
            bioseq::indexing::decode_hash_major_minimizer_head(
                encoded_head_);
        if (!decoded_head
            || decoded_head->query_key != query_key_
            || decoded_head->generation_id == 0u
            || !bioseq::indexing::sql_bigint_can_represent(
                decoded_head->generation_id)) {
            fail_("hash-major head fixed wire is invalid");
        }
        head_.emplace(*decoded_head);
        if (head_->query_key != query_key_
            || head_->generation_id == 0u
            || !bioseq::indexing::sql_bigint_can_represent(
                head_->generation_id)) {
            fail_("hash-major head fixed wire is invalid");
        }
        stats_.head_rows_read = 1u;
        stats_.head_bytes_read = k_hash_major_minimizer_head_bytes;
        stats_.cold.add(
            stats_.cold.row_crc_bytes,
            k_hash_major_minimizer_head_bytes);

        ScanKeyData manifest_keys[5];
        ScanKeyInit(&manifest_keys[0], k_minimizer_generation_index_generation_id,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(
                head_->generation_id)));
        ScanKeyInit(&manifest_keys[1], k_minimizer_generation_index_query_key,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(query_key_)));
        ScanKeyInit(&manifest_keys[2], k_minimizer_generation_index_sub_key,
            BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum(0));
        ScanKeyInit(&manifest_keys[3], k_minimizer_generation_index_position_segment,
            BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(0));
        ScanKeyInit(&manifest_keys[4], k_minimizer_generation_index_chunk_number,
            BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(0));
        active_scan_ = bioseq::postgres::support::invoke_pg_as_cpp([&] {
            return systable_beginscan_ordered(
                heap_relation_, primary_index_relation_,
                snapshot_, 5, manifest_keys);
        });
        ++stats_.index_scans;
        ++stats_.manifest_directory_scans;
        tuple = bioseq::postgres::support::invoke_pg_as_cpp([&] {
            return systable_getnext_ordered(
                active_scan_, ForwardScanDirection);
        });
        if (!HeapTupleIsValid(tuple)) {
            fail_("hash-major manifest row is missing");
        }
        minimizer_generation_loaded_row manifest_row =
            load_minimizer_generation_row_or_error(heap_relation_, tuple);
        if (manifest_row.query_key != query_key_
            || manifest_row.address != index_chunk_address{
                .generation_id = head_->generation_id,
                .position_segment = 0,
                .sub_key = 0,
                .chunk_no = 0u,
            }
            || manifest_row.bytes.size()
                != k_hash_major_minimizer_manifest_bytes) {
            manifest_row.release();
            fail_("hash-major manifest row shape disagrees");
        }
        std::copy(
            manifest_row.bytes.begin(), manifest_row.bytes.end(),
            encoded_manifest_.begin());
        manifest_row.release();
        extra = bioseq::postgres::support::invoke_pg_as_cpp([&] {
            return systable_getnext_ordered(
                active_scan_, ForwardScanDirection);
        });
        bioseq::postgres::support::invoke_pg_as_cpp(
            [&] { systable_endscan_ordered(active_scan_); });
        active_scan_ = nullptr;
        if (HeapTupleIsValid(extra)) {
            fail_("hash-major manifest primary key returned duplicates");
        }
        auto decoded_manifest =
            bioseq::indexing::decode_hash_major_minimizer_manifest(
                encoded_manifest_);
        if (!decoded_manifest
            || decoded_manifest->generation_id
                != head_->generation_id
            || decoded_manifest->source_epoch
                != head_->source_epoch
            || decoded_manifest->source_identity
                != head_->source_identity
            || decoded_manifest->source_content_crc64
                != head_->source_content_crc64
            || decoded_manifest->storage_profile_digest
                != head_->storage_profile_digest) {
            fail_("hash-major head/manifest authentication disagrees");
        }
        manifest_.emplace(*decoded_manifest);
        if (manifest_->generation_id != head_->generation_id
            || manifest_->source_epoch != head_->source_epoch
            || manifest_->source_identity != head_->source_identity
            || manifest_->source_content_crc64
                != head_->source_content_crc64
            || manifest_->storage_profile_digest
                != head_->storage_profile_digest
            || head_->manifest_payload_crc64
                != bioseq::indexing::crc64_ecma182(
                    encoded_manifest_)
            || head_->manifest_address_bound_row_crc64
                != bioseq::indexing::crc64_index_chunk_row(
                    head_->generation_id, 0, 0, 0u,
                    static_cast<std::uint32_t>(
                        encoded_manifest_.size()),
                    encoded_manifest_)
            || !bioseq::indexing::
                is_valid_hash_major_minimizer_storage_profile(
                    manifest_->storage_profile,
                    manifest_->target_length)
            || manifest_->streams[0].kind
                != hash_major_minimizer_stream_kind::directory
            || manifest_->streams[1].kind
                != hash_major_minimizer_stream_kind::controls
            || manifest_->streams[2].kind
                != hash_major_minimizer_stream_kind::postings) {
            fail_("hash-major head/manifest authentication disagrees");
        }
        std::uint64_t first_row = 0u;
        for (std::size_t index = 0u; index < 3u; ++index) {
            const auto& stream = manifest_->streams[index];
            if (stream.row_count
                    != hash_major_minimizer_pg_detail::ceil_rows(
                        stream.logical_bytes)
                || stream.first_row_ordinal != first_row
                || stream.row_count
                    > static_cast<std::uint64_t>(
                        (std::numeric_limits<std::int32_t>::max)())
                || stream.row_count
                    > (std::numeric_limits<std::uint64_t>::max)()
                        - first_row) {
                fail_("hash-major stream row descriptor disagrees");
            }
            first_row += stream.row_count;
        }
        stats_.manifest_rows_read = 1u;
        stats_.manifest_bytes_read =
            k_hash_major_minimizer_manifest_bytes;
        stats_.metadata_heap_rows_read = 2u;
        stats_.metadata_bytes_read =
            k_hash_major_minimizer_head_bytes
            + k_hash_major_minimizer_manifest_bytes;
        stats_.cold.add(
            stats_.cold.row_crc_bytes,
            k_hash_major_minimizer_manifest_bytes);
        state_ = operation_state::metadata_ready;
        return true;
    }

    [[nodiscard]] std::span<const std::byte>
    encoded_head() const noexcept {
        return state_ == operation_state::metadata_ready
                || state_ == operation_state::source_authorized
            ? std::span<const std::byte>{encoded_head_}
            : std::span<const std::byte>{};
    }

    [[nodiscard]] std::span<const std::byte>
    encoded_manifest() const noexcept {
        return state_ == operation_state::metadata_ready
                || state_ == operation_state::source_authorized
            ? std::span<const std::byte>{encoded_manifest_}
            : std::span<const std::byte>{};
    }

    [[nodiscard]] const hash_major_minimizer_head& head() const {
        if (!head_) {
            minimizer_generation_storage_error_as_cpp(
                "hash-major head requested before metadata");
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

    [[nodiscard]] const expected_hash_major_minimizer_generation&
    consume_source_authorization(minimizer_generation_catalog_source_authorization&& authorization) {
        try {
            if (state_ != operation_state::metadata_ready
                || !head_ || !manifest_
                || !authorization.valid_
                || !authorization.stats_.timed_equation_holds()
                || authorization.snapshot_ != snapshot_
                || authorization.database_oid_ != MyDatabaseId
                || authorization.tablespace_oid_ != expected_tablespace_oid_
                || authorization.index_name_ != index_name_
                || authorization.source_.sequence_rowno != query_key_
                || authorization.table_name_ != table_name_) {
                authorization.invalidate_();
                fail_("hash-major source authorization binding/work disagrees");
            }
            const auto source_identity =
                bioseq::indexing::hash_major_minimizer_source_identity(
                    query_key_,
                    authorization.source_.locator.source_epoch,
                    authorization.source_content_checksum_,
                    authorization.source_.locator.logical_length);
            if (authorization.source_.locator.source_epoch
                    != manifest_->source_epoch
                || authorization.source_content_checksum_
                    != manifest_->source_content_crc64
                || authorization.source_.locator.logical_length
                    != manifest_->target_length
                || authorization.source_.alphabet_code
                    != manifest_->storage_profile.alphabet_code
                || source_identity != manifest_->source_identity
                || source_identity != head_->source_identity) {
                authorization.invalidate_();
                fail_("hash-major source identity/content disagrees");
            }
            stats_.source_parent_rows_read =
                authorization.stats_.parent_rows_read;
            stats_.source_catalog_rows_read =
                authorization.stats_.catalog_rows_read;
            stats_.source_catalog_index_scans =
                authorization.stats_.catalog_index_scans;
            stats_.source_fragment_rows_read =
                authorization.stats_.fragment_rows_read;
            stats_.source_decoded_bp =
                authorization.stats_.decoded_bp;
            stats_.source_storage_incarnations_checked =
                authorization.stats_.storage_incarnations_checked;
            stats_.cold.merge(authorization.cold_stats_);
            expected_generation_.emplace(
                expected_hash_major_minimizer_generation{
                    .generation_id = head_->generation_id,
                    .query_key = query_key_,
                    .source_epoch = manifest_->source_epoch,
                    .source_content_crc64 =
                        manifest_->source_content_crc64,
                    .target_length = manifest_->target_length,
                    .storage_profile = manifest_->storage_profile,
                });
            authorization.invalidate_();
            state_ = operation_state::source_authorized;
            return *expected_generation_;
        } catch (...) {
            authorization.invalidate_();
            state_ = operation_state::failed;
            throw;
        }
    }

    [[nodiscard]] hash_major_minimizer_logical_stream_source_ref
    source_ref() noexcept {
        if (state_ != operation_state::source_authorized) return {};
        return {
            .context = this,
            .read_bytes = &read_bytes_callback_,
            .logical_size = &logical_size_callback_,
            .read_control_wave = &control_wave_callback_,
            .read_posting_wave = &posting_wave_callback_,
        };
    }

    /// Execute the isolated process-parallel transport experiment without
    /// changing the production selective-reader callbacks.
    [[nodiscard]] hash_major_minimizer_parallel_scan_stats
    run_parallel_scan_probe(std::uint32_t requested_workers) {
        if (state_ != operation_state::source_authorized
            || active_scan_ != nullptr
            || heap_relation_ == nullptr
            || !manifest_) {
            fail_(
                "hash-major parallel scan probe is out of order");
        }
        return hash_major_minimizer_parallel_probe_detail::run_probe(
            heap_relation_, snapshot_, *manifest_,
            query_key_, requested_workers);
    }

    /// Administrative/restart audit. Each stream is reread in primary-key
    /// order with one physical row resident at a time. The scan authenticates
    /// every address-bound row receipt and the exact concatenated logical CRC.
    [[nodiscard]] std::array<
        hash_major_minimizer_catalog_stream_receipt, 3>
    audit_complete_streams() {
        if (state_ != operation_state::source_authorized
            || active_scan_ != nullptr || !manifest_) {
            fail_("hash-major complete stream audit is out of order");
        }
        std::array<hash_major_minimizer_catalog_stream_receipt, 3>
            receipts{};
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
                    kind, 0u, descriptor.row_count - 1u,
                    [&](std::uint64_t,
                            std::span<const std::byte> row) {
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
            if (!read
                || rows != descriptor.row_count
                || bytes != descriptor.logical_bytes
                || crc.checksum() != descriptor.stream_crc64) {
                fail_("hash-major complete stream audit disagrees");
            }
        }
        return receipts;
    }

    /// Checked close used by a route-inapplicable, self-authenticated F6 head
    /// before the independent F5 sentinel is probed.
    void close_metadata_read() {
        if (state_ != operation_state::metadata_ready
            || active_scan_ != nullptr) {
            fail_("hash-major metadata close is out of order");
        }
        state_ = operation_state::finished;
        close_relations_();
    }

    void close_head_absent() {
        if (state_ != operation_state::head_absent
            || active_scan_ != nullptr) {
            fail_("hash-major absent-head close is out of order");
        }
        state_ = operation_state::finished;
        close_relations_();
    }

    [[nodiscard]] minimizer_generation_read_stats finish() {
        if (state_ != operation_state::source_authorized
            || active_scan_ != nullptr
            || heap_relation_ == nullptr
            || primary_index_relation_ == nullptr) {
            fail_("hash-major read finished out of order");
        }
        stats_.total_heap_rows_read =
            stats_.metadata_heap_rows_read
            + stats_.payload_heap_rows_read;
        stats_.total_bytes_read =
            stats_.metadata_bytes_read
            + stats_.payload_bytes_read;
        stats_.cold.add(
            stats_.cold.payload_rows_requested,
            stats_.payload_heap_rows_read);
        stats_.cold.add(
            stats_.cold.payload_bytes_requested,
            stats_.payload_bytes_read);
        stats_.hash_major.layout_revision =
            bioseq::indexing::k_hash_major_minimizer_layout_revision;
        stats_.hash_major.sessions = 1u;
        stats_.hash_major.generation_id = head_->generation_id;
        stats_.hash_major.source_identity = head_->source_identity;
        stats_.hash_major.source_content_crc64 =
            head_->source_content_crc64;
        stats_.hash_major.storage_contract_digest =
            manifest_->storage_profile_digest;
        stats_.hash_major.directory_rows =
            stats_.directory_chunks_read;
        stats_.hash_major.directory_bytes =
            stats_.directory_bytes_read;
        stats_.hash_major.control_rows =
            stats_.controls.physical_heap_rows_read;
        stats_.hash_major.control_bytes =
            stats_.controls.physical_bytes_read;
        stats_.hash_major.posting_rows =
            stats_.positions.physical_heap_rows_read;
        stats_.hash_major.posting_bytes =
            stats_.positions.physical_bytes_read;
        state_ = operation_state::finished;
        close_relations_();
        return stats_;
    }
};

static_assert(!std::is_default_constructible_v<
    index_storage_hash_major_minimizer_read_session>);
static_assert(!std::is_copy_constructible_v<
    index_storage_hash_major_minimizer_read_session>);
static_assert(std::is_nothrow_move_constructible_v<
    index_storage_hash_major_minimizer_read_session>);

struct hash_major_minimizer_read_session_open_result {
    std::optional<index_storage_hash_major_minimizer_read_session>
        session{};
    minimizer_generation_relation_inspection relation_failure{
        .kind = minimizer_generation_relation_kind::malformed,
        .schema_error = minimizer_generation_schema_error::none,
    };
    bool head_absent = false;

    [[nodiscard]] explicit operator bool() const noexcept {
        return session.has_value();
    }
};

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
        result.session.emplace(
            table_name,
            index_name,
            query_key,
            executor_read.snapshot(),
            executor_read.tablespace_oid());
        if (!result.session->read_active_head_manifest_once()) {
            result.session->close_head_absent();
            result.session.reset();
            result.head_absent = true;
            result.relation_failure = {
                .kind =
                    minimizer_generation_relation_kind::current_exact,
                .schema_error = minimizer_generation_schema_error::none,
            };
        } else {
            result.relation_failure = {
                .kind =
                    minimizer_generation_relation_kind::current_exact,
                .schema_error = minimizer_generation_schema_error::none,
            };
        }
    } catch (const minimizer_generation_relation_open_failure& failure) {
        result.relation_failure = failure.inspection;
    }
    return result;
}

/// Read-only restart/audit receipt for the complete fixed hash-major metadata
/// surface. parallel_scan_stream descriptors carry the directory/control/posting counts,
/// byte lengths and CRCs; the manifest carries the generation CRC.
struct ready_hash_major_minimizer_metadata_identity {
    hash_major_minimizer_head head{};
    hash_major_minimizer_manifest manifest{};
    encoded_hash_major_minimizer_head encoded_head{};
    encoded_hash_major_minimizer_manifest encoded_manifest{};
    minimizer_generation_source_locator source{};
    std::uint64_t source_content_checksum = 0;
    minimizer_generation_catalog_source_stats source_stats{};
    std::array<hash_major_minimizer_catalog_stream_receipt, 3>
        exact_catalog_streams{};
};

[[nodiscard]] inline ready_hash_major_minimizer_metadata_identity
inspect_ready_hash_major_minimizer_metadata_pg(
        const minimizer_generation_administration_identity& identity,
        std::uint64_t query_key,
        Snapshot registered_snapshot,
        minimizer_generation_catalog_source_authorization&& authorization) {
    if (identity.database_oid != MyDatabaseId
        || identity.index_name.empty()
        || registered_snapshot == nullptr
        || !IsMVCCSnapshot(registered_snapshot)
        || registered_snapshot->regd_count == 0) {
        minimizer_generation_storage_error_as_cpp(
            "hash-major metadata inspection input is invalid");
    }
    const std::string table_name{
        authorization.table_name_for_diagnostics()};
    if (table_name.empty()
        || (!identity.table_name.empty()
            && identity.table_name != table_name)) {
        minimizer_generation_storage_error_as_cpp(
            "hash-major metadata inspection source/table binding is invalid");
    }
    const minimizer_generation_source_locator source =
        authorization.source_for_diagnostics();
    const std::uint64_t source_content_checksum =
        authorization.source_content_checksum_for_diagnostics();
    const minimizer_generation_catalog_source_stats source_stats =
        authorization.stats_for_diagnostics();
    index_storage_hash_major_minimizer_read_session session{
        table_name,
        identity.index_name,
        query_key,
        registered_snapshot,
        identity.tablespace_oid,
    };
    if (!session.read_active_head_manifest_once()) {
        session.close_head_absent();
        minimizer_generation_storage_error_as_cpp(
            "hash-major metadata inspection found no READY head");
    }
    (void)session.consume_source_authorization(std::move(authorization));
    ready_hash_major_minimizer_metadata_identity result{
        .head = session.head(),
        .manifest = session.manifest(),
        .source = source,
        .source_content_checksum = source_content_checksum,
        .source_stats = source_stats,
    };
    std::ranges::copy(
        session.encoded_head(), result.encoded_head.begin());
    std::ranges::copy(
        session.encoded_manifest(),
        result.encoded_manifest.begin());
    result.exact_catalog_streams =
        session.audit_complete_streams();
    (void)session.finish();
    return result;
}

[[nodiscard]] inline ready_hash_major_minimizer_metadata_identity
inspect_ready_hash_major_minimizer_metadata_pg(
        const minimizer_generation_administration_identity& identity,
        std::uint64_t query_key,
        Snapshot registered_snapshot) {
    minimizer_generation_catalog_source_authorization authorization =
        authorize_minimizer_generation_catalog_source_pg(
            identity, query_key, registered_snapshot);
    return inspect_ready_hash_major_minimizer_metadata_pg(
        identity, query_key, registered_snapshot, std::move(authorization));
}

/// Test-only transport probe. It authenticates the same READY head, manifest
/// and source binding as the production reader, then scans only the three
/// current-generation hash-major stream subkeys through either one serial heap
/// scan or one core ParallelContext/shared ParallelTableScanDesc.
[[nodiscard]] inline hash_major_minimizer_parallel_scan_stats
probe_ready_hash_major_minimizer_parallel_scan_pg(
        const minimizer_generation_administration_identity& identity,
        std::uint64_t query_key,
        Snapshot registered_snapshot,
        minimizer_generation_catalog_source_authorization&& authorization,
        std::uint32_t requested_workers) {
    if (identity.database_oid != MyDatabaseId
        || identity.index_name.empty()
        || registered_snapshot == nullptr
        || !IsMVCCSnapshot(registered_snapshot)
        || registered_snapshot->regd_count == 0) {
        minimizer_generation_storage_error_as_cpp(
            "hash-major parallel scan probe input is invalid");
    }
    const std::string table_name{
        authorization.table_name_for_diagnostics()};
    if (table_name.empty()) {
        minimizer_generation_storage_error_as_cpp(
            "hash-major parallel scan probe table binding is empty");
    }
    index_storage_hash_major_minimizer_read_session session{
        table_name,
        identity.index_name,
        query_key,
        registered_snapshot,
        identity.tablespace_oid,
    };
    if (!session.read_active_head_manifest_once()) {
        session.close_head_absent();
        minimizer_generation_storage_error_as_cpp(
            "hash-major parallel scan probe found no READY head");
    }
    (void)session.consume_source_authorization(std::move(authorization));
    hash_major_minimizer_parallel_scan_stats result =
        session.run_parallel_scan_probe(requested_workers);
    (void)session.finish();
    return result;
}

inline void hash_major_minimizer_parallel_scan_probe_worker_main_pg(
        dsm_segment* segment,
        shm_toc* toc) {
    using namespace hash_major_minimizer_parallel_probe_detail;
    if (segment == nullptr || toc == nullptr
        || !IsParallelWorker()) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq hash-major parallel scan worker binding is invalid")));
    }
    auto* header = static_cast<parallel_scan_header*>(
        shm_toc_lookup(toc, k_header_toc_key, false));
    auto* parallel_scan =
        static_cast<ParallelTableScanDesc>(
            shm_toc_lookup(toc, k_parallel_scan_toc_key, false));
    auto* seen_words = static_cast<pg_atomic_uint64*>(
        shm_toc_lookup(toc, k_seen_bitset_toc_key, false));
    if (header == nullptr || parallel_scan == nullptr
        || seen_words == nullptr
        || header->magic != k_toc_magic
        || header->format_revision != k_parallel_header_format_revision
        || header->relation_locator.dbOid != MyDatabaseId
        || GetActiveSnapshot() == nullptr
        || !IsMVCCSnapshot(GetActiveSnapshot())) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq hash-major parallel scan worker state is invalid")));
    }

    // ParallelWorkerMain joined the leader's lock group before loading this
    // symbol, so NoLock reuses the leader-held AccessShareLock and cannot
    // release it independently.
    Relation relation = table_open(
        header->relation_oid, NoLock);
    parallel_scan_pg_leaf(
        relation, parallel_scan, header, seen_words);
    table_close(relation, NoLock);
}

/// Result of one current-generation logical-index clear.
struct minimizer_generation_clear_result {
    std::uint64_t dropped_relation_count = 0;
    std::uint64_t deleted_metadata_age_rows = 0;
};

struct canonical_generation_sidecar_name {
    bool valid = false;
    std::uint8_t bucket_idx = 0;
    std::uint32_t shard_idx = 0;
};

/// Parse only the exact spelling emitted by `format_seqidx_name`: unsigned
/// canonical decimal bucket, one underscore, unsigned canonical decimal shard.
/// Leading zeroes, signs, trailing decorations, overflow, and extra separators
/// are rejected without allocation.
[[nodiscard]] inline canonical_generation_sidecar_name
parse_canonical_generation_sidecar_name(
        std::string_view relation_name,
        std::string_view prefix) noexcept {
    if (!relation_name.starts_with(prefix)) return {};
    const std::string_view suffix = relation_name.substr(prefix.size());
    const std::size_t separator = suffix.find('_');
    if (separator == std::string_view::npos || separator == 0
        || separator + 1 >= suffix.size()
        || suffix.find('_', separator + 1) != std::string_view::npos) {
        return {};
    }
    const std::string_view bucket_text = suffix.substr(0, separator);
    const std::string_view shard_text = suffix.substr(separator + 1);
    if ((bucket_text.size() > 1 && bucket_text.front() == '0')
        || (shard_text.size() > 1 && shard_text.front() == '0')) {
        return {};
    }
    const auto parse_decimal = [](std::string_view text,
            std::uint64_t maximum, std::uint64_t& result) noexcept {
        result = 0;
        for (const char character : text) {
            if (character < '0' || character > '9') return false;
            const std::uint64_t digit =
                static_cast<std::uint64_t>(character - '0');
            if (result > (maximum - digit) / 10u) return false;
            result = result * 10u + digit;
        }
        return true;
    };
    std::uint64_t bucket = 0;
    std::uint64_t shard = 0;
    if (!parse_decimal(bucket_text,
            (std::numeric_limits<std::uint8_t>::max)(), bucket)
        || !parse_decimal(shard_text,
            (std::numeric_limits<std::uint32_t>::max)(), shard)) {
        return {};
    }
    return canonical_generation_sidecar_name{
        .valid = true,
        .bucket_idx = static_cast<std::uint8_t>(bucket),
        .shard_idx = static_cast<std::uint32_t>(shard),
    };
}

/// Clear-only DDL executor. Every SPI transition is checked and the caller-owned
/// activity bit remains set if PostgreSQL jumps
/// out of SPI_execute so the enclosing PG_CATCH can finish the connection.
inline void run_checked_generation_clear_ddl(
        const char* ddl,
        bool& spi_active) {
    if (ddl == nullptr || *ddl == '\0' || spi_active) {
        minimizer_generation_storage_error("generation clear DDL state is invalid");
    }
    const int connect_rc = SPI_connect();
    if (connect_rc != SPI_OK_CONNECT) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("bioseq current-generation index: generation clear DDL SPI_connect failed"),
             errdetail("SPI rc=%d", connect_rc)));
    }
    spi_active = true;
    const int execute_rc = SPI_execute(
        ddl, /*read_only=*/false, /*tcount=*/0);
    const int finish_rc = SPI_finish();
    spi_active = finish_rc != SPI_OK_FINISH;
    if (execute_rc != SPI_OK_UTILITY || finish_rc != SPI_OK_FINISH) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("bioseq current-generation index: generation clear DDL failed"),
             errdetail("execute_rc=%d finish_rc=%d",
                execute_rc, finish_rc)));
    }
}

// =========================================================================
//  fragment_index_storage  —  CHUNKED_PER_FRAGMENT
// =========================================================================
struct fragment_index_storage {
    using generation_read_session =
        index_storage_fragment_generation_read_session;
    using generation_row_sink =
        index_storage_fragment_generation_row_sink;
    using hash_major_minimizer_generation_writer =
        index_storage_hash_major_minimizer_writer;

    Oid           db_oid_ = InvalidOid, tablespace_oid_ = InvalidOid, main_relfilenode_ = InvalidOid, user_relid_ = InvalidOid;
    seq_caches*   read_caches_ = nullptr;
    std::int16_t  column_attno_ = 0;
    std::string_view index_name_{};
    std::uint8_t  bucket_idx_ = 0;
    std::uint32_t shard_idx_ = 0;
    std::uint64_t query_key_ = 0;
    MemoryContext own_mctx_ = nullptr;
    mutable Oid   index_relid_ = InvalidOid;
    mutable std::int64_t* decoded_ = nullptr;
    mutable std::size_t   decoded_units_ = 0;
    mutable std::size_t   decoded_bytes_ = 0;
    mutable index_sub_key_t decoded_sub_ = k_directory_sub_key;
    mutable std::size_t   decoded_chunk_ = std::numeric_limits<std::size_t>::max();
    struct cached_chunk {
        // Admission outlives the bytes it covers (reverse member destruction).
        bioseq::parallelism::memory_permit permit;
        index_sub_key_t sub_key = k_directory_sub_key;
        std::size_t chunk_no = std::numeric_limits<std::size_t>::max();
        std::size_t payload_bytes = 0;
        std::uint64_t last_used = 0;
        std::vector<char> bytes;
    };
    mutable std::vector<cached_chunk> chunk_cache_;
    mutable std::uint64_t chunk_cache_clock_ = 0;
    mutable std::size_t chunk_cache_bytes_ = 0;
    static constexpr std::size_t k_chunk_cache_max_entries = 8;
    static constexpr std::size_t k_chunk_cache_max_bytes = 256ULL << 20;
    std::int64_t* scratch_ = nullptr;
    std::size_t   scratch_units_ = 0;
    std::size_t   run_buffer_override_bytes_ = 0;
    // per-sub_key byte plan recorded by resize/resize_chunks, consumed by write_to_each.
    std::map<index_sub_key_t, std::vector<std::size_t>> chunk_payload_bytes_;
    std::optional<sorted_pair_run_store> sorted_runs_;

private:
    /// Own the PostgreSQL resources used by one direct fragment read while the
    /// surrounding C++ frame is live.  Destruction deliberately performs no
    /// PostgreSQL work: a healthy path must close explicitly, while a translated
    /// PostgreSQL ERROR must abandon the handles for transaction-abort cleanup.
    struct fragment_read_resources {
        Relation relation = nullptr;
        Relation ordered_index_relation = nullptr;
        SysScanDesc scan = nullptr;
        bool ordered_scan = false;
        bytea* detoasted = nullptr;
        bool detoasted_is_copy = false;
        LOCKMODE relation_lock = NoLock;
        bool abandoned = false;

        explicit fragment_read_resources(LOCKMODE lock) noexcept
            : relation_lock(lock) {}
        fragment_read_resources(
            const fragment_read_resources&) = delete;
        fragment_read_resources& operator=(
            const fragment_read_resources&) = delete;
        ~fragment_read_resources() {
            DEBUG_ASSERT(
                abandoned
                    || (relation == nullptr && scan == nullptr
                        && ordered_index_relation == nullptr
                        && detoasted == nullptr),
                "fragment read resources require checked close or abandonment");
        }

        [[nodiscard]] Relation release_relation() noexcept {
            DEBUG_ASSERT(scan == nullptr
                    && ordered_index_relation == nullptr
                    && detoasted == nullptr,
                "cannot transfer a relation with active fragment read resources");
            return std::exchange(relation, nullptr);
        }

        void release_detoasted_checked() {
            if (detoasted_is_copy && detoasted != nullptr) {
                bytea* copy = detoasted;
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [copy] { pfree(copy); });
            }
            detoasted = nullptr;
            detoasted_is_copy = false;
        }

        void close_checked() {
            release_detoasted_checked();
            if (scan != nullptr) {
                SysScanDesc finished_scan = scan;
                const bool finished_scan_is_ordered = ordered_scan;
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [finished_scan, finished_scan_is_ordered] {
                        if (finished_scan_is_ordered) {
                            systable_endscan_ordered(finished_scan);
                        } else {
                            systable_endscan(finished_scan);
                        }
                    });
                scan = nullptr;
                ordered_scan = false;
            }
            if (ordered_index_relation != nullptr) {
                Relation finished_index = ordered_index_relation;
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [finished_index] {
                        index_close(finished_index, AccessShareLock);
                    });
                ordered_index_relation = nullptr;
            }
            if (relation != nullptr) {
                Relation finished_relation = relation;
                const LOCKMODE finished_lock = relation_lock;
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [finished_relation, finished_lock] {
                        relation_close(finished_relation, finished_lock);
                    });
                relation = nullptr;
            }
        }

        void abandon_after_pg_error_noexcept() noexcept {
            relation = nullptr;
            ordered_index_relation = nullptr;
            scan = nullptr;
            ordered_scan = false;
            detoasted = nullptr;
            detoasted_is_copy = false;
            abandoned = true;
        }

        [[noreturn]] void close_then_rethrow_cpp(
                std::exception_ptr error) {
            try {
                close_checked();
            } catch (...) {
                abandon_after_pg_error_noexcept();
                throw;
            }
            std::rethrow_exception(error);
        }
    };
    static_assert(!std::is_copy_constructible_v<
        fragment_read_resources>);
    static_assert(!std::is_copy_assignable_v<
        fragment_read_resources>);
    static_assert(std::is_nothrow_destructible_v<
        fragment_read_resources>);
    static_assert(noexcept(
        std::declval<fragment_read_resources&>()
            .abandon_after_pg_error_noexcept()));

    /// Own every PostgreSQL object used by the direct fragment writer while
    /// the surrounding minimizer build still owns vectors, run stores and
    /// admission permits.  The destructor deliberately performs no PostgreSQL
    /// work: normal paths close explicitly and translated PostgreSQL errors
    /// abandon the handles for transaction-abort cleanup.
    struct fragment_write_resources {
        Relation relation = nullptr;
        SysScanDesc scan = nullptr;
        HeapTuple tuple = nullptr;
        bytea* blob = nullptr;
        LOCKMODE relation_lock = NoLock;
        bool abandoned = false;

        explicit fragment_write_resources(LOCKMODE lock) noexcept
            : relation_lock(lock) {}
        fragment_write_resources(
            const fragment_write_resources&) = delete;
        fragment_write_resources& operator=(
            const fragment_write_resources&) = delete;
        ~fragment_write_resources() {
            DEBUG_ASSERT(
                abandoned
                    || (relation == nullptr && scan == nullptr
                        && tuple == nullptr && blob == nullptr),
                "fragment write resources require checked close or abandonment");
        }

        void release_tuple_checked() {
            if (tuple != nullptr) {
                HeapTuple finished_tuple = tuple;
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [finished_tuple] { heap_freetuple(finished_tuple); });
                tuple = nullptr;
            }
        }

        void release_blob_checked() {
            if (blob != nullptr) {
                bytea* finished_blob = blob;
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [finished_blob] { pfree(finished_blob); });
                blob = nullptr;
            }
        }

        void finish_scan_checked() {
            if (scan != nullptr) {
                SysScanDesc finished_scan = scan;
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [finished_scan] { systable_endscan(finished_scan); });
                scan = nullptr;
            }
        }

        void finish_row_checked() {
            release_tuple_checked();
            release_blob_checked();
            finish_scan_checked();
        }

        void close_checked() {
            finish_row_checked();
            if (relation != nullptr) {
                Relation finished_relation = relation;
                const LOCKMODE finished_lock = relation_lock;
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [finished_relation, finished_lock] {
                        relation_close(finished_relation, finished_lock);
                    });
                relation = nullptr;
            }
        }

        void abandon_after_pg_error_noexcept() noexcept {
            relation = nullptr;
            scan = nullptr;
            tuple = nullptr;
            blob = nullptr;
            abandoned = true;
        }

        [[noreturn]] void close_then_rethrow_cpp(
                std::exception_ptr error) {
            try {
                close_checked();
            } catch (...) {
                abandon_after_pg_error_noexcept();
                throw;
            }
            std::rethrow_exception(error);
        }
    };
    static_assert(!std::is_copy_constructible_v<
        fragment_write_resources>);
    static_assert(!std::is_copy_assignable_v<
        fragment_write_resources>);
    static_assert(std::is_nothrow_destructible_v<
        fragment_write_resources>);
    static_assert(noexcept(
        std::declval<fragment_write_resources&>()
            .abandon_after_pg_error_noexcept()));

    // Authority-bearing minimizer-sequence spill state is intentionally not part of the
    // otherwise duck-typed storage surface.  Callers may observe its immutable
    // binding and mutate it only through the narrow logical-event APIs below.
    using segment_run_entry_type =
        std::pair<std::uint32_t, sorted_pair_run_store>;
    // Reverse destruction drops stores/vector nodes before metadata admission.
    bioseq::parallelism::memory_permit segment_sorted_runs_metadata_permit_;
    std::vector<segment_run_entry_type> segment_sorted_runs_;
    inline static std::atomic<std::uint64_t>
        next_segment_spill_generation_token_{1u};
    direct_minimizer_spill_storage_binding segment_spill_binding_{};
    bool segment_spill_pg_poisoned_ = false;
    void record_segment_spill_mutation_() noexcept;
    [[nodiscard]] bool ensure_segment_run_metadata_capacity_(
            std::size_t required) {
        const std::uint64_t retained = bioseq::parallelism::saturating_mul(
            static_cast<std::uint64_t>(segment_sorted_runs_.capacity()),
            static_cast<std::uint64_t>(sizeof(segment_run_entry_type)));
        if (retained == (std::numeric_limits<std::uint64_t>::max)()
            || (retained != 0u
                && (!segment_sorted_runs_metadata_permit_
                    || segment_sorted_runs_metadata_permit_.bytes()
                        < retained))) {
            return false;
        }
        if (segment_sorted_runs_.capacity() >= required) {
            return true;
        }
        const std::uint64_t headroom = bioseq::parallelism::saturating_mul(
            bioseq::parallelism::saturating_mul(
                static_cast<std::uint64_t>(required),
                static_cast<std::uint64_t>(sizeof(segment_run_entry_type))),
            bioseq::parallelism::k_vector_preallocation_headroom_factor);
        const std::uint64_t peak = bioseq::parallelism::saturating_add(
            retained, headroom);
        if (headroom == (std::numeric_limits<std::uint64_t>::max)()
            || peak == (std::numeric_limits<std::uint64_t>::max)()) {
            return false;
        }
        if (segment_sorted_runs_metadata_permit_) {
            if (!segment_sorted_runs_metadata_permit_.grow_to(peak)) {
                return false;
            }
        } else {
            auto permit =
                bioseq::parallelism::memory_admission_controller::instance()
                    .try_reserve(peak);
            if (!permit) {
                return false;
            }
            segment_sorted_runs_metadata_permit_ = std::move(*permit);
        }
        segment_sorted_runs_.reserve(required);
        const std::uint64_t exact = bioseq::parallelism::saturating_mul(
            static_cast<std::uint64_t>(segment_sorted_runs_.capacity()),
            static_cast<std::uint64_t>(sizeof(segment_run_entry_type)));
        if (exact == (std::numeric_limits<std::uint64_t>::max)()
            || exact > segment_sorted_runs_metadata_permit_.bytes()) {
            return false;
        }
        segment_sorted_runs_metadata_permit_.shrink_to(exact);
        return true;
    }

public:

    fragment_index_storage() = default;
    fragment_index_storage(const fragment_index_storage&) = delete;
    fragment_index_storage& operator=(const fragment_index_storage&) = delete;
    fragment_index_storage(fragment_index_storage&& o) noexcept { steal_(std::move(o)); }
    fragment_index_storage& operator=(fragment_index_storage&&) = delete;
    ~fragment_index_storage() { release_(); }

    void steal_(fragment_index_storage&& o) noexcept {
        db_oid_=o.db_oid_; tablespace_oid_=o.tablespace_oid_; main_relfilenode_=o.main_relfilenode_; user_relid_=o.user_relid_;
        read_caches_=o.read_caches_; column_attno_=o.column_attno_; index_name_=o.index_name_;
        bucket_idx_=o.bucket_idx_; shard_idx_=o.shard_idx_; query_key_=o.query_key_;
        own_mctx_=o.own_mctx_; index_relid_=o.index_relid_;
        decoded_=o.decoded_; decoded_units_=o.decoded_units_; decoded_bytes_=o.decoded_bytes_;
        decoded_sub_=o.decoded_sub_; decoded_chunk_=o.decoded_chunk_;
        chunk_cache_=std::move(o.chunk_cache_);
        chunk_cache_clock_=o.chunk_cache_clock_;
        chunk_cache_bytes_=o.chunk_cache_bytes_;
        scratch_=o.scratch_; scratch_units_=o.scratch_units_;
        run_buffer_override_bytes_=o.run_buffer_override_bytes_;
        chunk_payload_bytes_=std::move(o.chunk_payload_bytes_);
        sorted_runs_.swap(o.sorted_runs_);
        segment_sorted_runs_.swap(o.segment_sorted_runs_);
        segment_sorted_runs_metadata_permit_ =
            std::move(o.segment_sorted_runs_metadata_permit_);
        segment_spill_binding_=o.segment_spill_binding_;
        segment_spill_pg_poisoned_=o.segment_spill_pg_poisoned_;

        // A moved-from fragment is not merely destruction-safe: it is fully
        // unbound and cannot still describe or access the transferred entity.
        o.db_oid_=InvalidOid; o.tablespace_oid_=InvalidOid;
        o.main_relfilenode_=InvalidOid; o.user_relid_=InvalidOid;
        o.read_caches_=nullptr; o.column_attno_=0; o.index_name_={};
        o.bucket_idx_=0; o.shard_idx_=0; o.query_key_=0;
        o.own_mctx_=nullptr; o.index_relid_=InvalidOid;
        o.decoded_=nullptr; o.decoded_units_=0; o.decoded_bytes_=0;
        o.decoded_sub_=k_directory_sub_key;
        o.decoded_chunk_=(std::numeric_limits<std::size_t>::max)();
        {
            std::vector<cached_chunk> empty;
            o.chunk_cache_.swap(empty);
        }
        o.chunk_cache_clock_=0; o.chunk_cache_bytes_=0;
        o.scratch_=nullptr; o.scratch_units_=0;
        o.run_buffer_override_bytes_=0;
        o.chunk_payload_bytes_.clear();
        o.segment_spill_binding_={};
        o.segment_spill_pg_poisoned_=true;
        o.sorted_runs_.reset();
        bioseq::parallelism::release_admitted_vector_capacity_before_permit(
            o.segment_sorted_runs_,
            o.segment_sorted_runs_metadata_permit_);
    }
    void release_resident_runs_noexcept() noexcept {
        if (sorted_runs_.has_value()) {
            sorted_runs_->release_resident_runs_noexcept();
        }
        for (auto& [segment, runs] : segment_sorted_runs_) {
            (void)segment;
            runs.release_resident_runs_noexcept();
        }
        // sorted_pair_run_store::release_resident_runs_noexcept drops logical
        // memory-only runs; unlike its internal memory-to-BufFile compaction,
        // this is not representation-preserving and must invalidate replay.
        record_segment_spill_mutation_();
    }
    void release_() noexcept {
        DEBUG_ASSERT(own_mctx_ == nullptr,
            "fragment_index_storage must be close_checked or abandoned before destruction");
        DEBUG_ASSERT(!sorted_runs_.has_value()
                && segment_sorted_runs_.empty(),
            "fragment_index_storage run stores must be close_checked or abandoned before destruction");
        release_runs_noexcept();
        chunk_cache_.clear();
        chunk_cache_bytes_ = 0;
        // Destruction is an unwind-only abandonment boundary. A normal owner
        // must call close_checked(); a failing owner must call
        // abandon_after_pg_error_noexcept(). The parent PostgreSQL context
        // reclaims abandoned storage at statement/transaction abort.
        own_mctx_ = nullptr;
        decoded_ = nullptr;
        scratch_ = nullptr;
    }

    [[noreturn]] void index_error(std::string_view message) const {
        const char* message_data = message.empty() ? "" : message.data();
        const int message_size = static_cast<int>(std::min(
            message.size(),
            static_cast<std::size_t>((std::numeric_limits<int>::max)())));
        bioseq::postgres::support::invoke_pg_as_cpp(
            [message_data, message_size] {
                ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq index: %.*s",
                        message_size, message_data)));
            });
        pg_unreachable();
    }

    // ---- validators (translate to ereport on the pg side) ----
    [[nodiscard]] minimizer_hashmap_fragment_index::encoded_header validate_minimizer_blob_or_error(
            const char* p, std::size_t bytes, std::optional<std::uint32_t> expected_bucket_count = std::nullopt) const {
        using header_type = minimizer_hashmap_fragment_index::encoded_header;
        using bucket_type = minimizer_hashmap_fragment_index::encoded_bucket;
        if (bytes < sizeof(header_type)) index_error("minimizer blob truncated header");
        header_type header{};
        std::memcpy(&header, p, sizeof(header_type));
        if (header.magic != minimizer_hashmap_fragment_index::k_encoded_magic
            || header.layout_revision
                != minimizer_hashmap_fragment_index::k_encoded_layout_revision)
            index_error("minimizer blob magic/layout revision");
        const std::size_t expect = sizeof(header_type)
            + sizeof(bucket_type) * header.bucket_count
            + sizeof(std::uint64_t) * header.total_positions;
        if (bytes != expect) index_error("minimizer blob size mismatch");
        if (expected_bucket_count
            && header.bucket_count != *expected_bucket_count) {
            index_error("minimizer blob bucket_count mismatch");
        }
        return header;
    }
    void validate_minimizer_directory_or_error(const char* p, std::size_t bytes) const {
        if (bytes < 40) index_error("directory truncated");
        std::uint32_t magic{};
        std::uint32_t format_revision{};
        std::memcpy(&magic, p, 4);
        std::memcpy(&format_revision, p + 4, 4);
        if (magic != 0xB1000002u || format_revision != 2u) {
            index_error("directory magic/format revision");
        }
    }
    void validate_minimizer_chunk_metadata_or_error(index_sub_key_t sk, std::size_t chunk_no,
            std::size_t min_bytes, std::size_t /*alignment*/) const {
        if (chunk_byte_size(sk, chunk_no) < min_bytes) index_error("chunk metadata below minimum size");
    }

    // ---- relation resolution ----
    [[nodiscard]] std::string this_table_name() const {
        return format_seqidx_name(index_name_, db_oid_, tablespace_oid_, main_relfilenode_, column_attno_, bucket_idx_, shard_idx_);
    }

    /// Remove every backend-local object derived from this logical minimizer
    /// sidecar.  Relcache invalidation supplies a physical sidecar OID, whereas
    /// the cache keys use the owning user relation, so schema replacement must
    /// evict by the complete logical identity explicitly.
    void evict_generation_storage_residency_() {
        chunk_cache_.clear();
        chunk_cache_bytes_ = 0;
        chunk_cache_clock_ = 0;
        decoded_sub_ = k_directory_sub_key;
        decoded_chunk_ = (std::numeric_limits<std::size_t>::max)();
        decoded_bytes_ = 0;
        evict_index_residency(
            db_oid_, user_relid_, tablespace_oid_, main_relfilenode_,
            column_attno_,
            index_name_hash(db_oid_, tablespace_oid_, main_relfilenode_,
                column_attno_, index_name_));
    }

    /// Ensure the same-name minimizer sidecar has the one canonical generation
    /// schema. Normal ensure is deliberately non-destructive: an exact relation
    /// is reused by OID and any malformed same-name object fails closed.
    /// Explicit clear/reset owns the destructive replacement boundary.
    [[nodiscard]] Oid ensure_minimizer_generation_storage() {
        if (index_name_ != k_minimizers_per_fragment_index_name
            && index_name_ != k_minimizers_per_sequence_index_name) {
            minimizer_generation_storage_error(
                "generation storage is only valid for minimizer indexes");
        }
        if (db_oid_ != MyDatabaseId || !OidIsValid(user_relid_)
            || !OidIsValid(tablespace_oid_)
            || !OidIsValid(main_relfilenode_) || column_attno_ <= 0) {
            minimizer_generation_storage_error("generation storage identity is invalid");
        }
        acquire_index_advisory_xact_lock(
            db_oid_, tablespace_oid_, main_relfilenode_, column_attno_,
            index_name_);

        Relation source_relation = bioseq::postgres::support::try_open_postgres_relation(
            user_relid_, AccessShareLock);
        if (source_relation == nullptr
            || source_relation->rd_rel->relpersistence
                == RELPERSISTENCE_TEMP
            || source_relation->rd_locator.spcOid != tablespace_oid_) {
            if (source_relation != nullptr) {
                relation_close(source_relation, AccessShareLock);
            }
            minimizer_generation_storage_error(
                "generation storage requires a permanent/unlogged source in the resolved tablespace");
        }
        relation_close(source_relation, AccessShareLock);

        const std::string relation_name = this_table_name();
        const Oid internal_namespace = bioseq_internal_ns();
        const minimizer_generation_schema_expectation expected_schema{
            .namespace_oid = internal_namespace,
            .relation_name = relation_name,
            .tablespace_oid = tablespace_oid_,
        };
        index_relid_ = InvalidOid;
        Oid relation_oid = get_relname_relid(
            relation_name.c_str(), internal_namespace);

        if (OidIsValid(relation_oid)) {
            Relation relation = bioseq::postgres::support::try_open_postgres_relation(
                relation_oid, AccessShareLock);
            if (relation == nullptr
                || RelationGetNamespace(relation) != internal_namespace
                || std::string_view{RelationGetRelationName(relation)}
                    != relation_name) {
                if (relation != nullptr) {
                    relation_close(relation, AccessShareLock);
                }
                minimizer_generation_storage_error(
                    "generation relation disappeared during schema inspection");
            }
            const minimizer_generation_schema_inspection schema =
                inspect_minimizer_generation_schema(
                    relation, nullptr, expected_schema);
            if (schema) {
                // Keep the AccessShare relation lock until transaction end so
                // external DDL cannot replace the validated tuple descriptor
                // before the row sink opens it.
                evict_generation_storage_residency_();
                relation_close(relation, NoLock);
                return relation_oid;
            }
            relation_close(relation, AccessShareLock);
            minimizer_generation_storage_error(fmt::format(
                "existing generation relation requires explicit clear/reset: {}",
                minimizer_generation_schema_error_name(schema.error)));
        }

        char* raw_tablespace_name = get_tablespace_name(tablespace_oid_);
        if (raw_tablespace_name == nullptr) {
            minimizer_generation_storage_error("resolved source tablespace no longer exists");
        }
        const std::string tablespace_name{raw_tablespace_name};
        pfree(raw_tablespace_name);
        const std::string primary_key_name = fmt::format(
            "seqidxpk_{:016x}_{:02x}_{:08x}",
            identity_hash64(db_oid_, tablespace_oid_, main_relfilenode_,
                column_attno_, index_name_),
            static_cast<unsigned>(bucket_idx_),
            static_cast<unsigned>(shard_idx_));
        run_internal_ddl(fmt::format(
            "CREATE UNLOGGED TABLE bioseq_internal.{} ("
            "query_key BIGINT NOT NULL, generation_id BIGINT NOT NULL, "
            "position_segment INTEGER NOT NULL, sub_key BIGINT NOT NULL, "
            "chunk_no INTEGER NOT NULL, chunk_bytes INTEGER NOT NULL, "
            "checksum BIGINT NOT NULL, blob BYTEA NOT NULL, "
            "CONSTRAINT {} PRIMARY KEY "
            "(generation_id,query_key,sub_key,position_segment,chunk_no) "
            "WITH (fillfactor=90) USING INDEX TABLESPACE {}) "
            "USING heap WITH (fillfactor=100) TABLESPACE {}",
            quote_identifier(relation_name.c_str()),
            quote_identifier(primary_key_name.c_str()),
            quote_identifier(tablespace_name.c_str()),
            quote_identifier(tablespace_name.c_str())).c_str());
        run_internal_ddl(fmt::format(
            "ALTER TABLE bioseq_internal.{} "
            "ALTER COLUMN blob SET STORAGE PLAIN",
            quote_identifier(relation_name.c_str())).c_str());
        CommandCounterIncrement();

        relation_oid = get_relname_relid(
            relation_name.c_str(), internal_namespace);
        Relation created = OidIsValid(relation_oid)
            ? bioseq::postgres::support::try_open_postgres_relation(relation_oid, AccessShareLock)
            : nullptr;
        if (created == nullptr
            || RelationGetNamespace(created) != internal_namespace
            || std::string_view{RelationGetRelationName(created)}
                != relation_name) {
            if (created != nullptr) {
                relation_close(created, AccessShareLock);
            }
            minimizer_generation_storage_error(
                "canonical generation relation was not created");
        }
        const minimizer_generation_schema_inspection created_schema =
            inspect_minimizer_generation_schema(created, nullptr, expected_schema);
        if (!created_schema) {
            relation_close(created, AccessShareLock);
            minimizer_generation_storage_error(fmt::format(
                "created generation relation schema mismatch: {}",
                minimizer_generation_schema_error_name(created_schema.error)));
        }
        relation_close(created, NoLock);
        CacheInvalidateRelcacheByRelid(relation_oid);
        CacheInvalidateRelcacheByRelid(created_schema.primary_index_oid);
        // Broadcast on the owning relation because every residency key carries
        // `user_relid`, not the physical sidecar OID. Other backends can evict
        // cached bytes after this first canonical sidecar creation commits.
        CacheInvalidateRelcacheByRelid(user_relid_);
        evict_generation_storage_residency_();
        return relation_oid;
    }

    /// Transactionally remove the complete physical generation family for
    /// this one logical minimizer index.  Head, manifest, immutable payload,
    /// and temporary-run objects are row classes in the canonical sidecars,
    /// so dropping every table under the exact identity prefix clears all of
    /// them together.
    [[nodiscard]] minimizer_generation_clear_result
    clear_minimizer_generation_storage() {
        struct physical_relation {
            Oid oid = InvalidOid;
            std::string name;
            std::uint8_t bucket_idx = 0;
            std::uint32_t shard_idx = 0;
        };
        struct clear_state {
            bool spi_active = false;
            Relation locked_relation = nullptr;
            LOCKMODE locked_relation_mode = NoLock;
            char* spi_text = nullptr;
            char* quoted_literal = nullptr;
            Oid internal_namespace = InvalidOid;
            std::string index_name;
            std::string prefix;
            std::string owned_shards_sql;
            std::string discover_sql;
            std::string ddl_sql;
            std::vector<std::pair<std::uint8_t, std::uint32_t>>
                owned_shards;
            std::vector<physical_relation> candidates;
            std::vector<physical_relation> relations;
            std::vector<physical_relation> verification_candidates;
            minimizer_generation_clear_result result{};
            std::exception_ptr cpp_error;
            bool completed = false;
        };
        auto* state = new (std::nothrow) clear_state{};
        if (state == nullptr) {
            ereport(ERROR,
                (errcode(ERRCODE_OUT_OF_MEMORY),
                 errmsg("bioseq current-generation index: generation clear state allocation failed")));
        }
        MemoryContext saved_context = CurrentMemoryContext;
        ErrorData* volatile postgres_error = nullptr;

        PG_TRY();
        {
            try {
                if (index_name_ != k_minimizers_per_fragment_index_name
                    && index_name_ != k_minimizers_per_sequence_index_name) {
                    minimizer_generation_storage_error(
                        "generation clear is only valid for minimizer indexes");
                }
                if (db_oid_ != MyDatabaseId || !OidIsValid(user_relid_)
                    || !OidIsValid(tablespace_oid_)
                    || !OidIsValid(main_relfilenode_)
                    || column_attno_ <= 0) {
                    minimizer_generation_storage_error("generation clear identity is invalid");
                }

                acquire_index_advisory_xact_lock(
                    db_oid_, tablespace_oid_, main_relfilenode_,
                    column_attno_, index_name_);

                // A failed operation or enclosing savepoint rollback retains
                // no stale local object; the next access resolves by name.
                index_relid_ = InvalidOid;
                chunk_payload_bytes_.clear();
                release_runs();
                evict_generation_storage_residency_();

                state->prefix = fmt::format(
                    "seqidx_{}_{:016x}_", short_index_for(index_name_),
                    identity_hash64(db_oid_, tablespace_oid_,
                        main_relfilenode_, column_attno_, index_name_));
                state->index_name.assign(index_name_);
                state->internal_namespace = bioseq_internal_ns();
                state->owned_shards_sql = fmt::format(
                    "SELECT bucket_idx::integer, shard_idx "
                    "FROM bioseq_internal.seq_sub_table "
                    "WHERE database_oid={}::oid "
                    "AND tablespace_oid={}::oid "
                    "AND main_relfilenode={}::oid "
                    "AND column_attno={}::smallint "
                    "ORDER BY bucket_idx, shard_idx",
                    static_cast<unsigned>(db_oid_),
                    static_cast<unsigned>(tablespace_oid_),
                    static_cast<unsigned>(main_relfilenode_),
                    static_cast<int>(column_attno_));
                state->quoted_literal =
                    quote_literal_cstr(state->prefix.c_str());
                state->discover_sql = fmt::format(
                    "SELECT c.oid, c.relname::text FROM pg_class c "
                    "WHERE c.relnamespace={}::oid "
                    "AND c.relkind='r' "
                    "AND left(c.relname::text, length({}))={} "
                    "ORDER BY c.oid",
                    static_cast<unsigned>(state->internal_namespace),
                    state->quoted_literal, state->quoted_literal);
                pfree(state->quoted_literal);
                state->quoted_literal = nullptr;

                const int discovery_connect_rc = SPI_connect();
                if (discovery_connect_rc != SPI_OK_CONNECT) {
                    ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("bioseq current-generation index: generation clear discovery SPI_connect failed"),
                         errdetail("SPI rc=%d", discovery_connect_rc)));
                }
                state->spi_active = true;
                const int owned_rc = SPI_execute(
                    state->owned_shards_sql.c_str(), true, 0);
                if (owned_rc != SPI_OK_SELECT) {
                    ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("bioseq current-generation index: generation clear owned-shard discovery failed"),
                         errdetail("SPI rc=%d", owned_rc)));
                }
                state->owned_shards.reserve(
                    static_cast<std::size_t>(SPI_processed));
                for (std::uint64_t row = 0; row < SPI_processed; ++row) {
                    bool bucket_is_null = false;
                    bool shard_is_null = false;
                    const std::int32_t bucket = DatumGetInt32(SPI_getbinval(
                        SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 1,
                        &bucket_is_null));
                    const std::int32_t shard = DatumGetInt32(SPI_getbinval(
                        SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 2,
                        &shard_is_null));
                    if (bucket_is_null || shard_is_null || bucket < 0
                        || bucket > (std::numeric_limits<std::uint8_t>::max)()
                        || shard < 0) {
                        minimizer_generation_storage_error(
                            "generation clear found an invalid owned shard identity");
                    }
                    state->owned_shards.emplace_back(
                        static_cast<std::uint8_t>(bucket),
                        static_cast<std::uint32_t>(shard));
                }

                const int discover_rc = SPI_execute(
                    state->discover_sql.c_str(), true, 0);
                if (discover_rc != SPI_OK_SELECT) {
                    ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("bioseq current-generation index: generation clear relation discovery failed"),
                         errdetail("SPI rc=%d", discover_rc)));
                }
                state->candidates.reserve(
                    static_cast<std::size_t>(SPI_processed));
                for (std::uint64_t row = 0; row < SPI_processed; ++row) {
                    bool oid_is_null = false;
                    const Datum oid_datum = SPI_getbinval(
                        SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 1,
                        &oid_is_null);
                    state->spi_text = SPI_getvalue(
                        SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 2);
                    if (oid_is_null || state->spi_text == nullptr) {
                        minimizer_generation_storage_error(
                            "generation clear discovery returned a NULL identity");
                    }
                    state->candidates.emplace_back();
                    physical_relation& candidate = state->candidates.back();
                    candidate.oid = DatumGetObjectId(oid_datum);
                    candidate.name.assign(state->spi_text);
                    pfree(state->spi_text);
                    state->spi_text = nullptr;
                    if (!OidIsValid(candidate.oid)
                        || !candidate.name.starts_with(state->prefix)) {
                        minimizer_generation_storage_error(
                            "generation clear discovery escaped its exact prefix");
                    }
                }
                const int discovery_finish_rc = SPI_finish();
                state->spi_active =
                    discovery_finish_rc != SPI_OK_FINISH;
                if (discovery_finish_rc != SPI_OK_FINISH) {
                    ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("bioseq current-generation index: generation clear discovery SPI_finish failed"),
                         errdetail("SPI rc=%d", discovery_finish_rc)));
                }

                // A broad prefix match is only a candidate.  Ownership also
                // requires canonical decimal spelling, an exact source shard
                // catalog identity, and the exact generation tuple schema.
                for (std::size_t candidate_index = 0;
                     candidate_index < state->candidates.size();
                     ++candidate_index) {
                    physical_relation& candidate =
                        state->candidates[candidate_index];
                    const canonical_generation_sidecar_name parsed =
                        parse_canonical_generation_sidecar_name(
                            candidate.name, state->prefix);
                    if (!parsed.valid) continue;
                    bool owned = false;
                    for (const auto& shard : state->owned_shards) {
                        if (shard.first == parsed.bucket_idx
                            && shard.second == parsed.shard_idx) {
                            owned = true;
                            break;
                        }
                    }
                    if (!owned) continue;
                    state->locked_relation = bioseq::postgres::support::try_open_postgres_relation(
                        candidate.oid, AccessShareLock);
                    state->locked_relation_mode = AccessShareLock;
                    if (state->locked_relation == nullptr
                        || get_relname_relid(candidate.name.c_str(),
                            state->internal_namespace) != candidate.oid
                        || RelationGetNamespace(state->locked_relation)
                            != state->internal_namespace
                        || std::string_view{RelationGetRelationName(
                            state->locked_relation)} != candidate.name) {
                        minimizer_generation_storage_error(
                            "generation clear candidate identity changed during inspection");
                    }
                    const minimizer_generation_schema_inspection schema =
                        inspect_minimizer_generation_schema(state->locked_relation);
                    relation_close(
                        state->locked_relation, AccessShareLock);
                    state->locked_relation = nullptr;
                    state->locked_relation_mode = NoLock;
                    if (!schema) continue;
                    candidate.bucket_idx = parsed.bucket_idx;
                    candidate.shard_idx = parsed.shard_idx;
                    state->relations.push_back(candidate);
                }

                for (std::size_t relation_index = 0;
                     relation_index < state->relations.size();
                     ++relation_index) {
                    physical_relation& target =
                        state->relations[relation_index];
                    state->locked_relation = bioseq::postgres::support::try_open_postgres_relation(
                        target.oid, AccessExclusiveLock);
                    state->locked_relation_mode = AccessExclusiveLock;
                    if (state->locked_relation == nullptr
                        || get_relname_relid(target.name.c_str(),
                            state->internal_namespace) != target.oid
                        || RelationGetNamespace(state->locked_relation)
                            != state->internal_namespace
                        || std::string_view{RelationGetRelationName(
                            state->locked_relation)} != target.name) {
                        minimizer_generation_storage_error(
                            "generation relation identity changed while acquiring clear lock");
                    }
                    const minimizer_generation_schema_inspection locked_schema =
                        inspect_minimizer_generation_schema(state->locked_relation);
                    if (!locked_schema) {
                        minimizer_generation_storage_error(
                            "generation relation schema changed while acquiring clear lock");
                    }
                    const Oid primary_index =
                        locked_schema.primary_index_oid;
                    CacheInvalidateRelcacheByRelid(target.oid);
                    CacheInvalidateRelcacheByRelid(primary_index);
                    relation_close(state->locked_relation, NoLock);
                    state->locked_relation = nullptr;
                    state->locked_relation_mode = NoLock;

                    state->ddl_sql.assign(
                        "DROP TABLE bioseq_internal.");
                    state->ddl_sql.append(
                        quote_identifier(target.name.c_str()));
                    run_checked_generation_clear_ddl(
                        state->ddl_sql.c_str(), state->spi_active);
                    CommandCounterIncrement();
                    if (OidIsValid(get_relname_relid(
                            target.name.c_str(),
                            state->internal_namespace))) {
                        minimizer_generation_storage_error(
                            "generation clear DDL retained an owned relation");
                    }
                }
                state->result.dropped_relation_count =
                    static_cast<std::uint64_t>(state->relations.size());

                state->result.deleted_metadata_age_rows =
                    delete_metadata_age_log_pg(
                        user_relid_,
                        main_relfilenode_,
                        column_attno_,
                        state->index_name);
                CommandCounterIncrement();

                // Re-discover broadly, but fail only if a canonical,
                // catalog-owned, exact generation relation survived.  Benign
                // near-prefix relations are deliberately ignored.
                const int verify_connect_rc = SPI_connect();
                if (verify_connect_rc != SPI_OK_CONNECT) {
                    ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("bioseq current-generation index: generation clear verification SPI_connect failed"),
                         errdetail("SPI rc=%d", verify_connect_rc)));
                }
                state->spi_active = true;
                const int verify_rc = SPI_execute(
                    state->discover_sql.c_str(), true, 0);
                if (verify_rc != SPI_OK_SELECT) {
                    ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("bioseq current-generation index: generation clear verification query failed"),
                         errdetail("SPI rc=%d", verify_rc)));
                }
                state->verification_candidates.reserve(
                    static_cast<std::size_t>(SPI_processed));
                for (std::uint64_t row = 0; row < SPI_processed; ++row) {
                    bool oid_is_null = false;
                    const Datum oid_datum = SPI_getbinval(
                        SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 1,
                        &oid_is_null);
                    state->spi_text = SPI_getvalue(
                        SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 2);
                    if (oid_is_null || state->spi_text == nullptr) {
                        minimizer_generation_storage_error(
                            "generation clear verification returned a NULL identity");
                    }
                    state->verification_candidates.emplace_back();
                    physical_relation& candidate =
                        state->verification_candidates.back();
                    candidate.oid = DatumGetObjectId(oid_datum);
                    candidate.name.assign(state->spi_text);
                    pfree(state->spi_text);
                    state->spi_text = nullptr;
                }
                const int verify_finish_rc = SPI_finish();
                state->spi_active =
                    verify_finish_rc != SPI_OK_FINISH;
                if (verify_finish_rc != SPI_OK_FINISH) {
                    ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("bioseq current-generation index: generation clear verification SPI_finish failed"),
                         errdetail("SPI rc=%d", verify_finish_rc)));
                }
                for (std::size_t candidate_index = 0;
                     candidate_index
                        < state->verification_candidates.size();
                     ++candidate_index) {
                    physical_relation& candidate =
                        state->verification_candidates[candidate_index];
                    const canonical_generation_sidecar_name parsed =
                        parse_canonical_generation_sidecar_name(
                            candidate.name, state->prefix);
                    if (!parsed.valid) continue;
                    bool owned = false;
                    for (const auto& shard : state->owned_shards) {
                        if (shard.first == parsed.bucket_idx
                            && shard.second == parsed.shard_idx) {
                            owned = true;
                            break;
                        }
                    }
                    if (!owned) continue;
                    state->locked_relation = bioseq::postgres::support::try_open_postgres_relation(
                        candidate.oid, AccessShareLock);
                    if (state->locked_relation == nullptr) {
                        // The discovery snapshot may still name a relation
                        // that the preceding DROP has already removed.  This
                        // verifier rejects only an exact surviving owned
                        // sidecar; disappearance is the desired state.
                        state->locked_relation_mode = NoLock;
                        continue;
                    }
                    state->locked_relation_mode = AccessShareLock;
                    const bool exact_identity =
                        RelationGetNamespace(state->locked_relation)
                            == state->internal_namespace
                        && std::string_view{RelationGetRelationName(
                            state->locked_relation)} == candidate.name;
                    const minimizer_generation_schema_inspection schema =
                        inspect_minimizer_generation_schema(state->locked_relation);
                    relation_close(
                        state->locked_relation, AccessShareLock);
                    state->locked_relation = nullptr;
                    state->locked_relation_mode = NoLock;
                    if (exact_identity && schema) {
                        minimizer_generation_storage_error(
                            "generation clear verification retained an owned relation");
                    }
                }

                index_relid_ = InvalidOid;
                chunk_payload_bytes_.clear();
                release_runs();
                evict_generation_storage_residency_();
                CacheInvalidateRelcacheByRelid(user_relid_);
                state->completed = true;
            } catch (...) {
                state->cpp_error = std::current_exception();
            }
        }
        PG_CATCH();
        {
            MemoryContextSwitchTo(saved_context);
            ErrorData* volatile saved_error = CopyErrorData();
            FlushErrorState();
            if (state->spi_text != nullptr) {
                PG_TRY(); { pfree(state->spi_text); }
                PG_CATCH(); { FlushErrorState(); }
                PG_END_TRY();
                state->spi_text = nullptr;
            }
            if (state->quoted_literal != nullptr) {
                PG_TRY(); { pfree(state->quoted_literal); }
                PG_CATCH(); { FlushErrorState(); }
                PG_END_TRY();
                state->quoted_literal = nullptr;
            }
            if (state->locked_relation != nullptr) {
                Relation cleanup = state->locked_relation;
                const LOCKMODE cleanup_mode =
                    state->locked_relation_mode;
                state->locked_relation = nullptr;
                state->locked_relation_mode = NoLock;
                PG_TRY(); { relation_close(cleanup, cleanup_mode); }
                PG_CATCH(); { FlushErrorState(); }
                PG_END_TRY();
            }
            if (state->spi_active) {
                state->spi_active = false;
                PG_TRY(); { (void)SPI_finish(); }
                PG_CATCH(); { FlushErrorState(); }
                PG_END_TRY();
            }
            postgres_error = saved_error;
        }
        PG_END_TRY();

        if (postgres_error != nullptr) {
            ErrorData* exact_error = postgres_error;
            delete state;
            ReThrowError(exact_error);
        }

        if (state->cpp_error != nullptr) {
            if (state->spi_text != nullptr) {
                PG_TRY(); { pfree(state->spi_text); }
                PG_CATCH(); { FlushErrorState(); }
                PG_END_TRY();
                state->spi_text = nullptr;
            }
            if (state->quoted_literal != nullptr) {
                PG_TRY(); { pfree(state->quoted_literal); }
                PG_CATCH(); { FlushErrorState(); }
                PG_END_TRY();
                state->quoted_literal = nullptr;
            }
            if (state->locked_relation != nullptr) {
                Relation cleanup = state->locked_relation;
                const LOCKMODE cleanup_mode =
                    state->locked_relation_mode;
                state->locked_relation = nullptr;
                state->locked_relation_mode = NoLock;
                PG_TRY(); { relation_close(cleanup, cleanup_mode); }
                PG_CATCH(); { FlushErrorState(); }
                PG_END_TRY();
            }
            if (state->spi_active) {
                state->spi_active = false;
                PG_TRY(); { (void)SPI_finish(); }
                PG_CATCH(); { FlushErrorState(); }
                PG_END_TRY();
            }
            char cpp_error_detail[512] = "unknown C++ exception";
            try {
                std::rethrow_exception(state->cpp_error);
            } catch (const std::exception& error) {
                const char* message = error.what();
                strlcpy(cpp_error_detail,
                    message == nullptr
                        ? "std::exception without detail" : message,
                    sizeof(cpp_error_detail));
            } catch (...) {
                strlcpy(cpp_error_detail, "unknown C++ exception",
                    sizeof(cpp_error_detail));
            }
            state->cpp_error = nullptr;
            delete state;
            ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq current-generation index: generation clear failed"),
                 errdetail("C++ error=%s", cpp_error_detail)));
        }
        if (!state->completed) {
            delete state;
            minimizer_generation_storage_error(
                "generation clear did not complete or report an error");
        }
        const minimizer_generation_clear_result result = state->result;
        delete state;
        return result;
    }

    /// Open the current-generation reader against the exact physical name,
    /// bypassing the cached OID. A five-column relation therefore fails schema
    /// validation rather than entering the generation route.
    [[nodiscard]] generation_read_session begin_generation_read(
            std::uint64_t query_key,
            Snapshot executor_snapshot,
            expected_minimizer_generation_identity expected) const {
        if (query_key != query_key_) {
            minimizer_generation_storage_error("generation read changed the handle query key");
        }
        return generation_read_session{
            this_table_name(), query_key, executor_snapshot, expected,
            tablespace_oid_,
            minimizer_generation_snapshot_registration::borrow_registered};
    }

    /// Open the exact common-encoder row sink after atomically ensuring the
    /// canonical same-name relation.  The caller still owns generation
    /// encoding, complete reread, and READY-head publication.
    [[nodiscard]] generation_row_sink begin_generation_write(
            std::uint64_t query_key,
            std::uint64_t generation_id) {
        if (query_key != query_key_) {
            minimizer_generation_storage_error("generation writer changed the handle query key");
        }
        (void)ensure_minimizer_generation_storage();
        return generation_row_sink{
            minimizer_generation_administration_identity{
                .database_oid = db_oid_,
                .tablespace_oid = tablespace_oid_,
                .main_relfilenode = main_relfilenode_,
                .user_relid = user_relid_,
                .column_attno = column_attno_,
                .index_name = std::string{index_name_},
                .table_name = this_table_name(),
            },
            query_key,
            generation_id};
    }

    [[nodiscard]] hash_major_minimizer_generation_writer
    begin_hash_major_minimizer_generation_write(
            std::uint64_t query_key,
            std::uint64_t generation_id) {
        if (query_key != query_key_) {
            minimizer_generation_storage_error(
                "hash-major writer changed the handle query key");
        }
        (void)ensure_minimizer_generation_storage();
        return hash_major_minimizer_generation_writer{
            minimizer_generation_administration_identity{
                .database_oid = db_oid_,
                .tablespace_oid = tablespace_oid_,
                .main_relfilenode = main_relfilenode_,
                .user_relid = user_relid_,
                .column_attno = column_attno_,
                .index_name = std::string{index_name_},
                .table_name = this_table_name(),
            },
            query_key,
            generation_id};
    }

    [[nodiscard]] Oid resolve_relid_(bool create_if_missing) const {
        const std::string nm = this_table_name();
        const Oid ns = bioseq_internal_ns();
        const char* relation_name = nm.c_str();
        Oid rid = bioseq::postgres::support::invoke_pg_as_cpp(
            [relation_name, ns] {
                return get_relname_relid(relation_name, ns);
            });
        // The C++ handle can outlive a PostgreSQL subtransaction.  Never trust
        // a cached OID unless the authoritative namespace/name lookup still
        // resolves to exactly that OID; rollback may have restored a prior
        // relation or removed a relation created below a SAVEPOINT.
        if (OidIsValid(index_relid_) && index_relid_ == rid) {
            return index_relid_;
        }
        index_relid_ = InvalidOid;
        if (!OidIsValid(rid) && create_if_missing) {
            const Oid user_relid = user_relid_;
            const char* persistence = bioseq::postgres::support::invoke_pg_as_cpp(
                [user_relid] { return persistence_kw_for(user_relid); });
			const char* quoted_name_pg =
				bioseq::postgres::support::invoke_pg_as_cpp(
					[relation_name] {
						return quote_identifier(relation_name);
					});
			const bool quoted_name_is_copy =
				quoted_name_pg != relation_name;
			std::string quoted_name;
			try {
				quoted_name.assign(quoted_name_pg);
			} catch (...) {
				if (quoted_name_is_copy) {
					bioseq::postgres::support::invoke_pg_as_cpp(
						[quoted_name_pg] {
							pfree(const_cast<char*>(quoted_name_pg));
						});
				}
				throw;
			}
			if (quoted_name_is_copy) {
				bioseq::postgres::support::invoke_pg_as_cpp(
					[quoted_name_pg] {
						pfree(const_cast<char*>(quoted_name_pg));
					});
			}
            const std::string create_ddl = fmt::format(
                "CREATE {} TABLE IF NOT EXISTS bioseq_internal.{} ("
                "query_key BIGINT NOT NULL, sub_key BIGINT NOT NULL, chunk_no INT NOT NULL, "
                "chunk_bytes INT NOT NULL, blob BYTEA NOT NULL, "
                "PRIMARY KEY (query_key, sub_key, chunk_no)) USING heap;",
                persistence, quoted_name);
            const char* create_ddl_data = create_ddl.c_str();
            bioseq::postgres::support::invoke_pg_as_cpp(
                [create_ddl_data] { run_internal_ddl(create_ddl_data); });
            const std::string storage_ddl = fmt::format(
                "ALTER TABLE bioseq_internal.{} ALTER COLUMN blob SET STORAGE EXTERNAL;",
                quoted_name);
            const char* storage_ddl_data = storage_ddl.c_str();
            bioseq::postgres::support::invoke_pg_as_cpp(
                [storage_ddl_data] { run_internal_ddl(storage_ddl_data); });
            rid = bioseq::postgres::support::invoke_pg_as_cpp(
                [relation_name, ns] {
                    return get_relname_relid(relation_name, ns);
                });
            if (!OidIsValid(rid)) index_error("failed to create seqidx table");
        }
        index_relid_ = rid;
        return rid;
    }
    [[nodiscard]] Relation open_index_relation_(bool create_if_missing, LOCKMODE lock) const {
        const Oid rid = resolve_relid_(create_if_missing);
        if (!OidIsValid(rid)) return nullptr;
        const std::string expected_name = this_table_name();
        constexpr std::array<minimizer_generation_index_attribute_descriptor, 5>
            k_fragment_descriptors{{
                {k_attribute_query_key, "query_key", INT8OID},
                {k_attribute_sub_key, "sub_key", INT8OID},
                {k_attribute_chunk_number, "chunk_no", INT4OID},
                {k_attribute_chunk_bytes, "chunk_bytes", INT4OID},
                {k_attribute_blob, "blob", BYTEAOID},
            }};
        const Oid expected_namespace = bioseq_internal_ns();
        const char* expected_name_data = expected_name.c_str();
        const minimizer_generation_index_attribute_descriptor* descriptors =
            k_fragment_descriptors.data();
        const std::size_t descriptor_count = k_fragment_descriptors.size();
        fragment_read_resources resources{lock};
        try {
            resources.relation = bioseq::postgres::support::invoke_pg_as_cpp(
                [rid, lock] { return bioseq::postgres::support::try_open_postgres_relation(rid, lock); });
            if (resources.relation == nullptr) {
                return resources.release_relation();
            }

            // Return 1 for a physical-identity mismatch and 2 for a schema
            // mismatch. The leaf owns no C++ formatting state.
            const std::uint8_t invalid_reason =
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [relation = resources.relation, expected_namespace,
                     expected_name_data, descriptors, descriptor_count] {
                        if (relation->rd_rel->relam != HEAP_TABLE_AM_OID
                            || RelationGetNamespace(relation)
                                != expected_namespace
                            || std::strcmp(
                                RelationGetRelationName(relation),
                                expected_name_data) != 0) {
                            return std::uint8_t{1};
                        }

                        // This is the direct five-column surface. Never apply
                        // these attnos to the generation relation.
                        TupleDesc tuple_desc = RelationGetDescr(relation);
                        if (tuple_desc == nullptr
                            || tuple_desc->natts
                                != static_cast<int>(descriptor_count)) {
                            return std::uint8_t{2};
                        }
                        for (std::size_t attribute_index = 0;
                             attribute_index < descriptor_count;
                             ++attribute_index) {
                            const minimizer_generation_index_attribute_descriptor* expected =
                                descriptors + attribute_index;
                            Form_pg_attribute attribute = TupleDescAttr(
                                tuple_desc,
                                static_cast<int>(attribute_index));
                            if (attribute->attnum != expected->attno
                                || attribute->attisdropped
                                || std::strcmp(
                                    NameStr(attribute->attname),
                                    expected->name.data()) != 0
                                || attribute->atttypid != expected->type_oid
                                || !attribute->attnotnull) {
                                return std::uint8_t{2};
                            }
                        }
                        return std::uint8_t{0};
                    });
            if (invalid_reason != 0u) {
                index_relid_ = InvalidOid;
                resources.close_checked();
                const std::string error = invalid_reason == 1u
                    ? fmt::format(
                        "{} has the wrong physical identity", expected_name)
                    : fmt::format(
                        "fragment API refuses an incompatible schema for {}",
                        expected_name);
                index_error(error);
            }
            return resources.release_relation();
        } catch (const bioseq::postgres::support::postgres_error&) {
            resources.abandon_after_pg_error_noexcept();
            throw;
        } catch (...) {
            const std::exception_ptr error = std::current_exception();
            try {
                resources.close_checked();
            } catch (...) {
                resources.abandon_after_pg_error_noexcept();
                throw;
            }
            std::rethrow_exception(error);
        }
    }

    void ensure_buffer_(std::size_t units) const {
        if (decoded_units_ >= units && decoded_ != nullptr) return;
        if (own_mctx_ == nullptr) {
            index_error("fragment read buffer has no owning memory context");
        }
        if (units > MaxAllocSize / sizeof(std::int64_t)) {
            index_error("fragment read buffer exceeds PostgreSQL allocation limit");
        }
        const std::size_t bytes = units * sizeof(std::int64_t);
        MemoryContext owner = own_mctx_;
        std::int64_t* old_buffer = decoded_;
        std::int64_t* replacement = bioseq::postgres::support::invoke_pg_as_cpp(
            [owner, old_buffer, bytes] {
                MemoryContext previous = MemoryContextSwitchTo(owner);
                auto* result = old_buffer == nullptr
                    ? static_cast<std::int64_t*>(palloc(bytes))
                    : static_cast<std::int64_t*>(
                        repalloc(old_buffer, bytes));
                MemoryContextSwitchTo(previous);
                return result;
            });
        decoded_ = replacement;
        decoded_units_ = units;
    }
    [[nodiscard]] std::int64_t* scratch_buffer_(std::size_t units) {
        const std::size_t required_units = std::max<std::size_t>(units, 1u);
        if (scratch_units_ >= required_units && scratch_ != nullptr) {
            return scratch_;
        }
        if (own_mctx_ == nullptr) {
            index_error("fragment write scratch has no owning memory context");
        }
        if (required_units > MaxAllocSize / sizeof(std::int64_t)) {
            index_error("fragment write scratch exceeds PostgreSQL allocation limit");
        }
        const std::size_t bytes = required_units * sizeof(std::int64_t);
        MemoryContext owner = own_mctx_;
        std::int64_t* old_buffer = scratch_;
        std::int64_t* replacement = bioseq::postgres::support::invoke_pg_as_cpp(
            [owner, old_buffer, bytes] {
                MemoryContext previous = MemoryContextSwitchTo(owner);
                auto* result = old_buffer == nullptr
                    ? static_cast<std::int64_t*>(palloc(bytes))
                    : static_cast<std::int64_t*>(
                        repalloc(old_buffer, bytes));
                MemoryContextSwitchTo(previous);
                return result;
            });
        scratch_ = replacement;
        scratch_units_ = required_units;
        return scratch_;
    }
    [[nodiscard]] std::size_t scratch_buffer_bytes_() const { return scratch_units_ * sizeof(std::int64_t); }

    [[nodiscard]] index_chunk_key chunk_cache_key_(
            index_sub_key_t sub_key,
            std::size_t chunk_no) const noexcept {
        return index_chunk_key{
            .db_oid = db_oid_,
            .user_relid = user_relid_,
            .tablespace_oid = tablespace_oid_,
            .main_relfilenode = main_relfilenode_,
            .column_attno = column_attno_,
            .index_name_hash = index_name_hash(
                db_oid_, tablespace_oid_, main_relfilenode_, column_attno_, index_name_),
            .bucket_idx = bucket_idx_,
            .shard_idx = shard_idx_,
            .query_key = query_key_,
            .sub_key = sub_key,
            .chunk_no = static_cast<std::uint32_t>(chunk_no),
        };
    }

    void admit_cached_chunk_(
            index_sub_key_t sub_key,
            std::size_t chunk_no,
            std::span<const char> payload,
            bool written_in_current_xact) const {
        (void)store_cached_chunk(
            chunk_cache_key_(sub_key, chunk_no),
            payload,
            written_in_current_xact);
    }

    void load_chunk_into_buffer_(index_sub_key_t sub_key, std::size_t chunk_no) const {
        const index_chunk_key key = chunk_cache_key_(sub_key, chunk_no);
        if (auto cached = lookup_cached_chunk(key)) {
            const std::size_t units =
                (cached->size + sizeof(std::int64_t) - 1) / sizeof(std::int64_t);
            ensure_buffer_(units == 0 ? 1 : units);
            if (cached->size > 0) {
                std::memcpy(decoded_, cached->data, cached->size);
            }
            decoded_sub_ = sub_key;
            decoded_chunk_ = chunk_no;
            decoded_bytes_ = cached->size;
            return;
        }
        if (chunk_no
            > static_cast<std::size_t>(
                (std::numeric_limits<std::int32_t>::max)())) {
            index_error("fragment chunk number exceeds PostgreSQL INT range");
        }

        fragment_read_resources resources{AccessShareLock};
        try {
            resources.relation = open_index_relation_(
                false, AccessShareLock);
            if (resources.relation == nullptr) {
                const std::string error = fmt::format(
                    "chunk ({},{},{}) relation missing",
                    query_key_, sub_key, chunk_no);
                index_error(error);
            }

            const Oid primary_index =
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [relation = resources.relation] {
                        return bioseq::postgres::support::find_primary_key_oid(relation);
                    });
            std::array<ScanKeyData, 3> keys{};
            const Datum query_key = bioseq::postgres::support::pg_datum::from(
                static_cast<std::int64_t>(query_key_)).raw();
            const Datum stored_sub_key = bioseq::postgres::support::pg_datum::from(
                static_cast<std::int64_t>(sub_key)).raw();
            const Datum stored_chunk_no = bioseq::postgres::support::pg_datum::from(
                static_cast<std::int32_t>(chunk_no)).raw();
            ScanKeyData* key_data = keys.data();
            bioseq::postgres::support::invoke_pg_as_cpp(
                [key_data, query_key, stored_sub_key, stored_chunk_no] {
                    ScanKeyInit(
                        &key_data[0], k_attribute_query_key, BTEqualStrategyNumber,
                        F_INT8EQ, query_key);
                    ScanKeyInit(
                        &key_data[1], k_attribute_sub_key, BTEqualStrategyNumber,
                        F_INT8EQ, stored_sub_key);
                    ScanKeyInit(
                        &key_data[2], k_attribute_chunk_number, BTEqualStrategyNumber,
                        F_INT4EQ, stored_chunk_no);
                });
            resources.scan = bioseq::postgres::support::invoke_pg_as_cpp(
                [relation = resources.relation, primary_index, key_data] {
                    return systable_beginscan(
                        relation, primary_index,
                        OidIsValid(primary_index), nullptr, 3, key_data);
                });
            HeapTuple tuple = bioseq::postgres::support::invoke_pg_as_cpp(
                [scan = resources.scan] {
                    return systable_getnext(scan);
                });
            if (!HeapTupleIsValid(tuple)) {
                const std::string error = fmt::format(
                    "chunk ({},{},{}) is missing",
                    query_key_, sub_key, chunk_no);
                index_error(error);
            }

            bool is_null = false;
            const Datum blob = bioseq::postgres::support::invoke_pg_as_cpp(
                [relation = resources.relation, tuple, &is_null] {
                    return heap_getattr(
                        tuple, k_attribute_blob, RelationGetDescr(relation),
                        &is_null);
                });
            if (is_null) {
                index_error("fragment chunk blob is NULL");
            }
            auto detoasted = bioseq::postgres::support::invoke_pg_as_cpp(
                [blob] {
                    return bioseq::postgres::support::detoast_packed_as<bytea>(
                        bioseq::postgres::support::pg_datum::from_raw(blob));
                });
            resources.detoasted = detoasted.ptr;
            resources.detoasted_is_copy = detoasted.is_copy;
            detoasted.ptr = nullptr;
            detoasted.is_copy = false;

            bytea* raw = resources.detoasted;
            const std::size_t payload = VARSIZE_ANY_EXHDR(raw);
            const std::size_t units =
                (payload + sizeof(std::int64_t) - 1)
                / sizeof(std::int64_t);
            ensure_buffer_(units == 0 ? 1 : units);
            if (payload > 0) {
                std::memcpy(decoded_, VARDATA_ANY(raw), payload);
            }
            admit_cached_chunk_(
                sub_key,
                chunk_no,
                std::span<const char>(
                    reinterpret_cast<const char*>(VARDATA_ANY(raw)),
                    payload),
                false);

            resources.close_checked();
            decoded_sub_ = sub_key;
            decoded_chunk_ = chunk_no;
            decoded_bytes_ = payload;
        } catch (const bioseq::postgres::support::postgres_error&) {
            resources.abandon_after_pg_error_noexcept();
            throw;
        } catch (...) {
            const std::exception_ptr error = std::current_exception();
            try {
                resources.close_checked();
            } catch (...) {
                resources.abandon_after_pg_error_noexcept();
                throw;
            }
            std::rethrow_exception(error);
        }
    }

    // ---- const metadata queries (total: 0/empty on absent) ----
    [[nodiscard]] std::size_t number_of_sub_keys() const {
        fragment_read_resources resources{AccessShareLock};
        try {
            resources.relation = open_index_relation_(
                false, AccessShareLock);
            if (resources.relation == nullptr) return 0;
            const Oid primary_index =
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [relation = resources.relation] {
                        return bioseq::postgres::support::find_primary_key_oid(relation);
                    });
            std::array<ScanKeyData, 2> keys{};
            ScanKeyData* key_data = keys.data();
            const Datum query_key = bioseq::postgres::support::pg_datum::from(
                static_cast<std::int64_t>(query_key_)).raw();
            const Datum data_sub_key_limit =
                bioseq::postgres::support::pg_datum::from(
                    static_cast<std::int64_t>(k_temporary_run_sub_key_base)).raw();
            bioseq::postgres::support::invoke_pg_as_cpp(
                [key_data, query_key, data_sub_key_limit] {
                    ScanKeyInit(
                        &key_data[0], k_attribute_query_key, BTEqualStrategyNumber,
                        F_INT8EQ, query_key);
                    ScanKeyInit(
                        &key_data[1], k_attribute_sub_key, BTLessStrategyNumber,
                        F_INT8LT, data_sub_key_limit);
                });
            resources.scan = bioseq::postgres::support::invoke_pg_as_cpp(
                [relation = resources.relation, primary_index, key_data] {
                    return systable_beginscan(
                        relation, primary_index,
                        OidIsValid(primary_index), nullptr, 2, key_data);
                });
            std::int64_t last =
                (std::numeric_limits<std::int64_t>::min)();
            bool have_last = false;
            std::size_t count = 0;
            while (true) {
                HeapTuple tuple = bioseq::postgres::support::invoke_pg_as_cpp(
                    [scan = resources.scan] {
                        return systable_getnext(scan);
                    });
                if (!HeapTupleIsValid(tuple)) break;
                bool is_null = false;
                const Datum sub_key_datum =
                    bioseq::postgres::support::invoke_pg_as_cpp(
                        [relation = resources.relation, tuple, &is_null] {
                            return heap_getattr(
                                tuple, k_attribute_sub_key,
                                RelationGetDescr(relation), &is_null);
                        });
                if (is_null) index_error("fragment sub-key is NULL");
                const std::int64_t sub_key =
                    bioseq::postgres::support::pg_datum::to<std::int64_t>(
                        bioseq::postgres::support::pg_datum::from_raw(
                            sub_key_datum));
                if (!have_last || sub_key != last) {
                    ++count;
                    last = sub_key;
                    have_last = true;
                }
            }
            resources.close_checked();
            return count;
        } catch (const bioseq::postgres::support::postgres_error&) {
            resources.abandon_after_pg_error_noexcept();
            throw;
        } catch (...) {
            resources.close_then_rethrow_cpp(std::current_exception());
        }
    }
    [[nodiscard]] std::size_t number_of_chunks(index_sub_key_t sub_key) const {
        fragment_read_resources resources{AccessShareLock};
        try {
            resources.relation = open_index_relation_(
                false, AccessShareLock);
            if (resources.relation == nullptr) return 0;
            const Oid primary_index =
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [relation = resources.relation] {
                        return bioseq::postgres::support::find_primary_key_oid(relation);
                    });
            std::array<ScanKeyData, 2> keys{};
            ScanKeyData* key_data = keys.data();
            const Datum query_key = bioseq::postgres::support::pg_datum::from(
                static_cast<std::int64_t>(query_key_)).raw();
            const Datum stored_sub_key = bioseq::postgres::support::pg_datum::from(
                static_cast<std::int64_t>(sub_key)).raw();
            bioseq::postgres::support::invoke_pg_as_cpp(
                [key_data, query_key, stored_sub_key] {
                    ScanKeyInit(
                        &key_data[0], k_attribute_query_key, BTEqualStrategyNumber,
                        F_INT8EQ, query_key);
                    ScanKeyInit(
                        &key_data[1], k_attribute_sub_key, BTEqualStrategyNumber,
                        F_INT8EQ, stored_sub_key);
                });
            resources.scan = bioseq::postgres::support::invoke_pg_as_cpp(
                [relation = resources.relation, primary_index, key_data] {
                    return systable_beginscan(
                        relation, primary_index,
                        OidIsValid(primary_index), nullptr, 2, key_data);
                });
            std::size_t count = 0;
            while (bioseq::postgres::support::invoke_pg_as_cpp(
                    [scan = resources.scan] {
                        return systable_getnext(scan);
                    }) != nullptr) {
                ++count;
            }
            resources.close_checked();
            return count;
        } catch (const bioseq::postgres::support::postgres_error&) {
            resources.abandon_after_pg_error_noexcept();
            throw;
        } catch (...) {
            resources.close_then_rethrow_cpp(std::current_exception());
        }
    }
    [[nodiscard]] std::size_t chunk_byte_size(index_sub_key_t sub_key, std::size_t chunk_no) const {
        if (chunk_no
            > static_cast<std::size_t>(
                (std::numeric_limits<std::int32_t>::max)())) {
            index_error("fragment chunk number exceeds PostgreSQL INT range");
        }
        fragment_read_resources resources{AccessShareLock};
        try {
            resources.relation = open_index_relation_(
                false, AccessShareLock);
            if (resources.relation == nullptr) return 0;
            const Oid primary_index =
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [relation = resources.relation] {
                        return bioseq::postgres::support::find_primary_key_oid(relation);
                    });
            std::array<ScanKeyData, 3> keys{};
            ScanKeyData* key_data = keys.data();
            const Datum query_key = bioseq::postgres::support::pg_datum::from(
                static_cast<std::int64_t>(query_key_)).raw();
            const Datum stored_sub_key = bioseq::postgres::support::pg_datum::from(
                static_cast<std::int64_t>(sub_key)).raw();
            const Datum stored_chunk_no = bioseq::postgres::support::pg_datum::from(
                static_cast<std::int32_t>(chunk_no)).raw();
            bioseq::postgres::support::invoke_pg_as_cpp(
                [key_data, query_key, stored_sub_key, stored_chunk_no] {
                    ScanKeyInit(
                        &key_data[0], k_attribute_query_key, BTEqualStrategyNumber,
                        F_INT8EQ, query_key);
                    ScanKeyInit(
                        &key_data[1], k_attribute_sub_key, BTEqualStrategyNumber,
                        F_INT8EQ, stored_sub_key);
                    ScanKeyInit(
                        &key_data[2], k_attribute_chunk_number, BTEqualStrategyNumber,
                        F_INT4EQ, stored_chunk_no);
                });
            resources.scan = bioseq::postgres::support::invoke_pg_as_cpp(
                [relation = resources.relation, primary_index, key_data] {
                    return systable_beginscan(
                        relation, primary_index,
                        OidIsValid(primary_index), nullptr, 3, key_data);
                });
            HeapTuple tuple = bioseq::postgres::support::invoke_pg_as_cpp(
                [scan = resources.scan] {
                    return systable_getnext(scan);
                });
            std::size_t result = 0;
            if (HeapTupleIsValid(tuple)) {
                bool is_null = false;
                const Datum chunk_bytes_datum =
                    bioseq::postgres::support::invoke_pg_as_cpp(
                        [relation = resources.relation, tuple, &is_null] {
                            return heap_getattr(
                                tuple, k_attribute_chunk_bytes,
                                RelationGetDescr(relation), &is_null);
                        });
                if (is_null) index_error("fragment chunk byte count is NULL");
                const std::int32_t signed_result =
                    bioseq::postgres::support::pg_datum::to<std::int32_t>(
                        bioseq::postgres::support::pg_datum::from_raw(
                            chunk_bytes_datum));
                if (signed_result < 0) {
                    index_error("fragment chunk byte count is negative");
                }
                result = static_cast<std::size_t>(signed_result);
            }
            resources.close_checked();
            return result;
        } catch (const bioseq::postgres::support::postgres_error&) {
            resources.abandon_after_pg_error_noexcept();
            throw;
        } catch (...) {
            resources.close_then_rethrow_cpp(std::current_exception());
        }
    }

    template <class p_value_type>
    [[nodiscard]] const p_value_type* read_chunk(index_sub_key_t sub_key, std::size_t chunk_no) {
        static_assert(
            alignof(p_value_type) <= alignof(std::int64_t),
            "read_chunk element type requires excessive alignment");
        if (decoded_ != nullptr && decoded_sub_ == sub_key && decoded_chunk_ == chunk_no)
            return reinterpret_cast<const p_value_type*>(decoded_);
        load_chunk_into_buffer_(sub_key, chunk_no);
        return reinterpret_cast<const p_value_type*>(decoded_);
    }

    void resize(index_sub_key_t sub_key, std::size_t bytes_count) {
        const std::size_t n = bytes_count == 0
            ? 1
            : 1 + (bytes_count - 1) / k_max_index_chunk_size_bytes;
        std::vector<std::size_t> plan(n);
        std::size_t rem = bytes_count;
        for (std::size_t i = 0; i < n; ++i) { const std::size_t c = std::min(rem, k_max_index_chunk_size_bytes); plan[i] = c; rem -= c; }
        chunk_payload_bytes_[sub_key] = std::move(plan);
    }
    void resize_chunks(index_sub_key_t sub_key, std::span<const std::size_t> per_chunk_bytes) {
        std::vector<std::size_t> plan(per_chunk_bytes.begin(), per_chunk_bytes.end());
        for (std::size_t bytes : plan) {
            if (bytes > k_max_index_chunk_size_bytes) {
                index_error("fragment write chunk payload exceeds MAX");
            }
        }
        chunk_payload_bytes_[sub_key] = std::move(plan);
    }
    void prefetch_chunk(index_sub_key_t sub_key, std::size_t chunk_no) {
        if (decoded_sub_ == sub_key && decoded_chunk_ == chunk_no) {
            return;
        }
        load_chunk_into_buffer_(sub_key, chunk_no);
    }

    void upsert_chunk_row_(
            fragment_write_resources& resources,
            std::uint64_t query_key,
            index_sub_key_t sub_key,
            std::size_t chunk_no,
            std::size_t chunk_bytes,
            std::span<const char> payload) {
        if (resources.relation == nullptr) {
            index_error("fragment write relation is missing");
        }
        if (!bioseq::indexing::sql_bigint_can_represent(query_key)) {
            index_error("fragment write query key exceeds PostgreSQL BIGINT");
        }
        if (chunk_no > static_cast<std::size_t>(
                (std::numeric_limits<std::int32_t>::max)())) {
            index_error("fragment write chunk number exceeds PostgreSQL INT");
        }
        if (chunk_bytes > static_cast<std::size_t>(
                (std::numeric_limits<std::int32_t>::max)())
            || chunk_bytes != payload.size()) {
            index_error("fragment write chunk byte count is invalid");
        }
        if (payload.size() > MaxAllocSize - VARHDRSZ) {
            index_error("fragment write payload exceeds PostgreSQL allocation limit");
        }

        Relation relation = resources.relation;
        const Oid primary_index = bioseq::postgres::support::invoke_pg_as_cpp(
            [relation] { return bioseq::postgres::support::find_primary_key_oid(relation); });
        if (!OidIsValid(primary_index)) {
            index_error("fragment write relation has no primary index");
        }

        std::array<ScanKeyData, 3> keys{};
        ScanKeyData* key_data = keys.data();
        const Datum stored_query_key = bioseq::postgres::support::pg_datum::from(
            static_cast<std::int64_t>(query_key)).raw();
        const Datum stored_sub_key = bioseq::postgres::support::pg_datum::from(
            static_cast<std::int64_t>(sub_key)).raw();
        const Datum stored_chunk_no = bioseq::postgres::support::pg_datum::from(
            static_cast<std::int32_t>(chunk_no)).raw();
        bioseq::postgres::support::invoke_pg_as_cpp(
            [key_data, stored_query_key, stored_sub_key, stored_chunk_no] {
                ScanKeyInit(
                    &key_data[0], k_attribute_query_key, BTEqualStrategyNumber,
                    F_INT8EQ, stored_query_key);
                ScanKeyInit(
                    &key_data[1], k_attribute_sub_key, BTEqualStrategyNumber,
                    F_INT8EQ, stored_sub_key);
                ScanKeyInit(
                    &key_data[2], k_attribute_chunk_number, BTEqualStrategyNumber,
                    F_INT4EQ, stored_chunk_no);
            });
        resources.scan = bioseq::postgres::support::invoke_pg_as_cpp(
            [relation, primary_index, key_data] {
                return systable_beginscan(
                    relation, primary_index, true, nullptr, 3, key_data);
            });
        HeapTuple previous = bioseq::postgres::support::invoke_pg_as_cpp(
            [scan = resources.scan] { return systable_getnext(scan); });
        if (HeapTupleIsValid(previous)) {
            ItemPointerData previous_tid = previous->t_self;
            bioseq::postgres::support::invoke_pg_as_cpp(
                [relation, previous_tid]() mutable {
                    simple_heap_delete(relation, &previous_tid);
                });
        }
        resources.finish_scan_checked();

        const std::size_t allocation_bytes = VARHDRSZ + payload.size();
        resources.blob = bioseq::postgres::support::invoke_pg_as_cpp(
            [allocation_bytes] {
                return reinterpret_cast<bytea*>(palloc(allocation_bytes));
            });
        SET_VARSIZE(resources.blob, allocation_bytes);
        if (!payload.empty()) {
            std::memcpy(
                VARDATA(resources.blob), payload.data(), payload.size());
        }

        std::array<Datum, 5> values{
            stored_query_key,
            stored_sub_key,
            stored_chunk_no,
            bioseq::postgres::support::pg_datum::from(
                static_cast<std::int32_t>(chunk_bytes)).raw(),
            bioseq::postgres::support::pg_datum::from_pointer(resources.blob).raw(),
        };
        std::array<bool, 5> nulls{};
        Datum* value_data = values.data();
        bool* null_data = nulls.data();
        resources.tuple = bioseq::postgres::support::invoke_pg_as_cpp(
            [relation, value_data, null_data] {
                return heap_form_tuple(
                    RelationGetDescr(relation), value_data, null_data);
            });
        bioseq::postgres::support::invoke_pg_as_cpp(
            [relation, tuple = resources.tuple] {
                CatalogTupleInsert(relation, tuple);
            });
        resources.finish_row_checked();
    }

    void write_serialized_chunk_checked(
            std::uint64_t query_key,
            index_sub_key_t sub_key,
            std::size_t chunk_no,
            std::span<const char> payload,
            bool written_in_current_xact) {
        fragment_write_resources resources{RowExclusiveLock};
        try {
            resources.relation = open_index_relation_(
                true, RowExclusiveLock);
            if (resources.relation == nullptr) {
                index_error("serialized fragment write relation is missing");
            }
            upsert_chunk_row_(
                resources, query_key, sub_key, chunk_no,
                payload.size(), payload);
            admit_cached_chunk_(
                sub_key, chunk_no, payload, written_in_current_xact);
            resources.close_checked();
        } catch (const bioseq::postgres::support::postgres_error&) {
            resources.abandon_after_pg_error_noexcept();
            throw;
        } catch (...) {
            resources.close_then_rethrow_cpp(std::current_exception());
        }
    }

    template <class p_value_type, class p_callback>
    void write_to_each(index_sub_key_t sub_key, p_callback&& writer) {
        static_assert(
            bioseq::indexing::index_chunk_writer_fn<p_callback, p_value_type>,
            "write_to_each writer has an incompatible element callback shape");
        auto it = chunk_payload_bytes_.find(sub_key);
        if (it == chunk_payload_bytes_.end()) {
            index_error("fragment write_to_each has no preceding resize");
        }
        const std::vector<std::size_t>& plan = it->second;
        const std::size_t n = plan.size();
        std::size_t max_payload = 0;
        for (const std::size_t payload_bytes : plan) {
            if (payload_bytes > k_max_index_chunk_size_bytes) {
                index_error("fragment write plan exceeds MAX chunk size");
            }
            if (payload_bytes % sizeof(p_value_type) != 0) {
                index_error("fragment write payload is not a whole number of elements");
            }
            max_payload = std::max(max_payload, payload_bytes);
        }
        const std::size_t scratch_units = max_payload == 0
            ? 1
            : 1 + (max_payload - 1) / sizeof(std::int64_t);
        std::int64_t* buffer = scratch_buffer_(scratch_units);

        fragment_write_resources resources{RowExclusiveLock};
        try {
            resources.relation = open_index_relation_(
                true, RowExclusiveLock);
            if (resources.relation == nullptr) {
                index_error("fragment write relation could not be opened");
            }
            for (std::size_t chunk_no = 0;
                 chunk_no < n;
                 /* writer may skip */) {
                const std::size_t payload_bytes = plan[chunk_no];
                if (scratch_buffer_bytes_() < payload_bytes) {
                    index_error("fragment write scratch is undersized");
                }
                const std::size_t capacity = payload_bytes / sizeof(p_value_type);
                if (payload_bytes > 0) {
                    std::memset(buffer, 0, payload_bytes);
                }
                const std::size_t next = writer(
                    chunk_no, reinterpret_cast<p_value_type*>(buffer), capacity);
                if (next < chunk_no) {
                    index_error("fragment writer moved backwards");
                }
                const auto payload = std::span<const char>(
                    reinterpret_cast<const char*>(buffer), payload_bytes);
                upsert_chunk_row_(
                    resources, query_key_, sub_key, chunk_no,
                    payload_bytes, payload);
                admit_cached_chunk_(
                    sub_key, chunk_no, payload, true);
                chunk_no = next == chunk_no ? chunk_no + 1 : next;
            }
            resources.close_checked();
            chunk_payload_bytes_.erase(it);
        } catch (const bioseq::postgres::support::postgres_error&) {
            resources.abandon_after_pg_error_noexcept();
            throw;
        } catch (...) {
            resources.close_then_rethrow_cpp(std::current_exception());
        }
    }

    template <class p_callback> void read_chunks_in_range(index_sub_key_t lo, index_sub_key_t hi, p_callback&& cb) {
        if (hi <= lo) return;
        fragment_read_resources resources{AccessShareLock};
        try {
            resources.relation = open_index_relation_(
                false, AccessShareLock);
            if (resources.relation == nullptr) return;
            const Oid primary_index =
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [relation = resources.relation] {
                        return bioseq::postgres::support::find_primary_key_oid(relation);
                    });
            if (!OidIsValid(primary_index)) {
                resources.close_checked();
                index_error("range scan has no primary index");
            }
            resources.ordered_index_relation =
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [primary_index] {
                        return index_open(primary_index, AccessShareLock);
                    });
            std::array<ScanKeyData, 3> keys{};
            ScanKeyData* key_data = keys.data();
            const Datum query_key = bioseq::postgres::support::pg_datum::from(
                static_cast<std::int64_t>(query_key_)).raw();
            const Datum first_sub_key = bioseq::postgres::support::pg_datum::from(
                static_cast<std::int64_t>(lo)).raw();
            const Datum last_sub_key = bioseq::postgres::support::pg_datum::from(
                static_cast<std::int64_t>(hi - 1)).raw();
            bioseq::postgres::support::invoke_pg_as_cpp(
                [key_data, query_key, first_sub_key, last_sub_key] {
                    ScanKeyInit(
                        &key_data[0], k_attribute_query_key, BTEqualStrategyNumber,
                        F_INT8EQ, query_key);
                    ScanKeyInit(
                        &key_data[1], k_attribute_sub_key,
                        BTGreaterEqualStrategyNumber, F_INT8GE,
                        first_sub_key);
                    ScanKeyInit(
                        &key_data[2], k_attribute_sub_key,
                        BTLessEqualStrategyNumber, F_INT8LE,
                        last_sub_key);
                });
            resources.scan = bioseq::postgres::support::invoke_pg_as_cpp(
                [relation = resources.relation,
                 index_relation = resources.ordered_index_relation,
                 key_data] {
                    return systable_beginscan_ordered(
                        relation, index_relation, nullptr, 3, key_data);
                });
            resources.ordered_scan = true;

            std::vector<char> bytes;
            while (true) {
                HeapTuple tuple = bioseq::postgres::support::invoke_pg_as_cpp(
                    [scan = resources.scan] {
                        return systable_getnext_ordered(
                            scan, ForwardScanDirection);
                    });
                if (!HeapTupleIsValid(tuple)) break;

                bool is_null = false;
                const Datum sub_key_datum =
                    bioseq::postgres::support::invoke_pg_as_cpp(
                        [relation = resources.relation, tuple, &is_null] {
                            return heap_getattr(
                                tuple, k_attribute_sub_key,
                                RelationGetDescr(relation), &is_null);
                        });
                if (is_null) index_error("range row sub-key is NULL");
                const std::int64_t signed_sub_key =
                    bioseq::postgres::support::pg_datum::to<std::int64_t>(
                        bioseq::postgres::support::pg_datum::from_raw(
                            sub_key_datum));

                is_null = false;
                const Datum chunk_no_datum =
                    bioseq::postgres::support::invoke_pg_as_cpp(
                        [relation = resources.relation, tuple, &is_null] {
                            return heap_getattr(
                                tuple, k_attribute_chunk_number,
                                RelationGetDescr(relation), &is_null);
                        });
                if (is_null) index_error("range row chunk number is NULL");
                const std::int32_t signed_chunk_no =
                    bioseq::postgres::support::pg_datum::to<std::int32_t>(
                        bioseq::postgres::support::pg_datum::from_raw(
                            chunk_no_datum));
                if (signed_chunk_no < 0) {
                    index_error("range row chunk number is negative");
                }

                is_null = false;
                const Datum blob = bioseq::postgres::support::invoke_pg_as_cpp(
                    [relation = resources.relation, tuple, &is_null] {
                        return heap_getattr(
                            tuple, k_attribute_blob,
                            RelationGetDescr(relation), &is_null);
                    });
                if (is_null) index_error("range row blob is NULL");
                auto detoasted = bioseq::postgres::support::invoke_pg_as_cpp(
                    [blob] {
                        return bioseq::postgres::support::detoast_packed_as<bytea>(
                            bioseq::postgres::support::pg_datum::from_raw(blob));
                    });
                resources.detoasted = detoasted.ptr;
                resources.detoasted_is_copy = detoasted.is_copy;
                detoasted.ptr = nullptr;
                detoasted.is_copy = false;

                const std::size_t payload =
                    VARSIZE_ANY_EXHDR(resources.detoasted);
                bytes.resize(payload);
                if (payload > 0) {
                    std::memcpy(
                        bytes.data(), VARDATA_ANY(resources.detoasted),
                        payload);
                }
                const index_sub_key_t sub_key =
                    static_cast<index_sub_key_t>(signed_sub_key);
                const std::size_t chunk_no =
                    static_cast<std::size_t>(signed_chunk_no);
                const auto cached = store_cached_chunk(
                    chunk_cache_key_(sub_key, chunk_no),
                    std::span<const char>(bytes.data(), bytes.size()),
                    false);
                resources.release_detoasted_checked();

                if (cached) {
                    cb(sub_key, cached->data, cached->size);
                } else {
                    cb(sub_key, bytes.data(), payload);
                }
            }
            resources.close_checked();
        } catch (const bioseq::postgres::support::postgres_error&) {
            resources.abandon_after_pg_error_noexcept();
            throw;
        } catch (...) {
            resources.close_then_rethrow_cpp(std::current_exception());
        }
    }

    template <class p_callback> void for_each_sub_key(p_callback&& fn) const {
        fragment_read_resources resources{AccessShareLock};
        try {
            resources.relation = open_index_relation_(
                false, AccessShareLock);
            if (resources.relation == nullptr) return;
            const Oid primary_index =
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [relation = resources.relation] {
                        return bioseq::postgres::support::find_primary_key_oid(relation);
                    });
            std::array<ScanKeyData, 2> keys{};
            ScanKeyData* key_data = keys.data();
            const Datum query_key = bioseq::postgres::support::pg_datum::from(
                static_cast<std::int64_t>(query_key_)).raw();
            const Datum data_sub_key_limit =
                bioseq::postgres::support::pg_datum::from(
                    static_cast<std::int64_t>(k_temporary_run_sub_key_base)).raw();
            bioseq::postgres::support::invoke_pg_as_cpp(
                [key_data, query_key, data_sub_key_limit] {
                    ScanKeyInit(
                        &key_data[0], k_attribute_query_key, BTEqualStrategyNumber,
                        F_INT8EQ, query_key);
                    ScanKeyInit(
                        &key_data[1], k_attribute_sub_key, BTLessStrategyNumber,
                        F_INT8LT, data_sub_key_limit);
                });
            resources.scan = bioseq::postgres::support::invoke_pg_as_cpp(
                [relation = resources.relation, primary_index, key_data] {
                    return systable_beginscan(
                        relation, primary_index,
                        OidIsValid(primary_index), nullptr, 2, key_data);
                });
            std::int64_t last =
                (std::numeric_limits<std::int64_t>::min)();
            bool have_last = false;
            while (true) {
                HeapTuple tuple = bioseq::postgres::support::invoke_pg_as_cpp(
                    [scan = resources.scan] {
                        return systable_getnext(scan);
                    });
                if (!HeapTupleIsValid(tuple)) break;
                bool is_null = false;
                const Datum sub_key_datum =
                    bioseq::postgres::support::invoke_pg_as_cpp(
                        [relation = resources.relation, tuple, &is_null] {
                            return heap_getattr(
                                tuple, k_attribute_sub_key,
                                RelationGetDescr(relation), &is_null);
                        });
                if (is_null) index_error("fragment sub-key is NULL");
                const std::int64_t sub_key =
                    bioseq::postgres::support::pg_datum::to<std::int64_t>(
                        bioseq::postgres::support::pg_datum::from_raw(
                            sub_key_datum));
                if (!have_last || sub_key != last) {
                    fn(static_cast<index_sub_key_t>(sub_key));
                    last = sub_key;
                    have_last = true;
                }
            }
            resources.close_checked();
        } catch (const bioseq::postgres::support::postgres_error&) {
            resources.abandon_after_pg_error_noexcept();
            throw;
        } catch (...) {
            resources.close_then_rethrow_cpp(std::current_exception());
        }
    }

    // ---- lifecycle ----
    void delete_where_(
            std::optional<std::pair<index_sub_key_t, index_sub_key_t>>
                inclusive_sub_key_range) {
        if (inclusive_sub_key_range
            && inclusive_sub_key_range->second
                < inclusive_sub_key_range->first) {
            return;
        }
        if (!bioseq::indexing::sql_bigint_can_represent(query_key_)) {
            index_error("fragment delete query key exceeds PostgreSQL BIGINT");
        }
        // Opening through the five-column validator is intentional: a generation
        // generation table has query_key/sub_key columns too, so name-based
        // DELETE SQL could otherwise erase immutable generations while
        // bypassing every direct-row attribute check.
        fragment_write_resources resources{RowExclusiveLock};
        try {
            resources.relation = open_index_relation_(
                false, RowExclusiveLock);
            if (resources.relation == nullptr) return;
            Relation relation = resources.relation;
            const Oid primary_index = bioseq::postgres::support::invoke_pg_as_cpp(
                [relation] { return bioseq::postgres::support::find_primary_key_oid(relation); });
            if (!OidIsValid(primary_index)) {
                resources.close_checked();
                index_error("fragment delete requires a primary index");
            }

            std::array<ScanKeyData, 3> keys{};
            ScanKeyData* key_data = keys.data();
            const Datum query_key = bioseq::postgres::support::pg_datum::from(
                static_cast<std::int64_t>(query_key_)).raw();
            const bool bounded = inclusive_sub_key_range.has_value();
            const Datum first_sub_key = bounded
                ? bioseq::postgres::support::pg_datum::from(
                    static_cast<std::int64_t>(
                        inclusive_sub_key_range->first)).raw()
                : Datum{0};
            const Datum last_sub_key = bounded
                ? bioseq::postgres::support::pg_datum::from(
                    static_cast<std::int64_t>(
                        inclusive_sub_key_range->second)).raw()
                : Datum{0};
            bioseq::postgres::support::invoke_pg_as_cpp(
                [key_data, query_key, bounded,
                 first_sub_key, last_sub_key] {
                    ScanKeyInit(
                        &key_data[0], k_attribute_query_key,
                        BTEqualStrategyNumber, F_INT8EQ, query_key);
                    if (bounded) {
                        ScanKeyInit(
                            &key_data[1], k_attribute_sub_key,
                            BTGreaterEqualStrategyNumber, F_INT8GE,
                            first_sub_key);
                        ScanKeyInit(
                            &key_data[2], k_attribute_sub_key,
                            BTLessEqualStrategyNumber, F_INT8LE,
                            last_sub_key);
                    }
                });
            const int key_count = bounded ? 3 : 1;
            resources.scan = bioseq::postgres::support::invoke_pg_as_cpp(
                [relation, primary_index, key_count, key_data] {
                    return systable_beginscan(
                        relation, primary_index, true, nullptr,
                        key_count, key_data);
                });
            while (true) {
                HeapTuple tuple = bioseq::postgres::support::invoke_pg_as_cpp(
                    [scan = resources.scan] {
                        return systable_getnext(scan);
                    });
                if (!HeapTupleIsValid(tuple)) break;
                ItemPointerData tuple_id = tuple->t_self;
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [relation, tuple_id]() mutable {
                        simple_heap_delete(relation, &tuple_id);
                    });
            }
            resources.close_checked();
        } catch (const bioseq::postgres::support::postgres_error&) {
            resources.abandon_after_pg_error_noexcept();
            throw;
        } catch (...) {
            resources.close_then_rethrow_cpp(std::current_exception());
        }
        bioseq::postgres::support::invoke_pg_as_cpp(
            [] { CommandCounterIncrement(); });
        evict_generation_storage_residency_();
    }
    void clear_entity() {
        delete_where_(std::nullopt);
        decoded_sub_ = k_directory_sub_key;
        decoded_chunk_ = (std::numeric_limits<std::size_t>::max)();
        decoded_bytes_ = 0;
    }
    void discard_sub_keys(index_sub_key_t lo, index_sub_key_t hi) {
        delete_where_(std::pair{lo, hi});
    }
    void clear(std::string_view name); // index-wide drop across all shards (defined below, uses seq_sub_table enumeration)

    [[nodiscard]] std::size_t run_buffer_bytes() const {
        if (run_buffer_override_bytes_ > 0) return run_buffer_override_bytes_;
        constexpr std::uint64_t k_floor = std::uint64_t{64} << 20;
        constexpr std::uint64_t k_ceiling = std::uint64_t{8} << 30;
        const std::uint64_t desired = std::uint64_t{2} << 30;
        std::uint64_t capped =
            bioseq::memory::default_budget().clamp_working_set(desired, k_floor, k_ceiling);
        const std::uint64_t index_cap =
            bioseq::parallelism::memory_admission_controller::instance().index_memory_cap_bytes();
        if (index_cap != 0) {
            capped = std::min(capped, index_cap);
        }
        return static_cast<std::size_t>(capped);
    }

    // ---- external sort spill (BufFile-backed sorted runs) ----
    [[nodiscard]] sorted_pair_run_store& sorted_runs_ensure_() {
        if (!sorted_runs_.has_value()) {
            sorted_runs_.emplace();
        }
        return *sorted_runs_;
    }
    std::size_t spill_sorted_run(std::span<const std::pair<std::uint64_t,std::uint64_t>> sorted);
    std::size_t spill_sorted_run_owned(std::vector<std::pair<std::uint64_t,std::uint64_t>> sorted);
	    std::size_t spill_sorted_run_owned_admitted(
	        std::vector<std::pair<std::uint64_t,std::uint64_t>> sorted,
	        std::uint64_t admitted_bytes,
	        bioseq::parallelism::memory_permit permit);
	    void demote_resident_runs_until_available(std::uint64_t bytes_needed);
	    template <class p_callback> void merge_runs(p_callback&& on_kmer);
	    std::size_t spill_sorted_segment_run(
	        std::uint32_t position_segment,
	        std::span<const std::pair<std::uint64_t,std::uint64_t>> sorted);
	    bool reroute_segment_runs(
	        std::uint64_t old_span,
	        std::uint64_t new_span,
	        std::uint64_t target_length);
	    template <class p_callback> void merge_segment_runs(p_callback&& on_segment_kmer);
	    [[nodiscard]] direct_minimizer_spill_storage_binding
	    begin_segment_spill_generation();
	    [[nodiscard]] direct_minimizer_spill_storage_binding
	    segment_spill_storage_binding() const noexcept {
	        return segment_spill_binding_;
	    }
	    [[nodiscard]] sorted_run_store_stats sorted_run_stats() const noexcept;
    void snapshot_sorted_run_stats_before_release();
    void release_runs_noexcept() noexcept;
    void release_runs();
    void close_checked();
    void abandon_after_pg_error_noexcept() noexcept;
};

// A bounded position cursor over one kmer's unique positions (one vector).
struct position_batch_cursor {
    const std::vector<std::uint64_t>* v = nullptr;
    [[nodiscard]] std::uint64_t total_count() const { return v->size(); }
    template <class p_callback> void for_each_batch(p_callback&& f) const {
        if (!v->empty()) f(std::span<const std::uint64_t>(v->data(), v->size()));
    }
};

// =========================================================================
//  sequence_index_storage  —  INLINE_PER_SEQUENCE
// =========================================================================
struct sequence_index_storage {
    Oid           db_oid_ = InvalidOid, tablespace_oid_ = InvalidOid, main_relfilenode_ = InvalidOid, user_relid_ = InvalidOid;
    seq_caches*   read_caches_ = nullptr;
    std::int16_t  column_attno_ = 0;
    std::string_view index_name_{};
    std::uint8_t  bucket_idx_ = 0;
    std::uint32_t shard_idx_ = 0;
    std::uint64_t query_key_ = 0;
    MemoryContext own_mctx_ = nullptr;
    mutable Oid   index_relid_ = InvalidOid;
    mutable std::int64_t* decoded_ = nullptr;
    mutable std::size_t   decoded_units_ = 0;
    mutable std::size_t   decoded_bytes_ = 0;
    mutable bool          decoded_valid_ = false;
    std::size_t           planned_bytes_ = std::numeric_limits<std::size_t>::max();

    sequence_index_storage() = default;
    sequence_index_storage(const sequence_index_storage&) = delete;
    sequence_index_storage& operator=(const sequence_index_storage&) = delete;
    sequence_index_storage(sequence_index_storage&& o) noexcept { steal_(std::move(o)); }
    sequence_index_storage& operator=(sequence_index_storage&&) = delete;
    ~sequence_index_storage() { release_(); }
    void steal_(sequence_index_storage&& o) noexcept {
        db_oid_=o.db_oid_; tablespace_oid_=o.tablespace_oid_; main_relfilenode_=o.main_relfilenode_; user_relid_=o.user_relid_;
        read_caches_=o.read_caches_; column_attno_=o.column_attno_; index_name_=o.index_name_;
        bucket_idx_=o.bucket_idx_; shard_idx_=o.shard_idx_; query_key_=o.query_key_;
        own_mctx_=o.own_mctx_; index_relid_=o.index_relid_; decoded_=o.decoded_; decoded_units_=o.decoded_units_;
        decoded_bytes_=o.decoded_bytes_; decoded_valid_=o.decoded_valid_; planned_bytes_=o.planned_bytes_;
        o.own_mctx_=nullptr; o.decoded_=nullptr; o.decoded_units_=0; o.index_relid_=InvalidOid; o.decoded_valid_=false;
    }
    void release_() noexcept {
        DEBUG_ASSERT(own_mctx_ == nullptr,
            "sequence_index_storage must be close_checked or abandoned before destruction");
        own_mctx_ = nullptr;
        decoded_ = nullptr;
    }
    void close_checked() {
        if (own_mctx_ != nullptr) {
            MemoryContext finished_context = std::exchange(
                own_mctx_, nullptr);
            decoded_ = nullptr;
            bioseq::postgres::support::invoke_pg_as_cpp(
                [finished_context] {
                    MemoryContextDelete(finished_context);
                });
        }
    }
    void abandon_after_pg_error_noexcept() noexcept {
        own_mctx_ = nullptr;
        decoded_ = nullptr;
        decoded_units_ = 0;
        decoded_bytes_ = 0;
        decoded_valid_ = false;
        index_relid_ = InvalidOid;
    }

	[[noreturn]] void index_error(std::string_view msg) const {
		bioseq::postgres::support::invoke_pg_as_cpp([msg] {
			ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("bioseq index: %.*s",
					static_cast<int>(msg.size()), msg.data())));
		});
		pg_unreachable();
	}

    [[nodiscard]] std::string this_table_name() const {
        return format_seqidx_name(index_name_, db_oid_, tablespace_oid_, main_relfilenode_, column_attno_, bucket_idx_, shard_idx_);
    }
    [[nodiscard]] Oid resolve_relid_(bool create_if_missing) const {
        if (OidIsValid(index_relid_)) return index_relid_;
        const std::string nm = this_table_name(); const Oid ns = bioseq_internal_ns();
        Oid rid = get_relname_relid(nm.c_str(), ns);
        if (!OidIsValid(rid) && create_if_missing) {
            const char* pk = persistence_kw_for(user_relid_);
            std::string ddl = fmt::format(
                "CREATE {} TABLE IF NOT EXISTS bioseq_internal.{} ("
                "query_key BIGINT NOT NULL, chunk_bytes INT NOT NULL, blob BYTEA NOT NULL, "
                "PRIMARY KEY (query_key)) USING heap;", pk, quote_identifier(nm.c_str()));
            run_internal_ddl(ddl.c_str());
            run_internal_ddl(fmt::format(
                "ALTER TABLE bioseq_internal.{} ALTER COLUMN blob SET STORAGE EXTERNAL;",
                quote_identifier(nm.c_str())).c_str());
            rid = get_relname_relid(nm.c_str(), ns);
            if (!OidIsValid(rid)) index_error("failed to create inline seqidx table");
        }
        index_relid_ = rid; return rid;
    }
    [[nodiscard]] Relation open_(bool create, LOCKMODE lock) const {
        const Oid rid = resolve_relid_(create);
        if (!OidIsValid(rid)) return nullptr;
        return bioseq::postgres::support::try_open_postgres_relation(rid, lock);
    }
	[[nodiscard]] Oid resolve_relid_for_read_checked_() const {
		if (OidIsValid(index_relid_)) {
			return index_relid_;
		}
		// Build/destruct the C++ name outside every PG_TRY leaf. A PostgreSQL
		// longjmp from namespace/relation lookup is converted before this
		// ordinary C++ frame unwinds.
		const std::string table_name = this_table_name();
		const Oid namespace_oid = bioseq::postgres::support::invoke_pg_as_cpp(
			[] { return bioseq_internal_ns(); });
		const Oid relation_oid = bioseq::postgres::support::invoke_pg_as_cpp(
			[&table_name, namespace_oid] {
				inject_inline_sequence_pg_leaf_failure(
					inline_sequence_pg_leaf_failure::relation_lookup);
				return get_relname_relid(table_name.c_str(), namespace_oid);
			});
		index_relid_ = relation_oid;
		return relation_oid;
	}
	[[nodiscard]] Relation open_for_read_checked_() const {
		const Oid relation_oid = resolve_relid_for_read_checked_();
		if (!OidIsValid(relation_oid)) {
			return nullptr;
		}
		return bioseq::postgres::support::invoke_pg_as_cpp(
			[relation_oid] {
				return bioseq::postgres::support::try_open_postgres_relation(
					relation_oid, AccessShareLock);
			});
	}
	void ensure_buffer_checked_(const std::size_t units) const {
		if (decoded_units_ >= units && decoded_ != nullptr) {
			return;
		}
		MemoryContext owner = own_mctx_;
		std::int64_t* old_buffer = decoded_;
		std::int64_t* replacement = bioseq::postgres::support::invoke_pg_as_cpp(
			[owner, old_buffer, units] {
				MemoryContext previous = MemoryContextSwitchTo(owner);
				auto* result = old_buffer == nullptr
					? static_cast<std::int64_t*>(
						palloc(units * sizeof(std::int64_t)))
					: static_cast<std::int64_t*>(
						repalloc(old_buffer,
							units * sizeof(std::int64_t)));
				MemoryContextSwitchTo(previous);
				return result;
			});
		decoded_ = replacement;
		decoded_units_ = units;
	}
    void load_() const {
		Relation rel = open_for_read_checked_();
        if (rel == nullptr) { decoded_bytes_=0; decoded_valid_=true; return; }
		const Oid pk = bioseq::postgres::support::invoke_pg_as_cpp(
			[rel] { return bioseq::postgres::support::find_primary_key_oid(rel); });
		ScanKeyData key{};
		const Datum query_key = bioseq::postgres::support::pg_datum::from(
			static_cast<std::int64_t>(query_key_)).raw();
		bioseq::postgres::support::invoke_pg_as_cpp([&key, query_key] {
			ScanKeyInit(&key, 1, BTEqualStrategyNumber, F_INT8EQ,
				query_key);
		});
		SysScanDesc scan = bioseq::postgres::support::invoke_pg_as_cpp(
			[rel, pk, &key] {
				return systable_beginscan(
					rel, pk, OidIsValid(pk), nullptr, 1, &key);
			});
		HeapTuple tuple = bioseq::postgres::support::invoke_pg_as_cpp(
			[scan] {
				inject_inline_sequence_pg_leaf_failure(
					inline_sequence_pg_leaf_failure::tuple_read);
				return systable_getnext(scan);
			});
		if (HeapTupleIsValid(tuple)) {
			bool is_null = false;
			TupleDesc tuple_desc = RelationGetDescr(rel);
			Datum datum = bioseq::postgres::support::invoke_pg_as_cpp(
				[tuple, tuple_desc, &is_null] {
					return heap_getattr(tuple, 3, tuple_desc, &is_null);
				});
			if (is_null) {
				index_error("inline sequence index blob is NULL");
			}
			auto raw_detoasted = bioseq::postgres::support::invoke_pg_as_cpp(
				[datum] {
					inject_inline_sequence_pg_leaf_failure(
						inline_sequence_pg_leaf_failure::detoast);
					return bioseq::postgres::support::detoast_packed_as<bytea>(
						bioseq::postgres::support::pg_datum::from_raw(datum));
				});
            bytea* raw = raw_detoasted.ptr;
            const std::size_t payload = VARSIZE_ANY_EXHDR(raw);
            const std::size_t units = (payload + sizeof(std::int64_t) - 1)/sizeof(std::int64_t);
			ensure_buffer_checked_(units == 0 ? 1 : units);
            if (payload>0) std::memcpy(decoded_, VARDATA_ANY(raw), payload);
			bioseq::postgres::support::invoke_pg_as_cpp(
				[&raw_detoasted] { raw_detoasted.free_if_copy(); });
            decoded_bytes_ = payload;
        } else decoded_bytes_ = 0;
		bioseq::postgres::support::invoke_pg_as_cpp(
			[scan] { systable_endscan(scan); });
		bioseq::postgres::support::invoke_pg_as_cpp(
			[rel] {
				inject_inline_sequence_pg_leaf_failure(
					inline_sequence_pg_leaf_failure::close);
				relation_close(rel, AccessShareLock);
			});
		decoded_valid_ = true;
    }
    [[nodiscard]] std::size_t number_of_chunks() const { return chunk_byte_size() == 0 ? 0 : 1; }
    [[nodiscard]] std::size_t chunk_byte_size() const { if (!decoded_valid_) load_(); return decoded_bytes_; }
    template <class p_value_type> [[nodiscard]] const p_value_type* read_chunk() { if (!decoded_valid_) load_(); return reinterpret_cast<const p_value_type*>(decoded_); }
    void resize(std::size_t bytes) { if (bytes > k_max_index_chunk_size_bytes) bioseq_log<logging_level::error, logging_target::postgres>( "bioseq index inline: bytes exceed single-chunk MAX"); planned_bytes_ = bytes; }
    void prefetch_chunk() {}
    template <class p_value_type, class p_callback> void write_to_each(p_callback&& writer) {
        static_assert(bioseq::indexing::index_chunk_writer_fn<p_callback, p_value_type>, "inline write_to_each writer shape");
        if (planned_bytes_ == std::numeric_limits<std::size_t>::max()) bioseq_log<logging_level::error, logging_target::postgres>( "bioseq index inline: write_to_each without resize");
        const std::size_t payload = planned_bytes_;
        if (payload % sizeof(p_value_type) != 0) bioseq_log<logging_level::error, logging_target::postgres>( "bioseq index inline: payload is not a whole number of elements");
        std::vector<char> buf(payload, 0);
        const std::size_t next = writer(0, reinterpret_cast<p_value_type*>(buf.data()), payload / sizeof(p_value_type));
        if (next != 0) bioseq_log<logging_level::error, logging_target::postgres>( "bioseq index inline: writer emitted >1 chunk");
        Relation rel = open_(true, RowExclusiveLock);
        // upsert (delete then insert) so rebuild replaces the single row.
        { const Oid pk = bioseq::postgres::support::find_primary_key_oid(rel);
          ScanKeyData k[1]; ScanKeyInit(&k[0],1,BTEqualStrategyNumber,F_INT8EQ,
              bioseq::postgres::support::pg_datum::from(static_cast<std::int64_t>(query_key_)).raw());
          SysScanDesc s = systable_beginscan(rel, pk, OidIsValid(pk), nullptr, 1, k);
          HeapTuple old_t = systable_getnext(s);
          if (HeapTupleIsValid(old_t)) simple_heap_delete(rel, &old_t->t_self);
          systable_endscan(s); }
        bytea* blob = reinterpret_cast<bytea*>(palloc(VARHDRSZ + payload));
        SET_VARSIZE(blob, VARHDRSZ + payload); if (payload>0) std::memcpy(VARDATA(blob), buf.data(), payload);
        bioseq::postgres::support::pg_datum_list<3> values{};
        values.set(1, static_cast<std::int64_t>(query_key_));
        values.set(2, static_cast<std::int32_t>(payload));
        values.set_raw(3, bioseq::postgres::support::pg_datum::from_pointer(blob));
        HeapTuple tup = values.form_tuple(RelationGetDescr(rel));
        CatalogTupleInsert(rel, tup); heap_freetuple(tup); pfree(blob);
        relation_close(rel, RowExclusiveLock);
        decoded_valid_ = false; planned_bytes_ = std::numeric_limits<std::size_t>::max();
    }
    void clear_entity() {
        const Oid rid = resolve_relid_(false); if (!OidIsValid(rid)) return;
        std::string sql = fmt::format("DELETE FROM bioseq_internal.{} WHERE query_key = {}", quote_identifier(this_table_name().c_str()), (long long)query_key_);
        if (SPI_connect()==SPI_OK_CONNECT){ SPI_execute(sql.c_str(), false, 0); SPI_finish(); }
        decoded_valid_ = false;
    }
    void clear(std::string_view name);
    /// PostgreSQL-only reuse hook: reposition without reallocating own_mctx_.
    void reset_entity(const sequence_locator& loc, std::uint64_t sequence_rowno) {
        bucket_idx_ = (std::uint8_t)loc.bucket_index; shard_idx_ = loc.shard_index; query_key_ = sequence_rowno;
        index_relid_ = InvalidOid; decoded_valid_ = false; decoded_bytes_ = 0;
    }
};

// =========================================================================
//  table_index_storage  —  CHUNKED_PER_TABLE
// =========================================================================
struct table_index_storage {
    Oid db_oid_ = InvalidOid, tablespace_oid_ = InvalidOid, main_relfilenode_ = InvalidOid, user_relid_ = InvalidOid;
    std::int16_t column_attno_ = 0;
    std::string_view index_name_{};
    seq_caches* read_caches_ = nullptr;
    MemoryContext own_mctx_ = nullptr;
    mutable Oid index_relid_ = InvalidOid;
    mutable std::int64_t* decoded_ = nullptr; mutable std::size_t decoded_units_=0, decoded_bytes_=0;
    mutable std::int64_t decoded_chunk_ = -1;
    std::size_t planned_bytes_for_chunk_ = 0;

    table_index_storage() = default;
    table_index_storage(const table_index_storage&) = delete;
    table_index_storage& operator=(const table_index_storage&) = delete;
    table_index_storage(table_index_storage&& o) noexcept { steal_(std::move(o)); }
    table_index_storage& operator=(table_index_storage&&) = delete;
    ~table_index_storage() { release_(); }
    void steal_(table_index_storage&& o) noexcept {
        db_oid_=o.db_oid_; tablespace_oid_=o.tablespace_oid_; main_relfilenode_=o.main_relfilenode_; user_relid_=o.user_relid_;
        column_attno_=o.column_attno_; index_name_=o.index_name_; read_caches_=o.read_caches_; own_mctx_=o.own_mctx_;
        index_relid_=o.index_relid_; decoded_=o.decoded_; decoded_units_=o.decoded_units_; decoded_bytes_=o.decoded_bytes_;
        decoded_chunk_=o.decoded_chunk_; planned_bytes_for_chunk_=o.planned_bytes_for_chunk_;
        o.own_mctx_=nullptr; o.decoded_=nullptr; o.index_relid_=InvalidOid; o.decoded_units_=0;
    }
    void release_() noexcept {
        DEBUG_ASSERT(own_mctx_ == nullptr,
            "table_index_storage must be close_checked or abandoned before destruction");
        own_mctx_ = nullptr;
        decoded_ = nullptr;
    }
    void close_checked() {
        if (own_mctx_ != nullptr) {
            MemoryContext finished_context = std::exchange(
                own_mctx_, nullptr);
            decoded_ = nullptr;
            bioseq::postgres::support::invoke_pg_as_cpp(
                [finished_context] {
                    MemoryContextDelete(finished_context);
                });
        }
    }
    void abandon_after_pg_error_noexcept() noexcept {
        own_mctx_ = nullptr;
        decoded_ = nullptr;
        decoded_units_ = 0;
        decoded_bytes_ = 0;
        decoded_chunk_ = -1;
        index_relid_ = InvalidOid;
    }
    [[noreturn]] void index_error(std::string_view msg) const { bioseq_log<logging_level::error, logging_target::postgres>( "bioseq index: {}", msg); abort(); }

    [[nodiscard]] std::string this_table_name(std::int32_t shard=0) const {
        return format_seqidxtab_name(index_name_, db_oid_, tablespace_oid_, main_relfilenode_, column_attno_, shard);
    }
    [[nodiscard]] Oid resolve_relid_(bool create) const {
        if (OidIsValid(index_relid_)) return index_relid_;
        const std::string nm = this_table_name(0); const Oid ns = bioseq_internal_ns();
        Oid rid = get_relname_relid(nm.c_str(), ns);
        if (!OidIsValid(rid) && create) {
            const char* pk = persistence_kw_for(user_relid_);
            std::string ddl = fmt::format(
                "CREATE {} TABLE IF NOT EXISTS bioseq_internal.{} ("
                "chunk_no BIGINT NOT NULL, chunk_bytes INT NOT NULL, blob BYTEA NOT NULL, "
                "PRIMARY KEY (chunk_no)) USING heap;", pk, quote_identifier(nm.c_str()));
            run_internal_ddl(ddl.c_str());
            run_internal_ddl(fmt::format(
                "ALTER TABLE bioseq_internal.{} ALTER COLUMN blob SET STORAGE EXTERNAL;",
                quote_identifier(nm.c_str())).c_str());
            rid = get_relname_relid(nm.c_str(), ns);
        }
        index_relid_ = rid; return rid;
    }
    [[nodiscard]] Relation open_(bool create, LOCKMODE lock) const { const Oid rid = resolve_relid_(create); if (!OidIsValid(rid)) return nullptr; return bioseq::postgres::support::try_open_postgres_relation(rid, lock); }
    void ensure_buffer_(std::size_t units) const {
        if (decoded_units_>=units && decoded_!=nullptr) return;
        MemoryContext previous_context = MemoryContextSwitchTo(own_mctx_);
        decoded_ = (decoded_==nullptr)?(std::int64_t*)palloc(units*sizeof(std::int64_t)):(std::int64_t*)repalloc(decoded_, units*sizeof(std::int64_t));
        decoded_units_=units; MemoryContextSwitchTo(previous_context);
    }
    [[nodiscard]] std::size_t number_of_chunks() const {
        Relation rel = open_(false, AccessShareLock); if (rel==nullptr) return 0;
        SysScanDesc s = systable_beginscan(rel, InvalidOid, false, nullptr, 0, nullptr);
        std::size_t n=0; for (; systable_getnext(s)!=nullptr;) ++n; systable_endscan(s); relation_close(rel, AccessShareLock); return n;
    }
    [[nodiscard]] std::size_t chunk_byte_size(std::size_t chunk_no) const {
        Relation rel = open_(false, AccessShareLock); if (rel==nullptr) return 0;
        const Oid pk = bioseq::postgres::support::find_primary_key_oid(rel);
        ScanKeyData k[1]; ScanKeyInit(&k[0],1,BTEqualStrategyNumber,F_INT8EQ,
            bioseq::postgres::support::pg_datum::from(static_cast<std::int64_t>(chunk_no)).raw());
        SysScanDesc s = systable_beginscan(rel, pk, OidIsValid(pk), nullptr, 1, k);
        std::size_t out=0; HeapTuple t = systable_getnext(s);
        if (HeapTupleIsValid(t)){ bool isnull=false; out=static_cast<std::size_t>(bioseq::postgres::support::pg_datum::to<std::int32_t>(
            bioseq::postgres::support::pg_datum::from_raw(heap_getattr(t,2,RelationGetDescr(rel),&isnull)))); }
        systable_endscan(s); relation_close(rel, AccessShareLock); return out;
    }
    template <class p_value_type> [[nodiscard]] const p_value_type* read_chunk(std::size_t chunk_no) {
        if (decoded_!=nullptr && decoded_chunk_==(std::int64_t)chunk_no) return reinterpret_cast<const p_value_type*>(decoded_);
        Relation rel = open_(false, AccessShareLock);
        if (rel==nullptr) index_error("table index relation missing");
        const Oid pk = bioseq::postgres::support::find_primary_key_oid(rel);
        ScanKeyData k[1]; ScanKeyInit(&k[0],1,BTEqualStrategyNumber,F_INT8EQ,
            bioseq::postgres::support::pg_datum::from(static_cast<std::int64_t>(chunk_no)).raw());
        SysScanDesc s = systable_beginscan(rel, pk, OidIsValid(pk), nullptr, 1, k);
        HeapTuple t = systable_getnext(s);
        if (!HeapTupleIsValid(t)){ systable_endscan(s); relation_close(rel,AccessShareLock); index_error("table chunk missing"); }
        bool isnull=false; Datum d = heap_getattr(t,3,RelationGetDescr(rel),&isnull);
        auto raw_detoasted = bioseq::postgres::support::detoast_packed_as<bytea>(
            bioseq::postgres::support::pg_datum::from_raw(d));
        bytea* raw = raw_detoasted.ptr;
        const std::size_t payload = VARSIZE_ANY_EXHDR(raw);
        const std::size_t units = (payload+sizeof(std::int64_t)-1)/sizeof(std::int64_t);
        ensure_buffer_(units==0?1:units); if (payload>0) std::memcpy(decoded_, VARDATA_ANY(raw), payload);
        raw_detoasted.free_if_copy();
        decoded_chunk_=(std::int64_t)chunk_no; decoded_bytes_=payload;
        systable_endscan(s); relation_close(rel, AccessShareLock);
        return reinterpret_cast<const p_value_type*>(decoded_);
    }
    void resize(std::size_t bytes) { planned_bytes_for_chunk_ = bytes; }
    void prefetch_chunk(std::size_t) {}
    template <class p_value_type, class p_callback> void write_to_each(p_callback&& writer) { write_to<p_value_type>(0, std::forward<p_callback>(writer)); }
    template <class p_value_type, class p_callback> void write_to(std::size_t chunk_no, p_callback&& writer) {
        static_assert(bioseq::indexing::index_chunk_writer_fn<p_callback, p_value_type>, "table write_to writer shape");
        const std::size_t payload = planned_bytes_for_chunk_;
        if (payload % sizeof(p_value_type) != 0) bioseq_log<logging_level::error, logging_target::postgres>( "bioseq index table: payload is not a whole number of elements");
        std::vector<char> buf(payload, 0);
        writer(chunk_no, reinterpret_cast<p_value_type*>(buf.data()), payload / sizeof(p_value_type));
        Relation rel = open_(true, RowExclusiveLock);
        { const Oid pk = bioseq::postgres::support::find_primary_key_oid(rel);
          ScanKeyData k[1]; ScanKeyInit(&k[0],1,BTEqualStrategyNumber,F_INT8EQ,
              bioseq::postgres::support::pg_datum::from(static_cast<std::int64_t>(chunk_no)).raw());
          SysScanDesc s = systable_beginscan(rel, pk, OidIsValid(pk), nullptr, 1, k);
          HeapTuple old_t = systable_getnext(s); if (HeapTupleIsValid(old_t)) simple_heap_delete(rel, &old_t->t_self);
          systable_endscan(s); }
        bytea* blob = reinterpret_cast<bytea*>(palloc(VARHDRSZ + payload)); SET_VARSIZE(blob, VARHDRSZ + payload);
        if (payload>0) std::memcpy(VARDATA(blob), buf.data(), payload);
        bioseq::postgres::support::pg_datum_list<3> values{};
        values.set(1, static_cast<std::int64_t>(chunk_no));
        values.set(2, static_cast<std::int32_t>(payload));
        values.set_raw(3, bioseq::postgres::support::pg_datum::from_pointer(blob));
        HeapTuple tup = values.form_tuple(RelationGetDescr(rel));
        CatalogTupleInsert(rel, tup); heap_freetuple(tup); pfree(blob);
        relation_close(rel, RowExclusiveLock); decoded_chunk_ = -1;
    }
    template <class p_callback> void for_each_sequence(p_callback&& loader);
    void clear(std::string_view name);
};

// ---- shared native relation-drop helper -----------------------------------
[[noreturn]] inline void index_clear_spi_error_as_cpp(
        const char* stage,
        int status) {
    bioseq::postgres::support::invoke_pg_as_cpp([stage, status] {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("bioseq index clear failed"),
             errdetail("stage=%s status=%d", stage, status),
             errhint("The transaction was aborted so stale index relations cannot be accepted.")));
    });
    pg_unreachable();
}

struct drop_index_relation_selector
{
    enum class kind : std::uint8_t
    {
        all_index_tables_best_effort,
        prefix_strict
    };

    kind selected_kind;
    std::string_view prefix;

    [[nodiscard]] static constexpr drop_index_relation_selector
    all_index_tables() noexcept
    {
        return {
            .selected_kind = kind::all_index_tables_best_effort,
            .prefix = {}
        };
    }

    [[nodiscard]] static constexpr drop_index_relation_selector
    with_prefix(std::string_view selected_prefix) noexcept
    {
        return {
            .selected_kind = kind::prefix_strict,
            .prefix = selected_prefix
        };
    }
};

}  // namespace bioseq::storage::indexing

namespace bioseq::storage::indexing {
namespace {

enum class drop_index_spi_stage : std::uint8_t
{
    connect,
    discovery,
    drop,
    finish
};

enum class drop_index_return_code_action : std::uint8_t
{
    continue_operation,
    return_without_error,
    finish_then_return,
    raise_strict_error
};

[[nodiscard]] constexpr int
expected_drop_index_spi_status(drop_index_spi_stage stage) noexcept
{
    switch (stage) {
        case drop_index_spi_stage::connect:
            return SPI_OK_CONNECT;
        case drop_index_spi_stage::discovery:
            return SPI_OK_SELECT;
        case drop_index_spi_stage::drop:
            return SPI_OK_UTILITY;
        case drop_index_spi_stage::finish:
            return SPI_OK_FINISH;
    }
    return SPI_ERROR_OPUNKNOWN;
}

[[nodiscard]] constexpr drop_index_return_code_action
drop_index_return_code_policy(
        drop_index_relation_selector::kind selector_kind,
        drop_index_spi_stage stage,
        int status) noexcept
{
    if (status == expected_drop_index_spi_status(stage))
        return drop_index_return_code_action::continue_operation;
    if (selector_kind
            == drop_index_relation_selector::kind::prefix_strict) {
        return drop_index_return_code_action::raise_strict_error;
    }
    if (stage == drop_index_spi_stage::discovery
        || stage == drop_index_spi_stage::drop) {
        return drop_index_return_code_action::finish_then_return;
    }
    return drop_index_return_code_action::return_without_error;
}

[[nodiscard]] constexpr const char*
drop_index_spi_stage_name(drop_index_spi_stage stage) noexcept
{
    switch (stage) {
        case drop_index_spi_stage::connect:
            return "SPI_connect";
        case drop_index_spi_stage::discovery:
        case drop_index_spi_stage::drop:
            return "SPI_execute";
        case drop_index_spi_stage::finish:
            return "SPI_finish";
    }
    return "unknown SPI stage";
}

using drop_index_selector_kind = drop_index_relation_selector::kind;
using drop_index_status_action = drop_index_return_code_action;
static_assert(drop_index_return_code_policy(
    drop_index_selector_kind::all_index_tables_best_effort,
    drop_index_spi_stage::connect,
    SPI_ERROR_CONNECT)
    == drop_index_status_action::return_without_error);
static_assert(drop_index_return_code_policy(
    drop_index_selector_kind::all_index_tables_best_effort,
    drop_index_spi_stage::discovery,
    SPI_ERROR_ARGUMENT)
    == drop_index_status_action::finish_then_return);
static_assert(drop_index_return_code_policy(
    drop_index_selector_kind::all_index_tables_best_effort,
    drop_index_spi_stage::drop,
    SPI_ERROR_TRANSACTION)
    == drop_index_status_action::finish_then_return);
static_assert(drop_index_return_code_policy(
    drop_index_selector_kind::all_index_tables_best_effort,
    drop_index_spi_stage::finish,
    SPI_ERROR_UNCONNECTED)
    == drop_index_status_action::return_without_error);
static_assert(drop_index_return_code_policy(
    drop_index_selector_kind::prefix_strict,
    drop_index_spi_stage::connect,
    SPI_ERROR_CONNECT)
    == drop_index_status_action::raise_strict_error);
static_assert(drop_index_return_code_policy(
    drop_index_selector_kind::prefix_strict,
    drop_index_spi_stage::discovery,
    SPI_ERROR_ARGUMENT)
    == drop_index_status_action::raise_strict_error);
static_assert(drop_index_return_code_policy(
    drop_index_selector_kind::prefix_strict,
    drop_index_spi_stage::drop,
    SPI_ERROR_TRANSACTION)
    == drop_index_status_action::raise_strict_error);
static_assert(drop_index_return_code_policy(
    drop_index_selector_kind::prefix_strict,
    drop_index_spi_stage::finish,
    SPI_ERROR_UNCONNECTED)
    == drop_index_status_action::raise_strict_error);
static_assert(drop_index_return_code_policy(
    drop_index_selector_kind::all_index_tables_best_effort,
    drop_index_spi_stage::connect,
    SPI_OK_CONNECT)
    == drop_index_status_action::continue_operation);
static_assert(drop_index_return_code_policy(
    drop_index_selector_kind::prefix_strict,
    drop_index_spi_stage::discovery,
    SPI_OK_SELECT)
    == drop_index_status_action::continue_operation);
static_assert(drop_index_return_code_policy(
    drop_index_selector_kind::all_index_tables_best_effort,
    drop_index_spi_stage::drop,
    SPI_OK_UTILITY)
    == drop_index_status_action::continue_operation);
static_assert(drop_index_return_code_policy(
    drop_index_selector_kind::prefix_strict,
    drop_index_spi_stage::finish,
    SPI_OK_FINISH)
    == drop_index_status_action::continue_operation);

class drop_index_spi_session
{
    bool active_ = false;

public:
    drop_index_spi_session() = default;
    drop_index_spi_session(const drop_index_spi_session&) = delete;
    drop_index_spi_session& operator=(const drop_index_spi_session&) = delete;

    [[nodiscard]] int connect()
    {
        const int status = bioseq::postgres::support::invoke_pg_as_cpp(
            [] noexcept { return SPI_connect(); });
        active_ = status == SPI_OK_CONNECT;
        return status;
    }

    [[nodiscard]] int finish()
    {
        const int status = bioseq::postgres::support::invoke_pg_as_cpp(
            [] noexcept { return SPI_finish(); });
        active_ = false;
        return status;
    }

    [[nodiscard]] bool handle_returned_status(
            drop_index_relation_selector selector,
            drop_index_spi_stage stage,
            int status)
    {
        switch (drop_index_return_code_policy(
                    selector.selected_kind, stage, status)) {
            case drop_index_return_code_action::continue_operation:
                return true;
            case drop_index_return_code_action::return_without_error:
                return false;
            case drop_index_return_code_action::finish_then_return:
                (void) finish();
                return false;
            case drop_index_return_code_action::raise_strict_error:
                index_clear_spi_error_as_cpp(
                    drop_index_spi_stage_name(stage), status);
        }
        pg_unreachable();
    }

    ~drop_index_spi_session() noexcept
    {
        if (!active_)
            return;

        volatile int finish_status = SPI_ERROR_UNCONNECTED;
        PG_TRY();
        {
            finish_status = SPI_finish();
        }
        PG_CATCH();
        {
            FlushErrorState();
            proc_exit(1);
        }
        PG_END_TRY();

        active_ = false;
        if (finish_status != SPI_OK_FINISH)
            proc_exit(1);
    }
};

struct discovered_index_relation_name
{
    NameData value{};
    bool is_null = true;
};

[[nodiscard]] std::string
quote_index_relation_name(const std::string& relation_name)
{
    char* quoted = bioseq::postgres::support::invoke_pg_as_cpp(
        [relation_name_data = relation_name.c_str()] noexcept {
            return quote_qualified_identifier(
                "bioseq_internal", relation_name_data);
        });

    std::string result;
    try {
        result.assign(quoted);
    } catch (...) {
        bioseq::postgres::support::invoke_pg_as_cpp(
            [quoted] noexcept { pfree(quoted); });
        throw;
    }
    bioseq::postgres::support::invoke_pg_as_cpp(
        [quoted] noexcept { pfree(quoted); });
    return result;
}

}  // namespace
}  // namespace bioseq::storage::indexing

export namespace bioseq::storage::indexing {

void
drop_index_relations(drop_index_relation_selector selector)
{
    static constexpr char k_all_index_tables_query[] =
        "SELECT c.relname "
        "FROM pg_catalog.pg_class AS c "
        "WHERE c.relnamespace = $1::oid "
        "AND c.relkind = 'r'::\"char\" "
        "AND (starts_with(c.relname::text, 'seqidx_') "
        "OR starts_with(c.relname::text, 'seqidxtab_'))";
    static constexpr char k_prefix_query[] =
        "SELECT c.relname "
        "FROM pg_catalog.pg_class AS c "
        "WHERE c.relnamespace = $1::oid "
        "AND starts_with(c.relname::text, $2::text)";

    const std::string owned_prefix{selector.prefix};
    const Oid internal_namespace =
        bioseq::postgres::support::invoke_pg_as_cpp([] noexcept {
            return get_namespace_oid("bioseq_internal", false);
        });

    drop_index_spi_session spi_session;
    const int connect_status = spi_session.connect();
    if (!spi_session.handle_returned_status(
            selector, drop_index_spi_stage::connect, connect_status)) {
        return;
    }

    std::vector<std::string> relation_names;
    const int discovery_status =
        selector.selected_kind
            == drop_index_relation_selector::kind::
                all_index_tables_best_effort
        ? bioseq::postgres::support::invoke_pg_as_cpp(
            [internal_namespace] noexcept {
                Oid argument_types[1] = {OIDOID};
                Datum argument_values[1] = {
                    ObjectIdGetDatum(internal_namespace)
                };
                return SPI_execute_with_args(
                    k_all_index_tables_query,
                    1, argument_types, argument_values, nullptr,
                    true, 0);
            })
        : bioseq::postgres::support::invoke_pg_as_cpp(
            [internal_namespace,
             prefix_data = owned_prefix.c_str()] noexcept {
                Oid argument_types[2] = {OIDOID, TEXTOID};
                Datum argument_values[2] = {
                    ObjectIdGetDatum(internal_namespace),
                    CStringGetTextDatum(prefix_data)
                };
                const int status = SPI_execute_with_args(
                    k_prefix_query,
                    2, argument_types, argument_values, nullptr,
                    true, 0);
                pfree(DatumGetPointer(argument_values[1]));
                return status;
            });

    if (discovery_status != SPI_OK_SELECT || SPI_tuptable == nullptr) {
        const int effective_status = discovery_status == SPI_OK_SELECT
            ? SPI_ERROR_NOATTRIBUTE
            : discovery_status;
        (void) spi_session.handle_returned_status(
            selector, drop_index_spi_stage::discovery, effective_status);
        return;
    }

    bool invalid_discovery_row = false;
    {
        SPITupleTable* const discovery_table = SPI_tuptable;
        const std::uint64_t discovered_count = SPI_processed;
        relation_names.reserve(static_cast<std::size_t>(discovered_count));
        for (std::uint64_t row = 0; row < discovered_count; ++row) {
            const discovered_index_relation_name discovered =
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [discovery_table, row] noexcept {
                        discovered_index_relation_name result;
                        const Datum name = SPI_getbinval(
                            discovery_table->vals[row],
                            discovery_table->tupdesc,
                            1,
                            &result.is_null);
                        if (!result.is_null)
                            result.value = *DatumGetName(name);
                        return result;
                    });
            if (discovered.is_null) {
                invalid_discovery_row = true;
                break;
            }
            relation_names.emplace_back(NameStr(discovered.value));
        }
        bioseq::postgres::support::invoke_pg_as_cpp(
            [discovery_table] noexcept {
                SPI_freetuptable(discovery_table);
            });
    }

    if (invalid_discovery_row) {
        (void) spi_session.handle_returned_status(
            selector,
            drop_index_spi_stage::discovery,
            SPI_ERROR_NOATTRIBUTE);
        return;
    }

    for (const std::string& relation_name : relation_names) {
        const std::string qualified_name =
            quote_index_relation_name(relation_name);
        const std::string drop_statement = fmt::format(
            "DROP TABLE IF EXISTS {} CASCADE", qualified_name);
        const int drop_status = bioseq::postgres::support::invoke_pg_as_cpp(
            [statement = drop_statement.c_str()] noexcept {
                return SPI_execute(statement, false, 0);
            });
        if (!spi_session.handle_returned_status(
                selector, drop_index_spi_stage::drop, drop_status)) {
            return;
        }
    }

    const int finish_status = spi_session.finish();
    (void) spi_session.handle_returned_status(
        selector, drop_index_spi_stage::finish, finish_status);
}

inline void clear_with_prefix_(const std::string& prefix) {
    drop_index_relations(
        drop_index_relation_selector::with_prefix(prefix));
}

inline void fragment_index_storage::clear(std::string_view name) {
    clear_with_prefix_(fmt::format("seqidx_{}_{:016x}_", short_index_for(name),
                                   identity_hash64(db_oid_, tablespace_oid_, main_relfilenode_, column_attno_, name)));
    index_relid_ = InvalidOid;
}
inline void sequence_index_storage::clear(std::string_view name) {
    clear_with_prefix_(fmt::format("seqidx_{}_{:016x}_", short_index_for(name),
                                   identity_hash64(db_oid_, tablespace_oid_, main_relfilenode_, column_attno_, name)));
    index_relid_ = InvalidOid; decoded_valid_ = false;
}
inline void table_index_storage::clear(std::string_view name) {
    clear_with_prefix_(fmt::format("seqidxtab_{}_{:016x}_", short_index_for(name),
                                   identity_hash64(db_oid_, tablespace_oid_, main_relfilenode_, column_attno_, name)));
    index_relid_ = InvalidOid;
}

inline std::size_t fragment_index_storage::spill_sorted_run(std::span<const std::pair<std::uint64_t,std::uint64_t>> sorted) {
    return sorted_runs_ensure_().spill_sorted_run(sorted);
}

inline std::size_t fragment_index_storage::spill_sorted_run_owned(
        std::vector<std::pair<std::uint64_t,std::uint64_t>> sorted) {
    return sorted_runs_ensure_().spill_sorted_run_owned(std::move(sorted));
}

	inline std::size_t fragment_index_storage::spill_sorted_run_owned_admitted(
	        std::vector<std::pair<std::uint64_t,std::uint64_t>> sorted,
	        std::uint64_t admitted_bytes,
	        bioseq::parallelism::memory_permit permit) {
	    return sorted_runs_ensure_().spill_sorted_run_owned_admitted(
	        std::move(sorted), admitted_bytes, std::move(permit));
	}

	inline void fragment_index_storage::demote_resident_runs_until_available(
	        std::uint64_t bytes_needed) {
	    if (sorted_runs_.has_value()) {
	        sorted_runs_->demote_resident_runs_until_available(bytes_needed);
	    }
	}

template <class p_callback>
inline void fragment_index_storage::merge_runs(p_callback&& on_kmer) {
    if (!sorted_runs_.has_value()) {
        return;
    }
    g_last_index_merge_run_count = std::max<std::uint64_t>(
        g_last_index_merge_run_count, sorted_runs_->run_count());
    sorted_runs_->merge_runs(std::forward<p_callback>(on_kmer));
}

inline direct_minimizer_spill_storage_binding
fragment_index_storage::begin_segment_spill_generation() {
    if (segment_spill_pg_poisoned_) {
        minimizer_generation_storage_error_as_cpp(
            "poisoned minimizer-sequence spill storage reused after PostgreSQL error");
    }
    try {
        for (auto& [segment, runs] : segment_sorted_runs_) {
            (void)segment;
            runs.release_runs();
        }
    } catch (const bioseq::postgres::support::postgres_error&) {
        for (auto& [segment, runs] : segment_sorted_runs_) {
            (void)segment;
            runs.abandon_after_pg_error_noexcept();
        }
        segment_sorted_runs_.clear();
        segment_spill_binding_ = {};
        segment_spill_pg_poisoned_ = true;
        throw;
    }
    segment_sorted_runs_.clear();
    std::uint64_t token = next_segment_spill_generation_token_.load(
        std::memory_order_relaxed);
    for (;;) {
        if (token == 0) {
            segment_spill_binding_ = {};
            return segment_spill_binding_;
        }
        const std::uint64_t next = token
                == (std::numeric_limits<std::uint64_t>::max)()
            ? 0u : token + 1u;
        if (next_segment_spill_generation_token_.compare_exchange_weak(
                token,
                next,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            break;
        }
    }
    segment_spill_binding_ = direct_minimizer_spill_storage_binding{
        .generation_token = token,
        .mutation_revision = 0,
    };
    return segment_spill_binding_;
}

inline void fragment_index_storage::record_segment_spill_mutation_() noexcept {
    if (!segment_spill_binding_.valid()) {
        return;
    }
    if (segment_spill_binding_.mutation_revision
            == (std::numeric_limits<std::uint64_t>::max)()) {
        segment_spill_binding_ = {};
        return;
    }
    ++segment_spill_binding_.mutation_revision;
}

inline std::size_t fragment_index_storage::spill_sorted_segment_run(
        std::uint32_t position_segment,
        std::span<const std::pair<std::uint64_t,std::uint64_t>> sorted) {
    if (segment_spill_pg_poisoned_) {
        minimizer_generation_storage_error_as_cpp(
            "poisoned minimizer-sequence spill storage reused after PostgreSQL error");
    }
    if (!segment_spill_binding_.valid()) {
        (void)begin_segment_spill_generation();
    }
    auto iterator = std::lower_bound(
        segment_sorted_runs_.begin(),
        segment_sorted_runs_.end(),
        position_segment,
        [](const segment_run_entry_type& entry, std::uint32_t segment) {
            return entry.first < segment;
        });
    if (iterator == segment_sorted_runs_.end()
        || iterator->first != position_segment) {
        const std::size_t offset = static_cast<std::size_t>(
            iterator - segment_sorted_runs_.begin());
        if (!ensure_segment_run_metadata_capacity_(
                segment_sorted_runs_.size() + 1u)) {
            minimizer_generation_storage_error_as_cpp(
                "minimizer-sequence spill segment metadata admission failed");
        }
        iterator = segment_sorted_runs_.insert(
            segment_sorted_runs_.begin()
                + static_cast<std::ptrdiff_t>(offset),
            segment_run_entry_type{position_segment, sorted_pair_run_store{}});
    }
    // sorted_pair_run_store may compact/demote physical runs internally. Those
    // transformations preserve the exact canonical pair stream; this one
    // The mutation revision increment names the logical run injection, not its
    // eventual memory-vs-BufFile representation.
    try {
        const std::size_t run = iterator->second.spill_sorted_run(sorted);
        inject_minimizer_generation_failure_as_cpp(
            minimizer_generation_failure_injection::minimizer_sequence_spill,
            "minimizer_sequence_spill");
        record_segment_spill_mutation_();
        return run;
    } catch (const bioseq::postgres::support::postgres_error&) {
        for (auto& [segment, runs] : segment_sorted_runs_) {
            (void)segment;
            runs.abandon_after_pg_error_noexcept();
        }
        segment_spill_binding_ = {};
        segment_spill_pg_poisoned_ = true;
        throw;
    }
}

inline bool fragment_index_storage::reroute_segment_runs(
        std::uint64_t old_span,
        std::uint64_t new_span,
        std::uint64_t target_length) {
    if (!segment_spill_binding_.valid()
        || old_span == 0 || new_span == 0) {
        if (segment_spill_pg_poisoned_) {
            minimizer_generation_storage_error_as_cpp(
                "poisoned minimizer-sequence reroute storage reused after PostgreSQL error");
        }
        return false;
    }
    struct admitted_segment_runs {
        bioseq::parallelism::memory_permit permit;
        std::vector<segment_run_entry_type> values;
    } rerouted;
    struct admitted_pair_batch {
        bioseq::parallelism::memory_permit permit;
        std::vector<std::pair<std::uint64_t,std::uint64_t>> values;
    } routed_batch;
    const auto ensure_capacity = []<class p_value_type>(
            bioseq::parallelism::memory_permit& permit,
            std::vector<p_value_type>& values,
            std::size_t required) -> bool {
        const std::uint64_t retained = bioseq::parallelism::saturating_mul(
            static_cast<std::uint64_t>(values.capacity()),
            static_cast<std::uint64_t>(sizeof(p_value_type)));
        if (retained == (std::numeric_limits<std::uint64_t>::max)()
            || (retained != 0u
                && (!permit || permit.bytes() < retained))) {
            return false;
        }
        if (values.capacity() >= required) {
            return true;
        }
        const std::uint64_t headroom = bioseq::parallelism::saturating_mul(
            bioseq::parallelism::saturating_mul(
                static_cast<std::uint64_t>(required),
                static_cast<std::uint64_t>(sizeof(p_value_type))),
            bioseq::parallelism::k_vector_preallocation_headroom_factor);
        const std::uint64_t peak = bioseq::parallelism::saturating_add(
            retained, headroom);
        if (headroom == (std::numeric_limits<std::uint64_t>::max)()
            || peak == (std::numeric_limits<std::uint64_t>::max)()) {
            return false;
        }
        if (permit) {
            if (!permit.grow_to(peak)) {
                return false;
            }
        } else {
            auto acquired =
                bioseq::parallelism::memory_admission_controller::instance()
                    .try_reserve(peak);
            if (!acquired) {
                return false;
            }
            permit = std::move(*acquired);
        }
        values.reserve(required);
        const std::uint64_t exact = bioseq::parallelism::saturating_mul(
            static_cast<std::uint64_t>(values.capacity()),
            static_cast<std::uint64_t>(sizeof(p_value_type)));
        if (exact == (std::numeric_limits<std::uint64_t>::max)()
            || exact > permit.bytes()) {
            return false;
        }
        permit.shrink_to(exact);
        return true;
    };
    const auto rerouted_store = [&](std::uint32_t segment)
            -> sorted_pair_run_store& {
        auto iterator = std::lower_bound(
            rerouted.values.begin(),
            rerouted.values.end(),
            segment,
            [](const segment_run_entry_type& entry, std::uint32_t value) {
                return entry.first < value;
            });
        if (iterator == rerouted.values.end() || iterator->first != segment) {
            const std::size_t offset = static_cast<std::size_t>(
                iterator - rerouted.values.begin());
            if (!ensure_capacity(
                    rerouted.permit,
                    rerouted.values,
                    rerouted.values.size() + 1u)) {
                minimizer_generation_storage_error_as_cpp(
                    "minimizer-sequence reroute segment metadata admission failed");
            }
            iterator = rerouted.values.insert(
                rerouted.values.begin() + static_cast<std::ptrdiff_t>(offset),
                segment_run_entry_type{segment, sorted_pair_run_store{}});
        }
        return iterator->second;
    };
    bool valid = true;
    try {
        for (auto& [old_segment, runs] : segment_sorted_runs_) {
            runs.merge_runs(
                [&](std::uint64_t hash, auto&& positions) {
                    positions.for_each_batch(
                        [&](std::span<const std::uint64_t> batch) {
                            if (!valid) {
                                return;
                            }
                            std::size_t offset = 0;
                            while (offset < batch.size()) {
                                const std::uint64_t first_encoded = batch[offset];
                                const std::uint64_t first_position =
                                    first_encoded >> 1u;
                                const std::uint64_t first_segment =
                                    first_position / new_span;
                                if (first_position >= target_length
                                    || first_position / old_span != old_segment
                                    || first_segment
                                        > (std::numeric_limits<std::uint32_t>::max)()) {
                                    valid = false;
                                    return;
                                }
                                std::size_t end = offset + 1u;
                                for (; end < batch.size(); ++end) {
                                    const std::uint64_t position = batch[end] >> 1u;
                                    const std::uint64_t new_segment =
                                        position / new_span;
                                    if (position >= target_length
                                        || position / old_span != old_segment
                                        || new_segment
                                            > (std::numeric_limits<std::uint32_t>::max)()) {
                                        valid = false;
                                        return;
                                    }
                                    if (new_segment != first_segment) {
                                        break;
                                    }
                                }
                                const std::size_t count = end - offset;
                                if (!ensure_capacity(
                                        routed_batch.permit,
                                        routed_batch.values,
                                        count)) {
                                    minimizer_generation_storage_error_as_cpp(
                                        "minimizer-sequence reroute pair batch admission failed");
                                }
                                routed_batch.values.clear();
                                for (std::size_t i = offset; i < end; ++i) {
                                    const std::uint64_t encoded_position = batch[i];
                                    routed_batch.values.emplace_back(
                                        hash, encoded_position);
                                }
                                rerouted_store(
                                    static_cast<std::uint32_t>(first_segment))
                                    .spill_sorted_run(
                                        std::span<const std::pair<
                                            std::uint64_t,std::uint64_t>>{
                                                routed_batch.values});
                                offset = end;
                            }
                            // Streaming preserves hash/position order and
                            // bounds transient ownership to one admitted batch.
                        });
                });
            if (!valid) {
                segment_spill_binding_ = {};
                segment_spill_pg_poisoned_ = true;
                return false;
            }
        }
        // Every source store has been replayed successfully. Close its PostgreSQL
        // handles at a checked boundary before publication of the replacement;
        // the replacement stores remain open for qualification/encode replay.
        for (auto& [segment, runs] : segment_sorted_runs_) {
            (void)segment;
            runs.release_runs();
        }
    } catch (const bioseq::postgres::support::postgres_error&) {
        for (auto& [segment, runs] : segment_sorted_runs_) {
            (void)segment;
            runs.abandon_after_pg_error_noexcept();
        }
        for (auto& [segment, runs] : rerouted.values) {
            (void)segment;
            runs.abandon_after_pg_error_noexcept();
        }
        segment_spill_binding_ = {};
        segment_spill_pg_poisoned_ = true;
        throw;
    } catch (...) {
        for (auto& [segment, runs] : segment_sorted_runs_) {
            (void)segment;
            runs.release_runs_noexcept();
        }
        for (auto& [segment, runs] : rerouted.values) {
            (void)segment;
            runs.release_runs_noexcept();
        }
        segment_spill_binding_ = {};
        segment_spill_pg_poisoned_ = true;
        throw;
    }
    segment_sorted_runs_.swap(rerouted.values);
    std::swap(segment_sorted_runs_metadata_permit_, rerouted.permit);
    record_segment_spill_mutation_();
    return true;
}

template <class p_callback>
inline void fragment_index_storage::merge_segment_runs(
        p_callback&& on_segment_kmer) {
    if (segment_spill_pg_poisoned_) {
        minimizer_generation_storage_error_as_cpp(
            "poisoned minimizer-sequence merge storage reused after PostgreSQL error");
    }
    std::uint64_t total_runs = 0;
    for (const auto& [segment, runs] : segment_sorted_runs_) {
        (void)segment;
        total_runs += runs.run_count();
    }
    g_last_index_merge_run_count = std::max(
        g_last_index_merge_run_count, total_runs);
    try {
        for (auto& [segment, runs] : segment_sorted_runs_) {
            bool first_callback = true;
            runs.merge_runs(
                [&on_segment_kmer, segment, &first_callback](
                        std::uint64_t hash,
                        auto&& positions) {
                    if (first_callback) {
                        first_callback = false;
                        inject_minimizer_generation_failure_as_cpp(
                            minimizer_generation_failure_injection::
                                minimizer_sequence_merge,
                            "minimizer_sequence_merge_callback");
                    }
                    on_segment_kmer(segment, hash, positions);
                });
        }
    } catch (const bioseq::postgres::support::postgres_error&) {
        for (auto& [segment, runs] : segment_sorted_runs_) {
            (void)segment;
            runs.abandon_after_pg_error_noexcept();
        }
        segment_spill_binding_ = {};
        segment_spill_pg_poisoned_ = true;
        throw;
    }
}

inline sorted_run_store_stats fragment_index_storage::sorted_run_stats() const noexcept {
    return sorted_runs_.has_value() ? sorted_runs_->stats() : sorted_run_store_stats{};
}

inline void fragment_index_storage::snapshot_sorted_run_stats_before_release() {
    if (!sorted_runs_.has_value()) {
        return;
    }
    const sorted_run_store_stats st = sorted_runs_->stats();
    const std::uint64_t total_runs = sorted_runs_->run_count();
    g_last_index_merge_run_count = total_runs;
    g_last_minimizer_sequence_build_stats =
        last_minimizer_sequence_build_stats{
            .backend_pid = static_cast<std::uint64_t>(MyProcPid),
            .db_oid = static_cast<std::uint64_t>(db_oid_),
            .relfilenode = static_cast<std::uint64_t>(main_relfilenode_),
            .sequence_rowno = static_cast<std::uint64_t>(query_key_),
            .total_run_count = total_runs,
            .resident_runs = st.resident_runs,
            .file_runs = st.file_runs,
            .resident_run_bytes = st.resident_bytes,
            .resident_peak_bytes = st.resident_peak_bytes,
            .spilled_run_bytes = st.spilled_bytes,
            .postgres_temp_file_bytes = st.postgres_temp_file_bytes,
            .postgres_temp_file_count = st.postgres_temp_file_count,
            .merge_reader_buffer_bytes = st.merge_reader_buffer_bytes,
            .merge_pass_count = st.merge_pass_count,
            .spill_count_from_pressure = st.spill_count_from_pressure,
            .resident_demotions = st.resident_demotions,
            .position_spill_bytes = st.position_spill_bytes,
            .position_spill_count = st.position_spill_count,
        };
    bioseq_log<logging_level::debug>("[bioseq.index.minimizer_sequence_build.row] backend_pid={} db_oid={} relfilenode={} sequence_rowno={} "
            "resident_runs={} file_runs={} resident_run_bytes={} resident_peak_bytes={} "
            "spilled_run_bytes={} postgres_temp_file_bytes={} postgres_temp_file_count={} "
            "merge_reader_buffer_bytes={} merge_pass_count={} spill_count_from_pressure={} "
            "resident_demotions={} position_spill_bytes={} position_spill_count={}",
            MyProcPid,
            static_cast<unsigned long long>(db_oid_),
            static_cast<unsigned long long>(main_relfilenode_),
            static_cast<unsigned long long>(query_key_),
            static_cast<unsigned long long>(st.resident_runs),
            static_cast<unsigned long long>(st.file_runs),
            static_cast<unsigned long long>(st.resident_bytes),
            static_cast<unsigned long long>(st.resident_peak_bytes),
            static_cast<unsigned long long>(st.spilled_bytes),
            static_cast<unsigned long long>(st.postgres_temp_file_bytes),
            static_cast<unsigned long long>(st.postgres_temp_file_count),
            static_cast<unsigned long long>(st.merge_reader_buffer_bytes),
            static_cast<unsigned long long>(st.merge_pass_count),
            static_cast<unsigned long long>(st.spill_count_from_pressure),
            static_cast<unsigned long long>(st.resident_demotions),
            static_cast<unsigned long long>(st.position_spill_bytes),
            static_cast<unsigned long long>(st.position_spill_count));
}

inline void fragment_index_storage::release_runs_noexcept() noexcept {
    if (sorted_runs_.has_value()) {
        sorted_runs_->release_runs_noexcept();
        sorted_runs_.reset();
    }
    for (auto& [segment, runs] : segment_sorted_runs_) {
        (void)segment;
        runs.release_runs_noexcept();
    }
    segment_sorted_runs_.clear();
    record_segment_spill_mutation_();
}

inline void fragment_index_storage::release_runs() {
    try {
        if (sorted_runs_.has_value()) {
            sorted_runs_->release_runs();
            sorted_runs_.reset();
        }
        for (auto& [segment, runs] : segment_sorted_runs_) {
            (void)segment;
            runs.release_runs();
        }
        segment_sorted_runs_.clear();
        record_segment_spill_mutation_();
    } catch (const bioseq::postgres::support::postgres_error&) {
        if (sorted_runs_.has_value()) {
            sorted_runs_->abandon_after_pg_error_noexcept();
            sorted_runs_.reset();
        }
        for (auto& [segment, runs] : segment_sorted_runs_) {
            (void)segment;
            runs.abandon_after_pg_error_noexcept();
        }
        segment_sorted_runs_.clear();
        segment_spill_binding_ = {};
        segment_spill_pg_poisoned_ = true;
        throw;
    }
}

inline void fragment_index_storage::abandon_after_pg_error_noexcept() noexcept {
    if (sorted_runs_.has_value()) {
        sorted_runs_->abandon_after_pg_error_noexcept();
        sorted_runs_.reset();
    }
    for (auto& [segment, runs] : segment_sorted_runs_) {
        (void)segment;
        runs.abandon_after_pg_error_noexcept();
    }
    segment_sorted_runs_.clear();
    segment_spill_binding_ = {};
    segment_spill_pg_poisoned_ = true;

    // Memory contexts, relation references, and any palloc-backed scratch are
    // owned by the failing transaction. Null them without invoking PostgreSQL
    // cleanup while the exact leaf error unwinds; abort reclaims them through
    // their original PG owners.
    own_mctx_ = nullptr;
    index_relid_ = InvalidOid;
    decoded_ = nullptr;
    decoded_units_ = 0;
    decoded_bytes_ = 0;
    decoded_sub_ = k_directory_sub_key;
    decoded_chunk_ = (std::numeric_limits<std::size_t>::max)();
    scratch_ = nullptr;
    scratch_units_ = 0;
    chunk_cache_.clear();
    chunk_cache_bytes_ = 0;
    chunk_payload_bytes_.clear();
}

inline void fragment_index_storage::close_checked() {
    release_runs();
    chunk_cache_.clear();
    chunk_cache_bytes_ = 0;
    chunk_payload_bytes_.clear();
    if (own_mctx_ != nullptr) {
        MemoryContext finished_context = std::exchange(own_mctx_, nullptr);
        decoded_ = nullptr;
        scratch_ = nullptr;
        bioseq::postgres::support::invoke_pg_as_cpp(
            [finished_context] { MemoryContextDelete(finished_context); });
    }
}

inline void write_serialized_index_chunk_pg(
        Oid user_relid,
        Oid db_oid,
        Oid tablespace_oid,
        Oid main_relfilenode,
        std::int16_t column_attno,
        std::string_view index_name,
        std::uint8_t bucket_idx,
        std::uint32_t shard_idx,
        std::uint64_t sequence_rowno,
        index_sub_key_t sub_key,
        std::uint32_t chunk_no,
        std::span<const char> bytes) {
    ErrorData* postgres_error = nullptr;
    {
        fragment_index_storage storage;
        storage.db_oid_ = db_oid;
        storage.tablespace_oid_ = tablespace_oid;
        storage.main_relfilenode_ = main_relfilenode;
        storage.user_relid_ = user_relid;
        storage.column_attno_ = column_attno;
        storage.index_name_ = index_name;
        storage.bucket_idx_ = bucket_idx;
        storage.shard_idx_ = shard_idx;
        storage.query_key_ = sequence_rowno;
        storage.own_mctx_ = bioseq::postgres::support::invoke_pg_as_cpp([] {
            return AllocSetContextCreate(
                CurrentMemoryContext,
                "bioseq_async_index_persist",
                ALLOCSET_SMALL_SIZES);
        });
        try {
            storage.write_serialized_chunk_checked(
                sequence_rowno,
                sub_key,
                chunk_no,
                bytes,
                false);
            storage.close_checked();
        } catch (bioseq::postgres::support::postgres_error& error) {
            storage.abandon_after_pg_error_noexcept();
            postgres_error = error.release_error_data();
        } catch (...) {
            try {
                storage.close_checked();
            } catch (...) {
                storage.abandon_after_pg_error_noexcept();
            }
            throw;
        }
    }
    if (postgres_error != nullptr) {
        ReThrowError(postgres_error);
    }
}

inline void mark_serialized_index_write_successful_pg(
        Oid user_relid,
        Oid main_relfilenode,
        std::int16_t column_attno,
        std::string_view index_name) {
    static_cast<void>(upsert_metadata_age_log_pg(
        user_relid,
        main_relfilenode,
        column_attno,
        index_name));
}

// ---- table_index_storage::for_each_sequence (test-only CHUNKED_PER_TABLE) ---
template <class p_callback>
inline void table_index_storage::for_each_sequence(p_callback&& loader) {
    Relation main = table_open(user_relid_, AccessShareLock);
    TupleDesc td = RelationGetDescr(main);
    Snapshot snap = RegisterSnapshot(GetTransactionSnapshot());
    const std::size_t total = (std::size_t)column_table_row_count(main);
    List* projected = list_make1_int(column_attno_);
    column_read_state* rs = begin_column_read(main, td, projected, NIL, own_mctx_, snap, /*randomAccess=*/false, nullptr);
    MemoryContext inner = AllocSetContextCreate(own_mctx_, "seqidx_table_inner_seq", ALLOCSET_DEFAULT_SIZES);
    std::vector<Datum> vals(td->natts);
    std::vector<bool>  nullsv(td->natts);
    Datum* valp = vals.data();
    std::vector<char> nullbuf(td->natts);
    bool* nulls = reinterpret_cast<bool*>(nullbuf.data());
    std::uint64_t rowno = 0;
	    while (read_next_column_row(rs, valp, nulls, &rowno)) {
	        if (nulls[column_attno_ - 1]) continue;
	        MemoryContextReset(inner);
	        MemoryContext previous_context = MemoryContextSwitchTo(inner);
	        auto hdr_detoasted = bioseq::postgres::support::detoast_as<bioseq::pg_types::seq>(
	            bioseq::postgres::support::pg_datum::from_raw(valp[column_attno_ - 1]));
	        bioseq::pg_types::seq* hdr = hdr_detoasted.ptr;
	        MemoryContextSwitchTo(previous_context);
        const std::uint8_t alpha = hdr->alphabet_code;
        const std::uint8_t bw = logical_storage_unit_byte_width(hdr);
        if (!is_locator_stub(hdr)) {
            bioseq::pg_types::seq::dispatch_instance(
                alpha, bw, [hdr, &loader, rowno, total]<class p_sequence_type>() {
                compact_sequence_range<p_sequence_type> rng{*hdr};
                const auto view =
                    bioseq::storage::make_lazy_sequence_view(rng);
                loader(view, static_cast<std::size_t>(rowno), total);
            });
        } else {
            const sequence_locator loc = decode_locator_from_stub(hdr);
            bioseq::pg_types::seq::dispatch_instance(
                alpha,
                bw,
                [this, &loader, loc, rowno, total, rs, inner]<class p_sequence_type>() {
                using source_binding =
                    bioseq::storage::postgres_lazy_sequence_binding<p_sequence_type>;
                const source_binding source_binding{
                    db_oid_, tablespace_oid_, main_relfilenode_,
                    column_attno_, loc,
                    source_binding::expected_last_fragment_padding(loc),
                    rs->snapshot, read_caches_, inner};
                bioseq::storage::with_postgres_lazy_sequence<p_sequence_type>(
                    source_binding,
                    [&loader, rowno, total](auto view){
                        loader(view, static_cast<std::size_t>(rowno), total);
                    });
            });
        }
        hdr_detoasted.free_if_copy();
    }
    // with_postgres_lazy_sequence keyed cached fragment readers by rs->snapshot.
    // Release them before the outer reader unregisters a refreshed snapshot.
    if (read_caches_ != nullptr) {
        read_caches_->close_all(/*commit=*/false);
    }
    end_column_read(rs);
    UnregisterSnapshot(snap);
    MemoryContextDelete(inner);
    table_close(main, AccessShareLock);
}

// ---- conformance gates ---------------------------------------------------
template <class p_storage>
concept publicly_mutable_minimizer_sequence_segment_spill_state =
    requires(p_storage& storage) {
        storage.segment_sorted_runs_;
        storage.segment_spill_binding_;
        storage.segment_spill_pg_poisoned_;
        storage.next_segment_spill_generation_token_;
    };
static_assert(!publicly_mutable_minimizer_sequence_segment_spill_state<
    fragment_index_storage>);
static_assert(!std::is_move_assignable_v<fragment_index_storage>);
static_assert(!std::is_move_assignable_v<sequence_index_storage>);
static_assert(!std::is_move_assignable_v<table_index_storage>);
static_assert(bioseq::indexing::is_index_chunked_storage_for<
                  fragment_index_storage, sequence_index_layout::fragment_chunks>,
              "fragment_index_storage must satisfy CHUNKED_PER_FRAGMENT");
static_assert(bioseq::indexing::is_index_chunked_storage_for<
                  sequence_index_storage, sequence_index_layout::sequence_inline>,
              "sequence_index_storage must satisfy INLINE_PER_SEQUENCE");
static_assert(bioseq::indexing::is_index_chunked_storage_for<
                  table_index_storage, sequence_index_layout::table_chunks>,
              "table_index_storage must satisfy CHUNKED_PER_TABLE");

}  // namespace bioseq::storage::indexing

extern "C" PGDLLEXPORT void
bioseq_hash_major_parallel_scan_probe_worker_main(
        dsm_segment* segment,
        shm_toc* toc) {
    bioseq::storage::indexing::
        hash_major_minimizer_parallel_scan_probe_worker_main_pg(
            segment, toc);
}
