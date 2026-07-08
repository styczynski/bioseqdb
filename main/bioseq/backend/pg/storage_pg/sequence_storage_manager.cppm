/*-------------------------------------------------------------------------
 *
 * storage/sequence_storage_manager.cppm
 *
 * Phase A (DOC0260) length-bucketed sub-table sharding for sequence
 * storage. Owns:
 *   - the locator stub format (a fixed-size pgseq_t-shaped varlena that
 *     the main columnar relation stores in place of the full sequence);
 *   - the consteval bucket-threshold builder and the per-call
 *     bucket_index_for_size LUT;
 *   - the per-scan / per-batch seq_caches struct that consumers thread
 *     through every hot-path entry point (no thread-local state);
 *   - the stateless sequence_storage_manager<len_t, min, max> template
 *     that drives shard create/open and the write/read hot paths;
 *   - the bioseq_default_storage_manager =
 *     sequence_storage_manager<sequence_size_t, 8, 42> alias used by the
 *     TAM and dispatch buffer.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

extern "C" {
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/genam.h"
#include "executor/spi.h"
#include "storage/lmgr.h"
#include "utils/snapmgr.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "catalog/dependency.h"
#include "catalog/namespace.h"
#include "catalog/indexing.h"
#include "miscadmin.h"
#include "fmgr.h"
}

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.storage.manager:sequence_storage_manager;

import bioseq.common.mlogging;
import bioseq.pg_utils;
import bioseq.config;                  // DOC0322 §9.3 — bioseq_storage_max_seq_sub_table_gbs GUC
import bioseq.storage.metadata;
import bioseq.table.io;
import bioseq.types.pg_compact_sequence;
import bioseq.sequences;
import bioseq.common.sequan_compat;  // DOC0264 §8.1 — ceil_log2 used in letters_per_unit_for
import bioseq.storage.size;  // DOC0263 §2.2 — standalone module (was :sequence_size)
// DOC0260 §12.1 — the PG-free bucket math + seq_locator POD live in their own
// module so the unit-test TU can link them without the Postgres backend.
// Re-exported here so existing `import bioseq_storage` consumers are unchanged.
export import bioseq.storage.bucket_math;

static_assert(__cplusplus >= 202302L,
              "DOC0260 requires C++23-or-later (project default: C++26)");

export namespace bioseq::storage {

// ---------------------------------------------------------------------------
// DOC0264 §5 — Phase C locator stub format. A 48-byte pgseq_t-shaped varlena
// that lives in the main columnar relation in place of the full sequence
// payload. Replaces the Phase A 40-byte v1 stub (no on-disk back-compat —
// DOC0264 R0 / §1.1#10).
// ---------------------------------------------------------------------------

constexpr std::uint8_t kLocatorStubMarker = 0x80;

// DOC0260 §12.1 + DOC0264 §5 — `kLocatorVersion` and the `seq_locator` POD
// (with its 32-byte / offset static_asserts) live in the PG-free
// `bioseq_bucket_math` module so the unit-test TU can link them; they are
// re-exported above via `export import bioseq_bucket_math`, so every use of
// `seq_locator` / `kLocatorVersion` below resolves unchanged.

constexpr std::size_t kLocatorStubVarlenaBytes =
    pgseq_t::header_bytes() + sizeof(seq_locator);
static_assert(kLocatorStubVarlenaBytes == 48,
              "DOC0264 §5: Phase C locator stub must be exactly 48 bytes.");

[[nodiscard]] [[gnu::always_inline]]
inline bool is_locator_stub(const pgseq_t* hdr) noexcept {
    return hdr != nullptr
        && (static_cast<std::uint8_t>(hdr->storage_unit_byte_width)
            & kLocatorStubMarker) != 0u;
}

[[nodiscard]] [[gnu::always_inline]]
inline std::uint8_t logical_storage_unit_byte_width(const pgseq_t* hdr) noexcept {
    return static_cast<std::uint8_t>(
        static_cast<std::uint8_t>(hdr->storage_unit_byte_width) & 0x0Fu);
}

[[nodiscard]] [[gnu::always_inline]]
inline seq_locator decode_locator_from_stub(const pgseq_t* hdr) {
    if (!is_locator_stub(hdr)) {
        bioseq_log<ERR, PG>(
               "bioseq storage: decode_locator_from_stub called on a "
               "non-stub value (sub_w={}). Caller must guard with "
               "is_locator_stub(hdr) first.",
               static_cast<unsigned>(hdr->storage_unit_byte_width));
    }
    seq_locator out{};
    std::memcpy(&out, hdr->data, sizeof(out));
    if (out.version != kLocatorVersion) {
        bioseq_log<ERR, PG>(
               "bioseq storage: locator version mismatch (got {}, expected {})",
               static_cast<unsigned>(out.version),
               static_cast<unsigned>(kLocatorVersion));
    }
    return out;
}

[[gnu::always_inline]]
inline void encode_locator_into_stub(
    pgseq_t* dst,
    std::uint8_t logical_alphabet_code,
    std::uint8_t logical_byte_width,
    std::uint16_t padding_letters,
    sequence_size_t total_bp_size,
    const seq_locator& loc) noexcept
{
    std::memset(dst, 0, kLocatorStubVarlenaBytes);
    SET_VARSIZE(dst, kLocatorStubVarlenaBytes);
    dst->alphabet_code = static_cast<pgseq_t::alphabet_code_t>(logical_alphabet_code);
    dst->storage_unit_byte_width = static_cast<std::uint8_t>(
        kLocatorStubMarker | (logical_byte_width & 0x0Fu));
    dst->padding_letters = padding_letters;
    constexpr sequence_size_t kBpSizeFieldMax =
        static_cast<sequence_size_t>(std::numeric_limits<pgseq_t::bp_size_t>::max());
    dst->bp_size = static_cast<pgseq_t::bp_size_t>(
        std::min<sequence_size_t>(total_bp_size, kBpSizeFieldMax));
    std::memcpy(dst->data, &loc, sizeof(loc));
}

[[nodiscard]] [[gnu::always_inline]]
inline pgseq_t* alloc_locator_stub_in(
    MemoryContext mctx,
    std::uint8_t logical_alphabet_code,
    std::uint8_t logical_byte_width,
    std::uint16_t padding_letters,
    sequence_size_t total_bp_size,
    const seq_locator& loc)
{
    MemoryContextScope ctx_guard{mctx};
    pgseq_t* dst = static_cast<pgseq_t*>(palloc0(kLocatorStubVarlenaBytes));
    encode_locator_into_stub(dst, logical_alphabet_code, logical_byte_width,
                             padding_letters, total_bp_size, loc);
    return dst;
}

// ---------------------------------------------------------------------------
// §4 — Bucket math. The consteval threshold builder, len_log2_for_size, the
// branchless L->bucket LUT and bucket_index_for_size now live in the PG-free
// `bioseq_bucket_math` module (DOC0260 §12.1) and are re-exported above via
// `export import bioseq_bucket_math`. The template below delegates to them.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// §7 — Per-scan / per-batch relation cache. Owned by BioseqColumnScanDescData,
// IndexFetchBioseqColumnData, BioseqColumnWriteState and TupleDispatchBuffer.
// Always allocated separately (`palloc(sizeof(seq_caches)) + placement-new`)
// because std::map's non-standard-layout would otherwise spoil the surrounding
// POD struct's std::is_standard_layout_v guarantee.
// ---------------------------------------------------------------------------

struct seq_caches {
    struct entry {
        Relation rel = nullptr;
        BioseqColumnWriteState* write_state = nullptr;
        BioseqColumnReadState*  read_state  = nullptr;
        sequence_size_t pending_bytes_delta = 0;
        sequence_size_t pending_total_payload_delta = 0;
    };

    struct entry_map {
        using value_type = std::pair<std::uint64_t, entry>;
        using storage_type = std::vector<value_type>;
        using iterator = storage_type::iterator;
        using const_iterator = storage_type::const_iterator;

        [[nodiscard]] iterator begin() noexcept { return entries_.begin(); }
        [[nodiscard]] iterator end() noexcept { return entries_.end(); }
        [[nodiscard]] const_iterator begin() const noexcept { return entries_.begin(); }
        [[nodiscard]] const_iterator end() const noexcept { return entries_.end(); }
        [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
        void clear() noexcept { entries_.clear(); }

        [[nodiscard]] iterator find(std::uint64_t key) noexcept {
            auto it = lower_bound(key);
            return it != entries_.end() && it->first == key ? it : entries_.end();
        }

        [[nodiscard]] const_iterator find(std::uint64_t key) const noexcept {
            auto it = lower_bound(key);
            return it != entries_.end() && it->first == key ? it : entries_.end();
        }

        entry& operator[](std::uint64_t key) {
            auto it = lower_bound(key);
            if (it == entries_.end() || it->first != key) {
                it = entries_.emplace(it, key, entry{});
            }
            return it->second;
        }

    private:
        struct less_by_key {
            [[nodiscard]] constexpr bool operator()(const value_type& lhs,
                                                   std::uint64_t rhs) const noexcept {
                return lhs.first < rhs;
            }
        };

        [[nodiscard]] iterator lower_bound(std::uint64_t key) noexcept {
            return std::lower_bound(
                entries_.begin(), entries_.end(), key, less_by_key{});
        }

        [[nodiscard]] const_iterator lower_bound(std::uint64_t key) const noexcept {
            return std::lower_bound(
                entries_.begin(), entries_.end(), key, less_by_key{});
        }

        storage_type entries_{};
    };

    // DOC0260 R14 — sorted-vector map rather than std::map: no per-node
    // allocation, deterministic sorted iteration order for close_all, and
    // cache-friendly for the typical <50-entry working set. The hot paths only
    // do find / operator[] / clear, and never hold an `entry&` across an insert.
    entry_map entries{};
    LOCKMODE lock_mode = AccessShareLock;

    enum class Side { Read, Write };
    Side side = Side::Read;

    MemoryContext read_state_mctx = nullptr;

    sequence_size_t cap_bytes_snapshot = std::numeric_limits<sequence_size_t>::max();

    Oid main_database_oid   = InvalidOid;
    Oid main_tablespace_oid = InvalidOid;
    Oid main_relfilenode    = InvalidOid;

    [[nodiscard]] [[gnu::always_inline]]
    static constexpr std::uint64_t key_for(std::int16_t column_attno,
                                           std::int16_t bucket_idx,
                                           std::int32_t shard_idx) noexcept {
        return (static_cast<std::uint64_t>(
                    static_cast<std::uint16_t>(column_attno)) << 48)
             | (static_cast<std::uint64_t>(
                    static_cast<std::uint16_t>(bucket_idx))   << 32)
             |  static_cast<std::uint64_t>(
                    static_cast<std::uint32_t>(shard_idx));
    }

    void close_all(bool commit) noexcept;
};

// alloc_seq_caches / destroy_seq_caches own the placement-new dance so the
// dependent consumers (BioseqColumnScanDescData, BioseqColumnWriteState, ...)
// never see a half-constructed map. The header type stays
// reinterpret_cast-safe via void* opaque pointers in the consumer structs.
//
// destroy_seq_caches is NOT inline because write_state_management.cppm
// forward-declares it to avoid `import bioseq_storage` (which would
// create a module cycle with bioseq_table). Out-of-line gives it external
// linkage so the linker resolves the forward declaration.
[[nodiscard]] inline seq_caches* alloc_seq_caches(MemoryContext mctx) {
    MemoryContextScope ctx_guard{mctx};
    void* raw = palloc0(sizeof(seq_caches));
    return ::new (raw) seq_caches{};
}

void destroy_seq_caches(seq_caches* caches) noexcept;

[[gnu::always_inline]] inline void close_seq_caches(seq_caches& caches,
                                                    bool commit) noexcept {
    caches.close_all(commit);
}

void close_and_destroy_seq_caches(seq_caches* caches, bool commit) noexcept;

// DOC0286 §4.3 — public alias for the per-fragment storage-unit count
// (detail::kFragmentStorageUnits below aliases this so the two never drift).
// `bioseq.load`'s streaming sink uses it to size fragment payloads.
inline constexpr std::size_t kSequenceFragmentStorageUnits = std::size_t{1024};

// DOC0286 §4.4 — flush every open sub-table write state owned by `caches` so an
// in-statement index scan (e.g. the batched update_index `bioseq.load` runs
// after streaming) sees the just-written fragment rows. Unlike `close_all`,
// this only flushes pending columnar writes (it does NOT pop/close the write
// state); the central xact-commit teardown still owns lifecycle.
void flush_seq_cache_write_states(seq_caches* caches,
                                  SubTransactionId current_sub_xid) noexcept;

/// DOC0286 §4.3 — bounded streaming fragment writer for one sequence. Accepts
/// column-native packed bytes, writes full fragment rows (each
/// `kSequenceFragmentStorageUnits` storage units) as its internal buffer fills,
/// retains only the final partial fragment until `finish`, and returns the
/// Phase-C `seq_locator`. Peak extra memory is one ≤8 KiB fragment buffer + one
/// `pgseq_t` fragment row, so a >MaxAllocSize record never materialises as one
/// buffer. Writes to ONE shard; a single sequence exceeding the sub-table cap
/// raises ERROR (transaction rollback cleans the partial rows).
struct sequence_stream_writer {
    void begin(Oid db_oid, Oid spc_oid, RelFileNumber main_relfilenode,
               AttrNumber seq_attno, TupleDesc parent_td,
               std::uint8_t alphabet_code, std::uint8_t byte_width,
               sequence_size_t cap_bytes, seq_caches* caches);
    void append_packed_bytes(std::span<const std::uint8_t> bytes,
                             sequence_size_t bp_count);
    [[nodiscard]] seq_locator finish(sequence_size_t total_bp,
                                     std::uint16_t last_pad);
    void abort();

private:
    void open_shard();
    void write_fragment(const std::uint8_t* payload, std::size_t payload_bytes,
                        sequence_size_t bp, std::uint16_t pad);

    Oid                 db_oid_ = InvalidOid;
    Oid                 spc_oid_ = InvalidOid;
    RelFileNumber       main_relfilenode_{};
    std::int16_t        column_attno_ = 0;
    TupleDesc           parent_td_ = nullptr;
    std::uint8_t        alphabet_code_ = 0;
    std::uint8_t        byte_width_ = 0;
    sequence_size_t     cap_bytes_ = 0;
    seq_caches*         caches_ = nullptr;

    std::int16_t        bucket_idx_ = 0;
    std::int32_t        shard_idx_ = 0;
    std::uint64_t       cache_key_ = 0;
    Relation            shard_ = nullptr;
    BioseqColumnWriteState* write_state_ = nullptr;

    std::size_t         frag_payload_bytes_ = 0;  // kSequenceFragmentStorageUnits * byte_width
    std::size_t         frag_varlena_bytes_ = 0;  // header + payload
    sequence_size_t     frag_bp_ = 0;             // kSequenceFragmentStorageUnits * letters_per_unit
    std::vector<std::uint8_t> buffer_;            // < frag_payload_bytes_ leftover
    std::uint64_t       first_sub_row_no_ = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t       last_sub_row_no_  = std::numeric_limits<std::uint64_t>::max();
    std::uint32_t       num_fragments_ = 0;
    sequence_size_t     total_varlena_bytes_ = 0;
    bool                active_ = false;
};

}  // namespace bioseq::storage

// DOC0269 — Phase A required exporting this block so importers (notably
// access_method.cppm) can name `sequence_storage_manager`,
// `bioseq_default_storage_manager`, the §7.10 sidecar I/O helpers, and
// `read_max_sub_table_cap_bytes`. Without the `export` keyword on this
// re-opening of `namespace bioseq::storage`, the declarations land in the
// module purview but are not exported, so the names are present in the
// translation unit yet refused at import (clang's "declaration must be
// imported from module 'bioseq_storage' before it is required").
export namespace bioseq::storage {

// ---------------------------------------------------------------------------
// §6 — Catalog access shims. These call into storage_metadata which owns the
// bioseq_internal.seq_sub_table catalog table.
// ---------------------------------------------------------------------------

struct SeqColumnTypmod {
    std::string alphabet_name;
    int storage_unit_bit_width = 0;
};

[[nodiscard]] SeqColumnTypmod
column_typmod_at(TupleDesc parent_tupdesc, std::int16_t column_attno);

[[nodiscard]] const char*
persistence_keyword_for_relfilenode(Oid main_database_oid,
                                    Oid main_tablespace_oid,
                                    Oid main_relfilenode);

[[nodiscard]] std::string
format_sub_table_name(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                      std::int16_t column_attno, std::int16_t bucket_idx,
                      std::int32_t shard_idx);

[[nodiscard]] std::string
format_sidecar_name(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                    std::int16_t column_attno, std::int16_t bucket_idx,
                    std::int32_t shard_idx);

// ---------------------------------------------------------------------------
// §7.1 — sequence_storage_manager template.
// ---------------------------------------------------------------------------

template<class p_len_t,
         std::size_t p_bucket_min_len_bits,
         std::size_t p_bucket_max_len_bits>
struct sequence_storage_manager {
    using len_t = p_len_t;
    static constexpr std::size_t bucket_min_len_bits = p_bucket_min_len_bits;
    static constexpr std::size_t bucket_max_len_bits = p_bucket_max_len_bits;
    static constexpr std::size_t total_len_bits      = sizeof(len_t) * 8u;
    static constexpr auto table_seq_buckets_len_thresholds =
        generate_seq_buckets_len_thresholds<
            len_t, bucket_min_len_bits, bucket_max_len_bits>();
    static constexpr std::size_t bucket_count =
        table_seq_buckets_len_thresholds.size();

    static_assert(bucket_count <= std::numeric_limits<std::uint8_t>::max(),
                  "seq_locator::bucket_idx is uint8_t; raise the locator "
                  "format version before configuring more than 255 buckets");

    // DOC0260 §12.1 — delegate to the PG-free free template so production and
    // the unit-test TU resolve to identical bucketing code (the LUT itself is
    // bucket_lut_v in bioseq_bucket_math).
    [[nodiscard]] [[gnu::always_inline]] static constexpr std::size_t
    bucket_index_for_size(sequence_size_t seq_size) noexcept {
        return bioseq::storage::bucket_index_for_size<
            len_t, bucket_min_len_bits, bucket_max_len_bits>(seq_size);
    }

    // Shard lifecycle / write path.
    //
    // DOC0264 §8 — `insert_sequence_streamed` writes one fragment per
    // sub-table row from a per-fragment producer callback. The producer
    // receives a palloc'd `pgseq_t*` whose header is pre-filled with
    // alphabet/width, and fills `data[]` + `bp_size` + `padding_letters`
    // for fragment `n` (last fragment uses `is_last_fragment=true`). The
    // per-fragment MemoryContext is reset between fragments to bound peak
    // RSS. Returns a v2 locator pointing at the first fragment row.
    //
    // `insert_payload_as_fragments` is the convenience wrapper that the
    // existing single-varlena code paths (TAM rewrite-to-stubs, CLUSTER)
    // use: it slices an already-materialised `pgseq_t*` into fragments
    // via `insert_sequence_streamed`.
    template<class FragmentProducer>
    [[nodiscard]] static seq_locator
    insert_sequence_streamed(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                             std::int16_t column_attno,
                             TupleDesc parent_tupdesc,
                             std::uint8_t logical_alphabet_code,
                             std::uint8_t logical_storage_unit_byte_width,
                             sequence_size_t total_bp_size,
                             std::uint16_t last_fragment_padding_letters,
                             sequence_size_t current_cap_bytes,
                             seq_caches* cache,
                             FragmentProducer&& producer);

    [[nodiscard]] static seq_locator
    insert_payload_as_fragments(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                                std::int16_t column_attno,
                                TupleDesc parent_tupdesc,
                                const pgseq_t* original_hdr,
                                sequence_size_t current_cap_bytes,
                                seq_caches* cache);

    // Read path: single sequence — stitches all fragments into a single
    // contiguous palloc'd `pgseq_t*` in `out_mctx`. Used by the legacy
    // hydration path; Phase C raises bioseq_log<ERR, PG>() if the stitched varlena
    // would exceed MaxAllocSize (DOC0264 §9.4). Streaming readers should
    // use `sequence_scanner_pg<S>` directly.
    [[nodiscard]] static pgseq_t*
    fetch_payload(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                  std::int16_t column_attno, const seq_locator& loc,
                  MemoryContext out_mctx, seq_caches* cache);

    // Read path: bulk per-bucket.
    struct fetch_request {
        std::size_t out_slot = 0;
        seq_locator locator{};
    };

    static void
    fetch_payload_bulk(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                       std::int16_t column_attno, std::int16_t bucket_idx,
                       std::span<const fetch_request> requests,
                       std::span<pgseq_t*>            out_payloads,
                       std::span<fetch_request>       sort_scratch,
                       MemoryContext                  out_mctx,
                       seq_caches*                    cache);

    // Cascade on DROP TABLE / TRUNCATE / CLUSTER swap.
    static void
    drop_sub_tables_for_main(Oid db_oid, Oid spc_oid, Oid main_relfilenode);
};

using bioseq_default_storage_manager =
    sequence_storage_manager<sequence_size_t, /*min=*/8, /*max=*/42>;

// DOC0264 §7.9 — exported entry point so the Phase C `sequence_scanner_pg<S>`
// (in `bioseq_storage_scanner_pg`) can resolve a sub-table relation via the
// per-scan `seq_caches`. Implementation lives below in the .cppm body.
[[nodiscard]] Relation
open_sub_table_for_locator(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                           std::int16_t column_attno, const seq_locator& loc,
                           LOCKMODE lock_mode, seq_caches* cache);

// ---------------------------------------------------------------------------
// §7.10 — Sidecar I/O. Per-shard heap relations co-sharded with sub-tables.
// ---------------------------------------------------------------------------

void
sidecar_write_blob(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                   std::int16_t column_attno, const seq_locator& loc,
                   std::span<const std::uint8_t> blob, seq_caches* cache);

[[nodiscard]] std::optional<std::vector<char>>
sidecar_read_blob(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                  std::int16_t column_attno, const seq_locator& loc,
                  seq_caches* cache);

void
sidecar_read_blob_bulk(
    Oid db_oid, Oid spc_oid, Oid main_relfilenode,
    std::int16_t column_attno, std::int16_t bucket_idx,
    std::span<const sequence_storage_manager<sequence_size_t, 8, 42>::fetch_request> requests,
    std::span<std::optional<std::vector<char>>> out_blobs,
    std::span<sequence_storage_manager<sequence_size_t, 8, 42>::fetch_request> sort_scratch,
    MemoryContext out_mctx,
    seq_caches* cache);

// ---------------------------------------------------------------------------
// §6.4 / §11.3 — Catalog reaper, cascade-drop entry point.
// ---------------------------------------------------------------------------

void ReapOrphanSeqSubTables();

// ---------------------------------------------------------------------------
// §8 — TAM hot-path helper for write-state cap-snapshot read.
// ---------------------------------------------------------------------------

[[nodiscard]] sequence_size_t read_max_sub_table_cap_bytes();

}  // namespace bioseq::storage

// ============================================================================
// Implementation
// ============================================================================

namespace bioseq::storage {

constexpr std::int64_t kSeqStorageAdvisoryLockClass = 0x42105260LL;

[[nodiscard]] static std::int64_t
shard_alloc_lock_key(Oid db_oid, Oid main_relfilenode,
                     std::int16_t column_attno, std::int16_t bucket_idx) noexcept {
    return (static_cast<std::int64_t>(db_oid)                << 48)
         ^ (static_cast<std::int64_t>(main_relfilenode)      << 16)
         ^ (static_cast<std::int64_t>(
                static_cast<std::uint16_t>(column_attno))    << 8)
         ^  static_cast<std::int64_t>(
                static_cast<std::uint16_t>(bucket_idx));
}

SeqColumnTypmod column_typmod_at(TupleDesc parent_tupdesc, std::int16_t column_attno) {
    if (column_attno <= 0 || column_attno > parent_tupdesc->natts) {
        bioseq_log<ERR, PG>( "bioseq storage: column_attno {} out of bounds (natts={})",
               column_attno, parent_tupdesc->natts);
    }
    Form_pg_attribute attr = TupleDescAttr(parent_tupdesc, column_attno - 1);
    const std::int32_t typmod = attr->atttypmod;
    if (typmod < 0) {
        bioseq_log<ERR, PG>( "bioseq storage: SEQ column attno={} has no typmod; "
               "Phase A requires every bioseq.seq column to carry an "
               "(alphabet, width) typmod",
               column_attno);
    }
    const compact_sequence_stdc_type_params params =
        compact_sequence_stdc_type_params::decode(typmod);
    std::string_view name_sv = pgseq_t::name_of_alphabet_code(params.alphabet_code);
    if (name_sv == std::string_view{"?"}) {
        bioseq_log<ERR, PG>( "bioseq storage: alphabet_code={} not in pgseq_supported_alphabets",
               static_cast<unsigned>(params.alphabet_code));
    }
    SeqColumnTypmod out;
    out.alphabet_name.assign(name_sv.data(), name_sv.size());
    out.storage_unit_bit_width =
        static_cast<int>(params.storage_unit_byte_width) * 8;
    return out;
}

const char*
persistence_keyword_for_relfilenode(Oid /*db_oid*/, Oid /*spc_oid*/,
                                    Oid main_relfilenode) {
    const Oid main_relid = RelidByRelfilenumber(InvalidOid, main_relfilenode);
    if (!OidIsValid(main_relid)) {
        return "";
    }
    Relation rel = try_safe_relation_open(main_relid, AccessShareLock);
    if (rel == nullptr) {
        return "";
    }
    const char persistence = rel->rd_rel->relpersistence;
    relation_close(rel, AccessShareLock);
    switch (persistence) {
        case RELPERSISTENCE_UNLOGGED: return "UNLOGGED";
        case RELPERSISTENCE_TEMP:     return "TEMPORARY";
        default:                      return "";
    }
}

std::string format_sub_table_name(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                                  std::int16_t column_attno, std::int16_t bucket_idx,
                                  std::int32_t shard_idx) {
    return fmt::format("seq_sub_{}_{}_{}_{}_{}_{}",
                       static_cast<unsigned>(db_oid),
                       static_cast<unsigned>(spc_oid),
                       static_cast<unsigned>(main_relfilenode),
                       static_cast<int>(column_attno),
                       static_cast<int>(bucket_idx),
                       static_cast<int>(shard_idx));
}

std::string format_sidecar_name(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                                std::int16_t column_attno, std::int16_t bucket_idx,
                                std::int32_t shard_idx) {
    return fmt::format("idx_sidecar_{}_{}_{}_{}_{}_{}",
                       static_cast<unsigned>(db_oid),
                       static_cast<unsigned>(spc_oid),
                       static_cast<unsigned>(main_relfilenode),
                       static_cast<int>(column_attno),
                       static_cast<int>(bucket_idx),
                       static_cast<int>(shard_idx));
}

sequence_size_t read_max_sub_table_cap_bytes() {
    const int guc = std::atomic_ref<int>(bioseq_storage_max_seq_sub_table_gbs)
                        .load(std::memory_order_relaxed);
    if (guc <= 0) {
        return 0;
    }
    return static_cast<sequence_size_t>(guc) * (sequence_size_t{1} << 30);
}

}  // namespace bioseq::storage

namespace bioseq::storage {

void destroy_seq_caches(seq_caches* caches) noexcept {
    if (caches == nullptr) {
        return;
    }
    caches->~seq_caches();
    pfree(caches);
}

void close_and_destroy_seq_caches(seq_caches* caches, bool commit) noexcept {
    if (caches == nullptr) {
        return;
    }
    close_seq_caches(*caches, commit);
    destroy_seq_caches(caches);
}

void seq_caches::close_all(bool commit) noexcept {
    for (auto& kv : entries) {
        const std::uint64_t key = kv.first;
        entry& e = kv.second;
        const std::int16_t  column_attno = static_cast<std::int16_t>(key >> 48);
        const std::int16_t  bucket_idx   =
            static_cast<std::int16_t>((key >> 32) & 0xFFFFu);
        const std::int32_t  shard_idx    =
            static_cast<std::int32_t>(key & 0xFFFFFFFFu);

        if (side == Side::Write) {
            if (commit && e.pending_bytes_delta != 0) {
                SubTableManager::update_shard_bytes_written(
                    main_database_oid, main_tablespace_oid, main_relfilenode,
                    column_attno, bucket_idx, shard_idx,
                    e.pending_bytes_delta,
                    cap_bytes_snapshot);
            }
            e.pending_bytes_delta = 0;
            e.pending_total_payload_delta = 0;
            // We deliberately do NOT call BioseqColumnEndWrite here — the
            // sub-table WriteState lives in the central WriteStateCache and is
            // committed alongside the parent's WriteState at transaction
            // commit. Re-closing here would double-flush.
            e.write_state = nullptr;
        } else {
            if (e.read_state != nullptr) {
                BioseqColumnEndRead(e.read_state);
                e.read_state = nullptr;
            }
        }
        if (e.rel != nullptr) {
            relation_close(e.rel, lock_mode);
            e.rel = nullptr;
        }
    }
    entries.clear();
}

// DOC0286 §4.4 — flush (but do NOT pop/close) every open sub-table write state
// so a same-statement index scan sees the just-written fragment rows.
// `FlushWriteStateForRelfilenode` is keyed on the shard relation's relfilenode.
void flush_seq_cache_write_states(seq_caches* caches,
                                  SubTransactionId current_sub_xid) noexcept {
    if (caches == nullptr || caches->side != seq_caches::Side::Write) return;
    for (auto& kv : caches->entries) {
        seq_caches::entry& e = kv.second;
        if (e.rel != nullptr) {
            FlushWriteStateForRelfilenode(e.rel->rd_locator.relNumber,
                                          current_sub_xid);
        }
    }
}

// ---------------------------------------------------------------------------
// Shard open / create / lookup helpers.
// ---------------------------------------------------------------------------

[[nodiscard]] Relation
open_relation_cached(Oid sub_table_relid, LOCKMODE lock_mode,
                     seq_caches* cache,
                     std::int16_t column_attno, std::int16_t bucket_idx,
                     std::int32_t shard_idx)
{
    if (cache != nullptr) {
        const std::uint64_t key = seq_caches::key_for(column_attno, bucket_idx, shard_idx);
        auto it = cache->entries.find(key);
        if (it != cache->entries.end() && it->second.rel != nullptr) {
            return it->second.rel;
        }
        // DOC0260 §7.9 / §A1.4 — the resource owner the shard Relation is
        // pinned under MUST match the owner that is current when
        // `seq_caches::close_all` runs `relation_close()`. That owner differs
        // by side:
        //
        //   * Write side: the seq_caches lives on the transaction-scoped
        //     BioseqColumnWriteState and is closed by
        //     BioseqColumnPopWriteStateForAllRels at xact commit, under
        //     TopTransactionResourceOwner. The cached Relation therefore has
        //     to outlive the per-statement owner (which is torn down at the
        //     end of the surrounding INSERT). We pin it under
        //     TopTransactionResourceOwner so open/close balance. Pinning it
        //     under the per-stmt owner instead leaks it (force-closed at
        //     statement end) and then double-closes at xact commit.
        //
        //   * Read side: the seq_caches lives for the duration of one scan and
        //     is closed in bioseq_endscan / bioseq_index_fetch_end under the
        //     SAME portal/executor owner that is current here. We must NOT
        //     swap — pinning under TopTransaction trips "relcache reference
        //     0x... is not owned by resource owner Portal" at scan teardown.
        Relation rel = nullptr;
        if (cache->side == seq_caches::Side::Write) {
            ResourceOwner saved_owner = CurrentResourceOwner;
            CurrentResourceOwner       = TopTransactionResourceOwner;
            rel = try_safe_relation_open(sub_table_relid, lock_mode);
            CurrentResourceOwner       = saved_owner;
        } else {
            rel = try_safe_relation_open(sub_table_relid, lock_mode);
        }
        if (rel == nullptr) {
            bioseq_log<ERR, PG>( "bioseq storage: failed to open sub-table relid {}",
                   static_cast<unsigned>(sub_table_relid));
        }
        seq_caches::entry& entry = cache->entries[key];
        entry.rel = rel;
        return rel;
    }
    Relation rel = try_safe_relation_open(sub_table_relid, lock_mode);
    if (rel == nullptr) {
        bioseq_log<ERR, PG>( "bioseq storage: failed to open sub-table relid {}",
               static_cast<unsigned>(sub_table_relid));
    }
    return rel;
}

[[nodiscard]] Relation
allocate_new_shard_and_open(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                            std::int16_t column_attno, TupleDesc parent_tupdesc,
                            std::int16_t bucket_idx, std::int32_t starting_shard_idx,
                            LOCKMODE lock_mode, seq_caches* cache)
{
    const std::int64_t lock_key =
        shard_alloc_lock_key(db_oid, main_relfilenode, column_attno, bucket_idx);
    DirectFunctionCall1(pg_advisory_xact_lock_int8, Int64GetDatum(lock_key));

    if (auto rechecked = SubTableManager::lookup_active_shard(
            db_oid, spc_oid, main_relfilenode, column_attno, bucket_idx);
        rechecked.has_value() && rechecked->shard_idx >= starting_shard_idx)
    {
        return open_relation_cached(rechecked->sub_table_relid, lock_mode, cache,
                                    column_attno, bucket_idx, rechecked->shard_idx);
    }

    const SeqColumnTypmod tm = column_typmod_at(parent_tupdesc, column_attno);
    const char* const persistence_kw =
        persistence_keyword_for_relfilenode(db_oid, spc_oid, main_relfilenode);

    const std::string sub_name = format_sub_table_name(
        db_oid, spc_oid, main_relfilenode, column_attno, bucket_idx, starting_shard_idx);
    const std::string sidecar_name = format_sidecar_name(
        db_oid, spc_oid, main_relfilenode, column_attno, bucket_idx, starting_shard_idx);

    // DOC0260 §7.3 — lazy CREATE TABLE for the sub-table + matching sidecar.
    // We deliberately do NOT register the freshly-created relations with the
    // extension via ALTER EXTENSION ADD TABLE here: doing so requires a
    // SECURITY DEFINER plpgsql helper that issues ALTER EXTENSION inside SPI
    // inside the parent INSERT's access-method callback. That nests three
    // ResourceOwner layers (executor → SPI → SECURITY DEFINER) and Postgres
    // refuses to clean up the inner TupleDescs at xact end ("tupdesc
    // reference is not owned by resource owner TopTransaction" → PANIC).
    // The bioseq_internal.seq_sub_table catalog row is the canonical
    // tracker for cleanup (BioseqColumnTableDropHook /
    // bioseq_relation_set_new_filenode + drop_sub_tables_for_main); the
    // ALTER EXTENSION ADD TABLE was a pg_dump convenience that we trade
    // for correctness — pg_dump still round-trips data through SELECT/COPY.
    StringInfo ddl = makeStringInfo();
    appendStringInfo(ddl,
        "CREATE %s TABLE IF NOT EXISTS bioseq_internal.%s "
            "(seq bioseq.seq('%s', %d)) USING bioseq;\n"
        "CREATE %s TABLE IF NOT EXISTS bioseq_internal.%s "
            "(sub_rowno BIGINT PRIMARY KEY, blob BYTEA NOT NULL);",
        persistence_kw, quote_identifier(sub_name.c_str()),
        tm.alphabet_name.c_str(),
        tm.storage_unit_bit_width,
        persistence_kw, quote_identifier(sidecar_name.c_str()));

    Oid new_sub_relid = InvalidOid;
    Oid new_sidecar_relid = InvalidOid;

    // DOC0260 §7.3 — wrap the lazy CREATE TABLE in an internal sub-transaction
    // so the surrounding INSERT statement's ResourceOwner / xact state stays
    // intact regardless of what the DDL touches. Without this wrap, errors
    // raised by the SPI body abort the parent INSERT with a "ResourceOwnerEnlarge
    // called after release started" because the SPI-allocated resources outlive
    // their owner.
    //
    // DOC0260 §A1.4 ROOT-CAUSE FIX — BeginInternalSubTransaction switches
    // CurrentResourceOwner and CurrentMemoryContext to the sub-transaction's,
    // and CommitSubTransaction (reached via ReleaseCurrentSubTransaction)
    // leaves CurrentResourceOwner pointing at the *parent transaction's*
    // owner (TopTransactionResourceOwner) — NOT the executor/portal owner
    // that was current when this TAM insert callback was entered. If we do
    // not restore it, the surrounding INSERT's standard_ExecutorEnd runs
    // ExecResetTupleTable under TopTransactionResourceOwner and trips
    // "tupdesc reference 0x... is not owned by resource owner TopTransaction"
    // when releasing slot descriptors that were pinned under the executor
    // owner. We therefore save and restore BOTH the resource owner and the
    // memory context, mirroring the canonical pl/pgsql exception-block
    // pattern (pl_exec.c exec_stmt_block).
    MemoryContext subxact_saved_context = CurrentMemoryContext;
    ResourceOwner subxact_saved_owner   = CurrentResourceOwner;
    BeginInternalSubTransaction("bioseq storage CREATE shard");
    // Run the body in the caller's memory context, not the sub-xact's.
    MemoryContextSwitchTo(subxact_saved_context);
    PG_TRY();
    {
        if (SPI_connect() != SPI_OK_CONNECT) {
            bioseq_log<ERR, PG>( "bioseq storage: SPI_connect failed while allocating shard");
        }
        // SPI_execute returns the result code of the LAST command in a multi-
        // statement string. Our DDL is two CREATE TABLEs, so the expected
        // return is SPI_OK_UTILITY. Accept SPI_OK_SELECT too as defensive
        // back-compat in case a SELECT clause is added later.
        const int rc = SPI_execute(ddl->data, /*read_only=*/false, /*tcount=*/0);
        if (rc != SPI_OK_UTILITY && rc != SPI_OK_SELECT) {
            bioseq_log<ERR, PG>(
                   "bioseq storage: failed to create sub-table {} + sidecar {} "
                   "(SPI rc={})",
                   sub_name, sidecar_name, rc);
        }
        SPI_finish();

        const Oid bioseq_internal_ns = get_namespace_oid("bioseq_internal", false);
        new_sub_relid     = get_relname_relid(sub_name.c_str(),     bioseq_internal_ns);
        new_sidecar_relid = get_relname_relid(sidecar_name.c_str(), bioseq_internal_ns);
        if (!OidIsValid(new_sub_relid) || !OidIsValid(new_sidecar_relid)) {
            bioseq_log<ERR, PG>( "bioseq storage: sub-table or sidecar not found after CREATE");
        }
        ReleaseCurrentSubTransaction();
        MemoryContextSwitchTo(subxact_saved_context);
        CurrentResourceOwner = subxact_saved_owner;
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(subxact_saved_context);
        RollbackAndReleaseCurrentSubTransaction();
        MemoryContextSwitchTo(subxact_saved_context);
        CurrentResourceOwner = subxact_saved_owner;
        PG_RE_THROW();
    }
    PG_END_TRY();

    SubTableRow row = SubTableManager::insert_shard(db_oid, spc_oid, main_relfilenode,
                                          column_attno, bucket_idx, starting_shard_idx,
                                          new_sub_relid, new_sidecar_relid);

    if (row.sub_table_relid != new_sub_relid) {
        // Another backend won the race; we DROP what we just made.
        // Reaper is the long-tail safety net.
        StringInfo drop = makeStringInfo();
        appendStringInfo(drop,
            "DROP TABLE bioseq_internal.%s; DROP TABLE bioseq_internal.%s;",
            quote_identifier(sub_name.c_str()),
            quote_identifier(sidecar_name.c_str()));
        if (SPI_connect() == SPI_OK_CONNECT) {
            SPI_execute(drop->data, /*read_only=*/false, /*tcount=*/0);
            SPI_finish();
        }
    }

    return open_relation_cached(row.sub_table_relid, lock_mode, cache,
                                column_attno, bucket_idx, row.shard_idx);
}

[[nodiscard]] Relation
open_or_create_shard(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                     std::int16_t column_attno, TupleDesc parent_tupdesc,
                     std::int16_t bucket_idx,
                     LOCKMODE lock_mode, seq_caches* cache)
{
    if (auto active = SubTableManager::lookup_active_shard(
            db_oid, spc_oid, main_relfilenode, column_attno, bucket_idx);
        active.has_value())
    {
        return open_relation_cached(active->sub_table_relid, lock_mode, cache,
                                    column_attno, bucket_idx, active->shard_idx);
    }
    // No ACTIVE shard. This happens (a) on the very first insert into the
    // (parent, column, bucket), or (b) when every existing shard has been
    // marked full — e.g. under the cap=0 sentinel where close_all flags each
    // shard full at commit (DOC0260 §10.2). In case (b) shard 0 (and others)
    // already exist, so starting at shard_idx 0 would re-INSERT an existing
    // catalog PK ("duplicate key violates seq_sub_table_pkey"). Allocate the
    // next index after the highest existing shard for this (column, bucket).
    std::int32_t next_shard_idx = 0;
    for (const auto& r : SubTableManager::list_shards_for_main(
             db_oid, spc_oid, main_relfilenode)) {
        if (r.column_attno == column_attno && r.bucket_idx == bucket_idx
            && r.shard_idx >= next_shard_idx) {
            next_shard_idx = r.shard_idx + 1;
        }
    }
    return allocate_new_shard_and_open(db_oid, spc_oid, main_relfilenode,
                                       column_attno, parent_tupdesc, bucket_idx,
                                       next_shard_idx,
                                       lock_mode, cache);
}

[[nodiscard]] Relation
open_sub_table_for_locator(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                           std::int16_t column_attno, const seq_locator& loc,
                           LOCKMODE lock_mode, seq_caches* cache)
{
    if (cache != nullptr) {
        const std::uint64_t key = seq_caches::key_for(
            column_attno,
            static_cast<std::int16_t>(loc.bucket_idx),
            static_cast<std::int32_t>(loc.shard_idx));
        auto it = cache->entries.find(key);
        if (it != cache->entries.end() && it->second.rel != nullptr) {
            return it->second.rel;
        }
    }
    auto row = SubTableManager::lookup_shard(db_oid, spc_oid, main_relfilenode,
        column_attno, static_cast<std::int16_t>(loc.bucket_idx),
        static_cast<std::int32_t>(loc.shard_idx));
    if (!row.has_value()) {
        bioseq_log<ERR, PG>(
               "bioseq storage: locator references nonexistent shard "
               "(db={}, spc={}, relfilenode={}, attno={}, bucket={}, shard={})",
               static_cast<unsigned>(db_oid),
               static_cast<unsigned>(spc_oid),
               static_cast<unsigned>(main_relfilenode),
               static_cast<int>(column_attno),
               static_cast<unsigned>(loc.bucket_idx),
               loc.shard_idx);
    }
    return open_relation_cached(row->sub_table_relid, lock_mode, cache,
                                column_attno,
                                static_cast<std::int16_t>(loc.bucket_idx),
                                static_cast<std::int32_t>(loc.shard_idx));
}

// ---------------------------------------------------------------------------
// DOC0264 §8 — insert_sequence_streamed (write hot path).
//
// One sequence → ceil(total_bp / fragment_bp) sub-table rows. Producer is
// invoked once per fragment in a per-fragment MemoryContext that is reset
// between fragments so peak RSS is bounded by one fragment's worth of state
// (consolidated-review C3). Per-sequence shard-rollover pre-allocation
// (§8.3) keeps a sequence's fragments in a single shard so the locator's
// shard_idx is a single value.
// ---------------------------------------------------------------------------

namespace detail {

// DOC0264 §4 — per-fragment storage_unit count. Class member from
// `sequence_scanner_none<T>::fragment_storage_units` (DOC0263 §3) = 1024.
// DOC0286 §4.3 — aliases the exported `kSequenceFragmentStorageUnits` so the
// public constant and this hot-path detail constant can never drift.
inline constexpr std::size_t kFragmentStorageUnits = kSequenceFragmentStorageUnits;

// Look-up bits_per_char via the supported-alphabet pack so a future alphabet
// addition does not silently produce wrong fragment_bp values.
[[nodiscard]] inline std::size_t bits_per_char_for_alphabet_code(
    pgseq_t::alphabet_code_t alpha) noexcept {
    std::size_t bits_per_char = 0;
    pgseq_supported_alphabets::dispatch_alphabet_by_code(alpha,
        [&bits_per_char]<class A>() {
            bits_per_char = static_cast<std::size_t>(
                seqan3::detail::ceil_log2(
                    static_cast<std::size_t>(seqan3::alphabet_size<A>)));
        });
    return bits_per_char;
}

// letters_per_unit = (storage_unit_byte_width * 8) / bits_per_char.
[[nodiscard]] inline std::size_t letters_per_unit_for(
    pgseq_t::alphabet_code_t alpha, std::uint8_t logical_byte_width) noexcept {
    const std::size_t bpc = bits_per_char_for_alphabet_code(alpha);
    return bpc > 0
        ? (static_cast<std::size_t>(logical_byte_width) * 8u) / bpc
        : 0u;
}

}  // namespace detail

template<class L, std::size_t M, std::size_t X>
template<class FragmentProducer>
seq_locator
sequence_storage_manager<L, M, X>::insert_sequence_streamed(
    Oid db_oid, Oid spc_oid, Oid main_relfilenode,
    std::int16_t column_attno, TupleDesc parent_tupdesc,
    std::uint8_t logical_alphabet_code,
    std::uint8_t logical_storage_unit_byte_width,
    sequence_size_t total_bp_size,
    std::uint16_t last_fragment_padding_letters,
    sequence_size_t current_cap_bytes,
    seq_caches* cache,
    FragmentProducer&& producer)
{
    if (cache == nullptr || cache->side != seq_caches::Side::Write) {
        bioseq_log<ERR, PG>( "bioseq storage: insert_sequence_streamed requires a write-side seq_caches");
    }
    if (cache->main_database_oid == InvalidOid) {
        cache->main_database_oid   = db_oid;
        cache->main_tablespace_oid = spc_oid;
        cache->main_relfilenode    = main_relfilenode;
    }

    const auto alpha_code = static_cast<pgseq_t::alphabet_code_t>(logical_alphabet_code);
    const std::size_t lpu = detail::letters_per_unit_for(alpha_code,
                                                         logical_storage_unit_byte_width);
    if (lpu == 0) {
        bioseq_log<ERR, PG>(
               "bioseq storage Phase C: insert_sequence_streamed could not derive "
               "letters_per_unit (alphabet_code={}, byte_width={})",
               static_cast<unsigned>(alpha_code),
               static_cast<unsigned>(logical_storage_unit_byte_width));
    }
    const sequence_size_t frag_bp =
        static_cast<sequence_size_t>(detail::kFragmentStorageUnits) * lpu;
    const sequence_size_t num_fragments =
        total_bp_size == 0 ? 0u : (total_bp_size + frag_bp - 1) / frag_bp;
    if (num_fragments > static_cast<sequence_size_t>(
            std::numeric_limits<std::uint32_t>::max())) {
        bioseq_log<ERR, PG>(
               "bioseq storage Phase C: num_fragments {} exceeds UINT32_MAX",
               num_fragments);
    }

    // Per-row varlena bytes for a full (= non-last) fragment.
    const std::size_t frag_varlena_bytes =
        pgseq_t::header_bytes()
        + detail::kFragmentStorageUnits
              * static_cast<std::size_t>(logical_storage_unit_byte_width);
    // Total varlena bytes the sequence occupies in the shard (= sum of
    // fragment-row sizes). Used for the per-shard cap accounting; each row
    // is the same size in Phase C because the last fragment row is also
    // allocated at `frag_varlena_bytes` (palloc0 leaves trailing units 0).
    const sequence_size_t total_varlena_bytes =
        static_cast<sequence_size_t>(num_fragments) *
        static_cast<sequence_size_t>(frag_varlena_bytes);

    const std::int16_t bucket_idx =
        static_cast<std::int16_t>(bucket_index_for_size(total_bp_size));

    // DOC0264 §8.3 — per-sequence shard pre-allocation. If the active shard
    // cannot fit the WHOLE sequence's bytes, allocate a fresh shard up front.
    Relation shard = open_or_create_shard(db_oid, spc_oid, main_relfilenode,
                                          column_attno, parent_tupdesc, bucket_idx,
                                          RowExclusiveLock, cache);

    auto active = SubTableManager::lookup_active_shard(db_oid, spc_oid, main_relfilenode,
                                            column_attno, bucket_idx);
    if (!active.has_value()) {
        bioseq_log<ERR, PG>( "bioseq storage: active shard disappeared mid-insert");
    }
    const std::uint64_t active_key =
        seq_caches::key_for(column_attno, bucket_idx, active->shard_idx);
    sequence_size_t pending = 0;
    if (auto it = cache->entries.find(active_key); it != cache->entries.end()) {
        pending = it->second.pending_bytes_delta;
    }
    const sequence_size_t projected =
        active->bytes_written + pending + total_varlena_bytes;
    // DOC0264 §8.3 — pre-allocate a fresh shard when the active one cannot
    // fit the whole sequence. Same semantics as Phase A's per-row rollover,
    // but the unit of accounting is the whole sequence now.
    const bool rollover_required =
        active->bytes_written + pending > 0
        && (current_cap_bytes == 0 || projected > current_cap_bytes);

    std::int32_t shard_idx = 0;
    if (rollover_required) {
        if (auto it = cache->entries.find(active_key);
            it != cache->entries.end() && it->second.pending_bytes_delta != 0)
        {
            SubTableManager::update_shard_bytes_written(
                db_oid, spc_oid, main_relfilenode,
                column_attno, bucket_idx, active->shard_idx,
                it->second.pending_bytes_delta,
                current_cap_bytes);
            it->second.pending_bytes_delta = 0;
            it->second.pending_total_payload_delta = 0;
        } else {
            SubTableManager::update_shard_bytes_written(
                db_oid, spc_oid, main_relfilenode,
                column_attno, bucket_idx, active->shard_idx,
                /*delta_bytes=*/0,
                current_cap_bytes == 0 ? sequence_size_t{1} : current_cap_bytes);
        }
        shard = allocate_new_shard_and_open(db_oid, spc_oid, main_relfilenode,
                                            column_attno, parent_tupdesc, bucket_idx,
                                            active->shard_idx + 1,
                                            RowExclusiveLock, cache);
        shard_idx = active->shard_idx + 1;
    } else {
        shard_idx = active->shard_idx;
    }

    // DOC0264 §8.3 hard guard: refuse to write a sequence whose total bytes
    // exceed the cap (would not fit any shard, including a freshly-allocated
    // one). current_cap_bytes==0 is the sentinel "no cap, one shard per
    // sequence" and the check is suppressed.
    if (current_cap_bytes > 0 && total_varlena_bytes > current_cap_bytes) {
        bioseq_log<ERR, PG>(
            "bioseq storage Phase C: sequence with {} bytes exceeds shard cap "
            "{} bytes ({} GiB). Raise bioseq.storage.max_seq_sub_table_gbs.",
            total_varlena_bytes, current_cap_bytes,
            current_cap_bytes >> 30);
    }

    const std::uint64_t cache_key =
        seq_caches::key_for(column_attno, bucket_idx, shard_idx);
    seq_caches::entry& entry = cache->entries[cache_key];
    entry.rel = shard;

    if (entry.write_state == nullptr) {
        entry.write_state = bioseq_init_write_state(
            shard, RelationGetDescr(shard),
            RelationGetRelid(shard),
            GetCurrentSubTransactionId());
    }

    // DOC0264 §8.4 — serialise writers on this (parent, column, bucket,
    // shard) so the row numbers BioseqColumnWriteRow assigns are CONTIGUOUS
    // within one sequence's fragments.
    const std::int64_t row_lock_key =
          (static_cast<std::int64_t>(db_oid)                << 48)
        ^ (static_cast<std::int64_t>(main_relfilenode)      << 16)
        ^ (static_cast<std::int64_t>(
               static_cast<std::uint16_t>(column_attno))    << 8)
        ^ (static_cast<std::int64_t>(
               static_cast<std::uint16_t>(bucket_idx))      << 4)
        ^  static_cast<std::int64_t>(shard_idx);
    DirectFunctionCall1(pg_advisory_xact_lock_int8, Int64GetDatum(row_lock_key));

    // DOC0264 §8 — separate scratch + per-fragment contexts. The producer
    // may allocate long-lived state in `scratch_mctx`; per-fragment palloc
    // happens in `per_fragment_mctx` which is RESET between fragments to
    // keep peak RSS bounded.
    MemoryContext scratch_mctx = AllocSetContextCreate(
        CurrentMemoryContext, "bioseq insert_sequence scratch",
        ALLOCSET_DEFAULT_SIZES);
    MemoryContext per_fragment_mctx = AllocSetContextCreate(
        CurrentMemoryContext, "bioseq insert_sequence per-fragment",
        ALLOCSET_DEFAULT_SIZES);

    std::uint64_t first_sub_row_no = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t last_sub_row_no  = std::numeric_limits<std::uint64_t>::max();

    for (sequence_size_t fragment_no = 0; fragment_no < num_fragments; ++fragment_no) {
        MemoryContextReset(per_fragment_mctx);
        const bool is_last = (fragment_no + 1 == num_fragments);
        MemoryContextScope frag_ctx{per_fragment_mctx};

        pgseq_t* frag = static_cast<pgseq_t*>(palloc0(frag_varlena_bytes));
        SET_VARSIZE(frag, frag_varlena_bytes);
        frag->alphabet_code           = static_cast<pgseq_t::alphabet_code_t>(
                                            logical_alphabet_code);
        frag->storage_unit_byte_width = logical_storage_unit_byte_width;
        // Producer fills bp_size + padding_letters + data[] for fragment `n`.
        std::forward<FragmentProducer>(producer)(
            static_cast<std::size_t>(fragment_no), frag, scratch_mctx, is_last);

        // Cross-check the producer's bp_size / padding consistency.
        const sequence_size_t expected_bp = is_last
            ? (total_bp_size - fragment_no * frag_bp)
            : frag_bp;
        if (static_cast<sequence_size_t>(frag->bp_size) != expected_bp) {
            bioseq_log<ERR, PG>(
                "bioseq storage Phase C: producer bp_size mismatch on fragment {} "
                "(got {}, expected {})",
                fragment_no, static_cast<sequence_size_t>(frag->bp_size), expected_bp);
        }
        if (is_last && frag->padding_letters != last_fragment_padding_letters) {
            bioseq_log<ERR, PG>(
                "bioseq storage Phase C: producer last-fragment padding_letters "
                "mismatch (got {}, expected {})",
                static_cast<unsigned>(frag->padding_letters),
                static_cast<unsigned>(last_fragment_padding_letters));
        }
        if (!is_last && frag->padding_letters != 0) {
            bioseq_log<ERR, PG>(
                "bioseq storage Phase C: producer non-last padding_letters must be 0 "
                "(fragment {}, got {})",
                fragment_no, static_cast<unsigned>(frag->padding_letters));
        }

        Datum values[1] = { PointerGetDatum(frag) };
        bool  nulls[1]  = { false };
        const std::uint64_t row_no = BioseqColumnWriteRow(entry.write_state, values, nulls);
        if (fragment_no == 0) {
            first_sub_row_no = row_no;
        } else {
            if (row_no != last_sub_row_no + 1) {
                bioseq_log<ERR, PG>(
                    "bioseq storage Phase C: non-contiguous row numbers during "
                    "streamed insert (expected {}, got {})",
                    last_sub_row_no + 1, row_no);
            }
        }
        last_sub_row_no = row_no;
    }

    MemoryContextDelete(per_fragment_mctx);
    MemoryContextDelete(scratch_mctx);

    entry.pending_bytes_delta         += total_varlena_bytes;
    entry.pending_total_payload_delta += total_varlena_bytes;

    if (num_fragments == 0) {
        // Empty sequence — no rows were written. Choose a deterministic row
        // number (0) as the stub's first_sub_row_no; the scanner short-
        // circuits when num_fragments == 0 and never reads this row.
        first_sub_row_no = 0;
    }

    seq_locator out{};
    out.version          = kLocatorVersion;
    out.bucket_idx       = static_cast<std::uint8_t>(bucket_idx);
    out._pad_2           = 0;
    out.shard_idx        = static_cast<std::uint32_t>(shard_idx);
    out.num_fragments    = static_cast<std::uint32_t>(num_fragments);
    out._pad_4           = 0;
    out.total_bp_size    = total_bp_size;
    out.first_sub_row_no = first_sub_row_no;
    return out;
}

template<class L, std::size_t M, std::size_t X>
seq_locator
sequence_storage_manager<L, M, X>::insert_payload_as_fragments(
    Oid db_oid, Oid spc_oid, Oid main_relfilenode,
    std::int16_t column_attno, TupleDesc parent_tupdesc,
    const pgseq_t* original_hdr,
    sequence_size_t current_cap_bytes,
    seq_caches* cache)
{
    if (original_hdr == nullptr) {
        bioseq_log<ERR, PG>( "bioseq storage: insert_payload_as_fragments received null original_hdr");
    }
    if (is_locator_stub(original_hdr)) {
        bioseq_log<ERR, PG>( "bioseq storage: insert_payload_as_fragments received a stub; "
               "the caller must screen out stubs before re-inserting");
    }

    const sequence_size_t total_bp_size =
        static_cast<sequence_size_t>(original_hdr->bp_size);
    const std::uint8_t logical_alphabet_code =
        static_cast<std::uint8_t>(original_hdr->alphabet_code);
    const std::uint8_t logical_byte_width =
        static_cast<std::uint8_t>(original_hdr->storage_unit_byte_width);
    const std::uint16_t last_fragment_padding =
        static_cast<std::uint16_t>(original_hdr->padding_letters);

    const auto alpha_code = static_cast<pgseq_t::alphabet_code_t>(logical_alphabet_code);
    const std::size_t lpu = detail::letters_per_unit_for(alpha_code, logical_byte_width);
    if (lpu == 0) {
        bioseq_log<ERR, PG>(
               "bioseq storage Phase C: insert_payload_as_fragments could not derive "
               "letters_per_unit (alphabet_code={}, byte_width={})",
               static_cast<unsigned>(alpha_code),
               static_cast<unsigned>(logical_byte_width));
    }
    const sequence_size_t frag_bp =
        static_cast<sequence_size_t>(detail::kFragmentStorageUnits) * lpu;
    const sequence_size_t num_fragments =
        total_bp_size == 0 ? 0u : (total_bp_size + frag_bp - 1) / frag_bp;
    const std::size_t bytes_per_unit =
        static_cast<std::size_t>(logical_byte_width);
    const std::size_t fragment_bytes =
        detail::kFragmentStorageUnits * bytes_per_unit;

    // Slice the original payload into N fragment rows.
    const std::uint8_t* const src_data =
        reinterpret_cast<const std::uint8_t*>(original_hdr->data);
    const std::size_t src_bytes =
        static_cast<std::size_t>(VARSIZE(original_hdr)) - pgseq_t::header_bytes();

    auto producer = [src_data, src_bytes, fragment_bytes, frag_bp, total_bp_size,
                     last_fragment_padding](
        std::size_t fragment_no, pgseq_t* dst, MemoryContext /*scratch*/,
        bool is_last) noexcept
    {
        const std::size_t src_offset = fragment_no * fragment_bytes;
        const std::size_t copy_bytes = std::min(fragment_bytes,
            src_offset < src_bytes ? src_bytes - src_offset : std::size_t{0});
        if (copy_bytes > 0) {
            std::memcpy(dst->data, src_data + src_offset, copy_bytes);
        }
        // Trailing bytes already zero from palloc0.
        const sequence_size_t this_fragment_bp = is_last
            ? (total_bp_size - static_cast<sequence_size_t>(fragment_no) * frag_bp)
            : frag_bp;
        dst->bp_size = static_cast<pgseq_t::bp_size_t>(this_fragment_bp);
        dst->padding_letters = is_last ? last_fragment_padding : std::uint16_t{0};
    };

    return insert_sequence_streamed(
        db_oid, spc_oid, main_relfilenode, column_attno, parent_tupdesc,
        logical_alphabet_code, logical_byte_width,
        total_bp_size, last_fragment_padding, current_cap_bytes, cache,
        producer);
}

// ---------------------------------------------------------------------------
// DOC0286 §4.3 — sequence_stream_writer implementation. Mirrors the per-fragment
// row write of insert_sequence_streamed, but driven incrementally: the total
// length is unknown until finish(), so the shard is opened lazily and the cap
// is checked as the sequence streams. No cross-shard rollover (one shard per
// sequence); a single sequence over the cap raises ERROR and the transaction
// rollback cleans the partial fragment rows.
// ---------------------------------------------------------------------------
void sequence_stream_writer::begin(
    Oid db_oid, Oid spc_oid, RelFileNumber main_relfilenode,
    AttrNumber seq_attno, TupleDesc parent_td,
    std::uint8_t alphabet_code, std::uint8_t byte_width,
    sequence_size_t cap_bytes, seq_caches* caches)
{
    if (caches == nullptr || caches->side != seq_caches::Side::Write) {
        bioseq_log<ERR, PG>( "bioseq.load: sequence_stream_writer requires a write-side seq_caches");
    }
    db_oid_ = db_oid; spc_oid_ = spc_oid; main_relfilenode_ = main_relfilenode;
    column_attno_ = static_cast<std::int16_t>(seq_attno);
    parent_td_ = parent_td;
    alphabet_code_ = alphabet_code; byte_width_ = byte_width;
    cap_bytes_ = cap_bytes; caches_ = caches;

    if (caches_->main_database_oid == InvalidOid) {
        caches_->main_database_oid   = db_oid;
        caches_->main_tablespace_oid = spc_oid;
        caches_->main_relfilenode    = main_relfilenode;
    }

    const auto alpha = static_cast<pgseq_t::alphabet_code_t>(alphabet_code);
    const std::size_t lpu = detail::letters_per_unit_for(alpha, byte_width);
    if (lpu == 0) {
        bioseq_log<ERR, PG>( "bioseq.load: could not derive letters_per_unit "
               "(alphabet_code={}, byte_width={})",
               static_cast<unsigned>(alpha), static_cast<unsigned>(byte_width));
    }
    frag_payload_bytes_ = detail::kFragmentStorageUnits * static_cast<std::size_t>(byte_width);
    frag_varlena_bytes_ = pgseq_t::header_bytes() + frag_payload_bytes_;
    frag_bp_ = static_cast<sequence_size_t>(detail::kFragmentStorageUnits) * lpu;

    buffer_.clear();
    buffer_.reserve(frag_payload_bytes_ + 256);
    first_sub_row_no_ = std::numeric_limits<std::uint64_t>::max();
    last_sub_row_no_  = std::numeric_limits<std::uint64_t>::max();
    num_fragments_ = 0;
    total_varlena_bytes_ = 0;
    shard_ = nullptr;
    write_state_ = nullptr;
    active_ = true;
}

void sequence_stream_writer::open_shard() {
    if (shard_ != nullptr) return;
    // All streamed sequences land in the top length bucket: the size is unknown
    // until finish(), the bucket is purely organisational (the locator records
    // it; reads use the locator), and co-locating a load's fragments keeps
    // sequential-read locality.
    bucket_idx_ = static_cast<std::int16_t>(
        bioseq_default_storage_manager::bucket_index_for_size(
            std::numeric_limits<sequence_size_t>::max()));
    shard_ = open_or_create_shard(db_oid_, spc_oid_, main_relfilenode_,
                                  column_attno_, parent_td_, bucket_idx_,
                                  RowExclusiveLock, caches_);
    auto active = SubTableManager::lookup_active_shard(db_oid_, spc_oid_, main_relfilenode_,
                                            column_attno_, bucket_idx_);
    if (!active.has_value()) {
        bioseq_log<ERR, PG>( "bioseq.load: active shard disappeared mid-load");
    }
    shard_idx_ = active->shard_idx;
    cache_key_ = seq_caches::key_for(column_attno_, bucket_idx_, shard_idx_);
    seq_caches::entry& entry = caches_->entries[cache_key_];
    entry.rel = shard_;
    if (entry.write_state == nullptr) {
        entry.write_state = bioseq_init_write_state(
            shard_, RelationGetDescr(shard_), RelationGetRelid(shard_),
            GetCurrentSubTransactionId());
    }
    write_state_ = entry.write_state;
    // DOC0264 §8.4 — serialise writers on this (parent, column, bucket, shard)
    // so the row numbers BioseqColumnWriteRow assigns stay contiguous within one
    // sequence's fragments (and across the sequences of one load).
    const std::int64_t row_lock_key =
          (static_cast<std::int64_t>(db_oid_)            << 48)
        ^ (static_cast<std::int64_t>(main_relfilenode_)  << 16)
        ^ (static_cast<std::int64_t>(
               static_cast<std::uint16_t>(column_attno_)) << 8)
        ^ (static_cast<std::int64_t>(
               static_cast<std::uint16_t>(bucket_idx_))   << 4)
        ^  static_cast<std::int64_t>(shard_idx_);
    DirectFunctionCall1(pg_advisory_xact_lock_int8, Int64GetDatum(row_lock_key));
}

void sequence_stream_writer::write_fragment(
    const std::uint8_t* payload, std::size_t payload_bytes,
    sequence_size_t bp, std::uint16_t pad)
{
    open_shard();
    if (cap_bytes_ > 0 &&
        total_varlena_bytes_ + frag_varlena_bytes_ > cap_bytes_) {
        bioseq_log<ERR, PG>(
            "bioseq.load: sequence exceeds shard cap {} bytes ({} GiB) at "
            "fragment {}. Raise bioseq.storage.max_seq_sub_table_gbs.",
            total_varlena_bytes_ + frag_varlena_bytes_,
            cap_bytes_ >> 30, num_fragments_);
    }
    pgseq_t* frag;
    {
        MemoryContextScope guard{BioseqColumnWritePerTupleContext(write_state_)};
        frag = static_cast<pgseq_t*>(palloc0(frag_varlena_bytes_));
        SET_VARSIZE(frag, frag_varlena_bytes_);
        frag->alphabet_code           = static_cast<pgseq_t::alphabet_code_t>(alphabet_code_);
        frag->storage_unit_byte_width = byte_width_;
        frag->bp_size                 = static_cast<pgseq_t::bp_size_t>(bp);
        frag->padding_letters         = pad;
        if (payload_bytes > 0) {
            std::memcpy(frag->data, payload, payload_bytes);
        }
    }
    Datum values[1] = { PointerGetDatum(frag) };
    bool  nulls[1]  = { false };
    const std::uint64_t row_no = BioseqColumnWriteRow(write_state_, values, nulls);
    if (num_fragments_ == 0) {
        first_sub_row_no_ = row_no;
    } else if (row_no != last_sub_row_no_ + 1) {
        bioseq_log<ERR, PG>(
            "bioseq.load: non-contiguous row numbers during streamed insert "
            "(expected {}, got {})", last_sub_row_no_ + 1, row_no);
    }
    last_sub_row_no_ = row_no;
    ++num_fragments_;
    total_varlena_bytes_ += frag_varlena_bytes_;
}

void sequence_stream_writer::append_packed_bytes(
    std::span<const std::uint8_t> bytes, sequence_size_t /*bp_count*/)
{
    if (!active_) {
        bioseq_log<ERR, PG>( "bioseq.load: append_packed_bytes before begin()");
    }
    if (bytes.empty()) return;
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    // Write full fragments only while STRICTLY more than one full fragment is
    // buffered, so the final (possibly padded) fragment is always emitted by
    // finish() with the caller's last_pad. Non-final fragments are full units
    // (padding 0, bp = frag_bp_).
    std::size_t off = 0;
    while (buffer_.size() - off > frag_payload_bytes_) {
        write_fragment(buffer_.data() + off, frag_payload_bytes_, frag_bp_, 0);
        off += frag_payload_bytes_;
    }
    if (off > 0) {
        buffer_.erase(buffer_.begin(),
                      buffer_.begin() + static_cast<std::ptrdiff_t>(off));
    }
}

seq_locator sequence_stream_writer::finish(sequence_size_t total_bp,
                                           std::uint16_t last_pad)
{
    if (!active_) {
        bioseq_log<ERR, PG>( "bioseq.load: finish() before begin()");
    }
    // Emit the final fragment (the buffered remainder ≤ one fragment). Its bp is
    // whatever remains after the full fragments already written.
    const sequence_size_t emitted_bp =
        static_cast<sequence_size_t>(num_fragments_) * frag_bp_;
    if (!buffer_.empty()) {
        const sequence_size_t last_bp = total_bp - emitted_bp;
        write_fragment(buffer_.data(), buffer_.size(), last_bp, last_pad);
        buffer_.clear();
    }

    if (num_fragments_ == 0) {
        // Empty sequence — no rows written; the scanner short-circuits on
        // num_fragments == 0 and never reads first_sub_row_no.
        first_sub_row_no_ = 0;
    } else {
        seq_caches::entry& entry = caches_->entries[cache_key_];
        entry.pending_bytes_delta         += total_varlena_bytes_;
        entry.pending_total_payload_delta += total_varlena_bytes_;
    }

    seq_locator out{};
    out.version          = kLocatorVersion;
    out.bucket_idx       = static_cast<std::uint8_t>(bucket_idx_);
    out._pad_2           = 0;
    out.shard_idx        = static_cast<std::uint32_t>(shard_idx_);
    out.num_fragments    = num_fragments_;
    out._pad_4           = 0;
    out.total_bp_size    = total_bp;
    out.first_sub_row_no = first_sub_row_no_;
    active_ = false;
    return out;
}

void sequence_stream_writer::abort() {
    // Transaction rollback reclaims any fragment rows already written; just drop
    // the in-flight buffer + state so a re-begin starts clean.
    buffer_.clear();
    active_ = false;
}

// ---------------------------------------------------------------------------
// DOC0264 §9.4 — fetch_payload streams all fragments into a single
// contiguous pgseq_t* in `out_mctx`. For Phase C sequences this opens the
// shard once via Phase A's cache, opens a random-access BioseqColumnReadState,
// walks all `num_fragments` rows and memcpys each fragment's `data[]` into
// the right offset of the destination varlena's payload region.
//
// Raises bioseq_log<ERR, PG>() if the assembled varlena would exceed MaxAllocSize —
// streaming readers must use `sequence_scanner_pg<S>` instead. The legacy
// callers (auto-hydration of stub-backed scan slots) are bounded by the
// MaxAllocSize gate in `hydrate_seq_columns_in_slot` (§9.4 access_method).
// ---------------------------------------------------------------------------

template<class L, std::size_t M, std::size_t X>
pgseq_t*
sequence_storage_manager<L, M, X>::fetch_payload(
    Oid db_oid, Oid spc_oid, Oid main_relfilenode,
    std::int16_t column_attno, const seq_locator& loc,
    MemoryContext out_mctx, seq_caches* cache)
{
    if (cache == nullptr) {
        bioseq_log<ERR, PG>( "bioseq storage: fetch_payload requires a seq_caches");
    }

    // DOC0264 §6.5 — empty sequence (num_fragments == 0). The locator is the
    // only on-disk artefact; there are no sub-table rows to fetch. Return a
    // minimal empty pgseq_t in `out_mctx`.
    if (loc.num_fragments == 0) {
        MemoryContextScope ctx{out_mctx};
        pgseq_t* out = static_cast<pgseq_t*>(palloc0(pgseq_t::header_bytes()));
        SET_VARSIZE(out, pgseq_t::header_bytes());
        // alphabet/width/bp_size/padding are all zero — caller-side stub
        // hydration sees an empty payload (compact_sequence_range handles
        // size=0 correctly per DOC0258 §5.3).
        return out;
    }

    Relation shard = open_sub_table_for_locator(db_oid, spc_oid, main_relfilenode,
        column_attno, loc, AccessShareLock, cache);
    const std::uint64_t cache_key = seq_caches::key_for(
        column_attno,
        static_cast<std::int16_t>(loc.bucket_idx),
        static_cast<std::int32_t>(loc.shard_idx));
    seq_caches::entry& entry = cache->entries[cache_key];
    entry.rel = shard;

    if (entry.read_state == nullptr) {
        MemoryContext payload_mctx =
            cache->read_state_mctx != nullptr ? cache->read_state_mctx
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

    // DOC0264 §9.4 — derive fragment shape from logical alphabet/width on
    // first sub-table row.
    Datum first_vals[1]  = { 0 };
    bool  first_nulls[1] = { true };
    if (!BioseqColumnReadRowByRowNumber(entry.read_state, loc.first_sub_row_no,
                                    first_vals, first_nulls)) {
        bioseq_log<ERR, PG>(
               "bioseq storage Phase C: missing first sub-table row {} "
               "(locator bucket={} shard={} num_fragments={})",
               loc.first_sub_row_no,
               static_cast<unsigned>(loc.bucket_idx),
               loc.shard_idx, loc.num_fragments);
    }
    if (first_nulls[0]) {
        bioseq_log<ERR, PG>( "bioseq storage Phase C: first sub-table row {} is NULL",
               loc.first_sub_row_no);
    }
    pgseq_t* first_raw = reinterpret_cast<pgseq_t*>(PG_DETOAST_DATUM(first_vals[0]));
    if (is_locator_stub(first_raw)) {
        bioseq_log<ERR, PG>(
               "bioseq storage Phase C: first sub-table row {} is itself a stub (corruption)",
               loc.first_sub_row_no);
    }

    const std::uint8_t logical_alphabet_code =
        static_cast<std::uint8_t>(first_raw->alphabet_code);
    const std::uint8_t logical_byte_width =
        static_cast<std::uint8_t>(first_raw->storage_unit_byte_width);
    if ((logical_byte_width & kLocatorStubMarker) != 0u) {
        bioseq_log<ERR, PG>(
               "bioseq storage Phase C: fragment-row header carries the stub marker "
               "(sub_w={}) — corruption", static_cast<unsigned>(logical_byte_width));
    }

    const auto alpha_code = static_cast<pgseq_t::alphabet_code_t>(logical_alphabet_code);
    const std::size_t lpu = detail::letters_per_unit_for(alpha_code, logical_byte_width);
    if (lpu == 0) {
        bioseq_log<ERR, PG>(
               "bioseq storage Phase C: fetch_payload could not derive letters_per_unit "
               "(alphabet_code={}, byte_width={})",
               static_cast<unsigned>(alpha_code),
               static_cast<unsigned>(logical_byte_width));
    }
    const std::size_t bytes_per_unit = static_cast<std::size_t>(logical_byte_width);
    const std::size_t fragment_storage_bytes =
        detail::kFragmentStorageUnits * bytes_per_unit;

    // Compute the assembled varlena size and guard against MaxAllocSize.
    const sequence_size_t total_units =
        loc.total_bp_size == 0
            ? sequence_size_t{0}
            : (loc.total_bp_size + lpu - 1) / lpu;
    const sequence_size_t total_payload_bytes =
        static_cast<sequence_size_t>(total_units)
        * static_cast<sequence_size_t>(bytes_per_unit);
    const sequence_size_t assembled_varlena_bytes =
        static_cast<sequence_size_t>(pgseq_t::header_bytes())
        + total_payload_bytes;
    if (assembled_varlena_bytes > static_cast<sequence_size_t>(MaxAllocSize)) {
        bioseq_log<ERR, PG>(
               "bioseq storage Phase C: sequence too large to materialise as a "
               "single varlena ({} bp ⇒ ~{} bytes > MaxAllocSize). Stream via "
               "sequence_scanner_pg<S> or wait for Phase D D-VARLENA.",
               loc.total_bp_size,
               assembled_varlena_bytes);
    }

    MemoryContextScope ctx{out_mctx};
    pgseq_t* out = static_cast<pgseq_t*>(palloc0(static_cast<std::size_t>(assembled_varlena_bytes)));
    SET_VARSIZE(out, static_cast<std::size_t>(assembled_varlena_bytes));
    out->alphabet_code           = static_cast<pgseq_t::alphabet_code_t>(logical_alphabet_code);
    out->storage_unit_byte_width = logical_byte_width;
    out->bp_size                 = static_cast<pgseq_t::bp_size_t>(loc.total_bp_size);
    // Last-fragment padding is also the padding of the assembled varlena.

    std::uint8_t* const out_data = reinterpret_cast<std::uint8_t*>(out->data);

    // Copy the already-fetched first fragment.
    const std::size_t first_payload_bytes =
        static_cast<std::size_t>(VARSIZE(first_raw)) - pgseq_t::header_bytes();
    const std::size_t first_copy_bytes =
        std::min(first_payload_bytes,
                 static_cast<std::size_t>(total_payload_bytes));
    if (first_copy_bytes > 0) {
        std::memcpy(out_data, first_raw->data, first_copy_bytes);
    }
    if (loc.num_fragments > 0) {
        out->padding_letters = static_cast<std::uint16_t>(first_raw->padding_letters);
    }

    for (std::uint32_t fragment_no = 1; fragment_no < loc.num_fragments; ++fragment_no) {
        // DOC0276 — full-hydration of a multi-Gbp target into one varlena walks
        // tens of thousands of fragment rows. Make it cancellable and observable.
        CHECK_FOR_INTERRUPTS();
        if (enable_logging && (fragment_no & 0x3FFF) == 0) {
            bioseq_log_progress<DBG>("[fetch_payload] hydrated {}/{} fragments",
                            fragment_no, loc.num_fragments);
        }
        const std::uint64_t row_no = loc.first_sub_row_no + fragment_no;
        Datum vals[1]  = { 0 };
        bool  nulls[1] = { true };
        if (!BioseqColumnReadRowByRowNumber(entry.read_state, row_no, vals, nulls)) {
            bioseq_log<ERR, PG>(
                   "bioseq storage Phase C: missing sub-table fragment row {} "
                   "(locator bucket={} shard={} fragment={}/{})",
                   row_no,
                   static_cast<unsigned>(loc.bucket_idx),
                   loc.shard_idx, fragment_no, loc.num_fragments);
        }
        if (nulls[0]) {
            bioseq_log<ERR, PG>( "bioseq storage Phase C: sub-table row {} is NULL", row_no);
        }
        pgseq_t* raw = reinterpret_cast<pgseq_t*>(PG_DETOAST_DATUM(vals[0]));
        if (is_locator_stub(raw)) {
            bioseq_log<ERR, PG>(
                   "bioseq storage Phase C: sub-table fragment row {} is itself a stub",
                   row_no);
        }
        const std::size_t dst_offset =
            static_cast<std::size_t>(fragment_no) * fragment_storage_bytes;
        const std::size_t raw_payload =
            static_cast<std::size_t>(VARSIZE(raw)) - pgseq_t::header_bytes();
        const std::size_t copy_bytes =
            std::min(raw_payload,
                     static_cast<std::size_t>(total_payload_bytes) - dst_offset);
        if (copy_bytes > 0) {
            std::memcpy(out_data + dst_offset, raw->data, copy_bytes);
        }
        if (fragment_no + 1 == loc.num_fragments) {
            out->padding_letters = static_cast<std::uint16_t>(raw->padding_letters);
        }
    }

    return out;
}

// ---------------------------------------------------------------------------
// §7.7 — bulk fetch.
// ---------------------------------------------------------------------------

template<class L, std::size_t M, std::size_t X>
void
sequence_storage_manager<L, M, X>::fetch_payload_bulk(
    Oid db_oid, Oid spc_oid, Oid main_relfilenode,
    std::int16_t column_attno, std::int16_t bucket_idx,
    std::span<const fetch_request> requests,
    std::span<pgseq_t*>            out_payloads,
    std::span<fetch_request>       sort_scratch,
    MemoryContext                  out_mctx,
    seq_caches*                    cache)
{
    (void)bucket_idx;  // The bucket is implicit in each locator; carried for symmetry.
    if (requests.empty()) {
        return;
    }
    if (sort_scratch.size() < requests.size()) {
        bioseq_log<ERR, PG>(
               "bioseq storage: fetch_payload_bulk sort_scratch too small "
               "(have {} need {})", sort_scratch.size(), requests.size());
    }
    std::copy(requests.begin(), requests.end(), sort_scratch.begin());

    std::sort(sort_scratch.begin(), sort_scratch.begin() + requests.size(),
        [](const fetch_request& a, const fetch_request& b) noexcept {
            return std::tie(a.locator.shard_idx, a.locator.first_sub_row_no)
                 < std::tie(b.locator.shard_idx, b.locator.first_sub_row_no);
        });

    for (std::size_t i = 0; i < requests.size(); ++i) {
        const fetch_request& req = sort_scratch[i];
        out_payloads[req.out_slot] = fetch_payload(
            db_oid, spc_oid, main_relfilenode,
            column_attno, req.locator, out_mctx, cache);
    }
}

// ---------------------------------------------------------------------------
// §7.8 — DROP cascade.
// ---------------------------------------------------------------------------

template<class L, std::size_t M, std::size_t X>
void
sequence_storage_manager<L, M, X>::drop_sub_tables_for_main(
    Oid db_oid, Oid spc_oid, Oid main_relfilenode)
{
    const auto rows = SubTableManager::list_shards_for_main(db_oid, spc_oid, main_relfilenode);
    if (rows.empty()) {
        return;
    }

    constexpr std::size_t kDropBatchSize = 32;
    StringInfo buf = makeStringInfo();
    for (std::size_t i = 0; i < rows.size(); i += kDropBatchSize) {
        resetStringInfo(buf);
        appendStringInfoString(buf, "DROP TABLE IF EXISTS ");
        const std::size_t end = std::min(i + kDropBatchSize, rows.size());
        bool wrote_one = false;
        for (std::size_t j = i; j < end; ++j) {
            if (wrote_one) appendStringInfoString(buf, ", ");
            const std::string sub_name = format_sub_table_name(
                rows[j].database_oid, rows[j].tablespace_oid,
                rows[j].main_relfilenode, rows[j].column_attno,
                rows[j].bucket_idx, rows[j].shard_idx);
            appendStringInfo(buf, "bioseq_internal.%s",
                             quote_identifier(sub_name.c_str()));
            wrote_one = true;
        }
        appendStringInfoString(buf, ";");
        if (SPI_connect() == SPI_OK_CONNECT) {
            SPI_execute(buf->data, false, 0);
            SPI_finish();
        }
    }

    for (std::size_t i = 0; i < rows.size(); i += kDropBatchSize) {
        resetStringInfo(buf);
        appendStringInfoString(buf, "DROP TABLE IF EXISTS ");
        const std::size_t end = std::min(i + kDropBatchSize, rows.size());
        bool wrote_one = false;
        for (std::size_t j = i; j < end; ++j) {
            if (wrote_one) appendStringInfoString(buf, ", ");
            const std::string sidecar_name = format_sidecar_name(
                rows[j].database_oid, rows[j].tablespace_oid,
                rows[j].main_relfilenode, rows[j].column_attno,
                rows[j].bucket_idx, rows[j].shard_idx);
            appendStringInfo(buf, "bioseq_internal.%s",
                             quote_identifier(sidecar_name.c_str()));
            wrote_one = true;
        }
        appendStringInfoString(buf, ";");
        if (SPI_connect() == SPI_OK_CONNECT) {
            SPI_execute(buf->data, false, 0);
            SPI_finish();
        }
    }

    SubTableManager::delete_shards_for_main(db_oid, spc_oid, main_relfilenode);
}

// Explicit instantiation of the default alias so non-template TUs see the
// out-of-line definitions.
template struct sequence_storage_manager<sequence_size_t, 8, 42>;

// ---------------------------------------------------------------------------
// §7.10 — Per-shard sidecar I/O.
// ---------------------------------------------------------------------------

static Oid lookup_sidecar_relid(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                                std::int16_t column_attno,
                                std::int16_t bucket_idx, std::int32_t shard_idx)
{
    auto row = SubTableManager::lookup_shard(db_oid, spc_oid, main_relfilenode,
                                   column_attno, bucket_idx, shard_idx);
    if (!row.has_value()) {
        return InvalidOid;
    }
    return row->sidecar_relid;
}

void sidecar_write_blob(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                        std::int16_t column_attno, const seq_locator& loc,
                        std::span<const std::uint8_t> blob, seq_caches* /*cache*/)
{
    const Oid sidecar_relid = lookup_sidecar_relid(
        db_oid, spc_oid, main_relfilenode, column_attno,
        static_cast<std::int16_t>(loc.bucket_idx),
        static_cast<std::int32_t>(loc.shard_idx));
    if (!OidIsValid(sidecar_relid)) {
        return;
    }
    Relation rel = try_safe_relation_open(sidecar_relid, RowExclusiveLock);
    if (rel == nullptr) {
        return;
    }
    bytea* blob_varlena = reinterpret_cast<bytea*>(
        palloc(VARHDRSZ + blob.size()));
    SET_VARSIZE(blob_varlena, VARHDRSZ + blob.size());
    if (!blob.empty()) {
        std::memcpy(VARDATA(blob_varlena), blob.data(), blob.size());
    }
    TupleDesc td = RelationGetDescr(rel);
    Datum values[2] = {
        Int64GetDatum(static_cast<std::int64_t>(loc.first_sub_row_no)),
        PointerGetDatum(blob_varlena),
    };
    bool nulls[2] = { false, false };
    HeapTuple tup = heap_form_tuple(td, values, nulls);
    CatalogTupleInsert(rel, tup);
    heap_freetuple(tup);
    pfree(blob_varlena);
    relation_close(rel, RowExclusiveLock);
}

std::optional<std::vector<char>>
sidecar_read_blob(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                  std::int16_t column_attno, const seq_locator& loc,
                  seq_caches* /*cache*/)
{
    const Oid sidecar_relid = lookup_sidecar_relid(
        db_oid, spc_oid, main_relfilenode, column_attno,
        static_cast<std::int16_t>(loc.bucket_idx),
        static_cast<std::int32_t>(loc.shard_idx));
    if (!OidIsValid(sidecar_relid)) {
        return std::nullopt;
    }
    Relation rel = try_safe_relation_open(sidecar_relid, AccessShareLock);
    if (rel == nullptr) {
        return std::nullopt;
    }
    const Oid pk_index_oid = find_primary_key_oid(rel);

    ScanKeyData keys[1];
    ScanKeyInit(&keys[0], 1 /*sub_rowno*/,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(loc.first_sub_row_no)));
    SysScanDesc scan = systable_beginscan(rel, pk_index_oid,
                                          /*indexOK=*/ pk_index_oid != InvalidOid,
                                          /*snapshot=*/nullptr, 1, keys);
    std::optional<std::vector<char>> out;
    HeapTuple tup = systable_getnext(scan);
    if (HeapTupleIsValid(tup)) {
        bool is_null = false;
        Datum d = heap_getattr(tup, 2 /*blob*/, RelationGetDescr(rel), &is_null);
        if (!is_null) {
            bytea* flat = reinterpret_cast<bytea*>(PG_DETOAST_DATUM(d));
            const std::size_t data_size = VARSIZE_ANY_EXHDR(flat);
            std::vector<char> v(data_size);
            if (data_size > 0) {
                std::memcpy(v.data(), VARDATA_ANY(flat), data_size);
            }
            out = std::move(v);
            if (reinterpret_cast<void*>(flat) !=
                reinterpret_cast<void*>(DatumGetPointer(d))) {
                pfree(flat);
            }
        }
    }
    systable_endscan(scan);
    relation_close(rel, AccessShareLock);
    return out;
}

void sidecar_read_blob_bulk(
    Oid db_oid, Oid spc_oid, Oid main_relfilenode,
    std::int16_t column_attno, std::int16_t /*bucket_idx*/,
    std::span<const sequence_storage_manager<sequence_size_t, 8, 42>::fetch_request> requests,
    std::span<std::optional<std::vector<char>>> out_blobs,
    std::span<sequence_storage_manager<sequence_size_t, 8, 42>::fetch_request> sort_scratch,
    MemoryContext /*out_mctx*/,
    seq_caches* cache)
{
    if (requests.empty()) {
        return;
    }
    if (sort_scratch.size() < requests.size()) {
        bioseq_log<ERR, PG>(
               "bioseq storage: sidecar_read_blob_bulk sort_scratch too small "
               "(have {} need {})", sort_scratch.size(), requests.size());
    }
    std::copy(requests.begin(), requests.end(), sort_scratch.begin());
    using FR = sequence_storage_manager<sequence_size_t, 8, 42>::fetch_request;
    std::sort(sort_scratch.begin(), sort_scratch.begin() + requests.size(),
        [](const FR& a, const FR& b) noexcept {
            return std::tie(a.locator.shard_idx, a.locator.first_sub_row_no)
                 < std::tie(b.locator.shard_idx, b.locator.first_sub_row_no);
        });
    for (std::size_t i = 0; i < requests.size(); ++i) {
        const FR& req = sort_scratch[i];
        out_blobs[req.out_slot] = sidecar_read_blob(
            db_oid, spc_oid, main_relfilenode,
            column_attno, req.locator, cache);
    }
}

// ---------------------------------------------------------------------------
// §11.3 — Orphan reaper.
// ---------------------------------------------------------------------------

void ReapOrphanSeqSubTables() {
    // Two-direction sweep: catalog rows pointing at missing pg_class entries,
    // and pg_class entries matching our naming convention with no catalog row.
    if (SPI_connect() != SPI_OK_CONNECT) {
        return;
    }
    const char* del_orphan_rows =
        "DELETE FROM bioseq_internal.seq_sub_table s "
        "WHERE s.database_oid = $1 "
        "  AND NOT EXISTS (SELECT 1 FROM pg_class c WHERE c.oid = s.sub_table_relid);";
    Oid arg_types[1] = { OIDOID };
    Datum arg_values[1] = { ObjectIdGetDatum(MyDatabaseId) };
    SPI_execute_with_args(del_orphan_rows, 1, arg_types, arg_values,
                           /*Nulls=*/nullptr,
                           /*read_only=*/false, /*tcount=*/0);

    // pg_class entries with no catalog row — drop them.
    const char* find_orphan_relations =
        "SELECT n.nspname || '.' || quote_ident(c.relname) "
        "FROM pg_class c JOIN pg_namespace n ON c.relnamespace = n.oid "
        "WHERE n.nspname = 'bioseq_internal' "
        "  AND (c.relname LIKE 'seq_sub_%' OR c.relname LIKE 'idx_sidecar_%') "
        "  AND NOT EXISTS ("
        "      SELECT 1 FROM bioseq_internal.seq_sub_table s "
        "      WHERE s.sub_table_relid = c.oid OR s.sidecar_relid = c.oid);";
    if (SPI_execute(find_orphan_relations, /*read_only=*/true, /*tcount=*/0)
        == SPI_OK_SELECT)
    {
        const uint64_t n = SPI_processed;
        std::vector<std::string> to_drop;
        to_drop.reserve(n);
        for (uint64_t r = 0; r < n; ++r) {
            bool is_null = false;
            Datum d = SPI_getbinval(SPI_tuptable->vals[r],
                                    SPI_tuptable->tupdesc, 1, &is_null);
            if (is_null) continue;
            text* t = DatumGetTextPP(d);
            to_drop.emplace_back(VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t));
        }
        for (const auto& q : to_drop) {
            StringInfo drop_stmt = makeStringInfo();
            appendStringInfo(drop_stmt, "DROP TABLE IF EXISTS %s;", q.c_str());
            SPI_execute(drop_stmt->data, /*read_only=*/false, /*tcount=*/0);
        }
    }
    SPI_finish();
}

}  // namespace bioseq::storage
