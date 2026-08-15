/**
 * @file catalog_contracts.cppm
 * @brief Defines chunked-storage catalog contracts, schema validation, and failure seams.
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
export module bioseq.postgres.storage.indexing.chunked_storage:catalog_contracts;
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
static_assert(__cplusplus >= 202302L, "C++23-or-later");

export namespace bioseq::storage::indexing {
namespace indexing_entity_handles =
    bioseq::sql::generated::core::storage_indexing_pg_chunked_storage::entity_handles;
using minimizer_generation_storage_catalog_row =
    bioseq::storage::catalog_rows::minimizer_generation_storage;
using minimizer_generation_storage_codec =
    bioseq::postgres::storage::manager::sql_row_codecs::minimizer_generation_storage_codec;
using metadata_age_log_catalog_row = bioseq::storage::indexing::catalog_rows::metadata_age_log;
using metadata_age_log_codec =
    bioseq::postgres::storage::indexing::sql_row_codecs::metadata_age_log_codec;

static_assert(indexing_entity_handles::minimizer_generation_id_seq.schema_binding() ==
              bioseq::postgres::support::sql_schema_binding::fixed);

using bioseq::indexing::crc64_ecma182_accumulator;
using bioseq::indexing::derive_minimizer_position_extent_sub_key;
using bioseq::indexing::direct_minimizer_spill_storage_binding;
using bioseq::indexing::encoded_grouped_soa_generation;
using bioseq::indexing::encoded_hash_major_minimizer_head;
using bioseq::indexing::encoded_hash_major_minimizer_manifest;
using bioseq::indexing::expected_hash_major_minimizer_generation;
using bioseq::indexing::expected_minimizer_generation_identity;
using bioseq::indexing::grouped_soa_bucket_directory_entry;
using bioseq::indexing::grouped_soa_directory_view;
using bioseq::indexing::grouped_soa_minimizer_profile;
using bioseq::indexing::hash_major_minimizer_authorized_control_range;
using bioseq::indexing::hash_major_minimizer_authorized_posting_range;
using bioseq::indexing::hash_major_minimizer_consume_control_block_fn;
using bioseq::indexing::hash_major_minimizer_consume_posting_bytes_fn;
using bioseq::indexing::hash_major_minimizer_encoded_metadata;
using bioseq::indexing::hash_major_minimizer_error;
using bioseq::indexing::hash_major_minimizer_head;
using bioseq::indexing::hash_major_minimizer_logical_stream_sink_ref;
using bioseq::indexing::hash_major_minimizer_logical_stream_source_ref;
using bioseq::indexing::hash_major_minimizer_manifest;
using bioseq::indexing::hash_major_minimizer_storage_profile;
using bioseq::indexing::hash_major_minimizer_stream_kind;
using bioseq::indexing::index_chunk_address;
using bioseq::indexing::index_chunk_cache_policy;
using bioseq::indexing::index_chunk_read_stats;
using bioseq::indexing::index_sub_key_range;
using bioseq::indexing::index_sub_key_request;
using bioseq::indexing::index_sub_key_t;
using bioseq::indexing::k_directory_sub_key;
using bioseq::indexing::k_generation_head_sub_key;
using bioseq::indexing::k_hash_major_dense_seal_sub_key;
using bioseq::indexing::k_hash_major_generation_head_sub_key;
using bioseq::indexing::k_hash_major_minimizer_head_bytes;
using bioseq::indexing::k_hash_major_minimizer_manifest_bytes;
using bioseq::indexing::k_hash_major_minimizer_maximum_control_block_bytes;
using bioseq::indexing::k_hash_major_minimizer_read_slab_bytes;
using bioseq::indexing::k_hash_major_minimizer_row_payload_bytes;
using bioseq::indexing::k_max_index_chunk_size_bytes;
using bioseq::indexing::k_minimizers_per_fragment_index_name;
using bioseq::indexing::k_minimizers_per_sequence_index_name;
using bioseq::indexing::k_position_extent_sub_key_base;
using bioseq::indexing::k_symbol_counts_index_name;
using bioseq::indexing::k_temporary_run_sub_key_base;
using bioseq::indexing::minimizer_generation_encoding_error;
using bioseq::indexing::minimizer_generation_head;
using bioseq::indexing::minimizer_generation_manifest;
using bioseq::indexing::minimizer_generation_row_view;
using bioseq::indexing::minimizer_generation_row_write_receipt;
using bioseq::indexing::minimizer_hashmap_fragment_index;
using bioseq::indexing::project_minimizer_extent_sub_key;
using bioseq::indexing::sequence_index_layout;
using bioseq::indexing::unchecked_minimizer_generation_row_view;
using bioseq::indexing::validated_minimizer_generation_row_receipt;

static_assert(k_hash_major_generation_head_sub_key > k_temporary_run_sub_key_base);
static_assert(k_hash_major_generation_head_sub_key < k_hash_major_dense_seal_sub_key);
static_assert(k_hash_major_dense_seal_sub_key < k_generation_head_sub_key);

// PostgreSQL-side mirror checks: the macros are in scope here.
static_assert(bioseq::indexing::k_varlena_header_bytes == VARHDRSZ, "VARHDRSZ mirror drift");
static_assert(bioseq::indexing::k_postgres_max_allocation_bytes == MaxAllocSize,
              "MaxAllocSize mirror drift");

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
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());

    /// Add one SQL-bound value and fail closed on overflow.
    void add(std::uint64_t& destination, std::uint64_t increment) noexcept {
        if (destination > k_sql_bigint_max ||
            increment > k_sql_bigint_max - std::min(destination, k_sql_bigint_max)) {
            destination = k_sql_bigint_max;
            are_stats_saturated = true;
            return;
        }
        destination += increment;
    }

    /// Saturating merge used for policy, source and sidecar stage deltas.
    void merge(const cold_index_preparation_stats& rhs) noexcept {
        are_stats_saturated = are_stats_saturated || rhs.are_stats_saturated;
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
        add(extent_crc_raw_bytes_hashed, rhs.extent_crc_raw_bytes_hashed);
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

    [[nodiscard]] constexpr bool
    operator==(const cold_index_preparation_stats&) const noexcept = default;
};

/// Forward declarations required by the mutual friend-based authorization
/// cycle between executor context and catalog source authority.
struct minimizer_generation_administration_identity;
class minimizer_generation_catalog_source_authorization;
/// Authorize one catalog source against an already registered snapshot.
[[nodiscard]] inline minimizer_generation_catalog_source_authorization
authorize_minimizer_generation_catalog_source_pg(
    const minimizer_generation_administration_identity&, std::uint64_t, Snapshot);

/// Move-only, non-owning executor read capability.  The CustomScan owns the
/// Relation lock and the registered MVCC snapshot. Cold-index preparation may
/// borrow both only while this token remains valid.  Moving the token transfers
/// the right to borrow, but neither move nor destruction performs PostgreSQL
/// work (in particular, it never closes the Relation or unregisters the
/// snapshot).
class borrowed_executor_read_context {
  private:
    friend minimizer_generation_catalog_source_authorization
    authorize_minimizer_generation_catalog_source_pg(
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

    borrowed_executor_read_context(Relation relation, Snapshot registered_snapshot) noexcept
        : relation_(relation), snapshot_(registered_snapshot),
          relation_oid_(relation == nullptr ? InvalidOid : RelationGetRelid(relation)),
          database_oid_(relation == nullptr ? InvalidOid : relation->rd_locator.dbOid),
          tablespace_oid_(relation == nullptr ? InvalidOid : relation->rd_locator.spcOid),
          relfilenode_(relation == nullptr ? InvalidOid
                                           : static_cast<Oid>(relation->rd_locator.relNumber)) {}

  public:
    borrowed_executor_read_context() = delete;

    [[nodiscard]] static std::optional<borrowed_executor_read_context>
    bind_checked(Relation relation, Snapshot registered_snapshot) noexcept {
        borrowed_executor_read_context context{relation, registered_snapshot};
        if (!context.valid())
            return std::nullopt;
        return std::optional<borrowed_executor_read_context>{std::move(context)};
    }

    borrowed_executor_read_context(const borrowed_executor_read_context&) = delete;
    borrowed_executor_read_context& operator=(const borrowed_executor_read_context&) = delete;
    borrowed_executor_read_context(borrowed_executor_read_context&& other) noexcept
        : relation_(std::exchange(other.relation_, nullptr)),
          snapshot_(std::exchange(other.snapshot_, nullptr)),
          relation_oid_(std::exchange(other.relation_oid_, InvalidOid)),
          database_oid_(std::exchange(other.database_oid_, InvalidOid)),
          tablespace_oid_(std::exchange(other.tablespace_oid_, InvalidOid)),
          relfilenode_(std::exchange(other.relfilenode_, InvalidOid)) {}
    borrowed_executor_read_context& operator=(borrowed_executor_read_context&& other) noexcept {
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
        return relation_ != nullptr && snapshot_ != nullptr && IsMVCCSnapshot(snapshot_) &&
               snapshot_->regd_count > 0 && OidIsValid(relation_oid_) &&
               OidIsValid(database_oid_) && OidIsValid(tablespace_oid_) &&
               OidIsValid(relfilenode_) && RelationGetRelid(relation_) == relation_oid_ &&
               relation_->rd_locator.dbOid == database_oid_ &&
               relation_->rd_locator.spcOid == tablespace_oid_ &&
               static_cast<Oid>(relation_->rd_locator.relNumber) == relfilenode_;
    }

    [[nodiscard]] bool matches(Oid database_oid,
                               Oid tablespace_oid,
                               Oid relfilenode,
                               Oid relation_oid) const noexcept {
        return valid() && database_oid_ == database_oid && tablespace_oid_ == tablespace_oid &&
               relfilenode_ == relfilenode && relation_oid_ == relation_oid;
    }

    [[nodiscard]] Relation relation() const noexcept {
        return relation_;
    }
    [[nodiscard]] Snapshot snapshot() const noexcept {
        return snapshot_;
    }
    [[nodiscard]] Oid relation_oid() const noexcept {
        return relation_oid_;
    }
    [[nodiscard]] Oid database_oid() const noexcept {
        return database_oid_;
    }
    [[nodiscard]] Oid tablespace_oid() const noexcept {
        return tablespace_oid_;
    }
    [[nodiscard]] Oid relfilenode() const noexcept {
        return relfilenode_;
    }
};

static_assert(!std::is_copy_constructible_v<borrowed_executor_read_context>);
static_assert(std::is_nothrow_move_constructible_v<borrowed_executor_read_context>);

/// Monotonic project clock converted to the unsigned SQL-bound domain.
[[nodiscard]] inline std::uint64_t cold_index_now_ns() noexcept {
    return bioseq::instrumentation::clock::now_ns();
}

/// Add one elapsed interval while defending against a broken clock sample.
inline void add_cold_index_elapsed(cold_index_preparation_stats& stats,
                                   std::uint64_t& destination,
                                   std::uint64_t begin_ns) noexcept {
    const std::uint64_t end_ns = cold_index_now_ns();
    stats.add(destination, end_ns >= begin_ns ? end_ns - begin_ns : 0);
}

/// Attribute PostgreSQL shared-buffer traffic since `begin` to this delta.
inline void add_cold_index_buffer_usage(cold_index_preparation_stats& stats,
                                        const BufferUsage& begin) noexcept {
    BufferUsage delta{};
    BufferUsageAccumDiff(&delta, &pgBufferUsage, &begin);
    if (delta.shared_blks_hit < 0 || delta.shared_blks_read < 0) {
        stats.are_stats_saturated = true;
        return;
    }
    stats.add(stats.shared_blks_hit, static_cast<std::uint64_t>(delta.shared_blks_hit));
    stats.add(stats.shared_blks_read, static_cast<std::uint64_t>(delta.shared_blks_read));
}

inline std::uint64_t g_last_index_merge_run_count = 0;
inline last_minimizer_sequence_build_stats g_last_minimizer_sequence_build_stats{};

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
inline void
arm_minimizer_generation_failure_injection(minimizer_generation_failure_injection point) noexcept {
    g_minimizer_generation_failure_injection = point;
}

/// Clear any armed generation failure point.
inline void disarm_minimizer_generation_failure_injection() noexcept {
    g_minimizer_generation_failure_injection = minimizer_generation_failure_injection::none;
}

/// Raise an armed failure from inside an existing PostgreSQL error boundary.
/// Composite PostgreSQL operations use this leaf to avoid nesting a second
/// context-restoring boundary.
inline void inject_minimizer_generation_failure_leaf(minimizer_generation_failure_injection point,
                                                     const char* point_name) {
    if (g_minimizer_generation_failure_injection != point)
        return;
    g_minimizer_generation_failure_injection = minimizer_generation_failure_injection::none;
    ereport(ERROR,
            (errcode(ERRCODE_IO_ERROR),
             errmsg("injected PostgreSQL generation failure"),
             errdetail("failure_point=%s", point_name),
             errhint("retry the failed index build in a fresh transaction")));
}

/// Translate an armed PostgreSQL failure into the C++ boundary representation.
inline void inject_minimizer_generation_failure_as_cpp(minimizer_generation_failure_injection point,
                                                       const char* point_name) {
    bioseq::postgres::support::invoke_pg_as_cpp(
        [point, point_name] { inject_minimizer_generation_failure_leaf(point, point_name); });
}
#else
inline void
arm_minimizer_generation_failure_injection(minimizer_generation_failure_injection) noexcept {}
inline void disarm_minimizer_generation_failure_injection() noexcept {}
inline void inject_minimizer_generation_failure_leaf(minimizer_generation_failure_injection,
                                                     const char*) {}
inline void inject_minimizer_generation_failure_as_cpp(minimizer_generation_failure_injection,
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

[[nodiscard]] constexpr const char*
inline_sequence_pg_leaf_failure_name(const inline_sequence_pg_leaf_failure point) noexcept {
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
inline void
arm_inline_sequence_pg_leaf_failure(const inline_sequence_pg_leaf_failure point) noexcept {
    g_inline_sequence_pg_leaf_failure = point;
}

/// Clear the armed inline sequence leaf failure.
inline void disarm_inline_sequence_pg_leaf_failure() noexcept {
    g_inline_sequence_pg_leaf_failure = inline_sequence_pg_leaf_failure::none;
}

/// Raise and consume an inline sequence failure inside a PostgreSQL leaf.
/// Consumption makes the next statement a real recovery probe.
inline void inject_inline_sequence_pg_leaf_failure(const inline_sequence_pg_leaf_failure point) {
    if (g_inline_sequence_pg_leaf_failure != point) {
        return;
    }
    g_inline_sequence_pg_leaf_failure = inline_sequence_pg_leaf_failure::none;
    ereport(ERROR,
            (errcode(ERRCODE_IO_ERROR),
             errmsg("injected inline sequence storage PostgreSQL leaf failure"),
             errdetail("failure_point=%s", inline_sequence_pg_leaf_failure_name(point)),
             errhint("retry the symbol-count read in a fresh statement")));
}
#else
inline void arm_inline_sequence_pg_leaf_failure(inline_sequence_pg_leaf_failure) noexcept {}
inline void disarm_inline_sequence_pg_leaf_failure() noexcept {}
inline void inject_inline_sequence_pg_leaf_failure(inline_sequence_pg_leaf_failure) noexcept {}
#endif

/// Query-key attribute in a fragment-chunk tuple.
inline constexpr AttrNumber k_attribute_query_key = 1;
/// Sub-key attribute in a fragment-chunk tuple.
inline constexpr AttrNumber k_attribute_sub_key = 2;
/// Chunk-number attribute in a fragment-chunk tuple.
inline constexpr AttrNumber k_attribute_chunk_number = 3;
/// Logical byte-count attribute in a fragment-chunk tuple.
inline constexpr AttrNumber k_attribute_chunk_bytes = 4;
/// Payload attribute in a fragment-chunk tuple.
inline constexpr AttrNumber k_attribute_blob = 5;

/// Query-key attribute in the frozen generation-row layout.
inline constexpr AttrNumber k_minimizer_generation_attribute_query_key = 1;
/// Generation identifier attribute in the frozen generation-row layout.
inline constexpr AttrNumber k_minimizer_generation_attribute_generation_id = 2;
/// Position-segment attribute in the frozen generation-row layout.
inline constexpr AttrNumber k_minimizer_generation_attribute_position_segment = 3;
/// Sub-key attribute in the frozen generation-row layout.
inline constexpr AttrNumber k_minimizer_generation_attribute_sub_key = 4;
/// Chunk-number attribute in the frozen generation-row layout.
inline constexpr AttrNumber k_minimizer_generation_attribute_chunk_number = 5;
/// Logical byte-count attribute in the frozen generation-row layout.
inline constexpr AttrNumber k_minimizer_generation_attribute_chunk_bytes = 6;
/// Address-bound checksum attribute in the frozen generation-row layout.
inline constexpr AttrNumber k_minimizer_generation_attribute_checksum = 7;
/// PLAIN payload attribute in the frozen generation-row layout.
inline constexpr AttrNumber k_minimizer_generation_attribute_blob = 8;

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
static_assert(k_minimizer_generation_index_generation_id ==
              k_minimizer_generation_primary_key_attributes[0]);
static_assert(k_minimizer_generation_index_query_key ==
              k_minimizer_generation_primary_key_attributes[1]);
static_assert(k_minimizer_generation_index_sub_key ==
              k_minimizer_generation_primary_key_attributes[2]);
static_assert(k_minimizer_generation_index_position_segment ==
              k_minimizer_generation_primary_key_attributes[3]);
static_assert(k_minimizer_generation_index_chunk_number ==
              k_minimizer_generation_primary_key_attributes[4]);

/// Preserve all CRC-64 bits in PostgreSQL's signed BIGINT container.
[[nodiscard]] constexpr std::int64_t checksum_to_pg_bigint(std::uint64_t checksum) noexcept {
    return std::bit_cast<std::int64_t>(checksum);
}

/// Recover the exact CRC-64 word from PostgreSQL's signed BIGINT container.
[[nodiscard]] constexpr std::uint64_t checksum_from_pg_bigint(std::int64_t checksum) noexcept {
    return std::bit_cast<std::uint64_t>(checksum);
}

static_assert(checksum_from_pg_bigint(checksum_to_pg_bigint(0)) == 0);
static_assert(
    checksum_from_pg_bigint(checksum_to_pg_bigint((std::numeric_limits<std::uint64_t>::max)())) ==
    (std::numeric_limits<std::uint64_t>::max)());
static_assert(checksum_to_pg_bigint(std::uint64_t{1} << 63u) ==
              (std::numeric_limits<std::int64_t>::min)());

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
        return error == minimizer_generation_schema_error::none && OidIsValid(primary_index_oid);
    }
};

/// Raise a generation-storage PostgreSQL error through the C++ boundary.
[[noreturn]] inline void minimizer_generation_storage_error(std::string_view message);

[[nodiscard]] constexpr std::string_view
minimizer_generation_schema_error_name(minimizer_generation_schema_error error) noexcept {
    switch (error) {
    case minimizer_generation_schema_error::none:
        return "none";
    case minimizer_generation_schema_error::not_heap:
        return "relation is not heap AM";
    case minimizer_generation_schema_error::attribute_count:
        return "attribute count";
    case minimizer_generation_schema_error::attribute_number:
        return "attribute number";
    case minimizer_generation_schema_error::attribute_dropped:
        return "dropped attribute";
    case minimizer_generation_schema_error::attribute_name:
        return "attribute name";
    case minimizer_generation_schema_error::attribute_type:
        return "attribute type";
    case minimizer_generation_schema_error::attribute_options:
        return "attribute options";
    case minimizer_generation_schema_error::attribute_nullable:
        return "nullable attribute";
    case minimizer_generation_schema_error::blob_not_plain:
        return "blob storage is not PLAIN";
    case minimizer_generation_schema_error::wrong_namespace:
        return "relation namespace";
    case minimizer_generation_schema_error::wrong_relation_name:
        return "relation name";
    case minimizer_generation_schema_error::wrong_heap_persistence:
        return "heap is not UNLOGGED";
    case minimizer_generation_schema_error::wrong_index_persistence:
        return "primary index is not UNLOGGED";
    case minimizer_generation_schema_error::wrong_heap_tablespace:
        return "heap tablespace";
    case minimizer_generation_schema_error::wrong_index_tablespace:
        return "primary index tablespace";
    case minimizer_generation_schema_error::wrong_heap_fillfactor:
        return "heap fillfactor";
    case minimizer_generation_schema_error::wrong_index_fillfactor:
        return "primary index fillfactor";
    case minimizer_generation_schema_error::primary_index_count:
        return "primary index count";
    case minimizer_generation_schema_error::primary_index_not_unique:
        return "primary index is not unique";
    case minimizer_generation_schema_error::primary_index_not_immediate:
        return "primary index is deferred";
    case minimizer_generation_schema_error::primary_index_not_ready:
        return "primary index is not ready/valid/live";
    case minimizer_generation_schema_error::primary_index_key_count:
        return "primary index key count";
    case minimizer_generation_schema_error::primary_index_key_order:
        return "primary index key order";
    case minimizer_generation_schema_error::primary_index_not_btree:
        return "primary index is not btree";
    case minimizer_generation_schema_error::primary_index_partial:
        return "primary index is partial";
    case minimizer_generation_schema_error::primary_index_expression:
        return "primary index has expressions";
    case minimizer_generation_schema_error::primary_index_opclass:
        return "primary index opclass";
    case minimizer_generation_schema_error::primary_index_collation:
        return "primary index collation";
    case minimizer_generation_schema_error::primary_index_options:
        return "primary index ordering/opclass options";
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
[[nodiscard]] inline minimizer_generation_schema_inspection
inspect_minimizer_generation_schema(Relation relation,
                                    cold_index_preparation_stats* cold_stats = nullptr,
                                    minimizer_generation_schema_expectation expected = {},
                                    Oid permitted_auxiliary_index_oid = InvalidOid) {
    if (relation == nullptr || relation->rd_rel->relam != HEAP_TABLE_AM_OID ||
        relation->rd_rel->relkind != RELKIND_RELATION) {
        return {.error = minimizer_generation_schema_error::not_heap};
    }
    if (OidIsValid(expected.namespace_oid) &&
        RelationGetNamespace(relation) != expected.namespace_oid) {
        return {.error = minimizer_generation_schema_error::wrong_namespace};
    }
    if (!expected.relation_name.empty() &&
        std::string_view{RelationGetRelationName(relation)} != expected.relation_name) {
        return {.error = minimizer_generation_schema_error::wrong_relation_name};
    }
    if (relation->rd_rel->relpersistence != RELPERSISTENCE_UNLOGGED) {
        return {.error = minimizer_generation_schema_error::wrong_heap_persistence};
    }
    if (RelationGetFillFactor(relation, HEAP_DEFAULT_FILLFACTOR) != 100) {
        return {.error = minimizer_generation_schema_error::wrong_heap_fillfactor};
    }
    if (OidIsValid(expected.tablespace_oid) &&
        relation->rd_locator.spcOid != expected.tablespace_oid) {
        return {.error = minimizer_generation_schema_error::wrong_heap_tablespace};
    }
    TupleDesc tuple_desc = RelationGetDescr(relation);
    if (tuple_desc == nullptr ||
        tuple_desc->natts !=
            static_cast<int>(k_minimizer_generation_index_attribute_descriptors.size())) {
        return {.error = minimizer_generation_schema_error::attribute_count};
    }
    for (std::size_t index = 0; index < k_minimizer_generation_index_attribute_descriptors.size();
         ++index) {
        const minimizer_generation_index_attribute_descriptor& expected =
            k_minimizer_generation_index_attribute_descriptors[index];
        Form_pg_attribute attribute = TupleDescAttr(tuple_desc, static_cast<int>(index));
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
        if (attribute->atttypmod != -1 || OidIsValid(attribute->attcollation) ||
            attribute->atthasdef || attribute->atthasmissing || attribute->attidentity != '\0' ||
            attribute->attgenerated != '\0' || attribute->attcompression != '\0') {
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
        if (index_oid == permitted_auxiliary_index_oid)
            continue;
        ++index_count;
        Relation index_relation = index_open(index_oid, AccessShareLock);
        if (cold_stats != nullptr) {
            cold_stats->add(cold_stats->sidecar_index_opens, 1);
            cold_stats->add(cold_stats->lock_acquires, 1);
        }
        if (index_relation->rd_index != nullptr && index_relation->rd_index->indisprimary) {
            ++primary_index_count;
            primary_index_oid = index_oid;
            const Form_pg_index index_form = index_relation->rd_index;
            if (!index_form->indisunique) {
                primary_error = minimizer_generation_schema_error::primary_index_not_unique;
            } else if (index_form->indnullsnotdistinct) {
                primary_error = minimizer_generation_schema_error::primary_index_options;
            } else if (!index_form->indimmediate) {
                primary_error = minimizer_generation_schema_error::primary_index_not_immediate;
            } else if (!index_form->indisvalid || !index_form->indisready ||
                       !index_form->indislive) {
                primary_error = minimizer_generation_schema_error::primary_index_not_ready;
            } else if (index_form->indnatts !=
                           static_cast<std::int16_t>(
                               k_minimizer_generation_primary_key_attributes.size()) ||
                       index_form->indnkeyatts !=
                           static_cast<std::int16_t>(
                               k_minimizer_generation_primary_key_attributes.size())) {
                primary_error = minimizer_generation_schema_error::primary_index_key_count;
            } else if (index_relation->rd_rel->relam != BTREE_AM_OID) {
                primary_error = minimizer_generation_schema_error::primary_index_not_btree;
            } else if (index_relation->rd_rel->relpersistence != RELPERSISTENCE_UNLOGGED) {
                primary_error = minimizer_generation_schema_error::wrong_index_persistence;
            } else if (index_relation->rd_locator.spcOid != relation->rd_locator.spcOid ||
                       (OidIsValid(expected.tablespace_oid) &&
                        index_relation->rd_locator.spcOid != expected.tablespace_oid)) {
                primary_error = minimizer_generation_schema_error::wrong_index_tablespace;
            } else if (RelationGetFillFactor(index_relation, BTREE_DEFAULT_FILLFACTOR) != 90) {
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
                     key < k_minimizer_generation_primary_key_attributes.size();
                     ++key) {
                    if (index_form->indkey.values[key] !=
                        k_minimizer_generation_primary_key_attributes[key]) {
                        primary_error = minimizer_generation_schema_error::primary_index_key_order;
                        break;
                    }
                    if (get_index_column_opclass(index_oid, static_cast<int>(key + 1u)) !=
                        k_expected_opclasses[key]) {
                        primary_error = minimizer_generation_schema_error::primary_index_opclass;
                        break;
                    }
                    if (index_relation->rd_indcollation == nullptr ||
                        OidIsValid(index_relation->rd_indcollation[key])) {
                        primary_error = minimizer_generation_schema_error::primary_index_collation;
                        break;
                    }
                    if (index_relation->rd_indoption == nullptr ||
                        index_relation->rd_indoption[key] != 0 ||
                        (index_relation->rd_opcoptions != nullptr &&
                         index_relation->rd_opcoptions[key] != nullptr)) {
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
inspect_bound_minimizer_generation_schema(Relation relation,
                                          Relation primary_index_relation,
                                          minimizer_generation_schema_expectation expected = {}) {
    if (relation == nullptr || relation->rd_rel->relam != HEAP_TABLE_AM_OID ||
        relation->rd_rel->relkind != RELKIND_RELATION) {
        return {.error = minimizer_generation_schema_error::not_heap};
    }
    if (OidIsValid(expected.namespace_oid) &&
        RelationGetNamespace(relation) != expected.namespace_oid) {
        return {.error = minimizer_generation_schema_error::wrong_namespace};
    }
    if (!expected.relation_name.empty() &&
        std::string_view{RelationGetRelationName(relation)} != expected.relation_name) {
        return {.error = minimizer_generation_schema_error::wrong_relation_name};
    }
    if (relation->rd_rel->relpersistence != RELPERSISTENCE_UNLOGGED) {
        return {.error = minimizer_generation_schema_error::wrong_heap_persistence};
    }
    if (RelationGetFillFactor(relation, HEAP_DEFAULT_FILLFACTOR) != 100) {
        return {.error = minimizer_generation_schema_error::wrong_heap_fillfactor};
    }
    if (OidIsValid(expected.tablespace_oid) &&
        relation->rd_locator.spcOid != expected.tablespace_oid) {
        return {.error = minimizer_generation_schema_error::wrong_heap_tablespace};
    }
    TupleDesc tuple_desc = RelationGetDescr(relation);
    if (tuple_desc == nullptr ||
        tuple_desc->natts !=
            static_cast<int>(k_minimizer_generation_index_attribute_descriptors.size())) {
        return {.error = minimizer_generation_schema_error::attribute_count};
    }
    for (std::size_t index = 0; index < k_minimizer_generation_index_attribute_descriptors.size();
         ++index) {
        const minimizer_generation_index_attribute_descriptor& expected =
            k_minimizer_generation_index_attribute_descriptors[index];
        Form_pg_attribute attribute = TupleDescAttr(tuple_desc, static_cast<int>(index));
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
        if (attribute->atttypmod != -1 || OidIsValid(attribute->attcollation) ||
            attribute->atthasdef || attribute->atthasmissing || attribute->attidentity != '\0' ||
            attribute->attgenerated != '\0' || attribute->attcompression != '\0') {
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
    if (!exactly_one_index || primary_index_relation == nullptr ||
        primary_index_relation->rd_index == nullptr) {
        return {.error = minimizer_generation_schema_error::primary_index_count};
    }
    const bool exact_primary =
        RelationGetPrimaryKeyIndex(relation) == RelationGetRelid(primary_index_relation);
    if (!exact_primary ||
        primary_index_relation->rd_index->indrelid != RelationGetRelid(relation) ||
        !primary_index_relation->rd_index->indisprimary) {
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
    if (!index_form->indisvalid || !index_form->indisready || !index_form->indislive) {
        return {.error = minimizer_generation_schema_error::primary_index_not_ready};
    }
    if (index_form->indnatts !=
            static_cast<std::int16_t>(k_minimizer_generation_primary_key_attributes.size()) ||
        index_form->indnkeyatts !=
            static_cast<std::int16_t>(k_minimizer_generation_primary_key_attributes.size())) {
        return {.error = minimizer_generation_schema_error::primary_index_key_count};
    }
    if (primary_index_relation->rd_rel->relam != BTREE_AM_OID) {
        return {.error = minimizer_generation_schema_error::primary_index_not_btree};
    }
    if (primary_index_relation->rd_rel->relpersistence != RELPERSISTENCE_UNLOGGED) {
        return {.error = minimizer_generation_schema_error::wrong_index_persistence};
    }
    if (primary_index_relation->rd_locator.spcOid != relation->rd_locator.spcOid ||
        (OidIsValid(expected.tablespace_oid) &&
         primary_index_relation->rd_locator.spcOid != expected.tablespace_oid)) {
        return {.error = minimizer_generation_schema_error::wrong_index_tablespace};
    }
    if (RelationGetFillFactor(primary_index_relation, BTREE_DEFAULT_FILLFACTOR) != 90) {
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
    for (std::size_t key = 0; key < k_minimizer_generation_primary_key_attributes.size(); ++key) {
        if (index_form->indkey.values[key] != k_minimizer_generation_primary_key_attributes[key]) {
            return {.error = minimizer_generation_schema_error::primary_index_key_order};
        }
        if (get_index_column_opclass(primary_index_oid, static_cast<int>(key + 1u)) !=
            k_expected_opclasses[key]) {
            return {.error = minimizer_generation_schema_error::primary_index_opclass};
        }
        if (primary_index_relation->rd_indcollation == nullptr ||
            OidIsValid(primary_index_relation->rd_indcollation[key])) {
            return {.error = minimizer_generation_schema_error::primary_index_collation};
        }
        if (primary_index_relation->rd_indoption == nullptr ||
            primary_index_relation->rd_indoption[key] != 0 ||
            (primary_index_relation->rd_opcoptions != nullptr &&
             primary_index_relation->rd_opcoptions[key] != nullptr)) {
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
    minimizer_generation_relation_kind kind = minimizer_generation_relation_kind::malformed;
    minimizer_generation_schema_error schema_error = minimizer_generation_schema_error::none;
};

/// Non-PostgreSQL control-flow result used only while binding a production
/// read session.  The binder closes every normally-opened descriptor before
/// throwing it. PostgreSQL errors remain postgres_error and are left to the
/// ResourceOwner/transaction abort path.
struct minimizer_generation_relation_open_failure {
    minimizer_generation_relation_inspection inspection{};
};

/// Integration-only catalog corruption seam. It copies a real parsed
/// predicate or expression tree from a donor index onto the otherwise-valid
/// primary index row. Tests execute it inside a transaction and roll back,
/// proving PG17's varlena `indpred`/`indexprs` path rather than fabricating a
/// C++ Relation object.
inline void copy_primary_index_shape_pg(Oid table_oid, Oid donor_index_oid, bool copy_predicate) {
    if (!superuser()) {
        minimizer_generation_storage_error(
            "current generation index-shape corruption requires superuser");
    }
    Relation target =
        bioseq::postgres::support::try_open_postgres_relation(table_oid, AccessShareLock);
    if (target == nullptr) {
        minimizer_generation_storage_error(
            "current generation index-shape target relation is missing");
    }
    const minimizer_generation_schema_inspection schema =
        inspect_minimizer_generation_schema(target, nullptr, {}, donor_index_oid);
    relation_close(target, AccessShareLock);
    if (!schema) {
        minimizer_generation_storage_error(
            "current generation index-shape target schema is not initially valid");
    }
    Relation donor =
        bioseq::postgres::support::try_open_postgres_relation(donor_index_oid, AccessShareLock);
    if (donor == nullptr || donor->rd_index == nullptr || donor->rd_index->indrelid != table_oid ||
        (copy_predicate ? RelationGetIndexPredicate(donor) == NIL
                        : RelationGetIndexExpressions(donor) == NIL)) {
        if (donor != nullptr)
            relation_close(donor, AccessShareLock);
        minimizer_generation_storage_error(
            "current generation index-shape donor does not carry the requested shape");
    }
    relation_close(donor, AccessShareLock);

    Relation catalog = table_open(IndexRelationId, RowExclusiveLock);
    HeapTuple primary_tuple =
        SearchSysCacheCopy1(INDEXRELID, ObjectIdGetDatum(schema.primary_index_oid));
    HeapTuple donor_tuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(donor_index_oid));
    if (!HeapTupleIsValid(primary_tuple) || !HeapTupleIsValid(donor_tuple)) {
        if (HeapTupleIsValid(primary_tuple))
            heap_freetuple(primary_tuple);
        if (HeapTupleIsValid(donor_tuple))
            ReleaseSysCache(donor_tuple);
        table_close(catalog, RowExclusiveLock);
        minimizer_generation_storage_error(
            "current generation index-shape catalog tuple is missing");
    }
    const AttrNumber shape_attno = copy_predicate ? Anum_pg_index_indpred : Anum_pg_index_indexprs;
    bool donor_is_null = false;
    const Datum donor_shape =
        heap_getattr(donor_tuple, shape_attno, RelationGetDescr(catalog), &donor_is_null);
    if (donor_is_null) {
        heap_freetuple(primary_tuple);
        ReleaseSysCache(donor_tuple);
        table_close(catalog, RowExclusiveLock);
        minimizer_generation_storage_error(
            "current generation index-shape donor catalog value is NULL");
    }
    std::array<Datum, Natts_pg_index> values{};
    std::array<bool, Natts_pg_index> nulls{};
    std::array<bool, Natts_pg_index> replaces{};
    values[shape_attno - 1] = datumCopy(donor_shape, false, -1);
    replaces[shape_attno - 1] = true;
    HeapTuple modified = heap_modify_tuple(
        primary_tuple, RelationGetDescr(catalog), values.data(), nulls.data(), replaces.data());
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

} // namespace bioseq::storage::indexing
