/**
 * @file fragment_storage_contract.cppm
 * @brief Contract for chunked-per-fragment PostgreSQL storage and spill ownership.
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
export module bioseq.postgres.storage.indexing.chunked_storage:fragment_storage_contract;
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
import :generation_writer;
import :generation_session;
import :hash_major_writer;
import :hash_major_session;
export namespace bioseq::storage::indexing {

/// PostgreSQL adapter for chunked-per-fragment rows, generation sidecars, and
/// spill runs owned by one logical index handle.
struct fragment_index_storage {
    using generation_read_session = index_storage_fragment_generation_read_session;
    using generation_row_sink = index_storage_fragment_generation_row_sink;
    using hash_major_minimizer_generation_writer = index_storage_hash_major_minimizer_writer;

    Oid db_oid_ = InvalidOid, tablespace_oid_ = InvalidOid, main_relfilenode_ = InvalidOid,
        user_relid_ = InvalidOid;
    seq_caches* read_caches_ = nullptr;
    std::int16_t column_attno_ = 0;
    std::string_view index_name_{};
    std::uint8_t bucket_idx_ = 0;
    std::uint32_t shard_idx_ = 0;
    std::uint64_t query_key_ = 0;
    MemoryContext own_mctx_ = nullptr;
    mutable Oid index_relid_ = InvalidOid;
    mutable std::int64_t* decoded_ = nullptr;
    mutable std::size_t decoded_units_ = 0;
    mutable std::size_t decoded_bytes_ = 0;
    mutable index_sub_key_t decoded_sub_ = k_directory_sub_key;
    mutable std::size_t decoded_chunk_ = std::numeric_limits<std::size_t>::max();
    /// One admitted immutable chunk retained by the backend-local read cache.
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
    std::size_t scratch_units_ = 0;
    std::size_t run_buffer_override_bytes_ = 0;
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

        explicit fragment_read_resources(LOCKMODE lock) noexcept : relation_lock(lock) {}
        fragment_read_resources(const fragment_read_resources&) = delete;
        fragment_read_resources& operator=(const fragment_read_resources&) = delete;
        ~fragment_read_resources() {
            DEBUG_ASSERT(abandoned || (relation == nullptr && scan == nullptr &&
                                       ordered_index_relation == nullptr && detoasted == nullptr),
                         "fragment read resources require checked close or abandonment");
        }

        [[nodiscard]] Relation release_relation() noexcept {
            DEBUG_ASSERT(scan == nullptr && ordered_index_relation == nullptr &&
                             detoasted == nullptr,
                         "cannot transfer a relation with active fragment read resources");
            return std::exchange(relation, nullptr);
        }

        void release_detoasted_checked() {
            if (detoasted_is_copy && detoasted != nullptr) {
                bytea* copy = detoasted;
                bioseq::postgres::support::invoke_pg_as_cpp([copy] { pfree(copy); });
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
                    [finished_index] { index_close(finished_index, AccessShareLock); });
                ordered_index_relation = nullptr;
            }
            if (relation != nullptr) {
                Relation finished_relation = relation;
                const LOCKMODE finished_lock = relation_lock;
                bioseq::postgres::support::invoke_pg_as_cpp([finished_relation, finished_lock] {
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

        [[noreturn]] void close_then_rethrow_cpp(std::exception_ptr error) {
            try {
                close_checked();
            } catch (...) {
                abandon_after_pg_error_noexcept();
                throw;
            }
            std::rethrow_exception(error);
        }
    };
    static_assert(!std::is_copy_constructible_v<fragment_read_resources>);
    static_assert(!std::is_copy_assignable_v<fragment_read_resources>);
    static_assert(std::is_nothrow_destructible_v<fragment_read_resources>);
    static_assert(
        noexcept(std::declval<fragment_read_resources&>().abandon_after_pg_error_noexcept()));

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

        explicit fragment_write_resources(LOCKMODE lock) noexcept : relation_lock(lock) {}
        fragment_write_resources(const fragment_write_resources&) = delete;
        fragment_write_resources& operator=(const fragment_write_resources&) = delete;
        ~fragment_write_resources() {
            DEBUG_ASSERT(abandoned || (relation == nullptr && scan == nullptr && tuple == nullptr &&
                                       blob == nullptr),
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
                bioseq::postgres::support::invoke_pg_as_cpp([finished_relation, finished_lock] {
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

        [[noreturn]] void close_then_rethrow_cpp(std::exception_ptr error) {
            try {
                close_checked();
            } catch (...) {
                abandon_after_pg_error_noexcept();
                throw;
            }
            std::rethrow_exception(error);
        }
    };
    static_assert(!std::is_copy_constructible_v<fragment_write_resources>);
    static_assert(!std::is_copy_assignable_v<fragment_write_resources>);
    static_assert(std::is_nothrow_destructible_v<fragment_write_resources>);
    static_assert(
        noexcept(std::declval<fragment_write_resources&>().abandon_after_pg_error_noexcept()));

    // Authority-bearing minimizer-sequence spill state is intentionally not part of the
    // otherwise duck-typed storage surface.  Callers may observe its immutable
    // binding and mutate it only through the narrow logical-event APIs below.
    using segment_run_entry_type = std::pair<std::uint32_t, sorted_pair_run_store>;
    // Reverse destruction drops stores/vector nodes before metadata admission.
    bioseq::parallelism::memory_permit segment_sorted_runs_metadata_permit_;
    std::vector<segment_run_entry_type> segment_sorted_runs_;
    inline static std::atomic<std::uint64_t> next_segment_spill_generation_token_{1u};
    direct_minimizer_spill_storage_binding segment_spill_binding_{};
    bool segment_spill_pg_poisoned_ = false;
    void record_segment_spill_mutation_() noexcept;
    [[nodiscard]] bool ensure_segment_run_metadata_capacity_(std::size_t required);

  public:
    /// Create an unbound handle that acquires PostgreSQL ownership explicitly.
    fragment_index_storage() = default;
    fragment_index_storage(const fragment_index_storage&) = delete;
    fragment_index_storage& operator=(const fragment_index_storage&) = delete;
    fragment_index_storage(fragment_index_storage&& o) noexcept {
        steal_(std::move(o));
    }
    fragment_index_storage& operator=(fragment_index_storage&&) = delete;
    ~fragment_index_storage() {
        release_();
    }

    /// Transfer every C++ and PostgreSQL handle without releasing the source.
    void steal_(fragment_index_storage&& o) noexcept;
    /// Drop resident spill buffers without invoking PostgreSQL cleanup.
    void release_resident_runs_noexcept() noexcept;
    /// Assert that checked close or PG-error abandonment already ran.
    void release_() noexcept;

    /// Raise a PostgreSQL data-corruption error at the C++ boundary.
    [[noreturn]] void index_error(std::string_view message) const;

    // ---- validators (translate to ereport on the pg side) ----
    /// Decode and authenticate a complete persisted minimizer fragment header.
    [[nodiscard]] minimizer_hashmap_fragment_index::encoded_header validate_minimizer_blob_or_error(
        const char* p,
        std::size_t bytes,
        std::optional<std::uint32_t> expected_bucket_count = std::nullopt) const {
        using header_type = minimizer_hashmap_fragment_index::encoded_header;
        using bucket_type = minimizer_hashmap_fragment_index::encoded_bucket;
        if (bytes < sizeof(header_type))
            index_error("minimizer blob truncated header");
        header_type header{};
        std::memcpy(&header, p, sizeof(header_type));
        if (header.magic != minimizer_hashmap_fragment_index::k_encoded_magic ||
            header.layout_revision != minimizer_hashmap_fragment_index::k_encoded_layout_revision)
            index_error("minimizer blob magic/layout revision");
        const std::size_t expect = sizeof(header_type) + sizeof(bucket_type) * header.bucket_count +
                                   sizeof(std::uint64_t) * header.total_positions;
        if (bytes != expect)
            index_error("minimizer blob size mismatch");
        if (expected_bucket_count && header.bucket_count != *expected_bucket_count) {
            index_error("minimizer blob bucket_count mismatch");
        }
        return header;
    }
    /// Authenticate a persisted grouped-directory image before exposing it.
    void validate_minimizer_directory_or_error(const char* p, std::size_t bytes) const;
    /// Reject reserved keys, directory chunk misuse, and invalid chunk sizes.
    void validate_minimizer_chunk_metadata_or_error(index_sub_key_t sk,
                                                    std::size_t chunk_no,
                                                    std::size_t min_bytes,
                                                    std::size_t /*alignment*/) const;

    // ---- relation resolution ----
    /// Derive the physical fragment-table name from the logical handle.
    [[nodiscard]] std::string this_table_name() const;

    /// Remove every backend-local object derived from this logical minimizer
    /// sidecar.  Relcache invalidation supplies a physical sidecar OID, whereas
    /// the cache keys use the owning user relation, so schema replacement must
    /// evict by the complete logical identity explicitly.
    void evict_generation_storage_residency_();

    /// Ensure the same-name minimizer sidecar has the one canonical generation
    /// schema. Normal ensure is deliberately non-destructive: an exact relation
    /// is reused by OID and any malformed same-name object fails closed.
    /// Explicit clear/reset owns the destructive replacement boundary.
    [[nodiscard]] Oid ensure_minimizer_generation_storage();

    /// Transactionally remove the complete physical generation family for
    /// this one logical minimizer index.  Head, manifest, immutable payload,
    /// and temporary-run objects are row classes in the canonical sidecars,
    /// so dropping every table under the exact identity prefix clears all of
    /// them together.
    [[nodiscard]] minimizer_generation_clear_result clear_minimizer_generation_storage();

    /// Open the current-generation reader against the exact physical name,
    /// bypassing the cached OID. A five-column relation therefore fails schema
    /// validation rather than entering the generation route.
    [[nodiscard]] generation_read_session
    begin_generation_read(std::uint64_t query_key,
                          Snapshot executor_snapshot,
                          expected_minimizer_generation_identity expected) const;

    /// Open the exact common-encoder row sink after atomically ensuring the
    /// canonical same-name relation.  The caller still owns generation
    /// encoding, complete reread, and READY-head publication.
    [[nodiscard]] generation_row_sink begin_generation_write(std::uint64_t query_key,
                                                             std::uint64_t generation_id);

    /// Open a hash-major writer bound to this handle and unpublished generation.
    [[nodiscard]] hash_major_minimizer_generation_writer
    begin_hash_major_minimizer_generation_write(std::uint64_t query_key,
                                                std::uint64_t generation_id);

    /// Resolve or create the legacy fragment relation for this exact handle.
    [[nodiscard]] Oid resolve_relid_(bool create_if_missing) const;
    /// Open the resolved fragment relation under the caller-selected lock.
    [[nodiscard]] Relation open_index_relation_(bool create_if_missing, LOCKMODE lock) const;

    /// Grow the MemoryContext-owned decode buffer without changing its owner.
    void ensure_buffer_(std::size_t units) const;
    /// Return a scratch region large enough for the requested 64-bit units.
    [[nodiscard]] std::int64_t* scratch_buffer_(std::size_t units);
    /// Report the currently retained scratch capacity in bytes.
    [[nodiscard]] std::size_t scratch_buffer_bytes_() const;

    /// Form the complete logical and physical identity of one cached chunk.
    [[nodiscard]] index_chunk_key chunk_cache_key_(index_sub_key_t sub_key,
                                                   std::size_t chunk_no) const noexcept;

    /// Admit a copied payload only while its memory permit remains owned.
    void admit_cached_chunk_(index_sub_key_t sub_key,
                             std::size_t chunk_no,
                             std::span<const char> payload,
                             bool written_in_current_xact) const;

    /// Read and validate one tuple into the reusable decode buffer.
    void load_chunk_into_buffer_(index_sub_key_t sub_key, std::size_t chunk_no) const;

    // ---- const metadata queries (total: 0/empty on absent) ----
    /// Count distinct persisted sub-keys for this query identity.
    [[nodiscard]] std::size_t number_of_sub_keys() const;
    /// Count contiguous chunks for one persisted sub-key.
    [[nodiscard]] std::size_t number_of_chunks(index_sub_key_t sub_key) const;
    /// Return the authenticated payload size of one chunk.
    [[nodiscard]] std::size_t chunk_byte_size(index_sub_key_t sub_key, std::size_t chunk_no) const;

    /// Return one decoded chunk using the handle-owned reusable buffer.
    template <class p_value_type>
    [[nodiscard]] const p_value_type* read_chunk(index_sub_key_t sub_key, std::size_t chunk_no) {
        static_assert(alignof(p_value_type) <= alignof(std::int64_t),
                      "read_chunk element type requires excessive alignment");
        if (decoded_ != nullptr && decoded_sub_ == sub_key && decoded_chunk_ == chunk_no)
            return reinterpret_cast<const p_value_type*>(decoded_);
        load_chunk_into_buffer_(sub_key, chunk_no);
        return reinterpret_cast<const p_value_type*>(decoded_);
    }

    /// Record a single-chunk byte plan for a sub-key.
    void resize(index_sub_key_t sub_key, std::size_t bytes_count);
    /// Record the exact payload size of every planned chunk for a sub-key.
    void resize_chunks(index_sub_key_t sub_key, std::span<const std::size_t> per_chunk_bytes);
    /// Warm one chunk through the same validated cache path as a read.
    void prefetch_chunk(index_sub_key_t sub_key, std::size_t chunk_no);

    /// Replace one exact chunk tuple while the caller owns writer resources.
    void upsert_chunk_row_(fragment_write_resources& resources,
                           std::uint64_t query_key,
                           index_sub_key_t sub_key,
                           std::size_t chunk_no,
                           std::size_t chunk_bytes,
                           std::span<const char> payload);

    /// Persist an already serialized chunk and preserve cache visibility rules.
    void write_serialized_chunk_checked(std::uint64_t query_key,
                                        index_sub_key_t sub_key,
                                        std::size_t chunk_no,
                                        std::span<const char> payload,
                                        bool written_in_current_xact);

    /// Materialize and persist every chunk in the recorded plan.
    template <class p_value_type, class p_callback>
    void write_to_each(index_sub_key_t sub_key, p_callback&& writer) {
        static_assert(bioseq::indexing::index_chunk_writer_fn<p_callback, p_value_type>,
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
        const std::size_t scratch_units =
            max_payload == 0 ? 1 : 1 + (max_payload - 1) / sizeof(std::int64_t);
        std::int64_t* buffer = scratch_buffer_(scratch_units);

        fragment_write_resources resources{RowExclusiveLock};
        try {
            resources.relation = open_index_relation_(true, RowExclusiveLock);
            if (resources.relation == nullptr) {
                index_error("fragment write relation could not be opened");
            }
            for (std::size_t chunk_no = 0; chunk_no < n;
                 /* writer may skip */) {
                const std::size_t payload_bytes = plan[chunk_no];
                if (scratch_buffer_bytes_() < payload_bytes) {
                    index_error("fragment write scratch is undersized");
                }
                const std::size_t capacity = payload_bytes / sizeof(p_value_type);
                if (payload_bytes > 0) {
                    std::memset(buffer, 0, payload_bytes);
                }
                const std::size_t next =
                    writer(chunk_no, reinterpret_cast<p_value_type*>(buffer), capacity);
                if (next < chunk_no) {
                    index_error("fragment writer moved backwards");
                }
                const auto payload =
                    std::span<const char>(reinterpret_cast<const char*>(buffer), payload_bytes);
                upsert_chunk_row_(resources, query_key_, sub_key, chunk_no, payload_bytes, payload);
                admit_cached_chunk_(sub_key, chunk_no, payload, true);
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

    /// Visit validated cached or copied chunks in a half-open sub-key range.
    template <class p_callback>
    void read_chunks_in_range(index_sub_key_t lo, index_sub_key_t hi, p_callback&& cb) {
        if (hi <= lo)
            return;
        fragment_read_resources resources{AccessShareLock};
        try {
            resources.relation = open_index_relation_(false, AccessShareLock);
            if (resources.relation == nullptr)
                return;
            const Oid primary_index =
                bioseq::postgres::support::invoke_pg_as_cpp([relation = resources.relation] {
                    return bioseq::postgres::support::find_primary_key_oid(relation);
                });
            if (!OidIsValid(primary_index)) {
                resources.close_checked();
                index_error("range scan has no primary index");
            }
            resources.ordered_index_relation = bioseq::postgres::support::invoke_pg_as_cpp(
                [primary_index] { return index_open(primary_index, AccessShareLock); });
            std::array<ScanKeyData, 3> keys{};
            ScanKeyData* key_data = keys.data();
            const Datum query_key =
                bioseq::postgres::support::pg_datum::from(static_cast<std::int64_t>(query_key_))
                    .raw();
            const Datum first_sub_key =
                bioseq::postgres::support::pg_datum::from(static_cast<std::int64_t>(lo)).raw();
            const Datum last_sub_key =
                bioseq::postgres::support::pg_datum::from(static_cast<std::int64_t>(hi - 1)).raw();
            bioseq::postgres::support::invoke_pg_as_cpp(
                [key_data, query_key, first_sub_key, last_sub_key] {
                    ScanKeyInit(&key_data[0],
                                k_attribute_query_key,
                                BTEqualStrategyNumber,
                                F_INT8EQ,
                                query_key);
                    ScanKeyInit(&key_data[1],
                                k_attribute_sub_key,
                                BTGreaterEqualStrategyNumber,
                                F_INT8GE,
                                first_sub_key);
                    ScanKeyInit(&key_data[2],
                                k_attribute_sub_key,
                                BTLessEqualStrategyNumber,
                                F_INT8LE,
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
                HeapTuple tuple =
                    bioseq::postgres::support::invoke_pg_as_cpp([scan = resources.scan] {
                        return systable_getnext_ordered(scan, ForwardScanDirection);
                    });
                if (!HeapTupleIsValid(tuple))
                    break;

                bool is_null = false;
                const Datum sub_key_datum = bioseq::postgres::support::invoke_pg_as_cpp(
                    [relation = resources.relation, tuple, &is_null] {
                        return heap_getattr(
                            tuple, k_attribute_sub_key, RelationGetDescr(relation), &is_null);
                    });
                if (is_null)
                    index_error("range row sub-key is NULL");
                const std::int64_t signed_sub_key =
                    bioseq::postgres::support::pg_datum::to<std::int64_t>(
                        bioseq::postgres::support::pg_datum::from_raw(sub_key_datum));

                is_null = false;
                const Datum chunk_no_datum = bioseq::postgres::support::invoke_pg_as_cpp(
                    [relation = resources.relation, tuple, &is_null] {
                        return heap_getattr(
                            tuple, k_attribute_chunk_number, RelationGetDescr(relation), &is_null);
                    });
                if (is_null)
                    index_error("range row chunk number is NULL");
                const std::int32_t signed_chunk_no =
                    bioseq::postgres::support::pg_datum::to<std::int32_t>(
                        bioseq::postgres::support::pg_datum::from_raw(chunk_no_datum));
                if (signed_chunk_no < 0) {
                    index_error("range row chunk number is negative");
                }

                is_null = false;
                const Datum blob = bioseq::postgres::support::invoke_pg_as_cpp(
                    [relation = resources.relation, tuple, &is_null] {
                        return heap_getattr(
                            tuple, k_attribute_blob, RelationGetDescr(relation), &is_null);
                    });
                if (is_null)
                    index_error("range row blob is NULL");
                auto detoasted = bioseq::postgres::support::invoke_pg_as_cpp([blob] {
                    return bioseq::postgres::support::detoast_packed_as<bytea>(
                        bioseq::postgres::support::pg_datum::from_raw(blob));
                });
                resources.detoasted = detoasted.ptr;
                resources.detoasted_is_copy = detoasted.is_copy;
                detoasted.ptr = nullptr;
                detoasted.is_copy = false;

                const std::size_t payload = VARSIZE_ANY_EXHDR(resources.detoasted);
                bytes.resize(payload);
                if (payload > 0) {
                    std::memcpy(bytes.data(), VARDATA_ANY(resources.detoasted), payload);
                }
                const index_sub_key_t sub_key = static_cast<index_sub_key_t>(signed_sub_key);
                const std::size_t chunk_no = static_cast<std::size_t>(signed_chunk_no);
                const auto cached =
                    store_cached_chunk(chunk_cache_key_(sub_key, chunk_no),
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

    /// Visit each persisted data sub-key once for the bound query identity.
    template <class p_callback> void for_each_sub_key(p_callback&& fn) const {
        fragment_read_resources resources{AccessShareLock};
        try {
            resources.relation = open_index_relation_(false, AccessShareLock);
            if (resources.relation == nullptr)
                return;
            const Oid primary_index =
                bioseq::postgres::support::invoke_pg_as_cpp([relation = resources.relation] {
                    return bioseq::postgres::support::find_primary_key_oid(relation);
                });
            std::array<ScanKeyData, 2> keys{};
            ScanKeyData* key_data = keys.data();
            const Datum query_key =
                bioseq::postgres::support::pg_datum::from(static_cast<std::int64_t>(query_key_))
                    .raw();
            const Datum data_sub_key_limit =
                bioseq::postgres::support::pg_datum::from(
                    static_cast<std::int64_t>(k_temporary_run_sub_key_base))
                    .raw();
            bioseq::postgres::support::invoke_pg_as_cpp([key_data, query_key, data_sub_key_limit] {
                ScanKeyInit(&key_data[0],
                            k_attribute_query_key,
                            BTEqualStrategyNumber,
                            F_INT8EQ,
                            query_key);
                ScanKeyInit(&key_data[1],
                            k_attribute_sub_key,
                            BTLessStrategyNumber,
                            F_INT8LT,
                            data_sub_key_limit);
            });
            resources.scan = bioseq::postgres::support::invoke_pg_as_cpp(
                [relation = resources.relation, primary_index, key_data] {
                    return systable_beginscan(
                        relation, primary_index, OidIsValid(primary_index), nullptr, 2, key_data);
                });
            std::int64_t last = (std::numeric_limits<std::int64_t>::min)();
            bool have_last = false;
            while (true) {
                HeapTuple tuple = bioseq::postgres::support::invoke_pg_as_cpp(
                    [scan = resources.scan] { return systable_getnext(scan); });
                if (!HeapTupleIsValid(tuple))
                    break;
                bool is_null = false;
                const Datum sub_key_datum = bioseq::postgres::support::invoke_pg_as_cpp(
                    [relation = resources.relation, tuple, &is_null] {
                        return heap_getattr(
                            tuple, k_attribute_sub_key, RelationGetDescr(relation), &is_null);
                    });
                if (is_null)
                    index_error("fragment sub-key is NULL");
                const std::int64_t sub_key = bioseq::postgres::support::pg_datum::to<std::int64_t>(
                    bioseq::postgres::support::pg_datum::from_raw(sub_key_datum));
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
    /// Delete all rows or one inclusive sub-key range for the bound query.
    void delete_where_(
        std::optional<std::pair<index_sub_key_t, index_sub_key_t>> inclusive_sub_key_range);
    /// Remove every sub-key owned by the bound query identity.
    void clear_entity();
    /// Remove an inclusive range of sub-keys from the bound query identity.
    void discard_sub_keys(index_sub_key_t lo, index_sub_key_t hi);
    /// Drop every physical shard belonging to the named logical index.
    void clear(std::string_view name);

    /// Derive the admitted external-sort budget for this index handle.
    [[nodiscard]] std::size_t run_buffer_bytes() const;

    // ---- external sort spill (BufFile-backed sorted runs) ----
    /// Lazily create the whole-index sorted-run store.
    [[nodiscard]] sorted_pair_run_store& sorted_runs_ensure_();
    /// Spill a borrowed sorted run into ResourceOwner-managed storage.
    std::size_t spill_sorted_run(std::span<const std::pair<std::uint64_t, std::uint64_t>> sorted);
    /// Spill a sorted vector by transferring its resident allocation.
    std::size_t spill_sorted_run_owned(std::vector<std::pair<std::uint64_t, std::uint64_t>> sorted);
    /// Spill an owned run while transferring its existing admission permit.
    std::size_t
    spill_sorted_run_owned_admitted(std::vector<std::pair<std::uint64_t, std::uint64_t>> sorted,
                                    std::uint64_t admitted_bytes,
                                    bioseq::parallelism::memory_permit permit);
    /// Demote resident runs until the requested admission can succeed.
    void demote_resident_runs_until_available(std::uint64_t bytes_needed);
    /// Merge whole-index runs in key order without materializing the result.
    template <class p_callback> void merge_runs(p_callback&& on_kmer);
    /// Spill one position segment under the active generation binding.
    std::size_t
    spill_sorted_segment_run(std::uint32_t position_segment,
                             std::span<const std::pair<std::uint64_t, std::uint64_t>> sorted);
    /// Repartition admitted segment runs when the segment span changes.
    bool reroute_segment_runs(std::uint64_t old_span,
                              std::uint64_t new_span,
                              std::uint64_t target_length);
    /// Merge segment runs in segment and k-mer order.
    template <class p_callback> void merge_segment_runs(p_callback&& on_segment_kmer);
    /// Begin a new identity-bound segment spill generation.
    [[nodiscard]] direct_minimizer_spill_storage_binding begin_segment_spill_generation();
    /// Return the current segment spill identity without transferring it.
    [[nodiscard]] direct_minimizer_spill_storage_binding
    segment_spill_storage_binding() const noexcept;
    /// Aggregate retained and spilled run statistics for this handle.
    [[nodiscard]] sorted_run_store_stats sorted_run_stats() const noexcept;
    /// Preserve statistics before releasing their underlying run stores.
    void snapshot_sorted_run_stats_before_release();
    /// Abandon run handles after a translated PostgreSQL failure.
    void release_runs_noexcept() noexcept;
    /// Release every run through checked PostgreSQL cleanup.
    void release_runs();
    /// Close PostgreSQL state and then release admitted C++ storage.
    void close_checked();
    /// Forget PostgreSQL aliases that transaction-abort cleanup now owns.
    void abandon_after_pg_error_noexcept() noexcept;
};

} // namespace bioseq::storage::indexing
