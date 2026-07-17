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
#include "access/relation.h"
#include "access/table.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
}

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <utility>

export module bioseq.storage.scanner_pg;

import bioseq.common.mlogging;
import bioseq.pg_utils;
import bioseq.common.sequan_compat;              // seq::dna4 in static_assert below
import bioseq.storage.manager;             // DOC0264 §7.9 — seq_caches + open_sub_table_for_locator
import bioseq.storage.scanner;     // DOC0263 §3.4 — is_valid/active_sequence_scanner concept
import bioseq.storage.lazy;        // DOC0264 §9.1 — lazy_compact_sequence + with_lazy_postgres
import bioseq.storage.size;        // sequence_size_t
import bioseq.storage.metadata;    // StripeMetadata/FindStripeByRowNumber
import bioseq.sequences;           // compact_sequence_range, compact_sequence_stdc_instance
import bioseq.types.pg_compact_sequence;
import bioseq.table.io;            // BioseqColumnBeginRead/BioseqColumnReadRowByRowNumber

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
        mctx_ = bioseq::pg_utils::invoke_pg_as_cpp([&] {
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
    pgseq_t* raw = bioseq::pg_utils::invoke_pg_as_cpp([&] {
        return reinterpret_cast<pgseq_t*>(PG_DETOAST_DATUM(vals[0]));
    });
    if (is_locator_stub(raw)) {
        bioseq::pg_utils::invoke_pg_as_cpp([&] {
            bioseq_log<ERR, PG>(
                "sequence_scanner_pg: sub-table fragment row {} is itself a stub "
                "(corruption)", row_no);
        });
    }

    constexpr std::size_t bytes_to_copy =
        fragment_storage_units * sizeof(storage_unit);
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
    const std::size_t raw_payload_bytes =
        static_cast<std::size_t>(VARSIZE(raw)) - pgseq_t::header_bytes();
    const std::size_t live_bytes = std::min(raw_payload_bytes, bytes_to_copy);
    std::memcpy(dst, raw->data, live_bytes);

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

// DOC0264 §9.1 — `with_lazy_postgres<R>(...)` — the production callback
// dispatcher. Constructs a `sequence_scanner_pg<R>` and a
// `lazy_compact_sequence<R, sequence_scanner_pg<R>>` IN PLACE on the stack
// and invokes `fn(lazy)`. Mirrors Phase B's `with_lazy_in_memory` /
// `with_lazy_fragmented` factories.
template <class R, class Fn>
inline void with_lazy_postgres(
    Oid db_oid, Oid tablespace_oid, Oid main_relfilenode,
    std::int16_t column_attno,
    const seq_locator& locator,
    std::size_t last_fragment_padding,
    Snapshot registered_snapshot,
    seq_caches* caches, MemoryContext parent_mctx, Fn&& fn)
{
    using S = sequence_scanner_pg<R>;
    S scanner(db_oid, tablespace_oid, main_relfilenode, column_attno,
              locator, last_fragment_padding, registered_snapshot,
              caches, parent_mctx);
    lazy_compact_sequence<R, S> lazy{std::move(scanner)};
    try {
        std::forward<Fn>(fn)(lazy);
        lazy.scanner().finish_pg_resources_as_cpp();
    } catch (...) {
        // Never enter PostgreSQL while unwinding either a bridged ERROR or an
        // algorithm exception.  The parent MemoryContext remains authoritative.
        lazy.scanner().abandon_pg_resources_noexcept();
        throw;
    }
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

}  // namespace bioseq::storage
