/**
 * @file fragment_storage_contract.cppm
 * @brief Read and write fragment index chunks and manage sorted runs during index builds.
 */
module;

extern "C" {
#include "postgres.h"

#include "access/heapam.h"
#include "access/nbtree.h"
#include "access/relation.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/memutils.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#ifdef NDEBUG
#include <libassert/assert-macros.hpp>
#else
#include <libassert/assert.hpp>
#endif

#include <new>

export module bioseq.postgres.storage.indexing.chunked_storage:fragment_storage_contract;
import std;
import bioseq.postgres.support; // PostgreSQL relation and catalog helpers.
import bioseq.postgres.support.datums;
import bioseq.postgres.storage.manager; // Sequence storage, locators and table-name helpers.
import bioseq.indexing.type; // Index layouts, chunk limits and  reserved keys.
import bioseq.indexing.storage;           // Storage adapter requirements.
import bioseq.indexing.minimizer_hashmap; // Minimizer index header and bucket format.
import bioseq.parallelism.memory_admission;
import bioseq.postgres.storage.indexing.residency;
import bioseq.postgres.storage.indexing.sorted_runs;
import :catalog;
import :fragment_image;
import :generation_writer;
import :generation_session;
import :hash_major_writer;
export namespace bioseq::storage::indexing {

/**
 * @brief Record the refresh time after an encoded index has been written successfully
 * @param[in] user_relid User table OID.
 * @param[in] main_relfilenode Current main table file number.
 * @param[in] column_attno Sequence column number, starting at one.
 * @param[in] index_name Refreshed index name.
 */
void mark_serialized_index_write_successful_pg(Oid user_relid,
                                               Oid main_relfilenode,
                                               std::int16_t column_attno,
                                               std::string_view index_name);

/// PostgreSQL storage for one sequence index.
/// Read and write chunk rows, open index generations, and keep sorted build runs
/// in memory or temporary files.
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
    /// A cached chunk whose bytes are covered by a memory reservation.
    struct cached_chunk {
        // Declare  the reservation first so it is released after the byte buffer.
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
    // Payload sizes set by resize or resize_chunks for the next write_to_each call.
    std::map<index_sub_key_t, std::vector<std::size_t>> chunk_payload_bytes_;
    std::optional<sorted_pair_run_store> sorted_runs_;

  private:
    /// Track the, relation, sxan and temporary payload for a direct chunk read.
    /// Close them explicitly after success. After a PostgreSQL error, clear the
    /// handles and leave resource release to transaction cleanup. The destructor
    /// checks that one of these cleanup paths has run.
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

    /// Track the relation and temporary row objects for a direct chunk  write.
    /// Close them explicitly after success, or abandon the handles after a
    /// PostgreSQL error so transaction cleanup can release them. The destructor
    /// checks that cleanup was requested.
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

    // Keep segment runs private so every change can update the replay identity.
    using segment_run_entry_type = std::pair<std::uint32_t, sorted_pair_run_store>;
    // Destroy the segment stores before releasing their metadata reservation
    bioseq::parallelism::memory_permit segment_sorted_runs_metadata_permit_;
    std::vector<segment_run_entry_type> segment_sorted_runs_;
    // Store the segment number because vector insertion can move its entries.
    // Only this segment may hold the reusable write buffer.
    std::optional<std::uint32_t> active_segment_write_buffer_owner_;
    inline static std::atomic<std::uint64_t> next_segment_spill_generation_token_{1u};
    direct_minimizer_spill_storage_binding segment_spill_binding_{};
    bool segment_spill_pg_poisoned_ = false;
    void record_segment_spill_mutation_() noexcept;
    [[nodiscard]] bool ensure_segment_run_metadata_capacity_(std::size_t required);

  public:
    /// Create an empty handle to be bound to PostgreSQL storage by  its caller.
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

    /// Move resources and identity here, then clear the source handle.
    void steal_(fragment_index_storage&& o) noexcept;
    /// Discard runs kept only in mmory and invalidate saved segment replay state.
    void release_resident_runs_noexcept() noexcept;
    /// Finish destruction after checked close or PostgreSQL error cleanup preparation.
    void release_() noexcept;

    /// Report corrupt index data as a PostgreSQL error wrapped in a C++ exception.
    [[noreturn]] void index_error(std::string_view message) const;

    // Check stored index formats.
    /// Check the minimizer payload header, format version and size implied by its counts.
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
    /// Check the directory header  size, magic value and format version.
    void validate_minimizer_directory_or_error(const char* p, std::size_t bytes) const;
    /// Check that the recorded chunk size meets the minimum required by the reader.
    void validate_minimizer_chunk_metadata_or_error(index_sub_key_t sk,
                                                    std::size_t chunk_no,
                                                    std::size_t min_bytes,
                                                    std::size_t /*alignment*/) const;

    // Find or create index tables.
    /// Build the fragment index table name from this handle's storage identity.
    [[nodiscard]] std::string this_table_name() const;

    /// Invalidate this handle's decoded chunk and cached bytes for the owning table and index.
    /// Cache keys use the user table identity, so replacing an, index table must
    /// invalidate them through that identity.
    void evict_generation_storage_residency_();

    /// Find or create the minimizer generation  table and check its schema
    /// Reuse a matching table. An incompatible existing table requires an explicit reset.
    [[nodiscard]] Oid ensure_minimizer_generation_storage();

    /// Drop this logical index's generation tables and refresh-time metadata.
    /// Check ownership and scema before removing each table, including all
    /// generation headers, payload rows and temporary runs stored there.
    [[nodiscard]] minimizer_generation_clear_result clear_minimizer_generation_storage();

    /// Open a reader by the current index table name and the caller's snapshot.
    /// The reader checks the generation schema before reading rows.
    [[nodiscard]] generation_read_session
    begin_generation_read(std::uint64_t query_key,
                          Snapshot executor_snapshot,
                          expected_minimizer_generation_identity expected) const;

    /// Open a row writer  after finding or creating the checked generation table.
    /// The build must finish validation before publishing the generation as active.
    [[nodiscard]] generation_row_sink begin_generation_write(std::uint64_t query_key,
                                                             std::uint64_t generation_id);

    /// Open a writer for a new minimizer generation grouped by hash.
    [[nodiscard]] hash_major_minimizer_generation_writer
    begin_hash_major_minimizer_generation_write(std::uint64_t query_key,
                                                std::uint64_t generation_id);

    /// Find the direct fragment table, creating it if requested.
    [[nodiscard]] Oid resolve_relid_(bool create_if_missing) const;
    /// Open and check the direct fragment table with the requested lock.
    [[nodiscard]] Relation open_index_relation_(bool create_if_missing, LOCKMODE lock) const;

    /// Grow the read buffer in the handle's PostgreSQL memory context.
    void ensure_buffer_(std::size_t units) const;
    /// Return a temporary write buffer with room for this  many 64-bit units.
    [[nodiscard]] std::int64_t* scratch_buffer_(std::size_t units);
    /// Report the allocated scratch capacity in bytes.
    [[nodiscard]] std::size_t scratch_buffer_bytes_() const;

    /// Build the cache key for this table, sequence, sub-key and chunk.
    [[nodiscard]] index_chunk_key chunk_cache_key_(index_sub_key_t sub_key,
                                                   std::size_t chunk_no) const noexcept;

    /// Offer a chunk copy to the backend cache, which may refuse it when memory is tight
    void admit_cached_chunk_(index_sub_key_t sub_key,
                             std::size_t chunk_no,
                             std::span<const char> payload,
                             bool written_in_current_xact) const;

    /// Copy a chunk from the cache or its PostgreSQL row into the read buffer.
    void load_chunk_into_buffer_(index_sub_key_t sub_key, std::size_t chunk_no) const;

    // Stored chunk counts and wizes. Missing rows or tables give zero.
    /// Count stored data sub-keys for this sequence, excluding temporary runs.
    [[nodiscard]] std::size_t number_of_sub_keys() const;
    /// Count stored chunk rows for one sub-key.
    [[nodiscard]] std::size_t number_of_chunks(index_sub_key_t sub_key) const;
    /// Return the payload byte count recorded in the chunk  row.
    [[nodiscard]] std::size_t chunk_byte_size(index_sub_key_t sub_key, std::size_t chunk_no) const;

    /// Read a chunk through the handle's reusable buffer.
    /// The returned pointer remains valid until the, buffer is reused or the handle closes.
    template <class p_value_type>
    [[nodiscard]] const p_value_type* read_chunk(index_sub_key_t sub_key, std::size_t chunk_no) {
        static_assert(alignof(p_value_type) <= alignof(std::int64_t),
                      "read_chunk element type requires excessive alignment");
        if (decoded_ != nullptr && decoded_sub_ == sub_key && decoded_chunk_ == chunk_no)
            return reinterpret_cast<const p_value_type*>(decoded_);
        load_chunk_into_buffer_(sub_key, chunk_no);
        return reinterpret_cast<const p_value_type*>(decoded_);
    }

    /// Split a payload size into a plan of chunks within the storage size limit.
    void resize(index_sub_key_t sub_key, std::size_t bytes_count);
    /// Set the payload size for each chunk in the next write.
    void resize_chunks(index_sub_key_t sub_key, std::span<const std::size_t> per_chunk_bytes);
    /// Load a chunk into the read buffer if it is not already there.
    void prefetch_chunk(index_sub_key_t sub_key, std::size_t chunk_no);

    /// Insert a chunk row, replacing any row at the same sequence, sub-key and chunk number.
    void upsert_chunk_row_(fragment_write_resources& resources,
                           std::uint64_t query_key,
                           index_sub_key_t sub_key,
                           std::size_t chunk_no,
                           std::size_t chunk_bytes,
                           std::span<const char> payload);

    /// Store encoded chunk bytes and offer  them to the backend cache.
    void write_serialized_chunk_checked(std::uint64_t query_key,
                                        index_sub_key_t sub_key,
                                        std::size_t chunk_no,
                                        std::span<const char> payload,
                                        bool written_in_current_xact);

    /// Fill and write chunks from the preceding resize plan.
    /// The callback receives a temporary buffer and may return a later chunk number
    /// to skip ahead. Returning the current number advances to the next chunk
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
                 /* callback chooses the next chunk */) {
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

    /// Visit chunks with sub-keys in [lo, hi), in index order.
    /// Each callback borrows the payload bytes only for that call.
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

    /// Visit stored data sub-keys for this sequence, exclding temporary runs.
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

    // Delete stored chunks and release resources.
    /// Delete this sequence's chunks in an inclusive sub-key  range, or all its chunks.
    void delete_where_(
        std::optional<std::pair<index_sub_key_t, index_sub_key_t>> inclusive_sub_key_range);
    /// Delete all chunks for this sequence.
    void clear_entity();
    /// Delete this sequence's chunks whose sub-keys lie in the inclusive range.
    void discard_sub_keys(index_sub_key_t lo, index_sub_key_t hi);
    /// Drop the tables whose names identify shards of the named index.
    void clear(std::string_view name);

    /// Choose the sorted-run memory budget, using a handle override when supplied.
    [[nodiscard]] std::size_t run_buffer_bytes() const;

    // Sorted runs kept in memory or PostgreSQL temporary files.
    /// Return the general sorted-run store, creating it on first use.
    [[nodiscard]] sorted_pair_run_store& sorted_runs_ensure_();
    /// Add a borrowed sorted run to temporary storage.
    std::size_t spill_sorted_run(std::span<const std::pair<std::uint64_t, std::uint64_t>> sorted);
    /// Transfer a sorted vector to the run store, spilling it if memory cannot  be reserved.
    std::size_t spill_sorted_run_owned(std::vector<std::pair<std::uint64_t, std::uint64_t>> sorted);
    /// Transfer a sorted vector and its existing memory reservation to the run store.
    std::size_t
    spill_sorted_run_owned_admitted(std::vector<std::pair<std::uint64_t, std::uint64_t>> sorted,
                                    std::uint64_t admitted_bytes,
                                    bioseq::parallelism::memory_permit permit);
    /// Move runs to temporary files to free memory for the next reservation.
    void demote_resident_runs_until_available(std::uint64_t bytes_needed);
    /// Merge runs and visit each hash with a cursor over its positions.
    template <class p_callback> void merge_runs(p_callback&& on_kmer);
    /// Store a sorted run for one, target-position segment and update its reblay identity
    std::size_t
    spill_sorted_segment_run(std::uint32_t position_segment,
                             std::span<const std::pair<std::uint64_t, std::uint64_t>> sorted);
    /// Redistribute stored positions when the target segment size changes.
    bool reroute_segment_runs(std::uint64_t old_span,
                              std::uint64_t new_span,
                              std::uint64_t target_length);
    /// Visit merged runs in segment order, then hash order within  each segment.
    template <class p_callback> void merge_segment_runs(p_callback&& on_segment_kmer);
    /// Close previous segment runs and assign a new identity for the next build.
    [[nodiscard]] direct_minimizer_spill_storage_binding begin_segment_spill_generation();
    /// Return a copy of the current segment spill identity.
    [[nodiscard]] direct_minimizer_spill_storage_binding
    segment_spill_storage_binding() const noexcept;
    /// Return general-run statistics, or zero counters if that store is absent.
    [[nodiscard]] sorted_run_store_stats sorted_run_stats() const noexcept;
    /// Save general-run statistics for SQL diagnostics before releasing the store.
    void snapshot_sorted_run_stats_before_release();
    /// Drop run handles without PostgreSQL calls and invalidate replay state.
    void release_runs_noexcept() noexcept;
    /// Close all run stores, allowing PostgreSQL cleanup failures to  propagate.
    void release_runs();
    /// Close run stores and delete the handle's PostgreSQL memory context.
    void close_checked();
    /// Clear handles after a PostgreSQL error and leave its resources to transaction cleanup.
    void abandon_after_pg_error_noexcept() noexcept;
};

/**
 * @brief Merge general runs and visit each minimizer hash with its positions.
 * @tparam p_callback Callable accepting a hash and a read-only position cursor.
 * @param[in] on_kmer Callback invoked in hsh order.
 */
template <class p_callback> inline void fragment_index_storage::merge_runs(p_callback&& on_kmer) {
    if (!sorted_runs_.has_value()) {
        return;
    }
    g_last_index_merge_run_count =
        std::max<std::uint64_t>(g_last_index_merge_run_count, sorted_runs_->run_count());
    sorted_runs_->merge_runs(std::forward<p_callback>(on_kmer));
}

/**
 * @brief Merge runs and visit positions in segment order, then hash order.
 * @tparam p_callback Callable accepting a segment, hash and read-only position cursor.
 * @param[in] on_segment_kmer Callback invoked for each segment and  hash.
 */
template <class p_callback>
inline void fragment_index_storage::merge_segment_runs(p_callback&& on_segment_kmer) {
    if (segment_spill_pg_poisoned_) {
        minimizer_generation_storage_error_as_cpp(
            "poisoned minimizer-sequence merge storage reused after PostgreSQL error");
    }
    std::uint64_t total_runs = 0;
    for (const auto& [segment, runs] : segment_sorted_runs_) {
        (void)segment;
        total_runs += runs.run_count();
    }
    g_last_index_merge_run_count = std::max(g_last_index_merge_run_count, total_runs);
    try {
        // All spills have finished. Release every completed write buffer before
        // reserving memory for merge readers.
        for (auto& [segment, runs] : segment_sorted_runs_) {
            (void)segment;
            runs.release_completed_write_buffer();
        }
        active_segment_write_buffer_owner_.reset();
        for (auto& [segment, runs] : segment_sorted_runs_) {
            bool first_callback = true;
            runs.merge_runs(
                [&on_segment_kmer, segment, &first_callback](std::uint64_t hash, auto&& positions) {
                    if (first_callback) {
                        first_callback = false;
                        inject_minimizer_generation_failure_as_cpp(
                            minimizer_generation_failure_injection::minimizer_sequence_merge,
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
        active_segment_write_buffer_owner_.reset();
        segment_spill_binding_ = {};
        segment_spill_pg_poisoned_ = true;
        throw;
    }
}

/**
 * @brief Check that callers cannot directly change private segment spill state
 * @tparam p_storage Storage adapter type to inspect.
 */
template <class p_storage>
concept publicly_mutable_minimizer_sequence_segment_spill_state = requires(p_storage& storage) {
    storage.segment_sorted_runs_;
    storage.segment_spill_binding_;
    storage.segment_spill_pg_poisoned_;
    storage.next_segment_spill_generation_token_;
};

static_assert(!publicly_mutable_minimizer_sequence_segment_spill_state<fragment_index_storage>);
static_assert(!std::is_move_assignable_v<fragment_index_storage>);
static_assert(
    bioseq::indexing::is_index_chunked_storage_for<fragment_index_storage,
                                                   sequence_index_layout::fragment_chunks>,
    "fragment_index_storage must satisfy CHUNKED_PER_FRAGMENT");

} // namespace bioseq::storage::indexing
