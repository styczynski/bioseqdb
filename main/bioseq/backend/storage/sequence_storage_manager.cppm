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
#include "columnar/columnar.h"

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

#include <boost/container/flat_map.hpp>  // DOC0260 R14 — seq_caches::entries

export module bioseq_storage:sequence_storage_manager;

import mlogging;
import pg_utils;
import storage_metadata;
import bioseq_table_io;
import bioseq_types.pg_compact_sequence;
import bioseq_sequences;
import bioseq_storage_size;  // DOC0263 §2.2 — standalone module (was :sequence_size)
// DOC0260 §12.1 — the PG-free bucket math + seq_locator POD live in their own
// module so the unit-test TU can link them without the Postgres backend.
// Re-exported here so existing `import bioseq_storage` consumers are unchanged.
export import bioseq_bucket_math;

static_assert(__cplusplus >= 202302L,
              "DOC0260 requires C++23-or-later (project default: C++26)");

export namespace bioseq::storage {

// ---------------------------------------------------------------------------
// §5 — Locator stub format. A 40-byte pgseq_t-shaped varlena that lives in
// the main columnar relation in place of the full sequence payload.
// ---------------------------------------------------------------------------

constexpr std::uint8_t kLocatorStubMarker = 0x80;

// DOC0260 §12.1 — `kLocatorVersion` and the `seq_locator` POD (with its
// 24-byte / offset static_asserts) now live in the PG-free `bioseq_bucket_math`
// module so the unit-test TU can link them; they are re-exported above via
// `export import bioseq_bucket_math`, so every use of `seq_locator` /
// `kLocatorVersion` below resolves unchanged.

constexpr std::size_t kLocatorStubVarlenaBytes =
    pgseq_t::header_bytes() + sizeof(seq_locator);
static_assert(kLocatorStubVarlenaBytes == 40,
              "Phase A locator stub must be exactly 40 bytes (DOC0260 §5.3).");

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
        pg_log(ERROR,
               "bioseq storage: decode_locator_from_stub called on a "
               "non-stub value (sub_w={}). Caller must guard with "
               "is_locator_stub(hdr) first.",
               static_cast<unsigned>(hdr->storage_unit_byte_width));
    }
    seq_locator out{};
    std::memcpy(&out, hdr->data, sizeof(out));
    if (out.version != kLocatorVersion) {
        pg_log(ERROR,
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
// §7 — Per-scan / per-batch relation cache. Owned by ColumnarScanDescData,
// IndexFetchColumnarData, ColumnarWriteState and TupleDispatchBuffer.
// Always allocated separately (`palloc(sizeof(seq_caches)) + placement-new`)
// because std::map's non-standard-layout would otherwise spoil the surrounding
// POD struct's std::is_standard_layout_v guarantee.
// ---------------------------------------------------------------------------

struct seq_caches {
    struct entry {
        Relation rel = nullptr;
        ColumnarWriteState* write_state = nullptr;
        ColumnarReadState*  read_state  = nullptr;
        sequence_size_t pending_bytes_delta = 0;
        sequence_size_t pending_total_payload_delta = 0;
    };

    // DOC0260 R14 — boost::container::flat_map (a sorted vector) rather than
    // std::map: no per-node allocation, deterministic sorted iteration order
    // for close_all, cache-friendly for the typical <50-entry working set, and
    // safe to default-construct in a palloc-backed POD. The hot paths only do
    // find / operator[] / clear, and never hold an `entry&` across an insert
    // into `entries`, so flat_map's reference-invalidation-on-insert is a
    // non-issue here.
    boost::container::flat_map<std::uint64_t, entry> entries{};
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
// dependent consumers (ColumnarScanDescData, ColumnarWriteState, ...)
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

}  // namespace bioseq::storage

// DOC0260 Phase A — `extern "C++"` shim called from write_state_management
// at write-state teardown. Lives at file scope and is `export`-ed so
// importers (write_state_management.cppm) can name it directly without
// forcing a forward declaration that would otherwise be module-attached
// to the importer (and so wouldn't link against this definition).
export extern "C++" void
bioseq_storage_seq_caches_close_all(bioseq::storage::seq_caches* caches,
                                    bool commit) noexcept;

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
    [[nodiscard]] static seq_locator
    insert_payload(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                   std::int16_t column_attno, TupleDesc parent_tupdesc,
                   const pgseq_t* original_hdr,
                   sequence_size_t current_cap_bytes,
                   seq_caches* cache);

    // Read path: single payload.
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
        pg_log(ERROR, "bioseq storage: column_attno {} out of bounds (natts={})",
               column_attno, parent_tupdesc->natts);
    }
    Form_pg_attribute attr = TupleDescAttr(parent_tupdesc, column_attno - 1);
    const std::int32_t typmod = attr->atttypmod;
    if (typmod < 0) {
        pg_log(ERROR, "bioseq storage: SEQ column attno={} has no typmod; "
               "Phase A requires every bioseq.seq column to carry an "
               "(alphabet, width) typmod",
               column_attno);
    }
    const compact_sequence_stdc_type_params params =
        compact_sequence_stdc_type_params::decode(typmod);
    std::string_view name_sv = pgseq_t::name_of_alphabet_code(params.alphabet_code);
    if (name_sv == std::string_view{"?"}) {
        pg_log(ERROR, "bioseq storage: alphabet_code={} not in pgseq_supported_alphabets",
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

extern "C++" void
bioseq_storage_seq_caches_close_all(bioseq::storage::seq_caches* caches,
                                    bool commit) noexcept {
    if (caches != nullptr) {
        caches->close_all(commit);
    }
}

namespace bioseq::storage {

void destroy_seq_caches(seq_caches* caches) noexcept {
    if (caches == nullptr) {
        return;
    }
    caches->~seq_caches();
    pfree(caches);
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
                UpdateSubTableShardBytesWritten(
                    main_database_oid, main_tablespace_oid, main_relfilenode,
                    column_attno, bucket_idx, shard_idx,
                    e.pending_bytes_delta,
                    cap_bytes_snapshot);
            }
            e.pending_bytes_delta = 0;
            e.pending_total_payload_delta = 0;
            // We deliberately do NOT call ColumnarEndWrite here — the
            // sub-table WriteState lives in the central WriteStateCache and is
            // committed alongside the parent's WriteState at transaction
            // commit. Re-closing here would double-flush.
            e.write_state = nullptr;
        } else {
            if (e.read_state != nullptr) {
                ColumnarEndRead(e.read_state);
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

// ---------------------------------------------------------------------------
// Shard open / create / lookup helpers.
// ---------------------------------------------------------------------------

[[nodiscard]] static Relation
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
        //     ColumnarWriteState and is closed by
        //     ColumnarPopWriteStateForAllRels at xact commit, under
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
            pg_log(ERROR, "bioseq storage: failed to open sub-table relid {}",
                   static_cast<unsigned>(sub_table_relid));
        }
        seq_caches::entry& entry = cache->entries[key];
        entry.rel = rel;
        return rel;
    }
    Relation rel = try_safe_relation_open(sub_table_relid, lock_mode);
    if (rel == nullptr) {
        pg_log(ERROR, "bioseq storage: failed to open sub-table relid {}",
               static_cast<unsigned>(sub_table_relid));
    }
    return rel;
}

[[nodiscard]] static Relation
allocate_new_shard_and_open(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                            std::int16_t column_attno, TupleDesc parent_tupdesc,
                            std::int16_t bucket_idx, std::int32_t starting_shard_idx,
                            LOCKMODE lock_mode, seq_caches* cache)
{
    const std::int64_t lock_key =
        shard_alloc_lock_key(db_oid, main_relfilenode, column_attno, bucket_idx);
    DirectFunctionCall1(pg_advisory_xact_lock_int8, Int64GetDatum(lock_key));

    if (auto rechecked = LookupActiveSubTableShard(
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
    // tracker for cleanup (ColumnarTableDropHook /
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
            pg_log(ERROR, "bioseq storage: SPI_connect failed while allocating shard");
        }
        // SPI_execute returns the result code of the LAST command in a multi-
        // statement string. Our DDL is two CREATE TABLEs, so the expected
        // return is SPI_OK_UTILITY. Accept SPI_OK_SELECT too as defensive
        // back-compat in case a SELECT clause is added later.
        const int rc = SPI_execute(ddl->data, /*read_only=*/false, /*tcount=*/0);
        if (rc != SPI_OK_UTILITY && rc != SPI_OK_SELECT) {
            pg_log(ERROR,
                   "bioseq storage: failed to create sub-table {} + sidecar {} "
                   "(SPI rc={})",
                   sub_name, sidecar_name, rc);
        }
        SPI_finish();

        const Oid bioseq_internal_ns = get_namespace_oid("bioseq_internal", false);
        new_sub_relid     = get_relname_relid(sub_name.c_str(),     bioseq_internal_ns);
        new_sidecar_relid = get_relname_relid(sidecar_name.c_str(), bioseq_internal_ns);
        if (!OidIsValid(new_sub_relid) || !OidIsValid(new_sidecar_relid)) {
            pg_log(ERROR, "bioseq storage: sub-table or sidecar not found after CREATE");
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

    SubTableRow row = InsertSubTableShard(db_oid, spc_oid, main_relfilenode,
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

[[nodiscard]] static Relation
open_or_create_shard(Oid db_oid, Oid spc_oid, Oid main_relfilenode,
                     std::int16_t column_attno, TupleDesc parent_tupdesc,
                     std::int16_t bucket_idx,
                     LOCKMODE lock_mode, seq_caches* cache)
{
    if (auto active = LookupActiveSubTableShard(
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
    for (const auto& r : ListSubTableShardsForMain(
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

[[nodiscard]] static Relation
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
    auto row = LookupSubTableShard(db_oid, spc_oid, main_relfilenode,
        column_attno, static_cast<std::int16_t>(loc.bucket_idx),
        static_cast<std::int32_t>(loc.shard_idx));
    if (!row.has_value()) {
        pg_log(ERROR,
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
// §7.5 — insert_payload (write hot path).
// ---------------------------------------------------------------------------

template<class L, std::size_t M, std::size_t X>
seq_locator
sequence_storage_manager<L, M, X>::insert_payload(
    Oid db_oid, Oid spc_oid, Oid main_relfilenode,
    std::int16_t column_attno, TupleDesc parent_tupdesc,
    const pgseq_t* original_hdr,
    sequence_size_t current_cap_bytes,
    seq_caches* cache)
{
    if (original_hdr == nullptr) {
        pg_log(ERROR, "bioseq storage: insert_payload received null original_hdr");
    }
    if (is_locator_stub(original_hdr)) {
        pg_log(ERROR, "bioseq storage: insert_payload received a stub; the "
               "caller must screen out stubs before re-inserting");
    }
    if (cache == nullptr || cache->side != seq_caches::Side::Write) {
        pg_log(ERROR, "bioseq storage: insert_payload requires a write-side seq_caches");
    }
    if (cache->main_database_oid == InvalidOid) {
        cache->main_database_oid   = db_oid;
        cache->main_tablespace_oid = spc_oid;
        cache->main_relfilenode    = main_relfilenode;
    }

    const sequence_size_t total_bp_size =
        static_cast<sequence_size_t>(original_hdr->bp_size);
    const sequence_size_t total_varlena_bytes =
        static_cast<sequence_size_t>(VARSIZE(original_hdr));

    if (total_varlena_bytes > static_cast<sequence_size_t>(MaxAllocSize)) {
        pg_log(ERROR,
               "bioseq storage Phase A: sequence varlena size {} bytes exceeds "
               "MaxAllocSize ({}). Phase A stores each sequence as a single "
               "sub-table row; the ~4.3 Gbp ceiling is documented in DOC0260 "
               "§1.2. Phase C lifts the cap via per-fragment row layout. See "
               "DOC0264 (Phase C) and DOC0267 D-VARLENA.",
               total_varlena_bytes,
               static_cast<sequence_size_t>(MaxAllocSize));
    }

    const std::int16_t bucket_idx =
        static_cast<std::int16_t>(bucket_index_for_size(total_bp_size));

    std::int32_t shard_idx = 0;
    Relation shard = open_or_create_shard(db_oid, spc_oid, main_relfilenode,
                                          column_attno, parent_tupdesc, bucket_idx,
                                          RowExclusiveLock, cache);

    // Recheck — we may need to roll over to a new shard.
    auto active = LookupActiveSubTableShard(db_oid, spc_oid, main_relfilenode,
                                            column_attno, bucket_idx);
    if (!active.has_value()) {
        pg_log(ERROR, "bioseq storage: active shard disappeared mid-insert");
    }
    const std::uint64_t active_key =
        seq_caches::key_for(column_attno, bucket_idx, active->shard_idx);
    sequence_size_t pending = 0;
    if (auto it = cache->entries.find(active_key); it != cache->entries.end()) {
        pending = it->second.pending_bytes_delta;
    }
    const sequence_size_t projected =
        active->bytes_written + pending + total_varlena_bytes;
    // DOC0260 §10.2 — cap=0 sentinel forces a fresh shard per insert, but
    // only AFTER we have at least one row in the active shard. Otherwise
    // we would emit N+1 shards (one empty leading shard + N data shards)
    // for N inserts, breaking the §12.2 ShardPerRow.WhenCapIsZero test.
    const bool rollover_required =
        active->bytes_written + pending > 0
        && (current_cap_bytes == 0 || projected > current_cap_bytes);

    if (rollover_required) {
        // Flush any pending delta on the current shard before marking it full
        // (so the catalog's bytes_written reflects reality).
        if (auto it = cache->entries.find(active_key);
            it != cache->entries.end() && it->second.pending_bytes_delta != 0)
        {
            UpdateSubTableShardBytesWritten(
                db_oid, spc_oid, main_relfilenode,
                column_attno, bucket_idx, active->shard_idx,
                it->second.pending_bytes_delta,
                current_cap_bytes);
            it->second.pending_bytes_delta = 0;
            it->second.pending_total_payload_delta = 0;
        } else {
            // Mark the current shard full so subsequent backends roll over too.
            UpdateSubTableShardBytesWritten(
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

    Datum values[1] = { PointerGetDatum(original_hdr) };
    bool  nulls[1]  = { false };
    const std::uint64_t row_no = ColumnarWriteRow(entry.write_state, values, nulls);

    entry.pending_bytes_delta         += total_varlena_bytes;
    entry.pending_total_payload_delta += total_varlena_bytes;

    seq_locator out{};
    out.version       = kLocatorVersion;
    out.bucket_idx    = static_cast<std::uint8_t>(bucket_idx);
    out._pad_2        = 0;
    out.shard_idx     = static_cast<std::uint32_t>(shard_idx);
    out.total_bp_size = total_bp_size;
    out.sub_row_no    = row_no;
    return out;
}

// ---------------------------------------------------------------------------
// §7.6 — fetch_payload (read hot path).
// ---------------------------------------------------------------------------

template<class L, std::size_t M, std::size_t X>
pgseq_t*
sequence_storage_manager<L, M, X>::fetch_payload(
    Oid db_oid, Oid spc_oid, Oid main_relfilenode,
    std::int16_t column_attno, const seq_locator& loc,
    MemoryContext out_mctx, seq_caches* cache)
{
    if (cache == nullptr) {
        pg_log(ERROR, "bioseq storage: fetch_payload requires a seq_caches");
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
        entry.read_state = ColumnarBeginRead(shard, td, projected.release(),
                                             /*whereClauseList=*/NIL,
                                             payload_mctx, snap,
                                             /*randomAccess=*/true, nullptr);
        attr_needed.free();
    }

    Datum vals[1]  = { 0 };
    bool  nulls[1] = { true };
    if (!ColumnarReadRowByRowNumber(entry.read_state, loc.sub_row_no, vals, nulls)) {
        pg_log(ERROR,
               "bioseq storage: missing sub-table row {} (locator bucket={} shard={})",
               loc.sub_row_no,
               static_cast<unsigned>(loc.bucket_idx),
               loc.shard_idx);
    }
    if (nulls[0]) {
        pg_log(ERROR, "bioseq storage: sub-table row {} is NULL", loc.sub_row_no);
    }
    pgseq_t* raw = reinterpret_cast<pgseq_t*>(PG_DETOAST_DATUM(vals[0]));
    if (is_locator_stub(raw)) {
        pg_log(ERROR,
               "bioseq storage: sub-table row {} is itself a stub (corruption)",
               loc.sub_row_no);
    }
    MemoryContextScope ctx{out_mctx};
    const std::size_t raw_bytes = static_cast<std::size_t>(VARSIZE(raw));
    pgseq_t* copy = static_cast<pgseq_t*>(palloc(raw_bytes));
    std::memcpy(copy, raw, raw_bytes);
    return copy;
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
        pg_log(ERROR,
               "bioseq storage: fetch_payload_bulk sort_scratch too small "
               "(have {} need {})", sort_scratch.size(), requests.size());
    }
    std::copy(requests.begin(), requests.end(), sort_scratch.begin());

    std::sort(sort_scratch.begin(), sort_scratch.begin() + requests.size(),
        [](const fetch_request& a, const fetch_request& b) noexcept {
            return std::tie(a.locator.shard_idx, a.locator.sub_row_no)
                 < std::tie(b.locator.shard_idx, b.locator.sub_row_no);
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
    const auto rows = ListSubTableShardsForMain(db_oid, spc_oid, main_relfilenode);
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

    DeleteSubTableShardsForMain(db_oid, spc_oid, main_relfilenode);
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
    auto row = LookupSubTableShard(db_oid, spc_oid, main_relfilenode,
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
        Int64GetDatum(static_cast<std::int64_t>(loc.sub_row_no)),
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
                Int64GetDatum(static_cast<std::int64_t>(loc.sub_row_no)));
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
        pg_log(ERROR,
               "bioseq storage: sidecar_read_blob_bulk sort_scratch too small "
               "(have {} need {})", sort_scratch.size(), requests.size());
    }
    std::copy(requests.begin(), requests.end(), sort_scratch.begin());
    using FR = sequence_storage_manager<sequence_size_t, 8, 42>::fetch_request;
    std::sort(sort_scratch.begin(), sort_scratch.begin() + requests.size(),
        [](const FR& a, const FR& b) noexcept {
            return std::tie(a.locator.shard_idx, a.locator.sub_row_no)
                 < std::tie(b.locator.shard_idx, b.locator.sub_row_no);
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
