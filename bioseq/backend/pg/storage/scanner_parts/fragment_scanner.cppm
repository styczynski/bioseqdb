/**
 * @file fragment_scanner.cppm
 * @brief PostgreSQL fragmented sequence scanning and cleanup.
 */
module;

#include <bioseq/pg_compat.h>

extern "C" {
#include "access/xact.h"
#include "access/relation.h"
#include "access/table.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"
}

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

export module bioseq.postgres.storage.scanner:fragment_scanner;

import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.postgres.configuration.core;
import bioseq.postgres.support;
import bioseq.common.seqan_adapter;              // seq::dna4 in static_assert below
import bioseq.postgres.storage.manager;     // seq_caches + open_sub_table_for_locator
import bioseq.storage.scanner;              // scanner concepts
import bioseq.storage.lazy_sequence;        // lazy_sequence + with_postgres_lazy_sequence
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

/// Callback shape used by the scanner's checked fragment-read adapter.
using scanner_pg_fragment_read_callback = void (*)(void*);

#ifdef BIOSEQ_TEST_INDEXES
/** One-shot PostgreSQL leaf failures for scanner unwind integration tests. */
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

/// Arm one scanner PostgreSQL leaf failure for the next matching call.
inline void arm_scanner_pg_leaf_failure(scanner_leaf_failure point) noexcept {
    g_scanner_pg_leaf_failure = point;
}

/** Test cleanup hook. Safe even when the selected leaf was never reached. */
inline void disarm_scanner_pg_leaf_failure() noexcept {
    g_scanner_pg_leaf_failure = scanner_leaf_failure::none;
}

/// Report whether one scanner PostgreSQL leaf failure remains armed.
[[nodiscard]] inline bool scanner_pg_leaf_failure_is_armed() noexcept {
    return g_scanner_pg_leaf_failure != scanner_leaf_failure::none;
}

/// Raise the armed scanner failure at its selected PostgreSQL leaf.
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
 * Adapter for the generic packed-range caller.
 * Every PostgreSQL leaf used by load_fragment is now converted locally with
 * bioseq::postgres::support::invoke_pg_as_cpp, so this function must not place a broad setjmp
 * boundary around a callback that refers to caller-owned vectors.
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

/// Fragment-streaming scanner for either PostgreSQL shard rows or borrowed
/// in-memory packed units. PostgreSQL mode owns a bounded fragment cache in a
/// caller-selected MemoryContext, while in-memory mode never allocates.
template <class p_sequence_instance>
struct postgres_sequence_scanner final {
    using storage_unit   = typename p_sequence_instance::storage_unit;
    using base_container = std::span<const storage_unit>;
    using traits         = typename compact_sequence_range<p_sequence_instance>::traits;

    static constexpr std::size_t fragment_storage_units =
        default_fragment_storage_units;

    [[nodiscard]] [[gnu::always_inline]]
    static constexpr bool is_fragmented() noexcept { return true; }

    [[nodiscard]] [[gnu::always_inline]]
    sequence_size_t bp_size() const noexcept { return logical_length_; }
    [[nodiscard]] [[gnu::always_inline]]
    sequence_size_t storage_unit_count() const noexcept {
        constexpr std::size_t lpu = traits::letters_per_unit;
        return (logical_length_ + lpu - 1) / lpu;
    }
    [[nodiscard]] [[gnu::always_inline]]
    sequence_size_t storage_byte_count() const noexcept {
        return storage_unit_count() * sizeof(storage_unit);
    }
    [[nodiscard]] [[gnu::always_inline]]
    std::size_t fragment_count() const noexcept { return fragment_count_; }
    [[nodiscard]] [[gnu::always_inline]]
    sequence_size_t estimated_bp_size() const noexcept { return logical_length_; }
    [[nodiscard]] [[gnu::always_inline]]
    std::size_t trailing_padding() const noexcept {
        return trailing_padding_;
    }

    /// Load one packed fragment into a bounded scanner slot.
    /// The returned view remains valid until that fragment is released or the
    /// scanner finishes its PostgreSQL resources.
    [[nodiscard]] [[gnu::always_inline]]
    base_container load_fragment(std::size_t fragment_no) const;

    /**
     * Copy the complete packed sequence into caller-owned resident storage.
     *
     * The PostgreSQL-backed route opens one dedicated sequential column reader,
     * seeks it to the stripe containing the locator's first row, and copies
     * visible contiguous fragment rows directly into `destination`. This avoids
     * one random-access lookup, one checked PostgreSQL boundary and one scanner
     * slot copy for every 8 KiB fragment when a large target is admitted for
     * query/segment workers.
     */
    void copy_packed_to(std::span<storage_unit> destination) const;

    /// Release one cached fragment slot without entering PostgreSQL.
    [[gnu::always_inline]]
    void release_fragment(std::size_t fragment_no) const noexcept;

    /**
     * Checked success-only finalization.  The destructor deliberately never
     * enters PostgreSQL: after a bridged ERROR the scanner's allocations are
     * still owned by its parent MemoryContext and must be abandoned to normal
     * statement/transaction cleanup.  A caller that completed successfully
     * calls this method so MemoryContextDelete failures remain authoritative.
     */
    void finish_pg_resources_as_cpp() const;

    /** Drop local handles without issuing a PostgreSQL call. */
    void abandon_pg_resources_noexcept() const noexcept;

    using fragment_read_callback = scanner_pg_fragment_read_callback;

    /**
     * Execute a fragment read behind a PostgreSQL error boundary.
     *
     * The cleanup callback remains part of the caller signature but is no
     * longer called. PostgreSQL leaves inside load_fragment are converted
     * to C++ exceptions before they can cross the caller, so ordinary C++
     * unwinding owns destruction of caller materialization state.
     */
    [[gnu::always_inline]] static void run_fragment_reads_with_error_cleanup(
            void* callback_context,
            fragment_read_callback read_body,
            fragment_read_callback cleanup_after_error) {
        scanner_pg_run_fragment_reads_with_error_cleanup(
            callback_context, read_body, cleanup_after_error);
    }

    /// Bind a locator-backed scanner to one registered MVCC snapshot and
    /// caller-owned read cache.
    explicit postgres_sequence_scanner(
        Oid db_oid, Oid tablespace_oid, Oid main_relfilenode,
        std::int16_t column_attno, const sequence_locator& locator,
        std::size_t last_fragment_padding,
        Snapshot registered_snapshot,
        seq_caches* caches, MemoryContext parent_mctx)
        : db_oid_(db_oid),
          tablespace_oid_(tablespace_oid),
          main_relfilenode_(main_relfilenode),
          column_attno_(column_attno),
          locator_(locator),
          logical_length_(locator.logical_length),
          trailing_padding_(last_fragment_padding),
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
        for (auto& slot : slots_) {
            slot.fragment_no = std::numeric_limits<std::size_t>::max();
            slot.bytes       = nullptr;
        }
    }

    postgres_sequence_scanner(const postgres_sequence_scanner&)            = delete;
    postgres_sequence_scanner& operator=(const postgres_sequence_scanner&) = delete;

    /// Transfer scanner slots and PostgreSQL cleanup responsibility.
    postgres_sequence_scanner(postgres_sequence_scanner&& other) noexcept
        : db_oid_(other.db_oid_),
          tablespace_oid_(other.tablespace_oid_),
          main_relfilenode_(other.main_relfilenode_),
          column_attno_(other.column_attno_),
          locator_(other.locator_),
          logical_length_(other.logical_length_),
          trailing_padding_(other.trailing_padding_),
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
    /// Transfer scanner slots and PostgreSQL cleanup responsibility.
    postgres_sequence_scanner& operator=(postgres_sequence_scanner&& other) noexcept {
        if (this == &other) return *this;
        this->~postgres_sequence_scanner();
        new (this) postgres_sequence_scanner(std::move(other));
        return *this;
    }

    ~postgres_sequence_scanner() { abandon_pg_resources_noexcept(); }

    [[nodiscard]] [[gnu::always_inline]]
    bool is_in_memory_mode() const noexcept { return mctx_ == nullptr; }

    /// Validate that this scanner was constructed for the supplied PostgreSQL
    /// source binding. No raw identity field is exposed: an administrative
    /// writer can only compare the relation, snapshot, and locator axes.
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
            && locator_.reserved_32 == locator.reserved_32
            && locator_.logical_length == locator.logical_length
            && locator_.first_fragment_row == locator.first_fragment_row
            && locator_.source_epoch == locator.source_epoch;
    }

private:
    /// One reusable packed-fragment slot owned by the scanner memory context.
    struct slot_t {
        std::size_t   fragment_no = std::numeric_limits<std::size_t>::max();
        storage_unit* bytes       = nullptr;
    };

    Oid                                                  db_oid_;
    Oid                                                  tablespace_oid_;
    Oid                                                  main_relfilenode_;
    std::int16_t                                         column_attno_;
    sequence_locator                                          locator_;
    sequence_size_t                                      logical_length_;
    std::size_t                                          trailing_padding_;
    std::size_t                                          fragment_count_;
    // Borrowed from the executor/admin owner. The scanner neither registers
    // nor unregisters it, and every cached fragment reader starts from this
    // statement snapshot. The column reader may own a refreshed registered
    // copy when it flushes same-transaction pending writes; it never mutates
    // the registration owned by this scanner's caller.
    Snapshot                                             snapshot_;
    seq_caches*                                          caches_;
    mutable MemoryContext                                mctx_;
    // In-memory backing span. Empty in PostgreSQL-backed mode.
    base_container                                       in_memory_data_;
    // A PostgreSQL ERROR may leave a cached column reader or fragment slot
    // partially mutated. Never reuse this scanner in the failed statement;
    // PostgreSQL's ResourceOwner/transaction cleanup owns the PG resources.
    mutable bool                                         poisoned_ = false;
    // Fixed-size slot ring matching the wrapper's
    // fragments_buffer_capacity (16). LRU-ish eviction in
    // allocate_slot_for_; the fragmented-sequence wrapper typically drives
    // release_fragment so the ring is rarely full. Used only in PG-backed mode.
    mutable std::array<slot_t, 16>                       slots_{};

    /// Return the logical unit count exposed by one validated fragment.
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

    /// Closed failure classification for sequential bulk materialization.
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
    [[nodiscard]] slot_t* find_slot_(std::size_t fragment_no) const noexcept {
        for (auto& slot : slots_) {
            if (slot.fragment_no == fragment_no && slot.bytes != nullptr) {
                return &slot;
            }
        }
        return nullptr;
    }

    /// Select an empty slot or evict the lowest-numbered resident fragment.
    [[nodiscard]] slot_t* allocate_slot_for_(std::size_t fragment_no) const noexcept {
        for (auto& slot : slots_) {
            if (slot.fragment_no == std::numeric_limits<std::size_t>::max()) {
                return &slot;
            }
        }
        // Defensive eviction — pick the smallest fragment_no resident.
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
    // Each PG leaf below throws through the ordinary C++ ABI.  Mark the
    // scanner before the first leaf and clear it only after a complete slot
    // publication; an interrupted read therefore cannot be reused.
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

    // Cold fragment read: a cache miss that reaches columnar storage.
    // This is the per-fragment work that dominates a large scanner_pg target
    // scan (the native aligner driving it is NO_POSTGRES and cannot itself poll
    // for interrupts). Honour cancellation here so a long scan responds to
    // pg_cancel_backend, and emit a throttled progress heartbeat (count-based;
    // the fmtlog timestamp header gives the rate) so the scan is observably
    // alive. Throttling keeps this off the hot per-fragment cost when logging.
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

    // Resolve the sub-table relation and reader through the source cache. The
    // system-catalog scan allocates its SysScanDesc in CurrentMemoryContext,
    // so keep the complete lookup in the same stable ownership domain as the
    // cached column reader.  invoke_pg_as_cpp restores the caller's context
    // and ResourceOwner at the checked leaf boundary.
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

    // Reader reuse is keyed by the caller-owned source snapshot. The
    // column reader may replace its own snapshot pointer with a refreshed
    // registered copy after flushing, so comparing read_state->snapshot would
    // incorrectly discard a reader bound to the same source snapshot.
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
            if (!TupleDescAttr(td, i)->attisdropped && needed) {
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
    if (static_cast<std::size_t>(VARSIZE(raw)) < bioseq::pg_types::seq::header_bytes()) {
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
    constexpr std::uint64_t letters_per_fragment =
        static_cast<std::uint64_t>(fragment_storage_units)
        * traits::letters_per_unit;
    constexpr std::uint8_t expected_alphabet =
        pgseq_supported_alphabets::template index_of<
            typename p_sequence_instance::alphabet_type>();
    const bool is_last_fragment = fragment_no + 1u == fragment_count_;
    const std::size_t raw_payload_bytes =
        static_cast<std::size_t>(VARSIZE(raw)) - bioseq::pg_types::seq::header_bytes();
    const std::uint64_t fragment_from =
        static_cast<std::uint64_t>(fragment_no) * letters_per_fragment;
    const bool fragment_geometry_valid = fragment_from < logical_length_
        && (is_last_fragment
            || logical_length_ - fragment_from >= letters_per_fragment);
    const std::uint64_t expected_fragment_bp = !fragment_geometry_valid
        ? 0u
        : (is_last_fragment
            ? logical_length_ - fragment_from
            : letters_per_fragment);
    if (static_cast<std::uint8_t>(raw->alphabet_code) != expected_alphabet
        || static_cast<std::uint8_t>(raw->storage_unit_byte_width)
            != sizeof(storage_unit)
        || (!is_last_fragment && raw->padding_letters != 0u)
        || (is_last_fragment
            && static_cast<std::size_t>(raw->padding_letters)
                != trailing_padding_)
        || !fragment_geometry_valid
        || static_cast<std::uint64_t>(raw->bp_size)
            != expected_fragment_bp
        || raw_payload_bytes != bytes_to_copy) {
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
        // palloc0 so the wrapper observes zeroes in the trailing region of the
        // last fragment.
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
        // release_fragment retains each of the fixed sixteen context-owned
        // buffers for reuse, avoiding pfree from a noexcept eviction path.
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

    struct bulk_copy_state {
        storage_unit* destination = nullptr;
        std::size_t destination_bytes = 0;
        std::size_t copied_fragments = 0;
        std::uint64_t expected_row = 0;
        std::uint64_t observed_row = 0;
        std::size_t observed_payload_bytes = 0;
        bulk_copy_status status = bulk_copy_status::ok;
    } state{
        .destination = destination.data(),
        .destination_bytes = destination.size_bytes(),
        .expected_row = locator_.first_fragment_row,
    };
    const ResourceOwner read_resource_owner = caches_->read_resource_owner;

    /*
     * Keep this as one checked PostgreSQL leaf. The callback owns only POD
     * state and raw pointers; the caller-owned resident vector remains outside
     * the PG_TRY frame. A PostgreSQL ERROR therefore returns through the C++
     * ABI without skipping a vector destructor, while the hot path pays one
     * sigsetjmp instead of one per fragment.
     */
    poisoned_ = true;
    bioseq::postgres::support::invoke_pg_as_cpp(
        [this, &state, read_resource_owner] {
        // The shard lookup itself opens a system-catalog scan, so enter the
        // dedicated scanner context and domain ResourceOwner before opening
        // the relation.  The checked leaf restores both caller globals.
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
                static_cast<std::size_t>(VARSIZE(raw));
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
    // In-memory mode has no slot ownership.
    if (is_in_memory_mode()) {
        return;
    }
    for (auto& slot : slots_) {
        if (slot.fragment_no == fragment_no && slot.bytes != nullptr) {
            // Keep the context-owned buffer available for a later fragment.
            // The success-only finalizer deletes all sixteen allocations at
            // once; error cleanup merely abandons their parent context.
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
    // No PG calls here.  Memory contexts are children of the caller-provided
    // statement/transaction context and PostgreSQL will reclaim them.
    mctx_ = nullptr;
    for (auto& slot : slots_) {
        slot.fragment_no = std::numeric_limits<std::size_t>::max();
        slot.bytes = nullptr;
    }
    poisoned_ = true;
}

// Fail at module load if scanner concept conformance changes.
static_assert(sequence_scanner<postgres_sequence_scanner<
    compact_sequence_stdc_instance<bioseq::pg_types::seq, seq::dna4, std::size_t{8}>>>);
static_assert(fragmented_sequence_scanner<postgres_sequence_scanner<
    compact_sequence_stdc_instance<bioseq::pg_types::seq, seq::dna4, std::size_t{8}>>>);


}  // namespace bioseq::storage
