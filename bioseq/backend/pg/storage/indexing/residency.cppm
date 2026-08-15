/**
 * @file residency.cppm
 * @brief Backend-local residency caches for PostgreSQL index chunks and
 *        assembled minimizer indexes.
 */
module;
#include <bioseq/pg_compat.h>

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

export module bioseq.postgres.storage.indexing.residency;

import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.storage.size;
import bioseq.indexing.type;
import bioseq.indexing.minimizer_hashmap;
import bioseq.parallelism.memory_admission;
import bioseq.postgres.support;

namespace bioseq::storage::indexing::residency_detail {

using bioseq::indexing::index_sub_key_t;
using bioseq::storage::sequence_size_t;

struct registered_callbacks {
    bool xact_registered = false;
    bool subxact_registered = false;
    bool relcache_registered = false;
    bool installed = false;
};

[[nodiscard]] auto callback_state() -> registered_callbacks& {
    static registered_callbacks state{};
    return state;
}

[[nodiscard]] auto fnv_mix(std::uint64_t seed, std::uint64_t value) noexcept -> std::uint64_t {
    std::uint64_t hash = seed;
    for (int byte_index = 0; byte_index < 8; ++byte_index) {
        hash ^= value & 0xffu;
        hash *= 1099511628211ull;
        value >>= 8;
    }
    return hash;
}

[[nodiscard]] auto fnv_mix_bytes(std::uint64_t seed, std::string_view bytes) noexcept
    -> std::uint64_t {
    std::uint64_t hash = seed;
    for (const char byte : bytes) {
        hash ^= static_cast<std::uint8_t>(byte);
        hash *= 1099511628211ull;
    }
    return hash;
}

template <class p_key>
[[nodiscard]] auto same_relation_identity(const p_key& lhs, const p_key& rhs) noexcept -> bool {
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
using bioseq::indexing::minimizer_scheme_identity;
using bioseq::indexing::minimizer_sketch_backend;
using bioseq::storage::sequence_size_t;

#ifdef BIOSEQ_TEST_INDEXES
enum class index_residency_callback_failure : std::uint8_t {
    none = 0,
    register_xact = 1,
    register_subxact = 2,
    register_relcache = 3,
};

inline thread_local index_residency_callback_failure
    g_index_residency_callback_failure =
        index_residency_callback_failure::none;

void arm_index_residency_callback_failure(
        index_residency_callback_failure point) noexcept {
    g_index_residency_callback_failure = point;
}

void disarm_index_residency_callback_failure() noexcept {
    g_index_residency_callback_failure =
        index_residency_callback_failure::none;
}

void inject_index_residency_callback_failure(
        index_residency_callback_failure point) {
    if (g_index_residency_callback_failure != point) {
        return;
    }
    g_index_residency_callback_failure =
        index_residency_callback_failure::none;
    ereport(ERROR,
        (errcode(ERRCODE_INTERNAL_ERROR),
         errmsg("injected index residency callback registration failure"),
         errdetail("failure_point=%u", static_cast<unsigned>(point))));
}

struct index_residency_callback_installation {
    bool xact_registered = false;
    bool subxact_registered = false;
    bool relcache_registered = false;
    bool installed = false;
};

[[nodiscard]] index_residency_callback_installation
index_residency_callback_install_snapshot() noexcept {
    const auto& state = residency_detail::callback_state();
    return {
        .xact_registered = state.xact_registered,
        .subxact_registered = state.subxact_registered,
        .relcache_registered = state.relcache_registered,
        .installed = state.installed,
    };
}
#endif

struct index_chunk_key {
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

struct cached_chunk_view {
    const char* data = nullptr;
    std::size_t size = 0;
};

struct assembled_index_key {
    Oid db_oid = InvalidOid;
    Oid user_relid = InvalidOid;
    Oid tablespace_oid = InvalidOid;
    Oid main_relfilenode = InvalidOid;
    std::int16_t column_attno = 0;
    std::uint64_t index_name_hash = 0;
    minimizer_sketch_backend sketch_backend = minimizer_sketch_backend::scalar;
    minimizer_scheme_identity minimizer_scheme{};
    std::uint64_t query_key = 0;
    std::uint64_t query_signature = 0;
    sequence_size_t clip_lo_bp = 0;
    sequence_size_t clip_hi_bp = 0;
};

struct assembled_index_cache_hit {
    std::shared_ptr<const bioseq::indexing::minimizer_hashmap_fragment_index> index;
};

[[nodiscard]] inline auto index_name_hash(Oid db_oid,
                                          Oid tablespace_oid,
                                          Oid main_relfilenode,
                                          std::int16_t column_attno,
                                          std::string_view index_name) noexcept -> std::uint64_t {
    std::uint64_t hash = 1469598103934665603ull;
    hash = residency_detail::fnv_mix(hash, static_cast<std::uint64_t>(db_oid));
    hash = residency_detail::fnv_mix(hash, static_cast<std::uint64_t>(tablespace_oid));
    hash = residency_detail::fnv_mix(hash, static_cast<std::uint64_t>(main_relfilenode));
    hash = residency_detail::fnv_mix(hash, static_cast<std::uint64_t>(
        static_cast<std::uint16_t>(column_attno)));
    return residency_detail::fnv_mix_bytes(hash, index_name);
}

[[nodiscard]] inline bool operator==(const index_chunk_key& lhs, const index_chunk_key& rhs) noexcept {
    return std::tie(lhs.db_oid, lhs.user_relid, lhs.tablespace_oid,
                    lhs.main_relfilenode, lhs.column_attno, lhs.index_name_hash,
                    lhs.bucket_idx, lhs.shard_idx, lhs.query_key, lhs.sub_key,
                    lhs.chunk_no)
        == std::tie(rhs.db_oid, rhs.user_relid, rhs.tablespace_oid,
                    rhs.main_relfilenode, rhs.column_attno, rhs.index_name_hash,
                    rhs.bucket_idx, rhs.shard_idx, rhs.query_key, rhs.sub_key,
                    rhs.chunk_no);
}

[[nodiscard]] inline bool operator==(const assembled_index_key& lhs, const assembled_index_key& rhs) noexcept {
    return std::tie(lhs.db_oid, lhs.user_relid, lhs.tablespace_oid,
                    lhs.main_relfilenode, lhs.column_attno, lhs.index_name_hash,
                    lhs.sketch_backend, lhs.minimizer_scheme.id,
                    lhs.minimizer_scheme.revision, lhs.query_key,
                    lhs.query_signature, lhs.clip_lo_bp, lhs.clip_hi_bp)
        == std::tie(rhs.db_oid, rhs.user_relid, rhs.tablespace_oid,
                    rhs.main_relfilenode, rhs.column_attno, rhs.index_name_hash,
                    rhs.sketch_backend, rhs.minimizer_scheme.id,
                    rhs.minimizer_scheme.revision, rhs.query_key,
                    rhs.query_signature, rhs.clip_lo_bp, rhs.clip_hi_bp);
}

struct chunk_cache_entry {
    index_chunk_key key{};
    std::vector<char> bytes{};
    bioseq::parallelism::memory_permit permit{};
    SubTransactionId subxid = InvalidSubTransactionId;
    bool written_in_xact = false;
    std::uint64_t last_used = 0;
};

struct assembled_index_cache_entry {
    assembled_index_key key{};
    std::shared_ptr<const bioseq::indexing::minimizer_hashmap_fragment_index> index{};
    bioseq::parallelism::memory_permit permit{};
    std::uint64_t admitted_bytes = 0;
    std::uint64_t last_used = 0;
};

struct residency_state {
    std::vector<chunk_cache_entry> chunk_entries{};
    std::vector<assembled_index_cache_entry> assembled_entries{};
    std::uint64_t clock = 0;
    std::uint64_t chunk_bytes = 0;
    std::uint64_t assembled_bytes = 0;
};

[[nodiscard]] inline auto get_residency_state() -> residency_state& {
    static residency_state state{};
    return state;
}

inline constexpr std::size_t k_chunk_cache_maximum_entries = 256;
inline constexpr std::uint64_t k_chunk_cache_maximum_bytes = std::uint64_t{2} << 30;
inline constexpr std::size_t k_assembled_cache_maximum_entries = 32;
inline constexpr std::uint64_t k_assembled_cache_maximum_bytes = std::uint64_t{4} << 30;

void evict_chunk_index(std::size_t entry_index) noexcept {
    residency_state& state = get_residency_state();
    if (entry_index >= state.chunk_entries.size()) {
        return;
    }
    chunk_cache_entry& entry = state.chunk_entries[entry_index];
    if (state.chunk_bytes >= entry.bytes.size()) {
        state.chunk_bytes -= entry.bytes.size();
    } else {
        state.chunk_bytes = 0;
    }
    entry.permit.release();
    state.chunk_entries.erase(state.chunk_entries.begin() + static_cast<std::ptrdiff_t>(entry_index));
}

void evict_assembled_index(std::size_t entry_index) noexcept {
    residency_state& state = get_residency_state();
    if (entry_index >= state.assembled_entries.size()) {
        return;
    }
    assembled_index_cache_entry& entry = state.assembled_entries[entry_index];
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
    residency_state& state = get_residency_state();
    for (chunk_cache_entry& entry : state.chunk_entries) {
        entry.permit.release();
    }
    for (assembled_index_cache_entry& entry : state.assembled_entries) {
        entry.permit.release();
    }
    state.chunk_entries.clear();
    state.assembled_entries.clear();
    state.chunk_bytes = 0;
    state.assembled_bytes = 0;
}

void evict_relation_residency(Oid relid) noexcept {
    residency_state& state = get_residency_state();
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

/// Evict every backend-local cache entry for one logical sidecar identity.
///
/// Relcache callbacks receive the sidecar relation OID, while residency keys
/// intentionally carry the owning user relation OID.  A destructive sidecar
/// schema replacement therefore cannot rely on `evict_relation_residency`:
/// the two OIDs are different.  The generation-storage cutover calls this
/// identity form before dropping the old sidecar and again after publishing
/// the fresh physical relation.
void evict_index_residency(
        Oid db_oid,
        Oid user_relid,
        Oid tablespace_oid,
        Oid main_relfilenode,
        std::int16_t column_attno,
        std::uint64_t requested_index_name_hash) noexcept {
    residency_state& state = get_residency_state();
    const auto matches = [db_oid, user_relid, tablespace_oid,
            main_relfilenode, column_attno,
            requested_index_name_hash](const auto& key) noexcept {
        return key.db_oid == db_oid
            && key.user_relid == user_relid
            && key.tablespace_oid == tablespace_oid
            && key.main_relfilenode == main_relfilenode
            && key.column_attno == column_attno
            && key.index_name_hash == requested_index_name_hash;
    };
    for (std::size_t entry_index = 0;
         entry_index < state.chunk_entries.size();) {
        if (matches(state.chunk_entries[entry_index].key)) {
            evict_chunk_index(entry_index);
        } else {
            ++entry_index;
        }
    }
    for (std::size_t entry_index = 0;
         entry_index < state.assembled_entries.size();) {
        if (matches(state.assembled_entries[entry_index].key)) {
            evict_assembled_index(entry_index);
        } else {
            ++entry_index;
        }
    }
}

void evict_chunk_subxact(SubTransactionId subxid) noexcept {
    residency_state& state = get_residency_state();
    for (std::size_t entry_index = 0; entry_index < state.chunk_entries.size();) {
        const chunk_cache_entry& entry = state.chunk_entries[entry_index];
        if (entry.written_in_xact && entry.subxid == subxid) {
            evict_chunk_index(entry_index);
        } else {
            ++entry_index;
        }
    }
}

void finish_top_xact(bool committed) noexcept {
    residency_state& state = get_residency_state();
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
    for (chunk_cache_entry& entry : state.chunk_entries) {
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
    residency_detail::registered_callbacks& state = residency_detail::callback_state();
    if (state.installed) {
        return;
    }
    if (!state.xact_registered) {
        bioseq::postgres::support::invoke_pg_as_cpp([] {
#ifdef BIOSEQ_TEST_INDEXES
            inject_index_residency_callback_failure(
                index_residency_callback_failure::register_xact);
#endif
            RegisterXactCallback(
                bioseq_index_residency_xact_callback, nullptr);
        });
        state.xact_registered = true;
    }
    if (!state.subxact_registered) {
        bioseq::postgres::support::invoke_pg_as_cpp([] {
#ifdef BIOSEQ_TEST_INDEXES
            inject_index_residency_callback_failure(
                index_residency_callback_failure::register_subxact);
#endif
            RegisterSubXactCallback(
                bioseq_index_residency_subxact_callback, nullptr);
        });
        state.subxact_registered = true;
    }
    if (!state.relcache_registered) {
        bioseq::postgres::support::invoke_pg_as_cpp([] {
#ifdef BIOSEQ_TEST_INDEXES
            inject_index_residency_callback_failure(
                index_residency_callback_failure::register_relcache);
#endif
            CacheRegisterRelcacheCallback(
                bioseq_index_residency_relcache_callback,
                PointerGetDatum(nullptr));
        });
        state.relcache_registered = true;
    }
    state.installed = state.xact_registered
        && state.subxact_registered
        && state.relcache_registered;
    if (!state.installed) {
        // Defensive only: every leaf above either completed or threw.
        return;
    }
    bioseq_log<logging_level::debug>("[bioseq] index residency: transaction and relcache callbacks registered");
}

[[nodiscard]] std::optional<cached_chunk_view> lookup_cached_chunk(const index_chunk_key& key) {
    install_index_residency_callbacks();
    residency_state& state = get_residency_state();
    for (chunk_cache_entry& entry : state.chunk_entries) {
        if (entry.key == key) {
            entry.last_used = ++state.clock;
            return cached_chunk_view{entry.bytes.data(), entry.bytes.size()};
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<cached_chunk_view> store_cached_chunk(
        const index_chunk_key& key,
        std::span<const char> payload,
        bool written_in_current_xact) {
    install_index_residency_callbacks();
    if (payload.empty() || payload.size() > k_chunk_cache_maximum_bytes) {
        return std::nullopt;
    }
    auto permit =
        bioseq::parallelism::memory_admission_controller::instance()
            .try_reserve(payload.size());
    if (!permit) {
        return std::nullopt;
    }

    residency_state& state = get_residency_state();
    for (std::size_t entry_index = 0; entry_index < state.chunk_entries.size(); ++entry_index) {
        if (state.chunk_entries[entry_index].key == key) {
            evict_chunk_index(entry_index);
            break;
        }
    }
    while (!state.chunk_entries.empty()
           && (state.chunk_entries.size() >= k_chunk_cache_maximum_entries
               || state.chunk_bytes + payload.size() > k_chunk_cache_maximum_bytes)) {
        const auto victim = std::min_element(
            state.chunk_entries.begin(), state.chunk_entries.end(),
            [](const chunk_cache_entry& lhs, const chunk_cache_entry& rhs) {
                return lhs.last_used < rhs.last_used;
            });
        evict_chunk_index(static_cast<std::size_t>(
            std::distance(state.chunk_entries.begin(), victim)));
    }
    if (state.chunk_bytes + payload.size() > k_chunk_cache_maximum_bytes) {
        return std::nullopt;
    }

    chunk_cache_entry entry{};
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
    const chunk_cache_entry& stored = state.chunk_entries.back();
    return cached_chunk_view{stored.bytes.data(), stored.bytes.size()};
}

[[nodiscard]] std::optional<assembled_index_cache_hit> lookup_assembled_index(
        const assembled_index_key& key) {
    install_index_residency_callbacks();
    residency_state& state = get_residency_state();
    for (assembled_index_cache_entry& entry : state.assembled_entries) {
        if (entry.key == key) {
            entry.last_used = ++state.clock;
            return assembled_index_cache_hit{entry.index};
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::uint64_t estimate_assembled_index_bytes(
        const bioseq::indexing::minimizer_hashmap_fragment_index& index) noexcept {
    const std::uint64_t serialized =
        static_cast<std::uint64_t>(index.serialize_binary_size());
    const std::uint64_t table_bytes =
        bioseq::common::saturating_mul(
            static_cast<std::uint64_t>(index.table_size()),
            32u);
    const std::uint64_t position_capacity_bytes =
        bioseq::common::saturating_mul(
            static_cast<std::uint64_t>(index.positions.capacity()),
            static_cast<std::uint64_t>(sizeof(std::uint64_t)));
    return serialized + table_bytes + position_capacity_bytes;
}

void store_assembled_index(
        const assembled_index_key& key,
        bioseq::indexing::minimizer_hashmap_fragment_index&& index) {
    install_index_residency_callbacks();
    const std::uint64_t bytes = estimate_assembled_index_bytes(index);
    if (bytes == 0 || bytes > k_assembled_cache_maximum_bytes) {
        return;
    }
    auto permit =
        bioseq::parallelism::memory_admission_controller::instance()
            .try_reserve(bytes);
    if (!permit) {
        return;
    }

    residency_state& state = get_residency_state();
    for (std::size_t entry_index = 0; entry_index < state.assembled_entries.size(); ++entry_index) {
        if (state.assembled_entries[entry_index].key == key) {
            evict_assembled_index(entry_index);
            break;
        }
    }
    while (!state.assembled_entries.empty()
           && (state.assembled_entries.size() >= k_assembled_cache_maximum_entries
               || state.assembled_bytes + bytes > k_assembled_cache_maximum_bytes)) {
        const auto victim = std::min_element(
            state.assembled_entries.begin(), state.assembled_entries.end(),
            [](const assembled_index_cache_entry& lhs, const assembled_index_cache_entry& rhs) {
                return lhs.last_used < rhs.last_used;
            });
        evict_assembled_index(static_cast<std::size_t>(
            std::distance(state.assembled_entries.begin(), victim)));
    }
    if (state.assembled_bytes + bytes > k_assembled_cache_maximum_bytes) {
        return;
    }

    assembled_index_cache_entry entry{};
    entry.key = key;
    entry.index = std::make_shared<bioseq::indexing::minimizer_hashmap_fragment_index>(
        std::move(index));
    entry.permit = std::move(*permit);
    entry.admitted_bytes = bytes;
    entry.last_used = ++state.clock;
    state.assembled_bytes += bytes;
    state.assembled_entries.push_back(std::move(entry));
}

[[nodiscard]] auto
query_minimizer_signature(std::span<const bioseq::indexing::minimap_sketch_data> minimizers,
                          std::uint32_t kmer_size,
                          std::uint32_t window_size,
                          std::uint32_t max_occ) noexcept -> std::uint64_t {
    std::uint64_t hash = 1469598103934665603ull;
    hash = residency_detail::fnv_mix(hash, kmer_size);
    hash = residency_detail::fnv_mix(hash, window_size);
    hash = residency_detail::fnv_mix(hash, max_occ);
    for (const bioseq::indexing::minimap_sketch_data& minimizer : minimizers) {
        hash = residency_detail::fnv_mix(hash, minimizer.kmer());
    }
    return hash;
}

[[nodiscard]] auto cached_index_residency_bytes() noexcept -> std::uint64_t {
    const residency_state& state = get_residency_state();
    return state.chunk_bytes + state.assembled_bytes;
}

} // namespace bioseq::storage::indexing
