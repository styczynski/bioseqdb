/**
 * @file index_chunked_storage_pg.cppm
 * @brief DOC0279 §8 — the concrete Postgres storage implementations that satisfy
 *        the `is_index_chunked_storage_for<...>` concepts.
 *
 * Index chunks are raw BYTEA tuples in heap relations (heap_form_tuple /
 * CatalogTupleInsert + systable_beginscan), bypassing columnarisation
 * (Invariant C). The per-sequence layouts co-locate with the sequence's
 * `(bucket, shard)`; the table layout shards via `index_table_shard`.
 */
module;
#include "common.h"
#include "columnar/columnar.h"   // ColumnarReadState + columnar reader decls
#include "access/relation.h"
#include "access/table.h"
#include "access/heapam.h"
#include "catalog/namespace.h"   // RelnameGetRelid
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <queue>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
export module bioseq_storage_indexing_pg;
import mlogging;
import pg_utils;                       // try_safe_relation_open, find_primary_key_oid
import bioseq_storage;                 // seq_caches, seq_locator, format helpers
import bioseq_bucket_math;             // seq_locator
import bioseq_storage_size;            // sequence_size_t
import bioseq_indexing.type;           // SequenceIndexType, index_sub_key_t, MAX_INDEX_CHUNK_SIZE_BYTES, sentinels
import bioseq_indexing.storage;        // the concepts
import bioseq_indexing.minimizer_hashmap;   // SerializedHeader/Bucket validation
import bioseq_table_io;                // ColumnarBeginRead/ReadNextRow/EndRead/ReadState
import bioseq_storage_scanner_pg;      // with_lazy_postgres, make_scanner_pg_in_memory (for_each_sequence)
import bioseq_storage_lazy;            // lazy_compact_sequence
import bioseq_types.pg_compact_sequence;  // pgseq_t, dispatch_instance
import bioseq_sequences;               // compact_sequence_range
import bioseq_parallelism.memory_admission;
import bioseq_storage_indexing_residency_pg;
import bioseq_storage_indexing_sorted_runs_pg;
import bioseq_instr;
static_assert(__cplusplus >= 202302L, "C++23-or-later");

// Reached via extern "C++" (the bioseq_table:access_method cycle edge), exactly
// as sidecar.cppm / metadata_admin.cppm do.
extern "C++" std::uint64_t ColumnarTableRowCount(Relation relation);
extern "C++" bool IsColumnarTableAmTable(Oid relationId);

export namespace bioseq::storage::indexing {

using bioseq::indexing::SequenceIndexType;
using bioseq::indexing::index_sub_key_t;
using bioseq::indexing::MAX_INDEX_CHUNK_SIZE_BYTES;
using bioseq::indexing::TEMP_RUN_BASE;
using bioseq::indexing::DIRECTORY_SUB_KEY;
using bioseq::indexing::MinimizerHashmapFragmentIndex;
using bioseq::indexing::MINIMIZERS_PER_FRAGMENT_INDEX_NAME;
using bioseq::indexing::MINIMIZERS_PER_SEQUENCE_INDEX_NAME;
using bioseq::indexing::SYMBOL_COUNTS_INDEX_NAME;

// pg-side mirror checks (§5): the macros ARE in scope here.
static_assert(bioseq::indexing::kVarlenaHeaderBytes == VARHDRSZ, "VARHDRSZ mirror drift");
static_assert(bioseq::indexing::kPgMaxAllocSize == MaxAllocSize, "MaxAllocSize mirror drift");

// Backend-local per-sequence build observability (§8.8/DOC0299). Defined here
// (storage) so merge/release can set it; admin zeroes it at build start and
// mod.cppm reads it through SQL in the same backend.
struct LastMpsBuildStats {
    std::uint64_t backend_pid = 0;
    std::uint64_t db_oid = 0;
    std::uint64_t relfilenode = 0;
    std::uint64_t sequence_rowno = 0;
    std::uint64_t total_run_count = 0;
    std::uint64_t resident_runs = 0;
    std::uint64_t file_runs = 0;
    std::uint64_t resident_run_bytes = 0;
    std::uint64_t resident_peak_bytes = 0;
    std::uint64_t spilled_run_bytes = 0;
    std::uint64_t postgres_temp_file_bytes = 0;
    std::uint64_t postgres_temp_file_count = 0;
    std::uint64_t merge_reader_buffer_bytes = 0;
    std::uint64_t merge_pass_count = 0;
    std::uint64_t spill_count_from_pressure = 0;
    std::uint64_t resident_demotions = 0;
    std::uint64_t position_spill_bytes = 0;
    std::uint64_t position_spill_count = 0;
};

inline std::uint64_t g_last_index_merge_run_count = 0;
inline LastMpsBuildStats g_last_mps_build_stats{};

inline void reset_last_mps_build_stats() noexcept {
    g_last_index_merge_run_count = 0;
    g_last_mps_build_stats = {};
}

// Pinned attribute numbers (§8.1) for the CHUNKED_PER_FRAGMENT table.
inline constexpr AttrNumber kAttQueryKey   = 1;
inline constexpr AttrNumber kAttSubKey     = 2;
inline constexpr AttrNumber kAttChunkNo    = 3;
inline constexpr AttrNumber kAttChunkBytes = 4;
inline constexpr AttrNumber kAttBlob       = 5;

// ---- naming helpers ------------------------------------------------------

/// Compile-time-ish abbreviation for an index name (keeps the physical name
/// inside NAMEDATALEN). Unknown names fall back to a stable short hash.
[[nodiscard]] inline std::string short_index_for(std::string_view name) {
    if (name == MINIMIZERS_PER_FRAGMENT_INDEX_NAME) return "mpf";
    if (name == MINIMIZERS_PER_SEQUENCE_INDEX_NAME) return "mps";
    if (name == SYMBOL_COUNTS_INDEX_NAME)           return "sc";
    if (name == "test_table_rowcount")     return "ttr";
    // generic fallback: first 4 chars + length, still deterministic
    std::string out = "ix";
    for (char c : name) { if (out.size() >= 6) break; if ((c>='a'&&c<='z')||(c>='0'&&c<='9')) out.push_back(c); }
    return out;
}

/// Stable 64-bit FNV-1a over the logical identity tuple, hex-rendered.
[[nodiscard]] inline std::uint64_t identity_hash64(Oid db, Oid spc, Oid relfn,
                                                   std::int16_t attno, std::string_view name) {
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&](std::uint64_t v){ for (int i=0;i<8;++i){ h ^= (v & 0xff); h *= 1099511628211ull; v >>= 8; } };
    mix(db); mix(spc); mix(relfn); mix((std::uint64_t)(std::uint16_t)attno);
    for (char c : name) { h ^= (std::uint8_t)c; h *= 1099511628211ull; }
    return h;
}

[[nodiscard]] inline std::string format_seqidx_name(std::string_view name, Oid db, Oid spc, Oid relfn,
        std::int16_t attno, std::uint8_t bucket, std::uint32_t shard) {
    std::string out = fmt::format("seqidx_{}_{:016x}_{}_{}", short_index_for(name),
                                  identity_hash64(db, spc, relfn, attno, name),
                                  (unsigned)bucket, (unsigned)shard);
    if (out.size() >= NAMEDATALEN) bioseq_log<ERR, PG>( "bioseq index: derived name too long ({}): {}", out.size(), out);
    return out;
}
[[nodiscard]] inline std::string format_seqidxtab_name(std::string_view name, Oid db, Oid spc, Oid relfn,
        std::int16_t attno, std::int32_t shard) {
    std::string out = fmt::format("seqidxtab_{}_{:016x}_{}", short_index_for(name),
                                  identity_hash64(db, spc, relfn, attno, name), shard);
    if (out.size() >= NAMEDATALEN) bioseq_log<ERR, PG>( "bioseq index: derived name too long ({}): {}", out.size(), out);
    return out;
}

[[nodiscard]] inline Oid bioseq_internal_ns() { return get_namespace_oid("bioseq_internal", false); }

/// Run a DDL string inside an internal sub-transaction (matches
/// sequence_storage_manager's lazy-CREATE pattern). Returns nothing; throws on error.
inline void run_internal_ddl(const char* ddl) {
    MemoryContext saved_ctx   = CurrentMemoryContext;
    ResourceOwner saved_owner = CurrentResourceOwner;
    BeginInternalSubTransaction("bioseq index DDL");
    MemoryContextSwitchTo(saved_ctx);
    PG_TRY();
    {
        if (SPI_connect() != SPI_OK_CONNECT) bioseq_log<ERR, PG>( "bioseq index: SPI_connect failed");
        const int rc = SPI_execute(ddl, /*read_only=*/false, /*tcount=*/0);
        if (rc != SPI_OK_UTILITY && rc != SPI_OK_SELECT)
            bioseq_log<ERR, PG>( "bioseq index: DDL failed (SPI rc={})", rc);
        SPI_finish();
        ReleaseCurrentSubTransaction();
        MemoryContextSwitchTo(saved_ctx);
        CurrentResourceOwner = saved_owner;
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(saved_ctx);
        RollbackAndReleaseCurrentSubTransaction();
        MemoryContextSwitchTo(saved_ctx);
        CurrentResourceOwner = saved_owner;
        PG_RE_THROW();
    }
    PG_END_TRY();
}

/// Persistence keyword inherited from the user relation (UNLOGGED if parent is).
[[nodiscard]] inline const char* persistence_kw_for(Oid user_relid) {
    char persist = 'p';
    Relation r = try_safe_relation_open(user_relid, AccessShareLock);
    if (r != nullptr) { persist = r->rd_rel->relpersistence; relation_close(r, AccessShareLock); }
    return persist == 'u' ? "UNLOGGED" : "";
}

// =========================================================================
//  IndexStorageFragmentPg  —  CHUNKED_PER_FRAGMENT
// =========================================================================
struct IndexStorageFragmentPg {
    Oid           db_oid_ = InvalidOid, tablespace_oid_ = InvalidOid, main_relfilenode_ = InvalidOid, user_relid_ = InvalidOid;
    seq_caches*   read_caches_ = nullptr;
    std::int16_t  column_attno_ = 0;
    std::string_view index_name_{};
    std::uint8_t  bucket_idx_ = 0;
    std::uint32_t shard_idx_ = 0;
    std::uint64_t query_key_ = 0;
    MemoryContext own_mctx_ = nullptr;
    mutable Oid   index_relid_ = InvalidOid;
    mutable std::int64_t* decoded_ = nullptr;
    mutable std::size_t   decoded_units_ = 0;
    mutable std::size_t   decoded_bytes_ = 0;
    mutable index_sub_key_t decoded_sub_ = DIRECTORY_SUB_KEY;
    mutable std::size_t   decoded_chunk_ = std::numeric_limits<std::size_t>::max();
    struct CachedChunk {
        index_sub_key_t sub_key = DIRECTORY_SUB_KEY;
        std::size_t chunk_no = std::numeric_limits<std::size_t>::max();
        std::size_t payload_bytes = 0;
        std::uint64_t last_used = 0;
        std::vector<char> bytes;
        bioseq::parallelism::MemoryPermit permit;
    };
    mutable std::vector<CachedChunk> chunk_cache_;
    mutable std::uint64_t chunk_cache_clock_ = 0;
    mutable std::size_t chunk_cache_bytes_ = 0;
    static constexpr std::size_t kChunkCacheMaxEntries = 8;
    static constexpr std::size_t kChunkCacheMaxBytes = 256ULL << 20;
    std::int64_t* scratch_ = nullptr;
    std::size_t   scratch_units_ = 0;
    std::size_t   run_buffer_override_bytes_ = 0;
    // per-sub_key byte plan recorded by resize/resize_chunks, consumed by write_to_each.
    std::map<index_sub_key_t, std::vector<std::size_t>> chunk_payload_bytes_;
    std::optional<SortedPairRunStore> sorted_runs_;

    IndexStorageFragmentPg() = default;
    IndexStorageFragmentPg(const IndexStorageFragmentPg&) = delete;
    IndexStorageFragmentPg& operator=(const IndexStorageFragmentPg&) = delete;
    IndexStorageFragmentPg(IndexStorageFragmentPg&& o) noexcept { steal_(std::move(o)); }
    IndexStorageFragmentPg& operator=(IndexStorageFragmentPg&& o) noexcept {
        if (this != &o) { release_(); steal_(std::move(o)); } return *this; }
    ~IndexStorageFragmentPg() { release_(); }

    void steal_(IndexStorageFragmentPg&& o) noexcept {
        db_oid_=o.db_oid_; tablespace_oid_=o.tablespace_oid_; main_relfilenode_=o.main_relfilenode_; user_relid_=o.user_relid_;
        read_caches_=o.read_caches_; column_attno_=o.column_attno_; index_name_=o.index_name_;
        bucket_idx_=o.bucket_idx_; shard_idx_=o.shard_idx_; query_key_=o.query_key_;
        own_mctx_=o.own_mctx_; index_relid_=o.index_relid_;
        decoded_=o.decoded_; decoded_units_=o.decoded_units_; decoded_bytes_=o.decoded_bytes_;
        decoded_sub_=o.decoded_sub_; decoded_chunk_=o.decoded_chunk_;
        chunk_cache_=std::move(o.chunk_cache_);
        chunk_cache_clock_=o.chunk_cache_clock_;
        chunk_cache_bytes_=o.chunk_cache_bytes_;
        scratch_=o.scratch_; scratch_units_=o.scratch_units_;
        run_buffer_override_bytes_=o.run_buffer_override_bytes_;
        chunk_payload_bytes_=std::move(o.chunk_payload_bytes_);
        sorted_runs_=std::move(o.sorted_runs_);
        o.own_mctx_=nullptr; o.decoded_=nullptr; o.scratch_=nullptr; o.index_relid_=InvalidOid;
        o.decoded_units_=0; o.decoded_bytes_=0; o.scratch_units_=0;
        o.chunk_cache_clock_=0; o.chunk_cache_bytes_=0;
    }
    void release_resident_runs_noexcept() noexcept {
        if (sorted_runs_.has_value()) {
            sorted_runs_->release_resident_runs_noexcept();
        }
    }
    void release_() noexcept {
        release_runs_noexcept();
        chunk_cache_.clear();
        chunk_cache_bytes_ = 0;
        if (own_mctx_ != nullptr) {
            MemoryContextDelete(own_mctx_);
            own_mctx_=nullptr;
            decoded_=nullptr;
            scratch_=nullptr;
        }
    }

    [[noreturn]] void index_error(std::string_view msg) const { bioseq_log<ERR, PG>( "bioseq index: {}", msg); abort(); }

    // ---- validators (translate to ereport on the pg side) ----
    [[nodiscard]] MinimizerHashmapFragmentIndex::SerializedHeader validate_minimizer_blob_or_error(
            const char* p, std::size_t bytes, std::optional<std::uint32_t> expected_bucket_count = std::nullopt) const {
        using H = MinimizerHashmapFragmentIndex::SerializedHeader;
        using B = MinimizerHashmapFragmentIndex::SerializedBucket;
        if (bytes < sizeof(H)) index_error("minimizer blob truncated header");
        H h{}; std::memcpy(&h, p, sizeof(H));
        if (h.magic != MinimizerHashmapFragmentIndex::kSerializeMagic || h.version != MinimizerHashmapFragmentIndex::kSerializeVersion)
            index_error("minimizer blob magic/version");
        const std::size_t expect = sizeof(H) + sizeof(B)*h.bucket_count + sizeof(std::uint64_t)*h.total_positions;
        if (bytes != expect) index_error("minimizer blob size mismatch");
        if (expected_bucket_count && h.bucket_count != *expected_bucket_count) index_error("minimizer blob bucket_count mismatch");
        return h;
    }
    void validate_minimizer_directory_or_error(const char* p, std::size_t bytes) const {
        if (bytes < 32) index_error("directory truncated");
        std::uint32_t magic{}, version{}; std::memcpy(&magic, p, 4); std::memcpy(&version, p+4, 4);
        if (magic != 0xB1000002u || version != 1u) index_error("directory magic/version");
    }
    void validate_minimizer_chunk_metadata_or_error(index_sub_key_t sk, std::size_t chunk_no,
            std::size_t min_bytes, std::size_t /*alignment*/) const {
        if (chunk_byte_size(sk, chunk_no) < min_bytes) index_error("chunk metadata below minimum size");
    }

    // ---- relation resolution ----
    [[nodiscard]] std::string this_table_name() const {
        return format_seqidx_name(index_name_, db_oid_, tablespace_oid_, main_relfilenode_, column_attno_, bucket_idx_, shard_idx_);
    }
    [[nodiscard]] Oid resolve_relid_(bool create_if_missing) const {
        if (OidIsValid(index_relid_)) return index_relid_;
        const std::string nm = this_table_name();
        const Oid ns = bioseq_internal_ns();
        Oid rid = get_relname_relid(nm.c_str(), ns);
        if (!OidIsValid(rid) && create_if_missing) {
            const char* pk = persistence_kw_for(user_relid_);
            std::string ddl = fmt::format(
                "CREATE {} TABLE IF NOT EXISTS bioseq_internal.{} ("
                "query_key BIGINT NOT NULL, sub_key BIGINT NOT NULL, chunk_no INT NOT NULL, "
                "chunk_bytes INT NOT NULL, blob BYTEA NOT NULL, "
                "PRIMARY KEY (query_key, sub_key, chunk_no)) USING heap;",
                pk, quote_identifier(nm.c_str()));
            run_internal_ddl(ddl.c_str());
            run_internal_ddl(fmt::format(
                "ALTER TABLE bioseq_internal.{} ALTER COLUMN blob SET STORAGE EXTERNAL;",
                quote_identifier(nm.c_str())).c_str());
            rid = get_relname_relid(nm.c_str(), ns);
            if (!OidIsValid(rid)) index_error("failed to create seqidx table");
        }
        index_relid_ = rid;
        return rid;
    }
    [[nodiscard]] Relation open_index_relation_(bool create_if_missing, LOCKMODE lock) const {
        const Oid rid = resolve_relid_(create_if_missing);
        if (!OidIsValid(rid)) return nullptr;
        Relation rel = try_safe_relation_open(rid, lock);
        if (rel != nullptr && rel->rd_rel->relam != HEAP_TABLE_AM_OID)
            bioseq_log<ERR, PG>( "bioseq index: {} is not a heap-AM table", this_table_name());
        return rel;
    }

    void ensure_buffer_(std::size_t units) const {
        if (decoded_units_ >= units && decoded_ != nullptr) return;
        MemoryContext old = MemoryContextSwitchTo(own_mctx_);
        decoded_ = (decoded_ == nullptr) ? (std::int64_t*)palloc(units*sizeof(std::int64_t))
                                         : (std::int64_t*)repalloc(decoded_, units*sizeof(std::int64_t));
        decoded_units_ = units;
        MemoryContextSwitchTo(old);
    }
    [[nodiscard]] std::int64_t* scratch_buffer_(std::size_t units) {
        if (scratch_units_ < units || scratch_ == nullptr) {
            MemoryContext old = MemoryContextSwitchTo(own_mctx_);
            scratch_ = (scratch_ == nullptr) ? (std::int64_t*)palloc(units*sizeof(std::int64_t))
                                             : (std::int64_t*)repalloc(scratch_, units*sizeof(std::int64_t));
            scratch_units_ = units;
            MemoryContextSwitchTo(old);
        }
        return scratch_;
    }
    [[nodiscard]] std::size_t scratch_buffer_bytes_() const { return scratch_units_ * sizeof(std::int64_t); }

    [[nodiscard]] IndexChunkKey chunk_cache_key_(
            index_sub_key_t sub_key,
            std::size_t chunk_no) const noexcept {
        return IndexChunkKey{
            .db_oid = db_oid_,
            .user_relid = user_relid_,
            .tablespace_oid = tablespace_oid_,
            .main_relfilenode = main_relfilenode_,
            .column_attno = column_attno_,
            .index_name_hash = index_name_hash(
                db_oid_, tablespace_oid_, main_relfilenode_, column_attno_, index_name_),
            .bucket_idx = bucket_idx_,
            .shard_idx = shard_idx_,
            .query_key = query_key_,
            .sub_key = sub_key,
            .chunk_no = static_cast<std::uint32_t>(chunk_no),
        };
    }

    void admit_cached_chunk_(
            index_sub_key_t sub_key,
            std::size_t chunk_no,
            std::span<const char> payload,
            bool written_in_current_xact) const {
        (void)store_cached_chunk(
            chunk_cache_key_(sub_key, chunk_no),
            payload,
            written_in_current_xact);
    }

    void load_chunk_into_buffer_(index_sub_key_t sub_key, std::size_t chunk_no) const {
        const IndexChunkKey key = chunk_cache_key_(sub_key, chunk_no);
        if (auto cached = lookup_cached_chunk(key)) {
            const std::size_t units =
                (cached->size + sizeof(std::int64_t) - 1) / sizeof(std::int64_t);
            ensure_buffer_(units == 0 ? 1 : units);
            if (cached->size > 0) {
                std::memcpy(decoded_, cached->data, cached->size);
            }
            decoded_sub_ = sub_key;
            decoded_chunk_ = chunk_no;
            decoded_bytes_ = cached->size;
            return;
        }
        Relation rel = open_index_relation_(false, AccessShareLock);
        if (rel == nullptr) bioseq_log<ERR, PG>( "bioseq index: chunk ({},{},{}) relation missing",
                                   (long long)query_key_, (long long)sub_key, chunk_no);
        const Oid pk = find_primary_key_oid(rel);
        ScanKeyData k[3];
        ScanKeyInit(&k[0], kAttQueryKey, BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum((int64)query_key_));
        ScanKeyInit(&k[1], kAttSubKey,   BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum((int64)sub_key));
        ScanKeyInit(&k[2], kAttChunkNo,  BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum((int32)chunk_no));
        SysScanDesc s = systable_beginscan(rel, pk, OidIsValid(pk), nullptr, 3, k);
        HeapTuple t = systable_getnext(s);
        if (!HeapTupleIsValid(t)) { systable_endscan(s); relation_close(rel, AccessShareLock);
            bioseq_log<ERR, PG>( "bioseq index chunk ({},{},{}) missing", (long long)query_key_, (long long)sub_key, chunk_no); }
        bool isnull=false; Datum d = heap_getattr(t, kAttBlob, RelationGetDescr(rel), &isnull);
        if (isnull) bioseq_log<ERR, PG>( "bioseq index chunk blob NULL");
        bytea* raw = reinterpret_cast<bytea*>(PG_DETOAST_DATUM_PACKED(d));
        const std::size_t payload = VARSIZE_ANY_EXHDR(raw);
        const std::size_t units   = (payload + sizeof(std::int64_t) - 1) / sizeof(std::int64_t);
        ensure_buffer_(units == 0 ? 1 : units);
        if (payload > 0) std::memcpy(decoded_, VARDATA_ANY(raw), payload);
        admit_cached_chunk_(
            sub_key,
            chunk_no,
            std::span<const char>(reinterpret_cast<const char*>(VARDATA_ANY(raw)), payload),
            false);
        if (reinterpret_cast<void*>(raw) != reinterpret_cast<void*>(DatumGetPointer(d))) pfree(raw);
        decoded_sub_ = sub_key; decoded_chunk_ = chunk_no; decoded_bytes_ = payload;
        systable_endscan(s);
        relation_close(rel, AccessShareLock);
    }

    // ---- const metadata queries (total: 0/empty on absent) ----
    [[nodiscard]] std::size_t number_of_sub_keys() const {
        Relation rel = open_index_relation_(false, AccessShareLock);
        if (rel == nullptr) return 0;
        const Oid pk = find_primary_key_oid(rel);
        ScanKeyData k[2];
        ScanKeyInit(&k[0], kAttQueryKey, BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum((int64)query_key_));
        ScanKeyInit(&k[1], kAttSubKey,   BTLessStrategyNumber,  F_INT8LT, Int64GetDatum((int64)TEMP_RUN_BASE));
        SysScanDesc s = systable_beginscan(rel, pk, OidIsValid(pk), nullptr, 2, k);
        std::int64_t last = std::numeric_limits<std::int64_t>::min(); bool have=false; std::size_t count=0;
        for (HeapTuple t; (t = systable_getnext(s)) != nullptr; ) {
            bool isnull=false; std::int64_t sk = DatumGetInt64(heap_getattr(t, kAttSubKey, RelationGetDescr(rel), &isnull));
            if (!have || sk != last) { ++count; last = sk; have = true; }
        }
        systable_endscan(s); relation_close(rel, AccessShareLock);
        return count;
    }
    [[nodiscard]] std::size_t number_of_chunks(index_sub_key_t sub_key) const {
        Relation rel = open_index_relation_(false, AccessShareLock);
        if (rel == nullptr) return 0;
        const Oid pk = find_primary_key_oid(rel);
        ScanKeyData k[2];
        ScanKeyInit(&k[0], kAttQueryKey, BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum((int64)query_key_));
        ScanKeyInit(&k[1], kAttSubKey,   BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum((int64)sub_key));
        SysScanDesc s = systable_beginscan(rel, pk, OidIsValid(pk), nullptr, 2, k);
        std::size_t n=0; for (; systable_getnext(s) != nullptr; ) ++n;
        systable_endscan(s); relation_close(rel, AccessShareLock);
        return n;
    }
    [[nodiscard]] std::size_t chunk_byte_size(index_sub_key_t sub_key, std::size_t chunk_no) const {
        Relation rel = open_index_relation_(false, AccessShareLock);
        if (rel == nullptr) return 0;
        const Oid pk = find_primary_key_oid(rel);
        ScanKeyData k[3];
        ScanKeyInit(&k[0], kAttQueryKey, BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum((int64)query_key_));
        ScanKeyInit(&k[1], kAttSubKey,   BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum((int64)sub_key));
        ScanKeyInit(&k[2], kAttChunkNo,  BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum((int32)chunk_no));
        SysScanDesc s = systable_beginscan(rel, pk, OidIsValid(pk), nullptr, 3, k);
        std::size_t out=0; HeapTuple t = systable_getnext(s);
        if (HeapTupleIsValid(t)) { bool isnull=false; out = (std::size_t)DatumGetInt32(heap_getattr(t, kAttChunkBytes, RelationGetDescr(rel), &isnull)); }
        systable_endscan(s); relation_close(rel, AccessShareLock);
        return out;
    }

    template <class T>
    [[nodiscard]] const T* read_chunk(index_sub_key_t sub_key, std::size_t chunk_no) {
        static_assert(alignof(T) <= alignof(std::int64_t), "read_chunk<T>: T must be int64-aligned-compatible");
        if (decoded_ != nullptr && decoded_sub_ == sub_key && decoded_chunk_ == chunk_no)
            return reinterpret_cast<const T*>(decoded_);
        load_chunk_into_buffer_(sub_key, chunk_no);
        return reinterpret_cast<const T*>(decoded_);
    }

    void resize(index_sub_key_t sub_key, std::size_t bytes_count) {
        const std::size_t n = bytes_count == 0 ? 1 : (bytes_count + MAX_INDEX_CHUNK_SIZE_BYTES - 1) / MAX_INDEX_CHUNK_SIZE_BYTES;
        std::vector<std::size_t> plan(n);
        std::size_t rem = bytes_count;
        for (std::size_t i = 0; i < n; ++i) { const std::size_t c = std::min(rem, MAX_INDEX_CHUNK_SIZE_BYTES); plan[i] = c; rem -= c; }
        chunk_payload_bytes_[sub_key] = std::move(plan);
    }
    void resize_chunks(index_sub_key_t sub_key, std::span<const std::size_t> per_chunk_bytes) {
        std::vector<std::size_t> plan(per_chunk_bytes.begin(), per_chunk_bytes.end());
        for (std::size_t b : plan) if (b > MAX_INDEX_CHUNK_SIZE_BYTES) bioseq_log<ERR, PG>( "bioseq index: chunk payload exceeds MAX");
        chunk_payload_bytes_[sub_key] = std::move(plan);
    }
    void prefetch_chunk(index_sub_key_t sub_key, std::size_t chunk_no) {
        if (decoded_sub_ == sub_key && decoded_chunk_ == chunk_no) {
            return;
        }
        load_chunk_into_buffer_(sub_key, chunk_no);
    }

    void upsert_chunk_row_(Relation rel, std::uint64_t query_key, index_sub_key_t sub_key, std::size_t chunk_no,
                           std::size_t chunk_bytes, std::span<const char> payload) {
        {
            const Oid pk = find_primary_key_oid(rel);
            ScanKeyData k[3];
            ScanKeyInit(&k[0], kAttQueryKey, BTEqualStrategyNumber, F_INT8EQ,
                        Int64GetDatum((int64)query_key));
            ScanKeyInit(&k[1], kAttSubKey, BTEqualStrategyNumber, F_INT8EQ,
                        Int64GetDatum((int64)sub_key));
            ScanKeyInit(&k[2], kAttChunkNo, BTEqualStrategyNumber, F_INT4EQ,
                        Int32GetDatum((int32)chunk_no));
            SysScanDesc s = systable_beginscan(rel, pk, OidIsValid(pk), nullptr, 3, k);
            HeapTuple old_t = systable_getnext(s);
            if (HeapTupleIsValid(old_t)) {
                simple_heap_delete(rel, &old_t->t_self);
            }
            systable_endscan(s);
        }
        bytea* blob = reinterpret_cast<bytea*>(palloc(VARHDRSZ + payload.size()));
        SET_VARSIZE(blob, VARHDRSZ + payload.size());
        if (!payload.empty()) std::memcpy(VARDATA(blob), payload.data(), payload.size());
        TupleDesc td = RelationGetDescr(rel);
        Datum values[5] = { Int64GetDatum((int64)query_key), Int64GetDatum((int64)sub_key),
                            Int32GetDatum((int32)chunk_no), Int32GetDatum((int32)chunk_bytes), PointerGetDatum(blob) };
        bool nulls[5] = { false,false,false,false,false };
        HeapTuple tup = heap_form_tuple(td, values, nulls);
        CatalogTupleInsert(rel, tup);
        heap_freetuple(tup); pfree(blob);
    }

    template <class T, class F>
    void write_to_each(index_sub_key_t sub_key, F&& writer) {
        static_assert(bioseq::indexing::index_chunk_writer_fn<F, T>,
            "write_to_each<T>: writer must be (size_t, T*, size_t) -> size_t");
        Relation rel = open_index_relation_(true, RowExclusiveLock);
        auto it = chunk_payload_bytes_.find(sub_key);
        if (it == chunk_payload_bytes_.end())
            bioseq_log<ERR, PG>( "bioseq index: write_to_each(sub_key={}) without a preceding resize", (long long)sub_key);
        const std::vector<std::size_t>& plan = it->second;
        const std::size_t n = plan.size();
        std::size_t max_payload = 0; for (std::size_t c=0;c<n;++c) max_payload = std::max(max_payload, plan[c]);
        std::int64_t* buf = scratch_buffer_((max_payload + sizeof(std::int64_t) - 1) / sizeof(std::int64_t) + 1);
        for (std::size_t chunk_no = 0; chunk_no < n; /* writer may skip */) {
            const std::size_t payload_bytes = plan[chunk_no];
            if (scratch_buffer_bytes_() < payload_bytes) bioseq_log<ERR, PG>( "bioseq index: scratch undersized");
            if (payload_bytes % sizeof(T) != 0) bioseq_log<ERR, PG>( "bioseq index: chunk payload not a whole number of T");
            const std::size_t cap_units = payload_bytes / sizeof(T);
            if (payload_bytes > 0) std::memset(buf, 0, payload_bytes);
            const std::size_t next = writer(chunk_no, reinterpret_cast<T*>(buf), cap_units);
            if (next < chunk_no) bioseq_log<ERR, PG>( "bioseq index: writer moved backwards");
            upsert_chunk_row_(rel, query_key_, sub_key, chunk_no, payload_bytes,
                              std::span<const char>(reinterpret_cast<const char*>(buf), payload_bytes));
            admit_cached_chunk_(sub_key, chunk_no,
                std::span<const char>(reinterpret_cast<const char*>(buf), payload_bytes),
                true);
            chunk_no = (next == chunk_no) ? chunk_no + 1 : next;
        }
        relation_close(rel, RowExclusiveLock);
        chunk_payload_bytes_.erase(it);
    }

    template <class F> void read_chunks_in_range(index_sub_key_t lo, index_sub_key_t hi, F&& cb) {
        if (hi <= lo) return;
        Relation rel = open_index_relation_(false, AccessShareLock);
        if (rel == nullptr) return;
        const Oid pk = find_primary_key_oid(rel);
        if (!OidIsValid(pk)) { relation_close(rel, AccessShareLock); index_error("range scan: no PK index"); }
        Relation idx = index_open(pk, AccessShareLock);
        ScanKeyData k[3];
        ScanKeyInit(&k[0], kAttQueryKey, BTEqualStrategyNumber,        F_INT8EQ, Int64GetDatum((int64)query_key_));
        ScanKeyInit(&k[1], kAttSubKey,   BTGreaterEqualStrategyNumber, F_INT8GE, Int64GetDatum((int64)lo));
        ScanKeyInit(&k[2], kAttSubKey,   BTLessEqualStrategyNumber,    F_INT8LE, Int64GetDatum((int64)(hi - 1)));
        SysScanDesc s = systable_beginscan_ordered(rel, idx, nullptr, 3, k);
        std::vector<char> tmp;
        for (HeapTuple t; (t = systable_getnext_ordered(s, ForwardScanDirection)) != nullptr; ) {
            bool isnull=false;
            const std::int64_t sk = DatumGetInt64(heap_getattr(t, kAttSubKey, RelationGetDescr(rel), &isnull));
            const std::int32_t chunk_no = DatumGetInt32(
                heap_getattr(t, kAttChunkNo, RelationGetDescr(rel), &isnull));
            Datum d = heap_getattr(t, kAttBlob, RelationGetDescr(rel), &isnull);
            bytea* raw = reinterpret_cast<bytea*>(PG_DETOAST_DATUM_PACKED(d));
            const std::size_t payload = VARSIZE_ANY_EXHDR(raw);
            tmp.resize(payload); if (payload>0) std::memcpy(tmp.data(), VARDATA_ANY(raw), payload);
            const auto cached = store_cached_chunk(
                chunk_cache_key_((index_sub_key_t)sk, static_cast<std::size_t>(chunk_no)),
                std::span<const char>(tmp.data(), tmp.size()),
                false);
            if (reinterpret_cast<void*>(raw) != reinterpret_cast<void*>(DatumGetPointer(d))) pfree(raw);
            if (cached) {
                cb((index_sub_key_t)sk, cached->data, cached->size);
            } else {
                cb((index_sub_key_t)sk, tmp.data(), payload);
            }
        }
        systable_endscan_ordered(s);
        index_close(idx, AccessShareLock);
        relation_close(rel, AccessShareLock);
    }

    template <class F> void for_each_sub_key(F&& fn) const {
        Relation rel = open_index_relation_(false, AccessShareLock);
        if (rel == nullptr) return;
        const Oid pk = find_primary_key_oid(rel);
        ScanKeyData k[2];
        ScanKeyInit(&k[0], kAttQueryKey, BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum((int64)query_key_));
        ScanKeyInit(&k[1], kAttSubKey,   BTLessStrategyNumber,  F_INT8LT, Int64GetDatum((int64)TEMP_RUN_BASE));
        SysScanDesc s = systable_beginscan(rel, pk, OidIsValid(pk), nullptr, 2, k);
        std::int64_t last = std::numeric_limits<std::int64_t>::min(); bool have=false;
        for (HeapTuple t; (t = systable_getnext(s)) != nullptr; ) {
            bool isnull=false; std::int64_t sk = DatumGetInt64(heap_getattr(t, kAttSubKey, RelationGetDescr(rel), &isnull));
            if (!have || sk != last) { fn((index_sub_key_t)sk); last = sk; have = true; }
        }
        systable_endscan(s); relation_close(rel, AccessShareLock);
    }

    // ---- lifecycle ----
    void delete_where_(const char* extra_sql) {
        const Oid rid = resolve_relid_(false);
        if (!OidIsValid(rid)) return;
        std::string sql = fmt::format("DELETE FROM bioseq_internal.{} WHERE query_key = {}{}",
                                      quote_identifier(this_table_name().c_str()), (long long)query_key_, extra_sql);
        if (SPI_connect() == SPI_OK_CONNECT) { SPI_execute(sql.c_str(), false, 0); SPI_finish(); }
    }
    void clear_entity() { delete_where_(""); decoded_chunk_ = std::numeric_limits<std::size_t>::max(); decoded_ = decoded_; }
    void discard_sub_keys(index_sub_key_t lo, index_sub_key_t hi) {
        std::string extra = fmt::format(" AND sub_key BETWEEN {} AND {}", (long long)lo, (long long)hi);
        delete_where_(extra.c_str());
    }
    void clear(std::string_view name); // index-wide drop across all shards (defined below, uses seq_sub_table enumeration)

    [[nodiscard]] std::size_t run_buffer_bytes() const {
        if (run_buffer_override_bytes_ > 0) return run_buffer_override_bytes_;
        constexpr std::uint64_t floor = std::uint64_t{64} << 20;
        constexpr std::uint64_t ceil = std::uint64_t{8} << 30;
        const std::uint64_t desired = std::uint64_t{2} << 30;
        std::uint64_t capped =
            bioseq::memory::default_budget().clamp_working_set(desired, floor, ceil);
        const std::uint64_t index_cap =
            bioseq::parallelism::MemoryAdmissionController::instance().index_memory_cap_bytes();
        if (index_cap != 0) {
            capped = std::min(capped, index_cap);
        }
        return static_cast<std::size_t>(capped);
    }

    // ---- external sort spill (BufFile-backed sorted runs) ----
    [[nodiscard]] SortedPairRunStore& sorted_runs_ensure_() {
        if (!sorted_runs_.has_value()) {
            sorted_runs_.emplace();
        }
        return *sorted_runs_;
    }
    std::size_t spill_sorted_run(std::span<const std::pair<std::uint64_t,std::uint64_t>> sorted);
    std::size_t spill_sorted_run_owned(std::vector<std::pair<std::uint64_t,std::uint64_t>> sorted);
	    std::size_t spill_sorted_run_owned_admitted(
	        std::vector<std::pair<std::uint64_t,std::uint64_t>> sorted,
	        std::uint64_t admitted_bytes,
	        bioseq::parallelism::MemoryPermit permit);
	    void demote_resident_runs_until_available(std::uint64_t bytes_needed);
	    template <class F> void merge_runs(F&& on_kmer);
	    [[nodiscard]] SortedRunStoreStats sorted_run_stats() const noexcept;
	    void snapshot_sorted_run_stats_before_release();
    void release_runs_noexcept() noexcept;
    void release_runs();
};

// A bounded position cursor over one kmer's unique positions (one vector).
struct PositionBatchCursor {
    const std::vector<std::uint64_t>* v = nullptr;
    [[nodiscard]] std::uint64_t total_count() const { return v->size(); }
    template <class F> void for_each_batch(F&& f) const {
        if (!v->empty()) f(std::span<const std::uint64_t>(v->data(), v->size()));
    }
};

// =========================================================================
//  IndexStorageSequencePg  —  INLINE_PER_SEQUENCE
// =========================================================================
struct IndexStorageSequencePg {
    Oid           db_oid_ = InvalidOid, tablespace_oid_ = InvalidOid, main_relfilenode_ = InvalidOid, user_relid_ = InvalidOid;
    seq_caches*   read_caches_ = nullptr;
    std::int16_t  column_attno_ = 0;
    std::string_view index_name_{};
    std::uint8_t  bucket_idx_ = 0;
    std::uint32_t shard_idx_ = 0;
    std::uint64_t query_key_ = 0;
    MemoryContext own_mctx_ = nullptr;
    mutable Oid   index_relid_ = InvalidOid;
    mutable std::int64_t* decoded_ = nullptr;
    mutable std::size_t   decoded_units_ = 0;
    mutable std::size_t   decoded_bytes_ = 0;
    mutable bool          decoded_valid_ = false;
    std::size_t           planned_bytes_ = std::numeric_limits<std::size_t>::max();

    IndexStorageSequencePg() = default;
    IndexStorageSequencePg(const IndexStorageSequencePg&) = delete;
    IndexStorageSequencePg& operator=(const IndexStorageSequencePg&) = delete;
    IndexStorageSequencePg(IndexStorageSequencePg&& o) noexcept { steal_(std::move(o)); }
    IndexStorageSequencePg& operator=(IndexStorageSequencePg&& o) noexcept { if (this!=&o){release_(); steal_(std::move(o));} return *this; }
    ~IndexStorageSequencePg() { release_(); }
    void steal_(IndexStorageSequencePg&& o) noexcept {
        db_oid_=o.db_oid_; tablespace_oid_=o.tablespace_oid_; main_relfilenode_=o.main_relfilenode_; user_relid_=o.user_relid_;
        read_caches_=o.read_caches_; column_attno_=o.column_attno_; index_name_=o.index_name_;
        bucket_idx_=o.bucket_idx_; shard_idx_=o.shard_idx_; query_key_=o.query_key_;
        own_mctx_=o.own_mctx_; index_relid_=o.index_relid_; decoded_=o.decoded_; decoded_units_=o.decoded_units_;
        decoded_bytes_=o.decoded_bytes_; decoded_valid_=o.decoded_valid_; planned_bytes_=o.planned_bytes_;
        o.own_mctx_=nullptr; o.decoded_=nullptr; o.decoded_units_=0; o.index_relid_=InvalidOid; o.decoded_valid_=false;
    }
    void release_() noexcept { if (own_mctx_!=nullptr){ MemoryContextDelete(own_mctx_); own_mctx_=nullptr; decoded_=nullptr; } }

    [[noreturn]] void index_error(std::string_view msg) const { bioseq_log<ERR, PG>( "bioseq index: {}", msg); abort(); }

    [[nodiscard]] std::string this_table_name() const {
        return format_seqidx_name(index_name_, db_oid_, tablespace_oid_, main_relfilenode_, column_attno_, bucket_idx_, shard_idx_);
    }
    [[nodiscard]] Oid resolve_relid_(bool create_if_missing) const {
        if (OidIsValid(index_relid_)) return index_relid_;
        const std::string nm = this_table_name(); const Oid ns = bioseq_internal_ns();
        Oid rid = get_relname_relid(nm.c_str(), ns);
        if (!OidIsValid(rid) && create_if_missing) {
            const char* pk = persistence_kw_for(user_relid_);
            std::string ddl = fmt::format(
                "CREATE {} TABLE IF NOT EXISTS bioseq_internal.{} ("
                "query_key BIGINT NOT NULL, chunk_bytes INT NOT NULL, blob BYTEA NOT NULL, "
                "PRIMARY KEY (query_key)) USING heap;", pk, quote_identifier(nm.c_str()));
            run_internal_ddl(ddl.c_str());
            run_internal_ddl(fmt::format(
                "ALTER TABLE bioseq_internal.{} ALTER COLUMN blob SET STORAGE EXTERNAL;",
                quote_identifier(nm.c_str())).c_str());
            rid = get_relname_relid(nm.c_str(), ns);
            if (!OidIsValid(rid)) index_error("failed to create inline seqidx table");
        }
        index_relid_ = rid; return rid;
    }
    [[nodiscard]] Relation open_(bool create, LOCKMODE lock) const {
        const Oid rid = resolve_relid_(create);
        if (!OidIsValid(rid)) return nullptr;
        return try_safe_relation_open(rid, lock);
    }
    void ensure_buffer_(std::size_t units) const {
        if (decoded_units_ >= units && decoded_!=nullptr) return;
        MemoryContext old = MemoryContextSwitchTo(own_mctx_);
        decoded_ = (decoded_==nullptr)? (std::int64_t*)palloc(units*sizeof(std::int64_t)) : (std::int64_t*)repalloc(decoded_, units*sizeof(std::int64_t));
        decoded_units_ = units; MemoryContextSwitchTo(old);
    }
    void load_() const {
        Relation rel = open_(false, AccessShareLock);
        if (rel == nullptr) { decoded_bytes_=0; decoded_valid_=true; return; }
        const Oid pk = find_primary_key_oid(rel);
        ScanKeyData k[1]; ScanKeyInit(&k[0], 1, BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum((int64)query_key_));
        SysScanDesc s = systable_beginscan(rel, pk, OidIsValid(pk), nullptr, 1, k);
        HeapTuple t = systable_getnext(s);
        if (HeapTupleIsValid(t)) {
            bool isnull=false; Datum d = heap_getattr(t, 3, RelationGetDescr(rel), &isnull);
            bytea* raw = reinterpret_cast<bytea*>(PG_DETOAST_DATUM_PACKED(d));
            const std::size_t payload = VARSIZE_ANY_EXHDR(raw);
            const std::size_t units = (payload + sizeof(std::int64_t) - 1)/sizeof(std::int64_t);
            ensure_buffer_(units==0?1:units);
            if (payload>0) std::memcpy(decoded_, VARDATA_ANY(raw), payload);
            if (reinterpret_cast<void*>(raw)!=reinterpret_cast<void*>(DatumGetPointer(d))) pfree(raw);
            decoded_bytes_ = payload;
        } else decoded_bytes_ = 0;
        decoded_valid_ = true;
        systable_endscan(s); relation_close(rel, AccessShareLock);
    }
    [[nodiscard]] std::size_t number_of_chunks() const { return chunk_byte_size() == 0 ? 0 : 1; }
    [[nodiscard]] std::size_t chunk_byte_size() const { if (!decoded_valid_) load_(); return decoded_bytes_; }
    template <class T> [[nodiscard]] const T* read_chunk() { if (!decoded_valid_) load_(); return reinterpret_cast<const T*>(decoded_); }
    void resize(std::size_t bytes) { if (bytes > MAX_INDEX_CHUNK_SIZE_BYTES) bioseq_log<ERR, PG>( "bioseq index inline: bytes exceed single-chunk MAX"); planned_bytes_ = bytes; }
    void prefetch_chunk() {}
    template <class T, class F> void write_to_each(F&& writer) {
        static_assert(bioseq::indexing::index_chunk_writer_fn<F, T>, "inline write_to_each writer shape");
        if (planned_bytes_ == std::numeric_limits<std::size_t>::max()) bioseq_log<ERR, PG>( "bioseq index inline: write_to_each without resize");
        const std::size_t payload = planned_bytes_;
        if (payload % sizeof(T) != 0) bioseq_log<ERR, PG>( "bioseq index inline: payload not whole T");
        std::vector<char> buf(payload, 0);
        const std::size_t next = writer(0, reinterpret_cast<T*>(buf.data()), payload / sizeof(T));
        if (next != 0) bioseq_log<ERR, PG>( "bioseq index inline: writer emitted >1 chunk");
        Relation rel = open_(true, RowExclusiveLock);
        // upsert (delete then insert) so rebuild replaces the single row.
        { const Oid pk = find_primary_key_oid(rel);
          ScanKeyData k[1]; ScanKeyInit(&k[0],1,BTEqualStrategyNumber,F_INT8EQ,Int64GetDatum((int64)query_key_));
          SysScanDesc s = systable_beginscan(rel, pk, OidIsValid(pk), nullptr, 1, k);
          HeapTuple old_t = systable_getnext(s);
          if (HeapTupleIsValid(old_t)) simple_heap_delete(rel, &old_t->t_self);
          systable_endscan(s); }
        bytea* blob = reinterpret_cast<bytea*>(palloc(VARHDRSZ + payload));
        SET_VARSIZE(blob, VARHDRSZ + payload); if (payload>0) std::memcpy(VARDATA(blob), buf.data(), payload);
        Datum values[3] = { Int64GetDatum((int64)query_key_), Int32GetDatum((int32)payload), PointerGetDatum(blob) };
        bool nulls[3] = {false,false,false};
        HeapTuple tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
        CatalogTupleInsert(rel, tup); heap_freetuple(tup); pfree(blob);
        relation_close(rel, RowExclusiveLock);
        decoded_valid_ = false; planned_bytes_ = std::numeric_limits<std::size_t>::max();
    }
    void clear_entity() {
        const Oid rid = resolve_relid_(false); if (!OidIsValid(rid)) return;
        std::string sql = fmt::format("DELETE FROM bioseq_internal.{} WHERE query_key = {}", quote_identifier(this_table_name().c_str()), (long long)query_key_);
        if (SPI_connect()==SPI_OK_CONNECT){ SPI_execute(sql.c_str(), false, 0); SPI_finish(); }
        decoded_valid_ = false;
    }
    void clear(std::string_view name);
    /// pg-only reuse hook (§8.3) — reposition without re-allocating own_mctx_.
    void reset_entity(const seq_locator& loc, std::uint64_t sequence_rowno) {
        bucket_idx_ = (std::uint8_t)loc.bucket_idx; shard_idx_ = loc.shard_idx; query_key_ = sequence_rowno;
        index_relid_ = InvalidOid; decoded_valid_ = false; decoded_bytes_ = 0;
    }
};

// =========================================================================
//  IndexStorageTablePg  —  CHUNKED_PER_TABLE
// =========================================================================
struct IndexStorageTablePg {
    Oid db_oid_ = InvalidOid, tablespace_oid_ = InvalidOid, main_relfilenode_ = InvalidOid, user_relid_ = InvalidOid;
    std::int16_t column_attno_ = 0;
    std::string_view index_name_{};
    seq_caches* read_caches_ = nullptr;
    MemoryContext own_mctx_ = nullptr;
    mutable Oid index_relid_ = InvalidOid;
    mutable std::int64_t* decoded_ = nullptr; mutable std::size_t decoded_units_=0, decoded_bytes_=0;
    mutable std::int64_t decoded_chunk_ = -1;
    std::size_t planned_bytes_for_chunk_ = 0;

    IndexStorageTablePg() = default;
    IndexStorageTablePg(const IndexStorageTablePg&) = delete;
    IndexStorageTablePg& operator=(const IndexStorageTablePg&) = delete;
    IndexStorageTablePg(IndexStorageTablePg&& o) noexcept { steal_(std::move(o)); }
    IndexStorageTablePg& operator=(IndexStorageTablePg&& o) noexcept { if (this!=&o){release_(); steal_(std::move(o));} return *this; }
    ~IndexStorageTablePg() { release_(); }
    void steal_(IndexStorageTablePg&& o) noexcept {
        db_oid_=o.db_oid_; tablespace_oid_=o.tablespace_oid_; main_relfilenode_=o.main_relfilenode_; user_relid_=o.user_relid_;
        column_attno_=o.column_attno_; index_name_=o.index_name_; read_caches_=o.read_caches_; own_mctx_=o.own_mctx_;
        index_relid_=o.index_relid_; decoded_=o.decoded_; decoded_units_=o.decoded_units_; decoded_bytes_=o.decoded_bytes_;
        decoded_chunk_=o.decoded_chunk_; planned_bytes_for_chunk_=o.planned_bytes_for_chunk_;
        o.own_mctx_=nullptr; o.decoded_=nullptr; o.index_relid_=InvalidOid; o.decoded_units_=0;
    }
    void release_() noexcept { if (own_mctx_!=nullptr){ MemoryContextDelete(own_mctx_); own_mctx_=nullptr; decoded_=nullptr; } }
    [[noreturn]] void index_error(std::string_view msg) const { bioseq_log<ERR, PG>( "bioseq index: {}", msg); abort(); }

    [[nodiscard]] std::string this_table_name(std::int32_t shard=0) const {
        return format_seqidxtab_name(index_name_, db_oid_, tablespace_oid_, main_relfilenode_, column_attno_, shard);
    }
    [[nodiscard]] Oid resolve_relid_(bool create) const {
        if (OidIsValid(index_relid_)) return index_relid_;
        const std::string nm = this_table_name(0); const Oid ns = bioseq_internal_ns();
        Oid rid = get_relname_relid(nm.c_str(), ns);
        if (!OidIsValid(rid) && create) {
            const char* pk = persistence_kw_for(user_relid_);
            std::string ddl = fmt::format(
                "CREATE {} TABLE IF NOT EXISTS bioseq_internal.{} ("
                "chunk_no BIGINT NOT NULL, chunk_bytes INT NOT NULL, blob BYTEA NOT NULL, "
                "PRIMARY KEY (chunk_no)) USING heap;", pk, quote_identifier(nm.c_str()));
            run_internal_ddl(ddl.c_str());
            run_internal_ddl(fmt::format(
                "ALTER TABLE bioseq_internal.{} ALTER COLUMN blob SET STORAGE EXTERNAL;",
                quote_identifier(nm.c_str())).c_str());
            rid = get_relname_relid(nm.c_str(), ns);
        }
        index_relid_ = rid; return rid;
    }
    [[nodiscard]] Relation open_(bool create, LOCKMODE lock) const { const Oid rid = resolve_relid_(create); if (!OidIsValid(rid)) return nullptr; return try_safe_relation_open(rid, lock); }
    void ensure_buffer_(std::size_t units) const {
        if (decoded_units_>=units && decoded_!=nullptr) return;
        MemoryContext old = MemoryContextSwitchTo(own_mctx_);
        decoded_ = (decoded_==nullptr)?(std::int64_t*)palloc(units*sizeof(std::int64_t)):(std::int64_t*)repalloc(decoded_, units*sizeof(std::int64_t));
        decoded_units_=units; MemoryContextSwitchTo(old);
    }
    [[nodiscard]] std::size_t number_of_chunks() const {
        Relation rel = open_(false, AccessShareLock); if (rel==nullptr) return 0;
        SysScanDesc s = systable_beginscan(rel, InvalidOid, false, nullptr, 0, nullptr);
        std::size_t n=0; for (; systable_getnext(s)!=nullptr;) ++n; systable_endscan(s); relation_close(rel, AccessShareLock); return n;
    }
    [[nodiscard]] std::size_t chunk_byte_size(std::size_t chunk_no) const {
        Relation rel = open_(false, AccessShareLock); if (rel==nullptr) return 0;
        const Oid pk = find_primary_key_oid(rel);
        ScanKeyData k[1]; ScanKeyInit(&k[0],1,BTEqualStrategyNumber,F_INT8EQ,Int64GetDatum((int64)chunk_no));
        SysScanDesc s = systable_beginscan(rel, pk, OidIsValid(pk), nullptr, 1, k);
        std::size_t out=0; HeapTuple t = systable_getnext(s);
        if (HeapTupleIsValid(t)){ bool isnull=false; out=(std::size_t)DatumGetInt32(heap_getattr(t,2,RelationGetDescr(rel),&isnull)); }
        systable_endscan(s); relation_close(rel, AccessShareLock); return out;
    }
    template <class T> [[nodiscard]] const T* read_chunk(std::size_t chunk_no) {
        if (decoded_!=nullptr && decoded_chunk_==(std::int64_t)chunk_no) return reinterpret_cast<const T*>(decoded_);
        Relation rel = open_(false, AccessShareLock);
        if (rel==nullptr) index_error("table index relation missing");
        const Oid pk = find_primary_key_oid(rel);
        ScanKeyData k[1]; ScanKeyInit(&k[0],1,BTEqualStrategyNumber,F_INT8EQ,Int64GetDatum((int64)chunk_no));
        SysScanDesc s = systable_beginscan(rel, pk, OidIsValid(pk), nullptr, 1, k);
        HeapTuple t = systable_getnext(s);
        if (!HeapTupleIsValid(t)){ systable_endscan(s); relation_close(rel,AccessShareLock); index_error("table chunk missing"); }
        bool isnull=false; Datum d = heap_getattr(t,3,RelationGetDescr(rel),&isnull);
        bytea* raw = reinterpret_cast<bytea*>(PG_DETOAST_DATUM_PACKED(d));
        const std::size_t payload = VARSIZE_ANY_EXHDR(raw);
        const std::size_t units = (payload+sizeof(std::int64_t)-1)/sizeof(std::int64_t);
        ensure_buffer_(units==0?1:units); if (payload>0) std::memcpy(decoded_, VARDATA_ANY(raw), payload);
        if (reinterpret_cast<void*>(raw)!=reinterpret_cast<void*>(DatumGetPointer(d))) pfree(raw);
        decoded_chunk_=(std::int64_t)chunk_no; decoded_bytes_=payload;
        systable_endscan(s); relation_close(rel, AccessShareLock);
        return reinterpret_cast<const T*>(decoded_);
    }
    void resize(std::size_t bytes) { planned_bytes_for_chunk_ = bytes; }
    void prefetch_chunk(std::size_t) {}
    template <class T, class F> void write_to_each(F&& writer) { write_to<T>(0, std::forward<F>(writer)); }
    template <class T, class F> void write_to(std::size_t chunk_no, F&& writer) {
        static_assert(bioseq::indexing::index_chunk_writer_fn<F, T>, "table write_to writer shape");
        const std::size_t payload = planned_bytes_for_chunk_;
        if (payload % sizeof(T) != 0) bioseq_log<ERR, PG>( "bioseq index table: payload not whole T");
        std::vector<char> buf(payload, 0);
        writer(chunk_no, reinterpret_cast<T*>(buf.data()), payload / sizeof(T));
        Relation rel = open_(true, RowExclusiveLock);
        { const Oid pk = find_primary_key_oid(rel);
          ScanKeyData k[1]; ScanKeyInit(&k[0],1,BTEqualStrategyNumber,F_INT8EQ,Int64GetDatum((int64)chunk_no));
          SysScanDesc s = systable_beginscan(rel, pk, OidIsValid(pk), nullptr, 1, k);
          HeapTuple old_t = systable_getnext(s); if (HeapTupleIsValid(old_t)) simple_heap_delete(rel, &old_t->t_self);
          systable_endscan(s); }
        bytea* blob = reinterpret_cast<bytea*>(palloc(VARHDRSZ + payload)); SET_VARSIZE(blob, VARHDRSZ + payload);
        if (payload>0) std::memcpy(VARDATA(blob), buf.data(), payload);
        Datum values[3] = { Int64GetDatum((int64)chunk_no), Int32GetDatum((int32)payload), PointerGetDatum(blob) };
        bool nulls[3] = {false,false,false};
        HeapTuple tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
        CatalogTupleInsert(rel, tup); heap_freetuple(tup); pfree(blob);
        relation_close(rel, RowExclusiveLock); decoded_chunk_ = -1;
    }
    template <class F> void for_each_sequence(F&& loader);
    void clear(std::string_view name);
};

// ---- shared drop-by-prefix helper -----------------------------------------
inline void clear_with_prefix_(const std::string& prefix) {
    // escape LIKE metacharacters in the literal prefix
    std::string esc; for (char c : prefix) { if (c=='_'||c=='%'||c=='\\') esc.push_back('\\'); esc.push_back(c); }
    std::string sql = fmt::format(
        "DO $$ DECLARE r record; BEGIN "
        "FOR r IN SELECT c.relname FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace "
        "WHERE n.nspname='bioseq_internal' AND c.relname LIKE '{}%' ESCAPE '\\' LOOP "
        "EXECUTE format('DROP TABLE IF EXISTS bioseq_internal.%I CASCADE', r.relname); END LOOP; END $$;", esc);
    if (SPI_connect()==SPI_OK_CONNECT){ SPI_execute(sql.c_str(), false, 0); SPI_finish(); }
}

inline void IndexStorageFragmentPg::clear(std::string_view name) {
    clear_with_prefix_(fmt::format("seqidx_{}_{:016x}_", short_index_for(name),
                                   identity_hash64(db_oid_, tablespace_oid_, main_relfilenode_, column_attno_, name)));
    index_relid_ = InvalidOid;
}
inline void IndexStorageSequencePg::clear(std::string_view name) {
    clear_with_prefix_(fmt::format("seqidx_{}_{:016x}_", short_index_for(name),
                                   identity_hash64(db_oid_, tablespace_oid_, main_relfilenode_, column_attno_, name)));
    index_relid_ = InvalidOid; decoded_valid_ = false;
}
inline void IndexStorageTablePg::clear(std::string_view name) {
    clear_with_prefix_(fmt::format("seqidxtab_{}_{:016x}_", short_index_for(name),
                                   identity_hash64(db_oid_, tablespace_oid_, main_relfilenode_, column_attno_, name)));
    index_relid_ = InvalidOid;
}

inline std::size_t IndexStorageFragmentPg::spill_sorted_run(std::span<const std::pair<std::uint64_t,std::uint64_t>> sorted) {
    return sorted_runs_ensure_().spill_sorted_run(sorted);
}

inline std::size_t IndexStorageFragmentPg::spill_sorted_run_owned(
        std::vector<std::pair<std::uint64_t,std::uint64_t>> sorted) {
    return sorted_runs_ensure_().spill_sorted_run_owned(std::move(sorted));
}

	inline std::size_t IndexStorageFragmentPg::spill_sorted_run_owned_admitted(
	        std::vector<std::pair<std::uint64_t,std::uint64_t>> sorted,
	        std::uint64_t admitted_bytes,
	        bioseq::parallelism::MemoryPermit permit) {
	    return sorted_runs_ensure_().spill_sorted_run_owned_admitted(
	        std::move(sorted), admitted_bytes, std::move(permit));
	}

	inline void IndexStorageFragmentPg::demote_resident_runs_until_available(
	        std::uint64_t bytes_needed) {
	    if (sorted_runs_.has_value()) {
	        sorted_runs_->demote_resident_runs_until_available(bytes_needed);
	    }
	}

	template <class F>
	inline void IndexStorageFragmentPg::merge_runs(F&& on_kmer) {
    if (!sorted_runs_.has_value()) {
        return;
    }
    g_last_index_merge_run_count = std::max<std::uint64_t>(
        g_last_index_merge_run_count, sorted_runs_->run_count());
    sorted_runs_->merge_runs(std::forward<F>(on_kmer));
}

inline SortedRunStoreStats IndexStorageFragmentPg::sorted_run_stats() const noexcept {
    return sorted_runs_.has_value() ? sorted_runs_->stats() : SortedRunStoreStats{};
}

inline void IndexStorageFragmentPg::snapshot_sorted_run_stats_before_release() {
    if (!sorted_runs_.has_value()) {
        return;
    }
    const SortedRunStoreStats st = sorted_runs_->stats();
    const std::uint64_t total_runs = sorted_runs_->run_count();
    g_last_index_merge_run_count = total_runs;
    g_last_mps_build_stats = LastMpsBuildStats{
        .backend_pid = static_cast<std::uint64_t>(MyProcPid),
        .db_oid = static_cast<std::uint64_t>(db_oid_),
        .relfilenode = static_cast<std::uint64_t>(main_relfilenode_),
        .sequence_rowno = static_cast<std::uint64_t>(query_key_),
        .total_run_count = total_runs,
        .resident_runs = st.resident_runs,
        .file_runs = st.file_runs,
        .resident_run_bytes = st.resident_bytes,
        .resident_peak_bytes = st.resident_peak_bytes,
        .spilled_run_bytes = st.spilled_bytes,
        .postgres_temp_file_bytes = st.postgres_temp_file_bytes,
        .postgres_temp_file_count = st.postgres_temp_file_count,
        .merge_reader_buffer_bytes = st.merge_reader_buffer_bytes,
        .merge_pass_count = st.merge_pass_count,
        .spill_count_from_pressure = st.spill_count_from_pressure,
        .resident_demotions = st.resident_demotions,
        .position_spill_bytes = st.position_spill_bytes,
        .position_spill_count = st.position_spill_count,
    };
    bioseq_log<DBG>("[bioseq.index.mps_build.row] backend_pid={} db_oid={} relfilenode={} sequence_rowno={} "
            "resident_runs={} file_runs={} resident_run_bytes={} resident_peak_bytes={} "
            "spilled_run_bytes={} postgres_temp_file_bytes={} postgres_temp_file_count={} "
            "merge_reader_buffer_bytes={} merge_pass_count={} spill_count_from_pressure={} "
            "resident_demotions={} position_spill_bytes={} position_spill_count={}",
            MyProcPid,
            static_cast<unsigned long long>(db_oid_),
            static_cast<unsigned long long>(main_relfilenode_),
            static_cast<unsigned long long>(query_key_),
            static_cast<unsigned long long>(st.resident_runs),
            static_cast<unsigned long long>(st.file_runs),
            static_cast<unsigned long long>(st.resident_bytes),
            static_cast<unsigned long long>(st.resident_peak_bytes),
            static_cast<unsigned long long>(st.spilled_bytes),
            static_cast<unsigned long long>(st.postgres_temp_file_bytes),
            static_cast<unsigned long long>(st.postgres_temp_file_count),
            static_cast<unsigned long long>(st.merge_reader_buffer_bytes),
            static_cast<unsigned long long>(st.merge_pass_count),
            static_cast<unsigned long long>(st.spill_count_from_pressure),
            static_cast<unsigned long long>(st.resident_demotions),
            static_cast<unsigned long long>(st.position_spill_bytes),
            static_cast<unsigned long long>(st.position_spill_count));
}

inline void IndexStorageFragmentPg::release_runs_noexcept() noexcept {
    if (sorted_runs_.has_value()) {
        sorted_runs_->release_runs_noexcept();
        sorted_runs_.reset();
    }
}

inline void IndexStorageFragmentPg::release_runs() {
    release_runs_noexcept();
}

inline void write_serialized_index_chunk_pg(
        Oid user_relid,
        Oid db_oid,
        Oid tablespace_oid,
        Oid main_relfilenode,
        std::int16_t column_attno,
        std::string_view index_name,
        std::uint8_t bucket_idx,
        std::uint32_t shard_idx,
        std::uint64_t sequence_rowno,
        index_sub_key_t sub_key,
        std::uint32_t chunk_no,
        std::span<const char> bytes) {
    IndexStorageFragmentPg storage;
    storage.db_oid_ = db_oid;
    storage.tablespace_oid_ = tablespace_oid;
    storage.main_relfilenode_ = main_relfilenode;
    storage.user_relid_ = user_relid;
    storage.column_attno_ = column_attno;
    storage.index_name_ = index_name;
    storage.bucket_idx_ = bucket_idx;
    storage.shard_idx_ = shard_idx;
    storage.query_key_ = sequence_rowno;
    storage.own_mctx_ = AllocSetContextCreate(
        CurrentMemoryContext,
        "bioseq_async_index_persist",
        ALLOCSET_SMALL_SIZES);
    Relation rel = storage.open_index_relation_(true, RowExclusiveLock);
    if (rel == nullptr) {
        storage.index_error("async persist relation missing");
    }
    storage.upsert_chunk_row_(
        rel,
        sequence_rowno,
        sub_key,
        chunk_no,
        bytes.size(),
        bytes);
    storage.admit_cached_chunk_(sub_key, chunk_no, bytes, false);
    relation_close(rel, RowExclusiveLock);
}

inline void mark_serialized_index_write_successful_pg(
        Oid user_relid,
        Oid main_relfilenode,
        std::int16_t column_attno,
        std::string_view index_name) {
    if (SPI_connect() != SPI_OK_CONNECT) {
        return;
    }
    const std::string sql = fmt::format(
        "INSERT INTO bioseq_internal.metadata_age_log "
        "(user_relid, main_relfilenode, column_attno, index_name, last_updated) "
        "VALUES ({}::oid, {}::oid, {}, {}, now()) "
        "ON CONFLICT (user_relid, main_relfilenode, column_attno, index_name) "
        "DO UPDATE SET last_updated = now()",
        static_cast<unsigned>(user_relid),
        static_cast<unsigned>(main_relfilenode),
        static_cast<int>(column_attno),
        quote_literal_cstr(std::string(index_name).c_str()));
    SPI_execute(sql.c_str(), false, 0);
    SPI_finish();
}

// ---- IndexStorageTablePg::for_each_sequence (test-only CHUNKED_PER_TABLE) ---
template <class F>
inline void IndexStorageTablePg::for_each_sequence(F&& loader) {
    Relation main = table_open(user_relid_, AccessShareLock);
    TupleDesc td = RelationGetDescr(main);
    Snapshot snap = GetTransactionSnapshot();
    const std::size_t total = (std::size_t)ColumnarTableRowCount(main);
    List* projected = list_make1_int(column_attno_);
    ColumnarReadState* rs = ColumnarBeginRead(main, td, projected, NIL, own_mctx_, snap, /*randomAccess=*/false, nullptr);
    MemoryContext inner = AllocSetContextCreate(own_mctx_, "seqidx_table_inner_seq", ALLOCSET_DEFAULT_SIZES);
    std::vector<Datum> vals(td->natts);
    std::vector<bool>  nullsv(td->natts);
    Datum* valp = vals.data();
    std::vector<char> nullbuf(td->natts);
    bool* nulls = reinterpret_cast<bool*>(nullbuf.data());
    std::uint64_t rowno = 0;
    while (ColumnarReadNextRow(rs, valp, nulls, &rowno)) {
        if (nulls[column_attno_ - 1]) continue;
        MemoryContextReset(inner);
        MemoryContext old = MemoryContextSwitchTo(inner);
        pgseq_t* hdr = reinterpret_cast<pgseq_t*>(PG_DETOAST_DATUM(valp[column_attno_ - 1]));
        MemoryContextSwitchTo(old);
        const std::uint8_t alpha = hdr->alphabet_code;
        const std::uint8_t bw = logical_storage_unit_byte_width(hdr);
        if (!is_locator_stub(hdr)) {
            pgseq_t::dispatch_instance(alpha, bw, [&]<class S>(){
                compact_sequence_range<S> rng{*hdr};
                bioseq::storage::lazy_compact_sequence<S, bioseq::storage::sequence_scanner_pg<S>>
                    lazy{ bioseq::storage::make_scanner_pg_in_memory<S>(rng) };
                loader(lazy, (std::size_t)rowno, total);
            });
        } else {
            const seq_locator loc = decode_locator_from_stub(hdr);
            pgseq_t::dispatch_instance(alpha, bw, [&]<class S>(){
                constexpr std::size_t lpu = compact_sequence_range<S>::traits::letters_per_unit;
                const std::size_t pad = (loc.total_bp_size % lpu) == 0 ? 0 : (std::size_t)(lpu - loc.total_bp_size % lpu);
                bioseq::storage::with_lazy_postgres<S>(
                    db_oid_, tablespace_oid_, main_relfilenode_, column_attno_, loc, pad, read_caches_, inner,
                    [&](auto& lazy_seq){ loader(lazy_seq, (std::size_t)rowno, total); });
            });
        }
    }
    ColumnarEndRead(rs);
    MemoryContextDelete(inner);
    table_close(main, AccessShareLock);
}

// ---- conformance gates (§8) ----
static_assert(bioseq::indexing::is_index_chunked_storage_for<
                  IndexStorageFragmentPg, SequenceIndexType::CHUNKED_PER_FRAGMENT>,
              "IndexStorageFragmentPg must satisfy CHUNKED_PER_FRAGMENT");
static_assert(bioseq::indexing::is_index_chunked_storage_for<
                  IndexStorageSequencePg, SequenceIndexType::INLINE_PER_SEQUENCE>,
              "IndexStorageSequencePg must satisfy INLINE_PER_SEQUENCE");
static_assert(bioseq::indexing::is_index_chunked_storage_for<
                  IndexStorageTablePg, SequenceIndexType::CHUNKED_PER_TABLE>,
              "IndexStorageTablePg must satisfy CHUNKED_PER_TABLE");

}  // namespace bioseq::storage::indexing
