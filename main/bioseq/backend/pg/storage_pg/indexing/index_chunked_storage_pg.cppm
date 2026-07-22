/**
 * @file index_chunked_storage_pg.cppm
 * @brief DOC0279 §8 — the concrete Postgres storage implementations that satisfy
 *        the `is_index_chunked_storage_for<...>` concepts.
 *
 * Index chunks are raw BYTEA tuples in heap relations (heap_form_tuple /
 * CatalogTupleInsert + systable_beginscan), bypassing columnarisation
 * (Invariant C). The per-sequence layouts co-locate with the sequence's
 * `(bucket, shard)`; the table layout shards via `index_table_shard`.
 */
module;
#include "common.h"

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
#include "storage/procarray.h"
#include "storage/relfilelocator.h"
#include "storage/shm_toc.h"
#include "utils/lsyscache.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
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
export module bioseq.storage.indexing_pg.chunked_storage;
import bioseq.common.mlogging;
import bioseq.pg_utils;                       // try_safe_relation_open, find_primary_key_oid
import bioseq.storage.manager;                 // seq_caches, seq_locator, format helpers
import bioseq.storage.bucket_math;             // seq_locator
import bioseq.storage.size;            // sequence_size_t
import bioseq.indexing.type;           // SequenceIndexType, index_sub_key_t, MAX_INDEX_CHUNK_SIZE_BYTES, sentinels
import bioseq.indexing.storage;        // the concepts
import bioseq.indexing.minimizer_hashmap;   // SerializedHeader/Bucket validation
import bioseq.indexing.mpf5_image_v2;       // explicit fixed-endian MPF5 image ABI
import bioseq.indexing.minimizer_direct_inverted_index;
import bioseq.indexing.minimizer_format5_compact_h32_adapter;
import bioseq.table.io;                // BioseqColumnBeginRead/ReadNextRow/EndRead/ReadState
import bioseq.storage.scanner_pg;      // with_lazy_postgres, make_scanner_pg_in_memory (for_each_sequence)
import bioseq.storage.lazy;            // lazy_compact_sequence
import bioseq.types.pg_compact_sequence;  // pgseq_t, dispatch_instance
import bioseq.sequences;               // compact_sequence_range
import bioseq.sequences.simd;          // canonical bounded base decoding for source proof
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;
import bioseq.storage.indexing_pg.residency;
import bioseq.storage.indexing_pg.sorted_runs;
import bioseq.instr;
import bioseq.column.tam;                    // DOC0322 §7.4 — IsBioseqColumnTableAmTable (leaf)
import bioseq.storage.metadata;                     // DOC0322 §7.4 — BioseqColumnTableRowCount (moved to storage_metadata:core)
import bioseq.storage.physical;
static_assert(__cplusplus >= 202302L, "C++23-or-later");

export namespace bioseq::storage::indexing {

using bioseq::indexing::SequenceIndexType;
using bioseq::indexing::index_sub_key_t;
using bioseq::indexing::MAX_INDEX_CHUNK_SIZE_BYTES;
using bioseq::indexing::TEMP_RUN_BASE;
using bioseq::indexing::POSITION_EXTENT_BASE;
using bioseq::indexing::DIRECTORY_SUB_KEY;
using bioseq::indexing::FORMAT6_DENSE_SEAL_SUB_KEY;
using bioseq::indexing::FORMAT6_GENERATION_HEAD_SUB_KEY;
using bioseq::indexing::GENERATION_HEAD_SUB_KEY;
using bioseq::indexing::MinimizerHashmapFragmentIndex;
using bioseq::indexing::MinimizerGenerationHead;
using bioseq::indexing::MinimizerGenerationManifest;
using bioseq::indexing::MinimizerGenerationRowView;
using bioseq::indexing::ValidatedMinimizerGenerationRowReceipt;
using bioseq::indexing::UnchecksummedMinimizerGenerationRowView;
using bioseq::indexing::MinimizerGenerationRowWriteReceipt;
using bioseq::indexing::MinimizerGenerationWireError;
using bioseq::indexing::MinimizerGenerationPayloadProfile;
using bioseq::indexing::MinimizerGrouped32ColdLoadRoute;
using bioseq::indexing::MinimizerGrouped32ColdRoutePlan;
using bioseq::indexing::ExpectedMinimizerCompatibility;
using bioseq::indexing::EncodedMinimizerGeneration;
using bioseq::indexing::MinimizerBucketDirEntry;
using bioseq::indexing::DirView;
using bioseq::indexing::IndexChunkAddress;
using bioseq::indexing::IndexChunkCachePolicy;
using bioseq::indexing::IndexChunkReadStats;
using bioseq::indexing::IndexSubKeyRange;
using bioseq::indexing::MpsSegmentSpillStorageBinding;
using bioseq::indexing::derive_minimizer_position_extent_sub_key;
using bioseq::indexing::project_minimizer_extent_sub_key;
using bioseq::indexing::IndexSubKeyRequest;
using bioseq::indexing::Crc64Ecma182;
using bioseq::indexing::MINIMIZERS_PER_FRAGMENT_INDEX_NAME;
using bioseq::indexing::MINIMIZERS_PER_SEQUENCE_INDEX_NAME;
using bioseq::indexing::SYMBOL_COUNTS_INDEX_NAME;
using bioseq::indexing::EncodedMinimizerFormat6Head;
using bioseq::indexing::EncodedMinimizerFormat6Manifest;
using bioseq::indexing::MinimizerFormat6AuthorizedControlRange;
using bioseq::indexing::MinimizerFormat6AuthorizedPostingRange;
using bioseq::indexing::MinimizerFormat6Compatibility;
using bioseq::indexing::MinimizerFormat6ConsumeControlBlockFn;
using bioseq::indexing::MinimizerFormat6ConsumePostingBytesFn;
using bioseq::indexing::MinimizerFormat6EncodedMetadata;
using bioseq::indexing::MinimizerFormat6Error;
using bioseq::indexing::MinimizerFormat6ExpectedGeneration;
using bioseq::indexing::MinimizerFormat6Head;
using bioseq::indexing::MinimizerFormat6LogicalStreamSinkRef;
using bioseq::indexing::MinimizerFormat6LogicalStreamSourceRef;
using bioseq::indexing::MinimizerFormat6Manifest;
using bioseq::indexing::MinimizerFormat6StreamKind;
using bioseq::indexing::kMinimizerFormat6HeadBytes;
using bioseq::indexing::kMinimizerFormat6ManifestBytes;
using bioseq::indexing::kMinimizerFormat6MaximumControlBlockBytes;
using bioseq::indexing::kMinimizerFormat6ReadSlabBytes;
using bioseq::indexing::kMinimizerFormat6RowPayloadBytes;

static_assert(FORMAT6_GENERATION_HEAD_SUB_KEY > TEMP_RUN_BASE);
static_assert(FORMAT6_GENERATION_HEAD_SUB_KEY
    < FORMAT6_DENSE_SEAL_SUB_KEY);
static_assert(FORMAT6_DENSE_SEAL_SUB_KEY < GENERATION_HEAD_SUB_KEY);

// pg-side mirror checks (§5): the macros ARE in scope here.
static_assert(bioseq::indexing::kVarlenaHeaderBytes == VARHDRSZ, "VARHDRSZ mirror drift");
static_assert(bioseq::indexing::kPgMaxAllocSize == MaxAllocSize, "MaxAllocSize mirror drift");

// Backend-local per-sequence build observability (§8.8/DOC0299). Defined here
// (storage) so merge/release can set it; admin zeroes it at build start and
// mod.cppm reads it through SQL in the same backend.
struct LastMpsBuildStats {
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

/// PostgreSQL-local evidence for one cold persisted-index preparation.  This
/// is a transport delta, not a second execution/fallback authority: the
/// dispatch owner merges it into PipelineOperationalStats exactly once.
/// Every SQL-bound value clamps at INT64_MAX so the PostgreSQL bigint row
/// shape can never wrap a large counter into a plausible small value.
struct ColdIndexPreparationStatsPg {
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
    bool stats_saturated = false;

    static constexpr std::uint64_t sql_bigint_max =
        static_cast<std::uint64_t>(
            (std::numeric_limits<std::int64_t>::max)());

    /// Add one SQL-bound value and fail closed on overflow.
    void add(
            std::uint64_t& destination,
            std::uint64_t increment) noexcept {
        if (destination > sql_bigint_max
            || increment > sql_bigint_max -
                std::min(destination, sql_bigint_max)) {
            destination = sql_bigint_max;
            stats_saturated = true;
            return;
        }
        destination += increment;
    }

    /// Saturating merge used for policy, source and sidecar stage deltas.
    void merge(const ColdIndexPreparationStatsPg& rhs) noexcept {
        stats_saturated = stats_saturated || rhs.stats_saturated;
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
        const ColdIndexPreparationStatsPg&) const noexcept = default;
};

struct Rev3IndexAdministrationIdentity;
class Rev3CatalogSourceProof;
[[nodiscard]] inline Rev3CatalogSourceProof derive_rev3_catalog_source_proof_pg(
    const Rev3IndexAdministrationIdentity&, std::uint64_t, Snapshot);

/// Move-only, non-owning executor read capability.  The CustomScan owns the
/// Relation lock and the registered MVCC snapshot; cold-index preparation may
/// borrow both only while this token remains valid.  Moving the token transfers
/// the right to borrow, but neither move nor destruction performs PostgreSQL
/// work (in particular, it never closes the Relation or unregisters the
/// snapshot).
class BorrowedExecutorReadContextPg {
private:
    friend Rev3CatalogSourceProof derive_rev3_catalog_source_proof_pg(
        const Rev3IndexAdministrationIdentity&, std::uint64_t, Snapshot);

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

    BorrowedExecutorReadContextPg(
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
    BorrowedExecutorReadContextPg() = delete;

    [[nodiscard]] static std::optional<BorrowedExecutorReadContextPg>
    bind_checked(
            Relation relation,
            Snapshot registered_snapshot) noexcept {
        BorrowedExecutorReadContextPg context{
            relation, registered_snapshot};
        if (!context.valid()) return std::nullopt;
        return std::optional<BorrowedExecutorReadContextPg>{
            std::move(context)};
    }

    BorrowedExecutorReadContextPg(
        const BorrowedExecutorReadContextPg&) = delete;
    BorrowedExecutorReadContextPg& operator=(
        const BorrowedExecutorReadContextPg&) = delete;
    BorrowedExecutorReadContextPg(
            BorrowedExecutorReadContextPg&& other) noexcept
        : relation_(std::exchange(other.relation_, nullptr)),
          snapshot_(std::exchange(other.snapshot_, nullptr)),
          relation_oid_(std::exchange(other.relation_oid_, InvalidOid)),
          database_oid_(std::exchange(other.database_oid_, InvalidOid)),
          tablespace_oid_(std::exchange(other.tablespace_oid_, InvalidOid)),
          relfilenode_(std::exchange(other.relfilenode_, InvalidOid)) {}
    BorrowedExecutorReadContextPg& operator=(
            BorrowedExecutorReadContextPg&& other) noexcept {
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
    ~BorrowedExecutorReadContextPg() = default;

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

static_assert(!std::is_copy_constructible_v<BorrowedExecutorReadContextPg>);
static_assert(std::is_nothrow_move_constructible_v<
    BorrowedExecutorReadContextPg>);

/// Monotonic project clock converted to the unsigned SQL-bound domain.
[[nodiscard]] inline std::uint64_t cold_index_now_ns() noexcept {
    return bioseq::instr::clock::now_ns();
}

/// Add one elapsed interval while defending against a broken clock sample.
inline void add_cold_index_elapsed(
        ColdIndexPreparationStatsPg& stats,
        std::uint64_t& destination,
        std::uint64_t begin_ns) noexcept {
    const std::uint64_t end_ns = cold_index_now_ns();
    stats.add(destination, end_ns >= begin_ns ? end_ns - begin_ns : 0);
}

/// Attribute PostgreSQL shared-buffer traffic since `begin` to this delta.
inline void add_cold_index_buffer_usage(
        ColdIndexPreparationStatsPg& stats,
        const BufferUsage& begin) noexcept {
    BufferUsage delta{};
    BufferUsageAccumDiff(&delta, &pgBufferUsage, &begin);
    if (delta.shared_blks_hit < 0 || delta.shared_blks_read < 0) {
        stats.stats_saturated = true;
        return;
    }
    stats.add(stats.shared_blks_hit,
        static_cast<std::uint64_t>(delta.shared_blks_hit));
    stats.add(stats.shared_blks_read,
        static_cast<std::uint64_t>(delta.shared_blks_read));
}

inline std::uint64_t g_last_index_merge_run_count = 0;
inline LastMpsBuildStats g_last_mps_build_stats{};

inline void reset_last_mps_build_stats() noexcept {
    g_last_index_merge_run_count = 0;
    g_last_mps_build_stats = {};
}

enum class Rev3PgFailureInjection : std::uint8_t {
    none,
    mps_spill,
    mps_merge,
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
inline Rev3PgFailureInjection g_rev3_pg_failure_injection =
    Rev3PgFailureInjection::none;

inline void arm_rev3_pg_failure_injection(
        Rev3PgFailureInjection point) noexcept {
    g_rev3_pg_failure_injection = point;
}

inline void disarm_rev3_pg_failure_injection() noexcept {
    g_rev3_pg_failure_injection = Rev3PgFailureInjection::none;
}

// PostgreSQL leaf form: call only from inside an existing invoke_pg_as_cpp
// callback.  Composite PG operations such as one complete SPI session use it
// so an injected ERROR does not introduce a nested context-restoring boundary.
inline void inject_rev3_pg_failure_leaf(
        Rev3PgFailureInjection point,
        const char* point_name) {
    if (g_rev3_pg_failure_injection != point) return;
    g_rev3_pg_failure_injection = Rev3PgFailureInjection::none;
    ereport(ERROR,
        (errcode(ERRCODE_IO_ERROR),
         errmsg("injected revision-3 PostgreSQL failure"),
         errdetail("failure_point=%s", point_name),
         errhint("retry the failed index build in a fresh transaction")));
}

inline void inject_rev3_pg_failure_as_cpp(
        Rev3PgFailureInjection point,
        const char* point_name) {
    bioseq::pg_utils::invoke_pg_as_cpp([point, point_name] {
        inject_rev3_pg_failure_leaf(point, point_name);
    });
}
#else
inline void arm_rev3_pg_failure_injection(
        Rev3PgFailureInjection) noexcept {}
inline void disarm_rev3_pg_failure_injection() noexcept {}
inline void inject_rev3_pg_failure_leaf(
        Rev3PgFailureInjection,
        const char*) {}
inline void inject_rev3_pg_failure_as_cpp(
        Rev3PgFailureInjection,
        const char*) {}
#endif

enum class InlineSequencePgLeafFailure : std::uint8_t {
	none,
	relation_lookup,
	tuple_read,
	detoast,
	close,
};

[[nodiscard]] constexpr const char* inline_sequence_pg_leaf_failure_name(
		const InlineSequencePgLeafFailure point) noexcept {
	switch (point) {
		case InlineSequencePgLeafFailure::none:
			return "none";
		case InlineSequencePgLeafFailure::relation_lookup:
			return "relation_lookup";
		case InlineSequencePgLeafFailure::tuple_read:
			return "tuple_read";
		case InlineSequencePgLeafFailure::detoast:
			return "detoast";
		case InlineSequencePgLeafFailure::close:
			return "close";
	}
	return "invalid";
}

#ifdef BIOSEQ_TEST_INDEXES
inline InlineSequencePgLeafFailure g_inline_sequence_pg_leaf_failure =
	InlineSequencePgLeafFailure::none;

inline void arm_inline_sequence_pg_leaf_failure(
		const InlineSequencePgLeafFailure point) noexcept {
	g_inline_sequence_pg_leaf_failure = point;
}

inline void disarm_inline_sequence_pg_leaf_failure() noexcept {
	g_inline_sequence_pg_leaf_failure = InlineSequencePgLeafFailure::none;
}

// This raw ereport helper is called only from inside one invoke_pg_as_cpp
// operation leaf. Consume first so the next statement in the same backend is
// a real recovery probe.
inline void inject_inline_sequence_pg_leaf_failure(
		const InlineSequencePgLeafFailure point) {
	if (g_inline_sequence_pg_leaf_failure != point) {
		return;
	}
	g_inline_sequence_pg_leaf_failure = InlineSequencePgLeafFailure::none;
	ereport(ERROR,
		(errcode(ERRCODE_IO_ERROR),
		 errmsg("injected inline sequence storage PostgreSQL leaf failure"),
		 errdetail("failure_point=%s",
			inline_sequence_pg_leaf_failure_name(point)),
		 errhint("retry the symbol-count read in a fresh statement")));
}
#else
inline void arm_inline_sequence_pg_leaf_failure(
		InlineSequencePgLeafFailure) noexcept {}
inline void disarm_inline_sequence_pg_leaf_failure() noexcept {}
inline void inject_inline_sequence_pg_leaf_failure(
		InlineSequencePgLeafFailure) noexcept {}
#endif

// Pinned attribute numbers (§8.1) for the CHUNKED_PER_FRAGMENT table.
inline constexpr AttrNumber kAttQueryKey   = 1;
inline constexpr AttrNumber kAttSubKey     = 2;
inline constexpr AttrNumber kAttChunkNo    = 3;
inline constexpr AttrNumber kAttChunkBytes = 4;
inline constexpr AttrNumber kAttBlob       = 5;

// Revision-3 lives in this module as an inactive, fail-closed surface until the
// destructive schema cutover is made atomically with every live writer/reader.
// The legacy five-column constants above deliberately remain unchanged.
inline constexpr AttrNumber kRev3AttQueryKey       = 1;
inline constexpr AttrNumber kRev3AttGenerationId   = 2;
inline constexpr AttrNumber kRev3AttPositionSegment = 3;
inline constexpr AttrNumber kRev3AttSubKey         = 4;
inline constexpr AttrNumber kRev3AttChunkNo        = 5;
inline constexpr AttrNumber kRev3AttChunkBytes     = 6;
inline constexpr AttrNumber kRev3AttChecksum       = 7;
inline constexpr AttrNumber kRev3AttBlob           = 8;

// systable_beginscan_ordered() accepts HEAP attribute numbers, then remaps
// each key through pg_index.indkey before handing the scan to the index AM.
// Keep the call sites in primary-key order while retaining the unchanged
// eight-column heap tuple's physical attribute numbers. Passing index
// ordinals here happens to look ordered to C++, but PostgreSQL remaps them a
// second time and gives btree an out-of-order key sequence.
inline constexpr AttrNumber kRev3IdxGenerationId =
    kRev3AttGenerationId;
inline constexpr AttrNumber kRev3IdxQueryKey =
    kRev3AttQueryKey;
inline constexpr AttrNumber kRev3IdxSubKey =
    kRev3AttSubKey;
inline constexpr AttrNumber kRev3IdxPositionSegment =
    kRev3AttPositionSegment;
inline constexpr AttrNumber kRev3IdxChunkNo =
    kRev3AttChunkNo;

struct Rev3IndexAttributeDescriptor {
    AttrNumber attno = InvalidAttrNumber;
    std::string_view name;
    Oid type_oid = InvalidOid;
};

inline constexpr std::array<Rev3IndexAttributeDescriptor, 8>
kRev3IndexAttributeDescriptors{{
    {kRev3AttQueryKey, "query_key", INT8OID},
    {kRev3AttGenerationId, "generation_id", INT8OID},
    {kRev3AttPositionSegment, "position_segment", INT4OID},
    {kRev3AttSubKey, "sub_key", INT8OID},
    {kRev3AttChunkNo, "chunk_no", INT4OID},
    {kRev3AttChunkBytes, "chunk_bytes", INT4OID},
    {kRev3AttChecksum, "checksum", INT8OID},
    {kRev3AttBlob, "blob", BYTEAOID},
}};

inline constexpr std::array<AttrNumber, 5> kRev3IndexPrimaryKeyAttributes{
    kRev3AttGenerationId,
    kRev3AttQueryKey,
    kRev3AttSubKey,
    kRev3AttPositionSegment,
    kRev3AttChunkNo,
};

static_assert(kRev3IndexAttributeDescriptors.front().attno == 1);
static_assert(kRev3IndexAttributeDescriptors.back().attno == 8);
static_assert(kRev3IndexPrimaryKeyAttributes.size() == 5);
static_assert(kRev3IdxGenerationId == kRev3IndexPrimaryKeyAttributes[0]);
static_assert(kRev3IdxQueryKey == kRev3IndexPrimaryKeyAttributes[1]);
static_assert(kRev3IdxSubKey == kRev3IndexPrimaryKeyAttributes[2]);
static_assert(kRev3IdxPositionSegment ==
    kRev3IndexPrimaryKeyAttributes[3]);
static_assert(kRev3IdxChunkNo == kRev3IndexPrimaryKeyAttributes[4]);

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

enum class Rev3IndexSchemaError : std::uint8_t {
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

struct Rev3IndexSchemaInspection {
    Rev3IndexSchemaError error = Rev3IndexSchemaError::none;
    Oid primary_index_oid = InvalidOid;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == Rev3IndexSchemaError::none
            && OidIsValid(primary_index_oid);
    }
};

[[noreturn]] inline void rev3_storage_error(std::string_view message);

[[nodiscard]] constexpr std::string_view rev3_index_schema_error_name(
        Rev3IndexSchemaError error) noexcept {
    switch (error) {
        case Rev3IndexSchemaError::none: return "none";
        case Rev3IndexSchemaError::not_heap: return "relation is not heap AM";
        case Rev3IndexSchemaError::attribute_count: return "attribute count";
        case Rev3IndexSchemaError::attribute_number: return "attribute number";
        case Rev3IndexSchemaError::attribute_dropped: return "dropped attribute";
        case Rev3IndexSchemaError::attribute_name: return "attribute name";
        case Rev3IndexSchemaError::attribute_type: return "attribute type";
        case Rev3IndexSchemaError::attribute_options: return "attribute options";
        case Rev3IndexSchemaError::attribute_nullable: return "nullable attribute";
        case Rev3IndexSchemaError::blob_not_plain: return "blob storage is not PLAIN";
        case Rev3IndexSchemaError::wrong_namespace: return "relation namespace";
        case Rev3IndexSchemaError::wrong_relation_name: return "relation name";
        case Rev3IndexSchemaError::wrong_heap_persistence: return "heap is not UNLOGGED";
        case Rev3IndexSchemaError::wrong_index_persistence: return "primary index is not UNLOGGED";
        case Rev3IndexSchemaError::wrong_heap_tablespace: return "heap tablespace";
        case Rev3IndexSchemaError::wrong_index_tablespace: return "primary index tablespace";
        case Rev3IndexSchemaError::wrong_heap_fillfactor: return "heap fillfactor";
        case Rev3IndexSchemaError::wrong_index_fillfactor: return "primary index fillfactor";
        case Rev3IndexSchemaError::primary_index_count: return "primary index count";
        case Rev3IndexSchemaError::primary_index_not_unique: return "primary index is not unique";
        case Rev3IndexSchemaError::primary_index_not_immediate: return "primary index is deferred";
        case Rev3IndexSchemaError::primary_index_not_ready: return "primary index is not ready/valid/live";
        case Rev3IndexSchemaError::primary_index_key_count: return "primary index key count";
        case Rev3IndexSchemaError::primary_index_key_order: return "primary index key order";
        case Rev3IndexSchemaError::primary_index_not_btree: return "primary index is not btree";
        case Rev3IndexSchemaError::primary_index_partial: return "primary index is partial";
        case Rev3IndexSchemaError::primary_index_expression: return "primary index has expressions";
        case Rev3IndexSchemaError::primary_index_opclass: return "primary index opclass";
        case Rev3IndexSchemaError::primary_index_collation: return "primary index collation";
        case Rev3IndexSchemaError::primary_index_options: return "primary index ordering/opclass options";
    }
    return "unknown";
}

struct Rev3IndexSchemaExpectation {
    Oid namespace_oid = InvalidOid;
    std::string_view relation_name{};
    Oid tablespace_oid = InvalidOid;
};

/// Inspect the complete revision-3 relation shape without accepting attno
/// shims, included PK columns, nullable fields, or a toastable blob.
[[nodiscard]] inline Rev3IndexSchemaInspection inspect_rev3_index_schema(
        Relation relation,
        ColdIndexPreparationStatsPg* cold_stats = nullptr,
        Rev3IndexSchemaExpectation expected = {},
        Oid permitted_auxiliary_index_oid = InvalidOid) {
    if (relation == nullptr || relation->rd_rel->relam != HEAP_TABLE_AM_OID
        || relation->rd_rel->relkind != RELKIND_RELATION) {
        return {.error = Rev3IndexSchemaError::not_heap};
    }
    if (OidIsValid(expected.namespace_oid)
        && RelationGetNamespace(relation) != expected.namespace_oid) {
        return {.error = Rev3IndexSchemaError::wrong_namespace};
    }
    if (!expected.relation_name.empty()
        && std::string_view{RelationGetRelationName(relation)}
            != expected.relation_name) {
        return {.error = Rev3IndexSchemaError::wrong_relation_name};
    }
    if (relation->rd_rel->relpersistence != RELPERSISTENCE_UNLOGGED) {
        return {.error = Rev3IndexSchemaError::wrong_heap_persistence};
    }
    if (RelationGetFillFactor(relation, HEAP_DEFAULT_FILLFACTOR) != 100) {
        return {.error = Rev3IndexSchemaError::wrong_heap_fillfactor};
    }
    if (OidIsValid(expected.tablespace_oid)
        && relation->rd_locator.spcOid != expected.tablespace_oid) {
        return {.error = Rev3IndexSchemaError::wrong_heap_tablespace};
    }
    TupleDesc tuple_desc = RelationGetDescr(relation);
    if (tuple_desc == nullptr
        || tuple_desc->natts
            != static_cast<int>(kRev3IndexAttributeDescriptors.size())) {
        return {.error = Rev3IndexSchemaError::attribute_count};
    }
    for (std::size_t index = 0;
         index < kRev3IndexAttributeDescriptors.size(); ++index) {
        const Rev3IndexAttributeDescriptor& expected =
            kRev3IndexAttributeDescriptors[index];
        Form_pg_attribute attribute = TupleDescAttr(
            tuple_desc, static_cast<int>(index));
        if (attribute->attnum != expected.attno) {
            return {.error = Rev3IndexSchemaError::attribute_number};
        }
        if (attribute->attisdropped) {
            return {.error = Rev3IndexSchemaError::attribute_dropped};
        }
        if (std::string_view{NameStr(attribute->attname)} != expected.name) {
            return {.error = Rev3IndexSchemaError::attribute_name};
        }
        if (attribute->atttypid != expected.type_oid) {
            return {.error = Rev3IndexSchemaError::attribute_type};
        }
        if (attribute->atttypmod != -1
            || OidIsValid(attribute->attcollation)
            || attribute->atthasdef
            || attribute->atthasmissing
            || attribute->attidentity != '\0'
            || attribute->attgenerated != '\0'
            || attribute->attcompression != '\0') {
            return {.error = Rev3IndexSchemaError::attribute_options};
        }
        if (!attribute->attnotnull) {
            return {.error = Rev3IndexSchemaError::attribute_nullable};
        }
        if (attribute->attstorage != TYPSTORAGE_PLAIN) {
            return {.error = Rev3IndexSchemaError::blob_not_plain};
        }
    }

    Oid primary_index_oid = InvalidOid;
    std::uint32_t index_count = 0;
    std::uint32_t primary_index_count = 0;
    Rev3IndexSchemaError primary_error = Rev3IndexSchemaError::none;
    List* index_list = RelationGetIndexList(relation);
    ListCell* cell = nullptr;
    foreach (cell, index_list) {
        const Oid index_oid = lfirst_oid(cell);
        // Production callers leave this invalid and retain the exact-one-index
        // contract.  The revision-3 catalog-corruption fixture alone names its
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
                primary_error = Rev3IndexSchemaError::primary_index_not_unique;
            } else if (index_form->indnullsnotdistinct) {
                primary_error = Rev3IndexSchemaError::primary_index_options;
            } else if (!index_form->indimmediate) {
                primary_error = Rev3IndexSchemaError::primary_index_not_immediate;
            } else if (!index_form->indisvalid || !index_form->indisready
                       || !index_form->indislive) {
                primary_error = Rev3IndexSchemaError::primary_index_not_ready;
            } else if (index_form->indnatts
                           != static_cast<std::int16_t>(
                               kRev3IndexPrimaryKeyAttributes.size())
                       || index_form->indnkeyatts
                           != static_cast<std::int16_t>(
                               kRev3IndexPrimaryKeyAttributes.size())) {
                primary_error = Rev3IndexSchemaError::primary_index_key_count;
            } else if (index_relation->rd_rel->relam != BTREE_AM_OID) {
                primary_error = Rev3IndexSchemaError::primary_index_not_btree;
            } else if (index_relation->rd_rel->relpersistence
                       != RELPERSISTENCE_UNLOGGED) {
                primary_error = Rev3IndexSchemaError::wrong_index_persistence;
            } else if (index_relation->rd_locator.spcOid
                       != relation->rd_locator.spcOid
                       || (OidIsValid(expected.tablespace_oid)
                           && index_relation->rd_locator.spcOid
                               != expected.tablespace_oid)) {
                primary_error = Rev3IndexSchemaError::wrong_index_tablespace;
            } else if (RelationGetFillFactor(index_relation,
                           BTREE_DEFAULT_FILLFACTOR) != 90) {
                primary_error = Rev3IndexSchemaError::wrong_index_fillfactor;
            } else if (RelationGetIndexPredicate(index_relation) != NIL) {
                primary_error = Rev3IndexSchemaError::primary_index_partial;
            } else if (RelationGetIndexExpressions(index_relation) != NIL) {
                primary_error = Rev3IndexSchemaError::primary_index_expression;
            } else {
                constexpr std::array<Oid, 5> expected_opclasses{
                    INT8_BTREE_OPS_OID,
                    INT8_BTREE_OPS_OID,
                    INT8_BTREE_OPS_OID,
                    INT4_BTREE_OPS_OID,
                    INT4_BTREE_OPS_OID,
                };
                for (std::size_t key = 0;
                     key < kRev3IndexPrimaryKeyAttributes.size(); ++key) {
                    if (index_form->indkey.values[key]
                        != kRev3IndexPrimaryKeyAttributes[key]) {
                        primary_error = Rev3IndexSchemaError::primary_index_key_order;
                        break;
                    }
                    if (get_index_column_opclass(
                            index_oid, static_cast<int>(key + 1u))
                        != expected_opclasses[key]) {
                        primary_error = Rev3IndexSchemaError::primary_index_opclass;
                        break;
                    }
                    if (index_relation->rd_indcollation == nullptr
                        || OidIsValid(index_relation->rd_indcollation[key])) {
                        primary_error = Rev3IndexSchemaError::primary_index_collation;
                        break;
                    }
                    if (index_relation->rd_indoption == nullptr
                        || index_relation->rd_indoption[key] != 0
                        || (index_relation->rd_opcoptions != nullptr
                            && index_relation->rd_opcoptions[key] != nullptr)) {
                        primary_error = Rev3IndexSchemaError::primary_index_options;
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
        return {.error = Rev3IndexSchemaError::primary_index_count};
    }
    if (primary_error != Rev3IndexSchemaError::none) {
        return {.error = primary_error};
    }
    return {
        .error = Rev3IndexSchemaError::none,
        .primary_index_oid = primary_index_oid,
    };
}

/// Validate a revision-3 heap and its already-open primary-index descriptor.
/// Unlike inspect_rev3_index_schema(), this routine performs no Relation open
/// or close.  It is the production cold-read validator: the exact descriptor
/// proven here is the descriptor subsequently used by every ordered scan.
[[nodiscard]] inline Rev3IndexSchemaInspection
inspect_bound_rev3_index_schema(
        Relation relation,
        Relation primary_index_relation,
        Rev3IndexSchemaExpectation expected = {}) {
    if (relation == nullptr || relation->rd_rel->relam != HEAP_TABLE_AM_OID
        || relation->rd_rel->relkind != RELKIND_RELATION) {
        return {.error = Rev3IndexSchemaError::not_heap};
    }
    if (OidIsValid(expected.namespace_oid)
        && RelationGetNamespace(relation) != expected.namespace_oid) {
        return {.error = Rev3IndexSchemaError::wrong_namespace};
    }
    if (!expected.relation_name.empty()
        && std::string_view{RelationGetRelationName(relation)}
            != expected.relation_name) {
        return {.error = Rev3IndexSchemaError::wrong_relation_name};
    }
    if (relation->rd_rel->relpersistence != RELPERSISTENCE_UNLOGGED) {
        return {.error = Rev3IndexSchemaError::wrong_heap_persistence};
    }
    if (RelationGetFillFactor(relation, HEAP_DEFAULT_FILLFACTOR) != 100) {
        return {.error = Rev3IndexSchemaError::wrong_heap_fillfactor};
    }
    if (OidIsValid(expected.tablespace_oid)
        && relation->rd_locator.spcOid != expected.tablespace_oid) {
        return {.error = Rev3IndexSchemaError::wrong_heap_tablespace};
    }
    TupleDesc tuple_desc = RelationGetDescr(relation);
    if (tuple_desc == nullptr
        || tuple_desc->natts
            != static_cast<int>(kRev3IndexAttributeDescriptors.size())) {
        return {.error = Rev3IndexSchemaError::attribute_count};
    }
    for (std::size_t index = 0;
         index < kRev3IndexAttributeDescriptors.size(); ++index) {
        const Rev3IndexAttributeDescriptor& expected =
            kRev3IndexAttributeDescriptors[index];
        Form_pg_attribute attribute = TupleDescAttr(
            tuple_desc, static_cast<int>(index));
        if (attribute->attnum != expected.attno) {
            return {.error = Rev3IndexSchemaError::attribute_number};
        }
        if (attribute->attisdropped) {
            return {.error = Rev3IndexSchemaError::attribute_dropped};
        }
        if (std::string_view{NameStr(attribute->attname)} != expected.name) {
            return {.error = Rev3IndexSchemaError::attribute_name};
        }
        if (attribute->atttypid != expected.type_oid) {
            return {.error = Rev3IndexSchemaError::attribute_type};
        }
        if (attribute->atttypmod != -1
            || OidIsValid(attribute->attcollation)
            || attribute->atthasdef
            || attribute->atthasmissing
            || attribute->attidentity != '\0'
            || attribute->attgenerated != '\0'
            || attribute->attcompression != '\0') {
            return {.error = Rev3IndexSchemaError::attribute_options};
        }
        if (!attribute->attnotnull) {
            return {.error = Rev3IndexSchemaError::attribute_nullable};
        }
        if (attribute->attstorage != TYPSTORAGE_PLAIN) {
            return {.error = Rev3IndexSchemaError::blob_not_plain};
        }
    }

    List* index_list = RelationGetIndexList(relation);
    const bool exactly_one_index = list_length(index_list) == 1;
    list_free(index_list);
    if (!exactly_one_index || primary_index_relation == nullptr
        || primary_index_relation->rd_index == nullptr) {
        return {.error = Rev3IndexSchemaError::primary_index_count};
    }
    const bool exact_primary = RelationGetPrimaryKeyIndex(relation)
        == RelationGetRelid(primary_index_relation);
    if (!exact_primary
        || primary_index_relation->rd_index->indrelid
            != RelationGetRelid(relation)
        || !primary_index_relation->rd_index->indisprimary) {
        return {.error = Rev3IndexSchemaError::primary_index_count};
    }

    const Form_pg_index index_form = primary_index_relation->rd_index;
    if (!index_form->indisunique) {
        return {.error = Rev3IndexSchemaError::primary_index_not_unique};
    }
    if (index_form->indnullsnotdistinct) {
        return {.error = Rev3IndexSchemaError::primary_index_options};
    }
    if (!index_form->indimmediate) {
        return {.error = Rev3IndexSchemaError::primary_index_not_immediate};
    }
    if (!index_form->indisvalid || !index_form->indisready
        || !index_form->indislive) {
        return {.error = Rev3IndexSchemaError::primary_index_not_ready};
    }
    if (index_form->indnatts
            != static_cast<std::int16_t>(
                kRev3IndexPrimaryKeyAttributes.size())
        || index_form->indnkeyatts
            != static_cast<std::int16_t>(
                kRev3IndexPrimaryKeyAttributes.size())) {
        return {.error = Rev3IndexSchemaError::primary_index_key_count};
    }
    if (primary_index_relation->rd_rel->relam != BTREE_AM_OID) {
        return {.error = Rev3IndexSchemaError::primary_index_not_btree};
    }
    if (primary_index_relation->rd_rel->relpersistence
        != RELPERSISTENCE_UNLOGGED) {
        return {.error = Rev3IndexSchemaError::wrong_index_persistence};
    }
    if (primary_index_relation->rd_locator.spcOid
            != relation->rd_locator.spcOid
        || (OidIsValid(expected.tablespace_oid)
            && primary_index_relation->rd_locator.spcOid
                != expected.tablespace_oid)) {
        return {.error = Rev3IndexSchemaError::wrong_index_tablespace};
    }
    if (RelationGetFillFactor(primary_index_relation,
            BTREE_DEFAULT_FILLFACTOR) != 90) {
        return {.error = Rev3IndexSchemaError::wrong_index_fillfactor};
    }
    if (RelationGetIndexPredicate(primary_index_relation) != NIL) {
        return {.error = Rev3IndexSchemaError::primary_index_partial};
    }
    if (RelationGetIndexExpressions(primary_index_relation) != NIL) {
        return {.error = Rev3IndexSchemaError::primary_index_expression};
    }
    constexpr std::array<Oid, 5> expected_opclasses{
        INT8_BTREE_OPS_OID,
        INT8_BTREE_OPS_OID,
        INT8_BTREE_OPS_OID,
        INT4_BTREE_OPS_OID,
        INT4_BTREE_OPS_OID,
    };
    const Oid primary_index_oid = RelationGetRelid(primary_index_relation);
    for (std::size_t key = 0;
         key < kRev3IndexPrimaryKeyAttributes.size(); ++key) {
        if (index_form->indkey.values[key]
            != kRev3IndexPrimaryKeyAttributes[key]) {
            return {.error = Rev3IndexSchemaError::primary_index_key_order};
        }
        if (get_index_column_opclass(
                primary_index_oid, static_cast<int>(key + 1u))
            != expected_opclasses[key]) {
            return {.error = Rev3IndexSchemaError::primary_index_opclass};
        }
        if (primary_index_relation->rd_indcollation == nullptr
            || OidIsValid(primary_index_relation->rd_indcollation[key])) {
            return {.error = Rev3IndexSchemaError::primary_index_collation};
        }
        if (primary_index_relation->rd_indoption == nullptr
            || primary_index_relation->rd_indoption[key] != 0
            || (primary_index_relation->rd_opcoptions != nullptr
                && primary_index_relation->rd_opcoptions[key] != nullptr)) {
            return {.error = Rev3IndexSchemaError::primary_index_options};
        }
    }
    return {
        .error = Rev3IndexSchemaError::none,
        .primary_index_oid = primary_index_oid,
    };
}

enum class MinimizerGenerationRelationKind : std::uint8_t {
    missing,
    legacy_exact,
    revision3_exact,
    malformed,
};

struct MinimizerGenerationRelationInspection {
    MinimizerGenerationRelationKind kind =
        MinimizerGenerationRelationKind::malformed;
    Rev3IndexSchemaError revision3_error =
        Rev3IndexSchemaError::none;
};

/// Non-PostgreSQL control-flow result used only while binding a production
/// read session.  The binder closes every normally-opened descriptor before
/// throwing it; PostgreSQL errors remain PostgresErrorAsCpp and are left to the
/// ResourceOwner/transaction abort path.
struct MinimizerGenerationRelationOpenFailurePg {
    MinimizerGenerationRelationInspection inspection{};
};

/// Recognise only the two legacy schemas that the old PostgreSQL writers
/// actually created.  Attribute count alone is intentionally insufficient:
/// ordinal 2 must not describe an arbitrary lookalike or damaged relation as
/// a valid legacy index.
[[nodiscard]] inline bool inspect_exact_legacy_minimizer_index_schema(
        Relation relation,
        std::string_view index_name) {
    constexpr std::array<Rev3IndexAttributeDescriptor, 5>
        mpf_attributes{{
            {1, "query_key", INT8OID},
            {2, "sub_key", INT8OID},
            {3, "chunk_no", INT4OID},
            {4, "chunk_bytes", INT4OID},
            {5, "blob", BYTEAOID},
        }};
    constexpr std::array<AttrNumber, 3> mpf_primary_key{1, 2, 3};
    constexpr std::array<Oid, 3> mpf_opclasses{
        INT8_BTREE_OPS_OID,
        INT8_BTREE_OPS_OID,
        INT4_BTREE_OPS_OID,
    };
    constexpr std::array<Rev3IndexAttributeDescriptor, 3>
        mps_attributes{{
            {1, "query_key", INT8OID},
            {2, "chunk_bytes", INT4OID},
            {3, "blob", BYTEAOID},
        }};
    constexpr std::array<AttrNumber, 1> mps_primary_key{1};
    constexpr std::array<Oid, 1> mps_opclasses{INT8_BTREE_OPS_OID};

    std::span<const Rev3IndexAttributeDescriptor> attributes;
    std::span<const AttrNumber> primary_key;
    std::span<const Oid> opclasses;
    if (index_name == MINIMIZERS_PER_FRAGMENT_INDEX_NAME) {
        attributes = mpf_attributes;
        primary_key = mpf_primary_key;
        opclasses = mpf_opclasses;
    } else if (index_name == MINIMIZERS_PER_SEQUENCE_INDEX_NAME) {
        attributes = mps_attributes;
        primary_key = mps_primary_key;
        opclasses = mps_opclasses;
    } else {
        return false;
    }

    if (relation == nullptr || relation->rd_rel->relam != HEAP_TABLE_AM_OID) {
        return false;
    }
    TupleDesc tuple_desc = RelationGetDescr(relation);
    if (tuple_desc == nullptr
        || tuple_desc->natts != static_cast<int>(attributes.size())) {
        return false;
    }
    for (std::size_t index = 0; index < attributes.size(); ++index) {
        const Rev3IndexAttributeDescriptor& expected = attributes[index];
        Form_pg_attribute attribute = TupleDescAttr(
            tuple_desc, static_cast<int>(index));
        if (attribute->attnum != expected.attno
            || attribute->attisdropped
            || std::string_view{NameStr(attribute->attname)} != expected.name
            || attribute->atttypid != expected.type_oid
            || !attribute->attnotnull) {
            return false;
        }
        if (index + 1u == attributes.size()
            && attribute->attstorage != TYPSTORAGE_EXTERNAL) {
            return false;
        }
    }

    std::uint32_t primary_index_count = 0;
    bool primary_index_exact = false;
    List* index_list = RelationGetIndexList(relation);
    ListCell* cell = nullptr;
    foreach (cell, index_list) {
        const Oid index_oid = lfirst_oid(cell);
        Relation index_relation = index_open(index_oid, AccessShareLock);
        const Form_pg_index index_form = index_relation->rd_index;
        if (index_form != nullptr && index_form->indisprimary) {
            ++primary_index_count;
            primary_index_exact =
                index_form->indisunique
                && index_form->indimmediate
                && index_form->indisvalid
                && index_form->indisready
                && index_form->indislive
                && index_form->indnatts
                    == static_cast<std::int16_t>(primary_key.size())
                && index_form->indnkeyatts
                    == static_cast<std::int16_t>(primary_key.size())
                && index_relation->rd_rel->relam == BTREE_AM_OID
                && RelationGetIndexPredicate(index_relation) == NIL
                && RelationGetIndexExpressions(index_relation) == NIL;
            for (std::size_t key = 0;
                 primary_index_exact && key < primary_key.size(); ++key) {
                primary_index_exact =
                    index_form->indkey.values[key] == primary_key[key]
                    && get_index_column_opclass(
                        index_oid, static_cast<int>(key + 1u))
                        == opclasses[key]
                    && index_relation->rd_indcollation != nullptr
                    && !OidIsValid(index_relation->rd_indcollation[key])
                    && index_relation->rd_indoption != nullptr
                    && index_relation->rd_indoption[key] == 0
                    && (index_relation->rd_opcoptions == nullptr
                        || index_relation->rd_opcoptions[key] == nullptr);
            }
        }
        index_close(index_relation, AccessShareLock);
    }
    list_free(index_list);
    return primary_index_count == 1 && primary_index_exact;
}

/// Legacy classifier for the one-bound-descriptor production path.  It
/// recognises the historical schema without opening another index relation.
[[nodiscard]] inline bool inspect_bound_exact_legacy_minimizer_index_schema(
        Relation relation,
        Relation primary_index_relation,
        std::string_view index_name) {
    constexpr std::array<Rev3IndexAttributeDescriptor, 5>
        mpf_attributes{{
            {1, "query_key", INT8OID},
            {2, "sub_key", INT8OID},
            {3, "chunk_no", INT4OID},
            {4, "chunk_bytes", INT4OID},
            {5, "blob", BYTEAOID},
        }};
    constexpr std::array<AttrNumber, 3> mpf_primary_key{1, 2, 3};
    constexpr std::array<Oid, 3> mpf_opclasses{
        INT8_BTREE_OPS_OID,
        INT8_BTREE_OPS_OID,
        INT4_BTREE_OPS_OID,
    };
    constexpr std::array<Rev3IndexAttributeDescriptor, 3>
        mps_attributes{{
            {1, "query_key", INT8OID},
            {2, "chunk_bytes", INT4OID},
            {3, "blob", BYTEAOID},
        }};
    constexpr std::array<AttrNumber, 1> mps_primary_key{1};
    constexpr std::array<Oid, 1> mps_opclasses{INT8_BTREE_OPS_OID};

    std::span<const Rev3IndexAttributeDescriptor> attributes;
    std::span<const AttrNumber> primary_key;
    std::span<const Oid> opclasses;
    if (index_name == MINIMIZERS_PER_FRAGMENT_INDEX_NAME) {
        attributes = mpf_attributes;
        primary_key = mpf_primary_key;
        opclasses = mpf_opclasses;
    } else if (index_name == MINIMIZERS_PER_SEQUENCE_INDEX_NAME) {
        attributes = mps_attributes;
        primary_key = mps_primary_key;
        opclasses = mps_opclasses;
    } else {
        return false;
    }
    if (relation == nullptr || primary_index_relation == nullptr
        || relation->rd_rel->relam != HEAP_TABLE_AM_OID
        || primary_index_relation->rd_index == nullptr
        || primary_index_relation->rd_index->indrelid
            != RelationGetRelid(relation)) {
        return false;
    }
    TupleDesc tuple_desc = RelationGetDescr(relation);
    if (tuple_desc == nullptr
        || tuple_desc->natts != static_cast<int>(attributes.size())) {
        return false;
    }
    for (std::size_t index = 0; index < attributes.size(); ++index) {
        const Rev3IndexAttributeDescriptor& expected = attributes[index];
        Form_pg_attribute attribute = TupleDescAttr(
            tuple_desc, static_cast<int>(index));
        if (attribute->attnum != expected.attno
            || attribute->attisdropped
            || std::string_view{NameStr(attribute->attname)} != expected.name
            || attribute->atttypid != expected.type_oid
            || !attribute->attnotnull
            || (index + 1u == attributes.size()
                && attribute->attstorage != TYPSTORAGE_EXTERNAL)) {
            return false;
        }
    }
    const bool exact_primary = RelationGetPrimaryKeyIndex(relation)
        == RelationGetRelid(primary_index_relation);
    const Form_pg_index index_form = primary_index_relation->rd_index;
    bool exact = exact_primary
        && index_form->indisprimary
        && index_form->indisunique
        && index_form->indimmediate
        && index_form->indisvalid
        && index_form->indisready
        && index_form->indislive
        && index_form->indnatts
            == static_cast<std::int16_t>(primary_key.size())
        && index_form->indnkeyatts
            == static_cast<std::int16_t>(primary_key.size())
        && primary_index_relation->rd_rel->relam == BTREE_AM_OID
        && RelationGetIndexPredicate(primary_index_relation) == NIL
        && RelationGetIndexExpressions(primary_index_relation) == NIL;
    const Oid index_oid = RelationGetRelid(primary_index_relation);
    for (std::size_t key = 0; exact && key < primary_key.size(); ++key) {
        exact = index_form->indkey.values[key] == primary_key[key]
            && get_index_column_opclass(
                index_oid, static_cast<int>(key + 1u)) == opclasses[key]
            && primary_index_relation->rd_indcollation != nullptr
            && !OidIsValid(primary_index_relation->rd_indcollation[key])
            && primary_index_relation->rd_indoption != nullptr
            && primary_index_relation->rd_indoption[key] == 0
            && (primary_index_relation->rd_opcoptions == nullptr
                || primary_index_relation->rd_opcoptions[key] == nullptr);
    }
    return exact;
}

/// Integration-only catalog corruption seam. It copies a real parsed
/// predicate or expression tree from a donor index onto the otherwise-valid
/// primary index row. Tests execute it inside a transaction and roll back,
/// proving PG17's varlena `indpred`/`indexprs` path rather than fabricating a
/// C++ Relation object.
inline void rev3_test_copy_primary_index_shape_pg(
        Oid table_oid,
        Oid donor_index_oid,
        bool copy_predicate) {
    if (!superuser()) {
        rev3_storage_error("rev3 index-shape corruption requires superuser");
    }
    Relation target = try_safe_relation_open(table_oid, AccessShareLock);
    if (target == nullptr) {
        rev3_storage_error("rev3 index-shape target relation is missing");
    }
    const Rev3IndexSchemaInspection schema = inspect_rev3_index_schema(
        target, nullptr, {}, donor_index_oid);
    relation_close(target, AccessShareLock);
    if (!schema) {
        rev3_storage_error("rev3 index-shape target schema is not initially valid");
    }
    Relation donor = try_safe_relation_open(donor_index_oid, AccessShareLock);
    if (donor == nullptr || donor->rd_index == nullptr
        || donor->rd_index->indrelid != table_oid
        || (copy_predicate
            ? RelationGetIndexPredicate(donor) == NIL
            : RelationGetIndexExpressions(donor) == NIL)) {
        if (donor != nullptr) relation_close(donor, AccessShareLock);
        rev3_storage_error("rev3 index-shape donor does not carry the requested shape");
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
        rev3_storage_error("rev3 index-shape catalog tuple is missing");
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
        rev3_storage_error("rev3 index-shape donor catalog value is NULL");
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
    if (name == MINIMIZERS_PER_FRAGMENT_INDEX_NAME) return "mpf";
    if (name == MINIMIZERS_PER_SEQUENCE_INDEX_NAME) return "mps";
    if (name == SYMBOL_COUNTS_INDEX_NAME)           return "sc";
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
    bioseq::pg_utils::invoke_pg_as_cpp(
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
    return bioseq::pg_utils::invoke_pg_as_cpp(
        [] { return bioseq_internal_ns_pg_leaf(); });
}

/// Run a DDL string inside an internal sub-transaction (matches
/// sequence_storage_manager's lazy-CREATE pattern). Returns nothing; throws on error.
inline void run_internal_ddl(const char* ddl) {
    MemoryContext saved_ctx   = CurrentMemoryContext;
    ResourceOwner saved_owner = CurrentResourceOwner;
    BeginInternalSubTransaction("bioseq index DDL");
    MemoryContextSwitchTo(saved_ctx);
    PG_TRY();
    {
        if (SPI_connect() != SPI_OK_CONNECT) bioseq_log<ERR, PG>( "bioseq index: SPI_connect failed");
        const int rc = SPI_execute(ddl, /*read_only=*/false, /*tcount=*/0);
        if (rc != SPI_OK_UTILITY && rc != SPI_OK_SELECT)
            bioseq_log<ERR, PG>( "bioseq index: DDL failed (SPI rc={})", rc);
        SPI_finish();
        ReleaseCurrentSubTransaction();
        MemoryContextSwitchTo(saved_ctx);
        CurrentResourceOwner = saved_owner;
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(saved_ctx);
        RollbackAndReleaseCurrentSubTransaction();
        MemoryContextSwitchTo(saved_ctx);
        CurrentResourceOwner = saved_owner;
        PG_RE_THROW();
    }
    PG_END_TRY();
}

/// Persistence keyword inherited from the user relation (UNLOGGED if parent is).
[[nodiscard]] inline const char* persistence_kw_for(Oid user_relid) {
    char persist = 'p';
    Relation r = try_safe_relation_open(user_relid, AccessShareLock);
    if (r != nullptr) { persist = r->rd_rel->relpersistence; relation_close(r, AccessShareLock); }
    return persist == 'u' ? "UNLOGGED" : "";
}

[[noreturn]] inline void rev3_storage_error_pg_leaf(
        std::string_view message) {
    ereport(ERROR,
        (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
         errmsg("bioseq revision-3 index: %.*s",
            static_cast<int>(message.size()), message.data())));
    pg_unreachable();
}

[[noreturn]] inline void rev3_storage_error_as_cpp(
        std::string_view message) {
    bioseq::pg_utils::invoke_pg_as_cpp(
        [message] { rev3_storage_error_pg_leaf(message); });
    pg_unreachable();
}

[[noreturn]] inline void rev3_storage_error(std::string_view message) {
    rev3_storage_error_as_cpp(message);
    pg_unreachable();
}

/// Stable administration-lock key shared by the legacy administrator and the
/// future revision-3 reset/publication path. The hash is intentionally byte-for-
/// byte identical to the pre-existing admin helper.
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
    DirectFunctionCall1(pg_advisory_xact_lock_int8, Int64GetDatum(signed_key));
}

inline constexpr std::string_view kMinimizerGenerationSequenceName =
    "minimizer_generation_id_seq";

enum class MinimizerGenerationSequenceError : std::uint8_t {
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
        MinimizerGenerationSequenceError error) noexcept {
    switch (error) {
        case MinimizerGenerationSequenceError::none: return "none";
        case MinimizerGenerationSequenceError::missing: return "missing";
        case MinimizerGenerationSequenceError::wrong_namespace: return "wrong namespace";
        case MinimizerGenerationSequenceError::wrong_relkind: return "not a sequence";
        case MinimizerGenerationSequenceError::wrong_persistence: return "not permanent";
        case MinimizerGenerationSequenceError::missing_catalog_row: return "missing pg_sequence row";
        case MinimizerGenerationSequenceError::wrong_type: return "not BIGINT";
        case MinimizerGenerationSequenceError::wrong_bounds: return "wrong positive non-wrapping bounds";
        case MinimizerGenerationSequenceError::cycling: return "cycles";
    }
    return "unknown";
}

struct MinimizerGenerationSequenceInspection {
    MinimizerGenerationSequenceError error =
        MinimizerGenerationSequenceError::none;
    Oid sequence_oid = InvalidOid;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == MinimizerGenerationSequenceError::none
            && OidIsValid(sequence_oid);
    }
};

/// Validate the SQL installation object's exact non-wrapping BIGINT contract.
[[nodiscard]] inline MinimizerGenerationSequenceInspection
inspect_minimizer_generation_sequence_contract() {
    const Oid internal_namespace = bioseq::pg_utils::invoke_pg_as_cpp(
        [] { return bioseq_internal_ns(); });
    const Oid sequence_oid = bioseq::pg_utils::invoke_pg_as_cpp(
        [internal_namespace] {
            return get_relname_relid(
                kMinimizerGenerationSequenceName.data(),
                internal_namespace);
        });
    if (!OidIsValid(sequence_oid)) {
        return {.error = MinimizerGenerationSequenceError::missing};
    }
    Relation sequence_relation = bioseq::pg_utils::invoke_pg_as_cpp(
        [sequence_oid] {
            return try_safe_relation_open(
                sequence_oid, AccessShareLock);
        });
    if (sequence_relation == nullptr) {
        return {.error = MinimizerGenerationSequenceError::missing};
    }
    MinimizerGenerationSequenceError error =
        MinimizerGenerationSequenceError::none;
    if (RelationGetNamespace(sequence_relation) != internal_namespace) {
        error = MinimizerGenerationSequenceError::wrong_namespace;
    } else if (sequence_relation->rd_rel->relkind != RELKIND_SEQUENCE) {
        error = MinimizerGenerationSequenceError::wrong_relkind;
    } else if (sequence_relation->rd_rel->relpersistence
               != RELPERSISTENCE_PERMANENT) {
        error = MinimizerGenerationSequenceError::wrong_persistence;
    }
    bioseq::pg_utils::invoke_pg_as_cpp(
        [sequence_relation] {
            relation_close(sequence_relation, AccessShareLock);
        });
    if (error != MinimizerGenerationSequenceError::none) {
        return {.error = error};
    }
    HeapTuple sequence_tuple = bioseq::pg_utils::invoke_pg_as_cpp(
        [sequence_oid] {
            return SearchSysCache1(
                SEQRELID, ObjectIdGetDatum(sequence_oid));
        });
    if (!HeapTupleIsValid(sequence_tuple)) {
        return {.error =
            MinimizerGenerationSequenceError::missing_catalog_row};
    }
    const Form_pg_sequence sequence =
        reinterpret_cast<Form_pg_sequence>(GETSTRUCT(sequence_tuple));
    if (sequence->seqtypid != INT8OID) {
        error = MinimizerGenerationSequenceError::wrong_type;
    } else if (sequence->seqincrement != 1
               || sequence->seqmin != 1
               || sequence->seqmax
                    != (std::numeric_limits<std::int64_t>::max)()
               || sequence->seqstart != 1
               || sequence->seqcache != 1) {
        error = MinimizerGenerationSequenceError::wrong_bounds;
    } else if (sequence->seqcycle) {
        error = MinimizerGenerationSequenceError::cycling;
    }
    bioseq::pg_utils::invoke_pg_as_cpp(
        [sequence_tuple] { ReleaseSysCache(sequence_tuple); });
    return {.error = error, .sequence_oid =
        error == MinimizerGenerationSequenceError::none
            ? sequence_oid : InvalidOid};
}

/// Allocate a positive signed-SQL generation ID. This helper never creates the
/// sequence; installation/cutover owns that atomic DDL step.
[[nodiscard]] inline std::uint64_t allocate_minimizer_generation_id_pg() {
    const MinimizerGenerationSequenceInspection sequence =
        inspect_minimizer_generation_sequence_contract();
    if (!sequence) {
        rev3_storage_error_as_cpp(fmt::format(
            "owned generation sequence contract mismatch: {}",
            minimizer_generation_sequence_error_name(sequence.error)));
    }
    const std::int64_t generation_id = bioseq::pg_utils::invoke_pg_as_cpp(
        [&sequence] {
            return nextval_internal(sequence.sequence_oid, false);
        });
    if (generation_id <= 0) {
        rev3_storage_error_as_cpp(
            "generation sequence exhausted its positive BIGINT range");
    }
    return static_cast<std::uint64_t>(generation_id);
}

/// Validate one revision-3 address and its row checksum before a PostgreSQL
/// tuple is formed. The active-head row is never accepted by the immutable
/// generation sink and has a separate atomic publication helper below.
struct Rev3GenerationRowValidator;

class ValidatedRev3GenerationRow {
private:
    MinimizerGenerationRowView row_{};

    explicit ValidatedRev3GenerationRow(
            const MinimizerGenerationRowView& row) noexcept
        : row_(row) {}

    friend struct Rev3GenerationRowValidator;

public:
    ValidatedRev3GenerationRow() = delete;

    [[nodiscard]] const MinimizerGenerationRowView& row() const noexcept {
        return row_;
    }
};

/// Sole validation and construction authority. Defining this after the token is
/// complete avoids instantiating std::expected<ValidatedRev3GenerationRow, ...>
/// while preserving the invariant that no caller can construct an unchecked
/// token.
struct Rev3GenerationRowValidator {
private:
    [[nodiscard]] static std::expected<
        void, MinimizerGenerationWireError>
    validate_shape(
            std::uint64_t expected_generation_id,
            const IndexChunkAddress& address,
            std::span<const std::byte> bytes) noexcept {
        if (expected_generation_id == 0
            || !bioseq::indexing::sql_bigint_can_represent(
                expected_generation_id)
            || address.generation_id != expected_generation_id) {
            return std::unexpected(
                MinimizerGenerationWireError::generation_id);
        }
        if (bytes.empty()
            || bytes.size()
                > static_cast<std::size_t>(
                    (std::numeric_limits<std::int32_t>::max)())
            || !bioseq::indexing::sql_integer_can_represent(
                address.chunk_no)) {
            return std::unexpected(
                MinimizerGenerationWireError::byte_count);
        }
        if (address.position_segment == -1) {
            if (address.sub_key != DIRECTORY_SUB_KEY) {
                return std::unexpected(
                    MinimizerGenerationWireError::sub_key);
            }
            if ((address.chunk_no == 0
                    && bytes.size()
                        != MinimizerGenerationManifest::encoded_bytes
                    && bytes.size()
                        != MinimizerGenerationManifest::format5_encoded_bytes)
                || (address.chunk_no != 0
                    && bytes.size()
                        > bioseq::indexing::
                            kMinimizerFormat5ExtentPayloadBytes)) {
                return std::unexpected(
                    MinimizerGenerationWireError::byte_count);
            }
        } else if (address.position_segment < 0) {
            return std::unexpected(
                MinimizerGenerationWireError::position_segment);
        } else if (address.sub_key < 0
                   || address.sub_key >= TEMP_RUN_BASE
                   || bytes.size()
                        > bioseq::indexing::
                            kMinimizerFormat5ExtentPayloadBytes) {
            return std::unexpected(MinimizerGenerationWireError::sub_key);
        }
        return {};
    }

public:
    [[nodiscard]] static std::expected<
        ValidatedRev3GenerationRow, MinimizerGenerationWireError>
    validate(
            std::uint64_t expected_generation_id,
            const MinimizerGenerationRowView& row) noexcept {
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
            return std::unexpected(MinimizerGenerationWireError::checksum);
        }
        return ValidatedRev3GenerationRow{row};
    }

    /// Compute and bind the sole Format-5/S1 payload-row CRC. The caller does
    /// not provide a checksum that would require a second verification pass.
    [[nodiscard]] static std::expected<
        ValidatedRev3GenerationRow, MinimizerGenerationWireError>
    validate_and_checksum(
            std::uint64_t expected_generation_id,
            const UnchecksummedMinimizerGenerationRowView& row) noexcept {
        if (row.address.position_segment < 0) {
            return std::unexpected(MinimizerGenerationWireError::row_order);
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
        return ValidatedRev3GenerationRow{
            MinimizerGenerationRowView{
                .address = row.address,
                .bytes = row.bytes,
                .row_checksum = checksum,
            }};
    }
};

[[nodiscard]] inline std::expected<
    ValidatedRev3GenerationRow, MinimizerGenerationWireError>
validate_rev3_generation_row_for_write(
        std::uint64_t expected_generation_id,
        const MinimizerGenerationRowView& row) noexcept {
    return Rev3GenerationRowValidator::validate(
        expected_generation_id, row);
}

struct Rev3IndexAdministrationIdentity {
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

/// Immutable identity of the exact visible source row used by revision-3.
/// `compatibility_checksum` is filled only after the independently decoded
/// manifest has been checked against the source row and active compatibility
/// axes; no head/manifest digest is copied into this object.
struct Rev3AuthoritativeSourceLocator {
    std::uint64_t sequence_rowno = 0;
    seq_locator locator{};
    std::uint8_t alphabet_code = 0;
    std::uint8_t storage_unit_byte_width = 0;
    std::uint16_t padding_letters = 0;
    std::uint64_t compatibility_checksum = 0;

    [[nodiscard]] bool operator==(
            const Rev3AuthoritativeSourceLocator& other) const noexcept {
        return sequence_rowno == other.sequence_rowno
            && locator.version == other.locator.version
            && locator.bucket_idx == other.locator.bucket_idx
            && locator._pad_2 == other.locator._pad_2
            && locator.shard_idx == other.locator.shard_idx
            && locator.num_fragments == other.locator.num_fragments
            && locator._pad_4 == other.locator._pad_4
            && locator.total_bp_size == other.locator.total_bp_size
            && locator.first_sub_row_no == other.locator.first_sub_row_no
            && locator.source_epoch == other.locator.source_epoch
            && alphabet_code == other.alphabet_code
            && storage_unit_byte_width == other.storage_unit_byte_width
            && padding_letters == other.padding_letters
            && compatibility_checksum == other.compatibility_checksum;
    }
};

[[nodiscard]] constexpr bool rev3_same_seq_locator(
        const seq_locator& left,
        const seq_locator& right) noexcept {
    return left.version == right.version
        && left.bucket_idx == right.bucket_idx
        && left._pad_2 == right._pad_2
        && left.shard_idx == right.shard_idx
        && left.num_fragments == right.num_fragments
        && left._pad_4 == right._pad_4
        && left.total_bp_size == right.total_bp_size
        && left.first_sub_row_no == right.first_sub_row_no
        && left.source_epoch == right.source_epoch;
}

/// Canonical locator-derived identity. Base content is deliberately a second,
/// independently streamed proof so a same-length mutation cannot be hidden by
/// a self-consistent rewrite of the head and manifest.
[[nodiscard]] constexpr std::uint64_t rev3_source_identity(
        Oid /*main_relfilenode*/,
        const Rev3AuthoritativeSourceLocator& source) noexcept {
    return source.locator.source_epoch;
}

class IndexStorageFragmentGenerationRowSinkPg;
class IndexStorageFragmentGenerationReadSessionPg;
class IndexStorageMinimizerFormat6WriterPg;
class IndexStorageMinimizerFormat6ReadSessionPg;

/// Unforgeable PostgreSQL-owned proof derived from the exact visible main-row
/// datum and, for a locator stub, every referenced fragment row under one
/// registered MVCC snapshot. Only the derivation helper can construct it. The
/// relation/query/snapshot binding prevents replay into another read session;
/// consumption is move-only and invalidates the token.
class Rev3AuthoritativeSourceProof {
private:
    Rev3AuthoritativeSourceLocator source_{};
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

    Rev3AuthoritativeSourceProof() = default;

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

    friend Rev3AuthoritativeSourceProof
    derive_rev3_authoritative_source_proof_pg(
        const Rev3IndexAdministrationIdentity&,
        std::uint64_t,
        Snapshot);
    friend class IndexStorageFragmentGenerationRowSinkPg;
    friend class IndexStorageFragmentGenerationReadSessionPg;
    friend class IndexStorageMinimizerFormat6WriterPg;
    friend class IndexStorageMinimizerFormat6ReadSessionPg;

public:
    Rev3AuthoritativeSourceProof(
        const Rev3AuthoritativeSourceProof&) = delete;
    Rev3AuthoritativeSourceProof& operator=(
        const Rev3AuthoritativeSourceProof&) = delete;
    Rev3AuthoritativeSourceProof(
            Rev3AuthoritativeSourceProof&& other) noexcept
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
    Rev3AuthoritativeSourceProof& operator=(
            Rev3AuthoritativeSourceProof&& other) noexcept {
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
    ~Rev3AuthoritativeSourceProof() = default;

    [[nodiscard]] const Rev3AuthoritativeSourceLocator&
    source_for_diagnostics() const noexcept {
        return source_;
    }

    [[nodiscard]] std::uint64_t
    source_content_checksum_for_diagnostics() const noexcept {
        return source_content_checksum_;
    }
};

static_assert(!std::is_default_constructible_v<
    Rev3AuthoritativeSourceProof>);
static_assert(!std::is_copy_constructible_v<
    Rev3AuthoritativeSourceProof>);
static_assert(std::is_nothrow_move_constructible_v<
    Rev3AuthoritativeSourceProof>);

[[nodiscard]] inline Rev3AuthoritativeSourceProof
derive_rev3_authoritative_source_proof_pg(
        const Rev3IndexAdministrationIdentity& identity,
        std::uint64_t sequence_rowno,
        Snapshot registered_snapshot) {
    const auto rev3_storage_error = [](std::string_view message) {
        rev3_storage_error_as_cpp(message);
    };
    struct ProofState {
        Relation source_relation = nullptr;
        BioseqColumnReadState* source_read = nullptr;
        BioseqColumnReadState* fragment_read = nullptr;
        seq_caches* fragment_caches = nullptr;
        MemoryContext work_context = nullptr;
        MemoryContext fragment_context = nullptr;
    };
    auto* state = new ProofState{};
    Rev3AuthoritativeSourceProof proof{};
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
            rev3_storage_error("authoritative source proof input is invalid");
        }
        state->work_context = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            return AllocSetContextCreate(
                CurrentMemoryContext,
                "bioseq rev3 authoritative source proof",
                ALLOCSET_DEFAULT_SIZES);
        });
        state->fragment_context = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            return AllocSetContextCreate(
                state->work_context, "bioseq rev3 source proof fragment",
                ALLOCSET_DEFAULT_SIZES);
        });
        state->source_relation = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            return try_safe_relation_open(
                identity.user_relid, AccessShareLock);
        });
        if (state->source_relation == nullptr
            || RelationGetRelid(state->source_relation) != identity.user_relid
            || state->source_relation->rd_locator.dbOid != identity.database_oid
            || static_cast<Oid>(state->source_relation->rd_locator.relNumber)
                != identity.main_relfilenode) {
            rev3_storage_error("authoritative source relation is missing or changed");
        }
        const Oid source_tablespace =
            OidIsValid(state->source_relation->rd_rel->reltablespace)
            ? state->source_relation->rd_rel->reltablespace
            : MyDatabaseTableSpace;
        TupleDesc source_desc = RelationGetDescr(state->source_relation);
        if (source_tablespace != identity.tablespace_oid
            || identity.column_attno > source_desc->natts
            || TupleDescAttr(source_desc, identity.column_attno - 1)->attisdropped) {
            rev3_storage_error("authoritative source relation/column identity changed");
        }

        MemoryContext previous_context = bioseq::pg_utils::invoke_pg_as_cpp(
            [&] { return MemoryContextSwitchTo(state->work_context); });
        const auto switch_context = [](MemoryContext context) {
            bioseq::pg_utils::invoke_pg_as_cpp(
                [context] { MemoryContextSwitchTo(context); });
        };
        List* projected = bioseq::pg_utils::invoke_pg_as_cpp(
            [&] { return list_make1_int(identity.column_attno); });
        Datum* values = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            return static_cast<Datum*>(palloc0(
                sizeof(Datum)
                * static_cast<std::size_t>(source_desc->natts)));
        });
        bool* nulls = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            return static_cast<bool*>(palloc0(
                sizeof(bool)
                * static_cast<std::size_t>(source_desc->natts)));
        });
        state->source_read = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            return BioseqColumnBeginRead(
                state->source_relation, source_desc, projected, NIL,
                state->work_context, registered_snapshot,
                /*randomAccess=*/true, nullptr);
        });
        const bool row_visible = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            return BioseqColumnReadRowByRowNumber(
                state->source_read, sequence_rowno, values, nulls);
        });
        if (!row_visible || nulls[identity.column_attno - 1]) {
            switch_context(previous_context);
            rev3_storage_error("authoritative source row is not visible");
        }
        pgseq_t* source_header = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            return reinterpret_cast<pgseq_t*>(PG_DETOAST_DATUM(
                values[identity.column_attno - 1]));
        });
        const std::uint8_t alphabet_code = static_cast<std::uint8_t>(
            source_header->alphabet_code);
        const std::uint8_t storage_width =
            logical_storage_unit_byte_width(source_header);
        const std::uint16_t padding_letters = source_header->padding_letters;
        if (storage_width == 0) {
            switch_context(previous_context);
            rev3_storage_error("authoritative source storage width is zero");
        }

        if (!is_locator_stub(source_header)) {
            switch_context(previous_context);
            rev3_storage_error(
                "authoritative revision-3 source is not an epoch-bearing locator stub");
        }
        const seq_locator locator = decode_locator_from_stub(source_header);
        if (locator.version != kLocatorVersion
            || locator._pad_2 != 0 || locator._pad_4 != 0
            || locator.source_epoch == 0) {
            switch_context(previous_context);
            rev3_storage_error("authoritative source locator is invalid");
        }
        proof.source_ = Rev3AuthoritativeSourceLocator{
            .sequence_rowno = sequence_rowno,
            .locator = locator,
            .alphabet_code = alphabet_code,
            .storage_unit_byte_width = storage_width,
            .padding_letters = padding_letters,
            .compatibility_checksum = 0,
        };

        const auto source_catalog = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            return SequenceSourceVersionManager::lookup(
                locator.source_epoch, registered_snapshot);
        });
        const std::uint64_t main_storage_id =
            bioseq::pg_utils::invoke_pg_as_cpp([&] {
                return BioseqColumnStorageGetStorageId(
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
            || !rev3_same_seq_locator(source_catalog->locator, locator)
            || source_catalog->alphabet_code != alphabet_code
            || source_catalog->storage_unit_byte_width != storage_width
            || source_catalog->padding_letters != padding_letters) {
            switch_context(previous_context);
            rev3_storage_error(
                "authoritative source epoch/catalog binding disagrees");
        }

        Crc64Ecma182 content_checksum{};
        std::uint64_t decoded_bp = 0;
        const auto consume_visible_fragment = [&content_checksum, &decoded_bp,
                &rev3_storage_error, alphabet_code,
                storage_width](const pgseq_t* fragment) {
            if (fragment == nullptr || is_locator_stub(fragment)
                || static_cast<std::uint8_t>(fragment->alphabet_code)
                    != alphabet_code
                || logical_storage_unit_byte_width(fragment) != storage_width) {
                rev3_storage_error("authoritative source fragment header disagrees");
            }
            fragment->convert_to([&content_checksum, &decoded_bp,
                    &rev3_storage_error]<class S>(
                    const compact_sequence_range<S>& range) {
                const std::string decoded =
                    ::bioseq::simd::active_target::decode_sequence_simd<S>(range);
                if (decoded.size() != range.bp_size()
                    || decoded.size()
                        > (std::numeric_limits<std::uint64_t>::max)()
                            - decoded_bp) {
                    rev3_storage_error("authoritative source decoded length is invalid");
                }
                content_checksum.update(std::as_bytes(
                    std::span<const char>{decoded.data(), decoded.size()}));
                decoded_bp += decoded.size();
            });
        };

        if (!is_locator_stub(source_header)) {
            consume_visible_fragment(source_header);
        } else if (locator.num_fragments != 0) {
            state->fragment_caches = bioseq::pg_utils::invoke_pg_as_cpp(
                [&] { return alloc_seq_caches(state->work_context); });
            state->fragment_caches->side = seq_caches::Side::Read;
            state->fragment_caches->lock_mode = AccessShareLock;
            state->fragment_caches->read_state_mctx = state->work_context;
            state->fragment_caches->main_database_oid = identity.database_oid;
            state->fragment_caches->main_tablespace_oid = identity.tablespace_oid;
            state->fragment_caches->main_relfilenode = identity.main_relfilenode;
            Relation fragment_relation = bioseq::pg_utils::invoke_pg_as_cpp(
                [&] {
                    return open_sub_table_for_locator(
                        identity.database_oid, identity.tablespace_oid,
                        identity.main_relfilenode, identity.column_attno,
                        locator, AccessShareLock, state->fragment_caches);
                });
            if (RelationGetRelid(fragment_relation)
                    != source_catalog->fragment_relid
                || bioseq::pg_utils::invoke_pg_as_cpp([&] {
                    return BioseqColumnStorageGetStorageId(
                        fragment_relation, false);
                })
                    != source_catalog->fragment_storage_id) {
                switch_context(previous_context);
                rev3_storage_error(
                    "authoritative source fragment storage incarnation changed");
            }
            TupleDesc fragment_desc = RelationGetDescr(fragment_relation);
            if (fragment_desc->natts != 1
                || TupleDescAttr(fragment_desc, 0)->attisdropped) {
                switch_context(previous_context);
                rev3_storage_error("authoritative source fragment schema disagrees");
            }
            List* fragment_projected = bioseq::pg_utils::invoke_pg_as_cpp(
                [] { return list_make1_int(1); });
            state->fragment_read = bioseq::pg_utils::invoke_pg_as_cpp([&] {
                return BioseqColumnBeginRead(
                    fragment_relation, fragment_desc, fragment_projected,
                    NIL, state->work_context, registered_snapshot,
                    /*randomAccess=*/true, nullptr);
            });
            for (std::uint32_t fragment_no = 0;
                 fragment_no < locator.num_fragments; ++fragment_no) {
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [] { CHECK_FOR_INTERRUPTS(); });
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [&] { MemoryContextReset(state->fragment_context); });
                Datum fragment_values[1]{0};
                bool fragment_nulls[1]{true};
                const std::uint64_t fragment_rowno =
                    locator.first_sub_row_no + fragment_no;
                if (fragment_rowno < locator.first_sub_row_no
                    || !bioseq::pg_utils::invoke_pg_as_cpp([&] {
                        return BioseqColumnReadRowByRowNumber(
                            state->fragment_read, fragment_rowno,
                            fragment_values, fragment_nulls);
                    })
                    || fragment_nulls[0]) {
                    switch_context(previous_context);
                    rev3_storage_error("authoritative source fragment row is missing");
                }
                switch_context(state->fragment_context);
                pgseq_t* fragment = bioseq::pg_utils::invoke_pg_as_cpp([&] {
                    return reinterpret_cast<pgseq_t*>(
                        PG_DETOAST_DATUM(fragment_values[0]));
                });
                if ((fragment_no + 1u != locator.num_fragments
                        && fragment->padding_letters != 0)
                    || (fragment_no + 1u == locator.num_fragments
                        && fragment->padding_letters != padding_letters)) {
                    switch_context(previous_context);
                    rev3_storage_error("authoritative source fragment padding disagrees");
                }
                consume_visible_fragment(fragment);
                switch_context(state->work_context);
            }
        }
        if (decoded_bp != locator.total_bp_size) {
            switch_context(previous_context);
            rev3_storage_error("authoritative source decoded length disagrees with locator");
        }
        if (content_checksum.checksum()
                != source_catalog->source_content_checksum) {
            switch_context(previous_context);
            rev3_storage_error(
                "authoritative source decoded checksum disagrees with its epoch catalog");
        }
        proof.source_content_checksum_ =
            source_catalog->source_content_checksum;
        proof.database_oid_ = identity.database_oid;
        proof.tablespace_oid_ = identity.tablespace_oid;
        proof.main_relfilenode_ = identity.main_relfilenode;
        proof.user_relid_ = identity.user_relid;
        proof.column_attno_ = identity.column_attno;
        proof.index_name_ = identity.index_name;
        proof.table_name_ = format_seqidx_name(
            identity.index_name, identity.database_oid,
            identity.tablespace_oid, identity.main_relfilenode,
            identity.column_attno, proof.source_.locator.bucket_idx,
            proof.source_.locator.shard_idx);
        proof.snapshot_ = registered_snapshot;
        proof.transaction_id_ = bioseq::pg_utils::invoke_pg_as_cpp(
            [] { return GetCurrentTransactionId(); });
        proof.valid_ = true;

        if (state->fragment_read != nullptr) {
            bioseq::pg_utils::invoke_pg_as_cpp(
                [&] { BioseqColumnEndRead(state->fragment_read); });
            state->fragment_read = nullptr;
        }
        if (state->fragment_caches != nullptr) {
            bioseq::pg_utils::invoke_pg_as_cpp([&] {
                close_and_destroy_seq_caches(
                    state->fragment_caches, /*commit=*/false);
            });
            state->fragment_caches = nullptr;
        }
        bioseq::pg_utils::invoke_pg_as_cpp(
            [&] { BioseqColumnEndRead(state->source_read); });
        state->source_read = nullptr;
        switch_context(previous_context);
        Relation finished_source = std::exchange(
            state->source_relation, nullptr);
        bioseq::pg_utils::invoke_pg_as_cpp(
            [&] { relation_close(finished_source, AccessShareLock); });
        MemoryContext finished_context = std::exchange(
            state->work_context, nullptr);
        state->fragment_context = nullptr;
        bioseq::pg_utils::invoke_pg_as_cpp(
            [&] { MemoryContextDelete(finished_context); });
    } catch (...) {
        state->fragment_read = nullptr;
        state->fragment_caches = nullptr;
        state->source_read = nullptr;
        state->source_relation = nullptr;
        state->work_context = nullptr;
        state->fragment_context = nullptr;
        proof.invalidate_();
        delete state;
        throw;
    }
    delete state;
    return proof;
}

/// Fixed work equation for cold, timed source admission. These counters travel
/// inside the move-only proof and are rechecked by the read session, so a later
/// accidental source scan cannot be benchmark-accepted as an O(1) load.
struct Rev3CatalogSourceProofCounters {
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

/// O(1), PostgreSQL-owned source proof for a cold reader. It validates one
/// visible parent stub and one source-epoch catalog tuple, plus constant-size
/// relation storage-incarnation metadata. It never opens a source fragment
/// read state and never decodes bases.
class Rev3CatalogSourceProof {
private:
    Rev3AuthoritativeSourceLocator source_{};
    std::uint64_t source_content_checksum_ = 0;
    Rev3CatalogSourceProofCounters counters_{};
    ColdIndexPreparationStatsPg cold_stats_{};
    Oid database_oid_ = InvalidOid;
    Oid tablespace_oid_ = InvalidOid;
    Oid main_relfilenode_ = InvalidOid;
    Oid user_relid_ = InvalidOid;
    std::int16_t column_attno_ = 0;
    std::string index_name_{};
    std::string table_name_{};
    Snapshot snapshot_ = nullptr;
    bool valid_ = false;

    Rev3CatalogSourceProof() = default;

    void invalidate_() noexcept {
        source_ = {};
        source_content_checksum_ = 0;
        counters_ = {};
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

    friend Rev3CatalogSourceProof derive_rev3_catalog_source_proof_pg(
        const Rev3IndexAdministrationIdentity&, std::uint64_t,
        const BorrowedExecutorReadContextPg&);
    friend Rev3CatalogSourceProof derive_rev3_catalog_source_proof_pg(
        const Rev3IndexAdministrationIdentity&, std::uint64_t, Snapshot);
    friend class IndexStorageFragmentGenerationReadSessionPg;
    friend class IndexStorageMinimizerFormat6ReadSessionPg;

public:
    Rev3CatalogSourceProof(const Rev3CatalogSourceProof&) = delete;
    Rev3CatalogSourceProof& operator=(const Rev3CatalogSourceProof&) = delete;
    Rev3CatalogSourceProof(Rev3CatalogSourceProof&& other) noexcept
        : source_(other.source_),
          source_content_checksum_(other.source_content_checksum_),
          counters_(other.counters_),
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
    Rev3CatalogSourceProof& operator=(
            Rev3CatalogSourceProof&& other) noexcept {
        if (this != &other) {
            invalidate_();
            source_ = other.source_;
            source_content_checksum_ = other.source_content_checksum_;
            counters_ = other.counters_;
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
    ~Rev3CatalogSourceProof() = default;

    [[nodiscard]] const Rev3AuthoritativeSourceLocator&
    source_for_diagnostics() const noexcept { return source_; }
    [[nodiscard]] std::uint64_t
    source_content_checksum_for_diagnostics() const noexcept {
        return source_content_checksum_;
    }
    [[nodiscard]] const Rev3CatalogSourceProofCounters&
    counters_for_diagnostics() const noexcept { return counters_; }
    [[nodiscard]] std::string_view
    table_name_for_diagnostics() const noexcept { return table_name_; }
};

static_assert(!std::is_default_constructible_v<Rev3CatalogSourceProof>);
static_assert(!std::is_copy_constructible_v<Rev3CatalogSourceProof>);
static_assert(std::is_nothrow_move_constructible_v<Rev3CatalogSourceProof>);

[[nodiscard]] inline Rev3CatalogSourceProof
derive_rev3_catalog_source_proof_pg(
        const Rev3IndexAdministrationIdentity& identity,
        std::uint64_t sequence_rowno,
        const BorrowedExecutorReadContextPg& executor_read) {
    const std::uint64_t source_begin_ns = cold_index_now_ns();
    const BufferUsage source_buffer_begin = pgBufferUsage;
    const auto rev3_storage_error = [](std::string_view message) {
        rev3_storage_error_as_cpp(message);
    };
    struct ProofState {
        Relation source_relation = nullptr;
        Relation fragment_relation = nullptr;
        BioseqColumnReadState* source_read = nullptr;
        MemoryContext work_context = nullptr;
    };
    auto* state = new ProofState{};
    Rev3CatalogSourceProof proof{};
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
            rev3_storage_error("catalog source proof input is invalid");
        }
        state->work_context = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            return AllocSetContextCreate(
                CurrentMemoryContext, "bioseq rev3 catalog source proof",
                ALLOCSET_DEFAULT_SIZES);
        });
        state->source_relation = executor_read.relation();
        if (state->source_relation == nullptr
            || RelationGetRelid(state->source_relation) != identity.user_relid
            || state->source_relation->rd_locator.dbOid != identity.database_oid
            || static_cast<Oid>(state->source_relation->rd_locator.relNumber)
                != identity.main_relfilenode) {
            rev3_storage_error("catalog source relation is missing or changed");
        }
        const Oid source_tablespace =
            OidIsValid(state->source_relation->rd_rel->reltablespace)
            ? state->source_relation->rd_rel->reltablespace
            : MyDatabaseTableSpace;
        TupleDesc source_desc = RelationGetDescr(state->source_relation);
        if (source_tablespace != identity.tablespace_oid
            || identity.column_attno > source_desc->natts
            || TupleDescAttr(source_desc, identity.column_attno - 1)->attisdropped) {
            rev3_storage_error("catalog source relation/column identity changed");
        }

        MemoryContext previous_context = bioseq::pg_utils::invoke_pg_as_cpp(
            [&] { return MemoryContextSwitchTo(state->work_context); });
        const auto switch_context = [](MemoryContext context) {
            bioseq::pg_utils::invoke_pg_as_cpp(
                [context] { MemoryContextSwitchTo(context); });
        };
        List* projected = bioseq::pg_utils::invoke_pg_as_cpp(
            [&] { return list_make1_int(identity.column_attno); });
        Datum* values = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            return static_cast<Datum*>(palloc0(
                sizeof(Datum)
                * static_cast<std::size_t>(source_desc->natts)));
        });
        bool* nulls = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            return static_cast<bool*>(palloc0(
                sizeof(bool)
                * static_cast<std::size_t>(source_desc->natts)));
        });
        state->source_read = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            return BioseqColumnBeginRead(
                state->source_relation, source_desc, projected, NIL,
                state->work_context, registered_snapshot,
                /*randomAccess=*/true, nullptr);
        });
        const bool row_visible = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            return BioseqColumnReadRowByRowNumber(
                state->source_read, sequence_rowno, values, nulls);
        });
        if (!row_visible || nulls[identity.column_attno - 1]) {
            switch_context(previous_context);
            rev3_storage_error("catalog source row is not visible");
        }
        ++proof.counters_.parent_rows_read;
        proof.cold_stats_.parent_random_reads = 1;
        pgseq_t* source_header = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            return reinterpret_cast<pgseq_t*>(PG_DETOAST_DATUM(
                values[identity.column_attno - 1]));
        });
        if (!is_locator_stub(source_header)) {
            switch_context(previous_context);
            rev3_storage_error(
                "catalog source is not an epoch-bearing locator stub");
        }
        const seq_locator locator = decode_locator_from_stub(source_header);
        const std::uint8_t alphabet_code = static_cast<std::uint8_t>(
            source_header->alphabet_code);
        const std::uint8_t storage_width =
            logical_storage_unit_byte_width(source_header);
        const std::uint16_t padding_letters = source_header->padding_letters;
        if (locator.version != kLocatorVersion || locator.source_epoch == 0
            || locator._pad_2 != 0 || locator._pad_4 != 0
            || storage_width == 0) {
            switch_context(previous_context);
            rev3_storage_error("catalog source locator is invalid");
        }

        bool exact_primary_index_scan = false;
        SequenceSourceLookupTelemetryPg source_lookup_telemetry{};
        const auto catalog = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            return SequenceSourceVersionManager::lookup(
                locator.source_epoch, registered_snapshot,
                /*require_ready=*/true, &exact_primary_index_scan,
                &source_lookup_telemetry);
        });
        if (exact_primary_index_scan) {
            ++proof.counters_.catalog_index_scans;
            proof.cold_stats_.add(
                proof.cold_stats_.source_heap_opens,
                source_lookup_telemetry.heap_opens);
            proof.cold_stats_.add(
                proof.cold_stats_.source_index_opens,
                source_lookup_telemetry.index_opens);
            proof.cold_stats_.add(
                proof.cold_stats_.source_schema_validations,
                source_lookup_telemetry.schema_validations);
            proof.cold_stats_.add(
                proof.cold_stats_.lock_acquires,
                source_lookup_telemetry.lock_acquires);
            proof.cold_stats_.add(
                proof.cold_stats_.lock_releases,
                source_lookup_telemetry.lock_releases);
        }
        if (catalog) ++proof.counters_.catalog_rows_read;
        const std::uint64_t main_storage_id =
            bioseq::pg_utils::invoke_pg_as_cpp([&] {
                return BioseqColumnStorageGetStorageId(
                    state->source_relation, false);
            });
        ++proof.counters_.storage_incarnations_checked;
        if (!catalog
            || catalog->source_epoch != locator.source_epoch
            || catalog->user_relid != identity.user_relid
            || catalog->database_oid != identity.database_oid
            || catalog->tablespace_oid != identity.tablespace_oid
            || catalog->main_relfilenode != identity.main_relfilenode
            || catalog->main_storage_id != main_storage_id
            || catalog->column_attno != identity.column_attno
            || !rev3_same_seq_locator(catalog->locator, locator)
            || catalog->alphabet_code != alphabet_code
            || catalog->storage_unit_byte_width != storage_width
            || catalog->padding_letters != padding_letters) {
            switch_context(previous_context);
            rev3_storage_error("catalog source epoch binding disagrees");
        }
        if (locator.num_fragments == 0) {
            if (OidIsValid(catalog->fragment_relid)
                || catalog->fragment_storage_id != 0
                || locator.first_sub_row_no != 0) {
                switch_context(previous_context);
                rev3_storage_error("empty catalog source sentinel disagrees");
            }
        } else {
            state->fragment_relation = bioseq::pg_utils::invoke_pg_as_cpp([&] {
                return try_safe_relation_open(
                    catalog->fragment_relid, AccessShareLock);
            });
            proof.cold_stats_.fragment_heap_opens = 1;
            proof.cold_stats_.add(proof.cold_stats_.lock_acquires, 1);
            const std::string expected_fragment_name = format_sub_table_name(
                identity.database_oid, identity.tablespace_oid,
                identity.main_relfilenode, identity.column_attno,
                static_cast<std::int16_t>(locator.bucket_idx),
                static_cast<std::int32_t>(locator.shard_idx));
            if (state->fragment_relation == nullptr
                || RelationGetNamespace(state->fragment_relation)
                    != bioseq_internal_ns()
                || expected_fragment_name
                    != RelationGetRelationName(state->fragment_relation)
                || bioseq::pg_utils::invoke_pg_as_cpp([&] {
                    return BioseqColumnStorageGetStorageId(
                        state->fragment_relation, false);
                })
                    != catalog->fragment_storage_id) {
                switch_context(previous_context);
                rev3_storage_error(
                    "catalog source fragment storage incarnation changed");
            }
            ++proof.counters_.storage_incarnations_checked;
        }

        proof.source_ = Rev3AuthoritativeSourceLocator{
            .sequence_rowno = sequence_rowno,
            .locator = locator,
            .alphabet_code = alphabet_code,
            .storage_unit_byte_width = storage_width,
            .padding_letters = padding_letters,
            .compatibility_checksum = 0,
        };
        proof.source_content_checksum_ = catalog->source_content_checksum;
        proof.database_oid_ = identity.database_oid;
        proof.tablespace_oid_ = identity.tablespace_oid;
        proof.main_relfilenode_ = identity.main_relfilenode;
        proof.user_relid_ = identity.user_relid;
        proof.column_attno_ = identity.column_attno;
        proof.index_name_ = identity.index_name;
        proof.table_name_ = format_seqidx_name(
            identity.index_name, identity.database_oid,
            identity.tablespace_oid, identity.main_relfilenode,
            identity.column_attno, locator.bucket_idx, locator.shard_idx);
        proof.snapshot_ = registered_snapshot;
        proof.valid_ = proof.counters_.timed_equation_holds();
        if (!proof.valid_) {
            switch_context(previous_context);
            rev3_storage_error("catalog source proof work equation disagrees");
        }

        if (state->fragment_relation != nullptr) {
            Relation finished = std::exchange(
                state->fragment_relation, nullptr);
            bioseq::pg_utils::invoke_pg_as_cpp(
                [&] { relation_close(finished, AccessShareLock); });
            proof.cold_stats_.add(proof.cold_stats_.lock_releases, 1);
        }
        bioseq::pg_utils::invoke_pg_as_cpp(
            [&] { BioseqColumnEndRead(state->source_read); });
        state->source_read = nullptr;
        switch_context(previous_context);
        // The executor owns this Relation and its AccessShareLock.  The proof
        // closes only its BioseqColumn read state and never closes the borrowed
        // descriptor.
        state->source_relation = nullptr;
        MemoryContext finished_context = std::exchange(
            state->work_context, nullptr);
        bioseq::pg_utils::invoke_pg_as_cpp(
            [&] { MemoryContextDelete(finished_context); });
        add_cold_index_elapsed(
            proof.cold_stats_, proof.cold_stats_.source_ns,
            source_begin_ns);
        add_cold_index_buffer_usage(proof.cold_stats_, source_buffer_begin);
    } catch (...) {
        state->source_read = nullptr;
        state->fragment_relation = nullptr;
        state->source_relation = nullptr;
        state->work_context = nullptr;
        proof.invalidate_();
        delete state;
        throw;
    }
    delete state;
    return proof;
}

/// Administrative compatibility overload.  Admin callers do not own an
/// executor Relation descriptor, so they retain the explicit open; the timed
/// BatchJoin production path above never enters this overload.
[[nodiscard]] inline Rev3CatalogSourceProof
derive_rev3_catalog_source_proof_pg(
        const Rev3IndexAdministrationIdentity& identity,
        std::uint64_t sequence_rowno,
        Snapshot registered_snapshot) {
    Relation relation = nullptr;
    try {
        relation = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            return try_safe_relation_open(identity.user_relid, AccessShareLock);
        });
        if (relation == nullptr) {
            rev3_storage_error_as_cpp(
                "catalog source relation is missing or changed");
        }
        BorrowedExecutorReadContextPg borrowed{
            relation, registered_snapshot};
        auto proof = derive_rev3_catalog_source_proof_pg(
            identity, sequence_rowno, borrowed);
        Relation finished = std::exchange(relation, nullptr);
        bioseq::pg_utils::invoke_pg_as_cpp(
            [finished] { relation_close(finished, AccessShareLock); });
        proof.cold_stats_.target_relation_extra_opens = 1;
        proof.cold_stats_.add(proof.cold_stats_.lock_acquires, 1);
        proof.cold_stats_.add(proof.cold_stats_.lock_releases, 1);
        return proof;
    } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
        relation = nullptr;
        throw;
    } catch (...) {
        if (relation != nullptr) {
            bioseq::pg_utils::invoke_pg_as_cpp(
                [relation] { relation_close(relation, AccessShareLock); });
        }
        throw;
    }
}

class IndexStorageFragmentGenerationRowSinkPg;
class ReadyGenerationToken;

/// Exact receipt returned by the immutable MPF5 image writer.  Device/inode
/// are transaction-local sealing evidence; the descriptor persists the
/// provider, byte length and authenticated header checksum.
struct Mpf5V2FileReceiptPg {
    bioseq::indexing::Mpf5V2Provider provider =
        bioseq::indexing::Mpf5V2Provider::postgres_relation;
    std::uint64_t file_bytes = 0u;
    std::uint64_t payload_crc64 = 0u;
    std::uint64_t header_crc64 = 0u;
    dev_t device{};
    ino_t inode{};
};

enum class MinimizerGenerationStorageStatePg : std::int16_t {
    live = 1,
    retired = 2,
};

/// Production row model for
/// bioseq_internal.minimizer_generation_storage.  Nullable relation locator
/// attributes stay optional so provider 1 cannot acquire accidental relation
/// authority through a default value.
struct MinimizerGenerationStorageDescriptorPg {
    Oid database_oid = InvalidOid;
    Oid tablespace_oid = InvalidOid;
    Oid main_relfilenode = InvalidOid;
    Oid user_relid = InvalidOid;
    std::int16_t column_attno = 0;
    std::string index_name{};
    std::uint64_t query_key = 0u;
    std::uint64_t generation_id = 0u;
    bioseq::indexing::Mpf5V2Provider provider =
        bioseq::indexing::Mpf5V2Provider::postgres_relation;
    std::optional<Oid> payload_relation_oid{};
    std::optional<Oid> payload_database_oid{};
    std::optional<Oid> payload_tablespace_oid{};
    std::optional<Oid> payload_relfilenode{};
    std::optional<std::string> payload_relation_name{};
    std::uint64_t image_bytes = 0u;
    std::uint64_t header_checksum = 0u;
    std::optional<std::uint64_t> retire_full_xid{};
    MinimizerGenerationStorageStatePg state =
        MinimizerGenerationStorageStatePg::live;
};

inline constexpr AttrNumber kStorageDescriptorAttDatabaseOid = 1;
inline constexpr AttrNumber kStorageDescriptorAttTablespaceOid = 2;
inline constexpr AttrNumber kStorageDescriptorAttMainRelfilenode = 3;
inline constexpr AttrNumber kStorageDescriptorAttUserRelid = 4;
inline constexpr AttrNumber kStorageDescriptorAttColumnAttno = 5;
inline constexpr AttrNumber kStorageDescriptorAttIndexName = 6;
inline constexpr AttrNumber kStorageDescriptorAttQueryKey = 7;
inline constexpr AttrNumber kStorageDescriptorAttGenerationId = 8;
inline constexpr AttrNumber kStorageDescriptorAttProvider = 9;
inline constexpr AttrNumber kStorageDescriptorAttPayloadRelationOid = 10;
inline constexpr AttrNumber kStorageDescriptorAttPayloadDatabaseOid = 11;
inline constexpr AttrNumber kStorageDescriptorAttPayloadTablespaceOid = 12;
inline constexpr AttrNumber kStorageDescriptorAttPayloadRelfilenode = 13;
inline constexpr AttrNumber kStorageDescriptorAttPayloadRelationName = 14;
inline constexpr AttrNumber kStorageDescriptorAttImageBytes = 15;
inline constexpr AttrNumber kStorageDescriptorAttHeaderChecksum = 16;
inline constexpr AttrNumber kStorageDescriptorAttRetireFullXid = 17;
inline constexpr AttrNumber kStorageDescriptorAttState = 18;
inline constexpr std::size_t kStorageDescriptorAttributeCount = 18u;

[[nodiscard]] inline bool exact_storage_descriptor_identity(
        const MinimizerGenerationStorageDescriptorPg& descriptor,
        const Rev3IndexAdministrationIdentity& identity,
        const std::uint64_t query_key) noexcept {
    return descriptor.database_oid == identity.database_oid
        && descriptor.tablespace_oid == identity.tablespace_oid
        && descriptor.main_relfilenode == identity.main_relfilenode
        && descriptor.user_relid == identity.user_relid
        && descriptor.column_attno == identity.column_attno
        && descriptor.index_name == identity.index_name
        && descriptor.query_key == query_key;
}

[[nodiscard]] inline bool exact_storage_descriptor_provider_shape(
        const MinimizerGenerationStorageDescriptorPg& descriptor) noexcept {
    using bioseq::indexing::Mpf5V2Provider;
    if (descriptor.provider == Mpf5V2Provider::postgres_relation) {
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
    if (descriptor.provider == Mpf5V2Provider::mapped_file) {
        return !descriptor.payload_relation_oid
            && !descriptor.payload_database_oid
            && !descriptor.payload_tablespace_oid
            && !descriptor.payload_relfilenode
            && !descriptor.payload_relation_name;
    }
    return false;
}

struct ReadyGenerationPublishedHeadReceipt {
    std::string table_name;
    std::uint64_t query_key = 0;
    MinimizerGenerationHead head{};
    std::array<std::byte, MinimizerGenerationHead::encoded_bytes>
        head_bytes{};
    std::optional<std::uint64_t> replaced_generation_id{};
};

template <class Receipt>
concept ReportsReadyGenerationAcceptance =
    requires(const Receipt& receipt) { receipt.published(); }
    || requires(const Receipt& receipt) { receipt.execution_accepted(); }
    || requires(const Receipt& receipt) { receipt.accepted(); };

static_assert(!ReportsReadyGenerationAcceptance<
    ReadyGenerationPublishedHeadReceipt>);

[[nodiscard]] inline ReadyGenerationPublishedHeadReceipt
publish_ready_minimizer_generation_head_pg(
    ReadyGenerationToken&& token);

/// Move-only proof that one exact relation/source/generation was completely
/// reread under an own-write-visible registered snapshot. Construction is
/// private to the generation sink's full validation pass.
class ReadyGenerationToken {
private:
    Rev3IndexAdministrationIdentity identity_{};
    Oid sidecar_relation_oid_ = InvalidOid;
    RelFileLocator sidecar_locator_{};
    std::uint64_t advisory_lock_key_ = 0;
    std::uint64_t query_key_ = 0;
    std::uint64_t generation_id_ = 0;
    std::uint64_t source_identity_ = 0;
    std::uint64_t source_content_checksum_ = 0;
    std::uint64_t compatibility_checksum_ = 0;
    TransactionId transaction_id_ = InvalidTransactionId;
    std::array<std::byte, MinimizerGenerationHead::encoded_bytes>
        head_bytes_{};
    std::uint64_t head_row_checksum_ = 0;
    std::optional<Mpf5V2FileReceiptPg> file_receipt_{};
    bool valid_ = false;

    ReadyGenerationToken(
            Rev3IndexAdministrationIdentity identity,
            Oid sidecar_relation_oid,
            RelFileLocator sidecar_locator,
            std::uint64_t advisory_lock_key,
            std::uint64_t query_key,
            std::uint64_t generation_id,
            std::uint64_t source_identity,
            std::uint64_t source_content_checksum,
            std::uint64_t compatibility_checksum,
            TransactionId transaction_id,
            std::span<const std::byte> head_bytes,
            std::uint64_t head_row_checksum,
            std::optional<Mpf5V2FileReceiptPg> file_receipt)
        : identity_(std::move(identity)),
          sidecar_relation_oid_(sidecar_relation_oid),
          sidecar_locator_(sidecar_locator),
          advisory_lock_key_(advisory_lock_key),
          query_key_(query_key),
          generation_id_(generation_id),
          source_identity_(source_identity),
          source_content_checksum_(source_content_checksum),
          compatibility_checksum_(compatibility_checksum),
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
        compatibility_checksum_ = 0;
        transaction_id_ = InvalidTransactionId;
        head_bytes_.fill(std::byte{0});
        head_row_checksum_ = 0;
        file_receipt_.reset();
        valid_ = false;
    }

    friend class IndexStorageFragmentGenerationRowSinkPg;
    friend ReadyGenerationPublishedHeadReceipt
    publish_ready_minimizer_generation_head_pg(
        ReadyGenerationToken&& token);

public:
    ReadyGenerationToken() = delete;
    ReadyGenerationToken(const ReadyGenerationToken&) = delete;
    ReadyGenerationToken& operator=(const ReadyGenerationToken&) = delete;
    ReadyGenerationToken(ReadyGenerationToken&& other) noexcept
        : identity_(std::move(other.identity_)),
          sidecar_relation_oid_(other.sidecar_relation_oid_),
          sidecar_locator_(other.sidecar_locator_),
          advisory_lock_key_(other.advisory_lock_key_),
          query_key_(other.query_key_),
          generation_id_(other.generation_id_),
          source_identity_(other.source_identity_),
          source_content_checksum_(other.source_content_checksum_),
          compatibility_checksum_(other.compatibility_checksum_),
          transaction_id_(other.transaction_id_),
          head_bytes_(other.head_bytes_),
          head_row_checksum_(other.head_row_checksum_),
          file_receipt_(std::move(other.file_receipt_)),
          valid_(other.valid_) {
        other.invalidate();
    }
    ReadyGenerationToken& operator=(ReadyGenerationToken&& other) noexcept {
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
            compatibility_checksum_ = other.compatibility_checksum_;
            transaction_id_ = other.transaction_id_;
            head_bytes_ = other.head_bytes_;
            head_row_checksum_ = other.head_row_checksum_;
            file_receipt_ = std::move(other.file_receipt_);
            valid_ = other.valid_;
            other.invalidate();
        }
        return *this;
    }
    ~ReadyGenerationToken() = default;
};

static_assert(!std::is_default_constructible_v<ReadyGenerationToken>);
static_assert(!std::is_copy_constructible_v<ReadyGenerationToken>);
static_assert(!std::is_copy_assignable_v<ReadyGenerationToken>);
static_assert(std::is_nothrow_move_constructible_v<ReadyGenerationToken>);
static_assert(std::is_nothrow_move_assignable_v<ReadyGenerationToken>);

/// Read the sole LIVE descriptor for one exact storage identity through the
/// catalog's partial unique index.  The caller owns both relation locks and
/// the registered MVCC snapshot; this helper neither substitutes SnapshotSelf
/// nor consults the current payload-provider GUC.
[[nodiscard]] inline MinimizerGenerationStorageDescriptorPg
read_live_minimizer_generation_storage_descriptor_pg(
        Relation storage_relation,
        Relation live_index_relation,
        Snapshot registered_snapshot,
        const Rev3IndexAdministrationIdentity& identity,
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
        || identity.index_name != MINIMIZERS_PER_FRAGMENT_INDEX_NAME
        || !sql_bigint_can_represent(query_key)
        || expected_generation_id == 0u
        || !sql_bigint_can_represent(expected_generation_id)) {
        rev3_storage_error_as_cpp(
            "LIVE generation descriptor lookup binding is invalid");
    }

    NameData index_name{};
    bioseq::pg_utils::invoke_pg_as_cpp([&] {
        namestrcpy(&index_name, identity.index_name.c_str());
    });
    ScanKeyData keys[7];
    ScanKeyInit(&keys[0], kStorageDescriptorAttDatabaseOid,
        BTEqualStrategyNumber, F_OIDEQ,
        ObjectIdGetDatum(identity.database_oid));
    ScanKeyInit(&keys[1], kStorageDescriptorAttTablespaceOid,
        BTEqualStrategyNumber, F_OIDEQ,
        ObjectIdGetDatum(identity.tablespace_oid));
    ScanKeyInit(&keys[2], kStorageDescriptorAttMainRelfilenode,
        BTEqualStrategyNumber, F_OIDEQ,
        ObjectIdGetDatum(identity.main_relfilenode));
    ScanKeyInit(&keys[3], kStorageDescriptorAttUserRelid,
        BTEqualStrategyNumber, F_OIDEQ,
        ObjectIdGetDatum(identity.user_relid));
    ScanKeyInit(&keys[4], kStorageDescriptorAttColumnAttno,
        BTEqualStrategyNumber, F_INT2EQ,
        Int16GetDatum(identity.column_attno));
    ScanKeyInit(&keys[5], kStorageDescriptorAttIndexName,
        BTEqualStrategyNumber, F_NAMEEQ, NameGetDatum(&index_name));
    ScanKeyInit(&keys[6], kStorageDescriptorAttQueryKey,
        BTEqualStrategyNumber, F_INT8EQ,
        Int64GetDatum(static_cast<std::int64_t>(query_key)));

    SysScanDesc scan = nullptr;
    try {
        scan = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            return systable_beginscan_ordered(
                storage_relation, live_index_relation,
                registered_snapshot, 7, keys);
        });
        HeapTuple tuple = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            return systable_getnext_ordered(scan, ForwardScanDirection);
        });
        if (!HeapTupleIsValid(tuple)) {
            rev3_storage_error_as_cpp(
                "LIVE generation storage descriptor is missing");
        }

        Datum values[kStorageDescriptorAttributeCount]{};
        bool nulls[kStorageDescriptorAttributeCount]{};
        bioseq::pg_utils::invoke_pg_as_cpp([&] {
            heap_deform_tuple(
                tuple, RelationGetDescr(storage_relation), values, nulls);
        });
        constexpr std::array<std::size_t, 12u> required_non_null{
            0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 14u, 15u, 17u};
        for (const std::size_t attribute : required_non_null) {
            if (nulls[attribute]) {
                rev3_storage_error_as_cpp(
                    "LIVE generation descriptor has a NULL required attribute");
            }
        }

        const std::int64_t query_key_sql = DatumGetInt64(values[6]);
        const std::int64_t generation_id_sql = DatumGetInt64(values[7]);
        const std::int16_t provider_sql = DatumGetInt16(values[8]);
        const std::int64_t image_bytes_sql = DatumGetInt64(values[14]);
        const std::int16_t state_sql = DatumGetInt16(values[17]);
        if (query_key_sql < 0 || generation_id_sql <= 0
            || image_bytes_sql
                < static_cast<std::int64_t>(kMpf5V2HeaderBytes)
            || !is_valid_mpf5_v2_provider(
                static_cast<std::uint8_t>(provider_sql))) {
            rev3_storage_error_as_cpp(
                "LIVE generation descriptor scalar shape is invalid");
        }

        MinimizerGenerationStorageDescriptorPg descriptor{
            .database_oid = DatumGetObjectId(values[0]),
            .tablespace_oid = DatumGetObjectId(values[1]),
            .main_relfilenode = DatumGetObjectId(values[2]),
            .user_relid = DatumGetObjectId(values[3]),
            .column_attno = DatumGetInt16(values[4]),
            .index_name = NameStr(*DatumGetName(values[5])),
            .query_key = static_cast<std::uint64_t>(query_key_sql),
            .generation_id = static_cast<std::uint64_t>(generation_id_sql),
            .provider = static_cast<Mpf5V2Provider>(provider_sql),
            .payload_relation_oid = nulls[9]
                ? std::nullopt
                : std::optional<Oid>{DatumGetObjectId(values[9])},
            .payload_database_oid = nulls[10]
                ? std::nullopt
                : std::optional<Oid>{DatumGetObjectId(values[10])},
            .payload_tablespace_oid = nulls[11]
                ? std::nullopt
                : std::optional<Oid>{DatumGetObjectId(values[11])},
            .payload_relfilenode = nulls[12]
                ? std::nullopt
                : std::optional<Oid>{DatumGetObjectId(values[12])},
            .payload_relation_name = nulls[13]
                ? std::nullopt
                : std::optional<std::string>{
                    NameStr(*DatumGetName(values[13]))},
            .image_bytes = static_cast<std::uint64_t>(image_bytes_sql),
            .header_checksum = checksum_from_pg_bigint(
                DatumGetInt64(values[15])),
            .retire_full_xid = nulls[16]
                ? std::nullopt
                : std::optional<std::uint64_t>{U64FromFullTransactionId(
                    DatumGetFullTransactionId(values[16]))},
            .state = static_cast<MinimizerGenerationStorageStatePg>(
                state_sql),
        };
        if (!exact_storage_descriptor_identity(
                descriptor, identity, query_key)
            || descriptor.generation_id != expected_generation_id
            || descriptor.state
                != MinimizerGenerationStorageStatePg::live
            || descriptor.retire_full_xid
            || !exact_storage_descriptor_provider_shape(descriptor)) {
            rev3_storage_error_as_cpp(
                "LIVE generation descriptor disagrees with the active head");
        }

        HeapTuple duplicate = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            return systable_getnext_ordered(scan, ForwardScanDirection);
        });
        if (HeapTupleIsValid(duplicate)) {
            rev3_storage_error_as_cpp(
                "LIVE generation descriptor lookup returned duplicates");
        }
        bioseq::pg_utils::invoke_pg_as_cpp(
            [&] { systable_endscan_ordered(scan); });
        scan = nullptr;
        return descriptor;
    } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
        scan = nullptr;
        throw;
    } catch (...) {
        if (scan != nullptr) {
            bioseq::pg_utils::invoke_pg_as_cpp(
                [&] { systable_endscan_ordered(scan); });
            scan = nullptr;
        }
        throw;
    }
}

/// Form one immutable revision-3 payload/directory/manifest tuple. The caller
/// transfers the tuple into a TTSOpsHeapTuple slot and flushes a bounded group
/// with CatalogTuplesMultiInsertWithInfo, PostgreSQL 17's supported catalog
/// heap-multi-insert + index-maintenance API. This helper never inserts, probes
/// for, or replaces a row by itself.
[[nodiscard]] inline std::expected<HeapTuple, MinimizerGenerationWireError>
form_rev3_generation_row_pg(
        Relation relation,
        std::uint64_t query_key,
        const ValidatedRev3GenerationRow& validated_row,
        bool inject_failure = true) {
    const MinimizerGenerationRowView& row = validated_row.row();
    if (relation == nullptr
        || !bioseq::indexing::sql_bigint_can_represent(query_key)) {
        return std::unexpected(MinimizerGenerationWireError::generation_id);
    }
    bytea* blob = bioseq::pg_utils::invoke_pg_as_cpp([&] {
        return reinterpret_cast<bytea*>(
            palloc(VARHDRSZ + row.bytes.size()));
    });
    SET_VARSIZE(blob, VARHDRSZ + row.bytes.size());
    std::memcpy(VARDATA(blob), row.bytes.data(), row.bytes.size());
    bioseq::pg_utils::pg_datum_list<8> values{};
    values.set(kRev3AttQueryKey, static_cast<std::int64_t>(query_key));
    values.set(kRev3AttGenerationId,
        static_cast<std::int64_t>(row.address.generation_id));
    values.set(kRev3AttPositionSegment, row.address.position_segment);
    values.set(kRev3AttSubKey,
        static_cast<std::int64_t>(row.address.sub_key));
    values.set(kRev3AttChunkNo,
        static_cast<std::int32_t>(row.address.chunk_no));
    values.set(kRev3AttChunkBytes,
        static_cast<std::int32_t>(row.bytes.size()));
    values.set(kRev3AttChecksum,
        checksum_to_pg_bigint(row.row_checksum));
    values.set_raw(kRev3AttBlob,
        bioseq::pg_utils::pg_datum::from_pointer(blob));
    HeapTuple tuple = bioseq::pg_utils::invoke_pg_as_cpp(
        [&values, relation] {
            return values.form_tuple(RelationGetDescr(relation));
        });
    if (inject_failure) {
        inject_rev3_pg_failure_as_cpp(
            Rev3PgFailureInjection::generation_tuple_form,
            "generation_tuple_form");
    }
    bioseq::pg_utils::invoke_pg_as_cpp([blob] { pfree(blob); });
    return tuple;
}

/// Exact PostgreSQL ABI proof for the S1 one-page PLAIN payload contract.
struct PgInlinePayloadLayout {
    std::uint32_t block_bytes = 0;
    std::uint32_t max_heap_tuple_bytes = 0;
    std::uint32_t fixed_tuple_bytes = 0;
    std::uint32_t payload_bytes = 0;
    std::uint32_t alignment_multiple = 0;
    std::uint32_t full_tuple_bytes = 0;
};

[[nodiscard]] inline PgInlinePayloadLayout
verify_pg_inline_payload_layout(Relation relation) {
    static std::optional<PgInlinePayloadLayout> cached;
    if (cached) return *cached;
    if (relation == nullptr || BLCKSZ != 8192
        || RelationGetDescr(relation) == nullptr
        || RelationGetDescr(relation)->natts
            != static_cast<int>(kRev3IndexAttributeDescriptors.size())) {
        rev3_storage_error_as_cpp(
            "Format-5/S1 requires BLCKSZ=8192 and the exact tuple descriptor");
    }
    constexpr std::uint32_t payload_bytes =
        bioseq::indexing::kMinimizerFormat5ExtentPayloadBytes;
    constexpr std::uint32_t alignment_multiple = 96u;
    static_assert(payload_bytes % alignment_multiple == 0u);
    static_assert(payload_bytes % 16u == 0u);
    static_assert(payload_bytes % 12u == 0u);
    static_assert(payload_bytes % 8u == 0u);
    static_assert(payload_bytes % 4u == 0u);

    std::array<std::byte, payload_bytes> payload{};
    const IndexChunkAddress address{
        .generation_id = 1u,
        .position_segment = 0,
        .sub_key = 0,
        .chunk_no = 0u,
    };
    const MinimizerGenerationRowView row{
        .address = address,
        .bytes = payload,
        .row_checksum = bioseq::indexing::crc64_index_chunk_row(
            address.generation_id, address.position_segment,
            address.sub_key, address.chunk_no, payload_bytes, payload),
    };
    auto validated = validate_rev3_generation_row_for_write(1u, row);
    if (!validated) {
        rev3_storage_error_as_cpp(
            "Format-5/S1 representative row validation failed");
    }
    auto tuple = form_rev3_generation_row_pg(
        relation, 1u, *validated, /*inject_failure=*/false);
    if (!tuple) {
        rev3_storage_error_as_cpp(
            "Format-5/S1 representative tuple formation failed");
    }
    const std::uint32_t tuple_bytes = (*tuple)->t_len;
    heap_freetuple(*tuple);
    if (tuple_bytes < payload_bytes) {
        rev3_storage_error_as_cpp(
            "Format-5/S1 representative tuple length underflow");
    }
    const std::uint32_t fixed_tuple_bytes = tuple_bytes - payload_bytes;
    const std::uint32_t max_payload_by_tuple =
        static_cast<std::uint32_t>(MaxHeapTupleSize) - fixed_tuple_bytes;
    const std::uint32_t largest_aligned_payload =
        max_payload_by_tuple / alignment_multiple * alignment_multiple;
    const std::uint32_t page_payload_capacity =
        BLCKSZ - SizeOfPageHeaderData;
    if (tuple_bytes != 8140u
        || tuple_bytes > static_cast<std::uint32_t>(MaxHeapTupleSize)
        || MAXALIGN(tuple_bytes) + sizeof(ItemIdData) > page_payload_capacity
        || largest_aligned_payload != payload_bytes) {
        rev3_storage_error_as_cpp(
            "Format-5/S1 8064-byte payload does not satisfy the one-page tuple equation");
    }
    cached = PgInlinePayloadLayout{
        .block_bytes = BLCKSZ,
        .max_heap_tuple_bytes = MaxHeapTupleSize,
        .fixed_tuple_bytes = fixed_tuple_bytes,
        .payload_bytes = payload_bytes,
        .alignment_multiple = alignment_multiple,
        .full_tuple_bytes = tuple_bytes,
    };
    return *cached;
}

struct Rev3LoadedPgRow {
    std::uint64_t query_key = 0;
    IndexChunkAddress address{};
    std::uint32_t chunk_bytes = 0;
    std::uint64_t checksum = 0;
    // The schema is frozen to PLAIN and the loader rejects every external or
    // compressed datum. Payload bytes either borrow the current scan slot or
    // one admitted batch; neither form needs detoast ownership or pfree.
    std::span<const std::byte> bytes;
    std::optional<ValidatedMinimizerGenerationRowReceipt>
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
struct Rev3DeformedPgRow {
    HeapTuple tuple = nullptr;
    std::array<Datum, kRev3IndexAttributeDescriptors.size()> values{};
    std::array<bool, kRev3IndexAttributeDescriptors.size()> nulls{};
};

static_assert(std::is_trivially_copyable_v<Rev3DeformedPgRow>);
static_assert(std::is_trivially_destructible_v<Rev3DeformedPgRow>);

struct Rev3NextDeformedPgRow {
    Rev3DeformedPgRow row{};
    bool found = false;
};

static_assert(std::is_trivially_copyable_v<Rev3NextDeformedPgRow>);
static_assert(std::is_trivially_destructible_v<Rev3NextDeformedPgRow>);

/// PostgreSQL leaf: deform all fixed attributes once instead of entering one
/// PG_TRY/sigsetjmp bridge per heap_getattr call.  Call either from an existing
/// invoke_pg_as_cpp boundary or through deform_rev3_pg_row_or_error().
[[nodiscard]] inline Rev3DeformedPgRow deform_rev3_pg_row_pg_leaf(
        Relation relation,
        HeapTuple tuple) {
    Rev3DeformedPgRow deformed{.tuple = tuple};
    heap_deform_tuple(tuple, RelationGetDescr(relation),
        deformed.values.data(), deformed.nulls.data());
    return deformed;
}

/// Standalone deformation bridge used by metadata, publication validation,
/// and selective reads. The full-generation route instead combines its fetch
/// and deformation through next_rev3_pg_row_or_error().
[[nodiscard]] inline Rev3DeformedPgRow deform_rev3_pg_row_or_error(
        Relation relation,
        HeapTuple tuple) {
    return bioseq::pg_utils::invoke_pg_as_cpp(
        [relation, tuple] {
            return deform_rev3_pg_row_pg_leaf(relation, tuple);
        });
}

/// Fetch and deform one ordered-scan row under one PostgreSQL boundary. The
/// returned object is trivial; authentication and arbitrary sinks run only
/// after PG_END_TRY has restored PostgreSQL's exception stack.
[[nodiscard]] inline Rev3NextDeformedPgRow next_rev3_pg_row_or_error(
        Relation relation,
        SysScanDesc scan) {
    return bioseq::pg_utils::invoke_pg_as_cpp(
        [relation, scan] {
            HeapTuple tuple = systable_getnext_ordered(
                scan, ForwardScanDirection);
            if (!HeapTupleIsValid(tuple)) return Rev3NextDeformedPgRow{};
            return Rev3NextDeformedPgRow{
                .row = deform_rev3_pg_row_pg_leaf(relation, tuple),
                .found = true,
            };
        });
}

/// Trivial borrowed projection used while a PostgreSQL sigsetjmp frame is
/// active.  It performs no allocation, owns no receipt, and reports semantic
/// failure as a static message for the C++ caller to raise after leaving the
/// PostgreSQL boundary.
struct Rev3ProjectedPgRow {
    std::uint64_t query_key = 0;
    IndexChunkAddress address{};
    std::uint32_t chunk_bytes = 0;
    std::uint64_t checksum = 0;
    std::span<const std::byte> bytes{};
    const char* error = nullptr;
};

static_assert(std::is_trivially_copyable_v<Rev3ProjectedPgRow>);
static_assert(std::is_trivially_destructible_v<Rev3ProjectedPgRow>);

/// Decode the frozen eight-column projection and prove inline PLAIN/length
/// shape without constructing any C++ authority object.  Address-bound CRC
/// validation remains outside PostgreSQL error frames.
[[nodiscard]] inline Rev3ProjectedPgRow project_rev3_deformed_pg_row(
        const Rev3DeformedPgRow& deformed) noexcept {
    const auto read_attribute = [&deformed](AttrNumber attribute_number) {
        return deformed.values[static_cast<std::size_t>(
            attribute_number - 1)];
    };
    const auto attribute_is_null = [&deformed](AttrNumber attribute_number) {
        return deformed.nulls[static_cast<std::size_t>(
            attribute_number - 1)];
    };
    const auto failed = [](const char* message) noexcept {
        return Rev3ProjectedPgRow{.error = message};
    };
    const Datum query_key_datum = read_attribute(kRev3AttQueryKey);
    if (attribute_is_null(kRev3AttQueryKey)) {
        return failed("row query key is NULL");
    }
    const std::int64_t signed_query_key = DatumGetInt64(query_key_datum);
    if (signed_query_key < 0) return failed("row query key is negative");
    const Datum generation_datum = read_attribute(kRev3AttGenerationId);
    if (attribute_is_null(kRev3AttGenerationId)) {
        return failed("row generation ID is NULL");
    }
    const std::int64_t signed_generation_id = DatumGetInt64(generation_datum);
    if (signed_generation_id < 0) {
        return failed("row generation ID is negative");
    }
    const Datum position_datum = read_attribute(kRev3AttPositionSegment);
    if (attribute_is_null(kRev3AttPositionSegment)) {
        return failed("row position segment is NULL");
    }
    const std::int32_t position_segment = DatumGetInt32(position_datum);
    const Datum sub_key_datum = read_attribute(kRev3AttSubKey);
    if (attribute_is_null(kRev3AttSubKey)) {
        return failed("row sub-key is NULL");
    }
    const std::int64_t sub_key = DatumGetInt64(sub_key_datum);
    const Datum chunk_no_datum = read_attribute(kRev3AttChunkNo);
    if (attribute_is_null(kRev3AttChunkNo)) {
        return failed("row chunk number is NULL");
    }
    const std::int32_t signed_chunk_no = DatumGetInt32(chunk_no_datum);
    if (signed_chunk_no < 0) return failed("row chunk number is negative");
    const Datum chunk_bytes_datum = read_attribute(kRev3AttChunkBytes);
    if (attribute_is_null(kRev3AttChunkBytes)) {
        return failed("row byte count is NULL");
    }
    const std::int32_t signed_chunk_bytes = DatumGetInt32(chunk_bytes_datum);
    if (signed_chunk_bytes <= 0) {
        return failed("row byte count is non-positive");
    }
    const Datum checksum_datum = read_attribute(kRev3AttChecksum);
    if (attribute_is_null(kRev3AttChecksum)) {
        return failed("row checksum is NULL");
    }
    const std::int64_t signed_checksum = DatumGetInt64(checksum_datum);
    const Datum blob_datum = read_attribute(kRev3AttBlob);
    if (attribute_is_null(kRev3AttBlob)) return failed("row blob is NULL");
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
    const IndexChunkAddress address{
        .generation_id = static_cast<std::uint64_t>(signed_generation_id),
        .position_segment = position_segment,
        .sub_key = sub_key,
        .chunk_no = static_cast<std::uint32_t>(signed_chunk_no),
    };
    const std::span<const std::byte> bytes{
        reinterpret_cast<const std::byte*>(VARDATA_ANY(inline_blob)),
        payload_bytes};
    const std::uint64_t checksum = checksum_from_pg_bigint(signed_checksum);
    return Rev3ProjectedPgRow{
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
inline constexpr std::size_t kRev3FullGenerationPinnedBatchRows = 128;

struct Rev3PinnedProjectedPgRow {
    Rev3ProjectedPgRow projected{};
    ItemPointerData tid{};
};

static_assert(std::is_trivially_copyable_v<Rev3PinnedProjectedPgRow>);
static_assert(std::is_trivially_destructible_v<Rev3PinnedProjectedPgRow>);

/// Trivial output from one PostgreSQL boundary. Each projection borrows the
/// independently pinned BufferHeapTuple slot at the same ordinal. Receipts,
/// CRC work and arbitrary sinks are deliberately constructed only after the
/// boundary has been left.
struct Rev3PinnedProjectedPgRowBatch {
    std::array<Rev3PinnedProjectedPgRow,
        kRev3FullGenerationPinnedBatchRows> rows{};
    std::size_t size = 0;
    bool eof = false;
};

static_assert(std::is_trivially_copyable_v<Rev3PinnedProjectedPgRowBatch>);
static_assert(std::is_trivially_destructible_v<
    Rev3PinnedProjectedPgRowBatch>);

/// Keep systable_getnext_ordered's concurrent-abort contract while using
/// caller-owned slots. PostgreSQL keeps this helper private to genam.c, so the
/// extension mirrors its PG17 implementation inside the same protected leaf.
inline void handle_rev3_concurrent_abort_pg_leaf() {
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
/// identity remains the first observable failure, exactly as in the old
/// one-row reader.
[[nodiscard]] inline Rev3PinnedProjectedPgRowBatch
next_rev3_pinned_projected_pg_row_batch_or_error(
        Relation relation,
        SysScanDesc scan,
        const std::array<TupleTableSlot*,
            kRev3FullGenerationPinnedBatchRows>& slots) {
    Rev3PinnedProjectedPgRowBatch batch{};
    bioseq::pg_utils::invoke_pg_as_cpp(
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
                handle_rev3_concurrent_abort_pg_leaf();
                if (!HeapTupleIsValid(tuple)) {
                    batch.eof = true;
                    return;
                }

                Rev3PinnedProjectedPgRow& output =
                    batch.rows[batch.size];
                output.tid = tuple->t_self;
                output.projected = project_rev3_deformed_pg_row(
                    deform_rev3_pg_row_pg_leaf(relation, tuple));
                ++batch.size;
                if (output.projected.error != nullptr) return;
            }
        });
    return batch;
}

/// Validate the address-bound row checksum only after PostgreSQL deformation
/// has completed.  Callers outside a PG boundary may safely own the receipt
/// and invoke arbitrary C++ sinks.
[[nodiscard]] inline Rev3LoadedPgRow load_rev3_projected_pg_row_or_error(
        const Rev3ProjectedPgRow& projected) {
    if (projected.error != nullptr) {
        rev3_storage_error_as_cpp(projected.error);
    }
    auto validated_receipt =
        bioseq::indexing::validate_minimizer_generation_row_receipt(
            MinimizerGenerationRowView{
                .address = projected.address,
                .bytes = projected.bytes,
                .row_checksum = projected.checksum,
            });
    if (!validated_receipt) {
        rev3_storage_error_as_cpp("row checksum disagrees");
    }
    return Rev3LoadedPgRow{
        .query_key = projected.query_key,
        .address = projected.address,
        .chunk_bytes = projected.chunk_bytes,
        .checksum = projected.checksum,
        .bytes = projected.bytes,
        .validated_receipt = std::move(*validated_receipt),
    };
}

using Rev3ValidatedPgRowReceiptBatch = std::array<
    std::optional<ValidatedMinimizerGenerationRowReceipt>,
    kRev3FullGenerationPinnedBatchRows>;

inline constexpr std::size_t kRev3ParallelRowAuthenticationMinRows = 32u;
inline constexpr std::size_t kRev3ParallelRowAuthenticationGrainRows = 8u;

/// Destroy every row authority before its pinned slot can be advanced.
inline void clear_rev3_validated_pg_row_receipt_batch(
        Rev3ValidatedPgRowReceiptBatch& receipts) noexcept {
    for (auto& receipt : receipts) {
        receipt.reset();
    }
}

/// Authenticate one already-projected pinned batch. Only immutable payload
/// spans and storage-neutral CRC code enter TBB; no PostgreSQL API or callback
/// is invoked off the backend thread. The call joins before returning, so all
/// borrowed spans remain inside the lifetime of their pinned tuple slots.
inline void authenticate_rev3_projected_pg_row_batch(
        const Rev3PinnedProjectedPgRowBatch& batch,
        Rev3ValidatedPgRowReceiptBatch& receipts) {
    clear_rev3_validated_pg_row_receipt_batch(receipts);
    const auto authenticate_range =
        [&batch, &receipts](
                std::size_t begin,
                std::size_t end) noexcept {
            for (std::size_t ordinal = begin;
                 ordinal < end; ++ordinal) {
                const Rev3ProjectedPgRow& projected =
                    batch.rows[ordinal].projected;
                if (projected.error != nullptr) continue;
                auto validated =
                    bioseq::indexing::
                        validate_minimizer_generation_row_receipt(
                            MinimizerGenerationRowView{
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

    auto& pool = bioseq::parallelism::GlobalPool::instance();
    const bool use_parallel_authentication =
        batch.size >= kRev3ParallelRowAuthenticationMinRows
        && pool.active()
        && bioseq::parallelism::task_parallel(pool.mode())
        && pool.max_concurrency() > 1u;
    if (use_parallel_authentication) {
        pool.parallel_for(
            0u, batch.size,
            kRev3ParallelRowAuthenticationGrainRows,
            bioseq::parallelism::range_body{
                authenticate_range});
    } else {
        authenticate_range(0u, batch.size);
    }
}

/// Transfer one pre-authenticated row authority into the callback-scoped
/// loaded-row form without repeating its address-bound CRC pass.
[[nodiscard]] inline Rev3LoadedPgRow
load_rev3_authenticated_projected_pg_row_or_error(
        const Rev3ProjectedPgRow& projected,
        std::optional<ValidatedMinimizerGenerationRowReceipt>&
            validated_receipt) {
    if (projected.error != nullptr) {
        rev3_storage_error_as_cpp(projected.error);
    }
    if (!validated_receipt) {
        rev3_storage_error_as_cpp("row checksum disagrees");
    }
    bool exact_authority = false;
    const bool visited =
        bioseq::indexing::visit_validated_minimizer_generation_row(
            *validated_receipt,
            [&projected, &exact_authority](
                    const IndexChunkAddress& address,
                    std::span<const std::byte> bytes,
                    std::uint64_t row_checksum) noexcept {
                exact_authority =
                    address == projected.address
                    && bytes.data() == projected.bytes.data()
                    && bytes.size() == projected.bytes.size()
                    && row_checksum == projected.checksum;
            });
    if (!visited || !exact_authority) {
        rev3_storage_error_as_cpp(
            "row checksum authority disagrees");
    }
    return Rev3LoadedPgRow{
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
[[nodiscard]] inline Rev3LoadedPgRow load_rev3_deformed_pg_row_or_error(
        const Rev3DeformedPgRow& deformed) {
    return load_rev3_projected_pg_row_or_error(
        project_rev3_deformed_pg_row(deformed));
}

/// General single-row loader.  Its hot-path PostgreSQL work is one deformation
/// bridge regardless of the eight-column schema width.
[[nodiscard]] inline Rev3LoadedPgRow load_rev3_pg_row_or_error(
        Relation relation,
        HeapTuple tuple) {
    const Rev3DeformedPgRow deformed =
        deform_rev3_pg_row_or_error(relation, tuple);
    return load_rev3_deformed_pg_row_or_error(deformed);
}

// -------------------------------------------------------------------------
// Bounded transaction-owned generation file actions.
//
// This remains dark until the v2 image writer and descriptor catalog publish
// through it.  Keeping it separate prevents the v1 bridge's replace-by-rename
// lifetime from becoming an accidental compatibility contract.
// -------------------------------------------------------------------------

inline constexpr std::size_t kPendingFileActionCapacity = 4096u;
inline constexpr std::size_t kFileActionDiagnosticCapacity = 256u;
inline constexpr std::size_t kMaximumReaperFileActions = 64u;
inline constexpr std::uint16_t kNoPairedFileAction = UINT16_MAX;
inline constexpr std::uint8_t kMappedMpfIndexKind = 1u;
inline constexpr std::string_view kGenerationFileDirectoryName =
    "bioseq_mpf5";

enum class FileAction : std::uint8_t {
    unlink_on_abort,
    unlink_on_commit,
};

/// Callback-safe file authority.  The queue stores no owning C++ object and
/// no pointer into a PostgreSQL memory context.
struct PendingFileAction {
    FileAction action{};
    SubTransactionId owner_subxid{};
    dev_t expected_device{};
    ino_t expected_inode{};
    std::uint16_t paired_slot = kNoPairedFileAction;
    bool inode_valid = false;
    bool armed = false;
    char path[MAXPGPATH]{};
};

static_assert(std::is_trivially_copyable_v<PendingFileAction>);
static_assert(std::is_standard_layout_v<PendingFileAction>);

enum class FileActionDiagnosticCode : std::uint8_t {
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

struct FileActionDiagnostic {
    std::uint64_t sequence = 0u;
    std::uint16_t slot = kNoPairedFileAction;
    FileActionDiagnosticCode code = FileActionDiagnosticCode::none;
    int saved_errno = 0;
};

struct FileActionDiagnosticsSnapshot {
    std::array<FileActionDiagnostic, kFileActionDiagnosticCapacity> entries{};
    std::uint64_t total = 0u;
};

/// Opaque all-or-nothing reservation.  Slots are found by reservation id
/// rather than assumed contiguous because savepoint completion can leave
/// holes in the fixed queue.
struct FileActionReservation {
    std::uint32_t id = 0u;
    std::uint16_t count = 0u;
};

struct GenerationFileIdentity {
    Oid database_oid = InvalidOid;
    Oid tablespace_oid = InvalidOid;
    Oid main_relfilenode = InvalidOid;
    Oid user_relid = InvalidOid;
    std::int16_t column_attno = 0;
    std::uint8_t index_kind = kMappedMpfIndexKind;
    std::uint64_t query_key = 0u;
    std::uint64_t generation_id = 0u;
};

/// Fixed owned buffers let callbacks consume the exact backend-resolved path
/// without catalog access, formatting, allocation or a memory-context pointer.
struct ResolvedGenerationFilePath {
    std::array<char, MAXPGPATH> database_directory{};
    std::array<char, MAXPGPATH> bioseq_directory{};
    std::array<char, MAXPGPATH> shard_directory{};
    std::array<char, MAXPGPATH> final_path{};
};

struct PendingGenerationFilePublication {
    FileActionReservation reservation{};
    std::uint16_t temporary_slot = kNoPairedFileAction;
    std::uint16_t final_slot = kNoPairedFileAction;
    int fd = -1;
    dev_t expected_device{};
    ino_t expected_inode{};
    std::array<char, MAXPGPATH> shard_directory{};
    std::array<char, MAXPGPATH> temporary_path{};
    std::array<char, MAXPGPATH> final_path{};
    bool published = false;

    PendingGenerationFilePublication() = default;
    PendingGenerationFilePublication(
            const PendingGenerationFilePublication&) = delete;
    PendingGenerationFilePublication& operator=(
            const PendingGenerationFilePublication&) = delete;
    PendingGenerationFilePublication(
            PendingGenerationFilePublication&& other) noexcept
        : reservation(std::exchange(
              other.reservation, FileActionReservation{})),
          temporary_slot(std::exchange(
              other.temporary_slot, kNoPairedFileAction)),
          final_slot(std::exchange(
              other.final_slot, kNoPairedFileAction)),
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
    PendingGenerationFilePublication& operator=(
            PendingGenerationFilePublication&&) = delete;
    ~PendingGenerationFilePublication();
};

namespace generation_file_action_detail {

enum class SlotState : std::uint8_t {
    free,
    reserved,
    armed,
};

struct QueueState {
    std::array<PendingFileAction, kPendingFileActionCapacity> actions{};
    std::array<SlotState, kPendingFileActionCapacity> slot_states{};
    std::array<std::uint32_t, kPendingFileActionCapacity> reservation_ids{};
    std::array<FileActionDiagnostic,
        kFileActionDiagnosticCapacity> diagnostics{};
    std::uint64_t diagnostic_total = 0u;
    std::uint32_t next_reservation_id = 1u;
    bool xact_callback_installed = false;
    bool subxact_callback_installed = false;
};

// The extension image is private to one PostgreSQL backend process.
inline QueueState g_queue_state{};

static_assert(std::tuple_size_v<decltype(QueueState::actions)>
    == kPendingFileActionCapacity);
static_assert(std::tuple_size_v<decltype(QueueState::diagnostics)>
    == kFileActionDiagnosticCapacity);

[[nodiscard]] inline bool slot_is_occupied(
        const std::size_t slot) noexcept {
    return g_queue_state.slot_states[slot] != SlotState::free;
}

inline void record_diagnostic(
        const std::uint16_t slot,
        const FileActionDiagnosticCode code,
        const int saved_errno = 0) noexcept {
    const std::uint64_t sequence = g_queue_state.diagnostic_total++;
    FileActionDiagnostic& diagnostic =
        g_queue_state.diagnostics[
            sequence % kFileActionDiagnosticCapacity];
    diagnostic = FileActionDiagnostic{
        .sequence = sequence + 1u,
        .slot = slot,
        .code = code,
        .saved_errno = saved_errno,
    };
}

inline void release_slot(const std::size_t slot) noexcept {
    g_queue_state.actions[slot] = PendingFileAction{};
    g_queue_state.slot_states[slot] = SlotState::free;
    g_queue_state.reservation_ids[slot] = 0u;
}

[[nodiscard]] inline bool reservation_id_is_live(
        const std::uint32_t reservation_id) noexcept {
    for (std::size_t slot = 0u; slot < kPendingFileActionCapacity;
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
    rev3_storage_error_as_cpp(
        "generation file-action reservation id space is exhausted");
}

[[nodiscard]] inline std::optional<std::uint16_t> reservation_slot(
        const FileActionReservation reservation,
        const std::size_t ordinal) noexcept {
    if (reservation.id == 0u || ordinal >= reservation.count) {
        return std::nullopt;
    }
    std::size_t seen = 0u;
    for (std::size_t slot = 0u; slot < kPendingFileActionCapacity;
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
        const PendingFileAction& action) noexcept {
    return action.inode_valid
        && observed.st_dev == action.expected_device
        && observed.st_ino == action.expected_inode;
}

#ifdef BIOSEQ_TEST_INDEXES
enum class CallbackFailureInjection : std::uint8_t {
    none,
    stat_failed,
    identity_mismatch,
    unlink_failed,
};
inline CallbackFailureInjection g_callback_failure_injection =
    CallbackFailureInjection::none;
#endif

/// The sole callback filesystem mutation.  It cannot allocate, log or raise;
/// failures enter the fixed ring for later backend-thread reporting.
inline void unlink_identity_bound_noexcept(
        const std::uint16_t slot) noexcept {
    PendingFileAction& action = g_queue_state.actions[slot];
    if (!action.armed || !action.inode_valid || action.path[0] == '\0') {
        record_diagnostic(
            slot, FileActionDiagnosticCode::missing_inode_authority);
        return;
    }
#ifdef BIOSEQ_TEST_INDEXES
    if (g_callback_failure_injection
        == CallbackFailureInjection::stat_failed) {
        g_callback_failure_injection = CallbackFailureInjection::none;
        record_diagnostic(
            slot, FileActionDiagnosticCode::stat_failed, EIO);
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
                ? FileActionDiagnosticCode::already_missing
                : FileActionDiagnosticCode::stat_failed,
            saved_errno == ENOENT ? 0 : saved_errno);
        return;
    }
    if (!S_ISREG(observed.st_mode)) {
        record_diagnostic(slot, FileActionDiagnosticCode::not_regular);
        return;
    }
#ifdef BIOSEQ_TEST_INDEXES
    if (g_callback_failure_injection
        == CallbackFailureInjection::identity_mismatch) {
        g_callback_failure_injection = CallbackFailureInjection::none;
        record_diagnostic(
            slot, FileActionDiagnosticCode::identity_mismatch);
        return;
    }
#endif
    if (!same_inode(observed, action)) {
        record_diagnostic(
            slot, FileActionDiagnosticCode::identity_mismatch);
        return;
    }
    if (observed.st_nlink == 2) {
        if (action.paired_slot >= kPendingFileActionCapacity
            || action.paired_slot == slot) {
            record_diagnostic(
                slot, FileActionDiagnosticCode::paired_slot_invalid);
            return;
        }
        const PendingFileAction& paired =
            g_queue_state.actions[action.paired_slot];
        if (g_queue_state.slot_states[action.paired_slot]
                != SlotState::armed
            || !paired.armed || !paired.inode_valid
            || paired.path[0] == '\0') {
            record_diagnostic(
                slot, FileActionDiagnosticCode::paired_slot_invalid);
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
                FileActionDiagnosticCode::paired_identity_mismatch,
                errno);
            return;
        }
    } else if (observed.st_nlink != 1) {
        record_diagnostic(
            slot, FileActionDiagnosticCode::link_count_mismatch);
        return;
    }
#ifdef BIOSEQ_TEST_INDEXES
    if (g_callback_failure_injection
        == CallbackFailureInjection::unlink_failed) {
        g_callback_failure_injection = CallbackFailureInjection::none;
        record_diagnostic(
            slot, FileActionDiagnosticCode::unlink_failed, EIO);
        return;
    }
#endif
    if (unlink(action.path) != 0) {
        const int saved_errno = errno;
        record_diagnostic(
            slot,
            saved_errno == ENOENT
                ? FileActionDiagnosticCode::already_missing
                : FileActionDiagnosticCode::unlink_failed,
            saved_errno == ENOENT ? 0 : saved_errno);
        return;
    }
    record_diagnostic(slot, FileActionDiagnosticCode::unlinked);
}

inline void finish_top_transaction(const bool committed) noexcept {
    for (std::size_t slot = 0u; slot < kPendingFileActionCapacity;
         ++slot) {
        if (!slot_is_occupied(slot)) continue;
        const PendingFileAction& action = g_queue_state.actions[slot];
        if (g_queue_state.slot_states[slot] == SlotState::armed
            && action.armed
            && ((committed
                    && action.action == FileAction::unlink_on_commit)
                || (!committed
                    && action.action == FileAction::unlink_on_abort))) {
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
    for (std::size_t slot = 0u; slot < kPendingFileActionCapacity;
         ++slot) {
        if (!slot_is_occupied(slot)
            || g_queue_state.actions[slot].owner_subxid != subxid) {
            continue;
        }
        PendingFileAction& action = g_queue_state.actions[slot];
        if (committed) {
            action.owner_subxid = parent_subxid;
            continue;
        }
        if (g_queue_state.slot_states[slot] == SlotState::armed
            && action.armed
            && action.action == FileAction::unlink_on_abort) {
            unlink_identity_bound_noexcept(
                static_cast<std::uint16_t>(slot));
        }
        release_slot(slot);
    }
}

[[nodiscard]] inline bool has_occupied_slot() noexcept {
    for (std::size_t slot = 0u; slot < kPendingFileActionCapacity;
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
                    kNoPairedFileAction,
                    FileActionDiagnosticCode::prepare_rejected);
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
        bioseq::pg_utils::invoke_pg_as_cpp([] {
            RegisterXactCallback(xact_callback, nullptr);
        });
        g_queue_state.xact_callback_installed = true;
    }
    if (!g_queue_state.subxact_callback_installed) {
        bioseq::pg_utils::invoke_pg_as_cpp([] {
            RegisterSubXactCallback(subxact_callback, nullptr);
        });
        g_queue_state.subxact_callback_installed = true;
    }
}

template <std::size_t BufferBytes, typename... Args>
[[nodiscard]] inline bool format_fixed(
        std::array<char, BufferBytes>& destination,
        fmt::format_string<Args...> format,
        Args&&... args) {
    static_assert(BufferBytes > 1u);
    destination.fill('\0');
    const auto formatted = fmt::format_to_n(
        destination.data(), BufferBytes - 1u, format,
        std::forward<Args>(args)...);
    if (formatted.size >= BufferBytes) {
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
            rev3_storage_error_as_cpp(fmt::format(
                "generation file directory preflight failed: errno={}",
                errno));
        }
        if (mkdir(path, 0700) == 0) {
            created = true;
        } else if (errno != EEXIST) {
            rev3_storage_error_as_cpp(fmt::format(
                "generation file directory creation failed: errno={}",
                errno));
        }
        if (lstat(path, &observed) != 0) {
            rev3_storage_error_as_cpp(fmt::format(
                "generation file directory stat failed: errno={}",
                errno));
        }
    }
    if (!S_ISDIR(observed.st_mode)
        || observed.st_uid != geteuid()
        || (observed.st_mode & 07777) != 0700) {
        rev3_storage_error_as_cpp(
            "generation file directory is not server-owned mode-0700");
    }
    return created;
}

inline void arm_reserved_slot(
        const FileActionReservation reservation,
        const std::size_t ordinal,
        const FileAction action,
        const char* path,
        const dev_t expected_device,
        const ino_t expected_inode,
        const std::uint16_t paired_slot) {
    const std::optional<std::uint16_t> maybe_slot =
        reservation_slot(reservation, ordinal);
    if (!maybe_slot) {
        rev3_storage_error_as_cpp(
            "generation file-action reservation slot is missing");
    }
    const std::uint16_t slot = *maybe_slot;
    if (g_queue_state.slot_states[slot] != SlotState::reserved
        || path == nullptr) {
        rev3_storage_error_as_cpp(
            "generation file-action reservation cannot be armed");
    }
    const std::size_t path_bytes = strnlen(path, MAXPGPATH);
    if (path_bytes == 0u || path_bytes >= MAXPGPATH) {
        rev3_storage_error_as_cpp(
            "generation file-action path is empty or truncated");
    }
    PendingFileAction& pending = g_queue_state.actions[slot];
    pending = PendingFileAction{
        .action = action,
        .owner_subxid = GetCurrentSubTransactionId(),
        .expected_device = expected_device,
        .expected_inode = expected_inode,
        .paired_slot = paired_slot,
        .inode_valid = true,
        .armed = true,
    };
    std::memcpy(pending.path, path, path_bytes + 1u);
    g_queue_state.slot_states[slot] = SlotState::armed;
}

inline void fsync_directory(const char* path) {
    const int fd = open(
        path, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0) {
        rev3_storage_error_as_cpp(fmt::format(
            "generation file directory open failed: errno={}", errno));
    }
    const int sync_result = pg_fsync(fd);
    const int sync_errno = errno;
    const int close_result = close(fd);
    if (sync_result != 0 || close_result != 0) {
        rev3_storage_error_as_cpp(fmt::format(
            "generation file directory fsync/close failed: errno={}",
            sync_result != 0 ? sync_errno : errno));
    }
}

} // namespace generation_file_action_detail

[[nodiscard]] inline FileActionReservation reserve_file_actions(
        const std::size_t requested_count) {
    using namespace generation_file_action_detail;
    if (requested_count == 0u
        || requested_count > kPendingFileActionCapacity) {
        rev3_storage_error_as_cpp(
            "generation file-action reservation size is invalid");
    }
    install_callbacks();
    std::size_t available = 0u;
    for (std::size_t slot = 0u; slot < kPendingFileActionCapacity;
         ++slot) {
        available += slot_is_occupied(slot) ? 0u : 1u;
    }
    if (available < requested_count) {
        rev3_storage_error_as_cpp(
            "generation file-action queue capacity exhausted before mutation");
    }

    // After the complete preflight this pass cannot fail: it marks exactly
    // the requested number without allocation or partial-return behavior.
    const std::uint32_t reservation_id = next_unused_reservation_id();
    std::size_t reserved = 0u;
    for (std::size_t slot = 0u;
         slot < kPendingFileActionCapacity && reserved < requested_count;
         ++slot) {
        if (slot_is_occupied(slot)) continue;
        g_queue_state.actions[slot] = PendingFileAction{
            .owner_subxid = GetCurrentSubTransactionId(),
        };
        g_queue_state.slot_states[slot] = SlotState::reserved;
        g_queue_state.reservation_ids[slot] = reservation_id;
        ++reserved;
    }
    return FileActionReservation{
        .id = reservation_id,
        .count = static_cast<std::uint16_t>(requested_count),
    };
}

[[nodiscard]] inline FileActionReservation reserve_reaper_file_actions(
        const std::size_t requested_count) {
    if (requested_count == 0u
        || requested_count > kMaximumReaperFileActions) {
        rev3_storage_error_as_cpp(
            "generation reaper file-action batch exceeds hard cap 64");
    }
    return reserve_file_actions(requested_count);
}

inline void release_unarmed_file_action_reservation(
        const FileActionReservation reservation) noexcept {
    using namespace generation_file_action_detail;
    for (std::size_t slot = 0u; slot < kPendingFileActionCapacity;
         ++slot) {
        if (g_queue_state.slot_states[slot] == SlotState::reserved
            && g_queue_state.reservation_ids[slot] == reservation.id) {
            release_slot(slot);
        }
    }
}

inline void cancel_generation_file_publication_noexcept(
        PendingGenerationFilePublication& publication) noexcept {
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
    for (std::size_t slot = 0u; slot < kPendingFileActionCapacity;
         ++slot) {
        if (g_queue_state.reservation_ids[slot]
                == publication.reservation.id
            && g_queue_state.slot_states[slot] == SlotState::armed) {
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
    for (std::size_t slot = 0u; slot < kPendingFileActionCapacity;
         ++slot) {
        if (g_queue_state.reservation_ids[slot]
            != publication.reservation.id) {
            continue;
        }
        if (g_queue_state.slot_states[slot] == SlotState::armed
            && g_queue_state.actions[slot].armed
            && g_queue_state.actions[slot].action
                == FileAction::unlink_on_abort) {
            unlink_identity_bound_noexcept(
                static_cast<std::uint16_t>(slot));
        }
        release_slot(slot);
    }
    publication.reservation = {};
    publication.temporary_slot = kNoPairedFileAction;
    publication.final_slot = kNoPairedFileAction;
    publication.temporary_path.fill('\0');
    publication.final_path.fill('\0');
    publication.published = true;
}

inline PendingGenerationFilePublication::~PendingGenerationFilePublication() {
    cancel_generation_file_publication_noexcept(*this);
}

[[nodiscard]] inline ResolvedGenerationFilePath
resolve_generation_file_path(
        const GenerationFileIdentity& identity) {
    using namespace generation_file_action_detail;
    if (!OidIsValid(identity.database_oid)
        || !OidIsValid(identity.tablespace_oid)
        || !OidIsValid(identity.main_relfilenode)
        || !OidIsValid(identity.user_relid)
        || identity.column_attno <= 0
        || identity.index_kind != kMappedMpfIndexKind
        || identity.generation_id == 0u) {
        rev3_storage_error_as_cpp(
            "generation file identity is invalid or is not mapped MPF");
    }

    ResolvedGenerationFilePath resolved{};
    bioseq::pg_utils::invoke_pg_as_cpp(
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
        rev3_storage_error_as_cpp(
            "generation database path is not canonical");
    }
    if (!format_fixed(
            resolved.bioseq_directory, "{}/{}",
            database_directory, kGenerationFileDirectoryName)) {
        rev3_storage_error_as_cpp(
            "generation Bioseq directory path is truncated");
    }
    constexpr std::array<char, 16u> kLowerHex{
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    const std::uint8_t shard =
        static_cast<std::uint8_t>(identity.generation_id & 0xffu);
    const std::array<char, 3u> shard_name{
        kLowerHex[shard >> 4u], kLowerHex[shard & 0x0fu], '\0'};
    if (!format_fixed(
            resolved.shard_directory, "{}/{}",
            resolved.bioseq_directory.data(), shard_name.data())) {
        rev3_storage_error_as_cpp(
            "generation shard directory path is truncated");
    }
    if (!format_fixed(
            resolved.final_path,
            "{}/{}.{}.{}.1.{}.{}.mpf5",
            resolved.shard_directory.data(),
            static_cast<unsigned>(identity.main_relfilenode),
            static_cast<unsigned>(identity.user_relid),
            static_cast<int>(identity.column_attno),
            static_cast<unsigned long long>(identity.query_key),
            static_cast<unsigned long long>(identity.generation_id))) {
        rev3_storage_error_as_cpp(
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
        rev3_storage_error_as_cpp(
            "generation file path escapes its canonical database shard");
    }
    return resolved;
}

/// Reserve temp/final before the first filesystem mutation and then create a
/// cryptographically unique strict mode-0600 O_EXCL temp.
[[nodiscard]] inline PendingGenerationFilePublication
begin_generation_file_publication(
        const GenerationFileIdentity& identity) {
    using namespace generation_file_action_detail;
    const ResolvedGenerationFilePath resolved =
        resolve_generation_file_path(identity);
    const FileActionReservation reservation = reserve_file_actions(2u);
    const auto temporary_slot = reservation_slot(reservation, 0u);
    const auto final_slot = reservation_slot(reservation, 1u);
    if (!temporary_slot || !final_slot) {
        rev3_storage_error_as_cpp(
            "generation publication pair reservation is incomplete");
    }

    PendingGenerationFilePublication publication;
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
    constexpr std::size_t kNonceBytes = 16u;
    constexpr std::size_t kMaximumCollisionRetries = 128u;
    constexpr std::array<char, 16u> kLowerHex{
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    const std::uint64_t top_full_xid =
        bioseq::pg_utils::invoke_pg_as_cpp([] {
            return U64FromFullTransactionId(
                GetTopFullTransactionId());
        });
    for (std::size_t attempt = 0u;
         attempt < kMaximumCollisionRetries; ++attempt) {
        std::array<unsigned char, kNonceBytes> nonce{};
        if (!pg_strong_random(nonce.data(), nonce.size())) {
            rev3_storage_error_as_cpp(
                "generation temp nonce source failed");
        }
        std::array<char, kNonceBytes * 2u + 1u> nonce_hex{};
        for (std::size_t byte_index = 0u; byte_index < nonce.size();
             ++byte_index) {
            nonce_hex[byte_index * 2u] =
                kLowerHex[nonce[byte_index] >> 4u];
            nonce_hex[byte_index * 2u + 1u] =
                kLowerHex[nonce[byte_index] & 0x0fu];
        }
        if (!format_fixed(
                publication.temporary_path,
                "{}.tmp.{}.{}.{}",
                publication.final_path.data(),
                static_cast<unsigned long long>(top_full_xid),
                static_cast<int>(MyProcPid), nonce_hex.data())) {
            rev3_storage_error_as_cpp(
                "generation temporary path is truncated");
        }
        publication.fd = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            return OpenTransientFilePerm(
                publication.temporary_path.data(),
                O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW
                    | PG_BINARY,
                S_IRUSR | S_IWUSR);
        });
        if (publication.fd >= 0) break;
        if (errno != EEXIST) {
            rev3_storage_error_as_cpp(fmt::format(
                "generation O_EXCL temp creation failed: errno={}",
                errno));
        }
    }
    if (publication.fd < 0) {
        rev3_storage_error_as_cpp(
            "generation O_EXCL temp nonce collisions exceeded retry bound");
    }
    struct stat observed{};
    if (fstat(publication.fd, &observed) != 0
        || !S_ISREG(observed.st_mode)
        || observed.st_uid != geteuid()
        || (observed.st_mode & 07777) != 0600
        || observed.st_nlink != 1) {
        rev3_storage_error_as_cpp(
            "generation temp inode contract is invalid");
    }
    publication.expected_device = observed.st_dev;
    publication.expected_inode = observed.st_ino;
    arm_reserved_slot(
        reservation, 0u, FileAction::unlink_on_abort,
        publication.temporary_path.data(), observed.st_dev,
        observed.st_ino, publication.final_slot);
    return publication;
}

/// Freeze, link without replacement, validate and directory-fsync a completed
/// header-last image.  The caller finishes payload/header writes and image
/// validation before entering this function.
inline void publish_generation_file(
        PendingGenerationFilePublication& publication) {
    using namespace generation_file_action_detail;
    if (publication.fd < 0
        || publication.temporary_slot == kNoPairedFileAction
        || publication.final_slot == kNoPairedFileAction) {
        rev3_storage_error_as_cpp(
            "generation publication is not open");
    }
    if (fchmod(publication.fd, S_IRUSR) != 0
        || pg_fsync(publication.fd) != 0) {
        rev3_storage_error_as_cpp(fmt::format(
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
        rev3_storage_error_as_cpp(
            "generation frozen temp inode contract is invalid");
    }
    arm_reserved_slot(
        publication.reservation, 1u,
        FileAction::unlink_on_abort,
        publication.final_path.data(),
        publication.expected_device, publication.expected_inode,
        publication.temporary_slot);
    if (linkat(
            AT_FDCWD, publication.temporary_path.data(),
            AT_FDCWD, publication.final_path.data(), 0) != 0) {
        // EEXIST is intentionally not special: no existing final is ever
        // overwritten, renamed aside or pre-unlinked.
        rev3_storage_error_as_cpp(fmt::format(
            "generation immutable final link failed without replacement: errno={}",
            errno));
    }
    if (unlink(publication.temporary_path.data()) != 0) {
        // Both abort actions stay armed and paired.  Callback cleanup accepts
        // precisely this two-link window and refuses every unrelated link.
        rev3_storage_error_as_cpp(fmt::format(
            "generation strict temp unlink failed: errno={}", errno));
    }
    release_slot(publication.temporary_slot);
    PendingFileAction& final_action =
        g_queue_state.actions[publication.final_slot];
    final_action.paired_slot = kNoPairedFileAction;

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
        rev3_storage_error_as_cpp(
            "generation immutable final inode contract is invalid");
    }
    fsync_directory(publication.shard_directory.data());
    if (CloseTransientFile(publication.fd) != 0) {
        publication.fd = -1;
        rev3_storage_error_as_cpp(fmt::format(
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
        const FileActionReservation reservation,
        const std::size_t ordinal,
        const GenerationFileIdentity& identity) {
    using namespace generation_file_action_detail;
    // Never accept caller-assembled path authority.  Recompute the canonical
    // database/shard/final path from the persisted physical identity.
    const ResolvedGenerationFilePath resolved =
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
        rev3_storage_error_as_cpp(
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
        rev3_storage_error_as_cpp(
            "generation commit-unlink inode contract is invalid");
    }
    arm_reserved_slot(
        reservation, ordinal, FileAction::unlink_on_commit,
        resolved.final_path.data(), observed.st_dev, observed.st_ino,
        kNoPairedFileAction);
}

[[nodiscard]] inline FileActionDiagnosticsSnapshot
generation_file_action_diagnostics() noexcept {
    return FileActionDiagnosticsSnapshot{
        .entries = generation_file_action_detail::g_queue_state.diagnostics,
        .total =
            generation_file_action_detail::g_queue_state.diagnostic_total,
    };
}

#ifdef BIOSEQ_TEST_INDEXES
struct FileActionQueueTestSnapshot {
    std::size_t reserved = 0u;
    std::size_t armed_abort = 0u;
    std::size_t armed_commit = 0u;
};

[[nodiscard]] inline FileActionQueueTestSnapshot
generation_file_action_queue_test_snapshot() noexcept {
    using namespace generation_file_action_detail;
    FileActionQueueTestSnapshot snapshot{};
    for (std::size_t slot = 0u; slot < kPendingFileActionCapacity;
         ++slot) {
        if (g_queue_state.slot_states[slot] == SlotState::reserved) {
            ++snapshot.reserved;
        } else if (g_queue_state.slot_states[slot] == SlotState::armed) {
            if (g_queue_state.actions[slot].action
                == FileAction::unlink_on_abort) {
                ++snapshot.armed_abort;
            } else {
                ++snapshot.armed_commit;
            }
        }
    }
    return snapshot;
}

inline void arm_generation_file_callback_failure(
        const generation_file_action_detail::CallbackFailureInjection
            injection) noexcept {
    generation_file_action_detail::g_callback_failure_injection = injection;
}

inline void reset_generation_file_action_queue_for_test() noexcept {
    using namespace generation_file_action_detail;
    const bool xact_callback_installed =
        g_queue_state.xact_callback_installed;
    const bool subxact_callback_installed =
        g_queue_state.subxact_callback_installed;
    g_queue_state = QueueState{};
    g_queue_state.xact_callback_installed = xact_callback_installed;
    g_queue_state.subxact_callback_installed =
        subxact_callback_installed;
}
#endif

namespace mpf5_image_pg_detail {

[[nodiscard]] inline GenerationFileIdentity generation_file_identity(
        const Rev3IndexAdministrationIdentity& identity,
        const std::uint64_t query_key,
        const std::uint64_t generation_id) {
    if (identity.index_name != MINIMIZERS_PER_FRAGMENT_INDEX_NAME
        || identity.database_oid != MyDatabaseId
        || identity.column_attno <= 0) {
        rev3_storage_error_as_cpp(
            "MPF5 v2 file identity is not the current MPF relation");
    }
    return GenerationFileIdentity{
        .database_oid = identity.database_oid,
        .tablespace_oid = identity.tablespace_oid,
        .main_relfilenode = identity.main_relfilenode,
        .user_relid = identity.user_relid,
        .column_attno = identity.column_attno,
        .index_kind = kMappedMpfIndexKind,
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
            rev3_storage_error_as_cpp("MPF5 v2 write offset overflows");
        }
        const std::uint64_t current = offset + written;
        if (current > static_cast<std::uint64_t>(
                (std::numeric_limits<off_t>::max)())) {
            rev3_storage_error_as_cpp(
                "MPF5 v2 write offset exceeds off_t");
        }
        const ssize_t result = pg_pwrite(
            fd,
            bytes.data() + static_cast<std::ptrdiff_t>(written),
            bytes.size() - written,
            static_cast<off_t>(current));
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) {
            rev3_storage_error_as_cpp(fmt::format(
                "MPF5 v2 write failed: errno={}", errno));
        }
        written += static_cast<std::size_t>(result);
    }
}

inline void write_zero_padding(
        const int fd,
        const std::uint64_t begin,
        const std::uint64_t end) {
    static constexpr std::array<std::byte, 64u> zeros{};
    if (end < begin || end - begin > zeros.size()) {
        rev3_storage_error_as_cpp("MPF5 v2 padding geometry is invalid");
    }
    pwrite_all(
        fd,
        std::span<const std::byte>{zeros}.first(
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
        rev3_storage_error_as_cpp(
            "MPF5 v2 image length is not mmap/off_t representable");
    }
    while (::ftruncate(fd, static_cast<off_t>(image_bytes)) != 0) {
        if (errno == EINTR) continue;
        rev3_storage_error_as_cpp(fmt::format(
            "MPF5 v2 image resize failed: errno={}", errno));
    }
}

/// Scope-bound offset-zero shared mapping. Offset zero is page-aligned on
/// every supported PostgreSQL host, while callers expose only exact sections.
class ScopedMappedRegion {
private:
    void* mapping_ = MAP_FAILED;
    std::size_t bytes_ = 0u;

public:
    ScopedMappedRegion(
            const int fd,
            const std::uint64_t bytes,
            const int protection)
        : bytes_(static_cast<std::size_t>(bytes)) {
        if (fd < 0 || bytes == 0u
            || bytes > static_cast<std::uint64_t>(
                (std::numeric_limits<std::size_t>::max)())) {
            rev3_storage_error_as_cpp(
                "MPF5 v2 mapping geometry is invalid");
        }
        mapping_ = mmap(
            nullptr, bytes_, protection, MAP_SHARED, fd, 0);
        if (mapping_ == MAP_FAILED) {
            rev3_storage_error_as_cpp(fmt::format(
                "MPF5 v2 mmap failed: errno={}", errno));
        }
    }

    ScopedMappedRegion(const ScopedMappedRegion&) = delete;
    ScopedMappedRegion& operator=(const ScopedMappedRegion&) = delete;
    ScopedMappedRegion(ScopedMappedRegion&&) = delete;
    ScopedMappedRegion& operator=(ScopedMappedRegion&&) = delete;

    ~ScopedMappedRegion() {
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
            rev3_storage_error_as_cpp(fmt::format(
                "MPF5 v2 mapped image sync failed: errno={}", errno));
        }
    }
};

[[nodiscard]] inline bioseq::indexing::Mpf5V2ExpectedIdentity
expected_identity(
        const GenerationFileIdentity& identity,
        const bioseq::indexing::Mpf5V2Provider provider,
        const MinimizerGenerationHead& head,
        const MinimizerGenerationManifest& manifest,
        const bioseq::indexing::Mpf5V2ImageLayout& layout,
        const std::uint64_t header_crc64) noexcept {
    return bioseq::indexing::Mpf5V2ExpectedIdentity{
        .database_oid = identity.database_oid,
        .tablespace_oid = identity.tablespace_oid,
        .main_relfilenode = identity.main_relfilenode,
        .user_relid = identity.user_relid,
        .column_attno = static_cast<std::uint16_t>(identity.column_attno),
        .index_kind = bioseq::indexing::kMpf5V2IndexKind,
        .provider = static_cast<std::uint8_t>(provider),
        .wire_format = bioseq::indexing::kMpf5V2WireFormat,
        .query_key = identity.query_key,
        .generation_id = identity.generation_id,
        .source_identity = head.source_identity,
        .source_content_crc = head.source_content_checksum,
        .compatibility_crc = head.compatibility_checksum,
        .target_length = manifest.target_length,
        .file_bytes = layout.file_bytes,
        .position_segment_count = manifest.position_segment_count,
        .position_segment_span_bp = manifest.position_segment_span_bp,
        .payload = layout.payload,
        .directory = layout.directory,
        .manifest = layout.manifest,
        .header_crc64 = header_crc64,
        .feature_bitmap = layout.feature_bitmap,
        .compact_h32_direct = layout.compact_h32_direct,
    };
}

inline void validate_generation_metadata(
        const GenerationFileIdentity& identity,
        const MinimizerGenerationHead& head,
        const MinimizerGenerationManifest& manifest,
        const std::span<const std::byte> directory,
        const std::span<const std::byte> encoded_manifest) {
    if (manifest.wire_format
            != bioseq::indexing::MinimizerGenerationWireFormat::
                grouped_shape_soa_segment_local
        || static_cast<std::uint16_t>(manifest.wire_format)
            != bioseq::indexing::kMpf5V2WireFormat
        || identity.generation_id == 0u
        || identity.generation_id != head.generation_id
        || identity.generation_id != manifest.generation_id
        || head.source_identity != manifest.source_identity
        || head.source_content_checksum
            != manifest.source_content_checksum
        || head.compatibility_checksum
            != bioseq::indexing::minimizer_generation_compatibility_checksum(
                manifest)
        || directory.size() != manifest.directory_bytes
        || bioseq::indexing::crc64_ecma182(directory)
            != manifest.directory_checksum
        || encoded_manifest.size()
            != MinimizerGenerationManifest::format5_encoded_bytes
        || bioseq::indexing::crc64_ecma182(encoded_manifest)
            != head.manifest_checksum) {
        rev3_storage_error_as_cpp(
            "MPF5 v2 generation metadata receipt is invalid");
    }
}

struct MappingLease {
    ResourceOwner owner = nullptr;
    int fd = -1;
    void* mapping = MAP_FAILED;
    std::size_t mapping_bytes = 0u;
    dev_t device{};
    ino_t inode{};
    bool remembered = false;
};

inline void release_mapping_lease_raw(MappingLease& lease) noexcept {
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
    auto* const lease = reinterpret_cast<MappingLease*>(DatumGetPointer(value));
    if (lease == nullptr) return;
    lease->remembered = false;
    release_mapping_lease_raw(*lease);
}

inline constexpr ResourceReleasePriority kMappingLeaseReleasePriority = 10u;
static_assert(kMappingLeaseReleasePriority >= RELEASE_PRIO_FIRST);
inline const ResourceOwnerDesc kMappingLeaseResourceOwnerDesc{
    .name = "bioseq MPF5 v2 mapping lease",
    .release_phase = RESOURCE_RELEASE_AFTER_LOCKS,
    .release_priority = kMappingLeaseReleasePriority,
    .ReleaseResource = release_mapping_lease_resource,
    .DebugPrint = nullptr,
};

[[nodiscard]] inline MappingLease* allocate_mapping_lease() {
    if (CurrentResourceOwner == nullptr || TopTransactionContext == nullptr) {
        rev3_storage_error_as_cpp(
            "MPF5 v2 mapping requires a transaction ResourceOwner");
    }
    void* const storage = MemoryContextAlloc(
        TopTransactionContext, sizeof(MappingLease));
    return ::new (storage) MappingLease{
        .owner = CurrentResourceOwner,
    };
}

inline void destroy_mapping_lease(MappingLease*& lease) noexcept {
    if (lease == nullptr) return;
    if (lease->remembered) {
        ResourceOwnerForget(
            lease->owner, PointerGetDatum(lease),
            &kMappingLeaseResourceOwnerDesc);
        lease->remembered = false;
    }
    release_mapping_lease_raw(*lease);
    lease->~MappingLease();
    pfree(lease);
    lease = nullptr;
}

/// Form the compatibility token expected by the PostgreSQL-free mapped-index
/// readers without transferring or extending the lease lifetime.  The token
/// deliberately has no control block: callers must retain the backend-thread
/// lease guard which supplied it until every borrowed span has been destroyed.
[[nodiscard]] inline std::shared_ptr<const void>
borrow_mapping_token(MappingLease* const lease) noexcept {
    if (lease == nullptr || lease->owner == nullptr || !lease->remembered
        || lease->fd < 0 || lease->mapping == MAP_FAILED
        || lease->mapping == nullptr || lease->mapping_bytes == 0u) {
        return {};
    }
    std::shared_ptr<const void> no_owner;
    return std::shared_ptr<const void>{no_owner, lease->mapping};
}

struct PageAlignedAdviceRange {
    std::size_t begin = 0u;
    std::size_t end = 0u;

    [[nodiscard]] constexpr bool operator==(
        const PageAlignedAdviceRange&) const noexcept = default;
};

/// Translate one byte-exact authenticated section into the page interval
/// accepted by madvise-style APIs. The final interval never exceeds the
/// mapping; its first/last page may also contain adjacent wire bytes because
/// the VM can only advise complete pages.
[[nodiscard]] constexpr std::optional<PageAlignedAdviceRange>
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
    return PageAlignedAdviceRange{
        .begin = aligned_begin,
        .end = aligned_end,
    };
}

static_assert(page_aligned_advice_range(
    16'384u, 4'352u, 4'096u, 4'096u)
    == PageAlignedAdviceRange{4'096u, 12'288u});
static_assert(page_aligned_advice_range(
    10'000u, 9'000u, 1'000u, 4'096u)
    == PageAlignedAdviceRange{8'192u, 10'000u});
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
        const MappingLease& lease,
        const PageAlignedAdviceRange range) noexcept {
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
/// stable MappingLease which supplied the callback token.
inline void advise_borrowed_mapping_page_range(
        void* const context,
        const std::uintptr_t page_begin,
        const std::uintptr_t page_end) noexcept {
    auto* const lease = static_cast<MappingLease*>(context);
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
        PageAlignedAdviceRange{
            .begin = static_cast<std::size_t>(page_begin - mapping_begin),
            .end = static_cast<std::size_t>(clipped_end - mapping_begin),
        });
}

} // namespace mpf5_image_pg_detail

/// Shared header-last v2 writer.  Relation generations feed this from their
/// authenticated heap reread; mapped generations can feed the same append API
/// directly from the common Format-5 encoder callbacks without forming tuples.
class Mpf5V2WriterPg {
private:
    GenerationFileIdentity identity_{};
    bioseq::indexing::Mpf5V2Provider provider_{};
    PendingGenerationFilePublication publication_{};
    std::uint64_t payload_bytes_ = 0u;
    Crc64Ecma182 payload_crc_{};
    bool sealed_ = false;

public:
    Mpf5V2WriterPg(
            const Rev3IndexAdministrationIdentity& identity,
            const std::uint64_t query_key,
            const std::uint64_t generation_id,
            const bioseq::indexing::Mpf5V2Provider provider)
        : identity_(mpf5_image_pg_detail::generation_file_identity(
              identity, query_key, generation_id)),
          provider_(provider),
          publication_(begin_generation_file_publication(identity_)) {
        if (!bioseq::indexing::is_valid_mpf5_v2_provider(
                static_cast<std::uint8_t>(provider_))) {
            rev3_storage_error_as_cpp("MPF5 v2 provider is invalid");
        }
    }

    Mpf5V2WriterPg(const Mpf5V2WriterPg&) = delete;
    Mpf5V2WriterPg& operator=(const Mpf5V2WriterPg&) = delete;
    Mpf5V2WriterPg(Mpf5V2WriterPg&&) = delete;
    Mpf5V2WriterPg& operator=(Mpf5V2WriterPg&&) = delete;

    void append(const std::span<const std::byte> bytes) {
        if (sealed_ || publication_.fd < 0
            || bytes.size()
                > (std::numeric_limits<std::uint64_t>::max)()
                    - payload_bytes_) {
            rev3_storage_error_as_cpp(
                "MPF5 v2 payload append state/size is invalid");
        }
        mpf5_image_pg_detail::pwrite_all(
            publication_.fd, bytes,
            bioseq::indexing::kMpf5V2HeaderBytes + payload_bytes_);
        payload_crc_.update(bytes);
        payload_bytes_ += bytes.size();
    }

    [[nodiscard]] Mpf5V2FileReceiptPg publish(
            const MinimizerGenerationHead& head,
            const MinimizerGenerationManifest& manifest,
            const DirView& authenticated_directory,
            const std::span<const std::byte> encoded_manifest) {
        using namespace bioseq::indexing;
        const std::span<const std::byte> directory =
            authenticated_directory.encoded_bytes();
        mpf5_image_pg_detail::validate_generation_metadata(
            identity_, head, manifest, directory, encoded_manifest);
        if (sealed_ || publication_.fd < 0
            || payload_bytes_ != manifest.payload_bytes) {
            rev3_storage_error_as_cpp(
                "MPF5 v2 publication payload receipt is invalid");
        }
        const std::uint64_t payload_crc64 = payload_crc_.checksum();
        if (payload_bytes_
                > (std::numeric_limits<std::uint64_t>::max)()
                    - kMpf5V2HeaderBytes) {
            rev3_storage_error_as_cpp(
                "MPF5 v2 payload mapping length overflows");
        }
        const std::uint64_t payload_mapping_bytes =
            kMpf5V2HeaderBytes + payload_bytes_;
        mpf5_image_pg_detail::resize_temporary_image(
            publication_.fd, payload_mapping_bytes);
        mpf5_image_pg_detail::ScopedMappedRegion payload_mapping{
            publication_.fd, payload_mapping_bytes, PROT_READ};
        const std::span<const std::byte> mapped_payload =
            payload_mapping.bytes().subspan(
                static_cast<std::size_t>(kMpf5V2HeaderBytes),
                static_cast<std::size_t>(payload_bytes_));
        auto opened_adapter = Format5CompactH32Adapter::open(
            Format5CompactH32OpenedGenerationView{
                .head = &head,
                .manifest = &manifest,
                .directory = &authenticated_directory,
                .mapped_payload = mapped_payload,
            });
        if (!opened_adapter) {
            rev3_storage_error_as_cpp(
                "MPF5 v2 compact-H32 source adapter rejected Format-5");
        }
        Format5CompactH32Adapter compact_adapter =
            std::move(*opened_adapter);
        const auto profile = minimizer_generation_payload_profile(manifest);
        if (!profile || !profile->shape_v5
            || profile->shape_v5->max_occ_cap == 0u
            || profile->shape_v5->max_occ_cap
                > (std::numeric_limits<std::uint32_t>::max)()) {
            rev3_storage_error_as_cpp(
                "MPF5 v2 persisted Shape-V5 cap is invalid");
        }
        const std::uint32_t persisted_max_occ_cap =
            static_cast<std::uint32_t>(
                profile->shape_v5->max_occ_cap);
        const auto compact_options = compact_adapter.compact_build_options(
            persisted_max_occ_cap);
        if (!compact_options) {
            rev3_storage_error_as_cpp(
                "MPF5 v2 compact-H32 build options are incompatible");
        }
        const CompactH32SortedRunSourceRef compact_source =
            compact_adapter.sorted_run_source();
        const auto compact_plan = plan_compact_h32_direct_index(
            compact_source, *compact_options);
        if (!compact_plan) {
            rev3_storage_error_as_cpp(
                "MPF5 v2 compact-H32 image planning failed");
        }

        const auto provisional_layout = plan_mpf5_v2_layout(
            payload_bytes_, directory.size(), encoded_manifest.size(),
            payload_crc64, manifest.directory_checksum,
            head.manifest_checksum,
            Mpf5V2CompactH32DirectSectionSpec{
                .bytes = compact_plan->image_bytes,
                .direct_header_crc64 = 0u,
            });
        if (!provisional_layout
            || provisional_layout->feature_bitmap
                != kMpf5V2FeatureCompactH32Direct
            || provisional_layout->compact_h32_direct.bytes
                != compact_plan->image_bytes) {
            rev3_storage_error_as_cpp(
                "MPF5 v2 direct-section layout planning failed");
        }
        const std::uint64_t payload_end =
            provisional_layout->payload.offset
                + provisional_layout->payload.bytes;
        mpf5_image_pg_detail::write_zero_padding(
            publication_.fd, payload_end,
            provisional_layout->directory.offset);
        mpf5_image_pg_detail::pwrite_all(
            publication_.fd, directory,
            provisional_layout->directory.offset);
        const std::uint64_t directory_end =
            provisional_layout->directory.offset
                + provisional_layout->directory.bytes;
        mpf5_image_pg_detail::write_zero_padding(
            publication_.fd, directory_end,
            provisional_layout->manifest.offset);
        mpf5_image_pg_detail::pwrite_all(
            publication_.fd, encoded_manifest,
            provisional_layout->manifest.offset);
        const std::uint64_t manifest_end =
            provisional_layout->manifest.offset
                + provisional_layout->manifest.bytes;
        mpf5_image_pg_detail::write_zero_padding(
            publication_.fd, manifest_end,
            provisional_layout->compact_h32_direct.offset);
        mpf5_image_pg_detail::resize_temporary_image(
            publication_.fd, provisional_layout->file_bytes);

        std::uint64_t direct_header_crc64 = 0u;
        {
            mpf5_image_pg_detail::ScopedMappedRegion destination_mapping{
                publication_.fd, provisional_layout->file_bytes,
                PROT_READ | PROT_WRITE};
            const auto direct_offset = static_cast<std::size_t>(
                provisional_layout->compact_h32_direct.offset);
            const auto direct_bytes = static_cast<std::size_t>(
                provisional_layout->compact_h32_direct.bytes);
            std::span<std::byte> direct_image =
                destination_mapping.writable_bytes().subspan(
                    direct_offset, direct_bytes);
            if (auto built = build_compact_h32_direct_index_into(
                    compact_source, *compact_options,
                    *compact_plan, direct_image);
                !built) {
                rev3_storage_error_as_cpp(
                    "MPF5 v2 compact-H32 image replay/build failed");
            }
            const auto validated_direct =
                validate_compact_h32_direct_index(
                    std::span<const std::byte>{direct_image},
                    compact_options->identity,
                    compact_options->max_occ_cap,
                    compact_options->geometry);
            if (!validated_direct) {
                rev3_storage_error_as_cpp(
                    "MPF5 v2 compact-H32 nested image validation failed");
            }
            const CompactH32ImagePlan& validated_direct_plan =
                validated_direct->header;
            if (validated_direct_plan != *compact_plan) {
                rev3_storage_error_as_cpp(
                    "MPF5 v2 compact-H32 validated plan receipt changed");
            }
            direct_header_crc64 = crc64_ecma182(
                std::span<const std::byte>{direct_image}.first(
                    kCompactH32DirectIndexHeaderBytes));
            destination_mapping.sync();
        }

        const auto layout = plan_mpf5_v2_layout(
            payload_bytes_, directory.size(), encoded_manifest.size(),
            payload_crc64, manifest.directory_checksum,
            head.manifest_checksum,
            Mpf5V2CompactH32DirectSectionSpec{
                .bytes = compact_plan->image_bytes,
                .direct_header_crc64 = direct_header_crc64,
            });
        if (!layout
            || layout->payload != provisional_layout->payload
            || layout->directory != provisional_layout->directory
            || layout->manifest != provisional_layout->manifest
            || layout->feature_bitmap
                != provisional_layout->feature_bitmap
            || layout->compact_h32_direct.offset
                != provisional_layout->compact_h32_direct.offset
            || layout->compact_h32_direct.bytes
                != provisional_layout->compact_h32_direct.bytes
            || layout->file_bytes != provisional_layout->file_bytes) {
            rev3_storage_error_as_cpp(
                "MPF5 v2 authenticated direct layout changed after build");
        }

        DecodedMpf5HeaderV2 header{
            .magic = kMpf5V2Magic,
            .version = kMpf5V2Version,
            .header_bytes = kMpf5V2HeaderBytes,
            .database_oid = identity_.database_oid,
            .tablespace_oid = identity_.tablespace_oid,
            .main_relfilenode = identity_.main_relfilenode,
            .user_relid = identity_.user_relid,
            .column_attno = static_cast<std::uint16_t>(
                identity_.column_attno),
            .index_kind = kMpf5V2IndexKind,
            .provider = static_cast<std::uint8_t>(provider_),
            .wire_format = kMpf5V2WireFormat,
            .query_key = identity_.query_key,
            .generation_id = identity_.generation_id,
            .source_identity = head.source_identity,
            .source_content_crc = head.source_content_checksum,
            .compatibility_crc = head.compatibility_checksum,
            .target_length = manifest.target_length,
            .file_bytes = layout->file_bytes,
            .position_segment_count = manifest.position_segment_count,
            .position_segment_span_bp =
                manifest.position_segment_span_bp,
            .payload = layout->payload,
            .directory = layout->directory,
            .manifest = layout->manifest,
            .feature_bitmap = layout->feature_bitmap,
            .compact_h32_direct = layout->compact_h32_direct,
        };
        const auto encoded_header = encode_mpf5_v2_header(header);
        if (!encoded_header) {
            rev3_storage_error_as_cpp("MPF5 v2 header encoding failed");
        }
        const auto decoded_header = decode_mpf5_v2_header(*encoded_header);
        if (!decoded_header) {
            rev3_storage_error_as_cpp("MPF5 v2 header self-decode failed");
        }
        mpf5_image_pg_detail::pwrite_all(
            publication_.fd, *encoded_header, 0u);

        struct stat observed{};
        if (fstat(publication_.fd, &observed) != 0
            || observed.st_size < 0
            || static_cast<std::uint64_t>(observed.st_size)
                != layout->file_bytes
            || observed.st_dev != publication_.expected_device
            || observed.st_ino != publication_.expected_inode) {
            rev3_storage_error_as_cpp(
                "MPF5 v2 completed temp image size/inode is invalid");
        }
        const Mpf5V2ValidationOptions validation_options{
            .expected_identity = mpf5_image_pg_detail::expected_identity(
                identity_, provider_, head, manifest, *layout,
                decoded_header->header_crc64),
            .payload_crc_policy = Mpf5V2PayloadCrcPolicy::deferred,
        };
        {
            mpf5_image_pg_detail::ScopedMappedRegion validation_mapping{
                publication_.fd, layout->file_bytes, PROT_READ};
            const auto validated = validate_mpf5_v2_image(
                validation_mapping.bytes(), validation_options);
            if (!validated || validated->payload_crc_bytes != 0u) {
                rev3_storage_error_as_cpp(
                    "MPF5 v2 bounded publication validation failed");
            }
        }

        const Mpf5V2FileReceiptPg receipt{
            .provider = provider_,
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

enum class Mpf5V2MappedOpenValidationPg : std::uint8_t {
    /// Timed restart path: authenticate only the fixed outer header, layout,
    /// descriptor identity and section receipts. No section payload is read.
    bounded_header_only = 0u,
    /// Explicit publication/scrub compatibility path. Payload CRC remains
    /// deferred, while directory/manifest metadata receive the legacy full
    /// validation pass.
    full_metadata = 1u,
};

/// Backend-thread RAII authority for one registered MPF5 mmap/fd lease.
///
/// Borrowed reader tokens do not own or prolong this guard.  On the normal
/// executor path the guard must therefore be declared/stored before every
/// reader which borrows it, so reverse destruction releases the reader first
/// and then promptly forgets, unmaps, closes and frees the lease.  PostgreSQL's
/// ResourceOwner registration remains armed while the guard is live and is the
/// ERROR/longjmp fallback when ordinary C++ destruction cannot run.
class MappedMinimizerFormat5LeaseGuardPg {
private:
    mpf5_image_pg_detail::MappingLease* lease_ = nullptr;

    explicit MappedMinimizerFormat5LeaseGuardPg(
            mpf5_image_pg_detail::MappingLease* lease) noexcept
        : lease_(lease) {}

    friend class MappedMinimizerFormat5ImagePg;

public:
    MappedMinimizerFormat5LeaseGuardPg() = default;
    MappedMinimizerFormat5LeaseGuardPg(
            const MappedMinimizerFormat5LeaseGuardPg&) = delete;
    MappedMinimizerFormat5LeaseGuardPg& operator=(
            const MappedMinimizerFormat5LeaseGuardPg&) = delete;

    MappedMinimizerFormat5LeaseGuardPg(
            MappedMinimizerFormat5LeaseGuardPg&& other) noexcept
        : lease_(std::exchange(other.lease_, nullptr)) {}

    MappedMinimizerFormat5LeaseGuardPg& operator=(
            MappedMinimizerFormat5LeaseGuardPg&& other) noexcept {
        if (this != &other) {
            mpf5_image_pg_detail::destroy_mapping_lease(lease_);
            lease_ = std::exchange(other.lease_, nullptr);
        }
        return *this;
    }

    ~MappedMinimizerFormat5LeaseGuardPg() {
        mpf5_image_pg_detail::destroy_mapping_lease(lease_);
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(borrow_mapping_token());
    }

    /// Non-owning compatibility token.  Every copy becomes invalid as soon as
    /// this guard is destroyed or replaced.
    [[nodiscard]] std::shared_ptr<const void>
    borrow_mapping_token() const noexcept {
        return mpf5_image_pg_detail::borrow_mapping_token(lease_);
    }
};

static_assert(!std::is_copy_constructible_v<
    MappedMinimizerFormat5LeaseGuardPg>);
static_assert(!std::is_copy_assignable_v<
    MappedMinimizerFormat5LeaseGuardPg>);
static_assert(std::is_nothrow_move_constructible_v<
    MappedMinimizerFormat5LeaseGuardPg>);
static_assert(std::is_nothrow_move_assignable_v<
    MappedMinimizerFormat5LeaseGuardPg>);

class MappedMinimizerFormat5ImagePg {
private:
    mpf5_image_pg_detail::MappingLease* lease_ = nullptr;
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
        mpf5_image_pg_detail::destroy_mapping_lease(lease_);
    }

public:
    MappedMinimizerFormat5ImagePg() = default;
    MappedMinimizerFormat5ImagePg(
            const MappedMinimizerFormat5ImagePg&) = delete;
    MappedMinimizerFormat5ImagePg& operator=(
            const MappedMinimizerFormat5ImagePg&) = delete;
    MappedMinimizerFormat5ImagePg(
            MappedMinimizerFormat5ImagePg&& other) noexcept
        : lease_(std::exchange(other.lease_, nullptr)),
          payload_(std::exchange(other.payload_, {})),
          compact_h32_direct_(std::exchange(
              other.compact_h32_direct_, {})),
          payload_raw_checksum_(std::exchange(
              other.payload_raw_checksum_, 0u)),
          compact_h32_direct_header_receipt_(std::exchange(
              other.compact_h32_direct_header_receipt_, std::nullopt)) {}
    MappedMinimizerFormat5ImagePg& operator=(
            MappedMinimizerFormat5ImagePg&&) = delete;
    ~MappedMinimizerFormat5ImagePg() {
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
    /// persisted in and authenticated by the outer MPF5 header.
    [[nodiscard]] std::optional<std::uint64_t>
    compact_h32_direct_header_receipt() const noexcept {
        return compact_h32_direct_header_receipt_;
    }

    /// Issue fail-open asynchronous readahead for the authenticated compact-
    /// H32 metadata only. Selected posting pages are advised later, while the
    /// existing descriptor layout traversal already knows their exact spans.
    /// The unrelated outer MPF payload and unselected posting pages are never
    /// named as byte ranges; page granularity may include adjacent bytes on a
    /// boundary page.
    void advise_compact_h32_direct_metadata_readahead(
            const bioseq::indexing::CompactH32DirectIndexView&
                authenticated_direct) const noexcept {
        using mpf5_image_pg_detail::PageAlignedAdviceRange;
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
        std::array<PageAlignedAdviceRange, direct_sections.size()>
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
            const auto aligned = mpf5_image_pg_detail::
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
            mpf5_image_pg_detail::
                advise_mapping_range_async_best_effort(
                    *lease_, merged_ranges[ordinal]);
        }
    }

    /// Form a non-owning adviser before this image transfers its lease into
    /// the longer-lived guard. The context is the MappingLease allocation
    /// itself, whose address remains stable across that transfer.
    [[nodiscard]] bioseq::indexing::CompactDirectSelectedPageAdviser
    borrow_compact_h32_selected_page_adviser(
            const bioseq::indexing::CompactH32DirectIndexView&
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
        return bioseq::indexing::CompactDirectSelectedPageAdviser{
            .context = lease_,
            .page_bytes = static_cast<std::size_t>(observed_page_bytes),
            .advise_fn =
                &mpf5_image_pg_detail::advise_borrowed_mapping_page_range,
        };
    }

    /// Borrow the PG-free reader's shared_ptr-shaped compatibility token while
    /// retaining this image as the prompt normal-path lease authority.
    [[nodiscard]] std::shared_ptr<const void>
    borrow_mapping_token() const noexcept {
        return mpf5_image_pg_detail::borrow_mapping_token(lease_);
    }

    /// Transfer only the mmap/fd lifetime authority into a compact guard.  Any
    /// spans copied before this call remain valid until the returned guard is
    /// destroyed; this moved-from image no longer exposes mapped spans.
    [[nodiscard]] MappedMinimizerFormat5LeaseGuardPg
    release_mapping_lease_guard() && noexcept {
        auto* const lease = std::exchange(lease_, nullptr);
        clear_views_();
        return MappedMinimizerFormat5LeaseGuardPg{lease};
    }

    friend MappedMinimizerFormat5ImagePg
    open_exact_minimizer_format5_image_pg(
        const Rev3IndexAdministrationIdentity&,
        std::uint64_t,
        const MinimizerGenerationHead&,
        const MinimizerGenerationManifest&,
        const MinimizerGenerationStorageDescriptorPg&,
        Mpf5V2MappedOpenValidationPg);
};

[[nodiscard]] inline MappedMinimizerFormat5ImagePg
open_exact_minimizer_format5_image_pg(
        const Rev3IndexAdministrationIdentity& identity,
        const std::uint64_t query_key,
        const MinimizerGenerationHead& head,
        const MinimizerGenerationManifest& manifest,
        const MinimizerGenerationStorageDescriptorPg& descriptor,
        const Mpf5V2MappedOpenValidationPg validation) {
    using namespace bioseq::indexing;
    const GenerationFileIdentity file_identity =
        mpf5_image_pg_detail::generation_file_identity(
            identity, query_key, manifest.generation_id);
    if (manifest.wire_format
            != MinimizerGenerationWireFormat::
                grouped_shape_soa_segment_local
        || head.generation_id == 0u
        || head.generation_id != manifest.generation_id
        || head.source_identity != manifest.source_identity
        || head.source_content_checksum
            != manifest.source_content_checksum
        || !exact_storage_descriptor_identity(
            descriptor, identity, query_key)
        || descriptor.generation_id != manifest.generation_id
        || descriptor.state != MinimizerGenerationStorageStatePg::live
        || descriptor.retire_full_xid
        || !exact_storage_descriptor_provider_shape(descriptor)) {
        rev3_storage_error_as_cpp("MPF5 v2 open binding is invalid");
    }
    const ResolvedGenerationFilePath resolved =
        resolve_generation_file_path(file_identity);
    auto* lease = mpf5_image_pg_detail::allocate_mapping_lease();
    ResourceOwnerEnlarge(lease->owner);
    lease->fd = ::open(
        resolved.final_path.data(),
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | PG_BINARY);
    if (lease->fd < 0) {
        const int saved_errno = errno;
        mpf5_image_pg_detail::destroy_mapping_lease(lease);
        rev3_storage_error_as_cpp(fmt::format(
            "MPF5 v2 mapped image open failed: errno={}", saved_errno));
    }
    struct stat observed{};
    const bool inode_valid = fstat(lease->fd, &observed) == 0
        && S_ISREG(observed.st_mode)
        && observed.st_uid == geteuid()
        && (observed.st_mode & 07777) == 0400
        && observed.st_nlink == 1
        && observed.st_size >= static_cast<off_t>(kMpf5V2HeaderBytes)
        && static_cast<std::uint64_t>(observed.st_size)
            <= (std::numeric_limits<std::size_t>::max)()
        && static_cast<std::uint64_t>(observed.st_size)
            == descriptor.image_bytes;
    if (!inode_valid) {
        mpf5_image_pg_detail::destroy_mapping_lease(lease);
        rev3_storage_error_as_cpp(
            "MPF5 v2 mapped image inode contract is invalid");
    }
    lease->device = observed.st_dev;
    lease->inode = observed.st_ino;
    // No PostgreSQL call is permitted between the raw fd/inode acquisition
    // above and this pre-enlarged ResourceOwner registration.
    ResourceOwnerRemember(
        lease->owner, PointerGetDatum(lease),
        &mpf5_image_pg_detail::kMappingLeaseResourceOwnerDesc);
    lease->remembered = true;
    lease->mapping_bytes = static_cast<std::size_t>(observed.st_size);
    lease->mapping = mmap(
        nullptr, lease->mapping_bytes,
        PROT_READ, MAP_PRIVATE, lease->fd, 0);
    if (lease->mapping == MAP_FAILED) {
        const int saved_errno = errno;
        mpf5_image_pg_detail::destroy_mapping_lease(lease);
        rev3_storage_error_as_cpp(fmt::format(
            "MPF5 v2 image mmap failed: errno={}", saved_errno));
    }
    const std::span<const std::byte> image{
        static_cast<const std::byte*>(lease->mapping),
        lease->mapping_bytes};
    EncodedMpf5HeaderV2 encoded_header{};
    std::memcpy(
        encoded_header.data(), image.data(), encoded_header.size());
    const auto decoded_header = decode_mpf5_v2_header(encoded_header);
    if (!decoded_header
        || decoded_header->header_crc64 != descriptor.header_checksum) {
        mpf5_image_pg_detail::destroy_mapping_lease(lease);
        rev3_storage_error_as_cpp(
            "MPF5 v2 mapped header disagrees with its LIVE descriptor");
    }
    std::optional<Mpf5V2CompactH32DirectSectionSpec>
        compact_h32_direct;
    if ((decoded_header->feature_bitmap
            & kMpf5V2FeatureCompactH32Direct) != 0u) {
        compact_h32_direct = Mpf5V2CompactH32DirectSectionSpec{
            .bytes = decoded_header->compact_h32_direct.bytes,
            .direct_header_crc64 =
                decoded_header->compact_h32_direct.direct_header_crc64,
        };
    }
    const auto layout = plan_mpf5_v2_layout(
        manifest.payload_bytes,
        manifest.directory_bytes,
        MinimizerGenerationManifest::format5_encoded_bytes,
        decoded_header->payload.crc64,
        manifest.directory_checksum,
        head.manifest_checksum,
        compact_h32_direct);
    if (!layout || descriptor.image_bytes != layout->file_bytes) {
        mpf5_image_pg_detail::destroy_mapping_lease(lease);
        rev3_storage_error_as_cpp(
            "MPF5 v2 descriptor layout is invalid");
    }
    const Mpf5V2ValidationOptions options{
        .expected_identity = mpf5_image_pg_detail::expected_identity(
            file_identity, descriptor.provider, head, manifest, *layout,
            descriptor.header_checksum),
        .payload_crc_policy = Mpf5V2PayloadCrcPolicy::deferred,
    };
    const auto validated = validation
            == Mpf5V2MappedOpenValidationPg::bounded_header_only
        ? open_mpf5_v2_image_bounded(image, options)
        : validation == Mpf5V2MappedOpenValidationPg::full_metadata
            ? validate_mpf5_v2_image(image, options)
            : std::expected<ValidatedMpf5V2ImageView,
                Mpf5V2WireError>{std::unexpected(
                    Mpf5V2WireError::identity_mismatch)};
    const bool bounded_evidence_exact = validated
        && validated->header_crc_bytes == kMpf5V2HeaderBytes
        && validated->payload_crc_bytes == 0u
        && (validation
                == Mpf5V2MappedOpenValidationPg::bounded_header_only
            ? validated->directory_crc_bytes == 0u
                && validated->manifest_crc_bytes == 0u
            : validation == Mpf5V2MappedOpenValidationPg::full_metadata
                && validated->directory_crc_bytes
                    == manifest.directory_bytes
                && validated->manifest_crc_bytes
                    == MinimizerGenerationManifest::format5_encoded_bytes);
    if (!bounded_evidence_exact) {
        mpf5_image_pg_detail::destroy_mapping_lease(lease);
        rev3_storage_error_as_cpp(
            "MPF5 v2 mapped image validation/evidence failed");
    }
    MappedMinimizerFormat5ImagePg result;
    result.lease_ = lease;
    result.payload_ = validated->payload;
    result.compact_h32_direct_ = validated->compact_h32_direct;
    result.payload_raw_checksum_ = validated->header.payload.crc64;
    if (!validated->compact_h32_direct.empty()) {
        result.compact_h32_direct_header_receipt_ =
            validated->header.compact_h32_direct.direct_header_crc64;
    }
    return result;
}

/// Relation-owning sink factory target used directly by the existing common
/// two-pass generation encoder. It cannot publish the head and therefore cannot
/// make an incompletely reread generation visible.
class IndexStorageFragmentGenerationRowSinkPg {
private:
    enum class WriteStage : std::uint8_t {
        payload,
        directory,
        manifest,
        tokenized,
        failed,
        moved,
    };

    static constexpr std::uint64_t kWriteBatchRows = 4096;
    static constexpr std::uint64_t kWriteBatchBytes =
        MAX_CATALOG_MULTI_INSERT_BYTES;

    Rev3IndexAdministrationIdentity identity_{};
    Relation relation_ = nullptr;
    CatalogIndexState catalog_indexes_ = nullptr;
    Relation validation_primary_index_ = nullptr;
    Snapshot validation_snapshot_ = nullptr;
    SysScanDesc validation_scan_ = nullptr;
    std::uint64_t advisory_lock_key_ = 0;
    std::uint64_t query_key_ = 0;
    std::uint64_t generation_id_ = 0;
    std::uint64_t row_count_ = 0;
    std::uint64_t write_batch_row_limit_ = kWriteBatchRows;
    std::uint64_t catalog_multi_insert_batches_ = 0;
    std::vector<TupleTableSlot*> pending_slots_;
    std::uint64_t rows_in_batch_ = 0;
    std::uint64_t bytes_in_batch_ = 0;
    std::uint64_t payload_rows_written_ = 0;
    std::uint64_t payload_bytes_written_ = 0;
    Crc64Ecma182 payload_row_checksums_{};
    std::uint32_t next_directory_chunk_ = 1;
    std::uint64_t directory_bytes_written_ = 0;
    Crc64Ecma182 directory_checksum_{};
    std::optional<IndexChunkAddress> previous_payload_address_;
    bool legacy_payload_order_possible_ = true;
    bool global_payload_order_possible_ = true;
    std::array<std::byte, MinimizerGenerationManifest::format5_encoded_bytes>
        written_manifest_bytes_{};
    std::uint32_t written_manifest_size_ = 0;
    std::optional<Rev3AuthoritativeSourceProof> validation_source_proof_;
    WriteStage stage_ = WriteStage::payload;
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
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [slot] { ExecDropSingleTupleTableSlot(slot); });
                inject_rev3_pg_failure_as_cpp(
                    Rev3PgFailureInjection::generation_tts_drop,
                    "generation_tts_drop");
            }
        }
        std::vector<TupleTableSlot*>{}.swap(pending_slots_);
        rows_in_batch_ = 0;
        bytes_in_batch_ = 0;
    }

    void abandon_resources_noexcept_() noexcept {
        validation_source_proof_.reset();
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
        stage_ = WriteStage::failed;
        pg_poisoned_ = true;
        abandon_resources_noexcept_();
        identity_.release_owned_strings();
    }

    void abandon_after_pg_error_() noexcept {
        stage_ = WriteStage::failed;
        pg_poisoned_ = true;
        validation_source_proof_.reset();
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
                rev3_storage_error_as_cpp(
                    "generation write batch counters disagree");
            }
            return;
        }
        if (relation_ == nullptr || catalog_indexes_ == nullptr
            || pending_slots_.size() != rows_in_batch_
            || pending_slots_.size()
                > static_cast<std::size_t>(
                    (std::numeric_limits<int>::max)())) {
            rev3_storage_error_as_cpp(
                "generation write batch state is invalid");
        }
        bioseq::pg_utils::invoke_pg_as_cpp([&] {
            CatalogTuplesMultiInsertWithInfo(
                relation_, pending_slots_.data(),
                static_cast<int>(pending_slots_.size()), catalog_indexes_);
        });
        inject_rev3_pg_failure_as_cpp(
            Rev3PgFailureInjection::generation_multi_insert,
            "generation_multi_insert");
        if (!checked_add_(catalog_multi_insert_batches_, 1)) {
            rev3_storage_error_as_cpp(
                "generation write batch counter overflow");
        }
        drop_pending_slots_as_cpp_();
    }

    [[nodiscard]] std::optional<MinimizerGenerationWireError>
    validate_next_row_(const MinimizerGenerationRowView& row) noexcept {
        if (stage_ == WriteStage::manifest
            || stage_ == WriteStage::tokenized
            || stage_ == WriteStage::failed
            || stage_ == WriteStage::moved) {
            return MinimizerGenerationWireError::row_order;
        }
        const bool manifest = row.address.position_segment == -1
            && row.address.sub_key == DIRECTORY_SUB_KEY
            && row.address.chunk_no == 0;
        const bool directory = row.address.position_segment == -1
            && row.address.sub_key == DIRECTORY_SUB_KEY
            && row.address.chunk_no > 0;
        if (manifest) {
            if (row.address.chunk_no != 0
                || (row.bytes.size()
                        != MinimizerGenerationManifest::encoded_bytes
                    && row.bytes.size()
                        != MinimizerGenerationManifest::
                            format5_encoded_bytes)) {
                return MinimizerGenerationWireError::row_order;
            }
            return std::nullopt;
        }
        if (directory) {
            if (stage_ != WriteStage::payload
                && stage_ != WriteStage::directory) {
                return MinimizerGenerationWireError::row_order;
            }
            if (row.address.chunk_no != next_directory_chunk_) {
                return MinimizerGenerationWireError::chunk_range;
            }
            return std::nullopt;
        }
        if (stage_ != WriteStage::payload
            || row.address.position_segment < 0
            || row.address.sub_key < 0
            || row.address.sub_key >= TEMP_RUN_BASE) {
            return MinimizerGenerationWireError::row_order;
        }
        const bool position_extent =
            row.address.sub_key >= POSITION_EXTENT_BASE;
        const index_sub_key_t logical_sub_key = position_extent
            ? row.address.sub_key - POSITION_EXTENT_BASE
            : row.address.sub_key;
        if (logical_sub_key < 0
            || logical_sub_key >= POSITION_EXTENT_BASE) {
            return MinimizerGenerationWireError::sub_key;
        }
        if (previous_payload_address_) {
            const bool previous_position_extent =
                previous_payload_address_->sub_key >= POSITION_EXTENT_BASE;
            const index_sub_key_t previous_logical_sub_key =
                previous_position_extent
                ? previous_payload_address_->sub_key - POSITION_EXTENT_BASE
                : previous_payload_address_->sub_key;
            const auto previous = std::tuple{
                previous_payload_address_->position_segment,
                previous_logical_sub_key,
                previous_position_extent,
                previous_payload_address_->chunk_no};
            const auto current = std::tuple{
                row.address.position_segment,
                logical_sub_key,
                position_extent,
                row.address.chunk_no};
            legacy_payload_order_possible_ =
                legacy_payload_order_possible_ && previous < current;
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
            if (!legacy_payload_order_possible_
                && !global_payload_order_possible_) {
                return MinimizerGenerationWireError::row_order;
            }
        }
        return std::nullopt;
    }

    void record_written_row_(
            const MinimizerGenerationRowView& row,
            std::uint64_t admitted_batch_bytes) {
        const bool manifest = row.address.position_segment == -1
            && row.address.sub_key == DIRECTORY_SUB_KEY
            && row.address.chunk_no == 0;
        const bool directory = row.address.position_segment == -1
            && row.address.sub_key == DIRECTORY_SUB_KEY
            && row.address.chunk_no > 0;
        if (manifest) {
            written_manifest_bytes_.fill(std::byte{0});
            std::copy(row.bytes.begin(), row.bytes.end(),
                written_manifest_bytes_.begin());
            written_manifest_size_ = static_cast<std::uint32_t>(
                row.bytes.size());
            stage_ = WriteStage::manifest;
        } else if (directory) {
            stage_ = WriteStage::directory;
            ++next_directory_chunk_;
            directory_checksum_.update(row.bytes);
            if (!checked_add_(directory_bytes_written_, row.bytes.size())) {
                rev3_storage_error_as_cpp(
                    "directory byte counter overflow");
            }
        } else {
            previous_payload_address_ = row.address;
            payload_row_checksums_.update_little_endian(row.row_checksum);
            if (!checked_add_(payload_rows_written_, 1)
                || !checked_add_(payload_bytes_written_, row.bytes.size())) {
                rev3_storage_error_as_cpp(
                    "payload write counter overflow");
            }
        }
        if (!checked_add_(row_count_, 1)
            || !checked_add_(rows_in_batch_, 1)
            || !checked_add_(bytes_in_batch_, admitted_batch_bytes)) {
            rev3_storage_error_as_cpp(
                "generation write counter overflow");
        }
    }

    [[nodiscard]] std::expected<void, MinimizerGenerationWireError>
    write_validated_row_(
            const ValidatedRev3GenerationRow& validated_row) {
        const MinimizerGenerationRowView& row = validated_row.row();
        try {
            auto tuple = form_rev3_generation_row_pg(
                relation_, query_key_, validated_row);
            if (!tuple) {
                fail_cleanup_();
                return std::unexpected(tuple.error());
            }
            const std::uint64_t actual_batch_bytes =
                static_cast<std::uint64_t>((*tuple)->t_len)
                + sizeof(TupleTableSlot*) + sizeof(HeapTupleData);
            const auto batch_decision =
                bioseq::indexing::plan_revision3_tuple_batch_append(
                    pending_slots_.size(), bytes_in_batch_,
                    actual_batch_bytes,
                    write_batch_row_limit_, kWriteBatchBytes);
            if (!batch_decision.valid) {
                heap_freetuple(*tuple);
                rev3_storage_error_as_cpp(
                    "generation write row exceeds bounded catalog batch");
            }
            if (batch_decision.flush_before) {
                flush_bounded_batch_as_cpp_();
            }

            TupleTableSlot* slot = bioseq::pg_utils::invoke_pg_as_cpp([&] {
                return MakeSingleTupleTableSlot(
                    RelationGetDescr(relation_), &TTSOpsHeapTuple);
            });
            inject_rev3_pg_failure_as_cpp(
                Rev3PgFailureInjection::generation_tts_create,
                "generation_tts_create");
            try {
                pending_slots_.push_back(slot);
            } catch (...) {
                heap_freetuple(*tuple);
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [&] { ExecDropSingleTupleTableSlot(slot); });
                throw;
            }
            bioseq::pg_utils::invoke_pg_as_cpp(
                [&] { ExecStoreHeapTuple(*tuple, slot, true); });
            inject_rev3_pg_failure_as_cpp(
                Rev3PgFailureInjection::generation_tts_store,
                "generation_tts_store");
            record_written_row_(row, actual_batch_bytes);
            if (batch_decision.flush_after) {
                flush_bounded_batch_as_cpp_();
            }
        } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
            abandon_after_pg_error_();
            throw;
        }
        return {};
    }

public:
    IndexStorageFragmentGenerationRowSinkPg() = delete;
    IndexStorageFragmentGenerationRowSinkPg(
            Rev3IndexAdministrationIdentity identity,
            std::uint64_t query_key,
            std::uint64_t generation_id,
            std::uint64_t write_batch_row_limit = kWriteBatchRows)
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
                || write_batch_row_limit_ > kWriteBatchRows) {
                rev3_storage_error_as_cpp(
                    "generation sink identity/key is invalid");
            }
            advisory_lock_key_ = index_advisory_xact_lock_key(
                identity_.database_oid,
                identity_.tablespace_oid,
                identity_.main_relfilenode,
                identity_.column_attno,
                identity_.index_name);
            bioseq::pg_utils::invoke_pg_as_cpp([&] {
                acquire_index_advisory_xact_lock(
                    identity_.database_oid,
                    identity_.tablespace_oid,
                    identity_.main_relfilenode,
                    identity_.column_attno,
                    identity_.index_name);
            });
            const Oid relation_oid = bioseq::pg_utils::invoke_pg_as_cpp([&] {
                return get_relname_relid(
                    identity_.table_name.c_str(), bioseq_internal_ns());
            });
            relation_ = OidIsValid(relation_oid)
                ? bioseq::pg_utils::invoke_pg_as_cpp([&] {
                    return try_safe_relation_open(
                        relation_oid, RowExclusiveLock);
                })
                : nullptr;
            if (relation_ == nullptr
                || RelationGetNamespace(relation_) != bioseq_internal_ns()
                || std::string_view{RelationGetRelationName(relation_)}
                    != identity_.table_name) {
                rev3_storage_error_as_cpp(
                    "revision-3 generation relation is missing or renamed");
            }
            const Rev3IndexSchemaInspection schema =
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [&] { return inspect_rev3_index_schema(
                        relation_, nullptr,
                        Rev3IndexSchemaExpectation{
                            .namespace_oid = bioseq_internal_ns(),
                            .relation_name = identity_.table_name,
                            .tablespace_oid = identity_.tablespace_oid,
                        }); });
            if (!schema) {
                rev3_storage_error_as_cpp(fmt::format(
                    "generation relation schema mismatch: {}",
                    rev3_index_schema_error_name(schema.error)));
            }
            (void)verify_pg_inline_payload_layout(relation_);
            catalog_indexes_ = bioseq::pg_utils::invoke_pg_as_cpp(
                [&] { return CatalogOpenIndexes(relation_); });
            if (catalog_indexes_ == nullptr) {
                rev3_storage_error_as_cpp(
                    "CatalogOpenIndexes returned NULL");
            }
        } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
            abandon_after_pg_error_();
            throw;
        }
    }
    IndexStorageFragmentGenerationRowSinkPg(
        const IndexStorageFragmentGenerationRowSinkPg&) = delete;
    IndexStorageFragmentGenerationRowSinkPg& operator=(
        const IndexStorageFragmentGenerationRowSinkPg&) = delete;
    IndexStorageFragmentGenerationRowSinkPg(
            IndexStorageFragmentGenerationRowSinkPg&& other) noexcept
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
          legacy_payload_order_possible_(
              other.legacy_payload_order_possible_),
          global_payload_order_possible_(
              other.global_payload_order_possible_),
          written_manifest_bytes_(other.written_manifest_bytes_),
          written_manifest_size_(other.written_manifest_size_),
          validation_source_proof_(std::move(
              other.validation_source_proof_)),
          stage_(other.stage_),
          pg_poisoned_(other.pg_poisoned_) {
        other.stage_ = WriteStage::moved;
        other.pg_poisoned_ = true;
    }
    IndexStorageFragmentGenerationRowSinkPg& operator=(
        IndexStorageFragmentGenerationRowSinkPg&&) = delete;
    ~IndexStorageFragmentGenerationRowSinkPg() {
        abandon_resources_noexcept_();
    }

    [[nodiscard]] std::expected<void, MinimizerGenerationWireError>
    write(const MinimizerGenerationRowView& row) {
        if (pg_poisoned_) {
            rev3_storage_error_as_cpp(
                "poisoned generation row sink reused after PostgreSQL error");
        }
        if (auto order_error = validate_next_row_(row)) {
            fail_cleanup_();
            return std::unexpected(*order_error);
        }
        auto validated = validate_rev3_generation_row_for_write(
            generation_id_, row);
        if (!validated) {
            fail_cleanup_();
            return std::unexpected(validated.error());
        }
        return write_validated_row_(*validated);
    }

    /// Format-5/S1 payload owner: compute the address-bound CRC once, bind it
    /// into an unforgeable validated token, form the tuple from that token, and
    /// return the exact checksum for manifest folding.
    [[nodiscard]] std::expected<MinimizerGenerationRowWriteReceipt,
        MinimizerGenerationWireError>
    write_format5_payload(
            const UnchecksummedMinimizerGenerationRowView& row) {
        if (pg_poisoned_) {
            rev3_storage_error_as_cpp(
                "poisoned generation row sink reused after PostgreSQL error");
        }
        const MinimizerGenerationRowView order_view{
            .address = row.address,
            .bytes = row.bytes,
            .row_checksum = 0,
        };
        if (auto order_error = validate_next_row_(order_view)) {
            fail_cleanup_();
            return std::unexpected(*order_error);
        }
        auto validated = Rev3GenerationRowValidator::validate_and_checksum(
            generation_id_, row);
        if (!validated) {
            fail_cleanup_();
            return std::unexpected(validated.error());
        }
        const std::uint64_t checksum = validated->row().row_checksum;
        if (auto written = write_validated_row_(*validated); !written) {
            return std::unexpected(written.error());
        }
        return MinimizerGenerationRowWriteReceipt{
            .row_checksum = checksum,
        };
    }

    [[nodiscard]] std::uint64_t row_count() const noexcept {
        return row_count_;
    }
    [[nodiscard]] std::uint64_t catalog_multi_insert_batches() const noexcept {
        return catalog_multi_insert_batches_;
    }
    [[nodiscard]] ReadyGenerationToken validate_ready_generation(
            const EncodedMinimizerGeneration& generation) {
        const auto rev3_storage_error = [](std::string_view message) {
            rev3_storage_error_as_cpp(message);
        };
        Oid proven_relation_oid = InvalidOid;
        RelFileLocator proven_locator{};
        std::array<std::byte, MinimizerGenerationHead::encoded_bytes>
            proven_head_bytes{};
        std::uint64_t proven_head_checksum = 0;
        std::uint64_t proven_source_identity = 0;
        std::uint64_t proven_source_content_checksum = 0;
        std::uint64_t proven_compatibility_checksum = 0;
        std::optional<Mpf5V2FileReceiptPg> proven_file_receipt{};

        try {
            if (stage_ != WriteStage::manifest
                || relation_ == nullptr || catalog_indexes_ == nullptr
                || generation.head.generation_id != generation_id_
                || generation.manifest.generation_id != generation_id_) {
                rev3_storage_error("READY validation identity/stage disagrees");
            }
            if (auto metadata =
                    bioseq::indexing::validate_minimizer_generation_metadata(
                        generation.head,
                        generation.manifest,
                        generation.directory);
                !metadata) {
                rev3_storage_error("READY validation metadata disagrees");
            }
            const bool format5 = generation.manifest.wire_format
                == bioseq::indexing::MinimizerGenerationWireFormat::
                    grouped_shape_soa_segment_local;
			const bool mapped_mpf_format5 = format5
				&& identity_.index_name
					== MINIMIZERS_PER_FRAGMENT_INDEX_NAME;
            std::unique_ptr<Mpf5V2WriterPg>
                format5_image_writer;
			/* The sidecar path/header intentionally encode the MPF source
			 * identity, not a logical-index discriminator.  MPS Format-5 keeps
			 * the authoritative PostgreSQL payload route and therefore must not
			 * create or overwrite this MPF-only image. */
            if (mapped_mpf_format5) {
                format5_image_writer =
                    std::make_unique<Mpf5V2WriterPg>(
                        identity_, query_key_, generation_id_,
                        bioseq::indexing::Mpf5V2Provider::
                            postgres_relation);
            }
            if ((format5 && !global_payload_order_possible_)
                || (!format5 && !legacy_payload_order_possible_)) {
                rev3_storage_error(
                    "sink payload order disagrees with the selected wire format");
            }
            const std::uint32_t extent_payload_bytes =
                bioseq::indexing::minimizer_extent_payload_bytes(
                    generation.manifest.wire_format);
            if (extent_payload_bytes == 0) {
                rev3_storage_error("READY validation wire quantum is invalid");
            }
            const MinimizerGenerationRowView head_row{
                .address = generation.unpublished_head_row.address,
                .bytes = generation.unpublished_head_row.bytes(),
                .row_checksum = generation.unpublished_head_row.row_checksum,
            };
            if (head_row.address != IndexChunkAddress{
                    .generation_id = 0,
                    .position_segment = -1,
                    .sub_key = GENERATION_HEAD_SUB_KEY,
                    .chunk_no = 0,
                }
                || head_row.bytes.size() != proven_head_bytes.size()
                || head_row.row_checksum
                    != bioseq::indexing::crc64_index_chunk_row(
                        0, -1, GENERATION_HEAD_SUB_KEY, 0,
                        static_cast<std::uint32_t>(head_row.bytes.size()),
                        head_row.bytes)) {
                rev3_storage_error("READY validation head row is malformed");
            }
            std::copy(head_row.bytes.begin(), head_row.bytes.end(),
                proven_head_bytes.begin());
            proven_head_checksum = head_row.row_checksum;

            std::uint64_t expected_payload_rows = 0;
            std::uint64_t expected_payload_bytes = 0;
            std::optional<std::pair<index_sub_key_t, std::int32_t>>
                previous_storage_key;
            for (const MinimizerBucketDirEntry& entry
                 : generation.directory.entries()) {
                const std::pair storage_key{
                    entry.sub_key, entry.position_segment};
                if (previous_storage_key
                    && !(*previous_storage_key < storage_key)) {
                    rev3_storage_error(
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
                    rev3_storage_error("READY expected payload counters overflow");
                }
            }
            std::uint64_t expected_total_rows = expected_payload_rows;
            if (!checked_add_(expected_total_rows,
                    generation.manifest.directory_chunk_count)
                || !checked_add_(expected_total_rows, 1)) {
                rev3_storage_error("READY expected row counter overflow");
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
                rev3_storage_error("sink write order/count/checksum disagrees");
            }

            flush_bounded_batch_as_cpp_();
            bioseq::pg_utils::invoke_pg_as_cpp(
                [&] { CatalogCloseIndexes(catalog_indexes_); });
            catalog_indexes_ = nullptr;
            bioseq::pg_utils::invoke_pg_as_cpp(
                [] { CommandCounterIncrement(); });
            if (!bioseq::pg_utils::invoke_pg_as_cpp(
                    [] { return ActiveSnapshotSet(); })) {
                rev3_storage_error(
                    "READY reread requires an active MVCC snapshot");
            }
            Snapshot active_snapshot = bioseq::pg_utils::invoke_pg_as_cpp(
                [] { return GetActiveSnapshot(); });
            if (!IsMVCCSnapshot(active_snapshot)) {
                rev3_storage_error(
                    "READY reread active snapshot is not MVCC");
            }
            bioseq::pg_utils::invoke_pg_as_cpp(
                [] { UpdateActiveSnapshotCommandId(); });
            if (active_snapshot->curcid
                != bioseq::pg_utils::invoke_pg_as_cpp(
                    [] { return GetCurrentCommandId(false); })) {
                rev3_storage_error(
                    "READY reread active command ID was not refreshed");
            }
            validation_snapshot_ = bioseq::pg_utils::invoke_pg_as_cpp(
                [&] { return RegisterSnapshot(active_snapshot); });
            if (validation_snapshot_ == nullptr
                || validation_snapshot_->regd_count == 0) {
                rev3_storage_error("READY reread snapshot was not registered");
            }

            validation_source_proof_.emplace(
                derive_rev3_authoritative_source_proof_pg(
                    identity_, query_key_, validation_snapshot_));
            Rev3AuthoritativeSourceProof& source_proof =
                *validation_source_proof_;
            if (!source_proof.valid_
                || source_proof.snapshot_ != validation_snapshot_
                || source_proof.database_oid_ != identity_.database_oid
                || source_proof.tablespace_oid_ != identity_.tablespace_oid
                || source_proof.main_relfilenode_
                    != identity_.main_relfilenode
                || source_proof.user_relid_ != identity_.user_relid
                || source_proof.column_attno_ != identity_.column_attno
                || source_proof.index_name_ != identity_.index_name
                || source_proof.table_name_ != identity_.table_name
                || source_proof.source_.sequence_rowno != query_key_) {
                rev3_storage_error(
                    "READY authoritative source proof binding disagrees");
            }
            const std::uint64_t compatibility_checksum =
                bioseq::indexing::minimizer_generation_compatibility_checksum(
                    generation.manifest);
            if (generation.manifest.alphabet_code
                    != source_proof.source_.alphabet_code
                || generation.manifest.storage_unit_byte_width
                    != source_proof.source_.storage_unit_byte_width
                || generation.manifest.target_length
                    != source_proof.source_.locator.total_bp_size
                || generation.head.compatibility_checksum
                    != compatibility_checksum
                || generation.head.source_content_checksum
                    != source_proof.source_content_checksum_
                || generation.manifest.source_content_checksum
                    != source_proof.source_content_checksum_) {
                rev3_storage_error(
                    "READY authoritative source content/compatibility disagrees");
            }
            source_proof.source_.compatibility_checksum =
                compatibility_checksum;
            const std::uint64_t required_source_identity =
                rev3_source_identity(
                    identity_.main_relfilenode, source_proof.source_);
            if (required_source_identity == 0
                || generation.head.source_identity
                    != required_source_identity
                || generation.manifest.source_identity
                    != required_source_identity) {
                rev3_storage_error(
                    "READY authoritative source identity disagrees");
            }
            proven_source_identity = required_source_identity;
            proven_source_content_checksum =
                source_proof.source_content_checksum_;
            proven_compatibility_checksum = compatibility_checksum;
            source_proof.invalidate_();
            validation_source_proof_.reset();

            const Rev3IndexSchemaInspection schema =
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [&] { return inspect_rev3_index_schema(
                        relation_, nullptr,
                        Rev3IndexSchemaExpectation{
                            .namespace_oid = bioseq_internal_ns(),
                            .relation_name = identity_.table_name,
                            .tablespace_oid = identity_.tablespace_oid,
                        }); });
            if (!schema) {
                rev3_storage_error("READY relation schema changed");
            }
            proven_relation_oid = RelationGetRelid(relation_);
            proven_locator = relation_->rd_locator;
            validation_primary_index_ = bioseq::pg_utils::invoke_pg_as_cpp(
                [&] {
                    return index_open(
                        schema.primary_index_oid, AccessShareLock);
                });

            ScanKeyData metadata_keys[4];
            ScanKeyInit(&metadata_keys[0], kRev3IdxGenerationId,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(generation_id_)));
            ScanKeyInit(&metadata_keys[1], kRev3IdxQueryKey,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(query_key_)));
            ScanKeyInit(&metadata_keys[2], kRev3IdxSubKey,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(DIRECTORY_SUB_KEY)));
            ScanKeyInit(&metadata_keys[3], kRev3IdxPositionSegment,
                BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(-1));
            validation_scan_ = bioseq::pg_utils::invoke_pg_as_cpp([&] {
                return systable_beginscan_ordered(
                    relation_, validation_primary_index_,
                    validation_snapshot_, 4, metadata_keys);
            });
            const auto next_validation_tuple = [&] {
                return bioseq::pg_utils::invoke_pg_as_cpp([&] {
                    return systable_getnext_ordered(
                        validation_scan_, ForwardScanDirection);
                });
            };
            HeapTuple tuple = next_validation_tuple();
            if (!HeapTupleIsValid(tuple)) {
                rev3_storage_error("READY manifest row is missing");
            }
            Rev3LoadedPgRow manifest_row =
                load_rev3_pg_row_or_error(relation_, tuple);
            inject_rev3_pg_failure_as_cpp(
                Rev3PgFailureInjection::generation_reread,
                "generation_reread");
            if (manifest_row.query_key != query_key_
                || manifest_row.address != generation.manifest_row.address
                || manifest_row.checksum
                    != generation.manifest_row.row_checksum
                || !std::ranges::equal(
                    manifest_row.bytes, generation.manifest_row.bytes())) {
                manifest_row.release();
                rev3_storage_error("READY manifest reread disagrees");
            }
            manifest_row.release();

            Crc64Ecma182 reread_directory_checksum;
            std::uint64_t reread_directory_bytes = 0;
            const std::span<const std::byte> expected_directory =
                generation.directory.encoded_bytes();
            for (std::uint32_t chunk_no = 1;
                 chunk_no <= generation.manifest.directory_chunk_count;
                 ++chunk_no) {
                tuple = next_validation_tuple();
                if (!HeapTupleIsValid(tuple)) {
                    rev3_storage_error("READY directory chunk is missing");
                }
                Rev3LoadedPgRow directory_row =
                    load_rev3_pg_row_or_error(relation_, tuple);
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
                    || directory_row.address != IndexChunkAddress{
                        .generation_id = generation_id_,
                        .position_segment = -1,
                        .sub_key = DIRECTORY_SUB_KEY,
                        .chunk_no = chunk_no,
                    }
                    || !std::ranges::equal(
                        directory_row.bytes, expected_bytes)) {
                    directory_row.release();
                    rev3_storage_error("READY directory reread disagrees");
                }
                reread_directory_checksum.update(directory_row.bytes);
                if (!checked_add_(reread_directory_bytes,
                        directory_row.bytes.size())) {
                    directory_row.release();
                    rev3_storage_error("READY directory reread bytes overflow");
                }
                directory_row.release();
            }
            if (HeapTupleIsValid(next_validation_tuple())) {
                rev3_storage_error("READY directory reread has extra rows");
            }
            bioseq::pg_utils::invoke_pg_as_cpp(
                [&] { systable_endscan_ordered(validation_scan_); });
            validation_scan_ = nullptr;
            if (reread_directory_bytes
                    != generation.manifest.directory_bytes
                || reread_directory_checksum.checksum()
                    != generation.manifest.directory_checksum) {
                rev3_storage_error(
                    "READY directory aggregate reread disagrees");
            }

            ScanKeyData payload_keys[3];
            ScanKeyInit(&payload_keys[0], kRev3IdxGenerationId,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(generation_id_)));
            ScanKeyInit(&payload_keys[1], kRev3IdxQueryKey,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(query_key_)));
            ScanKeyInit(&payload_keys[2], kRev3IdxPositionSegment,
                BTGreaterEqualStrategyNumber, F_INT4GE, Int32GetDatum(0));
            validation_scan_ = bioseq::pg_utils::invoke_pg_as_cpp([&] {
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
            Crc64Ecma182 extent_checksum;
            for (tuple = next_validation_tuple();
                 HeapTupleIsValid(tuple);
                 tuple = next_validation_tuple()) {
                if (payload_complete
                    || entry_ordinal >= directory_entries.size()) {
                    rev3_storage_error("READY payload reread has extra rows");
                }
                const MinimizerBucketDirEntry& entry =
                    directory_entries[entry_ordinal];
                const auto position_sub_key =
                    derive_minimizer_position_extent_sub_key(entry.sub_key);
                if (!position_sub_key) {
                    rev3_storage_error(
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
                Rev3LoadedPgRow payload_row =
                    load_rev3_pg_row_or_error(relation_, tuple);
                if (payload_row.query_key != query_key_
                    || payload_row.address != IndexChunkAddress{
                        .generation_id = generation_id_,
                        .position_segment = entry.position_segment,
                        .sub_key = position_extent
                            ? *position_sub_key : entry.sub_key,
                        .chunk_no = entry_chunk,
                    }
                    || payload_row.bytes.size() != expected_bytes) {
                    payload_row.release();
                    rev3_storage_error("READY payload row/address/size disagrees");
                }
                extent_checksum.update(payload_row.bytes);
                if (!checked_add_(reread_payload_rows, 1)
                    || !checked_add_(reread_payload_bytes,
                        payload_row.bytes.size())) {
                    payload_row.release();
                    rev3_storage_error("READY payload reread counters overflow");
                }
                if (format5_image_writer) {
                    format5_image_writer->append(payload_row.bytes);
                }
                payload_row.release();
                ++entry_chunk;
                if (entry_chunk == extent_chunk_count) {
                    const std::uint64_t expected_extent_checksum =
                        position_extent
                        ? entry.position_checksum : entry.control_checksum;
                    if (extent_checksum.checksum()
                        != expected_extent_checksum) {
                        rev3_storage_error(position_extent
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
            bioseq::pg_utils::invoke_pg_as_cpp(
                [&] { systable_endscan_ordered(validation_scan_); });
            validation_scan_ = nullptr;
            // The S1 PK is the physical authority: every global control
            // sub-key precedes every derived position sub-key. Legacy wire
            // payload checksums retain their historical emitter order, so the
            // sink validates that aggregate before this physical reread.
            if (!payload_complete
                || entry_chunk != 0
                || reread_payload_rows != expected_payload_rows
                || reread_payload_bytes != expected_payload_bytes
                || reread_payload_bytes != generation.manifest.payload_bytes) {
                rev3_storage_error(
                    "READY complete payload aggregate reread disagrees");
            }
            if (format5_image_writer) {
                proven_file_receipt = format5_image_writer->publish(
                    generation.head,
                    generation.manifest,
                    generation.directory,
                    generation.manifest_row.bytes());
            }

            bioseq::pg_utils::invoke_pg_as_cpp(
                [&] { index_close(validation_primary_index_, AccessShareLock); });
            validation_primary_index_ = nullptr;
            bioseq::pg_utils::invoke_pg_as_cpp(
                [&] { UnregisterSnapshot(validation_snapshot_); });
            validation_snapshot_ = nullptr;
            bioseq::pg_utils::invoke_pg_as_cpp(
                [&] { relation_close(relation_, RowExclusiveLock); });
            relation_ = nullptr;
            inject_rev3_pg_failure_as_cpp(
                Rev3PgFailureInjection::generation_writer_close,
                "generation_writer_close");
        } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
            abandon_after_pg_error_();
            throw;
        }

        stage_ = WriteStage::tokenized;
        const TransactionId proven_transaction_id =
            bioseq::pg_utils::invoke_pg_as_cpp(
                [] { return GetCurrentTransactionId(); });
        Rev3IndexAdministrationIdentity proven_identity =
            std::move(identity_);
        return ReadyGenerationToken{
            std::move(proven_identity),
            proven_relation_oid,
            proven_locator,
            advisory_lock_key_,
            query_key_,
            generation_id_,
            proven_source_identity,
            proven_source_content_checksum,
            proven_compatibility_checksum,
            proven_transaction_id,
            proven_head_bytes,
            proven_head_checksum,
            std::move(proven_file_receipt)};
    }
};

static_assert(bioseq::indexing::MinimizerGenerationRowSink<
    IndexStorageFragmentGenerationRowSinkPg>);
static_assert(bioseq::indexing::Format5MinimizerGenerationPayloadSink<
    IndexStorageFragmentGenerationRowSinkPg>);
static_assert(!std::is_default_constructible_v<
    IndexStorageFragmentGenerationRowSinkPg>);
static_assert(!std::is_copy_constructible_v<
    IndexStorageFragmentGenerationRowSinkPg>);
static_assert(std::is_nothrow_move_constructible_v<
    IndexStorageFragmentGenerationRowSinkPg>);
static_assert(!std::is_move_assignable_v<
    IndexStorageFragmentGenerationRowSinkPg>);

[[nodiscard]] inline std::optional<std::array<
    std::byte, MinimizerGenerationHead::encoded_bytes>>
read_locked_generation_head_for_descriptor_publication_pg(
        const std::string_view table_name,
        const std::uint64_t query_key) {
    std::array<std::byte, MinimizerGenerationHead::encoded_bytes> bytes{};
    std::array<Oid, 5u> types{
        INT8OID, INT8OID, INT8OID, INT4OID, INT4OID};
    std::array<Datum, 5u> arguments{
        Int64GetDatum(static_cast<std::int64_t>(query_key)),
        Int64GetDatum(0),
        Int64GetDatum(static_cast<std::int64_t>(GENERATION_HEAD_SUB_KEY)),
        Int32GetDatum(-1),
        Int32GetDatum(0),
    };
    const std::string owned_table_name{table_name};
    const char* quoted_table = bioseq::pg_utils::invoke_pg_as_cpp([&] {
        return quote_identifier(owned_table_name.c_str());
    });
    const std::string sql = fmt::format(
        "SELECT blob FROM bioseq_internal.{} "
        "WHERE query_key=$1 AND generation_id=$2 AND sub_key=$3 "
        "AND position_segment=$4 AND chunk_no=$5 FOR UPDATE",
        quoted_table);
    struct State {
        int connect = 0;
        int execute = 0;
        int finish = 0;
        std::uint64_t rows = 0u;
        bool table = false;
        bool is_null = true;
        bool exact_size = false;
    } state{};
    bioseq::pg_utils::invoke_pg_as_cpp(
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
        rev3_storage_error_as_cpp(fmt::format(
            "descriptor publication head lock disagrees: connect={} "
            "execute={} rows={} table={} null={} size={} finish={}",
            state.connect, state.execute, state.rows, state.table,
            state.is_null, state.exact_size, state.finish));
    }
    if (state.rows == 0u) return std::nullopt;
    return bytes;
}

[[nodiscard]] inline std::optional<MinimizerGenerationStorageDescriptorPg>
read_locked_live_descriptor_for_publication_pg(
        const Rev3IndexAdministrationIdentity& identity,
        const std::uint64_t query_key) {
    NameData index_name{};
    bioseq::pg_utils::invoke_pg_as_cpp([&] {
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
    static constexpr std::string_view sql{
        "SELECT * FROM bioseq_internal.minimizer_generation_storage "
        "WHERE database_oid=$1 AND tablespace_oid=$2 "
        "AND main_relfilenode=$3 AND user_relid=$4 "
        "AND column_attno=$5 AND index_name=$6 AND query_key=$7 "
        "AND state=smallint '1' FOR UPDATE"};
    struct RawRow {
        Oid database_oid = InvalidOid;
        Oid tablespace_oid = InvalidOid;
        Oid main_relfilenode = InvalidOid;
        Oid user_relid = InvalidOid;
        std::int16_t column_attno = 0;
        NameData index_name{};
        std::int64_t query_key = -1;
        std::int64_t generation_id = 0;
        std::int16_t provider = -1;
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
    struct State {
        int connect = 0;
        int execute = 0;
        int finish = 0;
        std::uint64_t rows = 0u;
        bool table = false;
    } state{};
    bioseq::pg_utils::invoke_pg_as_cpp(
        [&state, &row, &types, &arguments] noexcept {
            state.connect = SPI_connect();
            if (state.connect != SPI_OK_CONNECT) return;
            state.execute = SPI_execute_with_args(
                sql.data(), static_cast<int>(types.size()), types.data(),
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
                row.provider = DatumGetInt16(required(9));
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
        rev3_storage_error_as_cpp(fmt::format(
            "descriptor publication LIVE lookup disagrees: connect={} "
            "execute={} rows={} table={} required_null={} finish={}",
            state.connect, state.execute, state.rows, state.table,
            row.required_null, state.finish));
    }
    if (state.rows == 0u) return std::nullopt;
    if (row.query_key < 0 || row.generation_id <= 0
        || row.image_bytes
            < static_cast<std::int64_t>(
                bioseq::indexing::kMpf5V2HeaderBytes)
        || !bioseq::indexing::is_valid_mpf5_v2_provider(
            static_cast<std::uint8_t>(row.provider))) {
        rev3_storage_error_as_cpp(
            "descriptor publication LIVE row scalar shape is invalid");
    }
    MinimizerGenerationStorageDescriptorPg result{
        .database_oid = row.database_oid,
        .tablespace_oid = row.tablespace_oid,
        .main_relfilenode = row.main_relfilenode,
        .user_relid = row.user_relid,
        .column_attno = row.column_attno,
        .index_name = NameStr(row.index_name),
        .query_key = static_cast<std::uint64_t>(row.query_key),
        .generation_id = static_cast<std::uint64_t>(row.generation_id),
        .provider = static_cast<bioseq::indexing::Mpf5V2Provider>(
            row.provider),
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
        .state = static_cast<MinimizerGenerationStorageStatePg>(row.state),
    };
    if (!exact_storage_descriptor_identity(result, identity, query_key)
        || !exact_storage_descriptor_provider_shape(result)
        || result.state != MinimizerGenerationStorageStatePg::live
        || result.retire_full_xid) {
        rev3_storage_error_as_cpp(
            "descriptor publication LIVE row shape disagrees");
    }
    return result;
}

inline void apply_minimizer_generation_storage_transition_pg(
        const Rev3IndexAdministrationIdentity& identity,
        const std::uint64_t query_key,
        const std::optional<MinimizerGenerationStorageDescriptorPg>& old_live,
        const std::optional<MinimizerGenerationStorageDescriptorPg>& new_live) {
    if (!old_live && !new_live) return;
    if ((old_live && (!exact_storage_descriptor_identity(
                *old_live, identity, query_key)
            || old_live->state != MinimizerGenerationStorageStatePg::live
            || old_live->retire_full_xid))
        || (new_live && (!exact_storage_descriptor_identity(
                *new_live, identity, query_key)
            || new_live->state != MinimizerGenerationStorageStatePg::live
            || new_live->retire_full_xid
            || !exact_storage_descriptor_provider_shape(*new_live)
            || !bioseq::indexing::sql_bigint_can_represent(
                new_live->generation_id)
            || !bioseq::indexing::sql_bigint_can_represent(
                new_live->image_bytes)))) {
        rev3_storage_error_as_cpp(
            "generation storage transition binding is invalid");
    }

    NameData index_name{};
    NameData payload_relation_name{};
    bioseq::pg_utils::invoke_pg_as_cpp([&] {
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
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [] { return GetTopFullTransactionId(); }))
            : FullTransactionIdGetDatum(InvalidFullTransactionId),
    };
    static constexpr std::string_view update_sql{
        "UPDATE bioseq_internal.minimizer_generation_storage "
        "SET retire_full_xid=$9, state=smallint '2' "
        "WHERE database_oid=$1 AND tablespace_oid=$2 "
        "AND main_relfilenode=$3 AND user_relid=$4 "
        "AND column_attno=$5 AND index_name=$6 AND query_key=$7 "
        "AND generation_id=$8 AND state=smallint '1' "
        "AND retire_full_xid IS NULL"};

    std::array<Oid, kStorageDescriptorAttributeCount> insert_types{
        OIDOID, OIDOID, OIDOID, OIDOID, INT2OID, NAMEOID,
        INT8OID, INT8OID, INT2OID,
        OIDOID, OIDOID, OIDOID, OIDOID, NAMEOID,
        INT8OID, INT8OID, XID8OID, INT2OID};
    std::array<Datum, kStorageDescriptorAttributeCount> insert_arguments{};
    std::array<char, kStorageDescriptorAttributeCount> insert_nulls{};
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
            Int16GetDatum(static_cast<std::int16_t>(new_live->provider)),
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
                MinimizerGenerationStorageStatePg::live)),
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
    static constexpr std::string_view insert_sql{
        "INSERT INTO bioseq_internal.minimizer_generation_storage "
        "(database_oid,tablespace_oid,main_relfilenode,user_relid,"
        "column_attno,index_name,query_key,generation_id,provider,"
        "payload_relation_oid,payload_database_oid,payload_tablespace_oid,"
        "payload_relfilenode,payload_relation_name,image_bytes,"
        "header_checksum,retire_full_xid,state) VALUES "
        "($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16,"
        "$17,$18)"};
    struct State {
        int connect = 0;
        int update = 0;
        int insert = 0;
        int finish = 0;
        std::uint64_t updated = 0u;
        std::uint64_t inserted = 0u;
    } state{};
    bioseq::pg_utils::invoke_pg_as_cpp(
        [&state, &update_types, &update_arguments, &insert_types,
         &insert_arguments, &insert_nulls, &old_live,
         &new_live] noexcept {
            state.connect = SPI_connect();
            if (state.connect != SPI_OK_CONNECT) return;
            if (old_live) {
                state.update = SPI_execute_with_args(
                    update_sql.data(), static_cast<int>(update_types.size()),
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
                    insert_sql.data(), static_cast<int>(insert_types.size()),
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
        rev3_storage_error_as_cpp(fmt::format(
            "generation storage transition disagrees: connect={} "
            "update={} updated={} insert={} inserted={} finish={}",
            state.connect, state.update, state.updated, state.insert,
            state.inserted, state.finish));
    }
}

/// Consume the private complete-reread proof and publish its head atomically.
/// There is no public overload that accepts an arbitrary relation/generation.
[[nodiscard]] inline ReadyGenerationPublishedHeadReceipt
publish_ready_minimizer_generation_head_pg(
        ReadyGenerationToken&& token) {
    const auto rev3_storage_error = [](std::string_view message) {
        rev3_storage_error_as_cpp(message);
    };
    struct PublicationState {
        Relation relation = nullptr;
        Relation descriptor_relation = nullptr;
        bytea* blob = nullptr;
        std::string sql;
    };
    auto* publication = new PublicationState{};
    std::array<std::byte, MinimizerGenerationHead::encoded_bytes>
        returned_head_bytes{};
    std::array<std::byte, MinimizerGenerationHead::encoded_bytes>
        previous_head_bytes{};
    bool returned_valid = false;
    bool previous_present = false;
    std::optional<MinimizerGenerationHead> decoded_previous_head{};

    try {
        if (!token.valid_
            || !bioseq::indexing::sql_bigint_can_represent(token.query_key_)
            || token.generation_id_ == 0
            || token.identity_.database_oid != MyDatabaseId
            || !TransactionIdIsValid(token.transaction_id_)
            || token.transaction_id_
                != bioseq::pg_utils::invoke_pg_as_cpp(
                    [] { return GetCurrentTransactionIdIfAny(); })
            || token.advisory_lock_key_
                != index_advisory_xact_lock_key(
                    token.identity_.database_oid,
                    token.identity_.tablespace_oid,
                    token.identity_.main_relfilenode,
                    token.identity_.column_attno,
                    token.identity_.index_name)) {
            rev3_storage_error("READY publication token is invalid");
        }
        bioseq::pg_utils::invoke_pg_as_cpp([&] {
            acquire_index_advisory_xact_lock(
                token.identity_.database_oid,
                token.identity_.tablespace_oid,
                token.identity_.main_relfilenode,
                token.identity_.column_attno,
                token.identity_.index_name);
        });
        publication->relation = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            return try_safe_relation_open(
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
            rev3_storage_error(
                "READY publication relation identity changed after reread");
        }
        const Rev3IndexSchemaInspection schema =
            bioseq::pg_utils::invoke_pg_as_cpp([&] {
                return inspect_rev3_index_schema(
                    publication->relation, nullptr,
                    Rev3IndexSchemaExpectation{
                        .namespace_oid = bioseq_internal_ns(),
                        .relation_name = token.identity_.table_name,
                        .tablespace_oid = token.identity_.tablespace_oid,
                    });
            });
        if (!schema) {
            rev3_storage_error("READY publication schema changed after reread");
        }
        auto decoded_token_head =
            bioseq::indexing::decode_minimizer_generation_head(
                token.head_bytes_);
        if (!decoded_token_head
            || decoded_token_head->generation_id != token.generation_id_
            || decoded_token_head->source_identity != token.source_identity_
            || decoded_token_head->source_content_checksum
                != token.source_content_checksum_
            || decoded_token_head->compatibility_checksum
                != token.compatibility_checksum_
            || token.head_row_checksum_
                != bioseq::indexing::crc64_index_chunk_row(
                    0, -1, GENERATION_HEAD_SUB_KEY, 0,
                    static_cast<std::uint32_t>(token.head_bytes_.size()),
                    token.head_bytes_)) {
            rev3_storage_error("READY publication token head disagrees");
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
                rev3_storage_error(
                    "READY publication previous head is malformed or names the new generation");
            }
            decoded_previous_head = *decoded_previous_head_result;
        }

        const bool fragment_index = token.identity_.index_name
            == MINIMIZERS_PER_FRAGMENT_INDEX_NAME;
        const bool new_format5 = decoded_token_head->wire_format
            == bioseq::indexing::MinimizerGenerationWireFormat::
                grouped_shape_soa_segment_local;
        if (!fragment_index && token.file_receipt_) {
            rev3_storage_error(
                "non-MPF generation carried an MPF5 file receipt");
        }
        if (fragment_index) {
            const Oid descriptor_oid =
                bioseq::pg_utils::invoke_pg_as_cpp([&] {
                    return get_relname_relid(
                        "minimizer_generation_storage",
                        bioseq_internal_ns());
                });
            if (!OidIsValid(descriptor_oid)) {
                rev3_storage_error(
                    "generation storage descriptor catalog is missing");
            }
            publication->descriptor_relation =
                bioseq::pg_utils::invoke_pg_as_cpp([&] {
                    return try_safe_relation_open(
                        descriptor_oid, RowExclusiveLock);
                });
            if (publication->descriptor_relation == nullptr
                || RelationGetNamespace(publication->descriptor_relation)
                    != bioseq_internal_ns()
                || std::string_view{RelationGetRelationName(
                    publication->descriptor_relation)}
                    != "minimizer_generation_storage") {
                rev3_storage_error(
                    "generation storage descriptor catalog identity changed");
            }
            bioseq::storage::
                validate_minimizer_generation_storage_catalogs_pg();

            auto old_live = read_locked_live_descriptor_for_publication_pg(
                token.identity_, token.query_key_);
            const bool old_format5 = decoded_previous_head
                && decoded_previous_head->wire_format
                    == bioseq::indexing::MinimizerGenerationWireFormat::
                        grouped_shape_soa_segment_local;
            if (old_format5 != old_live.has_value()
                || (old_live
                    && old_live->generation_id
                        != decoded_previous_head->generation_id)) {
                rev3_storage_error(
                    "active head and LIVE generation descriptor disagree");
            }
            if (old_live
                && old_live->provider
                    == bioseq::indexing::Mpf5V2Provider::postgres_relation
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
                rev3_storage_error(
                    "old relation-provider descriptor locator changed");
            }
            if (new_format5 != token.file_receipt_.has_value()) {
                rev3_storage_error(
                    "new MPF Format-5 head and file receipt disagree");
            }
            std::optional<MinimizerGenerationStorageDescriptorPg> new_live;
            if (token.file_receipt_) {
                const Mpf5V2FileReceiptPg& file = *token.file_receipt_;
                if (!bioseq::indexing::is_valid_mpf5_v2_provider(
                        static_cast<std::uint8_t>(file.provider))
                    || file.file_bytes
                        < bioseq::indexing::kMpf5V2HeaderBytes
                    || !bioseq::indexing::sql_bigint_can_represent(
                        file.file_bytes)) {
                    rev3_storage_error(
                        "new MPF5 file receipt cannot be cataloged");
                }
                new_live = MinimizerGenerationStorageDescriptorPg{
                    .database_oid = token.identity_.database_oid,
                    .tablespace_oid = token.identity_.tablespace_oid,
                    .main_relfilenode = token.identity_.main_relfilenode,
                    .user_relid = token.identity_.user_relid,
                    .column_attno = token.identity_.column_attno,
                    .index_name = token.identity_.index_name,
                    .query_key = token.query_key_,
                    .generation_id = token.generation_id_,
                    .provider = file.provider,
                    .payload_relation_oid = file.provider
                            == bioseq::indexing::Mpf5V2Provider::
                                postgres_relation
                        ? std::optional<Oid>{token.sidecar_relation_oid_}
                        : std::nullopt,
                    .payload_database_oid = file.provider
                            == bioseq::indexing::Mpf5V2Provider::
                                postgres_relation
                        ? std::optional<Oid>{token.sidecar_locator_.dbOid}
                        : std::nullopt,
                    .payload_tablespace_oid = file.provider
                            == bioseq::indexing::Mpf5V2Provider::
                                postgres_relation
                        ? std::optional<Oid>{token.sidecar_locator_.spcOid}
                        : std::nullopt,
                    .payload_relfilenode = file.provider
                            == bioseq::indexing::Mpf5V2Provider::
                                postgres_relation
                        ? std::optional<Oid>{
                            static_cast<Oid>(
                                token.sidecar_locator_.relNumber)}
                        : std::nullopt,
                    .payload_relation_name = file.provider
                            == bioseq::indexing::Mpf5V2Provider::
                                postgres_relation
                        ? std::optional<std::string>{
                            token.identity_.table_name}
                        : std::nullopt,
                    .image_bytes = file.file_bytes,
                    .header_checksum = file.header_crc64,
                    .retire_full_xid = std::nullopt,
                    .state = MinimizerGenerationStorageStatePg::live,
                };
            }
            apply_minimizer_generation_storage_transition_pg(
                token.identity_, token.query_key_, old_live, new_live);
        }

        publication->blob = bioseq::pg_utils::invoke_pg_as_cpp([&] {
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
        Int64GetDatum(static_cast<std::int64_t>(GENERATION_HEAD_SUB_KEY)),
        Int32GetDatum(0),
        Int32GetDatum(static_cast<std::int32_t>(token.head_bytes_.size())),
        Int64GetDatum(checksum_to_pg_bigint(token.head_row_checksum_)),
        PointerGetDatum(publication->blob),
    };
    const char* quoted_table = bioseq::pg_utils::invoke_pg_as_cpp([&] {
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
        struct PublishSpiState {
            int connect_status = 0;
            int execute_status = 0;
            int finish_status = 0;
            std::uint64_t processed = 0;
            bool table_present = false;
            bool returned_is_null = true;
            bool returned_size_valid = false;
        } spi{};
        const char* publication_sql = publication->sql.c_str();
        bioseq::pg_utils::invoke_pg_as_cpp(
            [&spi, publication_sql, &argument_types, &arguments,
             &returned_head_bytes] noexcept {
                spi.connect_status = SPI_connect();
                if (spi.connect_status != SPI_OK_CONNECT) return;
                spi.execute_status = SPI_execute_with_args(
                    publication_sql,
                    static_cast<int>(argument_types.size()),
                    argument_types.data(), arguments.data(),
                    nullptr, false, 1);
                inject_rev3_pg_failure_leaf(
                    Rev3PgFailureInjection::generation_spi_publish,
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
            rev3_storage_error(fmt::format(
                "READY publication SPI session disagrees: connect={} "
                "execute={} rows={} table={} null={} size={} finish={}",
                spi.connect_status, spi.execute_status, spi.processed,
                spi.table_present, spi.returned_is_null,
                spi.returned_size_valid, spi.finish_status));
        }
        bytea* finished_blob = std::exchange(publication->blob, nullptr);
        bioseq::pg_utils::invoke_pg_as_cpp(
            [&] { pfree(finished_blob); });
        if (publication->descriptor_relation != nullptr) {
            Relation finished_descriptor = std::exchange(
                publication->descriptor_relation, nullptr);
            bioseq::pg_utils::invoke_pg_as_cpp([&] {
                relation_close(finished_descriptor, RowExclusiveLock);
            });
        }
        Relation finished_relation = std::exchange(
            publication->relation, nullptr);
        bioseq::pg_utils::invoke_pg_as_cpp(
            [&] { relation_close(finished_relation, RowExclusiveLock); });
        auto returned_head =
            bioseq::indexing::decode_minimizer_generation_head(
                returned_head_bytes);
        if (!returned_valid || !returned_head
            || !bioseq::indexing::revision3_fixed_wire_returned_exactly(
                token.head_bytes_, returned_head_bytes)
            || *returned_head != *decoded_token_head) {
            rev3_storage_error(
                "READY publication did not return the byte-exact proven head");
        }
        std::optional<std::uint64_t> replaced_generation_id;
        if (previous_present) {
            const auto previous_head =
                bioseq::indexing::decode_minimizer_generation_head(
                    previous_head_bytes);
            if (!previous_head || previous_head->generation_id == 0
                || previous_head->generation_id
                    == returned_head->generation_id) {
                rev3_storage_error(
                    "READY publication previous head is malformed or names the new generation");
            }
            replaced_generation_id = previous_head->generation_id;
        }
        ReadyGenerationPublishedHeadReceipt receipt{
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
    } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
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

struct MinimizerGenerationReadMetadata {
    MinimizerGenerationHead head{};
    MinimizerGenerationManifest manifest{};
    DirView directory{};
};

/// One caller-declared admission group. A group is one exact ordered PG range
/// scan; every request must match one complete directory control/position
/// extent, and every skipped row must be predeclared by that directory.
struct MinimizerGenerationPayloadReadGroup {
    IndexSubKeyRange range{};
    std::vector<IndexSubKeyRequest> requests;
};

struct MinimizerGenerationPayloadPhaseReadStats {
    std::uint64_t plan_calls = 0;
    std::uint64_t planned_request_groups = 0;
    std::uint64_t executed_request_groups = 0;
    std::uint64_t range_scan_calls = 0;
    std::uint64_t physical_heap_rows_read = 0;
    std::uint64_t physical_bytes_read = 0;
    std::uint64_t control_authorization_checks = 0;
    IndexChunkReadStats read{};
};

/// Proof counters for the full-generation relation-scan route.  Unlike the
/// selected-range reader, this route consumes every payload row through one
/// ordered primary-index scan after metadata/source attestation has completed.
struct MinimizerGenerationFullPayloadReadStats {
    std::uint64_t scan_calls = 0;
    std::uint64_t planned_extents = 0;
    std::uint64_t completed_extents = 0;
    std::uint64_t control_chunks = 0;
    std::uint64_t position_chunks = 0;
    std::uint64_t control_bytes = 0;
    std::uint64_t position_bytes = 0;
    std::uint64_t payload_row_checksum = 0;
};

/// Compact Format-6-specific evidence nested under the existing generation
/// read receipt. It carries only correctness/performance gates that cannot be
/// reconstructed from the legacy control/position extent fields.
struct MinimizerFormat6ColdReadEvidence {
    std::uint64_t format_version = 0;
    std::uint64_t sessions = 0;
    std::uint64_t generation_id = 0;
    std::array<std::byte, 16> source_identity{};
    std::uint64_t source_content_crc64 = 0;
    std::array<std::byte, 32> compatibility_digest{};
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
    std::uint64_t directory_phase_peak_bytes = 0;
    std::uint64_t control_phase_peak_bytes = 0;
    std::uint64_t posting_phase_peak_bytes = 0;
    std::uint64_t peak_admitted_bytes = 0;
    std::uint64_t retained_owner_bytes = 0;
    std::uint64_t admitted_bytes = 0;
    std::uint64_t failures = 0;
    bool saturated = false;
};

struct MinimizerGenerationReadStats {
    ColdIndexPreparationStatsPg cold{};
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
    IndexChunkReadStats payload{};
    MinimizerGenerationPayloadPhaseReadStats controls{};
    MinimizerGenerationPayloadPhaseReadStats positions{};
    MinimizerGenerationFullPayloadReadStats full_generation{};
    MinimizerFormat6ColdReadEvidence format6{};
};

namespace rev3_full_generation_crc_detail {

[[nodiscard]] inline bool concatenation_self_test() noexcept {
    return bioseq::indexing::crc64_ecma182_concatenate(
        bioseq::indexing::crc64_ecma182(std::string_view{"control"}),
        bioseq::indexing::crc64_ecma182(std::string_view{"position"}),
        std::string_view{"position"}.size())
        == bioseq::indexing::crc64_ecma182(
            std::string_view{"controlposition"});
}

} // namespace rev3_full_generation_crc_detail

enum class Rev3SnapshotRegistration : std::uint8_t {
    register_copy,
    borrow_registered,
};

/// Inactive revision-3 target session. It binds one fresh heap OID, its exact
/// five-key primary index, one executor-owned data snapshot, and one query key
/// for the complete head -> metadata -> controls -> positions -> finish
/// lifecycle. Both payload phases retain the same open relations and snapshot.
class IndexStorageFragmentGenerationReadSessionPg {
private:
    enum class State : std::uint8_t {
        opened,
        head_read,
        metadata_unattested,
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

    enum class PayloadPhase : std::uint8_t {
        controls,
        positions,
    };

    struct BoundGrouped32ColdRoute {
        MinimizerGrouped32ColdLoadRoute route =
            MinimizerGrouped32ColdLoadRoute::empty;
        std::uint64_t control_groups = 0;
        std::uint64_t position_groups_upper_bound = 0;
    };

    static constexpr std::uint64_t kOrderedRangeScanPenaltyRows =
        bioseq::indexing::kMinimizerGrouped32ColdRouteRangePenaltyRows;

    using LoadedRow = Rev3LoadedPgRow;

    struct PlannedGroup {
        IndexSubKeyRange range{};
        std::vector<IndexSubKeyRequest> requests;
        std::size_t first_directory_ordinal = 0;
        std::size_t end_directory_ordinal = 0;
        std::uint64_t skip_budget_rows = 0;
        bool position_extent = false;
        bool executed = false;
    };

    struct RuntimeRequest {
        IndexSubKeyRequest request{};
        std::uint64_t end_chunk_no = 0;
        std::uint64_t next_chunk_no = 0;
        std::uint64_t total_bytes = 0;
        Crc64Ecma182 checksum{};
    };

    std::string table_name_;
    Relation heap_relation_ = nullptr;
    Relation primary_index_relation_ = nullptr;
    Relation storage_descriptor_relation_ = nullptr;
    Relation storage_descriptor_live_index_relation_ = nullptr;
    SysScanDesc active_scan_ = nullptr;
    std::array<TupleTableSlot*, kRev3FullGenerationPinnedBatchRows>
        full_generation_batch_slots_{};
    std::size_t full_generation_batch_slot_count_ = 0;
    Snapshot snapshot_ = nullptr;
    bool snapshot_registered_by_session_ = false;
    std::uint64_t query_key_ = 0;
    Oid expected_tablespace_oid_ = InvalidOid;
    ExpectedMinimizerCompatibility expected_{};
    MinimizerGenerationPayloadProfile expected_profile_{};
    State state_ = State::opened;
    bool catalog_source_attested_ = false;
    // Permits precede every allocation they cover, so reverse member
    // destruction cannot return admitted bytes while an owned view/buffer is
    // still alive.
    bioseq::parallelism::MemoryPermit metadata_permit_;
    bioseq::parallelism::MemoryPermit plan_permit_;
    bioseq::parallelism::MemoryPermit runtime_permit_;
    bioseq::parallelism::MemoryPermit control_authorization_permit_;
    std::optional<MinimizerGenerationHead> head_;
    std::optional<MinimizerGenerationReadMetadata> metadata_;
    std::optional<MinimizerGenerationManifest> manifest_only_;
    std::vector<std::byte> directory_bytes_scratch_;
    std::vector<PlannedGroup> planned_groups_;
    std::vector<RuntimeRequest> runtime_requests_;
    std::vector<std::uint32_t> authorized_position_ordinals_;
    std::size_t next_group_ = 0;
    std::optional<std::uint64_t> previous_payload_heap_block_;
    std::optional<std::uint64_t> payload_stage_begin_ns_;
    std::optional<BoundGrouped32ColdRoute> grouped32_cold_route_;
    MinimizerGenerationReadStats stats_{};

    /// Return the sole authenticated manifest authority. Full metadata and
    /// manifest-only state are mutually exclusive; observing both or neither
    /// fails closed at every transition that consumes a manifest.
    [[nodiscard]] const MinimizerGenerationManifest*
    active_manifest_() const noexcept {
        if (metadata_.has_value() == manifest_only_.has_value()) {
            return nullptr;
        }
        return metadata_
            ? &metadata_->manifest : &*manifest_only_;
    }

    [[noreturn]] void fail_(std::string_view message) {
        state_ = State::failed;
        rev3_storage_error_as_cpp(message);
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
    /// unavailable (zero) until their owning phases expose coarse boundaries;
    /// no tuple/chunk-local clock reads are permitted on the cold path.
    void finalize_payload_timing_() noexcept {
        if (!payload_stage_begin_ns_) return;
        const std::uint64_t end_ns = cold_index_now_ns();
        const std::uint64_t raw_total_ns =
            end_ns >= *payload_stage_begin_ns_
                ? end_ns - *payload_stage_begin_ns_ : 0;
        const std::uint64_t total_ns = std::min(
            raw_total_ns, ColdIndexPreparationStatsPg::sql_bigint_max);
        if (raw_total_ns != total_ns) {
            stats_.cold.stats_saturated = true;
        }
        stats_.cold.add(stats_.cold.payload_ns, total_ns);
        payload_stage_begin_ns_.reset();
    }

    void release_owned_state_() noexcept {
        // Free/reset every covered allocation before returning admission.
        std::vector<std::byte>{}.swap(directory_bytes_scratch_);
        std::vector<PlannedGroup>{}.swap(planned_groups_);
        std::vector<RuntimeRequest>{}.swap(runtime_requests_);
        std::vector<std::uint32_t>{}.swap(authorized_position_ordinals_);
        metadata_.reset();
        manifest_only_.reset();
        head_.reset();
        grouped32_cold_route_.reset();
        runtime_permit_.release();
        plan_permit_.release();
        control_authorization_permit_.release();
        metadata_permit_.release();
    }

    void release_phase_plan_() noexcept {
        std::vector<PlannedGroup>{}.swap(planned_groups_);
        next_group_ = 0;
        plan_permit_.release();
    }

    void release_control_authorization_() noexcept {
        std::vector<std::uint32_t>{}.swap(authorized_position_ordinals_);
        control_authorization_permit_.release();
    }

    void checked_add_read_stats_(
            IndexChunkReadStats& destination,
            const IndexChunkReadStats& increment) {
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
            const IndexChunkReadStats& total,
            const IndexChunkReadStats& left,
            const IndexChunkReadStats& right) noexcept {
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
            const IndexChunkReadStats& stats) noexcept {
        return read_stats_are_sum_(
            stats, IndexChunkReadStats{}, IndexChunkReadStats{});
    }

    [[nodiscard]] static constexpr bool phase_stats_are_zero_(
            const MinimizerGenerationPayloadPhaseReadStats& phase) noexcept {
        return phase.plan_calls == 0u
            && phase.planned_request_groups == 0u
            && phase.executed_request_groups == 0u
            && phase.range_scan_calls == 0u
            && phase.physical_heap_rows_read == 0u
            && phase.physical_bytes_read == 0u
            && phase.control_authorization_checks == 0u
            && read_stats_are_zero_(phase.read);
    }

    [[nodiscard]] static constexpr bool full_payload_stats_are_zero_(
            const MinimizerGenerationFullPayloadReadStats& full) noexcept {
        return full.scan_calls == 0u
            && full.planned_extents == 0u
            && full.completed_extents == 0u
            && full.control_chunks == 0u
            && full.position_chunks == 0u
            && full.control_bytes == 0u
            && full.position_bytes == 0u
            && full.payload_row_checksum == 0u;
    }

    [[nodiscard]] static constexpr bool phase_equations_hold_(
            const MinimizerGenerationPayloadPhaseReadStats& phase) noexcept {
        return phase.plan_calls == 1
            && phase.planned_request_groups
                == phase.executed_request_groups
            && phase.executed_request_groups == phase.range_scan_calls
            && phase.read.relation_opens == 0
            && phase.read.index_scans == phase.range_scan_calls
            && phase.read.requested_ranges == phase.range_scan_calls
            && phase.read.heap_rows_read == phase.physical_heap_rows_read
            && phase.read.heap_rows_read
                == phase.read.delivered_chunks + phase.read.skipped_rows
            && phase.read.toast_values_read == 0
            && phase.read.detoast_bytes == 0
            && phase.read.explicit_copy_bytes == 0
            && phase.read.wanted_subkeys == phase.read.read_subkeys
            && phase.read.wanted_extents == phase.read.read_extents
            && phase.read.wanted_chunks == phase.read.delivered_chunks
            && phase.read.skipped_rows == phase.read.skip_budget_rows
            && phase.read.repeated_chunk_reads == 0
            && phase.physical_bytes_read >= phase.read.delivered_bytes;
    }

    /// Allocate every auxiliary BufferHeapTuple slot before the batch reader
    /// starts. The count advances after each successful allocation so an ERROR
    /// part-way through setup leaves an exact cleanup prefix.
    void create_full_generation_batch_slots_checked_() {
        bioseq::pg_utils::invoke_pg_as_cpp(
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
        bioseq::pg_utils::invoke_pg_as_cpp(
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
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [&] { systable_endscan_ordered(active_scan_); });
                active_scan_ = nullptr;
            }
            if (storage_descriptor_live_index_relation_ != nullptr) {
                bioseq::pg_utils::invoke_pg_as_cpp([&] {
                    index_close(
                        storage_descriptor_live_index_relation_,
                        AccessShareLock);
                });
                storage_descriptor_live_index_relation_ = nullptr;
                stats_.cold.add(stats_.cold.lock_releases, 1);
            }
            if (storage_descriptor_relation_ != nullptr) {
                bioseq::pg_utils::invoke_pg_as_cpp([&] {
                    relation_close(
                        storage_descriptor_relation_, AccessShareLock);
                });
                storage_descriptor_relation_ = nullptr;
                stats_.cold.add(stats_.cold.lock_releases, 1);
            }
            if (primary_index_relation_ != nullptr) {
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [&] { index_close(primary_index_relation_, AccessShareLock); });
                primary_index_relation_ = nullptr;
                stats_.cold.add(stats_.cold.lock_releases, 1);
            }
            if (heap_relation_ != nullptr) {
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [&] { relation_close(heap_relation_, AccessShareLock); });
                heap_relation_ = nullptr;
                stats_.cold.add(stats_.cold.lock_releases, 1);
            }
            if (snapshot_registered_by_session_ && snapshot_ != nullptr) {
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [&] { UnregisterSnapshot(snapshot_); });
            }
            snapshot_ = nullptr;
            snapshot_registered_by_session_ = false;
            release_owned_state_();
            add_cold_index_elapsed(
                stats_.cold, stats_.cold.close_ns, close_begin_ns);
            add_cold_index_buffer_usage(stats_.cold, close_buffer_begin);
        } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
            state_ = State::failed;
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
        state_ = State::failed;
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

    struct ProjectedExtentRange {
        index_sub_key_t logical_begin = 0;
        index_sub_key_t logical_end = 0;
        bool position_extent = false;
    };

    /// Project one physical range onto the logical/control directory key
    /// namespace. A range may cover controls or positions, never cross the
    /// namespace boundary.
    [[nodiscard]] static std::optional<ProjectedExtentRange>
    project_extent_range_(const IndexSubKeyRange& range) noexcept {
        if (range.begin < 0 || range.begin >= range.end
            || range.end > TEMP_RUN_BASE) {
            return std::nullopt;
        }
        if (range.end <= POSITION_EXTENT_BASE) {
            return ProjectedExtentRange{
                .logical_begin = range.begin,
                .logical_end = range.end,
                .position_extent = false,
            };
        }
        if (range.begin >= POSITION_EXTENT_BASE) {
            return ProjectedExtentRange{
                .logical_begin = range.begin - POSITION_EXTENT_BASE,
                .logical_end = range.end - POSITION_EXTENT_BASE,
                .position_extent = true,
            };
        }
        return std::nullopt;
    }

    [[nodiscard]] static bool request_matches_extent_(
            const IndexSubKeyRequest& request,
            const MinimizerBucketDirEntry& entry,
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
    IndexStorageFragmentGenerationReadSessionPg() = delete;
    IndexStorageFragmentGenerationReadSessionPg(
            std::string_view table_name,
            std::uint64_t query_key,
            Snapshot executor_snapshot,
            ExpectedMinimizerCompatibility expected,
            Oid expected_tablespace_oid,
            Rev3SnapshotRegistration snapshot_registration =
                Rev3SnapshotRegistration::register_copy,
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
                rev3_storage_error(
                    "generation read requires a registered executor MVCC snapshot");
            }
            if (!classification_index_name.empty()
                && snapshot_registration
                    != Rev3SnapshotRegistration::borrow_registered) {
                rev3_storage_error(
                    "bound generation classification must borrow the executor snapshot");
            }
            if (!bioseq::indexing::sql_bigint_can_represent(query_key_)) {
                rev3_storage_error(
                    "generation read query key exceeds signed SQL range");
            }
            if (!OidIsValid(expected_tablespace_oid_)) {
                rev3_storage_error(
                    "generation read expected tablespace is invalid");
            }
            const bioseq::indexing::MinimizerSchemeIdentity expected_scheme{
                .id = expected_.minimizer_scheme_id,
                .version = expected_.minimizer_scheme_version,
            };
            const auto expected_profile =
                expected_.wire_format
                        == bioseq::indexing::MinimizerGenerationWireFormat::
                            grouped_shape_soa_segment_local
                    ? expected_.max_occ_cap
                        ? bioseq::indexing::
                            minimizer_generation_payload_profile(
                                expected_.wire_format,
                                expected_scheme,
                                expected_.alphabet_code,
                                expected_.kmer_size,
                                /*position_segment_span_bp=*/1u,
                                *expected_.max_occ_cap)
                        : std::expected<MinimizerGenerationPayloadProfile,
                            MinimizerGenerationWireError>{
                                std::unexpected(
                                    MinimizerGenerationWireError::
                                        compatibility_mismatch)}
                    : bioseq::indexing::
                        minimizer_generation_payload_profile(
                            expected_.wire_format,
                            expected_scheme,
                            expected_.kmer_size);
            const bool supported_storage_width =
                expected_.storage_unit_byte_width == 1u
                || expected_.storage_unit_byte_width == 2u
                || expected_.storage_unit_byte_width == 4u
                || expected_.storage_unit_byte_width == 8u;
            if (!bioseq::indexing::
                    is_supported_minimizer_generation_wire_format(
                        expected_.wire_format)
                || !expected_profile
                || expected_profile->wire_format != expected_.wire_format
                || expected_profile->minimizer_scheme != expected_scheme
                || expected_profile->kmer_size != expected_.kmer_size
                || !supported_storage_width
                || expected_.window_size == 0
                || expected_.gpu_abi_version
                    != expected_profile->gpu_abi_version) {
                rev3_storage_error(
                    "generation read expected minimizer compatibility is invalid");
            }
            expected_profile_ = *expected_profile;
            if (snapshot_registration
                    == Rev3SnapshotRegistration::register_copy) {
                snapshot_ = bioseq::pg_utils::invoke_pg_as_cpp(
                    [&] { return RegisterSnapshot(snapshot_); });
                snapshot_registered_by_session_ = true;
                stats_.cold.snapshot_registers = 1;
            }
            const Oid relation_oid = bioseq::pg_utils::invoke_pg_as_cpp([&] {
                return get_relname_relid(
                    table_name_.c_str(), bioseq_internal_ns());
            });
            if (!OidIsValid(relation_oid)
                && !classification_index_name.empty()) {
                throw MinimizerGenerationRelationOpenFailurePg{
                    .inspection = {
                        .kind = MinimizerGenerationRelationKind::missing,
                        .revision3_error = Rev3IndexSchemaError::none,
                    },
                };
            }
            heap_relation_ = OidIsValid(relation_oid)
                ? bioseq::pg_utils::invoke_pg_as_cpp([&] {
                    return try_safe_relation_open(
                        relation_oid, AccessShareLock);
                })
                : nullptr;
            if (heap_relation_ != nullptr) {
                stats_.cold.add(stats_.cold.sidecar_heap_opens, 1);
                stats_.cold.add(stats_.cold.lock_acquires, 1);
            }
            if (heap_relation_ == nullptr && !classification_index_name.empty()) {
                throw MinimizerGenerationRelationOpenFailurePg{
                    .inspection = {
                        .kind = MinimizerGenerationRelationKind::missing,
                        .revision3_error = Rev3IndexSchemaError::none,
                    },
                };
            }
            if (heap_relation_ == nullptr) {
                rev3_storage_error(
                    "revision-3 generation relation is missing or renamed");
            }
            const bool relation_identity_exact =
                RelationGetNamespace(heap_relation_) == bioseq_internal_ns()
                && std::string_view{RelationGetRelationName(heap_relation_)}
                    == table_name_;
            const Oid primary_index_oid = bioseq::pg_utils::invoke_pg_as_cpp(
                [&] {
                    return RelationGetPrimaryKeyIndex(heap_relation_);
                });
            if (OidIsValid(primary_index_oid)) {
                primary_index_relation_ = bioseq::pg_utils::invoke_pg_as_cpp(
                    [&] {
                        return index_open(
                            primary_index_oid, AccessShareLock);
                    });
                stats_.cold.add(stats_.cold.sidecar_index_opens, 1);
                stats_.cold.add(stats_.cold.lock_acquires, 1);
            }
            const Rev3IndexSchemaInspection schema = relation_identity_exact
                ? bioseq::pg_utils::invoke_pg_as_cpp([&] {
                    return inspect_bound_rev3_index_schema(
                        heap_relation_, primary_index_relation_,
                        Rev3IndexSchemaExpectation{
                            .namespace_oid = bioseq_internal_ns(),
                            .relation_name = table_name_,
                            .tablespace_oid = expected_tablespace_oid_,
                        });
                })
                : Rev3IndexSchemaInspection{
                    .error = Rev3IndexSchemaError::not_heap,
                };
            stats_.cold.add(stats_.cold.sidecar_schema_validations, 1);
            if (!schema) {
                if (!classification_index_name.empty()) {
                    const bool legacy_exact = relation_identity_exact
                        && primary_index_relation_ != nullptr
                        && bioseq::pg_utils::invoke_pg_as_cpp([&] {
                            return inspect_bound_exact_legacy_minimizer_index_schema(
                                heap_relation_, primary_index_relation_,
                                classification_index_name);
                        });
                    if (primary_index_relation_ != nullptr) {
                        Relation finished_index = std::exchange(
                            primary_index_relation_, nullptr);
                        bioseq::pg_utils::invoke_pg_as_cpp([&] {
                            index_close(finished_index, AccessShareLock);
                        });
                        stats_.cold.add(stats_.cold.lock_releases, 1);
                    }
                    Relation finished_heap = std::exchange(
                        heap_relation_, nullptr);
                    bioseq::pg_utils::invoke_pg_as_cpp([&] {
                        relation_close(finished_heap, AccessShareLock);
                    });
                    stats_.cold.add(stats_.cold.lock_releases, 1);
                    if (snapshot_registered_by_session_ && snapshot_ != nullptr) {
                        Snapshot finished_snapshot = snapshot_;
                        snapshot_ = nullptr;
                        snapshot_registered_by_session_ = false;
                        bioseq::pg_utils::invoke_pg_as_cpp([&] {
                            UnregisterSnapshot(finished_snapshot);
                        });
                    }
                    throw MinimizerGenerationRelationOpenFailurePg{
                        .inspection = {
                            .kind = legacy_exact
                                ? MinimizerGenerationRelationKind::legacy_exact
                                : MinimizerGenerationRelationKind::malformed,
                            .revision3_error = schema.error,
                        },
                    };
                }
                rev3_storage_error(fmt::format(
                    "generation relation schema mismatch: {}",
                    rev3_index_schema_error_name(schema.error)));
            }
            (void)verify_pg_inline_payload_layout(heap_relation_);
            stats_.relation_opens = 1;
            add_cold_index_elapsed(
                stats_.cold, stats_.cold.bind_ns, bind_begin_ns);
            add_cold_index_buffer_usage(stats_.cold, bind_buffer_begin);
        } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
            state_ = State::failed;
            abandon_relations_noexcept_();
            throw;
        }
    }
    IndexStorageFragmentGenerationReadSessionPg(
        const IndexStorageFragmentGenerationReadSessionPg&) = delete;
    IndexStorageFragmentGenerationReadSessionPg& operator=(
        const IndexStorageFragmentGenerationReadSessionPg&) = delete;
    IndexStorageFragmentGenerationReadSessionPg(
            IndexStorageFragmentGenerationReadSessionPg&& other) noexcept
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
          catalog_source_attested_(other.catalog_source_attested_),
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
          grouped32_cold_route_(other.grouped32_cold_route_),
          stats_(other.stats_) {
        other.full_generation_batch_slots_.fill(nullptr);
        other.state_ = State::moved;
        other.grouped32_cold_route_.reset();
    }
    IndexStorageFragmentGenerationReadSessionPg& operator=(
        IndexStorageFragmentGenerationReadSessionPg&&) = delete;
    ~IndexStorageFragmentGenerationReadSessionPg() {
        abandon_relations_noexcept_();
    }

private:
    [[nodiscard]] MinimizerGenerationHead read_active_head_impl_(
            bool bind_expected_wire_format) {
        MinimizerGenerationHead result{};
        const std::uint64_t head_begin_ns = cold_index_now_ns();
        const BufferUsage head_buffer_begin = pgBufferUsage;
        try {
            if (state_ != State::opened) {
                checked_add_(stats_.metadata_repeated_reads, 1,
                    "repeated metadata counter overflow");
                fail_("active generation head was read more than once or out of order");
            }
            ScanKeyData keys[5];
            ScanKeyInit(&keys[0], kRev3IdxGenerationId,
                BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum(0));
            ScanKeyInit(&keys[1], kRev3IdxQueryKey,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(query_key_)));
            ScanKeyInit(&keys[2], kRev3IdxSubKey,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(
                    GENERATION_HEAD_SUB_KEY)));
            ScanKeyInit(&keys[3], kRev3IdxPositionSegment,
                BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(-1));
            ScanKeyInit(&keys[4], kRev3IdxChunkNo,
                BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(0));
            active_scan_ = bioseq::pg_utils::invoke_pg_as_cpp([&] {
                return systable_beginscan_ordered(
                    heap_relation_, primary_index_relation_, snapshot_, 5,
                    keys);
            });
            const auto next_tuple = [&] {
                return bioseq::pg_utils::invoke_pg_as_cpp([&] {
                    return systable_getnext_ordered(
                        active_scan_, ForwardScanDirection);
                });
            };
            HeapTuple tuple = next_tuple();
            if (!HeapTupleIsValid(tuple)) {
                fail_("active generation head is missing");
            }
            LoadedRow row =
                load_rev3_pg_row_or_error(heap_relation_, tuple);
            std::array<std::byte, MinimizerGenerationHead::encoded_bytes> bytes{};
            if (row.query_key != query_key_
                || row.address != IndexChunkAddress{
                    .generation_id = 0,
                    .position_segment = -1,
                    .sub_key = GENERATION_HEAD_SUB_KEY,
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
            bioseq::pg_utils::invoke_pg_as_cpp(
                [&] { systable_endscan_ordered(active_scan_); });
            active_scan_ = nullptr;
            auto decoded =
                bioseq::indexing::decode_minimizer_generation_head(bytes);
            if (!decoded || !bioseq::indexing::sql_bigint_can_represent(
                    decoded->generation_id)) {
                fail_("active generation head fixed wire is invalid");
            }
            if (bind_expected_wire_format
                && decoded->wire_format != expected_profile_.wire_format) {
                fail_(
                    "active generation head wire format disagrees with reader");
            }
            result = *decoded;
            head_ = result;
            state_ = State::head_read;
            stats_.head_scans = 1;
            stats_.cold.head_scans = 1;
            stats_.head_rows_read = 1;
            stats_.head_bytes_read = MinimizerGenerationHead::encoded_bytes;
            stats_.index_scans = 1;
            stats_.explicit_copy_bytes = MinimizerGenerationHead::encoded_bytes;
            add_cold_index_elapsed(
                stats_.cold, stats_.cold.head_ns, head_begin_ns);
            add_cold_index_buffer_usage(stats_.cold, head_buffer_begin);
        } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
            state_ = State::failed;
            abandon_relations_noexcept_();
            throw;
        }
        return result;
    }

public:
    /// Read and authenticate an active head bound to the caller's independently
    /// selected expected format.
    [[nodiscard]] MinimizerGenerationHead read_active_head_once() {
        return read_active_head_impl_(/*bind_expected_wire_format=*/true);
    }

    /// Production cold readers accept the current Format-5 contract while
    /// retaining explicit read compatibility for already-published Format 4.
    /// Both compatibility objects are independently derived from the selected
    /// backend/GUCs before this call; the authenticated head may select one of
    /// them, but cannot supply or alter any biological compatibility field.
    /// The relation and primary index remain open exactly once.
    [[nodiscard]] MinimizerGenerationHead
    read_active_head_allowing_format4_compatibility_once(
            const ExpectedMinimizerCompatibility& format4_expected) {
        const MinimizerGenerationHead result = read_active_head_impl_(
            /*bind_expected_wire_format=*/false);
        if (result.wire_format == expected_.wire_format) return result;
        const bioseq::indexing::MinimizerSchemeIdentity scheme{
            .id = format4_expected.minimizer_scheme_id,
            .version = format4_expected.minimizer_scheme_version,
        };
        const auto profile =
            bioseq::indexing::minimizer_generation_payload_profile(
                format4_expected.wire_format, scheme,
                format4_expected.kmer_size);
        const bool same_common_contract =
            format4_expected.wire_format
                == bioseq::indexing::MinimizerGenerationWireFormat::
                    grouped_hash32_segment_local32
            && !format4_expected.max_occ_cap
            && format4_expected.alphabet_code == expected_.alphabet_code
            && format4_expected.storage_unit_byte_width
                == expected_.storage_unit_byte_width
            && format4_expected.kmer_size == expected_.kmer_size
            && format4_expected.window_size == expected_.window_size
            && format4_expected.minimizer_scheme_id
                == expected_.minimizer_scheme_id
            && format4_expected.minimizer_scheme_version
                == expected_.minimizer_scheme_version
            && format4_expected.producer_backend_id
                == expected_.producer_backend_id
            && format4_expected.producer_backend_identity_version
                == expected_.producer_backend_identity_version;
        if (result.wire_format != format4_expected.wire_format
            || !same_common_contract || !profile
            || profile->gpu_abi_version
                != format4_expected.gpu_abi_version) {
            fail_(
                "active generation head wire format disagrees with every permitted reader contract");
        }
        expected_ = format4_expected;
        expected_profile_ = *profile;
        return result;
    }

    /// Administrative wire-format discovery. This authenticates the head,
    /// defers only its expected-format comparison, and closes the head-only
    /// session before returning. The caller must reopen a normally bound
    /// session before reading manifest or payload bytes.
    [[nodiscard]] MinimizerGenerationHead
    discover_active_head_wire_format_and_close() {
        MinimizerGenerationHead result = read_active_head_impl_(
            /*bind_expected_wire_format=*/false);
        close_head_read();
        return result;
    }

    /// Timed compact-direct metadata path. Read the exact manifest tuple under
    /// the same snapshot as the head, using chunk_no=0 in the primary-index
    /// qualification so no directory tuple can be visited. The fixed manifest
    /// and its head checksum remain fully authenticated; only the O(directory)
    /// materialization/CRC/DirView construction is intentionally omitted.
    [[nodiscard]] const MinimizerGenerationManifest&
    read_manifest_once_without_directory(
            const MinimizerGenerationHead& expected_head) {
        const std::uint64_t manifest_begin_ns = cold_index_now_ns();
        const BufferUsage manifest_buffer_begin = pgBufferUsage;
        try {
            if (state_ != State::head_read || !head_
                || expected_head != *head_ || metadata_ || manifest_only_
                || expected_head.wire_format
                    != bioseq::indexing::MinimizerGenerationWireFormat::
                        grouped_shape_soa_segment_local) {
                checked_add_(stats_.metadata_repeated_reads, 1,
                    "repeated metadata counter overflow");
                fail_(
                    "manifest-only read repeated, early, changed, or not Format-5");
            }

            ScanKeyData keys[5];
            ScanKeyInit(&keys[0], kRev3IdxGenerationId,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(
                    expected_head.generation_id)));
            ScanKeyInit(&keys[1], kRev3IdxQueryKey,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(query_key_)));
            ScanKeyInit(&keys[2], kRev3IdxSubKey,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(
                    DIRECTORY_SUB_KEY)));
            ScanKeyInit(&keys[3], kRev3IdxPositionSegment,
                BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(-1));
            ScanKeyInit(&keys[4], kRev3IdxChunkNo,
                BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(0));
            active_scan_ = bioseq::pg_utils::invoke_pg_as_cpp([&] {
                return systable_beginscan_ordered(
                    heap_relation_, primary_index_relation_, snapshot_, 5,
                    keys);
            });
            const auto next_tuple = [this] {
                return bioseq::pg_utils::invoke_pg_as_cpp([this] {
                    return systable_getnext_ordered(
                        active_scan_, ForwardScanDirection);
                });
            };
            HeapTuple tuple = next_tuple();
            if (!HeapTupleIsValid(tuple)) {
                fail_("READY generation manifest is missing");
            }
            LoadedRow manifest_row =
                load_rev3_pg_row_or_error(heap_relation_, tuple);
            std::array<std::byte,
                MinimizerGenerationManifest::format5_encoded_bytes>
                manifest_bytes{};
            constexpr std::size_t expected_manifest_bytes =
                MinimizerGenerationManifest::format5_encoded_bytes;
            if (manifest_row.query_key != query_key_
                || manifest_row.address != IndexChunkAddress{
                    .generation_id = expected_head.generation_id,
                    .position_segment = -1,
                    .sub_key = DIRECTORY_SUB_KEY,
                    .chunk_no = 0,
                }
                || manifest_row.bytes.size() != expected_manifest_bytes) {
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
            bioseq::pg_utils::invoke_pg_as_cpp(
                [&] { systable_endscan_ordered(active_scan_); });
            active_scan_ = nullptr;

            const std::span<const std::byte> encoded_manifest{
                manifest_bytes};
            auto manifest =
                bioseq::indexing::decode_minimizer_generation_manifest(
                    encoded_manifest);
            if (!manifest
                || manifest->wire_format != expected_head.wire_format
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
                    minimizer_generation_compatibility_checksum(*manifest)
                    != expected_head.compatibility_checksum
                || !bioseq::indexing::
                    validate_minimizer_generation_compatibility(
                        *manifest, expected_)) {
                fail_("READY generation manifest/head binding disagrees");
            }
            const auto manifest_profile =
                bioseq::indexing::minimizer_generation_payload_profile(
                    *manifest);
            if (!manifest_profile
                || manifest_profile->wire_format != expected_.wire_format
                || !manifest_profile->shape_v5) {
                fail_(
                    "READY generation manifest payload profile is invalid");
            }
            const std::uint32_t extent_payload_bytes =
                bioseq::indexing::minimizer_extent_payload_bytes(
                    manifest->wire_format);
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
            state_ = State::metadata_unattested;
            stats_.manifest_directory_scans = 1u;
            stats_.cold.directory_scans = 1u;
            stats_.manifest_rows_read = 1u;
            stats_.manifest_bytes_read = expected_manifest_bytes;
            stats_.directory_chunks_read = 0u;
            stats_.directory_bytes_read = 0u;
            stats_.metadata_heap_rows_read = 2u;
            stats_.metadata_bytes_read =
                MinimizerGenerationHead::encoded_bytes
                + expected_manifest_bytes;
            checked_add_(stats_.index_scans, 1u,
                "metadata index scan count overflow");
            checked_add_(stats_.explicit_copy_bytes,
                expected_manifest_bytes,
                "metadata copy byte count overflow");
            add_cold_index_elapsed(
                stats_.cold, stats_.cold.directory_ns,
                manifest_begin_ns);
            add_cold_index_buffer_usage(
                stats_.cold, manifest_buffer_begin);
        } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
            state_ = State::failed;
            abandon_relations_noexcept_();
            throw;
        }
        return *manifest_only_;
    }

    [[nodiscard]] const MinimizerGenerationReadMetadata&
    read_manifest_directory_once(const MinimizerGenerationHead& expected_head) {
        const std::uint64_t directory_begin_ns = cold_index_now_ns();
        const BufferUsage directory_buffer_begin = pgBufferUsage;
        try {
        if (state_ != State::head_read || !head_
            || expected_head != *head_ || metadata_ || manifest_only_) {
            checked_add_(stats_.metadata_repeated_reads, 1,
                "repeated metadata counter overflow");
            fail_("manifest/directory read repeated, early, or changed head identity");
        }
        ScanKeyData keys[4];
        ScanKeyInit(&keys[0], kRev3IdxGenerationId,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(
                expected_head.generation_id)));
        ScanKeyInit(&keys[1], kRev3IdxQueryKey,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(query_key_)));
        ScanKeyInit(&keys[2], kRev3IdxSubKey,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(DIRECTORY_SUB_KEY)));
        ScanKeyInit(&keys[3], kRev3IdxPositionSegment,
            BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(-1));
        active_scan_ = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            return systable_beginscan_ordered(
                heap_relation_, primary_index_relation_, snapshot_, 4,
                keys);
        });
        const auto next_tuple = [&] {
            return bioseq::pg_utils::invoke_pg_as_cpp([&] {
                return systable_getnext_ordered(
                    active_scan_, ForwardScanDirection);
            });
        };
        HeapTuple tuple = next_tuple();
        if (!HeapTupleIsValid(tuple)) {
            fail_("READY generation manifest is missing");
        }
        LoadedRow manifest_row =
            load_rev3_pg_row_or_error(heap_relation_, tuple);
        std::array<std::byte,
            MinimizerGenerationManifest::format5_encoded_bytes>
            manifest_bytes{};
        const std::size_t expected_manifest_bytes =
            bioseq::indexing::minimizer_manifest_encoded_bytes(
                expected_head.wire_format);
        if (manifest_row.query_key != query_key_
            || manifest_row.address != IndexChunkAddress{
                .generation_id = expected_head.generation_id,
                .position_segment = -1,
                .sub_key = DIRECTORY_SUB_KEY,
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
        if (!bioseq::indexing::validate_minimizer_generation_compatibility(
                *manifest, expected_)) {
            fail_("READY generation minimizer compatibility disagrees");
        }
        const auto manifest_profile =
            bioseq::indexing::minimizer_generation_payload_profile(
                *manifest);
        if (!manifest_profile
            || manifest_profile->wire_format != expected_.wire_format) {
            fail_("READY generation manifest payload profile is invalid");
        }
        expected_profile_ = *manifest_profile;
        const std::uint32_t extent_payload_bytes =
            bioseq::indexing::minimizer_extent_payload_bytes(
                manifest->wire_format);
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
                    / sizeof(MinimizerBucketDirEntry)) {
            fail_("READY metadata admission size overflow");
        }
        const std::uint64_t entry_storage =
            entry_count * sizeof(MinimizerBucketDirEntry);
        const std::uint64_t directory_owned_logical =
            bioseq::parallelism::saturating_add(
                manifest->directory_bytes, entry_storage);
        const std::uint64_t scratch_headroom =
            bioseq::parallelism::saturating_mul(
                manifest->directory_bytes,
                bioseq::parallelism::kVectorPreallocationHeadroomFactor);
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
            bioseq::parallelism::MemoryAdmissionController::instance()
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
            LoadedRow directory_row =
                load_rev3_pg_row_or_error(heap_relation_, tuple);
            const std::uint64_t offset =
                static_cast<std::uint64_t>(chunk_no - 1u)
                * extent_payload_bytes;
            const std::uint64_t expected_bytes = std::min<std::uint64_t>(
                extent_payload_bytes,
                manifest->directory_bytes - offset);
            if (directory_row.query_key != query_key_
                || directory_row.address != IndexChunkAddress{
                    .generation_id = expected_head.generation_id,
                    .position_segment = -1,
                    .sub_key = DIRECTORY_SUB_KEY,
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
        bioseq::pg_utils::invoke_pg_as_cpp(
            [&] { systable_endscan_ordered(active_scan_); });
        active_scan_ = nullptr;
        if (directory_bytes_scratch_.size() != manifest->directory_bytes
            || bioseq::indexing::crc64_ecma182(
                std::span<const std::byte>{directory_bytes_scratch_})
                != manifest->directory_checksum) {
            fail_("READY generation directory aggregate checksum disagrees");
        }
        auto directory = DirView::decode_admitted(
            directory_bytes_scratch_, expected_profile_, metadata_permit_);
        if (!directory) {
            fail_("READY generation directory fixed wire is invalid");
        }
        metadata_.emplace(MinimizerGenerationReadMetadata{
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
        for (const MinimizerBucketDirEntry& entry
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
            kOrderedRangeScanPenaltyRows);
        metadata_permit_.shrink_to(
            metadata_->directory.owned_capacity_bytes());
        state_ = State::metadata_unattested;
        stats_.manifest_directory_scans = 1;
        stats_.cold.directory_scans = 1;
        stats_.manifest_rows_read = 1;
        stats_.manifest_bytes_read = expected_manifest_bytes;
        stats_.directory_chunks_read = manifest->directory_chunk_count;
        stats_.directory_bytes_read = manifest->directory_bytes;
        stats_.metadata_heap_rows_read = 2u
            + manifest->directory_chunk_count;
        stats_.metadata_bytes_read = MinimizerGenerationHead::encoded_bytes
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
        } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
            state_ = State::failed;
            abandon_relations_noexcept_();
            throw;
        }
        return *metadata_;
    }

    /// Bind metadata to an independently reopened source row. This is the only
    /// transition to metadata_ready: source identity alone is insufficient.
    void validate_full_source_attestation(
            Rev3AuthoritativeSourceProof&& proof) {
        try {
            const MinimizerGenerationManifest* const manifest =
                active_manifest_();
            if (state_ != State::metadata_unattested
                || !head_ || manifest == nullptr) {
                proof.invalidate_();
                fail_("READY source proof was consumed out of order");
            }
            if (!proof.valid_
                || proof.snapshot_ != snapshot_
                || proof.database_oid_ != MyDatabaseId
                || proof.source_.sequence_rowno != query_key_
                || proof.table_name_ != table_name_) {
                proof.invalidate_();
                fail_("READY source proof relation/query/snapshot binding disagrees");
            }
            const std::uint64_t compatibility_checksum =
                bioseq::indexing::minimizer_generation_compatibility_checksum(
                    *manifest);
            proof.source_.compatibility_checksum = compatibility_checksum;
            const std::uint64_t source_identity = rev3_source_identity(
                proof.main_relfilenode_, proof.source_);
            const std::uint64_t source_content_checksum =
                proof.source_content_checksum_;
            if (source_identity == 0
                || manifest->alphabet_code
                    != proof.source_.alphabet_code
                || manifest->storage_unit_byte_width
                    != proof.source_.storage_unit_byte_width
                || manifest->target_length
                    != proof.source_.locator.total_bp_size
                || head_->source_identity != source_identity
                || manifest->source_identity != source_identity
                || head_->source_content_checksum
                    != source_content_checksum
                || manifest->source_content_checksum
                    != source_content_checksum
                || head_->compatibility_checksum
                    != compatibility_checksum
                || bioseq::indexing::minimizer_generation_compatibility_checksum(
                    *manifest) != compatibility_checksum) {
                proof.invalidate_();
                fail_("READY metadata/source attestation disagrees");
            }
            proof.invalidate_();
            catalog_source_attested_ = false;
            state_ = State::metadata_ready;
        } catch (...) {
            proof.invalidate_();
            state_ = State::failed;
            throw;
        }
    }

    /// Timed cold-reader transition. Only the constant-work catalog proof is
    /// accepted here; the full decoded proof has a separate, explicitly named
    /// untimed writer/admin transition above.
    void validate_source_attestation(Rev3CatalogSourceProof&& proof) {
        try {
            const MinimizerGenerationManifest* const manifest =
                active_manifest_();
            if (state_ != State::metadata_unattested
                || !head_ || manifest == nullptr) {
                proof.invalidate_();
                fail_("READY catalog source proof was consumed out of order");
            }
            if (!proof.valid_
                || !proof.counters_.timed_equation_holds()
                || proof.snapshot_ != snapshot_
                || proof.database_oid_ != MyDatabaseId
                || proof.source_.sequence_rowno != query_key_
                || proof.table_name_ != table_name_) {
                proof.invalidate_();
                fail_("READY catalog source proof binding/work equation disagrees");
            }
            const std::uint64_t compatibility_checksum =
                bioseq::indexing::minimizer_generation_compatibility_checksum(
                    *manifest);
            proof.source_.compatibility_checksum = compatibility_checksum;
            const std::uint64_t source_identity = rev3_source_identity(
                proof.main_relfilenode_, proof.source_);
            const std::uint64_t source_content_checksum =
                proof.source_content_checksum_;
            if (source_identity == 0
                || manifest->alphabet_code
                    != proof.source_.alphabet_code
                || manifest->storage_unit_byte_width
                    != proof.source_.storage_unit_byte_width
                || manifest->target_length
                    != proof.source_.locator.total_bp_size
                || head_->source_identity != source_identity
                || manifest->source_identity != source_identity
                || head_->source_content_checksum
                    != source_content_checksum
                || manifest->source_content_checksum
                    != source_content_checksum
                || head_->compatibility_checksum
                    != compatibility_checksum) {
                proof.invalidate_();
                fail_("READY metadata/catalog source attestation disagrees");
            }
            stats_.source_parent_rows_read = proof.counters_.parent_rows_read;
            stats_.source_catalog_rows_read = proof.counters_.catalog_rows_read;
            stats_.source_catalog_index_scans =
                proof.counters_.catalog_index_scans;
            stats_.source_fragment_rows_read =
                proof.counters_.fragment_rows_read;
            stats_.source_decoded_bp = proof.counters_.decoded_bp;
            stats_.source_storage_incarnations_checked =
                proof.counters_.storage_incarnations_checked;
            stats_.cold.merge(proof.cold_stats_);
            proof.invalidate_();
            catalog_source_attested_ = true;
            state_ = State::metadata_ready;
        } catch (...) {
            proof.invalidate_();
            state_ = State::failed;
            throw;
        }
    }

    /// Bind the exact LIVE image descriptor under the same registered MVCC
    /// snapshot that supplied the head and manifest.  Both catalog locks stay
    /// owned by this session until the mapped image has been registered and
    /// the selected mapped Format-5 finish transition performs the checked
    /// close.
    [[nodiscard]] MinimizerGenerationStorageDescriptorPg
    read_live_storage_descriptor_once(
            const Rev3IndexAdministrationIdentity& identity) {
        try {
            const MinimizerGenerationManifest* const manifest =
                active_manifest_();
            if (state_ != State::metadata_ready
                || !catalog_source_attested_ || !head_
                || manifest == nullptr
                || manifest->wire_format
                    != bioseq::indexing::MinimizerGenerationWireFormat::
                        grouped_shape_soa_segment_local
                || storage_descriptor_relation_ != nullptr
                || storage_descriptor_live_index_relation_ != nullptr
                || identity.table_name != table_name_
                || identity.database_oid != MyDatabaseId
                || identity.tablespace_oid != expected_tablespace_oid_
                || identity.index_name
                    != MINIMIZERS_PER_FRAGMENT_INDEX_NAME) {
                fail_(
                    "LIVE generation descriptor was requested out of order");
            }
            const Oid descriptor_oid =
                bioseq::pg_utils::invoke_pg_as_cpp([&] {
                    return get_relname_relid(
                        "minimizer_generation_storage",
                        bioseq_internal_ns());
                });
            const Oid live_index_oid =
                bioseq::pg_utils::invoke_pg_as_cpp([&] {
                    return get_relname_relid(
                        "minimizer_generation_storage_one_live",
                        bioseq_internal_ns());
                });
            if (!OidIsValid(descriptor_oid) || !OidIsValid(live_index_oid)) {
                fail_("generation storage descriptor authority is missing");
            }
            storage_descriptor_relation_ =
                bioseq::pg_utils::invoke_pg_as_cpp([&] {
                    return try_safe_relation_open(
                        descriptor_oid, AccessShareLock);
                });
            if (storage_descriptor_relation_ == nullptr) {
                fail_("generation storage descriptor relation disappeared");
            }
            stats_.cold.add(stats_.cold.lock_acquires, 1);
            storage_descriptor_live_index_relation_ =
                bioseq::pg_utils::invoke_pg_as_cpp([&] {
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
            if (descriptor.provider
                    == bioseq::indexing::Mpf5V2Provider::postgres_relation
                && (descriptor.payload_relation_oid
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
                        != std::optional<std::string>{table_name_})) {
                fail_("LIVE relation-provider descriptor locator disagrees");
            }
            return descriptor;
        } catch (...) {
            cleanup_failed_session_and_rethrow_();
        }
    }

    /// Bind the single pre-I/O Format-4 route decision to this session.  The
    /// storage executor may validate and execute that authority, but it must
    /// never recompute the route or replace its candidate upper bounds with
    /// the smaller post-max-occ request set.
    void bind_grouped32_cold_route_once(
            const MinimizerGrouped32ColdRoutePlan& plan) {
        try {
            if (state_ != State::metadata_ready || !metadata_
                || !catalog_source_attested_ || grouped32_cold_route_) {
                fail_("grouped32 cold route was bound early, repeatedly, or without source attestation");
            }
            if (metadata_->manifest.wire_format
                    != bioseq::indexing::MinimizerGenerationWireFormat::
                        grouped_hash32_segment_local32
                || stats_.cold.route_empty != 0
                || stats_.cold.route_selective != 0
                || stats_.cold.route_full != 0
                || stats_.cold.route_candidate_rows != 0
                || stats_.cold.route_candidate_bytes != 0
                || stats_.cold.route_selective_cost != 0
                || plan.full_directory_entries
                    != metadata_->directory.entries().size()
                || plan.full_payload_bytes
                    != metadata_->manifest.payload_bytes) {
                fail_("grouped32 cold route metadata identity disagrees");
            }
            std::uint64_t full_rows = 0;
            for (const MinimizerBucketDirEntry& entry
                 : metadata_->directory.entries()) {
                checked_add_(full_rows, entry.control_chunk_count,
                    "grouped32 cold route full-row equation overflow");
                checked_add_(full_rows, entry.position_chunk_count,
                    "grouped32 cold route full-row equation overflow");
            }
            const auto sql_value = [](std::uint64_t value) noexcept {
                return std::min(
                    value, ColdIndexPreparationStatsPg::sql_bigint_max);
            };
            if (plan.full_payload_rows != full_rows
                || stats_.cold.route_full_rows != sql_value(full_rows)
                || stats_.cold.route_full_bytes
                    != sql_value(plan.full_payload_bytes)
                || stats_.cold.route_full_cost
                    != sql_value(plan.full_cost)
                || plan.control_groups
                    > plan.candidate_directory_entries
                || plan.position_groups_upper_bound
                    > plan.candidate_directory_entries) {
                fail_("grouped32 cold route scalar proof disagrees");
            }

            const bool authority_empty =
                plan.candidate_directory_ordinals.empty()
                && plan.control_requests.empty()
                && plan.position_upper_bound_requests.empty();
            if (plan.route == MinimizerGrouped32ColdLoadRoute::selective) {
                if (plan.candidate_directory_entries == 0
                    || plan.candidate_directory_entries
                        != plan.candidate_directory_ordinals.size()
                    || plan.control_requests.size()
                        != plan.candidate_directory_ordinals.size()
                    || plan.position_upper_bound_requests.size()
                        != plan.candidate_directory_ordinals.size()) {
                    fail_("grouped32 selective route authority shape disagrees");
                }
                std::optional<std::uint32_t> previous_ordinal;
                for (std::size_t index = 0;
                     index < plan.candidate_directory_ordinals.size();
                     ++index) {
                    const std::uint32_t ordinal =
                        plan.candidate_directory_ordinals[index];
                    const MinimizerBucketDirEntry* entry =
                        metadata_->directory.entry_by_ordinal(ordinal);
                    const IndexSubKeyRequest& control =
                        plan.control_requests[index];
                    const IndexSubKeyRequest& position =
                        plan.position_upper_bound_requests[index];
                    if ((previous_ordinal && *previous_ordinal >= ordinal)
                        || entry == nullptr
                        || control.generation_id
                            != metadata_->manifest.generation_id
                        || position.generation_id
                            != metadata_->manifest.generation_id
                        || control.position_segment
                            != entry->position_segment
                        || position.position_segment
                            != entry->position_segment
                        || !request_matches_extent_(control, *entry, false)
                        || !request_matches_extent_(position, *entry, true)) {
                        fail_("grouped32 selective route authority does not equal the directory");
                    }
                    previous_ordinal = ordinal;
                }
            } else if (!authority_empty) {
                fail_("grouped32 empty/full route unexpectedly owns selective authority");
            }
            if ((plan.route == MinimizerGrouped32ColdLoadRoute::empty
                    && plan.candidate_directory_entries != 0)
                || (plan.route != MinimizerGrouped32ColdLoadRoute::empty
                    && plan.candidate_directory_entries == 0)) {
                fail_("grouped32 cold route candidate/route equation disagrees");
            }

            grouped32_cold_route_ = BoundGrouped32ColdRoute{
                .route = plan.route,
                .control_groups = plan.control_groups,
                .position_groups_upper_bound =
                    plan.position_groups_upper_bound,
            };
            stats_.cold.route_empty =
                plan.route == MinimizerGrouped32ColdLoadRoute::empty ? 1u : 0u;
            stats_.cold.route_selective =
                plan.route == MinimizerGrouped32ColdLoadRoute::selective
                ? 1u : 0u;
            stats_.cold.route_full =
                plan.route == MinimizerGrouped32ColdLoadRoute::full ? 1u : 0u;
            stats_.cold.add(
                stats_.cold.route_candidate_rows,
                plan.candidate_rows_upper_bound);
            stats_.cold.add(
                stats_.cold.route_candidate_bytes,
                plan.candidate_bytes_upper_bound);
            stats_.cold.add(
                stats_.cold.route_selective_cost,
                plan.selective_cost);
            stats_.cold.stats_saturated = stats_.cold.stats_saturated
                || plan.counters_saturated
                || plan.selective_cost_saturated
                || plan.full_cost_saturated;
        } catch (...) {
            state_ = State::failed;
            throw;
        }
    }

private:
    [[nodiscard]] MinimizerGenerationReadStats
    finish_full_generation_() {
        if (state_ != State::full_generation_payload_complete
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
                != MinimizerGenerationHead::encoded_bytes
            || stats_.manifest_directory_scans != 1
            || stats_.manifest_rows_read != 1
            || stats_.manifest_bytes_read
                != bioseq::indexing::minimizer_manifest_encoded_bytes(
                    metadata_->manifest.wire_format)
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
            || !catalog_source_attested_
            || stats_.source_parent_rows_read != 1
            || stats_.source_catalog_rows_read != 1
            || stats_.source_catalog_index_scans != 1
            || stats_.source_fragment_rows_read != 0
            || stats_.source_decoded_bp != 0
            || stats_.source_storage_incarnations_checked < 1
            || stats_.source_storage_incarnations_checked > 2) {
            fail_("full-generation read proof equations disagree");
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
        state_ = State::finished;
        close_relations_checked_();
        return stats_;
    }

    void plan_phase_groups_(
            PayloadPhase phase,
            std::span<const MinimizerGenerationPayloadReadGroup> groups) {
        try {
            const bool position_phase = phase == PayloadPhase::positions;
            const State required_state = position_phase
                ? State::controls_complete : State::metadata_ready;
            if (state_ != required_state || !metadata_) {
                fail_(position_phase
                    ? "position payload requests were planned before control payload completed or more than once"
                    : "control payload requests were planned before metadata or more than once");
            }
            if (!planned_groups_.empty() || next_group_ != 0 || plan_permit_
                || !runtime_requests_.empty() || runtime_permit_
                || active_scan_ != nullptr) {
                fail_("payload phase planning found leaked prior-phase state");
            }
            if (!position_phase
                && (!authorized_position_ordinals_.empty()
                    || control_authorization_permit_)) {
                fail_("control payload authorization state was already populated");
            }
            if (grouped32_cold_route_) {
                const BoundGrouped32ColdRoute& route =
                    *grouped32_cold_route_;
                if (route.route == MinimizerGrouped32ColdLoadRoute::full
                    || (route.route
                            == MinimizerGrouped32ColdLoadRoute::empty
                        && !groups.empty())
                    || (!position_phase
                        && route.route
                            == MinimizerGrouped32ColdLoadRoute::selective
                        && groups.size() != route.control_groups)
                    || (position_phase
                        && route.route
                            == MinimizerGrouped32ColdLoadRoute::selective
                        && groups.size()
                            > route.position_groups_upper_bound)) {
                    fail_("payload phase disagrees with the bound grouped32 cold route");
                }
            }
            std::uint64_t request_count = 0;
            std::uint64_t requested_rows = 0;
            std::uint64_t requested_bytes = 0;
            for (const MinimizerGenerationPayloadReadGroup& group : groups) {
                checked_add_(request_count, group.requests.size(),
                    "payload plan request count overflow");
                for (const IndexSubKeyRequest& request : group.requests) {
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
                        / sizeof(PlannedGroup)
                || request_count
                    > (std::numeric_limits<std::uint64_t>::max)()
                        / sizeof(IndexSubKeyRequest)
                || (!position_phase
                    && request_count
                        > (std::numeric_limits<std::uint64_t>::max)()
                            / sizeof(std::uint32_t))) {
                fail_("payload plan admission size overflow");
            }
            plan_bytes = groups.size() * sizeof(PlannedGroup);
            checked_add_(plan_bytes,
                request_count * sizeof(IndexSubKeyRequest),
                "payload plan admission size overflow");
            if (!position_phase && request_count != 0) {
                const std::uint64_t authorization_bytes =
                    request_count * sizeof(std::uint32_t);
                auto authorization_permit =
                    bioseq::parallelism::MemoryAdmissionController::instance()
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
                    bioseq::parallelism::MemoryAdmissionController::instance()
                        .try_reserve(plan_bytes);
                if (!plan_permit) {
                    fail_("payload plan memory admission was denied");
                }
                plan_permit_ = std::move(*plan_permit);
            }
            planned_groups_.clear();
            planned_groups_.reserve(groups.size());
            std::optional<IndexSubKeyRange> previous_range;
            const auto entries = metadata_->directory.entries();
            const std::uint32_t extent_payload_bytes =
                bioseq::indexing::minimizer_extent_payload_bytes(
                    metadata_->manifest.wire_format);
            if (extent_payload_bytes == 0) {
                fail_("full-generation wire quantum is invalid");
            }
            std::uint64_t authorization_checks = 0;
            for (const MinimizerGenerationPayloadReadGroup& input : groups) {
                const IndexSubKeyRange& range = input.range;
                const auto projected_range = project_extent_range_(range);
                if (range.generation_id != metadata_->manifest.generation_id
                    || range.position_segment < 0
                    || !projected_range
                    || projected_range->position_extent != position_phase
                    || input.requests.empty()) {
                    fail_(position_phase
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
                using DirectoryKey =
                    std::pair<std::int32_t, index_sub_key_t>;
                const auto entry_less_than_key = [](
                        const MinimizerBucketDirEntry& entry,
                        const DirectoryKey& key) noexcept {
                    return DirectoryKey{
                        entry.position_segment, entry.sub_key} < key;
                };
                const auto first_entry = std::lower_bound(
                    entries.begin(), entries.end(),
                    DirectoryKey{range.position_segment,
                        projected_range->logical_begin},
                    entry_less_than_key);
                const auto end_entry = std::lower_bound(
                    first_entry, entries.end(),
                    DirectoryKey{range.position_segment,
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
                for (const IndexSubKeyRequest& request : input.requests) {
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
                    if (!position_phase) {
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
                planned_groups_.push_back(PlannedGroup{
                    .range = range,
                    .requests = input.requests,
                    .first_directory_ordinal = first_ordinal,
                    .end_directory_ordinal = end_ordinal,
                    .skip_budget_rows = declared_chunks - wanted_chunks,
                    .position_extent = projected_range->position_extent,
                });
            }
            auto& phase_stats = position_phase
                ? stats_.positions : stats_.controls;
            checked_add_(phase_stats.plan_calls, 1,
                "payload phase plan-call counter overflow");
            checked_add_(phase_stats.planned_request_groups,
                planned_groups_.size(),
                "payload phase planned-group counter overflow");
            checked_add_(phase_stats.control_authorization_checks,
                authorization_checks,
                "position control-authorization counter overflow");
            checked_add_(stats_.bulk_read_calls, 1,
                "payload plan-call counter overflow");
            checked_add_(stats_.planned_request_groups,
                planned_groups_.size(),
                "payload planned-group counter overflow");
            if (!grouped32_cold_route_) {
                if (!position_phase) {
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
            }
            stats_.cold.add(
                stats_.cold.payload_rows_requested, requested_rows);
            stats_.cold.add(
                stats_.cold.payload_bytes_requested, requested_bytes);
            if (!grouped32_cold_route_) {
                stats_.cold.add(
                    stats_.cold.route_selective_cost, requested_rows);
                const std::uint64_t group_penalty =
                    planned_groups_.size()
                        > ColdIndexPreparationStatsPg::sql_bigint_max
                            / kOrderedRangeScanPenaltyRows
                    ? ColdIndexPreparationStatsPg::sql_bigint_max
                    : static_cast<std::uint64_t>(planned_groups_.size())
                        * kOrderedRangeScanPenaltyRows;
                stats_.cold.add(
                    stats_.cold.route_selective_cost, group_penalty);
            }
            next_group_ = 0;
            if (position_phase) {
                release_control_authorization_();
                state_ = planned_groups_.empty()
                    ? State::payload_complete : State::positions_planned;
            } else {
                state_ = planned_groups_.empty()
                    ? State::controls_complete : State::controls_planned;
            }
        } catch (...) {
            state_ = State::failed;
            throw;
        }
    }

public:
    /// Stream the complete attested generation through one ordered physical
    /// scan.  Payload bytes remain opaque and callback-scoped: this layer does
    /// not decode controls, globalize/rebase local positions, or retain a
    /// payload image.  A callback may copy into already-admitted host or mapped
    /// device storage, but command submission/fence waits must occur only after
    /// this method returns and `active_scan_` has been closed.
    template <class ControlSink, class PositionSink>
    requires (
        bioseq::indexing::index_chunk_reader_fn<ControlSink>
        || std::invocable<ControlSink&,
            const ValidatedMinimizerGenerationRowReceipt&>)
          && (
        bioseq::indexing::index_chunk_reader_fn<PositionSink>
        || std::invocable<PositionSink&,
            const ValidatedMinimizerGenerationRowReceipt&>)
    [[nodiscard]] IndexChunkReadStats read_full_generation_payload_once(
            ControlSink&& control_sink,
            PositionSink&& position_sink) {
        IndexChunkReadStats local{};
        const BufferUsage payload_buffer_begin = pgBufferUsage;
        start_payload_timing_();
        try {
            if (!rev3_full_generation_crc_detail::concatenation_self_test()) {
                fail_("full-generation CRC concatenation self-test failed");
            }
            if (state_ != State::metadata_ready || !metadata_
                || !catalog_source_attested_
                || (grouped32_cold_route_
                    && grouped32_cold_route_->route
                        != MinimizerGrouped32ColdLoadRoute::full)
                || active_scan_ != nullptr
                || full_generation_batch_slot_count_ != 0
                || !planned_groups_.empty()
                || next_group_ != 0 || !runtime_requests_.empty()
                || plan_permit_ || runtime_permit_
                || control_authorization_permit_
                || !authorized_position_ordinals_.empty()) {
                checked_add_(stats_.payload.repeated_chunk_reads, 1,
                    "full-generation repeated-read counter overflow");
                fail_("full-generation payload scan was repeated, early, lacked catalog attestation, or found selected-range state");
            }
            const auto entries = metadata_->directory.entries();
            const bool format5 = metadata_->manifest.wire_format
                == bioseq::indexing::MinimizerGenerationWireFormat::
                    grouped_shape_soa_segment_local;
            const std::uint32_t extent_payload_bytes =
                bioseq::indexing::minimizer_extent_payload_bytes(
                    metadata_->manifest.wire_format);
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

            // The S1 primary key yields every global control sub-key followed
            // by every derived position sub-key. Format 5 deliberately hashes
            // that same physical order. Legacy manifests instead hash
            // control+position per directory entry, so retain one CRC state
            // per legacy control extent and concatenate it when the matching
            // position extent begins. No payload bytes are retained.
            std::uint64_t total_chunks = 0;
            std::uint64_t planned_control_chunks = 0;
            std::uint64_t planned_position_chunks = 0;
            std::uint64_t planned_control_bytes = 0;
            std::uint64_t planned_position_bytes = 0;
            for (const MinimizerBucketDirEntry& entry : entries) {
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
            if (entries.size()
                    > (std::numeric_limits<std::uint64_t>::max)()
                        / sizeof(std::uint64_t)) {
                fail_("full-generation checksum plan size overflow");
            }
            const std::uint64_t checksum_plan_bytes =
                static_cast<std::uint64_t>(entries.size())
                * sizeof(std::uint64_t);
            if (checksum_plan_bytes
                    > (std::numeric_limits<std::uint64_t>::max)()
                        / bioseq::parallelism::
                            kVectorPreallocationHeadroomFactor) {
                fail_("full-generation checksum plan headroom overflow");
            }
            const std::uint64_t checksum_plan_headroom =
                checksum_plan_bytes
                * bioseq::parallelism::kVectorPreallocationHeadroomFactor;
            auto reserved_checksum_plan =
                bioseq::parallelism::MemoryAdmissionController::instance()
                    .try_reserve(checksum_plan_headroom);
            if (!reserved_checksum_plan) {
                fail_("full-generation checksum plan memory admission was denied");
            }
            auto checksum_plan_permit =
                std::move(*reserved_checksum_plan);
            std::vector<std::uint64_t> control_extent_row_checksums;
            control_extent_row_checksums.resize(entries.size());
            const std::uint64_t actual_checksum_bytes =
                bioseq::parallelism::saturating_mul(
                    control_extent_row_checksums.capacity(),
                    sizeof(std::uint64_t));
            if (actual_checksum_bytes
                    == (std::numeric_limits<std::uint64_t>::max)()
                || actual_checksum_bytes > checksum_plan_headroom) {
                fail_("full-generation checksum plan capacity exceeded admitted headroom");
            }
            checksum_plan_permit.shrink_to(actual_checksum_bytes);

            local.index_scans = 1;
            local.requested_ranges = 1;
            local.wanted_subkeys = planned_extents;
            local.wanted_extents = planned_extents;
            local.wanted_chunks = total_chunks;
            if (!grouped32_cold_route_) {
                stats_.cold.route_full = 1;
            }
            stats_.cold.add(
                stats_.cold.payload_rows_requested, total_chunks);
            stats_.cold.add(
                stats_.cold.payload_bytes_requested,
                metadata_->manifest.payload_bytes);

            ScanKeyData keys[3];
            ScanKeyInit(&keys[0], kRev3IdxGenerationId,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(
                    metadata_->manifest.generation_id)));
            ScanKeyInit(&keys[1], kRev3IdxQueryKey,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(query_key_)));
            ScanKeyInit(&keys[2], kRev3IdxPositionSegment,
                BTGreaterEqualStrategyNumber, F_INT4GE, Int32GetDatum(0));
            active_scan_ = bioseq::pg_utils::invoke_pg_as_cpp([&] {
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
            Crc64Ecma182 extent_checksum{};
            Crc64Ecma182 extent_row_checksum{};
            Crc64Ecma182 payload_row_checksum{};

            // One bridge covers up to K fetch/deform/projection operations.
            // Each projection borrows its own pinned slot; row authority, CRC
            // and arbitrary sinks execute only after PG_END_TRY, so neither a
            // receipt nor sink-owned C++ state crosses a siglongjmp boundary.
            Rev3PinnedProjectedPgRowBatch batch{};
            std::size_t batch_ordinal = 0;
            for (;;) {
                if (batch_ordinal == batch.size) {
                    if (batch.eof) break;
                    batch = next_rev3_pinned_projected_pg_row_batch_or_error(
                        heap_relation_, active_scan_,
                        full_generation_batch_slots_);
                    batch_ordinal = 0;
                    if (batch.size == 0) {
                        if (batch.eof) break;
                        fail_("full-generation batched payload scan made no progress");
                    }
                }
                const Rev3PinnedProjectedPgRow& pinned =
                    batch.rows[batch_ordinal++];
                record_payload_tid_(pinned.tid);
                LoadedRow row =
                    load_rev3_projected_pg_row_or_error(pinned.projected);
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
                const MinimizerBucketDirEntry& entry = entries[entry_index];
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
                const IndexChunkAddress expected_address{
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
                            [&](const IndexChunkAddress& receipt_address,
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
                extent_row_checksum.update_little_endian(
                    validated_row_checksum);
                if (position_extent) {
                    if (!format5 && extent_chunk == 0) {
                        const std::uint64_t control_checksum_bytes =
                            static_cast<std::uint64_t>(
                                entry.control_chunk_count)
                            * sizeof(std::uint64_t);
                        payload_row_checksum.value =
                            bioseq::indexing::crc64_ecma182_concatenate(
                                payload_row_checksum.checksum(),
                                control_extent_row_checksums[entry_index],
                                control_checksum_bytes);
                    }
                    payload_row_checksum.update_little_endian(
                        validated_row_checksum);
                    if constexpr (std::invocable<PositionSink&,
                            const ValidatedMinimizerGenerationRowReceipt&>) {
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
                    if (format5) {
                        payload_row_checksum.update_little_endian(
                            validated_row_checksum);
                    }
                    if constexpr (std::invocable<ControlSink&,
                            const ValidatedMinimizerGenerationRowReceipt&>) {
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
                    if (!position_extent && !format5) {
                        control_extent_row_checksums[entry_index] =
                            extent_row_checksum.checksum();
                    }
                    extent_chunk = 0;
                    extent_checksum = {};
                    extent_row_checksum = {};
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
            bioseq::pg_utils::invoke_pg_as_cpp(
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
            bioseq::parallelism::
                release_admitted_vector_capacity_before_permit(
                    control_extent_row_checksums,
                    checksum_plan_permit);

            local.read_subkeys = local.wanted_subkeys;
            local.read_extents = local.wanted_extents;
            if (local.heap_rows_read != local.delivered_chunks
                || local.delivered_chunks != local.wanted_chunks
                || local.delivered_bytes
                    != metadata_->manifest.payload_bytes) {
                fail_("full-generation payload proof equations disagree");
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
            state_ = State::full_generation_payload_complete;
        } catch (...) {
            cleanup_failed_session_and_rethrow_();
        }
        return local;
    }

    void plan_control_groups(
            std::span<const MinimizerGenerationPayloadReadGroup> groups) {
        plan_phase_groups_(PayloadPhase::controls, groups);
    }

    void plan_position_groups(
            std::span<const MinimizerGenerationPayloadReadGroup> groups) {
        plan_phase_groups_(PayloadPhase::positions, groups);
    }

    template <class Sink>
    requires bioseq::indexing::index_chunk_reader_fn<Sink>
    [[nodiscard]] IndexChunkReadStats read_chunks_in_range(
            std::size_t group_index,
            IndexChunkCachePolicy cache_policy,
            Sink&& sink) {
        IndexChunkReadStats local{};
        const BufferUsage payload_buffer_begin = pgBufferUsage;
        start_payload_timing_();
        try {
            const bool position_phase = state_ == State::positions_planned;
            if ((state_ != State::controls_planned && !position_phase)
                || group_index != next_group_
                || group_index >= planned_groups_.size()) {
                checked_add_(stats_.payload.repeated_chunk_reads, 1,
                    "payload repeated-read counter overflow");
                fail_("payload group was repeated, reordered, or not planned");
            }
            if (cache_policy != IndexChunkCachePolicy::bypass_one_shot) {
                fail_("attested generation session requires bypass_one_shot");
            }
            PlannedGroup& group = planned_groups_[group_index];
            if (group.position_extent != position_phase
                || group.executed || active_scan_ != nullptr
                || !runtime_requests_.empty() || runtime_permit_) {
                checked_add_(stats_.payload.repeated_chunk_reads, 1,
                    "payload repeated-read counter overflow");
                fail_("payload group was executed more than once or session state leaked");
            }
            if (group.requests.size()
                > (std::numeric_limits<std::uint64_t>::max)()
                    / sizeof(RuntimeRequest)) {
                fail_("payload runtime admission size overflow");
            }
            const std::uint64_t runtime_bytes =
                group.requests.size() * sizeof(RuntimeRequest);
            auto runtime_permit =
                bioseq::parallelism::MemoryAdmissionController::instance()
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
            for (const IndexSubKeyRequest& request : group.requests) {
                runtime_requests_.push_back(RuntimeRequest{
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
            ScanKeyInit(&keys[0], kRev3IdxGenerationId,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(
                    group.range.generation_id)));
            ScanKeyInit(&keys[1], kRev3IdxQueryKey,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(query_key_)));
            ScanKeyInit(&keys[2], kRev3IdxSubKey,
                BTGreaterEqualStrategyNumber, F_INT8GE,
                Int64GetDatum(static_cast<std::int64_t>(group.range.begin)));
            ScanKeyInit(&keys[3], kRev3IdxSubKey,
                BTLessStrategyNumber, F_INT8LT,
                Int64GetDatum(static_cast<std::int64_t>(group.range.end)));
            ScanKeyInit(&keys[4], kRev3IdxPositionSegment,
                BTEqualStrategyNumber, F_INT4EQ,
                Int32GetDatum(group.range.position_segment));
            active_scan_ = bioseq::pg_utils::invoke_pg_as_cpp([&] {
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
                 (tuple = bioseq::pg_utils::invoke_pg_as_cpp([&] {
                      return systable_getnext_ordered(
                          active_scan_, ForwardScanDirection);
                  })) != nullptr;) {
                record_payload_tid_(tuple);
                LoadedRow row =
                    load_rev3_pg_row_or_error(heap_relation_, tuple);
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
                const MinimizerBucketDirEntry& entry =
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
                    RuntimeRequest& request =
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

                RuntimeRequest* matched = nullptr;
                if (request_cursor < runtime_requests_.size()) {
                    RuntimeRequest& request =
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
                                [&](const IndexChunkAddress& receipt_address,
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
                    sink(row.address,
                        reinterpret_cast<const char*>(row.bytes.data()),
                        row.bytes.size());
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
            bioseq::pg_utils::invoke_pg_as_cpp(
                [&] { systable_endscan_ordered(active_scan_); });
            active_scan_ = nullptr;

            if (directory_ordinal != group.end_directory_ordinal
                || expected_chunk_no != 0) {
                fail_("payload scan ended before its complete directory range");
            }
            for (; request_cursor < runtime_requests_.size(); ++request_cursor) {
                const RuntimeRequest& request =
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
                fail_("payload proof-counter equation disagrees");
            }

            group.executed = true;
            ++next_group_;
            const bool phase_complete = next_group_ == planned_groups_.size();
            auto& phase_stats = position_phase
                ? stats_.positions : stats_.controls;
            checked_add_(phase_stats.executed_request_groups, 1,
                "payload phase executed-group counter overflow");
            checked_add_(phase_stats.range_scan_calls, 1,
                "payload phase range-scan counter overflow");
            checked_add_(phase_stats.physical_heap_rows_read,
                local.heap_rows_read,
                "payload phase physical-row counter overflow");
            checked_add_(phase_stats.physical_bytes_read,
                physical_payload_bytes,
                "payload phase physical-byte counter overflow");
            checked_add_read_stats_(phase_stats.read, local);
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
            std::vector<RuntimeRequest>{}.swap(runtime_requests_);
            runtime_permit_.release();
            if (phase_complete) {
                release_phase_plan_();
                state_ = position_phase
                    ? State::payload_complete : State::controls_complete;
            }
            add_cold_index_buffer_usage(stats_.cold, payload_buffer_begin);
        } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
            state_ = State::failed;
            abandon_relations_noexcept_();
            throw;
        } catch (...) {
            state_ = State::failed;
            throw;
        }
        return local;
    }

    /// Checked success close for a publication verifier that intentionally
    /// rereads only the active head. Borrowed snapshots remain caller-owned.
    void close_head_read() {
        try {
            if (state_ != State::head_read || active_scan_ != nullptr
                || heap_relation_ == nullptr
                || primary_index_relation_ == nullptr
                || snapshot_ == nullptr) {
                fail_("head-only generation read closed in an invalid state");
            }
            state_ = State::finished;
            close_relations_checked_();
        } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
            state_ = State::failed;
            abandon_relations_noexcept_();
            throw;
        } catch (...) {
            state_ = State::failed;
            throw;
        }
    }

    /// Finish a Format-5 read whose immutable payload was consumed from the
    /// exact generation-bound mmap image. PostgreSQL remains the authority for
    /// the active head, manifest, directory and source attestation; the image
    /// itself was created only by the complete authenticated READY reread and
    /// was fsynced and durably renamed before publication.
    [[nodiscard]] MinimizerGenerationReadStats
    finish_mapped_format5_payload() {
        try {
            if (state_ != State::metadata_ready
                || !catalog_source_attested_
                || !head_ || !metadata_
                || metadata_->manifest.wire_format
                    != bioseq::indexing::MinimizerGenerationWireFormat::
                        grouped_shape_soa_segment_local
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
                    "mapped Format-5 generation read closed in an invalid state");
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
                fail_("mapped Format-5 metadata/source proof disagrees");
            }
            stats_.total_heap_rows_read = stats_.metadata_heap_rows_read;
            stats_.total_bytes_read = stats_.metadata_bytes_read;
            stats_.cold.add(
                stats_.cold.toast_values, stats_.toast_values_read);
            stats_.cold.add(
                stats_.cold.detoast_bytes, stats_.detoast_bytes);
            state_ = State::finished;
            close_relations_checked_();
            return stats_;
        } catch (...) {
            state_ = State::failed;
            throw;
        }
    }

    /// Checked success close for the timed compact-H32 mmap route. PostgreSQL
    /// authenticates exactly one head row and one manifest row; the exact
    /// chunk_no=0 scan qualification proves that no directory tuple was read.
    /// Any control/payload planning or scan evidence makes this transition
    /// invalid rather than silently falling back to the relation reader.
    [[nodiscard]] MinimizerGenerationReadStats
    finish_mapped_compact_direct() {
        try {
            const MinimizerGenerationManifest* const manifest =
                active_manifest_();
            const bool cold_payload_evidence_is_zero =
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
            if (state_ != State::metadata_ready
                || !catalog_source_attested_ || !head_
                || manifest == nullptr || !manifest_only_ || metadata_
                || manifest != &*manifest_only_
                || manifest->wire_format
                    != bioseq::indexing::MinimizerGenerationWireFormat::
                        grouped_shape_soa_segment_local
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
                || grouped32_cold_route_
                || stats_.payload_heap_rows_read != 0u
                || stats_.payload_bytes_read != 0u
                || stats_.bulk_read_calls != 0u
                || stats_.planned_request_groups != 0u
                || stats_.executed_request_groups != 0u
                || stats_.range_scan_calls != 0u
                || stats_.cache_lookups != 0u
                || stats_.cache_populations != 0u
                || !read_stats_are_zero_(stats_.payload)
                || !phase_stats_are_zero_(stats_.controls)
                || !phase_stats_are_zero_(stats_.positions)
                || !full_payload_stats_are_zero_(stats_.full_generation)
                || !cold_payload_evidence_is_zero) {
                fail_(
                    "mapped compact-H32 generation read closed in an invalid state");
            }

            constexpr std::uint64_t expected_manifest_bytes =
                MinimizerGenerationManifest::format5_encoded_bytes;
            constexpr std::uint64_t expected_metadata_bytes =
                MinimizerGenerationHead::encoded_bytes
                + expected_manifest_bytes;
            if (stats_.relation_opens != 1u
                || stats_.head_scans != 1u
                || stats_.cold.head_scans != 1u
                || stats_.head_rows_read != 1u
                || stats_.head_bytes_read
                    != MinimizerGenerationHead::encoded_bytes
                || stats_.manifest_directory_scans != 1u
                || stats_.cold.directory_scans != 1u
                || stats_.manifest_rows_read != 1u
                || stats_.manifest_bytes_read != expected_manifest_bytes
                || stats_.directory_chunks_read != 0u
                || stats_.directory_bytes_read != 0u
                || stats_.metadata_repeated_reads != 0u
                || stats_.metadata_heap_rows_read != 2u
                || stats_.metadata_bytes_read != expected_metadata_bytes
                || stats_.index_scans != 2u
                || stats_.explicit_copy_bytes != expected_metadata_bytes
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
                    "mapped compact-H32 metadata/source evidence disagrees");
            }

            stats_.total_heap_rows_read = 2u;
            stats_.total_bytes_read = expected_metadata_bytes;
            state_ = State::finished;
            close_relations_checked_();
            return stats_;
        } catch (...) {
            state_ = State::failed;
            throw;
        }
    }

    /// Checked success close for callers that intentionally validate only the
    /// immutable head/manifest/directory contract and do not enter payload
    /// phases. Borrowed snapshots remain caller-owned.
    void close_metadata_read() {
        try {
            if ((state_ != State::metadata_unattested
                    && state_ != State::metadata_ready)
                || active_manifest_() == nullptr
                || active_scan_ != nullptr || heap_relation_ == nullptr
                || primary_index_relation_ == nullptr
                || snapshot_ == nullptr) {
                fail_("metadata-only generation read closed in an invalid state");
            }
            state_ = State::finished;
            close_relations_checked_();
        } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
            state_ = State::failed;
            abandon_relations_noexcept_();
            throw;
        } catch (...) {
            state_ = State::failed;
            throw;
        }
    }

    [[nodiscard]] MinimizerGenerationReadStats finish() {
        MinimizerGenerationReadStats result{};
        try {
            if (state_ == State::full_generation_payload_complete) {
                return finish_full_generation_();
            }
            if (state_ != State::payload_complete
                || next_group_ != 0 || !planned_groups_.empty()
                || !runtime_requests_.empty() || plan_permit_
                || runtime_permit_ || control_authorization_permit_
                || !authorized_position_ordinals_.empty()
                || active_scan_ != nullptr || heap_relation_ == nullptr
                || primary_index_relation_ == nullptr || snapshot_ == nullptr) {
                fail_("generation read finished before position payload completed or with leaked phase state");
            }
            std::uint64_t expected_metadata_rows = 0;
            checked_add_(expected_metadata_rows, stats_.head_rows_read,
                "metadata proof row equation overflow");
            checked_add_(expected_metadata_rows, stats_.manifest_rows_read,
                "metadata proof row equation overflow");
            checked_add_(expected_metadata_rows,
                stats_.directory_chunks_read,
                "metadata proof row equation overflow");
            std::uint64_t expected_metadata_bytes = 0;
            checked_add_(expected_metadata_bytes, stats_.head_bytes_read,
                "metadata proof byte equation overflow");
            checked_add_(expected_metadata_bytes, stats_.manifest_bytes_read,
                "metadata proof byte equation overflow");
            checked_add_(expected_metadata_bytes, stats_.directory_bytes_read,
                "metadata proof byte equation overflow");
            std::uint64_t expected_index_scans = 0;
            checked_add_(expected_index_scans, stats_.head_scans,
                "index scan proof equation overflow");
            checked_add_(expected_index_scans,
                stats_.manifest_directory_scans,
                "index scan proof equation overflow");
            checked_add_(expected_index_scans, stats_.range_scan_calls,
                "index scan proof equation overflow");
            std::uint64_t expected_payload_rows = 0;
            checked_add_(expected_payload_rows,
                stats_.payload.delivered_chunks,
                "payload proof row equation overflow");
            checked_add_(expected_payload_rows, stats_.payload.skipped_rows,
                "payload proof row equation overflow");
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
                    != MinimizerGenerationHead::encoded_bytes
                || stats_.manifest_directory_scans != 1
                || stats_.manifest_rows_read != 1
                || stats_.manifest_bytes_read
                    != bioseq::indexing::minimizer_manifest_encoded_bytes(
                        metadata_->manifest.wire_format)
                || stats_.metadata_heap_rows_read != expected_metadata_rows
                || stats_.metadata_bytes_read != expected_metadata_bytes
                || !phase_equations_hold_(stats_.controls)
                || !phase_equations_hold_(stats_.positions)
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
                fail_("generation read final proof equations disagree");
            }
            stats_.total_heap_rows_read = 0;
            checked_add_(stats_.total_heap_rows_read,
                stats_.metadata_heap_rows_read,
                "total heap row proof equation overflow");
            checked_add_(stats_.total_heap_rows_read,
                stats_.payload_heap_rows_read,
                "total heap row proof equation overflow");
            stats_.total_bytes_read = 0;
            checked_add_(stats_.total_bytes_read,
                stats_.metadata_bytes_read,
                "total byte proof equation overflow");
            checked_add_(stats_.total_bytes_read,
                stats_.payload_bytes_read,
                "total byte proof equation overflow");
            stats_.cold.add(
                stats_.cold.toast_values, stats_.toast_values_read);
            stats_.cold.add(
                stats_.cold.detoast_bytes, stats_.detoast_bytes);
            finalize_payload_timing_();
            state_ = State::finished;
            close_relations_checked_();
            result = stats_;
        } catch (...) {
            state_ = State::failed;
            throw;
        }
        return result;
    }
};

static_assert(!std::is_default_constructible_v<
    IndexStorageFragmentGenerationReadSessionPg>);
static_assert(!std::is_copy_constructible_v<
    IndexStorageFragmentGenerationReadSessionPg>);
static_assert(std::is_nothrow_move_constructible_v<
    IndexStorageFragmentGenerationReadSessionPg>);
static_assert(!std::is_move_assignable_v<
    IndexStorageFragmentGenerationReadSessionPg>);

struct MinimizerGenerationReadSessionOpenResultPg {
    std::optional<IndexStorageFragmentGenerationReadSessionPg> session{};
    MinimizerGenerationRelationInspection failure{
        .kind = MinimizerGenerationRelationKind::malformed,
        .revision3_error = Rev3IndexSchemaError::none,
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
[[nodiscard]] inline MinimizerGenerationReadSessionOpenResultPg
open_exact_minimizer_generation_read_session_pg(
        std::string_view table_name,
        std::string_view index_name,
        std::uint64_t query_key,
        const BorrowedExecutorReadContextPg& executor_read,
        ExpectedMinimizerCompatibility expected) {
    if (!executor_read.valid()) {
        rev3_storage_error_as_cpp(
            "generation read requires a valid borrowed executor context");
    }
    MinimizerGenerationReadSessionOpenResultPg result{};
    try {
        result.session.emplace(
            table_name, query_key, executor_read.snapshot(), expected,
            executor_read.tablespace_oid(),
            Rev3SnapshotRegistration::borrow_registered, index_name);
    } catch (const MinimizerGenerationRelationOpenFailurePg& failure) {
        result.failure = failure.inspection;
    }
    return result;
}

namespace rev3_compile_contract_detail {
struct QualifiedReadSink {
    void operator()(
        const IndexChunkAddress&, const char*, std::size_t) const noexcept {}
};
}  // namespace rev3_compile_contract_detail

template IndexChunkReadStats
IndexStorageFragmentGenerationReadSessionPg::read_chunks_in_range<
    rev3_compile_contract_detail::QualifiedReadSink>(
        std::size_t,
        IndexChunkCachePolicy,
        rev3_compile_contract_detail::QualifiedReadSink&&);

/// Integration-only self-consistent metadata corruption. Both manifest and
/// head source-content fields, the head's manifest checksum, and both physical
/// row CRCs are rewritten together. Only an independent source-row decode can
/// detect the corruption.
inline void rev3_test_rewrite_source_checksum_pg(
        Oid table_oid,
        std::uint64_t query_key,
        std::uint64_t xor_mask) {
    if (!bioseq::pg_utils::invoke_pg_as_cpp([] { return superuser(); })
        || xor_mask == 0
        || !bioseq::indexing::sql_bigint_can_represent(query_key)) {
        rev3_storage_error_as_cpp(
            "rev3 checksum corruption input is invalid");
    }
    Relation relation = bioseq::pg_utils::invoke_pg_as_cpp(
        [table_oid] {
            return try_safe_relation_open(table_oid, AccessShareLock);
        });
    if (relation == nullptr
        || RelationGetNamespace(relation) != bioseq_internal_ns()) {
        if (relation != nullptr) {
            bioseq::pg_utils::invoke_pg_as_cpp(
                [relation] { relation_close(relation, AccessShareLock); });
        }
        rev3_storage_error_as_cpp(
            "rev3 checksum corruption relation is invalid");
    }
    const Rev3IndexSchemaInspection schema =
        bioseq::pg_utils::invoke_pg_as_cpp(
            [relation] { return inspect_rev3_index_schema(relation); });
    const std::string table_name{RelationGetRelationName(relation)};
    const Oid table_tablespace_oid = relation->rd_locator.spcOid;
    bioseq::pg_utils::invoke_pg_as_cpp(
        [relation] { relation_close(relation, AccessShareLock); });
    if (!schema) {
        rev3_storage_error_as_cpp(
            "rev3 checksum corruption schema is invalid");
    }

    Snapshot active_snapshot = bioseq::pg_utils::invoke_pg_as_cpp(
        [] { return GetActiveSnapshot(); });
    if (active_snapshot == nullptr || !IsMVCCSnapshot(active_snapshot)) {
        rev3_storage_error_as_cpp(
            "rev3 checksum corruption requires an active MVCC snapshot");
    }
    Snapshot registered_snapshot = bioseq::pg_utils::invoke_pg_as_cpp(
        [active_snapshot] { return RegisterSnapshot(active_snapshot); });
    MinimizerGenerationHead head{};
    MinimizerGenerationManifest manifest{};
    {
        constexpr ExpectedMinimizerCompatibility fixture_expected{
            .wire_format = bioseq::indexing::MinimizerGenerationWireFormat::
                sorted_dense_global64,
            .alphabet_code = 0,
            .storage_unit_byte_width = 8,
            .kmer_size = 15,
            .window_size = 10,
            .gpu_abi_version =
                bioseq::indexing::kMinimizerSortedDenseGpuAbiVersion,
            .minimizer_scheme_id =
                bioseq::indexing::kLegacyMinimapV1SchemeId,
            .minimizer_scheme_version =
                bioseq::indexing::kLegacyMinimapV1SchemeVersion,
            .producer_backend_id = bioseq::indexing::
                MinimizerProducerBackendId::scalar,
            .producer_backend_identity_version = bioseq::indexing::
                kMinimizerProducerBackendIdentityVersion,
        };
        IndexStorageFragmentGenerationReadSessionPg session{
            table_name, query_key, registered_snapshot,
            fixture_expected,
            table_tablespace_oid,
            Rev3SnapshotRegistration::borrow_registered};
        head = session.read_active_head_once();
        manifest = session.read_manifest_directory_once(head).manifest;
        session.close_metadata_read();
    }
    bioseq::pg_utils::invoke_pg_as_cpp(
        [registered_snapshot] { UnregisterSnapshot(registered_snapshot); });
    manifest.source_content_checksum ^= xor_mask;
    head.source_content_checksum = manifest.source_content_checksum;
    const auto manifest_bytes =
        bioseq::indexing::encode_minimizer_generation_manifest(manifest);
    if (!manifest_bytes) {
        rev3_storage_error_as_cpp(
            "rev3 checksum corruption could not encode manifest");
    }
    head.manifest_checksum = bioseq::indexing::crc64_ecma182(
        std::span<const std::byte>{*manifest_bytes});
    const auto head_bytes =
        bioseq::indexing::encode_minimizer_generation_head(head);
    if (!head_bytes) {
        rev3_storage_error_as_cpp(
            "rev3 checksum corruption could not encode head");
    }
    const std::uint64_t manifest_row_checksum =
        bioseq::indexing::crc64_index_chunk_row(
            head.generation_id, -1, DIRECTORY_SUB_KEY, 0,
            static_cast<std::uint32_t>(manifest_bytes->size()),
            std::span<const std::byte>{*manifest_bytes});
    const std::uint64_t head_row_checksum =
        bioseq::indexing::crc64_index_chunk_row(
            0, -1, GENERATION_HEAD_SUB_KEY, 0,
            static_cast<std::uint32_t>(head_bytes->size()),
            std::span<const std::byte>{*head_bytes});

    const auto make_bytea = [](std::span<const std::byte> bytes) {
        bytea* value = bioseq::pg_utils::invoke_pg_as_cpp([bytes] {
            return reinterpret_cast<bytea*>(
                palloc(VARHDRSZ + bytes.size()));
        });
        SET_VARSIZE(value, VARHDRSZ + bytes.size());
        std::memcpy(VARDATA(value), bytes.data(), bytes.size());
        return value;
    };
    const char* quoted_table = bioseq::pg_utils::invoke_pg_as_cpp(
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
        Int64GetDatum(static_cast<std::int64_t>(DIRECTORY_SUB_KEY)),
    };
    std::array<Datum, 6> head_arguments{
        PointerGetDatum(head_blob),
        Int32GetDatum(static_cast<std::int32_t>(head_bytes->size())),
        Int64GetDatum(checksum_to_pg_bigint(head_row_checksum)),
        Int64GetDatum(static_cast<std::int64_t>(query_key)),
        Int64GetDatum(0),
        Int64GetDatum(static_cast<std::int64_t>(GENERATION_HEAD_SUB_KEY)),
    };
    struct RewriteSpiState {
        int connect_status = 0;
        int manifest_status = 0;
        int head_status = 0;
        int finish_status = 0;
        std::uint64_t manifest_rows = 0;
        std::uint64_t head_rows = 0;
    } spi{};
    const char* sql_data = sql.c_str();
    bioseq::pg_utils::invoke_pg_as_cpp(
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
    bioseq::pg_utils::invoke_pg_as_cpp(
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
        rev3_storage_error_as_cpp(fmt::format(
            "rev3 checksum corruption SPI session disagrees: connect={} "
            "manifest_status={} manifest_rows={} head_status={} head_rows={} "
            "finish={}",
            spi.connect_status, spi.manifest_status, spi.manifest_rows,
            spi.head_status, spi.head_rows, spi.finish_status));
    }
    bioseq::pg_utils::invoke_pg_as_cpp(
        [] { CommandCounterIncrement(); });
}

/// Setup-only identity carried across the mandatory E2E restart. The timed
/// backend must obtain its own identity from statement attestation instead of
/// invoking this pre-restart inspection seam.
struct ReadyMinimizerGenerationIdentity {
    std::uint64_t generation_id = 0;
    std::uint64_t source_identity = 0;
    std::uint64_t source_content_checksum = 0;
    std::uint64_t compatibility_checksum = 0;

    [[nodiscard]] bool operator==(
        const ReadyMinimizerGenerationIdentity&) const = default;
};

/// Read-only, PostgreSQL-owned pre-restart audit for one exact relation row,
/// column and logical minimizer index. It validates the source relfilenode and
/// attribute, the exact eight-column sidecar/PK, the address-bound row CRCs,
/// fixed head/manifest wire, every directory chunk, and all cross-object
/// metadata equations under one registered MVCC snapshot. It intentionally
/// does not activate ordinal 2 or expose a standalone SQL symbol.
[[nodiscard]] inline ReadyMinimizerGenerationIdentity
inspect_ready_minimizer_generation_identity_pg(
        const Rev3IndexAdministrationIdentity& identity,
        std::uint64_t sequence_rowno,
        Snapshot executor_snapshot,
        ExpectedMinimizerCompatibility expected) {
    const auto rev3_storage_error = [](std::string_view message) {
        rev3_storage_error_as_cpp(message);
    };
    struct SourceInspectionState {
        Snapshot registered_snapshot = nullptr;
        MemoryContext work_context = nullptr;
        void* session_storage = nullptr;
        IndexStorageFragmentGenerationReadSessionPg* session = nullptr;
        bool session_constructed = false;
        std::optional<Rev3CatalogSourceProof> source_proof;
    };
    auto* source = new SourceInspectionState{};
    ReadyMinimizerGenerationIdentity result{};
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
            rev3_storage_error(
                "generation identity inspection input is invalid");
        }
        source->registered_snapshot = executor_snapshot;
        source->source_proof.emplace(
            derive_rev3_catalog_source_proof_pg(
                identity, sequence_rowno,
                source->registered_snapshot));
        const Rev3AuthoritativeSourceLocator& proven_source =
            source->source_proof->source_for_diagnostics();
        source->work_context = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            return AllocSetContextCreate(
                CurrentMemoryContext, "bioseq rev3 source inspection",
                ALLOCSET_DEFAULT_SIZES);
        });

        source->session_storage = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            return MemoryContextAlloc(
                source->work_context,
                sizeof(IndexStorageFragmentGenerationReadSessionPg));
        });
        source->session = ::new (source->session_storage)
            IndexStorageFragmentGenerationReadSessionPg{
                identity.table_name,
                sequence_rowno,
                source->registered_snapshot,
                expected,
                identity.tablespace_oid,
                Rev3SnapshotRegistration::borrow_registered};
        source->session_constructed = true;
        const MinimizerGenerationHead head =
            source->session->read_active_head_once();
        const MinimizerGenerationReadMetadata& metadata =
            source->session->read_manifest_directory_once(head);
        if (metadata.manifest.alphabet_code
                != proven_source.alphabet_code
            || metadata.manifest.storage_unit_byte_width
                != proven_source.storage_unit_byte_width
            || metadata.manifest.target_length
                != proven_source.locator.total_bp_size) {
            rev3_storage_error(
                "generation identity manifest/source row disagrees");
        }
        const std::uint64_t proven_source_content_checksum =
            source->source_proof
                ->source_content_checksum_for_diagnostics();
        const std::uint64_t proven_compatibility_checksum =
            bioseq::indexing::minimizer_generation_compatibility_checksum(
                metadata.manifest);
        source->session->validate_source_attestation(
            std::move(*source->source_proof));
        source->source_proof.reset();
        result = ReadyMinimizerGenerationIdentity{
            .generation_id = metadata.head.generation_id,
            .source_identity = metadata.head.source_identity,
            .source_content_checksum = proven_source_content_checksum,
            .compatibility_checksum = proven_compatibility_checksum,
        };
        source->session->plan_control_groups(
            std::span<const MinimizerGenerationPayloadReadGroup>{});
        source->session->plan_position_groups(
            std::span<const MinimizerGenerationPayloadReadGroup>{});
        const MinimizerGenerationReadStats read_stats =
            source->session->finish();
        if (read_stats.source_parent_rows_read != 1
            || read_stats.source_catalog_rows_read != 1
            || read_stats.source_catalog_index_scans != 1
            || read_stats.source_fragment_rows_read != 0
            || read_stats.source_decoded_bp != 0) {
            rev3_storage_error(
                "generation identity timed source work equation disagrees");
        }
        source->session->~IndexStorageFragmentGenerationReadSessionPg();
        source->session = nullptr;
        source->session_constructed = false;
        source->session_storage = nullptr;

        source->registered_snapshot = nullptr;
        MemoryContext finished_context = std::exchange(
            source->work_context, nullptr);
        bioseq::pg_utils::invoke_pg_as_cpp(
            [&] { MemoryContextDelete(finished_context); });
    } catch (...) {
        if (source->session_constructed && source->session != nullptr) {
            auto* cleanup_session = source->session;
            source->session = nullptr;
            source->session_constructed = false;
            cleanup_session->~IndexStorageFragmentGenerationReadSessionPg();
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

namespace rev3_pg_compile_contract_detail {
using Publisher = ReadyGenerationPublishedHeadReceipt (*)(
    ReadyGenerationToken&&);
using Inspector = ReadyMinimizerGenerationIdentity (*)(
    const Rev3IndexAdministrationIdentity&,
    std::uint64_t,
    Snapshot,
    ExpectedMinimizerCompatibility);

static_assert(std::same_as<
    decltype(&publish_ready_minimizer_generation_head_pg), Publisher>);
static_assert(std::same_as<
    decltype(&inspect_ready_minimizer_generation_identity_pg), Inspector>);
static_assert(requires(
    MappedMinimizerFormat5ImagePg& image,
    const MappedMinimizerFormat5LeaseGuardPg& guard,
    const bioseq::indexing::CompactH32DirectIndexView& direct) {
    { image.borrow_mapping_token() }
        -> std::same_as<std::shared_ptr<const void>>;
    { image.advise_compact_h32_direct_metadata_readahead(direct) }
        -> std::same_as<void>;
    { image.borrow_compact_h32_selected_page_adviser(direct) }
        -> std::same_as<
            bioseq::indexing::CompactDirectSelectedPageAdviser>;
    { std::move(image).release_mapping_lease_guard() }
        -> std::same_as<MappedMinimizerFormat5LeaseGuardPg>;
    { guard.borrow_mapping_token() }
        -> std::same_as<std::shared_ptr<const void>>;
});
static_assert(noexcept(std::declval<const MappedMinimizerFormat5ImagePg&>()
    .advise_compact_h32_direct_metadata_readahead(
        std::declval<const bioseq::indexing::CompactH32DirectIndexView&>())));
static_assert(noexcept(std::declval<const MappedMinimizerFormat5ImagePg&>()
    .borrow_compact_h32_selected_page_adviser(
        std::declval<const bioseq::indexing::CompactH32DirectIndexView&>())));
static_assert(requires(
    IndexStorageFragmentGenerationRowSinkPg& writer,
    const EncodedMinimizerGeneration& generation,
    const MinimizerGenerationRowView& row) {
    { writer.write(row) }
        -> std::same_as<std::expected<void, MinimizerGenerationWireError>>;
    { writer.validate_ready_generation(generation) }
        -> std::same_as<ReadyGenerationToken>;
});
static_assert(requires(
    IndexStorageFragmentGenerationReadSessionPg& session,
    const MinimizerGenerationHead& head,
    const MinimizerGrouped32ColdRoutePlan& route,
    std::span<const MinimizerGenerationPayloadReadGroup> groups) {
    { session.read_active_head_once() }
        -> std::same_as<MinimizerGenerationHead>;
    { session.read_manifest_directory_once(head) }
        -> std::same_as<const MinimizerGenerationReadMetadata&>;
    { session.read_manifest_once_without_directory(head) }
        -> std::same_as<const MinimizerGenerationManifest&>;
    { session.bind_grouped32_cold_route_once(route) };
    { session.plan_control_groups(groups) };
    { session.plan_position_groups(groups) };
    { session.read_full_generation_payload_once(
        rev3_compile_contract_detail::QualifiedReadSink{},
        rev3_compile_contract_detail::QualifiedReadSink{}) }
        -> std::same_as<IndexChunkReadStats>;
    { session.finish() } -> std::same_as<MinimizerGenerationReadStats>;
    { session.finish_mapped_compact_direct() }
        -> std::same_as<MinimizerGenerationReadStats>;
});

constexpr Rev3AuthoritativeSourceLocator kSourceIdentityGolden{
    .sequence_rowno = 17,
    .locator = seq_locator{
        .version = kLocatorVersion,
        .bucket_idx = 9,
        ._pad_2 = 0,
        .shard_idx = 4,
        .num_fragments = 3,
        ._pad_4 = 0,
        .total_bp_size = 12'345,
        .first_sub_row_no = 77,
        .source_epoch = 101,
    },
    .alphabet_code = 1,
    .storage_unit_byte_width = 8,
    .padding_letters = 3,
    .compatibility_checksum = 0x0102030405060708ULL,
};
constexpr auto kSourceIdentityDifferentEpoch = [] {
    auto source = kSourceIdentityGolden;
    ++source.locator.source_epoch;
    return source;
}();
static_assert(rev3_source_identity(42, kSourceIdentityGolden) == 101);
static_assert(rev3_source_identity(42, kSourceIdentityGolden)
    != rev3_source_identity(42, kSourceIdentityDifferentEpoch));
static_assert(rev3_source_identity(42, kSourceIdentityGolden)
    == rev3_source_identity(43, kSourceIdentityGolden));
}  // namespace rev3_pg_compile_contract_detail

// =========================================================================
//  Format-6 PostgreSQL storage adapter
// =========================================================================

namespace format6_pg_detail {

[[nodiscard]] constexpr std::size_t stream_index(
        MinimizerFormat6StreamKind kind) noexcept {
    return static_cast<std::size_t>(kind) - 1u;
}

[[nodiscard]] constexpr MinimizerFormat6StreamKind stream_kind(
        std::size_t index) noexcept {
    return static_cast<MinimizerFormat6StreamKind>(index + 1u);
}

[[nodiscard]] constexpr bool valid_stream_kind(
        MinimizerFormat6StreamKind kind) noexcept {
    return kind == MinimizerFormat6StreamKind::directory
        || kind == MinimizerFormat6StreamKind::controls
        || kind == MinimizerFormat6StreamKind::postings;
}

[[nodiscard]] constexpr index_sub_key_t stream_sub_key(
        MinimizerFormat6StreamKind kind) noexcept {
    return static_cast<index_sub_key_t>(
        static_cast<std::uint16_t>(kind));
}

[[nodiscard]] constexpr std::uint64_t ceil_rows(
        std::uint64_t bytes) noexcept {
    return bytes / kMinimizerFormat6RowPayloadBytes
        + (bytes % kMinimizerFormat6RowPayloadBytes != 0u ? 1u : 0u);
}

[[nodiscard]] inline BufFile* create_spool() {
    BufFile* file = bioseq::pg_utils::invoke_pg_as_cpp(
        [] { return BufFileCreateTemp(false); });
    if (file == nullptr) {
        rev3_storage_error_as_cpp(
            "Format-6 temporary logical stream could not be created");
    }
    return file;
}

inline void append_spool(
        BufFile* file,
        std::span<const std::byte> bytes) {
    if (file == nullptr || bytes.empty()) {
        rev3_storage_error_as_cpp(
            "Format-6 logical stream append is invalid");
    }
    bioseq::pg_utils::invoke_pg_as_cpp(
        [file, bytes] {
            BufFileWrite(file, bytes.data(), bytes.size());
        });
}

inline void rewind_spool(BufFile* file) {
    if (file == nullptr
        || bioseq::pg_utils::invoke_pg_as_cpp(
            [file] { return BufFileSeek(file, 0, 0, SEEK_SET); }) != 0) {
        rev3_storage_error_as_cpp(
            "Format-6 logical stream rewind failed");
    }
}

inline void read_spool_exact(
        BufFile* file,
        std::span<std::byte> bytes) {
    if (file == nullptr || bytes.empty()) {
        rev3_storage_error_as_cpp(
            "Format-6 logical stream read is invalid");
    }
    bioseq::pg_utils::invoke_pg_as_cpp(
        [file, bytes] {
            BufFileReadExact(file, bytes.data(), bytes.size());
        });
}

inline void close_spool(BufFile*& file) {
    BufFile* closing = std::exchange(file, nullptr);
    if (closing != nullptr) {
        bioseq::pg_utils::invoke_pg_as_cpp(
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
        const IndexChunkAddress& address,
        std::span<const std::byte> bytes,
        std::uint64_t checksum) {
    if (relation == nullptr || bytes.empty()
        || bytes.size() > kMinimizerFormat6RowPayloadBytes
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
        rev3_storage_error_as_cpp(
            "Format-6 row envelope is invalid");
    }
    bytea* blob = bioseq::pg_utils::invoke_pg_as_cpp(
        [bytes] {
            return reinterpret_cast<bytea*>(
                palloc(VARHDRSZ + bytes.size()));
        });
    SET_VARSIZE(blob, VARHDRSZ + bytes.size());
    std::memcpy(VARDATA(blob), bytes.data(), bytes.size());
    bioseq::pg_utils::pg_datum_list<8> values{};
    values.set(kRev3AttQueryKey, static_cast<std::int64_t>(query_key));
    values.set(kRev3AttGenerationId,
        static_cast<std::int64_t>(address.generation_id));
    values.set(kRev3AttPositionSegment, address.position_segment);
    values.set(kRev3AttSubKey,
        static_cast<std::int64_t>(address.sub_key));
    values.set(kRev3AttChunkNo,
        static_cast<std::int32_t>(address.chunk_no));
    values.set(kRev3AttChunkBytes,
        static_cast<std::int32_t>(bytes.size()));
    values.set(kRev3AttChecksum, checksum_to_pg_bigint(checksum));
    values.set_raw(kRev3AttBlob,
        bioseq::pg_utils::pg_datum::from_pointer(blob));
    HeapTuple tuple = bioseq::pg_utils::invoke_pg_as_cpp(
        [&] { return values.form_tuple(RelationGetDescr(relation)); });
    bioseq::pg_utils::invoke_pg_as_cpp([blob] { pfree(blob); });
    return tuple;
}

[[nodiscard]] inline HeapTuple form_row(
        Relation relation,
        std::uint64_t query_key,
        const IndexChunkAddress& address,
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

} // namespace format6_pg_detail

class ReadyMinimizerFormat6Token;

struct ReadyMinimizerFormat6PublishedHeadReceipt {
    std::string table_name;
    std::uint64_t query_key = 0u;
    MinimizerFormat6Head head{};
    EncodedMinimizerFormat6Head head_bytes{};
    std::optional<std::uint64_t> replaced_generation_id{};
};

[[nodiscard]] inline ReadyMinimizerFormat6PublishedHeadReceipt
publish_ready_minimizer_format6_head_pg(
    ReadyMinimizerFormat6Token&& token);

/// Move-only proof that the exact unpublished Format-6 generation was
/// authenticated from PostgreSQL rows and bound to the authoritative source.
class ReadyMinimizerFormat6Token {
private:
    Rev3IndexAdministrationIdentity identity_{};
    Oid sidecar_relation_oid_ = InvalidOid;
    RelFileLocator sidecar_locator_{};
    std::uint64_t advisory_lock_key_ = 0u;
    std::uint64_t query_key_ = 0u;
    std::uint64_t generation_id_ = 0u;
    TransactionId transaction_id_ = InvalidTransactionId;
    MinimizerFormat6Head head_{};
    EncodedMinimizerFormat6Head head_bytes_{};
    std::uint64_t head_row_checksum_ = 0u;
    bool valid_ = false;

    ReadyMinimizerFormat6Token(
            Rev3IndexAdministrationIdentity identity,
            Oid sidecar_relation_oid,
            RelFileLocator sidecar_locator,
            std::uint64_t advisory_lock_key,
            std::uint64_t query_key,
            std::uint64_t generation_id,
            TransactionId transaction_id,
            const MinimizerFormat6Head& head,
            const EncodedMinimizerFormat6Head& head_bytes,
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

    friend class IndexStorageMinimizerFormat6WriterPg;
    friend ReadyMinimizerFormat6PublishedHeadReceipt
    publish_ready_minimizer_format6_head_pg(
        ReadyMinimizerFormat6Token&& token);

public:
    ReadyMinimizerFormat6Token() = delete;
    ReadyMinimizerFormat6Token(
        const ReadyMinimizerFormat6Token&) = delete;
    ReadyMinimizerFormat6Token& operator=(
        const ReadyMinimizerFormat6Token&) = delete;
    ReadyMinimizerFormat6Token(
            ReadyMinimizerFormat6Token&& other) noexcept
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
    ReadyMinimizerFormat6Token& operator=(
        ReadyMinimizerFormat6Token&&) = delete;
    ~ReadyMinimizerFormat6Token() = default;
};

static_assert(!std::is_default_constructible_v<
    ReadyMinimizerFormat6Token>);
static_assert(!std::is_copy_constructible_v<
    ReadyMinimizerFormat6Token>);
static_assert(std::is_nothrow_move_constructible_v<
    ReadyMinimizerFormat6Token>);

inline constexpr std::uint64_t kFormat6PublicationSliceBytes =
    std::uint64_t{8} << 20u;

/// PostgreSQL-owned bounded sink for the storage-neutral Format-6 encoder.
/// The callback path only appends to three ResourceOwner-managed BufFiles.
/// Sealing replays exact 8,064-byte rows in postings -> controls -> directory
/// order, writes the manifest, and rereads the complete unpublished generation
/// before a private publication token can be produced.
class IndexStorageMinimizerFormat6WriterPg {
private:
    enum class State : std::uint8_t {
        accepting,
        sealed,
        failed,
        moved,
    };

    static constexpr std::uint64_t kBatchRows = 1'024u;

    Rev3IndexAdministrationIdentity identity_{};
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
    std::array<Crc64Ecma182, 3> logical_crc_{};
    bioseq::parallelism::MemoryPermit publication_permit_{};
    std::vector<TupleTableSlot*> pending_slots_{};
    std::uint64_t pending_tuple_bytes_ = 0u;
    std::array<std::byte, kMinimizerFormat6RowPayloadBytes> row_buffer_{};
    State state_ = State::accepting;

    [[noreturn]] void fail_(std::string_view message) {
        state_ = State::failed;
        rev3_storage_error_as_cpp(message);
    }

    void abandon_noexcept_() noexcept {
        relation_ = nullptr;
        catalog_indexes_ = nullptr;
        validation_snapshot_ = nullptr;
        validation_primary_index_ = nullptr;
        validation_scan_ = nullptr;
        for (BufFile*& file : stream_spools_) {
            format6_pg_detail::abandon_spool(file);
        }
        pending_slots_.clear();
        std::vector<TupleTableSlot*>{}.swap(pending_slots_);
        pending_tuple_bytes_ = 0u;
        publication_permit_.release();
    }

    void drop_pending_slots_() {
        for (TupleTableSlot* slot : pending_slots_) {
            if (slot != nullptr) {
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [slot] { ExecDropSingleTupleTableSlot(slot); });
            }
        }
        pending_slots_.clear();
        pending_tuple_bytes_ = 0u;
    }

    void flush_batch_() {
        if (pending_slots_.empty()) {
            if (pending_tuple_bytes_ != 0u) {
                fail_("Format-6 tuple batch accounting disagrees");
            }
            return;
        }
        if (relation_ == nullptr || catalog_indexes_ == nullptr
            || pending_slots_.size()
                > static_cast<std::size_t>(
                    (std::numeric_limits<int>::max)())) {
            fail_("Format-6 tuple batch state is invalid");
        }
        bioseq::pg_utils::invoke_pg_as_cpp([&] {
            CatalogTuplesMultiInsertWithInfo(
                relation_,
                pending_slots_.data(),
                static_cast<int>(pending_slots_.size()),
                catalog_indexes_);
        });
        drop_pending_slots_();
    }

    void queue_row_(
            const IndexChunkAddress& address,
            std::span<const std::byte> bytes) {
        HeapTuple tuple = format6_pg_detail::form_row(
            relation_, query_key_, address, bytes);
        const std::uint64_t tuple_bytes =
            static_cast<std::uint64_t>(tuple->t_len)
            + sizeof(TupleTableSlot*) + sizeof(HeapTupleData);
        const auto decision =
            bioseq::indexing::plan_revision3_tuple_batch_append(
                pending_slots_.size(),
                pending_tuple_bytes_,
                tuple_bytes,
                kBatchRows,
                kFormat6PublicationSliceBytes);
        if (!decision.valid) {
            heap_freetuple(tuple);
            fail_("Format-6 row exceeds the bounded publication batch");
        }
        if (decision.flush_before) flush_batch_();
        TupleTableSlot* slot = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            return MakeSingleTupleTableSlot(
                RelationGetDescr(relation_), &TTSOpsHeapTuple);
        });
        try {
            pending_slots_.push_back(slot);
        } catch (...) {
            heap_freetuple(tuple);
            bioseq::pg_utils::invoke_pg_as_cpp(
                [slot] { ExecDropSingleTupleTableSlot(slot); });
            throw;
        }
        bioseq::pg_utils::invoke_pg_as_cpp(
            [slot, tuple] { ExecStoreHeapTuple(tuple, slot, true); });
        if (tuple_bytes
            > (std::numeric_limits<std::uint64_t>::max)()
                - pending_tuple_bytes_) {
            fail_("Format-6 publication byte counter overflow");
        }
        pending_tuple_bytes_ += tuple_bytes;
        if (decision.flush_after) flush_batch_();
    }

    void replay_stream_(
            MinimizerFormat6StreamKind kind,
            const bioseq::indexing::MinimizerFormat6StreamDescriptor&
                descriptor) {
        using format6_pg_detail::ceil_rows;
        using format6_pg_detail::stream_index;
        const std::size_t index = stream_index(kind);
        if (descriptor.kind != kind
            || descriptor.logical_bytes != logical_bytes_[index]
            || descriptor.stream_crc64 != logical_crc_[index].checksum()
            || descriptor.row_count != ceil_rows(descriptor.logical_bytes)
            || descriptor.row_count
                > static_cast<std::uint64_t>(
                    (std::numeric_limits<std::int32_t>::max)())) {
            fail_("Format-6 stream descriptor disagrees with encoder output");
        }
        format6_pg_detail::rewind_spool(stream_spools_[index]);
        std::uint64_t remaining = descriptor.logical_bytes;
        for (std::uint64_t row = 0u; row < descriptor.row_count; ++row) {
            const std::size_t bytes = static_cast<std::size_t>(
                std::min<std::uint64_t>(
                    remaining, kMinimizerFormat6RowPayloadBytes));
            if (bytes == 0u
                || (row + 1u < descriptor.row_count
                    && bytes != kMinimizerFormat6RowPayloadBytes)) {
                fail_("Format-6 physical stream row equation disagrees");
            }
            auto payload = std::span<std::byte>{row_buffer_}.first(bytes);
            format6_pg_detail::read_spool_exact(
                stream_spools_[index], payload);
            queue_row_(
                IndexChunkAddress{
                    .generation_id = generation_id_,
                    .position_segment = 0,
                    .sub_key = format6_pg_detail::stream_sub_key(kind),
                    .chunk_no = static_cast<std::uint32_t>(row),
                },
                payload);
            remaining -= bytes;
        }
        if (remaining != 0u) {
            fail_("Format-6 physical stream replay was incomplete");
        }
    }

    [[nodiscard]] Rev3LoadedPgRow read_exact_row_(
            const IndexChunkAddress& expected_address) {
        ScanKeyData keys[5];
        ScanKeyInit(&keys[0], kRev3IdxGenerationId,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(
                expected_address.generation_id)));
        ScanKeyInit(&keys[1], kRev3IdxQueryKey,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(query_key_)));
        ScanKeyInit(&keys[2], kRev3IdxSubKey,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(
                expected_address.sub_key)));
        ScanKeyInit(&keys[3], kRev3IdxPositionSegment,
            BTEqualStrategyNumber, F_INT4EQ,
            Int32GetDatum(expected_address.position_segment));
        ScanKeyInit(&keys[4], kRev3IdxChunkNo,
            BTEqualStrategyNumber, F_INT4EQ,
            Int32GetDatum(static_cast<std::int32_t>(
                expected_address.chunk_no)));
        validation_scan_ = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            return systable_beginscan_ordered(
                relation_, validation_primary_index_,
                validation_snapshot_, 5, keys);
        });
        HeapTuple tuple = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            return systable_getnext_ordered(
                validation_scan_, ForwardScanDirection);
        });
        if (!HeapTupleIsValid(tuple)) {
            fail_("Format-6 validation row is missing");
        }
        Rev3LoadedPgRow row =
            load_rev3_pg_row_or_error(relation_, tuple);
        HeapTuple extra = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            return systable_getnext_ordered(
                validation_scan_, ForwardScanDirection);
        });
        bioseq::pg_utils::invoke_pg_as_cpp(
            [&] { systable_endscan_ordered(validation_scan_); });
        validation_scan_ = nullptr;
        if (HeapTupleIsValid(extra)
            || row.query_key != query_key_
            || row.address != expected_address) {
            row.release();
            fail_("Format-6 validation row identity is ambiguous");
        }
        return row;
    }

    void reread_stream_(
            MinimizerFormat6StreamKind kind,
            const bioseq::indexing::MinimizerFormat6StreamDescriptor&
                descriptor) {
        ScanKeyData keys[4];
        ScanKeyInit(&keys[0], kRev3IdxGenerationId,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(generation_id_)));
        ScanKeyInit(&keys[1], kRev3IdxQueryKey,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(query_key_)));
        ScanKeyInit(&keys[2], kRev3IdxSubKey,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(
                format6_pg_detail::stream_sub_key(kind))));
        ScanKeyInit(&keys[3], kRev3IdxPositionSegment,
            BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(0));
        validation_scan_ = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            return systable_beginscan_ordered(
                relation_, validation_primary_index_,
                validation_snapshot_, 4, keys);
        });
        Crc64Ecma182 crc;
        std::uint64_t bytes_read = 0u;
        std::uint64_t ordinal = 0u;
        for (HeapTuple tuple;
             HeapTupleIsValid(tuple =
                 bioseq::pg_utils::invoke_pg_as_cpp([&] {
                     return systable_getnext_ordered(
                         validation_scan_, ForwardScanDirection);
                 }));) {
            Rev3LoadedPgRow row =
                load_rev3_pg_row_or_error(relation_, tuple);
            const IndexChunkAddress expected_address{
                    .generation_id = generation_id_,
                    .position_segment = 0,
                    .sub_key = format6_pg_detail::stream_sub_key(kind),
                    .chunk_no = static_cast<std::uint32_t>(ordinal),
                };
            const std::uint64_t remaining =
                descriptor.logical_bytes - bytes_read;
            const std::size_t expected_bytes =
                static_cast<std::size_t>(std::min<std::uint64_t>(
                    remaining, kMinimizerFormat6RowPayloadBytes));
            if (ordinal >= descriptor.row_count
                || row.query_key != query_key_
                || row.address != expected_address
                || row.bytes.size() != expected_bytes) {
                row.release();
                fail_("Format-6 validation stream row length disagrees");
            }
            crc.update(row.bytes);
            bytes_read += row.bytes.size();
            ++ordinal;
            row.release();
        }
        bioseq::pg_utils::invoke_pg_as_cpp(
            [&] { systable_endscan_ordered(validation_scan_); });
        validation_scan_ = nullptr;
        if (ordinal != descriptor.row_count
            || bytes_read != descriptor.logical_bytes
            || crc.checksum() != descriptor.stream_crc64) {
            fail_("Format-6 validation stream aggregate disagrees");
        }
    }

    [[nodiscard]] static bool append_callback_(
            void* context,
            MinimizerFormat6StreamKind kind,
            std::span<const std::byte> bytes) {
        return static_cast<IndexStorageMinimizerFormat6WriterPg*>(
            context)->append_logical_bytes(kind, bytes);
    }

public:
    IndexStorageMinimizerFormat6WriterPg() = delete;
    IndexStorageMinimizerFormat6WriterPg(
            Rev3IndexAdministrationIdentity identity,
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
                fail_("Format-6 writer identity is invalid");
            }
            auto permit =
                bioseq::parallelism::MemoryAdmissionController::instance()
                    .try_reserve(kFormat6PublicationSliceBytes);
            if (!permit) {
                fail_("Format-6 publication memory admission was denied");
            }
            publication_permit_ = std::move(*permit);
            pending_slots_.reserve(kBatchRows);
            advisory_lock_key_ = index_advisory_xact_lock_key(
                identity_.database_oid,
                identity_.tablespace_oid,
                identity_.main_relfilenode,
                identity_.column_attno,
                identity_.index_name);
            bioseq::pg_utils::invoke_pg_as_cpp([&] {
                acquire_index_advisory_xact_lock(
                    identity_.database_oid,
                    identity_.tablespace_oid,
                    identity_.main_relfilenode,
                    identity_.column_attno,
                    identity_.index_name);
            });
            const Oid relation_oid =
                bioseq::pg_utils::invoke_pg_as_cpp([&] {
                    return get_relname_relid(
                        identity_.table_name.c_str(),
                        bioseq_internal_ns());
                });
            relation_ = OidIsValid(relation_oid)
                ? bioseq::pg_utils::invoke_pg_as_cpp([&] {
                    return try_safe_relation_open(
                        relation_oid, RowExclusiveLock);
                })
                : nullptr;
            if (relation_ == nullptr) {
                fail_("Format-6 generation relation is missing");
            }
            const Rev3IndexSchemaInspection schema =
                bioseq::pg_utils::invoke_pg_as_cpp([&] {
                    return inspect_rev3_index_schema(
                        relation_, nullptr,
                        Rev3IndexSchemaExpectation{
                            .namespace_oid = bioseq_internal_ns(),
                            .relation_name = identity_.table_name,
                            .tablespace_oid = identity_.tablespace_oid,
                        });
                });
            if (!schema) {
                fail_(fmt::format(
                    "Format-6 generation relation schema mismatch: {}",
                    rev3_index_schema_error_name(schema.error)));
            }
            (void)verify_pg_inline_payload_layout(relation_);
            catalog_indexes_ = bioseq::pg_utils::invoke_pg_as_cpp(
                [&] { return CatalogOpenIndexes(relation_); });
            if (catalog_indexes_ == nullptr) {
                fail_("Format-6 CatalogOpenIndexes returned NULL");
            }
            for (BufFile*& file : stream_spools_) {
                file = format6_pg_detail::create_spool();
            }
        } catch (...) {
            state_ = State::failed;
            abandon_noexcept_();
            throw;
        }
    }
    IndexStorageMinimizerFormat6WriterPg(
        const IndexStorageMinimizerFormat6WriterPg&) = delete;
    IndexStorageMinimizerFormat6WriterPg& operator=(
        const IndexStorageMinimizerFormat6WriterPg&) = delete;
    IndexStorageMinimizerFormat6WriterPg(
            IndexStorageMinimizerFormat6WriterPg&& other) noexcept
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
        other.state_ = State::moved;
    }
    IndexStorageMinimizerFormat6WriterPg& operator=(
        IndexStorageMinimizerFormat6WriterPg&&) = delete;
    ~IndexStorageMinimizerFormat6WriterPg() {
        abandon_noexcept_();
    }

    [[nodiscard]] bool append_logical_bytes(
            MinimizerFormat6StreamKind kind,
            std::span<const std::byte> bytes) {
        if (state_ != State::accepting
            || !format6_pg_detail::valid_stream_kind(kind)
            || bytes.empty()) {
            return false;
        }
        const std::size_t index =
            format6_pg_detail::stream_index(kind);
        if (bytes.size()
            > (std::numeric_limits<std::uint64_t>::max)()
                - logical_bytes_[index]) {
            return false;
        }
        format6_pg_detail::append_spool(
            stream_spools_[index], bytes);
        logical_crc_[index].update(bytes);
        logical_bytes_[index] += bytes.size();
        return true;
    }

    [[nodiscard]] MinimizerFormat6LogicalStreamSinkRef
    logical_stream_sink() noexcept {
        return {
            .context = this,
            .append_bytes = &append_callback_,
        };
    }

    [[nodiscard]] ReadyMinimizerFormat6Token
    seal_and_validate_ready(
            const MinimizerFormat6EncodedMetadata& metadata,
            Rev3AuthoritativeSourceProof source) {
        try {
            if (state_ != State::accepting) {
                fail_("Format-6 writer was sealed more than once");
            }
            auto decoded_head =
                bioseq::indexing::decode_minimizer_format6_head(
                    metadata.encoded_head);
            auto decoded_manifest =
                bioseq::indexing::decode_minimizer_format6_manifest(
                    metadata.encoded_manifest);
            if (!decoded_head || !decoded_manifest
                || *decoded_head != metadata.head
                || *decoded_manifest != metadata.manifest
                || metadata.head.generation_id != generation_id_
                || metadata.head.query_key != query_key_
                || metadata.manifest.generation_id != generation_id_
                || metadata.head.dense_seal.has_value()
                || metadata.manifest.streams[0].kind
                    != MinimizerFormat6StreamKind::directory
                || metadata.manifest.streams[1].kind
                    != MinimizerFormat6StreamKind::controls
                || metadata.manifest.streams[2].kind
                    != MinimizerFormat6StreamKind::postings) {
                fail_("Format-6 encoded metadata disagrees");
            }
            const TransactionId transaction_id =
                bioseq::pg_utils::invoke_pg_as_cpp(
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
                    "Format-6 source proof is not bound to this writer transaction");
            }
            for (std::size_t index = 0u; index < 3u; ++index) {
                const auto& stream = metadata.manifest.streams[index];
                if (stream.logical_bytes != logical_bytes_[index]
                    || stream.stream_crc64
                        != logical_crc_[index].checksum()
                    || stream.row_count
                        != format6_pg_detail::ceil_rows(
                            stream.logical_bytes)) {
                    fail_("Format-6 logical stream receipt disagrees");
                }
            }

            // Authority is persisted only after all postings, then controls,
            // then the directory.  The primary-key layout remains unchanged;
            // the head is still absent, so none of these rows is visible as a
            // READY generation during replay.
            replay_stream_(
                MinimizerFormat6StreamKind::postings,
                metadata.manifest.streams[2]);
            replay_stream_(
                MinimizerFormat6StreamKind::controls,
                metadata.manifest.streams[1]);
            replay_stream_(
                MinimizerFormat6StreamKind::directory,
                metadata.manifest.streams[0]);
            queue_row_(
                IndexChunkAddress{
                    .generation_id = generation_id_,
                    .position_segment = 0,
                    .sub_key = 0,
                    .chunk_no = 0u,
                },
                metadata.encoded_manifest);
            flush_batch_();
            bioseq::pg_utils::invoke_pg_as_cpp(
                [&] { CatalogCloseIndexes(catalog_indexes_); });
            catalog_indexes_ = nullptr;
            bioseq::pg_utils::invoke_pg_as_cpp(
                [] { CommandCounterIncrement(); });
            if (!bioseq::pg_utils::invoke_pg_as_cpp(
                    [] { return ActiveSnapshotSet(); })) {
                fail_("Format-6 validation requires an active snapshot");
            }
            Snapshot active_snapshot =
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [] { return GetActiveSnapshot(); });
            if (!IsMVCCSnapshot(active_snapshot)) {
                fail_("Format-6 validation snapshot is not MVCC");
            }
            bioseq::pg_utils::invoke_pg_as_cpp(
                [] { UpdateActiveSnapshotCommandId(); });
            validation_snapshot_ =
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [active_snapshot] {
                        return RegisterSnapshot(active_snapshot);
                    });
            const Oid primary_index_oid =
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [&] { return RelationGetPrimaryKeyIndex(relation_); });
            validation_primary_index_ = OidIsValid(primary_index_oid)
                ? bioseq::pg_utils::invoke_pg_as_cpp(
                    [primary_index_oid] {
                        return index_open(
                            primary_index_oid, AccessShareLock);
                    })
                : nullptr;
            if (validation_primary_index_ == nullptr) {
                fail_("Format-6 validation primary index is missing");
            }

            Rev3LoadedPgRow manifest_row = read_exact_row_(
                IndexChunkAddress{
                    .generation_id = generation_id_,
                    .position_segment = 0,
                    .sub_key = 0,
                    .chunk_no = 0u,
                });
            if (manifest_row.bytes.size()
                    != kMinimizerFormat6ManifestBytes
                || !std::ranges::equal(
                    manifest_row.bytes,
                    metadata.encoded_manifest)) {
                manifest_row.release();
                fail_("Format-6 validation manifest disagrees");
            }
            manifest_row.release();
            reread_stream_(
                MinimizerFormat6StreamKind::directory,
                metadata.manifest.streams[0]);
            reread_stream_(
                MinimizerFormat6StreamKind::controls,
                metadata.manifest.streams[1]);
            reread_stream_(
                MinimizerFormat6StreamKind::postings,
                metadata.manifest.streams[2]);

            const auto expected_source =
                bioseq::indexing::minimizer_format6_source_identity(
                    query_key_,
                    source.source_.locator.source_epoch,
                    source.source_content_checksum_,
                    source.source_.locator.total_bp_size);
            if (!source.valid_
                || source.source_.sequence_rowno != query_key_
                || source.table_name_ != identity_.table_name
                || source.source_.locator.source_epoch
                    != metadata.manifest.source_epoch
                || source.source_content_checksum_
                    != metadata.manifest.source_content_crc64
                || source.source_.locator.total_bp_size
                    != metadata.manifest.target_length
                || source.source_.alphabet_code
                    != metadata.manifest.compatibility.alphabet_code
                || expected_source != metadata.manifest.source_identity
                || expected_source != metadata.head.source_identity) {
                source.invalidate_();
                fail_("Format-6 validation source attestation disagrees");
            }
            source.invalidate_();

            Relation finished_index = std::exchange(
                validation_primary_index_, nullptr);
            bioseq::pg_utils::invoke_pg_as_cpp(
                [finished_index] {
                    index_close(finished_index, AccessShareLock);
                });
            Snapshot finished_snapshot = std::exchange(
                validation_snapshot_, nullptr);
            bioseq::pg_utils::invoke_pg_as_cpp(
                [finished_snapshot] {
                    UnregisterSnapshot(finished_snapshot);
                });
            for (BufFile*& file : stream_spools_) {
                format6_pg_detail::close_spool(file);
            }
            publication_permit_.release();

            const Oid proven_relation_oid = RelationGetRelid(relation_);
            const RelFileLocator proven_locator = relation_->rd_locator;
            Relation finished_relation =
                std::exchange(relation_, nullptr);
            bioseq::pg_utils::invoke_pg_as_cpp(
                [finished_relation] {
                    relation_close(finished_relation, RowExclusiveLock);
                });
            const std::uint64_t head_row_checksum =
                bioseq::indexing::crc64_index_chunk_row(
                    0u,
                    -1,
                    FORMAT6_GENERATION_HEAD_SUB_KEY,
                    0u,
                    static_cast<std::uint32_t>(
                        metadata.encoded_head.size()),
                    metadata.encoded_head);
            state_ = State::sealed;
            Rev3IndexAdministrationIdentity proven_identity =
                std::move(identity_);
            return ReadyMinimizerFormat6Token{
                std::move(proven_identity),
                proven_relation_oid,
                proven_locator,
                advisory_lock_key_,
                query_key_,
                generation_id_,
                transaction_id,
                metadata.head,
                metadata.encoded_head,
                head_row_checksum,
            };
        } catch (...) {
            state_ = State::failed;
            abandon_noexcept_();
            throw;
        }
    }
};

static_assert(!std::is_default_constructible_v<
    IndexStorageMinimizerFormat6WriterPg>);
static_assert(!std::is_copy_constructible_v<
    IndexStorageMinimizerFormat6WriterPg>);
static_assert(std::is_nothrow_move_constructible_v<
    IndexStorageMinimizerFormat6WriterPg>);

[[nodiscard]] inline ReadyMinimizerFormat6PublishedHeadReceipt
publish_ready_minimizer_format6_head_pg(
        ReadyMinimizerFormat6Token&& token) {
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
                != bioseq::pg_utils::invoke_pg_as_cpp(
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
                    0u, -1, FORMAT6_GENERATION_HEAD_SUB_KEY, 0u,
                    static_cast<std::uint32_t>(
                        token.head_bytes_.size()),
                    token.head_bytes_)) {
            rev3_storage_error_as_cpp(
                "Format-6 publication token is invalid");
        }
        auto decoded_head =
            bioseq::indexing::decode_minimizer_format6_head(
                token.head_bytes_);
        if (!decoded_head || *decoded_head != token.head_) {
            rev3_storage_error_as_cpp(
                "Format-6 publication token head disagrees");
        }
        bioseq::pg_utils::invoke_pg_as_cpp([&] {
            acquire_index_advisory_xact_lock(
                token.identity_.database_oid,
                token.identity_.tablespace_oid,
                token.identity_.main_relfilenode,
                token.identity_.column_attno,
                token.identity_.index_name);
        });
        relation = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            return try_safe_relation_open(
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
            rev3_storage_error_as_cpp(
                "Format-6 publication relation identity changed");
        }
        const Rev3IndexSchemaInspection schema =
            bioseq::pg_utils::invoke_pg_as_cpp([&] {
                return inspect_rev3_index_schema(
                    relation, nullptr,
                    Rev3IndexSchemaExpectation{
                        .namespace_oid = bioseq_internal_ns(),
                        .relation_name = token.identity_.table_name,
                        .tablespace_oid = token.identity_.tablespace_oid,
                    });
            });
        if (!schema) {
            rev3_storage_error_as_cpp(
                "Format-6 publication relation schema changed");
        }
        primary_index = bioseq::pg_utils::invoke_pg_as_cpp(
            [&] { return index_open(
                schema.primary_index_oid, AccessShareLock); });
        replacement = format6_pg_detail::form_row(
            relation,
            token.query_key_,
            IndexChunkAddress{
                .generation_id = 0u,
                .position_segment = -1,
                .sub_key = FORMAT6_GENERATION_HEAD_SUB_KEY,
                .chunk_no = 0u,
            },
            token.head_bytes_,
            token.head_row_checksum_);

        ScanKeyData keys[5];
        ScanKeyInit(&keys[0], kRev3IdxGenerationId,
            BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum(0));
        ScanKeyInit(&keys[1], kRev3IdxQueryKey,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(
                token.query_key_)));
        ScanKeyInit(&keys[2], kRev3IdxSubKey,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(
                FORMAT6_GENERATION_HEAD_SUB_KEY)));
        ScanKeyInit(&keys[3], kRev3IdxPositionSegment,
            BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(-1));
        ScanKeyInit(&keys[4], kRev3IdxChunkNo,
            BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(0));
        scan = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            return systable_beginscan_ordered(
                relation, primary_index, SnapshotSelf, 5, keys);
        });
        HeapTuple previous_tuple =
            bioseq::pg_utils::invoke_pg_as_cpp([&] {
                return systable_getnext_ordered(
                    scan, ForwardScanDirection);
            });
        std::optional<std::uint64_t> replaced_generation_id;
        ItemPointerData previous_tid{};
        bool replace = HeapTupleIsValid(previous_tuple);
        if (replace) {
            previous_tid = previous_tuple->t_self;
            Rev3LoadedPgRow previous =
                load_rev3_pg_row_or_error(relation, previous_tuple);
            if (previous.query_key != token.query_key_
                || previous.address != IndexChunkAddress{
                    .generation_id = 0u,
                    .position_segment = -1,
                    .sub_key = FORMAT6_GENERATION_HEAD_SUB_KEY,
                    .chunk_no = 0u,
                }
                || previous.bytes.size()
                    != kMinimizerFormat6HeadBytes) {
                previous.release();
                rev3_storage_error_as_cpp(
                    "previous Format-6 head row is malformed");
            }
            auto previous_head =
                bioseq::indexing::decode_minimizer_format6_head(
                    previous.bytes);
            previous.release();
            if (!previous_head
                || previous_head->generation_id == 0u
                || previous_head->generation_id
                    == token.generation_id_) {
                rev3_storage_error_as_cpp(
                    "previous Format-6 head generation is invalid");
            }
            replaced_generation_id =
                previous_head->generation_id;
        }
        HeapTuple extra = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            return systable_getnext_ordered(
                scan, ForwardScanDirection);
        });
        bioseq::pg_utils::invoke_pg_as_cpp(
            [&] { systable_endscan_ordered(scan); });
        scan = nullptr;
        if (HeapTupleIsValid(extra)) {
            rev3_storage_error_as_cpp(
                "Format-6 head primary key returned duplicates");
        }
        if (replace) {
            bioseq::pg_utils::invoke_pg_as_cpp([&] {
                CatalogTupleUpdate(
                    relation, &previous_tid, replacement);
            });
        } else {
            bioseq::pg_utils::invoke_pg_as_cpp([&] {
                CatalogTupleInsert(relation, replacement);
            });
        }
        heap_freetuple(replacement);
        replacement = nullptr;
        bioseq::pg_utils::invoke_pg_as_cpp(
            [] { CommandCounterIncrement(); });

        // systable_beginscan_ordered() rewrites heap attribute numbers in
        // place to index ordinals. Reinitialize before reusing this array;
        // passing the already-remapped keys through a second time produces
        // the non-monotone ordinal sequence 2,1,4,3,5 for this primary key.
        ScanKeyInit(&keys[0], kRev3IdxGenerationId,
            BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum(0));
        ScanKeyInit(&keys[1], kRev3IdxQueryKey,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(
                token.query_key_)));
        ScanKeyInit(&keys[2], kRev3IdxSubKey,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(
                FORMAT6_GENERATION_HEAD_SUB_KEY)));
        ScanKeyInit(&keys[3], kRev3IdxPositionSegment,
            BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(-1));
        ScanKeyInit(&keys[4], kRev3IdxChunkNo,
            BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(0));
        scan = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            return systable_beginscan_ordered(
                relation, primary_index, SnapshotSelf, 5, keys);
        });
        HeapTuple published_tuple =
            bioseq::pg_utils::invoke_pg_as_cpp([&] {
                return systable_getnext_ordered(
                    scan, ForwardScanDirection);
            });
        if (!HeapTupleIsValid(published_tuple)) {
            rev3_storage_error_as_cpp(
                "published Format-6 head is missing");
        }
        Rev3LoadedPgRow published =
            load_rev3_pg_row_or_error(relation, published_tuple);
        if (published.query_key != token.query_key_
            || published.address != IndexChunkAddress{
                .generation_id = 0u,
                .position_segment = -1,
                .sub_key = FORMAT6_GENERATION_HEAD_SUB_KEY,
                .chunk_no = 0u,
            }
            || !std::ranges::equal(
                published.bytes, token.head_bytes_)) {
            published.release();
            rev3_storage_error_as_cpp(
                "published Format-6 head differs from its token");
        }
        published.release();
        HeapTuple published_extra =
            bioseq::pg_utils::invoke_pg_as_cpp([&] {
                return systable_getnext_ordered(
                    scan, ForwardScanDirection);
            });
        bioseq::pg_utils::invoke_pg_as_cpp(
            [&] { systable_endscan_ordered(scan); });
        scan = nullptr;
        if (HeapTupleIsValid(published_extra)) {
            rev3_storage_error_as_cpp(
                "published Format-6 head is not unique");
        }
        Relation finished_index =
            std::exchange(primary_index, nullptr);
        bioseq::pg_utils::invoke_pg_as_cpp(
            [finished_index] {
                index_close(finished_index, AccessShareLock);
            });
        Relation finished_relation =
            std::exchange(relation, nullptr);
        bioseq::pg_utils::invoke_pg_as_cpp(
            [finished_relation] {
                relation_close(finished_relation, RowExclusiveLock);
            });
        ReadyMinimizerFormat6PublishedHeadReceipt receipt{
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

/// One Format-6 read session owns the exact revision-3 heap/primary-index
/// descriptors and borrows one already-registered executor MVCC snapshot.
/// Metadata discovery, source attestation, and every logical stream callback
/// therefore observe one relation incarnation and one snapshot.
struct MinimizerFormat6ExactCatalogStreamReceiptPg {
    MinimizerFormat6StreamKind kind =
        MinimizerFormat6StreamKind::directory;
    std::uint64_t exact_catalog_rows = 0;
    std::uint64_t exact_catalog_bytes = 0;
    std::uint64_t exact_catalog_stream_crc64 = 0;
};

/// Test-only proof returned by the Format-6 parallel heap-scan experiment.
/// The production reader does not consult this object or switch to this path.
struct MinimizerFormat6ParallelScanProbePg {
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
    bool leader_participated = false;
    bool mmap_advice_applied = false;
};

namespace format6_parallel_probe_detail {

inline constexpr std::uint64_t kTocMagic =
    UINT64_C(0x4236534350524f42); // "B6SCPROB"
inline constexpr std::uint64_t kHeaderTocKey =
    UINT64_C(0x4236534300000001);
inline constexpr std::uint64_t kParallelScanTocKey =
    UINT64_C(0x4236534300000002);
inline constexpr std::uint64_t kSeenBitsetTocKey =
    UINT64_C(0x4236534300000003);
inline constexpr std::uint32_t kHeaderVersion = 1u;

struct Stream {
    std::int64_t sub_key = 0;
    std::uint64_t logical_bytes = 0;
    std::uint64_t row_count = 0;
    std::uint64_t global_row_base = 0;
};

/// Pointer-free DSM state. Workers authenticate rows and publish only
/// commutative counters plus one exact expected-row bit per accepted tuple.
struct Header {
    std::uint64_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t requested_workers = 0;
    Oid relation_oid = InvalidOid;
    RelFileLocator relation_locator{};
    std::uint64_t generation_id = 0;
    std::uint64_t query_key = 0;
    std::uint64_t expected_rows = 0;
    std::uint64_t expected_bytes = 0;
    std::uint64_t bitset_word_count = 0;
    std::array<Stream, 3> streams{};
    pg_atomic_uint64 scanned_rows{};
    pg_atomic_uint64 scanned_bytes{};
    pg_atomic_uint64 duplicate_rows{};
    pg_atomic_uint64 corrupt_rows{};
    pg_atomic_uint64 checksum_digest{};
    pg_atomic_uint64 receipt_digest{};
};

static_assert(std::is_trivially_destructible_v<Header>);
static_assert(std::is_standard_layout_v<Header>);

inline constexpr std::uint64_t kImageMagic =
    UINT64_C(0x42364636494d4147); // "B6F6IMAG"
inline constexpr std::uint32_t kImageVersion = 1u;
inline constexpr std::uint64_t kImageSectionAlignment = 4096u;

struct ImageStream {
    std::int64_t sub_key = 0;
    std::uint64_t logical_bytes = 0;
    std::uint64_t row_count = 0;
    std::uint64_t global_row_base = 0;
    std::uint64_t payload_offset = 0;
};

/// Offset-only native POC image. Production adoption would persist this as a
/// generation object via temp+fsync+durable rename; this probe writes a
/// PostgreSQL-owned temporary file only.
struct ImageHeader {
    std::uint64_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t header_bytes = 0;
    std::uint64_t generation_id = 0;
    std::uint64_t query_key = 0;
    std::uint64_t expected_rows = 0;
    std::uint64_t expected_bytes = 0;
    std::uint64_t receipts_offset = 0;
    std::uint64_t payload_offset = 0;
    std::uint64_t image_bytes = 0;
    std::array<ImageStream, 3> streams{};
    std::uint64_t header_crc64 = 0;
};

static_assert(std::is_trivially_copyable_v<ImageHeader>);
static_assert(std::is_standard_layout_v<ImageHeader>);

struct ImageWriteTarget {
    File file = -1;
    const ImageHeader* header = nullptr;
};

struct ImageValidation {
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
            - (kImageSectionAlignment - 1u)) {
        rev3_storage_error_as_cpp(
            "Format-6 mmap image offset overflows");
    }
    return (value + kImageSectionAlignment - 1u)
        / kImageSectionAlignment * kImageSectionAlignment;
}

[[nodiscard]] inline std::uint64_t image_header_crc(
        const ImageHeader& header) noexcept {
    ImageHeader copy = header;
    copy.header_crc64 = 0;
    Crc64Ecma182 crc{};
    crc.update(std::as_bytes(
        std::span{&copy, std::size_t{1}}));
    return crc.checksum();
}

[[noreturn]] inline void corrupt_row_pg_leaf(
        Header* header,
        const char* detail) {
    if (header != nullptr) {
        (void)pg_atomic_fetch_add_u64(
            &header->corrupt_rows, 1);
    }
    ereport(ERROR,
        (errcode(ERRCODE_DATA_CORRUPTED),
         errmsg("bioseq Format-6 parallel scan probe rejected a row"),
         errdetail_internal("%s", detail)));
    pg_unreachable();
}

[[noreturn]] inline void duplicate_row_pg_leaf(
        Header* header,
        std::uint64_t global_row) {
    (void)pg_atomic_fetch_add_u64(
        &header->duplicate_rows, 1);
    ereport(ERROR,
        (errcode(ERRCODE_DATA_CORRUPTED),
         errmsg("bioseq Format-6 parallel scan probe found a duplicate row"),
         errdetail_internal(
            "global_row=%llu",
            static_cast<unsigned long long>(global_row))));
    pg_unreachable();
}

[[nodiscard]] inline int stream_index_for_sub_key(
        const Header& header,
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
        Header* header,
        pg_atomic_uint64* seen_words) {
    if (relation == nullptr || scan == nullptr || header == nullptr
        || seen_words == nullptr
        || header->magic != kTocMagic
        || header->version != kHeaderVersion
        || RelationGetRelid(relation) != header->relation_oid
        || !RelFileLocatorEquals(
            relation->rd_locator, header->relation_locator)
        || RelationGetDescr(relation) == nullptr
        || RelationGetDescr(relation)->natts
            != static_cast<int>(
                kRev3IndexAttributeDescriptors.size())) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq Format-6 parallel scan probe binding changed")));
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
        // Old generations and metadata rows are deliberately outside this
        // probe's corruption authority.
        if (!nulls[kRev3AttGenerationId - 1]
            && !nulls[kRev3AttQueryKey - 1]
            && !nulls[kRev3AttPositionSegment - 1]
            && !nulls[kRev3AttSubKey - 1]
            && DatumGetInt64(values[kRev3AttGenerationId - 1])
                == static_cast<std::int64_t>(
                    header->generation_id)
            && DatumGetInt64(values[kRev3AttQueryKey - 1])
                == static_cast<std::int64_t>(header->query_key)
            && DatumGetInt32(
                    values[kRev3AttPositionSegment - 1]) == 0) {
            const std::int64_t sub_key =
                DatumGetInt64(values[kRev3AttSubKey - 1]);
            const int stream_index =
                stream_index_for_sub_key(*header, sub_key);
            if (stream_index >= 0) {
                Rev3DeformedPgRow deformed{};
                for (std::size_t index = 0;
                     index < deformed.values.size(); ++index) {
                    deformed.values[index] = values[index];
                    deformed.nulls[index] = nulls[index];
                }
                const Rev3ProjectedPgRow projected =
                    project_rev3_deformed_pg_row(deformed);
                if (projected.error != nullptr) {
                    corrupt_row_pg_leaf(header, projected.error);
                }
                const Stream& stream =
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
                            / kMinimizerFormat6RowPayloadBytes) {
                    corrupt_row_pg_leaf(
                        header, "row identity or ordinal disagrees");
                }
                const std::uint64_t row_offset =
                    row_ordinal * kMinimizerFormat6RowPayloadBytes;
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
                            kMinimizerFormat6RowPayloadBytes));
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
        Header* header,
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
        Header* header,
        pg_atomic_uint64* seen_words) {
    TableScanDesc scan =
        table_beginscan_parallel(relation, parallel_scan);
    scan_table_desc_pg_leaf(
        relation, scan, header, seen_words);
    table_endscan(scan);
}

inline void initialize_header(
        Header* header,
        Relation relation,
        const MinimizerFormat6Manifest& manifest,
        std::uint64_t query_key,
        std::uint32_t requested_workers) {
    if (header == nullptr || relation == nullptr
        || manifest.generation_id == 0u
        || !bioseq::indexing::sql_bigint_can_represent(
            manifest.generation_id)
        || !bioseq::indexing::sql_bigint_can_represent(query_key)) {
        rev3_storage_error_as_cpp(
            "Format-6 parallel scan probe header is invalid");
    }
    std::memset(header, 0, sizeof(*header));
    header->magic = kTocMagic;
    header->version = kHeaderVersion;
    header->requested_workers = requested_workers;
    header->relation_oid = RelationGetRelid(relation);
    header->relation_locator = relation->rd_locator;
    header->generation_id = manifest.generation_id;
    header->query_key = query_key;

    std::uint64_t row_base = 0;
    std::uint64_t total_bytes = 0;
    for (std::size_t index = 0;
         index < header->streams.size(); ++index) {
        const MinimizerFormat6StreamKind expected_kind =
            format6_pg_detail::stream_kind(index);
        const auto& descriptor = manifest.streams[index];
        const std::uint64_t expected_rows =
            format6_pg_detail::ceil_rows(
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
            rev3_storage_error_as_cpp(
                "Format-6 parallel scan probe stream geometry is invalid");
        }
        header->streams[index] = Stream{
            .sub_key =
                format6_pg_detail::stream_sub_key(expected_kind),
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
        rev3_storage_error_as_cpp(
            "Format-6 parallel scan probe bitset geometry is invalid");
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
        rev3_storage_error_as_cpp(
            "Format-6 parallel scan probe bitset is invalid");
    }
    for (std::uint64_t index = 0; index < word_count; ++index) {
        pg_atomic_init_u64(&words[index], 0);
    }
}

[[nodiscard]] inline MinimizerFormat6ParallelScanProbePg
finalize_result(
        Header& header,
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
        rev3_storage_error_as_cpp(fmt::format(
            "Format-6 parallel scan probe receipt equation failed: "
            "expected_rows={} seen_rows={} scanned_rows={} "
            "expected_bytes={} scanned_bytes={} duplicates={} "
            "missing={} corrupt={}",
            header.expected_rows, seen_rows, scanned_rows,
            header.expected_bytes, scanned_bytes, duplicates,
            missing, corrupt));
    }
    return MinimizerFormat6ParallelScanProbePg{
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
        .leader_participated = true,
    };
}

[[nodiscard]] inline MinimizerFormat6ParallelScanProbePg
run_probe(
        Relation relation,
        Snapshot snapshot,
        const MinimizerFormat6Manifest& manifest,
        std::uint64_t query_key,
        std::uint32_t requested_workers) {
    if (relation == nullptr || snapshot == nullptr
        || !IsMVCCSnapshot(snapshot)
        || snapshot->regd_count == 0) {
        rev3_storage_error_as_cpp(
            "Format-6 parallel scan probe requires a registered MVCC snapshot");
    }

    if (requested_workers == 0u) {
        Header header{};
        initialize_header(
            &header, relation, manifest, query_key, 0u);
        const Size bitset_bytes = static_cast<Size>(
            header.bitset_word_count
            * sizeof(pg_atomic_uint64));
        auto* seen_words =
            static_cast<pg_atomic_uint64*>(
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [bitset_bytes] {
                        return palloc0(bitset_bytes);
                    }));
        initialize_seen_words(
            seen_words, header.bitset_word_count);
        instr_time begin{};
        instr_time end{};
        INSTR_TIME_SET_CURRENT(begin);
        bioseq::pg_utils::invoke_pg_as_cpp(
            [&] {
                serial_scan_pg_leaf(
                    relation, snapshot, &header, seen_words);
            });
        INSTR_TIME_SET_CURRENT(end);
        INSTR_TIME_SUBTRACT(end, begin);
        MinimizerFormat6ParallelScanProbePg result =
            finalize_result(
                header, seen_words, 0u,
                INSTR_TIME_GET_NANOSEC(end));
        bioseq::pg_utils::invoke_pg_as_cpp(
            [seen_words] { pfree(seen_words); });
        return result;
    }

    if (IsParallelWorker() || IsInParallelMode()
        || ParallelContextActive()) {
        rev3_storage_error_as_cpp(
            "Format-6 parallel scan probe cannot nest a parallel context");
    }
    const Size parallel_scan_bytes =
        bioseq::pg_utils::invoke_pg_as_cpp(
            [&] {
                return table_parallelscan_estimate(
                    relation, snapshot);
            });
    if (parallel_scan_bytes == 0u) {
        rev3_storage_error_as_cpp(
            "Format-6 parallel scan probe received an empty scan descriptor");
    }

    ParallelContext* context = nullptr;
    bool entered_parallel_mode = false;
    instr_time begin{};
    instr_time end{};
    INSTR_TIME_SET_CURRENT(begin);
    try {
        EnterParallelMode();
        entered_parallel_mode = true;
        context = bioseq::pg_utils::invoke_pg_as_cpp(
            [&] {
                return CreateParallelContext(
                    "bioseq",
                    "bioseq_format6_parallel_scan_probe_worker_main",
                    static_cast<int>(requested_workers));
            });
        if (context == nullptr) {
            rev3_storage_error_as_cpp(
                "Format-6 parallel scan probe could not create a parallel context");
        }
        bioseq::pg_utils::invoke_pg_as_cpp(
            [&] {
                shm_toc_estimate_chunk(
                    &context->estimator, sizeof(Header));
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
                             errmsg("bioseq Format-6 parallel scan row count overflows")));
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
                         errmsg("bioseq Format-6 parallel scan bitset is too large")));
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
            rev3_storage_error_as_cpp(
                "Format-6 parallel scan probe has no DSM table of contents");
        }

        auto* header = static_cast<Header*>(
            bioseq::pg_utils::invoke_pg_as_cpp(
                [&] {
                    return shm_toc_allocate(
                        context->toc, sizeof(Header));
                }));
        initialize_header(
            header, relation, manifest, query_key,
            requested_workers);
        const Size bitset_bytes = static_cast<Size>(
            header->bitset_word_count
            * sizeof(pg_atomic_uint64));
        auto* parallel_scan =
            static_cast<ParallelTableScanDesc>(
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [&] {
                        return shm_toc_allocate(
                            context->toc,
                            parallel_scan_bytes);
                    }));
        auto* seen_words =
            static_cast<pg_atomic_uint64*>(
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [&] {
                        return shm_toc_allocate(
                            context->toc, bitset_bytes);
                    }));
        initialize_seen_words(
            seen_words, header->bitset_word_count);
        bioseq::pg_utils::invoke_pg_as_cpp(
            [&] {
                table_parallelscan_initialize(
                    relation, parallel_scan, snapshot);
                shm_toc_insert(
                    context->toc, kHeaderTocKey, header);
                shm_toc_insert(
                    context->toc, kParallelScanTocKey,
                    parallel_scan);
                shm_toc_insert(
                    context->toc, kSeenBitsetTocKey,
                    seen_words);
                LaunchParallelWorkers(context);
                WaitForParallelWorkersToAttach(context);
            });
        const std::uint32_t launched_workers =
            static_cast<std::uint32_t>(
                std::max(context->nworkers_launched, 0));

        // The leader is an equal consumer of the shared block allocator.
        bioseq::pg_utils::invoke_pg_as_cpp(
            [&] {
                parallel_scan_pg_leaf(
                    relation, parallel_scan,
                    header, seen_words);
                WaitForParallelWorkersToFinish(context);
            });
        INSTR_TIME_SET_CURRENT(end);
        INSTR_TIME_SUBTRACT(end, begin);
        MinimizerFormat6ParallelScanProbePg result =
            finalize_result(
                *header, seen_words, launched_workers,
                INSTR_TIME_GET_NANOSEC(end));
        bioseq::pg_utils::invoke_pg_as_cpp(
            [&] { DestroyParallelContext(context); });
        context = nullptr;
        ExitParallelMode();
        entered_parallel_mode = false;
        return result;
    } catch (...) {
        if (context != nullptr) {
            bioseq::pg_utils::invoke_pg_as_cpp(
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

} // namespace format6_parallel_probe_detail

class IndexStorageMinimizerFormat6ReadSessionPg {
private:
    enum class State : std::uint8_t {
        opened,
        head_absent,
        metadata_ready,
        source_attested,
        finished,
        failed,
        moved,
    };

    std::string table_name_;
    std::string index_name_;
    Relation heap_relation_ = nullptr;
    Relation primary_index_relation_ = nullptr;
    SysScanDesc active_scan_ = nullptr;
    std::array<TupleTableSlot*, kRev3FullGenerationPinnedBatchRows>
        row_batch_slots_{};
    std::size_t row_batch_slot_count_ = 0u;
    bool cached_boundary_row_valid_ = false;
    MinimizerFormat6StreamKind cached_boundary_row_kind_ =
        MinimizerFormat6StreamKind::directory;
    std::uint64_t cached_boundary_row_ordinal_ = 0u;
    std::size_t cached_boundary_slot_ordinal_ =
        kRev3FullGenerationPinnedBatchRows;
    Rev3ProjectedPgRow cached_boundary_row_{};
    Snapshot snapshot_ = nullptr;
    std::uint64_t query_key_ = 0u;
    Oid expected_tablespace_oid_ = InvalidOid;
    State state_ = State::opened;
    EncodedMinimizerFormat6Head encoded_head_{};
    EncodedMinimizerFormat6Manifest encoded_manifest_{};
    std::optional<MinimizerFormat6Head> head_{};
    std::optional<MinimizerFormat6Manifest> manifest_{};
    std::optional<MinimizerFormat6ExpectedGeneration>
        expected_generation_{};
    std::array<std::byte, kMinimizerFormat6MaximumControlBlockBytes>
        control_wave_scratch_{};
    MinimizerGenerationReadStats stats_{};

    [[noreturn]] void fail_(std::string_view message) {
        state_ = State::failed;
        rev3_storage_error_as_cpp(message);
    }

    void create_row_batch_slots_checked_() {
        bioseq::pg_utils::invoke_pg_as_cpp(
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
            MinimizerFormat6StreamKind::directory;
        cached_boundary_row_ordinal_ = 0u;
        cached_boundary_slot_ordinal_ =
            row_batch_slots_.size();
        cached_boundary_row_ = {};
    }

    void clear_row_batch_slots_checked_() {
        if (row_batch_slot_count_ == 0u) return;
        invalidate_cached_boundary_row_();
        bioseq::pg_utils::invoke_pg_as_cpp(
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
            fail_("Format-6 boundary-row cache slot is invalid");
        }
        bioseq::pg_utils::invoke_pg_as_cpp(
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
        bioseq::pg_utils::invoke_pg_as_cpp(
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
            bioseq::pg_utils::invoke_pg_as_cpp(
                [&] { systable_endscan_ordered(active_scan_); });
            active_scan_ = nullptr;
        }
        if (primary_index_relation_ != nullptr) {
            Relation finished =
                std::exchange(primary_index_relation_, nullptr);
            bioseq::pg_utils::invoke_pg_as_cpp(
                [finished] {
                    index_close(finished, AccessShareLock);
                });
            stats_.cold.add(stats_.cold.lock_releases, 1u);
        }
        if (heap_relation_ != nullptr) {
            Relation finished =
                std::exchange(heap_relation_, nullptr);
            bioseq::pg_utils::invoke_pg_as_cpp(
                [finished] {
                    relation_close(finished, AccessShareLock);
                });
            stats_.cold.add(stats_.cold.lock_releases, 1u);
        }
        snapshot_ = nullptr;
    }

    [[nodiscard]] const
    bioseq::indexing::MinimizerFormat6StreamDescriptor&
    descriptor_(MinimizerFormat6StreamKind kind) const {
        if (!manifest_ || !format6_pg_detail::valid_stream_kind(kind)) {
            rev3_storage_error_as_cpp(
                "Format-6 stream descriptor requested before metadata");
        }
        const std::size_t index =
            format6_pg_detail::stream_index(kind);
        const auto& descriptor = manifest_->streams[index];
        if (descriptor.kind != kind) {
            rev3_storage_error_as_cpp(
                "Format-6 stream descriptor ordering changed");
        }
        return descriptor;
    }

    void record_stream_row_(
            MinimizerFormat6StreamKind kind,
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
        if (kind == MinimizerFormat6StreamKind::directory) {
            ++stats_.directory_chunks_read;
            stats_.directory_bytes_read += bytes;
            ++stats_.format6.directory_row_crc_receipts;
        } else if (kind == MinimizerFormat6StreamKind::controls) {
            ++stats_.controls.physical_heap_rows_read;
            stats_.controls.physical_bytes_read += bytes;
            ++stats_.controls.read.heap_rows_read;
            ++stats_.controls.read.delivered_chunks;
            stats_.controls.read.delivered_bytes += bytes;
            ++stats_.format6.control_row_crc_receipts;
        } else {
            ++stats_.positions.physical_heap_rows_read;
            stats_.positions.physical_bytes_read += bytes;
            ++stats_.positions.read.heap_rows_read;
            ++stats_.positions.read.delivered_chunks;
            stats_.positions.read.delivered_bytes += bytes;
            ++stats_.format6.posting_row_crc_receipts;
        }
    }

    template <class Consumer>
    [[nodiscard]] bool scan_stream_rows_(
            MinimizerFormat6StreamKind kind,
            std::uint64_t first_row,
            std::uint64_t last_row,
            Consumer&& consume) {
        const auto& descriptor = descriptor_(kind);
        if (state_ != State::source_attested
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
        ScanKeyInit(&keys[0], kRev3IdxGenerationId,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(
                manifest_->generation_id)));
        ScanKeyInit(&keys[1], kRev3IdxQueryKey,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(query_key_)));
        ScanKeyInit(&keys[2], kRev3IdxSubKey,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(
                format6_pg_detail::stream_sub_key(kind))));
        ScanKeyInit(&keys[3], kRev3IdxPositionSegment,
            BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(0));
        ScanKeyInit(&keys[4], kRev3IdxChunkNo,
            BTGreaterEqualStrategyNumber, F_INT4GE,
            Int32GetDatum(static_cast<std::int32_t>(expected_row)));
        ScanKeyInit(&keys[5], kRev3IdxChunkNo,
            BTLessEqualStrategyNumber, F_INT4LE,
            Int32GetDatum(static_cast<std::int32_t>(last_row)));
        create_row_batch_slots_checked_();
        active_scan_ = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            return systable_beginscan_ordered(
                heap_relation_, primary_index_relation_,
                snapshot_, 6, keys);
        });
        ++stats_.index_scans;
        ++stats_.range_scan_calls;
        stats_.cold.add(stats_.cold.payload_range_scans, 1u);
        Rev3PinnedProjectedPgRowBatch batch{};
        Rev3ValidatedPgRowReceiptBatch validated_receipts{};
        std::size_t batch_ordinal = 0u;
        while (accepted && expected_row <= last_row) {
            if (batch_ordinal == batch.size) {
                if (batch.eof) break;
                clear_rev3_validated_pg_row_receipt_batch(
                    validated_receipts);
                batch =
                    next_rev3_pinned_projected_pg_row_batch_or_error(
                        heap_relation_, active_scan_,
                        row_batch_slots_);
                batch_ordinal = 0u;
                if (batch.size == 0u) {
                    if (batch.eof) break;
                    fail_("Format-6 batched stream scan made no progress");
                }
                authenticate_rev3_projected_pg_row_batch(
                    batch, validated_receipts);
            }
            const Rev3PinnedProjectedPgRow& pinned =
                batch.rows[batch_ordinal];
            Rev3LoadedPgRow row =
                load_rev3_authenticated_projected_pg_row_or_error(
                    pinned.projected,
                    validated_receipts[batch_ordinal]);
            ++batch_ordinal;
            if (row.query_key != query_key_
                || row.address != IndexChunkAddress{
                    .generation_id = manifest_->generation_id,
                    .position_segment = 0,
                    .sub_key =
                        format6_pg_detail::stream_sub_key(kind),
                    .chunk_no =
                        static_cast<std::uint32_t>(expected_row),
                }) {
                row.release();
                fail_("Format-6 stream row identity/order disagrees");
            }
            const std::uint64_t row_offset =
                expected_row * kMinimizerFormat6RowPayloadBytes;
            const std::uint64_t remaining =
                descriptor.logical_bytes - row_offset;
            const std::size_t expected_bytes =
                static_cast<std::size_t>(std::min<std::uint64_t>(
                    remaining, kMinimizerFormat6RowPayloadBytes));
            if (row.bytes.size() != expected_bytes
                || (expected_row + 1u < descriptor.row_count
                    && row.bytes.size()
                        != kMinimizerFormat6RowPayloadBytes)) {
                row.release();
                fail_("Format-6 stream physical row length disagrees");
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
        clear_rev3_validated_pg_row_receipt_batch(
            validated_receipts);
        if (cached_boundary_row_valid_) {
            clear_row_batch_slots_except_checked_(
                cached_boundary_slot_ordinal_);
        } else {
            clear_row_batch_slots_checked_();
        }
        bioseq::pg_utils::invoke_pg_as_cpp(
            [&] { systable_endscan_ordered(active_scan_); });
        active_scan_ = nullptr;
        return accepted && expected_row == last_row + 1u;
    }

    [[nodiscard]] bool read_logical_bytes_(
            MinimizerFormat6StreamKind kind,
            std::uint64_t offset,
            std::span<std::byte> output) {
        if (state_ != State::source_attested
            || !format6_pg_detail::valid_stream_kind(kind)) {
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
            offset / kMinimizerFormat6RowPayloadBytes;
        const std::uint64_t last_row =
            (end - 1u) / kMinimizerFormat6RowPayloadBytes;
        std::size_t copied = 0u;
        const bool read = scan_stream_rows_(
            kind, first_row, last_row,
            [&](std::uint64_t row_ordinal,
                    std::span<const std::byte> row) {
                const std::uint64_t row_begin =
                    row_ordinal * kMinimizerFormat6RowPayloadBytes;
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
            std::span<const MinimizerFormat6AuthorizedControlRange>
                ranges,
            void* consumer_context,
            MinimizerFormat6ConsumeControlBlockFn consume) {
        if (state_ != State::source_attested
            || consume == nullptr
            || ranges.empty()
            || ranges.size()
                > bioseq::indexing::
                    kMinimizerFormat6ControlReadWaveBlocks
            || ranges.size()
                > static_cast<std::size_t>(
                    (std::numeric_limits<std::uint32_t>::max)())) {
            return false;
        }
        const auto& descriptor =
            descriptor_(MinimizerFormat6StreamKind::controls);
        std::uint64_t previous_end = 0u;
        std::uint64_t previous_block = 0u;
        bool have_previous = false;
        for (const auto& range : ranges) {
            const std::uint64_t end =
                range.control_byte_offset + range.byte_count;
            if (range.byte_count == 0u
                || range.byte_count
                    > kMinimizerFormat6MaximumControlBlockBytes
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
                / kMinimizerFormat6RowPayloadBytes;
            std::size_t group_end = group_begin + 1u;
            while (group_end < ranges.size()) {
                const std::uint64_t next_first_row =
                    ranges[group_end].control_byte_offset
                    / kMinimizerFormat6RowPayloadBytes;
                if (next_first_row > group_last_row + 1u) break;
                const std::uint64_t next_end =
                    ranges[group_end].control_byte_offset
                    + ranges[group_end].byte_count;
                group_last_row = std::max(
                    group_last_row,
                    (next_end - 1u)
                        / kMinimizerFormat6RowPayloadBytes);
                ++group_end;
            }
            const std::uint64_t group_first_row =
                ranges[group_begin].control_byte_offset
                / kMinimizerFormat6RowPayloadBytes;
            std::size_t cursor = group_begin;
            const bool delivered = scan_stream_rows_(
                MinimizerFormat6StreamKind::controls,
                group_first_row,
                group_last_row,
                [&](std::uint64_t row_ordinal,
                        std::span<const std::byte> row) {
                    const std::uint64_t row_begin =
                        row_ordinal
                        * kMinimizerFormat6RowPayloadBytes;
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
            std::span<const MinimizerFormat6AuthorizedPostingRange>
                ranges,
            void* consumer_context,
            MinimizerFormat6ConsumePostingBytesFn consume) {
        if (state_ != State::source_attested
            || consume == nullptr || ranges.empty()
            || ranges.size()
                > static_cast<std::size_t>(
                    (std::numeric_limits<std::uint32_t>::max)())) {
            return false;
        }
        const auto& descriptor =
            descriptor_(MinimizerFormat6StreamKind::postings);
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
                / kMinimizerFormat6RowPayloadBytes;
            std::size_t group_end = group_begin + 1u;
            while (group_end < ranges.size()) {
                const std::uint64_t next_first_row =
                    ranges[group_end].posting_byte_offset
                    / kMinimizerFormat6RowPayloadBytes;
                if (next_first_row > group_last_row + 1u) break;
                const std::uint64_t next_end =
                    ranges[group_end].posting_byte_offset
                    + ranges[group_end].byte_count;
                group_last_row = std::max(
                    group_last_row,
                    (next_end - 1u)
                        / kMinimizerFormat6RowPayloadBytes);
                ++group_end;
            }
            const std::uint64_t group_first_row =
                ranges[group_begin].posting_byte_offset
                / kMinimizerFormat6RowPayloadBytes;
            std::size_t cursor = group_begin;
            const bool delivered = scan_stream_rows_(
                MinimizerFormat6StreamKind::postings,
                group_first_row,
                group_last_row,
                [&](std::uint64_t row_ordinal,
                        std::span<const std::byte> row) {
                    const std::uint64_t row_begin =
                        row_ordinal
                        * kMinimizerFormat6RowPayloadBytes;
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
                        if (stats_.format6.posting_ranges_delivered
                                == (std::numeric_limits<
                                    std::uint64_t>::max)()
                            || range.byte_count
                                > (std::numeric_limits<
                                    std::uint64_t>::max)()
                                    - stats_.format6
                                        .authorized_posting_bytes) {
                            return false;
                        }
                        ++stats_.format6.posting_ranges_delivered;
                        stats_.format6.authorized_posting_bytes +=
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
            MinimizerFormat6StreamKind kind,
            std::uint64_t offset,
            std::span<std::byte> output) {
        return static_cast<IndexStorageMinimizerFormat6ReadSessionPg*>(
            context)->read_logical_bytes_(kind, offset, output);
    }

    [[nodiscard]] static std::uint64_t logical_size_callback_(
            void* context,
            MinimizerFormat6StreamKind kind) {
        auto& self =
            *static_cast<IndexStorageMinimizerFormat6ReadSessionPg*>(
                context);
        if (self.state_ != State::source_attested
            || !format6_pg_detail::valid_stream_kind(kind)) {
            return 0u;
        }
        return self.descriptor_(kind).logical_bytes;
    }

    [[nodiscard]] static bool control_wave_callback_(
            void* context,
            std::span<const MinimizerFormat6AuthorizedControlRange>
                ranges,
            void* consumer_context,
            MinimizerFormat6ConsumeControlBlockFn consume) {
        return static_cast<IndexStorageMinimizerFormat6ReadSessionPg*>(
            context)->read_control_wave_(
                ranges, consumer_context, consume);
    }

    [[nodiscard]] static bool posting_wave_callback_(
            void* context,
            std::span<const MinimizerFormat6AuthorizedPostingRange>
                ranges,
            void* consumer_context,
            MinimizerFormat6ConsumePostingBytesFn consume) {
        return static_cast<IndexStorageMinimizerFormat6ReadSessionPg*>(
            context)->read_posting_wave_(
                ranges, consumer_context, consume);
    }

public:
    IndexStorageMinimizerFormat6ReadSessionPg() = delete;
    IndexStorageMinimizerFormat6ReadSessionPg(
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
                fail_("Format-6 read binding is invalid");
            }
            const Oid relation_oid =
                bioseq::pg_utils::invoke_pg_as_cpp([&] {
                    return get_relname_relid(
                        table_name_.c_str(), bioseq_internal_ns());
                });
            if (!OidIsValid(relation_oid)) {
                throw MinimizerGenerationRelationOpenFailurePg{
                    .inspection = {
                        .kind = MinimizerGenerationRelationKind::missing,
                        .revision3_error = Rev3IndexSchemaError::none,
                    },
                };
            }
            heap_relation_ = bioseq::pg_utils::invoke_pg_as_cpp([&] {
                return try_safe_relation_open(
                    relation_oid, AccessShareLock);
            });
            if (heap_relation_ == nullptr) {
                throw MinimizerGenerationRelationOpenFailurePg{
                    .inspection = {
                        .kind = MinimizerGenerationRelationKind::missing,
                        .revision3_error = Rev3IndexSchemaError::none,
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
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [&] {
                        return RelationGetPrimaryKeyIndex(
                            heap_relation_);
                    });
            if (OidIsValid(primary_oid)) {
                primary_index_relation_ =
                    bioseq::pg_utils::invoke_pg_as_cpp(
                        [primary_oid] {
                            return index_open(
                                primary_oid, AccessShareLock);
                        });
                stats_.cold.add(
                    stats_.cold.sidecar_index_opens, 1u);
                stats_.cold.add(stats_.cold.lock_acquires, 1u);
            }
            const Rev3IndexSchemaInspection schema = exact_identity
                ? bioseq::pg_utils::invoke_pg_as_cpp([&] {
                    return inspect_bound_rev3_index_schema(
                        heap_relation_,
                        primary_index_relation_,
                        Rev3IndexSchemaExpectation{
                            .namespace_oid = bioseq_internal_ns(),
                            .relation_name = table_name_,
                            .tablespace_oid =
                                expected_tablespace_oid_,
                        });
                })
                : Rev3IndexSchemaInspection{
                    .error = Rev3IndexSchemaError::not_heap,
                };
            stats_.cold.add(
                stats_.cold.sidecar_schema_validations, 1u);
            if (!schema) {
                const bool legacy_exact = exact_identity
                    && primary_index_relation_ != nullptr
                    && bioseq::pg_utils::invoke_pg_as_cpp([&] {
                        return inspect_bound_exact_legacy_minimizer_index_schema(
                            heap_relation_,
                            primary_index_relation_,
                            index_name_);
                    });
                if (primary_index_relation_ != nullptr) {
                    Relation finished = std::exchange(
                        primary_index_relation_, nullptr);
                    bioseq::pg_utils::invoke_pg_as_cpp(
                        [finished] {
                            index_close(
                                finished, AccessShareLock);
                        });
                }
                Relation finished =
                    std::exchange(heap_relation_, nullptr);
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [finished] {
                        relation_close(
                            finished, AccessShareLock);
                    });
                snapshot_ = nullptr;
                throw MinimizerGenerationRelationOpenFailurePg{
                    .inspection = {
                        .kind = legacy_exact
                            ? MinimizerGenerationRelationKind::
                                legacy_exact
                            : MinimizerGenerationRelationKind::
                                malformed,
                        .revision3_error = schema.error,
                    },
                };
            }
            (void)verify_pg_inline_payload_layout(heap_relation_);
            stats_.relation_opens = 1u;
        } catch (const MinimizerGenerationRelationOpenFailurePg&) {
            state_ = State::failed;
            abandon_noexcept_();
            throw;
        } catch (...) {
            state_ = State::failed;
            abandon_noexcept_();
            throw;
        }
    }
    IndexStorageMinimizerFormat6ReadSessionPg(
        const IndexStorageMinimizerFormat6ReadSessionPg&) = delete;
    IndexStorageMinimizerFormat6ReadSessionPg& operator=(
        const IndexStorageMinimizerFormat6ReadSessionPg&) = delete;
    IndexStorageMinimizerFormat6ReadSessionPg(
            IndexStorageMinimizerFormat6ReadSessionPg&& other) noexcept
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
        other.state_ = State::moved;
    }
    IndexStorageMinimizerFormat6ReadSessionPg& operator=(
        IndexStorageMinimizerFormat6ReadSessionPg&&) = delete;
    ~IndexStorageMinimizerFormat6ReadSessionPg() {
        abandon_noexcept_();
    }

    /// Returns false only for an authenticated absence of the distinct
    /// Format-6 head sentinel.  A present malformed head or manifest is a
    /// hard error and never enters the Format-5 fallback.
    [[nodiscard]] bool read_active_head_manifest_once() {
        if (state_ != State::opened || active_scan_ != nullptr) {
            fail_("Format-6 metadata was read more than once");
        }
        ScanKeyData head_keys[5];
        ScanKeyInit(&head_keys[0], kRev3IdxGenerationId,
            BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum(0));
        ScanKeyInit(&head_keys[1], kRev3IdxQueryKey,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(query_key_)));
        ScanKeyInit(&head_keys[2], kRev3IdxSubKey,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(
                FORMAT6_GENERATION_HEAD_SUB_KEY)));
        ScanKeyInit(&head_keys[3], kRev3IdxPositionSegment,
            BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(-1));
        ScanKeyInit(&head_keys[4], kRev3IdxChunkNo,
            BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(0));
        active_scan_ = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            return systable_beginscan_ordered(
                heap_relation_, primary_index_relation_,
                snapshot_, 5, head_keys);
        });
        ++stats_.index_scans;
        ++stats_.head_scans;
        HeapTuple tuple = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            return systable_getnext_ordered(
                active_scan_, ForwardScanDirection);
        });
        if (!HeapTupleIsValid(tuple)) {
            bioseq::pg_utils::invoke_pg_as_cpp(
                [&] { systable_endscan_ordered(active_scan_); });
            active_scan_ = nullptr;
            state_ = State::head_absent;
            return false;
        }
        Rev3LoadedPgRow head_row =
            load_rev3_pg_row_or_error(heap_relation_, tuple);
        if (head_row.query_key != query_key_
            || head_row.address != IndexChunkAddress{
                .generation_id = 0u,
                .position_segment = -1,
                .sub_key = FORMAT6_GENERATION_HEAD_SUB_KEY,
                .chunk_no = 0u,
            }
            || head_row.bytes.size()
                != kMinimizerFormat6HeadBytes) {
            head_row.release();
            fail_("Format-6 head row shape disagrees");
        }
        std::copy(
            head_row.bytes.begin(), head_row.bytes.end(),
            encoded_head_.begin());
        head_row.release();
        HeapTuple extra = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            return systable_getnext_ordered(
                active_scan_, ForwardScanDirection);
        });
        bioseq::pg_utils::invoke_pg_as_cpp(
            [&] { systable_endscan_ordered(active_scan_); });
        active_scan_ = nullptr;
        if (HeapTupleIsValid(extra)) {
            fail_("Format-6 head primary key returned duplicates");
        }
        auto decoded_head =
            bioseq::indexing::decode_minimizer_format6_head(
                encoded_head_);
        if (!decoded_head
            || decoded_head->query_key != query_key_
            || decoded_head->generation_id == 0u
            || !bioseq::indexing::sql_bigint_can_represent(
                decoded_head->generation_id)) {
            fail_("Format-6 head fixed wire is invalid");
        }
        head_.emplace(*decoded_head);
        if (head_->query_key != query_key_
            || head_->generation_id == 0u
            || !bioseq::indexing::sql_bigint_can_represent(
                head_->generation_id)) {
            fail_("Format-6 head fixed wire is invalid");
        }
        stats_.head_rows_read = 1u;
        stats_.head_bytes_read = kMinimizerFormat6HeadBytes;
        stats_.cold.add(
            stats_.cold.row_crc_bytes,
            kMinimizerFormat6HeadBytes);

        ScanKeyData manifest_keys[5];
        ScanKeyInit(&manifest_keys[0], kRev3IdxGenerationId,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(
                head_->generation_id)));
        ScanKeyInit(&manifest_keys[1], kRev3IdxQueryKey,
            BTEqualStrategyNumber, F_INT8EQ,
            Int64GetDatum(static_cast<std::int64_t>(query_key_)));
        ScanKeyInit(&manifest_keys[2], kRev3IdxSubKey,
            BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum(0));
        ScanKeyInit(&manifest_keys[3], kRev3IdxPositionSegment,
            BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(0));
        ScanKeyInit(&manifest_keys[4], kRev3IdxChunkNo,
            BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(0));
        active_scan_ = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            return systable_beginscan_ordered(
                heap_relation_, primary_index_relation_,
                snapshot_, 5, manifest_keys);
        });
        ++stats_.index_scans;
        ++stats_.manifest_directory_scans;
        tuple = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            return systable_getnext_ordered(
                active_scan_, ForwardScanDirection);
        });
        if (!HeapTupleIsValid(tuple)) {
            fail_("Format-6 manifest row is missing");
        }
        Rev3LoadedPgRow manifest_row =
            load_rev3_pg_row_or_error(heap_relation_, tuple);
        if (manifest_row.query_key != query_key_
            || manifest_row.address != IndexChunkAddress{
                .generation_id = head_->generation_id,
                .position_segment = 0,
                .sub_key = 0,
                .chunk_no = 0u,
            }
            || manifest_row.bytes.size()
                != kMinimizerFormat6ManifestBytes) {
            manifest_row.release();
            fail_("Format-6 manifest row shape disagrees");
        }
        std::copy(
            manifest_row.bytes.begin(), manifest_row.bytes.end(),
            encoded_manifest_.begin());
        manifest_row.release();
        extra = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            return systable_getnext_ordered(
                active_scan_, ForwardScanDirection);
        });
        bioseq::pg_utils::invoke_pg_as_cpp(
            [&] { systable_endscan_ordered(active_scan_); });
        active_scan_ = nullptr;
        if (HeapTupleIsValid(extra)) {
            fail_("Format-6 manifest primary key returned duplicates");
        }
        auto decoded_manifest =
            bioseq::indexing::decode_minimizer_format6_manifest(
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
            || decoded_manifest->compatibility_digest
                != head_->compatibility_digest) {
            fail_("Format-6 head/manifest authentication disagrees");
        }
        manifest_.emplace(*decoded_manifest);
        if (manifest_->generation_id != head_->generation_id
            || manifest_->source_epoch != head_->source_epoch
            || manifest_->source_identity != head_->source_identity
            || manifest_->source_content_crc64
                != head_->source_content_crc64
            || manifest_->compatibility_digest
                != head_->compatibility_digest
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
                valid_minimizer_format6_compatibility(
                    manifest_->compatibility,
                    manifest_->target_length)
            || manifest_->streams[0].kind
                != MinimizerFormat6StreamKind::directory
            || manifest_->streams[1].kind
                != MinimizerFormat6StreamKind::controls
            || manifest_->streams[2].kind
                != MinimizerFormat6StreamKind::postings) {
            fail_("Format-6 head/manifest authentication disagrees");
        }
        std::uint64_t first_row = 0u;
        for (std::size_t index = 0u; index < 3u; ++index) {
            const auto& stream = manifest_->streams[index];
            if (stream.row_count
                    != format6_pg_detail::ceil_rows(
                        stream.logical_bytes)
                || stream.first_row_ordinal != first_row
                || stream.row_count
                    > static_cast<std::uint64_t>(
                        (std::numeric_limits<std::int32_t>::max)())
                || stream.row_count
                    > (std::numeric_limits<std::uint64_t>::max)()
                        - first_row) {
                fail_("Format-6 stream row descriptor disagrees");
            }
            first_row += stream.row_count;
        }
        stats_.manifest_rows_read = 1u;
        stats_.manifest_bytes_read =
            kMinimizerFormat6ManifestBytes;
        stats_.metadata_heap_rows_read = 2u;
        stats_.metadata_bytes_read =
            kMinimizerFormat6HeadBytes
            + kMinimizerFormat6ManifestBytes;
        stats_.cold.add(
            stats_.cold.row_crc_bytes,
            kMinimizerFormat6ManifestBytes);
        state_ = State::metadata_ready;
        return true;
    }

    [[nodiscard]] std::span<const std::byte>
    encoded_head() const noexcept {
        return state_ == State::metadata_ready
                || state_ == State::source_attested
            ? std::span<const std::byte>{encoded_head_}
            : std::span<const std::byte>{};
    }

    [[nodiscard]] std::span<const std::byte>
    encoded_manifest() const noexcept {
        return state_ == State::metadata_ready
                || state_ == State::source_attested
            ? std::span<const std::byte>{encoded_manifest_}
            : std::span<const std::byte>{};
    }

    [[nodiscard]] const MinimizerFormat6Head& head() const {
        if (!head_) {
            rev3_storage_error_as_cpp(
                "Format-6 head requested before metadata");
        }
        return *head_;
    }

    [[nodiscard]] const MinimizerFormat6Manifest& manifest() const {
        if (!manifest_) {
            rev3_storage_error_as_cpp(
                "Format-6 manifest requested before metadata");
        }
        return *manifest_;
    }

    [[nodiscard]] const MinimizerFormat6ExpectedGeneration&
    validate_source_attestation(Rev3CatalogSourceProof&& proof) {
        try {
            if (state_ != State::metadata_ready
                || !head_ || !manifest_
                || !proof.valid_
                || !proof.counters_.timed_equation_holds()
                || proof.snapshot_ != snapshot_
                || proof.database_oid_ != MyDatabaseId
                || proof.tablespace_oid_ != expected_tablespace_oid_
                || proof.index_name_ != index_name_
                || proof.source_.sequence_rowno != query_key_
                || proof.table_name_ != table_name_) {
                proof.invalidate_();
                fail_("Format-6 source proof binding/work disagrees");
            }
            const auto source_identity =
                bioseq::indexing::minimizer_format6_source_identity(
                    query_key_,
                    proof.source_.locator.source_epoch,
                    proof.source_content_checksum_,
                    proof.source_.locator.total_bp_size);
            if (proof.source_.locator.source_epoch
                    != manifest_->source_epoch
                || proof.source_content_checksum_
                    != manifest_->source_content_crc64
                || proof.source_.locator.total_bp_size
                    != manifest_->target_length
                || proof.source_.alphabet_code
                    != manifest_->compatibility.alphabet_code
                || source_identity != manifest_->source_identity
                || source_identity != head_->source_identity) {
                proof.invalidate_();
                fail_("Format-6 source identity/content disagrees");
            }
            stats_.source_parent_rows_read =
                proof.counters_.parent_rows_read;
            stats_.source_catalog_rows_read =
                proof.counters_.catalog_rows_read;
            stats_.source_catalog_index_scans =
                proof.counters_.catalog_index_scans;
            stats_.source_fragment_rows_read =
                proof.counters_.fragment_rows_read;
            stats_.source_decoded_bp =
                proof.counters_.decoded_bp;
            stats_.source_storage_incarnations_checked =
                proof.counters_.storage_incarnations_checked;
            stats_.cold.merge(proof.cold_stats_);
            expected_generation_.emplace(
                MinimizerFormat6ExpectedGeneration{
                    .generation_id = head_->generation_id,
                    .query_key = query_key_,
                    .source_epoch = manifest_->source_epoch,
                    .source_content_crc64 =
                        manifest_->source_content_crc64,
                    .target_length = manifest_->target_length,
                    .compatibility = manifest_->compatibility,
                });
            proof.invalidate_();
            state_ = State::source_attested;
            return *expected_generation_;
        } catch (...) {
            proof.invalidate_();
            state_ = State::failed;
            throw;
        }
    }

    [[nodiscard]] MinimizerFormat6LogicalStreamSourceRef
    source_ref() noexcept {
        if (state_ != State::source_attested) return {};
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
    [[nodiscard]] MinimizerFormat6ParallelScanProbePg
    run_parallel_scan_probe(std::uint32_t requested_workers) {
        if (state_ != State::source_attested
            || active_scan_ != nullptr
            || heap_relation_ == nullptr
            || !manifest_) {
            fail_(
                "Format-6 parallel scan probe is out of order");
        }
        return format6_parallel_probe_detail::run_probe(
            heap_relation_, snapshot_, *manifest_,
            query_key_, requested_workers);
    }

    /// Administrative/restart audit. Each stream is reread in primary-key
    /// order with one physical row resident at a time. The scan authenticates
    /// every address-bound row receipt and the exact concatenated logical CRC.
    [[nodiscard]] std::array<
        MinimizerFormat6ExactCatalogStreamReceiptPg, 3>
    audit_complete_streams() {
        if (state_ != State::source_attested
            || active_scan_ != nullptr || !manifest_) {
            fail_("Format-6 complete stream audit is out of order");
        }
        std::array<MinimizerFormat6ExactCatalogStreamReceiptPg, 3>
            receipts{};
        for (std::size_t index = 0u; index < receipts.size(); ++index) {
            const MinimizerFormat6StreamKind kind =
                format6_pg_detail::stream_kind(index);
            const auto& descriptor = descriptor_(kind);
            Crc64Ecma182 crc{};
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
                fail_("Format-6 complete stream audit disagrees");
            }
        }
        return receipts;
    }

    /// Checked close used by a route-inapplicable, self-authenticated F6 head
    /// before the independent F5 sentinel is probed.
    void close_metadata_read() {
        if (state_ != State::metadata_ready
            || active_scan_ != nullptr) {
            fail_("Format-6 metadata close is out of order");
        }
        state_ = State::finished;
        close_relations_();
    }

    void close_head_absent() {
        if (state_ != State::head_absent
            || active_scan_ != nullptr) {
            fail_("Format-6 absent-head close is out of order");
        }
        state_ = State::finished;
        close_relations_();
    }

    [[nodiscard]] MinimizerGenerationReadStats finish() {
        if (state_ != State::source_attested
            || active_scan_ != nullptr
            || heap_relation_ == nullptr
            || primary_index_relation_ == nullptr) {
            fail_("Format-6 read finished out of order");
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
        stats_.format6.format_version =
            bioseq::indexing::kMinimizerFormat6Version;
        stats_.format6.sessions = 1u;
        stats_.format6.generation_id = head_->generation_id;
        stats_.format6.source_identity = head_->source_identity;
        stats_.format6.source_content_crc64 =
            head_->source_content_crc64;
        stats_.format6.compatibility_digest =
            manifest_->compatibility_digest;
        stats_.format6.directory_rows =
            stats_.directory_chunks_read;
        stats_.format6.directory_bytes =
            stats_.directory_bytes_read;
        stats_.format6.control_rows =
            stats_.controls.physical_heap_rows_read;
        stats_.format6.control_bytes =
            stats_.controls.physical_bytes_read;
        stats_.format6.posting_rows =
            stats_.positions.physical_heap_rows_read;
        stats_.format6.posting_bytes =
            stats_.positions.physical_bytes_read;
        state_ = State::finished;
        close_relations_();
        return stats_;
    }
};

static_assert(!std::is_default_constructible_v<
    IndexStorageMinimizerFormat6ReadSessionPg>);
static_assert(!std::is_copy_constructible_v<
    IndexStorageMinimizerFormat6ReadSessionPg>);
static_assert(std::is_nothrow_move_constructible_v<
    IndexStorageMinimizerFormat6ReadSessionPg>);

struct MinimizerFormat6ReadSessionOpenResultPg {
    std::optional<IndexStorageMinimizerFormat6ReadSessionPg>
        session{};
    MinimizerGenerationRelationInspection relation_failure{
        .kind = MinimizerGenerationRelationKind::malformed,
        .revision3_error = Rev3IndexSchemaError::none,
    };
    bool head_absent = false;

    [[nodiscard]] explicit operator bool() const noexcept {
        return session.has_value();
    }
};

[[nodiscard]] inline MinimizerFormat6ReadSessionOpenResultPg
open_exact_minimizer_format6_read_session_pg(
        std::string_view table_name,
        std::string_view index_name,
        std::uint64_t query_key,
        const BorrowedExecutorReadContextPg& executor_read) {
    if (!executor_read.valid()) {
        rev3_storage_error_as_cpp(
            "Format-6 read requires a valid executor context");
    }
    MinimizerFormat6ReadSessionOpenResultPg result{};
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
                    MinimizerGenerationRelationKind::revision3_exact,
                .revision3_error = Rev3IndexSchemaError::none,
            };
        } else {
            result.relation_failure = {
                .kind =
                    MinimizerGenerationRelationKind::revision3_exact,
                .revision3_error = Rev3IndexSchemaError::none,
            };
        }
    } catch (const MinimizerGenerationRelationOpenFailurePg& failure) {
        result.relation_failure = failure.inspection;
    }
    return result;
}

/// Read-only restart/audit receipt for the complete fixed Format-6 metadata
/// surface. Stream descriptors carry the directory/control/posting counts,
/// byte lengths and CRCs; the manifest carries the generation CRC.
struct ReadyMinimizerFormat6MetadataIdentityPg {
    MinimizerFormat6Head head{};
    MinimizerFormat6Manifest manifest{};
    EncodedMinimizerFormat6Head encoded_head{};
    EncodedMinimizerFormat6Manifest encoded_manifest{};
    Rev3AuthoritativeSourceLocator source{};
    std::uint64_t source_content_checksum = 0;
    Rev3CatalogSourceProofCounters source_counters{};
    std::array<MinimizerFormat6ExactCatalogStreamReceiptPg, 3>
        exact_catalog_streams{};
};

[[nodiscard]] inline ReadyMinimizerFormat6MetadataIdentityPg
inspect_ready_minimizer_format6_metadata_pg(
        const Rev3IndexAdministrationIdentity& identity,
        std::uint64_t query_key,
        Snapshot registered_snapshot,
        Rev3CatalogSourceProof&& proof) {
    if (identity.database_oid != MyDatabaseId
        || identity.index_name.empty()
        || registered_snapshot == nullptr
        || !IsMVCCSnapshot(registered_snapshot)
        || registered_snapshot->regd_count == 0) {
        rev3_storage_error_as_cpp(
            "Format-6 metadata inspection input is invalid");
    }
    const std::string table_name{
        proof.table_name_for_diagnostics()};
    if (table_name.empty()
        || (!identity.table_name.empty()
            && identity.table_name != table_name)) {
        rev3_storage_error_as_cpp(
            "Format-6 metadata inspection source/table binding is invalid");
    }
    const Rev3AuthoritativeSourceLocator source =
        proof.source_for_diagnostics();
    const std::uint64_t source_content_checksum =
        proof.source_content_checksum_for_diagnostics();
    const Rev3CatalogSourceProofCounters source_counters =
        proof.counters_for_diagnostics();
    IndexStorageMinimizerFormat6ReadSessionPg session{
        table_name,
        identity.index_name,
        query_key,
        registered_snapshot,
        identity.tablespace_oid,
    };
    if (!session.read_active_head_manifest_once()) {
        session.close_head_absent();
        rev3_storage_error_as_cpp(
            "Format-6 metadata inspection found no READY head");
    }
    (void)session.validate_source_attestation(std::move(proof));
    ReadyMinimizerFormat6MetadataIdentityPg result{
        .head = session.head(),
        .manifest = session.manifest(),
        .source = source,
        .source_content_checksum = source_content_checksum,
        .source_counters = source_counters,
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

[[nodiscard]] inline ReadyMinimizerFormat6MetadataIdentityPg
inspect_ready_minimizer_format6_metadata_pg(
        const Rev3IndexAdministrationIdentity& identity,
        std::uint64_t query_key,
        Snapshot registered_snapshot) {
    Rev3CatalogSourceProof proof =
        derive_rev3_catalog_source_proof_pg(
            identity, query_key, registered_snapshot);
    return inspect_ready_minimizer_format6_metadata_pg(
        identity, query_key, registered_snapshot, std::move(proof));
}

/// Test-only transport POC. It authenticates the same READY head, manifest
/// and source binding as the production reader, then scans only the three
/// current-generation Format-6 stream subkeys through either one serial heap
/// scan or one core ParallelContext/shared ParallelTableScanDesc.
[[nodiscard]] inline MinimizerFormat6ParallelScanProbePg
probe_ready_minimizer_format6_parallel_scan_pg(
        const Rev3IndexAdministrationIdentity& identity,
        std::uint64_t query_key,
        Snapshot registered_snapshot,
        Rev3CatalogSourceProof&& proof,
        std::uint32_t requested_workers) {
    if (identity.database_oid != MyDatabaseId
        || identity.index_name.empty()
        || registered_snapshot == nullptr
        || !IsMVCCSnapshot(registered_snapshot)
        || registered_snapshot->regd_count == 0) {
        rev3_storage_error_as_cpp(
            "Format-6 parallel scan probe input is invalid");
    }
    const std::string table_name{
        proof.table_name_for_diagnostics()};
    if (table_name.empty()) {
        rev3_storage_error_as_cpp(
            "Format-6 parallel scan probe table binding is empty");
    }
    IndexStorageMinimizerFormat6ReadSessionPg session{
        table_name,
        identity.index_name,
        query_key,
        registered_snapshot,
        identity.tablespace_oid,
    };
    if (!session.read_active_head_manifest_once()) {
        session.close_head_absent();
        rev3_storage_error_as_cpp(
            "Format-6 parallel scan probe found no READY head");
    }
    (void)session.validate_source_attestation(std::move(proof));
    MinimizerFormat6ParallelScanProbePg result =
        session.run_parallel_scan_probe(requested_workers);
    (void)session.finish();
    return result;
}

inline void format6_parallel_scan_probe_worker_main_pg(
        dsm_segment* segment,
        shm_toc* toc) {
    using namespace format6_parallel_probe_detail;
    if (segment == nullptr || toc == nullptr
        || !IsParallelWorker()) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq Format-6 parallel scan worker binding is invalid")));
    }
    auto* header = static_cast<Header*>(
        shm_toc_lookup(toc, kHeaderTocKey, false));
    auto* parallel_scan =
        static_cast<ParallelTableScanDesc>(
            shm_toc_lookup(toc, kParallelScanTocKey, false));
    auto* seen_words = static_cast<pg_atomic_uint64*>(
        shm_toc_lookup(toc, kSeenBitsetTocKey, false));
    if (header == nullptr || parallel_scan == nullptr
        || seen_words == nullptr
        || header->magic != kTocMagic
        || header->version != kHeaderVersion
        || header->relation_locator.dbOid != MyDatabaseId
        || GetActiveSnapshot() == nullptr
        || !IsMVCCSnapshot(GetActiveSnapshot())) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq Format-6 parallel scan worker state is invalid")));
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

/// Result of one inactive revision-3/4 logical-index clear.  The operation is
/// deliberately separate from the legacy `clear()` entry point until every
/// live reader and writer is switched atomically to the generation format.
struct MinimizerGenerationClearResult {
    std::uint64_t dropped_relation_count = 0;
    std::uint64_t deleted_metadata_age_rows = 0;
};

struct CanonicalGenerationSidecarName {
    bool valid = false;
    std::uint8_t bucket_idx = 0;
    std::uint32_t shard_idx = 0;
};

/// Parse only the exact spelling emitted by `format_seqidx_name`: unsigned
/// canonical decimal bucket, one underscore, unsigned canonical decimal shard.
/// Leading zeroes, signs, trailing decorations, overflow, and extra separators
/// are rejected without allocation.
[[nodiscard]] inline CanonicalGenerationSidecarName
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
    return CanonicalGenerationSidecarName{
        .valid = true,
        .bucket_idx = static_cast<std::uint8_t>(bucket),
        .shard_idx = static_cast<std::uint32_t>(shard),
    };
}

/// Clear-only DDL executor.  Unlike the legacy helper, every SPI transition is
/// checked and the caller-owned activity bit remains set if PostgreSQL jumps
/// out of SPI_execute so the enclosing PG_CATCH can finish the connection.
inline void run_checked_generation_clear_ddl(
        const char* ddl,
        bool& spi_active) {
    if (ddl == nullptr || *ddl == '\0' || spi_active) {
        rev3_storage_error("generation clear DDL state is invalid");
    }
    const int connect_rc = SPI_connect();
    if (connect_rc != SPI_OK_CONNECT) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("bioseq revision-3 index: generation clear DDL SPI_connect failed"),
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
             errmsg("bioseq revision-3 index: generation clear DDL failed"),
             errdetail("execute_rc=%d finish_rc=%d",
                execute_rc, finish_rc)));
    }
}

// =========================================================================
//  IndexStorageFragmentPg  —  CHUNKED_PER_FRAGMENT
// =========================================================================
struct IndexStorageFragmentPg {
    using GenerationReadSession =
        IndexStorageFragmentGenerationReadSessionPg;
    using GenerationRowSink =
        IndexStorageFragmentGenerationRowSinkPg;
    using Format6GenerationWriter =
        IndexStorageMinimizerFormat6WriterPg;

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
    mutable index_sub_key_t decoded_sub_ = DIRECTORY_SUB_KEY;
    mutable std::size_t   decoded_chunk_ = std::numeric_limits<std::size_t>::max();
    struct CachedChunk {
        // Admission outlives the bytes it covers (reverse member destruction).
        bioseq::parallelism::MemoryPermit permit;
        index_sub_key_t sub_key = DIRECTORY_SUB_KEY;
        std::size_t chunk_no = std::numeric_limits<std::size_t>::max();
        std::size_t payload_bytes = 0;
        std::uint64_t last_used = 0;
        std::vector<char> bytes;
    };
    mutable std::vector<CachedChunk> chunk_cache_;
    mutable std::uint64_t chunk_cache_clock_ = 0;
    mutable std::size_t chunk_cache_bytes_ = 0;
    static constexpr std::size_t kChunkCacheMaxEntries = 8;
    static constexpr std::size_t kChunkCacheMaxBytes = 256ULL << 20;
    std::int64_t* scratch_ = nullptr;
    std::size_t   scratch_units_ = 0;
    std::size_t   run_buffer_override_bytes_ = 0;
    // per-sub_key byte plan recorded by resize/resize_chunks, consumed by write_to_each.
    std::map<index_sub_key_t, std::vector<std::size_t>> chunk_payload_bytes_;
    std::optional<SortedPairRunStore> sorted_runs_;

private:
    /// Own the PostgreSQL resources used by one legacy fragment read while the
    /// surrounding C++ frame is live.  Destruction deliberately performs no
    /// PostgreSQL work: a healthy path must close explicitly, while a translated
    /// PostgreSQL ERROR must abandon the handles for transaction-abort cleanup.
    struct LegacyFragmentReadResources {
        Relation relation = nullptr;
        Relation ordered_index_relation = nullptr;
        SysScanDesc scan = nullptr;
        bool ordered_scan = false;
        bytea* detoasted = nullptr;
        bool detoasted_is_copy = false;
        LOCKMODE relation_lock = NoLock;
        bool abandoned = false;

        explicit LegacyFragmentReadResources(LOCKMODE lock) noexcept
            : relation_lock(lock) {}
        LegacyFragmentReadResources(
            const LegacyFragmentReadResources&) = delete;
        LegacyFragmentReadResources& operator=(
            const LegacyFragmentReadResources&) = delete;
        ~LegacyFragmentReadResources() {
            DEBUG_ASSERT(
                abandoned
                    || (relation == nullptr && scan == nullptr
                        && ordered_index_relation == nullptr
                        && detoasted == nullptr),
                "legacy fragment read resources require checked close or abandonment");
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
                bioseq::pg_utils::invoke_pg_as_cpp(
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
                bioseq::pg_utils::invoke_pg_as_cpp(
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
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [finished_index] {
                        index_close(finished_index, AccessShareLock);
                    });
                ordered_index_relation = nullptr;
            }
            if (relation != nullptr) {
                Relation finished_relation = relation;
                const LOCKMODE finished_lock = relation_lock;
                bioseq::pg_utils::invoke_pg_as_cpp(
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
        LegacyFragmentReadResources>);
    static_assert(!std::is_copy_assignable_v<
        LegacyFragmentReadResources>);
    static_assert(std::is_nothrow_destructible_v<
        LegacyFragmentReadResources>);
    static_assert(noexcept(
        std::declval<LegacyFragmentReadResources&>()
            .abandon_after_pg_error_noexcept()));

    /// Own every PostgreSQL object used by the legacy fragment writer while
    /// the surrounding minimizer build still owns vectors, run stores and
    /// admission permits.  The destructor deliberately performs no PostgreSQL
    /// work: normal paths close explicitly and translated PostgreSQL errors
    /// abandon the handles for transaction-abort cleanup.
    struct LegacyFragmentWriteResources {
        Relation relation = nullptr;
        SysScanDesc scan = nullptr;
        HeapTuple tuple = nullptr;
        bytea* blob = nullptr;
        LOCKMODE relation_lock = NoLock;
        bool abandoned = false;

        explicit LegacyFragmentWriteResources(LOCKMODE lock) noexcept
            : relation_lock(lock) {}
        LegacyFragmentWriteResources(
            const LegacyFragmentWriteResources&) = delete;
        LegacyFragmentWriteResources& operator=(
            const LegacyFragmentWriteResources&) = delete;
        ~LegacyFragmentWriteResources() {
            DEBUG_ASSERT(
                abandoned
                    || (relation == nullptr && scan == nullptr
                        && tuple == nullptr && blob == nullptr),
                "legacy fragment write resources require checked close or abandonment");
        }

        void release_tuple_checked() {
            if (tuple != nullptr) {
                HeapTuple finished_tuple = tuple;
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [finished_tuple] { heap_freetuple(finished_tuple); });
                tuple = nullptr;
            }
        }

        void release_blob_checked() {
            if (blob != nullptr) {
                bytea* finished_blob = blob;
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [finished_blob] { pfree(finished_blob); });
                blob = nullptr;
            }
        }

        void finish_scan_checked() {
            if (scan != nullptr) {
                SysScanDesc finished_scan = scan;
                bioseq::pg_utils::invoke_pg_as_cpp(
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
                bioseq::pg_utils::invoke_pg_as_cpp(
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
        LegacyFragmentWriteResources>);
    static_assert(!std::is_copy_assignable_v<
        LegacyFragmentWriteResources>);
    static_assert(std::is_nothrow_destructible_v<
        LegacyFragmentWriteResources>);
    static_assert(noexcept(
        std::declval<LegacyFragmentWriteResources&>()
            .abandon_after_pg_error_noexcept()));

    // Authority-bearing MPS spill state is intentionally not part of the
    // otherwise duck-typed storage surface.  Callers may observe its immutable
    // binding and mutate it only through the narrow logical-event APIs below.
    using SegmentRunEntry =
        std::pair<std::uint32_t, SortedPairRunStore>;
    // Reverse destruction drops stores/vector nodes before metadata admission.
    bioseq::parallelism::MemoryPermit segment_sorted_runs_metadata_permit_;
    std::vector<SegmentRunEntry> segment_sorted_runs_;
    inline static std::atomic<std::uint64_t>
        next_segment_spill_generation_token_{1u};
    MpsSegmentSpillStorageBinding segment_spill_binding_{};
    bool segment_spill_pg_poisoned_ = false;
    void record_segment_spill_mutation_() noexcept;
    [[nodiscard]] bool ensure_segment_run_metadata_capacity_(
            std::size_t required) {
        const std::uint64_t retained = bioseq::parallelism::saturating_mul(
            static_cast<std::uint64_t>(segment_sorted_runs_.capacity()),
            static_cast<std::uint64_t>(sizeof(SegmentRunEntry)));
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
                static_cast<std::uint64_t>(sizeof(SegmentRunEntry))),
            bioseq::parallelism::kVectorPreallocationHeadroomFactor);
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
                bioseq::parallelism::MemoryAdmissionController::instance()
                    .try_reserve(peak);
            if (!permit) {
                return false;
            }
            segment_sorted_runs_metadata_permit_ = std::move(*permit);
        }
        segment_sorted_runs_.reserve(required);
        const std::uint64_t exact = bioseq::parallelism::saturating_mul(
            static_cast<std::uint64_t>(segment_sorted_runs_.capacity()),
            static_cast<std::uint64_t>(sizeof(SegmentRunEntry)));
        if (exact == (std::numeric_limits<std::uint64_t>::max)()
            || exact > segment_sorted_runs_metadata_permit_.bytes()) {
            return false;
        }
        segment_sorted_runs_metadata_permit_.shrink_to(exact);
        return true;
    }

public:

    IndexStorageFragmentPg() = default;
    IndexStorageFragmentPg(const IndexStorageFragmentPg&) = delete;
    IndexStorageFragmentPg& operator=(const IndexStorageFragmentPg&) = delete;
    IndexStorageFragmentPg(IndexStorageFragmentPg&& o) noexcept { steal_(std::move(o)); }
    IndexStorageFragmentPg& operator=(IndexStorageFragmentPg&&) = delete;
    ~IndexStorageFragmentPg() { release_(); }

    void steal_(IndexStorageFragmentPg&& o) noexcept {
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
        o.decoded_sub_=DIRECTORY_SUB_KEY;
        o.decoded_chunk_=(std::numeric_limits<std::size_t>::max)();
        {
            std::vector<CachedChunk> empty;
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
        // SortedPairRunStore::release_resident_runs_noexcept drops logical
        // memory-only runs; unlike its internal memory-to-BufFile compaction,
        // this is not representation-preserving and must invalidate replay.
        record_segment_spill_mutation_();
    }
    void release_() noexcept {
        DEBUG_ASSERT(own_mctx_ == nullptr,
            "IndexStorageFragmentPg must be close_checked or abandoned before destruction");
        DEBUG_ASSERT(!sorted_runs_.has_value()
                && segment_sorted_runs_.empty(),
            "IndexStorageFragmentPg run stores must be close_checked or abandoned before destruction");
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
        bioseq::pg_utils::invoke_pg_as_cpp(
            [message_data, message_size] {
                ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq index: %.*s",
                        message_size, message_data)));
            });
        pg_unreachable();
    }

    // ---- validators (translate to ereport on the pg side) ----
    [[nodiscard]] MinimizerHashmapFragmentIndex::SerializedHeader validate_minimizer_blob_or_error(
            const char* p, std::size_t bytes, std::optional<std::uint32_t> expected_bucket_count = std::nullopt) const {
        using H = MinimizerHashmapFragmentIndex::SerializedHeader;
        using B = MinimizerHashmapFragmentIndex::SerializedBucket;
        if (bytes < sizeof(H)) index_error("minimizer blob truncated header");
        H h{}; std::memcpy(&h, p, sizeof(H));
        if (h.magic != MinimizerHashmapFragmentIndex::kSerializeMagic || h.version != MinimizerHashmapFragmentIndex::kSerializeVersion)
            index_error("minimizer blob magic/version");
        const std::size_t expect = sizeof(H) + sizeof(B)*h.bucket_count + sizeof(std::uint64_t)*h.total_positions;
        if (bytes != expect) index_error("minimizer blob size mismatch");
        if (expected_bucket_count && h.bucket_count != *expected_bucket_count) index_error("minimizer blob bucket_count mismatch");
        return h;
    }
    void validate_minimizer_directory_or_error(const char* p, std::size_t bytes) const {
        if (bytes < 40) index_error("directory truncated");
        std::uint32_t magic{}, version{}; std::memcpy(&magic, p, 4); std::memcpy(&version, p+4, 4);
        if (magic != 0xB1000002u || version != 2u) index_error("directory magic/version");
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
        decoded_sub_ = DIRECTORY_SUB_KEY;
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
    /// is reused by OID and any legacy/malformed same-name object fails closed.
    /// Explicit clear/reset owns the only destructive cutover boundary.
    [[nodiscard]] Oid ensure_minimizer_generation_storage() {
        if (index_name_ != MINIMIZERS_PER_FRAGMENT_INDEX_NAME
            && index_name_ != MINIMIZERS_PER_SEQUENCE_INDEX_NAME) {
            rev3_storage_error(
                "generation storage is only valid for minimizer indexes");
        }
        if (db_oid_ != MyDatabaseId || !OidIsValid(user_relid_)
            || !OidIsValid(tablespace_oid_)
            || !OidIsValid(main_relfilenode_) || column_attno_ <= 0) {
            rev3_storage_error("generation storage identity is invalid");
        }
        acquire_index_advisory_xact_lock(
            db_oid_, tablespace_oid_, main_relfilenode_, column_attno_,
            index_name_);

        Relation source_relation = try_safe_relation_open(
            user_relid_, AccessShareLock);
        if (source_relation == nullptr
            || source_relation->rd_rel->relpersistence
                == RELPERSISTENCE_TEMP
            || source_relation->rd_locator.spcOid != tablespace_oid_) {
            if (source_relation != nullptr) {
                relation_close(source_relation, AccessShareLock);
            }
            rev3_storage_error(
                "generation storage requires a permanent/unlogged source in the resolved tablespace");
        }
        relation_close(source_relation, AccessShareLock);

        const std::string relation_name = this_table_name();
        const Oid internal_namespace = bioseq_internal_ns();
        const Rev3IndexSchemaExpectation expected_schema{
            .namespace_oid = internal_namespace,
            .relation_name = relation_name,
            .tablespace_oid = tablespace_oid_,
        };
        index_relid_ = InvalidOid;
        Oid relation_oid = get_relname_relid(
            relation_name.c_str(), internal_namespace);

        if (OidIsValid(relation_oid)) {
            Relation relation = try_safe_relation_open(
                relation_oid, AccessShareLock);
            if (relation == nullptr
                || RelationGetNamespace(relation) != internal_namespace
                || std::string_view{RelationGetRelationName(relation)}
                    != relation_name) {
                if (relation != nullptr) {
                    relation_close(relation, AccessShareLock);
                }
                rev3_storage_error(
                    "generation relation disappeared during schema inspection");
            }
            const Rev3IndexSchemaInspection schema =
                inspect_rev3_index_schema(
                    relation, nullptr, expected_schema);
            if (schema) {
                // Keep the AccessShare relation lock until transaction end so
                // external DDL cannot replace the proven tuple descriptor
                // before the row sink opens it.
                evict_generation_storage_residency_();
                relation_close(relation, NoLock);
                return relation_oid;
            }
            relation_close(relation, AccessShareLock);
            rev3_storage_error(fmt::format(
                "existing generation relation requires explicit clear/reset: {}",
                rev3_index_schema_error_name(schema.error)));
        }

        char* raw_tablespace_name = get_tablespace_name(tablespace_oid_);
        if (raw_tablespace_name == nullptr) {
            rev3_storage_error("resolved source tablespace no longer exists");
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
            ? try_safe_relation_open(relation_oid, AccessShareLock)
            : nullptr;
        if (created == nullptr
            || RelationGetNamespace(created) != internal_namespace
            || std::string_view{RelationGetRelationName(created)}
                != relation_name) {
            if (created != nullptr) {
                relation_close(created, AccessShareLock);
            }
            rev3_storage_error(
                "canonical generation relation was not created");
        }
        const Rev3IndexSchemaInspection created_schema =
            inspect_rev3_index_schema(created, nullptr, expected_schema);
        if (!created_schema) {
            relation_close(created, AccessShareLock);
            rev3_storage_error(fmt::format(
                "created generation relation schema mismatch: {}",
                rev3_index_schema_error_name(created_schema.error)));
        }
        relation_close(created, NoLock);
        CacheInvalidateRelcacheByRelid(relation_oid);
        CacheInvalidateRelcacheByRelid(created_schema.primary_index_oid);
        // Broadcast on the owning relation because every residency key carries
        // `user_relid`, not the physical sidecar OID. Other backends can evict
        // legacy bytes after this first canonical sidecar creation commits.
        CacheInvalidateRelcacheByRelid(user_relid_);
        evict_generation_storage_residency_();
        return relation_oid;
    }

    /// Transactionally remove the complete physical generation family for
    /// this one logical minimizer index.  Head, manifest, immutable payload,
    /// and temporary-run objects are row classes in the canonical sidecars,
    /// so dropping every table under the exact identity prefix clears all of
    /// them together.  This is an inactive prerequisite: no production caller
    /// is routed here until the format-4 reader/writer cutover is atomic.
    [[nodiscard]] MinimizerGenerationClearResult
    clear_minimizer_generation_storage() {
        struct PhysicalRelation {
            Oid oid = InvalidOid;
            std::string name;
            std::uint8_t bucket_idx = 0;
            std::uint32_t shard_idx = 0;
        };
        struct ClearState {
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
            std::string delete_age_sql;
            std::string ddl_sql;
            std::vector<std::pair<std::uint8_t, std::uint32_t>>
                owned_shards;
            std::vector<PhysicalRelation> candidates;
            std::vector<PhysicalRelation> relations;
            std::vector<PhysicalRelation> verification_candidates;
            MinimizerGenerationClearResult result{};
            std::exception_ptr cpp_error;
            bool completed = false;
        };
        auto* state = new (std::nothrow) ClearState{};
        if (state == nullptr) {
            ereport(ERROR,
                (errcode(ERRCODE_OUT_OF_MEMORY),
                 errmsg("bioseq revision-3 index: generation clear state allocation failed")));
        }
        MemoryContext saved_context = CurrentMemoryContext;
        ErrorData* volatile postgres_error = nullptr;

        PG_TRY();
        {
            try {
                if (index_name_ != MINIMIZERS_PER_FRAGMENT_INDEX_NAME
                    && index_name_ != MINIMIZERS_PER_SEQUENCE_INDEX_NAME) {
                    rev3_storage_error(
                        "generation clear is only valid for minimizer indexes");
                }
                if (db_oid_ != MyDatabaseId || !OidIsValid(user_relid_)
                    || !OidIsValid(tablespace_oid_)
                    || !OidIsValid(main_relfilenode_)
                    || column_attno_ <= 0) {
                    rev3_storage_error("generation clear identity is invalid");
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
                         errmsg("bioseq revision-3 index: generation clear discovery SPI_connect failed"),
                         errdetail("SPI rc=%d", discovery_connect_rc)));
                }
                state->spi_active = true;
                const int owned_rc = SPI_execute(
                    state->owned_shards_sql.c_str(), true, 0);
                if (owned_rc != SPI_OK_SELECT) {
                    ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("bioseq revision-3 index: generation clear owned-shard discovery failed"),
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
                        rev3_storage_error(
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
                         errmsg("bioseq revision-3 index: generation clear relation discovery failed"),
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
                        rev3_storage_error(
                            "generation clear discovery returned a NULL identity");
                    }
                    state->candidates.emplace_back();
                    PhysicalRelation& candidate = state->candidates.back();
                    candidate.oid = DatumGetObjectId(oid_datum);
                    candidate.name.assign(state->spi_text);
                    pfree(state->spi_text);
                    state->spi_text = nullptr;
                    if (!OidIsValid(candidate.oid)
                        || !candidate.name.starts_with(state->prefix)) {
                        rev3_storage_error(
                            "generation clear discovery escaped its exact prefix");
                    }
                }
                const int discovery_finish_rc = SPI_finish();
                state->spi_active =
                    discovery_finish_rc != SPI_OK_FINISH;
                if (discovery_finish_rc != SPI_OK_FINISH) {
                    ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("bioseq revision-3 index: generation clear discovery SPI_finish failed"),
                         errdetail("SPI rc=%d", discovery_finish_rc)));
                }

                // A broad prefix match is only a candidate.  Ownership also
                // requires canonical decimal spelling, an exact source shard
                // catalog identity, and the exact generation tuple schema.
                for (std::size_t candidate_index = 0;
                     candidate_index < state->candidates.size();
                     ++candidate_index) {
                    PhysicalRelation& candidate =
                        state->candidates[candidate_index];
                    const CanonicalGenerationSidecarName parsed =
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
                    state->locked_relation = try_safe_relation_open(
                        candidate.oid, AccessShareLock);
                    state->locked_relation_mode = AccessShareLock;
                    if (state->locked_relation == nullptr
                        || get_relname_relid(candidate.name.c_str(),
                            state->internal_namespace) != candidate.oid
                        || RelationGetNamespace(state->locked_relation)
                            != state->internal_namespace
                        || std::string_view{RelationGetRelationName(
                            state->locked_relation)} != candidate.name) {
                        rev3_storage_error(
                            "generation clear candidate identity changed during inspection");
                    }
                    const Rev3IndexSchemaInspection schema =
                        inspect_rev3_index_schema(state->locked_relation);
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
                    PhysicalRelation& target =
                        state->relations[relation_index];
                    state->locked_relation = try_safe_relation_open(
                        target.oid, AccessExclusiveLock);
                    state->locked_relation_mode = AccessExclusiveLock;
                    if (state->locked_relation == nullptr
                        || get_relname_relid(target.name.c_str(),
                            state->internal_namespace) != target.oid
                        || RelationGetNamespace(state->locked_relation)
                            != state->internal_namespace
                        || std::string_view{RelationGetRelationName(
                            state->locked_relation)} != target.name) {
                        rev3_storage_error(
                            "generation relation identity changed while acquiring clear lock");
                    }
                    const Rev3IndexSchemaInspection locked_schema =
                        inspect_rev3_index_schema(state->locked_relation);
                    if (!locked_schema) {
                        rev3_storage_error(
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
                        rev3_storage_error(
                            "generation clear DDL retained an owned relation");
                    }
                }
                state->result.dropped_relation_count =
                    static_cast<std::uint64_t>(state->relations.size());

                state->quoted_literal = quote_literal_cstr(
                    state->index_name.c_str());
                state->delete_age_sql = fmt::format(
                    "DELETE FROM bioseq_internal.metadata_age_log "
                    "WHERE user_relid={}::oid "
                    "AND main_relfilenode={}::oid "
                    "AND column_attno={}::smallint AND index_name={}",
                    static_cast<unsigned>(user_relid_),
                    static_cast<unsigned>(main_relfilenode_),
                    static_cast<int>(column_attno_),
                    state->quoted_literal);
                pfree(state->quoted_literal);
                state->quoted_literal = nullptr;
                const int metadata_connect_rc = SPI_connect();
                if (metadata_connect_rc != SPI_OK_CONNECT) {
                    ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("bioseq revision-3 index: generation clear metadata SPI_connect failed"),
                         errdetail("SPI rc=%d", metadata_connect_rc)));
                }
                state->spi_active = true;
                const int delete_age_rc = SPI_execute(
                    state->delete_age_sql.c_str(), false, 0);
                if (delete_age_rc != SPI_OK_DELETE || SPI_processed > 1) {
                    ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("bioseq revision-3 index: generation clear metadata deletion failed"),
                         errdetail("SPI rc=%d rows=%llu", delete_age_rc,
                            static_cast<unsigned long long>(SPI_processed))));
                }
                state->result.deleted_metadata_age_rows =
                    static_cast<std::uint64_t>(SPI_processed);
                const int metadata_finish_rc = SPI_finish();
                state->spi_active =
                    metadata_finish_rc != SPI_OK_FINISH;
                if (metadata_finish_rc != SPI_OK_FINISH) {
                    ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("bioseq revision-3 index: generation clear metadata SPI_finish failed"),
                         errdetail("SPI rc=%d", metadata_finish_rc)));
                }
                CommandCounterIncrement();

                // Re-discover broadly, but fail only if a canonical,
                // catalog-owned, exact generation relation survived.  Benign
                // near-prefix relations are deliberately ignored.
                const int verify_connect_rc = SPI_connect();
                if (verify_connect_rc != SPI_OK_CONNECT) {
                    ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("bioseq revision-3 index: generation clear verification SPI_connect failed"),
                         errdetail("SPI rc=%d", verify_connect_rc)));
                }
                state->spi_active = true;
                const int verify_rc = SPI_execute(
                    state->discover_sql.c_str(), true, 0);
                if (verify_rc != SPI_OK_SELECT) {
                    ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("bioseq revision-3 index: generation clear verification query failed"),
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
                        rev3_storage_error(
                            "generation clear verification returned a NULL identity");
                    }
                    state->verification_candidates.emplace_back();
                    PhysicalRelation& candidate =
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
                         errmsg("bioseq revision-3 index: generation clear verification SPI_finish failed"),
                         errdetail("SPI rc=%d", verify_finish_rc)));
                }
                for (std::size_t candidate_index = 0;
                     candidate_index
                        < state->verification_candidates.size();
                     ++candidate_index) {
                    PhysicalRelation& candidate =
                        state->verification_candidates[candidate_index];
                    const CanonicalGenerationSidecarName parsed =
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
                    state->locked_relation = try_safe_relation_open(
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
                    const Rev3IndexSchemaInspection schema =
                        inspect_rev3_index_schema(state->locked_relation);
                    relation_close(
                        state->locked_relation, AccessShareLock);
                    state->locked_relation = nullptr;
                    state->locked_relation_mode = NoLock;
                    if (exact_identity && schema) {
                        rev3_storage_error(
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
                 errmsg("bioseq revision-3 index: generation clear failed"),
                 errdetail("C++ error=%s", cpp_error_detail)));
        }
        if (!state->completed) {
            delete state;
            rev3_storage_error(
                "generation clear did not complete or report an error");
        }
        const MinimizerGenerationClearResult result = state->result;
        delete state;
        return result;
    }

    /// Open the inactive revision-3 reader against the exact physical name,
    /// intentionally bypassing the legacy cached OID. A five-column relation
    /// therefore fails schema validation rather than entering a legacy route.
    [[nodiscard]] GenerationReadSession begin_generation_read(
            std::uint64_t query_key,
            Snapshot executor_snapshot,
            ExpectedMinimizerCompatibility expected) const {
        if (query_key != query_key_) {
            rev3_storage_error("generation read changed the handle query key");
        }
        return GenerationReadSession{
            this_table_name(), query_key, executor_snapshot, expected,
            tablespace_oid_,
            Rev3SnapshotRegistration::borrow_registered};
    }

    /// Open the exact common-encoder row sink after atomically ensuring the
    /// canonical same-name relation.  The caller still owns generation
    /// encoding, complete reread, and READY-head publication.
    [[nodiscard]] GenerationRowSink begin_generation_write(
            std::uint64_t query_key,
            std::uint64_t generation_id) {
        if (query_key != query_key_) {
            rev3_storage_error("generation writer changed the handle query key");
        }
        (void)ensure_minimizer_generation_storage();
        return GenerationRowSink{
            Rev3IndexAdministrationIdentity{
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

    [[nodiscard]] Format6GenerationWriter
    begin_format6_generation_write(
            std::uint64_t query_key,
            std::uint64_t generation_id) {
        if (query_key != query_key_) {
            rev3_storage_error(
                "Format-6 writer changed the handle query key");
        }
        (void)ensure_minimizer_generation_storage();
        return Format6GenerationWriter{
            Rev3IndexAdministrationIdentity{
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
        Oid rid = bioseq::pg_utils::invoke_pg_as_cpp(
            [relation_name, ns] {
                return get_relname_relid(relation_name, ns);
            });
        // The C++ handle can outlive a PostgreSQL subtransaction.  Never trust
        // a cached OID unless the authoritative namespace/name lookup still
        // resolves to exactly that OID; rollback may have restored an old
        // relation or removed a relation created below a SAVEPOINT.
        if (OidIsValid(index_relid_) && index_relid_ == rid) {
            return index_relid_;
        }
        index_relid_ = InvalidOid;
        if (!OidIsValid(rid) && create_if_missing) {
            const Oid user_relid = user_relid_;
            const char* persistence = bioseq::pg_utils::invoke_pg_as_cpp(
                [user_relid] { return persistence_kw_for(user_relid); });
			const char* quoted_name_pg =
				bioseq::pg_utils::invoke_pg_as_cpp(
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
					bioseq::pg_utils::invoke_pg_as_cpp(
						[quoted_name_pg] {
							pfree(const_cast<char*>(quoted_name_pg));
						});
				}
				throw;
			}
			if (quoted_name_is_copy) {
				bioseq::pg_utils::invoke_pg_as_cpp(
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
            bioseq::pg_utils::invoke_pg_as_cpp(
                [create_ddl_data] { run_internal_ddl(create_ddl_data); });
            const std::string storage_ddl = fmt::format(
                "ALTER TABLE bioseq_internal.{} ALTER COLUMN blob SET STORAGE EXTERNAL;",
                quoted_name);
            const char* storage_ddl_data = storage_ddl.c_str();
            bioseq::pg_utils::invoke_pg_as_cpp(
                [storage_ddl_data] { run_internal_ddl(storage_ddl_data); });
            rid = bioseq::pg_utils::invoke_pg_as_cpp(
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
        constexpr std::array<Rev3IndexAttributeDescriptor, 5>
            legacy_descriptors{{
                {kAttQueryKey, "query_key", INT8OID},
                {kAttSubKey, "sub_key", INT8OID},
                {kAttChunkNo, "chunk_no", INT4OID},
                {kAttChunkBytes, "chunk_bytes", INT4OID},
                {kAttBlob, "blob", BYTEAOID},
            }};
        const Oid expected_namespace = bioseq_internal_ns();
        const char* expected_name_data = expected_name.c_str();
        const Rev3IndexAttributeDescriptor* descriptors =
            legacy_descriptors.data();
        const std::size_t descriptor_count = legacy_descriptors.size();
        LegacyFragmentReadResources resources{lock};
        try {
            resources.relation = bioseq::pg_utils::invoke_pg_as_cpp(
                [rid, lock] { return try_safe_relation_open(rid, lock); });
            if (resources.relation == nullptr) {
                return resources.release_relation();
            }

            // Return 1 for a physical-identity mismatch and 2 for a schema
            // mismatch. The leaf owns no C++ formatting state.
            const std::uint8_t invalid_reason =
                bioseq::pg_utils::invoke_pg_as_cpp(
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

                        // This is the legacy five-column surface. Never apply
                        // these attnos to the canonical generation relation.
                        TupleDesc tuple_desc = RelationGetDescr(relation);
                        if (tuple_desc == nullptr
                            || tuple_desc->natts
                                != static_cast<int>(descriptor_count)) {
                            return std::uint8_t{2};
                        }
                        for (std::size_t attribute_index = 0;
                             attribute_index < descriptor_count;
                             ++attribute_index) {
                            const Rev3IndexAttributeDescriptor* expected =
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
                        "legacy fragment API refuses non-legacy schema for {}",
                        expected_name);
                index_error(error);
            }
            return resources.release_relation();
        } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
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
        std::int64_t* replacement = bioseq::pg_utils::invoke_pg_as_cpp(
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
        std::int64_t* replacement = bioseq::pg_utils::invoke_pg_as_cpp(
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

    [[nodiscard]] IndexChunkKey chunk_cache_key_(
            index_sub_key_t sub_key,
            std::size_t chunk_no) const noexcept {
        return IndexChunkKey{
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
        const IndexChunkKey key = chunk_cache_key_(sub_key, chunk_no);
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

        LegacyFragmentReadResources resources{AccessShareLock};
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
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [relation = resources.relation] {
                        return find_primary_key_oid(relation);
                    });
            std::array<ScanKeyData, 3> keys{};
            const Datum query_key = bioseq::pg_utils::pg_datum::from(
                static_cast<std::int64_t>(query_key_)).raw();
            const Datum stored_sub_key = bioseq::pg_utils::pg_datum::from(
                static_cast<std::int64_t>(sub_key)).raw();
            const Datum stored_chunk_no = bioseq::pg_utils::pg_datum::from(
                static_cast<std::int32_t>(chunk_no)).raw();
            ScanKeyData* key_data = keys.data();
            bioseq::pg_utils::invoke_pg_as_cpp(
                [key_data, query_key, stored_sub_key, stored_chunk_no] {
                    ScanKeyInit(
                        &key_data[0], kAttQueryKey, BTEqualStrategyNumber,
                        F_INT8EQ, query_key);
                    ScanKeyInit(
                        &key_data[1], kAttSubKey, BTEqualStrategyNumber,
                        F_INT8EQ, stored_sub_key);
                    ScanKeyInit(
                        &key_data[2], kAttChunkNo, BTEqualStrategyNumber,
                        F_INT4EQ, stored_chunk_no);
                });
            resources.scan = bioseq::pg_utils::invoke_pg_as_cpp(
                [relation = resources.relation, primary_index, key_data] {
                    return systable_beginscan(
                        relation, primary_index,
                        OidIsValid(primary_index), nullptr, 3, key_data);
                });
            HeapTuple tuple = bioseq::pg_utils::invoke_pg_as_cpp(
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
            const Datum blob = bioseq::pg_utils::invoke_pg_as_cpp(
                [relation = resources.relation, tuple, &is_null] {
                    return heap_getattr(
                        tuple, kAttBlob, RelationGetDescr(relation),
                        &is_null);
                });
            if (is_null) {
                index_error("fragment chunk blob is NULL");
            }
            auto detoasted = bioseq::pg_utils::invoke_pg_as_cpp(
                [blob] {
                    return bioseq::pg_utils::detoast_packed_as<bytea>(
                        bioseq::pg_utils::pg_datum::from_raw(blob));
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
        } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
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
        LegacyFragmentReadResources resources{AccessShareLock};
        try {
            resources.relation = open_index_relation_(
                false, AccessShareLock);
            if (resources.relation == nullptr) return 0;
            const Oid primary_index =
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [relation = resources.relation] {
                        return find_primary_key_oid(relation);
                    });
            std::array<ScanKeyData, 2> keys{};
            ScanKeyData* key_data = keys.data();
            const Datum query_key = bioseq::pg_utils::pg_datum::from(
                static_cast<std::int64_t>(query_key_)).raw();
            const Datum data_sub_key_limit =
                bioseq::pg_utils::pg_datum::from(
                    static_cast<std::int64_t>(TEMP_RUN_BASE)).raw();
            bioseq::pg_utils::invoke_pg_as_cpp(
                [key_data, query_key, data_sub_key_limit] {
                    ScanKeyInit(
                        &key_data[0], kAttQueryKey, BTEqualStrategyNumber,
                        F_INT8EQ, query_key);
                    ScanKeyInit(
                        &key_data[1], kAttSubKey, BTLessStrategyNumber,
                        F_INT8LT, data_sub_key_limit);
                });
            resources.scan = bioseq::pg_utils::invoke_pg_as_cpp(
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
                HeapTuple tuple = bioseq::pg_utils::invoke_pg_as_cpp(
                    [scan = resources.scan] {
                        return systable_getnext(scan);
                    });
                if (!HeapTupleIsValid(tuple)) break;
                bool is_null = false;
                const Datum sub_key_datum =
                    bioseq::pg_utils::invoke_pg_as_cpp(
                        [relation = resources.relation, tuple, &is_null] {
                            return heap_getattr(
                                tuple, kAttSubKey,
                                RelationGetDescr(relation), &is_null);
                        });
                if (is_null) index_error("fragment sub-key is NULL");
                const std::int64_t sub_key =
                    bioseq::pg_utils::pg_datum::to<std::int64_t>(
                        bioseq::pg_utils::pg_datum::from_raw(
                            sub_key_datum));
                if (!have_last || sub_key != last) {
                    ++count;
                    last = sub_key;
                    have_last = true;
                }
            }
            resources.close_checked();
            return count;
        } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
            resources.abandon_after_pg_error_noexcept();
            throw;
        } catch (...) {
            resources.close_then_rethrow_cpp(std::current_exception());
        }
    }
    [[nodiscard]] std::size_t number_of_chunks(index_sub_key_t sub_key) const {
        LegacyFragmentReadResources resources{AccessShareLock};
        try {
            resources.relation = open_index_relation_(
                false, AccessShareLock);
            if (resources.relation == nullptr) return 0;
            const Oid primary_index =
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [relation = resources.relation] {
                        return find_primary_key_oid(relation);
                    });
            std::array<ScanKeyData, 2> keys{};
            ScanKeyData* key_data = keys.data();
            const Datum query_key = bioseq::pg_utils::pg_datum::from(
                static_cast<std::int64_t>(query_key_)).raw();
            const Datum stored_sub_key = bioseq::pg_utils::pg_datum::from(
                static_cast<std::int64_t>(sub_key)).raw();
            bioseq::pg_utils::invoke_pg_as_cpp(
                [key_data, query_key, stored_sub_key] {
                    ScanKeyInit(
                        &key_data[0], kAttQueryKey, BTEqualStrategyNumber,
                        F_INT8EQ, query_key);
                    ScanKeyInit(
                        &key_data[1], kAttSubKey, BTEqualStrategyNumber,
                        F_INT8EQ, stored_sub_key);
                });
            resources.scan = bioseq::pg_utils::invoke_pg_as_cpp(
                [relation = resources.relation, primary_index, key_data] {
                    return systable_beginscan(
                        relation, primary_index,
                        OidIsValid(primary_index), nullptr, 2, key_data);
                });
            std::size_t count = 0;
            while (bioseq::pg_utils::invoke_pg_as_cpp(
                    [scan = resources.scan] {
                        return systable_getnext(scan);
                    }) != nullptr) {
                ++count;
            }
            resources.close_checked();
            return count;
        } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
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
        LegacyFragmentReadResources resources{AccessShareLock};
        try {
            resources.relation = open_index_relation_(
                false, AccessShareLock);
            if (resources.relation == nullptr) return 0;
            const Oid primary_index =
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [relation = resources.relation] {
                        return find_primary_key_oid(relation);
                    });
            std::array<ScanKeyData, 3> keys{};
            ScanKeyData* key_data = keys.data();
            const Datum query_key = bioseq::pg_utils::pg_datum::from(
                static_cast<std::int64_t>(query_key_)).raw();
            const Datum stored_sub_key = bioseq::pg_utils::pg_datum::from(
                static_cast<std::int64_t>(sub_key)).raw();
            const Datum stored_chunk_no = bioseq::pg_utils::pg_datum::from(
                static_cast<std::int32_t>(chunk_no)).raw();
            bioseq::pg_utils::invoke_pg_as_cpp(
                [key_data, query_key, stored_sub_key, stored_chunk_no] {
                    ScanKeyInit(
                        &key_data[0], kAttQueryKey, BTEqualStrategyNumber,
                        F_INT8EQ, query_key);
                    ScanKeyInit(
                        &key_data[1], kAttSubKey, BTEqualStrategyNumber,
                        F_INT8EQ, stored_sub_key);
                    ScanKeyInit(
                        &key_data[2], kAttChunkNo, BTEqualStrategyNumber,
                        F_INT4EQ, stored_chunk_no);
                });
            resources.scan = bioseq::pg_utils::invoke_pg_as_cpp(
                [relation = resources.relation, primary_index, key_data] {
                    return systable_beginscan(
                        relation, primary_index,
                        OidIsValid(primary_index), nullptr, 3, key_data);
                });
            HeapTuple tuple = bioseq::pg_utils::invoke_pg_as_cpp(
                [scan = resources.scan] {
                    return systable_getnext(scan);
                });
            std::size_t result = 0;
            if (HeapTupleIsValid(tuple)) {
                bool is_null = false;
                const Datum chunk_bytes_datum =
                    bioseq::pg_utils::invoke_pg_as_cpp(
                        [relation = resources.relation, tuple, &is_null] {
                            return heap_getattr(
                                tuple, kAttChunkBytes,
                                RelationGetDescr(relation), &is_null);
                        });
                if (is_null) index_error("fragment chunk byte count is NULL");
                const std::int32_t signed_result =
                    bioseq::pg_utils::pg_datum::to<std::int32_t>(
                        bioseq::pg_utils::pg_datum::from_raw(
                            chunk_bytes_datum));
                if (signed_result < 0) {
                    index_error("fragment chunk byte count is negative");
                }
                result = static_cast<std::size_t>(signed_result);
            }
            resources.close_checked();
            return result;
        } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
            resources.abandon_after_pg_error_noexcept();
            throw;
        } catch (...) {
            resources.close_then_rethrow_cpp(std::current_exception());
        }
    }

    template <class T>
    [[nodiscard]] const T* read_chunk(index_sub_key_t sub_key, std::size_t chunk_no) {
        static_assert(alignof(T) <= alignof(std::int64_t), "read_chunk<T>: T must be int64-aligned-compatible");
        if (decoded_ != nullptr && decoded_sub_ == sub_key && decoded_chunk_ == chunk_no)
            return reinterpret_cast<const T*>(decoded_);
        load_chunk_into_buffer_(sub_key, chunk_no);
        return reinterpret_cast<const T*>(decoded_);
    }

    void resize(index_sub_key_t sub_key, std::size_t bytes_count) {
        const std::size_t n = bytes_count == 0
            ? 1
            : 1 + (bytes_count - 1) / MAX_INDEX_CHUNK_SIZE_BYTES;
        std::vector<std::size_t> plan(n);
        std::size_t rem = bytes_count;
        for (std::size_t i = 0; i < n; ++i) { const std::size_t c = std::min(rem, MAX_INDEX_CHUNK_SIZE_BYTES); plan[i] = c; rem -= c; }
        chunk_payload_bytes_[sub_key] = std::move(plan);
    }
    void resize_chunks(index_sub_key_t sub_key, std::span<const std::size_t> per_chunk_bytes) {
        std::vector<std::size_t> plan(per_chunk_bytes.begin(), per_chunk_bytes.end());
        for (std::size_t bytes : plan) {
            if (bytes > MAX_INDEX_CHUNK_SIZE_BYTES) {
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
            LegacyFragmentWriteResources& resources,
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
        const Oid primary_index = bioseq::pg_utils::invoke_pg_as_cpp(
            [relation] { return find_primary_key_oid(relation); });
        if (!OidIsValid(primary_index)) {
            index_error("fragment write relation has no primary index");
        }

        std::array<ScanKeyData, 3> keys{};
        ScanKeyData* key_data = keys.data();
        const Datum stored_query_key = bioseq::pg_utils::pg_datum::from(
            static_cast<std::int64_t>(query_key)).raw();
        const Datum stored_sub_key = bioseq::pg_utils::pg_datum::from(
            static_cast<std::int64_t>(sub_key)).raw();
        const Datum stored_chunk_no = bioseq::pg_utils::pg_datum::from(
            static_cast<std::int32_t>(chunk_no)).raw();
        bioseq::pg_utils::invoke_pg_as_cpp(
            [key_data, stored_query_key, stored_sub_key, stored_chunk_no] {
                ScanKeyInit(
                    &key_data[0], kAttQueryKey, BTEqualStrategyNumber,
                    F_INT8EQ, stored_query_key);
                ScanKeyInit(
                    &key_data[1], kAttSubKey, BTEqualStrategyNumber,
                    F_INT8EQ, stored_sub_key);
                ScanKeyInit(
                    &key_data[2], kAttChunkNo, BTEqualStrategyNumber,
                    F_INT4EQ, stored_chunk_no);
            });
        resources.scan = bioseq::pg_utils::invoke_pg_as_cpp(
            [relation, primary_index, key_data] {
                return systable_beginscan(
                    relation, primary_index, true, nullptr, 3, key_data);
            });
        HeapTuple previous = bioseq::pg_utils::invoke_pg_as_cpp(
            [scan = resources.scan] { return systable_getnext(scan); });
        if (HeapTupleIsValid(previous)) {
            ItemPointerData previous_tid = previous->t_self;
            bioseq::pg_utils::invoke_pg_as_cpp(
                [relation, previous_tid]() mutable {
                    simple_heap_delete(relation, &previous_tid);
                });
        }
        resources.finish_scan_checked();

        const std::size_t allocation_bytes = VARHDRSZ + payload.size();
        resources.blob = bioseq::pg_utils::invoke_pg_as_cpp(
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
            bioseq::pg_utils::pg_datum::from(
                static_cast<std::int32_t>(chunk_bytes)).raw(),
            bioseq::pg_utils::pg_datum::from_pointer(resources.blob).raw(),
        };
        std::array<bool, 5> nulls{};
        Datum* value_data = values.data();
        bool* null_data = nulls.data();
        resources.tuple = bioseq::pg_utils::invoke_pg_as_cpp(
            [relation, value_data, null_data] {
                return heap_form_tuple(
                    RelationGetDescr(relation), value_data, null_data);
            });
        bioseq::pg_utils::invoke_pg_as_cpp(
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
        LegacyFragmentWriteResources resources{RowExclusiveLock};
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
        } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
            resources.abandon_after_pg_error_noexcept();
            throw;
        } catch (...) {
            resources.close_then_rethrow_cpp(std::current_exception());
        }
    }

    template <class T, class F>
    void write_to_each(index_sub_key_t sub_key, F&& writer) {
        static_assert(bioseq::indexing::index_chunk_writer_fn<F, T>,
            "write_to_each<T>: writer must be (size_t, T*, size_t) -> size_t");
        auto it = chunk_payload_bytes_.find(sub_key);
        if (it == chunk_payload_bytes_.end()) {
            index_error("fragment write_to_each has no preceding resize");
        }
        const std::vector<std::size_t>& plan = it->second;
        const std::size_t n = plan.size();
        std::size_t max_payload = 0;
        for (const std::size_t payload_bytes : plan) {
            if (payload_bytes > MAX_INDEX_CHUNK_SIZE_BYTES) {
                index_error("fragment write plan exceeds MAX chunk size");
            }
            if (payload_bytes % sizeof(T) != 0) {
                index_error("fragment write payload is not a whole number of elements");
            }
            max_payload = std::max(max_payload, payload_bytes);
        }
        const std::size_t scratch_units = max_payload == 0
            ? 1
            : 1 + (max_payload - 1) / sizeof(std::int64_t);
        std::int64_t* buffer = scratch_buffer_(scratch_units);

        LegacyFragmentWriteResources resources{RowExclusiveLock};
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
                const std::size_t capacity = payload_bytes / sizeof(T);
                if (payload_bytes > 0) {
                    std::memset(buffer, 0, payload_bytes);
                }
                const std::size_t next = writer(
                    chunk_no, reinterpret_cast<T*>(buffer), capacity);
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
        } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
            resources.abandon_after_pg_error_noexcept();
            throw;
        } catch (...) {
            resources.close_then_rethrow_cpp(std::current_exception());
        }
    }

    template <class F> void read_chunks_in_range(index_sub_key_t lo, index_sub_key_t hi, F&& cb) {
        if (hi <= lo) return;
        LegacyFragmentReadResources resources{AccessShareLock};
        try {
            resources.relation = open_index_relation_(
                false, AccessShareLock);
            if (resources.relation == nullptr) return;
            const Oid primary_index =
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [relation = resources.relation] {
                        return find_primary_key_oid(relation);
                    });
            if (!OidIsValid(primary_index)) {
                resources.close_checked();
                index_error("range scan has no primary index");
            }
            resources.ordered_index_relation =
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [primary_index] {
                        return index_open(primary_index, AccessShareLock);
                    });
            std::array<ScanKeyData, 3> keys{};
            ScanKeyData* key_data = keys.data();
            const Datum query_key = bioseq::pg_utils::pg_datum::from(
                static_cast<std::int64_t>(query_key_)).raw();
            const Datum first_sub_key = bioseq::pg_utils::pg_datum::from(
                static_cast<std::int64_t>(lo)).raw();
            const Datum last_sub_key = bioseq::pg_utils::pg_datum::from(
                static_cast<std::int64_t>(hi - 1)).raw();
            bioseq::pg_utils::invoke_pg_as_cpp(
                [key_data, query_key, first_sub_key, last_sub_key] {
                    ScanKeyInit(
                        &key_data[0], kAttQueryKey, BTEqualStrategyNumber,
                        F_INT8EQ, query_key);
                    ScanKeyInit(
                        &key_data[1], kAttSubKey,
                        BTGreaterEqualStrategyNumber, F_INT8GE,
                        first_sub_key);
                    ScanKeyInit(
                        &key_data[2], kAttSubKey,
                        BTLessEqualStrategyNumber, F_INT8LE,
                        last_sub_key);
                });
            resources.scan = bioseq::pg_utils::invoke_pg_as_cpp(
                [relation = resources.relation,
                 index_relation = resources.ordered_index_relation,
                 key_data] {
                    return systable_beginscan_ordered(
                        relation, index_relation, nullptr, 3, key_data);
                });
            resources.ordered_scan = true;

            std::vector<char> bytes;
            while (true) {
                HeapTuple tuple = bioseq::pg_utils::invoke_pg_as_cpp(
                    [scan = resources.scan] {
                        return systable_getnext_ordered(
                            scan, ForwardScanDirection);
                    });
                if (!HeapTupleIsValid(tuple)) break;

                bool is_null = false;
                const Datum sub_key_datum =
                    bioseq::pg_utils::invoke_pg_as_cpp(
                        [relation = resources.relation, tuple, &is_null] {
                            return heap_getattr(
                                tuple, kAttSubKey,
                                RelationGetDescr(relation), &is_null);
                        });
                if (is_null) index_error("range row sub-key is NULL");
                const std::int64_t signed_sub_key =
                    bioseq::pg_utils::pg_datum::to<std::int64_t>(
                        bioseq::pg_utils::pg_datum::from_raw(
                            sub_key_datum));

                is_null = false;
                const Datum chunk_no_datum =
                    bioseq::pg_utils::invoke_pg_as_cpp(
                        [relation = resources.relation, tuple, &is_null] {
                            return heap_getattr(
                                tuple, kAttChunkNo,
                                RelationGetDescr(relation), &is_null);
                        });
                if (is_null) index_error("range row chunk number is NULL");
                const std::int32_t signed_chunk_no =
                    bioseq::pg_utils::pg_datum::to<std::int32_t>(
                        bioseq::pg_utils::pg_datum::from_raw(
                            chunk_no_datum));
                if (signed_chunk_no < 0) {
                    index_error("range row chunk number is negative");
                }

                is_null = false;
                const Datum blob = bioseq::pg_utils::invoke_pg_as_cpp(
                    [relation = resources.relation, tuple, &is_null] {
                        return heap_getattr(
                            tuple, kAttBlob,
                            RelationGetDescr(relation), &is_null);
                    });
                if (is_null) index_error("range row blob is NULL");
                auto detoasted = bioseq::pg_utils::invoke_pg_as_cpp(
                    [blob] {
                        return bioseq::pg_utils::detoast_packed_as<bytea>(
                            bioseq::pg_utils::pg_datum::from_raw(blob));
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
        } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
            resources.abandon_after_pg_error_noexcept();
            throw;
        } catch (...) {
            resources.close_then_rethrow_cpp(std::current_exception());
        }
    }

    template <class F> void for_each_sub_key(F&& fn) const {
        LegacyFragmentReadResources resources{AccessShareLock};
        try {
            resources.relation = open_index_relation_(
                false, AccessShareLock);
            if (resources.relation == nullptr) return;
            const Oid primary_index =
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [relation = resources.relation] {
                        return find_primary_key_oid(relation);
                    });
            std::array<ScanKeyData, 2> keys{};
            ScanKeyData* key_data = keys.data();
            const Datum query_key = bioseq::pg_utils::pg_datum::from(
                static_cast<std::int64_t>(query_key_)).raw();
            const Datum data_sub_key_limit =
                bioseq::pg_utils::pg_datum::from(
                    static_cast<std::int64_t>(TEMP_RUN_BASE)).raw();
            bioseq::pg_utils::invoke_pg_as_cpp(
                [key_data, query_key, data_sub_key_limit] {
                    ScanKeyInit(
                        &key_data[0], kAttQueryKey, BTEqualStrategyNumber,
                        F_INT8EQ, query_key);
                    ScanKeyInit(
                        &key_data[1], kAttSubKey, BTLessStrategyNumber,
                        F_INT8LT, data_sub_key_limit);
                });
            resources.scan = bioseq::pg_utils::invoke_pg_as_cpp(
                [relation = resources.relation, primary_index, key_data] {
                    return systable_beginscan(
                        relation, primary_index,
                        OidIsValid(primary_index), nullptr, 2, key_data);
                });
            std::int64_t last =
                (std::numeric_limits<std::int64_t>::min)();
            bool have_last = false;
            while (true) {
                HeapTuple tuple = bioseq::pg_utils::invoke_pg_as_cpp(
                    [scan = resources.scan] {
                        return systable_getnext(scan);
                    });
                if (!HeapTupleIsValid(tuple)) break;
                bool is_null = false;
                const Datum sub_key_datum =
                    bioseq::pg_utils::invoke_pg_as_cpp(
                        [relation = resources.relation, tuple, &is_null] {
                            return heap_getattr(
                                tuple, kAttSubKey,
                                RelationGetDescr(relation), &is_null);
                        });
                if (is_null) index_error("fragment sub-key is NULL");
                const std::int64_t sub_key =
                    bioseq::pg_utils::pg_datum::to<std::int64_t>(
                        bioseq::pg_utils::pg_datum::from_raw(
                            sub_key_datum));
                if (!have_last || sub_key != last) {
                    fn(static_cast<index_sub_key_t>(sub_key));
                    last = sub_key;
                    have_last = true;
                }
            }
            resources.close_checked();
        } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
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
            index_error("legacy fragment delete query key exceeds PostgreSQL BIGINT");
        }
        // Opening through the legacy validator is intentional: a canonical
        // generation table has query_key/sub_key columns too, so name-based
        // DELETE SQL could otherwise erase immutable generations while
        // bypassing every old-attno check.
        LegacyFragmentWriteResources resources{RowExclusiveLock};
        try {
            resources.relation = open_index_relation_(
                false, RowExclusiveLock);
            if (resources.relation == nullptr) return;
            Relation relation = resources.relation;
            const Oid primary_index = bioseq::pg_utils::invoke_pg_as_cpp(
                [relation] { return find_primary_key_oid(relation); });
            if (!OidIsValid(primary_index)) {
                resources.close_checked();
                index_error("legacy fragment delete requires a primary index");
            }

            std::array<ScanKeyData, 3> keys{};
            ScanKeyData* key_data = keys.data();
            const Datum query_key = bioseq::pg_utils::pg_datum::from(
                static_cast<std::int64_t>(query_key_)).raw();
            const bool bounded = inclusive_sub_key_range.has_value();
            const Datum first_sub_key = bounded
                ? bioseq::pg_utils::pg_datum::from(
                    static_cast<std::int64_t>(
                        inclusive_sub_key_range->first)).raw()
                : Datum{0};
            const Datum last_sub_key = bounded
                ? bioseq::pg_utils::pg_datum::from(
                    static_cast<std::int64_t>(
                        inclusive_sub_key_range->second)).raw()
                : Datum{0};
            bioseq::pg_utils::invoke_pg_as_cpp(
                [key_data, query_key, bounded,
                 first_sub_key, last_sub_key] {
                    ScanKeyInit(
                        &key_data[0], kAttQueryKey,
                        BTEqualStrategyNumber, F_INT8EQ, query_key);
                    if (bounded) {
                        ScanKeyInit(
                            &key_data[1], kAttSubKey,
                            BTGreaterEqualStrategyNumber, F_INT8GE,
                            first_sub_key);
                        ScanKeyInit(
                            &key_data[2], kAttSubKey,
                            BTLessEqualStrategyNumber, F_INT8LE,
                            last_sub_key);
                    }
                });
            const int key_count = bounded ? 3 : 1;
            resources.scan = bioseq::pg_utils::invoke_pg_as_cpp(
                [relation, primary_index, key_count, key_data] {
                    return systable_beginscan(
                        relation, primary_index, true, nullptr,
                        key_count, key_data);
                });
            while (true) {
                HeapTuple tuple = bioseq::pg_utils::invoke_pg_as_cpp(
                    [scan = resources.scan] {
                        return systable_getnext(scan);
                    });
                if (!HeapTupleIsValid(tuple)) break;
                ItemPointerData tuple_id = tuple->t_self;
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [relation, tuple_id]() mutable {
                        simple_heap_delete(relation, &tuple_id);
                    });
            }
            resources.close_checked();
        } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
            resources.abandon_after_pg_error_noexcept();
            throw;
        } catch (...) {
            resources.close_then_rethrow_cpp(std::current_exception());
        }
        bioseq::pg_utils::invoke_pg_as_cpp(
            [] { CommandCounterIncrement(); });
        evict_generation_storage_residency_();
    }
    void clear_entity() {
        delete_where_(std::nullopt);
        decoded_sub_ = DIRECTORY_SUB_KEY;
        decoded_chunk_ = (std::numeric_limits<std::size_t>::max)();
        decoded_bytes_ = 0;
    }
    void discard_sub_keys(index_sub_key_t lo, index_sub_key_t hi) {
        delete_where_(std::pair{lo, hi});
    }
    void clear(std::string_view name); // index-wide drop across all shards (defined below, uses seq_sub_table enumeration)

    [[nodiscard]] std::size_t run_buffer_bytes() const {
        if (run_buffer_override_bytes_ > 0) return run_buffer_override_bytes_;
        constexpr std::uint64_t floor = std::uint64_t{64} << 20;
        constexpr std::uint64_t ceil = std::uint64_t{8} << 30;
        const std::uint64_t desired = std::uint64_t{2} << 30;
        std::uint64_t capped =
            bioseq::memory::default_budget().clamp_working_set(desired, floor, ceil);
        const std::uint64_t index_cap =
            bioseq::parallelism::MemoryAdmissionController::instance().index_memory_cap_bytes();
        if (index_cap != 0) {
            capped = std::min(capped, index_cap);
        }
        return static_cast<std::size_t>(capped);
    }

    // ---- external sort spill (BufFile-backed sorted runs) ----
    [[nodiscard]] SortedPairRunStore& sorted_runs_ensure_() {
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
	        bioseq::parallelism::MemoryPermit permit);
	    void demote_resident_runs_until_available(std::uint64_t bytes_needed);
	    template <class F> void merge_runs(F&& on_kmer);
	    std::size_t spill_sorted_segment_run(
	        std::uint32_t position_segment,
	        std::span<const std::pair<std::uint64_t,std::uint64_t>> sorted);
	    bool reroute_segment_runs(
	        std::uint64_t old_span,
	        std::uint64_t new_span,
	        std::uint64_t target_length);
	    template <class F> void merge_segment_runs(F&& on_segment_kmer);
	    [[nodiscard]] MpsSegmentSpillStorageBinding
	    begin_segment_spill_generation();
	    [[nodiscard]] MpsSegmentSpillStorageBinding
	    segment_spill_storage_binding() const noexcept {
	        return segment_spill_binding_;
	    }
	    [[nodiscard]] SortedRunStoreStats sorted_run_stats() const noexcept;
    void snapshot_sorted_run_stats_before_release();
    void release_runs_noexcept() noexcept;
    void release_runs();
    void close_checked();
    void abandon_after_pg_error_noexcept() noexcept;
};

// A bounded position cursor over one kmer's unique positions (one vector).
struct PositionBatchCursor {
    const std::vector<std::uint64_t>* v = nullptr;
    [[nodiscard]] std::uint64_t total_count() const { return v->size(); }
    template <class F> void for_each_batch(F&& f) const {
        if (!v->empty()) f(std::span<const std::uint64_t>(v->data(), v->size()));
    }
};

// =========================================================================
//  IndexStorageSequencePg  —  INLINE_PER_SEQUENCE
// =========================================================================
struct IndexStorageSequencePg {
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

    IndexStorageSequencePg() = default;
    IndexStorageSequencePg(const IndexStorageSequencePg&) = delete;
    IndexStorageSequencePg& operator=(const IndexStorageSequencePg&) = delete;
    IndexStorageSequencePg(IndexStorageSequencePg&& o) noexcept { steal_(std::move(o)); }
    IndexStorageSequencePg& operator=(IndexStorageSequencePg&&) = delete;
    ~IndexStorageSequencePg() { release_(); }
    void steal_(IndexStorageSequencePg&& o) noexcept {
        db_oid_=o.db_oid_; tablespace_oid_=o.tablespace_oid_; main_relfilenode_=o.main_relfilenode_; user_relid_=o.user_relid_;
        read_caches_=o.read_caches_; column_attno_=o.column_attno_; index_name_=o.index_name_;
        bucket_idx_=o.bucket_idx_; shard_idx_=o.shard_idx_; query_key_=o.query_key_;
        own_mctx_=o.own_mctx_; index_relid_=o.index_relid_; decoded_=o.decoded_; decoded_units_=o.decoded_units_;
        decoded_bytes_=o.decoded_bytes_; decoded_valid_=o.decoded_valid_; planned_bytes_=o.planned_bytes_;
        o.own_mctx_=nullptr; o.decoded_=nullptr; o.decoded_units_=0; o.index_relid_=InvalidOid; o.decoded_valid_=false;
    }
    void release_() noexcept {
        DEBUG_ASSERT(own_mctx_ == nullptr,
            "IndexStorageSequencePg must be close_checked or abandoned before destruction");
        own_mctx_ = nullptr;
        decoded_ = nullptr;
    }
    void close_checked() {
        if (own_mctx_ != nullptr) {
            MemoryContext finished_context = std::exchange(
                own_mctx_, nullptr);
            decoded_ = nullptr;
            bioseq::pg_utils::invoke_pg_as_cpp(
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
		bioseq::pg_utils::invoke_pg_as_cpp([msg] {
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
        return try_safe_relation_open(rid, lock);
    }
	[[nodiscard]] Oid resolve_relid_for_read_checked_() const {
		if (OidIsValid(index_relid_)) {
			return index_relid_;
		}
		// Build/destruct the C++ name outside every PG_TRY leaf. A PostgreSQL
		// longjmp from namespace/relation lookup is converted before this
		// ordinary C++ frame unwinds.
		const std::string table_name = this_table_name();
		const Oid namespace_oid = bioseq::pg_utils::invoke_pg_as_cpp(
			[] { return bioseq_internal_ns(); });
		const Oid relation_oid = bioseq::pg_utils::invoke_pg_as_cpp(
			[&table_name, namespace_oid] {
				inject_inline_sequence_pg_leaf_failure(
					InlineSequencePgLeafFailure::relation_lookup);
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
		return bioseq::pg_utils::invoke_pg_as_cpp(
			[relation_oid] {
				return try_safe_relation_open(
					relation_oid, AccessShareLock);
			});
	}
	void ensure_buffer_checked_(const std::size_t units) const {
		if (decoded_units_ >= units && decoded_ != nullptr) {
			return;
		}
		MemoryContext owner = own_mctx_;
		std::int64_t* old_buffer = decoded_;
		std::int64_t* replacement = bioseq::pg_utils::invoke_pg_as_cpp(
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
		const Oid pk = bioseq::pg_utils::invoke_pg_as_cpp(
			[rel] { return find_primary_key_oid(rel); });
		ScanKeyData key{};
		const Datum query_key = bioseq::pg_utils::pg_datum::from(
			static_cast<std::int64_t>(query_key_)).raw();
		bioseq::pg_utils::invoke_pg_as_cpp([&key, query_key] {
			ScanKeyInit(&key, 1, BTEqualStrategyNumber, F_INT8EQ,
				query_key);
		});
		SysScanDesc scan = bioseq::pg_utils::invoke_pg_as_cpp(
			[rel, pk, &key] {
				return systable_beginscan(
					rel, pk, OidIsValid(pk), nullptr, 1, &key);
			});
		HeapTuple tuple = bioseq::pg_utils::invoke_pg_as_cpp(
			[scan] {
				inject_inline_sequence_pg_leaf_failure(
					InlineSequencePgLeafFailure::tuple_read);
				return systable_getnext(scan);
			});
		if (HeapTupleIsValid(tuple)) {
			bool is_null = false;
			TupleDesc tuple_desc = RelationGetDescr(rel);
			Datum datum = bioseq::pg_utils::invoke_pg_as_cpp(
				[tuple, tuple_desc, &is_null] {
					return heap_getattr(tuple, 3, tuple_desc, &is_null);
				});
			if (is_null) {
				index_error("inline sequence index blob is NULL");
			}
			auto raw_detoasted = bioseq::pg_utils::invoke_pg_as_cpp(
				[datum] {
					inject_inline_sequence_pg_leaf_failure(
						InlineSequencePgLeafFailure::detoast);
					return bioseq::pg_utils::detoast_packed_as<bytea>(
						bioseq::pg_utils::pg_datum::from_raw(datum));
				});
            bytea* raw = raw_detoasted.ptr;
            const std::size_t payload = VARSIZE_ANY_EXHDR(raw);
            const std::size_t units = (payload + sizeof(std::int64_t) - 1)/sizeof(std::int64_t);
			ensure_buffer_checked_(units == 0 ? 1 : units);
            if (payload>0) std::memcpy(decoded_, VARDATA_ANY(raw), payload);
			bioseq::pg_utils::invoke_pg_as_cpp(
				[&raw_detoasted] { raw_detoasted.free_if_copy(); });
            decoded_bytes_ = payload;
        } else decoded_bytes_ = 0;
		bioseq::pg_utils::invoke_pg_as_cpp(
			[scan] { systable_endscan(scan); });
		bioseq::pg_utils::invoke_pg_as_cpp(
			[rel] {
				inject_inline_sequence_pg_leaf_failure(
					InlineSequencePgLeafFailure::close);
				relation_close(rel, AccessShareLock);
			});
		decoded_valid_ = true;
    }
    [[nodiscard]] std::size_t number_of_chunks() const { return chunk_byte_size() == 0 ? 0 : 1; }
    [[nodiscard]] std::size_t chunk_byte_size() const { if (!decoded_valid_) load_(); return decoded_bytes_; }
    template <class T> [[nodiscard]] const T* read_chunk() { if (!decoded_valid_) load_(); return reinterpret_cast<const T*>(decoded_); }
    void resize(std::size_t bytes) { if (bytes > MAX_INDEX_CHUNK_SIZE_BYTES) bioseq_log<ERR, PG>( "bioseq index inline: bytes exceed single-chunk MAX"); planned_bytes_ = bytes; }
    void prefetch_chunk() {}
    template <class T, class F> void write_to_each(F&& writer) {
        static_assert(bioseq::indexing::index_chunk_writer_fn<F, T>, "inline write_to_each writer shape");
        if (planned_bytes_ == std::numeric_limits<std::size_t>::max()) bioseq_log<ERR, PG>( "bioseq index inline: write_to_each without resize");
        const std::size_t payload = planned_bytes_;
        if (payload % sizeof(T) != 0) bioseq_log<ERR, PG>( "bioseq index inline: payload not whole T");
        std::vector<char> buf(payload, 0);
        const std::size_t next = writer(0, reinterpret_cast<T*>(buf.data()), payload / sizeof(T));
        if (next != 0) bioseq_log<ERR, PG>( "bioseq index inline: writer emitted >1 chunk");
        Relation rel = open_(true, RowExclusiveLock);
        // upsert (delete then insert) so rebuild replaces the single row.
        { const Oid pk = find_primary_key_oid(rel);
          ScanKeyData k[1]; ScanKeyInit(&k[0],1,BTEqualStrategyNumber,F_INT8EQ,
              bioseq::pg_utils::pg_datum::from(static_cast<std::int64_t>(query_key_)).raw());
          SysScanDesc s = systable_beginscan(rel, pk, OidIsValid(pk), nullptr, 1, k);
          HeapTuple old_t = systable_getnext(s);
          if (HeapTupleIsValid(old_t)) simple_heap_delete(rel, &old_t->t_self);
          systable_endscan(s); }
        bytea* blob = reinterpret_cast<bytea*>(palloc(VARHDRSZ + payload));
        SET_VARSIZE(blob, VARHDRSZ + payload); if (payload>0) std::memcpy(VARDATA(blob), buf.data(), payload);
        bioseq::pg_utils::pg_datum_list<3> values{};
        values.set(1, static_cast<std::int64_t>(query_key_));
        values.set(2, static_cast<std::int32_t>(payload));
        values.set_raw(3, bioseq::pg_utils::pg_datum::from_pointer(blob));
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
    /// pg-only reuse hook (§8.3) — reposition without re-allocating own_mctx_.
    void reset_entity(const seq_locator& loc, std::uint64_t sequence_rowno) {
        bucket_idx_ = (std::uint8_t)loc.bucket_idx; shard_idx_ = loc.shard_idx; query_key_ = sequence_rowno;
        index_relid_ = InvalidOid; decoded_valid_ = false; decoded_bytes_ = 0;
    }
};

// =========================================================================
//  IndexStorageTablePg  —  CHUNKED_PER_TABLE
// =========================================================================
struct IndexStorageTablePg {
    Oid db_oid_ = InvalidOid, tablespace_oid_ = InvalidOid, main_relfilenode_ = InvalidOid, user_relid_ = InvalidOid;
    std::int16_t column_attno_ = 0;
    std::string_view index_name_{};
    seq_caches* read_caches_ = nullptr;
    MemoryContext own_mctx_ = nullptr;
    mutable Oid index_relid_ = InvalidOid;
    mutable std::int64_t* decoded_ = nullptr; mutable std::size_t decoded_units_=0, decoded_bytes_=0;
    mutable std::int64_t decoded_chunk_ = -1;
    std::size_t planned_bytes_for_chunk_ = 0;

    IndexStorageTablePg() = default;
    IndexStorageTablePg(const IndexStorageTablePg&) = delete;
    IndexStorageTablePg& operator=(const IndexStorageTablePg&) = delete;
    IndexStorageTablePg(IndexStorageTablePg&& o) noexcept { steal_(std::move(o)); }
    IndexStorageTablePg& operator=(IndexStorageTablePg&&) = delete;
    ~IndexStorageTablePg() { release_(); }
    void steal_(IndexStorageTablePg&& o) noexcept {
        db_oid_=o.db_oid_; tablespace_oid_=o.tablespace_oid_; main_relfilenode_=o.main_relfilenode_; user_relid_=o.user_relid_;
        column_attno_=o.column_attno_; index_name_=o.index_name_; read_caches_=o.read_caches_; own_mctx_=o.own_mctx_;
        index_relid_=o.index_relid_; decoded_=o.decoded_; decoded_units_=o.decoded_units_; decoded_bytes_=o.decoded_bytes_;
        decoded_chunk_=o.decoded_chunk_; planned_bytes_for_chunk_=o.planned_bytes_for_chunk_;
        o.own_mctx_=nullptr; o.decoded_=nullptr; o.index_relid_=InvalidOid; o.decoded_units_=0;
    }
    void release_() noexcept {
        DEBUG_ASSERT(own_mctx_ == nullptr,
            "IndexStorageTablePg must be close_checked or abandoned before destruction");
        own_mctx_ = nullptr;
        decoded_ = nullptr;
    }
    void close_checked() {
        if (own_mctx_ != nullptr) {
            MemoryContext finished_context = std::exchange(
                own_mctx_, nullptr);
            decoded_ = nullptr;
            bioseq::pg_utils::invoke_pg_as_cpp(
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
    [[noreturn]] void index_error(std::string_view msg) const { bioseq_log<ERR, PG>( "bioseq index: {}", msg); abort(); }

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
    [[nodiscard]] Relation open_(bool create, LOCKMODE lock) const { const Oid rid = resolve_relid_(create); if (!OidIsValid(rid)) return nullptr; return try_safe_relation_open(rid, lock); }
    void ensure_buffer_(std::size_t units) const {
        if (decoded_units_>=units && decoded_!=nullptr) return;
        MemoryContext old = MemoryContextSwitchTo(own_mctx_);
        decoded_ = (decoded_==nullptr)?(std::int64_t*)palloc(units*sizeof(std::int64_t)):(std::int64_t*)repalloc(decoded_, units*sizeof(std::int64_t));
        decoded_units_=units; MemoryContextSwitchTo(old);
    }
    [[nodiscard]] std::size_t number_of_chunks() const {
        Relation rel = open_(false, AccessShareLock); if (rel==nullptr) return 0;
        SysScanDesc s = systable_beginscan(rel, InvalidOid, false, nullptr, 0, nullptr);
        std::size_t n=0; for (; systable_getnext(s)!=nullptr;) ++n; systable_endscan(s); relation_close(rel, AccessShareLock); return n;
    }
    [[nodiscard]] std::size_t chunk_byte_size(std::size_t chunk_no) const {
        Relation rel = open_(false, AccessShareLock); if (rel==nullptr) return 0;
        const Oid pk = find_primary_key_oid(rel);
        ScanKeyData k[1]; ScanKeyInit(&k[0],1,BTEqualStrategyNumber,F_INT8EQ,
            bioseq::pg_utils::pg_datum::from(static_cast<std::int64_t>(chunk_no)).raw());
        SysScanDesc s = systable_beginscan(rel, pk, OidIsValid(pk), nullptr, 1, k);
        std::size_t out=0; HeapTuple t = systable_getnext(s);
        if (HeapTupleIsValid(t)){ bool isnull=false; out=static_cast<std::size_t>(bioseq::pg_utils::pg_datum::to<std::int32_t>(
            bioseq::pg_utils::pg_datum::from_raw(heap_getattr(t,2,RelationGetDescr(rel),&isnull)))); }
        systable_endscan(s); relation_close(rel, AccessShareLock); return out;
    }
    template <class T> [[nodiscard]] const T* read_chunk(std::size_t chunk_no) {
        if (decoded_!=nullptr && decoded_chunk_==(std::int64_t)chunk_no) return reinterpret_cast<const T*>(decoded_);
        Relation rel = open_(false, AccessShareLock);
        if (rel==nullptr) index_error("table index relation missing");
        const Oid pk = find_primary_key_oid(rel);
        ScanKeyData k[1]; ScanKeyInit(&k[0],1,BTEqualStrategyNumber,F_INT8EQ,
            bioseq::pg_utils::pg_datum::from(static_cast<std::int64_t>(chunk_no)).raw());
        SysScanDesc s = systable_beginscan(rel, pk, OidIsValid(pk), nullptr, 1, k);
        HeapTuple t = systable_getnext(s);
        if (!HeapTupleIsValid(t)){ systable_endscan(s); relation_close(rel,AccessShareLock); index_error("table chunk missing"); }
        bool isnull=false; Datum d = heap_getattr(t,3,RelationGetDescr(rel),&isnull);
        auto raw_detoasted = bioseq::pg_utils::detoast_packed_as<bytea>(
            bioseq::pg_utils::pg_datum::from_raw(d));
        bytea* raw = raw_detoasted.ptr;
        const std::size_t payload = VARSIZE_ANY_EXHDR(raw);
        const std::size_t units = (payload+sizeof(std::int64_t)-1)/sizeof(std::int64_t);
        ensure_buffer_(units==0?1:units); if (payload>0) std::memcpy(decoded_, VARDATA_ANY(raw), payload);
        raw_detoasted.free_if_copy();
        decoded_chunk_=(std::int64_t)chunk_no; decoded_bytes_=payload;
        systable_endscan(s); relation_close(rel, AccessShareLock);
        return reinterpret_cast<const T*>(decoded_);
    }
    void resize(std::size_t bytes) { planned_bytes_for_chunk_ = bytes; }
    void prefetch_chunk(std::size_t) {}
    template <class T, class F> void write_to_each(F&& writer) { write_to<T>(0, std::forward<F>(writer)); }
    template <class T, class F> void write_to(std::size_t chunk_no, F&& writer) {
        static_assert(bioseq::indexing::index_chunk_writer_fn<F, T>, "table write_to writer shape");
        const std::size_t payload = planned_bytes_for_chunk_;
        if (payload % sizeof(T) != 0) bioseq_log<ERR, PG>( "bioseq index table: payload not whole T");
        std::vector<char> buf(payload, 0);
        writer(chunk_no, reinterpret_cast<T*>(buf.data()), payload / sizeof(T));
        Relation rel = open_(true, RowExclusiveLock);
        { const Oid pk = find_primary_key_oid(rel);
          ScanKeyData k[1]; ScanKeyInit(&k[0],1,BTEqualStrategyNumber,F_INT8EQ,
              bioseq::pg_utils::pg_datum::from(static_cast<std::int64_t>(chunk_no)).raw());
          SysScanDesc s = systable_beginscan(rel, pk, OidIsValid(pk), nullptr, 1, k);
          HeapTuple old_t = systable_getnext(s); if (HeapTupleIsValid(old_t)) simple_heap_delete(rel, &old_t->t_self);
          systable_endscan(s); }
        bytea* blob = reinterpret_cast<bytea*>(palloc(VARHDRSZ + payload)); SET_VARSIZE(blob, VARHDRSZ + payload);
        if (payload>0) std::memcpy(VARDATA(blob), buf.data(), payload);
        bioseq::pg_utils::pg_datum_list<3> values{};
        values.set(1, static_cast<std::int64_t>(chunk_no));
        values.set(2, static_cast<std::int32_t>(payload));
        values.set_raw(3, bioseq::pg_utils::pg_datum::from_pointer(blob));
        HeapTuple tup = values.form_tuple(RelationGetDescr(rel));
        CatalogTupleInsert(rel, tup); heap_freetuple(tup); pfree(blob);
        relation_close(rel, RowExclusiveLock); decoded_chunk_ = -1;
    }
    template <class F> void for_each_sequence(F&& loader);
    void clear(std::string_view name);
};

// ---- shared drop-by-prefix helper -----------------------------------------
[[noreturn]] inline void index_clear_spi_error_as_cpp(
        const char* stage,
        int status) {
    bioseq::pg_utils::invoke_pg_as_cpp([stage, status] {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("bioseq index clear failed"),
             errdetail("stage=%s status=%d", stage, status),
             errhint("The transaction was aborted so stale index relations cannot be accepted.")));
    });
    pg_unreachable();
}

inline void clear_with_prefix_(const std::string& prefix) {
    // Build and own all variable-length formatting before entering any
    // PostgreSQL leaf. Escape SQL quotes plus LIKE metacharacters in the
    // literal prefix; relation identifiers are quoted again by format(%I).
    std::string escaped;
    escaped.reserve(prefix.size());
    for (const char byte : prefix) {
        if (byte == '\'') {
            escaped.push_back('\'');
        }
        if (byte == '_' || byte == '%' || byte == '\\') {
            escaped.push_back('\\');
        }
        escaped.push_back(byte);
    }
    const std::string sql = fmt::format(
        "DO $$ DECLARE r record; BEGIN "
        "FOR r IN SELECT c.relname FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace "
        "WHERE n.nspname='bioseq_internal' AND c.relname LIKE '{}%' ESCAPE '\\' LOOP "
        "EXECUTE format('DROP TABLE IF EXISTS bioseq_internal.%I CASCADE', r.relname); END LOOP; END $$;",
        escaped);

    struct SpiClearState {
        int connect_status = 0;
        int execute_status = 0;
        int finish_status = 0;
    } state{};
    const char* sql_data = sql.c_str();
    bioseq::pg_utils::invoke_pg_as_cpp([&state, sql_data] noexcept {
        state.connect_status = SPI_connect();
        if (state.connect_status != SPI_OK_CONNECT) return;
        state.execute_status = SPI_execute(sql_data, false, 0);
        state.finish_status = SPI_finish();
    });
    if (state.connect_status != SPI_OK_CONNECT) {
        index_clear_spi_error_as_cpp("SPI_connect", state.connect_status);
    }
    if (state.execute_status != SPI_OK_UTILITY) {
        index_clear_spi_error_as_cpp("SPI_execute", state.execute_status);
    }
    if (state.finish_status != SPI_OK_FINISH) {
        index_clear_spi_error_as_cpp("SPI_finish", state.finish_status);
    }
}

inline void IndexStorageFragmentPg::clear(std::string_view name) {
    clear_with_prefix_(fmt::format("seqidx_{}_{:016x}_", short_index_for(name),
                                   identity_hash64(db_oid_, tablespace_oid_, main_relfilenode_, column_attno_, name)));
    index_relid_ = InvalidOid;
}
inline void IndexStorageSequencePg::clear(std::string_view name) {
    clear_with_prefix_(fmt::format("seqidx_{}_{:016x}_", short_index_for(name),
                                   identity_hash64(db_oid_, tablespace_oid_, main_relfilenode_, column_attno_, name)));
    index_relid_ = InvalidOid; decoded_valid_ = false;
}
inline void IndexStorageTablePg::clear(std::string_view name) {
    clear_with_prefix_(fmt::format("seqidxtab_{}_{:016x}_", short_index_for(name),
                                   identity_hash64(db_oid_, tablespace_oid_, main_relfilenode_, column_attno_, name)));
    index_relid_ = InvalidOid;
}

inline std::size_t IndexStorageFragmentPg::spill_sorted_run(std::span<const std::pair<std::uint64_t,std::uint64_t>> sorted) {
    return sorted_runs_ensure_().spill_sorted_run(sorted);
}

inline std::size_t IndexStorageFragmentPg::spill_sorted_run_owned(
        std::vector<std::pair<std::uint64_t,std::uint64_t>> sorted) {
    return sorted_runs_ensure_().spill_sorted_run_owned(std::move(sorted));
}

	inline std::size_t IndexStorageFragmentPg::spill_sorted_run_owned_admitted(
	        std::vector<std::pair<std::uint64_t,std::uint64_t>> sorted,
	        std::uint64_t admitted_bytes,
	        bioseq::parallelism::MemoryPermit permit) {
	    return sorted_runs_ensure_().spill_sorted_run_owned_admitted(
	        std::move(sorted), admitted_bytes, std::move(permit));
	}

	inline void IndexStorageFragmentPg::demote_resident_runs_until_available(
	        std::uint64_t bytes_needed) {
	    if (sorted_runs_.has_value()) {
	        sorted_runs_->demote_resident_runs_until_available(bytes_needed);
	    }
	}

template <class F>
inline void IndexStorageFragmentPg::merge_runs(F&& on_kmer) {
    if (!sorted_runs_.has_value()) {
        return;
    }
    g_last_index_merge_run_count = std::max<std::uint64_t>(
        g_last_index_merge_run_count, sorted_runs_->run_count());
    sorted_runs_->merge_runs(std::forward<F>(on_kmer));
}

inline MpsSegmentSpillStorageBinding
IndexStorageFragmentPg::begin_segment_spill_generation() {
    if (segment_spill_pg_poisoned_) {
        rev3_storage_error_as_cpp(
            "poisoned MPS spill storage reused after PostgreSQL error");
    }
    try {
        for (auto& [segment, runs] : segment_sorted_runs_) {
            (void)segment;
            runs.release_runs();
        }
    } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
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
    segment_spill_binding_ = MpsSegmentSpillStorageBinding{
        .generation_token = token,
        .mutation_version = 0,
    };
    return segment_spill_binding_;
}

inline void IndexStorageFragmentPg::record_segment_spill_mutation_() noexcept {
    if (!segment_spill_binding_.valid()) {
        return;
    }
    if (segment_spill_binding_.mutation_version
            == (std::numeric_limits<std::uint64_t>::max)()) {
        segment_spill_binding_ = {};
        return;
    }
    ++segment_spill_binding_.mutation_version;
}

inline std::size_t IndexStorageFragmentPg::spill_sorted_segment_run(
        std::uint32_t position_segment,
        std::span<const std::pair<std::uint64_t,std::uint64_t>> sorted) {
    if (segment_spill_pg_poisoned_) {
        rev3_storage_error_as_cpp(
            "poisoned MPS spill storage reused after PostgreSQL error");
    }
    if (!segment_spill_binding_.valid()) {
        (void)begin_segment_spill_generation();
    }
    auto iterator = std::lower_bound(
        segment_sorted_runs_.begin(),
        segment_sorted_runs_.end(),
        position_segment,
        [](const SegmentRunEntry& entry, std::uint32_t segment) {
            return entry.first < segment;
        });
    if (iterator == segment_sorted_runs_.end()
        || iterator->first != position_segment) {
        const std::size_t offset = static_cast<std::size_t>(
            iterator - segment_sorted_runs_.begin());
        if (!ensure_segment_run_metadata_capacity_(
                segment_sorted_runs_.size() + 1u)) {
            rev3_storage_error_as_cpp(
                "MPS spill segment metadata admission failed");
        }
        iterator = segment_sorted_runs_.insert(
            segment_sorted_runs_.begin()
                + static_cast<std::ptrdiff_t>(offset),
            SegmentRunEntry{position_segment, SortedPairRunStore{}});
    }
    // SortedPairRunStore may compact/demote physical runs internally. Those
    // transformations preserve the exact canonical pair stream; this one
    // version increment therefore names the logical run injection, not its
    // eventual memory-vs-BufFile representation.
    try {
        const std::size_t run = iterator->second.spill_sorted_run(sorted);
        inject_rev3_pg_failure_as_cpp(
            Rev3PgFailureInjection::mps_spill, "mps_spill");
        record_segment_spill_mutation_();
        return run;
    } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
        for (auto& [segment, runs] : segment_sorted_runs_) {
            (void)segment;
            runs.abandon_after_pg_error_noexcept();
        }
        segment_spill_binding_ = {};
        segment_spill_pg_poisoned_ = true;
        throw;
    }
}

inline bool IndexStorageFragmentPg::reroute_segment_runs(
        std::uint64_t old_span,
        std::uint64_t new_span,
        std::uint64_t target_length) {
    if (!segment_spill_binding_.valid()
        || old_span == 0 || new_span == 0) {
        if (segment_spill_pg_poisoned_) {
            rev3_storage_error_as_cpp(
                "poisoned MPS reroute storage reused after PostgreSQL error");
        }
        return false;
    }
    struct AdmittedSegmentRuns {
        bioseq::parallelism::MemoryPermit permit;
        std::vector<SegmentRunEntry> values;
    } rerouted;
    struct AdmittedPairBatch {
        bioseq::parallelism::MemoryPermit permit;
        std::vector<std::pair<std::uint64_t,std::uint64_t>> values;
    } routed_batch;
    const auto ensure_capacity = []<class T>(
            bioseq::parallelism::MemoryPermit& permit,
            std::vector<T>& values,
            std::size_t required) -> bool {
        const std::uint64_t retained = bioseq::parallelism::saturating_mul(
            static_cast<std::uint64_t>(values.capacity()),
            static_cast<std::uint64_t>(sizeof(T)));
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
                static_cast<std::uint64_t>(sizeof(T))),
            bioseq::parallelism::kVectorPreallocationHeadroomFactor);
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
                bioseq::parallelism::MemoryAdmissionController::instance()
                    .try_reserve(peak);
            if (!acquired) {
                return false;
            }
            permit = std::move(*acquired);
        }
        values.reserve(required);
        const std::uint64_t exact = bioseq::parallelism::saturating_mul(
            static_cast<std::uint64_t>(values.capacity()),
            static_cast<std::uint64_t>(sizeof(T)));
        if (exact == (std::numeric_limits<std::uint64_t>::max)()
            || exact > permit.bytes()) {
            return false;
        }
        permit.shrink_to(exact);
        return true;
    };
    const auto rerouted_store = [&](std::uint32_t segment)
            -> SortedPairRunStore& {
        auto iterator = std::lower_bound(
            rerouted.values.begin(),
            rerouted.values.end(),
            segment,
            [](const SegmentRunEntry& entry, std::uint32_t value) {
                return entry.first < value;
            });
        if (iterator == rerouted.values.end() || iterator->first != segment) {
            const std::size_t offset = static_cast<std::size_t>(
                iterator - rerouted.values.begin());
            if (!ensure_capacity(
                    rerouted.permit,
                    rerouted.values,
                    rerouted.values.size() + 1u)) {
                rev3_storage_error_as_cpp(
                    "MPS reroute segment metadata admission failed");
            }
            iterator = rerouted.values.insert(
                rerouted.values.begin() + static_cast<std::ptrdiff_t>(offset),
                SegmentRunEntry{segment, SortedPairRunStore{}});
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
                                    rev3_storage_error_as_cpp(
                                        "MPS reroute pair batch admission failed");
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
        // Every old store has been replayed successfully. Close its PostgreSQL
        // handles at a checked boundary before publication of the replacement;
        // the replacement stores remain open for qualification/encode replay.
        for (auto& [segment, runs] : segment_sorted_runs_) {
            (void)segment;
            runs.release_runs();
        }
    } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
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

template <class F>
inline void IndexStorageFragmentPg::merge_segment_runs(
        F&& on_segment_kmer) {
    if (segment_spill_pg_poisoned_) {
        rev3_storage_error_as_cpp(
            "poisoned MPS merge storage reused after PostgreSQL error");
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
                        inject_rev3_pg_failure_as_cpp(
                            Rev3PgFailureInjection::mps_merge,
                            "mps_merge_callback");
                    }
                    on_segment_kmer(segment, hash, positions);
                });
        }
    } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
        for (auto& [segment, runs] : segment_sorted_runs_) {
            (void)segment;
            runs.abandon_after_pg_error_noexcept();
        }
        segment_spill_binding_ = {};
        segment_spill_pg_poisoned_ = true;
        throw;
    }
}

inline SortedRunStoreStats IndexStorageFragmentPg::sorted_run_stats() const noexcept {
    return sorted_runs_.has_value() ? sorted_runs_->stats() : SortedRunStoreStats{};
}

inline void IndexStorageFragmentPg::snapshot_sorted_run_stats_before_release() {
    if (!sorted_runs_.has_value()) {
        return;
    }
    const SortedRunStoreStats st = sorted_runs_->stats();
    const std::uint64_t total_runs = sorted_runs_->run_count();
    g_last_index_merge_run_count = total_runs;
    g_last_mps_build_stats = LastMpsBuildStats{
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
    bioseq_log<DBG>("[bioseq.index.mps_build.row] backend_pid={} db_oid={} relfilenode={} sequence_rowno={} "
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

inline void IndexStorageFragmentPg::release_runs_noexcept() noexcept {
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

inline void IndexStorageFragmentPg::release_runs() {
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
    } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
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

inline void IndexStorageFragmentPg::abandon_after_pg_error_noexcept() noexcept {
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
    decoded_sub_ = DIRECTORY_SUB_KEY;
    decoded_chunk_ = (std::numeric_limits<std::size_t>::max)();
    scratch_ = nullptr;
    scratch_units_ = 0;
    chunk_cache_.clear();
    chunk_cache_bytes_ = 0;
    chunk_payload_bytes_.clear();
}

inline void IndexStorageFragmentPg::close_checked() {
    release_runs();
    chunk_cache_.clear();
    chunk_cache_bytes_ = 0;
    chunk_payload_bytes_.clear();
    if (own_mctx_ != nullptr) {
        MemoryContext finished_context = std::exchange(own_mctx_, nullptr);
        decoded_ = nullptr;
        scratch_ = nullptr;
        bioseq::pg_utils::invoke_pg_as_cpp(
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
        IndexStorageFragmentPg storage;
        storage.db_oid_ = db_oid;
        storage.tablespace_oid_ = tablespace_oid;
        storage.main_relfilenode_ = main_relfilenode;
        storage.user_relid_ = user_relid;
        storage.column_attno_ = column_attno;
        storage.index_name_ = index_name;
        storage.bucket_idx_ = bucket_idx;
        storage.shard_idx_ = shard_idx;
        storage.query_key_ = sequence_rowno;
        storage.own_mctx_ = bioseq::pg_utils::invoke_pg_as_cpp([] {
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
        } catch (bioseq::pg_utils::PostgresErrorAsCpp& error) {
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

[[noreturn]] inline void serialized_index_write_marker_error_as_cpp(
        const char* stage,
        int status) {
    bioseq::pg_utils::invoke_pg_as_cpp([stage, status] {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("bioseq index write-success marker failed"),
             errdetail("stage=%s status=%d", stage, status)));
    });
    pg_unreachable();
}

inline void mark_serialized_index_write_successful_pg(
        Oid user_relid,
        Oid main_relfilenode,
        std::int16_t column_attno,
        std::string_view index_name) {
    const std::string owned_index_name{index_name};
    const char* index_name_data = owned_index_name.c_str();
    char* quoted_pg = bioseq::pg_utils::invoke_pg_as_cpp(
        [index_name_data] { return quote_literal_cstr(index_name_data); });
    std::string quoted_index_name;
    try {
        quoted_index_name.assign(quoted_pg);
    } catch (...) {
        bioseq::pg_utils::invoke_pg_as_cpp(
            [quoted_pg] { pfree(quoted_pg); });
        throw;
    }
    bioseq::pg_utils::invoke_pg_as_cpp(
        [quoted_pg] { pfree(quoted_pg); });

    const std::string sql = fmt::format(
        "INSERT INTO bioseq_internal.metadata_age_log "
        "(user_relid, main_relfilenode, column_attno, index_name, last_updated) "
        "VALUES ({}::oid, {}::oid, {}, {}, now()) "
        "ON CONFLICT (user_relid, main_relfilenode, column_attno, index_name) "
        "DO UPDATE SET last_updated = now()",
        static_cast<unsigned>(user_relid),
        static_cast<unsigned>(main_relfilenode),
        static_cast<int>(column_attno),
        quoted_index_name);
    struct SpiWriteMarkerState {
        int connect_status = 0;
        int execute_status = 0;
        int finish_status = 0;
        std::uint64_t affected_rows = 0;
    } state{};
    const char* sql_data = sql.c_str();
    bioseq::pg_utils::invoke_pg_as_cpp([&state, sql_data] noexcept {
        state.connect_status = SPI_connect();
        if (state.connect_status != SPI_OK_CONNECT) return;
        state.execute_status = SPI_execute(sql_data, false, 0);
        state.affected_rows = SPI_processed;
        state.finish_status = SPI_finish();
    });
    if (state.connect_status != SPI_OK_CONNECT) {
        serialized_index_write_marker_error_as_cpp(
            "SPI_connect", state.connect_status);
    }
    if (state.execute_status != SPI_OK_INSERT) {
        serialized_index_write_marker_error_as_cpp(
            "SPI_execute", state.execute_status);
    }
    if (state.affected_rows != 1) {
        bioseq::pg_utils::invoke_pg_as_cpp([&state] {
            ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq index write-success marker failed"),
                 errdetail("stage=SPI_execute affected_rows=%llu",
                           static_cast<unsigned long long>(
                               state.affected_rows))));
        });
        pg_unreachable();
    }
    if (state.finish_status != SPI_OK_FINISH) {
        serialized_index_write_marker_error_as_cpp(
            "SPI_finish", state.finish_status);
    }
}

// ---- IndexStorageTablePg::for_each_sequence (test-only CHUNKED_PER_TABLE) ---
template <class F>
inline void IndexStorageTablePg::for_each_sequence(F&& loader) {
    Relation main = table_open(user_relid_, AccessShareLock);
    TupleDesc td = RelationGetDescr(main);
    Snapshot snap = RegisterSnapshot(GetTransactionSnapshot());
    const std::size_t total = (std::size_t)BioseqColumnTableRowCount(main);
    List* projected = list_make1_int(column_attno_);
    BioseqColumnReadState* rs = BioseqColumnBeginRead(main, td, projected, NIL, own_mctx_, snap, /*randomAccess=*/false, nullptr);
    MemoryContext inner = AllocSetContextCreate(own_mctx_, "seqidx_table_inner_seq", ALLOCSET_DEFAULT_SIZES);
    std::vector<Datum> vals(td->natts);
    std::vector<bool>  nullsv(td->natts);
    Datum* valp = vals.data();
    std::vector<char> nullbuf(td->natts);
    bool* nulls = reinterpret_cast<bool*>(nullbuf.data());
    std::uint64_t rowno = 0;
	    while (BioseqColumnReadNextRow(rs, valp, nulls, &rowno)) {
	        if (nulls[column_attno_ - 1]) continue;
	        MemoryContextReset(inner);
	        MemoryContext old = MemoryContextSwitchTo(inner);
	        auto hdr_detoasted = bioseq::pg_utils::detoast_as<pgseq_t>(
	            bioseq::pg_utils::pg_datum::from_raw(valp[column_attno_ - 1]));
	        pgseq_t* hdr = hdr_detoasted.ptr;
	        MemoryContextSwitchTo(old);
        const std::uint8_t alpha = hdr->alphabet_code;
        const std::uint8_t bw = logical_storage_unit_byte_width(hdr);
        if (!is_locator_stub(hdr)) {
            pgseq_t::dispatch_instance(
                alpha, bw, [hdr, &loader, rowno, total]<class S>() {
                compact_sequence_range<S> rng{*hdr};
                const auto view =
                    bioseq::storage::as_lazy_sequence_view(rng);
                loader(view, static_cast<std::size_t>(rowno), total);
            });
        } else {
            const seq_locator loc = decode_locator_from_stub(hdr);
            pgseq_t::dispatch_instance(
                alpha,
                bw,
                [this, &loader, loc, rowno, total, rs, inner]<class S>() {
                using SourceBinding =
                    bioseq::storage::postgres_lazy_sequence_binding<S>;
                const SourceBinding source_binding{
                    db_oid_, tablespace_oid_, main_relfilenode_,
                    column_attno_, loc,
                    SourceBinding::expected_last_fragment_padding(loc),
                    rs->snapshot, read_caches_, inner};
                bioseq::storage::with_lazy_postgres<S>(
                    source_binding,
                    [&loader, rowno, total](auto view){
                        loader(view, static_cast<std::size_t>(rowno), total);
                    });
            });
        }
        hdr_detoasted.free_if_copy();
    }
    // with_lazy_postgres keyed cached fragment readers by rs->snapshot.
    // Release them before the outer reader unregisters a refreshed snapshot.
    if (read_caches_ != nullptr) {
        read_caches_->close_all(/*commit=*/false);
    }
    BioseqColumnEndRead(rs);
    UnregisterSnapshot(snap);
    MemoryContextDelete(inner);
    table_close(main, AccessShareLock);
}

// ---- conformance gates (§8) ----
template <class Storage>
concept publicly_mutable_mps_segment_spill_state = requires(Storage& storage) {
    storage.segment_sorted_runs_;
    storage.segment_spill_binding_;
    storage.segment_spill_pg_poisoned_;
    storage.next_segment_spill_generation_token_;
};
static_assert(!publicly_mutable_mps_segment_spill_state<
    IndexStorageFragmentPg>);
static_assert(!std::is_move_assignable_v<IndexStorageFragmentPg>);
static_assert(!std::is_move_assignable_v<IndexStorageSequencePg>);
static_assert(!std::is_move_assignable_v<IndexStorageTablePg>);
static_assert(bioseq::indexing::is_index_chunked_storage_for<
                  IndexStorageFragmentPg, SequenceIndexType::CHUNKED_PER_FRAGMENT>,
              "IndexStorageFragmentPg must satisfy CHUNKED_PER_FRAGMENT");
static_assert(bioseq::indexing::is_index_chunked_storage_for<
                  IndexStorageSequencePg, SequenceIndexType::INLINE_PER_SEQUENCE>,
              "IndexStorageSequencePg must satisfy INLINE_PER_SEQUENCE");
static_assert(bioseq::indexing::is_index_chunked_storage_for<
                  IndexStorageTablePg, SequenceIndexType::CHUNKED_PER_TABLE>,
              "IndexStorageTablePg must satisfy CHUNKED_PER_TABLE");

}  // namespace bioseq::storage::indexing

extern "C" PGDLLEXPORT void
bioseq_format6_parallel_scan_probe_worker_main(
        dsm_segment* segment,
        shm_toc* toc) {
    bioseq::storage::indexing::
        format6_parallel_scan_probe_worker_main_pg(
            segment, toc);
}
