/*-------------------------------------------------------------------------
 *
 * storage/scanner.cppm
 *
 * `postgres_sequence_scanner<p_sequence_instance>` is the production PostgreSQL-
 * backed implementation of the `fragmented_sequence_scanner` concept.
 *
 * The scanner reads one fragment per sub-table row through `seq_caches` and
 * `read_column_row_by_number`, copies
 * `fragment_storage_units * sizeof(storage_unit)` bytes into a slot in a
 * per-scanner `MemoryContext`, and exposes the bytes through `load_fragment`.
 * Eviction (`release_fragment`) recycles one of the
 * fixed slot buffers; checked success finalization deletes the whole context.
 *
 * RSS per scanner is bounded at `fragments_buffer_capacity *
 * fragment_storage_units * sizeof(storage_unit)` (~16 × 8 KiB = 128 KiB on
 * dna4/uint64) irrespective of sequence length.
 *
 *-------------------------------------------------------------------------
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

export module bioseq.postgres.storage.scanner;

import bioseq.common.logging;
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

static_assert(__cplusplus >= 202302L, "bioseq requires C++23-or-later");

export namespace bioseq::storage {

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

inline void arm_scanner_pg_leaf_failure(scanner_leaf_failure point) noexcept {
    g_scanner_pg_leaf_failure = point;
}

/** Test cleanup hook; safe even when the selected leaf was never reached. */
inline void disarm_scanner_pg_leaf_failure() noexcept {
    g_scanner_pg_leaf_failure = scanner_leaf_failure::none;
}

[[nodiscard]] inline bool scanner_pg_leaf_failure_is_armed() noexcept {
    return g_scanner_pg_leaf_failure != scanner_leaf_failure::none;
}

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

// Production fragment-streaming scanner. Two construction modes:
//
//   1. PG-backed (production target path): constructed from a locator + seq_caches.
//      Reads one fragment per sub-table row via `seq_caches` +
//      `read_column_row_by_number`, memcpys the bytes into a slot in a per-scanner
//      MemoryContext, and exposes them through `load_fragment(n)`.
//      `release_fragment` pfrees the slot.
//
//   2. In-memory (query / inline target path): constructed from a span over an
//      existing `compact_sequence_range<p_sequence_config>`'s packed storage
//      units. `load_fragment(n)`
//      returns a subspan with NO allocation, NO MemoryContext. `release_fragment`
//      is a no-op (caller owns the bytes). This lets a query literal (always inline)
//      share the same scanner type as a PG-backed target without doubling the
//      dispatch_buffer template tree.
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

    // PG-backed constructor (production target path).
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

    // In-memory constructor (query / inline target path). The caller owns the
    // bytes; the scanner just stores a span into them, no allocation. The total
    // bp count + last-fragment padding letters mirror what the FRAGMENTED lazy
    // wrapper would expect from a PG-backed scanner.
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

    [[nodiscard]] slot_t* find_slot_(std::size_t fragment_no) const noexcept {
        for (auto& slot : slots_) {
            if (slot.fragment_no == fragment_no && slot.bytes != nullptr) {
                return &slot;
            }
        }
        return nullptr;
    }

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
        bioseq::postgres::support::invoke_pg_as_cpp([&] {
            bioseq_log<logging_level::error, logging_target::postgres>(
                "postgres_sequence_scanner: load_fragment({}) >= fragment_count_({})",
                fragment_no, fragment_count_);
        });
    }

    // In-memory mode: subspan the caller-owned span without allocation.
    if (is_in_memory_mode()) {
        const std::size_t start = fragment_no * fragment_storage_units;
        if (start >= in_memory_data_.size()) {
            bioseq::postgres::support::invoke_pg_as_cpp([&] {
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

    if (auto* s = find_slot_(fragment_no); s != nullptr) {
        poisoned_ = false;
        return base_container(s->bytes, fragment_storage_units);
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
        [&, payload_mctx, read_resource_owner] {
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
        bioseq::postgres::support::invoke_pg_as_cpp([&] {
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
        bioseq::postgres::support::invoke_pg_as_cpp([&] {
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
        dst = bioseq::postgres::support::invoke_pg_as_cpp([&] {
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
    return base_container(dst, fragment_storage_units);
}

template <class p_sequence_instance>
inline void postgres_sequence_scanner<p_sequence_instance>::copy_packed_to(
    std::span<storage_unit> destination) const
{
    const std::size_t required_units = static_cast<std::size_t>(storage_unit_count());
    if (destination.size() != required_units) {
        bioseq::postgres::support::invoke_pg_as_cpp([&] {
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
            bioseq::postgres::support::invoke_pg_as_cpp([&] {
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
    bioseq::postgres::support::invoke_pg_as_cpp([&] {
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

/**
 * Immutable PostgreSQL relation, locator, and lifetime resources from which
 * one callback-scoped lazy facade may be constructed.
 *
 * Keeping this value separate from the facade lets administrative writers
 * validate publication inputs without reaching through the facade to its
 * concrete scanner. Construction and validation remain in this module.
 */
template <class p_sequence_config>
class postgres_lazy_sequence_binding final {
public:
    using sequence_config = p_sequence_config;
    using alphabet_type =
        typename compact_sequence_range<p_sequence_config>::traits::alphabet_type;

    postgres_lazy_sequence_binding(
        Oid db_oid,
        Oid tablespace_oid,
        Oid main_relfilenode,
        std::int16_t column_attno,
        const sequence_locator& locator,
        std::size_t last_fragment_padding,
        Snapshot registered_snapshot,
        seq_caches* caches,
        MemoryContext parent_mctx) noexcept
        : db_oid_(db_oid),
          tablespace_oid_(tablespace_oid),
          main_relfilenode_(main_relfilenode),
          column_attno_(column_attno),
          locator_(locator),
          trailing_padding_(last_fragment_padding),
          registered_snapshot_(registered_snapshot),
          caches_(caches),
          parent_mctx_(parent_mctx) {}

    postgres_lazy_sequence_binding(
        const postgres_lazy_sequence_binding&) noexcept = default;
    postgres_lazy_sequence_binding& operator=(
        const postgres_lazy_sequence_binding&) = delete;

    [[nodiscard]] Oid db_oid() const noexcept { return db_oid_; }
    [[nodiscard]] Oid tablespace_oid() const noexcept {
        return tablespace_oid_;
    }
    [[nodiscard]] Oid main_relfilenode() const noexcept {
        return main_relfilenode_;
    }
    [[nodiscard]] std::int16_t column_attno() const noexcept {
        return column_attno_;
    }
    [[nodiscard]] const sequence_locator& locator() const noexcept {
        return locator_;
    }
    [[nodiscard]] std::size_t last_fragment_padding() const noexcept {
        return trailing_padding_;
    }
    [[nodiscard]] Snapshot registered_snapshot() const noexcept {
        return registered_snapshot_;
    }
    [[nodiscard]] seq_caches* caches() const noexcept { return caches_; }
    [[nodiscard]] MemoryContext parent_mctx() const noexcept {
        return parent_mctx_;
    }

    [[nodiscard]] static constexpr std::uint8_t alphabet_code() noexcept {
        return static_cast<std::uint8_t>(
            pgseq_supported_alphabets::template index_of<alphabet_type>());
    }
    [[nodiscard]] static constexpr std::uint8_t
    storage_unit_byte_width() noexcept {
        return static_cast<std::uint8_t>(
            sizeof(typename compact_sequence_range<p_sequence_config>::traits::storage_unit));
    }
    [[nodiscard]] static constexpr std::uint64_t
    fragment_bp_capacity() noexcept {
        return static_cast<std::uint64_t>(
            postgres_sequence_scanner<p_sequence_config>::fragment_storage_units)
            * static_cast<std::uint64_t>(
                compact_sequence_range<p_sequence_config>::traits::letters_per_unit);
    }
    [[nodiscard]] static constexpr std::size_t
    expected_last_fragment_padding(const sequence_locator& locator) noexcept {
        constexpr std::uint64_t letters_per_unit =
            compact_sequence_range<p_sequence_config>::traits::letters_per_unit;
        const std::uint64_t remainder =
            locator.logical_length % letters_per_unit;
        return static_cast<std::size_t>(
            remainder == 0u ? 0u : letters_per_unit - remainder);
    }

    [[nodiscard]] bool matches_same_source(
            const postgres_lazy_sequence_binding& other) const noexcept {
        return db_oid_ == other.db_oid_
            && tablespace_oid_ == other.tablespace_oid_
            && main_relfilenode_ == other.main_relfilenode_
            && column_attno_ == other.column_attno_
            && std::memcmp(&locator_, &other.locator_, sizeof(locator_)) == 0
            && trailing_padding_ == other.trailing_padding_
            && registered_snapshot_ == other.registered_snapshot_
            && caches_ == other.caches_
            && parent_mctx_ == other.parent_mctx_;
    }

private:
    const Oid db_oid_;
    const Oid tablespace_oid_;
    const Oid main_relfilenode_;
    const std::int16_t column_attno_;
    const sequence_locator locator_;
    const std::size_t trailing_padding_;
    Snapshot const registered_snapshot_;
    seq_caches* const caches_;
    MemoryContext const parent_mctx_;
};

namespace lazy_postgres_detail {

template <class p_sequence_config>
[[nodiscard]] constexpr compact_sequence_stdc_type_params
binding_type_params() noexcept {
    return {
        .alphabet_code = postgres_lazy_sequence_binding<p_sequence_config>::alphabet_code(),
        .storage_unit_byte_width =
            postgres_lazy_sequence_binding<p_sequence_config>::storage_unit_byte_width(),
    };
}

template <class p_sequence_config>
void validate_binding(const postgres_lazy_sequence_binding<p_sequence_config>& binding) {
    using traits = typename compact_sequence_range<p_sequence_config>::traits;
    constexpr std::uint64_t letters_per_unit = traits::letters_per_unit;
    constexpr std::uint64_t fragment_units =
        postgres_sequence_scanner<p_sequence_config>::fragment_storage_units;
    const auto& locator = binding.locator();
    const std::uint64_t packed_units =
        locator.logical_length / letters_per_unit
        + static_cast<std::uint64_t>(
            locator.logical_length % letters_per_unit != 0u);
    const std::uint64_t expected_fragments =
        packed_units / fragment_units
        + static_cast<std::uint64_t>(packed_units % fragment_units != 0u);
    const std::uint64_t expected_padding =
        packed_units * letters_per_unit - locator.logical_length;

    const bool valid_locator = locator.payload_version == sequence_locator_payload_version
        && locator.reserved_16 == 0u && locator.reserved_32 == 0u
        && locator.source_epoch != 0u
        && expected_fragments == locator.fragment_count
        && expected_padding == binding.last_fragment_padding();
    const bool valid_snapshot = binding.registered_snapshot() != nullptr
        && IsMVCCSnapshot(binding.registered_snapshot())
        && binding.registered_snapshot()->regd_count != 0;
    const bool valid_caches = binding.caches() != nullptr
        && binding.caches()->side == seq_caches::access_side::read
        && binding.caches()->main_database_oid == binding.db_oid()
        && binding.caches()->main_tablespace_oid == binding.tablespace_oid()
        && binding.caches()->main_relfilenode == binding.main_relfilenode();
    if (!valid_locator || !valid_snapshot || !valid_caches
        || binding.parent_mctx() == nullptr || binding.column_attno() <= 0) {
        bioseq::postgres::support::invoke_pg_as_cpp([] {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("lazy PostgreSQL sequence binding is invalid")));
        });
    }
}

template <class p_sequence_config>
struct call_state final {
    using scanner_type = postgres_sequence_scanner<p_sequence_config>;
    using sequence_type = lazy_sequence<p_sequence_config, scanner_type>;

    explicit call_state(
            const postgres_lazy_sequence_binding<p_sequence_config>& binding,
            MemoryContext session_context)
        : sequence(scanner_type{
              binding.db_oid(), binding.tablespace_oid(),
              binding.main_relfilenode(), binding.column_attno(),
              binding.locator(), binding.last_fragment_padding(),
              binding.registered_snapshot(), binding.caches(),
              session_context}) {
        if (!sequence.scanner().matches_pg_binding(
                binding.db_oid(), binding.tablespace_oid(),
                binding.main_relfilenode(), binding.column_attno(),
                binding.locator(), binding.last_fragment_padding(),
                binding.registered_snapshot())) {
            bioseq::postgres::support::invoke_pg_as_cpp([] {
                ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("lazy PostgreSQL scanner does not match its binding")));
            });
        }
    }

    sequence_type sequence;
};

#ifdef BIOSEQ_TEST_INDEXES
/* The direct PostgreSQL binding is itself a source-backed facade. Proxy the
 * test build's backend table to record the same read/resident/bulk-stream
 * operations without changing the scanner or algorithm surface. */
template <bool p_is_reversed, bool p_write_reversed, class p_sequence_config>
[[nodiscard]] const std::uint8_t* observed_call_state_read_at(
        void const* opaque,
        sequence_size_t from,
        std::size_t count,
        std::span<std::uint8_t> output) {
    auto const& state = *static_cast<call_state<p_sequence_config> const*>(opaque);
    note_pg_sequence_facade_source_stream_callback();
    const auto view = make_lazy_sequence_view(state.sequence);
    const auto cursor = view.template rank_reader<
        p_is_reversed, p_write_reversed, std::uint8_t, 2>();
    return cursor.read_at(from, count, output);
}

template <class p_sequence_config>
[[nodiscard]] sequence_size_t observed_call_state_bp_size(
        void const* opaque) noexcept {
    auto const& state = *static_cast<call_state<p_sequence_config> const*>(opaque);
    return state.sequence.bp_size();
}

template <class p_sequence_config>
[[nodiscard]] auto observed_call_state_resident_probe(
        void const* opaque,
        sequence_size_t from,
        sequence_size_t to,
        std::uint8_t requested_width) noexcept
    -> typename lazy_sequence_view<
        typename postgres_lazy_sequence_binding<p_sequence_config>::alphabet_type>::
        resident_packed_result {
    auto const& state = *static_cast<call_state<p_sequence_config> const*>(opaque);
    note_pg_sequence_facade_source_resident_probe();
    return make_lazy_sequence_view(state.sequence).try_resident_packed(
        from, to, requested_width);
}

template <class p_sequence_config>
[[nodiscard]] bool observed_call_state_copy_packed_sequence(
        void const* opaque,
        void* destination,
        std::size_t unit_count,
        std::uint8_t requested_width) {
    auto const& state = *static_cast<call_state<p_sequence_config> const*>(opaque);
    const std::size_t destination_bytes =
        requested_width == 0u
            || unit_count > (std::numeric_limits<std::size_t>::max)()
                         / requested_width
            ? 0u
            : unit_count * requested_width;
    note_pg_sequence_facade_contiguous_access();
    note_pg_sequence_facade_source_stream_callback();
    return make_lazy_sequence_view(state.sequence).try_copy_packed_sequence(
        std::span<std::uint8_t>{
            static_cast<std::uint8_t*>(destination), destination_bytes},
        unit_count, requested_width);
}

template <class p_sequence_config>
[[nodiscard]] bool observed_call_state_worker_shareable(
        void const* opaque) noexcept {
    auto const& state = *static_cast<call_state<p_sequence_config> const*>(opaque);
    return make_lazy_sequence_view(state.sequence).worker_shareable();
}

template <class p_sequence_config>
struct observed_call_state_adapter final {
    using alphabet_type =
        typename postgres_lazy_sequence_binding<p_sequence_config>::alphabet_type;
    using view_type = lazy_sequence_view<alphabet_type>;
    static inline const typename view_type::backend_ops ops{
        .bp_size = &observed_call_state_bp_size<p_sequence_config>,
        .read_at = {
            &observed_call_state_read_at<false, false, p_sequence_config>,
            &observed_call_state_read_at<false, true, p_sequence_config>,
            &observed_call_state_read_at<true, false, p_sequence_config>,
            &observed_call_state_read_at<true, true, p_sequence_config>,
        },
        .try_resident_packed = &observed_call_state_resident_probe<p_sequence_config>,
        .copy_packed_sequence = &observed_call_state_copy_packed_sequence<p_sequence_config>,
        .worker_shareable = &observed_call_state_worker_shareable<p_sequence_config>,
    };
};

template <class p_sequence_config>
[[nodiscard]] auto observed_call_state_view(call_state<p_sequence_config> const& state) {
    using adapter = observed_call_state_adapter<p_sequence_config>;
    return typename adapter::view_type{
        std::addressof(state), std::addressof(adapter::ops)};
}
#endif

template <class p_sequence_config>
struct call_state_factory_argument final {
    const postgres_lazy_sequence_binding<p_sequence_config>* binding;
};

template <class p_sequence_config>
[[nodiscard]] void* create_call_state(
        void* opaque,
        MemoryContext payload_context) {
    const auto& factory =
        *static_cast<const call_state_factory_argument<p_sequence_config>*>(opaque);
    call_state<p_sequence_config>* state = nullptr;
    ErrorData* pg_error_data = nullptr;
    bool cpp_error = false;
    bool unknown_cpp_error = false;
    char detail[512] = "std::exception without detail";
    try {
        validate_binding(*factory.binding);
        void* storage = bioseq::postgres::support::invoke_pg_as_cpp([payload_context] {
            return MemoryContextAllocZero(
                payload_context, sizeof(call_state<p_sequence_config>));
        });
        state = ::new (storage) call_state<p_sequence_config>(
            *factory.binding, payload_context);
    } catch (bioseq::postgres::support::postgres_error& error) {
        pg_error_data = error.release_error_data();
    } catch (const std::exception& error) {
        strlcpy(
            detail,
            error.what() == nullptr
                ? "std::exception without detail" : error.what(),
            sizeof(detail));
        cpp_error = true;
    } catch (...) {
        unknown_cpp_error = true;
    }
    if (pg_error_data != nullptr) {
        ReThrowError(pg_error_data);
    }
    if (cpp_error) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("lazy PostgreSQL call-state construction failed"),
             errdetail_internal("%s", detail)));
    }
    if (unknown_cpp_error) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("lazy PostgreSQL call-state construction failed"),
             errdetail_internal("unknown C++ exception")));
    }
    return state;
}

template <class p_sequence_config>
void finish_call_state(void* opaque, bool) {
    auto* state = static_cast<call_state<p_sequence_config>*>(opaque);
    state->sequence.scanner().finish_pg_resources_as_cpp();
    state->~call_state();
}

template <class p_sequence_config>
void abort_call_state_noexcept(void* opaque) noexcept {
    auto* state = static_cast<call_state<p_sequence_config>*>(opaque);
    state->sequence.scanner().abandon_pg_resources_noexcept();
    state->~call_state();
}

template <class p_sequence_config>
[[nodiscard]] call_state<p_sequence_config>* register_call_state(
        pg_sequence_read_session& session,
        const postgres_lazy_sequence_binding<p_sequence_config>& binding) {
    const pg_sequence_read_entry_key key{
        .source = {
            .control_address = reinterpret_cast<std::uintptr_t>(&binding),
            .generation = 0u,
        },
        .locator = binding.locator(),
        .stored_type_params = binding_type_params<p_sequence_config>(),
    };
    call_state_factory_argument<p_sequence_config> factory{.binding = &binding};
    return static_cast<call_state<p_sequence_config>*>(find_or_create_pg_sequence_read_entry(
        session, key, &factory, &create_call_state<p_sequence_config>,
        &finish_call_state<p_sequence_config>,
        &abort_call_state_noexcept<p_sequence_config>));
}

template <class p_function>
void with_session(MemoryContext parent_mctx, p_function&& fn) {
    std::exception_ptr callback_failure;
    pg_sequence_read_session* volatile session = nullptr;
    PG_TRY();
    {
        auto* local_session = begin_pg_sequence_read_session(parent_mctx);
        session = local_session;
        try {
            std::forward<p_function>(fn)(*local_session);
            finish_pg_sequence_read_session(local_session, true);
        } catch (...) {
            callback_failure = std::current_exception();
            abort_pg_sequence_read_session_noexcept(local_session);
        }
        session = nullptr;
    }
    PG_CATCH();
    {
        abort_pg_sequence_read_session_noexcept(
            const_cast<pg_sequence_read_session*>(session));
        session = nullptr;
        PG_RE_THROW();
    }
    PG_END_TRY();
    if (callback_failure != nullptr) {
        std::rethrow_exception(std::move(callback_failure));
    }
}

} // namespace lazy_postgres_detail

/** Invoke one callback with only the canonical alphabet-specialized facade. */
template <class p_sequence_config, class p_function>
inline void with_postgres_lazy_sequence(
        postgres_lazy_sequence_binding<p_sequence_config> binding,
        p_function&& fn) {
    lazy_postgres_detail::with_session(
        binding.parent_mctx(),
        [&binding, &fn](pg_sequence_read_session& session) {
            auto* state =
                lazy_postgres_detail::register_call_state(session, binding);
            if (state == nullptr) {
                bioseq::postgres::support::invoke_pg_as_cpp([] {
                    ereport(ERROR,
                        (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                         errmsg("lazy PostgreSQL read session rejected its binding")));
                });
            }
#ifdef BIOSEQ_TEST_INDEXES
            std::forward<p_function>(fn)(observed_call_state_view(*state));
#else
            std::forward<p_function>(fn)(make_lazy_sequence_view(state->sequence));
#endif
        });
}

/**
 * Invoke a two-input callback under one read session. Identical bindings
 * reuse one concrete call state, so both facades share fragment-cache state.
 */
template <class p_left_sequence_config, class p_right_sequence_config, class p_function>
inline void with_postgres_lazy_sequence(
        postgres_lazy_sequence_binding<p_left_sequence_config> left_binding,
        postgres_lazy_sequence_binding<p_right_sequence_config> right_binding,
        p_function&& fn) {
    using left_alphabet =
        typename postgres_lazy_sequence_binding<p_left_sequence_config>::alphabet_type;
    using right_alphabet =
        typename postgres_lazy_sequence_binding<p_right_sequence_config>::alphabet_type;
    static_assert(std::is_same_v<left_alphabet, right_alphabet>,
                  "two lazy PostgreSQL inputs must share one alphabet");
    if (left_binding.parent_mctx() != right_binding.parent_mctx()) {
        bioseq::postgres::support::invoke_pg_as_cpp([] {
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("two lazy PostgreSQL bindings must share one parent memory context")));
        });
    }
    lazy_postgres_detail::with_session(
        left_binding.parent_mctx(),
        [&left_binding, &right_binding, &fn](
                pg_sequence_read_session& session) {
            auto* left = lazy_postgres_detail::register_call_state(
                session, left_binding);
            if (left == nullptr) {
                bioseq::postgres::support::invoke_pg_as_cpp([] {
                    ereport(ERROR,
                        (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                         errmsg("lazy PostgreSQL read session rejected its left binding")));
                });
            }

            if constexpr (std::is_same_v<p_left_sequence_config, p_right_sequence_config>) {
                if (left_binding.matches_same_source(right_binding)) {
                    const auto shared = make_lazy_sequence_view(left->sequence);
                    std::forward<p_function>(fn)(shared, shared);
                    return;
                }
            }

            auto* right = lazy_postgres_detail::register_call_state(
                session, right_binding);
            if (right == nullptr) {
                bioseq::postgres::support::invoke_pg_as_cpp([] {
                    ereport(ERROR,
                        (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                         errmsg("lazy PostgreSQL read session rejected its right binding")));
                });
            }
            std::forward<p_function>(fn)(make_lazy_sequence_view(left->sequence),
                                 make_lazy_sequence_view(right->sequence));
        });
}

// Construct an in-memory postgres_sequence_scanner from an existing
// compact_sequence_range. This wraps query literals or already-hydrated
// inline targets so they share the production
// `postgres_sequence_scanner<p_sequence_config>` template
// type with stub-backed (PG-backed) targets — eliminating the need for a
// separate in-memory scanner template in the dispatch_buffer pipeline tree.
//
// `range` must outlive the returned scanner: its `read_cptr(cbegin())` pointer
// is captured into the scanner's backing span, and the scanner just slices it
// per fragment with no copy.
template <class p_sequence_config>
[[nodiscard]] [[gnu::always_inline]]
inline postgres_sequence_scanner<p_sequence_config> make_in_memory_postgres_sequence_scanner(
    const compact_sequence_range<p_sequence_config>& range) noexcept
{
    using storage_unit = typename compact_sequence_range<p_sequence_config>::traits::storage_unit;
    const std::size_t units = static_cast<std::size_t>(range.size());
    std::span<const storage_unit> backing =
        units == 0
            ? std::span<const storage_unit>{}
            : std::span<const storage_unit>(range.read_cptr(range.cbegin()), units);
    const sequence_size_t total_bp = static_cast<sequence_size_t>(range.bp_size());
    const std::size_t padding = static_cast<std::size_t>(range.padding());
    return postgres_sequence_scanner<p_sequence_config>{backing, total_bp, padding};
}

// ---------------------------------------------------------------------------
// Shared PostgreSQL source registry and transaction-scoped source domains.
// ---------------------------------------------------------------------------

enum class pg_sequence_source_sweep_boundary : std::uint8_t {
    provider_end,
    upper_lease_end,
    rescan,
    standalone_end,
    transaction_end,
    subtransaction_end,
};

enum class pg_sequence_source_provider_kind : std::uint8_t {
    scan,
    standalone_index,
};

namespace sequence_source_detail {
struct pg_sequence_source_domain;
}

/**
 * Transaction-owned registry for one verified relation/storage/snapshot flow.
 * It contains no Relation pointer and is never shared through DSM.
 */
struct pg_sequence_source_registry {
    pg_sequence_backend_thread_token backend_thread;
    Oid user_relid = InvalidOid;
    Oid database_oid = InvalidOid;
    Oid tablespace_oid = InvalidOid;
    Oid main_relfilenode = InvalidOid;
    std::uint64_t main_storage_id = 0u;
    /* Bioseq's metapage storage id is the immutable binding generation.  Keep
     * the named copy in the registry key so a rel/attno pair can never reuse a
     * domain after its physical binding has been replaced. */
    std::uint64_t binding_generation = 0u;
    Snapshot authoritative_snapshot = nullptr;
    SubTransactionId creating_subid = InvalidSubTransactionId;
    std::uint64_t generation = 0u;
    sequence_source_detail::pg_sequence_source_domain* domains = nullptr;
    pg_sequence_source_registry* next = nullptr;
    std::uint32_t active_leases = 0u;
    bool accepting_lookups = false;
    bool provider_closed = false;
    bool lease_fault = false;
    bool xact_ended = false;
};

/** A checked upper/provider retain on a registry. */
struct pg_sequence_source_registry_lease {
    pg_sequence_source_registry* registry = nullptr;
    std::uint64_t provider_generation = 0u;
    bool owned = false;
};

/** Provider control owned by a base scan or standalone index owner. */
struct pg_sequence_source_provider {
    pg_sequence_source_registry_lease lease{};
    pg_sequence_source_provider* next = nullptr;
    pg_sequence_source_provider_kind kind =
        pg_sequence_source_provider_kind::scan;
    bool open = false;
};

/** One source-provider route retained by an upper executor owner. */
struct pg_sequence_source_route {
    Oid relation_oid = InvalidOid;
    pg_sequence_source_registry_lease lease{};
};

static_assert(std::is_trivially_copyable_v<pg_sequence_source_route>);

/** Query-lifetime routing for joins, appends and nested upper wrappers.
 *
 * A PostgreSQL self join has independent registries even when every binding
 * names the same physical relation. The provider generation is therefore the
 * route key; relation_oid supplies the physical-relation axis. Two routes stay inline
 * for the common binary-join case; larger legal plans grow in the executor's
 * query MemoryContext, before any new lease is retained.
 */
struct pg_sequence_source_route_set {
    std::array<pg_sequence_source_route, 2u> inline_routes{};
    pg_sequence_source_route* dynamic_routes = nullptr;
    std::size_t dynamic_capacity = 0u;
    std::size_t count = 0u;
    MemoryContext allocation_context = nullptr;
};

#ifdef BIOSEQ_TEST_INDEXES
enum class sequence_source_failure : std::uint8_t {
  none,
  provider_after_registry_control,
  provider_after_provider_control,
  domain_after_control,
  domain_after_context,
  domain_after_owner,
  domain_after_snapshot,
  domain_after_caches,
  close_before_caches,
  close_before_snapshot,
  close_before_before_locks,
  close_before_locks,
  close_before_after_locks,
  read_before_scanner,
  mmsearch_output_after_promotion,
};

struct sequence_source_stats {
  std::uint64_t registries_created = 0u;
  std::uint64_t registries_destroyed = 0u;
  std::uint64_t providers_created = 0u;
  std::uint64_t providers_destroyed = 0u;
  std::uint64_t scan_providers_created = 0u;
  std::uint64_t standalone_providers_created = 0u;
  std::uint64_t domains_created = 0u;
  std::uint64_t domains_reused = 0u;
  std::uint64_t domains_destroyed = 0u;
  std::uint64_t cache_sets_created = 0u;
  std::uint64_t cache_sets_destroyed = 0u;
  std::uint64_t snapshots_registered = 0u;
  std::uint64_t snapshots_unregistered = 0u;
  std::uint64_t scanner_entries_created = 0u;
  std::uint64_t scanner_entry_hits = 0u;
  std::uint64_t leases_acquired = 0u;
  std::uint64_t leases_released = 0u;
  std::uint64_t provider_closed_with_upper_leases = 0u;
  std::uint64_t last_upper_lease_after_provider_end = 0u;
  std::uint64_t cleanup_commit = 0u;
  std::uint64_t cleanup_abort = 0u;
  std::uint64_t sweeps = 0u;
  std::uint64_t swept_domains = 0u;
  std::uint64_t subxact_promotions = 0u;
  std::uint64_t subxact_aborts = 0u;
  std::uint64_t pre_prepare_rejections = 0u;
  std::uint64_t release_before_locks = 0u;
  std::uint64_t release_locks = 0u;
  std::uint64_t release_after_locks = 0u;
  std::uint64_t wrong_thread_rejections = 0u;
  std::uint64_t refcount_faults = 0u;
  std::uint64_t lease_faults = 0u;
  std::uint64_t fault_injections = 0u;
  std::uint64_t read_fault_injections = 0u;
  std::uint64_t mmsearch_output_context_resets = 0u;
  std::uint64_t mmsearch_output_locator_promotions = 0u;
  std::uint64_t mmsearch_output_pg_error_restores = 0u;
  std::uint64_t mmsearch_output_restore_mismatches = 0u;
  std::uint64_t facade_source_stream_callbacks = 0u;
  std::uint64_t facade_source_resident_probes = 0u;
  std::uint64_t facade_declared_materializations = 0u;
  std::uint64_t facade_completed_materializations = 0u;
  std::uint64_t facade_poison_hits = 0u;
  std::uint64_t live_registries = 0u;
  std::uint64_t live_providers = 0u;
  std::uint64_t live_domains = 0u;
  std::uint64_t live_leases = 0u;
  std::uint64_t live_cache_sets = 0u;
  std::uint64_t live_snapshots = 0u;
};

inline thread_local sequence_source_failure g_pg_sequence_source_failure =
    sequence_source_failure::none;
inline thread_local sequence_source_stats
    g_pg_sequence_source_stats{};

inline void
arm_pg_sequence_source_failure(sequence_source_failure point) noexcept {
  g_pg_sequence_source_failure = point;
}

inline void disarm_pg_sequence_source_failure() noexcept {
  g_pg_sequence_source_failure = sequence_source_failure::none;
  disarm_pg_sequence_facade_test_poison();
}

[[nodiscard]] inline sequence_source_stats
pg_sequence_source_stats() noexcept;

inline void reset_pg_sequence_source_stats() noexcept {
  g_pg_sequence_source_stats = {};
  reset_pg_sequence_facade_test_counters();
}
#endif

namespace sequence_source_detail {

enum class domain_lifecycle : std::uint8_t {
  constructing,
  live,
  cleaning,
  faulted,
  invalid,
  released,
};

enum class owner_release_stage_bits : std::uint8_t {
    none = 0u,
    before_locks = 1u,
    locks = 2u,
    after_locks = 4u,
};

[[nodiscard]] constexpr std::uint8_t stage_bit(
        owner_release_stage_bits stage) noexcept {
    return static_cast<std::uint8_t>(stage);
}

struct pg_sequence_source_domain {
    pg_sequence_backend_thread_token backend_thread;
    pg_sequence_source_registry* registry = nullptr;
    pg_sequence_source_domain* next = nullptr;
    MemoryContext payload_context = nullptr;
    ResourceOwner resource_owner = nullptr;
    Snapshot registered_snapshot = nullptr;
    seq_caches* read_caches = nullptr;
    AttrNumber column_attno = InvalidAttrNumber;
    compact_sequence_stdc_type_params stored_type{};
    std::uint64_t binding_generation = 0u;
    SubTransactionId creating_subid = InvalidSubTransactionId;
    std::uint64_t generation = 0u;
    std::uint32_t refcount = 0u;
    domain_lifecycle lifecycle = domain_lifecycle::constructing;
    std::uint8_t released_owner_stages = 0u;
    bool registry_token_owned = false;
    bool reclaimable_hint = false;
    bool refcount_fault = false;
};

struct source_scanner_entry {
    pg_sequence_source_domain* domain = nullptr;
    void* concrete_scanner = nullptr;
    void (*read_ranks)(source_scanner_entry&, sequence_transform,
                       std::uint64_t, std::span<std::uint8_t>) = nullptr;
    void (*finish)(source_scanner_entry&) = nullptr;
    void (*abort_noexcept)(source_scanner_entry&) noexcept = nullptr;
    bool constructed = false;
    bool domain_retained = false;
};

struct source_scanner_factory_argument {
    pg_sequence_source_domain* domain = nullptr;
    sequence_locator locator{};
    compact_sequence_stdc_type_params stored_type{};
};

inline pg_sequence_source_registry *registries = nullptr;
inline pg_sequence_source_provider *providers = nullptr;
inline std::uint64_t next_generation = 1u;
inline bool callbacks_registered = false;

#ifdef BIOSEQ_TEST_INDEXES
[[nodiscard]] inline sequence_source_stats
collect_pg_sequence_source_stats() noexcept {
  sequence_source_stats result = g_pg_sequence_source_stats;
  const sequence_facade_test_counters facade =
      pg_sequence_facade_test_counters();
  result.facade_source_stream_callbacks = facade.source_stream_callbacks;
  result.facade_source_resident_probes = facade.source_resident_probes;
  result.facade_declared_materializations = facade.declared_materializations;
  result.facade_completed_materializations = facade.completed_materializations;
  result.facade_poison_hits = facade.poison_hits;
  result.live_registries = 0u;
  result.live_providers = 0u;
  result.live_domains = 0u;
  result.live_leases = 0u;
  result.live_cache_sets = 0u;
  result.live_snapshots = 0u;
  for (auto *registry = registries; registry != nullptr;
       registry = registry->next) {
    ++result.live_registries;
    result.live_leases += registry->active_leases;
    for (auto *domain = registry->domains; domain != nullptr;
         domain = domain->next) {
      ++result.live_domains;
      if (domain->read_caches != nullptr) {
        ++result.live_cache_sets;
      }
      if (domain->registered_snapshot != nullptr) {
        ++result.live_snapshots;
      }
    }
  }
  for (auto *provider = providers; provider != nullptr;
       provider = provider->next) {
    ++result.live_providers;
  }
  return result;
}
#endif

#ifdef BIOSEQ_TEST_INDEXES
inline void inject_pg_sequence_source_failure(sequence_source_failure point) {
    if (g_pg_sequence_source_failure != point) {
        return;
    }
    g_pg_sequence_source_failure = sequence_source_failure::none;
    ++g_pg_sequence_source_stats.fault_injections;
    if (point == sequence_source_failure::read_before_scanner) {
        ++g_pg_sequence_source_stats.read_fault_injections;
    }
    ereport(ERROR,
        (errcode(ERRCODE_INTERNAL_ERROR),
         errmsg("injected PostgreSQL sequence source-domain failure"),
         errdetail("failure point %u", static_cast<unsigned>(point))));
}
#endif

[[nodiscard]] inline bool same_locator(
        const sequence_locator& left, const sequence_locator& right) noexcept {
    return std::memcmp(&left, &right, sizeof(sequence_locator)) == 0;
}

[[nodiscard]] inline std::uint64_t allocate_generation() {
    if (next_generation == 0u
        || next_generation == std::numeric_limits<std::uint64_t>::max()) {
        ereport(ERROR,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
             errmsg("bioseq source-domain generation space exhausted")));
    }
    return next_generation++;
}

[[nodiscard]] inline bool domain_is_readable(
        const pg_sequence_source_domain& domain) noexcept {
    return domain.lifecycle == domain_lifecycle::live
        && !domain.refcount_fault
        && domain.registry_token_owned
        && domain.registered_snapshot != nullptr
        && domain.resource_owner != nullptr
        && domain.read_caches != nullptr;
}

inline void retain_domain(void* opaque) noexcept {
    auto* domain = static_cast<pg_sequence_source_domain*>(opaque);
    if (domain == nullptr) {
        return;
    }
    if (domain->refcount == 0u
        || domain->refcount == std::numeric_limits<std::uint32_t>::max()) {
        domain->refcount_fault = true;
        domain->lifecycle = domain_lifecycle::faulted;
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.refcount_faults;
#endif
        return;
    }
    ++domain->refcount;
}

inline void release_domain(void* opaque) noexcept {
    auto* domain = static_cast<pg_sequence_source_domain*>(opaque);
    if (domain == nullptr) {
        return;
    }
    if (domain->refcount_fault || domain->refcount == 0u) {
        domain->refcount_fault = true;
        domain->lifecycle = domain_lifecycle::faulted;
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.refcount_faults;
#endif
        return;
    }
    const std::uint32_t remaining = --domain->refcount;
    if (remaining == 1u && domain->registry_token_owned) {
        domain->reclaimable_hint = true;
        return;
    }
    if (remaining != 0u) {
        return;
    }
    if (domain->registry_token_owned
        || (domain->lifecycle != domain_lifecycle::invalid
            && domain->lifecycle != domain_lifecycle::released)) {
        domain->refcount_fault = true;
        domain->lifecycle = domain_lifecycle::faulted;
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.refcount_faults;
#endif
        return;
    }
    domain->lifecycle = domain_lifecycle::released;
#ifdef BIOSEQ_TEST_INDEXES
    ++g_pg_sequence_source_stats.domains_destroyed;
#endif
    domain->backend_thread.~pg_sequence_backend_thread_token();
    pfree(domain);
}

[[nodiscard]] inline pg_sequence_source_identity domain_identity(
        const void* opaque) noexcept {
    auto const* domain = static_cast<const pg_sequence_source_domain*>(opaque);
    return domain == nullptr ? pg_sequence_source_identity{}
        : pg_sequence_source_identity{
            .control_address = reinterpret_cast<std::uintptr_t>(domain),
            .generation = domain->generation,
        };
}

[[nodiscard]] inline bool domain_worker_shareable(const void*) noexcept {
    return false;
}

[[nodiscard]] inline bool domain_key_matches(
        const pg_sequence_source_domain& domain,
        AttrNumber column_attno,
        compact_sequence_stdc_type_params stored_type) noexcept {
    return domain_is_readable(domain)
        && domain.column_attno == column_attno
        && domain.stored_type == stored_type
        && domain.binding_generation != 0u
        && domain.registry != nullptr
        && domain.binding_generation
            == domain.registry->binding_generation;
}

[[nodiscard]] inline bool registry_lease_is_live(
        const pg_sequence_source_registry_lease& lease) noexcept {
    return lease.owned && lease.registry != nullptr
        && !lease.registry->lease_fault
        && lease.provider_generation == lease.registry->generation
        && lease.registry->accepting_lookups
        && lease.registry->backend_thread.is_current();
}

} // namespace sequence_source_detail

#ifdef BIOSEQ_TEST_INDEXES
[[nodiscard]] inline sequence_source_stats
pg_sequence_source_stats() noexcept {
    return sequence_source_detail::collect_pg_sequence_source_stats();
}
#endif

/* Output-lifetime diagnostics share the source-domain test counter surface.
 * Production callers retain an unconditional, inlinable no-op. */
inline void note_mmsearch_output_context_reset_for_test() noexcept {
#ifdef BIOSEQ_TEST_INDEXES
    ++g_pg_sequence_source_stats
        .mmsearch_output_context_resets;
#endif
}

inline void note_mmsearch_output_locator_promotion_for_test(
        const bool promoted_locator) noexcept {
#ifdef BIOSEQ_TEST_INDEXES
    if (promoted_locator) {
        ++g_pg_sequence_source_stats
            .mmsearch_output_locator_promotions;
    }
#else
    (void)promoted_locator;
#endif
}

inline void note_mmsearch_output_pg_error_restore_for_test(
        const bool state_restored) noexcept {
#ifdef BIOSEQ_TEST_INDEXES
    ++g_pg_sequence_source_stats
        .mmsearch_output_pg_error_restores;
    if (!state_restored) {
        ++g_pg_sequence_source_stats
            .mmsearch_output_restore_mismatches;
    }
#else
    (void)state_restored;
#endif
}

inline void inject_mmsearch_output_after_promotion_failure_for_test() {
#ifdef BIOSEQ_TEST_INDEXES
    sequence_source_detail::inject_pg_sequence_source_failure(
        sequence_source_failure::mmsearch_output_after_promotion);
#endif
}

namespace sequence_source_detail {

inline void abort_close_caches_noerror(seq_caches*& caches) noexcept {
    if (caches == nullptr) {
        return;
    }
    PG_TRY();
    {
        close_and_destroy_seq_caches(caches, false);
        caches = nullptr;
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.cache_sets_destroyed;
#endif
    }
    PG_CATCH();
    {
        FlushErrorState();
        if (caches != nullptr) {
            destroy_seq_caches(caches);
            caches = nullptr;
#ifdef BIOSEQ_TEST_INDEXES
            ++g_pg_sequence_source_stats.cache_sets_destroyed;
#endif
        }
    }
    PG_END_TRY();
}

inline void abort_unregister_snapshot_noerror(
        pg_sequence_source_domain& domain) noexcept {
    if (domain.registered_snapshot == nullptr
        || domain.resource_owner == nullptr) {
        domain.registered_snapshot = nullptr;
        return;
    }
    const ResourceOwner saved_owner = CurrentResourceOwner;
    PG_TRY();
    {
        CurrentResourceOwner = domain.resource_owner;
        UnregisterSnapshotFromOwner(
            domain.registered_snapshot, domain.resource_owner);
        domain.registered_snapshot = nullptr;
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.snapshots_unregistered;
#endif
        CurrentResourceOwner = saved_owner;
    }
    PG_CATCH();
    {
        CurrentResourceOwner = saved_owner;
        FlushErrorState();
        /* The child ResourceOwner releases any still-registered snapshot. */
        domain.registered_snapshot = nullptr;
    }
    PG_END_TRY();
}

inline void abort_release_owner_stage_noerror(
        pg_sequence_source_domain& domain,
        ResourceReleasePhase postgres_stage,
        owner_release_stage_bits stage_flag) noexcept {
    if (domain.resource_owner == nullptr
        || (domain.released_owner_stages & stage_bit(stage_flag)) != 0u) {
        return;
    }
#ifdef BIOSEQ_TEST_INDEXES
    switch (stage_flag) {
        case owner_release_stage_bits::before_locks:
            ++g_pg_sequence_source_stats.release_before_locks;
            break;
        case owner_release_stage_bits::locks:
            ++g_pg_sequence_source_stats.release_locks;
            break;
        case owner_release_stage_bits::after_locks:
            ++g_pg_sequence_source_stats.release_after_locks;
            break;
        case owner_release_stage_bits::none:
            Assert(false);
            break;
    }
#endif
    PG_TRY();
    {
        ResourceOwnerRelease(
            domain.resource_owner, postgres_stage, false, false);
        domain.released_owner_stages |= stage_bit(stage_flag);
    }
    PG_CATCH();
    {
        FlushErrorState();
        /* Mark the attempted cleanup stage. PostgreSQL's parent abort cleanup is
         * the final owner of a resource whose release callback itself failed. */
        domain.released_owner_stages |= stage_bit(stage_flag);
    }
    PG_END_TRY();
}

inline void abort_delete_owner_noerror(
        pg_sequence_source_domain& domain) noexcept {
    if (domain.resource_owner == nullptr) {
        return;
    }
    ResourceOwner owner = domain.resource_owner;
    domain.resource_owner = nullptr;
    PG_TRY();
    {
        ResourceOwnerDelete(owner);
    }
    PG_CATCH();
    {
        FlushErrorState();
    }
    PG_END_TRY();
}

inline void abort_delete_payload_noerror(
        pg_sequence_source_domain& domain) noexcept {
    if (domain.payload_context == nullptr) {
        return;
    }
    MemoryContext payload = domain.payload_context;
    domain.payload_context = nullptr;
    PG_TRY();
    {
        MemoryContextDelete(payload);
    }
    PG_CATCH();
    {
        FlushErrorState();
    }
    PG_END_TRY();
}

inline void abort_cleanup_domain_noerror(
        pg_sequence_source_domain& domain) noexcept {
    if (domain.lifecycle == domain_lifecycle::invalid
        || domain.lifecycle == domain_lifecycle::released) {
        return;
    }
    domain.lifecycle = domain_lifecycle::cleaning;
#ifdef BIOSEQ_TEST_INDEXES
    ++g_pg_sequence_source_stats.cleanup_abort;
#endif
    abort_close_caches_noerror(domain.read_caches);
    abort_unregister_snapshot_noerror(domain);
    abort_release_owner_stage_noerror(
        domain, RESOURCE_RELEASE_BEFORE_LOCKS,
        owner_release_stage_bits::before_locks);
    abort_release_owner_stage_noerror(
        domain, RESOURCE_RELEASE_LOCKS,
        owner_release_stage_bits::locks);
    abort_release_owner_stage_noerror(
        domain, RESOURCE_RELEASE_AFTER_LOCKS,
        owner_release_stage_bits::after_locks);
    abort_delete_owner_noerror(domain);
    abort_delete_payload_noerror(domain);
    domain.lifecycle = domain_lifecycle::invalid;
}

inline void cleanup_domain(
        pg_sequence_source_domain& domain, bool is_commit) {
    if (domain.lifecycle == domain_lifecycle::invalid
        || domain.lifecycle == domain_lifecycle::released) {
        return;
    }
    domain.lifecycle = domain_lifecycle::cleaning;
#ifdef BIOSEQ_TEST_INDEXES
    if (is_commit) {
        ++g_pg_sequence_source_stats.cleanup_commit;
    } else {
        ++g_pg_sequence_source_stats.cleanup_abort;
    }
#endif
    ResourceOwner owner = domain.resource_owner;
    const ResourceOwner saved_owner = CurrentResourceOwner;
    PG_TRY();
    {
        CurrentResourceOwner = owner;
        if (domain.read_caches != nullptr) {
#ifdef BIOSEQ_TEST_INDEXES
            inject_pg_sequence_source_failure(
                sequence_source_failure::close_before_caches);
#endif
            close_and_destroy_seq_caches(domain.read_caches, is_commit);
            domain.read_caches = nullptr;
#ifdef BIOSEQ_TEST_INDEXES
            ++g_pg_sequence_source_stats.cache_sets_destroyed;
#endif
        }
        if (domain.registered_snapshot != nullptr) {
#ifdef BIOSEQ_TEST_INDEXES
            inject_pg_sequence_source_failure(
                sequence_source_failure::close_before_snapshot);
#endif
            UnregisterSnapshotFromOwner(
                domain.registered_snapshot, owner);
            domain.registered_snapshot = nullptr;
#ifdef BIOSEQ_TEST_INDEXES
            ++g_pg_sequence_source_stats.snapshots_unregistered;
#endif
        }
        CurrentResourceOwner = saved_owner;
    }
    PG_CATCH();
    {
        CurrentResourceOwner = saved_owner;
        PG_RE_THROW();
    }
    PG_END_TRY();

    if ((domain.released_owner_stages
            & stage_bit(owner_release_stage_bits::before_locks)) == 0u) {
#ifdef BIOSEQ_TEST_INDEXES
        inject_pg_sequence_source_failure(
            sequence_source_failure::close_before_before_locks);
#endif
        ResourceOwnerRelease(
            owner, RESOURCE_RELEASE_BEFORE_LOCKS, is_commit, false);
        domain.released_owner_stages |=
            stage_bit(owner_release_stage_bits::before_locks);
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.release_before_locks;
#endif
    }
    if ((domain.released_owner_stages
            & stage_bit(owner_release_stage_bits::locks)) == 0u) {
#ifdef BIOSEQ_TEST_INDEXES
        inject_pg_sequence_source_failure(
            sequence_source_failure::close_before_locks);
#endif
        ResourceOwnerRelease(
            owner, RESOURCE_RELEASE_LOCKS, is_commit, false);
        domain.released_owner_stages |=
            stage_bit(owner_release_stage_bits::locks);
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.release_locks;
#endif
    }
    if ((domain.released_owner_stages
            & stage_bit(owner_release_stage_bits::after_locks)) == 0u) {
#ifdef BIOSEQ_TEST_INDEXES
        inject_pg_sequence_source_failure(
            sequence_source_failure::close_before_after_locks);
#endif
        ResourceOwnerRelease(
            owner, RESOURCE_RELEASE_AFTER_LOCKS, is_commit, false);
        domain.released_owner_stages |=
            stage_bit(owner_release_stage_bits::after_locks);
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.release_after_locks;
#endif
    }
    ResourceOwnerDelete(owner);
    domain.resource_owner = nullptr;
    MemoryContextDelete(domain.payload_context);
    domain.payload_context = nullptr;
    domain.lifecycle = domain_lifecycle::invalid;
}

inline void unlink_domain(
        pg_sequence_source_registry& registry,
        pg_sequence_source_domain& domain) noexcept {
    auto** link = &registry.domains;
    while (*link != nullptr) {
        if (*link == &domain) {
            *link = domain.next;
            domain.next = nullptr;
            return;
        }
        link = &(*link)->next;
    }
}

inline void drop_registry_token_noexcept(
        pg_sequence_source_domain& domain) noexcept {
    if (!domain.registry_token_owned) {
        domain.refcount_fault = true;
        domain.lifecycle = domain_lifecycle::faulted;
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.refcount_faults;
#endif
        return;
    }
    domain.registry_token_owned = false;
    domain.registry = nullptr;
    domain.reclaimable_hint = false;
    release_domain(&domain);
}

inline void sweep_registry(
        pg_sequence_source_registry& registry,
        pg_sequence_source_sweep_boundary boundary,
        bool is_commit) {
#ifdef BIOSEQ_TEST_INDEXES
    ++g_pg_sequence_source_stats.sweeps;
#endif
    auto* domain = registry.domains;
    while (domain != nullptr) {
        auto* next = domain->next;
        const bool reclaimable = domain->registry_token_owned
            && domain->refcount == 1u
            && !domain->refcount_fault
            && domain->lifecycle == domain_lifecycle::live;
        /* A parameterized rescan starts a fresh callback/read session but may
         * retain the registry's current verified binding.  Keeping that one
         * bounded domain preserves its snapshot/cache and lets the next row
         * hit the matching (attribute,type,binding-generation) key. A stale
         * generation and every terminal/provider boundary still reclaim in
         * the ordinary owner-first order below. */
        const bool reusable_after_rescan =
            boundary == pg_sequence_source_sweep_boundary::rescan
            && !registry.provider_closed && registry.accepting_lookups
            && domain->binding_generation != 0u
            && domain->binding_generation == registry.binding_generation;
        if (reclaimable && !reusable_after_rescan) {
            cleanup_domain(*domain, is_commit);
            unlink_domain(registry, *domain);
            drop_registry_token_noexcept(*domain);
#ifdef BIOSEQ_TEST_INDEXES
            ++g_pg_sequence_source_stats.swept_domains;
#endif
        }
        domain = next;
    }
}

inline void abort_sweep_registry_noerror(
        pg_sequence_source_registry& registry,
        bool all_domains) noexcept {
#ifdef BIOSEQ_TEST_INDEXES
    ++g_pg_sequence_source_stats.sweeps;
#endif
    auto* domain = registry.domains;
    while (domain != nullptr) {
        auto* next = domain->next;
        if (all_domains || domain->refcount == 1u
            || domain->refcount_fault) {
            abort_cleanup_domain_noerror(*domain);
            unlink_domain(registry, *domain);
            drop_registry_token_noexcept(*domain);
#ifdef BIOSEQ_TEST_INDEXES
            ++g_pg_sequence_source_stats.swept_domains;
#endif
        }
        domain = next;
    }
}

inline void remove_registry_from_global(
        pg_sequence_source_registry& registry) noexcept {
    auto** link = &registries;
    while (*link != nullptr) {
        if (*link == &registry) {
            *link = registry.next;
            registry.next = nullptr;
            return;
        }
        link = &(*link)->next;
    }
}

inline void destroy_registry_control(
        pg_sequence_source_registry* registry) noexcept {
    if (registry == nullptr || registry->domains != nullptr
        || registry->active_leases != 0u) {
        return;
    }
    remove_registry_from_global(*registry);
#ifdef BIOSEQ_TEST_INDEXES
    ++g_pg_sequence_source_stats.registries_destroyed;
#endif
    registry->backend_thread.~pg_sequence_backend_thread_token();
    pfree(registry);
}

inline void maybe_destroy_registry_control(
        pg_sequence_source_registry* registry) noexcept {
    if (registry != nullptr && registry->domains == nullptr
        && registry->active_leases == 0u
        && (registry->provider_closed || registry->xact_ended)) {
        destroy_registry_control(registry);
    }
}

inline void remove_provider_from_global(
        pg_sequence_source_provider& provider) noexcept {
    auto** link = &providers;
    while (*link != nullptr) {
        if (*link == &provider) {
            *link = provider.next;
            provider.next = nullptr;
            return;
        }
        link = &(*link)->next;
    }
}

inline void destroy_provider_control(
        pg_sequence_source_provider* provider) noexcept {
    if (provider == nullptr) {
        return;
    }
    remove_provider_from_global(*provider);
#ifdef BIOSEQ_TEST_INDEXES
    ++g_pg_sequence_source_stats.providers_destroyed;
#endif
    pfree(provider);
}

inline bool release_lease_token_noexcept(
        pg_sequence_source_registry_lease& lease) noexcept {
    if (!lease.owned || lease.registry == nullptr) {
        lease = {};
        return true;
    }
    auto* registry = lease.registry;
    if (lease.provider_generation != registry->generation
        || registry->active_leases == 0u
        || !registry->backend_thread.is_current()) {
        registry->lease_fault = true;
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.lease_faults;
#endif
        lease = {};
        return false;
    }
    --registry->active_leases;
#ifdef BIOSEQ_TEST_INDEXES
    ++g_pg_sequence_source_stats.leases_released;
#endif
    lease = {};
    return true;
}

inline void close_provider_abort_noerror(
        pg_sequence_source_provider& provider) noexcept {
    auto* registry = provider.lease.registry;
    provider.open = false;
    if (registry != nullptr) {
#ifdef BIOSEQ_TEST_INDEXES
        if (registry->active_leases > 1u) {
            ++g_pg_sequence_source_stats
                .provider_closed_with_upper_leases;
        }
#endif
        registry->provider_closed = true;
        registry->accepting_lookups = false;
    }
    (void)release_lease_token_noexcept(provider.lease);
    if (registry != nullptr) {
        abort_sweep_registry_noerror(*registry, false);
        maybe_destroy_registry_control(registry);
    }
    destroy_provider_control(&provider);
}

inline void close_provider_checked(
        pg_sequence_source_provider& provider, bool is_commit) {
    auto* registry = provider.lease.registry;
    if (registry == nullptr || !provider.open
        || !registry->backend_thread.is_current()) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq source provider is not live on this backend thread")));
    }
    provider.open = false;
#ifdef BIOSEQ_TEST_INDEXES
    if (registry->active_leases > 1u) {
        ++g_pg_sequence_source_stats
            .provider_closed_with_upper_leases;
    }
#endif
    registry->provider_closed = true;
    if (!release_lease_token_noexcept(provider.lease)) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq source provider lease accounting is corrupt")));
    }
    if (registry->active_leases == 0u) {
        registry->accepting_lookups = false;
    }
    sweep_registry(
        *registry, pg_sequence_source_sweep_boundary::provider_end,
        is_commit);
    maybe_destroy_registry_control(registry);
    destroy_provider_control(&provider);
}

inline void cleanup_registry_at_xact(
        pg_sequence_source_registry& registry, bool is_commit) {
    registry.accepting_lookups = false;
    registry.provider_closed = true;
    auto* domain = registry.domains;
    while (domain != nullptr) {
        auto* next = domain->next;
        cleanup_domain(*domain, is_commit);
        unlink_domain(registry, *domain);
        drop_registry_token_noexcept(*domain);
        domain = next;
    }
    registry.authoritative_snapshot = nullptr;
    registry.xact_ended = true;
    remove_registry_from_global(registry);
    maybe_destroy_registry_control(&registry);
}

inline void abort_registry_at_xact_noerror(
        pg_sequence_source_registry& registry) noexcept {
    registry.accepting_lookups = false;
    registry.provider_closed = true;
    abort_sweep_registry_noerror(registry, true);
    registry.authoritative_snapshot = nullptr;
    registry.xact_ended = true;
    remove_registry_from_global(registry);
    maybe_destroy_registry_control(&registry);
}

inline void xact_callback(XactEvent event, void*) {
    switch (event) {
        case XACT_EVENT_PRE_COMMIT:
        case XACT_EVENT_PARALLEL_PRE_COMMIT: {
            while (registries != nullptr) {
                cleanup_registry_at_xact(*registries, true);
            }
            break;
        }
        case XACT_EVENT_ABORT:
        case XACT_EVENT_PARALLEL_ABORT: {
            while (registries != nullptr) {
                abort_registry_at_xact_noerror(*registries);
            }
            break;
        }
        case XACT_EVENT_COMMIT:
        case XACT_EVENT_PARALLEL_COMMIT:
            Assert(registries == nullptr);
            break;
        case XACT_EVENT_PRE_PREPARE:
            for (auto* registry = registries; registry != nullptr;
                registry = registry->next) {
                if (registry->domains != nullptr) {
#ifdef BIOSEQ_TEST_INDEXES
                    ++g_pg_sequence_source_stats.pre_prepare_rejections;
#endif
                    ereport(ERROR,
                        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                         errmsg("cannot PREPARE TRANSACTION with live bioseq source domains"),
                         errhint("materialize locator-backed bioseq values "
                                 "before PREPARE TRANSACTION")));
                }
            }
            while (registries != nullptr) {
                cleanup_registry_at_xact(*registries, true);
            }
            break;
        case XACT_EVENT_PREPARE:
            Assert(registries == nullptr);
            break;
        default:
            Assert(false);
            break;
    }
}

inline void subxact_callback(
        SubXactEvent event,
        SubTransactionId my_subid,
        SubTransactionId parent_subid,
        void*) {
    switch (event) {
        case SUBXACT_EVENT_START_SUB:
        case SUBXACT_EVENT_PRE_COMMIT_SUB:
            break;
        case SUBXACT_EVENT_COMMIT_SUB:
            for (auto* registry = registries; registry != nullptr;
                 registry = registry->next) {
                if (registry->creating_subid == my_subid) {
                    registry->creating_subid = parent_subid;
#ifdef BIOSEQ_TEST_INDEXES
                    ++g_pg_sequence_source_stats.subxact_promotions;
#endif
                }
                for (auto* domain = registry->domains; domain != nullptr;
                     domain = domain->next) {
                    if (domain->creating_subid == my_subid) {
                        domain->creating_subid = parent_subid;
#ifdef BIOSEQ_TEST_INDEXES
                        ++g_pg_sequence_source_stats.subxact_promotions;
#endif
                    }
                }
            }
            break;
        case SUBXACT_EVENT_ABORT_SUB: {
#ifdef BIOSEQ_TEST_INDEXES
            ++g_pg_sequence_source_stats.subxact_aborts;
#endif
            auto* registry = registries;
            while (registry != nullptr) {
                auto* next_registry = registry->next;
                if (registry->creating_subid == my_subid) {
                    abort_registry_at_xact_noerror(*registry);
                } else {
                    auto* domain = registry->domains;
                    while (domain != nullptr) {
                        auto* next_domain = domain->next;
                        if (domain->creating_subid == my_subid) {
                            abort_cleanup_domain_noerror(*domain);
                            unlink_domain(*registry, *domain);
                            drop_registry_token_noexcept(*domain);
                        }
                        domain = next_domain;
                    }
                }
                registry = next_registry;
            }
            break;
        }
        default:
            Assert(false);
            break;
    }
}

inline void ensure_callbacks_registered() {
    if (callbacks_registered) {
        return;
    }
    RegisterXactCallback(xact_callback, nullptr);
    RegisterSubXactCallback(subxact_callback, nullptr);
    callbacks_registered = true;
}

[[nodiscard]] inline bool source_row_matches_registry(
        const pg_sequence_source_registry& registry,
        AttrNumber column_attno,
        const sequence_locator& locator,
        compact_sequence_stdc_type_params stored_type,
        std::uint16_t last_padding_letters,
        const sequence_source_version_row& row) noexcept {
    return row.state == k_sequence_source_state_ready
        && row.source_epoch == locator.source_epoch
        && row.user_relid == registry.user_relid
        && row.database_oid == registry.database_oid
        && row.tablespace_oid == registry.tablespace_oid
        && row.main_relfilenode == registry.main_relfilenode
        && row.main_storage_id == registry.main_storage_id
        && row.main_storage_id == registry.binding_generation
        && row.column_attno == column_attno
        && same_locator(row.locator, locator)
        && row.alphabet_code == stored_type.alphabet_code
        && row.storage_unit_byte_width
            == stored_type.storage_unit_byte_width
        && row.padding_letters == last_padding_letters;
}

[[nodiscard]] inline pg_sequence_source_domain* create_domain(
        pg_sequence_source_registry& registry,
        AttrNumber column_attno,
        compact_sequence_stdc_type_params stored_type) {
    pg_sequence_source_domain* volatile pending = nullptr;
    pg_sequence_source_domain* volatile result = nullptr;
    const MemoryContext saved_context = CurrentMemoryContext;
    const ResourceOwner saved_owner = CurrentResourceOwner;
    PG_TRY();
    {
        auto* domain = static_cast<pg_sequence_source_domain*>(
            MemoryContextAllocZero(
                TopMemoryContext, sizeof(pg_sequence_source_domain)));
        pending = domain;
        ::new (static_cast<void*>(domain)) pg_sequence_source_domain{};
        domain->backend_thread = pg_sequence_backend_thread_token::capture();
        domain->registry = &registry;
        domain->column_attno = column_attno;
        domain->stored_type = stored_type;
        domain->binding_generation = registry.binding_generation;
        domain->creating_subid = GetCurrentSubTransactionId();
        domain->generation = allocate_generation();
        domain->refcount = 1u;
        domain->registry_token_owned = true;
#ifdef BIOSEQ_TEST_INDEXES
        inject_pg_sequence_source_failure(
            sequence_source_failure::domain_after_control);
#endif
        domain->payload_context = AllocSetContextCreate(
            TopTransactionContext, "bioseq shared sequence source domain",
            ALLOCSET_DEFAULT_SIZES);
#ifdef BIOSEQ_TEST_INDEXES
        inject_pg_sequence_source_failure(
            sequence_source_failure::domain_after_context);
#endif
        domain->resource_owner = ResourceOwnerCreate(
            TopTransactionResourceOwner,
            "bioseq shared sequence source domain");
#ifdef BIOSEQ_TEST_INDEXES
        inject_pg_sequence_source_failure(
            sequence_source_failure::domain_after_owner);
#endif
        CurrentResourceOwner = domain->resource_owner;
        domain->registered_snapshot = RegisterSnapshotOnOwner(
            registry.authoritative_snapshot, domain->resource_owner);
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.snapshots_registered;
        inject_pg_sequence_source_failure(
            sequence_source_failure::domain_after_snapshot);
#endif
        domain->read_caches = alloc_seq_caches(domain->payload_context);
        domain->read_caches->side = seq_caches::access_side::read;
        domain->read_caches->lock_mode = AccessShareLock;
        domain->read_caches->read_state_mctx = domain->payload_context;
        domain->read_caches->read_resource_owner = domain->resource_owner;
        domain->read_caches->main_database_oid = registry.database_oid;
        domain->read_caches->main_tablespace_oid = registry.tablespace_oid;
        domain->read_caches->main_relfilenode = registry.main_relfilenode;
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.cache_sets_created;
        inject_pg_sequence_source_failure(
            sequence_source_failure::domain_after_caches);
#endif
        CurrentResourceOwner = saved_owner;
        MemoryContextSwitchTo(saved_context);
        domain->lifecycle = domain_lifecycle::live;
        domain->next = registry.domains;
        registry.domains = domain;
        result = domain;
        pending = nullptr;
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.domains_created;
#endif
    }
    PG_CATCH();
    {
        CurrentResourceOwner = saved_owner;
        MemoryContextSwitchTo(saved_context);
        auto* domain = const_cast<pg_sequence_source_domain*>(pending);
        if (domain != nullptr) {
            abort_cleanup_domain_noerror(*domain);
            domain->registry_token_owned = false;
            domain->registry = nullptr;
            domain->refcount = 0u;
            domain->lifecycle = domain_lifecycle::released;
            domain->backend_thread.~pg_sequence_backend_thread_token();
            pfree(domain);
        }
        PG_RE_THROW();
    }
    PG_END_TRY();
    return const_cast<pg_sequence_source_domain*>(result);
}

[[nodiscard]] inline pg_sequence_source_domain* find_or_create_domain(
        pg_sequence_source_registry& registry,
        AttrNumber column_attno,
        compact_sequence_stdc_type_params stored_type) {
    for (auto* domain = registry.domains; domain != nullptr;
         domain = domain->next) {
        if (domain_key_matches(*domain, column_attno, stored_type)) {
#ifdef BIOSEQ_TEST_INDEXES
            ++g_pg_sequence_source_stats.domains_reused;
#endif
            return domain;
        }
    }
    return create_domain(registry, column_attno, stored_type);
}

[[nodiscard]] inline pg_sequence_source_provider* create_provider(
        Relation owning_relation,
        Snapshot authoritative_registered_snapshot,
        pg_sequence_source_provider_kind kind) {
    if (owning_relation == nullptr
        || authoritative_registered_snapshot == nullptr
        || !IsMVCCSnapshot(authoritative_registered_snapshot)
        || authoritative_registered_snapshot->regd_count == 0) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq source registry requires an owning relation and "
                    "registered MVCC snapshot")));
    }
    ensure_callbacks_registered();
    const std::uint64_t storage_id =
        bioseq::postgres::support::invoke_pg_as_cpp([owning_relation] {
            return column_storage_id(
                owning_relation, false);
        });
    if (storage_id == 0u) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq source registry relation has no physical storage identity")));
    }

    pg_sequence_source_registry* volatile pending_registry = nullptr;
    pg_sequence_source_provider* volatile pending_provider = nullptr;
    pg_sequence_source_provider* volatile result = nullptr;
    PG_TRY();
    {
        auto* registry = static_cast<pg_sequence_source_registry*>(
            MemoryContextAllocZero(
                TopMemoryContext, sizeof(pg_sequence_source_registry)));
        pending_registry = registry;
        ::new (static_cast<void*>(registry)) pg_sequence_source_registry{};
        registry->backend_thread =
            pg_sequence_backend_thread_token::capture();
        registry->user_relid = RelationGetRelid(owning_relation);
        registry->database_oid = owning_relation->rd_locator.dbOid;
        registry->tablespace_oid = owning_relation->rd_locator.spcOid;
        registry->main_relfilenode = owning_relation->rd_locator.relNumber;
        registry->main_storage_id = storage_id;
        registry->binding_generation = storage_id;
        registry->authoritative_snapshot =
            authoritative_registered_snapshot;
        registry->creating_subid = GetCurrentSubTransactionId();
        registry->generation = allocate_generation();
        registry->active_leases = 1u;
        registry->accepting_lookups = true;
#ifdef BIOSEQ_TEST_INDEXES
        inject_pg_sequence_source_failure(
            sequence_source_failure::provider_after_registry_control);
#endif

        auto* provider = static_cast<pg_sequence_source_provider*>(
            MemoryContextAllocZero(
                TopMemoryContext, sizeof(pg_sequence_source_provider)));
        pending_provider = provider;
        ::new (static_cast<void*>(provider)) pg_sequence_source_provider{};
        provider->lease = {
            .registry = registry,
            .provider_generation = registry->generation,
            .owned = true,
        };
        provider->kind = kind;
        provider->open = true;
#ifdef BIOSEQ_TEST_INDEXES
        inject_pg_sequence_source_failure(
            sequence_source_failure::provider_after_provider_control);
#endif

        registry->next = registries;
        registries = registry;
        provider->next = providers;
        providers = provider;
        result = provider;
        pending_registry = nullptr;
        pending_provider = nullptr;
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.registries_created;
        ++g_pg_sequence_source_stats.providers_created;
        if (kind == pg_sequence_source_provider_kind::scan) {
            ++g_pg_sequence_source_stats.scan_providers_created;
        } else {
            ++g_pg_sequence_source_stats
                .standalone_providers_created;
        }
        ++g_pg_sequence_source_stats.leases_acquired;
#endif
    }
    PG_CATCH();
    {
        auto* provider =
            const_cast<pg_sequence_source_provider*>(pending_provider);
        auto* registry =
            const_cast<pg_sequence_source_registry*>(pending_registry);
        if (provider != nullptr) {
            pfree(provider);
        }
        if (registry != nullptr) {
            registry->backend_thread.~pg_sequence_backend_thread_token();
            pfree(registry);
        }
        PG_RE_THROW();
    }
    PG_END_TRY();
    return const_cast<pg_sequence_source_provider*>(result);
}

template <class p_sequence_instance>
[[nodiscard]] constexpr std::uint16_t locator_padding_letters(
        const sequence_locator& locator) noexcept {
    using range = compact_sequence_range<p_sequence_instance>;
    constexpr std::uint64_t letters_per_unit =
        range::traits::letters_per_unit;
    const std::uint64_t remainder =
        locator.logical_length % letters_per_unit;
    return static_cast<std::uint16_t>(
        remainder == 0u ? 0u : letters_per_unit - remainder);
}

[[nodiscard]] inline bool locator_geometry_is_valid(
        const sequence_locator& locator,
        compact_sequence_stdc_type_params stored_type,
        std::uint16_t last_padding_letters,
        bool validate_padding = true) {
    bool dispatched = false;
    bool valid = false;
    bioseq::pg_types::seq::dispatch_instance(
        stored_type.alphabet_code,
        stored_type.storage_unit_byte_width,
        [&dispatched, &valid, locator,
         last_padding_letters, validate_padding]<class p_sequence_instance>() {
            using range = compact_sequence_range<p_sequence_instance>;
            constexpr std::uint64_t letters_per_fragment =
                static_cast<std::uint64_t>(
                    postgres_sequence_scanner<p_sequence_instance>::fragment_storage_units)
                * range::traits::letters_per_unit;
            const std::uint64_t complete =
                locator.logical_length / letters_per_fragment;
            const std::uint64_t remainder =
                locator.logical_length % letters_per_fragment;
            const std::uint64_t expected_fragments =
                complete + (remainder == 0u ? 0u : 1u);
            dispatched = true;
            valid = locator.payload_version == sequence_locator_payload_version
                && locator.reserved_16 == 0u && locator.reserved_32 == 0u
                && locator.source_epoch != 0u
                && locator.fragment_count == expected_fragments
                && (!validate_padding
                    || last_padding_letters
                        == locator_padding_letters<p_sequence_instance>(locator));
        });
    return dispatched && valid;
}

template <class p_sequence_instance>
inline void typed_scanner_read_ranks(
        source_scanner_entry& entry,
        sequence_transform transform,
        std::uint64_t source_from,
        std::span<std::uint8_t> output) {
    using scanner_type = postgres_sequence_scanner<p_sequence_instance>;
    using range = compact_sequence_range<p_sequence_instance>;
    using traits = typename range::traits;
    using unit = typename traits::storage_unit;
    auto& scanner = *static_cast<scanner_type*>(entry.concrete_scanner);
    constexpr std::uint64_t letters_per_fragment =
        static_cast<std::uint64_t>(scanner_type::fragment_storage_units)
        * traits::letters_per_unit;
    const bool reverse = bioseq::storage::is_reversed(transform);
    const bool complement = bioseq::storage::is_complemented(transform);

    for (std::size_t output_index = 0u;
         output_index < output.size(); ++output_index) {
        const std::uint64_t physical = source_from
            + (reverse ? output.size() - 1u - output_index
                       : output_index);
        const std::size_t fragment_no = static_cast<std::size_t>(
            physical / letters_per_fragment);
        const auto packed = scanner.load_fragment(fragment_no);
        const std::uint64_t local_symbol =
            physical % letters_per_fragment;
        const std::size_t unit_index = static_cast<std::size_t>(
            local_symbol / traits::letters_per_unit);
        if (unit_index >= packed.size()) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [fragment_no, unit_index, packed_size = packed.size()] {
                    ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("bioseq source scanner returned a truncated fragment"),
                         errdetail(
                            "fragment=%zu unit=%zu available_units=%zu",
                            fragment_no, unit_index, packed_size)));
                });
        }
        const unit word = packed[unit_index];
        const std::size_t shift = static_cast<std::size_t>(
            local_symbol % traits::letters_per_unit)
            * traits::bits_per_char;
        std::uint8_t rank = static_cast<std::uint8_t>(
            (word >> shift) & traits::char_mask);
        if (rank >= seqan3::alphabet_size<
                        typename p_sequence_instance::alphabet_type>) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [fragment_no, local_symbol, rank] {
                    ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("bioseq source fragment contains an invalid rank"),
                         errdetail(
                            "fragment=%zu local_symbol=%llu rank=%u",
                            fragment_no,
                            static_cast<unsigned long long>(local_symbol),
                            static_cast<unsigned>(rank))));
                });
        }
        if (complement) {
            if constexpr (requires(unit value) {
                    traits::letter_complement(value);
                }) {
                rank = static_cast<std::uint8_t>(
                    traits::letter_complement(static_cast<unit>(rank)));
            } else {
                bioseq::postgres::support::invoke_pg_as_cpp([] {
                    ereport(ERROR,
                        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                         errmsg("bioseq source alphabet has no complement operation")));
                });
            }
        }
        output[output_index] = rank;
    }
}

template <class p_sequence_instance>
inline void finish_typed_scanner_entry(source_scanner_entry& entry) {
    using scanner_type = postgres_sequence_scanner<p_sequence_instance>;
    auto* scanner = static_cast<scanner_type*>(entry.concrete_scanner);
    if (scanner == nullptr || !entry.constructed) {
        return;
    }
    scanner->finish_pg_resources_as_cpp();
    scanner->~scanner_type();
    entry.concrete_scanner = nullptr;
    entry.constructed = false;
}

template <class p_sequence_instance>
inline void abort_typed_scanner_entry_noexcept(
        source_scanner_entry& entry) noexcept {
    using scanner_type = postgres_sequence_scanner<p_sequence_instance>;
    auto* scanner = static_cast<scanner_type*>(entry.concrete_scanner);
    if (scanner == nullptr || !entry.constructed) {
        return;
    }
    scanner->abandon_pg_resources_noexcept();
    scanner->~scanner_type();
    entry.concrete_scanner = nullptr;
    entry.constructed = false;
}

inline void abort_source_scanner_entry_noexcept(void* opaque) noexcept {
    auto* entry = static_cast<source_scanner_entry*>(opaque);
    if (entry == nullptr) {
        return;
    }
    if (entry->abort_noexcept != nullptr) {
        entry->abort_noexcept(*entry);
    }
    if (entry->domain_retained) {
        entry->domain_retained = false;
        release_domain(entry->domain);
    }
    entry->domain = nullptr;
}

inline void finish_source_scanner_entry(void* opaque, bool) {
    auto* entry = static_cast<source_scanner_entry*>(opaque);
    if (entry == nullptr) {
        return;
    }
    ErrorData* pg_error_data = nullptr;
    bool cpp_error = false;
    bool unknown_cpp_error = false;
    char detail[512] = "std::exception without detail";
    if (entry->finish != nullptr) {
        try {
            entry->finish(*entry);
        } catch (bioseq::postgres::support::postgres_error& error) {
            pg_error_data = error.release_error_data();
        } catch (const std::exception& error) {
            strlcpy(
                detail,
                error.what() == nullptr
                    ? "std::exception without detail" : error.what(),
                sizeof(detail));
            cpp_error = true;
        } catch (...) {
            unknown_cpp_error = true;
        }
    }
    if (pg_error_data != nullptr) {
        ReThrowError(pg_error_data);
    }
    if (cpp_error) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("bioseq source scanner finalization failed"),
             errdetail_internal("%s", detail)));
    }
    if (unknown_cpp_error) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("bioseq source scanner finalization failed"),
             errdetail_internal("unknown C++ exception")));
    }
    if (entry->domain_retained) {
        entry->domain_retained = false;
        release_domain(entry->domain);
    }
    entry->domain = nullptr;
}

[[nodiscard]] inline void* create_source_scanner_entry(
        void* opaque,
        MemoryContext payload_context) {
    auto const& factory =
        *static_cast<const source_scanner_factory_argument*>(opaque);
    auto* entry = static_cast<source_scanner_entry*>(
        MemoryContextAllocZero(
            payload_context, sizeof(source_scanner_entry)));
    entry->domain = factory.domain;
    bool dispatched = false;
    ErrorData* pg_error_data = nullptr;
    bool cpp_error = false;
    bool unknown_cpp_error = false;
    char detail[512] = "std::exception without detail";
    try {
        bioseq::pg_types::seq::dispatch_instance(
            factory.stored_type.alphabet_code,
            factory.stored_type.storage_unit_byte_width,
            [entry, payload_context, locator = factory.locator,
             &dispatched]<class p_sequence_instance>() {
                using scanner_type = postgres_sequence_scanner<p_sequence_instance>;
                void* storage = bioseq::postgres::support::invoke_pg_as_cpp(
                    [payload_context] {
                        return MemoryContextAllocZero(
                            payload_context, sizeof(scanner_type));
                    });
                entry->concrete_scanner = storage;
                entry->read_ranks = &typed_scanner_read_ranks<p_sequence_instance>;
                entry->finish = &finish_typed_scanner_entry<p_sequence_instance>;
                entry->abort_noexcept =
                    &abort_typed_scanner_entry_noexcept<p_sequence_instance>;
                retain_domain(entry->domain);
                entry->domain_retained = true;
                if (!domain_is_readable(*entry->domain)) {
                    bioseq::postgres::support::invoke_pg_as_cpp([] {
                        ereport(ERROR,
                            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                             errmsg("bioseq source domain became invalid while "
                                    "creating a scanner")));
                    });
                }
                auto* registry = entry->domain->registry;
                ::new (storage) scanner_type(
                    registry->database_oid,
                    registry->tablespace_oid,
                    registry->main_relfilenode,
                    entry->domain->column_attno,
                    locator,
                    locator_padding_letters<p_sequence_instance>(locator),
                    entry->domain->registered_snapshot,
                    entry->domain->read_caches,
                    payload_context);
                entry->constructed = true;
                dispatched = true;
            });
        if (!dispatched) {
            bioseq::postgres::support::invoke_pg_as_cpp([] {
                ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("bioseq source binding uses an unsupported sequence representation")));
            });
        }
    } catch (bioseq::postgres::support::postgres_error& error) {
        pg_error_data = error.release_error_data();
    } catch (const std::exception& error) {
        strlcpy(
            detail,
            error.what() == nullptr
                ? "std::exception without detail" : error.what(),
            sizeof(detail));
        cpp_error = true;
    } catch (...) {
        unknown_cpp_error = true;
    }
    if (pg_error_data != nullptr || cpp_error || unknown_cpp_error) {
        abort_source_scanner_entry_noexcept(entry);
    }
    if (pg_error_data != nullptr) {
        ReThrowError(pg_error_data);
    }
    if (cpp_error) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("bioseq source scanner construction failed"),
             errdetail_internal("%s", detail)));
    }
    if (unknown_cpp_error) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("bioseq source scanner construction failed"),
             errdetail_internal("unknown C++ exception")));
    }
#ifdef BIOSEQ_TEST_INDEXES
    ++g_pg_sequence_source_stats.scanner_entries_created;
#endif
    return entry;
}

[[nodiscard]] inline pg_sequence_source_read_status
read_domain_ranks_on_backend(
        pg_sequence_source_domain* domain,
        pg_sequence_read_session& session,
        const sequence_locator& locator,
        compact_sequence_stdc_type_params stored_type,
        sequence_transform transform,
        std::uint64_t source_from,
        std::span<std::uint8_t> output) {
    if (!domain_is_readable(*domain)) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq source transaction has ended"),
             errhint("materialize locator-backed values while their owning transaction is live")));
    }
    if (domain->stored_type != stored_type
        || !locator_geometry_is_valid(
            locator, stored_type, 0u, /*validate_padding=*/false)
        || locator.logical_length
            > static_cast<std::uint64_t>(INT64_MAX)
        || source_from > locator.logical_length
        || output.size() > locator.logical_length - source_from) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq source read request does not match its verified binding")));
    }
    if (output.empty()) {
        return pg_sequence_source_read_status::ok;
    }

    const pg_sequence_read_entry_key key{
        .source = domain_identity(domain),
        .locator = locator,
        .stored_type_params = stored_type,
    };
#ifdef BIOSEQ_TEST_INDEXES
    for (auto* candidate = session.entries; candidate != nullptr;
         candidate = candidate->next) {
        if (candidate->key.source == key.source
            && same_locator(candidate->key.locator, key.locator)
            && candidate->key.stored_type_params
                == key.stored_type_params) {
            ++g_pg_sequence_source_stats.scanner_entry_hits;
            break;
        }
    }
#endif
    source_scanner_factory_argument factory{
        .domain = domain,
        .locator = locator,
        .stored_type = stored_type,
    };
    auto* reader = static_cast<source_scanner_entry*>(
        find_or_create_pg_sequence_read_entry(
            session, key, &factory, create_source_scanner_entry,
            finish_source_scanner_entry,
            abort_source_scanner_entry_noexcept));
    if (reader == nullptr || reader->read_ranks == nullptr) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq source read session is not accepting scanner entries")));
    }

#ifdef BIOSEQ_TEST_INDEXES
    /* The session entry and its abort callback are installed before this raw
     * PostgreSQL ERROR.  The outer source-ABI boundary must convert it to C++ so
     * every view shell can abort the session before rethrowing the PostgreSQL error. */
    inject_pg_sequence_source_failure(
        sequence_source_failure::read_before_scanner);
#endif

    const ResourceOwner saved_owner = CurrentResourceOwner;
    ErrorData* pg_error_data = nullptr;
    bool cpp_error = false;
    bool unknown_cpp_error = false;
    char detail[512] = "std::exception without detail";
    try {
        CurrentResourceOwner = domain->resource_owner;
        reader->read_ranks(*reader, transform, source_from, output);
        CurrentResourceOwner = saved_owner;
    } catch (bioseq::postgres::support::postgres_error& error) {
        CurrentResourceOwner = saved_owner;
        pg_error_data = error.release_error_data();
    } catch (const std::exception& error) {
        CurrentResourceOwner = saved_owner;
        strlcpy(
            detail,
            error.what() == nullptr
                ? "std::exception without detail" : error.what(),
            sizeof(detail));
        cpp_error = true;
    } catch (...) {
        CurrentResourceOwner = saved_owner;
        unknown_cpp_error = true;
    }
    if (pg_error_data != nullptr) {
        ReThrowError(pg_error_data);
    }
    if (cpp_error) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("bioseq source scanner read failed"),
             errdetail_internal("%s", detail)));
    }
    if (unknown_cpp_error) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("bioseq source scanner read failed"),
             errdetail_internal("unknown C++ exception")));
    }
    return pg_sequence_source_read_status::ok;
}

[[nodiscard]] inline pg_sequence_source_read_status read_domain_ranks(
        const void* opaque,
        pg_sequence_read_session& session,
        const sequence_locator& locator,
        compact_sequence_stdc_type_params stored_type,
        sequence_transform transform,
        std::uint64_t source_from,
        std::span<std::uint8_t> output) {
    auto* domain = const_cast<pg_sequence_source_domain*>(
        static_cast<const pg_sequence_source_domain*>(opaque));
    /* This check must remain before invoke_pg_as_cpp: its PG_TRY boundary and
     * ErrorData machinery are backend-thread-only. */
    if (domain == nullptr || !domain->backend_thread.is_current()
        || !session.backend_thread.is_current()
        || domain->backend_thread.owner != session.backend_thread.owner) {
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.wrong_thread_rejections;
#endif
        return pg_sequence_source_read_status::wrong_thread;
    }
    return bioseq::postgres::support::invoke_pg_as_cpp(
        [domain, &session, locator, stored_type, transform, source_from,
         output] {
            return read_domain_ranks_on_backend(
                domain, session, locator, stored_type, transform,
                source_from, output);
        });
}

[[nodiscard]] inline const pg_sequence_source_backend_ops&
source_backend_ops() noexcept {
    static const pg_sequence_source_backend_ops ops{
        .retain = retain_domain,
        .release = release_domain,
        .identity = domain_identity,
        .worker_shareable = domain_worker_shareable,
        .read_ranks = read_domain_ranks,
    };
    return ops;
}

} // namespace sequence_source_detail

[[nodiscard]] inline pg_sequence_source_provider*
create_pg_sequence_source_provider(
        Relation owning_relation,
        Snapshot authoritative_registered_snapshot) {
    return bioseq::postgres::support::invoke_pg_as_cpp(
        [owning_relation, authoritative_registered_snapshot] {
            return sequence_source_detail::create_provider(
                owning_relation, authoritative_registered_snapshot,
                pg_sequence_source_provider_kind::scan);
        });
}

[[nodiscard]] inline pg_sequence_source_provider*
create_standalone_pg_sequence_source_provider(
        Relation owning_relation,
        Snapshot authoritative_registered_snapshot) {
    return bioseq::postgres::support::invoke_pg_as_cpp(
        [owning_relation, authoritative_registered_snapshot] {
            return sequence_source_detail::create_provider(
                owning_relation, authoritative_registered_snapshot,
                pg_sequence_source_provider_kind::standalone_index);
        });
}

/** Rebind a scan provider to the registered snapshot restored from its
 * parallel DSM coordinate.  Upper owners may already retain leases on this
 * registry, so the registry is updated in place rather than replaced.  No
 * source domain may have observed the preceding snapshot. */
inline void rebind_pg_sequence_source_provider_snapshot(
        pg_sequence_source_provider* provider,
        Snapshot authoritative_registered_snapshot) {
    using namespace sequence_source_detail;
    if (provider == nullptr || !provider->open
        || provider->kind != pg_sequence_source_provider_kind::scan
        || !registry_lease_is_live(provider->lease)) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq scan source provider cannot rebind its snapshot")));
    }
    auto* registry = provider->lease.registry;
    if (authoritative_registered_snapshot == nullptr
        || !IsMVCCSnapshot(authoritative_registered_snapshot)
        || authoritative_registered_snapshot->regd_count == 0
        || registry->authoritative_snapshot == nullptr) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq scan source provider requires a registered MVCC snapshot")));
    }
    if (registry->domains != nullptr) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq scan source provider cannot rebind after source activation")));
    }
    registry->authoritative_snapshot = authoritative_registered_snapshot;
}

[[nodiscard]] inline pg_sequence_source_registry_lease
acquire_pg_sequence_source_registry_lease(
        const pg_sequence_source_registry_lease& parent) {
    using namespace sequence_source_detail;
    if (!registry_lease_is_live(parent)) {
        if (parent.registry != nullptr
            && !parent.registry->backend_thread.is_current()) {
#ifdef BIOSEQ_TEST_INDEXES
            ++g_pg_sequence_source_stats.wrong_thread_rejections;
#endif
            return {};
        }
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq source registry lease is not live")));
    }
    auto& registry = *parent.registry;
    if (registry.active_leases
        == std::numeric_limits<std::uint32_t>::max()) {
        registry.lease_fault = true;
        registry.accepting_lookups = false;
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.lease_faults;
#endif
        ereport(ERROR,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
             errmsg("bioseq source registry lease count overflow")));
    }
    ++registry.active_leases;
#ifdef BIOSEQ_TEST_INDEXES
    ++g_pg_sequence_source_stats.leases_acquired;
#endif
    return {
        .registry = &registry,
        .provider_generation = registry.generation,
        .owned = true,
    };
}

[[nodiscard]] inline pg_sequence_source_registry_lease
acquire_pg_sequence_source_registry_lease(
        const pg_sequence_source_provider* provider) {
    if (provider == nullptr || !provider->open) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq source provider is not open")));
    }
    return acquire_pg_sequence_source_registry_lease(provider->lease);
}

[[nodiscard]] inline bool release_pg_sequence_source_registry_lease(
        pg_sequence_source_registry_lease& lease,
        pg_sequence_source_sweep_boundary boundary,
        bool is_commit) {
    using namespace sequence_source_detail;
    auto* registry = lease.registry;
    if (registry == nullptr || !lease.owned) {
        lease = {};
        return true;
    }
    if (!registry->backend_thread.is_current()) {
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.wrong_thread_rejections;
#endif
        return false;
    }
    const bool released = release_lease_token_noexcept(lease);
    if (!released) {
        return false;
    }
    if (registry->provider_closed && registry->active_leases == 0u) {
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats
            .last_upper_lease_after_provider_end;
#endif
        registry->accepting_lookups = false;
    }
    if (is_commit) {
        sweep_registry(*registry, boundary, true);
    } else {
        abort_sweep_registry_noerror(*registry, false);
    }
    maybe_destroy_registry_control(registry);
    return true;
}

inline void release_pg_sequence_source_registry_lease_noexcept(
        pg_sequence_source_registry_lease& lease,
        pg_sequence_source_sweep_boundary boundary) noexcept {
    using namespace sequence_source_detail;
    auto* registry = lease.registry;
    if (registry == nullptr || !lease.owned) {
        lease = {};
        return;
    }
    if (!registry->backend_thread.is_current()) {
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.wrong_thread_rejections;
#endif
        lease = {};
        return;
    }
    (void)release_lease_token_noexcept(lease);
    if (registry->provider_closed && registry->active_leases == 0u) {
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats
            .last_upper_lease_after_provider_end;
#endif
        registry->accepting_lookups = false;
    }
    (void)boundary;
    abort_sweep_registry_noerror(*registry, false);
    maybe_destroy_registry_control(registry);
}

inline void close_pg_sequence_source_provider(
        pg_sequence_source_provider* provider,
        bool is_commit) {
    if (provider == nullptr) {
        return;
    }
    if (is_commit) {
        sequence_source_detail::close_provider_checked(
            *provider, true);
    } else {
        sequence_source_detail::close_provider_abort_noerror(*provider);
    }
}

inline void sweep_reclaimable_pg_sequence_source_domains(
        const pg_sequence_source_registry_lease& lease,
        pg_sequence_source_sweep_boundary boundary,
        bool is_commit = true) {
    using namespace sequence_source_detail;
    if (!registry_lease_is_live(lease)) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("cannot sweep a closed bioseq source registry")));
    }
    if (is_commit) {
        sweep_registry(*lease.registry, boundary, true);
    } else {
        abort_sweep_registry_noerror(*lease.registry, false);
    }
}

inline void initialize_pg_sequence_source_routes(
        pg_sequence_source_route_set& routes,
        MemoryContext allocation_context) {
    if (routes.count != 0u || routes.dynamic_routes != nullptr
        || allocation_context == nullptr) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq source route storage cannot be initialized")));
    }
    routes.allocation_context = allocation_context;
}

[[nodiscard]] inline std::size_t pg_sequence_source_route_capacity(
        const pg_sequence_source_route_set& routes) noexcept {
    return routes.dynamic_routes == nullptr
        ? routes.inline_routes.size() : routes.dynamic_capacity;
}

[[nodiscard]] inline pg_sequence_source_route* pg_sequence_source_routes_data(
        pg_sequence_source_route_set& routes) noexcept {
    return routes.dynamic_routes == nullptr
        ? routes.inline_routes.data() : routes.dynamic_routes;
}

[[nodiscard]] inline const pg_sequence_source_route*
pg_sequence_source_routes_data(
        const pg_sequence_source_route_set& routes) noexcept {
    return routes.dynamic_routes == nullptr
        ? routes.inline_routes.data() : routes.dynamic_routes;
}

[[nodiscard]] inline pg_sequence_source_registry_lease&
pg_sequence_source_route_lease(
        pg_sequence_source_route_set& routes,
        std::size_t index) noexcept {
    return pg_sequence_source_routes_data(routes)[index].lease;
}

[[nodiscard]] inline const pg_sequence_source_registry_lease&
pg_sequence_source_route_lease(
        const pg_sequence_source_route_set& routes,
        std::size_t index) noexcept {
    return pg_sequence_source_routes_data(routes)[index].lease;
}

inline void ensure_pg_sequence_source_route_capacity(
        pg_sequence_source_route_set& routes,
        std::size_t required) {
    const std::size_t current = pg_sequence_source_route_capacity(routes);
    if (routes.count > current) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq source route count is corrupt")));
    }
    if (required <= current) return;

    constexpr std::size_t entry_bytes = sizeof(pg_sequence_source_route);
    const std::size_t maximum = static_cast<std::size_t>(MaxAllocSize)
        / entry_bytes;
    if (required > maximum) {
        ereport(ERROR,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
             errmsg("bioseq source route set is too large")));
    }
    std::size_t capacity = current;
    while (capacity < required) {
        capacity = capacity > maximum / 2u ? maximum : capacity * 2u;
    }
    MemoryContext context = routes.allocation_context;
    if (context == nullptr) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq source route allocation context is unavailable")));
    }
    auto* grown = static_cast<pg_sequence_source_route*>(
        MemoryContextAllocZero(context, capacity * entry_bytes));
    std::memcpy(grown, pg_sequence_source_routes_data(routes),
        routes.count * entry_bytes);
    auto* old_dynamic = routes.dynamic_routes;
    routes.dynamic_routes = grown;
    routes.dynamic_capacity = capacity;
    if (old_dynamic != nullptr) pfree(old_dynamic);
}

[[nodiscard]] inline bool add_pg_sequence_source_route(
        pg_sequence_source_route_set& routes,
        const pg_sequence_source_registry_lease& parent) {
    if (routes.count > pg_sequence_source_route_capacity(routes)) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq source route count is corrupt")));
    }
    if (!sequence_source_detail::registry_lease_is_live(parent)) {
        return false;
    }
    const Oid relation_oid = parent.registry->user_relid;
    auto* entries = pg_sequence_source_routes_data(routes);
    for (std::size_t index = 0u; index < routes.count; ++index) {
        if (entries[index].lease.registry == parent.registry) {
            return true;
        }
    }
    if (routes.count == (std::numeric_limits<std::size_t>::max)()) {
        ereport(ERROR,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
             errmsg("bioseq source route count overflow")));
    }
    ensure_pg_sequence_source_route_capacity(routes, routes.count + 1u);
    entries = pg_sequence_source_routes_data(routes);
    auto lease = acquire_pg_sequence_source_registry_lease(parent);
    entries[routes.count] = pg_sequence_source_route{
        .relation_oid = relation_oid,
        .lease = lease,
    };
    ++routes.count;
    return true;
}

[[nodiscard]] inline bool add_pg_sequence_source_route(
        pg_sequence_source_route_set& routes,
        const pg_sequence_source_provider* provider) {
    return provider != nullptr && provider->open
        && add_pg_sequence_source_route(routes, provider->lease);
}

inline void rollback_pg_sequence_source_routes_noexcept(
        pg_sequence_source_route_set& routes,
        std::size_t original_count) noexcept {
    const std::size_t capacity = pg_sequence_source_route_capacity(routes);
    if (original_count > routes.count || routes.count > capacity) return;
    auto* entries = pg_sequence_source_routes_data(routes);
    while (routes.count > original_count) {
        const std::size_t index = routes.count - 1u;
        release_pg_sequence_source_registry_lease_noexcept(
            entries[index].lease,
            pg_sequence_source_sweep_boundary::upper_lease_end);
        entries[index] = {};
        routes.count = index;
    }
}

/** Acquire an independent upper-owner copy of every opaque child route. */
[[nodiscard]] inline bool merge_pg_sequence_source_routes(
        pg_sequence_source_route_set& destination,
        const pg_sequence_source_route_set& source) {
    using namespace sequence_source_detail;
    if (destination.count > pg_sequence_source_route_capacity(destination)
        || source.count > pg_sequence_source_route_capacity(source)) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq source route count is corrupt")));
    }
    std::size_t distinct_new_routes = 0u;
    const auto* source_entries = pg_sequence_source_routes_data(source);
    const auto* destination_entries = pg_sequence_source_routes_data(destination);
    for (std::size_t source_index = 0u;
         source_index < source.count; ++source_index) {
        const auto& source_route = source_entries[source_index];
        const auto& source_lease = source_route.lease;
        if (!registry_lease_is_live(source_lease)
            || source_route.relation_oid
                != source_lease.registry->user_relid) {
            return false;
        }
        bool already_present = false;
        for (std::size_t destination_index = 0u;
             destination_index < destination.count; ++destination_index) {
            if (destination_entries[destination_index].lease.registry
                == source_lease.registry) {
                already_present = true;
                break;
            }
        }
        if (!already_present) {
            for (std::size_t prior = 0u; prior < source_index; ++prior) {
                if (source_entries[prior].lease.registry
                    == source_lease.registry) {
                    already_present = true;
                    break;
                }
            }
        }
        if (!already_present) {
            ++distinct_new_routes;
        }
    }
    if (distinct_new_routes
        > (std::numeric_limits<std::size_t>::max)() - destination.count) {
        ereport(ERROR,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
             errmsg("bioseq source route count overflow")));
    }
    ensure_pg_sequence_source_route_capacity(
        destination, destination.count + distinct_new_routes);
    const std::size_t original_count = destination.count;
    volatile bool merge_ok = true;
    PG_TRY();
    {
        for (std::size_t source_index = 0u;
             source_index < source.count; ++source_index) {
            if (!add_pg_sequence_source_route(
                    destination, source_entries[source_index].lease)) {
                merge_ok = false;
                break;
            }
        }
    }
    PG_CATCH();
    {
        rollback_pg_sequence_source_routes_noexcept(
            destination, original_count);
        PG_RE_THROW();
    }
    PG_END_TRY();
    if (!merge_ok) {
        rollback_pg_sequence_source_routes_noexcept(
            destination, original_count);
    }
    return merge_ok;
}

[[nodiscard]] inline const pg_sequence_source_registry_lease*
find_pg_sequence_source_route(
        const pg_sequence_source_route_set& routes,
        Oid relation_oid,
        std::uint64_t provider_generation) noexcept {
    if (routes.count > pg_sequence_source_route_capacity(routes)
        || !OidIsValid(relation_oid) || provider_generation == 0u) {
        return nullptr;
    }
    const auto* entries = pg_sequence_source_routes_data(routes);
    const pg_sequence_source_registry_lease* match = nullptr;
    for (std::size_t index = 0u; index < routes.count; ++index) {
        const auto& route = entries[index];
        const auto& lease = route.lease;
        if (!sequence_source_detail::registry_lease_is_live(lease)
            || route.relation_oid != lease.registry->user_relid) {
            return nullptr;
        }
        if (route.relation_oid != relation_oid
            || lease.provider_generation != provider_generation) {
            continue;
        }
        if (match != nullptr && match->registry != lease.registry) {
            return nullptr;
        }
        match = &lease;
    }
    return match;
}

/** Relation-only lookup for validated single-source upper nodes.
 * It deliberately fails closed when self-join aliases contribute multiple
 * live registries for the same physical relation. */
[[nodiscard]] inline const pg_sequence_source_registry_lease*
find_pg_sequence_source_route(
        const pg_sequence_source_route_set& routes,
        Oid relation_oid) noexcept {
    if (routes.count > pg_sequence_source_route_capacity(routes)
        || !OidIsValid(relation_oid)) {
        return nullptr;
    }
    const auto* entries = pg_sequence_source_routes_data(routes);
    const pg_sequence_source_registry_lease* match = nullptr;
    for (std::size_t index = 0u; index < routes.count; ++index) {
        const auto& route = entries[index];
        const auto& lease = route.lease;
        if (!sequence_source_detail::registry_lease_is_live(lease)
            || route.relation_oid != lease.registry->user_relid) {
            return nullptr;
        }
        if (route.relation_oid != relation_oid) {
            continue;
        }
        if (match != nullptr && match->registry != lease.registry) {
            return nullptr;
        }
        match = &lease;
    }
    return match;
}

inline void release_pg_sequence_source_routes(
        pg_sequence_source_route_set& routes,
        bool is_commit) {
    if (routes.count > pg_sequence_source_route_capacity(routes)) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq source route count is corrupt")));
    }
    auto* entries = pg_sequence_source_routes_data(routes);
    for (std::size_t index = 0u; index < routes.count; ++index) {
        if (!release_pg_sequence_source_registry_lease(
                entries[index].lease,
                pg_sequence_source_sweep_boundary::upper_lease_end,
                is_commit)) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq source route lease accounting is corrupt")));
        }
        entries[index].relation_oid = InvalidOid;
    }
    routes.count = 0u;
}

inline void release_pg_sequence_source_routes_noexcept(
        pg_sequence_source_route_set& routes) noexcept {
    const std::size_t safe_count = std::min<std::size_t>(
        routes.count, pg_sequence_source_route_capacity(routes));
    auto* entries = pg_sequence_source_routes_data(routes);
    for (std::size_t index = 0u; index < safe_count; ++index) {
        release_pg_sequence_source_registry_lease_noexcept(
            entries[index].lease,
            pg_sequence_source_sweep_boundary::upper_lease_end);
        entries[index].relation_oid = InvalidOid;
    }
    routes.count = 0u;
}

[[nodiscard]] inline bool pg_sequence_source_registry_is_current_thread(
        const pg_sequence_source_registry_lease& lease) noexcept {
    return lease.registry != nullptr
        && lease.registry->backend_thread.is_current();
}

[[nodiscard]] inline Datum promote_pg_sequence_locator_impl(
        const pg_sequence_source_registry_lease& lease,
        AttrNumber column_attno,
        Datum locator_datum,
        MemoryContext result_context,
        slice_trace_attachment* trace = nullptr,
        std::uint32_t producer_value_id = 0u) {
    using namespace sequence_source_detail;
    if (!registry_lease_is_live(lease) || column_attno <= 0
        || result_context == nullptr) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq locator promotion has no live source-registry lease")));
    }
    auto& registry = *lease.registry;
    auto* header = bioseq::postgres::support::invoke_pg_as_cpp([locator_datum] {
        return reinterpret_cast<bioseq::pg_types::seq*>(
            PG_DETOAST_DATUM(locator_datum));
    });
    const pg_sequence_descriptor descriptor =
        classify_and_describe_sequence(PointerGetDatum(header));
    if (descriptor.kind != pg_sequence_datum_kind::locator) {
        ereport(ERROR,
            (errcode(ERRCODE_DATATYPE_MISMATCH),
             errmsg("bioseq source promotion requires a locator stub")));
    }
    const sequence_locator locator = decode_locator_from_stub(header);
    if (locator.logical_length
        > static_cast<std::uint64_t>(INT64_MAX)) {
        ereport(ERROR,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
             errmsg("bioseq locator logical length exceeds bigint")));
    }
    const compact_sequence_stdc_type_params stored_type{
        .alphabet_code = descriptor.alphabet_code,
        .storage_unit_byte_width = descriptor.storage_unit_byte_width,
    };
    const auto last_padding_letters = static_cast<std::uint16_t>(
        header->padding_letters);
    if (!locator_geometry_is_valid(
            locator, stored_type, last_padding_letters)) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq locator geometry does not match its sequence representation")));
    }
    const auto source_row = bioseq::postgres::support::invoke_pg_as_cpp(
        [source_epoch = locator.source_epoch,
         authoritative_snapshot = registry.authoritative_snapshot] {
            return sequence_source_version_manager::lookup(
                source_epoch, authoritative_snapshot,
                /*require_ready=*/true);
        });
    if (!source_row.has_value()
        || !source_row_matches_registry(
            registry, column_attno, locator, stored_type,
            last_padding_letters, *source_row)) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq locator no longer matches its immutable source"),
             errdetail("relation, column, snapshot, storage identity, source "
                       "epoch, locator, or type metadata changed")));
    }

    auto* domain = find_or_create_domain(
        registry, column_attno, stored_type);
    const pg_sequence_source_handle handle{
        .state = domain,
        .ops = &source_backend_ops(),
    };
    return bioseq::postgres::support::invoke_pg_as_cpp(
        [handle, locator, stored_type, last_padding_letters,
         result_context, trace, producer_value_id] {
        MemoryContextSwitchTo(result_context);
        return make_source_backed_pg_sequence(
            handle, locator, stored_type, last_padding_letters,
            trace, producer_value_id);
    });
}

[[nodiscard]] inline Datum promote_pg_sequence_locator(
        const pg_sequence_source_registry_lease& lease,
        AttrNumber column_attno,
        Datum locator_datum,
        MemoryContext result_context,
        slice_trace_attachment* trace = nullptr,
        std::uint32_t producer_value_id = 0u) {
    return bioseq::postgres::support::invoke_pg_as_cpp(
        [&lease, column_attno, locator_datum, result_context, trace,
         producer_value_id] {
            return promote_pg_sequence_locator_impl(
                lease, column_attno, locator_datum, result_context,
                trace, producer_value_id);
        });
}

[[nodiscard]] inline Datum promote_pg_sequence_locator(
        pg_sequence_source_provider* provider,
        AttrNumber column_attno,
        Datum locator_datum,
        MemoryContext result_context,
        slice_trace_attachment* trace = nullptr,
        std::uint32_t producer_value_id = 0u) {
    return bioseq::postgres::support::invoke_pg_as_cpp(
        [provider, column_attno, locator_datum, result_context, trace,
         producer_value_id] {
            if (provider == nullptr || !provider->open) {
                ereport(ERROR,
                    (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                     errmsg("bioseq locator promotion has no live owning provider")));
            }
            return promote_pg_sequence_locator(
                provider->lease, column_attno, locator_datum,
                result_context, trace, producer_value_id);
        });
}

[[nodiscard]] inline Datum promote_pg_sequence_locator(
        const pg_sequence_source_route_set& routes,
        Oid relation_oid,
        std::uint64_t provider_generation,
        AttrNumber column_attno,
        Datum locator_datum,
        MemoryContext result_context,
        slice_trace_attachment* trace = nullptr,
        std::uint32_t producer_value_id = 0u) {
    return bioseq::postgres::support::invoke_pg_as_cpp(
        [&routes, relation_oid, provider_generation, column_attno,
         locator_datum, result_context, trace, producer_value_id] {
            const auto* lease = find_pg_sequence_source_route(
                routes, relation_oid, provider_generation);
            if (lease == nullptr) {
                ereport(ERROR,
                    (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                     errmsg("bioseq locator has no matching source-provider route"),
                     errdetail("relation %u and provider generation %llu do "
                               "not identify one live registry",
                               relation_oid,
                               static_cast<unsigned long long>(
                                   provider_generation))));
            }
            return promote_pg_sequence_locator(
                *lease, column_attno, locator_datum, result_context,
                trace, producer_value_id);
        });
}

[[nodiscard]] inline Datum promote_pg_sequence_locator(
        const pg_sequence_source_route_set& routes,
        Oid relation_oid,
        AttrNumber column_attno,
        Datum locator_datum,
        MemoryContext result_context,
        slice_trace_attachment* trace = nullptr,
        std::uint32_t producer_value_id = 0u) {
    return bioseq::postgres::support::invoke_pg_as_cpp(
        [&routes, relation_oid, column_attno, locator_datum,
         result_context, trace, producer_value_id] {
            const auto* lease = find_pg_sequence_source_route(
                routes, relation_oid);
            if (lease == nullptr) {
                ereport(ERROR,
                    (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                     errmsg("bioseq locator has no unique source-provider route"),
                     errdetail("relation %u has no live route or is represented "
                               "by multiple scan bindings",
                               relation_oid)));
            }
            return promote_pg_sequence_locator(
                *lease, column_attno, locator_datum, result_context,
                trace, producer_value_id);
        });
}

#ifdef BIOSEQ_TEST_INDEXES
/**
 * Validate on a real TBB arena worker that the source callback rejects
 * before any PostgreSQL API, cache, snapshot, or logging side effect.
 */
[[nodiscard]] inline bool validate_pg_sequence_source_wrong_thread() {
    sequence_source_detail::pg_sequence_source_domain domain{};
    domain.backend_thread = pg_sequence_backend_thread_token::capture();
    pg_sequence_read_session session{};
    session.backend_thread = pg_sequence_backend_thread_token::capture();
    struct validation_argument {
        sequence_source_detail::pg_sequence_source_domain* domain;
        pg_sequence_read_session* session;
    } argument{&domain, &session};
    const bool rejected =
        bioseq::parallelism::global_pool::instance().invoke_worker_probe_noexcept(
        &argument, +[](void* opaque) noexcept {
            auto* validation = static_cast<validation_argument*>(opaque);
            const sequence_locator locator{};
            const compact_sequence_stdc_type_params stored_type{};
            return sequence_source_detail::read_domain_ranks(
                validation->domain, *validation->session, locator, stored_type,
                sequence_transform::identity, 0u,
                std::span<std::uint8_t>{})
                == pg_sequence_source_read_status::wrong_thread;
        });
    if (rejected) {
        /* Worker diagnostics are thread-local; mirror the observed rejection
         * once on the backend so SQL can assert it without reading worker TLS. */
        ++g_pg_sequence_source_stats.wrong_thread_rejections;
    }
    return rejected;
}

/** Exercise fail-closed retain accounting and merge rollback. */
[[nodiscard]] inline bool validate_pg_sequence_source_fault_contract() noexcept {
    sequence_source_detail::pg_sequence_source_domain domain{};
    domain.lifecycle = sequence_source_detail::domain_lifecycle::live;
    domain.refcount = std::numeric_limits<std::uint32_t>::max();
    domain.registry_token_owned = true;
    sequence_source_detail::retain_domain(&domain);

    pg_sequence_source_registry retained{};
    retained.backend_thread = pg_sequence_backend_thread_token::capture();
    retained.user_relid = static_cast<Oid>(1u);
    retained.generation = 1u;
    retained.active_leases = 1u;
    retained.accepting_lookups = true;
    pg_sequence_source_registry added{};
    added.backend_thread = pg_sequence_backend_thread_token::capture();
    added.user_relid = static_cast<Oid>(2u);
    added.generation = 2u;
    added.active_leases = 1u;
    added.accepting_lookups = true;
    pg_sequence_source_registry overflow{};
    overflow.backend_thread = pg_sequence_backend_thread_token::capture();
    overflow.user_relid = static_cast<Oid>(3u);
    overflow.generation = 3u;
    overflow.active_leases = std::numeric_limits<std::uint32_t>::max();
    overflow.accepting_lookups = true;

    pg_sequence_source_route_set destination{};
    initialize_pg_sequence_source_routes(destination, CurrentMemoryContext);
    destination.inline_routes[0] = pg_sequence_source_route{
        .relation_oid = retained.user_relid,
        .lease = pg_sequence_source_registry_lease{
            .registry = &retained,
            .provider_generation = retained.generation,
            .owned = true,
        },
    };
    destination.count = 1u;
    pg_sequence_source_route_set source{};
    source.inline_routes[0] = pg_sequence_source_route{
        .relation_oid = added.user_relid,
        .lease = pg_sequence_source_registry_lease{
            .registry = &added,
            .provider_generation = added.generation,
            .owned = true,
        },
    };
    source.inline_routes[1] = pg_sequence_source_route{
        .relation_oid = overflow.user_relid,
        .lease = pg_sequence_source_registry_lease{
            .registry = &overflow,
            .provider_generation = overflow.generation,
            .owned = true,
        },
    };
    source.count = 2u;

    volatile bool merge_rejected = false;
    PG_TRY();
    {
        (void)merge_pg_sequence_source_routes(destination, source);
    }
    PG_CATCH();
    {
        FlushErrorState();
        merge_rejected = true;
    }
    PG_END_TRY();
    const auto* destination_entries =
        pg_sequence_source_routes_data(destination);
    const bool overflow_and_merge_contract = domain.refcount_fault
        && domain.lifecycle == sequence_source_detail::domain_lifecycle::faulted
        && merge_rejected && overflow.lease_fault
        && !overflow.accepting_lookups
        && destination.count == 1u
        && destination_entries[0].lease.registry == &retained
        && destination_entries[0].lease.owned
        && retained.active_leases == 1u
        && added.active_leases == 1u;
    if (!overflow_and_merge_contract
        || sequence_source_detail::registries != nullptr
        || sequence_source_detail::providers != nullptr
        || !ActiveSnapshotSet()
        || GetActiveSnapshot() == nullptr
        || !IsMVCCSnapshot(GetActiveSnapshot())) {
        return false;
    }

    /* Build one real, readable source control, retain it as a piece
     * tree leaf does, and drive the production transaction cleanup callback.
     * The registry token is dropped by cleanup while the retained control
     * survives with refcount one, so the subsequent read reaches the expected
     * transaction-ended guard rather than a test-only approximation. */
    const auto stats_before_xact_end =
        sequence_source_detail::collect_pg_sequence_source_stats();
    MemoryContext saved_context = CurrentMemoryContext;
    pg_sequence_source_registry* volatile pending_registry = nullptr;
    pg_sequence_source_registry* volatile live_registry = nullptr;
    sequence_source_detail::pg_sequence_source_domain*
        volatile retained_domain = nullptr;
    volatile bool retained_reference_owned = false;
    volatile bool readable_before_cleanup = false;
    volatile bool cleanup_completed = false;
    volatile bool setup_or_cleanup_failed = false;
    PG_TRY();
    {
        sequence_source_detail::ensure_callbacks_registered();
        auto* registry = static_cast<pg_sequence_source_registry*>(
            MemoryContextAllocZero(
                TopMemoryContext, sizeof(pg_sequence_source_registry)));
        ::new (static_cast<void*>(registry)) pg_sequence_source_registry{};
        pending_registry = registry;
        registry->backend_thread =
            pg_sequence_backend_thread_token::capture();
        registry->binding_generation =
            sequence_source_detail::allocate_generation();
        registry->authoritative_snapshot = GetActiveSnapshot();
        registry->creating_subid = GetCurrentSubTransactionId();
        registry->generation =
            sequence_source_detail::allocate_generation();
        registry->accepting_lookups = true;
        registry->next = sequence_source_detail::registries;
        sequence_source_detail::registries = registry;
        live_registry = registry;
        pending_registry = nullptr;
        ++g_pg_sequence_source_stats.registries_created;

        auto* source_domain = sequence_source_detail::create_domain(
            *registry, static_cast<AttrNumber>(1),
            compact_sequence_stdc_type_params{});
        retained_domain = source_domain;
        readable_before_cleanup =
            sequence_source_detail::domain_is_readable(*source_domain);
        sequence_source_detail::retain_domain(source_domain);
        retained_reference_owned = true;

        sequence_source_detail::xact_callback(
            XACT_EVENT_PRE_COMMIT, nullptr);
        live_registry = nullptr;
        cleanup_completed = sequence_source_detail::registries == nullptr
            && source_domain->lifecycle
                == sequence_source_detail::domain_lifecycle::invalid
            && source_domain->refcount == 1u
            && !source_domain->registry_token_owned
            && source_domain->registry == nullptr;
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(saved_context);
        FlushErrorState();
        setup_or_cleanup_failed = true;
        auto* registry = const_cast<pg_sequence_source_registry*>(
            live_registry);
        if (registry != nullptr) {
            sequence_source_detail::abort_registry_at_xact_noerror(
                *registry);
            live_registry = nullptr;
        }
        auto* pending = const_cast<pg_sequence_source_registry*>(
            pending_registry);
        if (pending != nullptr) {
            pending->backend_thread.~pg_sequence_backend_thread_token();
            pfree(pending);
            pending_registry = nullptr;
        }
    }
    PG_END_TRY();
    MemoryContextSwitchTo(saved_context);

    volatile bool transaction_end_error_observed = false;
    if (!setup_or_cleanup_failed && cleanup_completed
        && readable_before_cleanup) {
        pg_sequence_read_session session{};
        session.backend_thread =
            pg_sequence_backend_thread_token::capture();
        PG_TRY();
        {
            (void)sequence_source_detail::read_domain_ranks_on_backend(
                const_cast<
                    sequence_source_detail::pg_sequence_source_domain*>(
                        retained_domain),
                session, sequence_locator{},
                compact_sequence_stdc_type_params{},
                sequence_transform::identity, 0u,
                std::span<std::uint8_t>{});
        }
        PG_CATCH();
        {
            MemoryContextSwitchTo(saved_context);
            ErrorData* error = CopyErrorData();
            transaction_end_error_observed = error != nullptr
                && error->sqlerrcode
                    == ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE
                && error->message != nullptr
                && std::strcmp(
                    error->message,
                    "bioseq source transaction has ended") == 0;
            FlushErrorState();
            if (error != nullptr) FreeErrorData(error);
        }
        PG_END_TRY();
        MemoryContextSwitchTo(saved_context);
    }

    auto* source_domain = const_cast<
        sequence_source_detail::pg_sequence_source_domain*>(
            retained_domain);
    if (retained_reference_owned && source_domain != nullptr) {
        sequence_source_detail::release_domain(source_domain);
        retained_reference_owned = false;
        retained_domain = nullptr;
    }
    const auto stats_after_xact_end =
        sequence_source_detail::collect_pg_sequence_source_stats();
    return !setup_or_cleanup_failed && readable_before_cleanup
        && cleanup_completed && transaction_end_error_observed
        && stats_after_xact_end.registries_created
            == stats_before_xact_end.registries_created + 1u
        && stats_after_xact_end.registries_destroyed
            == stats_before_xact_end.registries_destroyed + 1u
        && stats_after_xact_end.domains_created
            == stats_before_xact_end.domains_created + 1u
        && stats_after_xact_end.domains_destroyed
            == stats_before_xact_end.domains_destroyed + 1u
        && stats_after_xact_end.cache_sets_created
            == stats_before_xact_end.cache_sets_created + 1u
        && stats_after_xact_end.cache_sets_destroyed
            == stats_before_xact_end.cache_sets_destroyed + 1u
        && stats_after_xact_end.snapshots_registered
            == stats_before_xact_end.snapshots_registered + 1u
        && stats_after_xact_end.snapshots_unregistered
            == stats_before_xact_end.snapshots_unregistered + 1u
        && stats_after_xact_end.cleanup_commit
            == stats_before_xact_end.cleanup_commit + 1u
        && stats_after_xact_end.release_before_locks
            == stats_before_xact_end.release_before_locks + 1u
        && stats_after_xact_end.release_locks
            == stats_before_xact_end.release_locks + 1u
        && stats_after_xact_end.release_after_locks
            == stats_before_xact_end.release_after_locks + 1u
        && stats_after_xact_end.refcount_faults
            == stats_before_xact_end.refcount_faults
        && stats_after_xact_end.lease_faults
            == stats_before_xact_end.lease_faults
        && stats_after_xact_end.leases_acquired
            == stats_before_xact_end.leases_acquired
        && stats_after_xact_end.leases_released
            == stats_before_xact_end.leases_released
        && stats_after_xact_end.live_registries
            == stats_before_xact_end.live_registries
        && stats_after_xact_end.live_providers
            == stats_before_xact_end.live_providers
        && stats_after_xact_end.live_domains
            == stats_before_xact_end.live_domains
        && stats_after_xact_end.live_leases
            == stats_before_xact_end.live_leases
        && stats_after_xact_end.live_cache_sets
            == stats_before_xact_end.live_cache_sets
        && stats_after_xact_end.live_snapshots
            == stats_before_xact_end.live_snapshots;
}

/** Validate that self-join aliases retain independent routes while the
 * relation-only lookup refuses to guess between them. */
[[nodiscard]] inline bool validate_pg_sequence_source_route_aliases() noexcept {
    constexpr Oid relation_oid = static_cast<Oid>(1u);
    pg_sequence_source_registry first{};
    first.backend_thread = pg_sequence_backend_thread_token::capture();
    first.user_relid = relation_oid;
    first.generation = 41u;
    first.active_leases = 1u;
    first.accepting_lookups = true;
    pg_sequence_source_registry second{};
    second.backend_thread = pg_sequence_backend_thread_token::capture();
    second.user_relid = relation_oid;
    second.generation = 42u;
    second.active_leases = 1u;
    second.accepting_lookups = true;

    pg_sequence_source_route_set routes{};
    routes.inline_routes[0] = pg_sequence_source_route{
        .relation_oid = relation_oid,
        .lease = pg_sequence_source_registry_lease{
            .registry = &first,
            .provider_generation = first.generation,
            .owned = true,
        },
    };
    routes.inline_routes[1] = pg_sequence_source_route{
        .relation_oid = relation_oid,
        .lease = pg_sequence_source_registry_lease{
            .registry = &second,
            .provider_generation = second.generation,
            .owned = true,
        },
    };
    routes.count = 2u;

    return find_pg_sequence_source_route(routes, relation_oid) == nullptr
        && find_pg_sequence_source_route(
               routes, relation_oid, first.generation)
            == &routes.inline_routes[0].lease
        && find_pg_sequence_source_route(
               routes, relation_oid, second.generation)
            == &routes.inline_routes[1].lease
        && find_pg_sequence_source_route(
               routes, relation_oid, 43u) == nullptr;
}

/** Install one inert-but-live token and drive the real PRE_PREPARE switch. */
inline void validate_pg_sequence_source_pre_prepare() {
    using namespace sequence_source_detail;
    ensure_callbacks_registered();
    auto* registry = static_cast<pg_sequence_source_registry*>(
        MemoryContextAllocZero(
            TopMemoryContext, sizeof(pg_sequence_source_registry)));
    ::new (static_cast<void*>(registry)) pg_sequence_source_registry{};
    registry->backend_thread = pg_sequence_backend_thread_token::capture();
    registry->creating_subid = GetCurrentSubTransactionId();
    registry->generation = allocate_generation();
    registry->binding_generation = 1u;
    registry->accepting_lookups = true;

    pg_sequence_source_domain* volatile pending_domain = nullptr;
    PG_TRY();
    {
        auto* domain = static_cast<pg_sequence_source_domain*>(
            MemoryContextAllocZero(
                TopMemoryContext, sizeof(pg_sequence_source_domain)));
        pending_domain = domain;
        ::new (static_cast<void*>(domain)) pg_sequence_source_domain{};
        domain->backend_thread = pg_sequence_backend_thread_token::capture();
        domain->registry = registry;
        domain->creating_subid = registry->creating_subid;
        domain->generation = allocate_generation();
        domain->binding_generation = registry->binding_generation;
        domain->refcount = 1u;
        domain->registry_token_owned = true;
        domain->lifecycle = domain_lifecycle::live;
        registry->domains = domain;
        registry->next = registries;
        registries = registry;
        pending_domain = nullptr;
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.registries_created;
        ++g_pg_sequence_source_stats.domains_created;
#endif
    }
    PG_CATCH();
    {
        auto* domain = const_cast<pg_sequence_source_domain*>(pending_domain);
        if (domain != nullptr) {
            domain->backend_thread.~pg_sequence_backend_thread_token();
            pfree(domain);
        }
        registry->backend_thread.~pg_sequence_backend_thread_token();
        pfree(registry);
        PG_RE_THROW();
    }
    PG_END_TRY();
    xact_callback(XACT_EVENT_PRE_PREPARE, nullptr);
    ereport(ERROR,
        (errcode(ERRCODE_INTERNAL_ERROR),
         errmsg("bioseq PRE_PREPARE source-domain validation did not reject")));
}
#endif

}  // namespace bioseq::storage
