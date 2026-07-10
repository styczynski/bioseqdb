/**
 * @file minimizer_vulkan_index.cppm
 * @brief DOC0307 Phase B — GPU-resident view over `MinimizerHashmapFragmentIndex`
 *        and the batch/wave-scoped upload cache.
 *
 * Reuses the existing serialized bucket ABI (`SerializedBucket`) as the GPU slot
 * record; derives an open-addressed slot table + the positions array at upload
 * time. Keeps `minimizer_hashmap_index.cppm` storage-free — all Vulkan/cache
 * code lives here (DOC0307 §4.2, §4.4). The cache is owned by the true outer
 * query-batch/segment wave; selected Vulkan seed execution treats upload
 * failures and 32-bit ABI overflow as hard `VulkanRuntimeError`s.
 */
module;

#include "common.h"

#include <bit>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <expected>
#include <exception>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

export module bioseq.indexing.minimizer_vulkan;

import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.storage;
import bioseq.indexing.type;
import bioseq.gpu.vulkan.runtime;
import bioseq.parallelism.memory_admission;
import bioseq.common.radix_sort;

export namespace bioseq::indexing {

using GpuMinimizerSlot = MinimizerHashmapFragmentIndex::SerializedBucket;
static_assert(sizeof(GpuMinimizerSlot) == 16);
static_assert(offsetof(GpuMinimizerSlot, hash) == 0);
static_assert(offsetof(GpuMinimizerSlot, pos_offset) == 8);
static_assert(offsetof(GpuMinimizerSlot, pos_count) == 12);

// Legacy raw-target host upload object. The persisted path below cannot accept
// it and publishes only fully owned streamed residents through insert_resident.
struct GpuMinimizerIndexUpload {
    std::vector<GpuMinimizerSlot> slots;
    std::vector<std::uint64_t> positions;
    std::uint64_t target_len = 0;
    std::uint64_t table_size = 0;
    std::uint32_t slot_mask = 0;
};

struct GpuIndexTooLarge final : std::exception {
    [[nodiscard]] const char* what() const noexcept override {
        return "GPU minimizer index exceeds 32-bit device slot limits";
    }
};

// Legacy raw-target/recompute cache key. It is intentionally not accepted by
// any DOC0371 persisted API below; the later atomic production-routing change
// will migrate/delete this pointer-shaped path together with its pipe callers.
struct GpuMinimizerIndexCacheKey {
    std::uintptr_t source_id = 0;
    std::uint64_t caller_identity = 0;
    std::uint64_t target_len = 0;
    std::uint64_t table_size = 0;
    std::uint64_t position_count = 0;

    [[nodiscard]] bool operator==(const GpuMinimizerIndexCacheKey&) const = default;
};

[[nodiscard]] inline std::uint64_t splitmix64(std::uint64_t x) noexcept {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

struct GpuMinimizerIndexCacheKeyHash {
    [[nodiscard]] std::size_t operator()(const GpuMinimizerIndexCacheKey& k) const noexcept {
        std::uint64_t h = splitmix64(static_cast<std::uint64_t>(k.source_id));
        h ^= splitmix64(k.caller_identity + (h << 1));
        h ^= splitmix64(k.target_len + 0x9e3779b97f4a7c15ULL);
        h ^= splitmix64(k.table_size + (h << 1));
        h ^= splitmix64(k.position_count + (h << 1));
        return static_cast<std::size_t>(h);
    }
};

[[nodiscard]] inline GpuMinimizerIndexCacheKey make_gpu_index_cache_key_for_in_memory(
    const MinimizerHashmapFragmentIndex& idx) noexcept
{
    return GpuMinimizerIndexCacheKey{
        .source_id = reinterpret_cast<std::uintptr_t>(&idx),
        .caller_identity = 0u,
        .target_len = static_cast<std::uint64_t>(idx.target_len),
        .table_size = static_cast<std::uint64_t>(idx.table_size()),
        .position_count = static_cast<std::uint64_t>(idx.positions.size()),
    };
}

[[nodiscard]] inline GpuMinimizerIndexCacheKey make_gpu_index_cache_key_for_assembly(
    const MinimizerHashmapFragmentIndex& idx,
    std::uint64_t assembly_identity) noexcept
{
    DEBUG_ASSERT(assembly_identity != 0, "persistent GPU assembly key must be nonzero");
    return GpuMinimizerIndexCacheKey{
        .source_id = 0u,
        .caller_identity = assembly_identity,
        .target_len = static_cast<std::uint64_t>(idx.target_len),
        .table_size = static_cast<std::uint64_t>(idx.table_size()),
        .position_count = static_cast<std::uint64_t>(idx.positions.size()),
    };
}

struct GpuIndexCacheIdentity {
    // Zero means a stable, non-mutated in-memory index in the current wave.
    // Nonzero means persisted assembly or segmented recompute identity.
    std::uint64_t assembly_identity = 0;
    // Optional explicit shape for paths that preinsert a GPU-resident index
    // without materialising the CPU table/positions vectors.
    std::uint64_t table_size = 0;
    std::uint64_t position_count = 0;
};

[[nodiscard]] inline GpuMinimizerIndexCacheKey make_gpu_index_cache_key(
    const MinimizerHashmapFragmentIndex& idx,
    GpuIndexCacheIdentity identity) noexcept
{
    if (identity.assembly_identity == 0) {
        return make_gpu_index_cache_key_for_in_memory(idx);
    }
    if (identity.table_size != 0 || identity.position_count != 0) {
        return GpuMinimizerIndexCacheKey{
            .source_id = 0u,
            .caller_identity = identity.assembly_identity,
            .target_len = static_cast<std::uint64_t>(idx.target_len),
            .table_size = identity.table_size,
            .position_count = identity.position_count,
        };
    }
    return make_gpu_index_cache_key_for_assembly(idx, identity.assembly_identity);
}

struct GpuResidentMinimizerIndex {
    // Declared first so it is destroyed last, after both resident buffers.
    bioseq::parallelism::MemoryPermit permit;
    bioseq::gpu::vulkan::Buffer slots;
    bioseq::gpu::vulkan::Buffer positions;
    GpuMinimizerIndexCacheKey key{};
    std::uint64_t target_len = 0;
    std::uint32_t slot_mask = 0;
    std::uint32_t position_count = 0;
    // DOC0310 §12 — when nonzero, `slots` is a hash-sorted dense bucket array
    // (GPU-built) that the seed lookup binary-searches; `slot_mask` is then 0.
    // When zero, `slots` is the open-addressed table (CPU upload) and `slot_mask`
    // is the power-of-two mask.
    std::uint32_t bucket_count = 0;
    std::uint64_t target_position_offset = 0;
};
static_assert(std::is_standard_layout_v<GpuResidentMinimizerIndex>);
static_assert(offsetof(GpuResidentMinimizerIndex, permit)
    < offsetof(GpuResidentMinimizerIndex, slots));
static_assert(offsetof(GpuResidentMinimizerIndex, permit)
    < offsetof(GpuResidentMinimizerIndex, positions));

[[nodiscard]] inline std::uint32_t gpu_slot_capacity(std::uint64_t distinct) {
    using bioseq::parallelism::saturating_mul;
    const std::uint64_t wanted = bioseq::gpu::vulkan::sat_add(
        saturating_mul(distinct, std::uint64_t{2}), 1u);
    if (wanted > (std::uint64_t{1} << 31)) {
        throw GpuIndexTooLarge{};
    }
    return static_cast<std::uint32_t>(std::bit_ceil(std::max<std::uint64_t>(2, wanted)));
}

/// DOC0310 §12 — wrap a runtime GPU-built index (`GpuBuiltIndex`, owns its
/// slots/positions device buffers) into a `GpuResidentMinimizerIndex`, taking a
/// resident memory permit. Lives here (indexing layer) so `runtime.cppm` never
/// imports `bioseq::indexing`.
[[nodiscard]] inline std::shared_ptr<GpuResidentMinimizerIndex> adopt_gpu_built_index(
    bioseq::gpu::vulkan::Runtime& runtime,
    bioseq::gpu::vulkan::GpuBuiltIndex&& built,
    GpuMinimizerIndexCacheKey key)
{
    (void)runtime;
    if (!built.valid) return nullptr;
    auto resident = std::make_shared<GpuResidentMinimizerIndex>();
    const std::uint64_t bytes =
        bioseq::gpu::vulkan::sat_add(built.slots.bytes, built.positions.bytes);
    if (auto permit = bioseq::parallelism::MemoryAdmissionController::instance().try_reserve(bytes)) {
        resident->permit = std::move(*permit);
    }
    resident->slots = std::move(built.slots);
    resident->positions = std::move(built.positions);
    resident->key = key;
    resident->target_len = built.target_len;
    resident->slot_mask = built.slot_mask;          // 0 for the sorted-bucket form
    resident->position_count = built.position_count;
    resident->bucket_count = built.occupied_slots;  // dense bucket count for binary search
    return resident;
}

[[nodiscard]] inline GpuMinimizerIndexUpload
build_gpu_minimizer_upload(const MinimizerHashmapFragmentIndex& idx) {
    if (idx.positions.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw GpuIndexTooLarge{};
    }
    const std::uint32_t capacity = gpu_slot_capacity(idx.table_size());
    GpuMinimizerIndexUpload upload;
    upload.slots.resize(capacity);
    upload.positions = idx.positions;
    upload.target_len = idx.target_len;
    upload.table_size = idx.table_size();
    upload.slot_mask = capacity - 1u;

    idx.for_each_table_entry([&](std::uint64_t hash, std::uint64_t packed) {
        const std::uint64_t off64 = packed >> 32;
        const std::uint64_t cnt64 = packed & 0xffffffffULL;
        if (off64 > std::numeric_limits<std::uint32_t>::max() ||
            cnt64 > std::numeric_limits<std::uint32_t>::max() ||
            off64 + cnt64 > idx.positions.size()) {
            throw GpuIndexTooLarge{};
        }
        const std::uint32_t off = static_cast<std::uint32_t>(off64);
        const std::uint32_t cnt = static_cast<std::uint32_t>(cnt64);
        std::uint32_t p = static_cast<std::uint32_t>(splitmix64(hash)) & upload.slot_mask;
        for (;;) {
            GpuMinimizerSlot& slot = upload.slots[p];
            if (slot.pos_count == 0) {
                slot.hash = hash;
                slot.pos_offset = off;
                slot.pos_count = cnt;
                break;
            }
            p = (p + 1u) & upload.slot_mask;
        }
    });
    return upload;
}

[[nodiscard]] inline GpuMinimizerIndexUpload
build_gpu_minimizer_upload_from_pairs(
    std::span<std::pair<std::uint64_t, std::uint64_t>> pairs,
    sequence_size_t target_length)
{
    const std::size_t n = pairs.size();
    if (n > std::numeric_limits<std::uint32_t>::max()) {
        throw GpuIndexTooLarge{};
    }
    bioseq::sorting::radix_sort(
        pairs.data(), pairs.data() + n,
        [](const auto& p) noexcept { return p.first; });

    std::uint64_t distinct = 0;
    std::size_t i = 0;
    while (i < n) {
        const std::uint64_t kmer = pairs[i].first;
        do { ++i; } while (i < n && pairs[i].first == kmer);
        ++distinct;
    }

    const std::uint32_t capacity = gpu_slot_capacity(distinct);
    GpuMinimizerIndexUpload upload;
    upload.slots.resize(capacity);
    upload.positions.resize(n);
    upload.target_len = target_length;
    upload.table_size = distinct;
    upload.slot_mask = capacity - 1u;

    i = 0;
    while (i < n) {
        const std::uint64_t hash = pairs[i].first;
        const std::size_t begin = i;
        do {
            upload.positions[i] = pairs[i].second;
            ++i;
        } while (i < n && pairs[i].first == hash);
        const std::size_t count = i - begin;
        if (begin > std::numeric_limits<std::uint32_t>::max()
            || count > std::numeric_limits<std::uint32_t>::max()) {
            throw GpuIndexTooLarge{};
        }
        std::uint32_t p =
            static_cast<std::uint32_t>(splitmix64(hash)) & upload.slot_mask;
        for (;;) {
            GpuMinimizerSlot& slot = upload.slots[p];
            if (slot.pos_count == 0) {
                slot.hash = hash;
                slot.pos_offset = static_cast<std::uint32_t>(begin);
                slot.pos_count = static_cast<std::uint32_t>(count);
                break;
            }
            p = (p + 1u) & upload.slot_mask;
        }
    }
    return upload;
}

/// Resident device footprint (held by `GpuResidentMinimizerIndex::permit`).
[[nodiscard]] inline std::uint64_t gpu_index_resident_bytes(
    const MinimizerHashmapFragmentIndex& idx)
{
    using bioseq::parallelism::saturating_mul;
    const auto slot_bytes = saturating_mul(
        gpu_slot_capacity(idx.table_size()), sizeof(GpuMinimizerSlot));
    const auto pos_bytes = saturating_mul(idx.positions.size(), sizeof(std::uint64_t));
    return bioseq::gpu::vulkan::sat_add(slot_bytes, pos_bytes);
}

/// Transient upload peak (host vectors + one staging buffer); released after
/// the device copy completes.
[[nodiscard]] inline std::uint64_t gpu_index_upload_peak_bytes(
    const MinimizerHashmapFragmentIndex& idx)
{
    using bioseq::parallelism::saturating_mul;
    const auto slot_bytes = saturating_mul(
        gpu_slot_capacity(idx.table_size()), sizeof(GpuMinimizerSlot));
    const auto pos_bytes = saturating_mul(idx.positions.size(), sizeof(std::uint64_t));
    const auto staging_bytes = std::max(slot_bytes, pos_bytes);
    return bioseq::gpu::vulkan::sat_add(
        bioseq::gpu::vulkan::sat_add(slot_bytes, pos_bytes), staging_bytes);
}

/// Batch/wave-scoped GPU minimizer index cache. Owned by the outer query
/// batch/segment wave; uploads each distinct index once, reuses the resident
/// handle across queries that share it, and evicts LRU when the resident byte
/// budget is exceeded.
class GpuMinimizerIndexCache {
public:
    explicit GpuMinimizerIndexCache(std::uint64_t resident_limit_bytes)
        : resident_limit_bytes_(resident_limit_bytes) {}

    [[nodiscard]] std::shared_ptr<const GpuResidentMinimizerIndex> get_or_upload(
        bioseq::gpu::vulkan::Runtime& runtime,
        const MinimizerHashmapFragmentIndex& idx,
        const GpuMinimizerIndexCacheKey& key,
        bool cache_enabled)
    {
        std::lock_guard<std::mutex> guard(mu_);
        if (cache_enabled) {
            auto found = entries_.find(key);
            if (found != entries_.end()) {
                touch_lru_locked(key);
                return found->second;
            }
        }

        std::shared_ptr<const GpuResidentMinimizerIndex> handle;
        try {
            handle = upload_locked_index(runtime, idx, key);
        } catch (const GpuIndexTooLarge&) {
            throw bioseq::gpu::vulkan::VulkanRuntimeError(
                "Vulkan minimizer index exceeds the 32-bit device ABI");
        } catch (const bioseq::gpu::vulkan::VulkanRuntimeError&) {
            throw;
        }
        if (!handle) return nullptr;

        if (cache_enabled) {
            insert_with_eviction_locked(key, handle);
        }
        return handle;
    }

    /// DOC0310 §12 — insert a GPU-built resident index (adopted from a device
    /// index build) under `key` so the seed stage finds it instead of uploading
    /// the CPU `MinimizerHashmapFragmentIndex`.
    void insert_built(
        const GpuMinimizerIndexCacheKey& key,
        std::shared_ptr<const GpuResidentMinimizerIndex> handle)
    {
        if (!handle) return;
        std::lock_guard<std::mutex> guard(mu_);
        insert_with_eviction_locked(key, handle);
    }

    [[nodiscard]] std::shared_ptr<const GpuResidentMinimizerIndex> insert_upload(
        bioseq::gpu::vulkan::Runtime& runtime,
        GpuMinimizerIndexUpload&& upload,
        const GpuMinimizerIndexCacheKey& key,
        bool cache_enabled)
    {
        std::lock_guard<std::mutex> guard(mu_);
        if (cache_enabled) {
            auto found = entries_.find(key);
            if (found != entries_.end()) {
                touch_lru_locked(key);
                return found->second;
            }
        }

        auto handle = upload_prepared_locked(runtime, upload, key);
        if (!handle) return nullptr;
        if (cache_enabled) {
            insert_with_eviction_locked(key, handle);
        }
        return handle;
    }

    void erase(const GpuMinimizerIndexCacheKey& key) noexcept {
        std::lock_guard<std::mutex> guard(mu_);
        auto found = entries_.find(key);
        if (found == entries_.end()) return;
        resident_bytes_ -= handle_resident_bytes(*found->second);
        entries_.erase(found);
        auto pos = std::find(lru_.begin(), lru_.end(), key);
        if (pos != lru_.end()) lru_.erase(pos);
    }

    void clear() noexcept {
        std::lock_guard<std::mutex> guard(mu_);
        entries_.clear();
        lru_.clear();
        resident_bytes_ = 0;
    }

private:
    [[nodiscard]] static std::uint64_t handle_resident_bytes(
        const GpuResidentMinimizerIndex& handle) noexcept
    {
        return handle.slots.bytes + handle.positions.bytes;
    }

    void touch_lru_locked(const GpuMinimizerIndexCacheKey& key) {
        auto pos = std::find(lru_.begin(), lru_.end(), key);
        if (pos != lru_.end()) {
            lru_.splice(lru_.begin(), lru_, pos);
        }
    }

    void insert_with_eviction_locked(
        const GpuMinimizerIndexCacheKey& key,
        const std::shared_ptr<const GpuResidentMinimizerIndex>& handle)
    {
        const std::uint64_t handle_bytes = handle_resident_bytes(*handle);
        // A handle larger than the whole budget is used uncached (already
        // returned to the caller); do not pollute the cache with it.
        if (resident_limit_bytes_ != 0 && handle_bytes > resident_limit_bytes_) {
            return;
        }
        while (!lru_.empty() && resident_limit_bytes_ != 0
               && resident_bytes_ + handle_bytes > resident_limit_bytes_) {
            const GpuMinimizerIndexCacheKey victim = lru_.back();
            lru_.pop_back();
            auto found = entries_.find(victim);
            if (found != entries_.end()) {
                resident_bytes_ -= handle_resident_bytes(*found->second);
                entries_.erase(found);
            }
        }
        entries_[key] = handle;
        lru_.push_front(key);
        resident_bytes_ += handle_bytes;
    }

    [[nodiscard]] std::shared_ptr<const GpuResidentMinimizerIndex> upload_locked_index(
        bioseq::gpu::vulkan::Runtime& runtime,
        const MinimizerHashmapFragmentIndex& idx,
        const GpuMinimizerIndexCacheKey& key)
    {
        using bioseq::parallelism::MemoryAdmissionController;
        const std::uint64_t resident_bytes = gpu_index_resident_bytes(idx);
        const std::uint64_t upload_peak = gpu_index_upload_peak_bytes(idx);

        auto resident_permit =
            MemoryAdmissionController::instance().try_reserve(resident_bytes);
        if (!resident_permit) return nullptr;
        auto upload_permit =
            MemoryAdmissionController::instance().try_reserve(upload_peak);
        if (!upload_permit) return nullptr;

        const GpuMinimizerIndexUpload upload = build_gpu_minimizer_upload(idx);
        const std::uint64_t slot_bytes =
            static_cast<std::uint64_t>(upload.slots.size()) * sizeof(GpuMinimizerSlot);
        const std::uint64_t pos_bytes =
            static_cast<std::uint64_t>(upload.positions.size()) * sizeof(std::uint64_t);

        constexpr VkBufferUsageFlags usage =
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        auto resident = std::make_shared<GpuResidentMinimizerIndex>();
        resident->slots = runtime.create_device_buffer(std::max<std::uint64_t>(slot_bytes, 16u), usage);
        resident->positions = runtime.create_device_buffer(std::max<std::uint64_t>(pos_bytes, 8u), usage);
        if (resident->slots.mapped == nullptr || resident->positions.mapped == nullptr) {
            // Non-unified memory; Phase B uploads via the direct mapped path.
            return nullptr;
        }
        if (slot_bytes > 0) {
            std::memcpy(resident->slots.mapped, upload.slots.data(),
                        static_cast<std::size_t>(slot_bytes));
        }
        if (pos_bytes > 0) {
            std::memcpy(resident->positions.mapped, upload.positions.data(),
                        static_cast<std::size_t>(pos_bytes));
        }
        resident->permit = std::move(*resident_permit);
        resident->key = key;
        resident->target_len = upload.target_len;
        resident->slot_mask = upload.slot_mask;
        resident->position_count = static_cast<std::uint32_t>(upload.positions.size());
        // upload_permit released here (host vectors copied to device).
        return resident;
    }

    [[nodiscard]] std::shared_ptr<const GpuResidentMinimizerIndex> upload_prepared_locked(
        bioseq::gpu::vulkan::Runtime& runtime,
        const GpuMinimizerIndexUpload& upload,
        const GpuMinimizerIndexCacheKey& key)
    {
        using bioseq::parallelism::MemoryAdmissionController;
        const std::uint64_t slot_bytes =
            static_cast<std::uint64_t>(upload.slots.size()) * sizeof(GpuMinimizerSlot);
        const std::uint64_t pos_bytes =
            static_cast<std::uint64_t>(upload.positions.size()) * sizeof(std::uint64_t);
        const std::uint64_t resident_bytes =
            bioseq::gpu::vulkan::sat_add(slot_bytes, pos_bytes);
        auto resident_permit =
            MemoryAdmissionController::instance().try_reserve(resident_bytes);
        if (!resident_permit) return nullptr;

        constexpr VkBufferUsageFlags usage =
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        auto resident = std::make_shared<GpuResidentMinimizerIndex>();
        resident->slots = runtime.create_device_buffer(std::max<std::uint64_t>(slot_bytes, 16u), usage);
        resident->positions = runtime.create_device_buffer(std::max<std::uint64_t>(pos_bytes, 8u), usage);
        if (resident->slots.mapped == nullptr || resident->positions.mapped == nullptr) {
            return nullptr;
        }
        if (slot_bytes > 0) {
            std::memcpy(resident->slots.mapped, upload.slots.data(),
                        static_cast<std::size_t>(slot_bytes));
        }
        if (pos_bytes > 0) {
            std::memcpy(resident->positions.mapped, upload.positions.data(),
                        static_cast<std::size_t>(pos_bytes));
        }
        resident->permit = std::move(*resident_permit);
        resident->key = key;
        resident->target_len = upload.target_len;
        resident->slot_mask = upload.slot_mask;
        resident->position_count = static_cast<std::uint32_t>(upload.positions.size());
        resident->bucket_count = 0u;
        return resident;
    }

    std::uint64_t resident_bytes_ = 0;
    std::uint64_t resident_limit_bytes_ = 0;
    std::mutex mu_;
    std::list<GpuMinimizerIndexCacheKey> lru_;
    std::unordered_map<GpuMinimizerIndexCacheKey,
        std::shared_ptr<const GpuResidentMinimizerIndex>,
        GpuMinimizerIndexCacheKeyHash> entries_;
};

// ===========================================================================
// DOC0371 persisted sorted-dense GPU foundation (not production-routed yet)
// ===========================================================================

using GpuDeviceUuid = std::array<std::uint8_t, VK_UUID_SIZE>;
static_assert(VK_UUID_SIZE == 16u);

/// Exact immutable identity of one persisted sorted-dense resident partition.
/// Query contents, host pointers, and decoded-index shape are deliberately not
/// identity axes.
struct GpuPersistedMinimizerIndexCacheKey {
    std::uint64_t generation_id = 0;
    std::uint64_t source_identity = 0;
    std::int32_t position_segment = 0;
    index_sub_key_t sub_key = 0;
    GpuDeviceUuid device_uuid{};
    std::uint32_t sorted_dense_abi_version = 0;

    [[nodiscard]] bool operator==(
        const GpuPersistedMinimizerIndexCacheKey&) const = default;
};

[[nodiscard]] inline bool gpu_device_uuid_is_bound(
    const GpuDeviceUuid& uuid) noexcept
{
    return std::any_of(uuid.begin(), uuid.end(),
        [](std::uint8_t byte) noexcept { return byte != 0u; });
}

[[nodiscard]] inline bool valid_gpu_persisted_cache_key(
    const GpuPersistedMinimizerIndexCacheKey& key) noexcept
{
    return key.generation_id != 0u
        && key.position_segment >= 0
        && key.sub_key >= 0 && key.sub_key < TEMP_RUN_BASE
        && gpu_device_uuid_is_bound(key.device_uuid)
        && key.sorted_dense_abi_version == kMinimizerSortedDenseGpuAbiVersion;
}

struct GpuPersistedMinimizerIndexCacheKeyHash {
    [[nodiscard]] std::size_t operator()(
        const GpuPersistedMinimizerIndexCacheKey& key) const noexcept
    {
        std::uint64_t hash = splitmix64(key.generation_id);
        hash ^= splitmix64(key.source_identity + (hash << 1u));
        hash ^= splitmix64(
            std::bit_cast<std::uint32_t>(key.position_segment) + (hash << 1u));
        hash ^= splitmix64(
            static_cast<std::uint64_t>(key.sub_key) + (hash << 1u));
        for (std::size_t offset = 0; offset < key.device_uuid.size(); offset += 8u) {
            std::uint64_t word = 0;
            for (std::size_t byte = 0; byte < 8u; ++byte) {
                word |= static_cast<std::uint64_t>(
                    key.device_uuid[offset + byte]) << (byte * 8u);
            }
            hash ^= splitmix64(word + (hash << 1u));
        }
        hash ^= splitmix64(key.sorted_dense_abi_version + (hash << 1u));
        return static_cast<std::size_t>(hash);
    }
};

enum class GpuPersistedIndexFoundationError : std::uint8_t {
    invalid_identity,
    device_identity_mismatch,
    unsupported_endianness,
    directory_corrupt,
    metadata_mismatch,
    arithmetic_overflow,
    admission,
    non_mappable_device_memory,
    partition_mismatch,
    byte_count,
    checksum,
    duplicate_publication,
    resident_limit,
    malformed_ordinal_result,
    selection_identity_mismatch,
    invalid_resident_set,
};

/// Algorithm-local positive and forbidden-work proof. The canonical pipeline
/// owner maps this only after the later production-routing slice.
struct GpuPersistedIndexLocalProof {
    std::uint64_t authoritative_directory_uploads = 0;
    std::uint64_t authoritative_directory_bytes = 0;
    std::uint64_t device_partition_selection_dispatches = 0;
    std::uint64_t device_query_minimizers_processed = 0;
    std::uint64_t opaque_ordinals_read_back = 0;
    std::uint64_t streamed_resident_partitions = 0;
    std::uint64_t streamed_control_bytes = 0;
    std::uint64_t streamed_position_bytes = 0;
    std::uint64_t resident_insertions = 0;
    std::uint64_t resident_sets_prepared = 0;

    std::uint64_t forbidden_host_query_hash_materializations = 0;
    std::uint64_t forbidden_host_pair_materializations = 0;
    std::uint64_t forbidden_host_pair_sorts = 0;
    std::uint64_t forbidden_host_control_rebases = 0;
    std::uint64_t forbidden_host_anchor_concatenations = 0;
    std::uint64_t forbidden_cpu_or_simd_fallbacks = 0;
    bool saturated = false;

    [[nodiscard]] bool has_forbidden_work() const noexcept {
        return forbidden_host_query_hash_materializations != 0u
            || forbidden_host_pair_materializations != 0u
            || forbidden_host_pair_sorts != 0u
            || forbidden_host_control_rebases != 0u
            || forbidden_host_anchor_concatenations != 0u
            || forbidden_cpu_or_simd_fallbacks != 0u;
    }
};

inline void add_gpu_persisted_proof_counter(
    GpuPersistedIndexLocalProof& proof,
    std::uint64_t& counter,
    std::uint64_t increment) noexcept
{
    const std::uint64_t combined =
        bioseq::parallelism::saturating_add(counter, increment);
    if (combined == (std::numeric_limits<std::uint64_t>::max)()
        && increment > (std::numeric_limits<std::uint64_t>::max)() - counter) {
        proof.saturated = true;
    }
    counter = combined;
}

class GpuSortedDensePartitionStream;

/// Cache-owned, immutable, header-free sorted-dense partition. Only the
/// validated stream can construct one; the permit is destroyed after buffers.
class GpuResidentSortedDensePartition {
private:
    struct ConstructionToken {};

public:
    GpuResidentSortedDensePartition(
        ConstructionToken,
        bioseq::parallelism::MemoryPermit permit,
        bioseq::gpu::vulkan::Buffer controls,
        bioseq::gpu::vulkan::Buffer positions,
        GpuPersistedMinimizerIndexCacheKey key,
        std::uint32_t partition_id,
        std::uint32_t control_count,
        std::uint32_t position_count,
        std::uint64_t target_length,
        std::uint64_t core_begin_bp,
        std::uint64_t core_end_bp) noexcept
        : permit_(std::move(permit)),
          controls_(std::move(controls)),
          positions_(std::move(positions)),
          key_(key),
          partition_id_(partition_id),
          control_count_(control_count),
          position_count_(position_count),
          target_length_(target_length),
          core_begin_bp_(core_begin_bp),
          core_end_bp_(core_end_bp) {}

    [[nodiscard]] const bioseq::gpu::vulkan::Buffer& controls() const noexcept {
        return controls_;
    }
    [[nodiscard]] const bioseq::gpu::vulkan::Buffer& positions() const noexcept {
        return positions_;
    }
    [[nodiscard]] const GpuPersistedMinimizerIndexCacheKey& key() const noexcept {
        return key_;
    }
    [[nodiscard]] std::uint32_t partition_id() const noexcept {
        return partition_id_;
    }
    [[nodiscard]] std::uint32_t control_count() const noexcept {
        return control_count_;
    }
    [[nodiscard]] std::uint32_t position_count() const noexcept {
        return position_count_;
    }
    [[nodiscard]] std::uint64_t target_length() const noexcept {
        return target_length_;
    }
    [[nodiscard]] std::uint64_t core_begin_bp() const noexcept {
        return core_begin_bp_;
    }
    [[nodiscard]] std::uint64_t core_end_bp() const noexcept {
        return core_end_bp_;
    }
    [[nodiscard]] std::uint64_t admitted_bytes() const noexcept {
        return permit_.bytes();
    }

    [[nodiscard]] std::uint64_t logical_bytes() const noexcept {
        return static_cast<std::uint64_t>(control_count_)
                * sizeof(GpuMinimizerSlot)
            + static_cast<std::uint64_t>(position_count_)
                * sizeof(std::uint64_t);
    }

private:
    bioseq::parallelism::MemoryPermit permit_;
    bioseq::gpu::vulkan::Buffer controls_;
    bioseq::gpu::vulkan::Buffer positions_;
    GpuPersistedMinimizerIndexCacheKey key_{};
    std::uint32_t partition_id_ = 0;
    std::uint32_t control_count_ = 0;
    std::uint32_t position_count_ = 0;
    std::uint64_t target_length_ = 0;
    std::uint64_t core_begin_bp_ = 0;
    std::uint64_t core_end_bp_ = 0;

    friend class GpuSortedDensePartitionStream;
};

/// Exact identity bound into an opaque device-selection result. Directory
/// checksum/bytes prevent same-generation metadata substitution.
struct GpuPersistedDirectoryIdentity {
    std::uint64_t generation_id = 0;
    std::uint64_t source_identity = 0;
    std::uint64_t source_content_checksum = 0;
    std::uint64_t compatibility_checksum = 0;
    std::uint64_t directory_checksum = 0;
    std::uint64_t directory_bytes = 0;
    std::uint32_t entry_count = 0;
    std::uint32_t position_segment_count = 0;
    GpuDeviceUuid device_uuid{};
    std::uint32_t sorted_dense_abi_version = 0;

    [[nodiscard]] bool operator==(
        const GpuPersistedDirectoryIdentity&) const = default;
};

class GpuResidentAuthoritativeDirectory;

[[nodiscard]] std::expected<
    std::shared_ptr<const GpuResidentAuthoritativeDirectory>,
    GpuPersistedIndexFoundationError>
upload_authoritative_gpu_directory(
    bioseq::gpu::vulkan::Runtime& runtime,
    const MinimizerGenerationHead& head,
    const MinimizerGenerationManifest& manifest,
    const DirView& directory,
    GpuPersistedIndexLocalProof& proof);

/// Cache-owned authoritative encoded directory used only by the device
/// selector. Construction is token-gated through the validating upload above.
class GpuResidentAuthoritativeDirectory {
private:
    struct ConstructionToken {};

public:
    GpuResidentAuthoritativeDirectory(
        ConstructionToken,
        bioseq::parallelism::MemoryPermit permit,
        bioseq::gpu::vulkan::Buffer encoded_directory,
        GpuPersistedDirectoryIdentity identity) noexcept
        : permit_(std::move(permit)),
          encoded_directory_(std::move(encoded_directory)),
          identity_(identity) {}

    [[nodiscard]] const bioseq::gpu::vulkan::Buffer&
    encoded_directory() const noexcept { return encoded_directory_; }

    [[nodiscard]] const GpuPersistedDirectoryIdentity&
    identity() const noexcept { return identity_; }

private:
    // First declaration ensures reverse destruction releases the buffer first.
    bioseq::parallelism::MemoryPermit permit_;
    bioseq::gpu::vulkan::Buffer encoded_directory_;
    GpuPersistedDirectoryIdentity identity_{};

    friend std::expected<
        std::shared_ptr<const GpuResidentAuthoritativeDirectory>,
        GpuPersistedIndexFoundationError>
    upload_authoritative_gpu_directory(
        bioseq::gpu::vulkan::Runtime&,
        const MinimizerGenerationHead&,
        const MinimizerGenerationManifest&,
        const DirView&,
        GpuPersistedIndexLocalProof&);
};

/// Upload the exact validated DirView bytes. The Vulkan shader ABI is pinned to
/// the same 96-byte entry layout; there is no host-side re-encoding step.
[[nodiscard]] inline std::expected<
    std::shared_ptr<const GpuResidentAuthoritativeDirectory>,
    GpuPersistedIndexFoundationError>
upload_authoritative_gpu_directory(
    bioseq::gpu::vulkan::Runtime& runtime,
    const MinimizerGenerationHead& head,
    const MinimizerGenerationManifest& manifest,
    const DirView& directory,
    GpuPersistedIndexLocalProof& proof)
{
    static_assert(MinimizerBucketDirEntry::encoded_bytes
        == bioseq::gpu::vulkan::k_gpu_persisted_directory_entry_bytes);
    if constexpr (std::endian::native != std::endian::little) {
        return std::unexpected(
            GpuPersistedIndexFoundationError::unsupported_endianness);
    }
    if (!runtime.usable()
        || !gpu_device_uuid_is_bound(runtime.features().device_uuid)) {
        return std::unexpected(
            GpuPersistedIndexFoundationError::invalid_identity);
    }
    if (auto valid = validate_minimizer_generation_metadata(
            head, manifest, directory); !valid) {
        return std::unexpected(
            GpuPersistedIndexFoundationError::metadata_mismatch);
    }
    if (directory.entries().size()
        > (std::numeric_limits<std::uint32_t>::max)()) {
        return std::unexpected(
            GpuPersistedIndexFoundationError::arithmetic_overflow);
    }
    const std::uint64_t encoded_bytes = directory.encoded_bytes().size();
    const std::uint64_t allocated_bytes =
        std::max<std::uint64_t>(encoded_bytes, sizeof(std::uint32_t));
    auto permit = bioseq::parallelism::MemoryAdmissionController::instance()
        .try_reserve(allocated_bytes);
    if (!permit) {
        return std::unexpected(GpuPersistedIndexFoundationError::admission);
    }

    constexpr VkBufferUsageFlags usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bioseq::gpu::vulkan::Buffer encoded_directory =
        runtime.create_device_buffer(allocated_bytes, usage);
    if (encoded_directory.mapped == nullptr) {
        return std::unexpected(
            GpuPersistedIndexFoundationError::non_mappable_device_memory);
    }
    if (encoded_bytes != 0u) {
        std::memcpy(
            encoded_directory.mapped,
            directory.encoded_bytes().data(),
            static_cast<std::size_t>(encoded_bytes));
    }
    const GpuPersistedDirectoryIdentity identity{
        .generation_id = manifest.generation_id,
        .source_identity = manifest.source_identity,
        .source_content_checksum = manifest.source_content_checksum,
        .compatibility_checksum = head.compatibility_checksum,
        .directory_checksum = manifest.directory_checksum,
        .directory_bytes = encoded_bytes,
        .entry_count = static_cast<std::uint32_t>(directory.entries().size()),
        .position_segment_count = manifest.position_segment_count,
        .device_uuid = runtime.features().device_uuid,
        .sorted_dense_abi_version = manifest.gpu_abi_version,
    };
    auto resident = std::make_shared<GpuResidentAuthoritativeDirectory>(
        GpuResidentAuthoritativeDirectory::ConstructionToken{},
        std::move(*permit), std::move(encoded_directory), identity);
    add_gpu_persisted_proof_counter(
        proof, proof.authoritative_directory_uploads, 1u);
    add_gpu_persisted_proof_counter(
        proof, proof.authoritative_directory_bytes, encoded_bytes);
    std::shared_ptr<const GpuResidentAuthoritativeDirectory> immutable = resident;
    return immutable;
}

struct GpuDeviceQueryMinimizerView {
    const bioseq::gpu::vulkan::Buffer* device_words = nullptr;
    std::uint32_t count = 0;
    std::uint32_t word_kind = 0;
    std::uint32_t words32_per_record = 0;
    std::uint32_t hash_shift = 0;
};

/// Host-visible result whose values are opaque stable directory ordinals, not
/// query hashes. Callers may only use them to qualify persisted extent reads.
class GpuOpaqueDirectoryOrdinals {
public:
    GpuOpaqueDirectoryOrdinals() = default;

    [[nodiscard]] std::expected<
        std::span<const std::uint32_t>,
        GpuPersistedIndexFoundationError>
    values_for(
        const GpuResidentAuthoritativeDirectory& directory) const noexcept {
        if (identity_ != directory.identity()) {
            return std::unexpected(
                GpuPersistedIndexFoundationError::selection_identity_mismatch);
        }
        return std::span<const std::uint32_t>{ordinals_};
    }

    [[nodiscard]] bool contains_for(
        const GpuResidentAuthoritativeDirectory& directory,
        std::uint32_t ordinal) const noexcept {
        const auto values = values_for(directory);
        return values && std::binary_search(
            values->begin(), values->end(), ordinal);
    }

private:
    GpuOpaqueDirectoryOrdinals(
        std::vector<std::uint32_t> ordinals,
        GpuPersistedDirectoryIdentity identity) noexcept
        : ordinals_(std::move(ordinals)), identity_(identity) {}

    std::vector<std::uint32_t> ordinals_;
    GpuPersistedDirectoryIdentity identity_{};

    friend GpuOpaqueDirectoryOrdinals
    select_gpu_persisted_directory_ordinals(
        bioseq::gpu::vulkan::Runtime&,
        const GpuDeviceQueryMinimizerView&,
        const GpuResidentAuthoritativeDirectory&,
        GpuPersistedIndexLocalProof&);
};

/// Run the real device union/compaction selector. This signature has no host
/// minimizer/hash input and therefore cannot materialize or filter query hashes.
[[nodiscard]] inline GpuOpaqueDirectoryOrdinals
select_gpu_persisted_directory_ordinals(
    bioseq::gpu::vulkan::Runtime& runtime,
    const GpuDeviceQueryMinimizerView& query,
    const GpuResidentAuthoritativeDirectory& directory,
    GpuPersistedIndexLocalProof& proof)
{
    const GpuPersistedDirectoryIdentity& identity = directory.identity();
    if (identity.device_uuid != runtime.features().device_uuid
        || identity.generation_id == 0u
        || identity.sorted_dense_abi_version
            != kMinimizerSortedDenseGpuAbiVersion
        || identity.directory_bytes
            != static_cast<std::uint64_t>(identity.entry_count)
                * MinimizerBucketDirEntry::encoded_bytes) {
        throw bioseq::gpu::vulkan::VulkanRuntimeError(
            "persisted GPU directory identity/ABI mismatch");
    }
    auto selected = runtime.run_persisted_directory_ordinal_selection(
        bioseq::gpu::vulkan::GpuPersistedDirectorySelectionInputs{
            .device_minimizer_words = query.device_words,
            .encoded_directory = &directory.encoded_directory(),
            .minimizer_count = query.count,
            .minimizer_word_kind = query.word_kind,
            .minimizer_words32_per_record = query.words32_per_record,
            .hash_shift = query.hash_shift,
            .directory_entry_count = identity.entry_count,
        });
    const bool dispatched = query.count != 0u && identity.entry_count != 0u;
    const std::uint64_t expected_processed = dispatched ? query.count : 0u;
    if (selected.device_query_minimizers_processed != expected_processed
        || (!dispatched && !selected.ordinals.empty())) {
        throw bioseq::gpu::vulkan::VulkanRuntimeError(
            "persisted GPU directory selector returned fabricated zero-work proof");
    }
    if (dispatched) {
        add_gpu_persisted_proof_counter(
            proof, proof.device_partition_selection_dispatches, 1u);
    }
    add_gpu_persisted_proof_counter(
        proof, proof.device_query_minimizers_processed,
        selected.device_query_minimizers_processed);
    add_gpu_persisted_proof_counter(
        proof, proof.opaque_ordinals_read_back, selected.ordinals.size());
    return GpuOpaqueDirectoryOrdinals{
        std::move(selected.ordinals), identity};
}

/// Exact persisted-partition cache. `insert_resident` is its only publication
/// API; legacy raw-target `insert_built` remains isolated in the older cache
/// until the later atomic production-routing migration.
class GpuPersistedMinimizerIndexCache {
public:
    explicit GpuPersistedMinimizerIndexCache(std::uint64_t resident_limit_bytes)
        : resident_limit_bytes_(resident_limit_bytes) {}

private:
    [[nodiscard]] std::expected<
        std::shared_ptr<const GpuResidentSortedDensePartition>,
        GpuPersistedIndexFoundationError>
    insert_resident(
        std::shared_ptr<GpuResidentSortedDensePartition> resident,
        GpuPersistedIndexLocalProof& proof)
    {
        if (!resident
            || !valid_gpu_persisted_cache_key(resident->key())
            || !resident->controls().valid() || !resident->positions().valid()
            || resident->admitted_bytes() < resident->logical_bytes()) {
            return std::unexpected(
                GpuPersistedIndexFoundationError::invalid_identity);
        }
        const std::uint64_t bytes = resident->logical_bytes();
        std::lock_guard<std::mutex> guard(mutex_);
        if (entries_.contains(resident->key())) {
            return std::unexpected(
                GpuPersistedIndexFoundationError::duplicate_publication);
        }
        if (resident_limit_bytes_ != 0u
            && (bytes > resident_limit_bytes_
                || resident_bytes_ > resident_limit_bytes_ - bytes)) {
            return std::unexpected(
                GpuPersistedIndexFoundationError::resident_limit);
        }
        const auto key = resident->key();
        std::shared_ptr<const GpuResidentSortedDensePartition> published =
            std::move(resident);
        entries_.emplace(key, published);
        resident_bytes_ += bytes;
        add_gpu_persisted_proof_counter(
            proof, proof.resident_insertions, 1u);
        return published;
    }

public:
    [[nodiscard]] std::shared_ptr<const GpuResidentSortedDensePartition> find(
        const GpuPersistedMinimizerIndexCacheKey& key) const
    {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = entries_.find(key);
        return found == entries_.end() ? nullptr : found->second;
    }

    void clear() noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        entries_.clear();
        resident_bytes_ = 0u;
    }

private:
    std::uint64_t resident_limit_bytes_ = 0;
    std::uint64_t resident_bytes_ = 0;
    mutable std::mutex mutex_;
    std::unordered_map<
        GpuPersistedMinimizerIndexCacheKey,
        std::shared_ptr<const GpuResidentSortedDensePartition>,
        GpuPersistedMinimizerIndexCacheKeyHash> entries_;

    friend class GpuSortedDensePartitionStream;
};

/// Stateful direct-to-final-buffer receiver for one selected partition. It
/// accepts the already-qualified, header-free control extent followed by the
/// position extent; it never builds pairs, sorts, or rebases controls.
class GpuSortedDensePartitionStream {
public:
    GpuSortedDensePartitionStream(GpuSortedDensePartitionStream&&) noexcept = default;
    GpuSortedDensePartitionStream& operator=(
        GpuSortedDensePartitionStream&&) = delete;
    GpuSortedDensePartitionStream(const GpuSortedDensePartitionStream&) = delete;
    GpuSortedDensePartitionStream& operator=(
        const GpuSortedDensePartitionStream&) = delete;

    [[nodiscard]] static std::expected<
        GpuSortedDensePartitionStream,
        GpuPersistedIndexFoundationError>
    begin(
        bioseq::gpu::vulkan::Runtime& runtime,
        const GpuPersistedMinimizerIndexCacheKey& key,
        const GpuOpaqueDirectoryOrdinals& selection,
        const GpuResidentAuthoritativeDirectory& resident_directory,
        const DirView& authoritative_directory,
        const MinimizerGenerationManifest& manifest,
        std::uint32_t ordinal)
    {
        if constexpr (std::endian::native != std::endian::little) {
            return std::unexpected(
                GpuPersistedIndexFoundationError::unsupported_endianness);
        }
        const GpuPersistedDirectoryIdentity& directory_identity =
            resident_directory.identity();
        const auto selected = selection.values_for(resident_directory);
        const MinimizerBucketDirEntry* entry =
            authoritative_directory.entry_by_ordinal(ordinal);
        const bool directory_matches =
            directory_identity.generation_id == manifest.generation_id
            && directory_identity.source_identity == manifest.source_identity
            && directory_identity.source_content_checksum
                == manifest.source_content_checksum
            && directory_identity.compatibility_checksum
                == minimizer_generation_compatibility_checksum(manifest)
            && directory_identity.directory_checksum
                == manifest.directory_checksum
            && directory_identity.directory_bytes == manifest.directory_bytes
            && directory_identity.directory_bytes
                == authoritative_directory.encoded_bytes().size()
            && directory_identity.directory_checksum
                == crc64_ecma182(authoritative_directory.encoded_bytes())
            && directory_identity.entry_count
                == authoritative_directory.entries().size()
            && directory_identity.position_segment_count
                == manifest.position_segment_count
            && directory_identity.sorted_dense_abi_version
                == manifest.gpu_abi_version
            && directory_identity.device_uuid == runtime.features().device_uuid;
        if (!selected
            || !std::binary_search(selected->begin(), selected->end(), ordinal)
            || entry == nullptr || !directory_matches) {
            return std::unexpected(
                GpuPersistedIndexFoundationError::selection_identity_mismatch);
        }
        const auto entry_error = validate_minimizer_bucket_dir_entry(*entry);
        if (!valid_gpu_persisted_cache_key(key)
            || key.device_uuid != runtime.features().device_uuid
            || key.generation_id != manifest.generation_id
            || key.source_identity != manifest.source_identity
            || manifest.gpu_abi_version
                != kMinimizerSortedDenseGpuAbiVersion
            || entry->position_segment != key.position_segment
            || entry->sub_key != key.sub_key
            || entry_error.has_value()
            || entry->control_element_count
                > (std::numeric_limits<std::uint32_t>::max)()
            || entry->position_element_count
                > (std::numeric_limits<std::uint32_t>::max)()) {
            return std::unexpected(
                GpuPersistedIndexFoundationError::partition_mismatch);
        }
        const auto core = minimizer_position_segment_core(
            manifest, static_cast<std::uint32_t>(key.position_segment));
        if (!core) {
            return std::unexpected(
                GpuPersistedIndexFoundationError::partition_mismatch);
        }
        const std::uint64_t logical_bytes =
            bioseq::parallelism::saturating_add(
                entry->control_encoded_bytes, entry->position_encoded_bytes);
        if (logical_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(
                GpuPersistedIndexFoundationError::arithmetic_overflow);
        }
        auto permit = bioseq::parallelism::MemoryAdmissionController::instance()
            .try_reserve(logical_bytes);
        if (!permit) {
            return std::unexpected(GpuPersistedIndexFoundationError::admission);
        }
        constexpr VkBufferUsageFlags usage =
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bioseq::gpu::vulkan::Buffer controls = runtime.create_device_buffer(
            std::max<std::uint64_t>(entry->control_encoded_bytes, 16u), usage);
        bioseq::gpu::vulkan::Buffer positions = runtime.create_device_buffer(
            std::max<std::uint64_t>(entry->position_encoded_bytes, 8u), usage);
        if (controls.mapped == nullptr || positions.mapped == nullptr) {
            return std::unexpected(
                GpuPersistedIndexFoundationError::non_mappable_device_memory);
        }
        auto resident = std::make_shared<GpuResidentSortedDensePartition>(
            GpuResidentSortedDensePartition::ConstructionToken{},
            std::move(*permit), std::move(controls), std::move(positions), key,
            entry->partition_id,
            static_cast<std::uint32_t>(entry->control_element_count),
            static_cast<std::uint32_t>(entry->position_element_count),
            manifest.target_length, core->begin_bp, core->end_bp);
        return GpuSortedDensePartitionStream{
            std::move(resident), entry->control_encoded_bytes,
            entry->position_encoded_bytes, entry->control_checksum,
            entry->position_checksum};
    }

    [[nodiscard]] std::expected<void, GpuPersistedIndexFoundationError>
    accept_control_bytes(std::span<const std::byte> bytes) noexcept {
        if (!resident_ || position_bytes_accepted_ != 0u
            || bytes.size() > control_bytes_expected_ - control_bytes_accepted_) {
            return std::unexpected(GpuPersistedIndexFoundationError::byte_count);
        }
        if (!bytes.empty()) {
            std::memcpy(
                static_cast<std::byte*>(resident_->controls_.mapped)
                    + control_bytes_accepted_,
                bytes.data(), bytes.size());
            control_crc_.update(bytes);
            control_bytes_accepted_ += bytes.size();
        }
        return {};
    }

    [[nodiscard]] std::expected<void, GpuPersistedIndexFoundationError>
    accept_position_bytes(std::span<const std::byte> bytes) noexcept {
        if (!resident_ || control_bytes_accepted_ != control_bytes_expected_
            || bytes.size() > position_bytes_expected_ - position_bytes_accepted_) {
            return std::unexpected(GpuPersistedIndexFoundationError::byte_count);
        }
        if (!bytes.empty()) {
            std::memcpy(
                static_cast<std::byte*>(resident_->positions_.mapped)
                    + position_bytes_accepted_,
                bytes.data(), bytes.size());
            position_crc_.update(bytes);
            position_bytes_accepted_ += bytes.size();
        }
        return {};
    }

    [[nodiscard]] std::expected<
        std::shared_ptr<const GpuResidentSortedDensePartition>,
        GpuPersistedIndexFoundationError>
    finish(
        GpuPersistedMinimizerIndexCache& cache,
        GpuPersistedIndexLocalProof& proof)
    {
        if (!resident_
            || control_bytes_accepted_ != control_bytes_expected_
            || position_bytes_accepted_ != position_bytes_expected_) {
            return std::unexpected(GpuPersistedIndexFoundationError::byte_count);
        }
        if (control_crc_.checksum() != control_checksum_expected_
            || position_crc_.checksum() != position_checksum_expected_) {
            return std::unexpected(GpuPersistedIndexFoundationError::checksum);
        }
        const std::uint64_t controls = control_bytes_accepted_;
        const std::uint64_t positions = position_bytes_accepted_;
        auto published = cache.insert_resident(std::move(resident_), proof);
        if (!published) {
            return std::unexpected(published.error());
        }
        add_gpu_persisted_proof_counter(
            proof, proof.streamed_resident_partitions, 1u);
        add_gpu_persisted_proof_counter(
            proof, proof.streamed_control_bytes, controls);
        add_gpu_persisted_proof_counter(
            proof, proof.streamed_position_bytes, positions);
        return *published;
    }

private:
    GpuSortedDensePartitionStream(
        std::shared_ptr<GpuResidentSortedDensePartition> resident,
        std::uint64_t control_bytes_expected,
        std::uint64_t position_bytes_expected,
        std::uint64_t control_checksum_expected,
        std::uint64_t position_checksum_expected) noexcept
        : resident_(std::move(resident)),
          control_bytes_expected_(control_bytes_expected),
          position_bytes_expected_(position_bytes_expected),
          control_checksum_expected_(control_checksum_expected),
          position_checksum_expected_(position_checksum_expected) {}

    std::shared_ptr<GpuResidentSortedDensePartition> resident_;
    std::uint64_t control_bytes_expected_ = 0;
    std::uint64_t position_bytes_expected_ = 0;
    std::uint64_t control_checksum_expected_ = 0;
    std::uint64_t position_checksum_expected_ = 0;
    std::uint64_t control_bytes_accepted_ = 0;
    std::uint64_t position_bytes_accepted_ = 0;
    Crc64Ecma182 control_crc_{};
    Crc64Ecma182 position_crc_{};
};

struct GpuResidentPartitionDeviceView {
    const bioseq::gpu::vulkan::Buffer* controls = nullptr;
    const bioseq::gpu::vulkan::Buffer* positions = nullptr;
    std::uint32_t control_count = 0;
    std::uint32_t position_count = 0;
    std::uint32_t partition_id = 0;
};

/// Immutable, same-generation/segment/device set consumed by the future
/// multi-partition seed count/emit dispatch.
class GpuResidentSortedDensePartitionSet {
public:
    [[nodiscard]] static std::expected<
        GpuResidentSortedDensePartitionSet,
        GpuPersistedIndexFoundationError>
    prepare(
        std::span<const std::shared_ptr<
            const GpuResidentSortedDensePartition>> partitions,
        GpuPersistedIndexLocalProof& proof)
    {
        if (partitions.empty()) {
            return std::unexpected(
                GpuPersistedIndexFoundationError::invalid_resident_set);
        }
        std::vector<std::shared_ptr<const GpuResidentSortedDensePartition>> owned(
            partitions.begin(), partitions.end());
        std::sort(owned.begin(), owned.end(),
            [](const auto& lhs, const auto& rhs) noexcept {
                return lhs && rhs
                    ? lhs->partition_id() < rhs->partition_id()
                    : static_cast<bool>(lhs);
            });
        const auto& first = owned.front();
        if (!first) {
            return std::unexpected(
                GpuPersistedIndexFoundationError::invalid_resident_set);
        }
        std::vector<GpuResidentPartitionDeviceView> views;
        views.reserve(owned.size());
        std::optional<std::uint32_t> previous_partition;
        for (const auto& partition : owned) {
            if (!partition
                || !valid_gpu_persisted_cache_key(partition->key())
                || !partition->controls().valid() || !partition->positions().valid()
                || partition->admitted_bytes() < partition->logical_bytes()
                || partition->key().generation_id != first->key().generation_id
                || partition->key().source_identity != first->key().source_identity
                || partition->key().position_segment
                    != first->key().position_segment
                || partition->key().device_uuid != first->key().device_uuid
                || partition->key().sorted_dense_abi_version
                    != first->key().sorted_dense_abi_version
                || partition->target_length() != first->target_length()
                || partition->core_begin_bp() != first->core_begin_bp()
                || partition->core_end_bp() != first->core_end_bp()
                || (previous_partition
                    && partition->partition_id() <= *previous_partition)) {
                return std::unexpected(
                    GpuPersistedIndexFoundationError::invalid_resident_set);
            }
            views.push_back(GpuResidentPartitionDeviceView{
                .controls = &partition->controls(),
                .positions = &partition->positions(),
                .control_count = partition->control_count(),
                .position_count = partition->position_count(),
                .partition_id = partition->partition_id(),
            });
            previous_partition = partition->partition_id();
        }
        add_gpu_persisted_proof_counter(
            proof, proof.resident_sets_prepared, 1u);
        return GpuResidentSortedDensePartitionSet{
            std::move(owned), std::move(views)};
    }

    [[nodiscard]] std::span<const GpuResidentPartitionDeviceView>
    device_views() const noexcept { return views_; }

    [[nodiscard]] const GpuPersistedMinimizerIndexCacheKey&
    identity() const noexcept { return partitions_.front()->key(); }

private:
    GpuResidentSortedDensePartitionSet(
        std::vector<std::shared_ptr<const GpuResidentSortedDensePartition>> partitions,
        std::vector<GpuResidentPartitionDeviceView> views) noexcept
        : partitions_(std::move(partitions)), views_(std::move(views)) {}

    std::vector<std::shared_ptr<const GpuResidentSortedDensePartition>> partitions_;
    std::vector<GpuResidentPartitionDeviceView> views_;
};

} // namespace bioseq::indexing
