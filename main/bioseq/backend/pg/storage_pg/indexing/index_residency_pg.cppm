/**
 * @file index_residency_pg.cppm
 * @brief Backend-local residency caches for PostgreSQL index chunks and
 *        assembled minimizer indexes.
 */
module;
#include "common.h"

extern "C" {
#include "access/xact.h"
#include "utils/inval.h"
}

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <vector>

export module bioseq_storage_indexing_residency_pg;

import mlogging;
import bioseq_storage_size;
import bioseq_indexing.type;
import bioseq_indexing.minimizer_hashmap;
import bioseq_parallelism.memory_admission;

namespace bioseq::storage::indexing::residency_detail {

using bioseq::indexing::index_sub_key_t;
using bioseq::storage::sequence_size_t;

struct RegisteredCallbacks {
    bool registered = false;
};

[[nodiscard]] RegisteredCallbacks& callback_state() {
    static RegisteredCallbacks state{};
    return state;
}

[[nodiscard]] std::uint64_t fnv_mix(std::uint64_t seed, std::uint64_t value) noexcept {
    std::uint64_t hash = seed;
    for (int byte_index = 0; byte_index < 8; ++byte_index) {
        hash ^= value & 0xffu;
        hash *= 1099511628211ull;
        value >>= 8;
    }
    return hash;
}

[[nodiscard]] std::uint64_t fnv_mix_bytes(std::uint64_t seed, std::string_view bytes) noexcept {
    std::uint64_t hash = seed;
    for (const char byte : bytes) {
        hash ^= static_cast<std::uint8_t>(byte);
        hash *= 1099511628211ull;
    }
    return hash;
}

template <class Key>
[[nodiscard]] bool same_relation_identity(const Key& lhs, const Key& rhs) noexcept {
    return lhs.db_oid == rhs.db_oid
        && lhs.user_relid == rhs.user_relid
        && lhs.tablespace_oid == rhs.tablespace_oid
        && lhs.main_relfilenode == rhs.main_relfilenode
        && lhs.column_attno == rhs.column_attno
        && lhs.index_name_hash == rhs.index_name_hash;
}

} // namespace bioseq::storage::indexing::residency_detail

export namespace bioseq::storage::indexing {

using bioseq::indexing::index_sub_key_t;
using bioseq::storage::sequence_size_t;

struct IndexChunkKey {
    Oid db_oid = InvalidOid;
    Oid user_relid = InvalidOid;
    Oid tablespace_oid = InvalidOid;
    Oid main_relfilenode = InvalidOid;
    std::int16_t column_attno = 0;
    std::uint64_t index_name_hash = 0;
    std::uint8_t bucket_idx = 0;
    std::uint32_t shard_idx = 0;
    std::uint64_t query_key = 0;
    index_sub_key_t sub_key = 0;
    std::uint32_t chunk_no = 0;
};

struct CachedChunkView {
    const char* data = nullptr;
    std::size_t size = 0;
};

struct AssembledIndexKey {
    Oid db_oid = InvalidOid;
    Oid user_relid = InvalidOid;
    Oid tablespace_oid = InvalidOid;
    Oid main_relfilenode = InvalidOid;
    std::int16_t column_attno = 0;
    std::uint64_t index_name_hash = 0;
    std::uint64_t query_key = 0;
    std::uint64_t query_signature = 0;
    sequence_size_t clip_lo_bp = 0;
    sequence_size_t clip_hi_bp = 0;
};

struct AssembledIndexCacheHit {
    std::shared_ptr<const bioseq::indexing::MinimizerHashmapFragmentIndex> index;
};

[[nodiscard]] inline std::uint64_t index_name_hash(
        Oid db_oid,
        Oid tablespace_oid,
        Oid main_relfilenode,
        std::int16_t column_attno,
        std::string_view index_name) noexcept {
    std::uint64_t hash = 1469598103934665603ull;
    hash = residency_detail::fnv_mix(hash, static_cast<std::uint64_t>(db_oid));
    hash = residency_detail::fnv_mix(hash, static_cast<std::uint64_t>(tablespace_oid));
    hash = residency_detail::fnv_mix(hash, static_cast<std::uint64_t>(main_relfilenode));
    hash = residency_detail::fnv_mix(hash, static_cast<std::uint64_t>(
        static_cast<std::uint16_t>(column_attno)));
    return residency_detail::fnv_mix_bytes(hash, index_name);
}

[[nodiscard]] inline bool operator==(const IndexChunkKey& lhs, const IndexChunkKey& rhs) noexcept {
    return std::tie(lhs.db_oid, lhs.user_relid, lhs.tablespace_oid,
                    lhs.main_relfilenode, lhs.column_attno, lhs.index_name_hash,
                    lhs.bucket_idx, lhs.shard_idx, lhs.query_key, lhs.sub_key,
                    lhs.chunk_no)
        == std::tie(rhs.db_oid, rhs.user_relid, rhs.tablespace_oid,
                    rhs.main_relfilenode, rhs.column_attno, rhs.index_name_hash,
                    rhs.bucket_idx, rhs.shard_idx, rhs.query_key, rhs.sub_key,
                    rhs.chunk_no);
}

[[nodiscard]] inline bool operator==(const AssembledIndexKey& lhs, const AssembledIndexKey& rhs) noexcept {
    return std::tie(lhs.db_oid, lhs.user_relid, lhs.tablespace_oid,
                    lhs.main_relfilenode, lhs.column_attno, lhs.index_name_hash,
                    lhs.query_key, lhs.query_signature, lhs.clip_lo_bp,
                    lhs.clip_hi_bp)
        == std::tie(rhs.db_oid, rhs.user_relid, rhs.tablespace_oid,
                    rhs.main_relfilenode, rhs.column_attno, rhs.index_name_hash,
                    rhs.query_key, rhs.query_signature, rhs.clip_lo_bp,
                    rhs.clip_hi_bp);
}

struct ChunkCacheEntry {
    IndexChunkKey key{};
    std::vector<char> bytes{};
    bioseq::parallelism::MemoryPermit permit{};
    SubTransactionId subxid = InvalidSubTransactionId;
    bool written_in_xact = false;
    std::uint64_t last_used = 0;
};

struct AssembledIndexCacheEntry {
    AssembledIndexKey key{};
    std::shared_ptr<const bioseq::indexing::MinimizerHashmapFragmentIndex> index{};
    bioseq::parallelism::MemoryPermit permit{};
    std::uint64_t admitted_bytes = 0;
    std::uint64_t last_used = 0;
};

struct ResidencyState {
    std::vector<ChunkCacheEntry> chunk_entries{};
    std::vector<AssembledIndexCacheEntry> assembled_entries{};
    std::uint64_t clock = 0;
    std::uint64_t chunk_bytes = 0;
    std::uint64_t assembled_bytes = 0;
};

[[nodiscard]] inline ResidencyState& residency_state() {
    static ResidencyState state{};
    return state;
}

inline constexpr std::size_t kChunkCacheMaxEntries = 256;
inline constexpr std::uint64_t kChunkCacheMaxBytes = std::uint64_t{2} << 30;
inline constexpr std::size_t kAssembledCacheMaxEntries = 32;
inline constexpr std::uint64_t kAssembledCacheMaxBytes = std::uint64_t{4} << 30;

void evict_chunk_index(std::size_t entry_index) noexcept {
    ResidencyState& state = residency_state();
    if (entry_index >= state.chunk_entries.size()) {
        return;
    }
    ChunkCacheEntry& entry = state.chunk_entries[entry_index];
    if (state.chunk_bytes >= entry.bytes.size()) {
        state.chunk_bytes -= entry.bytes.size();
    } else {
        state.chunk_bytes = 0;
    }
    entry.permit.release();
    state.chunk_entries.erase(state.chunk_entries.begin() + static_cast<std::ptrdiff_t>(entry_index));
}

void evict_assembled_index(std::size_t entry_index) noexcept {
    ResidencyState& state = residency_state();
    if (entry_index >= state.assembled_entries.size()) {
        return;
    }
    AssembledIndexCacheEntry& entry = state.assembled_entries[entry_index];
    if (state.assembled_bytes >= entry.admitted_bytes) {
        state.assembled_bytes -= entry.admitted_bytes;
    } else {
        state.assembled_bytes = 0;
    }
    entry.permit.release();
    state.assembled_entries.erase(
        state.assembled_entries.begin() + static_cast<std::ptrdiff_t>(entry_index));
}

void evict_all_residency() noexcept {
    ResidencyState& state = residency_state();
    for (ChunkCacheEntry& entry : state.chunk_entries) {
        entry.permit.release();
    }
    for (AssembledIndexCacheEntry& entry : state.assembled_entries) {
        entry.permit.release();
    }
    state.chunk_entries.clear();
    state.assembled_entries.clear();
    state.chunk_bytes = 0;
    state.assembled_bytes = 0;
}

void evict_relation_residency(Oid relid) noexcept {
    ResidencyState& state = residency_state();
    if (!OidIsValid(relid)) {
        evict_all_residency();
        return;
    }
    for (std::size_t entry_index = 0; entry_index < state.chunk_entries.size();) {
        if (state.chunk_entries[entry_index].key.user_relid == relid) {
            evict_chunk_index(entry_index);
        } else {
            ++entry_index;
        }
    }
    for (std::size_t entry_index = 0; entry_index < state.assembled_entries.size();) {
        if (state.assembled_entries[entry_index].key.user_relid == relid) {
            evict_assembled_index(entry_index);
        } else {
            ++entry_index;
        }
    }
}

void evict_chunk_subxact(SubTransactionId subxid) noexcept {
    ResidencyState& state = residency_state();
    for (std::size_t entry_index = 0; entry_index < state.chunk_entries.size();) {
        const ChunkCacheEntry& entry = state.chunk_entries[entry_index];
        if (entry.written_in_xact && entry.subxid == subxid) {
            evict_chunk_index(entry_index);
        } else {
            ++entry_index;
        }
    }
}

void finish_top_xact(bool committed) noexcept {
    ResidencyState& state = residency_state();
    if (!committed) {
        for (std::size_t entry_index = 0; entry_index < state.chunk_entries.size();) {
            if (state.chunk_entries[entry_index].written_in_xact) {
                evict_chunk_index(entry_index);
            } else {
                ++entry_index;
            }
        }
        return;
    }
    for (ChunkCacheEntry& entry : state.chunk_entries) {
        entry.written_in_xact = false;
        entry.subxid = InvalidSubTransactionId;
    }
}

void bioseq_index_residency_xact_callback(XactEvent event, void*) {
    switch (event) {
        case XACT_EVENT_COMMIT:
        case XACT_EVENT_PARALLEL_COMMIT:
        case XACT_EVENT_PREPARE:
            finish_top_xact(true);
            break;
        case XACT_EVENT_ABORT:
        case XACT_EVENT_PARALLEL_ABORT:
            finish_top_xact(false);
            break;
        case XACT_EVENT_PRE_COMMIT:
        case XACT_EVENT_PARALLEL_PRE_COMMIT:
        case XACT_EVENT_PRE_PREPARE:
            break;
    }
}

void bioseq_index_residency_subxact_callback(
        SubXactEvent event,
        SubTransactionId my_subid,
        SubTransactionId,
        void*) {
    if (event == SUBXACT_EVENT_ABORT_SUB) {
        evict_chunk_subxact(my_subid);
    }
}

void bioseq_index_residency_relcache_callback(Datum, Oid relid) {
    evict_relation_residency(relid);
}

void install_index_residency_callbacks() {
    residency_detail::RegisteredCallbacks& state = residency_detail::callback_state();
    if (state.registered) {
        return;
    }
    RegisterXactCallback(bioseq_index_residency_xact_callback, nullptr);
    RegisterSubXactCallback(bioseq_index_residency_subxact_callback, nullptr);
    CacheRegisterRelcacheCallback(
        bioseq_index_residency_relcache_callback,
        PointerGetDatum(nullptr));
    state.registered = true;
    bioseq_log<DBG>("[bioseq] index residency: transaction and relcache callbacks registered");
}

[[nodiscard]] std::optional<CachedChunkView> lookup_cached_chunk(const IndexChunkKey& key) {
    install_index_residency_callbacks();
    ResidencyState& state = residency_state();
    for (ChunkCacheEntry& entry : state.chunk_entries) {
        if (entry.key == key) {
            entry.last_used = ++state.clock;
            return CachedChunkView{entry.bytes.data(), entry.bytes.size()};
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<CachedChunkView> store_cached_chunk(
        const IndexChunkKey& key,
        std::span<const char> payload,
        bool written_in_current_xact) {
    install_index_residency_callbacks();
    if (payload.empty() || payload.size() > kChunkCacheMaxBytes) {
        return std::nullopt;
    }
    auto permit =
        bioseq::parallelism::MemoryAdmissionController::instance()
            .try_reserve(payload.size());
    if (!permit) {
        return std::nullopt;
    }

    ResidencyState& state = residency_state();
    for (std::size_t entry_index = 0; entry_index < state.chunk_entries.size(); ++entry_index) {
        if (state.chunk_entries[entry_index].key == key) {
            evict_chunk_index(entry_index);
            break;
        }
    }
    while (!state.chunk_entries.empty()
           && (state.chunk_entries.size() >= kChunkCacheMaxEntries
               || state.chunk_bytes + payload.size() > kChunkCacheMaxBytes)) {
        const auto victim = std::min_element(
            state.chunk_entries.begin(), state.chunk_entries.end(),
            [](const ChunkCacheEntry& lhs, const ChunkCacheEntry& rhs) {
                return lhs.last_used < rhs.last_used;
            });
        evict_chunk_index(static_cast<std::size_t>(
            std::distance(state.chunk_entries.begin(), victim)));
    }
    if (state.chunk_bytes + payload.size() > kChunkCacheMaxBytes) {
        return std::nullopt;
    }

    ChunkCacheEntry entry{};
    entry.key = key;
    entry.bytes.assign(payload.begin(), payload.end());
    entry.permit = std::move(*permit);
    entry.subxid = written_in_current_xact
        ? GetCurrentSubTransactionId()
        : InvalidSubTransactionId;
    entry.written_in_xact = written_in_current_xact;
    entry.last_used = ++state.clock;
    state.chunk_bytes += entry.bytes.size();
    state.chunk_entries.push_back(std::move(entry));
    const ChunkCacheEntry& stored = state.chunk_entries.back();
    return CachedChunkView{stored.bytes.data(), stored.bytes.size()};
}

[[nodiscard]] std::optional<AssembledIndexCacheHit> lookup_assembled_index(
        const AssembledIndexKey& key) {
    install_index_residency_callbacks();
    ResidencyState& state = residency_state();
    for (AssembledIndexCacheEntry& entry : state.assembled_entries) {
        if (entry.key == key) {
            entry.last_used = ++state.clock;
            return AssembledIndexCacheHit{entry.index};
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::uint64_t estimate_assembled_index_bytes(
        const bioseq::indexing::MinimizerHashmapFragmentIndex& index) noexcept {
    const std::uint64_t serialized =
        static_cast<std::uint64_t>(index.serialize_binary_size());
    const std::uint64_t table_bytes =
        bioseq::parallelism::saturating_mul(
            static_cast<std::uint64_t>(index.table.size()),
            32u);
    const std::uint64_t position_capacity_bytes =
        bioseq::parallelism::saturating_mul(
            static_cast<std::uint64_t>(index.positions.capacity()),
            static_cast<std::uint64_t>(sizeof(std::uint64_t)));
    return serialized + table_bytes + position_capacity_bytes;
}

void store_assembled_index(
        const AssembledIndexKey& key,
        bioseq::indexing::MinimizerHashmapFragmentIndex&& index) {
    install_index_residency_callbacks();
    const std::uint64_t bytes = estimate_assembled_index_bytes(index);
    if (bytes == 0 || bytes > kAssembledCacheMaxBytes) {
        return;
    }
    auto permit =
        bioseq::parallelism::MemoryAdmissionController::instance()
            .try_reserve(bytes);
    if (!permit) {
        return;
    }

    ResidencyState& state = residency_state();
    for (std::size_t entry_index = 0; entry_index < state.assembled_entries.size(); ++entry_index) {
        if (state.assembled_entries[entry_index].key == key) {
            evict_assembled_index(entry_index);
            break;
        }
    }
    while (!state.assembled_entries.empty()
           && (state.assembled_entries.size() >= kAssembledCacheMaxEntries
               || state.assembled_bytes + bytes > kAssembledCacheMaxBytes)) {
        const auto victim = std::min_element(
            state.assembled_entries.begin(), state.assembled_entries.end(),
            [](const AssembledIndexCacheEntry& lhs, const AssembledIndexCacheEntry& rhs) {
                return lhs.last_used < rhs.last_used;
            });
        evict_assembled_index(static_cast<std::size_t>(
            std::distance(state.assembled_entries.begin(), victim)));
    }
    if (state.assembled_bytes + bytes > kAssembledCacheMaxBytes) {
        return;
    }

    AssembledIndexCacheEntry entry{};
    entry.key = key;
    entry.index = std::make_shared<bioseq::indexing::MinimizerHashmapFragmentIndex>(
        std::move(index));
    entry.permit = std::move(*permit);
    entry.admitted_bytes = bytes;
    entry.last_used = ++state.clock;
    state.assembled_bytes += bytes;
    state.assembled_entries.push_back(std::move(entry));
}

[[nodiscard]] std::uint64_t query_minimizer_signature(
        std::span<const bioseq::indexing::MinimapSketchData> minimizers,
        std::uint32_t kmer_size,
        std::uint32_t window_size,
        std::uint32_t max_occ) noexcept {
    std::uint64_t hash = 1469598103934665603ull;
    hash = residency_detail::fnv_mix(hash, kmer_size);
    hash = residency_detail::fnv_mix(hash, window_size);
    hash = residency_detail::fnv_mix(hash, max_occ);
    for (const bioseq::indexing::MinimapSketchData& minimizer : minimizers) {
        hash = residency_detail::fnv_mix(hash, minimizer.kmer());
    }
    return hash;
}

[[nodiscard]] std::uint64_t cached_index_residency_bytes() noexcept {
    const ResidencyState& state = residency_state();
    return state.chunk_bytes + state.assembled_bytes;
}

} // namespace bioseq::storage::indexing
