/**
 * @file minimizer_vulkan_index.cppm
 * @brief DOC0307 Phase B — GPU-resident view over `MinimizerHashmapFragmentIndex`
 *        and the batch/wave-scoped upload cache.
 *
 * Reuses the existing serialized bucket ABI (`SerializedBucket`) as the GPU slot
 * record; derives an open-addressed slot table + the positions array at upload
 * time. Keeps `minimizer_hashmap_index.cppm` storage-free — all Vulkan/cache
 * code lives here (DOC0307 §4.2, §4.4). The cache is owned by the true outer
 * query-batch/segment wave; a valid index that exceeds the 32-bit device ABI is
 * not corruption — it logs at debug and returns nullptr for the CPU seed path.
 */
module;

#include "common.h"

#include <bit>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

export module bioseq_indexing.minimizer_vulkan;

import mlogging;
import bioseq_indexing.minimizer_hashmap;
import bioseq_gpu.vulkan.runtime;
import bioseq_parallelism.memory_admission;

export namespace bioseq::indexing {

using GpuMinimizerSlot = MinimizerHashmapFragmentIndex::SerializedBucket;
static_assert(sizeof(GpuMinimizerSlot) == 16);
static_assert(offsetof(GpuMinimizerSlot, hash) == 0);
static_assert(offsetof(GpuMinimizerSlot, pos_offset) == 8);
static_assert(offsetof(GpuMinimizerSlot, pos_count) == 12);

struct GpuMinimizerIndexUpload {
    std::vector<GpuMinimizerSlot> slots;
    std::vector<std::uint64_t> positions;
    std::uint64_t target_len = 0;
    std::uint32_t slot_mask = 0;
};

struct GpuIndexTooLarge final : std::exception {
    [[nodiscard]] const char* what() const noexcept override {
        return "GPU minimizer index exceeds 32-bit device slot limits";
    }
};

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
        .table_size = static_cast<std::uint64_t>(idx.table.size()),
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
        .table_size = static_cast<std::uint64_t>(idx.table.size()),
        .position_count = static_cast<std::uint64_t>(idx.positions.size()),
    };
}

struct GpuIndexCacheIdentity {
    // Zero means a stable, non-mutated in-memory index in the current wave.
    // Nonzero means persisted assembly or segmented recompute identity.
    std::uint64_t assembly_identity = 0;
};

[[nodiscard]] inline GpuMinimizerIndexCacheKey make_gpu_index_cache_key(
    const MinimizerHashmapFragmentIndex& idx,
    GpuIndexCacheIdentity identity) noexcept
{
    return identity.assembly_identity == 0
        ? make_gpu_index_cache_key_for_in_memory(idx)
        : make_gpu_index_cache_key_for_assembly(idx, identity.assembly_identity);
}

struct GpuResidentMinimizerIndex {
    bioseq::gpu::vulkan::Buffer slots;
    bioseq::gpu::vulkan::Buffer positions;
    bioseq::parallelism::MemoryPermit permit;
    GpuMinimizerIndexCacheKey key{};
    std::uint64_t target_len = 0;
    std::uint32_t slot_mask = 0;
    std::uint32_t position_count = 0;
    // DOC0310 §12 — when nonzero, `slots` is a hash-sorted dense bucket array
    // (GPU-built) that the seed lookup binary-searches; `slot_mask` is then 0.
    // When zero, `slots` is the open-addressed table (CPU upload) and `slot_mask`
    // is the power-of-two mask.
    std::uint32_t bucket_count = 0;
};

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
    const std::uint32_t capacity = gpu_slot_capacity(idx.table.size());
    GpuMinimizerIndexUpload upload;
    upload.slots.resize(capacity);
    upload.positions = idx.positions;
    upload.target_len = idx.target_len;
    upload.slot_mask = capacity - 1u;

    for (const auto& kv : idx.table) {
        const std::uint64_t hash = kv.first;
        const std::uint64_t off64 = kv.second >> 32;
        const std::uint64_t cnt64 = kv.second & 0xffffffffULL;
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
    }
    return upload;
}

/// Resident device footprint (held by `GpuResidentMinimizerIndex::permit`).
[[nodiscard]] inline std::uint64_t gpu_index_resident_bytes(
    const MinimizerHashmapFragmentIndex& idx)
{
    using bioseq::parallelism::saturating_mul;
    const auto slot_bytes = saturating_mul(
        gpu_slot_capacity(idx.table.size()), sizeof(GpuMinimizerSlot));
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
        gpu_slot_capacity(idx.table.size()), sizeof(GpuMinimizerSlot));
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
            bioseq_log<DBG>("[vulkan index] index exceeds 32-bit device ABI; CPU seed path");
            return nullptr;
        } catch (const bioseq::gpu::vulkan::VulkanRuntimeError& e) {
            bioseq_log<DBG>("[vulkan index] upload failed; CPU seed path: {}", e.what());
            return nullptr;
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

    std::uint64_t resident_bytes_ = 0;
    std::uint64_t resident_limit_bytes_ = 0;
    std::mutex mu_;
    std::list<GpuMinimizerIndexCacheKey> lru_;
    std::unordered_map<GpuMinimizerIndexCacheKey,
        std::shared_ptr<const GpuResidentMinimizerIndex>,
        GpuMinimizerIndexCacheKeyHash> entries_;
};

} // namespace bioseq::indexing
