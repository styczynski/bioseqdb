/*-------------------------------------------------------------------------
 *
 * storage/sequence_scanner_pg.cppm
 *
 * DOC0264 §7 — `sequence_scanner_pg<StdcInstance>`: the production
 * Postgres-backed scanner satisfying Phase B's `is_active_sequence_scanner`
 * concept.
 *
 * The scanner reads ONE fragment per sub-table row via Phase A's
 * `seq_caches` + `BioseqColumnReadRowByRowNumber`, memcpys exactly
 * `fragment_storage_units * sizeof(storage_unit)` bytes into a slot in a
 * per-scanner `MemoryContext`, and exposes the bytes through Phase B's
 * `read_fragment(n)` API. Eviction (`release_fragment`) recycles one of the
 * fixed slot buffers; checked success finalization deletes the whole context.
 *
 * RSS per scanner is bounded at `fragments_buffer_capacity *
 * fragment_storage_units * sizeof(storage_unit)` (~16 × 8 KiB = 128 KiB on
 * dna4/uint64) irrespective of sequence length (DOC0264 R10/R11).
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

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
#include <new>
#include <span>
#include <type_traits>
#include <utility>

export module bioseq.storage.scanner_pg;

import bioseq.common.mlogging;
import bioseq.pg_utils;
import bioseq.common.sequan_compat;              // seq::dna4 in static_assert below
import bioseq.storage.manager;             // DOC0264 §7.9 — seq_caches + open_sub_table_for_locator
import bioseq.storage.scanner;     // DOC0263 §3.4 — is_valid/active_sequence_scanner concept
import bioseq.storage.lazy;        // DOC0264 §9.1 — lazy_compact_sequence + with_lazy_postgres
import bioseq.storage.coordinates;
import bioseq.storage.physical;
import bioseq.storage.size;        // sequence_size_t
import bioseq.storage.metadata;    // StripeMetadata/FindStripeByRowNumber
import bioseq.sequences;           // compact_sequence_range, compact_sequence_stdc_instance
import bioseq.types.pg_compact_sequence;
import bioseq.table.io;            // BioseqColumnBeginRead/BioseqColumnReadRowByRowNumber
import bioseq.parallelism.pool;

static_assert(__cplusplus >= 202302L, "DOC0264 requires C++23-or-later");

export namespace bioseq::storage {

using scanner_pg_fragment_read_callback = void (*)(void*);

#ifdef BIOSEQ_TEST_INDEXES
/** One-shot PostgreSQL leaf failures for scanner unwind integration tests. */
enum class ScannerPgLeafFailure : std::uint8_t {
    none,
    interrupt_before,
    interrupt_after,
    row_read_before,
    row_read_after,
    palloc_before,
    palloc_after,
};

inline thread_local ScannerPgLeafFailure g_scanner_pg_leaf_failure =
    ScannerPgLeafFailure::none;

inline void arm_scanner_pg_leaf_failure(ScannerPgLeafFailure point) noexcept {
    g_scanner_pg_leaf_failure = point;
}

/** Test cleanup hook; safe even when the selected leaf was never reached. */
inline void disarm_scanner_pg_leaf_failure() noexcept {
    g_scanner_pg_leaf_failure = ScannerPgLeafFailure::none;
}

[[nodiscard]] inline bool scanner_pg_leaf_failure_is_armed() noexcept {
    return g_scanner_pg_leaf_failure != ScannerPgLeafFailure::none;
}

inline void inject_scanner_pg_leaf_failure(ScannerPgLeafFailure point) {
    if (g_scanner_pg_leaf_failure != point) {
        return;
    }
    g_scanner_pg_leaf_failure = ScannerPgLeafFailure::none;
    ereport(ERROR,
        (errcode(ERRCODE_INTERNAL_ERROR),
         errmsg("injected sequence scanner PostgreSQL leaf failure"),
         errdetail("failure point %u", static_cast<unsigned>(point))));
}
#endif

/**
 * Compatibility trampoline retained for the generic packed-range caller.
 * Every PostgreSQL leaf used by read_fragment is now converted locally with
 * pg_utils::invoke_pg_as_cpp, so this function must not place a broad setjmp
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

// DOC0264 §7 — `sequence_scanner_pg<S>`.
//
// Production fragment-streaming scanner. Two construction modes:
//
//   1. PG-backed (production target path): constructed from a locator + seq_caches.
//      Reads one fragment per sub-table row via Phase A's `seq_caches` +
//      `BioseqColumnReadRowByRowNumber`, memcpys the bytes into a slot in a per-scanner
//      MemoryContext, exposes them through Phase B's `read_fragment(n)` API.
//      `release_fragment` pfrees the slot.
//
//   2. In-memory (query / inline target path): constructed from a span over an
//      existing `compact_sequence_range<R>`'s packed storage units. `read_fragment(n)`
//      returns a subspan with NO allocation, NO MemoryContext. `release_fragment`
//      is a no-op (caller owns the bytes). This lets a query literal (always inline)
//      share the same scanner type as a PG-backed target without doubling the
//      dispatch_buffer template tree (DOC0274).
template <class StdcInstance>
struct sequence_scanner_pg final {
    using storage_unit   = typename StdcInstance::storage_unit;
    using base_container = std::span<const storage_unit>;
    using traits         = typename compact_sequence_range<StdcInstance>::traits;

    static constexpr std::size_t fragment_storage_units =
        SCANNER_SEQUENCE_FRAGMENT_SIZE_ALIGNMENT;

    [[nodiscard]] [[gnu::always_inline]]
    static constexpr bool supports_fragments() noexcept { return true; }

    [[nodiscard]] [[gnu::always_inline]]
    sequence_size_t total_bp_size() const noexcept { return total_bp_size_; }
    [[nodiscard]] [[gnu::always_inline]]
    sequence_size_t total_size() const noexcept {
        constexpr std::size_t lpu = traits::letters_per_unit;
        return (total_bp_size_ + lpu - 1) / lpu;
    }
    [[nodiscard]] [[gnu::always_inline]]
    sequence_size_t total_byte_size() const noexcept {
        return total_size() * sizeof(storage_unit);
    }
    [[nodiscard]] [[gnu::always_inline]]
    std::size_t number_of_fragments() const noexcept { return num_fragments_; }
    [[nodiscard]] [[gnu::always_inline]]
    sequence_size_t estimated_total_bp_size() const noexcept { return total_bp_size_; }
    [[nodiscard]] [[gnu::always_inline]]
    std::size_t last_fragment_padding_letters() const noexcept {
        return last_fragment_padding_;
    }

    [[nodiscard]] [[gnu::always_inline]]
    base_container read_fragment(std::size_t fragment_no) const;

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
     * The cleanup callback is retained only for source compatibility and is
     * no longer called. PostgreSQL leaves inside read_fragment are converted
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
    explicit sequence_scanner_pg(
        Oid db_oid, Oid tablespace_oid, Oid main_relfilenode,
        std::int16_t column_attno, const seq_locator& locator,
        std::size_t last_fragment_padding,
        Snapshot registered_snapshot,
        seq_caches* caches, MemoryContext parent_mctx)
        : db_oid_(db_oid),
          tablespace_oid_(tablespace_oid),
          main_relfilenode_(main_relfilenode),
          column_attno_(column_attno),
          locator_(locator),
          total_bp_size_(locator.total_bp_size),
          last_fragment_padding_(last_fragment_padding),
          num_fragments_(locator.num_fragments),
          snapshot_(registered_snapshot),
          caches_(caches),
          mctx_(nullptr),
          in_memory_data_{}
    {
        if (snapshot_ == nullptr
            || !IsMVCCSnapshot(snapshot_)
            || snapshot_->regd_count == 0) {
            bioseq::pg_utils::invoke_pg_as_cpp([] {
                bioseq_log<ERR, PG>(
                    "sequence_scanner_pg: requires a borrowed registered MVCC snapshot");
            });
        }
        if (caches_ == nullptr) {
            bioseq::pg_utils::invoke_pg_as_cpp([] {
                bioseq_log<ERR, PG>(
                    "sequence_scanner_pg: caches pointer must be non-null");
            });
        }
        if (caches_->side != seq_caches::Side::Read) {
            bioseq::pg_utils::invoke_pg_as_cpp([] {
                bioseq_log<ERR, PG>(
                    "sequence_scanner_pg: requires read-side seq_caches");
            });
        }
        if (parent_mctx == nullptr) {
            bioseq::pg_utils::invoke_pg_as_cpp([] {
                bioseq_log<ERR, PG>(
                    "sequence_scanner_pg: parent_mctx must be non-null");
            });
        }
        mctx_ = bioseq::pg_utils::invoke_pg_as_cpp([parent_mctx] {
            return AllocSetContextCreate(
                parent_mctx,
                "bioseq sequence_scanner_pg",
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
    explicit sequence_scanner_pg(
        base_container in_memory_data,
        sequence_size_t total_bp_size,
        std::size_t last_fragment_padding) noexcept
        : db_oid_(InvalidOid),
          tablespace_oid_(InvalidOid),
          main_relfilenode_(InvalidOid),
          column_attno_(0),
          locator_{},
          total_bp_size_(total_bp_size),
          last_fragment_padding_(last_fragment_padding),
          num_fragments_(
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

    sequence_scanner_pg(const sequence_scanner_pg&)            = delete;
    sequence_scanner_pg& operator=(const sequence_scanner_pg&) = delete;

    sequence_scanner_pg(sequence_scanner_pg&& other) noexcept
        : db_oid_(other.db_oid_),
          tablespace_oid_(other.tablespace_oid_),
          main_relfilenode_(other.main_relfilenode_),
          column_attno_(other.column_attno_),
          locator_(other.locator_),
          total_bp_size_(other.total_bp_size_),
          last_fragment_padding_(other.last_fragment_padding_),
          num_fragments_(other.num_fragments_),
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
    sequence_scanner_pg& operator=(sequence_scanner_pg&& other) noexcept {
        if (this == &other) return *this;
        this->~sequence_scanner_pg();
        new (this) sequence_scanner_pg(std::move(other));
        return *this;
    }

    ~sequence_scanner_pg() { abandon_pg_resources_noexcept(); }

    [[nodiscard]] [[gnu::always_inline]]
    bool is_in_memory_mode() const noexcept { return mctx_ == nullptr; }

    /// Prove that this non-forgeable scanner instance was constructed for one
    /// exact PostgreSQL source binding.  No raw identity field is exposed: an
    /// administrative writer may only ask whether all relation, snapshot and
    /// locator axes agree with its independently derived source proof.
    [[nodiscard]] [[gnu::always_inline]]
    bool matches_pg_binding(
            Oid db_oid,
            Oid tablespace_oid,
            Oid main_relfilenode,
            std::int16_t column_attno,
            const seq_locator& locator,
            std::size_t last_fragment_padding,
            Snapshot registered_snapshot) const noexcept {
        return mctx_ != nullptr
            && !poisoned_
            && in_memory_data_.empty()
            && caches_ != nullptr
            && caches_->side == seq_caches::Side::Read
            && caches_->main_database_oid == db_oid
            && caches_->main_tablespace_oid == tablespace_oid
            && caches_->main_relfilenode == main_relfilenode
            && db_oid_ == db_oid
            && tablespace_oid_ == tablespace_oid
            && main_relfilenode_ == main_relfilenode
            && column_attno_ == column_attno
            && snapshot_ == registered_snapshot
            && last_fragment_padding_ == last_fragment_padding
            && locator_.version == locator.version
            && locator_.bucket_idx == locator.bucket_idx
            && locator_._pad_2 == locator._pad_2
            && locator_.shard_idx == locator.shard_idx
            && locator_.num_fragments == locator.num_fragments
            && locator_._pad_4 == locator._pad_4
            && locator_.total_bp_size == locator.total_bp_size
            && locator_.first_sub_row_no == locator.first_sub_row_no
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
    seq_locator                                          locator_;
    sequence_size_t                                      total_bp_size_;
    std::size_t                                          last_fragment_padding_;
    std::size_t                                          num_fragments_;
    // Borrowed from the executor/admin owner. The scanner neither registers
    // nor unregisters it, and every cached fragment reader starts from this
    // statement snapshot. The column reader may own a refreshed registered
    // copy when it flushes same-transaction pending writes; it never mutates
    // the registration owned by this scanner's caller.
    Snapshot                                             snapshot_;
    seq_caches*                                          caches_;
    mutable MemoryContext                                mctx_;
    // DOC0274 — in-memory mode backing span. Empty in PG-backed mode.
    base_container                                       in_memory_data_;
    // A PostgreSQL ERROR may leave a cached column reader or fragment slot
    // partially mutated. Never reuse this scanner in the failed statement;
    // PostgreSQL's ResourceOwner/transaction cleanup owns the PG resources.
    mutable bool                                         poisoned_ = false;
    // Fixed-size slot ring matching the wrapper's
    // fragments_buffer_capacity (16). LRU-ish eviction in
    // allocate_slot_for_; the Phase-B wrapper typically drives
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

template <class StdcInstance>
[[gnu::always_inline]]
inline auto sequence_scanner_pg<StdcInstance>::read_fragment(std::size_t fragment_no) const
    -> base_container
{
    if (poisoned_) {
        bioseq::pg_utils::invoke_pg_as_cpp([] {
            ereport(ERROR,
                (errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
                 errmsg("sequence_scanner_pg cannot be reused after a PostgreSQL read error")));
        });
    }
    // Each PG leaf below throws through the ordinary C++ ABI.  Mark the
    // scanner before the first leaf and clear it only after a complete slot
    // publication; an interrupted read therefore cannot be reused.
    poisoned_ = true;
    if (fragment_no >= num_fragments_) {
        bioseq::pg_utils::invoke_pg_as_cpp([&] {
            bioseq_log<ERR, PG>(
                "sequence_scanner_pg: read_fragment({}) >= num_fragments_({})",
                fragment_no, num_fragments_);
        });
    }

    // DOC0274 — in-memory mode: subspan the caller-owned span, no allocation.
    if (is_in_memory_mode()) {
        const std::size_t start = fragment_no * fragment_storage_units;
        if (start >= in_memory_data_.size()) {
            bioseq::pg_utils::invoke_pg_as_cpp([&] {
                bioseq_log<ERR, PG>(
                    "sequence_scanner_pg in-memory: fragment {} starts at unit {} but "
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

    // DOC0276 — COLD fragment read: a cache miss that hits the columnar storage.
    // This is the per-fragment work that dominates a large scanner_pg target
    // scan (the native aligner driving it is NO_POSTGRES and cannot itself poll
    // for interrupts). Honour cancellation here so a long scan responds to
    // pg_cancel_backend, and emit a throttled progress heartbeat (count-based;
    // the fmtlog timestamp header gives the rate) so the scan is observably
    // alive. Throttling keeps this off the hot per-fragment cost when logging.
    bioseq::pg_utils::invoke_pg_as_cpp([] {
#ifdef BIOSEQ_TEST_INDEXES
        inject_scanner_pg_leaf_failure(
            ScannerPgLeafFailure::interrupt_before);
#endif
        CHECK_FOR_INTERRUPTS();
#ifdef BIOSEQ_TEST_INDEXES
        inject_scanner_pg_leaf_failure(
            ScannerPgLeafFailure::interrupt_after);
#endif
    });
    if (enable_logging) {
        static thread_local std::uint64_t cold_read_count = 0;
        if ((cold_read_count++ % 4000) == 0) {
            bioseq_log_progress<DBG>("[scanner_pg] cold fragment read #{} (fragment {}/{})",
                cold_read_count, fragment_no, num_fragments_);
        }
    }

    // Resolve the sub-table relation + read state via Phase A's cache.
    Relation shard = bioseq::pg_utils::invoke_pg_as_cpp([&] {
        return open_sub_table_for_locator(
            db_oid_, tablespace_oid_, main_relfilenode_, column_attno_,
            locator_, AccessShareLock, caches_);
    });

    const std::uint64_t cache_key = seq_caches::key_for(column_attno_,
        static_cast<std::int16_t>(locator_.bucket_idx),
        static_cast<std::int32_t>(locator_.shard_idx));
    seq_caches::entry& entry = caches_->entries[cache_key];
    entry.rel = shard;

    // Compatibility is keyed by the caller-owned source snapshot. The
    // column reader may replace its own snapshot pointer with a refreshed
    // registered copy after flushing, so comparing read_state->snapshot would
    // incorrectly discard a compatible cached reader.
    if (entry.read_state != nullptr
        && entry.read_snapshot_source != snapshot_) {
        bioseq::pg_utils::invoke_pg_as_cpp(
            [&] { BioseqColumnEndRead(entry.read_state); });
        entry.read_state = nullptr;
        entry.read_snapshot_source = nullptr;
    }
    if (entry.read_state == nullptr) {
        MemoryContext payload_mctx = caches_->read_state_mctx != nullptr
            ? caches_->read_state_mctx
            : CurrentMemoryContext;
        TupleDesc td = RelationGetDescr(shard);
        PBitmapset attr_needed = bioseq::pg_utils::invoke_pg_as_cpp(
            [&] {
                MemoryContext previous = MemoryContextSwitchTo(payload_mctx);
                PBitmapset result = PBitmapset::full_range(td->natts);
                MemoryContextSwitchTo(previous);
                return result;
            });
        PIntList projected;
        for (int i = 0; i < td->natts; ++i) {
            const bool needed = bioseq::pg_utils::invoke_pg_as_cpp(
                [&] { return attr_needed.contains(i); });
            if (!TupleDescAttr(td, i)->attisdropped && needed) {
                bioseq::pg_utils::invoke_pg_as_cpp([&] {
                    MemoryContext previous = MemoryContextSwitchTo(payload_mctx);
                    projected.append(static_cast<AttrNumber>(i + 1));
                    MemoryContextSwitchTo(previous);
                });
            }
        }
        List* projected_attributes = projected.release();
        entry.read_state = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            return BioseqColumnBeginRead(
                shard, td, projected_attributes,
                /*whereClauseList=*/NIL, payload_mctx, snapshot_,
                /*randomAccess=*/true, nullptr);
        });
        bioseq::pg_utils::invoke_pg_as_cpp(
            [&] { BioseqColumnReadFlushPendingWrites(entry.read_state); });
        entry.read_snapshot_source = snapshot_;
        bioseq::pg_utils::invoke_pg_as_cpp([&] { attr_needed.free(); });
    }

    const std::uint64_t row_no = locator_.first_sub_row_no + fragment_no;
    Datum vals[1]  = { 0 };
    bool  nulls[1] = { true };
    const bool row_visible = bioseq::pg_utils::invoke_pg_as_cpp([&] {
#ifdef BIOSEQ_TEST_INDEXES
        inject_scanner_pg_leaf_failure(
            ScannerPgLeafFailure::row_read_before);
#endif
        const bool result = BioseqColumnReadRowByRowNumber(
            entry.read_state, row_no, vals, nulls);
#ifdef BIOSEQ_TEST_INDEXES
        inject_scanner_pg_leaf_failure(
            ScannerPgLeafFailure::row_read_after);
#endif
        return result;
    });
    if (!row_visible) {
        bioseq::pg_utils::invoke_pg_as_cpp([&] {
            bioseq_log<ERR, PG>(
                "sequence_scanner_pg: sub-table row {} missing (db={}, spc={}, "
                "relfilenode={}, attno={}, bucket={}, shard={}, fragment={}/{})",
                row_no,
                static_cast<unsigned>(db_oid_),
                static_cast<unsigned>(tablespace_oid_),
                static_cast<unsigned>(main_relfilenode_),
                static_cast<int>(column_attno_),
                static_cast<unsigned>(locator_.bucket_idx),
                locator_.shard_idx, fragment_no, num_fragments_);
        });
    }
    if (nulls[0]) {
        bioseq::pg_utils::invoke_pg_as_cpp([&] {
            bioseq_log<ERR, PG>(
                "sequence_scanner_pg: sub-table row {} is NULL (fragment {} of {})",
                row_no, fragment_no, num_fragments_);
        });
    }
    pgseq_t* raw = bioseq::pg_utils::invoke_pg_as_cpp([&vals] {
        return reinterpret_cast<pgseq_t*>(PG_DETOAST_DATUM(vals[0]));
    });
    if (static_cast<std::size_t>(VARSIZE(raw)) < pgseq_t::header_bytes()) {
        bioseq::pg_utils::invoke_pg_as_cpp([row_no] {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("sequence_scanner_pg fragment header is truncated"),
                 errdetail("row=%llu",
                    static_cast<unsigned long long>(row_no))));
        });
    }
    if (is_locator_stub(raw)) {
        bioseq::pg_utils::invoke_pg_as_cpp([row_no] {
            bioseq_log<ERR, PG>(
                "sequence_scanner_pg: sub-table fragment row {} is itself a stub "
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
            typename StdcInstance::alphabet_type>();
    const bool is_last_fragment = fragment_no + 1u == num_fragments_;
    const std::size_t raw_payload_bytes =
        static_cast<std::size_t>(VARSIZE(raw)) - pgseq_t::header_bytes();
    const std::uint64_t fragment_from =
        static_cast<std::uint64_t>(fragment_no) * letters_per_fragment;
    const bool fragment_geometry_valid = fragment_from < total_bp_size_
        && (is_last_fragment
            || total_bp_size_ - fragment_from >= letters_per_fragment);
    const std::uint64_t expected_fragment_bp = !fragment_geometry_valid
        ? 0u
        : (is_last_fragment
            ? total_bp_size_ - fragment_from
            : letters_per_fragment);
    if (static_cast<std::uint8_t>(raw->alphabet_code) != expected_alphabet
        || static_cast<std::uint8_t>(raw->storage_unit_byte_width)
            != sizeof(storage_unit)
        || (!is_last_fragment && raw->padding_letters != 0u)
        || (is_last_fragment
            && static_cast<std::size_t>(raw->padding_letters)
                != last_fragment_padding_)
        || !fragment_geometry_valid
        || static_cast<std::uint64_t>(raw->bp_size)
            != expected_fragment_bp
        || raw_payload_bytes != bytes_to_copy) {
        bioseq::pg_utils::invoke_pg_as_cpp(
            [row_no, raw_payload_bytes, is_last_fragment,
             expected_padding = last_fragment_padding_,
             expected_fragment_bp] {
                ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("sequence_scanner_pg fragment metadata is corrupt"),
                     errdetail(
                        "row=%llu payload_bytes=%zu last_fragment=%s expected_padding=%zu expected_fragment_bp=%llu",
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
        // palloc0 so the trailing-bytes-of-last-fragment region the wrapper
        // sees is uniformly zero (DOC0264 §7.3 / consolidated-review N20).
        dst = bioseq::pg_utils::invoke_pg_as_cpp([&] {
            MemoryContext previous = MemoryContextSwitchTo(mctx_);
#ifdef BIOSEQ_TEST_INDEXES
            inject_scanner_pg_leaf_failure(
                ScannerPgLeafFailure::palloc_before);
#endif
            auto* result = static_cast<storage_unit*>(palloc0(bytes_to_copy));
#ifdef BIOSEQ_TEST_INDEXES
            inject_scanner_pg_leaf_failure(
                ScannerPgLeafFailure::palloc_after);
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

template <class StdcInstance>
inline void sequence_scanner_pg<StdcInstance>::copy_packed_to(
    std::span<storage_unit> destination) const
{
    const std::size_t required_units = static_cast<std::size_t>(total_size());
    if (destination.size() != required_units) {
        bioseq::pg_utils::invoke_pg_as_cpp([&] {
            bioseq_log<ERR, PG>(
                "sequence_scanner_pg: resident destination has {} packed units, "
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
            bioseq::pg_utils::invoke_pg_as_cpp([&] {
                bioseq_log<ERR, PG>(
                    "sequence_scanner_pg in-memory: backing span has {} packed "
                    "units, expected at least {}",
                    in_memory_data_.size(), destination.size());
            });
        }
        std::copy_n(
            in_memory_data_.data(), destination.size(), destination.data());
        return;
    }
    if (poisoned_) {
        bioseq::pg_utils::invoke_pg_as_cpp([] {
            ereport(ERROR,
                (errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
                 errmsg("sequence_scanner_pg cannot be reused after a PostgreSQL read error")));
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
        .expected_row = locator_.first_sub_row_no,
    };

    /*
     * Keep this as one checked PostgreSQL leaf. The callback owns only POD
     * state and raw pointers; the caller-owned resident vector remains outside
     * the PG_TRY frame. A PostgreSQL ERROR therefore returns through the C++
     * ABI without skipping a vector destructor, while the hot path pays one
     * sigsetjmp instead of one per fragment.
     */
    poisoned_ = true;
    bioseq::pg_utils::invoke_pg_as_cpp([this, &state] {
        Relation shard = open_sub_table_for_locator(
            db_oid_, tablespace_oid_, main_relfilenode_, column_attno_,
            locator_, AccessShareLock, caches_);
        const std::uint64_t cache_key = seq_caches::key_for(
            column_attno_,
            static_cast<std::int16_t>(locator_.bucket_idx),
            static_cast<std::int32_t>(locator_.shard_idx));
        seq_caches::entry& entry = caches_->entries[cache_key];
        entry.rel = shard;

        MemoryContext previous = MemoryContextSwitchTo(mctx_);
        TupleDesc td = RelationGetDescr(shard);
        List* projected_attributes = list_make1_int(1);
        BioseqColumnReadState* read_state = BioseqColumnBeginRead(
            shard, td, projected_attributes,
            /*whereClauseList=*/NIL, mctx_, snapshot_,
            /*randomAccess=*/true, nullptr);
        BioseqColumnReadFlushPendingWrites(read_state);

        StripeMetadata* first_stripe = FindStripeByRowNumber(
            shard, locator_.first_sub_row_no, read_state->snapshot);
        if (first_stripe == nullptr
            || !BioseqColumnSetStripeReadState(read_state, first_stripe)) {
            state.status = bulk_copy_status::missing_start_stripe;
        }

        constexpr std::size_t fragment_bytes =
            fragment_storage_units * sizeof(storage_unit);
        while (state.status == bulk_copy_status::ok
               && state.copied_fragments < num_fragments_) {
            if ((state.copied_fragments & std::size_t{0x3ff}) == 0) {
                CHECK_FOR_INTERRUPTS();
            }

            Datum values[1] = {0};
            bool nulls[1] = {true};
            std::uint64_t row_no = 0;
            if (!BioseqColumnReadNextRow(
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

            pgseq_t* raw = reinterpret_cast<pgseq_t*>(
                PG_DETOAST_DATUM(values[0]));
            if (is_locator_stub(raw)) {
                state.status = bulk_copy_status::stub_fragment;
                break;
            }
            const std::size_t raw_total_bytes =
                static_cast<std::size_t>(VARSIZE(raw));
            if (raw_total_bytes < pgseq_t::header_bytes()) {
                state.status =
                    bulk_copy_status::payload_header_invalid;
                break;
            }
            const std::size_t raw_payload_bytes =
                raw_total_bytes - pgseq_t::header_bytes();
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

        BioseqColumnEndRead(read_state);
        MemoryContextSwitchTo(previous);
    });

    if (state.status != bulk_copy_status::ok
        || state.copied_fragments != num_fragments_) {
        bioseq::pg_utils::invoke_pg_as_cpp([this, &state] {
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
                 errmsg("sequence_scanner_pg bulk resident copy failed"),
                 errdetail(
                    "reason=%s expected_row=%llu observed_row=%llu "
                    "copied_fragments=%zu expected_fragments=%zu "
                    "payload_bytes=%zu",
                    reason,
                    static_cast<unsigned long long>(state.expected_row),
                    static_cast<unsigned long long>(state.observed_row),
                    state.copied_fragments,
                    num_fragments_,
                    state.observed_payload_bytes)));
        });
    }
    poisoned_ = false;
}

template <class StdcInstance>
[[gnu::always_inline]]
inline void sequence_scanner_pg<StdcInstance>::release_fragment(
    std::size_t fragment_no) const noexcept
{
    // DOC0274 — in-memory mode: no slot ownership, no-op release.
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

template <class StdcInstance>
inline void sequence_scanner_pg<StdcInstance>::finish_pg_resources_as_cpp() const {
    MemoryContext context = std::exchange(mctx_, nullptr);
    if (context == nullptr) {
        return;
    }
    for (auto& slot : slots_) {
        slot.fragment_no = std::numeric_limits<std::size_t>::max();
        slot.bytes = nullptr;
    }
    poisoned_ = true;
    bioseq::pg_utils::invoke_pg_as_cpp([&] {
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

template <class StdcInstance>
inline void sequence_scanner_pg<StdcInstance>::abandon_pg_resources_noexcept() const noexcept {
    // No PG calls here.  Memory contexts are children of the caller-provided
    // statement/transaction context and PostgreSQL will reclaim them.
    mctx_ = nullptr;
    for (auto& slot : slots_) {
        slot.fragment_no = std::numeric_limits<std::size_t>::max();
        slot.bytes = nullptr;
    }
    poisoned_ = true;
}

// Concept conformance checks — fail at module load if Phase B's concept
// changes without us catching it.
static_assert(is_valid_sequence_scanner<sequence_scanner_pg<
    compact_sequence_stdc_instance<pgseq_t, seq::dna4, std::size_t{8}>>>);
static_assert(is_active_sequence_scanner<sequence_scanner_pg<
    compact_sequence_stdc_instance<pgseq_t, seq::dna4, std::size_t{8}>>>);

/**
 * Immutable proof of the exact PostgreSQL relation, locator and lifetime
 * resources from which one callback-scoped lazy facade may be constructed.
 *
 * Keeping this value separate from the facade lets administrative writers
 * prove publication identity without reaching through the facade to its
 * concrete scanner.  Construction and validation remain in this module; a
 * callback receives this value only when it independently needs source proof.
 */
template <class R>
class postgres_lazy_sequence_binding final {
public:
    using sequence_config = R;
    using alphabet_type =
        typename compact_sequence_range<R>::traits::alphabet_type;

    postgres_lazy_sequence_binding(
        Oid db_oid,
        Oid tablespace_oid,
        Oid main_relfilenode,
        std::int16_t column_attno,
        const seq_locator& locator,
        std::size_t last_fragment_padding,
        Snapshot registered_snapshot,
        seq_caches* caches,
        MemoryContext parent_mctx) noexcept
        : db_oid_(db_oid),
          tablespace_oid_(tablespace_oid),
          main_relfilenode_(main_relfilenode),
          column_attno_(column_attno),
          locator_(locator),
          last_fragment_padding_(last_fragment_padding),
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
    [[nodiscard]] const seq_locator& locator() const noexcept {
        return locator_;
    }
    [[nodiscard]] std::size_t last_fragment_padding() const noexcept {
        return last_fragment_padding_;
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
            sizeof(typename compact_sequence_range<R>::traits::storage_unit));
    }
    [[nodiscard]] static constexpr std::uint64_t
    fragment_bp_capacity() noexcept {
        return static_cast<std::uint64_t>(
            sequence_scanner_pg<R>::fragment_storage_units)
            * static_cast<std::uint64_t>(
                compact_sequence_range<R>::traits::letters_per_unit);
    }
    [[nodiscard]] static constexpr std::size_t
    expected_last_fragment_padding(const seq_locator& locator) noexcept {
        constexpr std::uint64_t letters_per_unit =
            compact_sequence_range<R>::traits::letters_per_unit;
        const std::uint64_t remainder =
            locator.total_bp_size % letters_per_unit;
        return static_cast<std::size_t>(
            remainder == 0u ? 0u : letters_per_unit - remainder);
    }

    [[nodiscard]] bool exact_source_match(
            const postgres_lazy_sequence_binding& other) const noexcept {
        return db_oid_ == other.db_oid_
            && tablespace_oid_ == other.tablespace_oid_
            && main_relfilenode_ == other.main_relfilenode_
            && column_attno_ == other.column_attno_
            && std::memcmp(&locator_, &other.locator_, sizeof(locator_)) == 0
            && last_fragment_padding_ == other.last_fragment_padding_
            && registered_snapshot_ == other.registered_snapshot_
            && caches_ == other.caches_
            && parent_mctx_ == other.parent_mctx_;
    }

private:
    const Oid db_oid_;
    const Oid tablespace_oid_;
    const Oid main_relfilenode_;
    const std::int16_t column_attno_;
    const seq_locator locator_;
    const std::size_t last_fragment_padding_;
    Snapshot const registered_snapshot_;
    seq_caches* const caches_;
    MemoryContext const parent_mctx_;
};

namespace lazy_postgres_detail {

template <class R>
[[nodiscard]] constexpr compact_sequence_stdc_type_params
binding_type_params() noexcept {
    return {
        .alphabet_code = postgres_lazy_sequence_binding<R>::alphabet_code(),
        .storage_unit_byte_width =
            postgres_lazy_sequence_binding<R>::storage_unit_byte_width(),
    };
}

template <class R>
void validate_binding(const postgres_lazy_sequence_binding<R>& binding) {
    using traits = typename compact_sequence_range<R>::traits;
    constexpr std::uint64_t letters_per_unit = traits::letters_per_unit;
    constexpr std::uint64_t fragment_units =
        sequence_scanner_pg<R>::fragment_storage_units;
    const auto& locator = binding.locator();
    const std::uint64_t packed_units =
        locator.total_bp_size / letters_per_unit
        + static_cast<std::uint64_t>(
            locator.total_bp_size % letters_per_unit != 0u);
    const std::uint64_t expected_fragments =
        packed_units / fragment_units
        + static_cast<std::uint64_t>(packed_units % fragment_units != 0u);
    const std::uint64_t expected_padding =
        packed_units * letters_per_unit - locator.total_bp_size;

    const bool valid_locator = locator.version == kLocatorVersion
        && locator._pad_2 == 0u && locator._pad_4 == 0u
        && locator.source_epoch != 0u
        && expected_fragments == locator.num_fragments
        && expected_padding == binding.last_fragment_padding();
    const bool valid_snapshot = binding.registered_snapshot() != nullptr
        && IsMVCCSnapshot(binding.registered_snapshot())
        && binding.registered_snapshot()->regd_count != 0;
    const bool valid_caches = binding.caches() != nullptr
        && binding.caches()->side == seq_caches::Side::Read
        && binding.caches()->main_database_oid == binding.db_oid()
        && binding.caches()->main_tablespace_oid == binding.tablespace_oid()
        && binding.caches()->main_relfilenode == binding.main_relfilenode();
    if (!valid_locator || !valid_snapshot || !valid_caches
        || binding.parent_mctx() == nullptr || binding.column_attno() <= 0) {
        bioseq::pg_utils::invoke_pg_as_cpp([] {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("lazy PostgreSQL sequence binding is invalid")));
        });
    }
}

template <class R>
struct call_state final {
    using scanner_type = sequence_scanner_pg<R>;
    using sequence_type = lazy_compact_sequence<R, scanner_type>;

    explicit call_state(
            const postgres_lazy_sequence_binding<R>& binding,
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
            bioseq::pg_utils::invoke_pg_as_cpp([] {
                ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("lazy PostgreSQL scanner does not match its binding")));
            });
        }
    }

    sequence_type sequence;
};

template <class R>
struct call_state_factory_argument final {
    const postgres_lazy_sequence_binding<R>* binding;
};

template <class R>
[[nodiscard]] void* create_call_state(
        void* opaque,
        MemoryContext payload_context) {
    const auto& factory =
        *static_cast<const call_state_factory_argument<R>*>(opaque);
    call_state<R>* state = nullptr;
    ErrorData* exact_error = nullptr;
    bool cpp_error = false;
    bool unknown_cpp_error = false;
    char detail[512] = "std::exception without detail";
    try {
        validate_binding(*factory.binding);
        void* storage = bioseq::pg_utils::invoke_pg_as_cpp([payload_context] {
            return MemoryContextAllocZero(
                payload_context, sizeof(call_state<R>));
        });
        state = ::new (storage) call_state<R>(
            *factory.binding, payload_context);
    } catch (bioseq::pg_utils::PostgresErrorAsCpp& error) {
        exact_error = error.release_error_data();
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
    if (exact_error != nullptr) {
        ReThrowError(exact_error);
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

template <class R>
void finish_call_state(void* opaque, bool) {
    auto* state = static_cast<call_state<R>*>(opaque);
    state->sequence.scanner().finish_pg_resources_as_cpp();
    state->~call_state();
}

template <class R>
void abort_call_state_noexcept(void* opaque) noexcept {
    auto* state = static_cast<call_state<R>*>(opaque);
    state->sequence.scanner().abandon_pg_resources_noexcept();
    state->~call_state();
}

template <class R>
[[nodiscard]] call_state<R>* register_call_state(
        pg_sequence_read_session& session,
        const postgres_lazy_sequence_binding<R>& binding) {
    const pg_sequence_read_entry_key key{
        .source = {
            .control_address = reinterpret_cast<std::uintptr_t>(&binding),
            .generation = 0u,
        },
        .locator = binding.locator(),
        .stored_type_params = binding_type_params<R>(),
    };
    call_state_factory_argument<R> factory{.binding = &binding};
    return static_cast<call_state<R>*>(find_or_create_pg_sequence_read_entry(
        session, key, &factory, &create_call_state<R>, &finish_call_state<R>,
        &abort_call_state_noexcept<R>));
}

template <class Fn>
void with_session(MemoryContext parent_mctx, Fn&& fn) {
    std::exception_ptr callback_failure;
    pg_sequence_read_session* volatile session = nullptr;
    PG_TRY();
    {
        auto* local_session = begin_pg_sequence_read_session(parent_mctx);
        session = local_session;
        try {
            std::forward<Fn>(fn)(*local_session);
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
template <class R, class Fn>
inline void with_lazy_postgres(
        postgres_lazy_sequence_binding<R> binding,
        Fn&& fn) {
    lazy_postgres_detail::with_session(
        binding.parent_mctx(),
        [&binding, &fn](pg_sequence_read_session& session) {
            auto* state =
                lazy_postgres_detail::register_call_state(session, binding);
            if (state == nullptr) {
                bioseq::pg_utils::invoke_pg_as_cpp([] {
                    ereport(ERROR,
                        (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                         errmsg("lazy PostgreSQL read session rejected its binding")));
                });
            }
            std::forward<Fn>(fn)(as_lazy_sequence_view(state->sequence));
        });
}

/**
 * Invoke a two-input callback under one read session.  Exact repeated bindings
 * reuse one concrete call state, so both facades share fragment-cache state.
 */
template <class LeftR, class RightR, class Fn>
inline void with_lazy_postgres(
        postgres_lazy_sequence_binding<LeftR> left_binding,
        postgres_lazy_sequence_binding<RightR> right_binding,
        Fn&& fn) {
    using left_alphabet =
        typename postgres_lazy_sequence_binding<LeftR>::alphabet_type;
    using right_alphabet =
        typename postgres_lazy_sequence_binding<RightR>::alphabet_type;
    static_assert(std::is_same_v<left_alphabet, right_alphabet>,
                  "two lazy PostgreSQL inputs must share one alphabet");
    if (left_binding.parent_mctx() != right_binding.parent_mctx()) {
        bioseq::pg_utils::invoke_pg_as_cpp([] {
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
                bioseq::pg_utils::invoke_pg_as_cpp([] {
                    ereport(ERROR,
                        (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                         errmsg("lazy PostgreSQL read session rejected its left binding")));
                });
            }

            if constexpr (std::is_same_v<LeftR, RightR>) {
                if (left_binding.exact_source_match(right_binding)) {
                    const auto shared = as_lazy_sequence_view(left->sequence);
                    std::forward<Fn>(fn)(shared, shared);
                    return;
                }
            }

            auto* right = lazy_postgres_detail::register_call_state(
                session, right_binding);
            if (right == nullptr) {
                bioseq::pg_utils::invoke_pg_as_cpp([] {
                    ereport(ERROR,
                        (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                         errmsg("lazy PostgreSQL read session rejected its right binding")));
                });
            }
            std::forward<Fn>(fn)(as_lazy_sequence_view(left->sequence),
                                 as_lazy_sequence_view(right->sequence));
        });
}

// DOC0274 — construct a sequence_scanner_pg in IN-MEMORY mode from an existing
// compact_sequence_range. Used to wrap query literals or already-hydrated
// inline targets so they share the production `sequence_scanner_pg<R>` template
// type with stub-backed (PG-backed) targets — eliminating the need for a
// separate in-memory scanner template in the dispatch_buffer pipeline tree.
//
// `range` must outlive the returned scanner: its `read_cptr(cbegin())` pointer
// is captured into the scanner's backing span, and the scanner just slices it
// per fragment with no copy.
template <class R>
[[nodiscard]] [[gnu::always_inline]]
inline sequence_scanner_pg<R> make_scanner_pg_in_memory(
    const compact_sequence_range<R>& range) noexcept
{
    using storage_unit = typename compact_sequence_range<R>::traits::storage_unit;
    const std::size_t units = static_cast<std::size_t>(range.size());
    std::span<const storage_unit> backing =
        units == 0
            ? std::span<const storage_unit>{}
            : std::span<const storage_unit>(range.read_cptr(range.cbegin()), units);
    const sequence_size_t total_bp = static_cast<sequence_size_t>(range.bp_size());
    const std::size_t padding = static_cast<std::size_t>(range.padding());
    return sequence_scanner_pg<R>{backing, total_bp, padding};
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

/** Fixed two-provider routing used by binary/join upper wrappers. */
struct pg_sequence_source_route_set {
    std::array<Oid, 2u> relation_oids{InvalidOid, InvalidOid};
    std::array<pg_sequence_source_registry_lease, 2u> leases{};
    std::uint8_t count = 0u;
};

#ifdef BIOSEQ_TEST_INDEXES
enum class PgSequenceSourceFailure : std::uint8_t {
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
};

struct PgSequenceSourceDebugCounters {
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

inline thread_local PgSequenceSourceFailure g_pg_sequence_source_failure =
    PgSequenceSourceFailure::none;
inline thread_local PgSequenceSourceDebugCounters
    g_pg_sequence_source_debug_counters{};

inline void
arm_pg_sequence_source_failure(PgSequenceSourceFailure point) noexcept {
  g_pg_sequence_source_failure = point;
}

inline void disarm_pg_sequence_source_failure() noexcept {
  g_pg_sequence_source_failure = PgSequenceSourceFailure::none;
  disarm_pg_sequence_facade_test_poison();
}

[[nodiscard]] inline PgSequenceSourceDebugCounters
pg_sequence_source_debug_counters() noexcept;

inline void reset_pg_sequence_source_debug_counters() noexcept {
  g_pg_sequence_source_debug_counters = {};
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

enum class owner_release_phase_bits : std::uint8_t {
    none = 0u,
    before_locks = 1u,
    locks = 2u,
    after_locks = 4u,
};

[[nodiscard]] constexpr std::uint8_t phase_bit(
        owner_release_phase_bits phase) noexcept {
    return static_cast<std::uint8_t>(phase);
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
    std::uint8_t released_owner_phases = 0u;
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
    seq_locator locator{};
    compact_sequence_stdc_type_params stored_type{};
};

inline pg_sequence_source_registry *registries = nullptr;
inline pg_sequence_source_provider *providers = nullptr;
inline std::uint64_t next_generation = 1u;
inline bool callbacks_registered = false;

#ifdef BIOSEQ_TEST_INDEXES
[[nodiscard]] inline PgSequenceSourceDebugCounters
collect_pg_sequence_source_debug_counters() noexcept {
  PgSequenceSourceDebugCounters result = g_pg_sequence_source_debug_counters;
  const PgSequenceFacadeTestCounters facade =
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
inline void inject_pg_sequence_source_failure(PgSequenceSourceFailure point) {
    if (g_pg_sequence_source_failure != point) {
        return;
    }
    g_pg_sequence_source_failure = PgSequenceSourceFailure::none;
    ++g_pg_sequence_source_debug_counters.fault_injections;
    if (point == PgSequenceSourceFailure::read_before_scanner) {
        ++g_pg_sequence_source_debug_counters.read_fault_injections;
    }
    ereport(ERROR,
        (errcode(ERRCODE_INTERNAL_ERROR),
         errmsg("injected PostgreSQL sequence source-domain failure"),
         errdetail("failure point %u", static_cast<unsigned>(point))));
}
#endif

[[nodiscard]] inline bool same_locator(
        const seq_locator& left, const seq_locator& right) noexcept {
    return std::memcmp(&left, &right, sizeof(seq_locator)) == 0;
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
        ++g_pg_sequence_source_debug_counters.refcount_faults;
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
        ++g_pg_sequence_source_debug_counters.refcount_faults;
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
        ++g_pg_sequence_source_debug_counters.refcount_faults;
#endif
        return;
    }
    domain->lifecycle = domain_lifecycle::released;
#ifdef BIOSEQ_TEST_INDEXES
    ++g_pg_sequence_source_debug_counters.domains_destroyed;
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
[[nodiscard]] inline PgSequenceSourceDebugCounters
pg_sequence_source_debug_counters() noexcept {
    return sequence_source_detail::collect_pg_sequence_source_debug_counters();
}
#endif

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
        ++g_pg_sequence_source_debug_counters.cache_sets_destroyed;
#endif
    }
    PG_CATCH();
    {
        FlushErrorState();
        if (caches != nullptr) {
            destroy_seq_caches(caches);
            caches = nullptr;
#ifdef BIOSEQ_TEST_INDEXES
            ++g_pg_sequence_source_debug_counters.cache_sets_destroyed;
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
        ++g_pg_sequence_source_debug_counters.snapshots_unregistered;
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

inline void abort_release_owner_phase_noerror(
        pg_sequence_source_domain& domain,
        ResourceReleasePhase phase,
        owner_release_phase_bits phase_flag) noexcept {
    if (domain.resource_owner == nullptr
        || (domain.released_owner_phases & phase_bit(phase_flag)) != 0u) {
        return;
    }
#ifdef BIOSEQ_TEST_INDEXES
    switch (phase_flag) {
        case owner_release_phase_bits::before_locks:
            ++g_pg_sequence_source_debug_counters.release_before_locks;
            break;
        case owner_release_phase_bits::locks:
            ++g_pg_sequence_source_debug_counters.release_locks;
            break;
        case owner_release_phase_bits::after_locks:
            ++g_pg_sequence_source_debug_counters.release_after_locks;
            break;
        case owner_release_phase_bits::none:
            Assert(false);
            break;
    }
#endif
    PG_TRY();
    {
        ResourceOwnerRelease(
            domain.resource_owner, phase, false, false);
        domain.released_owner_phases |= phase_bit(phase_flag);
    }
    PG_CATCH();
    {
        FlushErrorState();
        /* Mark the attempted abort phase. PostgreSQL's parent abort cleanup is
         * the final owner of a resource whose release callback itself failed. */
        domain.released_owner_phases |= phase_bit(phase_flag);
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
    ++g_pg_sequence_source_debug_counters.cleanup_abort;
#endif
    abort_close_caches_noerror(domain.read_caches);
    abort_unregister_snapshot_noerror(domain);
    abort_release_owner_phase_noerror(
        domain, RESOURCE_RELEASE_BEFORE_LOCKS,
        owner_release_phase_bits::before_locks);
    abort_release_owner_phase_noerror(
        domain, RESOURCE_RELEASE_LOCKS,
        owner_release_phase_bits::locks);
    abort_release_owner_phase_noerror(
        domain, RESOURCE_RELEASE_AFTER_LOCKS,
        owner_release_phase_bits::after_locks);
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
        ++g_pg_sequence_source_debug_counters.cleanup_commit;
    } else {
        ++g_pg_sequence_source_debug_counters.cleanup_abort;
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
                PgSequenceSourceFailure::close_before_caches);
#endif
            close_and_destroy_seq_caches(domain.read_caches, is_commit);
            domain.read_caches = nullptr;
#ifdef BIOSEQ_TEST_INDEXES
            ++g_pg_sequence_source_debug_counters.cache_sets_destroyed;
#endif
        }
        if (domain.registered_snapshot != nullptr) {
#ifdef BIOSEQ_TEST_INDEXES
            inject_pg_sequence_source_failure(
                PgSequenceSourceFailure::close_before_snapshot);
#endif
            UnregisterSnapshotFromOwner(
                domain.registered_snapshot, owner);
            domain.registered_snapshot = nullptr;
#ifdef BIOSEQ_TEST_INDEXES
            ++g_pg_sequence_source_debug_counters.snapshots_unregistered;
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

    if ((domain.released_owner_phases
            & phase_bit(owner_release_phase_bits::before_locks)) == 0u) {
#ifdef BIOSEQ_TEST_INDEXES
        inject_pg_sequence_source_failure(
            PgSequenceSourceFailure::close_before_before_locks);
#endif
        ResourceOwnerRelease(
            owner, RESOURCE_RELEASE_BEFORE_LOCKS, is_commit, false);
        domain.released_owner_phases |=
            phase_bit(owner_release_phase_bits::before_locks);
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_debug_counters.release_before_locks;
#endif
    }
    if ((domain.released_owner_phases
            & phase_bit(owner_release_phase_bits::locks)) == 0u) {
#ifdef BIOSEQ_TEST_INDEXES
        inject_pg_sequence_source_failure(
            PgSequenceSourceFailure::close_before_locks);
#endif
        ResourceOwnerRelease(
            owner, RESOURCE_RELEASE_LOCKS, is_commit, false);
        domain.released_owner_phases |=
            phase_bit(owner_release_phase_bits::locks);
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_debug_counters.release_locks;
#endif
    }
    if ((domain.released_owner_phases
            & phase_bit(owner_release_phase_bits::after_locks)) == 0u) {
#ifdef BIOSEQ_TEST_INDEXES
        inject_pg_sequence_source_failure(
            PgSequenceSourceFailure::close_before_after_locks);
#endif
        ResourceOwnerRelease(
            owner, RESOURCE_RELEASE_AFTER_LOCKS, is_commit, false);
        domain.released_owner_phases |=
            phase_bit(owner_release_phase_bits::after_locks);
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_debug_counters.release_after_locks;
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
        ++g_pg_sequence_source_debug_counters.refcount_faults;
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
        pg_sequence_source_sweep_boundary,
        bool is_commit) {
#ifdef BIOSEQ_TEST_INDEXES
    ++g_pg_sequence_source_debug_counters.sweeps;
#endif
    auto* domain = registry.domains;
    while (domain != nullptr) {
        auto* next = domain->next;
        const bool reclaimable = domain->registry_token_owned
            && domain->refcount == 1u
            && !domain->refcount_fault
            && domain->lifecycle == domain_lifecycle::live;
        if (reclaimable) {
            cleanup_domain(*domain, is_commit);
            unlink_domain(registry, *domain);
            drop_registry_token_noexcept(*domain);
#ifdef BIOSEQ_TEST_INDEXES
            ++g_pg_sequence_source_debug_counters.swept_domains;
#endif
        }
        domain = next;
    }
}

inline void abort_sweep_registry_noerror(
        pg_sequence_source_registry& registry,
        bool all_domains) noexcept {
#ifdef BIOSEQ_TEST_INDEXES
    ++g_pg_sequence_source_debug_counters.sweeps;
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
            ++g_pg_sequence_source_debug_counters.swept_domains;
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
    ++g_pg_sequence_source_debug_counters.registries_destroyed;
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
    ++g_pg_sequence_source_debug_counters.providers_destroyed;
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
        ++g_pg_sequence_source_debug_counters.lease_faults;
#endif
        lease = {};
        return false;
    }
    --registry->active_leases;
#ifdef BIOSEQ_TEST_INDEXES
    ++g_pg_sequence_source_debug_counters.leases_released;
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
            ++g_pg_sequence_source_debug_counters
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
        ++g_pg_sequence_source_debug_counters
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
                    ++g_pg_sequence_source_debug_counters.pre_prepare_rejections;
#endif
                    ereport(ERROR,
                        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                         errmsg("cannot PREPARE TRANSACTION with live bioseq source domains"),
                         errhint("materialize locator-backed bioseq values before PREPARE TRANSACTION")));
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
                    ++g_pg_sequence_source_debug_counters.subxact_promotions;
#endif
                }
                for (auto* domain = registry->domains; domain != nullptr;
                     domain = domain->next) {
                    if (domain->creating_subid == my_subid) {
                        domain->creating_subid = parent_subid;
#ifdef BIOSEQ_TEST_INDEXES
                        ++g_pg_sequence_source_debug_counters.subxact_promotions;
#endif
                    }
                }
            }
            break;
        case SUBXACT_EVENT_ABORT_SUB: {
#ifdef BIOSEQ_TEST_INDEXES
            ++g_pg_sequence_source_debug_counters.subxact_aborts;
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
        const seq_locator& locator,
        compact_sequence_stdc_type_params stored_type,
        std::uint16_t last_padding_letters,
        const SequenceSourceVersionRow& row) noexcept {
    return row.state == kSequenceSourceStateReady
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
            PgSequenceSourceFailure::domain_after_control);
#endif
        domain->payload_context = AllocSetContextCreate(
            TopTransactionContext, "bioseq shared sequence source domain",
            ALLOCSET_DEFAULT_SIZES);
#ifdef BIOSEQ_TEST_INDEXES
        inject_pg_sequence_source_failure(
            PgSequenceSourceFailure::domain_after_context);
#endif
        domain->resource_owner = ResourceOwnerCreate(
            TopTransactionResourceOwner,
            "bioseq shared sequence source domain");
#ifdef BIOSEQ_TEST_INDEXES
        inject_pg_sequence_source_failure(
            PgSequenceSourceFailure::domain_after_owner);
#endif
        CurrentResourceOwner = domain->resource_owner;
        domain->registered_snapshot = RegisterSnapshotOnOwner(
            registry.authoritative_snapshot, domain->resource_owner);
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_debug_counters.snapshots_registered;
        inject_pg_sequence_source_failure(
            PgSequenceSourceFailure::domain_after_snapshot);
#endif
        domain->read_caches = alloc_seq_caches(domain->payload_context);
        domain->read_caches->side = seq_caches::Side::Read;
        domain->read_caches->lock_mode = AccessShareLock;
        domain->read_caches->read_state_mctx = domain->payload_context;
        domain->read_caches->read_resource_owner = domain->resource_owner;
        domain->read_caches->main_database_oid = registry.database_oid;
        domain->read_caches->main_tablespace_oid = registry.tablespace_oid;
        domain->read_caches->main_relfilenode = registry.main_relfilenode;
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_debug_counters.cache_sets_created;
        inject_pg_sequence_source_failure(
            PgSequenceSourceFailure::domain_after_caches);
#endif
        CurrentResourceOwner = saved_owner;
        MemoryContextSwitchTo(saved_context);
        domain->lifecycle = domain_lifecycle::live;
        domain->next = registry.domains;
        registry.domains = domain;
        result = domain;
        pending = nullptr;
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_debug_counters.domains_created;
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
            ++g_pg_sequence_source_debug_counters.domains_reused;
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
             errmsg("bioseq source registry requires an owning relation and registered MVCC snapshot")));
    }
    ensure_callbacks_registered();
    const std::uint64_t storage_id =
        bioseq::pg_utils::invoke_pg_as_cpp([owning_relation] {
            return BioseqColumnStorageGetStorageId(
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
            PgSequenceSourceFailure::provider_after_registry_control);
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
            PgSequenceSourceFailure::provider_after_provider_control);
#endif

        registry->next = registries;
        registries = registry;
        provider->next = providers;
        providers = provider;
        result = provider;
        pending_registry = nullptr;
        pending_provider = nullptr;
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_debug_counters.registries_created;
        ++g_pg_sequence_source_debug_counters.providers_created;
        if (kind == pg_sequence_source_provider_kind::scan) {
            ++g_pg_sequence_source_debug_counters.scan_providers_created;
        } else {
            ++g_pg_sequence_source_debug_counters
                .standalone_providers_created;
        }
        ++g_pg_sequence_source_debug_counters.leases_acquired;
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

template <class Instance>
[[nodiscard]] constexpr std::uint16_t locator_padding_letters(
        const seq_locator& locator) noexcept {
    using range = compact_sequence_range<Instance>;
    constexpr std::uint64_t letters_per_unit =
        range::traits::letters_per_unit;
    const std::uint64_t remainder =
        locator.total_bp_size % letters_per_unit;
    return static_cast<std::uint16_t>(
        remainder == 0u ? 0u : letters_per_unit - remainder);
}

[[nodiscard]] inline bool locator_geometry_is_valid(
        const seq_locator& locator,
        compact_sequence_stdc_type_params stored_type,
        std::uint16_t last_padding_letters,
        bool validate_padding = true) {
    bool dispatched = false;
    bool valid = false;
    pgseq_t::dispatch_instance(
        stored_type.alphabet_code,
        stored_type.storage_unit_byte_width,
        [&dispatched, &valid, locator,
         last_padding_letters, validate_padding]<class Instance>() {
            using range = compact_sequence_range<Instance>;
            constexpr std::uint64_t letters_per_fragment =
                static_cast<std::uint64_t>(
                    sequence_scanner_pg<Instance>::fragment_storage_units)
                * range::traits::letters_per_unit;
            const std::uint64_t complete =
                locator.total_bp_size / letters_per_fragment;
            const std::uint64_t remainder =
                locator.total_bp_size % letters_per_fragment;
            const std::uint64_t expected_fragments =
                complete + (remainder == 0u ? 0u : 1u);
            dispatched = true;
            valid = locator.version == kLocatorVersion
                && locator._pad_2 == 0u && locator._pad_4 == 0u
                && locator.source_epoch != 0u
                && locator.num_fragments == expected_fragments
                && (!validate_padding
                    || last_padding_letters
                        == locator_padding_letters<Instance>(locator));
        });
    return dispatched && valid;
}

template <class Instance>
inline void typed_scanner_read_ranks(
        source_scanner_entry& entry,
        sequence_transform transform,
        std::uint64_t source_from,
        std::span<std::uint8_t> output) {
    using scanner_type = sequence_scanner_pg<Instance>;
    using range = compact_sequence_range<Instance>;
    using traits = typename range::traits;
    using unit = typename traits::storage_unit;
    auto& scanner = *static_cast<scanner_type*>(entry.concrete_scanner);
    constexpr std::uint64_t letters_per_fragment =
        static_cast<std::uint64_t>(scanner_type::fragment_storage_units)
        * traits::letters_per_unit;
    const bool reverse = has_reverse(transform);
    const bool complement = has_complement(transform);

    for (std::size_t output_index = 0u;
         output_index < output.size(); ++output_index) {
        const std::uint64_t physical = source_from
            + (reverse ? output.size() - 1u - output_index
                       : output_index);
        const std::size_t fragment_no = static_cast<std::size_t>(
            physical / letters_per_fragment);
        const auto packed = scanner.read_fragment(fragment_no);
        const std::uint64_t local_symbol =
            physical % letters_per_fragment;
        const std::size_t unit_index = static_cast<std::size_t>(
            local_symbol / traits::letters_per_unit);
        if (unit_index >= packed.size()) {
            bioseq::pg_utils::invoke_pg_as_cpp(
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
                        typename Instance::alphabet_type>) {
            bioseq::pg_utils::invoke_pg_as_cpp(
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
                bioseq::pg_utils::invoke_pg_as_cpp([] {
                    ereport(ERROR,
                        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                         errmsg("bioseq source alphabet has no complement operation")));
                });
            }
        }
        output[output_index] = rank;
    }
}

template <class Instance>
inline void finish_typed_scanner_entry(source_scanner_entry& entry) {
    using scanner_type = sequence_scanner_pg<Instance>;
    auto* scanner = static_cast<scanner_type*>(entry.concrete_scanner);
    if (scanner == nullptr || !entry.constructed) {
        return;
    }
    scanner->finish_pg_resources_as_cpp();
    scanner->~scanner_type();
    entry.concrete_scanner = nullptr;
    entry.constructed = false;
}

template <class Instance>
inline void abort_typed_scanner_entry_noexcept(
        source_scanner_entry& entry) noexcept {
    using scanner_type = sequence_scanner_pg<Instance>;
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
    ErrorData* exact_error = nullptr;
    bool cpp_error = false;
    bool unknown_cpp_error = false;
    char detail[512] = "std::exception without detail";
    if (entry->finish != nullptr) {
        try {
            entry->finish(*entry);
        } catch (bioseq::pg_utils::PostgresErrorAsCpp& error) {
            exact_error = error.release_error_data();
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
    if (exact_error != nullptr) {
        ReThrowError(exact_error);
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
    ErrorData* exact_error = nullptr;
    bool cpp_error = false;
    bool unknown_cpp_error = false;
    char detail[512] = "std::exception without detail";
    try {
        pgseq_t::dispatch_instance(
            factory.stored_type.alphabet_code,
            factory.stored_type.storage_unit_byte_width,
            [entry, payload_context, locator = factory.locator,
             &dispatched]<class Instance>() {
                using scanner_type = sequence_scanner_pg<Instance>;
                void* storage = bioseq::pg_utils::invoke_pg_as_cpp(
                    [payload_context] {
                        return MemoryContextAllocZero(
                            payload_context, sizeof(scanner_type));
                    });
                entry->concrete_scanner = storage;
                entry->read_ranks = &typed_scanner_read_ranks<Instance>;
                entry->finish = &finish_typed_scanner_entry<Instance>;
                entry->abort_noexcept =
                    &abort_typed_scanner_entry_noexcept<Instance>;
                retain_domain(entry->domain);
                entry->domain_retained = true;
                if (!domain_is_readable(*entry->domain)) {
                    bioseq::pg_utils::invoke_pg_as_cpp([] {
                        ereport(ERROR,
                            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                             errmsg("bioseq source domain became invalid while creating a scanner")));
                    });
                }
                auto* registry = entry->domain->registry;
                ::new (storage) scanner_type(
                    registry->database_oid,
                    registry->tablespace_oid,
                    registry->main_relfilenode,
                    entry->domain->column_attno,
                    locator,
                    locator_padding_letters<Instance>(locator),
                    entry->domain->registered_snapshot,
                    entry->domain->read_caches,
                    payload_context);
                entry->constructed = true;
                dispatched = true;
            });
        if (!dispatched) {
            bioseq::pg_utils::invoke_pg_as_cpp([] {
                ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("bioseq source binding uses an unsupported sequence representation")));
            });
        }
    } catch (bioseq::pg_utils::PostgresErrorAsCpp& error) {
        exact_error = error.release_error_data();
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
    if (exact_error != nullptr || cpp_error || unknown_cpp_error) {
        abort_source_scanner_entry_noexcept(entry);
    }
    if (exact_error != nullptr) {
        ReThrowError(exact_error);
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
    ++g_pg_sequence_source_debug_counters.scanner_entries_created;
#endif
    return entry;
}

[[nodiscard]] inline pg_sequence_source_read_status
read_domain_ranks_on_backend(
        pg_sequence_source_domain* domain,
        pg_sequence_read_session& session,
        const seq_locator& locator,
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
        || locator.total_bp_size
            > static_cast<std::uint64_t>(INT64_MAX)
        || source_from > locator.total_bp_size
        || output.size() > locator.total_bp_size - source_from) {
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
            ++g_pg_sequence_source_debug_counters.scanner_entry_hits;
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
     * every view shell can abort the session before rethrowing the exact error. */
    inject_pg_sequence_source_failure(
        PgSequenceSourceFailure::read_before_scanner);
#endif

    const ResourceOwner saved_owner = CurrentResourceOwner;
    ErrorData* exact_error = nullptr;
    bool cpp_error = false;
    bool unknown_cpp_error = false;
    char detail[512] = "std::exception without detail";
    try {
        CurrentResourceOwner = domain->resource_owner;
        reader->read_ranks(*reader, transform, source_from, output);
        CurrentResourceOwner = saved_owner;
    } catch (bioseq::pg_utils::PostgresErrorAsCpp& error) {
        CurrentResourceOwner = saved_owner;
        exact_error = error.release_error_data();
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
    if (exact_error != nullptr) {
        ReThrowError(exact_error);
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
        const seq_locator& locator,
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
        ++g_pg_sequence_source_debug_counters.wrong_thread_rejections;
#endif
        return pg_sequence_source_read_status::wrong_thread;
    }
    return bioseq::pg_utils::invoke_pg_as_cpp(
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
    return bioseq::pg_utils::invoke_pg_as_cpp(
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
    return bioseq::pg_utils::invoke_pg_as_cpp(
        [owning_relation, authoritative_registered_snapshot] {
            return sequence_source_detail::create_provider(
                owning_relation, authoritative_registered_snapshot,
                pg_sequence_source_provider_kind::standalone_index);
        });
}

[[nodiscard]] inline pg_sequence_source_registry_lease
acquire_pg_sequence_source_registry_lease(
        const pg_sequence_source_registry_lease& parent) {
    using namespace sequence_source_detail;
    if (!registry_lease_is_live(parent)) {
        if (parent.registry != nullptr
            && !parent.registry->backend_thread.is_current()) {
#ifdef BIOSEQ_TEST_INDEXES
            ++g_pg_sequence_source_debug_counters.wrong_thread_rejections;
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
        ++g_pg_sequence_source_debug_counters.lease_faults;
#endif
        ereport(ERROR,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
             errmsg("bioseq source registry lease count overflow")));
    }
    ++registry.active_leases;
#ifdef BIOSEQ_TEST_INDEXES
    ++g_pg_sequence_source_debug_counters.leases_acquired;
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
        ++g_pg_sequence_source_debug_counters.wrong_thread_rejections;
#endif
        return false;
    }
    const bool released = release_lease_token_noexcept(lease);
    if (!released) {
        return false;
    }
    if (registry->provider_closed && registry->active_leases == 0u) {
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_debug_counters
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
        ++g_pg_sequence_source_debug_counters.wrong_thread_rejections;
#endif
        lease = {};
        return;
    }
    (void)release_lease_token_noexcept(lease);
    if (registry->provider_closed && registry->active_leases == 0u) {
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_debug_counters
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

[[nodiscard]] inline bool add_pg_sequence_source_route(
        pg_sequence_source_route_set& routes,
        const pg_sequence_source_registry_lease& parent) {
    if (routes.count > routes.leases.size()) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq source route count is corrupt")));
    }
    if (!sequence_source_detail::registry_lease_is_live(parent)) {
        return false;
    }
    const Oid relation_oid = parent.registry->user_relid;
    for (std::uint8_t index = 0u; index < routes.count; ++index) {
        if (routes.leases[index].registry == parent.registry) {
            return true;
        }
        if (routes.relation_oids[index] == relation_oid) {
            ereport(ERROR,
                (errcode(ERRCODE_AMBIGUOUS_ALIAS),
                 errmsg("bioseq source route has two bindings for one relation"),
                 errdetail("relation %u maps to distinct source registries",
                           relation_oid)));
        }
    }
    if (routes.count >= routes.leases.size()) {
        ereport(ERROR,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
             errmsg("bioseq source route set supports at most two providers")));
    }
    auto lease = acquire_pg_sequence_source_registry_lease(parent);
    routes.relation_oids[routes.count] = relation_oid;
    routes.leases[routes.count] = lease;
    ++routes.count;
    return true;
}

[[nodiscard]] inline bool add_pg_sequence_source_route(
        pg_sequence_source_route_set& routes,
        const pg_sequence_source_provider* provider) {
    return provider != nullptr && provider->open
        && add_pg_sequence_source_route(routes, provider->lease);
}

/** Acquire an independent upper-owner copy of every opaque child route. */
[[nodiscard]] inline bool merge_pg_sequence_source_routes(
        pg_sequence_source_route_set& destination,
        const pg_sequence_source_route_set& source) {
    using namespace sequence_source_detail;
    if (destination.count > destination.leases.size()
        || source.count > source.leases.size()) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq source route count is corrupt")));
    }
    std::size_t distinct_new_routes = 0u;
    for (std::uint8_t source_index = 0u;
         source_index < source.count; ++source_index) {
        const auto& source_lease = source.leases[source_index];
        if (!registry_lease_is_live(source_lease)
            || source.relation_oids[source_index]
                != source_lease.registry->user_relid) {
            return false;
        }
        bool already_present = false;
        for (std::uint8_t destination_index = 0u;
             destination_index < destination.count; ++destination_index) {
            if (destination.leases[destination_index].registry
                == source_lease.registry) {
                already_present = true;
                break;
            }
            if (destination.relation_oids[destination_index]
                == source.relation_oids[source_index]) {
                ereport(ERROR,
                    (errcode(ERRCODE_AMBIGUOUS_ALIAS),
                     errmsg("bioseq source route has two bindings for one relation"),
                     errdetail("relation %u maps to distinct source registries",
                               source.relation_oids[source_index])));
            }
        }
        if (!already_present) {
            for (std::uint8_t prior = 0u; prior < source_index; ++prior) {
                if (source.relation_oids[prior]
                        == source.relation_oids[source_index]
                    && source.leases[prior].registry
                        != source_lease.registry) {
                    ereport(ERROR,
                        (errcode(ERRCODE_AMBIGUOUS_ALIAS),
                         errmsg("bioseq source route has two bindings for one relation"),
                         errdetail("relation %u maps to distinct source registries",
                                   source.relation_oids[source_index])));
                }
                if (source.leases[prior].registry
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
    if (static_cast<std::size_t>(destination.count)
            + distinct_new_routes > destination.leases.size()) {
        ereport(ERROR,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
             errmsg("bioseq source route set supports at most two providers")));
    }
    for (std::uint8_t source_index = 0u;
         source_index < source.count; ++source_index) {
        if (!add_pg_sequence_source_route(
                destination, source.leases[source_index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline const pg_sequence_source_registry_lease*
find_pg_sequence_source_route(
        const pg_sequence_source_route_set& routes,
        Oid relation_oid) noexcept {
    if (routes.count > routes.leases.size()) {
        return nullptr;
    }
    for (std::uint8_t index = 0u; index < routes.count; ++index) {
        if (routes.relation_oids[index] == relation_oid
            && sequence_source_detail::registry_lease_is_live(
                routes.leases[index])) {
            return &routes.leases[index];
        }
    }
    return nullptr;
}

inline void release_pg_sequence_source_routes(
        pg_sequence_source_route_set& routes,
        bool is_commit) {
    if (routes.count > routes.leases.size()) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq source route count is corrupt")));
    }
    for (std::uint8_t index = 0u; index < routes.count; ++index) {
        if (!release_pg_sequence_source_registry_lease(
                routes.leases[index],
                pg_sequence_source_sweep_boundary::upper_lease_end,
                is_commit)) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq source route lease accounting is corrupt")));
        }
        routes.relation_oids[index] = InvalidOid;
    }
    routes.count = 0u;
}

inline void release_pg_sequence_source_routes_noexcept(
        pg_sequence_source_route_set& routes) noexcept {
    const std::uint8_t safe_count = static_cast<std::uint8_t>(
        std::min<std::size_t>(routes.count, routes.leases.size()));
    for (std::uint8_t index = 0u; index < safe_count; ++index) {
        release_pg_sequence_source_registry_lease_noexcept(
            routes.leases[index],
            pg_sequence_source_sweep_boundary::upper_lease_end);
        routes.relation_oids[index] = InvalidOid;
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
    auto* header = bioseq::pg_utils::invoke_pg_as_cpp([locator_datum] {
        return reinterpret_cast<pgseq_t*>(
            PG_DETOAST_DATUM(locator_datum));
    });
    const pg_sequence_descriptor descriptor =
        classify_and_describe_sequence(PointerGetDatum(header));
    if (descriptor.kind != pg_sequence_datum_kind::locator) {
        ereport(ERROR,
            (errcode(ERRCODE_DATATYPE_MISMATCH),
             errmsg("bioseq source promotion requires a locator stub")));
    }
    const seq_locator locator = decode_locator_from_stub(header);
    if (locator.total_bp_size
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
    const auto source_row = bioseq::pg_utils::invoke_pg_as_cpp(
        [source_epoch = locator.source_epoch,
         authoritative_snapshot = registry.authoritative_snapshot] {
            return SequenceSourceVersionManager::lookup(
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
             errdetail("relation, column, snapshot, storage identity, source epoch, locator, or type metadata changed")));
    }

    auto* domain = find_or_create_domain(
        registry, column_attno, stored_type);
    const pg_sequence_source_handle handle{
        .state = domain,
        .ops = &source_backend_ops(),
    };
    return bioseq::pg_utils::invoke_pg_as_cpp(
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
    return bioseq::pg_utils::invoke_pg_as_cpp(
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
    return bioseq::pg_utils::invoke_pg_as_cpp(
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
        AttrNumber column_attno,
        Datum locator_datum,
        MemoryContext result_context,
        slice_trace_attachment* trace = nullptr,
        std::uint32_t producer_value_id = 0u) {
    return bioseq::pg_utils::invoke_pg_as_cpp(
        [&routes, relation_oid, column_attno, locator_datum,
         result_context, trace, producer_value_id] {
            const auto* lease = find_pg_sequence_source_route(
                routes, relation_oid);
            if (lease == nullptr) {
                ereport(ERROR,
                    (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                     errmsg("bioseq locator has no matching source-provider route")));
            }
            return promote_pg_sequence_locator(
                *lease, column_attno, locator_datum, result_context,
                trace, producer_value_id);
        });
}

#ifdef BIOSEQ_TEST_INDEXES
/**
 * Prove on a real TBB arena worker that the concrete source callback rejects
 * before any PostgreSQL API, cache, snapshot, or logging side effect.
 */
[[nodiscard]] inline bool pg_sequence_source_wrong_thread_probe() {
    sequence_source_detail::pg_sequence_source_domain domain{};
    domain.backend_thread = pg_sequence_backend_thread_token::capture();
    pg_sequence_read_session session{};
    session.backend_thread = pg_sequence_backend_thread_token::capture();
    struct probe_argument {
        sequence_source_detail::pg_sequence_source_domain* domain;
        pg_sequence_read_session* session;
    } argument{&domain, &session};
    const bool rejected =
        bioseq::parallelism::GlobalPool::instance().invoke_worker_probe_noexcept(
        &argument, +[](void* opaque) noexcept {
            auto* probe = static_cast<probe_argument*>(opaque);
            const seq_locator locator{};
            const compact_sequence_stdc_type_params stored_type{};
            return sequence_source_detail::read_domain_ranks(
                probe->domain, *probe->session, locator, stored_type,
                sequence_transform::identity, 0u,
                std::span<std::uint8_t>{})
                == pg_sequence_source_read_status::wrong_thread;
        });
    if (rejected) {
        /* Worker diagnostics are thread-local; mirror the attested rejection
         * once on the backend so SQL can assert it without reading worker TLS. */
        ++g_pg_sequence_source_debug_counters.wrong_thread_rejections;
    }
    return rejected;
}

/** Exercise the fail-closed retain/lease corruption branches without PG. */
[[nodiscard]] inline bool pg_sequence_source_fault_contract_probe() noexcept {
    sequence_source_detail::pg_sequence_source_domain domain{};
    domain.lifecycle = sequence_source_detail::domain_lifecycle::live;
    domain.refcount = std::numeric_limits<std::uint32_t>::max();
    domain.registry_token_owned = true;
    sequence_source_detail::retain_domain(&domain);

    pg_sequence_source_registry registry{};
    registry.backend_thread = pg_sequence_backend_thread_token::capture();
    registry.generation = 1u;
    registry.active_leases = 0u;
    pg_sequence_source_registry_lease lease{
        .registry = &registry,
        .provider_generation = registry.generation,
        .owned = true,
    };
    const bool lease_rejected =
        !sequence_source_detail::release_lease_token_noexcept(lease);
    return domain.refcount_fault
        && domain.lifecycle == sequence_source_detail::domain_lifecycle::faulted
        && lease_rejected && registry.lease_fault && !lease.owned;
}

/** Install one inert-but-live token and drive the real PRE_PREPARE switch. */
inline void pg_sequence_source_pre_prepare_probe() {
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
        ++g_pg_sequence_source_debug_counters.registries_created;
        ++g_pg_sequence_source_debug_counters.domains_created;
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
         errmsg("bioseq PRE_PREPARE source-domain probe did not reject")));
}
#endif

}  // namespace bioseq::storage
