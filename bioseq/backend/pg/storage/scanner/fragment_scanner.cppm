/**
 * @file scanner/fragment_scanner.cppm
 * @brief Read packed sequence fragments from PostgreSQL or  memory.
 */
module;

extern "C" {
#include "postgres.h"

#include "access/attnum.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tupdesc.h"
#include "access/xact.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "storage/lockdefs.h"
#include "utils/errcodes.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"
#include "varatt.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <span>
#include <utility>
#include <type_traits>

export module bioseq.postgres.storage.scanner.fragment_scanner;

import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.postgres.configuration.core;
import bioseq.postgres.support;
import bioseq.common.seqan_adapter;              // seq::dna4 in static_assert below
import bioseq.postgres.storage.manager;     // Fragment, relation and reader cache.
import bioseq.storage.scanner;              // scanner concepts
import bioseq.storage.lazy_sequence;
import bioseq.storage.coordinates;
import bioseq.storage.layout;
import bioseq.postgres.storage.physical;
import bioseq.storage.size;        // sequence_size_t
import bioseq.postgres.storage.metadata;    // stripe_metadata/find_stripe_by_row_number
import bioseq.sequences;           // compact_sequence_range, compact_sequence_stdc_instance
import bioseq.postgres.types.compact_sequence;
import bioseq.postgres.table.io;            // begin_column_read/read_column_row_by_number
import bioseq.parallelism.pool;

export namespace bioseq::storage {

/// Callback that reads fragments using a caller-supplied context.
using scanner_pg_fragment_read_callback = void (*)(void*);

#ifdef BIOSEQ_TEST_INDEXES
/** Places where tests can raise a PostgreSQL error during a fragment read. */
enum class scanner_leaf_failure : std::uint8_t {
    none,
    interrupt_before,
    interrupt_after,
    row_read_before,
    row_read_after,
    palloc_before,
    palloc_after,
};

inline thread_local scanner_leaf_failure g_scanner_pg_leaf_failure =
    scanner_leaf_failure::none;

/// Raise a test error on the next visit to the selected location.
/// @param point Failure location, or `none` to disable the error.
inline void arm_scanner_pg_leaf_failure(scanner_leaf_failure point) noexcept {
    g_scanner_pg_leaf_failure = point;
}

/** Disable the test error, including one whose location was never reached. */
inline void disarm_scanner_pg_leaf_failure() noexcept {
    g_scanner_pg_leaf_failure = scanner_leaf_failure::none;
}

/// @return Whether a test error is waiting to be triggered.
[[nodiscard]] inline bool scanner_pg_leaf_failure_is_armed() noexcept {
    return g_scanner_pg_leaf_failure != scanner_leaf_failure::none;
}

/// Raise and clear the test error if this is its selected location.
/// @param point Failure  location being entered.
/// @throws PostgreSQL ERROR when `point` is armed.
inline void inject_scanner_pg_leaf_failure(scanner_leaf_failure point) {
    if (g_scanner_pg_leaf_failure != point) {
        return;
    }
    g_scanner_pg_leaf_failure = scanner_leaf_failure::none;
    ereport(ERROR,
        (errcode(ERRCODE_INTERNAL_ERROR),
         errmsg("injected sequence scanner PostgreSQL leaf failure"),
         errdetail("failure point %u", static_cast<unsigned>(point))));
}
#endif

/**
 * Call the fragment rader with its supplied context.
 * PostgreSQL calls inside `load_fragment` convert errors to C++ exceptions,
 * so caller-owned vectors are destroyed during normal C++ stack unwinding.
 * Keep those vectors outside any PostgreSQL longjmp error handler.
 * @param callback_context Context passed to `read_body`.
 * @param read_body Callback, or null to do nothing.
 * @param cleanup_after_error Unused compatibility parameter.
 */
#if defined(__GNUC__) && !defined(__clang__)
[[gnu::noinline, gnu::noipa]]
#else
[[gnu::noinline]]
#endif
void scanner_pg_run_fragment_reads_with_error_cleanup(
        void* callback_context,
        scanner_pg_fragment_read_callback read_body,
        scanner_pg_fragment_read_callback cleanup_after_error) {
    (void)cleanup_after_error;
    if (read_body != nullptr) {
        read_body(callback_context);
    }
}

/// Read a sequence one packed fragment at a time.
/// PostgreSQL reads keep up to sixteen fragment buffers in a child memory
/// context. In-memory reads borrow  the supplied span without allocating.
/// @tparam p_sequence_instance Alphabet and packed storage type
///
/// Use PostgreSQL-backed instances only on the creating backend thread.
template <class p_sequence_instance>
struct postgres_sequence_scanner final {
    /// Physical fragment units: bytes for RLE, packed words otherwise.
    using traits = typename compact_sequence_range<p_sequence_instance>::traits;
    static constexpr bool is_run_encoded = traits::is_run_encoded;
    using storage_unit = std::conditional_t<is_run_encoded, std::uint8_t,
                                           typename p_sequence_instance::storage_unit>;
    /// Non-owning packed-unit view returned for fragments.
    using base_container = std::span<const storage_unit>;

    /// Packed-unit capacity of one physical fragment row.
    static constexpr std::size_t fragment_storage_units =
        default_fragment_storage_units
            * (is_run_encoded ? sizeof(typename p_sequence_instance::storage_unit) : 1u);

    /// @return Always `true` for this fragment-oriented scanner.
    [[nodiscard]] [[gnu::always_inline]]
    static constexpr bool is_fragmented() noexcept { return true; }

    /// @return Logical sequence length in letters.
    [[nodiscard]] [[gnu::always_inline]]
    sequence_size_t bp_size() const noexcept { return logical_length_; }
    /// @return Packed units needed for the logical sequence length.
    [[nodiscard]] [[gnu::always_inline]]
    sequence_size_t storage_unit_count() const noexcept {
        if constexpr (is_run_encoded) {
            return locator_.encoded_payload_bytes;
        } else {
            constexpr std::size_t lpu = traits::letters_per_unit;
            return (logical_length_ + lpu - 1) / lpu;
        }
    }
    /// @return Packed payload size in bytes.
    [[nodiscard]] [[gnu::always_inline]]
    sequence_size_t storage_byte_count() const noexcept {
        return storage_unit_count() * sizeof(storage_unit);
    }
    /// @return Number of fragments read from the table or the in-memory span.
    [[nodiscard]] [[gnu::always_inline]]
    std::size_t fragment_count() const noexcept { return fragment_count_; }
    /// @return Exact sequence length in letters.
    [[nodiscard]] [[gnu::always_inline]]
    sequence_size_t estimated_bp_size() const noexcept { return logical_length_; }
    /// @return Unused letters in the final packed unit.
    [[nodiscard]] [[gnu::always_inline]]
    std::size_t trailing_padding() const noexcept {
        return trailing_padding_;
    }

    /// Read one fragment, reusing a cached copy when available.
    /// @param fragment_no Zero-based, fragment number.
    /// @return Borrowed packed units. In PostgreSQL mode the view lasts until
    /// release, cleanup or eviction when all sixteen slots are occupied.
    /// In-memory views last as long as the supplied backing span.
    /// @throws C++ exception  on a PostgreSQL read, validation or allocation error.
    [[nodiscard]] [[gnu::always_inline]]
    base_container load_fragment(std::size_t fragment_no) const;

    /**
     * Copy the whole packed sequence into caller-owned memory.
     * PostgreSQL reads start at the locator's fqrst stripe and walk consecutive
     * fragment rows, avoiding a separate lookup and cache copy for each fragment.
     * @param destination Space for exactly `storage_unit_count()` packed units.
     * @throws C++ exception if the destination size is wrong, rows are missing
     * or out of order, or a payload fails the bulk-read checks.
     */
    void copy_packed_to(std::span<storage_unit> destination) const;

    /// Make a PostgreSQL cache slot available for reuse. Its buffer stays allocated.
    /// @param fragment_no Zero-based fragment whose view is no  longer needed.
    [[gnu::always_inline]]
    void release_fragment(std::size_t fragment_no) const noexcept;

    /**
     * Delete the scanner's memory context after a successful read.
     * Call this explicitly so PostgreSQL cleanup errors can reach the caller
     * The destructor only drops pointers, leaving PostgreSQL to reclaim memory
     * during statement or transaction cleanup after a read has failed.
     * @throws C++ exception if PostgreSQL cannot delete the context.
     */
    void finish_pg_resources_as_cpp() const;

    /**
     * Forget scanner buffers without calling PostgreSQL. The parent memory
     * context and ResourceOwner will release the remaining resources.
     */
    void abandon_pg_resources_noexcept() const noexcept;

    /// Type-erased callback used by the generic packed-range adapter.
    using fragment_read_callback = scanner_pg_fragment_read_callback;

    /**
     * Run the fragment callback. PostgreSQL errors raiseb  by `load_fragment`
     * become C++ exceptions, allowing caller-owned containers to be destroyed.
     * @param callback_context Context passed to `read_body`.
     * @param read_body Callback, or null to do nothing.
     * @param cleanup_after_error Unused compatibility parameter.
     */
    [[gnu::always_inline]] static void run_fragment_reads_with_error_cleanup(
            void* callback_context,
            fragment_read_callback read_body,
            fragment_read_callback cleanup_after_error) {
        scanner_pg_run_fragment_reads_with_error_cleanup(
            callback_context, read_body, cleanup_after_error);
    }

    /// Read the locator's fragment range using a registered MVCC snapshot.
    /// @param db_oid Parent database.
    /// @param tablespace_oid Parent tablespace.
    /// @param main_relfilenode Parent table file number.
    /// @param column_attno One-based, sequence column number.
    /// @param locator Fragment range and sequence length.
    /// @param last_fragment_padding Unused letters in the last packed unit.
    /// @param registered_snapshot Borrowed registered MVCC snapshot.
    /// @param caches Borrowed read cache.
    /// @param parent_mctx Parent for the scanner's fragment buffers.
    ///
    /// The snapshot and cache must outlive the scanner. All  three supplied
    /// resource pointers must be non-null, and the cache must be for reading.
    /// @throws C++ exception if these checks or context allocation fail.
    explicit postgres_sequence_scanner(
        Oid db_oid, Oid tablespace_oid, Oid main_relfilenode,
        std::int16_t column_attno, const sequence_locator& locator,
        std::size_t last_fragment_padding,
        Snapshot registered_snapshot,
        seq_caches* caches, MemoryContext parent_mctx,
        bioseq::cigar_space stored_space = bioseq::cigar_space::compatibility)
        : db_oid_(db_oid),
          tablespace_oid_(tablespace_oid),
          main_relfilenode_(main_relfilenode),
          column_attno_(column_attno),
          locator_(locator),
          logical_length_(locator.logical_length),
          trailing_padding_(last_fragment_padding),
          stored_space_(stored_space),
          fragment_count_(locator.fragment_count),
          snapshot_(registered_snapshot),
          caches_(caches),
          mctx_(nullptr),
          in_memory_data_{}
    {
        if (snapshot_ == nullptr
            || !IsMVCCSnapshot(snapshot_)
            || snapshot_->regd_count == 0) {
            bioseq::postgres::support::invoke_pg_as_cpp([] {
                bioseq_log<logging_level::error, logging_target::postgres>(
                    "postgres_sequence_scanner: requires a borrowed registered MVCC snapshot");
            });
        }
        if (caches_ == nullptr) {
            bioseq::postgres::support::invoke_pg_as_cpp([] {
                bioseq_log<logging_level::error, logging_target::postgres>(
                    "postgres_sequence_scanner: caches pointer must be non-null");
            });
        }
        if (caches_->side != seq_caches::access_side::read) {
            bioseq::postgres::support::invoke_pg_as_cpp([] {
                bioseq_log<logging_level::error, logging_target::postgres>(
                    "postgres_sequence_scanner: requires read-side seq_caches");
            });
        }
        if (parent_mctx == nullptr) {
            bioseq::postgres::support::invoke_pg_as_cpp([] {
                bioseq_log<logging_level::error, logging_target::postgres>(
                    "postgres_sequence_scanner: parent_mctx must be non-null");
            });
        }
        mctx_ = bioseq::postgres::support::invoke_pg_as_cpp([parent_mctx] {
            return AllocSetContextCreate(
                parent_mctx,
                "bioseq postgres_sequence_scanner",
                ALLOCSET_DEFAULT_SIZES);
        });
        for (auto& slot : slots_) {
            slot.fragment_no = std::numeric_limits<std::size_t>::max();
            slot.bytes       = nullptr;
        }
    }

    /// Borrow packed in-memory units without allocating PostgreSQL resources.
    /// The backing span must outlive this scanner.
    /// @param in_memory_data Borrowed packed units.
    /// @param total_bp_size Logical sequence length in letters.
    /// @param last_fragment_padding Unused letters in the final packed unit.
    explicit postgres_sequence_scanner(
        base_container in_memory_data,
        sequence_size_t total_bp_size,
        std::size_t last_fragment_padding) noexcept
        : db_oid_(InvalidOid),
          tablespace_oid_(InvalidOid),
          main_relfilenode_(InvalidOid),
          column_attno_(0),
          locator_{},
          logical_length_(total_bp_size),
          trailing_padding_(last_fragment_padding),
          fragment_count_(
              (in_memory_data.size() + fragment_storage_units - 1)
                  / fragment_storage_units),
          snapshot_(nullptr),
          caches_(nullptr),
          mctx_(nullptr),
          in_memory_data_(in_memory_data)
    {
        if constexpr (is_run_encoded) locator_.encoded_payload_bytes = static_cast<std::uint32_t>(in_memory_data.size());
        for (auto& slot : slots_) {
            slot.fragment_no = std::numeric_limits<std::size_t>::max();
            slot.bytes       = nullptr;
        }
    }

    postgres_sequence_scanner(const postgres_sequence_scanner&)            = delete;
    postgres_sequence_scanner& operator=(const postgres_sequence_scanner&) = delete;

    /// Take the other scanner's buffers and cleanup responsibility.
    /// @param other Scanner left unusable after the move.
    postgres_sequence_scanner(postgres_sequence_scanner&& other) noexcept
        : db_oid_(other.db_oid_),
          tablespace_oid_(other.tablespace_oid_),
          main_relfilenode_(other.main_relfilenode_),
          column_attno_(other.column_attno_),
          locator_(other.locator_),
          logical_length_(other.logical_length_),
          trailing_padding_(other.trailing_padding_),
          stored_space_(other.stored_space_),
          fragment_count_(other.fragment_count_),
          snapshot_(other.snapshot_),
          caches_(other.caches_),
          mctx_(other.mctx_),
          in_memory_data_(other.in_memory_data_),
          poisoned_(other.poisoned_),
          slots_(other.slots_)
    {
        other.mctx_           = nullptr;
        other.snapshot_       = nullptr;
        other.in_memory_data_ = base_container{};
        other.caches_         = nullptr;
        other.poisoned_       = true;
        for (auto& slot : other.slots_) {
            slot.fragment_no = std::numeric_limits<std::size_t>::max();
            slot.bytes       = nullptr;
        }
    }
    /// Replace this scanner with `other`, leaving its old buffers to parent cleanup.
    /// @param other Scanner left unusable after the move.
    /// @return This scanner.
    postgres_sequence_scanner& operator=(postgres_sequence_scanner&& other) noexcept {
        if (this == &other) return *this;
        this->~postgres_sequence_scanner();
        new (this) postgres_sequence_scanner(std::move(other));
        return *this;
    }

    /// Forget local pointers and leave PostgreSQL resources to parent cleanup
    ~postgres_sequence_scanner() { abandon_pg_resources_noexcept(); }

    /// @return Whether this  scanner has no PostgreSQL memory context.
    [[nodiscard]] [[gnu::always_inline]]
    bool is_in_memory_mode() const noexcept { return mctx_ == nullptr; }

    /// Check that this usable scanner belongs to the giveo table column, locator
    /// and snapshot. Also check that its read cache belongs to the same table.
    /// @return Whether all supplied fields match.
    [[nodiscard]] [[gnu::always_inline]]
    bool matches_pg_binding(
            Oid db_oid,
            Oid tablespace_oid,
            Oid main_relfilenode,
            std::int16_t column_attno,
            const sequence_locator& locator,
            std::size_t last_fragment_padding,
            Snapshot registered_snapshot) const noexcept {
        return mctx_ != nullptr
            && !poisoned_
            && in_memory_data_.empty()
            && caches_ != nullptr
            && caches_->side == seq_caches::access_side::read
            && caches_->main_database_oid == db_oid
            && caches_->main_tablespace_oid == tablespace_oid
            && caches_->main_relfilenode == main_relfilenode
            && db_oid_ == db_oid
            && tablespace_oid_ == tablespace_oid
            && main_relfilenode_ == main_relfilenode
            && column_attno_ == column_attno
            && snapshot_ == registered_snapshot
            && trailing_padding_ == last_fragment_padding
            && locator_.payload_version == locator.payload_version
            && locator_.bucket_index == locator.bucket_index
            && locator_.reserved_16 == locator.reserved_16
            && locator_.shard_index == locator.shard_index
            && locator_.fragment_count == locator.fragment_count
            && locator_.encoded_payload_bytes == locator.encoded_payload_bytes
            && locator_.logical_length == locator.logical_length
            && locator_.first_fragment_row == locator.first_fragment_row
            && locator_.source_epoch == locator.source_epoch;
    }

private:
    /// One reusable packed-fragment slot owned by the scanner memory context.
    struct slot_t {
        /// Resident fragment number, or the maximum value when unused.
        std::size_t   fragment_no = std::numeric_limits<std::size_t>::max();
        /// Context-owned fixed-size fragment buffer.
        storage_unit* bytes       = nullptr;
    };

    Oid                                                  db_oid_;
    Oid                                                  tablespace_oid_;
    Oid                                                  main_relfilenode_;
    std::int16_t                                         column_attno_;
    sequence_locator                                          locator_;
    sequence_size_t                                      logical_length_;
    std::size_t                                          trailing_padding_;
    bioseq::cigar_space stored_space_{bioseq::cigar_space::compatibility};
    std::size_t                                          fragment_count_;
    // Borrowed from the caller, which keeps this snapshot registered.
    // Readers may register a refreshed copy after flushing pending writes,
    // while this pointer continues to identify the caller's snapshot.
    Snapshot                                             snapshot_;
    seq_caches*                                          caches_;
    mutable MemoryContext                                mctx_;
    // In-memory backing span. Empty in PostgreSQL-backed mode.
    base_container                                       in_memory_data_;
    // A failed read may leave partial reader or buffer state. Reject further
    // reads and  let PostgreSQL clean up the remaining resources.
    mutable bool                                         poisoned_ = false;
    // Sixteen reusable PostgreSQL fragment buffers. If, none is free,
    // allocate_slot_for_ evicts the fragment with the lowest number.
    mutable std::array<slot_t, 16>                       slots_{};

    /// Count the packed units from this fragment that belong to the sequence.
    /// @param fragment_no Zero-based fragment number.
    /// @return Packed units belonging to the logical sequence.
    /// @throws C++ exception if its offset overflows or falls outside the sequence.
    [[nodiscard]] std::size_t exposed_fragment_storage_units_(
            std::size_t fragment_no) const {
        const auto offset = bioseq::common::checked_mul(
            static_cast<std::uint64_t>(fragment_no),
            static_cast<std::uint64_t>(fragment_storage_units));
        const std::uint64_t total = storage_unit_count();
        if (!offset.has_value() || *offset >= total) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [fragment_no, total] {
                    ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("postgres_sequence_scanner fragment span is invalid"),
                         errdetail(
                            "fragment=%zu total_storage_units=%llu",
                            fragment_no,
                            static_cast<unsigned long long>(total))));
                });
            return 0u;
        }
        return static_cast<std::size_t>(std::min(
            static_cast<std::uint64_t>(fragment_storage_units),
            total - *offset));
    }

    /// Result of a sequential copy, including reasons for an incomplete reaj.
    enum class bulk_copy_status : std::uint8_t {
        ok,
        missing_start_stripe,
        missing_fragment,
        noncontiguous_fragment,
        null_fragment,
        stub_fragment,
        payload_header_invalid,
        payload_too_large,
        destination_geometry_invalid,
    };

    /// Find a populated cache slot for one fragment number.
    /// @return Matching mutable slot, or null on a cache miss.
    [[nodiscard]] slot_t* find_slot_(std::size_t fragment_no) const noexcept {
        for (auto& slot : slots_) {
            if (slot.fragment_no == fragment_no && slot.bytes != nullptr) {
                return &slot;
            }
        }
        return nullptr;
    }

    /// Choose a free slot, or evict  the fragment with the lowest number.
    /// @param fragment_no Unused.
    /// @return Slot whose buffer can be reused.
    [[nodiscard]] slot_t* allocate_slot_for_(std::size_t fragment_no) const noexcept {
        for (auto& slot : slots_) {
            if (slot.fragment_no == std::numeric_limits<std::size_t>::max()) {
                return &slot;
            }
        }
        // Evict the fragment with the lowest number
        slot_t* victim = &slots_[0];
        for (auto& slot : slots_) {
            if (slot.fragment_no < victim->fragment_no) {
                victim = &slot;
            }
        }
        victim->fragment_no = std::numeric_limits<std::size_t>::max();
        (void)fragment_no;
        return victim;
    }
};

template <class p_sequence_instance>
[[gnu::always_inline]]
inline auto postgres_sequence_scanner<p_sequence_instance>::load_fragment(
    std::size_t fragment_no) const
    -> base_container
{
    if (poisoned_) {
        bioseq::postgres::support::invoke_pg_as_cpp([] {
            ereport(ERROR,
                (errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
                 errmsg("postgres_sequence_scanner cannot be reused after a "
                        "PostgreSQL read error")));
        });
    }
    // Mark this scanner unusable until the read finishes. A PostgreSQL error
    // becomes a C++ exception, leaving the flag set if any step fails.
    poisoned_ = true;
    if (fragment_no >= fragment_count_) {
        bioseq::postgres::support::invoke_pg_as_cpp([this, fragment_no] {
            bioseq_log<logging_level::error, logging_target::postgres>(
                "postgres_sequence_scanner: load_fragment({}) >= fragment_count_({})",
                fragment_no, fragment_count_);
        });
    }

    // In-memory mode: subspan the caller-owned span without allocation.
    if (is_in_memory_mode()) {
        const std::size_t start = fragment_no * fragment_storage_units;
        if (start >= in_memory_data_.size()) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [this, fragment_no, start] {
                bioseq_log<logging_level::error, logging_target::postgres>(
                    "postgres_sequence_scanner in-memory: fragment {} starts at unit {} but "
                    "backing span only has {} units", fragment_no, start,
                    in_memory_data_.size());
            });
        }
        const std::size_t end =
            std::min(start + fragment_storage_units, in_memory_data_.size());
        poisoned_ = false;
        return in_memory_data_.subspan(start, end - start);
    }

    const std::size_t exposed_storage_units =
        exposed_fragment_storage_units_(fragment_no);
    if (auto* s = find_slot_(fragment_no); s != nullptr) {
        poisoned_ = false;
        return base_container(s->bytes, exposed_storage_units);
    }

    // Check cancellation when a fragment must be read from PostgreSQL.
    // The aligner cannot call PostgreSQL itself. Occasional progress messages
    // help show that a long scan is still running.
    bioseq::postgres::support::invoke_pg_as_cpp([] {
#ifdef BIOSEQ_TEST_INDEXES
        inject_scanner_pg_leaf_failure(
            scanner_leaf_failure::interrupt_before);
#endif
        CHECK_FOR_INTERRUPTS();
#ifdef BIOSEQ_TEST_INDEXES
        inject_scanner_pg_leaf_failure(
            scanner_leaf_failure::interrupt_after);
#endif
    });
    if (bioseq::settings::diagnostics::logging::enabled) {
        static thread_local std::uint64_t cold_read_count = 0;
        if ((cold_read_count++ % 4000) == 0) {
            bioseq_log_progress<logging_level::debug>("[scanner_pg] cold fragment read #{} (fragment {}/{})",
                cold_read_count, fragment_no, fragment_count_);
        }
    }

    MemoryContext payload_mctx = caches_->read_state_mctx != nullptr
        ? caches_->read_state_mctx
        : mctx_;
    const ResourceOwner read_resource_owner = caches_->read_resource_owner;

    // The catalog lookup allocates memory and opens resources. Give it the
    // cache's context and ResourceOwner so they survive  with the reader.
    // invoke_pg_as_cpp restores the caller's context and owner afterward.
    Relation shard = bioseq::postgres::support::invoke_pg_as_cpp(
        [this, payload_mctx, read_resource_owner] {
        MemoryContextSwitchTo(payload_mctx);
        if (read_resource_owner != nullptr) {
            CurrentResourceOwner = read_resource_owner;
        }
        return open_sub_table_for_locator(
            db_oid_, tablespace_oid_, main_relfilenode_, column_attno_,
            locator_, AccessShareLock, caches_);
    });

    const std::uint64_t cache_key = seq_caches::key_for(column_attno_,
        static_cast<std::int16_t>(locator_.bucket_index),
        static_cast<std::int32_t>(locator_.shard_index));
    seq_caches::entry& entry = caches_->entries[cache_key];
    entry.rel = shard;

    // Compare the caller's snapshot when reusing a reader. The reader can
    // register a refreshed snapshot after flushing pending writes, so its own
    // snapshot pointer may change during the same scan.
    if (entry.read_state != nullptr
        && entry.read_snapshot_source != snapshot_) {
        bioseq::postgres::support::invoke_pg_as_cpp([read_resource_owner, &entry] {
            if (read_resource_owner != nullptr) {
                CurrentResourceOwner = read_resource_owner;
            }
            end_column_read(entry.read_state);
        });
        entry.read_state = nullptr;
        entry.read_snapshot_source = nullptr;
    }
    if (entry.read_state == nullptr) {
        TupleDesc td = RelationGetDescr(shard);
        bioseq::postgres::support::postgres_bitmapset attr_needed =
            bioseq::postgres::support::invoke_pg_as_cpp(
            [payload_mctx, td] {
                MemoryContext previous = MemoryContextSwitchTo(payload_mctx);
                auto result = bioseq::postgres::support::postgres_bitmapset::
                    make_member_range(td->natts);
                MemoryContextSwitchTo(previous);
                return result;
            });
        bioseq::postgres::support::postgres_integer_list projected;
        for (int i = 0; i < td->natts; ++i) {
            const bool needed = bioseq::postgres::support::invoke_pg_as_cpp(
                [&attr_needed, i] { return attr_needed.has_member(i); });
            if (!bioseq::postgres::support::tuple_descriptor_attribute(td, i)->attisdropped && needed) {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [payload_mctx, &projected, i] {
                    MemoryContext previous = MemoryContextSwitchTo(payload_mctx);
                    projected.append(static_cast<AttrNumber>(i + 1));
                    MemoryContextSwitchTo(previous);
                });
            }
        }
        List* projected_attributes = projected.release();
        entry.read_state = bioseq::postgres::support::invoke_pg_as_cpp(
            [payload_mctx, read_resource_owner, shard, td,
             projected_attributes, snapshot = snapshot_] {
            MemoryContextSwitchTo(payload_mctx);
            if (read_resource_owner != nullptr) {
                CurrentResourceOwner = read_resource_owner;
            }
            column_read_state* result = begin_column_read(
                shard, td, projected_attributes,
                /*whereClauseList=*/NIL, payload_mctx, snapshot,
                /*randomAccess=*/true, nullptr);
            flush_pending_writes_for_read(result);
            return result;
        });
        entry.read_snapshot_source = snapshot_;
        bioseq::postgres::support::invoke_pg_as_cpp(
            [&attr_needed] { attr_needed.clear(); });
    }

    const std::uint64_t row_no = locator_.first_fragment_row + fragment_no;
    Datum vals[1]  = { 0 };
    bool  nulls[1] = { true };
    const bool row_visible = bioseq::postgres::support::invoke_pg_as_cpp(
        [payload_mctx, read_resource_owner, &entry, row_no, &vals, &nulls] {
        MemoryContextSwitchTo(payload_mctx);
        if (read_resource_owner != nullptr) {
            CurrentResourceOwner = read_resource_owner;
        }
#ifdef BIOSEQ_TEST_INDEXES
        inject_scanner_pg_leaf_failure(
            scanner_leaf_failure::row_read_before);
#endif
        const bool result = read_column_row_by_number(
            entry.read_state, row_no, vals, nulls);
#ifdef BIOSEQ_TEST_INDEXES
        inject_scanner_pg_leaf_failure(
            scanner_leaf_failure::row_read_after);
#endif
        return result;
    });
    if (!row_visible) {
        bioseq::postgres::support::invoke_pg_as_cpp(
            [this, row_no, fragment_no] {
            bioseq_log<logging_level::error, logging_target::postgres>(
                "postgres_sequence_scanner: sub-table row {} missing (db={}, spc={}, "
                "relfilenode={}, attno={}, bucket={}, shard={}, fragment={}/{})",
                row_no,
                static_cast<unsigned>(db_oid_),
                static_cast<unsigned>(tablespace_oid_),
                static_cast<unsigned>(main_relfilenode_),
                static_cast<int>(column_attno_),
                static_cast<unsigned>(locator_.bucket_index),
                locator_.shard_index, fragment_no, fragment_count_);
        });
    }
    if (nulls[0]) {
        bioseq::postgres::support::invoke_pg_as_cpp(
            [this, row_no, fragment_no] {
            bioseq_log<logging_level::error, logging_target::postgres>(
                "postgres_sequence_scanner: sub-table row {} is NULL (fragment {} of {})",
                row_no, fragment_no, fragment_count_);
        });
    }
    bioseq::pg_types::seq* raw = bioseq::postgres::support::invoke_pg_as_cpp([&vals] {
        return reinterpret_cast<bioseq::pg_types::seq*>(PG_DETOAST_DATUM(vals[0]));
    });
    if (static_cast<std::size_t>(bioseq::postgres::support::varlena_4byte_size(raw)) < bioseq::pg_types::seq::header_bytes()) {
        bioseq::postgres::support::invoke_pg_as_cpp([row_no] {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("postgres_sequence_scanner fragment header is truncated"),
                 errdetail("row=%llu",
                    static_cast<unsigned long long>(row_no))));
        });
    }
    if (is_locator_stub(raw)) {
        bioseq::postgres::support::invoke_pg_as_cpp([row_no] {
            bioseq_log<logging_level::error, logging_target::postgres>(
                "postgres_sequence_scanner: sub-table fragment row {} is itself a stub "
                "(corruption)", row_no);
        });
    }

    constexpr std::size_t bytes_to_copy =
        fragment_storage_units * sizeof(storage_unit);
    constexpr std::uint64_t letters_per_fragment = [] {
        if constexpr (is_run_encoded) return static_cast<std::uint64_t>(fragment_storage_units);
        else return static_cast<std::uint64_t>(fragment_storage_units) * traits::letters_per_unit;
    }();
    const auto stored_extent = is_run_encoded
        ? static_cast<std::uint64_t>(locator_.encoded_payload_bytes) : logical_length_;
    constexpr std::uint8_t expected_alphabet =
        pgseq_supported_alphabets::template index_of<
            typename p_sequence_instance::alphabet_type>();
    const bool is_last_fragment = fragment_no + 1u == fragment_count_;
    const std::size_t raw_payload_bytes =
        static_cast<std::size_t>(bioseq::postgres::support::varlena_4byte_size(raw)) - bioseq::pg_types::seq::header_bytes();
    const std::uint64_t fragment_from =
        static_cast<std::uint64_t>(fragment_no) * letters_per_fragment;
    const bool fragment_geometry_valid = fragment_from < stored_extent
        && (is_last_fragment
            || stored_extent - fragment_from >= letters_per_fragment);
    const std::uint64_t expected_fragment_bp = !fragment_geometry_valid
        ? 0u
        : (is_last_fragment
            ? stored_extent - fragment_from
            : letters_per_fragment);
    if (static_cast<std::uint8_t>(raw->alphabet_code) != expected_alphabet
        || static_cast<std::uint8_t>(raw->storage_unit_byte_width)
            != sizeof(typename p_sequence_instance::storage_unit)
        || (!is_last_fragment && raw->padding_letters != 0u)
        || (is_last_fragment
            && static_cast<std::size_t>(raw->padding_letters)
                != trailing_padding_)
        || !fragment_geometry_valid
        || static_cast<std::uint64_t>(raw->bp_size)
            != expected_fragment_bp
        || raw_payload_bytes != bytes_to_copy
        || raw->encoding != (is_run_encoded ? bioseq::sequence_encoding::cigar_rle_fragment
                                           : bioseq::sequence_encoding::rank_packed)
        || raw->space != stored_space_
        || (is_run_encoded && is_last_fragment
            && !sequence_encoded_fragment_fill_is_zero(*raw, raw_payload_bytes))) {
        bioseq::postgres::support::invoke_pg_as_cpp(
            [row_no, raw_payload_bytes, is_last_fragment,
             expected_padding = trailing_padding_,
             expected_fragment_bp] {
                ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("postgres_sequence_scanner fragment metadata is corrupt"),
                     errdetail(
                        "row=%llu payload_bytes=%zu last_fragment=%s "
                        "expected_padding=%zu expected_fragment_bp=%llu",
                        static_cast<unsigned long long>(row_no),
                        raw_payload_bytes,
                        is_last_fragment ? "true" : "false",
                        expected_padding,
                        static_cast<unsigned long long>(
                            expected_fragment_bp))));
            });
    }
    slot_t* slot = allocate_slot_for_(fragment_no);
    storage_unit* dst = slot->bytes;
    if (dst == nullptr) {
        // Allocate a full fragment buffer. The payload is copied belod.
        dst = bioseq::postgres::support::invoke_pg_as_cpp(
            [this, bytes_to_copy] {
            MemoryContext previous = MemoryContextSwitchTo(mctx_);
#ifdef BIOSEQ_TEST_INDEXES
            inject_scanner_pg_leaf_failure(
                scanner_leaf_failure::palloc_before);
#endif
            auto* result = static_cast<storage_unit*>(palloc0(bytes_to_copy));
#ifdef BIOSEQ_TEST_INDEXES
            inject_scanner_pg_leaf_failure(
                scanner_leaf_failure::palloc_after);
#endif
            MemoryContextSwitchTo(previous);
            return result;
        });
        slot->bytes = dst;
    } else {
        // Reuse the buffer, kept by release_fragment.
        std::memset(dst, 0, bytes_to_copy);
    }
    std::memcpy(dst, raw->data, raw_payload_bytes);

    slot->fragment_no = fragment_no;
    poisoned_ = false;
    return base_container(dst, exposed_storage_units);
}

template <class p_sequence_instance>
inline void postgres_sequence_scanner<p_sequence_instance>::copy_packed_to(
    std::span<storage_unit> destination) const
{
    if constexpr (is_run_encoded) {
        if (destination.size() != storage_unit_count()) {
            bioseq::postgres::support::invoke_pg_as_cpp([] {
                ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
                    errmsg("encoded source destination has invalid byte length")));
            });
        }
        std::size_t copied{};
        for (std::size_t fragment = 0; fragment < fragment_count_; ++fragment) {
            const auto bytes = load_fragment(fragment);
            std::copy(bytes.begin(), bytes.end(), destination.begin() + copied);
            copied += bytes.size();
        }
        return;
    }
    const std::size_t required_units = static_cast<std::size_t>(storage_unit_count());
    if (destination.size() != required_units) {
        bioseq::postgres::support::invoke_pg_as_cpp(
            [destination, required_units] {
            bioseq_log<logging_level::error, logging_target::postgres>(
                "postgres_sequence_scanner: resident destination has {} packed units, "
                "expected {}",
                destination.size(), required_units);
        });
    }
    if (destination.empty()) {
        return;
    }

    std::fill(destination.begin(), destination.end(), storage_unit{});
    if (is_in_memory_mode()) {
        if (in_memory_data_.size() < destination.size()) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [this, destination] {
                bioseq_log<logging_level::error, logging_target::postgres>(
                    "postgres_sequence_scanner in-memory: backing span has {} packed "
                    "units, expected at least {}",
                    in_memory_data_.size(), destination.size());
            });
        }
        std::copy_n(
            in_memory_data_.data(), destination.size(), destination.data());
        return;
    }
    if (poisoned_) {
        bioseq::postgres::support::invoke_pg_as_cpp([] {
            ereport(ERROR,
                (errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
                 errmsg("postgres_sequence_scanner cannot be reused after a "
                        "PostgreSQL read error")));
        });
    }

    /// Copy progress and error details that can safely cross PostgreSQL longjmp.
    struct bulk_copy_state {
        /// Caller-owned destination storage.
        storage_unit* destination = nullptr;
        /// Destination capacity in bytes.
        std::size_t destination_bytes = 0;
        /// Number of fragment rows copied successfully.
        std::size_t copied_fragments = 0;
        /// Next required physical row number.
        std::uint64_t expected_row = 0;
        /// Most recently observed physical row number.
        std::uint64_t observed_row = 0;
        /// Most recently observed payload size in bytes.
        std::size_t observed_payload_bytes = 0;
        /// Result of the sequential fragment read.
        bulk_copy_status status = bulk_copy_status::ok;
    } state{
        .destination = destination.data(),
        .destination_bytes = destination.size_bytes(),
        .expected_row = locator_.first_fragment_row,
    };
    const ResourceOwner read_resource_owner = caches_->read_resource_owner;

    /*
     * Read the batch  inside one PostgreSQL error handler to avoid entering it
     * for every fragment. The callback holds only plain values and pointers.
     * The caller's destination vector stays outside the handler, where C++
     * exception unwinding can run its destructor if PostgreSQL raises an error
     */
    poisoned_ = true;
    bioseq::postgres::support::invoke_pg_as_cpp(
        [this, &state, read_resource_owner] {
        // Use the scanner's context and read ResourceOwner for catalog and
        // fragment reads. The error wrapper restores the caller's settings.
        MemoryContextSwitchTo(mctx_);
        if (read_resource_owner != nullptr) {
            CurrentResourceOwner = read_resource_owner;
        }
        Relation shard = open_sub_table_for_locator(
            db_oid_, tablespace_oid_, main_relfilenode_, column_attno_,
            locator_, AccessShareLock, caches_);
        const std::uint64_t cache_key = seq_caches::key_for(
            column_attno_,
            static_cast<std::int16_t>(locator_.bucket_index),
            static_cast<std::int32_t>(locator_.shard_index));
        seq_caches::entry& entry = caches_->entries[cache_key];
        entry.rel = shard;

        TupleDesc td = RelationGetDescr(shard);
        List* projected_attributes = list_make1_int(1);
        column_read_state* read_state = begin_column_read(
            shard, td, projected_attributes,
            /*whereClauseList=*/NIL, mctx_, snapshot_,
            /*randomAccess=*/true, nullptr);
        flush_pending_writes_for_read(read_state);

        stripe_metadata* first_stripe = find_stripe_by_row_number(
            shard, locator_.first_fragment_row, read_state->snapshot);
        if (first_stripe == nullptr
            || !set_stripe_read_state(read_state, first_stripe)) {
            state.status = bulk_copy_status::missing_start_stripe;
        }

        constexpr std::size_t fragment_bytes =
            fragment_storage_units * sizeof(storage_unit);
        while (state.status == bulk_copy_status::ok
               && state.copied_fragments < fragment_count_) {
            if ((state.copied_fragments & std::size_t{0x3ff}) == 0) {
                CHECK_FOR_INTERRUPTS();
            }

            Datum values[1] = {0};
            bool nulls[1] = {true};
            std::uint64_t row_no = 0;
            if (!read_next_column_row(
                    read_state, values, nulls, &row_no)) {
                state.status = bulk_copy_status::missing_fragment;
                break;
            }
            if (row_no < state.expected_row) {
                continue;
            }
            state.observed_row = row_no;
            if (row_no != state.expected_row) {
                state.status =
                    bulk_copy_status::noncontiguous_fragment;
                break;
            }
            if (nulls[0]) {
                state.status = bulk_copy_status::null_fragment;
                break;
            }

            bioseq::pg_types::seq* raw = reinterpret_cast<bioseq::pg_types::seq*>(
                PG_DETOAST_DATUM(values[0]));
            if (is_locator_stub(raw)) {
                state.status = bulk_copy_status::stub_fragment;
                break;
            }
            const std::size_t raw_total_bytes =
                static_cast<std::size_t>(bioseq::postgres::support::varlena_4byte_size(raw));
            if (raw_total_bytes < bioseq::pg_types::seq::header_bytes()) {
                state.status =
                    bulk_copy_status::payload_header_invalid;
                break;
            }
            const std::size_t raw_payload_bytes =
                raw_total_bytes - bioseq::pg_types::seq::header_bytes();
            state.observed_payload_bytes = raw_payload_bytes;
            if (raw_payload_bytes > fragment_bytes) {
                state.status = bulk_copy_status::payload_too_large;
                break;
            }

            const std::size_t destination_offset =
                state.copied_fragments * fragment_bytes;
            if (destination_offset > state.destination_bytes) {
                state.status =
                    bulk_copy_status::destination_geometry_invalid;
                break;
            }
            const std::size_t remaining_bytes =
                state.destination_bytes - destination_offset;
            const std::size_t copy_bytes =
                std::min(raw_payload_bytes, remaining_bytes);
            if (copy_bytes != 0) {
                std::memcpy(
                    reinterpret_cast<std::byte*>(state.destination)
                        + destination_offset,
                    raw->data,
                    copy_bytes);
            }
            ++state.copied_fragments;
            ++state.expected_row;
        }

        end_column_read(read_state);
    });

    if (state.status != bulk_copy_status::ok
        || state.copied_fragments != fragment_count_) {
        bioseq::postgres::support::invoke_pg_as_cpp([this, &state] {
            const char* reason = "unknown";
            switch (state.status) {
                case bulk_copy_status::ok:
                    reason = "incomplete fragment sequence";
                    break;
                case bulk_copy_status::missing_start_stripe:
                    reason = "missing start stripe";
                    break;
                case bulk_copy_status::missing_fragment:
                    reason = "missing fragment";
                    break;
                case bulk_copy_status::noncontiguous_fragment:
                    reason = "non-contiguous fragment row";
                    break;
                case bulk_copy_status::null_fragment:
                    reason = "NULL fragment";
                    break;
                case bulk_copy_status::stub_fragment:
                    reason = "fragment row is a locator stub";
                    break;
                case bulk_copy_status::payload_header_invalid:
                    reason = "fragment payload is shorter than the sequence header";
                    break;
                case bulk_copy_status::payload_too_large:
                    reason = "fragment payload exceeds fixed fragment size";
                    break;
                case bulk_copy_status::destination_geometry_invalid:
                    reason = "fragment locator exceeds resident destination geometry";
                    break;
            }
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("postgres_sequence_scanner bulk resident copy failed"),
                 errdetail(
                    "reason=%s expected_row=%llu observed_row=%llu "
                    "copied_fragments=%zu expected_fragments=%zu "
                    "payload_bytes=%zu",
                    reason,
                    static_cast<unsigned long long>(state.expected_row),
                    static_cast<unsigned long long>(state.observed_row),
                    state.copied_fragments,
                    fragment_count_,
                    state.observed_payload_bytes)));
        });
    }
    poisoned_ = false;
}

template <class p_sequence_instance>
[[gnu::always_inline]]
inline void postgres_sequence_scanner<p_sequence_instance>::release_fragment(
    std::size_t fragment_no) const noexcept
{
    // Borrowed in-memory fragments need no cache cleanup.
    if (is_in_memory_mode()) {
        return;
    }
    for (auto& slot : slots_) {
        if (slot.fragment_no == fragment_no && slot.bytes != nullptr) {
            // Keep the buffer for the next fragment. Explicit  cleanup deletes
            // all buffers together, and failed reads leave them to parent cleanup.
            slot.fragment_no = std::numeric_limits<std::size_t>::max();
            return;
        }
    }
}

template <class p_sequence_instance>
inline void postgres_sequence_scanner<p_sequence_instance>::finish_pg_resources_as_cpp() const {
    MemoryContext context = std::exchange(mctx_, nullptr);
    if (context == nullptr) {
        return;
    }
    for (auto& slot : slots_) {
        slot.fragment_no = std::numeric_limits<std::size_t>::max();
        slot.bytes = nullptr;
    }
    poisoned_ = true;
    bioseq::postgres::support::invoke_pg_as_cpp([context] {
        MemoryContext active = CurrentMemoryContext;
        MemoryContext walker = active;
        bool inside = false;
        while (walker != nullptr) {
            if (walker == context) {
                inside = true;
                break;
            }
            walker = MemoryContextGetParent(walker);
        }
        if (inside) {
            MemoryContextSwitchTo(MemoryContextGetParent(context));
        }
        MemoryContextDelete(context);
    });
}

template <class p_sequence_instance>
inline void postgres_sequence_scanner<
    p_sequence_instance>::abandon_pg_resources_noexcept() const noexcept {
    // PostgreSQL will reclaim these buffers with the caller's parent context.
    mctx_ = nullptr;
    for (auto& slot : slots_) {
        slot.fragment_no = std::numeric_limits<std::size_t>::max();
        slot.bytes = nullptr;
    }
    poisoned_ = true;
}

// Check scanner interfaces at compile time.
static_assert(sequence_scanner<postgres_sequence_scanner<
    compact_sequence_stdc_instance<bioseq::pg_types::seq, seq::dna4, std::size_t{8}>>>);
static_assert(fragmented_sequence_scanner<postgres_sequence_scanner<
    compact_sequence_stdc_instance<bioseq::pg_types::seq, seq::dna4, std::size_t{8}>>>);


}  // namespace bioseq::storage
