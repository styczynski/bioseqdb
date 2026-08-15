/**
 * @file storage_adapters.cppm
 * @brief Concrete PostgreSQL chunked-index storage adapters and index
 *        administration entry points.
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
export module bioseq.postgres.storage.indexing.chunked_storage:storage_adapters;
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
import :generation_writer;
import :generation_session;
import :hash_major_writer;
import :hash_major_session;
export namespace bioseq::storage::indexing {

/// PostgreSQL adapter for chunked-per-fragment index storage and spill runs.
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
        const std::uint64_t retained = bioseq::common::saturating_mul(
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
        const std::uint64_t headroom = bioseq::common::saturating_mul(
            bioseq::common::saturating_mul(
                static_cast<std::uint64_t>(required),
                static_cast<std::uint64_t>(sizeof(segment_run_entry_type))),
            bioseq::parallelism::k_vector_preallocation_headroom_factor);
        const std::uint64_t peak = bioseq::common::saturating_add(
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
        const std::uint64_t exact = bioseq::common::saturating_mul(
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
            bioseq::common::memory::default_budget().clamp_working_set(
                desired, k_floor, k_ceiling);
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

/// Bounded cursor exposing one k-mer's unique positions as a single batch.
struct position_batch_cursor {
    const std::vector<std::uint64_t>* v = nullptr;
    [[nodiscard]] std::uint64_t total_count() const { return v->size(); }
    template <class p_callback> void for_each_batch(p_callback&& f) const {
        if (!v->empty()) f(std::span<const std::uint64_t>(v->data(), v->size()));
    }
};

/// PostgreSQL adapter for one inline-per-sequence index value.
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
        bucket_idx_ = static_cast<std::uint8_t>(loc.bucket_index);
        shard_idx_ = loc.shard_index;
        query_key_ = sequence_rowno;
        index_relid_ = InvalidOid; decoded_valid_ = false; decoded_bytes_ = 0;
    }
};

/// PostgreSQL adapter for chunked-per-table index storage.
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
        if (decoded_ != nullptr
            && decoded_chunk_ == static_cast<std::int64_t>(chunk_no)) {
            return reinterpret_cast<const p_value_type*>(decoded_);
        }
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
        decoded_chunk_ = static_cast<std::int64_t>(chunk_no);
        decoded_bytes_ = payload;
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

/// Select either best-effort global cleanup or strict prefix cleanup.
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

/// SPI stage whose return code is being classified during index cleanup.
enum class drop_index_spi_stage : std::uint8_t
{
    connect,
    discovery,
    drop,
    finish
};

/// Required control-flow response to an SPI cleanup return code.
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

/// Checked SPI lifetime used by native relation cleanup.
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

/// One relation name returned by the cleanup discovery query.
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
        "AND starts_with(c.relname::text, $2::text) "
        "ORDER BY (c.relkind = 'r'::\"char\") DESC, c.oid";

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
        const std::uint64_t retained = bioseq::common::saturating_mul(
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
        const std::uint64_t headroom = bioseq::common::saturating_mul(
            bioseq::common::saturating_mul(
                static_cast<std::uint64_t>(required),
                static_cast<std::uint64_t>(sizeof(p_value_type))),
            bioseq::parallelism::k_vector_preallocation_headroom_factor);
        const std::uint64_t peak = bioseq::common::saturating_add(
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
        const std::uint64_t exact = bioseq::common::saturating_mul(
            static_cast<std::uint64_t>(values.capacity()),
            static_cast<std::uint64_t>(sizeof(p_value_type)));
        if (exact == (std::numeric_limits<std::uint64_t>::max)()
            || exact > permit.bytes()) {
            return false;
        }
        permit.shrink_to(exact);
        return true;
    };
    const auto rerouted_store =
        [&rerouted, &ensure_capacity](std::uint32_t segment)
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
                [&valid, new_span, target_length, old_span, old_segment,
                 &routed_batch, &ensure_capacity, &rerouted_store](
                    std::uint64_t hash, auto&& positions) {
                    positions.for_each_batch(
                        [&valid, new_span, target_length, old_span,
                         old_segment, hash, &routed_batch, &ensure_capacity,
                         &rerouted_store](
                            std::span<const std::uint64_t> batch) {
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
    const std::size_t total = static_cast<std::size_t>(
        column_table_row_count(main));
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
                const source_binding bound_source{
                    db_oid_, tablespace_oid_, main_relfilenode_,
                    column_attno_, loc,
                    source_binding::expected_last_fragment_padding(loc),
                    rs->snapshot, read_caches_, inner};
                bioseq::storage::with_postgres_lazy_sequence<p_sequence_type>(
                    bound_source,
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
