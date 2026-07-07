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
 * `read_fragment(n)` API. Eviction (`release_fragment`) `pfree`s the slot.
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
import bioseq.sequences;           // compact_sequence_range, compact_sequence_stdc_instance
import bioseq.types.pg_compact_sequence;
import bioseq.table.io;            // BioseqColumnBeginRead/BioseqColumnReadRowByRowNumber

static_assert(__cplusplus >= 202302L, "DOC0264 requires C++23-or-later");

export namespace bioseq::storage {

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

    [[gnu::always_inline]]
    void release_fragment(std::size_t fragment_no) const noexcept;

    // PG-backed constructor (production target path).
    explicit sequence_scanner_pg(
        Oid db_oid, Oid tablespace_oid, Oid main_relfilenode,
        std::int16_t column_attno, const seq_locator& locator,
        std::size_t last_fragment_padding,
        seq_caches* caches, MemoryContext parent_mctx)
        : db_oid_(db_oid),
          tablespace_oid_(tablespace_oid),
          main_relfilenode_(main_relfilenode),
          column_attno_(column_attno),
          locator_(locator),
          total_bp_size_(locator.total_bp_size),
          last_fragment_padding_(last_fragment_padding),
          num_fragments_(locator.num_fragments),
          caches_(caches),
          mctx_(nullptr),
          in_memory_data_{}
    {
        if (caches_ == nullptr) {
            bioseq_log<ERR, PG>( "sequence_scanner_pg: caches pointer must be non-null");
        }
        if (caches_->side != seq_caches::Side::Read) {
            bioseq_log<ERR, PG>( "sequence_scanner_pg: requires read-side seq_caches");
        }
        if (parent_mctx == nullptr) {
            bioseq_log<ERR, PG>( "sequence_scanner_pg: parent_mctx must be non-null");
        }
        mctx_ = AllocSetContextCreate(parent_mctx,
                                      "bioseq sequence_scanner_pg",
                                      ALLOCSET_DEFAULT_SIZES);
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
          caches_(other.caches_),
          mctx_(other.mctx_),
          in_memory_data_(other.in_memory_data_),
          slots_(other.slots_)
    {
        other.mctx_           = nullptr;
        other.in_memory_data_ = base_container{};
        other.caches_         = nullptr;
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

    ~sequence_scanner_pg() {
        if (mctx_ != nullptr) {
            // DOC0264 §7.2 / consolidated-review C28a — switch off mctx_ if the
            // current context is inside its subtree before deleting it.
            MemoryContext active = CurrentMemoryContext;
            MemoryContext walker = active;
            bool inside = false;
            while (walker != nullptr) {
                if (walker == mctx_) {
                    inside = true;
                    break;
                }
                walker = MemoryContextGetParent(walker);
            }
            if (inside) {
                MemoryContextSwitchTo(MemoryContextGetParent(mctx_));
            }
            MemoryContextDelete(mctx_);
            mctx_ = nullptr;
        }
    }

    [[nodiscard]] [[gnu::always_inline]]
    bool is_in_memory_mode() const noexcept { return mctx_ == nullptr; }

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
    seq_caches*                                          caches_;
    MemoryContext                                        mctx_;
    // DOC0274 — in-memory mode backing span. Empty in PG-backed mode.
    base_container                                       in_memory_data_;
    // Fixed-size slot ring matching the wrapper's
    // fragments_buffer_capacity (16). LRU-ish eviction in
    // allocate_slot_for_; the Phase-B wrapper typically drives
    // release_fragment so the ring is rarely full. Used only in PG-backed mode.
    mutable std::array<slot_t, 16>                       slots_{};

    [[nodiscard]] slot_t* find_slot_(std::size_t fragment_no) const noexcept {
        for (auto& slot : slots_) {
            if (slot.fragment_no == fragment_no && slot.bytes != nullptr) {
                return &slot;
            }
        }
        return nullptr;
    }

    [[nodiscard]] slot_t* allocate_slot_for_(std::size_t fragment_no) const {
        for (auto& slot : slots_) {
            if (slot.bytes == nullptr) {
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
        pfree(victim->bytes);
        victim->bytes       = nullptr;
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
    if (fragment_no >= num_fragments_) {
        bioseq_log<ERR, PG>(
               "sequence_scanner_pg: read_fragment({}) >= num_fragments_({})",
               fragment_no, num_fragments_);
    }

    // DOC0274 — in-memory mode: subspan the caller-owned span, no allocation.
    if (is_in_memory_mode()) {
        const std::size_t start = fragment_no * fragment_storage_units;
        if (start >= in_memory_data_.size()) {
            bioseq_log<ERR, PG>(
                "sequence_scanner_pg in-memory: fragment {} starts at unit {} but "
                "backing span only has {} units", fragment_no, start,
                in_memory_data_.size());
        }
        const std::size_t end =
            std::min(start + fragment_storage_units, in_memory_data_.size());
        return in_memory_data_.subspan(start, end - start);
    }

    if (auto* s = find_slot_(fragment_no); s != nullptr) {
        return base_container(s->bytes, fragment_storage_units);
    }

    // DOC0276 — COLD fragment read: a cache miss that hits the columnar storage.
    // This is the per-fragment work that dominates a large scanner_pg target
    // scan (the native aligner driving it is NO_POSTGRES and cannot itself poll
    // for interrupts). Honour cancellation here so a long scan responds to
    // pg_cancel_backend, and emit a throttled progress heartbeat (count-based;
    // the fmtlog timestamp header gives the rate) so the scan is observably
    // alive. Throttling keeps this off the hot per-fragment cost when logging.
    CHECK_FOR_INTERRUPTS();
    if (enable_logging) {
        static thread_local std::uint64_t cold_read_count = 0;
        if ((cold_read_count++ % 4000) == 0) {
            bioseq_log_progress<DBG>("[scanner_pg] cold fragment read #{} (fragment {}/{})",
                cold_read_count, fragment_no, num_fragments_);
        }
    }

    // Resolve the sub-table relation + read state via Phase A's cache.
    Relation shard = open_sub_table_for_locator(
        db_oid_, tablespace_oid_, main_relfilenode_, column_attno_,
        locator_, AccessShareLock, caches_);

    const std::uint64_t cache_key = seq_caches::key_for(column_attno_,
        static_cast<std::int16_t>(locator_.bucket_idx),
        static_cast<std::int32_t>(locator_.shard_idx));
    seq_caches::entry& entry = caches_->entries[cache_key];
    entry.rel = shard;

    if (entry.read_state == nullptr) {
        MemoryContext payload_mctx = caches_->read_state_mctx != nullptr
            ? caches_->read_state_mctx
            : CurrentMemoryContext;
        MemoryContextScope ctx{payload_mctx};
        TupleDesc td = RelationGetDescr(shard);
        PBitmapset attr_needed = PBitmapset::full_range(td->natts);
        PIntList projected;
        for (int i = 0; i < td->natts; ++i) {
            if (!TupleDescAttr(td, i)->attisdropped && attr_needed.contains(i)) {
                projected.append(static_cast<AttrNumber>(i + 1));
            }
        }
        Snapshot snap = GetTransactionSnapshot();
        entry.read_state = BioseqColumnBeginRead(shard, td, projected.release(),
                                             /*whereClauseList=*/NIL,
                                             payload_mctx, snap,
                                             /*randomAccess=*/true, nullptr);
        BioseqColumnReadFlushPendingWrites(entry.read_state);
        attr_needed.free();
    }

    const std::uint64_t row_no = locator_.first_sub_row_no + fragment_no;
    Datum vals[1]  = { 0 };
    bool  nulls[1] = { true };
    if (!BioseqColumnReadRowByRowNumber(entry.read_state, row_no, vals, nulls)) {
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
    }
    if (nulls[0]) {
        bioseq_log<ERR, PG>(
            "sequence_scanner_pg: sub-table row {} is NULL (fragment {} of {})",
            row_no, fragment_no, num_fragments_);
    }
    pgseq_t* raw = reinterpret_cast<pgseq_t*>(PG_DETOAST_DATUM(vals[0]));
    if (is_locator_stub(raw)) {
        bioseq_log<ERR, PG>(
            "sequence_scanner_pg: sub-table fragment row {} is itself a stub "
            "(corruption)", row_no);
    }

    constexpr std::size_t bytes_to_copy =
        fragment_storage_units * sizeof(storage_unit);
    storage_unit* dst = nullptr;
    {
        MemoryContextScope ctx{mctx_};
        // palloc0 so the trailing-bytes-of-last-fragment region the wrapper
        // sees is uniformly zero (DOC0264 §7.3 / consolidated-review N20).
        dst = static_cast<storage_unit*>(palloc0(bytes_to_copy));
    }
    const std::size_t raw_payload_bytes =
        static_cast<std::size_t>(VARSIZE(raw)) - pgseq_t::header_bytes();
    const std::size_t live_bytes = std::min(raw_payload_bytes, bytes_to_copy);
    std::memcpy(dst, raw->data, live_bytes);

    slot_t* slot = allocate_slot_for_(fragment_no);
    slot->fragment_no = fragment_no;
    slot->bytes       = dst;
    return base_container(dst, fragment_storage_units);
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
            pfree(slot.bytes);
            slot.bytes       = nullptr;
            slot.fragment_no = std::numeric_limits<std::size_t>::max();
            return;
        }
    }
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
    seq_caches* caches, MemoryContext parent_mctx, Fn&& fn)
{
    using S = sequence_scanner_pg<R>;
    S scanner(db_oid, tablespace_oid, main_relfilenode, column_attno,
              locator, last_fragment_padding, caches, parent_mctx);
    lazy_compact_sequence<R, S> lazy{std::move(scanner)};
    std::forward<Fn>(fn)(lazy);
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
